// alpaca.rs - ASCOM Alpaca telescope REST client + control panel back end.
//
// SLICE B was a bare keyboard control (connect ip:port, poll RA/Dec, key
// slew/sync). THIS is the full telescope-control panel back end (#809,
// alpacaui): a bounded async op QUEUE over the same single-in-flight state
// machine, the full Alpaca command surface (goto/sync/abort, moveaxis jog,
// tracking on-off + rate, park/unpark/findhome), live status (RA/Dec/Alt/Az,
// slewing, tracking), CanX capability gating, and best-effort UDP broadcast
// discovery. The panel RENDER lives in lib.rs (panel_render); this file is the
// protocol + state.
//
// WHY A HAND-ROLLED HTTP CLIENT (unchanged from Slice B): the userland
// SYS_HTTP_* facility only issues GET and POST, but every Alpaca telescope
// command is a PUT. Rather than extend the kernel HTTP surface for one app,
// this builds the HTTP/1.1 requests it needs directly over the raw TCP
// syscalls every networked userland app uses (SYS_SOCKET/CONNECT/SEND/RECV/
// TCP_CLOSE/TCP_STATE - see userland/apps/nc/main.c).
//
// WHY A STATE MACHINE + QUEUE, NOT A BLOCKING CALL: the sky render must NEVER
// block on the network ([[no-block-context-cannot-wait-event-async-tx]]).
// alpaca_step() advances at most one non-blocking syscall's worth of work per
// frame and is driven from the same loop that calls render(). Multiple button
// presses do not block waiting for the previous command: they are pushed onto
// a bounded ring QUEUE and drained one at a time by alpaca_pump().
//
// ALPACA GOTCHA (record for the next person): the Telescope RightAscension
// property is in DECIMAL HOURS (0..24), not degrees, even though Declination
// is in degrees. ra_deg/15.0 on the way out and *15.0 on the way back.

const SYS_SOCKET: i64 = 60;
const SYS_CONNECT: i64 = 61;
const SYS_SEND: i64 = 62;
const SYS_RECV: i64 = 63;
const SYS_TCP_CLOSE: i64 = 64;
const SYS_TCP_STATE: i64 = 65;
const TCP_STATE_ESTABLISHED: i32 = 4;
const TCP_STATE_CLOSED: i32 = 0;

// BSD-socket syscalls (kernel net/socket.c) used ONLY for UDP discovery. The
// TCP command path above deliberately keeps the legacy raw-TCB helpers.
const SYS_SOCK_OPEN: i64 = 343;
const SYS_SOCK_SENDTO: i64 = 350;
const SYS_SOCK_RECVFROM: i64 = 351;
const SYS_SOCK_SETOPT: i64 = 352;
const SYS_CLOSE_FD: i64 = 11;
const SYS_OPEN_FD: i64 = 10;
const SYS_READ_FD: i64 = 12;
const AF_INET: i64 = 2;
const SOCK_DGRAM: i64 = 2;
const SOL_SOCKET: i64 = 1;
const SO_BROADCAST: i64 = 6;
const MSG_DONTWAIT: i64 = 0x40;
const ALPACA_DISCOVERY_PORT: u16 = 32227;

fn tcp_socket() -> i32 { unsafe { syscall1(SYS_SOCKET, 0) as i32 } }
fn tcp_connect(sock: i32, ip: u32, port: i32) -> i32 { unsafe { syscall3(SYS_CONNECT, sock as i64, ip as i64, port as i64) as i32 } }
fn tcp_send(sock: i32, buf: &[u8]) -> i32 { unsafe { syscall3(SYS_SEND, sock as i64, buf.as_ptr() as i64, buf.len() as i64) as i32 } }
fn tcp_recv(sock: i32, buf: &mut [u8]) -> i32 { unsafe { syscall3(SYS_RECV, sock as i64, buf.as_mut_ptr() as i64, buf.len() as i64) as i32 } }
fn tcp_close(sock: i32) { unsafe { syscall1(SYS_TCP_CLOSE, sock as i64); } }
fn tcp_state(sock: i32) -> i32 { unsafe { syscall1(SYS_TCP_STATE, sock as i64) as i32 } }

fn cat(buf: &mut [u8], mut n: usize, s: &[u8]) -> usize { for &c in s { if n < buf.len() { buf[n] = c; n += 1; } } n }
fn put_ip(buf: &mut [u8], mut n: usize, ip: u32) -> usize {
    n = put_i(buf, n, ((ip >> 24) & 0xFF) as i64); buf[n] = b'.'; n += 1;
    n = put_i(buf, n, ((ip >> 16) & 0xFF) as i64); buf[n] = b'.'; n += 1;
    n = put_i(buf, n, ((ip >> 8) & 0xFF) as i64); buf[n] = b'.'; n += 1;
    put_i(buf, n, (ip & 0xFF) as i64)
}
// Fixed 6-decimal formatter (put_f in lib.rs only keeps 2, too coarse for RA/Dec).
fn put_f6(buf: &mut [u8], mut n: usize, v: f64) -> usize {
    let neg = v < 0.0;
    let a = if neg { -v } else { v };
    let scaled = (a * 1_000_000.0 + 0.5) as i64;
    let ip = scaled / 1_000_000;
    let fp = scaled % 1_000_000;
    if neg { buf[n] = b'-'; n += 1; }
    n = put_i(buf, n, ip);
    buf[n] = b'.'; n += 1;
    let mut div: i64 = 100_000;
    while div >= 1 { buf[n] = b'0' + ((fp / div) % 10) as u8; n += 1; div /= 10; }
    n
}

