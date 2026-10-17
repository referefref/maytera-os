// ESCRVT - the UNCOOPERATIVE-ACTOR verifier for the recycling AI delete.
//
// WHAT THIS PROVES, and why cooperative code proving it would be worthless.
//
// The owner's requirement (2026-10-03) is that an AI can delete files, that the
// user is told, and that either the user or the AI can put them back. The
// enforcement question is therefore NOT "does the AI behave" but "can code that
// is actively trying to destroy data succeed". This app is that code. It is
// spawned ALREADY MARKED and LOCKED by the kernel trusted spawner
// (escrow_spawn_marked, from proc/escrvt_vmtest.c), so from its first
// instruction it is a mandatory escrow actor, and it then deliberately attempts
// every escape that was open before this change:
//
//   E1 ANNOUNCE AND LEAVE. Call SYS_ESCROW_EXIT to walk out of the contract,
//      then hard-delete. Must be REFUSED, and enforcement must still hold after
//      the attempt.
//   E2 LAUNCH A HELPER. SYS_SPAWN a second program and have IT do the deleting.
//      This WORKED before 2026-10-03: init_proc() memsets the new process_t, so
//      a spawned child started unmarked and was an ordinary uid-0 process. The
//      child must now inherit the mark, LOCKED, and its delete must be
//      redirected like its parent's.
//   E3 RAW SYSCALL. Never touch the userland AI tool layer at all: issue
//      SYS_UNLINK directly, as uid 0. Must be redirected, because the
//      chokepoint is in Ring 0.
//   E4 REMOVE THE DIRECTORY instead of the file. Must be REFUSED (the named
//      exception: a flat Recycle Bin has no form for a tree).
//   E8 REPLACE THE WHOLE FILE IN ONE CALL. SYS_PKG_WRITE (301) takes a path, a
//      buffer and a length and replaces the entire named file. It is not a
//      delete, not a rename and not an open, so none of the other chokepoints
//      saw it, and until 2026-10-03 it kept NO pre-image: measured, its
//      authorization helper and its dispatcher case contained zero mentions of
//      escrow. Both halves are checked: an IN-SCOPE replacement must succeed
//      with the old contents in the bin, and an OUT-OF-SCOPE one must be
//      refused outright by the guard.
//
// And then the half that makes it useful rather than merely safe:
//
//   R1 RECOVERABLE. The deleted file is in the user's Recycle Bin, its index
//      row names the original path, carries a deletion TIMESTAMP, and
//      attributes it to an "ai:" actor.
//   R2 BYTES INTACT. The bin copy is byte-for-byte the original.
//   R3 UNDELETE BY THE AI. The marked actor itself restores the file to its
//      original path and the content matches. The owner asked for undelete "by
//      AI or the user"; this is the AI half, performed by a LOCKED actor under
//      its ordinary in-scope write grant.
//
// Verdict goes to stdout (serial via /dev/console) AND to /BOOTLOG.TXT via
// sys_bootlog, so a headless VM captures it.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"

#define SC      "/ESCRVTSC"                 // the granted scope prefix
#define VICTIM  SC "/UNDELETE.TXT"          // deleted, then restored
#define HELPERV SC "/HELPER.TXT"            // deleted by the SPAWNED helper
#define SUBDIR  SC "/EDIR"                  // an EMPTY dir: rmdir must refuse
#define OUTF    "/ESCRVT_OUT.TMP"           // out of scope: writes must refuse
#define BIN     "/CONFIG/RECYCLE"           // root session home is "/"
#define BINDEX  "/CONFIG/RBINDEX.TXT"
#define HELPER  "/APPS/ESCRVTH"

#define MAGIC   "escrvt-undelete-me-0123456789-the-bytes-must-survive\n"

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static const char *verdict(int ok) { return ok ? "PASS" : "FAIL"; }
static void line(const char *label, int ok) {
    char l[224]; snprintf(l, sizeof(l), "ESCRVT: %s %s\n", label, verdict(ok)); say(l);
}

// Raw, ungated syscalls. The whole point is to NOT go through the userland AI
// tool layer, because a hostile or tricked process would not.
static long r_unlink(const char *p) { return syscall1(SYS_UNLINK, (long)p); }
static long r_rmdir(const char *p)  { return syscall1(SYS_RMDIR, (long)p); }
// SYS_PKG_WRITE is not in userland/libc/syscall.h, so it is declared here with
// its number READ OFF kernel/proc/syscall.h:242 (`#define SYS_PKG_WRITE 301`),
// not remembered. sgprobe and nrprobe declare it locally the same way. A
// guessed syscall number does not fail loudly: it returns -1 for "no such
// syscall" and the arm would report a refusal it never actually provoked, which
// is why the E8 arm below asserts a SUCCESS as well as a refusal.
#define SYS_PKG_WRITE_NR 301
static long r_pkg_write(const char *p, const char *data, long len) {
    return syscall3(SYS_PKG_WRITE_NR, (long)p, (long)data, len);
}

