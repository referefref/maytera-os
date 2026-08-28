// amlhid.rs - answer "what does the firmware SAY the internal input devices are?"
// without writing an AML interpreter.
//
// #ASUSKBD. The ASUS i7-4720HQ laptop boots MayteraOS with working USB keyboard
// and mouse and a dead INTERNAL keyboard. Its FADT sets IAPC_BOOT_ARCH bit 1
// ("8042 not present") while port 0x64 answers 0x15, so firmware and hardware
// disagree and the question "is the internal keyboard PS/2 at all?" has to be
// settled from something other than the disagreement itself.
//
// The DSDT settles it. Every x86 firmware that exposes a legacy PS/2 keyboard
// declares a device with _HID = EISAID("PNP0303") in its DSDT, and a PS/2
// pointing device as PNP0F13 (or one of the vendor variants). A machine whose
// internal keyboard is HID-over-I2C instead declares PNP0C50, and one whose
// touchpad is an I2C Elan/Synaptics part declares a vendor _HID string.
//
// WE DO NOT HAVE AN AML INTERPRETER AND THIS DOES NOT NEED ONE.
//
// Writing one is weeks of work and a large new attack surface parsing hostile
// firmware bytecode in Ring 0. It would buy us the ability to EVALUATE _STA and
// _CRS. We do not need to evaluate anything: we need to know which device
// declarations EXIST. An _HID with an EISA id is stored in AML as a
// DWordPrefix (0x0C) followed by four literal bytes, in a fixed byte order that
// does not depend on any surrounding scope, control flow or operand resolution.
// So the presence question is answerable by looking for those four bytes.
//
// That is a strictly weaker claim than an interpreter would give, and the
// difference matters, so the C side reports it as such:
//   - Found  => the firmware DECLARES such a device. It does not prove _STA
//               returns present, and on a laptop a PS2K device is sometimes
//               declared and then disabled by _STA.
//   - Absent => strong evidence the route does not exist. A firmware that
//               provides a PS/2 keyboard essentially always declares PNP0303,
//               because that is how every OS finds it.
//
// EISAID BYTE ORDER, derived not guessed. EISAID packs the three-letter
// manufacturer code into 16 bits, 5 bits per letter with 'A'=1:
//     "PNP" -> (16 << 10) | (14 << 5) | 16 = 0x41D0
// and the four-hex-digit product code into two more bytes. In memory the four
// bytes appear as [mfg_hi, mfg_lo, prod_hi, prod_lo], i.e. PNP0303 is
// 41 D0 03 03. Cross-checked against the one EISAID whose numeric value is
// widely published: EISAID("PNP0A03") (PCI root bus) = 0x030AD041, whose
// little-endian bytes are 41 D0 0A 03. Same layout. That table entry is also
// carried below as a POSITIVE CONTROL: every x86 DSDT in existence declares a
// PCI root bus, so a scan that finds ZERO PNP0A03 has found a broken scanner,
// not a strange machine. A check that has never been seen to succeed on a
// known-good input proves nothing (#514, #665).
//
// Rust, not C, per the standing new-kernel-code directive, and it earns it:
// this is unvalidated firmware bytecode of a length the firmware itself chose,
// scanned with a sliding window. That is precisely the shape of the ext2_lookup
// OOB (#476) and the usb_desc over-read. `windows()` cannot run off the end.

#[repr(C)]
pub struct AmlHidReport {
    pub tables_scanned: u32,
    pub bytes_scanned: u32,
    // Legacy PS/2 keyboard _HIDs.
    pub pnp0303: u32, // PNP0303 IBM enhanced keyboard, the canonical one
    pub pnp030b: u32, // PNP030B generic keyboard
    pub pnp0320: u32, // PNP0320 Japanese 106
    // Legacy PS/2 pointing-device _HIDs.
    pub pnp0f13: u32, // PNP0F13 PS/2 port mouse, the canonical one
    pub pnp0f03: u32, // PNP0F03 Microsoft PS/2 mouse
    pub pnp0f12: u32, // PNP0F12 Logitech PS/2 mouse
    pub pnp0f0e: u32, // PNP0F0E Microsoft bus mouse variant
    // The alternative route, if PS/2 turns out to be a dead end.
    pub pnp0c50: u32, // PNP0C50 HID over I2C
    pub acpi0c50: u32, // "ACPI0C50", the newer spelling, as a literal string
    // POSITIVE CONTROL. Nonzero on every x86 machine; zero means WE are broken.
    pub pnp0a03: u32, // PNP0A03 PCI root bus
    // ACPI device NAMES. Four literal ASCII bytes in a NameOp, so they are as
    // findable as an EISAID and they say what the firmware author called it.
    pub name_ps2k: u32,
    pub name_ps2m: u32,
    pub name_kbd_: u32,
    pub name_tpad: u32,
    pub name_etpd: u32,
    // I2C-HID touchpad/keyboard vendor _HID strings, stored as plain ASCII.
    pub vendor_elan: u32,
    pub vendor_syna: u32,
    pub vendor_asue: u32,
    pub vendor_ftec: u32,
}