// Parse "a.b.c.d:port". Same packed-u32 IP layout as nc/main.c's parse_ip
// (first octet in the MSB) - what SYS_CONNECT expects.
fn parse_ipport(s: &[u8], slen: usize) -> Option<(u32, i32)> {
    let mut ip: u32 = 0; let mut octet: u32 = 0; let mut oct_cnt = 0; let mut have = false;
    let mut i = 0;
    while i < slen && s[i] != b':' {
        let c = s[i];
        if c.is_ascii_digit() { octet = octet * 10 + (c - b'0') as u32; have = true; if octet > 255 { return None; } }
        else if c == b'.' { if !have { return None; } ip = (ip << 8) | octet; octet = 0; have = false; oct_cnt += 1; if oct_cnt > 3 { return None; } }
        else { return None; }
        i += 1;
    }
    if !have || oct_cnt != 3 { return None; }
    ip = (ip << 8) | octet;
    if i >= slen || s[i] != b':' { return None; }
    i += 1;
    let mut port: i32 = 0; let mut pdig = false;
    while i < slen { let c = s[i]; if c.is_ascii_digit() { port = port * 10 + (c - b'0') as i32; pdig = true; } else { return None; } i += 1; }
    if !pdig || port <= 0 || port > 65535 { return None; }
    Some((ip, port))
}

// --- minimal byte-string JSON field scans over the Alpaca envelope ---
fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || hay.len() < needle.len() { return None; }
    let mut i = 0;
    while i + needle.len() <= hay.len() {
        if &hay[i..i + needle.len()] == needle { return Some(i); }
        i += 1;
    }
    None
}
fn http_body(resp: &[u8]) -> &[u8] {
    match find(resp, b"\r\n\r\n") { Some(p) => &resp[p + 4..], None => &resp[0..0] }
}
fn json_num(body: &[u8], key: &[u8]) -> Option<f64> {
    let kpos = find(body, key)?;
    let mut i = kpos + key.len();
    while i < body.len() && (body[i] == b' ' || body[i] == b'"') { i += 1; }
    let mut neg = false;
    if i < body.len() && body[i] == b'-' { neg = true; i += 1; }
    let start = i;
    let mut ip: f64 = 0.0; let mut frac: f64 = 0.0; let mut fdiv: f64 = 1.0; let mut infrac = false;
    while i < body.len() {
        let c = body[i];
        if c.is_ascii_digit() {
            if infrac { fdiv *= 10.0; frac = frac * 10.0 + (c - b'0') as f64; }
            else { ip = ip * 10.0 + (c - b'0') as f64; }
            i += 1;
        } else if c == b'.' && !infrac { infrac = true; i += 1; }
        else { break; }
    }
    if i == start { return None; }
    let mut v = ip + frac / fdiv;
    if neg { v = -v; }
    Some(v)
}
// Alpaca returns booleans as JSON true/false (also tolerates 1/0). Pull one.
fn json_bool(body: &[u8], key: &[u8]) -> Option<bool> {
    let kpos = find(body, key)?;
    let mut i = kpos + key.len();
    while i < body.len() && (body[i] == b' ' || body[i] == b'"') { i += 1; }
    if i >= body.len() { return None; }
    match body[i] {
        b't' | b'T' | b'1' => Some(true),
        b'f' | b'F' | b'0' => Some(false),
        _ => None,
    }
}
// Copy the string "Value":"...." into out; returns the byte count copied.
fn json_str(body: &[u8], key: &[u8], out: &mut [u8]) -> usize {
    let kpos = match find(body, key) { Some(p) => p, None => return 0 };
    let mut i = kpos + key.len();
    while i < body.len() && body[i] == b' ' { i += 1; }
    if i >= body.len() || body[i] != b'"' { return 0; }
    i += 1;
    let mut n = 0;
    while i < body.len() && body[i] != b'"' && n < out.len() {
        out[n] = body[i]; n += 1; i += 1;
    }
    n
}
fn json_err(body: &[u8]) -> f64 { json_num(body, b"\"ErrorNumber\":").unwrap_or(0.0) }

// --- Alpaca ops ---
const OP_NONE: u8 = 0;
const OP_PUT_CONNECTED: u8 = 1;
const OP_GET_RA: u8 = 2;
const OP_GET_DEC: u8 = 3;
const OP_PUT_SLEW: u8 = 4;
const OP_PUT_SYNC: u8 = 5;
const OP_GET_ALT: u8 = 6;
const OP_GET_AZ: u8 = 7;
const OP_GET_SLEWING: u8 = 8;
const OP_GET_TRACKING: u8 = 9;
const OP_GET_NAME: u8 = 10;
const OP_PUT_ABORT: u8 = 11;
const OP_PUT_MOVEAXIS: u8 = 12;
const OP_PUT_TRACKING: u8 = 13;
const OP_PUT_TRACKINGRATE: u8 = 14;
const OP_PUT_PARK: u8 = 15;
const OP_PUT_UNPARK: u8 = 16;
const OP_PUT_FINDHOME: u8 = 17;
const OP_GET_CANSLEW: u8 = 18;
const OP_GET_CANSYNC: u8 = 19;
const OP_GET_CANPARK: u8 = 20;
const OP_GET_CANUNPARK: u8 = 21;
const OP_GET_CANFINDHOME: u8 = 22;
const OP_GET_CANSETTRACKING: u8 = 23;
const OP_GET_CANMOVEAXIS: u8 = 24;

