// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// startmenu_reg.c - install-time Start-menu registration for the App Store
// and the auto-updater.
//
// BEFORE THIS FILE: userland/apps/appstore/main.c's regini_register() and
// userland/apps/updated/main.c's regini_register() were two byte-for-byte
// duplicate copies of the same drop-and-rebuild logic, writing
// /APPS/REGINI.CFG - a file the userland compositor's Start menu has never
// read. The "live refresh" they both called after writing it,
// sys_desktop_menu_reload() (SYS_DESKTOP_MENU_RELOAD, syscall 300), resolves
// to desktop_menu_reload() in kernel/gui/desktop.c, which is a documented
// no-op ("the kernel start menu is gone... now a no-op"). Net effect,
// measured before this change: installing an app through the App Store did
// not add it to the live Start menu, and never had, on any build that ships
// the userland compositor.
//
// THIS FILE replaces both copies with one shared implementation that writes
// into the Start menu's real, live-read all-users config directory
// (/CONFIG/STARTMENU/SYSTEM.D/, read by startmenu_model.rs via
// userland/apps/compositor/startmenu.c's sm_feed_system_layer(), polled on a
// throttle by startmenu_rust_poll() - see that file for the two-layer merge
// this feeds into). One fragment file per package, named by package id, so
// concurrent installs/updates of DIFFERENT packages never race on the same
// file the way the old whole-file rewrite did, and an update simply
// overwrites its own package's one file.
#include "startmenu_reg.h"
#include "syscall.h"
#include "fcntl.h"
#include "string.h"
#include "userconf.h"     // #745: the ONE home join, for the per-user layer

// <dir>/APPSTORE_<sanitized pkg_id>.MENU, where <dir> is the caller's chosen
// layer directory. ONE name builder for both layers: the sanitizer below is the
// only thing standing between a package id and a path, and a second copy of it
// is a second place for it to be got wrong.
static void sm_reg_frag_path_in(const char *dir, const char *pkg_id, char *out, int outsz)
{
    int i = 0;
    while (dir[i] && i < outsz - 16) { out[i] = dir[i]; i++; }
    if (i > 0 && out[i - 1] != '/' && i < outsz - 16) out[i++] = '/';
    const char *prefix = "APPSTORE_";
    int pi = 0;
    while (prefix[pi] && i < outsz - 6) out[i++] = prefix[pi++];
    // Sanitize: package ids are expected to be short alnum/-/_ tokens
    // already (pkg_t.id in appstore/main.c is a 32-byte field parsed from the
    // signed manifest), but this file names have no business trusting a
    // string it did not itself constrain - anything outside [A-Za-z0-9_-] is
    // folded to '_' rather than reaching a path.
    int j = 0;
    while (pkg_id[j] && i < outsz - 6) {
        char c = pkg_id[j++];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_';
        out[i++] = ok ? c : '_';
    }
    const char *suffix = ".MENU";
    int k = 0;
    while (suffix[k] && i < outsz - 1) out[i++] = suffix[k++];
    out[i] = '\0';
}

// The all-users layer, unchanged: /CONFIG/STARTMENU/SYSTEM.D/APPSTORE_<id>.MENU
static void sm_reg_frag_path(const char *pkg_id, char *out, int outsz)
{
    sm_reg_frag_path_in("/CONFIG/STARTMENU/SYSTEM.D", pkg_id, out, outsz);
}

// #745: the per-user layer, <home>/CONFIG/STARTMENU/. Returns 0 on success, -1
// if the home path will not fit, in which case NOTHING is written: a truncated
// path is a different file.
static int sm_reg_user_dir(char *out, int outsz)
{
    return userhome_path("CONFIG", "STARTMENU", out, (unsigned long)outsz);
}

static int sm_reg_ensure_dirs(void)
{
    // sys_mkdir() on an already-existing directory is expected to fail
    // harmlessly; every golden ships these three levels already (see
    // build/build-golden.sh's startmenu system-layer overlay), this is only
    // a safety net for a live filesystem that somehow lacks them.
    sys_mkdir("/CONFIG", 0755);
    sys_mkdir("/CONFIG/STARTMENU", 0755);
    return sys_mkdir("/CONFIG/STARTMENU/SYSTEM.D", 0755);
}

