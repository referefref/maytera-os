; gdt.asm - GDT loading routine for x86_64
; void gdt_load(gdt_ptr_t *gdt_ptr, uint16_t code_seg, uint16_t data_seg)

[BITS 64]

section .note.GNU-stack noalloc noexec nowrite progbits

section .text

global gdt_load

; gdt_load - Load the Global Descriptor Table
; Arguments (System V AMD64 ABI):
;   RDI = pointer to GDT descriptor (gdt_ptr_t)
;   RSI = kernel code segment selector (0x08)
;   RDX = kernel data segment selector (0x10)
gdt_load:
    ; Load the GDT
    lgdt [rdi]

    ; Update data segment registers
    mov ax, dx          ; Data segment selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; To reload CS, we need to do a far return
    ; Push the new CS and return address, then do retfq
    pop rax             ; Get return address
    push rsi            ; Push new code segment selector
    push rax            ; Push return address
    retfq               ; Far return (pops CS:RIP)
