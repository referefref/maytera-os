// wallpaper.c - Wallpaper system for the MayteraOS userland compositor
// Loads BMP files from the FAT filesystem, scales them to the screen,
// and provides an interactive picker dialog for the user.
// No dynamic allocation: all buffers are static and sized at compile time.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/wallpapers.h"
#include "screensaver_gfx.h"   // #wpanim: reuse the low-res buffer + clipped upscale pipeline
#include "wallpaper_anim.h"    // #wpanim: the four animated effects

// ============================================================================
// Wallpaper list (#517: data-driven, no longer a hardcoded subset)
//
// The list used to be a hardcoded array with three DEAD entries (CLASSIC.BMP /
// DARKMODE.BMP / RETRO.BMP were absent from the image and fell back to a gradient)
// while 47 of the 65 shipped BMPs were unreachable. It is now enumerated at init
// from the filesystem via the shared wp_enumerate() (see libc/wallpapers.h), which
// Settings uses too, so their shared index cannot reference an absent file or
// diverge. A gradient entry is always present as the final entry.
// ============================================================================

static wp_entry_t g_wallpapers[WP_MAX_ENTRIES];
static int        g_wallpaper_count = 0;
// entry.file[0] == 0 means "gradient" (no BMP); mirrors the old NULL filename.
#define WP_IS_GRADIENT(i) (g_wallpapers[i].file[0] == 0)

// ============================================================================
// Static pixel buffer (holds the decoded wallpaper image)
// ============================================================================

static uint32_t g_wp_pixels[MAX_SCREEN_W * MAX_SCREEN_H];
static int      g_wp_width;
static int      g_wp_height;
static bool     g_wp_loaded;

// ============================================================================
// Picker state
// ============================================================================

static int g_current_wallpaper;
static int g_picker_scroll;
static int g_picker_hover;

// ============================================================================
// #wpanim: animated, window-reactive wallpaper state (owner request
// 2026-09-18). Compositor-local only, no new syscall (kernel has no notion
// of this - unlike the static-wallpaper index, which the kernel tracks so
// Settings and the compositor agree, this is compositor-chrome-only, the
// same way the picker's scroll position is).
//
// #wpcolor (2026-09-18, second owner pass): the DEFAULT is now OFF (plain
// static wallpaper, same as before #wpanim ever existed). The owner was
// explicit: "Default should be static as before, [B]oating during install" -
// the previous default (WPANIM_PLASMA) shipped an animated wallpaper ON by
// default, which is exactly the regression being fixed here. Animated
// effects remain fully available, opt-in only via this picker's strip.
// ============================================================================

static int g_wp_anim_mode      = WPANIM_OFF;
static int g_wp_anim_intensity = 55;
static int g_wp_anim_repel     = 1;   // 1 = repel (default), 0 = attract
// #wpcolor: base hue (0..360) and colour source. SPECTRUM + hue 200 is
// chosen so an old profile with no wallpaper_anim_hue/_palette key at all
// (prof_apply leaves these at their compile-time default) reproduces the
// pre-#wpcolor look byte-for-byte: wp_resolve_hue() for SPECTRUM is
// `hue_base + dynamic_hue`, and the old formulas never added a base hue, so
// hue_base must be a value that leaves the LOOK equivalent after the 0..360
// wrap - since the old hue formulas were already unbounded and wrapped
// inside wp_hsl(), any fixed offset only shifts the animation's starting
// phase, not its character; 200 is simply a pleasant default, not load
// -bearing for compatibility the way WPANIM_OFF/55/1 above are.
static float g_wp_anim_hue     = 200.0f;
static int   g_wp_anim_palette = WP_PALETTE_SPECTRUM;

int  get_wallpaper_anim(void)            { return g_wp_anim_mode; }
void set_wallpaper_anim(int mode)
{
    if (mode < 0 || mode >= WPANIM_MODE_COUNT) mode = WPANIM_OFF;
    g_wp_anim_mode = mode;
}
int  get_wallpaper_anim_intensity(void)  { return g_wp_anim_intensity; }
void set_wallpaper_anim_intensity(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_wp_anim_intensity = v;
}
int  get_wallpaper_anim_repel(void)      { return g_wp_anim_repel; }
void set_wallpaper_anim_repel(int repel) { g_wp_anim_repel = repel ? 1 : 0; }

// #wpcolor: base hue getter/setter takes/returns an int (0..360) at this
// boundary because profile.c's whole key/value format is integer-only (see
// prof_atoi()/put_kv() - there is no float persistence anywhere in this
// file), even though wpanim_render() itself wants a float. The internal
// state is stored as float so the picker's slider can address all 360
// values 1:1 without a lossy round trip through int on every mouse-move.
int  get_wallpaper_anim_hue(void)        { return (int)g_wp_anim_hue; }
void set_wallpaper_anim_hue(int deg)
{
    while (deg < 0)    deg += 360;
    while (deg >= 360) deg -= 360;
    g_wp_anim_hue = (float)deg;
}
int  get_wallpaper_anim_palette(void)    { return g_wp_anim_palette; }
void set_wallpaper_anim_palette(int p)
{
    if (p < 0 || p >= WP_PALETTE_COUNT) p = WP_PALETTE_SPECTRUM;
    g_wp_anim_palette = p;
}

// ============================================================================
// File read buffer (4 MB; fits the largest expected BMP wallpaper)
// ============================================================================

static uint8_t g_file_buf[4 * 1024 * 1024];

// ============================================================================
// Internal: read a 2-byte little-endian value from a byte array
// ============================================================================

static uint32_t read_u16_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

// ============================================================================
// Internal: read a 4-byte little-endian value from a byte array
// ============================================================================

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// ============================================================================
// parse_bmp
// Decodes an uncompressed 24-bit or 32-bit BMP from the in-memory buffer.
// On success the pixels are stored in g_wp_pixels and 0 is returned.
// On failure -1 is returned and g_wp_loaded is left unchanged.
// ============================================================================

static int parse_bmp_step(const uint8_t *data, uint32_t size, int step)
{
    if (step < 1) step = 1;
    // Minimum viable BMP header is 54 bytes.
    if (size < 54) return -1;

    // Check BMP signature.
    if (data[0] != 'B' || data[1] != 'M') return -1;

    // Offset to the first pixel byte.
    uint32_t pixel_offset = read_u32_le(data + 10);
    if (pixel_offset >= size) return -1;

    // DIB header size (determines which variant of the DIB header is in use).
    // We only need fields common to BITMAPINFOHEADER (size >= 40).
    uint32_t dib_size = read_u32_le(data + 14);
    if (dib_size < 40) return -1;   // older OS/2 format; not supported

    int32_t  width  = (int32_t)read_u32_le(data + 18);
    int32_t  height = (int32_t)read_u32_le(data + 22); // positive = bottom-up
    uint32_t bpp    = read_u16_le(data + 28);

    // Only 24-bit and 32-bit uncompressed formats are supported.
    if (bpp != 24 && bpp != 32) return -1;

    // Reject pathological sizes.
    if (width  <= 0 || width  > MAX_SCREEN_W) return -1;
    if (height <= 0 || height > MAX_SCREEN_H) return -1;

    // Row stride in the file must be 4-byte aligned.
    uint32_t bytes_per_pixel = bpp / 8;
    uint32_t stride = ((uint32_t)(width) * bytes_per_pixel + 3) & ~(uint32_t)3;

    // Verify the file is large enough to hold all pixel data.
    if (pixel_offset + stride * (uint32_t)height > size) return -1;

    // Decode rows: BMP stores bottom-up, so row 0 in the file is the bottom
    // row of the image. We reverse this when writing to g_wp_pixels. When
    // step>1 we point-sample every step-th row/column for a fast coarse pass;
    // wallpaper_render_background nearest-neighbor upscales any g_wp size to
    // the screen, so a coarse buffer just looks blocky until the full pass.
    int32_t out_w = (width  + step - 1) / step;
    int32_t out_h = (height + step - 1) / step;
    for (int32_t orow = 0; orow < out_h; orow++) {
        int32_t row = orow * step;
        uint32_t src_row_idx = (uint32_t)(height - 1 - row);
        const uint8_t *src = data + pixel_offset + src_row_idx * stride;
        uint32_t *dst = g_wp_pixels + (uint32_t)orow * (uint32_t)out_w;
        for (int32_t ocol = 0; ocol < out_w; ocol++) {
            const uint8_t *p = src + (uint32_t)(ocol * step) * bytes_per_pixel;
            dst[ocol] = 0xFF000000u
                      | ((uint32_t)p[2] << 16)
                      | ((uint32_t)p[1] <<  8)
                      | (uint32_t)p[0];
        }
    }

    g_wp_width  = (int)out_w;
    g_wp_height = (int)out_h;
    return 0;
}

