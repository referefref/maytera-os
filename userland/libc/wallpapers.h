// wallpapers.h - shared wallpaper enumeration for the MayteraOS userland (#517)
//
// The wallpaper picker (compositor) and the Settings Appearance grid used to keep
// their OWN hardcoded, index-coupled arrays of wallpaper filenames. That drifted:
// three entries (CLASSIC/DARKMODE/RETRO) referenced BMPs absent from the image and
// silently fell back to a gradient, and 47 of the 65 BMPs actually shipped were
// UNREACHABLE from either UI. Worse, the two arrays are coupled by INTEGER INDEX
// (Settings sets an index via set_wallpaper(); the compositor loads g_wallpapers[i]),
// so any divergence between them silently loads the wrong image.
//
// This is the single source of truth: BOTH consumers call wp_enumerate(), which
// scans the root directory for *.BMP wallpapers in the kernel's deterministic
// readdir order and appends a final gradient entry. Because both processes run the
// identical scan over the identical on-disk directory, they get byte-identical
// index -> file mappings, so the shared index can never desync. Adding a wallpaper
// to the image makes it reachable with NO recompile.
#ifndef _MAYTERA_WALLPAPERS_H
#define _MAYTERA_WALLPAPERS_H

#define WP_MAX_ENTRIES 96   // headroom over the ~64 wallpapers currently shipped
#define WP_FILE_MAX    40   // ext2 has no 8.3 limit, but wallpaper names stay short
#define WP_NAME_MAX    40

typedef struct {
    char file[WP_FILE_MAX];   // e.g. "MAYTERA.BMP"; empty ("") => gradient (no file)
    char name[WP_NAME_MAX];   // human display name
} wp_entry_t;

// Enumerate the wallpapers present at the filesystem root ("/"), in the kernel's
// deterministic readdir order, then append a final "Gradient (Blue)" entry whose
// file[0] == 0. Writes up to `max` entries into out[] and returns the count
// (always >= 1: even with no BMPs, the gradient entry is present). Skips a small
// blocklist of non-wallpaper BMPs (boot/studio splashes). Deterministic and
// side-effect free, so the compositor and Settings agree on every index.
int wp_enumerate(wp_entry_t *out, int max);

#endif // _MAYTERA_WALLPAPERS_H
