/*
 * fontmap.h - CSS font-family -> installed TTF face resolution (#245).
 *
 * MayteraOS ships 54 faces (DejaVu, IBM Plex Mono, Lato, Noto, Source Pro,
 * Inconsolata, Linus Libertinus, Symbola) and has always been able to DRAW in
 * any of them. What was missing was anyone asking. Every page rendered in one
 * face, so a site with a three-family type system (display / body / monospace)
 * came out flattened to one and its code, terminal and label runs read as prose.
 *
 * This module is the lookup, and it is deliberately platform-free: it reaches
 * the font registry through three hooks the embedder supplies, so the SAME code
 * runs on the device against the SYS_FONT_* syscalls and in the host-side test
 * harness against FreeType. Nothing here does I/O or allocates.
 */
#ifndef MAYTERA_FONTMAP_H
#define MAYTERA_FONTMAP_H

/* Style bits, matching the kernel's TTF_STYLE_* and libc's FONT_STYLE_*. */
#define FM_STYLE_NORMAL 0
#define FM_STYLE_BOLD   1
#define FM_STYLE_ITALIC 2

/* Generic families, in libcss's CSS_FONT_FAMILY_* order. */
#define FM_GENERIC_NONE       0
#define FM_GENERIC_SERIF      1
#define FM_GENERIC_SANS       2
#define FM_GENERIC_CURSIVE    3
#define FM_GENERIC_FANTASY    4
#define FM_GENERIC_MONOSPACE  5

/*
 * Platform hooks. On the device these are font_count()/font_name()/font_style()
 * from libc; in the harness they are a FreeType enumeration of the same .TTF
 * files the golden ships. Indices are stable and MAY CONTAIN HOLES: a removed
 * face returns a zero-length name and must be skipped, which is the registry's
 * documented contract and the reason this enumerates rather than counts.
 */
int fm_plat_font_count(void);
int fm_plat_font_name(int idx, char *buf, int cap);
int fm_plat_font_style(int idx, char *buf, int cap);

/* Build the table. Cheap (name + subfamily strings only, no outlines are
 * touched), idempotent, and safe to call before any face has been used. */
void fontmap_init(void);

/*
 * Resolve one CSS font-family list entry.
 *
 * `family` is a single family name as written in CSS (case-insensitive, may
 * carry the web-safe aliases real sheets list: system-ui, ui-monospace,
 * -apple-system, Segoe UI, SF Mono, Menlo, Consolas, ...). `bold`/`italic` are
 * the computed font-weight/font-style.
 *
 * Returns 1 and fills *face and *style on a match, 0 if that family is not
 * installed and the caller should try the next name in the list.
 *
 * *style carries the SYNTHETIC part only: if a real Bold face was found, *style
 * has no BOLD bit, because asking the rasteriser to embolden an already-bold
 * face draws mud. If no bold face exists for the family, the bit is set and the
 * kernel fakes it. Either way the width the matching ttf_measure_ex() returns is
 * the width that gets drawn, which is the whole point of routing both through
 * the same (face, style) pair.
 */
int fontmap_family(const char *family, int bold, int italic, int *face, int *style);

/* Same, for a generic family. Always succeeds (falls back to face 0). */
int fontmap_generic(int generic, int bold, int italic, int *face, int *style);

/*
 * The synthetic style bits needed to draw `face` at the requested weight/slant.
 * Used when a family list resolves to nothing and the parent's face is
 * inherited: <strong> inside a monospace block must stay monospace and become
 * bold, which means asking whether THAT face is already bold rather than
 * reusing the parent's answer.
 */
int fontmap_synth_for(int face, int bold, int italic);

/* Number of faces the table actually holds; 0 before fontmap_init(). */
int fontmap_face_count(void);

#endif
