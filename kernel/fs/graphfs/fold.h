// fold.h - GraphFS: the fold of the journal into a live graph (#711 slice 2).
//
// docs/GRAPHFS_DESIGN.md section 5.5: nodes and edges are not six APIs with six
// tables. They are record types in ONE append-only journal, and the graph is a
// fold of that record stream. Everything below either APPENDS A RECORD or READS
// THE FOLD. There is no third kind of function, which is how
// CONTRACT_ARCHITECTURE.md section 12.9 ("no bypass path") is obtained by
// construction rather than by discipline.
//
// A CONTRACT IS AN EDGE WITH rel = GRANT. See gfs_grant_issue below.
//
// The policy (who may issue a grant, what a valid scope and expiry are) lives in
// rustkern/gfsfold.rs, in ONE function that both the pre-check and the replay
// path call. This header is the C-visible surface of that.

#ifndef GRAPHFS_FOLD_H
#define GRAPHFS_FOLD_H

#include "../../types.h"

// --- node kinds. MIRRORED in rustkern/gfsfold.rs ---------------------------
// CONTRACT_ARCHITECTURE.md section 3: identities are plural, and an AI task
// identity is one of them, per task.
#define GFS_KIND_SYSTEM        1
#define GFS_KIND_USER          2
#define GFS_KIND_APP           3
#define GFS_KIND_SERVICE       4
#define GFS_KIND_DEVICE        5
#define GFS_KIND_AI_TASK       6
#define GFS_KIND_OBJECT        7
#define GFS_KIND_CONTRACT_DEF  8

// A node id is (kind << 24) | local, so the kind is derivable from the id and a
// producer that can compute a stable `local` (a uid, a task counter) can
// recompute the id without the graph storing any name.
#define GFS_NODE_ID(kind, local)  ((uint32_t)(((uint32_t)(kind) << 24) | ((uint32_t)(local) & 0x00FFFFFFu)))

// The escrow principal (CONTRACT_ARCHITECTURE.md section 5): the ONLY identity
// permitted to issue a GRANT. Enforced in the fold, on the replay path too.
#define GFS_NODE_KERNEL   GFS_NODE_ID(GFS_KIND_SYSTEM, 1)
#define GFS_NODE_ESCROW   GFS_NODE_ID(GFS_KIND_SYSTEM, 2)

// --- relations. MIRRORED in rustkern/gfsfold.rs ----------------------------
#define GFS_REL_GRANT         1   // THIS is a contract
#define GFS_REL_NAME          2
#define GFS_REL_DERIVED_FROM  3
#define GFS_REL_CONFIG_FOR    4

// Scope 0 and expiry 0 are INVALID, never "unlimited": a zeroed payload must be
// incapable of producing authority. The unlimited forms are explicit and are
// COUNTED (CONTRACT_ARCHITECTURE.md section 10).
#define GFS_SCOPE_INVALID     0
#define GFS_SCOPE_ANY         0xFFFF
#define GFS_EXPIRY_INVALID    0u
#define GFS_EXPIRY_NEVER      0xFFFFFFFFu

// --- views handed to C and, read-only, to Ring 3 ---------------------------
typedef struct gfs_node_view {
    uint32_t node_id;
    uint16_t kind;
    uint16_t state;      // 0 live, 1 retired
    uint32_t parent;
    uint32_t pad;
    uint64_t born_seq;
    uint64_t born_boot;
} gfs_node_view_t;

typedef struct gfs_edge_view {
    uint64_t edge_seq;     // the seq of its EDGE_ADD record: its identity
    uint64_t revoked_seq;  // 0 = live
    uint64_t boot_gen;
    uint32_t expiry_ms;
    uint32_t from;
    uint32_t to;
    uint16_t rel;
    uint16_t scope;
    uint32_t flags;        // 1 = expired, 2 = revoked
    uint32_t pad;
} gfs_edge_view_t;

typedef struct gfs_stats {
    uint32_t ready, degraded, overflow;
    uint32_t nodes, edges, node_cap, edge_cap;
    uint32_t grants_live, grants_revoked, grants_expired;
    uint32_t grants_unlimited_scope, grants_never_expire;
    uint32_t refused_not_escrow, refused_bad_scope, refused_bad_expiry;
    uint32_t pad;
    uint64_t boot_gen, records_seen, applied, rejected;
    uint64_t checks, allows, denies, grants_prior_boot;
} gfs_stats_t;

_Static_assert(sizeof(gfs_node_view_t) == 32, "#711: gfs_node_view_t is ABI (SYS_GFS_QUERY + rustkern/gfsfold.rs)");
_Static_assert(sizeof(gfs_edge_view_t) == 48, "#711: gfs_edge_view_t is ABI (SYS_GFS_QUERY + rustkern/gfsfold.rs)");
_Static_assert(sizeof(gfs_stats_t)     == 128, "#711: gfs_stats_t is ABI (SYS_GFS_QUERY + rustkern/gfsfold.rs)");

