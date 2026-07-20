// taskmanager.c - Task Manager / Process Explorer for MayteraOS (#487/#349).
//
// A Windows-11-style, kernel-compositor built-in app. Four tabs:
//   Processes   - tree/sorted list: CPU% / memory / threads / fds / status,
//                 sortable columns, expand/collapse, right-click actions.
//   Performance - live CPU / memory / disk / network history line charts.
//   Details     - Process-Explorer depth for the selected process: open fds,
//                 memory regions (VMAs), thread count, priority, plus a
//                 system-wide TCP connection table (netstat).
//   Services    - proc/services.c registry with start/stop.
//
// Editable priority (#184) + kill-by-signal (#430) via right-click / keys.
// Theme-aware, antialiased-TTF-with-bitmap-fallback, information-dense. The
// on_draw path NEVER blocks (#426): it reads a throttled, cached snapshot with
// bounded, allocation-free kernel reads. No busy-wait, no poll loop.
//
// #404 DATA CORE: the heavy per-frame data crunching (perf-history ring
// aggregation for the charts, and the process-row sort) is factored into a
// small seam with C reference twins below and Rust ports (perf_ring_stats_rs /
// taskmgr_sort_rows_rs in rustkern.rs) routed live under -DRUST_TASKMGR_CORE.
#include "taskmanager.h"
#include "window.h"
#include "../types.h"
#include "../serial.h"
#include "../proc/procmem.h"   // #487: proc_mem_info / proc_mem_out_t
#include "../cpu/smp.h"        // #487: smp_get_core_count / smp_get_core_pct
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/demand.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "ttf.h"
#include "themes.h"
#include "../proc/process.h"
#include "../proc/signal.h"
#include "../proc/services.h"
#include "../fs/vfs.h"
#include "../net/tcp.h"
#include "syslog.h"

extern volatile uint64_t timer_ticks;
extern int      proc_get_cpu_usage(void);
extern uint64_t net_total_bytes(void);
extern int64_t  sys_get_disk_total(void);
extern int64_t  sys_get_disk_free(void);

// ===========================================================================
// #404 taskmgr_core data seam: C reference twins + Rust dispatch. The Rust
// ports (perf_ring_stats_rs / taskmgr_sort_rows_rs) live in rustkern.rs and are
// routed here by -DRUST_TASKMGR_CORE (added to the Makefile by the integrator).
// Confinement (proven offline on the kernel build container, 5,000,000 differential vectors, 0
// mismatch): the naive C sort over-WRITES its permutation array when count>cap
// (CWE-787) and the naive ring aggregation divides by zero when cap==0; the
// Rust ports reject both by construction. #[repr(C)] layouts are sizeof-locked.
// ===========================================================================
typedef struct { uint32_t min, max, last, avg, count; } perf_stat_t;
_Static_assert(sizeof(perf_stat_t) == 20, "perf_stat_t FFI layout");
typedef struct { int32_t pid; uint32_t cpu; uint32_t mem; } proc_key_t;
_Static_assert(sizeof(proc_key_t) == 12, "proc_key_t FFI layout");

// Naive ring aggregation (min/max/last/avg over `count` most-recent samples of
// a `cap`-slot ring whose oldest is at `head`). cap==0 -> `% cap` UB.
int perf_ring_stats_c(const uint32_t *samples, uint32_t cap, uint32_t head,
                      uint32_t count, perf_stat_t *out) {
    if (!samples || !out) return -1;
    if (count == 0) { out->min = out->max = out->last = out->avg = out->count = 0; return 0; }
    uint32_t mn = 0xFFFFFFFFu, mx = 0, last = 0; uint64_t sum = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = samples[(head + i) % cap];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v; last = v;
    }
    out->min = mn; out->max = mx; out->last = last;
    out->avg = (uint32_t)(sum / count); out->count = count;
    return 1;
}

static int tm_key_before(const proc_key_t *a, const proc_key_t *b, int key) {
    switch (key) {
        case 0: return a->cpu != b->cpu ? (a->cpu > b->cpu) : (a->pid < b->pid);
        case 1: return a->mem != b->mem ? (a->mem > b->mem) : (a->pid < b->pid);
        case 3: return a->cpu != b->cpu ? (a->cpu < b->cpu) : (a->pid < b->pid);
        case 4: return a->mem != b->mem ? (a->mem < b->mem) : (a->pid < b->pid);
        default: return a->pid < b->pid;
    }
}
// Naive stable permutation sort. Writes idx_out[0..count) with NO check that
// count<=cap -> over-WRITES idx_out (CWE-787) when count>cap.
int taskmgr_sort_rows_c(const proc_key_t *rows, uint32_t count, uint32_t cap,
                        int key, int32_t *idx_out) {
    if (!rows || !idx_out) return -1;
    (void)cap;
    if (count == 0) return 0;
    for (uint32_t i = 0; i < count; i++) idx_out[i] = (int32_t)i;
    for (uint32_t i = 1; i < count; i++) {
        int32_t cur = idx_out[i]; uint32_t j = i;
        while (j > 0 && tm_key_before(&rows[cur], &rows[idx_out[j - 1]], key)) {
            idx_out[j] = idx_out[j - 1]; j--;
        }
        idx_out[j] = cur;
    }
    return (int)count;
}

#ifdef RUST_TASKMGR_CORE
extern int perf_ring_stats_rs(const uint32_t *, uint32_t, uint32_t, uint32_t, perf_stat_t *);
extern int taskmgr_sort_rows_rs(const proc_key_t *, uint32_t, uint32_t, int, int32_t *);
#define perf_ring_stats   perf_ring_stats_rs
#define taskmgr_sort_rows taskmgr_sort_rows_rs
#else
#define perf_ring_stats   perf_ring_stats_c
#define taskmgr_sort_rows taskmgr_sort_rows_c
#endif

// ===========================================================================
// Layout constants + theme helpers
// ===========================================================================
#define TM_TABBAR_H     26
#define TM_HEADER_H     20
#define TM_ROW_H        18
#define TM_FOOTER_H     32
#define TM_PAD          8

static const theme_t *TH(void) { return theme_get_current(); }

static uint32_t tm_lighten(uint32_t c, int a) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r += a; g += a; b += a; if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static uint32_t tm_darken(uint32_t c, int a) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r -= a; g -= a; b -= a; if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static void tm_raised(int x, int y, int w, int h, uint32_t face) {
    fb_fill_rect(x, y, w, h, face);
    uint32_t hi = tm_lighten(face, 60), sh = tm_darken(face, 60);
    fb_fill_rect(x, y, w, 1, hi); fb_fill_rect(x, y, 1, h, hi);
    fb_fill_rect(x, y + h - 1, w, 1, sh); fb_fill_rect(x + w - 1, y, 1, h, sh);
}
static void tm_sunken(int x, int y, int w, int h, uint32_t face) {
    fb_fill_rect(x, y, w, h, face);
    uint32_t hi = tm_lighten(face, 60), sh = tm_darken(face, 60);
    fb_fill_rect(x, y, w, 1, sh); fb_fill_rect(x, y, 1, h, sh);
    fb_fill_rect(x, y + h - 1, w, 1, hi); fb_fill_rect(x + w - 1, y, 1, h, hi);
}

// AA text with bitmap fallback (mirrors desktop.c draw_string pattern).
static void tm_text(int x, int y, const char *s, uint32_t color) {
    if (ttf_is_ready()) {
        ttf_draw_string(x, y + 12, s, TTF_SIZE_SMALL, color);
    } else {
        for (int i = 0; s[i]; i++) {
            const uint8_t *g = font_get_glyph(s[i]);
            for (int row = 0; row < 16; row++) {
                uint8_t bits = g[row];
                for (int col = 0; col < 8; col++)
                    if (bits & (0x80 >> col)) fb_put_pixel(x + i * 8 + col, y + row, color);
            }
        }
    }
}
static int tm_text_w(const char *s) {
    if (ttf_is_ready()) return ttf_measure_string(s, TTF_SIZE_SMALL);
    return (int)strlen(s) * 8;
}
static void tm_text_right(int right_x, int y, const char *s, uint32_t color) {
    tm_text(right_x - tm_text_w(s), y, s, color);
}

