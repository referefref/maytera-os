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

; Phase D2 (DELETED from the shipping path, #SMPGLOBALS 2026-08-30):
; g_syscall_saved_frame was ONE global written by every syscall on every core,
; which sys_rt_sigreturn() then read to locate the IRET frame it rewrites.
; Every task has its OWN ring-0 stack, so that address is per-task; the global
; therefore named whichever task finished a syscall most recently, anywhere in
; the system. rt_sigreturn then rewrote THAT task's saved registers, RIP and
; user RSP. One core plus a context switch is enough to hit it; four cores hit
; it with no switch at all.
;
; The frame is now published per-task into process_t::syscall_frame by
; syscall_check_return_work() (proc/signal.c), which the call below already
; hands the frame pointer to. That costs no store here at all.
;
; The global survives ONLY under -dSIGFRAME_DIFF (make SIGFRAMEDIFF=1), where
; it is the RED arm / differential reference that proves the above is real
; rather than argued. It is absent from a normal build: check with
;   nm kernel.dbg.elf | grep g_syscall_saved_frame
%ifdef SIGFRAME_DIFF
global g_syscall_saved_frame
g_syscall_saved_frame: dq 0
%endif

; #75 instrument: number of times syscall_entry found gs:0 (this core's ring-0
; stack) null and ran the syscall on the USER stack instead. Reported by
; [SCHEDCORE]. Expected to be 0; any non-zero value is a defect, not a warning.
global g_syscall_kstack_null
g_syscall_kstack_null: dq 0

; #smpfix (#75) CONTROL GATE for the SYSRET-tail interrupt mask below.
; 1 = mask (the fix, and the default), 0 = leave the window open (the control
; arm). Cleared by main.c when /NOSYSRETCLI.TXT is present on the FAT ESP, so
; both arms of the proof are the SAME kernel.elf and differ by one file.
global g_sysret_cli
g_sysret_cli: dd 1

; #smpfix (#75) THE WINDOW WIDENER, and why a reproducer needs one.
;
; The hazard below is ONE INSTRUCTION wide, so at natural timings it is entered
; perhaps once every few minutes: the panic that opened this ticket took 141
; seconds and three 300-second boots afterwards did not reproduce it. A
; detector that has only ever been observed SILENT is not evidence of anything
; (this tree has shipped a test flag nested in an unrelated conditional, a
; truncation detector structurally unable to fire, and a per-core meter that
; read 0% in exactly the configuration it existed for). So the window is held
; OPEN on demand, the same inversion proc/schedrace.h already documents: widen
; the race until it happens every run, rather than narrowing it and hoping.
;
; 1 = insert a fixed run of PAUSE instructions between `pop rsp` and `sysret`.
; Set by main.c when /SYSRETWIDEN.TXT is present on the FAT ESP.
global g_sysret_widen
g_sysret_widen: db 0

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

    ; Fallback to user stack if kernel stack is null.
    ;
    ; #75 2026-08-29: COUNT IT. This branch runs the whole kernel side of a
    ; syscall on the USER stack, and the next context_switch then stores that
    ; user rsp into process_t::rsp - which is exactly the shape of the open
    ; corruption ('COMPOSIT' priv=3, rsp=0xbffee738, outside its kernel stack).
    ; gs:0 is per-cpu and is only written by cpu_set_kernel_stack(), which
    ; sched_schedule() calls when it switches TO a user process, so a core that
    ; has never done that has gs:0 = 0. Whether that can happen is a question
    ; nobody could answer, because nothing counted it. Now something does.
    test rsp, rsp
    jnz .has_kernel_stack
    inc qword [rel g_syscall_kstack_null]
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

    ; #SMPGLOBALS: the saved-frame pointer is published PER TASK by
    ; syscall_check_return_work() below, which already receives rsp in rdi.
    ; The old global store lived here; it survives only in the differential
    ; build, as the reference the fix is measured against.
%ifdef SIGFRAME_DIFF
    mov [rel g_syscall_saved_frame], rsp
%endif

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

    ; =====================================================================
    ; #smpfix (#75): MASK INTERRUPTS BEFORE RSP POINTS AT THE USER STACK.
    ;
    ; The tail below ends `pop rsp` / `o64 sysret`. Between those two
    ; instructions the CPU is at CPL 0 with RSP pointing into the USER stack,
    ; and IF is 1 (the `sti` before syscall_dispatch, which bkl_release
    ; preserves). An interrupt delivered at that boundary is taken at CPL 0,
    ; so the CPU does NOT switch to TSS.RSP0 - no vector except #DF has an IST
    ; entry (cpu/idt.c sets idt[num].ist = 0 for everything else) - and it
    ; pushes its frame onto the USER stack. isr_handler then runs there, and
    ; sched_tick() -> sched_schedule() -> context_switch() stores that USER
    ; rsp into process_t::rsp.
    ;
    ; That is exactly the open #75 signature: a Ring-3 task whose SAVED KERNEL
    ; rsp is an address inside its own user stack, with g_syscall_kstack_null
    ; still 0 because this path never reads gs:0.
    ;
    ;   [SCHEDRACE] reason 1: rsp outside the incoming task's kernel stack
    ;   incoming: 'COMPOSIT' pid=26 rsp=0xbffee748 stack=[0x112cb4d0,0x112db4d0)
    ;   USERRSP  rsp=0xbffee748 user_rsp=0xbffeffa0 delta=6232
    ;
    ; It is a ONE-INSTRUCTION window per syscall return, which is why it takes
    ; minutes to hit and why it survived every single-core campaign: with one
    ; core the same core later resumes from that user rsp and the frames are
    ; still there, so it usually just works. It is not benign even then -
    ; kernel return addresses and saved registers are written to a page the
    ; process itself can write - but it is silent.
    ;
    ; NO INTERRUPT IS LOST. SYSRET loads RFLAGS from R11, the saved user
    ; RFLAGS, which has IF set, so anything pending is delivered immediately
    ; after SYSRET - in Ring 3, where the CPU DOES switch to TSS.RSP0 and the
    ; frame lands on the kernel stack, as intended. The mask covers about
    ; twenty instructions with no lock, no wait and no memory allocation.
    ;
    ; The gate is read HERE rather than next to `pop rsp` because RAX is dead
    ; at this point (it is reloaded by `pop rax` below) and every register is
    ; live once the pops begin.
    ; =====================================================================
    mov eax, [rel g_sysret_cli]
    test eax, eax
    jz .skip_sysret_cli
    cli
.skip_sysret_cli:

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

    ; #smpfix (#75): HOLD THE WINDOW OPEN. Reached only with
    ; /SYSRETWIDEN.TXT present, and inert otherwise (one compare against an
    ; already-hot byte).
    ;
    ; NOTHING HERE MAY TOUCH A GPR OR MEMORY. Every general register now holds
    ; user state - RCX is the return RIP and R11 the return RFLAGS, both read
    ; by SYSRET itself - and RSP points at the user stack, so a push would
    ; corrupt the program we are returning to. `cmp` against a memory byte
    ; writes only FLAGS, and FLAGS are dead here because SYSRET loads RFLAGS
    ; from R11. `pause` touches nothing at all. That is the whole reason the
    ; widener is an unrolled run of PAUSE and not a counted loop.
    cmp byte [rel g_sysret_widen], 0
    je .sysret_now
%rep 400
    pause
%endrep
.sysret_now:

    o64 sysret
