// fmsynth - #182: the DOS OPL2 consumer of the one FM synthesis core.
//
// ===========================================================================
// WHAT THIS IS
// ---------------------------------------------------------------------------
// A DOS guest writes FM registers to ports 0x388/0x389. The kernel timestamps
// those writes and queues them (kernel/rustkern/fmq.rs). This process drains
// that queue into the ONE synthesiser core (userland/lib/opl2) and pushes the
// rendered PCM at the SAME sink every other Ring-3 audio source uses.
//
// It contains no synthesis of its own and never will:
//
//     #[path = "../../lib/opl2/opl2.rs"] mod opl2;
//     chip.write_reg(reg, val);  chip.render_stereo(&mut buf);
//
// which is character for character how /APPS/FMTEST reaches the core, and how
// #183's MIDI player will. userland/lib/opl2/core-gate.sh fails the build if
// anyone writes a second one.
//
// ===========================================================================
// #426: WHERE THIS BLOCKS, AND WHY IT IS EXACTLY ONE PLACE
// ---------------------------------------------------------------------------
// The loop blocks in sys_audio_pcm_write's WAIT QUEUE and nowhere else. That
// call sleeps until the PCM pump has consumed frames, so the audio sink paces
// the whole process: render a block, hand it over, sleep until there is room,
// repeat. There is no sleep(1), no yield loop, and no poll.
//
// SYS_DOS_FM_EVENTS is deliberately NON-BLOCKING, and draining it once per
// audio block is NOT a poll by omission. Making it block would DEADLOCK the
// design: a sustaining note has to keep producing samples with no register
// writes arriving at all, so a process asleep on the event queue would stop
// feeding the sink and the note would cut off mid-way. The pacing has to come
// from the sink, and it does.
//
// ===========================================================================
// TIMING: WHY THE EVENTS CARRY TIMESTAMPS
// ---------------------------------------------------------------------------
// The naive version applies every drained write at the start of the next audio
// block. At 1024 frames that quantises every note-on to a 23 ms grid, which is
// audible as a mechanical, slightly-wrong rhythm on anything faster than a
// ballad. Instead each event carries mono_us() from the instant the guest
// issued it, and this renders up to the sample where it belongs before applying
// it. Same block, right position.
//
// The mapping from microseconds to samples self-corrects: `base_us` advances by
// exactly one block's worth of time per block, and is re-synced only when it
// has drifted by more than two blocks, which is what stops a slow clock and a
// fast clock from walking away from each other over a long game.

#![no_std]

use core::panic::PanicInfo;

#[path = "../../lib/opl2/opl2.rs"]
mod opl2;
use opl2::*;

#[panic_handler]
fn panic(_i: &PanicInfo) -> ! {
    unsafe {
        syscall1(SYS_EXIT, 103);
    }
    loop {}
}

extern "C" {
    fn syscall1(n: i64, a1: i64) -> i64;
    fn syscall2(n: i64, a1: i64, a2: i64) -> i64;
    fn syscall3(n: i64, a1: i64, a2: i64, a3: i64) -> i64;
}

// Syscall numbers. THE FIFTH-COPY HAZARD: a no_std Rust app cannot include the
// C header, so it holds its own copy, and a stale copy compiles fine and calls
// the WRONG syscall. The first-boot wizard shipped exactly that bug (#745), and
// rule 5 of kernel/tools/syscall-number-lint reads these very constants and
// caught SYS_EXIT written as 1 in the sibling app during this ticket. It is 0.
// Do not "tidy" these to look like the numbers you remember; run the lint.
const SYS_EXIT: i64 = 0;
const SYS_PUTCHAR: i64 = 40;
const SYS_AUDIO_PCM_OPEN: i64 = 315;
const SYS_AUDIO_PCM_WRITE: i64 = 316;
const SYS_AUDIO_PCM_CLOSE: i64 = 317;
const SYS_DOS_FM_EVENTS: i64 = 377;
/// Monotonic milliseconds since boot. Used ONLY by the health line below: this
/// loop's timeline is driven by the audio clock, never by this.
const SYS_UPTIME_MS: i64 = 252;
/// (#fmzombie) One line into the persistent /BOOTLOG.TXT, mirrored to serial by
/// the kernel as `[BOOTLOG] [USERSPACE uid=N] ...`.
///
/// EVERYTHING THIS PROCESS SAYS USED TO GO NOWHERE. `puts` below writes through
/// SYS_PUTCHAR, which sys_putchar() (proc/syscall.c) delivers to the caller's
/// fd 1 when it has one; /APPS/FMSYNTH is spawned by the kernel's own
/// launch_userspace_app() and its fd 1 is not the console, so its banner, its
/// error messages and its exit summary reached no log at all. MEASURED on a
/// stock golden Ring-3 DOS run: the serial capture contains every kernel line
/// ABOUT this process and not one line FROM it.
///
/// That is why this defect was reported as a CPU percentage rather than as a
/// message. The owner's machine has no serial port either, so the durable file
/// is the only channel that reaches him: SYS_BOOTLOG_WRITE is the one the DOS
/// layer already uses for exactly that reason (#307).
const SYS_BOOTLOG_WRITE: i64 = 298;

