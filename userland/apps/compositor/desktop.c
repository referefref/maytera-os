// desktop.c - Desktop surface for MayteraOS userland compositor
// Renders desktop icons, handles mouse interaction, and draws version string.
// No dynamic allocation: all state lives in fixed-size static arrays.
//
// Icons carry an absolute (px,py) screen position. The DEFAULT layout is a
// horizontal row across the TOP of the screen, wrapping to additional rows.
// Icons can be dragged (single or whole multi-selection), multi-selected via a
// rubber-band rectangle on the empty desktop, auto-arranged, and snapped to a
// grid. Positions persist to UIPROFIL.YML (see profile.c).

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../../kernel/version.h"
#include "../../libc/stdio.h"
// (#745) These three headers are all type-free (no typedefs that could clash
// with compositor.h's unguarded `typedef int bool`, the landmine documented at
// the top of startmenu.c and profile.c), so they are safe to include here:
//   userconf.h - THE home-directory join. userhome_path() is the one lookup of
//                "where is this session's home"; a private copy of that join is
//                exactly how a sandbox root and the paths inside it come to
//                disagree, so this file does not write one.
//   assoc.h    - THE file-type -> app mapping (/ASSOC.CFG + built-in defaults,
//                editable from Settings > Default Apps). Files already opens
//                documents through it; the desktop uses the same table so a
//                .TXT opens in the same app from both surfaces.
//   notify.h   - the shared notification spool, so a failed launch says so.
#include "../../libc/userconf.h"
#include "../../libc/assoc.h"
#include "../../libc/notify.h"

// ============================================================================
// Static state
// ============================================================================

static desktop_icon_t g_icons[DESKTOP_ICON_MAX];
static int            g_icon_count;
static int            g_selected_icon;    // index of last single-selected icon, or -1
static uint64_t       g_last_click_time;  // sys_clock() value at last click
static int            g_last_click_icon;  // icon index at last click, or -1

// Drag / rubber-band state machine.
// Modes: 0 = idle, 1 = dragging icon(s), 2 = rubber-band selection.
static int      g_drag_mode;
static int32_t  g_press_x, g_press_y;     // where the button went down
static int32_t  g_drag_off_x, g_drag_off_y; // cursor offset within the grabbed icon
static int      g_drag_anchor;            // primary icon being dragged, or -1
static bool     g_drag_moved;             // did the cursor move beyond the dead zone
static int32_t  g_band_x, g_band_y, g_band_w, g_band_h; // current rubber-band rect

#define DRAG_DEAD_ZONE 5   // pixels of slop before a press becomes a drag

// ============================================================================
// (#745) <home>/DESKTOP as the source of the user's own icons
// ============================================================================
// The icon set used to be a compiled-in add_icon() list and nothing else. It
// now has TWO sources, and the split is deliberate:
//
//   [0, g_sys_icon_count)  SYSTEM icons: the compiled-in shortcuts plus
//                          anything added this session from the Start menu.
//   [g_sys_icon_count, n)  USER icons: one per entry in <home>/DESKTOP.
//
// A USER FILE CANNOT SHADOW OR REORDER A SYSTEM ICON. This is a decision, not
// an accident. The system icons are the only guaranteed route to Files,
// Settings and Terminal, and <home>/DESKTOP is a directory the session user can
// write; letting a file there displace or re-sort them means a file named
// "Settings" that is not Settings, and a user who can lock themselves out of
// their own shell by dropping six files on the desktop. Duplicate LABELS are
// therefore allowed and harmless: the two icons have different keys, different
// kinds and different activation, and both work.
static int      g_sys_icon_count;   // built-ins + session-added, always first
static unsigned g_home_sig;         // signature of the last <home>/DESKTOP listing
static int      g_home_scanned;     // has a scan ever completed

// Names longer than this are SKIPPED, never truncated. A truncated name is a
// DIFFERENT file, and an icon that opens the wrong file is worse than an icon
// that is absent. 63 + the "<home>/DESKTOP/" prefix fits exec_path[128].
#define DESK_ENT_NAME_MAX 64

static unsigned fnv1a(const char *s, unsigned h) {
    while (*s) { h ^= (unsigned char)(*s++); h *= 16777619u; }
    return h;
}

// <home>/DESKTOP. userhome_path() is THE home join (userconf.c); root's home is
// "/", so a root session resolves this to "/DESKTOP" - see desktop_init().
static int desktop_home_dir(char *out, unsigned long cap) {
    return userhome_path(0, "DESKTOP", out, cap);
}

// "s" + the app basename, uppercased. "/APPS/FILES" -> "sFILES",
// "@RECYCLE" -> "sRECYCLE". Unique across the shipped set and stable forever.
static void make_sys_key(char *out, const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    if (b[0] == '@') b++;
    int i = 0;
    out[i++] = 's';
    for (int k = 0; b[k] && k < 12; k++) {
        char c = b[k];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[i++] = c;
        else out[i++] = '_';
    }
    out[i] = '\0';
}

// "u" + up to 12 sanitized characters of the file name + "_" + 4 hex digits of
// a hash of the FULL name. The hash is what makes two long names that share a
// prefix distinct keys; without it "PROJECT-NOTES-A.TXT" and
// "PROJECT-NOTES-B.TXT" would collide and swap positions.
static void make_user_key(char *out, const char *name) {
    int i = 0;
    out[i++] = 'u';
    for (int k = 0; name[k] && k < 12; k++) {
        char c = name[k];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-') out[i++] = c;
        else out[i++] = '_';
    }
    out[i++] = '_';
    unsigned h = fnv1a(name, 2166136261u);
    static const char hx[] = "0123456789ABCDEF";
    out[i++] = hx[(h >> 12) & 15];
    out[i++] = hx[(h >>  8) & 15];
    out[i++] = hx[(h >>  4) & 15];
    out[i++] = hx[ h        & 15];
    out[i] = '\0';
}

