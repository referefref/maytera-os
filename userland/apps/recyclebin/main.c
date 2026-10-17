// recyclebin - Recycle Bin for MayteraOS (user-space version)
// Deleted files management with restore and permanent delete
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/gui_scroll.h"   // shared scrollbar contrast rule (#745 item 77)
#include "../../libc/dirent.h"
#include "../../libc/sys/stat.h"
#include "../../libc/unistd.h"   // #742: fsync - a copy is not a copy until it is durable
#include "../../libc/userconf.h"   // #peruser2: the bin is per-user
#include "../../libc/rbindex.h"    // #aidelete: THE shared bin location + row format
#include "../../libc/time.h"       // #aidelete: render the deleted-at timestamp

// #aidelete: five columns now (Name, Original Location, Deleted, By, Size), so
// the default is wider. 500px could not fit them without truncating the
// original location to uselessness, and the point of the two new columns is
// that a file an AI deleted is VISIBLY different from one the user deleted.
static int g_win_w = 680, g_win_h = 400;  // #89: live window size (EVENT_RESIZE)
#define WIN_W g_win_w
#define WIN_H g_win_h
#define TOOLBAR_H 40
#define HEADER_H 28
#define ROW_H 24
#define STATUS_H 28

#define MAX_ITEMS 64

// Deleted item
typedef struct {
    char name[64];
    char original_path[128];
    int size_kb;
    // #aidelete: a REAL timestamp, read from field 3 of the index row. This
    // used to be set to 0 unconditionally, with the comment "(demo)", because
    // there was nowhere to read a time from: the row was two fields. 0 still
    // means AGE UNKNOWN (a V1 row from an existing bin) and is rendered as
    // "unknown", never as 1970.
    unsigned long deleted_time;
    char actor[RB_ACTOR_MAX];   // "user" or "ai:xxxxxxxx"
    int selected;
} deleted_item_t;

// State
static int win = -1;
static deleted_item_t items[MAX_ITEMS];
static int item_count = 0;
static int scroll_offset = 0;
static int hover_row = -1;
static int total_size_kb = 0;

// Initialize demo deleted items
static void init_items(void) {  /* replaced: real /RECYCLE trash backend */
}

// #peruser2: THE SAME RESOLVER AS apps/files/main.c, and it has to be, because
// these two apps read and write ONE store: Files moves a file into the bin and
// appends the index row, this app lists and restores from it. If they disagreed
// about the path, "Delete" would appear to work and the Recycle Bin would
// appear empty. Root's home is "/", so a root session still uses exactly
// /CONFIG/RECYCLE and /CONFIG/RBINDEX.TXT.
//
// #aidelete: "the same resolver SHAPE" is now literally the same resolver.
// There were two copies of these two functions, two copies of rb_join(), and
// two copies of the index parser/writer, in this file and in apps/files; a
// third was about to be written for the recycler daemon. They all live in
// libc/rbindex.c now and nothing here re-derives them. Deleting the copies IS
// the fix: two implementations of "where is the bin" is how the two apps come
// to disagree, which the old comment could only ask the reader not to do.
#define TRASH_DIR    rbindex_dir()
#define TRASH_INDEX  rbindex_path()

static int rb_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
#define rb_join(o, c, d, n) rbindex_join((o), (c), (d), (n))
// #742 failure reporting. The disk is what failed, so the report lives in RAM
// and is drawn in the window; printf/notify_post are extras.
static char g_err[192];
static void rb_err_clear(void) { g_err[0] = 0; }
static void rb_error(const char *what) {
    int o = 0;
    for (int i = 0; what && what[i] && o < (int)sizeof(g_err) - 1; i++) g_err[o++] = what[i];
    g_err[o] = 0;
    printf("[recyclebin] %s\n", g_err);
}

// (#239) ext2 rename() is unreliable, so restore falls back to copy+unlink, and
// (#742) returns 0 only if every byte is on the medium because the caller
// unlinks the SOURCE on a 0. #aidelete: that sequence now lives ONCE, inside
// rbindex_restore(), with the same checks and the same comment. This app's copy
// is deleted rather than kept beside it.

// #aidelete: idx_load/idx_lookup/idx_remove are GONE from this file. They are
// rbindex_load()/rbindex_lookup()/rbindex_remove() in libc/rbindex.c, which
// carries the #742 durability discipline verbatim (O_TRUNC never
// unlink-then-create, fsync before the in-RAM copy is updated, a non-zero
// return on any failed step) and the V1/V2 row parser.
#define idx_load()        rbindex_load()
#define idx_remove(n)     rbindex_remove(n)

