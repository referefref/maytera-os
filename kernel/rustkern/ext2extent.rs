// ext2extent.rs - #609. Pure block-extent math for the ext2 streaming writer.
//
// WHY THIS EXISTS
// ---------------
// fs/ext2.c used to append a large file ONE 4KB block at a time, and every
// single one of those blocks cost SIX device write transactions: the block
// bitmap, the group descriptor, the superblock free-count, the indirect
// pointer block, the inode, and finally the data. Measured on the real
// 103,563,185-byte OpenArena package with tools/ext2-harness: 25,284 data
// blocks -> 151,826 blk_write() calls, 1,062,742 sectors, i.e. 544 MB of
// device traffic for 103 MB of payload (5.26x amplification, 6.005 writes per
// block). On a USB-MSC or ATA root that is hundreds of thousands of device
// round trips, and the ext2 filesystem lock is held across all of them, so the
// whole box appears wedged.
//
// The fix is to append a RUN of contiguous blocks per metadata transaction.
// This module owns the two pure, I/O-free computations that requires:
//
//   1. find_run  - locate (and claim) a run of free bits in a block bitmap.
//   2. lblk_map  - map a logical block index onto the direct / single-indirect
//                  / double-indirect pointer that must hold it, AND report how
//                  many CONSECUTIVE logical blocks from there share that same
//                  pointer block (so the caller knows how large a batch it can
//                  publish with a single indirect-block write).
//
// Both operate on untrusted on-disk data (a bitmap block read off whatever
// disk is inserted; a logical index derived from a file size) and both are
// pure index arithmetic, which is exactly the class of code the Rust-first
// mandate is for: no I/O, no paging, no FPU, and every buffer access is a
// bounds-checked slice index rather than a C pointer walk.
//
// New in Rust (not a port): C had no run allocator at all.

/// Result of a bitmap run search: `len == 0` means "nothing free found".
#[repr(C)]
pub struct Ext2Run {
    pub start: u32,
    pub len: u32,
}

/// Where a logical block lives in the inode's pointer tree.
#[repr(C)]
pub struct Ext2LblkMap {
    /// 0 = direct i_block[], 1 = single-indirect, 2 = double-indirect,
    /// 3 = beyond double-indirect (unsupported by this driver).
    pub kind: u32,
    /// kind 0: index into i_block[]. kind 1: unused (0).
    /// kind 2: index into the double-indirect block (which single-indirect
    /// block holds this logical block).
    pub slot: u32,
    /// kind 0: unused (0). kind 1/2: index inside the single-indirect block.
    pub inner: u32,
    /// How many consecutive logical blocks starting AT `logical` (inclusive)
    /// are served by the same pointer block. Always >= 1 for kind 0/1/2.
    pub same_ptr_run: u32,
}

const NDIR: u32 = 12;

