// rustkern/cpuobs.rs - #83: "which core is this task on", made answerable.
//
// WHY THIS EXISTS. `process_t::running_cpu` was written in exactly one place,
// at the END of sched_rq_pop(), and never invalidated anywhere. So it recorded
// "which core last PICKED this task up" and then kept saying it forever - for a
// task that was subsequently blocked, sleeping, or running on a different core.
// The field's own consumers had already drifted to match: the [WAKEPROBE]
// diagnostic in proc/process.c printed it under the label `last_cpu=`, which is
// what it really was. Every question of the form "which CPU was that running
// on" was therefore answered by a value that no code was maintaining.
//
// That matters beyond tidiness. #130 was a hang whose whole signature was "the
// recorded owner of the BKL is itself spinning for the BKL", i.e. a core-id
// field that reported confidently and wrongly; the fix was to establish that a
// core id is valid only while interrupts are masked and must never be carried
// across an sti. A per-task core id has the same failure mode and needed the
// same treatment, and until it got it, no per-core deadlock could be debugged
// with it.
//
// THE SPLIT. proc/process.h now carries TWO fields, because one cannot answer
// both questions:
//   running_cpu  the core CURRENTLY executing the task, or -1. Published at
//                the switch and cleared for the outgoing task on the same
//                switch.
//   last_cpu     STICKY, the core that last ran it. This is what a placement
//                hint wants (sched_rq_push() asks "where did this last run"
//                about a task that by definition is not running now), and it
//                is what the merged field was silently computing.
//
// THIS module owns the ACCOUNTING and the VERDICTS as pure functions over
// caller-built snapshots. The two stores themselves stay in C, inside
// sched_schedule()'s cli() region next to the context_switch call, for the same
// reason rustkern/schedwatch.rs states: that code is entangled with the PCB,
// the #610 critical section and the switch asm. Same split, same reason.
//
// Everything here is integer-only and lock-free: the kernel target is
// soft-float with SSE disabled, and this is called from the context-switch
// path, so it may not block, allocate, or take a lock (#426).

use core::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering};

/// Widest core id this module will record. A larger id is DROPPED rather than
/// wrapped, because a shifted-out bit would silently under-report the
/// distinct-core count, which is the one number this module exists to make
/// trustworthy.
///
/// #143: the doc comment here used to say "Matches SCHED_RQ_CPUS on the C
/// side". It did not. This constant was 32 and SCHED_RQ_CPUS was 8, so the two
/// had never matched, and the comment was the only thing asserting they did.
/// It is now the width of the mask below, and the C side (cpu/cpumax.h) carries
/// a _Static_assert that MAYTERA_MAX_CPUS <= 64, which is what actually keeps
/// the two in agreement. A constant that agrees with another constant only
/// because a comment says so is exactly the failure this project keeps hitting;
/// see blame.md on prose that lies.
pub const CPUOBS_MAX_CPUS: u32 = 64;

/// Bit N set = core N has been published in some task's `running_cpu`.
static SEEN_MASK: AtomicU64 = AtomicU64::new(0);
/// Switch-ins where the incoming task's previous core differed from this one.
static MIGRATIONS: AtomicU64 = AtomicU64::new(0);
/// Total switch-ins observed, so a migration count has a denominator.
static SWITCHINS: AtomicU64 = AtomicU64::new(0);
/// Ignored notes (core id out of range). Non-zero means the counts above are
/// LOW, and it must be visible rather than inferred.
static DROPS: AtomicU64 = AtomicU64::new(0);

static LAST_MIG_PID: AtomicU32 = AtomicU32::new(0);
static LAST_MIG_FROM: AtomicI32 = AtomicI32::new(-1);
static LAST_MIG_TO: AtomicI32 = AtomicI32::new(-1);

/// Record one switch-IN: task `pid` begins executing on core `to_cpu`, having
/// last executed on `from_cpu` (`-1` if it has never run).
///
/// Called from sched_publish_cpu() in proc/process.c on every context switch,
/// with interrupts masked. Relaxed ordering throughout: these are counters read
/// only by a periodic report, never by a correctness decision, so no reader
/// depends on seeing them in any particular order relative to other memory.
/// Relaxed atomics are what make this safe to call from several cores at once
/// without a lock, which is a hard requirement on this path.
#[no_mangle]
pub extern "C" fn sched_cpuobs_note_rs(pid: u32, from_cpu: i32, to_cpu: u32) {
    if to_cpu >= CPUOBS_MAX_CPUS {
        DROPS.fetch_add(1, Ordering::Relaxed);
        return;
    }
    SEEN_MASK.fetch_or(1u64 << to_cpu, Ordering::Relaxed);
    SWITCHINS.fetch_add(1, Ordering::Relaxed);
    // from_cpu < 0 is "never run before", which is a first dispatch and not a
    // migration. Counting it as one would make the migration total meaningless
    // on a boot that simply started a lot of processes.
    if from_cpu >= 0 && from_cpu != to_cpu as i32 {
        MIGRATIONS.fetch_add(1, Ordering::Relaxed);
        LAST_MIG_PID.store(pid, Ordering::Relaxed);
        LAST_MIG_FROM.store(from_cpu, Ordering::Relaxed);
        LAST_MIG_TO.store(to_cpu as i32, Ordering::Relaxed);
    }
}

