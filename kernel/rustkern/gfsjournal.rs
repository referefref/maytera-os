// rustkern/gfsjournal.rs - GraphFS tamper-evident journal (#711), slice 1.
//
// NEW kernel code, so Rust per the 2026-07-16 standing rule. There is no C twin
// and no strangler flag: there was never any C here to strangle. The 72 gfs_*
// declarations in fs/graphfs/{node,query,version}.h have zero implementations
// tree-wide (MEASURED) and none of them is a journal, which is precisely the
// finding that motivated docs/GRAPHFS_DESIGN.md.
//
// WHAT LIVES HERE AND WHY IT IS THE RUST HALF.
// Everything byte-level: the record layout, encoding, the SHA-256 chain, the
// seal, and ALL verification. Verification is the code an attacker gets to feed
// (a tampered journal is hostile input by definition), so it is the part that
// belongs in the memory-safe language. Every access below goes through a slice
// built over an exactly-known extent, so a malformed or truncated file can only
// produce a REJECT, never an out-of-bounds read.
//
// The C half (fs/graphfs/journal.c) is I/O and threading glue only: read a file
// into a buffer, hand it here, write back what this produces. It holds no format
// knowledge. See docs/GRAPHFS_DESIGN.md section 12 for why that glue is C.
//
// NO FLOATS. MEASURED (docs/GRAPHFS_DESIGN.md section 2): with -mno-sse the
// compiler marshals a float through the x87 FPU, and this kernel never saves x87
// state across a context switch. The declared-but-unimplemented API is full of
// them; nothing here has one.
//
// NO ALLOCATION. Every function works in caller-supplied buffers, so this is
// callable from any context that can supply a 160-byte stack buffer.

// C: void sha256(const void *data, size_t length, uint8_t digest[32]);
// crypto/sha256.c. Its block core is ALREADY Rust (-DRUST_SHA256,
// sha256_transform_rs), so this is the one shared hash primitive, not a private
// fork of one.
extern "C" {
    fn sha256(data: *const u8, length: usize, digest: *mut u8);
}

/// One journal record. Fixed size: see docs/GRAPHFS_DESIGN.md section 6 for the
/// field table and the reasons fixed-size is load-bearing (exact truncation
/// arithmetic, backward walkability, a Ring-3 verifier anyone can write).
pub const GFSJ_REC_SIZE: usize = 160;
/// Bytes of a record covered by `self_hash` (everything before it).
const GFSJ_REC_SIGNED: usize = 128;
pub const GFSJ_SEAL_SIZE: usize = 96;
const GFSJ_SEAL_SIGNED: usize = 56;

const REC_MAGIC: u32 = 0x314a_4647; // "GFJ1" little-endian
const SEAL_MAGIC: u32 = 0x314c_5347; // "GSL1" little-endian
const FORMAT_VER: u16 = 1;

/// Bytes of payload that fit inline in a record. Larger payloads are stored in
/// the content-addressed blob store and referenced by `payload_hash`; slice 1
/// deliberately does not depend on that store yet (design section 11/15).
pub const GFSJ_INLINE_MAX: usize = 16;

// --- record field offsets ---------------------------------------------------
const O_MAGIC: usize = 0;
const O_VER: usize = 4;
const O_RECLEN: usize = 6;
const O_SEQ: usize = 8;
const O_BOOTGEN: usize = 16;
const O_MONOMS: usize = 24;
const O_ACTKIND: usize = 32;
const O_ACTID: usize = 36;
const O_OP: usize = 40;
const O_EFFECT: usize = 42;
const O_PLEN: usize = 44;
const O_PHASH: usize = 48;
const O_PREV: usize = 80;
const O_INLINE: usize = 112;
const O_SELF: usize = 128;

// --- seal field offsets -----------------------------------------------------
const S_MAGIC: usize = 0;
const S_VER: usize = 4;
const S_LEN: usize = 6;
const S_COUNT: usize = 8;
const S_BOOTGEN: usize = 16;
const S_HEAD: usize = 24;
const S_SELF: usize = 56;

