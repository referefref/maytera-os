// rustkern/dossb.rs - #181: the Sound Blaster DSP command protocol and the
// 8237A DMA channel state a DOS guest programs to play digitised audio.
//
// New kernel logic, so Rust per the 2026-07-16 rule. Every operation here is a
// handful of branches, one 16-bit decrement and one integer divide per DSP
// command; there is no float (the kernel is -mno-sse soft-float, which is why
// the rate arithmetic below is an integer divide and not a reciprocal
// multiply) and no measured performance reason for C.
//
// ===========================================================================
// SCOPE, STATED AT THE LINE
// ---------------------------------------------------------------------------
// THIS MODULE SYNTHESISES NOTHING. The guest hands us PCM: it fills a buffer
// in its own memory, programs the 8237 to feed that buffer to the card, and
// tells the DSP to play. So the job is DECODE + BOOKKEEPING, and the audio
// itself is a memcpy with a format conversion, done by the caller.
//
// It is also NOT an FM/OPL2 module. rustkern/opl2.rs (#175) owns 0x388/0x389
// and the AdLib detection protocol, and #182 owns FM synthesis. The two share
// a card in real life and share nothing in this tree, deliberately.
//
// WHAT IS EMULATED AND WHAT IS NOT, so that a later reader does not have to
// infer the boundary from the code:
//   YES  DSP reset handshake, the write-status and read-status ports, the
//        command/argument state machine, time constant -> sample rate,
//        8-bit single-cycle and auto-init DMA output, block size, speaker
//        on/off, halt/continue, DSP version, DSP identify, force-IRQ, and
//        the 8-bit end-of-block interrupt with its read-status acknowledge.
//   NO   16-bit DMA (SB16 commands 0xB0..0xCF), ADC/record (0x24/0x2C/0x20),
//        the mixer chip at base+4/base+5, MIDI/MPU-401, and ADPCM
//        (0x16/0x17/0x74..0x77). Each is DECLINED as an unknown command and
//        counted in `cmd_hist`, so the census says what a guest actually
//        wanted rather than us guessing in advance. See the note on
//        `cmd_arglen`.
//
// THE PORT MAP IS MEASURED, NOT ASSUMED. The brief for this ticket did not
// name one, and #175 was filed with a wrong constant (0x218 for what is really
// 0x388), so nothing here is taken on trust. Recorded from the guest's own
// port traffic under /CONFIG/DOSIO.CFG on build 1998, Aladdin, which reads its
// own /DOS/ALADDIN/SOUND.CFG and then programs, in this order:
//
//     OUT 0x226 <- 0x01, 0x00        DSP reset (base+6)          => base 0x220
//     IN  0x22E                      read-buffer status (base+E)
//     IN  0x22A                      read data (base+A), wants 0xAA
//     IN  0x22C x65535               write-buffer status (base+C)
//     OUT 0x22C <- 0xE1              get DSP version
//     OUT 0x00A <- 0x05              8237 single mask: MASK channel 1
//     OUT 0x00B <- 0x59              8237 mode: ch1, READ, auto-init, single
//     OUT 0x083 <- 0x06              page register for CHANNEL 1  => DMA 1
//     OUT 0x00C <- 0xFF              clear the byte-pointer flip-flop
//     OUT 0x002 <- 0x10, 0xA7        channel 1 base address = 0xA710
//     OUT 0x003 <- 0x6F, 0x0A        channel 1 base count   = 0x0A6F
//     OUT 0x00A <- 0x01              8237 single mask: UNMASK channel 1
//     OUT 0x22C <- 0x14              8-bit single-cycle DMA output
//     IN  0x003 x N                  poll the CURRENT COUNT for completion
//
// and SOUND.CFG's own bytes say base 0x0220, IRQ 5, DMA 1, which is what the
// three constants above independently confirm. Port 0x83 is the page register
// for CHANNEL 1 specifically (0x87/0x83/0x81/0x82 for channels 0/1/2/3), so
// the channel number is not inferred from a config file either.
//
// THE LAST LINE IS THE ONE THAT MATTERS. Aladdin does not wait for the
// end-of-block interrupt; it polls the 8237's current word count on port 0x003
// and waits for it to run out. Before this module that port was undecoded and
// read 0xFF forever, so the count never moved and the poll never ended: 21960
// reads and still going at the 120 s mark of a measured run. An emulation that
// raised the interrupt but left the count register frozen would still hang
// this title. That is why the count register below is driven from what has
// actually been PLAYED and not from what we have queued.

// ---------------------------------------------------------------------------
// Register offsets from the card's base address.
// ---------------------------------------------------------------------------
pub const SB_OFF_RESET: u16 = 0x6; // write 1 then 0
pub const SB_OFF_RDATA: u16 = 0xA; // read: DSP output byte
pub const SB_OFF_WRITE: u16 = 0xC; // write: command/data. read bit7: busy
pub const SB_OFF_RSTAT: u16 = 0xE; // read bit7: output byte available. ACKs 8-bit IRQ

/// The byte a DSP returns after a successful reset. This value IS the
/// detection: every probe in the corpus resets and then looks for it.
const DSP_READY: u8 = 0xAA;

/// 8237 mode-register bit 4. Set means the channel reloads its base address
/// and count at terminal count instead of stopping.
const DMA_MODE_AUTOINIT: u8 = 0x10;
/// Mode-register bits 3..2 == 10b: the DMAC READS memory and writes the
/// device, i.e. playback. 01b would be a record.
const DMA_MODE_READ: u8 = 0x08;

// ---------------------------------------------------------------------------
// 8237A, one channel of the 8-bit controller.
// ---------------------------------------------------------------------------
/// Mirrors `dos_dma_ch_t` in dos/dosexec.c, sizeof-locked there.
#[repr(C)]
pub struct DosDmaCh {
    pub base_addr: u16,
    pub base_count: u16,
    pub cur_addr: u16,
    pub cur_count: u16,
    pub page: u8,
    pub mode: u8,
    pub masked: u8,
    /// Set at terminal count, cleared by a read of the status register, which
    /// is the same latch-and-clear a real 8237 has.
    pub tc: u8,
}

