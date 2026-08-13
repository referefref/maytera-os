// journal.h - GraphFS tamper-evident journal (#711), slice 1.
//
// The journal is the SPINE of GraphFS, not a side-effect of it: nodes, edges
// and versions are a fold of this record stream (docs/GRAPHFS_DESIGN.md).
// Slice 1 implements the spine and one real consumer (the security audit sink);
// the fold is slice 2.
//
// The format, the hash chain and ALL verification live in Rust
// (rustkern/gfsjournal.rs). This header declares the C glue that does the I/O.

#ifndef GRAPHFS_JOURNAL_H
#define GRAPHFS_JOURNAL_H

#include "../../types.h"

// Wire sizes. MIRRORED in rustkern/gfsjournal.rs and checked at init through
// gfsj_sizes(), so the two cannot drift silently (the #503 lesson: a constant
// that drifts turns a check into decoration).
#define GFSJ_REC_SIZE     160
#define GFSJ_SEAL_SIZE    96
#define GFSJ_INLINE_MAX   16

// Record ops. 1-15 are journal/system records; 16+ are graph mutations and are
// declared here so the numbering is fixed before slice 2 uses them.
#define GFSJ_OP_BOOT            1   // a boot happened; carries the boot gen
#define GFSJ_OP_AUDIT           2   // a security_audit() event (slice 1 consumer)
#define GFSJ_OP_CHECKPOINT      3   // discontinuity marker (e.g. tamper found)
#define GFSJ_OP_NODE_CREATE     16  // slice 2
#define GFSJ_OP_VERSION_COMMIT  17  // slice 2
#define GFSJ_OP_EDGE_ADD        18  // slice 2: a contract GRANT is an edge
#define GFSJ_OP_EDGE_REVOKE     19  // slice 2
#define GFSJ_OP_NODE_RETIRE     20  // slice 2
#define GFSJ_OP_NODE_RESTORE    21  // slice 2

// Effect classes, from docs/CONTRACT_ARCHITECTURE.md section 7. Declared BEFORE
// execution so that "revert task X" can name what it could not take back
// instead of quietly failing to take it back.
#define GFSJ_EFFECT_REVERSIBLE      0
#define GFSJ_EFFECT_COMPENSATABLE   1
#define GFSJ_EFFECT_IRREVERSIBLE    2
#define GFSJ_EFFECT_NA              3

// Actor kinds (CONTRACT_ARCHITECTURE.md section 3: identities are plural).
#define GFSJ_ACTOR_KERNEL   0
#define GFSJ_ACTOR_PID      1
#define GFSJ_ACTOR_NODE     2   // an identity node: app, service, user, AI task

// Verification reasons. MIRRORED in rustkern/gfsjournal.rs.
#define GFSJ_OK                 0
#define GFSJ_R_RAGGED           1
#define GFSJ_R_BAD_MAGIC        2
#define GFSJ_R_BAD_VERSION      3
#define GFSJ_R_CHAIN_BREAK      4
#define GFSJ_R_SEQ_GAP          5
#define GFSJ_R_HASH_MISMATCH    6
#define GFSJ_R_SEAL_MISSING     7
#define GFSJ_R_SEAL_CORRUPT     8
#define GFSJ_R_TRUNCATED        9
#define GFSJ_R_EXTENDED         10
#define GFSJ_R_HEAD_MISMATCH    11
#define GFSJ_R_ANCHOR_COUNT     12
#define GFSJ_R_ANCHOR_MISMATCH  13
#define GFSJ_R_ARG              14
#define GFSJ_R_NOT_READY        15  // C-side only: journal never came up

// Result of a verification pass. Shared verbatim with Ring 3 (SYS_GFS_VERIFY),
// so the layout is part of the ABI: size locked below, in
// proc/syscall_argtab_lock.c, and across the FFI via gfsj_sizes().
typedef struct gfsj_verify {
    uint32_t status;        // GFSJ_OK, or the first failing reason
    uint32_t reason;        // same value; status may gain flags later
    uint32_t degraded;      // this boot started with an already-bad journal
    uint32_t pad;
    uint64_t bad_seq;       // record index the failure was found at
    uint64_t count;         // records present on disk
    uint64_t seal_count;    // records the seal claims
    uint64_t boot_gen;      // boot generation from the seal
    uint8_t  head_hash[32]; // chain head as computed from the file
} gfsj_verify_t;

_Static_assert(sizeof(gfsj_verify_t) == 80,
               "#711: gfsj_verify_t is ABI (SYS_GFS_VERIFY + rustkern/gfsjournal.rs)");

// Bring the journal up. Call AFTER the root filesystem is mounted. Refuses (and
// says so loudly) unless the root is ext2: the FAT ESP must never carry this,
// because build/invariant-gate.sh fails any image whose ESP holds anything but
// boot assets.
void gfs_journal_init(void);

// Append one record. Blocking: filesystem I/O. Callable ONLY from a context
// that may block (see the single-writer note in journal.c). Returns 0 on
// success, negative on failure. NEVER call this from an interrupt handler or
// with a spinlock held; producers in those contexts must hand off to a worker,
// exactly as security_audit() does.
int gfs_journal_append(uint32_t actor_kind, uint32_t actor_id,
                       uint16_t op, uint16_t effect,
                       const void *payload, uint32_t payload_len);

// The same append, reporting the seq of the record it wrote (#711 slice 2). An
// edge's identity IS its journal seq, so a caller that issues a contract learns
// its id from the position it occupies in the tamper-evident chain rather than
// from a second counter that could disagree with the log.
int gfs_journal_append_seq(uint32_t actor_kind, uint32_t actor_id,
                           uint16_t op, uint16_t effect,
                           const void *payload, uint32_t payload_len,
                           uint64_t *out_seq);

// Verify the on-disk journal against its seal and against the kernel's own
// in-memory head. Blocking (reads the files). 0 if it verifies, else the reason.
int gfs_journal_verify(gfsj_verify_t *out);

// 1 once the journal is up and appending.
int gfs_journal_ready(void);

#endif // GRAPHFS_JOURNAL_H
