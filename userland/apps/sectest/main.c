// sectest - #500 / MAYTERA-SEC-2026-0016 Ring-3 trust-boundary probe.
//
// Two independent jobs, deliberately separated:
//
//  (1) DIRECT HARDWARE PROBE. Before asking anything about syscalls, establish
//      whether Ring 3 can simply READ kernel memory with a plain mov. If it
//      can, the paging permissions are wrong and no amount of syscall argument
//      validation matters, because the attacker never needs a syscall. This has
//      to be measured, not assumed: vmm_init() keeps UEFI's page tables rather
//      than building its own, and whether UEFI sets U/S on its identity map is
//      a property of the firmware, not of our source.
//
//  (2) VALIDATOR BATTERY. Ask the kernel to run the negative controls in
//      security/validate_test.c against OUR address space, with a real user
//      buffer of ours.
//
// A fault in (1) kills this process; that is the GOOD outcome and we say so
// before attempting it, so the serial log distinguishes "faulted = isolated"
// from "printed a value = wide open".

#include "../../libc/maytera.h"

#define SYS_SECTEST 323

static char probe_buf[8192] __attribute__((aligned(4096)));

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("\n[sectest] ===== #500 Ring-3 trust boundary probe =====\n");

    // ---- (2) validator battery, in our address space, with our real buffer.
    printf("[sectest] calling SYS_SECTEST with user buf=%p\n", probe_buf);
    long fails = syscall2(SYS_SECTEST, (long)probe_buf, sizeof(probe_buf));
    printf("[sectest] validator battery reported %ld failing control(s)\n", fails);

    // ---- (1) the direct probe. Do this LAST: it may (should!) kill us.
    printf("[sectest] ---- direct Ring-3 read of kernel text @0x400000 ----\n");
    printf("[sectest] if the next line does NOT print, Ring 3 faulted = ISOLATED = good\n");
    printf("[sectest] if it DOES print a value, Ring 3 can read kernel memory\n");
    printf("[sectest]   directly and the syscall boundary is irrelevant.\n");

    volatile unsigned char *kernel_text = (volatile unsigned char *)0x400000UL;
    unsigned char b0 = kernel_text[0];
    unsigned char b1 = kernel_text[1];
    unsigned char b2 = kernel_text[2];
    unsigned char b3 = kernel_text[3];

    printf("[sectest] *** NO FAULT *** kernel text[0..3] = %02x %02x %02x %02x\n",
           b0, b1, b2, b3);
    printf("[sectest] *** RING 3 CAN READ KERNEL MEMORY DIRECTLY ***\n");

    return 0;
}
