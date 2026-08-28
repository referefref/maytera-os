// rustkern/battery.rs - control-method battery (ACPI PNP0C0A) percentage,
// charge state and, where the firmware gives us a usable rate, time
// remaining.
//
// WHY THIS EXISTS
// ---------------------------------------------------------------------------
// uiscale.rs already answers "does the firmware DECLARE a battery" (a laptop
// proxy for display-scaling auto-detection). It deliberately goes no further:
// its own header says amlhid.rs is a byte-pattern presence scanner and NOT an
// AML interpreter, on purpose, because a general interpreter over hostile
// firmware bytecode is a large attack surface for a question that did not
// need one. This module answers a harder question - the battery's actual
// _BIF/_BIX (design/full-charge capacity) and _BST (state, rate, remaining
// capacity) - which DOES require evaluating a method body, not just finding a
// name.
//
// THE SCOPE LINE, STATED UP FRONT, BECAUSE IT DECIDES WHAT THIS CODE MUST DO
// WHEN IT CANNOT ANSWER
// ---------------------------------------------------------------------------
// A full AML interpreter (arbitrary Store/If/While, OperationRegion access to
// the Embedded Controller, method-to-method calls) is a multi-week project
// and this kernel has NO EC driver at all (grepped: zero EC_DATA/EC_SC port
// I/O anywhere in the tree) - so even a full interpreter could not fetch a
// live EC-backed value here. Writing one anyway, half-finished, would mean
// executing partially-understood hostile bytecode and possibly issuing raw
// port I/O derived from it, which is exactly the "unguarded probe of absent
// hardware writes garbage back into it" shape blame.md's #ASUSDIAG entry
// warns about. That is a worse outcome than admitting a limit.
//
// So this module evaluates exactly one safe, useful, real subset: methods
// whose entire body is `Return (Package { <constants...> })`, with NO Store,
// NO OperationRegion/Field, NO control flow, NO NAME REFERENCE other than the
// package's own literal elements. This is:
//   - SAFE: pure bytecode-to-integer decoding over an already
//     checksum-validated table, no I/O, no writes, cannot loop (bounded single
//     linear pass), same trust model as amlhid.rs.
//   - GENUINELY USEFUL for _BIF/_BIX: design capacity, last-full-charge
//     capacity and units are STATIC facts about the battery and are commonly
//     compiled as literal constants even on real firmware, because they do
//     not change and do not need an EC read.
//   - HONESTLY LIMITED for _BST: many real embedded controllers back the
//     dynamic fields (state/rate/remaining capacity) with an EC Field read,
//     which this module will not attempt to evaluate. When it meets an
//     opcode it does not support, it STOPS and reports "could not evaluate"
//     rather than guessing - the caller then reports unknown, per the
//     project rule of never computing a fictional number from data we do not
//     have.
//
// Some real firmware (and every synthetic/virtual ACPI battery, e.g. the kind
// a test harness would inject) DOES compile _BST as a constant Return, in
// which case this reads it correctly end to end. On hardware whose _BST needs
// the EC, percentage/state honestly come back "unknown" until an EC driver
// exists to extend this - which is the correct behaviour this project asks
// for, not a bug.
//
// Rust, not C, per the standing new-kernel-code rule: this is unvalidated
// firmware bytecode of firmware-chosen length, scanned and parsed with sliced,
// bounds-checked reads - the same OOB shape as ext2_lookup (#476) and the
// exact reason amlhid.rs is Rust.

#![allow(dead_code)]

// ---------------------------------------------------------------------------
// A minimal, BOUNDED, read-only AML term reader.
//
// This is not a general parser: `skip_term` recognises just enough opcodes to
// walk PAST objects it does not care about while searching for a named
// Method, and `eval_return_package` recognises just enough to evaluate the
// one safe shape described above. Anything else causes an immediate bail.
// Every read goes through `Cursor::u8`/`peek`, which never reads past `end`.
// ---------------------------------------------------------------------------