static int parse_bmp(const uint8_t *data, uint32_t size)
{
    return parse_bmp_step(data, size, 1);
}

// ============================================================================
// wallpaper_load
// Opens the BMP file for wallpaper[index], reads it in 64 KB chunks into
// g_file_buf, then calls parse_bmp.  On success g_wp_loaded is set to true.
// ============================================================================

void wallpaper_load(int index)
{
    // Validate index and check that a filename is provided.
    if (index < 0 || index >= g_wallpaper_count
            || WP_IS_GRADIENT(index)) {
        g_wp_loaded = false;
        return;
    }

    const char *filename = g_wallpapers[index].file;

    int fd = sys_open(filename, 0);
    if (fd < 0) {
        g_wp_loaded = false;
        return;
    }

    // Read the entire file in 64 KB chunks.
    uint32_t total = 0;
    uint32_t max   = sizeof(g_file_buf);
    while (total < max) {
        uint32_t chunk = 64 * 1024;
        if (chunk > max - total) chunk = max - total;
        long n = sys_read(fd, g_file_buf + total, chunk);
        if (n <= 0) break;
        total += (uint32_t)n;
    }

    sys_close(fd);

    if (total < 54) {
        // File too small to be a valid BMP.
        g_wp_loaded = false;
        return;
    }

    if (parse_bmp(g_file_buf, total) == 0) {
        g_wp_loaded         = true;
        g_current_wallpaper = index;
    } else {
        g_wp_loaded = false;
    }
}

// ===========================================================================
// Progressive load (#246): read the file once, decode a fast coarse (1/4)
// preview immediately, and flag a full-resolution refine for the next frame.
// ===========================================================================
static bool     g_wp_refine_pending = false;
static uint32_t g_wp_refine_total   = 0;

void wallpaper_load_progressive(int index)
{
    if (index < 0 || index >= g_wallpaper_count
            || WP_IS_GRADIENT(index)) {
        g_wp_loaded = false;
        g_wp_refine_pending = false;
        return;
    }
    int fd = sys_open(g_wallpapers[index].file, 0);
    if (fd < 0) { g_wp_loaded = false; g_wp_refine_pending = false; return; }
    uint32_t total = 0, max = sizeof(g_file_buf);
    while (total < max) {
        uint32_t chunk = 64 * 1024;
        if (chunk > max - total) chunk = max - total;
        long n = sys_read(fd, g_file_buf + total, chunk);
        if (n <= 0) break;
        total += (uint32_t)n;
    }
    sys_close(fd);
    if (total < 54) { g_wp_loaded = false; g_wp_refine_pending = false; return; }

    // Fast coarse pass (1/4 in each dim = ~16x fewer pixels) shown immediately.
    if (parse_bmp_step(g_file_buf, total, 4) == 0) {
        g_wp_loaded         = true;
        g_current_wallpaper = index;
        g_wp_refine_total   = total;
        g_wp_refine_pending = true;
    } else {
        g_wp_loaded = false;
        g_wp_refine_pending = false;
    }
}

// Complete the full-resolution decode if one is pending. Returns 1 if it did
// work (so the caller can request a redraw), 0 otherwise.
int wallpaper_refine(void)
{
    if (!g_wp_refine_pending) return 0;
    g_wp_refine_pending = false;
    if (g_wp_refine_total >= 54) {
        parse_bmp_step(g_file_buf, g_wp_refine_total, 1);
        return 1;
    }
    return 0;
}

// ============================================================================
// wallpaper_init
// Must be called once before any other wallpaper function.
// ============================================================================

// Returns the index of the currently loaded wallpaper (for cross-app sync).
int wallpaper_current(void)
{
    return g_current_wallpaper;
}

// Case-sensitive exact match; local to this file, freestanding (no libc string.h).
static bool wp_name_eq(const char *a, const char *b)
{
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return false;
    return a[i] == b[i];
}

// The out-of-box default wallpaper (#745, second pass). Before #517 the default
// was whichever entry a hardcoded array put first; #517 made the list itself
// data-driven but left the default AS index 0 of wp_enumerate()'s scan order,
// which is the ext2 directory's raw on-disk entry order (creation/deletion
// history, NOT alphabetical - see blame.md, "debugfs order and Linux readdir
// order are DIFFERENT ORDERS"). That was an invisible dependency on write
// order for as long as the shipped set never changed; the 2026-08-11 library
// reset changed it, and index 0 became whatever wallpaper happened to land
// first in the rebuilt directory, not a deliberate choice. This name is the
// direct successor of the old default's spirit (MAYTERA.BMP / "Maytera
// Modern" was the first, primary entry in wp_pretty()'s now-removed curated
// table) and is what kernel/gui/login.c and kernel/gui/desktop.c also try
// first for their own BACK.BMP-shaped defaults, so all three surfaces agree
// on one wallpaper rather than each falling back to gradient or to whatever
// directory order gives them independently.
#define WP_DEFAULT_FILE "ABSTRACT_13.BMP"

void wallpaper_init(void)
{
    g_wp_loaded         = false;
    g_wp_width          = 0;
    g_wp_height         = 0;
    g_current_wallpaper = 0;
    g_picker_scroll     = 0;
    g_picker_hover      = -1;

    // #517: enumerate the wallpapers actually present on the image (shared with
    // Settings so indices agree). Always returns >= 1 (the gradient entry).
    g_wallpaper_count = wp_enumerate(g_wallpapers, WP_MAX_ENTRIES);

    // Prefer the deliberate default (see WP_DEFAULT_FILE above) if it is on
    // the image; fall back to index 0 (old behaviour) if it has been removed
    // or renamed, so this degrades gracefully instead of failing to load
    // anything.
    int default_index = 0;
    for (int i = 0; i < g_wallpaper_count; i++) {
        if (wp_name_eq(g_wallpapers[i].file, WP_DEFAULT_FILE)) {
            default_index = i;
            break;
        }
    }

    // Attempt to load the default wallpaper; if it fails the gradient is used.
    wallpaper_load(default_index);

    // (wallpersist, no-ticket) SYNC THE KERNEL'S CROSS-APP INDEX, OR THE VERY
    // NEXT FRAME UNDOES THIS CHOICE. main.c's main loop polls get_wallpaper()
    // (SYS_GET_WALLPAPER, the kernel's g_wallpaper_idx - a per-BOOT int that
    // starts at 0 and exists so Settings and the compositor agree on the live
    // wallpaper) every frame, and reloads whatever that value names the moment
    // it differs from wallpaper_current(). wallpaper_load() above only sets
    // this file's OWN g_current_wallpaper; it never told the kernel. On a
    // fresh boot with no persisted profile the kernel's index is still its
    // boot default of 0, so on THE VERY NEXT FRAME that poll saw
    // get_wallpaper()==0 != wallpaper_current()==default_index and forced a
    // reload of index 0 - silently undoing the WP_DEFAULT_FILE preference
    // just chosen above, every single time, before a single frame was ever
    // presented. MEASURED (2026-09-11, golden 2426, throwaway VM 2900): a
    // fresh account with no UIPROFIL.YML loaded ABSTRACT_13.BMP (index 5 on
    // this image) for one instant, then the profile that got auto-saved a
    // second later read "wallpaper: 0", and the desktop showed BOATING.BMP
    // (whatever landed at raw enumeration index 0) instead - exactly the
    // "reverts to the default BOATING" shape the owner reported, but
    // reproducible from a plain first boot, with no stick refresh or
    // keepstate involved at all. set_wallpaper() is the same one-line syscall
    // the picker's own mouse handler already calls after every real
    // selection (see the picker click handler below); calling it here too
    // means the deliberate default and a real click are seeded into the
    // kernel's live index identically, so this poll has nothing to correct.
    set_wallpaper(default_index);
}

