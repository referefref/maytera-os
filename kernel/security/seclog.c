// seclog.c - persist security events and surface them on the desktop (#653).
//
// THE GAP THIS CLOSES. security_audit() has always existed, but until #653 it
// had ZERO CALLERS anywhere in the kernel, and everything it would have
// recorded died with the boot anyway: a 64-entry in-memory ring plus a kprintf
// to serial. Nothing reached disk and nothing reached the user.
//
// CORRECTION, recorded because the first version of this comment got it wrong:
// an earlier draft asserted security_audit() "is called from real security
// paths". That was written from the task description, not from checking, and
// it was false - `grep -rn "security_audit("` matched only its own definition
// and this file's comments. A sink with no producer is decorative, which is
// the #514/#624/#646 failure this repo keeps repeating. proc/syscall.c now
// emits AUDIT_PTR_INVALID when syscall_validate_args() rejects a bad user
// pointer, so there is a real, Ring-3-reachable producer.
//
// A security event nobody can see after a reboot is not an audit trail.
//
// This adds two sinks, both fed from the SAME ring so there is exactly one
// producer API (security_audit) and no second path to keep in sync:
//   1. /CONFIG/SECURITY.LOG  - persistent, survives reboot, readable by the
//                              syslog app and by anything else on the box.
//   2. /CONFIG/NOTIFY.TXT    - the existing notification spool (#168), so
//                              CRITICAL and WARNING events raise a desktop
//                              toast and land in the notification centre.
//
// WHY A WORKER THREAD, AND NOT JUST WRITING IN security_audit().
// This is the whole design constraint. security_audit() is called from
// contexts that MUST NOT BLOCK: syscall validation failures, pointer checks,
// and fault paths, some with interrupts off or a spinlock held. Filesystem I/O
// blocks. Doing the write inline would be exactly the banned pattern that
// wq_assert_may_block() exists to catch (kernel/sync/noblock.c), and it would
// deadlock or trip [WQBLOCK] the first time a fault path logged an event.
//
// So security_audit() stays non-blocking: ring write + kprintf + a wake. All
// I/O happens on this PRIO_LOW worker, which sleeps on a wait-queue and is the
// only thing that touches the filesystem. Per CLAUDE.md's preference order
// this is case 2-adjacent but better: the wake source is entirely ours (every
// security_audit call kicks it), so there is no lost-wake window and no
// timeout is needed - plain wait_event() is correct here, not
// wait_event_timeout().
//
// SEQUENCE-BASED DRAIN, NOT A FLAG. The worker tracks the last audit sequence
// number it wrote. If the ring wrapped while it was asleep (64 events between
// drains), it detects the gap and records that events were LOST rather than
// silently writing whatever happens to be in the ring now. Silently losing
// security events while looking healthy is the failure mode worth spending
// eight lines to avoid.

// NOTE: do NOT include <stdint.h>/<stddef.h> here. This is a freestanding
// -nostdinc kernel: it defines its own uint64_t/size_t in types.h, and pulling
// the toolchain headers in produces "conflicting types" for every one of them.
// Follow security.c: kernel headers only.
#include "security.h"
#include "seclog.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../sync/waitq.h"
#include "../proc/process.h"
/* #653: fs/fat.h, not fs/netfs.h. fat_read_file/fat_write_file route to
 * ext2 when the path is on the ext2 root (fs/fat.c:186, fs/ext2.c:3449);
 * netfs.h's vfs_write_file only knows NFS and FAT and silently fails on
 * an ext2-root path. See the header comment above. */
#include "../fs/fat.h"

/* g_fat_fs has no declaration in any header - gui/theme.c, games/doom and every
 * other user externs it locally, so this matches the existing convention rather
 * than adding a 4th place that could drift. It is the single mounted volume;
 * fat_read_file/fat_write_file route off it to ext2 when the path lives there. */
extern fat_fs_t g_fat_fs;

extern void *kmalloc(size_t);
extern void kfree(void *);

