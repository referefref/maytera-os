// fold.c - GraphFS: the fold, C glue (#711 slice 2).
//
// WHAT THIS FILE IS, AND WHY IT IS C.
// Every rule about what a graph record MEANS lives in rustkern/gfsfold.rs: the
// payload layouts, the escrow check, what a valid scope and expiry are, when a
// grant is live. This file supplies four things Rust cannot reach in this tree
// and nothing else:
//
//   1. the LOCK (sync/spinlock.h, a C macro-free but C-only API),
//   2. the CLOCK (timer_ticks / g_timer_hz, a C extern; the fold reads no clock
//      of its own, deliberately, see design section 6.1),
//   3. the CALL into gfs_journal_append(), which is C because it does file I/O,
//   4. kprintf reporting.
//
// That is the stated justification the Rust-first rule requires, not "the
// surrounding code is C". No policy decision is taken in this file. Where it
// looks like one is (the self-test's expectations), it is an ASSERTION about
// what the Rust policy must answer, which is the point.
//
// THE ONE PATH IN. A mutation here does exactly this:
//
//     build a payload (Rust)  ->  build a pre-record (Rust)
//       ->  gfsf_apply(dry_run=1) under the lock   [refuse costs no journal]
//       ->  gfs_journal_append()                   [NO LOCK HELD: this does I/O]
//       ->  gfs_fold_on_record() -> gfsf_apply(dry_run=0) under the lock
//
// The pre-check and the real apply are THE SAME FUNCTION, so the policy cannot
// drift between them. And because gfs_fold_on_record() is called from inside
// gfs_journal_append(), a record that reached the journal ALWAYS reaches the
// fold: the graph cannot contain anything the audit trail does not.
//
// LOCKING RULE, stated once and obeyed everywhere below: the fold spinlock is
// held ONLY across gfsf_* calls, which are pure memory operations with no I/O,
// no blocking and no re-entry. It is NEVER held across gfs_journal_append().
// This is why holding it cannot trip the wq_assert_may_block() rule, and why the
// known blind spot in that assert (a plain spinlock held across a sleep) is not
// reachable from here.
//
// NO BUSY-WAIT. There is no loop in this file that waits for anything.

#include "fold.h"
#include "journal.h"
#include "../../types.h"
#include "../../string.h"
#include "../../serial.h"
#include "../../sync/spinlock.h"
#include "../../proc/users.h"

// 250 Hz monotonic tick. NOT a wall clock: this kernel has none, which is why
// every expiry below is milliseconds of THIS boot's uptime and why a grant from
// a previous boot generation is unconditionally expired (design section 6.1).
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;

// --- the Rust half ---------------------------------------------------------
extern int32_t  gfsf_apply(const unsigned char *rec, unsigned long rec_len,
                           uint64_t cur_boot_gen, uint32_t dry_run);
extern int32_t  gfsf_grant_check(uint32_t holder, uint32_t target, uint16_t scope,
                                 uint64_t now_ms);
extern int32_t  gfsf_edges(uint32_t node, uint32_t dir, uint64_t now_ms,
                           gfs_edge_view_t *out, uint32_t max);
extern int32_t  gfsf_nodes(gfs_node_view_t *out, uint32_t max);
extern int32_t  gfsf_node_state(uint32_t node_id);
extern int32_t  gfsf_stats(gfs_stats_t *out, uint64_t now_ms);
extern void     gfsf_reset(uint64_t boot_gen, uint32_t degraded);
extern void     gfsf_set_ready(uint32_t ready);
extern void     gfsf_set_degraded(uint32_t reason);
extern uint64_t gfsf_sizes(void);
extern unsigned long gfsf_prerec(unsigned char *out, unsigned long out_len,
                                 uint64_t boot_gen, uint32_t actor_kind,
                                 uint32_t actor_id, uint16_t op,
                                 const unsigned char *payload,
                                 unsigned long payload_len);
extern unsigned long gfsf_enc_node_create(unsigned char *out, unsigned long out_len,
                                          uint32_t node_id, uint16_t kind, uint32_t parent);
extern unsigned long gfsf_enc_node_state(unsigned char *out, unsigned long out_len,
                                         uint32_t node_id, uint16_t reason);
extern unsigned long gfsf_enc_edge_add(unsigned char *out, unsigned long out_len,
                                       uint32_t from, uint32_t to, uint32_t expiry_ms,
                                       uint16_t rel, uint16_t scope);
