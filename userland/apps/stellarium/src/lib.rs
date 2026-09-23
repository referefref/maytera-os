// Maytera Planetarium (stellarium) - a from-scratch native planetarium for
// MayteraOS in Rust, built on the hardware-float userland target
// x86_64-maytera-user (docs/STELLARIUM_PORT_ASSESSMENT.md, M0/M1).
//
// This file is M1's core: the astronomy math (Julian date, GMST, precession-free
// apparent alt/az), a stereographic projection centred on the view direction,
// magnitude-scaled star glyphs, and pan/zoom/time input. It proves the pipeline
// end to end. The full Hipparcos catalog (build/assets/stellarium/STARS.CAT,
// generated from hip_main.dat by a host tool) replaces the built-in bright-star
// table in a follow-up; the table here is enough to validate correctness and
// render a recognisable sky.
//
// Hardware float: every value below is f64 and compiles to real mulsd/sqrtsd/
// sin/cos on x86_64-maytera-user. libm (sin/cos/asin/atan2/...) is FFI'd from
// libc.a, which is ABI-sound because this target uses the SysV xmm convention
// (see the assessment's ABI note).
#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_i: &PanicInfo) -> ! {
    unsafe { syscall1(SYS_EXIT, 101) };
    loop {}
}

// --- syscalls (libc/syscall.asm) ---
extern "C" {
    fn syscall1(n: i64, a1: i64) -> i64;
    fn syscall2(n: i64, a1: i64, a2: i64) -> i64;
    fn syscall3(n: i64, a1: i64, a2: i64, a3: i64) -> i64;
    fn syscall5(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64) -> i64;
    fn syscall6(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64, a6: i64) -> i64;
}
// --- libm (libc.a, hardware double) ---
extern "C" {
    fn sin(x: f64) -> f64;
    fn cos(x: f64) -> f64;
    fn asin(x: f64) -> f64;
    fn atan2(y: f64, x: f64) -> f64;
    fn floor(x: f64) -> f64;
    fn sqrt(x: f64) -> f64;
    fn fmod(x: f64, y: f64) -> f64;
}

const SYS_EXIT: i64 = 0;
const SYS_WRITE: i64 = 13;
const SYS_WIN_DRAW_TEXT_SMALL: i64 = 232;
const SYS_WIN_DRAW_TTF: i64 = 235;
const SYS_WIN_CREATE: i64 = 30;
const SYS_WIN_DRAW_RECT: i64 = 32;
const SYS_WIN_GET_EVENT: i64 = 36;
const SYS_WIN_INVALIDATE: i64 = 37;
const SYS_TIME: i64 = 50; // seconds since the UNIX epoch (UTC), header ticket 113

const EVENT_MOUSE_MOVE: u32 = 1;
const EVENT_MOUSE_DOWN: u32 = 2;
const EVENT_MOUSE_UP: u32 = 3;
const EVENT_MOUSE_SCROLL: u32 = 4;
const EVENT_KEY_DOWN: u32 = 5;
const EVENT_WINDOW_CLOSE: u32 = 7;
const EVENT_RESIZE: u32 = 12;

#[repr(C)]
#[derive(Clone, Copy)]
struct GuiEvent {
    ty: u32,
    target_id: u32,
    mouse_x: i32,
    mouse_y: i32,
    mouse_buttons: u32,
    scroll_delta: i8,
    keycode: u32,
    key_char: u8,
}

fn win_create(title: &[u8], x: i32, y: i32, w: i32, h: i32) -> i32 {
    unsafe { syscall5(SYS_WIN_CREATE, title.as_ptr() as i64, x as i64, y as i64, w as i64, h as i64) as i32 }
}
fn win_draw_rect(h: i32, x: i32, y: i32, w: i32, ht: i32, c: u32) {
    unsafe { syscall6(SYS_WIN_DRAW_RECT, h as i64, x as i64, y as i64, w as i64, ht as i64, (c | 0xFF00_0000) as i64); }
}
fn win_get_event(h: i32, ev: &mut GuiEvent, timeout_ms: i32) -> i32 {
    unsafe { syscall3(SYS_WIN_GET_EVENT, h as i64, ev as *mut GuiEvent as i64, timeout_ms as i64) as i32 }
}
fn win_invalidate(h: i32) { unsafe { syscall1(SYS_WIN_INVALIDATE, h as i64); } }
// label: text must be NUL-terminated (kernel reads a char*); NAMED_STARS supplies it.
fn win_text(h: i32, x: i32, y: i32, t: &[u8], c: u32) {
    // Antialiased TrueType (SYS_WIN_DRAW_TTF); point size packed in the top byte
    // of the colour word, per the shared convention.
    let size: u32 = 12;
    let packed = ((c & 0x00FF_FFFF) | ((size & 0xFF) << 24)) as i64;
    unsafe { syscall5(SYS_WIN_DRAW_TTF, h as i64, x as i64, y as i64, t.as_ptr() as i64, packed); }
}
fn serial(s: &[u8]) { unsafe { syscall3(SYS_WRITE, 1, s.as_ptr() as i64, s.len() as i64); } }

const PI: f64 = 3.14159265358979323846;
const DEG: f64 = PI / 180.0;

// ---- astronomy core (Meeus, apparent alt/az; precession/nutation omitted at M1) ----
// Julian Date from a Gregorian civil date + fractional hours UT.
fn julian_date(mut y: i32, mut m: i32, d: i32, hours: f64) -> f64 {
    if m <= 2 { y -= 1; m += 12; }
    let a = (y as f64 / 100.0).floor_();
    let b = 2.0 - a + (a / 4.0).floor_();
    (365.25 * (y as f64 + 4716.0)).floor_()
        + (30.6001 * (m as f64 + 1.0)).floor_()
        + d as f64 + b - 1524.5 + hours / 24.0
}
// Greenwich Mean Sidereal Time in degrees (Meeus 12.4).
fn gmst_deg(jd: f64) -> f64 {
    let t = (jd - 2451545.0) / 36525.0;
    let mut g = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
        + 0.000387933 * t * t - t * t * t / 38710000.0;
    g = fmodp(g, 360.0);
    g
}
// RA/Dec (deg) at observer lat/lon (deg, east +) and GMST -> alt/az (deg).
fn radec_to_altaz(ra: f64, dec: f64, lat: f64, lon: f64, gmst: f64) -> (f64, f64) {
    let lst = fmodp(gmst + lon, 360.0);       // local sidereal time, deg
    let ha = fmodp(lst - ra, 360.0) * DEG;    // hour angle, rad
    let (dr, lr) = (dec * DEG, lat * DEG);
    let sin_alt = unsafe { sin(dr) * sin(lr) + cos(dr) * cos(lr) * cos(ha) };
    let alt = unsafe { asin(clamp(sin_alt, -1.0, 1.0)) };
    let az = unsafe { atan2(sin(ha), cos(ha) * sin(lr) - (sin(dr) / cos(dr)) * cos(lr)) };
    // az measured from South, +W in Meeus; convert to from-North, +E (0=N,90=E).
    let az_n = fmodp(az / DEG + 180.0, 360.0);
    (alt / DEG, az_n)
}

fn fmodp(x: f64, m: f64) -> f64 { let r = unsafe { fmod(x, m) }; if r < 0.0 { r + m } else { r } }
fn clamp(x: f64, lo: f64, hi: f64) -> f64 { if x < lo { lo } else if x > hi { hi } else { x } }
trait FloorExt { fn floor_(self) -> f64; }
impl FloorExt for f64 { fn floor_(self) -> f64 { unsafe { floor(self) } } }

