// rustkern/ktz.rs - #86: the KERNEL's reader for the timezone the user chose.
//
// WHY A KERNEL-SIDE READER EXISTS AT ALL, AND WHY IT IS NOT A SECOND TIMEZONE
// IMPLEMENTATION.
//
// #49/#50 consolidated timezone handling into userland/libc/tz.c: ONE zone
// list, ONE persisted setting (TZ.CFG, holding the zone ID STRING), ONE reader,
// ONE local-time helper. Every clock in the OS calls it. That consolidation
// held: Settings deleted its private 26-entry list and now delegates.
//
// The kernel's login screen (gui/login.c) cannot call it. It runs in Ring 0
// before any user process exists, so it cannot link a userland library. It
// therefore drew the RAW CMOS RTC, which this OS keeps in UTC, and a user in
// Adelaide saw a login clock 9.5 hours out while every other clock in the OS
// was right. That is #86.
//
// SO WHAT IS SHARED IS THE FILE FORMAT AND THE ZONE ID, NOT A COPY OF THE LIST.
// TZ.CFG stores a token like "UTC+09:30", and that token ENCODES ITS OWN
// OFFSET. So the kernel needs a PARSER, not a table: there is no 35-entry zone
// list in here to drift out of step with userland's, because there is no zone
// list in here at all. If userland adds Kathmandu tomorrow, this code reads it
// correctly having never been touched. That is the difference between reusing a
// format and forking an implementation.
//
// RUST, per the standing directive. It is new kernel code and it is a PARSER OF
// UNTRUSTED ON-DISK BYTES, which is the exact class where this tree has already
// shipped a real out-of-bounds read (#476, ext2_lookup trusting a disk-supplied
// name_len). Every index below is bounds-checked by construction.
//
// NO DST, deliberately, matching userland: a zone here is a fixed offset the
// user picked. userland/libc/tz.h states that limitation and this must not
// quietly invent a different policy.

/// Parse the timezone offset, in MINUTES EAST OF UTC, out of a TZ.CFG body.
///
/// Accepts the same shapes userland/libc/tz.c's tz_parse() accepts: leading
/// whitespace, other key=value lines, CRLF, and a missing trailing newline.
/// The value is a zone ID of the form "UTC", "UTC+HH:MM" or "UTC-HH:MM".
///
/// Returns 0 and writes *out_min on success. Returns -1 and writes NOTHING if
/// there is no parseable tz= line, so the caller keeps its previous value
/// rather than silently snapping to UTC on a torn or truncated read.
#[no_mangle]
pub extern "C" fn ktz_parse_offset_rs(buf: *const u8, len: i32, out_min: *mut i32) -> i32 {
    if buf.is_null() || out_min.is_null() || len <= 0 {
        return -1;
    }
    // SAFETY: caller guarantees `buf` points to `len` readable bytes. Length is
    // clamped to a sane ceiling so a corrupt size cannot make a huge slice.
    let n = if len > 4096 { 4096usize } else { len as usize };
    let s = unsafe { core::slice::from_raw_parts(buf, n) };

    let mut i = 0usize;
    while i + 3 <= s.len() {
        // Match "tz=" only at the start of a line (or of the buffer), exactly
        // as tz_parse() does. Without the line-start test, a key like "vtz="
        // would match.
        if !(s[i] == b't' && s[i + 1] == b'z' && s[i + 2] == b'=') {
            i += 1;
            continue;
        }
        if i > 0 && s[i - 1] != b'\n' && s[i - 1] != b'\r' && s[i - 1] != b' ' {
            i += 1;
            continue;
        }
        let mut j = i + 3;
        while j < s.len() && (s[j] == b' ' || s[j] == b'\t') {
            j += 1;
        }
        // Value runs to end of line.
        let start = j;
        while j < s.len() && s[j] != b'\n' && s[j] != b'\r' {
            j += 1;
        }
        let mut end = j;
        while end > start && (s[end - 1] == b' ' || s[end - 1] == b'\t') {
            end -= 1;
        }
        return parse_zone_id(&s[start..end], out_min);
    }
    -1
}

/// "UTC" | "UTC+HH:MM" | "UTC-HH:MM" -> minutes east of UTC.
fn parse_zone_id(v: &[u8], out_min: *mut i32) -> i32 {
    if v.len() < 3 || v[0] != b'U' || v[1] != b'T' || v[2] != b'C' {
        return -1;
    }
    if v.len() == 3 {
        // Bare "UTC" is a legitimate zone ID meaning zero offset.
        // SAFETY: out_min was null-checked by the caller.
        unsafe { *out_min = 0 };
        return 0;
    }
    // Need exactly sign + HH + ':' + MM.
    if v.len() != 9 {
        return -1;
    }
    let neg = match v[3] {
        b'+' => false,
        b'-' => true,
        _ => return -1,
    };
    if v[6] != b':' {
        return -1;
    }
    let d = |c: u8| -> i32 {
        if c.is_ascii_digit() { (c - b'0') as i32 } else { -1 }
    };
    let (h1, h2, m1, m2) = (d(v[4]), d(v[5]), d(v[7]), d(v[8]));
    if h1 < 0 || h2 < 0 || m1 < 0 || m2 < 0 {
        return -1;
    }
    let hours = h1 * 10 + h2;
    let mins = m1 * 10 + m2;
    // Real zones run from UTC-12:00 to UTC+14:00. Anything outside that is a
    // corrupt file, and a corrupt file must be a REFUSAL, not an offset that
    // silently moves the clock by a plausible-looking amount.
    if hours > 14 || mins > 59 {
        return -1;
    }
    let total = hours * 60 + mins;
    if total > 14 * 60 {
        return -1;
    }
    // SAFETY: out_min was null-checked by the caller.
    unsafe { *out_min = if neg { -total } else { total } };
    0
}