// Lowercase extension of a file name (empty string if there is no dot).
static void ext_of(const char *name, char *e, int esz) {
    int dot = -1;
    for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    int k = 0;
    if (dot >= 0)
        for (int i = dot + 1; name[i] && k < esz - 1; i++) {
            char c = name[i];
            e[k++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    e[k] = '\0';
}

static int ext_in(const char *e, const char *list) {
    // list is space-separated, e.g. "bmp png jpg".
    int i = 0;
    while (list[i]) {
        int j = 0;
        while (list[i + j] && list[i + j] != ' ' && e[j] && list[i + j] == e[j]) j++;
        if (e[j] == '\0' && (list[i + j] == ' ' || list[i + j] == '\0')) return 1;
        while (list[i] && list[i] != ' ') i++;
        while (list[i] == ' ') i++;
    }
    return 0;
}

// WHAT ICON A FILE GETS. There is no "unknown" hole: anything that matches no
// category draws ICON_FILE, which is a real glyph, not a blank.
static icon_id_t icon_for_file(const char *name) {
    char e[10]; ext_of(name, e, sizeof(e));
    if (ext_in(e, "bmp png jpg jpeg gif ico"))          return ICON_IMAGE;
    if (ext_in(e, "wav mp3 ogg flac m4a aac opus mid")) return ICON_MUSIC;
    if (ext_in(e, "mp4 avi mkv mov"))                   return ICON_VIDEO;
    if (ext_in(e, "htm html"))                          return ICON_BROWSER;
    return ICON_FILE;
}

// Every extension that is DATA, whichever bits it happens to carry.
#define DESK_DATA_EXTS \
    "bmp png jpg jpeg gif ico wav mp3 ogg flac m4a aac opus mid mp4 avi mkv " \
    "mov htm html txt md c h cfg log ini csv yml yaml json js asm conf zip " \
    "gz tar pdf ttf otf icn wad"

// Is this a program the desktop should RUN rather than OPEN WITH something?
//
// THE EXECUTE BIT ALONE IS NOT THE ANSWER ON THIS SYSTEM, and finding that out
// is why this function is longer than one line. The first verification image
// copied a shipped wallpaper into the desktop folder and it arrived mode 755:
// the golden's asset install leaves the execute bit set on plain DATA files all
// over the ext2 root. Trusting mode&0111 alone would have classified a .BMP as
// a program, drawn it with the app glyph, and made double-clicking it spawn a
// bitmap as an ELF - an icon that looks right and does nothing, which is the
// exact defect this ticket exists to remove.
//
// So the order is: a known DATA extension wins over the mode bit; ".elf" is
// decisive on its own (it is the answer on a FAT volume, where there is no mode
// at all and fs_type comes back FSPERM_TYPE_FAT); and the mode bit only decides
// the remaining case, an extensionless or unknown-extension file on a POSIX
// volume, where it is the only evidence there is.
static int is_executable_file(const char *full, const char *name) {
    char e[10]; ext_of(name, e, sizeof(e));
    if (ext_in(e, "elf")) return 1;
    if (e[0] && ext_in(e, DESK_DATA_EXTS)) return 0;
    fsperm_info_t fi;
    if (sys_fs_perm_info(full, &fi) != 0) return 0;
    if (fi.is_dir) return 0;
    if (fi.fs_type != FSPERM_TYPE_POSIX) return 0;
    return (fi.mode & 0111) ? 1 : 0;
}

// ============================================================================
// Layout helpers
// ============================================================================

// Full bounding box of an icon (image + label row).
static int32_t icon_box_w(void) { return DESKTOP_ICON_SIZE; }
static int32_t icon_box_h(void) { return DESKTOP_ICON_SIZE + 20; }

// Usable desktop bounds (between any top bar and the bottom bar) for clamping.
// #387: layout-aware so icons stay clear of the active dock/menu bar(s).
static int32_t desk_bottom(void) { return taskbar_get_y(); }
static int32_t desk_top(void)    { return DESKTOP_ICON_MARGIN_Y + taskbar_top_inset(); }

// Clamp an icon top-left so the whole box stays on the visible desktop.
// (#745) Clamped to the WORK AREA, not to the raw screen. grid_cell_pos()
// already started the default grid below any top bar via desk_top(), but the
// clamp floor was 0 and the left/right bounds were the screen, so a DRAGGED
// icon - or a position restored from a profile written under a different dock
// style - could still be parked under a top panel where it cannot be clicked.
static void clamp_icon(int32_t *x, int32_t *y) {
    int wax, way, waw, wah;
    taskbar_work_area(&wax, &way, &waw, &wah);
    int32_t minx = (int32_t)wax;
    int32_t miny = (int32_t)way;
    int32_t maxx = (int32_t)(wax + waw) - icon_box_w();
    int32_t maxy = (int32_t)(way + wah) - icon_box_h();
    if (maxx < minx) maxx = minx;
    if (maxy < miny) maxy = miny;
    if (*x < minx) *x = minx; else if (*x > maxx) *x = maxx;
    if (*y < miny) *y = miny; else if (*y > maxy) *y = maxy;
}

// Place the icon at logical grid cell (col,row) of the default TOP layout.
static void grid_cell_pos(int col, int row, int32_t *x, int32_t *y) {
    *x = DESKTOP_ICON_MARGIN_X + col * DESKTOP_ICON_SPACING_X;
    *y = desk_top() + row * DESKTOP_ICON_SPACING_Y;
}

// Number of columns that fit horizontally before wrapping to the next row.
static int grid_cols(void) {
    int avail = g_fb_width - DESKTOP_ICON_MARGIN_X * 2;
    int cols  = avail / DESKTOP_ICON_SPACING_X;
    if (cols < 1) cols = 1;
    return cols;
}

// Re-flow all visible icons into the default horizontal-top grid, left to right,
// wrapping to a second row when they run past the right edge.
static void layout_default_top(void) {
    int cols = grid_cols();
    int n = 0;
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible) continue;
        int col = n % cols;
        int row = n / cols;
        grid_cell_pos(col, row, &g_icons[i].px, &g_icons[i].py);
        clamp_icon(&g_icons[i].px, &g_icons[i].py);
        n++;
    }
}