// ---- star table (bright naked-eye stars; RA/Dec J2000 deg, visual mag, B-V) ----
struct Star { ra: f64, dec: f64, mag: f64, bv: f64 }
const STARS: &[Star] = &[
    Star { ra: 101.287, dec: -16.716, mag: -1.46, bv: 0.00 },  // Sirius
    Star { ra: 279.234, dec: 38.784,  mag: 0.03,  bv: 0.00 },  // Vega
    Star { ra: 37.955,  dec: 89.264,  mag: 1.97,  bv: 0.60 },  // Polaris
    Star { ra: 95.988,  dec: -52.696, mag: -0.74, bv: 0.15 },  // Canopus
    Star { ra: 213.915, dec: 19.182,  mag: -0.05, bv: 1.24 },  // Arcturus
    Star { ra: 78.634,  dec: -8.202,  mag: 0.13,  bv: -0.03 }, // Rigel
    Star { ra: 88.793,  dec: 7.407,   mag: 0.50,  bv: 1.85 },  // Betelgeuse
    Star { ra: 68.980,  dec: 16.509,  mag: 0.85,  bv: 1.54 },  // Aldebaran
    Star { ra: 201.298, dec: -11.161, mag: 0.98,  bv: -0.24 }, // Spica
    Star { ra: 152.093, dec: 11.967,  mag: 1.35,  bv: -0.11 }, // Regulus
    Star { ra: 297.696, dec: 8.868,   mag: 0.77,  bv: 0.22 },  // Altair
    Star { ra: 310.358, dec: 45.280,  mag: 1.25,  bv: -0.03 }, // Deneb
    Star { ra: 24.429,  dec: -57.237, mag: 0.46,  bv: 0.16 },  // Achernar
    Star { ra: 187.791, dec: -57.113, mag: 1.25,  bv: -0.23 }, // Mimosa
    Star { ra: 186.650, dec: -63.099, mag: 0.77,  bv: -0.24 }, // Acrux
    Star { ra: 114.825, dec: 5.225,   mag: 1.14,  bv: 0.00 },  // Procyon
    Star { ra: 116.329, dec: 28.026,  mag: 1.14,  bv: 1.00 },  // Pollux
    Star { ra: 113.649, dec: 31.888,  mag: 1.58,  bv: 0.03 },  // Castor
    Star { ra: 344.413, dec: -29.622, mag: 1.16,  bv: 0.09 },  // Fomalhaut
    Star { ra: 210.956, dec: -60.373, mag: 0.61,  bv: -0.02 }, // Hadar
];

include!("stars.rs");

// The brightest named stars (J2000), labelled in the view. Names are NUL-
// terminated for the text syscall. Kept small and hand-verified.
include!("star_names.rs");

// Iconic constellation stick figures as equatorial line segments
// (ra1,dec1,ra2,dec2), J2000. Orion, Ursa Major (Big Dipper), Cassiopeia, Cygnus.
static CONSTELLATION_SEGS: &[(f64, f64, f64, f64)] = &[
    // Orion
    (88.793,7.407, 85.190,-1.943), (81.283,6.350, 83.002,-0.299),
    (83.002,-0.299, 84.053,-1.202), (84.053,-1.202, 85.190,-1.943),
    (85.190,-1.943, 86.939,-9.670), (83.002,-0.299, 78.634,-8.202),
    (88.793,7.407, 81.283,6.350),
    // Big Dipper (Ursa Major)
    (165.932,61.751, 165.460,56.383), (165.460,56.383, 178.458,53.695),
    (178.458,53.695, 183.857,57.033), (183.857,57.033, 165.932,61.751),
    (183.857,57.033, 193.507,55.960), (193.507,55.960, 200.981,54.925),
    (200.981,54.925, 206.885,49.313),
    // Cassiopeia
    (2.295,59.150, 10.127,56.537), (10.127,56.537, 14.177,60.717),
    (14.177,60.717, 21.454,60.235), (21.454,60.235, 28.599,63.670),
    // Cygnus (Northern Cross)
    (310.358,45.280, 305.557,40.257), (305.557,40.257, 292.680,27.960),
    (296.243,45.131, 305.557,40.257), (305.557,40.257, 311.553,33.970),
];


include!("solar.rs");
include!("solar_tex.rs");
include!("deepsky.rs");
include!("alpaca.rs");

// B-V -> approximate RGB (blue-white hot -> orange-red cool).
fn bv_color(bv: f64) -> u32 {
    let t = clamp((bv + 0.4) / 2.0, 0.0, 1.0);
    let r = (150.0 + 105.0 * t) as u32;
    let g = (190.0 + 40.0 * (1.0 - (t - 0.5) * (t - 0.5) * 4.0)) as u32;
    let b = (255.0 - 120.0 * t) as u32;
    (r.min(255) << 16) | (g.min(255) << 8) | b.min(255)
}

fn put_f(buf: &mut [u8], mut n: usize, v: f64) -> usize {
    let neg = v < 0.0;
    let a = if neg { -v } else { v };
    let ip = a as i64;
    let fp = ((a - ip as f64) * 100.0 + 0.5) as i64;
    if neg { buf[n] = b'-'; n += 1; }
    n = put_i(buf, n, ip);
    buf[n] = b'.'; n += 1;
    if fp < 10 { buf[n] = b'0'; n += 1; }
    put_i(buf, n, fp)
}
fn put_i(buf: &mut [u8], mut n: usize, mut v: i64) -> usize {
    if v == 0 { buf[n] = b'0'; return n + 1; }
    let mut tmp = [0u8; 20]; let mut t = 0;
    while v > 0 { tmp[t] = b'0' + (v % 10) as u8; v /= 10; t += 1; }
    while t > 0 { t -= 1; buf[n] = tmp[t]; n += 1; }
    n
}

// Fill a small filled disc for a star of the given on-screen radius.
fn fill_disk(win: i32, cx: i32, cy: i32, r: i32, color: u32) {
    for dy in -r..=r {
        let hh = (r * r - dy * dy) as f64;
        if hh < 0.0 { continue; }
        let hw = unsafe { sqrt(hh) } as i32;
        win_draw_rect(win, cx - hw, cy + dy, 2 * hw + 1, 1, color);
    }
}
// Moon disk with a simple phase terminator. elong 0=new (dark) .. 180=full (lit).
fn draw_moon(win: i32, cx: i32, cy: i32, r: i32, elong: f64) {
    let ct = unsafe { cos(elong * DEG) };
    for dy in -r..=r {
        let hh = (r * r - dy * dy) as f64;
        if hh < 0.0 { continue; }
        let hw = unsafe { sqrt(hh) } as i32;
        let term = (hw as f64 * ct) as i32;
        let y = cy + dy;
        win_draw_rect(win, cx - hw, y, 2 * hw + 1, 1, 0x00303038);
        let litw = hw - term;
        if litw > 0 { win_draw_rect(win, cx + term, y, litw + 1, 1, 0x00E8E8F0); }
    }
}
// Blit a sphere-mapped body disk (RGBA, alpha=0 outside), scaled to radius r.
// ct is the phase terminator cos: -1.0 = fully lit (planets/Sun), cos(elong) for
// the Moon; pixels on the unlit side are dimmed.
fn blit_body(win: i32, cx: i32, cy: i32, r: i32, tex: &[u8], tdim: i32, ct: f64) {
    if r < 1 { return; }
    let d = 2 * r;
    for dy in 0..=d {
        let ndy = (dy - r) as f64 / r as f64;
        let hh = 1.0 - ndy * ndy;
        let hw = if hh > 0.0 { unsafe { sqrt(hh) } } else { 0.0 };
        let term = hw * ct;
        let ty = (dy * (tdim - 1)) / d;
        for dx in 0..=d {
            let tx = (dx * (tdim - 1)) / d;
            let o = ((ty * tdim + tx) * 4) as usize;
            if tex[o + 3] == 0 { continue; }
            let ndx = (dx - r) as f64 / r as f64;
            let (mut rr, mut gg, mut bb) = (tex[o] as u32, tex[o + 1] as u32, tex[o + 2] as u32);
            if ndx < term { rr = rr * 3 / 10; gg = gg * 3 / 10; bb = bb * 3 / 10; }
            win_draw_rect(win, cx - r + dx, cy - r + dy, 1, 1, (rr << 16) | (gg << 8) | bb);
        }
    }
}
fn draw_line(win: i32, x0: i32, y0: i32, x1: i32, y1: i32, c: u32) {
    let dx = (x1 - x0).abs(); let dy = -(y1 - y0).abs();
    let sx = if x0 < x1 { 1 } else { -1 }; let sy = if y0 < y1 { 1 } else { -1 };
    let (mut x, mut y) = (x0, y0); let mut err = dx + dy; let mut n = 0;
    loop {
        win_draw_rect(win, x, y, 1, 1, c);
        if x == x1 && y == y1 { break; }
        let e2 = 2 * err;
        if e2 >= dy { err += dy; x += sx; }
        if e2 <= dx { err += dx; y += sy; }
        n += 1; if n > 4000 { break; }
    }
}
// Stereographic screen position of an equatorial point, or None if not in view.
fn project(ra: f64, dec: f64, v: &View, g: f64, ccx: i32, ccy: i32, scale: f64, w: i32, h: i32) -> Option<(i32, i32)> {
    let (alt, az) = radec_to_altaz(ra, dec, v.lat, v.lon, g);
    let daz = ang_diff(az, v.caz) * DEG;
    let ca = unsafe { cos(alt * DEG) }; let sa = unsafe { sin(alt * DEG) };
    let cc = unsafe { cos(v.calt * DEG) }; let sc = unsafe { sin(v.calt * DEG) };
    let cosc = sc * sa + cc * ca * unsafe { cos(daz) };
    if cosc <= -0.3 { return None; }
    let kf = 2.0 / (1.0 + cosc);
    let x = kf * ca * unsafe { sin(daz) };
    let y = kf * (cc * sa - sc * ca * unsafe { cos(daz) });
    let px = ccx + (x / DEG * scale) as i32;
    let py = ccy - (y / DEG * scale) as i32;
    if px < -80 || px > w + 80 || py < -80 || py > h + 80 { return None; }
    Some((px, py))
}
fn star_glyph(win: i32, cx: i32, cy: i32, r: i32, color: u32) {
    if r <= 0 { win_draw_rect(win, cx, cy, 1, 1, color); return; }
    for dy in -r..=r {
        let span = ((r * r - dy * dy) as f64).sqrt_() as i32;
        if span >= 0 { win_draw_rect(win, cx - span, cy + dy, 2 * span + 1, 1, color); }
    }
}
trait SqrtExt { fn sqrt_(self) -> f64; }
impl SqrtExt for f64 { fn sqrt_(self) -> f64 { unsafe { sqrt(self) } } }

