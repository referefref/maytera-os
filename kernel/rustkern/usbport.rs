// rustkern/usbport.rs - #307/#433 follow-up: THE DECISION OF WHEN TO STOP
// RETRYING A ROOT PORT THAT WILL NEVER ENUMERATE.
//
// WHY THIS EXISTS
// ---------------
// Owner report, real ASUS i7-4720HQ (golden 2250): "usb ports on the left hand
// side of the laptop do not work and i assumed theyre faulty, can we disable
// those ports". His boot log says exactly why, and it is worse than a dead
// port:
//
//   [xHCI] Port 1: connected, speed Low-Speed (1.5 Mbps)
//   [xHCI] Port 1 slot 1: address device FAILED       (x4, slots 1..4)
//   [xHCI] Port 1: enumeration FAILED after retries; left eligible for re-scan
//   [xHCI] re-scan: port 1 connected but not enumerated; resetting + enumerating
//   ... forever ...
//
// MEASURED from that capture: the re-scan worker's heartbeat runs scan #1 at
// t=36678ms and scan #500 at t=2024232ms, i.e. one cycle every 3983ms, and each
// cycle spends its whole 4-attempt budget. 2046 Address Device attempts in a
// 34-minute log, one per second, for a port with nothing on it.
//
// The port is FAULTY, not occupied. CCS reads 1 in all 180 PORTSC samples of
// it, there are ZERO CCS transitions in the whole capture, and the only other
// bits that move are PED and PLS oscillating between 1/U0 and 0/Polling: the
// port asserts a phantom connect, negotiates a Low-Speed link, and then falls
// out of the enabled state every time we reset it. Ports 2,4,6..21 never assert
// CCS at all, so this is one faulty port, not a faulty side of the machine.
//
// WHY THE EXISTING GOVERNOR DID NOT CATCH IT
// ------------------------------------------
// #135 already added a flap governor, and it is keyed on CONFIRMED
// DISCONNECTS: a port whose CCS drops repeatedly gets backed off. This fault
// has CCS PINNED HIGH, so the flap count is zero forever and the governor never
// arms. The two faults look identical from the compositor (a port burning a
// device slot and a command-ring round trip every second) and were invisible to
// each other in code. This module is the missing half: back off on a port that
// keeps FAILING TO ENUMERATE, as distinct from one that keeps DISCONNECTING.
//
// THE RULE IS BEHAVIOURAL, NEVER POSITIONAL - AND THAT IS A SAFETY PROPERTY,
// NOT A STYLE PREFERENCE. The owner BOOTS FROM A USB STICK. "Disable the
// left-hand ports" cannot be implemented as a port index, because nothing in
// this kernel knows which physical socket a root-port number is wired to, and a
// wrong guess takes his root filesystem away with no way to boot and fix it. A
// port that has definitively failed to enumerate is, by construction, a port
// with nothing working on it: giving up on it cannot disconnect anything he
// uses. A port with a live device is never touched by any path in here.
//
// RE-ARMING IS EVENT-DRIVEN, NOT TIMED. A terminal port comes back only when
// the hardware reports a real connect-status change (CCS edge). No timer, no
// "try again every N minutes" - that would just be the same churn with a longer
// period, and CLAUDE.md's preference order says a timeout is the wrong shape
// when the correct wake source exists.
//
// CORRECTED 2026-08-29 (deadport). THIS PARAGRAPH USED TO END: "On the owner's
// machine the phantom connect never drops, so his port stays terminal, which is
// the intent." THAT IS FALSE, and it was the load-bearing assumption of the
// whole re-arm design: a CCS edge was chosen as the revival trigger precisely
// because this fault was believed unable to produce one.
//
// It was true of the capture it was measured from. The NEXT capture,
// /ssdmirror/asus-stick-logs-20260828/BOOTLOG.TXT, one boot on one monotonic
// clock, shows the same port doing exactly what it was said not to do:
//
//   [PORTSC] p1 was          raw=00000a03 CCS=1 PED=1 PLS=0(U0)
//   [PORTSC] p1 now CCS-CHANGED raw=000202a0 CCS=0 PED=0 PLS=5(RxDetect)
//   [xHCI] port 1: connect-status CHANGED ... Re-arming it for enumeration.
//   [PORTSC] p1 now CCS-CHANGED raw=00020ae1 CCS=1 PED=0 PLS=7(Polling)
//
// The port dropped CCS and re-asserted it on its own, with nothing plugged in.
// Two give-up lines in that boot, 48 failed attempts instead of 24. The retry
// budget was a real bound on ONE arming; the number of armings had none, so the
// fault was slowed rather than bounded. USBPORT_MAX_RETIRES below is the
// missing bound, and the lesson is the general one: a governor whose reset
// condition is unbounded is not a governor, however tight its inner budget.
//
// RUST, per the standing directive (CLAUDE.md): this is new kernel logic and
// there is no performance reason for C. It runs a few times per second at most,
// touches no MMIO, does no I/O, and the config parser below is the exact
// untrusted-input shape (a file the user hand-edits) that the Rust-first policy
// exists for. All MMIO, command-ring and logging work stays in xhci.c; this
// module is pure decision + state so it can be self-tested with no hardware.
//
// CONCURRENCY. Plain relaxed atomics, no lock. The callers are the re-scan
// worker thread and boot-time enumeration, which never run concurrently on the
// same port, and the worst outcome of a race would be one extra retry budget.
// A lock here would put a turnstile on a path that pre-scheduler boot code
// calls, which is the #426 shape. No `static mut`, so no aliasing UB.

