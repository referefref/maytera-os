// userconf.h - #683: where a PER-USER preference lives (kernel side).
// Mirrors userland/libc/userconf.c. The rule and its justification are in
// rustkern/userconf.rs; this is only the passwd-table half.
#ifndef USERCONF_H
#define USERCONF_H
#include "../types.h"

// Resolve "<session user's home>/CONFIG/<name>" into out. Returns 0 on success,
// -1 if it will not fit (fails rather than truncating: a truncated path is a
// different file). For the root session, whose home is "/", this yields exactly
// "/CONFIG/<name>", i.e. the pre-#683 path, so a root session is unchanged.
int userconf_kpath(const char *name, char *out, uint32_t cap);

// [noaccount] The directory userconf_kpath() builds that path inside: this
// session's per-account STATE ROOT. The user-table home when the session uid
// has an entry (root's entry says "/", so a root session is unchanged), and
// "/TMP" when it has none at all, which is every machine between power-on and
// the end of the first-boot wizard. The decision itself is in
// rustkern/userconf.rs so that this side and userland/libc/userconf.c cannot
// answer it differently; see there for the measurement.
int userconf_kstate_root(char *out, uint32_t cap);

// Same join, but for an EXPLICIT home instead of the session user's. For a
// caller acting on behalf of a uid that is not the session user (the cron
// worker launching a job as its owner). "/" -> "/CONFIG/<name>",
// "/HOME/X" -> "/HOME/X/CONFIG/<name>". Returns 0, or -1 if it will not fit
// or home is empty/relative. userconf_kpath() is a caller of this.
int userconf_kpath_home(const char *home, const char *name, char *out, uint32_t cap);

// [noaccount] CORRECTED, AND NOW AN ALIAS OF userconf_kpath().
//
// This used to be a SEPARATE rule: "/TMP/<name>", flat, with the CONFIG
// component dropped, plus an explicit uid-0 early-out to undo a "home != /"
// test that answers wrongly for root. It existed because userconf_kpath()
// resolved a no-account session's home to "/", and /CONFIG under it is
// root-owned 0711: Ring 0 writes there happily (it bypasses perms_check()
// entirely) and no Ring-3 process can create a name there at all. That gap is
// not special to the notification spool. MEASURED on golden 2564, the kernel
// created /CONFIG/RECYCLE through userconf_kpath() on a machine whose own
// recycler daemon was then refused a plain READ of it.
//
// userconf_kpath() now resolves the session's STATE ROOT (userconf_kstate_root
// above), so "<root>/CONFIG/<name>" is correct for every session and there is
// one rule instead of two. Kept as a NAME for one release so its call sites do
// not have to be edited in the same change that moves the file they share.
int userconf_kpath_scratch(const char *name, char *out, uint32_t cap);

// #684 + headless-AI: copy the /CONFIG/KIMI.KEY deployment seed, once and
// never clobbering, into <home>/CONFIG/AISVC.CFG as "api_key=...", mode 0600
// owned by uid:gid. The kernel is the ONLY reader of the seed. `why` names the
// trigger ("login", "cron") in the log line.
//
// The caller vouches for the identity: uid/gid/home must come from a REAL
// user-table entry (or uid 0 with home "/"). An empty home is refused here
// (the #OOBEAUTH bootstrap session). Process context only (filesystem I/O).
// Returns 1 if it provisioned, 0 if nothing to do (no seed, already set,
// refused), -1 on a write failure.
int ai_provision_key_for(uint32_t uid, uint32_t gid, const char *home, const char *why);

#endif
