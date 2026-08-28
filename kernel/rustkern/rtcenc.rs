// rustkern/rtcenc.rs - #135: THE one MC146818 RTC register codec, both directions.
//
// WHY THIS EXISTS: a read/write asymmetry that shipped a 6h06m clock error.
//
// This tree had FOUR places that turned an RTC register byte into a number and
// ONE place that turned a number back into a register byte. All five hardcoded
// their own arithmetic, and the four decoders did not agree with the one
// encoder:
//
//   DECODERS (all three consult Status Register B bit 2 and are therefore
//   correct about BCD-vs-binary):
//     gui/clock.c      rtc_read_time() / rtc_read_date()   - the canonical pair,
//                      behind syscalls 142/143, rustkern/ktime.rs's wall clock,
//                      the login clock, and every userland clock via tz.c.
//     gui/desktop.c    rtc_get_time()                      - a private 4th copy.
//
//   ENCODER (consults NOTHING):
//     proc/syscall.c   sys_set_rtc_time() / sys_set_rtc_date(), syscalls
//                      144/145, called `bin_to_bcd()` UNCONDITIONALLY.
//
// So on any machine whose RTC runs in BINARY mode (Status Register B bit 2 set)
// every clock-set wrote a BCD-shaped byte into a register the chip reads as
// binary, and the decoders then faithfully reported the wrong number back. The
// error is exact and computable: writing decimal v stores
// 16*floor(v/10) + v%10, so the field reads back HIGH by
//
//     6 * floor(v / 10)
//
// which for an hour in 10..=19 is +6 hours and for a minute in 10..=19 is
// +6 minutes. Set the clock at any local time of the form 1X:1Y and it reads
// exactly SIX HOURS AND SIX MINUTES FAST, forever, across reboots, because the
// wrong value is now sitting in battery-backed CMOS.
//
// That mattered more than a cosmetic clock, because EVERY path that corrects
// the time funnels through the broken encoder: the first-run wizard's Date and
// Time page, the Settings date/time panel, and net/sntp.c's SNTP client, which
// applies its validated UTC result with sys_set_rtc_time()/sys_set_rtc_date().
// A working NTP client on top of a broken encoder writes the network's correct
// time and stores a wrong one.
//
// THE INVARIANT THIS MODULE MAKES STRUCTURAL: decode(encode(v)) == v for every
// representable field value under every combination of the two Status Register
// B mode bits. That is asserted by rtc_selftest_rs() over the full 0..=59 /
// 0..=23 domains, not sampled, because the whole failure was a mode combination
// nobody exercised.
//
// RUST, per the 2026-07-16 standing rule. New kernel code, pure integer
// arithmetic on scalars, no paging, no asm, no FPU (the kernel is soft-float
// with SSE disabled, so this was never a float problem in any language) and no
// hot path - the RTC is touched at most once per second and usually far less.
// There is no performance argument for C here.
//
// NOT HANDLED, DELIBERATELY, AND SYMMETRICALLY: the century register (CMOS
// 0x32). No decoder in this tree reads it - gui/clock.c does `year + 2000` -
// so an encoder that wrote it would reintroduce exactly the read/write
// asymmetry this module exists to remove. Whether 0x32 is even a century
// register is board-specific (it is declared by the ACPI FADT century field,
// which this kernel does not parse); blind-writing it on a machine that uses
// that byte for something else is a real way to break firmware state. When the
// decoder learns to read it, the encoder here gains the matching branch, in
// this file, in one place.

// Status Register B mode bits. Recorded here next to the arithmetic that
// depends on them, and defined for real in drivers/rtc.h, which is the side
// that reads the register. `allow(dead_code)`: this crate never reads them,
// but getting the POLARITY backwards is the easy half of this bug and it
// belongs written down where the codec is.
//
//   bit 2 (DM)    SET = registers hold BINARY, clear = packed BCD
//   bit 1 (24/12) SET = 24-hour, clear = 12-hour with bit 7 of hours as PM
#[allow(dead_code)]
pub const RTC_REGB_DM_BINARY: u8 = 0x04;
#[allow(dead_code)]
pub const RTC_REGB_24HOUR: u8 = 0x02;

const HOUR_PM_BIT: i32 = 0x80;

