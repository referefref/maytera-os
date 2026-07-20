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

// Settings panel indices (must match the PANEL_* enum in apps/settings/main.c).
// set_settings_tab() opens Settings straight to the given panel.
#define NET_SETTINGS_TAB  3    // PANEL_NETWORK
#define BT_SETTINGS_TAB  14    // PANEL_BLUETOOTH
#define WIFI_SETTINGS_TAB 15   // PANEL_WIFI

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
    tm_item items[14];
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

// EQ bands are UI state (no audio DSP yet); volume is live via set_volume.
static int g_eq[5] = {50, 50, 50, 50, 50};

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
    if (!strncmp(b, "eq", 2)) { int i = b[2] - '0'; if (i >= 0 && i < 5) return g_eq[i]; }
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
    else if (!strncmp(b, "eq", 2)) { int i = b[2] - '0'; if (i >= 0 && i < 5) g_eq[i] = v; }
}

// #372: run a TM_ACTION item. Actions are dispatched by their bind string.
void traymenu_close(void);   // defined below
static void tm_action(const char *bind) {
    int tab = -1;
    if      (!strcmp(bind, "bt_settings"))   tab = BT_SETTINGS_TAB;
    else if (!strcmp(bind, "net_settings"))  tab = NET_SETTINGS_TAB;
    else if (!strcmp(bind, "wifi_settings")) tab = WIFI_SETTINGS_TAB;
    if (tab >= 0) {
        set_settings_tab(tab);               // one-shot: open Settings on that panel
        sys_spawn("/APPS/settings");
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
        "sound:\n"
        "  - {type: slider, label: Volume, bind: volume, min: 0, max: 100}\n"
        "  - {type: slider, label: Bass, bind: eq0, min: 0, max: 100}\n"
        "  - {type: slider, label: Mid, bind: eq2, min: 0, max: 100}\n"
        "  - {type: slider, label: Treble, bind: eq4, min: 0, max: 100}\n"
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
    for (int i = 0; i < wc && m->n < 13; i++) {   // leave room for the opacity slider
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
#define TM_W      214
#define TM_PAD    8
#define TM_TITLE  22
static int tm_item_h(const tm_item *it) {
    if (it->type == TM_SLIDER) return 40;
    return 26;
}
#define SND_W_FWD 308
#define SND_H_FWD 214
static void tm_box(const tm_menu *m, int *bx, int *by, int *bw, int *bh) {
    int h = TM_TITLE + TM_PAD * 2;
    for (int i = 0; i < m->n; i++) h += tm_item_h(&m->items[i]);
    int w = TM_W;
    // #336: the analog EQ "sound" panel is a fixed-size hardware face.
    if (!strcmp(m->name, "sound")) { w = SND_W_FWD; h = SND_H_FWD; }
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
        if (y + h > g_fb_height - 4) y = g_fb_height - h - 4;
    } else if (g_dock_style == DOCK_DEFAULT) {
        y = (g_fb_height - TASKBAR_HEIGHT) - h - 6;
    } else {
        y = g_tray_bar_y - h - 6;
    }
    if (y < 4) y = 4;
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
// ==========================================================================
extern int g_tray_muted;                 // taskbar.c (global): live mute state

#define SND_W    308
#define SND_H    214
#define SND_TY0(by)  ((by) + 48)         // top of the fader travel
#define SND_FH       102                 // fader travel height
static const char *EQ_FREQ[5] = { "60", "250", "1k", "4k", "12k" };
// Rotary-knob pointer directions (0=min lower-left .. 18=max lower-right),
// unit vectors x100 sweeping 270 degrees through straight-up. Screen coords.
static const signed char KNOB_DX[19]={-71,-87,-97,-100,-97,-87,-71,-50,-26,0,26,50,71,87,97,100,97,87,71};
static const signed char KNOB_DY[19]={71,50,26,0,-26,-50,-71,-87,-97,-100,-97,-87,-71,-50,-26,0,26,50,71};

static int  snd_fx(int bx, int i) { return bx + 28 + i * 36; }   // band column center
static void snd_master(int bx, int by, int *kcx, int *kcy, int *kr) {
    *kcx = bx + 254; *kcy = by + 82; *kr = 27;
}
static void snd_mute(int bx, int by, int *mx, int *my, int *mw, int *mh) {
    *mx = bx + 228; *my = by + 150; *mw = 52; *mh = 24;
}

// Draw a short radial "needle" line inside the knob using the direction table.
static void snd_needle(int cx, int cy, int r0, int r1, int k, uint32_t col) {
    if (k < 0) k = 0; if (k > 18) k = 18;
    for (int r = r0; r <= r1; r++) {
        int px = cx + KNOB_DX[k] * r / 100;
        int py = cy + KNOB_DY[k] * r / 100;
        draw_fill_rect(px, py, 2, 2, col);
    }
}

static void snd_render(int bx, int by) {
    // --- Brushed-metal face with a beveled, red-trimmed frame ---
    draw_gradient_v(bx, by, SND_W, SND_H, 0x00393C42, 0x0022242A);
    draw_rect_outline(bx, by, SND_W, SND_H, 0x00121317);
    draw_rect_outline(bx + 1, by + 1, SND_W - 2, SND_H - 2, 0x005A5E68);  // bevel highlight
    draw_hline(bx + 2, by + SND_H - 3, SND_W - 4, 0x00141519);            // bottom shade
    // red accent trim under the header
    draw_hline(bx + 8, by + 30, SND_W - 16, 0x00C0392B);
    draw_hline(bx + 8, by + 31, SND_W - 16, 0x00521812);

    // --- Header: red power LED + model name (TTF) ---
    draw_circle_filled(bx + 16, by + 15, 4, 0x00E8402C);
    draw_circle_filled(bx + 15, by + 14, 1, 0x00FFD0C0);   // LED glint
    draw_text_ttf(bx + 28, by + 7, "MAYTERA", 12, 0x00E8E4D8);
    draw_text_ttf(bx + 100, by + 7, "EQ-560", 12, 0x00E8402C);
    draw_text_ttf(bx + SND_W - 92, by + 9, "GRAPHIC EQ", 10, 0x008A8E96);

    // --- 5 band faders ---
    int ty0 = SND_TY0(by);
    for (int i = 0; i < 5; i++) {
        int fx = snd_fx(bx, i);
        int val = g_eq[i];
        // recessed slot
        draw_fill_rect(fx - 4, ty0 - 3, 8, SND_FH + 6, 0x00101115);
        draw_rect_outline(fx - 4, ty0 - 3, 8, SND_FH + 6, 0x00050608);
        draw_vline(fx - 3, ty0 - 2, SND_FH + 4, 0x001C1E24);
        // tick marks (0/25/50/75/100)
        for (int t = 0; t <= 4; t++) {
            int tyy = ty0 + t * SND_FH / 4;
            draw_hline(fx - 12, tyy, 6, (t == 2) ? 0x00706048 : 0x0044484F);
            draw_hline(fx + 6,  tyy, 6, (t == 2) ? 0x00706048 : 0x0044484F);
        }
        // level fill (accent) below the cap
        int capY = ty0 + (100 - val) * SND_FH / 100;
        draw_gradient_v(fx - 2, capY, 4, ty0 + SND_FH - capY, 0x00C0392B, 0x00521812);
        // metal fader cap with red index line
        int cy = capY - 6;
        draw_gradient_v(fx - 13, cy, 26, 13, 0x00D8DCE2, 0x008A8E96);
        draw_rect_outline(fx - 13, cy, 26, 13, 0x00202227);
        draw_hline(fx - 12, cy + 1, 24, 0x00F2F4F8);        // top highlight
        draw_hline(fx - 11, cy + 6, 22, 0x00E8402C);        // red index
        // band label (TTF, centered)
        int lw = text_width_ttf(EQ_FREQ[i], 10);
        draw_text_ttf(fx - lw / 2, ty0 + SND_FH + 8, EQ_FREQ[i], 10, 0x00C8CCD4);
    }

    // --- Divider between the band bank and the master section ---
    draw_vline(bx + 210, by + 40, SND_H - 52, 0x00141519);
    draw_vline(bx + 211, by + 40, SND_H - 52, 0x004A4E56);

    // --- MASTER rotary knob ---
    int kcx, kcy, kr; snd_master(bx, by, &kcx, &kcy, &kr);
    int vol = get_volume(); if (vol < 0) vol = 0; if (vol > 100) vol = 100;
    int kk = vol * 18 / 100;
    draw_text_ttf(kcx - text_width_ttf("MASTER", 10) / 2, by + 44, "MASTER", 10, 0x00C8CCD4);
    draw_circle_filled(kcx, kcy, kr + 2, 0x00101115);        // socket
    draw_circle_filled(kcx, kcy, kr, 0x004A4E56);            // knob rim (bright)
    draw_circle_filled(kcx, kcy, kr - 1, 0x00161A20);        // rim shadow ring
    // #341: proper round rotary dial built from concentric circles ONLY (the old
    // square gradient dome poked its corners past the rim = "square in a circle").
    draw_circle_filled(kcx, kcy, kr - 3, 0x00363940);        // dial face
    draw_circle_filled(kcx, kcy - 2, kr - 6, 0x00474B54);    // upper dome sheen
    draw_circle_filled(kcx, kcy + 1, kr - 10, 0x00303339);   // recessed center
    draw_circle_outline(kcx, kcy, kr - 3, 0x00686C76);       // crisp bright rim
    // Angled pointer indicator from center out to the current level.
    snd_needle(kcx, kcy, 5, kr - 6, kk, 0x00E8402C);         // red pointer
    draw_circle_filled(kcx, kcy, 3, 0x00D8DCE2);             // hub cap
    // numeric readout
    char vb[8]; int v = vol, d = 0, tmp[8];
    if (!v) { vb[0] = '0'; vb[1] = 0; } else { while (v) { tmp[d++] = v % 10; v /= 10; } for (int q = 0; q < d; q++) vb[q] = '0' + tmp[d - 1 - q]; vb[d] = 0; }
    draw_text_ttf(kcx - text_width_ttf(vb, 12) / 2, kcy + kr + 4, vb, 12, 0x00E8E4D8);

    // --- MUTE switch ---
    int mx, my, mw, mh; snd_mute(bx, by, &mx, &my, &mw, &mh);
    uint32_t mtop = g_tray_muted ? 0x00E8402C : 0x00363940;
    uint32_t mbot = g_tray_muted ? 0x00901F14 : 0x00202227;
    draw_gradient_v(mx, my, mw, mh, mtop, mbot);
    draw_rect_outline(mx, my, mw, mh, 0x00101115);
    draw_rect_outline(mx + 1, my + 1, mw - 2, mh - 2, g_tray_muted ? 0x00F08070 : 0x00565A63);
    const char *ml = "MUTE";
    draw_text_ttf(mx + (mw - text_width_ttf(ml, 11)) / 2, my + 6, ml,
                  11, g_tray_muted ? 0x00FFFFFF : 0x0090949C);
}

// Hit-test / drag for the analog sound panel. Returns true if consumed.
static bool snd_mouse(int bx, int by, int mx, int my, bool pressed, bool held) {
    int ty0 = SND_TY0(by);
    // Band faders (press starts a drag; g_tm_drag holds the band index 0..4).
    for (int i = 0; i < 5; i++) {
        int fx = snd_fx(bx, i);
        if (pressed && mx >= fx - 14 && mx <= fx + 14 &&
            my >= ty0 - 10 && my <= ty0 + SND_FH + 10) {
            g_tm_drag = i;
        }
    }
    // Master knob: press/drag around it maps direction -> volume.
    int kcx, kcy, kr; snd_master(bx, by, &kcx, &kcy, &kr);
    int ddx = mx - kcx, ddy = my - kcy;
    if (pressed && (ddx * ddx + ddy * ddy) <= (kr + 8) * (kr + 8)) {
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

    if (g_tm_drag >= 0 && (held || pressed)) {
        if (g_tm_drag == 90) {
            // nearest pointer direction -> volume fraction
            int best = 9, bestdot = -1000000;
            for (int k = 0; k < 19; k++) {
                int dot = ddx * KNOB_DX[k] + ddy * KNOB_DY[k];
                if (dot > bestdot) { bestdot = dot; best = k; }
            }
            int vol = best * 100 / 18;
            if (vol < 0) vol = 0; if (vol > 100) vol = 100;
            set_volume(vol);
        } else if (g_tm_drag >= 0 && g_tm_drag < 5) {
            int v = (ty0 + SND_FH - my) * 100 / SND_FH;
            if (v < 0) v = 0; if (v > 100) v = 100;
            g_eq[g_tm_drag] = v;
        }
    }
    if (!held) g_tm_drag = -1;
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
    draw_fill_rect(bx, by, bw, bh, 0x00262A33);
    draw_rect_outline(bx, by, bw, bh, 0x0090A0B0);
    // title (capitalised name)
    char title[16]; tm_cpy(title, m->name, 16);
    if (title[0] >= 'a' && title[0] <= 'z') title[0] -= 32;
    draw_text(bx + TM_PAD, by + 4, title, 0x00FFD040);
    draw_hline(bx + 4, by + TM_TITLE - 2, bw - 8, 0x00404854);

    int iy = by + TM_TITLE + TM_PAD;
    for (int i = 0; i < m->n; i++) {
        tm_item *it = &m->items[i];
        int val = it->bind[0] ? tm_get(it->bind) : 0;
        if (it->type == TM_CHECK) {
            uint32_t bc = val ? 0x0050C050 : 0x00404854;
            draw_fill_rect(bx + TM_PAD, iy + 4, 14, 14, bc);
            draw_rect_outline(bx + TM_PAD, iy + 4, 14, 14, 0x00B0B8C0);
            if (val) { draw_fill_rect(bx + TM_PAD + 4, iy + 9, 3, 3, 0x00FFFFFF);
                       draw_fill_rect(bx + TM_PAD + 6, iy + 7, 5, 2, 0x00FFFFFF); }
            draw_text(bx + TM_PAD + 22, iy + 5, it->label, 0x00E8E8E8);
        } else if (it->type == TM_SLIDER) {
            draw_text(bx + TM_PAD, iy + 2, it->label, 0x00C8D0D8);
            char vb[8]; int v = val, d = 0; char tmp[8];
            if (v == 0) { vb[0]='0'; vb[1]=0; } else { while (v>0){tmp[d++]='0'+v%10;v/=10;} for(int k=0;k<d;k++) vb[k]=tmp[d-1-k]; vb[d]=0; }
            draw_text(bx + bw - TM_PAD - text_width(vb), iy + 2, vb, 0x00FFFFFF);
            int tx = bx + TM_PAD, tw = bw - TM_PAD * 2, ty = iy + 24;
            draw_fill_rect(tx, ty, tw, 6, 0x00404854);
            int rng = it->vmax - it->vmin; if (rng < 1) rng = 1;
            int fill = (val - it->vmin) * tw / rng; if (fill < 0) fill = 0; if (fill > tw) fill = tw;
            draw_fill_rect(tx, ty, fill, 6, 0x005A78B0);
            draw_circle_filled(tx + fill, ty + 3, 5, 0x00FFFFFF);
        } else if (it->type == TM_RADIO) {
            draw_text(bx + TM_PAD, iy + 5, it->label, 0x00C8D0D8);
            int ox = bx + TM_PAD + 64;
            for (int o = 0; o < it->nopt; o++) {
                int ow = 64;
                uint32_t obg = (val == o) ? 0x00385078 : 0x00333842;
                draw_fill_rect(ox, iy + 3, ow - 4, 18, obg);
                draw_rect_outline(ox, iy + 3, ow - 4, 18, 0x00606870);
                draw_text(ox + 5, iy + 5, it->opt[o], (val==o)?0x00FFFFFF:0x00B0B8C0);
                ox += ow;
            }
        } else if (it->type == TM_ACTION) {   // #372: clickable action row
            draw_fill_rect(bx + TM_PAD, iy + 2, bw - TM_PAD * 2, 20, 0x00333842);
            draw_rect_outline(bx + TM_PAD, iy + 2, bw - TM_PAD * 2, 20, 0x00606870);
            draw_text_ttf(bx + TM_PAD + 8, iy + 4, it->label, 12, 0x00E8E8E8);
        }
        iy += tm_item_h(it);
    }
}

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
            int tx = bx + TM_PAD, tw = bw - TM_PAD * 2, ty = iy + 24;
            if (pressed && mx >= tx - 6 && mx < tx + tw + 6 && my >= ty - 8 && my < ty + 14)
                g_tm_drag = i;
        } else if (it->type == TM_RADIO) {
            if (pressed && my >= iy && my < iy + ih) {
                int ox = bx + TM_PAD + 64;
                for (int o = 0; o < it->nopt; o++) {
                    if (mx >= ox && mx < ox + 60) { tm_set(it->bind, o); break; }
                    ox += 64;
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
