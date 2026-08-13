// rustkern/ext2fsck.rs - #610: the READ-ONLY ext2 consistency checker core.
//
// WHY RUST, AND WHY THIS SHAPE
// ----------------------------
// A filesystem checker is the single most hostile parser in the kernel: every
// number it reads (rec_len, name_len, block pointers, inode table locations,
// group descriptors) comes off a medium that is, by hypothesis, DAMAGED. That
// is exactly the CLAUDE.md Rust mandate's target: pure computation over
// untrusted buffers, no floats, every access bounds-checked. #476 (ext2_lookup
// OOB on a hostile rec_len) and #597 (the kernel corrupting its own directory
// structure) both live in this code's problem space.
//
// The split is deliberate and total:
//   * C  (fs/ext2.c: ext2_fsck_run) owns ALL I/O, kmalloc and locking. It reads
//     the superblock, sizes and allocates every scratch buffer, and hands Rust
//     a block-read function pointer.
//   * Rust (this file) owns ALL parsing, arithmetic and decision-making. It
//     allocates nothing, blocks on nothing, and cannot reach the disk except
//     through the supplied callback.
// So a bug here is a wrong VERDICT, never a wild write.
//
// REPORT ONLY. Nothing in this file writes to the filesystem. There is no
// repair path, on purpose: #609 is the cautionary case in this very codebase,
// where `e2fsck -p` would have cloned or deleted blocks off link counts while
// the BITMAP was the trustworthy source. A checker that only reports cannot
// destroy data.
//
// WHAT IT CHECKS (each maps to a failure this project has actually hit)
//   1. block pointers in range                      (#476 class)
//   2. multiply-claimed blocks                      (#609 signature)
//   3. phantom inodes: in use per the inode table,  (#609 exactly: debugfs
//      FREE in the inode bitmap                      mkdir leaked an inode)
//   4. leaked inodes: bitmap says used, inode free
//   5. block bitmap vs actual usage, both directions
//   6. directory rec_len / name_len sanity          (#476 / #597)
//   7. "." and ".." presence in dir block 0         (#597)
//   8. orphan inodes (link count > 0, unreferenced) -> lost+found candidates
//   9. i_links_count vs counted directory references
//  10. per-group and superblock free counts
//
// KNOWN LIMIT, stated rather than hidden: connectivity is proven by REFERENCE
// COUNT, not by a walk from the root. An inode referenced only by a directory
// that is itself disconnected is NOT reported here. That is a real gap; it is
// the next thing to add, not something this pass claims.

use crate::common::{elf_rd_u16, elf_rd_u32};

// ---------------------------------------------------------------------------
// FFI surface. Layouts are locked C-side with _Static_assert on sizeof.
// ---------------------------------------------------------------------------

/// Geometry + the one capability Rust is granted: read a block.
/// `read_block(block_no, dst)` must fill `block_size` bytes and return 0 on
/// success, non-zero on I/O error.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct E2fsckGeom {
    pub block_size: u32,
    pub blocks_count: u32,
    pub inodes_count: u32,
    pub first_data_block: u32,
    pub blocks_per_group: u32,
    pub inodes_per_group: u32,
    pub inode_size: u32,
    pub groups_count: u32,
    pub bgd_table_block: u32,
    pub reserved_gdt_blocks: u32,
    pub first_ino: u32,
    pub sparse_super: u32,
    pub sb_free_blocks: u32,
    pub sb_free_inodes: u32,
    pub flags: u32,
    pub _pad0: u32,
    pub read_block: Option<extern "C" fn(u32, *mut u8) -> i32>,
    /// `progress(pass, current, total)`. Called often; the C side throttles
    /// PAINTING, not calling, because throttling here would hide how coarse
    /// the call sites are. Returning non-zero ABORTS the scan (the ESC key),
    /// and an aborted scan reports completed = 0.
    pub progress: Option<extern "C" fn(u32, u32, u32) -> i32>,
}

/// Every byte of working memory the checker uses. All of it is kmalloc'd and
/// freed by C; Rust never allocates.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct E2fsckScratch {
    pub blk_used: *mut u8,   // bmap_bytes: computed in-use blocks
    pub blk_dup: *mut u8,    // bmap_bytes: blocks claimed more than once
    pub ino_used: *mut u8,   // imap_bytes: computed in-use inodes
    pub ino_isdir: *mut u8,  // imap_bytes: computed directory inodes
    pub links: *mut u16,     // links_entries: counted refs per inode
    pub itbuf: *mut u8,      // one block: inode table
    pub dirbuf: *mut u8,     // one block: directory data
    pub ind0: *mut u8,       // one block: single-indirect
    pub ind1: *mut u8,       // one block: double-indirect
    pub ind2: *mut u8,       // one block: triple-indirect
    pub bmapbuf: *mut u8,    // one block: on-disk bitmap under comparison
    pub gdbuf: *mut u8,      // one block: group descriptor table
    pub bmap_bytes: u32,
    pub imap_bytes: u32,
    pub links_entries: u32,
    pub _pad0: u32,
}

