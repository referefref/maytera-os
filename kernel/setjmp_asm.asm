; setjmp_asm.asm - setjmp/longjmp implementation for x86_64
; Non-local jumps for MayteraOS kernel

section .text

; int setjmp(jmp_buf env)
; Save callee-saved registers and return 0
; rdi = pointer to jmp_buf
global setjmp
setjmp:
    ; Save callee-saved registers
    mov [rdi + 0],  rbx     ; rbx
    mov [rdi + 8],  rbp     ; rbp
    mov [rdi + 16], r12     ; r12
    mov [rdi + 24], r13     ; r13
    mov [rdi + 32], r14     ; r14
    mov [rdi + 40], r15     ; r15

    ; Save stack pointer (after return, so +8 for return address)
    lea rax, [rsp + 8]
    mov [rdi + 48], rax     ; rsp

    ; Save return address (instruction after call)
    mov rax, [rsp]
    mov [rdi + 56], rax     ; rip

    ; Return 0
    xor eax, eax
    ret

; void longjmp(jmp_buf env, int val) __attribute__((noreturn))
; Restore callee-saved registers and jump to saved rip
; rdi = pointer to jmp_buf
; esi = return value (if 0, becomes 1)
global longjmp
longjmp:
    ; Get return value (if 0, make it 1)
    mov eax, esi
    test eax, eax
    jnz .nonzero
    mov eax, 1
.nonzero:

    ; Restore callee-saved registers
    mov rbx, [rdi + 0]      ; rbx
    mov rbp, [rdi + 8]      ; rbp
    mov r12, [rdi + 16]     ; r12
    mov r13, [rdi + 24]     ; r13
    mov r14, [rdi + 32]     ; r14
    mov r15, [rdi + 40]     ; r15

    ; Restore stack pointer
    mov rsp, [rdi + 48]     ; rsp

    ; Jump to saved instruction pointer
    jmp [rdi + 56]          ; rip
