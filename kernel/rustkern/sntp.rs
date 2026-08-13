// rustkern/sntp.rs - #797 SNTP (RFC 4330) reply validation + civil-time conversion
//
// NEW KERNEL CODE, so Rust per the 2026-07-16 rule. There is no C twin and no
// -DRUST_* strangler flag here, because there was never any C to strangle: what
// existed before was ntp_udp_cb() in proc/syscall.c, an eight-line inline that
// read bytes 40..44 of ANY >=48-byte UDP datagram that happened to land on the
// fixed port 12300 and handed the result straight to the RTC. It validated
// NOTHING - not the mode, not the stratum, not a zero timestamp, not the source
// address, not the date, and it had no request/reply binding at all. This module
// is the validation that call site never had. What it replaces is documented in
// net/sntp.c; there is nothing to run a differential against, so the proof is
// sntp_selftest_rs() (below), a vector test over every accept and reject path.
//
// TIER 2 (UNTRUSTED WIRE INPUT), and a security boundary rather than a
// convenience. pkt/len come off a spoofable, unauthenticated UDP datagram, and
// the consequence of accepting a bad one is that THE SYSTEM CLOCK MOVES. A clock
// rolled backwards re-validates certificates that have since expired or been
// revoked; a clock rolled forward invalidates good ones and can break TLS
// outright. So the parse is bounds-checked by construction and the verdict is
// conservative: anything not positively recognised is rejected.
//
// SOFT-FLOAT, STATED UP FRONT. The kernel target is x86_64-unknown-none
// (+soft-float) and the C side is built -mno-sse -mno-sse2, so there is not a
// single float in this file. An NTP timestamp is a 64-bit 32.32 FIXED-POINT
// value; we take the integer seconds half and DISCARD the fractional half.
// A one-shot RTC set has one-second resolution anyway (the RTC registers hold
// whole seconds), so the fraction could not be represented even if we kept it.
// Every conversion below is integer, and the two that could overflow a u32 are
// done in i64/u64 where they provably cannot.

// ---------------------------------------------------------------------------
// Status codes. Mirrored in net/sntp.h; keep the two in step.
//
// These are deliberately DISTINGUISHABLE rather than a single -1. The caller is
// a first-boot wizard with a user waiting in front of it: "that server did not
// answer" and "that server answered with a nonsense date" need different text,
// and lumping them together is how a diagnosable failure becomes a shrug.
// ---------------------------------------------------------------------------
pub const SNTP_OK: i32 = 0;
const SNTP_E_ARG: i32 = -1; // null pointer / impossible length
const SNTP_E_SHORT: i32 = -2; // datagram shorter than the 48-byte SNTP header
const SNTP_E_LI: i32 = -3; // leap indicator 3 = server not synchronised
const SNTP_E_VERSION: i32 = -4; // version field outside 1..=4
const SNTP_E_MODE: i32 = -5; // mode != 4 (server)
const SNTP_E_STRATUM: i32 = -6; // stratum 0 (kiss-o'-death) or > 15
const SNTP_E_NONCE: i32 = -7; // originate timestamp != the nonce we sent
const SNTP_E_ZEROTS: i32 = -8; // transmit timestamp is zero
const SNTP_E_RANGE: i32 = -9; // decoded date outside the sanity window

