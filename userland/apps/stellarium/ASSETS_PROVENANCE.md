# Planetarium asset provenance (Slice A: real imagery)

Every texture and image byte shipped in `src/solar_tex.rs` came from a real,
licence-clear source. This file records exactly where each one came from, its
licence, and how it was converted into the app's existing texture pipeline
(a flat `[u8; N]` RGBA array, `alpha=0` outside the sphere/vignette, sampled
by `blit_body()` in `src/lib.rs`, which already existed and was NOT changed
by this work - only the array contents and their resolution changed).

## What existed before this work

The original `src/solar_tex.rs` (landed 2026-09-06, commit range
3ec59b69..4e604404) already shipped 96x96 RGBA sphere textures for the Sun,
the Moon, and the seven visible-from-Earth planets, sourced from Solar System
Scope (solarsystemscope.com/textures, CC BY 4.0) and reduced to 96x96 via
some now-undocumented generator (no generator script survived in the repo or
in `tools/`; only the output array and a one-line source comment did). That
CC BY 4.0 attribution was verified again for this work directly against
solarsystemscope.com's own licence page (fetched 2026-09-17: "Attribution 4.0
International license").

## What this work changed

1. **Re-fetched the SAME Solar System Scope source images**, this time at
   their 2k resolution (`2k_<body>.jpg` from
   `https://www.solarsystemscope.com/textures/download/`), and regenerated
   every sphere texture at **160x160** (up from 96x96) with
   `gen_textures.py` (below): an orthographic sphere projection (front
   hemisphere only, alpha=0 outside the unit circle) of the equirectangular
   source, with a simple ambient+diffuse shade (`0.35 + 0.65*nz`, `nz` the
   surface normal's view-axis component) so the limb darkens like a real
   photographed globe, matching the look of the original 96x96 set. This is
   a **visible resolution and detail upgrade** to bodies already shipped, not
   a new asset category - see the report/proof screenshots for the visual
   diff (Mars/Jupiter surface detail, Saturn's disc).
2. **New: a real Milky Way panorama.** Source:
   `https://www.solarsystemscope.com/textures/download/2k_stars_milky_way.jpg`
   (same site, same CC BY 4.0 licence). Downsampled directly (equirectangular,
   no reprojection) to 200x100 RGBA, brightness-boosted 2.4x for visibility
   against the app's dark sky background, alpha=0 below a near-black
   threshold so the coarse-grid renderer in `src/deepsky.rs` skips empty sky
   cheaply. See the HONESTY NOTE in `src/deepsky.rs`: the texture's own pixel
   content is a decorative/artistic asset, not a survey-calibrated star
   field, but its PLACEMENT on screen is real - every sample is looked up at
   a genuine galactic (l,b) coordinate and rotated to true J2000 RA/Dec via
   the standard IAU equatorial<->galactic transform, then projected exactly
   like every star. Verified against a known point: l=0,b=0 (galactic
   centre) round-trips to RA=266.36 deg / Dec=-28.98 deg, matching Sgr A*'s
   real position (RA 266.42 deg, Dec -29.01 deg) to within the difference
   between (0,0) and Sgr A*'s actual (l,b)=(359.94,-0.046).
3. **New: two marquee deep-sky objects, real Hubble/ESA imagery.**
   - **M42, Orion Nebula.** `heic0601a` ("Hubble's sharpest view of the Orion
     Nebula"), fetched from
     `https://cdn.esahubble.org/archives/images/screen/heic0601a.jpg`
     (1280x1280, the "screen" resolution tier). Licence: CC BY 4.0
     International, per esahubble.org/copyright/ (fetched 2026-09-17:
     "Creative Commons Attribution 4.0 International license").
   - **M31, Andromeda Galaxy.** `heic1502a` ("Sharpest ever view of the
     Andromeda Galaxy"), fetched from
     `https://cdn.esahubble.org/archives/images/screen/heic1502a.jpg`
     (1280x409, a wide detail crop of the galaxy's disc - this is a crop of
     the disc/dust-lane region, not the whole galaxy; documented here so
     nobody mistakes the in-app sprite for a full-galaxy view). Same CC BY
     4.0 licence.
   - Both were centre-cropped to square, resized to 128x128, and given a
     soft radial vignette (full alpha inside 0.72r, linear fade to 0 at 1.0r)
     so they blend into the night sky the same way the sphere-mapped bodies
     already do (`gen_nebula_sprite()` below), then rendered with the EXACT
     SAME `blit_body()` call already used for planets (`ct=-1.0`, i.e.
     "always lit" - see `src/deepsky.rs`). Real J2000 coordinates (not
     fabricated): M42 RA 05h35m17.3s / Dec -05d23m28s = 83.822/-5.391 deg;
     M31 RA 00h42m44.3s / Dec +41d16m09s = 10.685/41.269 deg (Simbad/NGC).

## What was considered and NOT shipped

- Stellarium's own bundled `textures/milkyway.png` (the Mellinger panorama)
  was considered first (task brief suggested checking Stellarium's own
  assets), but its licence is more complicated than a clean CC BY/PD grant
  (Axel Mellinger's panorama is used by Stellarium under a permission
  specific to that project, not a blanket redistribution licence). Rather
  than risk shipping something under an ambiguous licence, this work used
  Solar System Scope's OWN Milky Way texture instead - CC BY 4.0, unambiguous,
  and already the established provenance for every other texture in this app.
- A full DSS (Digitized Sky Survey) nebula/galaxy catalogue, as Stellarium
  ships as an optional add-on pack, was NOT fetched: DSS imagery is credited
  to multiple institutions (STScI, AURA, ESO, Caltech) under per-survey terms
  that are not uniformly CC/PD, and auditing each would be well outside this
  session's bound. Only ESA/Hubble's own explicitly CC BY 4.0 image archive
  was used for the two deep-sky sprites.
- No `docs/STELLARIUM_PORT_ASSESSMENT.md` ASCOM Alpaca or full-catalogue work
  was skipped for licence reasons - see `docs/PLANETARIUM_EXTENSION_PLAN.md`
  for what's deferred and why (scope/time, not licence risk).

## Size

Before: 9 x 96x96x4 = 331,776 bytes of raw texture data.
After: 9 x 160x160x4 (sun/planets/moon) + 200x100x4 (Milky Way) +
2 x 128x128x4 (nebulae) = 921,600 + 80,000 + 131,072 = 1,132,672 bytes of raw
texture data, embedded as Rust array literals in `src/solar_tex.rs` (which is
correspondingly larger as SOURCE TEXT, ~3.7MB, but that is source, not a
shipped artifact size - the compiled/linked `stellarium` binary that actually
lands on the ext2 root grew from 946,984 bytes to 1,912,136 bytes). This is a
small, bounded increase, not a full-catalogue asset dump.

## `gen_textures.py` (the generator, for the next person extending this)

Not committed (it is a one-off conversion tool, like the original 96x96
generator that did not survive either) - reproduced here so it is not lost a
second time. Run against the `2k_*.jpg`/`heic*.jpg` source files listed
above, with Pillow (`pip install pillow`):

```python
import math
from PIL import Image

def sample_equirect(img, lon, lat):
    w, h = img.size
    lon = lon % 360.0
    u = lon / 360.0
    v = (90.0 - lat) / 180.0
    x = int(u * w) % w
    y = min(h - 1, max(0, int(v * h)))
    return img.getpixel((x, y))

def gen_sphere_texture(src_path, dim, shade_ambient=0.35, shade_gain=0.65):
    src = Image.open(src_path).convert("RGB")
    out = bytearray(dim * dim * 4)
    r = dim / 2.0
    for ty in range(dim):
        ny = (r - 0.5 - ty) / r
        for tx in range(dim):
            nx = (tx + 0.5 - r) / r
            d2 = nx * nx + ny * ny
            o = (ty * dim + tx) * 4
            if d2 > 1.0:
                out[o:o+4] = (0, 0, 0, 0); continue
            nz = math.sqrt(max(0.0, 1.0 - d2))
            lon = math.degrees(math.atan2(nx, nz))
            lat = math.degrees(math.asin(max(-1.0, min(1.0, ny))))
            rr, gg, bb = sample_equirect(src, lon, lat)
            shade = shade_ambient + shade_gain * nz
            out[o+0] = min(255, int(rr * shade))
            out[o+1] = min(255, int(gg * shade))
            out[o+2] = min(255, int(bb * shade))
            out[o+3] = 255
    return out

def gen_milkyway(src_path, w, h, dark_thresh=6, boost=2.4):
    src = Image.open(src_path).convert("RGB").resize((w, h), Image.LANCZOS)
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            rr, gg, bb = src.getpixel((x, y))
            rr = min(255, int(rr * boost)); gg = min(255, int(gg * boost)); bb = min(255, int(bb * boost))
            o = (y * w + x) * 4
            out[o+0] = rr; out[o+1] = gg; out[o+2] = bb
            out[o+3] = 255 if (rr > dark_thresh or gg > dark_thresh or bb > dark_thresh) else 0
    return out

def gen_nebula_sprite(src_path, dim):
    src = Image.open(src_path).convert("RGB")
    w0, h0 = src.size
    side = min(w0, h0)
    left = (w0 - side) // 2; top = (h0 - side) // 2
    src = src.crop((left, top, left + side, top + side)).resize((dim, dim), Image.LANCZOS)
    out = bytearray(dim * dim * 4)
    r = dim / 2.0
    for ty in range(dim):
        ny = (ty + 0.5 - r) / r
        for tx in range(dim):
            nx = (tx + 0.5 - r) / r
            d = math.sqrt(nx * nx + ny * ny)
            rr, gg, bb = src.getpixel((tx, ty))
            o = (ty * dim + tx) * 4
            a = 0 if d >= 1.0 else (255 if d <= 0.72 else int(255 * (1.0 - (d - 0.72) / 0.28)))
            out[o+0] = rr; out[o+1] = gg; out[o+2] = bb; out[o+3] = a
    return out
```

Emit each array as `pub static TEX_X: [u8; N] = [b0,b1,...];` into
`src/solar_tex.rs`, in the exact format `blit_body()`/`draw_milkyway()`/
`draw_deep_sky()` already expect (see `src/lib.rs` and `src/deepsky.rs`).
