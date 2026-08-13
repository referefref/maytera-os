// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_dock.h - the ONE list of dock-style display names (#745)
//
// WHY THIS FILE EXISTS. The names were declared twice: DOCK_OPTS[] in
// userland/apps/settings/main.c and DOCK_NAMES[] in userland/apps/setup/main.rs
// (the first-boot wizard). They drifted, exactly as two lists do: Settings had
// been renamed off the third-party desktop names ("Default (MayteraOS)",
// "Lumina", "Classic UNIX", "Retro Bench", "Marble") while the wizard still
// shipped "macOS style", "CDE panel" and "Amiga bar" on screen. The user
// reported it twice.
//
// A comment saying "keep these in step" is what already failed, so the fix is
// not a better comment: there is now ONE array, here, and both apps read it.
// The wizard is Rust and Settings is C, but the wizard LINKS libc.a (see
// userland/apps/setup/Makefile), so a C function is reachable from both and no
// code generation step is needed. Divergence is not detected, it is
// unrepresentable: neither app owns a list to diverge with.
//
// build/dock-name-gate.sh is the backstop for the ONE thing this cannot
// prevent, someone declaring a new private list, and for the count staying
// equal to DOCK_COUNT in userland/apps/compositor/compositor.h (which cannot be
// included from Settings: its unguarded `typedef int bool` collides with
// libc/types.h).
//
// LABELS ONLY. The dock style is persisted by NUMERIC INDEX (UIPROFIL.YML
// dock_style, /DOCKSTYL.CFG) and the compositor's enum names are unchanged
// (DOCK_XFCE is still 4), so renaming a label here never resets a profile.
#ifndef _GUI_DOCK_H
#define _GUI_DOCK_H

// Must equal DOCK_COUNT in userland/apps/compositor/compositor.h.
// build/dock-name-gate.sh fails the build if the two ever differ.
#define GUI_DOCK_COUNT 5

// The array itself, for a caller that needs a `const char *const *` (the
// Settings dropdown widget takes one).
const char *const *gui_dock_style_names(void);
// One name, NUL-terminated, for a caller that indexes (the Rust wizard).
// Out-of-range returns "" and never NULL, so no caller can fault on it.
const char       *gui_dock_style_name(int idx);
int               gui_dock_style_count(void);

#endif // _GUI_DOCK_H
