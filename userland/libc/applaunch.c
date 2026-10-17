// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// applaunch.c - THE launch registry. See applaunch.h for why it lives here.
//
// MOVED VERBATIM out of libc/aiclient.c (#292's app.launch executor) when Maytera
// Flow's `launch` node became the second caller. The parsing, the slugging, the
// gui/win16/dos dispatch and the /ROMS resolver are the same code, now reachable.
// aiclient.c keeps ONLY the model-facing prose it builds from this registry.
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "fcntl.h"
#include "applaunch.h"

#define SM_LINE_MAX 1024   // matches startmenu.c SM_FRAG_LINE bound

static applaunch_ent_t g_launch[APPLAUNCH_MAX];
static int g_launch_n = 0;
static int g_loaded   = 0;

// case-insensitive whole-string match (a caller may say "DOOM" / "SkiFree")
static int id_eq_ci(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

// Slugify: lowercase, keep [a-z0-9] only. "The Incredible Machine" -> theincrediblemachine
static void slugify_ascii(const char *src, char *dst, int cap) {
    int o = 0;
    for (const char *s = src; *s && o < cap - 1; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) dst[o++] = c;
    }
    dst[o] = 0;
}

// First whitespace-delimited token of an exec line (the bare binary path).
static void first_token(const char *path, char *dst, int cap) {
    int o = 0;
    for (const char *s = path; *s && *s != ' ' && *s != '\t' && o < cap - 1; s++) dst[o++] = *s;
    dst[o] = 0;
}

// Slug of the exec basename: first token, final path component, strip a trailing
// .EXT, then slugify. "/GAMES/DOOM/DOOM.ELF" -> doom
static void basename_slug(const char *path, char *dst, int cap) {
    char tok[APPLAUNCH_PATH_MAX];
    first_token(path, tok, sizeof(tok));
    const char *base = tok;
    for (const char *p = tok; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    char stem[APPLAUNCH_PATH_MAX];
    int so = 0;
    for (const char *p = base; *p && so < (int)sizeof(stem) - 1; p++) stem[so++] = *p;
    stem[so] = 0;
    for (int k = so - 1; k > 0; k--) { if (stem[k] == '.') { stem[k] = 0; break; } }
    slugify_ascii(stem, dst, cap);
}

// Parse ONE manifest line. Only `item:` lines contribute a launchable app; we
// ignore category:/rename:/hide: (the registry is the union of launchable apps).
// Last-write-wins by primary id, so a later fragment overriding an app updates
// the same entry rather than duplicating it.
static void launch_parse_line(const char *line) {
    int p = 0; while (line[p] == ' ' || line[p] == '\t') p++;
    if (strncmp(line + p, "item:", 5) != 0) return;
    const char *rest = line + p + 5;

    char name[APPLAUNCH_NAME_MAX], exec[APPLAUNCH_PATH_MAX];
    int kind = APPLAUNCH_GUI;

    const char *f = rest;
    int fi = 0;
    name[0] = 0; exec[0] = 0;
    while (fi < 8) {
        const char *bar = f;
        while (*bar && *bar != '|') bar++;
        const char *a = f; const char *b = bar;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
        int len = (int)(b - a);
        if (fi == 0) {
            if (len > APPLAUNCH_NAME_MAX - 1) len = APPLAUNCH_NAME_MAX - 1;
            memcpy(name, a, (size_t)len); name[len] = 0;
        } else if (fi == 1) {
            if (len > APPLAUNCH_PATH_MAX - 1) len = APPLAUNCH_PATH_MAX - 1;
            memcpy(exec, a, (size_t)len); exec[len] = 0;
        } else {
            if (len >= 5 && !strncmp(a, "type=", 5)) {
                const char *v = a + 5;
                if      (!strncmp(v, "win16", 5)) kind = APPLAUNCH_WIN16;
                else if (!strncmp(v, "dos", 3))   kind = APPLAUNCH_DOS;
                else                              kind = APPLAUNCH_GUI;
            }
        }
        fi++;
        if (!*bar) break;
        f = bar + 1;
    }
    if (!name[0] || !exec[0]) return;

    char id[APPLAUNCH_ID_MAX], alt[APPLAUNCH_ID_MAX];
    slugify_ascii(name, id, sizeof(id));
    basename_slug(exec, alt, sizeof(alt));
    if (!id[0]) { if (!alt[0]) return; strlcpy(id, alt, sizeof(id)); }
    if (id_eq_ci(id, alt)) alt[0] = 0;   // no distinct alternate

    applaunch_ent_t *e = 0;
    for (int i = 0; i < g_launch_n; i++)
        if (id_eq_ci(g_launch[i].id, id)) { e = &g_launch[i]; break; }
    if (!e) {
        if (g_launch_n >= APPLAUNCH_MAX) return;
        e = &g_launch[g_launch_n++];
    }
    strlcpy(e->id, id, sizeof(e->id));
    strlcpy(e->alt, alt, sizeof(e->alt));
    strlcpy(e->path, exec, sizeof(e->path));
    strlcpy(e->name, name, sizeof(e->name));
    e->kind = kind;
}

// Feed one .MENU file, streamed a line at a time (no whole-file buffer, so no
// size limit to outgrow: the games fragment is ~29KB). Mirrors sm_feed_file().
static void launch_parse_fd(int fd) {
    static char chunk[2048];
    char line[SM_LINE_MAX]; int lp = 0;
    long n;
    while ((n = sys_read(fd, chunk, sizeof(chunk))) > 0) {
        for (long k = 0; k < n; k++) {
            char c = chunk[k];
            if (c == '\n' || c == '\r') {
                line[lp] = 0;
                if (lp) launch_parse_line(line);
                lp = 0;
            } else if (lp < SM_LINE_MAX - 1) {
                line[lp++] = c;
            }
        }
    }
    if (lp) { line[lp] = 0; launch_parse_line(line); }
}

// Scan one SYSTEM.D directory for *.MENU fragments and parse each.
static void launch_scan_dir(const char *dir) {
    int dfd = sys_open(dir, 0);
    if (dfd < 0) return;
    dirent_t e;
    int guard = 0;
    while (guard++ < 256) {
        int r = (int)syscall2(SYS_READDIR, dfd, (long)&e);
        if (r != 0 || e.name[0] == 0) break;
        int l = 0; while (e.name[l]) l++;
        if (l < 6) continue;
        // ".MENU" suffix, case-tolerant (FAT 8.3 exposes uppercase)
        const char *suf = e.name + l - 5;
        if (!(suf[0]=='.' &&
              (suf[1]=='M'||suf[1]=='m') && (suf[2]=='E'||suf[2]=='e') &&
              (suf[3]=='N'||suf[3]=='n') && (suf[4]=='U'||suf[4]=='u'))) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, e.name);
        int ffd = sys_open(path, O_RDONLY);
        if (ffd < 0) continue;
        launch_parse_fd(ffd);
        sys_close(ffd);
    }
    sys_close(dfd);
}

