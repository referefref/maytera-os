// wakeloss.rs - #167: the verdict for the block/wake race reproducer.
//
// WHY THIS IS RUST AND ITS CALLERS ARE NOT. CLAUDE.md's standing rule is that
// new kernel code is Rust unless there is a measured performance or genuine
// entanglement reason. The reproducer in proc/wakeloss.c stays C for the
// entanglement reason: its thread bodies are nothing but expansions of the
// wait_event() macro family and calls to proc_create/proc_sleep, i.e. they ARE
// the C wait path under test, and wrapping them would change what is being
// measured. This file is the part that is not entangled with anything: given
// two arrays of counters, decide whether a waiter has stopped making progress.
// That is arithmetic over indices, which is the class of code where an
// out-of-bounds read is a real risk in C and is impossible to express here
// without saying `unsafe` out loud.
//
// NO FLOAT: the kernel target is x86_64-unknown-none (soft-float, SSE
// disabled), so everything below is integer.

/// No waiter is stalled.
pub const WL_NONE: i32 = -1;
/// Upper bound on waiters, matching WL_WAITERS in proc/wakeloss.c. The FFI
/// takes `n` and clamps to this, so a mismatch cannot walk off the arrays.
pub const WL_MAX: usize = 8;

/// Advance the stall bookkeeping for one sample and return the index of the
/// first waiter that has now been stalled for `thresh` consecutive samples, or
/// `WL_NONE`.
///
/// A waiter counts as stalled for a sample only when its round counter did not
/// move AND the waker kicked at least once in the same interval. That second
/// clause is the whole reason this is not a timeout: it distinguishes "this
/// waiter was deleted from the scheduler" from "nothing woke it, correctly".
/// #165's leading failure mode was instruments whose false positive was
/// indistinguishable from their true positive; this one cannot fire on an idle
/// system, because on an idle system `kicks_delta` is zero.
///
/// `last` and `stuck` are caller-owned state, updated in place.
fn verdict(rounds: &[u64], last: &mut [u64], stuck: &mut [u32],
           kicks_delta: u64, thresh: u32) -> i32 {
    let mut hit: i32 = WL_NONE;
    for i in 0..rounds.len() {
        if rounds[i] == last[i] && kicks_delta > 0 {
            stuck[i] = stuck[i].saturating_add(1);
        } else {
            stuck[i] = 0;
        }
        last[i] = rounds[i];
        // Report the LOWEST-numbered stalled waiter, but keep updating the rest
        // so a second one going the same way is not hidden by an early return.
        if hit == WL_NONE && thresh > 0 && stuck[i] >= thresh {
            hit = i as i32;
        }
    }
    hit
}

/// C entry point. See proc/wakeloss.c for the caller.
///
/// # Safety
/// `rounds`, `last` and `stuck` must each point to at least `n` elements.
#[no_mangle]
pub extern "C" fn wakeloss_verdict_rs(rounds: *const u64, last: *mut u64,
                                      stuck: *mut u32, n: u32,
                                      kicks_delta: u64, thresh: u32) -> i32 {
    if rounds.is_null() || last.is_null() || stuck.is_null() || n == 0 {
        return WL_NONE;
    }
    let n = core::cmp::min(n as usize, WL_MAX);
    unsafe {
        let r = core::slice::from_raw_parts(rounds, n);
        let l = core::slice::from_raw_parts_mut(last, n);
        let s = core::slice::from_raw_parts_mut(stuck, n);
        verdict(r, l, s, kicks_delta, thresh)
    }
}

/// PROVE THE DETECTOR GOES RED AND GREEN. Returns the number of WRONG cases, so
/// 0 means every case behaved. Called from wakeloss_selftest() on every boot,
/// gate or no gate: #165 lost most of a campaign to five instruments that were
/// confidently wrong before the kernel was ever suspected, and a detector nobody
/// has watched go red is not a detector.
#[no_mangle]
pub extern "C" fn wakeloss_selftest_rs() -> u32 {
    let mut bad: u32 = 0;

    // CASE 1: a healthy run. Every waiter advances every sample.
    {
        let mut last = [0u64; 4];
        let mut stuck = [0u32; 4];
        let mut rounds = [0u64; 4];
        for step in 1..=10u64 {
            for i in 0..4 { rounds[i] = step * (i as u64 + 1); }
            if verdict(&rounds, &mut last, &mut stuck, 50, 3) != WL_NONE { bad += 1; }
        }
    }

    // CASE 2: waiter 2 is deleted from the scheduler at step 4 and never moves
    // again. It must be reported, and only after `thresh` samples, not on the
    // first one - a single missed sample is slowness, not deletion.
    {
        let mut last = [0u64; 4];
        let mut stuck = [0u32; 4];
        let mut rounds = [0u64; 4];
        let mut fired_at: i32 = -1;
        for step in 1..=10u64 {
            for i in 0..4 {
                if i == 2 && step >= 4 { continue; }   // frozen
                rounds[i] = step;
            }
            let v = verdict(&rounds, &mut last, &mut stuck, 50, 3);
            if v == 2 && fired_at < 0 { fired_at = step as i32; }
            if v != WL_NONE && v != 2 { bad += 1; }    // accused the wrong one
        }
        // last[2] was stamped at step 3, so steps 4,5,6 are the three
        // no-progress samples and step 6 is the earliest correct report.
        // Checked as an EQUALITY, not ">= 4": a detector that fires on the
        // first missed sample would call every slow waiter deleted.
        if fired_at != 6 { bad += 1; }
    }

    // CASE 3: an IDLE system. Nothing advances, but the waker is not kicking
    // either, so nothing may be reported. This is the false positive that would
    // make the detector worthless, and it is the one #165 warns about by name.
    {
        let mut last = [0u64; 4];
        let mut stuck = [0u32; 4];
        let rounds = [7u64; 4];
        for _ in 0..20 {
            if verdict(&rounds, &mut last, &mut stuck, 0, 3) != WL_NONE { bad += 1; }
        }
    }

    // CASE 4: a stalled waiter that RECOVERS must clear, not latch.
    {
        let mut last = [0u64; 4];
        let mut stuck = [0u32; 4];
        let mut rounds = [0u64; 4];
        for _ in 0..2 { let _ = verdict(&rounds, &mut last, &mut stuck, 50, 3); }
        for i in 0..4 { rounds[i] = 1; }
        if verdict(&rounds, &mut last, &mut stuck, 50, 3) != WL_NONE { bad += 1; }
        for i in 0..4 { if stuck[i] != 0 { bad += 1; } }
    }

    bad
}
