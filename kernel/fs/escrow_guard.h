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
// The owner's 2026-10-03 clarification split the two kinds of deletion, because
// one of them is recoverable and the other is not. rmdir is now its own op
// class so the guard does not have to probe the filesystem to tell them apart,
// and so a future reader cannot mistake "DELETE" for "covers directories too".
#define ESCROW_OP_RMDIR   4   // remove a DIRECTORY (never recycled: see below)

// Return values of escrow_fs_guard(). It used to be a two-valued allow/deny and
// every caller wrote `if (escrow_fs_guard(...) != 0) return -1;`. It now has a
// THIRD answer, and a caller that still tests `!= 0` would turn that answer
// into a refusal, which is why the op class above changed at the same time: a
// stale call site fails CLOSED (the delete is refused) rather than silently
// hard-deleting.
#define ESCROW_ALLOW       0   // proceed exactly as before
#define ESCROW_REDIRECT    1   // a marked actor's file delete: run your OWN
                               // permission checks first, then hand the path to
                               // escrow_delete_redirect() INSTEAD of unlinking

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
// RECYCLE-ONLY (2026-10-03). NOT a scoped contract: no grant, no authority, and
// writes are not scope-restricted. The ONE thing it changes is that a delete
// becomes a move into the user's Recycle Bin. The kernel applies it
// automatically at launch to the shipping AI programs
// (rustkern/escrowmark.rs escmark_is_ai_program_rs), so it is not something the
// AI opts into and not something it can opt out of, and it is inherited by
// everything such a process forks, clones, execs or spawns. See the long note
// on ACTIVE_RECYCLE in rustkern/escrowmark.rs for why this is a fourth state
// rather than a reuse of LOCKED.
#define ESCROW_ACTIVE_RECYCLE 3

// #246 pkgwrite: how a marked actor's FULL-FILE REPLACEMENT is dispositioned.
// MIRRORED from rustkern/escrowmark.rs REPL_*, which owns the rule. These are
// the values escmark_replace_disposition_rs() returns, and the three constants
// must stay numerically identical to the Rust side; they are scalars, so there
// is no struct to _Static_assert a sizeof on, and the boot self-test
// (escmark_policy_selftest_rs, 15 assertions for this rule) is what proves the
// Rust half behaves. A drift in these three numbers would show as the self-test
// still passing while this file acted on the wrong branch, so they are written
// here next to each other rather than scattered at the use sites.
#define ESCROW_REPL_SKIP      0   // nothing at risk: proceed, change nothing
#define ESCROW_REPL_SNAPSHOT  1   // COPY the current contents to the bin first
#define ESCROW_REPL_DENY      2   // no recoverable form possible: refuse

// #246 partialwrite: how a marked actor's PARTIAL OVERWRITE is dispositioned.
// MIRRORED from rustkern/escrowmark.rs OVW_*, which owns the rule, exactly as
// the ESCROW_REPL_* block above mirrors REPL_*. Scalars, so there is no struct
// to _Static_assert a sizeof on; escmark_policy_selftest_rs carries 19
// assertions for this rule and is what proves the Rust half behaves.
#define ESCROW_OVW_SKIP      0   // nothing that exists is at risk: proceed
#define ESCROW_OVW_SNAPSHOT  1   // COPY the pre-image to the bin, then proceed
#define ESCROW_OVW_DENY      2   // no recoverable form possible: refuse

// [usbescrow] #246: how a VICTIM LOOKUP is dispositioned, for the four routes
// that have to LOOK before they can decide. MIRRORED from rustkern/escrowmark.rs
// VIC_*, which owns the rule, exactly as the two blocks above mirror REPL_* and
// OVW_*. Scalars, so there is no struct to _Static_assert a sizeof on;
// escmark_policy_selftest_rs carries 11 assertions for this rule.
//
// The lookup itself is fs/recycle_fs.c recycle_victim_stat(), which is the ONE
// volume-aware "what is at this path" in the tree, sitting next to the ONE
// volume-aware "which filesystem serves this path" that the Recycle Bin capture
// already used. The four routes used to ask g_fat_fs directly, which cannot see
// a hot-plugged removable volume, so a file on a USB stick read as ABSENT and
// every one of them concluded "nothing to capture" and let the destruction
// through. See the rule's own comment in escrowmark.rs for the measurement.
#define ESCROW_VIC_ABSENT       0   // nothing there: proceed, keep nothing
#define ESCROW_VIC_PRESENT      1   // a file is there: the route's rule decides
#define ESCROW_VIC_UNREACHABLE  2   // nothing can read it: REFUSE, change nothing

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

