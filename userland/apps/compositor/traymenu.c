// traymenu.c - YAML-defined popup menus for the system-tray icons (#90).
// Clicking a tray icon opens a control menu (checkboxes / sliders / radios)
// whose structure is loaded from /APPS/TRAYMENU.YAML (a built-in default is
// used if the file is missing). Bindings map menu controls to live state:
// sheep show/speed/size/style, widget visibility, volume + EQ bands.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/bt_client.h"   // #372: Bluetooth tray menu (power toggle + settings)
#include "../../libc/wifi_client.h" // #384: Network/Wi-Fi tray menu (wifi toggle + settings)

// #129: SETTINGS_PANEL_* (compositor.h) are the named panel indices now -
// settings_open_panel() is THE way to open Settings on a specific panel
// (singleton-safe: focuses an already-open Settings window instead of
// spawning a duplicate one, see compositor.h/taskbar.c for the full "why").

// (#231r) The faceplate's fader bank is hand-laid for exactly this many
// columns (see snd_fx() and the SND_* geometry below), so this is NOT a
// free-floating count: if the kernel ever grows a sixth band, the panel
// geometry has to grow with it. snd_render()/snd_mouse() clamp the kernel's
// eq_band_count() to this, so a mismatch degrades to "the panel shows the
// five bands it has room for" rather than drawing off the end of its own
// face. (blame.md, #glassmodal: a bumped _COUNT breaking a hand-sized array
// three files away is a recurring shape in this tree.)
#define SND_EQ_BANDS 5

typedef enum { TM_ACTION = 0, TM_CHECK, TM_SLIDER, TM_RADIO } tm_type;
typedef struct {
    tm_type type;
    char    label[28];
    char    bind[20];
    int     vmin, vmax;
    char    opt[3][14];
    int     nopt;
} tm_item;
typedef struct {
    char    name[16];
    // #745 P2: 14 was sized for 13 widgets + the opacity slider. The registry
    // now carries 15 widgets (Dog was missing, see widgets.c), so this holds
    // 15 checks + 1 slider = 16. Bump this again if widget_registry() grows.
    tm_item items[16];
    int     n;
} tm_menu;

#define TM_MAX 6   // #372: + bluetooth section
static tm_menu g_tm[TM_MAX];
static int g_tm_count = 0;

// Open state (consumed by main.c render/input).
int g_tray_menu_open  = 0;
int g_tray_menu_which = -1;
int g_tray_menu_ax    = 0;
static int g_tm_drag = -1;     // index of slider being dragged

// (#231r) THE 5-BAND GRAPHIC EQ IS BACK, AND IT IS NOT A PROP THIS TIME.
//
// #231 (commit 8a5fcee5) deleted this control, and its reason was correct:
// the faders wrote "a static int g_eq[5] that only the fader itself reads -
// no EQ syscall exists and it is not even persisted", so dragging a band did
// nothing but move its own drawing. It also noted that five faders were
// bound to three YAML values (Bass/Mid/Treble on eq0/eq2/eq4, with eq1/eq3
// bound to nothing at all), which was itself evidence that nothing real was
// behind them.
//
// This restore SATISFIES that objection rather than reversing it. Every
// fader now drives SYS_AUDIO_EQ (413) -> rustkern/pcmeq.rs, five cascaded
// fixed-point RBJ biquads per channel applied post-mix in the kernel's PCM
// mixer, measured per band at boot and reported to /AUDIOLOG.TXT. There is
// no g_eq[] here any more: the KERNEL holds the state, which is the only way
// a tray control and the audio path cannot disagree.
//
// All FIVE bands are bound now, and the faceplate labels are DERIVED from
// the kernel's own centre frequencies (snd_band_label()), so the panel
// cannot claim a frequency the filter is not using - the exact three-vs-five
// mismatch #231 called out.
//
// Persistence is the profile (profile.c: eq0..eq4), which is hashed from
// profile_build()'s real serialized bytes, so these settings survive a
// reboot and cannot fall into the "applies live and vanishes at reboot" hole
// #231 fixed for the widget settings.

// ---- binding get/set -----------------------------------------------------
extern int g_win_opacity;  // main.c global window opacity (0-255)
extern int g_aichat_enabled;        // widgets.c (#185)
void aichat_set_enabled(int on);    // main.c: launch/stop the AI Chat app (#185)

extern int g_show_digclock;
extern int g_show_ha;   // #414
static int tm_get(const char *b) {
    if (!strcmp(b, "show_digclock")) return g_show_digclock;
    if (!strcmp(b, "sheep_show"))    return g_sheep_enabled;
    if (!strcmp(b, "sheep_speed"))   return g_sheep_speed;
    if (!strcmp(b, "sheep_size"))    return g_sheep_size;
    if (!strcmp(b, "sheep_style"))   return g_sheep_style;
    if (!strcmp(b, "sheep_count"))   return g_sheep_count;
    if (!strcmp(b, "dog_show"))      return g_dog_enabled;
    if (!strcmp(b, "show_clock"))    return g_show_clock;
    if (!strcmp(b, "show_calendar")) return g_show_calendar;
    if (!strcmp(b, "show_weather"))  return g_show_weather;
    if (!strcmp(b, "show_crypto"))   return g_show_crypto;
    if (!strcmp(b, "show_stocks"))   return g_show_stocks;
    if (!strcmp(b, "show_sysmon"))   return g_show_sysmon;
    if (!strcmp(b, "show_timer"))    return g_show_timer;
    if (!strcmp(b, "show_worldtime"))return g_show_worldtime;
    if (!strcmp(b, "show_uptime"))   return g_show_uptime;   // #341: was missing -> Uptime never toggled
    if (!strcmp(b, "show_ha"))       return g_show_ha;       // #414
    if (!strcmp(b, "show_stickies")) return g_show_stickies;
    if (!strcmp(b, "show_aichat"))   return g_aichat_enabled;
    if (!strcmp(b, "volume"))        return get_volume();
    if (!strcmp(b, "win_opacity"))   return g_win_opacity * 100 / 255;
    if (!strcmp(b, "bt_power"))      return bt_is_powered();      // #372
    if (!strcmp(b, "wifi_power"))    return wifi_is_powered();    // #384
    // (#231r) eq0..eq4 read the KERNEL's live band positions. There is no
    // local copy to go stale, which is the structural half of #231's fix.
    // Exactly "eq" + ONE digit. A loose strncmp would make "eq10" silently
    // mean band 1, which is the shape of a bug that only shows up the day
    // somebody adds a tenth of anything (profile.c's matching test is strict
    // for the same reason).
    if (b[0] == 'e' && b[1] == 'q' && b[2] >= '0' && b[2] <= '9' && b[3] == 0) {
        int i = b[2] - '0';
        if (i < SND_EQ_BANDS) return eq_band_get(i);
    }
    return 0;
}
static void tm_set(const char *b, int v) {
    if      (!strcmp(b, "sheep_show"))    g_sheep_enabled = v;
    else if (!strcmp(b, "sheep_speed"))   g_sheep_speed = v;
    else if (!strcmp(b, "sheep_size"))    g_sheep_size = v;
    else if (!strcmp(b, "sheep_style"))   g_sheep_style = v;
    else if (!strcmp(b, "sheep_count"))   g_sheep_count = v;
    else if (!strcmp(b, "dog_show"))      g_dog_enabled = v;
    else if (!strcmp(b, "show_digclock")) g_show_digclock = v;
    else if (!strcmp(b, "show_clock"))    g_show_clock = v;
    else if (!strcmp(b, "show_calendar")) g_show_calendar = v;
    else if (!strcmp(b, "show_weather"))  g_show_weather = v;
    else if (!strcmp(b, "show_crypto"))   g_show_crypto = v;
    else if (!strcmp(b, "show_stocks"))   g_show_stocks = v;
    else if (!strcmp(b, "show_sysmon"))   g_show_sysmon = v;
    else if (!strcmp(b, "show_timer"))    g_show_timer = v;
    else if (!strcmp(b, "show_worldtime"))g_show_worldtime = v;
    else if (!strcmp(b, "show_uptime"))   g_show_uptime = v;   // #341: was missing -> Uptime never toggled
    else if (!strcmp(b, "show_ha"))       g_show_ha = v;       // #414
    else if (!strcmp(b, "show_stickies")) g_show_stickies = v;
    else if (!strcmp(b, "show_aichat"))   aichat_set_enabled(v);
    else if (!strcmp(b, "volume"))        set_volume(v);
    else if (!strcmp(b, "win_opacity")) { int o = v*255/100; if(o<40)o=40; if(o>255)o=255; g_win_opacity=o; set_win_opacity(o); }
    else if (!strcmp(b, "bt_power"))      bt_power(v);            // #372
    else if (!strcmp(b, "wifi_power"))    wifi_power(v);          // #384
    // (#231r) and eq0..eq4 write straight through to the DSP.
    else if (b[0] == 'e' && b[1] == 'q' && b[2] >= '0' && b[2] <= '9' && b[3] == 0) {
        int i = b[2] - '0';
        if (i < SND_EQ_BANDS) eq_band_set(i, v);
    }
}

