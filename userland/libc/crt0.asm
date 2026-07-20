; crt0.asm - C runtime startup for MayteraOS user space
; This is the entry point for all user-mode programs
; Sets up the C environment and calls main()

section .text

extern main
extern _exit
extern _init_heap

global _start
_start:
    ; Clear frame pointer for clean stack traces
    xor     rbp, rbp

    ; On entry, stack layout (set up by kernel):
    ; RSP+0: argc (or 0 if not set)
    ; RSP+8: argv[0] (or 0)
    ; RSP+16: argv[1] (or 0)
    ; ... (null terminated)
    ; Then: envp[] (null terminated)

    ; For now, we use simple argc=0, argv=NULL convention
    ; The kernel sets up RSP pointing to a 16-byte aligned stack

    ; Get argc from stack (or default to 0)
    pop     rdi             ; argc
    test    rdi, rdi        ; if argc is 0
    jz      .no_args

    ; argc > 0, RSP now points to argv[]
    mov     rsi, rsp        ; argv = current stack pointer
    jmp     .call_main

.no_args:
    ; No arguments: argc=0, argv=NULL
    xor     rdi, rdi        ; argc = 0
    xor     rsi, rsi        ; argv = NULL

.call_main:
    ; Initialize heap (if malloc is used)
    ; call    _init_heap     ; Uncomment when malloc is implemented

    ; Align stack to 16 bytes before call
    and     rsp, ~0xF

    ; Call main(argc, argv)
    ; rdi = argc (already set)
    ; rsi = argv (already set)
    call    main

    ; main() returned, exit with return value
    ; rax contains main's return value
    mov     rdi, rax        ; exit status = main's return value
    call    _exit

    ; Should never reach here, but just in case
.hang:
    hlt
    jmp     .hang