// Perform a marked actor's delete as a RECOVERABLE one: capture the file into
// the session user's Recycle Bin, record the relationship in GraphFS, announce
// it on the desktop, and remove the original.
//
// THE CALL ORDER IS PART OF THE CONTRACT. escrow_fs_guard() must be consulted
// FIRST (it answers ESCROW_REDIRECT), the syscall's OWN permission checks must
// run SECOND, and this is called THIRD, in place of the unlink. Calling it
// before the permission check would let a marked actor relocate a file it has
// no right to remove, which is not a refusal but is still a change it should
// not be able to make.
//
// Returns 0 if the file is safely in the bin and the original is gone (the
// syscall reports SUCCESS to Ring 3: from the AI's point of view the delete
// worked), or -1 if it could not be captured, in which case NOTHING has been
// removed. Failing closed here is deliberate: if the kernel cannot promise the
// bytes are recoverable, it does not let them go.
//
// Blocking (filesystem I/O). Process context only.
int escrow_delete_redirect(const char *path);

// AN ftruncate() THAT SHRINKS A FILE DESTROYS THE BYTES PAST THE NEW LENGTH,
// and it is the LAST deterministic destruction path that is not a delete. It is
// separate from escrow_trunc_victim() because it happens on an ALREADY OPEN fd:
// the open may have carried no O_TRUNC at all, so the open-time check cannot
// see it coming.
//
// Call this from sys_ftruncate() BEFORE the shrink, with the path the fd refers
// to and the new length. A process with no mark, a grow (which this FS refuses
// anyway), a new length at or above the current one, or an empty file, are all
// no-ops returning 0. For a MARKED process shrinking a non-empty file the
// CURRENT on-disk contents are COPIED into the Recycle Bin (never moved: the fd
// stays open and the file must stay where it is).
//
// Returns 0 to proceed, -1 if the pre-image could not be kept, in which case
// the caller must refuse the ftruncate.
//
// Blocking (filesystem I/O). Process context only.
int escrow_shrink_victim(const char *path, uint64_t newlen);

// AN O_TRUNC OPEN OF AN EXISTING FILE DESTROYS EVERY BYTE IN IT, and it is not
// a delete at any layer, so the delete chokepoint never sees it either. This is
// the ONE sub-case of the overwrite family that is DETERMINISTIC AT OPEN TIME:
// the caller has said, in the flags, that the current contents are to go.
//
// Call this from the open path AFTER escrow_fs_guard() has allowed the open and
// BEFORE the open happens. For a process with no mark, a non-truncating open, or
// a file that does not exist or is already empty, it is a no-op returning 0.
// For a MARKED process truncating an existing non-empty file it keeps the
// CURRENT contents in the Recycle Bin first: a MOVE when O_CREAT is set (the
// file is about to be recreated anyway, so a move leaves no duplicate) and a
// COPY when it is not (the caller expects the file to still be there, and
// moving it would make the open fail with ENOENT).
//
// Returns 0 to proceed, -1 if the pre-image could not be made recoverable, in
// which case the caller must refuse the open.
//
// WHAT THIS DOES NOT COVER, and the line is worth knowing exactly. A plain
// O_WRONLY or O_RDWR open with no O_TRUNC, followed by writes that happen to
// land on existing bytes, is NOT captured. At open time there is no way to tell
// such an open from one that will only append or will write nothing, so
// capturing them all would mean snapshotting every writable open in the system
// by a marked process. That cost has not been measured, and guessing at it is
// how a feature becomes slow in a way nobody can attribute. O_TRUNC is the
// honest boundary: the flag IS the declaration of intent.
//
// NO DE-DUPLICATION. A file truncated five times produces five bin entries.
// That is the behaviour of a versioning store rather than a defect, the
// retention sweep clears them, and the alternative needs per-process state that
// process_t has no room for.
//
// Blocking (filesystem I/O). Process context only.
int escrow_trunc_victim(const char *path, int flags);

// A FULL-FILE REPLACEMENT IS A DELETE OF EVERY BYTE THAT WAS THERE.
//
// SYS_PKG_WRITE (301) takes a path, a buffer and a length and replaces the
// whole named file. It is not a delete, not a rename and not an open, so none
// of the other three chokepoints saw it, and a marked actor could use it to
// overwrite any file the invoking user may write while keeping NO pre-image.
// MEASURED before the fix: its authorization helper and its dispatcher case
// contained zero mentions of escrow.
//
// IT IS AS DETERMINISTIC AS THE O_TRUNC CASE, which is the whole argument for
// covering it. The coverage table's reason for leaving an ordinary overwrite
// uncaptured is that at open time an append cannot be told from an overwrite.
// That reason does not apply here: this call names ONE file and its COMPLETE
// new length up front, so at the moment of the check the kernel knows exactly
// which bytes are about to stop existing. The asymmetry was the gap.
//
// Call this from the SYS_PKG_WRITE case AFTER escrow_fs_guard() has allowed the
// write and AFTER the syscall's own permission check (pkg_write_permit), and
// BEFORE either writer runs. The order is the same contract the delete redirect
// states: guard first, the syscall's own permission checks second, this third,
// so a marked actor can never relocate a file it had no right to replace.
//
// For a process with no mark, a path that does not exist, a directory, or an
// already-empty file, it is a no-op returning 0. For a MARKED process replacing
// an existing non-empty file the CURRENT contents are COPIED into the Recycle
// Bin (never moved: a package write can still fail after this point, and today
// a failed SYS_PKG_WRITE leaves the target untouched, so moving the pre-image
// out would make the protection itself a new way to lose a file).
//
// Returns 0 to proceed, -1 if the pre-image could not be kept, in which case
// the caller must refuse the write.
//
// Blocking (filesystem I/O). Process context only.
int escrow_replace_victim(const char *path);

