// cron.c - #265 cron-like timer/scheduler subsystem for MayteraOS.
//
// See cron.h for the design overview and the on-disk config format. In short:
//   - INTERVAL / ONESHOT jobs run off the monotonic 250 Hz tick counter.
//   - DAILY / WEEKLY jobs run off the RTC wall clock.
//   - cron_tick() is a cheap hook off sched_tick() that only flags due
//     tick-based jobs; the cron worker kernel process actually fires them so
//     actions can block (filesystem I/O, proc_create) off the timer interrupt.
//   - Jobs persist in /CONFIG/CRON.CFG and reload at boot.
//
// This complements #95 background services: services own long-lived
// processes, cron owns time-driven work (and a job action can itself launch a
// program or call a registered kernel callback, e.g. a backup hook).

#include "cron.h"
#include "process.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../exec/elf.h"
#include "../gui/syslog.h"

extern fat_fs_t g_fat_fs;
extern volatile uint64_t timer_ticks;   // cpu/isr.h, 250 Hz monotonic
extern uint32_t g_timer_hz;             // cpu/pic.c
extern void rtc_read_time(int *hour, int *minute, int *second);
extern void rtc_read_date(int *day, int *month, int *year, int *weekday);

// ---------------------------------------------------------------------------
// Internal registry
// ---------------------------------------------------------------------------
typedef struct {
    cron_job_t j;            // public job record (wire format)
    uint8_t  used;           // slot occupied
    uint8_t  persist;        // write to /CONFIG/CRON.CFG
    volatile uint8_t pending;// set by cron_tick (hint), cleared by worker
    uint32_t last_wall_key;  // wall-clock dedup for DAILY/WEEKLY
    // #692: WHO this job runs as. A cron job is the sharp edge of the
    // spawn-identity bug: the worker that fires it is a KERNEL THREAD, so
    // the launched program used to inherit uid 0. Once the desktop session
    // is not root, that turns a user-created job into a root process, which
    // is privilege escalation created BY making root mean something.
    //
    // The owner is stamped by the KERNEL from the caller of cron_add(); it
    // is deliberately NOT part of cron_job_t, the userland wire struct, so
    // Ring 3 cannot ask for a job that runs as somebody else.
    uint32_t owner_uid;
} cron_slot_t;

static cron_slot_t g_jobs[CRON_MAX_JOBS];
static int      g_inited = 0;
static uint32_t g_next_id = 1;
static volatile int g_cron_wake = 0;   // worker latency hint set by cron_tick

#define CRON_CB_MAX     16
typedef struct { char name[24]; cron_callback_fn fn; void *ctx; } cron_cb_t;
static cron_cb_t g_cbs[CRON_CB_MAX];
static int       g_cb_count = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static uint32_t cron_atou(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

// Append an unsigned decimal to buf at *pos (bounded).
static void cron_app_u(char *buf, int *pos, int cap, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && *pos < cap - 1) buf[(*pos)++] = tmp[--n];
}

static void cron_app_str(char *buf, int *pos, int cap, const char *s) {
    while (*s && *pos < cap - 1) buf[(*pos)++] = *s++;
}

static void cron_app_2d(char *buf, int *pos, int cap, uint32_t v) {
    if (*pos < cap - 1) buf[(*pos)++] = (char)('0' + (v / 10) % 10);
    if (*pos < cap - 1) buf[(*pos)++] = (char)('0' + v % 10);
}

// ---------------------------------------------------------------------------
// #698 / #700 B4: CONTROL 2 of 3 - the SERIALIZER cannot emit a separator.
//
// Control 1 (validation in cron_add_common) is the one that runs today. This is
// the one that still holds when control 1 is wrong. Validation is a list of
// characters somebody remembered to forbid, and the entire history in blame.md
// is of controls that were accurate when written and quietly stopped covering
// the cases that mattered. Escaping is not a list: any byte that is not plainly
// safe becomes "%XX", so a newline, a tab or a space CANNOT reach the file no
// matter what a future caller, a future field, or a relaxed validator allows.
// A record separator that the writer is incapable of producing is not a
// vulnerability that can be reintroduced by editing a different function.
//
// Round-trip: '%' itself is escaped, so decoding is exact and unambiguous.
// Backwards compatible with the existing on-disk file, which contains no '%'.
static void cron_app_esc(char *buf, int *pos, int cap, const char *s) {
    static const char HX[] = "0123456789ABCDEF";
    for (; *s && *pos < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (c > 0x20 && c < 0x7F && c != '%') {
            buf[(*pos)++] = (char)c;
        } else {
            buf[(*pos)++] = '%';
            buf[(*pos)++] = HX[(c >> 4) & 0xF];
            buf[(*pos)++] = HX[c & 0xF];
        }
    }
}

