; entry.asm - 64-bit kernel entry point for MayteraOS
; This is the first code executed when the bootloader jumps to the kernel.
; The bootloader passes a pointer to boot_info in RDI.

[BITS 64]

; Mark stack as non-executable (security)
section .note.GNU-stack noalloc noexec nowrite progbits

section .text

; Export entry point
global _start
global kernel_stack_top

; Import kernel main
extern kernel_main

; Kernel entry point
_start:
    ; Clear direction flag
    cld

    ; Disable interrupts until we're ready
    cli

    ; Save boot_info pointer (passed in RDI by UEFI)
    mov rbp, rdi

    ; Set up kernel stack
    lea rsp, [kernel_stack_top]

    ; Clear all general purpose registers (except RDI for boot_info)
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    ; Keep RDI with boot_info pointer
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    ; Zero BSS section before entering C code
    ; __bss_start and __bss_end are defined in linker.ld
    extern __bss_start
    extern __bss_end
    
    lea rdi, [__bss_start]
    lea rcx, [__bss_end]
    sub rcx, rdi
    shr rcx, 3              ; divide by 8 (zero 8 bytes at a time)
    xor rax, rax
    cld
    rep stosq

    ; Restore boot_info pointer to RDI (first argument for System V AMD64 ABI)
    mov rdi, rbp

    ; Call kernel main function
    ; void kernel_main(boot_info_t *boot_info)
    call kernel_main

    ; If kernel_main returns (it shouldn't), halt the CPU
.halt:
    cli
    hlt
    jmp .halt

section .bss
    align 16
    ; Kernel stack (64 KB)
    kernel_stack_bottom:
        resb 65536
    kernel_stack_top:

; Multiboot header (optional, for GRUB compatibility)
section .multiboot
    align 8
    MULTIBOOT2_MAGIC    equ 0xE85250D6
    MULTIBOOT2_ARCH     equ 0          ; Protected mode i386
    MULTIBOOT2_LENGTH   equ multiboot_end - multiboot_start
    MULTIBOOT2_CHECKSUM equ -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_LENGTH)

    multiboot_start:
        dd MULTIBOOT2_MAGIC
        dd MULTIBOOT2_ARCH
        dd MULTIBOOT2_LENGTH
        dd MULTIBOOT2_CHECKSUM
        ; End tag
        dw 0    ; type
        dw 0    ; flags
        dd 8    ; size
    multiboot_end:
