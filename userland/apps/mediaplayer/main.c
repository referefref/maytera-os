// mediaplayer - Media Player for MayteraOS (user-space version), glass edition
// Audio/video playback with controls and playlist.
//
// (audglass) The chrome is the shared dark-teal glass language
// (docs/UI_GLASS_DESIGN_SYSTEM.md sections 1, 10, 11), the same material as
// the App Repo, the Task Manager (tmglass) and the Calculator (calcglass): a
// frosted-wallpaper backdrop in the margins, three rounded glass panels
// (stage, playlist, controls), two mode pills on the backdrop (Player /
// Cinema, which is the pre-existing fullscreen toggle), transport buttons and
// sliders built from the shared rounded-AA primitives, and antialiased
// TrueType text. Fixed dark glass regardless of theme (owner decision
// recorded at glasstm). Everything the player DOES (prev / play-pause / stop
// / next, click-to-seek, click-to-set volume, playlist selection, the
// simulated playback clock, the Space / S / F / Esc keys) is untouched:
// this is a visual restyle. The stage interior is still a flat fill so a
// frame blitter can overwrite it pixel-for-pixel.
//
// The design was drawn first as HTML/CSS (audglass-spec.html, kept with the
// proof PNGs) and this file ports that spec's geometry and tokens.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_style.h"    // gui_fill_rounded_aa / gui_rounded_border / gui_soft_shadow / gui_fill_circle_aa / gui_glass_backdrop_*

// All in-window text goes through the antialiased TrueType path
// (SYS_WIN_DRAW_TTF, size packed in the top byte of the colour by the libc
// wrapper). Sizes are TTF pixel sizes, one per role (glass doc section 5).
// FACE BYTES ARE ASCII: the TTF path maps each BYTE to a codepoint (no UTF-8
// decoding, no CP437 table, see the calcglass blame entry), so every glyph
// that is not plain text (transport icons, the speaker) is STROKED below.
#define TTF_TAB     12   // mode pills, playlist track names, stage status
#define TTF_SMALL   11   // times, playlist numbers / durations, eyebrow, position
#define TTF_TITLE   18   // stage track title

static int g_win_w = 640, g_win_h = 440;  // #89: live window size (EVENT_RESIZE)
#define WIN_W g_win_w
#define WIN_H g_win_h
#define MIN_WIN_W 420   // floor so the controls row never overlaps itself
#define MIN_WIN_H 300

// (audglass) ONE definition of every band, shared by the draw and hit-test
// paths (glass doc section 8: "one definition for anything three places agree
// about"). PAD / TAB_H / TAB_W / TAB_GAP / PANEL_R / PANEL_IN match the Task
// Manager's logic.rs and the Calculator so the windows share one geometry.
#define PAD       10                       // window margin: the backdrop shows here
#define TAB_H     26                       // mode pill height (radius TAB_H/2)
#define TAB_W     96
#define TAB_GAP   6
#define DISP_Y    (PAD + TAB_H + 10)       // stage + playlist panel top (46)
#define PANEL_R   12                       // panel corner radius
#define PANEL_IN  12                       // inset from a panel edge to its content
#define GAP       8                        // between the stage row and the controls panel
#define CTRL_H    86                       // controls panel height
#define PL_W      188                      // playlist panel width
#define ROW_H     26                       // playlist row pitch
#define LIST_TOP  40                       // first row, from the playlist panel top
#define SEEK_H    6                        // seek / volume track height (radius 3)
#define KNOB_D    12                       // seek knob diameter
#define VKNOB_D   10                       // volume knob diameter
#define BTN_D     36                       // prev / stop / next diameter
#define PLAY_D    40                       // play / pause diameter (the primary)
#define BTN_GAP   10
#define VOL_W     90
#define TIME_W    40                       // "MM:SS" column either side of the seek track

