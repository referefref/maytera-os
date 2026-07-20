/* Maytera Arena - hud.c
 * In-game HUD drawn directly into the ARGB blit buffer AFTER the 3D frame:
 * crosshair, health/armor, weapon+ammo, frag counter, kill feed, damage
 * flash and the death/respawn overlay.
 *
 * This file also owns the shared 2D drawing primitives (pixel/rect/text)
 * used by menu.c, exported with an arena_ prefix so the two files reuse ONE
 * implementation (no per-file forks). The glyphs come from the existing
 * public-domain 8x8 font already shipped in libgl (font8x8_basic.h), reused
 * here rather than re-inventing a bitmap font.
 */
#include "game.h"

/* The libgl font header declares `static GLbyte font8x8_basic[256][8]`.
 * It only needs the GLbyte type; provide it without dragging in GL/gl.h. */
typedef signed char GLbyte;
#include "../../libgl/src/font8x8_basic.h"

/* ==================================================================== */
/* Shared 2D primitives (also used by menu.c via extern declarations)    */
/* ==================================================================== */

static inline void put_pixel(uint32_t *blit, int W, int H, int x, int y,
                             uint32_t rgb) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    blit[y * W + x] = rgb;
}

static inline uint32_t blend_rgb(uint32_t dst, uint32_t src, int alpha) {
    /* alpha 0..255: out = src*a + dst*(1-a), per channel */
    int ia = 255 - alpha;
    uint32_t r = (((src >> 16) & 0xFF) * alpha + ((dst >> 16) & 0xFF) * ia) / 255;
    uint32_t g = (((src >>  8) & 0xFF) * alpha + ((dst >>  8) & 0xFF) * ia) / 255;
    uint32_t b = (((src      ) & 0xFF) * alpha + ((dst      ) & 0xFF) * ia) / 255;
    return (r << 16) | (g << 8) | b;
}

void arena_fill_rect(uint32_t *blit, int W, int H, int x, int y,
                     int rw, int rh, uint32_t rgb) {
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > W) rw = W - x;
    if (y + rh > H) rh = H - y;
    if (rw <= 0 || rh <= 0) return;
    for (int j = 0; j < rh; j++) {
        uint32_t *row = blit + (y + j) * W + x;
        for (int i = 0; i < rw; i++) row[i] = rgb;
    }
}

/* Alpha-blended rect: dim/tint helper. alpha 0..255. */
void arena_blend_rect(uint32_t *blit, int W, int H, int x, int y,
                      int rw, int rh, uint32_t rgb, int alpha) {
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > W) rw = W - x;
    if (y + rh > H) rh = H - y;
    if (rw <= 0 || rh <= 0) return;
    for (int j = 0; j < rh; j++) {
        uint32_t *row = blit + (y + j) * W + x;
        for (int i = 0; i < rw; i++) row[i] = blend_rgb(row[i], rgb, alpha);
    }
}

/* 8x8 bitmap text, integer scale. Advance = 8*scale per char. */
void arena_text(uint32_t *blit, int W, int H, int x, int y,
                const char *s, uint32_t rgb, int scale) {
    if (!s || scale < 1) return;
    int cx = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n') { cx = x; y += 9 * scale; continue; }
        if (c > 127) c = '?';
        const GLbyte *glyph = font8x8_basic[c];
        for (int row = 0; row < 8; row++) {
            int bits = glyph[row];
            if (!bits) continue;
            for (int col = 0; col < 8; col++) {
                if (!(bits & (1 << col))) continue;
                if (scale == 1) {
                    put_pixel(blit, W, H, cx + col, y + row, rgb);
                } else {
                    arena_fill_rect(blit, W, H, cx + col * scale,
                                    y + row * scale, scale, scale, rgb);
                }
            }
        }
        cx += 8 * scale;
    }
}

/* Text with a 1*scale drop shadow, for readability over the 3D scene. */
void arena_text_sh(uint32_t *blit, int W, int H, int x, int y,
                   const char *s, uint32_t rgb, int scale) {
    arena_text(blit, W, H, x + scale, y + scale, s, 0x00101010, scale);
    arena_text(blit, W, H, x, y, s, rgb, scale);
}

int arena_text_width(const char *s, int scale) {
    int n = 0, best = 0;
    if (!s) return 0;
    for (; *s; s++) {
        if (*s == '\n') { if (n > best) best = n; n = 0; continue; }
        n++;
    }
    if (n > best) best = n;
    return best * 8 * scale;
}