/// Claim a run of up to `max_run` consecutive free bits in a block bitmap.
///
/// `bm` points to `bm_len` writable bytes (one filesystem block). `bits` is
/// how many bits of that bitmap are actually in service (blocks_per_group);
/// it is clamped to what the buffer can hold, so a bogus superblock value can
/// never push the scan past the end of the buffer. The search starts at
/// `hint` and wraps once to 0.
///
/// On success the bits are SET (allocated) in `bm` and `out` receives the
/// first bit index and the run length; returns 1. Returns 0 if no free bit
/// exists, and -1 on a bad argument.
#[no_mangle]
pub extern "C" fn ext2_bitmap_find_run_rs(
    bm: *mut u8,
    bm_len: u32,
    bits: u32,
    hint: u32,
    max_run: u32,
    out: *mut Ext2Run,
) -> i32 {
    if bm.is_null() || out.is_null() || bm_len == 0 || bm_len > 65536 || max_run == 0 {
        return -1;
    }
    // SAFETY: the caller (fs/ext2.c ext2_alloc_run) guarantees `bm` points to
    // at least `bm_len` contiguous readable+writable bytes: it is a freshly
    // kmalloc(fs->block_size) buffer filled by ext2_read_block, and bm_len is
    // that same block_size. The slice spans exactly that extent and every
    // access below is a bounds-checked index into it.
    let map = unsafe { core::slice::from_raw_parts_mut(bm, bm_len as usize) };
    // SAFETY: `out` is a caller-owned Ext2Run on the C stack, non-null and
    // properly aligned (checked above for null; alignment is 4, guaranteed by
    // the C compiler for a u32-pair struct).
    let res = unsafe { &mut *out };
    res.start = 0;
    res.len = 0;

    let capacity = (bm_len as usize).saturating_mul(8);
    let nbits = core::cmp::min(bits as usize, capacity);
    if nbits == 0 {
        return 0;
    }
    let start_at = if (hint as usize) < nbits { hint as usize } else { 0 };

    // Two passes: [hint, nbits) then [0, hint). Never scans a bit twice.
    let mut pass_start = start_at;
    let mut pass_end = nbits;
    for pass in 0..2 {
        if pass == 1 {
            if start_at == 0 {
                break;
            }
            pass_start = 0;
            pass_end = start_at;
        }
        let mut i = pass_start;
        while i < pass_end {
            // Skip whole all-allocated bytes when byte-aligned: the bitmap of a
            // nearly-full group is mostly 0xFF and this is the hot loop.
            if i % 8 == 0 && map[i / 8] == 0xFF {
                i += 8;
                continue;
            }
            if map[i / 8] & (1u8 << (i % 8)) != 0 {
                i += 1;
                continue;
            }
            // Free bit at i: extend the run.
            let cap = core::cmp::min(max_run as usize, pass_end - i);
            let mut n = 0usize;
            while n < cap && map[(i + n) / 8] & (1u8 << ((i + n) % 8)) == 0 {
                n += 1;
            }
            for k in 0..n {
                map[(i + k) / 8] |= 1u8 << ((i + k) % 8);
            }
            res.start = i as u32;
            res.len = n as u32;
            return 1;
        }
    }
    0
}

/// Map a logical block index onto its pointer location, and report how many
/// consecutive logical blocks from there share the same pointer block.
///
/// `ptrs` is block_size/4 (pointers per indirect block). Returns 1 on success
/// (including kind 3 = unsupported range), -1 on a bad argument.
#[no_mangle]
pub extern "C" fn ext2_lblk_map_rs(logical: u32, ptrs: u32, out: *mut Ext2LblkMap) -> i32 {
    if out.is_null() || ptrs == 0 {
        return -1;
    }
    // SAFETY: caller-owned Ext2LblkMap on the C stack, non-null (checked) and
    // 4-byte aligned by construction.
    let m = unsafe { &mut *out };
    m.kind = 3;
    m.slot = 0;
    m.inner = 0;
    m.same_ptr_run = 0;

    if logical < NDIR {
        m.kind = 0;
        m.slot = logical;
        m.same_ptr_run = NDIR - logical; // rest of the direct array
        return 1;
    }
    let l1 = logical - NDIR;
    if l1 < ptrs {
        m.kind = 1;
        m.inner = l1;
        m.same_ptr_run = ptrs - l1;
        return 1;
    }
    let l2 = l1 - ptrs;
    // ptrs*ptrs cannot overflow u32 for any real block size (4096/4 = 1024 ->
    // 1_048_576), but use a widened compare so a garbage superblock cannot
    // wrap the check.
    if (l2 as u64) < (ptrs as u64) * (ptrs as u64) {
        m.kind = 2;
        m.slot = l2 / ptrs;
        m.inner = l2 % ptrs;
        m.same_ptr_run = ptrs - m.inner;
        return 1;
    }
    m.kind = 3; // triple-indirect: this driver does not support it
    1
}

// ---------------------------------------------------------------------------
// Self-tests, run at boot by ext2_extent_selftest() in fs/ext2.c. These assert
// the properties the FILESYSTEM depends on, not just that the code runs:
// a claimed run is never longer than asked, never crosses the bit limit, is
// idempotently marked, and lblk_map's same_ptr_run never lets a caller walk
// off the end of a pointer block.
// ---------------------------------------------------------------------------

