// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// wfpath.h - WHERE A WORKFLOW LIVES. One answer, shared by the editor
// (/APPS/FLOW), the runner (/APPS/FLOWRUN) and AI chat's /flow generator.
//
// THE DEFECT THIS FIXES, MEASURED ON GOLDEN 2539 (commit da09fb41).
// Every one of those three hardcoded the literal "/CONFIG/WORKFLOWS". PERMS.DB
// on the shipped image carries the row `/CONFIG:0:0:0711`: root-owned, and
// --x for everyone else. The account the OS itself creates at first boot is
// uid 1000 (kernel/proc/users.c users_create_first_admin, "the human who is
// actually sitting there"), and the desktop session runs as that uid. So for
// the ONLY user a fresh install has:
//   * sys_mkdir("/CONFIG/WORKFLOWS") is refused, so the editor's first-run seed
//     wrote NOTHING and the example catalog was empty;
//   * Save was refused;
//   * Run, which saves first, was therefore refused too.
// Flow did not work at all for a normal user, and said so only as "New workflow
// (cannot open /CONFIG/WORKFLOWS/hello.yml)" in the status bar, because
// seed_if_missing() swallowed every failure. A feature that is silently dead for
// every non-root session is indistinguishable, from the outside, from a feature
// nobody wrote.
//
// THE FIX IS THE MECHANISM THE REST OF THE TREE ALREADY USES, not a new one.
// #683/#745 moved per-user state to <home>/..., with the property that ROOT'S
// HOME IS "/", so a root session resolves to exactly the legacy path and nothing
// about it changes. userhome_path("CONFIG", "WORKFLOWS", ...) gives
// /CONFIG/WORKFLOWS for root and /HOME/<USER>/CONFIG/WORKFLOWS for uid 1000.
// Workflows ARE per-user state: they are things the user wrote.
//
// READS fall back to the legacy system location, so an image where root put
// workflows in /CONFIG/WORKFLOWS keeps working and the Library lists both.
// WRITES never touch it. That is the same asymmetry userconf_open_read/write
// uses, for the same reason.
#ifndef MAYTERA_WFPATH_H
#define MAYTERA_WFPATH_H

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "fcntl.h"
#include "userconf.h"

// The legacy, system-wide location. Still READ, never written.
#define WF_SYSTEM_DIR "/CONFIG/WORKFLOWS"

static inline int wf_exists(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

// The directory THIS session writes workflows to. Creates it (and its parents)
// on the way. Returns 0 on success.
static inline int wf_dir(char *out, int cap) {
    if (userhome_path("CONFIG", "WORKFLOWS", out, (unsigned long)cap) != 0) {
        snprintf(out, (unsigned long)cap, "%s", WF_SYSTEM_DIR);
        return -1;
    }
    // Create <home>/CONFIG first: a mkdir of a/b fails when a is absent.
    char parent[192];
    if (userhome_path(0, "CONFIG", parent, sizeof(parent)) == 0)
        sys_mkdir(parent, 0755);
    sys_mkdir(out, 0755);
    return 0;
}

// Where the bundled, forkable examples are seeded for this session.
static inline int wf_examples_dir(char *out, int cap) {
    char d[192];
    if (wf_dir(d, sizeof(d)) != 0) {
        snprintf(out, (unsigned long)cap, "%s/examples", WF_SYSTEM_DIR);
        return -1;
    }
    snprintf(out, (unsigned long)cap, "%s/examples", d);
    sys_mkdir(out, 0755);
    return 0;
}

// The path a workflow called `name` is SAVED to. Always the writable dir.
static inline int wf_save_path(const char *name, char *out, int cap) {
    char d[192];
    int rc = wf_dir(d, sizeof(d));
    snprintf(out, (unsigned long)cap, "%s/%s.yml", d, name && name[0] ? name : "workflow");
    return rc;
}

// Resolve a BARE workflow name to an existing file: this session's own
// directory first, then its examples, then the legacy system directory and its
// examples. Returns 1 when something was found (out holds it), else 0 (out
// holds the writable path, which is the right thing to report as missing).
static inline int wf_resolve(const char *name, char *out, int cap) {
    char d[192], cand[256];
    if (wf_dir(d, sizeof(d)) == 0) {
        snprintf(cand, sizeof(cand), "%s/%s.yml", d, name);
        if (wf_exists(cand)) { snprintf(out, (unsigned long)cap, "%s", cand); return 1; }
        snprintf(cand, sizeof(cand), "%s/examples/%s.yml", d, name);
        if (wf_exists(cand)) { snprintf(out, (unsigned long)cap, "%s", cand); return 1; }
    }
    snprintf(cand, sizeof(cand), "%s/%s.yml", WF_SYSTEM_DIR, name);
    if (wf_exists(cand)) { snprintf(out, (unsigned long)cap, "%s", cand); return 1; }
    snprintf(cand, sizeof(cand), "%s/examples/%s.yml", WF_SYSTEM_DIR, name);
    if (wf_exists(cand)) { snprintf(out, (unsigned long)cap, "%s", cand); return 1; }
    wf_save_path(name, out, cap);
    return 0;
}

#endif // MAYTERA_WFPATH_H
