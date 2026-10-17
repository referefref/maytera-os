// mediaplayer - Media Player for MayteraOS (user-space version)
// Audio/video playback with controls and playlist.
// UI modernized to the shared libc style engine (gui_style.h), matching the
// look of the Settings and Files apps: theme-aware palette, rounded cards,
// gui_button / gui_slider chrome, and antialiased TrueType typography.
// Visual / layout uplift only: the playback state machine, keymap, event
// wiring, and the video/display surface blit region are all preserved.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"

// Route in-window text through the antialiased TrueType path (matches Settings).
#define mp_text(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

static int g_win_w = 640, g_win_h = 440;  // #89: live window size (EVENT_RESIZE)
#define WIN_W g_win_w
#define WIN_H g_win_h

// Outer layout padding and gaps (matches Settings spacing tokens).
#define MP_PAD       12
#define MP_GAP       8

#define PLAYLIST_W   188
#define CONTROLS_H   86

// Control geometry
#define BTN_SIZE     40
#define BTN_GAP      8
#define SEEK_H       16
#define VOL_W        90

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
static int fullscreen = 0;    // letterbox/cinema toggle (hides playlist)
static playlist_item_t playlist[MAX_PLAYLIST];
static int playlist_count = 0;
static int hover_button = -1;
static int playlist_scroll = 0;

// Cached palette colors for the surrounding chrome (filled from active theme).
static uint32_t COL_WINDOW_BG;
static uint32_t COL_SURFACE;
static uint32_t COL_CARD;
static uint32_t COL_INK;
static uint32_t COL_INK_DIM;
static uint32_t COL_ACCENT;
static uint32_t COL_BORDER;
static uint32_t COL_LETTERBOX;

// Button positions
#define BTN_PREV    0
#define BTN_PLAY    1
#define BTN_STOP    2
#define BTN_NEXT    3
#define BTN_COUNT   4

typedef struct {
    int x, y, w, h;
    const char *label;
} button_t;

static button_t buttons[BTN_COUNT];

// Hit-test rectangles for the seek slider, volume slider, and fullscreen button.
static int seek_x, seek_y, seek_w;
static int vol_x, vol_y, vol_w;
static int fs_x, fs_y, fs_w, fs_h;

// ---------------------------------------------------------------------------
// Theme / style setup
// ---------------------------------------------------------------------------

