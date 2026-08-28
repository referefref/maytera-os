// rustkern/blkstage.rs - SINGLE-OWNER CLAIM over the block layer's write
// staging (bounce) buffer.
//
// New kernel logic, no C twin to strangle, so Rust per the 2026-07-16 rule.
// Same shape as fbown.rs/fetchown.rs: the INPUTS come from unchanged C (the
// physically contiguous buffer fs/blockdev.c allocates once from the PMM) and
// the OWNERSHIP DECISION lives here.
//
// ===========================================================================
// THE DEFECT THIS REMOVES. MEASURED, on the owner's boot stick.
// ---------------------------------------------------------------------------
// Golden build 2215 (commit 960d22095cd9) on a 116 GB stick, serial
// 9062521368051756, two-partition GPT: p1 FAT ESP at LBA 2048..526335, p2 ext2
// root at LBA 526336. After ONE boot on an ASUS laptop the root partition
// would not mount: blkid reported no filesystem on p2 at all and dumpe2fs said
// "Bad magic number in super-block". e2fsck -b 32768 -B 4096 recovered the
// volume from the backup superblock and reported 4750/98800 files,
// 129640/395003 blocks with only group-descriptor and free-count repairs, so
// the filesystem was INTACT apart from a tiny write at its very start.
//
// What was actually on the disk, read back sector by sector:
//
//   abs LBA 10152  (ESP /boot/STAGE.TXT data sector)   md5 173bfc37...
//   abs LBA 526338 (p2 + 1024, ext2 primary superblock, first half)  173bfc37...
//   abs LBA 526339 (p2 + 1536, ext2 primary superblock, second half) 5b89bf1c...
//   abs LBA 10151  (ESP /boot/PANIC.TXT data sector)   512 zero bytes
//
// All three damaged sectors contain EXT2 INODE TABLE bytes: 256-byte inodes
// with plausible i_mode (0100755, 0100600), timestamps and ascending i_block[]
// pointers. They contain no breadcrumb text and no superblock fields.
//
// THE FIRST READING OF THIS EVIDENCE WAS WRONG AND IS WORTH RECORDING. The
// obvious inference from "the bytes at the superblock are byte-identical to
// /boot/STAGE.TXT" is that the breadcrumb writer computed a bad LBA and wrote
// its 512-byte record over the superblock: a partition-relative vs
// disk-absolute confusion. That inference is DISPROVEN by measurement. The FAT
// directory entry for /boot/STAGE.TXT names first cluster 8, and the ESP is
// FAT32 with 512-byte clusters, 32 reserved sectors and 2 FATs of 4033, so
// cluster 8 sits at partition-relative sector 8104 = absolute LBA 10152,
// comfortably inside the 524288-sector ESP. The breadcrumb writer's target was
// never wrong. The two locations are identical because BOTH RECEIVED THE SAME
// STOLEN PAYLOAD.
//
// The theft happened in fs/blockdev.c blk_write(). On a USB-rooted machine it
// staged every write through ONE process-wide bounce buffer:
//
//     memcpy(g_wbounce, caller_data, run * 512);      // no lock
//     usb_msc_write(dev, 0, lba, g_wbounce, run);     // PARKS on msc_cmd_lock
//
// usb_msc_write reaches msc_cmd_lock(), which since #617 is a real sleeping
// lock: __wait_prepare + __wait_event_wait_deadline. So a writer that finds
// the MSC command lock held is DESCHEDULED with its payload already sitting in
// the shared buffer. Any second writer that runs during that window memcpys
// its own payload over it, and when the first writer is woken it DMAs the
// second writer's bytes to the first writer's LBA.
//
// That reproduces every measured byte. ext2 updates its superblock at mount
// (the recovered /BOOTLOG.TXT shows "[EXT2] #610: mount state 0x1 mount-count
// 18 dirty-mark ok" for that boot), which is a 1024-byte, two-sector blk_write
// at absolute LBA 526338. A racing inode-table write left inode bytes in the
// buffer, so LBA 526338 got bytes 0..511 of the inode block and LBA 526339 got
// bytes 512..1023: two DIFFERENT inode sectors, exactly as measured. The
// one-sector /boot/STAGE.TXT write at LBA 10152 took bytes 0..511 of the same
// staged block, which is why it is byte-identical to the first damaged
// superblock sector. The kernel had already cached the superblock in RAM at
// mount, so the session continued normally on a volume whose on-disk
// superblock was already gone. The user only found out at the next boot.
//
// This is a DATA-DESTROYING race with a wide window (the whole duration of a
// contended USB command), on the single choke point every filesystem write in
// the product passes through.
//
// ===========================================================================
// WHY OWNERSHIP AND NOT "ADD A LOCK IN blk_write"
// ---------------------------------------------------------------------------
// A lock in blk_write fixes the instance. It leaves the MECHANISM in place: a
// file-scope static uint8_t *g_wbounce that any present or future line in
// blockdev.c can memcpy into, with correctness resting on every author
// remembering the rule. The same class already bit this tree through
// fs/fat.c's shared sector_buf.
//
// So the buffer POINTER now lives here and nowhere else. blockdev.c allocates
// the pages, hands the address to blkstage_install_rs() ONCE, and then keeps
// no variable holding it. The only way to obtain the address is
// blkstage_try_claim_rs(), which is a compare-exchange from 0 to the caller's
// token, and the only way to give it back is blkstage_release_rs() with the
// SAME token. Code that does not hold the claim cannot name the buffer.
//
// HONEST LIMIT, do not overstate this. "Cannot name the buffer" is a property
// of that translation unit, not of the machine: C can still fabricate the
// address by other means, and nothing here stops a wild pointer. What it
// removes is the accidental, well-intentioned second user, which is what this
// bug was.
//
// ===========================================================================
// THE SEAL: A GUARD THAT IS WATCHED, NOT ASSUMED
// ---------------------------------------------------------------------------
// blame.md's standing complaint is that a guard nobody has seen fire is
// indistinguishable from one that is switched off. The claim makes theft
// impossible, which also makes it invisible. So the staged bytes are SEALED
// (a full-coverage 64-bit fold, not a sample) immediately after they are
// staged and VERIFIED immediately after the device call returns. On a correct
// kernel the seal always matches and the counter stays 0; if it ever does not,
// blockdev.c logs loudly and the count is carried on the heartbeat line, so
// "measured, zero" stays distinguishable from "never measured".
//
// The seal is also what makes the fix PROVABLE. make BLKSTAGE_UNSAFE=1 removes
// the claim (restoring the pre-fix shared-buffer behaviour) while leaving the
// seal in place, so the same self-test can be watched going RED on the broken
// shape and GREEN on the fixed one.
//
// COST: two full passes over at most 32 KB per USB write, against a USB
// Bulk-Only command that costs three xHCI waits. A sampled fold was rejected
// deliberately: a probabilistic detector invites the argument about whether
// the sample was dense enough, which is exactly the argument this file exists
// to end.
// ===========================================================================

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

