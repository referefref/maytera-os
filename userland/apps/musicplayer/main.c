// musicplayer - MayteraOS retro hi-fi music player (#322, phase 1)
//
// A DELIBERATE EXCEPTION to the OS Motif/material style engine: this is a
// classic WinAmp / XMMS / Audacious style skinned hi-fi "rack". It composes a
// fully bitmap/pixel chrome offscreen framebuffer and blits it with
// SYS_WIN_BLIT (the Solitaire/DOOM pattern), then overlays pixel text on top.
//
// Layout = one borderless (nochrome) window holding three stacked modules:
//   MAIN     - 7-seg LCD time, scrolling track title, animated spectrum,
//              transport buttons, seek + volume + balance sliders, kHz/kbps/
//              stereo indicators, shuffle/repeat toggles.
//   EQ       - preamp + 10 graphic-EQ band sliders + ON / AUTO / preset
//              (DSP is a phase-1 no-op; the slider state is the deliverable).
//   PLAYLIST - scans a folder for audio files; select + double-click plays.
//
// Playback: the real decode/output path is the kernel SYS_PLAY_WAV (192) ->
// audio_play_file() -> audio_decode_open() (libmad for MP3) -> audio_write().
// That call is synchronous in the kernel, so to keep this UI live we re-spawn
// THIS binary in a tiny "--play <path>" helper mode (sys_spawn_args), which
// performs the blocking decode in its own process while the UI keeps animating.
//
// Skins: a small palette-driven skin engine with several built-in retro skins
// (cycle with the [S] box in the title bar). WinAmp .wsz (ZIP of bitmaps) skin
// import via the archiver lib is phase 2.

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"
#include "arc.h"            // archiver: ZIP extraction for WinAmp .wsz skins
#include "viz.h"            // MilkDrop-style TinyGL audio visualizer window

// open() flag bits (libc fcntl, not pulled in by maytera.h)
#ifndef O_WRONLY
#define O_WRONLY  0x0001
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#endif
#define O_WRCREAT (O_WRONLY | O_CREAT | O_TRUNC)   // 0x241

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

// #335: real DAC playback position (ms). Returns -1 when no USB DAC is ready.
#ifndef SYS_AUDIO_POS_MS
#define SYS_AUDIO_POS_MS 295
#endif
static inline long mp_audio_pos_ms(void) { return syscall0(SYS_AUDIO_POS_MS); }

// ----------------------------------------------------------------------------
// Geometry
// ----------------------------------------------------------------------------
#define W            400
#define MOD_MAIN_Y   0
#define MOD_MAIN_H   150
#define MOD_EQ_Y     150
#define MOD_EQ_H     128
#define MOD_PL_Y     278
#define MOD_PL_H     200
#define H            (MOD_MAIN_H + MOD_EQ_H + MOD_PL_H)   // 478

#define NUM_BARS     19          // spectrum bars
#define NUM_EQ       10          // EQ bands
#define MAX_TRACKS   128
#define NAME_LEN     64

// ----------------------------------------------------------------------------
// Skin (palette) engine
// ----------------------------------------------------------------------------
typedef struct {
    const char *name;
    uint32_t bg;        // panel base
    uint32_t bg2;       // panel gradient bottom
    uint32_t light;     // bevel highlight
    uint32_t dark;      // bevel shadow
    uint32_t title_a;   // titlebar gradient top
    uint32_t title_b;   // titlebar gradient bottom
    uint32_t title_ink; // titlebar text
    uint32_t lcd_bg;    // LCD window background
    uint32_t lcd_fg;    // LCD lit segment / text
    uint32_t lcd_off;   // LCD unlit segment
    uint32_t accent;    // sliders / highlights
    uint32_t btn_face;  // transport button face
    uint32_t ink;       // generic label text
    uint32_t ink_dim;   // dim label text
} skin_t;

static const skin_t g_skins[] = {
    // 0: Classic WinAmp - charcoal metal, green phosphor LCD
    { "CLASSIC",
      0x2A2E33, 0x16181C, 0x4A5057, 0x0C0E10,
      0x39516E, 0x1C2A3C, 0xC8D6E6,
      0x06140A, 0x39FF6A, 0x0E3A1C, 0x39FF6A,
      0x3B4148, 0xB6C2CF, 0x6E7A86 },
    // 1: Amp Blue - deep blue rack, cyan LCD
    { "AMP BLUE",
      0x1B2A40, 0x0C1422, 0x3A5170, 0x060A12,
      0x2C6CA8, 0x123250, 0xDCEAFF,
      0x041018, 0x4FE8FF, 0x0C2C36, 0x4FE8FF,
      0x274058, 0xCFE2F5, 0x6F88A0 },
    // 2: Phosphor - black panel, amber CRT
    { "PHOSPHOR",
      0x202020, 0x0A0A0A, 0x484848, 0x000000,
      0x4A3A18, 0x241A08, 0xFFE0A0,
      0x140C02, 0xFFB000, 0x3A2600, 0xFFB000,
      0x303030, 0xD8D8D8, 0x808080 },
};
#define NUM_SKINS (int)(sizeof(g_skins)/sizeof(g_skins[0]))
static int g_skin = 0;
#define SK (g_skins[g_skin])

// #165 per-app transparency: title-strip [T] button cycles the window opacity
// 100% -> 85% -> 70% -> 100%. (definition + cycle_opacity() are further down)
static const int g_opa_levels[3] = { 255, 217, 178 };   // 100% / 85% / 70%
static int g_opa_idx = 0;

// ----------------------------------------------------------------------------
// Player state
// ----------------------------------------------------------------------------
enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };
static int   win = -1;
static int   g_state = ST_STOPPED;
static int   g_vol = 80;          // 0..100
static int   g_balance = 0;       // -50..50
static int   g_seek = 0;          // 0..100 (visual)
static int   g_shuffle = 0, g_repeat = 0;
static int   g_eq_on = 1;
static int   g_eq_preamp = 32;    // 0..63
static int   g_eq[NUM_EQ];        // 0..63 each
static int   g_frame = 0;
static int   g_title_scroll = 0;
// #334 WinAmp-style window dragging: grab the rack by an empty title strip and
// drag it. We track the grab offset (cursor - window origin) and call win_move()
// each frame while mouse button 1 is held.
static int   g_grab_dx  = 0;
static int   g_grab_dy  = 0;
// #334 verification self-test (gated by /CONFIG/MPDRAGTEST.CFG): glide the
// window with win_move() - the SAME syscall the mouse drag uses - so
// SYS_WIN_MOVE can be proven deterministically without a real pointer.
static int   g_dragtest = 0;

// #342 multi-window WinAmp rig: the EQ, PLAYLIST and ALBUM ART sections each
// live in their OWN borderless (nochrome) window so they can be shown/hidden and
// dragged independently, just like classic WinAmp. The MilkDrop visualizer
// (viz.c) is already a separate window. The EQ and PL windows simply BLIT the
// matching sub-region of the shared offscreen framebuffer g_fb (compose() /
// compose_skin() still render the full stacked rack into g_fb unchanged); the
// ART window renders the decoded cover into its own small buffer.
static int   eq_win  = -1;   // EQ window handle (>=0 = open)
static int   pl_win  = -1;   // playlist window handle
static int   art_win = -1;   // album-art window handle
static int   lib_win = -1;   // music-library browser window handle (g_view==1)
// which window is currently being dragged (>=0), plus grab offset. Replaces the
// single-window g_dragging flag so every window is independently draggable (#334).
static int   g_drag_win = -1;

// album-art window geometry (its own framebuffer, scaled from g_art)
#define ART_WIN_W   232
#define ART_WIN_TH  15          // drawn title strip height
#define ART_WIN_H   (ART_WIN_W + ART_WIN_TH)   // square art + title strip
static uint32_t g_artfb[ART_WIN_W * ART_WIN_H];

// panel show/hide toggle chips ([EQ][PL][VIS][ART]) drawn in the main title strip
#define TGL_Y   2
#define TGL_H   11
#define TGL_W   22
#define TGL_X0  3
#define TGL_PITCH (TGL_W + 2)

// pseudo spectrum
static int   g_bar[NUM_BARS];
static int   g_peak[NUM_BARS];

// elapsed time (cosmetic, RTC-driven)
static long  g_play_start_sec = 0;
static long  g_elapsed = 0;
static int   g_show_remaining = 0;
static long  g_track_len = 213;   // assumed length for the remaining display

// #335 continuous-loop playback state
static int   g_play_pid = -1;     // pid of the spawned "--play" decode helper
static int   g_advancing = 0;     // set while auto-advancing (do not cut the track)
static long  g_last_pos_ms = -1;  // last SYS_AUDIO_POS_MS reading (stall detect)
static int   g_pos_stall = 0;     // consecutive polls with a frozen DAC position

// playlist
// track metadata (phase 2b): parsed from the audio file's own tags. All fields
// are optional; has_meta marks whether any tag parse succeeded.
typedef struct {
    char name[NAME_LEN];   // filename (8.3 / basename)
    char path[160];        // absolute path
    char title[64];
    char artist[48];
    char album[48];
    char genre[24];
    int  year;
    int  track_no;
    int  duration;         // seconds, 0 = unknown
    int  has_meta;
} track_t;
static track_t g_tracks[MAX_TRACKS];
static int   g_ntracks = 0;
static int   g_sel = -1;
static int   g_cur = -1;          // currently playing index
static int   g_pl_scroll = 0;
static char  g_dir[128] = "/HOME";
static char  g_now[128] = "";   // "Artist - Title" marquee text

// offscreen framebuffer
static uint32_t g_fb[W * H];

// ---- phase 2b: metadata, album art, library (forward decls) ----------------
static void parse_metadata(const char *path, track_t *t);
static void track_label(const track_t *t, char *out, int outsz);
static void load_art_for(const char *path);
static void mp_draw_art(int bx, int by, int box);
static void mp_cpy(char *d, const char *s, int cap);
static void mp_zero(void *p, int n);
static void lib_open(void);    // #342 music-library browser window
static void lib_close(void);

// album art thumbnail (decoded via the kernel SYS_DECODE_IMAGE path)
// #342: decode covers at HIGH resolution (up to 512px) so the floating album-art
// window (~232px) and thumbnails downscale crisply instead of upscaling a tiny
// source. A 512x512 ARGB buffer is 1 MB (static). Embedded APIC covers are often
// 500x500+, and the online CoverArtArchive fetch requests front-500.
#define ART_SZ 512
static uint32_t g_art[ART_SZ * ART_SZ];
static int      g_art_have = 0;        // 1 = g_art holds a decoded thumbnail
static int      g_art_w = 0, g_art_h = 0;
static char     g_art_path[160] = "";  // path the current art was loaded from
// #342: cover loading is DEFERRED to the main loop (reading a multi-MB track for
// its embedded cover can stall; doing it synchronously in play_*() blanked the UI
// at startup). play_*() records the pending track here; the loop loads it once so
// the window + audio come up immediately, then the cover pops in.
static char     g_pending_art[256]    = "";
static char     g_pending_artist[48]  = "";
static char     g_pending_album[48]   = "";
static int      g_art_pending = 0;
static int      g_art_retry   = 0;     // frame-spread retries for a missed cover
static int      g_art_retry_at = 0;    // g_frame at which the next retry may run

// view mode: 0 = rack (3 modules), 1 = library browser
static int      g_view = 0;

// ---- phase 2c: online metadata + cover art (MusicBrainz + Cover Art Archive)
// When a track has artist+album tags but no embedded cover, look the release up
// on MusicBrainz (JSON), then pull the front cover from the Cover Art Archive.
// Both fetches run through the kernel's async HTTPS client (http_fetch_start/
// poll/read) so the rack keeps animating, and results are cached to disk under
// /CONFIG/MPART/ keyed by a hash of artist+album so we never re-fetch.
static void online_art_kick(const char *artist, const char *album);
static void online_art_poll(void);

// 32-entry sine-ish wave table (0..63) for the spectrum animation
static const unsigned char g_wave[32] = {
    31,37,43,49,54,58,61,63,63,61,58,54,49,43,37,31,
    25,19,13, 9, 5, 2, 0, 0, 2, 5, 9,13,19,25,29,30
};

// ----------------------------------------------------------------------------
// Framebuffer primitives (opaque 0xAARRGGBB)
// ----------------------------------------------------------------------------
static inline uint32_t op(uint32_t rgb) { return 0xFF000000u | (rgb & 0xFFFFFFu); }

static inline void px(int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) g_fb[y * W + x] = c;
}

static void fb_rect(int x, int y, int w, int h, uint32_t rgb) {
    uint32_t c = op(rgb);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        uint32_t *row = &g_fb[(y + j) * W + x];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void fb_hline(int x, int y, int w, uint32_t rgb) { fb_rect(x, y, w, 1, rgb); }
static void fb_vline(int x, int y, int h, uint32_t rgb) { fb_rect(x, y, 1, h, rgb); }

// vertical gradient fill
static void fb_grad_v(int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bot >> 16) & 0xFF, bg = (bot >> 8) & 0xFF, bb = bot & 0xFF;
    for (int j = 0; j < h; j++) {
        int r = tr + (br - tr) * j / (h > 1 ? h - 1 : 1);
        int g = tg + (bg - tg) * j / (h > 1 ? h - 1 : 1);
        int b = tb + (bb - tb) * j / (h > 1 ? h - 1 : 1);
        fb_hline(x, y + j, w, (r << 16) | (g << 8) | b);
    }
}

// raised (out) or sunken (in) 1px bevel
static void fb_bevel(int x, int y, int w, int h, int raised) {
    uint32_t lt = raised ? SK.light : SK.dark;
    uint32_t dk = raised ? SK.dark : SK.light;
    fb_hline(x, y, w, lt);
    fb_vline(x, y, h, lt);
    fb_hline(x, y + h - 1, w, dk);
    fb_vline(x + w - 1, y, h, dk);
}

// #342 panel toggle chips. tgl_on(i): is panel i currently shown?
//   0 = EQ, 1 = PL(aylist), 2 = VIS(ualizer), 3 = ART(work)
static const char *g_tgl_lbl[4] = { "EQ", "PL", "VIS", "ART" };
static int tgl_on(int i) {
    return i == 0 ? (eq_win >= 0) : i == 1 ? (pl_win >= 0)
         : i == 2 ? viz_is_open() : (art_win >= 0);
}
// draw the four toggle chips into g_fb (main region), so the main-window blit
// carries them. Text labels are overlaid separately (overlay_panel_toggles).
static void draw_panel_toggles_fb(void) {
    for (int i = 0; i < 4; i++) {
        int x = TGL_X0 + i * TGL_PITCH, on = tgl_on(i);
        fb_rect(x, TGL_Y, TGL_W, TGL_H, on ? 0x2A6CB0 : 0x2A2E33);
        fb_bevel(x, TGL_Y, TGL_W, TGL_H, on ? 0 : 1);
    }
}
static void overlay_panel_toggles(void) {
    for (int i = 0; i < 4; i++) {
        int x = TGL_X0 + i * TGL_PITCH;
        win_draw_text_small(win, x + 2, TGL_Y + 2, g_tgl_lbl[i],
                            tgl_on(i) ? 0xDFF0FF : 0x8A96A2);
    }
}

// ----------------------------------------------------------------------------
// 7-segment LCD digit
// ----------------------------------------------------------------------------
// segment order: a b c d e f g
static const unsigned char g_seg[11][7] = {
    {1,1,1,1,1,1,0}, // 0
    {0,1,1,0,0,0,0}, // 1
    {1,1,0,1,1,0,1}, // 2
    {1,1,1,1,0,0,1}, // 3
    {0,1,1,0,0,1,1}, // 4
    {1,0,1,1,0,1,1}, // 5
    {1,0,1,1,1,1,1}, // 6
    {1,1,1,0,0,0,0}, // 7
    {1,1,1,1,1,1,1}, // 8
    {1,1,1,1,0,1,1}, // 9
    {0,0,0,0,0,0,0}, // 10 = blank
};

static void draw_digit(int ox, int oy, int dw, int dh, int t, int d) {
    if (d < 0 || d > 10) d = 10;
    const unsigned char *s = g_seg[d];
    int vh = (dh - 3 * t) / 2;
    uint32_t on = SK.lcd_fg, off = SK.lcd_off;
    // a top
    fb_rect(ox + t, oy, dw - 2 * t, t, s[0] ? on : off);
    // g middle
    fb_rect(ox + t, oy + t + vh, dw - 2 * t, t, s[6] ? on : off);
    // d bottom
    fb_rect(ox + t, oy + dh - t, dw - 2 * t, t, s[3] ? on : off);
    // f top-left
    fb_rect(ox, oy + t, t, vh, s[5] ? on : off);
    // b top-right
    fb_rect(ox + dw - t, oy + t, t, vh, s[1] ? on : off);
    // e bottom-left
    fb_rect(ox, oy + 2 * t + vh, t, vh, s[4] ? on : off);
    // c bottom-right
    fb_rect(ox + dw - t, oy + 2 * t + vh, t, vh, s[2] ? on : off);
}

static void draw_time(int ox, int oy, long mm, long ss) {
    if (mm > 99) mm = 99;
    int dw = 16, dh = 30, t = 3, gap = 4;
    int x = ox;
    draw_digit(x, oy, dw, dh, t, (int)(mm / 10)); x += dw + gap;
    draw_digit(x, oy, dw, dh, t, (int)(mm % 10)); x += dw + gap;
    // colon
    fb_rect(x + 1, oy + dh / 3 - 2, 4, 4, SK.lcd_fg);
    fb_rect(x + 1, oy + 2 * dh / 3 - 2, 4, 4, SK.lcd_fg);
    x += 10;
    draw_digit(x, oy, dw, dh, t, (int)(ss / 10)); x += dw + gap;
    draw_digit(x, oy, dw, dh, t, (int)(ss % 10));
}

// ----------------------------------------------------------------------------
// Time helper (RTC seconds-of-day)
// ----------------------------------------------------------------------------
static long now_sec(void) {
    int h, m, s;
    get_rtc_time(&h, &m, &s);
    return (long)h * 3600 + (long)m * 60 + s;
}

// ----------------------------------------------------------------------------
// Spectrum animation
// ----------------------------------------------------------------------------
static void update_spectrum(void) {
    int active = (g_state == ST_PLAYING);
    for (int i = 0; i < NUM_BARS; i++) {
        int a = g_wave[(g_frame + i * 3) & 31];
        int b = g_wave[(g_frame * 2 + i * 7) & 31];
        int v = (a + b) / 2;                 // 0..63
        if (!active) v = (v / 4) + 1;        // idle: low shimmer
        if (v > 63) v = 63;
        g_bar[i] = v;
        if (v >= g_peak[i]) g_peak[i] = v;
        else if (g_peak[i] > 0) g_peak[i] -= 2;
    }
}

// ----------------------------------------------------------------------------
// Hit regions (window-local coords)
// ----------------------------------------------------------------------------
// transport buttons
#define TB_Y   (MOD_MAIN_Y + 104)
#define TB_H   20
#define TB_W   30
#define TB_X0  10
enum { HB_PREV = 0, HB_PLAY, HB_PAUSE, HB_STOP, HB_NEXT, HB_EJECT, HB_N };
static int tb_x(int i) { return TB_X0 + i * (TB_W + 2); }

// sliders
#define SEEK_X 10
#define SEEK_Y (MOD_MAIN_Y + 130)
#define SEEK_W (W - 20)
#define VOL_X  10
#define VOL_Y  (MOD_MAIN_Y + 141)
#define VOL_W  150
#define BAL_X  (W - 160)
#define BAL_W  150

// EQ
#define EQ_BANDS_X 44
#define EQ_BANDS_DX 32
#define EQ_SL_Y   (MOD_EQ_Y + 40)
#define EQ_SL_H   68
#define EQ_PRE_X  10

// playlist
#define PL_LIST_X 4
#define PL_LIST_Y (MOD_PL_Y + 20)
#define PL_ROW_H  12
#define PL_LIST_H 158

// ----------------------------------------------------------------------------
// Compose the whole rack into g_fb
// ----------------------------------------------------------------------------
static void module_frame(int y, int h, const char *title) {
    (void)title;
    fb_grad_v(0, y, W, h, SK.bg, SK.bg2);
    fb_bevel(0, y, W, h, 1);
    // title strip
    fb_grad_v(2, y + 2, W - 4, 14, SK.title_a, SK.title_b);
    fb_bevel(2, y + 2, W - 4, 14, 1);
}

static void draw_button_face(int x, int y, int w, int h, int pressed) {
    fb_rect(x, y, w, h, SK.btn_face);
    fb_bevel(x, y, w, h, pressed ? 0 : 1);
}

static void draw_slider_h(int x, int y, int w, int val, int maxv) {
    // trough
    fb_rect(x, y + 2, w, 5, SK.lcd_bg);
    fb_bevel(x, y + 2, w, 5, 0);
    // knob
    int kx = x + (w - 8) * val / (maxv ? maxv : 1);
    draw_button_face(kx, y, 8, 9, 0);
    fb_rect(kx + 3, y + 1, 2, 7, SK.accent);
}

