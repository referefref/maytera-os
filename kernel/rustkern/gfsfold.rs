// rustkern/gfsfold.rs - GraphFS: the FOLD (#711 slice 2).
//
// WHAT THIS IS.
// docs/GRAPHFS_DESIGN.md section 5.5: node creation, edge add, edge revoke and
// node retire/restore are NOT separate APIs with separate storage. They are
// record types in the one append-only journal, and the in-memory graph is a
// FOLD of that record stream. This file is that fold, and it is the ONLY thing
// in the kernel that holds graph state.
//
// That single sentence is the entire security argument for CONTRACT_ARCHITECTURE
// section 12.9 ("no bypass path"), and it is obtained by CONSTRUCTION, not by
// discipline: there is exactly one function that changes state, `gfsf_apply`,
// and its only argument is a 160-byte journal record. There is no
// "write the node table" entry point to forget about, because there is no node
// table API. If a mutation is not in the journal it did not happen, and if it is
// in the journal it is chained, sealed and anchored (slice 1).
//
// ONE RULE SET, TWO CALLERS. `gfsf_apply` is reached from two places and must
// behave identically in both:
//   * REPLAY at boot, over bytes read off the disk. Those bytes are hostile
//     input by definition (the whole point of slice 1 is that someone may have
//     edited them), so every rule below is enforced against them.
//   * LIVE, immediately after the C glue appended a record it just built.
// The C glue pre-checks a mutation by calling `gfsf_apply` with `dry_run=1`, so
// a refusal costs no journal space, and the accept path is the SAME CODE. Two
// copies of a policy drift; this one cannot.
//
// WHY RUST. Standing rule (2026-07-16), and on the merits: this parses attacker
// -writable bytes and answers an authorization question. Every read below goes
// through a slice over an exactly-known extent, so a malformed record can only
// produce a REJECT.
//
// NO FLOATS (MEASURED, design section 2: -mno-sse marshals floats through the
// x87 FPU and this kernel never saves x87 state across a context switch).
// NO ALLOCATION: fixed tables in .bss and caller-supplied out-arrays, so the
// hot path (Q1) can be answered from any context without touching the heap.
//
// LOCKING IS THE C SIDE'S JOB and is documented at every entry point here: the
// caller holds fs/graphfs/fold.c's spinlock across every call into this module.
// The lock is never held across I/O, because nothing here does I/O.

// ===========================================================================
// Record layout. MIRRORS rustkern/gfsjournal.rs, which owns the format; this
// module only READS records, it never encodes one.
// ===========================================================================
const REC_SIZE: usize = 160;
const O_SEQ: usize = 8;
const O_BOOTGEN: usize = 16;
const O_ACTKIND: usize = 32;
const O_ACTID: usize = 36;
const O_OP: usize = 40;
const O_PLEN: usize = 44;
const O_INLINE: usize = 112;
const INLINE_MAX: usize = 16;

// Ops. MIRRORED in fs/graphfs/journal.h, where the numbering was fixed in
// slice 1 precisely so slice 2 could not renumber it.
const OP_NODE_CREATE: u16 = 16;
const OP_VERSION_COMMIT: u16 = 17;
const OP_EDGE_ADD: u16 = 18;
const OP_EDGE_REVOKE: u16 = 19;
const OP_NODE_RETIRE: u16 = 20;
const OP_NODE_RESTORE: u16 = 21;
/// Ops at or above this are graph mutations and are folded. Below it are the
/// slice-1 journal/system records (BOOT, AUDIT, CHECKPOINT), which the fold
/// counts and ignores.
const OP_GRAPH_FIRST: u16 = 16;

// Actor kinds. MIRRORED in fs/graphfs/journal.h.
const ACTOR_NODE: u32 = 2;

// ===========================================================================
// The model (design section 5)
// ===========================================================================

/// Node kinds. CONTRACT_ARCHITECTURE section 3: identities are plural, and an
/// AI task identity is one of them, per task, non-negotiably.
///
/// A node id is `(kind << 24) | local`, so the kind is DERIVABLE from the id and
/// the fold cross-checks the two on every NODE_CREATE. That is a free integrity
/// check, and it is also why a node needs no stored name: a producer that can
/// compute a stable `local` (a uid, a task counter) can recompute the node id
/// without the graph having to store any string. Design section 5.2 says a node
/// carries no path and no description; this is how it manages that and stays
/// addressable.
pub const KIND_SYSTEM: u32 = 1;
pub const KIND_USER: u32 = 2;
pub const KIND_APP: u32 = 3;
pub const KIND_SERVICE: u32 = 4;
pub const KIND_DEVICE: u32 = 5;
pub const KIND_AI_TASK: u32 = 6;
pub const KIND_OBJECT: u32 = 7;
pub const KIND_CONTRACT_DEF: u32 = 8;
const KIND_MAX: u32 = 8;

/// The escrow principal (CONTRACT_ARCHITECTURE section 5). It is the ONLY
/// identity permitted to append an EDGE_ADD with rel=GRANT. An app that can
/// grant itself a contract is theatre, so this is enforced at the fold, on both
/// the live path and the replay path.
pub const NODE_KERNEL: u32 = (KIND_SYSTEM << 24) | 1;
pub const NODE_ESCROW: u32 = (KIND_SYSTEM << 24) | 2;

/// Relations. An integer compare on the enforcement hot path, never a string
/// (design section 5.3).
pub const REL_GRANT: u16 = 1; // THIS is a contract
pub const REL_NAME: u16 = 2;
pub const REL_DERIVED_FROM: u16 = 3;
pub const REL_CONFIG_FOR: u16 = 4;
const REL_MAX: u16 = 4;

