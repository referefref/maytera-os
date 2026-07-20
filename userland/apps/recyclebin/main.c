// recyclebin - Recycle Bin for MayteraOS (user-space version)
// Deleted files management with restore and permanent delete
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/dirent.h"
#include "../../libc/sys/stat.h"

static int g_win_w = 500, g_win_h = 400;  // #89: live window size (EVENT_RESIZE)
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
    int deleted_time;  // Unix timestamp (demo)
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

#define TRASH_DIR    "/CONFIG/RECYCLE"
#define TRASH_INDEX  "/CONFIG/RBINDEX.TXT"

static int rb_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void rb_join(char *out, int outsz, const char *dir, const char *name) {
    int j = 0;
    for (int i = 0; dir[i] && j < outsz - 1; i++) out[j++] = dir[i];
    if (j > 0 && out[j-1] != '/' && j < outsz - 1) out[j++] = '/';
    for (int i = 0; name[i] && j < outsz - 1; i++) out[j++] = name[i];
    out[j] = 0;
}
// (#239) ext2 rename() is unreliable, so restore falls back to copy+unlink.
static int rb_copy_file(const char *src, const char *dst) {
    int in = open(src, 0); if (in < 0) return -1;
    int out = open(dst, 0x41); if (out < 0) { close(in); return -1; }
    char b[4096]; int n, rc = 0;
    while ((n = read(in, b, sizeof(b))) > 0) {
        int w = 0; while (w < n) { int k = write(out, b + w, n - w); if (k <= 0) { rc = -1; break; } w += k; }
        if (rc) break;
    }
    if (n < 0) rc = -1;
    close(in); close(out); return rc;
}

static char idx_buf[8192];
static int  idx_len;

static void idx_load(void) {
    idx_len = 0; idx_buf[0] = 0;
    int fd = open(TRASH_INDEX, 0);
    if (fd < 0) return;
    char tmp[512]; int n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0)
        for (int i = 0; i < n && idx_len < (int)sizeof(idx_buf) - 1; i++) idx_buf[idx_len++] = tmp[i];
    idx_buf[idx_len] = 0;
    close(fd);
}

static int idx_lookup(const char *name, char *out, int outsz) {
    int i = 0;
    while (i < idx_len) {
        int ls = i; while (i < idx_len && idx_buf[i] != '\n') i++;
        int le = i; if (i < idx_len) i++;
        int bar = ls; while (bar < le && idx_buf[bar] != '|') bar++;
        if (bar < le) {
            int k = ls, t = 0, m = 1;
            while (k < bar) { if (idx_buf[k] != name[t]) { m = 0; break; } k++; t++; }
            if (m && name[t] == 0) { int o = 0, p = bar + 1; while (p < le && o < outsz - 1) out[o++] = idx_buf[p++]; out[o] = 0; return 1; }
        }
    }
    return 0;
}

static void idx_remove(const char *name) {
    char nb[8192]; int nl = 0; int i = 0;
    while (i < idx_len) {
        int ls = i; while (i < idx_len && idx_buf[i] != '\n') i++;
        int le = i; if (i < idx_len) i++;
        int bar = ls; while (bar < le && idx_buf[bar] != '|') bar++;
        int match = 0;
        if (bar < le) { int k = ls, t = 0, m = 1; while (k < bar) { if (idx_buf[k] != name[t]) { m = 0; break; } k++; t++; } if (m && name[t] == 0) match = 1; }
        if (!match) { for (int j = ls; j < le && nl < (int)sizeof(nb) - 1; j++) nb[nl++] = idx_buf[j]; if (nl < (int)sizeof(nb) - 1) nb[nl++] = '\n'; }
    }
    unlink(TRASH_INDEX);
    int fd = open(TRASH_INDEX, 0x41);
    if (fd >= 0) { if (nl) write(fd, nb, nl); close(fd); }
    for (int j = 0; j < nl; j++) idx_buf[j] = nb[j];
    idx_len = nl; idx_buf[nl] = 0;
}

