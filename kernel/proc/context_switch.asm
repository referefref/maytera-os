; context_switch.asm - Low-level context switching for MayteraOS
; 64-bit x86_64 implementation

section .text
global context_switch
global context_start

; void context_switch(uint64_t *old_rsp, uint64_t new_rsp)
;
; Saves current context to old stack, loads new context from new stack
; Parameters:
;   RDI = pointer to save current RSP
;   RSI = new RSP value to load
;
; Context layout on stack (pushed in this order):
;   R15, R14, R13, R12, R11, R10, R9, R8
;   RBP, RDI, RSI, RDX, RCX, RBX, RAX
;   RIP (return address), CS, RFLAGS, RSP, SS

context_switch:
    ; Save callee-saved registers
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Save additional registers that might be modified
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    ; Save RFLAGS
    pushfq

    ; Save SSE registers (xmm0-xmm15).
    ; TTF font rendering uses SSE via -msse/-msse2 compiler flags.
    ; Without saving these, context switches corrupt floating-point
    ; state between processes.
    sub rsp, 256            ; 16 regs * 16 bytes each
    movdqu [rsp+0],   xmm0
    movdqu [rsp+16],  xmm1
    movdqu [rsp+32],  xmm2
    movdqu [rsp+48],  xmm3
    movdqu [rsp+64],  xmm4
    movdqu [rsp+80],  xmm5
    movdqu [rsp+96],  xmm6
    movdqu [rsp+112], xmm7
    movdqu [rsp+128], xmm8
    movdqu [rsp+144], xmm9
    movdqu [rsp+160], xmm10
    movdqu [rsp+176], xmm11
    movdqu [rsp+192], xmm12
    movdqu [rsp+208], xmm13
    movdqu [rsp+224], xmm14
    movdqu [rsp+240], xmm15

    ; Save current stack pointer to [old_rsp]
    ; RDI contains pointer to old process's RSP storage
    mov [rdi], rsp

    ; Load new stack pointer
    ; RSI contains new RSP value
    mov rsp, rsi

    ; Restore SSE registers
    movdqu xmm0,  [rsp+0]
    movdqu xmm1,  [rsp+16]
    movdqu xmm2,  [rsp+32]
    movdqu xmm3,  [rsp+48]
    movdqu xmm4,  [rsp+64]
    movdqu xmm5,  [rsp+80]
    movdqu xmm6,  [rsp+96]
    movdqu xmm7,  [rsp+112]
    movdqu xmm8,  [rsp+128]
    movdqu xmm9,  [rsp+144]
    movdqu xmm10, [rsp+160]
    movdqu xmm11, [rsp+176]
    movdqu xmm12, [rsp+192]
    movdqu xmm13, [rsp+208]
    movdqu xmm14, [rsp+224]
    movdqu xmm15, [rsp+240]
    add rsp, 256

    ; Clear TF (trap flag bit 8) before restoring RFLAGS
    ; Prevents spurious INT 1 debug exceptions when NetHack or other apps
    ; were interrupted while TF was set in the kernel
    and qword [rsp], 0xFFFFFFFFFFFFFEFF
    ; Restore RFLAGS
    popfq

    ; Restore registers in reverse order
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Return to new process
    ; The RIP for the new process was pushed when it called context_switch
    ret

; void context_start(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3)
;
; Save current process state, then start a new user process via IRETQ.
; The old process can later be resumed via context_switch, which will
; ret back to the caller of context_start.
; Parameters:
;   RDI = pointer to save current RSP (old process)
;   RSI = new stack pointer with initial IRET context
;   RDX = CR3 (page table base) for user address space
;
; Stack layout expected at new_rsp (from proc_create_user):
;   15 GPRs (all zero)
;   RIP (user entry point)
;   CS (user code segment)
;   RFLAGS
;   RSP (user stack pointer)
;   SS (user data segment)

context_start:
    ; Save old process context (same layout as context_switch)
    ; so context_switch can restore it later with ret
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    pushfq
    sub rsp, 256
    movdqu [rsp+0],   xmm0
    movdqu [rsp+16],  xmm1
    movdqu [rsp+32],  xmm2
    movdqu [rsp+48],  xmm3
    movdqu [rsp+64],  xmm4
    movdqu [rsp+80],  xmm5
    movdqu [rsp+96],  xmm6
    movdqu [rsp+112], xmm7
    movdqu [rsp+128], xmm8
    movdqu [rsp+144], xmm9
    movdqu [rsp+160], xmm10
    movdqu [rsp+176], xmm11
    movdqu [rsp+192], xmm12
    movdqu [rsp+208], xmm13
    movdqu [rsp+224], xmm14
    movdqu [rsp+240], xmm15

    ; Save current RSP to old process (before CR3 switch)
    mov [rdi], rsp

    ; Load user address space CR3
    mov cr3, rdx
    clts                ; Clear Task Switched flag to allow FPU/SSE

    ; Load the new stack (IRET frame)
    mov rsp, rsi

    ; Pop the 15 general purpose registers (all zeros for new process)
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    ; Now stack contains IRET frame: RIP, CS, RFLAGS, RSP, SS
    ; Execute IRET to transition from Ring 0 (kernel) to Ring 3 (user)
    iretq