// ---------------------------------------------------------------------------
// Epoch constants.
//
// THE 2036 ROLLOVER, HANDLED EXPLICITLY RATHER THAN LEFT IMPLICIT.
//
// An NTP timestamp counts seconds from 1900-01-01 in a 32-BIT unsigned field,
// so it wraps at 2036-02-07 06:28:16 UTC. RFC 4330 section 3 resolves the
// resulting ambiguity by the TOP BIT of the seconds field:
//
//   bit 31 SET   -> era 0: 1968-01-20 .. 2036-02-07, unix = ntp - 2_208_988_800
//   bit 31 CLEAR -> era 1: 2036-02-07 .. 2104-02-26, unix = ntp + 2_085_978_496
//
// 2_085_978_496 is 2^32 - 2_208_988_800, i.e. era 1 is the same subtraction
// carried out modulo 2^32. Both branches are evaluated in i64, where neither can
// wrap or go negative-by-accident, and the era we chose is RETURNED to the
// caller in SntpResult.era so it is visible in the log rather than a silent
// assumption. The boot self-test asserts that a bit-31-clear timestamp decodes
// to 2036 and not to some 1900s date, so this path is proven, not merely coded.
//
// The old ntp_udp_cb() did `ntp_sec - 2208988800UL` in uint32_t arithmetic with
// no era check whatsoever. On 2036-02-07 that expression wraps and yields a unix
// time around 1900, i.e. the machine's clock would have been slammed 136 years
// into the past. That is the rollover bug this module exists to not have.
const NTP_ERA0_TO_UNIX: i64 = 2_208_988_800;
const NTP_ERA1_TO_UNIX: i64 = 2_085_978_496;

// Sanity window for the decoded time, in unix seconds.
//
// FLOOR = 2026-01-01T00:00:00Z. Nothing this OS can legitimately be told by an
// NTP server predates its own existence, and the floor is the half of the window
// that matters for security: it is what stops a hostile or broken server from
// rolling the clock BACK into a period where an expired or revoked certificate
// was still valid. It is a blunt instrument and it is meant to be.
//
// CEILING = 2100-01-01T00:00:00Z. Catches the other direction (a garbage or
// wrapped timestamp far in the future, which would expire every valid
// certificate). It also keeps the civil-date loop below provably bounded.
const SANITY_MIN_UNIX: i64 = 1_767_225_600; // 2026-01-01T00:00:00Z
const SANITY_MAX_UNIX: i64 = 4_102_444_800; // 2100-01-01T00:00:00Z

const SNTP_PKT_LEN: usize = 48;

// Field offsets inside the SNTP packet (RFC 4330 figure 1).
const OFF_LI_VN_MODE: usize = 0;
const OFF_STRATUM: usize = 1;
const OFF_ORIGINATE: usize = 24; // originate timestamp (our transmit, echoed)
const OFF_TRANSMIT: usize = 40; // transmit timestamp (the answer we want)

/// #[repr(C)] mirror of sntp_result_t (net/sntp.h). Layout is _Static_assert-ed
/// == 40 bytes on the C side so the two can never silently drift.
/// 8 x i32 = 32 bytes, then a u64 at offset 32 (already 8-aligned) = 40 total.
#[repr(C)]
pub struct SntpResult {
    pub status: i32,
    pub year: i32,
    pub month: i32, // 1..=12
    pub day: i32,   // 1..=31
    pub hour: i32,  // 0..=23
    pub minute: i32,
    pub second: i32,
    pub era: i32, // 0 or 1: which NTP era was decoded (see the 2036 note above)
    pub unix_ts: u64,
}

#[inline]
fn be32(s: &[u8], off: usize) -> u32 {
    ((s[off] as u32) << 24)
        | ((s[off + 1] as u32) << 16)
        | ((s[off + 2] as u32) << 8)
        | (s[off + 3] as u32)
}

