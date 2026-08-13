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
int userhome_root(char *out, unsigned long cap) {
    if (!out || cap < 2) return -1;

    const char *home = 0;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] == '/') home = pw->pw_dir;
    if (!home) home = "/";                // no passwd entry: behave as before

    unsigned long hlen = strlen(home);
    // Strip trailing separators but never the root itself: "/" must stay "/",
    // because an empty string is not a path and would silently become relative.
    while (hlen > 1 && home[hlen - 1] == '/') hlen--;

    if (hlen + 1 > cap) return -1;        // refuse rather than truncate
    for (unsigned long i = 0; i < hlen; i++) out[i] = home[i];
    out[hlen] = '\0';
    return 0;
}

int userhome_path(const char *sub, const char *name, char *out, unsigned long cap) {
    if (!name || !out || cap == 0) return -1;
    while (*name == '/') name++;          // accept "THEME.CFG" or "/THEME.CFG"
    if (!*name) return -1;
    if (sub) { while (*sub == '/') sub++; }

    char home[192];
    if (userhome_root(home, sizeof(home)) != 0) return -1;

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

int userconf_path(const char *name, char *out, unsigned long cap) {
    return userhome_path("CONFIG", name, out, cap);
}

// Open a per-user preference for READING, falling back to `legacy` when the
// per-user copy does not exist yet. `legacy` is the pre-#683 absolute path.
// This is the half of the rule that makes an upgrade keep the user's settings.
int userconf_open_read(const char *name, const char *legacy) {
    char p[256];
    if (userconf_path(name, p, sizeof(p)) == 0) {
        int fd = sys_open(p, 0 /*O_RDONLY*/);
        if (fd >= 0) return fd;
    }
    if (!legacy) return -1;
    return sys_open(legacy, 0 /*O_RDONLY*/);
}

// Open a per-user preference for WRITING. Always the per-user path, never the
// legacy one: writing back to /CONFIG is the thing being eliminated.
int userconf_open_write(const char *name) {
    char p[256];
    if (userconf_path(name, p, sizeof(p)) != 0) return -1;

    // Ensure <home>/CONFIG exists. users_make_home_skeleton() creates DESKTOP,
    // DOCUMENT, DOWNLOAD, MUSIC, PICTURES and VIDEOS but not CONFIG, and every
    // home that already exists on an installed system predates this change
    // regardless. mkdir is idempotent, and since #679 the user OWNS its home,
    // so this succeeds for a non-root session; the return value is deliberately
    // ignored because "already exists" and "created" are equally fine and the
    // open below is the real test.
    char dir[256];
    unsigned long i = 0, cut = 0;
    while (p[i]) { if (p[i] == '/') cut = i; i++; }
    if (cut > 0 && cut < sizeof(dir)) {
        for (unsigned long k = 0; k < cut; k++) dir[k] = p[k];
        dir[cut] = '\0';
        sys_mkdir(dir, 0755);
    }
    return sys_open(p, 0x41 | 0x200 /*O_WRONLY|O_CREAT|O_TRUNC*/);
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
    int fd = sys_open(path, 0x41 | 0x200 /*O_WRONLY|O_CREAT|O_TRUNC*/);
    if (fd < 0) return -1;
    return userconf_finish_write(fd, buf, len);
}
