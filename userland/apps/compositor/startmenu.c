// startmenu.c - Accordion-style start menu for the MayteraOS userland compositor.
// Renders a vertically stacked list of expandable category headers. Each header
// can be clicked to reveal or hide its application items. A fixed power section
// at the bottom provides Restart and Shutdown buttons. No malloc; all state is
// stored in static arrays sized at compile time.

#include "compositor.h"
#include "../../libc/syscall.h"

// ============================================================================
// Static state
// ============================================================================

#define MAX_CATEGORIES 14  // 3 built-in + data-driven groups + Win16 program groups

static menu_category_t g_categories[MAX_CATEGORIES];
static menu_item_t     g_menu_items[START_MENU_MAX_ITEMS];
static int             g_total_items;
static int             g_hover_item;   // index into g_menu_items, or -1
static int             g_hover_cat;    // index into g_categories, or -1
// Next free category slot after the 3 built-ins. Both the data-driven
// STARTMENU.YAML loader and the Win16 program-group loader append categories
// here, so it is shared instead of each hardcoding slot 3.
static int             g_next_cat = 3;

// Power button hover: 0 = none, 1 = Restart, 2 = Shutdown
static int g_hover_power;

// ============================================================================
// Internal helpers
// ============================================================================

// Launch types (must match menu_item_t.launch_type).
#define LAUNCH_NATIVE 0   // sys_spawn() a native ELF
#define LAUNCH_WIN16  1   // win16_run() a Win16 NE/.COM
#define LAUNCH_DOS    2   // dos_run() an MS-DOS .EXE/.COM (#208)

// Append one item to g_menu_items. The item is associated with whichever
// category was most recently registered, so all of a category's items must be
// added before the next add_category() call (they must stay contiguous).
static void add_item_typed(const char *name, icon_id_t icon, const char *path, int launch_type)
{
    if (g_total_items >= START_MENU_MAX_ITEMS)
        return;

    menu_item_t *it = &g_menu_items[g_total_items];
    strncpy(it->name,      name, sizeof(it->name) - 1);
    it->name[sizeof(it->name) - 1] = '\0';
    strncpy(it->exec_path, path, sizeof(it->exec_path) - 1);
    it->exec_path[sizeof(it->exec_path) - 1] = '\0';
    it->icon_id      = icon;
    it->is_separator = false;
    it->is_win16     = (launch_type == LAUNCH_WIN16);
    it->launch_type  = launch_type;

    // Increment the item_count of the last registered category.
    int last_cat = -1;
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_categories[i].label[0] != '\0')
            last_cat = i;
    }
    if (last_cat >= 0)
        g_categories[last_cat].item_count++;

    g_total_items++;
}

static void add_item_ex(const char *name, icon_id_t icon, const char *path, bool is_win16)
{
    add_item_typed(name, icon, path, is_win16 ? LAUNCH_WIN16 : LAUNCH_NATIVE);
}

static void add_item(const char *name, icon_id_t icon, const char *path)
{
    add_item_typed(name, icon, path, LAUNCH_NATIVE);
}

// Register a category. Must be called before add_item() calls for that group.
static void add_category(int index, const char *label, bool expanded)
{
    menu_category_t *cat = &g_categories[index];
    strncpy(cat->label, label, sizeof(cat->label) - 1);
    cat->label[sizeof(cat->label) - 1] = '\0';
    cat->expanded   = expanded;
    cat->item_start = g_total_items;
    cat->item_count = 0;
}

// Calculate the total pixel height of the menu given current expanded state.
// Layout: for each category, CAT_H header + (ITEM_H * visible_items if expanded),
// with a SEP_H gap drawn between categories, and POWER_H at the very bottom.
static int32_t calc_menu_height(void)
{
    int32_t h = 0;
    bool first = true;
    for (int c = 0; c < MAX_CATEGORIES; c++) {
        if (g_categories[c].label[0] == '\0')
            continue;   // unused slot: no header, no separator
        if (!first)
            h += START_MENU_SEP_H;
        first = false;
        h += START_MENU_CAT_H;
        if (g_categories[c].expanded)
            h += START_MENU_ITEM_H * g_categories[c].item_count;
    }
    h += START_MENU_POWER_H;
    return h;
}

