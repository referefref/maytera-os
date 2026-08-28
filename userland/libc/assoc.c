// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// assoc.c - OS-wide userland file associations (#84). See assoc.h.
#include "assoc.h"
#include "syscall.h"
#include "string.h"
#include "unistd.h"   // access() - see assoc_app_for()

#define ASSOC_PATH "/ASSOC.CFG"

static void lc(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

static void ext_of(const char *name, char *e, int esz) {
    int dot = -1;
    for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    int k = 0;
    if (dot >= 0) for (int i = dot + 1; name[i] && k < esz - 1; i++) e[k++] = name[i];
    e[k] = 0;
    lc(e);
}

static void copy_str(char *out, int outsz, const char *s) {
    int k = 0;
    for (; s[k] && k < outsz - 1; k++) out[k] = s[k];
    out[k] = 0;
}

// Built-in defaults used when /ASSOC.CFG has no entry for an extension.
typedef struct { const char *ext, *app; } defmap_t;
static const defmap_t DEFAULTS[] = {
    {"txt","/APPS/editor"}, {"md","/APPS/editor"},  {"c","/APPS/editor"},   {"h","/APPS/editor"},
    {"cfg","/APPS/editor"}, {"log","/APPS/editor"}, {"ini","/APPS/editor"}, {"csv","/APPS/editor"},
    {"yml","/APPS/editor"}, {"sh","/APPS/editor"},  {"cpp","/APPS/editor"},
    {"bmp","/APPS/IMAGEVIEWER"},{"png","/APPS/IMAGEVIEWER"},{"jpg","/APPS/IMAGEVIEWER"},{"jpeg","/APPS/IMAGEVIEWER"},
    {"gif","/APPS/IMAGEVIEWER"},{"ico","/APPS/IMAGEVIEWER"},
    {"wav","/APPS/musicplr"},{"mp3","/APPS/musicplr"},{"ogg","/APPS/musicplr"},{"flac","/APPS/musicplr"},
    {"m4a","/APPS/musicplr"},{"aac","/APPS/musicplr"},{"opus","/APPS/musicplr"},
    {"mp4","/APPS/MEDIAPLAYER"},{"avi","/APPS/MEDIAPLAYER"},{"mkv","/APPS/MEDIAPLAYER"},{"mov","/APPS/MEDIAPLAYER"},
    {"htm","/APPS/browser"},{"html","/APPS/browser"},
};
#define NDEF ((int)(sizeof(DEFAULTS) / sizeof(DEFAULTS[0])))

static int read_cfg(char *buf, int bufsz) {
    int fd = sys_open(ASSOC_PATH, 0);
    if (fd < 0) return 0;
    int n = (int)sys_read(fd, buf, bufsz - 1);
    sys_close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

// Look up "ext=" in /ASSOC.CFG, copy value to out. Returns 1 if found.
static int cfg_lookup(const char *ext, char *out, int outsz) {
    char buf[1024];
    int n = read_cfg(buf, sizeof(buf));
    if (!n) return 0;
    char *p = buf;
    while (*p) {
        char *eol = p; while (*eol && *eol != '\n') eol++;
        char *eq = p;  while (eq < eol && *eq != '=') eq++;
        if (eq < eol) {
            char key[16]; int kk = 0;
            for (char *c = p; c < eq && kk < 15; c++) key[kk++] = *c;
            key[kk] = 0; lc(key);
            if (!strcmp(key, ext)) {
                int vk = 0;
                for (char *v = eq + 1; v < eol && vk < outsz - 1; v++) out[vk++] = *v;
                // Trim trailing CR/space. A file written on a machine that ends
                // lines with CRLF would otherwise yield "/APPS/X\r", which no
                // lookup can resolve and which the existence check below would
                // then reject for the wrong reason.
                while (vk > 0 && (out[vk-1] == '\r' || out[vk-1] == ' ' || out[vk-1] == '\t')) vk--;
                out[vk] = 0;
                return 1;
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return 0;
}

// A CONFIG ENTRY NAMING A FILE THAT IS NOT THERE IS NOT AN ANSWER (#<config-ref>).
//
// This function used to consult /ASSOC.CFG first and return whatever it said,
// unconditionally. The shipped /ASSOC.CFG mapped every image extension to
// /APPS/IMGVIEW, a binary that has never existed in any build; the correct
// handler, /APPS/IMAGEVIEWER, was sitting right there in DEFAULTS[] below and
// could never be reached, because the wrong file beat the right compiled-in
// answer. Double-clicking any image on any golden gave "Cannot open file".
//
// The shipped file is fixed (build/assets/ASSOC.CFG) and a build gate now
// refuses to ship a config naming a path that is not on the image
// (build/config-ref-gate.sh), but neither of those helps a machine whose
// /ASSOC.CFG was written at RUNTIME - by Settings > Default Apps, or by hand,
// or by an app that has since been uninstalled. So the rule is here, in the
// resolver, where it holds for every source of the file: a config entry is
// preferred over the built-in default only if the app it names EXISTS. If it
// does not, we fall through exactly as if the entry were absent, and the user
// gets a working open instead of an error.
//
// access() is stat(), so this costs one stat per double-click, and only on the
// extensions that have a config line at all. The alternative price is the one
// this project already paid: a broken open that looks like a broken file.
const char *assoc_app_for(const char *filename, char *out, int outsz) {
    char e[8]; ext_of(filename, e, sizeof(e));
    if (e[0] && cfg_lookup(e, out, outsz) && out[0] && access(out, 0) == 0) return out;
    for (int i = 0; i < NDEF; i++)
        if (!strcmp(e, DEFAULTS[i].ext)) { copy_str(out, outsz, DEFAULTS[i].app); return out; }
    copy_str(out, outsz, "/APPS/editor");
    return out;
}

int assoc_set_default(const char *ext, const char *apppath) {
    char e[16]; int k = 0;
    for (int i = 0; ext[i] && k < 15; i++) e[k++] = ext[i];
    e[k] = 0; lc(e);

    char buf[1024]; int n = read_cfg(buf, sizeof(buf));
    char out[1280]; int o = 0;
    char *p = buf;
    while (p < buf + n && *p) {
        char *eol = p; while (*eol && *eol != '\n') eol++;
        char *eq = p;  while (eq < eol && *eq != '=') eq++;
        int keep = 1;
        if (eq < eol) {
            char key[16]; int kk = 0;
            for (char *c = p; c < eq && kk < 15; c++) key[kk++] = *c;
            key[kk] = 0; lc(key);
            if (!strcmp(key, e)) keep = 0;     // drop the old mapping for this ext
        }
        if (keep) {
            for (char *c = p; c < eol && o < (int)sizeof(out) - 1; c++) out[o++] = *c;
            if (o < (int)sizeof(out) - 1) out[o++] = '\n';
        }
        p = (*eol) ? eol + 1 : eol;
    }
    for (const char *s = e;       *s && o < (int)sizeof(out) - 1; s++) out[o++] = *s;
    if (o < (int)sizeof(out) - 1) out[o++] = '=';
    for (const char *s = apppath; *s && o < (int)sizeof(out) - 1; s++) out[o++] = *s;
    if (o < (int)sizeof(out) - 1) out[o++] = '\n';
    out[o] = 0;

    sys_unlink(ASSOC_PATH);
    int fd = sys_open(ASSOC_PATH, 0x41);   // O_WRONLY | O_CREAT
    if (fd < 0) return -1;
    sys_write(fd, out, (unsigned long)o);
    sys_close(fd);
    return 0;
}

static const char *TEXT_APPS[] = {"/APPS/editor", "/APPS/mvi"};
static const char *IMG_APPS[]  = {"/APPS/IMAGEVIEWER", "/APPS/paint"};
static const char *AUD_APPS[]  = {"/APPS/musicplr", "/APPS/MEDIAPLAYER"};
static const char *VID_APPS[]  = {"/APPS/MEDIAPLAYER"};
static const char *WEB_APPS[]  = {"/APPS/browser", "/APPS/editor"};
static const assoc_category_t CATS[] = {
    {"Documents", "txt md c h cfg log ini csv yml sh cpp", TEXT_APPS, 2},
    {"Images",    "bmp png jpg jpeg gif ico",             IMG_APPS,  2},
    {"Audio",     "wav mp3 ogg flac m4a aac opus",        AUD_APPS,  2},
    {"Video",     "mp4 avi mkv mov",                      VID_APPS,  1},
    {"Web pages", "htm html",                             WEB_APPS,  2},
};
#define NCAT ((int)(sizeof(CATS) / sizeof(CATS[0])))

const assoc_category_t *assoc_categories(int *count) { if (count) *count = NCAT; return CATS; }

const char *assoc_category_current(int idx, char *out, int outsz) {
    if (idx < 0 || idx >= NCAT) { out[0] = 0; return out; }
    char ext[16]; int k = 0;
    for (const char *e = CATS[idx].exts; *e && *e != ' ' && k < 15; e++) ext[k++] = *e;
    ext[k] = 0;
    char fn[24]; int f = 0; fn[f++] = 'x'; fn[f++] = '.';
    for (int i = 0; ext[i] && f < 23; i++) fn[f++] = ext[i];
    fn[f] = 0;
    return assoc_app_for(fn, out, outsz);
}
