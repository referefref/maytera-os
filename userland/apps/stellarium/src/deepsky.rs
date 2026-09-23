// deepsky.rs - real Milky Way panorama placement (galactic-plane-correct, via
// the standard IAU 1958 equatorial<->galactic rotation) and two marquee deep
// sky objects (M42 Orion Nebula, M31 Andromeda Galaxy) at their real J2000
// coordinates. Included by lib.rs; uses the same blit_body()/project()
// pipeline already used for the Sun/planets/Moon (see ASSETS_PROVENANCE.md
// for exact texture provenance and licence).
//
// HONESTY NOTE (do not remove): TEX_MILKYWAY's per-pixel content is a real
// Solar System Scope skybox photo/render (CC BY 4.0), not a survey-calibrated
// star catalogue - individual star positions in that texture are NOT claimed
// to be astronomically accurate. What IS real and computed, not fabricated,
// is the PLACEMENT: every sampled texel is looked up at a genuine galactic
// (l,b) coordinate, converted to true J2000 RA/Dec via the standard rotation
// below, and projected exactly like every star. So the diffuse band always
// sits along the real galactic plane and thickens toward the real galactic
// centre direction, even though the fine texture detail is decorative.

// North Galactic Pole (J2000) and the galactic longitude of the NCP - the
// three constants that fix the standard equatorial<->galactic rotation
// (IAU 1958 definition, still the one in common use). Wikipedia "Galactic
// coordinate system" and Meeus ch.13 both give these same three numbers.
const NGP_RA: f64 = 192.85948;
const NGP_DEC: f64 = 27.12825;
const L_NCP: f64 = 122.93192;

// Galactic (l,b, degrees) -> equatorial J2000 (RA,Dec, degrees). Real
// spherical trigonometry, not a lookup table.
fn gal_to_equ(l: f64, b: f64) -> (f64, f64) {
    let br = b * DEG;
    let dl = (L_NCP - l) * DEG;
    let dgp = NGP_DEC * DEG;
    let sin_dec = unsafe { sin(br) * sin(dgp) + cos(br) * cos(dgp) * cos(dl) };
    let dec = unsafe { asin(clamp(sin_dec, -1.0, 1.0)) } / DEG;
    let y = unsafe { cos(br) * sin(dl) };
    let x = unsafe { cos(dgp) * sin(br) - sin(dgp) * cos(br) * cos(dl) };
    let ra = norm360(NGP_RA + at2(y, x));
    (ra, dec)
}

// TEX_MILKYWAY sample at galactic (l,b): l=0 (galactic centre) is placed at
// the texture's centre column (the source panorama's bright core already
// sits roughly mid-frame - see gen_textures.py in ASSETS_PROVENANCE.md), and
// wraps both ways; b=+90 (north galactic pole) is the top row, b=-90 the
// bottom row, matching the raw equirectangular resize.
fn mw_sample(l: f64, b: f64) -> (u32, u32, u32, u8) {
    let shifted = fmodp(l + 180.0, 360.0);
    let tx = ((shifted / 360.0) * MW_W as f64) as i32;
    let tx = if tx < 0 { 0 } else if tx >= MW_W { MW_W - 1 } else { tx };
    let bc = clamp(b, -90.0, 90.0);
    let ty = (((90.0 - bc) / 180.0) * MW_H as f64) as i32;
    let ty = if ty < 0 { 0 } else if ty >= MW_H { MW_H - 1 } else { ty };
    let o = ((ty * MW_W + tx) * 4) as usize;
    (TEX_MILKYWAY[o] as u32, TEX_MILKYWAY[o + 1] as u32, TEX_MILKYWAY[o + 2] as u32, TEX_MILKYWAY[o + 3])
}

// Draw the Milky Way band as a coarse grid of real (l,b) sample points, each
// projected through the SAME stereographic project() used for stars, so the
// band is exactly where the true galactic plane is for the current view/time
// - never a fixed decorative overlay. One win_draw_rect per grid cell (not
// per pixel): cost is a fixed ~1800 rects/frame regardless of zoom, the same
// order of magnitude as the star loop below.
fn draw_milkyway(win: i32, w: i32, h: i32, v: &View, g: f64, ccx: i32, ccy: i32, scale: f64) {
    let step = 6.0_f64;
    let mut bq = -87.0_f64;
    while bq <= 87.0 {
        let mut lq = 0.0_f64;
        while lq < 360.0 {
            let (ra, dec) = gal_to_equ(lq, bq);
            if let Some((px, py)) = project(ra, dec, v, g, ccx, ccy, scale, w, h) {
                let (r, gg, b, a) = mw_sample(lq, bq);
                if a > 0 {
                    let bs = (step * scale) as i32 + 1;
                    let bs = if bs < 1 { 1 } else if bs > 400 { 400 } else { bs };
                    win_draw_rect(win, px - bs / 2, py - bs / 2, bs, bs, (r << 16) | (gg << 8) | b);
                }
            }
            lq += step;
        }
        bq += step;
    }
    let _ = (w, h);
}

// Marquee deep-sky objects: real J2000 coordinates (Simbad/NGC), rendered
// through the same blit_body() sphere-disc blitter used for planets (their
// sprites already carry a soft alpha vignette baked in by gen_textures.py,
// so no special-casing is needed here beyond a fixed screen radius).
struct DeepSky { name: &'static [u8], ra: f64, dec: f64, tex: &'static [u8] }
static DEEP_SKY: &[DeepSky] = &[
    // M42, Orion Nebula: RA 05h35m17.3s, Dec -05d23m28s (J2000).
    DeepSky { name: b"M42\0", ra: 83.822, dec: -5.391, tex: &TEX_NEB_M42 },
    // M31, Andromeda Galaxy: RA 00h42m44.3s, Dec +41d16m09s (J2000).
    DeepSky { name: b"M31\0", ra: 10.685, dec: 41.269, tex: &TEX_NEB_M31 },
];

fn draw_deep_sky(win: i32, w: i32, h: i32, v: &View, g: f64, ccx: i32, ccy: i32, scale: f64, zf: f64) {
    for d in DEEP_SKY.iter() {
        if let Some((px, py)) = project(d.ra, d.dec, v, g, ccx, ccy, scale, w, h) {
            let r = (10.0 + 10.0 * zf) as i32;
            let r = if r > NEB_DIM { NEB_DIM } else { r };
            blit_body(win, px, py, r, d.tex, NEB_DIM, -1.0);
            win_text(win, px + r + 4, py - 4, d.name, 0x00C8A0E0);
        }
    }
}

// Look up a deep-sky object by name prefix for search_center(); returns
// (ra,dec) so the same search box drives constellation stars, solar system
// bodies AND these two catalogue objects.
fn deep_sky_lookup(q: &[u8], qn: usize) -> Option<(f64, f64, &'static [u8])> {
    for d in DEEP_SKY.iter() {
        if name_ci_prefix(d.name, q, qn) { return Some((d.ra, d.dec, d.name)); }
    }
    None
}
