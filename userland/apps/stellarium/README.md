# Planetarium (stellarium)

A from-scratch real-time planetarium for MayteraOS. Ring-3 userland app, Rust,
`no_std`, **hardware float** (see below).

## What works

- **Hardware-float target.** Built against a local `x86_64-maytera-user.json`
  target (SSE2 on, soft-float ABI removed) so the astronomy is real `f64`
  trigonometry compiling to `mulsd`/`addsd`/`sqrtsd`, not the fixed-point every
  in-kernel decoder is forced into. libm (`sin`/`cos`/`asin`/`atan2`/...) is
  FFI'd from `libc.a`; the Makefile's `no-softfloat-libcall` gate proves zero
  `__*df3` soft-float helpers survive.
- **Astronomy core**, validated to sub-degree at startup (printed to the
  launching terminal): Julian date -> GMST -> equatorial-to-horizontal
  (alt/az). Reference check for 2026-01-01 00:00 UT at Greenwich gives
  Sirius alt 21.8 az 179.4, Vega alt 0.3 az 1.1, Polaris alt 51.0 az 358.9
  (Polaris altitude equals the observer latitude, as it must).
- **Live sky render**: stereographic projection about a pan/zoom view centre,
  the full Hipparcos naked-eye catalogue (~4990 stars, B-V -> colour, magnitude
  -> glyph size), iconic constellation stick figures, 442 IAU proper star
  names, the Sun/Moon/seven planets via Paul Schlyter's low-precision
  ephemeris with real sphere-mapped textures, Saturn's ring and the four
  Galilean moons, a horizon with ground fill, and cardinal-direction ticks.
- **Real imagery (Slice A, this extension).** Every solar-system body's
  texture was regenerated at 160x160 (up from 96x96) from real Solar System
  Scope CC BY 4.0 source photos/renders via an orthographic sphere projection
  with diffuse shading (`gen_textures.py`, documented in
  `ASSETS_PROVENANCE.md`). New: a real Milky Way panorama (same CC BY 4.0
  source), placed along the TRUE galactic plane via the standard equatorial
  <-> galactic rotation (`src/deepsky.rs`), and two marquee deep-sky objects
  rendered from real Hubble/ESA imagery (CC BY 4.0) at their real J2000
  coordinates: M42 (Orion Nebula) and M31 (Andromeda Galaxy).
- **ASCOM Alpaca telescope control (Slice B, this extension).** A bounded
  REST client (`src/alpaca.rs`) built on the OS's raw TCP syscalls: connect
  to an Alpaca telescope's `connected` endpoint, poll `rightascension`/
  `declination` and plot the mount as a reticle, and slew/sync to whatever
  object the search box last centred on. Press `A` to enter `host:port` and
  connect, `G` to slew (goto) the selection, `Y` to sync. See
  `docs/PLANETARIUM_EXTENSION_PLAN.md` for exactly what is proven vs deferred
  (full UDP device discovery, multi-device management).
- **Interaction**: drag to pan, scroll / `[` `]` to zoom, space to pause,
  `+` `-` to change time rate, `n` to reset to now, click Search to jump to a
  named star, planet, the Sun/Moon, or M42/M31.

## Known limitations / follow-ups

- Only two deep-sky objects and no comets; see `docs/PLANETARIUM_EXTENSION_PLAN.md`
  for the plan to extend the imagery to the full catalogue.
- The Alpaca client's `host:port` entry is manual; UDP auto-discovery of
  Alpaca devices on the LAN is not implemented (also in the plan doc).
- Ships with the generic app icon; a custom planetarium icon is a polish
  follow-up.

## Build

`make` (needs pinned rustc 1.97.0 + the `rust-src` component for `-Z
build-std`, because the custom target has no prebuilt `core`). Output binary
`stellarium`, overlaid to `/APPS/stellarium` by the golden build.

## Note for maintainers

Do **not** call `SYS_WIN_GET_SIZE` at startup from this build: it wedges the
process right after the validation print (window never paints). The byte-
identical call works in the soft-float Task Manager, and our own
`SYS_WIN_GET_EVENT` write-back works fine in the loop, so the root cause is not
yet isolated (see `blame.md`, 2026-09-06). The app does not need it: it starts
from the created content size and tracks changes via `EVENT_RESIZE`.
