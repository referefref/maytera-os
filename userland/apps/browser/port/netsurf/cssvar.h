/*
 * cssvar.h - CSS custom-property and math-function resolver (MayteraOS, MIT).
 *
 * WHY THIS EXISTS. libcss (the NetSurf CSS engine this browser links) has no
 * support for CSS custom properties at all: no `--name` declarations, no
 * `var()` substitution, and none of the `clamp()` / `min()` / `max()` / `calc()`
 * math functions. Grepping the pinned tree for "custom_prop" returns nothing and
 * "calc" appears only as a string in the property-name table.
 *
 * That is not a small gap on a modern page. On maytera.net's stylesheet, 243 of
 * roughly 540 declarations contain a `var()` and 29 contain a `clamp()`. Every
 * one of those is a PARSE ERROR to libcss, so the declaration is dropped and the
 * property falls back to its initial value. The measured effect: the entire
 * colour palette, every font stack, every border radius and most of the spacing
 * scale vanish, and the page renders as black text on white with no boxes. The
 * cascade works perfectly; it is simply being handed a stylesheet with the
 * colours removed.
 *
 * WHAT THIS IS, AND WHAT IT IS NOT. This is a SOURCE-LEVEL PREPROCESSOR that
 * runs over stylesheet text before libcss ever sees it. It is not a spec
 * implementation of custom properties, and the difference is worth stating
 * plainly:
 *
 *   - Real custom properties are INHERITED PER ELEMENT and resolved at
 *     computed-value time, so `--x` can hold different values on different
 *     elements of the same document. Here they are global.
 *   - `var()` inside a shorthand that is later overridden, cyclic references,
 *     and `@property` registrations are not modelled.
 *
 * What it DOES cover is the pattern essentially every real site uses: a palette
 * declared once in a `:root` block and referenced everywhere. Definitions are
 * therefore collected only from top-level `:root` / `html` / `body` / `*` rules
 * (never from inside an at-rule whose condition we have not evaluated), which is
 * exactly the "global palette" case and refuses to guess at the rest.
 *
 * ARITHMETIC IS INTEGER. Values are carried as milli-units (2.6rem is 2600) and
 * resolved to whole px. No float, no libm, deterministic.
 *
 * FAILURE IS ALWAYS "LEAVE IT ALONE". If a var is undefined with no fallback,
 * the substitution emits nothing, which makes the declaration invalid and libcss
 * drops it, which is what the spec requires anyway. If a math function contains
 * a unit we cannot resolve, the original text is copied through verbatim and
 * libcss drops it exactly as it does today. Nothing here can invent a value.
 */
#ifndef MAYTERA_CSSVAR_H
#define MAYTERA_CSSVAR_H

#include <stddef.h>

/*
 * Resolve custom properties and math functions in `src` (`len` bytes).
 *
 * viewport_w / viewport_h are the CSS pixel dimensions of the viewport and are
 * what `vw` / `vh` / `vmin` / `vmax` resolve against. Pass the real content
 * width, not a constant: clamp() on this class of page is how the whole spacing
 * and type scale is expressed.
 *
 * Returns a malloc'd NUL-terminated buffer (caller frees) with the resolved
 * length in *out_len, or NULL on allocation failure. Never returns a buffer
 * shorter than the semantic content of the input.
 */
char *cssvar_preprocess(const char *src, size_t len,
		int viewport_w, int viewport_h, size_t *out_len);

/*
 * Self-test. Returns the number of FAILING cases (0 = all pass) and, if `report`
 * is non-NULL, calls it once per case with a one-line result. Proves the
 * resolver on the shapes this page actually uses, including the ones designed
 * to be left alone.
 */
int cssvar_selftest(void (*report)(const char *line));

/* ------------------------------------------------------------------ */
/* CSS gradient carrier (#245 enggrad).                                */
/*
 * libcss (this pinned tree) does not parse CSS gradient functions:
 * `background: linear-gradient(...)` and `background-image:
 * radial-gradient(...)` are FUNCTION tokens it rejects, so the whole
 * declaration is dropped and the box loses its background. This is the same
 * gap that border-radius, var() and clamp() have, and it is closed the same
 * way: cssvar_preprocess() parses the bounded gradient syntax at source-rewrite
 * time into the table below and rewrites the declaration onto a carrier
 * property libcss DOES understand (outline-color, whose computed value carries
 * the table index), so the gradient reaches layout through the ordinary cascade
 * including specificity and @media. layout.c reads the carrier back and emits a
 * gradient paint item; main.c rasterises the colour ramp into the block's
 * background box behind its content.
 *
 * The table is process-global and single-threaded (one page load at a time). It
 * ACCUMULATES across every cssvar_preprocess() call for a page (one per author
 * <style>/<link> sheet), so the caller MUST call cssvar_gradients_reset() once
 * at the start of each page render, before any sheet is preprocessed.
 */
#define CSSVAR_GRAD_MAX_STOPS 8

typedef struct {
	unsigned char type;   /* 0 = linear, 1 = radial */
	unsigned char shape;  /* radial only: 0 = ellipse, 1 = circle */
	int angle;            /* linear only: degrees, CSS convention
	                       * (0 = to top, 90 = to right, clockwise) */
	int nstops;
	unsigned int stop_col[CSSVAR_GRAD_MAX_STOPS]; /* 0x00RRGGBB */
	int stop_pos[CSSVAR_GRAD_MAX_STOPS];          /* per-mille 0..1000, -1 = auto */
} cssvar_gradient;

/* Clear the gradient table. Call once per page render before preprocessing. */
void cssvar_gradients_reset(void);

/* Number of gradients parsed since the last reset. */
int cssvar_gradient_count(void);

/* Gradient at `idx`, or NULL if out of range. Valid until the next reset. */
const cssvar_gradient *cssvar_gradient_get(int idx);

#endif
