; idt.asm - IDT loading and interrupt stubs for x86_64

[BITS 64]

section .note.GNU-stack noalloc noexec nowrite progbits

section .text

; External C handler
extern isr_handler

; Export functions
global idt_load

; Export ISR stubs
global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
global isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
global isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
global isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

; Export IRQ stubs
global irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
global irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

; Export syscall stub
global isr128
global irq_smp_wake
global irq_hda_msi

; idt_load - Load the IDT
; Argument: RDI = pointer to IDT descriptor
idt_load:
    lidt [rdi]
    ret

; Macro for ISR without error code
%macro ISR_NOERRCODE 1
isr%1:
    push qword 0        ; Dummy error code
    push qword %1       ; Interrupt number
    jmp isr_common
%endmacro

; Macro for ISR with error code (pushed by CPU)
%macro ISR_ERRCODE 1
isr%1:
    push qword %1       ; Interrupt number
    jmp isr_common
%endmacro

; Macro for IRQ
%macro IRQ 2
irq%1:
    push qword 0        ; Dummy error code
    push qword %2       ; Interrupt number (32 + IRQ number)
    jmp isr_common
%endmacro

; CPU exceptions (0-31)
ISR_NOERRCODE 0     ; Divide Error
ISR_NOERRCODE 1     ; Debug
ISR_NOERRCODE 2     ; NMI
ISR_NOERRCODE 3     ; Breakpoint
ISR_NOERRCODE 4     ; Overflow
ISR_NOERRCODE 5     ; Bound Range Exceeded
ISR_NOERRCODE 6     ; Invalid Opcode
ISR_NOERRCODE 7     ; Device Not Available
ISR_ERRCODE   8     ; Double Fault (has error code)
ISR_NOERRCODE 9     ; Coprocessor Segment Overrun
ISR_ERRCODE   10    ; Invalid TSS (has error code)
ISR_ERRCODE   11    ; Segment Not Present (has error code)
ISR_ERRCODE   12    ; Stack-Segment Fault (has error code)
ISR_ERRCODE   13    ; General Protection Fault (has error code)
ISR_ERRCODE   14    ; Page Fault (has error code)
ISR_NOERRCODE 15    ; Reserved
ISR_NOERRCODE 16    ; x87 FPU Error
ISR_ERRCODE   17    ; Alignment Check (has error code)
ISR_NOERRCODE 18    ; Machine Check
ISR_NOERRCODE 19    ; SIMD Exception
ISR_NOERRCODE 20    ; Virtualization Exception
ISR_ERRCODE   21    ; Control Protection Exception
ISR_NOERRCODE 22    ; Reserved
ISR_NOERRCODE 23    ; Reserved
ISR_NOERRCODE 24    ; Reserved
ISR_NOERRCODE 25    ; Reserved
ISR_NOERRCODE 26    ; Reserved
ISR_NOERRCODE 27    ; Reserved
ISR_NOERRCODE 28    ; Hypervisor Injection Exception
ISR_ERRCODE   29    ; VMM Communication Exception
ISR_ERRCODE   30    ; Security Exception
ISR_NOERRCODE 31    ; Reserved

; Hardware IRQs (mapped to 32-47)
IRQ 0, 32           ; Timer
IRQ 1, 33           ; Keyboard
IRQ 2, 34           ; Cascade
IRQ 3, 35           ; COM2
IRQ 4, 36           ; COM1
IRQ 5, 37           ; LPT2
IRQ 6, 38           ; Floppy
IRQ 7, 39           ; LPT1 / Spurious
IRQ 8, 40           ; CMOS RTC
IRQ 9, 41           ; Free
IRQ 10, 42          ; Free
IRQ 11, 43          ; Free
IRQ 12, 44          ; Mouse
IRQ 13, 45          ; FPU
IRQ 14, 46          ; Primary ATA
IRQ 15, 47          ; Secondary ATA

; System call (INT 0x80)
isr128:
    push qword 0        ; Dummy error code
    push qword 128      ; Interrupt number
    jmp isr_common

; SMP AP wake IPI (vector 240) - kicks an idle HLT'd AP; handler just EOIs
irq_smp_wake:
    push qword 0        ; Dummy error code
    push qword 240      ; Interrupt number
    jmp isr_common

; #71: Intel HDA MSI vector (0x50 / 80). MSI is delivered straight to the
; Local APIC (bypasses the 8259 PIC / IOAPIC entirely), so this needs its own
; gate just like the SMP wake IPI above rather than one of the legacy IRQ0-15
; macros. See drivers/hda.c hda_setup_interrupt() / hda_msi_isr().
irq_hda_msi:
    push qword 0        ; Dummy error code
    push qword 0x50     ; Interrupt number
    jmp isr_common

; Common ISR handler
isr_common:
    ; Save all registers (in reverse order for stack layout)
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Pass stack pointer as argument (points to interrupt_frame_t)
    mov rdi, rsp

    ; Call C handler
    call isr_handler

    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    ; Remove interrupt number and error code from stack
    add rsp, 16

    ; Return from interrupt
    iretq