// (audglass) Glass tokens: docs/UI_GLASS_DESIGN_SYSTEM.md section 1, the
// exact names and values the Task Manager and Calculator use, so the windows
// read as one material side by side. Copy the NAMES as well as the values; a
// second name for the same hex is how two surfaces drift apart.
#define C_PANEL       0x00122420   // WEL_BG_MID: panel fill, the outer colour every AA edge blends toward
#define C_CARD        0x000E1D1B   // DK_CARD_FILL: transport buttons, disc body
#define C_HALO        0x00050A09   // DK_THUMB_HALO: the stage "screen" (the live blit area)
#define C_TRACK       0x0016241F   // DK_PROGRESS_TRACK: seek + volume track
#define C_EDGE        0x002C4A44   // DK_STROKE_UNSEL: panel border, card borders
#define C_HAIR        0x001E322E   // hairline rule under the playlist header
#define C_INK         0x00F3FBF9   // DK_HEADLINE: title, times, track names, knobs
#define C_INK_DIM     0x00A9D9CC   // DK_BODY: status, numbers, durations, pill labels
#define C_ACCENT      0x006AE2CF   // DK_ACCENT: selected pill, seek fill, hub, eyebrow
#define C_ACCENT_INK  0x0004231A   // text on the accent
#define C_BTN_TOP     0x000F8068   // DK_BTN_TOP: play button gradient top (section 6 primary button)
#define C_BTN_BOTTOM  0x000A5D4C   // DK_BTN_BOTTOM: play button gradient bottom
#define C_BTN_INK     0x00FFFFFF   // DK_BTN_TEXT: the glyph on the play button
#define WEL_BG_TOP    0x000A1614   // backdrop gradient fallback, top stop
#define WEL_BG_BOTTOM 0x00050A09   // backdrop gradient fallback, bottom stop

// Playback state
typedef enum {
    STATE_STOPPED,
    STATE_PLAYING,
    STATE_PAUSED
} playback_state_t;

// Playlist item
typedef struct {
    char name[64];
    char path[256];
    int duration_secs;
} playlist_item_t;

#define MAX_PLAYLIST 32

// State
static int win = -1;
static playback_state_t state = STATE_STOPPED;
static int current_track = 0;
static int current_time = 0;  // seconds
static int volume = 80;       // 0-100
static int fullscreen = 0;    // Cinema toggle (hides playlist)
static playlist_item_t playlist[MAX_PLAYLIST];
static int playlist_count = 0;
static int hover_button = -1;
static int hover_tab = -1;    // 0 Player, 1 Cinema, -1 none
static int hover_row = -1;    // hovered playlist index, -1 none
static int playlist_scroll = 0;

// Transport buttons
#define BTN_PREV    0
#define BTN_PLAY    1
#define BTN_STOP    2
#define BTN_NEXT    3
#define BTN_COUNT   4

// ---------------------------------------------------------------------------
// (glasslib) The frosted-wallpaper backdrop: now the shared libc recipe
// (userland/libc/gui_style.h gui_glass_backdrop_*), consolidated out of this
// file, the Task Manager, the Calculator and the Image Viewer, which all
// carried an identical copy (blame.md tmglass/calcglass/audglass/imgglass).
// This app owns only the two small persistent pieces the API asks for; the
// scratch (raw thumbnail bytes, blur temp plane) lives in gui.c.
// ---------------------------------------------------------------------------
static uint32_t g_bd[GUI_GLASS_BD_W * GUI_GLASS_BD_H];
static int g_bd_wi = GUI_GLASS_BD_NEVER;   // wallpaper index the backdrop was built for
static int g_chrome_dirty = 1;             // the ONLY thing that can make draw_all() blit the backdrop

// The ONE self-committing call in this app (see draw_all()): SYS_WIN_BLIT
// copies the backdrop into the window content scaled to the content rect
// (x/y are ignored by the kernel) and publishes that frame on its own.
static void bd_blit(int h){
    gui_glass_backdrop_blit(h, g_bd);
}

// The backdrop colour under content pixel (x, y): what the kernel's nearest-
// neighbour scale put there, to within the blur. Every AA edge and shadow
// drawn onto the backdrop takes its outer colour from here; a flat guess is
// what produces a square halo around a round corner (glass doc section 2).
static uint32_t bd_at(int x, int y){
    return gui_glass_backdrop_at(g_bd, WIN_W, WIN_H, x, y);
}

