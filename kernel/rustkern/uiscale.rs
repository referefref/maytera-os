// rustkern/uiscale.rs - THE GLOBAL UI SCALE FACTOR. ONE definition, one place.
//
// WHY THIS EXISTS
// ---------------------------------------------------------------------------
// Reported by the owner from a real ASUS laptop (i7-4720HQ, Haswell) boot:
// "the resolution is far too large to not have UI scaling however which made
// the first run wizard hard to read and overall everything is too small."
//
// Every size in MayteraOS was a raw pixel count tuned against ~100 PPI panels:
// the 1280x800 test VMs, and the iMac14,4's 21.5" 1920x1080 (102 PPI). A 15.6"
// 1920x1080 laptop panel is 141 PPI. The SAME pixel count is 1.47x smaller in
// the hand. Nothing in the tree knew that, because nothing in the tree had any
// concept of scale at all (grepped 2026-08-26: zero hits for a UI scale, DPI or
// zoom notion outside image resampling and one per-app terminal zoom).
//
// WHY IT IS ONE VALUE IN ONE MODULE
// ---------------------------------------------------------------------------
// This project's most expensive recurring fault is one value living in several
// places (dock_opacity.h documents SEVEN copies of one constant). A scale
// factor is the worst possible candidate for that fault, because a copy that
// drifts does not merely look wrong: a control drawn from a 1.5x copy and
// hit-tested against a 1.0x copy is a DEAD CONTROL (#208), which reads as a
// rendering bug and is diagnosed as one. So: the factor lives here, as one
// atomic, and every consumer in the kernel and in every Ring 3 app reaches it
// through a call, never a copy.
//
// WHY RUST. The standing 2026-07-16 rule: new kernel code is Rust unless there
// is a stated performance or entanglement reason. There is none. This is
// integer arithmetic and one atomic, called from draw paths that were already
// doing a table load and a multiply.
//
// NO FLOAT, AND THAT IS NOT A LIMITATION HERE. The kernel target is
// x86_64-unknown-none: soft-float, SSE disabled. A scale factor is therefore
// an integer PERCENT, never a float, and every derivation below is integer.
// This is the right representation anyway: 150 is exact, 1.5f32 is exact, but
// 1.1f32 is not, and a scale that is 1.0999999 produces off-by-one borders
// that look like a rendering bug.
//
// ===========================================================================
// THE ROUNDING CONTRACT - READ THIS BEFORE CHANGING ANY ARITHMETIC
// ---------------------------------------------------------------------------
// 2x is trivial: every integer maps to an even integer, borders stay even,
// nothing can look uneven. 1.25x and 1.5x are where scaling gets a reputation
// for looking broken, and there are exactly two ways to earn it:
//
//   (1) ROUNDING A POSITION AND A SIZE INDEPENDENTLY. Naively, a box at x=10
//       w=15 at 1.5x becomes x=15 w=23 (round(22.5)), so its right edge lands
//       at 38 while the NEXT box, at x=25, lands at round(37.5)=38 too. One
//       pixel of overlap, or one pixel of gap, depending on which way each
//       rounds. Across a row of controls the gaps end up 1px different from
//       each other and it reads as sloppy drawing.
//
//       THE FIX, and the reason `scale_span` exists: SCALE THE EDGES, DERIVE
//       THE EXTENT. w' = px(x+w) - px(x). Adjacent boxes then share an edge
//       EXACTLY, by construction, at every scale factor, because the shared
//       edge is one number scaled once. This is the single most important
//       line in this file. Any caller that scales a width on its own is
//       reintroducing the bug.
//
//   (2) ROUNDING THE SAME INPUT DIFFERENTLY IN TWO PLACES. Avoided by there
//       being one function.
//
// A nonzero input NEVER scales to zero. A 1px separator at 1.25x is 1px, not
// 0px. Losing a hairline entirely is a worse artefact than it being 1px thin,
// and a 0 return additionally collides with the theme table's "unknown id"
// sentinel (see the note in theme_get_metric_by_id).
//
// ===========================================================================
// WHY THE FACTOR IS *NOT* A THEME KEY, AND WHERE IT LIVES INSTEAD
// ---------------------------------------------------------------------------
// The theme system was the obvious host: `type.body` and `metric.menu_row_h`
// are already theme-driven and reach every widget, and a .mtheme file can be
// edited on a booted machine with no rebuild, which is exactly what the owner
// needs while judging what looks right. It is still the wrong home:
//
//   - A THEME IS PER-LOOK. SCALE IS PER-DISPLAY. They are independent axes.
//     Putting scale in the theme means every one of the 14 shipped themes needs
//     a variant per scale step, and switching from Retro UNIX to Maytera Dark
//     would silently change how big everything is.
//   - build/assets/theme-scale-lint.sh ENFORCES that every theme's type sizes
//     come from the fixed set {11,14,16,20,28}, radii from {0,3,4,6,10} and
//     spacing from {4,8,12,16,24,32}, with lineheight == round(size*1.4).
//     Multiplying the STORED values by 1.5 fails that gate for every theme.
//     The lint is right and the design scale it protects is right: the theme
//     stores the 1x design, and scale is applied ON TOP at read time.
//
// So the theme keeps its 1x values untouched, and the multiply happens inside
// theme_get_metric_by_id() - the ONE function every metric read in the kernel
// AND every SYS_THEME_METRIC from Ring 3 already passes through. Every widget
// that was already theme-wired scales for free, with no edit.
//
// The factor itself is persisted in /CONFIG/DISPLAY.CFG (`scale=150`), which
// is machine-wide and on the ext2 root, because the first-run wizard has to be
// readable BEFORE any user or any user config exists.
//
// ===========================================================================
// AUTO-DETECTION, AND THE HONEST LIMIT OF IT
// ---------------------------------------------------------------------------
// The correct input is PHYSICAL PANEL SIZE, and we cannot read it:
//
//   - EDID would give it exactly (bytes 21-22, cm; the detailed timing
//     descriptor gives mm). The UEFI route is EFI_EDID_ACTIVE_PROTOCOL on the
//     GOP handle, which uefi/bootloader.c could query in about thirty lines -
//     but the bootloader binary is NOT built by build/build-golden.sh (grepped:
//     the golden inherits BOOTX64.EFI from the asset base image), so changing
//     it is a separate change to a separate artefact. THE HOOK IS LEFT: if
//     boot_info ever carries an EDID block, feed physical mm into
//     uiscale_auto_pct_rs and delete the heuristic below.
//   - The in-tree GMBUS/EDID path (drivers/intel_gpu.c) is NOT an option. Its
//     own audit header says it is an unvalidated register crib with wrong
//     PCH-relative offsets, zero callers, and a probe loop that does not
//     terminate on a non-Intel first display device.
//
// So auto-detection uses what it actually has, and says so:
//
//   PIXELS ALONE CANNOT DISTINGUISH a 15.6" 1920x1080 laptop panel (141 PPI)
//   FROM A 21.5" 1920x1080 desktop panel (102 PPI). They are the same number.
//
// One extra bit is available and it is a real measurement, not a guess: a
// machine with a battery is a laptop, and a laptop that is running 1920x1080
// or better has a 13"-17" panel, i.e. 130-170 PPI. That is a firmware
// question (ACPI PNP0C0A, control-method battery), answerable by the same
// DSDT/SSDT byte scan drivers/mouse.c already uses for PS/2 _HIDs - so this
// reuses amlhid.rs's scanner rather than forking a second one.
//
// The resulting policy, with every branch stated:
//
//   logical floor    the scaled-down logical screen must never fall below
//                    MIN_LOGICAL_W x MIN_LOGICAL_H. This is a HARD bound on
//                    every path including a manual override, because a
//                    1366x768 panel at 2x has 683x384 of logical room and
//                    the Settings window alone wants 900x726: the UI would
//                    not merely look odd, it would not fit at all.
//   < 1600 wide      100. Small panels have no pixels to spend.
//   >= 1600, laptop  150. Battery present => 13"-17" => >=130 PPI.
//   >= 1600, desk    100. A 1920x1080 desktop/VM panel is ~100 PPI and is
//                    exactly what the current UI was tuned against.
//   >= 2560          150 even without a battery: at that width a desktop
//                    panel is 27" (109 PPI) at best and usually much denser.
//   >= 3200          200.
//
// EVERY branch is overridable. Auto-detection that cannot be overridden is a
// worse failure than no auto-detection, because the user has no recourse.