static void load_trash(void) {
    item_count = 0; total_size_kb = 0;
    userconf_ensure_parent_dir(TRASH_DIR);   // #peruser2: <home>/CONFIG
    mkdir(TRASH_DIR, 0755);
    idx_load();
    DIR *d = opendir(TRASH_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != 0 && item_count < MAX_ITEMS) {
        if (e->d_name[0] == '.') continue;
        if (rb_streq(e->d_name, "RBINDEX.TXT")) continue;
        if (e->d_type == DT_DIR) continue;
        deleted_item_t *it = &items[item_count];
        int j = 0; while (e->d_name[j] && j < 63) { it->name[j] = e->d_name[j]; j++; } it->name[j] = 0;
        rb_row_t row;
        if (rbindex_lookup(e->d_name, &row)) {
            int k = 0;
            while (row.original_path[k] && k < (int)sizeof(it->original_path) - 1) {
                it->original_path[k] = row.original_path[k]; k++;
            }
            it->original_path[k] = 0;
            it->deleted_time = row.deleted_unix;
            for (k = 0; k < RB_ACTOR_MAX; k++) it->actor[k] = row.actor[k];
        } else {
            // No row at all: this file was put in the bin by something that
            // did not record where it came from, so it cannot be restored and
            // its age is unknown. Both facts are shown, neither is guessed.
            const char *u = "(unknown)"; int k = 0;
            while (u[k] && k < (int)sizeof(it->original_path) - 1) { it->original_path[k] = u[k]; k++; }
            it->original_path[k] = 0;
            it->deleted_time = 0;
            it->actor[0] = 0;
        }
        char fp[200]; rb_join(fp, sizeof(fp), TRASH_DIR, e->d_name);
        struct stat st; it->size_kb = (stat(fp, &st) == 0) ? (int)((st.st_size + 1023) / 1024) : 0;
        it->selected = 0;
        total_size_kb += it->size_kb;
        item_count++;
    }
    closedir(d);
}

// #aidelete: "Deleted" and "By".
//
// A 0 timestamp is a V1 row, i.e. a file that was in the bin before the index
// carried a time. It renders as "unknown", NEVER as a 1970 date: a made-up date
// next to a retention setting would read as "this will be purged shortly", and
// the recycler's rule is the opposite (an unknown age is never purged).
static void format_deleted(unsigned long t, char *buf, int cap) {
    if (cap < 17) { if (cap > 0) buf[0] = 0; return; }
    if (t == 0) {
        const char *u = "unknown"; int i = 0;
        while (u[i]) { buf[i] = u[i]; i++; }
        buf[i] = 0;
        return;
    }
    time_t tv = (time_t)t;
    struct tm tmv;
    if (!localtime_r(&tv, &tmv)) { buf[0] = '?'; buf[1] = 0; return; }
    int y = tmv.tm_year + 1900, mo = tmv.tm_mon + 1, d = tmv.tm_mday;
    int hh = tmv.tm_hour, mi = tmv.tm_min;
    buf[0]  = (char)('0' + (y / 1000) % 10);
    buf[1]  = (char)('0' + (y / 100) % 10);
    buf[2]  = (char)('0' + (y / 10) % 10);
    buf[3]  = (char)('0' + y % 10);
    buf[4]  = '-';
    buf[5]  = (char)('0' + mo / 10); buf[6] = (char)('0' + mo % 10);
    buf[7]  = '-';
    buf[8]  = (char)('0' + d / 10);  buf[9] = (char)('0' + d % 10);
    buf[10] = ' ';
    buf[11] = (char)('0' + hh / 10); buf[12] = (char)('0' + hh % 10);
    buf[13] = ':';
    buf[14] = (char)('0' + mi / 10); buf[15] = (char)('0' + mi % 10);
    buf[16] = 0;
}

// "You" or "AI". The distinction is the whole requirement: a file an AI deleted
// must be visibly distinguishable from one the user deleted. The actor token
// also carries WHICH AI task (ai:XXXXXXXX), which the status bar shows for the
// selected row rather than squeezing eight hex digits into a list column.
static const char *actor_label(const deleted_item_t *it) {
    if (rbindex_actor_is_ai(it->actor)) return "AI";
    if (it->actor[0] == 0) return "-";
    return "You";
}

