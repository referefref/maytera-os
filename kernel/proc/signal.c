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
#include "../security/validate.h"  // #509: copy_*_user adoption
#include "../serial.h"
#include "../string.h"   // #dosowner: strcmp() for the Ring-0 DOS-worker name match in sys_kill
#include "../sync/waitq.h"   // #426: sys_pause() parks instead of yield-spinning
#include "../cpu/mono.h"      // #483/#499: sched_now_ms() for the alarm deadline

// #565: SZ_K_SIGACTION in rustkern/argtab.rs is the byte count the syscall
// pointer chokepoint validates for SYS_SIGACTION's new_act (READ) and old_act
// (WRITE). Lock it to the real struct here (public struct, its owning TU) so a
// field change fails the build pointing at THIS line rather than silently
// validating the wrong length. Same discipline as syscall_argtab_lock.c.
_Static_assert(sizeof(k_sigaction_t) == 40,
               "#565 argtab: SZ_K_SIGACTION in rustkern/argtab.rs is stale");

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

// ===========================================================================
// #SMPGLOBALS (2026-08-30): locating the frame SYSRET will pop.
// ===========================================================================
//
// syscall_entry (proc/syscall.asm) reloads rsp from gs:0 on EVERY syscall.
// gs:0 is written only by cpu_set_kernel_stack(), which every scheduler path
// calls with (stack_base + stack_size) of the task it is switching to. The
// stub then pushes exactly the 5 IRET qwords and 15 GPRs below. So the frame
// is always at the very top of THIS task's ring-0 stack, and nowhere else.
//
// That makes it a property of the TASK, which is what the old code got wrong:
// it read a single global that the asm wrote on every syscall return on every
// core, i.e. the frame of whichever task finished a syscall most recently
// anywhere in the system. Per-CPU storage would not have fixed it (both tasks
// share cpu 0's slot on the shipping single-CPU boot, and a task can migrate
// between the publish and the read); per-task does.
_Static_assert(sizeof(saved_frame_t) == 20 * 8,
               "saved_frame_t must be the 5 IRET qwords + 15 GPRs that "
               "syscall_entry pushes; see proc/syscall.asm");

// Always compiled, never gated: a guard whose counter cannot be read is a
// guard nobody can trust. Reported by sigframe_report().
uint64_t g_sigframe_calls        = 0;  // rt_sigreturn calls
uint64_t g_sigframe_refused      = 0;  // .. with no derivable frame
uint64_t g_sigframe_publish_odd  = 0;  // asm frame != derived frame (would mean
                                       // syscall_entry's push list changed)

// Derive this task's syscall frame. Correct on any core, and after any
// migration, because it reads only the task's own stack bounds.
static saved_frame_t *task_syscall_frame(process_t *p) {
    if (!p || !p->stack_base || p->stack_size < sizeof(saved_frame_t)) return 0;
    uint64_t top = (uint64_t)p->stack_base + p->stack_size;
    return (saved_frame_t *)(top - sizeof(saved_frame_t));
}

#ifdef SIGFRAME_DIFF
// -------- differential / RED arm. make SIGFRAMEDIFF=1 only. ---------------
// g_syscall_saved_frame is the deleted global, kept alive here so the fix can
// be MEASURED against the behaviour it replaces instead of argued about.
extern uint64_t g_syscall_saved_frame;
extern process_t *sigframe_owner_of(uint64_t frame_addr, uint64_t frame_bytes);
int g_sigframe_legacy_arm = 0;        // set by main.c when /SIGFRAMEBUG.TXT
uint64_t g_sigframe_legacy_wrong = 0; // legacy global named the WRONG frame
uint64_t g_sigframe_legacy_foreign = 0; // .. and it belonged to another task
#endif

