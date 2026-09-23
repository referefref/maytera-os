// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// ofxbar.c - the Calc formula/name bar (see ofxbar.h).
#include "ofxbar.h"
#include "oicon.h"
#include "oui.h"
#include "../../libc/string.h"

int ofxbar_height(void) { return oui_input_h() + 4; }

void ofxbar_init(ofxbar_t *f, int x, int y, int w) {
    memset(f, 0, sizeof(*f));
    f->x = x; f->y = y; f->w = w; f->h = ofxbar_height();
    tf_init(&f->name_tf, f->name_buf, sizeof(f->name_buf));
    tf_init(&f->fx_tf, f->fx_buf, sizeof(f->fx_buf));
}

void ofxbar_move(ofxbar_t *f, int x, int y, int w) { f->x = x; f->y = y; f->w = w; f->h = ofxbar_height(); }

void ofxbar_set(ofxbar_t *f, const char *ref, const char *formula) {
    if (ref) tf_set_text(&f->name_tf, ref);
    if (formula) { tf_set_text(&f->fx_tf, formula); oui_copy(f->fx_revert, sizeof(f->fx_revert), formula); }
}

static void ofx_rects(const ofxbar_t *f, int *ny, int *ih, int *bx, int *by, int *bh, int *fx, int *fw) {
    int i = oui_input_h(), b = oui_btn_h();
    if (ih) *ih = i;
    if (ny) *ny = f->y + (f->h - i) / 2;
    if (bx) *bx = f->x + OFX_FX_X;
    if (by) *by = f->y + (f->h - b) / 2;
    if (bh) *bh = b;
    if (fx) *fx = f->x + OFX_FIELD_X;
    if (fw) { int w = f->w - OFX_FIELD_X - 8; *fw = w < 40 ? 40 : w; }
}

void ofxbar_draw(int win, ofxbar_t *f) {
    uint32_t ground = oui_raised();
    uint32_t rule = gui_ensure_contrast(oui_border_subtle(), ground, GUI_FLOOR_NONTEXT);
    int ny, ih, bx, by, bh, fx, fw;
    ofx_rects(f, &ny, &ih, &bx, &by, &bh, &fx, &fw);
    win_draw_rect(win, f->x, f->y, f->w, f->h, ground);
    win_draw_rect(win, f->x, f->y + f->h - 1, f->w, 1, rule);
    gui_textfield_tf(win, f->x + 8, ny, OFX_NAME_W, ih, f->name_tf.buf, f->name_tf.len, f->name_tf.cursor,
                     f->name_tf.sel_anchor, f->focus == OFX_FOCUS_NAME, "A1");
    gui_state_t st = (f->fx_pressed && f->fx_hot) ? GUI_ST_PRESSED : (f->fx_hot ? GUI_ST_HOVER : GUI_ST_NORMAL);
    gui_button(win, bx, by, 26, bh, "", GUI_BTN_SECONDARY, st);
    gui_palette_t *p = gui_pal();
    uint32_t face = gui_mix(p->surface_raised, p->ink, 8);
    if (st == GUI_ST_HOVER) face = gui_lighten(face, 18); else if (st == GUI_ST_PRESSED) face = gui_darken(face, 18);
    uint32_t ink = oui_glyph_on(oui_on_surface(), face);
    if (!oicon_draw(win, "SCFX", bx + 5, by + (bh - 16) / 2, 16, ink, face))
        oui_text_bold_centered(win, bx, by, 26, bh, "fx", oui_body(), ink);
    gui_textfield_tf(win, fx, ny, fw, ih, f->fx_tf.buf, f->fx_tf.len, f->fx_tf.cursor,
                     f->fx_tf.sel_anchor, f->focus == OFX_FOCUS_FX, 0);
}

static int ofx_caret_from_x(const textfield_t *tf, int rel_x) {
    int size = oui_body();
    if (rel_x <= 0) return 0;
    char pre[512];
    int best = tf->len;
    for (int i = 1; i <= tf->len && i < (int)sizeof(pre) - 1; i++) {
        for (int k = 0; k < i; k++) pre[k] = tf->buf[k];
        pre[i] = 0;
        if (gui_ttf_render_width(pre, size) > rel_x) { best = i - 1; break; }
    }
    return best;
}

int ofxbar_press(ofxbar_t *f, int mx, int my) {
    int ny, ih, bx, by, bh, fx, fw;
    ofx_rects(f, &ny, &ih, &bx, &by, &bh, &fx, &fw);
    if (!oui_in(mx, my, f->x, f->y, f->w, f->h)) return -1;
    if (oui_in(mx, my, f->x + 8, ny, OFX_NAME_W, ih)) {
        f->focus = OFX_FOCUS_NAME;
        tf_set_caret(&f->name_tf, ofx_caret_from_x(&f->name_tf, mx - (f->x + 8 + 8)));
        return OFX_FOCUS_NAME;
    }
    if (oui_in(mx, my, fx, ny, fw, ih)) {
        f->focus = OFX_FOCUS_FX;
        tf_set_caret(&f->fx_tf, ofx_caret_from_x(&f->fx_tf, mx - (fx + 8)));
        return OFX_FOCUS_FX;
    }
    if (oui_in(mx, my, bx, by, 26, bh)) { f->fx_pressed = 1; f->fx_hot = 1; return OFX_PRESS_FXBTN; }
    return -1;
}

int ofxbar_release(ofxbar_t *f, int mx, int my) {
    if (!f->fx_pressed) return 0;
    f->fx_pressed = 0;
    int ny, ih, bx, by, bh, fx, fw;
    ofx_rects(f, &ny, &ih, &bx, &by, &bh, &fx, &fw);
    return oui_in(mx, my, bx, by, 26, bh) ? OFX_PRESS_FXBTN : 0;
}

int ofxbar_motion(ofxbar_t *f, int mx, int my) {
    int ny, ih, bx, by, bh, fx, fw;
    ofx_rects(f, &ny, &ih, &bx, &by, &bh, &fx, &fw);
    int hot = oui_in(mx, my, bx, by, 26, bh);
    if (hot != f->fx_hot) { f->fx_hot = hot; return 1; }
    return 0;
}

void ofxbar_blur(ofxbar_t *f) { f->focus = OFX_FOCUS_NONE; f->fx_hot = 0; f->fx_pressed = 0; }

int ofxbar_key(ofxbar_t *f, const gui_event_t *ev) {
    if (f->focus == OFX_FOCUS_NONE) return 0;
    unsigned int kc = ev->keycode; char ch = ev->key_char;
    textfield_t *tf = (f->focus == OFX_FOCUS_NAME) ? &f->name_tf : &f->fx_tf;
    if (ch == GUI_KEY_ESC) {
        if (f->focus == OFX_FOCUS_FX) tf_set_text(&f->fx_tf, f->fx_revert);
        f->focus = OFX_FOCUS_NONE;
        return 4;
    }
    if (ch == GUI_KEY_ENTER || kc == GUI_KEY_ENTER) { f->focus = OFX_FOCUS_NONE; return 2; }
    if (kc == GUI_KEY_TAB || ch == GUI_KEY_TAB)     { f->focus = OFX_FOCUS_NONE; return 3; }
    return tf_handle_key(tf, ev) ? 1 : 0;
}
