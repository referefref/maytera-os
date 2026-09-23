// advlktst - #404 Stage 6 end-to-end verification of POSIX (fcntl) + BSD
// (flock) advisory file locking, over the REAL syscall path.
//
// All output goes to fd 1, which the kernel mirrors to the serial console when
// it is not a terminal (proc/fdlayer.c sys_write_inner), so grep the boot
// serial for the "ADVLK:" markers. One SYS_WRITE per line keeps them contiguous
// against the kernel's own log mirror (same reason as pttest's outf).
//
// It forks CHILDREN to get a distinct lock owner: POSIX locks are owned by the
// thread group, so a thread would share the parent's owner and never conflict;
// a fork child gets its own tgid. Each child opens the file ITSELF (its own fd,
// its own owner), because the legacy fd table stamps ownership by tgid.
//
// Tests, each proving one required semantic:
//   T1 exclusive vs F_SETLK overlap  -> EAGAIN
//   T2 F_SETLKW blocks then acquires after the holder unlocks
//   T3 F_GETLK reports the holder (type + pid + range), F_UNLCK if free
//   T4 shared + shared both acquire
//   T5 a lock is released on close of the fd
//   T6 a lock is released on process exit (holder never unlocked)
//   T7 a two-way F_SETLKW deadlock returns EDEADLK to exactly one side

#include "../../libc/maytera.h"   // sys_fork, sys_wait, sys_getpid, vsnprintf, syscall3
#include "../../libc/fcntl.h"     // struct flock, F_*
#include "../../libc/stdlib.h"    // open
#include "../../libc/unistd.h"    // usleep, close
#include "../../libc/sys/file.h"  // flock(), LOCK_*
#include "../../libc/errno.h"

// Advisory locks are keyed on the PATH, not on file contents, and this kernel
// does not require a lock fd to be writable, so we lock an EXISTING readable
// file. The test image gives the session user (uid 1000) no writable directory,
// and creating a scratch file is not what this test is about: it is about the
// lock semantics over one shared file that every process opens by the same
// path. Byte-range locks may extend past EOF (POSIX), so the file's small size
// does not matter.
#define PATH "/HOME/PLAYLIST.M3U"
#define OPENMODE O_RDONLY

static void outf(const char *fmt, ...) {
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    syscall3(SYS_WRITE, 1, (long)(unsigned long)buf, (long)n);
}

// Apply one fcntl lock op. Returns fcntl's return (0 or -1); errno is set.
static int flk(int fd, int cmd, int type, long long start, long long len) {
    struct flock l;
    l.l_type = (short)type;
    l.l_whence = 0;          // SEEK_SET
    l.l_start = start;
    l.l_len = len;
    l.l_pid = 0;
    return fcntl(fd, cmd, &l);
}

static int pass_count = 0, fail_count = 0;
static void check(int ok, const char *name) {
    if (ok) { pass_count++; outf("ADVLK: [PASS] %s\n", name); }
    else    { fail_count++; outf("ADVLK: [FAIL] %s\n", name); }
}