/// Encode a plain 0..=99 field (second, minute, day, month, 2-digit year) into
/// the byte the chip expects.
///
/// Returns -1 for a value this codec cannot represent. -1 is a REFUSAL and the
/// caller must not write it: storing a garbage byte in CMOS is worse than
/// leaving the previous time alone, which is the same discipline
/// rustkern/ktime.rs applies to implausible dates.
#[no_mangle]
pub extern "C" fn rtc_encode_field_rs(value: i32, is_bcd: i32) -> i32 {
    if !(0..=99).contains(&value) {
        return -1;
    }
    if is_bcd != 0 {
        ((value / 10) << 4) | (value % 10)
    } else {
        value
    }
}

/// Decode a plain field byte. The inverse of `rtc_encode_field_rs`.
///
/// A BCD byte whose nibbles are not both decimal digits is not a number this
/// chip could have produced; -1 says so rather than inventing a value from
/// hex nibbles, which is how a torn read becomes a plausible-looking date.
#[no_mangle]
pub extern "C" fn rtc_decode_field_rs(raw: i32, is_bcd: i32) -> i32 {
    let r = raw & 0xFF;
    if is_bcd != 0 {
        let hi = (r >> 4) & 0x0F;
        let lo = r & 0x0F;
        if hi > 9 || lo > 9 {
            return -1;
        }
        hi * 10 + lo
    } else {
        r
    }
}

/// Encode a 24-hour clock hour (0..=23) into the hours register, honouring
/// BOTH mode bits. In 12-hour mode the returned byte carries the PM flag in
/// bit 7, which is set AFTER the BCD packing (the flag is not a BCD digit).
///
/// Midnight is 12 AM and noon is 12 PM: the two values every hand-rolled
/// 12-hour conversion in the wild gets wrong, and both are covered by the
/// self-test.
#[no_mangle]
pub extern "C" fn rtc_encode_hour_rs(hour24: i32, is_bcd: i32, is_24h: i32) -> i32 {
    if !(0..=23).contains(&hour24) {
        return -1;
    }
    if is_24h != 0 {
        return rtc_encode_field_rs(hour24, is_bcd);
    }
    let pm = hour24 >= 12;
    let mut h12 = hour24 % 12;
    if h12 == 0 {
        h12 = 12; // 00:xx is 12 AM, 12:xx is 12 PM.
    }
    let enc = rtc_encode_field_rs(h12, is_bcd);
    if enc < 0 {
        return -1;
    }
    if pm {
        enc | HOUR_PM_BIT
    } else {
        enc
    }
}

/// Decode the hours register into a 24-hour clock hour (0..=23).
///
/// This is the inverse of `rtc_encode_hour_rs`, and it fixes a real defect in
/// the C it replaces: gui/clock.c's 12-hour branch was
/// `if (pm) hour = (hour % 12) + 12;` with no AM branch at all, so 12 AM
/// (register 12, PM clear) decoded to 12 rather than 0 - midnight read as
/// noon, a twelve-hour error on any machine whose firmware leaves the RTC in
/// 12-hour mode.
#[no_mangle]
pub extern "C" fn rtc_decode_hour_rs(raw: i32, is_bcd: i32, is_24h: i32) -> i32 {
    let r = raw & 0xFF;
    if is_24h != 0 {
        // Bit 7 is not a PM flag in 24-hour mode, but mask it anyway: some
        // firmware leaves it set and the old C masked it too, so this keeps
        // behaviour identical rather than newly rejecting a live machine.
        let h = rtc_decode_field_rs(r & 0x7F, is_bcd);
        if !(0..=23).contains(&h) {
            return -1;
        }
        return h;
    }
    let pm = (r & HOUR_PM_BIT) != 0;
    let h12 = rtc_decode_field_rs(r & 0x7F, is_bcd);
    if !(1..=12).contains(&h12) {
        return -1;
    }
    let base = h12 % 12; // 12 -> 0, so 12 AM -> 0 and 12 PM -> 12.
    if pm {
        base + 12
    } else {
        base
    }
}

// ===========================================================================
// SELF-TEST.
//
// Exhaustive over the domains that matter rather than sampled, because the
// shipped bug was a MODE COMBINATION that no sampled vector happened to cover.
// Every mode pair (BCD/binary x 12h/24h) is round-tripped over every legal
// value of every field.
//
// The absolute vectors below are hand-computed from the MC146818 datasheet
// encoding, not from this code, so the test cannot agree with a wrong
// implementation of itself - the differential-blindness lesson in blame.md.
// ===========================================================================