// The installed staging buffer. 0 = never installed (ATA-root machines never
// install one, and blk_write never asks for one there).
static BASE: AtomicU64 = AtomicU64::new(0);
static BYTES: AtomicU32 = AtomicU32::new(0);

// The live claim. 0 = free. Otherwise the owning caller's token.
static OWNER: AtomicU64 = AtomicU64::new(0);

// Token generator. Never returns 0, because 0 is the "free" sentinel and a
// caller holding token 0 could release a buffer it does not own.
static NEXT_TOKEN: AtomicU64 = AtomicU64::new(1);

// The seal over the currently staged bytes, the length it covers, and THE
// TOKEN THAT LAID IT DOWN.
//
// SEAL_TOKEN is the load-bearing part and it is why the detector works on the
// broken build as well as the fixed one. Theft has two shapes: the thief
// changes the bytes (caught by the fold) or the thief re-seals over us (caught
// by the token). A seal keyed only on the ownership claim would be blind on a
// BLKSTAGE_UNSAFE build, where by construction there IS no claim, which is
// exactly the build the RED proof needs it to work on.
static SEAL: AtomicU64 = AtomicU64::new(0);
static SEAL_LEN: AtomicU32 = AtomicU32::new(0);
static SEAL_TOKEN: AtomicU64 = AtomicU64::new(0);

