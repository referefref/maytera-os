// rustkern/ext2.rs - #404 Phase C / #485 ext2 directory-block entry scan
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase C port (#404 / #485): ext2 directory-block entry scan.
//
// Faithful, memory-safe Rust drop-in for the inner per-block entry loop of
// fs/ext2.c ext2_lookup() (the loop that parses fully disk-controlled rec_len /
// name_len - the classic untrusted-input OOB-read surface, hardened in plain C
// under #476). Same signature and return contract as the C reference
// ext2_dirblock_find_c(): returns 1 and fills *out_ino / *out_type if `name`
// (length name_len) is found in this directory block, 0 otherwise. `ci` != 0 =>
// case-insensitive ASCII compare (mirrors g_root_ext2).
//
// GUARD ALIGNMENT (important, honest note): the PoC port proved equivalence to a
// C extract on WELL-FORMED input only, and used a looser `rec == 0` termination.
// The LIVE C reference here is the #476-hardened loop, whose three guards are
// (1) the 8-byte header must fit (`off + 8 <= block_size`), (2) rec_len must be
// sane (`rec >= 8 && off + rec <= block_size`, else stop), (3) the name field
// must fit (`off + 8 + name_len <= block_size`, else stop). To be a BYTE-
// IDENTICAL drop-in for that live loop on BOTH well-formed AND malformed blocks
// (so the boot-time differential self-test passes by construction, not by luck),
// this Rust adopts those exact three guards. Memory safety is unchanged and
// independent of the guards: every read goes through a slice spanning exactly
// `block_size` bytes, so an out-of-range index can only ever panic-or-return,
// never read out of bounds.
#[no_mangle]
pub extern "C" fn ext2_dirblock_find_rs(
    blk: *const u8,
    block_size: u32,
    name: *const u8,
    name_len: u32,
    ci: i32,
    out_ino: *mut u32,
    out_type: *mut u8,
) -> i32 {
    let bs = block_size as usize;
    let nl = name_len as usize;

    // SAFETY: The caller guarantees `blk` points to at least `block_size`
    // contiguous readable bytes (the exact contract fs/ext2.c relies on: `blk`
    // is a freshly kmalloc(block_size) buffer filled by ext2_read_block) and
    // that `name` points to at least `name_len` readable bytes. We build two
    // slices spanning exactly those extents and from here on index ONLY through
    // them, so every access is bounds-checked by Rust: a disk-controlled
    // rec_len / name_len that would push an index out of range hits the explicit
    // guards below and returns safely, never an out-of-bounds read.
    let block: &[u8] = unsafe { core::slice::from_raw_parts(blk, bs) };
    let qname: &[u8] = unsafe { core::slice::from_raw_parts(name, nl) };

    let mut off: usize = 0;
    // Guard 1 (matches C `while (off + 8 <= block_size)`): the 8-byte entry
    // header must fit before we read it.
    while off + 8 <= bs {
        let e_ino = u32::from_le_bytes([block[off], block[off + 1], block[off + 2], block[off + 3]]);
        let rec = u16::from_le_bytes([block[off + 4], block[off + 5]]) as usize;
        let nlen = block[off + 6] as usize;
        let ftype = block[off + 7];

        // Guard 2 (matches C `if (rec < 8 || off + rec > block_size) break;`):
        // reject a malformed rec_len that would infinite-loop or overrun.
        if rec < 8 || off + rec > bs {
            break;
        }

        if e_ino != 0 && nlen == nl {
            // Guard 3 (matches C `if (off + 8 + name_len > block_size) break;`):
            // the name field must lie within the block.
            if off + 8 + nl > bs {
                break;
            }
            let mut matched = true;
            let mut i = 0usize;
            while i < nl {
                let mut a = block[off + 8 + i];
                let mut b = qname[i];
                if ci != 0 {
                    if a >= b'a' && a <= b'z' {
                        a -= 32;
                    }
                    if b >= b'a' && b <= b'z' {
                        b -= 32;
                    }
                }
                if a != b {
                    matched = false;
                    break;
                }
                i += 1;
            }
            if matched {
                // SAFETY: out_ino / out_type are either null or valid,
                // caller-provided writable pointers (same contract as the C
                // reference which writes *out_ino / *out_type). We write only
                // after a null check.
                unsafe {
                    if !out_ino.is_null() {
                        *out_ino = e_ino;
                    }
                    if !out_type.is_null() {
                        *out_type = ftype;
                    }
                }
                return 1;
            }
        }

        off += rec;
    }
    0
}

// ===========================================================================
// #605 (extends the #485 seam): ext2 directory-entry INSERT within one block.
//
// This is the WRITE twin of ext2_dirblock_find above, and it exists because
// #597 fixed a real MEMORY-SAFETY bug here IN C: `slack = rec - used` is
// unsigned, so a record whose own name_len does not fit inside its own rec_len
// (corrupt on disk, or a rec_len we ourselves mis-wrote) made `slack` wrap to
// ~4e9. Every "is there room?" test then passed, the code wrote a rec_len
// pointing OUTSIDE the block, and memcpy'd the name PAST the end of the
// kmalloc(block_size) buffer: one lost update escalated into on-disk corruption
// plus heap corruption. That is exactly the class Rust removes by construction,
// so the pure buffer logic is ported here and the hand-added C guard becomes a
// property of the type system (`checked_sub`) instead of a patch.
//
// SCOPE, deliberately narrow (the whole point of the seam): this function is
// PURE and I/O-FREE. It touches ONE directory block buffer. It does not know
// about ext2_lock/ext2_unlock, ext2_read_block/ext2_write_block, inode or
// bitmap allocation, ext2_inode_append_block, i_size, or any ext2_fs_t static.
// All of that stays in C and keeps calling this through a ONE-function FFI.
//
// Return contract, byte-identical to the C reference ext2_dirblock_insert_c():
//    1 = INSERTED. `blk` has been mutated; the caller writes the block back.
//    0 = NO ROOM in this block. `blk` is UNMODIFIED; the caller tries the next
//        block, then falls back to the append path.
//   -1 = CORRUPT geometry or invalid argument. `blk` is UNMODIFIED; the caller
//        treats it exactly like 0 (skip this block). Pre-#605 C expressed both
//        0 and -1 as "fall out of the per-block loop"; splitting them costs
//        nothing and lets the boot self-test name the hostile case.

/// Pure directory-block record walk + insert. `#![forbid(unsafe_code)]` applies
/// to this whole module: there is no raw pointer, no unchecked index and no
/// `unwrap` in here, so it can neither read/write out of bounds nor panic (a
/// panic in kernel Rust is fatal: rustkern.rs's `#[panic_handler]` calls
/// `rust_kernel_panic`). Every fallible step returns an error code instead.
mod dirins {
    #![forbid(unsafe_code)]

    pub const INS_OK: i32 = 1;
    pub const INS_NOROOM: i32 = 0;
    pub const INS_CORRUPT: i32 = -1;

    /// The on-disk ext2 dirent header, mirrored purely as an ABI size lock: the
    /// C side carries the identical `#[repr(C)]`/packed struct with a matching
    /// `_Static_assert(sizeof(...) == 8)`. The parser below reads byte-wise
    /// little-endian rather than through this struct, because dirents are only
    /// 4-byte aligned and a `rec_len` may sit on an odd 2-byte boundary.
    #[repr(C, packed)]
    #[allow(dead_code)]
    struct Ext2DirentHdr {
        inode: u32,
        rec_len: u16,
        name_len: u8,
        file_type: u8,
    }
    const _: () = assert!(core::mem::size_of::<Ext2DirentHdr>() == 8);

    /// ext2 rounds every record up to a 4-byte boundary. `n` is always <= 263
    /// here (8 + a name_len of at most 255), so this cannot overflow.
    #[inline]
    fn round4(n: usize) -> usize {
        (n + 3) & !3usize
    }

    fn rd_u32(b: &[u8], o: usize) -> Option<u32> {
        let s = b.get(o..o.checked_add(4)?)?;
        let mut a = [0u8; 4];
        for (d, x) in a.iter_mut().zip(s.iter()) {
            *d = *x;
        }
        Some(u32::from_le_bytes(a))
    }

    fn rd_u16(b: &[u8], o: usize) -> Option<u16> {
        let s = b.get(o..o.checked_add(2)?)?;
        let mut a = [0u8; 2];
        for (d, x) in a.iter_mut().zip(s.iter()) {
            *d = *x;
        }
        Some(u16::from_le_bytes(a))
    }

    fn wr_bytes(b: &mut [u8], o: usize, src: &[u8]) -> Option<()> {
        let dst = b.get_mut(o..o.checked_add(src.len())?)?;
        for (d, s) in dst.iter_mut().zip(src.iter()) {
            *d = *s;
        }
        Some(())
    }

    pub fn insert(block: &mut [u8], name: &[u8], child_ino: u32, ftype: u8) -> i32 {
        let bs = block.len();
        let nlen = name.len();
        // An ext2 name_len is a single byte and a zero-length name is not a
        // directory entry. The C reference carries the identical guard, so the
        // two arms agree on these degenerate inputs instead of the C silently
        // truncating a >255 name_len into the on-disk byte.
        if nlen == 0 || nlen > 255 {
            return INS_CORRUPT;
        }
        let need = round4(8 + nlen); // <= 264

        let mut off: usize = 0;
        // Guard 1: the 8-byte header must fit before we read it.
        while off + 8 <= bs {
            let e_ino = match rd_u32(block, off) {
                Some(v) => v,
                None => return INS_CORRUPT,
            };
            let rec = match rd_u16(block, off + 4) {
                Some(v) => v as usize,
                None => return INS_CORRUPT,
            };
            let e_nl = match block.get(off + 6) {
                Some(v) => *v as usize,
                None => return INS_CORRUPT,
            };

            // Guard 2: a rec_len below the header size would not advance (an
            // infinite loop) and one past the block end would overrun.
            if rec < 8 || off + rec > bs {
                return INS_CORRUPT;
            }

            // How much of this record its own name actually occupies. A
            // tombstone (inode == 0) occupies nothing and is fully reusable.
            let used = if e_ino == 0 { 0 } else { round4(8 + e_nl) };

            // Guard 3 == THE #597 BUG, now structural. In C this was
            // `slack = rec - used` on uint32_t, wrapping to ~4e9 whenever the
            // record's own name did not fit its own rec_len. `checked_sub`
            // makes that underflow unrepresentable: the only way past this line
            // is used <= rec, so `slack` is a real byte count in 0..=rec.
            let slack = match rec.checked_sub(used) {
                Some(s) => s,
                None => return INS_CORRUPT,
            };

            if slack >= need {
                // Tombstone: take the whole record. Live entry: shrink it to
                // exactly what it uses and take the tail.
                let (newoff, newrec) = if e_ino == 0 {
                    (off, rec)
                } else {
                    (off + used, slack)
                };
                // Both writes must land inside the block, and the new rec_len
                // must fit the on-disk u16. Implied by the guards above (newoff
                // + newrec == off + rec <= bs, and newrec >= need >= 8 + nlen),
                // but stated so the function is total rather than relying on a
                // proof held only in a comment.
                if newoff + 8 + nlen > bs || newoff + newrec > bs || newrec > 0xFFFF {
                    return INS_CORRUPT;
                }
                if e_ino != 0 {
                    if wr_bytes(block, off + 4, &(used as u16).to_le_bytes()).is_none() {
                        return INS_CORRUPT;
                    }
                }
                if wr_bytes(block, newoff, &child_ino.to_le_bytes()).is_none() {
                    return INS_CORRUPT;
                }
                if wr_bytes(block, newoff + 4, &(newrec as u16).to_le_bytes()).is_none() {
                    return INS_CORRUPT;
                }
                match block.get_mut(newoff + 6) {
                    Some(p) => *p = nlen as u8,
                    None => return INS_CORRUPT,
                }
                match block.get_mut(newoff + 7) {
                    Some(p) => *p = ftype,
                    None => return INS_CORRUPT,
                }
                if wr_bytes(block, newoff + 8, name).is_none() {
                    return INS_CORRUPT;
                }
                return INS_OK;
            }

            // rec >= 8 was checked above, so `off` strictly increases and the
            // loop terminates.
            off += rec;
        }
        INS_NOROOM
    }
}

/// #746: repoint an EXISTING directory record at a different inode, in place.
///
/// WHY THIS IS ITS OWN OPERATION AND NOT insert()+remove(). This is the atomic
/// step of a POSIX rename over an existing destination. The record already
/// carries the destination's name, so nothing about the record's GEOMETRY
/// changes: only the 4-byte inode field and the 1-byte file_type. That means
/// the whole replacement is a single directory-block write, and a crash either
/// leaves the old inode named or the new one, never a directory with the name
/// missing. Doing it as remove-then-insert would open a window in which the
/// destination name does not exist at all, and would also have to find room the
/// block may no longer have.
///
/// Same discipline as `dirins`: `#![forbid(unsafe_code)]`, so no on-disk
/// `rec_len` / `name_len` can steer an access outside the block, and every
/// fallible step returns a code instead of panicking.
mod dirrep {
    #![forbid(unsafe_code)]

    pub const REP_OK: i32 = 1;       // found, and the block now points at new_ino
    pub const REP_MISS: i32 = 0;     // not in this block; the caller tries the next
    pub const REP_CORRUPT: i32 = -1; // record geometry is not walkable

    #[inline]
    fn round4(n: usize) -> usize {
        (n + 3) & !3usize
    }

    fn rd_u32(b: &[u8], o: usize) -> Option<u32> {
        let s = b.get(o..o.checked_add(4)?)?;
        let mut a = [0u8; 4];
        for (d, x) in a.iter_mut().zip(s.iter()) {
            *d = *x;
        }
        Some(u32::from_le_bytes(a))
    }

    fn rd_u16(b: &[u8], o: usize) -> Option<u16> {
        let s = b.get(o..o.checked_add(2)?)?;
        let mut a = [0u8; 2];
        for (d, x) in a.iter_mut().zip(s.iter()) {
            *d = *x;
        }
        Some(u16::from_le_bytes(a))
    }

    pub fn repoint(block: &mut [u8], name: &[u8], new_ino: u32, ftype: u8) -> i32 {
        if name.is_empty() || name.len() > 255 {
            return REP_CORRUPT;
        }
        let len = block.len();
        let mut off: usize = 0;
        while off + 8 <= len {
            let ino = match rd_u32(block, off) {
                Some(v) => v,
                None => return REP_CORRUPT,
            };
            let rec = match rd_u16(block, off + 4) {
                Some(v) => v as usize,
                None => return REP_CORRUPT,
            };
            let nlen = match block.get(off + 6) {
                Some(v) => *v as usize,
                None => return REP_CORRUPT,
            };
            // A record must be walkable and must stay inside the block. Same
            // two guards the C reference applies (#476/#597).
            if rec < 8 || off + rec > len {
                return REP_CORRUPT;
            }
            if ino != 0 && round4(8 + nlen) > rec {
                return REP_CORRUPT;
            }
            if ino != 0 && nlen == name.len() {
                let got = match block.get(off + 8..off + 8 + nlen) {
                    Some(g) => g,
                    None => return REP_CORRUPT,
                };
                if got == name {
                    let le = new_ino.to_le_bytes();
                    // Bounds already proven by the `off + 8 <= len` loop guard.
                    match block.get_mut(off..off + 4) {
                        Some(dst) => {
                            for (d, x) in dst.iter_mut().zip(le.iter()) {
                                *d = *x;
                            }
                        }
                        None => return REP_CORRUPT,
                    }
                    match block.get_mut(off + 7) {
                        Some(d) => *d = ftype,
                        None => return REP_CORRUPT,
                    }
                    return REP_OK;
                }
            }
            off += rec;
        }
        REP_MISS
    }
}

/// C ABI shim for `dirrep::repoint`. Same one-job-only shape as the insert shim
/// below: turn the caller's (pointer, length) pairs into slices of exactly that
/// extent, and let bounds-checked Rust do everything else.
#[no_mangle]
pub extern "C" fn ext2_dirblock_repoint_rs(
    blk: *mut u8,
    block_size: u32,
    name: *const u8,
    name_len: u32,
    new_ino: u32,
    ftype: u8,
) -> i32 {
    if blk.is_null() || name.is_null() {
        return dirrep::REP_CORRUPT;
    }
    if block_size == 0 || block_size > 65536 {
        return dirrep::REP_CORRUPT;
    }
    if name_len == 0 || name_len > 255 {
        return dirrep::REP_CORRUPT;
    }
    // SAFETY: identical contract to ext2_dirblock_insert_rs below. `blk` is a
    // kmalloc(fs->block_size) buffer filled by ext2_read_block and is at least
    // `block_size` readable+writable bytes; `name` is at least `name_len`
    // readable bytes of a caller-owned component whose length the caller
    // measured. The two slices span exactly those extents and cannot alias.
    let block = unsafe { core::slice::from_raw_parts_mut(blk, block_size as usize) };
    let nm = unsafe { core::slice::from_raw_parts(name, name_len as usize) };
    dirrep::repoint(block, nm, new_ino, ftype)
}

/// C ABI shim for `dirins::insert`. This is the ONLY `unsafe` in the #605 port,
/// and it does exactly one thing: turn the caller's (pointer, length) pairs into
/// slices of exactly that extent. Everything downstream is bounds-checked.
#[no_mangle]
pub extern "C" fn ext2_dirblock_insert_rs(
    blk: *mut u8,
    block_size: u32,
    name: *const u8,
    name_len: u32,
    child_ino: u32,
    ftype: u8,
) -> i32 {
    if blk.is_null() || name.is_null() {
        return dirins::INS_CORRUPT;
    }
    // A directory block is a kmalloc(block_size) buffer; block_size comes from
    // the mounted superblock (1024..4096 in practice). Refuse anything that
    // could not be a real block so the slice extent is always sane.
    if block_size == 0 || block_size > 65536 {
        return dirins::INS_CORRUPT;
    }
    // name_len is checked again inside dirins::insert; checked here too so the
    // `name` slice we build never claims more than the caller promised.
    if name_len == 0 || name_len > 255 {
        return dirins::INS_CORRUPT;
    }

    // SAFETY: the caller (fs/ext2.c ext2_dirblock_insert) guarantees `blk`
    // points to at least `block_size` contiguous readable AND writable bytes
    // (it is a freshly kmalloc(fs->block_size) buffer filled by
    // ext2_read_block), and that `name` points to at least `name_len` readable
    // bytes (the NUL-terminated component whose length the caller measured).
    // The two slices span exactly those extents and do not alias: `blk` is a
    // private heap buffer, `name` is a caller-owned string. From here on every
    // access is a bounds-checked slice index inside `dirins`, which forbids
    // unsafe code, so no disk-controlled rec_len / name_len can reach memory
    // outside the block.
    let block = unsafe { core::slice::from_raw_parts_mut(blk, block_size as usize) };
    let nm = unsafe { core::slice::from_raw_parts(name, name_len as usize) };
    dirins::insert(block, nm, child_ino, ftype)
}