// Rebuild the backdrop if the wallpaper changed. SYS_GET_WALLPAPER is the
// same poll the compositor makes; a changed index marks the chrome dirty.
static void sync_backdrop(void){
    if (gui_glass_backdrop_sync(g_bd, &g_bd_wi, C_PANEL, 158, WEL_BG_TOP, WEL_BG_BOTTOM))
        g_chrome_dirty = 1;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Initialize demo playlist
static void init_demo_playlist(void) {
    const char *demos[] = {
        "Track 01 - Opening",
        "Track 02 - Theme",
        "Track 03 - Battle",
        "Track 04 - Victory",
        "Track 05 - Credits"
    };

    playlist_count = 5;
    for (int i = 0; i < playlist_count; i++) {
        int j = 0;
        while (demos[i][j] && j < 63) {
            playlist[i].name[j] = demos[i][j];
            j++;
        }
        playlist[i].name[j] = '\0';
        playlist[i].path[0] = '\0';
        playlist[i].duration_secs = 180 + i * 30;  // 3-4 minutes each
    }
}

// Format time as MM:SS
static void format_time(int secs, char *buf) {
    int m = secs / 60;
    int s = secs % 60;
    buf[0] = '0' + (m / 10);
    buf[1] = '0' + (m % 10);
    buf[2] = ':';
    buf[3] = '0' + (s / 10);
    buf[4] = '0' + (s % 10);
    buf[5] = '\0';
}

static int cur_duration(void) {
    if (playlist_count > 0 && current_track < playlist_count)
        return playlist[current_track].duration_secs;
    return 0;
}

// ---------------------------------------------------------------------------
// Geometry (one definition each; draw and hit-test both call these)
// ---------------------------------------------------------------------------

// Mode pill i (0 = Player, 1 = Cinema).
static void tab_rect(int i, int *x, int *y, int *w, int *h){
    *x = PAD + i * (TAB_W + TAB_GAP); *y = PAD; *w = TAB_W; *h = TAB_H;
}

// Height shared by the stage and playlist panels.
static int stage_row_h(void){
    int h = WIN_H - DISP_Y - GAP - CTRL_H - PAD;
    return h < 40 ? 40 : h;
}

// The stage panel (shrinks to leave room for the playlist unless Cinema).
static void stage_rect(int *x, int *y, int *w, int *h){
    *x = PAD; *y = DISP_Y;
    *w = fullscreen ? WIN_W - 2 * PAD : WIN_W - 2 * PAD - PL_W - GAP;
    *h = stage_row_h();
}

static void playlist_rect(int *x, int *y, int *w, int *h){
    *x = WIN_W - PAD - PL_W; *y = DISP_Y; *w = PL_W; *h = stage_row_h();
}

static void ctrl_rect(int *x, int *y, int *w, int *h){
    *x = PAD; *y = WIN_H - PAD - CTRL_H; *w = WIN_W - 2 * PAD; *h = CTRL_H;
}

// Seek track: TIME_W + 8 in from either side of the controls panel content.
static void seek_rect(int *x, int *y, int *w, int *h){
    int cx, cy, cw, ch; ctrl_rect(&cx, &cy, &cw, &ch);
    *x = cx + PANEL_IN + TIME_W + 8; *y = cy + 14;
    *w = cw - 2 * (PANEL_IN + TIME_W + 8); *h = SEEK_H;
    if (*w < 20) *w = 20;
}

// Transport button i: a BTN_D circle (PLAY_D for play), the group centred in
// the panel on the row at cy + 34.
static void btn_rect(int i, int *x, int *y, int *d){
    int cx, cy, cw, ch; ctrl_rect(&cx, &cy, &cw, &ch);
    int total = 3 * BTN_D + PLAY_D + 3 * BTN_GAP;
    int sx = cx + (cw - total) / 2;
    int row = cy + 34;
    int px = sx;
    for (int k = 0; k < i; k++) px += (k == BTN_PLAY ? PLAY_D : BTN_D) + BTN_GAP;
    if (i == BTN_PLAY){ *x = px; *y = row; *d = PLAY_D; }
    else              { *x = px; *y = row + (PLAY_D - BTN_D) / 2; *d = BTN_D; }
}

// Volume track, left of the transport row (after the speaker glyph).
static void vol_rect(int *x, int *y, int *w, int *h){
    int cx, cy, cw, ch; ctrl_rect(&cx, &cy, &cw, &ch);
    *x = cx + PANEL_IN + 22; *y = cy + 34 + PLAY_D / 2 - SEEK_H / 2; *w = VOL_W; *h = SEEK_H;
}

// Playlist row r (0-based visible row) at its panel.
static void row_rect(int r, int *x, int *y, int *w, int *h){
    int px, py, pw, ph; playlist_rect(&px, &py, &pw, &ph);
    *x = px + PANEL_IN; *y = py + LIST_TOP + r * ROW_H; *w = pw - 2 * PANEL_IN; *h = ROW_H - 1;
}

static int visible_rows(void){
    int px, py, pw, ph; playlist_rect(&px, &py, &pw, &ph);
    int avail = ph - LIST_TOP - PANEL_IN;
    return avail > 0 ? avail / ROW_H : 0;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
// Centred TTF label. The rasterizer's y is the top of the LINE box, so the
// label is centred on the control by its nominal size (glass doc section 5).
static void text_center(int x, int y, int w, int h, const char *s, int size, uint32_t color){
    int tw = gui_ttf_width(s, size);
    win_draw_text_ttf(win, x + (w - tw) / 2, y + (h - size) / 2 - 1, s, size, color);
}
static void text_right(int right, int y, const char *s, int size, uint32_t color){
    win_draw_text_ttf(win, right - gui_ttf_width(s, size), y, s, size, color);
}

// (audglass) One glass panel: soft shadow, AA rounded fill, 1px border, 1px
// top highlight; the same layering the Task Manager's and Calculator's
// draw_panel() use. Every outer colour is sampled from the backdrop under
// that edge, so the fringe and corners match what the blit put there. Drawn
// EVERY frame on purpose: the same inputs give the same pixels, so the
// repaint is idempotent (section 11: only the backdrop blit is a commit).
static void draw_panel(int x, int y, int w, int h){
    uint32_t below = bd_at(x + w / 2, y + h + 3);
    uint32_t c0 = bd_at(x + 4, y + 4),     c1 = bd_at(x + w - 4, y + 4),
             c2 = bd_at(x + 4, y + h - 4), c3 = bd_at(x + w - 4, y + h - 4);
    uint32_t outer = 0;
    for (int sh = 0; sh <= 16; sh += 8){
        uint32_t m = (((c0 >> sh) & 0xFF) + ((c1 >> sh) & 0xFF) + ((c2 >> sh) & 0xFF) + ((c3 >> sh) & 0xFF)) / 4;
        outer |= m << sh;
    }
    gui_soft_shadow(win, x, y + 2, w, h, PANEL_R, below);
    gui_fill_rounded_aa(win, x, y, w, h, PANEL_R, C_PANEL, outer);
    gui_rounded_border(win, x, y, w, h, PANEL_R, C_EDGE);
    win_draw_rect(win, x + PANEL_R, y + 1, w - 2 * PANEL_R, 1, gui_lighten(C_PANEL, 16));
}

// Stroked glyphs (glass doc section 6: "stroked, not typed"). A horizontal-
// pointing solid triangle stamped as rows: (x, y) is its bounding box's top
// left, h its height (odd looks best), dir +1 points right, -1 points left.
static void tri_h(int x, int y, int h, int dir, uint32_t col){
    int half = h / 2;
    int wmax = half + 1;
    for (int r = 0; r < h; r++){
        int d = r <= half ? r : h - 1 - r;   // 0 at the tips, half at the middle
        int len = d + 1;
        if (len > wmax) len = wmax;
        int rx = dir > 0 ? x : x + wmax - len;
        win_draw_rect(win, rx, y + r, len, 1, col);
    }
}

// The transport glyph for button i, centred at (cx, cy).
static void draw_glyph(int i, int cx, int cy, uint32_t col){
    switch (i){
        case BTN_PREV:   // bar + left-pointing triangle
            win_draw_rect(win, cx - 8, cy - 6, 2, 13, col);
            tri_h(cx - 5, cy - 6, 13, -1, col);
            break;
        case BTN_PLAY:
            if (state == STATE_PLAYING){   // pause: two bars
                win_draw_rect(win, cx - 6, cy - 7, 4, 15, col);
                win_draw_rect(win, cx + 2, cy - 7, 4, 15, col);
            } else {                       // play: right-pointing triangle, nudged right to sit optically centred
                tri_h(cx - 5, cy - 7, 15, 1, col);
            }
            break;
        case BTN_STOP:   // square
            win_draw_rect(win, cx - 6, cy - 6, 12, 12, col);
            break;
        case BTN_NEXT:   // right-pointing triangle + bar
            tri_h(cx - 8, cy - 6, 13, 1, col);
            win_draw_rect(win, cx + 6, cy - 6, 2, 13, col);
            break;
    }
}

// Small speaker mark (body + cone), centred at (cx, cy).
static void draw_speaker(int cx, int cy, uint32_t col){
    win_draw_rect(win, cx - 6, cy - 2, 4, 5, col);
    tri_h(cx - 2, cy - 5, 11, -1, col);
    // two sound arcs, as short vertical ticks
    win_draw_rect(win, cx + 5, cy - 3, 1, 7, col);
    win_draw_rect(win, cx + 7, cy - 5, 1, 11, col);
}

// (audglass) The mode pills: Player / Cinema, sitting directly on the frosted
// backdrop. Selected = accent fill with the accent ink; the other = panel-
// coloured glass with the panel border (hover lightens the fill). Each pill's
// AA edge takes the backdrop colour at its own centre, and the rectangle is
// tab_rect(), the one the click handler tests.
static void draw_tabs(void){
    static const char *names[2] = {"Player", "Cinema"};
    for (int i = 0; i < 2; i++){
        int x, y, w, h; tab_rect(i, &x, &y, &w, &h);
        int sel = (fullscreen == i);
        uint32_t outer = bd_at(x + w / 2, y + h / 2);
        uint32_t fill = sel ? C_ACCENT : (hover_tab == i ? gui_lighten(C_PANEL, 18) : C_PANEL);
        gui_fill_rounded_aa(win, x, y, w, h, h / 2, fill, outer);
        if (!sel) gui_rounded_border(win, x, y, w, h, h / 2, C_EDGE);
        text_center(x, y, w, h, names[i], TTF_TAB, sel ? C_ACCENT_INK : C_INK_DIM);
    }
}

// The stage: a glass panel holding the "screen", a nested rounded card in the
// halo colour that is the live blit area (flat fill, so a frame blitter can
// overwrite it), with the disc motif, the track title and the state on it.
static void draw_display(void){
    int x, y, w, h; stage_rect(&x, &y, &w, &h);
    draw_panel(x, y, w, h);

    int sx = x + PANEL_IN, sy = y + PANEL_IN, sw = w - 2 * PANEL_IN, sh = h - 2 * PANEL_IN;
    gui_fill_rounded_aa(win, sx, sy, sw, sh, 6, C_HALO, C_PANEL);
    gui_rounded_border(win, sx, sy, sw, sh, 6, C_EDGE);

    int cx = sx + sw / 2, cy = sy + sh / 2;
    int have = (playlist_count > 0 && current_track < playlist_count);

    // Disc: ring, body, hub (accent while playing), spindle hole.
    if (sh >= 140){
        int dy = cy - 30;
        gui_fill_circle_aa(win, cx - 36, dy - 36, 72, C_EDGE, C_HALO);
        gui_fill_circle_aa(win, cx - 35, dy - 35, 70, C_CARD, C_EDGE);
        gui_fill_circle_aa(win, cx - 8,  dy - 8,  16, state == STATE_PLAYING ? C_ACCENT : C_INK_DIM, C_CARD);
        gui_fill_circle_aa(win, cx - 3,  dy - 3,  6,  C_CARD, state == STATE_PLAYING ? C_ACCENT : C_INK_DIM);
    }
    int ty = sh >= 140 ? cy + 14 : cy - 20;
    if (have) {
        text_center(sx, ty, sw, TTF_TITLE, playlist[current_track].name, TTF_TITLE, C_INK);
        const char *status_str = state == STATE_PLAYING ? "Now Playing" :
                                  state == STATE_PAUSED ? "Paused" : "Stopped";
        text_center(sx, ty + 26, sw, TTF_TAB, status_str, TTF_TAB, C_INK_DIM);
    } else {
        text_center(sx, ty, sw, TTF_TITLE, "No media loaded", TTF_TITLE, C_INK_DIM);
    }
}

// A rounded track with an accent fill to `value`/`max` and an ink knob.
static void draw_track(int x, int y, int w, int h, int value, int max, int knob_d){
    if (max < 1) max = 1;
    if (value < 0) value = 0;
    if (value > max) value = max;
    int fx = w * value / max;
    gui_fill_rounded_aa(win, x, y, w, h, h / 2, C_TRACK, C_PANEL);
    if (fx > h) gui_fill_rounded_aa(win, x, y, fx, h, h / 2, C_ACCENT, C_TRACK);
    int kx = x + fx - knob_d / 2;
    if (kx < x) kx = x;
    if (kx > x + w - knob_d) kx = x + w - knob_d;
    gui_fill_circle_aa(win, kx, y + h / 2 - knob_d / 2, knob_d, C_INK, fx > h ? C_ACCENT : C_TRACK);
}

// The controls panel: seek row, transport row, volume, track position.
static void draw_controls(void){
    int cx, cy, cw, ch; ctrl_rect(&cx, &cy, &cw, &ch);
    draw_panel(cx, cy, cw, ch);

    // ---- Seek row (time / track / duration) ----
    char time_str[16], dur_str[16];
    int duration = cur_duration();
    format_time(current_time, time_str);
    format_time(duration, dur_str);
    int sx, sy, sw, sh; seek_rect(&sx, &sy, &sw, &sh);
    win_draw_text_ttf(win, cx + PANEL_IN, cy + 10, time_str, TTF_SMALL, C_INK_DIM);
    draw_track(sx, sy, sw, sh, current_time, duration, KNOB_D);
    text_right(cx + cw - PANEL_IN, cy + 10, dur_str, TTF_SMALL, C_INK_DIM);

    // ---- Transport row ----
    for (int i = 0; i < BTN_COUNT; i++){
        int bx, by, d; btn_rect(i, &bx, &by, &d);
        int hov = (hover_button == i);
        if (i == BTN_PLAY){
            uint32_t top = C_BTN_TOP, bot = C_BTN_BOTTOM;
            if (hov){ top = gui_lighten(top, 18); bot = gui_lighten(bot, 18); }
            gui_fill_rounded_aa(win, bx, by, d, d, d / 2, bot, C_PANEL);
            gui_fill_rounded_grad(win, bx + 1, by + 1, d - 2, d - 2, d / 2 - 1, top, bot);
            draw_glyph(i, bx + d / 2, by + d / 2, C_BTN_INK);
        } else {
            uint32_t fill = hov ? gui_lighten(C_CARD, 18) : C_CARD;
            gui_fill_circle_aa(win, bx, by, d, C_EDGE, C_PANEL);
            gui_fill_circle_aa(win, bx + 1, by + 1, d - 2, fill, C_EDGE);
            draw_glyph(i, bx + d / 2, by + d / 2, C_INK);
        }
    }

    // ---- Volume (left of the transport row) ----
    int vx, vy, vw, vh; vol_rect(&vx, &vy, &vw, &vh);
    draw_speaker(cx + PANEL_IN + 6, vy + vh / 2, C_INK_DIM);
    draw_track(vx, vy, vw, vh, volume, 100, VKNOB_D);

    // ---- Track position (right of the transport row) ----
    if (playlist_count > 0){
        char pos[32];
        snprintf(pos, sizeof(pos), "Track %d of %d", current_track + 1, playlist_count);
        text_right(cx + cw - PANEL_IN, vy + vh / 2 - TTF_SMALL / 2 - 1, pos, TTF_SMALL, C_INK_DIM);
    }
}

// The playlist panel: eyebrow, count, hairline, then rows in the Task
// Manager's list grammar (accent band for the selection, alternate rows
// lightened by 4, hover by 12).
static void draw_playlist(void){
    if (fullscreen) return;  // Cinema hides the playlist column

    int px, py, pw, ph; playlist_rect(&px, &py, &pw, &ph);
    draw_panel(px, py, pw, ph);

    win_draw_text_ttf(win, px + PANEL_IN, py + PANEL_IN, "PLAYLIST", TTF_SMALL, C_ACCENT);
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "%d track%s", playlist_count, playlist_count == 1 ? "" : "s");
    text_right(px + pw - PANEL_IN, py + PANEL_IN, cnt, TTF_SMALL, C_INK_DIM);
    win_draw_rect(win, px + PANEL_IN, py + LIST_TOP - 6, pw - 2 * PANEL_IN, 1, C_HAIR);

    int rows = visible_rows();
    for (int r = 0; r < rows && r + playlist_scroll < playlist_count; r++){
        int idx = r + playlist_scroll;
        int rx, ry, rw, rh; row_rect(r, &rx, &ry, &rw, &rh);
        int sel = (idx == current_track);
        uint32_t band = sel ? C_ACCENT :
                        hover_row == idx ? gui_lighten(C_PANEL, 12) :
                        (r & 1) ? gui_lighten(C_PANEL, 4) : C_PANEL;
        if (band != C_PANEL) gui_fill_rounded_aa(win, rx, ry, rw, rh, 4, band, C_PANEL);
        uint32_t ink = sel ? C_ACCENT_INK : C_INK;
        uint32_t dim = sel ? C_ACCENT_INK : C_INK_DIM;

        char num[4];
        num[0] = '0' + ((idx + 1) / 10);
        num[1] = '0' + ((idx + 1) % 10);
        num[2] = '\0';
        win_draw_text_ttf(win, rx + 8, ry + (rh - TTF_SMALL) / 2 - 1, num, TTF_SMALL, dim);

        char dur[8]; format_time(playlist[idx].duration_secs, dur);
        const char *dshow = dur[0] == '0' ? dur + 1 : dur;   // "3:00" not "03:00"
        int dw = gui_ttf_width(dshow, TTF_SMALL);
        win_draw_text_ttf(win, rx + rw - 8 - dw, ry + (rh - TTF_SMALL) / 2 - 1, dshow, TTF_SMALL, dim);

        // Track name, truncated by MEASURED width to the room between the
        // number and the duration (a fixed character count was the old rule,
        // and a proportional face has no fixed character width).
        int avail = rw - 34 - 8 - dw - 6;
        char name[64];
        int n = 0;
        while (playlist[idx].name[n] && n < 63){ name[n] = playlist[idx].name[n]; n++; }
        name[n] = '\0';
        if (gui_ttf_width(name, TTF_TAB) > avail){
            while (n > 1 && gui_ttf_width(name, TTF_TAB) > avail){
                n--; name[n] = '\0';
                if (n >= 2){ name[n - 2] = '.'; name[n - 1] = '.'; }
            }
        }
        win_draw_text_ttf(win, rx + 34, ry + (rh - TTF_TAB) / 2 - 1, name, TTF_TAB, ink);
    }
}

// Full redraw
static void draw_all(void) {
    // Re-sync the live content size from the compositor every frame (same
    // idiom as calc/devmgr/taskmanager), so the panels reflow to the size
    // the window was actually granted.
    { int w = g_win_w, h = g_win_h;
      win_get_size(win, &w, &h);
      if (w < MIN_WIN_W) w = g_win_w > 0 ? g_win_w : MIN_WIN_W;
      if (h < MIN_WIN_H) h = g_win_h > 0 ? g_win_h : MIN_WIN_H;
      g_win_w = w; g_win_h = h;
    }
    // (audglass) THE ANTI-FLASH CONTRACT (docs/UI_GLASS_DESIGN_SYSTEM.md
    // section 11). SYS_WIN_BLIT self-commits: the kernel publishes the window
    // the instant the backdrop lands, and a compositor sample taken between
    // that commit and the win_invalidate() below would show a backdrop with
    // no content on it. So the blit runs ONLY when the chrome is dirty (start,
    // EVENT_RESIZE, EVENT_REDRAW, wallpaper change, the Cinema toggle, which
    // uncovers margin that the playlist panel used to cover), never on a
    // hover, a keystroke or the one-second playback tick. Everything else is
    // plain draws, which accumulate unpublished until the single invalidate
    // at the end. The window is never cleared with a flat fill: the panels
    // and pills cover every pixel that changes, and the margins are the
    // backdrop.
    sync_backdrop();
    if (g_chrome_dirty){
        bd_blit(win);
        g_chrome_dirty = 0;
    }
    draw_tabs();
    draw_display();
    draw_playlist();
    draw_controls();
    win_invalidate(win);
}

// ---------------------------------------------------------------------------
// Hit testing (the same rect functions the draw path uses)
// ---------------------------------------------------------------------------
static int in_rect(int lx, int ly, int x, int y, int w, int h){
    return lx >= x && lx < x + w && ly >= y && ly < y + h;
}

static int hit_tab(int lx, int ly){
    for (int i = 0; i < 2; i++){
        int x, y, w, h; tab_rect(i, &x, &y, &w, &h);
        if (in_rect(lx, ly, x, y, w, h)) return i;
    }
    return -1;
}

// Transport buttons hit-test on their bounding square: a circle's corners are
// a few pixels, and a click there is meant for the button.
static int hit_button(int lx, int ly){
    for (int i = 0; i < BTN_COUNT; i++){
        int x, y, d; btn_rect(i, &x, &y, &d);
        if (in_rect(lx, ly, x, y, d, d)) return i;
    }
    return -1;
}

static int hit_row(int lx, int ly){
    if (fullscreen) return -1;
    int rows = visible_rows();
    for (int r = 0; r < rows && r + playlist_scroll < playlist_count; r++){
        int x, y, w, h; row_rect(r, &x, &y, &w, &h);
        if (in_rect(lx, ly, x, y, w, h)) return r + playlist_scroll;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

// Handle button click
static void handle_button(int btn) {
    switch (btn) {
        case BTN_PREV:
            if (current_track > 0) {
                current_track--;
                current_time = 0;
            } else if (current_time > 3) {
                current_time = 0;
            }
            break;

        case BTN_PLAY:
            if (state == STATE_PLAYING) {
                state = STATE_PAUSED;
            } else {
                state = STATE_PLAYING;
            }
            break;

        case BTN_STOP:
            state = STATE_STOPPED;
            current_time = 0;
            break;

        case BTN_NEXT:
            if (current_track < playlist_count - 1) {
                current_track++;
                current_time = 0;
            }
            break;
    }
}

// The Cinema toggle (pill, or the F key): the playlist panel disappears and
// the stage widens, so margin that was covered is uncovered. That is a
// chrome change and needs the backdrop blitted again.
static void set_cinema(int on){
    if (fullscreen == on) return;
    fullscreen = on;
    hover_row = -1;
    g_chrome_dirty = 1;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Create window
    win = win_create("Media Player", 100, 60, WIN_W, WIN_H);
    if (win < 0) {
        printf("Failed to create window\n");
        return 1;
    }

    printf("Media Player window created (handle=%d)\n", win);

    // Initialize demo playlist
    init_demo_playlist();

    // (audglass) the palette is the fixed dark glass (see the token block), so
    // there is no theme poll: the window carries its own backdrop. draw_all()
    // starts with g_chrome_dirty set, so the first frame blits the backdrop.
    draw_all();

    // Event loop
    gui_event_t event;
    int running = 1;
    uint64_t last_tick = sys_clock();

    while (running) {
        int event_type = win_get_event(win, &event, 100);

        // Simulate playback progress
        uint64_t now = sys_clock();
        if (state == STATE_PLAYING && now - last_tick >= 1000) {
            last_tick = now;
            current_time++;

            // Check for track end
            if (playlist_count > 0 && current_track < playlist_count) {
                if (current_time >= playlist[current_track].duration_secs) {
                    if (current_track < playlist_count - 1) {
                        current_track++;
                        current_time = 0;
                    } else {
                        state = STATE_STOPPED;
                        current_time = 0;
                    }
                }
            }

            // The tick repaints the whole frame with plain draws (no blit):
            // the stage's state line, the seek row and the playlist
            // selection can all change on a track boundary.
            draw_all();
        }

        if (event_type == 0) continue;

        switch (event.type) {
            case EVENT_RESIZE:
                if (event.mouse_x > 0 && event.mouse_y > 0) { g_win_w = event.mouse_x; g_win_h = event.mouse_y; }
                g_chrome_dirty = 1;
                draw_all();
                break;
            case EVENT_REDRAW:
                g_chrome_dirty = 1;
                draw_all();
                break;

            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_KEY_DOWN:
                if (event.key_char == 27) {
                    running = 0;
                } else if (event.key_char == ' ') {
                    handle_button(BTN_PLAY);
                    draw_all();
                } else if (event.key_char == 's' || event.key_char == 'S') {
                    handle_button(BTN_STOP);
                    draw_all();
                } else if (event.key_char == 'f' || event.key_char == 'F') {
                    set_cinema(!fullscreen);
                    draw_all();
                }
                break;

            case EVENT_MOUSE_DOWN:
                if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    int lx = event.mouse_x;
                    int ly = event.mouse_y;

                    int t = hit_tab(lx, ly);
                    if (t >= 0) { set_cinema(t); draw_all(); break; }

                    int b = hit_button(lx, ly);
                    if (b >= 0) { handle_button(b); draw_all(); break; }

                    // Seek track (generous vertical hit zone)
                    int sx, sy, sw, sh; seek_rect(&sx, &sy, &sw, &sh);
                    if (in_rect(lx, ly, sx - KNOB_D / 2, sy - 8, sw + KNOB_D, sh + 16)) {
                        int dur = cur_duration();
                        if (dur > 0) {
                            int rel = lx - sx;
                            if (rel < 0) rel = 0;
                            if (rel > sw) rel = sw;
                            current_time = (rel * dur) / (sw > 0 ? sw : 1);
                            draw_all();
                        }
                        break;
                    }

                    // Volume track
                    int vx, vy, vw, vh; vol_rect(&vx, &vy, &vw, &vh);
                    if (in_rect(lx, ly, vx - VKNOB_D / 2, vy - 8, vw + VKNOB_D, vh + 16)) {
                        int rel = lx - vx;
                        if (rel < 0) rel = 0;
                        if (rel > vw) rel = vw;
                        volume = (rel * 100) / (vw > 0 ? vw : 1);
                        draw_all();
                        break;
                    }

                    // Playlist row
                    int r = hit_row(lx, ly);
                    if (r >= 0) {
                        current_track = r;
                        current_time = 0;
                        draw_all();
                    }
                }
                break;

            case EVENT_MOUSE_MOVE:
                {
                    int lx = event.mouse_x;
                    int ly = event.mouse_y;
                    int nb = hit_button(lx, ly);
                    int nt = hit_tab(lx, ly);
                    int nr = hit_row(lx, ly);
                    if (nb != hover_button || nt != hover_tab || nr != hover_row) {
                        hover_button = nb; hover_tab = nt; hover_row = nr;
                        draw_all();
                    }
                }
                break;

            default:
                break;
        }
    }

    win_destroy(win);
    printf("Media Player closed\n");

    return 0;
}
