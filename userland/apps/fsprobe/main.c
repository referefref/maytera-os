// FSPROBE - #746 probe for the open/close/rename filesystem fixes.
//
// WHY THIS IS A USERLAND APP AND NOT A KERNEL SELF-TEST. The defects live in
// the SYSCALL fd layer (kernel/proc/syscall.c), and every one of those syscalls
// takes USER pointers: sys_open bounces its path through strncpy_from_user()
// and the ext2 write path uses copy_from_user(). A kernel-side probe therefore
// cannot reach the code under test at all: a first attempt at one had every
// single case report BADTEST because -EFAULT came back from the very first
// open. Ring 3 is the only place this behaviour is observable, so this runs in
// Ring 3, as an autostart service, and prints to fd 1 (the serial console).
//
// THE SAME BINARY IS RUN AGAINST THE PRE-FIX AND POST-FIX KERNELS, so a RED and
// a GREEN run differ in exactly one file: kernel.elf.
//
// EVERY CASE ASSERTS ITS OWN PRECONDITION. A case whose setup did not happen
// prints BADTEST, not PASS. An out-of-space case that was never actually out of
// space proves nothing, and a control that can pass without reaching the code
// under test is not a control.

#include "../../libc/maytera.h"
#include "../../libc/fcntl.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"

static int g_pass, g_fail, g_bad;

// OUTPUT. A service's stdout does NOT reach the serial console on this kernel:
// the first run of this probe completed (the kernel's own [FS] lines prove the
// file operations happened) and produced ZERO visible output. So the report is
// accumulated here and written BOTH to fd 1 and to /FSPROBE.LOG on the ext2
// root, which the host reads out of the image afterwards with debugfs. The log
// file is written with a plain O_WRONLY|O_CREAT|O_TRUNC open, which behaves
// identically on the pre-fix and post-fix kernels, so the reporting channel is
// not itself part of what is under test.
static char g_log[16384];
static int  g_loglen;

static void emit(const char *s) {
    int n = 0; while (s[n]) n++;
    sys_write(1, s, n);
    if (g_loglen + n < (int)sizeof(g_log) - 1) {
        for (int i = 0; i < n; i++) g_log[g_loglen++] = s[i];
        g_log[g_loglen] = 0;
    }
}

static void emitf(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    emit(b);
}

static void rep(const char *name, int ok, const char *detail) {
    if (ok == 1)      { g_pass++; emitf("[FSPROBE] PASS    %-26s %s\n", name, detail); }
    else if (ok == 0) { g_fail++; emitf("[FSPROBE] FAIL    %-26s %s\n", name, detail); }
    else              { g_bad++;  emitf("[FSPROBE] BADTEST %-26s %s\n", name, detail); }
}

// SETUP, not a case: create a file with known content. Returns 0 on success.
static int put(const char *path, const char *data, int len) {
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    if (len && sys_write(fd, data, len) != len) { sys_close(fd); return -2; }
    return sys_close(fd) == 0 ? 0 : -3;
}

// Read a whole file back through a plain O_RDONLY fd. Returns bytes, or < 0.
static int get(const char *path, char *out, int cap) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = sys_read(fd, out, cap);
    sys_close(fd);
    if (n < 0) return -2;
    if (n < cap) out[n] = 0;
    return (int)n;
}

static void nm(char *dst, const char *tag, const char *suffix) {
    int i = 0; while (tag[i] && i < 20) { dst[i] = tag[i]; i++; }
    int j = 0; while (suffix[j] && i < 40) dst[i++] = suffix[j++];
    dst[i] = 0;
}