/* Small int -> decimal string (no float printf dependency). */
void arena_itoa(int v, char *out) {
    char tmp[16];
    int  i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    do { tmp[i++] = (char)('0' + (v % 10)); v /= 10; } while (v && i < 15);
    int o = 0;
    if (neg) out[o++] = '-';
    while (i > 0) out[o++] = tmp[--i];
    out[o] = 0;
}

/* ==================================================================== */
/* Kill feed - inferred from frag/death deltas cached between frames     */
/* ==================================================================== */
#define KF_LINES   5
#define KF_TTL_MS  4000
typedef struct { char msg[64]; int ttl_ms; uint32_t rgb; } KfLine;
static KfLine g_kf[KF_LINES];
static int    g_kf_prev_frags[MAX_PLAYERS];
static int    g_kf_prev_deaths[MAX_PLAYERS];
static int    g_kf_valid = 0;
static int    g_kf_prev_np = 0;

static void kf_append(const char *m, uint32_t rgb) {
    for (int i = KF_LINES - 1; i > 0; i--) g_kf[i] = g_kf[i - 1];
    int n = (int)strlen(m);
    if (n > 63) n = 63;
    memcpy(g_kf[0].msg, m, (unsigned)n);
    g_kf[0].msg[n] = 0;
    g_kf[0].ttl_ms = KF_TTL_MS;
    g_kf[0].rgb = rgb;
}

static void kf_cat(char *dst, int cap, const char *src) {
    int l = (int)strlen(dst);
    while (*src && l < cap - 1) dst[l++] = *src++;
    dst[l] = 0;
}

static void kf_update(World *w, int dt_ms) {
    for (int i = 0; i < KF_LINES; i++)
        if (g_kf[i].ttl_ms > 0) g_kf[i].ttl_ms -= dt_ms;

    if (!g_kf_valid || g_kf_prev_np != w->nplayers) {
        for (int i = 0; i < MAX_PLAYERS && i < w->nplayers; i++) {
            g_kf_prev_frags[i]  = w->players[i].frags;
            g_kf_prev_deaths[i] = w->players[i].deaths;
        }
        g_kf_prev_np = w->nplayers;
        g_kf_valid = 1;
        return;
    }

    /* find the (usually single) attacker whose frag count rose this frame */
    int attacker = -1, n_attackers = 0;
    for (int i = 0; i < w->nplayers && i < MAX_PLAYERS; i++)
        if (w->players[i].frags > g_kf_prev_frags[i]) { attacker = i; n_attackers++; }
    if (n_attackers > 1) attacker = -1;   /* ambiguous this frame */

    for (int v = 0; v < w->nplayers && v < MAX_PLAYERS; v++) {
        if (w->players[v].deaths <= g_kf_prev_deaths[v]) continue;
        char line[64]; line[0] = 0;
        uint32_t col = 0x00D0D0D0;
        int lp = w->local_player;
        if (attacker >= 0 && attacker != v) {
            if (attacker == lp) {
                kf_cat(line, 64, "You fragged ");
                kf_cat(line, 64, w->players[v].name);
                col = 0x0060FF60;
            } else if (v == lp) {
                kf_cat(line, 64, w->players[attacker].name);
                kf_cat(line, 64, " fragged you");
                col = 0x00FF6060;
            } else {
                kf_cat(line, 64, w->players[attacker].name);
                kf_cat(line, 64, " fragged ");
                kf_cat(line, 64, w->players[v].name);
            }
        } else {
            if (v == lp) { kf_cat(line, 64, "You died"); col = 0x00FF6060; }
            else { kf_cat(line, 64, w->players[v].name); kf_cat(line, 64, " died"); }
        }
        kf_append(line, col);
    }

    for (int i = 0; i < w->nplayers && i < MAX_PLAYERS; i++) {
        g_kf_prev_frags[i]  = w->players[i].frags;
        g_kf_prev_deaths[i] = w->players[i].deaths;
    }
}

/* ==================================================================== */
/* Damage flash state                                                    */
/* ==================================================================== */
static int g_last_hp   = -1;
static int g_flash_ms  = 0;

