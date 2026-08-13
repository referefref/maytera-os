// rustkern/http.rs - #404 Phase Y HTTP response length-parse seam (net/https.c, net/wget.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ============================================================================
// #404 Phase Y: HTTP client response length-parse seam (net/https.c + net/wget.c)
//
// The HTTP/1.1 response body framing is attacker/remote-influenced on EVERY
// fetch: the browser over plain http:// and https://, the Kimi/LLM API, the
// update check, and the netinfo/weather/crypto widget + music-metadata feeds.
// The classic HTTP-parser memory-safety surface is the chunked transfer-encoding
// chunk-size hex parse (a huge/overflowing hex size then used to gate a
// copy/read), the Content-Length parse, and the header-block CRLF framing.
//
// [RUST-SEC] REACHABLE (MAYTERA-SEC-2026-0008): the C `https_dechunk`
// (net/https.c) accumulates the chunk-size into a `uint32_t sz` from
// attacker-controlled hex, then clamps with `if (in + sz > len) sz = len - in;`.
// `in + sz` is uint32 arithmetic: with `sz` near 2^32 the sum WRAPS below `len`,
// the clamp is skipped, and `memmove(buf + out, buf + in, sz)` copies ~4 GiB out
// of a `kmalloc(body_len + 1)` heap body -> OOB read + OOB WRITE (CWE-190 ->
// CWE-787 / CWE-125). It is reachable behind the `https_chunked_is_complete`
// gate: the crafted body `"FFFFFFFE\r\n0\r\n\r\n"` makes that gate return 1
// (its own `p + sz > len` wraps p back to a clean 0-chunk terminator) so the
// dechunk runs. A malicious/compromised HTTPS origin the browser visits, or a
// MITM/compromised API/update endpoint, sends it. The Rust `https_dechunk_rs`
// below CONFINES it by construction (checked_add detects the wrap and takes the
// clamp). The C-fallback hardening of the same bound is task #504 (so a
// -DRUST_HTTP_PARSE rollback stays safe). A FOLD IS WARRANTED.
//
// The shared `http_decode_chunked` (net/wget.c, size_t, used by plain-HTTP GET
// and https_post) is already correctly bounded (`(end-read_ptr) < chunk_size`
// rejects before any copy; size_t needs 2^64 to wrap and that path returns -1),
// so it is LATENT/defense-in-depth. find_header_end and the Content-Length
// digit parse are likewise already bounded; ported here for defense-in-depth so
// the whole response-framing seam is memory-safe by construction.
//
// Only the byte-framing/length-bounding crosses into Rust. The socket recv, the
// body buffering, the header field extraction, and the redirect/SSRF gating all
// stay in C. Every seam takes a slice over exactly `len`, uses checked/wrapping
// arithmetic that MATCHES the C on well-formed input, and bounds every derived
// read/copy. Unsafe is documented at the FFI from_raw_parts + null-checked out
// writes only.
// ============================================================================

