; clone_asm.asm - #430 robust clone() trampoline for MayteraOS pthreads.
;
; The raw clone() contract (child resumes at the same RIP with a fresh stack)
; makes it impossible to rely on C stack locals in the child. This helper does
; it the glibc way: it stashes the entry function + arg on the CHILD stack
; before the syscall, and after the syscall the child pops them from its own
; stack and calls entry(arg) - no dependence on register/stack-spill codegen.
;
; long __clone_thread(unsigned int flags,      ; rdi
;                     void *child_stack_top,    ; rsi
;                     unsigned int *ptid,        ; rdx
;                     unsigned int *ctid,        ; rcx
;                     void (*entry)(void*),      ; r8
;                     void *arg);                ; r9
; Returns the new tid in the parent, and never returns in the child.
;
; Kernel SYS_CLONE arg mapping (see kernel proc/syscall.asm): rax=num,
; arg1=rdi, arg2=rsi, arg3=rdx, arg4=r10, arg5=r8, arg6=r9.

bits 64
section .note.GNU-stack noalloc noexec nowrite progbits
section .text

global __clone_thread
__clone_thread:
    ; Reserve two slots on the child stack and store entry + arg there.
    sub rsi, 16
    mov [rsi], r8            ; [child_top-16] = entry
    mov [rsi+8], r9          ; [child_top-8]  = arg

    ; Marshal the clone syscall arguments.
    mov r10, rcx             ; arg4 = ctid
    xor r8d, r8d             ; arg5 = tls = 0 (entry already saved on the stack)
    mov eax, 110             ; SYS_CLONE
    ; rdi=flags, rsi=child_stack (arg2, becomes the child's user RSP), rdx=ptid
    syscall

    test rax, rax
    jz .child                ; child returns 0
    ret                      ; parent: rax = new tid

.child:
    ; The kernel set our RSP to the child_stack value we passed (child_top-16).
    pop rax                  ; entry
    pop rdi                  ; arg
    call rax                 ; entry(arg) - runs the thread body (never returns)

    ; Safety net: if the thread body ever returns, exit cleanly.
    xor edi, edi
    xor eax, eax             ; SYS_EXIT
    syscall
.hang:
    hlt
    jmp .hang