// Audit counters.
static N_CLAIM: AtomicU64 = AtomicU64::new(0);
static N_RELEASE: AtomicU64 = AtomicU64::new(0);
static N_CONTEND: AtomicU64 = AtomicU64::new(0);
static N_BAD_RELEASE: AtomicU64 = AtomicU64::new(0);
static N_SEAL_BROKEN: AtomicU64 = AtomicU64::new(0);
static N_VERIFY: AtomicU64 = AtomicU64::new(0);

/// Mirrors blkstage_stats_t in fs/blockdev.h. sizeof-locked on both sides.
#[repr(C)]
pub struct BlkStageStats {
    pub base: u64,
    pub bytes: u32,
    pub owned: u32,
    pub claims: u64,
    pub releases: u64,
    pub contended: u64,
    pub bad_releases: u64,
    pub seal_broken: u64,
    pub verifies: u64,
}

const _: () = assert!(core::mem::size_of::<BlkStageStats>() == 64);

/// Install the one staging buffer. Called once, from fs/blockdev.c, with the
/// base of a physically contiguous, 64 KB-aligned PMM allocation.
///
/// Returns 1 if this call installed it, 0 if it was already installed with the
/// SAME base (idempotent), and -1 if a DIFFERENT base was offered. A second
/// distinct buffer is refused rather than accepted, because two staging
/// buffers is the very shape this module exists to make impossible.
///
/// Contract the caller must keep: base stays valid and mapped for the life of
/// the kernel and is never freed. blockdev.c allocates it from the PMM and
/// never frees it.
#[no_mangle]
pub extern "C" fn blkstage_install_rs(base: u64, bytes: u32) -> i32 {
    if base == 0 || bytes == 0 {
        return -1;
    }
    match BASE.compare_exchange(0, base, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => {
            BYTES.store(bytes, Ordering::SeqCst);
            1
        }
        Err(existing) => {
            if existing == base {
                0
            } else {
                -1
            }
        }
    }
}

/// A fresh, never-zero claim token for one blk_write() call.
#[no_mangle]
pub extern "C" fn blkstage_token_rs() -> u64 {
    let t = NEXT_TOKEN.fetch_add(1, Ordering::SeqCst);
    // fetch_add starts at 1 and wraps only after 2^64 writes; the guard costs
    // one predictable branch and removes the question entirely.
    if t == 0 {
        NEXT_TOKEN.fetch_add(1, Ordering::SeqCst)
    } else {
        t
    }
}

/// Is the buffer currently unowned? Used by the caller's wait-queue predicate.
/// Advisory only: the claim itself is the compare-exchange below.
#[no_mangle]
pub extern "C" fn blkstage_free_rs() -> i32 {
    if OWNER.load(Ordering::SeqCst) == 0 {
        1
    } else {
        0
    }
}

