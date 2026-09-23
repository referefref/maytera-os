// escrow_guard.c - #246/#305 AI escrow, KERNEL-ENFORCED slice (Stage 1).
//
// See escrow_guard.h for the model and the safety rule. This file is the FIRST
// real caller of gfs_grant_check() outside the fold's own boot self-test
// ([[zero-callers-and-differential-blindness]]: a security function with no
// callers never runs).
//
// WHY THIS IS C, NOT RUST. Every policy decision this file relies on is ALREADY
// in Rust and is merely called from here:
//   - the grant model, scope/expiry validity and the grant check itself live in
//     rustkern/gfsfold.rs (reached through the C glue in fs/graphfs/fold.c),
//   - the path-in-scope decision (boundary-aware, absolute-only, ".."-refusing)
//     is rustkern/elevate.rs elev_path_covered_rs(), reused verbatim.
// What remains here is entanglement glue that Rust cannot reach in this tree:
// the process_t PCB (C), sc_path_from_user() (a C static-inline over the C
// syscall path layer), and kprintf. This is the same stated justification
// fs/graphfs/fold.c is written under, not "the surrounding code is C".
//
// NO BUSY-WAIT, NO SPIN. The DENY/ALLOW decision in escrow_fs_guard() is
// memory-only and non-blocking (gfs_grant_check() takes the fold spinlock across
// a pure memory op and no I/O). The enter/exit paths call
// gfs_node_create/gfs_grant_issue/_revoke, which do journal I/O and therefore
// block, and are only ever reached from syscall context (scheduler live,
// proc_current != NULL, IF set), so they satisfy wq_assert_may_block().
//
// STAGE 3 TAMPER-EVIDENT AUDIT (docs/CONTRACT_ENFORCEMENT_PLAN.md). Once the
// verdict is decided, escrow_fs_guard() records the DECISION (every DENY, and
// each in-scope mutation ALLOW) into the SAME hash-chained GraphFS journal that
// grant issue/revoke use (fs/graphfs/journal.c gfs_journal_append -> the Rust
// gfsj_encode chain), via escrow_journal_decision() below. Three properties keep
// this safe on the FS-mutation hot path:
//   1. it fires ONLY for an escrow actor. A non-actor returns at the safety gate
//      before any journal code, having read one PCB byte, so ordinary FS ops are
//      byte-for-byte unaffected.
//   2. it is best-effort and NEVER changes the verdict. gfs_journal_append can
//      refuse (single-writer re-entrancy) or fail (I/O); either way the DENY
//      still denies and the ALLOW still allows, and the plaintext
//      /CONFIG/AIAUDIT.LOG remains the belt-and-braces record.
//   3. the append is called with NO lock held (gfs_grant_check already released
//      the fold spinlock) from the FS-mutation syscall path, which is inherently
//      a may-block context (it already does fat/ext2 I/O), so it is the same
//      safety class as the grant issue/revoke append and satisfies
//      wq_assert_may_block(). It contains no loop, so concurrency-lint is unmoved.
// A per-boot cap bounds how many records one actor can add to the shared journal,
// so a runaway or hostile actor cannot exhaust the (not-yet-rotated) audit log.

#include "escrow_guard.h"
#include "escrow_device.h"        // Stage 4: CAP_SCOPE_DEVICE (device identity + TOFU)
#include "escrow_undo.h"          // Stage 5B: rollback/undo engine (record + revert)
#include "../security/escrow_core.h"  // Stage 5A: immutable security core (principal source + tamper check)
#include "graphfs/fold.h"
#include "graphfs/journal.h"      // Stage 3: gfs_journal_append + GFSJ_OP_ESCROW_DECISION
#include "../proc/process.h"
#include "../proc/syscall_path.h"   // sc_path_from_user, SC_PATH_MAX
#include "../proc/elevate.h"        // elev_path_covered_rs (reused primitive)
#include "../string.h"
#include "../serial.h"
#include "fat.h"                 // g_fat_fs, fat_read_file (Stage 2 spawner)
#include "../mm/heap.h"          // kfree (Stage 2 spawner)

extern process_t *proc_current(void);
extern int elf_validate(const void *data, uint32_t size);   // Stage 2 spawner
extern fat_fs_t g_fat_fs;                                     // Stage 2 spawner

// A kernel-only monotonic counter for the per-contract GraphFS node locals.
//
// 0x100 is only the FRESH-BOOT floor: it starts above the reserved self-test
// locals (GFS_NODE_ST_OBJ uses local 1 and GFS_NODE_ST_OBJ_DEAD uses local 2,
// both OBJECT-kind). On a PERSISTED journal that is not enough: gfs_journal_init
// replays a prior boot's still-live (and retired) escrow AI_TASK/OBJECT nodes
// back into the fold, so on the 2nd+ boot a fresh contract at 0x101 collides
// with a replayed node -> gfs_node_create() returns GFSF_E_DUP_NODE and
// SYS_ESCROW_ENTER/escrow_spawn_marked return ESCROW_E_GRAPH. escrow_seed_local()
// (called once at boot from main.c, right after gfs_fold_init()) therefore SEEDS
// this counter ABOVE the fold's replayed max escrow local, so new contracts
// never reuse a replayed id. Wrap past 24 bits would be caught by
// gfs_node_create()'s DUP refusal (fail closed), but it takes 16M contracts to
// get there.
static uint32_t g_escrow_local = 0x100u;

