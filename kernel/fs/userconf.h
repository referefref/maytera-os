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

// Same join, but for an EXPLICIT home instead of the session user's. For a
// caller acting on behalf of a uid that is not the session user (the cron
// worker launching a job as its owner). "/" -> "/CONFIG/<name>",
// "/HOME/X" -> "/HOME/X/CONFIG/<name>". Returns 0, or -1 if it will not fit
// or home is empty/relative. userconf_kpath() is a caller of this.
int userconf_kpath_home(const char *home, const char *name, char *out, uint32_t cap);

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
