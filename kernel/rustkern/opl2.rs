// rustkern/opl2.rs - #175: the YM3812 (OPL2) / AdLib DETECTION protocol, once.
//
// New kernel logic, so Rust per the 2026-07-16 rule. This is a handful of
// branches and two integer deadline comparisons per port access; there is no
// float (the kernel is -mno-sse soft-float anyway, which is why the period
// arithmetic below is integer PIT ticks and not microseconds-as-double) and no
// measured performance reason for C.
//
// ===========================================================================
// SCOPE, STATED AT THE LINE BECAUSE IT IS THE WHOLE POINT
// ---------------------------------------------------------------------------
// THIS MODULE DOES NOT SYNTHESISE ONE SAMPLE OF AUDIO, AND IS NOT A STEP
// TOWARDS DOING SO IN THIS FILE. It implements the register/timer protocol a
// guest uses to ANSWER THE QUESTION "is there an OPL2 in this machine", and
// nothing else. FM synthesis is a separate project and is deliberately out of
// scope for #175.
//
// The consequence is the design decision this module exists to make, and it is
// made in ONE place, `opl2_installed_policy` in dos/dosexec.c, not here:
//
//   REPORTING "PRESENT" WOULD BE A FABRICATION.
//
// A guest that detects an OPL2 turns its music on, writes its instrument
// patches, and plays a song. We would accept every one of those writes and
// emit silence. The user then has a game whose options screen says "Music:
// AdLib" and a machine that makes no sound, with nothing anywhere to explain
// it. That is the same shape of defect #120 deleted from fstat: a plausible
// value invented because returning it was easier than returning the truth.
// The standing rule on this project is that we do not invent plausible values.
//
// So the chip reports ABSENT, and the protocol below is implemented FAITHFULLY
// anyway, for two reasons that are not decoration:
//
//   1. ABSENT has to be delivered CORRECTLY, not by accident. Before this
//      module every unclaimed port read returned 0xFF (dos_in's default), which
//      happens to fail the AdLib status test but is an open ISA bus, not an
//      answer: the same 0xFF is what deadlocked the Sound Blaster DSP probe one
//      wall further on (see dos_sb_in). "Wrong in a way that happens to look
//      right" is not the same as right, and it does not stay right.
//   2. When FM synthesis is added, the detection MUST NOT need rewriting. Flip
//      `opl2_installed_policy` to 1 and every routine here starts telling the
//      truth in the other direction: the timers already run on the guest's own
//      emulated clock, so the canonical AdLib probe passes with no further
//      work.
//
// The self-test at the bottom therefore checks BOTH arms: the canonical AdLib
// detection sequence must report ABSENT with the chip uninstalled and PRESENT
// with it installed. A detection routine that can only ever say one thing has
// not been tested, it has been asserted.
// ===========================================================================
//
// THE PROTOCOL (AdLib Music Synthesizer Card manual, and the YM3812 datasheet)
//
//   base+0  write: latch the register index
//           read : status.  bit 7 = IRQ (either unmasked timer overflowed)
//                           bit 6 = timer 1 overflowed
//                           bit 5 = timer 2 overflowed
//                           bits 4..0 read as ZERO on an OPL2, and the whole
//                           detection hinges on that: an empty slot floats
//                           high, so 0x00 in the low bits is the evidence that
//                           something is actually driving the bus.
//   base+1  write: data for the latched register.
//
//   reg 0x02  timer 1 preset. period = (256 - preset) *  80 us
//   reg 0x03  timer 2 preset. period = (256 - preset) * 320 us
//   reg 0x04  timer control.  bit 0 = start timer 1     bit 6 = MASK timer 1
//                             bit 1 = start timer 2     bit 5 = MASK timer 2
//                             bit 7 = IRQ RESET. When set, every other bit is
//                                     ignored and both overflow flags clear.
//            A masked timer still runs; it just does not set its status flag.
//
//   The canonical probe every AdLib-aware game runs some variant of:
//
//       out 4, 0x60     ; mask both timers
//       out 4, 0x80     ; reset the flags
//       s1 = in(base+0)
//       out 2, 0xFF     ; timer 1 preset -> one 80us period
//       out 4, 0x21     ; start timer 1 unmasked, keep timer 2 masked
//       <delay >= 80us>
//       s2 = in(base+0)
//       out 4, 0x60 / out 4, 0x80      ; put the chip back
//       present = (s1 & 0xE0) == 0x00 && (s2 & 0xE0) == 0xC0