// ============================================================================
// wallpaper_render_background
// Draws either the loaded BMP (scaled if necessary) or a vertical gradient.
// ============================================================================

// #wpanim: FPS cap for the animated-wallpaper recompute, NOT for the blit.
// An animated background is logically always-dirty while any desktop is
// visible (there is no static content to damage-cull against), so the real
// cost control is this cap, matching the screensaver's own
// SS_FRAME_MIN_MS-style throttle - see screensaver_gfx.c's file header for
// the measured 11-18fps/34-58%-of-one-core full-screen low-res+upscale
// budget this rides on. 50ms = 20fps, inside the 40-66ms/15-25fps range
// this was scoped against. The BLIT (ss_lores_upscale_to_fb_clipped) still
// runs on every call so a plain cursor-move repaint sees the last computed
// frame instead of a stale hole, exactly like a static wallpaper would.
#define WPANIM_FRAME_MIN_MS 50

static uint64_t s_wpanim_last_tick_ms = 0;
static int      s_wpanim_buf_ready    = 0;

// #wpcolor (owner request 2026-09-18: "window positions AND contents are
// properly influencing the effects in real time"). Average colour under a
// window's rect, sampled directly from g_fb.
//
// SAMPLING METHOD CHOSEN, AND WHY (see the four options weighed in the task
// spec): this reads g_fb itself, in FRAMEBUFFER pixel space, at the START of
// the rate-limited wpanim tick below - i.e. BEFORE this tick's
// wpanim_render()+blit touches a single pixel. The compositor's own draw
// order is wallpaper first, then window contents on top, every frame - so
// at this exact point g_fb still holds frame N-1's fully composited image
// everywhere, window rects included (this tick has not painted anything
// yet). That is option (a) from the spec (sample the previous frame's
// composited buffer) with no new syscall and no persistent cross-frame
// cache needed: because this whole block already only runs once per
// WPANIM_FRAME_MIN_MS, "sample now, use now" already IS "cache after frame
// N, use in frame N+1" - the cache is just this tick's own stack, since the
// next real use is the very call a few lines below. If a window's content
// changes colour (a video/media window), the NEXT tick's sample sees it -
// at most WPANIM_FRAME_MIN_MS point (50ms) after it changed, i.e. "within a
// frame or two" of this module's own cadence, matching the requirement.
//
// Downsampled hard (WP_CONTENT_SAMPLES^2 = 16 taps per window, <=16 windows
// = <=256 pixel reads per tick) to stay cheap on the single draw thread
// (#426) - this runs inside the same uptime_ms()-gated tick as the effect
// recompute itself, never on every draw-thread frame.
#define WP_CONTENT_SAMPLES 4

static void wp_sample_window_content(int32_t fx, int32_t fy, int32_t fw, int32_t fh,
                                      uint8_t *out_r, uint8_t *out_g, uint8_t *out_b)
{
    int32_t x0 = fx, y0 = fy, x1 = fx + fw, y1 = fy + fh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_fb_width)  x1 = g_fb_width;
    if (y1 > g_fb_height) y1 = g_fb_height;
    if (x1 <= x0 || y1 <= y0 || !g_fb) { *out_r = *out_g = *out_b = 0; return; }

    int64_t sr = 0, sg = 0, sb = 0;
    int     n  = 0;
    for (int j = 0; j < WP_CONTENT_SAMPLES; j++) {
        int32_t py = y0 + ((y1 - y0) * (j * 2 + 1)) / (WP_CONTENT_SAMPLES * 2);
        if (py < y0) py = y0;
        if (py >= y1) py = y1 - 1;
        const uint32_t *row = g_fb + (uint32_t)py * (uint32_t)g_fb_pitch;
        for (int i = 0; i < WP_CONTENT_SAMPLES; i++) {
            int32_t px = x0 + ((x1 - x0) * (i * 2 + 1)) / (WP_CONTENT_SAMPLES * 2);
            if (px < x0) px = x0;
            if (px >= x1) px = x1 - 1;
            uint32_t c = row[px];
            sr += (int64_t)((c >> 16) & 0xFF);
            sg += (int64_t)((c >> 8)  & 0xFF);
            sb += (int64_t)(c & 0xFF);
            n++;
        }
    }
    if (n <= 0) n = 1;
    *out_r = (uint8_t)(sr / n);
    *out_g = (uint8_t)(sg / n);
    *out_b = (uint8_t)(sb / n);
}

// #wpanim BUGFIX: main.c's render-path decision (partial/chrome/cursor-only
// vs full) happens BEFORE render_frame_body() ever runs this tick, so a flag
// set from INSIDE wallpaper_render_animated() (like g_needs_redraw) can only
// ever affect a FUTURE tick, and main.c unconditionally clears g_needs_redraw
// right after whichever path it picks - so it never accumulates across ticks
// either. main.c needs to know BEFOREHAND whether a new animation frame is
// due, so it can fold that into its own ui_busy decision (see main.c's
// ui_busy computation, the same treatment g_session_locked/
// screenshot_fs_toast_active() etc. already get) and take the full render
// path on exactly the ticks that matter - not every tick, which would defeat
// the FPS cap, and not zero ticks, which is the bug this fixes. Same clock
// wallpaper_render_animated() itself reads, so there is exactly one timer,
// not two that could drift.
//
// MEASURED (throwaway VM 2213/2216/2218, 2026-09-18): three screendumps of a
// FLOW effect 16+ seconds apart were byte-identical (md5-confirmed) on an
// idle desktop with a static window open, while the guest was demonstrably
// NOT hung (CPU non-zero, windows composited/placed correctly). This fix
// (folding wallpaper_anim_due() into main.c's ui_busy) is the correct
// architectural answer to that class of bug, but could NOT be conclusively
// re-verified as fully resolving it in the same constrained headless rig
// (no input device attached, per docs/TEST_VM_RECIPE.md's own recipe; a
// control test with the SAME rig and wallpaper_anim OFF showed the identical
// symptom - the desktop clock itself froze at the same idle point - proving
// this is a broader, PRE-EXISTING idle-repaint characteristic of the
// compositor, not something wpanim introduced or something this one fix can
// fully own). See blame.md for the full writeup; flagged here as a known
// follow-up rather than silently assumed fixed.
bool wallpaper_anim_due(void)
{
    if (g_wp_anim_mode == WPANIM_OFF) return false;
    if (!s_wpanim_buf_ready) return true;
    return ((uint64_t)uptime_ms() - s_wpanim_last_tick_ms) >= WPANIM_FRAME_MIN_MS;
}

