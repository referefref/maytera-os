// textfield.h - Reusable cursor-aware single-line text input field for
// MayteraOS user-space applications.
//
// Background (task #244): userland text input fields historically only
// appended characters at the end and deleted (backspace) the last character.
// This module adds a real text caret: Left/Right move the caret, Home/End
// jump to the ends, printable characters insert AT the caret (shifting the
// tail right), Backspace deletes the character BEFORE the caret, and Delete
// deletes the character AT the caret.
//
// Task #542 extends this into the OS-wide standard-editing widget: a real
// SELECTION (Ctrl+A select-all, click-drag range-select), the standard
// clipboard verbs through the SYSTEM-WIDE kernel clipboard (Ctrl+C copy,
// Ctrl+X cut, Ctrl+V paste, so copy in app A pastes in app B), and a per-field
// UNDO/REDO history (Ctrl+Z / Ctrl+Y). Every app that feeds its key events to
// tf_handle_key() inherits all of it at once.
//
// Keycodes match what the kernel delivers to userland windows in a
// gui_event_t (see kernel cpu/isr.c + gui/desktop.c):
//   Left  = keycode 0x82, Right = keycode 0x83  (key_char == 0)
//   Up    = keycode 0x80, Down  = keycode 0x81  (key_char == 0)
//   Home  = keycode 0x47, End   = keycode 0x4F  (extended; may not be emitted
//           by the current kernel keyboard layer, handled here for forward
//           compatibility and so apps do not have to special-case them)
//   Delete= keycode 0x53 (extended, same caveat as Home/End)
//   Backspace = key_char 0x08 (also keycode 0x0E on some paths)
//   Printable = key_char in 0x20..0x7E
//   Ctrl+<letter> arrives as an ASCII control char in key_char (the PS/2 and
//     USB-HID drivers both fold Ctrl+A..Z to 0x01..0x1A): Ctrl+A=0x01,
//     Ctrl+C=0x03, Ctrl+V=0x16, Ctrl+X=0x18, Ctrl+Y=0x19, Ctrl+Z=0x1A. None of
//     those collide with a normal key (unlike Ctrl+H/I/M = BS/Tab/Enter).
//
// This is header-only (static inline) so every app that includes gui.h gets
// it with zero Makefile / libc.a changes.
#ifndef _TEXTFIELD_H
#define _TEXTFIELD_H

#include "types.h"
#include "gui.h"
#include "syscall.h"   // #542: clipboard_set/clipboard_get (system-wide clipboard)

// Control-char shortcuts as delivered in gui_event_t.key_char.
#define TF_KEY_SELALL 0x01   // Ctrl+A
#define TF_KEY_COPY   0x03   // Ctrl+C
#define TF_KEY_PASTE  0x16   // Ctrl+V
#define TF_KEY_CUT    0x18   // Ctrl+X
#define TF_KEY_REDO   0x19   // Ctrl+Y
#define TF_KEY_UNDO   0x1A   // Ctrl+Z

// Undo/redo history bounds. The history is embedded in the field so the widget
// stays header-only and caller-alloc-free. A single-line field is small, so a
// modest depth and per-snapshot cap keep the struct a couple of KiB. Content
// longer than TF_UNDO_MAX-1 simply disables history for that field (rare for a
// single-line control); editing still works, only Ctrl+Z/Y go quiet.
#define TF_UNDO_DEPTH 10
#define TF_UNDO_MAX   256    // max captured content length per snapshot (incl NUL room)
#define TF_PASTE_MAX  512    // max bytes pulled from the clipboard in one paste

// Coalescing op kinds, so a run of typing (or of deletes) collapses into ONE
// undo step instead of one-per-keystroke.
enum { TF_OP_NONE = 0, TF_OP_INSERT, TF_OP_DELETE, TF_OP_OTHER };

typedef struct {
    char  buf[TF_UNDO_MAX];
    short len;
    short cursor;
} tf_snap_t;

// Caret-aware single-line text field state. The buffer is caller-owned; this
// struct only tracks length and caret position into it, plus (since #542) the
// selection anchor and the undo/redo history.
typedef struct {
    char *buf;     // caller-owned, NUL-terminated buffer
    int   cap;     // buffer capacity in bytes (including the NUL terminator)
    int   len;     // current string length (== strlen(buf))
    int   cursor;  // caret index, in [0, len]

    // --- Selection (#542). sel_anchor == -1 means no selection; otherwise the
    // selection spans [min(anchor,cursor), max(anchor,cursor)). The caret is
    // always the moving end (cursor), the anchor the fixed end.
    int   sel_anchor;

    // --- Undo/redo history (#542). hist[hist_pos] always mirrors the current
    // committed state; entries below are undo, entries above are redo.
    tf_snap_t hist[TF_UNDO_DEPTH];
    int   hist_len;   // number of valid snapshots
    int   hist_pos;   // index of the current-state snapshot
    int   hist_ok;    // 0 once a too-long content disabled history
    int   cur_op;     // last coalescing op kind (TF_OP_*)
} textfield_t;

