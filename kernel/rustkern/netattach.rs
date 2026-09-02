// rustkern/netattach.rs - hot-plug NIC attach handoff.
//
// THE BUG THIS EXISTS FOR
// ==========================================================================
// A USB Ethernet adapter plugged in AFTER boot enumerated correctly, the CDC-ECM
// driver claimed it and read its MAC, and then nothing happened: no DISCOVER, no
// OFFER, no lease, ever. net_init() binds a NIC exactly once during boot; the
// xHCI re-scan path that enumerates a hot-plugged device has no route back into
// it, so a NIC that appears later is attached at the driver layer and invisible
// to the network stack.
//
// WHY THIS IS RUST
// ==========================================================================
// New kernel logic per the 2026-07-16 rule. No C twin, no -DRUST_* strangler
// flag, no RUST_PORT_LEDGER row: there is nothing to differ from, so the
// rollback is reverting the commit.
//
// It is also the right shape for the language. This is a one-shot handoff
// between TWO THREADS that must not share a lock:
//
//   * the ARMING side runs on the xHCI re-scan worker, inside device
//     enumeration. It must be a store and nothing else. #549 is the recorded
//     cost of blocking in a context that cannot block, and the enumeration path
//     is one control transfer away from every other USB device on the bus.
//
//   * the CONSUMING side runs on the background net worker (net/net.c
//     net_worker), which is the single owner of DHCP acquisition and carrier
//     polling, is a normal PRIO_NORMAL thread, and is allowed to be slow.
//
// A plain `int` flag written by one thread and cleared by the other has a real
// lost-update window: two adapters plugged in quick succession, or an arm that
// lands between the worker's read and its clear, silently drops an attach. The
// consume is therefore a compare_exchange, which is the whole reason the take
// can be trusted to fire exactly once per arm.
//
// WHAT IT DELIBERATELY DOES NOT DO
// ==========================================================================
// It does not own, and does not duplicate, "is a NIC bound". net/net.c owns
// that (active_driver / net_initialized) and PASSES IT IN. Duplicated state is
// how the idempotence guarantee would rot: two copies of one fact drift, and
// the drift is only visible on the day someone hot-plugs a second adapter.
// Every decision here is a function of facts the caller supplies plus the
// module's own arm/take counters.
//
// It also does not start DHCP, bind a driver, or touch the wire. It answers one
// question ("should the net worker look at the NIC layer again?") and counts
// the answers so the artifact can show what happened.

#![allow(dead_code)]

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

// The one-shot handoff. 0 = nothing pending, 1 = an attach is waiting for the
// net worker. Armed from the xHCI re-scan thread, consumed by net_worker.
static PENDING: AtomicU32 = AtomicU32::new(0);

// The same handoff in the other direction: the NIC was UNPLUGGED. Armed from
// the xHCI slot-teardown path, consumed by net_worker, which unbinds the
// driver so that a REPLUG is seen as a fresh attach. Without it a replug is
// silently ignored: usb_net_probe() opens with `if (g_usbnet.active) return 0`
// and net_has_nic() still answers 1, so the new device is claimed by nobody.
static DETACH_PENDING: AtomicU32 = AtomicU32::new(0);

// Set once net_init() has RUN (whether it found a NIC or not). Until then every
// probe is a BOOT-TIME probe: USB enumeration runs before net_init(), so
// arming there would hand the net worker an attach it is about to perform for
// itself, and the worker does not exist yet either.
static BOOT_DONE: AtomicU32 = AtomicU32::new(0);

// Counters. These are the durable evidence: "armed 1, taken 1" and "armed 1,
// taken 0" are completely different failures and a boolean cannot tell them
// apart.
static N_PROBE: AtomicU64 = AtomicU64::new(0);      // probe notifications seen
static N_ARMED: AtomicU64 = AtomicU64::new(0);      // arms that actually latched
static N_TAKEN: AtomicU64 = AtomicU64::new(0);      // consumes by the net worker
static N_SKIP_BOOT: AtomicU64 = AtomicU64::new(0);  // ignored: still in boot
static N_SKIP_BOUND: AtomicU64 = AtomicU64::new(0); // ignored: a NIC is already bound
static N_COALESCED: AtomicU64 = AtomicU64::new(0);  // arm on an already-armed flag
static N_DETACH: AtomicU64 = AtomicU64::new(0);     // unplug notifications
static N_DETACH_TAKEN: AtomicU64 = AtomicU64::new(0); // unbinds by the net worker

