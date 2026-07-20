// idt.c - Interrupt Descriptor Table implementation for x86_64
#include "idt.h"
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

    // #71: Intel HDA MSI vector (0x50 / 80, HDA_MSI_VECTOR in drivers/hda.h).
    // MSI targets the Local APIC directly, so like the SMP wake IPI this needs
    // its own gate outside the legacy 32-47 IRQ range. The handler is
    // registered (or not, if the device has no MSI capability) at runtime by
    // hda_setup_interrupt() once the Local APIC is up.
    extern void irq_hda_msi(void);
    idt_set_gate(0x50, (uint64_t)irq_hda_msi, GDT_KERNEL_CODE, IDT_GATE_INTERRUPT);

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
void isr_handler(interrupt_frame_t *frame) {  // #279 3b-3C BKL wrapper
    extern int g_smp_bkl_full; extern void bkl_acquire(void); extern void bkl_release(void);
    if (g_smp_bkl_full) { bkl_acquire(); isr_handler_impl(frame); bkl_release(); }
    else { isr_handler_impl(frame); }
}
static void isr_handler_impl(interrupt_frame_t *frame) {
    uint64_t int_no = frame->int_no;

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
        kprintf("\n[EXCEPTION] %s (INT %lu)\n", name, int_no);
        kprintf("  Error code: 0x%lx\n", frame->error_code);
        kprintf("  RIP: 0x%lx  CS: 0x%lx\n", frame->rip, frame->cs);
        kprintf("  RSP: 0x%lx  SS: 0x%lx\n", frame->rsp, frame->ss);
        kprintf("  RFLAGS: 0x%lx\n", frame->rflags);
        kprintf("  RAX: 0x%lx  RBX: 0x%lx\n", frame->rax, frame->rbx);
        kprintf("  RCX: 0x%lx  RDX: 0x%lx\n", frame->rcx, frame->rdx);
        kprintf("  RSI: 0x%lx  RDI: 0x%lx\n", frame->rsi, frame->rdi);
        kprintf("  RBP: 0x%lx\n", frame->rbp);
        kprintf("  R8:  0x%lx  R9:  0x%lx\n", frame->r8, frame->r9);
        kprintf("  R10: 0x%lx  R11: 0x%lx\n", frame->r10, frame->r11);

        // Dump user-mode stack for any user-mode fault. The values near RSP are
        // saved return addresses; a corrupted one reveals a stack smash.
        if ((frame->cs & 0x3) != 0) {
            kprintf("  Stack dump (from RSP=0x%lx):\n", frame->rsp);
            uint64_t *sp = (uint64_t *)frame->rsp;
            for (int i = 0; i < 24; i++) {
                uint64_t addr = (uint64_t)&sp[i];
                if (addr >= 0x1000 && addr < 0x800000000000ULL) {
                    kprintf("    [RSP+0x%x] = 0x%lx\n", i * 8, sp[i]);
                }
            }
        }

        // Page fault has special handling
        if (int_no == EXCEPTION_PF) {
            uint64_t cr2 = read_cr2();
            kprintf("  CR2 (fault address): 0x%lx\n", cr2);
            kprintf("  Error bits: %s%s%s%s\n",
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
            kprintf("[KERNEL PANIC] %s at RIP=0x%lx\n", name, frame->rip);
            uint64_t cr2_val = 0;
            if (int_no == 14) {
                cr2_val = read_cr2();
                kprintf("[KERNEL PANIC] CR2=0x%lx err=0x%lx\n", cr2_val, frame->error_code);
            }
            kprintf("[KERNEL PANIC] RSP=0x%lx  Halting CPU.\n", frame->rsp);
            // #418: kernel-mode faults previously only reached kprintf()
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