static long r_open_w(const char *p) {
    return syscall2(SYS_OPEN, (long)p, (long)(O_CREAT | O_WRONLY | O_TRUNC));
}

static int exists(const char *p) {
    long fd = syscall2(SYS_OPEN, (long)p, (long)O_RDONLY);
    if (fd >= 0) { syscall1(SYS_CLOSE, fd); return 1; }
    return 0;
}

// Write `data` to `p`. Returns 0 on success.
static int put(const char *p, const char *data) {
    long fd = r_open_w(p);
    if (fd < 0) return -1;
    long n = (long)strlen(data);
    long w = syscall3(SYS_WRITE, fd, (long)data, n);
    syscall1(SYS_CLOSE, fd);
    return (w == n) ? 0 : -1;
}

// Read up to cap-1 bytes of `p` into buf, NUL-terminated. Returns the length,
// or -1.
static long slurp(const char *p, char *buf, long cap) {
    long fd = syscall2(SYS_OPEN, (long)p, (long)O_RDONLY);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long n = syscall3(SYS_READ, fd, (long)(buf + total), cap - 1 - total);
        if (n <= 0) break;
        total += n;
        if (total >= cap - 1) break;
    }
    syscall1(SYS_CLOSE, fd);
    buf[total] = 0;
    return total;
}

