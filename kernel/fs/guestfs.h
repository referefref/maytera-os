// fs/guestfs.h - #708: the filesystem gate for DOS and Win16 GUEST programs.
//
// The policy lives in rustkern/guestfs.rs (new kernel logic, so Rust per the
// 2026-07-16 rule). This header is the C surface, plus a thin logging shim:
// kprintf is variadic and is deliberately kept out of the Rust FFI (the same
// call the #674 permpath.rs port made), so the DECISION is Rust and the
// REPORTING is C.
//
// Read the header comment of rustkern/guestfs.rs for the identity model. The
// short version: a guest runs inside a kernel thread and therefore has no
// credentials of its own, so its identity is captured AT LAUNCH from the
// launcher, carried in a per-layer slot, and every guest-reachable filesystem
// call site checks against it. An unarmed slot denies.
#ifndef FS_GUESTFS_H
#define FS_GUESTFS_H

#include "../types.h"

// One slot per guest layer. Each layer runs at most one guest at a time
// (g_dos_busy / g_win16_busy enforce it), and every gated call site names its
// slot as a compile-time constant, so there is no ambient "which guest am I"
// guess for the gate to get wrong.
#define GUESTFS_SLOT_DOS    0u
#define GUESTFS_SLOT_WIN16  1u

// ---- the Rust policy core (rustkern/guestfs.rs) --------------------------
int      guestfs_arm_rs(uint32_t slot, uint32_t kind, uint32_t want_uid);
void     guestfs_disarm_rs(uint32_t slot);
int      guestfs_cred_rs(uint32_t slot, uint32_t *out_uid, uint32_t *out_gid);
void     guestfs_stats_rs(uint32_t slot, uint32_t *out_checks, uint32_t *out_denies);
int      guestfs_check_rs(uint32_t slot, const char *path, int access);
uint32_t guestfs_selftest_rs(void);

// ---- the C surface every call site uses ----------------------------------

// THE GATE. May the guest in `slot` touch `native_path` with `access`
// (R_OK|W_OK|X_OK from fs/perms.h)? Returns 1 to ALLOW, 0 to DENY, and logs
// every denial once per (slot, reason, path) up to a boot-lifetime cap.
//
// `native_path` must be the POST drive-letter-mapping MayteraOS path, because
// that is the name perms.c and the filesystem both use. Gating the raw DOS
// string would check a different namespace from the one the access lands in.
// `what` names the entry point (e.g. "INT21/3Dh open") and appears in the log.
int guestfs_allow(uint32_t slot, const char *native_path, int access, const char *what);

// Arm a slot from the launcher's context, BEFORE the guest thread is created.
// Both return 0 on success and negative on refusal; on refusal the slot is
// left DISARMED, so a caller that ignores the return value still produces a
// guest with no filesystem access rather than a root one.
//
//   _caller():  the launch came from a Ring-3 process (the SYS_DOS_RUN /
//               SYS_WIN16_RUN syscalls). The guest runs as that process.
//   _session(): the launch came from a SERVICE with no Ring-3 caller (the
//               /CONFIG/DOSRUN.CFG and /CONFIG/WIN16PM.RUN boot harnesses).
//               The guest runs as the logged-in desktop session, and the arm
//               is REFUSED if no session has authenticated yet.
int  guestfs_arm_caller(uint32_t slot);
int  guestfs_arm_session(uint32_t slot);

// Disarm + print the one-line enforcement report for the run. Called at guest
// teardown. The report exists because a gate that never fired is
// indistinguishable from a gate that was never wired in, which is the
// recurring "the prose says it is enforced" trap in blame.md: it prints the
// identity the guest ran as and how many checks and denials there were.
void guestfs_finish(uint32_t slot);

// Boot self-test wrapper: runs guestfs_selftest_rs() and prints PASS/FAIL.
void guestfs_boot_selftest(void);

#endif // FS_GUESTFS_H