use core::sync::atomic::{AtomicU32, Ordering};

// ---------------------------------------------------------------------------
// Geometry. MIRRORED IN kernel/drivers/usbport.h and checked at boot by
// usbport_abi_check_rs(), so the C and Rust ideas of the table cannot diverge
// silently into an out-of-range port being quietly folded onto port 0.
// ---------------------------------------------------------------------------

/// Matches MAX_XHCI_CONTROLLERS in drivers/xhci.c.
pub const USBPORT_MAX_CTRL: usize = 4;
/// Matches the g_port_enumerated[][256] geometry in drivers/xhci.c.
pub const USBPORT_MAX_PORT: usize = 256;

/// How many FULL retry budgets (XHCI_ENUM_RETRIES attempts each) a connected
/// port may burn before it is declared terminal.
///
/// Six, deliberately, not one. #433's lesson is that real xHCI enumeration is
/// racy and a port that fails once may succeed moments later, so a port must
/// ALWAYS be allowed to retry; the bug being fixed here is that "always" had no
/// upper bound. Six budgets is 24 Address Device attempts spread over roughly
/// 16 seconds on the owner's machine (two budgets during boot enumeration, then
/// four ~4s re-scan cycles), which is far more slack than any device that has
/// ever come up late on the iMac needed, and it is finite.
pub const USBPORT_GIVEUP_BUDGETS: u32 = 6;

/// How many times a port may be RETIRED AND REVIVED before the revival itself
/// stops being granted.
///
/// WHY THIS EXISTS, AND WHY THE ORIGINAL DESIGN NEEDED IT (deadport).
///
/// The header above says the owner's faulty port has "CCS PINNED HIGH" and
/// "ZERO CCS transitions in the whole capture", and the whole re-arm design
/// rests on that: a CCS edge was chosen as the revival trigger precisely
/// because it was believed to be UNREACHABLE on this fault. That was true of
/// the capture it was measured from. It is FALSE of the next one.
///
/// MEASURED, /ssdmirror/asus-stick-logs-20260828/BOOTLOG.TXT, one boot, one
/// monotonic clock, port 1 with nothing plugged into it:
///
///   [PORTSC] p1 was          raw=00000a03 CCS=1 PED=1 PLS=0(U0)
///   [PORTSC] p1 now CCS-CHANGED raw=000202a0 CCS=0 PED=0 PLS=5(RxDetect)
///   [xHCI] port 1: connect-status CHANGED, so the earlier give-up verdict is
///          stale. Re-arming it for enumeration.
///   [PORTSC] p1 now CCS-CHANGED raw=00020ae1 CCS=1 PED=0 PLS=7(Polling)
///   ... 24 more Address Device attempts, then a SECOND give-up ...
///
/// The port dropped and re-asserted CCS on its own. That is a genuine CCS edge
/// by every test the driver can apply, so the governor did exactly what it was
/// told to do and handed back a full fresh budget. Two give-up lines in one
/// boot, 48 failed attempts. The bound on retries was real; the bound on
/// REVIVALS did not exist, so the fault was slowed, not bounded.
///
/// Three, so a port that genuinely comes back late still gets two more chances
/// after the first verdict. Worst case per boot is now
/// MAX_RETIRES * GIVEUP_BUDGETS * XHCI_ENUM_RETRIES = 3 * 6 * 4 = 72 Address
/// Device attempts, and then the port is silent for the rest of the boot.
///
/// NOTHING HERE IS PERSISTED. A hard-retired port is re-armed by a reboot, so
/// the worst case for a port this governor misjudges is "it works again after a
/// restart", never "it is dead until someone finds the right config file". Only
/// /USBPORT.CFG, which the operator writes deliberately, survives a boot.
pub const USBPORT_MAX_RETIRES: u32 = 3;

// Port states, returned by usbport_state_rs().
/// Enumeration may be attempted.
pub const USBPORT_ST_ACTIVE: i32 = 0;
/// Retry budget exhausted; no further attempts until a CCS edge.
pub const USBPORT_ST_TERMINAL: i32 = 1;
/// Listed in /USBPORT.CFG; never attempted, and a CCS edge does NOT re-arm it.
pub const USBPORT_ST_CFGOFF: i32 = 2;
/// Retired USBPORT_MAX_RETIRES times; a CCS edge no longer revives it. Cleared
/// only by a reboot (this module persists nothing).
pub const USBPORT_ST_HARD: i32 = 3;