void sigframe_report(void) {
    kprintf("[SIGFRAME] rt_sigreturn calls=%llu refused=%llu publish_odd=%llu\n",
            (unsigned long long)g_sigframe_calls,
            (unsigned long long)g_sigframe_refused,
            (unsigned long long)g_sigframe_publish_odd);
#ifdef SIGFRAME_DIFF
    kprintf("[SIGFRAME] DIFF arm=%s legacy_wrong=%llu legacy_foreign=%llu\n",
            g_sigframe_legacy_arm ? "LEGACY(RED)" : "PERTASK(GREEN)",
            (unsigned long long)g_sigframe_legacy_wrong,
            (unsigned long long)g_sigframe_legacy_foreign);
#endif
}

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

    // #19/#645: build the frame in KERNEL memory and hand it over with the
    // canonical primitive. Two things change, both of them fixes:
    //   * copy_to_user brackets the store with stac/clac, so this path stops
    //     being a guaranteed #PF the moment CR4.SMAP is armed. There are 21
    //     field stores here; a 21-line AC window would be far wider than the
    //     access, and this is one instruction's worth of window instead.
    //   * the comment above admitted the old code took a kernel page fault on a
    //     bogus user_rsp ("Phase D4's paranoia can add validation"). It never
    //     did. copy_to_user validates U/S and carries the exception fixup, so a
    //     hostile or simply overflowed stack now terminates ONE process rather
    //     than panicking the kernel.
    sigframe_t kframe;
    kframe.saved_rax    = sf->rax;
    kframe.saved_rbx    = sf->rbx;
    kframe.saved_rcx    = sf->rcx;
    kframe.saved_rdx    = sf->rdx;
    kframe.saved_rsi    = sf->rsi;
    kframe.saved_rdi    = sf->rdi;
    kframe.saved_rbp    = sf->rbp;
    kframe.saved_r8     = sf->r8;
    kframe.saved_r9     = sf->r9;
    kframe.saved_r10    = sf->r10;
    kframe.saved_r11    = sf->r11;
    kframe.saved_r12    = sf->r12;
    kframe.saved_r13    = sf->r13;
    kframe.saved_r14    = sf->r14;
    kframe.saved_r15    = sf->r15;
    kframe.saved_rip    = sf->rip;
    kframe.saved_rflags = sf->rflags;
    kframe.saved_rsp    = sf->user_rsp;
    kframe.saved_mask   = p->sig_mask;
    kframe.signo        = (uint32_t)signo;
    kframe.__pad        = 0;
    if (copy_to_user(frame, &kframe, sizeof(kframe)) != 0) {
        kprintf("[SIG] pid=%u signal %d: user stack 0x%llx not writable; "
                "terminating instead of faulting in Ring 0\n",
                p->pid, signo, (unsigned long long)user_rsp);
        p->sig_pending &= ~(1ULL << (signo - 1));
        proc_exit(128 + signo);
        return;
    }

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
    // #SMPGLOBALS: THIS process's trampoline. It used to be one global latched
    // from whichever process called sigaction first, which under PIE handed
    // every other process an address inside somebody else's image.
    uint64_t trampoline = p->sig_trampoline;
    if (trampoline == 0) {
        // No trampoline registered; fall back to terminating the process
        // rather than jumping to zero.
        kprintf("[SIG] No trampoline; terminating pid=%u on signal %d\n",
                p->pid, signo);
        proc_exit(128 + signo);
        return;
    }

    // Push trampoline as the "return address" the handler will ret to.
    // #19/#645: one qword to the USER stack, through the primitive.
    user_rsp -= 8;
    if (copy_to_user((void *)user_rsp, &trampoline, sizeof(trampoline)) != 0) {
        kprintf("[SIG] pid=%u signal %d: trampoline slot 0x%llx not writable; "
                "terminating\n", p->pid, signo, (unsigned long long)user_rsp);
        p->sig_pending &= ~(1ULL << (signo - 1));
        proc_exit(128 + signo);
        return;
    }

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

    // #SMPGLOBALS: publish THIS task's syscall frame. The asm used to store
    // rsp into one global here; the pointer it stored is the rdi we were just
    // handed, so this costs the hot path nothing extra.
    //
    // The value is constant for a task (top of its own ring-0 stack), so the
    // body runs ONCE per task and is a predicted-not-taken compare after that.
    // When it does run it cross-checks the asm against the derivation, so a
    // change to syscall_entry's push list is caught on the first syscall after
    // it rather than the next time somebody handles a signal.
    if (__builtin_expect(p->syscall_frame != (uint64_t)user_frame, 0)) {
        p->syscall_frame = (uint64_t)user_frame;
        saved_frame_t *derived = task_syscall_frame(p);
        if ((uint64_t)derived != (uint64_t)user_frame) {
            g_sigframe_publish_odd++;
            if (g_sigframe_publish_odd <= 8)
                kprintf("[SIGFRAME] pid=%u '%s' asm frame 0x%llx != derived "
                        "0x%llx (kstack 0x%llx size 0x%llx)\n",
                        p->pid, p->name,
                        (unsigned long long)(uint64_t)user_frame,
                        (unsigned long long)(uint64_t)derived,
                        (unsigned long long)(uint64_t)p->stack_base,
                        (unsigned long long)p->stack_size);
        }
    }

    if (p->return_work == 0) return;
    return_work_handler(user_frame);
}