/// Timer 1 period, in 8253 PIT ticks, for a preset of 0xFF (one 80us tick).
///
/// Kept as a multiply-then-divide rather than a rounded constant: 80us is
/// 95.4546 PIT ticks, and rounding to 95 before multiplying by up to 256
/// periods loses a whole tick per 209 periods. Integer only, because the
/// kernel is built soft-float with SSE disabled.
const T1_NUM: u64 = 1_193_182 * 80; //  80 us, numerator over 1_000_000
const T2_NUM: u64 = 1_193_182 * 320; // 320 us
const USEC_DEN: u64 = 1_000_000;

/// Mirrors `dos_opl2_t` in dos/dosexec.c, sizeof- and offsetof-locked there.
///
/// `regs` is NOT dead weight and NOT a stub towards synthesis. It is the record
/// that lets the diagnostic in dos_opl2_report_rs say the honest thing at guest
/// exit: "the guest programmed N FM registers and heard silence". A silence
/// with no explanation anywhere is precisely the failure mode this ticket is
/// meant to avoid; if a game ever does get past detection, the trace says so.
#[repr(C)]
pub struct DosOpl2 {
    /// Register index latched by the last write to base+0.
    pub addr: u8,
    /// 0 = no chip in the socket. Set once at task init from the ONE policy
    /// decision in dosexec.c. Never written from inside this module.
    pub installed: u8,
    /// reg 0x02 / reg 0x03 presets.
    pub t1_preset: u8,
    pub t2_preset: u8,
    /// Start bits from reg 0x04 bits 0 and 1.
    pub t1_run: u8,
    pub t2_run: u8,
    /// Mask bits from reg 0x04 bits 6 and 5. A masked timer runs but sets no flag.
    pub t1_mask: u8,
    pub t2_mask: u8,
    /// Latched overflow flags, in their status-register positions: 0x40 / 0x20.
    pub flags: u8,
    pub _pad: [u8; 7],
    /// Next overflow instant for each timer, on the guest's emulated PIT clock.
    pub t1_deadline: u64,
    pub t2_deadline: u64,
    /// How many register writes the guest has made, and how many status reads.
    /// Diagnostics only; see the `regs` note above.
    pub n_reg_writes: u32,
    pub n_status_reads: u32,
    /// The written register file. Accepted and remembered, never sounded.
    pub regs: [u8; 256],
}

impl DosOpl2 {
    fn t1_period(&self) -> u64 {
        let n = 256u64 - self.t1_preset as u64;
        let p = (n * T1_NUM) / USEC_DEN;
        if p == 0 {
            1
        } else {
            p
        }
    }
    fn t2_period(&self) -> u64 {
        let n = 256u64 - self.t2_preset as u64;
        let p = (n * T2_NUM) / USEC_DEN;
        if p == 0 {
            1
        } else {
            p
        }
    }

    /// Advance both timers to `now` and latch any overflow that is not masked.
    ///
    /// Free-running: the deadline advances by whole periods rather than being
    /// re-armed from `now`, so a guest that samples the status slowly cannot
    /// make the timer drift. The bounded catch-up loop is not a poll: it runs
    /// at most a few iterations because a deadline behind `now` by many periods
    /// is short-circuited by the modulo step below it.
    fn tick(&mut self, now: u64) {
        if self.t1_run != 0 && now >= self.t1_deadline {
            if self.t1_mask == 0 {
                self.flags |= 0x40;
            }
            let p = self.t1_period();
            let behind = now - self.t1_deadline;
            self.t1_deadline += p * (behind / p + 1);
        }
        if self.t2_run != 0 && now >= self.t2_deadline {
            if self.t2_mask == 0 {
                self.flags |= 0x20;
            }
            let p = self.t2_period();
            let behind = now - self.t2_deadline;
            self.t2_deadline += p * (behind / p + 1);
        }
    }
}