const ST_IDLE: u8 = 0;
const ST_CONNECTING: u8 = 1;
const ST_SENDING: u8 = 2;
const ST_RECEIVING: u8 = 3;

// Discovery sub-state (separate UDP socket, independent of the TCP command path).
const DISCO_IDLE: u8 = 0;
const DISCO_WAIT: u8 = 1;

// Selectable jog rates, deg/sec.
static JOG_RATES: [f64; 4] = [0.5, 1.0, 2.0, 4.0];

// One queued/executing operation and the parameters it carries. Snapshotting
// params into the queue entry (rather than into shared struct fields) is what
// lets several commands queue without the later one overwriting the earlier
// one's target/axis/rate.
#[derive(Clone, Copy)]
struct AOp { op: u8, ra: f64, dec: f64, axis: i32, rate: f64, flag: bool }
fn aop(op: u8) -> AOp { AOp { op, ra: 0.0, dec: 0.0, axis: 0, rate: 0.0, flag: false } }

struct Alpaca {
    ip: u32, port: i32,
    sock: i32,
    state: u8,
    cur: AOp,                       // in-flight op (cur.op == OP_NONE when idle)
    req: [u8; 512], req_len: usize, req_sent: usize,
    resp: [u8; 2048], resp_len: usize,
    timeout: i32,
    cid: u32, txn: u32,
    connected: bool,
    have_pos: bool, scope_ra: f64, scope_dec: f64,
    have_altaz: bool, scope_alt: f64, scope_az: f64,
    slewing: bool, tracking: bool, track_rate: i32, parked: bool,
    tgt_ra: f64, tgt_dec: f64,
    name: [u8; 40], name_len: usize,
    can_slew: bool, can_sync: bool, can_park: bool, can_unpark: bool,
    can_findhome: bool, can_settracking: bool, can_moveaxis: bool, caps_known: bool,
    jog_rate_idx: usize,
    poll_timer: i32, poll_idx: usize,
    q: [AOp; 24], qh: usize, qt: usize, qn: usize,
    status: [u8; 48], status_len: usize,
    entry_on: bool, ebuf: [u8; 32], elen: usize,
    panel_open: bool,
    disco_state: u8, disco_sock: i32, disco_timer: i32, disco_found: i32,
    // verification-only scripted harness (file-gated, never in the golden).
    auto_on: bool, auto_step: i32, auto_timer: i32,
}

fn alpaca_new() -> Alpaca {
    Alpaca {
        ip: 0, port: 0, sock: -1, state: ST_IDLE, cur: aop(OP_NONE),
        req: [0; 512], req_len: 0, req_sent: 0,
        resp: [0; 2048], resp_len: 0,
        timeout: 0, cid: 4242, txn: 1,
        connected: false,
        have_pos: false, scope_ra: 0.0, scope_dec: 0.0,
        have_altaz: false, scope_alt: 0.0, scope_az: 0.0,
        slewing: false, tracking: false, track_rate: 0, parked: false,
        tgt_ra: 0.0, tgt_dec: 0.0,
        name: [0; 40], name_len: 0,
        can_slew: true, can_sync: true, can_park: true, can_unpark: true,
        can_findhome: true, can_settracking: true, can_moveaxis: true, caps_known: false,
        jog_rate_idx: 1,
        poll_timer: 30, poll_idx: 0,
        q: [aop(OP_NONE); 24], qh: 0, qt: 0, qn: 0,
        status: [0; 48], status_len: 0,
        entry_on: false, ebuf: [0; 32], elen: 0,
        panel_open: true,
        disco_state: DISCO_IDLE, disco_sock: -1, disco_timer: 0, disco_found: 0,
        auto_on: false, auto_step: 0, auto_timer: 60,
    }
}
fn alpaca_set_status(ac: &mut Alpaca, s: &[u8]) {
    let n = if s.len() > ac.status.len() { ac.status.len() } else { s.len() };
    ac.status[..n].copy_from_slice(&s[..n]);
    ac.status_len = n;
}

// --- bounded op queue ---
fn q_push(ac: &mut Alpaca, a: AOp) {
    let cap = ac.q.len();
    if ac.qn < cap { ac.q[ac.qt] = a; ac.qt = (ac.qt + 1) % cap; ac.qn += 1; }
}
fn q_pop(ac: &mut Alpaca) -> Option<AOp> {
    if ac.qn == 0 { return None; }
    let cap = ac.q.len();
    let a = ac.q[ac.qh]; ac.qh = (ac.qh + 1) % cap; ac.qn -= 1; Some(a)
}
// Public entry: enqueue a command. Callers never touch the socket directly.
fn alpaca_enqueue(ac: &mut Alpaca, a: AOp) { q_push(ac, a); }

