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
//      the per-user AIAUDIT.LOG (#aiaudit) remains the belt-and-braces record.
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
#include "recycle_fs.h"        // the Recycle Bin: an AI delete is a REDIRECT, not a refusal
#include "../proc/process.h"
#include "../proc/syscall_path.h"   // sc_path_from_user, SC_PATH_MAX
#include "../proc/elevate.h"        // elev_path_covered_rs (reused primitive)
#include "../string.h"
#include "../serial.h"
#include "fat.h"                 // g_fat_fs, fat_read_file (Stage 2 spawner)
#include "../mm/heap.h"          // kfree (Stage 2 spawner)

extern process_t *proc_current(void);
extern int elf_validate(const void *data, uint32_t size);   // Stage 2 spawner
extern uint32_t escmark_delete_disposition_rs(uint8_t active, uint32_t is_dir,
                                              uint32_t recoverable);
extern uint32_t escmark_may_self_exit_rs(uint8_t active);
extern uint32_t escmark_write_scoped_rs(uint8_t active);
// #246 pkgwrite: the FULL-FILE REPLACEMENT disposition (SYS_PKG_WRITE). New
// policy, so Rust per the 2026-07-16 rule; see rustkern/escrowmark.rs for the
// rule and escmark_policy_selftest_rs for the 15 assertions that cover it.
extern uint32_t escmark_replace_disposition_rs(uint8_t active, uint32_t exists,
                                               uint32_t is_dir, uint32_t cur_len,
                                               uint32_t recoverable);
// #246 partialwrite: the PARTIAL-OVERWRITE disposition (a plain write() that
// lands on bytes that already exist). New policy, so Rust per the 2026-07-16
// rule; see rustkern/escrowmark.rs for the rule and escmark_policy_selftest_rs
// for the 19 assertions that cover it.
extern uint32_t escmark_overwrite_disposition_rs(uint8_t active, uint64_t orig_len,
                                                 uint64_t pos, uint64_t count,
                                                 uint32_t already_kept,
                                                 uint32_t recoverable);
extern uint32_t escmark_is_ai_program_rs(const unsigned char *path);
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
#define ESCROW_DEC_RECYCLED     2   // a delete was REDIRECTED into the Recycle Bin
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

#define ESCROW_DEC_R_RECYCLED   7   // delete REDIRECTED: the file is in the user's
                                    // Recycle Bin, restorable by the user or by
                                    // the AI, and the user was notified
#define ESCROW_DEC_R_NO_BIN     8   // delete REFUSED: no usable Recycle Bin, so
                                    // the bytes could not be made recoverable
#define ESCROW_DEC_R_NO_RMDIR   9   // rmdir REFUSED: a directory has no
                                    // recoverable form in a flat bin
