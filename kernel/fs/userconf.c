// userconf.c - #683 kernel side: resolve a per-user preference path.
//
// Ring 0 bypasses perms_check() entirely, so the kernel is never DENIED one of
// these files. It still has to agree with userland about WHERE they are, or the
// relocation produces a split brain: the notification spool is the live example,
// posted to by Ring-0 seclog.c and by every app through libc/notify.c, and
// drained by the compositor. If those disagreed, security events would silently
// stop being shown while every individual component looked correct.
//
// C, not Rust, for the same reason as perms_check_leaf(): the only thing here is
// a lookup in the existing C user_entry_t table. The part with an actual failure
// mode, the bounded string join, IS in Rust (rustkern/userconf.rs).
#include "userconf.h"
#include "../proc/users.h"
#include "../gui/desktop.h"

extern int userconf_join_rs(const char *home, const char *name,
                            char *out, uint32_t cap);

int userconf_kpath(const char *name, char *out, uint32_t cap) {
    uint32_t uid = desktop_get_session_uid();
    user_entry_t *u = user_lookup_uid(uid);
    const char *home = (u && u->home[0]) ? u->home : "/";
    return userconf_join_rs(home, name, out, cap) < 0 ? -1 : 0;
}