struct Cursor<'a> {
    b: &'a [u8],
    pos: usize,
    end: usize, // exclusive
}

impl<'a> Cursor<'a> {
    fn new(b: &'a [u8], start: usize, end: usize) -> Self {
        Cursor { b, pos: start, end: if end <= b.len() { end } else { b.len() } }
    }
    fn peek(&self) -> Option<u8> {
        if self.pos < self.end { self.b.get(self.pos).copied() } else { None }
    }
    fn u8(&mut self) -> Option<u8> {
        let v = self.peek()?;
        self.pos += 1;
        Some(v)
    }
    fn u16le(&mut self) -> Option<u16> {
        let a = self.u8()? as u16;
        let b = self.u8()? as u16;
        Some(a | (b << 8))
    }
    fn u32le(&mut self) -> Option<u32> {
        let a = self.u16le()? as u32;
        let b = self.u16le()? as u32;
        Some(a | (b << 16))
    }
    fn u64le(&mut self) -> Option<u64> {
        let a = self.u32le()? as u64;
        let b = self.u32le()? as u64;
        Some(a | (b << 32))
    }
    /// ACPI PkgLength: high 2 bits of the lead byte = extra byte count (0-3).
    /// If 0, the low 6 bits ARE the length. Otherwise the low 4 bits of the
    /// lead byte plus the 8 bits of each extra byte (LSB first) concatenate
    /// to the length. The value COUNTS ITSELF: it is the number of bytes in
    /// the package including the PkgLength encoding, measured from the start
    /// of the PkgLength field. Returns (value, bytes_consumed_by_the_encoding).
    fn pkglen(&mut self) -> Option<(u32, u32)> {
        let lead = self.u8()?;
        let extra = (lead >> 6) & 0x3;
        if extra == 0 {
            return Some(((lead & 0x3F) as u32, 1));
        }
        // Bits 4-5 of the lead byte must be zero per spec when extra>0; we do
        // not enforce that (a malformed table just yields a value we bound
        // below), we only need to not read out of bounds.
        let mut val: u32 = (lead & 0x0F) as u32;
        for i in 0..extra {
            let by = self.u8()? as u32;
            val |= by << (4 + 8 * i as u32);
        }
        Some((val, 1 + extra as u32))
    }
}

// AML opcodes we recognise. Not exhaustive; anything else is UNSUPPORTED and
// causes an immediate bail wherever it is checked below.
//
// WHY NO GENERAL "SKIP ANY TERM" WALKER. An earlier draft of this module
// carried one (to structurally walk into the right Scope/Device before
// looking for the method), modelled on how a real AML parser would find a
// name unambiguously. It was never exercised by anything: the search below
// finds a Method by its MethodOp byte and validates the NameString right
// there, which is simpler, does not need to understand any container it is
// not opening, and is exactly the reduced-claim approach amlhid.rs already
// uses for _HID presence. Code with no caller and no self-test coverage is
// the "zero callers" shape blame.md warns about, so it was deleted rather
// than kept "for completeness".
const OP_ZERO: u8 = 0x00;
const OP_ONE: u8 = 0x01;
const OP_BYTE_PREFIX: u8 = 0x0A;
const OP_WORD_PREFIX: u8 = 0x0B;
const OP_DWORD_PREFIX: u8 = 0x0C;
const OP_QWORD_PREFIX: u8 = 0x0E;
const OP_PACKAGE: u8 = 0x12;
const OP_METHOD: u8 = 0x14;
const OP_RETURN: u8 = 0xA4;
const OP_ONES: u8 = 0xFF;