static uint32_t escrow_next_local(void) {
    return __atomic_add_fetch(&g_escrow_local, 1u, __ATOMIC_SEQ_CST) & 0x00FFFFFFu;
}

// #246 cross-boot fix. Seed g_escrow_local ABOVE every escrow node id the
// GraphFS fold replayed from prior boots, so a fresh contract's AI_TASK/OBJECT
// node ids can never DUP with a still-present replayed node. Called ONCE at boot
// from main.c, right after gfs_fold_init() and BEFORE any process can enter
// escrow, so this plain seeded store races nothing (escrow_next_local() stays
// atomic for the live SMP allocation path).
//
// WHY THE FOLD MAX, NOT A SECOND PERSISTED COUNTER: the fold IS the persistent
// state (it is the replay of the journal), so its current max local for the
// escrow kinds is by construction >= every id an earlier boot could have taken,
// RETIRED ones included (a retired node still occupies its id; the fold refuses
// to re-create any id it already holds). A separate persisted counter would be a
// second source of truth that could drift from the journal.
//
// A contract creates BOTH task=(AI_TASK,local) and obj=(OBJECT,local) with the
// SAME local, so the seed must clear the max over BOTH kinds. We reuse the fold's
// own gfs_fold_max_local() accessor rather than hand-rolling a node-table walk.
void escrow_seed_local(void) {
    if (!gfs_fold_ready()) return;   // fold not up: keep the compile-time floor
    uint32_t mt = gfs_fold_max_local(GFS_KIND_AI_TASK);
    uint32_t mo = gfs_fold_max_local(GFS_KIND_OBJECT);
    uint32_t m  = (mt > mo) ? mt : mo;
    // Keep the >=0x100 fresh-boot floor; if replayed escrow nodes already reach
    // higher, seed to that max so the first escrow_next_local() (an atomic
    // add-fetch, +1) returns m+1, strictly above every replayed escrow local.
    uint32_t seed = (m >= 0x100u) ? m : 0x100u;
    __atomic_store_n(&g_escrow_local, seed, __ATOMIC_SEQ_CST);
    kprintf("[ESCROW] seeded local allocator to %#x (replayed max: AI_TASK=%#x "
            "OBJECT=%#x); new contracts start above prior-boot nodes\n",
            (unsigned)seed, (unsigned)mt, (unsigned)mo);
}

// Bounded deny logging so a pathological loop cannot flood the console, while
// the handful of denials a real proof run makes stay visible on serial.
static uint32_t g_deny_logged = 0;
#define ESCROW_DENY_LOG_CAP 64

// ---------------------------------------------------------------------------
// Stage 3: record an enforcement DECISION into the hash-chained journal.
// ---------------------------------------------------------------------------
// Verdicts and reasons carried in the record payload.
#define ESCROW_DEC_ALLOW        0
#define ESCROW_DEC_DENY         1
#define ESCROW_DEC_R_ALLOWED    0   // an in-scope mutation was permitted
#define ESCROW_DEC_R_NO_DELETE  1   // delete refused: no-delete invariant
#define ESCROW_DEC_R_NO_GRANT   2   // write refused: no live grant (revoked/expired/degraded)
#define ESCROW_DEC_R_SCOPE_SRC  3   // write refused: path out of granted scope
#define ESCROW_DEC_R_SCOPE_DST  4   // rename refused: destination out of granted scope
#define ESCROW_DEC_R_NO_EFFECT  6   // #246 Stage 6: a NON-FS effect (device eject)
                                    // refused: out of contract (no such grant is
                                    // ever issued for a governed effect this stage)
#define ESCROW_DEC_R_DEVICE     5   // write refused (Stage 4): target is not on the
                                    // device this grant is scoped to, or that
                                    // device's identity changed (BadUSB) -> fail closed

// How much of each path is carried verbatim in the record. Paths beyond this are
// truncated in the journal record (a truncation flag is set and the full path
// stays in /CONFIG/AIAUDIT.LOG); the record's payload_hash binds exactly what is
// stored. 256 covers essentially every real path while keeping the (noinline)
// helper's stack frame small, so the non-actor fast path in escrow_fs_guard()
// carries none of this buffer.
#define ESCROW_AUDIT_PATH_MAX   256

// Per-boot cap on escrow-decision records so one actor cannot exhaust the shared
// journal (which has no rotation until journal slice 4). Well under
// GFSJ_MAX_RECORDS (16384), leaving room for grant issue/revoke and boot records.
// Beyond the cap, decisions are still enforced and still logged to the plaintext
// audit file; only the journal record is skipped.
#define ESCROW_DECISION_JOURNAL_CAP  1024
static uint32_t g_escrow_journaled = 0;

