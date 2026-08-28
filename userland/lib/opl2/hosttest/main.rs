// hosttest/main.rs - #182: run the FM core's self-test on an ordinary Linux
// host, in about a second, with no VM.
//
// WHY THIS EXISTS ALONGSIDE THE IN-OS /APPS/FMTEST
// ------------------------------------------------
// The core is integer-only and has no I/O, so THE SAME SOURCE compiles for the
// host and for Ring 3 and produces bit-identical arithmetic. That makes a host
// run real evidence about the shipped binary rather than about a lookalike, and
// it turns the edit-test loop from "build a kernel, build an image, boot a VM,
// read serial" into one command.
//
// It does NOT replace the in-OS run, and this file is not allowed to imply it
// does. The host arm proves the SYNTHESIS. Only the in-OS arm proves that the
// Ring-3 ELF links, that its .bss buffers are reachable, that the PCM sink
// accepts the format, and that nothing in the syscall path corrupts the
// samples. Both are reported separately in the CHANGELOG for exactly that
// reason.
//
// Build and run:
//   rustc --edition 2021 -O -o /tmp/fmhosttest userland/lib/opl2/hosttest/main.rs
//   /tmp/fmhosttest
// or use userland/lib/opl2/hosttest/run.sh, which also runs the table gate.
//
// Exit status is the number of FAILING checks, capped at 125, so it is usable
// as a gate: 0 is the only pass.

#[path = "../opl2.rs"]
mod opl2;

use opl2::*;

const RATE: u32 = 44100;
const NSAMP: usize = 32768; // 0.743 s: 327 cycles of A440, plenty for the
                            // zero-crossing estimator and small enough that the
                            // whole suite runs in well under a second.

fn main() {
    let mut buf = vec![0i16; NSAMP];
    let mut env = vec![0u16; NSAMP / 256 + 4];

    println!("#182 OPL2 FM core self-test  (host arm, rustc {}, rate {} Hz)",
             option_env!("RUSTC_VERSION").unwrap_or("unpinned-host"), RATE);
    println!("  chip rate {}/{} = {:.4} Hz",
             CHIP_CLOCK_HZ, CHIP_CLOCK_DIV,
             CHIP_CLOCK_HZ as f64 / CHIP_CLOCK_DIV as f64);
    println!("  A440 patch: FNUM {} BLOCK {} -> expected {} mHz ({} cents from 440.000)",
             A440_FNUM, A440_BLOCK,
             expected_mhz(A440_FNUM, A440_BLOCK),
             cents_x100(expected_mhz(A440_FNUM, A440_BLOCK), 440_000) as f64 / 100.0);
    println!();
    println!("  {:<20} {:>6}  {:>12} {:>12}  {}", "CHECK", "RESULT", "MEASURED", "EXPECTED", "WHAT IT MEANS");

    let mut emit = |r: &Report| {
        println!("  {:<20} {:>6}  {:>12} {:>12}  {}",
                 r.name,
                 if r.pass { "PASS" } else { "FAIL" },
                 r.measured, r.expected, r.note);
    };

    let fails = run_all(RATE, &mut buf, &mut env, &mut emit);

    println!();
    if fails == 0 {
        println!("  RESULT: 0 failing checks. The RED arms above are counted as PASS");
        println!("          because they correctly FAILED the assertion they were");
        println!("          given, which is what makes the GREEN arms mean anything.");
    } else {
        println!("  RESULT: {} FAILING CHECKS", fails);
    }
    std::process::exit(if fails > 125 { 125 } else { fails });
}