// Runs in place of the BMP/gradient path when an animated mode is selected.
// Never blocks, never sleeps (#426): the recompute itself is gated on
// uptime_ms(), not a wait - a frame that arrives early just reuses the
// cached low-res buffer from last tick.
// #emfield (owner request 2026-09-22): per-window VELOCITY tracking, entirely
// compositor-side. The kernel window info carries no velocity or previous
// position (wm_window_info_t, libc/syscall.h), so we remember each window's
// previous CENTRE in low-res cell space keyed by its stable window id and
// derive velocity = (now - prev) per tick, smoothed. Only wpanim's EMFIELD
// mode reads wr[].vx/vy, but the table is refreshed on every animated tick so
// it is already warm the instant EMFIELD is selected. Bounded (16 slots),
// non-blocking, no allocation - it lives on the same rate-limited recompute
// tick as everything else here (#426).
#define WP_VEL_SLOTS 16
typedef struct { int id; int used; int seen; float cx, cy, vx, vy; } wp_vel_slot_t;
static wp_vel_slot_t s_wp_vel[WP_VEL_SLOTS];

// Mark every slot unseen at the start of a tick so windows that have since
// closed can be reaped at the end (below).
static void wp_vel_begin_tick(void)
{
    for (int i = 0; i < WP_VEL_SLOTS; i++) s_wp_vel[i].seen = 0;
}

// Update the slot for window `id` with its current low-res centre (cx,cy) and
// return the smoothed velocity via *ovx/*ovy. A window seen for the first time
// (or after a table-full eviction) reports zero velocity - a still window, the
// correct default (no vortex until it actually moves).
static void wp_vel_update(int id, float cx, float cy, float *ovx, float *ovy)
{
    int slot = -1, freeslot = -1;
    for (int i = 0; i < WP_VEL_SLOTS; i++) {
        if (s_wp_vel[i].used && s_wp_vel[i].id == id) { slot = i; break; }
        if (!s_wp_vel[i].used && freeslot < 0) freeslot = i;
    }
    if (slot < 0) slot = (freeslot >= 0) ? freeslot : 0;   // table full: reuse slot 0

    wp_vel_slot_t *v = &s_wp_vel[slot];
    if (v->used && v->id == id) {
        float rawx = cx - v->cx, rawy = cy - v->cy;
        // Exponential smoothing so a single jittery frame does not spike the
        // vortex, while a sustained drag builds up quickly.
        v->vx = v->vx * 0.55f + rawx * 0.45f;
        v->vy = v->vy * 0.55f + rawy * 0.45f;
    } else {
        v->id = id; v->used = 1; v->vx = 0.0f; v->vy = 0.0f;
    }
    v->cx = cx; v->cy = cy; v->seen = 1;
    *ovx = v->vx; *ovy = v->vy;
}

// Free slots whose window was not seen this tick (window closed/minimized).
static void wp_vel_end_tick(void)
{
    for (int i = 0; i < WP_VEL_SLOTS; i++)
        if (s_wp_vel[i].used && !s_wp_vel[i].seen) s_wp_vel[i].used = 0;
}

static void wallpaper_render_animated(void)
{
    int32_t cx0 = g_clip_x0 < 0 ? 0 : g_clip_x0;
    int32_t cy0 = g_clip_y0 < 0 ? 0 : g_clip_y0;
    int32_t cx1 = g_clip_x1 > g_fb_width  ? g_fb_width  : g_clip_x1;
    int32_t cy1 = g_clip_y1 > g_fb_height ? g_fb_height : g_clip_y1;
    if (cx1 <= cx0 || cy1 <= cy0) return;

    // LO buffer (200x125): all four effects have a per-cell or per-particle
    // inner loop heavier than the plain wallpaper blit (domain warp / field
    // tracing), matching screensaver_gfx.h's own guidance on which standard
    // size to pick for that cost class.
    uint32_t *buf = ss_lores_buf(SS_LORES_LO_W, SS_LORES_LO_H);
    if (!buf) {
        // Never crash: same defensive fallback screensaver_gfx.c documents
        // for its own malloc-failure path (should not happen in practice).
        draw_gradient_v(cx0, cy0, cx1 - cx0, cy1 - cy0, CLR_WP_GRAD_TOP, CLR_WP_GRAD_BOT);
        return;
    }

    uint64_t now = (uint64_t)uptime_ms();
    if (!s_wpanim_buf_ready || (now - s_wpanim_last_tick_ms) >= WPANIM_FRAME_MIN_MS) {
        s_wpanim_last_tick_ms = now;

        // Live window rects, translated into the low-res buffer's coordinate
        // space (wallpaper_anim.h's contract - see its file header). Fetched
        // here rather than threaded in from main.c's own wm_get_windows()
        // call: this only runs on the rate-limited recompute tick (at most
        // once per WPANIM_FRAME_MIN_MS), not every frame, so the extra
        // bounded, non-blocking syscall (wm_get_windows is a linked-list
        // walk under a uaccess bracket, no wait_event - see sys_wm_get_windows,
        // kernel/gui/window.c) costs nothing on the common path where this
        // tick is skipped.
        wm_window_info_t wins[16];
        int n = wm_get_windows(wins, 16);
        wp_win_rect_t wr[16];
        int nw = 0;
        float sxr = (g_fb_width  > 0) ? (float)SS_LORES_LO_W / (float)g_fb_width  : 1.0f;
        float syr = (g_fb_height > 0) ? (float)SS_LORES_LO_H / (float)g_fb_height : 1.0f;
        wp_vel_begin_tick();   // #emfield: age out closed windows after the loop
        for (int i = 0; i < n && nw < 16; i++) {
            if (!wins[i].visible || wins[i].minimized) continue;
            wr[nw].x = (int32_t)((float)wins[i].x      * sxr);
            wr[nw].y = (int32_t)((float)wins[i].y      * syr);
            wr[nw].w = (int32_t)((float)wins[i].width  * sxr);
            wr[nw].h = (int32_t)((float)wins[i].height * syr);
            if (wr[nw].w < 1) wr[nw].w = 1;
            if (wr[nw].h < 1) wr[nw].h = 1;
            wr[nw].focused = wins[i].focused ? 1 : 0;
            // #wpcolor: sample this window's own content colour in
            // FRAMEBUFFER pixel space (wins[i].x/y/width/height, NOT the
            // already-downscaled wr[nw].x/y/w/h above) - see
            // wp_sample_window_content()'s header for why g_fb still holds
            // the right pixels here. Always set has_content=1: even a
            // freshly-created window's backing pixels are SOME colour (its
            // clear/background), which is a reasonable tint from frame one
            // rather than leaving CONTENT mode with nothing to show.
            wp_sample_window_content(wins[i].x, wins[i].y, wins[i].width, wins[i].height,
                                      &wr[nw].cr, &wr[nw].cg, &wr[nw].cb);
            wr[nw].has_content = 1;
            // #emfield: smoothed per-window velocity in low-res cell space,
            // keyed by the stable window id. Zero for still windows and the
            // frame a window first appears; only EMFIELD uses it.
            {
                float wcx = (float)wr[nw].x + (float)wr[nw].w * 0.5f;
                float wcy = (float)wr[nw].y + (float)wr[nw].h * 0.5f;
                wp_vel_update(wins[i].id, wcx, wcy, &wr[nw].vx, &wr[nw].vy);
            }
            nw++;
        }
        wp_vel_end_tick();

        wpanim_render(g_wp_anim_mode, buf, SS_LORES_LO_W, SS_LORES_LO_H,
                      wr, nw, now, g_wp_anim_intensity, g_wp_anim_repel,
                      g_wp_anim_hue, g_wp_anim_palette);
        s_wpanim_buf_ready = 1;
    }

    // #102/#379 dirty-rect: blit only the intersection of the screen and the
    // active clip rectangle, same contract as the static-wallpaper path
    // below.
    ss_lores_upscale_to_fb_clipped(buf, SS_LORES_LO_W, SS_LORES_LO_H, cx0, cy0, cx1, cy1);
}