/// Rust port of net/https.c `https_dechunk`: decode HTTP/1.1 chunked
/// transfer-encoding IN PLACE, returning the decoded byte length. Each chunk is
/// `<hex-size>[;ext]CRLF <data> CRLF`, terminated by a 0-size chunk. Byte-for-
/// byte identical to the C on all well-formed inputs and on all inputs whose
/// `in + sz` does not overflow u32 (the C's clamp handles those the same way);
/// where the C's `in + sz` WRAPS (the crafted near-2^32 chunk size) the C skips
/// the clamp and over-copies ~4 GiB, while this seam detects the wrap with
/// checked_add and takes the same clamp the C intended -> no OOB. The hex
/// accumulation uses wrapping_* to reproduce the C's u32 wrap so the clamped
/// `sz` value matches the C bit-for-bit.
/// # Safety: `buf` must point to >= `len` readable+writable bytes; may be null.
#[no_mangle]
pub extern "C" fn https_dechunk_rs(buf: *mut u8, len: u32) -> u32 {
    if buf.is_null() {
        return 0;
    }
    let n = len as usize;
    // SAFETY: caller guarantees `buf` spans >= `len` readable+writable bytes; the
    // slice covers exactly `len`. Every index/copy below is bounds-checked to it.
    let b: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(buf, n) };
    let mut in_i: u32 = 0;
    let mut out: u32 = 0;
    while (in_i as usize) < n {
        let mut sz: u32 = 0;
        let mut any = false;
        while (in_i as usize) < n {
            let c = b[in_i as usize];
            let d = match c {
                b'0'..=b'9' => c - b'0',
                b'a'..=b'f' => c - b'a' + 10,
                b'A'..=b'F' => c - b'A' + 10,
                _ => break,
            };
            sz = sz.wrapping_mul(16).wrapping_add(d as u32); // match C u32 wrap
            any = true;
            in_i += 1;
        }
        // skip chunk-ext + to the LF ending the size line
        while (in_i as usize) < n && b[in_i as usize] != b'\n' {
            in_i += 1;
        }
        if (in_i as usize) < n {
            in_i += 1; // skip the LF
        }
        if !any || sz == 0 {
            break; // final (0) chunk, or ran out of size digits
        }
        // C: if (in + sz > len) sz = (in <= len) ? (len - in) : 0;  -- but u32
        // `in + sz` can WRAP. Treat overflow as "clamp to remaining" too, which
        // is the C's INTENDED behavior on the non-wrapping side.
        let over = match in_i.checked_add(sz) {
            Some(e) => e > len,
            None => true,
        };
        if over {
            sz = if in_i <= len { len - in_i } else { 0 };
        }
        if sz != 0 {
            let (i0, o0, s) = (in_i as usize, out as usize, sz as usize);
            // memmove semantics; both ranges are now within [0,n): after the
            // clamp i0+s <= len == n, and out <= in always so o0+s <= i0+s <= n.
            b.copy_within(i0..i0 + s, o0);
            out += sz;
            in_i += sz;
        }
        if (in_i as usize) < n && b[in_i as usize] == b'\r' {
            in_i += 1;
        }
        if (in_i as usize) < n && b[in_i as usize] == b'\n' {
            in_i += 1;
        }
    }
    out
}

// https_chunked_is_complete / http_decode_chunked return codes (host-order ints).
const HTTP_CHUNK_NEED_MORE: i32 = 0;
const HTTP_CHUNK_COMPLETE: i32 = 1;
const HTTP_CHUNK_BAD: i32 = -1;

/// Rust port of net/https.c `https_chunked_is_complete`: walk real chunk framing
/// over buf[0..len) and report 1 (terminating 0-chunk + trailer blank line have
/// arrived), 0 (need more data), or -1 (malformed). Byte-for-byte identical to
/// the C on well-formed input; where the C's `p + sz > len` would WRAP (the same
/// crafted near-2^32 size that drives the https_dechunk OOB), this seam returns
/// NEED_MORE via checked_add instead of following the wrap, so the malicious
/// frame is rejected at the gate rather than reaching the decoder. That is a
/// deliberate divergence ONLY on the overflow vector (captured in [RUST-SEC]).
/// # Safety: `buf` must point to >= `len` readable bytes; may be null.
#[no_mangle]
pub extern "C" fn https_chunk_complete_rs(buf: *const u8, len: u32) -> i32 {
    if buf.is_null() {
        return HTTP_CHUNK_BAD;
    }
    let n = len as usize;
    // SAFETY: caller guarantees `buf` spans >= `len` readable bytes; slice covers
    // exactly `len`; all indexing below is bounds-checked against it.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, n) };
    let mut p: u32 = 0;
    loop {
        let mut sz: u32 = 0;
        let mut any = false;
        while (p as usize) < n {
            let c = b[p as usize];
            let d = match c {
                b'0'..=b'9' => c - b'0',
                b'a'..=b'f' => c - b'a' + 10,
                b'A'..=b'F' => c - b'A' + 10,
                _ => break,
            };
            sz = sz.wrapping_mul(16).wrapping_add(d as u32);
            any = true;
            p += 1;
        }
        if !any {
            return if (p as usize) >= n { HTTP_CHUNK_NEED_MORE } else { HTTP_CHUNK_BAD };
        }
        while (p as usize) < n && b[p as usize] != b'\n' {
            p += 1;
        }
        if (p as usize) >= n {
            return HTTP_CHUNK_NEED_MORE; // size line not finished
        }
        p += 1; // past the LF

        if sz == 0 {
            // Trailer: zero or more header lines, ended by a blank line.
            loop {
                if (p as usize) >= n {
                    return HTTP_CHUNK_NEED_MORE;
                }
                if b[p as usize] == b'\n' {
                    return HTTP_CHUNK_COMPLETE; // bare-LF blank line
                }
                if b[p as usize] == b'\r' {
                    if (p as usize) + 1 >= n {
                        return HTTP_CHUNK_NEED_MORE;
                    }
                    if b[p as usize + 1] == b'\n' {
                        return HTTP_CHUNK_COMPLETE; // CRLF blank line
                    }
                }
                while (p as usize) < n && b[p as usize] != b'\n' {
                    p += 1;
                }
                if (p as usize) >= n {
                    return HTTP_CHUNK_NEED_MORE;
                }
                p += 1;
            }
        }

        // Payload must fit: p + sz <= len, checked so a near-u32::MAX sz can
        // never wrap to appear in-bounds (the C's `p + sz > len` would).
        let after = match p.checked_add(sz) {
            Some(e) if e <= len => e,
            _ => return HTTP_CHUNK_NEED_MORE,
        };
        p = after;
        if (p as usize) >= n {
            return HTTP_CHUNK_NEED_MORE; // need the trailing CRLF
        }
        if b[p as usize] == b'\r' {
            p += 1;
        }
        if (p as usize) >= n {
            return HTTP_CHUNK_NEED_MORE;
        }
        if b[p as usize] == b'\n' {
            p += 1;
        } else {
            return HTTP_CHUNK_BAD;
        }
    }
}

