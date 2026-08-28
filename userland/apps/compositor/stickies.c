// stickies.c - Desktop sticky notes for the MayteraOS userland compositor (#270).
//
// Small colored note rectangles pinned to the desktop. Multiple notes, each:
//   - draggable by its title strip,
//   - editable inline (click the body to type; a blinking caret + word wrap),
//   - removable via a small [x] in the top-right corner,
//   - recolorable via a swatch in the top-left (cycles preset colors).
// A master visibility flag (g_show_stickies) appears in the widget registry /
// tray menu. "New Sticky Note" is added to the desktop right-click menu.
//
// Persistence: all notes (text + position + size + color index) are written to
// /CONFIG/STICKIES.DAT, one note per line:  "<x> <y> <w> <h> <color>|<text>".
// The loader is robust to a missing or garbage file (it simply ends up with no
// notes). Text never contains a newline (Enter inserts a literal-space wrap
// hint is avoided; Enter just commits/keeps editing with a soft break char).
//
// No dynamic allocation: a fixed array of notes. Freestanding C, no libm.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"    // #742: printf, the disk-independent report channel
#include "../../libc/notify.h"   // #742: notify_post (best effort, needs the disk)

#define STICKY_MAX     24
// #uiscale: scaled at the definition.
#define STICKY_W       ui_px(150)
#define STICKY_H       ui_px(120)
#define STICKY_TITLE_H ui_px(18)
#define STICKY_PAD     ui_px(6)
#define STICKY_TEXTMAX 240

typedef struct {
    int  used;
    int  x, y, w, h;
    int  color;                 // index into s_palette[]
    char text[STICKY_TEXTMAX + 1];
    int  len;
} sticky_t;

static sticky_t g_st[STICKY_MAX];

// Master enable (registry / tray / persistence). On by default.
int g_show_stickies = 0;

// Preset note colors: muted pastels (paper-like). Each is {body, title}.
// Theme-neutral: these read fine on any wallpaper because the note is opaque.
typedef struct { uint32_t body, title; } sticky_color_t;
static const sticky_color_t s_palette[] = {
    { 0x00FFF2A8, 0x00F0DE7A },   // yellow (classic)
    { 0x00B8E9A8, 0x00A0D890 },   // green
    { 0x00A8D8F0, 0x0090C4E0 },   // blue
    { 0x00F2B8C8, 0x00E0A2B4 },   // pink
    { 0x00E0C8F0, 0x00CCAEE2 },   // lilac
    { 0x00F2C89A, 0x00E0B484 },   // orange
};
#define STICKY_NCOLORS ((int)(sizeof(s_palette) / sizeof(s_palette[0])))

// Editing / drag state.
static int s_edit  = -1;        // note index being edited, or -1
static int s_drag  = -1;        // note index being dragged, or -1
static int s_dx = 0, s_dy = 0;  // drag grab offset
static int s_dirty = 0;         // pending save
static unsigned s_blink = 0;    // caret blink counter

// Ink that contrasts with the (always light) note body.
static uint32_t sticky_ink(void) { return 0x00282420; }

// ---- tiny string / number helpers ----------------------------------------
static int st_atoi(const char **pp) {
    const char *p = *pp; int v = 0, neg = 0;
    while (*p == ' ') p++;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *pp = p;
    return neg ? -v : v;
}
static char *st_itoa(char *b, int v) {
    int i = 0, neg = 0; char t[12];
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[i++] = '0';
    while (v) { t[i++] = '0' + v % 10; v /= 10; }
    int j = 0; if (neg) b[j++] = '-';
    while (i) b[j++] = t[--i];
    b[j] = '\0';
    return b;
}

// ---- persistence ---------------------------------------------------------
// #742: a failed save is REPORTED and RETRIED, and the note keeps its unsaved
// text. g_st[] is the live copy and it is never touched by a failure, so the
// user's edit is retained for as long as the compositor runs; s_dirty staying
// set means the next stickies_tick() tries again, which is what makes a
// transient failure (a momentarily full or busy volume) self-correcting.
int  g_stickies_save_failed = 0;      // drawn as an indicator on every note
static unsigned s_fail_count = 0;

