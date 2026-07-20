; setjmp.asm - Non-local jump implementation for x86_64
; Implements setjmp/longjmp for exception handling support

[BITS 64]

section .text

; jmp_buf structure offsets (matches setjmp.h)
%define JB_RBX  0
%define JB_RBP  8
%define JB_R12  16
%define JB_R13  24
%define JB_R14  32
%define JB_R15  40
%define JB_RSP  48
%define JB_RIP  56

; int setjmp(jmp_buf env)
; RDI = pointer to jmp_buf
; Returns 0 on first call, non-zero on longjmp return
global setjmp
setjmp:
    ; Save callee-saved registers
    mov [rdi + JB_RBX], rbx
    mov [rdi + JB_RBP], rbp
    mov [rdi + JB_R12], r12
    mov [rdi + JB_R13], r13
    mov [rdi + JB_R14], r14
    mov [rdi + JB_R15], r15

    ; Save stack pointer (after return address was pushed)
    lea rax, [rsp + 8]      ; RSP value before call instruction
    mov [rdi + JB_RSP], rax

    ; Save return address
    mov rax, [rsp]          ; Return address on stack
    mov [rdi + JB_RIP], rax

    ; Return 0 on first call
    xor eax, eax
    ret

; void longjmp(jmp_buf env, int val)
; RDI = pointer to jmp_buf
; RSI = return value (if 0, becomes 1)
; Never returns - jumps to setjmp location
global longjmp
longjmp:
    ; Prepare return value (if val==0, return 1)
    mov eax, esi
    test eax, eax
    jnz .val_ok
    inc eax                 ; 0 -> 1
.val_ok:

    ; Restore callee-saved registers
    mov rbx, [rdi + JB_RBX]
    mov rbp, [rdi + JB_RBP]
    mov r12, [rdi + JB_R12]
    mov r13, [rdi + JB_R13]
    mov r14, [rdi + JB_R14]
    mov r15, [rdi + JB_R15]

    ; Restore stack pointer
    mov rsp, [rdi + JB_RSP]

    ; Jump to saved return address
    jmp [rdi + JB_RIP]