// #745 P2: the widget live-apply channel (main.c widgets_cfg_poll()) needs to
// reach tm_set() from outside this file, and MUST go through it rather than
// writing a widget_desc_t.flag pointer directly, because tm_set() is the only
// place that knows show_aichat needs aichat_set_enabled() (spawn/stop the
// external app) instead of a bare assignment. tm_set() itself stays static;
// this is a thin, deliberate crack in that encapsulation for the one caller
// that needs it, not a general-purpose export.
void traymenu_set_bind(const char *b, int v) { tm_set(b, v); }

// #372: run a TM_ACTION item. Actions are dispatched by their bind string.
void traymenu_close(void);   // defined below
static void tm_action(const char *bind) {
    int tab = -1;
    if      (!strcmp(bind, "bt_settings"))   tab = SETTINGS_PANEL_BLUETOOTH;
    else if (!strcmp(bind, "net_settings"))  tab = SETTINGS_PANEL_NETWORK;
    else if (!strcmp(bind, "wifi_settings")) tab = SETTINGS_PANEL_WIFI;
    if (tab >= 0) {
        settings_open_panel(tab);   // #129: singleton-safe, see compositor.h
        traymenu_close();
    }
}

// ---- tiny helpers --------------------------------------------------------
static void tm_trim(char *s) {
    int n = 0; while (s[n]) n++;
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\r' || s[n-1] == '\t')) s[--n] = '\0';
    int i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) { int j = 0; while (s[i]) s[j++] = s[i++]; s[j] = '\0'; }
}
static int tm_atoi(const char *s) { int v = 0, neg = 0; if (*s=='-'){neg=1;s++;} while (*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return neg?-v:v; }
static void tm_cpy(char *d, const char *s, int max) { int i=0; while (i<max-1 && s[i]) { d[i]=s[i]; i++; } d[i]='\0'; }

// Parse one inline-map item line: "- {type: check, label: X, bind: y, min: 1, max: 5, options: A|B}"
static void tm_parse_item(tm_menu *m, const char *line) {
    const char *o = line; while (*o && *o != '{') o++;
    if (!*o) return; o++;
    char body[256]; int bi = 0;
    while (*o && *o != '}' && bi < 255) body[bi++] = *o++;
    body[bi] = '\0';
    if (m->n >= 14) return;
    tm_item *it = &m->items[m->n];
    it->type = TM_ACTION; it->label[0]=0; it->bind[0]=0; it->vmin=0; it->vmax=100; it->nopt=0;
    // split body by ','
    char tok[128]; int ti = 0;
    for (int i = 0; ; i++) {
        char c = body[i];
        if (c == ',' || c == '\0') {
            tok[ti] = '\0';
            // split tok by ':'
            char *col = tok; while (*col && *col != ':') col++;
            if (*col == ':') {
                *col = '\0'; char *key = tok; char *val = col + 1;
                tm_trim(key); tm_trim(val);
                if      (!strcmp(key,"type")) {
                    if      (!strcmp(val,"check"))  it->type = TM_CHECK;
                    else if (!strcmp(val,"slider")) it->type = TM_SLIDER;
                    else if (!strcmp(val,"radio"))  it->type = TM_RADIO;
                    else                            it->type = TM_ACTION;
                } else if (!strcmp(key,"label")) tm_cpy(it->label, val, 28);
                else if (!strcmp(key,"bind"))  tm_cpy(it->bind, val, 20);
                else if (!strcmp(key,"min"))   it->vmin = tm_atoi(val);
                else if (!strcmp(key,"max"))   it->vmax = tm_atoi(val);
                else if (!strcmp(key,"options")) {
                    // split val by '|'
                    char part[16]; int pi = 0;
                    for (int k = 0; ; k++) {
                        char vc = val[k];
                        if (vc == '|' || vc == '\0') {
                            part[pi] = '\0'; tm_trim(part);
                            if (part[0] && it->nopt < 3) { tm_cpy(it->opt[it->nopt++], part, 14); }
                            pi = 0; if (vc == '\0') break;
                        } else if (pi < 15) part[pi++] = vc;
                    }
                }
            }
            ti = 0; if (c == '\0') break;
        } else if (ti < 127) tok[ti++] = c;
    }
    m->n++;
}

static void tm_load_defaults(void) {
    static const char *def =
        "sheep:\n"
        "  - {type: check, label: Show sheep, bind: sheep_show}\n"
        "  - {type: slider, label: Speed, bind: sheep_speed, min: 1, max: 5}\n"
        "  - {type: slider, label: Size, bind: sheep_size, min: 1, max: 3}\n"
        "  - {type: radio, label: Style, bind: sheep_style, options: Classic|Spotted}\n"
        "  - {type: slider, label: Count, bind: sheep_count, min: 1, max: 50}\n"
        "  - {type: check, label: Sheepdog, bind: dog_show}\n"
        // (#231r) All FIVE bands are bound now, one per fader, matching the
        // faceplate. #231 removed the old three (Bass/Mid/Treble on
        // eq0/eq2/eq4, with eq1/eq3 bound to nothing) along with the
        // decorative fader bank; the three-vs-five mismatch was one of its
        // stated reasons for not believing the control. This in-code
        // fallback is used only when neither TRAYMENU.YML nor .YAML is on
        // disk, and is kept byte-consistent with both.
        //
        // The sound panel's RENDERER still does not read this list (it is a
        // hand-drawn hardware face, see snd_render()), but the bindings are
        // live: traymenu_set_bind() dispatches "eqN" through tm_set() to
        // eq_band_set(), so anything that drives a binding by name reaches
        // the real DSP.
        "sound:\n"
        "  - {type: slider, label: Volume, bind: volume, min: 0, max: 100}\n"
        "  - {type: slider, label: 60 Hz, bind: eq0, min: 0, max: 100}\n"
        "  - {type: slider, label: 250 Hz, bind: eq1, min: 0, max: 100}\n"
        "  - {type: slider, label: 1 kHz, bind: eq2, min: 0, max: 100}\n"
        "  - {type: slider, label: 4 kHz, bind: eq3, min: 0, max: 100}\n"
        "  - {type: slider, label: 12 kHz, bind: eq4, min: 0, max: 100}\n"
        "widgets:\n"
        "  - {type: check, label: Clock, bind: show_clock}\n"
        "  - {type: check, label: Calendar, bind: show_calendar}\n"
        "  - {type: check, label: Weather, bind: show_weather}\n"
        "  - {type: check, label: Crypto, bind: show_crypto}\n"
        "  - {type: check, label: Stocks, bind: show_stocks}\n"
        "  - {type: check, label: Home Assistant, bind: show_ha}\n"
        "  - {type: check, label: Sheep, bind: sheep_show}\n"
        "  - {type: slider, label: Window Opacity, bind: win_opacity, min: 40, max: 100}\n"
        "bluetooth:\n"
        "  - {type: check, label: Bluetooth, bind: bt_power}\n"
        "  - {type: action, label: Bluetooth settings, bind: bt_settings}\n"
        "network:\n"
        "  - {type: check, label: Wi-Fi, bind: wifi_power}\n"
        "  - {type: action, label: Network settings, bind: net_settings}\n"
        "  - {type: action, label: Wi-Fi settings, bind: wifi_settings}\n";
    extern void traymenu_parse(const char *text);
    traymenu_parse(def);
}

void traymenu_parse(const char *text) {
    g_tm_count = 0;
    char line[256]; int li = 0;
    for (int i = 0; ; i++) {
        char c = text[i];
        if (c == '\n' || c == '\0') {
            line[li] = '\0';
            // classify
            char t[256]; tm_cpy(t, line, 256); tm_trim(t);
            if (t[0]) {
                if (t[0] == '-') {
                    if (g_tm_count > 0) tm_parse_item(&g_tm[g_tm_count-1], line);
                } else {
                    // section header "name:"
                    int n = 0; while (t[n] && t[n] != ':') n++;
                    if (t[n] == ':' && g_tm_count < TM_MAX) {
                        t[n] = '\0';
                        tm_cpy(g_tm[g_tm_count].name, t, 16);
                        g_tm[g_tm_count].n = 0;
                        g_tm_count++;
                    }
                }
            }
            li = 0; if (c == '\0') break;
        } else if (li < 255) line[li++] = c;
    }
}

// Build the "widgets" tray menu DYNAMICALLY from the compositor widget registry
// (widgets.c::widget_registry) rather than a hardcoded list, so the menu always
// reflects the actual available widgets and adding a widget needs no edit here.
static void tm_force_widgets(void) {
    tm_menu *m = 0;
    for (int i = 0; i < g_tm_count; i++)
        if (!strcmp(g_tm[i].name, "widgets")) { m = &g_tm[i]; break; }
    if (!m) {
        if (g_tm_count >= TM_MAX) return;
        m = &g_tm[g_tm_count++]; tm_cpy(m->name, "widgets", 16);
    }
    int wc = 0;
    const widget_desc_t *reg = widget_registry(&wc);
    m->n = 0;
    // #745 P2: was "< 13" against a 14-slot array, which silently dropped the
    // LAST registry entry (AI Chat) once a 14th widget (Sheep) existed. items[]
    // is now sized 16 (see tm_menu above); this cap must stay ONE LESS than
    // that so the trailing opacity slider below always has a slot.
    for (int i = 0; i < wc && m->n < 15; i++) {   // leave room for the opacity slider
        tm_item *it = &m->items[m->n++];
        it->type = TM_CHECK; tm_cpy(it->label, reg[i].label, 28); tm_cpy(it->bind, reg[i].bind, 20);
        it->vmin = 0; it->vmax = 100; it->nopt = 0;
    }
    tm_item *o = &m->items[m->n++];
    o->type = TM_SLIDER; tm_cpy(o->label, "Window Opacity", 28); tm_cpy(o->bind, "win_opacity", 20);
    o->vmin = 40; o->vmax = 100; o->nopt = 0;
}

void traymenu_init(void) {
    int fd = sys_open("/APPS/TRAYMENU.YML", 0);
    if (fd < 0) fd = sys_open("/TRAYMENU.YML", 0);
    if (fd >= 0) {
        static char buf[4096];
        long n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) { buf[n] = '\0'; traymenu_parse(buf); }
    }
    if (g_tm_count == 0) tm_load_defaults();
    tm_force_widgets();
}