static void stickies_save_failed(const char *stage, int rc) {
    s_dirty = 1;                      // NEVER clear it: this is the retry
    if (!g_stickies_save_failed) {
        // First failure only. stickies_tick() runs on the compositor's profile
        // cadence, so reporting every attempt would be a flood, and the on-note
        // indicator is already permanent for as long as it is true.
        printf("[stickies] SAVE FAILED at %s rc=%d - notes are UNSAVED and will "
               "be retried; do not shut down until this clears\n", stage, rc);
        notify_post("Sticky Notes",
                    "Could not save your notes. They are still on screen and "
                    "will be retried, but they are NOT on disk yet.",
                    NOTIFY_ERROR);
    }
    g_stickies_save_failed = 1;
    s_fail_count++;
}

static void stickies_save(void) {
    // #742: O_WRONLY|O_CREAT|O_TRUNC in ONE call. This used to sys_unlink() the
    // file FIRST and then open it, so a failure to re-create destroyed every
    // saved note and lost the edit in the same breath. O_TRUNC has no window in
    // which the old file is already gone and the new one does not exist.
    int fd = sys_open("/CONFIG/STICKIES.DAT", 0x241);
    if (fd < 0) fd = sys_open("/STICKIES.DAT", 0x241);  // fall back to FAT root
    if (fd < 0) { stickies_save_failed("open", fd); return; }
    static char out[STICKY_MAX * (STICKY_TEXTMAX + 48)];
    int o = 0;
    for (int i = 0; i < STICKY_MAX; i++) {
        if (!g_st[i].used) continue;
        char nb[12];
        int vals[5] = { g_st[i].x, g_st[i].y, g_st[i].w, g_st[i].h, g_st[i].color };
        for (int f = 0; f < 5; f++) {
            st_itoa(nb, vals[f]);
            for (int k = 0; nb[k]; k++) out[o++] = nb[k];
            out[o++] = (f < 4) ? ' ' : '|';
        }
        for (int k = 0; k < g_st[i].len; k++) {
            char c = g_st[i].text[k];
            if (c == '\n' || c == '\r') c = ' ';   // never store a newline
            out[o++] = c;
        }
        out[o++] = '\n';
    }
    if (o > 0) {
        long w = sys_write(fd, out, (unsigned long)o);
        if (w != (long)o) { sys_close(fd); stickies_save_failed("write", (int)w); return; }
    }
    // fsync BEFORE close, because close() consumes the fd whether or not it
    // reports an error: a caller that only learns from close() has no handle
    // left to retry with. 0 here means and only means every byte is on the
    // medium.
    int fr = sys_fsync(fd);
    if (fr != 0) { sys_close(fd); stickies_save_failed("fsync", fr); return; }
    int cr = sys_close(fd);
    if (cr != 0) { stickies_save_failed("close", cr); return; }
    if (g_stickies_save_failed) {
        printf("[stickies] save recovered after %u failed attempt(s)\n", s_fail_count);
        notify_post("Sticky Notes", "Your notes have been saved.", NOTIFY_SUCCESS);
        g_stickies_save_failed = 0;
        s_fail_count = 0;
    }
    s_dirty = 0;                      // ONLY on a proven-durable save
}

