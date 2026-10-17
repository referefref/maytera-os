// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// userconf.c - #683: where a PER-USER preference lives (userland side).
//
// WHY THIS EXISTS
// ---------------
// Measured under #674/#679 on a uid-1000 session, the desktop was refused five
// writes: /CONFIG/THEME.CFG, /CONFIG/AICHAT.CFG, /CONFIG/NOTIFY.TXT, /CONFIG
// itself, and /DOCKSTYL.CFG. Every one is a PER-USER PREFERENCE (theme, dock
// style, notification spool, AI chat settings) stored in /etc, or loose in /.
//
// The wrong fix is to give the compositor authority over /etc, by relaxing the
// mode or adding a privileged helper. The right fix is that the files are in
// the wrong place. /etc is SYSTEM configuration; a user's preferences belong to
// the user. Relocating them REMOVES the need for the privilege instead of
// GRANTING it, and a permission nobody has to ask for cannot be misused.
//
// THE CONVENTION: a per-user preference NAME lives at <home>/CONFIG/<NAME>,
// where <home> comes from the passwd table. It mirrors the existing
// /CONFIG/<NAME> layout so the mapping is mechanical, and it follows the
// home-directory convention already in the tree (the per-user profile at
// /HOME/ADMIN/UIPROFIL.YML, and the home skeleton users_make_home_skeleton()
// already creates owned by the user).
//
// MIGRATION IS A PROPERTY OF THE RULE, NOT A SEPARATE STEP. Existing installs
// have these files in /CONFIG, and a build that simply stopped reading the old
// location would silently lose the user's theme and dock on upgrade. So the
// rule is deliberately asymmetric:
//
//     READ  -> per-user path first, then FALL BACK to the legacy path.
//     WRITE -> always the per-user path.
//
// A user who has never changed their theme keeps reading the shipped
// /CONFIG/THEME.CFG. The first time they change it, the write lands in their
// home and every later read finds it there. No one-shot copy pass that could
// half-complete, no "have I migrated" flag to get wrong, and an administrator's
// edit to the /CONFIG copy still supplies the default for every user who has
// not overridden it. That is the same shape as /etc/skel plus a dotfile.
//
// ROOT IS A NO-OP BY CONSTRUCTION. /CONFIG/PASSWD gives root the home "/", so
// the join produces "/CONFIG/<NAME>": exactly the legacy path. The shipping
// root session therefore reads and writes precisely the files it does today,
// which is what makes this safe to land while autologin is still root.

#include "userconf.h"
#include "pwd.h"
#include "unistd.h"
#include "string.h"
#include "syscall.h"
#include "stdio.h"   // #appwrite: snprintf for the refusal line
#include "notify.h"  // [noaccount] a refused save must reach the PERSON, not only a log

// THE ONE HOME JOIN. Builds "<home>/<sub>/<name>" into out; `sub` may be NULL
// or "" for "<home>/<name>". Returns 0 on success, -1 if it will not fit. FAILS
// rather than truncating: a truncated path is a DIFFERENT file, and silently
// reading or writing the wrong one is worse than not doing it.
//
// #745: this was the body of userconf_path(), which now calls it with
// sub="CONFIG". It was lifted out because the App Store needs a path THE
// SESSION USER CAN WRITE for a 100MB download scratch file, which is the same
// question this file already answers for preferences, and the answer must not
// be forked. ROOT IS STILL A NO-OP BY CONSTRUCTION: root's home is "/", so
// hlen collapses to 0 and "<home>/<name>" is exactly "/<name>".
// THE home lookup, and the only one. Everything else in the tree that needs to
// know where the session user's home is goes through here or through
// userhome_path() below, which is now a caller of it.
// [noaccount] DID THE CACHED HOME COME FROM A REAL PASSWD ENTRY, or from the
// "/" fallback inside userhome_root()? The returned STRING cannot tell you:
// root's entry says "/" and the no-entry fallback also says "/", and those two
// sessions need OPPOSITE treatment (root may write "/"; a bootstrap uid may
// not). Recorded here, beside the lookup that knows, instead of being
// re-derived by a second getpwuid() in uc_state_root(): that cache exists
// precisely because the compositor's preference polls would otherwise parse
// /CONFIG/PASSWD four times a second, and a second lookup would restore the
// cost the cache was added to remove. Written only inside the resolve block
// below, so it is always in step with s_home.
static int uc_entry_flag = 0;