static int cron_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Decode in place. A malformed "%" sequence is left literal rather than
// guessed at, so a hand-edited file degrades to a wrong-looking target (which
// then fails cron_field_ok) instead of a silently different one.
static void cron_unescape(char *s) {
    char *w = s;
    for (const char *r = s; *r; ) {
        if (r[0] == '%' && cron_hexval(r[1]) >= 0 && cron_hexval(r[2]) >= 0) {
            *w++ = (char)((cron_hexval(r[1]) << 4) | cron_hexval(r[2]));
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static uint64_t cron_ms_to_ticks(uint32_t ms) {
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t t = (uint64_t)ms * hz / 1000;
    return t ? t : 1;   // at least one tick
}

static const char *cron_type_name(int t) {
    switch (t) {
        case CRON_TYPE_ONESHOT:  return "ONESHOT";
        case CRON_TYPE_INTERVAL: return "INTERVAL";
        case CRON_TYPE_DAILY:    return "DAILY";
        case CRON_TYPE_WEEKLY:   return "WEEKLY";
        default:                 return "?";
    }
}

static const char *cron_act_name(int a) {
    switch (a) {
        case CRON_ACT_CALLBACK: return "callback";
        case CRON_ACT_LAUNCH:   return "launch";
        case CRON_ACT_EVENT:    return "event";
        default:                return "?";
    }
}

// ---------------------------------------------------------------------------
// Callback registry
// ---------------------------------------------------------------------------
int cron_register_callback(const char *name, cron_callback_fn fn, void *ctx) {
    if (!name || !fn) return -1;
    for (int i = 0; i < g_cb_count; i++) {
        if (strcmp(g_cbs[i].name, name) == 0) {  // replace existing
            g_cbs[i].fn = fn; g_cbs[i].ctx = ctx; return 0;
        }
    }
    if (g_cb_count >= CRON_CB_MAX) return -2;
    cron_cb_t *c = &g_cbs[g_cb_count++];
    strncpy(c->name, name, sizeof(c->name) - 1);
    c->name[sizeof(c->name) - 1] = '\0';
    c->fn = fn; c->ctx = ctx;
    return 0;
}

static cron_cb_t *cron_find_cb(const char *name) {
    for (int i = 0; i < g_cb_count; i++)
        if (strcmp(g_cbs[i].name, name) == 0) return &g_cbs[i];
    return 0;
}

// ---------------------------------------------------------------------------
// Built-in callbacks
// ---------------------------------------------------------------------------
// A simple heartbeat: logs an "alive" line to serial + syslog each time it
// fires. Useful as a default periodic job and as documentation of the API.
static void cb_heartbeat(uint32_t id, void *ctx) {
    (void)ctx;
    kprintf("[CRON] heartbeat job %u tick %lu\n",
            id, (unsigned long)timer_ticks);
    syslog_log(LOG_INFO, "cron heartbeat fired");
}

// A backup hook stub: real backup wiring can hang off this name from config
// (DAILY 03:00 callback backup). For now it just logs so the path is testable.
static void cb_backup(uint32_t id, void *ctx) {
    (void)ctx;
    kprintf("[CRON] backup hook job %u tick %lu\n",
            id, (unsigned long)timer_ticks);
    syslog_log(LOG_INFO, "cron backup hook fired");
}

// The self-test callback: prints a clearly-parseable line with the job id and
// both clocks so the headless serial test can assert on timing.
static void cb_selftest(uint32_t id, void *ctx) {
    (void)ctx;
    int h = 0, m = 0, s = 0;
    rtc_read_time(&h, &m, &s);
    kprintf("[CRON] fired job %u at tick %lu time %02d:%02d:%02d\n",
            id, (unsigned long)timer_ticks, h, m, s);
}

// ---------------------------------------------------------------------------
// Slot helpers
// ---------------------------------------------------------------------------
static cron_slot_t *cron_alloc_slot(void) {
    for (int i = 0; i < CRON_MAX_JOBS; i++)
        if (!g_jobs[i].used) return &g_jobs[i];
    return 0;
}

static cron_slot_t *cron_find_id(uint32_t id) {
    for (int i = 0; i < CRON_MAX_JOBS; i++)
        if (g_jobs[i].used && g_jobs[i].j.id == id) return &g_jobs[i];
    return 0;
}

// Compute the initial next_fire_tick for a tick-based job.
static void cron_schedule_first(cron_slot_t *s) {
    uint64_t now = timer_ticks;
    if (s->j.type == CRON_TYPE_INTERVAL || s->j.type == CRON_TYPE_ONESHOT)
        s->j.next_fire_tick = now + cron_ms_to_ticks(s->j.interval_ms);
    else
        s->j.next_fire_tick = 0;  // wall-clock driven
}

// Core add. If persist!=0 the registry is flushed to disk afterwards.
// #693: 0 only if /CONFIG/CRON.CFG is on the medium.
static int cron_save(void);

// ---------------------------------------------------------------------------
// #698 / #700 B4: CONTROL 1 of 3 - reject the injection at the door.
//
// THE BUG. cron_add_common() validated the type and action enums and nothing
// else. job.target[64] and job.label[32] arrive from a Ring-3 caller and were
// copied VERBATIM into the line cron_save() writes. The persisted format is
// newline-separated records of whitespace-separated fields, so a target
// containing a newline writes a SECOND RECORD that the caller composes.
//
// MEASURED, before this fix, on golden 1025, from uid 1000:
//   target = "X\nINTERVAL 10 launch /APPS/WHOAMI uid=0"   (39 of 63 bytes)
// produced in /CONFIG/CRON.CFG:
//   ONESHOT 3600 launch X
//   INTERVAL 10 launch /APPS/WHOAMI uid=0 uid=1000 sgp-inject
// and on the NEXT boot:
//   [CRON] added job 2 INTERVAL launch '/APPS/WHOAMI' ... owner uid=0
//   [CRON] launched '/APPS/WHOAMI' pid 26 as uid=0        (repeatedly)
//
// WHY #692 DID NOT STOP IT, and why that is the interesting part. #692 removed
// the caller's ability to CLAIM an identity: the owner is stamped by the kernel
// from the calling process and is not in the userland wire struct at all. This
// attack never claims anything. It is stamped honestly as uid=1000, and then
// writes bytes that a later, entirely legitimate read turns into a root job. An
// authorization control on the request cannot see it, because the request is
// authorized. The vulnerable surface is the SERIALIZER.
//
// A target is a program path, a callback name or an event name. None of those
// contain whitespace or control characters, so the safe set is exactly the
// printable non-space bytes. A label is free text on the tail of the line, so it
// may hold spaces, but never a control character.
static int cron_field_ok(const char *s, int max, int allow_space) {
    int i = 0;
    for (; i < max && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return 0;                 // \n, \r, \t, NUL-adjacent
        if (c == ' ' && !allow_space) return 0;              // would split a field
    }
    return (i < max);      // must have been NUL-terminated inside the array
}

static int cron_add_common(const cron_job_t *job, int persist, uint32_t owner_uid) {
    if (!g_inited) cron_init();
    if (!job) return -1;
    if (job->type > CRON_TYPE_WEEKLY || job->action > CRON_ACT_EVENT) return -2;

    // #698: a Ring-3 struct is not required to be NUL-terminated. Terminate a
    // local copy FIRST, or cron_app_str() walks target straight into label.
    cron_job_t jc = *job;
    jc.target[CRON_TARGET_MAX - 1] = '\0';
    jc.label[CRON_LABEL_MAX - 1]   = '\0';

    if (jc.target[0] == '\0') return -5;
    if (!cron_field_ok(jc.target, CRON_TARGET_MAX, 0) ||
        !cron_field_ok(jc.label,  CRON_LABEL_MAX,  1)) {
        kprintf("[CRON] REJECTED add from uid=%u: target/label carries whitespace "
                "or a control character (record-separator injection, #698)\n",
                owner_uid);
        return -6;
    }
    // A target that is itself an owner token would be ambiguous with the field
    // the kernel writes. Cheap to forbid; there is no legitimate such target.
    if (jc.target[0]=='u' && jc.target[1]=='i' && jc.target[2]=='d' && jc.target[3]=='=')
        return -6;

    cron_slot_t *s = cron_alloc_slot();
    if (!s) return -3;
    memset(s, 0, sizeof(*s));
    s->j = jc;
    s->j.id = g_next_id++;
    s->j.run_count = 0;
    s->used = 1;
    s->persist = persist ? 1 : 0;
    s->last_wall_key = 0xFFFFFFFFu;
    s->owner_uid = owner_uid;
    cron_schedule_first(s);
    // #693: the job is live in memory but would not survive a reboot.
    if (persist && cron_save() != 0) return -1;
    kprintf("[CRON] added job %u %s %s '%s' (%s) owner uid=%u\n",
            s->j.id, cron_type_name(s->j.type), cron_act_name(s->j.action),
            s->j.target, s->j.label[0] ? s->j.label : "-", s->owner_uid);
    return (int)s->j.id;
}

// #692: SYS_CRON_ADD lands here. The owner is taken from the CALLING Ring-3
// process and can never be chosen by the caller. A caller that is not a
// Ring-3 process (i.e. a kernel thread) is refused rather than defaulted to
// root, because "the parent happened to be a kernel thread" is exactly how
// this bug worked.
int cron_add(const cron_job_t *job) {
    uint32_t uid = 0, gid = 0;
    extern int spawnid_caller_ident(uint32_t *uid_out, uint32_t *gid_out);
    if (spawnid_caller_ident(&uid, &gid) != 0) {
        kprintf("[CRON] refusing add: no Ring-3 caller to own the job\n");
        return -4;
    }
    return cron_add_common(job, 1, uid);
}

// Built-in / self-test jobs. Kernel-authored, so root, stated explicitly.
int cron_add_volatile(const cron_job_t *job) { return cron_add_common(job, 0, 0); }

int cron_remove(uint32_t id) {
    cron_slot_t *s = cron_find_id(id);
    if (!s) return -1;
    int was_persist = s->persist;
    memset(s, 0, sizeof(*s));
    if (was_persist && cron_save() != 0) return -1;
    kprintf("[CRON] removed job %u\n", id);
    return 0;
}

int cron_enable(uint32_t id, int enable) {
    cron_slot_t *s = cron_find_id(id);
    if (!s) return -1;
    s->j.enabled = enable ? 1 : 0;
    if (enable && (s->j.type == CRON_TYPE_INTERVAL || s->j.type == CRON_TYPE_ONESHOT))
        cron_schedule_first(s);   // re-arm relative to now
    if (s->persist && cron_save() != 0) return -1;
    return 0;
}

int cron_list(cron_job_t *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < CRON_MAX_JOBS && n < max; i++)
        if (g_jobs[i].used) out[n++] = g_jobs[i].j;
    return n;
}

// ---------------------------------------------------------------------------
// Persistence: /CONFIG/CRON.CFG
// ---------------------------------------------------------------------------
static int cron_save(void) {
    // #693: no filesystem is a real failure to persist, not a silent no-op.
    if (!g_fat_fs.mounted) return -1;
    static char buf[4096];
    int pos = 0;
    // #698 / #700 B4: the owner is now the FIRST field, ahead of every byte the
    // caller chose, and it is mandatory. This follows #692's principle into the
    // persisted format: the kernel-stamped identity is not something that sits
    // downstream of user data and can be displaced by it.
    //
    // Being precise about what this does and does not buy, because overstating
    // it is how a control gets trusted for the wrong reason: field order does
    // NOT prevent injection. An attacker who can write a newline writes a whole
    // line and can put "uid=0" first just as easily as fifth. What it buys is
    // that the OLD field order no longer parses at all, so a pre-#698 line (or
    // one written by an older kernel) fails closed instead of being read with
    // the owner in the wrong place; and that "no owner" is now unrepresentable
    // rather than defaulting to root. The anti-injection controls are
    // cron_field_ok() and cron_app_esc().
    const char *hdr = "# MayteraOS cron jobs (#265). Auto-generated; edit with care.\n"
                      "# uid=<owner> <TYPE> <WHEN> <ACTION> <TARGET> [label]\n"
                      "# TARGET and label are %XX-escaped (#698). uid= is mandatory;\n"
                      "# a line without it is REJECTED, never assumed to be root's.\n";
    cron_app_str(buf, &pos, sizeof(buf), hdr);
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        cron_slot_t *s = &g_jobs[i];
        if (!s->used || !s->persist) continue;
        cron_app_str(buf, &pos, sizeof(buf), "uid=");
        cron_app_u(buf, &pos, sizeof(buf), s->owner_uid);
        buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = ' ';
        cron_app_str(buf, &pos, sizeof(buf), cron_type_name(s->j.type));
        buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = ' ';
        switch (s->j.type) {
            case CRON_TYPE_INTERVAL:
            case CRON_TYPE_ONESHOT:
                cron_app_u(buf, &pos, sizeof(buf), s->j.interval_ms / 1000);
                break;
            case CRON_TYPE_DAILY:
                cron_app_2d(buf, &pos, sizeof(buf), s->j.hour);
                buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = ':';
                cron_app_2d(buf, &pos, sizeof(buf), s->j.minute);
                break;
            case CRON_TYPE_WEEKLY:
                cron_app_u(buf, &pos, sizeof(buf), s->j.weekday);
                buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = ':';
                cron_app_2d(buf, &pos, sizeof(buf), s->j.hour);
                buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = ':';
                cron_app_2d(buf, &pos, sizeof(buf), s->j.minute);
                break;
        }
        buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = ' ';
        cron_app_str(buf, &pos, sizeof(buf), cron_act_name(s->j.action));
        buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = ' ';
        // #698: ESCAPED, not raw. This is the line that wrote the attacker's
        // newline into the file.
        cron_app_esc(buf, &pos, sizeof(buf), s->j.target);
        if (s->j.label[0]) {
            buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = ' ';
            cron_app_esc(buf, &pos, sizeof(buf), s->j.label);
        }
        buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = '\n';
    }
    buf[pos < (int)sizeof(buf) ? pos : (int)sizeof(buf) - 1] = '\0';
    int rc = fat_write_file(&g_fat_fs, "/CONFIG/CRON.CFG", buf, (uint32_t)pos);
    if (rc != 0)
        kprintf("[CRON] FAILED to write /CONFIG/CRON.CFG (rc=%d): scheduled jobs "
                "are NOT on disk and will be lost at reboot\n", rc);
    return rc;
}

// Parse one whitespace-delimited field; returns 1 if a field was read.
static int cron_field(const char **pp, const char *end, char *out, int outlen) {
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end) { *pp = p; return 0; }
    int n = 0;
    while (p < end && *p != ' ' && *p != '\t') {
        if (n < outlen - 1) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    *pp = p;
    return n > 0;
}

// Parse "HH:MM" -> hour/minute. Returns 1 on success.
static int cron_parse_hhmm(const char *s, int *hh, int *mm) {
    const char *c = strchr(s, ':');
    if (!c) return 0;
    *hh = (int)cron_atou(s);
    *mm = (int)cron_atou(c + 1);
    return (*hh >= 0 && *hh < 24 && *mm >= 0 && *mm < 60);
}

static void cron_parse_line(const char *ls, const char *le) {
    while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r')) ls++;
    if (ls >= le || *ls == '#') return;

    char typef[16], whenf[24], actf[16], targf[CRON_TARGET_MAX];
    const char *cur = ls;

    // -----------------------------------------------------------------------
    // #698 / #700 B4: CONTROL 3 of 3 - the reader FAILS CLOSED.
    //
    // What was here before: the owner was an OPTIONAL 5th field, found by
    // scanning for the first token that happened to start with "uid=", and a
    // line without one DEFAULTED TO ROOT. Two separate ways to become root out
    // of a malformed line: supply a uid= token anywhere the scan would reach,
    // or supply none at all. The justification for the default (only root can
    // write CRON.CFG, so an ownerless line must be root's) was true about the
    // FILE and false about its CONTENTS, which any uid could dictate through
    // cron_add(). "Nobody said who owns this, so it must be root" is the exact
    // shape of assumption #692 was written to delete.
    //
    // Now: field 1, positional, mandatory, digits only. Anything else and the
    // line is DISCARDED. An unparseable schedule entry is a lost job, which is
    // visible and fixable; an unparseable entry silently promoted to root is
    // not.
    // -----------------------------------------------------------------------
    uint32_t ownerf = 0;
    {
        char ownf[24];
        if (!cron_field(&cur, le, ownf, sizeof(ownf))) return;
        if (!(ownf[0]=='u' && ownf[1]=='i' && ownf[2]=='d' && ownf[3]=='=') ||
            ownf[4] == '\0') {
            kprintf("[CRON] REJECTED line: first field is '%s', expected "
                    "uid=<n> (#698: an owner is never inferred)\n", ownf);
            return;
        }
        for (int k = 4; ownf[k]; k++) {
            if (ownf[k] < '0' || ownf[k] > '9') {
                kprintf("[CRON] REJECTED line: owner field '%s' is not numeric\n", ownf);
                return;
            }
        }
        ownerf = cron_atou(ownf + 4);
    }

    if (!cron_field(&cur, le, typef, sizeof(typef))) return;
    if (!cron_field(&cur, le, whenf, sizeof(whenf))) return;
    if (!cron_field(&cur, le, actf, sizeof(actf)))   return;
    if (!cron_field(&cur, le, targf, sizeof(targf))) return;
    cron_unescape(targf);   // #698: %XX -> bytes, exactly inverse to cron_app_esc

    // Remainder of the line is the label.
    char labelf[CRON_LABEL_MAX];
    int li = 0;
    while (cur < le && (*cur == ' ' || *cur == '\t')) cur++;
    while (cur < le && *cur != '\r' && *cur != '\n' && li < CRON_LABEL_MAX - 1)
        labelf[li++] = *cur++;
    labelf[li] = '\0';

    cron_job_t job;
    memset(&job, 0, sizeof(job));
    job.enabled = 1;

    if      (strcmp(typef, "INTERVAL") == 0) job.type = CRON_TYPE_INTERVAL;
    else if (strcmp(typef, "ONESHOT")  == 0) job.type = CRON_TYPE_ONESHOT;
    else if (strcmp(typef, "DAILY")    == 0) job.type = CRON_TYPE_DAILY;
    else if (strcmp(typef, "WEEKLY")   == 0) job.type = CRON_TYPE_WEEKLY;
    else return;

    switch (job.type) {
        case CRON_TYPE_INTERVAL:
        case CRON_TYPE_ONESHOT:
            job.interval_ms = cron_atou(whenf) * 1000;
            if (job.interval_ms == 0) return;
            break;
        case CRON_TYPE_DAILY: {
            int hh, mm;
            if (!cron_parse_hhmm(whenf, &hh, &mm)) return;
            job.hour = (uint8_t)hh; job.minute = (uint8_t)mm;
            break;
        }
        case CRON_TYPE_WEEKLY: {
            // DOW:HH:MM
            const char *c1 = strchr(whenf, ':');
            if (!c1) return;
            int hh, mm;
            if (!cron_parse_hhmm(c1 + 1, &hh, &mm)) return;
            job.weekday = (uint8_t)(cron_atou(whenf) % 7);
            job.hour = (uint8_t)hh; job.minute = (uint8_t)mm;
            break;
        }
    }

    if      (strcmp(actf, "callback") == 0) job.action = CRON_ACT_CALLBACK;
    else if (strcmp(actf, "launch")   == 0) job.action = CRON_ACT_LAUNCH;
    else if (strcmp(actf, "event")    == 0) job.action = CRON_ACT_EVENT;
    else return;

    cron_unescape(labelf);   // #698: the label is escaped on write too
    strncpy(job.target, targf, CRON_TARGET_MAX - 1);
    strncpy(job.label, labelf, CRON_LABEL_MAX - 1);
    job.target[CRON_TARGET_MAX - 1] = '\0';
    job.label[CRON_LABEL_MAX - 1] = '\0';

    // #698: the file is not more trusted than the syscall. cron_add_common()
    // applies cron_field_ok() to this too, so a hand-edited or downgrade-written
    // CRON.CFG cannot reintroduce a target the serializer refuses to produce.
    cron_add_common(&job, 1, ownerf);  // a config job is persistent
}

static void cron_load(void) {
    if (!g_fat_fs.mounted) return;
    uint32_t sz = 0;
    char *data = (char *)fat_read_file(&g_fat_fs, "/CONFIG/CRON.CFG", &sz);
    if (!data || sz == 0) { if (data) kfree(data); return; }
    const char *p = data, *end = data + sz;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        cron_parse_line(ls, p);
        if (p < end) p++;  // skip newline
    }
    kfree(data);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void cron_init(void) {
    if (g_inited) return;
    g_inited = 1;
    memset(g_jobs, 0, sizeof(g_jobs));
    g_cb_count = 0;
    g_next_id = 1;

    // Built-in named callbacks usable from config or syscalls.
    cron_register_callback("heartbeat", cb_heartbeat, 0);
    cron_register_callback("backup",    cb_backup,    0);
    cron_register_callback("selftest",  cb_selftest,  0);

    // Load any persisted jobs.
    cron_load();

    int n = 0;
    for (int i = 0; i < CRON_MAX_JOBS; i++) if (g_jobs[i].used) n++;
    kprintf("[CRON] scheduler ready: %d job(s), %d callback(s)\n", n, g_cb_count);
}

// ---------------------------------------------------------------------------
// Firing (worker / process context only)
// ---------------------------------------------------------------------------
static void cron_launch_program(const char *path, uint32_t owner_uid) {
    if (!g_fat_fs.mounted) { kprintf("[CRON] launch '%s' failed: no fs\n", path); return; }
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data || sz == 0) { if (data) kfree(data);
        kprintf("[CRON] launch '%s' failed: not found\n", path); return; }
    if (elf_validate(data, sz) != 0) { kfree(data);
        kprintf("[CRON] launch '%s' failed: bad ELF\n", path); return; }
    // #692: THE FIX. This runs on the cron worker, a kernel thread, so the
    // old inherit gave the job uid 0 no matter who created it. It now runs
    // as the job's recorded owner, with the gid resolved from that uid.
    int pid = proc_create_user_as(path, data, sz, 0, 0, proc_as_uid(owner_uid));
    kfree(data);
    kprintf("[CRON] launched '%s' pid %d as uid=%u\n", path, pid, owner_uid);
}

