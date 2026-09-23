// solar.rs - low-precision solar-system ephemeris (Paul Schlyter's method,
// ~1-2 arcmin), included by lib.rs. Hardware f64: real sin/cos/sqrt/atan2.
// Gives geocentric RA/Dec (deg) for the Sun, Moon and the planets, for a given
// day count d = JD - 2451543.5 (days since 1999-12-31 00:00 UT).

fn norm360(mut x: f64) -> f64 { x %= 360.0; if x < 0.0 { x += 360.0; } x }
fn rd(x: f64) -> f64 { x * DEG }               // degrees -> radians
fn c(x: f64) -> f64 { unsafe { cos(rd(x)) } }  // cos of degrees
fn s(x: f64) -> f64 { unsafe { sin(rd(x)) } }  // sin of degrees
fn at2(y: f64, x: f64) -> f64 { (unsafe { atan2(y, x) }) / DEG }  // atan2 -> degrees
fn sq(x: f64) -> f64 { unsafe { sqrt(x) } }

// obliquity of the ecliptic for day d (degrees)
fn obliquity(d: f64) -> f64 { 23.4393 - 3.563e-7 * d }

// Sun: returns (RA_deg, Dec_deg, ecliptic-lon_deg, xs, ys) - xs/ys are the Sun's
// geocentric rectangular ecliptic coords, reused for the planets' geocentric step.
fn sun_full(d: f64) -> (f64, f64, f64, f64, f64) {
    let w = 282.9404 + 4.70935e-5 * d;
    let e = 0.016709 - 1.151e-9 * d;
    let m = norm360(356.0470 + 0.9856002585 * d);
    let ecl = obliquity(d);
    let ea = m + e * (180.0 / 3.14159265358979) * s(m) * (1.0 + e * c(m));
    let xv = c(ea) - e;
    let yv = sq(1.0 - e * e) * s(ea);
    let v = at2(yv, xv);
    let r = sq(xv * xv + yv * yv);
    let lon = norm360(v + w);
    let xs = r * c(lon);
    let ys = r * s(lon);
    // equatorial (rotate by obliquity)
    let xe = xs;
    let ye = ys * c(ecl);
    let ze = ys * s(ecl);
    let ra = norm360(at2(ye, xe));
    let dec = at2(ze, sq(xe * xe + ye * ye));
    (ra, dec, lon, xs, ys)
}

// Planet orbital elements as (N,i,w,a,e,M) linear-in-d coefficients.
// idx: 0 Mercury 1 Venus 2 Mars 3 Jupiter 4 Saturn 5 Uranus 6 Neptune.
fn planet_elems(idx: usize, d: f64) -> (f64, f64, f64, f64, f64, f64) {
    match idx {
        0 => (48.3313 + 3.24587e-5*d, 7.0047 + 5.00e-8*d, 29.1241 + 1.01444e-5*d, 0.387098, 0.205635 + 5.59e-10*d, 168.6562 + 4.0923344368*d),
        1 => (76.6799 + 2.46590e-5*d, 3.3946 + 2.75e-8*d, 54.8910 + 1.38374e-5*d, 0.723330, 0.006773 - 1.302e-9*d, 48.0052 + 1.6021302244*d),
        2 => (49.5574 + 2.11081e-5*d, 1.8497 - 1.78e-8*d, 286.5016 + 2.92961e-5*d, 1.523688, 0.093405 + 2.516e-9*d, 18.6021 + 0.5240207766*d),
        3 => (100.4542 + 2.76854e-5*d, 1.3030 - 1.557e-7*d, 273.8777 + 1.64505e-5*d, 5.20256, 0.048498 + 4.469e-9*d, 19.8950 + 0.0830853001*d),
        4 => (113.6634 + 2.38980e-5*d, 2.4886 - 1.081e-7*d, 339.3939 + 2.97661e-5*d, 9.55475, 0.055546 - 9.499e-9*d, 316.9670 + 0.0334442282*d),
        5 => (74.0005 + 1.3978e-5*d, 0.7733 + 1.9e-8*d, 96.6612 + 3.0565e-5*d, 19.18171 - 1.55e-8*d, 0.047318 + 7.45e-9*d, 142.5905 + 0.011725806*d),
        _ => (131.7806 + 3.0173e-5*d, 1.7700 - 2.55e-7*d, 272.8461 - 6.027e-6*d, 30.05826 + 3.313e-8*d, 0.008606 + 2.15e-9*d, 260.2471 + 0.005995147*d),
    }
}