// How much of each path is carried verbatim in the record. Paths beyond this are
// truncated in the journal record (a truncation flag is set and the full path
// stays in the per-user AIAUDIT.LOG); the record's payload_hash binds exactly what is
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
    // --- REMOVING A DIRECTORY -------------------------------------------
    // The NAMED EXCEPTION. The Recycle Bin is a flat store of files, one index
    // row each; it has no representation for a directory tree, so there is no
    // recoverable form to redirect an rmdir into, and the honest answer is to
    // refuse. In practice this costs an actor very little: ext2's rmdir already
    // refuses a non-empty directory, so the only thing refused here is removing
    // an EMPTY one.
    if (op == ESCROW_OP_RMDIR) {
        // RECYCLE mode is permitted to remove an EMPTY directory: ext2's rmdir
        // refuses a non-empty one, so nothing recoverable is at stake, and
        // refusing would break ordinary tidying the user asked for. A SCOPED
        // contract is still refused. The reasoning lives in
        // rustkern/escrowmark.rs beside the rule.
        if (escmark_delete_disposition_rs(p->escrow_active, 1 /* is_dir */,
                                          0 /* recoverable: n/a */) == 2 /* DISP_ALLOW */)
            return ESCROW_ALLOW;
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY rmdir pid=%u path=%s (a directory has no "
                    "recoverable form in the Recycle Bin; kernel-enforced)\n",
                    (unsigned)p->pid, path ? path : "(null)");
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, (uint8_t)ESCROW_OP_RMDIR,
                                ESCROW_DEC_R_NO_RMDIR, path, 0);
        return -1;
    }

    // --- DELETING A FILE --------------------------------------------------
    //
    // WHAT CHANGED HERE, 2026-10-03, and why it is not a weakening.
    //
    // This branch used to REFUSE outright: no DELETE-scope grant is ever
    // minted, so gfs_grant_check(...DELETE) is always 0 and every delete by a
    // marked actor failed. That satisfied "the AI cannot destroy your files"
    // by also making it unable to delete them, which the owner has since
    // clarified is not what was wanted: "AI can delete files but it must be
    // announced to the user AND they can be undeleted by AI or the user".
    //
    // So the delete now SUCCEEDS from Ring 3's point of view and the bytes are
    // redirected into the user's Recycle Bin. The binding property is no longer
    // "an actor cannot delete" but the stronger and more useful "an actor
    // cannot DESTROY": there is no path from a marked process to an
    // unrecoverable unlink, because this chokepoint is on every delete syscall
    // and the capture happens in Ring 0, not in the AI tool layer that a raw
    // syscall bypasses.
    //
    // THE ABSENCE OF A DELETE GRANT IS STILL THE SUBSTRATE-LEVEL INVARIANT. It
    // is checked first and still never fires. If some future stage DOES mint a
    // delete-scope grant, that contract gets a real hard delete, deliberately
    // and visibly, and everything else keeps the redirect.
    if (op == ESCROW_OP_DELETE) {
        // A RECYCLE-mode process holds NO grant at all, so asking the graph
        // whether it has a delete-scope grant is meaningless; the disposition
        // below is the whole rule for it.
        if (p->escrow_active != ESCROW_ACTIVE_RECYCLE &&
            gfs_grant_check(p->escrow_task_node, p->escrow_obj_node,
                            ESCROW_SCOPE_DELETE) == 1) {
            return ESCROW_ALLOW;   // never, in any shipping stage
        }

        uint32_t disp = escmark_delete_disposition_rs(
                            p->escrow_active, 0 /* a file, not a directory */,
                            (uint32_t)recycle_available());
        if (disp == 1 /* DISP_RECYCLE */) {
            // The DECISION is recorded here, beside every other enforcement
            // decision, so one file holds what the guard did. The capture
            // itself happens in escrow_delete_redirect(), called by the
            // syscall AFTER its own permission checks: a marked actor must not
            // be able to relocate a file it has no right to remove.
            escrow_journal_decision(p, ESCROW_DEC_RECYCLED, ESCROW_OP_DELETE,
                                    ESCROW_DEC_R_RECYCLED, path, 0);
            return ESCROW_REDIRECT;
        }

        // No usable bin. FAIL CLOSED: refuse rather than destroy. The user sees
        // the AI report that it could not delete the file, which is far better
        // than it reporting success over bytes that are gone.
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY delete pid=%u path=%s (no usable Recycle "
                    "Bin: the bytes could not be made recoverable, so the "
                    "delete is refused; kernel-enforced, fail closed)\n",
                    (unsigned)p->pid, path ? path : "(null)");
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_DELETE,
                                ESCROW_DEC_R_NO_BIN, path, 0);
        return -1;
    }

    // A write / create / mkdir / rename.
    //
    // RECYCLE mode stops here. It is NOT a scoped contract: it carries no grant
    // and no path scope, because the general AI assistant is asked to write
    // wherever the user wants. The ONE thing RECYCLE changes is what a DELETE
    // becomes, handled above. Its writes are governed by ordinary filesystem
    // permissions, exactly like any other process, which the syscall checks for
    // itself further down. The rule has one home
    // (rustkern/escrowmark.rs escmark_write_scoped_rs) so that a future fifth
    // mark state cannot be added to one branch and forgotten in the other.
    if (!escmark_write_scoped_rs(p->escrow_active)) return ESCROW_ALLOW;

    // A SCOPED CONTRACT. Require the live WRITE grant AND every path the
    // operation touches inside the granted scope.
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
// ONE PLACE WHERE A MARKED ACTOR'S PRE-IMAGE IS KEPT.
//
// Four routes destroy the bytes at a path without ever looking like a delete:
// an O_TRUNC open, an ftruncate shrink, a rename over an existing file, and
// (added 2026-10-03) a full-file replacement via SYS_PKG_WRITE, and a PARTIAL
// OVERWRITE by a plain write() into existing bytes (#246 partialwrite, the
// FIFTH route and the reason this helper's `pre` parameter exists). Before this
// helper existed, three of them each carried their OWN copy of the same five
// blocks: the no-bin refusal, the lazy AI_TASK identity mint, the capture call,
// the success journal line and the failure journal line. The fourth route was
// about to become a fourth copy, which is exactly the hand-rolled-duplicate
// failure mode CLAUDE.md's reuse rule exists to stop, so the five blocks moved
// here instead and the routes got SHORTER rather than longer.
//
// WHAT STAYS WITH EACH CALLER, deliberately: the decision about whether this
// path is at risk at all. Those decisions genuinely differ (an already-empty
// file matters to one route and not another; a directory destination is a
// refusal for rename and a no-op for the rest), and collapsing them into flags
// on this function would have turned four readable policies into one function
// with five booleans. The route's own policy is the route's own business; what
// is shared is the MECHANISM for keeping the bytes.
//
// `move_ok` chooses between a MOVE (recycle_capture: the file is going away
// anyway, so a move leaves no duplicate) and a COPY (recycle_snapshot: the
// caller expects the file to still be there afterwards, or the operation can
// still fail). `route` is a short label that goes on the serial line and makes
// an audit line say which of the four routes produced it.
//
// Returns 0 if the bytes are safely recoverable (the caller proceeds), -1 if
// they are not (the caller must REFUSE and change nothing).
//
// Blocking (filesystem I/O + a journal append). Process context only, which
// every one of the four callers is: they are all on an FS-mutation syscall
// path that already does fat/ext2 I/O. No loop, so concurrency-lint is unmoved.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// [usbescrow] #246: THE VICTIM LOOKUP THE FOUR LOOK-FIRST ROUTES SHARE.
//
// escrow_shrink_victim, escrow_trunc_victim, escrow_rename_victim and
// escrow_replace_victim all have to answer "is there a file at this path, how
// big is it, and is it a directory" before their own rule can say anything.
// All four asked `fat_exists(&g_fat_fs, path)` and `fat_read_file(&g_fat_fs,
// path, &sz)` directly, and g_fat_fs does NOT route a hot-plugged removable
// volume, so on a plugged-in USB stick all four read ABSENT and concluded
// "nothing to capture". MEASURED on a booted VM: four destructions on a
// synthetic stick, four files' bytes gone, not one [ESCROW] line printed.
//
// This replaces the three hand-rolled questions with the ONE volume-aware
// lookup (fs/recycle_fs.c recycle_victim_stat, built on the same rcy_victim_fs
// the Recycle Bin capture itself uses) and the ONE Rust rule for what its
// result means (rustkern/escrowmark.rs escmark_victim_disposition_rs).
//
// Returns ESCROW_VIC_*. On UNREACHABLE it has ALREADY logged and journalled the
// refusal, so the caller's whole job is `return -1`.
// ---------------------------------------------------------------------------
static uint32_t escrow_victim_look(process_t *p, const char *path,
                                   const char *route, uint32_t *len_out,
                                   int *isdir_out) {
    int st = recycle_victim_stat(path, len_out, isdir_out);
    extern uint32_t escmark_victim_disposition_rs(uint8_t active, int32_t stat_rc);
    uint32_t disp = escmark_victim_disposition_rs(p->escrow_active, (int32_t)st);
    if (disp == ESCROW_VIC_UNREACHABLE) {
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY %s pid=%u path=%s (this path is on a mounted "
                    "removable volume the kernel cannot read files from, so the "
                    "pre-image could not be kept; NOTHING was changed; "
                    "kernel-enforced, fail closed)\n",
                    route, (unsigned)p->pid, path);
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_DELETE,
                                ESCROW_DEC_R_NO_BIN, path, 0);
    }
    return disp;
}

