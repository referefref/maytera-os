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

#endif