// Snap one coordinate pair to the nearest default-grid cell.
static void snap_to_grid(int32_t *x, int32_t *y) {
    int col = (*x - DESKTOP_ICON_MARGIN_X + DESKTOP_ICON_SPACING_X / 2) / DESKTOP_ICON_SPACING_X;
    int row = (*y - desk_top() + DESKTOP_ICON_SPACING_Y / 2) / DESKTOP_ICON_SPACING_Y;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    grid_cell_pos(col, row, x, y);
    clamp_icon(x, y);
}

// Return the index of the topmost icon whose box contains (px,py), or -1.
static int find_icon_at(int32_t px, int32_t py) {
    for (int i = g_icon_count - 1; i >= 0; i--) {
        if (!g_icons[i].visible) continue;
        int32_t ix = g_icons[i].px;
        int32_t iy = g_icons[i].py;
        if (px >= ix && px < ix + icon_box_w() &&
            py >= iy && py < iy + icon_box_h()) {
            return i;
        }
    }
    return -1;
}

static void clear_selection(void) {
    for (int i = 0; i < g_icon_count; i++) g_icons[i].selected = false;
    g_selected_icon = -1;
}

static int selection_count(void) {
    int c = 0;
    for (int i = 0; i < g_icon_count; i++) if (g_icons[i].visible && g_icons[i].selected) c++;
    return c;
}

// Launch the app at exec_path using sys_spawn (no fork; forking from the
// compositor hangs the OS because it duplicates framebuffer mappings).
static void launch_app(const char *exec_path) {
    if (!exec_path || exec_path[0] == '\0') return;
    // #239: "@RECYCLE" opens Files directly in its integrated Recycle Bin view
    // (drop a one-shot sentinel the Files app consumes at startup).
    if (exec_path[0] == '@' && exec_path[1] == 'R') {
        int fd = sys_open("/RECYVIEW.FLG", 0x41);
        if (fd >= 0) { sys_write(fd, "1", 1); sys_close(fd); }
        sys_spawn("/APPS/FILES");
        return;
    }
    sys_spawn(exec_path);
}

// ============================================================================
// desktop_init
// ============================================================================

static void add_icon_kind(const char *name, const char *path, icon_id_t id,
                          unsigned char kind) {
    if (g_icon_count >= DESKTOP_ICON_MAX) return;
    desktop_icon_t *ic = &g_icons[g_icon_count++];
    strncpy(ic->name, name, DESKTOP_ICON_NAME_LEN - 1);
    ic->name[DESKTOP_ICON_NAME_LEN - 1] = '\0';
    strncpy(ic->exec_path, path, sizeof(ic->exec_path) - 1);
    ic->exec_path[sizeof(ic->exec_path) - 1] = '\0';
    ic->kind = kind;
    if (kind == DESK_KIND_SYSTEM) make_sys_key(ic->key, path);
    else                          make_user_key(ic->key, name);
    ic->icon_id  = id;
    ic->px = ic->py = 0;
    ic->pos_set  = 0;
    ic->selected = false;
    ic->visible  = true;
}

static void add_icon(const char *name, const char *path, icon_id_t id) {
    add_icon_kind(name, path, id, DESK_KIND_SYSTEM);
}

// Place an icon on the first default-grid cell no other icon already occupies,
// so a file that appears while the desktop is running does not land on top of
// something. Used only for icons with no saved position.
static void place_free_slot(int idx) {
    int cols = grid_cols();
    for (int n = 0; n < DESKTOP_ICON_MAX * 2; n++) {
        int32_t x, y;
        grid_cell_pos(n % cols, n / cols, &x, &y);
        clamp_icon(&x, &y);
        int taken = 0;
        for (int j = 0; j < g_icon_count; j++) {
            if (j == idx || !g_icons[j].visible) continue;
            int32_t dx = g_icons[j].px - x, dy = g_icons[j].py - y;
            if (dx > -8 && dx < 8 && dy > -8 && dy < 8) { taken = 1; break; }
        }
        if (!taken) {
            g_icons[idx].px = x; g_icons[idx].py = y;
            g_icons[idx].pos_set = 1;
            return;
        }
    }
    grid_cell_pos(0, 0, &g_icons[idx].px, &g_icons[idx].py);
    clamp_icon(&g_icons[idx].px, &g_icons[idx].py);
    g_icons[idx].pos_set = 1;
}

// (#745) The second pass. Only touches icons still marked unplaced, so calling
// it twice, or after every icon already has a position, does nothing.
void desktop_place_unplaced(void) {
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible || g_icons[i].pos_set) continue;
        place_free_slot(i);
        g_needs_redraw = true;
    }
}