/// Mirrors `dos_dma_t` in dos/dosexec.c, sizeof-locked there.
#[repr(C)]
pub struct DosDma {
    pub ch: [DosDmaCh; 4],
    /// The byte-pointer flip-flop. ONE per controller, not per channel: that
    /// is why every driver writes port 0x0C before touching an address or
    /// count pair, and why a per-channel copy would desynchronise the moment
    /// two channels were programmed in an interleaved order.
    pub ff: u8,
    pub _pad: [u8; 3],
    /// Census, for the "what did the guest actually do" report at exit.
    pub n_prog: u32,
    pub n_count_reads: u32,
}

impl DosDma {
    fn clear(&mut self) {
        for i in 0..4 {
            self.ch[i].base_addr = 0;
            self.ch[i].base_count = 0;
            self.ch[i].cur_addr = 0;
            self.ch[i].cur_count = 0;
            self.ch[i].page = 0;
            self.ch[i].mode = 0;
            // A real 8237 comes out of master reset with EVERY channel MASKED.
            // Starting unmasked would let a stale mode byte look like an armed
            // transfer to the pump before the guest has programmed anything.
            self.ch[i].masked = 1;
            self.ch[i].tc = 0;
        }
        self.ff = 0;
        self.n_prog = 0;
        self.n_count_reads = 0;
    }
}

// ---------------------------------------------------------------------------
// The DSP.
// ---------------------------------------------------------------------------
/// Mirrors `dos_sb_t` in dos/dosexec.c, sizeof-locked there.
#[repr(C)]
pub struct DosSb {
    /// 0 = empty socket. Set once at task init from the ONE policy decision in
    /// dosexec.c (`sb_installed_policy`). Never written from inside here.
    pub installed: u8,
    /// Base address >> 4, so 0x22 means 0x220. Stored narrow because it is
    /// compared on every port access and a u8 keeps the struct honest about
    /// the fact that a Sound Blaster base is always 0x2x0.
    pub base_hi: u8,
    pub irq: u8,
    pub dma: u8,
    pub dsp_major: u8,
    pub dsp_minor: u8,
    /// D1/D3. On a real card this connects or disconnects the DAC from the
    /// output amplifier; here it gates the samples we push, so a guest that
    /// leaves the speaker off gets the silence it asked for.
    pub speaker: u8,
    /// Set between the write of 1 and the write of 0 to base+6.
    pub in_reset: u8,

    /// Command byte currently collecting arguments, or 0 when idle. 0 is safe
    /// as "idle" because command 0x00 does not exist on any DSP.
    pub cmd: u8,
    pub argn: u8,
    pub argv: [u8; 2],
    /// Output FIFO the guest reads at base+A. Two bytes is enough for every
    /// command implemented here (0xE1 returns major then minor); a third is
    /// carried so a bug cannot silently truncate.
    pub out: [u8; 4],
    pub out_head: u8,
    pub out_tail: u8,

    /// A DSP transfer is armed. Cleared by 0xD0 (halt) and by reaching the end
    /// of a non-auto-init block.
    pub active: u8,
    pub autoinit: u8,
    /// Set by 0x90/0x98 (high-speed auto-init/single-cycle). Recorded because
    /// a high-speed transfer on a real SB 2.0 can only be ended by a DSP
    /// reset, and a guest that relies on that would otherwise look stuck.
    pub high_speed: u8,
    /// Generation counter, bumped every time a transfer is armed. The pump
    /// reads it to notice a NEW transfer even when the previous one had the
    /// same length and rate, which a plain "active" flag cannot express.
    pub gen: u8,

    pub time_const: u8,
    pub rate: u32,
    /// Block length in BYTES (the wire format is length-1, converted on the
    /// way in so that nothing downstream has to remember the off-by-one).
    pub block_len: u16,
    /// Set by 0x48. A 0x1C auto-init transfer takes its length from here and
    /// carries no length of its own, which is the whole reason 0x48 exists.
    pub block_len_48: u16,

    /// 8-bit IRQ asserted and not yet acknowledged. A real card clears this
    /// when the guest READS base+0xE, and several drivers rely on exactly that
    /// as their acknowledge, so the read has a side effect here too.
    pub irq_pending: u8,
    pub _pad: [u8; 3],
    pub irq_raised: u32,
    pub irq_acked: u32,

    /// Census: every command byte the guest wrote, by value. This is the
    /// instrument that answers "what does the corpus actually need" with a
    /// measurement instead of a guess, including for the commands DECLINED
    /// below, which is the more useful half.
    pub cmd_hist: [u32; 256],
    pub cmd_unknown: u32,
    pub resets: u32,
}

/// How many argument bytes a command takes after its opcode.
///
/// A WRONG ANSWER HERE DESYNCHRONISES THE WHOLE COMMAND STREAM, and it does so
/// silently: the next data byte is then read as an opcode, which is exactly
/// the failure blame.md records for an unimplemented Win16 import popping the
/// wrong number of stack bytes. So an unknown command takes ZERO arguments and
/// is counted, rather than being guessed at: a command we do not implement
/// then costs us one ignored byte, not the rest of the session.
fn cmd_arglen(cmd: u8) -> u8 {
    match cmd {
        0x10 => 1, // direct DAC output, one unsigned sample
        0x14 | 0x15 => 2, // 8-bit single-cycle DMA output, length-1 lo/hi
        0x1C | 0x1F => 0, // 8-bit auto-init DMA output (length comes from 0x48)
        0x40 => 1, // set time constant
        0x48 => 2, // set DMA block size, length-1 lo/hi
        0x80 => 2, // silence period, length-1 lo/hi
        0x90 | 0x98 => 0, // high-speed auto-init / single-cycle
        0xD0 | 0xD1 | 0xD3 | 0xD4 | 0xDA => 0,
        0xE0 => 1, // DSP identify: returns the complement of its argument
        0xE1 => 0, // get version
        0xE4 => 1, // write test register
        0xE8 => 0, // read test register
        0xF2 => 0, // force 8-bit IRQ
        _ => 0,
    }
}

