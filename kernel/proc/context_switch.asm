; context_switch.asm - Low-level context switching for MayteraOS
; 64-bit x86_64 implementation

; #67 SMP: THE OWNERSHIP RELEASE. Both entry points take a FIFTH argument,
; R8 = volatile int32_t *old_release (NULL to skip). Each stores 0 there at the
; point the OUTGOING process's context is fully saved AND this core has left the
; outgoing stack, i.e. after fxsave64, after "mov [rdi], rsp" and after
; "mov rsp, rsi".
;
; WHY IT IS IN THE ASM AND NOT IN C. The scheduler used to publish the outgoing
; process on the ready queue BEFORE calling this code, and this code is what
; writes prev->rsp. Another core could pop it in that window and switch to an
; rsp left over from the PREVIOUS deschedule, putting two cores on one kernel
; stack: the "hand off a half-saved context" livelock recorded in cpu/smp.c.
; A C "finish switch" hook cannot close it, because context_start NEVER RETURNS
; to its caller (it IRETQs to Ring 3), so the only place that can observe "the
; save is complete" for both paths is inside these two routines.
;
; ORDERING IS FREE ON x86: stores are not reordered with other stores (TSO), so
; a core that observes old_release == 0 is guaranteed to also observe the final
; value of [rdi] and the completed fxsave64. No fence is needed or emitted.
;
; The store is placed AFTER "mov rsp, rsi" on purpose: from that instruction on,
; this core no longer has RSP pointing into the outgoing process's stack, so not
; even an NMI landing in the window can push a frame onto a stack another core
; has already resumed.

section .text
global context_switch
global context_start
global fpu_save_live