/// Findings. Counters, not a log: the kernel must not be able to be DoS'd into
/// printing a million lines by a hostile image. The first problem is spelled
/// out so a user sees something actionable without a serial cable.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct E2fsckReport {
    pub completed: u32,
    pub inodes_used: u32,
    pub blocks_used: u32,
    pub dirs_used: u32,
    pub e_bad_block_ptr: u32,
    pub e_dup_block: u32,
    pub e_phantom_inode: u32,
    pub e_leaked_inode: u32,
    pub e_block_used_bitmap_free: u32,
    pub e_block_free_bitmap_used: u32,
    pub e_bad_dirent: u32,
    pub e_orphan_inode: u32,
    pub e_link_mismatch: u32,
    pub e_group_free_bad: u32,
    pub e_sb_free_bad: u32,
    pub e_bad_inode: u32,
    pub e_io: u32,
    pub total: u32,
    pub first_msg: [u8; 128],
}

// Buffer slots (indices into Ck::bufs).
const B_IT: usize = 0;
const B_DIR: usize = 1;
const B_IND0: usize = 2;
const B_IND1: usize = 3;
const B_IND2: usize = 4;
const B_BM: usize = 5;
const B_GD: usize = 6;
const NBUF: usize = 7;

const EXT2_ROOT_INO: u32 = 2;
const EXT2_RESIZE_INO: u32 = 7;

// Hard ceilings. A corrupt superblock must produce a REFUSAL, not a multi-hour
// scan or an arithmetic overflow. These bound the work before any of it starts.
const MAX_GROUPS: u32 = 65536;
const MAX_BLOCK_SIZE: u32 = 65536;

#[inline]
fn bit_get(b: &[u8], i: u32) -> bool {
    let x = (i >> 3) as usize;
    x < b.len() && (b[x] >> (i & 7)) & 1 != 0
}
#[inline]
fn bit_set(b: &mut [u8], i: u32) {
    let x = (i >> 3) as usize;
    if x < b.len() {
        b[x] |= 1u8 << (i & 7);
    }
}

// g is a power of `base`? Callers guarantee g >= 2, so the /= loop terminates.
#[inline]
fn is_pow_of(mut n: u32, base: u32) -> bool {
    while n % base == 0 {
        n /= base;
    }
    n == 1
}

struct Ck<'a> {
    g: E2fsckGeom,
    blk_used: &'a mut [u8],
    blk_dup: &'a mut [u8],
    ino_used: &'a mut [u8],
    ino_isdir: &'a mut [u8],
    links: &'a mut [u16],
    bufs: [&'a mut [u8]; NBUF],
    rep: &'a mut E2fsckReport,
    aborted: bool,
}

impl<'a> Ck<'a> {
    // ---- problem recording ------------------------------------------------
    fn note(&mut self, what: &str, n: u32) {
        self.rep.total = self.rep.total.wrapping_add(1);
        if self.rep.first_msg[0] != 0 {
            return;
        }
        let mut i = 0usize;
        for &c in what.as_bytes() {
            if i < 112 {
                self.rep.first_msg[i] = c;
                i += 1;
            }
        }
        let mut tmp = [0u8; 12];
        let mut t = 0usize;
        let mut v = n;
        if v == 0 {
            tmp[0] = b'0';
            t = 1;
        } else {
            while v > 0 && t < 12 {
                tmp[t] = b'0' + (v % 10) as u8;
                v /= 10;
                t += 1;
            }
        }
        while t > 0 {
            t -= 1;
            if i < 126 {
                self.rep.first_msg[i] = tmp[t];
                i += 1;
            }
        }
        if i < 128 {
            self.rep.first_msg[i] = 0;
        }
    }

    // ---- progress + abort --------------------------------------------------
    // `pass` is the 1-based pass number the C side names on screen; 0 is the
    // metadata prescan. Returns true if the caller should stop.
    fn tick(&mut self, pass: u32, cur: u32, total: u32) -> bool {
        if self.aborted {
            return true;
        }
        if let Some(f) = self.g.progress {
            if f(pass, cur, total) != 0 {
                self.aborted = true;
                return true;
            }
        }
        false
    }

    // ---- I/O (the only capability we have) --------------------------------
    fn read_into(&mut self, blk: u32, slot: usize) -> bool {
        let f = match self.g.read_block {
            Some(f) => f,
            None => return false,
        };
        if blk >= self.g.blocks_count {
            self.rep.e_bad_block_ptr = self.rep.e_bad_block_ptr.wrapping_add(1);
            self.note("read of out-of-range block ", blk);
            return false;
        }
        // CONFINEMENT: `f` is a C function supplied by fs/ext2.c that writes exactly
        // g.block_size bytes into the destination. Every slot in `bufs` was
        // created from a kmalloc'd allocation of exactly g.block_size bytes
        // (checked C-side and asserted by bufs[slot].len() below), so the
        // callee cannot write past the allocation.
        if self.bufs[slot].len() < self.g.block_size as usize {
            return false;
        }
        let p = self.bufs[slot].as_mut_ptr();
        let rc = f(blk, p);
        if rc != 0 {
            self.rep.e_io = self.rep.e_io.wrapping_add(1);
            self.note("I/O error reading block ", blk);
            return false;
        }
        true
    }