impl DosSb {
    fn out_push(&mut self, v: u8) {
        let n = self.out.len() as u8;
        let next = (self.out_tail + 1) % n;
        if next == self.out_head {
            return; // full: drop, rather than overwrite the byte being read
        }
        self.out[self.out_tail as usize] = v;
        self.out_tail = next;
    }
    fn out_avail(&self) -> bool {
        self.out_head != self.out_tail
    }
    fn out_pop(&mut self) -> u8 {
        if !self.out_avail() {
            // A real DSP returns the last byte it produced when the FIFO is
            // empty. Returning 0xFF here instead would look exactly like an
            // EMPTY SOCKET to a probe, which is the one thing this must never
            // accidentally say once a card is installed.
            return self.out[((self.out_tail + self.out.len() as u8 - 1)
                % self.out.len() as u8) as usize];
        }
        let v = self.out[self.out_head as usize];
        self.out_head = (self.out_head + 1) % self.out.len() as u8;
        v
    }
    fn clear_fifo(&mut self) {
        self.out_head = 0;
        self.out_tail = 0;
        self.out = [0; 4];
    }
}

/// Sample rate for a DSP time constant, in Hz.
///
/// TC = 256 - 1000000/rate, so rate = 1000000 / (256 - TC). Integer divide,
/// because the kernel is soft-float with SSE disabled; the truncation is at
/// most a few Hz at the rates a DOS title uses (TC 0xA6 -> 11111 Hz where the
/// nominal figure is 11025) and a real card's own divider is equally coarse.
/// TC 0xFF would divide by one and is clamped: no shipped title asks for a
/// 1 MHz DAC, and the clamp keeps a corrupt byte from producing a rate the
/// sink would reject.
fn rate_from_tc(tc: u8) -> u32 {
    let d = 256u32 - tc as u32;
    if d == 0 {
        return 44100;
    }
    let r = 1_000_000u32 / d;
    if r < 4000 {
        4000
    } else if r > 48000 {
        48000
    } else {
        r
    }
}

// ===========================================================================
// FFI: the DSP
// ===========================================================================

#[no_mangle]
pub extern "C" fn dos_sb_init_rs(
    p: *mut DosSb,
    installed: u8,
    base_hi: u8,
    irq: u8,
    dma: u8,
    major: u8,
    minor: u8,
) {
    if p.is_null() {
        return;
    }
    let s = unsafe { &mut *p };
    s.installed = installed;
    s.base_hi = base_hi;
    s.irq = irq;
    s.dma = dma;
    s.dsp_major = major;
    s.dsp_minor = minor;
    s.speaker = 0;
    s.in_reset = 0;
    s.cmd = 0;
    s.argn = 0;
    s.argv = [0; 2];
    s.clear_fifo();
    s.active = 0;
    s.autoinit = 0;
    s.high_speed = 0;
    s.gen = 0;
    s.time_const = 0;
    s.rate = 0;
    s.block_len = 0;
    s.block_len_48 = 0;
    s.irq_pending = 0;
    s.irq_raised = 0;
    s.irq_acked = 0;
    s.cmd_hist = [0; 256];
    s.cmd_unknown = 0;
    s.resets = 0;
}

/// Write to base+6. The reset handshake is `1` then `0`, and the READY byte is
/// produced on the falling edge, not on the write of 1: a probe that writes 1,
/// polls, and only then writes 0 must not see 0xAA early.
#[no_mangle]
pub extern "C" fn dos_sb_reset_rs(p: *mut DosSb, val: u8) {
    if p.is_null() {
        return;
    }
    let s = unsafe { &mut *p };
    if s.installed == 0 {
        return;
    }
    if val & 1 != 0 {
        s.in_reset = 1;
        s.clear_fifo();
        return;
    }
    if s.in_reset == 0 {
        return; // a 0 with no preceding 1 is not a reset
    }
    s.in_reset = 0;
    s.resets += 1;
    // Reset stops any transfer, silences the DAC and drops a pending IRQ:
    // that is what makes reset the documented escape from a high-speed
    // transfer on an SB 2.0.
    s.active = 0;
    s.autoinit = 0;
    s.high_speed = 0;
    s.speaker = 0;
    s.irq_pending = 0;
    s.cmd = 0;
    s.argn = 0;
    s.clear_fifo();
    s.out_push(DSP_READY);
}

/// Read from base+A, base+C or base+E. `off` is the offset from the base.
///
/// Returns 0xFF for every port when no card is installed, which is what an
/// undriven ISA data bus floats to and is the same answer dos_in's default
/// gives today: that equivalence is what makes the absent arm a NO-OP rather
/// than a second, subtly different kind of absence.
#[no_mangle]
pub extern "C" fn dos_sb_read_rs(p: *mut DosSb, off: u16) -> u8 {
    if p.is_null() {
        return 0xFF;
    }
    let s = unsafe { &mut *p };
    if s.installed == 0 {
        return 0xFF;
    }
    match off {
        SB_OFF_WRITE => {
            // Write-buffer status. Bit 7 CLEAR means "ready for a command
            // byte". We are always ready, so this is the bit whose 65535-deep
            // countdown Aladdin currently loses eight times per run.
            0x7F
        }
        SB_OFF_RSTAT => {
            // Read-buffer status. Bit 7 set means a byte is waiting at base+A.
            // READING THIS PORT ACKNOWLEDGES THE 8-BIT IRQ: that is not a
            // convenience, it is the documented acknowledge and several
            // drivers do nothing else to clear it.
            if s.irq_pending != 0 {
                s.irq_pending = 0;
                s.irq_acked += 1;
            }
            if s.out_avail() {
                0xFF
            } else {
                0x7F
            }
        }
        SB_OFF_RDATA => s.out_pop(),
        SB_OFF_RESET => 0xFF, // write-only on a real card
        _ => 0xFF,
    }
}

