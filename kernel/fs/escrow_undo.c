// escrow_undo.c - #246/#305 AI escrow, Stage 5 (part B): the ROLLBACK/UNDO
// engine. See escrow_undo.h for the model and the honest boundary.
//
// WHY THIS IS C, NOT RUST. The reversibility POLICY (what is reversible, and its
// inverse) is already Rust and merely called from here: rustkern/escrowundo.rs
// (escu_is_reversible_rs / escu_inverse_rs / escu_policy_selftest_rs). What
// remains here is entanglement glue Rust cannot reach in this tree: the C FS
// primitives (fat_rename / fat_delete / fat_exists, which route to ext2 on the
// ext2 root), the fixed per-contract table state, and kprintf. This is the same
// stated justification fs/escrow_device.c and fs/graphfs/fold.c are written
// under, not "the surrounding code is C".
//
// NO BUSY-WAIT, NO SPIN. record()/clear() take a bounded irqsave spinlock across
// pure memory only. rollback() takes the spinlock ONLY to detach the log, then
// releases it and runs the (may-block) fat_* FS calls with NO lock held, so it
// satisfies wq_assert_may_block() and never spins. The replay loop is a bounded
// index walk over a fixed array, not a wait loop (concurrency-lint is unmoved).

#include "escrow_undo.h"
#include "fat.h"                  // fat_rename / fat_delete / fat_exists (route to ext2)
#include "../sync/spinlock.h"
#include "../string.h"
#include "../serial.h"

// The pure policy (rustkern/escrowundo.rs).
extern uint32_t escu_is_reversible_rs(uint32_t op);
extern uint32_t escu_inverse_rs(uint32_t op);
extern int32_t  escu_policy_selftest_rs(uint32_t *out_checks);

// Inverse action codes. MIRRORED from rustkern/escrowundo.rs.
#define ESCU_INV_NONE         0
#define ESCU_INV_RENAME_BACK  1
#define ESCU_INV_RMDIR_EMPTY  2

extern fat_fs_t g_fat_fs;

// ---------------------------------------------------------------------------
// The per-contract undo log table.
// ---------------------------------------------------------------------------
// Bounded, static, fixed-size (no allocation on the record hot path). The caps
// are the interim bound before a journal-backed / rotating log (the same class
// of interim bound Stage 3's ESCROW_DECISION_JOURNAL_CAP is). Past the per-
// contract entry cap, further reversible ops are DROPPED and counted in
// `overflow`, so a rollback result never silently under-reports.
#define ESCU_PATH_MAX       256
// #246 Stage 5B2: raised 32 -> 64 so a realistic photo-organize batch (dozens of
// moves plus the date-directories it creates) is FULLY revertible in one
// contract. Past this cap, further reversible ops are DROPPED and counted in
// `overflow`, so a rollback result reports INCOMPLETE rather than silently losing
// track. Cost: ~520 bytes/entry * 64 * ESCU_MAX_CONTRACTS(8) ~= 266KB static, an
// acceptable bound for a desktop kernel; the journal-backed log (a later slice)
// removes the fixed cap entirely.
#define ESCU_MAX_ENTRIES    64   // reversible ops recorded per contract
#define ESCU_MAX_CONTRACTS  8    // concurrent contracts holding an undo log

// Per-slot state. A REVERTING slot is neither reusable by a new contract nor
// appendable, so rollback can iterate its entries lock-free after detaching.
#define ESCU_SLOT_FREE      0
#define ESCU_SLOT_ACTIVE    1
#define ESCU_SLOT_REVERTING 2

typedef struct {
    uint8_t op;                  // ESCU_OP_*
    char    a[ESCU_PATH_MAX];    // rename: from ; mkdir: created dir (NUL-terminated)
    char    b[ESCU_PATH_MAX];    // rename: to   ; mkdir: unused
} escu_entry_t;

typedef struct {
    uint8_t  state;              // ESCU_SLOT_*
    uint32_t task_node;          // the contract's AI_TASK node id (unique per contract)
    uint32_t n;                  // number of entries used (append order = chronological)
    uint32_t overflow;           // reversible ops dropped past ESCU_MAX_ENTRIES
    escu_entry_t e[ESCU_MAX_ENTRIES];
} escu_log_t;