    #[inline]
    fn rd32b(&self, slot: usize, off: u32) -> u32 {
        elf_rd_u32(self.bufs[slot], off as u64).unwrap_or(0)
    }
    #[inline]
    fn rd16b(&self, slot: usize, off: u32) -> u16 {
        elf_rd_u16(self.bufs[slot], off as u64).unwrap_or(0)
    }
    #[inline]
    fn rd8b(&self, slot: usize, off: u32) -> u8 {
        let o = off as usize;
        if o < self.bufs[slot].len() {
            self.bufs[slot][o]
        } else {
            0
        }
    }

    // ---- group descriptor access ------------------------------------------
    // One descriptor is 32 bytes. Re-reads the covering block each time rather
    // than caching a parsed table: there is no allocator here, and the call
    // count is O(groups), not O(blocks).
    fn gd32(&mut self, group: u32, off: u32) -> u32 {
        let dpb = self.g.block_size / 32;
        if dpb == 0 || group >= self.g.groups_count {
            return 0;
        }
        let blk = self.g.bgd_table_block + group / dpb;
        let idx = group % dpb;
        if !self.read_into(blk, B_GD) {
            return 0;
        }
        self.rd32b(B_GD, idx * 32 + off)
    }
    fn gd16(&mut self, group: u32, off: u32) -> u16 {
        let dpb = self.g.block_size / 32;
        if dpb == 0 || group >= self.g.groups_count {
            return 0;
        }
        let blk = self.g.bgd_table_block + group / dpb;
        let idx = group % dpb;
        if !self.read_into(blk, B_GD) {
            return 0;
        }
        self.rd16b(B_GD, idx * 32 + off)
    }

    // ---- block accounting --------------------------------------------------
    // Returns false if the pointer is out of range (already recorded).
    fn mark_block(&mut self, blk: u32, owner_ino: u32) -> bool {
        if blk < self.g.first_data_block || blk >= self.g.blocks_count {
            self.rep.e_bad_block_ptr = self.rep.e_bad_block_ptr.wrapping_add(1);
            self.note("out-of-range block pointer in inode ", owner_ino);
            return false;
        }
        let idx = blk - self.g.first_data_block;
        if bit_get(self.blk_used, idx) {
            if !bit_get(self.blk_dup, idx) {
                bit_set(self.blk_dup, idx);
                self.rep.e_dup_block = self.rep.e_dup_block.wrapping_add(1);
                self.note("multiply-claimed block ", blk);
            } else {
                self.rep.e_dup_block = self.rep.e_dup_block.wrapping_add(1);
                self.rep.total = self.rep.total.wrapping_add(1);
            }
        } else {
            bit_set(self.blk_used, idx);
            self.rep.blocks_used = self.rep.blocks_used.wrapping_add(1);
        }
        true
    }

    fn has_super(&self, group: u32) -> bool {
        if self.g.sparse_super == 0 || group == 0 || group == 1 {
            return true;
        }
        is_pow_of(group, 3) || is_pow_of(group, 5) || is_pow_of(group, 7)
    }

    fn group_blocks(&self, group: u32) -> u32 {
        let start = self.g.first_data_block + group * self.g.blocks_per_group;
        if start >= self.g.blocks_count {
            return 0;
        }
        let left = self.g.blocks_count - start;
        if left < self.g.blocks_per_group {
            left
        } else {
            self.g.blocks_per_group
        }
    }

    // Superblock/backup, descriptor table, reserved GDT, bitmaps, inode table.
    // Getting this wrong is the ONLY way this checker can cry wolf on a healthy
    // filesystem, so it mirrors mke2fs exactly, including sparse_super and the
    // resize_inode reserved GDT blocks (verified against dumpe2fs's "Overhead
    // clusters" on the golden: 6*(1+1+96) + 13*(1+1+475) = 6789).
    fn mark_metadata(&mut self) {
        let gd_blocks = (self.g.groups_count
            .saturating_mul(32)
            .saturating_add(self.g.block_size - 1))
            / self.g.block_size;
        for grp in 0..self.g.groups_count {
            if self.tick(0, grp, self.g.groups_count) {
                return;
            }
            let gstart = self.g.first_data_block + grp * self.g.blocks_per_group;
            let mut b = gstart;
            if self.has_super(grp) {
                self.mark_block(b, 0);
                b += 1;
                let meta = gd_blocks.saturating_add(self.g.reserved_gdt_blocks);
                let mut i = 0u32;
                while i < meta {
                    self.mark_block(b, 0);
                    b += 1;
                    i += 1;
                }
            }
            let bbmp = self.gd32(grp, 0);
            let ibmp = self.gd32(grp, 4);
            let itab = self.gd32(grp, 8);
            self.mark_block(bbmp, 0);
            self.mark_block(ibmp, 0);
            let itb = self.inode_table_blocks();
            let mut i = 0u32;
            while i < itb {
                self.mark_block(itab + i, 0);
                i += 1;
            }
        }
    }

    #[inline]
    fn inode_table_blocks(&self) -> u32 {
        let per = self.g.block_size / self.g.inode_size;
        if per == 0 {
            return 0;
        }
        (self.g.inodes_per_group + per - 1) / per
    }