static void compose(void) {
    // ============ MAIN ============
    module_frame(MOD_MAIN_Y, MOD_MAIN_H, "MAYTERA HiFi");
    // Title-strip controls. hifi-scrollfix/#2: the kernel window manager keeps
    // INVISIBLE min/maximize/close/cog buttons on the right of the title band even
    // for nochrome windows (it hit-tests them without checking the nochrome flag).
    // The maximize slot sits at window-x [364,380]; our old "S" skin button lived
    // right on top of it, so a click maximised the window instead of cycling the
    // skin. Fix (app side): keep [minimise] and [close] aligned with the kernel's
    // WORKING min/close buttons, and move our own [V]/[T]/[S] buttons LEFT of the
    // kernel button band (window-x < 328) where the kernel does not intercept, and
    // exclude that strip from the drag zone so they fire on click, not drag.
    // ALL five controls live LEFT of the kernel title-button band (window-x < 328)
    // and are excluded from the drag zone, so EVERY click reaches on_click and the
    // kernel never intercepts them. This matters for close too: under the exclusive
    // compositor the kernel's own close button only HIDES the window (its
    // EVENT_WINDOW_CLOSE lands on a global queue that is never drained), so the app
    // would keep running with its viz window orphaned. Handling close ourselves
    // lets us tear the viz down and exit cleanly (#3).
    draw_button_face(256, MOD_MAIN_Y + 3, 12, 12, viz_is_open() ? 1 : 0); // visualizer (V)
    draw_button_face(271, MOD_MAIN_Y + 3, 12, 12, g_opa_idx ? 1 : 0);     // transparency (T)
    draw_button_face(286, MOD_MAIN_Y + 3, 12, 12, 0);                     // change skin (S)
    draw_button_face(301, MOD_MAIN_Y + 3, 12, 12, 0);                     // minimise
    fb_hline(304, MOD_MAIN_Y + 11, 6, SK.ink);                            // minimise "_" glyph
    draw_button_face(316, MOD_MAIN_Y + 3, 12, 12, 0);                     // close (X)
    fb_hline(319, MOD_MAIN_Y + 6, 6, 0xE05050);
    fb_hline(319, MOD_MAIN_Y + 11, 6, 0xE05050);

    // LCD panel (time + spectrum + indicators)
    fb_rect(8, MOD_MAIN_Y + 22, W - 16, 44, SK.lcd_bg);
    fb_bevel(8, MOD_MAIN_Y + 22, W - 16, 44, 0);

    // 7-seg time
    long disp = g_show_remaining ? (g_track_len - g_elapsed) : g_elapsed;
    if (disp < 0) disp = 0;
    draw_time(16, MOD_MAIN_Y + 28, disp / 60, disp % 60);

    // spectrum analyzer
    int sx = 150, sy = MOD_MAIN_Y + 26, sw = 130, sh = 36;
    fb_rect(sx - 2, sy - 2, sw + 4, sh + 4, SK.lcd_bg);
    int bw = sw / NUM_BARS;
    for (int i = 0; i < NUM_BARS; i++) {
        int bh = g_bar[i] * sh / 63;
        int bx = sx + i * bw;
        for (int yy = 0; yy < bh; yy++) {
            int level = (sh - yy) * 63 / sh;   // 0 bottom .. 63 top
            uint32_t c;
            if (level > 44)      c = 0xE03020;  // red top
            else if (level > 26) c = 0xE0C020;  // amber mid
            else                 c = 0x30D040;  // green base
            fb_hline(bx, sy + sh - 1 - yy, bw - 1, c);
        }
        // peak cap
        int pk = g_peak[i] * sh / 63;
        if (pk > 0 && pk < sh)
            fb_hline(bx, sy + sh - 1 - pk, bw - 1, 0xC8D6E6);
    }

    // album art thumbnail (inside the right edge of the LCD panel)
    mp_draw_art(W - 64, MOD_MAIN_Y + 24, 56);   // #335 larger cover thumbnail

    // title marquee strip (narrowed on the right to leave room for album art)
    fb_rect(8, MOD_MAIN_Y + 70, 316, 14, SK.lcd_bg);
    fb_bevel(8, MOD_MAIN_Y + 70, 316, 14, 0);

    // info strip (state + toggles) drawn as small bevel chips
    draw_button_face(8,  MOD_MAIN_Y + 88, 56, 12, g_shuffle ? 1 : 0);
    draw_button_face(66, MOD_MAIN_Y + 88, 56, 12, g_repeat  ? 1 : 0);

    // transport buttons
    for (int i = 0; i < HB_N; i++) {
        int pressed = (i == HB_PLAY  && g_state == ST_PLAYING) ||
                      (i == HB_PAUSE && g_state == ST_PAUSED);
        draw_button_face(tb_x(i), TB_Y, TB_W, TB_H, pressed);
    }
    // glyphs (drawn into fb so they are crisp)
    {
        uint32_t g = SK.ink;
        int cy = TB_Y + TB_H / 2;
        // prev |<
        fb_rect(tb_x(0) + 9, cy - 5, 2, 10, g);
        for (int k = 0; k < 6; k++) fb_vline(tb_x(0) + 19 - k, cy - k, 2 * k + 1 < 10 ? 2 * k + 1 : 10, g);
        // play >
        for (int k = 0; k < 8; k++) fb_vline(tb_x(1) + 10 + k, cy - (8 - k), 2 * (8 - k), g);
        // pause ||
        fb_rect(tb_x(2) + 10, cy - 5, 3, 10, g);
        fb_rect(tb_x(2) + 16, cy - 5, 3, 10, g);
        // stop []
        fb_rect(tb_x(3) + 10, cy - 5, 10, 10, g);
        // next >|
        for (int k = 0; k < 6; k++) fb_vline(tb_x(4) + 9 + k, cy - k, 2 * k + 1 < 10 ? 2 * k + 1 : 10, g);
        fb_rect(tb_x(4) + 19, cy - 5, 2, 10, g);
        // eject ^_
        for (int k = 0; k < 5; k++) fb_hline(tb_x(5) + 10 - k, cy - 4 + k, 1 + 2 * k, g);
        fb_rect(tb_x(5) + 5, cy + 4, 11, 2, g);
    }

    // seek slider (visual)
    draw_slider_h(SEEK_X, SEEK_Y, SEEK_W, g_seek, 100);
    // volume + balance
    draw_slider_h(VOL_X, VOL_Y, VOL_W, g_vol, 100);
    draw_slider_h(BAL_X, VOL_Y, BAL_W, g_balance + 50, 100);

    // ============ EQ ============
    module_frame(MOD_EQ_Y, MOD_EQ_H, "GRAPHIC EQUALIZER");
    draw_button_face(8,  MOD_EQ_Y + 22, 26, 12, g_eq_on ? 1 : 0);   // ON
    draw_button_face(36, MOD_EQ_Y + 22, 30, 12, 0);                 // AUTO
    draw_button_face(W - 60, MOD_EQ_Y + 22, 52, 12, 0);             // PRESET
    // EQ response area background
    fb_rect(8, EQ_SL_Y - 2, W - 16, EQ_SL_H + 4, SK.lcd_bg);
    fb_bevel(8, EQ_SL_Y - 2, W - 16, EQ_SL_H + 4, 0);
    // preamp slider (vertical)
    {
        int x = EQ_PRE_X + 2, y = EQ_SL_Y, h = EQ_SL_H;
        fb_rect(x + 4, y, 4, h, SK.dark);
        int kny = y + (h - 8) * (63 - g_eq_preamp) / 63;
        draw_button_face(x, kny, 12, 8, 0);
    }
    // 10 band sliders + response curve
    int prevcx = -1, prevcy = -1;
    for (int i = 0; i < NUM_EQ; i++) {
        int x = EQ_BANDS_X + i * EQ_BANDS_DX;
        int y = EQ_SL_Y, h = EQ_SL_H;
        fb_rect(x + 4, y, 3, h, SK.dark);
        int kny = y + (h - 8) * (63 - g_eq[i]) / 63;
        draw_button_face(x, kny, 12, 8, 0);
        // response curve node
        int cx = x + 6, cy = kny + 4;
        if (prevcx >= 0) {
            // simple line
            int steps = cx - prevcx;
            for (int s = 0; s <= steps; s++) {
                int yy = prevcy + (cy - prevcy) * s / (steps ? steps : 1);
                px(prevcx + s, yy, op(SK.accent));
                px(prevcx + s, yy + 1, op(SK.accent));
            }
        }
        prevcx = cx; prevcy = cy;
    }

    // ============ PLAYLIST ============
    module_frame(MOD_PL_Y, MOD_PL_H, "PLAYLIST");
    fb_rect(PL_LIST_X, PL_LIST_Y, W - 2 * PL_LIST_X, PL_LIST_H, SK.lcd_bg);
    fb_bevel(PL_LIST_X, PL_LIST_Y, W - 2 * PL_LIST_X, PL_LIST_H, 0);
    int visible = PL_LIST_H / PL_ROW_H;
    for (int i = 0; i < visible && (g_pl_scroll + i) < g_ntracks; i++) {
        int idx = g_pl_scroll + i;
        int ry = PL_LIST_Y + 1 + i * PL_ROW_H;
        if (idx == g_sel)
            fb_rect(PL_LIST_X + 1, ry, W - 2 * PL_LIST_X - 2, PL_ROW_H, SK.title_a);
    }
    // bottom status strip
    fb_rect(PL_LIST_X, PL_LIST_Y + PL_LIST_H + 2, W - 2 * PL_LIST_X, 14, SK.bg2);
    fb_bevel(PL_LIST_X, PL_LIST_Y + PL_LIST_H + 2, W - 2 * PL_LIST_X, 14, 0);

    draw_panel_toggles_fb();   // #342 [EQ][PL][VIS][ART] chips in the title strip
}

// ----------------------------------------------------------------------------
// Overlay pixel text (drawn after the blit, on top of the chrome)
// ----------------------------------------------------------------------------
static void marquee(char *out, int outsz) {
    // Build a wrapping marquee window of the current track title.
    char base[160];
    const char *t = g_now[0] ? g_now : "MAYTERA HiFi  -  RETRO AUDIO RACK";
    snprintf(base, sizeof(base), "%s        ", t);
    int L = (int)strlen(base);
    int vis = outsz - 1;
    if (vis > 44) vis = 44;
    int start = (L > 0) ? (g_title_scroll % L) : 0;
    for (int i = 0; i < vis; i++) out[i] = base[(start + i) % L];
    out[vis] = 0;
}

// #335: antialiased TTF for playlist/library/labels. LCD digits + the scrolling
// marquee title stay bitmap (skin-authentic). Size packs into the color top byte.
static inline void mp_text(int x, int y, const char *s, uint32_t c) { win_draw_text_ttf(win, x, y, s, 11, c & 0xFFFFFF); }
static inline void mp_text_sz(int x, int y, const char *s, int sz, uint32_t c) { win_draw_text_ttf(win, x, y, s, sz, c & 0xFFFFFF); }

// #342 retargetable overlay-text helpers. Each panel is now its own window, so
// overlay text for the EQ / PL sections must be drawn into eq_win / pl_win with a
// y-offset subtracting the section's g_fb origin. Set g_ov_win / g_ov_dy before a
// block; a closed panel (handle -1) makes these no-ops.
static int g_ov_win = -1;
static int g_ov_dy  = 0;
static void OT(int x, int y, const char *s, uint32_t c)          { if (g_ov_win >= 0) win_draw_text_ttf(g_ov_win, x, y - g_ov_dy, s, 11, c & 0xFFFFFF); }
static void OTS(int x, int y, const char *s, int sz, uint32_t c) { if (g_ov_win >= 0) win_draw_text_ttf(g_ov_win, x, y - g_ov_dy, s, sz, c & 0xFFFFFF); }
static void OSM(int x, int y, const char *s, uint32_t c)         { if (g_ov_win >= 0) win_draw_text_small(g_ov_win, x, y - g_ov_dy, s, c); }

static void overlay(void) {
    char buf[80];
    // ---- MAIN window text ----
    g_ov_win = win; g_ov_dy = 0;
    // titlebar caption (shifted right of the [EQ][PL][VIS][ART] toggle chips)
    OSM(102, MOD_MAIN_Y + 4, "MAYTERA HiFi", SK.title_ink);
    // #2/#3: V / T / S button labels, centred on the relocated boxes.
    OSM(259, MOD_MAIN_Y + 4, "V", viz_is_open() ? SK.lcd_fg : SK.title_ink);
    OSM(274, MOD_MAIN_Y + 4, "T", g_opa_idx ? SK.lcd_fg : SK.title_ink);
    OSM(289, MOD_MAIN_Y + 4, "S", SK.title_ink);

    // indicators (right of LCD)
    OSM(286, MOD_MAIN_Y + 26, "kbps", SK.ink_dim);
    OSM(316, MOD_MAIN_Y + 26, g_state == ST_STOPPED ? "---" : "192", SK.lcd_fg);
    OSM(286, MOD_MAIN_Y + 38, "kHz", SK.ink_dim);
    OSM(316, MOD_MAIN_Y + 38, g_state == ST_STOPPED ? "--" : "44", SK.lcd_fg);
    OSM(286, MOD_MAIN_Y + 50, "STEREO", SK.ink_dim);

    // marquee title
    marquee(buf, sizeof(buf));
    OSM(12, MOD_MAIN_Y + 72, buf, SK.lcd_fg);

    // state + toggles
    const char *st = g_state == ST_PLAYING ? "PLAYING" :
                     g_state == ST_PAUSED  ? "PAUSED " : "STOPPED";
    OT(130, MOD_MAIN_Y + 89, st, SK.lcd_fg);
    OT(14, MOD_MAIN_Y + 89, "SHUFFLE", g_shuffle ? SK.lcd_fg : SK.ink_dim);
    OT(72, MOD_MAIN_Y + 89, "REPEAT", g_repeat ? SK.lcd_fg : SK.ink_dim);
    snprintf(buf, sizeof(buf), "VOL %d", g_vol);
    OT(250, MOD_MAIN_Y + 89, buf, SK.ink_dim);

    // ---- EQ window text ----
    g_ov_win = eq_win; g_ov_dy = MOD_EQ_Y;
    OT(11, MOD_EQ_Y + 4, "GRAPHIC EQUALIZER", SK.title_ink);
    OT(13, MOD_EQ_Y + 23, "ON", g_eq_on ? SK.lcd_fg : SK.ink_dim);
    OT(40, MOD_EQ_Y + 23, "AUTO", SK.ink_dim);
    OT(W - 56, MOD_EQ_Y + 23, "FLAT", SK.ink);
    OTS(EQ_PRE_X, MOD_EQ_Y + EQ_SL_H + 42, "PRE", 10, SK.ink_dim);
    static const char *bl[NUM_EQ] = {"60","170","310","600","1K","3K","6K","12K","14K","16K"};
    for (int i = 0; i < NUM_EQ; i++)
        OTS(EQ_BANDS_X + i * EQ_BANDS_DX - 2, MOD_EQ_Y + EQ_SL_H + 42, bl[i], 10, SK.ink_dim);

    // ---- PLAYLIST window text ----
    g_ov_win = pl_win; g_ov_dy = MOD_PL_Y;
    OT(11, MOD_PL_Y + 4, "PLAYLIST EDITOR", SK.title_ink);
    OT(W - 132, MOD_PL_Y + 4, "LIB", SK.ink);
    OT(W - 92, MOD_PL_Y + 4, "SAVE", SK.ink);
    OT(W - 52, MOD_PL_Y + 4, "LOAD", SK.ink);
    int visible = PL_LIST_H / PL_ROW_H;
    for (int i = 0; i < visible && (g_pl_scroll + i) < g_ntracks; i++) {
        int idx = g_pl_scroll + i;
        int ry = PL_LIST_Y + 2 + i * PL_ROW_H;
        uint32_t c = (idx == g_cur) ? SK.lcd_fg
                   : (idx == g_sel) ? SK.title_ink : SK.ink;
        char lbl[96]; track_label(&g_tracks[idx], lbl, sizeof lbl);
        snprintf(buf, sizeof(buf), "%2d. %s", idx + 1, lbl);
        OT(PL_LIST_X + 6, ry, buf, c);
    }
    snprintf(buf, sizeof(buf), "%d TRACKS", g_ntracks);
    OT(PL_LIST_X + 6, PL_LIST_Y + PL_LIST_H + 4, buf, SK.ink_dim);
    snprintf(buf, sizeof(buf), "%s", g_dir);
    OT(W - 140, PL_LIST_Y + PL_LIST_H + 4, buf, SK.ink_dim);

    overlay_panel_toggles();   // chip labels (main window, always)
}

// ----------------------------------------------------------------------------
// Playlist scan
// ----------------------------------------------------------------------------
static void build_path(char *dst, const char *dir, const char *file) {
    char *p = dst; const char *s = dir;
    while (*s) *p++ = *s++;
    if (p > dst && *(p - 1) != '/') *p++ = '/';
    s = file; while (*s) *p++ = *s++;
    *p = 0;
}