/// Self-test over the parser. 0 = pass. Provably RED via `make KTZTESTFAIL=1`.
#[no_mangle]
pub extern "C" fn ktz_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    let mut chk = |cond: bool| {
        n += 1;
        if !cond {
            ok = false;
        }
    };

    let mut o: i32 = 0x7FFF_FFFF;
    let mut p = |txt: &[u8], out: &mut i32| -> i32 {
        ktz_parse_offset_rs(txt.as_ptr(), txt.len() as i32, out as *mut i32)
    };

    // --- the shapes the wizard and Settings actually write -----------------
    chk(p(b"tz=UTC+09:30\n", &mut o) == 0 && o == 570);   // Adelaide
    chk(p(b"tz=UTC-08:00\n", &mut o) == 0 && o == -480);  // Pacific
    chk(p(b"tz=UTC+05:45\n", &mut o) == 0 && o == 345);   // Nepal
    chk(p(b"tz=UTC+12:45\n", &mut o) == 0 && o == 765);   // Chatham
    chk(p(b"tz=UTC+00:00\n", &mut o) == 0 && o == 0);
    chk(p(b"tz=UTC-12:00\n", &mut o) == 0 && o == -720);  // list minimum
    chk(p(b"tz=UTC+14:00\n", &mut o) == 0 && o == 840);   // list maximum
    chk(p(b"tz=UTC", &mut o) == 0 && o == 0);             // bare ID, no newline

    // OFFSETS ARE MINUTES, NOT HOURS. If anyone ever "simplifies" this to whole
    // hours, India, Adelaide, Nepal and Chatham all break. Assert a non-zero
    // minutes field explicitly so that regression cannot pass.
    chk(p(b"tz=UTC+05:30\n", &mut o) == 0 && o == 330 && (o % 60) != 0);

    // --- tolerated formatting ---------------------------------------------
    chk(p(b"tz=UTC+01:00\r\n", &mut o) == 0 && o == 60);          // CRLF
    chk(p(b"tz=  UTC+01:00\n", &mut o) == 0 && o == 60);          // padding
    chk(p(b"tz=UTC+01:00   \n", &mut o) == 0 && o == 60);         // trailing
    chk(p(b"other=1\ntz=UTC+02:00\n", &mut o) == 0 && o == 120);  // second line
    chk(p(b"tz=UTC+03:00", &mut o) == 0 && o == 180);             // no newline

    // --- REFUSALS. Each of these must write NOTHING ------------------------
    // A refusal that quietly returns 0 minutes is the dangerous failure: the
    // clock would look fine and be wrong for everyone not on UTC.
    let mut keep: i32 = 12345;
    chk(p(b"", &mut keep) == -1 && keep == 12345);
    chk(p(b"tz=\n", &mut keep) == -1 && keep == 12345);
    chk(p(b"tz=GMT+01:00\n", &mut keep) == -1 && keep == 12345);
    chk(p(b"tz=UTC+1:00\n", &mut keep) == -1 && keep == 12345);   // not 2-digit
    chk(p(b"tz=UTC+01-00\n", &mut keep) == -1 && keep == 12345);  // wrong sep
    chk(p(b"tz=UTC*01:00\n", &mut keep) == -1 && keep == 12345);  // bad sign
    chk(p(b"tz=UTC+xx:00\n", &mut keep) == -1 && keep == 12345);  // non-digit
    chk(p(b"tz=UTC+15:00\n", &mut keep) == -1 && keep == 12345);  // > +14:00
    chk(p(b"tz=UTC+13:99\n", &mut keep) == -1 && keep == 12345);  // minutes > 59
    chk(p(b"nothinghere\n", &mut keep) == -1 && keep == 12345);
    // "tz=" must be at a line start: a key that merely ENDS in tz= is not ours.
    chk(p(b"vtz=UTC+01:00\n", &mut keep) == -1 && keep == 12345);

    // --- the parser must not read past the length it was given -------------
    // Same buffer, shorter declared length: the offset lives beyond `len`, so
    // this is a refusal, and if it is not, we are reading out of bounds.
    let long = b"tz=UTC+09:30\n";
    chk(ktz_parse_offset_rs(long.as_ptr(), 5, &mut keep as *mut i32) == -1
        && keep == 12345);
    chk(ktz_parse_offset_rs(core::ptr::null(), 10, &mut keep as *mut i32) == -1);
    chk(ktz_parse_offset_rs(long.as_ptr(), 0, &mut keep as *mut i32) == -1);

    // The DELIBERATE failure, so this line has been WATCHED to go red.
    #[cfg(ktz_test_fail)]
    chk(p(b"tz=UTC+09:30\n", &mut o) == 0 && o == 999);

    if !out_checks.is_null() {
        // SAFETY: null-checked; caller passes the address of a u32 local.
        unsafe { *out_checks = n };
    }
    if ok { 0 } else { -1 }
}