/// Rust port of net/wget.c `http_decode_chunked` (size_t): decode chunked
/// transfer-encoding in place, returning the decoded length or -1 (incomplete/
/// malformed). Used by plain-HTTP GET and by https_post. The C is already
/// correctly bounded (LATENT/defense-in-depth); this removes the class by
/// construction and is byte-for-byte identical on every input (a huge size_t
/// chunk size fails `(len - read) < size` and returns -1 before any copy).
/// # Safety: `buf` must point to >= `len` readable+writable bytes; may be null.
#[no_mangle]
pub extern "C" fn http_decode_chunked_rs(buf: *mut u8, len: usize) -> isize {
    if buf.is_null() {
        return -1;
    }
    // SAFETY: caller guarantees `buf` spans >= `len` readable+writable bytes;
    // slice covers exactly `len`; all indexing/copies bounds-checked to it.
    let b: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(buf, len) };
    let mut read_i: usize = 0;
    let mut write_i: usize = 0;
    let mut total: usize = 0;
    while read_i < len {
        let mut chunk_size: usize = 0;
        let mut any = false;
        let mut p = read_i;
        while p < len {
            let c = b[p];
            let d = match c {
                b'0'..=b'9' => c - b'0',
                b'a'..=b'f' => c - b'a' + 10,
                b'A'..=b'F' => c - b'A' + 10,
                _ => break,
            };
            chunk_size = chunk_size.wrapping_mul(16).wrapping_add(d as usize);
            any = true;
            p += 1;
        }
        if !any {
            return -1; // no size digits: incomplete or malformed
        }
        while p < len && b[p] != b'\n' {
            p += 1;
        }
        if p >= len {
            return -1; // size line not fully received
        }
        p += 1; // skip LF
        read_i = p;
        if chunk_size == 0 {
            return total as isize; // genuine terminating chunk
        }
        if len - read_i < chunk_size {
            return -1; // chunk payload not fully received
        }
        if write_i != read_i {
            b.copy_within(read_i..read_i + chunk_size, write_i);
        }
        write_i += chunk_size;
        read_i += chunk_size;
        total += chunk_size;
        if read_i < len && b[read_i] == b'\r' {
            read_i += 1;
        }
        if read_i < len && b[read_i] == b'\n' {
            read_i += 1;
        }
    }
    -1 // ran out before the terminating 0-size chunk
}

