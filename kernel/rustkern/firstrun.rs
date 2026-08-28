// rustkern/firstrun.rs - #229: THE chokepoint for first-run (OOBE) state.
//
// New kernel logic, so Rust per the 2026-07-16 rule. There is no C twin to
// strangle: the thing this replaces was not kernel code at all, it was four
// open(O_CREAT) calls made by a Ring-3 app against a directory it has no
// business writing.
//
// ===========================================================================
// WHAT WENT WRONG, MEASURED (#226, golden build 2011, by typing real keys)
// ---------------------------------------------------------------------------
// A virgin image could not reach the desktop. The kernel half was fine: the
// create-account screen took the fields, made the account and handed over the
// session. Then /APPS/SETUP opened over that session and every durable thing
// it does is a write under /CONFIG:
//
//     /CONFIG/SETUPDONE    "the machine has been set up"
//     /CONFIG/SETUPSKIP    "let the desktop chrome through for this boot"
//     /CONFIG/SETUPNEW     "the machine just changed hands; log out"
//     /CONFIG/NETIP.CFG    the static IP the Network page collects
//
// /CONFIG is root-owned 0711 (fs/perms.c, #745) and the session is uid 1000,
// so creating a NAME in it needs write on the directory, which uid 1000 does
// not have and MUST NOT have: /CONFIG holds SHADOW, AUTHKEYS, SSHD.CFG and the
// owner's API keys. The kernel printed its own refusal every time,
// [PERMS-DENY] proc=SETUP uid=1000 gid=1000 want=-wx path=/CONFIG, and the
// wizard reported "Could not reach the desktop; try again."
//
// THE ESCAPE HATCH WAS THE FIRST CASUALTY, WHICH IS WHY THIS IS NOT COSMETIC.
// The wizard is modal on first boot (the compositor gates the taskbar, the
// desktop icons and the start menu off while the machine is unconfigured), so
// "Skip to Desktop" is the ONLY way out of a step that fails. Implementing
// that escape as a FILE WRITE made the escape share a failure mode with the
// thing it was there to escape from. A machine that cannot be set up and
// cannot be escaped is a dead end.
//
// ===========================================================================
// THE SHAPE, AND WHY THIS ONE
// ---------------------------------------------------------------------------
// Three options were on the table. This is why the other two lost.
//
//   RELAX /CONFIG. Rejected. The directory mode is not the problem; it is the
//     only thing standing between a Ring-3 process and SHADOW. #745 spent a
//     whole change getting 0755 down to 0711 for the enumeration alone. Going
//     the other way to let a wizard drop a two-byte marker would trade the
//     password database for a flag file.
//
//   MOVE THE STATE SOMEWHERE THE SESSION OWNS. Right answer for PER-USER
//     state, and it is already the rule (userland/libc/userconf.c, #683): the
//     wizard's own <home>/CONFIG/SETUPUSR marker goes there and needs nothing
//     from this file. But SETUPDONE is a statement about THE MACHINE, read by
//     the compositor of every user who ever signs in. Putting a machine fact
//     in one user's home makes that user's disk answer for everybody, which is
//     precisely the defect #126 was filed for.
//
//   A KERNEL CHOKEPOINT. This. It matches how the rest of the machine-scope
//     config already works: /CONFIG/LOGIN.CFG is not written by Ring 3 either,
//     it is written by sys_set_autologin()/sys_set_login_mode() from Ring 0
//     behind one authorization function (proc/syscall.c login_cfg_authorize).
//     The wizard's own mandatory step, sys_firstboot_admin(), is already a
//     syscall for the same reason. First-run state was the odd one out.
//
// ===========================================================================
// AND TWO OF THE FOUR FILES SHOULD NEVER HAVE BEEN FILES
// ---------------------------------------------------------------------------
// SETUPSKIP and SETUPNEW are not configuration. They are one-way signals from
// the wizard to the compositor, both meaningful for exactly one boot. Storing
// a per-boot signal on persistent media creates a stale-marker bug by
// construction, and the tree paid for that twice:
//
//   #136  a SETUPSKIP left by last boot's skip cleared g_setup_pending within
//         ~330ms of the next boot and hid the wizard that was just spawned.
//   #203  a SETUPNEW left behind logged the person straight back out of a
//         machine that had not changed hands.
//
// Both were fixed by adding an unlink at the compositor's spawn site. Those
// unlinks are writes to /CONFIG too, so on a non-root session BOTH cleanups
// were themselves refused, and the fix for the stale-marker bug had the same
// hole as the bug. Here the flags are two bits of kernel state that start
// clear at boot because .bss starts clear at boot. The stale-marker class
// stops existing rather than being cleaned up. That is the difference between
// fixing the instance and fixing the mechanism.
//
// ===========================================================================
// WHAT IS DELIBERATELY NOT HERE
// ---------------------------------------------------------------------------
// /CONFIG/NETIP.CFG. It is the fourth refused write and it is the one this
// module does NOT take, because the Network page is compile-disabled
// (NETWORK_PAGE_ENABLED = false in userland/apps/setup/main.rs, owner request
// 2026-08-18) and apply() only writes the file when the page cleared the DHCP
// default, which nothing can now do. An op for it would ship with zero
// callers, and this tree has a written rule about that: a feature with no
// caller never runs and cannot be said to work. WHEN THE PAGE IS TURNED BACK
// ON, add FR_SET_NETIP here with a dotted-quad validator and a canonical
// re-serialisation; do NOT reach for open() again.
//
// ===========================================================================
// AUTHORIZATION, STATED EXACTLY, INCLUDING WHAT IT DOES NOT BUY
// ---------------------------------------------------------------------------
// FR_MARK_DONE is the only op that touches the disk, and it is gated on a fact
// the kernel can check rather than on a claim the caller makes: THE MACHINE
// MUST ALREADY HAVE AT LEAST ONE ACTIVE ACCOUNT. That is precisely what the
// marker asserts ("the one mandatory step of setup has been completed"), so
// the gate is the statement itself rather than a ceremony around it. On a
// machine with no owner yet - the only state where suppressing the wizard
// could strand somebody - it is refused outright.
//
// HONEST LIMIT: on a machine that DOES have accounts, any Ring-3 process can
// call FR_MARK_DONE and suppress the first-run wizard. The wizard at that
// point offers optional personalisation only (the account already exists, and
// per-user state is keyed off <home>/CONFIG/SETUPUSR, which this op does not
// touch), so the worst outcome is a wizard that does not appear. That is a
// nuisance, not a privilege boundary, and it is a far smaller surface than the
// alternative it replaces, which was write access to the directory holding
// SHADOW. Every call is recorded on serial and in the boot log with the
// caller's uid, so it is at least never silent.
//
// The four per-boot ops are ungated on purpose. They mutate two bits of RAM,
// they are read only by a compositor that is already gating on "this machine
// is unconfigured", and the SKIP bit's whole job is to be the escape hatch
// that CANNOT FAIL. Putting a policy decision in front of the escape hatch is
// how the escape hatch acquires a failure mode, which is the bug being fixed.