// --- verification reasons. Kept in sync with fs/graphfs/journal.h -----------
pub const GFSJ_OK: u32 = 0;
pub const GFSJ_R_RAGGED: u32 = 1; // file size is not a whole number of records
pub const GFSJ_R_BAD_MAGIC: u32 = 2;
pub const GFSJ_R_BAD_VERSION: u32 = 3;
pub const GFSJ_R_CHAIN_BREAK: u32 = 4; // prev_hash != hash(previous record)
pub const GFSJ_R_SEQ_GAP: u32 = 5; // seq field is not its own index
pub const GFSJ_R_HASH_MISMATCH: u32 = 6; // record's own bytes were edited
pub const GFSJ_R_SEAL_MISSING: u32 = 7;
pub const GFSJ_R_SEAL_CORRUPT: u32 = 8;
pub const GFSJ_R_TRUNCATED: u32 = 9; // fewer records than the seal counted
pub const GFSJ_R_EXTENDED: u32 = 10; // more records than the seal counted
pub const GFSJ_R_HEAD_MISMATCH: u32 = 11; // chain head != sealed head
pub const GFSJ_R_ANCHOR_COUNT: u32 = 12; // count != the kernel's own count
pub const GFSJ_R_ANCHOR_MISMATCH: u32 = 13; // head != the kernel's own head
pub const GFSJ_R_ARG: u32 = 14; // caller passed something impossible

/// Result of a verification pass. `#[repr(C)]`; the C side locks its size with
/// a `_Static_assert` and this crate re-checks it at init through
/// `gfsj_sizes()`, so the two can never drift silently.
#[repr(C)]
pub struct GfsjVerify {
    pub status: u32,   // GFSJ_OK or the first failing reason
    pub reason: u32,   // same value; kept separate so status can gain flags
    pub degraded: u32, // set by C: this boot found the journal already bad
    pub pad: u32,
    pub bad_seq: u64,    // record index the failure was found at
    pub count: u64,      // records actually present on disk
    pub seal_count: u64, // records the seal claims
    pub boot_gen: u64,   // boot generation from the seal
    pub head_hash: [u8; 32],
}

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
#[inline]
fn wr_u16(s: &mut [u8], o: usize, v: u16) {
    s[o] = v as u8;
    s[o + 1] = (v >> 8) as u8;
}
#[inline]
fn wr_u32(s: &mut [u8], o: usize, v: u32) {
    let mut i = 0;
    while i < 4 {
        s[o + i] = (v >> (8 * i)) as u8;
        i += 1;
    }
}
#[inline]
fn wr_u64(s: &mut [u8], o: usize, v: u64) {
    let mut i = 0;
    while i < 8 {
        s[o + i] = (v >> (8 * i)) as u8;
        i += 1;
    }
}

/// SHA-256 of `data` into `out`, through the kernel's one hash implementation.
fn hash(data: &[u8], out: &mut [u8; 32]) {
    // SAFETY: sha256() reads exactly `data.len()` bytes from `data.as_ptr()` and
    // writes exactly 32 bytes to `out`. Both extents are owned by slices whose
    // lengths we pass verbatim, so neither can be overrun. Neither pointer is
    // retained by the callee.
    unsafe { sha256(data.as_ptr(), data.len(), out.as_mut_ptr()) };
}

fn eq32(a: &[u8], ao: usize, b: &[u8; 32]) -> bool {
    let mut i = 0;
    let mut diff = 0u8;
    while i < 32 {
        diff |= a[ao + i] ^ b[i];
        i += 1;
    }
    diff == 0
}

// ===========================================================================
// Encoding
// ===========================================================================

