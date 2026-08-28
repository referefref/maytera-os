// rustkern/dospit.rs - #172: the 8253/8254 PIT channel state machine, ONCE.
//
// New kernel logic, so Rust per the 2026-07-16 rule. A counter latch and a
// lo/hi byte toggle are a few branches per port access; there is no float and
// no measured performance reason for C. The COUNT DERIVATION stays in C
// (dos_pit_count_ch) because it reads the guest's retired-instruction clock,
// which is dosexec.c's own timebase; this module is handed the live count and
// owns only the register protocol.
//
// ===========================================================================
// WHY THIS EXISTS: Stunts, #172, third wall
// ---------------------------------------------------------------------------
// dosexec.c emulated PIT CHANNEL 0 and nothing else. `dos_out` case 0x43 read
// the channel field of the control word and did `if (ch != 0) break;`, and
// `dos_in` had no case for port 0x42 at all, so `in al,0x42` returned 0xFF.
//
// MEASURED, /DOS/STUNTS/LOAD.EXE /u MCGA /ssb on the #172 kernel, one run: the
// game loads, sets mode 13h, and then spins forever in this loop, sampled at
// 422a:0790..07B4 and reassembled from 157 @frame traces:
//
//     in  al,0x61 / or al,1 / out 0x61,al     ; open the channel-2 gate
//     mov al,0x80 / out 0x43,al               ; latch counter 2  <- DISCARDED
//     in  al,0x42 ... xchg al,ah ... in al,0x42   ; read it      <- 0xFF, 0xFF
//     cmp ax,imm16 / ja 0x0790                ; 0xFFFF > imm, forever
//
// This is the same defect the channel-0 comment in dos_in already describes,
// one channel over, in its own words: "a delay-loop calibration read
// start == end == 0xFFFF, its delta was always 0, its `cmp ax,imm` never
// passed, and the loop was unterminatable BY CONSTRUCTION."
//
// So channel 2 is not being bolted on beside channel 0: BOTH now run through
// the one state machine below. Two copies of this protocol is how the tree
// ended up with one channel that worked and one that returned 0xFF.
// ===========================================================================

/// Mirrors `dos_pit_ch_t` in dos/dosexec.c, sizeof- and offsetof-locked there.
#[repr(C)]
pub struct DosPitCh {
    /// Reload value. 0 means 65536, as on the hardware.
    pub divisor: u16,
    /// Value captured by a counter-latch command.
    pub latch: u16,
    /// A latch command is pending: reads return `latch`, not the live count.
    pub latched: u8,
    /// lo/hi byte toggles for access mode 3.
    pub rd_hi: u8,
    pub wr_hi: u8,
    /// RW field of the last control word: 1 = lobyte, 2 = hibyte, 3 = both.
    /// NEVER 0: 0 is the latch command, not an access mode, so a channel that
    /// has never been programmed must still read as 3 (dosexec.c's own comment
    /// on the channel-0 init says exactly this, and it is why every channel is
    /// initialised rather than memset).
    pub access: u8,
    /// Channel 2 only: port 0x61 bit 0, the counter GATE.
    pub gate: u8,
    pub _pad: [u8; 3],
}

/// A control word for this channel. `rw` is its RW field (bits 5:4).
/// `live` is the channel's current count, needed only for a latch.
#[no_mangle]
pub extern "C" fn dos_pit_ctrl_rs(ch: *mut DosPitCh, rw: u8, live: u16) {
    if ch.is_null() {
        return;
    }
    // SAFETY: caller passes &t->pit[n], n < 3.
    let c = unsafe { &mut *ch };
    if rw == 0 {
        // Counter-latch command: freeze the count for the next read pair.
        c.latch = live;
        c.latched = 1;
        c.rd_hi = 0;
    } else {
        c.access = rw;
        c.latched = 0;
        c.rd_hi = 0;
        c.wr_hi = 0;
    }
}

/// One data-port read. `live` is the current count from the emulated timebase.
#[no_mangle]
pub extern "C" fn dos_pit_read_rs(ch: *mut DosPitCh, live: u16) -> u8 {
    if ch.is_null() {
        return 0xFF;
    }
    // SAFETY: caller passes &t->pit[n], n < 3.
    let c = unsafe { &mut *ch };
    let v = if c.latched != 0 { c.latch } else { live };
    match c.access {
        1 => {
            c.latched = 0;
            (v & 0xFF) as u8
        }
        2 => {
            c.latched = 0;
            (v >> 8) as u8
        }
        _ => {
            if c.rd_hi == 0 {
                c.rd_hi = 1;
                (v & 0xFF) as u8
            } else {
                c.rd_hi = 0;
                c.latched = 0;
                (v >> 8) as u8
            }
        }
    }
}