// ---- geometry ------------------------------------------------------------
// #uiscale: scaled at the definition (see compositor.h's block comment).
#define TM_W      ui_px(214)
#define TM_PAD    ui_px(8)
#define TM_TITLE  ui_px(22)
// #uiscale BUGFIX (report: "the widgets system tray items also didn't
// scale"): TM_W and tm_item_h() (the ROW) were already scaled, but every
// CONTROL drawn inside a row - the checkbox box, its checkmark strokes, the
// slider track/handle, the radio pill and the action row - was still a bare
// 1x literal. At 200% each row doubled in height around a checkbox/slider
// that stayed exactly the same size, so it sat near the top of a mostly
// empty row instead of growing with it: the same "box scaled, control did
// not" shape as the AI-launcher taskbar button and the titlebar glyphs
// above. Shared here so the draw loop (traymenu_render) and the hit-test
// loop (traymenu_handle_mouse) read the SAME values and cannot drift, the
// same discipline default_bar_buttons_rect()/xfce_panel_buttons_rect()
// already use in taskbar.c. Every value is byte-identical to the literal it
// replaces at 100%.
#define TM_CHECK_BOX    ui_px(14)   // checkbox square
#define TM_CHECK_TEXT_X ui_px(22)   // label x-offset past TM_PAD
#define TM_SLIDER_TRACK_Y ui_px(24) // track y-offset past the row top
#define TM_SLIDER_TRACK_H ui_px(6)  // track thickness
#define TM_SLIDER_HANDLE_R ui_px(5) // drag handle radius
#define TM_RADIO_OPT_X  ui_px(64)   // first option x-offset past TM_PAD
#define TM_RADIO_OPT_W  ui_px(64)   // per-option pitch (box is OPT_W - 4 wide)
#define TM_RADIO_OPT_H  ui_px(18)
#define TM_ACTION_H     ui_px(20)   // clickable action row height
// #uiscale BUGFIX sweep: this returned a raw (unscaled) row height while
// TM_W (the menu's own width, just above) was already scaled - a half-scaled
// box, exactly the shape this bug report is about, just found by inspection
// rather than a screendump. Scaled row height now matches the scaled text
// drawn inside it (chokepoint) at every call site (tm_box(), the draw loop,
// and the drag-hit-test loop below all call this one function).
static int tm_item_h(const tm_item *it) {
    if (it->type == TM_SLIDER) return ui_px(40);
    return ui_px(26);
}
// (#231r) BACK TO 308x214, the original size, because the 5-band fader bank
// it was budgeted for is back (#231 had shrunk it to 180x190 when the bank
// was removed).
// #uiscale: this USED TO be a pre-existing duplicate-constant pair
// (SND_W_FWD/SND_H_FWD here, kept "in exact sync" with a second SND_W/SND_H
// definition further down by comment discipline alone, because tm_box()
// needs the sound panel's size before the point in the file where the sound-
// panel section previously defined it). A #define has no such ordering
// requirement - only the point of USE matters - so SND_W/SND_H are simply
// defined HERE, once, and the sound-panel section below now uses these same
// two macros instead of redefining them. That removes the sync-by-comment
// duplication and lets a single ui_px() cover both call sites.
#define SND_W ui_px(308)
#define SND_H ui_px(214)
static void tm_box(const tm_menu *m, int *bx, int *by, int *bw, int *bh) {
    int h = TM_TITLE + TM_PAD * 2;
    for (int i = 0; i < m->n; i++) h += tm_item_h(&m->items[i]);
    int w = TM_W;
    // #336: the sound "hardware face" panel is a fixed-size custom draw.
    if (!strcmp(m->name, "sound")) { w = SND_W; h = SND_H; }
    int x = g_tray_menu_ax - w / 2;
    if (x < 4) x = 4;
    if (x > g_fb_width - w - 4) x = g_fb_width - w - 4;
    // #387: anchor to the ACTUAL tray row. Top-bar layouts (Lumina/Retro Bench) drop the
    // menu downward; bottom layouts open it above the tray. DEFAULT keeps its
    // exact legacy position.
    extern int g_tray_bar_top, g_dock_style;
    extern int32_t g_tray_bar_y, g_tray_bar_h;
    int y;
    if (g_tray_bar_top) {
        y = g_tray_bar_y + g_tray_bar_h + 6;
    } else if (g_dock_style == DOCK_DEFAULT) {
        y = (g_fb_height - TASKBAR_HEIGHT) - h - 6;
    } else {
        y = g_tray_bar_y - h - 6;
    }
    // (local 81) ONE clamp covering all three anchor branches. Only the
    // top-bar branch used to clamp its bottom, and the "sound" panel's height
    // is the hardcoded SND_H_FWD, so an upward-opening menu taller than the
    // space above its bar was floored to y=4 and then simply ran off the
    // bottom. The shared helper also keeps it off the dock, which this menu
    // is drawn above.
    popup_clamp_to_work_area(w, h, &x, &y);
    *bx = x; *by = y; *bw = w; *bh = h;
}