static escu_log_t g_undo[ESCU_MAX_CONTRACTS];
static spinlock_t g_undo_lock = SPINLOCK_INIT;

static void escu_copy_path(char *dst, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    unsigned i = 0;
    for (; src[i] && i < ESCU_PATH_MAX - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Find an ACTIVE slot for this task, or -1. Caller holds g_undo_lock.
static int escu_find_active(uint32_t task_node) {
    for (int i = 0; i < ESCU_MAX_CONTRACTS; i++)
        if (g_undo[i].state == ESCU_SLOT_ACTIVE && g_undo[i].task_node == task_node)
            return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Record a reversible mutation. Called from escrow_fs_guard()'s ALLOW path.
// ---------------------------------------------------------------------------
void escrow_undo_record(uint32_t task_node, uint32_t op,
                        const char *path_a, const char *path_b) {
    if (task_node == 0) return;
    if (!escu_is_reversible_rs(op)) return;   // policy: only RENAME / MKDIR here
    if (!path_a || path_a[0] != '/') return;

    uint64_t f = spinlock_acquire_irqsave(&g_undo_lock);
    int idx = escu_find_active(task_node);
    if (idx < 0) {
        for (int i = 0; i < ESCU_MAX_CONTRACTS && idx < 0; i++) {
            if (g_undo[i].state == ESCU_SLOT_FREE) {
                idx = i;
                g_undo[i].state = ESCU_SLOT_ACTIVE;
                g_undo[i].task_node = task_node;
                g_undo[i].n = 0;
                g_undo[i].overflow = 0;
            }
        }
    }
    if (idx < 0) {
        // No slot available: fail SAFE. The effect still happened (this is audit
        // for undo, not an enforcement gate); we simply cannot offer to revert
        // it. A later journal-backed log removes this cap.
        spinlock_release_irqrestore(&g_undo_lock, f);
        return;
    }
    escu_log_t *lg = &g_undo[idx];
    if (lg->n >= ESCU_MAX_ENTRIES) {
        lg->overflow++;
        spinlock_release_irqrestore(&g_undo_lock, f);
        return;
    }
    escu_entry_t *ent = &lg->e[lg->n];
    ent->op = (uint8_t)op;
    escu_copy_path(ent->a, path_a);
    escu_copy_path(ent->b, path_b);
    lg->n++;
    spinlock_release_irqrestore(&g_undo_lock, f);
}

// ---------------------------------------------------------------------------
// Revert one entry, NON-DESTRUCTIVELY. Returns 1 if reverted, 0 if skipped.
// Runs with NO lock held (may-block fat_* calls).
// ---------------------------------------------------------------------------
static int escu_revert_entry(const escu_entry_t *ent) {
    uint32_t inv = escu_inverse_rs(ent->op);
    if (inv == ESCU_INV_RENAME_BACK) {
        // Forward moved a -> b. Inverse: move b back to a, but ONLY if a is free,
        // so we never clobber whatever now occupies the original slot.
        if (!fat_exists(&g_fat_fs, ent->b)) return 0;   // forward not present / already reverted
        if (fat_exists(&g_fat_fs, ent->a)) return 0;    // original slot occupied: refuse to clobber
        return fat_rename(&g_fat_fs, ent->b, ent->a) == 0 ? 1 : 0;
    }
    if (inv == ESCU_INV_RMDIR_EMPTY) {
        // Forward created dir a. Inverse: remove it, but ONLY if empty. fat_delete
        // routes to ext2_rmdir, which returns -3 (not 0) for a non-empty dir, so a
        // non-empty directory is left intact and counted as skipped.
        if (!fat_exists(&g_fat_fs, ent->a)) return 0;   // already gone
        return fat_delete(&g_fat_fs, ent->a) == 0 ? 1 : 0;
    }
    return 0;   // ESCU_INV_NONE: nothing to do
}

// ---------------------------------------------------------------------------
// Roll back a contract's reversible effects (LIFO), then clear the log.
// ---------------------------------------------------------------------------
escrow_undo_result_t escrow_undo_rollback(uint32_t task_node) {
    escrow_undo_result_t r;
    r.found = 0; r.reverted = 0; r.skipped = 0; r.overflow = 0;
    if (task_node == 0) return r;

    // Detach the log under the lock: mark it REVERTING so no new contract reuses
    // the slot and no append lands in it while we run the (may-block) FS calls.
    uint64_t f = spinlock_acquire_irqsave(&g_undo_lock);
    int idx = escu_find_active(task_node);
    if (idx < 0) { spinlock_release_irqrestore(&g_undo_lock, f); return r; }
    g_undo[idx].state = ESCU_SLOT_REVERTING;
    uint32_t n        = g_undo[idx].n;
    r.overflow        = g_undo[idx].overflow;
    spinlock_release_irqrestore(&g_undo_lock, f);
    r.found = 1;

    // Replay inverses newest-first, lock-free (the slot is detached/REVERTING).
    escu_log_t *lg = &g_undo[idx];
    for (int i = (int)n - 1; i >= 0; i--) {
        if (escu_revert_entry(&lg->e[i])) r.reverted++;
        else                              r.skipped++;
    }

    // Free the slot.
    f = spinlock_acquire_irqsave(&g_undo_lock);
    memset(lg, 0, sizeof(*lg));      // state -> ESCU_SLOT_FREE
    spinlock_release_irqrestore(&g_undo_lock, f);

    kprintf("[ESCROW-UNDO] rollback task=%08x: reverted=%u skipped=%u overflow=%u\n",
            (unsigned)task_node, (unsigned)r.reverted, (unsigned)r.skipped,
            (unsigned)r.overflow);
    return r;
}

// ---------------------------------------------------------------------------
// Drop a contract's undo log without reverting (a plain close KEEPS effects).
// ---------------------------------------------------------------------------
void escrow_undo_clear(uint32_t task_node) {
    if (task_node == 0) return;
    uint64_t f = spinlock_acquire_irqsave(&g_undo_lock);
    int idx = escu_find_active(task_node);
    if (idx >= 0) memset(&g_undo[idx], 0, sizeof(g_undo[idx]));
    spinlock_release_irqrestore(&g_undo_lock, f);
}

// ===========================================================================
// Gated boot self-test. Built only under -DESCROW_UNDO_SELFTEST. Drives the
// REAL FS primitives so the proof is end to end, not mocked.
// ===========================================================================
#ifdef ESCROW_UNDO_SELFTEST
#include "../mm/heap.h"          // kfree for the read-back buffer

static int escu_st_ok = 1;
static void escu_st(const char *name, int ok) {
    if (!ok) escu_st_ok = 0;
    kprintf("[ESCU5-TEST] %-52s %s\n", name, ok ? "PASS" : "FAIL");
}

// Best-effort setup helper: does the file's bytes equal `want`?
static int escu_file_is(const char *path, const char *want, uint32_t wlen) {
    uint32_t sz = 0;
    void *d = fat_read_file(&g_fat_fs, path, &sz);
    int ok = (d && sz == wlen && memcmp(d, want, wlen) == 0);
    if (d) kfree(d);
    return ok;
}

// ---- #246 Stage 5B2 whole-scope-tree helpers (self-test only) ------------
// A recursive, ORDER-INDEPENDENT digest of a directory subtree: every file
// contributes hash(relpath)+hash(bytes) and every directory hash(relpath), all
// ACCUMULATED BY ADDITION so readdir order does not matter. Two trees with the
// same files (paths + bytes) and same directories hash equal; ANY residue (a
// left-behind created dir, a not-moved-back file, a wrong byte) changes the
// digest. It drives the SAME fat_open/fat_readdir/fat_read_file primitives the
// rest of the kernel uses (they route to ext2 on the ext2 root), so the proof is
// end to end, not mocked.
static uint64_t escu_fnv(uint64_t h, const void *p, uint32_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (uint32_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
    return h;
}
static void escu_child_path(char *dst, const char *dir, const char *name) {
    int m = 0;
    for (const char *q = dir;  *q && m < ESCU_PATH_MAX - 2; q++) dst[m++] = *q;
    dst[m++] = '/';
    for (const char *q = name; *q && m < ESCU_PATH_MAX - 1; q++) dst[m++] = *q;
    dst[m] = '\0';
}
static int escu_is_dot(const char *n) {
    return n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'));
}
static void escu_tree_hash(const char *path, int depth, uint64_t *acc,
                           uint32_t *nfiles, uint32_t *ndirs) {
    if (depth > 8) return;
    fat_file_t dir;
    if (fat_open(&g_fat_fs, path, &dir) != 0) return;
    if (!fat_is_dir(&dir)) { fat_close(&dir); return; }
    fat_dir_entry_t ent;
    char name[256];
    while (fat_readdir(&dir, &ent, name) == 0) {
        if (escu_is_dot(name)) continue;
        char child[ESCU_PATH_MAX];
        escu_child_path(child, path, name);
        uint32_t clen = 0; while (child[clen]) clen++;
        if (ent.attr & FAT_ATTR_DIRECTORY) {
            uint64_t h = escu_fnv(1469598103934665603ULL, child, clen);
            h = escu_fnv(h, "D", 1);
            *acc += h; (*ndirs)++;
            escu_tree_hash(child, depth + 1, acc, nfiles, ndirs);
        } else {
            uint32_t sz = 0;
            void *d = fat_read_file(&g_fat_fs, child, &sz);
            uint64_t h = escu_fnv(1469598103934665603ULL, child, clen);
            h = escu_fnv(h, "F", 1);
            if (d) { h = escu_fnv(h, d, sz); kfree(d); }
            *acc += h; (*nfiles)++;
        }
    }
    fat_close(&dir);
}
// Recursively delete a subtree (files then the now-empty dirs). O(1) stack per
// level: it re-opens the dir and removes ONE child at a time until empty, so it
// never iterates a directory while mutating it. Best-effort test cleanup.
static void escu_rmtree(const char *path) {
    fat_file_t probe;
    if (fat_open(&g_fat_fs, path, &probe) != 0) return;   // gone
    int is_dir = fat_is_dir(&probe);
    fat_close(&probe);
    if (is_dir) {
        for (int guard = 0; guard < 8192; guard++) {
            fat_file_t dir;
            if (fat_open(&g_fat_fs, path, &dir) != 0) break;
            fat_dir_entry_t ent; char name[256]; int found = 0;
            while (fat_readdir(&dir, &ent, name) == 0) {
                if (escu_is_dot(name)) continue;
                found = 1; break;
            }
            fat_close(&dir);
            if (!found) break;
            char child[ESCU_PATH_MAX];
            escu_child_path(child, path, name);
            escu_rmtree(child);
        }
    }
    (void)fat_delete(&g_fat_fs, path);
}
static void escu_photo_path(char *dst, const char *dir, int i) {
    int m = 0;
    for (const char *q = dir; *q && m < ESCU_PATH_MAX - 12; q++) dst[m++] = *q;
    dst[m++] = '/'; dst[m++] = 'i'; dst[m++] = 'm'; dst[m++] = 'g';
    dst[m++] = (char)('0' + ((i / 10) % 10));
    dst[m++] = (char)('0' + (i % 10));
    dst[m++] = '.'; dst[m++] = 'j'; dst[m++] = 'p'; dst[m++] = 'g';
    dst[m] = '\0';
}

void escrow_undo_selftest(void) {
    kprintf("\n========== ESCROW STAGE 5 (ROLLBACK/UNDO) SELF-TEST ==========\n");
    escu_st_ok = 1;

    uint32_t pc = 0;
    escu_st("escrowundo.rs policy self-test (rs==expected)",
            escu_policy_selftest_rs(&pc) == 0);
    kprintf("[ESCU5-TEST] (policy assertions run: %u)\n", (unsigned)pc);

    // Scratch area on the writable root. Clean any stale remnants first.
    (void)fat_delete(&g_fat_fs, "/ESCU5/orig.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/moved.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/a.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/b.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/full/inside.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/full");
    (void)fat_delete(&g_fat_fs, "/ESCU5/newdir");
    (void)fat_delete(&g_fat_fs, "/ESCU5/m1.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/m1b.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/m2.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/m2b.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5");
    escu_st("scratch dir /ESCU5 created", fat_mkdir(&g_fat_fs, "/ESCU5") == 0);

    const char *PAYLOAD = "MAYTERA-UNDO-PROOF-42";
    const uint32_t PLEN = 21;   // strlen(PAYLOAD)

    // --- A. MOVE undo, DATA INTACT ---------------------------------------
    escu_st("A: write orig.txt", fat_write_file(&g_fat_fs, "/ESCU5/orig.txt", PAYLOAD, PLEN) == 0);
    escu_st("A: forward move orig -> moved", fat_rename(&g_fat_fs, "/ESCU5/orig.txt", "/ESCU5/moved.txt") == 0);
    escu_st("A: after move, moved exists & orig gone",
            fat_exists(&g_fat_fs, "/ESCU5/moved.txt") && !fat_exists(&g_fat_fs, "/ESCU5/orig.txt"));
    escrow_undo_record(0x05AB0001u, ESCU_OP_RENAME, "/ESCU5/orig.txt", "/ESCU5/moved.txt");
    {
        escrow_undo_result_t r = escrow_undo_rollback(0x05AB0001u);
        escu_st("A: rollback reverted exactly the move", r.found && r.reverted == 1 && r.skipped == 0);
        escu_st("A: after rollback, orig back & moved gone",
                fat_exists(&g_fat_fs, "/ESCU5/orig.txt") && !fat_exists(&g_fat_fs, "/ESCU5/moved.txt"));
        escu_st("A: reverted file BYTES INTACT", escu_file_is("/ESCU5/orig.txt", PAYLOAD, PLEN));
    }

    // --- B. MKDIR undo (empty dir) ---------------------------------------
    escu_st("B: forward mkdir newdir", fat_mkdir(&g_fat_fs, "/ESCU5/newdir") == 0);
    escrow_undo_record(0x05AB0002u, ESCU_OP_MKDIR, "/ESCU5/newdir", 0);
    {
        escrow_undo_result_t r = escrow_undo_rollback(0x05AB0002u);
        escu_st("B: rollback removed the created empty dir", r.found && r.reverted == 1 && r.skipped == 0);
        escu_st("B: after rollback, newdir gone", !fat_exists(&g_fat_fs, "/ESCU5/newdir"));
    }

    // --- C. NON-DESTRUCTIVE: original slot occupied ----------------------
    // Model a move a -> b whose original slot `a` is now occupied by other data.
    // Rollback must REFUSE to move b back over a, leaving BOTH files intact.
    escu_st("C: write a.txt=AAA & b.txt=BBB",
            fat_write_file(&g_fat_fs, "/ESCU5/a.txt", "AAA", 3) == 0 &&
            fat_write_file(&g_fat_fs, "/ESCU5/b.txt", "BBB", 3) == 0);
    escrow_undo_record(0x05AB0003u, ESCU_OP_RENAME, "/ESCU5/a.txt", "/ESCU5/b.txt");
    {
        escrow_undo_result_t r = escrow_undo_rollback(0x05AB0003u);
        escu_st("C: rollback SKIPPED (would clobber occupied slot)", r.found && r.reverted == 0 && r.skipped == 1);
        escu_st("C: a.txt STILL AAA (not clobbered)", escu_file_is("/ESCU5/a.txt", "AAA", 3));
        escu_st("C: b.txt STILL BBB (not moved)", escu_file_is("/ESCU5/b.txt", "BBB", 3));
    }

    // --- D. NON-DESTRUCTIVE: non-empty directory -------------------------
    escu_st("D: mkdir full + a file inside",
            fat_mkdir(&g_fat_fs, "/ESCU5/full") == 0 &&
            fat_write_file(&g_fat_fs, "/ESCU5/full/inside.txt", "X", 1) == 0);
    escrow_undo_record(0x05AB0004u, ESCU_OP_MKDIR, "/ESCU5/full", 0);
    {
        escrow_undo_result_t r = escrow_undo_rollback(0x05AB0004u);
        escu_st("D: rollback SKIPPED (dir not empty)", r.found && r.reverted == 0 && r.skipped == 1);
        escu_st("D: full/ and its file STILL present",
                fat_exists(&g_fat_fs, "/ESCU5/full") && escu_file_is("/ESCU5/full/inside.txt", "X", 1));
    }

    // --- E. LIFO multi-effect + log cleared after rollback ---------------
    escu_st("E: two forward moves",
            fat_write_file(&g_fat_fs, "/ESCU5/m1.txt", "m1", 2) == 0 &&
            fat_write_file(&g_fat_fs, "/ESCU5/m2.txt", "m2", 2) == 0 &&
            fat_rename(&g_fat_fs, "/ESCU5/m1.txt", "/ESCU5/m1b.txt") == 0 &&
            fat_rename(&g_fat_fs, "/ESCU5/m2.txt", "/ESCU5/m2b.txt") == 0);
    escrow_undo_record(0x05AB0005u, ESCU_OP_RENAME, "/ESCU5/m1.txt", "/ESCU5/m1b.txt");
    escrow_undo_record(0x05AB0005u, ESCU_OP_RENAME, "/ESCU5/m2.txt", "/ESCU5/m2b.txt");
    {
        escrow_undo_result_t r = escrow_undo_rollback(0x05AB0005u);
        escu_st("E: both moves reverted", r.found && r.reverted == 2 && r.skipped == 0);
        escu_st("E: both originals restored",
                fat_exists(&g_fat_fs, "/ESCU5/m1.txt") && fat_exists(&g_fat_fs, "/ESCU5/m2.txt") &&
                !fat_exists(&g_fat_fs, "/ESCU5/m1b.txt") && !fat_exists(&g_fat_fs, "/ESCU5/m2b.txt"));
        escrow_undo_result_t r2 = escrow_undo_rollback(0x05AB0005u);
        escu_st("E: log cleared (2nd rollback finds nothing)", r2.found == 0);
    }

    // --- F. clear() drops the log WITHOUT reverting ----------------------
    // a.txt still holds "AAA" from test C. Record a move that WOULD move it,
    // then clear() the log: rollback must find nothing and a.txt is untouched.
    escrow_undo_record(0x05AB0006u, ESCU_OP_RENAME, "/ESCU5/x.txt", "/ESCU5/a.txt");
    escrow_undo_clear(0x05AB0006u);
    {
        escrow_undo_result_t r = escrow_undo_rollback(0x05AB0006u);
        escu_st("F: after clear(), nothing to revert & a.txt untouched",
                r.found == 0 && escu_file_is("/ESCU5/a.txt", "AAA", 3));
    }

    // --- G. PHOTO-ORGANIZE: whole-scope-tree before/after HASH-EQUAL ------
    // The failed/expired-contract case Task B targets: create date sub-dirs and
    // MOVE many photos into them, then FAIL. Assert the WHOLE scope subtree
    // hashes identically to its pre-contract state (moves back + created dirs
    // gone), a pre-existing file the contract must not touch survives, and the
    // batch is LARGER than the old 32-entry cap yet reverts with ZERO overflow
    // (proving the #246 Stage 5B2 cap raise makes a realistic organize fully
    // revertible).
    {
        const char *SCOPE = "/ESCU5/PHOTOS";
        const uint32_t GTASK = 0x05AB00A0u;
        const int NPH = 36;   // > old ESCU_MAX_ENTRIES(32); + 2 mkdirs = 38 entries
        char pth[ESCU_PATH_MAX], dst[ESCU_PATH_MAX], pay[16];

        escu_rmtree(SCOPE);
        escu_st("G: scope /ESCU5/PHOTOS created", fat_mkdir(&g_fat_fs, SCOPE) == 0);
        // a pre-existing file the contract is FORBIDDEN to lose (not one of its moves)
        (void)fat_mkdir(&g_fat_fs, "/ESCU5/PHOTOS/keep");
        escu_st("G: seed keep/existing.jpg",
                fat_write_file(&g_fat_fs, "/ESCU5/PHOTOS/keep/existing.jpg", "KEEPME", 6) == 0);
        // seed NPH photos at the scope root, each with unique bytes
        int seeded = 1;
        for (int i = 0; i < NPH; i++) {
            escu_photo_path(pth, SCOPE, i);
            pay[0]='P'; pay[1]='H'; pay[2]='-';
            pay[3]=(char)('0'+((i/10)%10)); pay[4]=(char)('0'+(i%10)); pay[5]='\0';
            if (fat_write_file(&g_fat_fs, pth, pay, 5) != 0) seeded = 0;
        }
        escu_st("G: seeded all photos at scope root", seeded);

        // snapshot BEFORE the contract runs
        uint64_t before = 0; uint32_t bf = 0, bd = 0;
        escu_tree_hash(SCOPE, 0, &before, &bf, &bd);

        // run the contract: create date-dirs + move photos in, recording each
        // reversible effect exactly as escrow_fs_guard's ALLOW path would.
        int organized = 1;
        if (fat_mkdir(&g_fat_fs, "/ESCU5/PHOTOS/2024") != 0) organized = 0;
        escrow_undo_record(GTASK, ESCU_OP_MKDIR, "/ESCU5/PHOTOS/2024", 0);
        if (fat_mkdir(&g_fat_fs, "/ESCU5/PHOTOS/2024/jan") != 0) organized = 0;
        escrow_undo_record(GTASK, ESCU_OP_MKDIR, "/ESCU5/PHOTOS/2024/jan", 0);
        for (int i = 0; i < NPH; i++) {
            escu_photo_path(pth, SCOPE, i);
            escu_photo_path(dst, (i % 2) ? "/ESCU5/PHOTOS/2024/jan"
                                         : "/ESCU5/PHOTOS/2024", i);
            if (fat_rename(&g_fat_fs, pth, dst) != 0) organized = 0;
            escrow_undo_record(GTASK, ESCU_OP_RENAME, pth, dst);
        }
        escu_st("G: contract organized the photos (mkdirs + moves)", organized);

        uint64_t mid = 0; uint32_t mf = 0, md = 0;
        escu_tree_hash(SCOPE, 0, &mid, &mf, &md);
        escu_st("G: organize CHANGED the tree (mid != before)", mid != before);

        // FAIL / EXPIRE: roll the whole contract back.
        escrow_undo_result_t r = escrow_undo_rollback(GTASK);
        escu_st("G: rolled back ALL 38 effects, ZERO overflow (cap raise proven)",
                r.found && r.reverted == (uint32_t)(NPH + 2) &&
                r.skipped == 0 && r.overflow == 0);

        // snapshot AFTER
        uint64_t after = 0; uint32_t af = 0, ad = 0;
        escu_tree_hash(SCOPE, 0, &after, &af, &ad);
        escu_st("G: scope tree HASH-EQUAL to pre-contract state",
                after == before && af == bf && ad == bd);
        escu_st("G: created date-dirs are GONE (empty-created-dir removal)",
                !fat_exists(&g_fat_fs, "/ESCU5/PHOTOS/2024") &&
                !fat_exists(&g_fat_fs, "/ESCU5/PHOTOS/2024/jan"));
        escu_st("G: forbidden-to-lose keep/existing.jpg INTACT",
                escu_file_is("/ESCU5/PHOTOS/keep/existing.jpg", "KEEPME", 6));
        kprintf("[ESCU5-TEST] (photo-organize: before=%016lx f=%u d=%u | "
                "mid=%016lx f=%u d=%u | after=%016lx f=%u d=%u)\n",
                (unsigned long)before, (unsigned)bf, (unsigned)bd,
                (unsigned long)mid, (unsigned)mf, (unsigned)md,
                (unsigned long)after, (unsigned)af, (unsigned)ad);
        escu_rmtree(SCOPE);
    }

    // cleanup (best effort).
    (void)fat_delete(&g_fat_fs, "/ESCU5/orig.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/a.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/b.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/full/inside.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/full");
    (void)fat_delete(&g_fat_fs, "/ESCU5/m1.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5/m2.txt");
    (void)fat_delete(&g_fat_fs, "/ESCU5");

    kprintf("[ESCU5-TEST] SUMMARY %s\n", escu_st_ok ? "OVERALL PASS" : "OVERALL FAIL");
    kprintf("========== ESCROW STAGE 5 (ROLLBACK/UNDO) SELF-TEST END ==========\n");
}
#else
void escrow_undo_selftest(void) { /* not built in production */ }
#endif
