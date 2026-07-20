; syscall.asm - System call entry point for x86_64
; Build 109: Fixed arg6 passing correctly

[BITS 64]

section .note.GNU-stack noalloc noexec nowrite progbits

extern syscall_dispatch
extern tss
extern proc_current
extern return_work_handler
extern syscall_check_return_work
extern bkl_acquire
extern bkl_release
extern g_smp_bkl_full

section .data
align 16
user_stack_save:  dq 0
saved_syscall_num: dq 0

; Phase D2: pointer to the saved register frame at the top of the kernel
; syscall stack. Updated at the hook point so sys_rt_sigreturn can find
; and rewrite the IRET state. Single-CPU assumption holds for Maytera.
global g_syscall_saved_frame
g_syscall_saved_frame: dq 0

section .text

global syscall_entry
syscall_entry:
    ; Save user RSP and syscall number
    mov [gs:8], rsp            ; per-cpu user RSP scratch (#279)
    mov [gs:16], rax           ; per-cpu syscall-num scratch (#279)
    
    ; Load kernel SS and stack
    mov ax, 0x10
    mov ss, ax
    mov rsp, [gs:0]            ; per-cpu kernel stack (#279)
    
    ; Restore syscall number
    mov rax, [gs:16]

    ; Fallback to user stack if kernel stack is null
    test rsp, rsp
    jnz .has_kernel_stack
    mov rsp, [gs:8]
.has_kernel_stack:
    ; CRITICAL: interrupts MUST remain disabled here. The user RSP is held in
    ; the GLOBAL user_stack_save and is read below to build the IRET frame.
    ; If interrupts were enabled before that read, a timer preemption could
    ; switch to another process whose own syscall_entry overwrites the global;
    ; on resume this process would build its return frame with another task's
    ; RSP and then run on that task's user stack, corrupting it. Interrupts
    ; are re-enabled just before syscall_dispatch, after the frame is built.

    ; Build interrupt frame
    push qword 0x1B             ; SS
    push qword [gs:8] ; User RSP (per-cpu)
    push r11                    ; User RFLAGS
    push qword 0x23             ; CS
    push rcx                    ; User RIP

    ; Save all GPRs (15 registers)
    push rax                    ; [rsp + 14*8] syscall number
    push rbx                    ; [rsp + 13*8]
    push rcx                    ; [rsp + 12*8]
    push rdx                    ; [rsp + 11*8] arg3
    push rsi                    ; [rsp + 10*8] arg2
    push rdi                    ; [rsp + 9*8]  arg1
    push rbp                    ; [rsp + 8*8]
    push r8                     ; [rsp + 7*8]  arg5
    push r9                     ; [rsp + 6*8]  arg6 (COLOR!)
    push r10                    ; [rsp + 5*8]  arg4
    push r11                    ; [rsp + 4*8]
    push r12                    ; [rsp + 3*8]
    push r13                    ; [rsp + 2*8]
    push r14                    ; [rsp + 1*8]
    push r15                    ; [rsp + 0*8]

    ; #279 3b-2: take the Big Kernel Lock for the duration of the syscall, but
    ; ONLY when whole-kernel SMP locking is enabled (g_smp_bkl_full). With SMP
    ; off there is a single CPU and the lock would be pure overhead; gating it
    ; keeps the single-CPU syscall path identical to the pre-SMP kernel.
    ; rax is free here (dispatch arg regs are set up below).
    mov eax, [rel g_smp_bkl_full]
    test eax, eax
    jz .skip_bkl_acq
    call bkl_acquire
.skip_bkl_acq:

    ; Set up register arguments for syscall_dispatch(num, arg1, arg2, arg3, arg4, arg5, arg6)
    mov rdi, [rsp + 14*8]       ; num (from saved rax)
    mov rsi, [rsp + 9*8]        ; arg1 (from saved rdi)
    mov rdx, [rsp + 10*8]       ; arg2 (from saved rsi)
    mov rcx, [rsp + 11*8]       ; arg3 (from saved rdx)
    mov r8, [rsp + 5*8]         ; arg4 (from saved r10)
    mov r9, [rsp + 7*8]         ; arg5 (from saved r8)
    
    ; BUILD 109: Push padding FIRST, then arg6
    ; This way, after 'call' pushes return address:
    ;   [rsp + 0]  = return address
    ;   [rsp + 8]  = arg6 (7th C argument - where C expects it!)
    ;   [rsp + 16] = padding
    push qword 0                     ; Alignment padding first
    push qword [rsp + 6*8 + 8]       ; arg6 second (+8 for the push we just did)

    sti                               ; Re-enable interrupts for syscall handling

    call syscall_dispatch

    ; Label for fork child return (must match extern in process.c)
    global syscall_return_path
    syscall_return_path:

    ; Clean up stack (arg6 + padding = 16 bytes)
    add rsp, 16

    ; Store return value where rax was saved
    mov [rsp + 14*8], rax

    ; Phase D2: publish the saved-frame pointer so sys_rt_sigreturn can
    ; rewrite it. A subsequent nested syscall (from a handler) will
    ; overwrite this as it pushes its own frame; that is exactly what
    ; rt_sigreturn wants: it must rewrite ITS OWN frame, not the outer
    ; one (which is gone once the outer SYSRET has returned to user).
    mov [rel g_syscall_saved_frame], rsp

    ; Phase D1: return-work hook. Check current->return_work and, if set,
    ; invoke return_work_handler with a pointer to the saved frame. The
    ; handler may rewrite [rsp+15*8] (saved RIP) and [rsp+18*8] (saved
    ; user RSP) to redirect execution into a signal trampoline or a
    ; freshly exec'd image. rax (at [rsp+14*8]) is preserved.
    mov rdi, rsp
    call syscall_check_return_work

    ; #279 3b-2: release the Big Kernel Lock before returning to user (only if
    ; it was taken; see the gated acquire above). Caller-saved regs are free
    ; here; they are restored by the pops below.
    mov eax, [rel g_smp_bkl_full]
    test eax, eax
    jz .skip_bkl_rel
    call bkl_release
.skip_bkl_rel:

    ; Restore GPRs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax                     ; Return value

    ; Restore user state for SYSRET
    pop rcx                     ; User RIP
    add rsp, 8                  ; Skip CS
    pop r11                     ; User RFLAGS
    pop rsp                     ; User RSP

    o64 sysret