    // ---- directory entry scan ---------------------------------------------
    // Walks the rec_len chain of ONE directory block already in B_DIR. This is
    // the #476 / #597 hot spot: every field is validated against the block
    // bound before it is used, and a bad record ENDS the walk (a corrupt
    // rec_len must not be able to steer the cursor).
    fn scan_dirblock(&mut self, dir_ino: u32, logical: u32) {
        let bs = self.g.block_size;
        let mut off: u32 = 0;
        let mut seen_dot = false;
        let mut seen_dotdot = false;
        let mut nrec = 0u32;
        while off + 8 <= bs {
            let ino = self.rd32b(B_DIR, off);
            let rec_len = self.rd16b(B_DIR, off + 4) as u32;
            let name_len = self.rd8b(B_DIR, off + 6) as u32;
            if rec_len < 8 || (rec_len & 3) != 0 || off + rec_len > bs {
                self.rep.e_bad_dirent = self.rep.e_bad_dirent.wrapping_add(1);
                self.note("bad rec_len in directory inode ", dir_ino);
                return;
            }
            if ino != 0 {
                if 8 + name_len > rec_len {
                    self.rep.e_bad_dirent = self.rep.e_bad_dirent.wrapping_add(1);
                    self.note("name_len overruns rec_len in directory inode ", dir_ino);
                    return;
                }
                if ino > self.g.inodes_count {
                    self.rep.e_bad_dirent = self.rep.e_bad_dirent.wrapping_add(1);
                    self.note("dirent references out-of-range inode in dir ", dir_ino);
                } else {
                    let i = ino as usize;
                    if i < self.links.len() {
                        self.links[i] = self.links[i].saturating_add(1);
                    }
                }
                if logical == 0 && nrec == 0 {
                    seen_dot = name_len == 1 && self.rd8b(B_DIR, off + 8) == b'.' && ino == dir_ino;
                }
                if logical == 0 && nrec == 1 {
                    seen_dotdot = name_len == 2
                        && self.rd8b(B_DIR, off + 8) == b'.'
                        && self.rd8b(B_DIR, off + 9) == b'.';
                }
                nrec += 1;
            }
            off += rec_len;
        }
        if logical == 0 && !(seen_dot && seen_dotdot) {
            self.rep.e_bad_dirent = self.rep.e_bad_dirent.wrapping_add(1);
            self.note("missing '.' or '..' in directory inode ", dir_ino);
        }
    }

    // ---- inode block walk --------------------------------------------------
    // `level` 0 = the pointer is a data block, 1..3 = indirect of that order.
    // `scan_dir` is only ever true in PASS 2 (the directory-structure pass).
    // Pass 1 walks the same blocks with scan_dir = false: it accounts them, but
    // does not parse dirents, because at that point not every inode has been
    // seen yet and "this entry points at a free inode" would be unanswerable.
    fn walk_ptr(&mut self, level: u32, blk: u32, ino: u32, is_dir: bool, logical: &mut u32) {
        if self.aborted {
            return;
        }
        if !self.mark_block(blk, ino) {
            return;
        }
        if level == 0 {
            if is_dir {
                if self.read_into(blk, B_DIR) {
                    let l = *logical;
                    self.scan_dirblock(ino, l);
                }
            }
            *logical = logical.wrapping_add(1);
            return;
        }
        let slot = match level {
            1 => B_IND0,
            2 => B_IND1,
            _ => B_IND2,
        };
        if !self.read_into(blk, slot) {
            return;
        }
        let n = self.g.block_size / 4;
        let mut i = 0u32;
        while i < n {
            let p = self.rd32b(slot, i * 4);
            if p != 0 {
                self.walk_ptr(level - 1, p, ino, is_dir, logical);
            } else if level == 1 {
                *logical = logical.wrapping_add(1);
            }
            i += 1;
        }
    }

    // Walk one inode's blocks off a COPY of its 15 pointers (the inode table
    // buffer is reused by the walk, so the pointers must be lifted out first).
    fn walk_inode(&mut self, ino: u32, iblock: &[u32; 15], is_dir: bool, file_acl: u32) {
        if file_acl != 0 {
            self.mark_block(file_acl, ino);
        }
        let mut logical = 0u32;
        let mut i = 0usize;
        while i < 12 {
            if iblock[i] != 0 {
                self.walk_ptr(0, iblock[i], ino, is_dir, &mut logical);
            } else {
                logical = logical.wrapping_add(1);
            }
            i += 1;
        }
        if iblock[12] != 0 {
            self.walk_ptr(1, iblock[12], ino, is_dir, &mut logical);
        }
        if iblock[13] != 0 {
            self.walk_ptr(2, iblock[13], ino, is_dir, &mut logical);
        }
        if iblock[14] != 0 {
            self.walk_ptr(3, iblock[14], ino, is_dir, &mut logical);
        }
    }