use core::sync::atomic::{AtomicI32, Ordering};

/// Scale is an integer percent. 100 == 1x.
pub const PCT_MIN: i32 = 100;
pub const PCT_MAX: i32 = 300;
/// The step the UI offers. Arbitrary values still WORK (the arithmetic is
/// general); this is what the Settings control exposes, because a continuous
/// slider over a factor whose effect is quantised to whole pixels invites the
/// user to pick 137% and then wonder why it looks like 135%.
pub const PCT_STEP: i32 = 25;

/// The smallest logical screen the shipped UI is designed to lay out in.
/// Settings asks for 900x726 outer; Files 820x560; the OOBE wizard 688x616,
/// with a 640x480 fallback. 1024x600 clears all of them with room for the dock.
pub const MIN_LOGICAL_W: i32 = 1024;
pub const MIN_LOGICAL_H: i32 = 600;

/// The live factor. Starts at 100 so that anything reading it before
/// uiscale_init runs (early boot text, the panic path) behaves exactly as it
/// did before this module existed.
static PCT: AtomicI32 = AtomicI32::new(100);

/// What produced the current value, for the boot log and for Settings to show.
/// A user who sees "150% (auto: battery present, 1920x1080)" can tell whether
/// the machine guessed or whether they chose, which is the difference between
/// a setting they trust and one they fight.
pub const SRC_DEFAULT: i32 = 0;
pub const SRC_AUTO: i32 = 1;
pub const SRC_CONFIG: i32 = 2;
pub const SRC_USER: i32 = 3;
/// Pinned by a file on the FAT boot partition. See uiscale.c: this is the
/// recovery path for a machine whose UI is currently too small to operate,
/// which is the exact situation this feature exists to fix and therefore the
/// one situation in which "just change it in Settings" is useless advice.
pub const SRC_ESP: i32 = 4;
static SRC: AtomicI32 = AtomicI32::new(SRC_DEFAULT);