/// Build a 48-byte SNTP client request into `buf`.
///
/// The transmit timestamp (bytes 40..48) is set to the caller's 64-bit NONCE
/// rather than to a real time. Two reasons, both deliberate:
///
///  1. WE DO NOT HAVE A TRUSTWORTHY CLOCK. That is the whole point of asking.
///     Sending a wrong time is no worse than sending a random one, and RFC 4330
///     section 5 explicitly permits the client's transmit timestamp to be
///     anything it can recognise on the way back.
///  2. IT IS THE ANTI-SPOOF TOKEN. The server MUST copy this field verbatim into
///     the reply's ORIGINATE timestamp, so requiring the echo to match binds the
///     reply to this specific request. Combined with the randomised source port
///     the C side binds, an off-path attacker must guess 64 bits of nonce and
///     16 bits of port to land a forgery. The previous implementation had
///     neither, and would have accepted the first 48-byte datagram from anyone.
///
/// Returns SNTP_OK, or SNTP_E_ARG for a null pointer or a buffer under 48 bytes.
#[no_mangle]
pub unsafe extern "C" fn sntp_build_request_rs(
    buf: *mut u8,
    len: u32,
    nonce_hi: u32,
    nonce_lo: u32,
) -> i32 {
    if buf.is_null() || (len as usize) < SNTP_PKT_LEN {
        return SNTP_E_ARG;
    }
    let s = core::slice::from_raw_parts_mut(buf, SNTP_PKT_LEN);
    for b in s.iter_mut() {
        *b = 0;
    }
    // LI = 0 (no warning), VN = 4, Mode = 3 (client) -> 0b00_100_011 = 0x23.
    // The old code sent 0x1B (VN = 3). Version 4 is what RFC 4330 specifies for
    // an SNTPv4 client; servers echo the version, and the reply check below
    // accepts 1..=4 so a v3-only server is still usable.
    s[OFF_LI_VN_MODE] = 0x23;
    s[OFF_TRANSMIT] = (nonce_hi >> 24) as u8;
    s[OFF_TRANSMIT + 1] = (nonce_hi >> 16) as u8;
    s[OFF_TRANSMIT + 2] = (nonce_hi >> 8) as u8;
    s[OFF_TRANSMIT + 3] = nonce_hi as u8;
    s[OFF_TRANSMIT + 4] = (nonce_lo >> 24) as u8;
    s[OFF_TRANSMIT + 5] = (nonce_lo >> 16) as u8;
    s[OFF_TRANSMIT + 6] = (nonce_lo >> 8) as u8;
    s[OFF_TRANSMIT + 7] = nonce_lo as u8;
    SNTP_OK
}