void stickies_load(void) {
    for (int i = 0; i < STICKY_MAX; i++) g_st[i].used = 0;
    int fd = sys_open("/CONFIG/STICKIES.DAT", 0);
    if (fd < 0) fd = sys_open("/STICKIES.DAT", 0);
    if (fd < 0) return;
    static char buf[STICKY_MAX * (STICKY_TEXTMAX + 48)];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    int slot = 0, i = 0;
    while (buf[i] && slot < STICKY_MAX) {
        // read one line
        const char *p = &buf[i];
        // find end of line
        int j = i; while (buf[j] && buf[j] != '\n') j++;
        // parse "x y w h color|text"
        const char *cur = p;
        int x = st_atoi(&cur), y = st_atoi(&cur), w = st_atoi(&cur), h = st_atoi(&cur);
        int color = st_atoi(&cur);
        // expect '|'
        while (*cur == ' ') cur++;
        // sanity: a valid note line has a '|' before the line end
        int valid = 0;
        for (const char *q = p; q < &buf[j]; q++) if (*q == '|') { valid = 1; break; }
        if (valid && w >= 40 && h >= 40) {
            // skip up to and including the '|'
            const char *t = p;
            while (t < &buf[j] && *t != '|') t++;
            if (t < &buf[j]) t++;   // past '|'
            sticky_t *s = &g_st[slot];
            s->used = 1;
            s->x = x; s->y = y; s->w = w; s->h = h;
            s->color = (color >= 0 && color < STICKY_NCOLORS) ? color : 0;
            int li = 0;
            while (t < &buf[j] && li < STICKY_TEXTMAX) s->text[li++] = *t++;
            s->text[li] = '\0'; s->len = li;
            slot++;
        }
        i = (buf[j] == '\n') ? j + 1 : j;
    }
}

