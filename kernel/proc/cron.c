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
static void cron_save(void);

static int cron_add_common(const cron_job_t *job, int persist) {
    if (!g_inited) cron_init();
    if (!job) return -1;
    if (job->type > CRON_TYPE_WEEKLY || job->action > CRON_ACT_EVENT) return -2;
    cron_slot_t *s = cron_alloc_slot();
    if (!s) return -3;
    memset(s, 0, sizeof(*s));
    s->j = *job;
    s->j.id = g_next_id++;
    s->j.run_count = 0;
    s->used = 1;
    s->persist = persist ? 1 : 0;
    s->last_wall_key = 0xFFFFFFFFu;
    cron_schedule_first(s);
    if (persist) cron_save();
    kprintf("[CRON] added job %u %s %s '%s' (%s)\n",
            s->j.id, cron_type_name(s->j.type), cron_act_name(s->j.action),
            s->j.target, s->j.label[0] ? s->j.label : "-");
    return (int)s->j.id;
}

int cron_add(const cron_job_t *job)          { return cron_add_common(job, 1); }
int cron_add_volatile(const cron_job_t *job) { return cron_add_common(job, 0); }

int cron_remove(uint32_t id) {
    cron_slot_t *s = cron_find_id(id);
    if (!s) return -1;
    int was_persist = s->persist;
    memset(s, 0, sizeof(*s));
    if (was_persist) cron_save();
    kprintf("[CRON] removed job %u\n", id);
    return 0;
}

int cron_enable(uint32_t id, int enable) {
    cron_slot_t *s = cron_find_id(id);
    if (!s) return -1;
    s->j.enabled = enable ? 1 : 0;
    if (enable && (s->j.type == CRON_TYPE_INTERVAL || s->j.type == CRON_TYPE_ONESHOT))
        cron_schedule_first(s);   // re-arm relative to now
    if (s->persist) cron_save();
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
static void cron_save(void) {
    if (!g_fat_fs.mounted) return;
    static char buf[4096];
    int pos = 0;
    const char *hdr = "# MayteraOS cron jobs (#265). Auto-generated; edit with care.\n"
                      "# <TYPE> <WHEN> <ACTION> <TARGET> [label]\n";
    cron_app_str(buf, &pos, sizeof(buf), hdr);
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        cron_slot_t *s = &g_jobs[i];
        if (!s->used || !s->persist) continue;
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
        cron_app_str(buf, &pos, sizeof(buf), s->j.target);
        if (s->j.label[0]) {
            buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = ' ';
            cron_app_str(buf, &pos, sizeof(buf), s->j.label);
        }
        buf[pos < (int)sizeof(buf) - 1 ? pos++ : pos] = '\n';
    }
    buf[pos < (int)sizeof(buf) ? pos : (int)sizeof(buf) - 1] = '\0';
    fat_write_file(&g_fat_fs, "/CONFIG/CRON.CFG", buf, (uint32_t)pos);
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
    if (!cron_field(&cur, le, typef, sizeof(typef))) return;
    if (!cron_field(&cur, le, whenf, sizeof(whenf))) return;
    if (!cron_field(&cur, le, actf, sizeof(actf)))   return;
    if (!cron_field(&cur, le, targf, sizeof(targf))) return;

    // Remainder of the line (after the four fields) is the label.
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

    strncpy(job.target, targf, CRON_TARGET_MAX - 1);
    strncpy(job.label, labelf, CRON_LABEL_MAX - 1);

    cron_add_common(&job, 1);  // a config job is persistent
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
static void cron_launch_program(const char *path) {
    if (!g_fat_fs.mounted) { kprintf("[CRON] launch '%s' failed: no fs\n", path); return; }
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data || sz == 0) { if (data) kfree(data);
        kprintf("[CRON] launch '%s' failed: not found\n", path); return; }
    if (elf_validate(data, sz) != 0) { kfree(data);
        kprintf("[CRON] launch '%s' failed: bad ELF\n", path); return; }
    int pid = proc_create_user(path, data, sz, 0, 0);
    kfree(data);
    kprintf("[CRON] launched '%s' pid %d\n", path, pid);
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
            cron_launch_program(s->j.target);
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
                if (s->persist) cron_save();   // persist the disabled state
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