static tm_menu *tm_cur(void) {
    if (!g_tray_menu_open || g_tray_menu_which < 0 || g_tray_menu_which >= g_tm_count) return 0;
    return &g_tm[g_tray_menu_which];
}

// Map a tray icon (0=widgets,1=sound,2=network,3=bluetooth,4=sheep) to a menu.
void traymenu_open_for_icon(int icon, int anchor_x) {
    const char *want = (icon == 0) ? "widgets" : (icon == 1) ? "sound"
                     : (icon == 2) ? "network" : (icon == 3) ? "bluetooth" : "sheep";
    for (int i = 0; i < g_tm_count; i++) {
        if (!strcmp(g_tm[i].name, want)) {
            g_tray_menu_which = i; g_tray_menu_ax = anchor_x; g_tray_menu_open = 1; g_tm_drag = -1;
            return;
        }
    }
}
void traymenu_close(void) { g_tray_menu_open = 0; g_tray_menu_which = -1; g_tm_drag = -1; }

// ==========================================================================
// #336 - Premium analog-hardware EQ panel for the "sound" tray popup.
// Aesthetic reference: Rane/Red Rock EQ-560 - dark brushed-metal face, red
// accent trim, recessed fader slots with metal caps, a rotary MASTER volume
// knob, a MUTE switch, and antialiased (TTF) frequency-band labels. Replaces
// the flat Motif slider list for the sound section only; widgets/sheep keep
// the generic renderer.
//
// (#231r) The fader bank and the "EQ-560" / "GRAPHIC EQ" branding are
// restored EXACTLY as #336 drew them - same column pitch, same recessed
// slots, same five tick marks with the centre detent picked out, same metal
// cap with its red index line, same TTF band labels. What is different is
// underneath: the panel is now allowed to call itself a graphic EQ because
// it is one. See the header note at the top of this file.
// ==========================================================================
extern int g_tray_muted;                 // taskbar.c (global): live mute state

// #uiscale: SND_W/SND_H are now defined once, above tm_box() - see the
// comment there. Nothing left to redefine here.
//
// (#231r) FADER-BANK GEOMETRY: ONE SOURCE, READ BY BOTH SIDES.
//
// Every number below is the ORIGINAL #336 literal wrapped in ui_px(), so the
// panel is byte-identical at 100% and correct at 200% (the owner's 3840x2160
// panel), which is precisely where a dense five-fader cluster with unscaled
// internal literals shows up badly. snd_render() and snd_mouse() both derive
// their positions from snd_fx()/snd_ty0()/snd_cap_y(), so the drawn cap and
// the grabbable box cannot drift apart - the same discipline
// TM_SLIDER_TRACK_Y/TM_SLIDER_TRACK_H already impose on the generic
// renderer, and the same discipline default_bar_buttons_rect() imposes in
// taskbar.c.
#define SND_FAD_X0      ui_px(28)    // first band column centre, past bx
#define SND_FAD_DX      ui_px(36)    // column pitch
#define SND_FAD_TY0     ui_px(48)    // top of the fader travel, past by
#define SND_FH          ui_px(102)   // fader travel height
#define SND_FAD_GRAB_X  ui_px(14)    // half-width of the grab box (cap is 26 wide)
#define SND_FAD_GRAB_Y  ui_px(10)    // slop above and below the travel
#define SND_DIV_X       ui_px(210)   // divider between the bank and MASTER
#define SND_KNOB_CX     ui_px(254)   // MASTER knob centre, past bx
#define SND_MUTE_X      ui_px(228)   // MUTE switch, past bx