/// Write to base+C: a command byte, or an argument byte for the command being
/// assembled. Returns 1 if this write ARMED a transfer, so the caller knows to
/// wake the pump without having to re-read the struct and guess.
#[no_mangle]
pub extern "C" fn dos_sb_write_rs(p: *mut DosSb, val: u8) -> u8 {
    if p.is_null() {
        return 0;
    }
    let s = unsafe { &mut *p };
    if s.installed == 0 {
        return 0;
    }
    if s.cmd != 0 && s.argn < cmd_arglen(s.cmd) {
        s.argv[s.argn as usize] = val;
        s.argn += 1;
        if s.argn < cmd_arglen(s.cmd) {
            return 0;
        }
        let c = s.cmd;
        s.cmd = 0;
        return sb_exec(s, c);
    }
    s.cmd_hist[val as usize] += 1;
    s.cmd = val;
    s.argn = 0;
    if cmd_arglen(val) == 0 {
        s.cmd = 0;
        return sb_exec(s, val);
    }
    0
}

/// Execute a fully-assembled command. Returns 1 if a transfer was armed.
fn sb_exec(s: &mut DosSb, cmd: u8) -> u8 {
    match cmd {
        0x10 => {
            // Direct DAC output. Accepted and DISCARDED, deliberately: a
            // single 8-bit sample delivered by a guest busy-loop at an
            // unknown rate has no rate to resample from, and inventing one
            // would produce noise. Counted in cmd_hist so the census says a
            // title used it. No title in the shipped corpus does.
            0
        }
        0x14 | 0x15 => {
            let len = (s.argv[0] as u16) | ((s.argv[1] as u16) << 8);
            s.block_len = len.wrapping_add(1);
            s.autoinit = 0;
            s.high_speed = 0;
            s.active = 1;
            s.gen = s.gen.wrapping_add(1);
            1
        }
        0x1C | 0x1F => {
            s.block_len = s.block_len_48;
            s.autoinit = 1;
            s.high_speed = 0;
            s.active = 1;
            s.gen = s.gen.wrapping_add(1);
            1
        }
        0x90 | 0x98 => {
            s.block_len = s.block_len_48;
            s.autoinit = if cmd == 0x90 { 1 } else { 0 };
            s.high_speed = 1;
            s.active = 1;
            s.gen = s.gen.wrapping_add(1);
            1
        }
        0x40 => {
            s.time_const = s.argv[0];
            s.rate = rate_from_tc(s.argv[0]);
            0
        }
        0x48 => {
            let len = (s.argv[0] as u16) | ((s.argv[1] as u16) << 8);
            s.block_len_48 = len.wrapping_add(1);
            0
        }
        0x80 => {
            // Silence period, in samples. Armed as a normal transfer with a
            // silence flag would need a second buffer source; instead the
            // length is recorded and the IRQ is raised by the caller after
            // the equivalent time, which is all a guest can observe.
            let len = (s.argv[0] as u16) | ((s.argv[1] as u16) << 8);
            s.block_len = len.wrapping_add(1);
            s.autoinit = 0;
            s.active = 0;
            // Nothing to play: acknowledge immediately with the end-of-block
            // interrupt the command promises.
            s.irq_pending = 1;
            s.irq_raised += 1;
            0
        }
        0xD0 => {
            s.active = 0;
            0
        }
        0xD1 => {
            s.speaker = 1;
            0
        }
        0xD3 => {
            s.speaker = 0;
            0
        }
        0xD4 => {
            if s.block_len != 0 {
                s.active = 1;
            }
            0
        }
        0xDA => {
            s.autoinit = 0;
            0
        }
        0xE0 => {
            s.out_push(!s.argv[0]);
            0
        }
        0xE1 => {
            s.out_push(s.dsp_major);
            s.out_push(s.dsp_minor);
            0
        }
        0xE4 => 0,
        0xE8 => {
            s.out_push(0x00);
            0
        }
        0xF2 => {
            s.irq_pending = 1;
            s.irq_raised += 1;
            0
        }
        _ => {
            s.cmd_unknown += 1;
            0
        }
    }
}

/// Raise the end-of-block interrupt. Called by the pump when the last sample
/// of a block has actually been PLAYED, never when it was merely queued.
#[no_mangle]
pub extern "C" fn dos_sb_raise_irq_rs(p: *mut DosSb) {
    if p.is_null() {
        return;
    }
    let s = unsafe { &mut *p };
    s.irq_pending = 1;
    s.irq_raised += 1;
}

/// Is an 8-bit IRQ asserted? Read by the interpreter thread, which is the only
/// context allowed to push an interrupt frame into the guest.
#[no_mangle]
pub extern "C" fn dos_sb_irq_pending_rs(p: *const DosSb) -> u8 {
    if p.is_null() {
        return 0;
    }
    unsafe { (*p).irq_pending }
}

/// End a non-auto-init transfer. Separate from the IRQ so that the auto-init
/// case, which raises an IRQ per block and keeps running, does not have to
/// special-case it.
#[no_mangle]
pub extern "C" fn dos_sb_block_done_rs(p: *mut DosSb) {
    if p.is_null() {
        return;
    }
    let s = unsafe { &mut *p };
    if s.autoinit == 0 {
        s.active = 0;
    }
}

// ===========================================================================
// FFI: the 8237
// ===========================================================================

#[no_mangle]
pub extern "C" fn dos_dma_init_rs(p: *mut DosDma) {
    if p.is_null() {
        return;
    }
    unsafe { &mut *p }.clear();
}