// Payload schema of a GFSJ_OP_ESCROW_DECISION record. The record HEADER already
// carries the tamper-evident essentials: the monotonic timestamp (mono_ms) and
// boot generation, the actor identity (actor_id = escrow_task_node), the op class
// (GFSJ_OP_ESCROW_DECISION), the chain position (seq), the previous-record hash,
// and a SHA-256 over ALL of this payload (payload_hash). The payload adds the
// decision detail; its first 16 bytes are carried inline (readable back without
// the blob store) and the path(s) follow, bound by payload_hash even though they
// do not fit inline:
//   off  size  field
//   0    1     verdict    (ESCROW_DEC_ALLOW / ESCROW_DEC_DENY)
//   1    1     op_class   (ESCROW_OP_WRITE / ESCROW_OP_DELETE)
//   2    1     reason     (ESCROW_DEC_R_*)
//   3    1     flags      (bit0: a second path follows; bit1/bit2: path/path2 truncated)
//   4    4     pid        (u32 LE)
//   8    4     obj_node   (u32 LE)  the contract's OBJECT identity
//   12   2     scope      (u16 LE)  ESCROW_SCOPE_WRITE / _DELETE
//   14   2     path_len   (u16 LE)  bytes of path at off 16 (path2 fills the rest)
//   16   N     path       (canonical absolute path, not NUL-terminated)
//   16+N M     path2      (present iff flags bit0; a rename destination)
//
// noinline so its ~0.5KB path buffer lives in ITS frame, entered only by escrow
// actors, and never bloats escrow_fs_guard()'s frame on the non-actor fast path.
static __attribute__((noinline)) void escrow_journal_decision(
        process_t *p, uint8_t verdict, uint8_t op_class, uint8_t reason,
        const char *path, const char *path2) {
    // The journal not being up is a fail-safe no-op: the verdict is already
    // decided and returned by the caller; this is audit only.
    if (!gfs_journal_ready()) return;
    if (g_escrow_journaled >= ESCROW_DECISION_JOURNAL_CAP) {
        if (g_escrow_journaled == ESCROW_DECISION_JOURNAL_CAP) {
            g_escrow_journaled++;   // log the cap exactly once
            kprintf("[ESCROW] audit-journal cap (%u) reached this boot; further "
                    "escrow decisions are enforced and recorded in the plaintext "
                    "log, but not journalled (rotation is a later slice)\n",
                    (unsigned)ESCROW_DECISION_JOURNAL_CAP);
        }
        return;
    }

    unsigned char pl[16 + 2 * ESCROW_AUDIT_PATH_MAX];
    memset(pl, 0, 16);
    uint8_t flags = 0;

    unsigned long p1len = path ? strlen(path) : 0;
    if (p1len > ESCROW_AUDIT_PATH_MAX) { p1len = ESCROW_AUDIT_PATH_MAX; flags |= 0x02; }
    unsigned long p2len = 0;
    if (path2) {
        flags |= 0x01;
        p2len = strlen(path2);
        if (p2len > ESCROW_AUDIT_PATH_MAX) { p2len = ESCROW_AUDIT_PATH_MAX; flags |= 0x04; }
    }

    uint32_t pid   = p->pid;
    uint32_t obj   = p->escrow_obj_node;
    uint16_t scope = (op_class == ESCROW_OP_DELETE) ? ESCROW_SCOPE_DELETE
                   : (op_class == ESCROW_OP_EJECT)  ? ESCROW_SCOPE_EJECT
                                                    : ESCROW_SCOPE_WRITE;
    pl[0]  = verdict;
    pl[1]  = op_class;
    pl[2]  = reason;
    pl[3]  = flags;
    pl[4]  = (uint8_t)(pid & 0xFF);
    pl[5]  = (uint8_t)((pid >> 8) & 0xFF);
    pl[6]  = (uint8_t)((pid >> 16) & 0xFF);
    pl[7]  = (uint8_t)((pid >> 24) & 0xFF);
    pl[8]  = (uint8_t)(obj & 0xFF);
    pl[9]  = (uint8_t)((obj >> 8) & 0xFF);
    pl[10] = (uint8_t)((obj >> 16) & 0xFF);
    pl[11] = (uint8_t)((obj >> 24) & 0xFF);
    pl[12] = (uint8_t)(scope & 0xFF);
    pl[13] = (uint8_t)((scope >> 8) & 0xFF);
    pl[14] = (uint8_t)(p1len & 0xFF);
    pl[15] = (uint8_t)((p1len >> 8) & 0xFF);

    unsigned long off = 16;
    if (p1len) { memcpy(pl + off, path, p1len); off += p1len; }
    if (p2len) { memcpy(pl + off, path2, p2len); off += p2len; }

    // The actor identity IS the AI_TASK node (actor_kind NODE), exactly as grant
    // issue/revoke record it. Effect is NA: a decision record is a note, not a
    // graph mutation. Best-effort: a refused/failed append is a counted, loud
    // (journal.c) gap, never a verdict change.
    (void)gfs_journal_append(GFSJ_ACTOR_NODE, p->escrow_task_node,
                             GFSJ_OP_ESCROW_DECISION, GFSJ_EFFECT_NA,
                             pl, (uint32_t)off);
    g_escrow_journaled++;
}