static int is_audio(const char *n) {
    int L = (int)strlen(n);
    if (L < 4 || n[L - 4] != '.') {
        if (L < 5 || n[L - 5] != '.') return 0;
    }
    const char *e = n + L;
    // check common audio extensions, case-insensitive
    static const char *ex[] = {"mp3","wav","ogg","flac","m4a","aac","opus", 0};
    for (int k = 0; ex[k]; k++) {
        int el = (int)strlen(ex[k]);
        if (L < el + 1 || n[L - el - 1] != '.') continue;
        int ok = 1;
        for (int j = 0; j < el; j++) {
            char c = e[-el + j];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != ex[k][j]) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static void scan_dir(const char *dir) {
    g_ntracks = 0; g_sel = -1; g_pl_scroll = 0;
    dirent_t e;
    for (int i = 0; g_ntracks < MAX_TRACKS; i++) {
        if (sys_readdir(dir, i, &e) < 0) break;
        if (e.type == 1) continue;            // skip dirs
        if (!is_audio(e.name)) continue;
        int j = 0;
        while (e.name[j] && j < NAME_LEN - 1) { g_tracks[g_ntracks].name[j] = e.name[j]; j++; }
        g_tracks[g_ntracks].name[j] = 0;
        build_path(g_tracks[g_ntracks].path, dir, g_tracks[g_ntracks].name);
        parse_metadata(g_tracks[g_ntracks].path, &g_tracks[g_ntracks]);
        g_ntracks++;
    }
    if (g_ntracks > 0) g_sel = 0;
}

// ----------------------------------------------------------------------------
// Playback (spawn helper so the UI stays live)
// ----------------------------------------------------------------------------
// #335: (re)launch the blocking decode helper for `full`, keeping at most ONE
// helper alive. The previous helper is force-killed and reaped so a continuous
// loop never leaks process slots (MAX_PROCESSES is only 64).
static void spawn_play(const char *full) {
    if (g_play_pid > 0) {
        // Guarantee EXACTLY ONE stream to the DAC. On AUTO-advance the track has
        // already finished, so we simply wait for the helper to exit -> the iso
        // ring fully drains before the next stream starts (no overlap/garble, no
        // mid-drain cut). Only a MANUAL switch force-kills the current helper.
        if (!g_advancing) syscall2(SYS_KILL, g_play_pid, 9);
        int st; sys_waitpid(g_play_pid, &st, 0);    // block until it exits (ring free)
        g_play_pid = -1;
    }
    char *argv[3];
    argv[0] = "musicplr"; argv[1] = "--play"; argv[2] = (char *)full;
    g_play_pid = sys_spawn_args("/APPS/MUSICPLR", argv, 3);
    g_last_pos_ms = -1; g_pos_stall = 0;            // reset end-of-track detectors
}

// Play an absolute file path (used by file-association launch + /CONFIG/MPAUTO).
static void play_path(const char *full) {
    const char *b = full;
    for (const char *p = full; *p; p++) if (*p == '/') b = p + 1;
    // #342: parse tags + load the embedded cover BEFORE spawning the decode
    // helper, so the art read does not contend with the helper's streaming reads
    // of the same file (which made large-file art reads fail intermittently).
    track_t meta; mp_zero(&meta, (int)sizeof meta);
    mp_cpy(meta.name, b, NAME_LEN);
    parse_metadata(full, &meta);
    track_label(&meta, g_now, (int)sizeof g_now);
    if (meta.duration > 0) g_track_len = meta.duration;
    // #342: defer cover loading to the main loop (see g_art_pending) so the UI +
    // audio start immediately instead of blocking on a slow whole-file read.
    mp_cpy(g_pending_art, full, sizeof g_pending_art);
    mp_cpy(g_pending_artist, meta.artist, sizeof g_pending_artist);
    mp_cpy(g_pending_album, meta.album, sizeof g_pending_album);
    g_art_have = 0; g_art_path[0] = 0; g_art_pending = 1; g_art_retry = 0; g_art_retry_at = 0;
    spawn_play(full);
    g_state = ST_PLAYING;
    g_play_start_sec = now_sec();
    g_elapsed = 0; g_seek = 0; g_title_scroll = 0;
    set_volume(g_vol);
    for (int i = 0; i < g_ntracks; i++)
        if (strcmp(g_tracks[i].name, b) == 0) { g_sel = i; g_cur = i; break; }
}

static void play_index(int idx) {
    if (idx < 0 || idx >= g_ntracks) return;
    char path[256];
    if (g_tracks[idx].path[0]) strcpy(path, g_tracks[idx].path);
    else build_path(path, g_dir, g_tracks[idx].name);
    g_cur = idx; g_sel = idx;
    g_state = ST_PLAYING;
    if (!g_tracks[idx].has_meta) parse_metadata(path, &g_tracks[idx]);
    track_label(&g_tracks[idx], g_now, (int)sizeof g_now);
    if (g_tracks[idx].duration > 0) g_track_len = g_tracks[idx].duration;
    // #342: defer cover loading to the main loop (UI + audio start immediately).
    mp_cpy(g_pending_art, path, sizeof g_pending_art);
    mp_cpy(g_pending_artist, g_tracks[idx].artist, sizeof g_pending_artist);
    mp_cpy(g_pending_album, g_tracks[idx].album, sizeof g_pending_album);
    g_art_have = 0; g_art_path[0] = 0; g_art_pending = 1; g_art_retry = 0; g_art_retry_at = 0;
    spawn_play(path);
    g_play_start_sec = now_sec();
    g_elapsed = 0; g_seek = 0; g_title_scroll = 0;
    set_volume(g_vol);
}

static void do_stop(void)  { g_state = ST_STOPPED; g_elapsed = 0; g_seek = 0; }
static void do_pause(void) { if (g_state == ST_PLAYING) g_state = ST_PAUSED; else if (g_state == ST_PAUSED) g_state = ST_PLAYING; }
static void play_neighbor(int d) {
    if (g_ntracks == 0) return;
    int idx = (g_cur < 0 ? g_sel : g_cur) + d;
    if (idx < 0) idx = g_ntracks - 1;
    if (idx >= g_ntracks) idx = 0;
    play_index(idx);
}

// ============================================================================
// WinAmp .wsz classic skin engine (phase 2a)
// ----------------------------------------------------------------------------
// A .wsz is a ZIP of BMP sprite sheets + a couple of .txt config files. We
// extract it with the archiver lib, decode the BMPs to ARGB sprites, and blit
// the documented sub-rectangles (the WinAmp 2.x skinning spec) to render an
// authentic main / EQ / playlist rack instead of the procedural palette skins.
// ============================================================================
typedef struct { uint32_t *px; int w, h; } sprite_t;

typedef struct {
    int loaded;
    int nums_ex;          // numbers sheet is the extended NUMS_EX variant
    uint32_t rack_bg;     // colour for the area around the windows
    sprite_t main, titlebar, cbuttons, numbers, text, volume, balance,
             posbar, monoster, playpaus, shufrep, eqmain, pledit;
    // playlist colours (from PLEDIT.TXT)
    uint32_t pl_normal, pl_current, pl_normalbg, pl_selectbg;
    char name[64];        // display name (skin file basename)
} skin_bmp_t;
static skin_bmp_t g_bskin;

static int      g_bitmap_active = 0;     // render from g_bskin instead of palette
static int      g_sel_skin = 0;          // unified skin index (builtin then wsz)

// discovered .wsz files
#define MAX_WSZ 24
static char g_wsz[MAX_WSZ][160];
static int  g_nwsz = 0;

// --- little-endian readers (BMP headers are unaligned) ---------------------
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// --- case-insensitive equality --------------------------------------------
static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

// --- read a whole file into a malloc'd buffer ------------------------------
// #342: the FS read path can return a transient short/zero read mid-file for
// large tracks (observed: a 15 MB MP3 stops early, hiding a front-of-file APIC
// cover). The OLD loop broke on the first n<=0 and returned a truncated buffer.
// We now yield() and retry a bounded number of times so a transient stall is
// ridden out; a genuine EOF simply leaves off < sz after the retries.
static uint8_t *read_file(const char *path, size_t *out_len) {
    int fd = sys_open(path, 0);
    if (fd < 0) return 0;
    long sz = sys_seek(fd, 0, 2);   // SEEK_END
    if (sz <= 0) { sys_close(fd); return 0; }
    sys_seek(fd, 0, 0);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { sys_close(fd); return 0; }
    long off = 0; int zeros = 0;
    while (off < sz) {
        long n = sys_read(fd, buf + off, (unsigned long)(sz - off));
        if (n <= 0) { if (++zeros >= 600) break; yield(); continue; }
        off += n; zeros = 0;
    }
    sys_close(fd);
    *out_len = (size_t)off;
    return buf;
}

// ============================================================================
// PHASE 2b: tag metadata parsing (ID3v2/v1, Vorbis comment, FLAC, MP4 atoms),
// embedded album art extraction (APIC / FLAC PICTURE / MP4 covr / OGG b64),
// and a recursive music library with a browse view + persisted index.
// All parsers are bounds-checked: malformed tags must never crash the player.
// ============================================================================

// big-endian readers
static unsigned int be32(const uint8_t *p) { return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3]; }

// bounded string copy (NUL-terminated)
static void mp_cpy(char *d, const char *s, int cap) { int o = 0; for (; s[o] && o < cap - 1; o++) d[o] = s[o]; d[o] = 0; }
static void mp_zero(void *p, int n) { uint8_t *b = (uint8_t *)p; for (int i = 0; i < n; i++) b[i] = 0; }
static int  atoi_s(const char *s) { int v = 0, neg = 0; while (*s == ' ') s++; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return neg ? -v : v; }

// copy a raw (latin1/utf8) field with control-char filtering + trim
static void set_field(char *dst, int cap, const char *src, int srclen) {
    int o = 0, i = 0;
    while (i < srclen && (src[i] == ' ' || src[i] == '\t')) i++;
    for (; i < srclen && o < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0) break;
        if (c < 0x20) continue;
        if (c >= 0x80) c = '?';            // non-ASCII placeholder (bitmap font is ASCII)
        dst[o++] = (char)c;
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

// decode an ID3v2 text frame body (handles encoding byte: 0 latin1, 1/2 UTF-16, 3 UTF-8)
static void id3_text(char *dst, int cap, const uint8_t *p, int len) {
    dst[0] = 0;
    if (len <= 1) return;
    int enc = p[0]; p++; len--;
    if (enc == 1 || enc == 2) {
        int le = 1;
        if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE) { le = 1; p += 2; len -= 2; }
        else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) { le = 0; p += 2; len -= 2; }
        int o = 0;
        for (int i = 0; i + 1 < len && o < cap - 1; i += 2) {
            unsigned char lo = le ? p[i] : p[i + 1];
            unsigned char hi = le ? p[i + 1] : p[i];
            if (lo == 0 && hi == 0) break;
            char c = (hi || lo >= 0x80) ? '?' : (char)lo;
            if (c >= 0x20) dst[o++] = c;
        }
        while (o > 0 && dst[o - 1] == ' ') o--;
        dst[o] = 0;
    } else {
        set_field(dst, cap, (const char *)p, len);
    }
}