// Start-menu "Add to Desktop" (right-click item -> Add to Desktop). Places the
// new icon at the next free grid slot after the current icons (does not
// re-flow existing ones, so a manually-dragged layout is left alone). Session-
// only for now: the icon SET is not persisted across reboot (only positions of
// the compiled-in set are, via profile.c) - see compositor.h's comment.
bool desktop_add_icon(const char *name, const char *path, icon_id_t icon) {
    if (g_icon_count >= DESKTOP_ICON_MAX) return false;
    for (int i = 0; i < g_icon_count; i++) {
        if (g_icons[i].visible && strcmp(g_icons[i].exec_path, path) == 0)
            return false;   // already on the desktop
    }
    add_icon(name, path, icon);
    if (g_icon_count - 1 < 0) return false;
    // A session-added icon is a SYSTEM icon: it is an app shortcut, not a file
    // in <home>/DESKTOP, so desktop_rescan_home() must not sweep it away. It is
    // counted into g_sys_icon_count for the same reason.
    g_sys_icon_count = 0;
    for (int i = 0; i < g_icon_count; i++)
        if (g_icons[i].kind == DESK_KIND_SYSTEM) g_sys_icon_count++;
    int idx = g_icon_count - 1;
    int cols = grid_cols();
    int col = idx % cols, row = idx / cols;
    grid_cell_pos(col, row, &g_icons[idx].px, &g_icons[idx].py);
    clamp_icon(&g_icons[idx].px, &g_icons[idx].py);
    g_icons[idx].pos_set = 1;
    g_needs_redraw = true;
    return true;
}

// ============================================================================
// (#745) desktop_rescan_home
// ============================================================================
// Enumerates <home>/DESKTOP and rebuilds the USER half of the icon list.
//
// COST WHEN NOTHING CHANGED: one open, one readdir sweep, one close, and a
// 32-bit signature comparison. No allocation, no stat, no write, no redraw, no
// repaint request. The per-file sys_fs_perm_info() probes that decide "is this
// executable" only run on the rebuild path, so an unchanged directory never
// pays for them. It runs from the input tick, NEVER from a render path: the
// draw thread must not do file I/O (#426).
//
// The signature covers each entry's NAME and TYPE but deliberately not its
// SIZE, so editing a file does not churn the icon list. The one thing that
// escapes it is chmod +x on a file whose name did not change; that icon keeps
// its old kind until the next add/remove/rename or an explicit Refresh.
void desktop_rescan_home(int force) {
    // Never rebuild mid-drag: the user is holding an icon whose index and
    // position are both about to be rewritten underneath them.
    if (desktop_is_dragging()) return;

    char dir[160];
    if (desktop_home_dir(dir, sizeof(dir)) != 0) return;

    static struct { char name[DESK_ENT_NAME_MAX]; unsigned type; } ents[DESKTOP_ICON_MAX];
    int n = 0;
    unsigned sig = 2166136261u;

    int fd = sys_open(dir, 0);
    if (fd >= 0) {
        dirent_t de;
        while (sys_readdir_raw(fd, &de) == 0) {
            de.name[sizeof(de.name) - 1] = '\0';
            // "." / ".." and dotfiles: a hidden file is hidden on the desktop too.
            if (de.name[0] == '\0' || de.name[0] == '.') continue;
            unsigned long nl = strlen(de.name);
            if (nl >= DESK_ENT_NAME_MAX) continue;   // skip, never truncate
            sig = fnv1a(de.name, sig);
            sig = sig * 31u + de.type;
            if (n < DESKTOP_ICON_MAX) {
                strncpy(ents[n].name, de.name, DESK_ENT_NAME_MAX - 1);
                ents[n].name[DESK_ENT_NAME_MAX - 1] = '\0';
                ents[n].type = de.type;
                n++;
            }
        }
        sys_close(fd);
    }
    sig = sig * 31u + (unsigned)n;

    if (!force && g_home_scanned && sig == g_home_sig) return;   // nothing changed
    g_home_sig    = sig;
    g_home_scanned = 1;

    // Remember where the user's icons currently sit, BY KEY, so a rebuild does
    // not scramble an arrangement. This is the same reason positions are keyed
    // by identity on disk: the index is not stable across a directory change.
    static struct { char key[20]; int32_t x, y; } prev[DESKTOP_ICON_MAX];
    int np = 0;
    for (int i = 0; i < g_icon_count && np < DESKTOP_ICON_MAX; i++) {
        if (g_icons[i].kind == DESK_KIND_SYSTEM) continue;
        strncpy(prev[np].key, g_icons[i].key, sizeof(prev[np].key) - 1);
        prev[np].key[sizeof(prev[np].key) - 1] = '\0';
        prev[np].x = g_icons[i].px;
        prev[np].y = g_icons[i].py;
        np++;
    }

    // Compact the SYSTEM icons to the front, dropping every user icon.
    int w = 0;
    for (int i = 0; i < g_icon_count; i++) {
        if (g_icons[i].kind != DESK_KIND_SYSTEM) continue;
        if (w != i) g_icons[w] = g_icons[i];
        w++;
    }
    g_icon_count     = w;
    g_sys_icon_count = w;

    for (int i = 0; i < n; i++) {
        if (g_icon_count >= DESKTOP_ICON_MAX) break;   // bounded; see below
        char full[128];
        int fl = snprintf(full, sizeof(full), "%s/%s", dir, ents[i].name);
        if (fl <= 0 || fl >= (int)sizeof(full)) continue;   // will not fit: skip

        unsigned char kind = DESK_KIND_FILE;
        icon_id_t     id   = ICON_FILE;
        if (ents[i].type == 1) { kind = DESK_KIND_DIR; id = ICON_FOLDER; }
        else if (is_executable_file(full, ents[i].name)) { kind = DESK_KIND_EXEC; id = ICON_WINDOW; }
        else id = icon_for_file(ents[i].name);

        int idx = g_icon_count;
        add_icon_kind(ents[i].name, full, id, kind);
        if (g_icon_count == idx) break;   // add refused (full)

        // PASS 1 only restores. A new icon is left UNPLACED here on purpose:
        // asking "is this grid cell free?" now would compare against icons
        // whose own saved positions have not been applied yet.
        for (int k = 0; k < np; k++) {
            if (strcmp(prev[k].key, g_icons[idx].key) != 0) continue;
            g_icons[idx].px = prev[k].x;
            g_icons[idx].py = prev[k].y;
            clamp_icon(&g_icons[idx].px, &g_icons[idx].py);
            g_icons[idx].pos_set = 1;
            break;
        }
    }

    // PASS 2, once every restored position is in place.
    desktop_place_unplaced();

    g_needs_redraw = true;
}