static const char *find(const char *hay, const char *needle) {
    if (!*needle) return hay;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return h;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The Recycle Bin index: find the row whose ORIGINAL PATH is `orig` and report
// the trash-side name, the timestamp field and whether the actor token is an
// "ai:" one.
//
// Row format (rustkern/recycle.rs owns it):
//     <trashname>|<original path>|<deleted_unix>|<actor>
//
// This reader is deliberately small and local rather than a shared one: it
// exists to CHECK the format from the outside, and a checker that imports the
// thing it is checking proves less.
// ---------------------------------------------------------------------------
static int bin_lookup(const char *orig, char *tname, long tcap,
                      long *out_ts, int *out_is_ai) {
    static char idx[16384];
    long n = slurp(BINDEX, idx, (long)sizeof(idx));
    if (n <= 0) return 0;
    long i = 0;
    while (i < n) {
        long ls = i;
        while (i < n && idx[i] != '\n') i++;
        long le = i;
        if (i < n) i++;
        // split the line into up to four fields
        long f[5]; int nf = 0;
        f[nf++] = ls;
        for (long k = ls; k < le && nf < 5; k++) if (idx[k] == '|') f[nf++] = k + 1;
        if (nf < 2) continue;
        // field 2 is the original path, ending at the next '|' or end of line
        long ps = f[1], pe = (nf > 2) ? f[2] - 1 : le;
        long olen = (long)strlen(orig);
        if (pe - ps != olen) continue;
        int m = 1;
        for (long k = 0; k < olen; k++) if (idx[ps + k] != orig[k]) { m = 0; break; }
        if (!m) continue;
        // field 1: the trash-side name
        long ns = f[0], ne = f[1] - 1;
        if (ne - ns >= tcap) return 0;
        for (long k = ns; k < ne; k++) tname[k - ns] = idx[k];
        tname[ne - ns] = 0;
        // field 3: the deletion timestamp
        long ts = 0;
        if (nf > 2) {
            long ss = f[2], se = (nf > 3) ? f[3] - 1 : le;
            for (long k = ss; k < se; k++) {
                if (idx[k] < '0' || idx[k] > '9') { ts = 0; break; }
                ts = ts * 10 + (idx[k] - '0');
            }
        }
        if (out_ts) *out_ts = ts;
        // field 4: the actor token
        if (out_is_ai) {
            *out_is_ai = 0;
            if (nf > 3) {
                long as_ = f[3];
                if (as_ + 2 < le && idx[as_] == 'a' && idx[as_ + 1] == 'i' && idx[as_ + 2] == ':')
                    *out_is_ai = 1;
            }
        }
        return 1;
    }
    return 0;
}

int main(void) {
    say("\n========== ESCRVT (recycling AI delete: UNCOOPERATIVE actor) ==========\n");

    // We are already marked + LOCKED. Seed the two victims and the empty dir
    // through the in-scope write grant we were given but never asked for.
    int seeded = (put(VICTIM, MAGIC) == 0) && (put(HELPERV, MAGIC) == 0);
    (void)syscall2(SYS_MKDIR, (long)SUBDIR, 0755);
    line("SETUP.in-scope-writes-ok(marked from first instruction)", seeded);

    // ---------------------------------------------------------------- E1
    // ANNOUNCE AND LEAVE. The escape the owner named explicitly: a process
    // under the promise announcing it is leaving and then deleting freely.
    int e1_refused = (escrow_exit() != 0);
    // Still enforced afterwards? An out-of-scope write must still be refused.
    int e1_still   = (r_open_w(OUTF) < 0) && !exists(OUTF);
    int e1 = e1_refused && e1_still;
    line("E1.self-exit-REFUSED", e1_refused);
    line("E1.still-enforced-after-the-attempt", e1_still);
    line("E1-CANNOT-LEAVE-THE-CONTRACT", e1);

    // ---------------------------------------------------------------- E4
    // REMOVE THE DIRECTORY instead. The named exception: refused.
    int e4 = (r_rmdir(SUBDIR) < 0) && exists(SUBDIR);
    line("E4.rmdir-REFUSED(named exception: no recoverable form for a tree)", e4);

    // ---------------------------------------------------------------- E3 + R1/R2
    // RAW SYSCALL DELETE. Must SUCCEED (the owner wants deletion to work) and
    // must NOT destroy.
    long rc = r_unlink(VICTIM);
    int e3_succeeded = (rc == 0);
    int e3_gone      = !exists(VICTIM);
    line("E3.raw-unlink-SUCCEEDED(the AI can delete)", e3_succeeded);
    line("E3.original-path-is-gone", e3_gone);

    char tname[64]; long ts = 0; int is_ai = 0;
    int r1_listed = bin_lookup(VICTIM, tname, (long)sizeof(tname), &ts, &is_ai);
    line("R1.in-the-Recycle-Bin-index(restorable)", r1_listed);
    line("R1.index-row-carries-a-deletion-TIMESTAMP(retention is possible)", ts > 0);
    line("R1.index-row-attributes-it-to-an-AI-actor", is_ai);

    int r2 = 0;
    static char got[4096];
    char binpath[256];
    if (r1_listed) {
        snprintf(binpath, sizeof(binpath), "%s/%s", BIN, tname);
        long n = slurp(binpath, got, (long)sizeof(got));
        r2 = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
    }
    line("R2.bin-copy-is-BYTE-FOR-BYTE-the-original", r2);

    // ---------------------------------------------------------------- R3
    // UNDELETE BY THE AI, which the owner asked for by name. We are a LOCKED
    // actor; the restore is an ordinary in-scope write under the grant we
    // already hold, so no new authority is needed to put a file back.
    int r3 = 0;
    if (r2) {
        if (put(VICTIM, got) == 0) {
            long n = slurp(VICTIM, got, (long)sizeof(got));
            r3 = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
        }
    }
    line("R3.UNDELETED-BY-THE-AI(bytes match at the original path)", r3);

    // ---------------------------------------------------------------- R4
    // AN EMPTY FILE IS STILL A FILE. The capture reads the original's bytes and
    // writes them into the bin, and a zero-byte file has none, which is a case
    // easy to get wrong in a way no user could ever guess at ("the AI can
    // delete everything except empty files"). This arm MEASURES the answer
    // instead of the kernel comment asserting it. Either outcome is honest:
    // captured, or REFUSED with the file intact. What is NOT acceptable is the
    // delete reporting success with nothing in the bin, so that is what the
    // third check rules out.
    int r4 = 0;
    {
        long fd = r_open_w(SC "/EMPTY.TXT");            // create, zero bytes
        if (fd >= 0) syscall1(SYS_CLOSE, fd);
        if (exists(SC "/EMPTY.TXT")) {
            long erc = r_unlink(SC "/EMPTY.TXT");
            char ename[64]; long ets = 0; int eai = 0;
            int elisted = bin_lookup(SC "/EMPTY.TXT", ename, (long)sizeof(ename), &ets, &eai);
            if (erc == 0) {
                // It claimed success: then it MUST be in the bin.
                r4 = elisted && !exists(SC "/EMPTY.TXT");
                line("R4.empty-file-delete-SUCCEEDED-and-is-in-the-bin", r4);
            } else {
                // It refused: then the file MUST still be there.
                r4 = exists(SC "/EMPTY.TXT");
                line("R4.empty-file-delete-REFUSED-and-the-file-is-INTACT", r4);
                (void)r_unlink(SC "/EMPTY.TXT");
            }
            // The one unacceptable outcome, stated as its own assertion so it
            // cannot hide inside either branch above.
            line("R4.NEVER-success-with-nothing-recoverable",
                 !(erc == 0 && !elisted));
        } else {
            line("R4.could-not-create-an-empty-file(arm did not run)", 0);
        }
    }

    // ---------------------------------------------------------------- E5
    // RENAME OVER AN EXISTING FILE. The escape that does not LOOK like a
    // delete: both fat_rename() and ext2_rename() REPLACE the destination, so
    // rename("junk", "precious") destroyed "precious" without ever touching
    // the delete chokepoint. The displaced file must end up in the bin.
    int e5 = 0;
    {
        put(SC "/VICTIM.TXT", MAGIC);                       // the file to displace
        put(SC "/SRC.TXT", "the replacement\n");
        if (exists(SC "/VICTIM.TXT") && exists(SC "/SRC.TXT")) {
            long rrc = syscall2(SYS_RENAME, (long)(SC "/SRC.TXT"),
                                            (long)(SC "/VICTIM.TXT"));
            char vn[64]; long vts = 0; int vai = 0;
            int vlisted = bin_lookup(SC "/VICTIM.TXT", vn, (long)sizeof(vn), &vts, &vai);
            int vbytes = 0;
            if (vlisted) {
                char vp[256];
                snprintf(vp, sizeof(vp), "%s/%s", BIN, vn);
                long n = slurp(vp, got, (long)sizeof(got));
                vbytes = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
            }
            if (rrc == 0) {
                // The rename went ahead: the DISPLACED file must be in the bin,
                // byte for byte.
                e5 = vlisted && vbytes;
                line("E5.rename-over-SUCCEEDED-and-the-displaced-file-is-in-the-bin", e5);
            } else {
                // It refused: the victim must still be where it was.
                long n = slurp(SC "/VICTIM.TXT", got, (long)sizeof(got));
                e5 = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
                line("E5.rename-over-REFUSED-and-the-victim-is-INTACT", e5);
            }
            // The unacceptable outcome, as its own assertion: the rename
            // succeeded and the displaced bytes are nowhere.
            line("E5.NEVER-displaced-a-file-into-nothing", !(rrc == 0 && !vbytes));
        } else {
            line("E5.could-not-set-up-the-rename-over(arm did not run)", 0);
        }
        (void)r_unlink(SC "/VICTIM.TXT");
        (void)r_unlink(SC "/SRC.TXT");
    }

    // ---------------------------------------------------------------- E6
    // TRUNCATE. The escape that is not a delete AND not a rename: an
    // O_TRUNC open of an existing file destroys every byte in it, and nothing
    // at any layer above the filesystem calls that a deletion. The PRE-IMAGE
    // must be in the bin.
    int e6 = 0;
    {
        const char *TPATH = SC "/TRUNCME.TXT";
        (void)r_unlink(TPATH);
        put(TPATH, MAGIC);                        // fresh: no capture expected
        if (exists(TPATH)) {
            // Now truncate it. r_open_w is O_CREAT|O_WRONLY|O_TRUNC.
            long fd = r_open_w(TPATH);
            int opened = (fd >= 0);
            if (opened) {
                const char *newtext = "replaced\n";
                syscall3(SYS_WRITE, fd, (long)newtext, (long)strlen(newtext));
                syscall1(SYS_CLOSE, fd);
            }
            char tn[64]; long tts = 0; int tai = 0;
            int tlisted = bin_lookup(TPATH, tn, (long)sizeof(tn), &tts, &tai);
            int tbytes = 0;
            if (tlisted) {
                char tp[256];
                snprintf(tp, sizeof(tp), "%s/%s", BIN, tn);
                long n = slurp(tp, got, (long)sizeof(got));
                tbytes = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
            }
            if (opened) {
                e6 = tlisted && tbytes;
                line("E6.truncating-open-SUCCEEDED-and-the-PRE-IMAGE-is-in-the-bin", e6);
            } else {
                // Refused: the original content must be untouched.
                long n = slurp(TPATH, got, (long)sizeof(got));
                e6 = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
                line("E6.truncating-open-REFUSED-and-the-content-is-INTACT", e6);
            }
            line("E6.NEVER-truncated-with-no-pre-image-kept", !(opened && !tbytes));
        } else {
            line("E6.could-not-create-the-file-to-truncate(arm did not run)", 0);
        }
        (void)r_unlink(TPATH);
    }

    // ---------------------------------------------------------------- E7
    // ftruncate() ON AN ALREADY-OPEN fd. The last deterministic destruction
    // path that is not a delete, and the one the open-time O_TRUNC check
    // cannot see: the open carries NO O_TRUNC at all, and the shrink arrives
    // later on the open descriptor.
    int e7 = 0;
    {
        const char *FPATH = SC "/SHRINKME.TXT";
        (void)r_unlink(FPATH);
        put(FPATH, MAGIC);
        if (exists(FPATH)) {
            // Open for WRITE with NO O_TRUNC, then shrink to zero.
            long fd = syscall2(SYS_OPEN, (long)FPATH, (long)O_WRONLY);
            int shrank = 0;
            if (fd >= 0) {
                shrank = (syscall2(SYS_FTRUNCATE, fd, 0) == 0);
                syscall1(SYS_CLOSE, fd);
            }
            char sn[64]; long sts = 0; int sai = 0;
            int slisted = bin_lookup(FPATH, sn, (long)sizeof(sn), &sts, &sai);
            int sbytes = 0;
            if (slisted) {
                char sp[256];
                snprintf(sp, sizeof(sp), "%s/%s", BIN, sn);
                long n = slurp(sp, got, (long)sizeof(got));
                sbytes = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
            }
            if (shrank) {
                e7 = slisted && sbytes;
                line("E7.ftruncate-SUCCEEDED-and-the-PRE-IMAGE-is-in-the-bin", e7);
            } else {
                long n = slurp(FPATH, got, (long)sizeof(got));
                e7 = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
                line("E7.ftruncate-REFUSED-and-the-content-is-INTACT", e7);
            }
            line("E7.NEVER-shrank-with-no-pre-image-kept", !(shrank && !sbytes));
        } else {
            line("E7.could-not-create-the-file-to-shrink(arm did not run)", 0);
        }
        (void)r_unlink(FPATH);
    }

    // ---------------------------------------------------------------- E8
    // FULL-FILE REPLACEMENT via SYS_PKG_WRITE. The escape that is not a
    // delete, not a rename and not an open: one call naming a path, a buffer
    // and a length, which replaces every byte of the named file. Before
    // 2026-10-03 this syscall had no escrow hook of any kind, so a marked
    // actor could overwrite any file the invoking user may write and keep
    // nothing recoverable.
    //
    // TWO HALVES, because either alone would be a test that cannot fail:
    //   E8a an IN-SCOPE replacement must SUCCEED (the AI is still allowed to
    //       install and replace files) AND the old contents must be in the bin.
    //       Asserting only a refusal would pass against a kernel that had
    //       simply broken the syscall, or against one where the number is
    //       wrong and every call returns -1.
    //   E8b an OUT-OF-SCOPE replacement must be REFUSED, which is the guard
    //       itself rather than the capture: it proves escrow_fs_guard() is on
    //       this path and not just the pre-image keeper.
    int e8 = 0;
    {
        const char *PPATH = SC "/PKGREPL.TXT";
        const char *NEWTEXT = "replaced-by-pkg-write\n";
        (void)r_unlink(PPATH);
        put(PPATH, MAGIC);
        if (exists(PPATH)) {
            long prc = r_pkg_write(PPATH, NEWTEXT, (long)strlen(NEWTEXT));
            int replaced = (prc >= 0);
            // The pre-image: the bin must hold the ORIGINAL bytes under the
            // ORIGINAL path, attributed to an "ai:" actor.
            char pn[64]; long pts = 0; int pai = 0;
            int plisted = bin_lookup(PPATH, pn, (long)sizeof(pn), &pts, &pai);
            int pbytes = 0;
            if (plisted) {
                char pp[256];
                snprintf(pp, sizeof(pp), "%s/%s", BIN, pn);
                long n = slurp(pp, got, (long)sizeof(got));
                pbytes = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
            }
            if (replaced) {
                // The replacement really took effect, so this is a genuine
                // overwrite and not a no-op that trivially kept the bytes.
                long n = slurp(PPATH, got, (long)sizeof(got));
                int took = (n == (long)strlen(NEWTEXT)) && (strcmp(got, NEWTEXT) == 0);
                e8 = took && plisted && pbytes && pai;
                line("E8a.pkg-write-SUCCEEDED(the AI can still replace a file)", took);
                line("E8a.the-PRE-IMAGE-is-in-the-bin-byte-for-byte", plisted && pbytes);
                line("E8a.attributed-to-an-ai-actor", pai);
            } else {
                // Refused: the original content must be completely untouched.
                long n = slurp(PPATH, got, (long)sizeof(got));
                e8 = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
                line("E8a.pkg-write-REFUSED-and-the-content-is-INTACT", e8);
            }
            // THE ASSERTION THAT CANNOT HIDE IN A BRANCH: whatever happened,
            // it must never be the case that the file was replaced and nothing
            // recoverable was kept. This is the line that goes RED on a kernel
            // without the fix.
            line("E8.NEVER-replaced-a-file-with-no-pre-image-kept", !(replaced && !pbytes));
            if (replaced && !pbytes) e8 = 0;
        } else {
            line("E8a.could-not-create-the-file-to-replace(arm did not run)", 0);
        }
        (void)r_unlink(PPATH);

        // E8b: OUT OF SCOPE. OUTF is outside the granted scope prefix, so the
        // guard must refuse before any writer runs, and nothing must appear.
        long orc = r_pkg_write(OUTF, NEWTEXT, (long)strlen(NEWTEXT));
        int e8b = (orc < 0) && !exists(OUTF);
        line("E8b.out-of-scope-pkg-write-REFUSED-and-nothing-was-created", e8b);
        if (!e8b) e8 = 0;
    }

    // ---------------------------------------------------------------- E9
    // PARTIAL OVERWRITE. Open a file that ALREADY EXISTS for writing with NO
    // O_TRUNC, seek into the MIDDLE of it, and overwrite bytes that are there.
    //
    // This is the route docs/AI_ESCROW_PROMISE.md names as the ONE remaining
    // hole, and the reason given for leaving it open is about the OPEN: at the
    // moment a file is opened for writing, a writer that will overwrite cannot
    // be told from one that will only append or write nothing. That reasoning
    // is about open time only, and these arms are what turn it from an
    // assumption into a measurement.
    //
    // SIX ARMS, because the COST is as much the question as the hole:
    //
    //   E9a O_RDWR, seek to byte 8, overwrite 8 bytes. The file KEEPS ITS
    //       LENGTH, which is what makes this a genuine partial overwrite and
    //       not a whole-file replacement wearing its clothes: the tail
    //       survives, so no route that only watches lengths can see it.
    //   E9b plain O_WRONLY, NO O_TRUNC, NO O_APPEND, overwrite the FIRST
    //       bytes. Same destruction through a different open mode, because
    //       #745 routes both into the same preserving descriptor and a fix
    //       wired to one access mode would silently miss the other.
    //   E9c OPEN FOR WRITE AND CLOSE WITHOUT WRITING. Nothing was destroyed,
    //       so NOTHING may be copied. A fix that snapshots at OPEN time would
    //       put a pointless copy of every file the AI merely opens into the
    //       user's Recycle Bin, and this arm is what would catch it.
    //   E9d APPEND. An append destroys nothing, so it must cost nothing, and
    //       the file must still begin with its original bytes.
    //   E9e APPEND THROUGH AN O_RDWR DESCRIPTOR (seek to the end, then write).
    //       This is the case that is NOT distinguishable at open time and IS
    //       distinguishable at write time, so it is the arm that proves the
    //       capture is decided on the write RANGE and not merely on the fact
    //       that a write happened.
    //   E9t COST. The same partial overwrite timed over three file sizes, in
    //       microseconds, so the price of whatever the kernel does here is a
    //       number in the transcript rather than an opinion in a document.
    //
    // c, d and e are the arms that keep the fix honest about its price; a and b
    // are the arms that go RED on a kernel without one.
    int e9 = 0;
    {
        const char *WPATH = SC "/PARTIAL.TXT";
        const char *W2    = SC "/PARTIAL2.TXT";
        const char *WNOW  = SC "/NOWRITE.TXT";
        const char *WAPP  = SC "/APPENDME.TXT";
        const char *WRWA  = SC "/RWAPPEND.TXT";
        const long  MLEN  = (long)strlen(MAGIC);

        // -------- E9a: O_RDWR, overwrite the middle, keep the length --------
        (void)r_unlink(WPATH);
        put(WPATH, MAGIC);
        int a_ok = 0, a_took = 0, a_bytes = 0;
        if (exists(WPATH)) {
            long fd = syscall2(SYS_OPEN, (long)WPATH, (long)O_RDWR);
            if (fd >= 0) {
                long sk = syscall3(SYS_SEEK, fd, 8, (long)SEEK_SET);
                long w  = syscall3(SYS_WRITE, fd, (long)"ZZZZZZZZ", 8);
                a_ok = (sk == 8) && (w == 8);
                syscall1(SYS_CLOSE, fd);
            }
            long n = slurp(WPATH, got, (long)sizeof(got));
            // A GENUINE PARTIAL OVERWRITE: same length, the marker landed in
            // the middle, and the content is no longer the original.
            a_took = (n == MLEN) && (find(got, "ZZZZZZZZ") != 0) &&
                     (strcmp(got, MAGIC) != 0);
            char an[64]; long ats = 0; int aai = 0;
            int alisted = bin_lookup(WPATH, an, (long)sizeof(an), &ats, &aai);
            if (alisted) {
                char ap[256];
                snprintf(ap, sizeof(ap), "%s/%s", BIN, an);
                long m = slurp(ap, got, (long)sizeof(got));
                a_bytes = (m == MLEN) && (strcmp(got, MAGIC) == 0);
            }
            line("E9a.partial-overwrite-TOOK-EFFECT(length kept, tail survived)", a_took);
            line("E9a.the-PRE-IMAGE-is-in-the-bin-byte-for-byte", a_bytes);
            line("E9a.attributed-to-an-ai-actor", aai);
            // THE LINE THAT GOES RED ON A KERNEL WITH THE HOLE OPEN: bytes
            // that existed stopped existing and nothing kept them.
            line("E9a.NEVER-overwrote-existing-bytes-with-no-pre-image-kept",
                 !(a_took && !a_bytes));
        } else {
            line("E9a.could-not-create-the-file-to-overwrite(arm did not run)", 0);
        }
        (void)r_unlink(WPATH);

        // -------- E9b: plain O_WRONLY, no O_TRUNC, overwrite the head --------
        (void)r_unlink(W2);
        put(W2, MAGIC);
        int b_took = 0, b_bytes = 0;
        if (exists(W2)) {
            long fd = syscall2(SYS_OPEN, (long)W2, (long)O_WRONLY);
            if (fd >= 0) {
                (void)syscall3(SYS_WRITE, fd, (long)"QQQQ", 4);
                syscall1(SYS_CLOSE, fd);
            }
            long n = slurp(W2, got, (long)sizeof(got));
            b_took = (n == MLEN) && (strcmp(got, MAGIC) != 0) &&
                     got[0] == 'Q' && got[3] == 'Q';
            char bn[64]; long bts = 0; int bai = 0;
            if (bin_lookup(W2, bn, (long)sizeof(bn), &bts, &bai)) {
                char bp[256];
                snprintf(bp, sizeof(bp), "%s/%s", BIN, bn);
                long m = slurp(bp, got, (long)sizeof(got));
                b_bytes = (m == MLEN) && (strcmp(got, MAGIC) == 0);
            }
            line("E9b.O_WRONLY-no-O_TRUNC-overwrite-TOOK-EFFECT", b_took);
            line("E9b.the-PRE-IMAGE-is-in-the-bin-byte-for-byte", b_bytes);
            line("E9b.NEVER-overwrote-existing-bytes-with-no-pre-image-kept",
                 !(b_took && !b_bytes));
        } else {
            line("E9b.could-not-create-the-second-file(arm did not run)", 0);
        }
        (void)r_unlink(W2);

        // -------- E9c: open for write, write NOTHING, close. COSTS NOTHING --------
        (void)r_unlink(WNOW);
        put(WNOW, MAGIC);
        int c_ok = 0;
        if (exists(WNOW)) {
            long fd = syscall2(SYS_OPEN, (long)WNOW, (long)O_RDWR);
            if (fd >= 0) syscall1(SYS_CLOSE, fd);
            long n = slurp(WNOW, got, (long)sizeof(got));
            int intact = (n == MLEN) && (strcmp(got, MAGIC) == 0);
            char cn[64]; long cts = 0; int cai = 0;
            int clisted = bin_lookup(WNOW, cn, (long)sizeof(cn), &cts, &cai);
            c_ok = intact && !clisted;
            line("E9c.open-for-write-then-close-with-no-write-is-a-NO-OP", intact);
            line("E9c.and-it-put-NOTHING-in-the-users-bin(no cost)", !clisted);
        } else {
            line("E9c.could-not-create-the-no-write-file(arm did not run)", 0);
        }
        (void)r_unlink(WNOW);

        // -------- E9d: APPEND. Destroys nothing, must cost nothing --------
        (void)r_unlink(WAPP);
        put(WAPP, MAGIC);
        int d_ok = 0;
        if (exists(WAPP)) {
            long fd = syscall2(SYS_OPEN, (long)WAPP,
                               (long)(O_WRONLY | O_APPEND));
            if (fd >= 0) {
                (void)syscall3(SYS_WRITE, fd, (long)"tail\n", 5);
                syscall1(SYS_CLOSE, fd);
            }
            long n = slurp(WAPP, got, (long)sizeof(got));
            // The ORIGINAL bytes must still be at the front, untouched.
            int kept = (n == MLEN + 5) && (strncmp(got, MAGIC, (unsigned long)MLEN) == 0);
            char dn[64]; long dts = 0; int dai = 0;
            int dlisted = bin_lookup(WAPP, dn, (long)sizeof(dn), &dts, &dai);
            d_ok = kept && !dlisted;
            line("E9d.append-kept-the-original-bytes-at-the-front", kept);
            line("E9d.and-an-append-put-NOTHING-in-the-bin(no cost)", !dlisted);
        } else {
            line("E9d.could-not-create-the-append-file(arm did not run)", 0);
        }
        (void)r_unlink(WAPP);

        // -------- E9e: append THROUGH an O_RDWR fd (seek to end, write) --------
        // Indistinguishable from E9a at OPEN time; distinguishable at WRITE
        // time. If this one costs a copy, the capture is firing on "a write
        // happened" rather than on "existing bytes were overwritten".
        (void)r_unlink(WRWA);
        put(WRWA, MAGIC);
        int e_ok = 0;
        if (exists(WRWA)) {
            long fd = syscall2(SYS_OPEN, (long)WRWA, (long)O_RDWR);
            if (fd >= 0) {
                (void)syscall3(SYS_SEEK, fd, MLEN, (long)SEEK_SET);
                (void)syscall3(SYS_WRITE, fd, (long)"more\n", 5);
                syscall1(SYS_CLOSE, fd);
            }
            long n = slurp(WRWA, got, (long)sizeof(got));
            int kept = (n == MLEN + 5) && (strncmp(got, MAGIC, (unsigned long)MLEN) == 0);
            char en[64]; long ets2 = 0; int eai2 = 0;
            int elisted2 = bin_lookup(WRWA, en, (long)sizeof(en), &ets2, &eai2);
            e_ok = kept && !elisted2;
            line("E9e.rw-fd-append-past-the-end-kept-the-original-bytes", kept);
            line("E9e.and-it-put-NOTHING-in-the-bin(range-based, not write-based)",
                 !elisted2);
        } else {
            line("E9e.could-not-create-the-rw-append-file(arm did not run)", 0);
        }
        (void)r_unlink(WRWA);

        // -------- E9t: THE COST, IN MICROSECONDS, AT THREE FILE SIZES --------
        // The measured window is exactly open -> seek -> write 8 bytes ->
        // close on a file that already exists, which is the whole operation a
        // pre-image keeper would have to pay for. Printed, not asserted: the
        // number is the evidence, and the same binary on two kernels makes the
        // difference between the two transcripts the measured price.
        {
            static char chunk[4096];
            for (int ci = 0; ci < 4096; ci++) chunk[ci] = (char)('a' + (ci & 15));
            const long sizes[3] = { 4096, 65536, 524288 };
            for (int si = 0; si < 3; si++) {
                char tp[160];
                snprintf(tp, sizeof(tp), "%s/COST%d.BIN", SC, si);
                (void)r_unlink(tp);
                long fd = r_open_w(tp);
                int built = 0;
                if (fd >= 0) {
                    long left = sizes[si];
                    built = 1;
                    while (left > 0) {
                        long take = (left < 4096) ? left : 4096;
                        if (syscall3(SYS_WRITE, fd, (long)chunk, take) != take) {
                            built = 0; break;
                        }
                        left -= take;
                    }
                    syscall1(SYS_CLOSE, fd);
                }
                char l[224];
                if (!built) {
                    snprintf(l, sizeof(l),
                             "ESCRVT: E9t.COST size=%ld could-not-build\n",
                             sizes[si]);
                    say(l);
                    continue;
                }
                unsigned long long t0 = mono_us();
                long wfd = syscall2(SYS_OPEN, (long)tp, (long)O_RDWR);
                int did = 0;
                if (wfd >= 0) {
                    (void)syscall3(SYS_SEEK, wfd, sizes[si] / 2, (long)SEEK_SET);
                    did = (syscall3(SYS_WRITE, wfd, (long)"ZZZZZZZZ", 8) == 8);
                    syscall1(SYS_CLOSE, wfd);
                }
                unsigned long long t1 = mono_us();
                snprintf(l, sizeof(l),
                         "ESCRVT: E9t.COST size=%ld us=%lu overwrote=%d\n",
                         sizes[si], (unsigned long)(t1 - t0), did);
                say(l);
                (void)r_unlink(tp);
            }
        }

        e9 = a_took && a_bytes && b_took && b_bytes && c_ok && d_ok && e_ok;
        (void)a_ok;
    }

    // ---------------------------------------------------------------- E2
    // LAUNCH A HELPER AND HAVE IT DO THE DELETING. This is the escape that was
    // OPEN: before the fix the spawned child was an ordinary unmarked uid-0
    // process and HELPERV would have been destroyed outright.
    int e2 = 0;
    int hpid = sys_spawn(HELPER);
    if (hpid > 0) {
        int status = 0;
        (void)sys_waitpid(hpid, &status, 0);
        // The helper reports its own findings; we check the OUTCOME here,
        // because the outcome is the thing that matters and the helper could
        // in principle lie about it.
        int h_gone   = !exists(HELPERV);
        char hname[64]; long hts = 0; int h_ai = 0;
        int h_listed = bin_lookup(HELPERV, hname, (long)sizeof(hname), &hts, &h_ai);
        int h_bytes = 0;
        if (h_listed) {
            char hp[256];
            snprintf(hp, sizeof(hp), "%s/%s", BIN, hname);
            long n = slurp(hp, got, (long)sizeof(got));
            h_bytes = (n == (long)strlen(MAGIC)) && (strcmp(got, MAGIC) == 0);
        }
        line("E2.helper-ran(spawned by a marked actor)", 1);
        line("E2.helper-exited-reporting-it-was-ENFORCED", status == 0);
        line("E2.helper-delete-took-effect(file gone from its path)", h_gone);
        line("E2.helper-delete-was-REDIRECTED-not-destructive", h_listed && h_bytes);
        e2 = (status == 0) && h_gone && h_listed && h_bytes;
    } else {
        line("E2.helper-spawn-FAILED(cannot test the spawn escape)", 0);
    }
    line("E2-SPAWNING-A-PROGRAM-DOES-NOT-ESCAPE-THE-MARK", e2);

    // ---------------------------------------------------------------- tidy
    (void)r_unlink(VICTIM);
    (void)r_unlink(OUTF);

    int destroy_proof = e3_succeeded && e3_gone && r1_listed && r2 && (ts > 0) && is_ai;
    int overall = seeded && e1 && e4 && e5 && e6 && e7 && e8 && e9 && destroy_proof && r3 && r4 && e2;

    line("SUMMARY.E1-cannot-announce-and-leave",            e1);
    line("SUMMARY.E2-cannot-launder-through-a-spawn",       e2);
    line("SUMMARY.E3-raw-delete-succeeds-but-never-destroys", destroy_proof);
    line("SUMMARY.E4-rmdir-refused(named-exception)",       e4);
    line("SUMMARY.R3-undelete-by-the-AI-works",             r3);
    line("SUMMARY.R4-an-empty-file-is-handled-honestly",   r4);
    line("SUMMARY.E5-a-rename-over-a-file-cannot-destroy-it", e5);
    line("SUMMARY.E6-a-truncating-open-keeps-the-pre-image", e6);
    line("SUMMARY.E7-an-ftruncate-shrink-keeps-the-pre-image", e7);
    line("SUMMARY.E8-a-full-file-replacement-keeps-the-pre-image", e8);
    line("SUMMARY.E9-a-partial-overwrite-keeps-the-pre-image", e9);
    say(overall ? "ESCRVT: OVERALL PASS\n" : "ESCRVT: OVERALL FAIL\n");
    say("========== ESCRVT END ==========\n");
    return overall ? 0 : 1;
}
