// rustkern/e2rdio.rs - the ext2 read path's device-I/O ledger, plus the
// deterministic fault injector that PROVES the read path reports an I/O failure
// instead of laundering it into end-of-file.  (#e2rdio, 2026-09-29)
//
// NEW kernel code, so Rust per the 2026-07-16 rule. There is no C twin to
// strangle: none of this existed. The C it sits next to (fs/ext2.c's device
// seam, proc/fdlayer.c's ext2 read branch) stays C because it is the existing
// hot path and is entangled with the fd table and the block layer; what is new
// here is pure state and pure policy, which is exactly the seam worth having in
// Rust.
//
// ===========================================================================
// THE DEFECT THIS EXISTS FOR, stated precisely, because the ticket that asked
// for it described it in a place where it is NOT true
// ---------------------------------------------------------------------------
// The brief said fs/ext2.c's readers "return the bytes copied so far, a silent
// short read with no error code". MEASURED BY READING THE CODE: they do not.
// Both ext2_read_file_ino_on() and ext2_read_file_range_on() initialise
// `rc = -5` and only assign `rc = copied` at the very end, so EVERY failing
// exit (`goto done` / `goto rdone`) returns -5 and never a partial count. ext2
// was already honest.
//
// The laundering happens ONE layer up, at proc/fdlayer.c's ext2 branch of
// sys_read(). It refilled a bounded window, and on a refill that returned <= 0
// it did:
//
//     if (got <= 0) { ...log once per fd...; break; }
//     ...
//     return (int64_t)produced;          // produced can be, and is, 0
//
// So a hard device error became `read() == 0`. userland/libc/stdio_file.c's
// refill() reads `r == 0` as end-of-file (correctly - that IS what read()==0
// means), sets f->eof, and the caller concludes the file is truncated. A
// transient disk fault is thereby converted into "this file is corrupt", with
// the ONLY trace anywhere being one serial line per fd. That is the defect:
// not the absence of a retry, and not anything in ext2.c.
//
// ===========================================================================
// WHAT THIS MODULE IS FOR
// ---------------------------------------------------------------------------
//   1. A LEDGER. Every device read that failed and was re-issued, every one
//      that then succeeded, every one that never did, every sparse hole served
//      as zeros, and every time the fdlayer reached the site that used to
//      launder an error. None of these were counted anywhere before, which is
//      why "did a transient disk fault actually occur" has never been
//      answerable on this OS. A counter is how an intermittent fault stops
//      being a story and becomes a number.
//
//   2. AN INJECTOR, so the error path can be PROVEN rather than reasoned about.
//      A fault that reproduces once in four launches cannot be a regression
//      test. A synthetic one that fires exactly when asked can.
//
// ===========================================================================
// DESIGN CONSTRAINTS, all forced by where this is called from
// ---------------------------------------------------------------------------
//   - THE PRODUCTION COST IS ONE RELAXED u32 LOAD. e2rdio_dev_take_rs() is on
//     the hottest read path in the filesystem, so it tests the mode word first
//     and returns; everything else is behind that branch. No lock, no
//     allocation, no print, ever.
//   - NEVER PRINTS. Same rule, and for the same reason, as rustkern/netfail.rs:
//     the failure sites here are inside the block read path, where
//     bootlog_write() would re-enter blk_write(). Recording is separate from
//     reporting; only a caller in a safe context reports.
//   - THE INJECTOR CANNOT BE REACHED FROM RING 3. No syscall exposes it. It is
//     armed only by the in-kernel boot self-test, which SEALS the module when
//     it is done: after e2rdio_seal_rs() no arm can ever succeed again for the
//     rest of the boot, so a stale arm cannot leak into a running desktop.
//   - THE INJECTOR IS KEYED BY OWNER PID, so an armed shot cannot be consumed
//     by an unrelated thread's read. It is not keyed by LBA, because the
//     self-test knows an inode and not a block address, and translating one to
//     the other in the test would duplicate the resolver being tested.
//
// HONEST LIMITS. The owner key is proc_current_pid(), which is 0 for every
// context with no current process, so a read issued from another pid-0 context
// inside the (single-read-wide) armed window could consume the shot. That is
// why the self-test asserts the EXACT injection count: a stolen shot makes the
// test FAIL loudly rather than pass wrongly.