void wallpaper_render_background(void)
{
    // #wpanim: an animated mode REPLACES both the BMP and the gradient
    // fallback below - there is no "animated over static" layering.
    if (g_wp_anim_mode != WPANIM_OFF) {
        wallpaper_render_animated();
        return;
    }

    if (!g_wp_loaded || g_wp_width <= 0 || g_wp_height <= 0) {
        // Fall back to a vertical gradient.
        draw_gradient_v(0, 0, g_fb_width, g_fb_height,
                        CLR_WP_GRAD_TOP, CLR_WP_GRAD_BOT);
        return;
    }

    // #102/#379 dirty-rect: only blit the intersection of the screen and the
    // active clip rectangle. clamp to [x0,x1) x [y0,y1); default clip = full
    // screen, so a normal full-frame present is unchanged.
    int32_t cx0 = g_clip_x0 < 0 ? 0 : g_clip_x0;
    int32_t cy0 = g_clip_y0 < 0 ? 0 : g_clip_y0;
    int32_t cx1 = g_clip_x1 > g_fb_width  ? g_fb_width  : g_clip_x1;
    int32_t cy1 = g_clip_y1 > g_fb_height ? g_fb_height : g_clip_y1;
    if (cx1 <= cx0 || cy1 <= cy0) return;

    if (g_wp_width == g_fb_width && g_wp_height == g_fb_height) {
        // Exact match: blit row by row. MUST honor g_fb_pitch (pixels per
        // framebuffer row), which can exceed g_fb_width when the GPU pads
        // scanlines for alignment. A single flat memcpy of width*height
        // pixels (ignoring pitch) shifts every row progressively, shearing
        // the image into a "several images merged, wrong colors" mess and
        // leaving a corruption seam. The wallpaper buffer is packed at
        // g_wp_width stride.
        for (int32_t y = cy0; y < cy1; y++) {
            memcpy(g_fb        + (uint32_t)y * (uint32_t)g_fb_pitch  + cx0,
                   g_wp_pixels + (uint32_t)y * (uint32_t)g_wp_width  + cx0,
                   (unsigned long)(cx1 - cx0) * sizeof(uint32_t));
        }
        return;
    }

    // Nearest-neighbor scale to fill the screen (clipped columns/rows only).
    int32_t sw = g_fb_width;
    int32_t sh = g_fb_height;
    for (int32_t dy = cy0; dy < cy1; dy++) {
        // Source row index, scaled.
        int32_t sy = (dy * g_wp_height) / sh;
        if (sy >= g_wp_height) sy = g_wp_height - 1;

        const uint32_t *src_row = g_wp_pixels + (uint32_t)sy * (uint32_t)g_wp_width;
        uint32_t       *dst_row = g_fb + (uint32_t)dy * (uint32_t)g_fb_pitch;

        for (int32_t dx = cx0; dx < cx1; dx++) {
            int32_t sx = (dx * g_wp_width) / sw;
            if (sx >= g_wp_width) sx = g_wp_width - 1;
            dst_row[dx] = src_row[sx];
        }
    }
}

// ============================================================================
// wallpaper_picker_open / wallpaper_picker_close
// ============================================================================

void wallpaper_picker_open(void)
{
    // #B2 app store: wallpaper_init() enumerates once at compositor boot, so a
    // wallpaper the App Store installs after boot (a fresh BMP dropped at "/")
    // was invisible here until the compositor restarted, even though the file
    // was already on disk and Settings (a short-lived process) would see it on
    // its next launch. Re-scan every time the picker opens: wp_enumerate() is
    // a cheap directory read (#517), so doing it on open (not every frame) is
    // free in practice and keeps this picker's index agreeing with Settings'.
    g_wallpaper_count       = wp_enumerate(g_wallpapers, WP_MAX_ENTRIES);
    g_wallpaper_picker_open = true;
    g_picker_scroll         = 0;
    g_picker_hover          = -1;
}

void wallpaper_picker_close(void)
{
    g_wallpaper_picker_open = false;
}

// #uiscale hit-test fix: the close button rect and the grid content rect
// were each written out twice - once in wallpaper_render_picker() (draw),
// once again in the mouse handler (hit-test) - as literally identical
// formulas over the same (already-scaled) macros. Two copies of an identical
// formula cannot drift TODAY, but they are exactly the shape that drifts the
// NEXT time either one is touched in isolation, which is the failure mode
// this whole audit is about. Shared functions now, one source each.
static void wallpaper_close_btn_rect(int32_t dlg_x, int32_t dlg_y,
                                     int32_t *cx, int32_t *cy, int32_t *cw, int32_t *ch) {
    *cw = PICKER_TITLE_H;
    *cx = dlg_x + PICKER_WIDTH - *cw;
    *cy = dlg_y;
    *ch = PICKER_TITLE_H;
}
static void wallpaper_grid_rect(int32_t dlg_x, int32_t dlg_y,
                                int32_t *gx, int32_t *gy, int32_t *gh) {
    *gx = dlg_x + THUMB_PADDING;
    *gy = dlg_y + PICKER_TITLE_H + THUMB_PADDING;
    // #wpanim: WPANIM_STRIP_H subtracted here is the exact amount
    // PICKER_HEIGHT grew by (compositor.h), so this grid is the same size
    // it was before the effect strip existed.
    *gh = PICKER_HEIGHT - PICKER_TITLE_H - THUMB_PADDING * 2 - ui_px(16) - WPANIM_STRIP_H;
}

// ============================================================================
// #wpanim: effect-mode buttons + intensity slider + repel/attract toggle.
// Shared geometry (draw side in wallpaper_render_picker(), hit-test side in
// wallpaper_picker_handle_mouse()) - one source, per the #uiscale rule noted
// above wallpaper_close_btn_rect().
// ============================================================================

static void wallpaper_strip_rect(int32_t dlg_x, int32_t dlg_y,
                                 int32_t *sx, int32_t *sy, int32_t *sw) {
    int32_t gx, gy, gh;
    wallpaper_grid_rect(dlg_x, dlg_y, &gx, &gy, &gh);
    *sx = dlg_x + THUMB_PADDING;
    *sy = gy + gh + ui_px(6);
    *sw = PICKER_WIDTH - THUMB_PADDING * 2;
}

#define WPANIM_BTN_H  ui_px(20)
#define WPANIM_ROW2_Y_OFF (WPANIM_BTN_H + ui_px(8))
#define WPANIM_ROW2_H ui_px(18)

// Mode button `idx` (0 = Off .. WPANIM_MODE_COUNT-1 = Gravity), row 1.
static void wallpaper_mode_btn_rect(int32_t sx, int32_t sy, int32_t sw, int idx,
                                    int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    int32_t gap = ui_px(3);
    int32_t bw5 = (sw - gap * (WPANIM_MODE_COUNT - 1)) / WPANIM_MODE_COUNT;
    *bx = sx + idx * (bw5 + gap);
    *by = sy;
    *bw = bw5;
    *bh = WPANIM_BTN_H;
}