/// Port write in the 0x00..0x0F range (the 8-bit controller's own registers).
#[no_mangle]
pub extern "C" fn dos_dma_out_rs(p: *mut DosDma, port: u16, val: u8) {
    if p.is_null() {
        return;
    }
    let d = unsafe { &mut *p };
    match port {
        0x00 | 0x02 | 0x04 | 0x06 => {
            let c = (port >> 1) as usize;
            if d.ff == 0 {
                d.ch[c].base_addr = (d.ch[c].base_addr & 0xFF00) | val as u16;
            } else {
                d.ch[c].base_addr = (d.ch[c].base_addr & 0x00FF) | ((val as u16) << 8);
                // A real 8237 latches BOTH halves before the channel is armed,
                // so the current address is only refreshed on the high byte.
                // Refreshing on each half would leave a half-written address
                // visible to the pump for the duration of one guest
                // instruction, which is a race with a 50% chance of a wildly
                // wrong DMA source.
                d.ch[c].cur_addr = d.ch[c].base_addr;
                d.n_prog += 1;
            }
            d.ff ^= 1;
        }
        0x01 | 0x03 | 0x05 | 0x07 => {
            let c = (port >> 1) as usize;
            if d.ff == 0 {
                d.ch[c].base_count = (d.ch[c].base_count & 0xFF00) | val as u16;
            } else {
                d.ch[c].base_count = (d.ch[c].base_count & 0x00FF) | ((val as u16) << 8);
                d.ch[c].cur_count = d.ch[c].base_count;
                d.ch[c].tc = 0;
            }
            d.ff ^= 1;
        }
        0x0A => {
            // Single-channel mask. Bit 2 sets, clears the mask for channel
            // (val & 3).
            let c = (val & 3) as usize;
            d.ch[c].masked = if val & 0x04 != 0 { 1 } else { 0 };
        }
        0x0B => {
            let c = (val & 3) as usize;
            d.ch[c].mode = val;
        }
        0x0C => {
            d.ff = 0;
        }
        0x0D => {
            // Master reset: clears the flip-flop and masks every channel.
            d.clear();
        }
        0x0E => {
            for i in 0..4 {
                d.ch[i].masked = 0;
            }
        }
        0x0F => {
            for i in 0..4 {
                d.ch[i].masked = if val & (1 << i) != 0 { 1 } else { 0 };
            }
        }
        _ => {}
    }
}

/// Port read in the 0x00..0x0F range.
#[no_mangle]
pub extern "C" fn dos_dma_in_rs(p: *mut DosDma, port: u16) -> u8 {
    if p.is_null() {
        return 0xFF;
    }
    let d = unsafe { &mut *p };
    match port {
        0x00 | 0x02 | 0x04 | 0x06 => {
            let c = (port >> 1) as usize;
            let v = if d.ff == 0 {
                (d.ch[c].cur_addr & 0xFF) as u8
            } else {
                (d.ch[c].cur_addr >> 8) as u8
            };
            d.ff ^= 1;
            v
        }
        0x01 | 0x03 | 0x05 | 0x07 => {
            let c = (port >> 1) as usize;
            d.n_count_reads += 1;
            let v = if d.ff == 0 {
                (d.ch[c].cur_count & 0xFF) as u8
            } else {
                (d.ch[c].cur_count >> 8) as u8
            };
            d.ff ^= 1;
            v
        }
        0x08 => {
            // Status register: low nibble = terminal count reached, high
            // nibble = request pending. Reading CLEARS the TC bits, which is
            // the 8237's own latch-and-clear behaviour and is what a driver
            // that polls for completion this way depends on.
            let mut v = 0u8;
            for i in 0..4 {
                if d.ch[i].tc != 0 {
                    v |= 1 << i;
                    d.ch[i].tc = 0;
                }
            }
            v
        }
        _ => 0xFF,
    }
}

/// Page-register write. `chan` is resolved by the caller from the port
/// (0x87/0x83/0x81/0x82 for channels 0/1/2/3), because that mapping is a
/// property of the MOTHERBOARD's address decode and not of the 8237.
#[no_mangle]
pub extern "C" fn dos_dma_page_rs(p: *mut DosDma, chan: u8, val: u8) {
    if p.is_null() || chan > 3 {
        return;
    }
    unsafe { &mut *p }.ch[chan as usize].page = val;
}

/// The 24-bit physical address `off` bytes into the channel's programmed
/// buffer, WITH THE 16-BIT WRAP a real 8237 has.
///
/// The address counter is 16 bits and the page register supplies the top 8; the
/// counter is NOT carried into the page. So a transfer that runs off the end of
/// its 64 KB page continues at the START of that page. This is the reason every
/// DOS sound driver's documentation says a DMA buffer must not cross a 64 KB
/// boundary: the hardware does not object, it wraps, and the sound is garbage.
/// Reproducing the wrap means a guest with that bug sounds wrong HERE in the
/// same way it sounded wrong on a real machine, instead of reading whatever
/// happened to be in the next page of guest memory.
#[no_mangle]
pub extern "C" fn dos_dma_phys_at_rs(p: *const DosDma, chan: u8, off: u32) -> u32 {
    if p.is_null() || chan > 3 {
        return 0;
    }
    let c = &unsafe { &*p }.ch[chan as usize];
    let a = c.base_addr.wrapping_add(off as u16);
    ((c.page as u32) << 16) | a as u32
}

/// The 24-bit physical address of the channel's current position.
#[no_mangle]
pub extern "C" fn dos_dma_cur_phys_rs(p: *const DosDma, chan: u8) -> u32 {
    if p.is_null() || chan > 3 {
        return 0;
    }
    let d = unsafe { &*p };
    ((d.ch[chan as usize].page as u32) << 16) | d.ch[chan as usize].cur_addr as u32
}

/// Is this channel armed for playback: unmasked, in READ mode, non-zero count.
#[no_mangle]
pub extern "C" fn dos_dma_playback_armed_rs(p: *const DosDma, chan: u8) -> u8 {
    if p.is_null() || chan > 3 {
        return 0;
    }
    let c = &unsafe { &*p }.ch[chan as usize];
    if c.masked != 0 {
        return 0;
    }
    if c.mode & 0x0C != DMA_MODE_READ {
        return 0;
    }
    if c.base_count == 0 && c.cur_count == 0 {
        return 0;
    }
    1
}