// Throttled caller. 2000ms: a desktop folder changes at human speed, and the
// scan is the only file I/O the desktop does at all, so a slower cadence buys
// nothing measurable and a faster one only pays more often for a comparison
// that almost always says "unchanged". Explicit Refresh is the escape hatch for
// anyone who does not want to wait.
void desktop_home_tick(void) {
    static uint64_t s_last;
    uint64_t now = uptime_ms();
    if (s_last != 0 && (now - s_last) < 2000) return;
    s_last = now;
    desktop_rescan_home(0);
}

// ============================================================================
// (#745) New Folder / New File - context-menu actions that used to be stubs
// ============================================================================
// Both were dead menu entries: contextmenu.c dispatched them to a bare `break;`.
// They are wired rather than deleted because the desktop now HAS a backing
// directory to create in, which is the only thing they ever lacked.
static int desk_make_entry(int as_dir) {
    char dir[160];
    if (desktop_home_dir(dir, sizeof(dir)) != 0) return -1;
    sys_mkdir(dir, 0755);   // idempotent; root's /DESKTOP may not exist yet

    char full[160];
    for (int n = 1; n <= 99; n++) {
        // 8.3-safe names, because a home on a FAT volume cannot hold anything
        // longer and a name that fails to create is not a useful default.
        if (as_dir) {
            if (n == 1) snprintf(full, sizeof(full), "%s/NEWFOLD", dir);
            else        snprintf(full, sizeof(full), "%s/NEWFOLD%d", dir, n);
        } else {
            if (n == 1) snprintf(full, sizeof(full), "%s/NEWFILE.TXT", dir);
            else        snprintf(full, sizeof(full), "%s/NEWFIL%d.TXT", dir, n);
        }
        int probe = sys_open(full, 0);
        if (probe >= 0) { sys_close(probe); continue; }   // taken, try the next
        if (as_dir) {
            if (sys_mkdir(full, 0755) != 0) return -1;
        } else {
            int nf = sys_open(full, 0x41 | 0x200);        // O_WRONLY|O_CREAT|O_TRUNC
            if (nf < 0) return -1;
            sys_close(nf);
        }
        return 0;
    }
    return -1;
}

void desktop_new_folder(void) {
    if (desk_make_entry(1) != 0) {
        notify_post("Cannot create folder", "The desktop folder is not writable", NOTIFY_ERROR);
        return;
    }
    desktop_rescan_home(1);
}

void desktop_new_file(void) {
    if (desk_make_entry(0) != 0) {
        notify_post("Cannot create file", "The desktop folder is not writable", NOTIFY_ERROR);
        return;
    }
    desktop_rescan_home(1);
}

// ============================================================================
// (#745) activate_icon - what a double-click actually does, per kind
// ============================================================================
// Every branch ends in a real launch and reports its own failure. Nothing here
// can fall through to "nothing happens", which is the defect this ticket is
// about on the menu side.
static void activate_icon(int idx) {
    if (idx < 0 || idx >= g_icon_count) return;
    desktop_icon_t *ic = &g_icons[idx];

    if (ic->kind == DESK_KIND_SYSTEM) { launch_app(ic->exec_path); return; }

    if (ic->kind == DESK_KIND_DIR) {
        // Open the folder in Files. argv[1] is the start path; Files takes it
        // directly rather than through a sentinel file, so this works for a
        // non-root session that cannot write /CONFIG.
        char *av[2];
        av[0] = (char *)"/APPS/FILES";
        av[1] = ic->exec_path;
        if (sys_spawn_args("/APPS/FILES", av, 2) < 0)
            notify_post("Cannot open folder", ic->name, NOTIFY_ERROR);
        return;
    }

    if (ic->kind == DESK_KIND_EXEC) {
        if (sys_spawn(ic->exec_path) < 0)
            notify_post("Cannot run program", ic->name, NOTIFY_ERROR);
        return;
    }

    // Any other file: the OS-wide association table. assoc_app_for() never
    // returns NULL and falls back to the text editor, so an unrecognised type
    // still opens in something rather than doing nothing. It is given the FULL
    // path, not ic->name, because the label is truncated to fit the icon and a
    // truncated name can lose the extension.
    char app[96];
    assoc_app_for(ic->exec_path, app, sizeof(app));
    char *av[2];
    av[0] = app;
    av[1] = ic->exec_path;
    if (sys_spawn_args(app, av, 2) < 0)
        notify_post("Cannot open file", ic->name, NOTIFY_ERROR);
}

