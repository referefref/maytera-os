// rustkern/url.rs - #404 Phase P / #498 URL-string parse (net/url.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ---------------------------------------------------------------------------
// #404 / #498 Phase P: net/url.c URL-string parse (Tier 2, untrusted input:
// redirect Location headers + address bar). Pure parse - scheme/userinfo/host/
// port/path/query/fragment split, byte-for-byte matching url_parse_c. net/url.c
// routes the live url_parse() here under -DRUST_URL (else url_parse_c).
//
// SECURITY (honest): the C reference ALREADY caps every out-field copy (explicit
// >= sizeof(field) reject + safe_copy), so there is NO reachable fixed-buffer
// overflow (offline ASan+UBSan clean over 300k NUL-terminated vectors). This port
// removes the class BY CONSTRUCTION (every write is a bounds-checked slice index)
// and adds the C-s missing input upper bound (URL_MAX_INPUT): the C trusts NUL-
// termination with no cap (offline ASan proves a 1-byte over-read off a non-
// terminated buffer). Latent defense-in-depth, not a reachable win. url_t layout
// asserted == 2194 bytes in url.c so UrlParsed can never silently drift.
pub const URL_SCHEME_CAP: usize = 16;
pub const URL_HOST_CAP: usize = 256;
pub const URL_PATH_CAP: usize = 1024;
pub const URL_QUERY_CAP: usize = 512;
pub const URL_FRAG_CAP: usize = 256;
pub const URL_USERINFO_CAP: usize = 128;
// Max input scanned. The C url_parse trusts NUL-termination with NO upper bound;
// this cap confines a missing-terminator / unbounded input to a bounded scan.
// It is >> the sum of every out-field cap (~2200), so ANY URL the C could accept
// fits within it (identical fields) and ANY longer input is rejected by BOTH at
// the first field that exceeds its cap (identical reject); the cap can never
// change the accept/reject verdict vs the C. See net/url.c.
pub const URL_MAX_INPUT: usize = 8192;

// #[repr(C)] mirror of url_t (net/url.h). Layout asserted == 2194 bytes on the C
// side (_Static_assert in url.c) so this FFI struct can never silently drift.
#[repr(C)]
pub struct UrlParsed {
    pub scheme: [u8; 16],
    pub host: [u8; 256],
    pub port: u16,
    pub path: [u8; 1024],
    pub query: [u8; 512],
    pub fragment: [u8; 256],
    pub userinfo: [u8; 128],
}

#[inline]
fn url_is_alpha(c: u8) -> bool {
    (c >= b'a' && c <= b'z') || (c >= b'A' && c <= b'Z')
}
#[inline]
fn url_is_digit(c: u8) -> bool {
    c >= b'0' && c <= b'9'
}
#[inline]
fn url_is_alnum(c: u8) -> bool {
    url_is_alpha(c) || url_is_digit(c)
}
#[inline]
fn url_to_lower(c: u8) -> u8 {
    if c >= b'A' && c <= b'Z' {
        c + 32
    } else {
        c
    }
}

// Compare a NUL-terminated fixed buffer (e.g. the scheme[] just built) against a
// string literal, matching C's strcmp(buf, lit) == 0: equal bytes AND the buffer
// terminates (0) exactly at lit.len().
fn url_streq(buf: &[u8], lit: &[u8]) -> bool {
    let mut i = 0;
    while i < lit.len() {
        if i >= buf.len() || buf[i] != lit[i] {
            return false;
        }
        i += 1;
    }
    i < buf.len() && buf[i] == 0
}

// Default port for a NUL-terminated scheme buffer (verbatim url_default_port()).
fn url_default_port_rs(scheme: &[u8]) -> u16 {
    if url_streq(scheme, b"http") {
        80
    } else if url_streq(scheme, b"https") {
        443
    } else if url_streq(scheme, b"ftp") {
        21
    } else if url_streq(scheme, b"ssh") {
        22
    } else if url_streq(scheme, b"telnet") {
        23
    } else if url_streq(scheme, b"ws") {
        80
    } else if url_streq(scheme, b"wss") {
        443
    } else {
        0
    }
}

// strlen from an offset over the (infinite-trailing-zeros) string view.
fn url_strlen_from(g: &dyn Fn(usize) -> u8, p: usize) -> usize {
    let mut i = 0;
    while g(p + i) != 0 {
        i += 1;
    }
    i
}

