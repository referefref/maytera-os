// rustkern/dosmick.rs - (#mickey) WHAT A DOS GUEST IS TOLD THE MOUSE DID.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// It is pure arithmetic over a small #[repr(C)] state block that dos_task_t
// embeds; the DELIVERY of an INT 33h 0Ch upcall stays in C because it drives
// the two interpreter cores and their register files.
//
// ===========================================================================
// THE DEFECT, MEASURED
// ---------------------------------------------------------------------------
// A DOS mouse driver is RELATIVE. It counts mickeys, and INT 33h 0Fh sets how
// many mickeys make eight pixels: 8 horizontal and 16 vertical by default,
// i.e. one mickey per pixel across and TWO mickeys per pixel down, because the
// 320x200 modes this API was designed for have non-square pixels.
//
// Our pointer is ABSOLUTE. dos_pump_input() knows exactly where the host
// cursor is inside the guest picture, so it does not integrate anything; it
// maps a position. What it then did was re-derive a mickey stream from that
// position by MULTIPLYING the motion by the very ratio a guest is supposed to
// divide by:
//
//     t->mick_y += dy * t->mratio_y / 8;      // 16/8 = 2
//
// That is correct for a guest that models the driver and divides again. The
// Dig does not: its 0Ch handler (guest linear 0x0013F360, disassembled from
// DIG.EXE) reads the counters we pass in ESI/EDI, subtracts the values it
// saved last time, and ADDS THE DIFFERENCE STRAIGHT TO ITS POINTER:
//
//     mov dx, ds:0x5f24    ; the SI it stored last event
//     mov eax, esi         ; the SI we are passing now
//     mov cx, ds:0x5f14    ; its own pointer X
//     sub eax, edx
//     add ecx, eax         ; pointer += (si_now - si_prev)
//
// So one host pixel down became two guest pixels down. MEASURED by a sibling
// on The Dig launcher: internal y moved +219 for a host move of +107. The
// horizontal axis was right, the vertical was 2x, and the error accumulated,
// so the guest pointer walked away from the arrow the user is aiming with.
//
// ===========================================================================
// THE DESIGN, AND WHY THIS ONE
// ---------------------------------------------------------------------------
// There is no mickey stream that satisfies both classes of guest at once.
// Write G for the pixels-per-mickey gain the guest applies, and M for what we
// report; the pointer tracks only when M * G equals the host motion.
//
//   * A guest that divides by the ratio has G = 8/ratio, so M = motion*ratio/8
//     (what the code did).
//   * A guest that uses raw mickeys as pixels has G = 1, so M = motion.
//
// We cannot see which one a guest is, so the ratio has to stop being part of
// the answer. UNIT GAIN is chosen: the counters carry the host pointer's
// motion expressed in the guest's own virtual coordinate units. Reasons, in
// order:
//
//   1. It is what an absolute pointer actually means. The ratio exists to turn
//      PHYSICAL mouse motion into pixels. We have no physical motion; we have
//      pixels already. Scaling them by a mickeys-per-pixel ratio applies a
//      conversion whose input we do not have.
//   2. Of the titles measured, the one that integrates mickeys (The Dig) is
//      exactly the one unit gain fixes, and the ones that set a ratio with 0Fh
//      (Red Alert, The Incredible Machine) read the POSITION - CX/DX in the
//      upcall, or 03h - which is absolute and correct under either rule.
//   3. The guest-set ratio is still honoured where it is observable: 0Fh and
//      1Ah store it, 1Bh reads it back exactly, and it is never overwritten
//      with a hardcoded default.
//
// The ratio can be put back into the counters with /CONFIG/DOSMOUSE.CFG
// "gain=ratio", so a future title that genuinely divides has a lever that
// needs no rebuild. That knob restores the WHOLE pre-fix model - one
// free-running ratio-scaled accumulator, shared by both channels, homing
// declined - rather than half of it, which is what makes it usable as the
// "before" arm of an A/B measurement on one kernel. A compat switch whose
// other position nobody has watched is not a switch.
//
// ===========================================================================
// UNIT GAIN IS ONLY HALF OF IT: A RELATIVE INTEGRATOR CARRIES AN OFFSET
// ---------------------------------------------------------------------------
// Getting the gain right makes the guest pointer move by the right AMOUNT. It
// does NOT put it in the right PLACE: The Dig starts its pointer at its own
// (320,260) whatever the host cursor is doing, so a pure delta stream leaves a
// permanent offset, and pointing at one menu item highlights another. The
// guest's pointer is private and invisible (The Dig draws no cursor; the arrow
// is the compositor's own), so it cannot be read back and corrected.
//
// It can be HOMED, because the same disassembly shows the guest clamping its
// pointer to its own box on every event:
//
//     cmp WORD ds:0x5f14, 0   ; jge ...   -> negative clamps to 0
//     mov eax, ds:0x5eb8      ; dec eax   -> above width-1 clamps to width-1
//
// Any guest that integrates a relative mouse must clamp, or its pointer would
// leave the screen on real hardware and never come back. That makes the clamp
// a safe lever, and homing is three events delivered back to back inside ONE
// interpreter slice, so the guest's main loop never sees the intermediate:
//
//     phase 1   report 32000                 establish a KNOWN previous value
//                                            (the guest's saved value is stale
//                                            after an install and unknowable)
//     phase 2   report 2000                  a delta of -30000 pins the guest
//                                            pointer to its own minimum
//     phase 3   report 2000 + (pos - min)    a delta of +(pos-min) lands it
//                                            exactly under the host cursor
//
// and the steady state IS phase 3 continued: the counter is 2000 + (pos - min),
// an exact affine function of the absolute position. Two further defects fall
// out of that for free:
//
//   * IT CANNOT WRAP. The counter used to be a free-running accumulator handed
//     over as a 16-bit value, while The Dig subtracts ZERO-EXTENDED values in
//     32 bits. After about 32k pixels of travel it would step from 0xFFFF to
//     0x0000 and the guest would compute a delta of -65535, slamming its
//     pointer into the corner. Bounded by the virtual range, it never gets
//     near the wrap.
//   * A 0Bh READ CANNOT DESYNC A CALLBACK. 0Bh reads AND CLEARS the counters,
//     so on the shared-counter model a guest that polled 0Bh anywhere would
//     hand its own event handler a huge negative delta on the next event. The
//     0Bh accumulator is now a separate number.
//
// Homing is re-armed on a 0Ch/14h install (the guest's saved value is stale),
// on a 07h/08h range change (the mapping itself moved), and periodically, so a
// guest that re-centres its own pointer resyncs on its own rather than staying
// wrong until it is relaunched.
//
// ===========================================================================
// WHAT THIS DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
// It does not touch mx/my. Those are the driver position, they are already
// absolute and already correct, and 03h and the upcall's CX/DX report them. A
// guest that reads the position was never broken and must not be perturbed to
// fix one that does not.

