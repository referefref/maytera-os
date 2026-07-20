// signal.c - POSIX-ish signal delivery (Phase D2)
//
// Phase D1 established the return-work hook. Phase D2 turns the hook into
// real signal delivery: it picks a deliverable signal, consults the
// per-process handler table, and either takes a default action (terminate)
// or builds a sigframe on the user stack and redirects the saved IRET
// frame to the handler. The matching sys_rt_sigreturn() restores the saved
// registers exactly as they were at the point the signal was taken.

#include "signal.h"
#include "process.h"
#include "syscall.h"
#include "../serial.h"
#include "../sync/waitq.h"   // #426: sys_pause() parks instead of yield-spinning

extern process_t *proc_current(void);
extern void proc_wake(process_t *p);
extern process_t *proc_get(uint32_t pid);
extern void proc_exit(int exit_code);

// Stack layout at the hook point (pointer passed to return_work_handler).
// Matches the pushes in syscall.asm between push-args and the final sysret.
// NOTE: The first 15*8 bytes are the GPRs; [15*8]..[19*8] are the saved
// IRET frame (rip, cs, rflags, user_rsp, ss). Order is the pop-order, so
// index 0 is r15, 14 is rax, 15 is rip, etc.
typedef struct saved_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, user_rsp, ss;
} saved_frame_t;

// ============================================================================
// Public: queue / query
// ============================================================================

void sig_raise(process_t *target, int signo) {
    if (!target) return;
    if (signo <= 0 || signo > NSIG) return;
    target->sig_pending |= (1ULL << (signo - 1));
    target->return_work |= RETURN_WORK_SIGPENDING;
    // Phase D3: if the target is blocked on a wait queue, wake_up_process
    // unlinks it from the queue and sets its wake_reason to WAIT_EINTR so
    // the interrupted syscall returns -EINTR. For simpler SLEEPING state
    // (timer sleep without a wq), fall back to a plain wake.
    if (target->wait_entry) {
        extern void wake_up_process(process_t *p);
        wake_up_process(target);
    } else if (target->state == PROC_STATE_BLOCKED ||
               target->state == PROC_STATE_SLEEPING) {
        proc_wake(target);
    }
}

// Phase D4: raise a signal to every process in a process group. Used by the
// TTY line discipline (future C1) to deliver SIGINT/SIGQUIT/SIGTSTP on
// Ctrl-C/Ctrl-\/Ctrl-Z to the foreground pgrp, and by shell job control.
void sig_raise_pgrp(uint32_t pgrp, int signo) {
    if (pgrp == 0) return;
    for (uint32_t pid = 1; pid < MAX_PROCESSES; pid++) {
        process_t *p = proc_get(pid);
        if (p && p->pgrp == pgrp && p->state != PROC_STATE_UNUSED &&
            p->state != PROC_STATE_ZOMBIE) {
            sig_raise(p, signo);
        }
    }
}

uint64_t sig_deliverable(process_t *target) {
    if (!target) return 0;
    uint64_t unblocked = target->sig_pending & ~target->sig_mask;
    uint64_t unmaskable = target->sig_pending & ((1ULL << (SIGKILL - 1)) |
                                                  (1ULL << (SIGSTOP - 1)));
    return unblocked | unmaskable;
}

// Default POSIX action per signal: 0 = terminate, 1 = ignore, 2 = stop
// (we stub STOP as ignore for MVP).
static int default_action(int signo) {
    switch (signo) {
        case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH:
            return 1; // ignore
        case SIGTSTP: case SIGTTIN: case SIGTTOU: case SIGSTOP:
            return 1; // STOP semantics stubbed (no job control yet)
        default:
            return 0; // terminate
    }
}

// ============================================================================
// Return-work hook
// ============================================================================