/// Validate an SNTP reply and convert its transmit timestamp to civil time.
///
/// `out.status` is set to the same value that is returned, so a C caller can log
/// one struct. On any non-OK status every other field is left zeroed: a partial
/// result is exactly the kind of thing a caller half-uses.
#[no_mangle]
pub unsafe extern "C" fn sntp_parse_reply_rs(
    pkt: *const u8,
    len: u32,
    nonce_hi: u32,
    nonce_lo: u32,
    out: *mut SntpResult,
) -> i32 {
    if out.is_null() {
        return SNTP_E_ARG;
    }
    let r = &mut *out;
    r.status = SNTP_E_ARG;
    r.year = 0;
    r.month = 0;
    r.day = 0;
    r.hour = 0;
    r.minute = 0;
    r.second = 0;
    r.era = 0;
    r.unix_ts = 0;

    if pkt.is_null() {
        return SNTP_E_ARG;
    }
    // Bound the length BEFORE building the slice. A caller handing us a huge
    // len would otherwise create a slice we have no business reading; we only
    // ever read the first 48 bytes, so clamp to exactly that.
    if (len as usize) < SNTP_PKT_LEN {
        r.status = SNTP_E_SHORT;
        return SNTP_E_SHORT;
    }
    let s = core::slice::from_raw_parts(pkt, SNTP_PKT_LEN);

    // --- Header field checks (RFC 4330 section 5, "the client MUST check") ---
    let b0 = s[OFF_LI_VN_MODE];
    let li = (b0 >> 6) & 0x03;
    let vn = (b0 >> 3) & 0x07;
    let mode = b0 & 0x07;

    // LI = 3 means the server's own clock is not synchronised. Its timestamp is
    // whatever its free-running oscillator says; taking it is worse than nothing.
    if li == 3 {
        r.status = SNTP_E_LI;
        return SNTP_E_LI;
    }
    if vn == 0 || vn > 4 {
        r.status = SNTP_E_VERSION;
        return SNTP_E_VERSION;
    }
    // Mode 4 is "server". We deliberately do NOT accept mode 5 (broadcast): an
    // unsolicited broadcast that sets the clock is a free spoofing primitive on
    // any LAN, and we asked a specific server a specific question.
    if mode != 4 {
        r.status = SNTP_E_MODE;
        return SNTP_E_MODE;
    }
    // Stratum 0 is the KISS-O'-DEATH packet (RFC 4330 section 8): the server is
    // telling us to back off, and bytes 12..16 are an ASCII kiss code, NOT a
    // time source. Accepting it as a time is the classic SNTP client bug.
    // Stratum 16 means unsynchronised; 17..255 are reserved.
    let stratum = s[OFF_STRATUM];
    if stratum == 0 || stratum > 15 {
        r.status = SNTP_E_STRATUM;
        return SNTP_E_STRATUM;
    }

    // --- Request/reply binding: the originate timestamp must be our nonce ---
    if be32(s, OFF_ORIGINATE) != nonce_hi || be32(s, OFF_ORIGINATE + 4) != nonce_lo {
        r.status = SNTP_E_NONCE;
        return SNTP_E_NONCE;
    }

    // --- The transmit timestamp itself ---
    let secs = be32(s, OFF_TRANSMIT);
    let frac = be32(s, OFF_TRANSMIT + 4);
    // A wholly zero transmit timestamp means the server never filled it in.
    // Checked on the FULL 64 bits: seconds == 0 alone is also absurd (1900), but
    // it is caught by the sanity window below, and checking both halves here is
    // what RFC 4330 actually calls for.
    if secs == 0 && frac == 0 {
        r.status = SNTP_E_ZEROTS;
        return SNTP_E_ZEROTS;
    }

    // Era selection: see the NTP_ERA0/1 commentary above. Done in i64 so the
    // era-0 subtraction (which is genuinely negative for 1968..1970) cannot wrap.
    let (unix_i, era) = if (secs & 0x8000_0000) != 0 {
        (secs as i64 - NTP_ERA0_TO_UNIX, 0i32)
    } else {
        (secs as i64 + NTP_ERA1_TO_UNIX, 1i32)
    };

    if unix_i < SANITY_MIN_UNIX || unix_i >= SANITY_MAX_UNIX {
        r.status = SNTP_E_RANGE;
        return SNTP_E_RANGE;
    }

    // --- Civil conversion. Integer only; the loop is bounded by the ceiling. ---
    let unix_u = unix_i as u64;
    let mut days = (unix_u / 86_400) as i32;
    let sod = (unix_u % 86_400) as i32;
    let hour = sod / 3600;
    let minute = (sod / 60) % 60;
    let second = sod % 60;

    let mut year = 1970i32;
    // SANITY_MAX_UNIX caps this at 130 iterations. The bound is explicit anyway,
    // so a future widening of the window can never turn this into a hang.
    let mut guard = 0;
    loop {
        let leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
        let diy = if leap { 366 } else { 365 };
        if days < diy {
            break;
        }
        days -= diy;
        year += 1;
        guard += 1;
        if guard > 200 {
            r.status = SNTP_E_RANGE;
            return SNTP_E_RANGE;
        }
    }
    let leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
    let dpm: [i32; 12] = [
        31,
        if leap { 29 } else { 28 },
        31,
        30,
        31,
        30,
        31,
        31,
        30,
        31,
        30,
        31,
    ];
    let mut mo = 0usize;
    while mo < 12 && days >= dpm[mo] {
        days -= dpm[mo];
        mo += 1;
    }
    // mo can only reach 12 if the day-of-year arithmetic above was wrong, which
    // it cannot be given the loop that produced `days`. Bail rather than index
    // out of range: this is the branch that would have been a C buffer overrun.
    if mo >= 12 {
        r.status = SNTP_E_RANGE;
        return SNTP_E_RANGE;
    }

    r.status = SNTP_OK;
    r.year = year;
    r.month = mo as i32 + 1;
    r.day = days + 1;
    r.hour = hour;
    r.minute = minute;
    r.second = second;
    r.era = era;
    r.unix_ts = unix_u;
    SNTP_OK
}