extern unsigned long gfsf_enc_edge_revoke(unsigned char *out, unsigned long out_len,
                                          uint64_t edge_seq, uint16_t reason);

// Slice 1's append, with the seq of the record it wrote. The seq IS the edge id
// (design section 5.3), so issuing a contract and naming it are one operation.
extern int gfs_journal_append_seq(uint32_t actor_kind, uint32_t actor_id,
                                  uint16_t op, uint16_t effect,
                                  const void *payload, uint32_t payload_len,
                                  uint64_t *out_seq);

// Refusal reasons. MIRRORED from rustkern/gfsfold.rs.
#define GFSF_E_ARG           -1
#define GFSF_E_PAYLOAD_LEN   -2
#define GFSF_E_TABLE_FULL    -3
#define GFSF_E_DUP_NODE      -4
#define GFSF_E_NO_NODE       -5
#define GFSF_E_KIND_MISMATCH -6
#define GFSF_E_NOT_ESCROW    -7
#define GFSF_E_BAD_SCOPE     -8
#define GFSF_E_BAD_EXPIRY    -9
#define GFSF_E_RETIRED      -10
#define GFSF_E_NO_EDGE      -11
#define GFSF_E_BAD_REL      -12
#define GFSF_E_SELF_EDGE    -13
#define GFSF_E_NOT_LIVE     -14
#define GFSF_E_UNSUPPORTED  -15

// Reserved identities used by the boot self-test. They are ordinary nodes with
// no special powers; the point of the self-test is that the ENFORCEMENT PATH is
// exercised on the real machine every boot, and a check nobody has ever seen
// deny is indistinguishable from a constant.
#define GFS_NODE_ST_APP        GFS_NODE_ID(GFS_KIND_APP, 1)
#define GFS_NODE_ST_OBJ        GFS_NODE_ID(GFS_KIND_OBJECT, 1)
#define GFS_NODE_ST_OBJ_DEAD   GFS_NODE_ID(GFS_KIND_OBJECT, 2)
#define GFS_SCOPE_SELFTEST     0x00FF
#define GFS_SCOPE_SELFTEST_2   0x00FE

static spinlock_t g_lock = SPINLOCK_INIT;
static int      g_fold_ready = 0;
static uint64_t g_boot_gen = 0;
static int      g_selftest_pass = 0, g_selftest_fail = 0;

static uint64_t fold_now_ms(void) {
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    return (uint64_t)timer_ticks * 1000ULL / (uint64_t)hz;
}

static const char *fold_err(int rc) {
    switch (rc) {
        case 0:                   return "OK";
        case GFSF_E_ARG:          return "ARG";
        case GFSF_E_PAYLOAD_LEN:  return "PAYLOAD_LEN (does not fit the 16 inline bytes; the blob store is slice 3)";
        case GFSF_E_TABLE_FULL:   return "TABLE_FULL";
        case GFSF_E_DUP_NODE:     return "DUP_NODE";
        case GFSF_E_NO_NODE:      return "NO_NODE";
        case GFSF_E_KIND_MISMATCH:return "KIND_MISMATCH";
        case GFSF_E_NOT_ESCROW:   return "NOT_ESCROW (only the escrow may issue a GRANT)";
        case GFSF_E_BAD_SCOPE:    return "BAD_SCOPE (a contract must name a capability)";
        case GFSF_E_BAD_EXPIRY:   return "BAD_EXPIRY (a contract must state when it ends)";
        case GFSF_E_RETIRED:      return "RETIRED";
        case GFSF_E_NO_EDGE:      return "NO_EDGE";
        case GFSF_E_BAD_REL:      return "BAD_REL";
        case GFSF_E_SELF_EDGE:    return "SELF_EDGE";
        case GFSF_E_NOT_LIVE:     return "NOT_LIVE";
        case GFSF_E_UNSUPPORTED:  return "UNSUPPORTED (VERSION_COMMIT is slice 3)";
        default:                  return "UNKNOWN";
    }
}

int gfs_fold_ready(void) { return g_fold_ready; }