// Build a sigframe on the user stack and redirect the saved IRET frame
// so that on SYSRET the handler runs. The handler returns via the
// trampoline (which userland installs at signal-install time and we stash
// on the frame as the return address the handler pops).
static void deliver_signal(saved_frame_t *sf, process_t *p, int signo) {
    void *handler = p->sig_handlers[signo - 1];

    // Default / ignore path. Clear the pending bit and return without
    // touching the user frame; the process continues with its normal
    // syscall return value.
    if (handler == SIG_DFL) {
        int act = default_action(signo);
        if (act == 1) {
            // ignore
            p->sig_pending &= ~(1ULL << (signo - 1));
            return;
        }
        // terminate: conventional POSIX exit code 128 + signo.
        p->sig_pending &= ~(1ULL << (signo - 1));
        proc_exit(128 + signo);
        // proc_exit never returns; we should not get here.
        return;
    }
    if (handler == SIG_IGN) {
        p->sig_pending &= ~(1ULL << (signo - 1));
        return;
    }

    // Real handler: build the frame on the user stack. We reserve 128
    // bytes below the saved user_rsp as a red zone (we don't use the
    // AMD64 red zone, but be paranoid) and then push the sigframe.
    uint64_t user_rsp = sf->user_rsp;
    user_rsp -= 128;                         // red-zone pad
    user_rsp -= sizeof(sigframe_t);
    user_rsp &= ~0xFULL;                     // 16-byte align

    sigframe_t *frame = (sigframe_t *)user_rsp;

    // IMPORTANT: we are writing to user memory while still in the user
    // process's cr3 (syscalls do not swap cr3). If user_rsp is bogus the
    // write will page-fault here; Phase D4's paranoia can add validation.
    frame->saved_rax    = sf->rax;
    frame->saved_rbx    = sf->rbx;
    frame->saved_rcx    = sf->rcx;
    frame->saved_rdx    = sf->rdx;
    frame->saved_rsi    = sf->rsi;
    frame->saved_rdi    = sf->rdi;
    frame->saved_rbp    = sf->rbp;
    frame->saved_r8     = sf->r8;
    frame->saved_r9     = sf->r9;
    frame->saved_r10    = sf->r10;
    frame->saved_r11    = sf->r11;
    frame->saved_r12    = sf->r12;
    frame->saved_r13    = sf->r13;
    frame->saved_r14    = sf->r14;
    frame->saved_r15    = sf->r15;
    frame->saved_rip    = sf->rip;
    frame->saved_rflags = sf->rflags;
    frame->saved_rsp    = sf->user_rsp;
    frame->saved_mask   = p->sig_mask;
    frame->signo        = (uint32_t)signo;
    frame->__pad        = 0;

    // Update sig_mask: block this signal (unless SA_NODEFER) and block
    // the signals in sa_mask while the handler runs.
    uint32_t flags = (uint32_t)p->sig_flags[signo - 1];
    if (!(flags & SA_NODEFER)) {
        p->sig_mask |= (1ULL << (signo - 1));
    }
    p->sig_mask |= p->sig_handler_mask[signo - 1];

    // Clear the pending bit for this signal.
    p->sig_pending &= ~(1ULL << (signo - 1));

    // If SA_RESETHAND, reset to SIG_DFL after one delivery.
    if (flags & SA_RESETHAND) {
        p->sig_handlers[signo - 1] = SIG_DFL;
        p->sig_flags[signo - 1] = 0;
        p->sig_handler_mask[signo - 1] = 0;
    }

    // Redirect the saved IRET frame:
    //   RIP  = handler address
    //   RSP  = user_rsp (points at the sigframe; handler sees it via rdi+8
    //          if it used SA_SIGINFO; for simple signals it just runs with
    //          the signo in RDI).
    //   RDI  = signo (first arg to handler)
    // When handler does `ret`, it pops the first qword at RSP. We put the
    // trampoline address there so userland bounces into rt_sigreturn.
    //
    // The trampoline address was stashed in __reserved when sigaction was
    // installed. For D2 we also accept a well-known fallback address of 0,
    // in which case we assume the libc registered a trampoline and panic
    // if it didn't (the process will crash, which is the right failure
    // mode in development).
    extern uint64_t g_sig_trampoline;  // set by sys_sigaction on first install
    uint64_t trampoline = g_sig_trampoline;
    if (trampoline == 0) {
        // No trampoline registered; fall back to terminating the process
        // rather than jumping to zero.
        kprintf("[SIG] No trampoline; terminating pid=%u on signal %d\n",
                p->pid, signo);
        proc_exit(128 + signo);
        return;
    }

    // Push trampoline as the "return address" the handler will ret to.
    user_rsp -= 8;
    *(uint64_t *)user_rsp = trampoline;

    sf->user_rsp = user_rsp;
    sf->rip      = (uint64_t)handler;
    sf->rdi      = (uint64_t)signo;
    // Clear RFLAGS direction and interrupt-disabled bits? SYSRET sets
    // IF from r11; we preserve the user's original rflags except clearing
    // the trap flag so a stray TF from the kernel path doesn't trap the
    // handler.
    sf->rflags &= ~(1ULL << 8);  // TF
}