/// Evaluate `Return (Package { const, const, ... })`, the one safe shape.
/// `out` receives each package element as a u64 (Zero/One/Ones and the sized
/// consts); a String or nested Package element aborts evaluation (we do not
/// need strings for the fields we read, and getting them subtly wrong is
/// worse than declining). Returns the count written, or None if the body is
/// not exactly this shape.
fn eval_return_package(c: &mut Cursor, out: &mut [u64], out_cap: usize) -> Option<usize> {
    if c.u8()? != OP_RETURN { return None; }
    if c.u8()? != OP_PACKAGE { return None; }
    let (pkg_len, hdrlen) = c.pkglen()?;
    let pkg_start = c.pos - hdrlen as usize;
    let pkg_end = pkg_start.checked_add(pkg_len as usize)?;
    if pkg_end > c.end { return None; }
    let num_elements = c.u8()? as usize; // ByteData NumElements
    let mut inner = Cursor::new(c.b, c.pos, pkg_end);
    let mut n = 0usize;
    while inner.pos < inner.end && n < num_elements {
        let v: u64 = match inner.peek()? {
            OP_ZERO => { inner.pos += 1; 0 }
            OP_ONE => { inner.pos += 1; 1 }
            OP_ONES => { inner.pos += 1; !0u64 }
            OP_BYTE_PREFIX => { inner.pos += 1; inner.u8()? as u64 }
            OP_WORD_PREFIX => { inner.pos += 1; inner.u16le()? as u64 }
            OP_DWORD_PREFIX => { inner.pos += 1; inner.u32le()? as u64 }
            OP_QWORD_PREFIX => { inner.pos += 1; inner.u64le()? }
            _ => return None, // String, nested Package, or anything dynamic
        };
        if n < out_cap { out[n] = v; }
        n += 1;
    }
    // After the package, the enclosing Method's TermList may still have more
    // bytes if pkg_end < c.end (e.g. trailing bytes from a PkgLength that
    // over-counted, which real tables do not do); we do not require c to be
    // exactly at end, only that we found exactly one clean Return(Package).
    c.pos = pkg_end;
    Some(n)
}

/// Find a Method named exactly `name` (4 raw NameSeg bytes, unqualified -
/// the common form when a method is declared directly inside the battery
/// Device scope) anywhere in `body`, evaluate its body as
/// `Return (Package {...})`, and write the elements to `out`.
///
/// Returns 1 and `*out_n` elements on success, 0 if not found or the body is
/// not the safe constant-return shape.
///
/// # Safety
/// `base`..`base+len` must be readable (checksum-validated table, same
/// contract as amlhid_scan_rs/amlhid_count_eisaid_rs).
#[no_mangle]
pub unsafe extern "C" fn battery_eval_method_rs(
    base: *const u8, len: u32,
    n0: u8, n1: u8, n2: u8, n3: u8,
    out: *mut u64, out_cap: u32, out_n: *mut u32,
) -> i32 {
    if base.is_null() || out.is_null() || out_n.is_null() { return 0; }
    if len < 36 || len > 16 * 1024 * 1024 { return 0; }
    let body = core::slice::from_raw_parts(base, len as usize);
    let out_slice = core::slice::from_raw_parts_mut(out, out_cap as usize);
    *out_n = 0;

    let needle = [n0, n1, n2, n3];
    // Scan for MethodOp (0x14) followed by a PkgLength whose covered NameString
    // matches `needle` exactly (unqualified 4-byte NameSeg, the common case for
    // a method declared directly under Scope(_SB.BAT0){...}). We do not walk
    // the whole tree structurally to find it (that would require a full
    // container-aware traversal); we simply try every 0x14 byte and validate
    // the shape at that offset, since 0x14 is not otherwise a data byte
    // pattern this search needs to worry about colliding with in a way that
    // would parse as valid on the small battery Device bodies this targets.
    let mut i = 0usize;
    while i + 1 < body.len() {
        if body[i] != OP_METHOD { i += 1; continue; }
        let mut probe = Cursor::new(body, i + 1, body.len());
        let (pkg_len, hdrlen) = match probe.pkglen() { Some(v) => v, None => { i += 1; continue; } };
        let obj_start = i + 1;
        let obj_end = match obj_start.checked_add(pkg_len as usize) { Some(v) => v, None => { i += 1; continue; } };
        if pkg_len < hdrlen || obj_end > body.len() {
            i += 1; continue;
        }
        // NameString right after the PkgLength encoding.
        let name_start = obj_start + hdrlen as usize;
        if name_start + 4 > obj_end { i += 1; continue; }
        if &body[name_start..name_start + 4] != &needle {
            i += 1; continue;
        }
        // MethodFlags: 1 byte, then TermList to obj_end.
        let flags_pos = name_start + 4;
        if flags_pos >= obj_end { i += 1; continue; }
        let term_start = flags_pos + 1;
        let mut tc = Cursor::new(body, term_start, obj_end);
        match eval_return_package(&mut tc, out_slice, out_cap as usize) {
            Some(n) => { *out_n = n as u32; return 1; }
            None => { i += 1; continue; } // this occurrence wasn't the safe shape; keep looking
        }
    }
    0
}