// ---------------------------------------------------------------------------
// #246 Stage 6: the contract chokepoint generalised BEYOND the filesystem.
// A marked escrow actor is governed for MORE than files: a NON-FS effect (today
// a device eject) is checked here at the effect's syscall boundary, mirroring
// escrow_fs_guard. A non-actor returns "allow" after one process_t byte, so
// ordinary users of the effect (the Files/tray eject, the #708 AI safe-eject
// executor for an UNMARKED process) are byte-for-byte unaffected. For a marked
// actor the authority is the SAME GraphFS grant: no eject-scope grant is ever
// issued, so gfs_grant_check(...EJECT) is always !=1 and the effect is refused,
// by the ABSENCE of a grant, exactly like the no-delete invariant. The decision
// is recorded in the Stage 3 tamper-evident journal, like every FS decision.
// NO BUSY-WAIT, NO SPIN: gfs_grant_check() is a pure memory op under the fold
// spinlock, and the journal append (best-effort, never changes the verdict) is
// reached only for an actor from syscall context (a may-block context).
int escrow_effect_guard(int op, const char *detail) {
    process_t *p = proc_current();
    // THE SAFETY GATE: a process that is not an escrow actor is unaffected.
    if (!p || !p->escrow_active) return 0;

    // From here the caller IS a marked escrow actor. A contract-governed non-FS
    // effect is allowed only under a grant that permits it. For EJECT no such
    // grant is ever issued -> always refused (out-of-contract non-FS effect).
    if (op == ESCROW_OP_EJECT) {
        if (gfs_grant_check(p->escrow_task_node, p->escrow_obj_node,
                            ESCROW_SCOPE_EJECT) != 1) {
            if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
                g_deny_logged++;
                kprintf("[ESCROW] DENY eject pid=%u %s (no eject grant; "
                        "out-of-contract non-FS effect; kernel-enforced)\n",
                        (unsigned)p->pid, detail ? detail : "device.eject");
            }
            escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_EJECT,
                                    ESCROW_DEC_R_NO_EFFECT, detail, 0);
            return -1;
        }
        return 0;   // unreachable this stage: an eject grant is never minted
    }

    // Any other governed non-FS effect for a marked actor: fail closed.
    escrow_journal_decision(p, ESCROW_DEC_DENY, (uint8_t)op,
                            ESCROW_DEC_R_NO_EFFECT, detail, 0);
    return -1;
}

// ---------------------------------------------------------------------------
// The enforcement chokepoint.
// ---------------------------------------------------------------------------
int escrow_fs_guard(int op, const char *path, const char *path2) {
    process_t *p = proc_current();
    // THE SAFETY GATE: a process that is not an escrow actor is unaffected.
    if (!p || !p->escrow_active) return 0;

    // From here down the caller IS an escrow actor. The authority is the
    // GraphFS grant, never anything Ring 3 said. gfs_grant_check() fails closed
    // (0 unless a live, unrevoked, unexpired, in-scope grant exists) and denies
    // outright when the fold is degraded.
    if (op == ESCROW_OP_DELETE) {
        // No delete-scope grant is ever issued, so this is always 0: the
        // no-delete invariant, enforced by the substrate rather than a branch.
        if (gfs_grant_check(p->escrow_task_node, p->escrow_obj_node,
                            ESCROW_SCOPE_DELETE) != 1) {
            if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
                g_deny_logged++;
                kprintf("[ESCROW] DENY delete pid=%u path=%s "
                        "(no-delete invariant; kernel-enforced)\n",
                        (unsigned)p->pid, path ? path : "(null)");
            }
            escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_DELETE,
                                    ESCROW_DEC_R_NO_DELETE, path, 0);
            return -1;
        }
        return 0;   // unreachable in Stage 1: a delete grant is never minted
    }

    // A write / create / mkdir / rename. Require the live WRITE grant AND every
    // path the operation touches inside the granted scope.
    if (gfs_grant_check(p->escrow_task_node, p->escrow_obj_node,
                        ESCROW_SCOPE_WRITE) != 1) {
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY write pid=%u path=%s "
                    "(no live grant: revoked/expired/degraded)\n",
                    (unsigned)p->pid, path ? path : "(null)");
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_WRITE,
                                ESCROW_DEC_R_NO_GRANT, path, path2);
        return -1;
    }
    if (!path || !elev_path_covered_rs(path, p->escrow_scope_prefix)) {
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY write pid=%u path=%s out of scope %s "
                    "(kernel-enforced)\n",
                    (unsigned)p->pid, path ? path : "(null)",
                    p->escrow_scope_prefix);
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_WRITE,
                                ESCROW_DEC_R_SCOPE_SRC, path, path2);
        return -1;
    }
    if (path2 && !elev_path_covered_rs(path2, p->escrow_scope_prefix)) {
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY rename pid=%u dst=%s out of scope %s "
                    "(kernel-enforced)\n",
                    (unsigned)p->pid, path2, p->escrow_scope_prefix);
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_WRITE,
                                ESCROW_DEC_R_SCOPE_DST, path, path2);
        return -1;
    }
    // #246 Stage 4 CAP_SCOPE_DEVICE. If this contract is scoped to a validated
    // DEVICE IDENTITY (not just a path prefix), the write is allowed only if the
    // target still resides on THAT device and the device's identity has not
    // changed under the grant. escdev_check_write() re-derives the device's
    // fingerprint from the live enumerated descriptors on EVERY write, so a
    // remount at a different path still validates (same device, same
    // fingerprint) while a different device at the same path, a re-enumeration
    // with changed descriptors (BadUSB), or the device being gone all fail
    // closed. A contract that is NOT device-scoped returns 1 here immediately,
    // so Stage 1-3 behaviour is byte-for-byte unchanged.
    if (!escdev_check_write(p->escrow_task_node, path) ||
        (path2 && !escdev_check_write(p->escrow_task_node, path2))) {
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY write pid=%u path=%s not on the scoped device "
                    "(CAP_SCOPE_DEVICE: different device / identity changed / gone; "
                    "kernel-enforced, fail closed)\n",
                    (unsigned)p->pid, path ? path : "(null)");
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_WRITE,
                                ESCROW_DEC_R_DEVICE, path, path2);
        return -1;
    }

    // ALLOW: live grant + in scope + on the scoped device. Record the in-scope
    // mutation the actor was permitted, so the tamper-evident trail carries what
    // the AI task DID, not only what it was refused.
    // #246 Stage 5B/5C: the reversible effect is now recorded by the syscall
    // AFTER it SUCCEEDS (escrow_undo_note_rename / _mkdir), not here at the
    // pre-op ALLOW point, so the undo log holds CONFIRMED effects (and captures
    // MKDIR, which the guard cannot tell from a file create). The guard's job
    // stays purely the allow/deny decision.
    escrow_journal_decision(p, ESCROW_DEC_ALLOW, ESCROW_OP_WRITE,
                            ESCROW_DEC_R_ALLOWED, path, path2);
    return 0;
}

