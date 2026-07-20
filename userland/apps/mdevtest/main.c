// mdevtest - headless self-test for the mdev remote dev bridge.
//
// Prints identifiable markers, echoes its argv, then exits with a chosen code
// (default 0, or the decimal value of argv[1] if given). It is deliberately
// non-GUI and print-and-exit so the RemoteCtrl `run`/`shell` capture loop
// terminates cleanly and a client can read a real exit code.
//
// Build tag is baked in via -DMDEVTEST_TAG so successive rebuilds produce
// visibly different output, proving the push->rebuild->push->run loop actually
// shipped a new binary.
#include "../../libc/maytera.h"

#ifndef MDEVTEST_TAG
#define MDEVTEST_TAG "dev"
#endif

int main(int argc, char **argv) {
    printf("MDEVTEST: hello from MayteraOS userland (tag=%s)\n", MDEVTEST_TAG);
    printf("MDEVTEST: argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("MDEVTEST: argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");

    int code = 0;
#ifdef FORCE_EXIT
    code = FORCE_EXIT;
#endif
    if (argc >= 2 && argv[1]) {
        const char *p = argv[1];
        int v = 0, ok = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; ok = 1; }
        if (ok) code = v;
    }
    printf("MDEVTEST: done, exiting with code %d\n", code);
    return code;
}