/// Returns 0 if every assertion passes, -1 otherwise. `out_checks` receives the
/// number of assertions actually executed so a caller can distinguish "passed"
/// from "never ran".
#[no_mangle]
pub extern "C" fn rtc_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    {
        let mut chk = |cond: bool| {
            n += 1;
            if !cond {
                ok = false;
            }
        };

        // --- Absolute encodings, read off the datasheet by hand. ---
        chk(rtc_encode_field_rs(0, 1) == 0x00);
        chk(rtc_encode_field_rs(9, 1) == 0x09);
        chk(rtc_encode_field_rs(10, 1) == 0x10);
        chk(rtc_encode_field_rs(59, 1) == 0x59);
        chk(rtc_encode_field_rs(59, 0) == 59);
        chk(rtc_encode_field_rs(10, 0) == 10);
        chk(rtc_decode_field_rs(0x59, 1) == 59);
        chk(rtc_decode_field_rs(0x59, 0) == 0x59); // 89 as binary, not 59.
        chk(rtc_decode_field_rs(0x1A, 1) == -1);   // 'A' is not a BCD digit.
        chk(rtc_encode_field_rs(100, 1) == -1);
        chk(rtc_encode_field_rs(-1, 0) == -1);

        // --- THE #135 REGRESSION VECTOR. ---
        // This is the shipped bug, written as an assertion. The old encoder was
        // `bin_to_bcd()` unconditionally; on a BINARY-mode chip that stores
        // 0x13 for 13 and 0x15 for 15, which read back as 19 and 21.
        // 19:21 - 13:15 = 6h06m, the exact error reported on the owner's iMac.
        let old_encoder_hour = ((13 / 10) << 4) | (13 % 10); // what shipped
        let old_encoder_min = ((15 / 10) << 4) | (15 % 10);
        chk(rtc_decode_field_rs(old_encoder_hour, 0) == 19);
        chk(rtc_decode_field_rs(old_encoder_min, 0) == 21);
        chk(19 * 60 + 21 - (13 * 60 + 15) == 6 * 60 + 6);
        // And the fix: on a binary chip the encoder must store the plain value,
        // so the round trip is exact.
        chk(rtc_encode_field_rs(13, 0) == 13);
        chk(rtc_decode_field_rs(rtc_encode_field_rs(13, 0), 0) == 13);
        chk(rtc_decode_field_rs(rtc_encode_field_rs(15, 0), 0) == 15);

        // --- 12-hour mode absolutes, including both midnight and noon. ---
        chk(rtc_encode_hour_rs(0, 1, 0) == 0x12); // 12 AM, PM bit clear
        chk(rtc_encode_hour_rs(12, 1, 0) == 0x92); // 12 PM, PM bit set
        chk(rtc_encode_hour_rs(13, 1, 0) == 0x81); // 1 PM
        chk(rtc_encode_hour_rs(11, 1, 0) == 0x11); // 11 AM
        chk(rtc_encode_hour_rs(23, 0, 0) == (11 | 0x80)); // binary 12h, 11 PM
        chk(rtc_decode_hour_rs(0x12, 1, 0) == 0); // the case the old C got wrong
        chk(rtc_decode_hour_rs(0x92, 1, 0) == 12);
        chk(rtc_encode_hour_rs(24, 1, 1) == -1);
        chk(rtc_encode_hour_rs(-1, 1, 1) == -1);
        chk(rtc_decode_hour_rs(0x00, 1, 0) == -1); // hour 0 is illegal in 12h

        // --- Exhaustive round trips over every mode pair. ---
        // Plain fields: 0..=59 covers seconds/minutes and contains the
        // 1..=31 / 1..=12 / 0..=99 sub-domains' interesting carries.
        let mut bcd = 0i32;
        while bcd <= 1 {
            let mut v = 0i32;
            while v <= 99 {
                let e = rtc_encode_field_rs(v, bcd);
                chk(e >= 0);
                chk(rtc_decode_field_rs(e, bcd) == v);
                v += 1;
            }
            // Hours across both 12/24 settings.
            let mut h24 = 0i32;
            while h24 <= 1 {
                let mut h = 0i32;
                while h <= 23 {
                    let e = rtc_encode_hour_rs(h, bcd, h24);
                    chk(e >= 0);
                    chk(rtc_decode_hour_rs(e, bcd, h24) == h);
                    h += 1;
                }
                h24 += 1;
            }
            bcd += 1;
        }
    }

    if !out_checks.is_null() {
        // SAFETY: null-checked; the caller passes the address of a u32 local.
        unsafe {
            *out_checks = n;
        }
    }
    if ok {
        0
    } else {
        -1
    }
}
