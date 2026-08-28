// rustkern/intelgpu.rs - INTEL INTEGRATED GPU IDENTIFICATION (DETECTION ONLY).
// Pure logic: PCI device id -> generation + human name. Touches no hardware,
// maps no MMIO, programs no display register, and cannot affect scanout. New
// kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
//
// ===========================================================================
// WHY THIS EXISTS AND WHAT IT DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
// drivers/intel_gpu.c (980 lines) has had ZERO callers since it was written.
// It was audited on 2026-08-23 and is NOT a driver that merely needs wiring
// up; it is an unvalidated register crib whose PCH-relative register offsets
// are wrong for every generation it claims to support, and whose PCI search
// loop provably never terminates when the first display device is not Intel
// (see the audit block at the top of drivers/intel_gpu.c).
//
// Calling its intel_gpu_init() would hang boot in every QEMU VM, and on real
// hardware would try to disable VGA through the wrong register, busy-wait
// seconds on a GMBUS block that is not at the offset it uses on Gen5+, and arm
// a blitter ring whose base register is fed a raw CPU physical address where
// the hardware requires a GGTT graphics address.
//
// So this module does the ONE thing that is both useful and provably safe: it
// says WHICH Intel GPU is present. That is the question the owner's target
// hardware actually raises, and answering it costs nothing beyond reading a
// PCI config-space value that pci_init() has already fetched.
//
// ===========================================================================
// THE ANCHOR: 8086:0A26
// ---------------------------------------------------------------------------
// The owner's real target is an iMac14,4. A live PCI inventory captured from
// that machine (tools/mdev/imac-hw-findings.md) shows its SOLE display device
// is 8086:0a26 at 00:02.0, BAR0 = a0000004: Haswell-ULT GT3, marketed as
// Intel HD Graphics 5000. That id is the anchor of the self-test below.
//
// It is worth being precise about what that buys, because a QEMU VM cannot
// present an Intel iGPU at all and therefore cannot test any of this against
// silicon. What the self-test proves is that the TABLE is right about the
// owner's part: that 0x0A26 classifies as Gen 7.5 Haswell and resolves to a
// specific name rather than falling through to a guess. It proves nothing
// whatsoever about whether the hardware would respond to a register poke.
// Those are different claims and this file does not blur them.
//
// The pre-existing C table did NOT list 0x0A26. It listed 0x0A22 and relied on
// a high-byte family fallback to call the owner's GPU "Haswell (unknown)".
// That fallback is kept (it is the right behaviour for genuinely unlisted
// parts) but the parts that are actually corroborated are now listed exactly.
//
// ===========================================================================
// EVIDENCE KIND
// ---------------------------------------------------------------------------
// There is no C original to differ against, so there is no [RUST-DIFF] here.
// What stands in for it is intelgpu_selftest_rs(), a property test over the
// classifier's own invariants that runs on every boot and prints one line.
// Per the drvmap.rs precedent: a differential proves "same as the C", a
// property test proves "obeys its own rules". This is the second kind.

// Generation codes. These intentionally match the values of the C enum
// intel_gpu_gen_t in drivers/intel_gpu.h, so that if that driver is ever
// rewritten the two do not silently disagree about what "4" means.
pub const GEN_UNKNOWN: u32 = 0;
pub const GEN_5: u32 = 1; // Ironlake
pub const GEN_6: u32 = 2; // Sandy Bridge
pub const GEN_7: u32 = 3; // Ivy Bridge
pub const GEN_7_5: u32 = 4; // Haswell    <-- iMac14,4 target
pub const GEN_8: u32 = 5; // Broadwell
pub const GEN_9: u32 = 6; // Skylake
pub const GEN_9_5: u32 = 7; // Kaby Lake / Coffee Lake
pub const GEN_11: u32 = 8; // Ice Lake
pub const GEN_12: u32 = 9; // Tiger Lake

