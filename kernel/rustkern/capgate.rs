// rustkern/capgate.rs - Stage 0 of the system capability API
// (docs/SYSTEM_CAPABILITY_API.md section 12, "Close the ungated cross-process
// holes"). THE ONE DEFINITION of the two access rules that stage closes with,
// plus the refusal ledger that proves they fire.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16
// rule. There is no performance argument for C here: every function in this
// file is a handful of integer comparisons on a path that already does a
// syscall entry, a CR3 switch or a page map.
//
// ===========================================================================
// THE FIVE DEFECTS THIS FILE IS THE DECISION HALF OF
// ---------------------------------------------------------------------------
// Measured on dev @ e860a883, 2026-09-04, by reading the cited code. Each was
// reachable by an ordinary Ring-3 process with no capability, no consent, no
// audit record and no visible indicator.
//
//   1. SYS_GET_KEYBOARD (195), proc/syscall.c. A DESTRUCTIVE drain of the
//      global cooked key ring, gated on nothing but g_win16_owns_screen. Any
//      app could both READ and STEAL every keystroke in the system.
//   2. SYS_INJECT_KEY (197), proc/syscall.c. Posts a synthetic EVENT_KEY_DOWN
//      to the focused window, gated on nothing. Its matched pair
//      SYS_INJECT_MOUSE (214) IS gated with is_compositor(); one half of a
//      matched pair was gated and the other was not.
//   3. tcp_get_conn(), net/tcp.c. Checks only 0 <= sock < 64 and conn->active.
//      owner_pid was stamped and read ONLY by the Task Manager listing, so a
//      raw index into SYS_SEND / SYS_RECV / SYS_TCP_CLOSE read, wrote or tore
//      down ANOTHER PROCESS'S connection, the in-kernel sshd's included.
//   4. sys_shm_map(), ipc/shm.c. No credential check of any kind: any process
//      could map any allocated region 0..63 and read another process's memory.
//   5. sys_open_k(), proc/fdlayer.c. The /dev/ prefix was handled BEFORE the
//      permission block, so every device node bypassed perms_check() entirely.
//
// 1 and 2 are the reason this is Stage 0 rather than one increment among
// several. They DEFEAT #745, the only kernel-enforced consent gate the system
// has, in both directions:
//
//   - #745's trust story (proc/elevate.h) is that a requesting app "never
//     draws anything, never receives a keystroke and never learns the
//     password", because the COMPOSITOR draws the prompt. But the compositor
//     reads that prompt's keystrokes with SYS_GET_KEYBOARD, and defect 1 let
//     the requesting app drain the same ring. The password was readable by the
//     very app that raised the prompt.
//   - sys_elev_request() refuses with ELEV_ENOINPUT unless the window manager
//     recently delivered a REAL input event to a window the requester owns.
//     Defect 2 let an app post EVENT_KEY_DOWN to its own focused window and so
//     MANUFACTURE its own input credit, raising a password prompt the user
//     never asked for.
//
// compositor/main.c:854 already says "NO sys_inject_key. A trusted prompt
// whose keystrokes are also delivered to the app that raised it is worth
// nothing." That care was real and it was at the WRONG LAYER: an app simply
// does not go through the compositor. docs/CONTRACT_ARCHITECTURE.md section 6
// states the rule, and this file is where it moves to.
//
// ===========================================================================
// WHY THE RULES LIVE HERE AND NOT AT THE FIVE CALL SITES
// ---------------------------------------------------------------------------
// #500's finding about the pointer check applies verbatim: a per-handler rule
// is a rule every author has to remember, and 171 of 176 forgot. There are
// exactly TWO rules below, each written once:
//
//   capgate_is_compositor_rs()  "is the caller the latched compositor"
//   capgate_owner_ok_rs()       "does this Ring-3 caller own this handle"
//
// The second is a straight port of the ONLY correct instance of that check in
// the tree, pcm_lookup() at drivers/audio_pcm.c:171-196, INCLUDING the pthread
// widening a fresh implementation gets wrong (see the note on
// capgate_owner_ok_rs). TCP and SHM now share one definition with PCM's
// reasoning instead of growing a third and fourth private copy.
//
// ===========================================================================
// THE TRAP THIS FILE EXISTS TO AVOID, AND IT IS NOT HYPOTHETICAL
// ---------------------------------------------------------------------------
// The obvious fix for defects 1 and 2 is "call is_compositor() like
// SYS_INJECT_MOUSE does". THAT WOULD HAVE INTRODUCED A PRIVILEGE ESCALATION.
//
// is_compositor() in gui/fb_syscall.c is not a query. On a miss it falls
// through to fbown_claim_rs(p->pid) and CLAIMS the framebuffer latch for the
// caller. It is safe there because sys_fb_map() is the act of becoming the
// compositor. Wired to SYS_GET_KEYBOARD it would have meant that the first
// process to call sys_get_keyboard() while the latch was unclaimed BECAME the
// compositor, and the latch is not merely a drawing permission: proc/elevate.c
// admits SYS_ELEV_VIEW and SYS_ELEV_RESOLVE, reading a pending elevation
// request and SUBMITTING THE PASSWORD FOR IT, to whoever holds it. A gate
// added to close a keystroke leak would have opened a way to claim the screen.
//
// So this file uses fbown_is_owner_rs(), the NON-CLAIMING predicate that
// already exists and that proc/elevate.c already asks. Nothing in this file
// can arm, claim, narrow or release the latch, and capgate_selftest_rs()
// touches no global state at all, so it cannot perturb the latch either.