// Format size
static void format_size(int kb, char *buf) {
    if (kb >= 1024) {
        int mb = kb / 1024;
        int frac = (kb % 1024) / 102;
        buf[0] = '0' + (mb / 10);
        buf[1] = '0' + (mb % 10);
        buf[2] = '.';
        buf[3] = '0' + frac;
        buf[4] = ' ';
        buf[5] = 'M';
        buf[6] = 'B';
        buf[7] = '\0';
    } else {
        gui_itoa(kb, buf, 8);
        int len = 0;
        while (buf[len]) len++;
        buf[len++] = ' ';
        buf[len++] = 'K';
        buf[len++] = 'B';
        buf[len] = '\0';
    }
}

// Count selected items
static int count_selected(void) {
    int count = 0;
    for (int i = 0; i < item_count; i++) {
        if (items[i].selected) count++;
    }
    return count;
}

// Draw toolbar
static void draw_toolbar(void) {
    win_draw_rect(win, 0, 0, WIN_W, TOOLBAR_H, THEME_BG_SECONDARY);
    
    // Restore button
    win_draw_rect(win, 8, 6, 80, 28, THEME_BUTTON_BG);
    win_draw_text(win, 20, 12, "Restore", THEME_BUTTON_TEXT);
    
    // Delete permanently button
    win_draw_rect(win, 96, 6, 120, 28, THEME_BUTTON_BG);
    win_draw_text(win, 108, 12, "Delete Perm.", THEME_BUTTON_TEXT);
    
    // Empty bin button
    win_draw_rect(win, 224, 6, 100, 28, THEME_ERROR);
    win_draw_text(win, 240, 12, "Empty Bin", THEME_BUTTON_TEXT);
    
    // Selection info
    int sel_count = count_selected();
    if (sel_count > 0) {
        char sel_str[32] = "Selected: ";
        char num[8];
        gui_itoa(sel_count, num, 8);
        int i = 10;
        int j = 0;
        while (num[j]) sel_str[i++] = num[j++];
        sel_str[i] = '\0';
        win_draw_text(win, WIN_W - 120, 12, sel_str, THEME_TEXT_SECONDARY);
    }
}

// #aidelete: FIVE columns, and their x positions are derived ONCE here so the
// header, the rows and the truncation all read the same numbers. The three
// right-hand columns are pinned to the right edge (the window is resizable),
// and the two left-hand columns take what is left.
#define COL_NAME_X   28
static int col_size_x(void)    { return WIN_W - 84; }   // right-most
static int col_by_x(void)      { return WIN_W - 128; }
static int col_deleted_x(void) { return WIN_W - 268; }
static int col_path_x(void)    { return 188; }
static int col_name_w(void)    { return col_path_x() - COL_NAME_X - 8; }
static int col_path_w(void)    { return col_deleted_x() - col_path_x() - 8; }

// Draw column headers
static void draw_headers(void) {
    int y = TOOLBAR_H;
    win_draw_rect(win, 0, y, WIN_W, HEADER_H, THEME_BG_TERTIARY);

    win_draw_text(win, COL_NAME_X, y + 6, "Name", THEME_TEXT_PRIMARY);
    win_draw_text(win, col_path_x(), y + 6, "Original Location", THEME_TEXT_PRIMARY);
    win_draw_text(win, col_deleted_x(), y + 6, "Deleted", THEME_TEXT_PRIMARY);
    win_draw_text(win, col_by_x(), y + 6, "By", THEME_TEXT_PRIMARY);
    win_draw_text(win, col_size_x(), y + 6, "Size", THEME_TEXT_PRIMARY);
}

