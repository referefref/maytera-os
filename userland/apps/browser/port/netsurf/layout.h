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
 *   - outline-offset  (#245 engoutline: the libcss build in this port has NO
 *                      outline-offset property at all, so there is nothing to
 *                      read; the outline hugs the border edge)
 *   - outline dashes/dots (dashed and dotted are PAINTED SOLID; see LAYOUT_OL_*)
 *   - outline-color: invert  (falls back to the computed text colour)
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

/*
 * vertical-align modes (#245 engvalign). Deliberately NOT the libcss
 * CSS_VERTICAL_ALIGN_* numbering: BASELINE must be 0 here so that a zeroed
 * layout_item is already at the initial value.
 */
#define LAYOUT_VA_BASELINE     0
#define LAYOUT_VA_SUB          1
#define LAYOUT_VA_SUPER        2
#define LAYOUT_VA_TOP          3
#define LAYOUT_VA_TEXT_TOP     4
#define LAYOUT_VA_MIDDLE       5
#define LAYOUT_VA_BOTTOM       6
#define LAYOUT_VA_TEXT_BOTTOM  7
#define LAYOUT_VA_LENGTH       8

/*
 * outline styles (#245 engoutline). Deliberately NOT the libcss
 * CSS_OUTLINE_STYLE_* numbering, for the same reason as LAYOUT_VA_* above and
 * one that bites harder here: libcss puts CSS_OUTLINE_STYLE_INHERIT at 0x0 and
 * CSS_OUTLINE_STYLE_NONE at 0x1 (it aliases css_border_style_e). item_new()
 * memsets the item, so storing the raw enum would make an untouched item read
 * as INHERIT and a genuine `outline-style: none` read as a truthy style, which
 * is exactly backwards: the inert value MUST be 0 on this side.
 *
 * DASHED and DOTTED are kept distinct from SOLID even though today's painter
 * draws all three solid (it has no dash primitive, only an axis-aligned rect
 * fill). The distinction is plumbed so adding one later touches the painter
 * only.
 */
#define LAYOUT_OL_NONE    0
#define LAYOUT_OL_SOLID   1
#define LAYOUT_OL_DASHED  2
#define LAYOUT_OL_DOTTED  3

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
	/*
	 * CSS letter-spacing for this run, in px, ALREADY INCLUDED in the advance
	 * layout gave the run (#245 engletsp). The painter MUST apply it too, or
	 * measured width stops equalling drawn width (#589) and the glyphs sit
	 * tight inside a box laid out wide. Zero is the initial value and means
	 * "draw the whole string in one call", which is what every run on a page
	 * that never authors letter-spacing carries, so the legacy paint path is
	 * untouched. May be NEGATIVE (-0.02em is a common tracking tightener).
	 *
	 * The spacing is added after EVERY glyph including the last, which is what
	 * CSS specifies, so the run's drawn width is measure(text) + ls * glyphs.
	 * `text` is single-byte Latin-1 by the time it reaches here (utf8_squash
	 * folds it before measuring), and the rasteriser indexes it by byte, so
	 * the glyph count is exactly the string length. Do NOT "fix" that into a
	 * UTF-8 continuation-byte count: 0xB7 (middle dot) would then be dropped.
	 */
	int letter_spacing;
	/*
	 * CSS vertical-align for this run (#245 engvalign), as a LAYOUT_VA_*
	 * value, NOT the raw libcss enum. The raw enum starts BASELINE at 1, and
	 * this engine needs the initial value to be the zero an item_new() memset
	 * already produces, so that every item on a page that never authors the
	 * property is inert without a single extra write.
	 *
	 * These three fields are layout-internal: they are consumed by the
	 * line-close pass that resolves the shift into `y`, and the field is reset
	 * to LAYOUT_VA_BASELINE once that has happened, so an item the painter
	 * sees always reads 0 here and its `y` is already final. The painter needs
	 * NO vertical-align code at all: unlike letter-spacing, which changes what
	 * happens INSIDE a run, vertical-align only moves the run's origin, and
	 * `y` is what every consumer (draw, link hit rects, overflow extents)
	 * already reads.
	 *
	 * valign_h is the element's OWN inline box height (its computed
	 * line-height), which the line-box-relative modes need in order to place
	 * the box inside a taller line. valign_px is the resolved raise in pixels
	 * and is meaningful only for LAYOUT_VA_LENGTH (positive raises).
	 */
	int valign;
	int valign_h;
	int valign_px;
	/*
	 * CSS outline (#245 engoutline). PAINT-ONLY, and that is the whole point
	 * of the property: an outline is drawn just OUTSIDE the border edge and
	 * takes NO layout space, so unlike a border it must never reach a
	 * position, a width, a height, an inline advance or a line height. Nothing
	 * in layout.c reads these three fields; only the painter does.
	 *
	 * ol_w == 0 means no outline, and 0 is what item_new()'s memset already
	 * leaves, so every item on a page that never authors an outline carries
	 * the inert value without a single extra write and the painter's outline
	 * block is never entered.
	 *
	 * ol_style is a LAYOUT_OL_* value, NOT the raw libcss enum (see above).
	 * ol_col is 0x00RRGGBB, already composited against the effective
	 * background, the same way bcol[] is.
	 */
	uint8_t  ol_w;
	uint8_t  ol_style;
	uint32_t ol_col;
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
	/*
	 * engwalkstack (#245): set when the walk refused a subtree because the
	 * DOM nested deeper than WALK_MAX_DEPTH. Same contract as `overflowed`:
	 * the page rendered, but part of it is missing, and the caller should
	 * say so rather than present a short page as a complete one.
	 * walk_depth_peak is the deepest the walk actually got, so how close a
	 * normal page came to the ceiling is measurable and not guessed at.
	 */
	int deep_truncated;
	int walk_depth_peak;
	/* engscroll (#245): overflow:scroll/auto containers that overflow. */
	scroll_box scrolls[LAYOUT_MAX_SCROLLS];
	int n_scrolls;
} layout_result;

/*
 * engwalkstack (#245): the md5 of the layout.c this binary was built from, or
 * "unstamped". Printed once at startup so a measurement can be tied to the
 * source it came from instead of to "the two binaries differ".
 */
const char *layout_src_stamp(void);

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
