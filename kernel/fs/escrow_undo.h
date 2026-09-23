// escrow_undo.h - #246/#305 AI escrow, KERNEL-ENFORCED, Stage 5 (part B):
// the ROLLBACK/UNDO engine (docs/CONTRACT_ENFORCEMENT_PLAN.md section 3f;
// docs/CONTRACT_ARCHITECTURE.md section 7).
//
// WHAT THIS ADDS. Stages 1-4 make the kernel REFUSE an out-of-scope or delete
// action by a marked escrow actor and record every decision tamper-evidently.
// This slice adds the other half of "a task is a transaction": the ability to
// REVERT a contract's REVERSIBLE filesystem effects, so a failed promise or a
// trusted close can move a task's files back and remove the empty directories it
// created, WITHOUT DATA LOSS.
//
// THE HONEST BOUNDARY (architecture section 7). Only CLEANLY reversible effects
// are recorded and revertible here: a MOVE (rename) and a MKDIR of an empty
// directory. A file CREATE or an OVERWRITE is NOT undone (undoing a create by
// deleting risks data loss; undoing an overwrite needs a pre-image snapshot the
// journal-plus-snapshot design will add). The undo surface must never claim to
// take back what it cannot: the rollback result counts exactly what it reverted
// and what it skipped.
//
// NON-DESTRUCTIVE BY CONSTRUCTION. Reverting a move is done only if the original
// slot is FREE (never overwrites whatever now sits there). Reverting a mkdir is
// done only if the directory is EMPTY (fat_delete -> ext2_rmdir refuses a
// non-empty directory, -3). Anything the engine cannot revert safely is skipped
// and counted, never forced.
//
// REUSE, NOT REINVENTION. The replay uses the SAME canonical FS primitives the
// syscall handlers use: fat_rename() / fat_delete() (which route to ext2 on the
// ext2 root) and fat_exists(). The reversibility POLICY (what is reversible, and
// its inverse) lives once in rustkern/escrowundo.rs (escu_* FFI), Rust per the
// 2026-07-16 rule; this C file is the STATE + FS glue, the same entanglement
// exemption fs/escrow_device.c is written under.
#ifndef FS_ESCROW_UNDO_H
#define FS_ESCROW_UNDO_H

#include "../types.h"

// FS-mutation op classes. MIRRORED in rustkern/escrowundo.rs (escu_* FFI).
#define ESCU_OP_RENAME     1   // a move: from -> to
#define ESCU_OP_MKDIR      2   // a directory create
#define ESCU_OP_CREATE     3   // a new-file create (NOT reversible in this slice)
#define ESCU_OP_OVERWRITE  4   // an overwrite of existing bytes (needs a snapshot)
#define ESCU_OP_DELETE     5   // never recorded (no delete grant is ever issued)

// The outcome of one rollback pass. `reverted + skipped` is the number of
// recorded reversible effects; `overflow` is how many were dropped past the
// per-contract cap while recording (so the count never silently lies).
typedef struct {
    int      found;      // 1 if this task had an undo log, 0 if none existed
    uint32_t reverted;   // effects successfully and safely reverted
    uint32_t skipped;    // recorded effects the engine declined to revert (state
                         // moved on, original slot occupied, dir not empty)
    uint32_t overflow;   // reversible ops dropped past the per-contract cap
} escrow_undo_result_t;

// Record a REVERSIBLE mutation an escrow actor was ALLOWED to make, keyed by the
// contract's AI_TASK node id. Called from escrow_fs_guard()'s ALLOW path. A
// no-op for a non-reversible op (create/overwrite/delete) and for task_node 0.
// Memory-only and non-blocking (a bounded spinlock over a fixed table); it never
// changes an enforcement verdict.
void escrow_undo_record(uint32_t task_node, uint32_t op,
                        const char *path_a, const char *path_b);

// Replay the recorded inverses in LIFO order, NON-DESTRUCTIVELY, then clear the
// log. Safe to call from a may-block context (it takes no spinlock across the
// fat_* FS calls). For a task with no log it returns {found=0}.
escrow_undo_result_t escrow_undo_rollback(uint32_t task_node);

// Drop a contract's undo log WITHOUT reverting (a plain EXIT/close KEEPS the
// task's effects; only the log is freed). Idempotent; a no-op for task_node 0.
void escrow_undo_clear(uint32_t task_node);

// Gated boot self-test (built only under -DESCROW_UNDO_SELFTEST). Proves the
// engine end to end on the REAL filesystem: a recorded move reverts with the
// file's bytes intact, a mkdir reverts, and the engine REFUSES to clobber an
// occupied original slot or to remove a non-empty directory. No-op otherwise.
void escrow_undo_selftest(void);

#endif // FS_ESCROW_UNDO_H
