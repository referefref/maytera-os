// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// places.h - THE NAMED LOCATIONS a session can jump to, and the ONE table that
// says what they are called and where they resolve to.
//
// WHY THIS FILE EXISTS. Before it, the user folder layout was named in three
// unrelated places that could not all be right at once:
//
//   1. kernel/proc/users.c users_make_home_skeleton() CREATES the directories.
//   2. userland/apps/files/main.c carried `qa_label[]` (what the user reads) and
//      `qa_dir[]` (the on-disk name) as a pair of parallel arrays, and the two
//      DISAGREED: the label said "Documents" while the directory it opened was
//      DOCUMENT. A reader of either array alone could not tell.
//   3. userland/officelib/officeui/officeui.c's file chooser named NONE of them.
//      Its start directory was `od_start_dir()`, which tried "/DOCS" (a
//      directory that does not exist on any shipping image) and otherwise
//      opened "/". So the chooser in Writer, Calc and Slides did not know the
//      user had a home, let alone a Documents folder in it.
//
// So: the LABEL lives here, exactly once. The on-disk NAME lives in
// userconf.c, exactly once, behind the accessor this table calls. Nothing in a
// UI assembles a path out of a home and a literal ever again.
//
// This is a MODEL, not a widget. It draws nothing and does no layout, so the
// Files sidebar (its own glass palette) and the shared file chooser (theme
// tokens) can each render it in their own grammar while agreeing completely
// about what the places ARE. See docs/UI_STYLE_GUIDE.md.
#ifndef _PLACES_H
#define _PLACES_H

// Which heading a place sits under. The UI draws the heading; this only says
// which group a row belongs to, so the two consumers cannot disagree about
// whether /SHARED is a personal folder (it is not).
typedef enum {
    PLACE_SECT_USER   = 0,   // this session's own folders, under its home
    PLACE_SECT_SYSTEM = 1,   // machine-wide: /SHARED, /MEDIA, /TMP
    PLACE_SECT__COUNT = 2
} place_sect_t;

// When a row is offered at all.
typedef enum {
    PLACE_WHEN_ALWAYS  = 0,
    PLACE_WHEN_HOME    = 1,  // only when the session HAS a real home
    PLACE_WHEN_NO_HOME = 2   // only when it does NOT (see places_have_home)
} place_when_t;

typedef struct {
    const char *label;    // what the user reads. The ONLY copy of it.
    const char *icon;     // icon-font name for the shared icon helper, never a
                          // bespoke glyph. A consumer that cannot resolve it
                          // falls back to its own generic folder icon.
    place_sect_t sect;
    place_when_t when;
    // Resolves the absolute path. Delegates to the userconf.h accessor that
    // OWNS the directory's on-disk name, which is why no name literal appears
    // in this file. Returns 0 on success, -1 if it will not fit or does not
    // apply to this session.
    int (*resolve)(char *out, unsigned long cap);
    // Probe the directory at init and drop the row if it is absent. Set for
    // SYSTEM places only: a user place is guaranteed by
    // users_make_home_skeleton(), which runs on every login, whereas /MEDIA
    // ships as content and may simply not be on a given image. A row that
    // navigates nowhere is worse than no row.
    unsigned char probe;
} place_t;

// Build the list for THIS session. Idempotent, and it does a small amount of
// I/O (one opendir per probed row), so call it from startup and NOT from a
// draw path. Every other function here calls it if the caller forgot, because
// a silently empty sidebar is a worse failure than one late opendir.
void places_init(void);

// 0 for a pre-account / bootstrap session, whose home resolves to "/". On such
// a session every <home>/X row would point at a root-owned directory it cannot
// write, so the USER section is replaced by the scratch and shared rows rather
// than shown pointing somewhere misleading.
int places_have_home(void);

int places_count(void);
const place_t *places_at(int i);                          // 0 if i is out of range
int places_path(int i, char *out, unsigned long cap);      // 0 on success, -1 otherwise
const char *places_section_label(place_sect_t s);          // "Quick access" / "This system"

// Where a file chooser should OPEN. The session's Documents folder when there
// is a home, its scratch directory when there is not, and "/" only if even
// that cannot be resolved. Always writes something NUL-terminated.
void places_default_dir(char *out, unsigned long cap);

#endif