// Return codes from usbport_budget_failed_rs().
/// Still under budget: the caller keeps this port eligible for re-scan.
pub const USBPORT_GIVEUP_NO: i32 = 0;
/// This call crossed the threshold. The caller must emit the loud, DURABLE
/// give-up line exactly once. Returned once and only once per terminal
/// transition, so the caller cannot flood by asking again.
pub const USBPORT_GIVEUP_NOW: i32 = 1;
/// Already terminal before this call; caller stays quiet.
pub const USBPORT_GIVEUP_ALREADY: i32 = 2;
/// This call retired the port for the LAST time: it will not be revived by a
/// connect-status change again this boot. The caller logs a distinct, louder
/// line, once.
pub const USBPORT_GIVEUP_FINAL: i32 = 3;

// ---------------------------------------------------------------------------
// State. One u32 per (controller, port): bits 0-7 the exhausted-budget count,
// bits 8-15 the retire-and-revive count, bits 16-31 the state. Packed into a
// single atomic so a reader can never observe a count from one epoch with a
// state from another. Both counts saturate at 0xff and their live maxima are 6
// and 3, so neither can reach the other's field.
// ---------------------------------------------------------------------------

const CELLS: usize = USBPORT_MAX_CTRL * USBPORT_MAX_PORT;

#[allow(clippy::declare_interior_mutable_const)]
const ZERO: AtomicU32 = AtomicU32::new(0);
static CELL: [AtomicU32; CELLS] = [ZERO; CELLS];

#[inline]
fn idx(ctrl: i32, port: i32) -> Option<usize> {
    if ctrl < 0 || port < 0 {
        return None;
    }
    let (c, p) = (ctrl as usize, port as usize);
    if c >= USBPORT_MAX_CTRL || p >= USBPORT_MAX_PORT {
        return None;
    }
    Some(c * USBPORT_MAX_PORT + p)
}

#[inline]
fn pack(state: u32, budgets: u32, retires: u32) -> u32 {
    (state << 16) | ((retires & 0xff) << 8) | (budgets & 0xff)
}
/// -> (state, budgets, retires)
#[inline]
fn unpack(v: u32) -> (u32, u32, u32) {
    (v >> 16, v & 0xff, (v >> 8) & 0xff)
}

// ---------------------------------------------------------------------------
// The FFI surface. Every symbol here is declared in kernel/drivers/usbport.h
// and listed in kernel/rust-symbols.manifest.
// ---------------------------------------------------------------------------

/// May the caller attempt to enumerate this port? 1 = yes, 0 = no.
///
/// An unknown (out-of-range) port answers YES. Deliberate: this governor exists
/// to STOP work, and a table-geometry mistake must never be able to suppress
/// enumeration of a port that might hold the boot device. Fail open.
#[no_mangle]
pub extern "C" fn usbport_should_enumerate_rs(ctrl: i32, port: i32) -> i32 {
    match idx(ctrl, port) {
        None => 1,
        Some(i) => {
            let (st, _, _) = unpack(CELL[i].load(Ordering::Relaxed));
            if st == USBPORT_ST_ACTIVE as u32 {
                1
            } else {
                0
            }
        }
    }
}

/// Record that a full retry budget was exhausted on a connected port.
/// `still_connected` is the caller's freshly-read CCS bit.
///
/// A budget that ended because the device PHYSICALLY WENT AWAY is not counted:
/// that is an unplug, not a broken port, and counting it would let a user who
/// pulls a stick mid-enumeration six times retire a working socket.
#[no_mangle]
pub extern "C" fn usbport_budget_failed_rs(ctrl: i32, port: i32, still_connected: i32) -> i32 {
    let i = match idx(ctrl, port) {
        None => return USBPORT_GIVEUP_NO,
        Some(i) => i,
    };
    let (st, n, r) = unpack(CELL[i].load(Ordering::Relaxed));
    if st != USBPORT_ST_ACTIVE as u32 {
        return USBPORT_GIVEUP_ALREADY;
    }
    if still_connected == 0 {
        return USBPORT_GIVEUP_NO;
    }
    let n = n.saturating_add(1);
    if n < USBPORT_GIVEUP_BUDGETS {
        CELL[i].store(pack(USBPORT_ST_ACTIVE as u32, n, r), Ordering::Relaxed);
        return USBPORT_GIVEUP_NO;
    }
    // Retiring. Count it: a port that has already been retired and revived
    // MAX_RETIRES times has now demonstrated that its connect-status changes do
    // not predict a device that can be addressed, so the next one is not
    // believed either. Without this the CCS edge is an unbounded reset on a
    // bounded retry budget, which is not a bound at all (see MAX_RETIRES).
    let r = r.saturating_add(1);
    if r >= USBPORT_MAX_RETIRES {
        CELL[i].store(pack(USBPORT_ST_HARD as u32, n, r), Ordering::Relaxed);
        USBPORT_GIVEUP_FINAL
    } else {
        CELL[i].store(pack(USBPORT_ST_TERMINAL as u32, n, r), Ordering::Relaxed);
        USBPORT_GIVEUP_NOW
    }
}