// ---------------------------------------------------------------------------
// #745 (#77) THE CATEGORY, AND WHY IT IS A TABLE AND NOT A STRING COPY.
//
// Reported defect: "apps installed from the app store are going into 'Installed'
// instead of their relevant category, i.e. OpenArena into Games". The package's
// category WAS parsed (pkg_t.category, and the store's own sidebar filters on
// it), it simply never reached this file, so every install wrote the same
// hardcoded "category: Installed" line.
//
// THE CATEGORY IS UNTRUSTED INPUT. It arrives in a repository manifest fetched
// over the network. The manifest is signature-verified before it is parsed, so
// an attacker must first hold the signing key, but a VALID signature says the
// bytes came from the repository, not that the repository is careful. Two things
// go wrong if this string is copied into the fragment:
//
//   1. FRAGMENT INJECTION. The grammar is line-oriented and pipe-delimited
//      (startmenu_model.rs: "category: <Label> [| expanded] [| id=<catid>]"),
//      so a category of "X\nhide: apps-terminal" would hide a shipped item, and
//      "X | id=games" would silently retarget an existing group. A 16-byte
//      field is plenty for either.
//   2. ARBITRARY GROUP CREATION. A category the model has never seen simply
//      CREATES a group, because a category's identity is a slug of its label.
//      A repository could name a new top-level Start-menu group per package.
//
// Both are removed by construction here rather than by escaping: the untrusted
// string is only ever COMPARED, and the label written to disk is always one of
// the compile-time literals below. There is no code path from `repo_category`
// bytes to fragment bytes.
//
// THE `expanded` COLUMN IS NOT DECORATION. A `category:` line for an id that
// already exists REPLACES that record's label AND expanded flag wholesale
// (startmenu_model.rs process_category_line), and the App Store's fragments
// sort AFTER the shipped 0N-*.MENU ones, so they are processed last and win.
// build/assets/startmenu/system.d/02-accessories.MENU ships
// "category: Accessories | expanded"; emitting a bare "category: Accessories"
// here would COLLAPSE a group the user never touched, as a side effect of
// installing an office app. The column keeps that faithful, and
// tools/appstore-category-gate/category-map-gate.sh fails the moment this table
// and those fragments disagree, so the duplication cannot rot silently.
//
// UNKNOWN, EMPTY AND NULL ALL LAND IN "Installed". That is the behaviour every
// store install had before this change, so an unrecognised category is a
// no-worse-than-before outcome rather than a lost app; "Installed" is also a
// real group that the model will render. The auto-updater passes NULL (it never
// parses a category) and therefore keeps writing exactly what it wrote before.
// ---------------------------------------------------------------------------
typedef struct {
    const char *id;        // the repository's category id (untrusted, compared only)
    const char *label;     // the Start-menu group label (ours, compile-time)
    int         expanded;  // must match the shipped fragment for this label
} sm_group_t;

// The six ids the repository actually publishes are games/office/coding/media/
// system/themes (measured against the live manifest, not inferred from a doc).
// "themes" is absent on purpose: a palette is not launchable and the App Store
// never registers one (see is_app in appstore/main.c install_pkg()).
static const sm_group_t SM_GROUPS[] = {
    { "games",  "Games",       0 },
    { "media",  "Media",       0 },
    { "system", "System",      0 },
    { "office", "Accessories", 1 },   // 02-accessories.MENU ships "| expanded"
    { "coding", "Development", 0 },   // no shipped group; the repo's own label
};
#define SM_NGROUPS ((int)(sizeof(SM_GROUPS) / sizeof(SM_GROUPS[0])))

static const char *const SM_GROUP_FALLBACK = "Installed";

// Returns the group for a repository category id, or NULL for "use the
// fallback". Exact match only: no prefix match, no case folding, no
// normalisation. A near-miss must land in "Installed" rather than in a group
// it merely resembles.
static const sm_group_t *sm_group_for(const char *repo_category)
{
    if (!repo_category || !repo_category[0]) return 0;
    for (int i = 0; i < SM_NGROUPS; i++)
        if (strcmp(repo_category, SM_GROUPS[i].id) == 0) return &SM_GROUPS[i];
    return 0;
}