/// Monotonic generation counter. Bumped on every accepted change, so a
/// consumer that caches derived geometry (the compositor's chrome, a
/// gui_menu_bar_t's cached row height) can detect a change with one load and
/// re-derive, without anyone having to invent a broadcast.
static GEN: AtomicI32 = AtomicI32::new(0);

// ---------------------------------------------------------------------------
// SCALE-NATIVE PROCESSES.
//
// Exactly one Ring 3 program must see REAL screen pixels rather than logical
// ones: the compositor. It owns the framebuffer and draws the dock, the
// desktop and the taskbar at absolute coordinates, so it applies the scale
// factor to its own chrome itself. Everything else is scale-transparent: the
// kernel scales its window coordinates in and divides its mouse coordinates
// out, and it never learns the factor exists.
//
// THIS USED TO BE INFERRED FROM FRAMEBUFFER OWNERSHIP, AND THAT WAS WRONG IN A
// WAY THAT ONLY SHOWED UP ON A BOOTED MACHINE. The test was "is this process
// the framebuffer owner", which is true for the compositor - but only AFTER it
// claims the framebuffer, and it calls SYS_FB_INFO to learn the screen size
// BEFORE it claims. So at exactly the one call that decides its entire layout,
// the compositor was told the LOGICAL size. Measured at 150% on a 1920x1080
// display: it painted the wallpaper and the dock into the top-left 1280x720
// and left the remaining two thirds of the screen showing whatever the boot
// splash had left there.
//
// The lesson is the general one: a capability inferred from a side effect is
// only true after that side effect has happened, and "after" is a property of
// the call order, not of the process. So the mark is EXPLICIT - the compositor
// says so, as its first act - and claiming the framebuffer sets it too, as a
// backstop for anything that forgets.
//
// A SINGLE SLOT, not a set: there is one compositor, and a table of pids would
// need an exit hook to avoid a recycled pid inheriting the mark. One slot,
// cleared when the framebuffer owner is released, cannot accumulate stale
// entries.
static NATIVE_PID: AtomicI32 = AtomicI32::new(0);

