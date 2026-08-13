; SPDX-License-Identifier: MIT
; Copyright (c) MayteraOS contributors.
; Full license text: userland/libc/LICENSE (MIT License).
;
; sig_trampoline.asm - signal trampoline for MayteraOS userland
; When the kernel delivers a signal, it rewrites the saved user IRET frame:
;   rip = user handler, rdi = signo, rsp = sigframe + trampoline-return
; The user handler executes, does `ret`, pops the trampoline address, and
; lands here. We invoke sys_rt_sigreturn (163) which restores the saved
; frame (from the sigframe at rsp) and returns to pre-signal user state.

bits 64

section .text

global __sig_trampoline
__sig_trampoline:
    mov rax, 83         ; SYS_SIGRETURN (#430: was 163, which collides with
                        ; SYS_MSG_RECV in the kernel dispatcher)
    syscall
    ; rt_sigreturn should never return. If it does, crash loudly.
    ud2

; #640 leg 4: mark the stack NON-EXECUTABLE. ld marks PT_GNU_STACK RWE for
; the WHOLE link if ANY input object lacks this note, so every hand-written
; NASM object needs it, not just crt0. gcc emits it automatically for C.
section .note.GNU-stack noalloc noexec nowrite progbits