use core::sync::atomic::{AtomicI64, AtomicU32, AtomicU64, Ordering};

// ---------------------------------------------------------------------------
// Injection modes. The NAME is the contract; these numbers appear nowhere
// outside this file and its two C call sites.
// ---------------------------------------------------------------------------
/// Not armed. The only state a shipped, sealed kernel is ever in.
pub const E2RD_INJ_OFF: u32 = 0;
/// The next `shots` device reads by the owner return -1, i.e. exactly what
/// blk_read()/blkmgr_dev_rw_c() return when the transfer failed.
pub const E2RD_INJ_DEV_FAIL: u32 = 1;
/// The next `shots` device reads by the owner return `count - 1` sectors: a
/// SHORT transfer rather than a reported failure, which is the other half of
/// what `!= (int)(run * spb)` is there to catch.
pub const E2RD_INJ_DEV_SHORT: u32 = 2;
/// The next `shots` block resolutions of inode `ino` by the owner are treated
/// as a sparse hole (phys == 0). Proves the sparse path still returns SUCCESS
/// and zeros, i.e. that the error work did NOT turn holes into failures.
pub const E2RD_INJ_HOLE: u32 = 3;

static INJ_MODE: AtomicU32 = AtomicU32::new(E2RD_INJ_OFF);
static INJ_SHOTS: AtomicU32 = AtomicU32::new(0);
static INJ_OWNER: AtomicU32 = AtomicU32::new(0);
static INJ_INO: AtomicU32 = AtomicU32::new(0);
static SEALED: AtomicU32 = AtomicU32::new(0);

static N_RETRY: AtomicU64 = AtomicU64::new(0);
static N_RECOVERED: AtomicU64 = AtomicU64::new(0);
static N_EXHAUSTED: AtomicU64 = AtomicU64::new(0);
static N_HOLE: AtomicU64 = AtomicU64::new(0);
static N_LAUNDER: AtomicU64 = AtomicU64::new(0);
static N_INJECTED: AtomicU64 = AtomicU64::new(0);

static LAST_PRODUCED: AtomicU32 = AtomicU32::new(0);
static LAST_INO: AtomicU32 = AtomicU32::new(0);
static LAST_GOT: AtomicI64 = AtomicI64::new(0);
static PENDING: AtomicU32 = AtomicU32::new(0);

/// The ledger, shared with C. `#[repr(C)]` plus a `_Static_assert` on the C
/// side (fs/ext2.c) locks the layout, per this codebase's FFI convention.
#[repr(C)]
pub struct E2RdioStats {
    /// Device reads that came back wrong and were re-issued.
    pub retries: u64,
    /// ... of which a later attempt succeeded. A NON-ZERO value here is the
    /// evidence that transient device faults are real on this machine.
    pub recovered: u64,
    /// ... of which no attempt ever succeeded, so a real error was returned.
    pub exhausted: u64,
    /// Sparse (phys == 0) blocks served as zeros. Correct data, and counted
    /// only so that a "this file is corrupt" report can see them: a hole and a
    /// lost block are indistinguishable on disk, so the number is the only
    /// thing that can ever tell a reader which one it was looking at.
    pub holes: u64,
    /// Times sys_read() reached the site that used to convert a refill error
    /// into `read() == 0`.
    pub launder: u64,
    /// Synthetic failures produced by the injector. MUST be 0 on any machine
    /// past its boot self-test.
    pub injected: u64,
    /// The `produced` value at the most recent launder site: what the PRE-FIX
    /// code would have returned to userland. 0 means it would have said EOF.
    pub last_produced: u32,
    /// The inode that read was against.
    pub last_ino: u32,
    /// The refill return value at that site (-1 unmounted, -2 inode, -3 kmalloc,
    /// -4 indirect-block read, -5 data read, 0 at/after EOF).
    pub last_got: i64,
}

// ---------------------------------------------------------------------------
// Arming. Only the in-kernel self-test calls these.
// ---------------------------------------------------------------------------

