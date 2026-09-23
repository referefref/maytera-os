// COMPMULT - #smpreval2: a MULTI-WINDOW content pump that survives resizes.
//
// WHY THIS EXISTS, and why COMPCEIL beside it could not answer the question.
//
// #168 stage 2 (ON by default since 2026-09-04; /NOINVNARROW.TXT forces it off,
// /INVNARROW.TXT still forces it on) makes sys_win_invalidate()'s content-commit
// memcpy run with the BKL dropped, by keeping a SPARE copy of the window's
// content buffer alive while the copy is in flight. It WAS gated OFF for a
// MEMORY reason, not a correctness one, and THIS APP IS WHAT SETTLED THAT: the
// multi-window and repeated-resize numbers it produced are why stage 2 became
// the shipping default on 2026-09-04. At the time of writing: one 3200x2200
// COMPCEIL window costs a
// 27.8 MB spare on a 256 MB heap, and INVNARROW_SPARE_BUDGET caps the total at
// 48 MB. What was never measured is the case the budget exists for:
//
//   * SEVERAL windows committing at once, each holding its own spare, and
//   * the SAME windows resized repeatedly, which frees and reallocates every
//     buffer involved and is where heap fragmentation would show up.
//
// COMPCEIL creates exactly ONE window and never resizes it, so every
// fallback counter (nospare/stale/gone/nested/noblock/recommit/deferred) read
// exactly zero across 122,767 unlocked commits - which is evidence about the
// single-window case and nothing else. A budget that is never approached is
// not a budget that has been tested.
//
// WHAT IT DOES. Creates N windows, gives each its own ARGB buffer, and pumps
// perturbed content into ALL of them back to back (blit + invalidate), unpaced,
// exactly like COMPCEIL: whatever the compositor presents is its own ceiling.
// It also SERVICES EVENT_RESIZE, re-querying its size and reallocating the
// buffer, so a WM-driven drag-resize from the #334 testinput channel produces
// the real free/alloc churn rather than a window whose content buffer never
// moves.
//
// CONFIG, read from /CONFIG/COMPMULT.CFG so an arm can be changed without a
// rebuild (AUTORUN.CFG passes no arguments - kernel/gui/desktop.c reads one
// path and calls launch_userspace_app(path) - so argv is not available to an
// autorun-launched harness, the trap that cost #compceiling a headline number):
//
//   wins=<n>   number of windows      (default 4, max 8)
//   w=<px>     requested content width  (default 900)
//   h=<px>     requested content height (default 700)
//   secs=<n>   run length             (default 600, longer than any capture)
//
// Defaults are deliberately longer-lived than the measurement: an app that
// exits inside the capture window is a trap that fires silently and looks like
// data.
//
// OUTPUT: every 5 s,
//   [COMPMULT] t=Xs wins=N frames=K push=Y/s bytes=Z resizes=R sizes=WxH,...
// `bytes` is this process's own held content, the number the kernel-side
// spare_kb in [INVNARROW] has to be read against.
//
// Measurement harness, not a feature: listed in build/unshipped-apps.list
// beside COMPCEIL and BLITBNCH.
//
// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"
#include "gui.h"

#define SYS_WIN_BLIT_NUM 35
#define MAXW 8

static long blit_raw(int h, unsigned int sw, unsigned int sh, unsigned long p) {
    unsigned long packed = (sw & 0xFFFFu) | ((sh & 0xFFFFu) << 16);
    return syscall5(SYS_WIN_BLIT_NUM, (long)h, 0, 0, (long)packed, (long)p);
}

static void emit(const char *s) { printf("%s\n", s); sys_bootlog(s); }

