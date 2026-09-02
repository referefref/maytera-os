// rustkern/affinity.rs - #affinity: PERSISTENT PER-PROCESS CPU AFFINITY, and
// the PER-PROCESS migration accounting that is the only way to tell whether it
// achieved anything.
//
// WHAT WAS ALREADY HERE, so this module is not mistaken for more than it is.
// Measured by reading the tree at dev 75d0b2c8, not assumed:
//
//   * SOFT AFFINITY ALREADY EXISTS and it is good. process_t::last_cpu is
//     sticky, written at the switch by sched_publish_cpu(), and fed to
//     sched_place_rs() (rustkern/schedwatch.rs) as `prev_cpu`, where it wins
//     outright among idle cores and serves as the final tie-break when queueing.
//     Nothing here replaces that. The gaps it does not cover are placement step
//     2 (PREEMPTION picks the weakest victim core with no affinity term at all)
//     and WORK STEALING (cpu/smp.c:1626 already says it "STEALS ACROSS CORES
//     with no affinity of any kind").
//
//   * process_t::sched_pinned is NOT affinity. It is the #75 selection pin,
//     cpu+1, held from the pop until the switch, so exit cannot tear down a task
//     a core has committed to. Its lifetime is microseconds.
//
//   * process_t::migratable is DEAD. proc_create_user_as() sets `int __mig = 0;`
//     as a literal and zeroes g_next_user_migratable without reading it, so
//     SYS_RUN_NEXT_ON_AP has set a flag that is discarded three lines before it
//     would be read, and returned success, since #67 pass 2.
//
// SO WHAT THIS ADDS is the thing that genuinely did not exist: a PERSISTENT
// per-process CPU SET, and the per-process migration counters to judge it by.
//
// DEFAULT IS "ANY CORE". An absent table entry means every core is allowed, so
// a process that never asks is affected in no way whatsoever. That is a
// property of the lookup, not a value written at process creation, which is why
// this needs no hook in the process lifecycle to be correct.
//
// SOFT, NOT HARD, and this is the important design decision. A mask here is a
// PREFERENCE that placement honours and starvation overrides. A hard pin that
// leaves a core idle while its task waits is worse than the migration it
// prevents, and it is the failure mode that makes affinity features get turned
// off. The enforcement side (proc/process.c) therefore carries a starvation
// escape; this module only answers "is cpu N preferred for pid P".
//
// WHY A HASH TABLE AND NOT A FIELD ON process_t. Two reasons, one principled
// and one practical. Principled: the migration counters are fed from
// sched_cpuobs_note_rs(), which already receives (pid, from_cpu, to_cpu) on
// every switch-in, so the ENTIRE measurement half of this ticket needs no
// change to proc/process.c at all. Practical: proc/process.c is being worked
// concurrently on a two-core panic in the select-and-publish invariant, and a
// conflict there is a silent correctness bug rather than a build failure.
//
// #426 COMPLIANCE. Lock-free, allocation-free, wait-free. Every loop has a
// compile-time-constant bound. Called from the context-switch path with
// interrupts masked, so it may not block, allocate or take a lock.

use core::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering};

/// Table capacity. Power of two so the hash reduces with a mask rather than a
/// division on the switch path.
pub const AFF_SLOTS: usize = 256;
/// Probe distance. A lookup touches at most this many slots, so the cost on the
/// context-switch path is bounded by a constant and not by occupancy.
pub const AFF_PROBE: usize = 4;

/// "Every core", the default for any process with no entry.
pub const AFF_ALL: u64 = u64::MAX;

/// Reserved pid meaning "slot empty". Pid 0 is the idle task, which is
/// per-core by construction and can never want an affinity mask, so borrowing
/// it as the empty marker costs nothing.
const EMPTY: u32 = 0;

