// rustkern/imgra.rs - SEQUENTIAL READAHEAD POLICY for the disk-image block
//                     cache (dos/imgfile.c), and the victim-run choice that
//                     makes a readahead fetch ONE device command instead of N.
//
// New kernel logic, no C twin to strangle, so Rust per the 2026-07-16 rule.
// The I/O stays in dos/imgfile.c (it is glue against blk_read/ext2/FAT); every
// DECISION about how much to fetch and where to put it is here.
//
// ===========================================================================
// THE DEFECT THIS REMOVES. MEASURED, on the owner's laptop, from his boot log.
// ---------------------------------------------------------------------------
// His CD images are raw ISO 9660 volumes in the unpartitioned tail of the boot
// stick (dos/usbvol.c), so a Discworld II level load streams from a 442 MB ISO
// on a USB 2.0 flash drive:
//
//   [BLK122] mode=2 calls=33413 sectors=138028 | t_call=3373934us
//            device=3356804us in 27672 xfers -> DEVICE IS 99% OF THE BLOCK LAYER
//
// 27672 transfers for 138028 sectors is 2.5 KB per USB round trip, and 121 us
// per transfer. The block layer is not slow; the ROUND TRIP COUNT is the whole
// cost. The shape of that cost was measured directly on a real USB mass-storage
// device (the build host, Imation M100 USB 3 bridge, O_DIRECT, queue depth 1, read-only):
//
//   4 KB reads   27.7 MB/s   ->  148 us per command
//   8 KB reads   55.6 MB/s   ->  147 us per command
//   32 KB reads  88.6 MB/s   ->  370 us per command (device now bandwidth-bound)
//   64 KB..1 MB  89.3 MB/s   ->  saturated, no further gain
//
// Doubling the transfer from 4 KB to 8 KB doubled throughput and did NOT change
// the per-command cost, which is the signature of a fixed round-trip cost that
// dwarfs the data. The curve is flat from 32 KB up: 32 KB per command is where
// the device stops caring, so that is the size to aim at and there is no reason
// to go past it.
//
// dos/imgfile.c fetched exactly ONE 8 KiB cache block per miss, with no
// readahead of any kind, so a purely sequential stream paid one full round trip
// per 8 KiB = 128 commands per megabyte. This module makes a detected
// sequential stream fetch several consecutive blocks in ONE backing read.
//
// WHY A WINDOW AND NOT JUST A BIGGER BLOCK
// ----------------------------------------
// Enlarging IMGF_CACHE_BLK to 64 KiB would give the same transfer size and
// would be less code. It would also cut the number of distinct cached blocks
// from 32 to 4, and dos/diskimg.c walks the ISO directory tree through the SAME
// cache. Four blocks cannot hold a directory walk and a data stream at once, so
// the two would evict each other on every call and the miss rate would go UP.
// Keeping 8 KiB granularity for random and directory access while fetching a
// RUN of consecutive blocks for a sequential stream gets the large transfer
// without shrinking the cache's associativity.
//
// WHY IT RAMPS INSTEAD OF STARTING WIDE
// -------------------------------------
// A readahead that is wrong is not free: it costs device time and it evicts
// blocks that were going to be used. A window starts at 1 and doubles only when
// a caller comes back for exactly the block the previous fetch predicted, so a
// random-access caller never pays for readahead at all, and a sequential one
// reaches the cap after three misses.
//
// WHY SEVERAL STREAMS AND NOT ONE
// -------------------------------
// A drive letter is ONE imgfile handle, so everything a guest reads off that
// disc shares this state: the file being streamed, the ISO directory walk that
// resolves it, and any second file open at the same time. With a single
// last-position variable, ANY interleaving resets the window to 1 on every
// other miss and readahead never engages at all. That is not a corner case, it
// is the normal shape of a game loading assets. The tracker therefore keeps a
// small set of independent stream positions, so two interleaved sequential
// readers are both recognised, and a directory walk landing between two data
// reads no longer cancels the data reader's window.
#![allow(dead_code)]

// Independent sequential streams tracked at once. Four covers a data stream, a
// second data stream, a directory walk and one straggler. It is a fixed array
// scanned linearly, so growing it is not free; four is the point where the
// interleavings this actually sees are covered.
pub const IMGRA_STREAMS: usize = 4;

// A block index that cannot be a real one, so a fresh slot is never mistaken
// for a sequential continuation.
const NO_SEQ: u64 = u64::MAX;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ImgRaStream {
    pub next_seq: u64,   // block index this stream should miss on next
    pub win: u32,        // current window in cache blocks (1 = no readahead)
    pub lru: u32,        // recency stamp; lowest is evicted first
}