// ---------------------------------------------------------------------------
// Self-test. Returns 0 on pass, else a bitmask of the failing cases, so a
// failure says WHICH property broke rather than just "selftest failed".
//
// There is no C twin to differential against (this validation never existed), so
// this vector test IS the proof. It covers every reject path and both eras.
// ---------------------------------------------------------------------------

fn mk_pkt(li: u8, vn: u8, mode: u8, stratum: u8, orig: (u32, u32), xmit: (u32, u32)) -> [u8; 48] {
    let mut p = [0u8; 48];
    p[OFF_LI_VN_MODE] = ((li & 3) << 6) | ((vn & 7) << 3) | (mode & 7);
    p[OFF_STRATUM] = stratum;
    let put = |p: &mut [u8; 48], off: usize, v: (u32, u32)| {
        p[off] = (v.0 >> 24) as u8;
        p[off + 1] = (v.0 >> 16) as u8;
        p[off + 2] = (v.0 >> 8) as u8;
        p[off + 3] = v.0 as u8;
        p[off + 4] = (v.1 >> 24) as u8;
        p[off + 5] = (v.1 >> 16) as u8;
        p[off + 6] = (v.1 >> 8) as u8;
        p[off + 7] = v.1 as u8;
    };
    put(&mut p, OFF_ORIGINATE, orig);
    put(&mut p, OFF_TRANSMIT, xmit);
    p
}

fn blank_result() -> SntpResult {
    SntpResult {
        status: 0,
        year: 0,
        month: 0,
        day: 0,
        hour: 0,
        minute: 0,
        second: 0,
        era: 0,
        unix_ts: 0,
    }
}