/// Scope 0 is INVALID, not "any". CONTRACT_ARCHITECTURE section 4 requires a
/// contract to be scope-limited; a zeroed payload must therefore be incapable of
/// producing authority. `SCOPE_ANY` is a deliberate, loud, COUNTED value
/// (section 10: an unlimited contract must be visibly expensive), never the
/// default that a forgotten field falls into.
pub const SCOPE_INVALID: u16 = 0;
pub const SCOPE_ANY: u16 = 0xFFFF;

/// Likewise expiry: 0 is INVALID (a grant must state when it ends) and
/// `EXPIRY_NEVER` is explicit and counted.
pub const EXPIRY_INVALID: u32 = 0;
pub const EXPIRY_NEVER: u32 = 0xFFFF_FFFF;

/// Node states.
const STATE_LIVE: u16 = 0;
const STATE_RETIRED: u16 = 1;

// Refusal reasons, returned NEGATIVE. Each one is a rule the design states, and
// each is counted, so "the check never fires" is distinguishable from "the check
// is not wired in" (the recurring trap in blame.md).
pub const E_ARG: i32 = -1;
pub const E_PAYLOAD_LEN: i32 = -2; // graph op whose payload does not fit inline
pub const E_TABLE_FULL: i32 = -3;
pub const E_DUP_NODE: i32 = -4;
pub const E_NO_NODE: i32 = -5;
pub const E_KIND_MISMATCH: i32 = -6;
pub const E_NOT_ESCROW: i32 = -7; // only the escrow may issue a GRANT
pub const E_BAD_SCOPE: i32 = -8;
pub const E_BAD_EXPIRY: i32 = -9;
pub const E_RETIRED: i32 = -10;
pub const E_NO_EDGE: i32 = -11;
pub const E_BAD_REL: i32 = -12;
pub const E_SELF_EDGE: i32 = -13;
pub const E_NOT_LIVE: i32 = -14; // fold is not ready / is degraded
pub const E_UNSUPPORTED: i32 = -15; // VERSION_COMMIT: slice 3, see below

// ===========================================================================
// Capacity.
//
// Fixed and modest, in .bss, because Q1 must answer with no allocation. An
// OVERFLOW is not a soft condition: it means the fold is no longer a faithful
// image of the journal, and a fold that DROPPED A REVOKE would fail OPEN. So
// overflow is sticky and makes Q1 deny EVERYTHING (fail closed, section 12.7).
// ===========================================================================
const NODE_CAP: usize = 512;
const EDGE_CAP: usize = 1024;

#[derive(Clone, Copy)]
struct Node {
    node_id: u32,
    kind: u16,
    state: u16,
    parent: u32,
    born_seq: u64,
    born_boot: u64,
}

#[derive(Clone, Copy)]
struct Edge {
    edge_seq: u64,    // the seq of its EDGE_ADD record: its identity, unforgeable
    revoked_seq: u64, // 0 = live
    boot_gen: u64,
    expiry_ms: u32,
    from: u32,
    to: u32,
    rel: u16,
    scope: u16,
}

struct Fold {
    ready: u32,
    degraded: u32,
    overflow: u32,
    boot_gen: u64,

    nodes: [Node; NODE_CAP],
    n_nodes: usize,
    edges: [Edge; EDGE_CAP],
    n_edges: usize,

    // Counters. These exist so that a gate which never fires is visibly
    // different from a gate which is not wired in.
    records_seen: u64,
    applied: u64,
    rejected: u64,
    checks: u64,
    allows: u64,
    denies: u64,
    grants_prior_boot: u64, // expired by section 6.1: no trusted clock
    refused_not_escrow: u32,
    refused_bad_scope: u32,
    refused_bad_expiry: u32,
}

const NODE_ZERO: Node = Node {
    node_id: 0,
    kind: 0,
    state: 0,
    parent: 0,
    born_seq: 0,
    born_boot: 0,
};
const EDGE_ZERO: Edge = Edge {
    edge_seq: 0,
    revoked_seq: 0,
    boot_gen: 0,
    expiry_ms: 0,
    from: 0,
    to: 0,
    rel: 0,
    scope: 0,
};

static mut FOLD: Fold = Fold {
    ready: 0,
    degraded: 0,
    overflow: 0,
    boot_gen: 0,
    nodes: [NODE_ZERO; NODE_CAP],
    n_nodes: 0,
    edges: [EDGE_ZERO; EDGE_CAP],
    n_edges: 0,
    records_seen: 0,
    applied: 0,
    rejected: 0,
    checks: 0,
    allows: 0,
    denies: 0,
    grants_prior_boot: 0,
    refused_not_escrow: 0,
    refused_bad_scope: 0,
    refused_bad_expiry: 0,
};

/// SAFETY: every caller holds fs/graphfs/fold.c's spinlock across the whole
/// call, which is the documented contract at each `#[no_mangle]` entry point
/// below. Nothing here does I/O, blocks, or re-enters, so the lock is held for a
/// bounded number of memory operations only.
#[inline]
#[allow(clippy::mut_from_ref)]
unsafe fn fold() -> &'static mut Fold {
    unsafe { &mut *core::ptr::addr_of_mut!(FOLD) }
}