/// A device on this port enumerated successfully. Clears the budget count so an
/// old burst cannot retire a port that now works (the same reasoning as #135's
/// flap-count decay).
#[no_mangle]
pub extern "C" fn usbport_enum_ok_rs(ctrl: i32, port: i32) {
    if let Some(i) = idx(ctrl, port) {
        let (st, _, _) = unpack(CELL[i].load(Ordering::Relaxed));
        // A device on this port ADDRESSED. That is the only positive evidence
        // this module ever gets, so it clears the retire count as well as the
        // budget count: the port is fully forgiven, not merely paroled. Note it
        // deliberately does not rescue ST_HARD, because a hard-retired port is
        // never enumerated, so this can never be reached for one.
        if st != USBPORT_ST_CFGOFF as u32 && st != USBPORT_ST_HARD as u32 {
            CELL[i].store(pack(USBPORT_ST_ACTIVE as u32, 0, 0), Ordering::Relaxed);
        }
    }
}

/// The hardware reported a real connect-status change on this port (a CCS
/// edge). This is the ONLY thing that revives a terminal port: something was
/// physically plugged or unplugged, so our verdict about what is there is stale.
///
/// It deliberately does NOT revive a port the user disabled in /USBPORT.CFG.
/// That is an explicit instruction and a replug is not a retraction of it.
/// Returns 1 if a terminal port was revived (so the caller can log it), else 0.
#[no_mangle]
pub extern "C" fn usbport_connect_changed_rs(ctrl: i32, port: i32) -> i32 {
    let i = match idx(ctrl, port) {
        None => return 0,
        Some(i) => i,
    };
    // ONE load: reading the state and the retire count from the same packed
    // word is the reason they are packed together, and a second load here could
    // observe a different epoch.
    let (st, _, keep_r) = unpack(CELL[i].load(Ordering::Relaxed));
    // CFGOFF is an explicit operator instruction and a replug is not a
    // retraction of it. HARD is this module's own final verdict, reached only
    // after MAX_RETIRES full retire-and-revive cycles; believing the next CCS
    // edge after ignoring the last three would make the bound meaningless.
    if st == USBPORT_ST_CFGOFF as u32 || st == USBPORT_ST_HARD as u32 {
        return 0;
    }
    let revived = st == USBPORT_ST_TERMINAL as u32;
    if !revived {
        // ALREADY ACTIVE, SO THERE IS NO STALE VERDICT TO RETRACT, AND THE
        // BUDGET COUNT MUST NOT BE TOUCHED.
        //
        // This is the second half of the same bug, and it defeated the first
        // fix completely at a high enough flap rate. MEASURED in VM arm B
        // (a synthetic connect-status change every 5 re-scans, roughly every
        // 15s): the budget counter climbed 1/6, 2/6, 3/6, 4/6, 5/6 and NEVER
        // reached 6/6, because every flap zeroed it first. The port therefore
        // never retired a second time, `retires` never incremented, and
        // USBPORT_MAX_RETIRES was unreachable: 260 failed attempts and still
        // climbing, indistinguishable from the governor being switched off.
        //
        // Clearing the count here was only ever meant to give a REVIVED port a
        // clean slate. On an active port it silently converts "6 budgets" into
        // "6 budgets with no connect-status change in between", which is not a
        // bound at all on hardware whose whole fault is spurious edges. The
        // count is now cleared in exactly two places: a real revival (below)
        // and a successful enumeration (usbport_enum_ok_rs), which are the two
        // events that genuinely make an old failure count meaningless.
        return 0;
    }
    // The retire count is deliberately PRESERVED across a revival. Clearing it
    // here is exactly the bug this change fixes: it would let a port that flaps
    // CCS on its own hand itself a fresh budget forever.
    CELL[i].store(pack(USBPORT_ST_ACTIVE as u32, 0, keep_r), Ordering::Relaxed);
    1
}

/// Mark a port disabled by operator config. Returns 1 if this changed the state.
#[no_mangle]
pub extern "C" fn usbport_config_disable_rs(ctrl: i32, port: i32) -> i32 {
    let i = match idx(ctrl, port) {
        None => return 0,
        Some(i) => i,
    };
    let (st, _, _) = unpack(CELL[i].load(Ordering::Relaxed));
    CELL[i].store(pack(USBPORT_ST_CFGOFF as u32, 0, 0), Ordering::Relaxed);
    if st == USBPORT_ST_CFGOFF as u32 {
        0
    } else {
        1
    }
}

/// Current state of a port, for the heartbeat line and the self-test.
#[no_mangle]
pub extern "C" fn usbport_state_rs(ctrl: i32, port: i32) -> i32 {
    match idx(ctrl, port) {
        None => USBPORT_ST_ACTIVE,
        Some(i) => unpack(CELL[i].load(Ordering::Relaxed)).0 as i32,
    }
}