// ---------------------------------------------------------------------------
// #246 Stage 4 device binding. Bind a fresh contract's OBJECT to a validated
// device identity IF its scope path lives on a removable volume. Returns 0 on
// success (bound, OR the scope is on the root/non-removable fs so it is
// path-only exactly as before), or ESCROW_E_DEVMISMATCH if the scope's device
// has a KNOWN logical identity whose fingerprint has CHANGED since it was last
// trusted (TOFU MISMATCH) - in which case the caller must fail the enter closed.
// The AUTHORITY is unchanged: this only NARROWS a grant to a device; it can
// never widen one.
static int escrow_bind_device_scope(uint32_t task_node, const char *scope) {
    uint64_t fp = 0, logical = 0;
    int conf = 0;
    if (!escdev_fp_for_path(scope, &fp, &logical, &conf)) {
        // Scope is on the root / a non-removable fs: no device identity, so the
        // grant stays PATH-scoped (Stage 1-3). Nothing to bind.
        return 0;
    }
    int verdict = escdev_tofu_observe(logical, fp);
    if (verdict == ESCDEV_TOFU_MISMATCH) {
        kprintf("[ESCROW] DEVICE-SCOPE REFUSED scope=%s: this logical device was "
                "trusted with a DIFFERENT identity (TOFU MISMATCH, possible "
                "BadUSB/substitution); refusing to bind (fail closed)\n", scope);
        return ESCROW_E_DEVMISMATCH;
    }
    if (escdev_bind_set(task_node, fp) != 0) {
        // The binding table is full. Fail closed rather than silently downgrade
        // a device-scoped request to path-only.
        kprintf("[ESCROW] DEVICE-SCOPE REFUSED scope=%s: binding table full\n", scope);
        return ESCROW_E_DEVMISMATCH;
    }
    kprintf("[ESCROW] DEVICE-SCOPE bound task=%08x scope=%s fp=%016lx "
            "(confidence=%s%s; kernel re-validates on every write)\n",
            (unsigned)task_node, scope, (unsigned long)fp,
            conf == ESCDEV_CONF_SERIAL ? "unique-serial" :
            conf == ESCDEV_CONF_MODEL ? "MODEL/VID+PID+capacity, NOT unique" : "none",
            verdict == ESCDEV_TOFU_NEW ? ", trust-on-first-use" : ", previously trusted");
    return 0;
}

// ---------------------------------------------------------------------------
// SYS_ESCROW_ENTER: become an escrow actor under a fresh, scoped, TTL-bounded
// GraphFS WRITE grant. Fail closed: on ANY error no marker is left behind.
// ---------------------------------------------------------------------------
// #246 Stage 5C: record a CONFIRMED reversible effect for the current escrow
// actor, called by the FS-mutation syscalls AFTER the operation SUCCEEDS. A
// non-actor (or a process with no task node) is a no-op. Post-success fidelity
// replacing the Stage 5B pre-op recording, and it captures MKDIR live.
void escrow_undo_note_rename(const char *oldpath, const char *newpath) {
    process_t *p = proc_current();
    if (!p || !p->escrow_active || !p->escrow_task_node) return;
    escrow_undo_record(p->escrow_task_node, ESCU_OP_RENAME, oldpath, newpath);
}
void escrow_undo_note_mkdir(const char *path) {
    process_t *p = proc_current();
    if (!p || !p->escrow_active || !p->escrow_task_node) return;
    escrow_undo_record(p->escrow_task_node, ESCU_OP_MKDIR, path, 0);
}

