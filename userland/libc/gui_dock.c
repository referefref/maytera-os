// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_dock.c - see gui_dock.h. The ONE dock-style name list (#745).
#include "gui_dock.h"

// Order matches the compositor's DOCK_* enum (compositor.h):
//   0 DOCK_DEFAULT      classic bottom taskbar
//   1 DOCK_LUMINA       glass dock + translucent top menu bar
//   2 DOCK_CLASSIC_UNIX beveled front panel with a workspace switcher
//   3 DOCK_RETRO_BENCH  title-bar-at-top screen bar with depth/zoom gadgets
//   4 DOCK_XFCE         glass flush top panel + glass flush bottom dock
//                       ("opaque" here was STALE - #745 dockgrey, 2026-08-12:
//                       fixed, see compositor.h's own DOCK_XFCE comment,
//                       which already carried the correct "glass" wording;
//                       this one had not been updated to match)
// The enum identifiers and the persisted digits are historical and stay put;
// only these strings are shown to a user, and none of them names someone
// else's desktop.
static const char *const DOCK_STYLE_NAMES[GUI_DOCK_COUNT] = {
    "Default (MayteraOS)",
    "Lumina",
    "Classic UNIX",
    "Retro Bench",
    "Marble",
};

const char *const *gui_dock_style_names(void) { return DOCK_STYLE_NAMES; }
int gui_dock_style_count(void) { return GUI_DOCK_COUNT; }

const char *gui_dock_style_name(int idx) {
    if (idx < 0 || idx >= GUI_DOCK_COUNT) return "";
    return DOCK_STYLE_NAMES[idx];
}