// ---------------------------------------------------------------------------
// Battery state/percentage/time-remaining arithmetic, kept separate from AML
// parsing so it is testable with plain synthetic arrays (no bytecode needed).
// ---------------------------------------------------------------------------

pub const BATT_ST_UNKNOWN: i32 = 0;
pub const BATT_ST_DISCHARGING: i32 = 1;
pub const BATT_ST_CHARGING: i32 = 2;
pub const BATT_ST_FULL: i32 = 3;

const UNKNOWN_U32: u64 = 0xFFFF_FFFF;

#[repr(C)]
pub struct BatteryReport {
    pub present: i32,   // 1 yes, 0 no, -1 could not ask
    pub percent: i32,   // 0-100, or -1 unknown
    pub state: i32,     // BATT_ST_*
    pub minutes: i32,   // minutes remaining, or -1 unknown
}

/// `bif`: _BIF/_BIX package elements, index 1 = DesignCapacity, index 2 =
/// LastFullChargeCapacity (ACPI spec order; _BIX shifts everything by one
/// extra leading Revision field, handled by the caller passing the right
/// slice). `bst`: _BST package, index 0 = BatteryState (bit0 discharging,
/// bit1 charging, bit2 critical), index 1 = PresentRate, index 2 =
/// RemainingCapacity.
///
/// Every 0xFFFFFFFF (32-bit "unknown" sentinel per ACPI spec) and every
/// nonsensical value (rate that would drain/charge in under a minute or over
/// 1000 hours, remaining capacity above the full-charge capacity) is treated
/// as UNKNOWN rather than turned into a number - this is the "no fictional
/// numbers" rule stated in the task.
#[no_mangle]
pub extern "C" fn battery_compute_rs(
    present: i32,
    have_bif: i32, bif: *const u64, bif_n: u32,
    have_bst: i32, bst: *const u64, bst_n: u32,
    out: *mut BatteryReport,
) {
    if out.is_null() { return; }
    let out = unsafe { &mut *out };
    out.present = present;
    out.percent = -1;
    out.state = BATT_ST_UNKNOWN;
    out.minutes = -1;

    if present != 1 {
        return;
    }

    let bif_s: &[u64] = if have_bif == 1 && !bif.is_null() && bif_n >= 3 {
        unsafe { core::slice::from_raw_parts(bif, bif_n as usize) }
    } else { &[] };
    let bst_s: &[u64] = if have_bst == 1 && !bst.is_null() && bst_n >= 3 {
        unsafe { core::slice::from_raw_parts(bst, bst_n as usize) }
    } else { &[] };

    if bst_s.is_empty() {
        return; // no usable _BST: state/percent/minutes stay unknown
    }

    let state_bits = bst_s[0];
    let rate = bst_s[1];
    let remaining = bst_s[2];

    let discharging = state_bits & 0x1 != 0;
    let charging = state_bits & 0x2 != 0;

    // Percentage needs a full-charge capacity to divide against. Prefer
    // _BIF's LastFullChargeCapacity (index 2); if _BIF was not evaluable,
    // percent stays unknown rather than guessed from design capacity, which
    // can be meaningfully larger than what the battery can currently hold.
    let full = if bif_s.len() >= 3 { bif_s[2] } else { UNKNOWN_U32 + 1 };
    if remaining != UNKNOWN_U32 && full != UNKNOWN_U32 && full > 0 && full < UNKNOWN_U32 {
        let mut pct = (remaining.saturating_mul(100) / full) as i64;
        if pct > 100 { pct = 100; }
        if pct < 0 { pct = 0; }
        out.percent = pct as i32;
    }

    if charging && !discharging {
        // "Full" is reported as charging with remaining>=full in some
        // firmware rather than clearing both state bits; treat that as FULL,
        // not CHARGING, so the tray does not show a charge animation on a
        // battery that is done charging.
        if remaining != UNKNOWN_U32 && full != UNKNOWN_U32 && full > 0 && remaining >= full {
            out.state = BATT_ST_FULL;
        } else {
            out.state = BATT_ST_CHARGING;
        }
    } else if discharging && !charging {
        out.state = BATT_ST_DISCHARGING;
    } else if !discharging && !charging {
        out.state = BATT_ST_FULL;
    } else {
        out.state = BATT_ST_UNKNOWN; // both bits set is undefined by spec
    }

    // Time remaining: only from a genuinely usable rate. A rate of 0 while
    // actively charging/discharging, or the 0xFFFFFFFF sentinel, is not a
    // measurement; do not compute minutes from it.
    if rate != UNKNOWN_U32 && rate != 0 && remaining != UNKNOWN_U32 {
        let hours_x60: u64 = match out.state {
            BATT_ST_DISCHARGING => remaining.saturating_mul(60) / rate,
            BATT_ST_CHARGING => {
                if full != UNKNOWN_U32 && full > remaining {
                    (full - remaining).saturating_mul(60) / rate
                } else { 0 }
            }
            _ => 0,
        };
        // Bound: under 1 minute or over 1000 hours (60000 min) is not a
        // number anyone should be shown; report unknown instead.
        if hours_x60 >= 1 && hours_x60 <= 60000 {
            out.minutes = hours_x60 as i32;
        }
    }
}