// Mirrored by dos_mick_t in dos/dosexec.c with a _Static_assert on the size.
#[repr(C)]
pub struct DosMick {
    pub rep_x: i32,       // value most recently handed to the guest in SI
    pub rep_y: i32,       // ... and DI
    pub rel_x: i32,       // 0Bh accumulator, read-and-cleared by 0Bh alone
    pub rel_y: i32,
    pub prev_x: i32,      // last absolute position folded in
    pub prev_y: i32,
    pub have_prev: i32,   // 0 until the first fold, so the first is not a jump
    pub home_ph: i32,     // 0 = synced; 1/2/3 = the homing phase to deliver
    pub home_n: u32,      // completed homings (diagnostic)
    pub since_home: u32,  // move events delivered since the last one
    pub home_every: u32,  // re-home interval in delivered move events; 0 = off
    pub gain_ratio: i32,  // 1 = scale the counters by the guest ratio (opt-in)
}

// The counter band. BASE keeps the steady-state value clear of zero so a
// homing pin has room below it; PIN must exceed any virtual range we will home
// (checked against MAX_RANGE) and must stay inside a signed 16-bit delta, so a
// guest that subtracts in 16 bits sees the same number as one that subtracts
// zero-extended in 32.
const BASE: i32 = 2000;
const HIGH: i32 = 32000;
const PIN: i32 = HIGH - BASE;          // 30000
const MAX_RANGE: i32 = 29000;
// MEASURED, and the first value was wrong for a reason worth recording. At 240
// a re-home is minutes away, and the ONE home that does happen fires on the
// first upcall after the install - which for The Dig is BEFORE the game has
// initialised its own pointer to (320,260), so the home is correct and is then
// overwritten by the guest itself. Run a1: every later event tracked the host
// exactly (unit gain works) at a CONSTANT err=(320,260), i.e. the guest's own
// starting pointer. A homing schedule has to outlive the guest's own startup,
// so it is re-armed every 8 delivered move events: three extra upcalls per
// eight real ones, on a handler that is about sixty instructions long.
pub const DOS_MICK_HOME_EVERY: u32 = 8;