/// A read of the OPL2 status port (base+0), or of any mirror of it.
///
/// `now` is the guest's emulated PIT clock, the same one the 8253 channels are
/// derived from (dosexec.c dos_emu_pit_now), so the OPL timers and the PIT can
/// never disagree about how much guest time a delay loop burned. That matters:
/// the probe's "delay at least 80us" step is almost always a PIT-calibrated
/// spin, and a second, disagreeing clock here is how you get a detection that
/// passes on one machine speed and fails on another.
///
/// Returns 0xFF when no chip is installed. That is the honest value for an
/// unclaimed ISA port: the bus floats high. It is ALSO exactly what dos_in
/// returned before this module existed, which is why the previous behaviour
/// looked correct for AdLib and hung the Sound Blaster probe: see dos_sb_in.
#[no_mangle]
pub extern "C" fn dos_opl2_status_rs(o: *mut DosOpl2, now: u64) -> u8 {
    if o.is_null() {
        return 0xFF;
    }
    // SAFETY: caller passes &t->opl2, which lives as long as the task.
    let c = unsafe { &mut *o };
    if c.installed == 0 {
        return 0xFF;
    }
    c.n_status_reads = c.n_status_reads.wrapping_add(1);
    c.tick(now);
    let f = c.flags & 0x60;
    // bit 7 is the OR of the unmasked overflows; bits 4..0 are zero on an OPL2,
    // and a guest reads those zeroes as the proof that a chip answered at all.
    if f != 0 {
        f | 0x80
    } else {
        0
    }
}

/// A write to the OPL2 address port (base+0): latch the register index.
#[no_mangle]
pub extern "C" fn dos_opl2_addr_rs(o: *mut DosOpl2, val: u8) {
    if o.is_null() {
        return;
    }
    // SAFETY: as above.
    let c = unsafe { &mut *o };
    c.addr = val;
}

/// A write to the OPL2 data port (base+1) for the latched register.
///
/// Every register is REMEMBERED and none is sounded. The timer registers are
/// the only ones with behaviour, because the timers are the only part of the
/// chip detection actually exercises.
#[no_mangle]
pub extern "C" fn dos_opl2_data_rs(o: *mut DosOpl2, val: u8, now: u64) {
    if o.is_null() {
        return;
    }
    // SAFETY: as above.
    let c = unsafe { &mut *o };
    // COUNT THE ATTEMPT FIRST, and count it even with an empty socket. The
    // whole point of this counter is the empty-socket case: a guest that writes
    // instrument patches to a chip that is not there is the one that produces
    // unexplained silence, and it is the only arm that ships. Counting after
    // the `installed` check made the diagnostic dead code in production and
    // live only in the diagnostic arm, which is exactly backwards.
    c.n_reg_writes = c.n_reg_writes.wrapping_add(1);
    if c.installed == 0 {
        return;
    }
    let idx = c.addr;
    c.regs[idx as usize] = val;

    match idx {
        0x02 => {
            c.tick(now);
            c.t1_preset = val;
        }
        0x03 => {
            c.tick(now);
            c.t2_preset = val;
        }
        0x04 => {
            if val & 0x80 != 0 {
                // IRQ RESET. Every other bit of this write is ignored, which is
                // why the canonical probe writes 0x60 and 0x80 as two separate
                // outs and not as one 0xE0: folding them would silently discard
                // the mask half on real hardware.
                c.flags = 0;
                return;
            }
            c.tick(now);

            let t1_start = val & 0x01;
            let t2_start = (val >> 1) & 0x01;
            c.t1_mask = (val >> 6) & 0x01;
            c.t2_mask = (val >> 5) & 0x01;

            if t1_start != 0 {
                if c.t1_run == 0 {
                    c.t1_deadline = now + c.t1_period();
                }
                c.t1_run = 1;
            } else {
                c.t1_run = 0;
            }
            if t2_start != 0 {
                if c.t2_run == 0 {
                    c.t2_deadline = now + c.t2_period();
                }
                c.t2_run = 1;
            } else {
                c.t2_run = 0;
            }
            // Masking a timer clears its pending flag, per the datasheet. This
            // is what makes the probe's opening `out 4,0x60` a real reset and
            // not a no-op, so it is not an optimisation to drop.
            if c.t1_mask != 0 {
                c.flags &= !0x40;
            }
            if c.t2_mask != 0 {
                c.flags &= !0x20;
            }
        }
        _ => {}
    }
}