// ============================================================================
// Public API
// ============================================================================


// ----------------------------------------------------------------------------
// Win16 program groups
// ----------------------------------------------------------------------------
// The native MayteraOS "Win3.x installer" (RC `win16install`) writes a simple
// line-oriented config file that this compositor reads at start-menu init. Each
// program group becomes one collapsible accordion category whose items launch a
// Win16 (NE/.COM) executable via the win16_run() syscall rather than spawning a
// native ELF.
//
// /WIN16GRP.CFG format (one record per line, fields separated by '|'):
//   GROUP|<group display name>
//   ITEM|<item display name>|<exec path>
// ITEM lines apply to the most recently declared GROUP. Lines starting with '#'
// and blank lines are ignored. Categories are labelled "PROGRAMS \\ <group>" so
// they read as a nested program group under Programs.

static char *sm_trim(char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    int n = 0; while (p[n]) n++;
    while (n > 0 && (p[n-1] == ' ' || p[n-1] == '\t' ||
                     p[n-1] == '\r' || p[n-1] == '\n')) p[--n] = 0;
    return p;
}

// Split a config line on the next '|'. Returns the field (trimmed) and advances
// *pp past the '|' (or to the terminating NUL when no '|' is left).
static char *sm_next_field(char **pp)
{
    char *s = *pp;
    char *bar = s;
    while (*bar && *bar != '|') bar++;
    if (*bar == '|') { *bar = 0; *pp = bar + 1; }
    else             { *pp = bar; }
    return sm_trim(s);
}

// #208: Load extra retro-game entries into the GAMES category from /GAMES.CFG.
// The GAMES category must be the most recently registered category when this is
// called (items stay contiguous). Format, one record per line:
//   ITEM|<display name>|<exec path>|<type>
// where <type> is "native", "win16" or "dos" (default win16 if omitted). The
// per-item type selects sys_spawn / win16_run / dos_run at launch. This lets the
// retro games (EP Win16 titles, SkiFree, plus DOS TIM and Keen) all live under
// one Games submenu while launching through the right loader. Lines beginning
// with '#' and blank lines are ignored. If the file is absent the built-in
// defaults wired in startmenu_init() are used as-is.
static void startmenu_load_games_cfg(void)
{
    int fd = sys_open("/GAMES.CFG", 0);
    if (fd < 0) fd = sys_open("/CONFIG/GAMES.CFG", 0);
    if (fd < 0) return;

    static char buf[4096];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
        char *t = sm_trim(line);
        if (t[0] == 0 || t[0] == '#') continue;
        if (strncmp(t, "ITEM|", 5) != 0) continue;

        char *cur  = t + 5;
        char *name = sm_next_field(&cur);
        char *path = sm_next_field(&cur);
        char *type = sm_next_field(&cur);   // may be ""
        if (!name[0] || !path[0]) continue;

        int       lt = LAUNCH_WIN16;
        icon_id_t ic = ICON_WIN3X;
        if (strcmp(type, "dos") == 0)        { lt = LAUNCH_DOS;    ic = ICON_DOSAPP; }
        else if (strcmp(type, "native") == 0){ lt = LAUNCH_NATIVE; ic = ICON_GAME;   }
        add_item_typed(name, ic, path, lt);
    }
}

// Map an optional icon name from STARTMENU.YAML to an icon id. Unknown or
// missing -> a generic window icon.
static icon_id_t sm_icon_by_name(const char *n)
{
    if (!n || !n[0])                  return ICON_WINDOW;
    if (strcmp(n, "clock")      == 0) return ICON_CLOCK;
    if (strcmp(n, "calculator") == 0) return ICON_CALCULATOR;
    if (strcmp(n, "image")      == 0) return ICON_IMAGE;
    if (strcmp(n, "network")    == 0) return ICON_NETWORK;
    if (strcmp(n, "cog")        == 0) return ICON_COG;
    if (strcmp(n, "task")       == 0) return ICON_TASK_MANAGER;
    if (strcmp(n, "log")        == 0) return ICON_LOG_VIEWER;
    return ICON_WINDOW;
}