// Exact-match table. Names are NUL-terminated so they can be handed to C
// kprintf("%s") directly with no allocation and no copying.
//
// CORROBORATION NOTE: of every entry below, exactly ONE (0x0A26) is backed by
// a captured device list from hardware this project actually owns. The rest
// are transcribed from public Intel PCI id assignments and are UNVERIFIED
// here. That is fine for a log line and would NOT be fine as the basis for
// choosing a register layout.
static TABLE: &[(u16, u32, &[u8])] = &[
    // Gen 5 - Ironlake
    (0x0042, GEN_5, b"Ironlake Desktop\0"),
    (0x0046, GEN_5, b"Ironlake Mobile\0"),
    // Gen 6 - Sandy Bridge
    (0x0102, GEN_6, b"Sandy Bridge GT1\0"),
    (0x0106, GEN_6, b"Sandy Bridge Mobile GT1\0"),
    (0x0112, GEN_6, b"Sandy Bridge GT2\0"),
    (0x0116, GEN_6, b"Sandy Bridge Mobile GT2 (HD Graphics 3000)\0"),
    (0x0122, GEN_6, b"Sandy Bridge GT2+\0"),
    (0x0126, GEN_6, b"Sandy Bridge Mobile GT2+ (HD Graphics 3000)\0"),
    // Gen 7 - Ivy Bridge
    (0x0152, GEN_7, b"Ivy Bridge GT1 (HD Graphics 2500)\0"),
    (0x0156, GEN_7, b"Ivy Bridge Mobile GT1\0"),
    (0x0162, GEN_7, b"Ivy Bridge GT2 (HD Graphics 4000)\0"),
    (0x0166, GEN_7, b"Ivy Bridge Mobile GT2 (HD Graphics 4000)\0"),
    // Gen 7.5 - Haswell. The owner's iMac14,4 is in here.
    (0x0402, GEN_7_5, b"Haswell GT1\0"),
    (0x0406, GEN_7_5, b"Haswell Mobile GT1\0"),
    (0x040E, GEN_7_5, b"Haswell GT1\0"),
    (0x0412, GEN_7_5, b"Haswell GT2 (HD Graphics 4600)\0"),
    (0x0416, GEN_7_5, b"Haswell Mobile GT2 (HD Graphics 4600)\0"),
    (0x041E, GEN_7_5, b"Haswell GT2 (HD Graphics 4400)\0"),
    (0x0422, GEN_7_5, b"Haswell GT3\0"),
    (0x0426, GEN_7_5, b"Haswell Mobile GT3\0"),
    (0x0A02, GEN_7_5, b"Haswell ULT GT1\0"),
    (0x0A06, GEN_7_5, b"Haswell ULT GT1 (HD Graphics)\0"),
    (0x0A0E, GEN_7_5, b"Haswell ULX GT1\0"),
    (0x0A12, GEN_7_5, b"Haswell ULT GT2\0"),
    (0x0A16, GEN_7_5, b"Haswell ULT GT2 (HD Graphics 4400)\0"),
    (0x0A1E, GEN_7_5, b"Haswell ULX GT2 (HD Graphics 4200)\0"),
    (0x0A22, GEN_7_5, b"Haswell ULT GT3\0"),
    // ---- THE ANCHOR: measured on the owner's iMac14,4, 00:02.0 ----
    (0x0A26, GEN_7_5, b"Haswell ULT GT3 (HD Graphics 5000) [iMac14,4]\0"),
    (0x0A2E, GEN_7_5, b"Haswell ULT GT3 (Iris Graphics 5100)\0"),
    (0x0D22, GEN_7_5, b"Haswell GT3e (Iris Pro Graphics 5200)\0"),
    (0x0D26, GEN_7_5, b"Haswell GT3e (Iris Pro Graphics 5200)\0"),
    (0x0D2A, GEN_7_5, b"Haswell GT3e\0"),
    // Gen 8 - Broadwell
    (0x1602, GEN_8, b"Broadwell GT1\0"),
    (0x1606, GEN_8, b"Broadwell ULT GT1 (HD Graphics)\0"),
    (0x160E, GEN_8, b"Broadwell ULX GT1\0"),
    (0x1612, GEN_8, b"Broadwell GT2 (HD Graphics 5600)\0"),
    (0x1616, GEN_8, b"Broadwell ULT GT2 (HD Graphics 5500)\0"),
    (0x161E, GEN_8, b"Broadwell ULX GT2 (HD Graphics 5300)\0"),
    (0x1622, GEN_8, b"Broadwell GT3e (Iris Pro Graphics 6200)\0"),
    (0x1626, GEN_8, b"Broadwell ULT GT3 (HD Graphics 6000)\0"),
    // Gen 9 - Skylake
    (0x1902, GEN_9, b"Skylake GT1 (HD Graphics 510)\0"),
    (0x1906, GEN_9, b"Skylake ULT GT1 (HD Graphics 510)\0"),
    (0x190E, GEN_9, b"Skylake ULX GT1\0"),
    (0x1912, GEN_9, b"Skylake GT2 (HD Graphics 530)\0"),
    (0x1916, GEN_9, b"Skylake ULT GT2 (HD Graphics 520)\0"),
    (0x191E, GEN_9, b"Skylake ULX GT2 (HD Graphics 515)\0"),
    (0x1922, GEN_9, b"Skylake GT3\0"),
    (0x1926, GEN_9, b"Skylake ULT GT3 (Iris Graphics 540)\0"),
    (0x1932, GEN_9, b"Skylake GT4 (Iris Pro Graphics 580)\0"),
    // Gen 9.5 - Kaby Lake / Coffee Lake
    (0x5902, GEN_9_5, b"Kaby Lake GT1 (HD Graphics 610)\0"),
    (0x5906, GEN_9_5, b"Kaby Lake ULT GT1 (HD Graphics 610)\0"),
    (0x5912, GEN_9_5, b"Kaby Lake GT2 (HD Graphics 630)\0"),
    (0x5916, GEN_9_5, b"Kaby Lake ULT GT2 (HD Graphics 620)\0"),
    (0x591B, GEN_9_5, b"Kaby Lake GT2 Halo (HD Graphics 630)\0"),
    (0x5922, GEN_9_5, b"Kaby Lake GT3\0"),
    (0x5926, GEN_9_5, b"Kaby Lake ULT GT3 (Iris Plus Graphics 640)\0"),
    (0x3E91, GEN_9_5, b"Coffee Lake GT2 (UHD Graphics 630)\0"),
    (0x3E92, GEN_9_5, b"Coffee Lake GT2 (UHD Graphics 630)\0"),
    (0x3E98, GEN_9_5, b"Coffee Lake GT2 (UHD Graphics 630)\0"),
    (0x3E9B, GEN_9_5, b"Coffee Lake GT2 Halo (UHD Graphics 630)\0"),
    (0x3EA0, GEN_9_5, b"Whiskey Lake GT2 (UHD Graphics 620)\0"),
    // Gen 11 - Ice Lake
    (0x8A56, GEN_11, b"Ice Lake GT0.5\0"),
    (0x8A5A, GEN_11, b"Ice Lake GT1\0"),
    (0x8A5C, GEN_11, b"Ice Lake GT1.5\0"),
    (0x8A52, GEN_11, b"Ice Lake GT2 (Iris Plus Graphics)\0"),
    // Gen 12 - Tiger Lake
    (0x9A40, GEN_12, b"Tiger Lake GT1\0"),
    (0x9A49, GEN_12, b"Tiger Lake GT2 (Iris Xe Graphics)\0"),
    (0x9A60, GEN_12, b"Tiger Lake GT1\0"),
    (0x9A68, GEN_12, b"Tiger Lake GT1\0"),
    (0x9A70, GEN_12, b"Tiger Lake GT1\0"),
    (0x9A78, GEN_12, b"Tiger Lake GT2\0"),
];