// ===========================================================================
// Little-endian readers over a bounds-checked slice
// ===========================================================================
#[inline]
fn rd_u16(s: &[u8], o: usize) -> u16 {
    (s[o] as u16) | ((s[o + 1] as u16) << 8)
}
#[inline]
fn rd_u32(s: &[u8], o: usize) -> u32 {
    (s[o] as u32) | ((s[o + 1] as u32) << 8) | ((s[o + 2] as u32) << 16) | ((s[o + 3] as u32) << 24)
}
#[inline]
fn rd_u64(s: &[u8], o: usize) -> u64 {
    let mut v: u64 = 0;
    let mut i = 8;
    while i > 0 {
        i -= 1;
        v = (v << 8) | (s[o + i] as u64);
    }
    v
}

// ===========================================================================
// Views handed to C (and, through one read-only syscall, to Ring 3).
// #[repr(C)]; sizes locked by _Static_assert on the C side against gfsf_sizes().
// ===========================================================================

#[repr(C)]
#[derive(Clone, Copy)]
pub struct GfsNodeView {
    pub node_id: u32,
    pub kind: u16,
    pub state: u16,
    pub parent: u32,
    pub pad: u32,
    pub born_seq: u64,
    pub born_boot: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct GfsEdgeView {
    pub edge_seq: u64,
    pub revoked_seq: u64,
    pub boot_gen: u64,
    pub expiry_ms: u32,
    pub from: u32,
    pub to: u32,
    pub rel: u16,
    pub scope: u16,
    pub flags: u32, // bit 0: expired by wall time. bit 1: revoked.
    pub pad: u32,
}
pub const EDGEFLAG_EXPIRED: u32 = 1;
pub const EDGEFLAG_REVOKED: u32 = 2;

/// Counters, including the two that CONTRACT_ARCHITECTURE section 10 demands be
/// countable: grants with unlimited scope and grants that never expire. "Make
/// the wrong thing visibly expensive" needs a number to put in front of people.
#[repr(C)]
pub struct GfsStats {
    pub ready: u32,
    pub degraded: u32,
    pub overflow: u32,
    pub nodes: u32,
    pub edges: u32,
    pub node_cap: u32,
    pub edge_cap: u32,
    pub grants_live: u32,
    pub grants_revoked: u32,
    pub grants_expired: u32,
    pub grants_unlimited_scope: u32,
    pub grants_never_expire: u32,
    pub refused_not_escrow: u32,
    pub refused_bad_scope: u32,
    pub refused_bad_expiry: u32,
    pub pad: u32,
    pub boot_gen: u64,
    pub records_seen: u64,
    pub applied: u64,
    pub rejected: u64,
    pub checks: u64,
    pub allows: u64,
    pub denies: u64,
    pub grants_prior_boot: u64,
}

// ===========================================================================
// Table helpers
// ===========================================================================
fn find_node(f: &Fold, id: u32) -> Option<usize> {
    let mut i = 0;
    while i < f.n_nodes {
        if f.nodes[i].node_id == id {
            return Some(i);
        }
        i += 1;
    }
    None
}

fn find_edge(f: &Fold, seq: u64) -> Option<usize> {
    let mut i = 0;
    while i < f.n_edges {
        if f.edges[i].edge_seq == seq {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// A node may take part in a contract only if it EXISTS and is LIVE. Retiring an
/// identity therefore kills every contract it holds or is a target of, without
/// any separate cascade code: Q1 simply cannot find a live endpoint.
fn node_live(f: &Fold, id: u32) -> bool {
    match find_node(f, id) {
        Some(i) => f.nodes[i].state == STATE_LIVE,
        None => false,
    }
}

// ===========================================================================
// The fold itself
// ===========================================================================

/// Apply one 160-byte journal record to the fold.
///
/// `cur_boot_gen` is THIS boot's generation. `dry_run != 0` validates without
/// mutating, which is how the C glue refuses an illegal mutation BEFORE it costs
/// journal space, using this exact code and therefore this exact policy.
///
/// Returns 0 if the record was applied (or was a non-graph record, which is not
/// an error), or a negative `E_*` if the fold REFUSED it.
///
/// ## The rule that keeps slice 2 independent of the blob store
/// A graph record's payload must fit in the record's 16 inline bytes. Longer
/// payloads are content-addressed into `blob.c` (design section 6.2), which has
/// SEVEN measured defects and has never executed (section 11). Depending on it
/// here would make "the fold works" contingent on "the blob store works", which
/// is exactly the ordering slice 1 refused. So a graph record with
/// `payload_len > 16` is REFUSED, loudly, rather than silently folded from a
/// payload the fold cannot see. That is why `VERSION_COMMIT` (whose payload is a
/// 32-byte content hash) returns E_UNSUPPORTED here and is slice 3's job, not a
/// half-built version chain.
///
/// # Safety
/// `rec` must point to at least `rec_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsf_apply(
    rec: *const u8,
    rec_len: usize,
    cur_boot_gen: u64,
    dry_run: u32,
) -> i32 {
    if rec.is_null() || rec_len < REC_SIZE {
        return E_ARG;
    }
    // SAFETY: the caller guarantees `rec_len` readable bytes and we only ever
    // look at the first REC_SIZE of them, through this slice.
    let r: &[u8] = unsafe { core::slice::from_raw_parts(rec, REC_SIZE) };
    // SAFETY: see fold(). The caller holds the fold spinlock.
    let f = unsafe { fold() };

    let op = rd_u16(r, O_OP);
    if dry_run == 0 {
        f.records_seen += 1;
    }
    if op < OP_GRAPH_FIRST {
        // BOOT / AUDIT / CHECKPOINT. Counted, not folded.
        return 0;
    }

    let seq = rd_u64(r, O_SEQ);
    let boot_gen = rd_u64(r, O_BOOTGEN);
    let actor_kind = rd_u32(r, O_ACTKIND);
    let actor_id = rd_u32(r, O_ACTID);
    let plen = rd_u32(r, O_PLEN) as usize;
    let p: &[u8] = &r[O_INLINE..O_INLINE + INLINE_MAX];

    if plen > INLINE_MAX {
        if dry_run == 0 {
            f.rejected += 1;
        }
        return E_PAYLOAD_LEN;
    }

    let rc = match op {
        OP_NODE_CREATE => apply_node_create(f, p, plen, seq, boot_gen, dry_run),
        OP_NODE_RETIRE => apply_node_state(f, p, plen, STATE_RETIRED, dry_run),
        OP_NODE_RESTORE => apply_node_state(f, p, plen, STATE_LIVE, dry_run),
        OP_EDGE_ADD => apply_edge_add(
            f,
            p,
            plen,
            seq,
            boot_gen,
            cur_boot_gen,
            actor_kind,
            actor_id,
            dry_run,
        ),
        OP_EDGE_REVOKE => apply_edge_revoke(f, p, plen, seq, actor_kind, actor_id, dry_run),
        OP_VERSION_COMMIT => E_UNSUPPORTED,
        _ => E_ARG,
    };

    if dry_run == 0 {
        if rc == 0 {
            f.applied += 1;
        } else {
            f.rejected += 1;
        }
    }
    rc
}

// --- NODE_CREATE ------------------------------------------------------------
// payload (12 bytes): node_id u32 | kind u16 | state u16 | parent u32
fn apply_node_create(
    f: &mut Fold,
    p: &[u8],
    plen: usize,
    seq: u64,
    boot_gen: u64,
    dry_run: u32,
) -> i32 {
    if plen < 12 {
        return E_PAYLOAD_LEN;
    }
    let node_id = rd_u32(p, 0);
    let kind = rd_u16(p, 4);
    let parent = rd_u32(p, 8);

    if node_id == 0 || kind == 0 || kind as u32 > KIND_MAX {
        return E_ARG;
    }
    // The kind is derivable from the id (see KIND_* above). Disagreement means
    // the record was built wrong or edited, so refuse it rather than store a
    // node whose id lies about what it is.
    if (node_id >> 24) != kind as u32 {
        return E_KIND_MISMATCH;
    }
    if find_node(f, node_id).is_some() {
        return E_DUP_NODE;
    }
    // A stated parent must exist. It is how an AI task identity records the
    // identity that spawned it (section 3), and a dangling parent would make
    // "what did this task's owner authorise" unanswerable.
    if parent != 0 && find_node(f, parent).is_none() {
        return E_NO_NODE;
    }
    if f.n_nodes >= NODE_CAP {
        if dry_run == 0 {
            f.overflow = 1;
        }
        return E_TABLE_FULL;
    }
    if dry_run != 0 {
        return 0;
    }
    let i = f.n_nodes;
    f.nodes[i] = Node {
        node_id,
        kind,
        state: STATE_LIVE,
        parent,
        born_seq: seq,
        born_boot: boot_gen,
    };
    f.n_nodes += 1;
    0
}

// --- NODE_RETIRE / NODE_RESTORE ---------------------------------------------
// payload (8 bytes): node_id u32 | reason u16 | pad u16
//
// Append-only, so "delete" can only ever be a state change (design, disposition
// #7). The record stays; the identity stops being usable.
fn apply_node_state(f: &mut Fold, p: &[u8], plen: usize, state: u16, dry_run: u32) -> i32 {
    if plen < 8 {
        return E_PAYLOAD_LEN;
    }
    let node_id = rd_u32(p, 0);
    let idx = match find_node(f, node_id) {
        Some(i) => i,
        None => return E_NO_NODE,
    };
    if dry_run != 0 {
        return 0;
    }
    f.nodes[idx].state = state;
    0
}

// --- EDGE_ADD ---------------------------------------------------------------
// payload (16 bytes): from u32 | to u32 | expiry_ms u32 | rel u16 | scope u16
//
// A CONTRACT IS AN EDGE WITH rel=GRANT (design section 5.3). Check section 4's
// four required properties against that payload:
//   explicit        the edge had to be appended by the escrow, and is listable
//   directional     from -> to, and ONLY that direction. B gains nothing.
//   scope-limited   `scope`, which cannot be 0
//   time-limited    `expiry_ms`, which cannot be 0
// and the two that follow necessarily:
//   revocable       revoked_seq, written by an EDGE_REVOKE record
//   auditable       it IS a journal record; there is no other way to make one
//
// `edge_seq` is NOT in the payload: it is the seq of this record, so an edge's
// identity is its position in the tamper-evident chain and cannot be forged or
// duplicated. Likewise `granted_seq`. That is why the payload fits in 16 bytes
// at all (see gfsf_apply's doc comment).
#[allow(clippy::too_many_arguments)]
fn apply_edge_add(
    f: &mut Fold,
    p: &[u8],
    plen: usize,
    seq: u64,
    boot_gen: u64,
    cur_boot_gen: u64,
    actor_kind: u32,
    actor_id: u32,
    dry_run: u32,
) -> i32 {
    if plen < 16 {
        return E_PAYLOAD_LEN;
    }
    let from = rd_u32(p, 0);
    let to = rd_u32(p, 4);
    let expiry_ms = rd_u32(p, 8);
    let rel = rd_u16(p, 12);
    let scope = rd_u16(p, 14);

    if rel == 0 || rel > REL_MAX {
        return E_BAD_REL;
    }
    if from == 0 || to == 0 {
        return E_ARG;
    }
    if from == to {
        return E_SELF_EDGE;
    }

    if rel == REL_GRANT {
        // CONTRACT_ARCHITECTURE section 5: the escrow is the only issuer. This
        // is enforced HERE, on the replay path as well as the live path, so a
        // record that somehow reached the log without escrow authority still
        // does not become authority.
        // The refusal counters increment on the DRY RUN TOO, deliberately. The
        // dry run is the path that actually protects: it refuses the mutation
        // before it costs a journal record. Counting only the accepted-then-
        // rejected path would leave every genuine refusal invisible, and a
        // counter that reads 0 while the rule fires every boot is exactly the
        // "the check is not wired in" ambiguity these counters exist to remove.
        // MEASURED: with the counters guarded by dry_run they read 0 on a boot
        // whose self-test had just proven all three rules firing.
        if actor_kind != ACTOR_NODE || actor_id != NODE_ESCROW {
            f.refused_not_escrow += 1;
            return E_NOT_ESCROW;
        }
        if scope == SCOPE_INVALID {
            f.refused_bad_scope += 1;
            return E_BAD_SCOPE;
        }
        if expiry_ms == EXPIRY_INVALID {
            f.refused_bad_expiry += 1;
            return E_BAD_EXPIRY;
        }
    }

    // Both endpoints must be live identities. This is what makes retiring an
    // identity revoke its contracts with no cascade code.
    if !node_live(f, from) || !node_live(f, to) {
        return if find_node(f, from).is_some() && find_node(f, to).is_some() {
            E_RETIRED
        } else {
            E_NO_NODE
        };
    }

    // Section 6.1, the honest consequence of having NO WALL CLOCK: an expiry is
    // milliseconds of THIS boot's uptime, so a grant from a previous boot
    // generation cannot be evaluated and is unconditionally expired. It is
    // counted and DROPPED rather than stored: keeping it would occupy a table
    // slot forever and could never answer anything but "denied".
    if boot_gen != cur_boot_gen {
        if dry_run == 0 && rel == REL_GRANT {
            f.grants_prior_boot += 1;
        }
        return 0;
    }

    if f.n_edges >= EDGE_CAP {
        if dry_run == 0 {
            f.overflow = 1;
        }
        return E_TABLE_FULL;
    }
    if dry_run != 0 {
        return 0;
    }
    let i = f.n_edges;
    f.edges[i] = Edge {
        edge_seq: seq,
        revoked_seq: 0,
        boot_gen,
        expiry_ms,
        from,
        to,
        rel,
        scope,
    };
    f.n_edges += 1;
    0
}

// --- EDGE_REVOKE ------------------------------------------------------------
// payload (12 bytes): edge_seq u64 | reason u16 | pad u16
//
// Removes nothing (disposition #11). It records that the edge stopped being
// authority at this point in the chain, which is what makes "revoked" auditable
// rather than merely absent.
fn apply_edge_revoke(
    f: &mut Fold,
    p: &[u8],
    plen: usize,
    seq: u64,
    actor_kind: u32,
    actor_id: u32,
    dry_run: u32,
) -> i32 {
    if plen < 12 {
        return E_PAYLOAD_LEN;
    }
    let edge_seq = rd_u64(p, 0);
    let idx = match find_edge(f, edge_seq) {
        Some(i) => i,
        // A revoke naming an edge this fold never held is not an error: the
        // commonest case is revoking a grant from a previous boot, which was
        // dropped above. Counted as applied, changes nothing.
        None => return 0,
    };
    if f.edges[idx].rel == REL_GRANT && (actor_kind != ACTOR_NODE || actor_id != NODE_ESCROW) {
        f.refused_not_escrow += 1;
        return E_NOT_ESCROW;
    }
    if dry_run != 0 {
        return 0;
    }
    if f.edges[idx].revoked_seq == 0 {
        f.edges[idx].revoked_seq = seq;
    }
    0
}

// ===========================================================================
// Q1: the enforcement query (design section 7)
// ===========================================================================

/// Does holder `holder` hold a LIVE grant to `target` with scope `scope`,
/// right now?
///
/// This is the query that CONTRACT_ARCHITECTURE section 6 puts at the syscall
/// chokepoint, so its constraints are absolute: NO allocation, NO I/O, NO
/// blocking, bounded time. It reads the in-memory fold and nothing else.
///
/// FAIL CLOSED (section 12.7). It returns 0 (deny) if the fold is not ready, if
/// this boot found the journal tampered with, or if the fold overflowed. That
/// last one matters and is not defensive decoration: an overflowed fold may have
/// DROPPED A REVOKE, and a dropped revoke fails OPEN. Denying everything is the
/// only honest response to "my state may not match the journal".
///
/// `now_ms` is uptime in milliseconds, supplied by the caller because this
/// module reads no clock. Section 6.1: there is no wall clock in this kernel and
/// a fabricated one would be worse than none.
///
/// Returns 1 to ALLOW, 0 to DENY. Never returns an error code: an authorization
/// question has two answers, and a caller that mistook -1 for "true" would fail
/// open.
///
/// # Safety
/// Touches this module's tables only. The caller holds the fold spinlock.
#[no_mangle]
pub unsafe extern "C" fn gfsf_grant_check(
    holder: u32,
    target: u32,
    scope: u16,
    now_ms: u64,
) -> i32 {
    // SAFETY: see fold(). The caller holds the fold spinlock.
    let f = unsafe { fold() };
    f.checks += 1;

    if f.ready == 0 || f.degraded != 0 || f.overflow != 0 {
        f.denies += 1;
        return 0;
    }
    // A caller asking for scope 0 is asking for the invalid scope. Deny.
    if scope == SCOPE_INVALID || holder == 0 || target == 0 {
        f.denies += 1;
        return 0;
    }
    if !node_live(f, holder) || !node_live(f, target) {
        f.denies += 1;
        return 0;
    }

    let mut i = 0;
    while i < f.n_edges {
        let e = f.edges[i];
        i += 1;
        if e.rel != REL_GRANT || e.from != holder || e.to != target {
            continue;
        }
        if e.revoked_seq != 0 {
            continue; // section 6: revocation bites in flight
        }
        if e.boot_gen != f.boot_gen {
            continue; // section 6.1: no trusted clock across a reboot
        }
        // SCOPE_ANY is honoured but counted (section 10). An exact scope match
        // is the normal case.
        if e.scope != scope && e.scope != SCOPE_ANY {
            continue;
        }
        if e.expiry_ms != EXPIRY_NEVER && now_ms >= e.expiry_ms as u64 {
            continue;
        }
        f.allows += 1;
        return 1;
    }
    f.denies += 1;
    0
}

// ===========================================================================
// Q2 / Q3: what does H hold, what is granted on T (design section 7)
// ===========================================================================

/// `dir` 0 = outgoing (Q2, "what contracts does H hold"), 1 = incoming
/// (Q3, "what contracts exist on T"). Writes at most `max` `GfsEdgeView`s and
/// returns how many, or a negative `E_*`.
///
/// The caller supplies the array: bounded work, no allocation, and the same
/// shape the contract manager UI needs (section 8 of the architecture).
///
/// # Safety
/// `out` must point to at least `max * size_of::<GfsEdgeView>()` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsf_edges(
    node: u32,
    dir: u32,
    now_ms: u64,
    out: *mut GfsEdgeView,
    max: u32,
) -> i32 {
    if out.is_null() || max == 0 {
        return E_ARG;
    }
    // SAFETY: caller guarantees `max` writable GfsEdgeView elements.
    let o: &mut [GfsEdgeView] = unsafe { core::slice::from_raw_parts_mut(out, max as usize) };
    // SAFETY: see fold(). The caller holds the fold spinlock.
    let f = unsafe { fold() };

    let mut n = 0usize;
    let mut i = 0usize;
    while i < f.n_edges && n < o.len() {
        let e = f.edges[i];
        i += 1;
        let hit = if dir == 0 { e.from == node } else { e.to == node };
        if !hit {
            continue;
        }
        let mut flags = 0u32;
        if e.revoked_seq != 0 {
            flags |= EDGEFLAG_REVOKED;
        }
        if e.boot_gen != f.boot_gen
            || (e.expiry_ms != EXPIRY_NEVER && now_ms >= e.expiry_ms as u64)
        {
            flags |= EDGEFLAG_EXPIRED;
        }
        o[n] = GfsEdgeView {
            edge_seq: e.edge_seq,
            revoked_seq: e.revoked_seq,
            boot_gen: e.boot_gen,
            expiry_ms: e.expiry_ms,
            from: e.from,
            to: e.to,
            rel: e.rel,
            scope: e.scope,
            flags,
            pad: 0,
        };
        n += 1;
    }
    n as i32
}

/// Every node in the fold. The identity list (section 3), for the contract
/// manager and for any audit view.
///
/// # Safety
/// `out` must point to at least `max * size_of::<GfsNodeView>()` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsf_nodes(out: *mut GfsNodeView, max: u32) -> i32 {
    if out.is_null() || max == 0 {
        return E_ARG;
    }
    // SAFETY: caller guarantees `max` writable GfsNodeView elements.
    let o: &mut [GfsNodeView] = unsafe { core::slice::from_raw_parts_mut(out, max as usize) };
    // SAFETY: see fold(). The caller holds the fold spinlock.
    let f = unsafe { fold() };
    let mut n = 0usize;
    while n < f.n_nodes && n < o.len() {
        let nd = f.nodes[n];
        o[n] = GfsNodeView {
            node_id: nd.node_id,
            kind: nd.kind,
            state: nd.state,
            parent: nd.parent,
            pad: 0,
            born_seq: nd.born_seq,
            born_boot: nd.born_boot,
        };
        n += 1;
    }
    n as i32
}

/// Is this node present, and live? 1 = live, 0 = absent, -1 = retired.
///
/// # Safety
/// Touches this module's tables only. The caller holds the fold spinlock.
#[no_mangle]
pub unsafe extern "C" fn gfsf_node_state(node_id: u32) -> i32 {
    // SAFETY: see fold(). The caller holds the fold spinlock.
    let f = unsafe { fold() };
    match find_node(f, node_id) {
        Some(i) => {
            if f.nodes[i].state == STATE_LIVE {
                1
            } else {
                -1
            }
        }
        None => 0,
    }
}

// ===========================================================================
// Stats, lifecycle
// ===========================================================================

/// Fill in the counters. The two that CONTRACT_ARCHITECTURE section 10 requires
/// (`grants_unlimited_scope`, `grants_never_expire`) are computed here from the
/// live table rather than accumulated, so they cannot drift from the truth.
///
/// # Safety
/// `out` must point to a writable `GfsStats`.
#[no_mangle]
pub unsafe extern "C" fn gfsf_stats(out: *mut GfsStats, now_ms: u64) -> i32 {
    if out.is_null() {
        return E_ARG;
    }
    // SAFETY: caller guarantees a writable GfsStats.
    let s: &mut GfsStats = unsafe { &mut *out };
    // SAFETY: see fold(). The caller holds the fold spinlock.
    let f = unsafe { fold() };

    let mut live = 0u32;
    let mut revoked = 0u32;
    let mut expired = 0u32;
    let mut unlimited = 0u32;
    let mut never = 0u32;
    let mut i = 0usize;
    while i < f.n_edges {
        let e = f.edges[i];
        i += 1;
        if e.rel != REL_GRANT {
            continue;
        }
        if e.revoked_seq != 0 {
            revoked += 1;
            continue;
        }
        if e.boot_gen != f.boot_gen
            || (e.expiry_ms != EXPIRY_NEVER && now_ms >= e.expiry_ms as u64)
        {
            expired += 1;
            continue;
        }
        live += 1;
        if e.scope == SCOPE_ANY {
            unlimited += 1;
        }
        if e.expiry_ms == EXPIRY_NEVER {
            never += 1;
        }
    }

    s.ready = f.ready;
    s.degraded = f.degraded;
    s.overflow = f.overflow;
    s.nodes = f.n_nodes as u32;
    s.edges = f.n_edges as u32;
    s.node_cap = NODE_CAP as u32;
    s.edge_cap = EDGE_CAP as u32;
    s.grants_live = live;
    s.grants_revoked = revoked;
    s.grants_expired = expired;
    s.grants_unlimited_scope = unlimited;
    s.grants_never_expire = never;
    s.refused_not_escrow = f.refused_not_escrow;
    s.refused_bad_scope = f.refused_bad_scope;
    s.refused_bad_expiry = f.refused_bad_expiry;
    s.pad = 0;
    s.boot_gen = f.boot_gen;
    s.records_seen = f.records_seen;
    s.applied = f.applied;
    s.rejected = f.rejected;
    s.checks = f.checks;
    s.allows = f.allows;
    s.denies = f.denies;
    s.grants_prior_boot = f.grants_prior_boot;
    0
}

/// Reset the fold and set this boot's generation. Called ONCE, before replay.
///
/// # Safety
/// Touches this module's tables only. The caller holds the fold spinlock.
#[no_mangle]
pub unsafe extern "C" fn gfsf_reset(boot_gen: u64, degraded: u32) {
    // SAFETY: see fold(). The caller holds the fold spinlock.
    let f = unsafe { fold() };
    f.ready = 0;
    f.degraded = degraded;
    f.overflow = 0;
    f.boot_gen = boot_gen;
    f.n_nodes = 0;
    f.n_edges = 0;
    f.records_seen = 0;
    f.applied = 0;
    f.rejected = 0;
    f.checks = 0;
    f.allows = 0;
    f.denies = 0;
    f.grants_prior_boot = 0;
    f.refused_not_escrow = 0;
    f.refused_bad_scope = 0;
    f.refused_bad_expiry = 0;
}

/// Open the fold for queries once replay has finished. Kept separate from
/// `gfsf_reset` so that Q1 denies during replay rather than answering from a
/// half-built graph.
///
/// # Safety
/// Touches this module's tables only. The caller holds the fold spinlock.
#[no_mangle]
pub unsafe extern "C" fn gfsf_set_ready(ready: u32) {
    // SAFETY: see fold(). The caller holds the fold spinlock.
    let f = unsafe { fold() };
    f.ready = ready;
}

/// Mark the store degraded. Sticky for the boot; makes Q1 deny everything.
///
/// # Safety
/// Touches this module's tables only. The caller holds the fold spinlock.
#[no_mangle]
pub unsafe extern "C" fn gfsf_set_degraded(void_reason: u32) {
    // SAFETY: see fold(). The caller holds the fold spinlock.
    let f = unsafe { fold() };
    let _ = void_reason;
    f.degraded = 1;
}

/// Sizes this crate compiled with, so the C side can `_Static_assert` that the
/// FFI structures did not drift: node view | edge view | stats, 16 bits each.
/// The same mechanism slice 1 used for the record and the seal, for the same
/// reason: a silently drifted layout produces answers that look right.
#[no_mangle]
pub extern "C" fn gfsf_sizes() -> u64 {
    (core::mem::size_of::<GfsNodeView>() as u64)
        | ((core::mem::size_of::<GfsEdgeView>() as u64) << 16)
        | ((core::mem::size_of::<GfsStats>() as u64) << 32)
}

// ===========================================================================
// Payload encoders.
//
// These live here, not in the C glue, so that the ONE definition of a graph
// payload's byte layout is in the module that also decodes it. A second
// definition on the C side is exactly the drift that turns a check into
// decoration (#503).
// ===========================================================================

/// Build a PRE-RECORD: a 160-byte buffer carrying only the header fields
/// `gfsf_apply` reads, so a mutation can be validated with `dry_run=1` BEFORE it
/// costs a journal record.
///
/// It is deliberately NOT a valid journal record: it has no magic, no hashes and
/// no seq, so it can never be mistaken for one or written to the log. It exists
/// so that the pre-check and the real apply run THE SAME POLICY CODE. The
/// alternative, a second copy of the rules on the C side, is precisely the drift
/// that turns a check into decoration.
///
/// Returns 160, or 0 on a refused argument.
///
/// # Safety
/// `out` must point to at least `out_len` writable bytes and `payload` to at
/// least `payload_len` readable bytes (or be null when `payload_len` is 0).
#[no_mangle]
pub unsafe extern "C" fn gfsf_prerec(
    out: *mut u8,
    out_len: usize,
    boot_gen: u64,
    actor_kind: u32,
    actor_id: u32,
    op: u16,
    payload: *const u8,
    payload_len: usize,
) -> usize {
    if out.is_null() || out_len < REC_SIZE {
        return 0;
    }
    if payload.is_null() && payload_len != 0 {
        return 0;
    }
    // SAFETY: extents are exactly those the caller promised.
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, REC_SIZE) };
    let p: &[u8] = if payload_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(payload, payload_len) }
    };
    let mut i = 0usize;
    while i < REC_SIZE {
        o[i] = 0;
        i += 1;
    }
    let mut k = 0usize;
    while k < 8 {
        o[O_BOOTGEN + k] = (boot_gen >> (8 * k)) as u8;
        k += 1;
    }
    let mut k = 0usize;
    while k < 4 {
        o[O_ACTKIND + k] = (actor_kind >> (8 * k)) as u8;
        o[O_ACTID + k] = (actor_id >> (8 * k)) as u8;
        o[O_PLEN + k] = ((payload_len as u32) >> (8 * k)) as u8;
        k += 1;
    }
    o[O_OP] = op as u8;
    o[O_OP + 1] = (op >> 8) as u8;
    let inl = if payload_len < INLINE_MAX { payload_len } else { INLINE_MAX };
    let mut j = 0usize;
    while j < inl {
        o[O_INLINE + j] = p[j];
        j += 1;
    }
    REC_SIZE
}