static void item1(const char *tag, const char *path) {
    static const char seed[] = "HELLO-WORLD-0123456789";
    const int slen = 22;
    char buf[80], name[48];
    int n;

    // 1a: open O_RDWR and close WITHOUT writing. Nothing was written, so any
    // change to the file was caused by the OPEN itself.
    nm(name, tag, ".rdwr-noop");
    if (put(path, seed, slen) != 0) rep(name, -1, "setup: could not create the seed file");
    else {
        int fd = sys_open(path, O_RDWR);
        if (fd < 0) { emitf("[FSPROBE]   open(O_RDWR) rc=%d\n", fd);
                      rep(name, -1, "setup: the O_RDWR open itself failed"); }
        else {
            sys_close(fd);
            n = get(path, buf, sizeof(buf) - 1);
            if (n == slen && memcmp(buf, seed, slen) == 0)
                rep(name, 1, "file intact after open(O_RDWR)+close()");
            else { emitf("[FSPROBE]   size now %d, was %d\n", n, slen);
                   rep(name, 0, "OPEN+CLOSE DESTROYED THE FILE"); }
        }
    }

    // 1b: the fd must be able to READ.
    nm(name, tag, ".rdwr-read");
    if (put(path, seed, slen) != 0) rep(name, -1, "setup: could not create the seed file");
    else {
        int fd = sys_open(path, O_RDWR);
        if (fd < 0) rep(name, -1, "setup: the O_RDWR open itself failed");
        else {
            memset(buf, 0, sizeof(buf));
            long r = sys_read(fd, buf, 5);
            sys_close(fd);
            if (r == 5 && memcmp(buf, "HELLO", 5) == 0)
                rep(name, 1, "read(5) on an O_RDWR fd returned HELLO");
            else { emitf("[FSPROBE]   read returned %ld\n", r);
                   rep(name, 0, "THE O_RDWR fd CANNOT READ"); }
        }
    }

    // 1c: the whole POSIX read-modify-write idiom.
    nm(name, tag, ".rdwr-rmw");
    if (put(path, seed, slen) != 0) rep(name, -1, "setup: could not create the seed file");
    else {
        int fd = sys_open(path, O_RDWR);
        if (fd < 0) rep(name, -1, "setup: the O_RDWR open itself failed");
        else {
            memset(buf, 0, sizeof(buf));
            long r = sys_read(fd, buf, 5);
            long s = sys_seek(fd, 0, 0);
            long w = sys_write(fd, "J", 1);
            int  c = sys_close(fd);
            memset(buf, 0, sizeof(buf));
            n = get(path, buf, sizeof(buf) - 1);
            if (r == 5 && s == 0 && w == 1 && c == 0 && n == slen &&
                memcmp(buf, "JELLO-WORLD-0123456789", slen) == 0)
                rep(name, 1, "read+seek+write kept the tail: JELLO-WORLD-0123456789");
            else { emitf("[FSPROBE]   r=%ld seek=%ld w=%ld close=%d size=%d '%s'\n",
                          r, s, w, c, n, buf);
                   rep(name, 0, "read-modify-write did not preserve the file"); }
        }
    }

    // 1d: O_TRUNC must empty the file. On BOTH filesystems.
    nm(name, tag, ".o_trunc");
    if (put(path, seed, slen) != 0) rep(name, -1, "setup: could not create the seed file");
    else {
        int fd = sys_open(path, O_WRONLY | O_TRUNC);
        if (fd < 0) rep(name, -1, "setup: the O_TRUNC open itself failed");
        else {
            long w = sys_write(fd, "ABC", 3);
            int  c = sys_close(fd);
            memset(buf, 0, sizeof(buf));
            n = get(path, buf, sizeof(buf) - 1);
            if (w == 3 && c == 0 && n == 3 && memcmp(buf, "ABC", 3) == 0)
                rep(name, 1, "O_TRUNC emptied it: 3 bytes, no stale tail");
            else { emitf("[FSPROBE]   w=%ld close=%d size=%d '%s'\n", w, c, n, buf);
                   rep(name, 0, "O_TRUNC LEFT A STALE TAIL"); }
        }
    }

    // 1e: O_APPEND must append, not overwrite from byte 0.
    nm(name, tag, ".o_append");
    if (put(path, "0123456789", 10) != 0) rep(name, -1, "setup: could not create the seed file");
    else {
        int fd = sys_open(path, O_WRONLY | O_CREAT | O_APPEND);
        if (fd < 0) rep(name, -1, "setup: the O_APPEND open itself failed");
        else {
            long w = sys_write(fd, "XY", 2);
            int  c = sys_close(fd);
            memset(buf, 0, sizeof(buf));
            n = get(path, buf, sizeof(buf) - 1);
            if (w == 2 && c == 0 && n == 12 && memcmp(buf, "0123456789XY", 12) == 0)
                rep(name, 1, "O_APPEND appended: 0123456789XY");
            else { emitf("[FSPROBE]   w=%ld close=%d size=%d '%s'\n", w, c, n, buf);
                   rep(name, 0, "O_APPEND OVERWROTE FROM BYTE 0"); }
        }
    }
    sys_unlink(path);
}

