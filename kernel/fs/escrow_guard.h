// escrow_guard.h - #246/#305 AI escrow, KERNEL-ENFORCED slice (Stage 1 of
// docs/CONTRACT_ENFORCEMENT_PLAN.md).
//
// WHAT THIS ADDS. The escrow contract (#712) was ADVISORY: enforcement lived
// entirely in Ring-3 userland (userland/libc/escrow.c + aicap.c), so a Ring-3
// process running as uid 0 could bypass it by calling the raw filesystem
// syscalls directly. This slice moves the FS-mutation half of the enforcement
// into the kernel, at the syscall chokepoint (docs/CONTRACT_ARCHITECTURE.md
// section 6), reusing the GraphFS contract graph (fs/graphfs/fold.{c,h}) as the
// authority.
//
// THE ONE SAFETY RULE. The check applies ONLY to a process that has explicitly
// entered escrow-actor mode via SYS_ESCROW_ENTER. A process that has not is
// treated exactly as before: escrow_fs_guard() reads a single process_t byte
// and returns "allow". Normal filesystem operations by the shell, apps, the
// compositor, the installer and the kernel are therefore untouched.
//
// WHY IT CANNOT BE FORGED. The marker is the process_t.escrow_* fields, which
// Ring 3 has no syscall to write except SYS_ESCROW_ENTER (set) / SYS_ESCROW_EXIT
// (clear), the same unforgeability argument as the #745 elevation fields and the
// cap_grants array. And the AUTHORITY is not the marker: it is a GraphFS GRANT
// edge that ONLY the kernel escrow principal (GFS_NODE_ESCROW) can issue - a
// Ring-3 process cannot mint one, cannot widen its scope, and cannot obtain a
// delete-scope grant at all. So a marked actor issuing a raw out-of-scope write
// or ANY delete is refused BY THE KERNEL even with the userland layer bypassed.
#ifndef FS_ESCROW_GUARD_H
#define FS_ESCROW_GUARD_H

#include "../types.h"

// FS mutation op classes the guard distinguishes.
#define ESCROW_OP_WRITE   1   // create / write-open / mkdir / rename (a mutation)
#define ESCROW_OP_DELETE  2   // unlink / rmdir (a deletion)
// #246 Stage 6: the contract chokepoint generalised BEYOND the filesystem.
#define ESCROW_OP_EJECT   3   // a NON-FS effect: eject a device (device teardown)

// #246 Stage 2 (docs/CONTRACT_ENFORCEMENT_PLAN.md): the process_t.escrow_active
// byte now carries THREE states, so Stage 2 needs NO new PCB field (no struct
// size change, no asm/FFI offset shift). escrow_fs_guard() still branches only
// on truthiness, so BOTH active states are enforced IDENTICALLY; only contract
// CLOSE (sys_escrow_exit) distinguishes them. fork/clone copy the byte via the
// whole-struct memcpy, so a child of a LOCKED actor is itself LOCKED.
#define ESCROW_ACTIVE_NONE    0   // not an escrow actor (an ordinary process)
#define ESCROW_ACTIVE_SELF    1   // Stage 1 self-service: entered via
                                  // SYS_ESCROW_ENTER, MAY leave via _EXIT
#define ESCROW_ACTIVE_LOCKED  2   // Stage 2 MANDATORY: spawned already-marked by
                                  // the trusted spawner, or inherited on fork;
                                  // CANNOT clear its own mark

// GraphFS scopes (uint16, distinct from the fold self-test scopes 0x00FF/0x00FE
// and from the caps.rs capability classes). An escrow contract is issued WITH
// the WRITE scope and NEVER with the DELETE scope, so gfs_grant_check(...DELETE)
// is always 0 for an escrow actor: the no-delete invariant is enforced by the
// ABSENCE of a grant, not by a special-cased branch that could be forgotten.
#define ESCROW_SCOPE_WRITE   0x0200u
#define ESCROW_SCOPE_DELETE  0x0201u
// #246 Stage 6: an escrow contract is NEVER issued with the EJECT scope, so
// gfs_grant_check(...EJECT) is always 0 for an escrow actor: a device eject is
// an out-of-contract non-FS effect, refused by the ABSENCE of a grant exactly
// like the no-delete invariant. A future stage may mint an eject-scope grant to
// permit a specific device teardown; today the contract governs files.
#define ESCROW_SCOPE_EJECT   0x0202u