/// Build a NODE_CREATE payload. Returns its length, or 0 on a refused argument.
///
/// # Safety
/// `out` must point to at least `out_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsf_enc_node_create(
    out: *mut u8,
    out_len: usize,
    node_id: u32,
    kind: u16,
    parent: u32,
) -> usize {
    if out.is_null() || out_len < 12 {
        return 0;
    }
    // SAFETY: caller guarantees `out_len` writable bytes; we write 12.
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, 12) };
    o[0] = node_id as u8;
    o[1] = (node_id >> 8) as u8;
    o[2] = (node_id >> 16) as u8;
    o[3] = (node_id >> 24) as u8;
    o[4] = kind as u8;
    o[5] = (kind >> 8) as u8;
    o[6] = 0;
    o[7] = 0;
    o[8] = parent as u8;
    o[9] = (parent >> 8) as u8;
    o[10] = (parent >> 16) as u8;
    o[11] = (parent >> 24) as u8;
    12
}

/// Build a NODE_RETIRE / NODE_RESTORE payload.
///
/// # Safety
/// `out` must point to at least `out_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsf_enc_node_state(
    out: *mut u8,
    out_len: usize,
    node_id: u32,
    reason: u16,
) -> usize {
    if out.is_null() || out_len < 8 {
        return 0;
    }
    // SAFETY: caller guarantees `out_len` writable bytes; we write 8.
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, 8) };
    o[0] = node_id as u8;
    o[1] = (node_id >> 8) as u8;
    o[2] = (node_id >> 16) as u8;
    o[3] = (node_id >> 24) as u8;
    o[4] = reason as u8;
    o[5] = (reason >> 8) as u8;
    o[6] = 0;
    o[7] = 0;
    8
}