// Compute the length of a NUL-terminated string (local, no libc dependency).
static inline int tf__strlen(const char *s) {
    int n = 0;
    if (s) { while (s[n]) n++; }
    return n;
}

// ---- Undo/redo internals ---------------------------------------------------
// Capture the current field state into a snapshot. Returns 1 on success, 0 if
// the content is too long to snapshot (history then goes dormant).
static inline int tf__capture(const textfield_t *tf, tf_snap_t *s) {
    if (tf->len > TF_UNDO_MAX - 1) return 0;
    int i = 0;
    for (; i < tf->len; i++) s->buf[i] = tf->buf[i];
    s->buf[i] = '\0';
    s->len = (short)tf->len;
    s->cursor = (short)tf->cursor;
    return 1;
}

// Restore a snapshot into the field (clamped to the caller's capacity).
static inline void tf__restore(textfield_t *tf, const tf_snap_t *s) {
    int n = s->len;
    if (n > tf->cap - 1) n = tf->cap - 1;
    int i = 0;
    for (; i < n; i++) tf->buf[i] = s->buf[i];
    tf->buf[i] = '\0';
    tf->len = n;
    tf->cursor = s->cursor;
    if (tf->cursor > tf->len) tf->cursor = tf->len;
    if (tf->cursor < 0) tf->cursor = 0;
    tf->sel_anchor = -1;
}

// Seed the history with the current state as the sole baseline.
static inline void tf__hist_reset(textfield_t *tf) {
    tf->hist_len = 0;
    tf->hist_pos = 0;
    tf->hist_ok  = 1;
    tf->cur_op   = TF_OP_NONE;
    if (tf__capture(tf, &tf->hist[0])) {
        tf->hist_len = 1;
    } else {
        tf->hist_ok = 0;   // content already too long to track
    }
}

// Commit the current (post-edit) state to history under coalescing op `op`.
static inline void tf__commit(textfield_t *tf, int op) {
    if (!tf->hist_ok) return;
    // Coalesce with the previous step when the op kind matches and we sit at the
    // top of the history: overwrite the current-state snapshot in place so the
    // whole run undoes to the state before the run began.
    if (op == tf->cur_op && op != TF_OP_NONE &&
        tf->hist_len > 0 && tf->hist_pos == tf->hist_len - 1) {
        if (!tf__capture(tf, &tf->hist[tf->hist_pos])) tf->hist_ok = 0;
        return;
    }
    // New step: drop any redo tail, then append (dropping the oldest if full).
    tf->hist_len = tf->hist_pos + 1;
    if (tf->hist_len >= TF_UNDO_DEPTH) {
        for (int i = 1; i < TF_UNDO_DEPTH; i++) tf->hist[i - 1] = tf->hist[i];
        tf->hist_len = TF_UNDO_DEPTH - 1;
        tf->hist_pos = tf->hist_len - 1;
    }
    if (!tf__capture(tf, &tf->hist[tf->hist_pos + 1])) { tf->hist_ok = 0; return; }
    tf->hist_pos++;
    tf->hist_len = tf->hist_pos + 1;
    tf->cur_op = op;
}

// Bind a field to a buffer. The current buffer contents are adopted and the
// caret is placed at the end (matching prior append behaviour on focus).
static inline void tf_init(textfield_t *tf, char *buf, int cap) {
    tf->buf = buf;
    tf->cap = cap;
    tf->len = tf__strlen(buf);
    if (tf->len > cap - 1) tf->len = cap - 1;
    tf->cursor = tf->len;
    tf->sel_anchor = -1;
    tf__hist_reset(tf);
}

// Attach the field to an existing buffer whose length and caret the CALLER
// tracks, for the transient-wrapper pattern (a stack textfield_t rebuilt on
// every key event around app-owned buffer+len+cursor variables). Unlike
// tf_init this does NOT scan the buffer with strlen (the caller passes len) and
// it DISABLES the undo history, because a per-event wrapper is discarded before
// the next event so its history and selection cannot persist anyway. This keeps
// such callers crash-safe now that the struct carries history state that must
// be initialized. Apps wanting real selection/undo keep a PERSISTENT field and
// use tf_init instead.
static inline void tf_attach(textfield_t *tf, char *buf, int cap, int len,
                             int cursor) {
    tf->buf = buf;
    tf->cap = cap;
    if (len < 0) len = 0;
    if (len > cap - 1) len = cap - 1;
    tf->len = len;
    if (cursor < 0) cursor = 0;
    if (cursor > len) cursor = len;
    tf->cursor = cursor;
    tf->sel_anchor = -1;
    tf->hist_len = 0;
    tf->hist_pos = 0;
    tf->hist_ok  = 0;   // history off for transient wrappers (see above)
    tf->cur_op   = TF_OP_NONE;
}

