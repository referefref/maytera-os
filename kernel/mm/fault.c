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
#include "../security/validate.h"  // #509: copy_*_user under test

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
static int deliver_segv_handler(process_t *p, interrupt_frame_t *frame, uint64_t cr2) {
    void *handler = p->sig_handlers[SIGSEGV - 1];
    if (handler == SIG_DFL || handler == SIG_IGN) {
        return -1;  // no catchable handler -> default action
    }
    // #SMPGLOBALS: THIS process's trampoline (process_t::sig_trampoline), not a
    // machine-wide global. Under PIE the global meant delivering a SIGSEGV by
    // jumping into another process's image.
    if (p->sig_trampoline == 0) {
        return -1;  // this process's libc never registered a trampoline
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

    // #19/#645: build in KERNEL memory, hand over with the canonical primitive.
    // Under CR4.SMAP a raw store here is a #PF *inside the #PF handler*, which
    // is the worst blast radius on the whole ledger. copy_to_user also means a
    // user stack that went bad between the vmm_get_physical_in() probe above
    // and this write returns -EFAULT instead of double-faulting.
    sigframe_t kfr;
    kfr.saved_rax = frame->rax; kfr.saved_rbx = frame->rbx;
    kfr.saved_rcx = frame->rcx; kfr.saved_rdx = frame->rdx;
    kfr.saved_rsi = frame->rsi; kfr.saved_rdi = frame->rdi;
    kfr.saved_rbp = frame->rbp;
    kfr.saved_r8  = frame->r8;  kfr.saved_r9  = frame->r9;
    kfr.saved_r10 = frame->r10; kfr.saved_r11 = frame->r11;
    kfr.saved_r12 = frame->r12; kfr.saved_r13 = frame->r13;
    kfr.saved_r14 = frame->r14; kfr.saved_r15 = frame->r15;
    kfr.saved_rip = frame->rip; kfr.saved_rflags = frame->rflags;
    kfr.saved_rsp = frame->rsp; kfr.saved_mask = p->sig_mask;
    kfr.signo = (uint32_t)SIGSEGV; kfr.__pad = 0;
    if (copy_to_user((void *)user_rsp, &kfr, sizeof(kfr)) != 0) {
        return -1;   // fall back to the default terminate action
    }

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
    // #19/#645: one qword to the USER stack, through the primitive.
    user_rsp -= 8;
    {
        uint64_t tramp = p->sig_trampoline;
        if (copy_to_user((void *)user_rsp, &tramp, sizeof(tramp)) != 0) {
            return -1;
        }
    }

    // #421 phase 5 diagnostic: log the ORIGINAL faulting RIP/CR2 before
    // overwriting frame->rip with the handler entry point below. Without
    // this, the real crash site is only ever visible inside sfr->saved_rip,
    // a USER-STACK structure the kernel never prints; a userland crash
    // handler catching its own SIGSEGV (AssaultCube's signalbinder is the
    // case that motivated this) left no serial-log trace of where the
    // ORIGINAL fault actually happened, only where the handler runs, which
    // is useless for root-causing the real bug. Cheap, permanent, and useful
    // for any future userland SIGSEGV-handler app, not just this one.
    kprintf("[#PF] original fault: pid=%u rip=0x%lx cr2=0x%lx err=0x%lx\n",
            p->pid, frame->rip, cr2, frame->error_code);

    frame->rsp = user_rsp;
    frame->rip = (uint64_t)handler;
    frame->rdi = (uint64_t)SIGSEGV;   // first arg to the handler
    frame->rflags &= ~(1ULL << 8);    // clear TF

    kprintf("[#PF] SIGSEGV delivered to handler in pid=%u (rip->0x%lx)\n",
            p->pid, (uint64_t)handler);
    return 0;
}

// ---------------------------------------------------------------------------
// #509 TOCTOU keystone: user-copy fault fixup (exception table)
// ---------------------------------------------------------------------------
//
// The ONLY safe way for the kernel to touch user memory is through the
// mm/uaccess.asm primitives (__uaccess_copy/set/strncpy/strnlen), which
// copy_*_user() call. Each has a faultable instruction region bracketed by
// _ex_start/_ex_end and a _ex_fixup. If a #PF fires with RIP inside one of
// these regions AND mm_fault() (below) could not resolve it as a legitimate
// lazy/COW demand page, the page is genuinely bad at USE time (a sibling thread
// unmapped it - the TOCTOU race, or it was never valid). We redirect execution
// to the fixup, which makes the primitive return "bytes not copied" / -1, so
// copy_*_user() returns -EFAULT. The kernel survives; only the offending
// syscall fails. This is stateless (keyed on the faulting RIP, not a per-CPU
// flag), so it is inherently correct under SMP and re-entrancy.
extern char __uaccess_copy_ex_start[],    __uaccess_copy_ex_end[],    __uaccess_copy_ex_fixup[];
extern char __uaccess_set_ex_start[],     __uaccess_set_ex_end[],     __uaccess_set_ex_fixup[];
extern char __uaccess_strncpy_ex_start[], __uaccess_strncpy_ex_end[], __uaccess_strncpy_ex_fixup[];
extern char __uaccess_strnlen_ex_start[], __uaccess_strnlen_ex_end[], __uaccess_strnlen_ex_fixup[];

struct uaccess_ex_entry { uint64_t start, end, fixup; };
static const struct uaccess_ex_entry uaccess_ex_table[] = {
    { (uint64_t)__uaccess_copy_ex_start,    (uint64_t)__uaccess_copy_ex_end,    (uint64_t)__uaccess_copy_ex_fixup },
    { (uint64_t)__uaccess_set_ex_start,     (uint64_t)__uaccess_set_ex_end,     (uint64_t)__uaccess_set_ex_fixup },
    { (uint64_t)__uaccess_strncpy_ex_start, (uint64_t)__uaccess_strncpy_ex_end, (uint64_t)__uaccess_strncpy_ex_fixup },
    { (uint64_t)__uaccess_strnlen_ex_start, (uint64_t)__uaccess_strnlen_ex_end, (uint64_t)__uaccess_strnlen_ex_fixup },
};

// Return the fixup RIP for a fault whose RIP is inside a uaccess copy region,
// or 0 if the fault did not happen inside one (a genuine kernel bug -> fatal).
static uint64_t uaccess_fixup_lookup(uint64_t rip) {
    for (unsigned i = 0; i < sizeof(uaccess_ex_table) / sizeof(uaccess_ex_table[0]); i++) {
        if (rip >= uaccess_ex_table[i].start && rip < uaccess_ex_table[i].end) {
            return uaccess_ex_table[i].fixup;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// #CR2FRAME witness + deterministic self-test
// ---------------------------------------------------------------------------
//
// THE WITNESS IS ALWAYS ON, INCLUDING IN THE GOLDEN, AND IT COSTS ONE CR2 READ
// ON THE FAULT PATH. The failure it watches for is a scheduling race: it
// appeared 5 times in 702 app launches and each occurrence killed a healthy
// process with a fault address belonging to somebody else. A defect at that
// rate is invisible to any A/B you can afford to run, so the binary has to
// count it for you. When the frame's captured CR2 and a read taken here
// disagree, this frame WAS preempted or migrated between the fault and the
// handler, and before the capture moved into the stub that disagreement was
// silently the value the kernel acted on.
//
// Rate-limited to one line, then a running count, because a busy machine can
// produce these in bursts and this runs before the fault is resolved.
static volatile uint64_t g_cr2race_n;

static void cr2race_witness(const interrupt_frame_t *frame, uint64_t captured) {
    uint64_t now = read_cr2();
    if (now == captured) return;
    uint64_t n = __atomic_add_fetch(&g_cr2race_n, 1, __ATOMIC_RELAXED);
    // First one in full, then one line per 64 so a burst cannot flood the
    // console from inside an unresolved fault, but the TOTAL is always visible.
    if (n != 1 && (n & 63)) return;
    kprintf("[CR2RACE] n=%lu: the #PF frame was preempted or migrated before "
            "the handler ran. CR2 captured at entry = 0x%lx, CR2 read now = "
            "0x%lx, rip=0x%lx cs=0x%lx err=0x%lx. The captured value is the one "
            "this fault belongs to; before #CR2FRAME the kernel acted on the "
            "other one and killed the wrong process.\n",
            n, captured, now, frame->rip, frame->cs, frame->error_code);
}

#ifdef CR2RACETEST
// make CR2RACETEST=1 - NOT IN THE GOLDEN.
//
// DO NOT WAIT FOR A 1-IN-140 RACE; CONSTRUCT IT. CR2 is writable in Ring 0, so
// the exact damage the race does (CR2 no longer belongs to this fault) is one
// instruction, with no scheduler, no second core and no luck involved. Armed
// here, EVERY Ring-3 page fault on the machine gets a poisoned CR2.
//
//   make CR2RACETEST=1                  -> GREEN. The kernel uses frame->cr2,
//                                          every fault still resolves, the box
//                                          boots to the desktop and prints
//                                          [CR2RACE] because the witness sees
//                                          the poison.
//   make CR2RACETEST=1 CR2RACELEGACY=1  -> RED. The kernel reads CR2 here, as
//                                          it did before this change, and the
//                                          first userland demand fault kills
//                                          its process at CR2=0x7ffc2ace0000.
//                                          That is the reported symptom, on
//                                          demand, on the first boot.
//
// Same shape as PTROFAULTTEST / NOBLOCKTEST / SMAPTEST: one flag, two builds,
// a failure you can watch happening and watch not happening.
#define CR2RACE_POISON 0x00007FFC2ACE0000ULL
static void cr2race_poison(interrupt_frame_t *frame) {
    if ((frame->cs & 0x3) == 0) return;          // Ring 0 only would deadlock boot
    __asm__ volatile("mov %0, %%cr2" :: "r"(CR2RACE_POISON) : "memory");
#ifdef CR2RACELEGACY
    frame->cr2 = CR2RACE_POISON;                 // == the pre-#CR2FRAME read_cr2()
#endif
}
#endif

// ---------------------------------------------------------------------------
// The #PF handler proper (registered on IDT vector 14 by isr_init)
// ---------------------------------------------------------------------------

void page_fault_handler(interrupt_frame_t *frame) {
#ifdef CR2RACETEST
    cr2race_poison(frame);
#endif
    // #CR2FRAME: THE FAULT ADDRESS COMES FROM THE FRAME, NOT FROM CR2.
    //
    // #smpreval2 put a read_cr2() here and called it "the only point at which
    // CR2 still belongs to this fault". It is not, and that sentence is the
    // whole bug. This function is reached from isr_handler(), which calls
    // bkl_acquire() BEFORE dispatching; the contended wait runs with
    // interrupts ENABLED on purpose (cpu/smp.c bkl_take_locked) and
    // sched_rq_pop() steals across cores, so by the time this line runs the
    // frame may have been preempted, may have watched other threads on this
    // core take and RESOLVE their own faults, and may be executing on a
    // different core entirely. Each of those replaces CR2. Reading it here
    // resolves, reports and kills against another fault's address.
    //
    // cpu/idt.asm now pushes CR2 into the frame at entry, where nothing but
    // kernel-stack pushes has happened yet. See the comment there.
    uint64_t cr2 = frame->cr2;
    cr2race_witness(frame, cr2);
    uint64_t err = frame->error_code;
    process_t *p = proc_current();

    // 1. Try to resolve as a valid fault: COW write, demand-zero / lazy page,
    //    or file-backed page. mm_fault() also resolves a kernel-mode write to
    //    a user COW page (the copy_to_user case), so it runs regardless of ring.
    if (p && p->cr3 != 0 && mm_fault(p, cr2, err) == 0) {
        return;  // fixed up; IRET re-executes the faulting instruction
    }

    // 1b. #509 TOCTOU keystone. A kernel-mode fault that mm_fault() could NOT
    //     resolve, while executing inside a copy_*_user primitive, is the
    //     TOCTOU case (a racing thread unmapped the user buffer, or it was bad).
    //     Redirect to the primitive's fixup so copy_*_user returns -EFAULT
    //     instead of panicking. A kernel-mode fault NOT inside a uaccess region
    //     (uaccess_fixup_lookup == 0) is a genuine kernel bug and stays fatal.
    if ((frame->cs & 0x3) == 0) {
        uint64_t fixup = uaccess_fixup_lookup(frame->rip);
        if (fixup) {
            frame->rip = fixup;
            return;  // primitive returns "not copied" -> copy_*_user -> EFAULT
        }
    }

    // 2. Unrecoverable. A user fault becomes SIGSEGV.
    int from_user = (frame->cs & 0x3) != 0;

    // #dosmem: SAY WHY, BEFORE ANYTHING ELSE ON THIS PATH RUNS.
    //
    // Ordering is load-bearing and is the same lesson f6865391 paid for two
    // days ago. Below this point deliver_segv_handler() writes a signal frame
    // to the USER STACK and can take and RESOLVE a page fault of its own; that
    // nested fault calls mm_fault(), which would overwrite this core's reason
    // slot exactly as it overwrote CR2. Report first, then deliver.
    //
    // Only for a fault the kernel has already decided it cannot fix, so a
    // healthy machine pays nothing: every resolvable fault returned at step 1.
    if (p) mm_fault_report(p, cr2, err);

    // #procspawn: SAY WHAT THE ERROR CODE ALREADY SAYS, because the generic
    // reporter does not. A kernel-mode fault with P=1 and W=1 is a SUPERVISOR
    // WRITE TO A PRESENT, READ-ONLY PAGE. mm_fault_report() reached that exact
    // fault and printed "WHY=NO_VMA(not present and NO VMA covers this
    // address)", which is the opposite of what err=0x3 means and sends the
    // next reader after demand paging. This costs nothing on a healthy boot:
    // every resolvable fault returned at step 1 above.
    if (!from_user && (err & 0x1) && (err & 0x2)) {
        extern uint64_t g_kernel_cr3;   // mm/vmm.c, set once from the UEFI CR3
        kprintf("[#PF-RO] SUPERVISOR WRITE to a PRESENT page: the mapping is "
                "READ-ONLY, this is NOT a missing page. cr2=0x%lx rip=0x%lx "
                "identity-eff-flags=0x%lx firmware-write-protected=%d\n",
                cr2, frame->rip,
                vmm_get_effective_flags_in(g_kernel_cr3 ? g_kernel_cr3
                                                        : read_cr3(), cr2),
                vmm_identity_write_refused(cr2));
    }

    if (from_user && p) {
        if (deliver_segv_handler(p, frame, cr2) == 0) {
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


// ---------------------------------------------------------------------------
// #509 boot-time TOCTOU keystone self-test (deterministic, kernel context)
// ---------------------------------------------------------------------------
//
// Runs once from demand_init() (after isr_init() has registered this #PF
// handler). It fires several DELIBERATE page faults from inside the copy_*_user
// primitives against a canonical user-half address with NO mapping and NO VMA
// (0x700000000000, the same address the #500 SECTEST uses as "unmapped user").
//
// The entry gate (validate_user_copy_range) PERMITS a not-present page (it might
// be a lazy demand page), so the ONLY thing preventing a kernel panic here is
// the fault-fixup exception table above. If the fixup regresses, mm_fault()
// cannot resolve the address either, the fault reaches exception_fatal(), and
// the kernel PANICS at boot - a loud, immediate tripwire. That this routine
// prints its RESULT line and the boot continues IS the proof that the kernel
// survives a bad user page at copy time and returns -EFAULT to just the caller.
extern size_t __uaccess_copy(void *dst, const void *src, size_t n);

void uaccess_toctou_selftest(void) {
    const uint64_t BAD = 0x0000700000000000ULL;  // canonical user, no VMA, not present
    int pass = 0, total = 0;

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= VMM_ADDR_MASK;
    uint64_t eff = vmm_get_effective_flags_in(cr3, BAD);
    kprintf("[UACCESS-TOCTOU] BAD=0x%lx present=%d (entry gate ALLOWS not-present; "
            "only the fault-fixup can make a copy fail here)\n",
            BAD, (eff & VMM_FLAG_PRESENT) ? 1 : 0);

    // Positive: the copy engine itself works on good memory.
    char a[64], b[64];
    for (int i = 0; i < 64; i++) b[i] = (char)(i + 1);
    size_t nc = __uaccess_copy(a, b, 64);
    int eng = (nc == 0 && a[0] == 1 && a[63] == 64);
    total++; if (eng) pass++;
    kprintf("[UACCESS-TOCTOU] %s engine __uaccess_copy(good) notcopied=%lu\n",
            eng ? "PASS" : "FAIL", (unsigned long)nc);

    char kbuf[64];
    kbuf[0] = 0x5A;

    // T2: READ from a page that is bad AT COPY TIME -> EFAULT, kernel survives.
    int r2 = copy_from_user(kbuf, (void *)BAD, 64);
    total++; if (r2 == -14) pass++;
    kprintf("[UACCESS-TOCTOU] %s T2 copy_from_user(bad-read) ret=%d want=-14\n",
            r2 == -14 ? "PASS" : "FAIL", r2);

    // T3: WRITE to a bad page AT COPY TIME -> EFAULT, no mis-directed Ring-0
    // write, kernel survives. This is the dangerous arbitrary-write path.
    int r3 = copy_to_user((void *)BAD, kbuf, 64);
    total++; if (r3 == -14) pass++;
    kprintf("[UACCESS-TOCTOU] %s T3 copy_to_user(bad-write) ret=%d want=-14\n",
            r3 == -14 ? "PASS" : "FAIL", r3);

    // T4: clear_user to a bad page -> EFAULT.
    int r4 = clear_user((void *)BAD, 64);
    total++; if (r4 == -14) pass++;
    kprintf("[UACCESS-TOCTOU] %s T4 clear_user(bad) ret=%d want=-14\n",
            r4 == -14 ? "PASS" : "FAIL", r4);

    // T5: strncpy_from_user from a bad page -> EFAULT.
    ssize_t r5 = strncpy_from_user(kbuf, (const char *)BAD, 64);
    total++; if (r5 == -14) pass++;
    kprintf("[UACCESS-TOCTOU] %s T5 strncpy_from_user(bad) ret=%ld want=-14\n",
            r5 == -14 ? "PASS" : "FAIL", (long)r5);

    // T6: strnlen_user on a bad page -> EFAULT.
    ssize_t r6 = strnlen_user((const char *)BAD, 64);
    total++; if (r6 == -14) pass++;
    kprintf("[UACCESS-TOCTOU] %s T6 strnlen_user(bad) ret=%ld want=-14\n",
            r6 == -14 ? "PASS" : "FAIL", (long)r6);

    kprintf("[UACCESS-TOCTOU] kernel SURVIVED %d deliberate bad-page copies\n", total - 1);
    kprintf("[UACCESS-TOCTOU] RESULT: %d/%d PASS - %s\n", pass, total,
            pass == total ? "TOCTOU-SAFE" : "REGRESSION");
}