// ============================================================================
// Trampoline registration: PER PROCESS, in process_t::sig_trampoline. See the
// field's comment in proc/process.h for what the global that used to live here
// did to every process that was not the first to install a handler.
// ============================================================================


// ============================================================================
// Syscalls
// ============================================================================

int64_t sys_kill(int pid, int signo) {
    if (signo < 0 || signo > NSIG) return -1;
    if (pid <= 0) return -1;  // broadcasts not supported yet
    process_t *tgt = proc_get((uint32_t)pid);
    if (!tgt) return -1;
    if (signo == 0) return 0;  // POSIX existence probe (a zombie DOES exist)
    // #161: A ZOMBIE HAS ALREADY EXITED. Raising a signal on it did exactly
    // nothing (there is no Ring 3 left to deliver to, and sig_deliverable is
    // only ever consulted on a return path that corpse will never take) and
    // this returned 0, i.e. SUCCESS. Task Manager's Kill and the dock's Force
    // Quit both took that 0 at face value, so the owner clicked Kill on a
    // zombie AssaultCube and the button reported success while nothing
    // happened - the "silently does nothing" class this project keeps
    // reproducing. -ESRCH is the truth: there is no process to signal, only a
    // corpse waiting for its parent to reap it. The UI can now say so.
    if (tgt->state == PROC_STATE_ZOMBIE) return -3;  // -ESRCH
    sig_raise(tgt, signo);

    // (#dosowner) A TERMINATING SIGNAL AIMED AT THE RING-0 DOS WORKER NEEDS A
    // SECOND, NON-SIGNAL ROUTE, OR IT IS A SILENT NO-OP THAT RETURNS SUCCESS.
    //
    // sig_raise() above only sets a pending bit. That bit is consumed at
    // exactly two chokepoints, both of which are RETURNS TO RING 3:
    // syscall_check_return_work() on the way out of a syscall (syscall.asm)
    // and sig_async_terminate_pending() on an interrupt return (cpu/idt.c,
    // #161). The in-kernel DOS interpreter is a Ring-0 kernel worker
    // (dos/dosexec.c: proc_create("dos", dos_proc_entry, ...)); it never
    // executes SYSCALL and every interrupt frame it takes has cs&3 == 0, so it
    // reaches NEITHER. #compkill measured this: SIGKILL against it does
    // nothing at all, forever, while this function returns 0 for success.
    //
    // gui/desktop.c's session_end_teardown() already solved this once, for the
    // same worker, by calling the SAME stop request the titlebar X uses. This
    // is the second call site for that decision, not a second mechanism:
    // dos_request_close() is idempotent (a flag set plus a wake of a possibly
    // empty wait queue) and is a safe no-op when no guest is running.
    //
    // WHY IT MATTERS NOW rather than as a standing latent bug: stamping
    // owner_pid on the DOS host window (proc/syscall.c
    // win16_host_route_close_to_dos) makes sys_wm_get_windows() populate
    // app_id, and the compositor's dock right-click menu gates its "Force
    // Quit" item on app_id being non-empty (contextmenu.c). Force Quit
    // dispatches by process NAME through SYS_KILL (taskbar.c
    // tb_force_quit_dispatch). Without this, the icon fix would have shipped a
    // brand new menu item whose only possible outcome is nothing happening -
    // strictly worse than the wrong icon it replaced. Task Manager's End Task
    // on the same row was already broken in exactly this way and is fixed by
    // the same lines.
    //
    // Scoped to terminating signals: a non-terminating signal (or a signal
    // with a handler, which a kernel worker cannot have) must not tear a guest
    // down. Matched by NAME because a Ring-0 worker carries no window, fd or
    // session this could key off instead - the same identity, and the same
    // reason, session_end_teardown() already uses.
    if ((signo == SIGKILL || signo == SIGTERM) &&
        tgt->privilege != PRIV_USER &&
        (strcmp(tgt->name, "dos") == 0 || strcmp(tgt->name, "dosrun") == 0)) {
        extern void dos_request_close(void);
        dos_request_close();
        kprintf("[dos] sig %d on Ring-0 worker pid %u '%s': SIGKILL cannot "
                "reach it (#compkill), also sent dos_request_close()\n",
                signo, (unsigned)tgt->pid, tgt->name);
    }
    return 0;
}

