// rustkern/tlspool.rs - #616 TLS connection reuse for the HTTPS client.
//
// WHY THIS EXISTS
// ---------------
// net/https.c opened a FRESH TCP connection AND a FRESH TLS handshake for every
// single request. #615 measured a 103 MB App Store package download as 396
// Range GETs at ~550 ms each, of which 414 ms (75%) was the TLS handshake. That
// is ~2.7 minutes of pure handshake on one download.
//
// The fix is the same shape as the plain-HTTP keep-alive pool that already
// exists in net/wget.c (http_get_connection_ex / http_drop_pooled): keep the
// live, already-verified connection and send the next request on it.
//
// WHY RUST (kernel policy: new kernel code is Rust unless there is a stated
// performance reason)
// ---------------------------------------------------------------------------
// Two separable pieces of NEW logic live here, both pure state + buffer work:
//
//   1. The pool itself. It copies an attacker-influenced host string (the URL
//      host) into fixed-size slots and compares it on every lookup. The obvious
//      C is strncpy + strcmp over a char[128] and is one off-by-one away from
//      an unterminated compare or an overflow. Here the host is a [u8; 128]
//      with an explicit length and every access is a bounds-checked slice, so
//      "the name in the slot is exactly the name that was stored" is a property
//      of the type rather than of remembering the NUL.
//
//   2. http_reuse_ok_rs: the decision "may this connection carry another
//      request?", taken by PARSING THE PEER'S RESPONSE HEADERS. That is
//      untrusted input, and getting it wrong is not a performance bug: if we
//      keep a connection the server considered closed, the NEXT request's bytes
//      land on a dead or (worse) reassigned socket.
//
// The TLS/TCP calls themselves (tls_create, tcp_connect, the handshake) stay in
// C: they are the existing net stack, not new logic, and moving them is a port,
// not this task.
//
// SECURITY NOTE (explicit, #232/#510)
// -----------------------------------
// This implements LIVE-CONNECTION reuse only. There is NO session ticket, NO
// session-ID resumption, and NO abbreviated handshake. A pooled connection is
// one whose full handshake ALREADY completed with certificate-chain, validity
// window and hostname verification (tls_set_verify(tls, 1, 0) in
// https_connect). Reuse therefore inherits a peer identity that was actually
// verified, because it IS the same connection. A cache hit requires an EXACT
// (host bytes, host length, port, alpn-mode) match, so a connection opened to
// one host can never be handed to a request for another.
//
// Routed live under -DRUST_TLS_POOL. Boot self-test: tlspool_rust_selftest()
// in net/https.c ([RUST-DIFF] tlspool for the header policy against the C twin
// http_reuse_ok_c, plus pool invariant assertions - a differential against a
// twin that does not exist would prove nothing).

// Mirrors https_pool_slot_t in net/https.c. sizeof-locked on both sides.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TlsPoolSlot {
    /// Opaque https_conn_t*. 0 == slot free.
    pub conn: u64,
    pub last_used_ms: u64,
    pub port: u16,
    /// bit0: the connection was opened with the ALPN forced to http/1.1.
    pub flags: u16,
    pub hostlen: u16,
    pub _pad: u16,
    pub host: [u8; 128],
}
const _: () = assert!(core::mem::size_of::<TlsPoolSlot>() == 152);

const HOST_MAX: usize = 128;

#[inline]
unsafe fn slots<'a>(p: *mut TlsPoolSlot, n: u32) -> Option<&'a mut [TlsPoolSlot]> {
    if p.is_null() || n == 0 || n > 64 {
        return None;
    }
    Some(core::slice::from_raw_parts_mut(p, n as usize))
}

#[inline]
unsafe fn hostbytes<'a>(h: *const u8, len: u32) -> Option<&'a [u8]> {
    if h.is_null() || len == 0 || len as usize > HOST_MAX {
        return None;
    }
    Some(core::slice::from_raw_parts(h, len as usize))
}

#[inline]
fn slot_matches(s: &TlsPoolSlot, host: &[u8], port: u16, flags: u16) -> bool {
    s.conn != 0
        && s.port == port
        && s.flags == flags
        && s.hostlen as usize == host.len()
        && s.host[..host.len()] == *host
}

#[inline]
fn slot_clear(s: &mut TlsPoolSlot) {
    s.conn = 0;
    s.last_used_ms = 0;
    s.port = 0;
    s.flags = 0;
    s.hostlen = 0;
    s.host = [0u8; HOST_MAX];
}