static int escrow_preimage_keep(process_t *p, const char *path, int move_ok,
                                const char *route, const void *pre,
                                uint32_t prelen) {
    if (!recycle_available()) {
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY %s pid=%u path=%s (no usable Recycle Bin, so "
                    "the pre-image could not be kept; kernel-enforced, fail "
                    "closed)\n", route, (unsigned)p->pid, path);
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_DELETE,
                                ESCROW_DEC_R_NO_BIN, path, 0);
        return -1;
    }

    // Mint the AI_TASK IDENTITY on first use, exactly as escrow_delete_redirect
    // does, and for the same reason: escrow_recycle_bind() runs under
    // preemption-disable and could not do journal I/O there.
    if (p->escrow_task_node == 0 && p->escrow_active == ESCROW_ACTIVE_RECYCLE &&
        gfs_fold_ready()) {
        uint32_t cand = GFS_NODE_ID(GFS_KIND_AI_TASK, escrow_next_local());
        if (gfs_node_create(cand, GFS_KIND_AI_TASK, GFS_NODE_ESCROW) == 0)
            p->escrow_task_node = cand;
    }

    // #246 partialwrite: a caller that ALREADY HOLDS the pristine bytes hands
    // them over instead of making this read them back. The ext2 preserving
    // descriptor is that caller: its open filled a whole-file buffer from the
    // file, so the pre-image is already in kernel memory and a re-read would
    // be a second read of the same bytes. `pre` NULL keeps the original
    // behaviour for the four routes that do not have them.
    int rc = pre ? recycle_snapshot_bytes(path, pre, prelen, RCY_ACTOR_AI,
                                          p->escrow_task_node)
                 : (move_ok ? recycle_capture(path, RCY_ACTOR_AI, p->escrow_task_node)
                            : recycle_snapshot(path, RCY_ACTOR_AI, p->escrow_task_node));
    if (rc == RCY_OK) {
        escrow_journal_decision(p, ESCROW_DEC_RECYCLED, ESCROW_OP_DELETE,
                                ESCROW_DEC_R_RECYCLED, path, 0);
        kprintf("[ESCROW] PRE-IMAGE kept pid=%u route=%s path=%s (%s into the "
                "Recycle Bin; the bytes survive and can be restored)\n",
                (unsigned)p->pid, route, path, move_ok ? "MOVED" : "COPIED");
        return 0;
    }

    if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
        g_deny_logged++;
        kprintf("[ESCROW] DENY %s pid=%u path=%s (could not keep the pre-image: "
                "%s [rc=%d]; NOTHING was changed; kernel-enforced, fail "
                "closed)\n", route, (unsigned)p->pid, path,
                recycle_strresult(rc), rc);
    }
    escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_DELETE,
                            ESCROW_DEC_R_NO_BIN, path, 0);
    return -1;
}