// Pull the active kernel theme colors into a gui_palette_t and push them into
// the shared style engine so every gui_* primitive renders in the system theme,
// exactly like the Settings app does. The renderer family is CLASSIC for the
// kernel's beveled CDE theme (id 4) and MODERN otherwise.
static void load_theme_palette(void) {
    int active = theme_get_active();

    COL_WINDOW_BG = theme_color(THEME_COLOR_WINDOW_BG);
    COL_SURFACE   = theme_color(THEME_COLOR_BACKGROUND);
    COL_INK       = theme_color(THEME_COLOR_FOREGROUND);
    COL_ACCENT    = theme_color(THEME_COLOR_ACCENT);
    COL_BORDER    = theme_color(THEME_COLOR_WINDOW_BORDER);

    // Derive the remaining tokens from the base colors.
    COL_CARD     = gui_mix(COL_SURFACE, COL_INK, 10);
    COL_INK_DIM  = gui_mix(COL_INK, COL_SURFACE, 110);
    // The video/audio stage is a deep neutral letterbox; bias slightly toward
    // black for a cinema feel regardless of theme.
    COL_LETTERBOX = gui_mix(COL_SURFACE, 0x00000000, 170);

    gui_set_style(active == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);

    gui_palette_t pal;
    pal.surface        = COL_SURFACE;
    pal.surface_raised = COL_CARD;
    pal.ink            = COL_INK;
    pal.ink_dim        = COL_INK_DIM;
    pal.accent         = COL_ACCENT;
    pal.accent_hover   = gui_lighten(COL_ACCENT, 18);
    pal.border         = COL_BORDER;
    pal.field_bg       = gui_mix(COL_SURFACE, 0x00000000, 60);
    pal.field_border   = COL_BORDER;
    pal.track          = gui_mix(COL_SURFACE, COL_INK, 30);
    gui_set_palette(&pal);
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

// Current width of the display column (shrinks to leave room for the playlist
// unless fullscreen/cinema mode hides it).
static int display_col_w(void) {
    if (fullscreen) return WIN_W - 2 * MP_PAD;
    return WIN_W - 2 * MP_PAD - PLAYLIST_W - MP_GAP;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Draw the video / album-art stage. This is the playback render surface: only
// the surrounding letterbox + framing is restyled here. The interior rect at
// (stage_ix, stage_iy, stage_iw, stage_ih) is the live blit area where decoded
// frames / track info are drawn, and it is left as a flat fill so a frame
// blitter can overwrite it pixel-for-pixel without interference.
static void draw_display(void) {
    int x = MP_PAD;
    int y = MP_PAD;
    int w = display_col_w();
    int h = WIN_H - 2 * MP_PAD - CONTROLS_H - MP_GAP;

    // Outer framed card around the stage.
    gui_card(win, x, y, w, h);

    // Inner letterbox surface (the live video blit area).
    int stage_ix = x + 6;
    int stage_iy = y + 6;
    int stage_iw = w - 12;
    int stage_ih = h - 12;
    int r = (gui_active_style().base == GUI_STYLE_MODERN) ? 4 : 0;
    gui_fill_rounded_aa(win, stage_ix, stage_iy, stage_iw, stage_ih, r,
                        COL_LETTERBOX, COL_CARD);

    // Centered now-playing / track info overlay on the stage.
    uint32_t ink_on_stage = gui_ink_on(COL_LETTERBOX);
    uint32_t dim_on_stage = gui_mix(ink_on_stage, COL_LETTERBOX, 90);

    int cx = stage_ix;
    int cw = stage_iw;
    int info_y = stage_iy + stage_ih / 2 - 22;

    if (playlist_count > 0 && current_track < playlist_count) {
        const char *name = playlist[current_track].name;
        gui_text_ttf_centered(win, cx, info_y, cw, 22, name, ink_on_stage, 18);

        const char *status_str = state == STATE_PLAYING ? "Now Playing" :
                                  state == STATE_PAUSED ? "Paused" : "Stopped";
        gui_text_ttf_centered(win, cx, info_y + 26, cw, 18, status_str,
                              dim_on_stage, 13);
    } else {
        gui_text_ttf_centered(win, cx, info_y, cw, 22, "No media loaded",
                              dim_on_stage, 16);
    }
}

// Draw the control bar: seek + transport buttons + time + volume + fullscreen.
static void draw_controls(void) {
    int bar_x = MP_PAD;
    int bar_y = WIN_H - MP_PAD - CONTROLS_H;
    int bar_w = WIN_W - 2 * MP_PAD;
    int bar_h = CONTROLS_H;

    // Control bar card.
    gui_card(win, bar_x, bar_y, bar_w, bar_h);

    int inner_x = bar_x + MP_PAD;
    int inner_w = bar_w - 2 * MP_PAD;

    // ---- Seek row (time / slider / duration) ----
    char time_str[16];
    char dur_str[16];
    format_time(current_time, time_str);

    int duration = 0;
    if (playlist_count > 0 && current_track < playlist_count)
        duration = playlist[current_track].duration_secs;
    format_time(duration, dur_str);

    int seek_row_y = bar_y + 12;
    int time_w = 44;

    mp_text(win, inner_x, seek_row_y + 1, time_str, 12, COL_INK_DIM);

    seek_x = inner_x + time_w + 6;
    seek_y = seek_row_y - 5;
    seek_w = inner_w - 2 * (time_w + 6);
    int seek_max = duration > 0 ? duration : 1;
    gui_slider(win, seek_x, seek_y, seek_w, current_time, seek_max, GUI_ST_NORMAL);

    mp_text(win, seek_x + seek_w + 6, seek_row_y + 1, dur_str, 12, COL_INK_DIM);

    // ---- Transport row (prev / play-pause / stop / next + volume + fullscreen) ----
    int row_y = bar_y + 40;
    int total_btn_w = BTN_COUNT * BTN_SIZE + (BTN_COUNT - 1) * BTN_GAP;
    int start_x = bar_x + (bar_w - total_btn_w) / 2;
    int btn_y = row_y;

    const char *labels[] = {"<<", state == STATE_PLAYING ? "II" : ">", "[]", ">>"};

    for (int i = 0; i < BTN_COUNT; i++) {
        int bx = start_x + i * (BTN_SIZE + BTN_GAP);

        buttons[i].x = bx;
        buttons[i].y = btn_y;
        buttons[i].w = BTN_SIZE;
        buttons[i].h = BTN_SIZE;
        buttons[i].label = labels[i];

        gui_btn_variant_t var = (i == BTN_PLAY) ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY;
        gui_state_t st = (hover_button == i) ? GUI_ST_HOVER : GUI_ST_NORMAL;
        gui_button(win, bx, btn_y, BTN_SIZE, BTN_SIZE, labels[i], var, st);
    }

    // ---- Volume (left of center) ----
    int vol_label_x = inner_x;
    int vol_center_y = btn_y + BTN_SIZE / 2;
    mp_text(win, vol_label_x, vol_center_y - 7, "Vol", 12, COL_INK_DIM);

    vol_x = vol_label_x + 28;
    vol_y = vol_center_y - 8;
    vol_w = VOL_W;
    gui_slider(win, vol_x, vol_y, vol_w, volume, 100, GUI_ST_NORMAL);

    // ---- Fullscreen / cinema toggle (right) ----
    fs_w = 92;
    fs_h = BTN_SIZE;
    fs_x = bar_x + bar_w - MP_PAD - fs_w;
    fs_y = btn_y;
    gui_button(win, fs_x, fs_y, fs_w, fs_h,
               fullscreen ? "Windowed" : "Cinema",
               GUI_BTN_GHOST, GUI_ST_NORMAL);
}

// Draw playlist
static void draw_playlist(void) {
    if (fullscreen) return;  // cinema mode hides the playlist column

    int pl_x = WIN_W - MP_PAD - PLAYLIST_W;
    int pl_y = MP_PAD;
    int pl_w = PLAYLIST_W;
    int pl_h = WIN_H - 2 * MP_PAD - CONTROLS_H - MP_GAP;

    gui_card(win, pl_x, pl_y, pl_w, pl_h);

    int pad = 8;
    int hdr_y = pl_y + pad;
    mp_text(win, pl_x + pad, hdr_y, "Playlist", 14, COL_INK);

    // Separator under header.
    win_draw_rect(win, pl_x + pad, hdr_y + 22, pl_w - 2 * pad, 1, COL_BORDER);

    int list_top = hdr_y + 30;
    int item_h = 26;
    int avail = pl_h - (list_top - pl_y) - pad;
    int visible_items = avail / item_h;
    int r = (gui_active_style().base == GUI_STYLE_MODERN) ? 4 : 0;

    for (int i = 0; i < visible_items && i + playlist_scroll < playlist_count; i++) {
        int idx = i + playlist_scroll;
        int iy = list_top + i * item_h;
        int ix = pl_x + pad;
        int iw = pl_w - 2 * pad;

        bool selected = (idx == current_track);
        if (selected) {
            gui_fill_rounded_aa(win, ix, iy, iw, item_h - 2, r, COL_ACCENT, COL_CARD);
        }

        // Track number badge.
        char num[4];
        num[0] = '0' + ((idx + 1) / 10);
        num[1] = '0' + ((idx + 1) % 10);
        num[2] = '.';
        num[3] = '\0';
        uint32_t numc = selected ? gui_ink_on(COL_ACCENT) : COL_INK_DIM;
        mp_text(win, ix + 6, iy + 5, num, 12, numc);

        // Track name (truncated to fit).
        char name[24];
        int j = 0;
        while (playlist[idx].name[j] && j < 18) {
            name[j] = playlist[idx].name[j];
            j++;
        }
        name[j] = '\0';

        uint32_t text_color = selected ? gui_ink_on(COL_ACCENT) : COL_INK;
        mp_text(win, ix + 34, iy + 5, name, 13, text_color);
    }
}

// Full redraw
static void draw_all(void) {
    // Window background fill.
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, COL_WINDOW_BG);
    draw_display();
    draw_controls();
    draw_playlist();
    win_invalidate(win);
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

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Load the active theme palette into the shared style engine.
    load_theme_palette();

    // Create window
    win = win_create("Media Player", 100, 60, WIN_W, WIN_H);
    if (win < 0) {
        printf("Failed to create window\n");
        return 1;
    }

    printf("Media Player window created (handle=%d)\n", win);

    // Initialize demo playlist
    init_demo_playlist();

    // Initial draw
    draw_all();

    // Event loop
    gui_event_t event;
    int running = 1;
    int win_x = 100, win_y = 60;
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

            draw_controls();
            win_invalidate(win);
        }

        if (event_type == 0) continue;

        switch (event.type) {
            case EVENT_RESIZE:
                if (event.mouse_x > 0 && event.mouse_y > 0) { g_win_w = event.mouse_x; g_win_h = event.mouse_y; }
                draw_all();
                break;
            case EVENT_REDRAW:
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
                    fullscreen = !fullscreen;
                    draw_all();
                }
                break;

            case EVENT_MOUSE_DOWN:
                if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    int lx = event.mouse_x;
                    int ly = event.mouse_y;

                    int handled = 0;

                    // Transport buttons
                    for (int i = 0; i < BTN_COUNT; i++) {
                        if (gui_point_in_rect(lx, ly, buttons[i].x, buttons[i].y,
                                              buttons[i].w, buttons[i].h)) {
                            handle_button(i);
                            draw_all();
                            handled = 1;
                            break;
                        }
                    }

                    // Fullscreen / cinema toggle
                    if (!handled &&
                        gui_point_in_rect(lx, ly, fs_x, fs_y, fs_w, fs_h)) {
                        fullscreen = !fullscreen;
                        draw_all();
                        handled = 1;
                    }

                    // Seek slider (generous vertical hit zone)
                    if (!handled &&
                        gui_point_in_rect(lx, ly, seek_x, seek_y - 4, seek_w, SEEK_H + 8)) {
                        if (playlist_count > 0 && current_track < playlist_count) {
                            int dur = playlist[current_track].duration_secs;
                            int rel = lx - seek_x;
                            if (rel < 0) rel = 0;
                            if (rel > seek_w) rel = seek_w;
                            current_time = (rel * dur) / (seek_w > 0 ? seek_w : 1);
                            draw_all();
                        }
                        handled = 1;
                    }

                    // Volume slider
                    if (!handled &&
                        gui_point_in_rect(lx, ly, vol_x, vol_y - 4, vol_w, SEEK_H + 8)) {
                        int rel = lx - vol_x;
                        if (rel < 0) rel = 0;
                        if (rel > vol_w) rel = vol_w;
                        volume = (rel * 100) / (vol_w > 0 ? vol_w : 1);
                        draw_all();
                        handled = 1;
                    }

                    // Playlist click
                    if (!handled && !fullscreen) {
                        int pl_x = WIN_W - MP_PAD - PLAYLIST_W;
                        int list_top = MP_PAD + 8 + 30;
                        int item_h = 26;
                        if (lx >= pl_x && ly >= list_top) {
                            int idx = playlist_scroll + (ly - list_top) / item_h;
                            if (idx >= 0 && idx < playlist_count) {
                                current_track = idx;
                                current_time = 0;
                                draw_all();
                            }
                        }
                    }
                }
                break;

            case EVENT_MOUSE_MOVE:
                {
                    int lx = event.mouse_x;
                    int ly = event.mouse_y;

                    int new_hover = -1;
                    for (int i = 0; i < BTN_COUNT; i++) {
                        if (gui_point_in_rect(lx, ly, buttons[i].x, buttons[i].y,
                                              buttons[i].w, buttons[i].h)) {
                            new_hover = i;
                            break;
                        }
                    }

                    if (new_hover != hover_button) {
                        hover_button = new_hover;
                        draw_controls();
                        win_invalidate(win);
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