void desktop_init(void) {
    g_icon_count      = 0;
    g_selected_icon   = -1;
    g_last_click_time = 0;
    g_last_click_icon = -1;
    g_drag_mode       = 0;
    g_drag_anchor     = -1;
    g_drag_moved      = false;
    g_band_w = g_band_h = 0;

    add_icon("Computer",    "/APPS/FILES",          ICON_COMPUTER);
    add_icon("Recycle Bin", "@RECYCLE",             ICON_TRASH);
    add_icon("Terminal",    "/APPS/TERMINAL",       ICON_TERMINAL);
    add_icon("Settings",    "/APPS/SETTINGS",       ICON_COG);
    add_icon("Browser",     "/APPS/BROWSER",        ICON_BROWSER);
    // #745: the App Store replaces DOOM in the default set, in the SAME slot
    // (index 5) DOOM occupied.
    //
    // WHAT THIS DOES TO AN EXISTING DESKTOP, stated rather than discovered:
    // the icon SET is compiled in and has never been persisted (profile.c
    // stores only POSITIONS, as "ico<N>x"/"ico<N>y" keyed by index, and
    // desktop_add_icon()'s own comment records that additions are session-only).
    // So there is no saved user icon list for this change to overwrite, and no
    // migration is written: nothing rewrites a profile file. An upgraded
    // machine keeps the saved positions of indices 0-4 exactly as the user
    // arranged them, and the saved position of index 5 - DOOM's - is where the
    // App Store icon now appears. DOOM itself is untouched: still installed at
    // /GAMES/DOOM/DOOM.ELF and still in the start menu (Games ->
    // build/assets/startmenu/system.d/03-games.MENU), only its DEFAULT desktop
    // shortcut is gone. A user who wants it back can right-click it in the
    // start menu and choose Add to Desktop.
    //
    // The path is the BINARY basename as it is actually installed: the ext2
    // root ships /APPS/APPSTORE (uppercase), which is also what the start menu
    // entry and main.c's ICON_APPSTORE loader already use. A name that does not
    // exist on the volume fails silently at spawn (#517 COMPOSIT/COMPOSITOR).
    add_icon("App Store",   "/APPS/APPSTORE",       ICON_APPSTORE);

    g_sys_icon_count = g_icon_count;   // everything above is a SYSTEM icon

    // (#745) The user's own desktop folder.
    //
    // ROOT'S HOME IS "/", so for the shipping autologin session this resolves
    // to "/DESKTOP", and that is the DEFAULT case, not an edge case. The mkdir
    // below is what makes it behave sensibly instead of showing nothing: it is
    // idempotent, it costs one syscall once per session, and for a normal user
    // users_make_home_skeleton() has already created the directory so it is a
    // no-op. If the create is refused (read-only volume, no permission) the
    // scan simply finds no directory, adds no icons, and the desktop is exactly
    // what it is today. It is never an error and never a dialog.
    {
        char dir[160];
        if (desktop_home_dir(dir, sizeof(dir)) == 0) {
            // Result deliberately unused: "already exists" and "created" are
            // equally fine, and the scan below is the real test of either.
            sys_mkdir(dir, 0755);
        }
    }
    desktop_rescan_home(1);

    // Default layout: a horizontal row across the TOP of the screen. profile.c
    // overrides these positions afterwards if saved coordinates exist.
    layout_default_top();

    // (#745) NOTHING PLACED SO FAR IS AUTHORITATIVE, and saying so is the whole
    // point of this line. desktop_rescan_home() above ran its own second pass
    // and marked every icon placed, but that happened BEFORE profile_load() had
    // said a word, so those marks describe a provisional grid, not the user's
    // arrangement. Leaving them set makes the real second pass (after the
    // profile is applied) a no-op, and an icon with no saved position keeps a
    // default slot that another icon has since been restored onto. That is
    // exactly the AA0.TXT-on-top-of-AAA.TXT collision seen on the verification
    // VM, and clearing the marks here is what fixes it: profile_load() marks
    // what it restores, and desktop_place_unplaced() then deals with the rest.
    for (int i = 0; i < g_icon_count; i++) g_icons[i].pos_set = 0;
}

// ============================================================================
// desktop_render
// ============================================================================

void desktop_render(void) {
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible) continue;

        int32_t sx = g_icons[i].px;
        int32_t sy = g_icons[i].py;

        // Selection highlight behind the icon image + label row.
        if (g_icons[i].selected) {
            draw_fill_rect(sx - 4, sy - 4,
                           icon_box_w() + 8, icon_box_h() + 4,
                           CLR_ICON_SEL_BG);
        }

        icon_draw_scaled(g_icons[i].icon_id, sx, sy,
                         DESKTOP_ICON_SIZE, CLR_TEXT_WHITE);

        int32_t label_y  = sy + DESKTOP_ICON_SIZE + 4;
        int32_t label_cx = sx + DESKTOP_ICON_SIZE / 2;

        int lsz = (g_font_px <= 12) ? 12 : (g_font_px <= 16) ? 14 : (g_font_px <= 20) ? 18 : 20;
        int32_t tw = text_width_ttf(g_icons[i].name, lsz);
        int32_t label_x = label_cx - tw / 2;
        const char *nm = g_icons[i].name;
        static const int ox[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
        static const int oy[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
        for (int o = 0; o < 8; o++)
            draw_text_ttf(label_x + ox[o], label_y + oy[o], nm, lsz, CLR_TEXT_SHADOW);
        draw_text_ttf(label_x, label_y, nm, lsz, CLR_TEXT_WHITE);
    }
}

// Draw the rubber-band selection rectangle.
void desktop_render_overlay(void) {
    if (g_drag_mode != 2 || (g_band_w == 0 && g_band_h == 0)) return;
    draw_fill_rect(g_band_x, g_band_y, g_band_w, g_band_h, CLR_ICON_SEL_BG);
    draw_rect_outline(g_band_x, g_band_y, g_band_w, g_band_h, CLR_TEXT_WHITE);
}

// ============================================================================
// desktop_render_version
// ============================================================================

void desktop_render_version(void) {
    // Show the LIVE kernel version (SYS_GET_VERSION) so the desktop never lags
    // the running kernel just because the compositor wasn't rebuilt. Falls back
    // to the compile-time string if the syscall returns nothing.
    static char ver[64]; char vbuf[48]; vbuf[0] = 0;
    get_version(vbuf, sizeof(vbuf));
    if (vbuf[0]) snprintf(ver, sizeof(ver), "v%s", vbuf);
    else snprintf(ver, sizeof(ver), "v%s Build %d", MAYTERA_VERSION_STRING, MAYTERA_BUILD_NUMBER);

    int32_t tw = text_width(ver);
    int32_t vx = g_fb_width  - tw - 10;
    int32_t vy = taskbar_get_y() - 20;

    draw_text_shadow(vx, vy, ver, CLR_VERSION_TEXT, CLR_TEXT_SHADOW);
}