int main(void) {
    outf("ADVLK: ==== advisory-lock end-to-end test start (pid=%d) ====\n", getpid());

    // Open the shared lock file (read-only; see PATH note above).
    int fd = open(PATH, OPENMODE);
    if (fd < 0) {
        outf("ADVLK: [FATAL] cannot open %s (fd=%d) - cannot run\n", PATH, fd);
        outf("ADVLK: RESULT SOME FAIL\n");
        return 1;
    }

    // ---- T1: exclusive lock blocks an overlapping F_SETLK (EAGAIN) ----------
    check(flk(fd, F_SETLK, F_WRLCK, 0, 10) == 0, "T1a parent takes WRLCK [0,10)");
    {
        int pid = sys_fork();
        if (pid == 0) {
            int cfd = open(PATH, OPENMODE);
            int r = flk(cfd, F_SETLK, F_WRLCK, 5, 10);   // overlaps [0,10)
            int e = errno;
            outf("ADVLK: T1 child F_SETLK overlap -> r=%d errno=%d (want -1/EAGAIN=%d)\n", r, e, EAGAIN);
            sys_exit((r == -1 && e == EAGAIN) ? 0 : 1);
        }
        int st = -1; sys_wait(&st);
        check(st == 0, "T1b overlapping F_SETLK returns EAGAIN");
    }

    // ---- T2: F_SETLKW blocks until the holder unlocks -----------------------
    // parent still holds WRLCK [0,10).
    {
        int pid = sys_fork();
        if (pid == 0) {
            int cfd = open(PATH, OPENMODE);
            outf("ADVLK: T2 child about to F_SETLKW WRLCK [0,10) (should BLOCK)\n");
            int r = flk(cfd, F_SETLKW, F_WRLCK, 0, 10);
            outf("ADVLK: T2 child F_SETLKW returned r=%d (should be 0, AFTER parent unlock)\n", r);
            sys_exit(r == 0 ? 0 : 1);
        }
        usleep(400000);   // 400ms: let the child reach and block in F_SETLKW
        outf("ADVLK: T2 parent unlocking [0,10) now (child should then acquire)\n");
        check(flk(fd, F_SETLK, F_UNLCK, 0, 10) == 0, "T2a parent F_UNLCK [0,10)");
        int st = -1; sys_wait(&st);
        check(st == 0, "T2b F_SETLKW acquired after the holder unlocked");
    }

    // ---- T3: F_GETLK reports the conflicting holder -------------------------
    check(flk(fd, F_SETLK, F_WRLCK, 0, 10) == 0, "T3a parent re-takes WRLCK [0,10)");
    {
        int pid = sys_fork();
        if (pid == 0) {
            int cfd = open(PATH, OPENMODE);
            struct flock g;
            g.l_type = F_WRLCK; g.l_whence = 0; g.l_start = 0; g.l_len = 10; g.l_pid = 0;
            int r = fcntl(cfd, F_GETLK, &g);
            outf("ADVLK: T3 child F_GETLK -> r=%d type=%d start=%lld len=%lld pid=%d\n",
                 r, g.l_type, g.l_start, g.l_len, g.l_pid);
            int held_ok = (r == 0 && g.l_type == F_WRLCK && g.l_start == 0 && g.l_pid != 0);
            // A free range must report F_UNLCK.
            struct flock g2;
            g2.l_type = F_WRLCK; g2.l_whence = 0; g2.l_start = 500; g2.l_len = 10; g2.l_pid = 0;
            int r2 = fcntl(cfd, F_GETLK, &g2);
            outf("ADVLK: T3 child F_GETLK free-range -> r=%d type=%d (want F_UNLCK=%d)\n", r2, g2.l_type, F_UNLCK);
            int free_ok = (r2 == 0 && g2.l_type == F_UNLCK);
            sys_exit((held_ok && free_ok) ? 0 : 1);
        }
        int st = -1; sys_wait(&st);
        check(st == 0, "T3b F_GETLK reports holder, F_UNLCK when free");
    }
    flk(fd, F_SETLK, F_UNLCK, 0, 10);

    // ---- T4: shared + shared are compatible --------------------------------
    check(flk(fd, F_SETLK, F_RDLCK, 0, 10) == 0, "T4a parent takes RDLCK [0,10)");
    {
        int pid = sys_fork();
        if (pid == 0) {
            int cfd = open(PATH, OPENMODE);
            int r = flk(cfd, F_SETLK, F_RDLCK, 0, 10);   // shared over the same range
            outf("ADVLK: T4 child shared F_SETLK -> r=%d (want 0)\n", r);
            sys_exit(r == 0 ? 0 : 1);
        }
        int st = -1; sys_wait(&st);
        check(st == 0, "T4b shared+shared both acquire");
    }
    flk(fd, F_SETLK, F_UNLCK, 0, 10);

    // ---- T5: a lock is released on close of the fd -------------------------
    {
        int fd2 = open(PATH, OPENMODE);
        check(flk(fd2, F_SETLK, F_WRLCK, 20, 10) == 0, "T5a lock [20,30) on a 2nd fd");
        close(fd2);   // POSIX: closing any fd on the file releases the process's locks
        int pid = sys_fork();
        if (pid == 0) {
            int cfd = open(PATH, OPENMODE);
            int r = flk(cfd, F_SETLK, F_WRLCK, 20, 10);
            outf("ADVLK: T5 child F_SETLK [20,30) after close -> r=%d (want 0)\n", r);
            sys_exit(r == 0 ? 0 : 1);
        }
        int st = -1; sys_wait(&st);
        check(st == 0, "T5b lock released on close");
    }

    // ---- T6: a lock is released on process exit ----------------------------
    {
        int pid = sys_fork();
        if (pid == 0) {
            int cfd = open(PATH, OPENMODE);
            int r = flk(cfd, F_SETLK, F_WRLCK, 40, 10);
            outf("ADVLK: T6 holder-child took WRLCK [40,50) r=%d, exiting WITHOUT unlock\n", r);
            sys_exit(0);   // exit still holding the lock
        }
        int st = -1; sys_wait(&st);   // holder is now gone
        int pid2 = sys_fork();
        if (pid2 == 0) {
            int cfd = open(PATH, OPENMODE);
            int r = flk(cfd, F_SETLK, F_WRLCK, 40, 10);
            outf("ADVLK: T6 second-child F_SETLK [40,50) -> r=%d (want 0, holder exited)\n", r);
            sys_exit(r == 0 ? 0 : 1);
        }
        int st2 = -1; sys_wait(&st2);
        check(st2 == 0, "T6 lock released on process exit");
    }

    // ---- T7: two-way F_SETLKW deadlock returns EDEADLK to one side ----------
    // X: hold R1=[0,10), then wait for R2=[20,30). Y: hold R2, then wait for R1.
    {
        int px = sys_fork();
        if (px == 0) {
            int cfd = open(PATH, OPENMODE);
            flk(cfd, F_SETLK, F_WRLCK, 0, 10);   // R1
            usleep(300000);                      // let Y grab R2
            int r = flk(cfd, F_SETLKW, F_WRLCK, 20, 10);  // wait for R2
            int e = errno;
            outf("ADVLK: T7 X F_SETLKW R2 -> r=%d errno=%d (%s)\n", r, e,
                 (r == -1 && e == EDEADLK) ? "EDEADLK" : (r == 0 ? "acquired" : "other"));
            sys_exit(r == 0 ? 0 : (e == EDEADLK ? 2 : 1));
        }
        int py = sys_fork();
        if (py == 0) {
            int cfd = open(PATH, OPENMODE);
            flk(cfd, F_SETLK, F_WRLCK, 20, 10);  // R2
            usleep(300000);                      // let X grab R1
            int r = flk(cfd, F_SETLKW, F_WRLCK, 0, 10);   // wait for R1
            int e = errno;
            outf("ADVLK: T7 Y F_SETLKW R1 -> r=%d errno=%d (%s)\n", r, e,
                 (r == -1 && e == EDEADLK) ? "EDEADLK" : (r == 0 ? "acquired" : "other"));
            sys_exit(r == 0 ? 0 : (e == EDEADLK ? 2 : 1));
        }
        int sx = -1, sy = -1;
        sys_wait(&sx);
        sys_wait(&sy);
        outf("ADVLK: T7 exits: sx=%d sy=%d (want one 0 and one 2)\n", sx, sy);
        check((sx == 2 && sy == 0) || (sx == 0 && sy == 2),
              "T7 two-way F_SETLKW deadlock -> EDEADLK to exactly one side");
    }

    // ---- flock(): a quick BSD whole-file sanity pass ------------------------
    {
        int r = flock(fd, LOCK_EX);
        outf("ADVLK: flock(LOCK_EX) -> r=%d (want 0)\n", r);
        int ok_ex = (r == 0);
        int pid = sys_fork();
        if (pid == 0) {
            int cfd = open(PATH, OPENMODE);
            int cr = flock(cfd, LOCK_EX | LOCK_NB);   // should fail, parent holds EX
            int e = errno;
            outf("ADVLK: flock child LOCK_EX|NB -> r=%d errno=%d (want -1/EAGAIN)\n", cr, e);
            sys_exit((cr == -1 && e == EAGAIN) ? 0 : 1);
        }
        int st = -1; sys_wait(&st);
        int ok_nb = (st == 0);
        flock(fd, LOCK_UN);
        check(ok_ex && ok_nb, "T8 flock LOCK_EX excludes a second LOCK_EX|LOCK_NB");
    }

    close(fd);
    outf("ADVLK: ==== done: %d passed, %d failed ====\n", pass_count, fail_count);
    outf("ADVLK: RESULT %s\n", fail_count == 0 ? "ALL PASS" : "SOME FAIL");
    return 0;
}