; #745 local 107: cpu/sse.c decides, per core, whether this machine saves FPU
; state with xsave64 (x87 + SSE + AVX) or fxsave64 (x87 + SSE only), and
; guarantees that AVX is DISABLED (CR4.OSXSAVE clear, so every VEX-encoded
; instruction #UDs) whenever it is the latter. Both are read RIP-relative.
extern g_fpu_use_xsave          ; uint8_t : 0 = fxsave64, 1 = xsave64
extern g_fpu_xcr0               ; uint64_t: the RFBM for xsave64/xrstor64

; #588/#446 FPU/SSE state model. A switch saves the full FPU/SSE state
; (xmm0-15 + MXCSR + x87 control/status/tag + st0-st7) with a single
; fxsave64/fxrstor64 pair (#588), replacing the older movdqu-only block which
; saved xmm0-15 but NOT MXCSR or x87.
;
; #745 local 107 EXTENDS that to AVX, and it is written in assembly for the
; same reason the rest of this file is: it is the switch primitive itself, and
; xsave64/xrstor64 take their state-component bitmap in EDX:EAX, which no C
; expression can spell without the compiler owning those registers at exactly
; the point the outgoing context is being saved.
;
; fxsave64 saves XMM0-15. It DOES NOT save the upper 128 bits of YMM0-15. On a
; CPU where AVX is enabled, an fxsave64-only switch therefore drops half of
; every 256-bit register, silently and non-deterministically, with no fault to
; trace. xsave64/xrstor64 with XCR0 = x87|SSE|AVX save all of it.
;
; The branch below is on a byte that is written once per core at boot and read
; on every switch, so it predicts perfectly; the alternative (patching the
; instruction stream at boot) buys nothing measurable and would put
; self-modifying code in the one routine this project has already
; double-faulted twice.
;
; SAFETY OF THE FALLBACK ARM: it is not "save less and hope". cpu/sse.c
; CLEARS CR4.OSXSAVE whenever g_fpu_use_xsave stays 0, which makes every AVX
; instruction #UD, so in that arm there is no YMM state in existence to lose.
;
; #446: the 512-byte FXSAVE image lives in a PER-PROCESS 16-byte-aligned
; buffer (process_t::fpu_area / thread_t::fpu_area) passed in as an argument,
; NOT carved off the kernel stack.
;
; #588 originally carved 528 bytes off the stack, aligned the base down with
; "and rsp,-16" (fxsave64/fxrstor64 #GP on a misaligned operand, and the
; kernel does not keep rsp reliably 16-aligned across a switch) and stashed a
; pointer to the saved RFLAGS at [base+512] so the restore side could recover
; the GPR frame across the resulting variable gap. That was fatal whenever the
; outgoing and the incoming proc's switch frames happened to sit on the SAME
; kernel stack a few hundred bytes apart: the outgoing "fxsave64 [rsp]" wrote
; its 512-byte image straight through the incoming proc's stashed pointer, so
; the following "mov rsp,[rsp+512]" loaded XMM bytes into RSP and the next
; instruction double-faulted. MEASURED on the shipping golden: RIP = the andq
; below, RSP = FXSAVE image data taken from an xmm slot, ~1 boot in 9 during
; the early-boot DHCP window.
;
; Passing the save areas in removes the carve, the alignment slack AND the
; stashed pointer in one go: the on-stack frame is a fixed 16 qwords (15 GPRs
; + RFLAGS) followed by the return address, which is exactly what the
; synthetic frame builders in proc/process.c write, and rsp alignment no
; longer matters to this code at all. It also gives back 528 bytes of kernel
; stack per switch (PROCESS_STACK_SIZE is only 16 KB).

; void context_switch(uint64_t *old_rsp, uint64_t new_rsp,
;                     void *old_fpu, void *new_fpu,
;                     volatile int32_t *old_release)
;   RDI = where to save the outgoing RSP (will point at its saved RFLAGS)
;   RSI = RSP to load (points at the incoming proc's saved RFLAGS)
;   RDX = 64-byte-aligned FPU_AREA_SIZE save area of the OUTGOING proc
;   RCX = 64-byte-aligned FPU_AREA_SIZE save area of the INCOMING proc
;   R8  = &outgoing->sched_on_cpu, cleared once the save is complete (#67);
;         NULL when there is no outgoing process_t (the dummy_rsp paths)
;   R9  = &incoming->sched_pinned, cleared once we have switched to it (#75);
;         NULL when the incoming task is not a queued process_t
context_switch:
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
    pushfq                  ; rsp now points at the saved RFLAGS

    ; RDX/RCX still hold their argument values here: push writes memory, it
    ; does not modify the pushed register.
    ;
    ; Save the outgoing FPU/SSE(/AVX) state OFF the stack. RAX/R10 are
    ; clobbered freely: they were pushed above, and the matching pops at the
    ; bottom read the INCOMING task's stack, so nothing here can be observed.
    ; RDX is dead after this block (the rest of the routine uses only RDI,
    ; RSI, RCX, R8, R9), which is why xsave64's EDX:EAX bitmap may take it.
    cmp byte [rel g_fpu_use_xsave], 0
    je  .cs_fxsave
    mov r10, rdx            ; r10 = outgoing save area
    mov rax, [rel g_fpu_xcr0]
    mov rdx, rax
    shr rdx, 32             ; EDX:EAX = RFBM
    xsave64 [r10]
    jmp .cs_saved
.cs_fxsave:
    fxsave64 [rdx]
.cs_saved:

    ; Save the outgoing stack pointer, load the incoming one.
    ; RDX/RCX/R8 still hold their argument values (push writes memory only).
    mov [rdi], rsp
    mov rsp, rsi

    ; #67: the outgoing context is now complete AND we have left its stack.
    ; Release it so any core may run it. See the note at the top of this file.
    test r8, r8
    jz .cs_no_release
    mov dword [r8], 0
.cs_no_release:

    ; #75: and release the INCOMING task's selection pin. R9 = &next->sched_pinned.
    ; The two stores are now genuinely symmetric, which is what the C comment
    ; used to claim while the code did not do it:
    ;   R8 (sched_on_cpu) protects the OUTGOING task until its context is SAVED;
    ;   R9 (sched_pinned) protects the INCOMING task until we have SWITCHED to it.
    ; This must live here rather than in C after the call, because context_start
    ; never returns to its caller - a C release could only ever run on one of the
    ; two paths, and this ticket has already measured what "wrong on one path"
    ; costs.
    test r9, r9
    jz .cs_no_unpin
    mov dword [r9], 0
.cs_no_unpin:

    ; Restore the incoming FPU/SSE(/AVX) state. RCX still holds the argument
    ; our caller passed; RAX/RDX are restored by the pops below.
    cmp byte [rel g_fpu_use_xsave], 0
    je  .cs_fxrstor
    mov rax, [rel g_fpu_xcr0]
    mov rdx, rax
    shr rdx, 32             ; EDX:EAX = RFBM
    xrstor64 [rcx]
    jmp .cs_restored
.cs_fxrstor:
    fxrstor64 [rcx]
.cs_restored:

    ; Clear TF (bit 8) before restoring RFLAGS to avoid a spurious INT 1.
    and qword [rsp], 0xFFFFFFFFFFFFFEFF
    popfq

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
    ret

; void context_start(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3,
;                    void *old_fpu, volatile int32_t *old_release)
;   RDI = where to save the outgoing RSP
;   RSI = new stack pointer holding the initial IRET context
;   RDX = CR3 for the user address space
;   RCX = 64-byte-aligned FPU_AREA_SIZE save area of the OUTGOING proc
;   R8  = &outgoing->sched_on_cpu, cleared once the save is complete (#67);
;         NULL when there is no outgoing process_t. Safe to store AFTER the CR3
;         load: it points into the kernel's low-half .bss, which stays mapped in
;         every user address space - the very next instructions pop the incoming
;         IRET frame off a kmalloc'd kernel stack in that same low half.
context_start:
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
    pushfq                  ; rsp = saved RFLAGS of the OLD process

    ; Save the OLD process FPU/SSE state into its own area (same shape the
    ; context_switch restore side expects). The NEW process needs no restore
    ; here: it enters Ring 3 fresh via IRETQ. Must happen BEFORE the CR3 load.
    ; Unlike context_switch, RDX is LIVE here: it is the incoming CR3, needed
    ; by the "mov cr3, rdx" below. xsave64 wants EDX:EAX, so park CR3 in R11
    ; across the save and put it back. R10/R11/RAX were all pushed above and
    ; are never read again on this path (context_start never returns; the pops
    ; at the bottom unwind the INCOMING IRET frame).
    cmp byte [rel g_fpu_use_xsave], 0
    je  .cst_fxsave
    mov r10, rcx            ; r10 = outgoing save area
    mov r11, rdx            ; r11 = CR3, parked
    mov rax, [rel g_fpu_xcr0]
    mov rdx, rax
    shr rdx, 32             ; EDX:EAX = RFBM
    xsave64 [r10]
    mov rdx, r11            ; CR3 back into RDX for the load below
    jmp .cst_saved
.cst_fxsave:
    fxsave64 [rcx]
.cst_saved:

    ; Save the outgoing RSP (before the CR3 switch).
    mov [rdi], rsp

    mov cr3, rdx
    clts                    ; allow FPU/SSE in the new address space

    ; Load the new stack (IRET frame) and enter Ring 3
    mov rsp, rsi

    ; #67: outgoing context complete and its stack abandoned; release it.
    test r8, r8
    jz .cst_no_release
    mov dword [r8], 0
.cst_no_release:

    ; #75: release the INCOMING task's selection pin. See the note in
    ; context_switch. R9 = &next->sched_pinned, NULL to skip.
    test r9, r9
    jz .cst_no_unpin
    mov dword [r9], 0
.cst_no_unpin:

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
    iretq

; ============================================================================
; #110: fork()/clone() must give the child the PARENT'S FPU state, not a fresh
; one. The parent's live FPU/SSE(/AVX) registers are NOT in its
; process_t::fpu_area at that moment: that field only holds whatever was saved
; the last time the parent was switched OUT, which is stale by an arbitrary
; amount of user execution. The state that has to be copied is the one sitting
; in the physical registers right now, so the child's area is filled by a LIVE
; save taken in the forking parent's own syscall context.
;
; This is in assembly for the same reason the switch above is: xsave64 takes
; its state-component bitmap in EDX:EAX, which no C expression can spell, and
; keeping the fxsave64-vs-xsave64 selection in ONE file means fork can never
; drift from what the switch does (that drift is exactly what #110 is: the old
; code seeded a default image while the switch was already saving AVX).
;
; void fpu_save_live(void *area)
;   RDI = FPU_AREA_ALIGN-aligned, FPU_AREA_SIZE-byte buffer.
;
; THE CALLER MUST ZERO `area` FIRST. xsave64 writes
; XSTATE_BV = (old XSTATE_BV & ~RFBM) | (XINUSE & RFBM), so any stale bit
; outside the RFBM survives into the image and would make the matching
; xrstor64 #GP. proc/process.c's fpu_capture_live() does the memset.
;
; RAX and RDX are caller-saved in the SysV ABI, so clobbering them for the
; bitmap needs no save/restore.
fpu_save_live:
    cmp byte [rel g_fpu_use_xsave], 0
    je  .fsl_fxsave
    mov rax, [rel g_fpu_xcr0]
    mov rdx, rax
    shr rdx, 32             ; EDX:EAX = RFBM
    xsave64 [rdi]
    ret
.fsl_fxsave:
    fxsave64 [rdi]
    ret