/// How many retry budgets this port has burned (for the give-up log line).
#[no_mangle]
pub extern "C" fn usbport_budgets_rs(ctrl: i32, port: i32) -> i32 {
    match idx(ctrl, port) {
        None => 0,
        Some(i) => unpack(CELL[i].load(Ordering::Relaxed)).1 as i32,
    }
}

/// How many times this port has been retired this boot, for the log line and
/// the heartbeat.
#[no_mangle]
pub extern "C" fn usbport_retires_rs(ctrl: i32, port: i32) -> i32 {
    match idx(ctrl, port) {
        None => 0,
        Some(i) => unpack(CELL[i].load(Ordering::Relaxed)).2 as i32,
    }
}

/// Locks the C header's constants against this module's. Returns 1 on match.
#[no_mangle]
pub extern "C" fn usbport_abi_check_rs(
    max_ctrl: i32,
    max_port: i32,
    budgets: i32,
    st_active: i32,
    st_terminal: i32,
    st_cfgoff: i32,
    max_retires: i32,
    st_hard: i32,
) -> i32 {
    let ok = max_ctrl == USBPORT_MAX_CTRL as i32
        && max_port == USBPORT_MAX_PORT as i32
        && budgets == USBPORT_GIVEUP_BUDGETS as i32
        && st_active == USBPORT_ST_ACTIVE
        && st_terminal == USBPORT_ST_TERMINAL
        && st_cfgoff == USBPORT_ST_CFGOFF
        && max_retires == USBPORT_MAX_RETIRES as i32
        && st_hard == USBPORT_ST_HARD;
    if ok {
        1
    } else {
        0
    }
}

// ---------------------------------------------------------------------------
// /USBPORT.CFG - the explicit operator override.
//
// The owner asked to "disable those ports". The automatic rule above answers
// that behaviourally, but a behavioural rule is a judgement and he is entitled
// to a deterministic one, so this is the escape hatch in BOTH directions: it
// can retire a port the governor would keep retrying, and (by simply not
// listing a port) it never removes one he needs.
//
// FORMAT, one directive per line, '#' to end-of-line is a comment:
//
//     disable 1        # root port 1 on EVERY xHCI controller
//     disable 0:1      # controller 0, root port 1 only
//
// Port numbers are 1-based, matching the "[xHCI] Port N" numbering in
// /BOOTLOG.TXT, because that log is the only place the owner can see which port
// misbehaved. Controller numbers are 0-based, matching "[xHCI-HB] ctrlN".
//
// PARSING AN OPERATOR-EDITED FILE IS UNTRUSTED-INPUT PARSING, which is why it
// is here in Rust and not in the driver. It never allocates, never reads past
// `len`, requires no NUL terminator, and silently ignores anything it does not
// understand rather than guessing - a typo'd line that disabled the wrong port
// would be exactly the outcome this whole ticket is trying to avoid.
// ---------------------------------------------------------------------------

/// Parse `buf[..len]` and write up to `max` (ctrl, port) pairs into the caller's
/// arrays. Returns the number written, or -1 on a bad argument.
///
/// A `disable <port>` line with no controller writes ctrl = -1, meaning "every
/// controller"; the C caller expands that.
///
/// # Safety
/// `buf` must be readable for `len` bytes; `out_ctrl` and `out_port` must each
/// be writable for `max` i32s.
#[no_mangle]
pub unsafe extern "C" fn usbport_parse_cfg_rs(
    buf: *const u8,
    len: i32,
    out_ctrl: *mut i32,
    out_port: *mut i32,
    max: i32,
) -> i32 {
    if buf.is_null() || out_ctrl.is_null() || out_port.is_null() || len < 0 || max <= 0 {
        return -1;
    }
    let s = core::slice::from_raw_parts(buf, len as usize);
    let mut n: usize = 0;
    let maxn = max as usize;

    let mut i: usize = 0;
    while i < s.len() && n < maxn {
        // Slice off one line.
        let start = i;
        while i < s.len() && s[i] != b'\n' && s[i] != b'\r' {
            i += 1;
        }
        let mut line = &s[start..i];
        while i < s.len() && (s[i] == b'\n' || s[i] == b'\r') {
            i += 1;
        }
        // Strip a trailing comment.
        if let Some(h) = line.iter().position(|&c| c == b'#' || c == b';') {
            line = &line[..h];
        }
        if let Some((c, p)) = parse_line(line) {
            *out_ctrl.add(n) = c;
            *out_port.add(n) = p;
            n += 1;
        }
    }
    n as i32
}