static int snd_fx(int bx, int i)  { return bx + SND_FAD_X0 + i * SND_FAD_DX; }
static int snd_ty0(int by)        { return by + SND_FAD_TY0; }
/// Top of the metal cap's travel for a fader at `val` (0..100).
static int snd_cap_y(int by, int val) {
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    return snd_ty0(by) + (100 - val) * SND_FH / 100;
}
/// The exact inverse, used by the drag, so a grabbed cap tracks the pointer
/// instead of jumping by a rounding error.
static int snd_val_from_y(int by, int my) {
    int v = (snd_ty0(by) + SND_FH - my) * 100 / SND_FH;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}
/// How many bands to draw. The kernel is the authority on how many there
/// ARE; the faceplate is the authority on how many it has room for.
static int snd_bands(void) {
    int n = eq_band_count();
    if (n < 0) n = 0;
    if (n > SND_EQ_BANDS) n = SND_EQ_BANDS;
    return n;
}
/// The band label, DERIVED from the kernel's own centre frequency rather
/// than from a private table. 60 -> "60", 250 -> "250", 1000 -> "1k",
/// 4000 -> "4k", 12000 -> "12k", which reproduces #336's original faceplate
/// labels exactly - but now it is impossible for the panel to print a
/// frequency the filter is not actually using. That divergence (the YAML
/// saying Bass/Mid/Treble while the face said 60/250/1k/4k/12k) is one of
/// the things #231 correctly held against the old control.
static void snd_band_label(int i, char *out) {
    int hz = eq_band_freq(i);
    int n = 0;
    if (hz < 0) hz = 0;
    if (hz >= 1000 && (hz % 1000) == 0) {
        int k = hz / 1000;
        if (k >= 100) out[n++] = (char)('0' + (k / 100) % 10);
        if (k >= 10)  out[n++] = (char)('0' + (k / 10) % 10);
        out[n++] = (char)('0' + k % 10);
        out[n++] = 'k';
    } else {
        char t[8]; int d = 0, v = hz;
        if (!v) t[d++] = '0';
        while (v > 0 && d < 8) { t[d++] = (char)('0' + v % 10); v /= 10; }
        while (d > 0) out[n++] = t[--d];
    }
    out[n] = '\0';
}

// Rotary-knob pointer directions (0=min lower-left .. 18=max lower-right),
// unit vectors x100 sweeping 270 degrees through straight-up. Screen coords.
static const signed char KNOB_DX[19]={-71,-87,-97,-100,-97,-87,-71,-50,-26,0,26,50,71,87,97,100,97,87,71};
static const signed char KNOB_DY[19]={71,50,26,0,-26,-50,-71,-87,-97,-100,-97,-87,-71,-50,-26,0,26,50,71};

// #uiscale: SND_W is already scaled (ui_px(180) above); these offsets/sizes
// scale the same way, independently (a knob center and a switch box, not an
// edge-sharing pair).
// (#231r) Both are back on the RIGHT of the divider, where #336 put them,
// because the left two thirds of the face is the fader bank again. #231 had
// centred them in the shrunken 180-wide panel.
static void snd_master(int bx, int by, int *kcx, int *kcy, int *kr) {
    *kcx = bx + SND_KNOB_CX; *kcy = by + ui_px(82); *kr = ui_px(27);
}
static void snd_mute(int bx, int by, int *mx, int *my, int *mw, int *mh) {
    *mw = ui_px(52); *mh = ui_px(24);
    *mx = bx + SND_MUTE_X; *my = by + ui_px(150);
}

// Draw a short radial "needle" line inside the knob using the direction table.
static void snd_needle(int cx, int cy, int r0, int r1, int k, uint32_t col) {
    if (k < 0) k = 0; if (k > 18) k = 18;
    for (int r = r0; r <= r1; r++) {
        int px = cx + KNOB_DX[k] * r / 100;
        int py = cy + KNOB_DY[k] * r / 100;
        draw_fill_rect(px, py, ui_px(2), ui_px(2), col);
    }
}