// #454: Data-driven Start-menu groups. Reads a small YAML-subset file so new
// native apps can be added to the menu without recompiling the compositor.
// Each `category:` line opens a new collapsible group; each `item:` line adds
// an entry to the current group. Format:
//   # comment
//   category: Utilities
//   item: Launcher | /APPS/LAUNCHER
//   item: Timers   | /APPS/TIMERS | clock
// The optional third '|' field on an item is an icon name (see sm_icon_by_name).
// New categories are appended after the built-ins using the shared g_next_cat
// slot counter (also used by the Win16 program-group loader). If the file is
// absent nothing is added and the built-in menu is used as-is.
static void startmenu_load_apps_cfg(void)
{
    /* Primary name is 8.3-compliant (STARTMNU.YML) so the FAT driver can find
     * it by short name on the FAT-only live-USB image; the long STARTMENU.YAML
     * and /ext2/ variants are fallbacks for ext2-root installs. */
    static const char *const paths[] = {
        "/CONFIG/STARTMNU.YML",       "/STARTMNU.YML",
        "/ext2/CONFIG/STARTMNU.YML",  "/ext2/STARTMNU.YML",
        "/CONFIG/STARTMENU.YAML",     "/STARTMENU.YAML",
        "/ext2/CONFIG/STARTMENU.YAML","/ext2/STARTMENU.YAML",
    };
    static char buf[4096];
    long n = -1;
    for (unsigned pi = 0; pi < sizeof(paths) / sizeof(paths[0]); pi++) {
        int fd = sys_open(paths[pi], 0);
        if (fd < 0) continue;
        n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) break;
    }
    if (n <= 0) return;
    buf[n] = 0;

    bool have_cat = false;
    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
        char *t = sm_trim(line);
        if (t[0] == 0 || t[0] == '#') continue;

        if (strncmp(t, "category:", 9) == 0) {
            char *name = sm_trim(t + 9);
            if (!name[0]) { have_cat = false; continue; }
            if (g_next_cat >= MAX_CATEGORIES) { have_cat = false; continue; }
            add_category(g_next_cat, name, false);   // collapsed by default
            g_next_cat++;
            have_cat = true;
        } else if (strncmp(t, "item:", 5) == 0) {
            if (!have_cat) continue;   // item before any category: skip
            char *cur  = t + 5;
            char *name = sm_next_field(&cur);
            char *path = sm_next_field(&cur);
            char *icon = sm_next_field(&cur);   // optional
            if (!name[0] || !path[0]) continue;
            add_item_typed(name, sm_icon_by_name(icon), path, LAUNCH_NATIVE);
        }
    }
}

// #134: Load Win16 program groups from /WIN16GRP.CFG and render each as its own
// collapsible Start-menu category (folder), with items launched via win16_run().
// The ole2c kernel's Win3.x installer writes this file to the ext2 root. Format,
// one record per line (fields separated by '|'):
//   GROUP|<group display name>
//   ITEM|<item display name>|<exec path>[|<icon, ignored>]
// ITEM lines belong to the most recently declared GROUP. Lines beginning with
// '#' and blank lines are ignored. Groups occupy category slots after the three
// built-in ones (APPLICATIONS/GAMES/SYSTEM), up to MAX_CATEGORIES. If the file
// is absent nothing is added and the built-in menu is used as-is.
static void startmenu_load_win16_groups(void)
{
    // Try several locations. On the ole2c kernel the config lives at the ext2
    // root; the explicit "/ext2/..." forms bypass the root-fs routing so the
    // read still succeeds regardless of how paths are resolved.
    static const char *const paths[] = {
        "/WIN16GRP.CFG", "/ext2/WIN16GRP.CFG",
        "/CONFIG/WIN16GRP.CFG", "/ext2/CONFIG/WIN16GRP.CFG",
    };
    static char buf[8192];
    long n = -1;
    for (unsigned pi = 0; pi < sizeof(paths) / sizeof(paths[0]); pi++) {
        int fd = sys_open(paths[pi], 0);
        if (fd < 0) continue;
        n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) break;
    }
    if (n <= 0) return;
    buf[n] = 0;

    // Next free category slot is shared (g_next_cat): the built-ins take 0..2
    // and the data-driven STARTMENU.YAML loader may already have consumed some.
    bool have_group = false;

    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
        char *t = sm_trim(line);
        if (t[0] == 0 || t[0] == '#') continue;

        if (strncmp(t, "GROUP|", 6) == 0) {
            char *cur  = t + 6;
            char *name = sm_next_field(&cur);
            if (!name[0]) { have_group = false; continue; }
            if (g_next_cat >= MAX_CATEGORIES) { have_group = false; continue; }
            add_category(g_next_cat, name, false);   // collapsed by default
            g_next_cat++;
            have_group = true;
        } else if (strncmp(t, "ITEM|", 5) == 0) {
            if (!have_group) continue;   // ITEM before any GROUP: skip
            char *cur  = t + 5;
            char *name = sm_next_field(&cur);
            char *path = sm_next_field(&cur);
            // Optional 4th icon field is ignored; all Win16 items use ICON_WIN3X.
            if (!name[0] || !path[0]) continue;
            add_item_typed(name, ICON_WIN3X, path, LAUNCH_WIN16);
        }
        // Any other record type is ignored.
    }
}