/// net_init() has finished (success OR failure). Called from net/net.c at every
/// return point, so a boot with no NIC at all still flips it.
#[no_mangle]
pub extern "C" fn netattach_boot_done_rs() {
    BOOT_DONE.store(1, Ordering::SeqCst);
}

#[no_mangle]
pub extern "C" fn netattach_boot_is_done_rs() -> i32 {
    BOOT_DONE.load(Ordering::SeqCst) as i32
}

/// A USB network driver just claimed a device.
///
/// `nic_bound` is net/net.c's own answer to "do I already have an active NIC",
/// passed in rather than cached here.
///
/// Returns 1 if this arm latched a fresh pending attach (the caller may log it),
/// 0 if it was ignored or coalesced into an arm already outstanding.
///
/// SAFE IN ANY CONTEXT: three atomic ops, no allocation, no lock, no wait.
#[no_mangle]
pub extern "C" fn netattach_on_probe_rs(nic_bound: i32) -> i32 {
    N_PROBE.fetch_add(1, Ordering::SeqCst);

    if BOOT_DONE.load(Ordering::SeqCst) == 0 {
        // Boot-time enumeration. net_init() has not run; it will find this
        // device by itself through usb_eth_present().
        N_SKIP_BOOT.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    if nic_bound != 0 {
        // IDEMPOTENCE, the second-adapter case. A NIC is already carrying the
        // stack and may hold a lease. Attaching another one must not start a
        // second DHCP client or rebind the stack under the live one.
        N_SKIP_BOUND.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    match PENDING.compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => {
            N_ARMED.fetch_add(1, Ordering::SeqCst);
            1
        }
        Err(_) => {
            // Already armed and not yet consumed. One bind services both.
            N_COALESCED.fetch_add(1, Ordering::SeqCst);
            0
        }
    }
}

/// Consume a pending attach. Returns 1 AT MOST ONCE per arm; the net worker
/// calls this every iteration and does the bind only when it gets a 1.
#[no_mangle]
pub extern "C" fn netattach_take_rs() -> i32 {
    match PENDING.compare_exchange(1, 0, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => {
            N_TAKEN.fetch_add(1, Ordering::SeqCst);
            1
        }
        Err(_) => 0,
    }
}

/// Re-arm after a bind attempt that failed, so the next worker pass retries
/// instead of losing the device until the user replugs it. Counted as an arm.
#[no_mangle]
pub extern "C" fn netattach_rearm_rs() {
    if PENDING.compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst).is_ok() {
        N_ARMED.fetch_add(1, Ordering::SeqCst);
    } else {
        N_COALESCED.fetch_add(1, Ordering::SeqCst);
    }
}

/// The bound USB NIC was UNPLUGGED. Called from the xHCI slot teardown, which
/// runs on the same re-scan worker as the probe: a store, nothing else.
#[no_mangle]
pub extern "C" fn netattach_on_detach_rs() {
    N_DETACH.fetch_add(1, Ordering::SeqCst);
    DETACH_PENDING.store(1, Ordering::SeqCst);
    // A pending ATTACH for a device that has just been unplugged is stale.
    // Dropping it here means a plug-then-immediately-unplug cannot leave the
    // worker binding a NIC that is no longer there.
    PENDING.store(0, Ordering::SeqCst);
}

/// Consume a pending detach. Returns 1 at most once per unplug.
#[no_mangle]
pub extern "C" fn netattach_take_detach_rs() -> i32 {
    match DETACH_PENDING.compare_exchange(1, 0, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => {
            N_DETACH_TAKEN.fetch_add(1, Ordering::SeqCst);
            1
        }
        Err(_) => 0,
    }
}

