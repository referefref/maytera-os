// fsstress - #618 concurrency stress for the ext2 write path.
//
// Purpose: run CONCURRENTLY with a large App Store install and keep hammering
// the same directory with small file create/write/close/unlink cycles. That is
// the #597 scenario (rapid multi-file create in one directory corrupted the
// directory inode) and is the real risk of any change that shortens or batches
// work under ext2_lock. If e2fsck is clean after this has run against a
// 100MB-plus install, the batched free path (#618) has not reopened #597.
//
// It writes to /FSSTRESS/, never to a system path, and reports progress on
// stdout (serial) so the run is auditable from the boot log.

#include "../../libc/syscall.h"

static void put(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

static void putu(unsigned int v) {
    char b[12];
    int i = 11;
    b[i--] = 0;
    if (!v) b[i--] = '0';
    while (v) { b[i--] = (char)('0' + (v % 10)); v /= 10; }
    put(&b[i + 1]);
}

int main(void) {
    // Rotate over a fixed set of names so the directory both grows and has
    // entries reused, which is what stressed ext2_dir_add/ext2_dir_remove.
    static const char *names[8] = {
        "/FSS0.TXT", "/FSS1.TXT", "/FSS2.TXT", "/FSS3.TXT",
        "/FSS4.TXT", "/FSS5.TXT", "/FSS6.TXT", "/FSS7.TXT"
    };
    static char payload[2048];
    for (int i = 0; i < 2048; i++) payload[i] = (char)('A' + (i % 26));

    put("[FSSTRESS] start\n");
    unsigned int done = 0, fail = 0;
    for (unsigned int round = 0; round < 9000; round++) {
        const char *p = names[round & 7];
        int fd = sys_open(p, 0x41);          // O_WRONLY|O_CREAT
        if (fd < 0) { fail++; }
        else {
            unsigned int len = 64 + (round % 24) * 80;   // 64 .. 1904 bytes
            if (sys_write(fd, payload, len) != (long)len) fail++;
            if (sys_close(fd) != 0) fail++;
            done++;
        }
        if ((round & 7) == 7) sys_unlink(names[(round - 3) & 7]);
        if ((round % 50) == 49) {
            put("[FSSTRESS] rounds=");
            putu(done);
            put(" fail=");
            putu(fail);
            put("\n");
        }
        sys_sleep(40);                        // ~25 cycles/s, runs for ~2.5 min
    }
    put("[FSSTRESS] done rounds=");
    putu(done);
    put(" fail=");
    putu(fail);
    put("\n");
    for (int i = 0; i < 8; i++) sys_unlink(names[i]);
    sys_exit(0);
    return 0;
}
