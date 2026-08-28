// rustkern/dosint15.rs - #252: INT 15h AH=86h, the BIOS WAIT, and why a DOS
// guest could load an instrument bank and still never make a sound.
//
// New kernel logic, so Rust per the 2026-07-16 rule. There is no performance
// argument for C here and none is claimed: this runs once per guest INT 15h
// call, which the measurement below counts in the dozens for a whole session,
// not in a hot path.
//
// ===========================================================================
// THE DEFECT, MEASURED
// ---------------------------------------------------------------------------
// dos_int_handler_inner() had no `case 0x15:` at all. Every INT 15h fell
// through to the generic "(ignored)" line: it returned immediately, left the
// flags untouched, and charged the guest's emulated clock NOTHING.
//
// For most of INT 15h that is harmless. For AH=86h it is not, because AH=86h
// IS a delay: "wait CX:DX microseconds". Answering it instantly is not
// declining to implement it, it is implementing it WRONG, and in the one
// direction the caller cannot detect.
//
// MEASURED on golden 2215 (commit 960d2209), The Secret of Monkey Island,
// /CONFIG/DOSIO.CFG=2, one run. This is the canonical AdLib detection
// protocol, and the three lines in the middle are the whole ticket:
//
//   [OPLPROBE] WRITE reg=0x04 val=0x60 pit_now=51136   mask both timers
//   [OPLPROBE] WRITE reg=0x04 val=0x80 pit_now=51140   reset the flags
//   [OPLPROBE] status=0xFF     pit_now=51141           read 1: expect 0x00
//   [OPLPROBE] WRITE reg=0x02 val=0xFF pit_now=51145   timer 1 = ONE 80 us period
//   [OPLPROBE] WRITE reg=0x04 val=0x21 pit_now=51149   START timer 1 unmasked
//   [dos] INT 15h ax=8600 bx=000e from 106f:1953 (ignored)      <-- the delay
//   [dos] INT 15h ax=8600 bx=000e from 106f:1953 (ignored)      <-- the delay
//   [dos] INT 15h ax=8600 bx=000e from 106f:1953 (ignored)      <-- the delay
//   [OPLPROBE] status=0xFF     pit_now=51151           read 2: expect 0xC0
//   [OPLPROBE] WRITE reg=0x04 val=0x60 pit_now=51154   give up, put it back
//
// The guest arms an 80 us timer, asks the BIOS to wait THREE times, and arrives
// at the status read 2 PIT ticks later. 80 us is 95 ticks. It abandons the
// probe 93 ticks early, reads the flag it set as still clear, and correctly
// concludes there is no AdLib. The chip is right, the synthesiser is right, the
// probe is right; the BIOS lied about how long it waited.
//
// ===========================================================================
// WHY #176 DID NOT ALREADY FIX THIS, WHICH IS THE INTERESTING PART
// ---------------------------------------------------------------------------
// #176 fixed the identical SYMPTOM for Commander Keen 5 by giving a guest port
// access an ISA bus cost, because Keen 5 spends its 80 us as 142 reads of the
// status port. The era's delay loops are written as port reads precisely
// because that self-calibrates on real hardware.
//
// Monkey Island does not do that. MEASURED under #175: Keen 5's probe is 6
// index writes, 6 data writes and 348 STATUS READS; Monkey Island's is 7 index
// writes, 7 data writes and TWO status reads. It does not spin on the port at
// all, it asks the BIOS. So #176's bus charge, which moved Keen 5's delay loop
// from 22 ticks to 194, buys Monkey Island nothing: its port traffic is 1
// permille of its clock.
//
// TWO GUESTS, ONE FAULT, TWO DIFFERENT DELAY MECHANISMS. That is why fixing
// Keen 5 in #176 and declaring the class closed was wrong, and it is why the
// corpus table that recorded "Monkey Island: PASS, live" for six consecutive
// tickets never noticed: liveness was never the thing that was broken.
//
// ===========================================================================
// THE FIX IS A TIME CHARGE, NOT A WAIT. #426 IS NOT EVEN REACHABLE HERE.
// ---------------------------------------------------------------------------
// Nothing blocks, sleeps, spins or yields. The guest's emulated clock is
// charged the interval it asked for through dos_bus_charge_us_rs(), the SAME
// accumulator #176 charges a port access through, and the interpreter carries
// straight on. There is no wait to get wrong, no wake source to lose, and no
// new busy-loop for the concurrency lint to catch, because there is no loop.
//
// That the charge goes through the #176 accumulator is load-bearing rather than
// tidy: dos_emu_clock_rate() reads `ticks_charged` and takes the charged time
// OUT of the same real second, so an 80 us BIOS wait does NOT make the guest's
// clock outrun the wall clock. A private counter here would advance the clock
// without being visible to that correction, which is precisely the defect #176
// existed to fix, arriving through a different door.
//
// ===========================================================================
// WHAT THIS DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
// 1. NO REAL DELAY. A guest that uses AH=86h as a frame pacer gets correct
//    VIRTUAL time and no wall-clock delay, so it still runs as fast as the
//    interpreter can carry it. That is exactly what it does today for every
//    other timing path in this emulator, so this is not a regression; making
//    the guest pace against the wall clock is a separate change with its own
//    blast radius across the whole corpus, and it must not be put behind this
//    measurement.
// 2. NO CHANGE TO ANY OTHER INT 15h FUNCTION. Everything except AH=86h returns
//    "not handled" and reaches the existing (ignored) path byte for byte. Real
//    BIOS reports an unsupported INT 15h function as CF=1 with AH=86h, and this
//    emulator instead returns with the flags untouched, which is its own small
//    fabrication; correcting it changes the answer every DOS title in the
//    corpus gets from every INT 15h call it makes, and that is a second change
//    needing a second measurement. It is noted here so the next person finds it
//    rather than rediscovers it.