int64_t sys_escrow_enter(const char *u_scope_path, uint64_t ttl_ms) {
    process_t *p = proc_current();
    if (!p) return ESCROW_E_ARG;
    if (p->escrow_active) return ESCROW_E_BUSY;   // one contract per process
    if (!gfs_fold_ready()) return ESCROW_E_NOTREADY;

    // Bounce + canonicalise the scope exactly like every FS syscall does, so
    // "." and ".." are already collapsed and the stored prefix is canonical.
    char kscope[SC_PATH_MAX];
    int prc = sc_path_from_user(u_scope_path, kscope, sizeof(kscope));
    if (prc != 0) return ESCROW_E_ARG;
    if (kscope[0] != '/') return ESCROW_E_ARG;
    unsigned long slen = strlen(kscope);
    if (slen == 0 || slen >= ESCROW_SCOPE_PREFIX_MAX) return ESCROW_E_ARG;

    uint32_t ttl = (ttl_ms == 0 || ttl_ms > (uint64_t)ESCROW_TTL_MAX_MS)
                       ? ESCROW_TTL_DEFAULT_MS : (uint32_t)ttl_ms;

    // Fresh graph identities for THIS contract (AI identity is per task,
    // docs/CONTRACT_ARCHITECTURE.md section 3).
    uint32_t local = escrow_next_local();
    uint32_t task = GFS_NODE_ID(GFS_KIND_AI_TASK, local);
    uint32_t obj  = GFS_NODE_ID(GFS_KIND_OBJECT, local);

    if (gfs_node_create(task, GFS_KIND_AI_TASK, 0) != 0) return ESCROW_E_GRAPH;
    if (gfs_node_create(obj, GFS_KIND_OBJECT, 0) != 0) {
        gfs_node_retire(task, 1);
        return ESCROW_E_GRAPH;
    }

    // The KERNEL is the escrow principal here: actor is hardcoded to
    // GFS_NODE_ESCROW (Ring 3 cannot spoof it; the fold refuses any other
    // issuer) and the scope is WRITE only. A delete grant is never minted.
    // #305 Stage 5A: the GRANT issuer is read from the IMMUTABLE CORE and
    // re-verified, so a tampered/patched principal fails closed (a wrong value is
    // refused by the fold; when the core is sealed a write to it faults first).
    uint32_t principal = escrow_core_principal();
    if (!escrow_core_verify() || principal == 0) {
        gfs_node_retire(task, 1);
        gfs_node_retire(obj, 1);
        return ESCROW_E_GRAPH;
    }
    uint64_t edge = 0;
    if (gfs_grant_issue(principal, task, obj, ESCROW_SCOPE_WRITE, ttl, &edge) != 0
        || edge == 0) {
        gfs_node_retire(task, 1);
        gfs_node_retire(obj, 1);
        return ESCROW_E_GRAPH;
    }

    // #246 Stage 4: bind the OBJECT to the device identity backing the scope, if
    // the scope is on a removable volume. On a TOFU mismatch (the device's
    // identity changed since it was last trusted) fail closed: tear down the
    // fresh grant + identities and leave no marker.
    int dvr = escrow_bind_device_scope(task, kscope);
    if (dvr != 0) {
        (void)gfs_grant_revoke(GFS_NODE_ESCROW, edge, 1);
        gfs_node_retire(task, 1);
        gfs_node_retire(obj, 1);
        return dvr;
    }

    // Commit to the PCB LAST, so any failure above leaves NO marker.
    p->escrow_task_node  = task;
    p->escrow_obj_node   = obj;
    p->escrow_grant_edge = edge;
    strncpy(p->escrow_scope_prefix, kscope, ESCROW_SCOPE_PREFIX_MAX - 1);
    p->escrow_scope_prefix[ESCROW_SCOPE_PREFIX_MAX - 1] = '\0';
    p->escrow_active = ESCROW_ACTIVE_SELF;   // Stage 1 self-service (may _EXIT)

    kprintf("[ESCROW] ENTER pid=%u scope=%s ttl=%ums task=%08x obj=%08x "
            "edge=%lu (kernel-enforced WRITE grant, no delete)\n",
            (unsigned)p->pid, p->escrow_scope_prefix, (unsigned)ttl,
            (unsigned)task, (unsigned)obj, (unsigned long)edge);
    return 0;
}