fn count(hay: &[u8], needle: &[u8]) -> u32 {
    if needle.is_empty() || hay.len() < needle.len() {
        return 0;
    }
    let mut n: u32 = 0;
    for w in hay.windows(needle.len()) {
        if w == needle {
            n = n.saturating_add(1);
        }
    }
    n
}

/// Zero a report before the first scan.
///
/// # Safety
/// `out` must point to a valid, writable `AmlHidReport`.
#[no_mangle]
pub unsafe extern "C" fn amlhid_reset_rs(out: *mut AmlHidReport) {
    if out.is_null() {
        return;
    }
    core::ptr::write_bytes(out as *mut u8, 0, core::mem::size_of::<AmlHidReport>());
}

/// Scan one ACPI table body and ACCUMULATE into `out`, so the caller can feed
/// the DSDT and then every SSDT into the same report. Returns bytes scanned.
///
/// # Safety
/// `base`..`base+len` must be readable. The caller establishes that by having
/// already checksummed the table over exactly that range, which reads every
/// byte; a table that survived `validate_table()` is readable by construction.
#[no_mangle]
pub unsafe extern "C" fn amlhid_scan_rs(base: *const u8, len: u32, out: *mut AmlHidReport) -> u32 {
    if base.is_null() || out.is_null() || len < 36 {
        return 0;
    }
    // Sanity band. A real DSDT is a few KB to a few hundred KB. Anything above
    // 16 MB is a firmware value we do not believe, and believing it would turn
    // a scan into a very long walk through unmapped memory.
    if len > 16 * 1024 * 1024 {
        return 0;
    }
    let body = core::slice::from_raw_parts(base, len as usize);
    let r = &mut *out;

    r.tables_scanned = r.tables_scanned.saturating_add(1);
    r.bytes_scanned = r.bytes_scanned.saturating_add(len);

    r.pnp0303 = r.pnp0303.saturating_add(count(body, &[0x41, 0xD0, 0x03, 0x03]));
    r.pnp030b = r.pnp030b.saturating_add(count(body, &[0x41, 0xD0, 0x03, 0x0B]));
    r.pnp0320 = r.pnp0320.saturating_add(count(body, &[0x41, 0xD0, 0x03, 0x20]));

    r.pnp0f13 = r.pnp0f13.saturating_add(count(body, &[0x41, 0xD0, 0x0F, 0x13]));
    r.pnp0f03 = r.pnp0f03.saturating_add(count(body, &[0x41, 0xD0, 0x0F, 0x03]));
    r.pnp0f12 = r.pnp0f12.saturating_add(count(body, &[0x41, 0xD0, 0x0F, 0x12]));
    r.pnp0f0e = r.pnp0f0e.saturating_add(count(body, &[0x41, 0xD0, 0x0F, 0x0E]));

    r.pnp0c50 = r.pnp0c50.saturating_add(count(body, &[0x41, 0xD0, 0x0C, 0x50]));
    r.acpi0c50 = r.acpi0c50.saturating_add(count(body, b"ACPI0C50"));

    r.pnp0a03 = r.pnp0a03.saturating_add(count(body, &[0x41, 0xD0, 0x0A, 0x03]));

    r.name_ps2k = r.name_ps2k.saturating_add(count(body, b"PS2K"));
    r.name_ps2m = r.name_ps2m.saturating_add(count(body, b"PS2M"));
    r.name_kbd_ = r.name_kbd_.saturating_add(count(body, b"KBD_"));
    r.name_tpad = r.name_tpad.saturating_add(count(body, b"TPAD"));
    r.name_etpd = r.name_etpd.saturating_add(count(body, b"ETPD"));

    r.vendor_elan = r.vendor_elan.saturating_add(count(body, b"ELAN"));
    r.vendor_syna = r.vendor_syna.saturating_add(count(body, b"SYNA"));
    r.vendor_asue = r.vendor_asue.saturating_add(count(body, b"ASUE"));
    r.vendor_ftec = r.vendor_ftec.saturating_add(count(body, b"FTE"));

    len
}

/// GENERIC EISAID PRESENCE COUNT over one already-validated ACPI table body.
///
/// WHY THIS IS HERE AND NOT IN A SECOND MODULE. The UI-scale auto-detector
/// needs to ask the firmware a different question ("does this machine have a
/// battery, i.e. is it a laptop?") using the SAME technique this module already
/// implements: a byte-pattern scan for an EISAID in AML. Writing a second
/// scanner would be a private copy of `count()` plus a second set of bounds
/// assumptions over hostile firmware bytecode, which is exactly the shape of
/// the ext2_lookup OOB (#476). CLAUDE.md's rule is to IMPROVE the shared
/// primitive, so the scanner is exposed rather than duplicated.
///
/// `a`,`b`,`c`,`d` are the four EISAID bytes in memory order. See this file's
/// header for the derivation of that order; PNP0C0A (control-method battery)
/// is 41 D0 0C 0A.
///
/// The claim this supports is exactly as weak as the one the report above
/// supports: DECLARED, not `_STA`-present. A battery bay with no battery in it
/// still declares PNP0C0A, and that is fine for this purpose - the question
/// being answered is "is this chassis a laptop", not "is a battery installed".
///
/// # Safety
/// `base`..`base+len` must be readable, established the same way
/// `amlhid_scan_rs` establishes it.
#[no_mangle]
pub unsafe extern "C" fn amlhid_count_eisaid_rs(
    base: *const u8, len: u32, a: u8, b: u8, c: u8, d: u8,
) -> u32 {
    if base.is_null() || len < 36 || len > 16 * 1024 * 1024 {
        return 0;
    }
    let body = core::slice::from_raw_parts(base, len as usize);
    count(body, &[a, b, c, d])
}