// #161: ASYNCHRONOUS TERMINATION POINT - the reason Force Quit did not work on
// a LIVE, BUSY app.
//
// Until this ticket, signal delivery had exactly ONE site: syscall.asm:135
// calls syscall_check_return_work() on the way out of a syscall. Nothing runs
// return_work_handler() on the IRETQ return from an interrupt. So a Ring 3
// process that is not making syscalls - a game or a media player grinding
// through a frame, or anything wedged in a loop of its own - carried a pending
// SIGKILL forever, and SIGKILL is precisely the signal whose whole contract is
// that it does NOT depend on the target cooperating.
//
// This is the predicate the interrupt return path (cpu/idt.c) consults. It
// answers ONE question: is the signal the syscall path would deliver next a
// signal that terminates the process outright, needing no user stack frame?
// If so the caller can act on it from an asynchronous context, because
// terminating requires nothing of the victim.
//
// It deliberately reuses BOTH existing authorities rather than restating them:
// sig_deliverable() for what is unblocked (including the SIGKILL/SIGSTOP
// unmaskable rule) and default_action() for what "default" means per signal.
// It also honours the same LOWEST-numbered-first choice return_work_handler()
// makes, and bails the moment that lowest signal needs a handler frame, so the
// two paths can never disagree about which signal is next.
//
// WHY THIS IS C AND NOT RUST (2026-07-16 rule): it is not new logic. It is a
// second CALL SITE for the selection this file already performs, and the only
// way to express it in rustkern/ would be to copy default_action()'s table and
// sig_deliverable()'s mask rule across the FFI - forking the shared primitive,
// which is the thing the reuse rule forbids. The decision stays where its two
// inputs are defined.
//
// Returns the signal number to terminate on, or 0 for "nothing to do here".
int sig_async_terminate_pending(void) {
    process_t *p = proc_current();
    if (!p) return 0;
    if (!(p->return_work & RETURN_WORK_SIGPENDING)) return 0;
    uint64_t deliverable = sig_deliverable(p);
    if (!deliverable) return 0;
    for (int signo = 1; signo <= NSIG; signo++) {
        if (!(deliverable & (1ULL << (signo - 1)))) continue;
        // Lowest deliverable signal wins, exactly as return_work_handler does.
        // If IT needs a user stack frame, we do nothing at all and let the
        // syscall path handle the whole queue in order.
        if (p->sig_handlers[signo - 1] != SIG_DFL) return 0;
        if (default_action(signo) != 0) return 0;
        return signo;
    }
    return 0;
}

