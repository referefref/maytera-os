/* start.S - #640 stage 1: entry stub for the PIE proof app.
 *
 * Deliberately does NOT use libc/crt0.asm. Stage 1's whole job is to prove the
 * KERNEL's relocation engine does real work; linking against a libc built
 * without -fPIE would drag text relocations into the link and turn a clean
 * kernel-side result into an argument about the toolchain. Stage 2 owns making
 * libc position-independent.
 *
 * Every instruction here is position-independent by construction: no absolute
 * address is named.
 */
    .text
    .globl _start
    .type _start, @function
_start:
    xorq    %rbp, %rbp          /* clean frame chain */
    andq    $-16, %rsp          /* the kernel hands us 16-byte alignment; the
                                   call below then leaves rsp%16 == 8 at
                                   pie_main's entry, which is what the SysV ABI
                                   requires. Getting this backwards is the
                                   classic freestanding-entry movaps fault. */
    call    pie_main            /* PC-relative */
    movq    %rax, %rdi          /* exit(status from pie_main) */
1:  xorq    %rax, %rax          /* SYS_EXIT == 0 */
    syscall
    jmp     1b                  /* SYS_EXIT must not return; if it ever does,
                                   spin rather than fall into whatever follows */
    .size _start, .-_start
    .section .note.GNU-stack,"",@progbits