struct View {
    // observer
    lat: f64, lon: f64,
    // epoch seconds (UT) + a manual offset for time control
    t0: f64, t_off: f64, paused: bool, rate: f64,
    // view centre (alt/az deg) and field-of-view (deg across the height)
    caz: f64, calt: f64, fov: f64,
    // control-bar UI state
    my: i32, loc: usize, magb: f64,
    search_on: bool, sbuf: [u8; 24], slen: usize,
    // last successfully-searched object: the Alpaca "slew/sync to selection"
    // target (Slice B). Set by search_center() for stars, solar-system
    // bodies AND the M42/M31 deep-sky entries.
    has_sel: bool, sel_ra: f64, sel_dec: f64, sel_name: [u8; 16], sel_name_len: usize,
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    let win = win_create(b"Maytera Planetarium\0", 80, 60, 900, 650);
    if win < 0 { return 1; }

    let now = unsafe { syscall1(SYS_TIME, 0) } as f64;
    let mut v = View {
        lat: 51.4779, lon: -0.0015,          // Greenwich default
        t0: if now > 1_000_000.0 { now } else { 1_756_000_000.0 },
        t_off: 0.0, paused: false, rate: 1.0,
        caz: 180.0, calt: 88.0, fov: 150.0,
        my: 0, loc: 0, magb: 0.0, search_on: false, sbuf: [0; 24], slen: 0,
        has_sel: false, sel_ra: 0.0, sel_dec: 0.0, sel_name: [0; 16], sel_name_len: 0,
    };
    let mut ac = alpaca_new();
    // Verification-only: arm the scripted harness if /ALPACA.AUTO exists (never
    // present on the golden). No-op otherwise. See alpaca.rs.
    alpaca_auto_load(&mut ac);

    // --- M1 VALIDATION: alt/az of Sirius/Vega/Polaris for a fixed instant, to
    // serial, so correctness is REPORTED (assessment section 4). 2026-01-01 00:00 UT
    // at Greenwich; JD then GMST then alt/az.
    {
        let jd = julian_date(2026, 1, 1, 0.0);
        let g = gmst_deg(jd);
        let names: [&[u8]; 3] = [b"Sirius", b"Vega", b"Polaris"];
        let idx = [0usize, 1, 2];
        let mut line = [0u8; 96];
        serial(b"[STELLARIUM] validation 2026-01-01 00:00 UT, Greenwich:\n");
        for k in 0..3 {
            let s = &STARS[idx[k]];
            let (alt, az) = radec_to_altaz(s.ra, s.dec, 51.4779, -0.0015, g);
            let mut n = 0;
            for &c in b"  " { line[n] = c; n += 1; }
            for &c in names[k] { line[n] = c; n += 1; }
            for &c in b" alt=" { line[n] = c; n += 1; }
            n = put_f(&mut line, n, alt);
            for &c in b" az=" { line[n] = c; n += 1; }
            n = put_f(&mut line, n, az);
            line[n] = b'\n'; n += 1;
            serial(&line[..n]);
        }
    }

    // Content size. We do NOT call SYS_WIN_GET_SIZE here: in this hardware-float
    // static-PIE build a startup win_get_size() call reliably wedges the process
    // right after this point (validation prints, nothing past here does, the
    // window stays unpainted) - even though the byte-identical call works in the
    // soft-float Task Manager and our own SYS_WIN_GET_EVENT write-back works in
    // the loop below. Root cause not yet isolated (see blame.md); the app does
    // not need it. win_create() asked for 900x650 OUTER, so start from the
    // content estimate and let EVENT_RESIZE (handled in the loop) keep it exact.
    let (mut w, mut h) = (892i32, 618i32);
    let mut ev = GuiEvent { ty: 0, target_id: 0, mouse_x: 0, mouse_y: 0, mouse_buttons: 0, scroll_delta: 0, keycode: 0, key_char: 0 };
    let mut dragging = false;
    let (mut lx, mut ly) = (0i32, 0i32);
    let mut running = true;
    let mut ticks: u64 = 0;