static void cron_fire(cron_slot_t *s) {
    switch (s->j.action) {
        case CRON_ACT_CALLBACK: {
            cron_cb_t *c = cron_find_cb(s->j.target);
            if (c) c->fn(s->j.id, c->ctx);
            else kprintf("[CRON] job %u: no callback '%s'\n", s->j.id, s->j.target);
            break;
        }
        case CRON_ACT_LAUNCH:
            cron_launch_program(s->j.target, s->owner_uid);
            break;
        case CRON_ACT_EVENT:
            kprintf("[CRON] event '%s' from job %u tick %lu\n",
                    s->j.target, s->j.id, (unsigned long)timer_ticks);
            syslog_log(LOG_INFO, "cron event emitted");
            break;
    }
    s->j.run_count++;
}

// Scan all jobs, fire the due ones, and recompute their next fire. Runs only
// in the worker (process context), so blocking actions are safe here.
static void cron_run_due(void) {
    uint64_t now = timer_ticks;
    int h = 0, m = 0, sec = 0, day = 0, month = 0, year = 0, wday = 0;
    int got_wall = 0;

    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        cron_slot_t *s = &g_jobs[i];
        if (!s->used || !s->j.enabled) continue;

        int fire = 0;
        switch (s->j.type) {
            case CRON_TYPE_ONESHOT:
            case CRON_TYPE_INTERVAL:
                if (s->j.next_fire_tick && now >= s->j.next_fire_tick) fire = 1;
                break;
            case CRON_TYPE_DAILY:
            case CRON_TYPE_WEEKLY: {
                if (!got_wall) {
                    rtc_read_time(&h, &m, &sec);
                    rtc_read_date(&day, &month, &year, &wday);
                    got_wall = 1;
                }
                uint32_t key = (uint32_t)(((day * 24) + h) * 60 + m);
                if (h == s->j.hour && m == s->j.minute && key != s->last_wall_key) {
                    if (s->j.type == CRON_TYPE_DAILY || wday == s->j.weekday) {
                        s->last_wall_key = key;
                        fire = 1;
                    }
                }
                break;
            }
        }

        if (!fire) continue;
        s->pending = 0;
        cron_fire(s);

        // Recompute the next fire time.
        switch (s->j.type) {
            case CRON_TYPE_ONESHOT:
                s->j.enabled = 0;
                s->j.next_fire_tick = 0;
                if (s->persist && cron_save() != 0)
                    kprintf("[CRON] disabled state not persisted for job %u\n", s->j.id);
                break;
            case CRON_TYPE_INTERVAL: {
                uint64_t step = cron_ms_to_ticks(s->j.interval_ms);
                s->j.next_fire_tick += step;
                if (s->j.next_fire_tick <= now) s->j.next_fire_tick = now + step;
                break;
            }
            default:
                break;  // wall-clock jobs re-arm via last_wall_key dedup
        }
    }
}