/// Mark `pid` as thinking in real screen pixels.
#[no_mangle]
pub extern "C" fn uiscale_mark_native_rs(pid: i32) {
    if pid > 0 {
        NATIVE_PID.store(pid, Ordering::Relaxed);
    }
}

/// Drop the mark if it belongs to `pid`. Called when the framebuffer owner is
/// released, so a recycled pid can never inherit it.
#[no_mangle]
pub extern "C" fn uiscale_clear_native_rs(pid: i32) {
    let _ = NATIVE_PID.compare_exchange(pid, 0, Ordering::Relaxed, Ordering::Relaxed);
}

/// Does `pid` think in real screen pixels?
#[no_mangle]
pub extern "C" fn uiscale_is_native_rs(pid: i32) -> i32 {
    if pid > 0 && NATIVE_PID.load(Ordering::Relaxed) == pid { 1 } else { 0 }
}

#[inline]
fn clamp_pct(p: i32) -> i32 {
    if p < PCT_MIN { PCT_MIN } else if p > PCT_MAX { PCT_MAX } else { p }
}

/// The current factor, in percent.
#[no_mangle]
pub extern "C" fn uiscale_pct_rs() -> i32 {
    PCT.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn uiscale_src_rs() -> i32 {
    SRC.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn uiscale_gen_rs() -> i32 {
    GEN.load(Ordering::Relaxed)
}

/// Set the factor. Returns the value actually adopted after clamping, which
/// may differ from what was asked: callers should REPORT that value rather
/// than the one they passed, or the UI will claim a scale that is not in force.
/// `src` is one of the SRC_* codes.
#[no_mangle]
pub extern "C" fn uiscale_set_pct_rs(pct: i32, src: i32) -> i32 {
    let v = clamp_pct(pct);
    let old = PCT.swap(v, Ordering::Relaxed);
    SRC.store(src, Ordering::Relaxed);
    if old != v {
        GEN.fetch_add(1, Ordering::Relaxed);
    }
    v
}

/// The largest factor this framebuffer can carry without the logical screen
/// falling below the minimum the UI is designed for. Rounded DOWN to a whole
/// PCT_STEP so the answer is one of the values the UI offers.
///
/// This is a bound on EVERY path, including a hand-edited config file: a user
/// who types scale=300 on a 1366x768 panel has asked for something that cannot
/// draw, and the honest response is to give them the largest that can and say
/// so, not to obey and leave them with an unusable machine and no way back.
#[no_mangle]
pub extern "C" fn uiscale_max_pct_rs(fb_w: i32, fb_h: i32) -> i32 {
    if fb_w <= 0 || fb_h <= 0 {
        return PCT_MIN;
    }
    let by_w = fb_w * 100 / MIN_LOGICAL_W;
    let by_h = fb_h * 100 / MIN_LOGICAL_H;
    let mut m = if by_w < by_h { by_w } else { by_h };
    m = m - (m % PCT_STEP);
    if m < PCT_MIN { PCT_MIN } else if m > PCT_MAX { PCT_MAX } else { m }
}

/// The auto-detected default for this display.
///
/// `laptop`: 1 if the firmware declares a battery, 0 if it declares none,
/// and -1 if we could not ask (no validated DSDT, or the scanner's own
/// positive control failed). -1 is treated as "not a laptop", i.e. it never
/// makes the picture BIGGER on the strength of a measurement that did not
/// happen. An unknown must not be silently promoted to a yes.
#[no_mangle]
pub extern "C" fn uiscale_auto_pct_rs(fb_w: i32, fb_h: i32, laptop: i32) -> i32 {
    let cap = uiscale_max_pct_rs(fb_w, fb_h);
    // 3200+ is the 4K branch, and it is not hypothetical: the owner's ASUS
    // i7-4720HQ runs 3840x2160 (measured off its own /BOOTLOG.TXT:
    // "fb 3840x2160 pitch=15360 bpp=32"). 200% gives an effective 1920x1080,
    // which is the standard answer for a 4K laptop panel and is an INTEGER
    // factor, so every scaled edge lands on a whole pixel and nothing can look
    // uneven.
    //
    // It is worth being precise about what 200% does and does not achieve. A
    // 15.6" 3840x2160 panel is about 282 PPI against a UI drawn for about 100,
    // so "physically the same size as the desktop this was designed on" would
    // be nearer 275%. 200% is deliberately the DEFAULT rather than the maximum:
    // it is crisp, it is what the other desktops pick, and 225-300% remain one
    // dropdown click away for anyone who wants the picture larger still.
    let want = if fb_w >= 3200 {
        200
    } else if fb_w >= 2560 {
        150
    } else if fb_w >= 1600 {
        // 1600-2559: 100%, AND THE BATTERY IS DELIBERATELY NOT CONSULTED HERE.
        //
        // The battery probe exists because a laptop running 1920x1080 has a
        // 13"-17" panel at 130-170 PPI, which genuinely wants 150%. That
        // inference has a precondition: THE FRAMEBUFFER IS THE PANEL'S NATIVE
        // MODE. Boot-time GOP mode selection (\boot\MODE.TXT) breaks it. The
        // owner's ASUS has a 3840x2160 panel and has been given MODE.TXT
        // 1920x1080 as an interim way to make the UI readable, so on that
        // machine a 1920x1080 framebuffer is a 4K panel being driven at half
        // resolution - and 150% on top of a mode change that already doubled
        // everything would land at three times the native size. That is a worse
        // outcome than the bug this feature fixes, and it would look like this
        // feature caused it.
        //
        // We cannot yet tell the two apart: boot_info gained video_mode_status
        // (MODESEL_NONE / MODESEL_REFUSED / applied) in the mode-selection work,
        // but that field is not in this tree yet, and the FILE's contents are
        // not evidence - a mode override can be present and REFUSED, in which
        // case the firmware mode was kept.
        //
        // So the safe direction is taken: auto-detection never makes the UI
        // BIGGER on a resolution where it might be reasoning about the wrong
        // surface. A native 1920x1080 laptop therefore gets 100% by default,
        // exactly as it does today, and its owner opts in. The battery probe is
        // still run and still reported, in the boot log and in Settings, because
        // it is the input this branch will use the moment the mode-status field
        // is available: WHEN video_mode_status SAYS NO OVERRIDE WAS APPLIED,
        // `laptop == 1` may return 150 here again.
        let _ = laptop;
        100
    } else {
        100
    };
    if want > cap { cap } else { want }
}

// ===========================================================================
// THE ARITHMETIC. Every scaled pixel in the system comes from these four.
// ===========================================================================

/// Scale one coordinate or one standalone length. Round half up.
///
/// A POSITIVE input never returns 0 (see the rounding contract above). A
/// negative input is scaled symmetrically, because coordinates legitimately go
/// negative: apps draw at negative offsets to clip content against the top or
/// left edge of their window, and a negative that rounded the other way would
/// shift such content by a pixel relative to everything around it.
/// The core, with the factor passed in. Everything else is a wrapper.
///
/// EXPLICIT-FACTOR VARIANTS EXIST FOR ONE CALLER AND ONE REASON: when the scale
/// changes on a running machine, every open window has to be resized so its
/// LOGICAL size is unchanged, which means converting a physical size through
/// the OLD factor and back through the NEW one. Doing that with `x * old / new`
/// at the call site would be a SECOND rounding rule living somewhere else, and
/// a second rounding rule is exactly what this module exists to prevent. So the
/// conversion uses these, and there is still only one implementation.
#[no_mangle]
pub extern "C" fn uiscale_px_at_rs(v: i32, pct: i32) -> i32 {
    if pct == 100 || v == 0 {
        return v;
    }
    // i64 throughout. v is a screen coordinate and pct <= 300, so i32 would in
    // practice hold it, but a caller passing a garbage sentinel like INT_MAX
    // would wrap and produce a NEGATIVE coordinate that then indexes a buffer.
    // Widening costs nothing on this path.
    let p = pct as i64;
    let a = (v as i64).unsigned_abs() as i64;
    // Round half up, then mirror the sign, so px() is an ODD function. Odd
    // matters: apps draw at negative offsets to clip content against the top or
    // left edge of their window, and a negative that rounded the other way
    // would shift that content one pixel relative to everything around it.
    let mut r = (a * p + 50) / 100;
    if r < 1 {
        r = 1;   // a hairline never disappears (see the rounding contract)
    }
    let r = r as i32;
    if v > 0 { r } else { -r }
}

/// The exact inverse of `uiscale_px_at_rs`, with the factor passed in.
#[no_mangle]
pub extern "C" fn uiscale_unpx_at_rs(v: i32, pct: i32) -> i32 {
    if pct == 100 || v == 0 {
        return v;
    }
    // THE EXACT INVERSE, DERIVED, NOT GUESSED. This was wrong in the first
    // draft and the self-test's ROUNDTRIP property caught it before the code
    // ever booted: a plain floor divide (v*100/pct) looks like the obvious
    // inverse of a multiply and is NOT the inverse of a multiply that ROUNDS
    // HALF UP. At 125% it sent physical 46 back to logical 36 when px(37) == 46,
    // i.e. the last physical pixel of a control mapped to its NEIGHBOUR. That
    // is precisely the dead-control-at-the-edge fault (#208) this whole
    // mechanism exists to avoid, and it was one line deep.
    //
    // Derivation. px(x) = floor((x*p + 50)/100). We want the largest x with
    // px(x) <= y:
    //     floor((x*p + 50)/100) <= y
    //  => x*p + 50 < 100*(y + 1)
    //  => x*p      < 100*y + 50
    //  => x       <= floor((100*y + 49) / p)
    // so unpx(y) = floor((100*y + 49) / p), and every physical pixel in
    // [px(l), px(l+1)) maps back to exactly l, with no gaps and no overlaps.
    let p = pct as i64;
    let a = (v as i64).unsigned_abs() as i64;
    let q = ((a * 100 + 49) / p) as i32;
    if v > 0 { q } else { -q }
}

/// Scale one coordinate or one standalone length by the LIVE factor.
#[no_mangle]
pub extern "C" fn uiscale_px_rs(v: i32) -> i32 {
    uiscale_px_at_rs(v, PCT.load(Ordering::Relaxed))
}

/// Scale an extent that starts at `origin`, by scaling both EDGES and taking
/// the difference. THIS is what callers with an (x,w) or (y,h) pair must use.
/// See the rounding contract: it is what makes adjacent boxes share an edge
/// exactly at 1.25x and 1.5x instead of overlapping or gapping by a pixel.
///
/// A positive extent never becomes 0.
#[no_mangle]
pub extern "C" fn uiscale_span_rs(origin: i32, extent: i32) -> i32 {
    let pct = PCT.load(Ordering::Relaxed);
    if pct == 100 || extent == 0 {
        return extent;
    }
    let a = uiscale_px_at_rs(origin, pct);
    let b = uiscale_px_at_rs(origin + extent, pct);
    let d = b - a;
    if extent > 0 && d < 1 {
        1
    } else if extent < 0 && d > -1 {
        -1
    } else {
        d
    }
}

/// PHYSICAL -> LOGICAL by the LIVE factor. The hit-testing direction, and why
/// scaling cannot produce a dead control: an app lays out and hit-tests
/// entirely in logical pixels, draws through a boundary that multiplies, and
/// receives input through this boundary that divides. Neither side ever sees
/// the factor.
#[no_mangle]
pub extern "C" fn uiscale_unpx_rs(v: i32) -> i32 {
    uiscale_unpx_at_rs(v, PCT.load(Ordering::Relaxed))
}

// ===========================================================================
// SELF-TEST. Runs on every boot and prints one line.
//
// There is no C original to differ against, so this is a PROPERTY test in the
// drvmap.rs/intelgpu.rs sense: it proves the arithmetic obeys its own rules,
// not that it matches something older. The properties are exactly the ones the
// rounding contract promises, because a contract nothing checks is prose.
//
// Returns 0 on pass; otherwise a bitmask naming which property failed, so a
// boot log says WHICH rule broke rather than "self-test failed".
// ===========================================================================

pub const ST_IDENTITY: u32 = 1 << 0;
pub const ST_NEVER_ZERO: u32 = 1 << 1;
pub const ST_EDGE_SHARED: u32 = 1 << 2;
pub const ST_ROUNDTRIP: u32 = 1 << 3;
pub const ST_MONOTONIC: u32 = 1 << 4;
pub const ST_CAP: u32 = 1 << 5;
pub const ST_AUTO: u32 = 1 << 6;
pub const ST_CLAMP: u32 = 1 << 7;

#[no_mangle]
pub extern "C" fn uiscale_selftest_rs() -> u32 {
    let saved_pct = PCT.load(Ordering::Relaxed);
    let saved_src = SRC.load(Ordering::Relaxed);
    let mut bad: u32 = 0;

    // The factors that matter: 1x (must be a bit-exact no-op), the two
    // fractional ones that are hard, and 2x/3x.
    let factors = [100i32, 125, 150, 175, 200, 300];

    for &f in factors.iter() {
        PCT.store(f, Ordering::Relaxed);

        // IDENTITY: at 100 every function is the identity, so a 1x machine is
        // byte-identical to a machine without this module.
        if f == 100 {
            for v in -64i32..1024 {
                if uiscale_px_rs(v) != v || uiscale_unpx_rs(v) != v {
                    bad |= ST_IDENTITY;
                }
                if uiscale_span_rs(v, 7) != 7 {
                    bad |= ST_IDENTITY;
                }
            }
        }

        // NEVER ZERO: a 1px hairline survives at every factor, and so does a
        // 1px span at any origin.
        if uiscale_px_rs(1) < 1 {
            bad |= ST_NEVER_ZERO;
        }
        for o in 0i32..64 {
            if uiscale_span_rs(o, 1) < 1 {
                bad |= ST_NEVER_ZERO;
            }
        }

        // EDGE SHARED: this is the property that stops 1.25x and 1.5x looking
        // uneven. Boxes tiled edge to edge must stay tiled edge to edge: the
        // scaled right edge of one is the scaled left edge of the next, with
        // no overlap and no gap, for every origin and every width.
        for x in 0i32..200 {
            for w in 1i32..40 {
                let a = uiscale_px_rs(x);
                let wa = uiscale_span_rs(x, w);
                let b = uiscale_px_rs(x + w);
                if a + wa != b {
                    bad |= ST_EDGE_SHARED;
                }
            }
        }

        // ROUNDTRIP: every physical pixel inside a logical pixel maps back to
        // that logical pixel. This is the hit-testing guarantee: a control
        // drawn at logical x is hit by every physical pixel it covers, and by
        // none that it does not. If this fails, controls go dead near edges.
        for l in 0i32..400 {
            let p0 = uiscale_px_rs(l);
            let p1 = uiscale_px_rs(l + 1);
            let mut p = p0;
            while p < p1 {
                if uiscale_unpx_rs(p) != l {
                    bad |= ST_ROUNDTRIP;
                }
                p += 1;
            }
        }

        // MONOTONIC: scaling never reorders. Two things that were apart stay
        // apart, in the same order.
        let mut prev = uiscale_px_rs(-100);
        for v in -99i32..1000 {
            let c = uiscale_px_rs(v);
            if c < prev {
                bad |= ST_MONOTONIC;
            }
            prev = c;
        }
    }

    // CAP: the logical floor is respected, and the answer is always a usable
    // step. Checked on the three shapes that matter: the test VMs, the owner's
    // two candidate panels, and a 4K panel.
    //  1280x800 -> by_w 125, by_h 133 -> 125
    //  1920x1080 -> by_w 187, by_h 180 -> 175
    //  3840x2160 -> by_w 375, by_h 360 -> 300 (PCT_MAX)
    //  1366x768  -> by_w 133, by_h 128 -> 125
    if uiscale_max_pct_rs(1280, 800) != 125 { bad |= ST_CAP; }
    if uiscale_max_pct_rs(1920, 1080) != 175 { bad |= ST_CAP; }
    if uiscale_max_pct_rs(3840, 2160) != 300 { bad |= ST_CAP; }
    if uiscale_max_pct_rs(1366, 768) != 125 { bad |= ST_CAP; }
    if uiscale_max_pct_rs(0, 0) != 100 { bad |= ST_CAP; }
    // And the floor actually holds: at the returned cap the logical screen is
    // still at least the minimum, for every mode we can enumerate cheaply.
    {
        let modes = [
            (1024i32, 768i32), (1280, 720), (1280, 800), (1366, 768),
            (1440, 900), (1600, 900), (1680, 1050), (1920, 1080),
            (1920, 1200), (2560, 1440), (3200, 1800), (3840, 2160),
        ];
        for &(w, h) in modes.iter() {
            let c = uiscale_max_pct_rs(w, h);
            if (w * 100 / c) < MIN_LOGICAL_W || (h * 100 / c) < MIN_LOGICAL_H {
                bad |= ST_CAP;
            }
        }
    }

    // AUTO: the documented policy, branch by branch, including the one that
    // matters most - that a 1920x1080 DESKTOP panel is left at 1x, so this
    // change cannot alter what the iMac and every test VM already look like.
    if uiscale_auto_pct_rs(1280, 800, 0) != 100 { bad |= ST_AUTO; }
    if uiscale_auto_pct_rs(1280, 800, 1) != 100 { bad |= ST_AUTO; }
    if uiscale_auto_pct_rs(1920, 1080, 0) != 100 { bad |= ST_AUTO; }
    // 1920x1080 MUST BE 1x FROM EVERY PATH, including a machine that reports a
    // battery. See the 1600-2559 branch: at this resolution we cannot tell a
    // native 1080p panel from a 4K panel driven at 1080p by \boot\MODE.TXT, and
    // scaling the second one would be four times too large.
    if uiscale_auto_pct_rs(1920, 1080, 1) != 100 { bad |= ST_AUTO; }
    // Unknown must behave as "not a laptop": never bigger on a non-measurement.
    if uiscale_auto_pct_rs(1920, 1080, -1) != 100 { bad |= ST_AUTO; }
    if uiscale_auto_pct_rs(2560, 1440, 0) != 150 { bad |= ST_AUTO; }
    // THE OWNER'S ACTUAL MACHINE, by its measured framebuffer geometry.
    // Asserted for all three values of the laptop probe, because the answer
    // must not depend on whether the ACPI scan happened to work: a 4K panel
    // wants scaling whether or not the firmware would admit to a battery.
    if uiscale_auto_pct_rs(3840, 2160, 0) != 200 { bad |= ST_AUTO; }
    if uiscale_auto_pct_rs(3840, 2160, 1) != 200 { bad |= ST_AUTO; }
    if uiscale_auto_pct_rs(3840, 2160, -1) != 200 { bad |= ST_AUTO; }
    // And 200% must actually be REACHABLE on that panel - a policy that names
    // a factor the cap would claw back is a policy that does not apply.
    if uiscale_max_pct_rs(3840, 2160) < 200 { bad |= ST_AUTO; }
    // The other 4K-class shapes the same laptop could be set to.
    if uiscale_auto_pct_rs(3200, 1800, 0) != 200 { bad |= ST_AUTO; }
    if uiscale_auto_pct_rs(2560, 1600, 1) != 150 { bad |= ST_AUTO; }
    // THE TWO CONFIGURATIONS THE OWNER'S MACHINE CAN BE IN, ASSERTED TOGETHER
    // AND FROM THE SAME CODE PATH, because he can add or delete \boot\MODE.TXT
    // at any time and both must be right with no rebuild:
    //   native panel, MODE.TXT absent  -> 3840x2160 -> 200%
    //   MODE.TXT = 1920x1080           -> 1920x1080 -> 100%
    if uiscale_auto_pct_rs(3840, 2160, 1) != 200 { bad |= ST_AUTO; }
    if uiscale_auto_pct_rs(1920, 1080, 1) != 100 { bad |= ST_AUTO; }
    // A small laptop panel must not be scaled up past what it can carry: the
    // cap wins over the policy.
    if uiscale_auto_pct_rs(1600, 900, 1) != 100 { bad |= ST_AUTO; }
    if uiscale_auto_pct_rs(1366, 768, 1) != 100 { bad |= ST_AUTO; }

    // CLAMP: out-of-range input is bounded rather than obeyed, and the
    // adopted value is what is returned.
    if uiscale_set_pct_rs(9999, SRC_USER) != PCT_MAX { bad |= ST_CLAMP; }
    if uiscale_set_pct_rs(-5, SRC_USER) != PCT_MIN { bad |= ST_CLAMP; }

    PCT.store(saved_pct, Ordering::Relaxed);
    SRC.store(saved_src, Ordering::Relaxed);
    bad
}
