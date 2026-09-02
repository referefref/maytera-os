// COMPCEIL - #compceiling: an UNPACED full-window content pump.
//
// WHY THIS EXISTS. Every existing redraw-heavy app in this tree paces
// itself: glcube blocks on win_get_event(win,&ev,16) (a self-imposed ~62.5Hz
// cap independent of the compositor), and BLITBNCH runs a fixed 120-frame
// sample. Neither can answer "what is the compositor's OWN maximum sustained
// present rate", because in both cases the APP is the thing setting the
// pace, not the compositor. This app applies no sleep, no event wait, no
// frame cap of its own: it blits and invalidates back to back, as fast as
// the syscalls return, so whatever rate the compositor actually PRESENTS at
// is the compositor's ceiling and nothing else's.
//
// The window content is perturbed every frame (a shifting diagonal band, not
// a static fill) for two reasons: so the blit is not a no-op the compositor
// could special-case away, and so a screendump taken mid-run shows visible
// motion (the "verify liveness" rule - two screendumps a fixed image apart
// should differ).
//
// TWO MODES, selected by argv[1]:
//   (no arg) or "fixed"  - window geometry never changes. The compositor's
//                          #compositor-partial windowed path (composite only
//                          the visible window bounds + chrome) is what
//                          services every frame, which is the common real
//                          case (a video/game window sitting still while its
//                          content updates).
//   "move"               - the window is also nudged by SYS_WIN_MOVE every
//                          frame. A geometry change defeats the windowed
//                          fast path (see main.c's windowed_dirty_only
//                          logic), forcing render_frame()'s full-screen
//                          composite+present every tick - the worst case,
//                          comparable to continuously dragging a window.
//
// argv[2] is the run length in seconds (default 600; 0 = run forever, until
// killed).
//
// #wakelag CHANGED THE DEFAULT FROM 60 TO 600, and the reason is a real error
// this harness caused once. /CONFIG/AUTORUN.CFG passes NO ARGUMENTS - the
// parser in kernel/gui/desktop.c reads one path and calls
// launch_userspace_app(path) - so an autorun-launched COMPCEIL can only ever
// take the DEFAULT. #compceiling's first read of its own data used the last 3
// of 6 [COMPIDLE] windows as "steady state" and got a materially wrong headline
// number, because COMPCEIL had exited at ~60s inside a 150s capture and two
// thirds of the samples were an idle desktop with no demand app at all. It was
// caught by a screendump, not by the numbers.
//
// A default shorter than a normal capture window is a trap that fires silently
// and looks like data. 600s is longer than any capture this rig has used, so
// the app now outlives the measurement by construction rather than by the next
// person remembering. It still TERMINATES (unlike 0), so [COMPCEIL] done stays
// a real liveness signal, and argv[2] still overrides for a manual run.
//
// OUTPUT: one bootlog line every 5s, "[COMPCEIL] t=Xs frames=N push=Y/s",
// which is the APP-SIDE push rate - how many times this process asked for a
// present. It is deliberately expected to exceed the compositor-side present
// rate reported by [COMPIDLE] ticks=.../s and [FLIPPROF] flips=...: the gap
// between the two numbers is direct, measured evidence of a queue the
// compositor could not drain at the offered rate, which is the ceiling this
// harness exists to find. See build/unshipped-apps.list for why this never
// ships: it is a measurement harness, not a feature, in the same class as
// BLITBNCH beside it.
//
// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

#define SYS_WIN_BLIT_NUM 35

static long blit_raw(int h, unsigned int sw, unsigned int sh, unsigned long p) {
    unsigned long packed = (sw & 0xFFFFu) | ((sh & 0xFFFFu) << 16);
    return syscall5(SYS_WIN_BLIT_NUM, (long)h, 0, 0, (long)packed, (long)p);
}

static void emit(const char *s) { printf("%s\n", s); sys_bootlog(s); }