int userhome_root(char *out, unsigned long cap) {
    if (!out || cap < 2) return -1;

    // #peruser2: RESOLVED ONCE PER (PROCESS, UID). getpwuid() has no cache of
    // its own: libc/pwd.c's parse_passwd() opens, reads and parses
    // /CONFIG/PASSWD on EVERY call. Before this migration that cost was paid by
    // a handful of one-shot save sites and did not matter. It is now on the
    // COMPOSITOR'S throttled preference polls, which are the one place in this
    // userland where it would: notif.c reloads ALERTS.CFG about every 250ms and
    // lockscreen.c PRIVACY.CFG every 2s, so an uncached lookup would add four
    // PASSWD parses a second to the draw thread. That is the same per-frame
    // disk I/O that made Settings' Default Apps panel visibly re-pop (#262).
    //
    // KEYED BY THE UID, not merely "resolved once": `su` and `login` change uid
    // inside one process, and a cache that ignored that would hand the NEW
    // identity the OLD identity's home. That is a confidentiality bug, not a
    // stale value.
    //
    // AND THE BOOTSTRAP RESULT IS NEVER CACHED. A session with no account
    // resolves to "/", and that answer can become WRONG DURING THE PROCESS'S
    // LIFETIME: the OOBE wizard creates the first account while the desktop is
    // already running, after which this uid does have a home. Caching "/" would
    // pin the whole session to the pre-account answer and every per-user write
    // would keep going to the root-owned path it was refused from. The re-resolve
    // costs an open of a file that does not exist yet, which is the cheap case.
    static char s_home[192];
    static int  s_uid = -1;

    int uid = (int)getuid();
    int bootstrap_cached = (uid != 0 && s_home[0] == '/' && s_home[1] == '\0');
    if (s_uid != uid || s_home[0] == '\0' || bootstrap_cached) {
        const char *home = 0;
        struct passwd *pw = getpwuid((uid_t)uid);
        if (pw && pw->pw_dir && pw->pw_dir[0] == '/') home = pw->pw_dir;
        uc_entry_flag = home ? 1 : 0;     // [noaccount] set BEFORE the fallback
        if (!home) home = "/";            // no passwd entry: behave as before

        unsigned long hl = strlen(home);
        // Strip trailing separators but never the root itself: "/" must stay
        // "/", because an empty string is not a path and would silently become
        // relative.
        while (hl > 1 && home[hl - 1] == '/') hl--;
        if (hl + 1 > sizeof(s_home)) return -1;   // refuse rather than truncate
        for (unsigned long i = 0; i < hl; i++) s_home[i] = home[i];
        s_home[hl] = '\0';
        s_uid = uid;
    }

    unsigned long hlen = strlen(s_home);
    if (hlen + 1 > cap) return -1;        // refuse rather than truncate
    for (unsigned long i = 0; i < hlen; i++) out[i] = s_home[i];
    out[hlen] = '\0';
    return 0;
}

// [noaccount] DOES THIS SESSION HAVE A PASSWD ENTRY AT ALL. Not "is its home
// different from /": root's home IS "/" and root has an entry, while the
// first-boot bootstrap session has uid 1000 and NO entry, and userhome_root()
// hands both of them the same string. The userhome_root() call is what
// REFRESHES uc_entry_flag for this uid; reading the flag without it would
// return a stale answer after an su or a login.
static int uc_have_passwd_entry(void) {
    char home[192];
    if (userhome_root(home, sizeof(home)) != 0) return 0;
    return uc_entry_flag;
}

// ===========================================================================
// [noaccount] THE ROOT OF THIS SESSION'S PER-ACCOUNT STATE: ONE DECISION,
// DECIDED ONCE, AND EVERY ACCESSOR BELOW DERIVES FROM IT.
// ===========================================================================
//
// userhome_root() above answers the LITERAL question "what does the passwd
// table say". That is the right question for "does this machine have a home to
// show in a sidebar" (places.c) and the WRONG one for "where do I put this
// user's files", because on a machine with no account yet the answer is "/",
// and "/" is root-owned 0755. A Ring-3 process cannot create one name there,
// nor in the /CONFIG beneath it (root-owned 0711, searchable but not writable:
// kernel/fs/perms.c perms_fix_config_dir()).
//
// MEASURED, golden 2564 (commit 0fe362a4), ONE BOOT WITH NO USER INPUT AT ALL,
// read back out of /BOOTLOG.TXT: SEVEN per-account writes refused with -13,
// across six distinct files, every one of them carrying this file's own
// [NOHOME] marker:
//     /CONFIG/DOCKSTYL.CFG  /CONFIG/DOCKOPAC.CFG  /CONFIG/DOCKHGT.CFG
//     /CONFIG/DOCKZOOM.CFG  /CONFIG/THEME.CFG     /UIPROFIL.YML (twice)
// and, in the kernel's own refusal channel on the same boot, the Recycle Bin's
// daemon refused BOTH the creation of /CONFIG/RCYCLOG.TXT and a plain READ of
// /CONFIG/RECYCLE, a directory the KERNEL had created there moments earlier
// through its own mirror of these accessors. Every freshly built machine is in
// that state from power-on until the first-boot wizard creates an account.
//
// SEVEN IS WHAT ONE IDLE BOOT REACHES, NOT THE SIZE OF THE DEFECT. Eighty-two
// distinct per-account file names resolve through these accessors; sixty-seven
// of them use the plain (non-scratch) family and therefore all fail the same
// way the moment anything touches them. Correcting the seven at their write
// sites would have left seventy-five, so the answer is decided HERE:
//
//     has a passwd entry (INCLUDING root, whose entry says "/")
//                      -> that home. BYTE-IDENTICAL to the old behaviour.
//     no entry at all   -> "/TMP", mode 01777, seeded by perms_seed_system().
//
// ASKING FOR THE ENTRY RATHER THAN COMPARING THE STRING IS THE WHOLE TRICK.
// The earlier, narrower fix for the notification spool had to bolt an explicit
// `getuid() == 0` early-out onto a `home != "/"` test, because that test
// answers "no account" for a genuine root session and would have moved root's
// files out from under the kernel. With the right question there is no special
// case to remember and none to forget.
//
// THE SHAPE UNDER THE ROOT IS UNCHANGED, and that is the property that keeps
// both ends of every pair together. "<root>/CONFIG/<name>" and "<root>/<name>"
// are built by the same userhome_path_as() whatever the root is, so a file
// whose writer and reader both come through these accessors cannot land in two
// places. The kernel makes the SAME decision from the SAME two facts in
// rustkern/userconf.rs (userconf_state_root_rs); the "/TMP" literal is spelled
// out on both sides because Ring 3 cannot link rustkern, and the Rust
// self-test asserts the exact bytes this constant must equal.
//
// WHAT THIS DELIBERATELY DOES NOT DO: state written before an account exists
// is NOT migrated into the account afterwards. Adding the no-account location
// to every READ fallback chain would put a world-writable directory ahead of
// the system default for every logged-in user, which is a file-planting route,
// and the window being traded away is one run of the setup wizard on a machine
// that has never been used.
#define UC_NOACCOUNT_ROOT "/TMP"

