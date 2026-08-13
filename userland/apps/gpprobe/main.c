// gpprobe - #708 adversarial launcher for the 16-bit guest filesystem probe.
//
// WHY IT EXISTS. The hole under test is "a DOS or Win16 GUEST bypasses
// perms_check()". Proving that needs a guest running at a NON-ROOT uid, and the
// only way a guest gets a non-root identity is to be launched by a non-root
// Ring-3 process. This app is that process.
//
// IT DROPS TO uid 1000 FIRST. The shipped image autologins as root
// (build/assets/LOGIN.CFG: autologin=root), so an autorun-launched app inherits
// uid 0, and at uid 0 perms_check() returns 0 on its first line: every probe
// would come back ALLOWED and would prove nothing at all. setuid(1000) is the
// very first action and its result is asserted, because a failed drop would
// make every line below it a lie.
//
// EVERYTHING GOES TO fd 2. Measured on golden 1004 and re-recorded in
// apps/canarytest: an autorun-spawned process's fd 1 does not reach the serial
// console; fd 2 does.
//
// It launches the SAME .COM twice, through two DIFFERENT syscalls:
//   SYS_DOS_RUN   -> dos/dosexec.c's INT 21h  (the DOS layer)
//   SYS_WIN16_RUN -> exec/ne.c's INT 21h      (the Win16 layer's OWN, separate
//                                              implementation)
// Two layers, two independent gates, one instrument.

#include "../../libc/maytera.h"

#define SYS_WIN16_RUN   237
#define SYS_DOS_RUN     240

#define PROBE_PATH "/DOS/PROBE/PROBE.COM"
// #736 Stage 2: the filesystem-service probe. It CREATES, WRITES, RENAMES,
// DELETES, mkdirs and rmdirs, so at uid 1000 it measures the NEW write paths
// against a root-owned 0755 directory. Every one of them must be denied.
#define FSPROBE_PATH "/DOS/FSPROBE/FSPROBE.COM"

static char g_line[512];
static int  g_len;

static void put_s(const char *s) {
    while (*s && g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = *s++;
}
static void put_n(long v) {
    char b[24]; int i = 23; int neg = 0;
    b[i--] = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i--] = '0';
    while (v > 0) { b[i--] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) b[i--] = '-';
    put_s(&b[i + 1]);
}
// ONE write(2, ...) PER LINE, always: the kernel serial logger treats every
// write() as its own timestamped record, so a line assembled from four writes
// comes out as four reordered fragments (measured, sgprobe's first run).
static void flush_line(void) {
    if (g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = '\n';
    write(2, g_line, (unsigned long)g_len);
    g_len = 0;
}
static void msg(const char *s) { g_len = 0; put_s("[GPP] "); put_s(s); flush_line(); }
static void note(const char *s, long v) {
    g_len = 0; put_s("[GPP] "); put_s(s); put_s(" = "); put_n(v); flush_line();
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    msg("===== #708 16-bit guest filesystem gate probe =====");
    note("uid at entry", syscall0(SYS_GETUID));
    note("setgid(1000) rc", syscall1(SYS_SETGID, 1000));
    note("setuid(1000) rc", syscall1(SYS_SETUID, 1000));

    long uid = syscall0(SYS_GETUID);
    note("uid now", uid);
    if (uid != 1000) {
        msg("FATAL: not uid 1000; every result below would be meaningless. Aborting.");
        return 1;
    }
    msg("CONFIRMED uid=1000. Both guests below are launched by an UNPRIVILEGED process.");

    // ---- ARM 1: the DOS layer (kernel/dos/dosexec.c INT 21h) --------------
    msg("--- ARM 1: SYS_DOS_RUN " PROBE_PATH " (DOS layer INT 21h) ---");
    note("SYS_DOS_RUN rc", syscall1(SYS_DOS_RUN, (long)PROBE_PATH));

    // The guest runs in its own kernel proc and dos_run_file() holds the final
    // frame for 2 s before tearing down, so wait generously rather than racing
    // the teardown. g_dos_busy would reject an overlapping Win16 launch anyway,
    // but a rejected launch would look exactly like a denied one in the log,
    // which is the kind of ambiguity this probe exists to remove.
    sys_sleep(25000);

    // ---- ARM 2: the Win16 layer (kernel/exec/ne.c INT 21h) ----------------
    msg("--- ARM 2: SYS_WIN16_RUN " PROBE_PATH " (Win16 layer INT 21h, ne.c) ---");
    note("SYS_WIN16_RUN rc", syscall1(SYS_WIN16_RUN, (long)PROBE_PATH));

    sys_sleep(25000);

    // ---- ARM 4/5: #736 Stage 2 write paths, both contexts -----------------
    // These run BEFORE arm 3 deliberately. TETRIS.EXE is an interactive NE app
    // that never exits, so it holds g_win16_busy for the remainder of the boot
    // and every later SYS_WIN16_RUN returns -1. Measured: arm 5 reported
    // rc = -1 for exactly that reason when it ran last.
    // Stage 2 made AH=3Ch/40h real and added 41h/56h/39h/3Ah/3Bh/45h. A gate
    // that covered only the read paths would now be a hole, so the write paths
    // are measured the same way the read paths were: from a real guest, at a
    // real non-root uid, against a directory it does not own.
    msg("--- ARM 4: SYS_DOS_RUN " FSPROBE_PATH " (Stage 2 write paths, DOS) ---");
    note("SYS_DOS_RUN(FSPROBE) rc", syscall1(SYS_DOS_RUN, (long)FSPROBE_PATH));
    sys_sleep(25000);

    msg("--- ARM 5: SYS_WIN16_RUN " FSPROBE_PATH " (Stage 2 write paths, Win16) ---");
    note("SYS_WIN16_RUN(FSPROBE) rc", syscall1(SYS_WIN16_RUN, (long)FSPROBE_PATH));
    sys_sleep(25000);

    // ---- ARM 3: a REAL Win16 NE app (kernel/exec/win16api.c) --------------
    // The two arms above reach INT 21h. They do NOT reach the Win16 KERNEL API
    // surface (_lopen, the .INI profile family, DOS3Call, the trace flush),
    // which needs a genuine NE binary running the API thunk. TETRIS.EXE is a
    // real Windows Entertainment Pack title already on the image.
    //
    // What this proves at uid 1000: the win16 slot is armed for an NE guest,
    // win16api.c's gates actually execute (checks > 0 in the teardown report),
    // and the /WIN16LOG.TXT trace write, which is a REAL write to a root-owned
    // path at the filesystem root, is DENIED. At root the same write succeeds,
    // which is the other half of the red/green.
    msg("--- ARM 3: SYS_WIN16_RUN /TETRIS.EXE (real NE, win16api.c surface) ---");
    note("SYS_WIN16_RUN(TETRIS) rc", syscall1(SYS_WIN16_RUN, (long)"/TETRIS.EXE"));
    sys_sleep(40000);

    msg("===== probe run complete =====");
    return 0;
}