/// `disable [<ctrl>:]<port>` -> (ctrl, port), ctrl = -1 for "all controllers".
/// Pure and total; separated out so the self-test can hammer it.
fn parse_line(line: &[u8]) -> Option<(i32, i32)> {
    let mut t = line;
    // Leading whitespace.
    while let [c, rest @ ..] = t {
        if *c == b' ' || *c == b'\t' {
            t = rest;
        } else {
            break;
        }
    }
    // Keyword. Case-insensitive, so DISABLE and disable both work.
    const KW: &[u8] = b"disable";
    if t.len() < KW.len() {
        return None;
    }
    for (a, b) in t[..KW.len()].iter().zip(KW.iter()) {
        if a.to_ascii_lowercase() != *b {
            return None;
        }
    }
    t = &t[KW.len()..];
    // At least one separator, so "disabled" is not read as "disable d".
    if !matches!(t.first(), Some(b' ') | Some(b'\t') | Some(b'=')) {
        return None;
    }
    while let [c, rest @ ..] = t {
        if *c == b' ' || *c == b'\t' || *c == b'=' {
            t = rest;
        } else {
            break;
        }
    }
    let (a, rest) = take_u32(t)?;
    if let Some(b':') = rest.first() {
        let (b, tail) = take_u32(&rest[1..])?;
        if !trailing_ok(tail) {
            return None;
        }
        if a as usize >= USBPORT_MAX_CTRL || b == 0 || b as usize > USBPORT_MAX_PORT {
            return None;
        }
        // Port numbers in the file are 1-based; the table is 0-based.
        Some((a as i32, (b - 1) as i32))
    } else {
        if !trailing_ok(rest) {
            return None;
        }
        if a == 0 || a as usize > USBPORT_MAX_PORT {
            return None;
        }
        Some((-1, (a - 1) as i32))
    }
}

/// Only whitespace may follow a directive. "disable 1 2" is a typo, not two
/// directives, and must be REJECTED rather than half-applied.
fn trailing_ok(t: &[u8]) -> bool {
    t.iter().all(|c| *c == b' ' || *c == b'\t')
}

fn take_u32(t: &[u8]) -> Option<(u32, &[u8])> {
    let mut v: u32 = 0;
    let mut k = 0usize;
    while k < t.len() && t[k].is_ascii_digit() {
        v = v.checked_mul(10)?.checked_add((t[k] - b'0') as u32)?;
        k += 1;
    }
    if k == 0 {
        None
    } else {
        Some((v, &t[k..]))
    }
}

// ---------------------------------------------------------------------------
// Boot self-test. Proves the state machine actually retires a port after the
// budget, actually revives it on a CCS edge, actually REFUSES to revive a
// config-disabled one, and that the parser rejects the malformed lines an
// operator will really write. A self-test that only exercises the happy path
// would pass on a module that never retires anything, which is the pre-fix
// behaviour it exists to distinguish from.
// ---------------------------------------------------------------------------

