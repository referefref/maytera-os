// rustkern/sched_age.rs - #254/#601 scheduler anti-starvation policy.
//
// The MayteraOS ready queue is a single intrusive list kept sorted by priority,
// popped from the head. That is STRICT priority with no aging: while any
// higher-priority process stays runnable, a lower-priority one is passed over
// for ever. Measured on build 969 during a 103 MB App Store install: the
// PRIO_LOW `cron` thread sat in PROC_STATE_READY for 1,180 consecutive ticks
// (4.7 s) at a time behind a Ring-3 app pegged at 99%, and `idle` (PRIO_NORMAL,
// requeued by sched_tick) sat permanently ahead of every PRIO_LOW thread.
//
// THIS module owns the DECISION - "which queued entries have waited past the
// bound and should be promoted this sweep" - as a pure function over a
// caller-built snapshot. The list surgery that acts on the decision stays in C
// (proc/process.c): it is intrusive-linked-list manipulation entangled with the
// PCB, the context switch and the #610 cli() critical section, so moving it is a
// port of existing C, not new logic. See CHANGELOG.
//
// Called ONLY from the rate-limited sweep (once per 100 ms), never per schedule:
// sched_schedule() is the hottest path in the kernel and pays nothing but one
// tick comparison for this feature.

/// One ready-queue entry, snapshotted in queue order (head first).
/// Mirrors sched_age_ent_t in proc/process.c; sizeof-locked there by
/// _Static_assert.
#[repr(C)]
pub struct SchedAgeEnt {
    pub ready_since: u64,
    pub prio: u32,
    pub boosted: u32,
}

/// Select the entries to promote.
///
/// `ents[0..n]`   ready queue in queue order, head first.
/// `now`          current scheduler tick.
/// `bound`        starvation bound, in ticks.
/// `max_promote`  cap on promotions per sweep.
/// `out[0..n]`    written 1 (promote) / 0 (leave), always fully initialised on
///                every success path.
///
/// Returns the number marked, or -1 on invalid arguments.
///
/// Policy, and why each rule is here:
///  * entry 0 (the head) is never promoted: it is what the very next
///    remove_from_ready_queue() will return, so by definition it is not being
///    passed over.
///  * an already-boosted entry is never promoted again. The boost is ONE-SHOT
///    (C clears it the moment the process is selected), so without this rule a
///    process that is boosted but not yet run would be re-promoted every sweep
///    and could hold the head indefinitely - swapping one starvation for
///    another.
///  * `max_promote` caps a single sweep. A burst of waiters must not be able to
///    invert the whole queue in one go and push a genuinely high-priority
///    process (PRIO_REALTIME) behind a wall of promoted background threads.
///  * the age is computed wrap-safely, and an age in the "impossible" upper
///    half of the u64 range (a ready_since in the future, i.e. an entry
///    inserted after `now` was sampled) is REFUSED rather than treated as a
///    huge age. Fail-closed: a missed promotion costs one more sweep (100 ms),
///    a bogus one puts a just-queued process ahead of a starving one.
#[no_mangle]
pub extern "C" fn sched_age_select_rs(
    ents: *const SchedAgeEnt,
    n: u32,
    now: u64,
    bound: u64,
    max_promote: u32,
    out: *mut u8,
    outcap: u32,
) -> i32 {
    if ents.is_null() || out.is_null() {
        return -1;
    }
    if n > outcap {
        return -1;
    }
    let n = n as usize;
    if n == 0 {
        return 0;
    }

    // SAFETY: the caller guarantees `ents` points at `n` contiguous, readable,
    // properly aligned SchedAgeEnt and `out` at `outcap >= n` writable bytes
    // (both are stack arrays in sched_age_ready_queue()). Every access below
    // goes through these two slices, so the indexing is bounds-checked.
    let e: &[SchedAgeEnt] = unsafe { core::slice::from_raw_parts(ents, n) };
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, n) };

    for slot in o.iter_mut() {
        *slot = 0;
    }

    // A queue of one cannot starve anybody, and a zero bound would promote
    // everything on every sweep.
    if n < 2 || bound == 0 {
        return 0;
    }

    let mut marked: u32 = 0;
    for i in 1..n {
        if marked >= max_promote {
            break;
        }
        if e[i].boosted != 0 {
            continue;
        }
        let age = now.wrapping_sub(e[i].ready_since);
        if age > u64::MAX / 2 {
            continue; // ready_since is in the future: refuse
        }
        if age < bound {
            continue;
        }
        o[i] = 1;
        marked += 1;
    }
    marked as i32
}
