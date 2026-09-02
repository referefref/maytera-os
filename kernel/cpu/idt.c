// idt.c - Interrupt Descriptor Table implementation for x86_64
#include "idt.h"
#include "../fs/bootlog.h"   // #134: bootlog_fault_write (owning header, NOT a private extern)
#include "gdt.h"
#include "../serial.h"
#include "../gui/crashhandler.h"
#include "../string.h"
#include "../fs/panic.h"

// IDT and pointer
static idt_entry_t idt[IDT_ENTRIES] __attribute__((aligned(16)));
static idt_ptr_t idt_ptr;

// Registered interrupt handlers
static interrupt_handler_t handlers[IDT_ENTRIES];

// Set an IDT entry
void idt_set_gate(int num, uint64_t handler, uint16_t selector, uint8_t type_attr) {
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].selector    = selector;
    idt[num].ist         = 0;  // Don't use IST by default
    idt[num].type_attr   = type_attr;
    idt[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].reserved    = 0;
}

// Register an interrupt handler
void idt_register_handler(int num, interrupt_handler_t handler) {
    if (num < IDT_ENTRIES) {
        handlers[num] = handler;
    }
}

// Initialize IDT
void idt_load_ap(void) {
    // Load the already-initialized global IDT on an application processor.
    idt_load(&idt_ptr);
}