// Draw item list
static void draw_items(void) {
    int list_y = TOOLBAR_H + HEADER_H;
    int list_h = WIN_H - TOOLBAR_H - HEADER_H - STATUS_H;
    int visible_rows = list_h / ROW_H;
    
    // Background
    win_draw_rect(win, 0, list_y, WIN_W, list_h, THEME_BG_PRIMARY);
    
    if (item_count == 0) {
        win_draw_text(win, WIN_W / 2 - 60, list_y + list_h / 2, "Recycle Bin is empty", THEME_TEXT_SECONDARY);
        return;
    }
    
    for (int i = 0; i < visible_rows && i + scroll_offset < item_count; i++) {
        int idx = i + scroll_offset;
        int y = list_y + i * ROW_H;
        
        // Hover/selection highlight
        if (items[idx].selected) {
            win_draw_rect(win, 0, y, WIN_W, ROW_H, THEME_SELECTION_BG);
        } else if (idx == hover_row) {
            win_draw_rect(win, 0, y, WIN_W, ROW_H, THEME_BG_SECONDARY);
        }
        
        uint32_t text_color = items[idx].selected ? THEME_SELECTION_TEXT : THEME_TEXT_PRIMARY;
        
        // Checkbox
        win_draw_rect(win, 8, y + 4, 16, 16, THEME_BG_TERTIARY);
        gui_draw_rect_outline(win, 8, y + 4, 16, 16, THEME_WINDOW_BORDER);
        if (items[idx].selected) {
            // Checkmark
            win_draw_rect(win, 11, y + 9, 10, 2, THEME_ACCENT);
            win_draw_rect(win, 13, y + 7, 2, 10, THEME_ACCENT);
        }
        
        // Name (truncated to the column, which is now derived rather than a
        // hardcoded 20 characters: the window is resizable)
        char name_disp[72];
        int maxc = col_name_w() / 8;
        if (maxc > (int)sizeof(name_disp) - 4) maxc = (int)sizeof(name_disp) - 4;
        if (maxc < 4) maxc = 4;
        int j = 0;
        while (items[idx].name[j] && j < maxc) { name_disp[j] = items[idx].name[j]; j++; }
        if (items[idx].name[j]) { name_disp[j++] = '.'; name_disp[j++] = '.'; name_disp[j++] = '.'; }
        name_disp[j] = '\0';
        win_draw_text(win, COL_NAME_X, y + 4, name_disp, text_color);

        // Original path (truncated to the column)
        char path_disp[72];
        maxc = col_path_w() / 8;
        if (maxc > (int)sizeof(path_disp) - 4) maxc = (int)sizeof(path_disp) - 4;
        if (maxc < 4) maxc = 4;
        j = 0;
        while (items[idx].original_path[j] && j < maxc) { path_disp[j] = items[idx].original_path[j]; j++; }
        if (items[idx].original_path[j]) { path_disp[j++] = '.'; path_disp[j++] = '.'; }
        path_disp[j] = '\0';
        win_draw_text(win, col_path_x(), y + 4, path_disp, THEME_TEXT_SECONDARY);

        // #aidelete: Deleted (when) and By (who).
        char when[20];
        format_deleted(items[idx].deleted_time, when, (int)sizeof(when));
        win_draw_text(win, col_deleted_x(), y + 4, when,
                      items[idx].selected ? text_color : THEME_TEXT_SECONDARY);

        // An AI delete is drawn in the warning ink so it stands out in a list of
        // "You" rows, which is the point: the owner's requirement is that an AI
        // delete is ANNOUNCED, and a bin where the two look identical announces
        // nothing. The selection fill wins where it would hurt contrast.
        {
            int ai = rbindex_actor_is_ai(items[idx].actor);
            uint32_t bc = items[idx].selected ? text_color
                        : (ai ? THEME_WARNING : THEME_TEXT_SECONDARY);
            win_draw_text(win, col_by_x(), y + 4, actor_label(&items[idx]), bc);
        }

        // Size
        char size_str[16];
        format_size(items[idx].size_kb, size_str);
        win_draw_text(win, col_size_x(), y + 4, size_str, text_color);
    }
    
    // Scrollbar
    if (item_count > visible_rows) {
        int sb_h = list_h;
        int thumb_h = (visible_rows * sb_h) / item_count;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = list_y + (scroll_offset * (sb_h - thumb_h)) / (item_count - visible_rows);
        
        // Colours via the shared rule (#745 item 77), geometry unchanged.
        uint32_t sb_track, sb_thumb;
        gui_scroll_colors(0, THEME_BG_PRIMARY, &sb_track, &sb_thumb);
        win_draw_rect(win, WIN_W - 12, list_y, 12, sb_h, sb_track);
        // (#117) was fill-only, no boundary; see gui_scroll_trough_border()'s
        // comment in gui_scroll.h.
        gui_scroll_trough_border(win, WIN_W - 12, list_y, 12, sb_h, sb_track, THEME_BG_PRIMARY);
        win_draw_rect(win, WIN_W - 12, thumb_y, 12, thumb_h, sb_thumb);
    }
}

