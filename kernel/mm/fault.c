// fault.c - Page-fault (#PF, vector 14) entry point and glue (#429)
//
// Before #429 the IDT had NO handler registered for vector 14: a page fault
// fell through to the generic exception path in cpu/idt.c, which for a user
// fault killed the process and for a kernel fault panicked. The ~1300-line
// demand-paging subsystem in mm/demand.c had ZERO callers, so demand-zero
// pages, lazily-mapped mmap regions and copy-on-write fork never worked; every
// page fault was instantly fatal.
//
// This file registers a real #PF handler that:
//   1. Reads CR2 + the error code and asks mm/demand.c to resolve the fault
//      (COW write, demand-zero / lazy mmap page, file-backed page).
//   2. On an unrecoverable USER fault: delivers SIGSEGV. If the process
//      installed a SIGSEGV handler (via #430's now-wired signal path) we build
//      a signal frame and IRET into it; otherwise the default action
//      (terminate this one process, kernel keeps running) is taken via the
//      shared exception_fatal() tail.
//   3. On an unrecoverable KERNEL fault: exception_fatal() panics (a real bug).
//
// It also enables EFER.NXE so the demand paths can mark writable data pages
// no-execute (partial W^X; see cpu_enable_nx / g_nx_enabled).

#include "../types.h"
#include "demand.h"
#include "vmm.h"
#include "../proc/process.h"
#include "../proc/signal.h"
#include "../cpu/idt.h"
#include "../serial.h"
#include "../string.h"

// ---------------------------------------------------------------------------
// NX / W^X support
// ---------------------------------------------------------------------------

// 1 once EFER.NXE has been enabled on the boot CPU (and the demand paths are
// therefore allowed to set the NX bit on writable pages). The demand.c
// handlers consult this before OR-ing in VMM_FLAG_NX; setting bit 63 with NXE
// disabled would raise a reserved-bit page fault, so this gate is mandatory.
int g_nx_enabled = 0;

// Enable no-execute on THIS cpu. Called on the BSP (from isr_init) and on each
// AP (from smp.c ap_entry). EFER is per-CPU, so every core that can run a user
// thread must have NXE set or a page whose PTE has NX=1 would #PF (reserved
// bit) on that core.
void cpu_enable_nx(void) {
    uint32_t a, b, c, d;
    cpuid(0x80000001u, &a, &b, &c, &d);
    if (!(d & (1u << 20))) {
        // CPU does not advertise NX; leave W^X off (honest: cannot enforce).
        return;
    }
    uint64_t efer = rdmsr(0xC0000080u);   // IA32_EFER
    if (!(efer & (1u << 11))) {
        efer |= (1u << 11);               // NXE
        wrmsr(0xC0000080u, efer);
    }
    g_nx_enabled = 1;
}

// ---------------------------------------------------------------------------
// SIGSEGV delivery to a user-installed handler (real recoverable fault)
// ---------------------------------------------------------------------------

