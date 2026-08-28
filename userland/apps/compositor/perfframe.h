// perfframe.h - #62 revalidation: a real frame-interval instrument.
//
// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// #62 claims "one frame per several seconds" on the real iMac desktop, but the
// only trace of that number in the tree is the ticket restating itself - no
// independent measurement backs it, and the owner has since reported BOTH a
// smooth fullscreen game AND "I never noticed that". This instrument exists to
// settle it with a number instead of a memory of a memory.
//
// Every fb_flip() in main.c is a real present. This file buckets the interval
// SINCE THE LAST PRESENT ON THE SAME PATH (not a single global rate - #62's
// own premise is that paths differ) into a small ring per path tag, using
// SYS_MONO_US (TSC-backed - see kernel/proc/syscall.h), never SYS_UPTIME_MS/
// SYS_GET_TICKS: both are timer_ticks-derived, and blame.md's own
// timer-ticks-is-not-a-wall-clock entry records that KVM replays a starved
// vCPU's lost tick IRQs in BURSTS, which would either manufacture or hide the
// very stall this instrument exists to catch.
//
// GATED, per CLAUDE.md's flat-file hand-off convention, on a config file's
// existence on the EXT2 ROOT partition (fat_read_file() routes to ext2 when
// g_root_ext2 is set - see kernel/drivers/audio.c around line 261 - so a file
// dropped on the FAT ESP is silently never read; that mistake already cost one
// full test round per this ticket's brief). Absent the gate file, every call
// in this module is a single boolean check: no ring writes, no mono_us()
// syscall, so a normal shipping build pays nothing for carrying this in.
//
// The draw thread must never block (#426): every operation here is a bounded
// array write plus, on the periodic dump only, one small buffered file write -
// never a wait, never a retry loop.
#ifndef PERFFRAME_H
#define PERFFRAME_H

// Throttled gate check (existence of /CONFIG/PERF62.CFG on ext2 root) and
// periodic verdict dump. Call once per main-loop iteration from inside the
// compositor's existing ~330ms throttle group (matches dock_style_poll()'s
// cadence - see main.c) so this never adds its own syscall on every frame.
void perfframe_poll(void);

// Record one present on the named path. Tag strings are short C literals
// compared by prefix (idle/cursor/interactive/windowed/chrome/fullscreen/
// screensaver/lock) - see perfframe.c PF_TAGS. Unknown tags are silently
// dropped (defensive, not fatal: a typo'd tag must never crash the
// compositor). No-op when the gate is off.
void perfframe_mark(const char *tag);

// #62 (2026-08-20 real-iMac follow-up): call on every main-loop tick where an
// EXCLUSIVE display state - the session lock, or the screensaver's #652
// "blank after" steady state - deliberately presents NOTHING and every other
// path (chrome/windowed/interactive/cursor/fullscreen) is structurally
// unreachable this tick, by construction (main.c's render_frame() returns
// before the normal composite dispatch while either is active). That is
// correct, battery-saving behaviour: a machine left unattended for 20+
// minutes with the screensaver blanked SHOULD present nothing. Without this
// call, the NEXT real present on any other path measures its interval from
// that path's own last mark BEFORE the exclusive state began, so a benign
// 20-minute blank stretch is reported as a 20-minute stall on whatever path
// wakes up first - this is exactly the "silently conflates a slow redraw
// with a path that simply had nothing to draw" trap blame.md already
// recorded once (2026-08-19, a VM test-harness pause); this is the same
// trap tripped by a real, shipped, intentional zero-work state instead of an
// unplanned pause, and it is what actually produced #62's own 1,476-second
// "chrome" outlier - confirmed by the timestamps matching the screensaver's
// active window exactly. perfframe_mark() floors its interval baseline at
// the latest quiescent mark, so a path's next present is timed from when
// the display became presentable again, never from further back than that.
// No-op when the gate is off (one boolean check, same as perfframe_mark()).
void perfframe_note_quiescent(void);

#endif