// Replace the field text wholesale; caret goes to the end. Resets history.
static inline void tf_set_text(textfield_t *tf, const char *text) {
    int i = 0;
    if (text) {
        while (text[i] && i < tf->cap - 1) { tf->buf[i] = text[i]; i++; }
    }
    tf->buf[i] = '\0';
    tf->len = i;
    tf->cursor = i;
    tf->sel_anchor = -1;
    tf__hist_reset(tf);
}

// Clamp the caret into a valid range (call after the app mutates buf/len).
static inline void tf_clamp(textfield_t *tf) {
    tf->len = tf__strlen(tf->buf);
    if (tf->len > tf->cap - 1) tf->len = tf->cap - 1;
    if (tf->cursor < 0) tf->cursor = 0;
    if (tf->cursor > tf->len) tf->cursor = tf->len;
    if (tf->sel_anchor > tf->len) tf->sel_anchor = tf->len;
}

static inline void tf_home(textfield_t *tf) { tf->cursor = 0; }
static inline void tf_end(textfield_t *tf)  { tf->cursor = tf->len; }

static inline void tf_left(textfield_t *tf) {
    if (tf->cursor > 0) tf->cursor--;
}
static inline void tf_right(textfield_t *tf) {
    if (tf->cursor < tf->len) tf->cursor++;
}

// ---- Selection (#542) ------------------------------------------------------
static inline int  tf_sel_active(const textfield_t *tf) {
    return tf->sel_anchor >= 0 && tf->sel_anchor != tf->cursor;
}
static inline int  tf_sel_lo(const textfield_t *tf) {
    int a = tf->sel_anchor, b = tf->cursor;
    if (a < 0) return tf->cursor;
    return a < b ? a : b;
}
static inline int  tf_sel_hi(const textfield_t *tf) {
    int a = tf->sel_anchor, b = tf->cursor;
    if (a < 0) return tf->cursor;
    return a > b ? a : b;
}
static inline void tf_clear_sel(textfield_t *tf) { tf->sel_anchor = -1; }

static inline void tf_select_all(textfield_t *tf) {
    tf->sel_anchor = 0;
    tf->cursor = tf->len;
}

// Delete the current selection (if any). Returns 1 if content changed. Pure:
// does not touch history (callers commit).
static inline int tf_delete_selection(textfield_t *tf) {
    if (!tf_sel_active(tf)) { tf->sel_anchor = -1; return 0; }
    int lo = tf_sel_lo(tf), hi = tf_sel_hi(tf);
    int n = hi - lo;
    for (int i = lo; i + n <= tf->len; i++) tf->buf[i] = tf->buf[i + n];
    tf->len -= n;
    tf->cursor = lo;
    tf->sel_anchor = -1;
    return 1;
}

// Map a pixel X offset (relative to the text origin) to a character index for a
// fixed-width FONT_WIDTH font. TTF fields should measure substrings with their
// own width function and call tf_set_caret/tf_drag_* directly.
static inline int tf_index_from_px_mono(const textfield_t *tf, int px) {
    if (px <= 0) return 0;
    int idx = (px + FONT_WIDTH / 2) / FONT_WIDTH;
    if (idx > tf->len) idx = tf->len;
    return idx;
}

// Mouse selection: begin a drag (sets both ends), extend a drag (moves the
// caret end, keeps the anchor). Indices are character positions in [0,len].
static inline void tf_drag_begin(textfield_t *tf, int idx) {
    if (idx < 0) idx = 0; if (idx > tf->len) idx = tf->len;
    tf->sel_anchor = idx;
    tf->cursor = idx;
}
static inline void tf_drag_to(textfield_t *tf, int idx) {
    if (idx < 0) idx = 0; if (idx > tf->len) idx = tf->len;
    if (tf->sel_anchor < 0) tf->sel_anchor = tf->cursor;
    tf->cursor = idx;
}
static inline void tf_set_caret(textfield_t *tf, int idx) {
    if (idx < 0) idx = 0; if (idx > tf->len) idx = tf->len;
    tf->cursor = idx;
    tf->sel_anchor = -1;
}