int64_t sys_sigaction(int signo, const void *new_act, void *old_act) {
    if (signo <= 0 || signo > NSIG) return -1;
    // SIGKILL and SIGSTOP are not catchable.
    if (signo == SIGKILL || signo == SIGSTOP) return -1;
    process_t *p = proc_current();
    if (!p) return -1;

    // #509: use copy_*_user (TOCTOU-safe) instead of dereferencing the
    // user-supplied k_sigaction_t pointers directly. old_act is written FIRST
    // (state unchanged if it faults) and via copy_to_user, so a racing unmap of
    // old_act cannot land a Ring-0 write on a freed/remapped frame.
    if (old_act) {
        k_sigaction_t oa;
        oa.sa_handler  = p->sig_handlers[signo - 1];
        oa.sa_mask     = p->sig_handler_mask[signo - 1];
        oa.sa_flags    = (uint32_t)p->sig_flags[signo - 1];
        oa.__pad       = 0;
        oa.sa_restorer = (void *)0;
        oa.__reserved  = 0;
        if (copy_to_user(old_act, &oa, sizeof(oa)) != 0) return -14;  // EFAULT
    }

    if (new_act) {
        k_sigaction_t na;
        if (copy_from_user(&na, new_act, sizeof(na)) != 0) return -14;  // EFAULT
        p->sig_handlers[signo - 1]     = na.sa_handler;
        p->sig_handler_mask[signo - 1] = na.sa_mask;
        p->sig_flags[signo - 1]        = na.sa_flags;

        // The first sigaction call with a real handler carries the trampoline
        // address in __reserved; latch it. Subsequent calls may pass 0.
        if (na.__reserved != 0 && p->sig_trampoline == 0) {
            p->sig_trampoline = na.__reserved;
        }
    }
    return 0;
}

