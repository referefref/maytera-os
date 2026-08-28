// idleprof.c - #COMPIDLE. See idleprof.h for the full rationale.
//
// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.

#include <stdint.h>
#include "idleprof.h"
#include "../../libc/syscall.h"
#include "../../libc/fcntl.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"
#include "../../libc/pwd.h"
#include "../../libc/unistd.h"

// ---------------------------------------------------------------------------
// TWO DELIVERY CHANNELS, BECAUSE THE FIRST ONE THIS FILE TRIED SILENTLY DID
// NOTHING FOR 450 SECONDS.
//
// The first version wrote one long record to /COMPIDLE.TXT and echoed it with
// sys_bootlog(). MEASURED on VM <vmid>: the dump function ran (proved with a
// throwaway counter, the interval clock advanced and the accumulators reset),
// and NEITHER output existed. Two independent silent failures:
//
//   1. sys_bootlog_write() bounces the user string into a `char buf[200]` and
//      RETURNS -1 if it does not fit. It does not truncate. The record was
//      about 300 bytes, so every call was rejected, and the return value was
//      being ignored, so it looked exactly like the code never ran.
//   2. sys_open("/COMPIDLE.TXT", O_WRONLY|O_CREAT|O_TRUNC) failed. The
//      compositor runs as uid 1000 and the ext2 root is not its to write in.
//      profile.c and stickies.c both already know this - each writes to a
//      per-user path FIRST and only falls back to the root - and this file did
//      not copy that pattern.
//
// So: every record is now split into TWO lines that each fit inside the
// 200-byte syscall bounce, the file is attempted at the per-user path first,
// and the file is a BONUS rather than the channel. sys_bootlog() is the one
// that has actually been observed working from this process (a short probe
// line landed in /BOOTLOG.TXT on the same boot where the long one vanished),
// so the diagnostic rides that and cannot be lost to a permission.
// ---------------------------------------------------------------------------
#define IP_DUMP_MS      30000u      // one verdict every 30s
#define IP_RING_RECS    16          // ~8 minutes of history in the file
#define IP_LINE_MAX     192         // MUST stay under sys_bootlog_write()'s 200

static inline uint64_t ip_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// The compositor's screen size. Every composite cost scales with it, so a
// record that omits it cannot be compared between two machines: a VM at
// 1280x800 and a laptop at 1920x1080 differ by 2.02x in pixels before anything
// else is considered.
extern int32_t g_fb_width;
extern int32_t g_fb_height;

// ---- per-interval accumulators -------------------------------------------
static uint64_t s_busy_cyc;          // cycles inside the loop body
static uint64_t s_wall_cyc;          // cycles from one tick_begin to the next
static uint64_t s_tick_begin;
static uint64_t s_prev_begin;
static uint64_t s_max_tick_cyc;      // slowest single tick this interval
static uint32_t s_ticks;
static uint32_t s_reason_n[IP_R_COUNT];
static uint32_t s_path_n[IP_P_COUNT];
static uint64_t s_damage_px;         // pixels the compositor ASKED to present

static unsigned s_cur_reason;
static int      s_cur_path;

// Present cost, measured from Ring 3. See idleprof.h.
static uint64_t s_flip_cyc;
static uint32_t s_flips;
static uint64_t s_flip_max_cyc;

// Raw pointer activity. A hand resting on a laptop touchpad produces a steady
// drizzle of one-pixel deltas; a still mouse produces exactly none. That
// distinction is invisible in recent_input (both look identical) and it is the
// difference between a device problem and a compositor problem.
static uint32_t s_motion_ticks;
static uint64_t s_motion_px;

// ---- dump bookkeeping ----------------------------------------------------
static uint64_t s_next_dump_ms;
static uint64_t s_prev_us;
static uint64_t s_prev_dump_tsc;
static int      s_seq;
static char     s_ring[IP_RING_RECS][2][IP_LINE_MAX];
static int      s_ring_n;
static int      s_ring_next;

void idleprof_tick_begin(void) {
    uint64_t now = ip_tsc();
    if (s_prev_begin) s_wall_cyc += now - s_prev_begin;
    s_prev_begin = now;
    s_tick_begin = now;
    s_cur_reason = 0;
    s_cur_path   = IP_P_NOTHING;
}

void idleprof_reason(unsigned bits) { s_cur_reason |= bits; }
void idleprof_path(int path) {
    if (path >= 0 && path < IP_P_COUNT) s_cur_path = path;
}
void idleprof_damage_px(unsigned long px) { s_damage_px += px; }

int idleprof_flip(void) {
    uint64_t t0 = ip_tsc();
    int r = fb_flip();
    uint64_t d = ip_tsc() - t0;
    s_flip_cyc += d;
    if (d > s_flip_max_cyc) s_flip_max_cyc = d;
    s_flips++;
    return r;
}