/// Returns 1 if every check passed; `*out_checks` receives the check count.
///
/// # Safety
/// `out_checks` must be writable, or null.
#[no_mangle]
pub unsafe extern "C" fn usbport_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks: u32 = 0;
    let mut ok = true;
    macro_rules! check {
        ($cond:expr) => {{
            checks += 1;
            if !($cond) {
                ok = false;
            }
        }};
    }

    // Use the top controller slot so a live controller 0 is never disturbed.
    let c = (USBPORT_MAX_CTRL - 1) as i32;
    let p = (USBPORT_MAX_PORT - 1) as i32;
    let saved = CELL[c as usize * USBPORT_MAX_PORT + p as usize].load(Ordering::Relaxed);

    // 1. A fresh port is enumerable.
    CELL[c as usize * USBPORT_MAX_PORT + p as usize].store(0, Ordering::Relaxed);
    check!(usbport_should_enumerate_rs(c, p) == 1);

    // 2. Budget failures on a DISCONNECTED port never retire it, however many.
    for _ in 0..(USBPORT_GIVEUP_BUDGETS * 4) {
        check!(usbport_budget_failed_rs(c, p, 0) == USBPORT_GIVEUP_NO);
    }
    check!(usbport_should_enumerate_rs(c, p) == 1);

    // 3. Budget failures on a CONNECTED port retire it, at the budget and not
    //    before, and the "log it" answer is returned exactly once.
    for _ in 0..(USBPORT_GIVEUP_BUDGETS - 1) {
        check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_NO);
        check!(usbport_should_enumerate_rs(c, p) == 1);
    }
    check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_NOW);
    check!(usbport_state_rs(c, p) == USBPORT_ST_TERMINAL);
    // THE POINT OF THE WHOLE CHANGE: it now says no.
    check!(usbport_should_enumerate_rs(c, p) == 0);
    // And it does not shout again.
    check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_ALREADY);
    check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_ALREADY);

    // 4. A real connect-status change revives it, once.
    check!(usbport_connect_changed_rs(c, p) == 1);
    check!(usbport_should_enumerate_rs(c, p) == 1);
    check!(usbport_connect_changed_rs(c, p) == 0);

    // 5. A successful enumeration clears the budget count.
    usbport_budget_failed_rs(c, p, 1);
    usbport_enum_ok_rs(c, p);
    check!(usbport_budgets_rs(c, p) == 0);

    // 6. A config-disabled port is off, and a replug does NOT retract that.
    check!(usbport_config_disable_rs(c, p) == 1);
    check!(usbport_should_enumerate_rs(c, p) == 0);
    check!(usbport_connect_changed_rs(c, p) == 0);
    check!(usbport_should_enumerate_rs(c, p) == 0);
    check!(usbport_state_rs(c, p) == USBPORT_ST_CFGOFF);
    usbport_enum_ok_rs(c, p);
    check!(usbport_state_rs(c, p) == USBPORT_ST_CFGOFF);

    // 6b. THE BOUND ON REVIVALS (deadport). A port that is revived by a
    //     connect-status change and immediately fails again must EVENTUALLY
    //     stop being revived, or the retry budget is not a bound at all: the
    //     owner's faulty port produced a real CCS edge on its own and helped
    //     itself to a second full budget in the same boot. Drive the whole
    //     escalation, because a self-test that only reaches the first verdict
    //     would pass on exactly the pre-fix code.
    let cell = c as usize * USBPORT_MAX_PORT + p as usize;
    CELL[cell].store(0, Ordering::Relaxed);

    // Retire 1 of 3: the ordinary verdict, still revivable.
    for _ in 0..(USBPORT_GIVEUP_BUDGETS - 1) {
        check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_NO);
    }
    check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_NOW);
    check!(usbport_state_rs(c, p) == USBPORT_ST_TERMINAL);
    check!(usbport_retires_rs(c, p) == 1);

    // A CCS edge revives it AND THE RETIRE COUNT SURVIVES. If this ever reads
    // zero the escalation can never be reached and the port churns for the rest
    // of the boot, which is precisely the bug.
    check!(usbport_connect_changed_rs(c, p) == 1);
    check!(usbport_retires_rs(c, p) == 1);
    check!(usbport_budgets_rs(c, p) == 0);
    check!(usbport_should_enumerate_rs(c, p) == 1);

    // Retire 2 of 3: still revivable.
    for _ in 0..USBPORT_GIVEUP_BUDGETS {
        usbport_budget_failed_rs(c, p, 1);
    }
    check!(usbport_state_rs(c, p) == USBPORT_ST_TERMINAL);
    check!(usbport_retires_rs(c, p) == 2);
    check!(usbport_connect_changed_rs(c, p) == 1);

    // Retire 3 of 3: FINAL. A distinct return code, so the caller can log a
    // distinct line, and a distinct state, so the heartbeat can show it.
    for _ in 0..(USBPORT_GIVEUP_BUDGETS - 1) {
        check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_NO);
    }
    check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_FINAL);
    check!(usbport_state_rs(c, p) == USBPORT_ST_HARD);
    check!(usbport_retires_rs(c, p) == USBPORT_MAX_RETIRES as i32);
    check!(usbport_should_enumerate_rs(c, p) == 0);

    // THE POINT OF THE WHOLE CHANGE: a connect-status change no longer revives
    // it, however many of them arrive.
    check!(usbport_connect_changed_rs(c, p) == 0);
    check!(usbport_connect_changed_rs(c, p) == 0);
    check!(usbport_should_enumerate_rs(c, p) == 0);
    check!(usbport_state_rs(c, p) == USBPORT_ST_HARD);
    // And it does not shout again.
    check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_ALREADY);

    // 6b-ii. A connect-status change on an ALREADY-ACTIVE port must NOT reset
    //        the budget count. If it does, a port flapping faster than the
    //        budget can fill never retires at all and the whole escalation
    //        above is unreachable. This exact regression was MEASURED in a VM
    //        (260 attempts and climbing) after the first version of the fix.
    CELL[cell].store(0, Ordering::Relaxed);
    check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_NO);
    check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_NO);
    check!(usbport_budgets_rs(c, p) == 2);
    check!(usbport_connect_changed_rs(c, p) == 0);   // nothing to revive
    check!(usbport_budgets_rs(c, p) == 2);           // AND THE COUNT SURVIVED
    check!(usbport_state_rs(c, p) == USBPORT_ST_ACTIVE);
    // So the port still retires after the remaining budgets, flaps or no flaps.
    for _ in 0..(USBPORT_GIVEUP_BUDGETS - 3) {
        usbport_connect_changed_rs(c, p);
        check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_NO);
    }
    usbport_connect_changed_rs(c, p);
    check!(usbport_budget_failed_rs(c, p, 1) == USBPORT_GIVEUP_NOW);
    check!(usbport_state_rs(c, p) == USBPORT_ST_TERMINAL);

    // 6c. A port that ACTUALLY ENUMERATES is fully forgiven, retire count and
    //     all. Without this, a working port that had one bad patch early in a
    //     boot would carry that history for the rest of it, and the bound meant
    //     to protect a dead port would start punishing a live one.
    CELL[cell].store(0, Ordering::Relaxed);
    for _ in 0..USBPORT_GIVEUP_BUDGETS {
        usbport_budget_failed_rs(c, p, 1);
    }
    check!(usbport_retires_rs(c, p) == 1);
    usbport_connect_changed_rs(c, p);
    usbport_enum_ok_rs(c, p);
    check!(usbport_retires_rs(c, p) == 0);
    check!(usbport_budgets_rs(c, p) == 0);
    check!(usbport_state_rs(c, p) == USBPORT_ST_ACTIVE);

    // 6d. The three packed fields do not bleed into one another. The counts are
    //     now 8 bits each rather than one 16-bit count, so a packing mistake
    //     would show up as a port silently changing state.
    check!(unpack(pack(3, 6, 3)) == (3, 6, 3));
    check!(unpack(pack(USBPORT_ST_CFGOFF as u32, 0xff, 0)) == (USBPORT_ST_CFGOFF as u32, 0xff, 0));
    check!(unpack(pack(USBPORT_ST_HARD as u32, 0, 0xff)) == (USBPORT_ST_HARD as u32, 0, 0xff));
    // The live maxima fit their fields with room to spare.
    check!(USBPORT_GIVEUP_BUDGETS <= 0xff && USBPORT_MAX_RETIRES <= 0xff);

    CELL[c as usize * USBPORT_MAX_PORT + p as usize].store(saved, Ordering::Relaxed);

    // 7. An out-of-range port FAILS OPEN. If this is ever backwards, a geometry
    //    bug becomes "the boot device's port is silently never enumerated".
    check!(usbport_should_enumerate_rs(-1, 0) == 1);
    check!(usbport_should_enumerate_rs(0, -1) == 1);
    check!(usbport_should_enumerate_rs(USBPORT_MAX_CTRL as i32, 0) == 1);
    check!(usbport_should_enumerate_rs(0, USBPORT_MAX_PORT as i32) == 1);

    // 8. The parser: accept what is documented.
    check!(parse_line(b"disable 1") == Some((-1, 0)));
    check!(parse_line(b"  disable   7  ") == Some((-1, 6)));
    check!(parse_line(b"DISABLE 21") == Some((-1, 20)));
    check!(parse_line(b"disable 0:1") == Some((0, 0)));
    check!(parse_line(b"disable 3:256") == Some((3, 255)));
    check!(parse_line(b"disable=4") == Some((-1, 3)));

    // 9. The parser: reject what is not. Every one of these is a line a real
    //    operator writes, and every one of them must be a no-op rather than a
    //    guess at a port number.
    check!(parse_line(b"").is_none());
    check!(parse_line(b"# disable 1").is_none()); // comment stripping is the caller's
    check!(parse_line(b"disabled 1").is_none()); // no separator after the keyword
    check!(parse_line(b"disable").is_none());
    check!(parse_line(b"disable ").is_none());
    check!(parse_line(b"disable 0").is_none()); // ports are 1-based
    check!(parse_line(b"disable 257").is_none()); // past the table
    check!(parse_line(b"disable 4:1").is_none()); // no such controller
    check!(parse_line(b"disable 0:0").is_none());
    check!(parse_line(b"disable 1 2").is_none()); // ambiguous, so refused
    check!(parse_line(b"disable abc").is_none());
    check!(parse_line(b"disable 99999999999999").is_none()); // no wrap
    check!(parse_line(b"enable 1").is_none());

    // 10. Whole-file parse, including CRLF, comments and blank lines.
    let f = b"# ports to skip\r\ndisable 1\r\n\r\n  disable 0:2   # trailing\r\nbogus\r\ndisable 3\n";
    let mut oc = [0i32; 8];
    let mut op = [0i32; 8];
    let got = usbport_parse_cfg_rs(f.as_ptr(), f.len() as i32, oc.as_mut_ptr(), op.as_mut_ptr(), 8);
    check!(got == 3);
    check!(oc[0] == -1 && op[0] == 0);
    check!(oc[1] == 0 && op[1] == 1);
    check!(oc[2] == -1 && op[2] == 2);

    // 11. The parse respects `max` and never writes past it.
    let mut oc2 = [0i32; 2];
    let mut op2 = [0i32; 2];
    let got2 =
        usbport_parse_cfg_rs(f.as_ptr(), f.len() as i32, oc2.as_mut_ptr(), op2.as_mut_ptr(), 2);
    check!(got2 == 2);

    // 12. Degenerate arguments.
    check!(usbport_parse_cfg_rs(core::ptr::null(), 0, oc.as_mut_ptr(), op.as_mut_ptr(), 8) == -1);
    check!(usbport_parse_cfg_rs(f.as_ptr(), 0, oc.as_mut_ptr(), op.as_mut_ptr(), 8) == 0);

    if !out_checks.is_null() {
        *out_checks = checks;
    }
    if ok {
        1
    } else {
        0
    }
}