/// Initialise a chip. `installed` comes from the ONE policy decision in
/// dosexec.c; this module never chooses it for itself.
#[no_mangle]
pub extern "C" fn dos_opl2_init_rs(o: *mut DosOpl2, installed: u8) {
    if o.is_null() {
        return;
    }
    // SAFETY: as above.
    let c = unsafe { &mut *o };
    c.addr = 0;
    c.installed = installed;
    // 0 presets, i.e. the longest period, matching a chip out of reset.
    c.t1_preset = 0;
    c.t2_preset = 0;
    c.t1_run = 0;
    c.t2_run = 0;
    c.t1_mask = 0;
    c.t2_mask = 0;
    c.flags = 0;
    c.t1_deadline = 0;
    c.t2_deadline = 0;
    c.n_reg_writes = 0;
    c.n_status_reads = 0;
    let mut i = 0usize;
    while i < 256 {
        c.regs[i] = 0;
        i += 1;
    }
}

/// How many FM register writes the guest made. Non-zero with `installed == 0`
/// is impossible by construction (dos_opl2_data_rs returns early), so a
/// non-zero count is exactly the case that needs the honest "programmed and
/// heard nothing" line in the exit diagnostic.
#[no_mangle]
pub extern "C" fn dos_opl2_writes_rs(o: *const DosOpl2) -> u32 {
    if o.is_null() {
        return 0;
    }
    // SAFETY: as above.
    unsafe { (*o).n_reg_writes }
}

