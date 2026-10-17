// vmascale.rs - #245: has some process's VMA list grown long enough that every
// mmap and every page fault in it is paying a linear list walk?
//
// WHY THIS EXISTS. The second half of #245 was a superlinear term above roughly
// 100 MB of userland heap that the allocator had been cleared of by measurement
// (b93cab6d, then 6255bf25). The mechanism turned out to be in this kernel:
// userland/libc/stdlib.c ensure_mapped() maps only the page-aligned gap it
// needs, at an explicit address adjacent to the previous mapping, so a heap
// growing to S bytes issued S/4096 separate SYS_MMAP calls; do_mmap() created a
// SEPARATE vma_t for every one of them because nothing in this kernel ever
// called vma_merge(); and vma_add_nolock(), vma_find_range_nolock() and
// vma_find() all walk that list from the head. Measured on the real code with
// kernel/mm/tests/run_vma_scale.sh: 1024 pages -> 1024 VMAs and 1534 node
// visits per page, 4096 pages -> 4096 VMAs and 6142 node visits per page. Four
// times the heap, sixteen times the work.
//
// do_mmap() now coalesces, so the heap case is one VMA and the walks are O(1).
// That makes the defect INVISIBLE, which is exactly how this tree has lost
// mechanisms before: a fix with no instrument is a fix nobody can see regress.
// Hence this module. It says, once per boot per power-of-two threshold, that
// some address space has reached a VMA count at which the walks start to cost
// real time. On a healthy system after the fix it NEVER prints.
//
// WHY RUST. Per the 2026-07-16 rule, new kernel code is Rust unless there is a
// stated performance or entanglement reason. The threshold decision and the
// counters are pure arithmetic over two integers, called once per mmap syscall,
// which is already microseconds of work, so there is no performance reason to
// keep it in C. The COUNTER SITES inside the list walks themselves stay in C:
// they are inside loops in mm/demand.c that run per node under the mm spinlock,
// which is the same hot/entangled exemption bklstat.rs documents for the BKL.
//
// NO FLOAT: the kernel target is soft-float with SSE disabled, so the ratio
// below is integer arithmetic, as in rqlock.rs and bklstat.rs.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

/// The VMA count at which the first report fires. 256 VMAs is already ~128
/// node visits per walk on average, three walks per mmap-plus-fault pair, i.e.
/// the regime where the quadratic term starts to be measurable. Below that a
/// report would be noise: a normal process has a handful of mappings.
pub const VMASCALE_FIRST: u32 = 256;

/// Highest power-of-two threshold already reported, so each one is said once.
static HIGH_REPORTED: AtomicU32 = AtomicU32::new(0);
/// Highest vma_count ever observed in any address space this boot.
static HIGH_SEEN: AtomicU32 = AtomicU32::new(0);
/// Total successful anonymous mmap inserts, and how many of them coalesced
/// into an existing neighbour instead of lengthening the list.
static MMAPS: AtomicU64 = AtomicU64::new(0);
static MERGED: AtomicU64 = AtomicU64::new(0);

/// Pure: which threshold, if any, this `vma_count` newly crosses given that
/// `already` has been reported. Returns 0 for "nothing to report".
///
/// Separated from the counter update so the self-test can exercise the decision
/// without mutating boot-global state. A self-test that can only run once, or
/// that leaves the instrument in a different state than it found it, is not one
/// anybody will run twice.
pub fn vmascale_threshold(vma_count: u32, already: u32) -> u32 {
    if vma_count < VMASCALE_FIRST {
        return 0;
    }
    // Largest power of two <= vma_count. vma_count >= 256 here, so the shift
    // count is at most 23 and cannot be the undefined 32.
    let thr = 1u32 << (31 - vma_count.leading_zeros());
    if thr > already {
        thr
    } else {
        0
    }
}

/// Called from do_mmap() with the mm lock held, AFTER the insert and the
/// coalesce attempt. `merged` is non-zero when the new range was absorbed by a
/// neighbour. Returns a threshold the caller should print, or 0.
///
/// The caller prints rather than this function, for the same reason bklstat.rs
/// returns numbers instead of formatting them: kprintf is a C variadic on a
/// serial port, and the caller is also the one holding the lock and so the one
/// that knows to defer the line until after it is dropped.
#[no_mangle]
pub extern "C" fn vmascale_note_mmap(vma_count: u32, merged: u32) -> u32 {
    MMAPS.fetch_add(1, Ordering::Relaxed);
    if merged != 0 {
        MERGED.fetch_add(1, Ordering::Relaxed);
    }
    HIGH_SEEN.fetch_max(vma_count, Ordering::Relaxed);

    let already = HIGH_REPORTED.load(Ordering::Relaxed);
    let thr = vmascale_threshold(vma_count, already);
    if thr == 0 {
        return 0;
    }
    // One winner per threshold even if two cores cross it at once. A loser
    // simply does not print, which is the correct outcome.
    match HIGH_REPORTED.compare_exchange(already, thr, Ordering::Relaxed, Ordering::Relaxed) {
        Ok(_) => thr,
        Err(_) => 0,
    }
}

/// Read the running totals for a status line. Any pointer may be null.
#[no_mangle]
pub extern "C" fn vmascale_totals_rs(mmaps: *mut u64, merged: *mut u64, high: *mut u32) {
    // SAFETY: each pointer is either null (checked) or a caller-owned object
    // valid for the duration of this call. Nothing is retained.
    unsafe {
        if !mmaps.is_null() {
            *mmaps = MMAPS.load(Ordering::Relaxed);
        }
        if !merged.is_null() {
            *merged = MERGED.load(Ordering::Relaxed);
        }
        if !high.is_null() {
            *high = HIGH_SEEN.load(Ordering::Relaxed);
        }
    }
}

/// Proves the decision function both FIRES and STAYS QUIET, because an
/// instrument that can only be silent is indistinguishable from one that is
/// broken. Returns 1 on pass, 0 on fail.
#[no_mangle]
pub extern "C" fn vmascale_selftest_rs() -> i32 {
    // Quiet below the floor, at any history.
    if vmascale_threshold(0, 0) != 0 || vmascale_threshold(255, 0) != 0 {
        return 0;
    }
    // Fires exactly at the floor, and reports the power of two, not the count.
    if vmascale_threshold(256, 0) != 256 || vmascale_threshold(300, 0) != 256 {
        return 0;
    }
    if vmascale_threshold(1024, 0) != 1024 || vmascale_threshold(24576, 0) != 16384 {
        return 0;
    }
    // Says each threshold once: the same count again after reporting is quiet.
    if vmascale_threshold(300, 256) != 0 || vmascale_threshold(1023, 512) != 0 {
        return 0;
    }
    // But a genuinely higher threshold still speaks.
    if vmascale_threshold(1024, 512) != 1024 {
        return 0;
    }
    1
}