int64_t sys_sigprocmask(int how, const uint64_t *set, uint64_t *oldset) {
    process_t *p = proc_current();
    if (!p) return -1;

    // #509: TOCTOU-safe read/write of the user masks via copy_*_user.
    if (oldset) {
        uint64_t old = p->sig_mask;
        if (copy_to_user(oldset, &old, sizeof(old)) != 0) return -14;  // EFAULT
    }
    if (!set) return 0;

    uint64_t nv;
    if (copy_from_user(&nv, set, sizeof(nv)) != 0) return -14;  // EFAULT
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

    // #SMPGLOBALS: derive rt_sigreturn's OWN frame from THIS task's ring-0
    // stack. See the writeup above task_syscall_frame(). The global this used
    // to read named whichever task last finished a syscall anywhere in the
    // system, so rt_sigreturn rewrote a bystander's saved registers, RIP and
    // user RSP whenever that was not us.
    g_sigframe_calls++;
    saved_frame_t *sf = task_syscall_frame(p);
    if (!sf) {
        g_sigframe_refused++;
        kprintf("[SIG] pid=%u rt_sigreturn: no ring-0 stack recorded "
                "(base=0x%llx size=0x%llx); refusing\n", p->pid,
                (unsigned long long)(uint64_t)p->stack_base,
                (unsigned long long)p->stack_size);
        return -1;
    }
#ifdef SIGFRAME_DIFF
    {
        uint64_t legacy = g_syscall_saved_frame;
        // Print the RAW pair for the first 20 calls, match or not. Three
        // reproducer attempts read legacy_wrong=0 and the counter alone could
        // not say whether that meant "the global was right" or "the instrument
        // is not seeing what I think". Show the numbers.
        // Sample ACROSS the run, not just the first 20 calls: the first 20 are
        // all inside sigprobe's phase-1 tight loop, where nothing else is
        // scheduled, so they answer a question nobody was asking.
        static uint64_t dbg_n = 0;
        if ((g_sigframe_calls % 97) == 0 && dbg_n < 60) {
            dbg_n++;
            process_t *own = sigframe_owner_of(legacy, sizeof(saved_frame_t));
            kprintf("[SIGFRAME-RAW] #%llu pid=%u '%s' legacy=0x%llx own=0x%llx "
                    "owner=%s kstack=[0x%llx,+0x%llx)\n",
                    (unsigned long long)dbg_n, p->pid, p->name,
                    (unsigned long long)legacy, (unsigned long long)(uint64_t)sf,
                    own ? own->name : "(none)",
                    (unsigned long long)(uint64_t)p->stack_base,
                    (unsigned long long)p->stack_size);
        }
        if (legacy != (uint64_t)sf) {
            g_sigframe_legacy_wrong++;
            process_t *victim = sigframe_owner_of(legacy, sizeof(saved_frame_t));
            if (victim && victim != p) g_sigframe_legacy_foreign++;
            if (g_sigframe_legacy_wrong <= 24)
                kprintf("[SIGFRAME] pid=%u '%s' rt_sigreturn: legacy global "
                        "0x%llx != own frame 0x%llx -> would rewrite %s\n",
                        p->pid, p->name, (unsigned long long)legacy,
                        (unsigned long long)(uint64_t)sf,
                        victim ? victim->name : "(no live task owns it)");
        }
        // RED arm: reproduce the pre-fix behaviour exactly, so the corruption
        // can be SEEN rather than inferred from a counter.
        if (g_sigframe_legacy_arm && legacy) sf = (saved_frame_t *)legacy;
    }
#endif

    // User RSP currently points at the sigframe.
    // #19/#645: read it into KERNEL memory through the primitive first. This
    // was a raw Ring-0 read of a fully user-controlled address: sf->user_rsp is
    // whatever Ring 3 left in RSP before invoking rt_sigreturn, so the old code
    // would happily load 21 kernel qwords into the register frame it is about
    // to return through. copy_from_user rejects a PRESENT supervisor page (the
    // confused-deputy half) and its exception fixup handles an unmapped one
    // (the TOCTOU half), and it brackets the read for SMAP.
    // The user frame address is lifted into its own local FIRST. Passing
    // `sf->user_rsp` directly to copy_from_user makes smap-uaccess-lint's B3
    // provenance rule attribute `sf` itself as a user pointer, and every later
    // `sf->` field restore (all kernel memory, the syscall's own saved frame)
    // then reads as an unbracketed user deref. Naming the user address is both
    // clearer and what the lint needs.
    uint64_t ufrm = sf->user_rsp;
    sigframe_t kframe;
    if (copy_from_user(&kframe, (const void *)ufrm, sizeof(kframe)) != 0) {
        kprintf("[SIG] pid=%u rt_sigreturn: sigframe at 0x%llx is not readable "
                "user memory; refusing\n", p->pid, (unsigned long long)ufrm);
        return -1;
    }
    const sigframe_t *frame = &kframe;

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

    // #483/#499: alarm_time is an absolute mono_ms() deadline in MILLISECONDS
    // now, not a timer_ticks value, so a KVM tick burst cannot fire SIGALRM
    // early. The sweep in wake_sleeping_procs() reads the same mono_ms() clock.
    uint64_t now = sched_now_ms();

    // Remaining time on the previous alarm, rounded UP so a live alarm never
    // reports 0 (0 is indistinguishable from "no alarm was set").
    uint64_t prev = 0;
    if (p->alarm_time != 0) {
        int64_t left = (int64_t)(p->alarm_time - now);   // ms, signed: wrap-safe
        if (left > 0) prev = ((uint64_t)left + 999) / 1000;   // ms -> seconds, round up
    }

    // Arm or cancel. Interrupts off: the sweep runs from the tick, so the
    // read-modify-write of alarm_time must not be observed half-done.
    __asm__ volatile("cli");
    p->alarm_time = seconds ? (now + (uint64_t)seconds * 1000ULL) : 0;
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