/// Counters, for the [NETATTACH] diagnostic line and net_diag_line().
/// Any pointer may be NULL.
#[no_mangle]
pub extern "C" fn netattach_stats_rs(probe: *mut u64, armed: *mut u64, taken: *mut u64,
                                     skip_boot: *mut u64, skip_bound: *mut u64,
                                     coalesced: *mut u64, pending: *mut u64,
                                     detach: *mut u64, detach_taken: *mut u64) {
    unsafe {
        if !probe.is_null()      { *probe      = N_PROBE.load(Ordering::SeqCst); }
        if !armed.is_null()      { *armed      = N_ARMED.load(Ordering::SeqCst); }
        if !taken.is_null()      { *taken      = N_TAKEN.load(Ordering::SeqCst); }
        if !skip_boot.is_null()  { *skip_boot  = N_SKIP_BOOT.load(Ordering::SeqCst); }
        if !skip_bound.is_null() { *skip_bound = N_SKIP_BOUND.load(Ordering::SeqCst); }
        if !coalesced.is_null()  { *coalesced  = N_COALESCED.load(Ordering::SeqCst); }
        if !pending.is_null()    { *pending    = PENDING.load(Ordering::SeqCst) as u64; }
        if !detach.is_null()       { *detach       = N_DETACH.load(Ordering::SeqCst); }
        if !detach_taken.is_null() { *detach_taken = N_DETACH_TAKEN.load(Ordering::SeqCst); }
    }
}

// ==========================================================================
// Self-test.
//
// It proves the four properties the fix rests on, destructively, against the
// real statics, then restores them. Not a compile check: each case asserts the
// take count, which is the thing that decides whether DHCP runs.
// ==========================================================================

struct Saved {
    pending: u32, detach_pending: u32, boot: u32,
    detach: u64, detach_taken: u64,
    probe: u64, armed: u64, taken: u64,
    skip_boot: u64, skip_bound: u64, coalesced: u64,
}

fn save() -> Saved {
    Saved {
        pending: PENDING.load(Ordering::SeqCst),
        detach_pending: DETACH_PENDING.load(Ordering::SeqCst),
        boot: BOOT_DONE.load(Ordering::SeqCst),
        detach: N_DETACH.load(Ordering::SeqCst),
        detach_taken: N_DETACH_TAKEN.load(Ordering::SeqCst),
        probe: N_PROBE.load(Ordering::SeqCst),
        armed: N_ARMED.load(Ordering::SeqCst),
        taken: N_TAKEN.load(Ordering::SeqCst),
        skip_boot: N_SKIP_BOOT.load(Ordering::SeqCst),
        skip_bound: N_SKIP_BOUND.load(Ordering::SeqCst),
        coalesced: N_COALESCED.load(Ordering::SeqCst),
    }
}

fn restore(s: &Saved) {
    PENDING.store(s.pending, Ordering::SeqCst);
    DETACH_PENDING.store(s.detach_pending, Ordering::SeqCst);
    BOOT_DONE.store(s.boot, Ordering::SeqCst);
    N_DETACH.store(s.detach, Ordering::SeqCst);
    N_DETACH_TAKEN.store(s.detach_taken, Ordering::SeqCst);
    N_PROBE.store(s.probe, Ordering::SeqCst);
    N_ARMED.store(s.armed, Ordering::SeqCst);
    N_TAKEN.store(s.taken, Ordering::SeqCst);
    N_SKIP_BOOT.store(s.skip_boot, Ordering::SeqCst);
    N_SKIP_BOUND.store(s.skip_bound, Ordering::SeqCst);
    N_COALESCED.store(s.coalesced, Ordering::SeqCst);
}