use core::sync::atomic::{AtomicU32, Ordering};

// ---------------------------------------------------------------------------
// OPS. MIRRORED in kernel/proc/syscall.h (FR_*), userland/libc/syscall.h and
// the private const block of userland/apps/setup/main.rs. Locked by
// firstrun_selftest_rs() below, which asserts the numeric values on THIS
// build: a no_std Rust app cannot include the C header, so it keeps its own
// copy, and that fifth copy going stale is a defect this tree has already
// shipped once (see kernel/tools/syscall-number-lint rule 5).
// ---------------------------------------------------------------------------
pub const FR_MARK_DONE: i64 = 0; // durable: /CONFIG/SETUPDONE, written by Ring 0
pub const FR_SKIP_SET: i64 = 1; // per-boot: the wizard's escape hatch
pub const FR_SKIP_GET: i64 = 2; // per-boot: -> 1 if escaped this boot
pub const FR_SKIP_CLEAR: i64 = 3; // per-boot: the compositor arms a fresh first run
pub const FR_HANDOVER_SET: i64 = 4; // per-boot: the machine just changed hands
pub const FR_HANDOVER_TAKE: i64 = 5; // per-boot: consuming read of the above
pub const FR_BOOTSTRAP_QUERY: i64 = 6; // #OOBEAUTH: see firstboot_bootstrap_ok_rs() below