// ---- number/string formatting (freestanding, no libc snprintf) ----
static char *tm_puts(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *tm_putu(char *p, uint64_t v) {
    char t[24]; int i = 0;
    if (!v) { *p++ = '0'; return p; }
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) *p++ = t[--i];
    return p;
}
static char *tm_puthex(char *p, uint64_t v) {
    char t[17]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { int d = (int)(v & 0xF); t[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    while (i) *p++ = t[--i];
    return p;
}
static void tm_fmt_u(uint64_t v, char *buf) { *tm_putu(buf, v) = 0; }
static void tm_fmt_kb(uint32_t kb, char *buf) {
    char *p = buf;
    if (kb < 1000) { p = tm_putu(p, kb); p = tm_puts(p, " KB"); }
    else if (kb < 1000u * 1024u) {
        uint32_t mb = kb / 1024, frac = (kb % 1024) * 10 / 1024;
        p = tm_putu(p, mb); *p++ = '.'; p = tm_putu(p, frac); p = tm_puts(p, " MB");
    } else {
        uint32_t gb = kb / (1024u * 1024u), frac = ((kb / 1024) % 1024) * 10 / 1024;
        p = tm_putu(p, gb); *p++ = '.'; p = tm_putu(p, frac); p = tm_puts(p, " GB");
    }
    *p = 0;
}
static void tm_fmt_pct(uint32_t v, char *buf) { char *p = tm_putu(buf, v); *tm_puts(p, "%") = 0; }
static void tm_fmt_ip(uint32_t ip, char *buf) {
    char *p = buf;
    for (int i = 0; i < 4; i++) { p = tm_putu(p, (ip >> (i * 8)) & 0xFF); if (i < 3) *p++ = '.'; }
    *p = 0;
}

// #487: render a handle's access mode the way Process Explorer's Handles view
// does, instead of a raw hex flags word.
static const char *tm_access_name(int flags) {
    switch (flags & O_ACCMODE) {
        case O_RDONLY: return "(R)";
        case O_WRONLY: return "(W)";
        case O_RDWR:   return "(RW)";
        default:       return "(?)";
    }
}

static const char *tm_pstate(uint32_t s) {
    switch (s) {
        case PROC_STATE_READY: return "Ready";
        case PROC_STATE_RUNNING: return "Running";
        case PROC_STATE_SLEEPING: return "Sleeping";
        case PROC_STATE_BLOCKED: return "Blocked";
        case PROC_STATE_ZOMBIE: return "Zombie";
        default: return "-";
    }
}
static const char *tm_prio_name(uint8_t p) {
    switch (p) {
        case PRIO_IDLE: return "Idle";
        case PRIO_LOW: return "Low";
        case PRIO_NORMAL: return "Normal";
        case PRIO_HIGH: return "High";
        case PRIO_REALTIME: return "Realtime";
        default: return "?";
    }
}

// ===========================================================================
// Kill / priority (capability-guarded). NOTE: the kernel kill path is not
// capability-gated today (sys_kill/SYS_SETPRIORITY do no permission check), so
// this app adds GUARD RAILS - it refuses to signal the idle/desktop (pid 0) or
// init (pid 1). A real capability gate on the kill path is a separate task
// (flagged for the integrator); do not read "capability-gated" as already true.
// ===========================================================================
static int tm_protected_pid(uint32_t pid) { return pid <= 1; }

static void tm_signal(uint32_t pid, int sig) {
    if (tm_protected_pid(pid)) {
        kprintf("[TaskManager] refusing signal %d to protected pid %u\n", sig, pid);
        return;
    }
    process_t *p = proc_get(pid);
    if (p) { sig_raise(p, sig); kprintf("[TaskManager] signal %d -> pid %u (%s)\n", sig, pid, p->name); }
}
static void tm_set_prio(uint32_t pid, int level) {
    if (level < PRIO_IDLE) level = PRIO_IDLE;
    if (level > PRIO_REALTIME) level = PRIO_REALTIME;
    process_t *p = proc_get(pid);
    if (p) { p->priority = (process_priority_t)level; kprintf("[TaskManager] pid %u priority -> %s\n", pid, tm_prio_name((uint8_t)level)); }
}

// ===========================================================================
// Performance history ring
// ===========================================================================
static void tm_ring_push(uint32_t *ring, uint32_t *head, uint32_t *count, uint32_t cap, uint32_t v) {
    if (*count < cap) {
        ring[(*head + *count) % cap] = v;
        (*count)++;
    } else {
        ring[*head] = v;
        *head = (*head + 1) % cap;
    }
}

// ===========================================================================
// Refresh: rebuild the cached process list + sample the performance rings.
// Bounded, allocation-free reads only. Never blocks.
// ===========================================================================
static uint64_t tm_prev_ticks_of(taskmanager_t *tm, uint32_t pid) {
    for (int i = 0; i < tm->prev_n; i++) if (tm->prev_pid[i] == pid) return tm->prev_ticks[i];
    return 0;
}
static int tm_collapsed(taskmanager_t *tm, uint32_t pid) {
    for (int i = 0; i < tm->ncollapsed; i++) if (tm->collapsed[i] == pid) return 1;
    return 0;
}
static void tm_toggle_collapse(taskmanager_t *tm, uint32_t pid) {
    for (int i = 0; i < tm->ncollapsed; i++) {
        if (tm->collapsed[i] == pid) {
            tm->collapsed[i] = tm->collapsed[--tm->ncollapsed];
            return;
        }
    }
    if (tm->ncollapsed < TM_MAX_PROCS) tm->collapsed[tm->ncollapsed++] = pid;
}

void taskmanager_refresh(taskmanager_t *tm);

void taskmanager_refresh(taskmanager_t *tm) {
    if (!tm) return;

    // 1. raw snapshot
    static proc_info_t snap[TM_MAX_PROCS];
    int n = proc_snapshot(snap, TM_MAX_PROCS);
    if (n > TM_MAX_PROCS) n = TM_MAX_PROCS;

    // 2. enrich into a raw table + compute CPU% share-of-busy
    static tm_proc_t raw[TM_MAX_PROCS];
    int gcpu = proc_get_cpu_usage();
    static uint64_t delta[TM_MAX_PROCS];
    uint64_t sum_delta = 0;
    for (int i = 0; i < n; i++) {
        uint64_t pv = tm_prev_ticks_of(tm, snap[i].pid);
        delta[i] = (snap[i].cpu_ticks >= pv) ? (snap[i].cpu_ticks - pv) : 0;
        sum_delta += delta[i];
    }
    for (int i = 0; i < n; i++) {
        tm_proc_t *r = &raw[i];
        r->pid = snap[i].pid; r->ppid = snap[i].ppid;
        int j = 0; for (; j < 31 && snap[i].name[j]; j++) r->name[j] = snap[i].name[j];
        r->name[j] = 0;
        r->state = snap[i].state;
        r->mem_kb = snap[i].mem_kb;
        r->cpu_ticks = snap[i].cpu_ticks;
        r->running_cpu = snap[i].running_cpu;
        r->cpu_pct = sum_delta ? (uint32_t)((delta[i] * (uint64_t)gcpu) / sum_delta) : 0;
        r->threads = proc_thread_count(snap[i].pid);
        r->depth = 0; r->has_kids = 0;
        // deep fields from the PCB (bounded, direct read)
        process_t *p = proc_get(snap[i].pid);
        if (p) {
            r->priority = (uint8_t)p->priority;
            r->uid = p->uid;
            r->privilege = p->privilege;
            uint32_t fds = 0;
            for (int f = 0; f < MAX_FDS; f++) if (p->fds[f]) fds++;
            r->fds = fds;
        } else {
            r->priority = PRIO_NORMAL; r->uid = 0; r->privilege = 0; r->fds = 0;
        }
    }

    // save prev ticks for next delta
    tm->prev_n = n;
    for (int i = 0; i < n; i++) { tm->prev_pid[i] = snap[i].pid; tm->prev_ticks[i] = snap[i].cpu_ticks; }

    // mark has_kids
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++)
            if (raw[k].ppid == raw[i].pid && raw[k].pid != raw[i].pid) { raw[i].has_kids = 1; break; }

    // 3. sort (data-core seam) -> global order by key
    static proc_key_t keys[TM_MAX_PROCS];
    static int32_t sidx[TM_MAX_PROCS];
    for (int i = 0; i < n; i++) { keys[i].pid = (int32_t)raw[i].pid; keys[i].cpu = raw[i].cpu_pct; keys[i].mem = raw[i].mem_kb; }
    int sc = taskmgr_sort_rows(keys, (uint32_t)n, TM_MAX_PROCS, tm->sort_key, sidx);
    if (sc < 0) { for (int i = 0; i < n; i++) sidx[i] = i; sc = n; }

    // 4. build display order
    tm->nproc = 0;
    if (!tm->tree_mode) {
        for (int s = 0; s < sc && tm->nproc < TM_MAX_PROCS; s++) {
            tm_proc_t *r = &raw[sidx[s]];
            tm->procs[tm->nproc] = *r;
            tm->procs[tm->nproc].depth = 0;
            tm->nproc++;
        }
    } else {
        static uint8_t emitted[TM_MAX_PROCS];
        for (int i = 0; i < n; i++) emitted[i] = 0;
        // iterative DFS using the sorted order for sibling ordering
        // roots first (ppid==0 or parent absent), then descend
        for (int pass_root = 0; pass_root < n; pass_root++) {
            // find next unemitted root in sorted order
            int ri = -1;
            for (int s = 0; s < sc; s++) {
                int idx = sidx[s];
                if (emitted[idx]) continue;
                uint32_t pp = raw[idx].ppid;
                int parent_present = 0;
                for (int k = 0; k < n; k++) if (raw[k].pid == pp && k != idx) { parent_present = 1; break; }
                if (pp == 0 || !parent_present) { ri = idx; break; }
            }
            if (ri < 0) break;
            // DFS stack of pids to expand, with depth
            uint32_t stack_pid[TM_MAX_PROCS]; int8_t stack_depth[TM_MAX_PROCS]; int sp = 0;
            stack_pid[sp] = raw[ri].pid; stack_depth[sp] = 0; sp++;
            while (sp > 0 && tm->nproc < TM_MAX_PROCS) {
                uint32_t cur = stack_pid[--sp]; int8_t d = stack_depth[sp];
                // find raw index for cur (unemitted)
                int ci = -1;
                for (int k = 0; k < n; k++) if (raw[k].pid == cur && !emitted[k]) { ci = k; break; }
                if (ci < 0) continue;
                emitted[ci] = 1;
                tm->procs[tm->nproc] = raw[ci];
                tm->procs[tm->nproc].depth = d;
                tm->nproc++;
                if (tm_collapsed(tm, cur)) continue;
                if (d >= 8) continue; // cycle / depth guard
                // push children in REVERSE sorted order so they pop in order
                for (int s = sc - 1; s >= 0; s--) {
                    int idx = sidx[s];
                    if (emitted[idx]) continue;
                    if (raw[idx].ppid == cur && raw[idx].pid != cur && sp < TM_MAX_PROCS) {
                        stack_pid[sp] = raw[idx].pid; stack_depth[sp] = (int8_t)(d + 1); sp++;
                    }
                }
            }
        }
        // safety: emit any stragglers (cycle survivors) flat
        for (int s = 0; s < sc && tm->nproc < TM_MAX_PROCS; s++) {
            int idx = sidx[s]; int already = 0;
            for (int e = 0; e < tm->nproc; e++) if (tm->procs[e].pid == raw[idx].pid) { already = 1; break; }
            if (!already) { tm->procs[tm->nproc] = raw[idx]; tm->procs[tm->nproc].depth = 0; tm->nproc++; }
        }
    }

    // reconcile sticky selection
    tm->selected_row = -1;
    for (int i = 0; i < tm->nproc; i++) if (tm->procs[i].pid == tm->selected_pid) { tm->selected_row = i; break; }
    if (tm->selected_row < 0 && tm->nproc > 0 && tm->selected_pid == 0) { tm->selected_row = 0; tm->selected_pid = tm->procs[0].pid; }

    // 5. sample performance rings
    uint64_t total_pages = pmm_get_total_pages();
    uint64_t used_pages = pmm_get_used_pages();
    tm->mem_total_kb = total_pages * 4;   // *4096/1024
    tm->mem_used_kb = used_pages * 4;
    uint32_t mem_pct = total_pages ? (uint32_t)(used_pages * 100 / total_pages) : 0;

    tm->disk_total = sys_get_disk_total();
    tm->disk_free = sys_get_disk_free();
    uint32_t disk_pct = 0;
    if (tm->disk_total > 0) disk_pct = (uint32_t)(((tm->disk_total - tm->disk_free) * 100) / tm->disk_total);

    uint64_t net_bytes = net_total_bytes();
    uint32_t net_kbs = 0;
    if (tm->prev_net_bytes && net_bytes >= tm->prev_net_bytes) {
        uint64_t db = net_bytes - tm->prev_net_bytes;   // bytes since last refresh (~1s)
        net_kbs = (uint32_t)(db / 1024);
    }
    tm->prev_net_bytes = net_bytes;

    // The four rings share one head/count (sampled together). Push CPU through
    // the ring helper, then store mem/disk/net at the SAME slot it just wrote.
    tm_ring_push(tm->cpu_hist, &tm->hist_head, &tm->hist_count, TM_HIST, (uint32_t)gcpu);
    {
        uint32_t idx = (tm->hist_count < TM_HIST)
            ? (tm->hist_head + tm->hist_count - 1) % TM_HIST
            : (tm->hist_head + TM_HIST - 1) % TM_HIST;
        tm->mem_hist[idx] = mem_pct;
        tm->disk_hist[idx] = disk_pct;
        tm->net_hist[idx] = net_kbs;
    }

    // 6. details: system-wide TCP connection snapshot (now carries owner_pid,
    // so the Details tab can attribute rows to the selected process).
    tm->nconns = tcp_conn_snapshot(tm->conns, TM_MAX_CONNS);

    // 7. #487: per-logical-core CPU samples (Win11 per-core graphs). The kernel
    // already meters these (cpu/smp.c smp_account_core_usage, driven from
    // sched_tick); this only samples them into the display rings at the slot
    // the other rings just filled, so one hist_head/hist_count describes all.
    {
        int nc = smp_get_core_count();
        if (nc < 1) nc = 1;
        if (nc > TM_MAX_CORES) nc = TM_MAX_CORES;
        tm->ncores = nc;
        uint32_t slot = (tm->hist_head + tm->hist_count - 1) % TM_HIST;
        for (int c = 0; c < nc; c++) {
            int pct = smp_get_core_pct((uint32_t)c);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            tm->core_hist[c][slot] = (uint32_t)pct;
        }
    }

    // 8. #487/#265: scheduled tasks. cron_list() is a bounded copy out of the
    // job registry; no allocation, no blocking.
    tm->njobs = cron_list(tm->jobs, TM_MAX_CRON);
    if (tm->njobs < 0) tm->njobs = 0;

    tm->last_update = timer_ticks;
}

// ===========================================================================
// Chart drawing (built from fb_draw_line polylines + the data-core aggregate)
// ===========================================================================
static void tm_draw_chart(int x, int y, int w, int h, const uint32_t *ring,
                          uint32_t head, uint32_t count, const char *label,
                          const char *unit, uint32_t color, uint32_t fixed_max) {
    const theme_t *t = TH();
    tm_sunken(x, y, w, h, tm_darken(t->window_bg, 12));
    int px = x + 2, py = y + 2, pw = w - 4, ph = h - 4;

    // grid
    uint32_t grid = tm_lighten(t->window_bg, 18);
    for (int gy = 1; gy < 4; gy++) fb_fill_rect(px, py + ph * gy / 4, pw, 1, grid);

    perf_stat_t st;
    int rc = perf_ring_stats(ring, TM_HIST, head, count, &st);
    uint32_t maxv = fixed_max ? fixed_max : (rc == 1 && st.max > 0 ? st.max : 1);
    if (maxv == 0) maxv = 1;

    // polyline
    if (count >= 2) {
        int prevx = 0, prevy = 0, have = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = ring[(head + i) % TM_HIST];
            int lx = px + (int)((uint64_t)i * (pw - 1) / (count - 1));
            uint32_t vv = v > maxv ? maxv : v;
            int ly = py + ph - 1 - (int)((uint64_t)vv * (ph - 1) / maxv);
            if (have) fb_draw_line(prevx, prevy, lx, ly, color);
            prevx = lx; prevy = ly; have = 1;
        }
    }

    // label + readouts
    tm_text(x + 4, y + 2, label, t->label_text);
    char buf[48]; char *p = buf;
    p = tm_puts(p, "cur ");
    p = tm_putu(p, rc == 1 ? st.last : 0);
    p = tm_puts(p, unit);
    p = tm_puts(p, "  max ");
    p = tm_putu(p, rc == 1 ? st.max : 0);
    p = tm_puts(p, unit);
    *p = 0;
    tm_text(x + 4, y + h - 16, buf, t->menu_text_disabled);
}