/* ==================================================================== */
/* HUD pieces                                                            */
/* ==================================================================== */
static void draw_crosshair(uint32_t *blit, int W, int H) {
    int cx = W / 2, cy = H / 2;
    int gap = 4, len = 7;
    uint32_t c = 0x00E8FFE8, sh = 0x00103010;
    /* soft shadow first */
    for (int d = gap; d < gap + len; d++) {
        put_pixel(blit, W, H, cx + d + 1, cy + 1, sh);
        put_pixel(blit, W, H, cx - d + 1, cy + 1, sh);
        put_pixel(blit, W, H, cx + 1, cy + d + 1, sh);
        put_pixel(blit, W, H, cx + 1, cy - d + 1, sh);
    }
    for (int d = gap; d < gap + len; d++) {
        put_pixel(blit, W, H, cx + d, cy, c);
        put_pixel(blit, W, H, cx - d, cy, c);
        put_pixel(blit, W, H, cx, cy + d, c);
        put_pixel(blit, W, H, cx, cy - d, c);
    }
    put_pixel(blit, W, H, cx, cy, c);
}

static uint32_t vital_color(int v) {
    if (v >= 66) return 0x0040F050;   /* green  */
    if (v >= 33) return 0x00F0D030;   /* yellow */
    return 0x00F04030;                /* red    */
}

static void draw_damage_flash(uint32_t *blit, int W, int H) {
    if (g_flash_ms <= 0) return;
    int a = (g_flash_ms * 150) / 500;         /* peak alpha 150 */
    if (a > 150) a = 150;
    int tv = H / 8, th = W / 10;               /* band thickness  */
    uint32_t red = 0x00C00000;
    /* two-layer edge vignette: outer strong band + inner soft band */
    arena_blend_rect(blit, W, H, 0, 0,       W, tv,      red, a);
    arena_blend_rect(blit, W, H, 0, H - tv,  W, tv,      red, a);
    arena_blend_rect(blit, W, H, 0, tv,      th, H-2*tv, red, a);
    arena_blend_rect(blit, W, H, W - th, tv, th, H-2*tv, red, a);
    arena_blend_rect(blit, W, H, 0, 0,       W, tv/2,    red, a/2);
    arena_blend_rect(blit, W, H, 0, H-tv/2,  W, tv/2,    red, a/2);
}

