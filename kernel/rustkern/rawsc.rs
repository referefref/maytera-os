// rawsc.rs - #DOSRING3 Stage 1: focus-scoped RAW SCANCODE delivery to Ring 3.
//
// WHY THIS EXISTS. A DOS guest's INT 9 handler reads port 0x60 directly and
// maintains its own Keyboard[] scancode array (id Software's Galaxy engine, so
// Commander Keen 4-6, does exactly this). Cooked key events cannot serve it:
// it needs the raw set-1 MAKE and BREAK bytes, including the 0xE0 prefix.
//
// The kernel already has such a tap: g_dos_scancode_tap in cpu/isr.c, which
// splices the DOS layer directly into the live keyboard path. That works only
// because the DOS interpreter is IN THE KERNEL. Moving the interpreter to
// Ring 3 (#DOSRING3) needs the same bytes delivered across the syscall
// boundary, and delivering them safely is the whole content of this module.
//
// WHY IT IS NOT A PRIVILEGE ESCALATION. A raw scancode is not more privileged
// than the cooked key event SYS_GET_KEYBOARD already hands to Ring 3 today; it
// is the SAME keystroke in a less processed form. The risk is not the encoding,
// it is SCOPE: a keylogger would want the bytes while some OTHER window has
// focus. So delivery is bound to both conditions at once:
//
//   1. the subscriber must OWN the window handle it names, and
//   2. that window must currently HAVE FOCUS.
//
// Both are re-checked by the syscall on every single drain, never latched.
// Losing focus therefore stops delivery at the next call with no teardown step
// to forget, and the ring is flushed on the focus edge so nothing typed into
// another window can be read out afterwards. That is a STRICTER contract than
// the in-kernel tap it replaces: g_dos_scancode_tap is a global on/off flag
// with no window scoping at all, and this module is what lets that splice be
// removed from the ISR once the Ring-3 host is the only DOS host.
//
// WHY RUST. CLAUDE.md's standing rule: new kernel code is Rust unless there is
// a stated performance or entanglement reason. This is a ring buffer and an
// arming policy - pure integer state with no paging, asm or FPU involvement -
// so there is none.
//
// NO FLOAT: the kernel target is x86_64-unknown-none (soft-float, SSE off).

use core::sync::atomic::{AtomicU32, AtomicI32, Ordering};

const RING: usize = 256;

// The bytes. AtomicU32 cells rather than a plain array: the producer runs in
// hard IRQ context (PS/2 IRQ1) and can preempt another producer running in a
// thread (USB HID, Bluetooth HID, the #334 test channel), exactly the
// multi-producer situation #763 found on the DOS ring.
static CELL: [AtomicU32; RING] = {
    const Z: AtomicU32 = AtomicU32::new(0);
    [Z; RING]
};
static RD: AtomicU32 = AtomicU32::new(0);
static WR: AtomicU32 = AtomicU32::new(0);

// Subscription. 0 = nobody. Written only by the syscall path.
static SUB_PID: AtomicU32 = AtomicU32::new(0);
static SUB_WIN: AtomicI32 = AtomicI32::new(-1);

// Census, so a silent delivery failure is visible rather than inferred.
static PUSHED:  AtomicU32 = AtomicU32::new(0);
static DROPPED: AtomicU32 = AtomicU32::new(0);
static DRAINED: AtomicU32 = AtomicU32::new(0);

/// Is anyone subscribed? The keyboard path calls this on EVERY scancode, so it
/// is one relaxed load and nothing else.
#[no_mangle]
pub extern "C" fn rawsc_armed_rs() -> i32 {
    if SUB_PID.load(Ordering::Relaxed) != 0 { 1 } else { 0 }
}

/// Claim the subscription for (pid, handle). Idempotent: re-arming the same
/// pair is a no-op, so the host can call it on every drain and never needs a
/// separate setup step that could be missed. A DIFFERENT pid taking over
/// flushes the ring, so bytes typed while the previous owner was focused can
/// never be handed to the new one.
#[no_mangle]
pub extern "C" fn rawsc_arm_rs(pid: u32, handle: i32) {
    if pid == 0 { return; }
    let prev = SUB_PID.swap(pid, Ordering::AcqRel);
    let prevw = SUB_WIN.swap(handle, Ordering::AcqRel);
    if prev != pid || prevw != handle {
        rawsc_clear_rs();
    }
}

/// Drop the subscription if `pid` holds it. Called on focus loss and at
/// process exit. Safe to call for a pid that holds nothing.
#[no_mangle]
pub extern "C" fn rawsc_disarm_rs(pid: u32) {
    if pid == 0 { return; }
    if SUB_PID.load(Ordering::Acquire) != pid { return; }
    SUB_PID.store(0, Ordering::Release);
    SUB_WIN.store(-1, Ordering::Release);
    rawsc_clear_rs();
}

/// Discard everything buffered. Called on every focus edge and takeover.
#[no_mangle]
pub extern "C" fn rawsc_clear_rs() {
    let w = WR.load(Ordering::Acquire);
    RD.store(w, Ordering::Release);
}