const AUDIO_FORMAT_S16_LE: i64 = 0x0002;

/// Returned by SYS_DOS_FM_EVENTS when no guest holds the chip AND the queue is
/// empty. It means "you may stop now", and it is the ONLY exit condition:
/// exiting on an empty queue alone would kill the synthesiser between two notes.
const FM_ENODEV: i64 = -6;
/// Another process latched the queue first.
const FM_EPERM: i64 = -5;

const RATE: u32 = 44100;
/// Frames per block. 1024 at 44100 is 23.2 ms.
///
/// SIZED FROM THE TWO THINGS IT TRADES OFF, not from a round number. Smaller
/// means more syscalls per second and more scheduling pressure on a machine
/// where a DOS guest ALREADY drops the desktop from 22 fps to 2 (#67). Larger
/// means more latency between a guest's note-on and the sound. 1024 costs about
/// 43 blocks per second, i.e. 86 syscalls per second, which is noise next to
/// what the DOS interpreter itself is doing, and the event timestamps mean the
/// block size does NOT quantise note timing.
const BLOCK: usize = 1024;

// .bss, NOT the stack. The Ring-3 stack is 16 KB.
static mut PCM: [i16; BLOCK * 2] = [0; BLOCK * 2];
static mut EVENTS: [FmEvent; 256] = [FmEvent { t_us: 0, reg: 0, val: 0, flags: 0, _pad: 0, seq: 0 }; 256];

/// Mirrors dos_fm_event_t in kernel/dos/dosexec.c and FmEvent in
/// kernel/rustkern/fmq.rs. 16 bytes, _Static_assert-locked on BOTH C sides.
#[repr(C)]
#[derive(Clone, Copy)]
struct FmEvent {
    t_us: u64,
    reg: u8,
    val: u8,
    flags: u8,
    _pad: u8,
    seq: u32,
}
const FMEV_RESET: u8 = 0x01;

/// The address of frame `n` inside PCM. Split out so the retry loop above reads
/// as arithmetic rather than as a pointer cast in the middle of a syscall.
fn unsafe_pcm_at(frame: i64) -> i64 {
    core::ptr::addr_of!(PCM) as i64 + frame * 2 * 2
}

/// A fixed 192-byte line for SYS_BOOTLOG_WRITE (the kernel bounces at 200 and
/// one call is one record, so a line is assembled whole and sent once).
struct Line {
    b: [u8; 192],
    n: usize,
}
impl Line {
    fn new() -> Line {
        Line { b: [0u8; 192], n: 0 }
    }
    fn s(&mut self, t: &str) -> &mut Line {
        for c in t.as_bytes() {
            if self.n < self.b.len() - 1 {
                self.b[self.n] = *c;
                self.n += 1;
            }
        }
        self
    }
    fn i(&mut self, mut v: i64) -> &mut Line {
        if v < 0 {
            self.s("-");
            v = -v;
        }
        if v == 0 {
            return self.s("0");
        }
        let mut d = [0u8; 24];
        let mut k = 0;
        while v > 0 {
            d[k] = b'0' + (v % 10) as u8;
            v /= 10;
            k += 1;
        }
        while k > 0 {
            k -= 1;
            if self.n < self.b.len() - 1 {
                self.b[self.n] = d[k];
                self.n += 1;
            }
        }
        self
    }
    /// Send it, and ALSO put it on the SYS_PUTCHAR channel. Both, because the
    /// two reach different readers: the bootlog reaches a machine with no
    /// serial port, and the putchar channel is what a terminal-hosted run sees.
    fn emit(&mut self) {
        self.b[self.n] = 0;
        unsafe {
            syscall1(SYS_BOOTLOG_WRITE, self.b.as_ptr() as i64);
        }
        for i in 0..self.n {
            putc(self.b[i]);
        }
        putc(b'\n');
    }
}