// case-insensitive key match: f[0..klen) == lit ?
static int key_is(const char *f, int klen, const char *lit) {
    int i = 0;
    for (; i < klen && lit[i]; i++) {
        char a = f[i], b = lit[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return i == klen && lit[i] == 0;
}

// file format codes
enum { E_OTHER, E_MP3, E_FLAC, E_OGG, E_OPUS, E_M4A, E_WAV };
static int ext_of(const char *path) {
    int L = (int)strlen(path); const char *e = 0;
    for (int i = L - 1; i >= 0 && i > L - 7; i--) if (path[i] == '.') { e = path + i + 1; break; }
    if (!e) return E_OTHER;
    char x[6]; int o = 0; for (; e[o] && o < 5; o++) { char c = e[o]; if (c >= 'A' && c <= 'Z') c += 32; x[o] = c; } x[o] = 0;
    if (!strcmp(x, "mp3")) return E_MP3;
    if (!strcmp(x, "flac")) return E_FLAC;
    if (!strcmp(x, "ogg") || !strcmp(x, "oga")) return E_OGG;
    if (!strcmp(x, "opus")) return E_OPUS;
    if (!strcmp(x, "m4a") || !strcmp(x, "mp4") || !strcmp(x, "aac")) return E_M4A;
    if (!strcmp(x, "wav")) return E_WAV;
    return E_OTHER;
}

// shared scratch buffer for header reads (64 KB covers ID3 text frames, small
// FLAC/OGG tags, and front-located MP4 moov; the MP4 path also retries the file
// tail). Album-art extraction re-reads the whole file separately.
#define META_HEAD (64 * 1024)
static uint8_t g_meta_buf[META_HEAD];

static int read_head(const char *path, uint8_t *buf, int cap) {
    int fd = sys_open(path, 0); if (fd < 0) return -1;
    int off = 0;
    while (off < cap) { long n = sys_read(fd, buf + off, (unsigned long)(cap - off)); if (n <= 0) break; off += (int)n; }
    sys_close(fd);
    return off;
}
static int read_tail(const char *path, uint8_t *buf, int cap) {
    int fd = sys_open(path, 0); if (fd < 0) return -1;
    long sz = sys_seek(fd, 0, SEEK_END);
    if (sz <= 0) { sys_close(fd); return -1; }
    long start = sz > cap ? sz - cap : 0;
    sys_seek(fd, start, SEEK_SET);
    int off = 0;
    while (off < cap) { long n = sys_read(fd, buf + off, (unsigned long)(cap - off)); if (n <= 0) break; off += (int)n; }
    sys_close(fd);
    return off;
}

// --- ID3v2 (.2/.3/.4) text frames ------------------------------------------
static int tag3(const char *id, const char *m) { return id[0] == m[0] && id[1] == m[1] && id[2] == m[2]; }
static int tag4(const char *id, const char *m) { return id[0] == m[0] && id[1] == m[1] && id[2] == m[2] && id[3] == m[3]; }

static int parse_id3v2(const uint8_t *b, int len, track_t *t) {
    if (len < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
    int ver = b[3], flags = b[5];
    int tagsize = ((b[6] & 0x7F) << 21) | ((b[7] & 0x7F) << 14) | ((b[8] & 0x7F) << 7) | (b[9] & 0x7F);
    int end = 10 + tagsize; if (end > len) end = len;
    int p = 10;
    if (flags & 0x40) {  // extended header
        if (p + 4 > end) return 0;
        if (ver >= 4) { int esz = ((b[p] & 0x7F) << 21) | ((b[p+1] & 0x7F) << 14) | ((b[p+2] & 0x7F) << 7) | (b[p+3] & 0x7F); p += esz; }
        else { int esz = (b[p] << 24) | (b[p+1] << 16) | (b[p+2] << 8) | b[p+3]; p += 4 + esz; }
    }
    int v22 = (ver == 2), idlen = v22 ? 3 : 4;
    while (p + (v22 ? 6 : 10) <= end) {
        char id[5]; for (int i = 0; i < idlen; i++) id[i] = (char)b[p + i]; id[idlen] = 0;
        if (id[0] == 0) break;
        const uint8_t *s = b + p + idlen; int fsz;
        if (v22) fsz = (s[0] << 16) | (s[1] << 8) | s[2];
        else if (ver >= 4) fsz = ((s[0] & 0x7F) << 21) | ((s[1] & 0x7F) << 14) | ((s[2] & 0x7F) << 7) | (s[3] & 0x7F);
        else fsz = (s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3];
        int dpos = p + idlen + (v22 ? 3 : 6);
        if (fsz <= 0 || dpos + fsz > end) break;
        const uint8_t *d = b + dpos; char tmp[24];
        if (!v22) {
            if      (tag4(id, "TIT2")) id3_text(t->title,  sizeof t->title,  d, fsz);
            else if (tag4(id, "TPE1")) id3_text(t->artist, sizeof t->artist, d, fsz);
            else if (tag4(id, "TALB")) id3_text(t->album,  sizeof t->album,  d, fsz);
            else if (tag4(id, "TCON")) id3_text(t->genre,  sizeof t->genre,  d, fsz);
            else if (tag4(id, "TRCK")) { id3_text(tmp, sizeof tmp, d, fsz); t->track_no = atoi_s(tmp); }
            else if (tag4(id, "TYER") || tag4(id, "TDRC") || tag4(id, "TDAT")) { id3_text(tmp, sizeof tmp, d, fsz); int y = atoi_s(tmp); if (y > 1000) t->year = y; }
        } else {
            if      (tag3(id, "TT2")) id3_text(t->title,  sizeof t->title,  d, fsz);
            else if (tag3(id, "TP1")) id3_text(t->artist, sizeof t->artist, d, fsz);
            else if (tag3(id, "TAL")) id3_text(t->album,  sizeof t->album,  d, fsz);
            else if (tag3(id, "TCO")) id3_text(t->genre,  sizeof t->genre,  d, fsz);
            else if (tag3(id, "TRK")) { id3_text(tmp, sizeof tmp, d, fsz); t->track_no = atoi_s(tmp); }
            else if (tag3(id, "TYE")) { id3_text(tmp, sizeof tmp, d, fsz); int y = atoi_s(tmp); if (y > 1000) t->year = y; }
        }
        p = dpos + fsz;
    }
    return (t->title[0] || t->artist[0] || t->album[0]);
}

static void parse_id3v1(const char *path, track_t *t) {
    int fd = sys_open(path, 0); if (fd < 0) return;
    long sz = sys_seek(fd, 0, SEEK_END); if (sz < 128) { sys_close(fd); return; }
    sys_seek(fd, sz - 128, SEEK_SET);
    unsigned char b[128]; long n = sys_read(fd, b, 128); sys_close(fd);
    if (n < 128 || b[0] != 'T' || b[1] != 'A' || b[2] != 'G') return;
    if (!t->title[0])  set_field(t->title,  sizeof t->title,  (char *)b + 3,  30);
    if (!t->artist[0]) set_field(t->artist, sizeof t->artist, (char *)b + 33, 30);
    if (!t->album[0])  set_field(t->album,  sizeof t->album,  (char *)b + 63, 30);
    if (!t->year) { char y[5]; for (int i = 0; i < 4; i++) y[i] = (char)b[93 + i]; y[4] = 0; int yy = atoi_s(y); if (yy > 1000) t->year = yy; }
    if (!t->track_no && b[125] == 0 && b[126] != 0) t->track_no = b[126];   // ID3v1.1
}

// --- Vorbis comment (FLAC/OGG/Opus, little-endian lengths) ------------------
static void parse_vorbis_comment(const uint8_t *b, int len, track_t *t) {
    if (len < 8) return;
    int p = 0;
    unsigned int vlen = rd32(b + p); p += 4;
    if (p + (int)vlen + 4 > len) return; p += (int)vlen;
    unsigned int cnt = rd32(b + p); p += 4;
    for (unsigned int i = 0; i < cnt && p + 4 <= len; i++) {
        unsigned int fl = rd32(b + p); p += 4;
        if (fl > (unsigned)(len - p)) break;
        const char *f = (const char *)(b + p);
        int eq = -1; for (unsigned int j = 0; j < fl; j++) if (f[j] == '=') { eq = (int)j; break; }
        if (eq > 0) {
            const char *val = f + eq + 1; int vl = (int)fl - eq - 1; char tmp[24];
            if      (key_is(f, eq, "TITLE"))  set_field(t->title,  sizeof t->title,  val, vl);
            else if (key_is(f, eq, "ARTIST")) set_field(t->artist, sizeof t->artist, val, vl);
            else if (key_is(f, eq, "ALBUM"))  set_field(t->album,  sizeof t->album,  val, vl);
            else if (key_is(f, eq, "GENRE"))  set_field(t->genre,  sizeof t->genre,  val, vl);
            else if (key_is(f, eq, "TRACKNUMBER")) { set_field(tmp, sizeof tmp, val, vl); t->track_no = atoi_s(tmp); }
            else if (key_is(f, eq, "DATE") || key_is(f, eq, "YEAR")) { set_field(tmp, sizeof tmp, val, vl); int y = atoi_s(tmp); if (y > 1000) t->year = y; }
        }
        p += (int)fl;
    }
}

static int parse_flac(const uint8_t *b, int len, track_t *t) {
    if (len < 4 || b[0] != 'f' || b[1] != 'L' || b[2] != 'a' || b[3] != 'C') return 0;
    int p = 4;
    while (p + 4 <= len) {
        int last = b[p] & 0x80, type = b[p] & 0x7F;
        int bsz = (b[p+1] << 16) | (b[p+2] << 8) | b[p+3]; p += 4;
        if (bsz < 0 || p + bsz > len) break;
        if (type == 4) parse_vorbis_comment(b + p, bsz, t);
        else if (type == 0 && bsz >= 18) {   // STREAMINFO -> duration
            const uint8_t *s = b + p;
            unsigned int sr = ((unsigned)s[10] << 12) | ((unsigned)s[11] << 4) | (s[12] >> 4);
            unsigned long long ts = ((unsigned long long)(s[13] & 0x0F) << 32) | ((unsigned long long)s[14] << 24) | ((unsigned)s[15] << 16) | ((unsigned)s[16] << 8) | s[17];
            if (sr > 0 && ts > 0) t->duration = (int)(ts / sr);
        }
        p += bsz;
        if (last) break;
    }
    return (t->title[0] || t->artist[0] || t->album[0]);
}

static int parse_ogg(const uint8_t *b, int len, track_t *t) {
    for (int i = 0; i + 8 < len; i++) {
        if (b[i]=='O'&&b[i+1]=='p'&&b[i+2]=='u'&&b[i+3]=='s'&&b[i+4]=='T'&&b[i+5]=='a'&&b[i+6]=='g'&&b[i+7]=='s') {
            parse_vorbis_comment(b + i + 8, len - i - 8, t); break;
        }
        if (b[i]==0x03&&b[i+1]=='v'&&b[i+2]=='o'&&b[i+3]=='r'&&b[i+4]=='b'&&b[i+5]=='i'&&b[i+6]=='s') {
            parse_vorbis_comment(b + i + 7, len - i - 7, t); break;
        }
    }
    return (t->title[0] || t->artist[0] || t->album[0]);
}

// --- MP4 / M4A atom tree ----------------------------------------------------
static int mp4_find(const uint8_t *b, int s, int e, const char *name, int *cs, int *ce) {
    int p = s;
    while (p + 8 <= e) {
        unsigned int sz = be32(b + p);
        if (sz == 0) sz = (unsigned)(e - p);
        if (sz < 8) break;
        if ((int)sz > e - p) sz = (unsigned)(e - p);
        if (b[p+4]==(uint8_t)name[0] && b[p+5]==(uint8_t)name[1] && b[p+6]==(uint8_t)name[2] && b[p+7]==(uint8_t)name[3]) {
            *cs = p + 8; *ce = p + (int)sz; return 1;
        }
        p += (int)sz;
    }
    return 0;
}
static int parse_mp4(const uint8_t *b, int len, track_t *t) {
    int ms, me; if (!mp4_find(b, 0, len, "moov", &ms, &me)) return 0;
    int xs, xe;
    if (mp4_find(b, ms, me, "mvhd", &xs, &xe) && xe - xs >= 20) {
        const uint8_t *s = b + xs;
        if (s[0] == 0) { unsigned int ts = be32(s + 12), du = be32(s + 16); if (ts) t->duration = (int)(du / ts); }
    }
    int us, ue; if (!mp4_find(b, ms, me, "udta", &us, &ue)) return (t->duration != 0);
    int es, ee; if (!mp4_find(b, us, ue, "meta", &es, &ee)) return 0;
    es += 4;   // meta has a version/flags word before its children
    int is, ie; if (!mp4_find(b, es, ee, "ilst", &is, &ie)) return 0;
    int p = is;
    while (p + 8 <= ie) {
        unsigned int sz = be32(b + p);
        if (sz == 0) sz = (unsigned)(ie - p);
        if (sz < 8) break;
        if ((int)sz > ie - p) sz = (unsigned)(ie - p);
        const uint8_t *nm = b + p + 4;
        int ds, de;
        if (mp4_find(b, p + 8, p + (int)sz, "data", &ds, &de) && de - ds >= 8) {
            const uint8_t *v = b + ds + 8; int vl = de - (ds + 8);   // skip data type(4)+locale(4)
            if      (nm[0]==0xA9 && nm[1]=='n' && nm[2]=='a' && nm[3]=='m') set_field(t->title,  sizeof t->title,  (char *)v, vl);
            else if (nm[0]==0xA9 && nm[1]=='A' && nm[2]=='R' && nm[3]=='T') set_field(t->artist, sizeof t->artist, (char *)v, vl);
            else if (nm[0]==0xA9 && nm[1]=='a' && nm[2]=='l' && nm[3]=='b') set_field(t->album,  sizeof t->album,  (char *)v, vl);
            else if (nm[0]==0xA9 && nm[1]=='g' && nm[2]=='e' && nm[3]=='n') set_field(t->genre,  sizeof t->genre,  (char *)v, vl);
            else if (nm[0]==0xA9 && nm[1]=='d' && nm[2]=='a' && nm[3]=='y') { char tmp[24]; set_field(tmp, sizeof tmp, (char *)v, vl); int y = atoi_s(tmp); if (y > 1000) t->year = y; }
            else if (nm[0]=='t'  && nm[1]=='r' && nm[2]=='k' && nm[3]=='n') { if (vl >= 4) t->track_no = (v[2] << 8) | v[3]; }
        }
        p += (int)sz;
    }
    return (t->title[0] || t->artist[0] || t->album[0]);
}

// --- MP3 duration (Xing/Info VBR frame count, else CBR from filesize) -------
static const int mp3_br_v1l3[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
static const int mp3_sr_v1[4]    = {44100,48000,32000,0};
static void mp3_duration(const uint8_t *b, int n, const char *path, track_t *t) {
    int i = 0;
    if (n > 10 && b[0]=='I' && b[1]=='D' && b[2]=='3') {
        int ts = ((b[6]&0x7F)<<21)|((b[7]&0x7F)<<14)|((b[8]&0x7F)<<7)|(b[9]&0x7F); i = 10 + ts;
    }
    for (; i + 4 < n; i++) {
        if (b[i] != 0xFF || (b[i+1] & 0xE0) != 0xE0) continue;
        int verbits = (b[i+1] >> 3) & 3, layer = (b[i+1] >> 1) & 3;
        if (verbits != 3 || layer != 1) continue;   // MPEG1 Layer III only (table)
        int bri = (b[i+2] >> 4) & 0xF, sri = (b[i+2] >> 2) & 3;
        int br = mp3_br_v1l3[bri], sr = mp3_sr_v1[sri];
        if (br <= 0 || sr <= 0) continue;
        int frames = 0;
        for (int k = i + 4; k < i + 200 && k + 8 < n; k++) {
            if ((b[k]=='X'&&b[k+1]=='i'&&b[k+2]=='n'&&b[k+3]=='g') || (b[k]=='I'&&b[k+1]=='n'&&b[k+2]=='f'&&b[k+3]=='o')) {
                unsigned int flags = be32(b + k + 4);
                if (flags & 1) frames = (int)be32(b + k + 8);
                break;
            }
        }
        if (frames > 0) t->duration = (int)((long long)frames * 1152 / sr);
        else {
            int fd = sys_open(path, 0);
            if (fd >= 0) { long sz = sys_seek(fd, 0, SEEK_END); sys_close(fd); if (sz > 0) t->duration = (int)((long long)sz * 8 / (br * 1000)); }
        }
        return;
    }
}

// --- dispatcher: fill t->title/artist/album/genre/year/track_no/duration ----
static void parse_metadata(const char *path, track_t *t) {
    t->title[0] = t->artist[0] = t->album[0] = t->genre[0] = 0;
    t->year = 0; t->track_no = 0; t->duration = 0; t->has_meta = 0;
    int ext = ext_of(path);
    int n = read_head(path, g_meta_buf, META_HEAD);
    if (n <= 0) return;
    switch (ext) {
        case E_MP3:
            if (!parse_id3v2(g_meta_buf, n, t)) parse_id3v1(path, t);
            if (!t->duration) mp3_duration(g_meta_buf, n, path, t);
            break;
        case E_FLAC: parse_flac(g_meta_buf, n, t); break;
        case E_OGG:
        case E_OPUS: parse_ogg(g_meta_buf, n, t); break;
        case E_M4A:
            if (!parse_mp4(g_meta_buf, n, t)) {   // moov may be at the file tail
                int m = read_tail(path, g_meta_buf, META_HEAD);
                if (m > 0) parse_mp4(g_meta_buf, m, t);
            }
            break;
        default: break;   // WAV / unknown -> filename only
    }
    t->has_meta = (t->title[0] || t->artist[0] || t->album[0]);
}

// --- display label: "Artist - Title", or Title, or filename -----------------
static void track_label(const track_t *t, char *out, int outsz) {
    if (t->artist[0] && t->title[0]) snprintf(out, outsz, "%s - %s", t->artist, t->title);
    else if (t->title[0])            snprintf(out, outsz, "%s", t->title);
    else                             snprintf(out, outsz, "%s", t->name);
}

// ============================================================================
// Album art extraction (whole-file) + decode via the kernel image decoder
// ============================================================================
static int find_apic(const uint8_t *b, int len, const uint8_t **out, int *outlen) {
    if (len < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
    int ver = b[3];
    int tagsize = ((b[6]&0x7F)<<21)|((b[7]&0x7F)<<14)|((b[8]&0x7F)<<7)|(b[9]&0x7F);
    int end = 10 + tagsize; if (end > len) end = len;
    int v22 = (ver == 2), idlen = v22 ? 3 : 4, p = 10;
    while (p + (v22 ? 6 : 10) <= end) {
        char id[5]; for (int i = 0; i < idlen; i++) id[i] = (char)b[p + i]; id[idlen] = 0;
        if (id[0] == 0) break;
        const uint8_t *s = b + p + idlen; int fsz;
        if (v22) fsz = (s[0] << 16) | (s[1] << 8) | s[2];
        else if (ver >= 4) fsz = ((s[0]&0x7F)<<21)|((s[1]&0x7F)<<14)|((s[2]&0x7F)<<7)|(s[3]&0x7F);
        else fsz = (s[0]<<24)|(s[1]<<16)|(s[2]<<8)|s[3];
        int dpos = p + idlen + (v22 ? 3 : 6);
        if (fsz <= 0 || dpos + fsz > end) break;
        int isart = v22 ? tag3(id, "PIC") : tag4(id, "APIC");
        if (isart) {
            const uint8_t *d = b + dpos; int q = 0, enc = d[q++];
            if (v22) q += 3;                                  // image format (3 chars)
            else { while (q < fsz && d[q]) q++; q++; }        // MIME (NUL-term)
            q++;                                              // picture type
            if (enc == 1 || enc == 2) { while (q + 1 < fsz && (d[q] || d[q+1])) q += 2; q += 2; }
            else { while (q < fsz && d[q]) q++; q++; }        // description
            if (q < fsz) { *out = d + q; *outlen = fsz - q; return 1; }
        }
        p = dpos + fsz;
    }
    return 0;
}
static int flac_pic_body(const uint8_t *d, int bsz, const uint8_t **out, int *outlen) {
    int q = 4; if (q + 4 > bsz) return 0;
    unsigned int mlen = be32(d + q); q += 4 + (int)mlen;
    if (q + 4 > bsz) return 0;
    unsigned int dlen = be32(d + q); q += 4 + (int)dlen;
    if (q + 20 > bsz) return 0; q += 16;
    unsigned int plen = be32(d + q); q += 4;
    if (q + (int)plen > bsz || plen < 8) return 0;
    *out = d + q; *outlen = (int)plen; return 1;
}
static int find_flac_pic(const uint8_t *b, int len, const uint8_t **out, int *outlen) {
    if (len < 4 || b[0] != 'f' || b[1] != 'L' || b[2] != 'a' || b[3] != 'C') return 0;
    int p = 4;
    while (p + 4 <= len) {
        int last = b[p] & 0x80, type = b[p] & 0x7F;
        int bsz = (b[p+1] << 16) | (b[p+2] << 8) | b[p+3]; p += 4;
        if (bsz < 0 || p + bsz > len) break;
        if (type == 6 && flac_pic_body(b + p, bsz, out, outlen)) return 1;
        p += bsz; if (last) break;
    }
    return 0;
}
static int find_mp4_cover(const uint8_t *b, int len, const uint8_t **out, int *outlen) {
    int ms, me; if (!mp4_find(b, 0, len, "moov", &ms, &me)) return 0;
    int us, ue; if (!mp4_find(b, ms, me, "udta", &us, &ue)) return 0;
    int es, ee; if (!mp4_find(b, us, ue, "meta", &es, &ee)) return 0; es += 4;
    int is, ie; if (!mp4_find(b, es, ee, "ilst", &is, &ie)) return 0;
    int cs, ce; if (!mp4_find(b, is, ie, "covr", &cs, &ce)) return 0;
    int ds, de; if (!mp4_find(b, cs, ce, "data", &ds, &de)) return 0;
    if (de - ds > 8) { *out = b + ds + 8; *outlen = de - (ds + 8); return 1; }
    return 0;
}
// OGG/Opus METADATA_BLOCK_PICTURE (base64 of a FLAC picture block). Best-effort:
// returns a malloc'd decoded buffer (caller frees) with *out into it, or NULL.
static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62; if (c == '/') return 63; return -1;
}
static uint8_t *ogg_pic(const uint8_t *buf, size_t len, const uint8_t **out, int *outlen) {
    static const char *key = "metadata_block_picture="; int kl = 23; size_t found = 0; int got = 0;
    for (size_t i = 0; i + kl < len; i++) {
        int ok = 1; for (int j = 0; j < kl; j++) { char c = (char)buf[i+j]; if (c >= 'A' && c <= 'Z') c += 32; if (c != key[j]) { ok = 0; break; } }
        if (ok) { found = i + kl; got = 1; break; }
    }
    if (!got) return 0;
    size_t e = found; while (e < len && b64v((char)buf[e]) >= 0) e++;
    int nb = (int)(e - found); if (nb < 12) return 0;
    uint8_t *dec = (uint8_t *)malloc((size_t)(nb / 4 * 3 + 4)); if (!dec) return 0;
    int o = 0, acc = 0, bits = 0;
    for (size_t i = found; i < e; i++) { int v = b64v((char)buf[i]); if (v < 0) break; acc = (acc << 6) | v; bits += 6; if (bits >= 8) { bits -= 8; dec[o++] = (uint8_t)(acc >> bits); } }
    if (flac_pic_body(dec, o, out, outlen)) return dec;
    free(dec); return 0;
}

// last-attempt diagnostics (set by load_art_for, read by the boot self-test)
static int g_dbg_r = 0, g_dbg_d0 = 0, g_dbg_d1 = 0, g_dbg_artlen = 0;
static unsigned g_dbg_magic = 0;
static int g_dbg_readlen = 0;

static void load_art_for(const char *path) {
    if (strcmp(path, g_art_path) == 0) return;     // already attempted this file
    mp_cpy(g_art_path, path, sizeof g_art_path);
    g_art_have = 0; g_art_w = g_art_h = 0;
    g_dbg_r = -100; g_dbg_d0 = g_dbg_d1 = 0; g_dbg_artlen = 0; g_dbg_magic = 0;
    // read the whole track so the embedded cover (ID3 APIC / FLAC PICTURE / OGG /
    // mp4 covr) is fully present. The FS read path is erratic for large files
    // (a read can come back short/empty, e.g. under contention with the decode
    // helper), so retry the whole open a few times until we get a substantial
    // read. read_file also rides out transient short reads within one open.
    size_t len = 0; uint8_t *buf = 0;
    for (int att = 0; att < 4; att++) {
        if (buf) free(buf);
        buf = read_file(path, &len);
        if (buf && len > 65536) break;      // got a substantial read
        sys_sleep(30);
    }
    g_dbg_readlen = (int)len;
    if (!buf) { g_dbg_r = -101; return; }
    const uint8_t *art = 0; int artlen = 0; uint8_t *b64buf = 0;
    switch (ext_of(path)) {
        case E_MP3:  find_apic(buf, (int)len, &art, &artlen); break;
        case E_FLAC: find_flac_pic(buf, (int)len, &art, &artlen); break;
        case E_M4A:  find_mp4_cover(buf, (int)len, &art, &artlen); break;
        case E_OGG:
        case E_OPUS: b64buf = ogg_pic(buf, len, &art, &artlen); break;
        default: break;
    }
    g_dbg_artlen = artlen;
    if (art && artlen > 4) g_dbg_magic = ((unsigned)art[0] << 24) | ((unsigned)art[1] << 16) | ((unsigned)art[2] << 8) | art[3];
    if (art && artlen > 16) {
        int dims[2] = {0, 0};
        g_dbg_r = decode_image(art, (unsigned)artlen, ART_SZ, ART_SZ, g_art, sizeof g_art, dims);
        g_dbg_d0 = dims[0]; g_dbg_d1 = dims[1];
        if (g_dbg_r > 0 && dims[0] > 0 && dims[1] > 0 && dims[0] <= ART_SZ && dims[1] <= ART_SZ) {
            g_art_w = dims[0]; g_art_h = dims[1]; g_art_have = 1;
        }
    } else g_dbg_r = -102;   // no embedded art found
    if (b64buf) free(b64buf);
    free(buf);
}

// boot-time art decode self-test: for each path in /CONFIG/ARTTEST.LST, run
// load_art_for and accumulate all results into ONE /CONFIG/ARTDBG.TXT written
// in a single pass (this FS does not honor append/SEEK_END on writes). Used to
// discover which cover-art encodings the kernel image decoder accepts.
static void art_selftest(void) {
    size_t len = 0; uint8_t *buf = read_file("/CONFIG/ARTTEST.LST", &len);
    if (!buf) return;
    static char out[2048]; int pos = 0;
    size_t i = 0; char line[200];
    while (i < len && pos < (int)sizeof out - 200) {
        int k = 0; while (i < len && buf[i] != '\n' && buf[i] != '\r' && k < (int)sizeof line - 1) line[k++] = (char)buf[i++];
        line[k] = 0; while (i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;
        if (k == 0 || line[0] == '#') continue;
        g_art_path[0] = 0;                 // bypass the one-shot cache
        load_art_for(line);
        pos += snprintf(out + pos, sizeof out - pos, "%s read=%d artlen=%d magic=%08x r=%d %dx%d have=%d\n",
                        line, g_dbg_readlen, g_dbg_artlen, g_dbg_magic, g_dbg_r, g_dbg_d0, g_dbg_d1, g_art_have);
    }
    free(buf);
    g_art_path[0] = 0;
    int fd = sys_open("/CONFIG/ARTDBG.TXT", O_WRCREAT);
    if (fd >= 0) { sys_write(fd, out, (unsigned long)pos); sys_close(fd); }
}

// ============================================================================
// PHASE 2c: online metadata + cover-art fetch (MusicBrainz + Cover Art Archive)
// ============================================================================
// Shared response buffer (JSON release lookup, then the cover image bytes). A
// CAA front-250 cover is typically a 250x250 JPEG/PNG well under 256 KB.
#define OF_BUF_SZ   (256 * 1024)
static char g_of_buf[OF_BUF_SZ];

// Disk debug log: GUI apps' stdout (printf) does not reach the serial console,
// so mirror key fetch events to /CONFIG/MP2CLOG.TXT (rewritten whole each time
// because this ext2 does not honour append/SEEK_END writes).
static char g_of_log[2560]; static int g_of_logpos = 0;
static void of_log(const char *s) {
    int n = (int)strlen(s);
    if (g_of_logpos + n + 1 >= (int)sizeof g_of_log) g_of_logpos = 0;
    for (int i = 0; i < n; i++) g_of_log[g_of_logpos++] = s[i];
    g_of_log[g_of_logpos++] = '\n';
    int fd = sys_open("/CONFIG/MP2CLOG.TXT", O_WRCREAT);
    if (fd >= 0) { sys_write(fd, g_of_log, (unsigned long)g_of_logpos); sys_close(fd); }
}

enum { OF_IDLE = 0, OF_MB_WAIT, OF_CAA_WAIT };
static int           g_of_state = OF_IDLE;
static int           g_of_job   = -1;
static unsigned long g_of_t0    = 0;
static char          g_of_key[12]   = "";    // hash of artist+album
static char          g_of_mbid[40]  = "";    // resolved MusicBrainz release id
static char          g_of_artist[48] = "";
static char          g_of_album[48]  = "";
// #342 SAFETY: online cover-art fetch is OFF by default. A slow/stalled HTTPS
// cover fetch can wedge the kernel TCP/IP stack and hang the whole OS (the
// #297/#333 hang class). Embedded ID3/FLAC art (decoded full-res) is the default
// source; the online CoverArtArchive upgrade is opt-in via /CONFIG/MPONLINE.CFG
// containing "1".
static int           g_online_enabled = 0;   // opt-in via /CONFIG/MPONLINE.CFG "1"

// FNV-1a hash of "artist|album" (lowercased) -> 8 hex chars. Stable cache key.
static void of_key(char *out, const char *artist, const char *album) {
    unsigned int h = 2166136261u;
    for (const char *p = artist; *p; p++) { char c = *p; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ (unsigned char)c) * 16777619u; }
    h = (h ^ (unsigned char)'|') * 16777619u;
    for (const char *p = album; *p; p++) { char c = *p; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ (unsigned char)c) * 16777619u; }
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 8; i++) out[i] = hx[(h >> ((7 - i) * 4)) & 15];
    out[8] = 0;
}

// percent-encode everything that is not an unreserved char (spaces -> %20,
// quotes -> %22). Keeps MusicBrainz query strings well-formed.
static void url_enc(char *out, int cap, const char *s) {
    static const char *hx = "0123456789ABCDEF";
    int o = 0;
    for (int i = 0; s[i] && o < cap - 4; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hx[c >> 4]; out[o++] = hx[c & 15]; }
    }
    out[o] = 0;
}

// pull the first MusicBrainz release-group id (UUID) out of the JSON response
static int of_parse_mbid(const char *json, char *mbid) {
    const char *r = strstr(json, "\"release-groups\"");
    const char *p = strstr(r ? r : json, "\"id\":\"");
    if (!p) return 0;
    p += 6;
    int i = 0; for (; i < 36 && p[i] && p[i] != '"'; i++) mbid[i] = p[i];
    mbid[i] = 0;
    if (i != 36) return 0;
    if (mbid[8] != '-' || mbid[13] != '-' || mbid[18] != '-' || mbid[23] != '-') return 0;
    return 1;
}

static void of_save_blob(const char *suffix, const char *key, const void *buf, int n) {
    char p[64]; snprintf(p, sizeof p, "/CONFIG/MPART/%s.%s", key, suffix);
    int fd = sys_open(p, O_WRCREAT); if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)n); sys_close(fd);
}
static void of_mark_notfound(const char *key) { of_save_blob("NF", key, "x", 1); }

// Try the on-disk cache for this key. Returns:
//   1  = cover decoded into g_art (rendered now)
//   2  = negative-cached (looked up before, no cover) -> do not re-fetch
//  -1  = raw cover bytes cached but the decoder rejected them (JPEG pre-#332)
//   0  = nothing cached -> caller should fetch
static int of_load_cache(const char *key) {
    char p[64];
    snprintf(p, sizeof p, "/CONFIG/MPART/%s.NF", key);
    int fd = sys_open(p, 0); if (fd >= 0) { sys_close(fd); return 2; }
    snprintf(p, sizeof p, "/CONFIG/MPART/%s.IMG", key);
    size_t len = 0; uint8_t *buf = read_file(p, &len);
    if (!buf) return 0;
    int dims[2] = {0, 0};
    int r = decode_image(buf, (unsigned)len, ART_SZ, ART_SZ, g_art, sizeof g_art, dims);
    free(buf);
    if (r > 0 && dims[0] > 0 && dims[1] > 0 && dims[0] <= ART_SZ && dims[1] <= ART_SZ) {
        g_art_w = dims[0]; g_art_h = dims[1]; g_art_have = 1; return 1;
    }
    return -1;   // cached, undecodable for now (e.g. JPEG awaiting kernel #332)
}

// Kick off an online cover-art lookup for a track that has no embedded art.
static void online_art_kick(const char *artist, const char *album) {
    if (!g_online_enabled) return;
    if (!artist || !album || !artist[0] || !album[0]) return;
    char key[12]; of_key(key, artist, album);
    // already showing this key's art? nothing to do
    if (g_of_state == OF_IDLE) {
        int c = of_load_cache(key);
        char l[64];
        if (c == 1) { snprintf(l, sizeof l, "cache HIT key=%s (rendered)", key); of_log(l); return; }
        if (c == 2) { snprintf(l, sizeof l, "cache NEG key=%s (no cover)", key); of_log(l); return; }
        if (c == -1) { snprintf(l, sizeof l, "cache RAW key=%s pending #332", key); of_log(l); return; }
    } else {
        // a fetch is already running; if it is for a different track, cancel it
        if (strcmp(key, g_of_key) == 0) return;
        if (g_of_job >= 0) http_fetch_cancel(g_of_job);
        g_of_job = -1; g_of_state = OF_IDLE;
    }
    mp_cpy(g_of_key, key, sizeof g_of_key);
    mp_cpy(g_of_artist, artist, sizeof g_of_artist);
    mp_cpy(g_of_album, album, sizeof g_of_album);
    // Optional manual cover-URL override (/CONFIG/MPARTURL.CFG): fetch this image
    // URL directly and treat the reply as the cover. Lets a user pin a cover and
    // is the sanctioned controlled-fetch path when the MusicBrainz h2 lookup is
    // not serviceable by the current kernel HTTPS client.
    { int fd = sys_open("/CONFIG/MPARTURL.CFG", 0);
      if (fd >= 0) {
        char durl[400]; int dn = (int)sys_read(fd, durl, sizeof durl - 1); sys_close(fd);
        if (dn > 0) {
            durl[dn] = 0; for (int i = 0; i < dn; i++) if (durl[i] == '\n' || durl[i] == '\r') { durl[i] = 0; break; }
            if (durl[0]) {
                g_of_job = http_fetch_start(durl);
                char l[440]; snprintf(l, sizeof l, "DIRECT GET %s job=%d", durl, g_of_job); of_log(l);
                if (g_of_job >= 0) { g_of_state = OF_CAA_WAIT; g_of_t0 = uptime_ms(); return; }
                g_of_state = OF_IDLE; return;
            }
        }
      }
    }
    char ea[160], eb[160], url[480];
    url_enc(ea, sizeof ea, artist);
    url_enc(eb, sizeof eb, album);
    snprintf(url, sizeof url,
        "https://musicbrainz.org/ws/2/release-group/?query=releasegroup:%%22%s%%22%%20AND%%20artist:%%22%s%%22&fmt=json&limit=1",
        eb, ea);
    g_of_job = http_fetch_start(url);
    { char l[300]; snprintf(l, sizeof l, "kick key=%s artist='%s' album='%s' job=%d", key, artist, album, g_of_job); of_log(l); }
    if (g_of_job < 0) { of_log("MB start failed (no socket)"); g_of_state = OF_IDLE; return; }
    g_of_state = OF_MB_WAIT; g_of_t0 = uptime_ms();
    { char l[420]; snprintf(l, sizeof l, "MB GET %s", url); of_log(l); }
}