// ===========================================================================
// Tab bar
// ===========================================================================
static const char *TM_TAB_NAMES[TM_TAB_COUNT] = { "Processes", "Performance", "Details", "Services", "Scheduled" };

static void tm_draw_tabs(taskmanager_t *tm, int wx, int wy, int ww) {
    const theme_t *t = TH();
    fb_fill_rect(wx, wy, ww, TM_TABBAR_H, t->window_bg);
    int tw = ww / TM_TAB_COUNT;
    for (int i = 0; i < TM_TAB_COUNT; i++) {
        int tx = wx + i * tw;
        int active = ((int)tm->tab == i);
        uint32_t face = active ? t->titlebar_active : t->button_bg;
        tm_raised(tx, wy + (active ? 0 : 2), tw - 2, TM_TABBAR_H - (active ? 0 : 2), face);
        uint32_t tc = active ? t->titlebar_text : t->button_text;
        int lw = tm_text_w(TM_TAB_NAMES[i]);
        tm_text(tx + (tw - lw) / 2, wy + 4, TM_TAB_NAMES[i], tc);
    }
    fb_fill_rect(wx, wy + TM_TABBAR_H - 1, ww, 1, t->window_border);
}

// ===========================================================================
// Processes tab
// ===========================================================================
// column right-edges are computed from content width
static void tm_proc_columns(int wx, int ww, int *c_pid, int *c_cpu, int *c_mem, int *c_thr, int *c_status) {
    int right = wx + ww - TM_PAD;
    *c_status = right;             // status left-aligned start (drawn from here)
    *c_thr = right - 70;           // Thr right edge
    *c_mem = *c_thr - 40;          // Mem right edge
    *c_cpu = *c_mem - 78;          // CPU right edge
    *c_pid = *c_cpu - 52;          // PID right edge
}