static int uc_state_root(char *out, unsigned long cap) {
    if (uc_have_passwd_entry()) return userhome_root(out, cap);
    if (!out || cap < sizeof(UC_NOACCOUNT_ROOT)) return -1;   // never truncate
    for (unsigned long i = 0; i < sizeof(UC_NOACCOUNT_ROOT); i++)
        out[i] = UC_NOACCOUNT_ROOT[i];
    return 0;
}

// #harendezvous: THE JOIN, with the home as a parameter. Lifted verbatim out of
// userhome_path() (which is now its only unprivileged caller) so that a
// privileged service resolving ANOTHER principal's per-user path uses the same
// bytes of code as the app that wrote the file. A rendezvous whose two ends each
// build the path their own way is a rendezvous that works until one side is
// edited.
int userhome_path_as(const char *home, const char *sub, const char *name,
                     char *out, unsigned long cap) {
    if (!home || home[0] != '/') return -1;
    if (!name || !out || cap == 0) return -1;
    while (*name == '/') name++;          // accept "THEME.CFG" or "/THEME.CFG"
    if (!*name) return -1;
    if (sub) { while (*sub == '/') sub++; }

    unsigned long hlen = strlen(home);
    if (hlen == 1) hlen = 0;              // "/" contributes nothing; the join adds the '/'

    unsigned long slen = 0;
    if (sub) {
        slen = strlen(sub);
        while (slen > 0 && sub[slen - 1] == '/') slen--;
    }

    unsigned long nlen = strlen(name);
    // "<home>" "/" ["<sub>" "/"] "<name>"
    unsigned long total = hlen + 1 + (slen ? slen + 1 : 0) + nlen;
    if (total + 1 > cap) return -1;

    unsigned long w = 0;
    for (unsigned long i = 0; i < hlen; i++) out[w++] = home[i];
    out[w++] = '/';
    if (slen) {
        for (unsigned long i = 0; i < slen; i++) out[w++] = sub[i];
        out[w++] = '/';
    }
    for (unsigned long i = 0; i < nlen; i++) out[w++] = name[i];
    out[w] = '\0';
    return 0;
}

int userhome_path(const char *sub, const char *name, char *out, unsigned long cap) {
    // [noaccount] uc_state_root(), not userhome_root(): see the long note on
    // uc_state_root(). For every session that HAS an account, including root,
    // the two are the same string and this call is byte-identical to before.
    char home[192];
    if (uc_state_root(home, sizeof(home)) != 0) return -1;
    return userhome_path_as(home, sub, name, out, cap);
}

// #harendezvous: see userconf.h. THE FALLBACK IS THE POINT, not politeness.
// sys_session_home() refuses a non-root caller, and a non-root caller's correct
// answer is its own home, which is what userhome_root() returns; so one function
// is right for both principals and no call site has to branch on getuid().
int userhome_root_session(char *out, unsigned long cap) {
    if (!out || cap < 2) return -1;
    if (getuid() == 0) {
        char k[192];
        int n = sys_session_home(k, (int)sizeof(k));
        // n > 1 only: a kernel answer of "/" is indistinguishable from
        // userhome_root()'s own fallback, so there is nothing to prefer, and
        // taking it here would skip the uid-keyed cache for no gain.
        if (n > 1 && k[0] == '/') {
            unsigned long kl = strlen(k);
            if (kl + 1 > cap) return -1;       // refuse rather than truncate
            for (unsigned long i = 0; i <= kl; i++) out[i] = k[i];
            return 0;
        }
    }
    // [noaccount] uc_state_root(), not userhome_root(). A service resolving
    // the requester's path must land on the SAME root the requester wrote to,
    // and on a no-account machine that is /TMP, not "/". SYS_SESSION_HOME
    // (kernel/proc/syscall.c) now answers the state root for exactly this
    // reason, so the kernel branch above and this fallback agree.
    return uc_state_root(out, cap);
}

int userhome_path_session(const char *sub, const char *name,
                          char *out, unsigned long cap) {
    char home[192];
    if (userhome_root_session(home, sizeof(home)) != 0) return -1;
    return userhome_path_as(home, sub, name, out, cap);
}

int userhome_open_read_session(const char *sub, const char *name,
                               const char *legacy) {
    char p[256];
    if (userhome_path_session(sub, name, p, sizeof(p)) == 0) {
        int fd = sys_open(p, 0 /*O_RDONLY*/);
        if (fd >= 0) return fd;
    }
    if (!legacy) return -1;
    return sys_open(legacy, 0 /*O_RDONLY*/);
}

int userconf_path(const char *name, char *out, unsigned long cap) {
    return userhome_path("CONFIG", name, out, cap);
}

// Open a per-user preference for READING, falling back to `legacy` when the
// per-user copy does not exist yet. `legacy` is the pre-#683 absolute path.
// This is the half of the rule that makes an upgrade keep the user's settings.
int userhome_open_read(const char *sub, const char *name, const char *legacy) {
    char p[256];
    if (userhome_path(sub, name, p, sizeof(p)) == 0) {
        int fd = sys_open(p, 0 /*O_RDONLY*/);
        if (fd >= 0) return fd;
    }
    if (!legacy) return -1;
    return sys_open(legacy, 0 /*O_RDONLY*/);
}

int userconf_open_read(const char *name, const char *legacy) {
    return userhome_open_read("CONFIG", name, legacy);
}