#[inline]
fn homeable(min: i32, max: i32) -> bool {
    max > min && (max - min) <= MAX_RANGE && PIN > (max - min)
}

// The steady-state counter for one axis. Kept clear of the 16-bit wrap: a
// range too large for a homing pin also loses the BASE offset, so that
// BASE + range can never exceed 65535.
#[inline]
fn steady(pos: i32, min: i32, max: i32) -> i32 {
    let b = if homeable(min, max) { BASE } else { 0 };
    b + (pos - min)
}

#[no_mangle]
pub extern "C" fn dos_mick_reset_rs(st: *mut DosMick) {
    if st.is_null() { return; }
    let s = unsafe { &mut *st };
    s.rep_x = 0; s.rep_y = 0;
    s.rel_x = 0; s.rel_y = 0;
    s.prev_x = 0; s.prev_y = 0;
    s.have_prev = 0;
    s.home_ph = 1;                 // the first delivery homes
    s.home_n = 0;
    s.since_home = 0;
    if s.home_every == 0 { s.home_every = DOS_MICK_HOME_EVERY; }
}

// Re-arm homing. Called on a 0Ch/14h install, on a 07h/08h range change, and
// by the periodic tick. Never interrupts a homing already in flight.
#[no_mangle]
pub extern "C" fn dos_mick_arm_home_rs(st: *mut DosMick) {
    if st.is_null() { return; }
    let s = unsafe { &mut *st };
    if s.home_ph == 0 { s.home_ph = 1; }
}

// Fold an absolute guest-virtual position in. Returns 1 if it moved.
//
// ratio_x/ratio_y are the guest's 0Fh/1Ah values and are used ONLY when the
// gain_ratio knob is set; the default is unit gain (see the header).
#[no_mangle]
pub extern "C" fn dos_mick_move_rs(st: *mut DosMick, x: i32, y: i32,
                                   min_x: i32, max_x: i32,
                                   min_y: i32, max_y: i32,
                                   ratio_x: i32, ratio_y: i32) -> i32 {
    if st.is_null() { return 0; }
    let s = unsafe { &mut *st };
    if s.have_prev == 0 {
        s.prev_x = x; s.prev_y = y;
        s.have_prev = 1;
        if s.gain_ratio == 0 {
            s.rep_x = steady(x, min_x, max_x);
            s.rep_y = steady(y, min_y, max_y);
        }
        return 0;
    }
    let dx = x - s.prev_x;
    let dy = y - s.prev_y;
    if dx == 0 && dy == 0 {
        // The reported value is a pure function of the position, so recompute
        // it anyway: a range change moves it without the pointer moving.
        if s.gain_ratio == 0 {
            s.rep_x = steady(x, min_x, max_x);
            s.rep_y = steady(y, min_y, max_y);
        }
        return 0;
    }
    s.prev_x = x; s.prev_y = y;
    if s.gain_ratio != 0 {
        // THE PRE-FIX MODEL, reproduced exactly and on purpose. One
        // free-running accumulator scaled by the guest ratio, serving both
        // channels, handed over 16 bits wide by the C side so it wraps the same
        // way. It is the escape hatch for a title that genuinely divides, and it
        // is the "before" arm of the A/B measurement: a switch whose other
        // position nobody has watched is not a switch.
        let rx = if ratio_x > 0 { ratio_x } else { 8 };
        let ry = if ratio_y > 0 { ratio_y } else { 8 };
        let mx = dx * rx / 8;
        let my = dy * ry / 8;
        s.rel_x += mx;
        s.rel_y += my;
        s.rep_x += mx;
        s.rep_y += my;
    } else {
        s.rel_x += dx;
        s.rel_y += dy;
        s.rep_x = steady(x, min_x, max_x);
        s.rep_y = steady(y, min_y, max_y);
    }
    1
}