// ---------------------------------------------------------------------------
// VERDICTS the C shim acts on. Rust owns the decision; C owns the I/O, because
// the FAT write and the account table are C. Nothing below decides bytes on a
// disk, and nothing in C decides policy.
// ---------------------------------------------------------------------------
pub const FRV_FALSE: i64 = 0; // done, answer 0
pub const FRV_TRUE: i64 = 1; // done, answer 1
pub const FRV_WRITE_DONE: i64 = 2; // C: write /CONFIG/SETUPDONE, then answer 0
pub const FRV_EINVAL: i64 = -1; // no such op
pub const FRV_ENOACCOUNT: i64 = -2; // FR_MARK_DONE on a machine with no owner

// Per-boot signals. AtomicU32 rather than a plain static because a syscall on
// one core and the compositor's poll on another are genuinely concurrent, and
// because taking a lock for one bit would be the wrong trade in a path the
// compositor calls three times a second. Relaxed ordering is correct here and
// is not laziness: each flag is a single independent bit with no other state
// hanging off it, so there is nothing for an acquire/release pair to publish.
static SKIP: AtomicU32 = AtomicU32::new(0);
static HANDOVER: AtomicU32 = AtomicU32::new(0);

/// THE decision. `have_account` is 1 when the machine already has at least one
/// active account (C: users_count_active() > 0); it is the only fact this
/// module cannot establish for itself.
///
/// Returns one of the FRV_* verdicts above. It never blocks, never allocates
/// and never touches a device, so it is safe from any context a syscall can
/// arrive in.
#[no_mangle]
pub extern "C" fn firstrun_op_rs(op: i64, have_account: i32) -> i64 {
    match op {
        FR_MARK_DONE => {
            if have_account == 0 {
                // Refused, and this is the case that matters: a machine with
                // no owner is the one state where a forged "setup is done"
                // could strand somebody at a desktop they cannot sign back
                // into.
                FRV_ENOACCOUNT
            } else {
                FRV_WRITE_DONE
            }
        }
        FR_SKIP_SET => {
            SKIP.store(1, Ordering::Relaxed);
            FRV_FALSE
        }
        FR_SKIP_GET => {
            if SKIP.load(Ordering::Relaxed) != 0 {
                FRV_TRUE
            } else {
                FRV_FALSE
            }
        }
        FR_SKIP_CLEAR => {
            SKIP.store(0, Ordering::Relaxed);
            FRV_FALSE
        }
        FR_HANDOVER_SET => {
            HANDOVER.store(1, Ordering::Relaxed);
            FRV_FALSE
        }
        FR_HANDOVER_TAKE => {
            // CONSUMING, and it must be atomic: the compositor polls this
            // every ~500ms and a read-then-clear pair could hand the same
            // handover to two polls, which for this signal means logging the
            // session out twice.
            if HANDOVER.swap(0, Ordering::Relaxed) != 0 {
                FRV_TRUE
            } else {
                FRV_FALSE
            }
        }
        _ => FRV_EINVAL,
    }
}