// Per-open-image readahead state. Shared with dos/imgfile.h by value, so the
// layout is locked by a _Static_assert on the C side.
#[repr(C)]
pub struct ImgRa {
    pub st: [ImgRaStream; IMGRA_STREAMS],
    pub clock: u32,
    pub _pad: u32,
    pub n_seq: u64,      // misses matched to a live stream
    pub n_rand: u64,     // misses that started a new stream
    pub n_fetch: u64,    // backing reads issued
    pub n_blocks: u64,   // cache blocks filled by those backing reads
}

#[no_mangle]
pub extern "C" fn imgra_reset_rs(ra: *mut ImgRa) {
    if ra.is_null() { return; }
    let r = unsafe { &mut *ra };
    for i in 0..IMGRA_STREAMS {
        r.st[i].next_seq = NO_SEQ;
        r.st[i].win = 1;
        r.st[i].lru = 0;
    }
    r.clock = 0;
    r._pad = 0;
    r.n_seq = 0;
    r.n_rand = 0;
    r.n_fetch = 0;
    r.n_blocks = 0;
}

// How many CONSECUTIVE cache blocks starting at `blk` should this miss fetch?
//
// `max_win` is the caller's cap in blocks (dos/imgfile.h IMGF_RA_MAX, itself
// clamped against the slot count). `avail` is how many blocks remain before
// end-of-image, so a plan never asks for bytes that do not exist.
//
// Returns at least 1 and never more than min(max_win, avail).
#[no_mangle]
pub extern "C" fn imgra_plan_rs(ra: *mut ImgRa, blk: u64, max_win: u32, avail: u64) -> u32 {
    if ra.is_null() { return 1; }
    let r = unsafe { &mut *ra };
    r.clock = r.clock.wrapping_add(1);
    if r.clock == 0 { r.clock = 1; }

    // A stream that predicted exactly this block is the one continuing.
    let mut idx = usize::MAX;
    for i in 0..IMGRA_STREAMS {
        if r.st[i].next_seq != NO_SEQ && r.st[i].next_seq == blk { idx = i; break; }
    }

    if idx != usize::MAX {
        r.n_seq = r.n_seq.wrapping_add(1);
        let doubled = r.st[idx].win.saturating_mul(2);
        r.st[idx].win = if doubled > max_win { max_win } else { doubled };
    } else {
        // No stream expected this block: start one, replacing the least
        // recently used slot. A brand-new stream gets no readahead at all until
        // it proves itself, which is what keeps random access free.
        r.n_rand = r.n_rand.wrapping_add(1);
        let mut victim = 0usize;
        let mut oldest = u32::MAX;
        for i in 0..IMGRA_STREAMS {
            if r.st[i].next_seq == NO_SEQ { victim = i; oldest = 0; break; }
            if r.st[i].lru < oldest { oldest = r.st[i].lru; victim = i; }
        }
        idx = victim;
        r.st[idx].win = 1;
    }

    if r.st[idx].win == 0 { r.st[idx].win = 1; }
    if r.st[idx].win > max_win && max_win >= 1 { r.st[idx].win = max_win; }

    let mut n = r.st[idx].win as u64;
    if avail == 0 { n = 1; } else if n > avail { n = avail; }

    // The NEXT miss on this stream lands one past everything this fetch installs.
    r.st[idx].next_seq = blk.wrapping_add(n);
    r.st[idx].lru = r.clock;
    r.n_fetch = r.n_fetch.wrapping_add(1);
    r.n_blocks = r.n_blocks.wrapping_add(n);
    n as u32
}

// A fetch that failed installed nothing, so the prediction it made must be
// withdrawn or the next miss is judged sequential against blocks that are not
// there. The most recently touched stream is the one that made it.
#[no_mangle]
pub extern "C" fn imgra_abort_rs(ra: *mut ImgRa) {
    if ra.is_null() { return; }
    let r = unsafe { &mut *ra };
    let mut idx = 0usize;
    let mut newest = 0u32;
    for i in 0..IMGRA_STREAMS {
        if r.st[i].lru >= newest { newest = r.st[i].lru; idx = i; }
    }
    r.st[idx].next_seq = NO_SEQ;
    r.st[idx].win = 1;
    r.st[idx].lru = 0;
}