/* ==================================================================== */
/* hud_draw                                                              */
/* ==================================================================== */
void hud_draw(World *w, uint32_t *blit, int width, int height) {
    if (!w || !blit || width <= 0 || height <= 0) return;

    /* menu / scoreboard already fully drawn by menu_draw (called before us
     * in main.c) - do not scribble HUD elements on top of them.            */
    if (w->state == GS_MENU || w->state == GS_SCOREBOARD) {
        g_kf_valid = 0;             /* re-snapshot when play resumes */
        return;
    }
    if (w->state == GS_GAMEOVER) {
        /* main.c only calls menu_draw for MENU/SCOREBOARD, so the game
         * over screen is delegated from here (menu.c implements it).      */
        menu_draw(w, blit, width, height);
        return;
    }

    int lp = w->local_player;
    if (lp < 0 || lp >= w->nplayers || lp >= MAX_PLAYERS) return;
    PlayerSlot *ps = &w->players[lp];
    Entity *pe = (ps->entity >= 0) ? ent_get(w, ps->entity) : 0;

    const int dt = 16;              /* main.c runs a fixed 16 ms frame */
    kf_update(w, dt);

    /* ---- damage flash bookkeeping ---- */
    if (pe) {
        if (g_last_hp > 0 && pe->health < g_last_hp) {
            g_flash_ms += 150 + (g_last_hp - pe->health) * 8;
            if (g_flash_ms > 500) g_flash_ms = 500;
        }
        g_last_hp = pe->health;
    } else {
        g_last_hp = -1;
    }
    if (g_flash_ms > 0) g_flash_ms -= dt;
    draw_damage_flash(blit, width, height);

    const int m = 14;
    char num[16], buf[64];

    /* ---- kill feed, top-left ---- */
    {
        int y = m;
        for (int i = 0; i < KF_LINES; i++) {
            if (g_kf[i].ttl_ms <= 0) continue;
            int a = g_kf[i].ttl_ms >= 1000 ? 110 : (g_kf[i].ttl_ms * 110) / 1000;
            int tw = arena_text_width(g_kf[i].msg, 1);
            arena_blend_rect(blit, width, height, m - 4, y - 2, tw + 8, 12,
                             0x00000000, a);
            arena_text_sh(blit, width, height, m, y, g_kf[i].msg,
                          g_kf[i].rgb, 1);
            y += 13;
        }
    }

    /* ---- frags, top-right ---- */
    {
        buf[0] = 0;
        kf_cat(buf, 64, "FRAGS ");
        arena_itoa(ps->frags, num); kf_cat(buf, 64, num);
        kf_cat(buf, 64, " / ");
        arena_itoa(w->frag_limit > 0 ? w->frag_limit : FRAG_LIMIT, num);
        kf_cat(buf, 64, num);
        int tw = arena_text_width(buf, 2);
        arena_blend_rect(blit, width, height, width - m - tw - 8, m - 4,
                         tw + 12, 24, 0x00000000, 100);
        arena_text_sh(blit, width, height, width - m - tw, m, buf,
                      0x00F0F0F0, 2);
    }

    if (pe && pe->health > 0) {
        /* ---- health + armor, bottom-left ---- */
        int hy = height - m - 32;
        arena_blend_rect(blit, width, height, m - 6, hy - 16,
                         236, 54, 0x00000000, 90);
        arena_text_sh(blit, width, height, m, hy - 12, "HEALTH",
                      0x00A0A0A0, 1);
        arena_itoa(pe->health, num);
        arena_text_sh(blit, width, height, m, hy, num,
                      vital_color(pe->health), 4);
        int ax = m + 128;
        arena_text_sh(blit, width, height, ax, hy - 12, "ARMOR",
                      0x00A0A0A0, 1);
        arena_itoa(pe->armor, num);
        arena_text_sh(blit, width, height, ax, hy + 8, num,
                      pe->armor > 0 ? 0x0060A0F0 : 0x00707070, 3);

        /* ---- weapon + ammo, bottom-right ---- */
        int wpn = pe->weapon;
        if (wpn >= 0 && wpn < NUM_WEAPONS) {
            const char *wname = g_weapons[wpn].name ? g_weapons[wpn].name
                                                    : "?";
            if (wpn == W_GAUNTLET) { num[0] = '-'; num[1] = '-'; num[2] = 0; }
            else arena_itoa(pe->ammo[wpn], num);
            int aw = arena_text_width(num, 4);
            int nw = arena_text_width(wname, 1);
            int bw = (aw > nw ? aw : nw) + 16;
            arena_blend_rect(blit, width, height, width - m - bw - 6,
                             hy - 16, bw + 12, 54, 0x00000000, 90);
            arena_text_sh(blit, width, height, width - m - nw, hy - 12,
                          wname, 0x00A0A0A0, 1);
            uint32_t acol = 0x00F0F0F0;
            if (wpn != W_GAUNTLET) {
                if (pe->ammo[wpn] <= 0)      acol = 0x00F04030;
                else if (pe->ammo[wpn] < 10) acol = 0x00F0D030;
            }
            arena_text_sh(blit, width, height, width - m - aw, hy, num,
                          acol, 4);
        }

        draw_crosshair(blit, width, height);
    } else {
        /* ---- dead / respawning overlay ---- */
        arena_blend_rect(blit, width, height, 0, 0, width, height,
                         0x00200000, 90);
        const char *t1 = "FRAGGED!";
        const char *t2 = "respawning...";
        int w1 = arena_text_width(t1, 4);
        int w2 = arena_text_width(t2, 2);
        arena_text_sh(blit, width, height, (width - w1) / 2,
                      height / 2 - 40, t1, 0x00FF3030, 4);
        arena_text_sh(blit, width, height, (width - w2) / 2,
                      height / 2 + 8, t2, 0x00E0E0E0, 2);
    }
}

/* ====================================================================== */
/* hud_minimap - overhead radar in the top-right corner. Draws wall/brush   */
/* footprints, the player (arrow = facing), bots, items and barrels. Auto-   */
/* scales to the level bounds. Cheap: a handful of rects per brush/entity.   */
/* ====================================================================== */
static void mm_px(uint32_t *b, int W, int H, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    b[y * W + x] = c;
}
static void mm_dot(uint32_t *b, int W, int H, int x, int y, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r) mm_px(b, W, H, x+dx, y+dy, c);
}