/// Arm the injector. Returns 0 on success, -1 if the module is SEALED (the
/// permanent state after the boot self-test), -2 on a bad mode.
///
/// Ordering: mode is published LAST, with Release, so a taker that sees a
/// non-zero mode is guaranteed to see the shots/owner/ino that go with it.
#[no_mangle]
pub extern "C" fn e2rdio_arm_rs(mode: u32, shots: u32, ino: u32, owner: u32) -> i32 {
    if SEALED.load(Ordering::Acquire) != 0 {
        return -1;
    }
    if mode == E2RD_INJ_OFF || mode > E2RD_INJ_HOLE {
        return -2;
    }
    INJ_SHOTS.store(shots, Ordering::Relaxed);
    INJ_OWNER.store(owner, Ordering::Relaxed);
    INJ_INO.store(ino, Ordering::Relaxed);
    INJ_MODE.store(mode, Ordering::Release);
    0
}

/// Disarm. Always succeeds, sealed or not: disarming can only ever be safe.
#[no_mangle]
pub extern "C" fn e2rdio_disarm_rs() {
    INJ_MODE.store(E2RD_INJ_OFF, Ordering::Release);
    INJ_SHOTS.store(0, Ordering::Relaxed);
}

/// Refuse every future arm, permanently, and disarm now. Called once, by the
/// boot self-test, as its last act. There is deliberately no unseal.
#[no_mangle]
pub extern "C" fn e2rdio_seal_rs() {
    e2rdio_disarm_rs();
    SEALED.store(1, Ordering::Release);
}

/// 1 if sealed. For the self-test's own assertion that sealing took.
#[no_mangle]
pub extern "C" fn e2rdio_sealed_rs() -> i32 {
    (SEALED.load(Ordering::Acquire) != 0) as i32
}

extern "C" {
    /// proc/process.c. Returns 0 when there is no current process, which is the
    /// case for the boot thread that runs the self-test.
    fn proc_current_pid() -> u32;
}

// ---------------------------------------------------------------------------
// The hot path. ONE relaxed load when not armed, which is always, in a shipped
// kernel past its boot self-test.
//
// The pid lookup is deliberately BEHIND the mode test and lives here rather
// than at the C call site, so an unarmed kernel pays for the load and nothing
// else on the hottest read path in the filesystem. The owner-explicit cores
// below are what the self-test drives, because at boot proc_current_pid() is 0
// for every context and a one-owner test could not tell keying from luck.
// ---------------------------------------------------------------------------

fn dev_take(owner: u32, count: u32, out_rc: *mut i32) -> i32 {
    // THE WHOLE PRODUCTION COST OF THIS MODULE IS THIS LOAD.
    let mode = INJ_MODE.load(Ordering::Relaxed);
    if mode != E2RD_INJ_DEV_FAIL && mode != E2RD_INJ_DEV_SHORT {
        return 0;
    }
    if INJ_OWNER.load(Ordering::Relaxed) != owner {
        return 0;
    }
    // Take a shot atomically so two CPUs cannot both consume the same one.
    let mut cur = INJ_SHOTS.load(Ordering::Relaxed);
    loop {
        if cur == 0 {
            return 0;
        }
        match INJ_SHOTS.compare_exchange_weak(
            cur,
            cur - 1,
            Ordering::AcqRel,
            Ordering::Relaxed,
        ) {
            Ok(_) => break,
            Err(seen) => cur = seen,
        }
    }
    let rc: i32 = if mode == E2RD_INJ_DEV_FAIL {
        -1
    } else {
        // A short transfer. Saturating at 0 keeps it != count for count >= 1,
        // which is the only property the caller's comparison needs.
        (count as i32).saturating_sub(1)
    };
    if !out_rc.is_null() {
        unsafe {
            *out_rc = rc;
        }
    }
    N_INJECTED.fetch_add(1, Ordering::Relaxed);
    PENDING.store(1, Ordering::Relaxed);
    1
}