// Draw status bar
static void draw_status(void) {
    int y = WIN_H - STATUS_H;
    win_draw_rect(win, 0, y, WIN_W, STATUS_H, THEME_BG_TERTIARY);
    if (g_err[0]) {
        // #742: takes over the whole status bar and stays until the next action.
        // A failed delete or restore that scrolls away has not been reported.
        win_draw_rect(win, 0, y, WIN_W, STATUS_H, 0x00A02020);
        win_draw_text(win, 8, y + 6, g_err, 0x00FFFFFF);
        return;
    }
    
    // Item count and size
    char status[64];
    int i = 0;
    
    char count_str[8];
    gui_itoa(item_count, count_str, 8);
    int j = 0;
    while (count_str[j]) status[i++] = count_str[j++];
    
    const char *items_text = " items, ";
    j = 0;
    while (items_text[j]) status[i++] = items_text[j++];
    
    char size_str[16];
    format_size(total_size_kb, size_str);
    j = 0;
    while (size_str[j]) status[i++] = size_str[j++];
    
    const char *total_text = " total";
    j = 0;
    while (total_text[j]) status[i++] = total_text[j++];

    // #aidelete: the retention policy belongs next to the item list, because
    // "Deleted" only means something if the user can see how long a deleted
    // file is kept. Read fresh each draw so a Settings change shows up without
    // restarting the app.
    {
        const char *lab = rbindex_retention_label(rbindex_read_retention());
        const char *pre = "   Kept: ";
        j = 0; while (pre[j] && i < (int)sizeof(status) - 20) status[i++] = pre[j++];
        j = 0; while (lab[j] && i < (int)sizeof(status) - 2) status[i++] = lab[j++];
    }
    status[i] = '\0';

    win_draw_text(win, 8, y + 6, status, THEME_TEXT_SECONDARY);

    // Exactly one AI-deleted row selected: name the AI task that did it. Eight
    // hex digits is the GraphFS journal's node id for the same delete, so it is
    // the join key between this window and the tamper-evident trail, which is
    // why it is shown verbatim rather than summarised.
    {
        int sel = -1, n = 0;
        for (int k = 0; k < item_count; k++) if (items[k].selected) { sel = k; n++; }
        if (n == 1 && rbindex_actor_is_ai(items[sel].actor)) {
            char msg[64]; int o = 0;
            const char *p = "Deleted by AI task ";
            while (p[o]) { msg[o] = p[o]; o++; }
            for (int k = 3; items[sel].actor[k] && o < (int)sizeof(msg) - 1; k++)
                msg[o++] = items[sel].actor[k];
            msg[o] = 0;
            int w = 8 * o;
            win_draw_text(win, WIN_W - w - 10, y + 6, msg, THEME_WARNING);
        }
    }
}

// Full redraw
static void draw_all(void) {
    draw_toolbar();
    draw_headers();
    draw_items();
    draw_status();
    win_invalidate(win);
}

// Restore selected items. #742: an item whose restore FAILED keeps its index
// entry, so it stays in the bin and stays visible. The previous code dropped
// the entry unconditionally, which made a failed restore look identical to a
// successful one while leaving the file in /CONFIG/RECYCLE with no record of
// where it came from.
static void restore_selected(void) {
    int failed = 0, done = 0;
    rb_err_clear();
    for (int i = item_count - 1; i >= 0; i--) {
        if (!items[i].selected) continue;
        // #aidelete: rbindex_restore() IS the rename-then-fsynced-copy-then-
        // unlink sequence this loop used to inline, with the row dropped last
        // and only on success. Flags 0 keeps today's exact behaviour (an
        // existing destination is overwritten, as it always has been here); the
        // AI undelete path passes RB_RESTORE_NOCLOBBER instead. One
        // implementation, two policies, rather than two implementations.
        rb_row_t row;
        int k = 0;
        while (items[i].name[k] && k < RB_NAME_MAX - 1) { row.name[k] = items[i].name[k]; k++; }
        row.name[k] = 0;
        for (k = 0; k < RB_PATH_MAX - 1 && items[i].original_path[k]; k++)
            row.original_path[k] = items[i].original_path[k];
        row.original_path[k] = 0;
        row.deleted_unix = items[i].deleted_time;
        row.actor[0] = 0;
        int rc = rbindex_restore(&row, 0);
        if (rc == 0) { done++; continue; }
        failed++;
    }
    if (failed)
        rb_error(done ? "Some items could not be restored and are still here"
                      : "Could not restore; the item is still in the Recycle Bin");
    load_trash();
}

// Permanently delete selected items. #742: a failed unlink used to still drop
// the index entry, so the file stayed on disk forever, invisible and
// unrestorable, and the user was shown a successful delete.
static void delete_selected(void) {
    int failed = 0;
    rb_err_clear();
    for (int i = item_count - 1; i >= 0; i--) {
        if (!items[i].selected) continue;
        char p[200]; rb_join(p, sizeof(p), TRASH_DIR, items[i].name);
        if (unlink(p) != 0) { failed++; continue; }   // keep the index entry
        if (idx_remove(items[i].name) != 0) failed++;
    }
    if (failed) rb_error("Some items could not be deleted and are still here");
    load_trash();
}

