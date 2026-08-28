// midi.rs - #183: THE module root of the MayteraOS MIDI player library. It is
// reached exactly the way userland/lib/opl2 documents, and it is the THIRD
// consumer of that one FM core:
//
//   #[path = "../../lib/opl2/opl2.rs"] mod opl2;
//   #[path = "../../lib/midi/midi.rs"] mod midi;
//
// in that order, in both consumers:
//
//   userland/apps/midiplay/main.rs        the GUI player
//   userland/lib/midi/hosttest/main.rs    the host verification harness
//
// THERE IS NO SYNTHESISER IN THIS DIRECTORY. Not a log-sine table, not an
// operator offset map, not a MULT table. Every sound this library makes comes
// out of `chip.write_reg()` and `chip.render_stereo()` on the one core in
// userland/lib/opl2, and `userland/lib/opl2/core-gate.sh` FAILS THE BUILD if a
// second implementation appears anywhere in the tree. The one thing this
// library needed that the core did not already expose is the operator offset
// for a channel; that was added to the core as `op_offset()` rather than copied
// here, which is what "improve the shared primitive" means in practice and is
// also the only way core-gate.sh stays green.
//
// NO_STD, NO ALLOC, NO FLOAT, NO I/O, NO WAITING, exactly like the FM core and
// for the same reason: the identical source runs in a Ring-3 ELF and in an
// ordinary host binary, so a green host run is evidence about the shipped code
// rather than about a lookalike.
//
// FLOAT, SPECIFICALLY: `x86_64-unknown-none` is a SOFT-FLOAT target
// ("rustc-abi": "softfloat"), so a float here is a libcall, not an
// instruction. Everything in this library is integer. The one place a
// transcendental is needed, equal temperament and the dB curves, is computed
// ONCE by gen_midi_tables.py on a host with a real FPU and checked in as a
// table that table-gate.sh proves still matches its formula.
//
//   midi_tables.rs    GENERATED. Note to F-Number/BLOCK, velocity to
//                     attenuation, pitch bend to frequency ratio.
//   smf.rs            Standard MIDI File parsing. VLQ and running status.
//   gm2opl.rs         General MIDI onto nine 2-operator voices, and the list
//                     of everything that mapping loses.
//   player.rs         The tempo map, the track merge, and sample-accurate
//                     scheduling. Contains no clock, deliberately.
//   midiselftest.rs   The scenarios, each with its RED twin.

#![allow(dead_code)]

use crate::opl2::*;

include!("midi_tables.rs");
include!("smf.rs");
include!("gm2opl.rs");
include!("player.rs");
include!("midiselftest.rs");
