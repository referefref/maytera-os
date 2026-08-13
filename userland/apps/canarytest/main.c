// canarytest - proof-of-firing test for the userland stack protector (#651)
//
// WHY THIS EXISTS. "The build succeeded" and "nm shows __stack_chk_fail" both
// only prove the symbol is LINKED. Neither proves the guard is seeded with a
// live value, nor that the epilogue check actually catches a smash. This repo
// has a documented history of shipping features that were compiled but never
// ran (security_init with zero callers, sse_save with zero callers, an
// increment_build.sh that was "exit 0"). So this app makes the protector prove
// itself at runtime, on the real artifact.
//
// EVERYTHING IS WRITTEN TO fd 2, NOT printf. Measured on golden 1004: an
// autorun-spawned process's fd 1 does NOT reach the serial console, while fd 2
// does. The first version of this test used printf, and its entire diagnostic
// output vanished; only __stack_chk_fail's own write(2,...) survived. That is
// exactly the trap this file is meant to avoid, so it does its own formatting
// and writes straight to fd 2.
//
// HOW THE SMASH IS DONE, and why not a plain buffer overrun:
// A naive `for (i=0;i<64;i++) buf[i]=...` on a 32-byte buffer depends on the
// exact frame layout gcc chose. Overrun too little and it misses the canary;
// too much and it destroys the saved return address, which crashes the process
// BEFORE the canary check runs - a crash that looks like a failed test but
// actually proves nothing. Instead this locates the canary by VALUE in its own
// frame and flips a bit in it. That is layout-independent, touches exactly one
// 8-byte slot, and leaves the return address intact so the epilogue check is
// definitely the thing that trips.

#include "../../libc/maytera.h"

extern unsigned long __stack_chk_guard;

static void emit(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    write(2, s, n);
}

static void emit_hex(unsigned long v) {
    static const char d[] = "0123456789abcdef";
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = d[(v >> ((15 - i) * 4)) & 0xF];
    b[18] = 0;
    emit(b);
}

// noinline so it gets a real frame of its own; the char array makes it a
// -fstack-protector-strong candidate.
__attribute__((noinline))
static int smash_own_canary(void) {
    char buf[64];
    unsigned long *base = (unsigned long *)(void *)(buf + sizeof(buf));

    // The canary sits above the locals, below the saved RBP/return address.
    // Scan a bounded window upward for the guard value.
    for (unsigned i = 0; i < 16; i++) {
        if (base[i] == __stack_chk_guard) {
            emit("[canarytest] found canary in frame, corrupting it now\n");
            *(volatile unsigned long *)&base[i] ^= 0x0000FF00UL;
            return (int)(unsigned char)buf[0];   // keep buf live
        }
    }
    emit("[canarytest] INCONCLUSIVE: guard value not found in this frame.\n");
    return -1;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    emit("[canarytest] === #651 userland stack protector ===\n");
    emit("[canarytest] guard = ");
    emit_hex((unsigned long)__stack_chk_guard);
    emit("\n");

    if (__stack_chk_guard == 0) {
        emit("[canarytest] FAIL 1/3: guard is ZERO. __libc_init did not seed it,\n");
        emit("[canarytest]   so every guarded function compares 0 against 0 and\n");
        emit("[canarytest]   the protector is decorative. Do not ship.\n");
        return 1;
    }
    emit("[canarytest] PASS 1/3: guard is non-zero (seeded by __libc_init)\n");

    if ((__stack_chk_guard & 0xFF) != 0) {
        emit("[canarytest] WARN 2/3: low byte is not NUL; a str*()-driven overflow\n");
        emit("[canarytest]   could rewrite the canary intact.\n");
    } else {
        emit("[canarytest] PASS 2/3: low byte is NUL (str*() overflow cannot\n");
        emit("[canarytest]   reproduce the canary)\n");
    }

    // #653: trigger a REAL kernel security event before the smash, so the
    // security log and the desktop notification path can be verified end to
    // end. Handing a kernel address to a syscall is rejected by
    // syscall_validate_args() (#500/#503), which now emits AUDIT_PTR_INVALID.
    // The call is EXPECTED to fail with -EFAULT; that failure is the point.
    emit("[canarytest] handing a kernel pointer to write() on purpose;\n");
    emit("[canarytest]   expect -EFAULT + a [SYSARG] REJECTED + a security event\n");
    (void)write(1, (const void *)0xFFFFFFFF80000000UL, 16);

    emit("[canarytest] now smashing this process's own canary; the next line\n");
    emit("[canarytest]   MUST be the stack-smashing message (that is PASS 3/3)\n");
    int r = smash_own_canary();

    // Reaching here means the corrupted canary was NOT caught.
    emit("[canarytest] FAIL 3/3: returned from smash_own_canary() (r=");
    emit_hex((unsigned long)(long)r);
    emit(") WITHOUT tripping __stack_chk_fail. The check is not running.\n");
    return 1;
}