// Rust record formatter (see rustkern.rs). New kernel logic goes in Rust per
// the standing rule; this is the pure part (bytes in, bytes out, no I/O).
extern unsigned long sec_fmt_record(unsigned int event, unsigned int pid,
                                    unsigned long ts,
                                    const unsigned char *detail, unsigned long detail_len,
                                    unsigned char *out, unsigned long out_cap);
extern unsigned int sec_event_severity(unsigned int event);
extern const unsigned char *sec_event_name(unsigned int event);

#define SECLOG_PATH   "/CONFIG/SECURITY.LOG"
// #683: the notification spool is per-SESSION state, not system state, and it
// moved into the logged-in user's home so the compositor can drain it without
// write access to /etc. Ring 0 is never denied either path; what matters is
// that this resolves to the SAME file libc/notify.c writes and the compositor
// drains. NOTIFY_SPOOL_NAME is the leaf; userconf_kpath() supplies the rest.
#include "../fs/userconf.h"
#define NOTIFY_SPOOL_NAME  "NOTIFY.TXT"
#define SECLOG_CAP    32768      // ~400 events; trimmed from the FRONT when full
#define SPOOL_CAP     8000       // must match userland/libc/notify.c
#define AUDIT_RING    64         // must match AUDIT_LOG_SIZE in security.c

static wait_queue_head_t g_seclog_wq;
static volatile int      g_seclog_ready = 0;
static uint64_t          g_seclog_drained = 0;

static char   g_log[SECLOG_CAP];
static size_t g_log_len = 0;

// Called from security_audit(). MUST be safe from any context: no allocation,
// no I/O, no lock we could already hold. wake_up_all on an empty queue is a
// no-op, so this is safe before the worker exists too.
void seclog_kick(void) {
    if (!g_seclog_ready) return;
    wake_up_all(&g_seclog_wq);
}

static int seclog_pending(void) {
    return security_audit_seq() != g_seclog_drained;
}

// Append to the in-memory log, trimming whole lines off the front if needed so
// the file never grows without bound and never ends up half a record.
static void log_append(const char *s, size_t n) {
    if (n >= SECLOG_CAP) return;
    if (g_log_len + n > SECLOG_CAP) {
        size_t need = (g_log_len + n) - SECLOG_CAP;
        size_t cut = 0;
        while (cut < g_log_len && cut < need) cut++;
        while (cut < g_log_len && g_log[cut] != '\n') cut++;   // whole lines only
        if (cut < g_log_len) cut++;
        memcpy(g_log, g_log + cut, g_log_len - cut);
        g_log_len -= cut;
    }
    memcpy(g_log + g_log_len, s, n);
    g_log_len += n;
}

// Post one notification by read-modify-write of the spool. RMW (not append)
// because the compositor also rewrites this file, and because that is exactly
// what userland/libc/notify.c does - same file, same record format, one
// convention.
static void notify_spool_post(int severity, const char *title, const char *body) {
    static char buf[SPOOL_CAP + 320];
    size_t blen = 0;
    uint32_t sz = 0;   /* fat_read_file takes uint32_t*, not size_t* */
    char spool[256];
    if (userconf_kpath(NOTIFY_SPOOL_NAME, spool, sizeof(spool)) != 0) return;
    void *cur = fat_read_file(&g_fat_fs, spool, &sz);
    if (cur) {
        if (sz > SPOOL_CAP) sz = SPOOL_CAP;
        memcpy(buf, cur, sz);
        blen = sz;
        kfree(cur);
    }
    char *p = buf + blen;
    *p++ = (char)('0' + (severity & 3));
    *p++ = '|';
    for (const char *t = title; *t; t++) *p++ = (*t == '|' || *t == '\n') ? ' ' : *t;
    *p++ = '|';
    for (const char *b = body; *b; b++)  *p++ = (*b == '|' || *b == '\n') ? ' ' : *b;
    *p++ = '\n';
    // #693: a security event that is not on disk is a hole in the audit trail,
    // and the audit trail has no caller to propagate to. Say so loudly.
    if (fat_write_file(&g_fat_fs, spool, buf, (uint32_t)(p - buf)) != 0)
        kprintf("[SECLOG] FAILED to spool a security event to %s: the audit "
                "trail has a GAP\n", spool);
}