/// Encode one journal record into `buf` (which must be at least
/// `GFSJ_REC_SIZE`), chaining onto `prev_hash` (32 bytes; all-zero for seq 0),
/// and write the new record's own hash to `out_self` (32 bytes).
///
/// `payload_hash` is the SHA-256 of the WHOLE payload, whatever its length, so
/// a record cryptographically binds content that lives outside the journal (the
/// blob store later; the security log's formatted line today). The first
/// `GFSJ_INLINE_MAX` bytes are additionally carried verbatim inside the record,
/// which makes the common short payload free to read back.
///
/// Returns the number of bytes written, or 0 on a bad argument. Never partially
/// writes a record: all argument checks happen first.
///
/// # Safety
/// `buf` must point to at least `buf_len` writable bytes, `payload` to at least
/// `payload_len` readable bytes (or be null when `payload_len` is 0),
/// `prev_hash` to 32 readable bytes, `out_self` to 32 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsj_encode(
    buf: *mut u8,
    buf_len: usize,
    seq: u64,
    boot_gen: u64,
    mono_ms: u64,
    actor_kind: u32,
    actor_id: u32,
    op: u16,
    effect: u16,
    payload: *const u8,
    payload_len: usize,
    prev_hash: *const u8,
    out_self: *mut u8,
) -> usize {
    if buf.is_null() || buf_len < GFSJ_REC_SIZE || prev_hash.is_null() || out_self.is_null() {
        return 0;
    }
    if payload.is_null() && payload_len != 0 {
        return 0;
    }
    // SAFETY: extents are exactly those the caller promised above. From here on
    // every access is through these slices, so it is bounds-checked by Rust.
    let b: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(buf, GFSJ_REC_SIZE) };
    let prev: &[u8] = unsafe { core::slice::from_raw_parts(prev_hash, 32) };
    let pay: &[u8] = if payload_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(payload, payload_len) }
    };

    let mut i = 0;
    while i < GFSJ_REC_SIZE {
        b[i] = 0;
        i += 1;
    }
    wr_u32(b, O_MAGIC, REC_MAGIC);
    wr_u16(b, O_VER, FORMAT_VER);
    wr_u16(b, O_RECLEN, GFSJ_REC_SIZE as u16);
    wr_u64(b, O_SEQ, seq);
    wr_u64(b, O_BOOTGEN, boot_gen);
    wr_u64(b, O_MONOMS, mono_ms);
    wr_u32(b, O_ACTKIND, actor_kind);
    wr_u32(b, O_ACTID, actor_id);
    wr_u16(b, O_OP, op);
    wr_u16(b, O_EFFECT, effect);
    wr_u32(b, O_PLEN, payload_len as u32);

    let mut ph = [0u8; 32];
    hash(pay, &mut ph);
    let mut k = 0;
    while k < 32 {
        b[O_PHASH + k] = ph[k];
        b[O_PREV + k] = prev[k];
        k += 1;
    }
    // Inline the first GFSJ_INLINE_MAX bytes; payload_hash above already binds
    // the whole thing however long it is.
    let inl = if payload_len < GFSJ_INLINE_MAX { payload_len } else { GFSJ_INLINE_MAX };
    let mut j = 0;
    while j < inl {
        b[O_INLINE + j] = pay[j];
        j += 1;
    }

    let mut sh = [0u8; 32];
    hash(&b[0..GFSJ_REC_SIGNED], &mut sh);
    let mut m = 0;
    while m < 32 {
        b[O_SELF + m] = sh[m];
        m += 1;
    }
    // SAFETY: out_self points to 32 writable bytes per the contract above.
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out_self, 32) };
    let mut n = 0;
    while n < 32 {
        o[n] = sh[n];
        n += 1;
    }
    GFSJ_REC_SIZE
}

/// Encode the seal: the sealed head of the chain. Rewritten after every append.
/// Returns bytes written, or 0 on a bad argument.
///
/// # Safety
/// `buf` must point to at least `buf_len` writable bytes and `head` to 32
/// readable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsj_seal_encode(
    buf: *mut u8,
    buf_len: usize,
    count: u64,
    boot_gen: u64,
    head: *const u8,
) -> usize {
    if buf.is_null() || buf_len < GFSJ_SEAL_SIZE || head.is_null() {
        return 0;
    }
    // SAFETY: extents are exactly those the caller promised above.
    let b: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(buf, GFSJ_SEAL_SIZE) };
    let h: &[u8] = unsafe { core::slice::from_raw_parts(head, 32) };
    let mut i = 0;
    while i < GFSJ_SEAL_SIZE {
        b[i] = 0;
        i += 1;
    }
    wr_u32(b, S_MAGIC, SEAL_MAGIC);
    wr_u16(b, S_VER, FORMAT_VER);
    wr_u16(b, S_LEN, GFSJ_SEAL_SIZE as u16);
    wr_u64(b, S_COUNT, count);
    wr_u64(b, S_BOOTGEN, boot_gen);
    let mut k = 0;
    while k < 32 {
        b[S_HEAD + k] = h[k];
        k += 1;
    }
    let mut sh = [0u8; 32];
    hash(&b[0..GFSJ_SEAL_SIGNED], &mut sh);
    let mut m = 0;
    while m < 32 {
        b[S_SELF + m] = sh[m];
        m += 1;
    }
    GFSJ_SEAL_SIZE
}