// No trustworthy snprintf in this freestanding subset (BLITBNCH's own comment,
// still true here): assemble lines by hand.
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *putu(char *p, unsigned long long v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

// Minimal key=value reader. Returns the value for `key`, or `dflt`.
static int cfg_int(const char *blob, const char *key, int dflt) {
    const char *p = blob;
    int klen = 0; while (key[klen]) klen++;
    while (*p) {
        int i = 0;
        while (i < klen && p[i] == key[i]) i++;
        if (i == klen && p[i] == '=' && (p == blob || p[-1] == '\n' || p[-1] == ' ')) {
            int v = 0, got = 0; const char *q = p + klen + 1;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; got = 1; }
            if (got) return v;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return dflt;
}

int main(void) {
    char cfg[256];
    int n = 0;
    int fd = sys_open("/CONFIG/COMPMULT.CFG", 0);
    if (fd >= 0) {
        long r = sys_read(fd, cfg, sizeof(cfg) - 1);
        if (r > 0) n = (int)r;
        sys_close(fd);
    }
    cfg[n] = 0;

    int nwin  = cfg_int(cfg, "wins", 4);
    int reqw  = cfg_int(cfg, "w", 900);
    int reqh  = cfg_int(cfg, "h", 700);
    int secs  = cfg_int(cfg, "secs", 600);
    if (nwin < 1) nwin = 1;
    if (nwin > MAXW) nwin = MAXW;
    if (reqw < 64) reqw = 64;
    if (reqh < 64) reqh = 64;

    {
        char line[192]; char *p = line;
        p = put(p, "[COMPMULT] start wins="); p = putu(p, (unsigned)nwin);
        p = put(p, " req="); p = putu(p, (unsigned)reqw);
        *p++ = 'x'; p = putu(p, (unsigned)reqh);
        p = put(p, " secs="); p = putu(p, (unsigned)secs);
        p = put(p, " cfgbytes="); p = putu(p, (unsigned)n);
        *p = 0; emit(line);
    }

    int win[MAXW], w[MAXW], h[MAXW];
    unsigned int *buf[MAXW];
    int live = 0;
    for (int i = 0; i < nwin; i++) {
        // Stagger so the windows overlap partially: an overlapped stack is what
        // makes the compositor composite more than one window per present.
        int x = 20 + i * 90, y = 20 + i * 60;
        win[i] = win_create("compmult", x, y, reqw, reqh);
        if (win[i] < 0) { emit("[COMPMULT] ABORT win_create failed"); break; }
        w[i] = h[i] = 0;
        if (win_get_size(win[i], &w[i], &h[i]) != 0 || w[i] < 1 || h[i] < 1) {
            emit("[COMPMULT] ABORT win_get_size failed"); break;
        }
        buf[i] = (unsigned int *)malloc((size_t)w[i] * (size_t)h[i] * 4u);
        if (!buf[i]) { emit("[COMPMULT] ABORT malloc failed"); break; }
        live++;
    }
    if (live < 1) { emit("[COMPMULT] ABORT no live window"); return 1; }
    {
        char line[192]; char *p = line;
        p = put(p, "[COMPMULT] live="); p = putu(p, (unsigned)live);
        p = put(p, " granted=");
        for (int i = 0; i < live; i++) {
            if (i) *p++ = ',';
            p = putu(p, (unsigned)w[i]); *p++ = 'x'; p = putu(p, (unsigned)h[i]);
        }
        *p = 0; emit(line);
    }

    unsigned long long t_start = mono_us(), t_report = t_start;
    unsigned long long frames = 0, frames_at_report = 0, resizes = 0;

    for (;;) {
        unsigned long long now = mono_us();
        if (secs > 0 && (now - t_start) >= (unsigned long long)secs * 1000000ULL) break;

        for (int i = 0; i < live; i++) {
            // Drain this window's events. EVENT_RESIZE is the one that matters:
            // the WM has already replaced the KERNEL-side content buffer, and
            // an app that keeps blitting its old dimensions is measuring a
            // rejected blit, not a commit. timeout 0 = poll, never block: this
            // pump is deliberately unpaced.
            gui_event_t ev;
            while (win_get_event(win[i], &ev, 0) > 0) {
                if (ev.type == EVENT_RESIZE) {
                    int nw = 0, nh = 0;
                    if (win_get_size(win[i], &nw, &nh) == 0 && nw > 0 && nh > 0
                        && (nw != w[i] || nh != h[i])) {
                        unsigned int *nb =
                            (unsigned int *)malloc((size_t)nw * (size_t)nh * 4u);
                        if (nb) {
                            free(buf[i]);
                            buf[i] = nb; w[i] = nw; h[i] = nh;
                            resizes++;
                            char line[128]; char *p = line;
                            p = put(p, "[COMPMULT] resize win="); p = putu(p, (unsigned)i);
                            *p++ = ' '; p = putu(p, (unsigned)nw); *p++ = 'x';
                            p = putu(p, (unsigned)nh);
                            p = put(p, " n="); p = putu(p, resizes);
                            *p = 0; emit(line);
                        } else {
                            emit("[COMPMULT] resize malloc FAILED, keeping old size");
                        }
                    }
                }
            }

            unsigned band = (unsigned)((frames + (unsigned long long)i * 37ULL) & 0xFFu);
            for (int row = 0; row < h[i]; row += 4) {
                unsigned v = 0xFF000000u
                           | (((band + (unsigned)row) & 0xFFu) << 16)
                           | ((unsigned)(0x30 + i * 0x18) << 8)
                           | band;
                unsigned int *rp = buf[i] + (size_t)row * (size_t)w[i];
                for (int col = 0; col < w[i]; col++) rp[col] = v;
            }
            blit_raw(win[i], (unsigned)w[i], (unsigned)h[i], (unsigned long)buf[i]);
            win_invalidate(win[i]);
        }
        frames++;

        if (now - t_report >= 5000000ULL) {
            unsigned long long dt = now - t_report;
            unsigned long long df = frames - frames_at_report;
            unsigned long long fps = dt ? (df * 1000000ULL / dt) : 0;
            unsigned long long bytes = 0;
            for (int i = 0; i < live; i++)
                bytes += (unsigned long long)w[i] * (unsigned long long)h[i] * 4ULL;
            char line[256]; char *p = line;
            p = put(p, "[COMPMULT] t="); p = putu(p, (now - t_start) / 1000000ULL);
            p = put(p, "s wins="); p = putu(p, (unsigned)live);
            p = put(p, " frames="); p = putu(p, frames);
            p = put(p, " push="); p = putu(p, fps); p = put(p, "/s");
            p = put(p, " bytes="); p = putu(p, bytes);
            p = put(p, " resizes="); p = putu(p, resizes);
            p = put(p, " sizes=");
            for (int i = 0; i < live; i++) {
                if (i) *p++ = ',';
                p = putu(p, (unsigned)w[i]); *p++ = 'x'; p = putu(p, (unsigned)h[i]);
            }
            *p = 0; emit(line);
            t_report = now; frames_at_report = frames;
        }
    }

    {
        unsigned long long total_us = mono_us() - t_start;
        unsigned long long avg = total_us ? (frames * 1000000ULL / total_us) : 0;
        char line[160]; char *p = line;
        p = put(p, "[COMPMULT] done frames="); p = putu(p, frames);
        p = put(p, " avg_push="); p = putu(p, avg); p = put(p, "/s");
        p = put(p, " resizes="); p = putu(p, resizes);
        *p = 0; emit(line);
    }
    for (int i = 0; i < live; i++) { free(buf[i]); win_destroy(win[i]); }
    return 0;
}