// Insert a printable character AT the caret, shifting the tail right.
// Returns 1 if the buffer changed, 0 otherwise (e.g. full or non-printable).
// Pure: does not touch selection or history (callers manage those).
static inline int tf_insert(textfield_t *tf, char c) {
    if (c < 0x20 || c > 0x7E) return 0;
    if (tf->len >= tf->cap - 1) return 0;
    if (tf->cursor < 0) tf->cursor = 0;
    if (tf->cursor > tf->len) tf->cursor = tf->len;
    // Shift [cursor..len] right by one (including the terminator).
    for (int i = tf->len; i >= tf->cursor; i--) {
        tf->buf[i + 1] = tf->buf[i];
    }
    tf->buf[tf->cursor] = c;
    tf->len++;
    tf->cursor++;
    return 1;
}

// Delete the character BEFORE the caret (Backspace). Returns 1 if changed.
static inline int tf_backspace(textfield_t *tf) {
    if (tf->cursor <= 0 || tf->len <= 0) return 0;
    for (int i = tf->cursor - 1; i < tf->len; i++) {
        tf->buf[i] = tf->buf[i + 1];
    }
    tf->len--;
    tf->cursor--;
    return 1;
}

// Delete the character AT the caret (Delete). Returns 1 if changed.
static inline int tf_delete(textfield_t *tf) {
    if (tf->cursor >= tf->len || tf->len <= 0) return 0;
    for (int i = tf->cursor; i < tf->len; i++) {
        tf->buf[i] = tf->buf[i + 1];
    }
    tf->len--;
    return 1;
}

// ---- Undo / redo (#542) ----------------------------------------------------
static inline int tf_undo(textfield_t *tf) {
    if (!tf->hist_ok || tf->hist_pos <= 0) return 0;
    tf->hist_pos--;
    tf__restore(tf, &tf->hist[tf->hist_pos]);
    tf->cur_op = TF_OP_NONE;
    return 1;
}
static inline int tf_redo(textfield_t *tf) {
    if (!tf->hist_ok || tf->hist_pos >= tf->hist_len - 1) return 0;
    tf->hist_pos++;
    tf__restore(tf, &tf->hist[tf->hist_pos]);
    tf->cur_op = TF_OP_NONE;
    return 1;
}

// ---- Clipboard verbs (#542) ------------------------------------------------
// Copy the selection (or, when nothing is selected, the whole field) to the
// system-wide clipboard. Returns 1 if anything was placed on the clipboard.
static inline int tf_copy(textfield_t *tf) {
    int lo, hi;
    if (tf_sel_active(tf)) { lo = tf_sel_lo(tf); hi = tf_sel_hi(tf); }
    else                   { lo = 0; hi = tf->len; }
    if (hi <= lo) return 0;
    clipboard_set(tf->buf + lo, hi - lo);
    return 1;
}

// Cut the selection to the clipboard. With no selection this is a no-op (so a
// stray Ctrl+X never wipes the whole field). Returns 1 if the field changed.
static inline int tf_cut(textfield_t *tf) {
    if (!tf_sel_active(tf)) return 0;
    int lo = tf_sel_lo(tf), hi = tf_sel_hi(tf);
    clipboard_set(tf->buf + lo, hi - lo);
    tf_delete_selection(tf);
    tf__commit(tf, TF_OP_OTHER);
    return 1;
}

// Paste clipboard text at the caret, replacing any selection. Non-printable
// bytes (newlines, tabs, control) are dropped so a single-line field stays
// single-line. Returns 1 if the field changed.
static inline int tf_paste(textfield_t *tf) {
    char tmp[TF_PASTE_MAX];
    int held = clipboard_get(tmp, (int)sizeof(tmp));
    if (held <= 0) return 0;
    if (held > (int)sizeof(tmp)) held = (int)sizeof(tmp);
    int had_sel = tf_sel_active(tf);
    if (had_sel) tf_delete_selection(tf);
    int changed = 0;
    for (int i = 0; i < held; i++) {
        char c = tmp[i];
        if (c >= 0x20 && c <= 0x7E) { if (tf_insert(tf, c)) changed = 1; }
    }
    if (changed || had_sel) { tf__commit(tf, TF_OP_OTHER); return 1; }
    return 0;
}