struct Slot {
    pid: AtomicU32,
    mask: AtomicU64,
    /// Switch-ins for this pid where the incoming core differed from the last.
    migrations: AtomicU64,
    /// Total switch-ins for this pid: the denominator, without which a
    /// migration count cannot be compared between two runs of different length.
    switchins: AtomicU64,
    /// Last core seen for this pid, for the migration test. Independent of
    /// process_t::last_cpu on purpose: this module must be able to account
    /// without reading the PCB.
    last: AtomicI32,
    /// Value of EPOCH at this slot's last switch-in. Used ONLY to reclaim slots
    /// belonging to processes that have exited. See `find_or_make`.
    stamp: AtomicU64,
}

#[allow(clippy::declare_interior_mutable_const)]
const EMPTY_SLOT: Slot = Slot {
    pid: AtomicU32::new(EMPTY),
    mask: AtomicU64::new(AFF_ALL),
    migrations: AtomicU64::new(0),
    switchins: AtomicU64::new(0),
    last: AtomicI32::new(-1),
    stamp: AtomicU64::new(0),
};

static TABLE: [Slot; AFF_SLOTS] = [EMPTY_SLOT; AFF_SLOTS];

/// #affinity A/B GATE: 1 = the mask is INERT, every process reads AFF_ALL and
/// every core is allowed.
///
/// WHY THIS EXISTS RATHER THAN TWO BUILDS. A before/after taken from two
/// different kernel.elf files compares two binaries, not one change: any other
/// difference between them, including compiler layout, rides along in the
/// result. Armed from /NOAFF.TXT on the FAT ESP (main.c), the same marker-file
/// convention as /SMPSCHED.TXT, /TESTINPUT.TXT and /INPUTLAT.TXT, so BOTH ARMS
/// COME FROM ONE KERNEL.ELF and the only variable is this flag.
///
/// It gates only the two READ paths. Masks are still stored and still reported,
/// so the [AFFMIG] line looks identical in both arms and the two logs are
/// directly comparable line for line.
static DISABLED: AtomicU32 = AtomicU32::new(0);

/// Arm or disarm the A/B gate. Returns the previous value.
#[no_mangle]
pub extern "C" fn affinity_set_disabled_rs(on: i32) -> i32 {
    DISABLED.swap(if on != 0 { 1 } else { 0 }, Ordering::Release) as i32
}

/// Is the mask currently inert? For the report line, so a log can never be
/// misread as the wrong arm.
#[no_mangle]
pub extern "C" fn affinity_disabled_rs() -> i32 {
    DISABLED.load(Ordering::Relaxed) as i32
}

/// Entries refused because every probe slot was taken. Non-zero means the
/// accounting is INCOMPLETE for some processes, so it is reported rather than
/// inferred: a silently-dropped process is how a migration count comes to look
/// better than the truth.
static FULL: AtomicU64 = AtomicU64::new(0);

/// Monotonic count of switch-ins accounted here. Serves as a coarse clock for
/// slot reclamation, so this module needs no timer and can be called with
/// interrupts off.
static EPOCH: AtomicU64 = AtomicU64::new(0);

/// Slots reclaimed from processes that had not been switched in for
/// AFF_STALE_EPOCHS. Reported so a reclaim is visible as an event rather than
/// silently losing a process's counter history.
static EVICTED: AtomicU64 = AtomicU64::new(0);

/// How far behind EPOCH a slot's stamp must be before it may be reclaimed.
///
/// WHY RECLAMATION EXISTS AT ALL. The honest release point for an entry is
/// process exit, and that lives in proc/process.c, which this change is
/// deliberately not touching (a concurrent agent is working a two-core panic in
/// the select-and-publish invariant there). Without any reclaim, every exited
/// process would hold its slot for ever and the table would eventually start
/// refusing LIVE processes, which would show up only as FULL rising.
///
/// The rule is conservative: a slot is reclaimed only when its probe run is
/// otherwise full AND it has seen no switch-in for this many system-wide
/// switch-ins. At the measured rate of a few thousand context switches per
/// second, 200k is on the order of a minute of not running at all, which no
/// live process on a desktop does. The cost of getting it wrong is losing one
/// process's counter history, never a correctness fault: the entry is recreated
/// on its next switch-in with a fresh, and therefore visibly reset, count.
pub const AFF_STALE_EPOCHS: u64 = 200_000;