// ---------------------------------------------------------------------------
// Replay: the fold is rebuilt from the exact bytes slice 1 verified
// ---------------------------------------------------------------------------
void gfs_fold_replay(const void *log, uint32_t log_len, uint64_t boot_gen, int degraded) {
    // FFI size lock, same mechanism and the same reason as slice 1's: a silently
    // drifted layout produces answers that LOOK right.
    uint64_t sz = gfsf_sizes();
    if ((sz & 0xFFFF) != sizeof(gfs_node_view_t) ||
        ((sz >> 16) & 0xFFFF) != sizeof(gfs_edge_view_t) ||
        ((sz >> 32) & 0xFFFF) != sizeof(gfs_stats_t)) {
        kprintf("[GFSF] REFUSING to start: Rust/C size mismatch "
                "(node %lu/%d, edge %lu/%d, stats %lu/%d)\n",
                (unsigned long)(sz & 0xFFFF), (int)sizeof(gfs_node_view_t),
                (unsigned long)((sz >> 16) & 0xFFFF), (int)sizeof(gfs_edge_view_t),
                (unsigned long)((sz >> 32) & 0xFFFF), (int)sizeof(gfs_stats_t));
        return;
    }

    g_boot_gen = boot_gen;

    uint64_t flags = spinlock_acquire_irqsave(&g_lock);
    gfsf_reset(boot_gen, degraded ? 1u : 0u);
    uint32_t n = (log && log_len >= GFSJ_REC_SIZE) ? (log_len / GFSJ_REC_SIZE) : 0;
    uint32_t applied = 0, refused = 0;
    const unsigned char *p = (const unsigned char *)log;
    for (uint32_t i = 0; i < n; i++) {
        int rc = gfsf_apply(p + (uint64_t)i * GFSJ_REC_SIZE, GFSJ_REC_SIZE, boot_gen, 0);
        if (rc == 0) applied++;
        else {
            refused++;
            // Loud, but bounded: a hostile journal could hold thousands of bad
            // records and drowning the console is its own denial of service.
            if (refused <= 8)
                kprintf("[GFSF] replay REFUSED record %lu: %s\n",
                        (unsigned long)i, fold_err(rc));
        }
    }
    gfsf_set_ready(1);
    spinlock_release_irqrestore(&g_lock, flags);

    g_fold_ready = 1;
    kprintf("[GFSF] fold replayed %lu record(s) from the verified journal "
            "(%lu applied, %lu refused), boot generation %lu%s\n",
            (unsigned long)n, (unsigned long)applied, (unsigned long)refused,
            (unsigned long)boot_gen, degraded ? ", DEGRADED: every grant check will DENY" : "");
}

void gfs_fold_on_record(const void *rec, uint64_t boot_gen) {
    if (!rec) return;
    uint64_t flags = spinlock_acquire_irqsave(&g_lock);
    int rc = gfsf_apply((const unsigned char *)rec, GFSJ_REC_SIZE, boot_gen, 0);
    spinlock_release_irqrestore(&g_lock, flags);
    if (rc != 0 && rc != GFSF_E_UNSUPPORTED)
        kprintf("[GFSF] the fold REFUSED a record the journal accepted: %s. The "
                "graph and the audit trail now disagree; grant checks stay "
                "conservative.\n", fold_err(rc));
}

// ---------------------------------------------------------------------------
// The one mutation path (see the file header)
// ---------------------------------------------------------------------------
static int fold_mutate(uint32_t actor_kind, uint32_t actor_id, uint16_t op,
                       uint16_t effect, const unsigned char *payload,
                       unsigned long payload_len, uint64_t *out_seq) {
    if (!gfs_journal_ready()) return GFSF_E_NOT_LIVE;

    unsigned char pre[GFSJ_REC_SIZE];
    if (gfsf_prerec(pre, sizeof(pre), g_boot_gen, actor_kind, actor_id, op,
                    payload, payload_len) != GFSJ_REC_SIZE)
        return GFSF_E_ARG;

    // Pre-check under the lock. Same function, same rules, zero journal cost on
    // refusal. Note the lock is NOT held across the append below.
    uint64_t flags = spinlock_acquire_irqsave(&g_lock);
    int rc = gfsf_apply(pre, sizeof(pre), g_boot_gen, 1);
    spinlock_release_irqrestore(&g_lock, flags);
    if (rc != 0) return rc;

    // The append calls gfs_fold_on_record() on success, which is what actually
    // mutates the fold. There is no path that mutates without appending.
    uint64_t seq = 0;
    int arc = gfs_journal_append_seq(actor_kind, actor_id, op, effect,
                                     payload, (uint32_t)payload_len, &seq);
    if (arc != 0) return arc;
    if (out_seq) *out_seq = seq;
    return 0;
}