// ============================================================================
// Auto-arrange / align (context-menu actions)
// ============================================================================

void desktop_auto_arrange(void) {
    // layout_default_top() deliberately does NOT mark icons placed (its result
    // at boot is provisional, to be overridden by the profile). An explicit
    // Auto Arrange IS authoritative, so mark them here.
    layout_default_top();
    for (int i = 0; i < g_icon_count; i++) g_icons[i].pos_set = 1;
    g_needs_redraw = true;
}

void desktop_align_to_grid(void) {
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible) continue;
        snap_to_grid(&g_icons[i].px, &g_icons[i].py);
    }
    g_needs_redraw = true;
}

// ============================================================================
// Persistence hooks (profile.c)
// ============================================================================

int desktop_icon_count(void) { return g_icon_count; }

void desktop_get_icon_pos(int idx, int32_t *x, int32_t *y) {
    if (idx < 0 || idx >= g_icon_count) { *x = 0; *y = 0; return; }
    *x = g_icons[idx].px;
    *y = g_icons[idx].py;
}

void desktop_set_icon_pos(int idx, int32_t x, int32_t y) {
    if (idx < 0 || idx >= g_icon_count) return;
    g_icons[idx].px = x;
    g_icons[idx].py = y;
    clamp_icon(&g_icons[idx].px, &g_icons[idx].py);
    g_icons[idx].pos_set = 1;   // (#745) restored from the profile: authoritative
}

// (#745) Re-clamp every icon into the CURRENT work area.
// desktop_set_icon_pos() already clamps, but the profile restore that calls it
// does not control the order in which dock_style and the icon positions are
// parsed, and the dock style can change live afterwards. Re-running the clamp
// when the work area is established or changes is what makes a position saved
// under a bottom-panel style safe under a top-panel one.
void desktop_reclamp_icons(void) {
    for (int i = 0; i < g_icon_count; i++) {
        int32_t ox = g_icons[i].px, oy = g_icons[i].py;
        clamp_icon(&g_icons[i].px, &g_icons[i].py);
        if (g_icons[i].px != ox || g_icons[i].py != oy) g_needs_redraw = true;
    }
}

int desktop_builtin_count(void) { return g_sys_icon_count; }

int desktop_icon_key(int idx, char *out, int cap) {
    if (idx < 0 || idx >= g_icon_count || !out || cap <= 0) return -1;
    int i = 0;
    while (g_icons[idx].key[i] && i < cap - 1) { out[i] = g_icons[idx].key[i]; i++; }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

void desktop_set_icon_pos_by_key(const char *key, int axis, int v) {
    if (!key || !key[0]) return;
    for (int i = 0; i < g_icon_count; i++) {
        if (strcmp(g_icons[i].key, key) != 0) continue;
        if (axis == 0) g_icons[i].px = v; else g_icons[i].py = v;
        clamp_icon(&g_icons[i].px, &g_icons[i].py);
        g_icons[i].pos_set = 1;   // (#745) restored from the profile: authoritative
        return;
    }
    // No match: a saved position for a file that no longer exists. Dropping it
    // is correct; it is re-derived if the file comes back.
}

// (#745) Folds the icon KEY and KIND in, not just the coordinates. profile_tick()
// saves only when this changes, so without the key a rename that happened to
// leave the icon at the same coordinates would never be written back. This is
// still ONE term (prime 157) in profile_tick()'s hash; no new term is added, so
// the every-term-a-unique-prime invariant is untouched.
//
// Masked to 22 bits on the way out: profile_tick() multiplies this by 157 and
// adds it to a plain int, and an unbounded 32-bit value there would overflow.
int desktop_positions_hash(void) {
    unsigned h = 2166136261u;
    for (int i = 0; i < g_icon_count; i++) {
        h = fnv1a(g_icons[i].key, h);
        h = h * 31u + (unsigned)(g_icons[i].px + 1);
        h = h * 31u + (unsigned)(g_icons[i].py + 1);
        h = h * 31u + (unsigned)g_icons[i].kind;
    }
    return (int)(h & 0x3FFFFFu);
}

// ============================================================================
// Drag + rubber-band state machine (driven per-frame from main.c)
// ============================================================================

bool desktop_is_dragging(void) { return g_drag_mode != 0; }

// Left button just pressed. Returns true when the icon layer should treat the
// event as consumed for this frame.
bool desktop_press(int32_t x, int32_t y) {
    g_press_x    = x;
    g_press_y    = y;
    g_drag_moved = false;

    int hit = find_icon_at(x, y);
    if (hit >= 0) {
        // Press on an icon: begin a (potential) drag.
        // If the icon is not part of the current selection, make it the sole
        // selection. If it is already selected, keep the whole selection so a
        // multi-drag moves everything together.
        if (!g_icons[hit].selected) {
            clear_selection();
            g_icons[hit].selected = true;
        }
        g_selected_icon = hit;
        g_drag_anchor   = hit;
        g_drag_off_x    = x - g_icons[hit].px;
        g_drag_off_y    = y - g_icons[hit].py;
        g_drag_mode     = 1;
        g_needs_redraw  = true;
        return true;
    }

    // Press on empty desktop: clear selection and start a rubber-band.
    clear_selection();
    g_last_click_icon = -1;
    g_drag_anchor     = -1;
    g_drag_mode       = 2;
    g_band_x = x; g_band_y = y; g_band_w = 0; g_band_h = 0;
    g_needs_redraw    = true;
    return true;
}

// Mouse moved while the left button is held.
void desktop_drag(int32_t x, int32_t y) {
    if (g_drag_mode == 0) return;

    int dx = x - g_press_x;
    int dy = y - g_press_y;
    if (!g_drag_moved && (dx*dx + dy*dy) >= DRAG_DEAD_ZONE * DRAG_DEAD_ZONE) {
        g_drag_moved = true;
    }

    if (g_drag_mode == 1) {
        if (!g_drag_moved) return;   // still within the dead zone: not a real drag yet
        if (g_drag_anchor < 0) return;

        int32_t nx = x - g_drag_off_x;
        int32_t ny = y - g_drag_off_y;
        int32_t want_dx = nx - g_icons[g_drag_anchor].px;
        int32_t want_dy = ny - g_icons[g_drag_anchor].py;

        if (selection_count() > 1) {
            // Move the whole selection by the same delta, clamping each.
            for (int i = 0; i < g_icon_count; i++) {
                if (!g_icons[i].visible || !g_icons[i].selected) continue;
                int32_t ix = g_icons[i].px + want_dx;
                int32_t iy = g_icons[i].py + want_dy;
                clamp_icon(&ix, &iy);
                g_icons[i].px = ix;
                g_icons[i].py = iy;
            }
        } else {
            clamp_icon(&nx, &ny);
            g_icons[g_drag_anchor].px = nx;
            g_icons[g_drag_anchor].py = ny;
        }
        g_needs_redraw = true;
        return;
    }

    if (g_drag_mode == 2) {
        // Update rubber-band rectangle (normalize to positive w/h).
        int32_t x0 = g_press_x, y0 = g_press_y;
        int32_t x1 = x,         y1 = y;
        if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
        if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }
        g_band_x = x0; g_band_y = y0;
        g_band_w = x1 - x0; g_band_h = y1 - y0;

        // Live-select icons intersecting the band.
        for (int i = 0; i < g_icon_count; i++) {
            if (!g_icons[i].visible) { g_icons[i].selected = false; continue; }
            int32_t ax = g_icons[i].px, ay = g_icons[i].py;
            int32_t aw = icon_box_w(), ah = icon_box_h();
            bool isect = !(ax + aw <= g_band_x || ax >= g_band_x + g_band_w ||
                           ay + ah <= g_band_y || ay >= g_band_y + g_band_h);
            g_icons[i].selected = isect;
        }
        g_needs_redraw = true;
        return;
    }
}

