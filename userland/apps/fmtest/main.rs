// fmtest - #182: run the OPL2 FM core's objective test INSIDE MayteraOS, in
// Ring 3, and then push a known tone at the real PCM sink.
//
// ===========================================================================
// THIS IS THE SECOND CONSUMER, AND THAT IS HALF ITS PURPOSE
// ---------------------------------------------------------------------------
// #182's decision is ONE synthesiser core with TWO consumers. A core with one
// consumer has not been shown to be reusable; it has been shown to compile. So
// this app and /APPS/FMSYNTH reach the core the same way, through the same
// include, calling the same two methods:
//
//     #[path = "../../lib/opl2/opl2.rs"] mod opl2;
//     chip.write_reg(reg, val);  chip.render(&mut buf);
//
// and #183's MIDI player will be the third. userland/lib/opl2/core-gate.sh
// fails the build if anyone writes a second core instead.
//
// ===========================================================================
// WHAT THIS PROVES THAT THE HOST HARNESS CANNOT
// ---------------------------------------------------------------------------
// The core is integer-only with no I/O, so the host harness
// (userland/lib/opl2/hosttest) runs bit-identical arithmetic and is the fast
// falsifiable loop. It is NOT a substitute for this, and the distinction is
// reported rather than blurred:
//
//   host arm proves   the synthesis maths
//   THIS arm proves   the Ring-3 ELF links and loads; the 64 KB buffers are in
//                     .bss and reachable (a 64 KB buffer on the 16 KB stack is
//                     the exact fault that bit the earlier Rust userland port);
//                     the arithmetic is the same after the userland toolchain
//                     has had its way with it; and the PCM sink accepts what
//                     the core produces.
//
// ===========================================================================
// THE SINK TEST, AND AN HONEST STATEMENT OF ITS LIMIT
// ---------------------------------------------------------------------------
// After the suite, this renders A440 and hands it to sys_audio_pcm_write, then
// analyses THE EXACT BUFFER IT HANDED OVER. So the samples that were measured
// are literally the bytes that crossed the syscall boundary, not a separate
// rendering that resembles them.
//
// WHAT IT DOES NOT PROVE: that the DAC produced 440 Hz. There is no loopback in
// this machine, so nothing in Ring 3 can observe the analogue output. The
// buffer at the syscall boundary is as far as an automated test can honestly
// reach, and saying "440 Hz came out of the speaker" on this evidence would be
// exactly the fabrication this ticket exists to avoid. What is claimed is: the
// core produced 440 Hz, and those samples were accepted by the sink.
//
// #426: this app blocks in exactly one place, sys_audio_pcm_write's wait queue.
// There is no spin, no poll and no proc_yield anywhere in it.

#![no_std]

use core::panic::PanicInfo;

#[path = "../../lib/opl2/opl2.rs"]
mod opl2;
use opl2::*;

#[panic_handler]
fn panic(_i: &PanicInfo) -> ! {
    // panic=abort in Ring 3: exit loudly rather than spin. A spin here would
    // burn a core forever (#426).
    unsafe {
        syscall1(SYS_EXIT, 102);
    }
    loop {}
}

extern "C" {
    fn syscall1(n: i64, a1: i64) -> i64;
    fn syscall3(n: i64, a1: i64, a2: i64, a3: i64) -> i64;
}

// Syscall numbers. THIS IS THE FIFTH-COPY HAZARD (see
// kernel/tools/syscall-number-lint rule 5): a no_std Rust app cannot include
// the C header, so it holds its own copy, and a stale copy here compiles fine
// and calls the wrong syscall. The first-boot wizard shipped exactly that bug.
// Rule 5 of the lint reads these constants, so they cannot drift silently.
const SYS_EXIT: i64 = 0;
const SYS_PUTCHAR: i64 = 40;
const SYS_OPEN: i64 = 10;
const SYS_CLOSE: i64 = 11;
const SYS_WRITE: i64 = 13;
const O_WRONLY: i64 = 0x0001;
const O_CREAT: i64 = 0x0040;
const O_TRUNC: i64 = 0x0200;
const SYS_AUDIO_PCM_OPEN: i64 = 315;
const SYS_AUDIO_PCM_WRITE: i64 = 316;
const SYS_AUDIO_PCM_CLOSE: i64 = 317;

const AUDIO_FORMAT_S16_LE: i64 = 0x0002;

const RATE: u32 = 44100;
const NSAMP: usize = 32768;

// .bss, NOT the stack. The Ring-3 stack is 16 KB and these are 64 KB and 320
// bytes. Putting the render buffer on the stack is the fault that bit the
// earlier Rust userland port, and it does not fail at the declaration: it
// corrupts whatever is below it and the app dies somewhere unrelated.
static mut BUF: [i16; NSAMP] = [0; NSAMP];
static mut ENVBUF: [u16; NSAMP / 256 + 4] = [0; NSAMP / 256 + 4];
static mut SINKBUF: [i16; 4096] = [0; 4096];