// ---------------------------------------------------------------------------
// An ftruncate() that shrinks a file destroys the bytes past the new length.
// The LAST deterministic destruction path that is not a delete: it arrives on
// an already-open fd, so the open-time check could not have seen it coming.
// ---------------------------------------------------------------------------
int escrow_shrink_victim(const char *path, uint64_t newlen) {
    process_t *p = proc_current();
    if (!p || !p->escrow_active) return 0;
    if (!path || !path[0]) return 0;

    // A NETWORK path has no local recoverable form (the bin is local), and the
    // one thing that must not happen is a destructive path staying destructive
    // because it was out of reach. Refuse, as sys_unlink and sys_rename do.
    if (path[0] == '/' && (path[1] == 'S' || path[1] == 'N' || path[1] == 'M')) {
        if (!strncmp(path, "/SMB/", 5) || !strncmp(path, "/NFS/", 5) ||
            !strncmp(path, "/MNT/", 5)) {
            kprintf("[ESCROW] DENY ftruncate pid=%u path=%s (not a local path, "
                    "so the pre-image could not be kept; kernel-enforced)\n",
                    (unsigned)p->pid, path);
            return -1;
        }
    }

    uint32_t sz = 0; int isdir = 0;
    uint32_t vd = escrow_victim_look(p, path, "ftruncate", &sz, &isdir);
    if (vd == ESCROW_VIC_UNREACHABLE) return -1;   // already logged + journalled
    if (vd != ESCROW_VIC_PRESENT) return 0;
    if (isdir) return 0;                   // ftruncate of a directory is not a thing
    if (sz == 0) return 0;                 // nothing to lose
    if (newlen >= (uint64_t)sz) return 0;  // not a shrink: no bytes are dropped

    // A SNAPSHOT, never a MOVE: the caller holds the fd open and the file must
    // stay exactly where it is. The old/new lengths ride along in the route
    // label so the one serial line still says what was about to be dropped;
    // newlen < sz here, and sz is a uint32, so the narrowing cannot lose a bit.
    char rt[56];
    snprintf(rt, sizeof(rt), "ftruncate shrink %u -> %u",
             (unsigned)sz, (unsigned)newlen);
    return escrow_preimage_keep(p, path, 0, rt, 0, 0);
}