// Verbatim safe_copy(): copy min(src_len, cap-1) bytes then NUL-terminate. Every
// write is a bounds-checked index into dst, so no copy can overrun the field.
fn url_scopy(dst: &mut [u8], g: &dyn Fn(usize) -> u8, src_off: usize, src_len: usize) -> usize {
    let cap = dst.len();
    if cap == 0 {
        return 0;
    }
    let copy_len = if src_len < cap - 1 { src_len } else { cap - 1 };
    let mut i = 0;
    while i < copy_len {
        dst[i] = g(src_off + i);
        i += 1;
    }
    dst[copy_len] = 0;
    copy_len
}

/// Rust port of net/url.c url_parse() (Tier 2, untrusted input: URL strings come
/// from attacker-controllable redirect Location headers + the address bar).
/// PURE parse/validate: splits a NUL-terminated URL string into
/// scheme/userinfo/host/port/path/query/fragment. Returns 1 on success, 0 on a
/// parse error, EXACTLY matching the C (default http scheme, default ports,
/// lowercased scheme+host, default "/" path, IPv6 [..] literal, over-long field
/// reject). Never sends, connects, allocates, touches a global, or mutates the
/// input. Every out-field write is a bounds-checked index (no strcpy-style
/// overrun) and the input scan is capped at URL_MAX_INPUT.
#[no_mangle]
pub extern "C" fn url_parse_rs(url_string: *const u8, out: *mut UrlParsed) -> i32 {
    // Matches C `if (!url_string || !out) return false;`.
    if url_string.is_null() || out.is_null() {
        return 0;
    }

    // SAFETY: url_string is a C NUL-terminated string supplied by the caller
    // (net/url.c url_parse -> browser redirect target / address bar). We scan at
    // most URL_MAX_INPUT bytes for the terminator: every read is *url_string.add(i)
    // with i < URL_MAX_INPUT, so even a missing terminator can never read beyond
    // the cap (the C trusts the terminator with NO upper bound; this cap is the
    // added confinement). `n` is the resulting strnlen and the slice below spans
    // exactly those n readable bytes.
    let n = {
        let mut i = 0usize;
        unsafe {
            while i < URL_MAX_INPUT && *url_string.add(i) != 0 {
                i += 1;
            }
        }
        i
    };
    let s: &[u8] = unsafe { core::slice::from_raw_parts(url_string, n) };
    // Byte accessor: s[i] for i < n, else 0. Models the NUL terminator (and the C
    // only ever reading past the string via short-circuited `*p` tests) as an
    // infinite run of trailing zeros. EVERY access below goes through this, so no
    // read leaves the slice.
    let g = |i: usize| -> u8 {
        if i < n {
            s[i]
        } else {
            0
        }
    };

    // == url_init(): zero the struct, default path "/". ==
    // SAFETY: out is non-null (checked) and a valid writable UrlParsed* per the C
    // ABI contract (same as the C reference). POD zero-init + field writes only.
    let o: &mut UrlParsed = unsafe { &mut *out };
    *o = UrlParsed {
        scheme: [0; 16],
        host: [0; 256],
        port: 0,
        path: [0; 1024],
        query: [0; 512],
        fragment: [0; 256],
        userinfo: [0; 128],
    };
    o.path[0] = b'/';
    o.path[1] = 0;

    let mut p: usize = 0;
    // skip leading whitespace
    while g(p) == b' ' || g(p) == b'\t' {
        p += 1;
    }
    // empty string
    if g(p) == 0 {
        return 0;
    }

    // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
    let mut start = p;
    if url_is_alpha(g(p)) {
        p += 1;
        while url_is_alnum(g(p)) || g(p) == b'+' || g(p) == b'-' || g(p) == b'.' {
            p += 1;
        }
    }

    if g(p) == b':' && p > start {
        let scheme_len = p - start;
        if scheme_len >= URL_SCHEME_CAP {
            return 0;
        }
        let mut i = 0;
        while i < scheme_len {
            o.scheme[i] = url_to_lower(g(start + i));
            i += 1;
        }
        o.scheme[scheme_len] = 0;

        p += 1; // skip ':'

        if g(p) == b'/' && g(p + 1) == b'/' {
            p += 2;
        } else if url_streq(&o.scheme, b"file") {
            if g(p) == b'/' {
                p += 1;
            }
        } else if url_streq(&o.scheme, b"mailto")
            || url_streq(&o.scheme, b"tel")
            || url_streq(&o.scheme, b"javascript")
        {
            let l = url_strlen_from(&g, p);
            url_scopy(&mut o.path, &g, p, l);
            return 1;
        } else {
            // other schemes: lenient, continue
        }
    } else {
        // no scheme -> default http
        p = start;
        o.scheme[0] = b'h';
        o.scheme[1] = b't';
        o.scheme[2] = b't';
        o.scheme[3] = b'p';
        o.scheme[4] = 0;
    }

    // authority: [userinfo@]host[:port], ending at / ? # or end
    start = p;
    let mut auth_end = p;
    while g(auth_end) != 0 && g(auth_end) != b'/' && g(auth_end) != b'?' && g(auth_end) != b'#' {
        auth_end += 1;
    }
    let _ = start;

    // first '@' -> userinfo
    let mut at_sign: usize = usize::MAX;
    {
        let mut c = p;
        while c < auth_end {
            if g(c) == b'@' {
                at_sign = c;
                break;
            }
            c += 1;
        }
    }
    if at_sign != usize::MAX {
        let userinfo_len = at_sign - p;
        if userinfo_len >= URL_USERINFO_CAP {
            return 0;
        }
        url_scopy(&mut o.userinfo, &g, p, userinfo_len);
        p = at_sign + 1;
    }

    // host[:port]
    let host_start0 = p;
    let mut host_end = auth_end;
    let mut port_start: usize = usize::MAX;
    if g(p) == b'[' {
        // IPv6 literal
        p += 1;
        while g(p) != 0 && g(p) != b']' {
            p += 1;
        }
        if g(p) != b']' {
            return 0;
        }
        p += 1; // skip ']'
        host_end = p;
        if g(p) == b':' && p < auth_end {
            port_start = p + 1;
        }
    } else {
        // last colon before auth_end is the port separator
        let mut c = p;
        while c < auth_end {
            if g(c) == b':' {
                port_start = c + 1;
                host_end = c;
            }
            c += 1;
        }
        if port_start == usize::MAX {
            host_end = auth_end;
        }
    }

    // host copy (lowercased); length cap checked INCLUDING brackets, exactly as C
    let mut hstart = host_start0;
    let mut host_len = host_end - hstart;
    if host_len > 0 {
        if host_len >= URL_HOST_CAP {
            return 0;
        }
        if g(hstart) == b'[' {
            hstart += 1;
            host_len -= 2;
        }
        let mut i = 0;
        while i < host_len {
            o.host[i] = url_to_lower(g(hstart + i));
            i += 1;
        }
        o.host[host_len] = 0;
    }

    // port
    if port_start != usize::MAX && port_start < auth_end {
        let mut port: u32 = 0;
        let mut c = port_start;
        while c < auth_end && url_is_digit(g(c)) {
            port = port * 10 + (g(c) - b'0') as u32;
            if port > 65535 {
                return 0;
            }
            c += 1;
        }
        o.port = port as u16;
    } else {
        o.port = url_default_port_rs(&o.scheme);
    }

    // path
    p = auth_end;
    start = p;
    while g(p) != 0 && g(p) != b'?' && g(p) != b'#' {
        p += 1;
    }
    let path_len = p - start;
    if path_len > 0 {
        if path_len >= URL_PATH_CAP {
            return 0;
        }
        url_scopy(&mut o.path, &g, start, path_len);
    } else {
        o.path[0] = b'/';
        o.path[1] = 0;
    }

    // query
    if g(p) == b'?' {
        p += 1;
        start = p;
        while g(p) != 0 && g(p) != b'#' {
            p += 1;
        }
        let query_len = p - start;
        if query_len > 0 {
            if query_len >= URL_QUERY_CAP {
                return 0;
            }
            url_scopy(&mut o.query, &g, start, query_len);
        }
    }

    // fragment
    if g(p) == b'#' {
        p += 1;
        let frag_len = url_strlen_from(&g, p);
        if frag_len > 0 {
            if frag_len >= URL_FRAG_CAP {
                return 0;
            }
            url_scopy(&mut o.fragment, &g, p, frag_len);
        }
    }

    1
}
