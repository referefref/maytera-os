// scprof.rs - #121: WHAT A SLOW SYSCALL IS ACTUALLY DOING.
//
// WHY THIS EXISTS
// ---------------
// #118 established that proc/syscall.asm:89 takes the Big Kernel Lock and holds
// it for the ENTIRE duration of the syscall. Its instrument (rustkern/bklsite.rs)
// keys on the return address of the depth-0 acquire, and EVERY syscall in the
// kernel shares that one address, so it can name the doorway and never the
// room: "syscall_entry held the lock" is true of every syscall ever made.
//
// This keys on the SYSCALL NUMBER, and on named PHASES inside the read and
// spawn paths, and records for the worst call of each number how long it took,
// how many interrupt entries happened during it, and how many times the BKL
// hold was BROKEN by a context switch.
//
// THE DISTINCTION THAT MATTERS, AND WHICH IS EASY TO GET WRONG
// ------------------------------------------------------------
// A BKL HOLD and a SYSCALL are not the same interval. proc/process.c drops the
// lock across every context switch (bkl_release_all/bkl_reacquire), so a
// syscall that BLOCKS ends its syscall_entry hold at the first switch and the
// remainder is charged to sched_schedule's reacquire instead. A long syscall is
// therefore NOT automatically a long hold. `brk` is what tells them apart, and
// it is MEASURED rather than inferred because getting this backwards is how a
// 710 ms SYS_READ reads as a 710 ms critical section when it is really 71
// segments with 70 context switches in between.
//
// WHY THE PHASE SPLIT IS KEPT ONLY FOR UNBROKEN CALLS
// ---------------------------------------------------
// MEASURED, and it invalidated the first version of this file: the phase
// accumulators are PER-CPU, so if the profiled syscall context-switches, some
// OTHER thread runs on that core and its block I/O lands in our delta. The
// first run reported a SYS_READ whose phases summed to 1764 ms inside a 710 ms
// call - impossible on its face, which is the only reason it was caught. So a
// phase profile is now recorded ONLY for calls with brk == 0, where no other
// thread ran on this core and the delta is provably this call's own. A profile
// that cannot be trusted is not printed at all, rather than printed with a
// caveat nobody will read.
//
// (Residual, stated rather than hidden: an interrupt handler that itself calls
// blk_read during an unbroken call would still be counted. irqs is printed
// next to the profile so that possibility is visible.)
//
// STATE OWNERSHIP
// ---------------
// The table lives in C (cpu/scprof.c) because it is written from the syscall
// dispatch path; the DECISIONS - which slot, how a sample merges, which entries
// rank highest, and whether the self-test passed - are here in Rust per the
// all-new-kernel-code-in-Rust rule, the same split rustkern/bklsite.rs uses.
//
// Integer only: the kernel target is soft-float with SSE disabled.

/// Phase slots. Must equal SCP_PHASE_N in cpu/scprof.h; the C side locks the
/// struct size with a _Static_assert so the two cannot drift.
pub const SCP_PHASE_N: usize = 12;

/// Highest syscall number the table holds. DIRECT-MAPPED on the syscall number
/// rather than a small associative set like bklsite's: there is no eviction
/// policy to get wrong and no "table full" state that would make a busy entry
/// indistinguishable from an absent one. Anything at or above this is counted
/// as out-of-range rather than folded into a neighbour.
pub const SCPROF_MAX: u32 = 512;

/// One syscall number's accounting. repr(C), sizeof-locked on the C side.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ScStat {
    /// Completed calls attributed to this number.
    pub count: u64,
    /// Sum of their durations, microseconds of real time (cpu/mono.h).
    pub total_us: u64,
    /// Longest single call of ANY kind, microseconds.
    pub max_us: u64,
    /// Interrupt entries during that longest call.
    pub worst_irqs: u64,
    /// bkl_release_all() calls during it, i.e. context switches. Non-zero means
    /// the syscall_entry BKL hold was broken that many times and the call is
    /// NOT one critical section.
    pub worst_brk: u64,

    /// Calls that completed with brk == 0. For these, and only these, the
    /// syscall duration IS the syscall_entry BKL hold duration.
    pub unbrk_count: u64,
    /// Sum of those durations.
    pub unbrk_total_us: u64,
    /// Longest unbroken call: the longest BKL hold this syscall number caused.
    pub unbrk_max_us: u64,
    /// Interrupt entries during that longest unbroken call. ZERO is the
    /// interesting value and it is AMBIGUOUS on its own between "interrupts
    /// were masked" and "the vCPU was not running": mono_us() is TSC-backed and
    /// keeps counting through a hypervisor deschedule. Corroborate with the
    /// host's schedstat run_delay before concluding either (#118 pitfall).
    pub unbrk_max_irqs: u64,
    /// Per-phase microseconds during that longest unbroken call.
    pub unbrk_max_ph: [u64; SCP_PHASE_N],
    /// Per-phase microseconds summed over ALL unbroken calls, so the aggregate
    /// question ("where does this syscall's kernel time go overall") has an
    /// answer that is not one outlier.
    pub unbrk_sum_ph: [u64; SCP_PHASE_N],
}

/// Sample recorded.
pub const SCPROF_OK: i32 = 0;
/// Syscall number at or above SCPROF_MAX: NOT recorded anywhere.
pub const SCPROF_OOR: i32 = 1;
/// Caller passed a null table or a bad length.
pub const SCPROF_BADARG: i32 = -1;