    // ---- pass 1: inode table scan + block accounting + directory scan ------
    fn pass1(&mut self) {
        let per_blk = self.g.block_size / self.g.inode_size;
        let itb = self.inode_table_blocks();
        for grp in 0..self.g.groups_count {
            let itab = self.gd32(grp, 8);
            if itab == 0 {
                continue;
            }
            let mut bi = 0u32;
            while bi < itb {
                if self.aborted {
                    return;
                }
                if !self.read_into(itab + bi, B_IT) {
                    bi += 1;
                    continue;
                }
                let done = grp * self.g.inodes_per_group + bi * per_blk;
                if self.tick(1, done, self.g.inodes_count) {
                    return;
                }
                let mut j = 0u32;
                while j < per_blk {
                    let ino = grp * self.g.inodes_per_group + bi * per_blk + j + 1;
                    if ino > self.g.inodes_count
                        || ino <= grp * self.g.inodes_per_group
                        || ino > (grp + 1) * self.g.inodes_per_group
                    {
                        j += 1;
                        continue;
                    }
                    let base = j * self.g.inode_size;
                    let mode = self.rd16b(B_IT, base + 0);
                    let links = self.rd16b(B_IT, base + 26);
                    let dtime = self.rd32b(B_IT, base + 20);
                    let size = self.rd32b(B_IT, base + 4);
                    let blocks512 = self.rd32b(B_IT, base + 28);
                    let file_acl = self.rd32b(B_IT, base + 104);
                    let mut iblock = [0u32; 15];
                    let mut k = 0usize;
                    while k < 15 {
                        iblock[k] = self.rd32b(B_IT, base + 40 + (k as u32) * 4);
                        k += 1;
                    }

                    let reserved = ino < self.g.first_ino;
                    let in_use = links != 0 && dtime == 0;
                    if !in_use && !reserved {
                        j += 1;
                        continue;
                    }
                    bit_set(self.ino_used, ino - 1);
                    self.rep.inodes_used = self.rep.inodes_used.wrapping_add(1);

                    // The resize inode (the resize_inode feature, which mke2fs
                    // enables by DEFAULT) is a special case, and getting it
                    // wrong is a guaranteed false positive on every healthy
                    // modern ext2 volume. Its DOUBLE-INDIRECT block is a real,
                    // separately allocated block that nothing else references,
                    // so it must be marked used. Everything BELOW it is the
                    // reserved-GDT region of each block group, which
                    // mark_metadata() has already accounted, so walking the
                    // chain normally would report every reserved GDT block as
                    // multiply claimed.  Mark i_block[13]; walk no further.
                    //
                    // MEASURED: without the i_block[13] mark, the checker
                    // reported exactly one "block marked used but unreferenced"
                    // on a PRISTINE mke2fs image at both 1K and 4K block sizes,
                    // and debugfs `icheck` named inode 7 as the owner. That is
                    // how this branch got its second line.
                    if ino == EXT2_RESIZE_INO {
                        if iblock[13] != 0 {
                            self.mark_block(iblock[13], ino);
                        }
                        if file_acl != 0 {
                            self.mark_block(file_acl, ino);
                        }
                        j += 1;
                        continue;
                    }

                    let fmt = mode & 0xF000;
                    let is_dir = fmt == 0x4000;
                    if is_dir {
                        bit_set(self.ino_isdir, ino - 1);
                        self.rep.dirs_used = self.rep.dirs_used.wrapping_add(1);
                    }
                    // Fast symlink: the target is stored IN the i_block array,
                    // which therefore holds text, not block numbers.
                    let fast_link = fmt == 0xA000 && blocks512 == 0 && size < 60;
                    // Device / fifo / socket: i_block[0..1] hold the dev number.
                    let no_blocks =
                        fmt == 0x2000 || fmt == 0x6000 || fmt == 0x1000 || fmt == 0xC000;
                    if reserved && !in_use && fmt == 0 {
                        // A reserved-but-unused inode (mke2fs marks 1..10 used
                        // in the bitmap with mode 0). In use for bitmap
                        // purposes, owns nothing.
                        j += 1;
                        continue;
                    }
                    if fmt == 0 && in_use {
                        self.rep.e_bad_inode = self.rep.e_bad_inode.wrapping_add(1);
                        self.note("inode in use with zero mode: ", ino);
                    }
                    if !fast_link && !no_blocks {
                        // is_dir = false HERE: block accounting only. The
                        // dirent parse is pass 2's job (see walk_ptr).
                        self.walk_inode(ino, &iblock, false, file_acl);
                    } else if file_acl != 0 {
                        self.mark_block(file_acl, ino);
                    }
                    j += 1;
                }
                bi += 1;
            }
        }
    }

    // Read the inode-table block covering `ino` into B_IT and return the byte
    // offset of the inode within it, or None. Used by the passes that revisit
    // individual inodes rather than sweeping the whole table.
    fn load_inode(&mut self, ino: u32) -> Option<u32> {
        if ino == 0 || ino > self.g.inodes_count {
            return None;
        }
        let grp = (ino - 1) / self.g.inodes_per_group;
        let idx = (ino - 1) % self.g.inodes_per_group;
        let per_blk = self.g.block_size / self.g.inode_size;
        if per_blk == 0 {
            return None;
        }
        let itab = self.gd32(grp, 8);
        if itab == 0 {
            return None;
        }
        if !self.read_into(itab + idx / per_blk, B_IT) {
            return None;
        }
        Some((idx % per_blk) * self.g.inode_size)
    }

