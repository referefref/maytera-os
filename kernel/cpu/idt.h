// idt.h - Interrupt Descriptor Table for x86_64
#ifndef IDT_H
#define IDT_H

#include "../types.h"

// Number of IDT entries
#define IDT_ENTRIES 256

// IDT gate types
#define IDT_GATE_INTERRUPT  0x8E  // P=1, DPL=0, Type=interrupt gate (0xE)
#define IDT_GATE_TRAP       0x8F  // P=1, DPL=0, Type=trap gate (0xF)
#define IDT_GATE_USER       0xEE  // P=1, DPL=3, Type=interrupt gate

// Exception vectors
#define EXCEPTION_DE    0   // Divide Error
#define EXCEPTION_DB    1   // Debug
#define EXCEPTION_NMI   2   // Non-Maskable Interrupt
#define EXCEPTION_BP    3   // Breakpoint
#define EXCEPTION_OF    4   // Overflow
#define EXCEPTION_BR    5   // Bound Range Exceeded
#define EXCEPTION_UD    6   // Invalid Opcode
#define EXCEPTION_NM    7   // Device Not Available
#define EXCEPTION_DF    8   // Double Fault
#define EXCEPTION_TS    10  // Invalid TSS
#define EXCEPTION_NP    11  // Segment Not Present
#define EXCEPTION_SS    12  // Stack-Segment Fault
#define EXCEPTION_GP    13  // General Protection Fault
#define EXCEPTION_PF    14  // Page Fault
#define EXCEPTION_MF    16  // x87 FPU Error
#define EXCEPTION_AC    17  // Alignment Check
#define EXCEPTION_MC    18  // Machine Check
#define EXCEPTION_XM    19  // SIMD Exception
#define EXCEPTION_VE    20  // Virtualization Exception

// IRQ vectors (remapped from 32)
#define IRQ_BASE        32
#define IRQ_TIMER       (IRQ_BASE + 0)
#define IRQ_KEYBOARD    (IRQ_BASE + 1)
#define IRQ_CASCADE     (IRQ_BASE + 2)
#define IRQ_COM2        (IRQ_BASE + 3)
#define IRQ_COM1        (IRQ_BASE + 4)
#define IRQ_LPT2        (IRQ_BASE + 5)
#define IRQ_FLOPPY      (IRQ_BASE + 6)
#define IRQ_LPT1        (IRQ_BASE + 7)
#define IRQ_CMOS        (IRQ_BASE + 8)
#define IRQ_FREE1       (IRQ_BASE + 9)
#define IRQ_FREE2       (IRQ_BASE + 10)
#define IRQ_FREE3       (IRQ_BASE + 11)
#define IRQ_MOUSE       (IRQ_BASE + 12)
#define IRQ_FPU         (IRQ_BASE + 13)
#define IRQ_ATA_PRI     (IRQ_BASE + 14)
#define IRQ_ATA_SEC     (IRQ_BASE + 15)

// Syscall vector
#define SYSCALL_VECTOR  0x80

// IDT entry structure (16 bytes for 64-bit)
typedef struct {
    uint16_t offset_low;    // Offset bits 0-15
    uint16_t selector;      // Code segment selector
    uint8_t  ist;           // Interrupt Stack Table (0 = not used)
    uint8_t  type_attr;     // Gate type and attributes
    uint16_t offset_mid;    // Offset bits 16-31
    uint32_t offset_high;   // Offset bits 32-63
    uint32_t reserved;      // Must be zero
} __attribute__((packed)) idt_entry_t;

// IDT pointer structure (for LIDT instruction)
typedef struct {
    uint16_t limit;         // Size of IDT - 1
    uint64_t base;          // Linear address of IDT
} __attribute__((packed)) idt_ptr_t;

// Interrupt frame pushed by CPU
typedef struct {
    // Pushed by our interrupt stub
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;

    // Interrupt number and error code
    uint64_t int_no;
    uint64_t error_code;

    // Pushed by CPU
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;

// Interrupt handler type
typedef void (*interrupt_handler_t)(interrupt_frame_t *frame);

// Initialize IDT
void idt_init(void);
void idt_load_ap(void);

// Set an IDT entry
void idt_set_gate(int num, uint64_t handler, uint16_t selector, uint8_t type_attr);

// Register an interrupt handler
void idt_register_handler(int num, interrupt_handler_t handler);

// #325 Device Manager: report whether IDT vector vec is populated.
// Returns 1 if the gate present bit is set. type_attr/has_handler optional.
int idt_get_vector_info(int vec, uint8_t *type_attr, int *has_handler);

// #429: shared fatal-exception tail. Records a panic log and, for a user-mode
// fault, shows the crash dialog and terminates the faulting process (kernel
// survives); for a kernel-mode fault it panic-halts. Used by isr_handler_impl
// for every CPU exception and by the #PF handler (mm/fault.c) as the fallback
// when a fault is not a valid demand-paging / COW fault. Does not return.
void exception_fatal(interrupt_frame_t *frame);

// External assembly functions
extern void idt_load(idt_ptr_t *idt_ptr);

// Exception handler stubs (from idt.asm)
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

// IRQ handler stubs
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

// Syscall handler stub
extern void isr128(void);

#endif // IDT_H