// Feed a key event (only meaningful for EVENT_KEY_DOWN) to the field.
// Returns 1 if the field content OR caret/selection changed (caller should
// redraw). Enter/Tab/Escape are NOT consumed here: the caller handles
// submit / focus / cancel itself, so this returns 0 for them.
static inline int tf_handle_key(textfield_t *tf, const gui_event_t *ev) {
    uint32_t kc = ev->keycode;
    char     c  = ev->key_char;

    // Standard-editing shortcuts (control chars in key_char). Checked first so
    // they are never mistaken for printable input.
    switch ((unsigned char)c) {
        case TF_KEY_SELALL: tf_select_all(tf); return 1;
        case TF_KEY_COPY:   return tf_copy(tf);
        case TF_KEY_CUT:    return tf_cut(tf);
        case TF_KEY_PASTE:  return tf_paste(tf);
        case TF_KEY_UNDO:   return tf_undo(tf);
        case TF_KEY_REDO:   return tf_redo(tf);
        default: break;
    }

    // Caret movement. Any bare arrow/Home/End collapses the selection.
    switch (kc) {
        case 0x82: { int p = tf->cursor; int had = tf_sel_active(tf); tf_clear_sel(tf); tf_left(tf);  return had || tf->cursor != p; }  // Left
        case 0x83: { int p = tf->cursor; int had = tf_sel_active(tf); tf_clear_sel(tf); tf_right(tf); return had || tf->cursor != p; }  // Right
        case 0x47: { int p = tf->cursor; int had = tf_sel_active(tf); tf_clear_sel(tf); tf_home(tf);  return had || tf->cursor != p; }  // Home
        case 0x4F: { int p = tf->cursor; int had = tf_sel_active(tf); tf_clear_sel(tf); tf_end(tf);   return had || tf->cursor != p; }  // End
        case 0x53:   // Delete
            if (tf_sel_active(tf)) { tf_delete_selection(tf); tf__commit(tf, TF_OP_DELETE); return 1; }
            if (tf_delete(tf)) { tf__commit(tf, TF_OP_DELETE); return 1; }
            return 0;
        default: break;
    }

    // Backspace (delivered as ASCII 0x08, or keycode 0x0E on some paths).
    if (c == (char)0x08 || kc == 0x0E) {
        if (tf_sel_active(tf)) { tf_delete_selection(tf); tf__commit(tf, TF_OP_DELETE); return 1; }
        if (tf_backspace(tf)) { tf__commit(tf, TF_OP_DELETE); return 1; }
        return 0;
    }

    // Printable insert at caret, replacing any selection.
    if (c >= 0x20 && c <= 0x7E) {
        int had_sel = tf_sel_active(tf);
        if (had_sel) tf_delete_selection(tf);
        if (tf_insert(tf, c)) { tf__commit(tf, TF_OP_INSERT); return 1; }
        if (had_sel) { tf__commit(tf, TF_OP_OTHER); return 1; }
        return 0;
    }

    return 0;
}

// Convenience: caret X offset (in pixels) for a fixed-width FONT_WIDTH font,
// relative to the text origin. For TTF fields, measure the substring
// buf[0..cursor) with the app's own width function instead.
static inline int tf_caret_px_mono(const textfield_t *tf) {
    return tf->cursor * FONT_WIDTH;
}

// ---- Shared one-call renderer (#542) --------------------------------------
// Draw a complete monospaced field: background, border, selection highlight,
// text and a caret. Apps with their own (e.g. TrueType) layout draw the
// selection themselves from tf_sel_lo()/tf_sel_hi(); this covers the common
// fixed-width case so a field gets full selection rendering in one call.
// `focused` draws the caret; pass 0 for an unfocused field.
static inline void gui_draw_textfield_tf(int handle, int x, int y, int width,
                                         int height, const textfield_t *tf,
                                         int focused, uint32_t bg_color,
                                         uint32_t text_color, uint32_t border_color,
                                         uint32_t sel_color) {
    win_draw_rect(handle, x, y, width, height, bg_color);
    gui_draw_rect(handle, x, y, width, height, border_color);
    int tx = x + 4;
    int ty = y + (height - FONT_HEIGHT) / 2;
    // Selection highlight behind the text.
    if (tf_sel_active(tf)) {
        int lo = tf_sel_lo(tf), hi = tf_sel_hi(tf);
        int sx = tx + lo * FONT_WIDTH;
        int sw = (hi - lo) * FONT_WIDTH;
        if (sw > 0) win_draw_rect(handle, sx, ty, sw, FONT_HEIGHT, sel_color);
    }
    if (tf->buf && tf->buf[0]) {
        win_draw_text(handle, tx, ty, tf->buf, text_color);
    }
    if (focused) {
        int cx = tx + tf->cursor * FONT_WIDTH;
        win_draw_rect(handle, cx, ty, 1, FONT_HEIGHT, text_color);
    }
}

#endif // _TEXTFIELD_H