// Open a per-user preference for WRITING. Always the per-user path, never the
// legacy one: writing back to /CONFIG is the thing being eliminated.
// ---------------------------------------------------------------------------
// #appwrite: A FAILED PREFERENCE WRITE MUST LEAVE A TRACE SOMEWHERE THE OWNER
// CAN READ, AND UNTIL NOW IT DID NOT.
//
// MEASURED on golden 2480 (build a1a344eb, VM 2498): a Ring-3 process at
// uid 1000 gets -13 from sys_open(O_WRONLY|O_CREAT) for every destination
// outside its own home, and on a machine with no account yet (which is every
// freshly built golden, because #226 ships no /CONFIG/PASSWD on purpose) that
// includes its home too, because userhome_root() resolves to "/".
//
// The two mechanisms this userland uses to TELL somebody a save failed are
// themselves writes to those same places: notify_post() spools through this
// very function, and Settings' save_failed() writes /SETLOG.TXT at the ext2
// root. So a careful caller that checks every return value still produces
// nothing a person can see. That is why a facility that has never worked
// looked like an assortment of unrelated app bugs.
//
// sys_bootlog() is the one reporting channel MEASURED to work from a uid-1000
// app on this image: every line appears on serial as
// "[BOOTLOG] [USERSPACE uid=1000] ..." and is replayed into /BOOTLOG.TXT,
// which the kernel writes, so it does not depend on the permission that just
// failed. It is not a user-visible toast and this does not pretend to be one;
// it is the difference between a diagnosable failure and an invisible one.
//
// BOUNDED, for the same reason perms.c bounds [PERMS-DENY]: a preference save
// inside an app's event loop could otherwise flood the boot log and push the
// earlier, more informative lines out of it.
#define UC_REFUSAL_LOG_MAX 32
// [noaccount] separate from the log cap on purpose: see the note at the end of
// uc_report_write_refusal(). Three is enough to tell a person that saving is
// broken and few enough that it cannot bury the desktop.
#define UC_REFUSAL_TOAST_MAX 3
void uc_report_write_refusal(const char *what, const char *path, int rc) {
    static int logged = 0;
    if (logged >= UC_REFUSAL_LOG_MAX) return;
    logged++;
    // #aiaudit: CLAMPED, because an over-long message is not truncated by the
    // kernel, it is DROPPED ENTIRELY. sys_bootlog_write() bounces through a
    // char[200] via strncpy_from_user(), which returns EFAULT rather than a
    // short copy when no NUL falls inside that window, and the syscall then
    // logs nothing. A 224-byte buffer plus an unbounded %s for `path` made this
    // reporter silently droppable for exactly the long paths a per-user
    // relocation produces. MEASURED as a real loss on the AI audit sink
    // (golden 2486, VM 2497); fixed at both reporters, not just the one that
    // happened to be caught.
    // #peruser2: "EACCES" and "EACCES BECAUSE THIS SESSION HAS NO HOME AT ALL"
    // are different diagnoses and the second one is not the app's fault. A
    // freshly built golden ships no /CONFIG/PASSWD on purpose (#226/#202), so
    // login_run() hands out a BOOTSTRAP session: uid 1000, no account, and
    // userhome_root() falling back to "/". Every per-user path then resolves to
    // the legacy root-owned one and every write is refused, which looks
    // identical to a permission bug in whichever app reported last. The marker
    // goes FIRST, right after the tag, because sys_bootlog() drops an over-long
    // line whole rather than truncating it, so anything that must survive has to
    // be at the front (see the clamp note below).
    int nohome = 0;
    if (getuid() != 0) {
        struct passwd *pw = getpwuid(getuid());
        if (!pw || !pw->pw_dir || pw->pw_dir[0] != '/' ||
            (pw->pw_dir[0] == '/' && pw->pw_dir[1] == '\0'))
            nohome = 1;
    }
    char msg[180];
    snprintf(msg, sizeof(msg),
             "[FSDENY-U]%s %.24s REFUSED path=%.100s rc=%d uid=%d%s",
             nohome ? " [NOHOME]" : "",
             what ? what : "?", path ? path : "?", rc, getuid(),
             logged == UC_REFUSAL_LOG_MAX ? " (log cap reached)" : "");
    (void)sys_bootlog(msg);

    // [noaccount] AND TELL THE PERSON. A line in /BOOTLOG.TXT is a breadcrumb
    // for whoever already suspects something; it is not the OS saying "your
    // setting was not saved". The seven refusals measured on an idle boot of
    // golden 2564 had been happening on every machine since the image was
    // built and nothing on screen ever said so. Fixing only the path would
    // leave the NEXT cause of a refused save exactly as invisible as this one
    // was, which is why this lands in the same change and not after it.
    //
    // THREE THINGS KEEP THIS FROM BECOMING THE NOISE IT IS REPLACING:
    //  * REENTRANCY. notify_post() posts by WRITING A FILE, through these very
    //    accessors. Without the guard, a machine where writes are refused
    //    would recurse until it ran out of stack. The guard is checked and set
    //    around the call, not merely tested.
    //  * A CAP. At most UC_REFUSAL_TOAST_MAX per process per boot, so a
    //    polling save cannot paper the screen. The bootlog line above is NOT
    //    capped to the same number: diagnosis wants every event, a person does
    //    not.
    //  * REPEAT SUPPRESSION on the path, so a save retried in a loop says it
    //    once. Compared by hash of the string rather than kept verbatim: this
    //    runs inside libc in every process, and a second 256-byte buffer per
    //    process to make an error message prettier is not a trade worth making.
    {
        static int  toasted = 0;
        static int  in_report = 0;
        static unsigned int last_h = 0;
        unsigned int h = 0;
        for (const char *q = path ? path : "?"; *q; q++)
            h = h * 131u + (unsigned char)*q;
        if (!in_report && toasted < UC_REFUSAL_TOAST_MAX && h != last_h) {
            in_report = 1;
            last_h = h;
            toasted++;
            char tb[160];
            snprintf(tb, sizeof(tb), "%.100s could not be written%s",
                     path ? path : "(unknown path)",
                     nohome ? " (this machine has no account yet)" : "");
            (void)notify_post("Save failed", tb, NOTIFY_ERROR);
            in_report = 0;
        }
    }
}

