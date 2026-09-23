/*
 * layout.h - MayteraOS block+inline+text layout (MIT, our own code).
 *
 * Walks a styled libdom tree and produces positioned text runs, background/
 * border boxes and image boxes for rendering into the browser viewport.
 *
 * Model: a real (if small) CSS box model. Every element gets a containing block
 * (an x origin and an available width); margins, borders and padding inset the
 * content box inside it; `width` and `max-width` constrain it; `margin: 0 auto`
 * centres it. Block-level children stack; inline content flows and wraps.
 *
 * NOT implemented, deliberately and with the consequence stated:
 *   - floats                  (rare on modern layouts; ignored, content stacks)
 *   - flexbox and grid        (containers lay their children out as blocks, so
 *                              a row of buttons stacks vertically instead of
 *                              sitting side by side; see the CHANGELOG for the
 *                              size of the real job)
 *   - position: absolute  (#245 engabspos: out-of-flow, placed by top/left
 *                          (bounded right/bottom) against the nearest
 *                          positioned ancestor / ICB, flat z-index stacking;
 *                          nested stacking contexts deferred, see the plan doc)
 *   - position: fixed / sticky  (fixed shares the absolute path but does not
 *                                re-pin to the viewport on scroll; sticky static)
 *   - box-shadow, gradients, opacity, transforms
 *   - per-corner border-radius (the shorthand's first value rounds all four)
 *
 * No libm: integer math only.
 */
#ifndef MAYTERA_LAYOUT_H
#define MAYTERA_LAYOUT_H

#include <stdint.h>
#include <dom/dom.h>
#include "css_select_bind.h"
#include "fontmap.h"

#define LAYOUT_MAX_ITEMS 4096
#define LAYOUT_RUN_MAX   256
#define LAYOUT_HREF_MAX  512

/* border sides, in the order CSS writes them */
#define LB_TOP    0
#define LB_RIGHT  1
#define LB_BOTTOM 2
#define LB_LEFT   3

typedef struct layout_item {
	int kind;          /* 0 = text run, 1 = box (bg/border), 3 = image */
	int w;             /* box width  (kind 1 and 3) */
	int h;             /* box height (kind 1 and 3) */
	uint32_t bg;       /* box background 0x00RRGGBB (kind 1) */
	int has_bg;
	/*
	 * Per-side borders. This used to be a single width plus a single colour,
	 * read from border-top and painted on the top AND bottom edges. That is
	 * wrong in both directions at once: an element with only a border-bottom
	 * grew a phantom top rule, and an element with a full `border: 1px solid`
	 * lost its left and right edges. Cards, code wells and header rules all
	 * depend on getting this right.
	 */
	uint32_t bcol[4];  /* T R B L */
	uint8_t  bw[4];    /* T R B L, px */
	/*
	 * Corner radius in px, already clamped to half the shorter side. Zero
	 * means a square box and the painter takes its old path. libcss has no
	 * radius properties, so this arrives through a carrier property; see the
	 * long note in cssvar.c before changing either end.
	 */
	int radius;
	int x;            /* relative to content origin */
	int y;            /* relative to content top (pre-scroll) */
	int size;         /* font size px */
	uint32_t color;   /* 0x00RRGGBB */
	/*
	 * The resolved TTF face and the SYNTHETIC style bits left over after
	 * resolution (#245). A page naming three font families used to draw in
	 * one, because layout had no way to ask how wide a run would be in a face
	 * other than the active one; ttf_measure_ex() closed that, and these two
	 * fields are what carry the answer from the cascade to the pixels.
	 * `bold`/`italic` below stay as the CSS-computed intent for anything that
	 * wants to know it; `fstyle` is what to pass the rasteriser, and it has
	 * the BOLD bit set ONLY when no real bold face existed for the family.
	 */
	int face;
	int fstyle;
	int bold;
	int italic;
	int underline;
	char text[LAYOUT_RUN_MAX];
	char href[LAYOUT_HREF_MAX];  /* link target, or form action for a control */
	int form_kind;     /* 0 none, 1 text field, 2 submit/button */
	char field_name[64];  /* <input name=> */
	/*
	 * engabspos (#245): paint-order key for CSS stacking. 0 for in-flow,
	 * non-positioned content, so a page with no positioned boxes keeps every
	 * item at 0 and the stable z-sort in layout_document() is skipped (AE=0).
	 * A positioned box stamps its own items with ZORDER_BASE + z-index, so
	 * they paint after in-flow content, ordered by z-index with document
	 * order breaking ties.
	 */
	int zorder;
} layout_item;