// ---------------------------------------------------------------------------
// An O_TRUNC open of an existing file destroys every byte in it. See the
// contract on the declaration in escrow_guard.h, including the precise
// statement of what this does NOT cover.
// ---------------------------------------------------------------------------
int escrow_trunc_victim(const char *path, int flags) {
    // O_TRUNC is 0x200 and O_CREAT is 0x40 in BOTH fs/vfs.h and
    // userland/libc/fcntl.h (checked, not assumed). Compared numerically rather
    // than by pulling a flags header into this file.
    if (!(flags & 0x200)) return 0;
    process_t *p = proc_current();
    if (!p || !p->escrow_active) return 0;
    if (!path || !path[0]) return 0;
    uint32_t sz = 0; int isdir = 0;
    uint32_t vd = escrow_victim_look(p, path, "truncating open", &sz, &isdir);
    if (vd == ESCROW_VIC_UNREACHABLE) return -1;   // already logged + journalled
    if (vd != ESCROW_VIC_PRESENT) return 0;        // nothing to lose
    // Truncating a directory is not a thing. Let the FS refuse it as it
    // already would, and do not manufacture a bin entry for it.
    if (isdir) return 0;
    // An ALREADY-EMPTY file has no pre-image worth keeping, and capturing one
    // would drop a zero-byte entry into the user's bin every time an app
    // reopened a fresh file with O_TRUNC, which is most of them.
    if (sz == 0) return 0;

    // WITH O_CREAT the file is about to be recreated, so a MOVE is correct and
    // leaves no duplicate. WITHOUT it the caller expects the file to still be
    // there, so the pre-image has to be a COPY: moving it would make the open
    // fail with ENOENT and turn a destructive operation into a confusing one.
    return escrow_preimage_keep(p, path, (flags & 0x40) != 0, "truncating open",
                                0, 0);
}

// ---------------------------------------------------------------------------
// A rename over an existing file is a delete of that file. See the contract on
// the declaration in escrow_guard.h for why this is separate from the DELETE
// branch of the guard: by the time a rename reaches the filesystem, nothing in
// the call looks like a deletion, so the delete chokepoint never sees it.
// ---------------------------------------------------------------------------
int escrow_rename_victim(const char *newpath) {
    process_t *p = proc_current();
    if (!p || !p->escrow_active) return 0;        // not our business
    if (!newpath || !newpath[0]) return 0;
    int isdir = 0;
    uint32_t vd = escrow_victim_look(p, newpath, "rename", 0, &isdir);
    if (vd == ESCROW_VIC_UNREACHABLE) return -1;   // already logged + journalled
    if (vd != ESCROW_VIC_PRESENT) return 0;        // nothing to displace

    // A DIRECTORY at the destination is not recyclable (the flat-bin
    // exception), and a rename over a directory is not something any contract
    // issued today needs. Refuse rather than destroy.
    { if (isdir) {
          if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
              g_deny_logged++;
              kprintf("[ESCROW] DENY rename pid=%u over DIRECTORY %s (a "
                      "directory has no recoverable form in the Recycle Bin; "
                      "kernel-enforced)\n", (unsigned)p->pid, newpath);
          }
          escrow_journal_decision(p, ESCROW_DEC_DENY, (uint8_t)ESCROW_OP_RMDIR,
                                  ESCROW_DEC_R_NO_RMDIR, newpath, 0);
          return -1;
      } }

    // [renamevol] 2026-10-04: THE REMOVABLE-DESTINATION REFUSAL THAT USED TO
    // BE HERE IS GONE, AND THE PRECONDITION IT WAS WAITING FOR IS NOW MET.
    //
    // WHAT IT SAID AND WHY. A MOVE capture is only safe where the rename is
    // CERTAIN to replace the destination. On 2026-10-03 sys_rename performed
    // every rename with fat_rename(&g_fat_fs, ...), which cannot reach a
    // hot-plugged volume, so a rename on a stick ALWAYS failed. Once the
    // victim lookup below could SEE a destination on a stick, this route
    // captured it as a MOVE and the rename then failed: the file left the
    // stick and was not replaced. MEASURED at the time: "rename over a file on
    // the bound device rc=-1 victim-intact=no". The bytes were recoverable
    // from the bin, so it was not data loss, but a file vanished for an
    // operation that did not happen. The answer then was a REFUSAL, and the
    // comment named the condition for removing it: "WHEN sys_rename IS MADE TO
    // REACH REMOVABLE VOLUMES ... the question to ask then is whether the
    // rename is certain to replace the destination on that volume. If it is,
    // delete this branch and the MOVE below is correct again."
    //
    // IT IS, AND THERE ARE NOW TWO INDEPENDENT REASONS, not one:
    //   1. sys_rename routes BOTH ends through hotplug_volume_for_path() and
    //      performs the rename on the volume that serves them, so a
    //      same-volume rename reaches the medium and replaces the destination
    //      exactly as it does on the root filesystem. Proven end to end by
    //      /APPS/USBRNT and by ESCROW4T's rename arm, which now asserts a
    //      CAPTURE rather than a non-destruction.
    //   2. sys_rename refuses a cross-volume or unreachable pair BEFORE it
    //      calls this function, and requires the SOURCE to exist before
    //      calling it. So by the time this runs, the rename's preconditions
    //      have been checked and the only remaining failure is the filesystem
    //      operation itself - the same residual risk the root path has always
    //      carried and has been proven on.
    //
    // Both conditions live in sys_rename, not here, and that is the ordering
    // this function depends on. If a future caller reaches this without them,
    // the vanish is back.

    // A MOVE: the destination is about to be overwritten by the rename, so the
    // displaced file is going away whatever happens and a copy would leave a
    // duplicate behind.
    return escrow_preimage_keep(p, newpath, 1, "rename over", 0, 0);
}