/// Returns the number of FAILED cases (0 = pass).
#[no_mangle]
pub extern "C" fn netattach_selftest_rs() -> i32 {
    let saved = save();
    let mut fails = 0i32;

    // 1. A probe during boot arms nothing, and the worker gets nothing.
    PENDING.store(0, Ordering::SeqCst);
    BOOT_DONE.store(0, Ordering::SeqCst);
    if netattach_on_probe_rs(0) != 0 { fails += 1; }
    if netattach_take_rs() != 0 { fails += 1; }

    // 2. A hot-plug probe with no NIC bound arms exactly one attach, and the
    //    worker consumes it exactly once.
    PENDING.store(0, Ordering::SeqCst);
    BOOT_DONE.store(1, Ordering::SeqCst);
    if netattach_on_probe_rs(0) != 1 { fails += 1; }
    if netattach_take_rs() != 1 { fails += 1; }
    if netattach_take_rs() != 0 { fails += 1; }   // one arm, ONE take

    // 3. IDEMPOTENCE, second adapter: a probe while a NIC is already bound
    //    arms nothing at all, so nothing rebinds and no second client starts.
    PENDING.store(0, Ordering::SeqCst);
    if netattach_on_probe_rs(1) != 0 { fails += 1; }
    if netattach_take_rs() != 0 { fails += 1; }

    // 4. COALESCING: several probes before the worker runs still produce one
    //    bind, not one per probe.
    PENDING.store(0, Ordering::SeqCst);
    if netattach_on_probe_rs(0) != 1 { fails += 1; }
    if netattach_on_probe_rs(0) != 0 { fails += 1; }
    if netattach_on_probe_rs(0) != 0 { fails += 1; }
    if netattach_take_rs() != 1 { fails += 1; }
    if netattach_take_rs() != 0 { fails += 1; }

    // 5. UNPLUG/REPLUG: a fresh probe after a completed take arms again, so the
    //    second insertion is not swallowed by the first.
    PENDING.store(0, Ordering::SeqCst);
    if netattach_on_probe_rs(0) != 1 { fails += 1; }
    if netattach_take_rs() != 1 { fails += 1; }
    if netattach_on_probe_rs(0) != 1 { fails += 1; }
    if netattach_take_rs() != 1 { fails += 1; }

    // 6. A failed bind re-arms, so the device is retried rather than lost.
    PENDING.store(0, Ordering::SeqCst);
    netattach_rearm_rs();
    if netattach_take_rs() != 1 { fails += 1; }

    // 7. UNPLUG is a one-shot too, and only the worker consumes it.
    PENDING.store(0, Ordering::SeqCst);
    DETACH_PENDING.store(0, Ordering::SeqCst);
    if netattach_take_detach_rs() != 0 { fails += 1; }
    netattach_on_detach_rs();
    if netattach_take_detach_rs() != 1 { fails += 1; }
    if netattach_take_detach_rs() != 0 { fails += 1; }

    // 8. THE FULL REPLUG CYCLE, which is what the owner will actually do.
    //    plug (bound=0) -> bind -> unplug -> unbind -> plug again -> bind again.
    //    The second plug must arm, and it only can because the unplug cleared
    //    the caller's "a NIC is bound" answer.
    PENDING.store(0, Ordering::SeqCst);
    DETACH_PENDING.store(0, Ordering::SeqCst);
    if netattach_on_probe_rs(0) != 1 { fails += 1; }   // plug
    if netattach_take_rs() != 1 { fails += 1; }        // worker binds
    if netattach_on_probe_rs(1) != 0 { fails += 1; }   // spurious re-probe: refused
    netattach_on_detach_rs();                          // unplug
    if netattach_take_detach_rs() != 1 { fails += 1; } // worker unbinds
    if netattach_on_probe_rs(0) != 1 { fails += 1; }   // replug arms again
    if netattach_take_rs() != 1 { fails += 1; }

    // 9. A plug IMMEDIATELY followed by an unplug must not leave the worker
    //    binding a device that has gone. The stale arm is dropped.
    PENDING.store(0, Ordering::SeqCst);
    DETACH_PENDING.store(0, Ordering::SeqCst);
    if netattach_on_probe_rs(0) != 1 { fails += 1; }
    netattach_on_detach_rs();
    if netattach_take_rs() != 0 { fails += 1; }        // stale attach dropped
    if netattach_take_detach_rs() != 1 { fails += 1; }

    restore(&saved);
    fails
}
