// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// wallpapers.c - shared wallpaper enumeration (#517). See wallpapers.h.
//
// Scans "/" for *.BMP via SYS_READDIR (the kernel's deterministic directory order),
// maps each filename to a friendly display name, and appends a gradient entry. Both
// the compositor picker and the Settings Appearance grid call this, so their shared
// integer index can never reference an absent file or diverge from each other.
#include "wallpapers.h"
#include "syscall.h"

// --- tiny self-contained string helpers (freestanding, no libc dependency) -----
static int wp_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static char wp_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static char wp_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int wp_ieq(const char *a, const char *b) {
    while (*a && *b) { if (wp_upper(*a) != wp_upper(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

// copy at most cap-1 chars of s into d, NUL-terminate.
static void wp_ncpy(char *d, const char *s, int cap) {
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

// true if name ends in ".BMP" (case-insensitive).
static int wp_is_bmp(const char *n) {
    int L = wp_len(n);
    if (L < 5) return 0;   // at least "x.BMP"
    const char *e = n + L - 4;
    return e[0] == '.' && wp_upper(e[1]) == 'B' && wp_upper(e[2]) == 'M'
        && wp_upper(e[3]) == 'P';
}

// non-wallpaper BMPs that also live at the root: boot/studio splash screens.
static int wp_blocked(const char *n) {
    static const char *bl[] = { "BOOT.BMP", "STUDIO.BMP", 0 };
    for (int i = 0; bl[i]; i++) if (wp_ieq(n, bl[i])) return 1;
    return 0;
}

// Append the decimal form of n (0..99) to *p (advancing it). Small helper so we
// avoid pulling in printf just to render "Mountain 15".
static void wp_append_num(char **p, const char *stem, int off) {
    // stem+off points at the trailing digits; copy them verbatim but strip a
    // single leading zero so "01" reads as "1".
    const char *d = stem + off;
    if (d[0] == '0' && d[1]) d++;      // drop one leading zero
    while (*d) { *(*p)++ = *d++; }
}

// Derive a friendly display name from a BMP filename into out (WP_NAME_MAX).
static void wp_pretty(const char *file, char *out) {
    // Tier 1 used to be an exact-match curated table for the originally-shipped
    // set ("MAYTERA.BMP" -> "Maytera Modern", "BACK.BMP" -> "Default Blue", ...).
    // REMOVED 2026-08-11 (#745, second pass): the 2026-08-11 wallpaper-library
    // reset deleted every file those eight entries named, and the compositor
    // binary still carried the eight old filenames as string literals from this
    // very table, which is what tripped `build/invariant-gate.sh`'s "every
    // wallpaper the compositor references exists on the image" check - the
    // COMPILED CODE, not any data file, was the thing still pointing at removed
    // files. See blame.md, "checking the data and not the code" for the general
    // lesson.
    //
    // Not repointing the table at the new 8.3-ish names: the tier-3 fallback
    // below (fixed the same day - see the blame.md entry on wp_pretty's
    // title-casing) already reproduces every display name the table used to
    // curate, from the CURRENT filenames alone, because gen-wallpapers.sh names
    // the modern equivalents word-for-word: "MAYTERA_MODERN.BMP" -> "Maytera
    // Modern", "MAYTERA_CYBER.BMP" -> "Maytera Cyber", "MAYTERA_DESERT.BMP" ->
    // "Maytera Desert", "MOUNTAIN_MAYTERA.BMP" -> "Mountain Maytera",
    // "MOUNTAIN_MAYTERA_2.BMP" -> "Mountain Maytera 2" (verified with a
    // standalone harness against this exact function). "Maytera Green" and
    // "Maytera Mark" have no file in the current set to curate, and "Default
    // Blue" named a swirl/starfield brand wallpaper the user's curated set does
    // not include at all (see the default-wallpaper fix in wallpaper.c and
    // kernel/gui/{login,desktop}.c for what replaced it as the default). A
    // curated table is one more thing that can silently go stale exactly like
    // this one did; the fallback needs no maintenance to stay correct as
    // wallpapers come and go.
    //
    // stem = filename without the ".BMP" extension.
    char stem[WP_FILE_MAX];
    int L = wp_len(file);
    int slen = (L >= 4) ? L - 4 : L;
    if (slen > WP_FILE_MAX - 1) slen = WP_FILE_MAX - 1;
    for (int i = 0; i < slen; i++) stem[i] = wp_upper(file[i]);
    stem[slen] = 0;

    // 1. category prefixes with a trailing number: EBERG -> Mountain, etc.
    static const struct { const char *pfx; const char *label; } prefix[] = {
        { "EBERG", "Mountain" },
        { "OCEAN", "Ocean" },
        { "MACRO", "Macro" },
        { 0, 0 }
    };
    for (int i = 0; prefix[i].pfx; i++) {
        int pl = wp_len(prefix[i].pfx);
        int match = 1;
        for (int j = 0; j < pl; j++) if (stem[j] != prefix[i].pfx[j]) { match = 0; break; }
        // require a digit immediately after the prefix
        if (match && stem[pl] >= '0' && stem[pl] <= '9') {
            char *p = out;
            const char *lbl = prefix[i].label;
            while (*lbl) *p++ = *lbl++;
            *p++ = ' ';
            wp_append_num(&p, stem, pl);
            *p = 0;
            return;
        }
    }

    // 2. fallback: title-case EVERY word of the stem, not just the first character
    // of the whole string (#745 wallpaper-library-reset). The old version did
    // "TCX" -> "Tcx" (fine, one word) but "MOUNTAIN_VISTA" -> "Mountain_vista"
    // (wrong: only the very first letter of the entire name was capitalised,
    // every other word silently lower-cased, and the separator itself was left
    // in the display string). That was invisible on every wallpaper shipped
    // before 2026-08-11 because each one matched a curated exact name or an
    // EBERG/OCEAN/MACRO + number prefix, so this fallback path was never
    // exercised by a multi-word name. The 2026-08-11 wallpaper set
    // ("MOUNTAIN_VISTA.BMP", "CYBERPUNK_MAYTERA_WALL.BMP", ...) hits it for
    // most of the library, so the bug became visible in the picker.
    //
    // Shipped filenames use '_' as the word separator, NOT a space. Space was
    // tried first and reverted: build/build-golden.sh and build/invariant-
    // gate.sh both extract wallpaper filenames from `debugfs -R "ls -l /" |
    // awk '{print $NF}'`, which splits on whitespace, so a name with a real
    // space (e.g. "MOUNTAIN VISTA.BMP") gets truncated to its last word
    // ("VISTA.BMP") by that tooling and its WPTHUMB dump/lookup then misses
    // the file. '_' survives that parsing untouched.
    //
    // Fix here: '_' emits an actual space in the DISPLAY string and starts a
    // new word; every other character is capitalised only if it opens a word
    // (position 0, or right after a '_'), lower-cased otherwise. "TCX" and
    // every other underscore-free one-word name are byte-identical to the old
    // output (start_of_word is 1 only at i==0), so this changes nothing for
    // any name that used to look right.
    char *p = out;
    int start_of_word = 1;
    for (int i = 0; i < slen && (p - out) < WP_NAME_MAX - 1; i++) {
        char c = stem[i];
        if (c == '_') {
            *p++ = ' ';
            start_of_word = 1;
            continue;
        }
        *p++ = start_of_word ? wp_upper(c) : wp_lower(c);
        start_of_word = 0;
    }
    *p = 0;
}

int wp_enumerate(wp_entry_t *out, int max) {
    int count = 0;
    if (max <= 0) return 0;

    int fd = sys_open("/", 0);
    if (fd >= 0) {
        dirent_t e;
        // Guard against a pathological/looping directory: at most a few hundred
        // iterations. SYS_READDIR: 0 = entry filled, non-zero = end/error.
        for (int guard = 0; guard < 1024 && count < max - 1; guard++) {
            if (syscall2(SYS_READDIR, fd, (long)&e) != 0) break;
            if (!wp_is_bmp(e.name)) continue;
            if (wp_blocked(e.name)) continue;
            wp_ncpy(out[count].file, e.name, WP_FILE_MAX);
            wp_pretty(e.name, out[count].name);
            count++;
        }
        sys_close(fd);
    }

    // Always append the gradient entry last (file[0] == 0 signals "no BMP").
    if (count < max) {
        out[count].file[0] = 0;
        wp_ncpy(out[count].name, "Gradient (Blue)", WP_NAME_MAX);
        count++;
    }
    return count;
}