void startmenu_init(void)
{
    memset(g_categories, 0, sizeof(g_categories));
    memset(g_menu_items,  0, sizeof(g_menu_items));
    g_total_items = 0;
    g_hover_item  = -1;
    g_hover_cat   = -1;
    g_hover_power = 0;
    g_next_cat    = 5;   // built-ins occupy 0..4; loaders append from here

    // Consolidated categories: the old catch-all "Applications" is dissolved
    // into thematic groups, and the near-duplicate "System"/"System Tools" are
    // merged into one "System". The ten review-pass apps live in their thematic
    // homes. Special launch types (Recycle Bin, Win16/DOS games) stay hardcoded.

    // Category 0: Internet
    add_category(0, "Internet", false);
    add_item("Browser",   ICON_BROWSER,  "/APPS/browser");
    add_item("IRC",       ICON_IRC,      "/APPS/irc");
    add_item("AI Chat",   ICON_IRC,      "/APPS/aichat");
    add_item("Weather",   ICON_NETWORK,  "/APPS/WEATHER");
    add_item("Feeds",     ICON_NETWORK,  "/APPS/RSS");

    // Category 1: Media
    add_category(1, "Media", false);
    add_item("Paint",        ICON_PAINT, "/APPS/paint");
    add_item("Image Viewer", ICON_IMAGE, "/APPS/imgview");
    add_item("Gallery",      ICON_IMAGE, "/APPS/GALLERY");
    add_item("Media Player", ICON_VIDEO, "/APPS/mplayer");
    add_item("Music Player", ICON_MUSIC, "/APPS/musicplr");
    add_item("Snapshot",     ICON_IMAGE, "/APPS/SNAPSHOT");

    // Category 2: Accessories (general tools + productivity), expanded by default
    add_category(2, "Accessories", true);
    add_item("Terminal",      ICON_TERMINAL,   "/APPS/terminal");
    add_item("Files",         ICON_FOLDER,     "/APPS/files");
    add_item("Editor",        ICON_HIGHLIGHT,  "/APPS/editor");
    add_item("Notes",         ICON_HIGHLIGHT,  "/APPS/notes");
    add_item("Calculator",    ICON_CALCULATOR, "/APPS/calc");
    add_item("Converter",     ICON_CALCULATOR, "/APPS/CONVERT");
    add_item("World Clock",   ICON_CLOCK,      "/APPS/clock");
    add_item("Timers",        ICON_CLOCK,      "/APPS/TIMERS");
    add_item("Authenticator", ICON_FILE,       "/APPS/mfa");
    add_item("Launcher",      ICON_WINDOW,     "/APPS/LAUNCHER");
    add_item("Task Switcher", ICON_WINDOW,     "/APPS/WINSWTCH");
    add_item("Python",        ICON_TERMINAL,   "/APPS/PYTHON.ELF");
    add_item("Help",          ICON_FILE,       "/APPS/help");

    // Category 3: GAMES (collapsed by default). One unified submenu holding the
    // native games, the Win16 Entertainment Pack titles + SkiFree (win16_run),
    // and the DOS games TIM + Commander Keen 5 (dos_run). See #208.
    add_category(3, "Games", false);
    // Native (sys_spawn).
    add_item("Maytera Arena",    ICON_GAME,       "/APPS/ARENA");     // TinyGL arena FPS
    add_item("Maytera Chess",    ICON_GAME,       "/APPS/CHESS");     // 3D TinyGL chess
    add_item("Maytera Squadron", ICON_GAME,       "/APPS/SQUADRON");  // 1942-style vertical shmup
    // #537: DOOM lives at /GAMES/DOOM/DOOM.ELF (alongside its DOOM1.WAD); the old
    // /APPS/DOOM.ELF path did not exist (the file is /APPS/DOOM, no .ELF), so the
    // start-menu entry launched nothing. Point it where the binary + WAD actually are.
    add_item("DOOM",         ICON_GAME_DOOM,      "/GAMES/DOOM/DOOM.ELF");
    add_item("Lemmings",     ICON_GAME_LEMMINGS,  "/APPS/lemmings");
    add_item("Solitaire",    ICON_GAME_SOLITAIRE, "/APPS/solitr");
    add_item("Pong",         ICON_GAME_PONG,      "/APPS/pong");
    add_item("GL Cube",      ICON_GAME,           "/APPS/GLCUBE");    /* #319 TinyGL demo (reconciled #336) */
    add_item("GL Matrix",    ICON_GAME,           "/APPS/GLMATRIX");  /* #319 TinyGL demo (reconciled #336) */
    // Win16 Entertainment Pack (win16_run, WIN3X icon).
    add_item_typed("Tetris",            ICON_WIN3X, "/WIN16/MSEP/TETRIS.EXE",   LAUNCH_WIN16);
    add_item_typed("Chips Challenge",   ICON_WIN3X, "/WIN16/MSEP/CHIPS.EXE",    LAUNCH_WIN16);
    add_item_typed("FreeCell",          ICON_WIN3X, "/WIN16/MSEP/FREECELL.EXE", LAUNCH_WIN16);
    add_item_typed("Golf",              ICON_WIN3X, "/WIN16/MSEP/GOLF.EXE",     LAUNCH_WIN16);
    add_item_typed("JezzBall",          ICON_WIN3X, "/WIN16/MSEP/JEZZBALL.EXE", LAUNCH_WIN16);
    add_item_typed("TetraVex",          ICON_WIN3X, "/WIN16/MSEP/TETRAVEX.EXE", LAUNCH_WIN16);
    add_item_typed("Rodent's Revenge",  ICON_WIN3X, "/WIN16/MSEP/RODENT.EXE",   LAUNCH_WIN16);
    add_item_typed("Tut's Tomb",        ICON_WIN3X, "/WIN16/MSEP/TUTSTOMB.EXE", LAUNCH_WIN16);
    add_item_typed("SkiFree",           ICON_WIN3X, "/WIN16/EP3/SKI.EXE",       LAUNCH_WIN16);
    // DOS games (dos_run, DOSAPP icon).
    add_item_typed("The Incredible Machine", ICON_DOSAPP, "/DOS/TIM/TIM.EXE",      LAUNCH_DOS);
    add_item_typed("Commander Keen 5",       ICON_DOSAPP, "/DOS/KEEN5/KEEN5E.EXE", LAUNCH_DOS);
    // Optional extra entries from /GAMES.CFG (typed). Appended into GAMES; must
    // run while GAMES is still the most recently registered category.
    startmenu_load_games_cfg();

    // Category 4: System (collapsed). Merges the old "System" + "System Tools".
    add_category(4, "System", false);
    add_item("Settings",       ICON_COG,          "/APPS/settings");
    add_item("Font Book",      ICON_HIGHLIGHT,    "/APPS/FONTBOOK");
    add_item("Network",        ICON_NETWORK,      "/APPS/network");
    add_item("Task Manager",   ICON_TASK_MANAGER, "/APPS/taskmgr");
    add_item("System Monitor", ICON_TASK_MANAGER, "/APPS/SYSMON");
    add_item("Services",       ICON_COG,          "/APPS/SVCMGR");
    add_item("Device Manager", ICON_COMPUTER,     "/APPS/DEVMGR");
    add_item("App Store",      ICON_WINDOW,       "/APPS/APPSTORE");
    add_item("System Log",     ICON_LOG_VIEWER,   "/APPS/syslog");
    add_item("3D Print",       ICON_PALETTE,      "/APPS/PRINT3D");
    add_item("Recycle Bin",    ICON_TRASH,        "@RECYCLE");

    // #454: Data-driven extra groups from /CONFIG/STARTMNU.YML (native apps
    // added without recompiling the compositor). Appends new categories after
    // the built-ins; must run before the Win16 loader so they share g_next_cat.
    startmenu_load_apps_cfg();

    // #134: Win16 program groups (ProgMan) as extra Start-menu folders. Read
    // from /WIN16GRP.CFG (written by the ole2c kernel's Win3.x installer). Each
    // GROUP becomes a collapsible category whose items launch via win16_run().
    startmenu_load_win16_groups();
}