// ---------------------------------------------------------------------------
// #246 partialwrite: A PLAIN write() INTO BYTES THAT ALREADY EXIST.
//
// THE ROUTE THIS CLOSES, and why it was the last one open.
//
// Every other route in this family is named after its destruction: unlink,
// rmdir, rename, O_TRUNC, ftruncate, a full-file replacement. A write() is
// not. docs/AI_ESCROW_PROMISE.md listed this as the one remaining hole with a
// correct reason: at the moment a file is OPENED for writing, a writer that
// will overwrite cannot be told from one that will only append or write
// nothing, so covering it AT OPEN TIME would mean copying every file an AI
// opens for writing.
//
// The reason is about open time. By the time a write() arrives, the offset and
// the length are both in hand, so "will this destroy bytes that exist" has an
// exact answer, and it is `pos < orig_len`. That is what this function asks.
//
// WHO CALLS IT, and with what:
//
//   ext2 preserving descriptor (the ROOT volume, so /HOME and /CONFIG) -
//     proc/fdlayer.c sys_write_inner()'s `e->rw` branch passes `pre` =
//     the descriptor's whole-file buffer. That buffer was filled from the file
//     at open and, until the first write that lands inside the original extent,
//     still holds the PRISTINE bytes. So the pre-image costs NO READ AT ALL:
//     the open already paid for it. This is the cheap route and it is the one
//     that matters, because it is where the user's files live.
//
//   every other family (in-place FAT, /MNT aux, SMB/NFS) - `pre` is NULL and
//     the bytes are read back from the filesystem instead. See the honest
//     limits below for which of those are captured and which are refused.
//
// `orig_len` is the length the file had BEFORE this descriptor wrote anything,
// NOT its current length. A descriptor that has appended a megabyte still has
// only its original extent to protect; one whose pre-image has already been
// kept has none, which is what `already_kept` is for at the call site.
//
// RETURNS, and all three values matter to the caller:
//    1  a pre-image was KEPT. The caller must proceed AND must remember not to
//       ask again for this descriptor (one copy per descriptor, not per write).
//    0  nothing that exists is at risk (a non-actor, an append, a create, an
//       empty original, a zero-length write). Proceed, nothing was done.
//   -1  REFUSE the write and change nothing. Fail closed: the bytes could not
//       be made recoverable, or this family cannot be captured at all.
//
// Blocking (filesystem I/O plus a journal append) and therefore process
// context only, which every caller is: they are all on the write path of a
// syscall that already does fat/ext2 I/O. No loop, so concurrency-lint is
// unmoved. A non-actor returns at the first line having read one process_t
// byte, so the write path for every ordinary process is unchanged.
int escrow_overwrite_victim(const char *path, uint64_t pos, uint64_t count,
                            uint64_t orig_len, const void *pre,
                            uint32_t prelen);


// A RENAME IS A DELETE OF WHATEVER WAS AT THE DESTINATION. Both fat_rename()
// and ext2_rename() REPLACE an existing destination, so for a marked process
// `rename("a", "victim")` destroyed `victim` without ever going near the delete
// chokepoint. That is a hole in "a marked actor cannot destroy", and it is a
// particularly nasty one because it does not look like a delete at any layer
// above the filesystem.
//
// Call this from sys_rename() AFTER its own permission checks (including the
// destination-removal check, which is what decides whether this caller is
// allowed to displace the victim at all) and BEFORE the rename.
//
// For a process with no mark, or a destination that does not exist, it is a
// no-op returning 0 and the rename proceeds exactly as before. For a MARKED
// process displacing an existing file it captures that file into the Recycle
// Bin first, so the bytes survive. Returns 0 to proceed, -1 if the victim could
// not be made recoverable, in which case the caller must REFUSE the rename
// rather than destroy it.
//
// Blocking (filesystem I/O). Process context only.
int escrow_rename_victim(const char *newpath);

// Bind `p` into RECYCLE-ONLY mode (ESCROW_ACTIVE_RECYCLE), irrevocably.
//
// KERNEL-SIDE ONLY: there is no syscall that reaches this, deliberately. It is
// called from proc_create_user_as() when the program being launched is on the
// kernel's AI list, BEFORE the process runs its first instruction, so the
// restriction is not something the program can decline. It is not a
// permission, so nothing is escalated by applying it.
//
// It also mints an AI_TASK identity node in the GraphFS graph (an identity,
// with NO grant edge: identity without authority) so that every file the
// process recycles can be attributed to it in the tamper-evident journal and
// joined to the file's OBJECT node. That is what lets the Recycle Bin answer
// "which agent deleted this, and what else did it touch in that session".
//
// A no-op if `p` already carries any mark, so it can never downgrade a scoped
// contract to RECYCLE. Blocking (it appends a journal record): process context.
void escrow_recycle_bind(void *p, const char *program_path);

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