/// Rust port of the identical `find_header_end` (net/wget.c) / `https_find_header_end`
/// (net/https.c): scan for the CRLFCRLF that ends the HTTP header block, returning
/// the byte offset just past it, or -1. The scan is bounded to buf[0..len)
/// (`i + 3 < len`). Byte-for-byte identical to the C. Defense-in-depth: the C is
/// already bounded, but a length-not-NUL scan is exactly the classic
/// "CRLF search runs past the buffer" bug, removed here by construction.
/// # Safety: `buf` must point to >= `len` readable bytes; may be null.
#[no_mangle]
pub extern "C" fn http_find_header_end_rs(buf: *const u8, len: u32) -> i32 {
    if buf.is_null() {
        return -1;
    }
    let n = len as usize;
    // SAFETY: caller guarantees `buf` spans >= `len` readable bytes; slice covers
    // exactly `len`; all indexing bounds-checked against it.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, n) };
    let mut i: usize = 0;
    while i + 3 < n {
        if b[i] == b'\r' && b[i + 1] == b'\n' && b[i + 2] == b'\r' && b[i + 3] == b'\n' {
            return (i + 4) as i32;
        }
        i += 1;
    }
    -1
}

/// Rust port of net/https.c `https_content_length`: scan the header region
/// hdr[0..len) case-insensitively for "content-length:", then parse the decimal
/// value (0 = absent/unknown). Returns the u32 value. Byte-for-byte identical to
/// the C, including the C's u32 wrap on the digit accumulation (wrapping_*), so a
/// differential over any value agrees bit-for-bit. Bounded to `len` (the C
/// relied on the header region's NUL terminator; this removes that reliance).
/// Content-Length overflow is not itself an OOB (the receive/copy that consumes
/// it is separately bounded), so this is LATENT/defense-in-depth.
/// # Safety: `hdr` must point to >= `len` readable bytes; may be null.
#[no_mangle]
pub extern "C" fn https_content_length_rs(hdr: *const u8, len: u32) -> u32 {
    if hdr.is_null() {
        return 0;
    }
    let n = len as usize;
    // SAFETY: caller guarantees `hdr` spans >= `len` readable bytes; slice covers
    // exactly `len`; all indexing bounds-checked against it.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(hdr, n) };
    let needle: &[u8] = b"content-length:";
    let nl = needle.len();
    let mut i: usize = 0;
    while i < n {
        // case-insensitive compare of needle at position i, bounded to n
        let mut k = 0usize;
        while k < nl && i + k < n {
            let mut ca = b[i + k];
            if ca >= b'A' && ca <= b'Z' {
                ca += 32;
            }
            if ca != needle[k] {
                break;
            }
            k += 1;
        }
        if k == nl {
            // matched: skip spaces/tabs, parse digits
            let mut a = i + nl;
            while a < n && (b[a] == b' ' || b[a] == b'\t') {
                a += 1;
            }
            let mut v: u32 = 0;
            let mut any = false;
            while a < n && b[a] >= b'0' && b[a] <= b'9' {
                v = v.wrapping_mul(10).wrapping_add((b[a] - b'0') as u32);
                a += 1;
                any = true;
            }
            return if any { v } else { 0 };
        }
        i += 1;
    }
    0
}

/// Rust port of net/wget.c `parse_uint` used for the Content-Length value: parse
/// leading decimal digits from s[0..len), bounded to `len` (the C stopped at the
/// buffer's NUL terminator). Returns the value with the C's size_t wrap
/// reproduced via wrapping_*. LATENT/defense-in-depth (the value only bounds a
/// separately-bounded receive loop).
/// # Safety: `s` must point to >= `len` readable bytes; may be null.
#[no_mangle]
pub extern "C" fn http_parse_uint_rs(s: *const u8, len: usize) -> usize {
    if s.is_null() {
        return 0;
    }
    // SAFETY: caller guarantees `s` spans >= `len` readable bytes; slice covers
    // exactly `len`; indexing bounds-checked.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(s, len) };
    let mut val: usize = 0;
    let mut i = 0usize;
    while i < len && b[i] >= b'0' && b[i] <= b'9' {
        val = val.wrapping_mul(10).wrapping_add((b[i] - b'0') as usize);
        i += 1;
    }
    val
}