    while running {
        // Drain ALL queued events before rendering: block up to 33ms for the
        // first, then poll the rest non-blocking. A burst of scroll (zoom) is
        // then fully applied even when a wide-FOV frame is slow, instead of one
        // step per slow frame.
        let mut got = win_get_event(win, &mut ev, 33);
        while got > 0 {
            match ev.ty {
                EVENT_WINDOW_CLOSE => running = false,
                EVENT_RESIZE => { if ev.mouse_x > 200 { w = ev.mouse_x; } if ev.mouse_y > 200 { h = ev.mouse_y; } }
                EVENT_KEY_DOWN => {
                    if v.search_on {
                        let ch = ev.key_char;
                        if ch == 13 || ch == 10 { search_center(&mut v); v.search_on = false; }
                        else if ch == 27 { v.search_on = false; v.slen = 0; }
                        else if ch == 8 { if v.slen > 0 { v.slen -= 1; } }
                        else if ch >= 32 && ch < 127 && v.slen < 23 { v.sbuf[v.slen] = ch; v.slen += 1; }
                    } else if ac.entry_on {
                        // Alpaca connect entry: type "ip:port" of the ASCOM Alpaca
                        // telescope's REST endpoint (Slice B), Enter to connect.
                        let ch = ev.key_char;
                        if ch == 13 || ch == 10 {
                            if let Some((ip, port)) = parse_ipport(&ac.ebuf, ac.elen) {
                                ac.ip = ip; ac.port = port;
                                cmd_connect(&mut ac);
                            } else { alpaca_set_status(&mut ac, b"Alpaca: bad ip:port"); }
                            ac.entry_on = false;
                        } else if ch == 27 { ac.entry_on = false; ac.elen = 0; }
                        else if ch == 8 { if ac.elen > 0 { ac.elen -= 1; } }
                        else if ch >= 32 && ch < 127 && ac.elen < 31 { ac.ebuf[ac.elen] = ch; ac.elen += 1; }
                    } else {
                        match ev.key_char {
                            b' ' => v.paused = !v.paused,
                            b'+' | b'=' => v.rate *= 4.0,
                            b'-' | b'_' => v.rate /= 4.0,
                            b'n' | b'N' => { v.t_off = 0.0; v.rate = 1.0; v.paused = false; }
                            b'[' => v.fov = clamp(v.fov * 1.15, 1.0, 170.0),
                            b']' => v.fov = clamp(v.fov / 1.15, 1.0, 170.0),
                            // --- ASCOM Alpaca telescope control panel (#809) ---
                            // Connection + panel (always available).
                            b't' | b'T' => ac.panel_open = !ac.panel_open,
                            b'a' | b'A' => { ac.entry_on = true; ac.elen = 0; }
                            b'c' | b'C' => { if ac.ip != 0 { cmd_connect(&mut ac); } else { ac.entry_on = true; ac.elen = 0; } }
                            b'x' | b'X' => { if ac.connected { cmd_disconnect(&mut ac); } }
                            b'd' | b'D' => disco_start(&mut ac),
                            // Slew / sync / abort.
                            b'g' | b'G' => {
                                if ac.connected && v.has_sel { cmd_goto(&mut ac, v.sel_ra, v.sel_dec); }
                                else { alpaca_set_status(&mut ac, b"Connect + select an object first"); }
                            }
                            b'y' | b'Y' => {
                                if ac.connected && v.has_sel { cmd_sync(&mut ac, v.sel_ra, v.sel_dec); }
                                else { alpaca_set_status(&mut ac, b"Connect + select an object first"); }
                            }
                            b'b' | b'B' => { if ac.connected { cmd_abort(&mut ac); } }
                            // Jog N/S/E/W (axis 0 = primary/RA-Az, 1 = secondary/Dec-Alt).
                            b'i' | b'I' => { if ac.connected { cmd_jog(&mut ac, 1, 1.0); } }
                            b'k' | b'K' => { if ac.connected { cmd_jog(&mut ac, 1, -1.0); } }
                            b'l' | b'L' => { if ac.connected { cmd_jog(&mut ac, 0, 1.0); } }
                            b'j' | b'J' => { if ac.connected { cmd_jog(&mut ac, 0, -1.0); } }
                            b'.' | b'>' => { if ac.connected { cmd_jog_stop(&mut ac); } }
                            b'r' | b'R' => cmd_jog_rate_cycle(&mut ac),
                            // Tracking on/off + rate (1 sidereal / 2 lunar / 3 solar).
                            b'o' | b'O' => { if ac.connected { cmd_tracking_toggle(&mut ac); } }
                            b'1' => { if ac.connected { cmd_tracking_rate(&mut ac, 0); } }
                            b'2' => { if ac.connected { cmd_tracking_rate(&mut ac, 1); } }
                            b'3' => { if ac.connected { cmd_tracking_rate(&mut ac, 2); } }
                            // Park / unpark / find home.
                            b'p' | b'P' => { if ac.connected { cmd_park(&mut ac); } }
                            b'u' | b'U' => { if ac.connected { cmd_unpark(&mut ac); } }
                            b'h' | b'H' => { if ac.connected { cmd_findhome(&mut ac); } }
                            _ => {}
                        }
                    }
                }
                EVENT_MOUSE_DOWN => {
                    v.my = ev.mouse_y;
                    if !bar_click(ev.mouse_x, ev.mouse_y, w, h, &mut v) {
                        dragging = true; lx = ev.mouse_x; ly = ev.mouse_y;
                    }
                }
                EVENT_MOUSE_UP => dragging = false,
                EVENT_MOUSE_MOVE => {
                    v.my = ev.mouse_y;
                    if dragging {
                        let s = v.fov / h as f64;
                        v.caz = fmodp(v.caz - (ev.mouse_x - lx) as f64 * s, 360.0);
                        v.calt = clamp(v.calt + (ev.mouse_y - ly) as f64 * s, -20.0, 90.0);
                        lx = ev.mouse_x; ly = ev.mouse_y;
                    }
                }
                EVENT_MOUSE_SCROLL => {
                    if ev.scroll_delta > 0 { v.fov = clamp(v.fov / 1.2, 1.0, 170.0); }
                    else { v.fov = clamp(v.fov * 1.2, 1.0, 170.0); }
                }
                _ => {}
            }
            if !running { break; }
            got = win_get_event(win, &mut ev, 0);
        }
        if !running { break; }
        if !v.paused { v.t_off += v.rate * 0.033; }
        // Alpaca: at most one non-blocking syscall's worth of I/O per frame
        // (never a sleep loop - see alpaca.rs). advance the in-flight op, start
        // the next queued op, roll the live-status poll, service discovery, and
        // drive the verification harness - each strictly non-blocking so a slow
        // or absent mount can never freeze the sky render.
        alpaca_step(&mut ac);
        alpaca_pump(&mut ac);
        alpaca_poll_tick(&mut ac);
        disco_step(&mut ac);
        alpaca_auto_tick(&mut ac);
        render(win, w, h, &v, &ac);
        win_invalidate(win);
        ticks = ticks.wrapping_add(1);
        let _ = ticks;
    }
    unsafe { syscall1(SYS_EXIT, 0) };
    0
}

static LOCATIONS: &[(&[u8], f64, f64)] = &[
    (b"London\0", 51.5074, -0.1278), (b"New York\0", 40.7128, -74.0060),
    (b"Tokyo\0", 35.6762, 139.6503), (b"Sydney\0", -33.8688, 151.2093),
    (b"Cape Town\0", -33.9249, 18.4241), (b"Reykjavik\0", 64.1466, -21.9426),
    (b"Nairobi\0", -1.2921, 36.8219), (b"Equator 0\0", 0.0, 0.0),
];

// UTC civil date/time from epoch seconds (Howard Hinnant's algorithm).
fn civil_from_epoch(secs: f64) -> (i64, i64, i64, i64, i64) {
    let days = unsafe { floor(secs / 86400.0) } as i64;
    let rem = (secs as i64) - days * 86400;
    let hh = rem / 3600; let mm = (rem % 3600) / 60;
    let z = days + 719468;
    let era = (if z >= 0 { z } else { z - 146096 }) / 146097;
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let yy = if m <= 2 { y + 1 } else { y };
    (yy, m, d, hh, mm)
}

