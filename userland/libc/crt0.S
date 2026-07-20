# crt0.S - C runtime startup for MayteraOS userland programs
.section .text
.global _start
.global _exit

_start:
    # Clear frame pointer for clean stack traces
    xor %rbp, %rbp

    # Save argc/argv across __libc_init (it may use the stack)
    mov (%rsp), %r12        # argc
    lea 8(%rsp), %r13       # argv

    # Align stack to 16 bytes before any call
    and $-16, %rsp

    # Initialize libc state (stdio streams, etc.)
    call __libc_init

    # Call main(argc, argv)
    mov %r12, %rdi
    mov %r13, %rsi
    call main

    # Exit with return value from main (after flushing stdio)
    mov %rax, %r12
    call __libc_fini
    mov %r12, %rdi
    jmp _exit

    hlt

# void _exit(int status)
_exit:
    mov %rdi, %rdi
    mov $0, %rax            # SYS_EXIT = 0
    syscall
.Lhang:
    hlt
    jmp .Lhang

.size _start, . - _start
.size _exit, . - _exit