/// Build an EDGE_ADD payload.
///
/// # Safety
/// `out` must point to at least `out_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsf_enc_edge_add(
    out: *mut u8,
    out_len: usize,
    from: u32,
    to: u32,
    expiry_ms: u32,
    rel: u16,
    scope: u16,
) -> usize {
    if out.is_null() || out_len < 16 {
        return 0;
    }
    // SAFETY: caller guarantees `out_len` writable bytes; we write 16.
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, 16) };
    o[0] = from as u8;
    o[1] = (from >> 8) as u8;
    o[2] = (from >> 16) as u8;
    o[3] = (from >> 24) as u8;
    o[4] = to as u8;
    o[5] = (to >> 8) as u8;
    o[6] = (to >> 16) as u8;
    o[7] = (to >> 24) as u8;
    o[8] = expiry_ms as u8;
    o[9] = (expiry_ms >> 8) as u8;
    o[10] = (expiry_ms >> 16) as u8;
    o[11] = (expiry_ms >> 24) as u8;
    o[12] = rel as u8;
    o[13] = (rel >> 8) as u8;
    o[14] = scope as u8;
    o[15] = (scope >> 8) as u8;
    16
}

/// Build an EDGE_REVOKE payload.
///
/// # Safety
/// `out` must point to at least `out_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsf_enc_edge_revoke(
    out: *mut u8,
    out_len: usize,
    edge_seq: u64,
    reason: u16,
) -> usize {
    if out.is_null() || out_len < 12 {
        return 0;
    }
    // SAFETY: caller guarantees `out_len` writable bytes; we write 12.
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, 12) };
    let mut i = 0usize;
    while i < 8 {
        o[i] = (edge_seq >> (8 * i)) as u8;
        i += 1;
    }
    o[8] = reason as u8;
    o[9] = (reason >> 8) as u8;
    o[10] = 0;
    o[11] = 0;
    12
}