/// Returns the number of FAILED checks (0 = all pass).
#[no_mangle]
pub extern "C" fn ext2_extent_selftest_rs() -> u32 {
    let mut fails: u32 = 0;
    let mut bm = [0u8; 512]; // 4096 bits
    let mut run = Ext2Run { start: 0, len: 0 };

    // (1) Empty bitmap: a full-length run at 0.
    if ext2_bitmap_find_run_rs(bm.as_mut_ptr(), 512, 4096, 0, 32, &mut run) != 1
        || run.start != 0
        || run.len != 32
    {
        fails += 1;
    }
    // ...and those 32 bits are now allocated, so the next call moves on.
    if ext2_bitmap_find_run_rs(bm.as_mut_ptr(), 512, 4096, 0, 32, &mut run) != 1
        || run.start != 32
        || run.len != 32
    {
        fails += 1;
    }
    // (2) A run is truncated at an allocated bit, never spans it.
    let mut bm2 = [0u8; 512];
    bm2[0] = 0b0001_0000; // bit 4 taken
    if ext2_bitmap_find_run_rs(bm2.as_mut_ptr(), 512, 4096, 0, 32, &mut run) != 1
        || run.start != 0
        || run.len != 4
    {
        fails += 1;
    }
    // (3) `bits` shorter than the buffer is honoured (never allocates past it).
    let mut bm3 = [0u8; 512];
    if ext2_bitmap_find_run_rs(bm3.as_mut_ptr(), 512, 10, 0, 64, &mut run) != 1
        || run.start != 0
        || run.len != 10
    {
        fails += 1;
    }
    // (4) A completely full bitmap reports "nothing free" instead of lying.
    let mut bm4 = [0xFFu8; 512];
    if ext2_bitmap_find_run_rs(bm4.as_mut_ptr(), 512, 4096, 0, 32, &mut run) != 0 {
        fails += 1;
    }
    // (5) hint wraps: bits 0..8 free, hint past the end of the free area.
    let mut bm5 = [0xFFu8; 512];
    bm5[0] = 0x00;
    if ext2_bitmap_find_run_rs(bm5.as_mut_ptr(), 512, 4096, 100, 32, &mut run) != 1
        || run.start != 0
        || run.len != 8
    {
        fails += 1;
    }

    // (6) lblk_map boundaries for a 4KB block (ptrs = 1024).
    let mut m = Ext2LblkMap { kind: 0, slot: 0, inner: 0, same_ptr_run: 0 };
    ext2_lblk_map_rs(0, 1024, &mut m);
    if m.kind != 0 || m.slot != 0 || m.same_ptr_run != 12 {
        fails += 1;
    }
    ext2_lblk_map_rs(11, 1024, &mut m);
    if m.kind != 0 || m.slot != 11 || m.same_ptr_run != 1 {
        fails += 1;
    }
    ext2_lblk_map_rs(12, 1024, &mut m); // first single-indirect block
    if m.kind != 1 || m.inner != 0 || m.same_ptr_run != 1024 {
        fails += 1;
    }
    ext2_lblk_map_rs(12 + 1023, 1024, &mut m); // last single-indirect block
    if m.kind != 1 || m.inner != 1023 || m.same_ptr_run != 1 {
        fails += 1;
    }
    ext2_lblk_map_rs(12 + 1024, 1024, &mut m); // first double-indirect block
    if m.kind != 2 || m.slot != 0 || m.inner != 0 || m.same_ptr_run != 1024 {
        fails += 1;
    }
    ext2_lblk_map_rs(12 + 1024 + 1025, 1024, &mut m); // second dind sub-block
    if m.kind != 2 || m.slot != 1 || m.inner != 1 || m.same_ptr_run != 1023 {
        fails += 1;
    }
    // Beyond double-indirect must be reported, never silently mapped.
    ext2_lblk_map_rs(12 + 1024 + 1024 * 1024, 1024, &mut m);
    if m.kind != 3 {
        fails += 1;
    }
    fails
}