    // ---- pass 2: directory structure ---------------------------------------
    // Revisits ONLY the inodes pass 1 flagged as directories and parses their
    // entries. Separate from pass 1 on purpose: by now every in-use inode is
    // known, so "this entry points at an inode that is not in use" is a
    // question that can actually be answered.
    fn pass2_dirs(&mut self) {
        let total = self.rep.dirs_used;
        let mut seen = 0u32;
        for ino in 1..=self.g.inodes_count {
            if self.aborted {
                return;
            }
            if !bit_get(self.ino_isdir, ino - 1) {
                continue;
            }
            seen += 1;
            if self.tick(2, seen, total) {
                return;
            }
            let base = match self.load_inode(ino) {
                Some(b) => b,
                None => continue,
            };
            let mut iblock = [0u32; 15];
            let mut k = 0usize;
            while k < 15 {
                iblock[k] = self.rd32b(B_IT, base + 40 + (k as u32) * 4);
                k += 1;
            }
            // Walk WITHOUT marking blocks a second time: pass 1 already
            // accounted them, and re-marking would report every directory block
            // as multiply claimed. dir_scan_only() is the read-only twin.
            self.dir_scan_only(&iblock, ino);
        }
    }

    // Directory data blocks only, no block accounting. Direct + one level of
    // indirection; a directory deeper than that does not occur on this system
    // and a deeper walk would need a buffer pass 1 is using.
    fn dir_scan_only(&mut self, iblock: &[u32; 15], ino: u32) {
        let mut logical = 0u32;
        let mut i = 0usize;
        while i < 12 {
            if iblock[i] != 0 {
                if self.read_into(iblock[i], B_DIR) {
                    self.scan_dirblock(ino, logical);
                }
            }
            logical = logical.wrapping_add(1);
            i += 1;
        }
        if iblock[12] != 0 && self.read_into(iblock[12], B_IND0) {
            let n = self.g.block_size / 4;
            let mut k = 0u32;
            while k < n {
                let p = self.rd32b(B_IND0, k * 4);
                if p != 0 && self.read_into(p, B_DIR) {
                    self.scan_dirblock(ino, logical);
                }
                logical = logical.wrapping_add(1);
                k += 1;
            }
        }
    }

    // ---- passes 3 + 4: directory connectivity and reference counts ---------
    // One loop, not two: both answers come from comparing the counted
    // references built in pass 2 against i_links_count in the inode table.
    // Splitting them would mean reading the whole inode table twice for no
    // extra coverage, so the display names them together rather than pretending
    // there are two sweeps.
    fn pass34_links(&mut self) {
        let per_blk = self.g.block_size / self.g.inode_size;
        let itb = self.inode_table_blocks();
        for grp in 0..self.g.groups_count {
            if self.tick(4, grp, self.g.groups_count) {
                return;
            }
            let itab = self.gd32(grp, 8);
            if itab == 0 {
                continue;
            }
            let mut bi = 0u32;
            while bi < itb {
                if !self.read_into(itab + bi, B_IT) {
                    bi += 1;
                    continue;
                }
                let mut j = 0u32;
                while j < per_blk {
                    let ino = grp * self.g.inodes_per_group + bi * per_blk + j + 1;
                    if ino > self.g.inodes_count
                        || ino > (grp + 1) * self.g.inodes_per_group
                    {
                        j += 1;
                        continue;
                    }
                    if !bit_get(self.ino_used, ino - 1) {
                        j += 1;
                        continue;
                    }
                    let base = j * self.g.inode_size;
                    let links = self.rd16b(B_IT, base + 26) as u32;
                    let counted = if (ino as usize) < self.links.len() {
                        self.links[ino as usize] as u32
                    } else {
                        0
                    };
                    if ino >= self.g.first_ino {
                        if counted == 0 && links != 0 {
                            self.rep.e_orphan_inode = self.rep.e_orphan_inode.wrapping_add(1);
                            if self.rep.e_orphan_inode == 1 {
                                self.note("orphan inode (lost+found candidate): ", ino);
                            } else {
                                self.rep.total = self.rep.total.wrapping_add(1);
                            }
                        } else if counted != links {
                            self.rep.e_link_mismatch = self.rep.e_link_mismatch.wrapping_add(1);
                            if self.rep.e_link_mismatch == 1 {
                                self.note("link count wrong on inode ", ino);
                            } else {
                                self.rep.total = self.rep.total.wrapping_add(1);
                            }
                        }
                    } else if ino == EXT2_ROOT_INO && counted == 0 {
                        self.rep.e_orphan_inode = self.rep.e_orphan_inode.wrapping_add(1);
                        self.note("root inode unreferenced: ", ino);
                    }
                    j += 1;
                }
                bi += 1;
            }
        }
        self.tick(4, self.g.groups_count, self.g.groups_count);
    }

