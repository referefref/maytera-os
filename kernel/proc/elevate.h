// proc/elevate.h - #745 privilege elevation for SYSTEM-WIDE package installs.
//
// The POLICY and the state machine are in rustkern/elevate.rs (new kernel
// logic, so Rust per the 2026-07-16 rule). This header and elevate.c are the
// C glue that the rule does allow: syscall argument bouncing, process_t
// access, and the call into the existing users_authenticate* chokepoint. None
// of the decisions live here.
//
// THE TRUST STORY, in one paragraph, because it is the only reason any of this
// exists. An app asks for a system-wide install; it never draws anything, never
// receives a keystroke and never learns the password. The COMPOSITOR draws the
// prompt, and the compositor is a principal an app cannot impersonate:
// is_compositor() in gui/fb_syscall.c latches the FIRST process to map the
// framebuffer and rejects every other pid for the life of the boot. The kernel
// holds the request, derives the account to authenticate from the REQUESTER's
// own uid (so no username crosses any interface and account enumeration is
// impossible rather than merely unadvertised), and issues the resulting
// privilege as a bounded, path-scoped grant on the requester's process_t that
// Ring 3 has no syscall to write.
#ifndef PROC_ELEVATE_H
#define PROC_ELEVATE_H

#include "../types.h"

// States, mirroring rustkern/elevate.rs.
#define ELEV_ST_IDLE     0
#define ELEV_ST_OPEN     1
#define ELEV_ST_GRANTED  2
#define ELEV_ST_DENIED   3

// Refusals from SYS_ELEV_REQUEST. All negative, all distinguishable, because
// the App Store must say something different for each and "it failed" is the
// message this whole ticket exists to delete.
#define ELEV_EARG      (-1)   // malformed request
#define ELEV_EBUSY     (-2)   // a prompt is already open: REFUSED, never queued
#define ELEV_ESTALE    (-3)   // that seq is not the live request
#define ELEV_EPERM     (-4)   // this account is not in the admin set (Surface C)
#define ELEV_EROOT     (-5)   // caller is uid 0: root is NEVER prompted
#define ELEV_EATTEMPTS (-6)   // three wrong passwords; request closed
#define ELEV_ELOCKED   (-7)   // the account's ELEVATION lockout is running
#define ELEV_ENOINPUT  (-8)   // no recent dispatched input: a spontaneous prompt

// SYS_ELEV_RESOLVE actions (compositor only).
#define ELEV_ACT_CANCEL   0
#define ELEV_ACT_SUBMIT   1
#define ELEV_ACT_LOCKSECS 2

// The SYSTEM-WIDE install destination, and the ONLY prefix a grant is ever
// issued for. It is a kernel constant on purpose: the requesting app supplies
// the package name, version and source (displayed as data, sanitised), and
// supplies NOTHING that decides where the privilege applies.
#define ELEV_SYS_APPS_DIR "/APPS"
// The SYSTEM Start-menu layer (userland/libc/startmenu_reg.c). Granted so a
// system-wide install can finish; deliberately the LEAF directory and not
// /CONFIG, and elev_path_covered_rs() refuses any "." or ".." element, so this
// cannot be widened into /CONFIG/SHADOW by a string.
#define ELEV_SYS_MENU_DIR "/CONFIG/STARTMENU/SYSTEM.D"

// The password buffer this mechanism bounces into. SC_PASSWORD_MAX is a macro
// private to proc/syscall.c, so this is a THIRD copy of the same number (the
// second is s(128) in rustkern/argtab.rs, which decides how many bytes the
// argument validator proves readable). A duplicated bound that drifts does not
// fail loudly, it silently truncates or under-validates a credential, so
// proc/syscall.c carries a _Static_assert that locks this one to SC_PASSWORD_MAX
// where both are in scope.
#define ELEV_PASSWORD_MAX 128

// What the requesting app sends. Every field is app-supplied DISPLAY text and
// is sanitised in Rust on the way in (control bytes and non-ASCII dropped,
// truncated to 40 glyphs). There is deliberately no destination field: see
// ELEV_SYS_APPS_DIR.
typedef struct {
    char name[64];
    char version[32];
    char source[64];
} elev_request_t;

// What the compositor is given. Mirrors ElevView in rustkern/elevate.rs.
typedef struct {
    uint64_t seq;
    uint64_t opened_ms;
    uint32_t state;
    uint32_t req_pid;
    uint32_t req_uid;
    uint32_t attempts_used;
    uint32_t attempts_max;
    uint32_t pad;
    char     name[64];
    char     version[32];
    char     source[64];
    char     dest[96];
} elev_view_t;

// Rust FFI (rustkern/elevate.rs).
int64_t elev_open_rs(uint32_t pid, uint32_t uid, uint64_t now_ms,
                     const char *name, const char *version,
                     const char *source, const char *dest);
int      elev_view_rs(elev_view_t *out);
int      elev_state_rs(uint64_t seq);
uint32_t elev_owner_pid_rs(void);
uint32_t elev_owner_uid_rs(void);
int      elev_attempt_rs(uint64_t seq, uint32_t ok);
int      elev_cancel_rs(uint64_t seq);
int      elev_reap_rs(uint64_t seq);
int      elev_tick_rs(uint64_t now_ms, uint32_t requester_alive);
int      elev_path_covered_rs(const char *path, const char *prefix);
uint32_t elevate_selftest_rs(void);

// C glue (proc/elevate.c). These are the syscall bodies; the dispatcher cases
// in syscall.c are one line each.
int64_t sys_elev_request(const elev_request_t *u_req);
int64_t sys_elev_status(uint64_t seq);
int64_t sys_elev_view(elev_view_t *u_out);
// `k_pw` is a KERNEL buffer, already bounced out of Ring 3 by the dispatcher
// case (sc_bounce_str is static to proc/syscall.c). The ARRAY type states that.
int64_t sys_elev_resolve(uint64_t seq, int action, const char k_pw[ELEV_PASSWORD_MAX]);
int64_t sys_elev_may(void);

// Enforcement hook, called from fs/perms.c. Returns 1 only when the CURRENT
// process holds a live grant, the uid being checked is that process's own, and
// `path` is a plain absolute path strictly under the granted prefix.
int elev_grant_permits(const char *path, uint32_t proc_uid);

// Boot self-test driver: runs elevate_selftest_rs() and logs the result.
void elevate_selftest(void);

#endif // PROC_ELEVATE_H
