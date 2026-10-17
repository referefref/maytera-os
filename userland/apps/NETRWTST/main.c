// NETRWTST - #747 verifier for READ-MODIFY-WRITE on the three whole-file
// filesystem families: SMB shares, NFS exports and /MNT aux mounts.
//
// Modelled on ESCROWT/FDXTEST: a Ring-3 app launched at boot by the gated
// proc/netrw_vmtest.c launcher, whose verdict goes to stdout (serial via
// /dev/console) AND to /BOOTLOG.TXT via sys_bootlog, so a headless VM can
// capture it either way. It is a Ring-3 PROGRAM on purpose: the defect it
// measures is not AI-specific or app-specific, it affects every program that
// opens a file on a share for writing, so the proof is an ordinary program
// doing ordinary POSIX file I/O.
//
// THE DEFECT, in the words of the measurement:
//   On an SMB share, an NFS export and a /MNT aux mount, opening an EXISTING
//   file for writing and closing it EMPTIED THE FILE. Writing four bytes to a
//   one-megabyte file left a four-byte file. The descriptor also could not read
//   back a single byte of the file it had just opened.
//
// Every case below prints the MEASURED byte count, not just PASS/FAIL, because
// "FAIL" alone does not tell you whether a file lost its tail or lost
// everything.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"

// 64 KiB: big enough that "4 bytes left" is unmistakable, small enough to
// upload over SMB/NFS quickly and to fit the 256 KiB /MNT aux bound.
#define SEED_LEN   65536
// A runaway guard for the read-back loop, not a test size.
#define READ_MAX   (4 * 1024 * 1024)

static int g_pass = 0, g_fail = 0;
// Which instance is speaking. This binary is launched TWICE on a test image,
// by two EXISTING boot-launch primitives, because they differ in the one way
// that matters for the aux-mount arm:
//   /CONFIG/AUTORUN.CFG   -> the session uid (1000 on a shipping image), which
//                            is the realistic case for the SMB/NFS arms;
//   /CONFIG/ESCROW.TEST   -> uid 0, which SYS_MOUNT requires.
// Using the launchers that already exist means the RED and GREEN arms use an
// IDENTICAL launch path and the kernel is the only variable.
static char g_tag[24] = "NETRWTST";

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }

static void sayf(const char *fmt, long a, long b, long c) {
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, a, b, c);
    say(buf);
}

static void verdict(const char *fam, const char *what, int ok,
                    long got, long want) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %-18s %-28s %s  (measured %ld, expected %ld)\n",
             g_tag, fam, what, ok ? "PASS" : "FAIL", got, want);
    say(buf);
    if (ok) g_pass++; else g_fail++;
}

// One byte of the seed pattern. Position-dependent, so a wrong offset shows up
// as wrong CONTENT and not only as a wrong length.
static unsigned char pat(long i) { return (unsigned char)(0x41 + (i % 23)); }

// Write `len` bytes of the pattern as the file's entire contents. Uses
// O_CREAT|O_WRONLY|O_TRUNC, which is the one open mode that is SUPPOSED to
// replace a file, so seeding never depends on the behaviour under test.
static int seed(const char *path, long len) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
    static unsigned char blk[4096];
    long off = 0;
    while (off < len) {
        long n = len - off;
        if (n > (long)sizeof(blk)) n = sizeof(blk);
        for (long i = 0; i < n; i++) blk[i] = pat(off + i);
        long w = write(fd, blk, n);
        if (w != n) { close(fd); return -2; }
        off += n;
    }
    if (close(fd) != 0) return -3;
    return 0;
}

// Read the whole file back through a FRESH O_RDONLY descriptor (so nothing is
// being read out of the buffer under test) and return its length, or negative.
// `first4` receives the first four bytes; `tailok` reports whether every byte
// from `check_from` to the end still matches the seed pattern.
static long readback(const char *path, unsigned char *first4,
                     long check_from, int *tailok) {
    if (first4) { first4[0] = first4[1] = first4[2] = first4[3] = 0; }
    if (tailok) *tailok = 1;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    static unsigned char blk[4096];
    long total = 0;
    for (;;) {
        long r = read(fd, blk, sizeof(blk));
        if (r < 0) { close(fd); return -2; }
        if (r == 0) break;
        for (long i = 0; i < r; i++) {
            long pos = total + i;
            if (pos < 4 && first4) first4[pos] = blk[i];
            if (tailok && pos >= check_from && blk[i] != pat(pos)) *tailok = 0;
        }
        total += r;
        if (total > READ_MAX) break;   // runaway guard
    }
    close(fd);
    return total;
}