#[no_mangle]
pub extern "C" fn dos_dma_autoinit_rs(p: *const DosDma, chan: u8) -> u8 {
    if p.is_null() || chan > 3 {
        return 0;
    }
    if unsafe { &*p }.ch[chan as usize].mode & DMA_MODE_AUTOINIT != 0 {
        1
    } else {
        0
    }
}

/// Total bytes in the programmed block: count register + 1, as the hardware
/// counts a transfer of N bytes with a count of N-1.
#[no_mangle]
pub extern "C" fn dos_dma_block_bytes_rs(p: *const DosDma, chan: u8) -> u32 {
    if p.is_null() || chan > 3 {
        return 0;
    }
    unsafe { &*p }.ch[chan as usize].base_count as u32 + 1
}

/// Publish the PLAYED position of a channel: `played` bytes since the block
/// began. Sets the terminal-count latch and returns 1 when the block is over.
///
/// The current count is DERIVED here rather than decremented incrementally,
/// because the caller's authority is a monotonic count of frames the sink has
/// consumed and a derived value cannot drift away from it. An incremented
/// copy can, and a count register that disagrees with the audio by a few bytes
/// per block would drift a poll loop into a hang over a long track.
#[no_mangle]
pub extern "C" fn dos_dma_set_played_rs(p: *mut DosDma, chan: u8, played: u32) -> u8 {
    if p.is_null() || chan > 3 {
        return 0;
    }
    let d = unsafe { &mut *p };
    let c = &mut d.ch[chan as usize];
    let total = c.base_count as u32 + 1;
    if played >= total {
        // Terminal count. A real 8237 leaves the count at 0xFFFF (it wrapped
        // past zero) and sets the TC status bit.
        c.cur_count = 0xFFFF;
        c.cur_addr = c.base_addr.wrapping_add(c.base_count).wrapping_add(1);
        c.tc = 1;
        if c.mode & DMA_MODE_AUTOINIT != 0 {
            c.cur_addr = c.base_addr;
            c.cur_count = c.base_count;
        }
        return 1;
    }
    c.cur_count = (total - 1 - played) as u16;
    c.cur_addr = c.base_addr.wrapping_add(played as u16);
    0
}

// ===========================================================================
// Format conversion: guest 8-bit unsigned mono -> S16_LE.
//
// On the DOS side and not in the sink, because this is the ONE place that
// knows the samples are unsigned-8 in the first place; audio_pcm's contract is
// S16_LE and widening it to "or maybe u8" would push a format question into
// every consumer of that ring.
// ===========================================================================
#[no_mangle]
pub extern "C" fn dos_sb_u8_to_s16_rs(src: *const u8, dst: *mut i16, n: u32) {
    if src.is_null() || dst.is_null() {
        return;
    }
    for i in 0..n as usize {
        let b = unsafe { *src.add(i) } as i32;
        // Centre at zero and scale to full range. (b - 128) << 8 maps 0x00 to
        // -32768 and 0xFF to +32512; the residual 255/256 gain error is below
        // the quantisation of the 8-bit source and no correction can recover
        // information that is not there.
        unsafe { *dst.add(i) = ((b - 128) << 8) as i16 };
    }
}

// ===========================================================================
// Self-test. Both arms, always, for the reason #175 gives: a detection tested
// only in the arm it currently ships in scores a pass for `return ABSENT` and
// proves nothing.
// ===========================================================================
fn blank_sb() -> DosSb {
    DosSb {
        installed: 0,
        base_hi: 0x22,
        irq: 5,
        dma: 1,
        dsp_major: 2,
        dsp_minor: 1,
        speaker: 0,
        in_reset: 0,
        cmd: 0,
        argn: 0,
        argv: [0; 2],
        out: [0; 4],
        out_head: 0,
        out_tail: 0,
        active: 0,
        autoinit: 0,
        high_speed: 0,
        gen: 0,
        time_const: 0,
        rate: 0,
        block_len: 0,
        block_len_48: 0,
        irq_pending: 0,
        _pad: [0; 3],
        irq_raised: 0,
        irq_acked: 0,
        cmd_hist: [0; 256],
        cmd_unknown: 0,
        resets: 0,
    }
}

fn blank_dma() -> DosDma {
    let mut d = DosDma {
        ch: [
            DosDmaCh { base_addr: 0, base_count: 0, cur_addr: 0, cur_count: 0, page: 0, mode: 0, masked: 1, tc: 0 },
            DosDmaCh { base_addr: 0, base_count: 0, cur_addr: 0, cur_count: 0, page: 0, mode: 0, masked: 1, tc: 0 },
            DosDmaCh { base_addr: 0, base_count: 0, cur_addr: 0, cur_count: 0, page: 0, mode: 0, masked: 1, tc: 0 },
            DosDmaCh { base_addr: 0, base_count: 0, cur_addr: 0, cur_count: 0, page: 0, mode: 0, masked: 1, tc: 0 },
        ],
        ff: 0,
        _pad: [0; 3],
        n_prog: 0,
        n_count_reads: 0,
    };
    d.clear();
    d
}