void return_work_handler(void *user_frame) {
    saved_frame_t *sf = (saved_frame_t *)user_frame;
    process_t *p = proc_current();
    if (!p) return;

    // Phase G: perform pending execve first, so we land on the new image
    // and (optionally) deliver any still-pending signals against it.
    if (p->return_work & RETURN_WORK_EXECPENDING) {
        extern void proc_execve_finalize(void *user_frame);
        proc_execve_finalize(user_frame);
    }

    if (!(p->return_work & RETURN_WORK_SIGPENDING)) {
        p->return_work = 0;
        return;
    }

    uint64_t deliverable = sig_deliverable(p);
    if (!deliverable) {
        p->return_work &= ~RETURN_WORK_SIGPENDING;
        return;
    }

    // Deliver the lowest-numbered deliverable signal this pass. (Linux
    // picks the lowest; so do we. Additional signals wait for the next
    // syscall-return or rt_sigreturn tick.)
    int signo = 1;
    while (signo <= NSIG && !(deliverable & (1ULL << (signo - 1)))) {
        signo++;
    }
    if (signo > NSIG) {
        p->return_work &= ~RETURN_WORK_SIGPENDING;
        return;
    }

    deliver_signal(sf, p, signo);

    // If more signals remain pending, leave the bit set so the next
    // return path picks them up.
    if (sig_deliverable(p) == 0) {
        p->return_work &= ~RETURN_WORK_SIGPENDING;
    }
}

void syscall_check_return_work(void *user_frame) {
    process_t *p = proc_current();
    if (!p) return;
    if (p->return_work == 0) return;
    return_work_handler(user_frame);
}

// ============================================================================
// Trampoline registration (set by sigaction on first install)
// ============================================================================

uint64_t g_sig_trampoline = 0;

// ============================================================================
// Syscalls
// ============================================================================

int64_t sys_kill(int pid, int signo) {
    if (signo < 0 || signo > NSIG) return -1;
    if (pid <= 0) return -1;  // broadcasts not supported yet
    process_t *tgt = proc_get((uint32_t)pid);
    if (!tgt) return -1;
    if (signo == 0) return 0;  // POSIX existence probe
    sig_raise(tgt, signo);
    return 0;
}

int64_t sys_sigaction(int signo, const void *new_act, void *old_act) {
    if (signo <= 0 || signo > NSIG) return -1;
    // SIGKILL and SIGSTOP are not catchable.
    if (signo == SIGKILL || signo == SIGSTOP) return -1;
    process_t *p = proc_current();
    if (!p) return -1;

    const k_sigaction_t *na = (const k_sigaction_t *)new_act;
    k_sigaction_t *oa = (k_sigaction_t *)old_act;

    if (oa) {
        oa->sa_handler  = p->sig_handlers[signo - 1];
        oa->sa_mask     = p->sig_handler_mask[signo - 1];
        oa->sa_flags    = (uint32_t)p->sig_flags[signo - 1];
        oa->__pad       = 0;
        oa->sa_restorer = (void *)0;
        oa->__reserved  = 0;
    }

    if (na) {
        p->sig_handlers[signo - 1]     = na->sa_handler;
        p->sig_handler_mask[signo - 1] = na->sa_mask;
        p->sig_flags[signo - 1]        = na->sa_flags;

        // The first sigaction call with a real handler is expected to
        // carry the trampoline address in __reserved. We latch it into
        // g_sig_trampoline. Subsequent calls may pass 0 and it's ignored.
        if (na->__reserved != 0 && g_sig_trampoline == 0) {
            g_sig_trampoline = na->__reserved;
        }
    }
    return 0;
}