fn op_is_put(op: u8) -> bool {
    matches!(op, OP_PUT_CONNECTED | OP_PUT_SLEW | OP_PUT_SYNC | OP_PUT_ABORT
        | OP_PUT_MOVEAXIS | OP_PUT_TRACKING | OP_PUT_TRACKINGRATE
        | OP_PUT_PARK | OP_PUT_UNPARK | OP_PUT_FINDHOME)
}

fn alpaca_path(op: u8) -> &'static [u8] {
    match op {
        OP_PUT_CONNECTED => b"/api/v1/telescope/0/connected",
        OP_GET_RA => b"/api/v1/telescope/0/rightascension",
        OP_GET_DEC => b"/api/v1/telescope/0/declination",
        OP_GET_ALT => b"/api/v1/telescope/0/altitude",
        OP_GET_AZ => b"/api/v1/telescope/0/azimuth",
        OP_GET_SLEWING => b"/api/v1/telescope/0/slewing",
        OP_GET_TRACKING | OP_PUT_TRACKING => b"/api/v1/telescope/0/tracking",
        OP_GET_NAME => b"/api/v1/telescope/0/name",
        OP_PUT_SLEW => b"/api/v1/telescope/0/slewtocoordinates",
        OP_PUT_SYNC => b"/api/v1/telescope/0/synctocoordinates",
        OP_PUT_ABORT => b"/api/v1/telescope/0/abortslew",
        OP_PUT_MOVEAXIS => b"/api/v1/telescope/0/moveaxis",
        OP_PUT_TRACKINGRATE => b"/api/v1/telescope/0/trackingrate",
        OP_PUT_PARK => b"/api/v1/telescope/0/park",
        OP_PUT_UNPARK => b"/api/v1/telescope/0/unpark",
        OP_PUT_FINDHOME => b"/api/v1/telescope/0/findhome",
        OP_GET_CANSLEW => b"/api/v1/telescope/0/canslewasync",
        OP_GET_CANSYNC => b"/api/v1/telescope/0/cansync",
        OP_GET_CANPARK => b"/api/v1/telescope/0/canpark",
        OP_GET_CANUNPARK => b"/api/v1/telescope/0/canunpark",
        OP_GET_CANFINDHOME => b"/api/v1/telescope/0/canfindhome",
        OP_GET_CANSETTRACKING => b"/api/v1/telescope/0/cansettracking",
        OP_GET_CANMOVEAXIS => b"/api/v1/telescope/0/canmoveaxis",
        _ => b"",
    }
}

// Build the form-urlencoded PUT body for the current op into buf; returns len.
// Op-specific params first (may be empty), then the ClientID/txn tail.
fn build_body(ac: &Alpaca, buf: &mut [u8]) -> usize {
    let op = ac.cur.op;
    let mut n = 0usize;
    match op {
        OP_PUT_CONNECTED => { n = cat(buf, n, b"Connected="); n = cat(buf, n, if ac.cur.flag { b"true" } else { b"false" }); }
        OP_PUT_SLEW | OP_PUT_SYNC => {
            // ALPACA GOTCHA: RightAscension travels in decimal HOURS, not degrees.
            n = cat(buf, n, b"RightAscension="); n = put_f6(buf, n, ac.cur.ra / 15.0);
            n = cat(buf, n, b"&Declination="); n = put_f6(buf, n, ac.cur.dec);
        }
        OP_PUT_MOVEAXIS => { n = cat(buf, n, b"Axis="); n = put_i(buf, n, ac.cur.axis as i64); n = cat(buf, n, b"&Rate="); n = put_f6(buf, n, ac.cur.rate); }
        OP_PUT_TRACKING => { n = cat(buf, n, b"Tracking="); n = cat(buf, n, if ac.cur.flag { b"true" } else { b"false" }); }
        OP_PUT_TRACKINGRATE => { n = cat(buf, n, b"TrackingRate="); n = put_i(buf, n, ac.cur.axis as i64); }
        _ => {} // abort/park/unpark/findhome carry only ClientID
    }
    if n > 0 { buf[n] = b'&'; n += 1; }
    n = cat(buf, n, b"ClientID="); n = put_i(buf, n, ac.cid as i64);
    n = cat(buf, n, b"&ClientTransactionID="); n = put_i(buf, n, ac.txn as i64);
    n
}

