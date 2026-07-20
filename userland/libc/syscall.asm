; syscall.asm - System call stubs for MayteraOS user space
; x86-64 syscall calling convention:
;   Syscall number in RAX
;   Arguments: RDI, RSI, RDX, R10, R8, R9 (note: R10 instead of RCX)
;   Return value in RAX
;   RCX and R11 are clobbered by syscall instruction

section .text

; long syscall0(long num)
global syscall0
syscall0:
    mov     rax, rdi        ; syscall number
    syscall
    ret

; long syscall1(long num, long arg1)
global syscall1
syscall1:
    mov     rax, rdi        ; syscall number
    mov     rdi, rsi        ; arg1
    syscall
    ret

; long syscall2(long num, long arg1, long arg2)
global syscall2
syscall2:
    mov     rax, rdi        ; syscall number
    mov     rdi, rsi        ; arg1
    mov     rsi, rdx        ; arg2
    syscall
    ret

; long syscall3(long num, long arg1, long arg2, long arg3)
global syscall3
syscall3:
    mov     rax, rdi        ; syscall number
    mov     rdi, rsi        ; arg1
    mov     rsi, rdx        ; arg2
    mov     rdx, rcx        ; arg3
    syscall
    ret

; long syscall4(long num, long arg1, long arg2, long arg3, long arg4)
global syscall4
syscall4:
    mov     rax, rdi        ; syscall number
    mov     rdi, rsi        ; arg1
    mov     rsi, rdx        ; arg2
    mov     rdx, rcx        ; arg3
    mov     r10, r8         ; arg4
    syscall
    ret

; long syscall5(long num, long arg1, long arg2, long arg3, long arg4, long arg5)
global syscall5
syscall5:
    mov     rax, rdi        ; syscall number
    mov     rdi, rsi        ; arg1
    mov     rsi, rdx        ; arg2
    mov     rdx, rcx        ; arg3
    mov     r10, r8         ; arg4
    mov     r8, r9          ; arg5
    syscall
    ret

; long syscall6(long num, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6)
global syscall6
syscall6:
    mov     rax, rdi        ; syscall number
    mov     rdi, rsi        ; arg1
    mov     rsi, rdx        ; arg2
    mov     rdx, rcx        ; arg3
    mov     r10, r8         ; arg4
    mov     r8, r9          ; arg5
    mov     r9, [rsp+8]     ; arg6 (on stack)
    syscall
    ret