int64_t sys_sigprocmask(int how, const uint64_t *set, uint64_t *oldset) {
    process_t *p = proc_current();
    if (!p) return -1;

    if (oldset) *oldset = p->sig_mask;
    if (!set) return 0;

    uint64_t nv = *set;
    // SIGKILL and SIGSTOP cannot be masked.
    nv &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

    switch (how) {
        case SIG_BLOCK:   p->sig_mask |= nv; break;
        case SIG_UNBLOCK: p->sig_mask &= ~nv; break;
        case SIG_SETMASK: p->sig_mask = nv; break;
        default: return -1;
    }

    // Unmasking may have exposed a pending signal.
    if (sig_deliverable(p)) {
        p->return_work |= RETURN_WORK_SIGPENDING;
    }
    return 0;
}

// sys_rt_sigreturn is called via the libc trampoline after a handler returns.
// The user stack currently points at a sigframe (below it: the original
// "return address" slot we filled with the trampoline, which ret consumed).
// We restore the saved state into the kernel's saved IRET frame so SYSRET
// puts the process back exactly where it was.
//
// Returns the value to leave in rax. We return saved_rax so the syscall
// that was interrupted sees its original return value. Because the asm
// layer overwrites rax with our return here, we must use the user_frame
// pointer to rewrite the saved rax directly.
//
// NOTE: The signal trampoline calls this via the regular syscall path,
// which re-enters syscall_return_path. The frame we're returning through
// is the rt_sigreturn syscall's own frame, not the handler's. The user
// stack at this point is at (handler_entry_rsp - (trampoline already popped)).
// It points at the sigframe we built in deliver_signal.
int64_t sys_rt_sigreturn(void) {
    process_t *p = proc_current();
    if (!p) return -1;

    // To get at the kernel-saved frame, we rely on a helper: the syscall
    // asm passed a pointer to its own saved frame to syscall_check_return_work.
    // For rt_sigreturn we instead read our user_rsp from the current
    // syscall's saved frame. The easiest way is to have the asm expose a
    // per-CPU pointer; for MVP we inline a re-read via a kernel helper
    // that walks up from a known offset. That's too fragile. Instead we
    // take a simpler route: use the current kernel RSP to locate the frame.
    //
    // The frame is immediately above the saved C-call padding: at the
    // syscall entry we pushed 15 GPRs + IRET frame then args. By the time
    // we're in sys_rt_sigreturn, rsp has moved beyond the syscall C call
    // frame, but the saved frame is at a fixed offset from tss.rsp0
    // (the kernel stack top).

    // Read the syscall saved frame through the pointer syscall.asm
    // publishes at the hook point. Single-CPU: this is always the frame
    // of the currently-executing syscall (rt_sigreturn's own frame).
    extern uint64_t g_syscall_saved_frame;
    saved_frame_t *sf = (saved_frame_t *)g_syscall_saved_frame;
    if (!sf) return -1;

    // User RSP currently points at the sigframe.
    sigframe_t *frame = (sigframe_t *)sf->user_rsp;

    // Restore saved state.
    sf->r15      = frame->saved_r15;
    sf->r14      = frame->saved_r14;
    sf->r13      = frame->saved_r13;
    sf->r12      = frame->saved_r12;
    sf->r11      = frame->saved_r11;
    sf->r10      = frame->saved_r10;
    sf->r9       = frame->saved_r9;
    sf->r8       = frame->saved_r8;
    sf->rbp      = frame->saved_rbp;
    sf->rdi      = frame->saved_rdi;
    sf->rsi      = frame->saved_rsi;
    sf->rdx      = frame->saved_rdx;
    sf->rcx      = frame->saved_rcx;
    sf->rbx      = frame->saved_rbx;
    sf->rax      = frame->saved_rax;
    sf->rip      = frame->saved_rip;
    sf->rflags   = frame->saved_rflags;
    sf->user_rsp = frame->saved_rsp;

    // Restore the saved signal mask from the frame.
    p->sig_mask = frame->saved_mask;

    // Return value is whatever was in saved_rax; we already wrote it above.
    // Returning from this C function sets rax, which the asm will
    // overwrite with our return into the saved frame; but we've already
    // rewritten that slot. So we return the same value to keep it stable.
    return (int64_t)frame->saved_rax;
}