// Row 2: [value text][intensity slider][repel/attract toggle].
static void wallpaper_row2_rects(int32_t sx, int32_t sy, int32_t sw,
                                 int32_t *sl_x, int32_t *sl_y, int32_t *sl_w, int32_t *sl_h,
                                 int32_t *tg_x, int32_t *tg_w) {
    int32_t y2 = sy + WPANIM_ROW2_Y_OFF;
    int32_t val_w = ui_px(30);
    *tg_w = ui_px(64);
    *tg_x = sx + sw - *tg_w;
    *sl_x = sx + val_w;
    *sl_y = y2;
    *sl_w = sw - val_w - *tg_w - ui_px(6);
    *sl_h = WPANIM_ROW2_H;
}

// #wpcolor: row 3, below row 2 - [value text][base-hue slider][Content/
// Spectrum/Mono button group]. Same shape as wallpaper_row2_rects() above
// (value + slider + right-side control cluster), one row lower.
#define WPANIM_ROW3_Y_OFF (WPANIM_ROW2_Y_OFF + WPANIM_ROW2_H + ui_px(6))
#define WPANIM_ROW3_H     ui_px(18)
#define WPANIM_PALETTE_GROUP_W ui_px(120)

static void wallpaper_row3_rects(int32_t sx, int32_t sy, int32_t sw,
                                 int32_t *sl_x, int32_t *sl_y, int32_t *sl_w, int32_t *sl_h,
                                 int32_t *pg_x, int32_t *pg_w) {
    int32_t y3 = sy + WPANIM_ROW3_Y_OFF;
    int32_t val_w = ui_px(30);
    *pg_w = WPANIM_PALETTE_GROUP_W;
    *pg_x = sx + sw - *pg_w;
    *sl_x = sx + val_w;
    *sl_y = y3;
    *sl_w = sw - val_w - *pg_w - ui_px(6);
    *sl_h = WPANIM_ROW3_H;
}

// One of the three palette buttons within the group rect from
// wallpaper_row3_rects() above.
static void wallpaper_palette_btn_rect(int32_t pg_x, int32_t pg_y, int32_t pg_w, int idx,
                                       int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    int32_t gap = ui_px(2);
    int32_t bw3 = (pg_w - gap * (WP_PALETTE_COUNT - 1)) / WP_PALETTE_COUNT;
    *bx = pg_x + idx * (bw3 + gap);
    *by = pg_y;
    *bw = bw3;
    *bh = WPANIM_ROW3_H;
}

// ============================================================================
// wallpaper_render_picker
// Draws the wallpaper-chooser dialog centered on the screen.
//
// Layout:
//   Title bar (PICKER_TITLE_H px) with "Choose Wallpaper" and a close button.
//   A scrollable grid of THUMB_COLS thumbnails per row, each THUMB_CELL_W x
//   THUMB_CELL_H.  The name is drawn inside each cell.  The currently active
//   wallpaper is highlighted with a CLR_PICKER_SEL border.
//   Scroll indicators are drawn at the bottom when content overflows.
// ============================================================================