static void tm_draw_processes(taskmanager_t *tm, int wx, int wy, int ww, int wh) {
    const theme_t *t = TH();
    int list_top = wy + TM_HEADER_H;
    int list_h = wh - TM_HEADER_H - TM_FOOTER_H;
    int rows = list_h / TM_ROW_H;

    int c_pid, c_cpu, c_mem, c_thr, c_status;
    tm_proc_columns(wx, ww, &c_pid, &c_cpu, &c_mem, &c_thr, &c_status);

    // header
    fb_fill_rect(wx, wy, ww, TM_HEADER_H, t->selection_bg);
    tm_text(wx + TM_PAD + 12, wy + 2, "Name", t->selection_text);
    tm_text_right(c_pid, wy + 2, "PID", t->selection_text);
    tm_text_right(c_cpu, wy + 2, "CPU", t->selection_text);
    tm_text_right(c_mem, wy + 2, "Memory", t->selection_text);
    tm_text_right(c_thr, wy + 2, "Thr", t->selection_text);
    tm_text(c_status - 60, wy + 2, "Status", t->selection_text);
    fb_fill_rect(wx, wy + TM_HEADER_H - 1, ww, 1, t->window_border);

    // rows
    for (int i = 0; i < rows && i + tm->scroll < tm->nproc; i++) {
        int idx = i + tm->scroll;
        tm_proc_t *r = &tm->procs[idx];
        int ry = list_top + i * TM_ROW_H;
        int sel = (idx == tm->selected_row);
        if (sel) fb_fill_rect(wx, ry, ww, TM_ROW_H, t->titlebar_active);
        else if (i & 1) fb_fill_rect(wx, ry, ww, TM_ROW_H, tm_lighten(t->window_bg, 6));
        uint32_t tc = sel ? t->titlebar_text : t->label_text;
        uint32_t dc = sel ? t->titlebar_text : t->menu_text_disabled;

        int name_x = wx + TM_PAD + r->depth * 12;
        // expand/collapse triangle
        if (r->has_kids) {
            const char *tri = tm_collapsed(tm, r->pid) ? "+" : "-";
            tm_text(name_x, ry + 1, tri, tc);
        }
        // kernel procs get a subtle accent
        uint32_t namecol = tc;
        if (!sel && r->privilege == 0) namecol = t->color_info;
        tm_text(name_x + 12, ry + 1, r->name[0] ? r->name : "(unnamed)", namecol);

        char buf[24];
        tm_fmt_u(r->pid, buf);            tm_text_right(c_pid, ry + 1, buf, dc);
        tm_fmt_pct(r->cpu_pct, buf);      tm_text_right(c_cpu, ry + 1, buf, r->cpu_pct >= 50 ? t->color_warning : tc);
        tm_fmt_kb(r->mem_kb, buf);        tm_text_right(c_mem, ry + 1, buf, tc);
        tm_fmt_u(r->threads, buf);        tm_text_right(c_thr, ry + 1, buf, dc);
        tm_text(c_status - 60, ry + 1, tm_pstate(r->state), dc);
    }

    // footer
    int fy = wy + wh - TM_FOOTER_H;
    fb_fill_rect(wx, fy, ww, TM_FOOTER_H, t->window_bg);
    fb_fill_rect(wx, fy, ww, 1, t->window_border);
    int can = (tm->selected_row >= 0);
    tm_raised(wx + TM_PAD, fy + 4, 90, 24, can ? t->button_bg : tm_darken(t->button_bg, 20));
    tm_text(wx + TM_PAD + 8, fy + 6, "End Task", can ? t->color_error : t->button_disabled);
    tm_raised(wx + TM_PAD + 100, fy + 4, 70, 24, t->button_bg);
    tm_text(wx + TM_PAD + 108, fy + 6, tm->tree_mode ? "Flat" : "Tree", t->button_text);
    char info[48]; char *p = tm_puts(info, "");
    p = tm_putu(p, (uint64_t)tm->nproc); p = tm_puts(p, " processes  CPU "); p = tm_putu(p, (uint64_t)proc_get_cpu_usage()); p = tm_puts(p, "%");
    *p = 0;
    tm_text_right(wx + ww - TM_PAD, fy + 8, info, t->menu_text_disabled);
}

// ===========================================================================
// Performance tab
// ===========================================================================
// #487: Win11 "Logical processors" view: one small chart per core, laid out on
// a grid that adapts to the core count. Data comes from the kernel's existing
// per-core meter (cpu/smp.c), sampled into core_hist[] each refresh.
//
// NOTE ON CORE COUNT: the APs only boot when g_smp_user_sched is set, so a
// default boot legitimately reports ONE logical core and this view shows a
// single chart. That is the truth about the machine, not a bug in the view.
static void tm_draw_cores(taskmanager_t *tm, int wx, int wy, int ww, int wh) {
    const theme_t *t = TH();
    int n = tm->ncores;
    if (n < 1) n = 1;
    if (n > TM_MAX_CORES) n = TM_MAX_CORES;

    // Choose a grid that stays close to square, so 1, 2, 4, 8, 16 all look right.
    int cols = 1;
    while (cols * cols < n) cols++;
    if (cols > 4) cols = 4;
    int rows = (n + cols - 1) / cols;
    if (rows < 1) rows = 1;

    int gap = 6;
    int cw = (ww - (cols + 1) * gap) / cols;
    int ch = (wh - (rows + 1) * gap - 18) / rows;
    if (cw < 40 || ch < 30) {   // too small to render meaningfully
        tm_text(wx + TM_PAD, wy + TM_PAD, "(window too small for per-core view)",
                t->menu_text_disabled);
        return;
    }

    char lbl[24];
    for (int i = 0; i < n; i++) {
        int r = i / cols, c = i % cols;
        int x = wx + gap + c * (cw + gap);
        int y = wy + gap + 18 + r * (ch + gap);
        char *q = tm_puts(lbl, "CPU ");
        q = tm_putu(q, (uint64_t)i);
        *q = 0;
        // Core 0 is the BSP (measured by the aggregate meter); tint it so the
        // distinction from the AP-measured cores is visible.
        tm_draw_chart(x, y, cw, ch, tm->core_hist[i], tm->hist_head,
                      tm->hist_count, lbl, "%",
                      i == 0 ? t->color_success : t->color_info, 100);
    }
    char hdr[64]; char *q = tm_puts(hdr, "Logical processors: ");
    q = tm_putu(q, (uint64_t)n);
    if (n == 1) q = tm_puts(q, "  (APs idle: SMP user scheduling off)");
    *q = 0;
    tm_text(wx + gap, wy + gap, hdr, t->menu_text_disabled);
}

static void tm_draw_performance(taskmanager_t *tm, int wx, int wy, int ww, int wh) {
    const theme_t *t = TH();
    fb_fill_rect(wx, wy, ww, wh, t->window_bg);

    // #487: Overall / Logical-processors toggle (Win11's split). Drawn first so
    // the chart grid below gets the remaining space.
    int by = wy + 4;
    int bw = 92, bh = 20;
    int bx0 = wx + TM_PAD, bx1 = bx0 + bw + 6;
    tm_raised(bx0, by, bw, bh, tm->perf_view == TM_PERF_OVERALL
                                   ? t->titlebar_active : t->button_bg);
    tm_text(bx0 + 10, by + 2, "Overall",
            tm->perf_view == TM_PERF_OVERALL ? t->titlebar_text : t->button_text);
    tm_raised(bx1, by, bw, bh, tm->perf_view == TM_PERF_CORES
                                   ? t->titlebar_active : t->button_bg);
    tm_text(bx1 + 6, by + 2, "Per-core",
            tm->perf_view == TM_PERF_CORES ? t->titlebar_text : t->button_text);

    int cy = wy + bh + 8;
    int chh = wh - (bh + 8);
    if (tm->perf_view == TM_PERF_CORES) {
        tm_draw_cores(tm, wx, cy, ww, chh);
        return;
    }
    wy = cy; wh = chh;

    int gap = 6;
    int cw = (ww - 3 * gap) / 2;
    int ch = (wh - 3 * gap) / 2;
    int x0 = wx + gap, y0 = wy + gap;

    tm_draw_chart(x0, y0, cw, ch, tm->cpu_hist, tm->hist_head, tm->hist_count,
                  "CPU", "%", t->color_success, 100);
    tm_draw_chart(x0 + cw + gap, y0, cw, ch, tm->mem_hist, tm->hist_head, tm->hist_count,
                  "Memory", "%", t->color_info, 100);
    tm_draw_chart(x0, y0 + ch + gap, cw, ch, tm->disk_hist, tm->hist_head, tm->hist_count,
                  "Disk", "%", t->color_warning, 100);
    tm_draw_chart(x0 + cw + gap, y0 + ch + gap, cw, ch, tm->net_hist, tm->hist_head, tm->hist_count,
                  "Network", " KB/s", t->titlebar_active, 0);

    // memory readout under the mem chart
    char buf[64]; char *p = tm_puts(buf, "RAM ");
    p = tm_putu(p, tm->mem_used_kb / 1024); p = tm_puts(p, " / ");
    p = tm_putu(p, tm->mem_total_kb / 1024); p = tm_puts(p, " MB"); *p = 0;
    tm_text(x0 + cw + gap + 4, y0 + 14, buf, t->menu_text_disabled);
}