// Drive the fetch state machine (called once per UI frame). Non-blocking.
static void online_art_poll(void) {
    if (g_of_state == OF_IDLE) return;
    int status = 0; unsigned int len = 0;
    int ps = http_fetch_poll(g_of_job, &status, &len);
    if (ps == 0) {                                   // still running
        if (uptime_ms() - g_of_t0 > 25000) {         // give up on a stalled fetch
            of_log("fetch timeout");
            http_fetch_cancel(g_of_job); g_of_job = -1; g_of_state = OF_IDLE;
        }
        return;
    }
    if (ps < 0) { g_of_job = -1; g_of_state = OF_IDLE; of_log("poll slot gone"); return; }
    // ps == 1 (done) or ps == 2 (error); read frees the slot either way.
    int n = (int)http_fetch_read(g_of_job, g_of_buf, OF_BUF_SZ - 1);
    g_of_job = -1;
    if (g_of_state == OF_MB_WAIT) {
        char l[200]; snprintf(l, sizeof l, "MB done ps=%d status=%d n=%d", ps, status, n); of_log(l);
        if (ps == 1 && status == 200 && n > 0) {
            g_of_buf[n] = 0;
            if (of_parse_mbid(g_of_buf, g_of_mbid)) {
                char line[160];
                int ml = snprintf(line, sizeof line, "%s|%s|%s\n", g_of_artist, g_of_album, g_of_mbid);
                of_save_blob("TXT", g_of_key, line, ml);        // cache resolved metadata
                char url[160];
                snprintf(url, sizeof url, "https://coverartarchive.org/release-group/%s/front-500", g_of_mbid);
                snprintf(l, sizeof l, "mbid=%s -> CAA", g_of_mbid); of_log(l);
                sys_sleep(400);                                 // be gentle between hosts
                g_of_job = http_fetch_start(url);
                if (g_of_job >= 0) { g_of_state = OF_CAA_WAIT; g_of_t0 = uptime_ms(); return; }
                of_log("CAA start failed"); g_of_state = OF_IDLE; return;
            } else {
                of_log("MB no release id (negative-cache)");
                of_mark_notfound(g_of_key);
            }
        } else {
            of_log("MB transient (will retry)");               // network/TLS/no body
        }
        g_of_state = OF_IDLE;
        return;
    }
    if (g_of_state == OF_CAA_WAIT) {
        unsigned magic = (n > 3) ? (((unsigned)(unsigned char)g_of_buf[0] << 24) | ((unsigned)(unsigned char)g_of_buf[1] << 16) |
                          ((unsigned)(unsigned char)g_of_buf[2] << 8) | (unsigned char)g_of_buf[3]) : 0;
        char l[200];
        if (ps == 1 && status == 200 && n > 16) {
            of_save_blob("IMG", g_of_key, g_of_buf, n);         // cache raw bytes
            int dims[2] = {0, 0};
            int r = decode_image(g_of_buf, (unsigned)n, ART_SZ, ART_SZ, g_art, sizeof g_art, dims);
            if (r > 0 && dims[0] > 0 && dims[1] > 0 && dims[0] <= ART_SZ && dims[1] <= ART_SZ) {
                g_art_w = dims[0]; g_art_h = dims[1]; g_art_have = 1;
                snprintf(l, sizeof l, "CAA status=200 bytes=%d magic=%08x decoded %dx%d OK", n, magic, dims[0], dims[1]);
            } else {
                // raw bytes are cached; will render once kernel decoder supports
                // this format (JPEG -> #332). Keep the placeholder for now.
                snprintf(l, sizeof l, "CAA status=200 bytes=%d magic=%08x decode=%d PENDING(cached)", n, magic, r);
            }
            of_log(l);
        } else if (ps == 1 && status == 404) {
            of_log("CAA 404 no cover (negative-cache)");
            of_mark_notfound(g_of_key);
        } else {
            snprintf(l, sizeof l, "CAA transient ps=%d status=%d n=%d (will retry)", ps, status, n); of_log(l);
        }
        g_of_state = OF_IDLE;
        return;
    }
}

// draw the album-art thumbnail (or a placeholder note) into g_fb
static void mp_draw_art(int bx, int by, int box) {
    fb_rect(bx - 1, by - 1, box + 2, box + 2, 0x000000);
    if (g_art_have && g_art_w > 0 && g_art_h > 0) {
        // #335: scale the decoded cover to fill `box`, preserving aspect ratio.
        int dw = box, dh = box;
        if (g_art_w >= g_art_h) dh = box * g_art_h / g_art_w;
        else                    dw = box * g_art_w / g_art_h;
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;
        int ox = bx + (box - dw) / 2, oy = by + (box - dh) / 2;
        for (int y = 0; y < dh; y++) {
            int sy = y * g_art_h / dh; if (sy >= g_art_h) sy = g_art_h - 1;
            for (int x = 0; x < dw; x++) {
                int sx = x * g_art_w / dw; if (sx >= g_art_w) sx = g_art_w - 1;
                px(ox + x, oy + y, 0xFF000000u | (g_art[sy * g_art_w + sx] & 0xFFFFFF));
            }
        }
    } else {
        fb_rect(bx, by, box, box, 0x2A3038);
        fb_bevel(bx, by, box, box, 1);
        uint32_t c = 0x90A0B0;            // musical-note placeholder
        int nw = box / 16; if (nw < 2) nw = 2;
        fb_rect(bx + box / 2 + box / 8, by + box / 6, nw, box * 2 / 3, c);
        fb_rect(bx + box / 2 - box / 5, by + box - box / 4, box / 3, box / 8 < 2 ? 2 : box / 8, c);
        fb_rect(bx + box / 2 + box / 8, by + box / 6, box / 5, nw, c);
    }
}

// ============================================================================
// Music library (recursive scan, persisted index, browse-by-artist/album view)
// ============================================================================
#define MAX_LIB   384
static track_t g_lib[MAX_LIB];
static int     g_nlib = 0;
static track_t g_tmp_tr;
static int     g_rows_type[MAX_LIB * 2];   // 0 artist hdr, 1 album hdr, 2 track
static int     g_rows_idx[MAX_LIB * 2];
static int     g_nrows = 0;
static int     g_lib_sel = 0, g_lib_scroll = 0;

static void lib_scan_dir(const char *dir, int depth) {
    if (depth > 4 || g_nlib >= MAX_LIB) return;
    dirent_t e;
    for (int i = 0; g_nlib < MAX_LIB; i++) {
        if (sys_readdir(dir, i, &e) < 0) break;
        if (e.name[0] == '.') continue;
        char child[256]; build_path(child, dir, e.name);
        if (e.type == 1) { lib_scan_dir(child, depth + 1); continue; }
        if (!is_audio(e.name)) continue;
        track_t *t = &g_lib[g_nlib];
        mp_zero(t, (int)sizeof *t);
        mp_cpy(t->name, e.name, NAME_LEN);
        mp_cpy(t->path, child, (int)sizeof t->path);
        parse_metadata(child, t);
        g_nlib++;
    }
}
static int ci_cmp(const char *a, const char *b) {
    while (*a && *b) { char ca = *a, cb = *b; if (ca>='A'&&ca<='Z')ca+=32; if (cb>='A'&&cb<='Z')cb+=32; if (ca != cb) return (int)ca - (int)cb; a++; b++; }
    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}