// #aiaudit: THE one place that makes <home>/CONFIG. See userconf.h for why it
// is shared rather than inlined here. Since #679 the user OWNS its home, so
// this succeeds for a non-root session; mkdir is idempotent, so a second call
// is a no-op.
// #prewrite: MKDIR -P, not one level.
//
// This created only the LAST component, which was enough while every caller
// asked for <home>/CONFIG/<name> and users_make_home_skeleton() had already
// made <home>/CONFIG's parent. The owner's layout has a two-component tier
// (<home>/APPS/<app>/<file>), and for that the single-level version fails with
// ENOENT on the intermediate directory rather than creating it. sys_mkdir()
// bottoms out in ONE fat_mkdir() with no parent creation, so the walk has to
// be here.
//
// Each component is created and the result deliberately ignored: "already
// exists" and "created" are equally fine, and the open that follows is the real
// test. Bounded by the buffer, and it never tries to create "/" itself.
void userconf_ensure_parent_dir(const char *path) {
    if (!path) return;
    char dir[256];
    unsigned long i = 0, cut = 0;
    while (path[i]) { if (path[i] == '/') cut = i; i++; }
    if (cut == 0 || cut >= sizeof(dir)) return;     // "/NAME": parent is "/"
    for (unsigned long k = 0; k < cut; k++) dir[k] = path[k];
    dir[cut] = '\0';

    // Walk forward, creating each intermediate component. Start at 1 so the
    // leading '/' is never a mkdir target of its own.
    for (unsigned long k = 1; k < cut; k++) {
        if (dir[k] != '/') continue;
        dir[k] = '\0';
        sys_mkdir(dir, 0755);
        dir[k] = '/';
    }
    sys_mkdir(dir, 0755);
}

// ---------------------------------------------------------------------------
// #prewrite: THE NAMED PLACES IN A USER'S HOME.
//
// WHY THESE EXIST RATHER THAN LETTING EACH APP JOIN ITS OWN STRING. If the only
// way an app can find a user's music directory is to build "<home>/MUSIC"
// itself, every app will, and the layout rots the first time it changes: there
// would be no single place to change it, and no way to tell which literals were
// the layout and which were somebody's data file that happens to live nearby.
// The Files app's quick locations, the shared file-browser used by other apps,
// and anything storing per-app data are all consumers of the SAME question, so
// the question is asked in one place.
//
// THE LAYOUT, and which parts of it are real. users_make_home_skeleton()
// (kernel/proc/users.c) already creates DESKTOP, DOCUMENT, DOWNLOAD, PICTURES,
// MUSIC, VIDEOS, APPS, CONFIG and GAMES, owned by the account at 0750, at
// ACCOUNT CREATION and again at every login. So these are real directories,
// not aspirational labels, and the accessors below name the ones that exist.
// NOTE THE TWO SINGULARS: the skeleton makes DOCUMENT and DOWNLOAD, not
// DOCUMENTS/DOWNLOADS, while the Files app labels them "Documents" and
// "Downloads". The accessor returns THE DIRECTORY THAT EXISTS; fixing the
// spelling is a rename of a shipped directory and is not smuggled in here.
//
// CASE: ext2 lookup folds case when the root is ext2 (kernel/fs/ext2.c, the
// `ci` argument is g_root_ext2), so "<workspace>" and "/HOME/REF/MUSIC"
// resolve to the same object and the owner's lowercase spelling works today
// with no rename. CREATION does not fold (ext2_dirblock_insert_c stores the
// name verbatim), so these accessors emit UPPERCASE to match every directory
// already on the image rather than producing a case-mixed tree that resolves
// correctly and reads badly.
// ---------------------------------------------------------------------------

// "<home>/<sub>", the DIRECTORY itself, with no name joined onto it. For a
// caller that needs somewhere to browse or scan, not a single file.
int userhome_dir(const char *sub, char *out, unsigned long cap) {
    // [noaccount] the DIRECTORY form has to move with the FILE form or the two
    // disagree: an app that navigates to userhome_dir() and an app that writes
    // through userhome_path() would be looking at different places.
    if (!out || cap == 0) return -1;
    if (!sub || !*sub) return uc_state_root(out, cap);
    while (*sub == '/') sub++;
    if (!*sub) return uc_state_root(out, cap);

    char home[192];
    if (uc_state_root(home, sizeof(home)) != 0) return -1;
    unsigned long hlen = strlen(home);
    if (hlen == 1) hlen = 0;              // "/" contributes nothing
    unsigned long slen = strlen(sub);
    while (slen > 0 && sub[slen - 1] == '/') slen--;
    if (hlen + 1 + slen + 1 > cap) return -1;   // refuse rather than truncate

    unsigned long w = 0;
    for (unsigned long i = 0; i < hlen; i++) out[w++] = home[i];
    out[w++] = '/';
    for (unsigned long i = 0; i < slen; i++) out[w++] = sub[i];
    out[w] = '\0';
    return 0;
}

// This user's music directory. The owner's `/home/<user>/music`.
int userhome_music_dir(char *out, unsigned long cap) {
    return userhome_dir("MUSIC", out, cap);
}