// ---------------------------------------------------------------------------
// SYS_ESCROW_EXIT: contract close. Revoke the grant (revocation bites in
// flight), retire the identities and clear the marker. Idempotent.
// ---------------------------------------------------------------------------
int64_t sys_escrow_exit(void) {
    process_t *p = proc_current();
    if (!p) return ESCROW_E_ARG;
    if (!p->escrow_active) return 0;

    // #246 Stage 2 NO SELF-EXIT. A MANDATORY escrow actor (spawned
    // already-marked by the trusted spawner, or inherited on fork) CANNOT free
    // ITSELF to escape enforcement. Only the trusted owner/kernel closes the
    // contract, or it expires by TTL. Stage 1 self-service actors
    // (ESCROW_ACTIVE_SELF) still exit here, so the Stage 1 ESCROWT test and the
    // userland advisory early-close are unaffected. This is the privilege
    // boundary that turns "enforced WHILE marked" into "enforced for the whole
    // life of the AI task": a Ring-3 uid-0 actor issuing SYS_ESCROW_EXIT to
    // unshackle itself is refused BY THE KERNEL and stays enforced.
    if (p->escrow_active == ESCROW_ACTIVE_LOCKED) {
        kprintf("[ESCROW] REFUSED self-exit pid=%u task=%08x (mandatory actor: "
                "only the trusted owner may close this contract)\n",
                (unsigned)p->pid, (unsigned)p->escrow_task_node);
        return ESCROW_E_LOCKED;
    }

    uint32_t task = p->escrow_task_node, obj = p->escrow_obj_node;
    uint64_t edge = p->escrow_grant_edge;

    // #246 Stage 5B2 TIMEOUT SEMANTICS. A clean EXIT means "promise fulfilled,
    // KEEP the effects" - but ONLY if the contract is still LIVE at close. If the
    // WRITE grant has already EXPIRED (its TTL elapsed) or been revoked by the
    // time the actor closes, this close is a timeout-WITHOUT-fulfilled-promise:
    // the kernel ROLLS BACK the contract's recorded reversible effects (moved
    // files returned to origin, empty created dirs removed) rather than leaving a
    // half-finished reorganisation behind. gfs_grant_check() returns 1 only for a
    // live, unexpired, in-scope grant, so it is precisely the "did this contract
    // complete inside its time box" test - the SAME authority escrow_fs_guard
    // uses on every write. Rollback is non-destructive (it never clobbers an
    // occupied slot or removes a non-empty dir) and runs in this may-block
    // syscall context, the same safety class as the grant-revoke journal write.
    int grant_live = (gfs_grant_check(task, obj, ESCROW_SCOPE_WRITE) == 1);

    // Clear the marker FIRST so the process is an ordinary process for the
    // duration of the (blocking) journal writes below and nothing can observe a
    // half-torn-down actor. Rollback keys on the captured task node, not the PCB
    // marker, so clearing first is safe.
    p->escrow_active = 0;
    p->escrow_task_node = 0;
    p->escrow_obj_node = 0;
    p->escrow_grant_edge = 0;
    p->escrow_scope_prefix[0] = '\0';

    escdev_bind_clear(task);   // Stage 4: drop the device binding on close
    uint32_t rb_reverted = 0, rb_skipped = 0;
    int timed_out = !grant_live;
    if (timed_out) {
        // Timeout / grant not live at close: RESTORE the pre-contract state.
        escrow_undo_result_t ur = escrow_undo_rollback(task);
        rb_reverted = ur.reverted;
        rb_skipped  = ur.skipped;
    } else {
        // Promise fulfilled inside the time box: KEEP effects, drop the log only.
        escrow_undo_clear(task);
    }
    if (edge) (void)gfs_grant_revoke(GFS_NODE_ESCROW, edge, 1);
    (void)gfs_node_retire(task, 1);
    (void)gfs_node_retire(obj, 1);

    kprintf("[ESCROW] EXIT pid=%u task=%08x obj=%08x edge=%lu (grant revoked, "
            "marker cleared; %s)\n",
            (unsigned)p->pid, (unsigned)task, (unsigned)obj, (unsigned long)edge,
            timed_out ? "TIMED OUT: effects rolled back" : "effects kept");
    if (timed_out)
        kprintf("[ESCROW] EXIT rollback (timeout-without-fulfilled-promise) "
                "reverted=%u skipped=%u\n", (unsigned)rb_reverted, (unsigned)rb_skipped);
    return 0;
}

// ---------------------------------------------------------------------------
// #246 Stage 5B AUTO-INVOKE: sys_escrow_abort. A self-service escrow actor that
// detects its promise FAILED aborts: the kernel REVERTS the contract's recorded
// reversible effects (moves back, empty dirs removed) via escrow_undo_rollback()
// THEN closes (revoke grant, clear marker). This is architecture section 7's "on
// failure, a task auto-reverts its reversible effects", wired to a real close.
// A MANDATORY (LOCKED) actor may NOT abort itself (the same escape-hatch reason
// as no-self-exit); only its trusted owner aborts it. Returns the number of
// effects reverted (>=0) on success, or ESCROW_E_LOCKED for a locked actor.
// ---------------------------------------------------------------------------
int64_t sys_escrow_abort(void) {
    process_t *p = proc_current();
    if (!p) return ESCROW_E_ARG;
    if (!p->escrow_active) return 0;
    if (p->escrow_active == ESCROW_ACTIVE_LOCKED) {
        kprintf("[ESCROW] REFUSED self-abort pid=%u task=%08x (mandatory actor: "
                "only the trusted owner may abort this contract)\n",
                (unsigned)p->pid, (unsigned)p->escrow_task_node);
        return ESCROW_E_LOCKED;
    }

    uint32_t task = p->escrow_task_node, obj = p->escrow_obj_node;
    uint64_t edge = p->escrow_grant_edge;

    // Revert the recorded reversible effects BEFORE the marker is torn down, so
    // the rollback runs against a still-consistent contract identity. This also
    // clears the undo log.
    escrow_undo_result_t ur = escrow_undo_rollback(task);

    // Close: same teardown as sys_escrow_exit (a SELF actor may free itself).
    p->escrow_active = 0;
    p->escrow_task_node = 0;
    p->escrow_obj_node = 0;
    p->escrow_grant_edge = 0;
    p->escrow_scope_prefix[0] = '\0';
    escdev_bind_clear(task);
    if (edge) (void)gfs_grant_revoke(GFS_NODE_ESCROW, edge, 1);
    (void)gfs_node_retire(task, 1);
    (void)gfs_node_retire(obj, 1);

    kprintf("[ESCROW] ABORT pid=%u task=%08x obj=%08x edge=%lu: rolled back "
            "reverted=%u skipped=%u (grant revoked, marker cleared)\n",
            (unsigned)p->pid, (unsigned)task, (unsigned)obj, (unsigned long)edge,
            (unsigned)ur.reverted, (unsigned)ur.skipped);
    return (int64_t)ur.reverted;
}

// ---------------------------------------------------------------------------
// #246 Stage 2 TRUSTED SPAWNER (see escrow_guard.h). KERNEL-SIDE only. Spawns an
// AI-actor task ALREADY MARKED so it is enforced from its first instruction and
// cannot self-exit. Reuses the Stage 1 grant machinery verbatim; the ONLY new
// mechanism is the per-cpu single-shot spawn-arm in proc/process.c, consumed by
// init_proc() before the child is enqueued.
// ---------------------------------------------------------------------------
extern bool  sched_set_preemption(bool enable);
// The per-cpu escrow spawn-arm lives in proc/process.c next to the tty bind,
// because init_proc() there is what consumes it, before the child is enqueued.
extern void  proc_escrow_arm(uint32_t task, uint32_t obj, uint64_t edge,
                             const char *scope);