static void snd_render(int bx, int by) {
    // --- Brushed-metal face with a beveled, red-trimmed frame ---
    draw_gradient_v(bx, by, SND_W, SND_H, 0x00393C42, 0x0022242A);
    draw_rect_outline(bx, by, SND_W, SND_H, 0x00121317);
    // #uiscale (#231r pass 2): these were raw 1x literals, and a 200%
    // screendump is what caught it - the red trim sat at by+30 while the
    // header text, whose SIZE draw_text_ttf() scales for us, had grown to
    // 24px and reached y=38, so the accent line was drawn THROUGH
    // "MAYTERA / EQ-560 / GRAPHIC EQ". A hairline stays a hairline (1px
    // bevel, 1px shade) because ui_px() would fatten it into a border; every
    // POSITION scales.
    draw_rect_outline(bx + 1, by + 1, SND_W - 2, SND_H - 2, 0x005A5E68);  // bevel highlight
    draw_hline(bx + 2, by + SND_H - ui_px(3), SND_W - 4, 0x00141519);     // bottom shade
    // red accent trim under the header
    draw_hline(bx + ui_px(8), by + ui_px(30), SND_W - ui_px(16), 0x00C0392B);
    draw_hline(bx + ui_px(8), by + ui_px(31), SND_W - ui_px(16), 0x00521812);

    // --- Header: red power LED + model name (TTF) ---
    // (#231r) The "EQ-560" / "GRAPHIC EQ" branding is restored because the
    // panel is a graphic EQ again. #uiscale: these coordinates were raw 1x
    // literals even after #231 shrank the panel, so they were wrong at 200%
    // before this change; the ui_px() is a fix, not a restore. The TTF SIZE
    // argument is deliberately NOT wrapped - draw_text_ttf()/text_width_ttf()
    // apply ui_px() to it themselves at call time (see main.c's note).
    draw_circle_filled(bx + ui_px(16), by + ui_px(15), ui_px(4), 0x00E8402C);
    draw_circle_filled(bx + ui_px(15), by + ui_px(14), ui_px(1), 0x00FFD0C0);   // LED glint
    // The three header strings are placed by MEASURED width rather than by
    // the three hardcoded x offsets #336 used (28 / 100 / SND_W-92). At 100%
    // those literals happened to clear each other; at 200% every string is
    // twice as wide while the gaps between the literals were not, so
    // "MAYTERA" ran into "EQ-560". text_width_ttf() already reports the
    // scaled width, so laying them out from it is correct at every scale and
    // byte-identical at 100% only where the measured width matches - which is
    // why the model name is measured off MAYTERA's real width instead of
    // being pinned to 100.
    int hx = bx + ui_px(28);
    draw_text_ttf(hx, by + ui_px(7), "MAYTERA", 12, 0x00E8E4D8);
    hx += text_width_ttf("MAYTERA", 12) + ui_px(10);
    draw_text_ttf(hx, by + ui_px(7), "EQ-560", 12, 0x00E8402C);
    {
        const char *ge = "GRAPHIC EQ";
        int gw = text_width_ttf(ge, 10);
        int gx = bx + SND_W - ui_px(10) - gw;
        // Only draw the right-hand legend if it clears the model name; on a
        // narrow panel it is the least important of the three.
        if (gx > hx + text_width_ttf("EQ-560", 12) + ui_px(8))
            draw_text_ttf(gx, by + ui_px(9), ge, 10, 0x008A8E96);
    }

    // --- 5 band faders -----------------------------------------------------
    // Restored verbatim from #336 (recessed slot, five tick marks with the
    // centre detent picked out, accent fill below the cap, brushed metal cap
    // with a red index line, TTF band label), with every literal ui_px()'d
    // and the value now read from the kernel's live DSP state rather than
    // from a local array nothing else could see.
    {
        int ty0 = snd_ty0(by);
        int nb  = snd_bands();
        for (int i = 0; i < nb; i++) {
            int fx  = snd_fx(bx, i);
            int val = eq_band_get(i);
            if (val < 0) val = AEQ_POS_FLAT;
            // recessed slot
            draw_fill_rect(fx - ui_px(4), ty0 - ui_px(3), ui_px(8), SND_FH + ui_px(6), 0x00101115);
            draw_rect_outline(fx - ui_px(4), ty0 - ui_px(3), ui_px(8), SND_FH + ui_px(6), 0x00050608);
            draw_vline(fx - ui_px(3), ty0 - ui_px(2), SND_FH + ui_px(4), 0x001C1E24);
            // tick marks (0/25/50/75/100). The middle one is the CENTRE
            // DETENT and it now means something exact: 50 is 0.0 dB, the
            // position at which the band is bypassed rather than merely
            // computed to unity.
            for (int t = 0; t <= 4; t++) {
                int tyy = ty0 + t * SND_FH / 4;
                draw_hline(fx - ui_px(12), tyy, ui_px(6), (t == 2) ? 0x00706048 : 0x0044484F);
                draw_hline(fx + ui_px(6),  tyy, ui_px(6), (t == 2) ? 0x00706048 : 0x0044484F);
            }
            // level fill (accent) below the cap
            int capY = snd_cap_y(by, val);
            draw_gradient_v(fx - ui_px(2), capY, ui_px(4), ty0 + SND_FH - capY, 0x00C0392B, 0x00521812);
            // metal fader cap with red index line
            int cy = capY - ui_px(6);
            draw_gradient_v(fx - ui_px(13), cy, ui_px(26), ui_px(13), 0x00D8DCE2, 0x008A8E96);
            draw_rect_outline(fx - ui_px(13), cy, ui_px(26), ui_px(13), 0x00202227);
            draw_hline(fx - ui_px(12), cy + ui_px(1), ui_px(24), 0x00F2F4F8);   // top highlight
            draw_hline(fx - ui_px(11), cy + ui_px(6), ui_px(22), 0x00E8402C);   // red index
            // band label (TTF, centered), derived from the kernel's own
            // centre frequency - see snd_band_label().
            char lbl[8]; snd_band_label(i, lbl);
            int lw = text_width_ttf(lbl, 10);
            draw_text_ttf(fx - lw / 2, ty0 + SND_FH + ui_px(8), lbl, 10, 0x00C8CCD4);
        }
    }

    // --- Divider between the band bank and the master section ---
    draw_vline(bx + SND_DIV_X,           by + ui_px(40), SND_H - ui_px(52), 0x00141519);
    draw_vline(bx + SND_DIV_X + ui_px(1), by + ui_px(40), SND_H - ui_px(52), 0x004A4E56);

    // --- MASTER rotary knob ---
    int kcx, kcy, kr; snd_master(bx, by, &kcx, &kcy, &kr);
    int vol = get_volume(); if (vol < 0) vol = 0; if (vol > 100) vol = 100;
    int kk = vol * 18 / 100;
    draw_text_ttf(kcx - text_width_ttf("MASTER", 10) / 2, by + ui_px(44), "MASTER", 10, 0x00C8CCD4);
    draw_circle_filled(kcx, kcy, kr + ui_px(2), 0x00101115); // socket
    draw_circle_filled(kcx, kcy, kr, 0x004A4E56);            // knob rim (bright)
    draw_circle_filled(kcx, kcy, kr - ui_px(1), 0x00161A20); // rim shadow ring
    // #341: proper round rotary dial built from concentric circles ONLY (the old
    // square gradient dome poked its corners past the rim = "square in a circle").
    draw_circle_filled(kcx, kcy, kr - ui_px(3), 0x00363940);           // dial face
    draw_circle_filled(kcx, kcy - ui_px(2), kr - ui_px(6), 0x00474B54); // upper dome sheen
    draw_circle_filled(kcx, kcy + ui_px(1), kr - ui_px(10), 0x00303339); // recessed center
    draw_circle_outline(kcx, kcy, kr - ui_px(3), 0x00686C76);          // crisp bright rim
    // Angled pointer indicator from center out to the current level.
    snd_needle(kcx, kcy, ui_px(5), kr - ui_px(6), kk, 0x00E8402C);     // red pointer
    draw_circle_filled(kcx, kcy, ui_px(3), 0x00D8DCE2);                // hub cap
    // numeric readout
    char vb[8]; int v = vol, d = 0, tmp[8];
    if (!v) { vb[0] = '0'; vb[1] = 0; } else { while (v) { tmp[d++] = v % 10; v /= 10; } for (int q = 0; q < d; q++) vb[q] = '0' + tmp[d - 1 - q]; vb[d] = 0; }
    draw_text_ttf(kcx - text_width_ttf(vb, 12) / 2, kcy + kr + ui_px(4), vb, 12, 0x00E8E4D8);

    // --- MUTE switch ---
    int mx, my, mw, mh; snd_mute(bx, by, &mx, &my, &mw, &mh);
    uint32_t mtop = g_tray_muted ? 0x00E8402C : 0x00363940;
    uint32_t mbot = g_tray_muted ? 0x00901F14 : 0x00202227;
    draw_gradient_v(mx, my, mw, mh, mtop, mbot);
    draw_rect_outline(mx, my, mw, mh, 0x00101115);
    draw_rect_outline(mx + 1, my + 1, mw - 2, mh - 2, g_tray_muted ? 0x00F08070 : 0x00565A63);
    const char *ml = "MUTE";
    draw_text_ttf(mx + (mw - text_width_ttf(ml, 11)) / 2, my + ui_px(6), ml,
                  11, g_tray_muted ? 0x00FFFFFF : 0x0090949C);
}