void idt_init(void) {
    kprintf("[IDT] Initializing Interrupt Descriptor Table...\n");

    // Clear IDT and handlers
    memset(idt, 0, sizeof(idt));
    memset(handlers, 0, sizeof(handlers));

    // Set up exception handlers (0-31)
    idt_set_gate(0, (uint64_t)isr0, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(1, (uint64_t)isr1, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(2, (uint64_t)isr2, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(3, (uint64_t)isr3, GDT_KERNEL_CODE, IDT_GATE_TRAP);  // Breakpoint
    idt_set_gate(4, (uint64_t)isr4, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(5, (uint64_t)isr5, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(6, (uint64_t)isr6, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(7, (uint64_t)isr7, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(8, (uint64_t)isr8, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(9, (uint64_t)isr9, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(10, (uint64_t)isr10, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(11, (uint64_t)isr11, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(12, (uint64_t)isr12, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(13, (uint64_t)isr13, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(14, (uint64_t)isr14, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(15, (uint64_t)isr15, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(16, (uint64_t)isr16, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(17, (uint64_t)isr17, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(18, (uint64_t)isr18, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(19, (uint64_t)isr19, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(20, (uint64_t)isr20, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(21, (uint64_t)isr21, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(22, (uint64_t)isr22, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(23, (uint64_t)isr23, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(24, (uint64_t)isr24, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(25, (uint64_t)isr25, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(26, (uint64_t)isr26, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(27, (uint64_t)isr27, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(28, (uint64_t)isr28, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(29, (uint64_t)isr29, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(30, (uint64_t)isr30, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(31, (uint64_t)isr31, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);

    // Set up IRQ handlers (32-47)
    idt_set_gate(32, (uint64_t)irq0, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(33, (uint64_t)irq1, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(34, (uint64_t)irq2, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(35, (uint64_t)irq3, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(36, (uint64_t)irq4, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(37, (uint64_t)irq5, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(38, (uint64_t)irq6, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(39, (uint64_t)irq7, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(40, (uint64_t)irq8, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(41, (uint64_t)irq9, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(42, (uint64_t)irq10, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(43, (uint64_t)irq11, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(44, (uint64_t)irq12, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(45, (uint64_t)irq13, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(46, (uint64_t)irq14, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    idt_set_gate(47, (uint64_t)irq15, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);

    // System call handler (INT 0x80)
    idt_set_gate(128, (uint64_t)isr128, GDT_KERNEL_CODE, IDT_GATE_USER);

    // #279 SMP: AP wake-IPI vector (240). Lets idle APs HLT and be kicked awake.
    extern void irq_smp_wake(void);
    idt_set_gate(240, (uint64_t)irq_smp_wake, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);
    // #75: IPI_VECTOR_STOP (0xF3). Needed so a panic can actually stop the
    // other cores; without the gate the broadcast raises #GP on each AP.
    extern void irq_smp_stop(void);
    idt_set_gate(0xF3, (uint64_t)irq_smp_stop, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);    // #404: IPI_VECTOR_TLB (0xF2). Needed so a cross-CPU TLB shootdown can
    // actually be received. Before this, cpu/smp.c broadcast this vector from
    // smp_tlb_shootdown() (which had zero callers) into a not-present IDT
    // entry, which raises #GP on every AP that takes it. A vector needs BOTH a
    // gate here AND a stub in cpu/idt.asm, never one of the two: #75 measured
    // that registering only a C handler stopped 0 of 3 cores.
    //
    // Note irq_smp_tlb does NOT route through isr_handler and therefore does
    // not need idt_register_handler() at all. See the comment on the stub in
    // cpu/idt.asm for why it must not take the BKL.
    extern void irq_smp_tlb(void);
    idt_set_gate(0xF2, (uint64_t)irq_smp_tlb, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);

    // #smpfix (#67/#168): BKL_WAKE_VECTOR (0xF4). Sent by bkl_release() and
    // bkl_release_all() to a core parked in HLT waiting for the lock. 0xF4
    // rather than the declared-but-dead IPI_VECTOR_CALL (0xF1), so a vector
    // with no sender stays a vector with no sender; see the audit comment in
    // cpu/smp.c. Priority class 15, so it cannot be blocked on the target by a
    // lower-priority in-service vector - which matters, because this is the
    // PRIMARY wake arm and the per-core tick is only the backstop. Like
    // irq_smp_tlb the stub does NOT route through isr_handler, and must not.
    extern void irq_bkl_wake(void);
    idt_set_gate(0xF4, (uint64_t)irq_bkl_wake, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);


    // #71: Intel HDA MSI vector (0x50 / 80, HDA_MSI_VECTOR in drivers/hda.h).
    // MSI targets the Local APIC directly, so like the SMP wake IPI this needs
    // its own gate outside the legacy 32-47 IRQ range. The handler is
    // registered (or not, if the device has no MSI capability) at runtime by
    // hda_setup_interrupt() once the Local APIC is up.
    extern void irq_hda_msi(void);
    idt_set_gate(0x50, (uint64_t)irq_hda_msi, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);

    // #139: xHCI MSI vector (0x51 / 81, XHCI_MSI_VECTOR in drivers/xhci.h).
    // Until #139 this kernel had NO xHCI interrupt at all: the event ring was
    // drained only by timer-driven workers, so every USB completion (a HID
    // report included) waited for the next poll pass. The C handler is
    // registered at runtime by xhci_setup_interrupt(), or not at all if the
    // controller turns out to have no MSI capability.
    extern void irq_xhci_msi(void);
    idt_set_gate(0x51, (uint64_t)irq_xhci_msi, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);

    // #745 (#62): gate for the REDUNDANT tick source (Local APIC timer,
    // vector 0x41). cpu/isr.c registers the C handler; without THIS the gate
    // is absent and the first redundant tick raises #GP (error code 0x20b =
    // vector 0x41, IDT, external).
    extern void irq_tick_redundant(void);
    idt_set_gate(0x41, (uint64_t)irq_tick_redundant, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);

    // #169: gate for the PER-CORE AP PREEMPTION TICK (Local APIC timer,
    // vector 0x42). Installed unconditionally on the ONE IDT the whole machine
    // shares (idt_load_ap() loads this same table on every AP), so by the time
    // an AP arms its timer the gate is already in place and arming is a single
    // register write with no window. The C handler is registered in cpu/isr.c;
    // doing only one half builds clean and #GPs with error code 0x213 on the
    // first interrupt (0x42 << 3 | 2 | 1) - see the 0x41 note above, which is
    // the same mistake, measured.
    extern void irq_ap_tick(void);
    idt_set_gate(0x42, (uint64_t)irq_ap_tick, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);

    // Use IST1 for Double Fault (vector 8) to prevent stack overflow
    idt[8].ist = 1;

    // Set up IDT pointer
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;

    // Load IDT
    kprintf("[IDT] Loading IDT at 0x%lx (limit %u)\n", idt_ptr.base, idt_ptr.limit);
    idt_load(&idt_ptr);

    kprintf("[IDT] IDT initialized with %d entries\n", IDT_ENTRIES);
}

// Exception names for debugging
static const char *exception_names[] = {
    "Divide Error",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 FPU Error",
    "Alignment Check",
    "Machine Check",
    "SIMD Exception",
    "Virtualization Exception",
};

// Common interrupt handler (called from assembly)
static void isr_handler_impl(interrupt_frame_t *frame);
// #161: isr_handler_impl() plus the asynchronous signal delivery point that
// follows it. Defined below; see isr_async_sig_check() for the full writeup.
static void isr_handler_body(interrupt_frame_t *frame);
// ===========================================================================
// #smpfix (#75): AN INTERRUPT TAKEN AT CPL 0 ON A USER STACK.
// ===========================================================================
//
// In 64-bit mode the CPU pushes SS:RSP for EVERY interrupt, same-privilege
// ones included, so frame->rsp is the interrupted stack pointer and this test
// costs one compare. A CPL-0 interrupt does not switch stacks (only #DF has an
// IST entry here), so if the interrupted RSP is a user address the handler is
// running on the USER stack, and any context switch taken from it saves a user
// rsp into process_t::rsp - the open reason-1 corruption.
//
// This counts the WINDOW BEING ENTERED, not just the rare case where a switch
// follows, so it is a far higher-rate signal than the panic and can be
// compared between arms in one boot. Physical == virtual in this kernel and
// the PMM is capped at 2 GB, so every kernel address is below 0x80000000 and
// every user address is at or above it (user text loads at 2 GB, user stacks
// sit just under 3 GB).
#define SMPFIX_USER_BASE 0x80000000ull
volatile uint64_t g_irq_on_user_stack = 0;
volatile uint64_t g_irq_on_user_stack_rip = 0;
volatile uint64_t g_irq_on_user_stack_vec = 0;
volatile uint64_t g_irq_on_user_stack_rsp = 0;

static inline void smpfix_note_cpl0_user_stack(interrupt_frame_t *frame) {
    if ((frame->cs & 0x3) != 0) return;              // came from Ring 3: normal
    if (frame->rsp < SMPFIX_USER_BASE) return;       // on a kernel stack: normal
    // COUNT ONLY. This runs at the very top of isr_handler, BEFORE
    // bkl_acquire, on a core whose stack pointer is already wrong. A kprintf
    // from here takes the console lock from interrupt context on a corrupted
    // stack, and the first RED-arm run of this campaign hung dead at the first
    // occurrence with a truncated line on the wire. The evidence is not worth
    // a second failure mode: the counters are printed once a second by
    // sched_smp_report()'s [IRQUSTACK] line, from a context that can safely
    // print, and the last offending RIP is recorded here for addr2line.
    g_irq_on_user_stack++;
    g_irq_on_user_stack_rip = frame->rip;
    g_irq_on_user_stack_vec = frame->int_no;
    g_irq_on_user_stack_rsp = frame->rsp;
}

void isr_handler(interrupt_frame_t *frame) {  // #279 3b-3C BKL wrapper
    extern int g_smp_bkl_full; extern void bkl_acquire(void); extern void bkl_release(void);
    smpfix_note_cpl0_user_stack(frame);   // #smpfix (#75)
    // #67 pass 5: tag what this core is doing so a long BKL hold can be blamed
    // on a VECTOR rather than on this wrapper, which is the same address every
    // time. One per-cpu store; no shared cacheline.
    extern void bkl_set_reason(uint32_t r);
    // #118: execution-continuity witness. See cpu/smp.c bkl_irq_mark().
    extern void bkl_irq_mark(void);
    // #121: an interrupt-entry witness that does NOT depend on the BKL.
    // bkl_irq_mark() lives inside the g_smp_bkl_full branch below AND
    // returns early unless a hold is already in progress, so it cannot
    // answer "did this core service anything at all during that syscall".
    // That is the question #118 left open, so it gets its own counter.
    { extern void scp_irq_tick(void); scp_irq_tick(); }
    // #wakeipi (#67/#168): THE SECOND, ALWAYS-ARMED BKL WAKE ARM. It MUST be
    // here, above bkl_acquire(), and not inside any handler: the tick arm the
    // parking design assumed was starved precisely because bkl_acquire() runs
    // first and a contended core never reaches its handler body. See the long
    // comment on bkl_wake_rescue() in cpu/smp.c. In the healthy case this is
    // one load of a hot word and no ICR write.
    { extern void bkl_wake_rescue(void); bkl_wake_rescue(); }
    if (g_smp_bkl_full) {
        bkl_acquire();
        bkl_set_reason(0x0100u | (uint32_t)(frame->int_no & 0xFF));
        bkl_irq_mark();
        isr_handler_body(frame);
        bkl_release();
    }
    else { isr_handler_body(frame); }
}

// #161: THE ASYNCHRONOUS SIGKILL DELIVERY POINT.
//
// Signal delivery used to happen at exactly one place, syscall.asm's
// syscall_check_return_work() on the way out of a syscall. A Ring 3 process
// grinding through a render loop therefore never reached a delivery point, and
// a pending SIGKILL sat on it forever: Force Quit and Task Manager's Kill
// failed on exactly the busy apps a user most wants to kill (a game, a media
// player) while appearing to work on idle ones, because an idle app is blocked
// in a syscall that sig_raise() wakes.
//
// The fix is here rather than in the app or the caller because SIGKILL must not
// depend on the target cooperating. Every Ring 3 process is interrupted by the
// timer many times a second whatever it is doing, and THAT is a delivery point
// no process can decline.
//
// Restricted to signals that terminate outright with no handler
// (sig_async_terminate_pending(), which is SIGKILL and any other
// default-action-terminate signal the app has not caught). Handler delivery
// stays on the syscall path, because building a sigframe means rewriting a
// saved user frame and the interrupt frame's GPR order differs from the syscall
// frame's - one layout, one delivery site, no second frame builder to keep in
// step.
//
// SAFETY, and why proc_exit() from here is not new ground: this runs only when
// the interrupt came FROM Ring 3 ((cs & 3) == 3), so the victim was in
// userland, its kernel stack holds nothing but this interrupt frame, and it is
// not mid-syscall holding anything. exception_fatal() below already calls
// proc_exit(-1) from this same isr_handler_impl() context, with the BKL held,
// for every user-mode fault; sched_schedule() drops the BKL across the context
// switch (bkl_release_all()), so the abandoned hold is released exactly as it
// is for a faulting process. proc_exit() does not return.
static void isr_async_sig_check(interrupt_frame_t *frame) {
    if ((frame->cs & 0x3) != 0x3) return;   // returning to Ring 0: not a delivery point
    extern int sig_async_terminate_pending(void);
    int signo = sig_async_terminate_pending();
    if (signo <= 0) return;
    extern void proc_exit(int exit_code);
    proc_exit(128 + signo);   // conventional POSIX status, same as the syscall path
}

static void isr_handler_body(interrupt_frame_t *frame) {
    isr_handler_impl(frame);
    isr_async_sig_check(frame);
}
static void isr_handler_impl(interrupt_frame_t *frame) {
    uint64_t int_no = frame->int_no;

    // #75: NMI (vector 2) DURING A PANIC STOP-THE-WORLD. Only then; outside a
    // panic an NMI still falls through to the normal fatal path below, so this
    // does not hijack the vector. An NMI is used because it is the only
    // delivery that reaches a core running with RFLAGS.IF clear, which is where
    // a core spinning for the BKL inside an interrupt handler actually is.
    if (int_no == 2) {
        extern int  smp_panic_stopping(void);
        extern void smp_panic_stop_ack(void);
        if (smp_panic_stopping()) smp_panic_stop_ack();   // never returns
    }

    // Check if there's a registered handler
    if (handlers[int_no]) {
        handlers[int_no](frame);
        return;
    }

    // Handle exceptions: any CPU exception (0-31) goes to the shared fatal tail.
    if (int_no < 32) {
        exception_fatal(frame);
        return;
    }

    // Unhandled IRQ - just log it
    kprintf("[IRQ] Unhandled interrupt %lu\n", int_no);
}

// #429: shared fatal-exception tail (declared in idt.h). Extracted from
// isr_handler_impl so the page-fault handler (mm/fault.c) can fall back here
// for an unrecoverable fault. For a user-mode fault it records a panic log,
// shows the crash dialog (unless under the SMP BKL) and terminates the
// faulting process, leaving the kernel running; for a kernel-mode fault it
// panic-halts this CPU.
void exception_fatal(interrupt_frame_t *frame) {
    uint64_t int_no = frame->int_no;
    {
        const char *name = int_no < 21 ? exception_names[int_no] : "Unknown Exception";
// #130 (2026-08-14): THE PANIC PATH MUST NOT TAKE THE CONSOLE LOCK.
// kprintf() acquires g_console_lock (serial.c:830/836). A core that faults
// WHILE HOLDING that lock then enters this handler, calls kprintf(), and blocks
// on a lock it already owns - with interrupts off, so nothing can rescue it.
// The other cores pile up behind it and the machine hangs SILENTLY.
//
// MEASURED: a 4-vCPU boot wedged with ALL FOUR CPUs at spinlock.h:251 (the
// spin) with RDI = 0x210c8c0 = g_console_lock, desktop never reached, and NOT
// ONE line of panic output emitted.
//
// It is worse than a hang: it MASKS the original fault. Note the ordering below
// - the register dump runs BEFORE panic_log_write(), so a deadlock here also
// means /boot/PANIC.TXT is never written and the fault leaves no record at all.
// That is the signature the user hit on real hardware in #137 (whole OS frozen,
// clock stopped, empty panic file).
//
// kprintf_nolock() (serial.c:847) already exists for exactly this. Using the
// shared primitive rather than inventing another one, per the project rule.
// Interleaved output from two dying cores is an acceptable price; a silent hang
// is not.
        kprintf_nolock("\n[EXCEPTION] %s (INT %lu)\n", name, int_no);
// #134: AND IT MUST REACH THE PERSISTENT LOG, because the machine this matters
// on has no serial console. Everything above and below here is kprintf_nolock,
// i.e. serial only, so on the iMac14,4 a fault has produced NO RECORD AT ALL -
// blame.md records that exact cost for #153 (a launched app that spawned and
// instantly died, with /BOOTLOG.TXT missing precisely the lines needed).
//
// bootlog_write() MUST NOT be called here: it calls kprintf(), which takes
// g_console_lock, which is the deadlock 240dc9f fixed a few lines above, and
// its flush enters fat/ext2 -> blk_write -> usb_msc_transport from a context
// that must never park. bootlog_fault_write() is the fault-safe sibling: a
// stack-formatted line, a lock-free CAS reservation into a static ring,
// mirrored with kprintf_nolock, and NO filesystem. A later safe context (the
// next bootlog_write, or the 2 s heartbeat) flushes it. One line, every field
// an addr2line needs. See fs/bootlog.c for the full argument.
        bootlog_fault_write("[EXCEPTION] %s (INT %lu) %s err=0x%lx RIP=0x%lx "
                            "CS=0x%lx RSP=0x%lx RFLAGS=0x%lx CR2=0x%lx",
                            name, int_no,
                            ((frame->cs & 0x3) != 0) ? "USER" : "KERNEL",
                            frame->error_code, frame->rip, frame->cs,
                            frame->rsp, frame->rflags,
                            (int_no == EXCEPTION_PF) ? read_cr2() : 0UL);
        // ==================================================================
        // #COMPRESPAWN: A FAULTING RIP IS USELESS UNDER PIE+ASLR ON ITS OWN.
        //
        // Every userland binary is a static PIE and exec/elf.c randomises its
        // base within a 1 GB window at 2 MB granularity (#640 stage 3). The
        // line above prints RIP=0x8001ebe772; to turn that into a function you
        // must first know which of up to 512 ASLR slots this run got, and
        // nothing recorded it. MEASURED 2026-08-25 on the owner's VM <vmid>: the
        // slot had to be brute-forced by hand (the only value for which
        // RIP-base lands inside .text was slot 15, giving image+0xBE772 =
        // memcpy). That is not a thing anyone can do on a machine they cannot
        // attach a debugger to.
        //
        // Two lines fix it permanently:
        //   1. the image base and the RIP as an IMAGE OFFSET, which is exactly
        //      what `addr2line -e <binary> <offset>` wants;
        //   2. a conservative scan of the user stack for words that fall inside
        //      the image, printed as offsets. Frame pointers are omitted at -O2
        //      so an RBP walk is not reliable, but saved return addresses are
        //      still ON the stack, and a handful of candidates names the CALLER
        //      - which is the whole question when the fault is inside memcpy.
        //
        // Fault-safe by construction: every stack word is read through
        // vmm_get_physical_in() (so an unmapped page ENDS the scan instead of
        // faulting inside the fault handler) and through the PHYSICAL address
        // (so SMAP and the user mapping are irrelevant). Bounded at 96 words.
        // ==================================================================
        if ((frame->cs & 0x3) != 0) {
            extern int proc_current_image(uint32_t *pid, uint64_t *base,
                                          uint64_t *end, uint64_t *cr3);
            extern uint64_t vmm_get_physical_in(uint64_t pml4_phys,
                                                uint64_t virt_addr);
            extern const char *proc_current_name(void);
            uint32_t fpid = 0; uint64_t ibase = 0, iend = 0, fcr3 = 0;
            int have = proc_current_image(&fpid, &ibase, &iend, &fcr3);
            if (have) {
                if (frame->rip >= ibase && frame->rip < iend) {
                    bootlog_fault_write("[FAULT] pid=%u '%s' image=[0x%lx,0x%lx) "
                                        "FAULTING RIP = image+0x%lx  (addr2line -e "
                                        "the binary 0x%lx)",
                                        fpid, proc_current_name(), ibase, iend,
                                        frame->rip - ibase, frame->rip - ibase);
                } else {
                    bootlog_fault_write("[FAULT] pid=%u '%s' image=[0x%lx,0x%lx) "
                                        "RIP 0x%lx is OUTSIDE its own image - a "
                                        "wild jump, not a bug at a source line",
                                        fpid, proc_current_name(), ibase, iend,
                                        frame->rip);
                }
                // Candidate return addresses, as image offsets, nearest first.
                if (fcr3) {
                    char cand[192];
                    int  used = 0, found = 0;
                    cand[0] = 0;
                    uint64_t sp = frame->rsp & ~7ULL;
                    for (int i = 0; i < 96 && found < 8; i++) {
                        uint64_t a  = sp + (uint64_t)i * 8;
                        uint64_t pp = vmm_get_physical_in(fcr3, a & ~0xFFFULL);
                        if (!pp) break;              // unmapped: stop, do not fault
                        uint64_t v  = *(volatile uint64_t *)(pp + (a & 0xFFF));
                        if (v <= ibase || v >= iend) continue;
                        // 12 chars max per entry ("+0x000abcde "), 192-byte buf
                        if (used > (int)sizeof(cand) - 16) break;
                        int n = snprintf(cand + used, sizeof(cand) - (size_t)used,
                                         "+0x%lx ", v - ibase);
                        if (n <= 0) break;
                        used += n; found++;
                    }
                    if (found) {
                        bootlog_fault_write("[FAULT] pid=%u user-stack candidates "
                                            "(image offsets, addr2line these): %s",
                                            fpid, cand);
                    } else {
                        bootlog_fault_write("[FAULT] pid=%u no in-image words found "
                                            "on the user stack near RSP", fpid);
                    }
                }
            }
        }
        kprintf_nolock("  Error code: 0x%lx\n", frame->error_code);
        kprintf_nolock("  RIP: 0x%lx  CS: 0x%lx\n", frame->rip, frame->cs);
        kprintf_nolock("  RSP: 0x%lx  SS: 0x%lx\n", frame->rsp, frame->ss);
        kprintf_nolock("  RFLAGS: 0x%lx\n", frame->rflags);
        kprintf_nolock("  RAX: 0x%lx  RBX: 0x%lx\n", frame->rax, frame->rbx);
        kprintf_nolock("  RCX: 0x%lx  RDX: 0x%lx\n", frame->rcx, frame->rdx);
        kprintf_nolock("  RSI: 0x%lx  RDI: 0x%lx\n", frame->rsi, frame->rdi);
        kprintf_nolock("  RBP: 0x%lx\n", frame->rbp);
        kprintf_nolock("  R8:  0x%lx  R9:  0x%lx\n", frame->r8, frame->r9);
        kprintf_nolock("  R10: 0x%lx  R11: 0x%lx\n", frame->r10, frame->r11);

        // Dump user-mode stack for any user-mode fault. The values near RSP are
        // saved return addresses; a corrupted one reveals a stack smash.
        if ((frame->cs & 0x3) != 0) {
            kprintf_nolock("  Stack dump (from RSP=0x%lx):\n", frame->rsp);
            uint64_t *sp = (uint64_t *)frame->rsp;
            for (int i = 0; i < 24; i++) {
                uint64_t addr = (uint64_t)&sp[i];
                if (addr >= 0x1000 && addr < 0x800000000000ULL) {
                    kprintf_nolock("    [RSP+0x%x] = 0x%lx\n", i * 8, sp[i]);
                }
            }
        }

        // Page fault has special handling
        if (int_no == EXCEPTION_PF) {
            uint64_t cr2 = read_cr2();
            kprintf_nolock("  CR2 (fault address): 0x%lx\n", cr2);
            kprintf_nolock("  Error bits: %s%s%s%s\n",
                    (frame->error_code & 1) ? "P " : "",
                    (frame->error_code & 2) ? "W " : "R ",
                    (frame->error_code & 4) ? "U " : "S ",
                    (frame->error_code & 16) ? "I " : "");
        }

        // Report crash to handler
        crash_regs_t regs = {
            .rax = frame->rax, .rbx = frame->rbx, .rcx = frame->rcx, .rdx = frame->rdx,
            .rsi = frame->rsi, .rdi = frame->rdi, .rbp = frame->rbp, .rsp = frame->rsp,
            .r8 = frame->r8, .r9 = frame->r9, .r10 = frame->r10, .r11 = frame->r11,
            .r12 = frame->r12, .r13 = frame->r13, .r14 = frame->r14, .r15 = frame->r15,
            .rip = frame->rip, .rflags = frame->rflags, .cs = frame->cs, .ss = frame->ss,
            .error_code = frame->error_code
        };
        
        // Get CR2 for page faults
        if (int_no == EXCEPTION_PF) {
            regs.cr2 = read_cr2();
        }
        
        // Map exception to crash type
        crash_type_t crash_type = CRASH_UNKNOWN;
        switch (int_no) {
            case 0:  crash_type = CRASH_DIVIDE_BY_ZERO; break;
            case 6:  crash_type = CRASH_INVALID_OPCODE; break;
            case 8:  crash_type = CRASH_DOUBLE_FAULT; break;
            case 12: crash_type = CRASH_STACK_FAULT; break;
            case 13: crash_type = CRASH_GENERAL_PROTECTION; break;
            case 14: crash_type = CRASH_PAGE_FAULT; break;
        }
        
        // Report to crash handler
        // Kernel-mode faults: halt to prevent triple fault.
        // The GUI crash dialog would re-enter corrupt kernel state.
        if ((frame->cs & 0x3) == 0) {
            kprintf_nolock("[KERNEL PANIC] %s at RIP=0x%lx\n", name, frame->rip);
            uint64_t cr2_val = 0;
            if (int_no == 14) {
                cr2_val = read_cr2();
                kprintf_nolock("[KERNEL PANIC] CR2=0x%lx err=0x%lx\n", cr2_val, frame->error_code);
            }
            kprintf_nolock("[KERNEL PANIC] RSP=0x%lx  Halting CPU.\n", frame->rsp);
            // ==============================================================
            // #167: A WILD RIP IS A JUMP, AND A JUMP HAS A SOURCE.
            //
            // "Invalid Opcode at RIP=0x45" is #75's original signature and it
            // has now been reported four separate times (the original report,
            // #165's arm-gateon/run05, and three times in ONE boot of #167's
            // SCHEDRACE + exit-churn reproducer) with nothing after it but the
            // register dump. 0x45 is not a code address; the core got there by
            // executing a `ret` off a stack slot that did not hold a return
            // address, or by an indirect branch through corrupted memory.
            //
            // Either way the STACK still holds the frames that led there, and
            // the ones that are real return addresses are exactly the ones that
            // land in kernel TEXT. Printing sixteen words with that one test
            // applied turns a wild RIP from a dead end into a list of
            // addresses to run through addr2line - which is what every previous
            // report of this fault needed and did not have.
            //
            // SAFETY. Read-only, no locks, no heap, no framebuffer, bounded at
            // 16 words, and it runs AFTER everything else has been printed, so
            // it can only ever add information to a machine that is already
            // halting. RSP is sanity-checked first: a fault whose RSP is itself
            // wild (#75 also reported RSP=0x10002) must not be dereferenced.
            // The bound is the kernel identity map, which is what every other
            // pointer check in this file uses.
            {
                extern char __text_start[], __text_end[];
                uint64_t lo = (uint64_t)__text_start, hi = (uint64_t)__text_end;
                uint64_t sp = frame->rsp;
                if (sp >= 0x100000ULL && sp < 0x80000000ULL && (sp & 7) == 0) {
                    kprintf_nolock("[KERNEL PANIC] stack walk from RSP "
                                   "(text=[0x%lx,0x%lx), * = a return address):\n",
                                   lo, hi);
                    const uint64_t *w = (const uint64_t *)sp;
                    for (int i = 0; i < 16; i++) {
                        uint64_t v = w[i];
                        kprintf_nolock("[KERNEL PANIC]   [rsp+0x%02x] = 0x%lx %s\n",
                                       i * 8, v,
                                       (v >= lo && v < hi) ? "*" : "");
                    }
                } else {
                    kprintf_nolock("[KERNEL PANIC] RSP 0x%lx is not a usable "
                                   "stack pointer; no walk.\n", sp);
                }
            }
            // #418: kernel-mode faults previously only reached kprintf_nolock()
            // (serial-only) - on the physical iMac (no serial cable) that is
            // a total loss of diagnosis. Write RIP/CR2/error-code/CR3/last
            // stage/version to /PANIC.TXT via a raw, unlocked, single-sector
            // overwrite BEFORE halting. No lock, no heap, no framebuffer -
            // safe even if the fault happened mid-FAT-operation (see
            // fs/panic.c for why this is safe to call unconditionally here).
            panic_log_write(frame->rip, cr2_val, frame->error_code,
                             read_cr3(), name, 0);
            // #480: reuse the ONE canonical terminal halt (kpanic_halt) instead
            // of a duplicate inline hlt loop. It disables interrupts and drops
            // the whole-kernel BKL first (#279: a dead CPU must not keep every
            // other CPU spinning for a lock it will never release). We keep the
            // detailed panic_log_write() ABOVE - which owns the full fault frame
            // (rip/cr2/error_code/cr3) - so /PANIC.TXT retains that richer
            // record; kpanic_halt() only halts, it does not re-write the record.
            kpanic_halt();
        }
        // #418: write the panic record for a USER-mode fault too, BEFORE
        // calling into crashhandler_report()/crashhandler_show_dialog() below
        // - those draw to the framebuffer and, prior to the #418 CR3 fix,
        // could themselves re-fault. Landing this on disk first means even a
        // worst-case double/triple fault inside the dialog still leaves a
        // readable, correctly-sized /PANIC.TXT from the ORIGINAL fault.
        panic_log_write(frame->rip,
                         (int_no == EXCEPTION_PF) ? read_cr2() : 0,
                         frame->error_code, read_cr3(), name, 1);
        // User-mode fault: show crash dialog, then kill the process
        crashhandler_report(crash_type, &regs, -1);
        // Patch in the process name since we passed app_id=-1
        {
            extern crash_info_t *crashhandler_get_last(void);
            extern const char *proc_current_name(void);
            crash_info_t *ci2 = crashhandler_get_last();
            const char *pname = proc_current_name();
            if (ci2 && pname && pname[0]) {
                ci2->app_name = pname;
            }
        }
        // Populate stack trace entries while user page tables are active.
        // After crashhandler_report(), g_current_crash points to the crash info.
        {
            extern crash_info_t *crashhandler_get_last(void);
            crash_info_t *ci = crashhandler_get_last();
            if (ci && frame->rsp >= 0x1000 && frame->rsp < 0x800000000000ULL) {
                volatile uint64_t *usp = (volatile uint64_t *)frame->rsp;
                int cnt = 0;
                for (int si = 0; si < 8; si++) {
                    uint64_t saddr = frame->rsp + (uint64_t)(si * 8);
                    if (saddr < 0x800000000000ULL) {
                        ci->stack_entries[si] = usp[si];
                        cnt++;
                    }
                }
                ci->stack_entry_count = cnt;
            }
        }
        // #279: the crash dialog is a BLOCKING modal (polls the PS/2 mouse with
        // preemption off) that holds the whole-kernel BKL until dismissed. Under
        // SMP that wedges the machine: the userland compositor (which owns input)
        // can never run to deliver the click because it needs the BKL this CPU
        // holds. So when the whole-kernel BKL is active, skip the modal and just
        // log + kill the faulting process (the crash is already recorded by
        // crashhandler_report and remains visible in the syslog).
        { extern int g_smp_bkl_full;
          if (!g_smp_bkl_full) crashhandler_show_dialog();
          else kprintf("[CrashHandler] SMP: skipping modal dialog (would hold BKL); killing process\n"); }
        // #NETDROP: hand the NIC back before we resume normal operation.
        //
        // crashhandler_report() above calls e1000_enter_crash_context() as its
        // FIRST action. That makes e1000_can_access_mmio() false, which disables
        // the ENTIRE driver: TX, RX, and the link-status register read. The
        // quiesce is right while we are walking a faulted address space, but it
        // was a ONE-WAY DOOR. e1000_exit_crash_context() had ZERO callers in the
        // whole tree, and this is the only path that ever enters the context, a
        // path that ALWAYS continues running (kernel-mode faults halt further
        // up, in the branch above, and never reach here).
        //
        // So any Ring-3 fault, i.e. one crashed application on an otherwise
        // healthy desktop, permanently killed networking for the rest of the
        // boot: nic_link_up() returned 0 forever, so the stack reported
        // NO-CARRIER on a NIC whose link was physically up, every send failed,
        // and the RX ring was never drained again. Owner-visible symptom was an
        // SSH session dying mid-use and the machine showing no connectivity,
        // with nothing in the log naming the NIC.
        //
        // A recoverable user fault must leave the NIC exactly as it found it.
        { extern void e1000_exit_crash_context(void);
          e1000_exit_crash_context(); }
        kprintf("[KERNEL] Killing crashed user process\n");
        // Terminate the faulting process. Returning would re-execute
        // the faulting instruction in an infinite crash loop.
        extern void proc_exit(int exit_code);
        proc_exit(-1);
        // proc_exit does not return (switches to next process)
    }
}

// #325 Device Manager: expose populated IDT vectors to userland.
int idt_get_vector_info(int vec, uint8_t *type_attr, int *has_handler) {
    if (vec < 0 || vec >= IDT_ENTRIES) return 0;
    if (type_attr) *type_attr = idt[vec].type_attr;
    if (has_handler) *has_handler = (handlers[vec] != 0) ? 1 : 0;
    return (idt[vec].type_attr & 0x80) ? 1 : 0;
}