// ============================================================================
// #608 (NEW kernel logic, written in Rust per the Rust-first mandate): HTTP
// byte-range response framing, for the App Store's chunked package download.
//
// The App Store downloads a >100MB package as a sequence of 256KB `Range:`
// requests streamed to disk and hashed incrementally (#570). Nothing in the
// kernel ever checked that a 206 Partial Content response actually carries the
// range that was asked for. A server (or a caching proxy, or a MITM on plain
// http) that answers a DIFFERENT range with 206 makes the client write the
// wrong bytes at the wrong file offset. The signed-manifest sha256 still
// catches that and refuses the install (fail-closed, unchanged), but only
// after ~100MB has been transferred, and it is reported as a hash mismatch
// rather than as the framing error it is.
//
// These parse REMOTE, attacker-influenced header bytes, which is exactly the
// Tier-2 untrusted-wire-input category the ledger puts in Rust. Both are pure,
// bounded to the caller's slice, allocation-free, and take no locks. NEW code,
// so there is no `_c` reference twin to keep and no -DRUST_* strangler flag:
// the strangler pattern applies where Rust REPLACES existing C.
//
// FAIL-OPEN BY DESIGN on anything unparseable. http_range_check_rs returns
// ACCEPT unless BOTH sides parsed AND provably disagree, and a server that
// returns a SHORTER range than requested is accepted (RFC 7233 lets a server
// satisfy less than was asked; the client loop advances by the bytes it got).
// Only a wrong START offset or an OVERSHOOT past the requested end is
// rejected. A new check that starts refusing responses no previous build
// refused is itself a regression risk; this one can only reject a definite,
// provable mismatch.
// ============================================================================

/// Case-insensitively locate a header whose name is `name` (which must include
/// the trailing ':') at the START OF A LINE within `b`, returning the offset of
/// the first byte after the colon. Anchoring to a line start is what keeps
/// "Access-Control-Expose-Headers: Content-Range" from being mistaken for the
/// Content-Range header itself.
fn hdr_line_value(b: &[u8], name: &[u8]) -> Option<usize> {
    let n = b.len();
    let nl = name.len();
    if nl == 0 || n < nl {
        return None;
    }
    let mut i: usize = 0;
    while i + nl <= n {
        let at_line_start = i == 0 || b[i - 1] == b'\n';
        if at_line_start {
            let mut k = 0usize;
            while k < nl {
                let mut ca = b[i + k];
                if ca.is_ascii_uppercase() {
                    ca += 32;
                }
                let mut cb = name[k];
                if cb.is_ascii_uppercase() {
                    cb += 32;
                }
                if ca != cb {
                    break;
                }
                k += 1;
            }
            if k == nl {
                return Some(i + nl);
            }
        }
        i += 1;
    }
    None
}

/// Parse decimal digits at b[i..], returning (value, next_index, saw_digit).
/// Saturating so a pathological digit run can never wrap into a small value.
fn parse_u64(b: &[u8], mut i: usize) -> (u64, usize, bool) {
    let n = b.len();
    let mut v: u64 = 0;
    let mut any = false;
    while i < n && b[i].is_ascii_digit() {
        v = v.saturating_mul(10).saturating_add((b[i] - b'0') as u64);
        i += 1;
        any = true;
    }
    (v, i, any)
}

fn skip_ws(b: &[u8], mut i: usize) -> usize {
    while i < b.len() && (b[i] == b' ' || b[i] == b'\t') {
        i += 1;
    }
    i
}

/// Parse the REQUEST header block for a single `Range: bytes=FIRST-LAST`.
/// Returns 1 and writes *first/*last on success; 0 otherwise (absent header,
/// multi-range, suffix range "-N", or open-ended "N-", none of which this
/// kernel ever emits and none of which can be checked against a response).
/// # Safety: `hdr` must point to >= `len` readable bytes; may be null.
#[no_mangle]
pub extern "C" fn http_range_req_parse_rs(hdr: *const u8, len: u32,
                                          first: *mut u64, last: *mut u64) -> i32 {
    if hdr.is_null() {
        return 0;
    }
    let n = len as usize;
    // SAFETY: caller guarantees `hdr` spans >= `len` readable bytes; the slice
    // covers exactly `len` and every index below is bounds-checked against it.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(hdr, n) };
    let Some(v) = hdr_line_value(b, b"range:") else { return 0; };
    let i = skip_ws(b, v);
    // require the "bytes=" unit
    let unit = b"bytes=";
    if i + unit.len() > n {
        return 0;
    }
    let mut k = 0usize;
    while k < unit.len() {
        let mut ca = b[i + k];
        if ca.is_ascii_uppercase() {
            ca += 32;
        }
        if ca != unit[k] {
            return 0;
        }
        k += 1;
    }
    let (f, i2, any_f) = parse_u64(b, i + unit.len());
    if !any_f || i2 >= n || b[i2] != b'-' {
        return 0;
    }
    let (l, i3, any_l) = parse_u64(b, i2 + 1);
    if !any_l || l < f {
        return 0;
    }
    // A comma here means a multi-range request: not checkable, decline.
    if i3 < n && b[i3] == b',' {
        return 0;
    }
    // SAFETY: out pointers are null-checked before each write.
    unsafe {
        if !first.is_null() {
            *first = f;
        }
        if !last.is_null() {
            *last = l;
        }
    }
    1
}