// Tiny hardcoded FALLBACK, used ONLY when the manifest is unreadable (no
// SYSTEM.D on this image). Not the source of truth: it exists so a caller is not
// entirely unable to launch anything on a broken image, and is deliberately
// minimal. The real list is the .MENU manifest above.
static void launch_seed_fallback(void) {
    static const struct { const char *nm, *pt; int k; } fb[] = {
        { "Files",     "/APPS/FILES",          APPLAUNCH_GUI },
        { "Terminal",  "/APPS/TERMINAL",       APPLAUNCH_GUI },
        { "Settings",  "/APPS/SETTINGS",       APPLAUNCH_GUI },
        { "Browser",   "/APPS/BROWSER",        APPLAUNCH_GUI },
        { "Calculator","/APPS/CALC",           APPLAUNCH_GUI },
        { "DOOM",      "/GAMES/DOOM/DOOM.ELF", APPLAUNCH_GUI },
        { "Game Boy",  "/APPS/gbemu",          APPLAUNCH_GUI },
    };
    for (unsigned i = 0; i < sizeof(fb)/sizeof(fb[0]) && g_launch_n < APPLAUNCH_MAX; i++) {
        applaunch_ent_t *e = &g_launch[g_launch_n++];
        slugify_ascii(fb[i].nm, e->id, sizeof(e->id));
        basename_slug(fb[i].pt, e->alt, sizeof(e->alt));
        if (id_eq_ci(e->id, e->alt)) e->alt[0] = 0;
        strlcpy(e->path, fb[i].pt, sizeof(e->path));
        strlcpy(e->name, fb[i].nm, sizeof(e->name));
        e->kind = fb[i].k;
    }
}

