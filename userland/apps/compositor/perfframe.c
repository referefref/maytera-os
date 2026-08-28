// perfframe.c - #62 revalidation: a real frame-interval instrument.
// See perfframe.h for the full rationale.
//
// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.

#include <stdint.h>
#include "perfframe.h"
#include "../../libc/syscall.h"
#include "../../libc/fcntl.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------

#define PF_GATE_PATH    "/CONFIG/PERF62.CFG"   // existence-only gate, ext2 root
#define PF_OUT_PATH     "/PERF62.TXT"          // verdict log, ext2 root, sibling
                                                // of /BOOTLOG.TXT and /HEARTBEAT.TXT
#define PF_GATE_POLL_MS   2000   // matches dock_style_poll()'s throttle class
#define PF_DUMP_PERIOD_MS 10000  // one verdict block roughly every 10s while armed
#define PF_RING_CAP       256    // samples per path, ring (overwrite oldest)
#define PF_OVER_MS        100    // ">100ms" bucket the brief asked for

typedef enum {
    PF_IDLE = 0,
    PF_CURSOR,
    PF_INTERACTIVE,
    PF_WINDOWED,
    PF_CHROME,
    PF_FULLSCREEN,
    PF_SCREENSAVER,
    PF_LOCK,
    PF_TAG_COUNT
} pf_tag_t;

static const char *const PF_TAGS[PF_TAG_COUNT] = {
    "idle", "cursor", "interactive", "windowed",
    "chrome", "fullscreen", "screensaver", "lock",
};

// One ring per path. Values are the interval (real microseconds, SYS_MONO_US)
// since the PREVIOUS present on the same path - never a global present rate,
// because #62's whole premise is that paths differ (a windowed redraw is not
// a fullscreen present is not an idle tick).
typedef struct {
    uint32_t us[PF_RING_CAP];
    int      count;     // samples written so far, saturates at PF_RING_CAP
    int      next;      // next ring slot to write
    uint64_t last_ts_us; // mono_us() at the previous mark(), 0 = none yet
    uint64_t total;      // lifetime present count on this path (not ring-limited)
} pf_ring_t;

static pf_ring_t s_ring[PF_TAG_COUNT];

static int      s_gate_on   = 0;   // -1 unknown, 0 off, 1 on (see perfframe_poll)
static uint64_t s_next_gate_poll_ms = 0;
static uint64_t s_next_dump_ms      = 0;
static int      s_dump_seq  = 0;

// #62: latest mono_us() at which SOME exclusive display state (lock, or the
// screensaver's #652 blank-after steady state) confirmed it is still
// deliberately presenting nothing. See perfframe_note_quiescent() /
// perfframe.h for the full rationale - this is the floor perfframe_mark()
// applies to its interval baseline so a benign, intentional silence is never
// reported as a stall on whichever path presents next.
static uint64_t s_last_quiescent_us = 0;

// ----------------------------------------------------------------------------
// Ring bookkeeping
// ----------------------------------------------------------------------------

static pf_tag_t pf_find_tag(const char *tag) {
    if (!tag) return PF_TAG_COUNT;
    for (int i = 0; i < PF_TAG_COUNT; i++) {
        if (strcmp(tag, PF_TAGS[i]) == 0) return (pf_tag_t)i;
    }
    return PF_TAG_COUNT;   // unknown: caller treats as "not found"
}

void perfframe_mark(const char *tag) {
    if (!s_gate_on) return;      // the common case: one branch, no syscall
    pf_tag_t t = pf_find_tag(tag);
    if (t == PF_TAG_COUNT) return;   // unknown tag: dropped, not fatal

    pf_ring_t *r = &s_ring[t];
    uint64_t now = mono_us();    // TSC-backed real time - see perfframe.h
    r->total++;
    if (r->last_ts_us != 0 && now > r->last_ts_us) {
        // #62: floor the interval baseline at the latest confirmed quiescent
        // mark (lock / screensaver blank-after - see perfframe_note_quiescent()
        // in perfframe.h). A present that follows one of those exclusive
        // states is timed from when the display became presentable again,
        // never from further back than that, so a legitimate 20-minute
        // blanked-screensaver silence is not reported as a 20-minute stall
        // on whichever path happens to present next. An ordinary busy present
        // is unaffected: last_ts_us is already newer than any stale
        // quiescent stamp, so baseline == last_ts_us exactly as before.
        uint64_t baseline = r->last_ts_us;
        if (s_last_quiescent_us > baseline && s_last_quiescent_us < now)
            baseline = s_last_quiescent_us;
        uint64_t delta = now - baseline;
        // Clamp to uint32_t range (~71 minutes) rather than wrap silently;
        // an interval that long is already off the chart for this ticket.
        uint32_t d32 = (delta > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)delta;
        r->us[r->next] = d32;
        r->next = (r->next + 1) % PF_RING_CAP;
        if (r->count < PF_RING_CAP) r->count++;
    }
    r->last_ts_us = now;
}

// See perfframe.h for the full rationale. Called once per tick from every
// main.c site where an exclusive display state (lock, screensaver
// blank-after) is about to return without presenting anything - i.e. every
// tick that path structurally cannot be interrupted by chrome/windowed/
// interactive/cursor/fullscreen. Bounded, no allocation, no wait: same cost
// class as perfframe_mark() and a no-op when the gate is off.
void perfframe_note_quiescent(void) {
    if (!s_gate_on) return;
    s_last_quiescent_us = mono_us();
}