// Attempt to redirect the faulting user context into its SIGSEGV handler by
// building a sigframe on the user stack, exactly like proc/signal.c's
// deliver_signal() but operating on the interrupt frame. Returns 0 if the
// handler was set up (caller should IRET back to run it), -1 if there is no
// catchable handler or the user stack looks unusable (caller should fall back
// to the default terminate action).
static int deliver_segv_handler(process_t *p, interrupt_frame_t *frame) {
    void *handler = p->sig_handlers[SIGSEGV - 1];
    if (handler == SIG_DFL || handler == SIG_IGN) {
        return -1;  // no catchable handler -> default action
    }
    extern uint64_t g_sig_trampoline;
    if (g_sig_trampoline == 0) {
        return -1;  // libc never registered a trampoline
    }

    // The user stack must be present+writable in the faulting address space,
    // or writing the sigframe would just fault again (a stack-overflow SIGSEGV
    // cannot be delivered without a sigaltstack; take the default action).
    uint64_t probe = (frame->rsp - 256) & ~0xFFFULL;
    if (frame->rsp < 0x1000 || frame->rsp >= 0x0000800000000000ULL) return -1;
    if (vmm_get_physical_in(p->cr3, probe) == 0) return -1;

    uint64_t user_rsp = frame->rsp;
    user_rsp -= 128;                       // red-zone pad
    user_rsp -= sizeof(sigframe_t);
    user_rsp &= ~0xFULL;                   // 16-byte align
    if (vmm_get_physical_in(p->cr3, user_rsp & ~0xFFFULL) == 0) return -1;

    sigframe_t *sfr = (sigframe_t *)user_rsp;
    sfr->saved_rax = frame->rax; sfr->saved_rbx = frame->rbx;
    sfr->saved_rcx = frame->rcx; sfr->saved_rdx = frame->rdx;
    sfr->saved_rsi = frame->rsi; sfr->saved_rdi = frame->rdi;
    sfr->saved_rbp = frame->rbp;
    sfr->saved_r8  = frame->r8;  sfr->saved_r9  = frame->r9;
    sfr->saved_r10 = frame->r10; sfr->saved_r11 = frame->r11;
    sfr->saved_r12 = frame->r12; sfr->saved_r13 = frame->r13;
    sfr->saved_r14 = frame->r14; sfr->saved_r15 = frame->r15;
    sfr->saved_rip = frame->rip; sfr->saved_rflags = frame->rflags;
    sfr->saved_rsp = frame->rsp; sfr->saved_mask = p->sig_mask;
    sfr->signo = (uint32_t)SIGSEGV; sfr->__pad = 0;

    // Block SIGSEGV (unless SA_NODEFER) plus the handler's sa_mask while it runs.
    uint32_t flags = (uint32_t)p->sig_flags[SIGSEGV - 1];
    if (!(flags & SA_NODEFER)) p->sig_mask |= (1ULL << (SIGSEGV - 1));
    p->sig_mask |= p->sig_handler_mask[SIGSEGV - 1];
    p->sig_pending &= ~(1ULL << (SIGSEGV - 1));
    if (flags & SA_RESETHAND) {
        p->sig_handlers[SIGSEGV - 1] = SIG_DFL;
        p->sig_flags[SIGSEGV - 1] = 0;
        p->sig_handler_mask[SIGSEGV - 1] = 0;
    }

    // Push the trampoline as the return address the handler will `ret` to.
    user_rsp -= 8;
    *(uint64_t *)user_rsp = g_sig_trampoline;

    frame->rsp = user_rsp;
    frame->rip = (uint64_t)handler;
    frame->rdi = (uint64_t)SIGSEGV;   // first arg to the handler
    frame->rflags &= ~(1ULL << 8);    // clear TF

    kprintf("[#PF] SIGSEGV delivered to handler in pid=%u (rip->0x%lx)\n",
            p->pid, (uint64_t)handler);
    return 0;
}

// ---------------------------------------------------------------------------
// The #PF handler proper (registered on IDT vector 14 by isr_init)
// ---------------------------------------------------------------------------

void page_fault_handler(interrupt_frame_t *frame) {
    uint64_t cr2 = read_cr2();
    uint64_t err = frame->error_code;
    process_t *p = proc_current();

    // 1. Try to resolve as a valid fault: COW write, demand-zero / lazy page,
    //    or file-backed page. mm_fault() also resolves a kernel-mode write to
    //    a user COW page (the copy_to_user case), so it runs regardless of ring.
    if (p && p->cr3 != 0 && mm_fault(p, cr2, err) == 0) {
        return;  // fixed up; IRET re-executes the faulting instruction
    }

    // 2. Unrecoverable. A user fault becomes SIGSEGV.
    int from_user = (frame->cs & 0x3) != 0;
    if (from_user && p) {
        if (deliver_segv_handler(p, frame) == 0) {
            return;  // caught by the process's SIGSEGV handler
        }
        // No handler: default action is terminate. sig_raise records the
        // signal for coherence; exception_fatal() performs the actual
        // terminate-this-one-process + crash diagnostics, kernel survives.
        sig_raise(p, SIGSEGV);
    }

    // 3. Default action (user) or a genuine kernel-mode fault: shared fatal
    //    tail. For user faults it shows the crash dialog and proc_exit()s the
    //    faulting process (kernel keeps ticking); for kernel faults it panics.
    exception_fatal(frame);
}