// --- [filesui] the rest of the skeleton, one accessor each -------------------
//
// THE ON-DISK NAMES LIVE HERE AND NOWHERE ELSE IN USERLAND. They are macros
// only so that the string appears once per directory even within this file, and
// so that the singular/plural question below is one edit rather than a grep.
//
// DOCUMENTS and DOWNLOADS are PLURAL, and that is a change. See userconf.h for
// why the directory was the wrong half of the mismatch: the 8.3 rationale for
// truncating them died when homes moved to the ext2 root, while the label the
// user reads was already plural in Files and is plural in every other system.
// kernel/proc/users.c users_make_home_skeleton() creates these names and
// migrates a legacy singular directory to them; if that migration has not run
// for a given home, the directory is simply absent and the row opens an empty
// view, which is visible and fixable, not silent.
#define UH_DESKTOP    "DESKTOP"
#define UH_DOCUMENTS  "DOCUMENTS"
#define UH_DOWNLOADS  "DOWNLOADS"
#define UH_PICTURES   "PICTURES"
#define UH_VIDEOS     "VIDEOS"

int userhome_desktop_dir(char *out, unsigned long cap)   { return userhome_dir(UH_DESKTOP,   out, cap); }
int userhome_documents_dir(char *out, unsigned long cap) { return userhome_dir(UH_DOCUMENTS, out, cap); }
int userhome_downloads_dir(char *out, unsigned long cap) { return userhome_dir(UH_DOWNLOADS, out, cap); }
int userhome_pictures_dir(char *out, unsigned long cap)  { return userhome_dir(UH_PICTURES,  out, cap); }
int userhome_videos_dir(char *out, unsigned long cap)    { return userhome_dir(UH_VIDEOS,    out, cap); }

// A plain absolute-literal copy, shared by the three directory accessors below.
// Bounds-checked and refuses rather than truncating, exactly like every other
// accessor here: a truncated path is a different directory.
static int uc_copy_abs(const char *lit, char *out, unsigned long cap) {
    if (!out || cap == 0) return -1;
    unsigned long n = strlen(lit);
    if (n + 1 > cap) return -1;
    for (unsigned long i = 0; i < n; i++) out[i] = lit[i];
    out[n] = '\0';
    return 0;
}

// The all-users directory itself. shared_path() builds a NAME inside it; a
// chooser needs the directory. Same literal, one definition.
int shared_dir(char *out, unsigned long cap) { return uc_copy_abs("/SHARED", out, cap); }

// [foundbugs] THE ONE ANSWER TO "does this session have an account yet".
// userscratch_dir(), userscratch_path() and userconf_path_scratch() each
// needed this exact question, and before this it was the same four-line
// check inlined in each of them. A third or fourth inline copy is exactly
// how the others stop agreeing with each other: one gets edited (a different
// cap on `home[]`, a different NUL check) and the rest quietly keep the old
// answer. This is the one definition all of them call.
static int uc_session_has_account(void) {
    char home[192];
    return userhome_root(home, sizeof(home)) == 0 &&
           !(home[0] == '/' && home[1] == '\0');
}

// The scratch DIRECTORY, with exactly the session rule userscratch_path() uses:
// the home when there is an account, /TMP when there is not.
int userscratch_dir(char *out, unsigned long cap) {
    if (uc_session_has_account()) return userhome_dir(0, out, cap);
    return uc_copy_abs("/TMP", out, cap);
}

// Shipped sample media, root-owned 0755. See userconf.h: this is NOT the user's
// own music and must never be labelled as if it were.
int media_music_dir(char *out, unsigned long cap) { return uc_copy_abs("/MEDIA/MUSIC", out, cap); }
int media_dir(char *out, unsigned long cap)       { return uc_copy_abs("/MEDIA",       out, cap); }

// This user's per-app data directory for `app`: the owner's
// `/home/<user>/apps/<appname>/`. `app` is a bare name; a caller that passes a
// path separator, a "." or a ".." is REFUSED rather than normalised, because
// an app name is not a path and treating it as one is how an app-data accessor
// becomes a way to write outside the home.
int userhome_app_dir(const char *app, char *out, unsigned long cap) {
    if (!app || !*app) return -1;
    for (const char *q = app; *q; q++) {
        if (*q == '/' || *q == '\\') return -1;
        if ((unsigned char)*q < 0x20) return -1;
    }
    if (app[0] == '.') return -1;         // "." and ".." and hidden shapes
    char sub[96];
    unsigned long n = strlen(app);
    if (n + 6 > sizeof(sub)) return -1;   // "APPS/" + NUL
    sub[0] = 'A'; sub[1] = 'P'; sub[2] = 'P'; sub[3] = 'S'; sub[4] = '/';
    for (unsigned long i = 0; i < n; i++) sub[5 + i] = app[i];
    sub[5 + n] = '\0';
    return userhome_dir(sub, out, cap);
}

// One file inside this user's per-app data directory: the owner's
// `/home/<user>/apps/<appname>/database.db`.
int userhome_app_path(const char *app, const char *name, char *out, unsigned long cap) {
    char dir[192];
    if (userhome_app_dir(app, dir, sizeof(dir)) != 0) return -1;
    if (!name) return -1;
    while (*name == '/') name++;
    if (!*name) return -1;
    unsigned long dl = strlen(dir), nl = strlen(name);
    if (dl + 1 + nl + 1 > cap) return -1;
    unsigned long w = 0;
    for (unsigned long i = 0; i < dl; i++) out[w++] = dir[i];
    out[w++] = '/';
    for (unsigned long i = 0; i < nl; i++) out[w++] = name[i];
    out[w] = '\0';
    return 0;
}

// "/SHARED/<name>", the all-users space. Not derived from a home: it is the
// same directory for everyone, which is the point. Owner-only-delete is
// enforced in the KERNEL (fs/perms.c perms_may_remove), not here: a userland
// accessor is a convenience and can never be a security boundary.
int shared_path(const char *name, char *out, unsigned long cap) {
    static const char pre[] = "/SHARED/";
    if (!name || !out) return -1;
    while (*name == '/') name++;
    if (!*name) return -1;
    unsigned long pl = sizeof(pre) - 1, nl = strlen(name);
    if (pl + nl + 1 > cap) return -1;
    for (unsigned long i = 0; i < pl; i++) out[i] = pre[i];
    for (unsigned long i = 0; i < nl; i++) out[pl + i] = name[i];
    out[pl + nl] = '\0';
    return 0;
}