// Left button released: finish the drag/rubber-band, or treat a no-move press as
// a click (select; double-click launches).
void desktop_release(int32_t x, int32_t y) {
    int mode = g_drag_mode;
    g_drag_mode = 0;

    if (mode == 2) {
        // Rubber-band already applied live during drag; just clear the rect.
        g_band_w = g_band_h = 0;
        g_needs_redraw = true;
        return;
    }

    if (mode == 1) {
        if (g_drag_moved) {
            // A real drag happened: positions are already updated. profile_tick
            // picks up the change via desktop_positions_hash and persists it.
            g_band_w = g_band_h = 0;
            g_needs_redraw    = true;
            g_last_click_icon = -1;   // a drop is never a double-click
            return;
        }

        // No movement: treat as a click on the anchor icon.
        int hit = g_drag_anchor;
        if (hit < 0) hit = find_icon_at(x, y);
        if (hit < 0) { g_needs_redraw = true; return; }

        uint64_t now = (uint64_t)sys_clock();
        bool should_launch = (hit == g_last_click_icon &&
                              (now - g_last_click_time) < 500ULL);

        // Single-click selects only this icon.
        clear_selection();
        g_icons[hit].selected = true;
        g_selected_icon  = hit;
        g_needs_redraw   = true;

        if (should_launch) {
            activate_icon(hit);
            clear_selection();          /* deselect after opening */
            g_last_click_icon = -1;
            g_last_click_time = 0;
        } else {
            g_last_click_icon = hit;
            g_last_click_time = now;
        }
    }
}

// ============================================================================
// desktop_handle_mouse (legacy entry: right-click + explicit double-click)
// ============================================================================

void desktop_handle_mouse(int32_t x, int32_t y,
                          bool left_click, bool right_click, bool dbl_click) {
    // Right-click: open context menu at cursor position.
    if (right_click) {
        contextmenu_open(x, y);
        return;
    }

    // Explicit kernel double-click on an icon launches immediately. (The press/
    // drag/release path in main.c handles single clicks, selection, and drags.)
    if (dbl_click) {
        int hit = find_icon_at(x, y);
        if (hit >= 0) {
            clear_selection();
            g_icons[hit].selected = true;
            g_selected_icon = hit;
            activate_icon(hit);
            g_last_click_icon = -1;
            g_last_click_time = 0;
            g_needs_redraw = true;
        }
        return;
    }

    // Plain left_click without the press/drag path (should not normally happen
    // since main.c routes presses through desktop_press). Fall back to a select.
    if (left_click) {
        int hit = find_icon_at(x, y);
        if (hit < 0) { clear_selection(); g_last_click_icon = -1; }
        else { clear_selection(); g_icons[hit].selected = true; g_selected_icon = hit; }
        g_needs_redraw = true;
    }
}

#ifdef MAYTERA_TESTHOOK
// #334 headless verification hook ONLY - see compositor.h's declaration and
// testhook.c for the full explanation. Never compiled into a normal build
// (see the Makefile's TESTHOOK guard). Deliberately calls launch_app()
// directly instead of computing the icon's screen rect and injecting a
// click at it: the whole point of a name-driven hook is to bypass
// hit-testing, not to reimplement it.
bool desktop_launch_icon_by_name(const char *name) {
    if (!name || !name[0]) return false;
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible) continue;
        if (strcmp(g_icons[i].name, name) == 0) {
            clear_selection();
            g_icons[i].selected = true;
            g_selected_icon = i;
            activate_icon(i);
            g_needs_redraw = true;
            return true;
        }
    }
    return false;
}
#endif