/// Returns the number of FAILING checks (0 = pass), so a caller can print the
/// count rather than a bool, matching every other selftest in dos/.
#[no_mangle]
pub extern "C" fn dos_sb_selftest_rs() -> i32 {
    let mut fail = 0i32;

    // ---- ARM 1: NO CARD. Every port floats, and the corpus probes conclude
    // absent. This arm must stay byte-identical to the pre-#181 default, which
    // is what makes "no card configured behaves exactly as today" a test
    // rather than a claim.
    {
        let mut s = blank_sb();
        s.installed = 0;
        dos_sb_reset_rs(&mut s, 1);
        dos_sb_reset_rs(&mut s, 0);
        if dos_sb_read_rs(&mut s, SB_OFF_RSTAT) != 0xFF { fail += 1; }
        if dos_sb_read_rs(&mut s, SB_OFF_RDATA) != 0xFF { fail += 1; }
        // The write-status bit 7 must stay SET, because a clear bit is what
        // ends the guest's countdown, and an absent card must never end it.
        if dos_sb_read_rs(&mut s, SB_OFF_WRITE) & 0x80 == 0 { fail += 1; }
        if dos_sb_write_rs(&mut s, 0xE1) != 0 { fail += 1; }
        if s.cmd_hist[0xE1] != 0 { fail += 1; }
    }

    // ---- ARM 2: CARD PRESENT. The canonical detection every title runs.
    {
        let mut s = blank_sb();
        s.installed = 1;
        // SkyRoads' own shape: reset, then read status and data until 0xAA.
        dos_sb_reset_rs(&mut s, 1);
        // Mid-reset the READY byte must NOT be there yet.
        if dos_sb_read_rs(&mut s, SB_OFF_RSTAT) & 0x80 != 0 { fail += 1; }
        dos_sb_reset_rs(&mut s, 0);
        if dos_sb_read_rs(&mut s, SB_OFF_RSTAT) & 0x80 == 0 { fail += 1; }
        if dos_sb_read_rs(&mut s, SB_OFF_RDATA) != DSP_READY { fail += 1; }
        // ...and the byte is consumed: a second read must not re-serve it as
        // if a new reset had happened.
        if dos_sb_read_rs(&mut s, SB_OFF_RSTAT) & 0x80 != 0 { fail += 1; }
        // Write-buffer status must now say READY (bit 7 clear), which is the
        // bit Aladdin loses 65535 times per command today.
        if dos_sb_read_rs(&mut s, SB_OFF_WRITE) & 0x80 != 0 { fail += 1; }

        // Get version.
        dos_sb_write_rs(&mut s, 0xE1);
        if dos_sb_read_rs(&mut s, SB_OFF_RDATA) != 2 { fail += 1; }
        if dos_sb_read_rs(&mut s, SB_OFF_RDATA) != 1 { fail += 1; }

        // Identify: returns the complement of its argument. Proves the
        // one-argument path collects exactly one byte.
        dos_sb_write_rs(&mut s, 0xE0);
        dos_sb_write_rs(&mut s, 0x5A);
        if dos_sb_read_rs(&mut s, SB_OFF_RDATA) != 0xA5 { fail += 1; }

        // Time constant -> rate. 0xA6 is the canonical 11 kHz constant.
        dos_sb_write_rs(&mut s, 0x40);
        dos_sb_write_rs(&mut s, 0xA6);
        if s.rate != 1_000_000 / 90 { fail += 1; }

        // Single-cycle transfer, length-1 = 0x0A6F, i.e. Aladdin's block.
        dos_sb_write_rs(&mut s, 0xD1);
        if s.speaker != 1 { fail += 1; }
        let armed = { dos_sb_write_rs(&mut s, 0x14); dos_sb_write_rs(&mut s, 0x6F); dos_sb_write_rs(&mut s, 0x0A) };
        if armed != 1 { fail += 1; }
        if s.active != 1 { fail += 1; }
        if s.autoinit != 0 { fail += 1; }
        if s.block_len != 0x0A70 { fail += 1; }

        // End of block: the IRQ is asserted, the read-status port ACKs it.
        dos_sb_raise_irq_rs(&mut s);
        if dos_sb_irq_pending_rs(&s) != 1 { fail += 1; }
        let _ = dos_sb_read_rs(&mut s, SB_OFF_RSTAT);
        if dos_sb_irq_pending_rs(&s) != 0 { fail += 1; }
        if s.irq_acked != 1 { fail += 1; }
        dos_sb_block_done_rs(&mut s);
        if s.active != 0 { fail += 1; }

        // Auto-init takes its length from 0x48 and does NOT clear active at
        // the end of a block.
        dos_sb_write_rs(&mut s, 0x48);
        dos_sb_write_rs(&mut s, 0xFF);
        dos_sb_write_rs(&mut s, 0x03);
        if s.block_len_48 != 0x0400 { fail += 1; }
        if dos_sb_write_rs(&mut s, 0x1C) != 1 { fail += 1; }
        if s.block_len != 0x0400 { fail += 1; }
        if s.autoinit != 1 { fail += 1; }
        dos_sb_block_done_rs(&mut s);
        if s.active != 1 { fail += 1; }
        // ...and 0xDA leaves it, which is the documented way out.
        dos_sb_write_rs(&mut s, 0xDA);
        dos_sb_block_done_rs(&mut s);
        if s.active != 0 { fail += 1; }

        // An UNKNOWN command must consume exactly its own byte and no more:
        // the next byte has to be read as a command, not swallowed as an
        // argument. This is the desynchronisation blame.md records.
        let before = s.cmd_unknown;
        dos_sb_write_rs(&mut s, 0x35); // no such command
        if s.cmd_unknown != before + 1 { fail += 1; }
        dos_sb_write_rs(&mut s, 0xD3);
        if s.speaker != 0 { fail += 1; } // 0xD3 was NOT eaten as an argument

        // A reset in the middle of a transfer stops it, which is the only way
        // out of a high-speed transfer on a real SB 2.0.
        dos_sb_write_rs(&mut s, 0x90);
        if s.active != 1 || s.high_speed != 1 { fail += 1; }
        dos_sb_reset_rs(&mut s, 1);
        dos_sb_reset_rs(&mut s, 0);
        if s.active != 0 || s.high_speed != 0 { fail += 1; }
    }

    // ---- ARM 3: the 8237, programmed exactly as Aladdin programs it.
    {
        let mut d = blank_dma();
        dos_dma_out_rs(&mut d, 0x0A, 0x05); // mask channel 1
        dos_dma_out_rs(&mut d, 0x0B, 0x59); // ch1, read, auto-init, single
        dos_dma_page_rs(&mut d, 1, 0x06);
        dos_dma_out_rs(&mut d, 0x0C, 0xFF); // clear flip-flop
        dos_dma_out_rs(&mut d, 0x02, 0x10);
        dos_dma_out_rs(&mut d, 0x02, 0xA7);
        dos_dma_out_rs(&mut d, 0x03, 0x6F);
        dos_dma_out_rs(&mut d, 0x03, 0x0A);
        if dos_dma_playback_armed_rs(&d, 1) != 0 { fail += 1; } // still masked
        dos_dma_out_rs(&mut d, 0x0A, 0x01); // unmask channel 1
        if dos_dma_playback_armed_rs(&d, 1) != 1 { fail += 1; }
        if dos_dma_cur_phys_rs(&d, 1) != 0x06A710 { fail += 1; }
        if dos_dma_block_bytes_rs(&d, 1) != 0x0A70 { fail += 1; }
        if dos_dma_autoinit_rs(&d, 1) != 1 { fail += 1; }

        // The count register the guest polls must MOVE, and must read back
        // through the flip-flop in the order the guest reads it.
        if dos_dma_set_played_rs(&mut d, 1, 0x100) != 0 { fail += 1; }
        // ONE read first, so the flip-flop is left HALF WAY THROUGH a pair.
        // Without this the next line proves nothing: the flip-flop happens to
        // be zero already after an even number of accesses, so a port 0x0C
        // that did nothing at all would pass. Measured: that exact mutant
        // survived the first version of this test.
        let _odd = dos_dma_in_rs(&mut d, 0x03);
        if d.ff != 1 { fail += 1; }
        dos_dma_out_rs(&mut d, 0x0C, 0xFF);
        if d.ff != 0 { fail += 1; }
        let lo = dos_dma_in_rs(&mut d, 0x03);
        let hi = dos_dma_in_rs(&mut d, 0x03);
        if ((hi as u16) << 8 | lo as u16) != 0x0A6F - 0x100 { fail += 1; }
        // Terminal count sets TC and, because this channel is auto-init,
        // reloads rather than stopping.
        if dos_dma_set_played_rs(&mut d, 1, 0x0A70) != 1 { fail += 1; }
        if d.ch[1].cur_count != 0x0A6F { fail += 1; }
        if dos_dma_in_rs(&mut d, 0x08) & 0x02 == 0 { fail += 1; }
        if dos_dma_in_rs(&mut d, 0x08) & 0x02 != 0 { fail += 1; } // read clears it

        // The 16-bit wrap inside the page, which is the whole reason
        // dos_dma_phys_at_rs exists. base 0xFF00 page 0x06, 512 bytes in,
        // is 0x060100 on real hardware and 0x070100 if someone adds the
        // offset to the 24-bit address instead.
        {
            let mut w = blank_dma();
            dos_dma_page_rs(&mut w, 1, 0x06);
            dos_dma_out_rs(&mut w, 0x0C, 0xFF);
            dos_dma_out_rs(&mut w, 0x02, 0x00);
            dos_dma_out_rs(&mut w, 0x02, 0xFF);
            if dos_dma_phys_at_rs(&w, 1, 0) != 0x06FF00 { fail += 1; }
            if dos_dma_phys_at_rs(&w, 1, 0x100) != 0x060000 { fail += 1; }
            if dos_dma_phys_at_rs(&w, 1, 0x200) != 0x060100 { fail += 1; }
        }

        // A NON-auto-init channel must stop at terminal count with the count
        // wrapped to 0xFFFF, which is what a completion poll looks for.
        let mut e = blank_dma();
        dos_dma_out_rs(&mut e, 0x0B, 0x49); // ch1, read, single, NO auto-init
        dos_dma_out_rs(&mut e, 0x0C, 0xFF);
        dos_dma_out_rs(&mut e, 0x03, 0x0F);
        dos_dma_out_rs(&mut e, 0x03, 0x00);
        dos_dma_out_rs(&mut e, 0x0A, 0x01);
        if dos_dma_set_played_rs(&mut e, 1, 16) != 1 { fail += 1; }
        if e.ch[1].cur_count != 0xFFFF { fail += 1; }

        // The flip-flop is per CONTROLLER. Writing the low byte of channel 1's
        // address and then touching channel 2 must not reset it: a per-channel
        // copy would pass every test above and fail here.
        let mut f = blank_dma();
        dos_dma_out_rs(&mut f, 0x0C, 0xFF);
        dos_dma_out_rs(&mut f, 0x02, 0x34); // ch1 low
        dos_dma_out_rs(&mut f, 0x04, 0x78); // ch2 low? no: it is ch2 HIGH
        if f.ch[2].base_addr != 0x7800 { fail += 1; }
    }

    // ---- ARM 4: format conversion, at both rails and the midpoint.
    {
        let src: [u8; 3] = [0x00, 0x80, 0xFF];
        let mut dst: [i16; 3] = [1, 1, 1];
        dos_sb_u8_to_s16_rs(src.as_ptr(), dst.as_mut_ptr(), 3);
        if dst[0] != -32768 { fail += 1; }
        if dst[1] != 0 { fail += 1; }
        if dst[2] != 32512 { fail += 1; }
    }

    // ---- ARM 5: rate arithmetic at the constants real titles use.
    {
        // These are the ACTUAL arithmetic results, not the nominal rates the
        // constants are named after: TC 0xA6 is "11 kHz" in every DOS setup
        // program and is really 11111 Hz, and TC 0xD3 is "22 kHz" and is
        // really 22222 Hz. Asserting the nominal figure would be asserting a
        // marketing number against a divider.
        if rate_from_tc(0xA6) != 11111 { fail += 1; } // "11 kHz"
        if rate_from_tc(0xD3) != 22222 { fail += 1; } // "22 kHz"
        if rate_from_tc(0x00) != 4000 { fail += 1; }  // clamped floor
        if rate_from_tc(0xFF) != 48000 { fail += 1; } // clamped ceiling
    }

    fail
}