// Draw one control-bar button (glass), return its click rect for hit-testing.
fn barbtn(win: i32, x: i32, y: i32, bw: i32, label: &[u8], hot: bool) {
    let bg = if hot { 0x001D3A33 } else { 0x00122420 };
    win_draw_rect(win, x, y, bw, 24, bg);
    win_draw_rect(win, x, y, bw, 1, 0x002C4A44);
    win_draw_rect(win, x, y + 23, bw, 1, 0x000A1614);
    // centre the (short) label roughly; TTF is proportional, so just inset.
    win_text(win, x + 5, y + 4, label, 0x00E8F4F0);
}
fn hit(mx: i32, my: i32, x: i32, y: i32, bw: i32, bh: i32) -> bool {
    mx >= x && mx < x + bw && my >= y && my < y + bh
}
// Shared control-bar button layout (draw + hit-test read the SAME rects).
// 0 Rev 1 Slow 2 Pause 3 Fast 4 Now 5 -d 6 -h 7 +h 8 +d 9 Search 10 Loc 11 Mag- 12 Mag+ 13 Zoom- 14 Zoom+
fn bar_rect(idx: usize, w: i32, h: i32) -> (i32, i32, i32) {
    let by = h - 30;
    match idx {
        0 => (6, by, 30), 1 => (40, by, 24), 2 => (68, by, 48), 3 => (120, by, 24),
        4 => (148, by, 40), 5 => (192, by, 26), 6 => (222, by, 26), 7 => (252, by, 26), 8 => (282, by, 26),
        9 => (w - 392, by, 144), 10 => (w - 242, by, 94),
        11 => (w - 142, by, 30), 12 => (w - 108, by, 30), 13 => (w - 68, by, 30), 14 => (w - 34, by, 30),
        _ => (0, by, 0),
    }
}
fn fmt2(buf: &mut [u8], mut n: usize, v: i64, width: usize) -> usize {
    let mut tmp = [0u8; 8]; let mut t = 0; let mut x = if v < 0 { 0 } else { v };
    if x == 0 { tmp[0] = b'0'; t = 1; } else { while x > 0 { tmp[t] = b'0' + (x % 10) as u8; x /= 10; t += 1; } }
    while t < width { tmp[t] = b'0'; t += 1; }
    while t > 0 { t -= 1; buf[n] = tmp[t]; n += 1; }
    n
}
// Record the object a search just centred on as the Alpaca slew/sync target
// (Slice B: "let the user slew/sync the telescope to whatever object is
// currently selected in the planetarium UI").
fn set_selection(v: &mut View, ra: f64, dec: f64, name: &[u8]) {
    v.has_sel = true; v.sel_ra = ra; v.sel_dec = dec;
    let n = if name.len() > v.sel_name.len() { v.sel_name.len() } else { name.len() };
    v.sel_name[..n].copy_from_slice(&name[..n]);
    v.sel_name_len = n;
}
fn search_center(v: &mut View) {
    let g = gmst_deg(julian_date_from_epoch(v.t0 + v.t_off));
    for &(nm, ra, dec, _m) in NAMED_STARS.iter() {
        if name_ci_prefix(nm, &v.sbuf, v.slen) {
            let (alt, az) = radec_to_altaz(ra, dec, v.lat, v.lon, g);
            v.calt = clamp(alt, -20.0, 90.0); v.caz = az; v.fov = 20.0;
            set_selection(v, ra, dec, nm); v.slen = 0; return;
        }
    }
    let dday = epoch_to_d(v.t0 + v.t_off);
    let (_sr, _sd, _sl, xs, ys) = sun_full(dday);
    let bodies: [(&[u8], usize); 9] = [
        (b"Sun\0", 100), (b"Mercury\0", 0), (b"Venus\0", 1), (b"Mars\0", 2),
        (b"Jupiter\0", 3), (b"Saturn\0", 4), (b"Uranus\0", 5), (b"Neptune\0", 6), (b"Moon\0", 200)];
    for &(nm, id) in bodies.iter() {
        if name_ci_prefix(nm, &v.sbuf, v.slen) {
            let (ra, dec) = if id == 100 { (_sr, _sd) }
                else if id == 200 { let (r, d, _l) = moon_full(dday); (r, d) }
                else { let (r, d, _dist) = planet_radec(id, dday, xs, ys); (r, d) };
            let (alt, az) = radec_to_altaz(ra, dec, v.lat, v.lon, g);
            v.calt = clamp(alt, -20.0, 90.0); v.caz = az; v.fov = 15.0;
            set_selection(v, ra, dec, nm); v.slen = 0; return;
        }
    }
    if let Some((ra, dec, nm)) = deep_sky_lookup(&v.sbuf, v.slen) {
        let (alt, az) = radec_to_altaz(ra, dec, v.lat, v.lon, g);
        v.calt = clamp(alt, -20.0, 90.0); v.caz = az; v.fov = 20.0;
        set_selection(v, ra, dec, nm); v.slen = 0; return;
    }
    v.slen = 0;
}
fn bar_click(mx: i32, my: i32, w: i32, h: i32, v: &mut View) -> bool {
    if my < h - 40 { return false; }
    let mut idx = 0;
    while idx < 15 {
        let (bx, byy, bw) = bar_rect(idx, w, h);
        if bw > 0 && hit(mx, my, bx, byy, bw, 24) {
            match idx {
                0 => v.rate = -v.rate,
                1 => { v.rate /= 4.0; if v.rate.abs() < 0.02 { v.rate = if v.rate < 0.0 { -0.02 } else { 0.02 }; } }
                2 => v.paused = !v.paused,
                3 => v.rate *= 4.0,
                4 => { v.t_off = 0.0; v.rate = 1.0; v.paused = false; }
                5 => v.t_off -= 86400.0,
                6 => v.t_off -= 3600.0,
                7 => v.t_off += 3600.0,
                8 => v.t_off += 86400.0,
                9 => { v.search_on = true; v.slen = 0; }
                10 => { v.loc = (v.loc + 1) % LOCATIONS.len(); v.lat = LOCATIONS[v.loc].1; v.lon = LOCATIONS[v.loc].2; }
                11 => v.magb = clamp(v.magb - 0.5, -3.0, 4.0),
                12 => v.magb = clamp(v.magb + 0.5, -3.0, 4.0),
                13 => v.fov = clamp(v.fov * 1.3, 1.0, 170.0),
                14 => v.fov = clamp(v.fov / 1.3, 1.0, 170.0),
                _ => {}
            }
            return true;
        }
        idx += 1;
    }
    true
}

// Case-insensitive equal-ignoring-case for ASCII, over a NUL-terminated name.
fn name_ci_prefix(name: &[u8], q: &[u8], qn: usize) -> bool {
    if qn == 0 { return false; }
    let mut i = 0;
    while i < qn && i < name.len() && name[i] != 0 {
        let a = name[i]; let b = q[i];
        let al = if a >= b'A' && a <= b'Z' { a + 32 } else { a };
        let bl = if b >= b'A' && b <= b'Z' { b + 32 } else { b };
        if al != bl { return false; }
        i += 1;
    }
    i == qn
}