static int lib_less(const track_t *x, const track_t *y) {
    int c = ci_cmp(x->artist[0] ? x->artist : "~~", y->artist[0] ? y->artist : "~~"); if (c) return c < 0;
    c = ci_cmp(x->album[0] ? x->album : "~~", y->album[0] ? y->album : "~~"); if (c) return c < 0;
    if (x->track_no != y->track_no) return x->track_no < y->track_no;
    return ci_cmp(x->title[0] ? x->title : x->name, y->title[0] ? y->title : y->name) < 0;
}
static void lib_sort(void) {
    for (int i = 0; i < g_nlib - 1; i++) {
        int m = i;
        for (int j = i + 1; j < g_nlib; j++) if (lib_less(&g_lib[j], &g_lib[m])) m = j;
        if (m != i) { g_tmp_tr = g_lib[i]; g_lib[i] = g_lib[m]; g_lib[m] = g_tmp_tr; }
    }
}
static void lib_build_rows(void) {
    g_nrows = 0; char curart[48] = "", curalb[48] = "";
    for (int i = 0; i < g_nlib && g_nrows < MAX_LIB * 2 - 2; i++) {
        const char *ar = g_lib[i].artist[0] ? g_lib[i].artist : "Unknown Artist";
        const char *al = g_lib[i].album[0]  ? g_lib[i].album  : "Unknown Album";
        if (!ci_eq(ar, curart)) { g_rows_type[g_nrows] = 0; g_rows_idx[g_nrows] = i; g_nrows++; mp_cpy(curart, ar, sizeof curart); curalb[0] = 0; }
        if (!ci_eq(al, curalb)) { g_rows_type[g_nrows] = 1; g_rows_idx[g_nrows] = i; g_nrows++; mp_cpy(curalb, al, sizeof curalb); }
        g_rows_type[g_nrows] = 2; g_rows_idx[g_nrows] = i; g_nrows++;
    }
    if (g_lib_sel >= g_nrows) g_lib_sel = g_nrows - 1;
    if (g_lib_sel < 0) g_lib_sel = 0;
}
static void lib_save(void) {
    int fd = sys_open("/CONFIG/MPLIBRARY.DAT", O_WRCREAT); if (fd < 0) return;
    char line[480];
    for (int i = 0; i < g_nlib; i++) {
        track_t *t = &g_lib[i];
        int n = snprintf(line, sizeof line, "%s|%s|%s|%s|%s|%d|%d|%d\n",
                         t->path, t->title, t->artist, t->album, t->genre, t->year, t->track_no, t->duration);
        sys_write(fd, line, (unsigned long)n);
    }
    sys_close(fd);
}
static int lib_load(void) {
    size_t len = 0; uint8_t *buf = read_file("/CONFIG/MPLIBRARY.DAT", &len);
    if (!buf) return 0;
    g_nlib = 0; size_t i = 0; char line[480];
    while (i < len && g_nlib < MAX_LIB) {
        int k = 0; while (i < len && buf[i] != '\n' && buf[i] != '\r' && k < (int)sizeof line - 1) line[k++] = (char)buf[i++];
        line[k] = 0; while (i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;
        if (k == 0) continue;
        char *f[8]; int nf = 0; f[nf++] = line;
        for (int j = 0; line[j] && nf < 8; j++) if (line[j] == '|') { line[j] = 0; f[nf++] = &line[j + 1]; }
        if (nf < 8) continue;
        track_t *t = &g_lib[g_nlib]; mp_zero(t, (int)sizeof *t);
        mp_cpy(t->path, f[0], sizeof t->path); mp_cpy(t->title, f[1], sizeof t->title);
        mp_cpy(t->artist, f[2], sizeof t->artist); mp_cpy(t->album, f[3], sizeof t->album);
        mp_cpy(t->genre, f[4], sizeof t->genre);
        t->year = atoi_s(f[5]); t->track_no = atoi_s(f[6]); t->duration = atoi_s(f[7]);
        const char *b = t->path; for (const char *p = t->path; *p; p++) if (*p == '/') b = p + 1;
        mp_cpy(t->name, b, NAME_LEN);
        t->has_meta = (t->title[0] || t->artist[0] || t->album[0]);
        g_nlib++;
    }
    free(buf);
    return g_nlib;
}
static void lib_load_dirs(char dirs[][128], int *ndir) {
    *ndir = 0;
    int fd = sys_open("/CONFIG/MPLIB.CFG", 0);
    if (fd >= 0) {
        char b[512]; int n = (int)sys_read(fd, b, sizeof b - 1); sys_close(fd);
        if (n > 0) { b[n] = 0; int i = 0;
            while (i < n && *ndir < 8) {
                int k = 0; char line[128];
                while (i < n && b[i] != '\n' && b[i] != '\r' && k < 127) line[k++] = b[i++];
                line[k] = 0; while (i < n && (b[i] == '\n' || b[i] == '\r')) i++;
                if (line[0] && line[0] != '#') { mp_cpy(dirs[*ndir], line, 128); (*ndir)++; }
            }
        }
    }
    if (*ndir == 0) { mp_cpy(dirs[0], "/HOME", 128); mp_cpy(dirs[1], "/HOME/MUSIC", 128); *ndir = 2; }
}
static void lib_rescan(void) {
    g_nlib = 0;
    char dirs[8][128]; int nd; lib_load_dirs(dirs, &nd);
    for (int i = 0; i < nd; i++) lib_scan_dir(dirs[i], 0);
    lib_sort(); lib_build_rows(); lib_save();
}
static void lib_play_row(int r) {
    if (r < 0 || r >= g_nrows || g_rows_type[r] != 2) return;
    int idx = g_rows_idx[r];
    int pi = -1;
    for (int i = 0; i < g_ntracks; i++) if (strcmp(g_tracks[i].path, g_lib[idx].path) == 0) { pi = i; break; }
    if (pi < 0 && g_ntracks < MAX_TRACKS) { g_tracks[g_ntracks] = g_lib[idx]; pi = g_ntracks; g_ntracks++; }
    if (pi >= 0) { lib_close(); play_index(pi); }
}

// library browse view: compose chrome + overlay text rows
#define LIB_TOP   22
#define LIB_ROWH  11
static void compose_library(void) {
    fb_rect(0, 0, W, H, 0x10141A);
    fb_grad_v(0, 0, W, 18, 0x39516E, 0x1C2A3C);
    fb_bevel(0, 0, W, H, 1);
    int vis = (H - LIB_TOP - 4) / LIB_ROWH;
    for (int i = 0; i < vis; i++) {
        int r = g_lib_scroll + i; if (r >= g_nrows) break;
        if (r == g_lib_sel) fb_rect(2, LIB_TOP + i * LIB_ROWH, W - 4, LIB_ROWH, 0x2C4A6E);
        else if (g_rows_type[r] == 0) fb_rect(2, LIB_TOP + i * LIB_ROWH, W - 4, LIB_ROWH, 0x202830);
    }
}
static void overlay_library(void) {
    char buf[128];
    g_ov_win = lib_win; g_ov_dy = 0;   // #342 library is its own window
    snprintf(buf, sizeof buf, "MUSIC LIBRARY  %d TRACKS   V=BACK  R=RESCAN  ENTER=PLAY", g_nlib);
    OT(6, 4, buf, 0xC8D6E6);
    int vis = (H - LIB_TOP - 4) / LIB_ROWH;
    for (int i = 0; i < vis; i++) {
        int r = g_lib_scroll + i; if (r >= g_nrows) break;
        int y = LIB_TOP + i * LIB_ROWH + 2, idx = g_rows_idx[r];
        if (g_rows_type[r] == 0) {
            snprintf(buf, sizeof buf, "%s", g_lib[idx].artist[0] ? g_lib[idx].artist : "Unknown Artist");
            OT(6, y, buf, 0x6EE0FF);
        } else if (g_rows_type[r] == 1) {
            snprintf(buf, sizeof buf, "  %s", g_lib[idx].album[0] ? g_lib[idx].album : "Unknown Album");
            OT(6, y, buf, 0xE0C060);
        } else {
            track_t *t = &g_lib[idx]; char d[8];
            if (t->duration > 0) snprintf(d, sizeof d, "%d:%02d", t->duration / 60, t->duration % 60);
            else mp_cpy(d, "--:--", sizeof d);
            const char *ti = t->title[0] ? t->title : t->name;
            if (t->track_no > 0) snprintf(buf, sizeof buf, "    %2d. %s  %s", t->track_no, ti, d);
            else                 snprintf(buf, sizeof buf, "    %s  %s", ti, d);
            OT(6, y, buf, (r == g_lib_sel) ? 0xFFFFFF : 0xB6C2CF);
        }
    }
}
static void lib_scroll_to_sel(void) {
    int vis = (H - LIB_TOP - 4) / LIB_ROWH;
    if (g_lib_sel < g_lib_scroll) g_lib_scroll = g_lib_sel;
    if (g_lib_sel >= g_lib_scroll + vis) g_lib_scroll = g_lib_sel - vis + 1;
    if (g_lib_scroll < 0) g_lib_scroll = 0;
}
static void lib_click(int mx, int my) {
    (void)mx;
    if (my < 18) { lib_close(); return; }   // header bar -> back to rack
    int r = g_lib_scroll + (my - LIB_TOP) / LIB_ROWH;
    if (r < 0 || r >= g_nrows) return;
    if (r == g_lib_sel && g_rows_type[r] == 2) lib_play_row(r);
    else { g_lib_sel = r; lib_scroll_to_sel(); }
}

// --- decode a BMP buffer (1/4/8/24/32-bit, uncompressed) to ARGB -----------
static int decode_bmp(const uint8_t *d, size_t len, sprite_t *out) {
    if (len < 54 || d[0] != 'B' || d[1] != 'M') return -1;
    uint32_t off  = rd32(d + 10);
    uint32_t hsz  = rd32(d + 14);
    int32_t  w    = (int32_t)rd32(d + 18);
    int32_t  hraw = (int32_t)rd32(d + 22);
    uint16_t bpp  = rd16(d + 28);
    uint32_t comp = rd32(d + 30);
    if (comp != 0) return -1;
    int bottom_up = hraw > 0;
    int h = hraw < 0 ? -hraw : hraw;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
    int rowsz = ((w * bpp + 31) / 32) * 4;
    if ((size_t)off + (size_t)rowsz * h > len) return -1;
    const uint8_t *pal = d + 14 + hsz;     // palette (BGRA quads) for <=8bpp
    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!px) return -1;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = d + off + (size_t)y * rowsz;
        int dy = bottom_up ? (h - 1 - y) : y;
        uint32_t *o = px + (size_t)dy * w;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (bpp == 24)      { const uint8_t *p = row + x * 3; c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
            else if (bpp == 32) { const uint8_t *p = row + x * 4; c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
            else if (bpp == 8)  { const uint8_t *p = pal + (size_t)row[x] * 4; c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
            else if (bpp == 4)  { int idx = (row[x / 2] >> ((x & 1) ? 0 : 4)) & 0xF; const uint8_t *p = pal + (size_t)idx * 4; c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
            else if (bpp == 1)  { int idx = (row[x / 8] >> (7 - (x & 7))) & 1; const uint8_t *p = pal + (size_t)idx * 4; c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
            else c = 0;
            o[x] = 0xFF000000u | c;
        }
    }
    out->px = px; out->w = w; out->h = h;
    return 0;
}

// --- sprite blit (opaque copy of a sub-rect into g_fb) ---------------------
static void blit_sprite(const sprite_t *s, int sx, int sy, int sw, int sh, int dx, int dy) {
    if (!s->px) return;
    for (int j = 0; j < sh; j++) {
        int yy = sy + j, ty = dy + j;
        if (yy < 0 || yy >= s->h || ty < 0 || ty >= H) continue;
        const uint32_t *srow = s->px + (size_t)yy * s->w;
        uint32_t *drow = g_fb + (size_t)ty * W;
        for (int i = 0; i < sw; i++) {
            int xx = sx + i, tx = dx + i;
            if (xx < 0 || xx >= s->w || tx < 0 || tx >= W) continue;
            drow[tx] = srow[xx];
        }
    }
}
static void blit_full(const sprite_t *s, int dx, int dy) { if (s->px) blit_sprite(s, 0, 0, s->w, s->h, dx, dy); }

// --- text.bmp bitmap font (5x6 cells, 3 rows) ------------------------------
static int text_char_src(char c, int *sx, int *sy) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') { *sx = (c - 'A') * 5; *sy = 0;  return 1; }
    if (c >= '0' && c <= '9') { *sx = (c - '0') * 5; *sy = 6;  return 1; }
    int col;
    switch (c) {
        case '.': col = 10; break; case ':': col = 11; break; case '(': col = 12; break;
        case ')': col = 13; break; case '-': col = 14; break; case '\'': col = 15; break;
        case '!': col = 16; break; case '_': col = 17; break; case '+': col = 18; break;
        case '/': col = 20; break; case '[': col = 21; break; case ']': col = 22; break;
        case '&': col = 24; break; case '%': col = 25; break; case ',': col = 26; break;
        case '=': col = 27; break; case '$': col = 28; break; case '#': col = 29; break;
        default: return 0;   // space / unknown -> blank cell, just advance
    }
    *sx = col * 5; *sy = 6;
    return 1;
}
static void draw_text_bmp(int dx, int dy, const char *s, int maxchars) {
    if (!g_bskin.text.px) return;
    int x = dx, n = 0;
    for (const char *p = s; *p && n < maxchars; p++, n++) {
        int sx, sy;
        if (text_char_src(*p, &sx, &sy)) blit_sprite(&g_bskin.text, sx, sy, 5, 6, x, dy);
        x += 5;
    }
}

// --- numbers.bmp LCD digit (9x13) ------------------------------------------
static void draw_digit_bmp(int v, int dx, int dy) {
    if (!g_bskin.numbers.px || v < 0 || v > 9) return;
    blit_sprite(&g_bskin.numbers, v * 9, 0, 9, 13, dx, dy);
}

// --- free / load -----------------------------------------------------------
static void free_sprite(sprite_t *s) { if (s->px) free(s->px); s->px = 0; s->w = s->h = 0; }
static void free_bskin(void) {
    free_sprite(&g_bskin.main);     free_sprite(&g_bskin.titlebar); free_sprite(&g_bskin.cbuttons);
    free_sprite(&g_bskin.numbers);  free_sprite(&g_bskin.text);     free_sprite(&g_bskin.volume);
    free_sprite(&g_bskin.balance);  free_sprite(&g_bskin.posbar);   free_sprite(&g_bskin.monoster);
    free_sprite(&g_bskin.playpaus); free_sprite(&g_bskin.shufrep);  free_sprite(&g_bskin.eqmain);
    free_sprite(&g_bskin.pledit);
    g_bskin.loaded = 0;
}

static const arc_entry *find_entry(arc_entry *e, int n, const char *want) {
    for (int i = 0; i < n; i++) {
        if (e[i].is_dir || !e[i].data) continue;
        const char *b = e[i].name;
        for (const char *p = e[i].name; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
        if (ci_eq(b, want)) return &e[i];
    }
    return 0;
}
static int load_one(arc_entry *e, int n, const char *want, sprite_t *s) {
    const arc_entry *x = find_entry(e, n, want);
    if (!x) return -1;
    return decode_bmp(x->data, x->size, s);
}

// parse a hex colour after a '#': #RRGGBB
static uint32_t parse_hex_after(const char *p) {
    uint32_t v = 0; int got = 0;
    while (*p && got < 6) {
        char c = *p++;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d; got++;
    }
    return v;
}
static void parse_pledit(const uint8_t *d, size_t len) {
    // line-oriented: Normal=#..  Current=#..  NormalBG=#..  SelectedBG=#..
    char line[96];
    size_t i = 0;
    while (i < len) {
        int k = 0;
        while (i < len && d[i] != '\n' && d[i] != '\r' && k < (int)sizeof(line) - 1) line[k++] = (char)d[i++];
        line[k] = 0;
        while (i < len && (d[i] == '\n' || d[i] == '\r')) i++;
        const char *h = 0;
        for (int j = 0; line[j]; j++) if (line[j] == '#') { h = &line[j + 1]; break; }
        if (!h) continue;
        if      (strncmp(line, "Normal=", 7) == 0)     g_bskin.pl_normal   = parse_hex_after(h);
        else if (strncmp(line, "Current=", 8) == 0)    g_bskin.pl_current  = parse_hex_after(h);
        else if (strncmp(line, "NormalBG=", 9) == 0)   g_bskin.pl_normalbg = parse_hex_after(h);
        else if (strncmp(line, "SelectedBG=", 11) == 0) g_bskin.pl_selectbg = parse_hex_after(h);
    }
}

static int load_wsz(const char *path) {
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf) return -1;
    int cnt = 0;
    arc_entry *e = arc_zip_extract(buf, len, &cnt);
    free(buf);
    if (!e || cnt <= 0) { if (e) arc_free_entries(e, cnt); return -1; }

    free_bskin();
    int ok = (load_one(e, cnt, "main.bmp", &g_bskin.main) == 0);
    load_one(e, cnt, "titlebar.bmp", &g_bskin.titlebar);
    load_one(e, cnt, "cbuttons.bmp", &g_bskin.cbuttons);
    if (load_one(e, cnt, "nums_ex.bmp", &g_bskin.numbers) == 0) g_bskin.nums_ex = 1;
    else { g_bskin.nums_ex = 0; load_one(e, cnt, "numbers.bmp", &g_bskin.numbers); }
    load_one(e, cnt, "text.bmp", &g_bskin.text);
    load_one(e, cnt, "volume.bmp", &g_bskin.volume);
    load_one(e, cnt, "balance.bmp", &g_bskin.balance);
    load_one(e, cnt, "posbar.bmp", &g_bskin.posbar);
    load_one(e, cnt, "monoster.bmp", &g_bskin.monoster);
    load_one(e, cnt, "playpaus.bmp", &g_bskin.playpaus);
    load_one(e, cnt, "shufrep.bmp", &g_bskin.shufrep);
    load_one(e, cnt, "eqmain.bmp", &g_bskin.eqmain);
    load_one(e, cnt, "pledit.bmp", &g_bskin.pledit);

    // playlist colour defaults + PLEDIT.TXT overrides
    g_bskin.pl_normal = 0x00E000; g_bskin.pl_current = 0xFFFFFF;
    g_bskin.pl_normalbg = 0x101010; g_bskin.pl_selectbg = 0x0030A0;
    const arc_entry *pt = find_entry(e, cnt, "pledit.txt");
    if (pt && pt->data) parse_pledit(pt->data, pt->size);

    arc_free_entries(e, cnt);
    if (!ok) { free_bskin(); return -1; }

    // rack background colour = sampled from the main window
    g_bskin.rack_bg = (g_bskin.main.px ? (g_bskin.main.px[(g_bskin.main.h - 6) * g_bskin.main.w + 5] & 0xFFFFFF) : 0x101010);

    // display name = file basename without extension
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    int j = 0; while (b[j] && b[j] != '.' && j < 63) { g_bskin.name[j] = b[j]; j++; }
    g_bskin.name[j] = 0;

    g_bskin.loaded = 1;
    return 0;
}

// --- discover .wsz files ---------------------------------------------------
static int is_wsz(const char *n) {
    int L = (int)strlen(n);
    if (L < 5 || n[L - 4] != '.') return 0;
    return (n[L - 3] == 'w' || n[L - 3] == 'W') &&
           (n[L - 2] == 's' || n[L - 2] == 'S') &&
           (n[L - 1] == 'z' || n[L - 1] == 'Z');
}
static void add_wsz_dir(const char *dir) {
    dirent_t e;
    for (int i = 0; g_nwsz < MAX_WSZ; i++) {
        if (sys_readdir(dir, i, &e) < 0) break;
        if (e.type == 1) continue;
        if (!is_wsz(e.name)) continue;
        // de-dup by basename so the same skin in two dirs is not listed twice
        int dup = 0;
        for (int k = 0; k < g_nwsz; k++) {
            const char *b = g_wsz[k]; for (const char *p = g_wsz[k]; *p; p++) if (*p == '/') b = p + 1;
            if (ci_eq(b, e.name)) { dup = 1; break; }
        }
        if (dup) continue;
        build_path(g_wsz[g_nwsz], dir, e.name);
        g_nwsz++;
    }
}
static void scan_skins(void) {
    g_nwsz = 0;
    add_wsz_dir("/SKINS");
    add_wsz_dir("/HOME/SKINS");
    add_wsz_dir("/HOME");
}

// --- persistence: /CONFIG/MPSKIN.CFG --------------------------------------
static void save_skin_cfg(void) {
    int fd = sys_open("/CONFIG/MPSKIN.CFG", O_WRCREAT);
    if (fd < 0) return;
    char buf[176];
    int n;
    if (g_bitmap_active) n = snprintf(buf, sizeof(buf), "%s\n", g_wsz[g_sel_skin - NUM_SKINS]);
    else                 n = snprintf(buf, sizeof(buf), "*%d\n", g_skin);
    sys_write(fd, buf, (unsigned long)n);
    sys_close(fd);
}

// --- apply a unified skin index (builtin 0..NUM_SKINS-1, then wsz) ---------
static void apply_skin(int idx) {
    int total = NUM_SKINS + g_nwsz;
    if (total <= 0) return;
    idx = ((idx % total) + total) % total;
    if (idx < NUM_SKINS) {
        g_skin = idx;
        g_bitmap_active = 0;
        g_sel_skin = idx;
    } else {
        if (load_wsz(g_wsz[idx - NUM_SKINS]) == 0) {
            g_bitmap_active = 1;
            g_sel_skin = idx;
        } else {
            // failed: fall back to the first builtin
            g_skin = 0; g_bitmap_active = 0; g_sel_skin = 0;
        }
    }
    save_skin_cfg();
}
static void cycle_skin(int d) { apply_skin(g_sel_skin + d); }

// Minimise (iconify) our own window to the taskbar. win_create() returns a
// slot HANDLE, but SYS_WM_MINIMIZE_WINDOW matches on the window-manager id
// (win->id), a different namespace. So resolve our wm id via wm_get_windows()
// by matching the window title, then minimise that id.
static void do_minimize(void) {
    wm_window_info_t wins[32];
    int n = wm_get_windows(wins, 32);
    // Prefer our window by title.
    for (int i = 0; i < n; i++)
        if (strcmp(wins[i].title, "Maytera HiFi") == 0) { wm_minimize(wins[i].id); return; }
    // Fallback: the focused window (that's us right after the button click).
    for (int i = 0; i < n; i++)
        if (wins[i].focused) { wm_minimize(wins[i].id); return; }
}

// #165 per-app transparency: a title-strip button cycles the window opacity
// 100% -> 85% -> 70% -> 100%. NOTE: SYS_SET_WIN_OPACITY(233) is the GLOBAL
// default opacity in this kernel (there is no per-window setter syscall), so it
// dims all windows; it is exposed here on the HiFi as the user requested. The
// kernel clamps opacity to a 40 floor, so 70%/85%/100% are all safe.
// (g_opa_levels[] / g_opa_idx are declared up with the player state.)
static void cycle_opacity(void) {
    g_opa_idx = (g_opa_idx + 1) % 3;
    set_win_opacity(g_opa_levels[g_opa_idx]);
}

// ----------------------------------------------------------------------------
// #342 multi-window rig: EQ / PLAYLIST / ALBUM ART sub-windows + toggles
// ----------------------------------------------------------------------------
// Begin dragging the given window (mirrors the main-window #334 drag): record the
// cursor-to-origin offset so the grabbed point stays glued to the pointer.
static void start_drag(int handle) {
    if (handle < 0) return;
    int gx = 0, gy = 0; unsigned int gb = 0; get_global_mouse(&gx, &gy, &gb);
    int wx = 0, wy = 0; win_get_pos(handle, &wx, &wy);
    g_grab_dx = gx - wx; g_grab_dy = gy - wy; g_drag_win = handle;
}

static void panel_open_eq(void) {
    if (eq_win >= 0) return;
    int mx = 0, my = 0; win_get_pos(win, &mx, &my);
    eq_win = win_create("HiFi Equalizer", mx, my + MOD_MAIN_H, W, MOD_EQ_H);
    if (eq_win >= 0) win_set_nochrome(eq_win);
}
static void panel_close_eq(void) {
    if (eq_win >= 0) { if (g_drag_win == eq_win) g_drag_win = -1; win_destroy(eq_win); eq_win = -1; }
}
static void panel_open_pl(void) {
    if (pl_win >= 0) return;
    int mx = 0, my = 0; win_get_pos(win, &mx, &my);
    int py = my + MOD_MAIN_H + (eq_win >= 0 ? MOD_EQ_H : 0);
    pl_win = win_create("HiFi Playlist", mx, py, W, MOD_PL_H);
    if (pl_win >= 0) win_set_nochrome(pl_win);
}
static void panel_close_pl(void) {
    if (pl_win >= 0) { if (g_drag_win == pl_win) g_drag_win = -1; win_destroy(pl_win); pl_win = -1; }
}
static void panel_open_art(void) {
    if (art_win >= 0) return;
    int mx = 0, my = 0; win_get_pos(win, &mx, &my);
    art_win = win_create("Album Art", mx + W + 8, my, ART_WIN_W, ART_WIN_H);
    if (art_win >= 0) win_set_nochrome(art_win);
}
static void panel_close_art(void) {
    if (art_win >= 0) { if (g_drag_win == art_win) g_drag_win = -1; win_destroy(art_win); art_win = -1; }
}
static void panel_toggle_eq(void)  { if (eq_win  >= 0) panel_close_eq();  else panel_open_eq();  }
static void panel_toggle_pl(void)  { if (pl_win  >= 0) panel_close_pl();  else panel_open_pl();  }
static void panel_toggle_art(void) { if (art_win >= 0) panel_close_art(); else panel_open_art(); }

// A click in the main-window title strip: was it on one of the four toggle chips?
static int panel_toggle_hit(int mx, int my) {
    if (my < TGL_Y || my >= TGL_Y + TGL_H) return 0;
    for (int i = 0; i < 4; i++) {
        int x = TGL_X0 + i * TGL_PITCH;
        if (mx >= x && mx < x + TGL_W) {
            if      (i == 0) panel_toggle_eq();
            else if (i == 1) panel_toggle_pl();
            else if (i == 2) viz_toggle();
            else             panel_toggle_art();
            return 1;
        }
    }
    return 0;
}

// --- album-art window: render the decoded cover (or a placeholder) ----------
static void artfb_fill(int x, int y, int w, int h, uint32_t rgb) {
    uint32_t c = 0xFF000000u | (rgb & 0xFFFFFF);
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < 0 || yy >= ART_WIN_H) continue;
        uint32_t *row = g_artfb + (size_t)yy * ART_WIN_W;
        for (int i = 0; i < w; i++) { int xx = x + i; if (xx < 0 || xx >= ART_WIN_W) continue; row[xx] = c; }
    }
}
// bilinear blend of four ARGB source texels; fx/fy are 0..256 fractions.
static inline uint32_t art_bilerp(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, int fx, int fy) {
    int r0 = ((c00 >> 16) & 255) + ((((int)((c10 >> 16) & 255)) - ((int)((c00 >> 16) & 255))) * fx >> 8);
    int r1 = ((c01 >> 16) & 255) + ((((int)((c11 >> 16) & 255)) - ((int)((c01 >> 16) & 255))) * fx >> 8);
    int rr = r0 + ((r1 - r0) * fy >> 8);
    int g0 = ((c00 >> 8) & 255) + ((((int)((c10 >> 8) & 255)) - ((int)((c00 >> 8) & 255))) * fx >> 8);
    int g1 = ((c01 >> 8) & 255) + ((((int)((c11 >> 8) & 255)) - ((int)((c01 >> 8) & 255))) * fx >> 8);
    int gg = g0 + ((g1 - g0) * fy >> 8);
    int b0 = (c00 & 255) + ((((int)(c10 & 255)) - ((int)(c00 & 255))) * fx >> 8);
    int b1 = (c01 & 255) + ((((int)(c11 & 255)) - ((int)(c01 & 255))) * fx >> 8);
    int bb = b0 + ((b1 - b0) * fy >> 8);
    return 0xFF000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
}
static void compose_art(void) {
    artfb_fill(0, 0, ART_WIN_W, ART_WIN_H, 0x0D1014);
    artfb_fill(0, 0, ART_WIN_W, ART_WIN_TH, 0x39516E);   // title strip (drag handle)
    int ay = ART_WIN_TH, box = ART_WIN_W;
    if (g_art_have && g_art_w > 0 && g_art_h > 0) {
        int dw = box, dh = box;
        if (g_art_w >= g_art_h) dh = box * g_art_h / g_art_w;
        else                    dw = box * g_art_w / g_art_h;
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;
        int ox = (box - dw) / 2, oy = ay + (box - dh) / 2;
        artfb_fill(0, ay, box, box, 0x000000);
        // #342: BILINEAR downscale of the high-res cover -> smooth, not blocky.
        for (int y = 0; y < dh; y++) {
            int syq = (dh > 1) ? (y * (g_art_h - 1) * 256 / (dh - 1)) : 0;
            int sy0 = syq >> 8, fy = syq & 255; int sy1 = sy0 + 1; if (sy1 >= g_art_h) sy1 = g_art_h - 1;
            const uint32_t *r0 = g_art + (size_t)sy0 * g_art_w;
            const uint32_t *r1 = g_art + (size_t)sy1 * g_art_w;
            int yy = oy + y; if (yy < 0 || yy >= ART_WIN_H) continue;
            uint32_t *drow = g_artfb + (size_t)yy * ART_WIN_W;
            for (int x = 0; x < dw; x++) {
                int sxq = (dw > 1) ? (x * (g_art_w - 1) * 256 / (dw - 1)) : 0;
                int sx0 = sxq >> 8, fx = sxq & 255; int sx1 = sx0 + 1; if (sx1 >= g_art_w) sx1 = g_art_w - 1;
                int xx = ox + x; if (xx < 0 || xx >= ART_WIN_W) continue;
                drow[xx] = art_bilerp(r0[sx0], r0[sx1], r1[sx0], r1[sx1], fx, fy);
            }
        }
    } else {
        // no cover: musical-note placeholder on a muted panel
        artfb_fill(0, ay, box, box, 0x20262E);
        artfb_fill(box / 2 + 6, ay + 34, 9, box - 78, 0x8CA0B4);
        artfb_fill(box / 2 - 34, ay + box - 52, 49, 15, 0x8CA0B4);
        artfb_fill(box / 2 + 6, ay + 34, box / 5, 9, 0x8CA0B4);
    }
}

// --- music-library browser window (g_view == 1) ----------------------------
static void lib_open(void) {
    if (lib_win < 0) {
        int mx = 0, my = 0; win_get_pos(win, &mx, &my);
        lib_win = win_create("Music Library", mx, my, W, H);
        if (lib_win >= 0) win_set_nochrome(lib_win);
    }
    g_view = 1;
}
static void lib_close(void) {
    if (lib_win >= 0) { if (g_drag_win == lib_win) g_drag_win = -1; win_destroy(lib_win); lib_win = -1; }
    g_view = 0;
}

// Tear down every sub-window (used by the main [X] close: closing the HiFi
// closes the art / EQ / playlist / viz / library windows too).
static void close_all_windows(void) {
    viz_close();
    panel_close_eq(); panel_close_pl(); panel_close_art();
    if (lib_win >= 0) { win_destroy(lib_win); lib_win = -1; }
}

static void load_skin_cfg(void) {
    int fd = sys_open("/CONFIG/MPSKIN.CFG", 0);
    if (fd < 0) return;
    char buf[176];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (int i = 0; buf[i]; i++) if (buf[i] == '\n' || buf[i] == '\r') { buf[i] = 0; break; }
    if (buf[0] == '*') { apply_skin(buf[1] - '0'); return; }
    // match a path against discovered wsz, else try to load directly
    for (int i = 0; i < g_nwsz; i++)
        if (strcmp(g_wsz[i], buf) == 0) { apply_skin(NUM_SKINS + i); return; }
    if (load_wsz(buf) == 0) { g_bitmap_active = 1; g_sel_skin = NUM_SKINS; }
}