fn putc(c: u8) {
    unsafe {
        syscall1(SYS_PUTCHAR, c as i64);
    }
}
fn puts(s: &str) {
    for b in s.as_bytes() {
        putc(*b);
    }
}
fn put_i64(mut v: i64) {
    if v < 0 {
        putc(b'-');
        v = -v;
    }
    if v == 0 {
        putc(b'0');
        return;
    }
    let mut d = [0u8; 24];
    let mut n = 0;
    while v > 0 {
        d[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    while n > 0 {
        n -= 1;
        putc(d[n]);
    }
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    puts("[FMSYNTH] #182 OPL2 FM synthesiser (Ring 3), rate ");
    put_i64(RATE as i64);
    puts(" block ");
    put_i64(BLOCK as i64);
    puts("\n");

    let h = unsafe { syscall3(SYS_AUDIO_PCM_OPEN, RATE as i64, 2, AUDIO_FORMAT_S16_LE) };
    if h < 1 {
        // NO SINK, SO NO SYNTHESISER. Exiting here is the correct behaviour and
        // it is what keeps the kernel's PRESENT answer honest: the OPL2 only
        // reports PRESENT because this process launched, and if it cannot make
        // a sound it must not pretend to. It exits, and the next DOS guest gets
        // ABSENT.
        puts("[FMSYNTH] sys_audio_pcm_open failed (");
        put_i64(h);
        puts("): no audio sink, exiting. The OPL2 will report ABSENT.\n");
        return 1;
    }

    let mut chip = Opl2::new();
    chip.init(RATE);

    // The audio timeline. `base_us` is the guest-clock instant that the FIRST
    // sample of the block about to be rendered corresponds to.
    let mut base_us: u64 = 0;
    let mut have_base = false;
    let block_us: u64 = (BLOCK as u64) * 1_000_000 / RATE as u64;

    let mut total_events: i64 = 0;
    let mut total_blocks: i64 = 0;
    let mut resyncs: i64 = 0;
    let mut seq_gaps: i64 = 0;
    let mut last_seq: u32 = 0;
    let mut have_seq = false;
    let mut guest_gone = false;
    let mut tail_blocks: i64 = 0;

    // ---- #fmzombie: THE HEALTH LINE, AND WHY IT IS A RATE ------------------
    // This process is paced by ONE thing, sys_audio_pcm_write's wait queue, so
    // the single number that says whether it is healthy is HOW MANY BLOCKS PER
    // SECOND it completes. At 1024 frames and 44100 Hz that is 43. A rate far
    // above 43 means the write is returning without blocking and this loop is a
    // busy-wait, which is exactly the thing CLAUDE.md #426 bans and exactly what
    // a reader looking at 89% of a core needs told. A rate far below 43 means
    // the sink is starving it. Neither is visible from the outside: the OS-level
    // view is a CPU percentage, which cannot distinguish "synthesising hard"
    // from "spinning on a write that accepts nothing".
    //
    // Emitted on WALL time, not on a block count, so a spinning loop cannot
    // flood the serial port with its own evidence.
    let mut last_report_ms: i64 = unsafe { syscall1(SYS_UPTIME_MS, 0) };
    let mut last_report_blocks: i64 = 0;
    let mut zero_writes: i64 = 0;

    loop {
        // ---- 1. DRAIN. Non-blocking; see the header. ----------------------
        let n = unsafe {
            syscall2(
                SYS_DOS_FM_EVENTS,
                core::ptr::addr_of_mut!(EVENTS) as i64,
                256,
            )
        };
        if n == FM_EPERM {
            puts("[FMSYNTH] another process already owns the FM queue; exiting.\n");
            break;
        }
        // (#fmzombie) ANY OTHER NEGATIVE IS A REFUSAL, AND IGNORING IT IS THE
        // SAME MISTAKE AS IGNORING A ZERO WRITE. The old code recognised
        // exactly two negatives and let every other one fall through to the
        // `n > 0` test, which is false, so an unrecognised error became a
        // silent no-op repeated for the life of the process. dos_fm_drain()
        // already returns -1 for a bad argument, and a kernel without
        // SYS_DOS_FM_EVENTS returns -ENOSYS; either way this process has no
        // source of events and no way to learn the guest has gone, so it must
        // stop rather than render silence for ever.
        if n < 0 && n != FM_ENODEV {
            puts("[FMSYNTH] SYS_DOS_FM_EVENTS returned an unrecognised error (");
            put_i64(n);
            puts("): no event source, exiting.\n");
            break;
        }
        if n == FM_ENODEV {
            // The guest is gone AND the queue is empty. Do NOT stop here: any
            // note still sounding has to be allowed to finish, or every DOS
            // session would end with an abrupt click.
            guest_gone = true;
        } else if n > 0 {
            total_events += n;
            let evs = unsafe { &*core::ptr::addr_of!(EVENTS) };
            for i in 0..(n as usize) {
                let e = evs[i];

                // A sequence gap means the kernel ring overflowed and register
                // writes were lost. REPORT IT. A synthesiser that silently
                // plays the wrong notes because it lost a note-on is the
                // unexplained-wrongness failure this whole ticket is about.
                if have_seq && e.seq != last_seq.wrapping_add(1) {
                    seq_gaps += 1;
                }
                last_seq = e.seq;
                have_seq = true;

                if e.flags & FMEV_RESET != 0 {
                    // A new guest has the chip. Reset to power-on state so no
                    // instrument or key-on survives from the previous game.
                    chip.init(RATE);
                    base_us = e.t_us;
                    have_base = true;
                }
                if !have_base {
                    base_us = e.t_us;
                    have_base = true;
                }

                // BOUNDED SELF-CORRECTION. `base_us` advances by exactly one
                // block per block, on the AUDIO clock. Guest events arrive on
                // mono_us(), the CPU clock. Those two are close but not equal,
                // so over a long game they walk apart, and once they have
                // walked more than a block the sample offset computed below
                // saturates and every write lands at one end of the block.
                //
                // Re-anchoring on EVERY event would be wrong in the other
                // direction: it would make the block position jitter with the
                // scheduler and destroy the timing this whole mechanism exists
                // to preserve. So it re-anchors only when the drift exceeds two
                // blocks, which is large enough never to fire on jitter and
                // small enough that no note is ever misplaced by more than
                // that.
                //
                // The counter is REPORTED at exit. A run with many resyncs
                // means the two clocks genuinely disagree and is worth knowing;
                // a run with none means the timeline held.
                let ahead = e.t_us > base_us && e.t_us - base_us > 2 * block_us;
                let behind = base_us > e.t_us && base_us - e.t_us > 2 * block_us;
                if ahead || behind {
                    base_us = e.t_us;
                    resyncs += 1;
                }

                // ---- render UP TO the sample where this write belongs -----
                let off_us = if e.t_us > base_us { e.t_us - base_us } else { 0 };
                let mut pos = (off_us * RATE as u64 / 1_000_000) as usize;
                if pos > BLOCK {
                    pos = BLOCK;
                }
                // Rendered-so-far is tracked by how much of PCM is filled; see
                // `filled` below. Handled in the render step so that the whole
                // block is produced exactly once.
                apply_at(&mut chip, pos, e.reg, e.val);
            }
        }

        // ---- 2. RENDER one block, applying each pending write in place ----
        let filled = unsafe {
            let p = &mut *core::ptr::addr_of_mut!(PCM);
            render_block(&mut chip, p)
        };
        let _ = filled;
        total_blocks += 1;

        // ---- 3. HAND IT TO THE SINK. THIS IS THE ONLY BLOCKING CALL. ------
        //
        // (#fmzombie) A ZERO IS NOT A SUCCESS, AND TESTING ONLY `w < 0` IS WHAT
        // TURNED THIS LOOP INTO A BUSY-WAIT.
        //
        // sys_audio_pcm_write blocks in the PCM ring's wait queue while the
        // stream is live, and that block is the ONLY thing pacing this process:
        // the drain above is deliberately non-blocking and the render is pure
        // computation. So the moment the write stops blocking, this loop has no
        // pacing left at all and spins at the speed of the CPU.
        //
        // pcm_write_common() (kernel/drivers/audio_pcm.c) opens its loop with
        // `if (s->stop) break;` and returns the frame count it managed to write,
        // so a stream whose pump or mixer has gone away returns ZERO, every
        // time, immediately. The old test here was `if (w < 0)`, which reads a
        // zero as a successful write, and the process then renders and offers
        // block after block that nothing accepts. That is the 89%-of-a-core the
        // owner saw, and it is the hand-rolled poll CLAUDE.md #426 bans, arrived
        // at by omission rather than by writing a poll on purpose.
        //
        // The kernel's OWN callers already knew this: drivers/audio.c:262 tests
        // `if (w <= 0) break;` with the comment "stream gone; do not spin on
        // it", and media/audio_decode.c:493 tests `if (w <= 0)`. Every C
        // consumer of this call had the right test. This one did not.
        //
        // A SHORT WRITE IS ALSO NOT A FULL ONE. The same function can return a
        // partial count (it stops early if the stream is torn down mid-block,
        // and returns what it managed). The old code discarded the remainder
        // silently, which is dropped audio nobody could account for. Offer the
        // rest, and only give up when the sink accepts nothing at all.
        let mut sent: i64 = 0;
        let mut w: i64 = 0;
        let mut dead = false;
        while sent < BLOCK as i64 {
            w = unsafe {
                syscall3(
                    SYS_AUDIO_PCM_WRITE,
                    h,
                    unsafe_pcm_at(sent),
                    BLOCK as i64 - sent,
                )
            };
            if w < 0 {
                // Through the bootlog, not puts(): see SYS_BOOTLOG_WRITE above.
                // A message about why this process stopped is worthless on a
                // channel that reaches no log, and that is exactly how this
                // defect came to be reported as a CPU percentage.
                let mut l = Line::new();
                l.s("[FMSYNTH] sys_audio_pcm_write failed (").i(w)
                 .s("), exiting after ").i(sent).s(" of ").i(BLOCK as i64)
                 .s(" frames");
                l.emit();
                dead = true;
                break;
            }
            if w == 0 {
                // The sink accepted NOTHING. It is stopped or torn down: a
                // blocking write that returns zero has told us there is nothing
                // left to pace us. Stop, rather than offer it the same block
                // for the rest of the boot.
                zero_writes += 1;
                let mut l = Line::new();
                l.s("[FMSYNTH] the PCM sink accepted 0 of ")
                 .i(BLOCK as i64 - sent)
                 .s(" frames after ").i(total_blocks)
                 .s(" blocks: the stream is stopped, so nothing is left to ")
                 .s("pace this loop. Exiting rather than spinning.");
                l.emit();
                dead = true;
                break;
            }
            sent += w;
        }
        if dead {
            break;
        }

        // ---- 4. advance the timeline, with bounded self-correction --------
        base_us += block_us;

        if w == 0 { zero_writes += 1; }

        // ---- 4b. #fmzombie HEALTH LINE ------------------------------------
        {
            let now_ms = unsafe { syscall1(SYS_UPTIME_MS, 0) };
            let dt = now_ms - last_report_ms;
            if dt >= 5000 {
                let db = total_blocks - last_report_blocks;
                let mut l = Line::new();
                l.s("[FMSYNTH] blocks=").i(total_blocks)
                 .s(" +").i(db).s(" in ").i(dt).s("ms = ")
                 .i(db * 1000 / if dt > 0 { dt } else { 1 })
                 .s(" blk/s (44100/1024 = 43 is correct; far above 43 means the ")
                 .s("PCM write stopped blocking and this is a SPIN) events=")
                 .i(total_events).s(" lastwrite=").i(w)
                 .s(" zerowrites=").i(zero_writes)
                 .s(" lastdrain=").i(n)
                 .s(" guestgone=").i(if guest_gone { 1 } else { 0 });
                l.emit();
                last_report_ms = now_ms;
                last_report_blocks = total_blocks;
            }
        }

        // ---- 5. exit condition --------------------------------------------
        if guest_gone {
            if chip.is_silent() {
                break;
            }
            // Bounded tail. A note whose release rate is 0 never decays, and
            // without this bound the process would render silence forever for a
            // guest that is already gone. 5 seconds is longer than the longest
            // release the chip can produce at any rate above 0.
            tail_blocks += 1;
            if tail_blocks > (RATE as i64 * 5) / BLOCK as i64 {
                puts("[FMSYNTH] guest gone and a voice is still sounding after 5 s ");
                puts("(release rate 0?); stopping.\n");
                break;
            }
        }
    }

    unsafe {
        syscall1(SYS_AUDIO_PCM_CLOSE, h);
    }

    {
        let mut l = Line::new();
        l.s("[FMSYNTH] done: ").i(total_events).s(" regwrites ")
         .i(total_blocks).s(" blocks ").i(resyncs).s(" resyncs ")
         .i(seq_gaps).s(" seqgaps ").i(zero_writes).s(" zerowrites");
        if seq_gaps > 0 {
            l.s(" <<<< EVENTS WERE LOST");
        }
        l.emit();
    }
    puts("[FMSYNTH] done: ");
    put_i64(total_events);
    puts(" register writes, ");
    put_i64(total_blocks);
    puts(" blocks, ");
    put_i64(resyncs);
    puts(" resyncs, ");
    put_i64(seq_gaps);
    puts(" sequence gaps, ");
    put_i64(zero_writes);
    puts(" zero-length writes");
    if seq_gaps > 0 {
        puts(" <<<< EVENTS WERE LOST: expect wrong or stuck notes");
    }
    puts("\n");
    0
}

/// Pending register writes for the block being assembled, each with the sample
/// offset inside the block at which it takes effect.
///
/// A fixed array rather than an allocation: this app has no allocator, and a
/// bound here is a bound on how much of a burst one block can express. 256
/// matches the drain size, and the largest measured corpus burst is Keen 5's
/// 264-register instrument load, which simply spills into the next block and
/// arrives 23 ms later. That is inaudible for an instrument bank and is the
/// right trade against a heap.
static mut PENDING: [(u16, u8, u8); 256] = [(0, 0, 0); 256];
static mut PENDING_N: usize = 0;

fn apply_at(_chip: &mut Opl2, pos: usize, reg: u8, val: u8) {
    // SAFETY: single-threaded, and PENDING is drained by render_block below
    // before the next drain can add to it.
    unsafe {
        let n = PENDING_N;
        if n < 256 {
            (*core::ptr::addr_of_mut!(PENDING))[n] = (pos as u16, reg, val);
            PENDING_N = n + 1;
        }
    }
}

/// Render one block, applying each pending register write at its own sample
/// offset rather than all of them at the block boundary.
///
/// This is the difference between a melody and a melody quantised to a 23 ms
/// grid, and it is the entire reason the kernel bothers to timestamp events.
fn render_block(chip: &mut Opl2, out: &mut [i16]) -> usize {
    let frames = out.len() / 2;
    let mut cursor = 0usize;
    // SAFETY: single-threaded; PENDING_N is only written by apply_at, which
    // this function's caller has finished calling for this block.
    let n = unsafe { PENDING_N };
    let pend = unsafe { &*core::ptr::addr_of!(PENDING) };
    for i in 0..n {
        let (pos, reg, val) = pend[i];
        let mut p = pos as usize;
        if p > frames {
            p = frames;
        }
        if p > cursor {
            chip.render_stereo(&mut out[cursor * 2..p * 2]);
            cursor = p;
        }
        chip.write_reg(reg, val);
    }
    if cursor < frames {
        chip.render_stereo(&mut out[cursor * 2..frames * 2]);
    }
    unsafe {
        PENDING_N = 0;
    }
    frames
}