fn render(win: i32, w: i32, h: i32, v: &View, ac: &Alpaca) {
    // sky background: darker toward the top, a horizon glow near the bottom edge
    win_draw_rect(win, 0, 0, w, h, 0x00040814);
    // ground
    let horizon_y = h / 2 + ((v.calt / v.fov) * h as f64) as i32;
    if horizon_y < h {
        let gy = if horizon_y < 0 { 0 } else { horizon_y };
        win_draw_rect(win, 0, gy, w, h - gy, 0x000A0E0A);
        win_draw_rect(win, 0, gy, w, 1, 0x00203028);
    }

    let jd = julian_date_from_epoch(v.t0 + v.t_off);
    let g = gmst_deg(jd);
    let scale = h as f64 / v.fov;            // pixels per degree at centre
    let (ccx, ccy) = (w / 2, h / 2);

    // Real Milky Way panorama (Slice A), galactic-plane-correct via the true
    // equatorial<->galactic rotation - see deepsky.rs. Drawn first so stars,
    // constellations and bodies all sit on top of it.
    draw_milkyway(win, w, h, v, g, ccx, ccy, scale);

    // Constellation stick figures (faint), drawn under the stars.
    for &(r1, d1, r2, d2) in CONSTELLATION_SEGS.iter() {
        if let (Some((ax, ay)), Some((bx, by))) =
            (project(r1, d1, v, g, ccx, ccy, scale, w, h),
             project(r2, d2, v, g, ccx, ccy, scale, w, h)) {
            draw_line(win, ax, ay, bx, by, 0x00203848);
        }
    }

    // Only draw stars brighter than a limit that scales with zoom: bright-only
    // when wide (fast, uncluttered), fainter as you zoom in (more detail). The
    // catalogue is sorted brightest-first, so break as soon as we pass the limit.
    let mag_limit = 3.0 + 3.6 * ((160.0 - v.fov) / 155.0) + v.magb;
    for s in HIP_STARS.iter() {
        if s.mag > mag_limit { break; }
        let (alt, az) = radec_to_altaz(s.ra, s.dec, v.lat, v.lon, g);
        if alt < -2.0 { continue; }
        // gnomonic-ish tangent projection about (calt, caz)
        let daz = ang_diff(az, v.caz) * DEG;
        let dalt = (alt - v.calt) * DEG;
        let (ca, sa) = (unsafe { cos(alt * DEG) }, unsafe { sin(alt * DEG) });
        let cc = unsafe { cos(v.calt * DEG) };
        let sc = unsafe { sin(v.calt * DEG) };
        let cosc = sc * sa + cc * ca * unsafe { cos(daz) };
        if cosc <= -0.92 { continue; }       // opposite hemisphere (>~157 deg away)
        // Stereographic (not gnomonic): finite out to ~180 deg, so a wide field
        // shows a full sky instead of flinging everything past ~60 deg off-screen.
        let kf = 2.0 / (1.0 + cosc);
        let x = kf * ca * unsafe { sin(daz) };
        let y = kf * (cc * sa - sc * ca * unsafe { cos(daz) });
        let _ = (dalt,);
        let px = ccx + (x / DEG * scale) as i32;
        let py = ccy - (y / DEG * scale) as i32;
        if px < -4 || px > w + 4 || py < -4 || py > h + 4 { continue; }
        // magnitude -> radius (brighter = bigger); dim stars are single pixels
        let rad = (3.2 - s.mag * 0.7) as i32;
        let rr = if rad < 0 { 0 } else if rad > 5 { 5 } else { rad };
        star_glyph(win, px, py, rr, bv_color(s.bv));
        if rr >= 3 { star_glyph(win, px, py, rr + 2, dim(bv_color(s.bv))); } // faint halo
    }

    // Labels for the brightest named stars (only when comfortably in view).
    let label_limit = 1.4 + 4.8 * ((160.0 - v.fov) / 155.0) + v.magb;
    for &(nm, ra, dec, mag) in NAMED_STARS.iter() {
        if mag > label_limit { break; }
        let (alt, az) = radec_to_altaz(ra, dec, v.lat, v.lon, g);
        if alt < 0.0 { continue; }
        let daz = ang_diff(az, v.caz) * DEG;
        let ca = unsafe { cos(alt * DEG) }; let sa = unsafe { sin(alt * DEG) };
        let cc = unsafe { cos(v.calt * DEG) }; let sc = unsafe { sin(v.calt * DEG) };
        let cosc = sc * sa + cc * ca * unsafe { cos(daz) };
        if cosc <= -0.4 { continue; }
        let kf = 2.0 / (1.0 + cosc);
        let x = kf * ca * unsafe { sin(daz) };
        let y = kf * (cc * sa - sc * ca * unsafe { cos(daz) });
        let px = ccx + (x / DEG * scale) as i32;
        let py = ccy - (y / DEG * scale) as i32;
        if px < 24 || px > w - 48 || py < 12 || py > h - 12 { continue; }
        win_text(win, px + 6, py - 4, nm, 0x009FCFC0);
    }

    // Solar-system bodies (Schlyter ephemeris): Sun, planets, Moon, drawn on top.
    let dday = epoch_to_d(v.t0 + v.t_off);
    let (sra, sdec, slon, xs, ys) = sun_full(dday);
    let zf = 90.0 / v.fov;
    let sr = (5.0 + 5.0 * zf) as i32;
    if let Some((px, py)) = project(sra, sdec, v, g, ccx, ccy, scale, w, h) {
        if sr >= 5 { blit_body(win, px, py, sr.min(48), &TEX_SUN, TEX_DIM, -1.0); }
        else { fill_disk(win, px, py, if sr < 2 { 2 } else { sr }, 0x00FFF2C8); }
        win_text(win, px + sr.min(48) + 4, py - 4, b"Sun\0", 0x00FFE070);
    }
    let pnames: [&[u8]; 7] = [b"Mercury\0", b"Venus\0", b"Mars\0", b"Jupiter\0", b"Saturn\0", b"Uranus\0", b"Neptune\0"];
    let pcol: [u32; 7] = [0x00B0A090, 0x00EAE2C0, 0x00E06038, 0x00D8B888, 0x00E8DAA0, 0x00A8D8E8, 0x005878E0];
    let ptex: [&[u8]; 7] = [&TEX_MERCURY, &TEX_VENUS, &TEX_MARS, &TEX_JUPITER, &TEX_SATURN, &TEX_URANUS, &TEX_NEPTUNE];
    let pbase: [f64; 7] = [1.0, 2.0, 2.0, 3.0, 2.5, 1.5, 1.5];
    let pzoom: [f64; 7] = [0.8, 2.0, 1.6, 3.0, 2.2, 0.8, 0.6];
    for i in 0..7 {
        let (pra, pdec, _dist) = planet_radec(i, dday, xs, ys);
        let rr = (pbase[i] + pzoom[i] * zf) as i32;
        if let Some((px, py)) = project(pra, pdec, v, g, ccx, ccy, scale, w, h) {
            // Saturn: a tilted ring ellipse around the globe (drawn first, so the
            // globe sits on top of the back arc). Simple single band.
            if i == 4 && rr >= 6 {
                let rd = rr.min(48) as f64;
                let flat = 0.42;
                let mut t = 0;
                while t < 128 {
                    let a = (t as f64 / 128.0) * 6.2831853;
                    let ca = unsafe { cos(a) }; let sa = unsafe { sin(a) };
                    let ex = px + (rd * 2.30 * ca) as i32;
                    let ey = py + (rd * 2.30 * flat * sa) as i32;
                    let ix = px + (rd * 1.45 * ca) as i32;
                    let iy = py + (rd * 1.45 * flat * sa) as i32;
                    draw_line(win, ix, iy, ex, ey, 0x00C8B080);
                    t += 1;
                }
            }
            if rr >= 5 { blit_body(win, px, py, rr.min(48), ptex[i], TEX_DIM, -1.0); }
            else { fill_disk(win, px, py, if rr < 1 { 1 } else { rr }, pcol[i]); }
            win_text(win, px + (if rr >= 5 { rr.min(48) } else { rr }) + 3, py - 4, pnames[i], pcol[i]);
        }
    }
    // Galilean moons: simplified circular orbits (correct periods, approx phase),
    // offset from Jupiter roughly along the ecliptic. Visible/labelled when the
    // view is zoomed in enough to separate them from the disk.
    {
        let (jra, jdec, _jd) = planet_radec(3, dday, xs, ys);
        let cjd = { let c0 = unsafe { cos(jdec * DEG) }; if c0.abs() < 0.02 { 0.02 } else { c0 } };
        let gm: [(&[u8], f64, f64); 4] = [
            (b"Io\0", 1.769, 2.32), (b"Europa\0", 3.551, 3.70),
            (b"Ganymede\0", 7.155, 5.90), (b"Callisto\0", 16.689, 10.37),
        ];
        for (k, &(nm, per, emax)) in gm.iter().enumerate() {
            let phase = (dday / per) * 360.0 + (k as f64) * 61.0;
            let elong_deg = emax * unsafe { sin(phase * DEG) } / 60.0;
            let mmra = jra + elong_deg / cjd;
            if let Some((px, py)) = project(mmra, jdec, v, g, ccx, ccy, scale, w, h) {
                fill_disk(win, px, py, 1, 0x00E0D8C8);
                if v.fov < 6.0 { win_text(win, px + 3, py - 5, nm, 0x0090C0B0); }
            }
        }
    }

    let (mra, mdec, mlon) = moon_full(dday);
    if let Some((px, py)) = project(mra, mdec, v, g, ccx, ccy, scale, w, h) {
        let e0 = fmodp(mlon - slon, 360.0);
        let elong = if e0 > 180.0 { 360.0 - e0 } else { e0 };
        let mr = (5.0 + 5.0 * zf) as i32;
        let ct = unsafe { cos(elong * DEG) };
        if mr >= 5 { blit_body(win, px, py, mr.min(48), &TEX_MOON, TEX_DIM, ct); }
        else { draw_moon(win, px, py, if mr < 2 { 2 } else { mr }, elong); }
        win_text(win, px + mr.min(48) + 4, py - 4, b"Moon\0", 0x00E8E8F0);
    }

    // Marquee deep-sky objects (Slice A): M42 Orion Nebula, M31 Andromeda
    // Galaxy, real Hubble/ESA imagery at their real J2000 coordinates.
    draw_deep_sky(win, w, h, v, g, ccx, ccy, scale, zf);

    // Alpaca telescope reticle (Slice B): where the real mount is actually
    // pointed right now, from the last successful GET rightascension/
    // declination poll. A simple crosshair, distinct colour from everything
    // else on screen.
    if ac.have_pos {
        if let Some((px, py)) = project(ac.scope_ra, ac.scope_dec, v, g, ccx, ccy, scale, w, h) {
            let rr = 10;
            win_draw_rect(win, px - rr, py, 2 * rr + 1, 1, 0x00FF4030);
            win_draw_rect(win, px, py - rr, 1, 2 * rr + 1, 0x00FF4030);
            win_text(win, px + rr + 3, py - 4, b"Scope\0", 0x00FF4030);
        }
    }

    // cardinal marks at the horizon (N/E/S/W by azimuth) as small ticks
    for (a, _c) in [(0.0, 0u8), (90.0, 0), (180.0, 0), (270.0, 0)] {
        let daz = ang_diff(a, v.caz);
        if daz.abs() < v.fov * 0.6 {
            let px = ccx + (daz * scale) as i32;
            let hy = h / 2 + ((v.calt / v.fov) * h as f64) as i32;
            if px >= 0 && px < w && hy >= 0 && hy < h {
                win_draw_rect(win, px, hy - 6, 2, 12, 0x006AE2CF);
            }
        }
    }

    // ---- Alpaca host-entry box (modal input; top-left, always shown while
    // typing an ip:port). The full telescope status + controls live in the
    // right-docked panel (panel_render). When the panel is hidden, a one-line
    // status is still shown top-left so a slew/sync result is never missed. ----
    if ac.entry_on {
        win_draw_rect(win, 6, 6, 260, 24, 0x00213B34);
        win_draw_rect(win, 6, 6, 260, 1, 0x006AE2CF);
        let mut buf = [0u8; 40]; let mut n = 0;
        n = cat(&mut buf, n, b"Alpaca host:port> ");
        for i in 0..ac.elen { buf[n] = ac.ebuf[i]; n += 1; }
        buf[n] = 0; n += 1;
        win_text(win, 12, 10, &buf[..n], 0x00F3FBF9);
    } else if !ac.panel_open && ac.status_len > 0 {
        let mut buf = [0u8; 52]; let mut n = 0;
        n = cat(&mut buf, n, b"Telescope: ");
        for i in 0..ac.status_len { buf[n] = ac.status[i]; n += 1; }
        buf[n] = 0; n += 1;
        win_text(win, 12, 10, &buf[..n], 0x0090D0C0);
        win_text(win, 12, 26, b"[T] show telescope panel\0", 0x00607068);
    }

    // ---- Telescope control panel (right-docked glass; see design spec) ----
    if ac.panel_open { panel_render(win, w, h, v, ac); }

    // ---- Control bar (hover the bottom edge to reveal) ----
    if v.my >= h - 40 {
        let by = h - 30;
        win_draw_rect(win, 0, by - 5, w, 35, 0x000E1D1B);
        win_draw_rect(win, 0, by - 5, w, 1, 0x002C4A44);
        let paused = v.paused;
        let labels: [&[u8]; 15] = [b"Rev\0", b"-\0", if paused { b"Play\0" } else { b"Pause\0" }, b"+\0",
            b"Now\0", b"-d\0", b"-h\0", b"+h\0", b"+d\0", b"\0", b"\0", b"Mag-\0", b"Mag+\0", b"Zm-\0", b"Zm+\0"];
        let mut idx = 0;
        while idx < 15 {
            let (bx, byy, bw) = bar_rect(idx, w, h);
            if bw <= 0 { idx += 1; continue; }
            if idx == 9 {
                if v.search_on {
                    win_draw_rect(win, bx, byy, bw, 24, 0x00213B34);
                    win_draw_rect(win, bx, byy, bw, 1, 0x006AE2CF);
                    let mut sbf = [0u8; 28]; let mut i = 0; while i < v.slen { sbf[i] = v.sbuf[i]; i += 1; } sbf[i] = 0;
                    win_text(win, bx + 5, byy + 4, &sbf[..i + 1], 0x00F3FBF9);
                } else { barbtn(win, bx, byy, bw, b"Search...\0", false); }
            } else if idx == 10 {
                barbtn(win, bx, byy, bw, LOCATIONS[v.loc % LOCATIONS.len()].0, false);
            } else {
                barbtn(win, bx, byy, bw, labels[idx], (idx == 2 && paused) || (idx == 0 && v.rate < 0.0));
            }
            idx += 1;
        }
        let (yy, mo, dd, hh, mn) = civil_from_epoch(v.t0 + v.t_off);
        let mut b = [0u8; 56]; let mut n = 0;
        n = fmt2(&mut b, n, yy, 4); b[n] = b'-'; n += 1; n = fmt2(&mut b, n, mo, 2); b[n] = b'-'; n += 1; n = fmt2(&mut b, n, dd, 2);
        b[n] = b' '; n += 1; n = fmt2(&mut b, n, hh, 2); b[n] = b':'; n += 1; n = fmt2(&mut b, n, mn, 2);
        for &cc in b" UT  " { b[n] = cc; n += 1; }
        if paused { for &cc in b"paused" { b[n] = cc; n += 1; } }
        else if v.rate < 0.0 { for &cc in b"rev x" { b[n] = cc; n += 1; } n = put_i(&mut b, n, (-v.rate) as i64); }
        else { b[n] = b'x'; n += 1; n = put_i(&mut b, n, v.rate as i64); }
        b[n] = 0; n += 1;
        win_text(win, 316, by + 4, &b[..n], 0x00A9D9CC);
    }
}