// ---------------------------------------------------------------------------
// A FULL-FILE REPLACEMENT IS A DELETE OF EVERY BYTE THAT WAS THERE. See the
// contract on the declaration in escrow_guard.h, including why a COPY and not
// a MOVE.
// ---------------------------------------------------------------------------
int escrow_replace_victim(const char *path) {
    process_t *p = proc_current();
    if (!p || !p->escrow_active) return 0;     // not our business: one PCB byte
    if (!path || !path[0]) return 0;

    // A NETWORK path has no local recoverable form (the bin is local), refused
    // exactly as escrow_shrink_victim refuses one. SYS_PKG_WRITE's own writers
    // do not reach these mounts today, so this is a guard against a future
    // widening rather than a live route: the one thing that must not happen is
    // a destructive path staying destructive because it was out of reach.
    if (path[0] == '/' && (path[1] == 'S' || path[1] == 'N' || path[1] == 'M')) {
        if (!strncmp(path, "/SMB/", 5) || !strncmp(path, "/NFS/", 5) ||
            !strncmp(path, "/MNT/", 5)) {
            kprintf("[ESCROW] DENY package write pid=%u path=%s (not a local "
                    "path, so the pre-image could not be kept; kernel-enforced)\n",
                    (unsigned)p->pid, path);
            return -1;
        }
    }

    // Gather the three facts the policy needs, through the ONE volume-aware
    // lookup so this sees the real target of the write on the ext2 root, the
    // FAT ESP, a mounted disk image OR a removable volume. It used to ask
    // g_fat_fs directly, which is blind to a removable volume and therefore
    // reported every file on a plugged-in stick as absent.
    uint32_t cur_len = 0; int isdir_i = 0;
    uint32_t vd = escrow_victim_look(p, path, "package write", &cur_len, &isdir_i);
    if (vd == ESCROW_VIC_UNREACHABLE) return -1;   // already logged + journalled
    uint32_t exists = (vd == ESCROW_VIC_PRESENT) ? 1u : 0u;
    uint32_t isdir = isdir_i ? 1u : 0u;

    // THE DECISION IS RUST (rustkern/escrowmark.rs). This function gathers
    // facts and performs I/O; it does not decide.
    uint32_t disp = escmark_replace_disposition_rs(p->escrow_active, exists,
                                                   isdir, cur_len,
                                                   recycle_available() ? 1u : 0u);
    if (disp == ESCROW_REPL_SKIP) return 0;
    if (disp == ESCROW_REPL_DENY) {
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY package write pid=%u path=%s (no usable "
                    "Recycle Bin, so the %u bytes about to be replaced could "
                    "not be kept; kernel-enforced, fail closed)\n",
                    (unsigned)p->pid, path, (unsigned)cur_len);
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_DELETE,
                                ESCROW_DEC_R_NO_BIN, path, 0);
        return -1;
    }

    // A COPY, never a MOVE, and this is the ONE place this route differs from
    // the O_TRUNC|O_CREAT one it otherwise matches exactly. An O_TRUNC|O_CREAT
    // open has already committed to recreating the file by the time the check
    // runs, so a move leaves no duplicate and loses nothing. A package write
    // can still fail AFTER this point (a kmalloc for the payload, a
    // copy_from_user of the caller's buffer, an ext2 or FAT write error), and
    // today a failed SYS_PKG_WRITE leaves the target file untouched. Moving
    // the pre-image out would turn a failed install into a destroyed file:
    // that would be a NEW destruction route introduced by the protection
    // against destruction. A duplicate in the bin is the cheaper mistake, and
    // it is the behaviour the bin already has (no de-duplication: see the note
    // on escrow_trunc_victim in the header).
    return escrow_preimage_keep(p, path, 0, "package write", 0, 0);
}