// WAIT FOR THE MOUNT TO EXIST BEFORE JUDGING IT.
//
// /CONFIG/AUTORUN.CFG launches this app as soon as the compositor presents its
// first frame, and the SMB/NFS mounts are re-created from /CONFIG/NETMOUNTS.CFG
// by a deferred kernel worker that sleeps 12 s first and then performs its
// mount round trips. So the targets are reliably NOT there yet at startup, and
// a run that started immediately would report "cannot reach this filesystem"
// for a reason that has nothing to do with the thing under test.
//
// A retry loop in a RING-3 TEST PROGRAM, not a kernel poll: there is no wait
// queue reaching Ring 3 for "a network mount appeared", and the #426 rule is
// about kernel waits. 90 tries at 1 s is a bound, and the run SAYS which it
// hit, so a timeout is reported as a missing test environment rather than as a
// failure of the code under test.
static int wait_for_target(const char *dir, int tries)
{
    char probe[256];
    snprintf(probe, sizeof(probe), "%s/RWPROBE.TMP", dir);
    for (int i = 0; i < tries; i++) {
        int fd = open(probe, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            long w = write(fd, "p", 1);
            int c = close(fd);
            if (w == 1 && c == 0) { unlink(probe); return i; }
        }
        sleep(1);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// The seven cases. Each one seeds a fresh file, so a failure cannot cascade.
// ---------------------------------------------------------------------------
static void run_family(const char *fam, const char *dir, long len)
{
    char path[256];
    unsigned char f4[4];
    int tailok;
    long back;

    snprintf(path, sizeof(path), "%s/RWTEST.BIN", dir);

    {
        int waited = wait_for_target(dir, 90);
        char l[200];
        if (waited < 0) {
            snprintf(l, sizeof(l), "%s: %s %s NEVER became writable in 90s - "
                                   "NOT TESTED (missing test environment)\n", g_tag, fam, dir);
            say(l);
            return;
        }
        snprintf(l, sizeof(l), "%s: %s %s reachable after %ds\n", g_tag, fam, dir, waited);
        say(l);
    }

    // ---- gate: the seed itself must work, or nothing below means anything.
    if (seed(path, len) != 0) {
        verdict(fam, "seed the file at all", 0, -1, len);
        say("NETRWTST:   (cannot reach this filesystem; the cases below are SKIPPED)\n");
        return;
    }
    back = readback(path, f4, 0, &tailok);
    if (back != len || !tailok) {
        verdict(fam, "seed read-back", 0, back, len);
        return;
    }
    verdict(fam, "seed read-back", 1, back, len);

    // ---- CASE A. Open for read-write and close WITHOUT WRITING. POSIX: the
    // file is untouched. This case needs no write at all, so a failure here is
    // the open and the close destroying data between them.
    {
        int fd = open(path, O_RDWR, 0);
        int opened = (fd >= 0);
        if (opened) close(fd);
        back = readback(path, f4, 0, &tailok);
        verdict(fam, opened ? "A open O_RDWR + close" : "A O_RDWR open failed",
                (opened && back == len && tailok), back, len);
    }

    // ---- CASE B. The read half of read-modify-write: a descriptor must be
    // able to read the file it just opened O_RDWR.
    if (seed(path, len) == 0) {
        int fd = open(path, O_RDWR, 0);
        long r = -1;
        unsigned char b[16];
        int content_ok = 0;
        if (fd >= 0) {
            r = read(fd, b, sizeof(b));
            if (r == (long)sizeof(b)) {
                content_ok = 1;
                for (long i = 0; i < r; i++) if (b[i] != pat(i)) content_ok = 0;
            }
            close(fd);
        }
        verdict(fam, "B O_RDWR read its own file",
                (r == (long)sizeof(b) && content_ok), r, (long)sizeof(b));
    }

    // ---- CASE C. THE HEADLINE CASE. Overwrite four bytes at offset 0 of an
    // existing file and keep the rest.
    if (seed(path, len) == 0) {
        int fd = open(path, O_RDWR, 0);
        long w = -1;
        if (fd >= 0) {
            w = write(fd, "ZZZZ", 4);
            if (close(fd) != 0) w = -99;
        }
        back = readback(path, f4, 4, &tailok);
        int ok = (w == 4 && back == len && tailok &&
                  f4[0] == 'Z' && f4[1] == 'Z' && f4[2] == 'Z' && f4[3] == 'Z');
        verdict(fam, "C write 4 bytes, keep rest", ok, back, len);
        if (!ok)
            sayf("NETRWTST:   -> write returned %ld, file is now %ld bytes, "
                 "tail intact=%ld\n", w, back, (long)tailok);
    }

    // ---- CASE D. O_WRONLY with NEITHER O_TRUNC NOR O_APPEND. This is the
    // exact mode of the ext2 "#745 local 109" measurement, and busybox vi's
    // deliberate save mode.
    if (seed(path, len) == 0) {
        int fd = open(path, O_WRONLY, 0);
        long w = -1;
        if (fd >= 0) {
            w = write(fd, "YYYY", 4);
            if (close(fd) != 0) w = -99;
        }
        back = readback(path, f4, 4, &tailok);
        verdict(fam, "D O_WRONLY no-trunc keeps rest",
                (w == 4 && back == len && tailok && f4[0] == 'Y'), back, len);
    }

    // ---- CASE E. O_APPEND must ADD to the file, not replace it.
    if (seed(path, len) == 0) {
        int fd = open(path, O_WRONLY | O_APPEND, 0);
        long w = -1;
        if (fd >= 0) {
            w = write(fd, "WWWW", 4);
            if (close(fd) != 0) w = -99;
        }
        back = readback(path, f4, 0, &tailok);
        // Only the first `len` bytes are pattern; the 4 appended are not, so
        // check the pattern up to len by reading it back separately.
        int head_ok = 1;
        {
            int rfd = open(path, O_RDONLY, 0);
            if (rfd < 0) head_ok = 0;
            else {
                static unsigned char blk[4096];
                long pos = 0;
                for (;;) {
                    long r = read(rfd, blk, sizeof(blk));
                    if (r <= 0) break;
                    for (long i = 0; i < r; i++) {
                        long p = pos + i;
                        if (p < len && blk[i] != pat(p)) head_ok = 0;
                    }
                    pos += r;
                    if (pos > len + 4096) break;
                }
                close(rfd);
            }
        }
        verdict(fam, "E O_APPEND adds, keeps rest",
                (w == 4 && back == len + 4 && head_ok), back, len + 4);
    }

    // ---- CASE F. O_TRUNC MUST STILL EMPTY THE FILE. The fix must not turn
    // into "never shorten anything": a caller that ASKS for an empty file gets
    // one. This is the case that would catch an over-correction.
    if (seed(path, len) == 0) {
        int fd = open(path, O_WRONLY | O_TRUNC, 0);
        long w = -1;
        if (fd >= 0) {
            w = write(fd, "TT", 2);
            if (close(fd) != 0) w = -99;
        }
        back = readback(path, f4, 0, 0);
        verdict(fam, "F O_TRUNC still empties", (w == 2 && back == 2), back, 2);
    }

    // ---- CASE G. ftruncate() on one of these descriptors. Before the fix it
    // returned -1 for this whole family, which did not matter while the open
    // was emptying the file anyway. It matters now: without it a program that
    // saves a SHORTER file leaves a stale tail.
    if (seed(path, len) == 0) {
        int fd = open(path, O_RDWR, 0);
        long rc = -1;
        if (fd >= 0) {
            rc = ftruncate(fd, 100);
            if (close(fd) != 0) rc = -99;
        }
        back = readback(path, f4, 0, &tailok);
        verdict(fam, "G ftruncate shortens to 100",
                (rc == 0 && back == 100 && tailok), back, 100);
    }

    // Leave the medium tidy.
    unlink(path);
}

int main(void)
{
    int uid = (int)getuid();
    snprintf(g_tag, sizeof(g_tag), uid == 0 ? "NETRWTST(root)" : "NETRWTST(u%d)", uid);
    say("\n================ ");
    say(g_tag);
    say(" (#747) ================\n");
    say("NETRWTST: read-modify-write on whole-file filesystems (SMB / NFS / aux)\n");

    // ---- THE TARGET LIST IS THE MARKER FILE. /CONFIG/NETRW.TEST both arms the
    // launcher and names what to test, one entry per line:
    //     /SMB/<server>/<share>     a mounted SMB share
    //     /NFS/<server>/<label>     a mounted NFS export
    //     AUXMNT                    mount the scratch device at /MNT/rwtest
    //     #...                      comment
    // No server address is compiled into this app: the test environment lives
    // in the test image, not in the source tree.
    static char cfg[2048];
    long cfglen = 0;
    {
        int fd = open("/CONFIG/NETRW.TEST", O_RDONLY, 0);
        if (fd >= 0) {
            cfglen = read(fd, cfg, sizeof(cfg) - 1);
            close(fd);
        }
        if (cfglen < 0) cfglen = 0;
        cfg[cfglen] = 0;
    }
    if (cfglen == 0) {
        say("NETRWTST: /CONFIG/NETRW.TEST is empty or unreadable - NOTHING TESTED\n");
        say("================ NETRWTST: NO TARGETS ================\n");
        return 2;
    }

    char *p = cfg;
    int did_aux = 0;
    while (*p) {
        char line[256]; int i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < 255) line[i++] = *p++;
        line[i] = 0;
        while (*p == '\n' || *p == '\r') p++;
        if (line[0] == 0 || line[0] == '#') continue;

        if (line[0] == '/') {
            // The network arms belong to the UNPRIVILEGED instance: that is the
            // realistic caller, and running them twice would have two
            // instances fighting over the same files on the same share.
            if (uid == 0) continue;
            const char *fam = (line[1] == 'S' || line[1] == 's') ? "SMB share"
                            : (line[1] == 'N' || line[1] == 'n') ? "NFS export"
                            : "filesystem";
            run_family(fam, line, SEED_LEN);
            continue;
        }
        if (!strcmp(line, "AUXMNT")) {
            // SYS_MOUNT is root-only, so only the uid-0 instance can do this.
            if (uid != 0) {
                say("NETRWTST: AUXMNT skipped by the unprivileged instance "
                    "(SYS_MOUNT is root-only); the uid-0 instance runs it\n");
                did_aux = 1;
                continue;
            }
            // The scratch block device and its ext2 partition are prepared by
            // the /DISKMGR.TST boot harness; mounting it is a root-only
            // syscall, which this app has because the launcher starts it uid 0.
            did_aux = 1;
            int rc = mount_at(BLK_KIND_SCRATCH, 0, 1, "/MNT/rwtest");
            char l2[160];
            snprintf(l2, sizeof(l2),
                     "NETRWTST: mount_at(scratch p1 -> /MNT/rwtest) = %d\n", rc);
            say(l2);
            if (rc == 0) {
                run_family("MNT aux (ext2)", "/MNT/rwtest", SEED_LEN);
                umount_at("/MNT/rwtest");
            } else {
                say("NETRWTST: /MNT aux mount NOT AVAILABLE - aux family NOT TESTED "
                    "(needs /DISKMGR.TST on the ESP so the scratch device exists)\n");
            }
            continue;
        }
        {
            char l3[200];
            snprintf(l3, sizeof(l3),
                     "NETRWTST: ignoring unrecognised target line '%s'\n", line);
            say(l3);
        }
    }
    if (!did_aux)
        say("NETRWTST: no AUXMNT line - the /MNT aux family was NOT TESTED\n");

    {
        char line[160];
        snprintf(line, sizeof(line),
                 "%s: TOTAL %d passed, %d FAILED\n", g_tag, g_pass, g_fail);
        say(line);
        say(g_fail == 0 ? "================ NETRWTST: PASS ================\n"
                        : "================ NETRWTST: FAIL ================\n");
    }
    return g_fail == 0 ? 0 : 1;
}