void idleprof_motion(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    s_motion_ticks++;
    s_motion_px += (uint64_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
}

static void ip_dump(void);

void idleprof_tick_end(void) {
    uint64_t d = ip_tsc() - s_tick_begin;
    s_busy_cyc += d;
    if (d > s_max_tick_cyc) s_max_tick_cyc = d;
    s_ticks++;
    for (int i = 0; i < IP_R_COUNT; i++)
        if (s_cur_reason & (1u << i)) s_reason_n[i]++;
    s_path_n[s_cur_path]++;

    // The dump cadence is checked with the tick-derived clock (coarse is fine
    // for a 30s period) so the hot path stays free of the finer SYS_MONO_US.
    uint64_t now_ms = (uint64_t)uptime_ms();
    if (!s_next_dump_ms) {
        s_next_dump_ms = now_ms + IP_DUMP_MS;
        // Take the rate baseline HERE, not at the first dump. Without this,
        // record #0 reports dt=0s and therefore ticks/s, Kpx/s, flip-us and
        // maxtick all read 0 - and #0 is the one record a short session is
        // guaranteed to leave behind, so it is the one that most needs to be
        // usable. MEASURED: it printed "dt=0s ticks=302(0/s)" before this.
        s_prev_us       = mono_us();
        s_prev_dump_tsc = ip_tsc();
        return;
    }
    if (now_ms >= s_next_dump_ms) {
        s_next_dump_ms = now_ms + IP_DUMP_MS;
        ip_dump();
    }
}

static void ip_write_all(int fd, const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    if (n) sys_write(fd, s, n);
}

// Per-user path first, root as a fallback: the same order profile.c and
// stickies.c use, and for the same reason (the compositor is uid 1000 and the
// ext2 root is not writable by it).
static void ip_path(char *out, int cap) {
    struct passwd *pw = getpwuid(getuid());
    const char *d = (pw && pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : "/";
    int i = 0;
    if (!(d[0] == '/' && d[1] == '\0')) {
        while (d[i] && i < cap - 20) { out[i] = d[i]; i++; }
        if (i > 0 && out[i-1] == '/') i--;
    }
    const char *f = "/COMPIDLE.TXT"; int j = 0;
    while (f[j] && i < cap - 1) out[i++] = f[j++];
    out[i] = '\0';
}

static void ip_dump(void) {
    // Calibrate cycles -> real time from THIS interval rather than trusting a
    // published TSC rate: two SYS_MONO_US calls per 30 seconds, and the ratio
    // is right even if the platform rate was never calibrated.
    uint64_t now_us  = mono_us();
    uint64_t now_tsc = ip_tsc();
    uint64_t d_us    = (s_prev_us && now_us > s_prev_us) ? (now_us - s_prev_us) : 0;
    uint64_t d_tsc   = (s_prev_dump_tsc && now_tsc > s_prev_dump_tsc)
                     ? (now_tsc - s_prev_dump_tsc) : 0;
    s_prev_us = now_us; s_prev_dump_tsc = now_tsc;

    // busy% is busy cycles over WALL cycles, which needs no calibration at all
    // and is therefore the number to trust if the two ever disagree.
    unsigned long busy_pct   = s_wall_cyc ? (unsigned long)(s_busy_cyc * 100ULL / s_wall_cyc) : 0;
    unsigned long cyc_per_us = (d_us && d_tsc) ? (unsigned long)(d_tsc / d_us) : 0;
    unsigned long maxtick_us = cyc_per_us ? (unsigned long)(s_max_tick_cyc / cyc_per_us) : 0;
    unsigned long secs       = d_us ? (unsigned long)(d_us / 1000000ULL) : 0;
    unsigned long tps        = secs ? (unsigned long)(s_ticks / secs) : 0;
    unsigned long kpxs       = secs ? (unsigned long)((s_damage_px / secs) / 1000ULL) : 0;
    unsigned long flip_us    = (s_flips && cyc_per_us)
                             ? (unsigned long)((s_flip_cyc / s_flips) / cyc_per_us) : 0;
    unsigned long flipmax_us = cyc_per_us ? (unsigned long)(s_flip_max_cyc / cyc_per_us) : 0;
    unsigned long flip_pct   = s_wall_cyc ? (unsigned long)(s_flip_cyc * 100ULL / s_wall_cyc) : 0;
    // Cycles per 1000 pixels presented: the memory-type fingerprint. Scaled by
    // 1000 because a per-pixel figure is a small integer that rounds to zero.
    unsigned long cyc_kpx    = s_damage_px
                             ? (unsigned long)(s_flip_cyc * 1000ULL / s_damage_px) : 0;

    int seq = s_seq++;
    char *a = s_ring[s_ring_next][0];
    char *b = s_ring[s_ring_next][1];

    snprintf(a, IP_LINE_MAX,
             "[COMPIDLE] #%d %ldx%ld up=%lus dt=%lus ticks=%lu(%lu/s) busy=%lu%% "
             "maxtick=%luus flip=%lu %luus/max%luus %lu%% cyc/Kpx=%lu Kpx/s=%lu mot=%lu/%lu",
             seq, (long)g_fb_width, (long)g_fb_height,
             (unsigned long)uptime_ms() / 1000UL, secs,
             (unsigned long)s_ticks, tps, busy_pct, maxtick_us,
             (unsigned long)s_flips, flip_us, flipmax_us, flip_pct, cyc_kpx, kpxs,
             (unsigned long)s_motion_ticks, (unsigned long)s_motion_px);

    snprintf(b, IP_LINE_MAX,
             "[COMPIDLE] #%d path none=%lu idle=%lu chrome=%lu cursor=%lu full=%lu ss=%lu"
             " why rin=%lu dirty=%lu wdg=%lu tb=%lu ntf=%lu menu=%lu drag=%lu rdrw=%lu"
             " lock=%lu anim=%lu ss=%lu quiet=%lu",
             seq,
             (unsigned long)s_path_n[IP_P_NOTHING],
             (unsigned long)s_path_n[IP_P_IDLE],
             (unsigned long)s_path_n[IP_P_CHROME],
             (unsigned long)s_path_n[IP_P_CURSOR],
             (unsigned long)s_path_n[IP_P_FULL],
             (unsigned long)s_path_n[IP_P_SCRSAVER],
             (unsigned long)s_reason_n[0],  (unsigned long)s_reason_n[1],
             (unsigned long)s_reason_n[8],  (unsigned long)s_reason_n[9],
             (unsigned long)s_reason_n[10], (unsigned long)s_reason_n[4],
             (unsigned long)s_reason_n[3],  (unsigned long)s_reason_n[7],
             (unsigned long)s_reason_n[5],  (unsigned long)s_reason_n[6],
             (unsigned long)s_reason_n[2],  (unsigned long)s_reason_n[11]);

    s_ring_next = (s_ring_next + 1) % IP_RING_RECS;
    if (s_ring_n < IP_RING_RECS) s_ring_n++;

    // /BOOTLOG.TXT is the channel that is KNOWN to work from this process and
    // the first file anyone is asked for. Every record for the first ten (five
    // minutes, which is what a diagnostic session needs), then one in ten.
    // bootlog_write() rewrites its whole buffer and a 1 Hz version of that is
    // what #373/#748 had to remove, so the rate limit is structural.
    if (seq < 10 || (seq % 10) == 0) { sys_bootlog(a); sys_bootlog(b); }

    // Reset the interval. Lifetime totals are deliberately not kept: a
    // lifetime average of an idle-CPU fault is dominated by boot and says
    // nothing about the steady state anyone is complaining about.
    s_busy_cyc = s_wall_cyc = s_max_tick_cyc = 0;
    s_ticks = 0; s_damage_px = 0;
    s_flip_cyc = s_flip_max_cyc = 0; s_flips = 0;
    s_motion_ticks = 0; s_motion_px = 0;
    for (int i = 0; i < IP_R_COUNT; i++) s_reason_n[i] = 0;
    for (int i = 0; i < IP_P_COUNT; i++) s_path_n[i] = 0;

    // The file is a convenience on top of that: the whole bounded ring,
    // rewritten in place, so it can never grow without bound on a machine left
    // running for days. Per-user path first (see ip_path); the ext2 root is
    // tried second and BOTH failing is survivable, because the bootlog above
    // already carries the record.
    char path[128];
    ip_path(path, sizeof(path));
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) fd = sys_open("/COMPIDLE.TXT", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    ip_write_all(fd,
        "MayteraOS compositor idle profile (#COMPIDLE). Newest record last.\n"
        "busy% is the compositor's OWN Ring-3 CPU as a fraction of wall clock.\n"
        "flip= is the present syscall timed from Ring 3; cyc/Kpx is the\n"
        "memory-type fingerprint. 'why' counts ticks each condition forced a\n"
        "non-idle path. Also mirrored into /BOOTLOG.TXT.\n");
    int start = (s_ring_n == IP_RING_RECS) ? s_ring_next : 0;
    for (int i = 0; i < s_ring_n; i++) {
        int k = (start + i) % IP_RING_RECS;
        ip_write_all(fd, s_ring[k][0]); ip_write_all(fd, "\n");
        ip_write_all(fd, s_ring[k][1]); ip_write_all(fd, "\n");
    }
    sys_close(fd);
}
