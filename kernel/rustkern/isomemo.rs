// rustkern/isomemo.rs - SMALL MEMO over the ISO 9660 paths most recently
//                       resolved on a mounted disc image.
//
// New kernel logic, no C twin to strangle, so Rust per the 2026-07-16 rule.
// It owns the bounded path handling, which is the part of this that is easy to
// get wrong in C, and dos/diskimg.c keeps the I/O.
//
// ===========================================================================
// WHAT IT REMOVES
// ---------------------------------------------------------------------------
// dos/diskimg.c read_range_inner() called iso_resolve() on EVERY read. A DOS
// guest reads through INT 21h AH=3Fh, which dos/int21svc.c serves in chunks of
// sizeof(x->io_buf) = 4096 bytes, so streaming the 105 MB speech file off
// Discworld II disc 2 re-walked the ENTIRE directory path from the volume root
// about 26,000 times, once per 4 KB delivered.
//
// Each walk reads the directory extent one 2048-byte logical sector at a time
// through the same imgfile cache the data stream is using. While the cache
// happened to hold those sectors the cost was CPU only, which is why it never
// showed up in a device profile. It stops being CPU-only the moment readahead
// (rustkern/imgra.rs) starts using that cache the way a stream wants to: the
// directory blocks get evicted, and then every 4 KB of file costs a fresh
// directory walk ON THE DEVICE. So this is not an optimisation bolted onto the
// readahead change, it is the PRECONDITION for it, and shipping the readahead
// without it would have made the reported case slower, not faster.
//
// WHY FOUR ENTRIES AND NOT ONE
// ----------------------------
// One entry is enough for a single file read straight through, and useless the
// moment two files on the same disc are read alternately, which is exactly what
// a game loading assets does. Four entries cost 4 * 152 bytes per mounted image
// and survive the interleavings this actually sees. The same reasoning is why
// rustkern/imgra.rs tracks four streams rather than one.
//
// WHY IT CANNOT SERVE STALE BYTES
// -------------------------------
// The memo is per-mounted-image and is cleared whenever that image is mounted
// or released, so it cannot outlive the disc it describes. The eject/swap check
// that matters (the mount generation, #739) happens in diskimg_read_range_gen
// BEFORE read_range_inner is reached, so a memo hit is only ever consulted
// after the caller has already been proven to be talking about the disc that is
// still in the drive. An ISO 9660 volume is read-only by construction, so
// within one mount a path cannot change its extent.
#![allow(dead_code)]

pub const ISOMEMO_PATH_MAX: usize = 128;
pub const ISOMEMO_WAYS: usize = 4;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct IsoMemoEnt {
    pub path: [u8; ISOMEMO_PATH_MAX],
    pub lba: u32,
    pub ext: u32,
    pub isdir: u32,
    pub multi: u32,
    pub valid: u32,
    pub lru: u32,
}

#[repr(C)]
pub struct IsoMemo {
    pub e: [IsoMemoEnt; ISOMEMO_WAYS],
    pub clock: u32,
    pub _pad: u32,
    pub n_hit: u64,
    pub n_miss: u64,
}

// Bounded length of a NUL-terminated C string, refusing anything that does not
// terminate inside the buffer we are able to store.
fn cstr_len(p: *const u8, cap: usize) -> Option<usize> {
    if p.is_null() { return None; }
    for i in 0..cap {
        if unsafe { *p.add(i) } == 0 { return Some(i); }
    }
    None
}

// Compare including the terminator, so "/A" never matches "/AB" in either
// direction. `n` is the index of the caller's NUL.
fn same(ent: &IsoMemoEnt, path: *const u8, n: usize) -> bool {
    for i in 0..=n {
        if ent.path[i] != unsafe { *path.add(i) } { return false; }
    }
    true
}

#[no_mangle]
pub extern "C" fn isomemo_reset_rs(m: *mut IsoMemo) {
    if m.is_null() { return; }
    let s = unsafe { &mut *m };
    for i in 0..ISOMEMO_WAYS {
        s.e[i].valid = 0;
        s.e[i].path[0] = 0;
        s.e[i].lba = 0; s.e[i].ext = 0; s.e[i].isdir = 0; s.e[i].multi = 0;
        s.e[i].lru = 0;
    }
    s.clock = 0;
    s._pad = 0;
    s.n_hit = 0;
    s.n_miss = 0;
}