/// Take a live connection for host:port out of the pool.
///
/// Returns 1 and writes the connection pointer to `out_conn` on a hit; the slot
/// is CLEARED, so a connection can only ever be checked out by one caller
/// (exclusivity is structural, not a convention). Returns 0 on a miss. An entry
/// that has been idle longer than `idle_ms` is not handed out; it is left in
/// place for tlspool_reap_rs so the caller can close it properly.
#[no_mangle]
pub extern "C" fn tlspool_acquire_rs(
    p: *mut TlsPoolSlot,
    n: u32,
    host: *const u8,
    hostlen: u32,
    port: u16,
    flags: u16,
    now_ms: u64,
    idle_ms: u64,
    out_conn: *mut u64,
) -> i32 {
    if out_conn.is_null() {
        return 0;
    }
    unsafe {
        *out_conn = 0;
    }
    // SAFETY: caller (net/https.c) passes its own static array of >= n slots and
    // a host buffer of >= hostlen bytes.
    let sl = match unsafe { slots(p, n) } {
        Some(s) => s,
        None => return 0,
    };
    let hb = match unsafe { hostbytes(host, hostlen) } {
        Some(h) => h,
        None => return 0,
    };
    for s in sl.iter_mut() {
        if !slot_matches(s, hb, port, flags) {
            continue;
        }
        if now_ms.saturating_sub(s.last_used_ms) > idle_ms {
            continue; // stale: leave for the reaper, which closes it
        }
        let c = s.conn;
        slot_clear(s);
        unsafe {
            *out_conn = c;
        }
        return 1;
    }
    0
}

/// Remove ONE entry that has been idle longer than `idle_ms`.
///
/// Returns 1 and writes its connection pointer to `out_conn` (the caller must
/// close it), 0 when nothing is expired. Call in a loop until it returns 0.
#[no_mangle]
pub extern "C" fn tlspool_reap_rs(
    p: *mut TlsPoolSlot,
    n: u32,
    now_ms: u64,
    idle_ms: u64,
    out_conn: *mut u64,
) -> i32 {
    if out_conn.is_null() {
        return 0;
    }
    unsafe {
        *out_conn = 0;
    }
    let sl = match unsafe { slots(p, n) } {
        Some(s) => s,
        None => return 0,
    };
    for s in sl.iter_mut() {
        if s.conn != 0 && now_ms.saturating_sub(s.last_used_ms) > idle_ms {
            let c = s.conn;
            slot_clear(s);
            unsafe {
                *out_conn = c;
            }
            return 1;
        }
    }
    0
}

/// Put a live connection back into the pool.
///
/// Returns 1 when the connection is now owned by the pool. If a slot had to be
/// freed to make room, the evicted connection is written to `out_evicted` and
/// the caller must close IT (never `conn`). Returns 0 when the connection could
/// NOT be stored (null/zero pointer, host too long, bad arguments); in that
/// case `out_evicted` is left 0 and the caller must close `conn` itself. The
/// two outcomes are disjoint, so no path can double-close.
#[no_mangle]
pub extern "C" fn tlspool_store_rs(
    p: *mut TlsPoolSlot,
    n: u32,
    host: *const u8,
    hostlen: u32,
    port: u16,
    flags: u16,
    conn: u64,
    now_ms: u64,
    out_evicted: *mut u64,
) -> i32 {
    if out_evicted.is_null() {
        return 0;
    }
    unsafe {
        *out_evicted = 0;
    }
    if conn == 0 {
        return 0;
    }
    let sl = match unsafe { slots(p, n) } {
        Some(s) => s,
        None => return 0,
    };
    let hb = match unsafe { hostbytes(host, hostlen) } {
        Some(h) => h,
        None => return 0,
    };

    // A connection pointer must never appear twice in the pool.
    for s in sl.iter_mut() {
        if s.conn == conn {
            slot_clear(s);
        }
    }

    // Prefer a free slot; otherwise evict the least recently used one.
    let mut idx: Option<usize> = None;
    for (i, s) in sl.iter().enumerate() {
        if s.conn == 0 {
            idx = Some(i);
            break;
        }
    }
    let i = match idx {
        Some(i) => i,
        None => {
            let mut lru = 0usize;
            for (i, s) in sl.iter().enumerate() {
                if s.last_used_ms < sl[lru].last_used_ms {
                    lru = i;
                }
            }
            let victim = sl[lru].conn;
            slot_clear(&mut sl[lru]);
            unsafe {
                *out_evicted = victim;
            }
            lru
        }
    };

    let s = &mut sl[i];
    s.conn = conn;
    s.last_used_ms = now_ms;
    s.port = port;
    s.flags = flags;
    s.hostlen = hb.len() as u16;
    s.host = [0u8; HOST_MAX];
    s.host[..hb.len()].copy_from_slice(hb);
    1
}