#![allow(dead_code)]

use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};

// ---------------------------------------------------------------------------
// Refusal ledger
// ---------------------------------------------------------------------------
// A gate nobody can see fire is a gate nobody can tell has stopped firing.
// These counters are the cheap, always-on half of the audit story; the durable
// half is the seclog/GraphFS record the C side emits (rate limited, because a
// refused syscall in a hot loop must not be able to fill the journal).
//
// Deliberately NOT reset by anything. A counter that a caller can zero is a
// counter an attacker can zero.

/// Refusal kinds. Kept dense and stable: they are printed by index.
pub const CAPGATE_K_INPUT_OBSERVE: u32 = 0; // SYS_GET_KEYBOARD, non-compositor
pub const CAPGATE_K_INPUT_INJECT: u32 = 1; // SYS_INJECT_KEY, non-compositor
pub const CAPGATE_K_TCP_OWNER: u32 = 2; // raw socket index, not the caller's
pub const CAPGATE_K_SHM_OWNER: u32 = 3; // shm region, not the caller's
pub const CAPGATE_K_DEV_PERM: u32 = 4; // /dev node refused by perms_check
pub const CAPGATE_K_MAX: u32 = 5;

static REFUSALS: [AtomicU32; CAPGATE_K_MAX as usize] = [
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
];

/// Allowed-through counters, per kind, so a ratio is available rather than
/// only an absolute. "0 refusals" reads identically to "this gate is not
/// wired"; "0 refusals out of 41,000 allowed" does not.
static ALLOWED: [AtomicU32; CAPGATE_K_MAX as usize] = [
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
];

/// Mirrors capgate_stats_t in proc/capgate.h. sizeof-locked on both sides.
#[repr(C)]
pub struct CapgateStats {
    pub refused: [u32; CAPGATE_K_MAX as usize],
    pub allowed: [u32; CAPGATE_K_MAX as usize],
}

#[no_mangle]
pub extern "C" fn capgate_note_refusal_rs(kind: u32) {
    if kind < CAPGATE_K_MAX {
        REFUSALS[kind as usize].fetch_add(1, Ordering::Relaxed);
    }
}

#[no_mangle]
pub extern "C" fn capgate_note_allowed_rs(kind: u32) {
    if kind < CAPGATE_K_MAX {
        ALLOWED[kind as usize].fetch_add(1, Ordering::Relaxed);
    }
}