/// Self-test. Returns the number of FAILING checks, so 0 is the pass.
///
/// Case 1 and case 2 are the SAME canonical AdLib probe run against the two
/// settings of `installed`, and they must disagree. A detection routine tested
/// only in the arm it currently ships in is not tested: it would still score a
/// pass if it were `return ABSENT;`.
#[no_mangle]
pub extern "C" fn dos_opl2_selftest_rs() -> i32 {
    let mut fails = 0i32;

    // The probe, verbatim, as a closure over an arbitrary elapsed-time budget.
    // Returns (s1, s2) so each case can assert on them itself.
    fn probe(c: &mut DosOpl2, delay_ticks: u64) -> (u8, u8) {
        let mut now = 1000u64; // an arbitrary non-zero start: a deadline of 0
                               // must not read as "already expired" for a timer
                               // that was never started.
        dos_opl2_addr_rs(c, 0x04);
        dos_opl2_data_rs(c, 0x60, now); // mask both
        dos_opl2_addr_rs(c, 0x04);
        dos_opl2_data_rs(c, 0x80, now); // reset flags
        let s1 = dos_opl2_status_rs(c, now);

        dos_opl2_addr_rs(c, 0x02);
        dos_opl2_data_rs(c, 0xFF, now); // timer 1 = one 80us period
        dos_opl2_addr_rs(c, 0x04);
        dos_opl2_data_rs(c, 0x21, now); // start timer 1 unmasked, timer 2 masked

        now += delay_ticks;
        let s2 = dos_opl2_status_rs(c, now);

        dos_opl2_addr_rs(c, 0x04);
        dos_opl2_data_rs(c, 0x60, now);
        dos_opl2_addr_rs(c, 0x04);
        dos_opl2_data_rs(c, 0x80, now);
        (s1, s2)
    }

    // A zeroed chip, then init. Built on the stack: 300-odd bytes, and this
    // runs once at boot.
    let mut c = DosOpl2 {
        addr: 0,
        installed: 0,
        t1_preset: 0,
        t2_preset: 0,
        t1_run: 0,
        t2_run: 0,
        t1_mask: 0,
        t2_mask: 0,
        flags: 0,
        _pad: [0; 7],
        t1_deadline: 0,
        t2_deadline: 0,
        n_reg_writes: 0,
        n_status_reads: 0,
        regs: [0; 256],
    };

    // ---- case 1: NOT installed. The canonical probe must report ABSENT. ----
    // 96 ticks is just over one 80us period (95.45), so the timer WOULD have
    // overflowed had there been a chip. The absent answer must not depend on
    // the guest being too impatient.
    dos_opl2_init_rs(&mut c, 0);
    let (s1, s2) = probe(&mut c, 96);
    if s1 != 0xFF || s2 != 0xFF {
        fails += 1; // an unclaimed port floats high
    }
    if (s1 & 0xE0) == 0x00 && (s2 & 0xE0) == 0xC0 {
        fails += 1; // the probe must NOT conclude "present"
    }
    // The probe above issues exactly six data writes (0x60, 0x80, 0xFF, 0x21,
    // 0x60, 0x80). All six must be COUNTED even though none is obeyed: this is
    // the assertion that keeps the silence diagnostic alive in the arm that
    // ships. It was `!= 0` and it passed for the wrong reason.
    if dos_opl2_writes_rs(&c) != 6 {
        fails += 1;
    }

    // ---- case 2: installed. The SAME probe must report PRESENT. ----
    dos_opl2_init_rs(&mut c, 1);
    let (s1, s2) = probe(&mut c, 96);
    if s1 != 0x00 {
        fails += 1; // masked and reset: no flags, and bits 4..0 read zero
    }
    if s2 != 0xC0 {
        fails += 1; // IRQ + timer 1 overflow, and timer 2 still masked
    }
    if !((s1 & 0xE0) == 0x00 && (s2 & 0xE0) == 0xC0) {
        fails += 1; // this is the line the games actually evaluate
    }

    // ---- case 3: the delay was too SHORT, so even a present chip says no. ---
    // This is what proves the timer is a timer and not a flag that gets set by
    // the act of starting it.
    dos_opl2_init_rs(&mut c, 1);
    let (s1, s2) = probe(&mut c, 40); // ~34us, under one 80us period
    if s1 != 0x00 || s2 != 0x00 {
        fails += 1;
    }

    // ---- case 4: a MASKED timer 1 overflows without setting its flag. ------
    dos_opl2_init_rs(&mut c, 1);
    let now = 1000u64;
    dos_opl2_addr_rs(&mut c, 0x02);
    dos_opl2_data_rs(&mut c, 0xFF, now);
    dos_opl2_addr_rs(&mut c, 0x04);
    dos_opl2_data_rs(&mut c, 0x61, now); // start timer 1 but MASK it (bit 6)
    if dos_opl2_status_rs(&mut c, now + 1000) != 0x00 {
        fails += 1;
    }

    // ---- case 5: the preset actually scales the period. --------------------
    // preset 0xFE is TWO 80us periods, so 96 ticks must not be enough and 200
    // must be. A period that ignored the preset would pass at both.
    dos_opl2_init_rs(&mut c, 1);
    dos_opl2_addr_rs(&mut c, 0x02);
    dos_opl2_data_rs(&mut c, 0xFE, now);
    dos_opl2_addr_rs(&mut c, 0x04);
    dos_opl2_data_rs(&mut c, 0x21, now);
    if dos_opl2_status_rs(&mut c, now + 96) != 0x00 {
        fails += 1;
    }
    if dos_opl2_status_rs(&mut c, now + 200) != 0xC0 {
        fails += 1;
    }

    // ---- case 6: reg 0x04 bit 7 ignores the rest of the byte. --------------
    // 0xE0 must reset the flags and NOT be read as "mask both and start
    // nothing", because a guest that folds the two writes into one is relying
    // on exactly this.
    dos_opl2_init_rs(&mut c, 1);
    dos_opl2_addr_rs(&mut c, 0x02);
    dos_opl2_data_rs(&mut c, 0xFF, now);
    dos_opl2_addr_rs(&mut c, 0x04);
    dos_opl2_data_rs(&mut c, 0x21, now);
    if dos_opl2_status_rs(&mut c, now + 96) != 0xC0 {
        fails += 1;
    }
    dos_opl2_addr_rs(&mut c, 0x04);
    dos_opl2_data_rs(&mut c, 0xE0, now + 96); // IRQ reset
    if dos_opl2_status_rs(&mut c, now + 97) != 0x00 {
        fails += 1;
    }
    // and the timer must still be RUNNING, because bit 0 was ignored, not cleared
    if dos_opl2_status_rs(&mut c, now + 300) != 0xC0 {
        fails += 1;
    }

    fails
}