int gfs_node_create(uint32_t node_id, uint16_t kind, uint32_t parent) {
    unsigned char pl[GFSJ_INLINE_MAX];
    unsigned long n = gfsf_enc_node_create(pl, sizeof(pl), node_id, kind, parent);
    if (!n) return GFSF_E_ARG;
    // Creating an identity is reversible: NODE_RETIRE takes it out of service
    // and the record stays. CONTRACT_ARCHITECTURE.md section 7 requires the
    // class to be declared BEFORE execution, which is what this argument is.
    return fold_mutate(GFSJ_ACTOR_NODE, GFS_NODE_KERNEL, GFSJ_OP_NODE_CREATE,
                       GFSJ_EFFECT_REVERSIBLE, pl, n, 0);
}

static int node_set_state(uint32_t node_id, uint16_t reason, uint16_t op) {
    unsigned char pl[GFSJ_INLINE_MAX];
    unsigned long n = gfsf_enc_node_state(pl, sizeof(pl), node_id, reason);
    if (!n) return GFSF_E_ARG;
    return fold_mutate(GFSJ_ACTOR_NODE, GFS_NODE_KERNEL, op,
                       GFSJ_EFFECT_REVERSIBLE, pl, n, 0);
}

int gfs_node_retire(uint32_t node_id, uint16_t reason) {
    return node_set_state(node_id, reason, GFSJ_OP_NODE_RETIRE);
}
int gfs_node_restore(uint32_t node_id, uint16_t reason) {
    return node_set_state(node_id, reason, GFSJ_OP_NODE_RESTORE);
}

int gfs_grant_issue(uint32_t actor, uint32_t holder, uint32_t target,
                    uint16_t scope, uint32_t ttl_ms, uint64_t *out_edge) {
    // Absolute deadline in THIS boot's uptime. GFS_EXPIRY_NEVER passes through
    // unchanged and is counted as such by gfs_fold_stats().
    uint32_t expiry;
    if (ttl_ms == GFS_EXPIRY_NEVER) {
        expiry = GFS_EXPIRY_NEVER;
    } else if (ttl_ms == GFS_EXPIRY_INVALID) {
        expiry = GFS_EXPIRY_INVALID;   // refused by the fold; kept, not fixed up
    } else {
        uint64_t d = fold_now_ms() + (uint64_t)ttl_ms;
        expiry = (d >= 0xFFFFFFFFull) ? 0xFFFFFFFEu : (uint32_t)d;
    }
    unsigned char pl[GFSJ_INLINE_MAX];
    unsigned long n = gfsf_enc_edge_add(pl, sizeof(pl), holder, target, expiry,
                                        GFS_REL_GRANT, scope);
    if (!n) return GFSF_E_ARG;
    // Issuing authority is REVERSIBLE: revoking it fully undoes it, which is
    // exactly why revocation had to be designed in rather than bolted on.
    return fold_mutate(GFSJ_ACTOR_NODE, actor, GFSJ_OP_EDGE_ADD,
                       GFSJ_EFFECT_REVERSIBLE, pl, n, out_edge);
}