// Empty the recycle bin
static void empty_bin(void) {
    int failed = 0;
    rb_err_clear();
    for (int i = 0; i < item_count; i++) {
        char p[200]; rb_join(p, sizeof(p), TRASH_DIR, items[i].name);
        if (unlink(p) != 0) failed++;
    }
    if (failed) {
        // #742: do NOT wipe the index while files remain. Their entries are the
        // only record of where they came from.
        rb_error("Could not empty the Recycle Bin; some items remain");
    } else {
        // #aidelete: rbindex_clear() is the same O_TRUNC + fsync + checked
        // close this did inline, with the in-RAM copy updated only on success.
        if (rbindex_clear() != 0) rb_error("Emptied, but the index could not be cleared");
    }
    load_trash();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    // Create window
    win = win_create("Recycle Bin", 90, 60, WIN_W, WIN_H);
    if (win < 0) {
        printf("Failed to create window\n");
        return 1;
    }
    
    printf("Recycle Bin window created (handle=%d)\n", win);
    
    // Initialize demo data
    load_trash();
    
    // Initial draw
    draw_all();
    
    // Event loop
    gui_event_t event;
    int running = 1;
    int win_x = 90, win_y = 60;
    
    while (running) {
        int event_type = win_get_event(win, &event, 100);
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
                } else if (event.key_char == 'a' || event.key_char == 'A') {
                    // Select all
                    for (int i = 0; i < item_count; i++) {
                        items[i].selected = 1;
                    }
                    draw_all();
                } else if (event.key_char == 'd' || event.key_char == 'D') {
                    // Deselect all
                    for (int i = 0; i < item_count; i++) {
                        items[i].selected = 0;
                    }
                    draw_all();
                } else if (event.key_char == 'r' || event.key_char == 'R') {
                    restore_selected();
                    draw_all();
                }
                break;
                
            case EVENT_MOUSE_DOWN:
                if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    int lx = event.mouse_x;
                    int ly = event.mouse_y;
                    
                    // Check toolbar buttons
                    if (ly >= 6 && ly < 34) {
                        if (lx >= 8 && lx < 88) {
                            restore_selected();
                            draw_all();
                        } else if (lx >= 96 && lx < 216) {
                            delete_selected();
                            draw_all();
                        } else if (lx >= 224 && lx < 324) {
                            empty_bin();
                            draw_all();
                        }
                    }
                    
                    // Check item list click
                    int list_y = TOOLBAR_H + HEADER_H;
                    int list_h = WIN_H - TOOLBAR_H - HEADER_H - STATUS_H;
                    
                    if (ly >= list_y && ly < list_y + list_h) {
                        int row = (ly - list_y) / ROW_H + scroll_offset;
                        if (row < item_count) {
                            // Toggle selection on checkbox or entire row
                            items[row].selected = !items[row].selected;
                            draw_all();
                        }
                    }
                }
                break;
                
            case EVENT_MOUSE_MOVE:
                {
                    int ly = event.mouse_y;
                    int list_y = TOOLBAR_H + HEADER_H;
                    int list_h = WIN_H - TOOLBAR_H - HEADER_H - STATUS_H;
                    
                    int new_hover = -1;
                    if (ly >= list_y && ly < list_y + list_h) {
                        new_hover = (ly - list_y) / ROW_H + scroll_offset;
                        if (new_hover >= item_count) new_hover = -1;
                    }
                    
                    if (new_hover != hover_row) {
                        hover_row = new_hover;
                        draw_items();
                        win_invalidate(win);
                    }
                }
                break;
                
            case EVENT_MOUSE_SCROLL:
                {
                    int list_h = WIN_H - TOOLBAR_H - HEADER_H - STATUS_H;
                    int visible_rows = list_h / ROW_H;
                    int max_scroll = item_count - visible_rows;
                    if (max_scroll < 0) max_scroll = 0;
                    
                    if (event.scroll_delta < 0 && scroll_offset < max_scroll) {
                        scroll_offset++;
                        draw_all();
                    } else if (event.scroll_delta > 0 && scroll_offset > 0) {
                        scroll_offset--;
                        draw_all();
                    }
                }
                break;
                
            default:
                break;
        }
    }
    
    win_destroy(win);
    printf("Recycle Bin closed\n");
    
    return 0;
}