// 1 = hit (outputs written), 0 = miss (outputs untouched).
#[no_mangle]
pub extern "C" fn isomemo_lookup_rs(m: *mut IsoMemo, path: *const u8,
                                    out_lba: *mut u32, out_ext: *mut u32,
                                    out_isdir: *mut i32, out_multi: *mut i32) -> i32 {
    if m.is_null() { return 0; }
    let s = unsafe { &mut *m };
    let n = match cstr_len(path, ISOMEMO_PATH_MAX) {
        Some(n) => n,
        None => { s.n_miss = s.n_miss.wrapping_add(1); return 0; }
    };
    for i in 0..ISOMEMO_WAYS {
        if s.e[i].valid == 0 { continue; }
        if !same(&s.e[i], path, n) { continue; }
        if !out_lba.is_null()   { unsafe { *out_lba = s.e[i].lba; } }
        if !out_ext.is_null()   { unsafe { *out_ext = s.e[i].ext; } }
        if !out_isdir.is_null() { unsafe { *out_isdir = s.e[i].isdir as i32; } }
        if !out_multi.is_null() { unsafe { *out_multi = s.e[i].multi as i32; } }
        s.clock = s.clock.wrapping_add(1);
        if s.clock == 0 { s.clock = 1; }
        s.e[i].lru = s.clock;
        s.n_hit = s.n_hit.wrapping_add(1);
        return 1;
    }
    s.n_miss = s.n_miss.wrapping_add(1);
    0
}

// Store a freshly resolved extent. A path too long to hold is simply not
// memoised, which costs a walk and can never answer wrongly.
#[no_mangle]
pub extern "C" fn isomemo_store_rs(m: *mut IsoMemo, path: *const u8,
                                   lba: u32, ext: u32, isdir: i32, multi: i32) {
    if m.is_null() { return; }
    let s = unsafe { &mut *m };
    let n = match cstr_len(path, ISOMEMO_PATH_MAX) { Some(n) => n, None => return };

    // Overwrite the entry for this path if it already has one, else the least
    // recently used. An invalid entry is free and wins immediately.
    let mut victim = 0usize;
    let mut oldest = u32::MAX;
    let mut found = usize::MAX;
    for i in 0..ISOMEMO_WAYS {
        if s.e[i].valid != 0 && same(&s.e[i], path, n) { found = i; break; }
        if s.e[i].valid == 0 { victim = i; oldest = 0; continue; }
        if s.e[i].lru < oldest { oldest = s.e[i].lru; victim = i; }
    }
    let idx = if found != usize::MAX { found } else { victim };

    for i in 0..=n { s.e[idx].path[i] = unsafe { *path.add(i) }; }
    s.e[idx].lba = lba;
    s.e[idx].ext = ext;
    s.e[idx].isdir = if isdir != 0 { 1 } else { 0 };
    s.e[idx].multi = if multi != 0 { 1 } else { 0 };
    s.e[idx].valid = 1;
    s.clock = s.clock.wrapping_add(1);
    if s.clock == 0 { s.clock = 1; }
    s.e[idx].lru = s.clock;
}

#[no_mangle]
pub extern "C" fn isomemo_stats_rs(m: *const IsoMemo, hit: *mut u64, miss: *mut u64) {
    if m.is_null() { return; }
    let s = unsafe { &*m };
    if !hit.is_null()  { unsafe { *hit = s.n_hit; } }
    if !miss.is_null() { unsafe { *miss = s.n_miss; } }
}