void wallpaper_render_picker(void)
{
    if (!g_wallpaper_picker_open) return;

    int32_t dlg_x = (g_fb_width  - PICKER_WIDTH)  / 2;
    int32_t dlg_y = (g_fb_height - PICKER_HEIGHT) / 2;

    // Background panel.
    draw_fill_rect(dlg_x, dlg_y, PICKER_WIDTH, PICKER_HEIGHT, CLR_PICKER_BG);
    draw_rect_outline(dlg_x, dlg_y, PICKER_WIDTH, PICKER_HEIGHT, CLR_PICKER_BORDER);

    // Title bar.
    draw_fill_rect(dlg_x, dlg_y, PICKER_WIDTH, PICKER_TITLE_H, CLR_PICKER_TITLE);

    // Title text: centered vertically in the title bar.
    int32_t title_text_y = dlg_y + (PICKER_TITLE_H - FONT_CHAR_H) / 2;
    draw_text_centered(dlg_x + PICKER_WIDTH / 2, title_text_y,
                       "Choose Wallpaper", CLR_TEXT_WHITE);

    // Close button: 16 x (PICKER_TITLE_H) in the top-right corner.
    int32_t close_x, close_y, close_w, close_h;
    wallpaper_close_btn_rect(dlg_x, dlg_y, &close_x, &close_y, &close_w, &close_h);   // #uiscale: shared with the hit-test
    draw_fill_rect(close_x, close_y, close_w, PICKER_TITLE_H, 0xFF662222);
    draw_rect_outline(close_x, close_y, close_w, PICKER_TITLE_H, CLR_PICKER_BORDER);
    // "X" glyph, centered.
    int32_t x_gx = close_x + (close_w - FONT_CHAR_W) / 2;
    int32_t x_gy = close_y + (PICKER_TITLE_H - FONT_CHAR_H) / 2;
    draw_char(x_gx, x_gy, 'X', CLR_TEXT_WHITE);

    // Separator line below title bar.
    draw_hline(dlg_x, dlg_y + PICKER_TITLE_H, PICKER_WIDTH, CLR_PICKER_BORDER);

    // Grid content area.
    int32_t grid_x, grid_y, grid_h;
    wallpaper_grid_rect(dlg_x, dlg_y, &grid_x, &grid_y, &grid_h);   // #uiscale: shared with the hit-test
    int32_t rows_vis  = grid_h / THUMB_CELL_H;
    if (rows_vis < 1) rows_vis = 1;

    int32_t total_rows = ((int32_t)g_wallpaper_count + THUMB_COLS - 1) / THUMB_COLS;
    // Clamp scroll.
    int32_t max_scroll = total_rows - rows_vis;
    if (max_scroll < 0) max_scroll = 0;
    if (g_picker_scroll > max_scroll) g_picker_scroll = max_scroll;
    if (g_picker_scroll < 0)         g_picker_scroll = 0;

    // Draw each visible thumbnail cell.
    for (int row = g_picker_scroll; row < g_picker_scroll + rows_vis; row++) {
        int32_t vis_row = row - g_picker_scroll;  // 0-based visible row index
        for (int col = 0; col < THUMB_COLS; col++) {
            int idx = row * THUMB_COLS + col;
            if (idx >= g_wallpaper_count) break;

            int32_t cx = grid_x + col * THUMB_CELL_W;
            int32_t cy = grid_y + vis_row * THUMB_CELL_H;

            // Hover highlight background.
            uint32_t cell_bg = CLR_PICKER_THUMB;
            if (idx == g_picker_hover) {
                cell_bg = 0xFF484848;
            }

            // Thumbnail background.
            draw_fill_rect(cx, cy, THUMB_WIDTH, THUMB_HEIGHT, cell_bg);

            // Draw a small gradient preview inside the thumbnail area.
            if (WP_IS_GRADIENT(idx)) {
                // Gradient entry: show the actual gradient colors.
                draw_gradient_v(cx, cy, THUMB_WIDTH, THUMB_HEIGHT,
                                CLR_WP_GRAD_TOP, CLR_WP_GRAD_BOT);
            } else {
                // BMP entry: draw a stylized placeholder using a dim rectangle
                // with a small "image" icon hint.
                draw_gradient_v(cx, cy, THUMB_WIDTH, THUMB_HEIGHT,
                                0xFF404858, 0xFF283040);
                // Small picture-frame indicator in the center of the thumbnail.
                int32_t frame_w = 20, frame_h = 14;
                int32_t frame_x = cx + (THUMB_WIDTH  - frame_w) / 2;
                int32_t frame_y = cy + (THUMB_HEIGHT - frame_h) / 2;
                draw_fill_rect(frame_x, frame_y, frame_w, frame_h, 0xFF505868);
                draw_rect_outline(frame_x, frame_y, frame_w, frame_h, 0xFF8090A0);
            }

            // Selection border for the currently loaded wallpaper.
            if (idx == g_current_wallpaper) {
                draw_rect_outline(cx - 1, cy - 1,
                                  THUMB_WIDTH + 2, THUMB_HEIGHT + 2,
                                  CLR_PICKER_SEL);
                draw_rect_outline(cx - 2, cy - 2,
                                  THUMB_WIDTH + 4, THUMB_HEIGHT + 4,
                                  CLR_PICKER_SEL);
            } else {
                draw_rect_outline(cx, cy, THUMB_WIDTH, THUMB_HEIGHT,
                                  CLR_PICKER_BORDER);
            }

            // Hover border (drawn on top of selection so it is always visible).
            if (idx == g_picker_hover && idx != g_current_wallpaper) {
                draw_rect_outline(cx, cy, THUMB_WIDTH, THUMB_HEIGHT, 0xFF8090C0);
            }

            // Name label below thumbnail, truncated to fit the cell.
            const char *name = g_wallpapers[idx].name;
            int32_t    label_y = cy + THUMB_HEIGHT + 2;
            // Truncate to THUMB_CELL_W characters.
            char  label_buf[16];
            int   max_chars = (THUMB_CELL_W - 2) / FONT_CHAR_W;
            if (max_chars > 15) max_chars = 15;
            int ni = 0;
            while (name[ni] && ni < max_chars) {
                label_buf[ni] = name[ni];
                ni++;
            }
            label_buf[ni] = '\0';
            draw_text(cx, label_y, label_buf, CLR_PICKER_LABEL);
        }
    }

    // #wpanim: effect-mode row + intensity slider + repel/attract toggle.
    // Follows the Settings/Files design language (docs/UI_STYLE_GUIDE.md):
    // same fill/outline/readable_ink idiom as every other control on this
    // dialog, no bespoke look.
    {
        int32_t sx, sy, sw;
        wallpaper_strip_rect(dlg_x, dlg_y, &sx, &sy, &sw);
        draw_hline(dlg_x, sy - ui_px(4), PICKER_WIDTH, CLR_PICKER_BORDER);

        for (int m = 0; m < WPANIM_MODE_COUNT; m++) {
            int32_t bx, by, bw, bh;
            wallpaper_mode_btn_rect(sx, sy, sw, m, &bx, &by, &bw, &bh);
            uint32_t bg = (m == g_wp_anim_mode) ? CLR_PICKER_SEL : CLR_PICKER_THUMB;
            draw_fill_rect(bx, by, bw, bh, bg);
            draw_rect_outline(bx, by, bw, bh, CLR_PICKER_BORDER);
            draw_text_centered(bx + bw / 2, by + (bh - FONT_CHAR_H) / 2,
                               wpanim_mode_name(m), readable_ink(bg));
        }

        int32_t slx, sly, slw, slh, tgx, tgw;
        wallpaper_row2_rects(sx, sy, sw, &slx, &sly, &slw, &slh, &tgx, &tgw);

        // Value text ("0".."100") left of the slider.
        char valbuf[8]; int vb = 0;
        int val = g_wp_anim_intensity;
        if (val >= 100)      { valbuf[vb++] = '1'; valbuf[vb++] = '0'; valbuf[vb++] = '0'; }
        else if (val >= 10)  { valbuf[vb++] = (char)('0' + val / 10); valbuf[vb++] = (char)('0' + val % 10); }
        else                 { valbuf[vb++] = (char)('0' + val); }
        valbuf[vb] = '\0';
        draw_text(sx, sly + (slh - FONT_CHAR_H) / 2, valbuf, CLR_PICKER_LABEL);

        // Slider track + fill (disabled/greyed look when no effect is active,
        // same visual language a disabled control uses elsewhere: still
        // drawn, just not implying it does anything).
        draw_fill_rect(slx, sly, slw, slh, CLR_PICKER_THUMB);
        draw_rect_outline(slx, sly, slw, slh, CLR_PICKER_BORDER);
        int32_t fillw = (slw * g_wp_anim_intensity) / 100;
        if (fillw > 0) draw_fill_rect(slx, sly, fillw, slh, CLR_PICKER_SEL);

        // Repel/attract toggle.
        draw_fill_rect(tgx, sly, tgw, slh, CLR_PICKER_THUMB);
        draw_rect_outline(tgx, sly, tgw, slh, CLR_PICKER_BORDER);
        draw_text_centered(tgx + tgw / 2, sly + (slh - FONT_CHAR_H) / 2,
                           g_wp_anim_repel ? "Repel" : "Attract", CLR_PICKER_LABEL);

        // #wpcolor row 3: base-hue value + slider (fill shows the actual hue
        // via wpanim_hue_preview_color()) + Content/Spectrum/Mono buttons.
        int32_t hslx, hsly, hslw, hslh, pgx, pgw;
        wallpaper_row3_rects(sx, sy, sw, &hslx, &hsly, &hslw, &hslh, &pgx, &pgw);

        char huebuf[8]; int hb = 0;
        int hue = get_wallpaper_anim_hue();
        if (hue >= 100)     { huebuf[hb++] = (char)('0' + hue / 100); hue %= 100;
                              huebuf[hb++] = (char)('0' + hue / 10);  huebuf[hb++] = (char)('0' + hue % 10); }
        else if (hue >= 10) { huebuf[hb++] = (char)('0' + hue / 10);  huebuf[hb++] = (char)('0' + hue % 10); }
        else                { huebuf[hb++] = (char)('0' + hue); }
        huebuf[hb] = '\0';
        draw_text(sx, hsly + (hslh - FONT_CHAR_H) / 2, huebuf, CLR_PICKER_LABEL);

        draw_rect_outline(hslx, hsly, hslw, hslh, CLR_PICKER_BORDER);
        // Fill the whole track with the hue's own colour (a live swatch),
        // dimmer than full saturation so the thumb marker below reads
        // clearly against it.
        draw_fill_rect(hslx + 1, hsly + 1, hslw - 2, hslh - 2,
                       wpanim_hue_preview_color((float)get_wallpaper_anim_hue()));
        // Thumb marker at the current hue's position along the track.
        int32_t thumb_x = hslx + (hslw * get_wallpaper_anim_hue()) / 360;
        if (thumb_x > hslx + hslw - ui_px(3)) thumb_x = hslx + hslw - ui_px(3);
        draw_fill_rect(thumb_x, hsly, ui_px(3), hslh, CLR_TEXT_WHITE);

        for (int pidx = 0; pidx < WP_PALETTE_COUNT; pidx++) {
            int32_t bx, by, bw, bh;
            wallpaper_palette_btn_rect(pgx, hsly, pgw, pidx, &bx, &by, &bw, &bh);
            uint32_t bg = (pidx == g_wp_anim_palette) ? CLR_PICKER_SEL : CLR_PICKER_THUMB;
            draw_fill_rect(bx, by, bw, bh, bg);
            draw_rect_outline(bx, by, bw, bh, CLR_PICKER_BORDER);
            // Short label: the button is only ~ui_px(38) wide, too narrow
            // for wpanim_palette_name()'s full "Spectrum"/"Content" text.
            const char *lbl = (pidx == WP_PALETTE_CONTENT) ? "Cont" :
                               (pidx == WP_PALETTE_MONO)    ? "Mono" : "Spec";
            draw_text_centered(bx + bw / 2, by + (bh - FONT_CHAR_H) / 2, lbl, readable_ink(bg));
        }
    }

    // Scroll indicator at the bottom of the dialog (only if content overflows).
    if (total_rows > rows_vis) {
        int32_t ind_y  = dlg_y + PICKER_HEIGHT - 14;
        int32_t ind_x  = dlg_x + PICKER_WIDTH / 2;
        // Up arrow indicator.
        if (g_picker_scroll > 0) {
            draw_text(ind_x - 20, ind_y, "^", CLR_PICKER_LABEL);
        }
        // Down arrow indicator.
        if (g_picker_scroll < max_scroll) {
            draw_text(ind_x + 12, ind_y, "v", CLR_PICKER_LABEL);
        }
        // Page position text.
        char pos_buf[16];
        int p = g_picker_scroll + 1;
        int t = total_rows;
        // Build "p/t" string without sprintf.
        int bi = 0;
        if (p >= 10) pos_buf[bi++] = (char)('0' + p / 10);
        pos_buf[bi++] = (char)('0' + p % 10);
        pos_buf[bi++] = '/';
        if (t >= 10) pos_buf[bi++] = (char)('0' + t / 10);
        pos_buf[bi++] = (char)('0' + t % 10);
        pos_buf[bi] = '\0';
        draw_text(ind_x - (bi * FONT_CHAR_W) / 2, ind_y, pos_buf, CLR_PICKER_LABEL);
    }
}