// 0Bh: read and clear the RELATIVE motion counters. Separate from the counter
// the upcall carries, on purpose (see the header).
#[no_mangle]
pub extern "C" fn dos_mick_take_rel_rs(st: *mut DosMick, ox: *mut i32, oy: *mut i32) {
    if st.is_null() || ox.is_null() || oy.is_null() { return; }
    let s = unsafe { &mut *st };
    unsafe { *ox = s.rel_x; *oy = s.rel_y; }
    s.rel_x = 0; s.rel_y = 0;
}

// The counter pair for the NEXT upcall. Returns 1 when this is a homing phase
// and the caller must therefore force a MOVE event; 0 for the ordinary case,
// where the values are the steady-state ones.
#[no_mangle]
pub extern "C" fn dos_mick_next_rs(st: *mut DosMick, x: i32, y: i32,
                                   min_x: i32, max_x: i32,
                                   min_y: i32, max_y: i32,
                                   osi: *mut i32, odi: *mut i32) -> i32 {
    if st.is_null() || osi.is_null() || odi.is_null() { return 0; }
    let s = unsafe { &mut *st };
    // A range we cannot pin is a range we must not try to home: the phase-2
    // delta would be smaller than the box and would leave the pointer
    // somewhere arbitrary instead of on its minimum. Nor can a NON-UNIT gain be
    // homed: the landing delta would be scaled by the guest on the way in, so
    // the pointer would come to rest somewhere other than the cursor.
    // And there is nothing to home TO until a real position has been folded in:
    // before the first pump the driver cursor is (0,0), which is a coordinate,
    // not a measurement. Run a1 spent its only homing sequence there.
    if s.home_ph != 0 && s.have_prev == 0 {
        unsafe { *osi = s.rep_x; *odi = s.rep_y; }
        return 0;
    }
    if s.home_ph != 0 && (s.gain_ratio != 0 ||
                          !(homeable(min_x, max_x) && homeable(min_y, max_y))) {
        s.home_ph = 0;
    }
    match s.home_ph {
        1 => { unsafe { *osi = HIGH; *odi = HIGH; } 1 }
        2 => { unsafe { *osi = BASE; *odi = BASE; } 1 }
        3 => {
            unsafe { *osi = BASE + (x - min_x); *odi = BASE + (y - min_y); }
            1
        }
        _ => {
            unsafe { *osi = s.rep_x; *odi = s.rep_y; }
            0
        }
    }
}

// The upcall for the current phase reached the guest. Advance.
#[no_mangle]
pub extern "C" fn dos_mick_phase_done_rs(st: *mut DosMick, x: i32, y: i32,
                                         min_x: i32, max_x: i32,
                                         min_y: i32, max_y: i32) {
    if st.is_null() { return; }
    let s = unsafe { &mut *st };
    if s.home_ph == 0 { return; }
    if s.home_ph >= 3 {
        s.home_ph = 0;
        s.home_n += 1;
        s.since_home = 0;
        // Phase 3 IS the steady state, so the two agree by construction and a
        // later move continues from it rather than stepping.
        s.rep_x = steady(x, min_x, max_x);
        s.rep_y = steady(y, min_y, max_y);
        s.prev_x = x; s.prev_y = y;
        s.have_prev = 1;
    } else {
        s.home_ph += 1;
    }
}