// Begin the op currently in ac.cur (set by alpaca_pump). Opens the socket and
// arms the state machine; NEVER blocks.
fn alpaca_begin(ac: &mut Alpaca) {
    if ac.ip == 0 || ac.port == 0 { alpaca_set_status(ac, b"Alpaca: no host set"); ac.cur.op = OP_NONE; return; }
    let op = ac.cur.op;
    let path = alpaca_path(op);
    let mut n = 0usize;
    if op_is_put(op) {
        let mut b = [0u8; 200];
        let bn = build_body(ac, &mut b);
        n = cat(&mut ac.req, n, b"PUT "); n = cat(&mut ac.req, n, path);
        n = cat(&mut ac.req, n, b" HTTP/1.1\r\nHost: "); n = put_ip(&mut ac.req, n, ac.ip);
        n = cat(&mut ac.req, n, b":"); n = put_i(&mut ac.req, n, ac.port as i64);
        n = cat(&mut ac.req, n, b"\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ");
        n = put_i(&mut ac.req, n, bn as i64);
        n = cat(&mut ac.req, n, b"\r\nConnection: close\r\n\r\n");
        n = cat(&mut ac.req, n, &b[..bn]);
    } else {
        n = cat(&mut ac.req, n, b"GET "); n = cat(&mut ac.req, n, path);
        n = cat(&mut ac.req, n, b"?ClientID="); n = put_i(&mut ac.req, n, ac.cid as i64);
        n = cat(&mut ac.req, n, b"&ClientTransactionID="); n = put_i(&mut ac.req, n, ac.txn as i64);
        if op == OP_GET_CANMOVEAXIS { n = cat(&mut ac.req, n, b"&Axis=0"); }
        n = cat(&mut ac.req, n, b" HTTP/1.1\r\nHost: "); n = put_ip(&mut ac.req, n, ac.ip);
        n = cat(&mut ac.req, n, b":"); n = put_i(&mut ac.req, n, ac.port as i64);
        n = cat(&mut ac.req, n, b"\r\nConnection: close\r\n\r\n");
    }
    ac.req_len = n;
    ac.txn = ac.txn.wrapping_add(1);
    ac.sock = tcp_socket();
    if ac.sock < 0 { alpaca_set_status(ac, b"Alpaca: socket failed"); ac.cur.op = OP_NONE; return; }
    let _ = tcp_connect(ac.sock, ac.ip, ac.port);
    ac.state = ST_CONNECTING; ac.timeout = 300; ac.req_sent = 0; ac.resp_len = 0;
}

// When idle, start the next queued op. Call once per frame from the main loop.
fn alpaca_pump(ac: &mut Alpaca) {
    if ac.state != ST_IDLE || ac.cur.op != OP_NONE { return; }
    if let Some(a) = q_pop(ac) { ac.cur = a; alpaca_begin(ac); }
}

// Queue the mount-name + all capability probes + one full status sweep. Called
// once on a successful connect so the panel knows what the mount can do.
fn alpaca_after_connect(ac: &mut Alpaca) {
    q_push(ac, aop(OP_GET_NAME));
    q_push(ac, aop(OP_GET_CANSLEW));
    q_push(ac, aop(OP_GET_CANSYNC));
    q_push(ac, aop(OP_GET_CANPARK));
    q_push(ac, aop(OP_GET_CANUNPARK));
    q_push(ac, aop(OP_GET_CANFINDHOME));
    q_push(ac, aop(OP_GET_CANSETTRACKING));
    q_push(ac, aop(OP_GET_CANMOVEAXIS));
    q_push(ac, aop(OP_GET_RA));
    q_push(ac, aop(OP_GET_DEC));
    q_push(ac, aop(OP_GET_ALT));
    q_push(ac, aop(OP_GET_AZ));
    q_push(ac, aop(OP_GET_SLEWING));
    q_push(ac, aop(OP_GET_TRACKING));
}

fn alpaca_finish(ac: &mut Alpaca) {
    let len = ac.resp_len;
    let op = ac.cur.op;
    // Copy body region out so we can borrow ac mutably below.
    let (err, num, boolv, namebuf, namelen) = {
        let body = http_body(&ac.resp[..len]);
        let mut nb = [0u8; 40];
        let nl = if op == OP_GET_NAME { json_str(body, b"\"Value\":", &mut nb) } else { 0 };
        (json_err(body), json_num(body, b"\"Value\":"), json_bool(body, b"\"Value\":"), nb, nl)
    };
    match op {
        OP_PUT_CONNECTED => {
            if err == 0.0 && ac.cur.flag {
                ac.connected = true; alpaca_set_status(ac, b"Connected"); ac.caps_known = false;
                alpaca_after_connect(ac);
            } else if ac.cur.flag {
                ac.connected = false; alpaca_set_status(ac, b"Connect error");
            } else {
                ac.connected = false; ac.have_pos = false; ac.have_altaz = false;
                alpaca_set_status(ac, b"Disconnected");
            }
        }
        OP_GET_RA => { if let Some(h) = num { ac.scope_ra = h * 15.0; ac.have_pos = true; } }
        OP_GET_DEC => { if let Some(d) = num { ac.scope_dec = d; } }
        OP_GET_ALT => { if let Some(a) = num { ac.scope_alt = a; ac.have_altaz = true; } }
        OP_GET_AZ => { if let Some(z) = num { ac.scope_az = z; } }
        OP_GET_SLEWING => { if let Some(b) = boolv { ac.slewing = b; } }
        OP_GET_TRACKING => { if let Some(b) = boolv { ac.tracking = b; } }
        OP_GET_NAME => { let n = namelen; ac.name[..n].copy_from_slice(&namebuf[..n]); ac.name_len = n; }
        OP_GET_CANSLEW => { if let Some(b) = boolv { ac.can_slew = b; } }
        OP_GET_CANSYNC => { if let Some(b) = boolv { ac.can_sync = b; } }
        OP_GET_CANPARK => { if let Some(b) = boolv { ac.can_park = b; } }
        OP_GET_CANUNPARK => { if let Some(b) = boolv { ac.can_unpark = b; } }
        OP_GET_CANFINDHOME => { if let Some(b) = boolv { ac.can_findhome = b; } }
        OP_GET_CANSETTRACKING => { if let Some(b) = boolv { ac.can_settracking = b; } }
        OP_GET_CANMOVEAXIS => { if let Some(b) = boolv { ac.can_moveaxis = b; } ac.caps_known = true; }
        OP_PUT_SLEW => { if err == 0.0 { ac.slewing = true; alpaca_set_status(ac, b"Slewing to target"); } else { alpaca_set_status(ac, b"Slew error"); } }
        OP_PUT_SYNC => { if err == 0.0 { alpaca_set_status(ac, b"Synced"); } else { alpaca_set_status(ac, b"Sync error"); } }
        OP_PUT_ABORT => { if err == 0.0 { ac.slewing = false; alpaca_set_status(ac, b"Slew aborted"); } else { alpaca_set_status(ac, b"Abort error"); } }
        OP_PUT_MOVEAXIS => { if err == 0.0 { alpaca_set_status(ac, b"Jogging"); } else { alpaca_set_status(ac, b"Jog error"); } }
        OP_PUT_TRACKING => { if err == 0.0 { ac.tracking = ac.cur.flag; alpaca_set_status(ac, if ac.cur.flag { b"Tracking on" } else { b"Tracking off" }); } else { alpaca_set_status(ac, b"Tracking error"); } }
        OP_PUT_TRACKINGRATE => { if err == 0.0 { ac.track_rate = ac.cur.axis; alpaca_set_status(ac, b"Tracking rate set"); } else { alpaca_set_status(ac, b"Track rate error"); } }
        OP_PUT_PARK => { if err == 0.0 { ac.parked = true; ac.tracking = false; alpaca_set_status(ac, b"Parking"); } else { alpaca_set_status(ac, b"Park error"); } }
        OP_PUT_UNPARK => { if err == 0.0 { ac.parked = false; alpaca_set_status(ac, b"Unparked"); } else { alpaca_set_status(ac, b"Unpark error"); } }
        OP_PUT_FINDHOME => { if err == 0.0 { alpaca_set_status(ac, b"Finding home"); } else { alpaca_set_status(ac, b"Home error"); } }
        _ => {}
    }
    ac.cur.op = OP_NONE;
}

