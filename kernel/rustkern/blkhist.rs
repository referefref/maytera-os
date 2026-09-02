// rustkern/blkhist.rs - TRANSFER-SIZE ACCOUNTING for the block layer.
//
// New kernel logic, no C twin to strangle, so Rust per the 2026-07-16 rule.
// fs/blockdev.c keeps the counters (they are incremented on the hottest path in
// the product and must stay a single add); this module owns the bucketing and
// every DERIVED quantity that gets printed.
//
// ===========================================================================
// WHY THIS EXISTS
// ---------------------------------------------------------------------------
// The existing [BLK122] census answers "how much of the block layer is the
// device" and answers it well: on the owner's laptop, 99%. What it could not
// answer is WHY, because it reported only totals:
//
//   device=3356804us in 27672 xfers
//
// 27672 transfers for 138028 sectors is 5 sectors per transfer ON AVERAGE, and
// an average cannot distinguish "everything is 5 sectors" from "half are 1 and
// half are 64". Those two have completely different fixes, and #69's rule
// applies exactly: a population that mixes two behaviours describes neither.
//
// The quantity that decides this ticket is ROUND TRIPS PER MEGABYTE, because a
// USB mass-storage command costs a FIXED ~121-148 us whatever its size
// (measured on real hardware; see rustkern/imgra.rs). So that is what gets
// printed, next to the histogram that shows whether the average is real.
//
// PRINT THE CONTRACT'S OWN QUANTITY, not the raw term (#122's lesson): "410
// round trips per MB" invites the right fix, while "27672 xfers" invites a
// hunt for what 27672 means.
#![allow(dead_code)]

// Buckets by SECTOR COUNT, in powers of four, because that is the resolution
// that separates the sizes this code can actually issue: 1 sector (512 B),
// 4 (an ISO logical block / a 2 KB fs block), 16 (an 8 KiB image cache block),
// 64 (BLK_USB_CHUNK, the largest single SCSI command this kernel emits).
pub const BLKHIST_N: usize = 6;

// Bucket edges as sector counts, for labelling. Index i covers
// [BLKHIST_LO[i], BLKHIST_LO[i+1]).
pub const BLKHIST_LO: [u32; BLKHIST_N] = [1, 2, 5, 17, 33, 65];

#[no_mangle]
pub extern "C" fn blkhist_bucket_rs(sectors: u32) -> u32 {
    // 1 | 2..4 | 5..16 | 17..32 | 33..64 | >64
    if sectors <= 1 { 0 }
    else if sectors <= 4 { 1 }
    else if sectors <= 16 { 2 }
    else if sectors <= 32 { 3 }
    else if sectors <= 64 { 4 }
    else { 5 }
}

// Round trips per megabyte, the quantity the fix is judged on.
//
// One megabyte is 2048 sectors. Returned scaled by 10 so the caller can print
// one decimal place without touching a float (the kernel is soft-float,
// SSE-disabled, so this is not a stylistic choice).
#[no_mangle]
pub extern "C" fn blkhist_xfers_per_mb_x10_rs(xfers: u64, sectors: u64) -> u64 {
    if sectors == 0 { return 0; }
    // xfers per 2048 sectors, times 10.
    xfers.saturating_mul(2048).saturating_mul(10) / sectors
}

// Mean sectors per transfer, scaled by 10.
#[no_mangle]
pub extern "C" fn blkhist_sectors_per_xfer_x10_rs(xfers: u64, sectors: u64) -> u64 {
    if xfers == 0 { return 0; }
    sectors.saturating_mul(10) / xfers
}

// Throughput in KB/s from bytes and microseconds. 1 byte per us is 1 MB/s, so
// bytes * 1000 / us is KB/s with no conversion constant to get wrong.
#[no_mangle]
pub extern "C" fn blkhist_kbps_rs(bytes: u64, us: u64) -> u64 {
    if us == 0 { return 0; }
    bytes.saturating_mul(1000) / us
}

// What this many transfers WOULD cost on a device whose fixed per-command cost
// is `cmd_us`, in milliseconds. The owner's stick measured 121 us; the the build host
// reference USB bridge measured 147 us. Printing the projection next to the
// count is what turns a round-trip count into a time somebody can act on.
#[no_mangle]
pub extern "C" fn blkhist_projected_ms_rs(xfers: u64, cmd_us: u64) -> u64 {
    xfers.saturating_mul(cmd_us) / 1000
}

#[no_mangle]
pub extern "C" fn blkhist_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks = 0u32;
    let mut fails = 0i32;
    macro_rules! chk { ($c:expr) => { { checks += 1; if !($c) { fails += 1; } } } }

    // Every bucket is reachable and the edges land where the labels say.
    chk!(blkhist_bucket_rs(0) == 0);
    chk!(blkhist_bucket_rs(1) == 0);
    chk!(blkhist_bucket_rs(2) == 1);
    chk!(blkhist_bucket_rs(4) == 1);
    chk!(blkhist_bucket_rs(5) == 2);
    chk!(blkhist_bucket_rs(16) == 2);
    chk!(blkhist_bucket_rs(17) == 3);
    chk!(blkhist_bucket_rs(32) == 3);
    chk!(blkhist_bucket_rs(33) == 4);
    chk!(blkhist_bucket_rs(64) == 4);
    chk!(blkhist_bucket_rs(65) == 5);
    chk!(blkhist_bucket_rs(u32::MAX) == 5);
    // Monotonic: a bigger transfer never lands in a lower bucket.
    let mut prev = 0u32;
    for n in 0..1000u32 {
        let b = blkhist_bucket_rs(n);
        chk!(b >= prev);
        chk!((b as usize) < BLKHIST_N);
        prev = b;
    }

    // The owner's own numbers, reproduced: 27672 transfers for 138028 sectors.
    // 138028 sectors is 67.4 MB, so 27672 / 67.4 = 410.6 round trips per MB.
    let x10 = blkhist_xfers_per_mb_x10_rs(27672, 138028);
    chk!(x10 >= 4100 && x10 <= 4110);
    chk!(blkhist_sectors_per_xfer_x10_rs(27672, 138028) == 49);   // 4.9 sectors

    // A perfect 32 KB-per-command stream is 32 round trips per MB.
    chk!(blkhist_xfers_per_mb_x10_rs(32, 2048) == 320);
    chk!(blkhist_sectors_per_xfer_x10_rs(32, 2048) == 640);

    // Zero denominators answer 0, never divide.
    chk!(blkhist_xfers_per_mb_x10_rs(5, 0) == 0);
    chk!(blkhist_sectors_per_xfer_x10_rs(0, 5) == 0);
    chk!(blkhist_kbps_rs(1000, 0) == 0);

    // 1 byte per microsecond IS 1 MB/s, so the identity holds with no constant.
    chk!(blkhist_kbps_rs(1_000_000, 1_000_000) == 1000);
    chk!(blkhist_kbps_rs(64 * 1024 * 1024, 1_000_000) == 67108);

    // The projection is the count times the fixed cost, and nothing else.
    chk!(blkhist_projected_ms_rs(27672, 121) == 3348);
    chk!(blkhist_projected_ms_rs(0, 121) == 0);

    // No input overflows into a wrong answer.
    chk!(blkhist_xfers_per_mb_x10_rs(u64::MAX, 1) > 0);
    chk!(blkhist_projected_ms_rs(u64::MAX, u64::MAX) > 0);

    if !out_checks.is_null() { unsafe { *out_checks = checks; } }
    fails
}
