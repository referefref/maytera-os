// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// places.c - the one table of named locations. See places.h for why it exists.
//
// NO PATH LITERAL APPEARS IN THIS FILE. Every row resolves through the
// userconf.h accessor that owns that directory's on-disk name, which is the
// whole point: the label is here, the name is there, and neither is duplicated.
#include "places.h"
#include "userconf.h"
#include "dirent.h"

// --- resolvers ---------------------------------------------------------------
// Thin, so that the table below reads as data. `userhome_dir(0, ...)` is the
// home itself, which is why it is not one of the named-subdirectory accessors.
static int r_home(char *o, unsigned long c) { return userhome_dir(0, o, c); }

// --- THE TABLE ---------------------------------------------------------------
// Order is the order the UI draws, so it is the order the user reads. Home
// first, then the skeleton in the order users_make_home_skeleton() creates it,
// then the machine-wide rows.
//
// "Media" pointing at /MEDIA, NOT "Music" pointing at /MEDIA/MUSIC. Two rows
// both called Music would be the clearest possible way to hide from the user
// that one is theirs at 0750 and the other is shipped content at root-owned
// 0755 that they cannot write. The label is the only thing that distinguishes
// them on screen, so the label has to do the work, and it also has to FIT: the
// sidebar is 160px wide with the label starting 34px in, so a row is a word or
// two, not a phrase. Pointing at the parent also means a later /MEDIA/VIDEO
// appears with no UI change at all.
static const place_t PLACES[] = {
    { "Home",          "HOME",     PLACE_SECT_USER,   PLACE_WHEN_HOME,    r_home,                  0 },
    { "Desktop",       "FOLDER",   PLACE_SECT_USER,   PLACE_WHEN_HOME,    userhome_desktop_dir,    0 },
    { "Documents",     "FOLDER",   PLACE_SECT_USER,   PLACE_WHEN_HOME,    userhome_documents_dir,  0 },
    { "Downloads",     "FOLDER",   PLACE_SECT_USER,   PLACE_WHEN_HOME,    userhome_downloads_dir,  0 },
    { "Pictures",      "FOLDER",   PLACE_SECT_USER,   PLACE_WHEN_HOME,    userhome_pictures_dir,   0 },
    { "Music",         "FOLDER",   PLACE_SECT_USER,   PLACE_WHEN_HOME,    userhome_music_dir,      0 },
    { "Videos",        "FOLDER",   PLACE_SECT_USER,   PLACE_WHEN_HOME,    userhome_videos_dir,     0 },

    // A bootstrap session (no /CONFIG/PASSWD yet, so no account and no home)
    // gets the one directory it can actually write instead of seven rows into
    // root-owned space. This row REPLACES the block above rather than joining
    // it, which is what PLACE_WHEN_NO_HOME means.
    { "Scratch",       "FOLDER",   PLACE_SECT_USER,   PLACE_WHEN_NO_HOME, userscratch_dir,         0 },

    { "Shared",        "FOLDER",   PLACE_SECT_SYSTEM, PLACE_WHEN_ALWAYS,  shared_dir,              1 },
    { "Media",         "FOLDER",   PLACE_SECT_SYSTEM, PLACE_WHEN_ALWAYS,  media_dir,               1 },
};
#define PLACES_N ((int)(sizeof(PLACES) / sizeof(PLACES[0])))

// --- session-filtered view ---------------------------------------------------
static int  g_ready;
static int  g_have_home;
static int  g_vis[PLACES_N];    // indices into PLACES, in draw order
static int  g_vis_n;

static int dir_exists(const char *p) {
    DIR *d = opendir(p);
    if (!d) return 0;
    closedir(d);
    return 1;
}

void places_init(void) {
    if (g_ready) return;
    g_ready = 1;

    // "Has a home" is asked of userhome_root(), the same lookup every accessor
    // uses, and NOT of getpwuid() here. Files used to call getpwuid() itself and
    // fall back to the literal "/HOME/ADMIN" when the answer was empty or "/",
    // which pointed a session at ANOTHER USER'S HOME whenever it had none of
    // its own. A second implementation of "where is home" is exactly how a
    // fallback like that survives review.
    char home[192];
    g_have_home = (userhome_root(home, sizeof(home)) == 0 &&
                   !(home[0] == '/' && home[1] == '\0'));

    g_vis_n = 0;
    for (int i = 0; i < PLACES_N; i++) {
        const place_t *p = &PLACES[i];
        if (p->when == PLACE_WHEN_HOME    && !g_have_home) continue;
        if (p->when == PLACE_WHEN_NO_HOME &&  g_have_home) continue;
        char buf[256];
        if (!p->resolve || p->resolve(buf, sizeof(buf)) != 0) continue;
        // Probe SYSTEM rows only: see places.h. Done once here, never in a
        // draw path, which is why places_init() is a separate call.
        if (p->probe && !dir_exists(buf)) continue;
        g_vis[g_vis_n++] = i;
    }
}

int places_have_home(void) { places_init(); return g_have_home; }
int places_count(void)     { places_init(); return g_vis_n; }

const place_t *places_at(int i) {
    places_init();
    if (i < 0 || i >= g_vis_n) return 0;
    return &PLACES[g_vis[i]];
}

int places_path(int i, char *out, unsigned long cap) {
    const place_t *p = places_at(i);
    if (!p || !p->resolve || !out || cap == 0) return -1;
    return p->resolve(out, cap);
}

const char *places_section_label(place_sect_t s) {
    switch (s) {
        case PLACE_SECT_USER:   return "Quick access";
        case PLACE_SECT_SYSTEM: return "This system";
        default:                return "";
    }
}

void places_default_dir(char *out, unsigned long cap) {
    if (!out || cap == 0) return;
    out[0] = '/'; out[1] = '\0';
    places_init();
    // Documents when there is a home, scratch when there is not. Deliberately
    // NOT the home itself: a chooser that opens on a directory of directories
    // makes the user navigate before they can see a single document.
    if (g_have_home) {
        char b[256];
        if (userhome_documents_dir(b, sizeof(b)) == 0 && dir_exists(b)) {
            unsigned long n = 0; while (b[n]) n++;
            if (n + 1 <= cap) { for (unsigned long i = 0; i <= n; i++) out[i] = b[i]; return; }
        }
        if (userhome_dir(0, b, sizeof(b)) == 0) {
            unsigned long n = 0; while (b[n]) n++;
            if (n + 1 <= cap) { for (unsigned long i = 0; i <= n; i++) out[i] = b[i]; return; }
        }
        return;
    }
    char b[256];
    if (userscratch_dir(b, sizeof(b)) == 0 && dir_exists(b)) {
        unsigned long n = 0; while (b[n]) n++;
        if (n + 1 <= cap) for (unsigned long i = 0; i <= n; i++) out[i] = b[i];
    }
}
