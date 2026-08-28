// hosttest/main.rs - #183: run the MIDI player's self-test on an ordinary Linux
// host, in about a second, with no VM.
//
// SAME REASONING AS userland/lib/opl2/hosttest, and the same limits stated the
// same way. The library is integer-only with no I/O, so THE SAME SOURCE
// compiles for the host and for Ring 3 and produces bit-identical arithmetic.
// That makes a host run evidence about the shipped binary rather than about a
// lookalike, and it turns the edit-test loop from "build a kernel, build an
// image, boot a VM, read serial" into one command.
//
// IT DOES NOT REPLACE THE IN-OS RUN AND THIS FILE MAY NOT IMPLY THAT IT DOES:
//
//   host arm proves   the parsing, the tempo map, the GM mapping and the
//                     synthesis, all of which are pure integer computation.
//   in-OS arm proves  the Ring-3 ELF links and loads; the .bss buffers are
//                     reachable; the arithmetic survives the userland
//                     toolchain; and the PCM sink accepts what this produces.
//   NEITHER proves    that a speaker made a sound. See the WAV capture in the
//                     CHANGELOG for the one arm that reaches past the sink.
//
// Build and run:
//   rustc --edition 2021 -O -o /tmp/midihosttest userland/lib/midi/hosttest/main.rs
//   /tmp/midihosttest
//
// Exit status is the number of FAILING checks, capped at 125, so it is usable
// as a gate: 0 is the only pass.

#[path = "../../opl2/opl2.rs"]
mod opl2;
#[path = "../midi.rs"]
mod midi;

use midi::*;
// Only the two chip constants are pulled from the FM core here; a glob of both
// modules collides on run_all, which is a real signal that the two suites are
// separate things and not one.
use opl2::{CHIP_CLOCK_HZ, CHIP_CLOCK_DIV};

const RATE: u32 = 44100;

fn main() {
    let mut buf = vec![0u8; 4096];
    let mut sc = Box::new(Scratch::new());
    let mut syn = Box::new(FmSynth::new());
    let mut pl = Box::new(Player::new());

    println!("#183 MIDI player self-test  (host arm, rate {} Hz)", RATE);
    println!("  FM core: userland/lib/opl2, reached as the THIRD consumer.");
    println!("  chip rate {}/{} = {:.4} Hz, A440 = F-Number {} BLOCK {}",
             CHIP_CLOCK_HZ, CHIP_CLOCK_DIV,
             CHIP_CLOCK_HZ as f64 / CHIP_CLOCK_DIV as f64,
             NOTE_FNUM[69], NOTE_BLOCK[69]);
    println!("  test sequence: {:?} at {} PPQ, {} BPM then {} BPM from note {}",
             SEQ_NOTES, SEQ_PPQ,
             60_000_000 / SEQ_TEMPO0, 60_000_000 / SEQ_TEMPO1, SEQ_TEMPO_AT);
    println!();
    println!("  {:<22} {:>6}  {:>12} {:>12}  {}",
             "CHECK", "RESULT", "MEASURED", "EXPECTED", "WHAT IT MEANS");

    let mut emit = |r: &MidiReport| {
        println!("  {:<22} {:>6}  {:>12} {:>12}  {}",
                 r.name,
                 if r.pass { "PASS" } else { "FAIL" },
                 r.measured, r.expected, r.note);
    };

    let fails = midi_run_all(RATE, &mut buf, &mut pl, &mut syn, &mut sc, &mut emit);

    println!();
    if fails == 0 {
        println!("  ALL CHECKS PASSED. Every RED arm passed by correctly FAILING.");
    } else {
        println!("  {} FAILING CHECKS", fails);
    }
    std::process::exit(if fails > 125 { 125 } else { fails });
}
