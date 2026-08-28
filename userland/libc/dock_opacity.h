// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// dock_opacity.h - the ONE definition of the dock/chrome opacity bounds (#132)
//
// WHY THIS FILE EXISTS. Before #132 the value 70 (the old hard floor) and 75
// (the default) were spelled as bare literals, independently, in SIX places
// across two separate binaries (Settings and the compositor cannot share a
// header the way two files in one binary can - Settings' own gui_dock.h
// comment documents the same constraint for the dock STYLE names):
//   - userland/apps/settings/main.c        DOCK_OPACITY_FLOOR (its own #define)
//   - userland/apps/compositor/main.c      dock_opacity_write_cfg()'s clamp
//   - userland/apps/compositor/main.c      dock_opacity_poll()'s clamp
//   - userland/apps/compositor/profile.c   the UIPROFIL.YML load clamp
//   - userland/apps/compositor/draw.c      glass_render()'s contrast floor
//   - userland/apps/compositor/taskbar.c   flat_chrome_alpha()'s FLAT_OPACITY_FLOOR
//   - userland/apps/compositor/testhook.c  the GLASSPROBE verb's own clamp
// kept in sync BY COMMENT ONLY ("must match draw.c glass_render()'s floor").
// settings/main.c's own #745 comment already names the failure mode: a fourth
// copy was missed by a `grep "< 60"` sweep because it spelled the check as a
// bare `>= 60`. The owner reported (#123/#132) that none of his dock requests,
// including a below-70 opacity, appeared to land; a re-clamp anywhere on this
// list would have silently reproduced exactly that complaint. Fix the CLASS:
// one shared header, included by both binaries, so there is nothing left to
// keep "in sync" because there is only one copy.
//
// OWNER DECISION (#132, standing): replace the hard clamp with a WARNING. A
// value below the old floor is now honoured by every renderer, not silently
// raised back to 70.
//
// DOCK_OPACITY_MIN is the new floor enforced at every clamp site above. NOT 0:
//   - draw.c's glass_render() and taskbar.c's flat_chrome_alpha() both compute
//     alpha = op*255/100 and hand it to draw_fill_rect()/glass_render() as a
//     BLEND factor. At op=0 that fill is a literal no-op - zero bytes of the
//     framebuffer touched - on every glass AND every flat theme.
//   - The dock's ICONS stay fully visible regardless of this value (they are
//     drawn with g_draw_blend restored to opaque, outside the scope the
//     opacity slider controls - see taskbar.c glass_or_flat()/xfce_draw_slot()
//     call sites), so the dock never becomes unrecoverable via the GUI even at
//     the floor: there is always something to click.
//   - A literal 0 would still throw away the one thing a floor buys for free:
//     a non-zero surface cue that says "this pixel is the dock, not bare
//     wallpaper". 15 keeps a faint but real background (alpha ~38/255) at the
//     floor while landing far below the old 70, which is what was asked for.
//
// DOCK_OPACITY_WARN is NOT the old floor moved sideways: the compositor
// honours anything down to DOCK_OPACITY_MIN unconditionally, this is only
// where Settings starts SHOWING a warning. It is the STRICTER of the two
// independently measured WCAG AA 4.5:1 floors already in this tree - draw.c
// glass_render() measured 70 as the glass-tint floor (Ocean, white backdrop,
// 4.62:1), taskbar.c's flat_chrome_alpha() measured 73 as the flat-token floor
// (retro_unix over black, 4.59:1) - so the warning fires before EITHER path's
// measured contrast risk begins, across all 14 shipped themes. Below it,
// contrast is the user's explicit trade-off for more transparency, which is
// exactly why this is a warning and not a second hard clamp.
//
// A value here is never itself the fix for a specific pixel bug: if a theme's
// contrast measurement changes, update ONLY the derivation comments in
// draw.c/taskbar.c (the source of the 70/73 figures) and re-run the sweep
// referenced there before touching DOCK_OPACITY_WARN.
#ifndef _DOCK_OPACITY_H
#define _DOCK_OPACITY_H

#define DOCK_OPACITY_MIN     15    // #132: the honoured floor (was a hard 70)
#define DOCK_OPACITY_MAX     100
#define DOCK_OPACITY_DEFAULT 75    // unchanged by #132; already above every warn line
#define DOCK_OPACITY_WARN    73    // below this, Settings warns; nothing is blocked

#endif // _DOCK_OPACITY_H