// ---------------------------------------------------------------------------
// Tick hook + worker
// ---------------------------------------------------------------------------
// Cheap: runs in timer-interrupt context. Only inspects tick-based jobs and
// raises a wake hint so the worker reacts promptly. Never reads the RTC and
// never fires anything (firing lives in the worker / process context).
void cron_tick(void) {
    if (!g_inited) return;
    uint64_t now = timer_ticks;
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        cron_slot_t *s = &g_jobs[i];
        if (!s->used || !s->j.enabled) continue;
        if (s->j.type != CRON_TYPE_INTERVAL && s->j.type != CRON_TYPE_ONESHOT)
            continue;
        if (s->j.next_fire_tick && now >= s->j.next_fire_tick) {
            s->pending = 1;
            g_cron_wake = 1;
        }
    }
}

static void cron_worker(void *arg) {
    (void)arg;
    proc_sleep(2000);   // let the filesystem and desktop settle
    kprintf("[CRON] worker running\n");
    for (;;) {
        cron_run_due();
        g_cron_wake = 0;
        proc_sleep(100);   // 100 ms granularity is ample for scheduled jobs
    }
}

void cron_start_worker(void) {
    if (!g_inited) cron_init();
    proc_create_ex("cron", cron_worker, 0, PRIO_LOW, 256 * 1024);
}

