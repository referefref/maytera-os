// recycled - the MayteraOS Recycle Bin recycler service (#aidelete).
//
// THE REQUIREMENT, verbatim from the owner on 2026-10-03: "A recycler service
// should auto delete these periodically with a controllable timing in the
// settings app, also with an option for never but defaulting to 30 days".
//
// WHAT IT DOES. Every 30 minutes, and once at startup, it reads
// <home>/CONFIG/RECYCLE.CFG, walks <home>/CONFIG/RBINDEX.TXT, and permanently
// removes each bin entry whose recorded deletion time is older than the
// retention setting. It writes a line to the serial console and appends to
// <home>/CONFIG/RCYCLOG.TXT for everything it removes and for everything it
// deliberately keeps.
//
// WHAT IT WILL NOT DO, and each of these is a rule rather than an accident:
//
//   - NEVER MEANS NEVER. retention_days = never (or 0) and the sweep removes
//     nothing at all. It does not "tidy up a bit"; it returns.
//   - AN UNKNOWN AGE IS NEVER PURGED. A V1 index row (the two-field form that
//     shipped) carries no timestamp, so rbindex_parse_row() reports
//     deleted_unix = 0, and rbindex_is_expired() keeps it forever. An existing
//     bin on an installed machine is therefore SAFE on the first boot after
//     this change, rather than suddenly eligible for deletion. The kernel's
//     rcy_is_expired_rs() resolves the same way, and that agreement is the
//     point of the shared rule living in libc/rbindex.c.
//   - A FILE WITH NO INDEX ROW IS NEVER PURGED. It has no age to judge and no
//     recorded origin, so it is reported, not removed.
//   - IT DOES NOT PRUNE "ORPHAN" ROWS whose file is already gone. It is
//     tempting and it is wrong to do blindly: the row is the only record of
//     where that file came from, and this service is not the only writer of
//     the bin. They are reported instead.
//
// THE ORDER OF THE TWO DESTRUCTIVE STEPS, AND WHY.
//
// A purge is two operations: unlink the file in the bin, and drop its row from
// the index. A crash can land between them, so the order decides which torn
// state a user wakes up to.
//
//   FILE FIRST, THEN ROW (what this does). A crash leaves an ORPHAN ROW: an
//   index line naming a file that no longer exists. The Recycle Bin lists the
//   DIRECTORY and looks each name up in the index, so an orphan row is
//   invisible, harmless, and costs one line of text. Nothing is unrestorable
//   because nothing restorable is left.
//
//   ROW FIRST, THEN FILE (rejected). A crash leaves a file in the bin with no
//   record of where it came from. The Recycle Bin shows it as "(unknown)" and
//   Restore cannot run, so a file the user could have recovered a moment
//   earlier becomes permanently unrecoverable while still occupying the disk.
//   That is the exact failure class #742 was about.
//
// So the safer order is file-then-row, and it is the opposite of the ordering
// a capture uses (#742: the file must be IN the bin before its row is written,
// because a row with no file is better than a file with no row). Both follow
// from one rule: never create a window in which something restorable has no
// way to be restored.
//
// NO BUSY-WAIT. The interval is one sys_sleep(), which is the kernel timed
// sleep (SYS_SLEEP), not a yield loop and not a poll. CLAUDE.md's wait rule
// bans a hand-rolled proc_sleep/proc_yield poll; a service whose whole job is
// to run twice an hour is the one case where sleeping for a fixed period IS
// the correct semantics rather than a workaround for a wake it failed to arm.
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"
#include "../../libc/unistd.h"
#include "../../libc/stdlib.h"
#include "../../libc/time.h"
#include "../../libc/dirent.h"
#include "../../libc/userconf.h"
#include "../../libc/rbindex.h"

#define SWEEP_INTERVAL_MS (30u * 60u * 1000u)   // 30 minutes
#define MAX_PURGE_PER_SWEEP 128                 // bounded work per pass