static void item2(const char *tag, const char *a, const char *b) {
    char buf[80], name[48];
    nm(name, tag, ".rename-over");
    if (put(a, "SOURCE-DATA-AAAA", 16) != 0 ||
        put(b, "DESTINATION-DATA-BBBBBBBB", 25) != 0) {
        rep(name, -1, "setup: could not create both endpoints");
        return;
    }
    int rr = sys_rename(a, b);
    int na = get(a, buf, sizeof(buf) - 1);
    memset(buf, 0, sizeof(buf));
    int nb = get(b, buf, sizeof(buf) - 1);
    if (rr == 0 && na < 0 && nb == 16 && memcmp(buf, "SOURCE-DATA-AAAA", 16) == 0)
        rep(name, 1, "destination replaced, source gone, content complete");
    else { emitf("[FSPROBE]   rc=%d src=%d dst=%d '%s'\n", rr, na, nb, buf);
           rep(name, 0, "rename over an existing destination is wrong"); }
    sys_unlink(a);
    sys_unlink(b);
}

// A failed FAT write must be reported by close(). The failure is REAL: the ESP
// was filled beforehand, so fat_alloc_cluster() genuinely returns 0 and
// fat_write() returns a SHORT count through its own out-of-clusters path. That
// short count is POSITIVE, which is exactly the shape a caller checking
// `rc < 0` reads as success.
static void item3(void) {
    static char big[8192];
    for (int i = 0; i < (int)sizeof(big); i++) big[i] = (char)('A' + (i % 26));
    int fd = sys_open("/boot/FSPFULL.BIN", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        emitf("[FSPROBE]   open(/boot/FSPFULL.BIN, O_CREAT) rc=%d\n", fd);
        rep("fat.close-reports-enospc", -1,
            "setup: could not even CREATE the file (the ESP is already full)");
        return;
    }
    long total = 0, w = 0;
    int shortw = 0;
    for (int i = 0; i < 8192; i++) {          // bounded; never an unbounded loop
        w = sys_write(fd, big, sizeof(big));
        if (w != (long)sizeof(big)) { shortw = 1; break; }
        total += w;
    }
    int c = sys_close(fd);
    emitf("[FSPROBE]   wrote %ld bytes, then write returned %ld; close returned %d\n",
           total, w, c);
    if (!shortw)
        rep("fat.close-reports-enospc", -1,
            "PRECONDITION NOT MET: the ESP never ran out of clusters, so no "
            "write ever failed. This case proves nothing.");
    else if (w >= 0 && c == 0)
        rep("fat.close-reports-enospc", 0,
            "a write FAILED (short, NON-NEGATIVE) and close() still returned 0");
    else if (c != 0)
        rep("fat.close-reports-enospc", 1, "close() reported the failed write");
    else
        rep("fat.close-reports-enospc", 0, "unexpected combination");
    sys_unlink("/boot/FSPFULL.BIN");
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    emitf("[FSPROBE] ==== #746 open/close/rename probe ====\n");

    // The ext2 ROOT: a plain "/" path routes there once ext2 is the root fs.
    item1("ext2", "/FSPROBE.TXT");
    item2("ext2", "/FSPROBEA.TXT", "/FSPROBEB.TXT");

    // The FAT ESP: /boot and /EFI are the two prefixes never routed to ext2,
    // so a path under /boot is the ESP on an ext2-rooted image.
    item1("fat", "/boot/FSPROBE.TXT");
    item2("fat", "/boot/FSPROBEA.TXT", "/boot/FSPROBEB.TXT");

    item3();

    emitf("[FSPROBE] ==== RESULT pass=%d fail=%d badtest=%d ====\n",
           g_pass, g_fail, g_bad);

    // Persist the report where the host can read it out of the image.
    int lf = sys_open("/FSPROBE.LOG", O_WRONLY | O_CREAT | O_TRUNC);
    if (lf >= 0) { sys_write(lf, g_log, g_loglen); sys_close(lf); }
    return 0;
}