/// How many DISTINCT cores have ever been published in a `running_cpu`.
///
/// This is the number that falsifies the pre-#83 state directly: while the
/// field was never maintained it read 0 for every task on every core, so any
/// honest count of distinct published cores was 1. Two or more can only happen
/// if the field is genuinely tracking cores.
#[no_mangle]
pub extern "C" fn sched_cpuobs_distinct_rs() -> u32 {
    SEEN_MASK.load(Ordering::Relaxed).count_ones()
}

#[no_mangle]
pub extern "C" fn sched_cpuobs_migrations_rs() -> u64 {
    MIGRATIONS.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn sched_cpuobs_switchins_rs() -> u64 {
    SWITCHINS.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn sched_cpuobs_drops_rs() -> u64 {
    DROPS.load(Ordering::Relaxed)
}

/// Most recent migration, for the report line. Null pointers are ignored
/// individually so a caller may ask for only part of it.
///
/// # Safety
/// Each non-null pointer must be a valid, aligned, writable location of the
/// matching type. The C caller passes the addresses of three locals.
#[no_mangle]
pub unsafe extern "C" fn sched_cpuobs_last_mig_rs(
    pid: *mut u32,
    from: *mut i32,
    to: *mut i32,
) {
    if !pid.is_null() {
        *pid = LAST_MIG_PID.load(Ordering::Relaxed);
    }
    if !from.is_null() {
        *from = LAST_MIG_FROM.load(Ordering::Relaxed);
    }
    if !to.is_null() {
        *to = LAST_MIG_TO.load(Ordering::Relaxed);
    }
}

/// LIVE cross-core verdict: given a snapshot of what each core's currently
/// running task reports as its own `running_cpu`, do at least two cores report
/// DIFFERENT values?
///
/// This is the strong form of the proof and the reason it is a separate
/// function from [`sched_cpuobs_distinct_rs`]. A distinct count accumulated
/// over time can be explained by one task moving around; this asks whether, at
/// a single instant, two cores disagree - which a field stuck at a constant can
/// never produce, however many samples you take.
///
/// Returns 1 if two entries are both >= 0 and differ, else 0. Entries of -1
/// (core idle, or no current task) are skipped rather than counted as a value,
/// because -1 is this field's "nowhere" and pairing it with a real core id
/// would manufacture a disagreement out of an absence.
///
/// # Safety
/// `vals` must point to at least `n` readable, aligned `i32`s. A null pointer
/// or `n == 0` returns 0.
#[no_mangle]
pub unsafe extern "C" fn sched_cpuobs_live_verdict_rs(vals: *const i32, n: u32) -> i32 {
    if vals.is_null() || n == 0 {
        return 0;
    }
    let mut first: i32 = -1;
    let mut have = false;
    for i in 0..n as usize {
        let v = *vals.add(i);
        if v < 0 {
            continue;
        }
        if !have {
            first = v;
            have = true;
        } else if v != first {
            return 1;
        }
    }
    0
}

// ===========================================================================
// SELF-TEST: the verdict functions are pure, so they can be proven on this
// exact build rather than argued about. Called from the C side at boot.
// ===========================================================================

/// Returns 0 on success, or the 1-based number of the first failing check.
///
/// The cases that matter are the ones that would let a BROKEN field look
/// proven: an all-equal snapshot (what the pre-#83 constant produced) must
/// return 0, and a snapshot whose only "difference" involves -1 must also
/// return 0.
#[no_mangle]
pub extern "C" fn sched_cpuobs_selftest_rs() -> i32 {
    // 1: the pre-#83 shape. Every core reports the same value -> NOT proven.
    let all_zero: [i32; 4] = [0, 0, 0, 0];
    if unsafe { sched_cpuobs_live_verdict_rs(all_zero.as_ptr(), 4) } != 0 {
        return 1;
    }
    // 2: a real disagreement -> proven.
    let mixed: [i32; 4] = [0, 1, 0, 3];
    if unsafe { sched_cpuobs_live_verdict_rs(mixed.as_ptr(), 4) } != 1 {
        return 2;
    }
    // 3: -1 is an absence, not a value. One core running, three idle.
    let one_live: [i32; 4] = [-1, 2, -1, -1];
    if unsafe { sched_cpuobs_live_verdict_rs(one_live.as_ptr(), 4) } != 0 {
        return 3;
    }
    // 4: all idle.
    let none: [i32; 4] = [-1, -1, -1, -1];
    if unsafe { sched_cpuobs_live_verdict_rs(none.as_ptr(), 4) } != 0 {
        return 4;
    }
    // 5: degenerate inputs must not read memory.
    if unsafe { sched_cpuobs_live_verdict_rs(core::ptr::null(), 4) } != 0 {
        return 5;
    }
    if unsafe { sched_cpuobs_live_verdict_rs(all_zero.as_ptr(), 0) } != 0 {
        return 6;
    }
    // 7: two live cores that AGREE is not a disagreement. This is the case that
    // separates "the field moves" from "two cores are on the same value", and
    // it is the one a lazy implementation (count distinct non-negative entries
    // >= 2) would get wrong.
    let agree: [i32; 4] = [1, 1, -1, -1];
    if unsafe { sched_cpuobs_live_verdict_rs(agree.as_ptr(), 4) } != 0 {
        return 7;
    }
    0
}