// Hit-test / drag for the analog sound panel. Returns true if consumed.
static bool snd_mouse(int bx, int by, int mx, int my, bool pressed, bool held) {
    // (#231r) Band faders: a press starts a drag and g_tm_drag holds the band
    // index 0..4 (90 is the master knob's sentinel, kept from #336). The grab
    // box is derived from the SAME snd_fx()/snd_ty0()/SND_FH the draw side
    // uses, so it is exactly where the cap is at any UI scale.
    {
        int ty0 = snd_ty0(by);
        int nb  = snd_bands();
        for (int i = 0; i < nb; i++) {
            int fx = snd_fx(bx, i);
            if (pressed && mx >= fx - SND_FAD_GRAB_X && mx <= fx + SND_FAD_GRAB_X &&
                my >= ty0 - SND_FAD_GRAB_Y && my <= ty0 + SND_FH + SND_FAD_GRAB_Y) {
                g_tm_drag = i;
            }
        }
    }
    // Master knob: press/drag around it maps direction -> volume.
    int kcx, kcy, kr; snd_master(bx, by, &kcx, &kcy, &kr);
    int ddx = mx - kcx, ddy = my - kcy;
    // #uiscale: the grab margin was a raw 8, so at 200% it shrank from a
    // quarter of the knob's radius to an eighth. Same drift class as the
    // fader box below, just on the pre-existing control.
    int kgrab = kr + ui_px(8);
    if (pressed && (ddx * ddx + ddy * ddy) <= kgrab * kgrab) {
        g_tm_drag = 90;   // sentinel: master knob
    }
    // Mute switch toggle.
    int mmx, mmy, mmw, mmh; snd_mute(bx, by, &mmx, &mmy, &mmw, &mmh);
    if (pressed && mx >= mmx && mx < mmx + mmw && my >= mmy && my < mmy + mmh) {
        g_tray_muted = !g_tray_muted;
        set_mute(g_tray_muted);
        g_tm_drag = -1;
        return true;
    }

    if (g_tm_drag == 90 && (held || pressed)) {
        // nearest pointer direction -> volume fraction
        int best = 9, bestdot = -1000000;
        for (int k = 0; k < 19; k++) {
            int dot = ddx * KNOB_DX[k] + ddy * KNOB_DY[k];
            if (dot > bestdot) { bestdot = dot; best = k; }
        }
        int vol = best * 100 / 18;
        if (vol < 0) vol = 0; if (vol > 100) vol = 100;
        set_volume(vol);
    } else if (g_tm_drag >= 0 && g_tm_drag < SND_EQ_BANDS && (held || pressed)) {
        // (#231r) This is the line #231 was written about. It used to be
        // `g_eq[g_tm_drag] = v;`, a write to a static nothing else read.
        // It is now a syscall into the kernel's live filter bank.
        eq_band_set(g_tm_drag, snd_val_from_y(by, my));
    }
    if (!held) {
        // (#231r) Log the settled EQ once, on RELEASE. Not during the drag:
        // the kernel side rewrites the whole of /AUDIOLOG.TXT per call, and a
        // 50-event drag would rewrite it 50 times. The UI is the only place
        // that knows the button came up, so the decision belongs here rather
        // than in a throttle guessed at in the kernel.
        if (g_tm_drag >= 0 && g_tm_drag < SND_EQ_BANDS) eq_log();
        g_tm_drag = -1;
    }
    return true;
}

// ---- render --------------------------------------------------------------
void traymenu_render(void) {
    tm_menu *m = tm_cur(); if (!m) return;
    // #336: the "sound" section uses the premium analog EQ panel.
    if (!strcmp(m->name, "sound")) {
        int bx, by, bw, bh; tm_box(m, &bx, &by, &bw, &bh);
        snd_render(bx, by);
        return;
    }
    int bx, by, bw, bh; tm_box(m, &bx, &by, &bw, &bh);
    // #128: this used to be a flat rect filled with hardcoded 0x00-prefixed
    // literals (0x00262A33 bg, 0x0090A0B0 border, gold 0x00FFD040 title) that
    // never tracked the active theme at all - the single most literal case
    // of "each popup has its own bespoke treatment" this ticket names. Same
    // shared panel + theme tokens as the notifications center and the AI
    // launcher now.
    draw_popup_panel(bx, by, bw, bh, 8);
    uint32_t ink = readable_ink(CLR_MENU_BG), dim = readable_ink_dim(CLR_MENU_BG);
    // title (capitalised name)
    char title[16]; tm_cpy(title, m->name, 16);
    if (title[0] >= 'a' && title[0] <= 'z') title[0] -= 32;
    draw_text(bx + TM_PAD, by + 4, title, ink);
    draw_hline(bx + 4, by + TM_TITLE - 2, bw - 8, CLR_MENU_SEP);

    int iy = by + TM_TITLE + TM_PAD;
    for (int i = 0; i < m->n; i++) {
        tm_item *it = &m->items[i];
        int val = it->bind[0] ? tm_get(it->bind) : 0;
        if (it->type == TM_CHECK) {
            uint32_t bc = val ? 0xFF50C050 : CLR_MENU_CAT_BG;
            int32_t bxp = bx + TM_PAD, byp = iy + ui_px(4);
            draw_fill_rect(bxp, byp, TM_CHECK_BOX, TM_CHECK_BOX, bc);
            draw_rect_outline(bxp, byp, TM_CHECK_BOX, TM_CHECK_BOX, CLR_MENU_BORDER);
            if (val) { draw_fill_rect(bxp + ui_px(4), byp + ui_px(5), ui_px(3), ui_px(3), readable_ink(bc));
                       draw_fill_rect(bxp + ui_px(6), byp + ui_px(3), ui_px(5), ui_px(2), readable_ink(bc)); }
            draw_text(bx + TM_CHECK_TEXT_X, iy + ui_px(5), it->label, ink);
        } else if (it->type == TM_SLIDER) {
            draw_text(bx + TM_PAD, iy + ui_px(2), it->label, dim);
            char vb[8]; int v = val, d = 0; char tmp[8];
            if (v == 0) { vb[0]='0'; vb[1]=0; } else { while (v>0){tmp[d++]='0'+v%10;v/=10;} for(int k=0;k<d;k++) vb[k]=tmp[d-1-k]; vb[d]=0; }
            draw_text(bx + bw - TM_PAD - text_width(vb), iy + ui_px(2), vb, ink);
            int tx = bx + TM_PAD, tw = bw - TM_PAD * 2, ty = iy + TM_SLIDER_TRACK_Y;
            draw_fill_rect(tx, ty, tw, TM_SLIDER_TRACK_H, CLR_MENU_CAT_BG);
            int rng = it->vmax - it->vmin; if (rng < 1) rng = 1;
            int fill = (val - it->vmin) * tw / rng; if (fill < 0) fill = 0; if (fill > tw) fill = tw;
            draw_fill_rect(tx, ty, fill, TM_SLIDER_TRACK_H, readable_accent(0xFF5A78B0, CLR_MENU_BG));
            draw_circle_filled(tx + fill, ty + TM_SLIDER_TRACK_H / 2, TM_SLIDER_HANDLE_R, ink);
        } else if (it->type == TM_RADIO) {
            draw_text(bx + TM_PAD, iy + ui_px(5), it->label, dim);
            int ox = bx + TM_PAD + TM_RADIO_OPT_X;
            for (int o = 0; o < it->nopt; o++) {
                int ow = TM_RADIO_OPT_W;
                uint32_t obg = (val == o) ? readable_accent(0xFF3868A8, CLR_MENU_BG) : CLR_MENU_CAT_BG;
                draw_fill_rect(ox, iy + ui_px(3), ow - ui_px(4), TM_RADIO_OPT_H, obg);
                draw_rect_outline(ox, iy + ui_px(3), ow - ui_px(4), TM_RADIO_OPT_H, CLR_MENU_BORDER);
                draw_text(ox + ui_px(5), iy + ui_px(5), it->opt[o], (val==o)?readable_ink(obg):dim);
                ox += ow;
            }
        } else if (it->type == TM_ACTION) {   // #372: clickable action row
            draw_fill_rect(bx + TM_PAD, iy + ui_px(2), bw - TM_PAD * 2, TM_ACTION_H, CLR_MENU_CAT_BG);
            draw_rect_outline(bx + TM_PAD, iy + ui_px(2), bw - TM_PAD * 2, TM_ACTION_H, CLR_MENU_BORDER);
            draw_text_ttf(bx + TM_PAD + ui_px(8), iy + ui_px(4), it->label, 12, ink);
        }
        iy += tm_item_h(it);
    }
}