// SCRATCH, and this is the accessor that fixes the pre-account machine.
//
// A freshly built golden ships no /CONFIG/PASSWD on purpose (#226/#202), so
// login_run() hands out a BOOTSTRAP session: uid 1000, no account, and
// userhome_root() resolving to "/". Every per-user path then lands on a
// root-owned location and every write is refused with -13, which is why the
// App Store could not stage a download and why 34 per-user writes were
// refused on one measured boot.
//
// So: the user's home when there IS one (a scratch file belongs to its user
// and should be cleaned up with them), and /TMP when there is not. /TMP is
// 01777 like /SHARED, so a bootstrap session can create a name there and no
// later account can delete or replace what it left.
int userscratch_path(const char *name, char *out, unsigned long cap) {
    if (!name || !out) return -1;
    while (*name == '/') name++;
    if (!*name) return -1;

    if (uc_session_has_account()) return userhome_path(0, name, out, cap);
    // No account yet. NOT "/" (root-owned 0755, and the reason the App Store
    // fell over), and not a hardcoded literal at each call site either.
    static const char pre[] = "/TMP/";
    unsigned long pl = sizeof(pre) - 1, nl = strlen(name);
    if (pl + nl + 1 > cap) return -1;
    for (unsigned long i = 0; i < pl; i++) out[i] = pre[i];
    for (unsigned long i = 0; i < nl; i++) out[pl + i] = name[i];
    out[pl + nl] = '\0';
    return 0;
}

// #peruser2: THE open-for-write, for a path the caller has already resolved.
// Everything below funnels through here so that "make the parent", "O_TRUNC not
// unlink" and "a refusal is never silent" have exactly one implementation.
int userconf_open_write_path(const char *path) {
    if (!path || !path[0]) return -1;
    userconf_ensure_parent_dir(path);
    int fd = sys_open(path, 0x41 | 0x200 /*O_WRONLY|O_CREAT|O_TRUNC*/);
    if (fd < 0) uc_report_write_refusal("userconf_open_write", path, fd);
    return fd;
}

// [foundbugs] see userconf.h.
//
// THE QUESTION THIS ASKS IS NOT "does this session have a home". It is "can
// the Ring-3 side of this rendezvous actually reach <home>/CONFIG/<name>",
// and uid 0 always can: kernel/fs/perms.c:1992 returns allow on its first
// line for uid 0, whatever the mode on the directory. So root keeps the
// ordinary path, exactly as the kernel-side mirror
// (kernel/fs/userconf.c userconf_kpath_scratch()) does for the same uid.
//
// THAT uid-0 EARLY-OUT IS LOAD-BEARING, NOT POLITENESS. Root's own home IS
// "/" in the passwd table, so uc_session_has_account() answers "no" for a
// root session, and without this line the userland side would move root's
// spool to /TMP while the kernel side kept writing /CONFIG. The whole point
// of this file agreeing with the kernel one is that the notification spool
// is a SINGLE file written by three parties; two of them disagreeing is the
// split-brain that libc/notify.c's header warns about, and it would have hit
// the one session type (root) where neither side was ever broken to begin
// with. Caught by walking the root case through both sides rather than by a
// test, which is why it is written down here.
//
// Only a NON-root session whose resolved home is still "/" (the #OOBEAUTH
// bootstrap placeholder, no passwd entry at all) takes the /TMP branch.
// [noaccount] THESE THREE ARE NOW THE ORDINARY ACCESSORS, and keeping them as
// names rather than deleting them is deliberate for exactly one release: their
// five call sites (libc/notify.c, compositor/notif.c, and the kernel mirror)
// are a THREE-PARTY rendezvous on one file, and a rename touching all of them
// in the same change as a path change would make it impossible to say which
// edit moved the file.
//
// THEY USED TO BE DIFFERENT, AND THE DIFFERENCE WAS THE BUG. The scratch
// variant answered "/TMP/<name>" FLAT, dropping the CONFIG component, because
// it was the only accessor that knew about the no-account case. Now that
// uc_state_root() knows, "<root>/CONFIG/<name>" is correct for every session
// and the flat form would be a SECOND spelling of the same file. The spool
// therefore moves from /TMP/NOTIFY.TXT to /TMP/CONFIG/NOTIFY.TXT on a
// no-account machine - on ALL FIVE sites at once, because all five resolve
// through this one joiner, which is the property that stops a split spool.
int userconf_path_scratch(const char *name, char *out, unsigned long cap) {
    return userconf_path(name, out, cap);
}

int userconf_open_write_scratch(const char *name) {
    return userconf_open_write(name);
}

// No legacy fallback, unlike userconf_open_read(): the notification spool is
// not a preference with a shipped default, and falling back to a system copy
// would make one user read another's notifications.
int userconf_open_read_scratch(const char *name) {
    char p[256];
    if (userconf_path(name, p, sizeof(p)) != 0) return -1;
    return sys_open(p, 0 /*O_RDONLY*/);
}

int userhome_open_write(const char *sub, const char *name) {
    char p[256];
    if (userhome_path(sub, name, p, sizeof(p)) != 0) return -1;
    return userconf_open_write_path(p);
}

int userconf_open_write(const char *name) {
    return userhome_open_write("CONFIG", name);
}

// #peruser2: see userconf.h. A refused chmod does not fail the save.
void userconf_protect(const char *path) {
    if (!path || !path[0]) return;
    if (chmod(path, 0600) != 0)
        uc_report_write_refusal("userconf_protect", path, -1);
}