#[no_mangle]
pub extern "C" fn capgate_refusals_rs(kind: u32) -> u32 {
    if kind < CAPGATE_K_MAX {
        REFUSALS[kind as usize].load(Ordering::Relaxed)
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn capgate_stats_rs(out: *mut CapgateStats) -> i32 {
    if out.is_null() {
        return -1;
    }
    let mut s = CapgateStats {
        refused: [0u32; CAPGATE_K_MAX as usize],
        allowed: [0u32; CAPGATE_K_MAX as usize],
    };
    for i in 0..(CAPGATE_K_MAX as usize) {
        s.refused[i] = REFUSALS[i].load(Ordering::Relaxed);
        s.allowed[i] = ALLOWED[i].load(Ordering::Relaxed);
    }
    // SAFETY: `out` is a kernel pointer supplied by the caller (proc/capgate.h
    // callers pass the address of a stack capgate_stats_t). One aligned write
    // of a #[repr(C)] POD whose layout is _Static_assert-locked against the C
    // definition.
    unsafe {
        ptr::write(out, s);
    }
    0
}

// ---------------------------------------------------------------------------
// RULE 1: is the caller the compositor
// ---------------------------------------------------------------------------

/// The pure rule, so it can be self-tested without touching the live latch.
///
/// `owner` is the framebuffer-ownership latch (0 == unclaimed), `caller` is
/// the asking pid. Both zeroes are refused explicitly rather than falling out
/// of the comparison: pid 0 is "no current process" to proc_current_pid(), and
/// an unclaimed latch is 0, so a bare `owner == caller` would answer YES to a
/// kernel-context caller on a machine with no compositor. That is the exact
/// shape of an ambient hole, in the gate meant to close one.
#[inline]
fn principal_ok(owner: u32, caller: u32) -> bool {
    owner != 0 && caller != 0 && owner == caller
}

/// Is `caller_pid` the latched compositor? 1/0.
///
/// NON-CLAIMING by construction: this reads fbown_is_owner_rs() and there is
/// no path from here to fbown_claim_rs(). See the header note on why that
/// distinction is the difference between closing a hole and opening one.
#[no_mangle]
pub extern "C" fn capgate_is_compositor_rs(caller_pid: u32) -> i32 {
    if caller_pid == 0 {
        return 0;
    }
    crate::fbown::fbown_is_owner_rs(caller_pid)
}

// ---------------------------------------------------------------------------
// RULE 2: does this Ring-3 caller own this handle
// ---------------------------------------------------------------------------

/// Ownership gate for a kernel-side handle addressed by a RAW INDEX from
/// Ring 3 (a TCP slot, an SHM region id). 1 = allowed, 0 = refused.
///
/// This is drivers/audio_pcm.c:171-196 with the reasoning intact, because that
/// is the one instance of this check in the tree that is correct, and copying
/// it is strictly better than reinventing it twice:
///
/// THREAD GROUP, NOT THREAD. A pthread in MayteraOS is a separate process_t
/// with its OWN pid (proc_clone does `child->pid = next_pid++`) that shares the
/// address space and carries the group leader in `tgid`. A pid-only gate
/// therefore means "the handle belongs to the one THREAD that opened it", so an
/// app that opens a socket on one thread and reads it from another gets a
/// refusal, with nothing in the failure that points at threads. audio_pcm.c
/// records that this was a live hazard for the Ring-3 DOS host, whose SB pump
/// only worked because open, write and close happened on one thread; moving the
/// open one frame outwards would have silenced it. Widening to the thread group
/// removes the landmine and is strictly no more permissive ACROSS processes,
/// which is what the gate is actually for.
///
/// UNOWNED IS REFUSED, NOT ALLOWED. `owner_pid == 0` means the handle was
/// created with no Ring-3 process current, i.e. it belongs to the KERNEL: the
/// in-kernel sshd's connections are exactly this case. proc_current_pid()
/// returns 0 when there is no current process, so an `owner_pid == 0 => allow`
/// default would hand every kernel-owned connection to any Ring-3 caller,
/// which is the precise defect being closed. Fail closed (principle 5).
///
/// `cur_tgid` is the caller's `tgid ? tgid : pid`, normalised by the C caller;
/// `owner_tgid` was normalised the same way when the handle was stamped.
#[no_mangle]
pub extern "C" fn capgate_owner_ok_rs(
    owner_pid: u32,
    owner_tgid: u32,
    cur_pid: u32,
    cur_tgid: u32,
) -> i32 {
    // Kernel-owned or unstamped: never reachable from Ring 3.
    if owner_pid == 0 {
        return 0;
    }
    // No current process: a kernel context has no business coming through the
    // Ring-3 chokepoint at all, and cannot be shown to own anything.
    if cur_pid == 0 {
        return 0;
    }
    // NORMALISE BOTH SIDES TO THE GROUP, THEN COMPARE ONCE. This is the
    // fdown.rs / fetchown.rs form rather than audio_pcm.c's
    // "pid matches OR tgid matches" disjunction. The two agree on every input
    // a real system can produce, because a pid belongs to exactly one thread
    // group, and the normalised form is strictly the tighter of the two: the
    // disjunction would also admit a caller whose pid equals the owner's pid
    // while their groups differ, which is unrepresentable but is the sort of
    // "cannot happen" that a gate should not be relying on.
    let g_owner = capgate_tgid_of_rs(owner_pid, owner_tgid);
    let g_cur = capgate_tgid_of_rs(cur_pid, cur_tgid);
    if g_owner != 0 && g_owner == g_cur {
        1
    } else {
        0
    }
}

/// Normalise a (pid, tgid) pair to the group identity, exactly as
/// audio_pcm.c's `cur->tgid ? cur->tgid : cur->pid`. Exported so the C stamp
/// sites and the C check sites cannot disagree about what "thread group" means
/// (a disagreement that would show up as an intermittent, thread-dependent
/// refusal, i.e. the hardest possible way to find it).
#[no_mangle]
pub extern "C" fn capgate_tgid_of_rs(pid: u32, tgid: u32) -> u32 {
    if tgid != 0 {
        tgid
    } else {
        pid
    }
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
// Every assertion below is a REFUSAL, because a refusal that was never
// exercised is a claim and not a control (proc/elevate.c's selftest says the
// same thing and for the same reason). Returns 0 on pass, else the case
// number, so a failure names itself on a machine with no serial port.
//
// Touches NO global state: it calls only the pure rules with a vector table,
// so running it at boot cannot perturb the framebuffer latch, the refusal
// ledger, or anything else. That property is what makes it safe to run on
// every boot including the golden.

#[no_mangle]
pub extern "C" fn capgate_selftest_rs() -> u32 {
    // --- RULE 1: compositor principal --------------------------------------
    // The compositor itself is allowed.
    if !principal_ok(7, 7) {
        return 1;
    }
    // A different live process is refused, which is defects 1 and 2.
    if principal_ok(7, 9) {
        return 2;
    }
    // THE CASE THAT MATTERS: an UNCLAIMED latch must not admit anybody. If
    // this ever returns true, every process on a machine with no compositor
    // yet is "the compositor", which is worse than the hole being closed.
    if principal_ok(0, 9) {
        return 3;
    }
    // A kernel context (caller pid 0) is not the compositor either, including
    // against an unclaimed latch, where a bare equality would have said yes.
    if principal_ok(0, 0) {
        return 4;
    }
    if principal_ok(7, 0) {
        return 5;
    }

    // --- RULE 2: handle ownership ------------------------------------------
    // The opening thread reaches its own handle.
    if capgate_owner_ok_rs(11, 11, 11, 11) != 1 {
        return 6;
    }
    // A SIBLING PTHREAD reaches it. This is the case a pid-only gate breaks,
    // and breaking it looks like "sockets randomly fail in threaded apps".
    // pid 12 is a different process_t from pid 11 but shares tgid 11.
    if capgate_owner_ok_rs(11, 11, 12, 11) != 1 {
        return 7;
    }
    // A handle opened ON a worker thread is reachable from the group leader,
    // i.e. the widening works in both directions rather than only outwards.
    if capgate_owner_ok_rs(12, 11, 11, 11) != 1 {
        return 8;
    }
    // AN UNRELATED PROCESS IS REFUSED. This is defects 3 and 4: pid 40 in its
    // own thread group must not reach pid 11's socket or shared memory.
    if capgate_owner_ok_rs(11, 11, 40, 40) != 0 {
        return 9;
    }
    // A KERNEL-OWNED handle (owner_pid 0, the in-kernel sshd's connections) is
    // refused to every Ring-3 caller. An allow-on-unowned default would have
    // handed sshd's sockets to anyone.
    if capgate_owner_ok_rs(0, 0, 40, 40) != 0 {
        return 10;
    }
    // ...and is still refused when the CALLER is also unidentified, so two
    // zeroes cannot compare equal into an allow.
    if capgate_owner_ok_rs(0, 0, 0, 0) != 0 {
        return 11;
    }
    // A caller with no current process is refused against a real owner.
    if capgate_owner_ok_rs(11, 11, 0, 0) != 0 {
        return 12;
    }
    // Zero tgids must not collide into a match: owner in group 11, caller an
    // unrelated pid 40 whose tgid normalised to 40.
    if capgate_owner_ok_rs(11, 11, 40, 0) != 0 {
        return 13;
    }

    // --- tgid normalisation, shared by the stamp and the check -------------
    if capgate_tgid_of_rs(11, 0) != 11 {
        return 14;
    }
    if capgate_tgid_of_rs(12, 11) != 11 {
        return 15;
    }

    0
}