// ---------------------------------------------------------------------------
// A PLAIN write() INTO BYTES THAT ALREADY EXIST. See the contract on the
// declaration in escrow_guard.h for the route, who calls it and why the
// decision is possible at write time when it was not possible at open time.
// ---------------------------------------------------------------------------
int escrow_overwrite_victim(const char *path, uint64_t pos, uint64_t count,
                            uint64_t orig_len, const void *pre,
                            uint32_t prelen) {
    process_t *p = proc_current();
    if (!p || !p->escrow_active) return 0;   // not our business: one PCB byte
    if (!path || !path[0]) return 0;

    // THE DECISION IS RUST (rustkern/escrowmark.rs). Asked BEFORE any I/O, so
    // an append, a create, an empty original or an already-captured descriptor
    // costs exactly this call and nothing else.
    uint32_t disp = escmark_overwrite_disposition_rs(
        p->escrow_active, orig_len, pos, count,
        0u /* the caller tracks per-descriptor state and does not ask twice */,
        recycle_available() ? 1u : 0u);
    if (disp == ESCROW_OVW_SKIP) return 0;
    if (disp == ESCROW_OVW_DENY) {
        if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
            g_deny_logged++;
            kprintf("[ESCROW] DENY partial write pid=%u path=%s (%llu bytes at "
                    "offset %llu would overwrite part of a %llu-byte file and "
                    "there is no usable Recycle Bin to keep the pre-image in; "
                    "kernel-enforced, fail closed)\n",
                    (unsigned)p->pid, path, (unsigned long long)count,
                    (unsigned long long)pos, (unsigned long long)orig_len);
        }
        escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_WRITE,
                                ESCROW_DEC_R_NO_BIN, path, 0);
        return -1;
    }

    // A NETWORK OR AUX-MOUNT path has no local recoverable form (the bin is
    // local and /MNT has no fat_fs_t handle to read through), refused exactly
    // as escrow_shrink_victim and escrow_replace_victim refuse one. Checked
    // AFTER the policy so an append to a file on such a mount is still allowed:
    // the refusal is for DESTRUCTION, not for the mount.
    if (path[0] == '/' && (path[1] == 'S' || path[1] == 'N' || path[1] == 'M')) {
        if (!strncmp(path, "/SMB/", 5) || !strncmp(path, "/NFS/", 5) ||
            !strncmp(path, "/MNT/", 5)) {
            if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
                g_deny_logged++;
                kprintf("[ESCROW] DENY partial write pid=%u path=%s (not a "
                        "local path, so the pre-image could not be kept; "
                        "kernel-enforced, fail closed)\n",
                        (unsigned)p->pid, path);
            }
            escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_WRITE,
                                    ESCROW_DEC_R_NO_BIN, path, 0);
            return -1;
        }
    }

    // A COPY, never a MOVE: the caller holds the descriptor open and the file
    // must stay exactly where it is. `pre`, when the caller has it, is the
    // pristine original still sitting in the descriptor's own whole-file
    // buffer, so this costs NO read; without it the bytes are read back from
    // the filesystem.
    char rt[72];
    snprintf(rt, sizeof(rt), "partial write %llu bytes at %llu of %llu",
             (unsigned long long)count, (unsigned long long)pos,
             (unsigned long long)orig_len);
    if (escrow_preimage_keep(p, path, 0, rt, pre, prelen) != 0) return -1;
    return 1;
}