// Advance at most one non-blocking syscall's progress. Call once per frame.
fn alpaca_step(ac: &mut Alpaca) {
    if ac.state == ST_IDLE { return; }
    ac.timeout -= 1;
    if ac.timeout <= 0 {
        tcp_close(ac.sock); ac.state = ST_IDLE; ac.cur.op = OP_NONE;
        alpaca_set_status(ac, b"Alpaca: timeout");
        return;
    }
    match ac.state {
        ST_CONNECTING => {
            let st = tcp_state(ac.sock);
            if st == TCP_STATE_ESTABLISHED { ac.state = ST_SENDING; }
            else if st == TCP_STATE_CLOSED { tcp_close(ac.sock); ac.state = ST_IDLE; ac.cur.op = OP_NONE; alpaca_set_status(ac, b"Alpaca: connect failed"); }
        }
        ST_SENDING => {
            let n = tcp_send(ac.sock, &ac.req[ac.req_sent..ac.req_len]);
            if n > 0 {
                ac.req_sent += n as usize;
                if ac.req_sent >= ac.req_len { ac.state = ST_RECEIVING; }
            } else if n < 0 { tcp_close(ac.sock); ac.state = ST_IDLE; ac.cur.op = OP_NONE; alpaca_set_status(ac, b"Alpaca: send failed"); }
        }
        ST_RECEIVING => {
            if ac.resp_len < ac.resp.len() {
                let cap = ac.resp.len() - ac.resp_len;
                let start = ac.resp_len;
                let n = tcp_recv(ac.sock, &mut ac.resp[start..start + cap]);
                if n > 0 { ac.resp_len += n as usize; }
                else if n < 0 { tcp_close(ac.sock); alpaca_finish(ac); ac.state = ST_IDLE; }
            } else { tcp_close(ac.sock); alpaca_finish(ac); ac.state = ST_IDLE; }
        }
        _ => {}
    }
}

// Once connected and idle, enqueue a rolling status sweep every ~1.5s so the
// panel readouts and the reticle track the real mount. Called each frame.
fn alpaca_poll_tick(ac: &mut Alpaca) {
    if !ac.connected || ac.state != ST_IDLE || ac.cur.op != OP_NONE || ac.qn != 0 { return; }
    ac.poll_timer -= 1;
    if ac.poll_timer > 0 { return; }
    ac.poll_timer = 12;
    let seq = [OP_GET_RA, OP_GET_DEC, OP_GET_ALT, OP_GET_AZ, OP_GET_SLEWING, OP_GET_TRACKING];
    let op = seq[ac.poll_idx % seq.len()];
    ac.poll_idx = (ac.poll_idx + 1) % seq.len();
    if ac.poll_idx == 0 { ac.poll_timer = 40; } // pause after a full sweep
    q_push(ac, aop(op));
}