// The report, mirrored into .bss so it can be written to a file at the end.
//
// EVERY byte that goes to putc() also lands here. Two sinks, ONE formatting
// path: if the file said something different from the console it would be
// impossible to know which one to believe, and having two formatters is the
// same fragmentation mistake this ticket is about, at a smaller scale.
static mut REPORT: [u8; 8192] = [0; 8192];
static mut REPORT_N: usize = 0;

fn putc(c: u8) {
    unsafe {
        syscall1(SYS_PUTCHAR, c as i64);
        // AND into the report buffer. See the REPORT comment: one formatter,
        // two sinks. Silently truncating at capacity rather than wrapping,
        // because a wrapped report would look complete and be a lie; the
        // truncation is visible as a report that stops mid-line.
        let n = REPORT_N;
        if n < 8192 {
            (*core::ptr::addr_of_mut!(REPORT))[n] = c;
            REPORT_N = n + 1;
        }
    }
}

/// Write the accumulated report to `path`. Returns the byte count, or < 0.
///
/// This is the ONLY reason this app can be believed from outside: it was run
/// once with the report going only to SYS_PUTCHAR, and not one character
/// reached the serial console, because a kernel-spawned process has a stdout fd
/// and SYS_PUTCHAR only falls through to the kernel console when it does not.
/// The exit code was the whole of the evidence, and an exit code is one bit.
fn write_report(path: &[u8]) -> i64 {
    unsafe {
        let n = REPORT_N;
        if n == 0 {
            return 0;
        }
        let fd = syscall3(SYS_OPEN, path.as_ptr() as i64, O_WRONLY | O_CREAT | O_TRUNC, 0o644);
        if fd < 0 {
            return fd;
        }
        let w = syscall3(SYS_WRITE, fd, core::ptr::addr_of!(REPORT) as i64, n as i64);
        syscall1(SYS_CLOSE, fd);
        w
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
    let mut d = [0u8; 24];
    let mut n = 0;
    if v == 0 {
        putc(b'0');
        return;
    }
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

/// Right-align an integer in `w` columns.
fn put_i64_w(v: i64, w: usize) {
    let mut len = if v < 0 { 1 } else { 0 };
    let mut t = if v < 0 { -v } else { v };
    if t == 0 {
        len += 1;
    }
    while t > 0 {
        len += 1;
        t /= 10;
    }
    let mut i = len;
    while i < w {
        putc(b' ');
        i += 1;
    }
    put_i64(v);
}

/// Left-align a string in `w` columns.
fn put_str_w(s: &str, w: usize) {
    puts(s);
    let mut i = s.len();
    while i < w {
        putc(b' ');
        i += 1;
    }
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    puts("\n[FMTEST] #182 OPL2 FM core, IN-OS arm (Ring 3), rate 44100 Hz\n");
    puts("[FMTEST] this is the SECOND consumer of userland/lib/opl2; the first\n");
    puts("[FMTEST] is /APPS/FMSYNTH and both use the same write_reg/render pair.\n");
    puts("[FMTEST] A440 patch: FNUM ");
    put_i64(A440_FNUM as i64);
    puts(" BLOCK ");
    put_i64(A440_BLOCK as i64);
    puts(" -> expected ");
    put_i64(expected_mhz(A440_FNUM, A440_BLOCK) as i64);
    puts(" mHz\n\n");

    puts("[FMTEST]   CHECK                RESULT      MEASURED     EXPECTED\n");

    let mut emit = |r: &Report| {
        puts("[FMTEST]   ");
        put_str_w(r.name, 21);
        puts(if r.pass { "  PASS  " } else { "  FAIL  " });
        put_i64_w(r.measured, 12);
        put_i64_w(r.expected, 13);
        puts("  ");
        puts(r.note);
        putc(b'\n');
    };

    // SAFETY: single-threaded app, and these statics are touched only here and
    // in the sink block below, which runs after run_all has returned.
    let fails = unsafe {
        let b = &mut *core::ptr::addr_of_mut!(BUF);
        let e = &mut *core::ptr::addr_of_mut!(ENVBUF);
        run_all(RATE, b, e, &mut emit)
    };

    puts("\n[FMTEST] SUITE: ");
    put_i64(fails as i64);
    puts(" failing checks");
    if fails == 0 {
        puts(" (the RED arms PASSED by correctly FAILING their assertion)\n");
    } else {
        puts(" <<<< SUITE FAILED\n");
    }

    // -----------------------------------------------------------------------
    // THE SINK TEST
    // -----------------------------------------------------------------------
    puts("\n[FMTEST] --- sink test: A440 through sys_audio_pcm_write ---\n");
    let h = unsafe { syscall3(SYS_AUDIO_PCM_OPEN, RATE as i64, 2, AUDIO_FORMAT_S16_LE) };
    if h < 1 {
        // NOT a suite failure and NOT silently ignored. A VM with no audio
        // device is a legitimate configuration, and reporting "sink test
        // passed" there would be a fabrication. Reporting nothing at all would
        // let a real sink regression hide behind an absent device.
        puts("[FMTEST] SINK UNAVAILABLE: sys_audio_pcm_open returned ");
        put_i64(h);
        puts("\n[FMTEST] SINK: SKIPPED (no audio device). The synthesis result\n");
        puts("[FMTEST]       above stands on its own; this arm is UNVERIFIED.\n");
        puts("[FMTEST] DONE fails=");
        put_i64(fails as i64);
        puts(" sink=skipped\n");
        write_report(b"/FMTEST.TXT\0");
        return if fails == 0 { 0 } else { 1 };
    }

    let mut chip = Opl2::new();
    chip.init(RATE);
    patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
    key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);

    let mut frames_written: i64 = 0;
    let mut first_block_mhz: i64 = 0;
    let mut short_writes: i64 = 0;

    // ~1.5 seconds of A440. Long enough to be unmistakable if anyone is
    // listening, short enough that an automated run is not waiting on it.
    let blocks = (RATE as usize * 3 / 2) / 2048;
    for bi in 0..blocks {
        // SAFETY: single-threaded, and BUF/ENVBUF are no longer in use.
        let sb = unsafe { &mut *core::ptr::addr_of_mut!(SINKBUF) };
        chip.render_stereo(sb); // 2048 frames, interleaved L/R

        if bi == 0 {
            // MEASURE THE BYTES WE ARE ABOUT TO HAND OVER, not a separate
            // rendering of the same patch. Deinterleave the left channel in
            // place into the tail of BUF, which is free again.
            // SAFETY: as above.
            // from_raw_parts_mut rather than `&mut (*addr_of_mut!(BUF))[..n]`:
            // the latter is an implicit autoref through a raw pointer, which
            // rustc 1.97 rejects outright (dangerous_implicit_autorefs). This
            // form states the length once and takes no intermediate reference
            // to the whole array.
            let mono = unsafe {
                core::slice::from_raw_parts_mut(core::ptr::addr_of_mut!(BUF) as *mut i16, 2048)
            };
            for i in 0..2048 {
                mono[i] = sb[i * 2];
            }
            first_block_mhz = dominant_mhz(mono, RATE) as i64;
        }

        let n = unsafe {
            syscall3(
                SYS_AUDIO_PCM_WRITE,
                h,
                sb.as_ptr() as i64,
                2048,
            )
        };
        if n < 0 {
            puts("[FMTEST] SINK: write returned ");
            put_i64(n);
            putc(b'\n');
            break;
        }
        if n != 2048 {
            short_writes += 1;
        }
        frames_written += n;
    }

    unsafe {
        syscall1(SYS_AUDIO_PCM_CLOSE, h);
    }

    puts("[FMTEST] SINK: handle ");
    put_i64(h);
    puts(", frames accepted ");
    put_i64(frames_written);
    puts(", short writes ");
    put_i64(short_writes);
    putc(b'\n');
    puts("[FMTEST] SINK: dominant frequency OF THE BUFFER HANDED TO THE KERNEL: ");
    put_i64(first_block_mhz);
    puts(" mHz\n");

    // 2048 frames is 46 ms, i.e. only ~20 cycles of A440, so the zero-crossing
    // estimator has far less to work with than in the suite above. The band is
    // widened to match, and stated, rather than being quietly tightened until
    // it passed.
    let want = expected_mhz(A440_FNUM, A440_BLOCK) as i64;
    let err = if first_block_mhz > want { first_block_mhz - want } else { want - first_block_mhz };
    let sink_ok = frames_written > 0 && err <= 5000;
    puts("[FMTEST] SINK: ");
    if sink_ok {
        puts("PASS (within 5000 mHz over a 46 ms window; expected ");
    } else {
        puts("FAIL (outside 5000 mHz; expected ");
    }
    put_i64(want);
    puts(" mHz)\n");
    puts("[FMTEST] NOTE: this measures the buffer at the syscall boundary. It\n");
    puts("[FMTEST]       does NOT observe the DAC; there is no loopback here.\n");

    let bad = if fails == 0 && sink_ok { 0 } else { 1 };
    puts("[FMTEST] DONE fails=");
    put_i64(fails as i64);
    puts(" sink=");
    puts(if sink_ok { "ok" } else { "bad" });
    puts(" exit=");
    put_i64(bad as i64);
    putc(b'\n');
    // Written LAST, after every line is in the buffer. The return value is
    // reported into the buffer it cannot itself appear in, so it goes to the
    // console only; that is the one asymmetry and it is stated rather than
    // hidden.
    let w = write_report(b"/FMTEST.TXT\0");
    puts("[FMTEST] report written to /FMTEST.TXT (");
    put_i64(w);
    puts(" bytes)\n");
    bad
}