// ===========================================================================
// #OOBEAUTH (owner decision, 2026-08-23): THE FIRST-BOOT BOOTSTRAP PRIVILEGE
// BOUNDARY.
// ---------------------------------------------------------------------------
// WHY THIS EXISTS. #229 (above) made SYS_FIRSTBOOT_ADMIN - the call that
// creates the first interactive account and sets root's own password -
// root-only (`if p->euid != 0`), correctly: it is the one call in this OS
// that mints uid 0's credential. But #202/#219 had already removed every
// shipped default account, so a virgin image has NO root either, and nothing
// can ever BECOME root through this call to satisfy that gate. The kernel's
// own Ring-0 login gate (gui/login.c) used to paper over this by drawing its
// OWN "Create your account" form and calling users_create_first_admin()
// directly from Ring 0, where there is no euid check to satisfy. That put
// TWO implementations of the same screen in the tree: the kernel's, which
// ran, and the userland OOBE wizard's PG_ACCOUNT page
// (userland/apps/setup/main.rs, dk_draw_account()), which could never be
// reached because the session the kernel gate handed out was uid 1000, not
// 0. The owner's decision was to delete the duplicate and make the wizard's
// page the reachable one, which means SOMETHING has to open the door
// SYS_FIRSTBOOT_ADMIN currently keeps shut for uid 1000.
//
// THE BOUNDARY, stated precisely. `firstboot_bootstrap_ok_rs()` answers ONE
// question: may THIS caller, right now, act as if it were root for the sole
// purpose of SYS_FIRSTBOOT_ADMIN? Yes, if and only if ALL of:
//
//   1. have_account == 0        the account table is genuinely empty. This
//                                is a FACT the kernel checks (users_count_
//                                active()), never a claim the caller makes.
//                                The instant users_create_first_admin()
//                                succeeds this becomes 1, so the SAME call
//                                that uses the exception is what revokes it:
//                                there is no window after account creation
//                                in which it is still open.
//   2. fb_owner_pid != 0        a compositor exists and has latched the
//                                framebuffer (gui/fb_syscall.c is_compositor()
//                                / fbown_owner_rs()). That latch is already
//                                the trust anchor proc/elevate.c's App Store
//                                elevation prompt relies on: exactly one pid
//                                on the whole machine can ever hold it, for
//                                the life of the boot, and no other process
//                                can forge it.
//   3. caller_ppid == fb_owner_pid   the CALLER is a direct child of THAT
//                                process. In the shipped boot sequence this
//                                is /APPS/SETUP, spawned by the compositor
//                                (userland/apps/compositor/main.c) after the
//                                bootstrap login handoff in gui/login.c. It
//                                is not "any process while the table is
//                                empty": a background service or anything
//                                else that happened to start during that
//                                window, and is NOT a child of the
//                                compositor, is refused exactly like any
//                                other non-root caller.
//
// WHAT THIS IS NOT. It is not a general elevation: it grants exactly one
// syscall, not a uid, not a path prefix, not a time-boxed grant on the
// process_t (contrast proc/elevate.c, which IS all three of those, for a
// DIFFERENT problem - authenticating an EXISTING account for a system-wide
// install path grant. There is no account to authenticate against here,
// which is precisely why that machinery does not fit and this one is
// smaller). The calling process's actual uid/gid/euid/egid are left exactly
// as gui/login.c set them (FIRST_ADMIN_UID, 1000 - see sessionid.rs) for the
// WHOLE bootstrap session; this function grants nothing to that uid, it
// grants a one-question exception to a fact about the process tree.
//
// HONEST COST. Before this, a virgin machine ran a small Ring-0-drawn form
// and nothing else with any code in it. Now it runs the compositor and the
// full OOBE wizard - real, larger Ring-3 code - before any account exists.
// That is a bigger pre-auth attack surface by any measure. What is bounded
// is what that surface can DO: uid 1000 with no matching account owns
// nothing and can create nothing outside its own (nonexistent) home, and the
// one door this function can open leads to exactly one syscall, closed for
// good the instant it is used once.
// ===========================================================================

/// See the module-level comment above for the full rationale. Returns 1 (may
/// proceed) or 0 (refused). Never blocks, never allocates, reads only its
/// arguments: the caller (proc/syscall.c) supplies every fact from process_t
/// and users_count_active(), because those live in C and this module's job
/// is the DECISION, not the lookup.
#[no_mangle]
pub extern "C" fn firstboot_bootstrap_ok_rs(
    // Not part of the test (see below); kept in the signature so a caller can
    // log which pid asked without a second FFI call, and so this predicate's
    // shape matches every other C-glue-supplies-the-facts function in this
    // file.
    _caller_pid: u32,
    caller_ppid: u32,
    fb_owner_pid: u32,
    have_account: u32,
) -> i32 {
    if have_account != 0 {
        return 0;
    }
    if fb_owner_pid == 0 {
        return 0;
    }
    // The compositor itself is not this session's exception: only a process
    // IT SPAWNS (the wizard) ever calls SYS_FIRSTBOOT_ADMIN, so the test is a
    // parent/child hop, not "== fb_owner_pid OR child of it".
    (caller_ppid == fb_owner_pid) as i32
}