// --- playlist save / load (M3U / PLS) --------------------------------------
#define PLAYLIST_FILE "/HOME/PLAYLIST.M3U"
static void playlist_save(void) {
    int fd = sys_open(PLAYLIST_FILE, O_WRCREAT);
    if (fd < 0) return;
    const char *hdr = "#EXTM3U\n";
    sys_write(fd, hdr, (unsigned long)strlen(hdr));
    char line[200];
    for (int i = 0; i < g_ntracks; i++) {
        const char *p = g_tracks[i].path[0] ? g_tracks[i].path : g_tracks[i].name;
        int n = snprintf(line, sizeof(line), "%s\n", p);
        sys_write(fd, line, (unsigned long)n);
    }
    sys_close(fd);
}
static void add_track_path(const char *full) {
    if (g_ntracks >= MAX_TRACKS) return;
    track_t *t = &g_tracks[g_ntracks];
    int j = 0; while (full[j] && j < (int)sizeof(t->path) - 1) { t->path[j] = full[j]; j++; }
    t->path[j] = 0;
    const char *b = full; for (const char *p = full; *p; p++) if (*p == '/') b = p + 1;
    int k = 0; while (b[k] && k < NAME_LEN - 1) { t->name[k] = b[k]; k++; }
    t->name[k] = 0;
    g_ntracks++;
}
static void playlist_load(void) {
    size_t len = 0;
    uint8_t *buf = read_file(PLAYLIST_FILE, &len);
    if (!buf) return;
    g_ntracks = 0; g_sel = -1; g_cur = -1; g_pl_scroll = 0;
    size_t i = 0;
    char line[200];
    while (i < len) {
        int k = 0;
        while (i < len && buf[i] != '\n' && buf[i] != '\r' && k < (int)sizeof(line) - 1) line[k++] = (char)buf[i++];
        line[k] = 0;
        while (i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;
        if (k == 0 || line[0] == '#') continue;     // skip blanks, #EXTM3U/#EXTINF and .pls keys with '='
        // .pls lines look like "File1=/path"; take the part after '='
        char *path = line;
        for (int j = 0; line[j]; j++) if (line[j] == '=') { path = &line[j + 1]; break; }
        if (path[0]) add_track_path(path);
    }
    free(buf);
    if (g_ntracks > 0) g_sel = 0;
}

// ----------------------------------------------------------------------------
// Bitmap-skin layout (window stays 400x478; the WinAmp windows are 275 wide)
// ----------------------------------------------------------------------------
#define SK_MX   0
#define SK_MY   0
#define SK_EQX  0
// #342: align the skin EQ/PL regions to the SAME g_fb rows as the procedural
// modules (MOD_EQ_Y / MOD_PL_Y) so the EQ / PL sub-windows can blit a fixed g_fb
// region regardless of which skin engine is active. The skin main/eq bitmaps are
// a little shorter than the region, leaving a thin rack strip below them.
#define SK_EQY  MOD_EQ_Y
#define SK_PLX  0
#define SK_PLY  MOD_PL_Y
#define SK_PLH  (H - SK_PLY)
#define SK_PLROW 12

static void compose_skin(void) {
    // rack background fill
    fb_rect(0, 0, W, H, g_bskin.rack_bg);

    // ============ MAIN (275x116) ============
    blit_full(&g_bskin.main, SK_MX, SK_MY);
    if (g_bskin.titlebar.px) blit_sprite(&g_bskin.titlebar, 27, 0, 275, 14, SK_MX, SK_MY);

    // mono / stereo
    if (g_bskin.monoster.px) {
        blit_sprite(&g_bskin.monoster, 29, 12, 27, 12, SK_MX + 212, SK_MY + 41);                       // mono (off)
        blit_sprite(&g_bskin.monoster, 0, (g_state != ST_STOPPED) ? 0 : 12, 29, 12, SK_MX + 239, SK_MY + 41); // stereo
    }
    // play/pause/stop status flag
    if (g_bskin.playpaus.px) {
        int s = (g_state == ST_PLAYING) ? 0 : (g_state == ST_PAUSED) ? 9 : 18;
        blit_sprite(&g_bskin.playpaus, s, 0, 9, 9, SK_MX + 24, SK_MY + 28);
    }
    // time
    {
        long disp = g_show_remaining ? (g_track_len - g_elapsed) : g_elapsed;
        if (disp < 0) disp = 0;
        long mm = disp / 60, ss = disp % 60; if (mm > 99) mm = 99;
        draw_digit_bmp((int)(mm / 10), SK_MX + 48, SK_MY + 26);
        draw_digit_bmp((int)(mm % 10), SK_MX + 60, SK_MY + 26);
        draw_digit_bmp((int)(ss / 10), SK_MX + 78, SK_MY + 26);
        draw_digit_bmp((int)(ss % 10), SK_MX + 90, SK_MY + 26);
    }
    // spectrum visualizer (24,43) 76x16
    {
        int vx = SK_MX + 24, vy = SK_MY + 43, vw = 76, vh = 16;
        int bw = vw / NUM_BARS; if (bw < 1) bw = 1;
        for (int i = 0; i < NUM_BARS; i++) {
            int bh = g_bar[i] * vh / 63;
            for (int yy = 0; yy < bh; yy++) {
                int lvl = (vh - yy) * 63 / vh;
                uint32_t c = lvl > 44 ? 0xE0A000 : lvl > 26 ? 0xC0D020 : 0x20C040;
                fb_hline(vx + i * bw, vy + vh - 1 - yy, bw > 1 ? bw - 1 : 1, c);
            }
        }
    }
    // marquee title (text.bmp) + bitrate/khz
    {
        char buf[80];
        marquee(buf, sizeof(buf));
        draw_text_bmp(SK_MX + 111, SK_MY + 24, buf, 30);
        draw_text_bmp(SK_MX + 111, SK_MY + 43, g_state == ST_STOPPED ? "" : "192", 3);
        draw_text_bmp(SK_MX + 130, SK_MY + 43, g_state == ST_STOPPED ? "" : "44", 2);
    }
    // posbar
    if (g_bskin.posbar.px) {
        blit_sprite(&g_bskin.posbar, 0, 0, 248, 10, SK_MX + 16, SK_MY + 72);
        int tx = SK_MX + 16 + (248 - 29) * g_seek / 100;
        blit_sprite(&g_bskin.posbar, 248, 0, 29, 10, tx, SK_MY + 72);
    }
    // volume
    if (g_bskin.volume.px) {
        int bar = g_vol * 27 / 100; if (bar < 0) bar = 0; if (bar > 27) bar = 27;
        blit_sprite(&g_bskin.volume, 0, bar * 15, 68, 13, SK_MX + 107, SK_MY + 57);
        int tx = SK_MX + 107 + (68 - 14) * g_vol / 100;
        blit_sprite(&g_bskin.volume, 15, 422, 14, 11, tx, SK_MY + 57);
    }
    // balance
    if (g_bskin.balance.px) {
        int bal = g_balance < 0 ? -g_balance : g_balance;
        int bar = bal * 27 / 50; if (bar > 27) bar = 27;
        blit_sprite(&g_bskin.balance, 9, bar * 15, 38, 13, SK_MX + 177, SK_MY + 57);
        int tx = SK_MX + 177 + (38 - 14) * (g_balance + 50) / 100;
        blit_sprite(&g_bskin.balance, 15, 422, 14, 11, tx, SK_MY + 57);
    }
    // transport buttons
    if (g_bskin.cbuttons.px) {
        int pp = (g_state == ST_PLAYING), pz = (g_state == ST_PAUSED);
        blit_sprite(&g_bskin.cbuttons, 0,   0,       23, 18, SK_MX + 16,  SK_MY + 88);
        blit_sprite(&g_bskin.cbuttons, 23,  pp ? 18 : 0, 23, 18, SK_MX + 39,  SK_MY + 88);
        blit_sprite(&g_bskin.cbuttons, 46,  pz ? 18 : 0, 23, 18, SK_MX + 62,  SK_MY + 88);
        blit_sprite(&g_bskin.cbuttons, 69,  0,       23, 18, SK_MX + 85,  SK_MY + 88);
        blit_sprite(&g_bskin.cbuttons, 92,  0,       22, 18, SK_MX + 108, SK_MY + 88);
        blit_sprite(&g_bskin.cbuttons, 114, 0,       22, 16, SK_MX + 136, SK_MY + 89);
    }
    // shuffle / repeat / eq / pl toggles
    if (g_bskin.shufrep.px) {
        blit_sprite(&g_bskin.shufrep, 28, g_shuffle ? 30 : 0, 47, 15, SK_MX + 164, SK_MY + 89);
        blit_sprite(&g_bskin.shufrep, 0,  g_repeat  ? 30 : 0, 28, 15, SK_MX + 210, SK_MY + 89);
        blit_sprite(&g_bskin.shufrep, 0,  g_eq_on ? 73 : 61,  23, 12, SK_MX + 219, SK_MY + 58);
        blit_sprite(&g_bskin.shufrep, 23, 73,                 23, 12, SK_MX + 242, SK_MY + 58);
    }

    // ============ EQ (275x116) ============
    blit_full(&g_bskin.eqmain, SK_EQX, SK_EQY);
    if (g_bskin.eqmain.px) {
        blit_sprite(&g_bskin.eqmain, 0, 134, 275, 14, SK_EQX, SK_EQY);           // titlebar (selected)
        blit_sprite(&g_bskin.eqmain, g_eq_on ? 69 : 10, 119, 26, 12, SK_EQX + 14, SK_EQY + 18); // ON
        blit_sprite(&g_bskin.eqmain, 36, 119, 32, 12, SK_EQX + 39, SK_EQY + 18); // AUTO
        int ty0 = SK_EQY + 38, trav = 50;
        int ky = ty0 + (63 - g_eq_preamp) * trav / 63;
        blit_sprite(&g_bskin.eqmain, 0, 164, 11, 11, SK_EQX + 21, ky);           // preamp
        for (int i = 0; i < NUM_EQ; i++) {
            int kx = SK_EQX + 78 + i * 18;
            int kk = ty0 + (63 - g_eq[i]) * trav / 63;
            blit_sprite(&g_bskin.eqmain, 0, 164, 11, 11, kx, kk);
        }
    }

    // ============ PLAYLIST ============
    fb_rect(SK_PLX, SK_PLY, W, SK_PLH, g_bskin.pl_normalbg);
    if (g_bskin.pledit.px) {
        blit_sprite(&g_bskin.pledit, 0, 21, 25, 20, SK_PLX, SK_PLY);
        for (int x = SK_PLX + 25; x < SK_PLX + W - 25; x += 100)
            blit_sprite(&g_bskin.pledit, 26, 21, 100, 20, x, SK_PLY);
        blit_sprite(&g_bskin.pledit, 153, 21, 25, 20, SK_PLX + W - 25, SK_PLY);
    }
    {
        int listy = SK_PLY + 20, vis = (SK_PLH - 24) / SK_PLROW;
        for (int i = 0; i < vis && (g_pl_scroll + i) < g_ntracks; i++) {
            int idx = g_pl_scroll + i, ry = listy + i * SK_PLROW;
            if (idx == g_sel) fb_rect(SK_PLX + 3, ry, W - 6, SK_PLROW, g_bskin.pl_selectbg);
        }
    }
    // album art thumbnail (top-right of the playlist area)
    mp_draw_art(284, SK_MY + 18, 100);   // #335 album art fills the space right of the 275px main module

    draw_panel_toggles_fb();   // #342 [EQ][PL][VIS][ART] chips overlaid on the skin title bar
}

// Overlay (text drawn on top of the blitted chrome) for bitmap-skin mode.
static void overlay_skin(void) {
    char buf[96];
    // The skin supplies its own bitmap title bars, so the only text we overlay
    // is the playlist contents (text.bmp is uppercase-only) and a status line,
    // routed into the separate playlist window (#342).
    g_ov_win = pl_win; g_ov_dy = MOD_PL_Y;
    int listy = SK_PLY + 20, vis = (SK_PLH - 24) / SK_PLROW;
    for (int i = 0; i < vis && (g_pl_scroll + i) < g_ntracks; i++) {
        int idx = g_pl_scroll + i, ry = listy + 2 + i * SK_PLROW;
        uint32_t c = (idx == g_cur) ? g_bskin.pl_current : g_bskin.pl_normal;
        char lbl[96]; track_label(&g_tracks[idx], lbl, sizeof lbl);
        snprintf(buf, sizeof(buf), "%2d. %s", idx + 1, lbl);
        OT(SK_PLX + 6, ry, buf, c);
    }
    // bottom status row: tracks / volume / current skin (left), LIB / SAVE / LOAD (right)
    snprintf(buf, sizeof(buf), "%d TRK  VOL %d  SKIN: %s  [K=NEXT V=LIB]", g_ntracks, g_vol, g_bskin.name);
    OT(SK_PLX + 6, H - 11, buf, g_bskin.pl_normal);
    OT(W - 132, H - 11, "LIB", g_bskin.pl_current);
    OT(W - 92, H - 11, "SAVE", g_bskin.pl_current);
    OT(W - 52, H - 11, "LOAD", g_bskin.pl_current);
    // #335 skin-cycle [S] box indicator over the shade slot (matches on_click_skin)
    win_draw_text_small(win, SK_MX + 255, SK_MY + 3, "S", g_bskin.pl_current);
    overlay_panel_toggles();   // #342 [EQ][PL][VIS][ART] chip labels (main window)
}

// Hit-testing for bitmap-skin mode.
static int hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}
static void on_click_skin(int mx, int my) {
    if (panel_toggle_hit(mx, my)) return;   // #342 [EQ][PL][VIS][ART] chips
    // titlebar controls, left-to-right: minimise, change-skin (shade slot), close
    if (hit(mx, my, SK_MX + 244, SK_MY + 3, 9, 11)) { do_minimize(); return; }
    if (hit(mx, my, SK_MX + 264, SK_MY + 3, 11, 11)) { close_all_windows(); win_destroy(win); exit(0); }
    // #335: skin cycling ONLY via the small [S] box. A drag on the body must NOT
    // change the skin (window drag = move via #334; a real click reaches here).
    if (hit(mx, my, SK_MX + 254, SK_MY + 2, 10, 11)) { cycle_skin(+1); return; }
    // transport
    if (hit(mx, my, SK_MX + 16,  SK_MY + 88, 23, 18)) { play_neighbor(-1); return; }
    if (hit(mx, my, SK_MX + 39,  SK_MY + 88, 23, 18)) { if (g_sel >= 0) play_index(g_sel); return; }
    if (hit(mx, my, SK_MX + 62,  SK_MY + 88, 23, 18)) { do_pause(); return; }
    if (hit(mx, my, SK_MX + 85,  SK_MY + 88, 23, 18)) { do_stop(); return; }
    if (hit(mx, my, SK_MX + 108, SK_MY + 88, 22, 18)) { play_neighbor(+1); return; }
    if (hit(mx, my, SK_MX + 136, SK_MY + 89, 22, 16)) { scan_dir(g_dir); return; }
    // shuffle / repeat / eq / pl
    if (hit(mx, my, SK_MX + 164, SK_MY + 89, 47, 15)) { g_shuffle = !g_shuffle; return; }
    if (hit(mx, my, SK_MX + 210, SK_MY + 89, 28, 15)) { g_repeat = !g_repeat; return; }
    if (hit(mx, my, SK_MX + 219, SK_MY + 58, 23, 12)) { g_eq_on = !g_eq_on; return; }
    // time readout toggles elapsed/remaining
    if (hit(mx, my, SK_MX + 36, SK_MY + 26, 64, 13)) { g_show_remaining = !g_show_remaining; return; }
    // posbar
    if (hit(mx, my, SK_MX + 16, SK_MY + 72, 248, 10)) {
        g_seek = (mx - (SK_MX + 16)) * 100 / 248;
        if (g_seek < 0) g_seek = 0; if (g_seek > 100) g_seek = 100; return;
    }
    // volume
    if (hit(mx, my, SK_MX + 107, SK_MY + 57, 68, 13)) {
        g_vol = (mx - (SK_MX + 107)) * 100 / 68;
        if (g_vol < 0) g_vol = 0; if (g_vol > 100) g_vol = 100; set_volume(g_vol); return;
    }
    // balance
    if (hit(mx, my, SK_MX + 177, SK_MY + 57, 38, 13)) {
        g_balance = (mx - (SK_MX + 177)) * 100 / 38 - 50;
        if (g_balance < -50) g_balance = -50; if (g_balance > 50) g_balance = 50; return;
    }
    // EQ ON / AUTO(FLAT)
    if (hit(mx, my, SK_EQX + 14, SK_EQY + 18, 26, 12)) { g_eq_on = !g_eq_on; return; }
    if (hit(mx, my, SK_EQX + 39, SK_EQY + 18, 32, 12)) {
        for (int i = 0; i < NUM_EQ; i++) g_eq[i] = 32; g_eq_preamp = 32; return;
    }
    // EQ preamp + bands
    if (my >= SK_EQY + 38 && my < SK_EQY + 88) {
        if (mx >= SK_EQX + 21 && mx < SK_EQX + 32) {
            g_eq_preamp = 63 - (my - (SK_EQY + 38)) * 63 / 50;
            if (g_eq_preamp < 0) g_eq_preamp = 0; if (g_eq_preamp > 63) g_eq_preamp = 63; return;
        }
        for (int i = 0; i < NUM_EQ; i++) {
            int kx = SK_EQX + 78 + i * 18;
            if (mx >= kx - 3 && mx < kx + 13) {
                g_eq[i] = 63 - (my - (SK_EQY + 38)) * 63 / 50;
                if (g_eq[i] < 0) g_eq[i] = 0; if (g_eq[i] > 63) g_eq[i] = 63; return;
            }
        }
    }
    // playlist LIB / SAVE / LOAD (bottom status row)
    if (my >= H - 12 && my < H) {
        if (mx >= W - 132 && mx < W - 100) { lib_open(); lib_scroll_to_sel(); return; }
        if (mx >= W - 92 && mx < W - 60) { playlist_save(); return; }
        if (mx >= W - 52 && mx < W - 20) { playlist_load(); return; }
    }
    // playlist rows
    if (mx >= SK_PLX && mx < W && my >= SK_PLY + 20 && my < H - 12) {
        int idx = g_pl_scroll + (my - (SK_PLY + 20)) / SK_PLROW;
        if (idx >= 0 && idx < g_ntracks) {
            if (idx == g_sel) play_index(idx);
            else g_sel = idx;
        }
        return;
    }
}

// ----------------------------------------------------------------------------
// Frame
// ----------------------------------------------------------------------------
// #342: blit a sub-region of the shared g_fb (rows [y0, y0+h)) into a window.
static void blit_region(int handle, int y0, int h) {
    if (handle < 0) return;
    syscall5(SYS_WIN_BLIT, handle, 0, 0, (W & 0xFFFF) | ((h & 0xFFFF) << 16),
             (long)(g_fb + (size_t)y0 * W));
    win_invalidate(handle);
}
static void render(void) {
    if (g_view == 1 && lib_win >= 0) {
        compose_library();
        syscall5(SYS_WIN_BLIT, lib_win, 0, 0, (W & 0xFFFF) | ((H & 0xFFFF) << 16), (long)g_fb);
        overlay_library();
        win_invalidate(lib_win);
        return;
    }
    update_spectrum();
    if (g_bitmap_active && g_bskin.loaded) compose_skin();
    else compose();
    // main window = the top MAIN region; EQ / PL sub-windows blit their own g_fb
    // region only when open (#342).
    blit_region(win, MOD_MAIN_Y, MOD_MAIN_H);
    blit_region(eq_win, MOD_EQ_Y, MOD_EQ_H);
    blit_region(pl_win, MOD_PL_Y, MOD_PL_H);
    if (g_bitmap_active && g_bskin.loaded) overlay_skin();
    else overlay();
    // album-art window (its own buffer + a title caption)
    if (art_win >= 0) {
        compose_art();
        syscall5(SYS_WIN_BLIT, art_win, 0, 0,
                 (ART_WIN_W & 0xFFFF) | ((ART_WIN_H & 0xFFFF) << 16), (long)g_artfb);
        win_draw_text_small(art_win, 6, 3, g_art_have ? "ALBUM ART" : "ALBUM ART (NONE)", 0xDFF0FF);
        win_invalidate(art_win);
    }
}

// #335: current track finished on the DAC -> advance so audio never stops.
// With the playlist wrapping in play_neighbor this loops forever (repeat-all).
static void advance_track(void) {
    g_last_pos_ms = -1; g_pos_stall = 0;
    if (g_ntracks > 0) { g_advancing = 1; play_neighbor(+1); g_advancing = 0; }  // wraps => continuous loop
    else do_stop();
}

static void tick_time(void) {
    if (g_state == ST_PLAYING) {
        long since = now_sec() - g_play_start_sec;
        if (since < 0) since += 86400;
        long pos = mp_audio_pos_ms();      // #335 real DAC position (ms); -1 if no USB DAC
        if (pos >= 0) {
            g_elapsed = pos / 1000;
            if (pos > 300) {
                if (pos == g_last_pos_ms) g_pos_stall++;
                else { g_pos_stall = 0; g_last_pos_ms = pos; }
            }
            // End of track = the DAC position has been FROZEN for ~1.5s (the stream
            // stopped submitting frames and is draining). advance_track() then waits
            // for the helper to exit so the ring is idle before the next stream.
            if (since >= 3 && g_pos_stall >= 15) { advance_track(); return; }
        } else {
            // No USB-DAC position (e.g. HDA-only VM): fall back to the wall clock.
            g_elapsed = since;
            if (g_track_len > 0 && since >= 2 && g_elapsed >= g_track_len) { advance_track(); return; }
        }
        g_seek = (g_track_len > 0) ? (int)(g_elapsed * 100 / g_track_len) : 0;
        if (g_seek > 100) g_seek = 100;
    }
    // hifi-scrollfix: advance the marquee at a FIXED WALL-CLOCK rate (one
    // character step per ~200ms) using the monotonic uptime clock, NOT the
    // frame counter. The MilkDrop viz raised the loop to ~60fps, and the old
    // per-frame step (1 char / 2 frames) made the title whip around at ~30
    // chars/sec. Gating on real time keeps it at a steady, readable ~5 chars/sec
    // whether the viz is open (60fps) or the player idles (10fps).
    {
        static unsigned long s_scroll_ms = 0;
        unsigned long now = uptime_ms();
        if (s_scroll_ms == 0) s_scroll_ms = now;
        while (now - s_scroll_ms >= 200) { g_title_scroll++; s_scroll_ms += 200; }
    }
}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------
// #334: which parts of the rack act as a grab handle for moving the window.
// Returns 1 over empty title strips / module headers (never over a control), so a
// press there starts a window drag instead of activating a control. Every
// transport button, slider, the [S] skin box and playlist row stay clickable (a
// press there falls through to on_click). A drag press never calls on_click, so
// the old drag-changes-skin bug cannot recur.
static int in_drag_zone(int mx, int my) {
    // Main module title strip = universal drag handle. Exclude the control boxes
    // clustered on the right (visualizer/transparency/minimise/skin/close,
    // x >= W-78) so a press there is a real click, never a drag (#335 - a drag
    // must not toggle the viz, transparency or skin).
    // #2/#3: title strip is a drag handle only LEFT of the control cluster
    // (V/T/S/min/close), which starts at x=256. Everything x>=254 in the band is a
    // control area (our buttons, then the dead kernel-button zone) and must NOT
    // start a drag so our buttons fire on click.
    // #342: exclude the [EQ][PL][VIS][ART] toggle cluster on the far left
    // (x < 98) so those chips fire on click, not drag.
    if (my >= MOD_MAIN_Y && my < MOD_MAIN_Y + 18 && mx >= 98 && mx < 254) return 1;
    // #342: EQ / PL are now their own windows; their top title strip is a drag
    // handle in BOTH the procedural and bitmap-skin modes. The playlist header
    // excludes the right side where LIB/SAVE/LOAD live (x >= W-140).
    if (my >= MOD_EQ_Y && my < MOD_EQ_Y + 16) return 1;
    if (my >= MOD_PL_Y && my < MOD_PL_Y + 16 && mx < W - 140) return 1;
    return 0;
}