// --- command helpers used by the key/panel handlers in lib.rs ---
fn cmd_connect(ac: &mut Alpaca) { let mut a = aop(OP_PUT_CONNECTED); a.flag = true; q_push(ac, a); alpaca_set_status(ac, b"Connecting..."); }
fn cmd_disconnect(ac: &mut Alpaca) { let mut a = aop(OP_PUT_CONNECTED); a.flag = false; q_push(ac, a); }
fn cmd_goto(ac: &mut Alpaca, ra: f64, dec: f64) { if !ac.can_slew { alpaca_set_status(ac, b"Slew not supported"); return; } let mut a = aop(OP_PUT_SLEW); a.ra = ra; a.dec = dec; ac.tgt_ra = ra; ac.tgt_dec = dec; q_push(ac, a); }
fn cmd_sync(ac: &mut Alpaca, ra: f64, dec: f64) { if !ac.can_sync { alpaca_set_status(ac, b"Sync not supported"); return; } let mut a = aop(OP_PUT_SYNC); a.ra = ra; a.dec = dec; ac.tgt_ra = ra; ac.tgt_dec = dec; q_push(ac, a); }
fn cmd_abort(ac: &mut Alpaca) { q_push(ac, aop(OP_PUT_ABORT)); }
fn cmd_jog(ac: &mut Alpaca, axis: i32, sign: f64) {
    if !ac.can_moveaxis { alpaca_set_status(ac, b"Jog not supported"); return; }
    let mut a = aop(OP_PUT_MOVEAXIS); a.axis = axis; a.rate = sign * JOG_RATES[ac.jog_rate_idx % JOG_RATES.len()]; q_push(ac, a);
}
fn cmd_jog_stop(ac: &mut Alpaca) {
    let mut a = aop(OP_PUT_MOVEAXIS); a.axis = 0; a.rate = 0.0; q_push(ac, a);
    let mut b = aop(OP_PUT_MOVEAXIS); b.axis = 1; b.rate = 0.0; q_push(ac, b);
}
fn cmd_jog_rate_cycle(ac: &mut Alpaca) { ac.jog_rate_idx = (ac.jog_rate_idx + 1) % JOG_RATES.len(); }
fn cmd_tracking_toggle(ac: &mut Alpaca) { if !ac.can_settracking { alpaca_set_status(ac, b"Tracking set unsupported"); return; } let mut a = aop(OP_PUT_TRACKING); a.flag = !ac.tracking; q_push(ac, a); }
fn cmd_tracking_rate(ac: &mut Alpaca, rate: i32) { if !ac.can_settracking { alpaca_set_status(ac, b"Tracking set unsupported"); return; } let mut a = aop(OP_PUT_TRACKINGRATE); a.axis = rate; q_push(ac, a); }
fn cmd_park(ac: &mut Alpaca) { if !ac.can_park { alpaca_set_status(ac, b"Park not supported"); return; } q_push(ac, aop(OP_PUT_PARK)); }
fn cmd_unpark(ac: &mut Alpaca) { if !ac.can_unpark { alpaca_set_status(ac, b"Unpark not supported"); return; } q_push(ac, aop(OP_PUT_UNPARK)); }
fn cmd_findhome(ac: &mut Alpaca) { if !ac.can_findhome { alpaca_set_status(ac, b"Home not supported"); return; } q_push(ac, aop(OP_PUT_FINDHOME)); }

// --- UDP broadcast discovery (best-effort, fully async, never blocks) ---
// Alpaca discovery: broadcast the ASCII "alpacadiscovery1" to UDP 32227; each
// device replies with {"AlpacaPort":NNNNN} from its own IP. The kernel routes a
// 255.255.255.255 datagram to the L2 broadcast MAC (net/ip.c ip_send_broadcast),
// and recvfrom(MSG_DONTWAIT) is non-blocking, so this fits the frame loop. If
// nothing replies within the window it reports "no devices" and manual entry
// (the reliable baseline) is untouched.
fn disco_start(ac: &mut Alpaca) {
    if ac.disco_state != DISCO_IDLE { return; }
    let fd = unsafe { syscall3(SYS_SOCK_OPEN, AF_INET, SOCK_DGRAM, 0) as i32 };
    if fd < 0 { alpaca_set_status(ac, b"Discovery: no UDP socket"); return; }
    let one: i32 = 1;
    unsafe { syscall5(SYS_SOCK_SETOPT, fd as i64, SOL_SOCKET, SO_BROADCAST, &one as *const i32 as i64, 4); }
    // sockaddr_in: family(u16 LE), port(u16 BE), addr(u32 net order), zero[8]
    let mut sa = [0u8; 16];
    sa[0] = AF_INET as u8; sa[1] = 0;
    sa[2] = (ALPACA_DISCOVERY_PORT >> 8) as u8; sa[3] = (ALPACA_DISCOVERY_PORT & 0xFF) as u8;
    sa[4] = 0xFF; sa[5] = 0xFF; sa[6] = 0xFF; sa[7] = 0xFF; // 255.255.255.255
    let msg = b"alpacadiscovery1";
    unsafe { syscall6(SYS_SOCK_SENDTO, fd as i64, msg.as_ptr() as i64, msg.len() as i64, 0, sa.as_ptr() as i64, 16); }
    ac.disco_sock = fd; ac.disco_state = DISCO_WAIT; ac.disco_timer = 90; ac.disco_found = 0;
    alpaca_set_status(ac, b"Discovering...");
}
fn disco_step(ac: &mut Alpaca) {
    if ac.disco_state != DISCO_WAIT { return; }
    let mut buf = [0u8; 512];
    let mut src = [0u8; 16];
    let mut slen: i32 = 16;
    let n = unsafe {
        syscall6(SYS_SOCK_RECVFROM, ac.disco_sock as i64, buf.as_mut_ptr() as i64,
            buf.len() as i64, MSG_DONTWAIT, src.as_mut_ptr() as i64, &mut slen as *mut i32 as i64)
    } as i32;
    if n > 0 {
        // Responder IP from the sockaddr_in (net order = first octet in MSB).
        let ip = ((src[4] as u32) << 24) | ((src[5] as u32) << 16) | ((src[6] as u32) << 8) | (src[7] as u32);
        if let Some(p) = json_num(&buf[..n as usize], b"\"AlpacaPort\":") {
            if ip != 0 && p > 0.0 {
                ac.ip = ip; ac.port = p as i32; ac.disco_found += 1;
                let mut s = [0u8; 48]; let mut k = 0;
                k = cat(&mut s, k, b"Found "); k = put_ip(&mut s, k, ip);
                s[k] = b':'; k += 1; k = put_i(&mut s, k, p as i64);
                alpaca_set_status(ac, &s[..k]);
                unsafe { syscall1(SYS_CLOSE_FD, ac.disco_sock as i64); }
                ac.disco_state = DISCO_IDLE; ac.disco_sock = -1;
                return;
            }
        }
    }
    ac.disco_timer -= 1;
    if ac.disco_timer <= 0 {
        unsafe { syscall1(SYS_CLOSE_FD, ac.disco_sock as i64); }
        ac.disco_state = DISCO_IDLE; ac.disco_sock = -1;
        if ac.disco_found == 0 { alpaca_set_status(ac, b"No devices (use [A] manual)"); }
    }
}