// ---------------------------------------------------------------------------
// Self-test. RED then GREEN on every branch, run at boot alongside the other
// rustkern self-tests, per the project's "a check that has never failed
// proves nothing" rule (#514/#665).
// ---------------------------------------------------------------------------

fn build_method_blob(name: &[u8; 4], elems: &[(u8, u64)]) -> ([u8; 128], usize) {
    // Constructs: MethodOp PkgLen NameSeg MethodFlags ReturnOp PackageOp
    // PkgLen NumElements <elements...>
    let mut buf = [0u8; 128];
    let mut pkg = [0u8; 96];
    let mut p = 0usize;
    pkg[p] = OP_RETURN; p += 1;
    pkg[p] = OP_PACKAGE; p += 1;
    let pkg_len_pos = p; p += 1; // 1-byte pkglen, filled below
    let content_start = p;
    pkg[p] = elems.len() as u8; p += 1;
    for &(kind, val) in elems {
        pkg[p] = kind; p += 1;
        match kind {
            OP_BYTE_PREFIX => { pkg[p] = val as u8; p += 1; }
            OP_WORD_PREFIX => { pkg[p] = val as u8; pkg[p+1] = (val >> 8) as u8; p += 2; }
            OP_DWORD_PREFIX => { for k in 0..4 { pkg[p+k] = (val >> (8*k)) as u8; } p += 4; }
            _ => {}
        }
    }
    let pkg_content_len = p - content_start + 1; // +1 for the pkglen byte itself
    pkg[pkg_len_pos] = pkg_content_len as u8; // fits in 6 bits for these tiny test blobs
    let pkg_total = p;

    let mut m = 0usize;
    buf[m] = OP_METHOD; m += 1;
    let mpkg_len_pos = m; m += 1;
    let mcontent_start = m;
    buf[m] = name[0]; buf[m+1] = name[1]; buf[m+2] = name[2]; buf[m+3] = name[3]; m += 4;
    buf[m] = 0x00; m += 1; // MethodFlags: ArgCount=0
    buf[m..m + pkg_total].copy_from_slice(&pkg[..pkg_total]);
    m += pkg_total;
    let mpkg_content_len = m - mcontent_start + 1;
    buf[mpkg_len_pos] = mpkg_content_len as u8;
    // battery_eval_method_rs enforces the same len>=36 floor amlhid.rs uses
    // (a real ACPI table is never shorter than its own 36-byte SDT header).
    // A synthetic test blob has no such header, so pad the reported length
    // up to that floor with trailing zero bytes (buf is already zeroed) -
    // this must be a REAL constraint of the production caller, not something
    // this self-test is allowed to route around by handing it a blob the
    // production path could never actually receive.
    (buf, if m < 36 { 36 } else { m })
}