// ---------------------------------------------------------------------------
// Gated serial self-test (#265). No-op unless /CONFIG/CRONTEST.CFG exists.
// Schedules a ONESHOT at +3s and an INTERVAL every 2s, both calling the
// "selftest" callback, then prints a banner so the headless test can assert.
// ---------------------------------------------------------------------------
static void cron_selftest_worker(void *arg) {
    (void)arg;
    proc_sleep(4000);   // after the cron worker is up and fs is settled
    uint32_t cfgsz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/CRONTEST.CFG", &cfgsz);
    if (!cfg) return;   // not flagged -> silent no-op
    kfree(cfg);

    kprintf("\n========== CRON SCHEDULER SELFTEST (#265) ==========\n");
    kprintf("[CRONTEST] now tick %lu hz %u\n",
            (unsigned long)timer_ticks, g_timer_hz);

    cron_job_t one;
    memset(&one, 0, sizeof(one));
    one.type = CRON_TYPE_ONESHOT;
    one.action = CRON_ACT_CALLBACK;
    one.enabled = 1;
    one.interval_ms = 3000;             // +3 s
    strncpy(one.target, "selftest", CRON_TARGET_MAX - 1);
    strncpy(one.label, "test-oneshot", CRON_LABEL_MAX - 1);
    int ida = cron_add_volatile(&one);

    cron_job_t iv;
    memset(&iv, 0, sizeof(iv));
    iv.type = CRON_TYPE_INTERVAL;
    iv.action = CRON_ACT_CALLBACK;
    iv.enabled = 1;
    iv.interval_ms = 2000;             // every 2 s
    strncpy(iv.target, "selftest", CRON_TARGET_MAX - 1);
    strncpy(iv.label, "test-interval", CRON_LABEL_MAX - 1);
    int idb = cron_add_volatile(&iv);

    kprintf("[CRONTEST] scheduled ONESHOT id=%d (+3s) INTERVAL id=%d (2s)\n",
            ida, idb);
    kprintf("[CRONTEST] watch for '[CRON] fired job ...' lines below\n");
    kprintf("========== CRON SELFTEST ARMED ==========\n");
}

void cron_start_deferred_selftest(void) {
    proc_create_ex("crontest", cron_selftest_worker, 0, PRIO_LOW, 256 * 1024);
}