int applaunch_load(void) {
    g_launch_n = 0;
    g_loaded = 1;
    // The same directories the compositor's sm_feed_system_layer() feeds.
    launch_scan_dir("/CONFIG/STARTMENU/SYSTEM.D");
    launch_scan_dir("/ext2/CONFIG/STARTMENU/SYSTEM.D");
    if (g_launch_n == 0) launch_seed_fallback();
    return g_launch_n;
}

static void ensure_loaded(void) { if (!g_loaded) applaunch_load(); }

int applaunch_count(void) { ensure_loaded(); return g_launch_n; }

const applaunch_ent_t *applaunch_at(int i) {
    ensure_loaded();
    if (i < 0 || i >= g_launch_n) return 0;
    return &g_launch[i];
}

const applaunch_ent_t *applaunch_find(const char *id) {
    if (!id || !id[0]) return 0;
    ensure_loaded();
    for (int i = 0; i < g_launch_n; i++)
        if (id_eq_ci(g_launch[i].id, id) ||
            (g_launch[i].alt[0] && id_eq_ci(g_launch[i].alt, id)))
            return &g_launch[i];
    return 0;
}

void applaunch_bin(const applaunch_ent_t *e, char *out, int cap) {
    if (cap > 0) out[0] = 0;
    if (!e) return;
    first_token(e->path, out, cap);
}

// case-insensitive substring test (needle within haystack).
static int substr_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 0;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        for (;;) {
            char cb = *b;
            if (!cb) return 1;
            char ca = *a;
            if (!ca) break;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
    }
    return 0;
}

int applaunch_resolve_rom(const char *query, char *out, int cap) {
    if (cap > 0) out[0] = 0;
    if (!query || !query[0]) return 0;
    int dfd = sys_open("/ROMS", 0);
    if (dfd < 0) return 0;
    dirent_t e;
    int hit = 0, guard = 0;
    while (guard++ < 1024) {
        int r = (int)syscall2(SYS_READDIR, dfd, (long)&e);
        if (r != 0 || e.name[0] == 0) break;
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        if (DIRENT_IS_DIR(e)) continue;
        if (substr_ci(e.name, query)) {
            snprintf(out, cap, "/ROMS/%s", e.name);
            hit = 1;
            break;
        }
    }
    sys_close(dfd);
    return hit;
}

int applaunch_run(const char *id, const char *rom_query, const char *arg,
                  applaunch_result_t *res) {
    applaunch_result_t local;
    if (!res) res = &local;
    memset(res, 0, sizeof(*res));

    const applaunch_ent_t *e = applaunch_find(id);
    if (!e) return APPLAUNCH_ERR_UNKNOWN;

    strlcpy(res->id,   e->id,   sizeof(res->id));
    strlcpy(res->name, e->name, sizeof(res->name));
    res->kind = e->kind;

    if (e->kind == APPLAUNCH_WIN16) {
        strlcpy(res->path, e->path, sizeof(res->path));
        res->rc = win16_run(e->path);
        return res->rc < 0 ? APPLAUNCH_ERR_SPAWN : 0;
    }
    if (e->kind == APPLAUNCH_DOS) {
        strlcpy(res->path, e->path, sizeof(res->path));
        res->rc = dos_run(e->path);
        return res->rc < 0 ? APPLAUNCH_ERR_SPAWN : 0;
    }

    // NATIVE. The exec line's first token is the binary; a rom or a verbatim arg
    // becomes argv[1]. A rom query that matches nothing launches the app BARE so
    // it shows its own picker, which is a reported outcome and not a failure.
    char bin[APPLAUNCH_PATH_MAX];
    first_token(e->path, bin, sizeof(bin));
    strlcpy(res->path, bin, sizeof(res->path));

    res->rom_requested = (rom_query && rom_query[0]) ? 1 : 0;
    if (res->rom_requested &&
        applaunch_resolve_rom(rom_query, res->rom, sizeof(res->rom))) {
        res->rom_matched = 1;
        char *argv[2] = { bin, res->rom };
        res->rc = sys_spawn_args(bin, argv, 2);
    } else if (!res->rom_requested && arg && arg[0]) {
        char a[APPLAUNCH_PATH_MAX];
        strlcpy(a, arg, sizeof(a));
        char *argv[2] = { bin, a };
        res->rc = sys_spawn_args(bin, argv, 2);
    } else {
        res->rc = sys_spawn(bin);
    }
    return res->rc < 0 ? APPLAUNCH_ERR_SPAWN : 0;
}