// No trustworthy snprintf in this freestanding subset (BLITBNCH's own
// comment, still true here): assemble lines by hand.
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *putu(char *p, unsigned long long v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

int main(int argc, char *argv[]) {
    int move_mode = (argc > 1 && argv[1][0] == 'm');
    int run_secs  = (argc > 2) ? atoi(argv[2]) : 600;   // #wakelag: was 60; see the header

    {
        char line[160]; char *p = line;
        p = put(p, "[COMPCEIL] start mode=");
        p = put(p, move_mode ? "move" : "fixed");
        p = put(p, " run_secs="); p = putu(p, (unsigned long long)(run_secs < 0 ? 0 : run_secs));
        *p = 0; emit(line);
    }

    // Request a large window; win_get_size() reports what was actually
    // granted (clamped to the screen), so this adapts to 1280x800 and to a
    // 2560x1600 present_scale=2 rig without a rebuild.
    int win = win_create("compceil", 0, 0, 3200, 2200);
    if (win < 0) { emit("[COMPCEIL] ABORT win_create failed"); return 1; }
    int dw = 0, dh = 0;
    if (win_get_size(win, &dw, &dh) != 0 || dw < 1 || dh < 1) {
        emit("[COMPCEIL] ABORT win_get_size failed"); win_destroy(win); return 1;
    }
    {
        char line[96]; char *p = line;
        p = put(p, "[COMPCEIL] window "); p = putu(p, (unsigned)dw);
        *p++ = 'x'; p = putu(p, (unsigned)dh);
        *p = 0; emit(line);
    }

    unsigned int *buf = (unsigned int *)malloc((size_t)dw * (size_t)dh * 4u);
    if (!buf) { emit("[COMPCEIL] ABORT malloc failed"); win_destroy(win); return 1; }

    unsigned long long t_start  = mono_us();
    unsigned long long t_report = t_start;
    unsigned long long frames = 0, frames_at_report = 0;
    int x = 0, y = 0, dx = 2, dy = 2;

    for (;;) {
        unsigned long long now = mono_us();
        if (run_secs > 0 && (now - t_start) >= (unsigned long long)run_secs * 1000000ULL)
            break;

        // Cheap per-frame content perturbation: a diagonal band whose color
        // and offset both depend on the frame count. Touches every row (not
        // every pixel) to keep this loop from being blit-cost-dominated by
        // the CONTENT FILL rather than by the win_blit/win_invalidate pair
        // actually under test.
        unsigned band = (unsigned)(frames & 0xFFu);
        for (int row = 0; row < dh; row += 4) {
            unsigned v = 0xFF000000u
                       | (((band + (unsigned)row) & 0xFFu) << 16)
                       | (0x40u << 8)
                       | band;
            unsigned int *rp = buf + (size_t)row * (size_t)dw;
            for (int col = 0; col < dw; col++) rp[col] = v;
        }

        blit_raw(win, (unsigned)dw, (unsigned)dh, (unsigned long)buf);
        win_invalidate(win);

        if (move_mode) {
            x += dx; y += dy;
            if (x <= 0 || x >= 60) dx = -dx;
            if (y <= 0 || y >= 60) dy = -dy;
            win_move(win, 40 + x, 40 + y);
        }

        frames++;

        if (now - t_report >= 5000000ULL) {
            unsigned long long dt = now - t_report;
            unsigned long long df = frames - frames_at_report;
            unsigned long long fps = dt ? (df * 1000000ULL / dt) : 0;
            char line[128]; char *p = line;
            p = put(p, "[COMPCEIL] t=");
            p = putu(p, (now - t_start) / 1000000ULL);
            p = put(p, "s frames="); p = putu(p, frames);
            p = put(p, " push="); p = putu(p, fps); p = put(p, "/s");
            *p = 0; emit(line);
            t_report = now; frames_at_report = frames;
        }
    }

    {
        unsigned long long total_us = mono_us() - t_start;
        unsigned long long avg = total_us ? (frames * 1000000ULL / total_us) : 0;
        char line[128]; char *p = line;
        p = put(p, "[COMPCEIL] done frames="); p = putu(p, frames);
        p = put(p, " avg_push="); p = putu(p, avg); p = put(p, "/s");
        *p = 0; emit(line);
    }

    free(buf);
    win_destroy(win);
    return 0;
}