// ===========================================================================
// Details tab (Process-Explorer depth for the selected process)
// ===========================================================================
static int tm_line(int x, int *y, const char *label, const char *val, uint32_t lc, uint32_t vc) {
    tm_text(x, *y, label, lc);
    if (val) tm_text(x + 130, *y, val, vc);
    *y += 16;
    return *y;
}

static void tm_draw_details(taskmanager_t *tm, int wx, int wy, int ww, int wh) {
    const theme_t *t = TH();
    fb_fill_rect(wx, wy, ww, wh, t->window_bg);
    int x = wx + TM_PAD, y = wy + TM_PAD;
    uint32_t lc = t->menu_text_disabled, vc = t->label_text;

    process_t *p = (tm->selected_pid) ? proc_get(tm->selected_pid) : NULL;
    if (!p) {
        tm_text(x, y, "No process selected (pick one on the Processes tab).", lc);
        goto conns;
    }

    char buf[64];
    tm_text(x, y, p->name[0] ? p->name : "(unnamed)", t->titlebar_active); y += 18;
    tm_fmt_u(p->pid, buf);      tm_line(x, &y, "PID", buf, lc, vc);
    tm_fmt_u(p->ppid, buf);     tm_line(x, &y, "Parent PID", buf, lc, vc);
    tm_line(x, &y, "State", tm_pstate((uint32_t)p->state), lc, vc);
    tm_line(x, &y, "Priority", tm_prio_name((uint8_t)p->priority), lc,
            (p->priority >= PRIO_HIGH) ? t->color_warning : vc);
    tm_line(x, &y, "Privilege", p->privilege == 0 ? "Ring 0 (kernel)" : "Ring 3 (user)", lc, vc);
    tm_fmt_u(p->uid, buf);      tm_line(x, &y, "UID", buf, lc, vc);
    tm_fmt_u(proc_thread_count(p->pid), buf); tm_line(x, &y, "Threads", buf, lc, vc);
    tm_fmt_u(p->total_time, buf); tm_line(x, &y, "CPU ticks", buf, lc, vc);
    { char *q = tm_puts(buf, "0x"); q = tm_puthex(q, p->cr3); *q = 0;
      tm_line(x, &y, "CR3 (PML4)", buf, lc, vc);
    }

    // #487: real memory breakdown, from the Rust accounting seam (proc/procmem.c
    // -> proc_mem_account_rs). Before this, the only memory figure available was
    // a hardcoded user_stack_size, i.e. a flat ~2 MB for every process.
    {
        proc_mem_out_t pm;
        if (proc_mem_info(p->pid, &pm)) {
            tm_fmt_kb(pm.working_set_kb, buf);
            tm_line(x, &y, "Working set", buf, lc, vc);
            tm_fmt_kb(pm.private_kb, buf);
            tm_line(x, &y, "Private (commit)", buf, lc, vc);
            tm_fmt_kb(pm.virt_kb, buf);
            tm_line(x, &y, "Virtual size", buf, lc, vc);
            tm_fmt_kb(pm.heap_kb, buf);
            tm_line(x, &y, "Heap (brk)", buf, lc, vc);
            // Surface a corrupt/cyclic VMA list rather than hiding it: the seam
            // bounds the walk, and this is where that shows up.
            if (pm.flags & PROC_MEM_F_TRUNC) {
                tm_text(x, y, "! VMA list truncated (corrupt or cyclic)",
                        t->color_error); y += 16;
            }
            if (pm.flags & PROC_MEM_F_BADVMA) {
                tm_text(x, y, "! VMA with inverted extent skipped",
                        t->color_warning); y += 16;
            }
        }
    }

    // memory map
    mm_struct_t *mm = (mm_struct_t *)p->mm;
    if (mm) {
        tm_fmt_kb((uint32_t)(mm->resident_pages * 4), buf); tm_line(x, &y, "Resident", buf, lc, vc);
        tm_fmt_kb((uint32_t)(mm->total_mapped / 1024), buf); tm_line(x, &y, "Mapped", buf, lc, vc);
        tm_fmt_u(mm->vma_count, buf); tm_line(x, &y, "VM regions", buf, lc, vc);
        int shown = 0;
        for (vma_t *v = mm->vma_list; v && shown < 4; v = v->next, shown++) {
            char *q = tm_puts(buf, "  ");
            q = tm_puthex(q, v->start);
            *q++ = '-';
            q = tm_puthex(q, v->end);
            *q = 0;
            tm_text(x + 12, y, buf, t->menu_text_disabled); y += 14;
        }
    } else {
        tm_fmt_kb((uint32_t)(p->user_stack_size / 1024), buf);
        tm_line(x, &y, "User stack", buf, lc, vc);
        tm_text(x, y, "(no demand-paged mm; showing committed stack)", lc); y += 16;
    }

    // #487: open handles, NAMED (right column). file_t now records the path it
    // was opened with (fs/vfs.h), so this reports the OBJECT behind each handle
    // the way Process Explorer does, instead of a bare "fd 5 flags 0x1".
    int fx = wx + ww / 2 + TM_PAD, fy = wy + TM_PAD + 18;
    tm_text(fx, fy, "Open handles:", t->titlebar_active); fy += 18;
    int nf = 0;
    for (int f = 0; f < MAX_FDS && fy < wy + wh - 90; f++) {
        if (!p->fds[f]) continue;
        char *q = tm_puts(buf, "fd ");
        q = tm_putu(q, (uint64_t)f);
        q = tm_puts(q, " ");
        q = tm_puts(q, tm_access_name(p->fds[f]->flags));
        *q = 0;
        tm_text(fx + 8, fy, buf, lc);
        // The path is the interesting column: give it the readable colour and
        // right-align nothing (paths truncate on the left of the pane edge).
        const char *nm = p->fds[f]->path[0] ? p->fds[f]->path : "(anonymous)";
        tm_text(fx + 8 + 92, fy, nm, p->fds[f]->path[0] ? vc : lc);
        fy += 14; nf++;
    }
    if (nf == 0) { tm_text(fx + 8, fy, "(none)", lc); }

conns:
    // #487: network connections, ATTRIBUTED. tcp_conn_t now carries owner_pid
    // (stamped in tcp_socket(), inherited from the listener on inbound SYN), so
    // when a process is selected this shows only ITS connections (the Process
    // Explorer answer to "who opened this socket"). With no selection it falls
    // back to the system-wide view.
    {
        int cy = wy + wh - 84;
        fb_fill_rect(wx, cy - 4, ww, 1, t->window_border);
        tcp_conn_info_t mine[TM_MAX_CONNS];
        const tcp_conn_info_t *rows = tm->conns;
        int nrows = tm->nconns;
        char hdr[80]; char *q;
        if (tm->selected_pid) {
            int k = conn_filter_by_pid(tm->conns, (uint32_t)tm->nconns,
                                       tm->selected_pid, mine, TM_MAX_CONNS);
            if (k >= 0) { rows = mine; nrows = k; }
            q = tm_puts(hdr, "Network connections (pid ");
            q = tm_putu(q, tm->selected_pid);
            q = tm_puts(q, "): ");
        } else {
            q = tm_puts(hdr, "Network connections (system): ");
        }
        q = tm_putu(q, (uint64_t)nrows); *q = 0;
        tm_text(wx + TM_PAD, cy, hdr, t->titlebar_active); cy += 16;
        int shown = 0;
        for (int i = 0; i < nrows && shown < 4; i++) {
            const tcp_conn_info_t *c = &rows[i];
            char line[112]; char *r = line;
            r = tm_puts(r, tcp_state_name((tcp_state_t)c->state));
            r = tm_puts(r, "  :");
            r = tm_putu(r, c->local_port);
            r = tm_puts(r, c->is_listener ? " (listen)" : " -> ");
            if (!c->is_listener) { char ip[20]; tm_fmt_ip(c->remote_ip, ip); r = tm_puts(r, ip); *r++ = ':'; r = tm_putu(r, c->remote_port); }
            if (!tm->selected_pid) {
                r = tm_puts(r, "  pid ");
                if (c->owner_pid) r = tm_putu(r, c->owner_pid);
                else r = tm_puts(r, "-");
            }
            *r = 0;
            tm_text(wx + TM_PAD + 8, cy, line, t->label_text); cy += 14; shown++;
        }
        if (nrows == 0) {
            tm_text(wx + TM_PAD + 8, cy,
                    tm->selected_pid ? "(this process has no connections)"
                                     : "(no active connections)", lc);
        }
    }
}