int gfs_grant_revoke(uint32_t actor, uint64_t edge_seq, uint16_t reason) {
    unsigned char pl[GFSJ_INLINE_MAX];
    unsigned long n = gfsf_enc_edge_revoke(pl, sizeof(pl), edge_seq, reason);
    if (!n) return GFSF_E_ARG;
    // Synchronous by design (design section 7): the fold is updated before this
    // returns, so the NEXT check denies. CONTRACT_ARCHITECTURE.md section 6:
    // "a revocation that takes effect eventually is not a revocation".
    return fold_mutate(GFSJ_ACTOR_NODE, actor, GFSJ_OP_EDGE_REVOKE,
                       GFSJ_EFFECT_REVERSIBLE, pl, n, 0);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
int gfs_grant_check(uint32_t holder, uint32_t target, uint16_t scope) {
    uint64_t now = fold_now_ms();
    uint64_t flags = spinlock_acquire_irqsave(&g_lock);
    int r = gfsf_grant_check(holder, target, scope, now);
    spinlock_release_irqrestore(&g_lock, flags);
    return r;
}

int gfs_edges_get(uint32_t node, int dir, gfs_edge_view_t *out, int max) {
    if (!out || max <= 0) return GFSF_E_ARG;
    uint64_t now = fold_now_ms();
    uint64_t flags = spinlock_acquire_irqsave(&g_lock);
    int r = gfsf_edges(node, dir ? 1u : 0u, now, out, (uint32_t)max);
    spinlock_release_irqrestore(&g_lock, flags);
    return r;
}

int gfs_nodes_list(gfs_node_view_t *out, int max) {
    if (!out || max <= 0) return GFSF_E_ARG;
    uint64_t flags = spinlock_acquire_irqsave(&g_lock);
    int r = gfsf_nodes(out, (uint32_t)max);
    spinlock_release_irqrestore(&g_lock, flags);
    return r;
}

int gfs_fold_stats(gfs_stats_t *out) {
    if (!out) return GFSF_E_ARG;
    uint64_t now = fold_now_ms();
    uint64_t flags = spinlock_acquire_irqsave(&g_lock);
    int r = gfsf_stats(out, now);
    spinlock_release_irqrestore(&g_lock, flags);
    return r;
}

// ---------------------------------------------------------------------------
// The Ring-3 surface. READ-ONLY: there is no mutating command and no plan for
// one (design section 8). Everything below produces bytes into a kernel buffer
// and bounces them through copy_to_user; a user pointer is never dereferenced
// here, and the entry validator (rustkern/argtab.rs descriptor 363) has already
// proven `out_bytes` writable bytes at entry.
// ---------------------------------------------------------------------------
extern int copy_to_user(void *dest, const void *src, unsigned long size);

// Bounded so one syscall cannot make the kernel build an arbitrarily large
// stack buffer, and so the fold lock is held for a bounded number of copies.
#define GFS_Q_MAX_ELEMS  32

int64_t sys_gfs_query(int cmd, uint32_t a1, uint32_t a2,
                      void *u_out, int out_bytes, uint32_t a3) {
    if (cmd == GFS_Q_CHECK) {
        // Q1 from Ring 3. This is an ORACLE over the contract graph, not a way
        // to obtain authority: it can only ever report what the enforcement
        // chokepoint would decide. It writes nothing.
        if (a3 > 0xFFFF) return -1;
        return (int64_t)gfs_grant_check(a1, a2, (uint16_t)a3);
    }
    if (!u_out || out_bytes <= 0) return -1;

    if (cmd == GFS_Q_STATS) {
        gfs_stats_t s;
        if (out_bytes < (int)sizeof(s)) return -1;
        if (gfs_fold_stats(&s) != 0) return -1;
        if (copy_to_user(u_out, &s, sizeof(s)) != 0) return -14;
        return (int64_t)sizeof(s);
    }
    if (cmd == GFS_Q_NODES) {
        gfs_node_view_t v[GFS_Q_MAX_ELEMS];
        int max = out_bytes / (int)sizeof(v[0]);
        if (max <= 0) return -1;
        if (max > GFS_Q_MAX_ELEMS) max = GFS_Q_MAX_ELEMS;
        int n = gfs_nodes_list(v, max);
        if (n < 0) return -1;
        if (n && copy_to_user(u_out, v, (unsigned long)n * sizeof(v[0])) != 0) return -14;
        return (int64_t)((unsigned long)n * sizeof(v[0]));
    }
    if (cmd == GFS_Q_EDGES_OUT || cmd == GFS_Q_EDGES_IN) {
        gfs_edge_view_t v[GFS_Q_MAX_ELEMS];
        int max = out_bytes / (int)sizeof(v[0]);
        if (max <= 0) return -1;
        if (max > GFS_Q_MAX_ELEMS) max = GFS_Q_MAX_ELEMS;
        int n = gfs_edges_get(a1, cmd == GFS_Q_EDGES_IN, v, max);
        if (n < 0) return -1;
        if (n && copy_to_user(u_out, v, (unsigned long)n * sizeof(v[0])) != 0) return -14;
        return (int64_t)((unsigned long)n * sizeof(v[0]));
    }
    (void)a2;
    return -1;
}

// ---------------------------------------------------------------------------
// Seeding: the graph's identities are the machine's REAL identities
// ---------------------------------------------------------------------------
static int seed_node(uint32_t id, uint16_t kind, uint32_t parent) {
    uint64_t flags = spinlock_acquire_irqsave(&g_lock);
    int st = gfsf_node_state(id);
    spinlock_release_irqrestore(&g_lock, flags);
    if (st != 0) return 0;   // already present (live or retired): idempotent
    int rc = gfs_node_create(id, kind, parent);
    if (rc != 0)
        kprintf("[GFSF] could not seed node %08lx kind %u: %s\n",
                (unsigned long)id, (unsigned)kind, fold_err(rc));
    return rc;
}

// A dry-run mutation: validates against the real policy and writes NOTHING.
// This is how the self-test proves a refusal fires without spending a journal
// record on every boot for every rule.
static int dry_edge_add(uint32_t actor, uint32_t from, uint32_t to,
                        uint32_t expiry, uint16_t rel, uint16_t scope,
                        unsigned long forced_plen) {
    unsigned char pl[GFSJ_INLINE_MAX];
    unsigned long n = gfsf_enc_edge_add(pl, sizeof(pl), from, to, expiry, rel, scope);
    if (!n) return GFSF_E_ARG;
    if (forced_plen) n = forced_plen;
    unsigned char pre[GFSJ_REC_SIZE];
    if (gfsf_prerec(pre, sizeof(pre), g_boot_gen, GFSJ_ACTOR_NODE, actor,
                    GFSJ_OP_EDGE_ADD, pl, n) != GFSJ_REC_SIZE)
        return GFSF_E_ARG;
    uint64_t flags = spinlock_acquire_irqsave(&g_lock);
    int rc = gfsf_apply(pre, sizeof(pre), g_boot_gen, 1);
    spinlock_release_irqrestore(&g_lock, flags);
    return rc;
}

static void st_check(const char *name, int got, int want) {
    if (got == want) {
        g_selftest_pass++;
    } else {
        g_selftest_fail++;
        kprintf("[GFSF] SELFTEST FAIL %s: got %d (%s), expected %d (%s)\n",
                name, got, fold_err(got), want, fold_err(want));
    }
}

// ---------------------------------------------------------------------------
// The boot enforcement self-test.
//
// WHY IT RUNS IN THE SHIPPED KERNEL AND NOT BEHIND A FLAG. A check that has only
// ever been seen to pass is indistinguishable from a constant, and this codebase
// has shipped eleven security functions with no callers at all. Every boot,
// against the real fold, on the real machine, this proves that:
//
//   * a grant check DENIES before any grant exists,
//   * seven distinct refusal rules actually refuse,
//   * a real, escrow-issued contract ALLOWS,
//   * the same contract denies for a different scope (scope-limited),
//   * it denies in the reverse direction (directional: B gains nothing),
//   * it denies at a time past its expiry (time-limited),
//   * and after a REVOKE the very next check denies (revocation bites).
//
// Cost: TWO journal records per boot (the grant and its revoke). The refusals
// are dry runs and cost nothing. The records are honest data in an append-only
// audit trail and are labelled with a reserved scope, which is the only way this
// evidence could be trustworthy: a self-test that hid its own writes from the
// audit trail would be arguing against the trail's whole purpose.
//
// A FAILURE MARKS THE STORE DEGRADED, which makes every grant check deny. If the
// enforcement path is broken, denying everything is the only honest answer.
// ---------------------------------------------------------------------------
static void fold_selftest(void) {
    const uint32_t A = GFS_NODE_ST_APP, O = GFS_NODE_ST_OBJ, D = GFS_NODE_ST_OBJ_DEAD;
    const uint16_t S = GFS_SCOPE_SELFTEST;
    uint64_t edge = 0;

    // 1. Nothing is granted yet. This is the RED baseline the rest is measured
    //    against; without it "ALLOW" proves nothing.
    st_check("deny-before-grant", gfs_grant_check(A, O, S), 0);

    // 2-8. Seven refusal rules, dry run, zero journal cost.
    st_check("refuse-not-escrow",
             dry_edge_add(A, A, O, GFS_EXPIRY_NEVER, GFS_REL_GRANT, S, 0), GFSF_E_NOT_ESCROW);
    st_check("refuse-bad-scope",
             dry_edge_add(GFS_NODE_ESCROW, A, O, GFS_EXPIRY_NEVER, GFS_REL_GRANT,
                          GFS_SCOPE_INVALID, 0), GFSF_E_BAD_SCOPE);
    st_check("refuse-bad-expiry",
             dry_edge_add(GFS_NODE_ESCROW, A, O, GFS_EXPIRY_INVALID, GFS_REL_GRANT, S, 0),
             GFSF_E_BAD_EXPIRY);
    st_check("refuse-unknown-node",
             dry_edge_add(GFS_NODE_ESCROW, A, GFS_NODE_ID(GFS_KIND_OBJECT, 0x777777),
                          GFS_EXPIRY_NEVER, GFS_REL_GRANT, S, 0), GFSF_E_NO_NODE);
    st_check("refuse-self-edge",
             dry_edge_add(GFS_NODE_ESCROW, A, A, GFS_EXPIRY_NEVER, GFS_REL_GRANT, S, 0),
             GFSF_E_SELF_EDGE);
    st_check("refuse-retired-target",
             dry_edge_add(GFS_NODE_ESCROW, A, D, GFS_EXPIRY_NEVER, GFS_REL_GRANT, S, 0),
             GFSF_E_RETIRED);
    // The rule that keeps slice 2 independent of the never-executed blob store:
    // a graph payload that does not fit the 16 inline bytes is REFUSED, not
    // silently folded from bytes the fold cannot see.
    st_check("refuse-oversize-payload",
             dry_edge_add(GFS_NODE_ESCROW, A, O, GFS_EXPIRY_NEVER, GFS_REL_GRANT, S, 32),
             GFSF_E_PAYLOAD_LEN);

    // 9. A real contract, issued by the escrow, through the real chokepoint.
    //    This is journal record 1 of 2.
    int rc = gfs_grant_issue(GFS_NODE_ESCROW, A, O, S, 3600000u, &edge);
    st_check("issue-grant", rc, 0);
    if (rc != 0) goto done;

    st_check("allow-in-scope",   gfs_grant_check(A, O, S), 1);
    st_check("deny-wrong-scope", gfs_grant_check(A, O, GFS_SCOPE_SELFTEST_2), 0);
    st_check("deny-reverse-direction", gfs_grant_check(O, A, S), 0);

    // 13. Time-limited, proven with no sleep: ask the SAME live edge at a time
    //     an hour past its expiry. This calls the Rust check directly with a
    //     chosen `now`, which is the existing signature, not a new API and not
    //     reachable from Ring 3. It can only ever make the check deny MORE.
    {
        uint64_t future = fold_now_ms() + 7200000ull;
        uint64_t flags = spinlock_acquire_irqsave(&g_lock);
        int r = gfsf_grant_check(A, O, S, future);
        spinlock_release_irqrestore(&g_lock, flags);
        st_check("deny-after-expiry", r, 0);
    }

    // 14. A non-escrow revoke must be refused too, or revocation becomes a
    //     denial-of-service any app could aim at any contract.
    {
        unsigned char pl[GFSJ_INLINE_MAX];
        unsigned long n = gfsf_enc_edge_revoke(pl, sizeof(pl), edge, 1);
        unsigned char pre[GFSJ_REC_SIZE];
        gfsf_prerec(pre, sizeof(pre), g_boot_gen, GFSJ_ACTOR_NODE, A,
                    GFSJ_OP_EDGE_REVOKE, pl, n);
        uint64_t flags = spinlock_acquire_irqsave(&g_lock);
        int r = gfsf_apply(pre, sizeof(pre), g_boot_gen, 1);
        spinlock_release_irqrestore(&g_lock, flags);
        st_check("refuse-revoke-not-escrow", r, GFSF_E_NOT_ESCROW);
    }

    // 15-16. Revoke for real (journal record 2 of 2), and the very next check
    //        must deny.
    st_check("revoke-grant", gfs_grant_revoke(GFS_NODE_ESCROW, edge, 1), 0);
    st_check("deny-after-revoke", gfs_grant_check(A, O, S), 0);

done:
    if (g_selftest_fail) {
        kprintf("[GFSF] SELFTEST FAILED: %d/%d checks. The enforcement path is "
                "NOT trustworthy on this build, so the fold is marked DEGRADED "
                "and every grant check will DENY.\n",
                g_selftest_pass, g_selftest_pass + g_selftest_fail);
        uint64_t flags = spinlock_acquire_irqsave(&g_lock);
        gfsf_set_degraded(1);
        spinlock_release_irqrestore(&g_lock, flags);
    } else {
        gfs_stats_t sx;
        (void)gfs_fold_stats(&sx);
        kprintf("[GFSF] selftest: %d/%d - deny before grant, 8 refusal rules, "
                "allow in scope, deny out of scope, deny reversed, deny expired, "
                "deny after revoke [checks=%lu allow=%lu deny=%lu refused: "
                "not-escrow=%lu bad-scope=%lu bad-expiry=%lu]\n",
                g_selftest_pass, g_selftest_pass,
                (unsigned long)sx.checks, (unsigned long)sx.allows,
                (unsigned long)sx.denies, (unsigned long)sx.refused_not_escrow,
                (unsigned long)sx.refused_bad_scope,
                (unsigned long)sx.refused_bad_expiry);
    }
}

// ---------------------------------------------------------------------------
// Bring the fold up. Called from main.c after gfs_journal_init().
// ---------------------------------------------------------------------------
void gfs_fold_init(void) {
    if (!g_fold_ready || !gfs_journal_ready()) {
        kprintf("[GFSF] not starting: the journal is not up, so there is no "
                "record stream to fold and no way to record a mutation\n");
        return;
    }

    // The system identities. The escrow is the only GRANT issuer
    // (CONTRACT_ARCHITECTURE.md section 5); it exists as a node so that the
    // check "was this issued by the escrow" has a real subject to compare.
    seed_node(GFS_NODE_KERNEL, GFS_KIND_SYSTEM, 0);
    seed_node(GFS_NODE_ESCROW, GFS_KIND_SYSTEM, 0);

    // The self-test's fixtures. GFS_NODE_ST_OBJ_DEAD is created and immediately
    // retired ONCE, on the first boot that has a fold; it stays retired in every
    // later replay, which is what lets the retired-target rule be proven on
    // every boot at zero journal cost.
    seed_node(GFS_NODE_ST_APP, GFS_KIND_APP, 0);
    seed_node(GFS_NODE_ST_OBJ, GFS_KIND_OBJECT, 0);
    {
        uint64_t flags = spinlock_acquire_irqsave(&g_lock);
        int st = gfsf_node_state(GFS_NODE_ST_OBJ_DEAD);
        spinlock_release_irqrestore(&g_lock, flags);
        if (st == 0) {
            if (gfs_node_create(GFS_NODE_ST_OBJ_DEAD, GFS_KIND_OBJECT, 0) == 0)
                (void)gfs_node_retire(GFS_NODE_ST_OBJ_DEAD, 1);
        }
    }

    // THE REAL IDENTITIES. CONTRACT_ARCHITECTURE.md section 3 names users as a
    // first-class identity class, and this machine already has a user database,
    // so the USER nodes in the graph are the actual accounts rather than
    // invented ones. users_init() runs well before this (main.c), so the list is
    // populated. A user created LATER gets its node from user_create(), which
    // calls gfs_node_create() directly; this loop is what covers the accounts
    // that already existed the first time a fold ever ran.
    {
        int nu = 0;
        user_entry_t *all = users_all(&nu);
        int seeded = 0;
        for (int i = 0; all && i < nu; i++) {
            if (!all[i].active) continue;
            if (seed_node(GFS_NODE_ID(GFS_KIND_USER, all[i].uid), GFS_KIND_USER, 0) == 0)
                seeded++;
        }
        kprintf("[GFSF] identities: %d user account(s) present in the graph\n", seeded);
    }

    fold_selftest();

    gfs_stats_t s;
    if (gfs_fold_stats(&s) == 0)
        kprintf("[GFSF] up: %lu node(s), %lu edge(s), %lu live grant(s) "
                "(%lu unlimited-scope, %lu never-expire), %lu record(s) folded, "
                "%lu refused, %lu prior-boot grant(s) expired\n",
                (unsigned long)s.nodes, (unsigned long)s.edges,
                (unsigned long)s.grants_live,
                (unsigned long)s.grants_unlimited_scope,
                (unsigned long)s.grants_never_expire,
                (unsigned long)s.applied, (unsigned long)s.rejected,
                (unsigned long)s.grants_prior_boot);
}
