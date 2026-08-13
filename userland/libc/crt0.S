/* SPDX-License-Identifier: MIT
 * Copyright (c) MayteraOS contributors.
 * Full license text: userland/libc/LICENSE (MIT License).
 */
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

/* #640 leg 4: mark the stack NON-EXECUTABLE.
   An object with no .note.GNU-stack section is assumed by the linker to need
   an executable stack, and ld then marks PT_GNU_STACK RWE for the whole link.
   crt0.o is in every app, so ONE missing note made the entire 141-app fleet
   advertise an executable stack. MayteraOS does not read PT_GNU_STACK today,
   so this changes no behaviour now; it stops the binaries from asserting
   something false, and it is a prerequisite for ever honouring the flag. */
.section .note.GNU-stack,"",@progbits
