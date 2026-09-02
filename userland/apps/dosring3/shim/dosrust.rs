// dosrust.rs - crate root for the Ring-3 DOS host's Rust half (#DOSRING3).
//
// The kernel's DOS subsystem is roughly half Rust already (20k lines across
// dos*.rs, dpmi*.rs, go32.rs, le.rs, x86_32.rs, vbe.rs, cga.rs and friends).
// Those modules build for x86_64-unknown-none, which is the SAME target the
// kernel uses, so they do not need porting at all: they need a different crate
// root that selects the DOS subset instead of the whole kernel.
//
// This is that root. Every `#[path]` below points at a VERBATIM copy of the
// kernel module (see mkgen.sh), so there is exactly one implementation of DOS
// semantics in the tree and the Ring-0 and Ring-3 paths cannot drift.
//
// MEASURED: across the entire Rust DOS set, only FOUR extern "C" functions are
// declared (kprintf plus three dpmi_rmcs logging helpers). That is why this
// relinks rather than ports: the modules are self-contained state machines
// over plain memory, with no Ring-0 coupling to sever.
#![no_std]
#![allow(dead_code)]

use core::panic::PanicInfo;

extern "C" {
    // Ring-3 diagnostic sink, implemented in shim/kshim.c. Same name and same
    // signature as the kernel's, so the modules below are unmodified.
    fn dosring3_rust_panic(msg: *const u8) -> !;
}

// panic=abort, and a Rust panic must be LOUD rather than a silent freeze -
// the same rule the kernel crate root applies.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe { dosring3_rust_panic(b"rust panic (dosring3)\0".as_ptr()); }
}

#[path = "../gen/rustkern/common.rs"]   mod common;
#[path = "../gen/rustkern/cfgread.rs"]  mod cfgread;
#[path = "../gen/rustkern/blkhist.rs"]  mod blkhist;
#[path = "../gen/rustkern/cga.rs"]      mod cga;
#[path = "../gen/rustkern/doscrtc.rs"]  mod doscrtc;
#[path = "../gen/rustkern/dosmem.rs"]   mod dosmem;
#[path = "../gen/rustkern/doslinger.rs"] mod doslinger;
#[path = "../gen/rustkern/dosmcb.rs"]   mod dosmcb;
#[path = "../gen/rustkern/dospit.rs"]   mod dospit;
#[path = "../gen/rustkern/opl2.rs"]     mod opl2;
#[path = "../gen/rustkern/dossb.rs"]    mod dossb;
// fmq.rs is DELIBERATELY NOT COMPILED HERE (#fmbridge). It is the FM event
// QUEUE, and this process must not have one: its guest's OPL2 register
// writes go to the KERNEL's queue through SYS_DOS_FM_HOST, which is the
// same queue /APPS/FMSYNTH drains. A local copy compiled in here is
// precisely the bug that was fixed - a perfectly correct queue that nothing
// could ever drain - so it is removed rather than left unreferenced. See
// kernel/dos/dosfmq.h and the tail of mkgen.sh.
#[path = "../gen/rustkern/dosbus.rs"]   mod dosbus;
#[path = "../gen/rustkern/dosint15.rs"] mod dosint15;
#[path = "../gen/rustkern/doswin.rs"]   mod doswin;
#[path = "../gen/rustkern/dosprof.rs"]  mod dosprof;
#[path = "../gen/rustkern/dosdisp.rs"]  mod dosdisp;
#[path = "../gen/rustkern/dosmick.rs"]  mod dosmick;
#[path = "../gen/rustkern/dos4gw.rs"]   mod dos4gw;
#[path = "../gen/rustkern/dpmi.rs"]     mod dpmi;
#[path = "../gen/rustkern/dpmi_rmcs.rs"] mod dpmi_rmcs;
#[path = "../gen/rustkern/drvmap.rs"]   mod drvmap;
#[path = "../gen/rustkern/dosovl.rs"]   mod dosovl;
#[path = "../gen/rustkern/dospath.rs"]  mod dospath;
#[path = "../gen/rustkern/le.rs"]       mod le;
#[path = "../gen/rustkern/go32.rs"]     mod go32;
#[path = "../gen/rustkern/vbe.rs"]      mod vbe;
#[path = "../gen/rustkern/x86_32.rs"]   mod x86_32;
#[path = "../gen/rustkern/modex.rs"]    mod modex;
// mono.rs is DELIBERATELY NOT COMPILED HERE (#flipfix). It calibrates the TSC
// against PIT channel 0 with Ring-0 port I/O, which this process cannot do:
// mono_init() was never called, so mono_ready_rs() answered 0 for the life of
// the host and every instrument gated on it stayed silent, [DOSFRAME] included.
// shim/kshim.c supplies mono_ready_rs/mono_us_rs/mono_ms_rs from SYS_MONO_US
// instead - the kernel's own TSC clock, so there is one calibration rather than
// two that can disagree. Dropping the module rather than adding a Ring-3 entry
// point to it also keeps mono_busy_delay_us_rs() out of the link, whose PIT
// fallback would #GP here; an unresolved symbol is the better failure.
#[path = "../gen/rustkern/ktime.rs"]    mod ktime;
#[path = "../gen/rustkern/guestfs.rs"]  mod guestfs;
#[path = "../gen/rustkern/imgra.rs"]    mod imgra;
#[path = "../gen/rustkern/iso9660.rs"]  mod iso9660;
#[path = "../gen/rustkern/isomemo.rs"]  mod isomemo;
#[path = "../gen/rustkern/x87.rs"]      mod x87;
#[path = "../gen/rustkern/spawnid.rs"] mod spawnid;
#[path = "../gen/rustkern/usbvol.rs"]  mod usbvol;