/// Take the claim. Returns the buffer address on success, 0 if the buffer is
/// not installed or is owned by somebody else.
///
/// THE WHOLE DIFFERENCE BETWEEN A CORRECT KERNEL AND A DELIBERATELY BROKEN ONE
/// LIVES IN THIS FUNCTION AND IN blkstage_release_rs(). `make
/// BLKSTAGE_UNSAFE=1` is a RUSTFLAG only: not one line of C changes, and
/// fs/blockdev.c compiles to the same object either way. That is deliberate.
/// If the knob also changed the C, a reader could always wonder whether the
/// RED result came from the missing claim or from some other difference
/// between the two builds. This way there is nothing else it could be.
#[no_mangle]
pub extern "C" fn blkstage_try_claim_rs(token: u64) -> u64 {
    if token == 0 {
        return 0;
    }
    let base = BASE.load(Ordering::SeqCst);
    if base == 0 {
        return 0;
    }
    #[cfg(blkstage_unsafe)]
    {
        // DELIBERATELY BROKEN. Hand the buffer to every caller with no
        // exclusion whatsoever, which is exactly what blk_write() did before
        // this ticket. NEVER SHIP A KERNEL BUILT THIS WAY.
        OWNER.store(token, Ordering::SeqCst);
        N_CLAIM.fetch_add(1, Ordering::SeqCst);
        base
    }
    #[cfg(not(blkstage_unsafe))]
    {
        match OWNER.compare_exchange(0, token, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => {
                N_CLAIM.fetch_add(1, Ordering::SeqCst);
                base
            }
            Err(_) => {
                N_CONTEND.fetch_add(1, Ordering::SeqCst);
                0
            }
        }
    }
}