// TTL bounds. Milliseconds of THIS boot's uptime, because the GraphFS grant
// expiry is uptime-relative (there is no wall clock; see fold.c).
#define ESCROW_TTL_DEFAULT_MS  3600000u   // 1 hour, the owner's time box
#define ESCROW_TTL_MAX_MS      86400000u  // 24h hard cap

// SYS_ESCROW_ENTER / _EXIT refusal codes (negative, returned to Ring 3).
#define ESCROW_E_ARG      (-1)
#define ESCROW_E_BUSY     (-2)   // already an escrow actor (one contract/process)
#define ESCROW_E_NOTREADY (-3)   // the GraphFS fold is not up: fail closed
#define ESCROW_E_GRAPH    (-4)   // the graph refused to create identities/grant
#define ESCROW_E_LOCKED   (-5)   // Stage 2: a MANDATORY actor may not self-exit
#define ESCROW_E_DEVMISMATCH (-6) // Stage 4: the scope's device identity changed
                                 // since it was last trusted (TOFU MISMATCH);
                                 // refuse to bind a grant to it (fail closed)

// The enforcement chokepoint. Called from EVERY FS mutation syscall AFTER the
// path has been bounced+canonicalised (sc_path_from_user) and BEFORE the
// operation runs. Returns 0 to ALLOW, negative to DENY.
//
// For a process that is NOT an escrow actor it returns 0 immediately, having
// read nothing but process_t.escrow_active, so an ordinary process's FS path is
// byte-for-byte what it was before this file existed.
//
// `path` (and `path2`, used by rename) are canonical absolute kernel paths.
int escrow_fs_guard(int op, const char *path, const char *path2);

// #246 Stage 6: the SAME chokepoint pattern generalised to a NON-FS effect
// class. escrow_effect_guard() is called at the syscall boundary of a
// contract-governed effect that is not a filesystem mutation (today: device
// eject, ESCROW_OP_EJECT). For a non-actor it returns 0 (allow) after one
// process_t byte, so ordinary users of that effect are byte-for-byte unaffected.
// For a marked actor the effect is allowed only if the grant permits it; no
// eject-scope grant is ever issued, so a marked actor is refused, and the
// decision is recorded in the Stage 3 tamper-evident journal. Returns 0 to
// allow, -1 to refuse. `detail` is a short label for the audit/serial line.
int escrow_effect_guard(int op, const char *detail);

// #246 cross-boot fix: seed the per-contract local allocator above the escrow
// node ids the GraphFS fold replayed from prior boots. Call ONCE at boot from
// main.c, right after gfs_fold_init() and before any process enters escrow.
// Without it, g_escrow_local restarts at 0x100 every boot and a 2nd boot of a
// persisted journal collides (GFSF_E_DUP_NODE) -> ESCROW_E_GRAPH (-4).
void escrow_seed_local(void);

// The two syscalls (dispatched from proc/syscall.c).
int64_t sys_escrow_enter(const char *u_scope_path, uint64_t ttl_ms);
int64_t sys_escrow_exit(void);
int64_t sys_escrow_abort(void);   // Stage 5B: revert reversible effects then close

// #246 Stage 5C: the FS-mutation syscalls call these AFTER a successful mutation
// to record a CONFIRMED reversible effect for the current escrow actor (a no-op
// for a non-actor). This is the post-success fidelity for the rollback engine.
void escrow_undo_note_rename(const char *oldpath, const char *newpath);
void escrow_undo_note_mkdir(const char *path);

// #246 Stage 2 TRUSTED SPAWNER (docs/CONTRACT_ENFORCEMENT_PLAN.md). KERNEL-SIDE
// ONLY: there is NO Ring-3 syscall that reaches this, so Ring 3 cannot ask to be
// "spawned marked with scope X". It mints a fresh WRITE-only, TTL-bounded
// GraphFS grant under the kernel escrow principal (GFS_NODE_ESCROW, exactly like
// SYS_ESCROW_ENTER), then spawns the ELF at `path` ALREADY MARKED
// (escrow_active = ESCROW_ACTIVE_LOCKED) bound to `scope`, BEFORE the task runs
// its first instruction. Returns the new pid (> 0) or a negative ESCROW_E_*.
// `uid` is the identity the task runs as (0 for the strong case).
int escrow_spawn_marked(const char *path, const char *scope,
                        uint32_t ttl_ms, uint32_t uid);

#endif // FS_ESCROW_GUARD_H
