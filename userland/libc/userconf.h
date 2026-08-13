// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// userconf.h - #683: per-user preference paths. See userconf.c for the rule.
//
// A per-user preference NAME lives at <home>/CONFIG/<NAME>. READS fall back to
// the legacy /CONFIG (or /) location so an upgrade keeps the user's settings;
// WRITES always go to the user's home, which is what stops the desktop needing
// write authority over /etc. Root's home is "/", so for a root session every
// path below resolves to exactly the legacy one and nothing changes.
//
// Names must be FAT 8.3 (uppercase, no leading dot), like the rest of the tree.
#ifndef _USERCONF_H
#define _USERCONF_H

// Build "<home>/<sub>/<name>", where <home> comes from the passwd table. `sub`
// may be NULL or "" for "<home>/<name>". Returns 0 on success, -1 if it will not
// fit. Fails rather than truncating: a truncated path is a different file.
//
// #745: this is the join userconf_path() always performed, lifted out so that
// anything needing a path THE SESSION USER CAN WRITE uses one implementation.
// Root's home is "/", so for a root session "<home>/<name>" is exactly "/<name>"
// and nothing about a root session changes.
int userhome_path(const char *sub, const char *name, char *out, unsigned long cap);

// The session user's home directory itself, with no trailing '/' (so "/" stays
// "/", and "/HOME/ADMIN/" becomes "/HOME/ADMIN"). Returns 0 on success, -1 if
// it will not fit.
//
// #745: this is the SAME lookup userhome_path() performs, factored out rather
// than copied, because the per-user install needs the home as a SANDBOX ROOT to
// confine package destinations against (pkgdest.c), not as a prefix to join a
// name onto. Two implementations of "where is home" is exactly how a sandbox
// root and the paths inside it come to disagree.
int userhome_root(char *out, unsigned long cap);

// Build "<home>/CONFIG/<name>". Returns 0 on success, -1 if it will not fit.
// Fails rather than truncating: a truncated path is a different file.
int userconf_path(const char *name, char *out, unsigned long cap);

// Open a per-user preference for reading, falling back to `legacy` (the
// pre-#683 absolute path, e.g. "/CONFIG/THEME.CFG") when the per-user copy does
// not exist. Returns an fd, or -1.
int userconf_open_read(const char *name, const char *legacy);

// Open a per-user preference for writing (O_WRONLY|O_CREAT|O_TRUNC), creating
// <home>/CONFIG if needed. Never writes the legacy location. Returns an fd, -1.
int userconf_open_write(const char *name);


// --- #743: completing a write, so a failed save cannot look like a good one ---
//
// UC_MUST_CHECK mirrors kernel/types.h's MUST_CHECK. Userland does not include
// kernel headers, so the attribute is spelled out here rather than shared.
// NOTE, because this bit an earlier ticket: a `(void)f()` cast does NOT silence
// warn_unused_result on gcc 12.2 and never did. To ignore one of these results
// deliberately, assign it and say in a comment why the failure does not matter.
#ifndef UC_MUST_CHECK
#define UC_MUST_CHECK __attribute__((warn_unused_result))
#endif

// Write every byte of `buf` to `fd`, fsync it, then close it. ALWAYS closes fd,
// on every path, so the caller never leaks one and must never close it again.
// Returns 0 ONLY if the write, the fsync and the close ALL succeeded; -1 if any
// of them failed. Any non-zero return means the file on disk is NOT known to
// hold `buf`, so the caller must not report success to the user.
UC_MUST_CHECK int userconf_finish_write(int fd, const void *buf, unsigned long len);

// Replace the entire contents of `path` with `buf`. Returns 0 on success, -1 on
// failure. Does NOT unlink first: a refused open leaves the previous file
// intact, which is the whole point. See userconf.c for the limits (this is not
// an atomic replace, and cannot be until the kernel grows one).
UC_MUST_CHECK int userconf_write_all(const char *path, const void *buf, unsigned long len);

#endif