static void load_trash(void) {
    item_count = 0; total_size_kb = 0;
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
        if (!idx_lookup(e->d_name, it->original_path, sizeof(it->original_path))) {
            const char *u = "(unknown)"; int k = 0; while (u[k] && k < 127) { it->original_path[k] = u[k]; k++; } it->original_path[k] = 0;
        }
        char fp[200]; rb_join(fp, sizeof(fp), TRASH_DIR, e->d_name);
        struct stat st; it->size_kb = (stat(fp, &st) == 0) ? (int)((st.st_size + 1023) / 1024) : 0;
        it->deleted_time = 0; it->selected = 0;
        total_size_kb += it->size_kb;
        item_count++;
    }
    closedir(d);
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

// Draw column headers
static void draw_headers(void) {
    int y = TOOLBAR_H;
    win_draw_rect(win, 0, y, WIN_W, HEADER_H, THEME_BG_TERTIARY);
    
    win_draw_text(win, 28, y + 6, "Name", THEME_TEXT_PRIMARY);
    win_draw_text(win, 200, y + 6, "Original Location", THEME_TEXT_PRIMARY);
    win_draw_text(win, 400, y + 6, "Size", THEME_TEXT_PRIMARY);
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
        
        // Name (truncated)
        char name_disp[24];
        int j = 0;
        while (items[idx].name[j] && j < 20) {
            name_disp[j] = items[idx].name[j];
            j++;
        }
        if (items[idx].name[j]) {
            name_disp[j++] = '.';
            name_disp[j++] = '.';
            name_disp[j++] = '.';
        }
        name_disp[j] = '\0';
        win_draw_text(win, 28, y + 4, name_disp, text_color);
        
        // Original path (truncated)
        char path_disp[24];
        j = 0;
        while (items[idx].original_path[j] && j < 22) {
            path_disp[j] = items[idx].original_path[j];
            j++;
        }
        if (items[idx].original_path[j]) {
            path_disp[j++] = '.';
            path_disp[j++] = '.';
        }
        path_disp[j] = '\0';
        win_draw_text(win, 200, y + 4, path_disp, THEME_TEXT_SECONDARY);
        
        // Size
        char size_str[16];
        format_size(items[idx].size_kb, size_str);
        win_draw_text(win, 400, y + 4, size_str, text_color);
    }
    
    // Scrollbar
    if (item_count > visible_rows) {
        int sb_h = list_h;
        int thumb_h = (visible_rows * sb_h) / item_count;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = list_y + (scroll_offset * (sb_h - thumb_h)) / (item_count - visible_rows);
        
        win_draw_rect(win, WIN_W - 12, list_y, 12, sb_h, THEME_SCROLLBAR_BG);
        win_draw_rect(win, WIN_W - 12, thumb_y, 12, thumb_h, THEME_SCROLLBAR_THUMB);
    }
}

// Draw status bar
static void draw_status(void) {
    int y = WIN_H - STATUS_H;
    win_draw_rect(win, 0, y, WIN_W, STATUS_H, THEME_BG_TERTIARY);
    
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
    status[i] = '\0';
    
    win_draw_text(win, 8, y + 6, status, THEME_TEXT_SECONDARY);
}

// Full redraw
static void draw_all(void) {
    draw_toolbar();
    draw_headers();
    draw_items();
    draw_status();
    win_invalidate(win);
}

// Restore selected items
static void restore_selected(void) {
    for (int i = item_count - 1; i >= 0; i--) {
        if (!items[i].selected) continue;
        char src[200]; rb_join(src, sizeof(src), TRASH_DIR, items[i].name);
        if (items[i].original_path[0] && !rb_streq(items[i].original_path, "(unknown)")) {
            if (rename(src, items[i].original_path) != 0) {
                if (rb_copy_file(src, items[i].original_path) == 0) unlink(src);
            }
        }
        idx_remove(items[i].name);
    }
    load_trash();
}

// Permanently delete selected items
static void delete_selected(void) {
    for (int i = item_count - 1; i >= 0; i--) {
        if (!items[i].selected) continue;
        char p[200]; rb_join(p, sizeof(p), TRASH_DIR, items[i].name);
        unlink(p);
        idx_remove(items[i].name);
    }
    load_trash();
}

// Empty the recycle bin
static void empty_bin(void) {
    for (int i = 0; i < item_count; i++) {
        char p[200]; rb_join(p, sizeof(p), TRASH_DIR, items[i].name);
        unlink(p);
    }
    unlink(TRASH_INDEX);
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