void startmenu_render(void)
{
    if (!g_start_menu_open)
        return;

    int32_t w  = START_MENU_WIDTH;
    int32_t mh = calc_menu_height();
    int32_t mx = TASKBAR_PADDING;
    // #387: top-bar layouts (Lumina/Retro Bench) drop the menu down from the top bar.
    int32_t my = taskbar_menu_drops_from_top()
               ? taskbar_top_inset() + 2
               : taskbar_get_y() - mh - 4;

    // Keep menu on screen if taskbar is at the top.
    if (my < 0)
        my = 0;

    // Shadow
    draw_fill_rect(mx + 3, my + 3, w, mh, CLR_MENU_SHADOW);

    // Background
    draw_fill_rect(mx, my, w, mh, CLR_MENU_BG);

    // Border
    draw_rect_outline(mx, my, w, mh, CLR_MENU_BORDER);

    int32_t cy = my;  // running y cursor
    bool first_cat = true;

    for (int c = 0; c < MAX_CATEGORIES; c++) {
        menu_category_t *cat = &g_categories[c];

        // Skip unused category slots so they do not render as blank sections.
        if (cat->label[0] == '\0')
            continue;

        // Separator line between visible categories (not before the first one).
        if (!first_cat) {
            draw_hline(mx + 8, cy, w - 16, CLR_MENU_SEP);
            cy += START_MENU_SEP_H;
        }
        first_cat = false;

        // Category header background.
        uint32_t cat_bg = cat->expanded ? CLR_MENU_CAT_BG : CLR_MENU_BG;
        if (g_hover_cat == c)
            cat_bg = CLR_MENU_ITEM_HOVER;
        draw_fill_rect(mx, cy, w, START_MENU_CAT_H, cat_bg);

        // Category label (left-padded).
        draw_text(mx + START_MENU_PADDING + 4,
                  cy + (START_MENU_CAT_H - FONT_CHAR_H) / 2,
                  cat->label, CLR_MENU_TEXT);

        // Expand/collapse indicator: chevron icon (down=expanded, right=collapsed).
        if (!icon_draw_color_tinted(cat->expanded ? ICON_CHEVD : ICON_CHEVR,
                mx + w - START_MENU_PADDING - 16, cy + (START_MENU_CAT_H - 14) / 2,
                14, CLR_MENU_TEXT))
            draw_text(mx + w - START_MENU_PADDING - FONT_CHAR_W - 2,
                      cy + (START_MENU_CAT_H - FONT_CHAR_H) / 2,
                      cat->expanded ? "v" : ">", CLR_MENU_TEXT);

        cy += START_MENU_CAT_H;

        // Items for this category, drawn only when expanded.
        if (cat->expanded) {
            for (int i = 0; i < cat->item_count; i++) {
                int item_idx = cat->item_start + i;
                menu_item_t *it = &g_menu_items[item_idx];

                // Hover highlight.
                uint32_t item_bg = CLR_MENU_BG;
                if (g_hover_item == item_idx)
                    item_bg = CLR_MENU_ITEM_HOVER;
                draw_fill_rect(mx, cy, w, START_MENU_ITEM_H, item_bg);

                // Icon (20x20, vertically centred in the row).
                int32_t icon_y = cy + (START_MENU_ITEM_H - 20) / 2;
                icon_draw_scaled(it->icon_id, mx + 8, icon_y, 20, CLR_MENU_TEXT);

                // App name text.
                draw_text(mx + 34,
                          cy + (START_MENU_ITEM_H - FONT_CHAR_H) / 2,
                          it->name, CLR_MENU_TEXT);

                cy += START_MENU_ITEM_H;
            }
        }
    }

    // Power section separator.
    draw_hline(mx + 8, cy, w - 16, CLR_MENU_SEP);

    // Power section background.
    draw_fill_rect(mx, cy, w, START_MENU_POWER_H, CLR_MENU_BG);

    // Two side-by-side buttons inside the power section.
    int32_t btn_w   = (w - 24) / 2;
    int32_t btn_h   = START_MENU_POWER_H - 8;
    int32_t btn_y   = cy + 4;
    int32_t btn_x1  = mx + 8;
    int32_t btn_x2  = mx + 8 + btn_w + 8;

    // Restart button.
    uint32_t rst_bg = (g_hover_power == 1) ? CLR_MENU_ITEM_HOVER : CLR_MENU_ITEM_NORM;
    draw_fill_rect(btn_x1, btn_y, btn_w, btn_h, rst_bg);
    draw_rect_outline(btn_x1, btn_y, btn_w, btn_h, CLR_MENU_BORDER);
    icon_draw_scaled(ICON_REFRESH,
                     btn_x1 + 4,
                     btn_y + (btn_h - 14) / 2,
                     14, CLR_MENU_TEXT);
    draw_text(btn_x1 + 22,
              btn_y + (btn_h - FONT_CHAR_H) / 2,
              "Restart", CLR_MENU_TEXT);

    // Shutdown button.
    uint32_t sdn_bg = (g_hover_power == 2) ? CLR_MENU_ITEM_HOVER : CLR_MENU_ITEM_NORM;
    draw_fill_rect(btn_x2, btn_y, btn_w, btn_h, sdn_bg);
    draw_rect_outline(btn_x2, btn_y, btn_w, btn_h, CLR_MENU_BORDER);
    icon_draw_scaled(ICON_POWER,
                     btn_x2 + 4,
                     btn_y + (btn_h - 14) / 2,
                     14, CLR_POWER_RED);
    draw_text(btn_x2 + 22,
              btn_y + (btn_h - FONT_CHAR_H) / 2,
              "Shutdown", CLR_MENU_TEXT);
}

