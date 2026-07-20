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
    // 1. exact-match curated names for the originally-shipped set.
    static const struct { const char *f; const char *n; } curated[] = {
        { "MAYTERA.BMP", "Maytera Modern" },
        { "CYBER.BMP",   "Maytera Cyber" },
        { "DESERT.BMP",  "Maytera Desert" },
        { "GREEN.BMP",   "Maytera Green" },
        { "MARK.BMP",    "Maytera Mark" },
        { "BACK.BMP",    "Default Blue" },
        { "MTNMAY1.BMP", "Mountain Maytera" },
        { "MTNMAY2.BMP", "Mountain Maytera 2" },
        { 0, 0 }
    };
    for (int i = 0; curated[i].f; i++) {
        if (wp_ieq(file, curated[i].f)) { wp_ncpy(out, curated[i].n, WP_NAME_MAX); return; }
    }

    // stem = filename without the ".BMP" extension.
    char stem[WP_FILE_MAX];
    int L = wp_len(file);
    int slen = (L >= 4) ? L - 4 : L;
    if (slen > WP_FILE_MAX - 1) slen = WP_FILE_MAX - 1;
    for (int i = 0; i < slen; i++) stem[i] = wp_upper(file[i]);
    stem[slen] = 0;

    // 2. category prefixes with a trailing number: EBERG -> Mountain, etc.
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

    // 3. fallback: title-case the stem ("TCX" -> "Tcx", "PLASMA" -> "Plasma").
    char *p = out;
    for (int i = 0; i < slen && (p - out) < WP_NAME_MAX - 1; i++) {
        *p++ = (i == 0) ? wp_upper(stem[i]) : wp_lower(stem[i]);
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