/// One data-port write: loads the reload value a byte at a time.
#[no_mangle]
pub extern "C" fn dos_pit_write_rs(ch: *mut DosPitCh, b: u8) {
    if ch.is_null() {
        return;
    }
    // SAFETY: caller passes &t->pit[n], n < 3.
    let c = unsafe { &mut *ch };
    let b16 = b as u16;
    match c.access {
        1 => c.divisor = (c.divisor & 0xFF00) | b16,
        2 => c.divisor = (c.divisor & 0x00FF) | (b16 << 8),
        _ => {
            if c.wr_hi == 0 {
                c.divisor = (c.divisor & 0xFF00) | b16;
                c.wr_hi = 1;
            } else {
                c.divisor = (c.divisor & 0x00FF) | (b16 << 8);
                c.wr_hi = 0;
            }
        }
    }
}

/// Self-test. Returns the number of FAILING checks, so 0 is the pass.
///
/// Case 1 is the CHANNEL-0 protocol exactly as the C it replaces performed it,
/// so this is a transliteration check and not decoration; case 5 is the Stunts
/// sequence that produced 0xFFFF forever.
#[no_mangle]
pub extern "C" fn dos_pit_selftest_rs() -> i32 {
    let mut fail = 0;

    let fresh = || DosPitCh {
        divisor: 0,
        latch: 0,
        latched: 0,
        rd_hi: 0,
        wr_hi: 0,
        access: 3,
        gate: 0,
        _pad: [0; 3],
    };

    // 1. Access mode 3, unlatched: lo then hi of the LIVE count.
    let mut c = fresh();
    if dos_pit_read_rs(&mut c, 0x1234) != 0x34 {
        fail += 1;
    }
    if dos_pit_read_rs(&mut c, 0x1234) != 0x12 {
        fail += 1;
    }
    // and the toggle is back to lo.
    if dos_pit_read_rs(&mut c, 0xABCD) != 0xCD {
        fail += 1;
    }

    // 2. Access mode 1 (lobyte only) and 2 (hibyte only) never toggle.
    let mut c = fresh();
    dos_pit_ctrl_rs(&mut c, 1, 0);
    if dos_pit_read_rs(&mut c, 0x1234) != 0x34 || dos_pit_read_rs(&mut c, 0x1234) != 0x34 {
        fail += 1;
    }
    dos_pit_ctrl_rs(&mut c, 2, 0);
    if dos_pit_read_rs(&mut c, 0x1234) != 0x12 || dos_pit_read_rs(&mut c, 0x1234) != 0x12 {
        fail += 1;
    }

    // 3. A latch freezes the value: the count moves under us and the read pair
    //    must still return the LATCHED one, then release.
    let mut c = fresh();
    dos_pit_ctrl_rs(&mut c, 0, 0x8765);
    if dos_pit_read_rs(&mut c, 0x0001) != 0x65 {
        fail += 1;
    }
    if dos_pit_read_rs(&mut c, 0x0002) != 0x87 {
        fail += 1;
    }
    if c.latched != 0 {
        fail += 1;
    }
    // released: now live again.
    if dos_pit_read_rs(&mut c, 0x0003) != 0x03 {
        fail += 1;
    }

    // 4. A write pair assembles the divisor lo-then-hi and re-arms.
    let mut c = fresh();
    dos_pit_ctrl_rs(&mut c, 3, 0);
    dos_pit_write_rs(&mut c, 0x9C);
    dos_pit_write_rs(&mut c, 0x02);
    if c.divisor != 0x029C {
        fail += 1;
    }
    dos_pit_write_rs(&mut c, 0xFF);
    dos_pit_write_rs(&mut c, 0xFF);
    if c.divisor != 0xFFFF {
        fail += 1;
    }

    // 5. THE STUNTS SEQUENCE, on a channel that has never been programmed.
    //    `out 0x43,0x80` is control word 0x80: channel 2, RW = 0 = LATCH. Two
    //    reads of 0x42 must then hand back the latched count lo-then-hi, so a
    //    `cmp ax,imm / ja` sees a number that MOVES. Before #172 the control
    //    word was dropped for ch != 0 and both reads returned 0xFF.
    let mut c = fresh();
    let cw: u8 = 0x80;
    if (cw >> 6) != 2 {
        fail += 1; // the channel decode this case depends on
    }
    dos_pit_ctrl_rs(&mut c, (cw >> 4) & 3, 0x4E20);
    let lo = dos_pit_read_rs(&mut c, 0xFFFF);
    let hi = dos_pit_read_rs(&mut c, 0xFFFF);
    if ((hi as u16) << 8 | lo as u16) != 0x4E20 {
        fail += 1;
    }

    // 6. A null channel is 0xFF and no crash, matching the pre-#172 default
    //    for an unhandled port rather than inventing a new failure shape.
    if dos_pit_read_rs(core::ptr::null_mut(), 0x1234) != 0xFF {
        fail += 1;
    }
    dos_pit_ctrl_rs(core::ptr::null_mut(), 0, 0);
    dos_pit_write_rs(core::ptr::null_mut(), 0);

    fail
}