// ---------------------------------------------------------------------------
// BOOT SELF-TEST
// ---------------------------------------------------------------------------
// blame.md's most repeated lesson is that in-tree prose lies. Every assertion
// below is about a REFUSAL or a CONSUMING read, which are exactly the
// behaviours a normal boot never exercises and therefore the ones that rot
// unnoticed.
//
// It runs against the live statics and puts them back the way it found them,
// so arming it at boot cannot itself change what the compositor sees.
//
// Returns a bit mask; 0 is a pass. `out_checks`, when non-null, receives the
// number of assertions made, so a test that silently stopped testing is
// visible as a dropping count rather than as a continuing PASS.
#[no_mangle]
pub extern "C" fn firstrun_selftest_rs(out_checks: *mut u32) -> u32 {
    let mut fails: u32 = 0;
    let mut checks: u32 = 0;
    let mut bit: u32 = 1;
    // A plain fn, not a closure: it takes every piece of state it touches, so
    // there is no capture to reason about and the call sites below read as the
    // assertions they are.
    //
    // The mask has 32 bits and there are 25 assertions. If that ever grows past
    // 32 the shift would run off the end and later failures would OR in zero,
    // which is a self-test that stops reporting while still printing PASS. So
    // the overflow sets the TOP bit instead: a mask of 0x80000000 with no other
    // bit set reads as "this self-test outgrew its mask", not as a pass.
    fn check(ok: bool, fails: &mut u32, checks: &mut u32, bit: &mut u32) {
        *checks += 1;
        if *bit == 0 {
            *fails |= 0x8000_0000;
            return;
        }
        if !ok {
            *fails |= *bit;
        }
        *bit <<= 1;
    }

    let saved_skip = SKIP.load(Ordering::Relaxed);
    let saved_handover = HANDOVER.load(Ordering::Relaxed);

    // ---- the numbers the five copies must agree on -------------------------
    check(FR_MARK_DONE == 0, &mut fails, &mut checks, &mut bit);
    check(FR_SKIP_SET == 1, &mut fails, &mut checks, &mut bit);
    check(FR_SKIP_GET == 2, &mut fails, &mut checks, &mut bit);
    check(FR_SKIP_CLEAR == 3, &mut fails, &mut checks, &mut bit);
    check(FR_HANDOVER_SET == 4, &mut fails, &mut checks, &mut bit);
    check(FR_HANDOVER_TAKE == 5, &mut fails, &mut checks, &mut bit);
    check(FR_BOOTSTRAP_QUERY == 6, &mut fails, &mut checks, &mut bit);

    // ---- the durable op is REFUSED with no owner, and only then ------------
    check(
        firstrun_op_rs(FR_MARK_DONE, 0) == FRV_ENOACCOUNT,
        &mut fails, &mut checks, &mut bit,
    );
    check(
        firstrun_op_rs(FR_MARK_DONE, 1) == FRV_WRITE_DONE,
        &mut fails, &mut checks, &mut bit,
    );

    // ---- an unknown op is a refusal, never a silent no-op ------------------
    // A no-op would let a stale userland copy of the op numbers "succeed" at
    // doing nothing, which is the exact failure the syscall-number lint's
    // rule 5 exists for. FR_BOOTSTRAP_QUERY (6) is deliberately included here:
    // it is a LEGAL op number for SYS_FIRSTRUN, but proc/syscall.c intercepts
    // it BEFORE calling firstrun_op_rs() (it needs pid/ppid/fb-owner, which
    // this function's (op, have_account) shape has no room for), so as far as
    // firstrun_op_rs() itself is concerned 6 is still simply unrecognised.
    check(firstrun_op_rs(-1, 1) == FRV_EINVAL, &mut fails, &mut checks, &mut bit);
    check(firstrun_op_rs(6, 1) == FRV_EINVAL, &mut fails, &mut checks, &mut bit);
    check(
        firstrun_op_rs(0x7fff_ffff, 1) == FRV_EINVAL,
        &mut fails, &mut checks, &mut bit,
    );

    // ---- THE FIRST-BOOT BOOTSTRAP PREDICATE, every dimension it checks -----
    // Numbers are arbitrary but distinct: a real pid, a real fb-owner pid,
    // and a THIRD number that must never accidentally equal either.
    const PID: u32 = 42;
    const PPID: u32 = 7;
    const FBOWNER: u32 = 7;   // caller's ppid IS the compositor: the happy path
    const OTHER: u32 = 99;    // some unrelated pid
    // The happy path: virgin table, caller is a direct child of the compositor.
    check(
        firstboot_bootstrap_ok_rs(PID, PPID, FBOWNER, 0) == 1,
        &mut fails, &mut checks, &mut bit,
    );
    // An account exists: refused, even with the right lineage. This is the
    // check that MUST fire the instant SYS_FIRSTBOOT_ADMIN succeeds once.
    check(
        firstboot_bootstrap_ok_rs(PID, PPID, FBOWNER, 1) == 0,
        &mut fails, &mut checks, &mut bit,
    );
    // No compositor has ever mapped the framebuffer yet: refused. A caller
    // cannot manufacture eligibility before a compositor exists to be a
    // child of.
    check(
        firstboot_bootstrap_ok_rs(PID, PPID, 0, 0) == 0,
        &mut fails, &mut checks, &mut bit,
    );
    // Wrong lineage: a process that is NOT a child of the compositor (e.g. a
    // background service started during the same boot) gets nothing merely
    // because the table happens to still be empty.
    check(
        firstboot_bootstrap_ok_rs(PID, OTHER, FBOWNER, 0) == 0,
        &mut fails, &mut checks, &mut bit,
    );
    // The compositor calling THIS DIRECTLY (caller_pid == fb_owner_pid) is
    // NOT exempted: its own parent is whatever spawned it (some init pid, not
    // itself), so the lineage check refuses it exactly like any other caller
    // whose ppid is not the compositor. Only a CHILD of the compositor passes
    // - see the happy-path check above.
    const INIT_PID: u32 = 1;
    check(
        firstboot_bootstrap_ok_rs(FBOWNER, INIT_PID, FBOWNER, 0) == 0,
        &mut fails, &mut checks, &mut bit,
    );

    // ---- THE ESCAPE HATCH CANNOT FAIL --------------------------------------
    // The whole point of moving it off the disk. Set it with no account, on a
    // machine in any state, and it is still set.
    SKIP.store(0, Ordering::Relaxed);
    check(firstrun_op_rs(FR_SKIP_GET, 0) == FRV_FALSE, &mut fails, &mut checks, &mut bit);
    check(firstrun_op_rs(FR_SKIP_SET, 0) == FRV_FALSE, &mut fails, &mut checks, &mut bit);
    check(firstrun_op_rs(FR_SKIP_GET, 0) == FRV_TRUE, &mut fails, &mut checks, &mut bit);
    // Idempotent: a person who presses F9 twice must not toggle it back off.
    check(firstrun_op_rs(FR_SKIP_SET, 0) == FRV_FALSE, &mut fails, &mut checks, &mut bit);
    check(firstrun_op_rs(FR_SKIP_GET, 0) == FRV_TRUE, &mut fails, &mut checks, &mut bit);
    // Non-consuming: the compositor polls it three times a second for the rest
    // of the boot and every poll must still say yes.
    check(firstrun_op_rs(FR_SKIP_GET, 0) == FRV_TRUE, &mut fails, &mut checks, &mut bit);
    // And the compositor's arm-a-fresh-first-run clear is the #136 cleanup,
    // now unable to fail on a permission.
    check(firstrun_op_rs(FR_SKIP_CLEAR, 0) == FRV_FALSE, &mut fails, &mut checks, &mut bit);
    check(firstrun_op_rs(FR_SKIP_GET, 0) == FRV_FALSE, &mut fails, &mut checks, &mut bit);

    // ---- the handover is CONSUMED, exactly once ----------------------------
    HANDOVER.store(0, Ordering::Relaxed);
    check(firstrun_op_rs(FR_HANDOVER_TAKE, 1) == FRV_FALSE, &mut fails, &mut checks, &mut bit);
    check(firstrun_op_rs(FR_HANDOVER_SET, 1) == FRV_FALSE, &mut fails, &mut checks, &mut bit);
    check(firstrun_op_rs(FR_HANDOVER_TAKE, 1) == FRV_TRUE, &mut fails, &mut checks, &mut bit);
    // The second take is the one that matters: #203's failure mode is a
    // handover that fires again and logs somebody out of a machine that did
    // not change hands.
    check(firstrun_op_rs(FR_HANDOVER_TAKE, 1) == FRV_FALSE, &mut fails, &mut checks, &mut bit);

    // ---- the two signals are independent -----------------------------------
    SKIP.store(0, Ordering::Relaxed);
    HANDOVER.store(0, Ordering::Relaxed);
    let _ = firstrun_op_rs(FR_SKIP_SET, 1);
    check(firstrun_op_rs(FR_HANDOVER_TAKE, 1) == FRV_FALSE, &mut fails, &mut checks, &mut bit);
    check(firstrun_op_rs(FR_SKIP_GET, 1) == FRV_TRUE, &mut fails, &mut checks, &mut bit);

    SKIP.store(saved_skip, Ordering::Relaxed);
    HANDOVER.store(saved_handover, Ordering::Relaxed);

    if !out_checks.is_null() {
        unsafe {
            // SAFETY: the caller is kernel C (main.c) passing the address of
            // one of its own uint32_t locals. The pointer is checked non-null
            // above; it is never a Ring-3 address, because this function is
            // not reachable from a syscall.
            *out_checks = checks;
        }
    }
    fails
}
