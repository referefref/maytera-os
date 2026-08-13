// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// startmenu_reg.h - install-time Start-menu registration, shared by the App
// Store (userland/apps/appstore/main.c) and the auto-updater
// (userland/apps/updated/main.c). See startmenu_reg.c for why this exists.
#ifndef STARTMENU_REG_H
#define STARTMENU_REG_H

// Registers (or re-registers) one installed app into the Start menu's
// all-users system config layer. Idempotent: calling it again for the same
// pkg_id (e.g. after an update changes the launch path) overwrites that
// package's one fragment file, not the whole directory. Returns 0 on success,
// -1 on failure (including a NULL/empty argument).
//
// #745 (#77): `repo_category` is the package's `category` field STRAIGHT OUT OF
// THE REPOSITORY MANIFEST, and it is treated as UNTRUSTED. It is never copied
// into the fragment; it only SELECTS one of the compile-time group labels in
// startmenu_reg.c's SM_GROUPS table. Anything unrecognised, empty or NULL lands
// in "Installed", which is what every store install used to get unconditionally
// and is why that group still exists. See sm_group_for() for the reasoning.
int startmenu_register_app(const char *pkg_id, const char *name, const char *exec_path,
                           const char *repo_category);

// Removes a package's Start-menu fragment (e.g. on uninstall). Returns 0 on
// success (including "was already absent"), -1 on failure.
int startmenu_unregister_app(const char *pkg_id);

// #745: the same registration, but into the SESSION USER'S OWN menu layer at
// <home>/CONFIG/STARTMENU/, read by sm_feed_user_layer() in
// userland/apps/compositor/startmenu.c.
//
// WHY A THIRD LAYER AND NOT THE EXISTING /CONFIG/STARTMENU/USER/<name>/ ONE.
// That directory is real and is already read, but it lives under /CONFIG, which
// is root:root 0711 and holds SHADOW. Making a subdirectory of it writable by a
// user to support an unprivileged install would put a user-writable directory
// inside the tree that holds the password database, which is a strictly worse
// trade for an identical outcome. The user's own home needs no permission grant
// at all: they already own it. Same principle as userconf.c (#683), which
// relocated the desktop's preference writes instead of widening /CONFIG.
//
// Root's home is "/", so for a ROOT session this resolves to
// /CONFIG/STARTMENU/ (the parent of SYSTEM.D, which contains no fragments of
// its own). Root therefore keeps using startmenu_register_app() and this
// function is not called on that path.
int startmenu_register_app_user(const char *pkg_id, const char *name, const char *exec_path,
                                const char *repo_category);

// Per-user counterpart of startmenu_unregister_app().
int startmenu_unregister_app_user(const char *pkg_id);

#endif