// Choose the start slot of a run of `run` CONSECUTIVE cache slots to evict.
//
// Consecutive matters: the slots are one contiguous allocation, so a run of
// them is one contiguous buffer and the whole readahead is ONE blk_read into
// it. A scattered set of LRU victims would need one device command each, which
// is the cost this module exists to remove.
//
// `ages` is the cache's LRU stamp array (0 = free slot, higher = more recently
// used). The run with the lowest MAXIMUM age is the one whose most-recently-used
// member is oldest, which is the least valuable run to lose. Ties go to the
// lowest index, deterministically, so the choice is reproducible in a log.
//
// `align` restricts the candidate starts to multiples of itself, and it exists
// for a MEASURED reason rather than tidiness. fs/blockdev.c must clip every
// device command at the next 64 KB boundary of the destination, because an xHCI
// Transfer TRB may not cross one. A 64 KiB fetch that begins on a 64 KiB
// boundary is therefore exactly two 32 KB commands; the same fetch beginning
// mid-boundary is three ragged ones, a third more round trips for identical
// bytes. Passing align == run (with the cache base itself 64 KiB-aligned) makes
// every readahead span aligned by construction. align == 1 imposes nothing.
//
// Returns a start index in 0..=(nslots - run), or 0 on any malformed input.
#[no_mangle]
pub extern "C" fn imgra_victim_rs(ages: *const u32, nslots: u32, run: u32, align: u32) -> u32 {
    if ages.is_null() || nslots == 0 || run == 0 || run > nslots { return 0; }
    let step = if align == 0 { 1usize } else { align as usize };
    let a = unsafe { core::slice::from_raw_parts(ages, nslots as usize) };
    let last_start = (nslots - run) as usize;
    let mut best_start = 0usize;
    let mut best_max = u32::MAX;
    let mut s = 0usize;
    while s <= last_start {
        let mut m = 0u32;
        for k in 0..(run as usize) {
            let v = a[s + k];
            if v > m { m = v; }
        }
        if m < best_max {
            best_max = m;
            best_start = s;
            if m == 0 { break; }   // an all-free run cannot be beaten
        }
        s += step;
    }
    best_start as u32
}

#[no_mangle]
pub extern "C" fn imgra_stats_rs(ra: *const ImgRa, seq: *mut u64, rand: *mut u64,
                                 fetch: *mut u64, blocks: *mut u64) {
    if ra.is_null() { return; }
    let r = unsafe { &*ra };
    if !seq.is_null()    { unsafe { *seq = r.n_seq; } }
    if !rand.is_null()   { unsafe { *rand = r.n_rand; } }
    if !fetch.is_null()  { unsafe { *fetch = r.n_fetch; } }
    if !blocks.is_null() { unsafe { *blocks = r.n_blocks; } }
}