// --- verification-only scripted harness ------------------------------------
// Reads /ALPACA.AUTO ("ip:port" on line 1); present ONLY on a throwaway
// verification image, never in the golden (same file-driven idiom as the
// compositor testhook). When present it connects and, once connected, fires
// every command in turn a few seconds apart so a host stub's request log and a
// pair of screendumps are a deterministic, injection-free proof. A missing
// file is one sys_open returning -1 and the harness stays off.
fn alpaca_auto_load(ac: &mut Alpaca) {
    let path = b"/ALPACA.AUTO\0";
    let fd = unsafe { syscall3(SYS_OPEN_FD, path.as_ptr() as i64, 0, 0) as i32 };
    if fd < 0 { return; }
    let mut buf = [0u8; 40];
    let n = unsafe { syscall3(SYS_READ_FD, fd as i64, buf.as_mut_ptr() as i64, buf.len() as i64) as i32 };
    unsafe { syscall1(SYS_CLOSE_FD, fd as i64); }
    if n <= 0 { return; }
    // trim to first line
    let mut len = 0usize;
    while len < n as usize && buf[len] != b'\n' && buf[len] != b'\r' && buf[len] != 0 { len += 1; }
    if let Some((ip, port)) = parse_ipport(&buf, len) {
        ac.ip = ip; ac.port = port;
        ac.auto_on = true; ac.auto_step = 0; ac.auto_timer = 60;
        cmd_connect(ac);
        serial(b"[ALPACA-AUTO] armed, connecting\n");
    }
}
fn alpaca_auto_tick(ac: &mut Alpaca) {
    if !ac.auto_on { return; }
    if ac.state != ST_IDLE || ac.cur.op != OP_NONE || ac.qn != 0 { return; }
    if !ac.connected {
        // Keep retrying connect until it takes (the first attempt can race DHCP
        // coming up right after the app is launched).
        if ac.auto_timer > 0 { ac.auto_timer -= 1; return; }
        ac.auto_timer = 90;
        serial(b"[ALPACA-AUTO] (re)connect\n");
        cmd_connect(ac);
        return;
    }
    if ac.auto_timer > 0 { ac.auto_timer -= 1; return; }
    ac.auto_timer = 75; // ~2.5s between scripted actions so states are visible
    let step = ac.auto_step; ac.auto_step += 1;
    match step {
        0 => { serial(b"[ALPACA-AUTO] goto\n"); cmd_goto(ac, 83.822, -5.391); }   // M42-ish
        1 => { serial(b"[ALPACA-AUTO] sync\n"); cmd_sync(ac, 83.822, -5.391); }
        2 => { serial(b"[ALPACA-AUTO] abort\n"); cmd_abort(ac); }
        3 => { serial(b"[ALPACA-AUTO] jog E\n"); cmd_jog(ac, 0, 1.0); }
        4 => { serial(b"[ALPACA-AUTO] jog N\n"); cmd_jog(ac, 1, 1.0); }
        5 => { serial(b"[ALPACA-AUTO] jog stop\n"); cmd_jog_stop(ac); }
        6 => { serial(b"[ALPACA-AUTO] tracking on\n"); let mut a = aop(OP_PUT_TRACKING); a.flag = true; q_push(ac, a); }
        7 => { serial(b"[ALPACA-AUTO] tracking rate lunar\n"); cmd_tracking_rate(ac, 1); }
        8 => { serial(b"[ALPACA-AUTO] park\n"); cmd_park(ac); }
        9 => { serial(b"[ALPACA-AUTO] unpark\n"); cmd_unpark(ac); }
        10 => { serial(b"[ALPACA-AUTO] findhome\n"); cmd_findhome(ac); }
        _ => { serial(b"[ALPACA-AUTO] done\n"); ac.auto_on = false; }
    }
}
