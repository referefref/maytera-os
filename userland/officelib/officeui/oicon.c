// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// oicon.c - shared MICO icon loader (see oicon.h).
#include "oicon.h"
#include "oui.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#define OICON_SRC_MAX 64      // loaders cap the source at 64x64 (every shipped icon is)

typedef struct {
    char name[16];
    int  size;
    int  state;               // 0 empty, 1 loaded, -1 tried-and-missing
    unsigned char cov[OICON_MAX_SIZE * OICON_MAX_SIZE];   // 0..255 coverage at `size`
} oicon_slot_t;

static oicon_slot_t g_slots[OICON_SLOTS];
static int g_nslots = 0;
static int g_next_evict = 0;

static int oicon_name_eq(const char *a, const char *b) {
    for (int i = 0; i < 15; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

// Read the whole .ICN into a malloc'd BGRA buffer. Returns 0 on any failure.
static unsigned char *oicon_read(const char *name, int *out_w, int *out_h) {
    char path[48]; int l = 0;
    const char *p = "/ICONS/"; while (*p) path[l++] = *p++;
    for (int i = 0; name[i] && i < 12 && l < 40; i++) path[l++] = name[i];
    const char *e = ".ICN"; while (*e) path[l++] = *e++;
    path[l] = 0;
    int fd = open(path, 0);
    if (fd < 0) return 0;
    unsigned char hdr[12];
    if (read(fd, (char *)hdr, 12) != 12 ||
        hdr[0] != 'M' || hdr[1] != 'I' || hdr[2] != 'C' || hdr[3] != 'O') { close(fd); return 0; }
    int w = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);
    int h = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (hdr[11] << 24);
    if (w <= 0 || h <= 0 || w > OICON_SRC_MAX || h > OICON_SRC_MAX) { close(fd); return 0; }
    int want = w * h * 4, got = 0;
    unsigned char *px = (unsigned char *)malloc((size_t)want);
    if (!px) { close(fd); return 0; }
    while (got < want) {
        int n = read(fd, (char *)px + got, want - got);
        if (n <= 0) break;
        got += n;
    }
    close(fd);
    if (got != want) { free(px); return 0; }
    *out_w = w; *out_h = h;
    return px;
}

// Box-average the source's alpha-weighted luminance into a size x size
// coverage map. Integer box bounds: for 64->16 every destination pixel
// averages exactly 4x4 source texels; at 1:1 the box is one texel (exact
// copy); for a non-integer ratio (24->16) boxes are 1 or 2 texels wide, which
// is still an area sample rather than the 1-of-16 nearest pick the older
// per-app loaders do.
static void oicon_sample(const unsigned char *px, int w, int h, int size, unsigned char *cov) {
    for (int dy = 0; dy < size; dy++) {
        int sy0 = (dy * h) / size, sy1 = ((dy + 1) * h) / size;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (int dx = 0; dx < size; dx++) {
            int sx0 = (dx * w) / size, sx1 = ((dx + 1) * w) / size;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            unsigned int sum = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const unsigned char *row = px + (sy * w) * 4;
                for (int sx = sx0; sx < sx1; sx++) {
                    const unsigned char *s = row + sx * 4;
                    unsigned int b = s[0], g = s[1], r = s[2], a = s[3];
                    unsigned int lum = (r * 30 + g * 59 + b * 11) / 100;   // white glyph -> coverage
                    sum += (a * lum) / 255;
                    n++;
                }
            }
            cov[dy * size + dx] = (unsigned char)(n ? sum / n : 0);
        }
    }
}

static oicon_slot_t *oicon_get(const char *name, int size) {
    if (!name || !name[0]) return 0;
    if (size <= 0) size = 16;
    if (size > OICON_MAX_SIZE) size = OICON_MAX_SIZE;
    for (int i = 0; i < g_nslots; i++)
        if (g_slots[i].size == size && oicon_name_eq(g_slots[i].name, name)) return &g_slots[i];
    oicon_slot_t *sl;
    if (g_nslots < OICON_SLOTS) sl = &g_slots[g_nslots++];
    else { sl = &g_slots[g_next_evict]; g_next_evict = (g_next_evict + 1) % OICON_SLOTS; }
    oui_copy(sl->name, sizeof(sl->name), name);
    sl->size = size;
    sl->state = -1;
    int w = 0, h = 0;
    unsigned char *px = oicon_read(name, &w, &h);
    if (!px) return sl;
    oicon_sample(px, w, h, size, sl->cov);
    free(px);
    sl->state = 1;
    return sl;
}

int oicon_draw(int win, const char *name, int x, int y, int size,
               unsigned int ink, unsigned int bg) {
    if (size <= 0) size = 16;
    if (size > OICON_MAX_SIZE) size = OICON_MAX_SIZE;
    oicon_slot_t *sl = oicon_get(name, size);
    if (!sl || sl->state != 1) return 0;
    // Composite over `bg` into an opaque BGRA tile and blit it in one call.
    uint32_t tile[OICON_MAX_SIZE * OICON_MAX_SIZE];
    int ir = (ink >> 16) & 0xFF, ig = (ink >> 8) & 0xFF, ib = ink & 0xFF;
    int br = (bg  >> 16) & 0xFF, bgg = (bg  >> 8) & 0xFF, bb = bg  & 0xFF;
    for (int i = 0; i < size * size; i++) {
        int a = sl->cov[i];
        int r = (ir * a + br  * (255 - a)) / 255;
        int g = (ig * a + bgg * (255 - a)) / 255;
        int b = (ib * a + bb  * (255 - a)) / 255;
        // 0x00RRGGBB, the same byte shape win_draw_rect() stores: the kernel's
        // sys_win_draw_image() copies each u32 into the content buffer raw
        // (kernel/proc/syscall.c, `drow[dx] = ksrc[sx]`), so the alpha byte
        // must match what every other primitive leaves there.
        tile[i] = (uint32_t)((r << 16) | (g << 8) | b);
    }
    win_draw_image(win, x, y, size, size, tile);
    return 1;
}

int oicon_present(const char *name) {
    oicon_slot_t *sl = oicon_get(name, 16);
    return sl && sl->state == 1;
}

void oicon_flush(void) {
    g_nslots = 0;
    g_next_evict = 0;
}