// Count a delivered ordinary move event; arm a re-home when one is due.
// Returns 1 if it armed one.
#[no_mangle]
pub extern "C" fn dos_mick_tick_rs(st: *mut DosMick) -> i32 {
    if st.is_null() { return 0; }
    let s = unsafe { &mut *st };
    if s.home_ph != 0 { return 0; }
    if s.home_every == 0 { return 0; }
    s.since_home += 1;
    if s.since_home >= s.home_every {
        s.home_ph = 1;
        // Reset HERE, not only on completion. A home that never completes (the
        // guest sits in a cli region, or its mask excludes MOVE) would
        // otherwise leave the counter at or above the interval and re-arm on
        // every single tick from then on.
        s.since_home = 0;
        return 1;
    }
    0
}

// A model of the guest side of the contract, used by the self-test: integrate
// the differences of the counters we hand out and clamp to the box, exactly as
// The Dig's handler at 0x0013F360 does.
struct Guest { p: i32, prev: i32, lo: i32, hi: i32, first: bool }
impl Guest {
    fn new(start: i32, lo: i32, hi: i32) -> Guest {
        Guest { p: start, prev: 0, lo, hi, first: true }
    }
    fn event(&mut self, si: i32) {
        if self.first { self.first = false; } else { self.p += si - self.prev; }
        self.prev = si;
        if self.p < self.lo { self.p = self.lo; }
        if self.p > self.hi { self.p = self.hi; }
    }
}

fn drive(st: *mut DosMick, gx: &mut Guest, gy: &mut Guest, hx: i32, hy: i32,
         bx: (i32, i32), by: (i32, i32)) -> (i32, i32) {
    let mut si = 0i32;
    let mut di = 0i32;
    dos_mick_move_rs(st, hx, hy, bx.0, bx.1, by.0, by.1, 8, 16);
    for _ in 0..4 {
        if dos_mick_next_rs(st, hx, hy, bx.0, bx.1, by.0, by.1,
                            &mut si, &mut di) == 0 { break; }
        gx.event(si); gy.event(di);
        dos_mick_phase_done_rs(st, hx, hy, bx.0, bx.1, by.0, by.1);
    }
    dos_mick_next_rs(st, hx, hy, bx.0, bx.1, by.0, by.1, &mut si, &mut di);
    gx.event(si); gy.event(di);
    (si, di)
}

fn blank() -> DosMick {
    DosMick { rep_x: 0, rep_y: 0, rel_x: 0, rel_y: 0, prev_x: 0, prev_y: 0,
              have_prev: 0, home_ph: 0, home_n: 0, since_home: 0,
              home_every: 0, gain_ratio: 0 }
}