/*
 * engscroll (#245): one overflow:scroll / overflow:auto container whose content
 * overflows its content box vertically. Reported by layout so the app can draw
 * a scrollbar and drive a keyboard scroll offset. Indexed in block-CLOSE order,
 * which is deterministic and stable for a static document, so the app can key a
 * persistent per-container offset by array index across relayouts.
 */
#define LAYOUT_MAX_SCROLLS 32
typedef struct scroll_box {
	int content_x;      /* content box left  (absolute, pre page-scroll) */
	int content_y;      /* content box top   (absolute, pre page-scroll) */
	int content_w;      /* content box width  */
	int content_h;      /* content box height (the declared height) */
	int extent_h;       /* natural content height (>= content_h when overflowing) */
	int offset;         /* vertical scroll offset actually applied this layout */
	/* brhscroll (#245) horizontal axis: natural content width and the horizontal
	 * scroll offset actually applied. extent_w == content_w and offset_x == 0 for
	 * a box that does not overflow horizontally, so a vertical-only container is
	 * reported exactly as before. */
	int extent_w;       /* natural content width (>= content_w when overflowing) */
	int offset_x;       /* horizontal scroll offset actually applied this layout */
} scroll_box;

typedef struct layout_result {
	layout_item *items;
	int n_items;
	int content_height; /* total laid-out height in px */
	/*
	 * Canvas background, propagated from <html> (or <body> if <html> has
	 * none) the way CSS requires. The browser used to paint the page onto a
	 * hardcoded white sheet, so a dark site rendered as dark boxes floating
	 * on white with white gutters. has_doc_bg is 0 when the document sets no
	 * background at all, and the caller should keep its own default then.
	 */
	uint32_t doc_bg;
	int has_doc_bg;
	/* Set when the item array filled up, so the caller can say the page was
	 * truncated instead of silently showing a short one. */
	int overflowed;
	/* engscroll (#245): overflow:scroll/auto containers that overflow. */
	scroll_box scrolls[LAYOUT_MAX_SCROLLS];
	int n_scrolls;
} layout_result;

/*
 * Lay out the document tree into `out`. `content_width` is the wrap width in px.
 * `measure` measures a string at a size and returns pixel width.
 * Returns 0 on success.
 */
/*
 * `measure` measures a string at a size IN A NAMED FACE AND STYLE and returns
 * the pixel width. The face and style arguments are not optional decoration:
 * measuring a monospace run against the proportional default face wraps it at
 * the wrong word and overlaps whatever sits beside it. The caller must route
 * this to the same (face, style) pair it will later draw with.
 */
int layout_document(mcs_ctx *css, dom_document *doc, int content_width,
		int (*measure)(const char *s, int size, int face, int style),
		layout_result *out);

/* #245 perf: count of measure() (SYS_MEASURE_TTF) calls made by layout. */
/*
 * engscroll (#245): the app sets a requested vertical scroll offset per
 * overflow container (keyed by block-close-order index) BEFORE calling
 * layout_document; layout clamps it to the container extent and reports the
 * applied value in layout_result.scrolls[]. Reset clears all requests to 0.
 */
void layout_reset_scroll_requests(void);
void layout_set_scroll_request(int idx, int off);
void layout_set_scroll_request_x(int idx, int off);   /* brhscroll (#245) */

extern unsigned long g_layout_measure_calls;
extern unsigned long g_layout_measure_words;
/* #245 profiling buckets, in TSC cycles, reset at each layout_document(). */
extern unsigned long long g_lp_style, g_lp_text, g_lp_flow, g_lp_meas;
extern unsigned long long g_lp_attr, g_lp_walk;
extern unsigned long g_lp_nelem, g_lp_ntext;

#endif