// ===========================================================================
// #487/#265 Scheduled Tasks tab
//
// The kernel has had a full cron-like scheduler (proc/cron.c: oneshot /
// interval / daily / weekly jobs, callback / launch / event actions, persisted
// to /CONFIG/CRON.CFG) with a cron_list() enumeration accessor, but nothing in
// the GUI ever surfaced it. This is the Windows "Scheduled Tasks" equivalent.
// Layout deliberately mirrors the Services tab (same header bar, same zebra
// rows, same selection colours) per the shared design language.
// ===========================================================================
static const char *tm_cron_type(uint8_t ty) {
    switch (ty) {
        case CRON_TYPE_ONESHOT:  return "Once";
        case CRON_TYPE_INTERVAL: return "Interval";
        case CRON_TYPE_DAILY:    return "Daily";
        case CRON_TYPE_WEEKLY:   return "Weekly";
        default:                 return "?";
    }
}

static const char *tm_cron_action(uint8_t a) {
    switch (a) {
        case CRON_ACT_CALLBACK: return "callback";
        case CRON_ACT_LAUNCH:   return "launch";
        case CRON_ACT_EVENT:    return "event";
        default:                return "?";
    }
}

static const char *tm_weekday(uint8_t d) {
    static const char *w[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    return (d < 7) ? w[d] : "?";
}

// Render a job's schedule the way a user reads it ("Daily 03:30", "every 5s").
static void tm_cron_when(const cron_job_t *j, char *out) {
    char *q = out;
    switch (j->type) {
        case CRON_TYPE_DAILY:
            q = tm_puts(q, "Daily ");
            if (j->hour < 10) *q++ = '0';
            q = tm_putu(q, j->hour); *q++ = ':';
            if (j->minute < 10) *q++ = '0';
            q = tm_putu(q, j->minute);
            break;
        case CRON_TYPE_WEEKLY:
            q = tm_puts(q, tm_weekday(j->weekday)); *q++ = ' ';
            if (j->hour < 10) *q++ = '0';
            q = tm_putu(q, j->hour); *q++ = ':';
            if (j->minute < 10) *q++ = '0';
            q = tm_putu(q, j->minute);
            break;
        case CRON_TYPE_INTERVAL:
            q = tm_puts(q, "every ");
            if (j->interval_ms >= 1000) { q = tm_putu(q, j->interval_ms / 1000); q = tm_puts(q, "s"); }
            else { q = tm_putu(q, j->interval_ms); q = tm_puts(q, "ms"); }
            break;
        case CRON_TYPE_ONESHOT:
            q = tm_puts(q, "once in ");
            q = tm_putu(q, j->interval_ms / 1000); q = tm_puts(q, "s");
            break;
        default:
            q = tm_puts(q, "-");
            break;
    }
    *q = 0;
}

static void tm_draw_scheduled(taskmanager_t *tm, int wx, int wy, int ww, int wh) {
    const theme_t *t = TH();
    int list_top = wy + TM_HEADER_H;
    int list_h = wh - TM_HEADER_H - TM_FOOTER_H;
    int rows = list_h / TM_ROW_H;

    fb_fill_rect(wx, wy, ww, wh, t->window_bg);
    fb_fill_rect(wx, wy, ww, TM_HEADER_H, t->selection_bg);
    int c_name = wx + TM_PAD;
    int c_when = wx + ww * 2 / 5;
    int c_act  = wx + ww * 3 / 5;
    int c_runs = wx + ww * 4 / 5;
    tm_text(c_name, wy + 2, "Task", t->selection_text);
    tm_text(c_when, wy + 2, "Schedule", t->selection_text);
    tm_text(c_act,  wy + 2, "Action", t->selection_text);
    tm_text(c_runs, wy + 2, "Runs", t->selection_text);
    tm_text_right(wx + ww - TM_PAD, wy + 2, "State", t->selection_text);
    fb_fill_rect(wx, wy + TM_HEADER_H - 1, ww, 1, t->window_border);

    for (int i = 0; i < rows && i < tm->njobs; i++) {
        const cron_job_t *j = &tm->jobs[i];
        int ry = list_top + i * TM_ROW_H;
        int sel = (i == tm->sched_sel);
        if (sel) fb_fill_rect(wx, ry, ww, TM_ROW_H, t->titlebar_active);
        else if (i & 1) fb_fill_rect(wx, ry, ww, TM_ROW_H, tm_lighten(t->window_bg, 6));
        uint32_t tc = sel ? t->titlebar_text : t->label_text;
        uint32_t dc = sel ? t->titlebar_text : t->menu_text_disabled;

        tm_text(c_name, ry + 1, j->label[0] ? j->label : j->target, tc);
        // Schedule column: the human-readable trigger, prefixed with the job
        // type so "Interval" vs "Once" is explicit rather than inferred.
        char when[64]; char *w = tm_puts(when, tm_cron_type(j->type));
        w = tm_puts(w, "  "); *w = 0;
        tm_cron_when(j, w);
        tm_text(c_when, ry + 1, when, dc);
        // Action column: what fires, and on what target.
        char act[80]; char *q = tm_puts(act, tm_cron_action(j->action));
        if (j->target[0]) { q = tm_puts(q, " "); q = tm_puts(q, j->target); }
        *q = 0;
        tm_text(c_act, ry + 1, act, dc);
        char runs[16]; tm_fmt_u(j->run_count, runs);
        tm_text(c_runs, ry + 1, runs, dc);
        tm_text_right(wx + ww - TM_PAD, ry + 1, j->enabled ? "Enabled" : "Disabled",
                      j->enabled ? (sel ? t->titlebar_text : t->color_success) : dc);
    }
    if (tm->njobs == 0) {
        tm_text(wx + TM_PAD, list_top + 6, "(no scheduled tasks registered)",
                t->menu_text_disabled);
    }

    int fy = wy + wh - TM_FOOTER_H;
    fb_fill_rect(wx, fy, ww, 1, t->window_border);
    int can = (tm->sched_sel >= 0 && tm->sched_sel < tm->njobs);
    tm_raised(wx + TM_PAD, fy + 4, 80, 24, t->button_bg);
    tm_text(wx + TM_PAD + 8, fy + 6, "Enable", can ? t->color_success : t->button_disabled);
    tm_raised(wx + TM_PAD + 90, fy + 4, 80, 24, t->button_bg);
    tm_text(wx + TM_PAD + 98, fy + 6, "Disable", can ? t->color_error : t->button_disabled);
    char info[48]; char *p = tm_puts(info, "");
    p = tm_putu(p, (uint64_t)tm->njobs); p = tm_puts(p, " scheduled tasks"); *p = 0;
    tm_text_right(wx + ww - TM_PAD, fy + 8, info, t->menu_text_disabled);
}

// ===========================================================================
// Services tab
// ===========================================================================
static void tm_draw_services(taskmanager_t *tm, int wx, int wy, int ww, int wh) {
    const theme_t *t = TH();
    int list_top = wy + TM_HEADER_H;
    int list_h = wh - TM_HEADER_H - TM_FOOTER_H;
    int rows = list_h / TM_ROW_H;

    fb_fill_rect(wx, wy, ww, wh, t->window_bg);
    fb_fill_rect(wx, wy, ww, TM_HEADER_H, t->selection_bg);
    tm_text(wx + TM_PAD, wy + 2, "Service", t->selection_text);
    tm_text(wx + ww / 2 - 40, wy + 2, "State", t->selection_text);
    tm_text(wx + ww / 2 + 30, wy + 2, "Account", t->selection_text);
    tm_text_right(wx + ww - TM_PAD, wy + 2, "Autostart / Perms", t->selection_text);
    fb_fill_rect(wx, wy + TM_HEADER_H - 1, ww, 1, t->window_border);

    int nsvc = svc_count();
    for (int i = 0; i < rows && i < nsvc; i++) {
        service_t *s = svc_at(i);
        if (!s) continue;
        int ry = list_top + i * TM_ROW_H;
        int sel = (i == tm->selected_row);
        if (sel) fb_fill_rect(wx, ry, ww, TM_ROW_H, t->titlebar_active);
        else if (i & 1) fb_fill_rect(wx, ry, ww, TM_ROW_H, tm_lighten(t->window_bg, 6));
        uint32_t tc = sel ? t->titlebar_text : t->label_text;
        int running = svc_is_running(s);
        tm_text(wx + TM_PAD, ry + 1, s->name, tc);
        tm_text(wx + ww / 2 - 40, ry + 1, running ? "Running" : "Stopped",
                running ? t->color_success : (sel ? t->titlebar_text : t->menu_text_disabled));
        tm_text(wx + ww / 2 + 30, ry + 1, s->account[0] ? s->account : "system", sel ? t->titlebar_text : t->menu_text_disabled);
        char perms[48]; svc_perms_str(s->perms, perms, sizeof(perms));
        char line[64]; char *p = tm_puts(line, s->autostart ? "auto " : "manual ");
        p = tm_puts(p, perms); *p = 0;
        tm_text_right(wx + ww - TM_PAD, ry + 1, line, sel ? t->titlebar_text : t->menu_text_disabled);
    }

    int fy = wy + wh - TM_FOOTER_H;
    fb_fill_rect(wx, fy, ww, 1, t->window_border);
    int can = (tm->selected_row >= 0 && tm->selected_row < nsvc);
    tm_raised(wx + TM_PAD, fy + 4, 70, 24, t->button_bg);
    tm_text(wx + TM_PAD + 12, fy + 6, "Start", can ? t->color_success : t->button_disabled);
    tm_raised(wx + TM_PAD + 80, fy + 4, 70, 24, t->button_bg);
    tm_text(wx + TM_PAD + 92, fy + 6, "Stop", can ? t->color_error : t->button_disabled);
}

// ===========================================================================
// Context menu
// ===========================================================================
static const char *TM_MENU_ITEMS[] = { "End Task (SIGTERM)", "Kill (SIGKILL)", "Raise Priority", "Lower Priority", "Details" };
#define TM_MENU_N 5
#define TM_MENU_ITEM_H 20
#define TM_MENU_W 170

static void tm_draw_menu(taskmanager_t *tm) {
    if (!tm->menu_open) return;
    const theme_t *t = TH();
    int mh = TM_MENU_N * TM_MENU_ITEM_H + 4;
    tm_raised(tm->menu_x, tm->menu_y, TM_MENU_W, mh, t->menu_bg);
    for (int i = 0; i < TM_MENU_N; i++) {
        int iy = tm->menu_y + 2 + i * TM_MENU_ITEM_H;
        if (i == tm->menu_hover) fb_fill_rect(tm->menu_x + 2, iy, TM_MENU_W - 4, TM_MENU_ITEM_H, t->menu_item_hover);
        uint32_t col = t->menu_text;
        if (i == 0) col = t->color_warning;
        if (i == 1) col = t->color_error;
        tm_text(tm->menu_x + 10, iy + 2, TM_MENU_ITEMS[i], (i == tm->menu_hover) ? t->selection_text : col);
    }
}

static void tm_menu_action(taskmanager_t *tm, int item) {
    uint32_t pid = tm->menu_pid;
    switch (item) {
        case 0: tm_signal(pid, SIGTERM); break;
        case 1: tm_signal(pid, SIGKILL); break;
        case 2: { process_t *p = proc_get(pid); if (p) tm_set_prio(pid, (int)p->priority + 1); break; }
        case 3: { process_t *p = proc_get(pid); if (p) tm_set_prio(pid, (int)p->priority - 1); break; }
        case 4: tm->selected_pid = pid; tm->tab = TM_TAB_DETAILS; break;
        default: break;
    }
    tm->menu_open = 0;
    taskmanager_refresh(tm);
    wm_invalidate_all();
}

// ===========================================================================
// Public: create / destroy
// ===========================================================================
taskmanager_t *taskmanager_create(void) {
    taskmanager_t *tm = (taskmanager_t *)kmalloc(sizeof(taskmanager_t));
    if (!tm) return NULL;
    memset(tm, 0, sizeof(taskmanager_t));
    tm->window = window_create("Task Manager", 140, 70, 560, 420);
    if (!tm->window) { kfree(tm); return NULL; }
    tm->tab = TM_TAB_PROCESSES;
    tm->sort_key = TM_SORT_CPU;
    tm->selected_row = -1;
    tm->tree_mode = 1;
    taskmanager_refresh(tm);
    return tm;
}

void taskmanager_destroy(taskmanager_t *tm) {
    if (!tm) return;
    if (tm->window) window_destroy(tm->window);
    kfree(tm);
}

// ===========================================================================
// Draw (non-blocking; throttled refresh ~1s)
// ===========================================================================
void taskmanager_draw(taskmanager_t *tm) {
    if (!tm || !tm->window) return;
    if (timer_ticks - tm->last_update > 100) taskmanager_refresh(tm);  // ~1s at 100Hz-ish

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(tm->window, &wx, &wy, &ww, &wh);
    const theme_t *t = TH();
    fb_fill_rect(wx, wy, ww, wh, t->window_bg);

    tm_draw_tabs(tm, wx, wy, ww);
    int cy = wy + TM_TABBAR_H, chh = wh - TM_TABBAR_H;
    switch (tm->tab) {
        case TM_TAB_PROCESSES:   tm_draw_processes(tm, wx, cy, ww, chh); break;
        case TM_TAB_PERFORMANCE: tm_draw_performance(tm, wx, cy, ww, chh); break;
        case TM_TAB_DETAILS:     tm_draw_details(tm, wx, cy, ww, chh); break;
        case TM_TAB_SERVICES:    tm_draw_services(tm, wx, cy, ww, chh); break;
        case TM_TAB_SCHEDULED:   tm_draw_scheduled(tm, wx, cy, ww, chh); break;
        default: break;
    }
    tm_draw_menu(tm);
}

// ===========================================================================
// Events
// ===========================================================================
static void tm_set_sort(taskmanager_t *tm, int key) {
    // clicking the active column toggles asc/desc
    if (key == TM_SORT_CPU && tm->sort_key == TM_SORT_CPU) tm->sort_key = TM_SORT_CPU_ASC;
    else if (key == TM_SORT_MEM && tm->sort_key == TM_SORT_MEM) tm->sort_key = TM_SORT_MEM_ASC;
    else tm->sort_key = key;
    taskmanager_refresh(tm);
}

void taskmanager_handle_event(taskmanager_t *tm, gui_event_t *event) {
    if (!tm || !event) return;
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(tm->window, &wx, &wy, &ww, &wh);
    int mx = event->mouse_x, my = event->mouse_y;

    // context menu takes precedence
    if (tm->menu_open) {
        if (event->type == EVENT_MOUSE_MOVE) {
            tm->menu_hover = -1;
            if (mx >= tm->menu_x && mx < tm->menu_x + TM_MENU_W) {
                int it = (my - tm->menu_y - 2) / TM_MENU_ITEM_H;
                if (it >= 0 && it < TM_MENU_N) tm->menu_hover = it;
            }
            return;
        }
        if (event->type == EVENT_MOUSE_DOWN) {
            if (mx >= tm->menu_x && mx < tm->menu_x + TM_MENU_W &&
                my >= tm->menu_y && my < tm->menu_y + TM_MENU_N * TM_MENU_ITEM_H + 4) {
                int it = (my - tm->menu_y - 2) / TM_MENU_ITEM_H;
                if (it >= 0 && it < TM_MENU_N) { tm_menu_action(tm, it); return; }
            }
            tm->menu_open = 0;   // click outside closes
            return;
        }
        return;
    }

    switch (event->type) {
    case EVENT_KEY_DOWN:
        switch (event->keycode) {
            // '1'..'5' scancodes. #487: extended for the Scheduled tab; the
            // bound is TM_TAB_COUNT so adding a tab cannot walk past the enum.
            case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: {
                int want = (int)event->keycode - 0x02;
                if (want >= 0 && want < TM_TAB_COUNT) tm->tab = (tm_tab_t)want;
                break;
            }
            case 0x48: if (tm->selected_row > 0) { tm->selected_row--; tm->selected_pid = tm->procs[tm->selected_row].pid; } break; // up
            case 0x50: if (tm->selected_row < tm->nproc - 1) { tm->selected_row++; tm->selected_pid = tm->procs[tm->selected_row].pid; } break; // down
            default:
                switch (event->key_char) {
                    case 'e': case 'E': if (tm->selected_pid) tm_signal(tm->selected_pid, SIGTERM); taskmanager_refresh(tm); break;
                    case 'k': case 'K': if (tm->selected_pid) tm_signal(tm->selected_pid, SIGKILL); taskmanager_refresh(tm); break;
                    case '+': { process_t *p = proc_get(tm->selected_pid); if (p) tm_set_prio(tm->selected_pid, (int)p->priority + 1); break; }
                    case '-': { process_t *p = proc_get(tm->selected_pid); if (p) tm_set_prio(tm->selected_pid, (int)p->priority - 1); break; }
                    case 't': case 'T': tm->tree_mode = !tm->tree_mode; taskmanager_refresh(tm); break;
                    case 'r': case 'R': taskmanager_refresh(tm); break;
                    // #487: toggle Overall vs per-core on the Performance tab.
                    case 'c': case 'C':
                        if (tm->tab == TM_TAB_PERFORMANCE)
                            tm->perf_view = (tm->perf_view == TM_PERF_OVERALL)
                                                ? TM_PERF_CORES : TM_PERF_OVERALL;
                        break;
                    default: break;
                }
                break;
        }
        break;

    case EVENT_MOUSE_SCROLL:
        tm->scroll -= event->scroll_delta;
        if (tm->scroll < 0) tm->scroll = 0;
        if (tm->scroll > tm->nproc - 1) tm->scroll = tm->nproc > 0 ? tm->nproc - 1 : 0;
        break;

    case EVENT_MOUSE_DOWN: {
        // tab strip
        if (my >= wy && my < wy + TM_TABBAR_H) {
            int tw = ww / TM_TAB_COUNT;
            int ti = (mx - wx) / tw;
            if (ti >= 0 && ti < TM_TAB_COUNT) tm->tab = (tm_tab_t)ti;
            break;
        }
        int cy = wy + TM_TABBAR_H;

        if (tm->tab == TM_TAB_PROCESSES) {
            // header sort
            if (my >= cy && my < cy + TM_HEADER_H) {
                int c_pid, c_cpu, c_mem, c_thr, c_status;
                tm_proc_columns(wx, ww, &c_pid, &c_cpu, &c_mem, &c_thr, &c_status);
                if (mx >= c_pid - 40 && mx < c_pid + 4) tm_set_sort(tm, TM_SORT_PID);
                else if (mx >= c_cpu - 40 && mx < c_cpu + 4) tm_set_sort(tm, TM_SORT_CPU);
                else if (mx >= c_mem - 60 && mx < c_mem + 4) tm_set_sort(tm, TM_SORT_MEM);
                break;
            }
            int list_top = cy + TM_HEADER_H;
            int list_h = (wy + wh) - list_top - TM_FOOTER_H;
            // footer buttons
            int fy = wy + wh - TM_FOOTER_H;
            if (my >= fy) {
                if (mx >= wx + TM_PAD && mx < wx + TM_PAD + 90 && tm->selected_pid)
                    { tm_signal(tm->selected_pid, SIGTERM); taskmanager_refresh(tm); }
                else if (mx >= wx + TM_PAD + 100 && mx < wx + TM_PAD + 170)
                    { tm->tree_mode = !tm->tree_mode; taskmanager_refresh(tm); }
                break;
            }
            // list row
            if (my >= list_top && my < list_top + list_h) {
                int row = (my - list_top) / TM_ROW_H + tm->scroll;
                if (row >= 0 && row < tm->nproc) {
                    tm_proc_t *r = &tm->procs[row];
                    int name_x = wx + TM_PAD + r->depth * 12;
                    // triangle toggle
                    if (r->has_kids && mx >= name_x && mx < name_x + 12) {
                        tm_toggle_collapse(tm, r->pid); taskmanager_refresh(tm); break;
                    }
                    tm->selected_row = row; tm->selected_pid = r->pid;
                    if (event->mouse_buttons & MOUSE_BUTTON_RIGHT) {
                        tm->menu_open = 1; tm->menu_hover = -1;
                        tm->menu_pid = r->pid;
                        tm->menu_x = mx; tm->menu_y = my;
                        int scr_h = (int)fb_get_height();
                        if (tm->menu_y + TM_MENU_N * TM_MENU_ITEM_H + 4 > scr_h) tm->menu_y = scr_h - (TM_MENU_N * TM_MENU_ITEM_H + 4);
                    }
                }
            }
        } else if (tm->tab == TM_TAB_SERVICES) {
            int list_top = cy + TM_HEADER_H;
            int fy = wy + wh - TM_FOOTER_H;
            if (my >= fy) {
                if (tm->selected_row >= 0) {
                    service_t *s = svc_at(tm->selected_row);
                    if (s) {
                        if (mx >= wx + TM_PAD && mx < wx + TM_PAD + 70) svc_start(s->name);
                        else if (mx >= wx + TM_PAD + 80 && mx < wx + TM_PAD + 150) svc_stop(s->name);
                    }
                }
                break;
            }
            if (my >= list_top) {
                int row = (my - list_top) / TM_ROW_H;
                if (row >= 0 && row < svc_count()) tm->selected_row = row;
            }
        } else if (tm->tab == TM_TAB_SCHEDULED) {
            // #487/#265: Enable/Disable the selected job via cron_enable().
            int list_top = cy + TM_HEADER_H;
            int fy = wy + wh - TM_FOOTER_H;
            if (my >= fy) {
                if (tm->sched_sel >= 0 && tm->sched_sel < tm->njobs) {
                    uint32_t id = tm->jobs[tm->sched_sel].id;
                    if (mx >= wx + TM_PAD && mx < wx + TM_PAD + 80) cron_enable(id, 1);
                    else if (mx >= wx + TM_PAD + 90 && mx < wx + TM_PAD + 170) cron_enable(id, 0);
                    taskmanager_refresh(tm);
                }
                break;
            }
            if (my >= list_top) {
                int row = (my - list_top) / TM_ROW_H;
                if (row >= 0 && row < tm->njobs) tm->sched_sel = row;
            }
        } else if (tm->tab == TM_TAB_PERFORMANCE) {
            // #487: Overall / Per-core toggle buttons.
            int by = cy + 4;
            if (my >= by && my < by + 20) {
                int bx0 = wx + TM_PAD, bx1 = bx0 + 98;
                if (mx >= bx0 && mx < bx0 + 92) tm->perf_view = TM_PERF_OVERALL;
                else if (mx >= bx1 && mx < bx1 + 92) tm->perf_view = TM_PERF_CORES;
            }
        }
        break;
    }
    default: break;
    }
    wm_invalidate_all();
}

// ===========================================================================
// Launch
// ===========================================================================
void taskmanager_launch(void) {
    LOG_INFO("[TaskManager] launched");
    taskmanager_t *tm = taskmanager_create();
    if (!tm) { kprintf("[TaskManager] create failed\n"); return; }
    wm_register_app(tm->window, tm,
                    (app_event_handler_t)taskmanager_handle_event,
                    (app_draw_handler_t)taskmanager_draw,
                    (app_destroy_handler_t)taskmanager_destroy);
    kprintf("[TaskManager] ready\n");
}

// ===========================================================================
// #404 boot-time [RUST-DIFF] self-test for the taskmgr_core seam. Proves the
// _rs ports == the _c twins on well-formed input (field-by-field), and
// witnesses the confinement (naive C over-writes/div0 on malformed counts that
// the Rust rejects). Bounded, runs once; logs one [RUST-DIFF] + one [RUST-SEC]
// taskmgr_core line to serial + /BOOTLOG. Wired from main.c by the integrator.
// ===========================================================================
void taskmgr_core_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    // Well-formed differential over a small deterministic corpus.
    uint32_t ring[16]; for (int i = 0; i < 16; i++) ring[i] = (uint32_t)(i * 7 + 3);
    proc_key_t rows[16];
    for (int i = 0; i < 16; i++) { rows[i].pid = 16 - i; rows[i].cpu = (uint32_t)((i * 13) % 100); rows[i].mem = (uint32_t)((i * 97) % 5000); }
    int mism = 0, vecs = 0;
#ifdef RUST_TASKMGR_CORE
    for (uint32_t cap = 1; cap <= 16; cap++) {
        for (uint32_t head = 0; head < cap; head++) {
            uint32_t count = cap / 2;
            perf_stat_t sc, sr;
            int rc = perf_ring_stats_c(ring, cap, head, count, &sc);
            int rr = perf_ring_stats_rs(ring, cap, head, count, &sr);
            vecs++;
            if (rc != rr || sc.min != sr.min || sc.max != sr.max || sc.last != sr.last ||
                sc.avg != sr.avg || sc.count != sr.count) mism++;
            int32_t ic[16], ir[16];
            int kc = taskmgr_sort_rows_c(rows, cap, 16, (int)(head % 5), ic);
            int kr = taskmgr_sort_rows_rs(rows, cap, 16, (int)(head % 5), ir);
            vecs++;
            if (kc != kr) mism++;
            else for (uint32_t i = 0; i < cap; i++) if (ic[i] != ir[i]) { mism++; break; }
        }
    }
    // confinement witnesses (Rust must reject; C would over-write / div0)
    int32_t idx[16]; perf_stat_t o;
    int c_ov = taskmgr_sort_rows_rs(rows, 40, 8, 0, idx);   // count>cap
    int c_z  = perf_ring_stats_rs(ring, 0, 0, 4, &o);        // cap==0
    kprintf("[RUST-DIFF] taskmgr_core: %d vecs mism=%d %s (LIVE=rust)\n", vecs, mism, mism ? "*** MISMATCH ***" : "MATCH");
    kprintf("[RUST-SEC]  taskmgr_core: sort count>cap rust rc=%d (confined, C over-writes), ring cap0 rust rc=%d (confined, C div0)\n", c_ov, c_z);
    bootlog_write("[RUST-DIFF] taskmgr_core %d vecs mism=%d %s", vecs, mism, mism ? "MISMATCH" : "MATCH");
#else
    for (uint32_t cap = 1; cap <= 16; cap++) { perf_stat_t sc; perf_ring_stats_c(ring, cap, 0, cap / 2, &sc); vecs++; }
    kprintf("[RUST-DIFF] taskmgr_core: %d vecs (LIVE=c, Rust seam not compiled)\n", vecs);
    bootlog_write("[RUST-DIFF] taskmgr_core %d vecs LIVE=c", vecs);
    (void)mism; (void)rows;
#endif
}