/// Consume one device-read injection if one is armed for the calling context.
///
/// Returns 1 and writes the value the caller must PRETEND the device returned
/// into `*out_rc`; returns 0 to mean "issue the real read". The out-parameter
/// exists because a legitimate injected value can be 0 (a short transfer of a
/// 1-sector read), so a single return value could not carry both answers.
#[no_mangle]
pub extern "C" fn e2rdio_dev_take_rs(count: u32, out_rc: *mut i32) -> i32 {
    if INJ_MODE.load(Ordering::Relaxed) == E2RD_INJ_OFF {
        return 0;
    }
    dev_take(unsafe { proc_current_pid() }, count, out_rc)
}

fn hole_take(owner: u32, ino: u32) -> i32 {
    if INJ_MODE.load(Ordering::Relaxed) != E2RD_INJ_HOLE {
        return 0;
    }
    if INJ_OWNER.load(Ordering::Relaxed) != owner {
        return 0;
    }
    let want = INJ_INO.load(Ordering::Relaxed);
    if want != 0 && want != ino {
        return 0;
    }
    let mut cur = INJ_SHOTS.load(Ordering::Relaxed);
    loop {
        if cur == 0 {
            return 0;
        }
        match INJ_SHOTS.compare_exchange_weak(
            cur,
            cur - 1,
            Ordering::AcqRel,
            Ordering::Relaxed,
        ) {
            Ok(_) => break,
            Err(seen) => cur = seen,
        }
    }
    N_INJECTED.fetch_add(1, Ordering::Relaxed);
    1
}

/// Consume one sparse-hole injection if one is armed for the calling context
/// and `ino`. Returns 1 to mean "treat this resolved block as a hole".
#[no_mangle]
pub extern "C" fn e2rdio_hole_take_rs(ino: u32) -> i32 {
    if INJ_MODE.load(Ordering::Relaxed) != E2RD_INJ_HOLE {
        return 0;
    }
    hole_take(unsafe { proc_current_pid() }, ino)
}

// ---------------------------------------------------------------------------
// The ledger. All relaxed: these are counters a human reads, not values another
// CPU's control flow depends on.
// ---------------------------------------------------------------------------

/// A device read came back wrong and is about to be re-issued.
#[no_mangle]
pub extern "C" fn e2rdio_note_retry_rs() {
    N_RETRY.fetch_add(1, Ordering::Relaxed);
    PENDING.store(1, Ordering::Relaxed);
}

/// A re-issued device read succeeded. `attempts` is how many it took in total.
#[no_mangle]
pub extern "C" fn e2rdio_note_recovered_rs(_attempts: u32) {
    N_RECOVERED.fetch_add(1, Ordering::Relaxed);
    PENDING.store(1, Ordering::Relaxed);
}

/// Every attempt failed; a real error is being returned upwards.
#[no_mangle]
pub extern "C" fn e2rdio_note_exhausted_rs(_rc: i32) {
    N_EXHAUSTED.fetch_add(1, Ordering::Relaxed);
    PENDING.store(1, Ordering::Relaxed);
}

/// A sparse hole was served as zeros. Correct data; counted, not complained
/// about.
#[no_mangle]
pub extern "C" fn e2rdio_note_hole_rs() {
    N_HOLE.fetch_add(1, Ordering::Relaxed);
}

/// sys_read() reached the site that used to return `produced` (which is 0 here
/// whenever the very first refill failed) in place of an error. Records what the
/// pre-fix code WOULD have returned, which is how the boot self-test can assert
/// the old behaviour was wrong without re-introducing it.
#[no_mangle]
pub extern "C" fn e2rdio_note_launder_rs(produced: u32, got: i64, ino: u32) {
    LAST_PRODUCED.store(produced, Ordering::Relaxed);
    LAST_GOT.store(got, Ordering::Relaxed);
    LAST_INO.store(ino, Ordering::Relaxed);
    N_LAUNDER.fetch_add(1, Ordering::Release);
    PENDING.store(1, Ordering::Relaxed);
}

/// 1 if anything worth printing has happened since the last report. Lets a
/// caller in a SAFE context (sys_read, the boot self-test) do the printing that
/// the recording sites must never do.
#[no_mangle]
pub extern "C" fn e2rdio_pending_rs() -> i32 {
    // Plain load first so the common answer (nothing happened) costs a load and
    // not a locked exchange: this is called once per ext2 read() syscall.
    if PENDING.load(Ordering::Relaxed) == 0 {
        return 0;
    }
    (PENDING.swap(0, Ordering::Relaxed) != 0) as i32
}