// ============================================================================
// Telescope control panel (right-docked glass). Geometry + tokens are the port
// of design/alpaca-panel.html; see that file for the reviewable spec. Every
// control here is also a global key (handled in the main loop), because QEMU
// pointer injection is unreliable (#334) so the panel must be fully
// keyboard-drivable. Buttons draw their key hint and gray out (disabled token)
// when the mount reports it cannot do the action (CanX gating).
// ----------------------------------------------------------------------------
const PANEL_W: i32 = 296;
// Button states.
const BS_IDLE: u8 = 0; const BS_HOT: u8 = 1; const BS_DIS: u8 = 2;

fn pbtn(win: i32, x: i32, y: i32, bw: i32, key: u8, label: &[u8], state: u8) {
    let (bg, ktxt, ltxt, top) = match state {
        BS_DIS => (0x000C1512u32, 0x00435853u32, 0x00435853u32, 0x00162622u32),
        BS_HOT => (0x001D3A33u32, 0x006AE2CFu32, 0x00E8F4F0u32, 0x002C4A44u32),
        _ =>      (0x00122420u32, 0x006AE2CFu32, 0x00E8F4F0u32, 0x002C4A44u32),
    };
    win_draw_rect(win, x, y, bw, 22, bg);
    win_draw_rect(win, x, y, bw, 1, top);
    win_draw_rect(win, x, y + 21, bw, 1, 0x000A1614);
    let kb = [b'[', key, b']', 0u8];
    win_text(win, x + 5, y + 3, &kb, ktxt);
    win_text(win, x + 22, y + 3, label, ltxt);
}
// A label/value row. Both slices must be NUL-terminated (TTF path).
fn prow(win: i32, x: i32, y: i32, label: &[u8], value: &[u8], vcol: u32) {
    win_text(win, x, y, label, 0x00A9D9CC);
    win_text(win, x + 92, y, value, vcol);
}
// Format v to 2 decimals with a NUL-terminated suffix into a fresh buffer and
// draw it as a value at (x+92, y).
fn prow_f(win: i32, x: i32, y: i32, label: &[u8], v: f64, suffix: &[u8], vcol: u32) {
    let mut b = [0u8; 24]; let mut n = 0;
    n = put_f(&mut b, n, v);
    n = cat_l(&mut b, n, suffix);
    b[n] = 0; n += 1;
    prow(win, x, y, label, &b[..n], vcol);
}
// local cat (alpaca.rs's cat is in scope, but keep panel self-contained).
fn cat_l(buf: &mut [u8], mut n: usize, s: &[u8]) -> usize { for &c in s { if n < buf.len() { buf[n] = c; n += 1; } } n }

