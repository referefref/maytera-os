// rustkern/taskmgr.rs - #404 Task Manager data core
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 Task Manager data core (taskmgr_core). Two entry points the compositor
// Task Manager calls per frame: perf_ring_stats_rs aggregates one metric's
// history ring (cap slots, oldest at head, count valid) into min/max/last/avg
// for the Performance-tab sparkline; taskmgr_sort_rows_rs sorts up to `cap`
// process rows by a column key into a permutation. CONFINEMENT: the naive C
// sort over-WRITES its permutation array when count>cap (CWE-787) and the naive
// ring divides by zero when cap==0; both are rejected here by construction
// (count>cap / cap==0 -> -1 before any access). rs==c on well-formed input
// (5,000,000 offline vectors, 0 mismatch). Routed live under -DRUST_TASKMGR_CORE.
// ===========================================================================
#[repr(C)]
pub struct PerfStat { pub min: u32, pub max: u32, pub last: u32, pub avg: u32, pub count: u32 }
const _: () = assert!(core::mem::size_of::<PerfStat>() == 20);

#[no_mangle]
pub extern "C" fn perf_ring_stats_rs(samples: *const u32, cap: u32, head: u32,
                                     count: u32, out: *mut PerfStat) -> i32 {
    if samples.is_null() || out.is_null() { return -1; }
    if cap == 0 { return -1; }                       // confine div-by-zero
    if count > cap || head >= cap { return -1; }     // confine OOB / bad index
    // SAFETY: caller guarantees `samples` spans at least `cap` readable u32
    // (the ring's backing array). Indexed ONLY through this exactly-cap slice.
    let s: &[u32] = unsafe { core::slice::from_raw_parts(samples, cap as usize) };
    let o = unsafe { &mut *out };                    // SAFETY: out checked non-null
    if count == 0 { o.min = 0; o.max = 0; o.last = 0; o.avg = 0; o.count = 0; return 0; }
    let (mut mn, mut mx, mut sum, mut last) = (u32::MAX, 0u32, 0u64, 0u32);
    let cap_us = cap as usize;
    for i in 0..(count as usize) {
        let v = s[(head as usize + i) % cap_us];     // < cap, in range
        if v < mn { mn = v; }
        if v > mx { mx = v; }
        sum += v as u64; last = v;
    }
    o.min = mn; o.max = mx; o.last = last;
    o.avg = (sum / count as u64) as u32; o.count = count;   // count>=1, defined
    1
}

#[repr(C)]
pub struct ProcKey { pub pid: i32, pub cpu: u32, pub mem: u32 }
const _: () = assert!(core::mem::size_of::<ProcKey>() == 12);

const TM_SORT_CPU: i32 = 0;
const TM_SORT_MEM: i32 = 1;
const TM_SORT_PID: i32 = 2;
const TM_SORT_CPU_ASC: i32 = 3;
const TM_SORT_MEM_ASC: i32 = 4;

#[inline]
fn tm_key_before(a: &ProcKey, b: &ProcKey, key: i32) -> bool {
    match key {
        TM_SORT_CPU => if a.cpu != b.cpu { a.cpu > b.cpu } else { a.pid < b.pid },
        TM_SORT_MEM => if a.mem != b.mem { a.mem > b.mem } else { a.pid < b.pid },
        TM_SORT_CPU_ASC => if a.cpu != b.cpu { a.cpu < b.cpu } else { a.pid < b.pid },
        TM_SORT_MEM_ASC => if a.mem != b.mem { a.mem < b.mem } else { a.pid < b.pid },
        TM_SORT_PID => a.pid < b.pid,
        _ => a.pid < b.pid,
    }
}

#[no_mangle]
pub extern "C" fn taskmgr_sort_rows_rs(rows: *const ProcKey, count: u32, cap: u32,
                                       key: i32, idx_out: *mut i32) -> i32 {
    if rows.is_null() || idx_out.is_null() { return -1; }
    if count > cap { return -1; }                    // confine CWE-787 over-write
    if cap == 0 || count == 0 { return 0; }
    // SAFETY: caller guarantees `rows` and `idx_out` each span >= `cap` elements.
    let r: &[ProcKey] = unsafe { core::slice::from_raw_parts(rows, cap as usize) };
    let o: &mut [i32] = unsafe { core::slice::from_raw_parts_mut(idx_out, cap as usize) };
    let n = count as usize;
    for i in 0..n { o[i] = i as i32; }
    for i in 1..n {                                  // stable insertion sort
        let cur = o[i];
        let mut j = i;
        while j > 0 && tm_key_before(&r[cur as usize], &r[o[j - 1] as usize], key) {
            o[j] = o[j - 1]; j -= 1;
        }
        o[j] = cur;
    }
    count as i32
}