// #513: alarm(2), for real.
//
// This was a stub that took the seconds argument, ignored it, and returned 0.
// Its own comment admitted it "relies on a follow-up patch to check" that never
// landed. That is the worst possible failure mode: 0 is POSIX for "no previous
// alarm was set", i.e. a plausible SUCCESS, so a caller (userland libc exports
// alarm() at userland/libc/signal.c:99) armed a timer, got a success back, and
// then waited forever for a SIGALRM that no code path could ever raise. A
// syscall that silently no-ops is worse than one that returns ENOSYS, because
// ENOSYS is a fact the caller can branch on.
//
// Implemented rather than stubbed out: everything needed already exists. The
// deadline lives in the PCB (process_t::alarm_time, absolute ticks, 0 =
// disarmed) and the sweep is folded into wake_sleeping_procs() in process.c,
// which already walks the whole proc table on every schedule pass. See the
// comment at that sweep for why it checks EVERY live process rather than only
// sleeping ones.
//
// POSIX contract implemented here:
//   - returns the number of seconds REMAINING on any previous alarm (0 if none)
//   - alarm(0) cancels a pending alarm and returns its remaining seconds
//   - a new alarm replaces (does not stack with) the previous one
int64_t sys_alarm(uint32_t seconds) {
    process_t *p = proc_current();
    if (!p) return 0;

    extern volatile uint64_t timer_ticks;
    extern uint32_t g_timer_hz;
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;

    // Remaining time on the previous alarm, rounded UP so a live alarm never
    // reports 0 (0 is indistinguishable from "no alarm was set").
    uint64_t prev = 0;
    if (p->alarm_time != 0) {
        int64_t left = (int64_t)(p->alarm_time - timer_ticks);   // signed: wrap-safe
        if (left > 0) prev = ((uint64_t)left + hz - 1) / hz;
    }

    // Arm or cancel. Interrupts off: the sweep runs from the tick, so the
    // read-modify-write of alarm_time must not be observed half-done.
    __asm__ volatile("cli");
    p->alarm_time = seconds ? (timer_ticks + (uint64_t)seconds * hz) : 0;
    __asm__ volatile("sti");

    return (int64_t)prev;
}

// A single wait queue shared by every pause()r. It is correct for this to be
// shared rather than per-process: the ONLY waker is sig_raise(), which targets
// one specific process via wake_up_process(p) -> p->wait_entry, unlinking just
// that process's entry. Nothing ever calls wake_up() on this queue, so no
// pauser can be woken by another pauser's signal. That keeps sys_pause off
// process_t (a high-blast-radius shared struct) for zero behavioural cost.
static wait_queue_head_t g_pause_wq = { .head = NULL, .lock = SPINLOCK_INIT };

int64_t sys_pause(void) {
    // #426: this used to be `while (sig_deliverable(p) == 0) proc_yield();`, a
    // yield-spin that kept the process permanently runnable and burned a core
    // for the entire duration of a pause() -- which is, by definition, "until
    // something happens", i.e. potentially forever.
    //
    // Class A, no timeout, and the wake source ALREADY EXISTS: sig_raise()
    // (signal.c:46) already calls wake_up_process(target) whenever the target
    // has a wait_entry, which parking here is exactly what gives us. There is
    // deliberately no timeout: pause() waiting forever for a signal that never
    // comes is not a bug, it is the specified behaviour.
    //
    // The check-then-park race is closed by the wait_event_interruptible macro
    // itself, which re-tests the condition AFTER __wait_prepare() has published
    // our wait_entry. So a signal landing in the window between our first test
    // and the park cannot be lost: either sig_raise sees wait_entry and kicks
    // us, or it set sig_pending before our recheck and the recheck sees it.
    process_t *p = proc_current();
    if (!p) return -1;

    (void)wait_event_interruptible(&g_pause_wq, sig_deliverable(p) != 0);

    // Either a signal became deliverable (WAIT_OK) or sig_raise interrupted the
    // park (WAIT_EINTR). Both mean the same thing to pause(), and the
    // return_work hook delivers the signal on the way back out to Ring 3.
    return -1;  // POSIX: pause always returns -1 with errno=EINTR
}