#ifdef MAYTERA_TESTHOOK
// (#231r) VERIFICATION ONLY. Compiled out of every shipping build by the same
// -DMAYTERA_TESTHOOK gate as testhook.c itself.
//
// Returns the screen point at which band `b`'s fader CAP sits when that band
// is at `pos`, derived from the VERY SAME snd_fx()/snd_cap_y() the renderer
// uses and through the VERY SAME tm_box() that positions the panel. A hook
// that then drives traymenu_handle_mouse() at this point is proving that the
// HIT-TEST accepts the exact pixel the DRAW code puts the cap at, and that
// snd_val_from_y() inverts snd_cap_y() exactly - at whatever UI scale the
// machine is running.
//
// That is a different and much stronger claim than "the panel renders", which
// is the distinction blame.md's #glassmodal entry was written about.
int traymenu_eq_fader_point(int b, int pos, int *out_x, int *out_y) {
    // snd_bands(), not SND_EQ_BANDS: the hook must not be able to target a
    // column the panel does not actually draw.
    if (b < 0 || b >= snd_bands()) return -1;
    tm_menu *m = tm_cur();
    if (!m || strcmp(m->name, "sound")) return -1;
    int bx, by, bw, bh;
    tm_box(m, &bx, &by, &bw, &bh);
    if (out_x) *out_x = snd_fx(bx, b);
    // The cap is ui_px(13) tall with its top at snd_cap_y() - ui_px(6), so
    // snd_cap_y() IS its centre line.
    if (out_y) *out_y = snd_cap_y(by, pos);
    return 0;
}
#endif

// ---- input ---------------------------------------------------------------
// Returns true if it consumed the event.
bool traymenu_handle_mouse(int mx, int my, bool pressed, bool held) {
    tm_menu *m = tm_cur(); if (!m) return false;
    int bx, by, bw, bh; tm_box(m, &bx, &by, &bw, &bh);

    int inside = (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
    if (pressed && !inside) { traymenu_close(); return true; }

    // #336: the analog EQ sound panel has its own hit-testing.
    if (!strcmp(m->name, "sound")) return snd_mouse(bx, by, mx, my, pressed, held);

    int iy = by + TM_TITLE + TM_PAD;
    for (int i = 0; i < m->n; i++) {
        tm_item *it = &m->items[i];
        int ih = tm_item_h(it);
        if (it->type == TM_CHECK) {
            if (pressed && my >= iy && my < iy + ih && mx >= bx && mx < bx + bw)
                tm_set(it->bind, tm_get(it->bind) ? 0 : 1);
        } else if (it->type == TM_SLIDER) {
            // #uiscale: ty and the grab tolerance now match TM_SLIDER_TRACK_Y/
            // TM_SLIDER_TRACK_H used to draw the track - was a raw `+24` and a
            // fixed -6/+6/-8/+14 tolerance around it, which drifted from the
            // draw side (this file's own report-2 bug class) the moment the
            // row started scaling.
            int tx = bx + TM_PAD, tw = bw - TM_PAD * 2, ty = iy + TM_SLIDER_TRACK_Y;
            int tol_x = ui_px(6), tol_y = ui_px(8);
            if (pressed && mx >= tx - tol_x && mx < tx + tw + tol_x &&
                my >= ty - tol_y && my < ty + TM_SLIDER_TRACK_H + tol_y)
                g_tm_drag = i;
        } else if (it->type == TM_RADIO) {
            if (pressed && my >= iy && my < iy + ih) {
                int ox = bx + TM_PAD + TM_RADIO_OPT_X;
                int obw = TM_RADIO_OPT_W - ui_px(4);   // matches the drawn box width
                for (int o = 0; o < it->nopt; o++) {
                    if (mx >= ox && mx < ox + obw) { tm_set(it->bind, o); break; }
                    ox += TM_RADIO_OPT_W;
                }
            }
        } else if (it->type == TM_ACTION) {   // #372
            if (pressed && my >= iy && my < iy + ih && mx >= bx && mx < bx + bw) {
                tm_action(it->bind);
                return true;   // menu may have closed / spawned an app
            }
        }
        iy += ih;
    }

    // Slider drag continuation.
    if (g_tm_drag >= 0 && g_tm_drag < m->n) {
        if (held) {
            tm_item *it = &m->items[g_tm_drag];
            // recompute that slider's track y
            int yy = by + TM_TITLE + TM_PAD;
            for (int i = 0; i < g_tm_drag; i++) yy += tm_item_h(&m->items[i]);
            int tx = bx + TM_PAD, tw = bw - TM_PAD * 2;
            int rng = it->vmax - it->vmin; if (rng < 1) rng = 1;
            int v = it->vmin + (mx - tx) * rng / (tw < 1 ? 1 : tw);
            if (v < it->vmin) v = it->vmin;
            if (v > it->vmax) v = it->vmax;
            tm_set(it->bind, v);
        } else g_tm_drag = -1;
    }
    return true;
}
