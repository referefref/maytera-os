// svchb - MayteraOS heartbeat background service (#95 demo service)
//
// A minimal, windowless background service that proves the service manager
// works end to end:
//   - it is launched at boot by the kernel service manager (proc/services.c),
//   - it runs under the dedicated "svc_hb" service account (uid 100),
//   - it is sandboxed to the fs-write capability only (SVC_PERM_FSWRITE),
//   - every ~2 seconds it writes an incrementing heartbeat tick to
//     /SVCLOG.TXT, so an observer can confirm it is alive (and that stopping
//     the service halts the file's updates).
//
// The line is written at a fixed width from offset 0 each cycle, so the file
// never accumulates a stale tail.

#include "../../libc/maytera.h"

#define O_WRONLY  0x0001
#define O_CREAT   0x0040

#define LOG_PATH  "/SVCLOG.TXT"
#define LINE_W    63   // fixed payload width; line[LINE_W] = '\n'

// Minimal unsigned-to-decimal conversion (no libc dependency assumptions).
static int u32_to_str(unsigned long v, char *out) {
    char tmp[12];
    int n = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    unsigned long tick = 0;

    for (;;) {
        tick++;

        // Build a fixed-width line:  "MayteraOS heartbeat service  tick=N"
        char line[LINE_W + 2];
        for (int i = 0; i < LINE_W; i++) line[i] = ' ';
        line[LINE_W]     = '\n';
        line[LINE_W + 1] = '\0';

        const char *pfx = "MayteraOS heartbeat service  tick=";
        int p = 0;
        while (pfx[p] && p < LINE_W) { line[p] = pfx[p]; p++; }
        if (p < LINE_W) p += u32_to_str(tick, line + p);

        int fd = sys_open(LOG_PATH, O_WRONLY | O_CREAT);
        if (fd >= 0) {
            sys_write(fd, line, LINE_W + 1);
            sys_close(fd);
        }

        sys_sleep(2000);
    }

    return 0;
}