extern void  proc_escrow_disarm(void);

int escrow_spawn_marked(const char *path, const char *scope,
                        uint32_t ttl_ms, uint32_t uid) {
    if (!path || !scope || scope[0] != '/') return ESCROW_E_ARG;
    unsigned long slen = strlen(scope);
    if (slen == 0 || slen >= ESCROW_SCOPE_PREFIX_MAX) return ESCROW_E_ARG;
    if (!gfs_fold_ready()) return ESCROW_E_NOTREADY;   // fail closed
    if (!g_fat_fs.mounted) return ESCROW_E_NOTREADY;

    uint32_t ttl = (ttl_ms == 0 || ttl_ms > (uint32_t)ESCROW_TTL_MAX_MS)
                       ? ESCROW_TTL_DEFAULT_MS : ttl_ms;

    // Read + validate the ELF with preemption ENABLED (blocking fs I/O).
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data || sz == 0) { if (data) kfree(data); return ESCROW_E_ARG; }
    if (elf_validate(data, sz) != 0) { kfree(data); return ESCROW_E_ARG; }

    // Mint the per-task identities + WRITE grant, preemption still ENABLED
    // (gfs_* do journal I/O). Same discipline as sys_escrow_enter(): actor is
    // hardcoded GFS_NODE_ESCROW (Ring 3 cannot spoof it; the fold refuses any
    // other issuer), scope is WRITE only, a DELETE grant is never minted.
    uint32_t local = escrow_next_local();
    uint32_t task = GFS_NODE_ID(GFS_KIND_AI_TASK, local);
    uint32_t obj  = GFS_NODE_ID(GFS_KIND_OBJECT, local);
    if (gfs_node_create(task, GFS_KIND_AI_TASK, 0) != 0) { kfree(data); return ESCROW_E_GRAPH; }
    if (gfs_node_create(obj, GFS_KIND_OBJECT, 0) != 0) {
        gfs_node_retire(task, 1); kfree(data); return ESCROW_E_GRAPH;
    }
    // #305 Stage 5A: issuer from the immutable core + re-verify (fail closed).
    uint32_t principal = escrow_core_principal();
    if (!escrow_core_verify() || principal == 0) {
        gfs_node_retire(task, 1); gfs_node_retire(obj, 1); kfree(data);
        return ESCROW_E_GRAPH;
    }
    uint64_t edge = 0;
    if (gfs_grant_issue(principal, task, obj, ESCROW_SCOPE_WRITE, ttl, &edge) != 0
        || edge == 0) {
        gfs_node_retire(task, 1); gfs_node_retire(obj, 1); kfree(data);
        return ESCROW_E_GRAPH;
    }

    // #246 Stage 4: bind the OBJECT to the device backing the scope (if the
    // scope is on a removable volume), before the task runs. Fail closed on a
    // TOFU mismatch: tear down the fresh grant + identities.
    int dvr = escrow_bind_device_scope(task, scope);
    if (dvr != 0) {
        (void)gfs_grant_revoke(GFS_NODE_ESCROW, edge, 1);
        gfs_node_retire(task, 1); gfs_node_retire(obj, 1); kfree(data);
        return dvr;
    }

    // Spawn ALREADY MARKED. Hold preemption OFF from the arm through the spawn
    // so (1) we cannot migrate off this cpu and (2) no other process creation on
    // this cpu interleaves. The arm is PER-CPU and consumed by init_proc()
    // BEFORE the child is enqueued, so the child NEVER runs unmarked. A
    // concurrent NORMAL spawn on any other cpu reads its own (unarmed) slot and
    // stays unmarked, so ordinary process creation is untouched.
    // proc_create_user_as() does no blocking I/O here (the ELF is already in
    // `data`), so holding preemption off across it is safe.
    bool old = sched_set_preemption(false);
    proc_escrow_arm(task, obj, edge, scope);
    int pid = proc_create_user_as(path, data, sz, 0, 0, proc_as_uid(uid));
    proc_escrow_disarm();   // idempotent no-op if init_proc already consumed it
    sched_set_preemption(old);

    kfree(data);

    if (pid < 0) {
        // Spawn failed and nothing inherited the contract: tear it down.
        escdev_bind_clear(task);   // Stage 4: drop any device binding
        escrow_undo_clear(task);   // Stage 5B: drop any undo log
        (void)gfs_grant_revoke(GFS_NODE_ESCROW, edge, 1);
        (void)gfs_node_retire(task, 1);
        (void)gfs_node_retire(obj, 1);
        kprintf("[ESCROW] SPAWN-MARKED FAILED path=%s (contract revoked)\n", path);
        return ESCROW_E_GRAPH;
    }

    kprintf("[ESCROW] SPAWN-MARKED pid=%d path=%s scope=%s ttl=%ums task=%08x "
            "obj=%08x edge=%lu (MANDATORY actor: marked from first instruction, "
            "cannot self-exit, children inherit)\n",
            pid, path, scope, (unsigned)ttl, (unsigned)task, (unsigned)obj,
            (unsigned long)edge);
    return pid;
}