// ---------------------------------------------------------------------------
// SELF-TEST. It asserts the PROPERTIES the C side depends on, not a table of
// expected values, so it cannot be wrong about what the answer should be.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn imgra_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks = 0u32;
    let mut fails = 0i32;

    let zero = ImgRaStream { next_seq: 0, win: 0, lru: 0 };
    let mut ra = ImgRa { st: [zero; IMGRA_STREAMS], clock: 0, _pad: 0,
                         n_seq: 0, n_rand: 0, n_fetch: 0, n_blocks: 0 };

    macro_rules! chk { ($c:expr) => { { checks += 1; if !($c) { fails += 1; } } } }

    imgra_reset_rs(&mut ra);
    // A cold first miss fetches exactly one block: no readahead is guessed
    // before any evidence of streaming exists.
    chk!(imgra_plan_rs(&mut ra, 100, 8, 1_000_000) == 1);
    // Then a perfectly sequential caller ramps 1 -> 2 -> 4 -> 8 and CAPS.
    chk!(imgra_plan_rs(&mut ra, 101, 8, 1_000_000) == 2);
    chk!(imgra_plan_rs(&mut ra, 103, 8, 1_000_000) == 4);
    chk!(imgra_plan_rs(&mut ra, 107, 8, 1_000_000) == 8);
    chk!(imgra_plan_rs(&mut ra, 115, 8, 1_000_000) == 8);

    // THE INTERLEAVING PROPERTY, and the reason this is not one variable.
    // Two sequential streams read alternately: BOTH must keep ramping. With a
    // single last-position this sequence returns 1 forever.
    imgra_reset_rs(&mut ra);
    let (mut a, mut b) = (0u64, 500_000u64);
    let mut last_a = 0u32;
    let mut last_b = 0u32;
    for _ in 0..6 {
        last_a = imgra_plan_rs(&mut ra, a, 8, 1_000_000); a += last_a as u64;
        last_b = imgra_plan_rs(&mut ra, b, 8, 1_000_000); b += last_b as u64;
    }
    chk!(last_a == 8);
    chk!(last_b == 8);

    // A directory walk landing between two data reads must not cancel the data
    // stream's window: three interleaved readers, all still recognised.
    imgra_reset_rs(&mut ra);
    let mut d = 0u64;
    let mut n = 0u32;
    for _ in 0..6 {
        n = imgra_plan_rs(&mut ra, d, 8, 1_000_000); d += n as u64;
        let _ = imgra_plan_rs(&mut ra, 900_000 + (d % 3), 8, 1_000_000);  // random pokes
    }
    chk!(n == 8);

    // A caller that is genuinely random never gets readahead, however long it
    // runs: every plan is 1.
    imgra_reset_rs(&mut ra);
    let mut seed = 12345u64;
    for _ in 0..200 {
        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        let blk = (seed >> 33) | 1_000_000;    // never adjacent to a prediction
        chk!(imgra_plan_rs(&mut ra, blk, 8, 1_000_000) == 1);
    }

    // End of image clamps the plan, so a fetch can never be planned past the
    // last block even at full window.
    imgra_reset_rs(&mut ra);
    chk!(imgra_plan_rs(&mut ra, 10, 8, 1) == 1);
    chk!(imgra_plan_rs(&mut ra, 11, 8, 3) == 2);
    imgra_reset_rs(&mut ra);
    let _ = imgra_plan_rs(&mut ra, 0, 8, 1_000_000);
    let _ = imgra_plan_rs(&mut ra, 1, 8, 1_000_000);
    let _ = imgra_plan_rs(&mut ra, 3, 8, 1_000_000);
    chk!(imgra_plan_rs(&mut ra, 7, 8, 2) == 2);   // window is 8, only 2 remain

    // A plan is never zero and never exceeds the cap, for any input.
    imgra_reset_rs(&mut ra);
    for b in 0..64u64 {
        let n = imgra_plan_rs(&mut ra, b, 4, 1_000_000);
        chk!(n >= 1 && n <= 4);
    }

    // An aborted fetch withdraws its prediction, so the next miss is random.
    imgra_reset_rs(&mut ra);
    let _ = imgra_plan_rs(&mut ra, 50, 8, 1_000_000);
    imgra_abort_rs(&mut ra);
    let before = ra.n_rand;
    chk!(imgra_plan_rs(&mut ra, 51, 8, 1_000_000) == 1);
    chk!(ra.n_rand == before + 1);

    // Victim choice: an all-free run wins outright.
    let ages = [9u32, 9, 9, 0, 0, 0, 0, 9];
    chk!(imgra_victim_rs(ages.as_ptr(), 8, 3, 1) == 3);
    // With no free run, the run whose most-recently-used member is oldest wins.
    let ages2 = [5u32, 6, 1, 2, 9, 9, 9, 9];
    chk!(imgra_victim_rs(ages2.as_ptr(), 8, 2, 1) == 2);
    // The result is always a legal start index.
    for run in 1..=8u32 {
        let s = imgra_victim_rs(ages2.as_ptr(), 8, run, 1);
        chk!(s + run <= 8);
    }
    // Malformed input is refused rather than trusted.
    chk!(imgra_victim_rs(core::ptr::null(), 8, 2, 1) == 0);
    chk!(imgra_victim_rs(ages2.as_ptr(), 8, 99, 1) == 0);
    chk!(imgra_victim_rs(ages2.as_ptr(), 0, 1, 1) == 0);
    // An aligned search only ever returns an aligned start, and still returns a
    // legal one. This is the property fs/blockdev.c's 64 KB TRB clip depends on.
    let ages3 = [1u32, 2, 3, 4, 9, 9, 9, 9, 0, 0, 0, 0, 7, 7, 7, 7,
                 8, 8, 8, 8, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4];
    for run in [1u32, 2, 4, 8] {
        let st = imgra_victim_rs(ages3.as_ptr(), 32, run, run);
        chk!(st % run == 0 && st + run <= 32);
    }
    // With one all-free aligned run present, that is the one chosen.
    chk!(imgra_victim_rs(ages3.as_ptr(), 32, 4, 4) == 8);

    // A null handle is inert, not a fault, and still answers safely.
    chk!(imgra_plan_rs(core::ptr::null_mut(), 1, 8, 8) == 1);
    imgra_abort_rs(core::ptr::null_mut());
    imgra_reset_rs(core::ptr::null_mut());

    if !out_checks.is_null() { unsafe { *out_checks = checks; } }
    fails
}