/// Self-test with a synthetic table, run at boot. RED then GREEN: it asserts
/// that a table containing PNP0303 and PNP0A03 is FOUND, and that a table
/// containing neither reports ZERO for both. A scanner only ever seen to say
/// "found" is not evidence (#514).
/// Returns 0 on pass, or a bitmask of failed checks.
#[no_mangle]
pub extern "C" fn amlhid_selftest_rs() -> u32 {
    let mut fail: u32 = 0;
    // The generic EISAID counter, RED then GREEN on the same principle as the
    // report scan below: it must FIND a planted PNP0C0A (control-method
    // battery, the UI-scale laptop probe's input) and must find ZERO of an
    // EISAID that is not there. Bit 31 so it cannot collide with the existing
    // check bits, whatever they grow to.
    {
        let mut blob = [0u8; 64];
        blob[40] = 0x41; blob[41] = 0xD0; blob[42] = 0x0C; blob[43] = 0x0A;
        unsafe {
            let found = amlhid_count_eisaid_rs(blob.as_ptr(), 64, 0x41, 0xD0, 0x0C, 0x0A);
            let absent = amlhid_count_eisaid_rs(blob.as_ptr(), 64, 0x41, 0xD0, 0x0C, 0x0D);
            if found != 1 || absent != 0 {
                fail |= 1 << 31;
            }
        }
    }
    let mut rep = AmlHidReport {
        tables_scanned: 0, bytes_scanned: 0,
        pnp0303: 0, pnp030b: 0, pnp0320: 0,
        pnp0f13: 0, pnp0f03: 0, pnp0f12: 0, pnp0f0e: 0,
        pnp0c50: 0, acpi0c50: 0, pnp0a03: 0,
        name_ps2k: 0, name_ps2m: 0, name_kbd_: 0, name_tpad: 0, name_etpd: 0,
        vendor_elan: 0, vendor_syna: 0, vendor_asue: 0, vendor_ftec: 0,
    };

    // GREEN case: a 40-byte blob carrying a PNP0303, a PNP0F13, a PNP0A03 and
    // the name PS2K. All four must be found exactly once.
    let mut green = [0u8; 40];
    green[8..12].copy_from_slice(&[0x41, 0xD0, 0x03, 0x03]);
    green[16..20].copy_from_slice(&[0x41, 0xD0, 0x0F, 0x13]);
    green[24..28].copy_from_slice(&[0x41, 0xD0, 0x0A, 0x03]);
    green[32..36].copy_from_slice(b"PS2K");
    unsafe { amlhid_scan_rs(green.as_ptr(), green.len() as u32, &mut rep); }
    if rep.pnp0303 != 1 { fail |= 1 << 0; }
    if rep.pnp0f13 != 1 { fail |= 1 << 1; }
    if rep.pnp0a03 != 1 { fail |= 1 << 2; }
    if rep.name_ps2k != 1 { fail |= 1 << 3; }
    if rep.tables_scanned != 1 { fail |= 1 << 4; }

    // RED case: a blob of 0xAA carrying NONE of them. Every counter must stay
    // where the green pass left it, i.e. the scanner must be capable of NOT
    // finding things.
    let red = [0xAAu8; 40];
    unsafe { amlhid_scan_rs(red.as_ptr(), red.len() as u32, &mut rep); }
    if rep.pnp0303 != 1 { fail |= 1 << 5; }
    if rep.pnp0f13 != 1 { fail |= 1 << 6; }
    if rep.pnp0a03 != 1 { fail |= 1 << 7; }
    if rep.name_ps2k != 1 { fail |= 1 << 8; }
    if rep.tables_scanned != 2 { fail |= 1 << 9; }

    // A too-short table and a null pointer must both be refused, not scanned.
    let tiny = [0u8; 8];
    if unsafe { amlhid_scan_rs(tiny.as_ptr(), tiny.len() as u32, &mut rep) } != 0 { fail |= 1 << 10; }
    if unsafe { amlhid_scan_rs(core::ptr::null(), 4096, &mut rep) } != 0 { fail |= 1 << 11; }
    if rep.tables_scanned != 2 { fail |= 1 << 12; }

    fail
}