// ---------------------------------------------------------------------------
// RECYCLE-ONLY binding. See the contract on the declaration in escrow_guard.h.
//
// Called from proc_create_user_as() BEFORE the new process is enqueued, so the
// restriction is in place from its first instruction and the program has no
// opportunity to decline it.
// ---------------------------------------------------------------------------
void escrow_recycle_bind(void *pv, const char *program_path) {
    process_t *p = (process_t *)pv;
    if (!p || !program_path) return;
    // Never downgrade an existing mark. A process the trusted spawner already
    // marked with a scoped contract must keep it.
    if (p->escrow_active != ESCROW_ACTIVE_NONE) return;
    if (!escmark_is_ai_program_rs((const unsigned char *)program_path)) return;

    // NO I/O HERE, DELIBERATELY, and this is not a style preference.
    // proc_create_user_as() is called by spawn_impl() with PREEMPTION DISABLED
    // across the whole call. gfs_node_create() appends a journal record, which
    // is blocking filesystem I/O, so minting the graph identity here would be
    // a may-block operation in a no-block window. It would NOT be caught by
    // wq_assert_may_block() either, because preemption-disable leaves RFLAGS.IF
    // set, which is precisely what makes this class of mistake ship. So the
    // mark, which is three stores and a byte, happens here; the IDENTITY is
    // minted lazily on the first capture, in escrow_delete_redirect(), which
    // runs in ordinary syscall context.
    p->escrow_task_node  = 0;    // minted on first use (see below)
    p->escrow_obj_node   = 0;    // no contract object: nothing is scoped
    p->escrow_grant_edge = 0;    // no grant: no authority is conferred
    p->escrow_scope_prefix[0] = '\0';
    p->escrow_active = ESCROW_ACTIVE_RECYCLE;

    kprintf("[ESCROW] RECYCLE-BIND pid=%u program=%s "
            "(kernel-applied at launch, irrevocable, inherited: this process "
            "can delete but cannot destroy)\n",
            (unsigned)p->pid, program_path);
}

// ---------------------------------------------------------------------------
// The redirect itself. See the contract on the declaration in escrow_guard.h:
// guard FIRST, the syscall's own permission checks SECOND, this THIRD.
// ---------------------------------------------------------------------------
int escrow_delete_redirect(const char *path) {
    process_t *p = proc_current();
    if (!p || !p->escrow_active) return -1;   // only ever for a marked actor

    // Mint the AI_TASK IDENTITY on first use, for a RECYCLE-mode process that
    // was bound at launch (see escrow_recycle_bind: it could not do this
    // itself, under preemption-disable). An IDENTITY, with NO grant edge: an
    // identity says who did something and is what the journal records and the
    // object edges are keyed on; a grant says what they may do, and RECYCLE
    // mode confers nothing.
    //
    // If the graph is not up, or the mint fails, the task node stays 0 and the
    // capture proceeds anyway with an unattributed "ai" actor token. The
    // ENFORCEMENT must never depend on the audit layer being available: a
    // degraded graph turning a recoverable delete into a destructive one would
    // be the worst possible failure mode for this feature.
    if (p->escrow_task_node == 0 && p->escrow_active == ESCROW_ACTIVE_RECYCLE &&
        gfs_fold_ready()) {
        uint32_t cand = GFS_NODE_ID(GFS_KIND_AI_TASK, escrow_next_local());
        if (gfs_node_create(cand, GFS_KIND_AI_TASK, GFS_NODE_ESCROW) == 0)
            p->escrow_task_node = cand;
    }

    int rc = recycle_capture(path, RCY_ACTOR_AI, p->escrow_task_node);
    if (rc == RCY_OK) return 0;

    // The capture failed AFTER the guard said it could proceed, which means
    // something changed underneath (the bin went away, the volume filled, the
    // index would not take another row). Nothing has been removed. Record the
    // real outcome rather than leaving the earlier RECYCLED record as the last
    // word, because an audit trail that says a file was recycled when it was
    // not is worse than one that says nothing.
    if (g_deny_logged < ESCROW_DENY_LOG_CAP) {
        g_deny_logged++;
        kprintf("[ESCROW] DENY delete pid=%u path=%s (Recycle Bin capture "
                "failed: %s [rc=%d]; NOTHING was removed; kernel-enforced, "
                "fail closed)\n", (unsigned)p->pid, path ? path : "(null)",
                recycle_strresult(rc), rc);
    }
    escrow_journal_decision(p, ESCROW_DEC_DENY, ESCROW_OP_DELETE,
                            ESCROW_DEC_R_NO_BIN, path, 0);
    return -1;
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
    // ONE DEFINITION OF THE RULE. This used to test ESCROW_ACTIVE_LOCKED
    // directly; it now asks rustkern/escrowmark.rs, which is the same function
    // the SPAWN inheritance rule consults. Two copies of "who may leave a
    // contract" in two files is exactly how an escape gets reintroduced by
    // someone fixing one of them.
    if (!escmark_may_self_exit_rs(p->escrow_active)) {
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