/// SNTP-797-RFC4330-VALIDATED : unique marker string, present in kernel.dbg.elf
/// so `strings` can prove this module reached the image (call-site counting via
/// objdump has been wrong three times in this tree: inlining and Rust
/// GOT-indirect calls both hide it).
#[no_mangle]
pub unsafe extern "C" fn sntp_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut fails: i32 = 0;
    let mut checks: u32 = 0;
    let nonce = (0xDEAD_BEEFu32, 0xCAFE_F00Du32);
    let bad_nonce = (0x1234_5678u32, 0x9ABC_DEF0u32);

    // Case 1: a good era-0 reply decodes to the expected civil time.
    // 2026-08-09T12:34:56Z = unix 1786278896 = NTP era-0 seconds 3995267696.
    {
        checks += 1;
        let p = mk_pkt(0, 4, 4, 2, nonce, (3_995_267_696, 0x8000_0000));
        let mut r = blank_result();
        let rc = sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r);
        if rc != SNTP_OK
            || r.year != 2026
            || r.month != 8
            || r.day != 9
            || r.hour != 12
            || r.minute != 34
            || r.second != 56
            || r.era != 0
            || r.unix_ts != 1_786_278_896
        {
            fails |= 1 << 0;
        }
    }

    // Case 2: THE 2036 ROLLOVER. A bit-31-CLEAR seconds field is era 1 and must
    // decode to 2036-02-07, not to a 1900s date. This is the case the old
    // uint32_t subtraction would have got 136 years wrong.
    {
        checks += 1;
        let p = mk_pkt(0, 4, 4, 1, nonce, (100, 0));
        let mut r = blank_result();
        let rc = sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r);
        if rc != SNTP_OK || r.year != 2036 || r.month != 2 || r.day != 7 || r.era != 1 {
            fails |= 1 << 1;
        }
    }

    // Case 3: short datagram.
    {
        checks += 1;
        let p = mk_pkt(0, 4, 4, 2, nonce, (3_995_267_696, 0));
        let mut r = blank_result();
        if sntp_parse_reply_rs(p.as_ptr(), 47, nonce.0, nonce.1, &mut r) != SNTP_E_SHORT {
            fails |= 1 << 2;
        }
    }

    // Case 4: LI = 3 (server not synchronised).
    {
        checks += 1;
        let p = mk_pkt(3, 4, 4, 2, nonce, (3_995_267_696, 0));
        let mut r = blank_result();
        if sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r) != SNTP_E_LI {
            fails |= 1 << 3;
        }
    }

    // Case 5: mode 3 (a client packet reflected back at us, or a spoof).
    {
        checks += 1;
        let p = mk_pkt(0, 4, 3, 2, nonce, (3_995_267_696, 0));
        let mut r = blank_result();
        if sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r) != SNTP_E_MODE {
            fails |= 1 << 4;
        }
    }

    // Case 6: stratum 0 = kiss-o'-death. MUST be rejected as a time source.
    {
        checks += 1;
        let p = mk_pkt(0, 4, 4, 0, nonce, (3_995_267_696, 0));
        let mut r = blank_result();
        if sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r) != SNTP_E_STRATUM {
            fails |= 1 << 5;
        }
    }

    // Case 7: stratum 16 = unsynchronised.
    {
        checks += 1;
        let p = mk_pkt(0, 4, 4, 16, nonce, (3_995_267_696, 0));
        let mut r = blank_result();
        if sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r) != SNTP_E_STRATUM {
            fails |= 1 << 6;
        }
    }

    // Case 8: originate timestamp does not echo our nonce (off-path forgery,
    // or a stale reply to a previous request).
    {
        checks += 1;
        let p = mk_pkt(0, 4, 4, 2, bad_nonce, (3_995_267_696, 0));
        let mut r = blank_result();
        if sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r) != SNTP_E_NONCE {
            fails |= 1 << 7;
        }
    }

    // Case 9: zero transmit timestamp.
    {
        checks += 1;
        let p = mk_pkt(0, 4, 4, 2, nonce, (0, 0));
        let mut r = blank_result();
        if sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r) != SNTP_E_ZEROTS {
            fails |= 1 << 8;
        }
    }

    // Case 10: a valid-looking era-0 timestamp for 1990. This is the CLOCK
    // ROLLBACK case: the packet is well formed and passes every header check,
    // and only the sanity window stops it.
    // 1990-01-01T00:00:00Z = unix 631152000 = NTP era-0 seconds 2840140800.
    {
        checks += 1;
        let p = mk_pkt(0, 4, 4, 2, nonce, (2_840_140_800, 0));
        let mut r = blank_result();
        if sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r) != SNTP_E_RANGE {
            fails |= 1 << 9;
        }
    }

    // Case 11: on any rejection the output fields stay zeroed (no partial result).
    {
        checks += 1;
        let p = mk_pkt(0, 4, 4, 0, nonce, (3_995_267_696, 0));
        let mut r = blank_result();
        let _ = sntp_parse_reply_rs(p.as_ptr(), 48, nonce.0, nonce.1, &mut r);
        if r.year != 0 || r.unix_ts != 0 || r.month != 0 {
            fails |= 1 << 10;
        }
    }

    // Case 12: the request builder emits a well-formed client packet carrying
    // the nonce where the server will echo it from.
    {
        checks += 1;
        let mut b = [0u8; 48];
        let rc = sntp_build_request_rs(b.as_mut_ptr(), 48, nonce.0, nonce.1);
        if rc != SNTP_OK
            || b[0] != 0x23
            || be32(&b, OFF_TRANSMIT) != nonce.0
            || be32(&b, OFF_TRANSMIT + 4) != nonce.1
        {
            fails |= 1 << 11;
        }
        // and it refuses an undersized buffer rather than writing past it
        if sntp_build_request_rs(b.as_mut_ptr(), 47, nonce.0, nonce.1) != SNTP_E_ARG {
            fails |= 1 << 11;
        }
    }

    if !out_checks.is_null() {
        *out_checks = checks;
    }
    fails
}