/// Forget a connection the caller is about to close itself. Returns 1 if it was
/// in the pool.
#[no_mangle]
pub extern "C" fn tlspool_drop_rs(p: *mut TlsPoolSlot, n: u32, conn: u64) -> i32 {
    if conn == 0 {
        return 0;
    }
    let sl = match unsafe { slots(p, n) } {
        Some(s) => s,
        None => return 0,
    };
    let mut hit = 0;
    for s in sl.iter_mut() {
        if s.conn == conn {
            slot_clear(s);
            hit = 1;
        }
    }
    hit
}

/// How many slots currently hold a connection (self-test / diagnostics).
#[no_mangle]
pub extern "C" fn tlspool_count_rs(p: *mut TlsPoolSlot, n: u32) -> i32 {
    let sl = match unsafe { slots(p, n) } {
        Some(s) => s,
        None => return -1,
    };
    let mut c = 0;
    for s in sl.iter() {
        if s.conn != 0 {
            c += 1;
        }
    }
    c
}

// ---------------------------------------------------------------------------
// Response-header policy: may this connection carry another request?
// ---------------------------------------------------------------------------
// Input is the raw response header block straight off the wire. Length-driven
// throughout: no NUL is assumed, no index is unchecked, and a header block with
// no terminator simply runs out of bytes.
//
// Rules (RFC 7230 6.1/6.3):
//   HTTP/1.1  -> persistent by default; any `Connection:` value containing the
//                token `close` forbids reuse.
//   HTTP/1.0  -> NOT persistent by default; requires an explicit
//                `Connection: keep-alive`.
//   anything else (HTTP/0.9, HTTP/2 preface, garbage) -> refuse.
// `Proxy-Connection: close` is honoured as well.
//
// Fail-CLOSED: anything unparseable returns 0 (open a fresh connection). The
// cost of a false negative is one extra handshake; the cost of a false positive
// is writing a request onto a connection the peer has already torn down.

#[inline]
fn lower(b: u8) -> u8 {
    if b.is_ascii_uppercase() {
        b + 32
    } else {
        b
    }
}

fn eq_ci(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for i in 0..a.len() {
        if lower(a[i]) != lower(b[i]) {
            return false;
        }
    }
    true
}

/// Does the comma/whitespace separated field value contain `tok` as a token?
fn has_token(val: &[u8], tok: &[u8]) -> bool {
    let mut i = 0usize;
    while i < val.len() {
        // skip separators
        while i < val.len() && (val[i] == b',' || val[i] == b' ' || val[i] == b'\t') {
            i += 1;
        }
        let start = i;
        while i < val.len() && val[i] != b',' && val[i] != b' ' && val[i] != b'\t' {
            i += 1;
        }
        if i > start && eq_ci(&val[start..i], tok) {
            return true;
        }
    }
    false
}

#[no_mangle]
pub extern "C" fn http_reuse_ok_rs(hdr: *const u8, len: u32) -> i32 {
    if hdr.is_null() || len == 0 || len > (1u32 << 20) {
        return 0;
    }
    // SAFETY: caller passes the header region of its own receive buffer, of at
    // least `len` bytes. Only read, only through this slice.
    let h: &[u8] = unsafe { core::slice::from_raw_parts(hdr, len as usize) };

    // --- status line: version decides the default ---
    let mut eol = 0usize;
    while eol < h.len() && h[eol] != b'\n' {
        eol += 1;
    }
    let mut line = &h[..eol];
    if let Some(&b'\r') = line.last() {
        line = &line[..line.len() - 1];
    }
    let http11: bool;
    if line.len() >= 8 && eq_ci(&line[..8], b"HTTP/1.1") {
        http11 = true;
    } else if line.len() >= 8 && eq_ci(&line[..8], b"HTTP/1.0") {
        http11 = false;
    } else {
        return 0;
    }

    let mut persistent = http11;
    let mut pos = if eol < h.len() { eol + 1 } else { h.len() };

    while pos < h.len() {
        let mut e = pos;
        while e < h.len() && h[e] != b'\n' {
            e += 1;
        }
        let mut l = &h[pos..e];
        if let Some(&b'\r') = l.last() {
            l = &l[..l.len() - 1];
        }
        pos = if e < h.len() { e + 1 } else { h.len() };
        if l.is_empty() {
            break; // end of header block
        }
        // split name: value
        let mut c = 0usize;
        while c < l.len() && l[c] != b':' {
            c += 1;
        }
        if c >= l.len() {
            continue; // no colon: malformed line, ignore
        }
        let name = &l[..c];
        let mut val = &l[c + 1..];
        while !val.is_empty() && (val[0] == b' ' || val[0] == b'\t') {
            val = &val[1..];
        }
        if eq_ci(name, b"connection") || eq_ci(name, b"proxy-connection") {
            if has_token(val, b"close") {
                persistent = false;
            } else if has_token(val, b"keep-alive") {
                persistent = true;
            }
        }
    }

    if persistent {
        1
    } else {
        0
    }
}