/// Snapshot the ledger. Not a consistent snapshot across fields and does not
/// need to be: every field is independently monotonic.
#[no_mangle]
pub extern "C" fn e2rdio_stats_rs(out: *mut E2RdioStats) {
    if out.is_null() {
        return;
    }
    unsafe {
        (*out).retries = N_RETRY.load(Ordering::Relaxed);
        (*out).recovered = N_RECOVERED.load(Ordering::Relaxed);
        (*out).exhausted = N_EXHAUSTED.load(Ordering::Relaxed);
        (*out).holes = N_HOLE.load(Ordering::Relaxed);
        (*out).launder = N_LAUNDER.load(Ordering::Acquire);
        (*out).injected = N_INJECTED.load(Ordering::Relaxed);
        (*out).last_produced = LAST_PRODUCED.load(Ordering::Relaxed);
        (*out).last_ino = LAST_INO.load(Ordering::Relaxed);
        (*out).last_got = LAST_GOT.load(Ordering::Relaxed);
    }
}

// ---------------------------------------------------------------------------
// Self-test of the STATE MACHINE only. The end-to-end proof (that a read
// through a real fd on the real volume reports EIO rather than EOF) is the
// ext2/readfault group in fs/ext2.c; this half asserts the things that can be
// asserted without a disk, so a failure there cannot be blamed on the harness.
//
// Deliberately NOT a [RUST-DIFF] differential: there is no C twin, and #433's
// lesson is that a differential whose two arms share an assumption scores a
// meaningless pass. These are absolute assertions.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn e2rdio_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks: u32 = 0;
    let mut bad: i32 = 0;
    let mut rc: i32 = 0;

    // Saving and restoring the live state means this can run at any point in
    // boot without disturbing a ledger that is already accumulating.
    let s_mode = INJ_MODE.load(Ordering::Relaxed);
    let s_shots = INJ_SHOTS.load(Ordering::Relaxed);
    let s_owner = INJ_OWNER.load(Ordering::Relaxed);
    let s_ino = INJ_INO.load(Ordering::Relaxed);
    let s_inj = N_INJECTED.load(Ordering::Relaxed);
    let s_seal = SEALED.load(Ordering::Relaxed);
    SEALED.store(0, Ordering::Relaxed);

    let ck = |ok: bool, checks: &mut u32, bad: &mut i32| {
        *checks += 1;
        if !ok {
            *bad += 1;
        }
    };

    // Not armed: the hot path must be a no-op for any owner.
    e2rdio_disarm_rs();
    ck(dev_take(0, 8, &mut rc) == 0, &mut checks, &mut bad);
    ck(dev_take(7, 8, &mut rc) == 0, &mut checks, &mut bad);
    ck(hole_take(7, 12) == 0, &mut checks, &mut bad);

    // A bad mode is refused, and refusing it must not arm anything.
    ck(e2rdio_arm_rs(E2RD_INJ_OFF, 1, 0, 7) == -2, &mut checks, &mut bad);
    ck(e2rdio_arm_rs(99, 1, 0, 7) == -2, &mut checks, &mut bad);
    ck(dev_take(7, 8, &mut rc) == 0, &mut checks, &mut bad);

    // DEV_FAIL, one shot, owner 7: fires once for 7 and never for 8.
    ck(e2rdio_arm_rs(E2RD_INJ_DEV_FAIL, 1, 0, 7) == 0, &mut checks, &mut bad);
    ck(dev_take(8, 8, &mut rc) == 0, &mut checks, &mut bad);
    rc = 12345;
    ck(dev_take(7, 8, &mut rc) == 1, &mut checks, &mut bad);
    ck(rc == -1, &mut checks, &mut bad);
    ck(dev_take(7, 8, &mut rc) == 0, &mut checks, &mut bad);

    // DEV_SHORT returns count-1, which is what the caller's `!= count` needs,
    // including for count == 1 where the short value is 0.
    ck(e2rdio_arm_rs(E2RD_INJ_DEV_SHORT, 2, 0, 7) == 0, &mut checks, &mut bad);
    ck(dev_take(7, 8, &mut rc) == 1, &mut checks, &mut bad);
    ck(rc == 7, &mut checks, &mut bad);
    ck(dev_take(7, 1, &mut rc) == 1, &mut checks, &mut bad);
    ck(rc == 0, &mut checks, &mut bad);
    ck(dev_take(7, 8, &mut rc) == 0, &mut checks, &mut bad);

    // HOLE is inode-keyed, and a DEV mode must never satisfy a hole take (or
    // the sparse arm of the boot test would be testing the wrong thing).
    ck(e2rdio_arm_rs(E2RD_INJ_HOLE, 1, 42, 7) == 0, &mut checks, &mut bad);
    ck(hole_take(7, 41) == 0, &mut checks, &mut bad);
    ck(dev_take(7, 8, &mut rc) == 0, &mut checks, &mut bad);
    ck(hole_take(7, 42) == 1, &mut checks, &mut bad);
    ck(hole_take(7, 42) == 0, &mut checks, &mut bad);

    // ino == 0 means any inode.
    ck(e2rdio_arm_rs(E2RD_INJ_HOLE, 1, 0, 7) == 0, &mut checks, &mut bad);
    ck(hole_take(7, 999) == 1, &mut checks, &mut bad);

    // Exactly five injections were consumed above: 1 (DEV_FAIL) + 2 (DEV_SHORT)
    // + 1 (HOLE ino-keyed) + 1 (HOLE any-inode). Counted
    // explicitly rather than assumed, because a miscount here would silently
    // weaken the exact-count assertion the boot test relies on.
    ck(
        N_INJECTED.load(Ordering::Relaxed) == s_inj + 5,
        &mut checks,
        &mut bad,
    );

    // A NULL out-pointer must not fault.
    ck(e2rdio_arm_rs(E2RD_INJ_DEV_FAIL, 1, 0, 7) == 0, &mut checks, &mut bad);
    ck(
        dev_take(7, 8, core::ptr::null_mut()) == 1,
        &mut checks,
        &mut bad,
    );
    e2rdio_stats_rs(core::ptr::null_mut());

    // The two SHIPPING entry points, the ones the C seams actually call, taking
    // their owner from proc_current_pid(). Armed for whatever this context's pid
    // is, so this holds wherever the self-test is called from.
    let me = unsafe { proc_current_pid() };
    ck(e2rdio_arm_rs(E2RD_INJ_DEV_FAIL, 1, 0, me) == 0, &mut checks, &mut bad);
    rc = 0;
    ck(e2rdio_dev_take_rs(8, &mut rc) == 1, &mut checks, &mut bad);
    ck(rc == -1, &mut checks, &mut bad);
    ck(e2rdio_dev_take_rs(8, &mut rc) == 0, &mut checks, &mut bad);
    ck(e2rdio_arm_rs(E2RD_INJ_HOLE, 1, 77, me) == 0, &mut checks, &mut bad);
    ck(e2rdio_hole_take_rs(76) == 0, &mut checks, &mut bad);
    ck(e2rdio_hole_take_rs(77) == 1, &mut checks, &mut bad);
    ck(e2rdio_hole_take_rs(77) == 0, &mut checks, &mut bad);

    // Sealing refuses every later arm, and a sealed module cannot inject.
    e2rdio_seal_rs();
    ck(e2rdio_sealed_rs() == 1, &mut checks, &mut bad);
    ck(e2rdio_arm_rs(E2RD_INJ_DEV_FAIL, 1, 0, 7) == -1, &mut checks, &mut bad);
    ck(dev_take(7, 8, &mut rc) == 0, &mut checks, &mut bad);

    // Restore. The injected counter is left where the test put it, on purpose:
    // it is the honest record of what ran, and the boot group reads DELTAS.
    SEALED.store(s_seal, Ordering::Relaxed);
    INJ_SHOTS.store(s_shots, Ordering::Relaxed);
    INJ_OWNER.store(s_owner, Ordering::Relaxed);
    INJ_INO.store(s_ino, Ordering::Relaxed);
    INJ_MODE.store(s_mode, Ordering::Release);

    if !out_checks.is_null() {
        unsafe {
            *out_checks = checks;
        }
    }
    bad
}