/// Give the claim back. Returns 1 on success, 0 if the caller was not the
/// owner (counted, and loud on the C side: a mismatched release means some
/// path released a claim it never took, which would reopen the whole defect).
#[no_mangle]
pub extern "C" fn blkstage_release_rs(token: u64) -> i32 {
    if token == 0 {
        N_BAD_RELEASE.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    #[cfg(blkstage_unsafe)]
    {
        // Matching half of the broken claim above: there is no ownership to
        // check, so a release is unconditional. Without this the unsafe build
        // would drown in "release by a NON-OWNER" lines that are an artefact
        // of the knob rather than the fault being demonstrated.
        OWNER.store(0, Ordering::SeqCst);
        N_RELEASE.fetch_add(1, Ordering::SeqCst);
        return 1;
    }
    #[cfg(not(blkstage_unsafe))]
    match OWNER.compare_exchange(token, 0, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => {
            SEAL_LEN.store(0, Ordering::SeqCst);
            SEAL_TOKEN.store(0, Ordering::SeqCst);
            SEAL.store(0, Ordering::SeqCst);
            N_RELEASE.fetch_add(1, Ordering::SeqCst);
            1
        }
        Err(_) => {
            N_BAD_RELEASE.fetch_add(1, Ordering::SeqCst);
            0
        }
    }
}

// Full-coverage 64-bit fold over the staged bytes. Deliberately NOT a sample:
// see the header. Reads are volatile so the compiler cannot fuse the seal pass
// and the verify pass into one.
fn fold(base: u64, len: u32) -> u64 {
    let words = (len as usize) / 8;
    let p = base as *const u64;
    let mut acc: u64 = 0x9E37_79B9_7F4A_7C15u64 ^ (len as u64);
    let mut i = 0usize;
    while i < words {
        // SAFETY: the caller holds the claim, so base is the installed buffer,
        // and len has been clamped to BYTES by bounded_len() before we get
        // here, so every read is inside the allocation.
        let v = unsafe { core::ptr::read_volatile(p.add(i)) };
        acc = acc.rotate_left(7) ^ v.wrapping_mul(0xC2B2_AE3D_27D4_EB4Fu64);
        i += 1;
    }
    let b = base as *const u8;
    let mut k = words * 8;
    while k < len as usize {
        // SAFETY: as above; k < len <= BYTES.
        let v = unsafe { core::ptr::read_volatile(b.add(k)) };
        acc = acc.rotate_left(3) ^ (v as u64).wrapping_mul(0x1656_67B1_9E37_79F9u64);
        k += 1;
    }
    acc
}

fn bounded_len(len: u32) -> u32 {
    let cap = BYTES.load(Ordering::SeqCst);
    if len > cap {
        cap
    } else {
        len
    }
}

/// Seal the bytes just staged into the buffer. Called immediately after the
/// memcpy and before the device call. Returns 1 if sealed, 0 if there is no
/// buffer or the token is invalid.
///
/// Note what is deliberately NOT checked here: ownership. The seal must work
/// on a BLKSTAGE_UNSAFE build too, where no claim exists, or the RED half of
/// the proof would be blind.
#[no_mangle]
pub extern "C" fn blkstage_seal_rs(token: u64, len: u32) -> i32 {
    let base = BASE.load(Ordering::SeqCst);
    if base == 0 || token == 0 {
        return 0;
    }
    let n = bounded_len(len);
    // Order matters: publish the token LAST. A racing sealer that gets between
    // our fold and our token store leaves ITS token visible, so our verify
    // reports the mismatch rather than silently comparing against its fold.
    SEAL_LEN.store(n, Ordering::SeqCst);
    SEAL.store(fold(base, n), Ordering::SeqCst);
    SEAL_TOKEN.store(token, Ordering::SeqCst);
    1
}

/// Verify the seal after the device call returns.
///
/// Returns  1 = intact, the device was handed exactly the staged bytes;
///          0 = nothing was sealed (nothing to check);
///         -1 = THE STAGED PAYLOAD WAS TAKEN FROM US between staging and the
///              device write, either because another writer re-sealed the
///              buffer (token mismatch) or because the bytes changed under a
///              seal that is still ours (fold mismatch). On a kernel built
///              WITH the claim this is unreachable; if it is ever seen, some
///              path is using the staging buffer without holding the claim and
///              the defect this module removes has been reintroduced.
#[no_mangle]
pub extern "C" fn blkstage_verify_rs(token: u64) -> i32 {
    let base = BASE.load(Ordering::SeqCst);
    if base == 0 || token == 0 {
        return 0;
    }
    let n = SEAL_LEN.load(Ordering::SeqCst);
    if n == 0 {
        return 0;
    }
    N_VERIFY.fetch_add(1, Ordering::SeqCst);
    if SEAL_TOKEN.load(Ordering::SeqCst) != token {
        N_SEAL_BROKEN.fetch_add(1, Ordering::SeqCst);
        return -1;
    }
    if fold(base, n) != SEAL.load(Ordering::SeqCst) {
        N_SEAL_BROKEN.fetch_add(1, Ordering::SeqCst);
        return -1;
    }
    1
}

// Bit values for BlkStageStats.owned. Kept as an integer rather than two
// fields so the struct size stays locked at 64 on both sides of the FFI.
const OWNED_BIT_HELD: u32 = 0x1;
const OWNED_BIT_UNSAFE_BUILD: u32 = 0x2;

#[cfg(blkstage_unsafe)]
const BUILD_BITS: u32 = OWNED_BIT_UNSAFE_BUILD;
#[cfg(not(blkstage_unsafe))]
const BUILD_BITS: u32 = 0;

/// Snapshot for the serial report and the heartbeat line.
///
/// Contract the caller must keep: out points to a writable blkstage_stats_t.
#[no_mangle]
pub unsafe extern "C" fn blkstage_stats_rs(out: *mut BlkStageStats) {
    if out.is_null() {
        return;
    }
    let s = BlkStageStats {
        base: BASE.load(Ordering::SeqCst),
        bytes: BYTES.load(Ordering::SeqCst),
        owned: (if OWNER.load(Ordering::SeqCst) == 0 { 0 } else { OWNED_BIT_HELD })
            | BUILD_BITS,
        claims: N_CLAIM.load(Ordering::SeqCst),
        releases: N_RELEASE.load(Ordering::SeqCst),
        contended: N_CONTEND.load(Ordering::SeqCst),
        bad_releases: N_BAD_RELEASE.load(Ordering::SeqCst),
        seal_broken: N_SEAL_BROKEN.load(Ordering::SeqCst),
        verifies: N_VERIFY.load(Ordering::SeqCst),
    };
    // SAFETY: the caller guarantees out is a writable BlkStageStats.
    unsafe { core::ptr::write(out, s) };
}

/// The seal-broken count on its own, for the heartbeat line.
#[no_mangle]
pub extern "C" fn blkstage_seal_broken_rs() -> u64 {
    N_SEAL_BROKEN.load(Ordering::SeqCst)
}