/// Producer. Called from keyboard_process_scancode(), the ONE function every
/// scancode source in the kernel funnels through (PS/2 IRQ1, the polled i8042,
/// USB HID, Bluetooth HID and the #334 test channel), so a Ring-3 DOS guest
/// sees keys from all five by construction rather than by someone remembering
/// to add a call. Drops the byte when nobody is subscribed.
#[no_mangle]
pub extern "C" fn rawsc_push_rs(b: u8) {
    if SUB_PID.load(Ordering::Relaxed) == 0 { return; }
    // Multi-producer safe: claim a slot with a CAS on the write index, then
    // publish the byte into it. A full ring drops the NEWEST byte, matching
    // dos_sc_push()'s behaviour in cpu/isr.c so the two taps cannot disagree
    // about what a guest sees under overflow.
    loop {
        let w = WR.load(Ordering::Acquire);
        let nx = (w + 1) % (RING as u32);
        if nx == RD.load(Ordering::Acquire) {
            DROPPED.fetch_add(1, Ordering::Relaxed);
            return;
        }
        if WR.compare_exchange(w, nx, Ordering::AcqRel, Ordering::Acquire).is_ok() {
            CELL[w as usize].store(b as u32, Ordering::Release);
            PUSHED.fetch_add(1, Ordering::Relaxed);
            return;
        }
    }
}

/// Consumer. Copies up to `cap` bytes into a KERNEL buffer and returns the
/// count. The caller is responsible for the Ring-3 copy and for having checked
/// ownership and focus first.
///
/// # Safety
/// `out` must be a writable kernel buffer of at least `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn rawsc_drain_rs(out: *mut u8, cap: u32) -> u32 {
    if out.is_null() || cap == 0 { return 0; }
    let mut n: u32 = 0;
    while n < cap {
        let r = RD.load(Ordering::Acquire);
        if r == WR.load(Ordering::Acquire) { break; }
        let b = CELL[r as usize].load(Ordering::Acquire) as u8;
        RD.store((r + 1) % (RING as u32), Ordering::Release);
        unsafe { *out.add(n as usize) = b; }
        n += 1;
    }
    if n > 0 { DRAINED.fetch_add(n, Ordering::Relaxed); }
    n
}

/// pushed / dropped / drained, for the boot report.
#[no_mangle]
pub extern "C" fn rawsc_census_rs(pushed: *mut u32, dropped: *mut u32, drained: *mut u32) {
    unsafe {
        if !pushed.is_null()  { *pushed  = PUSHED.load(Ordering::Relaxed); }
        if !dropped.is_null() { *dropped = DROPPED.load(Ordering::Relaxed); }
        if !drained.is_null() { *drained = DRAINED.load(Ordering::Relaxed); }
    }
}

/// Self-test with a NEGATIVE ARM, because a positive-only test cannot tell a
/// gate from a wire (blame.md 2026-08-29, the BKL hold-sum lesson: an
/// instrument that simply accumulated would have passed a positive check).
///
/// Arm 1 (negative): with NOBODY subscribed, push a byte and require that
/// draining yields nothing. A build where rawsc_push_rs ignored the
/// subscription would deliver it, and only this arm can see that.
/// Arm 2 (positive): subscribe, push a known 3-byte sequence, require exactly
/// those 3 bytes back in order.
/// Arm 3 (negative): disarm, push, require nothing again - proving the gate
/// closes as well as opens.
///
/// Returns 1 on pass, 0 on failure. Restores the previous subscription.
#[no_mangle]
pub extern "C" fn rawsc_selftest_rs() -> i32 {
    let save_pid = SUB_PID.load(Ordering::Acquire);
    let save_win = SUB_WIN.load(Ordering::Acquire);
    let mut ok = true;
    let mut buf = [0u8; 8];

    // Arm 1: unsubscribed pushes must not be delivered.
    SUB_PID.store(0, Ordering::Release);
    rawsc_clear_rs();
    rawsc_push_rs(0x1E);
    if unsafe { rawsc_drain_rs(buf.as_mut_ptr(), 8) } != 0 { ok = false; }

    // Arm 2: subscribed pushes must be delivered, in order, exactly.
    rawsc_arm_rs(0xD05_u32, 0);
    rawsc_push_rs(0xE0);
    rawsc_push_rs(0x48);
    rawsc_push_rs(0xC8);
    let n = unsafe { rawsc_drain_rs(buf.as_mut_ptr(), 8) };
    if n != 3 || buf[0] != 0xE0 || buf[1] != 0x48 || buf[2] != 0xC8 { ok = false; }

    // Arm 3: after disarm the gate must close again.
    rawsc_disarm_rs(0xD05_u32);
    rawsc_push_rs(0x1E);
    if unsafe { rawsc_drain_rs(buf.as_mut_ptr(), 8) } != 0 { ok = false; }

    SUB_PID.store(save_pid, Ordering::Release);
    SUB_WIN.store(save_win, Ordering::Release);
    rawsc_clear_rs();
    if ok { 1 } else { 0 }
}