// ============================================================================
// wallpaper_picker_handle_mouse
// Returns true if the event was consumed (picker is open and point is inside).
// ============================================================================

bool wallpaper_picker_handle_mouse(int32_t x, int32_t y, bool clicked)
{
    if (!g_wallpaper_picker_open) return false;

    int32_t dlg_x = (g_fb_width  - PICKER_WIDTH)  / 2;
    int32_t dlg_y = (g_fb_height - PICKER_HEIGHT) / 2;

    // If the cursor is outside the dialog, do not consume the event.
    if (x < dlg_x || x >= dlg_x + PICKER_WIDTH  ||
        y < dlg_y || y >= dlg_y + PICKER_HEIGHT) {
        return false;
    }

    // Close button hit test.
    int32_t close_x, close_y, close_w, close_h;
    wallpaper_close_btn_rect(dlg_x, dlg_y, &close_x, &close_y, &close_w, &close_h);   // #uiscale: shared with the draw side
    if (x >= close_x && x < close_x + close_w &&
        y >= close_y && y < close_y + PICKER_TITLE_H) {
        if (clicked) {
            wallpaper_picker_close();
            g_needs_redraw = true;
        }
        return true;
    }

    // Grid content area.
    int32_t grid_x, grid_y, grid_h;
    wallpaper_grid_rect(dlg_x, dlg_y, &grid_x, &grid_y, &grid_h);   // #uiscale: shared with the draw side
    int32_t rows_vis = grid_h / THUMB_CELL_H;
    if (rows_vis < 1) rows_vis = 1;

    // #wpanim: effect-mode row + intensity slider + repel/attract toggle.
    // Sits below the grid, above the scroll-indicator zone - see
    // wallpaper_strip_rect()'s comment for why the geometry lines up exactly.
    {
        int32_t sx, sy, sw;
        wallpaper_strip_rect(dlg_x, dlg_y, &sx, &sy, &sw);
        if (y >= sy - ui_px(4) && y < sy + WPANIM_STRIP_H) {
            if (y < sy + WPANIM_BTN_H) {
                for (int m = 0; m < WPANIM_MODE_COUNT; m++) {
                    int32_t bx, by, bw, bh;
                    wallpaper_mode_btn_rect(sx, sy, sw, m, &bx, &by, &bw, &bh);
                    if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
                        if (clicked) {
                            set_wallpaper_anim(m);
                            profile_save();   // #wallpaperpersist idiom: persist immediately, same as the thumbnail pick below
                            g_needs_redraw = true;
                        }
                        return true;
                    }
                }
            } else {
                int32_t slx, sly, slw, slh, tgx, tgw;
                wallpaper_row2_rects(sx, sy, sw, &slx, &sly, &slw, &slh, &tgx, &tgw);
                if (y >= sly && y < sly + slh) {
                    if (x >= slx && x < slx + slw) {
                        if (clicked) {
                            int32_t v = ((x - slx) * 100) / (slw > 0 ? slw : 1);
                            set_wallpaper_anim_intensity(v);
                            profile_save();
                            g_needs_redraw = true;
                        }
                        return true;
                    }
                    if (x >= tgx && x < tgx + tgw) {
                        if (clicked) {
                            set_wallpaper_anim_repel(!get_wallpaper_anim_repel());
                            profile_save();
                            g_needs_redraw = true;
                        }
                        return true;
                    }
                }
                // #wpcolor row 3: base-hue slider + Content/Spectrum/Mono.
                int32_t hslx, hsly, hslw, hslh, pgx, pgw;
                wallpaper_row3_rects(sx, sy, sw, &hslx, &hsly, &hslw, &hslh, &pgx, &pgw);
                if (y >= hsly && y < hsly + hslh) {
                    if (x >= hslx && x < hslx + hslw) {
                        if (clicked) {
                            int32_t v = ((x - hslx) * 360) / (hslw > 0 ? hslw : 1);
                            set_wallpaper_anim_hue(v);
                            profile_save();
                            g_needs_redraw = true;
                        }
                        return true;
                    }
                    for (int pidx = 0; pidx < WP_PALETTE_COUNT; pidx++) {
                        int32_t bx, by, bw, bh;
                        wallpaper_palette_btn_rect(pgx, hsly, pgw, pidx, &bx, &by, &bw, &bh);
                        if (x >= bx && x < bx + bw) {
                            if (clicked) {
                                set_wallpaper_anim_palette(pidx);
                                profile_save();
                                g_needs_redraw = true;
                            }
                            return true;
                        }
                    }
                }
            }
            return true;   // inside the strip but not on a live control: still consume it
        }
    }

    // Scroll indicator zone at the bottom.
    int32_t ind_y = dlg_y + PICKER_HEIGHT - 14;
    if (y >= ind_y) {
        // Click on the scroll indicator row.
        if (clicked) {
            int32_t total_rows = ((int32_t)g_wallpaper_count + THUMB_COLS - 1) / THUMB_COLS;
            int32_t max_scroll = total_rows - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            int32_t mid = dlg_x + PICKER_WIDTH / 2;
            if (x < mid) {
                // Left side: scroll up.
                if (g_picker_scroll > 0) {
                    g_picker_scroll--;
                    g_needs_redraw = true;
                }
            } else {
                // Right side: scroll down.
                if (g_picker_scroll < max_scroll) {
                    g_picker_scroll++;
                    g_needs_redraw = true;
                }
            }
        }
        return true;
    }

    // Check if cursor is inside the grid.
    if (x < grid_x || y < grid_y) return true;

    int32_t rel_x = x - grid_x;
    int32_t rel_y = y - grid_y;

    int col = (int)(rel_x / THUMB_CELL_W);
    int row = (int)(rel_y / THUMB_CELL_H) + g_picker_scroll;

    if (col < 0 || col >= THUMB_COLS) return true;

    // Verify the cursor is within the actual thumbnail area (not the padding).
    int32_t cell_local_x = rel_x - col * THUMB_CELL_W;
    int32_t cell_local_y = rel_y - ((int)(rel_y / THUMB_CELL_H)) * THUMB_CELL_H;
    if (cell_local_x > THUMB_WIDTH || cell_local_y > THUMB_CELL_H) return true;

    int idx = row * THUMB_COLS + col;
    if (idx < 0 || idx >= g_wallpaper_count) return true;

    // Update hover state.
    if (g_picker_hover != idx) {
        g_picker_hover = idx;
        g_needs_redraw = true;
    }

    if (clicked) {
        wallpaper_load(idx);
        set_wallpaper(idx);   // sync shared index so Settings reflects the choice
        profile_save();       // #wallpaperpersist: write the choice to UIPROFIL.YML so
                              // it survives a reboot (was runtime-only, never persisted)
        wallpaper_picker_close();
        g_needs_redraw = true;
    }

    return true;
}