// Family fallback by device-id high byte, for parts Intel shipped that are not
// in the exact table. Deliberately kept: an unlisted 0x0Axx part really is a
// Haswell, and saying so is more useful than "unknown". The NAME still says
// "unlisted" so a log line can never be mistaken for a confirmed match.
//
// 0x01xx is handled separately below because Sandy Bridge and Ivy Bridge SHARE
// that high byte (0x0102 vs 0x0152), so a high-byte-only rule would mislabel
// every unlisted Ivy Bridge as a Sandy Bridge.
static FAMILY: &[(u16, u32, &[u8])] = &[
    (0x0400, GEN_7_5, b"Haswell (unlisted device id)\0"),
    (0x0A00, GEN_7_5, b"Haswell ULT/ULX (unlisted device id)\0"),
    (0x0D00, GEN_7_5, b"Haswell GT3e (unlisted device id)\0"),
    (0x1600, GEN_8, b"Broadwell (unlisted device id)\0"),
    (0x1900, GEN_9, b"Skylake (unlisted device id)\0"),
    (0x5900, GEN_9_5, b"Kaby Lake (unlisted device id)\0"),
    (0x3E00, GEN_9_5, b"Coffee Lake (unlisted device id)\0"),
    (0x8A00, GEN_11, b"Ice Lake (unlisted device id)\0"),
    (0x9A00, GEN_12, b"Tiger Lake (unlisted device id)\0"),
];

static UNKNOWN_NAME: &[u8] = b"Intel graphics (unrecognised device id)\0";
static SNB_UNLISTED: &[u8] = b"Sandy Bridge (unlisted device id)\0";
static IVB_UNLISTED: &[u8] = b"Ivy Bridge (unlisted device id)\0";

/// Core classifier. Pure: no hardware access, no allocation, cannot panic.
fn classify(device_id: u16) -> (u32, &'static [u8]) {
    for &(id, gen, name) in TABLE {
        if id == device_id {
            return (gen, name);
        }
    }
    // Sandy Bridge and Ivy Bridge share the 0x01xx high byte; split on the
    // second nibble (Ivy Bridge ids are 0x015x/0x016x, Sandy Bridge 0x010x/
    // 0x011x/0x012x).
    if device_id & 0xFF00 == 0x0100 {
        if device_id & 0x00F0 >= 0x0050 {
            return (GEN_7, IVB_UNLISTED);
        }
        return (GEN_6, SNB_UNLISTED);
    }
    for &(fam, gen, name) in FAMILY {
        if device_id & 0xFF00 == fam {
            return (gen, name);
        }
    }
    (GEN_UNKNOWN, UNKNOWN_NAME)
}

