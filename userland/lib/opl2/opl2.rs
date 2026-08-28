// opl2.rs - #182: THE module root of the one and only MayteraOS FM synthesis
// core. Both consumers pull in THIS file and nothing else:
//
//   userland/apps/fmsynth/main.rs   the DOS OPL2 bridge
//   userland/apps/fmtest/main.rs    the verification harness
//   (#183's MIDI player, when it lands, does the same)
//
// as
//
//   #[path = "../../lib/opl2/opl2.rs"]
//   mod opl2;
//
// It is a single flat namespace assembled from four fragments in a fixed order,
// because the later fragments use items from the earlier ones and Rust's
// include! keeps everything in one module scope. Splitting it into four
// sub-modules would buy nothing but `use` lines.
//
//   opl2_tables.rs   GENERATED. The log-sine and exponent ROMs. See
//                    gen_tables.py for the formulas and table-gate.sh for the
//                    check that they still match those formulas.
//   opl2core.rs      The chip: registers, operators, envelopes, rendering.
//   opl2check.rs     Integer signal analysis. The objective test.
//   opl2selftest.rs  The scenarios, each with its RED twin.
//
// NO_STD, NO ALLOC, NO FLOAT, NO I/O, NO WAITING. This module computes and
// nothing else, which is why the identical source runs unmodified in a Ring-3
// ELF and in an ordinary host binary, and why a green host run is evidence
// about the shipped code.

#![allow(dead_code)]

include!("opl2_tables.rs");
include!("opl2core.rs");
include!("opl2check.rs");
include!("opl2selftest.rs");
