// idleprof.h - #COMPIDLE: why is the compositor awake, and what does it cost?
//
// THE PROBLEM THIS EXISTS TO SOLVE, STATED HONESTLY.
//
// The owner reports the compositor at 70% CPU AT IDLE on his own laptop. The
// same class of report was raised once before (44% with Settings open), a VM
// was measured at approximately 0%, the VM number was believed, and the ticket
// died. Measured again here on VM <vmid> / golden 2065: at a bare idle desktop
// the compositor asks for 0 or 1 presents per TWO SECONDS and the kernel's
// back->front copy costs 0% of a core. So the VM says, again, that nothing is
// wrong, and the VM is again not the machine with the problem.
//
// What separates the two machines is NOT knowable from here, so this file does
// not try to guess it. It records the one thing that transfers: WHICH BRANCH
// OF THE MAIN LOOP RAN, HOW OFTEN, AND WHY. The main loop has exactly one
// decision that matters for idle CPU - interactive/cursor/chrome/idle/nothing
// - and every input to that decision is a boolean already computed on the same
// tick. Counting them costs a handful of adds and answers, from a single boot
// on the affected machine, questions that no amount of VM time can:
//
//   rin=  ticks that ran the 125Hz interactive path because INPUT was recent.
//         A laptop touchpad that reports jitter with a hand resting near it
//         would pin this at 100% and would not exist in a VM at all. If rin
//         is high on his machine and zero on ours, the bug is input, not
//         painting, and no amount of framebuffer work will touch it.
//   dirty= ticks where an open app window said its content changed (#564).
//   wdg=/tb=/notif= ticks where a desktop widget, the taskbar gauges or a
//         toast produced damage. A gauge whose DISPLAYED value changes every
//         sample on a real machine (real CPU load, real network) and never
//         changes in an idle VM would show up here and nowhere else.
//   busy= the compositor's own Ring-3 CPU, as a percentage of wall clock,
//         measured with rdtsc around the body of the loop. This is the number
//         the owner is quoting, measured at the source instead of inferred
//         from a task list.
//
// COST. Two rdtsc per tick and a few adds; no syscall on the hot path. One
// ~2 KB file rewrite every 30 s (about 70 bytes/s of device traffic, versus
// the 43-57 KB/s that #748 had to remove). Always on, deliberately: an
// instrument behind a gate file is an instrument the person with the fault
// does not have when the fault happens.
//
// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
#ifndef COMPOSITOR_IDLEPROF_H
#define COMPOSITOR_IDLEPROF_H

// #rinpin: for wm_window_info_t (idleprof_dirty_pin_report() below). Self-
// contained (no further includes, no bool typedef of its own), so unlike most
// userland/libc headers this one is safe to pull into the compositor's
// otherwise draw.c-primitives-only world - see the compositor agent's own
// "Key facts" and blame.md's gui_scroll.h `bool` collision writeup (#563) for
// why every OTHER libc GUI header is avoided here.
#include "../../libc/syscall.h"

// WHY the tick was not idle. Set from the exact booleans main.c already has.
#define IP_R_INPUT      (1u << 0)   // recent_input (pointer/key within 500ms)
#define IP_R_APPSDIRTY  (1u << 1)   // an open window invalidated itself
#define IP_R_SCRSAVER   (1u << 2)
#define IP_R_DRAG       (1u << 3)   // any drag in flight
#define IP_R_MENU       (1u << 4)   // start/context/tray/launcher/widget/popup
#define IP_R_LOCKED     (1u << 5)
#define IP_R_DOCKANIM   (1u << 6)   // dock hover ease / drag ghost / toast
#define IP_R_REDRAW     (1u << 7)   // g_needs_redraw was set by something
#define IP_R_WIDGET     (1u << 8)   // widgets_collect_damage produced rects
#define IP_R_TASKBAR    (1u << 9)   // taskbar_collect_damage produced rects
#define IP_R_NOTIF      (1u << 10)  // notif_collect_damage produced rects
#define IP_R_QUIET      (1u << 11)  // #COMPIDLE: a tick inside the 500ms
                                    // post-input window in which NOTHING
                                    // actually happened, and which therefore
                                    // no longer pays a full-screen composite
#define IP_R_COUNT      12