/// C entry point. Writes the generation code through `out_gen` (ignored if
/// null) and returns a pointer to a static NUL-terminated name suitable for
/// kprintf("%s"). The returned pointer is always valid and never null.
///
/// # Safety
/// `out_gen` must be either null or a valid, writable `*mut u32`.
#[no_mangle]
pub unsafe extern "C" fn intelgpu_ident_rs(device_id: u16, out_gen: *mut u32) -> *const u8 {
    let (gen, name) = classify(device_id);
    if !out_gen.is_null() {
        // SAFETY: caller contract above; single aligned u32 write.
        unsafe { *out_gen = gen };
    }
    name.as_ptr()
}

/// Property self-test over the classifier's own invariants. Returns the number
/// of FAILURES (0 = pass) and writes the number of checks performed through
/// `out_checks` (ignored if null). Runs on every boot; costs microseconds.
///
/// # Safety
/// `out_checks` must be either null or a valid, writable `*mut u32`.
#[no_mangle]
pub unsafe extern "C" fn intelgpu_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks: u32 = 0;
    let mut fails: i32 = 0;

    // ---- 1. THE ANCHOR. The owner's iMac14,4 reports 8086:0a26. It must
    // classify as Haswell Gen 7.5 and must NOT land on the unknown name or on
    // an "unlisted" family fallback. If this ever fails, the one piece of real
    // hardware this project targets has stopped being recognised.
    checks += 1;
    let (gen, name) = classify(0x0A26);
    if gen != GEN_7_5 {
        fails += 1;
    }
    checks += 1;
    if name.as_ptr() == UNKNOWN_NAME.as_ptr() {
        fails += 1;
    }
    checks += 1;
    // The anchor name must be the EXACT entry, identifiable by carrying the
    // marketing number "5000". Scanned without allocation.
    let mut has_5000 = false;
    let mut i = 0usize;
    while i + 4 <= name.len() {
        if name[i] == b'5' && name[i + 1] == b'0' && name[i + 2] == b'0' && name[i + 3] == b'0' {
            has_5000 = true;
            break;
        }
        i += 1;
    }
    if !has_5000 {
        fails += 1;
    }

    // ---- 2. Every exact-table entry classifies to its own generation, and
    // none of them is UNKNOWN. A typo that shadowed an entry would show here.
    for &(id, gen, _) in TABLE {
        checks += 1;
        let (got_gen, _) = classify(id);
        if got_gen != gen || got_gen == GEN_UNKNOWN {
            fails += 1;
        }
    }

    // ---- 3. No duplicate device ids in the exact table. A duplicate would be
    // silently shadowed by whichever came first, so it can never be caught by
    // check 2 alone.
    let mut a = 0usize;
    while a < TABLE.len() {
        let mut b = a + 1;
        while b < TABLE.len() {
            checks += 1;
            if TABLE[a].0 == TABLE[b].0 {
                fails += 1;
            }
            b += 1;
        }
        a += 1;
    }

    // ---- 4. Every name is NUL-terminated. C prints these with %s; a missing
    // terminator would read off the end of the object into whatever follows.
    for &(_, _, name) in TABLE.iter().chain(FAMILY.iter()) {
        checks += 1;
        if name.is_empty() || name[name.len() - 1] != 0 {
            fails += 1;
        }
    }
    for name in [UNKNOWN_NAME, SNB_UNLISTED, IVB_UNLISTED] {
        checks += 1;
        if name.is_empty() || name[name.len() - 1] != 0 {
            fails += 1;
        }
    }

    // ---- 5. Family fallback still works for a genuinely unlisted part.
    checks += 1;
    if classify(0x0A99).0 != GEN_7_5 {
        fails += 1;
    }
    checks += 1;
    if classify(0x1699).0 != GEN_8 {
        fails += 1;
    }
    checks += 1;
    if classify(0x0159).0 != GEN_7 {
        fails += 1;
    }
    checks += 1;
    if classify(0x0119).0 != GEN_6 {
        fails += 1;
    }

    // ---- 6. Non-graphics / bogus ids must NOT be claimed. 0x9C20 is the
    // iMac's Lynx Point HD Audio controller and 0x9C31 its xHCI; if the
    // classifier ever claimed one of those, the log would name the wrong
    // device as the GPU.
    for bogus in [0x0000u16, 0xFFFF, 0x9C20, 0x9C31, 0x1568] {
        checks += 1;
        if classify(bogus).0 != GEN_UNKNOWN {
            fails += 1;
        }
    }

    if !out_checks.is_null() {
        // SAFETY: caller contract above; single aligned u32 write.
        unsafe { *out_checks = checks };
    }
    fails
}