// Planet geocentric RA/Dec (deg) and Earth-distance r (AU).
fn planet_radec(idx: usize, d: f64, xs: f64, ys: f64) -> (f64, f64, f64) {
    let (nn, ii, ww, a, e, m0) = planet_elems(idx, d);
    let n = norm360(nn); let m = norm360(m0);
    // eccentric anomaly (two iterations for the outer planets' larger e is fine)
    let mut ea = m + e * (180.0 / 3.14159265358979) * s(m) * (1.0 + e * c(m));
    for _ in 0..2 {
        ea = ea - (ea - e * (180.0 / 3.14159265358979) * s(ea) - m) / (1.0 - e * c(ea));
    }
    let xv = a * (c(ea) - e);
    let yv = a * sq(1.0 - e * e) * s(ea);
    let v = at2(yv, xv);
    let r = sq(xv * xv + yv * yv);
    let vw = v + ww;
    // heliocentric ecliptic
    let xh = r * (c(n) * c(vw) - s(n) * s(vw) * c(ii));
    let yh = r * (s(n) * c(vw) + c(n) * s(vw) * c(ii));
    let zh = r * (s(vw) * s(ii));
    // geocentric ecliptic
    let xg = xh + xs; let yg = yh + ys; let zg = zh;
    // equatorial
    let ecl = obliquity(d);
    let xe = xg;
    let ye = yg * c(ecl) - zg * s(ecl);
    let ze = yg * s(ecl) + zg * c(ecl);
    let ra = norm360(at2(ye, xe));
    let dec = at2(ze, sq(xe * xe + ye * ye));
    let dist = sq(xg * xg + yg * yg + zg * zg);
    (ra, dec, dist)
}

// Moon geocentric RA/Dec (deg) with the main perturbations, plus its ecliptic
// longitude (for the illuminated-fraction / phase calc).
fn moon_full(d: f64) -> (f64, f64, f64) {
    let n = norm360(125.1228 - 0.0529538083 * d);
    let i = 5.1454;
    let w = norm360(318.0634 + 0.1643573223 * d);
    let a = 60.2666;
    let e = 0.054900;
    let m = norm360(115.3654 + 13.0649929509 * d);
    let mut ea = m + e * (180.0/3.14159265358979) * s(m) * (1.0 + e * c(m));
    for _ in 0..2 { ea = ea - (ea - e*(180.0/3.14159265358979)*s(ea) - m) / (1.0 - e*c(ea)); }
    let xv = a * (c(ea) - e);
    let yv = a * sq(1.0 - e*e) * s(ea);
    let v = at2(yv, xv);
    let r = sq(xv*xv + yv*yv);
    let vw = v + w;
    let mut xh = r * (c(n)*c(vw) - s(n)*s(vw)*c(i));
    let mut yh = r * (s(n)*c(vw) + c(n)*s(vw)*c(i));
    let mut zh = r * (s(vw)*s(i));
    // ecliptic lon/lat, then main perturbations (Schlyter)
    let mut lon = norm360(at2(yh, xh));
    let mut lat = at2(zh, sq(xh*xh + yh*yh));
    let ls = norm360(282.9404 + 4.70935e-5*d + 356.0470 + 0.9856002585*d); // Sun mean lon
    let ms = norm360(356.0470 + 0.9856002585*d);   // Sun mean anomaly
    let mm = m;                                     // Moon mean anomaly
    let ll = norm360(n + w + m);                    // Moon mean longitude
    let dd = norm360(ll - ls);                      // mean elongation
    let f = norm360(ll - n);                        // argument of latitude
    lon += -1.274*s(mm - 2.0*dd) + 0.658*s(2.0*dd) - 0.186*s(ms)
         - 0.059*s(2.0*mm - 2.0*dd) - 0.057*s(mm - 2.0*dd + ms)
         + 0.053*s(mm + 2.0*dd) + 0.046*s(2.0*dd - ms) + 0.041*s(mm - ms)
         - 0.035*s(dd) - 0.031*s(mm + ms) - 0.015*s(2.0*f - 2.0*dd) + 0.011*s(mm - 4.0*dd);
    lat += -0.173*s(f - 2.0*dd) - 0.055*s(mm - f - 2.0*dd) - 0.046*s(mm + f - 2.0*dd)
         + 0.033*s(f + 2.0*dd) + 0.017*s(2.0*mm + f);
    let _ = (xh, yh, zh); xh = 0.0; yh = 0.0; zh = 0.0; let _ = (xh, yh, zh);
    lon = norm360(lon);
    // ecliptic -> equatorial
    let rg = 1.0;
    let xg = rg * c(lon) * c(lat);
    let yg = rg * s(lon) * c(lat);
    let zg = rg * s(lat);
    let ecl = obliquity(d);
    let xe = xg;
    let ye = yg * c(ecl) - zg * s(ecl);
    let ze = yg * s(ecl) + zg * c(ecl);
    let ra = norm360(at2(ye, xe));
    let dec = at2(ze, sq(xe*xe + ye*ye));
    let _ = (v, r);
    (ra, dec, lon)
}

// day count d for an epoch-seconds UT time (JD = 2440587.5 + secs/86400).
fn epoch_to_d(secs: f64) -> f64 { (2440587.5 + secs / 86400.0) - 2451543.5 }
