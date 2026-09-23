// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// escrow.h - MayteraOS AI escrow contract with a verifiable PROMISE (#712).
//
// An escrow contract lets the AI assistant perform a bounded, reversible-in-
// intent batch of filesystem work under a PROMISE: a declared set of
// postconditions (folders that must exist, files that must have moved bytes-
// intact, and a hard "delete nothing" guarantee) that is machine-VERIFIED after
// the work runs.
//
// KERNEL-ENFORCED (#246/#305 Stage 6). This is now a THIN CLIENT of the kernel
// escrow. escrow_request() enters the kernel contract (SYS_ESCROW_ENTER); from
// then until close the calling process is a marked Ring-0 escrow actor. The
// contract is:
//   - SCOPED   : a GraphFS WRITE grant scoped to scope_path, issued ONLY by the
//                kernel escrow principal. Every FS mutation by the marked process
//                is checked AT RING 0 (escrow_fs_guard); an out-of-scope write is
//                refused even to a raw uid-0 syscall. The userland aicap layer no
//                longer scopes anything: there is one source of truth, the kernel.
//   - NO-DELETE: no delete-scope grant is EVER issued, so the kernel refuses any
//                delete by the marked process (the invariant is enforced by the
//                ABSENCE of a grant, not a userland denylist).
//   - TIME-BOXED: the grant's TTL (default 3600s = 1 hour) bounds the work. The
//                contract CLOSES EARLY (SYS_ESCROW_EXIT) the moment the promise
//                verifies FULFILLED: the grant is revoked before the TTL.
//   - AUDITED  : every enforcement DECISION (allow and deny) lands in the kernel's
//                tamper-evident hash-chained journal (Stage 3); the request /
//                grant / verdict / close breadcrumbs also go to the plaintext
//                /CONFIG/AIAUDIT.LOG via aicap_audit(), which stays human-readable
//                belt-and-braces, not the source of truth.
//   - REVERSIBLE-ON-FAILURE: on an UNMET promise, escrow_close() calls
//                SYS_ESCROW_ABORT so the KERNEL rolls back the contract's recorded
//                reversible effects (moves back, empty dirs removed) then revokes.
//
// WHAT STAYS USERLAND, honestly. The PROMISE check (escrow_verify) is a
// postcondition ORACLE, not an enforcement boundary: it reports whether the
// declared folders/moves/zero-deletes actually hold, and cannot itself stop a
// disallowed write - the kernel does that. See escrow.c for the full note.
#ifndef ESCROW_H
#define ESCROW_H

// Bounds (contract state lives in BSS, not on the caller's stack).
#define ESCROW_MAX_FILES   512   // recursive before-snapshot cap (delete counter)
#define ESCROW_MAX_MOVES   384   // promised relocations
#define ESCROW_MAX_FOLDERS 64    // promised date folders
#define ESCROW_MAX_UNMET   32    // reported unmet postconditions
#define ESCROW_MAX_DIRS    256   // BFS queue cap for the recursive snapshot
#define ESCROW_TTL_DEFAULT 3600  // 1 hour, the owner's time box

// Verdict / close results.
enum {
    ESCROW_FULFILLED    = 0,
    ESCROW_PARTIAL      = 1,
    ESCROW_ERROR        = -1
};
enum {
    ESCROW_CLOSED_EARLY = 0,   // FULFILLED -> grant revoked before the TTL
    ESCROW_OPEN_TTL     = 1    // NOT fulfilled -> grant retained until the TTL
};

typedef struct {
    char          path[256];
    long          size;
    unsigned char is_dir;
    unsigned char seen_after;   // scratch during verify
} escrow_file_t;

typedef struct {
    char          src[256];     // must be ABSENT after
    char          dst[256];     // must EXIST after, bytes identical to src-before
    long          size;         // src size captured at promise time
    unsigned int  hash;         // FNV-1a of src content at promise time
    unsigned char executed;     // organiser attempted the move
} escrow_move_t;

typedef struct {
    char path[256];             // date folder that must exist after
} escrow_folder_t;

