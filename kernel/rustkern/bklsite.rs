// rustkern/bklsite.rs - #118: per-CALL-SITE Big Kernel Lock hold accounting.
//
// WHY THIS EXISTS
// ---------------
// [SCHEDCORE] reports the longest BKL hold with a "reason" tag (0x1NN =
// interrupt vector NN, 0x2NN = syscall NN, 0x300 = other). That tag is read at
// RELEASE time out of a per-cpu variable that is written at every ISR and
// syscall ENTRY (cpu/idt.c, proc/syscall.c) and never restored. So for any hold
// longer than one timer period the tag names the LAST interrupt that happened
// to fire during the hold, not the code that took the lock. At 250 Hz that is
// every hold over 4 ms, which is precisely the set of holds worth reporting.
// The published symptom of #118, "maxhold=446195us@0x120", is that defect:
// 0x120 is vector 0x20, the timer, and the timer is simply what ticked last.
//
// The fix is to key the accounting on the return address of the acquire that
// took the lock from depth 0, which cpu/smp.c already records for the #75 halt
// forensics (g_bkl_ra[cpu][0]). That address resolves to a real function with
// addr2line, and it cannot be overwritten by an unrelated nested interrupt.
//
// WHY A TABLE AND NOT JUST THE MAXIMUM
// ------------------------------------
// A single 446 ms outlier and a site that holds 5 ms a thousand times are very
// different problems with very different fixes, and the existing instrument can
// only see the first. This keeps count, total and maximum per site so both
// rankings are available, because "worst single hold" and "most lock time
// consumed" are frequently not the same site.
//
// STATE OWNERSHIP
// ---------------
// The table lives in C (cpu/smp.c) because it is written from inside the BKL
// release path with interrupts masked; the DECISIONS, meaning where an entry
// goes, how it is merged, and which entries rank highest, are here in Rust per
// the all-new-kernel-code-in-Rust rule, following the same split as
// rustkern/schedwatch.rs and rustkern/tickwatch.rs.
//
// Integer-only: the kernel target is soft-float with SSE disabled, so there is
// no averaging in floating point anywhere in this file.

/// One call site. repr(C) and sizeof-locked on the C side.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BklSite {
    /// Return address of the depth 0 to 1 acquire. 0 means the slot is free.
    pub ra: u64,
    /// How many completed holds were attributed to this site.
    pub count: u64,
    /// Sum of those holds, microseconds of real time (cpu/mono.h).
    pub total_us: u64,
    /// Longest single hold at this site, microseconds.
    pub max_us: u64,
    /// Longest gap between consecutive interrupt entries observed DURING this
    /// site's worst hold. See bkl_irq_mark in cpu/smp.c: with interrupts
    /// enabled the timer fires every 4 ms, so a gap near the tick period means
    /// the core was genuinely EXECUTING for the whole hold, and a gap near the
    /// hold length means it was not running at all (halted, or descheduled by
    /// the hypervisor). This is what separates a real critical section from a
    /// measurement artefact, and #118 exists because the old instrument could
    /// not tell them apart.
    pub max_gap_us: u64,
    /// Syscall number in effect during this site's worst hold, or U64_MAX if no
    /// syscall had run on this core yet. Unlike the [SCHEDCORE] reason tag this
    /// is written ONLY by the syscall dispatcher, so an interrupt arriving
    /// mid-hold cannot overwrite it. For the syscall_entry site it names the
    /// syscall; for any other site it is merely the last syscall to have run,
    /// and means nothing.
    pub worst_syscall: u64,
}

/// Slots. A boot touches far fewer distinct depth-0 acquire sites than this;
/// overflow is reported rather than silently folded, because a silently-merged
/// site would be indistinguishable from a real one (the trap blame.md records
/// as "NEVER MEASURED and HEALTHY must not share a value").
pub const BKLSITE_MAX: u32 = 48;

/// Result of bklsite_add: a new slot was claimed.
pub const BKLSITE_ADDED: i32 = 0;
/// Merged into an existing slot for the same call site.
pub const BKLSITE_MERGED: i32 = 1;
/// Table full: this sample was DROPPED and is not counted anywhere.
pub const BKLSITE_FULL: i32 = 2;
/// Caller passed a null table or a bad length.
pub const BKLSITE_BADARG: i32 = -1;