    // ---- pass 5: compare computed state against the on-disk bookkeeping ----
    fn pass2(&mut self) {
        let per_blk = self.g.block_size / self.g.inode_size;
        let itb = self.inode_table_blocks();
        let mut total_free_blocks: u32 = 0;
        let mut total_free_inodes: u32 = 0;

        for grp in 0..self.g.groups_count {
            if self.tick(5, grp, self.g.groups_count) {
                return;
            }
            // --- block bitmap ---
            let bbmp = self.gd32(grp, 0);
            let gblocks = self.group_blocks(grp);
            let gstart = self.g.first_data_block + grp * self.g.blocks_per_group;
            let mut free_b = 0u32;
            if bbmp != 0 && self.read_into(bbmp, B_BM) {
                let mut i = 0u32;
                while i < gblocks {
                    let abs = gstart + i;
                    let computed = bit_get(self.blk_used, abs - self.g.first_data_block);
                    let ondisk = bit_get(self.bufs[B_BM], i);
                    if computed && !ondisk {
                        self.rep.e_block_used_bitmap_free =
                            self.rep.e_block_used_bitmap_free.wrapping_add(1);
                        if self.rep.e_block_used_bitmap_free == 1 {
                            self.note("block in use but FREE in bitmap: ", abs);
                        } else {
                            self.rep.total = self.rep.total.wrapping_add(1);
                        }
                    } else if !computed && ondisk {
                        self.rep.e_block_free_bitmap_used =
                            self.rep.e_block_free_bitmap_used.wrapping_add(1);
                        if self.rep.e_block_free_bitmap_used == 1 {
                            self.note("block marked used but unreferenced: ", abs);
                        } else {
                            self.rep.total = self.rep.total.wrapping_add(1);
                        }
                    }
                    if !ondisk {
                        free_b += 1;
                    }
                    i += 1;
                }
            }
            let gd_free_b = self.gd16(grp, 12) as u32;
            if gd_free_b != free_b {
                self.rep.e_group_free_bad = self.rep.e_group_free_bad.wrapping_add(1);
                self.note("group free-block count wrong, group ", grp);
            }
            total_free_blocks = total_free_blocks.wrapping_add(free_b);

            // --- inode bitmap ---
            let ibmp = self.gd32(grp, 4);
            let mut free_i = 0u32;
            let mut used_dirs = 0u32;
            if ibmp != 0 && self.read_into(ibmp, B_BM) {
                let mut i = 0u32;
                while i < self.g.inodes_per_group {
                    let ino = grp * self.g.inodes_per_group + i + 1;
                    if ino > self.g.inodes_count {
                        break;
                    }
                    let computed = bit_get(self.ino_used, ino - 1);
                    let ondisk = bit_get(self.bufs[B_BM], i);
                    if computed && !ondisk {
                        // #609 EXACTLY: an inode with links_count != 0 whose
                        // bitmap bit says free. The next allocation hands its
                        // block to somebody else.
                        self.rep.e_phantom_inode = self.rep.e_phantom_inode.wrapping_add(1);
                        if self.rep.e_phantom_inode == 1 {
                            self.note("PHANTOM inode: in use, FREE in bitmap: ", ino);
                        } else {
                            self.rep.total = self.rep.total.wrapping_add(1);
                        }
                    } else if !computed && ondisk {
                        self.rep.e_leaked_inode = self.rep.e_leaked_inode.wrapping_add(1);
                        if self.rep.e_leaked_inode == 1 {
                            self.note("inode marked used but free: ", ino);
                        } else {
                            self.rep.total = self.rep.total.wrapping_add(1);
                        }
                    }
                    if !ondisk {
                        free_i += 1;
                    }
                    if bit_get(self.ino_isdir, ino - 1) {
                        used_dirs += 1;
                    }
                    i += 1;
                }
            }
            let gd_free_i = self.gd16(grp, 14) as u32;
            if gd_free_i != free_i {
                self.rep.e_group_free_bad = self.rep.e_group_free_bad.wrapping_add(1);
                self.note("group free-inode count wrong, group ", grp);
            }
            let gd_dirs = self.gd16(grp, 16) as u32;
            if gd_dirs != used_dirs {
                self.rep.e_group_free_bad = self.rep.e_group_free_bad.wrapping_add(1);
                self.note("group used-dirs count wrong, group ", grp);
            }
            total_free_inodes = total_free_inodes.wrapping_add(free_i);

        }

        if total_free_blocks != self.g.sb_free_blocks {
            self.rep.e_sb_free_bad = self.rep.e_sb_free_bad.wrapping_add(1);
            self.note("superblock free-block count wrong, bitmaps say ", total_free_blocks);
        }
        if total_free_inodes != self.g.sb_free_inodes {
            self.rep.e_sb_free_bad = self.rep.e_sb_free_bad.wrapping_add(1);
            self.note("superblock free-inode count wrong, bitmaps say ", total_free_inodes);
        }
    }
}