/// Fibonacci hash. Spreading matters because pids are allocated sequentially,
/// so the low bits alone would cluster every live process into one probe run.
fn slot_of(pid: u32) -> usize {
    ((pid.wrapping_mul(2_654_435_761)) as usize >> 8) & (AFF_SLOTS - 1)
}

/// Find the slot holding `pid`, without creating one.
fn find(pid: u32) -> Option<usize> {
    if pid == EMPTY {
        return None;
    }
    let h = slot_of(pid);
    let mut i = 0;
    while i < AFF_PROBE {
        let s = (h + i) & (AFF_SLOTS - 1);
        if TABLE[s].pid.load(Ordering::Acquire) == pid {
            return Some(s);
        }
        i += 1;
    }
    None
}

/// Find the slot holding `pid`, creating one if there is room.
fn find_or_make(pid: u32) -> Option<usize> {
    if pid == EMPTY {
        return None;
    }
    let h = slot_of(pid);
    let mut i = 0;
    while i < AFF_PROBE {
        let s = (h + i) & (AFF_SLOTS - 1);
        if TABLE[s].pid.load(Ordering::Acquire) == pid {
            return Some(s);
        }
        i += 1;
    }
    // No existing entry. Claim the first empty slot in the probe run with a CAS
    // so two cores cannot both take it. The CAS is BOUNDED: one attempt per
    // slot, never retried in a loop, because an unbounded retry on the
    // context-switch path is the #426 anti-pattern.
    let mut i = 0;
    while i < AFF_PROBE {
        let s = (h + i) & (AFF_SLOTS - 1);
        if TABLE[s]
            .pid
            .compare_exchange(EMPTY, pid, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            TABLE[s].mask.store(AFF_ALL, Ordering::Relaxed);
            TABLE[s].migrations.store(0, Ordering::Relaxed);
            TABLE[s].switchins.store(0, Ordering::Relaxed);
            TABLE[s].last.store(-1, Ordering::Relaxed);
            TABLE[s].stamp.store(EPOCH.load(Ordering::Relaxed), Ordering::Relaxed);
            return Some(s);
        }
        // Lost the race to this pid itself: use it.
        if TABLE[s].pid.load(Ordering::Acquire) == pid {
            return Some(s);
        }
        i += 1;
    }
    // Every probe slot is taken by a live-looking pid. Reclaim the STALEST one
    // in the run, and only if it is genuinely stale. Bounded: one more pass
    // over the same AFF_PROBE slots, no retry loop.
    let now = EPOCH.load(Ordering::Relaxed);
    let mut victim: Option<usize> = None;
    let mut oldest: u64 = u64::MAX;
    let mut i = 0;
    while i < AFF_PROBE {
        let s = (h + i) & (AFF_SLOTS - 1);
        let st = TABLE[s].stamp.load(Ordering::Relaxed);
        if now.wrapping_sub(st) >= AFF_STALE_EPOCHS && st < oldest {
            oldest = st;
            victim = Some(s);
        }
        i += 1;
    }
    if let Some(s) = victim {
        // Take it by CAS from the pid we observed, so a slot that came alive
        // between the scan and here is not stolen from under its owner.
        let old_pid = TABLE[s].pid.load(Ordering::Acquire);
        if old_pid != EMPTY
            && TABLE[s]
                .pid
                .compare_exchange(old_pid, pid, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
        {
            TABLE[s].mask.store(AFF_ALL, Ordering::Relaxed);
            TABLE[s].migrations.store(0, Ordering::Relaxed);
            TABLE[s].switchins.store(0, Ordering::Relaxed);
            TABLE[s].last.store(-1, Ordering::Relaxed);
            TABLE[s].stamp.store(now, Ordering::Relaxed);
            EVICTED.fetch_add(1, Ordering::Relaxed);
            return Some(s);
        }
    }
    FULL.fetch_add(1, Ordering::Relaxed);
    None
}

// ---------------------------------------------------------------------------
// Mask API.
// ---------------------------------------------------------------------------

/// Set the affinity mask for `pid`. Bit N set = core N is PREFERRED.
///
/// A mask of 0 is refused rather than stored: it would mean "no core may run
/// this", which is a request to hang the process, and a silently-accepted 0
/// would be indistinguishable from the default until the process stopped
/// running. Returns 0 on success, -1 on a bad pid or a zero mask, -2 if the
/// table is full.
#[no_mangle]
pub extern "C" fn affinity_set_rs(pid: u32, mask: u64) -> i32 {
    if pid == EMPTY || mask == 0 {
        return -1;
    }
    match find_or_make(pid) {
        Some(s) => {
            TABLE[s].mask.store(mask, Ordering::Release);
            0
        }
        None => -2,
    }
}

/// The affinity mask for `pid`, or AFF_ALL if it has none.
///
/// AFF_ALL for an unknown pid is the whole default policy in one line: a
/// process that never asked is unconstrained, and nothing had to run at its
/// creation for that to be true.
#[no_mangle]
pub extern "C" fn affinity_get_rs(pid: u32) -> u64 {
    // A/B gate: inert means unconstrained, which is exactly the default, so the
    // disabled arm behaves identically to a kernel with no mask feature at all.
    if DISABLED.load(Ordering::Relaxed) != 0 {
        return AFF_ALL;
    }
    match find(pid) {
        Some(s) => TABLE[s].mask.load(Ordering::Acquire),
        None => AFF_ALL,
    }
}

/// Is core `cpu` preferred for `pid`? 1 = yes (including the default), 0 = no.
#[no_mangle]
pub extern "C" fn affinity_allows_rs(pid: u32, cpu: u32) -> i32 {
    if DISABLED.load(Ordering::Relaxed) != 0 {
        return 1;   // A/B gate: every core allowed
    }
    if cpu >= 64 {
        // Out of range for a 64-bit mask. Answer YES rather than NO: a mask
        // that cannot express a core must not be read as EXCLUDING it, or a
        // machine wider than the mask would silently strand work.
        return 1;
    }
    if (affinity_get_rs(pid) & (1u64 << cpu)) != 0 {
        1
    } else {
        0
    }
}

/// Release `pid`'s entry, at process exit. Safe to call for a pid that has
/// none. Keeping the entry would eventually fill the probe run for its hash and
/// start refusing live processes, which would show up as FULL rising rather
/// than as anything visibly wrong.
#[no_mangle]
pub extern "C" fn affinity_clear_rs(pid: u32) {
    if let Some(s) = find(pid) {
        TABLE[s].mask.store(AFF_ALL, Ordering::Relaxed);
        TABLE[s].migrations.store(0, Ordering::Relaxed);
        TABLE[s].switchins.store(0, Ordering::Relaxed);
        TABLE[s].last.store(-1, Ordering::Relaxed);
        TABLE[s].stamp.store(0, Ordering::Relaxed);
        TABLE[s].pid.store(EMPTY, Ordering::Release);
    }
}

// ---------------------------------------------------------------------------
// Per-process migration accounting.
// ---------------------------------------------------------------------------

/// Record one switch-IN for `pid` on core `to_cpu`.
///
/// Called from sched_cpuobs_note_rs() (rustkern/cpuobs.rs), which the scheduler
/// already invokes on every context switch with exactly these arguments. That
/// is why the measurement half of this ticket needs no change to
/// proc/process.c: the data was already flowing past a Rust function.
///
/// `from_cpu < 0` means the task has never run, which is a first dispatch and
/// NOT a migration. Counting it would make the total meaningless on a boot that
/// simply started a lot of processes.
///
/// An entry is created on demand, so a process is accounted from its first
/// switch-in whether or not it ever asked for an affinity mask. That is
/// deliberate: the BASELINE has to cover the processes that will never be
/// pinned, or a before/after comparison only measures the ones that were.
pub fn note_switchin(pid: u32, from_cpu: i32, to_cpu: u32) {
    let s = match find_or_make(pid) {
        Some(s) => s,
        None => return,
    };
    let e = EPOCH.fetch_add(1, Ordering::Relaxed);
    TABLE[s].stamp.store(e, Ordering::Relaxed);
    TABLE[s].switchins.fetch_add(1, Ordering::Relaxed);
    TABLE[s].last.store(to_cpu as i32, Ordering::Relaxed);
    if from_cpu >= 0 && from_cpu != to_cpu as i32 {
        TABLE[s].migrations.fetch_add(1, Ordering::Relaxed);
    }
}

/// Migration and switch-in counts for `pid`. Returns 0 on success, -1 if the
/// pid has no entry (which means it has never been switched in).
///
/// # Safety
/// Each non-null pointer must be a valid, aligned, writable u64.
#[no_mangle]
pub unsafe extern "C" fn affinity_stats_rs(
    pid: u32,
    migrations: *mut u64,
    switchins: *mut u64,
    mask: *mut u64,
    last_cpu: *mut i32,
) -> i32 {
    let s = match find(pid) {
        Some(s) => s,
        None => return -1,
    };
    if !migrations.is_null() {
        *migrations = TABLE[s].migrations.load(Ordering::Relaxed);
    }
    if !switchins.is_null() {
        *switchins = TABLE[s].switchins.load(Ordering::Relaxed);
    }
    if !mask.is_null() {
        *mask = TABLE[s].mask.load(Ordering::Relaxed);
    }
    if !last_cpu.is_null() {
        *last_cpu = TABLE[s].last.load(Ordering::Relaxed);
    }
    0
}

/// Walk the table for a report. Fills `pid_out`, `mig_out`, `sw_out` and
/// `mask_out` with up to `max` LIVE entries, returns how many were written.
///
/// Deliberately NOT sorted here: sorting would be a loop whose bound depends on
/// occupancy, and this is called from a report, not the switch path, so the
/// caller can rank the small result itself.
///
/// # Safety
/// Each output pointer must be valid, aligned and writable for `max` elements.
#[no_mangle]
pub unsafe extern "C" fn affinity_walk_rs(
    max: u32,
    pid_out: *mut u32,
    mig_out: *mut u64,
    sw_out: *mut u64,
    mask_out: *mut u64,
) -> u32 {
    if max == 0 || pid_out.is_null() {
        return 0;
    }
    let mut n: u32 = 0;
    let mut i = 0usize;
    while i < AFF_SLOTS && n < max {
        let pid = TABLE[i].pid.load(Ordering::Acquire);
        if pid != EMPTY {
            *pid_out.add(n as usize) = pid;
            if !mig_out.is_null() {
                *mig_out.add(n as usize) = TABLE[i].migrations.load(Ordering::Relaxed);
            }
            if !sw_out.is_null() {
                *sw_out.add(n as usize) = TABLE[i].switchins.load(Ordering::Relaxed);
            }
            if !mask_out.is_null() {
                *mask_out.add(n as usize) = TABLE[i].mask.load(Ordering::Relaxed);
            }
            n += 1;
        }
        i += 1;
    }
    n
}

/// Entries refused because the table was full. MUST be 0; non-zero means some
/// processes are unaccounted and every migration total below is LOW.
#[no_mangle]
pub extern "C" fn affinity_full_rs() -> u64 {
    FULL.load(Ordering::Relaxed)
}

// ---------------------------------------------------------------------------
// Deferred spawn request: the honest replacement for the dead `migratable`.
// ---------------------------------------------------------------------------
//
// SYS_RUN_NEXT_ON_AP ("run the next app I launch on an application processor",
// the `runap` shell command) has been a LIE since #67 pass 2:
// proc_set_next_migratable() set a global that proc_create_user_as() zeroes
// three lines before it would be read, because `int __mig = 0;` is a literal.
// The syscall returned success for a request that was discarded.
//
// The mechanism it wanted was the old migration queue, which #67 removed for
// good reason: that queue bypassed the scheduler entirely, so anything on it
// was out of reach of preemption, aging and stealing. The right mechanism is an
// affinity MASK, which is what this module is, so the syscall is revived here
// rather than deleted.
//
// ONE-SHOT AND OWNED. The request records the REQUESTER's pid, so a stale
// request cannot be consumed by a spawn from an unrelated process, which is the
// obvious failure of a bare global one-shot flag.

static SPAWN_REQ_PID: AtomicU32 = AtomicU32::new(EMPTY);
static SPAWN_REQ_MASK: AtomicU64 = AtomicU64::new(0);

/// Ask that the next user process spawned BY `requester` be given `mask`.
/// Returns 0, or -1 for a bad pid or a zero mask.
#[no_mangle]
pub extern "C" fn affinity_next_spawn_set_rs(requester: u32, mask: u64) -> i32 {
    if requester == EMPTY || mask == 0 {
        return -1;
    }
    SPAWN_REQ_MASK.store(mask, Ordering::Relaxed);
    SPAWN_REQ_PID.store(requester, Ordering::Release);
    0
}

/// Consume the pending request if it belongs to `requester`. Returns the mask,
/// or 0 if there is none. Clearing on read is what makes it one-shot.
#[no_mangle]
pub extern "C" fn affinity_next_spawn_take_rs(requester: u32) -> u64 {
    if SPAWN_REQ_PID.load(Ordering::Acquire) != requester || requester == EMPTY {
        return 0;
    }
    SPAWN_REQ_PID.store(EMPTY, Ordering::Release);
    SPAWN_REQ_MASK.swap(0, Ordering::Relaxed)
}

/// Every core EXCEPT the boot processor, for `ncpu` cores. This is what
/// "run it on an application processor" means as an affinity mask.
///
/// Returns 0 when there is only one core: on a uniprocessor the request cannot
/// be honoured, and a caller must be told so rather than handed a mask of 0
/// that would be silently treated as "no constraint".
#[no_mangle]
pub extern "C" fn affinity_ap_mask_rs(ncpu: u32) -> u64 {
    if ncpu <= 1 {
        return 0;
    }
    let n = if ncpu > 64 { 64 } else { ncpu };
    let all: u64 = if n == 64 { u64::MAX } else { (1u64 << n) - 1 };
    all & !1u64
}

/// Slots reclaimed from processes that had gone AFF_STALE_EPOCHS switch-ins
/// without running. Expected to be small and non-zero on a long boot; a large
/// value means the table is under pressure and AFF_SLOTS should grow.
#[no_mangle]
pub extern "C" fn affinity_evicted_rs() -> u64 {
    EVICTED.load(Ordering::Relaxed)
}

/// Zero the migration counters without disturbing the masks, so a measurement
/// can be bracketed around one action instead of covering the whole boot.
#[no_mangle]
pub extern "C" fn affinity_reset_stats_rs() {
    let mut i = 0usize;
    while i < AFF_SLOTS {
        TABLE[i].migrations.store(0, Ordering::Relaxed);
        TABLE[i].switchins.store(0, Ordering::Relaxed);
        i += 1;
    }
    FULL.store(0, Ordering::Relaxed);
}

// ---------------------------------------------------------------------------
// Self-test.
// ---------------------------------------------------------------------------

/// Prove the table, the mask semantics and the migration rule, on pids chosen
/// high enough not to collide with anything the kernel has really created.
/// Returns 0 on success or a bitmask of failed checks.
#[no_mangle]
pub extern "C" fn affinity_selftest_rs() -> i32 {
    let mut fail: i32 = 0;
    // Pids well above anything a boot will have allocated, and cleared at the
    // end so the live table is left exactly as it was found.
    const A: u32 = 0x7000_0001;
    const B: u32 = 0x7000_0002;

    // SAVE THE BOOT'S A/B ARM AND FORCE THE GATE OFF FOR THE BODY OF THIS
    // SUITE, THEN PUT IT BACK EXACTLY AS FOUND.
    //
    // THIS IS A BUG THIS SUITE ACTUALLY CAUSED, not a hypothetical. The first
    // version ended with a bare `affinity_set_disabled_rs(0)` to "restore" the
    // gate, and main.c arms /NOAFF.TXT BEFORE the self-tests run. So on the
    // disabled arm the self-test silently RE-ENABLED affinity for the rest of
    // the boot, and a measurement campaign ran three boots of a control arm
    // that was not a control. It was caught only because the [AFFMIG] line
    // prints aff=ON/OFF and the harness verifies the arm from the kernel's own
    // output rather than from what was written to the ESP.
    //
    // Restoring to a CONSTANT is the defect. A test that mutates global state
    // must restore what it FOUND, not what it assumes was there. And forcing it
    // off here is equally necessary: with the gate armed, every
    // mask-must-exclude check below would legitimately fail and report a broken
    // module on a boot where nothing is broken.
    let prev_disabled = affinity_set_disabled_rs(0);

    // THE DEFAULT IS THE WHOLE POLICY: an unknown pid is unconstrained.
    if affinity_get_rs(A) != AFF_ALL {
        fail |= 1 << 0;
    }
    if affinity_allows_rs(A, 0) != 1 || affinity_allows_rs(A, 3) != 1 {
        fail |= 1 << 1;
    }

    // Set and read back.
    if affinity_set_rs(A, 0b0010) != 0 {
        fail |= 1 << 2;
    }
    if affinity_get_rs(A) != 0b0010 {
        fail |= 1 << 3;
    }
    if affinity_allows_rs(A, 1) != 1 {
        fail |= 1 << 4;
    }
    // The check that matters: a mask must actually EXCLUDE. A function that
    // returned 1 unconditionally would pass every check above this one.
    if affinity_allows_rs(A, 0) != 0 {
        fail |= 1 << 5;
    }
    if affinity_allows_rs(A, 2) != 0 {
        fail |= 1 << 6;
    }

    // Setting A must not have touched B.
    if affinity_get_rs(B) != AFF_ALL {
        fail |= 1 << 7;
    }

    // A zero mask is a request to hang the process and must be refused.
    if affinity_set_rs(A, 0) != -1 {
        fail |= 1 << 8;
    }
    if affinity_get_rs(A) != 0b0010 {
        fail |= 1 << 9; // the refused set must not have clobbered the old mask
    }

    // A core wider than the mask must be ALLOWED, not excluded.
    if affinity_allows_rs(A, 64) != 1 || affinity_allows_rs(A, 200) != 1 {
        fail |= 1 << 10;
    }

    // Migration accounting. First dispatch is not a migration.
    note_switchin(B, -1, 0);
    let mut m: u64 = 0;
    let mut sw: u64 = 0;
    if unsafe { affinity_stats_rs(B, &mut m, &mut sw, core::ptr::null_mut(), core::ptr::null_mut()) } != 0 {
        fail |= 1 << 11;
    }
    if m != 0 || sw != 1 {
        fail |= 1 << 12;
    }
    // Same core again: a switch-in, not a migration.
    note_switchin(B, 0, 0);
    unsafe { affinity_stats_rs(B, &mut m, &mut sw, core::ptr::null_mut(), core::ptr::null_mut()) };
    if m != 0 || sw != 2 {
        fail |= 1 << 13;
    }
    // Different core: a migration.
    note_switchin(B, 0, 1);
    unsafe { affinity_stats_rs(B, &mut m, &mut sw, core::ptr::null_mut(), core::ptr::null_mut()) };
    if m != 1 || sw != 3 {
        fail |= 1 << 14;
    }

    // Clear must remove the entry, not merely blank it: a stale entry would
    // eventually fill the probe run and start refusing live processes.
    // THE A/B GATE. Checked in both directions, and restored, because a gate
    // that could not be turned back off would silently disable affinity for the
    // rest of the boot after this self-test ran.
    if affinity_set_rs(A, 0b0010) != 0 {
        fail |= 1 << 25;
    }
    affinity_set_disabled_rs(1);
    if affinity_get_rs(A) != AFF_ALL {
        fail |= 1 << 26;   // gate must make the mask read as unconstrained
    }
    if affinity_allows_rs(A, 0) != 1 || affinity_allows_rs(A, 3) != 1 {
        fail |= 1 << 27;   // gate must allow every core
    }
    if affinity_disabled_rs() != 1 {
        fail |= 1 << 28;
    }
    affinity_set_disabled_rs(0);
    if affinity_get_rs(A) != 0b0010 {
        fail |= 1 << 29;   // the stored mask must SURVIVE the gate, not be lost
    }
    if affinity_allows_rs(A, 0) != 0 {
        fail |= 1 << 30;   // and exclusion must come back
    }
    if affinity_disabled_rs() != 0 {
        fail |= 1 << 31;
    }

    // The AP mask: every core but the BSP, and REFUSED on a uniprocessor rather
    // than returned as 0, which placement would read as "no constraint".
    // All three share ONE bit deliberately: they test a single small pure
    // function, so "affinity_ap_mask_rs is wrong" is the whole diagnosis and
    // three bits would buy nothing. The mask is i32 and every other bit is
    // spoken for; spending three on one function while a real check went
    // homeless would be the wrong trade.
    if affinity_ap_mask_rs(1) != 0
        || affinity_ap_mask_rs(4) != 0b1110
        || affinity_ap_mask_rs(2) != 0b10
    {
        fail |= 1 << 17;
    }

    // The one-shot spawn request is OWNED: an unrelated spawner must not
    // consume it, and a second take must return nothing.
    if affinity_next_spawn_set_rs(A, 0b1110) != 0 {
        fail |= 1 << 20;
    }
    if affinity_next_spawn_take_rs(B) != 0 {
        fail |= 1 << 22; // wrong owner took it
    }
    if affinity_next_spawn_take_rs(A) != 0b1110 {
        fail |= 1 << 18; // the owner's own take must return the mask
    }
    if affinity_next_spawn_take_rs(A) != 0 {
        fail |= 1 << 21; // not one-shot
    }
    if affinity_next_spawn_set_rs(A, 0) != -1 {
        fail |= 1 << 23;
    }

    affinity_clear_rs(A);
    affinity_clear_rs(B);

    // Put the boot's arm back EXACTLY as found, then assert that it is back.
    // The assert matters: a restore that silently failed would be invisible,
    // and invisible is precisely how the original version of this destroyed
    // three boots of a measurement.
    affinity_set_disabled_rs(prev_disabled);
    if affinity_disabled_rs() != prev_disabled {
        fail |= 1 << 24;
    }
    if affinity_get_rs(A) != AFF_ALL || affinity_get_rs(B) != AFF_ALL {
        fail |= 1 << 15;
    }
    if unsafe {
        affinity_stats_rs(B, core::ptr::null_mut(), core::ptr::null_mut(),
                          core::ptr::null_mut(), core::ptr::null_mut())
    } != -1
    {
        fail |= 1 << 16;
    }

    fail
}

/// NEGATIVE CONTROL: prove the self-test above is capable of failing.
///
/// It re-runs the two checks whose failure would be silent and most damaging (a
/// mask that does not exclude, and a first dispatch counted as a migration) in
/// the INVERTED sense, and returns 1 if the module answers the way a broken
/// implementation would. Returns 0 when the suite is discriminating.
///
/// This exists because this tree has shipped a truncation detector structurally
/// unable to fire and a panic-detecting harness that scored a boot with 65 panic
/// lines as a pass. A green self-test is worth nothing on its own.
#[no_mangle]
pub extern "C" fn affinity_selftest_negative_rs() -> i32 {
    const C: u32 = 0x7000_0003;
    let mut bad = 0;
    affinity_set_rs(C, 0b0001);
    // If EVERY core were allowed under a one-core mask, the mask does nothing
    // and every result built on it is meaningless.
    if affinity_allows_rs(C, 0) == affinity_allows_rs(C, 1) {
        bad = 1;
    }
    // If a first dispatch counted as a migration, every baseline would be
    // inflated by one per process and the before/after delta would be noise.
    note_switchin(C, -1, 2);
    let mut m: u64 = 0;
    unsafe {
        affinity_stats_rs(C, &mut m, core::ptr::null_mut(),
                          core::ptr::null_mut(), core::ptr::null_mut())
    };
    if m != 0 {
        bad = 1;
    }
    affinity_clear_rs(C);
    bad
}