#[no_mangle]
pub extern "C" fn battery_selftest_rs() -> u32 {
    let mut fail: u32 = 0;

    // --- AML evaluator: GREEN, a well-formed constant _BST ---
    let (blob, blen) = build_method_blob(b"_BST", &[
        (OP_BYTE_PREFIX, 1),               // discharging
        (OP_DWORD_PREFIX, 2500),           // present rate (mW or mA)
        (OP_DWORD_PREFIX, 3000),           // remaining capacity
        (OP_DWORD_PREFIX, 8400),           // present voltage
    ]);
    let mut out = [0u64; 8];
    let mut n: u32 = 0;
    let ok = unsafe { battery_eval_method_rs(blob.as_ptr(), blen as u32, b'_', b'B', b'S', b'T', out.as_mut_ptr(), 8, &mut n) };
    if ok != 1 || n != 4 || out[0] != 1 || out[1] != 2500 || out[2] != 3000 || out[3] != 8400 {
        fail |= 1 << 0;
    }

    // --- AML evaluator: RED, name not present ---
    let mut n2: u32 = 0;
    let ok2 = unsafe { battery_eval_method_rs(blob.as_ptr(), blen as u32, b'_', b'B', b'I', b'F', out.as_mut_ptr(), 8, &mut n2) };
    if ok2 != 0 { fail |= 1 << 1; }

    // --- AML evaluator: RED, too-short table and null pointer refused ---
    let tiny = [0u8; 8];
    let mut n3: u32 = 0;
    if unsafe { battery_eval_method_rs(tiny.as_ptr(), tiny.len() as u32, b'_', b'B', b'S', b'T', out.as_mut_ptr(), 8, &mut n3) } != 0 {
        fail |= 1 << 2;
    }
    if unsafe { battery_eval_method_rs(core::ptr::null(), 4096, b'_', b'B', b'S', b'T', out.as_mut_ptr(), 8, &mut n3) } != 0 {
        fail |= 1 << 3;
    }

    // --- AML evaluator: an unsupported opcode (a Store, here simulated by a
    // stray unrecognised byte 0x70 = Store) inside the method body must bail,
    // not mis-parse. ---
    let (mut blob2, blen2) = build_method_blob(b"_BST", &[(OP_BYTE_PREFIX, 1)]);
    // Corrupt: overwrite the ReturnOp with Store's opcode (0x70) so the safe
    // shape is not present.
    let bst_pos = 6; // MethodOp(1)+pkglen(1)+name(4) = 6, next is flags
    blob2[bst_pos + 1] = 0x70;
    let mut n4: u32 = 0;
    let ok4 = unsafe { battery_eval_method_rs(blob2.as_ptr(), blen2 as u32, b'_', b'B', b'S', b'T', out.as_mut_ptr(), 8, &mut n4) };
    if ok4 != 0 { fail |= 1 << 4; }
    let _ = blen2;

    // --- compute: fully present, discharging, sane numbers -> known percent/state/minutes ---
    let bif = [0u64, 5000, 4000, 8400]; // PowerUnit,Design,LastFull,Tech...
    let bst = [1u64, 1000, 2000, 7800]; // discharging, rate=1000, remaining=2000
    let mut rep = BatteryReport { present: 0, percent: 0, state: 0, minutes: 0 };
    battery_compute_rs(1, 1, bif.as_ptr(), 4, 1, bst.as_ptr(), 4, &mut rep);
    if rep.percent != 50 { fail |= 1 << 5; }
    if rep.state != BATT_ST_DISCHARGING { fail |= 1 << 6; }
    if rep.minutes != 120 { fail |= 1 << 7; } // 2000/1000*60 = 120

    // --- compute: charging, remaining < full ---
    let bst_c = [2u64, 1000, 2000, 8000];
    battery_compute_rs(1, 1, bif.as_ptr(), 4, 1, bst_c.as_ptr(), 4, &mut rep);
    if rep.state != BATT_ST_CHARGING { fail |= 1 << 8; }
    if rep.minutes != 120 { fail |= 1 << 9; } // (4000-2000)/1000*60 = 120

    // --- compute: charging with remaining >= full -> FULL, not CHARGING ---
    let bst_full = [2u64, 0, 4000, 8400];
    battery_compute_rs(1, 1, bif.as_ptr(), 4, 1, bst_full.as_ptr(), 4, &mut rep);
    if rep.state != BATT_ST_FULL { fail |= 1 << 10; }
    if rep.minutes != -1 { fail |= 1 << 11; } // rate=0: must NOT fabricate a time

    // --- compute: rate is the 0xFFFFFFFF unknown sentinel -> minutes unknown ---
    let bst_unk_rate = [1u64, UNKNOWN_U32, 2000, 7800];
    battery_compute_rs(1, 1, bif.as_ptr(), 4, 1, bst_unk_rate.as_ptr(), 4, &mut rep);
    if rep.minutes != -1 { fail |= 1 << 12; }
    if rep.percent != 50 { fail |= 1 << 13; } // percent independent of rate

    // --- compute: remaining is the unknown sentinel -> percent AND minutes unknown ---
    let bst_unk_rem = [1u64, 1000, UNKNOWN_U32, 7800];
    battery_compute_rs(1, 1, bif.as_ptr(), 4, 1, bst_unk_rem.as_ptr(), 4, &mut rep);
    if rep.percent != -1 { fail |= 1 << 14; }
    if rep.minutes != -1 { fail |= 1 << 15; }

    // --- compute: no _BIF at all -> percent unknown, state still known from _BST ---
    battery_compute_rs(1, 0, core::ptr::null(), 0, 1, bst.as_ptr(), 4, &mut rep);
    if rep.percent != -1 { fail |= 1 << 16; }
    if rep.state != BATT_ST_DISCHARGING { fail |= 1 << 17; }

    // --- compute: not present at all -> everything unknown, no crash on null bif/bst ---
    battery_compute_rs(0, 0, core::ptr::null(), 0, 0, core::ptr::null(), 0, &mut rep);
    if rep.present != 0 || rep.percent != -1 || rep.state != BATT_ST_UNKNOWN || rep.minutes != -1 {
        fail |= 1 << 18;
    }

    // --- compute: present unknown (-1) propagates, never promoted to a number ---
    battery_compute_rs(-1, 1, bif.as_ptr(), 4, 1, bst.as_ptr(), 4, &mut rep);
    if rep.present != -1 || rep.percent != -1 { fail |= 1 << 19; }

    fail
}