// The fragment TEXT, identical for both layers. Factored out with the path
// builder for the same reason: the two layers differ in WHERE the file goes and
// in nothing else, and a duplicated body is how they drift apart.
static int sm_reg_write(const char *path, const char *pkg_id, const char *name,
                        const char *exec_path, const char *repo_category)
{
    // Two lines: one "category:" then one "item:". Every fragment reopens its
    // own category (startmenu_model.rs resets "current category" at each
    // fragment's start), so this file is self-contained and does not depend on
    // load order relative to any other fragment.
    static char body[512];
    int o = 0;
    const char *hdr =
        "# Auto-generated by the App Store installer. Do not hand-edit; "
        "uninstalling\n# the package removes this file (startmenu_reg.c).\n"
        "category: ";
    for (const char *p = hdr; *p && o < (int)sizeof(body) - 1; p++) body[o++] = *p;
    {
        const sm_group_t *g = sm_group_for(repo_category);
        const char *lab = g ? g->label : SM_GROUP_FALLBACK;
        for (const char *p = lab; *p && o < (int)sizeof(body) - 1; p++) body[o++] = *p;
        if (g && g->expanded) {
            const char *ex = " | expanded";
            for (const char *p = ex; *p && o < (int)sizeof(body) - 1; p++) body[o++] = *p;
        }
    }
    {
        const char *nl = "\nitem: ";
        for (const char *p = nl; *p && o < (int)sizeof(body) - 1; p++) body[o++] = *p;
    }
    for (const char *p = name; *p && o < (int)sizeof(body) - 1; p++) body[o++] = *p;
    const char *mid = " | ";
    for (const char *p = mid; *p && o < (int)sizeof(body) - 1; p++) body[o++] = *p;
    for (const char *p = exec_path; *p && o < (int)sizeof(body) - 1; p++) body[o++] = *p;
    const char *idpre = " | id=appstore-";
    for (const char *p = idpre; *p && o < (int)sizeof(body) - 1; p++) body[o++] = *p;
    for (const char *p = pkg_id; *p && o < (int)sizeof(body) - 1; p++) body[o++] = *p;
    if (o < (int)sizeof(body) - 1) body[o++] = '\n';

    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    long wr = sys_write(fd, body, (unsigned long)o);
    sys_close(fd);
    return (wr == o) ? 0 : -1;
}

int startmenu_register_app(const char *pkg_id, const char *name, const char *exec_path,
                           const char *repo_category)
{
    if (!pkg_id || !pkg_id[0] || !name || !name[0] || !exec_path || !exec_path[0])
        return -1;
    sm_reg_ensure_dirs();
    char path[128];
    sm_reg_frag_path(pkg_id, path, sizeof(path));
    return sm_reg_write(path, pkg_id, name, exec_path, repo_category);
}

int startmenu_register_app_user(const char *pkg_id, const char *name, const char *exec_path,
                                const char *repo_category)
{
    if (!pkg_id || !pkg_id[0] || !name || !name[0] || !exec_path || !exec_path[0])
        return -1;

    char dir[192];
    if (sm_reg_user_dir(dir, (int)sizeof(dir)) != 0) return -1;

    // <home>/CONFIG and <home>/CONFIG/STARTMENU. users_make_home_skeleton()
    // creates the first of these since #745 and the user OWNS its home since
    // #679, so both succeed for a non-root session; every home that already
    // exists on an installed system predates the change, hence the mkdirs. The
    // results are deliberately not checked: "already exists" and "created" are
    // equally fine and the write below is the real test.
    {
        char up[192];
        int cut = 0;
        for (int i = 0; dir[i]; i++) if (dir[i] == '/') cut = i;
        if (cut > 0 && cut < (int)sizeof(up)) {
            for (int k = 0; k < cut; k++) up[k] = dir[k];
            up[cut] = '\0';
            sys_mkdir(up, 0755);
        }
    }
    sys_mkdir(dir, 0755);

    char path[256];
    sm_reg_frag_path_in(dir, pkg_id, path, sizeof(path));
    return sm_reg_write(path, pkg_id, name, exec_path, repo_category);
}

int startmenu_unregister_app_user(const char *pkg_id)
{
    if (!pkg_id || !pkg_id[0]) return -1;
    char dir[192];
    if (sm_reg_user_dir(dir, (int)sizeof(dir)) != 0) return -1;
    char path[256];
    sm_reg_frag_path_in(dir, pkg_id, path, sizeof(path));
    sys_unlink(path);   // see startmenu_unregister_app() for why this is best-effort
    return 0;
}

int startmenu_unregister_app(const char *pkg_id)
{
    if (!pkg_id || !pkg_id[0]) return -1;
    char path[128];
    sm_reg_frag_path(pkg_id, path, sizeof(path));
    // Best-effort: sys_unlink() failing because the fragment was already
    // absent is not distinguishable here from a real I/O failure without
    // errno plumbing this call site does not have, and either way an
    // uninstall must not be blocked on the menu bookkeeping - so this always
    // reports success, matching the header comment's contract.
    sys_unlink(path);
    return 0;
}
