// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_palette.c - file-based TERMINAL COLOUR SCHEME loader. See gui_palette.h
// for why this is a separate concept from gui_theme.c (OS theme).
#include "gui_palette.h"
#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define PALETTE_INDEX_PATH  "/PALETTES/INDEX.TXT"
#define PALETTE_DIR_PREFIX  "/PALETTES/"

// The built-in fallback: "MayteraOS Classic". Byte-identical to the ANSI 16
// this terminal always drew before colour schemes existed (see main.c's old
// `static const uint32_t ansi_colors[16]`), so selecting it, or falling back
// to it on any load failure, changes nothing a user was already looking at.
static const term_palette_t PALETTE_CLASSIC_FALLBACK = {
    .ansi = {
        0x00000000, 0x00AA0000, 0x0000AA00, 0x00AAAA00,
        0x000000AA, 0x00AA00AA, 0x0000AAAA, 0x00AAAAAA,
        0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55,
        0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF,
    },
    .fg = 0x00AAAAAA, .bg = 0x00000000, .cursor = 0x00AAAAAA,
    .selection_bg = 0x002563C9, .selection_fg = 0x00FFFFFF,
};

static int gp_read_whole(const char *path, char *buf, int cap) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    long n = sys_read(fd, buf, (unsigned long)(cap - 1));
    sys_close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static void gp_slug_from_filename(const char *fname, char *slug, int cap) {
    int len = 0;
    while (fname[len]) len++;
    int slen = len;
    if (len > 9 && strcmp(fname + len - 9, ".tpalette") == 0) slen = len - 9;
    if (slen > cap - 1) slen = cap - 1;
    int i = 0;
    for (; i < slen; i++) slug[i] = fname[i];
    slug[i] = 0;
}

static void gp_peek_name(const char *path, gui_palette_entry_t *e) {
    static char fb[2048];
    int fn = gp_read_whole(path, fb, sizeof(fb));
    if (fn <= 0) return;
    int q = 0;
    while (q < fn) {
        char line[80];
        int ll = 0;
        while (q < fn && fb[q] != '\n' && ll < (int)sizeof(line) - 1) { line[ll++] = fb[q]; q++; }
        if (q < fn && fb[q] == '\n') q++;
        if (ll > 0 && line[ll - 1] == '\r') ll--;
        line[ll] = 0;
        if (strncmp(line, "name=", 5) == 0) {
            int k = 0;
            const char *v = line + 5;
            while (v[k] && k < GUI_PALETTE_NAME_MAX - 1) { e->name[k] = v[k]; k++; }
            e->name[k] = 0;
            return;   // name= is enough for a picker entry
        }
    }
}

int gui_palette_list(gui_palette_entry_t *out, int max) {
    static char idx[4096];
    int n = gp_read_whole(PALETTE_INDEX_PATH, idx, sizeof(idx));
    if (n <= 0 || max <= 0) return 0;

    int count = 0;
    int p = 0;
    while (p < n && count < max) {
        char fname[64];
        int fl = 0;
        while (p < n && idx[p] != '\n' && fl < (int)sizeof(fname) - 1) { fname[fl++] = idx[p]; p++; }
        if (p < n && idx[p] == '\n') p++;
        if (fl > 0 && fname[fl - 1] == '\r') fl--;
        fname[fl] = 0;
        if (fl == 0) continue;

        gui_palette_entry_t *e = &out[count];
        gp_slug_from_filename(fname, e->slug, GUI_PALETTE_SLUG_MAX);
        e->name[0] = 0;
        e->index = count;

        char path[96];
        snprintf(path, sizeof(path), "%s%s", PALETTE_DIR_PREFIX, fname);
        gp_peek_name(path, e);

        if (e->name[0] == 0) {
            int k = 0;
            while (e->slug[k] && k < GUI_PALETTE_NAME_MAX - 1) { e->name[k] = e->slug[k]; k++; }
            e->name[k] = 0;
        }
        count++;
    }
    return count;
}

// Parse a single "key=0xAARRGGBB"-shaped line's value as hex. Accepts a
// leading "0x"/"0X" (every shipped .tpalette writes it; tolerated if absent).
static uint32_t gp_parse_color(const char *v) {
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
    return (uint32_t)strtoul(v, 0, 16);
}

int gui_palette_load(const char *slug, term_palette_t *out) {
    *out = PALETTE_CLASSIC_FALLBACK;
    if (!slug || !slug[0] || strcmp(slug, GUI_PALETTE_SYSTEM_SLUG) == 0) return -1;

    char path[96];
    snprintf(path, sizeof(path), "%s%s.tpalette", PALETTE_DIR_PREFIX, slug);
    static char buf[2048];
    int n = gp_read_whole(path, buf, sizeof(buf));
    if (n <= 0) return -1;

    int q = 0;
    while (q < n) {
        char line[96];
        int ll = 0;
        while (q < n && buf[q] != '\n' && ll < (int)sizeof(line) - 1) { line[ll++] = buf[q]; q++; }
        if (q < n && buf[q] == '\n') q++;
        if (ll > 0 && line[ll - 1] == '\r') ll--;
        line[ll] = 0;
        if (ll == 0 || line[0] == '#') continue;

        char *eq = 0;
        for (int i = 0; i < ll; i++) if (line[i] == '=') { eq = &line[i]; break; }
        if (!eq) continue;
        *eq = 0;
        const char *key = line;
        const char *val = eq + 1;

        if (strncmp(key, "ansi", 4) == 0 && key[4] >= '0' && key[4] <= '9') {
            int idx = atoi(key + 4);
            if (idx >= 0 && idx < 16) out->ansi[idx] = gp_parse_color(val);
        } else if (strcmp(key, "fg") == 0) {
            out->fg = gp_parse_color(val);
        } else if (strcmp(key, "bg") == 0) {
            out->bg = gp_parse_color(val);
        } else if (strcmp(key, "cursor") == 0) {
            out->cursor = gp_parse_color(val);
        } else if (strcmp(key, "selection_bg") == 0) {
            out->selection_bg = gp_parse_color(val);
        } else if (strcmp(key, "selection_fg") == 0) {
            out->selection_fg = gp_parse_color(val);
        }
        // "name="/"author=" are display metadata, already consumed by
        // gui_palette_list()'s peek; ignored here.
    }
    return 0;
}