// ---- creation / deletion -------------------------------------------------
// Spawn a new note near (px,py), clamped on screen. Returns its index or -1.
int sticky_new_at(int px, int py) {
    int idx = -1;
    for (int i = 0; i < STICKY_MAX; i++) if (!g_st[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    sticky_t *s = &g_st[idx];
    s->used = 1;
    s->w = STICKY_W; s->h = STICKY_H;
    s->x = px - s->w / 2; s->y = py - STICKY_TITLE_H / 2;
    if (s->x < 0) s->x = 0;
    if (s->y < 24) s->y = 24;
    if (s->x > g_fb_width - s->w) s->x = g_fb_width - s->w;
    if (s->y > g_fb_height - s->h - 30) s->y = g_fb_height - s->h - 30;
    // cycle a fresh color based on how many notes already exist
    int n = 0; for (int i = 0; i < STICKY_MAX; i++) if (g_st[i].used) n++;
    s->color = (n - 1) % STICKY_NCOLORS;
    s->text[0] = '\0'; s->len = 0;
    g_show_stickies = 1;            // creating one implies they're visible
    s_edit = idx; s_blink = 0;
    s_dirty = 1;
    return idx;
}
// Convenience used by the desktop context menu (default position = screen-ish center).
void sticky_new(void) {
    sticky_new_at(g_fb_width / 2, g_fb_height / 3);
}

static void sticky_delete(int idx) {
    if (idx < 0 || idx >= STICKY_MAX) return;
    g_st[idx].used = 0;
    if (s_edit == idx) s_edit = -1;
    if (s_drag == idx) s_drag = -1;
    s_dirty = 1;
}

// ---- geometry / hit testing ----------------------------------------------
// Small control boxes: color swatch (left) + close [x] (right) in the title.
static void sticky_swatch_box(const sticky_t *s, int *bx, int *by, int *bw, int *bh) {
    *bx = s->x + 3; *by = s->y + 3; *bw = STICKY_TITLE_H - 6; *bh = STICKY_TITLE_H - 6;
}
static void sticky_close_box(const sticky_t *s, int *bx, int *by, int *bw, int *bh) {
    *bw = STICKY_TITLE_H - 6; *bh = STICKY_TITLE_H - 6;
    *bx = s->x + s->w - *bw - 3; *by = s->y + 3;
}
static int pt_in(int x, int y, int bx, int by, int bw, int bh) {
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

// Topmost note under (x,y), or -1. The edited note is treated as topmost.
int stickies_hit(int x, int y) {
    if (!g_show_stickies) return -1;
    if (s_edit >= 0 && g_st[s_edit].used) {
        sticky_t *s = &g_st[s_edit];
        if (pt_in(x, y, s->x, s->y, s->w, s->h)) return s_edit;
    }
    for (int i = STICKY_MAX - 1; i >= 0; i--) {
        if (!g_st[i].used) continue;
        if (pt_in(x, y, g_st[i].x, g_st[i].y, g_st[i].w, g_st[i].h)) return i;
    }
    return -1;
}

// ---- input ---------------------------------------------------------------
// Handle a left-button press. Returns 1 if consumed (so other layers don't act).
int stickies_press(int x, int y) {
    if (!g_show_stickies) return 0;
    int hit = stickies_hit(x, y);
    if (hit < 0) {
        // Clicking off all notes commits any edit.
        if (s_edit >= 0) { s_edit = -1; s_dirty = 1; }
        return 0;
    }
    sticky_t *s = &g_st[hit];
    int bx, by, bw, bh;
    sticky_close_box(s, &bx, &by, &bw, &bh);
    if (pt_in(x, y, bx, by, bw, bh)) { sticky_delete(hit); return 1; }
    sticky_swatch_box(s, &bx, &by, &bw, &bh);
    if (pt_in(x, y, bx, by, bw, bh)) { s->color = (s->color + 1) % STICKY_NCOLORS; s_dirty = 1; return 1; }
    // Title strip drags the note; body selects it for editing.
    if (y < s->y + STICKY_TITLE_H) {
        s_drag = hit; s_dx = x - s->x; s_dy = y - s->y;
        s_edit = hit;       // also focus it
    } else {
        s_edit = hit; s_blink = 0;
    }
    s_dirty = 1;
    return 1;
}
void stickies_drag_to(int x, int y) {
    if (s_drag < 0) return;
    sticky_t *s = &g_st[s_drag];
    s->x = x - s_dx; s->y = y - s_dy;
    if (s->x < 0) s->x = 0;
    if (s->y < 24) s->y = 24;
    if (s->x > g_fb_width - s->w) s->x = g_fb_width - s->w;
    if (s->y > g_fb_height - s->h - 30) s->y = g_fb_height - s->h - 30;
}
void stickies_release(void) {
    if (s_drag >= 0) { s_drag = -1; s_dirty = 1; }
}
int stickies_is_dragging(void) { return s_drag >= 0; }
int stickies_editing(void) { return s_edit >= 0; }

// Keyboard input while a note is focused. Returns 1 if the key was consumed.
int stickies_handle_key(int key) {
    if (s_edit < 0 || !g_st[s_edit].used) return 0;
    sticky_t *s = &g_st[s_edit];
    if (key == 27) { s_edit = -1; s_dirty = 1; return 1; }            // ESC commits
    if (key == '\b' || key == 8 || key == 127) {
        if (s->len > 0) s->text[--s->len] = '\0';
        s_dirty = 1; return 1;
    }
    // Enter / printable chars are inserted (Enter stored as space; we wrap on draw).
    if (key == '\n' || key == '\r') {
        if (s->len < STICKY_TEXTMAX) { s->text[s->len++] = ' '; s->text[s->len] = '\0'; }
        s_dirty = 1; return 1;
    }
    if (key >= 0x20 && key <= 0x7E && s->len < STICKY_TEXTMAX) {
        s->text[s->len++] = (char)key; s->text[s->len] = '\0';
        s_dirty = 1; return 1;
    }
    return 1;   // swallow other keys while editing
}

// ---- render --------------------------------------------------------------
// Draw one note with word-wrapped text and (if focused) a blinking caret.
static void sticky_draw(int idx) {
    sticky_t *s = &g_st[idx];
    const sticky_color_t *c = &s_palette[s->color];
    int editing = (idx == s_edit);

    // Soft shadow for a "pinned paper" look.
    draw_fill_rect(s->x + 3, s->y + 3, s->w, s->h, 0x40000000);
    draw_fill_rect(s->x, s->y, s->w, s->h, c->body);
    draw_fill_rect(s->x, s->y, s->w, STICKY_TITLE_H, c->title);
    // #742: while the save is failing, EVERY note says so, permanently and
    // without touching the disk. A toast scrolls away and the notification
    // spool is itself on the volume that just failed; this cannot be missed and
    // cannot fail. Red border + a red title strip.
    if (g_stickies_save_failed) {
        draw_fill_rect(s->x, s->y, s->w, STICKY_TITLE_H, 0x00B03030);
        draw_text(s->x + STICKY_TITLE_H, s->y, "UNSAVED", 0x00FFFFFF);
    }
    draw_rect_outline(s->x, s->y, s->w, s->h,
                      g_stickies_save_failed ? 0x00FF2020
                                             : (editing ? 0x00404040 : 0x00808060));

    // Color swatch (left) and close [x] (right) in the title strip.
    int bx, by, bw, bh;
    sticky_swatch_box(s, &bx, &by, &bw, &bh);
    draw_fill_rect(bx, by, bw, bh, c->body);
    draw_rect_outline(bx, by, bw, bh, 0x00505040);
    sticky_close_box(s, &bx, &by, &bw, &bh);
    draw_text(bx + 2, by, "x", 0x00603030);

    // Word-wrapped body text. draw_text/text_width are the (variable-width) TTF
    // font, so all wrap math uses text_width() to stay consistent.
    uint32_t ink = sticky_ink();
    int tx = s->x + STICKY_PAD;
    int ty = s->y + STICKY_TITLE_H + 4;
    int maxw = s->w - STICKY_PAD * 2;
    int line_h = FONT_CHAR_H + 1;
    int sp_w = text_width(" "); if (sp_w < 1) sp_w = 4;
    int cx = tx;
    int caret_x = tx, caret_y = ty;
    char word[64]; int wl = 0;
    int n = s->len;
    for (int i = 0; i <= n; i++) {
        char ch = (i < n) ? s->text[i] : ' ';
        if (ch == ' ') {
            // flush the pending word
            if (wl > 0) {
                word[wl] = '\0';
                int ww = text_width(word);
                if (cx > tx && cx - tx + ww > maxw) {   // wrap before this word
                    cx = tx; ty += line_h;
                }
                if (ww > maxw) {
                    // hard-break a word longer than the note width
                    for (int k = 0; k < wl; k++) {
                        char one[2] = { word[k], 0 };
                        int ow = text_width(one);
                        if (cx - tx + ow > maxw) { cx = tx; ty += line_h; }
                        if (ty + FONT_CHAR_H <= s->y + s->h - 2)
                            draw_text(cx, ty, one, ink);
                        cx += ow;
                    }
                } else {
                    if (ty + FONT_CHAR_H <= s->y + s->h - 2)
                        draw_text(cx, ty, word, ink);
                    cx += ww;
                }
                wl = 0;
            }
            // the space itself (only if not at end and room remains)
            if (i < n) {
                if (cx - tx + sp_w > maxw) { cx = tx; ty += line_h; }
                else cx += sp_w;
            }
            caret_x = cx; caret_y = ty;
        } else if (wl < 63) {
            word[wl++] = ch;
            word[wl] = '\0';
            caret_x = cx + text_width(word); caret_y = ty;
        }
    }

    if (editing && ((s_blink / 15) & 1) == 0) {
        if (caret_y + FONT_CHAR_H <= s->y + s->h - 2)
            draw_fill_rect(caret_x, caret_y, 2, FONT_CHAR_H, ink);
    }
}

void stickies_render(void) {
    if (!g_show_stickies) return;
    s_blink++;
    // Draw non-focused notes first, focused note last (on top).
    for (int i = 0; i < STICKY_MAX; i++)
        if (g_st[i].used && i != s_edit) sticky_draw(i);
    if (s_edit >= 0 && g_st[s_edit].used) sticky_draw(s_edit);
}

// Periodic save (called from the compositor's profile_tick cadence).
void stickies_tick(void) {
    if (s_dirty) stickies_save();
}
