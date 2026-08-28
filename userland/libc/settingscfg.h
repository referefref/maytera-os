// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// settingscfg.h - #230: THE cross-process reader for the handful of
// SETTINGS.CFG keys that a process OTHER than Settings itself needs to
// consume live.
//
// WHY THIS EXISTS
// ---------------
// docs/SETTINGS_CONTROL_AUDIT.md (#224) found /CONFIG/SETTINGS.CFG was
// WRITE-ONLY across the entire OS: userland/apps/settings/main.c wrote it,
// and its own settings_load() was the only reader that ever existed, so most
// of its keys were a pure Settings-internal round trip that survived
// relaunch and did nothing else - including the clock format and the
// mouse double-click threshold.
//
// Same idiom as tz.c/tz.h (#49/#50) and ALERTS.CFG/STARTMENU.PREFS (read on a
// throttle by the compositor's main loop): ONE throttled read with an
// in-between cache, so a caller may poll every frame (a taskbar clock redraw,
// a mouse click classifier) with no syscall most of the time and no risk of
// ever blocking on one.
//
// Do NOT add a second parser of SETTINGS.CFG anywhere else in the tree: add a
// getter HERE and have it share settingscfg_reload()'s one file read. Every
// getter here must have a real, cited consumer - tools/pref-reader-lint
// (#230) fails the build otherwise; see tools/pref-reader-lint/READERS.tsv.
#ifndef LIBC_SETTINGSCFG_H
#define LIBC_SETTINGSCFG_H

// 'h' key. 1 = 24-hour clock, 0 = 12-hour (AM/PM). Defaults to 1 (the
// historical hardcoded behavior) until SETTINGS.CFG has ever been read.
// Consumers: compositor/taskbar.c tb_clock_str() (all four dock styles),
// userland/apps/settings/main.c's own Date & Time clock preview.
int settingscfg_use24h(void);

// 'k' key (double_click_speed, raw 0-100 UI slider value), RANGE-MAPPED to a
// real millisecond threshold: 900 - (raw * 7), clamped to [150, 900]. Higher
// slider value ("faster") -> shorter window, matching the "Double-click
// Speed" label. Consumers (#236): compositor/desktop.c's desktop_release()
// (the live detector for a desktop-icon double-click) and
// compositor/main.c's dbl_click_threshold(), the latter also pushing this
// value to the kernel's title-bar maximize/restore detector via
// SYS_SET_DBLCLICK_MS (gui/window.c) whenever it changes. Compare the result
// against uptime_ms(), not sys_clock(): sys_clock() is raw PIT ticks (4ms
// each at the kernel's 250Hz), and comparing a millisecond threshold against
// a tick delta silently multiplies the effective wait by the tick period -
// exactly the bug that made this setting appear to do nothing on golden 2016.
int settingscfg_dblclick_ms(void);

// Drop the cache so the very next call re-reads the file. For a process that
// just wrote SETTINGS.CFG itself (Settings) and wants its own next read (if
// any) to see the fresh value rather than wait out the throttle. Cross-
// process callers (the compositor) do not need this - they pick up a change
// within SETTINGSCFG_REFRESH_MS regardless.
void settingscfg_invalidate(void);

#endif // LIBC_SETTINGSCFG_H