static void seclog_drain(void) {
    security_audit_rec_t rec;
    unsigned char line[256];
    uint64_t now = security_audit_seq();
    uint64_t seq = g_seclog_drained;
    uint64_t diag_start = seq;            /* SECLOG-DIAG: range for the report */

    // Ring wrapped while we slept: say so instead of pretending we saw them.
    if (now > seq + AUDIT_RING) {
        uint64_t lost = now - seq - AUDIT_RING;
        int n = 0;
        const char *pre = "t=0 WARNING SECLOG_OVERRUN pid=0 lost ";
        for (const char *c = pre; *c; c++) line[n++] = (unsigned char)*c;
        char num[24]; int d = 0;
        if (lost == 0) num[d++] = '0';
        while (lost > 0) { num[d++] = (char)('0' + (lost % 10)); lost /= 10; }
        while (d > 0) line[n++] = (unsigned char)num[--d];
        const char *suf = " events before they could be written\n";
        for (const char *c = suf; *c; c++) line[n++] = (unsigned char)*c;
        log_append((const char *)line, (size_t)n);
        seq = now - AUDIT_RING;
    }

    int wrote = 0;
    for (; seq < now; seq++) {
        if (security_audit_fetch(seq, &rec) != 0) continue;
        unsigned long n = sec_fmt_record(rec.event, rec.pid, rec.timestamp,
                                         (const unsigned char *)rec.detail,
                                         strlen(rec.detail),
                                         line, sizeof(line));
        if (n == 0) continue;
        log_append((const char *)line, (size_t)n);
        wrote = 1;

        /* #711: mirror every security event into the GraphFS journal.
         * SECURITY.LOG is rewritten whole on every drain, so it cannot be
         * append-only in any meaningful sense and cannot detect an edit.
         * The journal record binds SHA-256 of THIS EXACT LINE, so the text
         * above is attested even though the bytes still live there. The
         * journal ignores the call when it is not up (not-ext2 root, or a
         * refused init), so this cannot break the existing sink. */
        { extern int gfs_journal_append(unsigned int, unsigned int,
                                        unsigned short, unsigned short,
                                        const void *, unsigned int);
          (void)gfs_journal_append(1 /*GFSJ_ACTOR_PID*/, rec.pid,
                                   2 /*GFSJ_OP_AUDIT*/, 3 /*EFFECT_NA*/,
                                   line, (unsigned int)n); }

        // CRITICAL (1) and WARNING (2) also surface on the desktop. INFO does
        // not: a toast per INFO event would train the user to ignore all of
        // them, which is worse than not showing them.
        unsigned int sev = sec_event_severity(rec.event);
        if (sev <= 2) {
            char body[192];
            int  bl = 0;
            const unsigned char *nm = sec_event_name(rec.event);
            while (*nm && bl < 40) body[bl++] = (char)*nm++;
            body[bl++] = ' ';
            const char *dp = rec.detail;
            while (*dp && bl < (int)sizeof(body) - 1) body[bl++] = *dp++;
            body[bl] = 0;
            notify_spool_post(sev == 1 ? 3 /*NOTIFY_ERROR*/ : 2 /*NOTIFY_WARNING*/,
                              sev == 1 ? "Security Alert" : "Security Warning",
                              body);
        }
    }

    g_seclog_drained = now;
    if (wrote) {
        int rc = fat_write_file(&g_fat_fs, SECLOG_PATH, g_log, (uint32_t)g_log_len);

        /* SECLOG-DIAG (#653): the write return code was previously DISCARDED,
         * which is why an absent SECURITY.LOG gave no clue whether the write
         * was attempted, refused, or silently lost on an unclean shutdown.
         * Bounded so a repeatedly-failing write cannot flood the console. */
        static int seclog_diag_w = 0;
        if (seclog_diag_w < 8) {
            seclog_diag_w++;
            kprintf("[SECLOG-DIAG] drained seq %lu..%lu, buf=%lu bytes, "
                    "fat_write_file(%s) rc=%d\n",
                    (unsigned long)diag_start, (unsigned long)now,
                    (unsigned long)g_log_len, SECLOG_PATH, rc);
        }
    } else {
        static int seclog_diag_n = 0;
        if (seclog_diag_n < 4) {
            seclog_diag_n++;
            kprintf("[SECLOG-DIAG] drain ran but wrote NOTHING "
                    "(seq %lu..%lu) - fetch returned no usable records\n",
                    (unsigned long)diag_start, (unsigned long)now);
        }
    }
}