typedef struct {
    int rows;            // index rows examined
    int purged;           // file AND row removed
    int kept_unknown;     // deleted_unix == 0, age unknown
    int kept_young;       // dated, not yet expired
    int failed;           // unlink or row removal refused
    int orphan_rows;      // row whose file is already gone
    int files_no_row;     // file in the bin with no index row
} rcyd_stats_t;

// ---------------------------------------------------------------------------
// Logging. Deliberately not printf: this process never exits, so anything that
// sits in a stdio buffer waiting for __libc_fini is a log that is never read.
// Each line goes straight to fd 1 (serial) and is appended to
// <home>/CONFIG/RCYCLOG.TXT, so a purge is still explainable after a reboot.
// ---------------------------------------------------------------------------
#define LOGCAP 240
static char g_log[LOGCAP];
static int  g_loglen;

static void lg_reset(void) { g_loglen = 0; g_log[0] = 0; }
static void lg_s(const char *s) {
    if (!s) s = "(null)";
    while (*s && g_loglen < LOGCAP - 2) g_log[g_loglen++] = *s++;
    g_log[g_loglen] = 0;
}
static void lg_u(unsigned long v) {
    char tmp[24]; int n = 0;
    if (v == 0) { if (g_loglen < LOGCAP - 2) g_log[g_loglen++] = '0'; g_log[g_loglen] = 0; return; }
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n > 0 && g_loglen < LOGCAP - 2) g_log[g_loglen++] = tmp[--n];
    g_log[g_loglen] = 0;
}
static void lg_flush(void) {
    if (g_loglen < LOGCAP - 1) { g_log[g_loglen++] = '\n'; g_log[g_loglen] = 0; }
    sys_write(1, g_log, (unsigned long)g_loglen);
    {
        char p[256];
        if (userhome_path_session("CONFIG", "RCYCLOG.TXT", p, sizeof(p)) == 0) {
            int fd = open(p, 0x41);   // O_WRONLY|O_CREAT, append by seek
            if (fd >= 0) {
                if (sys_seek(fd, 0, SEEK_END) >= 0)
                    (void)write(fd, g_log, (unsigned long)g_loglen);
                close(fd);
            }
        }
    }
    lg_reset();
}