bool startmenu_handle_mouse(int32_t x, int32_t y, bool clicked)
{
    if (!g_start_menu_open)
        return false;

    int32_t w  = START_MENU_WIDTH;
    int32_t mh = calc_menu_height();
    int32_t mx = TASKBAR_PADDING;
    // #387: top-bar layouts (Lumina/Retro Bench) drop the menu down from the top bar.
    int32_t my = taskbar_menu_drops_from_top()
               ? taskbar_top_inset() + 2
               : taskbar_get_y() - mh - 4;
    if (my < 0)
        my = 0;

    // Outside menu bounds: a mouse-DOWN dismisses the menu (click-away to close),
    // consumed so the same click does not also activate whatever sits underneath.
    // A hover/move outside is ignored (return false) so the rest of the UI stays
    // live. A click on the Start button itself also lands here (the button sits on
    // the taskbar, below the menu rect); closing + consuming here means the
    // taskbar's own Start-button toggle is skipped, so the menu just closes with
    // no double-toggle.
    if (x < mx || x >= mx + w || y < my || y >= my + mh) {
        if (clicked) {
            g_start_menu_open = false;
            g_hover_item  = -1;
            g_hover_cat   = -1;
            g_hover_power = 0;
            g_needs_redraw = true;
            return true;
        }
        return false;
    }

    // Reset hover state; we re-derive it below.
    g_hover_item  = -1;
    g_hover_cat   = -1;
    g_hover_power = 0;

    int32_t cy = my;
    bool first_cat = true;

    for (int c = 0; c < MAX_CATEGORIES; c++) {
        menu_category_t *cat = &g_categories[c];

        // Skip unused category slots (must match startmenu_render layout).
        if (cat->label[0] == '\0')
            continue;

        // Account for the separator gap before each visible category but the first.
        if (!first_cat)
            cy += START_MENU_SEP_H;
        first_cat = false;

        // Check category header region.
        if (y >= cy && y < cy + START_MENU_CAT_H) {
            g_hover_cat = c;
            if (clicked) {
                // Accordion: only ONE group open at a time. Collapse every
                // category, then open the clicked one (or leave all collapsed
                // if it was the one already open). Keeps the menu height bounded
                // so it never grows off the top of the screen.
                bool was = cat->expanded;
                for (int k = 0; k < MAX_CATEGORIES; k++)
                    g_categories[k].expanded = false;
                cat->expanded = !was;
                g_needs_redraw = true;
            }
            return true;
        }
        cy += START_MENU_CAT_H;

        // Check item rows when the category is expanded.
        if (cat->expanded) {
            for (int i = 0; i < cat->item_count; i++) {
                if (y >= cy && y < cy + START_MENU_ITEM_H) {
                    int item_idx = cat->item_start + i;
                    g_hover_item = item_idx;
                    if (clicked) {
                        // Launch the selected item (no fork; fork from the
                        // compositor hangs the OS). The per-item launch type
                        // selects the right loader (#208): native ELF via
                        // sys_spawn, Win16 NE/.COM via win16_run, MS-DOS .EXE
                        // via dos_run.
                        const char *path = g_menu_items[item_idx].exec_path;
                        switch (g_menu_items[item_idx].launch_type) {
                            case LAUNCH_WIN16: win16_run(path); break;
                            case LAUNCH_DOS:   dos_run(path);   break;
                            default:
                                if (path[0] == '@' && path[1] == 'R') {
                                    int fd = sys_open("/RECYVIEW.FLG", 0x41);
                                    if (fd >= 0) { sys_write(fd, "1", 1); sys_close(fd); }
                                    sys_spawn("/APPS/FILES");
                                } else sys_spawn(path);
                                break;
                        }
                        // Close the menu and request redraw.
                        g_start_menu_open = false;
                        g_hover_item      = -1;
                        g_hover_cat       = -1;
                        g_hover_power     = 0;
                        g_needs_redraw    = true;
                    }
                    return true;
                }
                cy += START_MENU_ITEM_H;
            }
        }
    }

    // Power section: starts at cy (after all categories and their separators).
    int32_t btn_w  = (w - 24) / 2;
    int32_t btn_h  = START_MENU_POWER_H - 8;
    int32_t btn_y  = cy + 4;
    int32_t btn_x1 = mx + 8;
    int32_t btn_x2 = mx + 8 + btn_w + 8;

    if (y >= btn_y && y < btn_y + btn_h) {
        if (x >= btn_x1 && x < btn_x1 + btn_w) {
            // Restart button.
            g_hover_power = 1;
            if (clicked) {
                g_start_menu_open = false;
                g_needs_redraw    = true;
                // Real reboot: kernel shows the splash then issues ACPI reset.
                reboot();
            }
            return true;
        }
        if (x >= btn_x2 && x < btn_x2 + btn_w) {
            // Shutdown button.
            g_hover_power = 2;
            if (clicked) {
                g_start_menu_open = false;
                g_needs_redraw    = true;
                // Real shutdown: kernel shows the splash then ACPI/emulator power-off.
                poweroff();
            }
            return true;
        }
    }

    // Cursor is inside the menu bounds but over a gap or border: consume the
    // event so clicks do not pass through to the desktop underneath.
    return true;
}

void startmenu_toggle(void)
{
    g_start_menu_open = !g_start_menu_open;
    g_hover_item      = -1;
    g_hover_cat       = -1;
    g_hover_power     = 0;
    g_needs_redraw    = true;
}