#[no_mangle]
pub extern "C" fn isomemo_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks = 0u32;
    let mut fails = 0i32;
    macro_rules! chk { ($c:expr) => { { checks += 1; if !($c) { fails += 1; } } } }

    let blank = IsoMemoEnt { path: [0u8; ISOMEMO_PATH_MAX], lba: 0, ext: 0,
                             isdir: 0, multi: 0, valid: 0, lru: 0 };
    let mut m = IsoMemo { e: [blank; ISOMEMO_WAYS], clock: 0, _pad: 0,
                          n_hit: 0, n_miss: 0 };
    isomemo_reset_rs(&mut m);

    let a = b"/DW2/ENGLISH.SMP\0";
    let pre = b"/DW2/ENGLISH.SM\0";      // prefix: must NOT match
    let ext_ = b"/DW2/ENGLISH.SMPX\0";   // extension: must NOT match

    let (mut lba, mut ext) = (0u32, 0u32);
    let (mut isdir, mut multi) = (0i32, 0i32);

    // An empty memo answers nothing.
    chk!(isomemo_lookup_rs(&mut m, a.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 0);

    isomemo_store_rs(&mut m, a.as_ptr(), 1234, 105358318, 0, 0);
    chk!(isomemo_lookup_rs(&mut m, a.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 1);
    chk!(lba == 1234 && ext == 105358318 && isdir == 0 && multi == 0);

    // The two ways a naive prefix compare gets this wrong, both directions.
    chk!(isomemo_lookup_rs(&mut m, pre.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 0);
    chk!(isomemo_lookup_rs(&mut m, ext_.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 0);
    // A refused lookup must not have overwritten the outputs.
    chk!(lba == 1234 && ext == 105358318);

    // isdir/multi survive the round trip as flags, not as raw ints, and storing
    // the same path again UPDATES it rather than consuming a second way.
    isomemo_store_rs(&mut m, a.as_ptr(), 7, 8, 5, -3);
    chk!(isomemo_lookup_rs(&mut m, a.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 1);
    chk!(isdir == 1 && multi == 1 && lba == 7);
    let mut used = 0;
    for i in 0..ISOMEMO_WAYS { if m.e[i].valid != 0 { used += 1; } }
    chk!(used == 1);

    // THE INTERLEAVING PROPERTY: four paths read round-robin all stay resident,
    // which is what a one-entry memo cannot do and is why this has four ways.
    isomemo_reset_rs(&mut m);
    let paths: [&[u8]; 4] = [b"/A/ONE\0", b"/A/TWO\0", b"/B/THREE\0", b"/B/FOUR\0"];
    for (i, p) in paths.iter().enumerate() {
        isomemo_store_rs(&mut m, p.as_ptr(), 100 + i as u32, 9, 0, 0);
    }
    for _ in 0..10 {
        for (i, p) in paths.iter().enumerate() {
            chk!(isomemo_lookup_rs(&mut m, p.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 1);
            chk!(lba == 100 + i as u32);
        }
    }
    chk!(m.n_miss == 0);

    // A fifth path evicts the least recently used, never a hot one.
    let fifth = b"/C/FIVE\0";
    let _ = isomemo_lookup_rs(&mut m, paths[1].as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi);
    let _ = isomemo_lookup_rs(&mut m, paths[2].as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi);
    let _ = isomemo_lookup_rs(&mut m, paths[3].as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi);
    isomemo_store_rs(&mut m, fifth.as_ptr(), 500, 9, 0, 0);
    chk!(isomemo_lookup_rs(&mut m, fifth.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 1);
    chk!(isomemo_lookup_rs(&mut m, paths[0].as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 0);
    chk!(isomemo_lookup_rs(&mut m, paths[3].as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 1);

    // A path that does not terminate inside the buffer is refused, and refusing
    // it must not corrupt or evict anything that was already there.
    isomemo_reset_rs(&mut m);
    isomemo_store_rs(&mut m, a.as_ptr(), 3, 4, 0, 0);
    let long = [b'A'; ISOMEMO_PATH_MAX + 8];
    isomemo_store_rs(&mut m, long.as_ptr(), 1, 1, 0, 0);
    chk!(isomemo_lookup_rs(&mut m, long.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 0);
    chk!(isomemo_lookup_rs(&mut m, a.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 1);
    chk!(lba == 3 && ext == 4);

    // A path exactly filling the buffer (127 chars + NUL) is storable, and its
    // 126-char prefix is not confused with it.
    isomemo_reset_rs(&mut m);
    let mut maxp = [b'X'; ISOMEMO_PATH_MAX];
    maxp[ISOMEMO_PATH_MAX - 1] = 0;
    isomemo_store_rs(&mut m, maxp.as_ptr(), 77, 78, 0, 0);
    chk!(isomemo_lookup_rs(&mut m, maxp.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 1);
    chk!(lba == 77);
    let mut shortp = [b'X'; ISOMEMO_PATH_MAX];
    shortp[ISOMEMO_PATH_MAX - 2] = 0;
    chk!(isomemo_lookup_rs(&mut m, shortp.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 0);

    // A reset clears everything, counters included.
    isomemo_reset_rs(&mut m);
    chk!(m.n_hit == 0 && m.n_miss == 0);
    chk!(isomemo_lookup_rs(&mut m, a.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 0);

    // A null memo is inert, not a fault.
    chk!(isomemo_lookup_rs(core::ptr::null_mut(), a.as_ptr(), &mut lba, &mut ext, &mut isdir, &mut multi) == 0);
    isomemo_store_rs(core::ptr::null_mut(), a.as_ptr(), 1, 1, 0, 0);
    isomemo_reset_rs(core::ptr::null_mut());

    if !out_checks.is_null() { unsafe { *out_checks = checks; } }
    fails
}