// ---------------------------------------------------------------------------
// One sweep.
//
// `now` and `retention_days` are PARAMETERS rather than being read inside, so
// the sweep is exercisable at any age and any policy without touching a clock
// or a config file. That is what tools/trash-harness/th_recycled.c drives.
// Returns the number of entries purged.
// ---------------------------------------------------------------------------
static int rcyd_sweep(unsigned long now, unsigned retention_days, rcyd_stats_t *st)
{
    char doomed[MAX_PURGE_PER_SWEEP][RB_NAME_MAX];
    int  ndoomed = 0;
    int  i, n;

    if (st) {
        st->rows = st->purged = st->kept_unknown = st->kept_young = 0;
        st->failed = st->orphan_rows = st->files_no_row = 0;
    }

    // NEVER MEANS NEVER: no scan, no stat, no unlink. Returning here rather
    // than relying on rbindex_is_expired() to say no for every row is
    // deliberate belt and braces, and it means the log says plainly that the
    // sweep did nothing because the user asked for nothing.
    if (retention_days == RB_RETENTION_NEVER) return 0;

    rbindex_load();
    n = rbindex_count();
    if (st) st->rows = n;

    for (i = 0; i < n && ndoomed < MAX_PURGE_PER_SWEEP; i++) {
        const rb_row_t *row = rbindex_at(i);
        if (!row) continue;
        if (row->deleted_unix == 0) { if (st) st->kept_unknown++; continue; }
        if (!rbindex_is_expired(row->deleted_unix, now, retention_days)) {
            if (st) st->kept_young++;
            continue;
        }
        // Collected first, acted on second: rbindex_remove() rewrites the
        // in-memory index, which would renumber the rows underneath this loop.
        {
            int k = 0;
            while (row->name[k] && k < RB_NAME_MAX - 1) { doomed[ndoomed][k] = row->name[k]; k++; }
            doomed[ndoomed][k] = 0;
            ndoomed++;
        }
    }

    for (i = 0; i < ndoomed; i++) {
        char p[320];
        int present;
        rbindex_join(p, (int)sizeof(p), rbindex_dir(), doomed[i]);
        {
            int fd = open(p, 0);
            present = (fd >= 0);
            if (fd >= 0) close(fd);
        }
        if (!present) {
            // The file is already gone: the row is an orphan. Reported, and the
            // row IS dropped in this one case, because there is nothing left
            // for it to describe and nothing it could make restorable.
            if (st) st->orphan_rows++;
            lg_s("[recycled] orphan row (file already gone), dropping row: ");
            lg_s(doomed[i]); lg_flush();
            if (rbindex_remove(doomed[i]) != 0 && st) st->failed++;
            continue;
        }
        // FILE FIRST. See the ordering note at the top of this file.
        if (unlink(p) != 0) {
            if (st) st->failed++;
            lg_s("[recycled] could NOT remove expired item, keeping its row: ");
            lg_s(doomed[i]); lg_flush();
            continue;
        }
        // THEN THE ROW. A failure here leaves an orphan row, which the next
        // sweep reports and drops; the file is already gone either way, so the
        // user has lost nothing they could have restored.
        if (rbindex_remove(doomed[i]) != 0) {
            if (st) st->failed++;
            lg_s("[recycled] purged the file but could not drop its index row: ");
            lg_s(doomed[i]); lg_flush();
            continue;
        }
        if (st) st->purged++;
        lg_s("[recycled] purged "); lg_s(doomed[i]);
        lg_s(" (retention "); lg_u(retention_days); lg_s(" days)");
        lg_flush();
    }

    // Report bin files that no index row names. They are never purged (no age,
    // no origin), but a bin quietly accumulating unrestorable files is worth
    // saying out loud.
    if (st) {
        dirent_t e;
        for (i = 0; i < 1024; i++) {
            rb_row_t r;
            if (sys_readdir(rbindex_dir(), i, &e) != 0) break;
            if (e.name[0] == '.') continue;
            if (DIRENT_IS_DIR(e)) continue;
            if (!rbindex_lookup(e.name, &r)) st->files_no_row++;
        }
    }
    return st ? st->purged : 0;
}

#ifndef RECYCLED_NO_MAIN
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    // A service runs as uid 0 while the bin it must sweep belongs to the
    // SESSION user, so its own uid would answer "/" for ever. This is the one
    // supported way to ask for the session's home instead, and it must come
    // before the first rbindex_dir() because that call caches.
    rbindex_use_session_home();

    lg_s("[recycled] Recycle Bin recycler starting; bin=");
    lg_s(rbindex_dir());
    lg_s(" cfg="); lg_s(rbindex_cfg_path());
    lg_flush();

    for (;;) {
        unsigned retention = rbindex_read_retention();   // re-read EVERY sweep,
                                                         // so a Settings change
                                                         // needs no restart
        time_t now = time(0);
        rcyd_stats_t st;

        if (retention == RB_RETENTION_NEVER) {
            lg_s("[recycled] retention is Never; nothing is removed");
            lg_flush();
        } else if (now <= 0) {
            // No clock, no ages, no purge. Guessing would mean treating every
            // item as epoch-old and deleting the lot on the first sweep.
            lg_s("[recycled] the clock is not set; skipping this sweep");
            lg_flush();
        } else {
            rcyd_sweep((unsigned long)now, retention, &st);
            lg_s("[recycled] sweep: "); lg_u((unsigned long)st.rows);
            lg_s(" rows, purged "); lg_u((unsigned long)st.purged);
            lg_s(", kept "); lg_u((unsigned long)st.kept_young);
            lg_s(" not yet expired, kept "); lg_u((unsigned long)st.kept_unknown);
            lg_s(" with no recorded date, "); lg_u((unsigned long)st.failed);
            lg_s(" failed, "); lg_u((unsigned long)st.orphan_rows);
            lg_s(" orphan rows, "); lg_u((unsigned long)st.files_no_row);
            lg_s(" files with no row");
            lg_flush();
        }

        sys_sleep(SWEEP_INTERVAL_MS);
    }
    return 0;
}
#endif