/// Rank by total_us descending.
pub const SCPROF_BY_TOTAL: i32 = 0;
/// Rank by max_us descending.
pub const SCPROF_BY_MAX: i32 = 1;
/// Rank by unbrk_max_us descending: the longest UNBROKEN hold, which is the
/// ranking this ticket actually needs. All three are published because they
/// disagree, and #118 proved that mattered: the site with the highest mean hold
/// there would never have appeared in a maximum ranking.
pub const SCPROF_BY_UNBRK: i32 = 2;

/// Record one completed syscall.
///
/// # Safety
/// `tab` must point to at least `n` writable, initialised `ScStat` entries and
/// `ph` to at least `nph` readable u64s.
#[no_mangle]
pub unsafe extern "C" fn scprof_add(
    tab: *mut ScStat,
    n: u32,
    num: u64,
    us: u64,
    irqs: u64,
    brk: u64,
    ph: *const u64,
    nph: u32,
) -> i32 {
    if tab.is_null() || n == 0 || n > SCPROF_MAX {
        return SCPROF_BADARG;
    }
    if num >= n as u64 {
        return SCPROF_OOR;
    }
    let t = core::slice::from_raw_parts_mut(tab, n as usize);
    let e = &mut t[num as usize];
    e.count = e.count.wrapping_add(1);
    e.total_us = e.total_us.wrapping_add(us);
    if us > e.max_us {
        e.max_us = us;
        e.worst_irqs = irqs;
        e.worst_brk = brk;
    }
    if brk != 0 {
        return SCPROF_OK;   // phases would be another thread's; see the header
    }
    let take = if nph as usize > SCP_PHASE_N { SCP_PHASE_N } else { nph as usize };
    e.unbrk_count = e.unbrk_count.wrapping_add(1);
    e.unbrk_total_us = e.unbrk_total_us.wrapping_add(us);
    if !ph.is_null() {
        for i in 0..take {
            e.unbrk_sum_ph[i] = e.unbrk_sum_ph[i].wrapping_add(*ph.add(i));
        }
    }
    if us > e.unbrk_max_us {
        // The profile is evidence about ONE call, so it is replaced wholesale
        // with the new worst call's, never accumulated: mixing a new maximum
        // with an older call's breakdown produces a profile of a call that
        // never happened.
        e.unbrk_max_us = us;
        e.unbrk_max_irqs = irqs;
        for i in 0..SCP_PHASE_N {
            e.unbrk_max_ph[i] = if i < take && !ph.is_null() { *ph.add(i) } else { 0 };
        }
    }
    SCPROF_OK
}

/// Write the indices of the top `out_n` used syscall numbers into `out_idx`,
/// ranked as requested. Returns how many were written.
///
/// Selection sort over the whole table, picking only as many as asked for; this
/// runs from the periodic serial report, not a hot path. The "already taken"
/// set is a bitmap rather than a bool array so the stack frame stays 64 bytes
/// even at SCPROF_MAX = 512.
///
/// # Safety
/// `tab` must point to `n` readable entries and `out_idx` to `out_n` writable
/// u32s.
#[no_mangle]
pub unsafe extern "C" fn scprof_top(
    tab: *const ScStat,
    n: u32,
    by: i32,
    out_idx: *mut u32,
    out_n: u32,
) -> u32 {
    if tab.is_null() || out_idx.is_null() || n == 0 || n > SCPROF_MAX || out_n == 0 {
        return 0;
    }
    let t = core::slice::from_raw_parts(tab, n as usize);
    let out = core::slice::from_raw_parts_mut(out_idx, out_n as usize);
    let mut taken = [0u64; (SCPROF_MAX as usize) / 64];
    let mut written = 0usize;
    while written < out.len() {
        let mut best: Option<usize> = None;
        let mut best_key = 0u64;
        for i in 0..t.len() {
            if t[i].count == 0 || (taken[i / 64] >> (i % 64)) & 1 == 1 {
                continue;
            }
            let key = match by {
                SCPROF_BY_TOTAL => t[i].total_us,
                SCPROF_BY_UNBRK => t[i].unbrk_max_us,
                _ => t[i].max_us,
            };
            if key == 0 {
                continue;   // never ranked above a real measurement
            }
            if best.is_none() || key > best_key {
                best = Some(i);
                best_key = key;
            }
        }
        match best {
            Some(i) => {
                taken[i / 64] |= 1u64 << (i % 64);
                out[written] = i as u32;
                written += 1;
            }
            None => break,
        }
    }
    written as u32
}

/// Self-check for the C side's probe plumbing.
///
/// FOUR instruments in this ticket family have become the thing they measured,
/// most recently a `[BKLMAX]` line that always reported its own call site. The
/// rule is that a measurement gets validated against a KNOWN answer before it
/// gets believed. cpu/scprof.c runs a synthetic "syscall" that burns a known
/// number of microseconds inside a known phase and passes what the table
/// recorded here.
///
/// Returns 0 if both the total and the phase are within a generous band of the
/// requested duration, else a bitmask: 1 = total wrong, 2 = phase wrong.
#[no_mangle]
pub extern "C" fn scprof_selfcheck(want_us: u64, got_us: u64, got_ph_us: u64) -> i32 {
    let lo = want_us.saturating_mul(8) / 10;
    let hi = want_us.saturating_mul(3);
    let mut bad = 0i32;
    if got_us < lo || got_us > hi {
        bad |= 1;
    }
    if got_ph_us < lo || got_ph_us > hi {
        bad |= 2;
    }
    bad
}

// There is deliberately NO scprof_reset(). The table is in .bss so it starts
// zeroed, and the rankings want totals CUMULATIVE SINCE BOOT: a per-window
// maximum would hide exactly the rare multi-hundred-millisecond outlier this
// ticket exists to explain. An exported reset with no caller would be a feature
// that never runs, which this tree treats as worse than an absent one.