static void seclog_worker(void *arg) {
    (void)arg;

    // Carry forward whatever previous boots recorded, so the log is a history
    // and not just this boot.
    uint32_t sz = 0;   /* fat_read_file takes uint32_t*, not size_t* */
    void *prev = fat_read_file(&g_fat_fs, SECLOG_PATH, &sz);
    if (prev) {
        if (sz > SECLOG_CAP) {
            // Keep the NEWEST bytes, aligned to a line start.
            size_t off = sz - SECLOG_CAP;
            const char *b = (const char *)prev;
            while (off < sz && b[off] != '\n') off++;
            if (off < sz) off++;
            memcpy(g_log, b + off, sz - off);
            g_log_len = sz - off;
        } else {
            memcpy(g_log, prev, sz);
            g_log_len = sz;
        }
        kfree(prev);
    }
    kprintf("[SECLOG] worker up; carried %lu bytes of prior history from %s\n",
            (unsigned long)g_log_len, SECLOG_PATH);

    g_seclog_drained = security_audit_seq();   // don't replay pre-FS boot events twice
    g_seclog_ready = 1;

    for (;;) {
        // Plain wait_event: every producer calls seclog_kick(), so the wake
        // source is ours and cannot be lost. No timeout, because a timeout here
        // would only hide a missing kick.
        wait_event(&g_seclog_wq, seclog_pending());

        /* SECLOG-DIAG (#653): distinguishes "the worker never ran" (PRIO_LOW
         * starvation, or a wake that never arrived) from "it ran but the write
         * failed". Without this the two are indistinguishable from an absent
         * file. Bounded. */
        static int seclog_diag_k = 0;
        if (seclog_diag_k < 8) {
            seclog_diag_k++;
            kprintf("[SECLOG-DIAG] worker woke: audit_seq=%lu drained=%lu\n",
                    (unsigned long)security_audit_seq(),
                    (unsigned long)g_seclog_drained);
        }

        seclog_drain();
    }
}

void seclog_init(void) {
    wait_queue_head_init(&g_seclog_wq);

    /* #653: CHECK THE RESULT. This return used to be discarded, so when the
     * call was made before proc_init() and the thread was never created, the
     * line below still cheerfully announced that the sink had started. An
     * absent SECURITY.LOG then looked like a filesystem problem for two build
     * cycles. A sink that fails to start must SAY SO. */
    int rc = proc_create_ex("seclog", seclog_worker, 0, PRIO_LOW, 64 * 1024);
    if (rc < 0) {
        kprintf("[SECLOG] FAILED to create worker thread (rc=%d): security "
                "events will stay in the in-memory ring and NOTHING will be "
                "written to %s. Check that seclog_init() runs after "
                "proc_init().\n", rc, SECLOG_PATH);
        return;
    }
    kprintf("[SECLOG] security event sink started (-> %s + %s)\n",
            SECLOG_PATH, "<home>/CONFIG/" NOTIFY_SPOOL_NAME);
}