static void on_click(int mx, int my) {
    if (panel_toggle_hit(mx, my)) return;   // #342 [EQ][PL][VIS][ART] chips
    // #2/#3: all five title-strip controls live left of the kernel button band
    // (window-x < 328) and are excluded from the drag zone, so a real click reaches
    // here instead of being intercepted (or dropped) by the kernel window manager.
    // close box [X] -> tear down all sub-windows too, then exit cleanly (#3/#342)
    if (mx >= 316 && mx < 328 && my >= MOD_MAIN_Y + 3 && my < MOD_MAIN_Y + 15) {
        close_all_windows();      // close viz / EQ / PL / art / library windows
        win_destroy(win); exit(0);
    }
    // minimise box -> iconify to the taskbar
    if (mx >= 301 && mx < 313 && my >= MOD_MAIN_Y + 3 && my < MOD_MAIN_Y + 15) {
        do_minimize(); return;
    }
    // change-skin box [S] (builtin palettes then discovered .wsz skins)
    if (mx >= 286 && mx < 298 && my >= MOD_MAIN_Y + 3 && my < MOD_MAIN_Y + 15) {
        cycle_skin(+1); return;
    }
    // transparency cycle box [T]
    if (mx >= 271 && mx < 283 && my >= MOD_MAIN_Y + 3 && my < MOD_MAIN_Y + 15) {
        cycle_opacity(); return;
    }
    // visualizer toggle box [V] -> open/close the MilkDrop viz window
    if (mx >= 256 && mx < 268 && my >= MOD_MAIN_Y + 3 && my < MOD_MAIN_Y + 15) {
        viz_toggle(); return;
    }
    // playlist LIB / SAVE / LOAD (title strip, right side)
    if (my >= MOD_PL_Y + 4 && my < MOD_PL_Y + 15) {
        if (mx >= W - 132 && mx < W - 100) { lib_open(); lib_scroll_to_sel(); return; }
        if (mx >= W - 92 && mx < W - 60) { playlist_save(); return; }
        if (mx >= W - 52 && mx < W - 20) { playlist_load(); return; }
    }
    // transport
    if (my >= TB_Y && my < TB_Y + TB_H) {
        for (int i = 0; i < HB_N; i++) {
            if (mx >= tb_x(i) && mx < tb_x(i) + TB_W) {
                switch (i) {
                    case HB_PREV:  play_neighbor(-1); break;
                    case HB_PLAY:  if (g_sel >= 0) play_index(g_sel); break;
                    case HB_PAUSE: do_pause(); break;
                    case HB_STOP:  do_stop(); break;
                    case HB_NEXT:  play_neighbor(+1); break;
                    case HB_EJECT: scan_dir(g_dir); break;
                }
                return;
            }
        }
    }
    // shuffle / repeat chips
    if (my >= MOD_MAIN_Y + 88 && my < MOD_MAIN_Y + 100) {
        if (mx >= 8 && mx < 64)  { g_shuffle = !g_shuffle; return; }
        if (mx >= 66 && mx < 122) { g_repeat = !g_repeat; return; }
    }
    // LCD toggle elapsed/remaining
    if (mx >= 16 && mx < 140 && my >= MOD_MAIN_Y + 28 && my < MOD_MAIN_Y + 58) {
        g_show_remaining = !g_show_remaining; return;
    }
    // seek slider
    if (my >= SEEK_Y && my < SEEK_Y + 11 && mx >= SEEK_X && mx < SEEK_X + SEEK_W) {
        g_seek = (mx - SEEK_X) * 100 / SEEK_W;
        if (g_seek < 0) g_seek = 0; if (g_seek > 100) g_seek = 100;
        return;
    }
    // volume slider
    if (my >= VOL_Y && my < VOL_Y + 11 && mx >= VOL_X && mx < VOL_X + VOL_W) {
        g_vol = (mx - VOL_X) * 100 / VOL_W;
        if (g_vol < 0) g_vol = 0; if (g_vol > 100) g_vol = 100;
        set_volume(g_vol); return;
    }
    // balance slider
    if (my >= VOL_Y && my < VOL_Y + 11 && mx >= BAL_X && mx < BAL_X + BAL_W) {
        g_balance = (mx - BAL_X) * 100 / BAL_W - 50;
        if (g_balance < -50) g_balance = -50; if (g_balance > 50) g_balance = 50;
        return;
    }
    // EQ ON
    if (my >= MOD_EQ_Y + 22 && my < MOD_EQ_Y + 34) {
        if (mx >= 8 && mx < 34) { g_eq_on = !g_eq_on; return; }
        if (mx >= W - 60 && mx < W - 8) {     // FLAT preset
            for (int i = 0; i < NUM_EQ; i++) g_eq[i] = 32;
            g_eq_preamp = 32; return;
        }
    }
    // EQ preamp
    if (mx >= EQ_PRE_X && mx < EQ_PRE_X + 14 && my >= EQ_SL_Y && my < EQ_SL_Y + EQ_SL_H) {
        g_eq_preamp = 63 - (my - EQ_SL_Y) * 63 / EQ_SL_H;
        if (g_eq_preamp < 0) g_eq_preamp = 0; if (g_eq_preamp > 63) g_eq_preamp = 63;
        return;
    }
    // EQ bands
    if (my >= EQ_SL_Y && my < EQ_SL_Y + EQ_SL_H) {
        for (int i = 0; i < NUM_EQ; i++) {
            int x = EQ_BANDS_X + i * EQ_BANDS_DX;
            if (mx >= x - 4 && mx < x + 16) {
                g_eq[i] = 63 - (my - EQ_SL_Y) * 63 / EQ_SL_H;
                if (g_eq[i] < 0) g_eq[i] = 0; if (g_eq[i] > 63) g_eq[i] = 63;
                return;
            }
        }
    }
    // playlist row
    if (mx >= PL_LIST_X && mx < W - PL_LIST_X &&
        my >= PL_LIST_Y && my < PL_LIST_Y + PL_LIST_H) {
        int idx = g_pl_scroll + (my - PL_LIST_Y - 1) / PL_ROW_H;
        if (idx >= 0 && idx < g_ntracks) {
            if (idx == g_sel) play_index(idx);   // re-click = play
            else g_sel = idx;
        }
        return;
    }
}

// ----------------------------------------------------------------------------
// #342: pump events for a sub-window (EQ / PL). Clicks are translated back into
// the shared rack coordinate space (add the section's g_fb origin) and routed
// through the SAME on_click / on_click_skin handlers the main window uses, so all
// existing hit-testing keeps working. The section's top title strip acts as a
// drag handle (in_drag_zone).
static void poll_panel(int handle, int base) {
    if (handle < 0) return;
    gui_event_t ev;
    while (win_get_event(handle, &ev, 0) > 0) {
        if (ev.type == EVENT_WINDOW_CLOSE) {
            if (handle == eq_win) panel_close_eq();
            else if (handle == pl_win) panel_close_pl();
            return;
        }
        if (ev.type == EVENT_KEY_DOWN) {
            // #342: toggle hotkeys also work when a sub-window holds focus. A
            // toggle may destroy THIS window, so return rather than keep pumping it.
            char c = ev.key_char;
            if (c == 'e' || c == 'E') { panel_toggle_eq(); return; }
            if (c == 'p' || c == 'P') { panel_toggle_pl(); return; }
            if (c == 'a' || c == 'A') { panel_toggle_art(); return; }
            if (c == 'g' || c == 'G') { viz_toggle(); return; }
            continue;
        }
        if (ev.type == EVENT_MOUSE_DOWN) {
            int mx = ev.mouse_x, my = ev.mouse_y + base;
            if (in_drag_zone(mx, my)) start_drag(handle);
            else if (g_bitmap_active && g_bskin.loaded) on_click_skin(mx, my);
            else on_click(mx, my);
        } else if (ev.type == EVENT_MOUSE_SCROLL && base == MOD_PL_Y) {
            g_pl_scroll -= ev.scroll_delta;
            if (g_pl_scroll < 0) g_pl_scroll = 0;
            int maxs = g_ntracks - PL_LIST_H / PL_ROW_H;
            if (maxs < 0) maxs = 0;
            if (g_pl_scroll > maxs) g_pl_scroll = maxs;
        }
    }
}
// Pump the album-art window: its title strip drags it; a click elsewhere closes.
static void poll_art(void) {
    if (art_win < 0) return;
    gui_event_t ev;
    while (win_get_event(art_win, &ev, 0) > 0) {
        if (ev.type == EVENT_WINDOW_CLOSE) { panel_close_art(); return; }
        if (ev.type == EVENT_KEY_DOWN) {
            char c = ev.key_char;
            if (c == 'e' || c == 'E') { panel_toggle_eq(); return; }
            if (c == 'p' || c == 'P') { panel_toggle_pl(); return; }
            if (c == 'a' || c == 'A') { panel_toggle_art(); return; }   // may destroy art_win
            if (c == 'g' || c == 'G') { viz_toggle(); return; }
            continue;
        }
        if (ev.type == EVENT_MOUSE_DOWN) {
            if (ev.mouse_y < ART_WIN_TH) start_drag(art_win);   // title strip = drag
        }
    }
}
// Pump the library window (its own window now): navigation + play + close.
static void poll_lib(void) {
    if (lib_win < 0) return;
    gui_event_t ev;
    while (win_get_event(lib_win, &ev, 0) > 0) {
        if (ev.type == EVENT_WINDOW_CLOSE) { lib_close(); return; }
        if (ev.type == EVENT_KEY_DOWN) {
            if (ev.keycode == 0x01 || ev.key_char == 'v' || ev.key_char == 'V') { lib_close(); return; }
            else if (ev.key_char == 'r' || ev.key_char == 'R') lib_rescan();
            else if (ev.keycode == 0x80) { if (g_lib_sel > 0) g_lib_sel--; lib_scroll_to_sel(); }
            else if (ev.keycode == 0x81) { if (g_lib_sel < g_nrows - 1) g_lib_sel++; lib_scroll_to_sel(); }
            else if (ev.keycode == 0x1C || ev.key_char == 10 || ev.key_char == 13) lib_play_row(g_lib_sel);
        } else if (ev.type == EVENT_MOUSE_DOWN) {
            lib_click(ev.mouse_x, ev.mouse_y);
        } else if (ev.type == EVENT_MOUSE_SCROLL) {
            g_lib_scroll -= ev.scroll_delta;
            int vis = (H - LIB_TOP - 4) / LIB_ROWH, maxs = g_nrows - vis;
            if (maxs < 0) maxs = 0;
            if (g_lib_scroll < 0) g_lib_scroll = 0;
            if (g_lib_scroll > maxs) g_lib_scroll = maxs;
        }
    }
}

// ----------------------------------------------------------------------------
// main (dual mode: UI or --play helper)
// ----------------------------------------------------------------------------
int main(int argc, char **argv) {
    // Helper mode: perform the blocking kernel decode/play, then exit. This is
    // spawned by the UI so the main window keeps animating.
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'p') {
        sys_play_wav(argv[2]);
        return 0;
    }

    for (int i = 0; i < NUM_EQ; i++) g_eq[i] = 32;
    g_repeat = 1;   // #335 repeat-all so playback loops continuously

    // #342: the main window now holds ONLY the MAIN module; EQ / PLAYLIST / ART
    // are separate toggleable windows.
    win = win_create("Maytera HiFi", 170, 46, W, MOD_MAIN_H);
    if (win < 0) return 1;
    win_set_nochrome(win);
    { int _fd = sys_open("/CONFIG/MPDRAGTEST.CFG", 0);
      if (_fd >= 0) { g_dragtest = 1; sys_close(_fd); } }

    scan_dir(g_dir);
    if (g_ntracks == 0) { strcpy(g_dir, "/"); scan_dir(g_dir); }

    // Discover .wsz skins and restore the last chosen skin (built-in or .wsz).
    scan_skins();
    load_skin_cfg();

    // Build the music library: load the persisted index if present, else do a
    // recursive scan of the configured music dirs (/CONFIG/MPLIB.CFG or the
    // /HOME + /HOME/MUSIC defaults) and persist the index for next launch.
    if (lib_load() > 0) { lib_build_rows(); }
    else lib_rescan();

    art_selftest();   // optional /CONFIG/ARTTEST.LST decode diagnostics

    // Phase 2c: ensure the online cover-art cache dir exists, and honour an
    // opt-out flag (/CONFIG/MPONLINE.CFG containing "0" disables online lookups).
    sys_mkdir("/CONFIG/MPART", 0755);
    { int fd = sys_open("/CONFIG/MPONLINE.CFG", 0);
      if (fd >= 0) { char c = 0; if (sys_read(fd, &c, 1) > 0 && c == '1') g_online_enabled = 1; sys_close(fd); } }

    // Initial file to auto-play: a path argument (file association, #84) or the
    // path stored in /CONFIG/MPAUTO. Lets "Open with" from Files start playback,
    // and gives a focus-independent way to kick off playback.
    char initfile[160]; initfile[0] = 0;
    if (argc >= 2 && argv[1][0] == '/') {
        int j = 0; while (argv[1][j] && j < (int)sizeof(initfile) - 1) { initfile[j] = argv[1][j]; j++; }
        initfile[j] = 0;
    } else {
        int fd = sys_open("/CONFIG/MPAUTO", 0);
        if (fd >= 0) {
            int n = (int)sys_read(fd, initfile, sizeof(initfile) - 1);
            sys_close(fd);
            if (n > 0) { initfile[n] = 0; for (int k = 0; k < n; k++) if (initfile[k] == '\n' || initfile[k] == '\r') { initfile[k] = 0; break; } }
            else initfile[0] = 0;
        }
    }
    if (initfile[0]) play_path(initfile);

    // hifi-milkdrop-viz: optionally auto-open the visualizer at startup, gated by
    // /CONFIG/MPVIZ.CFG. If its first byte is '1'..'3' it selects the preset
    // (1=TUNNEL, 2=SWIRL, 3=STARBURST). Handy for headless verification.
    { int fd = sys_open("/CONFIG/MPVIZ.CFG", 0);
      if (fd >= 0) {
          char c = 0; sys_read(fd, &c, 1); sys_close(fd);
          viz_open();
          if (c >= '1' && c <= '3') for (int p = 1; p < c - '0'; p++) viz_next_preset();
      } }

    // #342: which panels to show at startup. Default = EQ + PLAYLIST open (matches
    // the classic stacked rack). /CONFIG/MPPANELS.CFG lets you override with any of
    // the letters e (EQ), p (playlist), a (album art), v (visualizer); e.g. "epav"
    // opens all four. Handy for headless screendump verification.
    { char pcfg[16]; int have = 0, n = 0;
      int fd = sys_open("/CONFIG/MPPANELS.CFG", 0);
      if (fd >= 0) { n = (int)sys_read(fd, pcfg, sizeof(pcfg) - 1); sys_close(fd); if (n > 0) { pcfg[n] = 0; have = 1; } }
      if (!have) { mp_cpy(pcfg, "ep", sizeof pcfg); }
      for (int i = 0; pcfg[i]; i++) {
          char c = pcfg[i];
          if (c == 'e' || c == 'E') panel_open_eq();
          else if (c == 'p' || c == 'P') panel_open_pl();
          else if (c == 'a' || c == 'A') panel_open_art();
          else if (c == 'v' || c == 'V') { if (!viz_is_open()) viz_open(); }
      } }

    int running = 1;
    while (running) {
        gui_event_t ev;
        // Keep the loop lively when dragging or when the viz window is open (so it
        // animates at ~60fps); otherwise idle at 10fps to save CPU.
        int busy = (g_drag_win >= 0) || viz_is_open() || art_win >= 0;
        int r = win_get_event(win, &ev, (g_drag_win >= 0) ? 10 : (busy ? 16 : 100));
        if (r > 0) {
            switch (ev.type) {
                case EVENT_WINDOW_CLOSE:
                    running = 0; break;
                case EVENT_KEY_DOWN:
                    if (g_view == 1) {                              // library browser
                        if (ev.keycode == 0x01 || ev.key_char == 'v' || ev.key_char == 'V') lib_close();
                        else if (ev.key_char == 'r' || ev.key_char == 'R') lib_rescan();
                        else if (ev.keycode == 0x80) { if (g_lib_sel > 0) g_lib_sel--; lib_scroll_to_sel(); }
                        else if (ev.keycode == 0x81) { if (g_lib_sel < g_nrows - 1) g_lib_sel++; lib_scroll_to_sel(); }
                        else if (ev.keycode == 0x1C || ev.key_char == 10 || ev.key_char == 13) lib_play_row(g_lib_sel);
                        break;
                    }
                    if (ev.keycode == 0x01) running = 0;           // Esc
                    else if (ev.key_char == ' ') do_pause();
                    else if (ev.key_char == 's' || ev.key_char == 'S') do_stop();
                    else if (ev.key_char == 'z' || ev.key_char == 'Z') play_neighbor(-1);
                    else if (ev.key_char == 'b' || ev.key_char == 'B') play_neighbor(+1);
                    else if (ev.key_char == 'k' || ev.key_char == 'K') cycle_skin(+1);
                    else if (ev.key_char == 'j' || ev.key_char == 'J') cycle_skin(-1);
                    else if (ev.key_char == 'o' || ev.key_char == 'O') playlist_save();
                    else if (ev.key_char == 'l' || ev.key_char == 'L') playlist_load();
                    else if (ev.key_char == 'v' || ev.key_char == 'V') { lib_open(); lib_scroll_to_sel(); }
                    else if (ev.key_char == 'g' || ev.key_char == 'G') viz_toggle();       // toggle MilkDrop viz
                    // #342 keyboard equivalents of the [EQ][PL][ART][VIS] chips.
                    else if (ev.key_char == 'e' || ev.key_char == 'E') panel_toggle_eq();
                    else if (ev.key_char == 'p' || ev.key_char == 'P') panel_toggle_pl();
                    else if (ev.key_char == 'a' || ev.key_char == 'A') panel_toggle_art();
                    else if (ev.key_char == 't' || ev.key_char == 'T') cycle_opacity();     // cycle transparency
                    else if (ev.key_char == ']') { if (viz_is_open()) viz_next_preset(); }  // next viz preset
                    else if (ev.keycode == 0x80) { if (g_sel > 0) g_sel--; }
                    else if (ev.keycode == 0x81) { if (g_sel < g_ntracks - 1) g_sel++; }
                    else if (ev.keycode == 0x1C || ev.key_char == 10 || ev.key_char == 13) { if (g_sel >= 0) play_index(g_sel); }
                    break;
                case EVENT_MOUSE_DOWN:
                    // #334/#342: main-window press. A press on a title-strip drag
                    // handle starts a window drag; otherwise route the click.
                    if (in_drag_zone(ev.mouse_x, ev.mouse_y)) {
                        start_drag(win);
                    }
                    else if (g_bitmap_active && g_bskin.loaded) on_click_skin(ev.mouse_x, ev.mouse_y);
                    else on_click(ev.mouse_x, ev.mouse_y);
                    break;
                case EVENT_MOUSE_UP:
                    g_drag_win = -1;
                    break;
                case EVENT_MOUSE_SCROLL:
                    g_pl_scroll -= ev.scroll_delta;
                    if (g_pl_scroll < 0) g_pl_scroll = 0;
                    {
                        int maxs = g_ntracks - PL_LIST_H / PL_ROW_H;
                        if (maxs < 0) maxs = 0;
                        if (g_pl_scroll > maxs) g_pl_scroll = maxs;
                    }
                    break;
                default: break;
            }
        }
        // #334/#342: pump the sub-windows (EQ / PL / ART / library) each loop so
        // they are clickable + draggable independently of the main window.
        if (g_view == 1) poll_lib();
        else { poll_panel(eq_win, MOD_EQ_Y); poll_panel(pl_win, MOD_PL_Y); poll_art(); }

        // #334: drive an in-progress window drag from the GLOBAL cursor (works for
        // whichever window is being dragged). Release (button 1 up) ends the drag.
        if (g_drag_win >= 0) {
            int gx = 0, gy = 0; unsigned int gb = 0;
            get_global_mouse(&gx, &gy, &gb);
            if (gb & 1) win_move(g_drag_win, gx - g_grab_dx, gy - g_grab_dy);
            else g_drag_win = -1;
        }
        // #334 self-test glide: repeated win_move() (mirrors the drag poll).
        if (g_dragtest && g_drag_win < 0 && g_frame >= 40 && g_frame <= 90) {
            int wx = 0, wy = 0; win_get_pos(win, &wx, &wy);
            win_move(win, wx + 8, wy + 5);
        }
        g_frame++;
        tick_time();
        online_art_poll();           // drive the async cover-art fetch (non-blocking)
        // Drive the MilkDrop visualizer (its own GL window in this process). Feed
        // it the SAME spectrum the EQ/analyzer shows plus the real DAC position so
        // the warp + waveform track the actual audio; it renders + blits itself.
        if (viz_is_open()) {
            // hifi-vizreact: refresh the spectrum EVERY loop while the viz is open
            // (independent of the render() throttle / which window has focus) so
            // the visualizer always has live, moving bars to react to.
            update_spectrum();
            viz_set_audio(g_bar, NUM_BARS, g_state == ST_PLAYING, mp_audio_pos_ms());
            viz_frame();
        }
        // Throttle redraws when idle to save CPU: full 10fps spectrum while
        // playing or right after an event; a gentle ~3fps shimmer when stopped.
        if (g_state == ST_PLAYING || r > 0 || g_drag_win >= 0 || (g_frame % 3) == 0)
            render();

        // #342: deferred cover load. Runs AFTER the first render so the window +
        // audio are already up; the whole-track read fills the embedded cover and
        // the online lookup (opt-in) upgrades it. The FS read can be starved while
        // the decode helper streams the SAME file, so a miss is retried a few times
        // spread ~1.5s apart across frames (never blocks the loop, never touches
        // the network). Purely local; verified not to affect stability.
        if (g_art_pending && g_frame >= g_art_retry_at) {
            g_art_pending = 0;
            load_art_for(g_pending_art);
            online_art_kick(g_pending_artist, g_pending_album);
            if (!g_art_have && g_art_retry < 8) {
                g_art_retry++;
                g_art_path[0] = 0;                 // clear one-shot cache so we re-read
                g_art_pending = 1;
                g_art_retry_at = g_frame + 15;
            } else {
                g_art_retry = 0;
            }
        }
    }
    close_all_windows();
    win_destroy(win);
    return 0;
}