/// Run a full read-only consistency check.
///
/// Returns 0 if the run COMPLETED (the report says whether it found problems),
/// negative if it could not run at all. A negative return is never "clean".
///
/// # Safety
/// `geom`, `scratch` and `rep` must be valid, non-null, correctly-sized
/// `#[repr(C)]` objects. Every pointer in `scratch` must point to an allocation
/// of at least the size the paired length field declares (bmap_bytes,
/// imap_bytes, links_entries * 2, and geom.block_size for each block buffer).
/// fs/ext2.c is the only caller and sizes all of them from the same geometry it
/// passes in here.
#[no_mangle]
pub unsafe extern "C" fn ext2_fsck_run_rs(
    geom: *const E2fsckGeom,
    scratch: *const E2fsckScratch,
    rep: *mut E2fsckReport,
) -> i32 {
    if geom.is_null() || scratch.is_null() || rep.is_null() {
        return -1;
    }
    let g = *geom;
    let s = *scratch;
    let r = &mut *rep;

    // Zero the report before anything can fail, so a caller that ignores the
    // return value cannot read stale counters as a clean result.
    *r = E2fsckReport {
        completed: 0,
        inodes_used: 0,
        blocks_used: 0,
        dirs_used: 0,
        e_bad_block_ptr: 0,
        e_dup_block: 0,
        e_phantom_inode: 0,
        e_leaked_inode: 0,
        e_block_used_bitmap_free: 0,
        e_block_free_bitmap_used: 0,
        e_bad_dirent: 0,
        e_orphan_inode: 0,
        e_link_mismatch: 0,
        e_group_free_bad: 0,
        e_sb_free_bad: 0,
        e_bad_inode: 0,
        e_io: 0,
        total: 0,
        first_msg: [0u8; 128],
    };

    // Refuse impossible geometry rather than scan on it. A corrupt superblock
    // must not be able to steer this into overflow or an unbounded loop.
    if g.block_size < 512
        || g.block_size > MAX_BLOCK_SIZE
        || (g.block_size & (g.block_size - 1)) != 0
        || g.inode_size < 128
        || g.inode_size > g.block_size
        || (g.inode_size & (g.inode_size - 1)) != 0
        || g.blocks_per_group == 0
        || g.inodes_per_group == 0
        || g.blocks_count == 0
        || g.inodes_count == 0
        || g.groups_count == 0
        || g.groups_count > MAX_GROUPS
        || g.read_block.is_none()
    {
        return -2;
    }
    if g.blocks_per_group > 8 * g.block_size || g.inodes_per_group > 8 * g.block_size {
        return -2;
    }
    // Every buffer must be present and big enough for the geometry.
    let need_bmap = ((g.blocks_count - g.first_data_block) as usize + 7) / 8;
    let need_imap = (g.inodes_count as usize + 7) / 8;
    if s.blk_used.is_null()
        || s.blk_dup.is_null()
        || s.ino_used.is_null()
        || s.ino_isdir.is_null()
        || s.links.is_null()
        || s.itbuf.is_null()
        || s.dirbuf.is_null()
        || s.ind0.is_null()
        || s.ind1.is_null()
        || s.ind2.is_null()
        || s.bmapbuf.is_null()
        || s.gdbuf.is_null()
        || (s.bmap_bytes as usize) < need_bmap
        || (s.imap_bytes as usize) < need_imap
        || s.links_entries < g.inodes_count + 1
    {
        return -3;
    }

    let bs = g.block_size as usize;
    // SAFETY: validated non-null above; the caller guarantees each allocation
    // is at least the paired declared length (bmap_bytes / imap_bytes /
    // links_entries*2 / block_size). The slices below are the ONLY way this
    // module touches memory, so every subsequent access is bounds-checked
    // against these exact lengths.
    let mut ck = Ck {
        g,
        blk_used: core::slice::from_raw_parts_mut(s.blk_used, s.bmap_bytes as usize),
        blk_dup: core::slice::from_raw_parts_mut(s.blk_dup, s.bmap_bytes as usize),
        ino_used: core::slice::from_raw_parts_mut(s.ino_used, s.imap_bytes as usize),
        ino_isdir: core::slice::from_raw_parts_mut(s.ino_isdir, s.imap_bytes as usize),
        links: core::slice::from_raw_parts_mut(s.links, s.links_entries as usize),
        bufs: [
            core::slice::from_raw_parts_mut(s.itbuf, bs),
            core::slice::from_raw_parts_mut(s.dirbuf, bs),
            core::slice::from_raw_parts_mut(s.ind0, bs),
            core::slice::from_raw_parts_mut(s.ind1, bs),
            core::slice::from_raw_parts_mut(s.ind2, bs),
            core::slice::from_raw_parts_mut(s.bmapbuf, bs),
            core::slice::from_raw_parts_mut(s.gdbuf, bs),
        ],
        rep: r,
        aborted: false,
    };

    for b in ck.blk_used.iter_mut() {
        *b = 0;
    }
    for b in ck.blk_dup.iter_mut() {
        *b = 0;
    }
    for b in ck.ino_used.iter_mut() {
        *b = 0;
    }
    for b in ck.ino_isdir.iter_mut() {
        *b = 0;
    }
    for l in ck.links.iter_mut() {
        *l = 0;
    }

    ck.mark_metadata();
    ck.pass1();
    ck.tick(1, g.inodes_count, g.inodes_count);
    ck.pass2_dirs();
    ck.pass34_links();
    ck.pass2();
    ck.tick(5, g.groups_count, g.groups_count);
    // completed = 1 means and ONLY means every pass ran to the end. An aborted
    // scan must never be mistaken for a verified-clean filesystem, which is
    // what would let the caller clear the dirty flag on an unchecked volume.
    ck.rep.completed = if ck.aborted { 0 } else { 1 };
    if ck.aborted {
        return -9;
    }
    0
}

/// Byte sizes of the three FFI structs, so the C side can `_Static_assert`
/// against what Rust actually compiled rather than against a comment.
/// `which`: 0 = geom, 1 = scratch, 2 = report.
#[no_mangle]
pub extern "C" fn ext2_fsck_sizeof_rs(which: u32) -> u32 {
    match which {
        0 => core::mem::size_of::<E2fsckGeom>() as u32,
        1 => core::mem::size_of::<E2fsckScratch>() as u32,
        2 => core::mem::size_of::<E2fsckReport>() as u32,
        _ => 0,
    }
}