#![allow(dead_code)]

/// AH=86h: WAIT. CX:DX is the interval in microseconds.
const INT15_WAIT: u8 = 0x86;

/// Decode one guest INT 15h.
///
/// Returns 1 if this call was SERVICED, in which case the caller must charge
/// `*wait_us` microseconds of emulated time and clear CF. Returns 0 if it was
/// not, in which case the caller must behave exactly as it did before this
/// module existed: touch nothing and log the MISS.
///
/// `wait_us` is always written, including on the not-handled path, so a caller
/// cannot read an uninitialised value if it gets the protocol wrong.
#[no_mangle]
pub extern "C" fn dos_int15_rs(ax: u16, cx: u16, dx: u16, wait_us: *mut u32) -> u32 {
    let us = ((cx as u32) << 16) | (dx as u32);
    if !wait_us.is_null() {
        // SAFETY: caller passes the address of a local u32.
        unsafe { *wait_us = 0 };
    }
    if (ax >> 8) as u8 != INT15_WAIT {
        return 0;
    }
    if !wait_us.is_null() {
        // SAFETY: as above.
        unsafe { *wait_us = us };
    }
    1
}

// ---------------------------------------------------------------------------
// Self-test, reported at boot beside the #172 PIT, #175 OPL2 and #176 bus ones.
//
// EVERY CASE ASSERTS BOTH DIRECTIONS. A decoder that answered "handled, wait
// 80 us" to everything would satisfy the positive cases and would silently
// charge emulated time for every INT 15h in the corpus, so each positive case
// has a negative twin that must NOT be claimed.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn dos_int15_selftest_rs() -> u32 {
    let mut fail = 0u32;
    let mut us: u32 = 0xDEAD_BEEF;

    // 1. THE CASE THE TICKET IS ABOUT. Monkey Island's probe delay: AH=86h with
    //    CX:DX = 80 us. Must be claimed, and must ask for exactly 80.
    if dos_int15_rs(0x8600, 0, 80, &mut us) != 1 || us != 80 {
        fail += 1;
    }

    // 2. THE FULL 32-BIT INTERVAL. CX is the HIGH word. Swapping CX and DX is
    //    the obvious bug and it is invisible for any interval under 65536 us,
    //    which is every interval an AdLib driver uses, so it would ship.
    if dos_int15_rs(0x8600, 0x0001, 0x0000, &mut us) != 1 || us != 65_536 {
        fail += 1;
    }
    if dos_int15_rs(0x8600, 0xFFFF, 0xFFFF, &mut us) != 1 || us != 0xFFFF_FFFF {
        fail += 1;
    }

    // 3. AL IS NOT PART OF THE FUNCTION NUMBER. AH alone selects it, so
    //    AX=0x86FF is the same call. The measured guest issues AX=0x8600, and a
    //    decoder that compared the whole of AX would work on that ONE title and
    //    fail on the next.
    if dos_int15_rs(0x86FF, 0, 80, &mut us) != 1 || us != 80 {
        fail += 1;
    }

    // 4. THE NEGATIVE TWINS. Nothing else may be claimed, and none of them may
    //    leave a stale interval behind for the caller to charge.
    for ah in [0x00u16, 0x41, 0x83, 0x85, 0x87, 0x88, 0x89, 0xC0, 0xFF] {
        us = 0xDEAD_BEEF;
        if dos_int15_rs((ah << 8) | 0x00, 0xFFFF, 0xFFFF, &mut us) != 0 || us != 0 {
            fail += 1;
        }
    }

    // 5. AH=86h WITH A ZERO INTERVAL IS STILL A SERVICED CALL. It asks for no
    //    time, which is a legal thing to ask for; reporting it unhandled would
    //    log a MISS for a call that was answered correctly.
    if dos_int15_rs(0x8600, 0, 0, &mut us) != 1 || us != 0 {
        fail += 1;
    }

    // 6. A NULL out-pointer must not fault, and must still classify.
    if dos_int15_rs(0x8600, 0, 80, core::ptr::null_mut()) != 1 {
        fail += 1;
    }
    if dos_int15_rs(0x8800, 0, 80, core::ptr::null_mut()) != 0 {
        fail += 1;
    }

    fail
}