// ----------------------------------------------------------------------------
// Stats: min / median(p50) / p95 / max / count-over-threshold, in whole+tenth
// ms (fixed point - NOT %f: #621 was a real printf(%f) stack smash in this
// libc, so this file never passes a float to snprintf).
// ----------------------------------------------------------------------------

static void pf_insertion_sort(uint32_t *a, int n) {
    for (int i = 1; i < n; i++) {
        uint32_t key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
}

// Formats "whole.tenth" ms from a microsecond value into buf (caller-sized).
static void pf_fmt_ms(char *buf, int bufsz, uint32_t us) {
    uint32_t whole = us / 1000;
    uint32_t tenth = (us % 1000) / 100;
    snprintf(buf, bufsz, "%u.%u", whole, tenth);
}

// ----------------------------------------------------------------------------
// Gate + periodic dump
// ----------------------------------------------------------------------------

static int pf_file_exists(const char *path) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    sys_close(fd);
    return 1;
}

static void pf_write(int fd, const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    if (n) sys_write(fd, s, n);
}

static void pf_dump_verdict(void) {
    int fd = sys_open(PF_OUT_PATH, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) return;   // best-effort: never let logging wedge the compositor

    char line[256];
    snprintf(line, sizeof(line),
             "\n=== FRAME VERDICT (perf62, #%d, uptime=%lums) ===\n",
             s_dump_seq++, (unsigned long)uptime_ms());
    pf_write(fd, line);
    pf_write(fd, "path         n     min    p50    p95    max  >100ms\n");

    uint32_t worst_max_us = 0;
    int worst_tag = -1;
    int any_samples = 0;

    // Local scratch copy so sorting never disturbs the live ring while a
    // mark() could in principle land mid-dump (single-threaded compositor,
    // so it can't today, but this keeps the function correct if that ever
    // changes without anyone having to remember why).
    static uint32_t scratch[PF_RING_CAP];

    for (int t = 0; t < PF_TAG_COUNT; t++) {
        pf_ring_t *r = &s_ring[t];
        int n = r->count;
        if (n <= 0) continue;
        any_samples = 1;
        for (int i = 0; i < n; i++) scratch[i] = r->us[i];
        pf_insertion_sort(scratch, n);

        int over = 0;
        for (int i = 0; i < n; i++) if (scratch[i] > PF_OVER_MS * 1000u) over++;

        uint32_t vmin = scratch[0];
        uint32_t vmed = scratch[n / 2];
        int p95i = (n * 95) / 100; if (p95i >= n) p95i = n - 1;
        uint32_t vp95 = scratch[p95i];
        uint32_t vmax = scratch[n - 1];

        if (vmax > worst_max_us) { worst_max_us = vmax; worst_tag = t; }

        char smin[16], smed[16], sp95[16], smax[16];
        pf_fmt_ms(smin, sizeof(smin), vmin);
        pf_fmt_ms(smed, sizeof(smed), vmed);
        pf_fmt_ms(sp95, sizeof(sp95), vp95);
        pf_fmt_ms(smax, sizeof(smax), vmax);

        snprintf(line, sizeof(line), "%-11s %4d %7s %6s %6s %6s %7d\n",
                 PF_TAGS[t], n, smin, smed, sp95, smax, over);
        pf_write(fd, line);
    }

    if (!any_samples) {
        pf_write(fd, "(no presents observed on any path yet)\n");
    } else {
        char smax[16];
        pf_fmt_ms(smax, sizeof(smax), worst_max_us);
        if (worst_max_us > 1000000u) {   // > 1000ms == "several seconds" territory
            snprintf(line, sizeof(line),
                     "VERDICT: REPRODUCED on path '%s' - worst single interval "
                     "%sms (>1s)\n", PF_TAGS[worst_tag], smax);
        } else {
            snprintf(line, sizeof(line),
                     "VERDICT: NOT reproduced here - worst single interval across "
                     "all paths is %sms on '%s'\n", smax, PF_TAGS[worst_tag]);
        }
        pf_write(fd, line);
    }
    sys_close(fd);
}

void perfframe_poll(void) {
    uint64_t now = uptime_ms();   // gate/dump cadence: coarse, tick-based is fine here

    if (now >= s_next_gate_poll_ms) {
        s_next_gate_poll_ms = now + PF_GATE_POLL_MS;
        int was_on = s_gate_on;
        s_gate_on = pf_file_exists(PF_GATE_PATH) ? 1 : 0;
        if (s_gate_on && !was_on) {
            // Just armed: reset rings so a stale prior session's samples
            // never bleed into a fresh one, and log an unmistakable marker
            // to serial via bootlog so "was it even on" is never a guess.
            for (int t = 0; t < PF_TAG_COUNT; t++) {
                s_ring[t].count = 0;
                s_ring[t].next = 0;
                s_ring[t].last_ts_us = 0;
                s_ring[t].total = 0;
            }
            s_last_quiescent_us = 0;   // #62: no stale floor from a prior session
            s_next_dump_ms = now + PF_DUMP_PERIOD_MS;
            sys_bootlog("[PERF62] gate ON (" PF_GATE_PATH " present): frame-interval "
                        "instrument armed, logging to " PF_OUT_PATH);
        } else if (!s_gate_on && was_on) {
            sys_bootlog("[PERF62] gate OFF: instrument disarmed");
        }
    }

    if (!s_gate_on) return;

    if (now >= s_next_dump_ms) {
        s_next_dump_ms = now + PF_DUMP_PERIOD_MS;
        pf_dump_verdict();
    }
}