typedef struct {
    int  used;
    int  active_grant;          // the KERNEL escrow contract is live (entered)
    int  verdict;               // last escrow_verify() result
    char scope_path[160];       // the device mount prefix the grant is scoped to
    char tag[64];               // audit_tag identifying this contract in the log
    char note[192];             // human description of the promise
    char last_how[96];          // verify counts, audited by close after the window
    long ttl_secs;
    int  n_before;
    int  n_moves;
    int  n_folders;
    escrow_file_t   before[ESCROW_MAX_FILES];
    escrow_move_t   moves[ESCROW_MAX_MOVES];
    escrow_folder_t folders[ESCROW_MAX_FOLDERS];
} escrow_contract_t;

typedef struct {
    int  verdict;
    int  folders_ok;
    int  folders_total;
    int  moves_ok;
    int  moves_total;
    int  delete_count;          // untracked pre-existing files that vanished (MUST be 0)
    int  n_unmet;
    char unmet[ESCROW_MAX_UNMET][160];
} escrow_verify_t;

// Open a contract over `scope_path` (a mount prefix such as "/MEDIA/USB"):
//   - takes the recursive BEFORE snapshot (for the promise oracle's delete
//     counter) and audits the request/grant, BEFORE entering the marked window,
//   - ENTERS the KERNEL escrow contract (SYS_ESCROW_ENTER): a WRITE-only grant
//     scoped to scope_path (TTL secs, default ESCROW_TTL_DEFAULT when <=0), no
//     delete grant ever, device-bound if scope_path is removable. The kernel is
//     the enforcement authority from here until escrow_close().
// Returns a contract handle (a pointer into a static pool), or 0 on error
// (including the kernel refusing the enter: fail closed, no advisory fallback).
// `promise_note` is a short human description recorded in the audit trail.
escrow_contract_t *escrow_request(const char *scope_path,
                                  const char *promise_note, long ttl_secs);

// Execution primitives the organiser drives INSIDE the marked window. These are
// ordinary FS syscalls that the KERNEL escrow guard checks (scope + no-delete +
// device) and records in the tamper-evident journal; sys_rename/sys_mkdir record
// the confirmed effect for SYS_ESCROW_ABORT rollback. They perform NO userland
// enforcement and write NO /CONFIG audit (which would be refused mid-window).
// escrow_do_mkdir returns 0 on create (non-zero, e.g. already-exists, is left for
// the promise oracle to resolve); escrow_do_move returns 0 on a successful move.
int escrow_do_mkdir(escrow_contract_t *c, const char *path);
int escrow_do_move(escrow_contract_t *c, const char *src, const char *dst);

// Declare a postcondition. add_folder records a date folder that must exist
// after the work. add_move records that `src` must have RELOCATED to `dst`
// bytes-intact and be ABSENT afterwards; it reads `src` NOW to capture its size
// and content hash for the later identity check. Both dedup. Return 0 on
// success, -1 if the promise table is full or src is unreadable.
int escrow_promise_add_folder(escrow_contract_t *c, const char *folder);
int escrow_promise_add_move(escrow_contract_t *c, const char *src, const char *dst);

// Verify the AFTER state against the declared promise. Fills *out (optional)
// with the counts and the list of unmet postconditions. Returns ESCROW_FULFILLED
// only if EVERY promised folder exists, EVERY promised move landed bytes-intact
// with its source gone, AND delete_count == 0 (no pre-existing file vanished
// except as a tracked move). Reads only: this is the postcondition ORACLE, not
// an enforcement path, and it writes no /CONFIG audit (close audits the verdict).
int escrow_verify(escrow_contract_t *c, escrow_verify_t *out);

// Close the KERNEL escrow contract, then audit (unmarked). If the last verify was
// FULFILLED: SYS_ESCROW_EXIT revokes the grant NOW (early close, before the TTL);
// returns ESCROW_CLOSED_EARLY. Otherwise: SYS_ESCROW_ABORT so the KERNEL rolls
// back the recorded reversible effects and revokes the grant (fail-safe: never
// widen access, never leave reversible half-done work); returns ESCROW_OPEN_TTL.
// Frees the contract slot in both cases.
int escrow_close(escrow_contract_t *c);

// Shared readdir helper (raw SYS_READDIR loop, the same idiom aiclient.c uses).
// Fills up to `max` entries of directory `dir`; returns the count, or -1 if the
// directory cannot be opened. Exposed so the organiser reuses one copy.
int escrow_list_dir(const char *dir, char (*names)[256], unsigned char *isdir,
                    unsigned int *sizes, int max);

#endif // ESCROW_H