void hud_minimap(World *w, uint32_t *blit, int width, int height) {
    if (!w || !blit || width <= 0 || height <= 0) return;
    Level *lv = &w->level;

    int size = 168;
    if (size > width / 3)  size = width / 3;
    if (size > height / 3) size = height / 3;
    int pad  = 12;
    int x0 = width - size - pad;
    int y0 = 44;                          /* below the FRAGS readout           */

    /* frame + translucent backdrop */
    arena_blend_rect(blit, width, height, x0 - 2, y0 - 2, size + 4, size + 4,
                     0x00000000, 150);
    arena_fill_rect(blit, width, height, x0 - 2, y0 - 2, size + 4, 1, 0x00304050);
    arena_fill_rect(blit, width, height, x0 - 2, y0 + size + 1, size + 4, 1, 0x00304050);
    arena_fill_rect(blit, width, height, x0 - 2, y0 - 2, 1, size + 4, 0x00304050);
    arena_fill_rect(blit, width, height, x0 + size + 1, y0 - 2, 1, size + 4, 0x00304050);

    float wminx = lv->world_mins.x, wmaxx = lv->world_maxs.x;
    float wminy = lv->world_mins.y, wmaxy = lv->world_maxs.y;
    float ww = wmaxx - wminx, wh = wmaxy - wminy;
    if (ww < 1.0f) ww = 1.0f;
    if (wh < 1.0f) wh = 1.0f;
    float sx = (float)size / ww, sy = (float)size / wh;
    float worldarea = ww * wh;

    /* wall / structure footprints (skip the giant ground slab) */
    for (int i = 0; i < lv->nbrush && i < MAX_BRUSHES; i++) {
        Brush *br = &lv->brushes[i];
        float bw = br->maxs.x - br->mins.x, bh = br->maxs.y - br->mins.y;
        if (bw * bh > worldarea * 0.75f) continue;        /* the floor slab      */
        int rx = x0 + (int)((br->mins.x - wminx) * sx);
        int ry = y0 + (int)((wmaxy - br->maxs.y) * sy);   /* flip: north = up    */
        int rw = (int)(bw * sx); if (rw < 1) rw = 1;
        int rh = (int)(bh * sy); if (rh < 1) rh = 1;
        if (rx < x0) { rw -= x0 - rx; rx = x0; }
        if (ry < y0) { rh -= y0 - ry; ry = y0; }
        if (rx + rw > x0 + size) rw = x0 + size - rx;
        if (ry + rh > y0 + size) rh = y0 + size - ry;
        if (rw <= 0 || rh <= 0) continue;
        int tall = (br->maxs.z - br->mins.z) > 96.0f;
        arena_blend_rect(blit, width, height, rx, ry, rw, rh,
                         tall ? 0x006888A8 : 0x00465868, tall ? 210 : 150);
    }

    /* entities: items, barrels, bots, then the local player (drawn last/top) */
    int local_ent = (w->local_player >= 0 && w->local_player < MAX_PLAYERS)
                    ? w->players[w->local_player].entity : -1;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *e = &w->ents[i];
        if (!e->alive || i == local_ent) continue;
        int ex = x0 + (int)((e->pos.x - wminx) * sx);
        int ey = y0 + (int)((wmaxy - e->pos.y) * sy);
        if (ex < x0 || ey < y0 || ex >= x0 + size || ey >= y0 + size) continue;
        if (e->type == ET_ITEM && e->respawn_ms <= 0) {
            uint32_t c = 0x00F0D040;
            if (e->item_kind == IT_HEALTH) c = 0x0050FF60;
            else if (e->item_kind == IT_ARMOR) c = 0x0060A0FF;
            else if (e->item_kind == IT_MEGA)  c = 0x00FF60FF;
            else if (e->item_kind == IT_AMMO)  c = 0x00FF9030;
            mm_dot(blit, width, height, ex, ey, 1, c);
        } else if (e->type == ET_PROP && e->prop_kind == PROP_BARREL) {
            mm_dot(blit, width, height, ex, ey, 2, 0x00FF5020);
        } else if ((e->type == ET_BOT) && e->health > 0) {
            mm_dot(blit, width, height, ex, ey, 2, 0x00FF4040);
        }
    }

    /* local player as a facing arrow */
    Entity *p = (local_ent >= 0) ? &w->ents[local_ent] : 0;
    if (p && p->alive) {
        int px = x0 + (int)((p->pos.x - wminx) * sx);
        int py = y0 + (int)((wmaxy - p->pos.y) * sy);
        float dx = mx_cosf(p->yaw), dy = mx_sinf(p->yaw);   /* world facing      */
        for (int t = 0; t <= 9; t++) {                       /* nose line         */
            int lx = px + (int)(dx * (float)t);
            int ly = py - (int)(dy * (float)t);              /* screen y flipped  */
            mm_px(blit, width, height, lx, ly, 0x0040E0FF);
        }
        mm_dot(blit, width, height, px, py, 2, 0x0000FFFF);
    }
}