// ---------------------------------------------------------------------------
// #743: COMPLETING a write, and why this is a shared function and not a
// four-line idiom repeated at every save site.
//
// The saves in this tree were all written as the same five steps:
//
//     sys_unlink(PATH);                       // 1. destroy the old copy
//     int fd = sys_open(PATH, O_WRONLY|O_CREAT);
//     if (fd < 0) return;                     // 2. ...and give up
//     sys_write(fd, buf, len);                // 3. result discarded
//     sys_close(fd);                          // 4. result discarded
//                                             // 5. no fsync at all
//
// Every one of those steps is a defect and they compound:
//
//   1+2  THE FAILURE PATH IS THE DELETE PATH. The unlink ALWAYS runs, the open
//        may not. A read-only volume, a full volume, a permission refusal, or
//        an exhausted fd table therefore does not fail to save: it DESTROYS
//        the users

// ---------------------------------------------------------------------------
// #743: COMPLETING a write, and why this is a shared function and not a
// four-line idiom repeated at every save site.
//
// The config saves in this tree were all written as the same five steps:
//
//     sys_unlink(PATH);                        // 1. destroy the old copy
//     int fd = sys_open(PATH, O_WRONLY|O_CREAT);
//     if (fd < 0) return;                      // 2. ...and give up
//     sys_write(fd, buf, len);                 // 3. result discarded
//     sys_close(fd);                           // 4. result discarded
//                                              // 5. no fsync at all
//
// Every one of those steps is a defect and they compound:
//
//   1+2  THE FAILURE PATH IS THE DELETE PATH. The unlink ALWAYS runs, the open
//        may not. A read-only volume, a full volume, a permission refusal or an
//        exhausted fd table therefore does not "fail to save": it DESTROYS the
//        user's existing configuration and then returns as if nothing had
//        happened. This is the identical shape as the #742 Recycle Bin fault,
//        where a failed move to the bin ran unlink(src) and permanently deleted
//        the file. The unlink is not needed at all: O_TRUNC empties the file
//        only AFTER the open has succeeded, which is the ordering actually
//        wanted.
//
//   3    A partial write is a CORRUPT config and a failed write is no config,
//        and neither is distinguishable from success by the caller.
//
//   4    close() is where a buffered filesystem reports the error it could not
//        report earlier: release() returns int precisely so that it can. A
//        discarded close() result throws away the only report there was.
//
//   5    Without fsync the bytes may never reach the medium, so a save the user
//        was told had succeeded can still be absent after a power loss.
//
// KNOWN LIMIT, stated rather than implied away. This makes a failed save
// REPORTABLE, and stops a failed save from destroying the previous config at
// the OPEN stage. It does NOT make the replacement atomic: if the write itself
// fails midway the file is left truncated. The usual fix is write-to-temp then
// atomic rename, and that is NOT available here: sys_rename() bottoms out in
// fat_rename(), which for an ext2 path is copy-then-delete (not atomic), and
// for a FAT path adds a directory entry WITHOUT removing an existing one at the
// destination, so renaming over an existing file is not supported. An atomic
// replace primitive is a kernel change and a separate piece of work.

// Write every byte of `buf` to `fd`, flush it, then close it. ALWAYS closes fd,
// including on every error path, so a caller can never leak one. Returns 0 only
// if the write, the fsync AND the close all succeeded.
int userconf_finish_write(int fd, const void *buf, unsigned long len) {
    if (fd < 0) return -1;
    const char *p = (const char *)buf;
    unsigned long done = 0;
    int bad = 0;

    // A short write is not an error: satisfying part of a request is allowed.
    // Loop, but treat 0 and negative alike as failure, so a driver that always
    // returns 0 cannot spin here forever.
    while (done < len) {
        long n = sys_write(fd, p + done, len - done);
        if (n <= 0) { bad = 1; break; }
        done += (unsigned long)n;
    }

    // fsync BEFORE close: a failure here means the bytes are not on the medium.
    if (!bad && sys_fsync(fd) != 0) bad = 1;

    // close() is checked, not discarded. It is the last chance the filesystem
    // has to report a deferred error, and on this kernel release() returns int
    // specifically so that such an error can reach Ring 3.
    if (sys_close(fd) != 0) bad = 1;

    return bad ? -1 : 0;
}

// Replace the entire contents of `path`. Returns 0 on success, -1 on failure.
//
// Deliberately does NOT unlink first. O_TRUNC empties the file only once the
// open has succeeded, so a refused open leaves the PREVIOUS configuration
// exactly where it was, instead of deleting it and only then discovering that
// no replacement can be written.
int userconf_write_all(const char *path, const void *buf, unsigned long len) {
    if (!path) return -1;
    // #peruser2: MAKE THE PARENT. This was the one asymmetry between the two
    // write entry points: userconf_open_write() created <home>/CONFIG and this
    // did not, so a caller that resolved a per-user path ITSELF and handed it
    // here got ENOENT on a home that predates the convention, which is every
    // home on an upgraded install. Idempotent, and a no-op for a "/NAME" path.
    userconf_ensure_parent_dir(path);
    int fd = sys_open(path, 0x41 | 0x200 /*O_WRONLY|O_CREAT|O_TRUNC*/);
    if (fd < 0) {
        // #appwrite: the OTHER entry point, and the one that matters most for
        // diagnosability. Settings' save_failed() reports into setlog(), which
        // lands here with "/SETLOG.TXT" at the ext2 root, root-owned 0755, and
        // deliberately discards the result because "there is no fallback for a
        // breadcrumb". Correct as far as it goes, and it means the app's ENTIRE
        // failure-reporting channel is a write to a path a non-root session
        // cannot create. Every panel that dutifully checks its save and calls
        // save_failed() has been writing into a void.
        uc_report_write_refusal("userconf_write_all", path, fd);
        return -1;
    }
    return userconf_finish_write(fd, buf, len);
}