// ---------------------------------------------------------------------------
// Lifecycle. Called by the journal, which owns the record stream.
// ---------------------------------------------------------------------------

// Rebuild the fold from `log_len` bytes of VERIFIED journal. Called by
// gfs_journal_init() after verification and before the journal accepts its
// first append of this boot, so the fold is never asked a question from a
// half-built graph. `degraded` marks the boot that found the journal bad; it is
// sticky and makes every grant check DENY.
void gfs_fold_replay(const void *log, uint32_t log_len, uint64_t boot_gen, int degraded);

// Fold one record that was JUST appended. The single call site is
// gfs_journal_append(), which is the only place a record comes into existence.
void gfs_fold_on_record(const void *rec, uint64_t boot_gen);

// Seed the system identities, mirror the real user accounts into the graph, and
// run the enforcement self-test. Call from main.c AFTER gfs_journal_init().
void gfs_fold_init(void);

// ---------------------------------------------------------------------------
// Mutations. Each one APPENDS A JOURNAL RECORD; there is no other way to change
// the graph. Blocking (filesystem I/O), so callable only from a context that may
// block, exactly like gfs_journal_append().
// ---------------------------------------------------------------------------

// Create an identity. Returns 0, or negative on refusal.
int gfs_node_create(uint32_t node_id, uint16_t kind, uint32_t parent);

// Retire / restore an identity. Retiring kills every contract it holds or is a
// target of, with no cascade code: a grant check cannot find a live endpoint.
int gfs_node_retire(uint32_t node_id, uint16_t reason);
int gfs_node_restore(uint32_t node_id, uint16_t reason);

// Issue a contract: a directed, scoped, time-limited GRANT edge from `holder`
// to `target`. `actor` MUST be GFS_NODE_ESCROW; anything else is refused and
// counted (CONTRACT_ARCHITECTURE.md section 5). `ttl_ms` is milliseconds from
// now, or GFS_EXPIRY_NEVER. On success `*out_edge` receives the edge id, which
// is the journal seq of the record that created it.
int gfs_grant_issue(uint32_t actor, uint32_t holder, uint32_t target,
                    uint16_t scope, uint32_t ttl_ms, uint64_t *out_edge);

// Revoke a contract. Takes effect in memory before this returns
// (CONTRACT_ARCHITECTURE.md section 6: revocation must bite in flight).
int gfs_grant_revoke(uint32_t actor, uint64_t edge_seq, uint16_t reason);

// ---------------------------------------------------------------------------
// Queries (docs/GRAPHFS_DESIGN.md section 7). Read-only, non-blocking.
// ---------------------------------------------------------------------------

// Q1, the enforcement query. Answered from memory only: no allocation, no I/O,
// no blocking, bounded time, so it is callable from the syscall chokepoint.
// FAILS CLOSED: 0 unless a live, unrevoked, unexpired, in-scope GRANT exists.
// Returns 1 to allow, 0 to deny, and nothing else: an authorization question has
// two answers, and a caller that mistook a negative errno for "true" would fail
// open.
int gfs_grant_check(uint32_t holder, uint32_t target, uint16_t scope);

// Q2 (dir 0, "what does H hold") and Q3 (dir 1, "what is granted on T").
// Bounded by the caller's array. Returns the count, or negative.
int gfs_edges_get(uint32_t node, int dir, gfs_edge_view_t *out, int max);

// The identity list.
int gfs_nodes_list(gfs_node_view_t *out, int max);

// Counters, including the two CONTRACT_ARCHITECTURE.md section 10 requires:
// grants with unlimited scope and grants that never expire.
int gfs_fold_stats(gfs_stats_t *out);

// 1 once the fold is up and answering.
int gfs_fold_ready(void);

// ---------------------------------------------------------------------------
// The Ring-3 surface: SYS_GFS_QUERY (363). READ-ONLY, and there is deliberately
// no mutating counterpart (docs/GRAPHFS_DESIGN.md section 8). An audit trail
// userland can write is not an audit trail, and a contract an app can issue to
// itself is not a contract.
//
// Reading is not merely permitted, it is REQUIRED: CONTRACT_ARCHITECTURE.md
// section 4 says "a contract nobody can see or withdraw is not a contract", and
// section 8's contract-manager UI is exactly commands 2 and 3 below.
// ---------------------------------------------------------------------------
#define GFS_Q_STATS      0   // -> gfs_stats_t
#define GFS_Q_NODES      1   // -> gfs_node_view_t[]
#define GFS_Q_EDGES_OUT  2   // node=a1 -> gfs_edge_view_t[]   (Q2)
#define GFS_Q_EDGES_IN   3   // node=a1 -> gfs_edge_view_t[]   (Q3)
#define GFS_Q_CHECK      4   // holder=a1 target=a2 scope=a3 -> 1 allow / 0 deny (Q1)

int64_t sys_gfs_query(int cmd, uint32_t a1, uint32_t a2,
                      void *u_out, int out_bytes, uint32_t a3);

#endif // GRAPHFS_FOLD_H