fn panel_render(win: i32, w: i32, h: i32, v: &View, ac: &Alpaca) {
    let mut x0 = w - PANEL_W;
    if x0 < 0 { x0 = 0; }
    let cx = x0 + 12;
    // panel ground + left accent edge
    win_draw_rect(win, x0, 0, PANEL_W, h, 0x000B1A18);
    win_draw_rect(win, x0, 0, 1, h, 0x006AE2CF);

    win_text(win, cx, 8, b"Telescope Control\0", 0x006AE2CF);
    win_text(win, x0 + PANEL_W - 62, 10, b"[T] hide\0", 0x00709088);
    win_draw_rect(win, cx, 30, PANEL_W - 24, 1, 0x001B302C);

    // ---- Connection ----
    win_text(win, cx, 36, b"CONNECTION\0", 0x0067B8A6);
    // host row
    {
        let mut b = [0u8; 28]; let mut n = 0;
        if ac.ip != 0 {
            n = put_iu(&mut b, n, (ac.ip >> 24) & 0xFF); b[n] = b'.'; n += 1;
            n = put_iu(&mut b, n, (ac.ip >> 16) & 0xFF); b[n] = b'.'; n += 1;
            n = put_iu(&mut b, n, (ac.ip >> 8) & 0xFF); b[n] = b'.'; n += 1;
            n = put_iu(&mut b, n, ac.ip & 0xFF); b[n] = b':'; n += 1;
            n = put_i(&mut b, n, ac.port as i64);
        } else { n = cat_l(&mut b, n, b"(unset)"); }
        b[n] = 0; n += 1;
        prow(win, cx, 52, b"Host\0", &b[..n], 0x00F3FBF9);
    }
    // connection buttons: A edit / C connect / X disconnect / D discover
    pbtn(win, cx,       72, 64, b'A', b"Edit\0", BS_IDLE);
    pbtn(win, cx + 68,  72, 64, b'C', b"Conn\0", if ac.connected { BS_HOT } else { BS_IDLE });
    pbtn(win, cx + 136, 72, 64, b'X', b"Disc\0", if ac.connected { BS_IDLE } else { BS_DIS });
    pbtn(win, cx + 204, 72, 64, b'D', b"Scan\0", BS_IDLE);
    // status dot + text
    {
        let dot = if ac.connected { 0x0058E28Au32 } else { 0x00709088u32 };
        win_draw_rect(win, cx, 102, 8, 8, dot);
        let mut b = [0u8; 52]; let mut n = 0;
        for i in 0..ac.status_len { b[n] = ac.status[i]; n += 1; }
        if ac.status_len == 0 { n = cat_l(&mut b, n, b"Disconnected"); }
        b[n] = 0; n += 1;
        win_text(win, cx + 14, 100, &b[..n], if ac.connected { 0x00E8F4F0 } else { 0x0090D0C0 });
    }
    // mount name
    {
        let mut b = [0u8; 44]; let mut n = 0;
        if ac.name_len > 0 { for i in 0..ac.name_len { b[n] = ac.name[i]; n += 1; } }
        else { n = cat_l(&mut b, n, b"--"); }
        b[n] = 0; n += 1;
        prow(win, cx, 118, b"Mount\0", &b[..n], 0x00C7E7DE);
    }
    win_draw_rect(win, cx, 138, PANEL_W - 24, 1, 0x001B302C);

    // ---- Live status ----
    win_text(win, cx, 144, b"LIVE STATUS\0", 0x0067B8A6);
    if ac.have_pos {
        prow_f(win, cx, 160, b"RA (h)\0", ac.scope_ra / 15.0, b"", 0x00F3FBF9);
        prow_f(win, cx, 176, b"Dec\0", ac.scope_dec, b" deg", 0x00F3FBF9);
    } else {
        prow(win, cx, 160, b"RA (h)\0", b"--\0", 0x00709088);
        prow(win, cx, 176, b"Dec\0", b"--\0", 0x00709088);
    }
    if ac.have_altaz {
        prow_f(win, cx, 192, b"Alt\0", ac.scope_alt, b" deg", 0x00F3FBF9);
        prow_f(win, cx, 208, b"Az\0", ac.scope_az, b" deg", 0x00F3FBF9);
    } else {
        prow(win, cx, 192, b"Alt\0", b"--\0", 0x00709088);
        prow(win, cx, 208, b"Az\0", b"--\0", 0x00709088);
    }
    // slewing indicator
    if ac.slewing { prow(win, cx, 224, b"Slewing\0", b"SLEWING\0", 0x00FFC24A); }
    else { prow(win, cx, 224, b"Slewing\0", b"idle\0", 0x00709088); }
    // tracking + rate
    {
        let rates: [&[u8]; 3] = [b"sidereal\0", b"lunar\0", b"solar\0"];
        if ac.tracking {
            let rn = (ac.track_rate as usize) % 3;
            let mut b = [0u8; 20]; let mut n = 0;
            n = cat_l(&mut b, n, b"ON ");
            let r = rates[rn]; let mut i = 0; while r[i] != 0 { b[n] = r[i]; n += 1; i += 1; }
            b[n] = 0; n += 1;
            prow(win, cx, 240, b"Tracking\0", &b[..n], 0x0058E28A);
        } else { prow(win, cx, 240, b"Tracking\0", b"off\0", 0x00709088); }
    }
    // current target
    if v.has_sel {
        let mut b = [0u8; 20]; let mut n = 0;
        for i in 0..v.sel_name_len { let c = v.sel_name[i]; if c == 0 { break; } b[n] = c; n += 1; }
        b[n] = 0; n += 1;
        prow(win, cx, 256, b"Target\0", &b[..n], 0x00C7E7DE);
    } else { prow(win, cx, 256, b"Target\0", b"(none)\0", 0x00709088); }
    win_draw_rect(win, cx, 276, PANEL_W - 24, 1, 0x001B302C);

    // ---- Slew / Sync ----
    win_text(win, cx, 282, b"SLEW / SYNC\0", 0x0067B8A6);
    let goto_st = if ac.connected && v.has_sel && ac.can_slew { BS_IDLE } else { BS_DIS };
    let sync_st = if ac.connected && v.has_sel && ac.can_sync { BS_IDLE } else { BS_DIS };
    let abrt_st = if !ac.connected { BS_DIS } else if ac.slewing { BS_HOT } else { BS_IDLE };
    pbtn(win, cx,       298, 84, b'G', b"Goto\0", goto_st);
    pbtn(win, cx + 90,  298, 84, b'Y', b"Sync\0", sync_st);
    pbtn(win, cx + 180, 298, 84, b'B', b"Abort\0", abrt_st);

    // ---- Jog ----
    {
        let mut b = [0u8; 20]; let mut n = 0;
        n = cat_l(&mut b, n, b"JOG   [R] ");
        n = put_f(&mut b, n, JOG_RATES[ac.jog_rate_idx % JOG_RATES.len()]);
        n = cat_l(&mut b, n, b"/s"); b[n] = 0; n += 1;
        win_text(win, cx, 326, &b[..n], 0x0067B8A6);
    }
    let jst = if ac.connected && ac.can_moveaxis { BS_IDLE } else { BS_DIS };
    pbtn(win, cx + 69,  344, 64, b'I', b"N\0", jst);
    pbtn(win, cx,       370, 64, b'J', b"W\0", jst);
    pbtn(win, cx + 69,  370, 64, b'.', b"Stop\0", if ac.connected { BS_IDLE } else { BS_DIS });
    pbtn(win, cx + 138, 370, 64, b'L', b"E\0", jst);
    pbtn(win, cx + 69,  396, 64, b'K', b"S\0", jst);

    // ---- Tracking ----
    win_text(win, cx, 426, b"TRACKING\0", 0x0067B8A6);
    let tst = if ac.connected && ac.can_settracking { BS_IDLE } else { BS_DIS };
    pbtn(win, cx,       442, 64, b'O', if ac.tracking { b"On \0" } else { b"Off\0" }, if ac.tracking { BS_HOT } else { tst });
    let tr = if ac.connected && ac.can_settracking { ac.track_rate } else { -1 };
    pbtn(win, cx + 68,  442, 64, b'1', b"Sid\0", if tr == 0 { BS_HOT } else { tst });
    pbtn(win, cx + 136, 442, 64, b'2', b"Lun\0", if tr == 1 { BS_HOT } else { tst });
    pbtn(win, cx + 204, 442, 64, b'3', b"Sol\0", if tr == 2 { BS_HOT } else { tst });

    // ---- Mount ----
    win_text(win, cx, 470, b"MOUNT\0", 0x0067B8A6);
    let pst = if ac.connected && ac.can_park { if ac.parked { BS_HOT } else { BS_IDLE } } else { BS_DIS };
    let ust = if ac.connected && ac.can_unpark { BS_IDLE } else { BS_DIS };
    let hst = if ac.connected && ac.can_findhome { BS_IDLE } else { BS_DIS };
    pbtn(win, cx,       486, 84, b'P', b"Park\0", pst);
    pbtn(win, cx + 90,  486, 84, b'U', b"Unpark\0", ust);
    pbtn(win, cx + 180, 486, 84, b'H', b"Home\0", hst);

    // footer hint (kept above the hover-reveal control bar band)
    if h > 540 {
        win_text(win, cx, 516, b"Keys work panel-shown or hidden.\0", 0x00607068);
        win_text(win, cx, 532, b"Grayed = mount reports unsupported.\0", 0x00607068);
    }
}
// Unsigned small-int formatter used by the host row (avoids i64 casts).
fn put_iu(buf: &mut [u8], n: usize, v: u32) -> usize { put_i(buf, n, v as i64) }

fn ang_diff(a: f64, b: f64) -> f64 { let mut d = fmodp(a - b + 180.0, 360.0) - 180.0; if d < -180.0 { d += 360.0; } d }
fn dim(c: u32) -> u32 {
    let r = ((c >> 16) & 0xFF) / 3; let g = ((c >> 8) & 0xFF) / 3; let b = (c & 0xFF) / 3;
    (r << 16) | (g << 8) | b
}
// epoch seconds (UT) -> JD directly (1970-01-01 = JD 2440587.5).
fn julian_date_from_epoch(secs: f64) -> f64 { 2440587.5 + secs / 86400.0 }

trait AbsExt { fn abs(self) -> f64; }
impl AbsExt for f64 { fn abs(self) -> f64 { if self < 0.0 { -self } else { self } } }