#[inline]
unsafe fn slice_of<'a>(tab: *mut BklSite, n: u32) -> Option<&'a mut [BklSite]> {
    if tab.is_null() || n == 0 || n > BKLSITE_MAX {
        return None;
    }
    Some(core::slice::from_raw_parts_mut(tab, n as usize))
}

/// Record one completed hold of us microseconds taken at return address ra,
/// during which the longest observed inter-interrupt gap was gap_us.
///
/// # Safety
/// tab must point to at least n writable, initialised BklSite entries and must
/// not be concurrently accessed. cpu/smp.c calls this with interrupts masked,
/// holding the lock being released, which satisfies both.
#[no_mangle]
pub unsafe extern "C" fn bklsite_add(
    tab: *mut BklSite,
    n: u32,
    ra: u64,
    us: u64,
    gap_us: u64,
    syscall: u64,
) -> i32 {
    let t = match slice_of(tab, n) {
        Some(t) => t,
        None => return BKLSITE_BADARG,
    };
    // ra == 0 would collide with the free-slot marker. Fold those into a single
    // reserved bucket rather than dropping them, so the totals still balance.
    let key = if ra == 0 { 1u64 } else { ra };
    let mut free: Option<usize> = None;
    for i in 0..t.len() {
        if t[i].ra == key {
            t[i].count = t[i].count.wrapping_add(1);
            t[i].total_us = t[i].total_us.wrapping_add(us);
            if us > t[i].max_us {
                t[i].max_us = us;
                // The gap and the syscall belong to the WORST hold, not the most
                // recent one: they are evidence about that specific hold.
                t[i].max_gap_us = gap_us;
                t[i].worst_syscall = syscall;
            }
            return BKLSITE_MERGED;
        }
        if t[i].ra == 0 && free.is_none() {
            free = Some(i);
        }
    }
    match free {
        Some(i) => {
            t[i].ra = key;
            t[i].count = 1;
            t[i].total_us = us;
            t[i].max_us = us;
            t[i].max_gap_us = gap_us;
            t[i].worst_syscall = syscall;
            BKLSITE_ADDED
        }
        None => BKLSITE_FULL,
    }
}

/// Rank order selector: by total_us descending.
pub const BKLSITE_BY_TOTAL: i32 = 0;

/// Write the indices of the top out_n occupied slots into out_idx, ranked as
/// requested (BKLSITE_BY_TOTAL, or anything else for max_us descending).
/// Returns how many indices were written.
///
/// Selection sort over at most BKLSITE_MAX entries, picking only as many as the
/// caller asked for: this runs from the serial report path, not the hot path.
///
/// # Safety
/// tab must point to n readable entries and out_idx to out_n writable u32s.
#[no_mangle]
pub unsafe extern "C" fn bklsite_top(
    tab: *const BklSite,
    n: u32,
    by: i32,
    out_idx: *mut u32,
    out_n: u32,
) -> u32 {
    if tab.is_null() || out_idx.is_null() || n == 0 || n > BKLSITE_MAX || out_n == 0 {
        return 0;
    }
    let t = core::slice::from_raw_parts(tab, n as usize);
    let out = core::slice::from_raw_parts_mut(out_idx, out_n as usize);
    let mut written = 0usize;
    let mut taken = [false; BKLSITE_MAX as usize];
    while written < out.len() {
        let mut best: Option<usize> = None;
        let mut best_key = 0u64;
        for i in 0..t.len() {
            if t[i].ra == 0 || taken[i] {
                continue;
            }
            let key = if by == BKLSITE_BY_TOTAL { t[i].total_us } else { t[i].max_us };
            if best.is_none() || key > best_key {
                best = Some(i);
                best_key = key;
            }
        }
        match best {
            Some(i) => {
                taken[i] = true;
                out[written] = i as u32;
                written += 1;
            }
            None => break,
        }
    }
    written as u32
}

// There is deliberately NO bklsite_reset(). The table is in .bss so it starts
// zeroed, and the ranking wants totals CUMULATIVE SINCE BOOT rather than per
// report window: "which site has consumed the most lock time" is a question
// about the whole run. An exported reset with no caller would be a feature that
// never runs, which this tree treats as worse than an absent one.