#[no_mangle]
pub extern "C" fn dos_mick_selftest_rs() -> i32 {
    let mut bad = 0;
    let mut st = blank();
    let p: *mut DosMick = &mut st;
    dos_mick_reset_rs(p);

    // The Dig's box: 0..639 x 0..479, and a guest pointer that starts nowhere
    // near the host cursor (its own 320,260).
    let bx = (0i32, 639i32);
    let by = (0i32, 479i32);
    let mut gx = Guest::new(320, bx.0, bx.1);
    let mut gy = Guest::new(260, by.0, by.1);
    let mut hx = 100i32;
    let mut hy = 50i32;

    // First contact: the pointer must end up ON the host cursor, not 210
    // pixels above it. That is what homing is for.
    drive(p, &mut gx, &mut gy, hx, hy, bx, by);
    if gx.p != hx || gy.p != hy { bad += 1; }

    // A vertical move must be 1x, not 2x, with the documented 8/16 ratio in
    // force. This is the reported defect, written as an assertion.
    hy += 107;
    drive(p, &mut gx, &mut gy, hx, hy, bx, by);
    if gy.p != hy { bad += 1; }
    if gx.p != hx { bad += 1; }

    // A long walk must not accumulate error and must not wrap the counter.
    for _ in 0..400 {
        hx = (hx + 37) % 640;
        hy = (hy + 23) % 480;
        let (si, di) = drive(p, &mut gx, &mut gy, hx, hy, bx, by);
        if gx.p != hx || gy.p != hy { bad += 1; break; }
        if si < 0 || si > 65535 || di < 0 || di > 65535 { bad += 1; break; }
    }

    // A guest that has moved its own pointer far outside the box, as a
    // re-centring title would, must be recovered by an armed home.
    gx.p = 5000; gy.p = -900;
    dos_mick_arm_home_rs(p);
    drive(p, &mut gx, &mut gy, hx, hy, bx, by);
    if gx.p != hx || gy.p != hy { bad += 1; }

    // The periodic tick must arm exactly once per interval, and never while a
    // homing is already in flight.
    st.home_every = 4; st.home_ph = 0; st.since_home = 0;
    let mut armed = 0;
    for _ in 0..12 { if dos_mick_tick_rs(p) != 0 { armed += 1; st.home_ph = 0; } }
    if armed != 3 { bad += 1; }

    // 0Bh is a SEPARATE accumulator: reading it must not disturb the counter
    // the upcall carries, which was the shared-counter defect.
    st.home_every = 0; st.home_ph = 0;
    dos_mick_move_rs(p, 200, 200, bx.0, bx.1, by.0, by.1, 8, 16);
    let before = st.rep_x;
    let mut rx = 0i32;
    let mut ry = 0i32;
    dos_mick_take_rel_rs(p, &mut rx, &mut ry);
    if st.rep_x != before { bad += 1; }
    if st.rel_x != 0 || st.rel_y != 0 { bad += 1; }

    // Unit gain: 0Bh must report the motion in guest units, NOT doubled by the
    // vertical ratio, unless the ratio knob is explicitly on.
    dos_mick_move_rs(p, 200, 300, bx.0, bx.1, by.0, by.1, 8, 16);
    dos_mick_take_rel_rs(p, &mut rx, &mut ry);
    if ry != 100 { bad += 1; }
    // The compat arm must reproduce the defect, not merely be selectable: a
    // 100-pixel host move must arrive as 200, on BOTH channels, and homing must
    // decline rather than half-work under a non-unit gain.
    st.gain_ratio = 1;
    let rep_before = st.rep_y;
    dos_mick_move_rs(p, 200, 400, bx.0, bx.1, by.0, by.1, 8, 16);
    dos_mick_take_rel_rs(p, &mut rx, &mut ry);
    if ry != 200 { bad += 1; }
    if st.rep_y - rep_before != 200 { bad += 1; }
    dos_mick_arm_home_rs(p);
    let mut cs = 0i32;
    let mut cd = 0i32;
    if dos_mick_next_rs(p, 200, 400, bx.0, bx.1, by.0, by.1, &mut cs, &mut cd) != 0 { bad += 1; }
    st.gain_ratio = 0;

    // The Incredible Machine's range, 0..2556 x 0..1916, must home like any
    // other: it is well inside the pin.
    let mut st3 = blank();
    let r: *mut DosMick = &mut st3;
    dos_mick_reset_rs(r);
    let tx = (0i32, 2556i32);
    let ty = (0i32, 1916i32);
    let mut tgx = Guest::new(1278, tx.0, tx.1);
    let mut tgy = Guest::new(958, ty.0, ty.1);
    drive(r, &mut tgx, &mut tgy, 2000, 1500, tx, ty);
    if tgx.p != 2000 || tgy.p != 1500 { bad += 1; }

    // A range too large to pin must DECLINE to home rather than half-home, and
    // its counter must still fit 16 bits.
    let mut st2 = blank();
    let q: *mut DosMick = &mut st2;
    dos_mick_reset_rs(q);
    dos_mick_move_rs(q, 40000, 40000, 0, 60000, 0, 60000, 8, 16);
    let mut s2 = 0i32;
    let mut d2 = 0i32;
    if dos_mick_next_rs(q, 40000, 40000, 0, 60000, 0, 60000,
                        &mut s2, &mut d2) != 0 { bad += 1; }
    if s2 > 65535 || d2 > 65535 || s2 < 0 || d2 < 0 { bad += 1; }

    // A null state must be inert rather than a fault.
    dos_mick_reset_rs(core::ptr::null_mut());
    dos_mick_arm_home_rs(core::ptr::null_mut());
    if dos_mick_move_rs(core::ptr::null_mut(), 1, 1, 0, 9, 0, 9, 8, 16) != 0 { bad += 1; }
    if dos_mick_tick_rs(core::ptr::null_mut()) != 0 { bad += 1; }

    bad
}