/// Read the boot generation out of a seal buffer, 0 if the seal is unusable.
/// Used at init to continue the generation counter across reboots.
///
/// # Safety
/// `seal` must point to at least `seal_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn gfsj_seal_bootgen(seal: *const u8, seal_len: usize) -> u64 {
    if seal.is_null() || seal_len < GFSJ_SEAL_SIZE {
        return 0;
    }
    // SAFETY: extent is exactly what the caller promised.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(seal, GFSJ_SEAL_SIZE) };
    if rd_u32(s, S_MAGIC) != SEAL_MAGIC || rd_u16(s, S_VER) != FORMAT_VER {
        return 0;
    }
    let mut sh = [0u8; 32];
    hash(&s[0..GFSJ_SEAL_SIGNED], &mut sh);
    if !eq32(s, S_SELF, &sh) {
        return 0;
    }
    rd_u64(s, S_BOOTGEN)
}

// ===========================================================================
// Verification: the whole point of the exercise
// ===========================================================================

fn set_result(out: &mut GfsjVerify, reason: u32, bad_seq: u64) {
    out.status = reason;
    out.reason = reason;
    out.bad_seq = bad_seq;
}

/// Verify a journal against its seal and (optionally) against the kernel's own
/// in-memory head anchor.
///
/// Three independent mechanisms, because each catches what the others cannot
/// (docs/GRAPHFS_DESIGN.md section 9):
///
///  1. THE CHAIN catches EDITS and REORDERING. Each record hashes its own bytes
///     into `self_hash` and carries the previous record's hash in `prev_hash`.
///  2. THE SEAL catches TRUNCATION. A truncated chain is still internally
///     consistent, so the chain alone cannot see it; the sealed count and head
///     can.
///  3. THE IN-KERNEL ANCHOR catches an ADAPTIVE attacker who rewrote the chain
///     AND the seal consistently. Ring 3 cannot write kernel memory, so within a
///     boot that attacker is still caught. Pass `anchor_count == 0` to skip
///     (nothing has been appended this boot, so there is nothing to compare).
///
/// Honest boundary, stated in the design doc and repeated here so it cannot be
/// read out of context: the files are UNKEYED. Across a reboot the anchor is
/// gone and this degrades to non-adaptive tampering only. Signing the seal needs
/// a key in the immutable core (#305) and a non-root session (#679).
///
/// Returns 0 if the journal verifies, otherwise the failing reason (also written
/// into `out`).
///
/// # Safety
/// `log`/`seal` must point to at least `log_len`/`seal_len` readable bytes (or be
/// null with a length of 0), `anchor_head` to 32 readable bytes or be null, and
/// `out` to a writable `GfsjVerify`.
#[no_mangle]
pub unsafe extern "C" fn gfsj_verify(
    log: *const u8,
    log_len: usize,
    seal: *const u8,
    seal_len: usize,
    anchor_count: u64,
    anchor_head: *const u8,
    out: *mut GfsjVerify,
) -> u32 {
    if out.is_null() {
        return GFSJ_R_ARG;
    }
    // SAFETY: caller guarantees `out` is a writable GfsjVerify.
    let o: &mut GfsjVerify = unsafe { &mut *out };
    o.status = GFSJ_OK;
    o.reason = GFSJ_OK;
    o.bad_seq = 0;
    o.count = 0;
    o.seal_count = 0;
    o.boot_gen = 0;
    o.head_hash = [0u8; 32];

    if (log.is_null() && log_len != 0) || (seal.is_null() && seal_len != 0) {
        set_result(o, GFSJ_R_ARG, 0);
        return GFSJ_R_ARG;
    }
    // SAFETY: extents are exactly those the caller promised. Every read below
    // goes through these slices, so a hostile file can only cause a reject.
    let l: &[u8] = if log_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(log, log_len) }
    };
    let s: &[u8] = if seal_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(seal, seal_len) }
    };

    // --- 1. the chain -------------------------------------------------------
    if log_len % GFSJ_REC_SIZE != 0 {
        set_result(o, GFSJ_R_RAGGED, (log_len / GFSJ_REC_SIZE) as u64);
        return GFSJ_R_RAGGED;
    }
    let n = log_len / GFSJ_REC_SIZE;
    o.count = n as u64;

    let mut prev = [0u8; 32];
    let mut i = 0usize;
    while i < n {
        let base = i * GFSJ_REC_SIZE;
        let r = &l[base..base + GFSJ_REC_SIZE];

        if rd_u32(r, O_MAGIC) != REC_MAGIC {
            set_result(o, GFSJ_R_BAD_MAGIC, i as u64);
            return GFSJ_R_BAD_MAGIC;
        }
        if rd_u16(r, O_VER) != FORMAT_VER || rd_u16(r, O_RECLEN) as usize != GFSJ_REC_SIZE {
            set_result(o, GFSJ_R_BAD_VERSION, i as u64);
            return GFSJ_R_BAD_VERSION;
        }
        // Chain link BEFORE the seq check: a swap of two records breaks both,
        // and "the chain is broken here" is the more diagnostic of the two.
        if !eq32(r, O_PREV, &prev) {
            set_result(o, GFSJ_R_CHAIN_BREAK, i as u64);
            return GFSJ_R_CHAIN_BREAK;
        }
        if rd_u64(r, O_SEQ) != i as u64 {
            set_result(o, GFSJ_R_SEQ_GAP, i as u64);
            return GFSJ_R_SEQ_GAP;
        }
        let mut sh = [0u8; 32];
        hash(&r[0..GFSJ_REC_SIGNED], &mut sh);
        if !eq32(r, O_SELF, &sh) {
            set_result(o, GFSJ_R_HASH_MISMATCH, i as u64);
            return GFSJ_R_HASH_MISMATCH;
        }
        prev = sh;
        i += 1;
    }
    o.head_hash = prev;

    // --- 2. the seal --------------------------------------------------------
    if seal_len == 0 {
        // No seal at all. Only legitimate for a journal that does not exist yet.
        if n == 0 {
            return GFSJ_OK;
        }
        set_result(o, GFSJ_R_SEAL_MISSING, 0);
        return GFSJ_R_SEAL_MISSING;
    }
    if seal_len < GFSJ_SEAL_SIZE
        || rd_u32(s, S_MAGIC) != SEAL_MAGIC
        || rd_u16(s, S_VER) != FORMAT_VER
        || rd_u16(s, S_LEN) as usize != GFSJ_SEAL_SIZE
    {
        set_result(o, GFSJ_R_SEAL_CORRUPT, 0);
        return GFSJ_R_SEAL_CORRUPT;
    }
    let mut ssh = [0u8; 32];
    hash(&s[0..GFSJ_SEAL_SIGNED], &mut ssh);
    if !eq32(s, S_SELF, &ssh) {
        set_result(o, GFSJ_R_SEAL_CORRUPT, 0);
        return GFSJ_R_SEAL_CORRUPT;
    }
    o.seal_count = rd_u64(s, S_COUNT);
    o.boot_gen = rd_u64(s, S_BOOTGEN);

    if (n as u64) < o.seal_count {
        set_result(o, GFSJ_R_TRUNCATED, n as u64);
        return GFSJ_R_TRUNCATED;
    }
    if (n as u64) > o.seal_count {
        set_result(o, GFSJ_R_EXTENDED, o.seal_count);
        return GFSJ_R_EXTENDED;
    }
    if !eq32(s, S_HEAD, &prev) {
        set_result(o, GFSJ_R_HEAD_MISMATCH, n as u64);
        return GFSJ_R_HEAD_MISMATCH;
    }

    // --- 3. the in-kernel anchor -------------------------------------------
    if anchor_count != 0 && !anchor_head.is_null() {
        if n as u64 != anchor_count {
            set_result(o, GFSJ_R_ANCHOR_COUNT, anchor_count);
            return GFSJ_R_ANCHOR_COUNT;
        }
        // SAFETY: caller guarantees 32 readable bytes when non-null.
        let a: &[u8] = unsafe { core::slice::from_raw_parts(anchor_head, 32) };
        if !eq32(a, 0, &prev) {
            set_result(o, GFSJ_R_ANCHOR_MISMATCH, n as u64);
            return GFSJ_R_ANCHOR_MISMATCH;
        }
    }
    GFSJ_OK
}

/// Report the sizes this crate compiled with, so the C side can assert the FFI
/// structures did not drift. Packs record size, seal size and the size of
/// `GfsjVerify` into one u64 (16 bits each). Checked at init; a mismatch is a
/// hard failure, not a warning.
#[no_mangle]
pub extern "C" fn gfsj_sizes() -> u64 {
    (GFSJ_REC_SIZE as u64)
        | ((GFSJ_SEAL_SIZE as u64) << 16)
        | ((core::mem::size_of::<GfsjVerify>() as u64) << 32)
}