// WHICH present path the tick took.
enum {
    IP_P_NOTHING = 0,   // no present at all: the good idle state
    IP_P_IDLE,          // render_frame_idle: desktop damage rects only
    IP_P_CHROME,        // render_frame_chrome: taskbar/toast rects only
    IP_P_CURSOR,        // render_frame_cursor: two small cursor rects
    IP_P_WINDOWED,      // render_frame_windowed: apps_dirty, geometry stable -
                        // union of visible window bounds + chrome damage only
                        // (no-ticket compositor partial-present fix)
    IP_P_FULL,          // render_frame: whole-screen composite + present
    IP_P_SCRSAVER,      // render_frame under the screensaver
    IP_P_COUNT
};

// THE ONE MEASUREMENT A VM STRUCTURALLY CANNOT MAKE.
//
// idleprof_flip() wraps the present syscall in rdtsc. The kernel's own
// [FLIPPROF] already times the back->front copy, but it goes to SERIAL, and
// serial is silent in GUI mode: on the owner's laptop it does not exist. This
// records the same cost from Ring 3, into a file he can read, and pairs it
// with the pixel count so the interesting quantity - CYCLES PER PIXEL
// PRESENTED - falls out. In a VM the front buffer is host RAM and that number
// is a memcpy rate. On real hardware it is the memory type of the display
// aperture, directly: an uncached mapping cannot hide in a per-pixel cost.
// Comparing his number with the VM's is the measurement #642's own header
// says can only be taken on the real machine.
// #compthread: per-phase breakdown of a render_frame_body() composite pass,
// in cycles, charged to whichever phase ran between two idleprof_phase_mark()
// calls. Exists to answer "where do the composite ms/tick actually go" from
// evidence instead of guessing before deciding which axis (tiling the pixel
// work across threads vs moving decode/scale off the draw thread) any
// parallelisation should target. Covers every call to render_frame_body(),
// not only the full-screen path, so the average is weighted by whatever mix
// of full/windowed/idle/cursor paths actually ran.
//
// IP_PH_WINWM is compositor_render_windows(), a SINGLE KERNEL SYSCALL
// (SYS_COMPOSITOR_RENDER_WINDOWS -> wm_draw_all/wm_draw_apps/wm_draw_winmenu
// in kernel/gui/window.c) that runs Ring 0 on the compositor's own thread.
// A userland pthread_create() in this process CANNOT parallelise time spent
// inside that one syscall - only the phases either side of it run in
// Ring-3 code this process controls.
enum {
    IP_PH_WALLPAPER = 0,   // wallpaper_render_background()
    IP_PH_DESKTOP,         // desktop icons/version + widgets + stickies
    IP_PH_SHADOWS,         // windows_render_shadows()
    IP_PH_WINWM,           // compositor_render_windows() - KERNEL syscall, see above
    IP_PH_CHROME,          // taskbar/startmenu/contextmenu/popups/notif/OSD/etc
    IP_PH_CURSOR,          // cursor_render() + apply_display_effects()
    IP_PH_COUNT
};
void idleprof_phase_begin(void);      // call once at the top of render_frame_body()
void idleprof_phase_mark(int phase);  // charge cycles since the last mark/begin

void idleprof_tick_begin(void);
int  idleprof_flip(void);                 // timed fb_flip()
void idleprof_motion(int dx, int dy);     // raw pointer deltas this tick
// #rinpin: WHICH non-motion signal set got_input this tick (see idleprof.c's
// declaration comment). Bits, not an enum, so a tick with both a button edge
// and a key event counts each once.
#define IP_IN_BUTTON (1u << 0)
#define IP_IN_KEY    (1u << 1)
void idleprof_input_kind(unsigned bits);
void idleprof_reason(unsigned bits);
void idleprof_path(int path);
void idleprof_damage_px(unsigned long px);
void idleprof_tick_end(void);

// One-shot diagnostic for the apps_dirty circuit breaker in main.c: dumps the
// current window list (id/visible/minimized/app_id/title) to /BOOTLOG.TXT,
// one line per window (each comfortably under sys_bootlog_write()'s 200-byte
// bounce - see the file-top comment on that limit). Called at most once per
// trip, not on a poll cadence, so it does not need its own rate limit.
void idleprof_dirty_pin_report(int n, const wm_window_info_t *wins);

#endif // COMPOSITOR_IDLEPROF_H