/// Parse a RESPONSE header block for `Content-Range: bytes FIRST-LAST/TOTAL`.
/// Returns 1 and writes *first/*last/*total on success; 0 otherwise. An
/// unknown total ("*") is reported as u64::MAX. An unsatisfied-range form
/// ("bytes */NNN") has no first/last and is declined (0).
/// # Safety: `hdr` must point to >= `len` readable bytes; may be null.
#[no_mangle]
pub extern "C" fn http_content_range_parse_rs(hdr: *const u8, len: u32, first: *mut u64,
                                              last: *mut u64, total: *mut u64) -> i32 {
    if hdr.is_null() {
        return 0;
    }
    let n = len as usize;
    // SAFETY: caller guarantees `hdr` spans >= `len` readable bytes; the slice
    // covers exactly `len` and every index below is bounds-checked against it.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(hdr, n) };
    let Some(v) = hdr_line_value(b, b"content-range:") else { return 0; };
    let mut i = skip_ws(b, v);
    // optional "bytes" unit followed by whitespace
    let unit = b"bytes";
    if i + unit.len() <= n {
        let mut k = 0usize;
        while k < unit.len() {
            let mut ca = b[i + k];
            if ca.is_ascii_uppercase() {
                ca += 32;
            }
            if ca != unit[k] {
                break;
            }
            k += 1;
        }
        if k == unit.len() {
            i = skip_ws(b, i + unit.len());
        }
    }
    let (f, i2, any_f) = parse_u64(b, i);
    if !any_f || i2 >= n || b[i2] != b'-' {
        return 0;
    }
    let (l, i3, any_l) = parse_u64(b, i2 + 1);
    if !any_l || l < f {
        return 0;
    }
    let mut t: u64 = u64::MAX;
    if i3 < n && b[i3] == b'/' {
        if i3 + 1 < n && b[i3 + 1] == b'*' {
            t = u64::MAX;
        } else {
            let (tv, _, any_t) = parse_u64(b, i3 + 1);
            if any_t {
                t = tv;
            }
        }
    }
    // SAFETY: out pointers are null-checked before each write.
    unsafe {
        if !first.is_null() {
            *first = f;
        }
        if !last.is_null() {
            *last = l;
        }
        if !total.is_null() {
            *total = t;
        }
    }
    1
}

/// Gate a 206 Partial Content response against the Range that was requested.
/// Returns 1 = ACCEPT, 0 = REJECT. Rejects only a provable mismatch: a wrong
/// start offset, or a last byte past the end that was asked for. Everything
/// else (either header absent or unparseable, a shorter-than-requested range)
/// is accepted so this can never turn a previously working fetch into a
/// failure.
/// # Safety: both pointers must span their stated lengths; either may be null.
#[no_mangle]
pub extern "C" fn http_range_check_rs(req_hdr: *const u8, req_len: u32,
                                      resp_hdr: *const u8, resp_len: u32) -> i32 {
    let mut qf: u64 = 0;
    let mut ql: u64 = 0;
    if http_range_req_parse_rs(req_hdr, req_len, &mut qf, &mut ql) != 1 {
        return 1;
    }
    let mut rf: u64 = 0;
    let mut rl: u64 = 0;
    let mut rt: u64 = 0;
    if http_content_range_parse_rs(resp_hdr, resp_len, &mut rf, &mut rl, &mut rt) != 1 {
        return 1;
    }
    if rf != qf {
        return 0;
    }
    if rl > ql {
        return 0;
    }
    1
}
