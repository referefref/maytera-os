/*
 * layout.c - MayteraOS HTML layout engine (MIT, original code).
 * See layout.h for the model and for what is deliberately not implemented.
 * Integer arithmetic only; no libm.
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <dom/dom.h>
#include <libcss/libcss.h>
#include <libcss/computed.h>
#include <libcss/properties.h>
#include <libcss/fpmath.h>

#include <libwapcaplet/libwapcaplet.h>

#include "layout.h"
#include "css_select_bind.h"
#include "cssvar.h"        /* #245 enggrad: gradient table lookup */
#include "fontmap.h"

static int node_is(dom_node *node, const char *name);

/*
 * engfloat2 (#245): the maximum number of CONCURRENT float bands modelled in
 * one containing block. Real pages rarely stack more than a couple of floats
 * on the same line; a small fixed cap keeps the state flat (no allocation, and
 * snapshot/restore across a child walk is a bounded memcpy) and bounds the
 * fit/recompute scans. A float that would exceed the cap falls back to normal
 * flow, exactly as a second float used to before multi-float support.
 */
#define FL_MAX 8

/*
 * engabspos (#245): base offset for a positioned box's paint-order key, so
 * ZORDER_BASE + z-index is always > 0 (in-flow content is 0 and paints first)
 * for any sane z-index. z-index is clamped to +/- 1e6 before the add so the
 * sum never overflows int.
 */
#define ZORDER_BASE 100000000

/* ---- layout state ---- */
typedef struct {
	mcs_ctx *css;
	int width;                 /* initial containing-block width */
	int (*measure)(const char *, int, int, int);
	layout_result *out;

	int cursor_x;              /* inline pen, in content coordinates */
	int cursor_y;              /* top of the current line box */
	int line_left;             /* x a wrapped line restarts at */
	int line_right;            /* x a line must wrap before */
	int line_height;           /* tallest run on the current line */
	bool line_has_content;
	/*
	 * A collapsed space that has not been spent yet, carried ACROSS text
	 * nodes. Whitespace between two inline elements is its own text node
	 * containing nothing but whitespace, so a per-node flag skipped it and
	 * the whole nav bar rendered as
	 * "HomeFeaturesDocsChangelogSecurityAboutGitHub". Words inside one node
	 * were spaced correctly, which is exactly why this went unnoticed.
	 */
	bool pending_space;
	/*
	 * How far down an out-of-flow (absolutely positioned) child reached.
	 * Absolutes are laid out where they start and then REWIND the pen, so
	 * two `position:absolute; inset:0` layers stack on top of each other
	 * instead of one below the other. Their parent still has to end up tall
	 * enough to contain them, which is what this carries back up.
	 */
	int oof_max_y;
	/* column-gap of the enclosing flex container, in px. See cssvar.c for why
	 * `gap` arrives dressed as a multi-column property. */
	int flex_gap;
	char cur_href[LAYOUT_HREF_MAX];  /* current <a> target, empty if none */
	/*
	 * text-align (#245). line_align is the establishing block's computed
	 * alignment (0 left, 1 center, 2 right); line_first is the index in
	 * out->items of the first inline item on the line currently being
	 * filled. A completed line's inline runs are shifted right by the free
	 * space (or half of it) when the block is centre- or right-aligned.
	 * LEFT/JUSTIFY leave every item where it was, so a left-aligned page
	 * cannot change.
	 */
	int line_align;
	int line_first;
	/*
	 * Set while a throwaway measuring pass is running (the table max-content
	 * probe lays a cell out against a 4000px line). Alignment must NOT shift
	 * runs then, or a centred th would report a ~2000px content width and
	 * blow the column-width algorithm up. Alignment changes position, never
	 * content width, so a measuring pass wants it off.
	 */
	int measuring;
	/*
	 * CSS float (#245 engfloat / engfloat2). Up to FL_MAX concurrent left/right
	 * float exclusion bands in the current containing block. Every consult is
	 * gated on fl_n > 0, which no page without a float ever sets, so normal flow
	 * is byte-identical (AE=0). With exactly one band the geometry reduces to the
	 * old single-float model exactly (see fl_fit/fl_recompute).
	 *
	 * fl_n: number of active bands. fl_bside[i]: 1 left, 2 right. fl_bx0/fl_bx1:
	 * band i's occupied margin-box left/right in content coords. fl_bbot[i]: the
	 * y at/below which band i no longer applies. fl_bseq[i]: a monotonic id used
	 * to tell floats opened by THIS block's children apart from pre-existing
	 * ones across a compaction that reorders the array. fl_cx0/fl_cx1: the
	 * containing block's full line bounds, captured when the first band opens and
	 * reverted to when every band has ended. fl_next_seq: the seq allocator.
	 */
	int fl_n;
	int8_t fl_bside[FL_MAX];
	int fl_bx0[FL_MAX], fl_bx1[FL_MAX];
	int fl_bbot[FL_MAX];
	int fl_bseq[FL_MAX];
	int fl_cx0, fl_cx1;
	int fl_next_seq;
	/*
	 * engabspos (#245): the current positioned containing block, the padding
	 * box of the nearest positioned ancestor (position != static), else the
	 * initial containing block / viewport. An absolutely positioned box
	 * resolves top/left/right/bottom against THIS, not the normal-flow
	 * containing block. pcb_h is -1 while an auto-height ancestor is still
	 * growing; bottom placement is applied only when it is known. Set to the
	 * ICB in layout_document() and pushed/popped around every positioned
	 * block, so a page with no absolutes never reads it (AE=0).
	 */
	int pcb_x, pcb_y, pcb_w, pcb_h;
} lstate;

/*
 * Composite `fg` over `bg` at alpha `a` (0-255). Integer, rounded.
 *
 * Why this is needed at all: libcss returns colours as 0xAARRGGBB and the old
 * reader tested (c >> 24) only as "not transparent", then used c & 0xffffff at
 * full strength. A chip at rgba(19, 32, 31, .7) and a tint at rgba(69, 216,
 * 194, .08) would both paint as solid slabs. Alpha is not decoration on this
 * class of page; it is how the whole surface hierarchy is expressed.
 */
static uint32_t blend_rgb(uint32_t fg, uint32_t bg, uint32_t a)
{
	uint32_t r = ((((fg >> 16) & 0xff) * a) + (((bg >> 16) & 0xff) * (255 - a)) + 127) / 255;
	uint32_t g = ((((fg >>  8) & 0xff) * a) + (((bg >>  8) & 0xff) * (255 - a)) + 127) / 255;
	uint32_t b = ((((fg      ) & 0xff) * a) + (((bg      ) & 0xff) * (255 - a)) + 127) / 255;
	return (r << 16) | (g << 8) | b;
}

/* engflex (#245): cap on flex items a single row container can align in a
 * paint pass; a container with more than this is left packed at the start
 * (correct, just unaligned) rather than aligned. Real nav/toolbar rows are
 * far under this. */
#define FLEX_MAX_ITEMS 64

static layout_item *item_new(lstate *st)
{
	layout_item *it;
	if (st->out->n_items >= LAYOUT_MAX_ITEMS) {
		st->out->overflowed = 1;
		return NULL;
	}
	it = &st->out->items[st->out->n_items++];
	memset(it, 0, sizeof(*it));
	return it;
}

static void emit_run(lstate *st, const char *s, int len, int size,
		uint32_t color, int bold, int italic, int underline,
		int face, int fstyle)
{
	layout_item *it;
	if (len <= 0) return;
	it = item_new(st);
	if (!it) return;
	it->kind = 0;
	if (len > LAYOUT_RUN_MAX - 1) len = LAYOUT_RUN_MAX - 1;
	memcpy(it->text, s, len);
	it->text[len] = '\0';
	it->x = st->cursor_x;
	it->y = st->cursor_y;
	it->size = size;
	it->color = color;
	it->face = face;
	it->fstyle = fstyle;
	it->bold = bold;
	it->italic = italic;
	it->underline = underline;
	{
		int i = 0;
		if (st->cur_href[0]) {
			while (st->cur_href[i] && i < LAYOUT_HREF_MAX - 1) { it->href[i] = st->cur_href[i]; i++; }
		}
		it->href[i] = 0;
	}
}

/*
 * Shift the inline runs of the line currently being filled to satisfy the
 * establishing block's text-align (#245). Called at the moment a line is
 * closed, while line_left / line_right / line_align still describe that line.
 *
 * STRICT NO-OP FOR LEFT AND JUSTIFY: nothing moves unless line_align is centre
 * or right, so no left-aligned content can shift and no page that renders
 * correctly today can regress. Only kind 0 (text) and kind 3 (inline image)
 * items move; a block background/border (kind 1) keeps the geometry the flow
 * gave it. `free` is the room left on the line; a line that overflowed its box
 * (free <= 0) is left where it is rather than pulled off the left edge.
 */
static void align_line(lstate *st)
{
	int free, shift, i, n;
	if (st->line_align == 0 || st->measuring) return;
	n = st->out->n_items;
	if (st->line_first >= n) return;
	free = st->line_right - st->cursor_x;
	if (free <= 0) return;
	shift = (st->line_align == 1) ? free / 2 : free;
	for (i = st->line_first; i < n; i++) {
		layout_item *it = &st->out->items[i];
		if (it->kind == 0 || it->kind == 3)
			it->x += shift;
	}
}

/*
 * Flexbox main-axis (justify-content) and cross-axis (align-items) alignment
 * for a ROW-direction, SINGLE-LINE flex container (#245 engflex).
 *
 * PAINT-ONLY. The flex line has ALREADY been laid out packed at the start of
 * the main axis and at the top of the cross axis (the pre-existing behaviour).
 * This pass RIGIDLY TRANSLATES each flex item's already-emitted layout_items
 * along the main axis (x) and/or the cross axis (y). It never resizes anything
 * (no flex-grow/shrink/basis, no stretch) and never reflows (no wrap): a
 * container that wrapped to a second line is detected and left exactly as it
 * was. It is only ever ARMED when justify-content asks for something other than
 * flex-start OR align-items asks for center/flex-end, so a default flex
 * container is byte-identical because this function is never entered for it.
 *
 * bnd[k] is the items[] index at which flex item k begins (bnd[0] == fstart),
 * bnd[cnt] == n_items is the sentinel end. cy[k] is the cursor_y flex item k
 * started on; any value other than line_top means the line wrapped and the
 * whole pass bails, leaving the item list untouched.
 */
static void flex_align(lstate *st, int fstart, int fend, int line_top,
		int content_x, int content_w, int used_x,
		uint8_t justify, uint8_t align,
		const int *bnd, const int *cy, int cnt)
{
	int n = st->out->n_items;
	int i, k;

	if (st->measuring || cnt < 1 || fstart >= n)
		return;
	/* Wrap guard: if any item did not start on the first line, this is a
	 * multi-line flex container and single-line alignment does not apply.
	 * Leave every item exactly where flow put it. */
	for (k = 0; k < cnt; k++)
		if (cy[k] != line_top)
			return;

	/* Main axis: distribute the free space per justify-content. */
	if (justify >= CSS_JUSTIFY_CONTENT_FLEX_END &&
			justify <= CSS_JUSTIFY_CONTENT_SPACE_EVENLY) {
		int freev = content_w - (used_x - content_x);
		if (freev > 0) {
			for (k = 0; k < cnt; k++) {
				int dx = 0;
				switch (justify) {
				case CSS_JUSTIFY_CONTENT_FLEX_END:
					dx = freev; break;
				case CSS_JUSTIFY_CONTENT_CENTER:
					dx = freev / 2; break;
				case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN:
					dx = (cnt > 1) ? (freev * k) / (cnt - 1) : 0;
					break;
				case CSS_JUSTIFY_CONTENT_SPACE_AROUND:
					dx = (freev * (2 * k + 1)) / (2 * cnt);
					break;
				case CSS_JUSTIFY_CONTENT_SPACE_EVENLY:
					dx = (freev * (k + 1)) / (cnt + 1);
					break;
				default: dx = 0; break;
				}
				if (dx)
					for (i = bnd[k]; i < bnd[k + 1]; i++)
						st->out->items[i].x += dx;
			}
		}
	}

	/* Cross axis: position each item within the line per align-items. Only
	 * center and flex-end move anything; stretch/flex-start/baseline keep the
	 * existing top-packed behaviour. A flex item's cross-size is the bounding
	 * box of its own items (text runs carry no h, so their cross-size is the
	 * font size). */
	if (align == CSS_ALIGN_ITEMS_CENTER || align == CSS_ALIGN_ITEMS_FLEX_END) {
		int line_cross = 0;
		for (i = fstart; i < fend; i++) {
			layout_item *it = &st->out->items[i];
			int ih = (it->kind == 0) ? it->size : it->h;
			int bot = (it->y - line_top) + ih;
			if (bot > line_cross)
				line_cross = bot;
		}
		for (k = 0; k < cnt; k++) {
			int top = 0x3fffffff, bot = 0, item_cross, dy;
			for (i = bnd[k]; i < bnd[k + 1]; i++) {
				layout_item *it = &st->out->items[i];
				int ih = (it->kind == 0) ? it->size : it->h;
				if (it->y < top) top = it->y;
				if (it->y + ih > bot) bot = it->y + ih;
			}
			if (bot <= top)
				continue;
			item_cross = bot - top;
			dy = (align == CSS_ALIGN_ITEMS_CENTER)
				? (line_top + (line_cross - item_cross) / 2) - top
				: (line_top + (line_cross - item_cross)) - top;
			if (dy)
				for (i = bnd[k]; i < bnd[k + 1]; i++)
					st->out->items[i].y += dy;
		}
	}
}

/*
 * Flexbox main-axis SIZING (flex-grow) for a ROW-direction, SINGLE-LINE flex
 * container (#245 engflexgrow). When the packed flex line leaves free main-axis
 * space AND at least one item declares flex-grow > 0, distribute that free space
 * to the growing items in proportion to their grow factors: widen each growing
 * item's principal box and shift every following item right by the running
 * total. Returns the px it distributed so the caller can tell justify-content
 * how much free space grow already ate (grow consumes it first, per CSS).
 *
 * BOUNDED, and never a regression by CONSTRUCTION:
 *   - grow ONLY (free space > 0). flex-shrink (free < 0) is handled by the
 *     separate flex_shrink_pass, which floors each item at a bounded
 *     min-content measure (flex_item_min_content). See
 *     docs/BROWSER_ENGINE_FLEXSIZE_PLAN.md.
 *   - a default container (every flex-grow factor is the initial 0) makes
 *     sum == 0 and this returns 0 having touched nothing, so its item list is
 *     byte-identical.
 *   - an item's own inner content is NOT re-wrapped: widening a shrink-to-fit
 *     box only adds trailing space and its (default left-aligned) content stays
 *     put, which is exactly what flex-grow does to it. Re-wrapping needs the
 *     re-layout pass the plan defers.
 *   - a container that wrapped to a second line is detected (cy[k] != line_top)
 *     and left exactly as flow put it (single-line only).
 * bnd[k]/cy[k] are the same per-item item-array boundary and start line
 * flex_align() uses; grow[k] is item k's flex-grow factor as a css_fixed.
 */
static int flex_grow_pass(lstate *st, int line_top, int content_x,
		int content_w, int used_x, const int *bnd, const int *cy,
		const css_fixed *grow, int cnt)
{
	int k, i, freev, cum, distributed, last_grow;
	css_fixed sum;
	int ek[FLEX_MAX_ITEMS];

	if (st->measuring || cnt < 1)
		return 0;
	/* Single-line only: bail if any item started below the first line. */
	for (k = 0; k < cnt; k++)
		if (cy[k] != line_top)
			return 0;

	freev = content_w - (used_x - content_x);
	if (freev <= 0)
		return 0;

	/* An item with an empty item range (display:none, or an element that
	 * emitted nothing) is not a growable flex item; exclude it so its share
	 * never becomes a phantom gap. */
	sum = 0;
	last_grow = -1;
	for (k = 0; k < cnt; k++) {
		if (grow[k] > 0 && bnd[k + 1] > bnd[k]) {
			sum += grow[k];
			last_grow = k;
		}
	}
	if (sum <= 0 || last_grow < 0)
		return 0;

	/* Per-item growth in integer px; the rounding remainder goes to the last
	 * growing item so the line fills the container exactly (no residual gap). */
	distributed = 0;
	for (k = 0; k < cnt; k++) {
		ek[k] = 0;
		if (grow[k] > 0 && bnd[k + 1] > bnd[k]) {
			ek[k] = FIXTOINT(FDIV(FMUL(INTTOFIX(freev), grow[k]), sum));
			if (ek[k] < 0) ek[k] = 0;
			distributed += ek[k];
		}
	}
	if (distributed < freev)
		ek[last_grow] += (freev - distributed);

	/* Apply: shift item k right by the running total of earlier growth, then
	 * widen its own principal box (first box/image item) by its share. A
	 * text-only item has no widenable box; its slot still expands (trailing
	 * space) so following items shift, which is correct. */
	cum = 0;
	for (k = 0; k < cnt; k++) {
		if (cum)
			for (i = bnd[k]; i < bnd[k + 1]; i++)
				st->out->items[i].x += cum;
		if (ek[k] > 0) {
			int bi = -1;
			for (i = bnd[k]; i < bnd[k + 1]; i++) {
				layout_item *it = &st->out->items[i];
				if (it->kind == 1 || it->kind == 3) { bi = i; break; }
			}
			if (bi >= 0)
				st->out->items[bi].w += ek[k];
			cum += ek[k];
		}
	}
	return cum;
}

/*
 * Flexbox main-axis SIZING (flex-basis) for a ROW-direction, SINGLE-LINE flex
 * container (#245 engflexsize). Before grow/shrink distribute free space, each
 * item's main size STARTS from its flex-basis when the author set one. This
 * pass resizes every item whose flex-basis is an explicit POSITIVE length to
 * that length and shifts each following item by the running delta, so the
 * packed line end handed to grow/shrink/align reflects the basis sizes.
 *
 * BOUNDED, and never a regression by CONSTRUCTION:
 *   - basis[k] is -1 for every item whose flex-basis is auto/content (the
 *     initial value) OR resolves to <= 0. Those keep their content-based size,
 *     so a container with no explicit positive flex-basis is byte-identical,
 *     AND the `flex: N` shorthand's basis-0 seed is left to the (unchanged)
 *     grow path rather than reworked here. A page's item list therefore cannot
 *     change unless it authored a positive flex-basis this engine previously
 *     ignored.
 *   - resizes ONLY an item with a principal box (kind 1 box / kind 3 image);
 *     a text-only item has no resizable box, so its basis is skipped (its slot
 *     is left at content width). Same bound the grow pass uses.
 *   - basis is applied as the item's BORDER-box main size. Exact under
 *     box-sizing:border-box; a content-box item is off by its horizontal
 *     padding+border, a bounded approximation the plan documents.
 *   - single-line only: bail if any item started below the first line.
 *   - an item's own inner content is NOT re-wrapped, exactly as the grow pass
 *     does not; widening only adds trailing space.
 * bnd[k]/cy[k] are the same per-item boundary and start line the other passes
 * use. Returns the net px the line grew (negative if a basis shrank a box).
 */
static int flex_basis_pass(lstate *st, int line_top,
		const int *bnd, const int *cy, const int *basis, int cnt)
{
	int k, i, cum = 0;

	if (st->measuring || cnt < 1)
		return 0;
	for (k = 0; k < cnt; k++)
		if (cy[k] != line_top)
			return 0;

	for (k = 0; k < cnt; k++) {
		int delta = 0;
		if (cum)
			for (i = bnd[k]; i < bnd[k + 1]; i++)
				st->out->items[i].x += cum;
		if (basis[k] > 0 && bnd[k + 1] > bnd[k]) {
			int bi = -1;
			for (i = bnd[k]; i < bnd[k + 1]; i++) {
				layout_item *it = &st->out->items[i];
				if (it->kind == 1 || it->kind == 3) { bi = i; break; }
			}
			if (bi >= 0) {
				delta = basis[k] - st->out->items[bi].w;
				st->out->items[bi].w = basis[k];
			}
		}
		cum += delta;
	}
	return cum;
}

/* Forward: text measure (defined later); needed by the min-content floor. */
static int lmeasure(lstate *st, const char *s, int size, int face, int fstyle);

/*
 * A BOUNDED min-content main size (px) for one flex item, the floor flex-shrink
 * may not cross (CSS flexbox 4.5 automatic minimum size / 9.7). This engine has
 * no general intrinsic-sizing pass, so the measure is the common-shape
 * approximation the plan (docs/BROWSER_ENGINE_FLEXSIZE_PLAN.md) scopes: the
 * widest unbreakable word across the item's text runs (nothing may shrink so
 * far a word is clipped), the width of any replaced/image item, PLUS the item's
 * own chrome (border+padding), recovered as the difference between its
 * border-box width and the extent its content currently occupies.
 *
 * `bi` is the item's principal box index, which is EXCLUDED from both the
 * widest-content and the content-extent scan: it is the box being shrunk, not
 * its content. Returns a value clamped to [1, curw].
 */
static int flex_item_min_content(lstate *st, int lo, int hi, int bi, int curw)
{
	int i, widest = 0, cx0 = 0x3fffffff, cx1 = 0, chrome, m;

	for (i = lo; i < hi; i++) {
		layout_item *it = &st->out->items[i];
		if (i == bi)
			continue;
		if (it->kind == 0) {
			const char *s = it->text;
			int a = 0, r;
			while (s[a]) {
				int b = a;
				while (s[b] && s[b] != ' ') b++;
				if (b > a) {
					char w[LAYOUT_RUN_MAX];
					int n = b - a, ww;
					if (n > LAYOUT_RUN_MAX - 1) n = LAYOUT_RUN_MAX - 1;
					memcpy(w, s + a, n); w[n] = '\0';
					ww = lmeasure(st, w, it->size, it->face, it->fstyle);
					if (ww > widest) widest = ww;
				}
				a = (s[b] == ' ') ? b + 1 : b;
			}
			r = it->x + lmeasure(st, it->text, it->size, it->face, it->fstyle);
			if (it->x < cx0) cx0 = it->x;
			if (r > cx1) cx1 = r;
		} else {
			if (it->w > widest) widest = it->w;
			if (it->x < cx0) cx0 = it->x;
			if (it->x + it->w > cx1) cx1 = it->x + it->w;
		}
	}
	chrome = (cx1 > cx0) ? (curw - (cx1 - cx0)) : 0;
	if (chrome < 0) chrome = 0;
	m = widest + chrome;
	if (m < 1) m = 1;
	if (m > curw) m = curw;
	return m;
}

/*
 * Flexbox main-axis SIZING (flex-shrink) for a ROW-direction, SINGLE-LINE flex
 * container (#245 engflexsize). When the packed (post-basis) line OVERFLOWS its
 * container, distribute the NEGATIVE free space by each item's scaled shrink
 * factor (flex-shrink * flex base size, CSS flexbox 9.7), never taking any item
 * below its bounded min-content floor. Narrows each shrinking item's principal
 * box and shifts every following item left by the running total. Returns the px
 * it removed (<= 0) so the caller can hand the true packed line end to
 * flex_align.
 *
 * BOUNDED, and never a regression by CONSTRUCTION:
 *   - acts ONLY on NEGATIVE free space (freev < 0). A default or fitting
 *     container has freev >= 0 and this returns 0 having touched nothing, so
 *     its item list is byte-identical. Grow (freev > 0) and shrink (freev < 0)
 *     are therefore mutually exclusive on the same used_x.
 *   - single-line only: a container that wrapped (cy[k] != line_top) is left
 *     exactly as flow put it. Re-wrapping needs the re-layout pass the plan
 *     defers, so a shrink is applied to the ONE case that is exact: an
 *     over-constrained single line (typically created by flex-basis above).
 *   - an item with flex-shrink 0, no principal box, or already at its floor is
 *     frozen and never narrowed.
 *   - inner content is NOT re-wrapped (same bound as grow); the min-content
 *     floor guarantees no word is clipped.
 * shrink[k] is item k's flex-shrink factor as a css_fixed (default 1).
 */
static int flex_shrink_pass(lstate *st, int line_top, int content_x,
		int content_w, int used_x, const int *bnd, const int *cy,
		const css_fixed *shrink, int cnt)
{
	int k, i, freev, deficit, remaining, cum, iter;
	int bi[FLEX_MAX_ITEMS], curw[FLEX_MAX_ITEMS], minf[FLEX_MAX_ITEMS];
	int removed[FLEX_MAX_ITEMS], frozen[FLEX_MAX_ITEMS];
	long long sfac[FLEX_MAX_ITEMS];

	if (st->measuring || cnt < 1)
		return 0;
	for (k = 0; k < cnt; k++)
		if (cy[k] != line_top)
			return 0;
	freev = content_w - (used_x - content_x);
	if (freev >= 0)
		return 0;
	deficit = -freev;

	for (k = 0; k < cnt; k++) {
		removed[k] = 0; frozen[k] = 0; bi[k] = -1;
		curw[k] = 0; minf[k] = 0; sfac[k] = 0;
		if (bnd[k + 1] <= bnd[k]) { frozen[k] = 1; continue; }
		for (i = bnd[k]; i < bnd[k + 1]; i++) {
			layout_item *it = &st->out->items[i];
			if (it->kind == 1 || it->kind == 3) { bi[k] = i; break; }
		}
		if (bi[k] < 0) { frozen[k] = 1; continue; }
		if (shrink[k] <= 0) { frozen[k] = 1; continue; }
		curw[k] = st->out->items[bi[k]].w;
		minf[k] = flex_item_min_content(st, bnd[k], bnd[k + 1], bi[k], curw[k]);
		if (curw[k] <= minf[k]) { frozen[k] = 1; continue; }
		/* Scaled shrink factor = flex-shrink * base size. shrink[k] is a
		 * css_fixed (1/1024 scale); the 1024 cancels in the division below. */
		sfac[k] = (long long) shrink[k] * (long long) curw[k];
	}

	remaining = deficit;
	for (iter = 0; iter < cnt && remaining > 0; iter++) {
		long long sum = 0;
		int distributed = 0, any_frozen = 0;
		for (k = 0; k < cnt; k++)
			if (!frozen[k]) sum += sfac[k];
		if (sum <= 0)
			break;
		for (k = 0; k < cnt; k++) {
			int give, cap;
			if (frozen[k]) continue;
			give = (int) (((long long) remaining * sfac[k]) / sum);
			cap = curw[k] - removed[k] - minf[k];
			if (give < 0) give = 0;
			if (give >= cap) { give = cap; frozen[k] = 1; any_frozen = 1; }
			removed[k] += give;
			distributed += give;
		}
		remaining -= distributed;
		if (!any_frozen)
			break;
	}
	/* Assign any rounding remainder to the first item that still has room. */
	if (remaining > 0) {
		for (k = 0; k < cnt; k++) {
			int cap = curw[k] - removed[k] - minf[k];
			if (bi[k] >= 0 && cap > 0) {
				int give = (cap < remaining) ? cap : remaining;
				removed[k] += give; remaining -= give;
				if (remaining <= 0) break;
			}
		}
	}

	cum = 0;
	for (k = 0; k < cnt; k++) {
		if (cum)
			for (i = bnd[k]; i < bnd[k + 1]; i++)
				st->out->items[i].x += cum;
		if (removed[k] > 0 && bi[k] >= 0) {
			st->out->items[bi[k]].w -= removed[k];
			cum -= removed[k];
		}
	}
	return cum;
}

/*
 * Main-axis span [minx, maxx) of one flex item (its items[] range [lo, hi)).
 * A text run measures its own width; a box or image carries it. Used by the
 * wrap pass to decide where a line breaks and to repack each line.
 */
static void flex_item_span(lstate *st, int lo, int hi, int *pminx, int *pmaxx)
{
	int i, minx = 0x3fffffff, maxx = -0x3fffffff;
	for (i = lo; i < hi; i++) {
		layout_item *it = &st->out->items[i];
		int l = it->x, r;
		if (it->kind == 0)
			r = it->x + lmeasure(st, it->text, it->size,
					it->face, it->fstyle);
		else
			r = it->x + it->w;
		if (l < minx) minx = l;
		if (r > maxx) maxx = r;
	}
	*pminx = minx; *pmaxx = maxx;
}

/*
 * Flexbox MULTI-LINE layout for a ROW-direction flex container with
 * flex-wrap:wrap (#245 engflexwrap). Built ON TOP of the single-line
 * machinery: it breaks the packed flex items into lines that each fit
 * content_w, then lays out EACH line independently with the existing per-line
 * passes (flex_grow_pass / flex_shrink_pass / flex_align) and stacks the lines
 * down the cross axis. flex-basis is applied ONCE up front (on the still-
 * packed line) so the wrap decision uses each item's hypothetical main size,
 * exactly as CSS specifies.
 *
 * GATED: only ever called when css_computed_flex_wrap == WRAP, so a
 * nowrap/default flex row and non-flex pages never enter here and stay byte-
 * identical (AE=0). A single item wider than the container keeps its own line
 * (CSS: an item that cannot fit is not split). Lines stack top-to-bottom.
 *
 * DEFERRED (docs/BROWSER_ENGINE_FLEXWRAP_PLAN.md): flex-wrap:wrap-reverse
 * (falls through to the single-line path, unchanged), align-content
 * distribution of leftover cross-axis space, and per-line cross-size stretch.
 * gap is used for BOTH the column gap between items and the row gap between
 * lines.
 *
 * bnd[0..cnt] are the item boundaries in items[]; grow/shrink/basis are the
 * per-item factors the caller already gathered. ftop is the y all items
 * currently sit on (the packed line). On return, st->cursor_y is the last
 * line's top and st->line_height its height, so the caller's block close lands
 * the pen at the bottom of the wrapped container.
 */
static void flex_wrap_lines(lstate *st, int ftop, int content_x,
		int content_w, uint8_t justify, uint8_t align, int gap,
		const int *bnd, const css_fixed *grow, const css_fixed *shrink,
		const int *basis, int cnt)
{
	int k, i, l, nlines;
	int W[FLEX_MAX_ITEMS];
	int lstart[FLEX_MAX_ITEMS + 1];
	int allcy[FLEX_MAX_ITEMS];
	int line_top, last_top = ftop, last_h = 0;

	if (st->measuring || cnt < 1)
		return;

	/* flex-basis first: resize items to their hypothetical main size on the
	 * still-packed single line, so the wrap decision below sees basis sizes. */
	for (k = 0; k < cnt; k++)
		allcy[k] = ftop;
	flex_basis_pass(st, ftop, bnd, allcy, basis, cnt);

	/* measure each item's post-basis main-axis width. */
	for (k = 0; k < cnt; k++) {
		int minx, maxx;
		flex_item_span(st, bnd[k], bnd[k + 1], &minx, &maxx);
		W[k] = (maxx > minx) ? (maxx - minx) : 0;
	}

	/* greedy line breaking: a new line starts when the next item would push
	 * the used width past content_w, unless it is the first on the line. */
	nlines = 0;
	lstart[0] = 0;
	{
		int used = 0;
		for (k = 0; k < cnt; k++) {
			int first = (k == lstart[nlines]);
			int need = W[k] + (first ? 0 : gap);
			if (!first && used + need > content_w) {
				nlines++;
				lstart[nlines] = k;
				used = W[k];
			} else {
				used += need;
			}
		}
	}
	nlines++;
	lstart[nlines] = cnt;

	/* lay out each line independently and stack it below the previous. */
	line_top = ftop;
	for (l = 0; l < nlines; l++) {
		int ks = lstart[l], ke = lstart[l + 1];
		int m = ke - ks;
		int tx, used_x, lh, grew, shrunk, j;
		int bl[FLEX_MAX_ITEMS + 1], cyl[FLEX_MAX_ITEMS];
		css_fixed gl[FLEX_MAX_ITEMS], sl[FLEX_MAX_ITEMS];
		if (m <= 0)
			continue;

		/* repack this line at content_x and move it down to line_top. */
		tx = content_x;
		for (k = ks; k < ke; k++) {
			int minx, maxx, w, dx, dy, ii, miny = 0x3fffffff;
			flex_item_span(st, bnd[k], bnd[k + 1], &minx, &maxx);
			for (ii = bnd[k]; ii < bnd[k + 1]; ii++)
				if (st->out->items[ii].y < miny)
					miny = st->out->items[ii].y;
			w = (maxx > minx) ? (maxx - minx) : 0;
			dx = (maxx > minx) ? (tx - minx) : 0;
			/* Move the item's top to line_top regardless of where flow (or the
			 * pre-existing atomic-inline wrap) already left it, so an item that
			 * overflowed and was wrapped once is not shifted a second time. */
			dy = (miny != 0x3fffffff) ? (line_top - miny) : 0;
			if (dx || dy)
				for (i = bnd[k]; i < bnd[k + 1]; i++) {
					st->out->items[i].x += dx;
					st->out->items[i].y += dy;
				}
			tx += w;
			if (k < ke - 1)
				tx += gap;
		}
		used_x = tx;

		/* per-line grow / shrink / justify / align via the existing passes.
		 * cyl[] all equal line_top so none of them takes its wrap-guard
		 * bail; bl[] is this line's sub-slice of the item boundaries. */
		for (j = 0; j < m; j++) {
			bl[j] = bnd[ks + j];
			cyl[j] = line_top;
			gl[j] = grow[ks + j];
			sl[j] = shrink[ks + j];
		}
		bl[m] = bnd[ke];
		grew = flex_grow_pass(st, line_top, content_x, content_w,
				used_x, bl, cyl, gl, m);
		used_x += grew;
		shrunk = flex_shrink_pass(st, line_top, content_x, content_w,
				used_x, bl, cyl, sl, m);
		used_x += shrunk;
		flex_align(st, bnd[ks], bnd[ke], line_top, content_x, content_w,
				used_x, justify, align, bl, cyl, m);

		/* line height = tallest item cross-size on this line. */
		lh = 0;
		for (i = bnd[ks]; i < bnd[ke]; i++) {
			layout_item *it = &st->out->items[i];
			int ih = (it->kind == 0) ? it->size : it->h;
			int bot = (it->y - line_top) + ih;
			if (bot > lh)
				lh = bot;
		}
		last_top = line_top;
		last_h = lh;
		line_top += lh + gap;
	}

	/* leave the pen at the LAST line's top with its height, so the caller's
	 * block-close line_break() advances to the bottom of the wrapped set and
	 * the container grows to contain every line. */
	st->cursor_y = last_top;
	st->cursor_x = content_x;
	st->line_height = last_h;
	st->line_has_content = true;
	st->line_align = 0;
	st->line_first = st->out->n_items;
}

/*
 * engfloat2 (#245) float-band primitives. All are inert on a float-free page
 * because every caller gates them on fl_n > 0 (or on opening a float), and with
 * exactly one band each reduces to the old single-float arithmetic.
 */

/* Drop every band that has ended at or above y, compacting the array so fl_n
 * returns to 0 once the last float is past. Reordering is safe: bands carry a
 * seq id for the one place (child-float containment) that must distinguish
 * them. */
static void fl_compact(lstate *st, int y)
{
	int i, j = 0;
	for (i = 0; i < st->fl_n; i++) {
		if (st->fl_bbot[i] <= y) continue;
		if (j != i) {
			st->fl_bside[j] = st->fl_bside[i];
			st->fl_bx0[j] = st->fl_bx0[i];
			st->fl_bx1[j] = st->fl_bx1[i];
			st->fl_bbot[j] = st->fl_bbot[i];
			st->fl_bseq[j] = st->fl_bseq[i];
		}
		j++;
	}
	st->fl_n = j;
}

/* Set line_left/line_right to the room left between the containing block bounds
 * and every band still active at y. With no active band this restores the full
 * containing box (the old single-float revert). */
static void fl_recompute(lstate *st, int y)
{
	int left = st->fl_cx0, right = st->fl_cx1, i;
	for (i = 0; i < st->fl_n; i++) {
		if (y >= st->fl_bbot[i]) continue;
		if (st->fl_bside[i] == 1) {
			if (st->fl_bx1[i] > left) left = st->fl_bx1[i];
		} else {
			if (st->fl_bx0[i] < right) right = st->fl_bx0[i];
		}
	}
	if (left > right) left = right;
	st->line_left = left;
	st->line_right = right;
}

/* Find the y >= ftop at which a float of margin-box width mbw on `side`
 * (1 left, 2 right) fits within [fl_cx0,fl_cx1] given the active bands, and
 * return its occupied [*ox0,*ox1]. If it does not fit beside the floats already
 * present it drops below the shortest overlapping one (CSS 2.1 9.5.1), bounded
 * by the band count. With no active band it places at the containing edge, which
 * is exactly what the old single-float path did. */
static int fl_fit(lstate *st, int side, int ftop, int mbw, int *ox0, int *ox1)
{
	int y = ftop, guard;
	for (guard = 0; guard <= st->fl_n; guard++) {
		int left = st->fl_cx0, right = st->fl_cx1, i;
		int nextbot = 0, have_next = 0;
		for (i = 0; i < st->fl_n; i++) {
			if (y >= st->fl_bbot[i]) continue;
			if (st->fl_bside[i] == 1) {
				if (st->fl_bx1[i] > left) left = st->fl_bx1[i];
			} else {
				if (st->fl_bx0[i] < right) right = st->fl_bx0[i];
			}
			if (!have_next || st->fl_bbot[i] < nextbot) {
				nextbot = st->fl_bbot[i];
				have_next = 1;
			}
		}
		if (mbw <= right - left || !have_next) {
			if (side == 1) { *ox0 = left; *ox1 = left + mbw; }
			else { *ox0 = right - mbw; *ox1 = right; }
			return y;
		}
		y = nextbot;   /* drop below the shortest overlapping float and retry */
	}
	if (side == 1) { *ox0 = st->fl_cx0; *ox1 = st->fl_cx0 + mbw; }
	else { *ox0 = st->fl_cx1 - mbw; *ox1 = st->fl_cx1; }
	return y;
}

/* Append a band. Caller has already checked fl_n < FL_MAX. */
static void fl_add(lstate *st, int side, int x0, int x1, int bottom)
{
	int k = st->fl_n;
	st->fl_bside[k] = (int8_t) side;
	st->fl_bx0[k] = x0;
	st->fl_bx1[k] = x1;
	st->fl_bbot[k] = bottom;
	st->fl_bseq[k] = st->fl_next_seq++;
	st->fl_n = k + 1;
}

static void line_break(lstate *st)
{
	align_line(st);
	if (st->line_has_content || st->line_height > 0)
		st->cursor_y += st->line_height > 0 ? st->line_height : 18;
	/*
	 * float (#245 engfloat2): retire bands the wrapped line has cleared and
	 * recompute the line box from whatever floats remain active at the new y.
	 * When the last band ends this restores the full containing box, matching
	 * the old single-float revert exactly. Inert unless a float is active, so no
	 * float-free page can move here.
	 */
	if (st->fl_n > 0) {
		fl_compact(st, st->cursor_y);
		fl_recompute(st, st->cursor_y);
	}
	st->cursor_x = st->line_left;
	st->line_height = 0;
	st->line_has_content = false;
	st->pending_space = false;   /* a wrapped line never starts with a space */
	st->line_first = st->out->n_items;
}

/* ---- style helpers ---- */
typedef struct {
	uint8_t display;
	int font_size;
	int line_height;           /* px for one line box of this element's text */
	uint32_t color;
	int bold;
	int italic;
	int underline;
	/* #245: the resolved face, and the style bits the FACE could not supply
	 * (so an already-bold face is never emboldened again). */
	int face;
	int fstyle;
	int margin_top;
	int margin_bottom;
	int margin_left;           /* -1 == auto */
	int margin_right;          /* -1 == auto */
	int pad[4];                /* T R B L */
	uint32_t bcol[4];
	int bw[4];
	int width;                 /* -1 == auto */
	int min_width;             /* -1 == none */
	int height;                /* -1 == auto */
	int max_width;             /* -1 == none */
	uint32_t bg; int has_bg;
	int radius;                /* border-radius px, via the cssvar carrier */
	int list_none;
	uint8_t list_type;         /* raw CSS_LIST_STYLE_TYPE_* (#245) */
	int text_align;            /* 0 left/justify, 1 center, 2 right (#245) */
	int text_transform;        /* 0 none, 1 upper, 2 lower, 3 capitalize (#245) */
	/*
	 * The opaque colour actually behind this element, inherited down the
	 * tree. There is no alpha channel in the framebuffer blit path, so every
	 * translucent colour has to be composited here, against this.
	 */
	uint32_t eff_bg;
	/*
	 * white-space mode (#245 engfix5). Resolved from the computed getter
	 * (preferred) with a DOM tag fallback, then inherited down the tree.
	 * See layout_text() for how each maps to collapse / wrap / newline.
	 */
	int white_space;
	/* text-indent px, applied to the first line only; 0 is inert (#245 engfix6) */
	int text_indent;
	/* visibility: 0 visible (initial), 1 hidden; inherited, paint-only (#245 engfix6) */
	int visibility;
} estyle;

/* white-space modes (#245 engfix5). WS_NORMAL is the initial value and MUST
 * drive the byte-identical legacy path in layout_text(). */
#define WS_NORMAL    0
#define WS_PRE       1
#define WS_NOWRAP    2
#define WS_PRE_WRAP  3
#define WS_PRE_LINE  4

/*
 * Viewport and root font size, for the units that need them. Set once per
 * layout_document(); layout is single-threaded and reentrant only through the
 * recursive walk, so file scope is the honest place for them.
 */
static int g_vp_w = 1024, g_vp_h = 768;
static const int G_ROOT_FS = 16;

/*
 * Resolve a CSS length to whole pixels.
 *
 * THIS FUNCTION USED TO HAVE A `default:` THAT RETURNED THE RAW NUMBER, and the
 * cost was not subtle. libcss does not pre-convert rem, ch, vw, vh, in, cm, mm,
 * pc or Q, so every one of them fell through that default: `padding: 1.4rem`
 * resolved to ONE pixel and `max-width: 34rem` to THIRTY-FOUR, which collapsed
 * a 544px terminal panel into a 36px column of stacked single words. It looked
 * like a wrapping bug and was a unit bug. A silent default that returns a
 * plausible-looking number is worse than one that returns zero, because zero
 * gets noticed.
 *
 * The em case also truncated before multiplying (FIXTOINT then scale), so
 * 1.4em came out as 16px rather than 22. Scale first, round after.
 */
static int fixed_px(css_fixed v, css_unit unit, int font_size, int pct_basis)
{
	switch (unit) {
	case CSS_UNIT_PX:   return FIXTOINT(v);
	case CSS_UNIT_EM:   return FIXTOINT(FMUL(v, INTTOFIX(font_size)));
	case CSS_UNIT_REM:  return FIXTOINT(FMUL(v, INTTOFIX(G_ROOT_FS)));
	case CSS_UNIT_LH:   return FIXTOINT(FMUL(v, INTTOFIX(font_size)));
	/* ex and ch have no metrics available here; the usual approximations. */
	case CSS_UNIT_EX:   return FIXTOINT(FMUL(v, INTTOFIX(font_size))) / 2;
	case CSS_UNIT_CH:   return FIXTOINT(FMUL(v, INTTOFIX(font_size))) / 2;
	case CSS_UNIT_PT:   return FIXTOINT(FMUL(v, INTTOFIX(96))) / 72;
	case CSS_UNIT_PC:   return FIXTOINT(FMUL(v, INTTOFIX(16)));
	case CSS_UNIT_IN:   return FIXTOINT(FMUL(v, INTTOFIX(96)));
	case CSS_UNIT_CM:   return FIXTOINT(FMUL(v, INTTOFIX(96))) / 254 * 10;
	case CSS_UNIT_MM:   return FIXTOINT(FMUL(v, INTTOFIX(96))) / 254;
	case CSS_UNIT_Q:    return FIXTOINT(FMUL(v, INTTOFIX(96))) / 1016;
	case CSS_UNIT_VW:
	case CSS_UNIT_VI:   return FIXTOINT(FMUL(v, INTTOFIX(g_vp_w))) / 100;
	case CSS_UNIT_VH:
	case CSS_UNIT_VB:   return FIXTOINT(FMUL(v, INTTOFIX(g_vp_h))) / 100;
	case CSS_UNIT_VMIN: return FIXTOINT(FMUL(v,
				INTTOFIX(g_vp_w < g_vp_h ? g_vp_w : g_vp_h))) / 100;
	case CSS_UNIT_VMAX: return FIXTOINT(FMUL(v,
				INTTOFIX(g_vp_w > g_vp_h ? g_vp_w : g_vp_h))) / 100;
	case CSS_UNIT_PCT:  return pct_basis >= 0
				? FIXTOINT(FDIV(FMUL(v, INTTOFIX(pct_basis)), INTTOFIX(100)))
				: 0;
	/* An unresolved calc() and every non-length unit (deg, s, Hz) resolve to
	 * nothing rather than to their bare number. */
	default:            return 0;
	}
}

/*
 * Read one border side. `side` is 0=T 1=R 2=B 3=L. Returns the used width (0
 * when the side is none/hidden or the colour is transparent) and writes the
 * composited colour.
 */
static int read_border_side(const css_computed_style *s, int side,
		int font_size, uint32_t under, uint32_t *col_out)
{
	css_fixed bw = 0; css_unit bu = CSS_UNIT_PX;
	css_color bcol = 0;
	uint8_t bs = CSS_BORDER_STYLE_NONE;
	uint8_t cr = 0;
	int w;

	switch (side) {
	case LB_TOP:
		css_computed_border_top_width(s, &bw, &bu);
		bs = css_computed_border_top_style(s);
		cr = css_computed_border_top_color(s, &bcol);
		break;
	case LB_RIGHT:
		css_computed_border_right_width(s, &bw, &bu);
		bs = css_computed_border_right_style(s);
		cr = css_computed_border_right_color(s, &bcol);
		break;
	case LB_BOTTOM:
		css_computed_border_bottom_width(s, &bw, &bu);
		bs = css_computed_border_bottom_style(s);
		cr = css_computed_border_bottom_color(s, &bcol);
		break;
	default:
		css_computed_border_left_width(s, &bw, &bu);
		bs = css_computed_border_left_style(s);
		cr = css_computed_border_left_color(s, &bcol);
		break;
	}

	*col_out = 0;
	if (bs == CSS_BORDER_STYLE_NONE || bs == CSS_BORDER_STYLE_HIDDEN)
		return 0;
	if (cr != CSS_BORDER_COLOR_COLOR)
		return 0;
	{
		uint32_t a = (bcol >> 24) & 0xff;
		if (a == 0) return 0;
		*col_out = (a == 255) ? (bcol & 0xffffff)
			: blend_rgb(bcol & 0xffffff, under, a);
	}
	w = fixed_px(bw, bu, font_size, -1);
	if (w <= 0) return 0;
	if (w > 8) w = 8;
	return w;
}

static void read_style(const css_computed_style *s, const estyle *parent,
		int cb_width, estyle *e)
{
	css_fixed fv; css_unit fu;
	css_color col;
	int i;

	e->display = css_computed_display(s, false);

	css_computed_font_size(s, &fv, &fu);
	if (fu == CSS_UNIT_PX) e->font_size = FIXTOINT(fv);
	else e->font_size = parent ? parent->font_size : 16;
	if (e->font_size < 6) e->font_size = 6;
	if (e->font_size > 96) e->font_size = 96;

	/*
	 * Line height. This was hardcoded to font_size + 4, so a page setting
	 * line-height: 1.65 on <body> (which is a very ordinary thing to do) got
	 * roughly 1.25 and every block of prose came out visibly cramped.
	 */
	{
		css_fixed lv; css_unit lu;
		uint8_t lt = css_computed_line_height(s, &lv, &lu);
		if (lt == CSS_LINE_HEIGHT_NUMBER)
			e->line_height = FIXTOINT(FMUL(lv, INTTOFIX(e->font_size)));
		else if (lt == CSS_LINE_HEIGHT_DIMENSION)
			e->line_height = fixed_px(lv, lu, e->font_size, -1);
		else
			e->line_height = (e->font_size * 12) / 10;
		if (e->line_height < e->font_size + 1)
			e->line_height = e->font_size + 1;
		if (e->line_height > e->font_size * 4)
			e->line_height = e->font_size * 4;
	}

	/*
	 * Background first: the text colour, the border colours and every
	 * descendant all composite against it.
	 */
	{
		css_color bc;
		uint32_t under = parent ? parent->eff_bg : 0x00ffffffu;
		e->eff_bg = under;
		e->has_bg = 0;
		e->bg = 0;
		if (css_computed_background_color(s, &bc) == CSS_BACKGROUND_COLOR_COLOR) {
			uint32_t a = (bc >> 24) & 0xff;
			if (a == 255) {
				e->has_bg = 1;
				e->bg = bc & 0xffffff;
				e->eff_bg = e->bg;
			} else if (a > 0) {
				e->has_bg = 1;
				e->bg = blend_rgb(bc & 0xffffff, under, a);
				e->eff_bg = e->bg;
			}
		}
	}

	/*
	 * Text colour. An alpha of 0 is treated as "no colour of its own" rather
	 * than as invisible text: an invisible run is the worst possible failure
	 * mode here, and inheriting is what an unset value means anyway.
	 */
	if (css_computed_color(s, &col) == CSS_COLOR_COLOR && ((col >> 24) & 0xff)) {
		uint32_t ca = (col >> 24) & 0xff;
		e->color = (ca == 255) ? (col & 0xffffff)
			: blend_rgb(col & 0xffffff,
				parent ? parent->eff_bg : 0x00ffffffu, ca);
	} else {
		e->color = parent ? parent->color : 0x000000;
	}

	{
		uint8_t w = css_computed_font_weight(s);
		e->bold = (w == CSS_FONT_WEIGHT_BOLD || w == CSS_FONT_WEIGHT_BOLDER ||
				w >= CSS_FONT_WEIGHT_700) ? 1 : 0;
	}
	{
		uint8_t fs = css_computed_font_style(s);
		e->italic = (fs == CSS_FONT_STYLE_ITALIC) ? 1 : 0;
	}
	{
		uint8_t td = css_computed_text_decoration(s);
		e->underline = (td & CSS_TEXT_DECORATION_UNDERLINE) ? 1 : 0;
	}

	/*
	 * FONT FAMILY (#245). This was never read at all, so every page rendered
	 * in the one active face: maytera.net names three (a display face, a body
	 * sans and IBM Plex Mono), and the mono half of that, which is its
	 * eyebrows, chips, stat labels, figure captions, code spans and terminal
	 * panel, came out as prose in the body face.
	 *
	 * The list is walked in the order the author wrote it, which is the whole
	 * mechanism CSS gives a page for degrading gracefully. The generic at the
	 * end of the list is the last resort, and fontmap_generic() never fails,
	 * so e->face is always valid.
	 *
	 * Not implemented: @font-face and webfont download. A page whose first
	 * choice is a webfont therefore falls through its own fallback list
	 * exactly as a browser on a machine without that font would, which is the
	 * correct behaviour rather than an approximation of it.
	 */
	{
		lwc_string **names = NULL;
		uint8_t gen = css_computed_font_family(s, &names);
		int got = 0;
		e->face = 0;
		e->fstyle = (e->bold ? FM_STYLE_BOLD : 0) |
			(e->italic ? FM_STYLE_ITALIC : 0);
		if (names) {
			int k;
			for (k = 0; names[k] != NULL && !got; k++) {
				char nm[64];
				const char *d = lwc_string_data(names[k]);
				size_t l = lwc_string_length(names[k]);
				size_t j;
				if (l > sizeof(nm) - 1) l = sizeof(nm) - 1;
				for (j = 0; j < l; j++) nm[j] = d[j];
				nm[l] = 0;
				got = fontmap_family(nm, e->bold, e->italic,
						&e->face, &e->fstyle);
			}
		}
		if (!got && gen != CSS_FONT_FAMILY_INHERIT)
			got = fontmap_generic((int) gen, e->bold, e->italic,
					&e->face, &e->fstyle);
		/*
		 * No family list and no generic: keep the parent's FACE but
		 * recompute the synthetic bits against it, because <strong> under
		 * a mono parent must stay mono and become bold, not inherit the
		 * parent's "not bold" answer.
		 */
		if (!got) {
			e->face = parent ? parent->face : 0;
			e->fstyle = fontmap_synth_for(e->face, e->bold, e->italic);
		}
	}
	{
		uint8_t lst = css_computed_list_style_type(s);
		e->list_none = (lst == CSS_LIST_STYLE_TYPE_NONE);
		e->list_type = lst;
	}
	{
		uint8_t ta = css_computed_text_align(s);
		if (ta == CSS_TEXT_ALIGN_CENTER || ta == CSS_TEXT_ALIGN_LIBCSS_CENTER)
			e->text_align = 1;
		else if (ta == CSS_TEXT_ALIGN_RIGHT || ta == CSS_TEXT_ALIGN_LIBCSS_RIGHT)
			e->text_align = 2;
		else
			e->text_align = 0;
	}
	/*
	 * text-transform (#245). An INHERITED property, and css_select_bind
	 * composes against the parent, so the value read here is already the
	 * inherited one (a <span> inside a nav that set uppercase reports
	 * uppercase without any manual walk). The transform itself is applied
	 * per word in layout_text(), on the folded bytes, so measured width
	 * still equals drawn width. Anything but the three cased values (the
	 * initial `none`, and INHERIT which compose resolves away) is 0/no-op.
	 */
	{
		uint8_t tt = css_computed_text_transform(s);
		if (tt == CSS_TEXT_TRANSFORM_UPPERCASE)
			e->text_transform = 1;
		else if (tt == CSS_TEXT_TRANSFORM_LOWERCASE)
			e->text_transform = 2;
		else if (tt == CSS_TEXT_TRANSFORM_CAPITALIZE)
			e->text_transform = 3;
		else
			e->text_transform = 0;
	}

	/*
	 * white-space (#245 engfix5). INHERITED. Prefer the computed getter, but
	 * this port's libcss does not compute every property (engfix4: the
	 * list-style-type getter is dead and always returns DISC), so a
	 * NORMAL / INHERIT reading is treated as "no information" and the
	 * parent's already-resolved value is inherited. walk() then applies a
	 * DOM tag override (<pre>, <nobr>) that only promotes FROM normal, so a
	 * non-normal getter value always wins and a future libcss fix lights up
	 * automatically. Inert at the initial value: a tree that authors
	 * white-space nowhere resolves to WS_NORMAL throughout, which drives the
	 * byte-identical legacy path in layout_text().
	 */
	e->white_space = parent ? parent->white_space : WS_NORMAL;
	{
		uint8_t wsp = css_computed_white_space(s);
		if (wsp == CSS_WHITE_SPACE_PRE)             e->white_space = WS_PRE;
		else if (wsp == CSS_WHITE_SPACE_NOWRAP)     e->white_space = WS_NOWRAP;
		else if (wsp == CSS_WHITE_SPACE_PRE_WRAP)   e->white_space = WS_PRE_WRAP;
		else if (wsp == CSS_WHITE_SPACE_PRE_LINE)   e->white_space = WS_PRE_LINE;
		/* NORMAL / INHERIT: keep the inherited value set above. */
	}

	/*
	 * text-indent (#245 engfix6). INHERITED; libcss composes, so the getter
	 * returns the already-inherited length. Resolved to px here (percentage
	 * against the containing-block width, like padding). Applied to the FIRST
	 * line only at block open in walk(). Inert at the initial 0: a tree that
	 * authors text-indent nowhere resolves to 0 and adds nothing to cursor_x.
	 */
	{
		css_fixed iv; css_unit iu;
		css_computed_text_indent(s, &iv, &iu);
		e->text_indent = fixed_px(iv, iu, e->font_size, cb_width);
	}

	/*
	 * visibility (#245 engfix6). INHERITED, and PAINT-ONLY: a hidden element
	 * still takes its space, only its text/background/border/marker are not
	 * emitted (see emit_word() and the box gates in walk()). As with
	 * white-space, only an explicit HIDDEN/COLLAPSE is treated as information;
	 * otherwise the parent's resolved value is inherited, so a hidden subtree
	 * stays hidden even where this port's libcss does not compose the property
	 * down (getter-can-lie, engfix4). A descendant setting visibility:visible
	 * back on is NOT honoured here (documented follow-up). Inert at the initial
	 * VISIBLE: e->visibility stays 0 and every paint gate is a no-op.
	 */
	e->visibility = parent ? parent->visibility : 0;
	{
		uint8_t vis = css_computed_visibility(s);
		if (vis == CSS_VISIBILITY_HIDDEN || vis == CSS_VISIBILITY_COLLAPSE)
			e->visibility = 1;
	}

	for (i = 0; i < 4; i++)
		e->bw[i] = read_border_side(s, i, e->font_size, e->eff_bg, &e->bcol[i]);

	/*
	 * BORDER-RADIUS, arriving as column-rule-width. libcss has no radius
	 * property; cssvar.c rewrites the declaration onto a carrier before the
	 * sheet is parsed, so the value reaches here through the ordinary cascade
	 * including @media. Read cssvar.c's note before touching either side.
	 */
	e->radius = 0;
	{
		css_fixed rv; css_unit ru;
		if (css_computed_column_rule_width(s, &rv, &ru) ==
				CSS_COLUMN_RULE_WIDTH_WIDTH) {
			int r = fixed_px(rv, ru, e->font_size, cb_width);
			if (r < 0) r = 0;
			if (r > 4000) r = 4000;
			e->radius = r;
		}
	}

	/* Padding. Percentages resolve against the containing block WIDTH on
	 * every side, including top and bottom; that is what CSS says. */
	{
		css_fixed pv; css_unit pu;
		css_computed_padding_top(s, &pv, &pu);
		e->pad[LB_TOP] = fixed_px(pv, pu, e->font_size, cb_width);
		css_computed_padding_right(s, &pv, &pu);
		e->pad[LB_RIGHT] = fixed_px(pv, pu, e->font_size, cb_width);
		css_computed_padding_bottom(s, &pv, &pu);
		e->pad[LB_BOTTOM] = fixed_px(pv, pu, e->font_size, cb_width);
		css_computed_padding_left(s, &pv, &pu);
		e->pad[LB_LEFT] = fixed_px(pv, pu, e->font_size, cb_width);
		for (i = 0; i < 4; i++) {
			if (e->pad[i] < 0) e->pad[i] = 0;
			if (e->pad[i] > 400) e->pad[i] = 400;
		}
	}

	{
		css_fixed mv; css_unit mu;
		uint8_t mt;
		mt = css_computed_margin_top(s, &mv, &mu);
		e->margin_top = (mt == CSS_MARGIN_SET)
			? fixed_px(mv, mu, e->font_size, cb_width) : 0;
		mt = css_computed_margin_bottom(s, &mv, &mu);
		e->margin_bottom = (mt == CSS_MARGIN_SET)
			? fixed_px(mv, mu, e->font_size, cb_width) : 0;
		mt = css_computed_margin_left(s, &mv, &mu);
		e->margin_left = (mt == CSS_MARGIN_AUTO) ? -1
			: ((mt == CSS_MARGIN_SET)
				? fixed_px(mv, mu, e->font_size, cb_width) : 0);
		mt = css_computed_margin_right(s, &mv, &mu);
		e->margin_right = (mt == CSS_MARGIN_AUTO) ? -1
			: ((mt == CSS_MARGIN_SET)
				? fixed_px(mv, mu, e->font_size, cb_width) : 0);
		if (e->margin_top < 0) e->margin_top = 0;
		if (e->margin_bottom < 0) e->margin_bottom = 0;
	}

	{
		css_fixed wv; css_unit wu;
		uint8_t wt = css_computed_width(s, &wv, &wu);
		e->width = (wt == CSS_WIDTH_SET)
			? fixed_px(wv, wu, e->font_size, cb_width) : -1;
		wt = css_computed_max_width(s, &wv, &wu);
		e->max_width = (wt == CSS_MAX_WIDTH_SET)
			? fixed_px(wv, wu, e->font_size, cb_width) : -1;
		/* min-width matters for shrink-to-fit boxes and for nothing else
		 * here: it is what stops a short flex item collapsing onto its own
		 * text, which is exactly what the site's advisory stat tiles use it
		 * for (`min-width: 130px` on a box whose content is "12"). */
		wt = css_computed_min_width(s, &wv, &wu);
		e->min_width = (wt == CSS_MIN_WIDTH_SET)
			? fixed_px(wv, wu, e->font_size, cb_width) : -1;
		if (e->width == 0) e->width = -1;
		/*
		 * HEIGHT. Never read until now, so an element sized ONLY by height
		 * collapsed to nothing and was painted as a zero-pixel box. The
		 * terminal panels on maytera.net each carry three 9x9 window pips
		 * (.frame-dots i sets width:9px and height:9px): the width was
		 * honoured and the height was not, so all 36 pips across the page
		 * had h=0 and none of them ever appeared.
		 *
		 * A PERCENTAGE IS DELIBERATELY LEFT AUTO. It resolves against the
		 * containing block HEIGHT, which this engine does not track, and
		 * resolving it against the WIDTH instead (which is what the shared
		 * fixed_px helper would do) is how you get a box of confidently
		 * wrong size. Auto is the honest answer; see the unit-switch
		 * default trap in blame.md, same shape.
		 */
		wt = css_computed_height(s, &wv, &wu);
		e->height = (wt == CSS_HEIGHT_SET && wu != CSS_UNIT_PCT)
			? fixed_px(wv, wu, e->font_size, cb_width) : -1;
		if (e->height <= 0) e->height = -1;
	}

	/*
	 * box-sizing (#245, engfix3). The initial value is content-box, under
	 * which a declared width/height names the CONTENT box and the padding
	 * and border are added on top: exactly what the layout math downstream
	 * assumes. Under border-box the declared width/height INCLUDES the
	 * padding and border, so recover the content-box value here by
	 * subtracting them. The getter had zero call sites before this, and the
	 * whole block is inert at the initial content-box value, so a page that
	 * never sets border-box cannot move (proven by an AE=0 pixel diff).
	 *
	 * This runs identically in the throwaway table max-content probe
	 * (st->measuring): it is a pure per-element style computation, not a
	 * line-position shift like text-align, so a cell's content width is
	 * corrected the same whether it is being measured or drawn and
	 * measured-equals-drawn (#589) is preserved for free. min/max-width
	 * follow box-sizing too (they clamp the content width downstream), so
	 * subtract from them as well; every result is floored at 0.
	 */
	if (css_computed_box_sizing(s) == CSS_BOX_SIZING_BORDER_BOX) {
		int hsub = e->pad[LB_LEFT] + e->pad[LB_RIGHT]
			+ e->bw[LB_LEFT] + e->bw[LB_RIGHT];
		int vsub = e->pad[LB_TOP] + e->pad[LB_BOTTOM]
			+ e->bw[LB_TOP] + e->bw[LB_BOTTOM];
		if (e->width >= 0)     { e->width     -= hsub; if (e->width < 0)     e->width = 0; }
		if (e->max_width >= 0) { e->max_width -= hsub; if (e->max_width < 0) e->max_width = 0; }
		if (e->min_width >= 0) { e->min_width -= hsub; if (e->min_width < 0) e->min_width = 0; }
		if (e->height >= 0)    { e->height    -= vsub; if (e->height < 0)    e->height = 0; }
	}
}

/* #245 perf instrumentation: how many measure() calls one layout costs.
 * Each one is a SYS_MEASURE_TTF syscall, so this is a syscall count. */
/*
 * engscroll (#245): per-container requested scroll offsets, set by the app via
 * layout_set_scroll_request() before a layout pass. Indexed in block-close
 * order (see scroll_box in layout.h). File-scope so the layout_document()
 * signature stays unchanged (and the m*_test harnesses keep compiling), the
 * same reason the gradient/radius carriers live outside the API. All zero by
 * default, so a caller that never touches them gets no scrolling (AE=0).
 */
static int g_scroll_req[LAYOUT_MAX_SCROLLS];
static int g_scroll_req_x[LAYOUT_MAX_SCROLLS];   /* brhscroll (#245): horizontal */
void layout_reset_scroll_requests(void)
{
	int i;
	for (i = 0; i < LAYOUT_MAX_SCROLLS; i++) { g_scroll_req[i] = 0; g_scroll_req_x[i] = 0; }
}
void layout_set_scroll_request(int idx, int off)
{
	if (idx >= 0 && idx < LAYOUT_MAX_SCROLLS) g_scroll_req[idx] = off < 0 ? 0 : off;
}
void layout_set_scroll_request_x(int idx, int off)
{
	if (idx >= 0 && idx < LAYOUT_MAX_SCROLLS) g_scroll_req_x[idx] = off < 0 ? 0 : off;
}
static int layout_get_scroll_request(int idx)
{
	return (idx >= 0 && idx < LAYOUT_MAX_SCROLLS) ? g_scroll_req[idx] : 0;
}
static int layout_get_scroll_request_x(int idx)
{
	return (idx >= 0 && idx < LAYOUT_MAX_SCROLLS) ? g_scroll_req_x[idx] : 0;
}

unsigned long g_layout_measure_calls = 0;
/* #245 perf: total measure REQUESTS (cache hits + misses). measures/words
 * is the memo hit rate. */
unsigned long g_layout_measure_words = 0;

/* #245 profiling buckets. rdtsc, not uptime_ms: the 250Hz tick is 4ms granular
 * and every one of these calls is far shorter than that, so a tick-based timer
 * would round almost all of them to zero. */
#ifdef BROWSER_PERF
static inline unsigned long long lrdtsc(void)
{
	unsigned int lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long) hi << 32) | lo;
}
#else
/* Default build: no timing instructions at all. The bucket arithmetic below
 * then folds to adding zero, which costs a couple of instructions per node and
 * keeps this file free of #ifdefs at every measurement site. Build the browser
 * with -DBROWSER_PERF to get the real numbers back. */
static inline unsigned long long lrdtsc(void) { return 0; }
#endif
unsigned long long g_lp_style = 0;   /* mcs_compute_style()            */
unsigned long long g_lp_text  = 0;   /* dom_node_get_text_content()    */
unsigned long long g_lp_flow  = 0;   /* layout_text() word flow        */
unsigned long long g_lp_meas  = 0;   /* the measure() syscall itself   */
unsigned long long g_lp_attr  = 0;   /* per-element href attribute get */
unsigned long long g_lp_walk  = 0;   /* whole walk (denominator)       */
unsigned long g_lp_nelem = 0;
unsigned long g_lp_ntext = 0;

/* #245 perf: memoise measure().
 *
 * measure() is a SYS_MEASURE_TTF syscall, and the kernel walks the string glyph
 * by glyph doing an uncached kerning lookup for every adjacent pair. Measured on
 * a 1,000,090-byte page: 133,327 calls costing 226,008 ms, i.e. 62% of a 363 s
 * page load, at ~1.7 ms per call.
 *
 * measure() is PURE for a fixed active font face and size, and running prose
 * repeats words heavily, so this memo is EXACT and not an approximation: a hit
 * returns the identical width the kernel would have returned, which is what
 * keeps the #589 "measured width == drawn width" invariant intact. Only the
 * syscall is skipped, never the answer.
 *
 * Direct-mapped, fixed size, no allocation: a collision just evicts and re-asks
 * the kernel, so a pathological page degrades to today's behaviour and can never
 * grow memory or return a wrong width. Words longer than LM_KEY-1 bypass the
 * cache entirely (they are rare and the key would not fit). The table is reset
 * at the top of every layout_document() because the active face can change
 * between page loads; within one layout it cannot.
 */
#define LM_SLOTS 4096
#define LM_KEY   32
static struct {
	int  used;
	int  size;
	int  face;
	int  fstyle;
	int  len;
	int  w;
	char k[LM_KEY];
} lm_cache[LM_SLOTS];

static void lm_reset(void)
{
	int i;
	for (i = 0; i < LM_SLOTS; i++) lm_cache[i].used = 0;
}

/*
 * #245: the FACE AND STYLE ARE PART OF THE KEY. They have to be. The memo is
 * only exact because measure() is pure for a fixed (face, style, size), and the
 * moment a page uses two families the same word appears at the same size in
 * both. Keying on the size alone would hand a monospace run the proportional
 * width, and the wrong answer would be returned confidently and forever.
 */
static int lmeasure(lstate *st, const char *s, int size, int face, int fstyle)
{
	g_layout_measure_words++;
	unsigned int h = 2166136261u;
	int len = 0, i;
	int slot, w;

	while (s[len]) len++;
	if (len >= LM_KEY) {		/* too long to key: ask the kernel */
		g_layout_measure_calls++;
		{ unsigned long long _t = lrdtsc();
		  int _w = st->measure(s, size, face, fstyle);
		  g_lp_meas += lrdtsc() - _t; return _w; }
	}
	for (i = 0; i < len; i++) {	/* FNV-1a over the bytes, then the size */
		h ^= (unsigned char) s[i];
		h *= 16777619u;
	}
	h ^= (unsigned int) size;
	h *= 16777619u;
	h ^= (unsigned int) (face * 131 + fstyle);
	h *= 16777619u;
	slot = (int) (h & (LM_SLOTS - 1));

	if (lm_cache[slot].used && lm_cache[slot].size == size &&
			lm_cache[slot].face == face &&
			lm_cache[slot].fstyle == fstyle &&
			lm_cache[slot].len == len) {
		for (i = 0; i < len; i++)
			if (lm_cache[slot].k[i] != s[i]) break;
		if (i == len) return lm_cache[slot].w;	/* exact key match */
	}

	g_layout_measure_calls++;
	{ unsigned long long _t = lrdtsc();
	  w = st->measure(s, size, face, fstyle);
	  g_lp_meas += lrdtsc() - _t; }
	lm_cache[slot].used = 1;
	lm_cache[slot].size = size;
	lm_cache[slot].face = face;
	lm_cache[slot].fstyle = fstyle;
	lm_cache[slot].len  = len;
	lm_cache[slot].w    = w;
	for (i = 0; i < len; i++) lm_cache[slot].k[i] = s[i];
	return w;
}

/*
 * Fold UTF-8 down to the single-byte space the renderer can actually draw.
 *
 * THIS IS NOT A TRANSPORT DETAIL. The kernel's text path is byte-oriented from
 * end to end: ttf_draw_string() and ttf_measure_string() both do
 * ttf_get_glyph((unsigned char) str[i], ...), so the codepoint IS the byte.
 * libhubbub decodes HTML entities into UTF-8, so every `&nbsp;`, `&middot;`,
 * `&ndash;` and `&approx;` on a page arrives as two or three bytes and gets
 * drawn as two or three separate Latin-1 glyphs. Measured on the live site:
 * "Windows&nbsp;3.1" rendered as "WindowsA 3.1", "1&ndash;16 cores" as
 * "1a__16 cores", and "C&nbsp;+&nbsp;Rust" as "CA +A Rust".
 *
 * The fold happens HERE, before measuring, so measured width still equals drawn
 * width. Codepoints below 0x100 are Latin-1 and pass through as their byte,
 * which is exactly what the rasteriser will look up. The punctuation table
 * covers what real prose actually uses; anything else becomes '?', which is
 * honest about being unrenderable rather than emitting mojibake.
 *
 * Doing this in the kernel instead would be the better fix and is a bigger one:
 * ttf_measure_string(), ttf_draw_string(), the cursor-step helper and every
 * caller that indexes a string by byte would all have to agree on a decoder,
 * and #589's measured-equals-drawn invariant is what holds that together.
 */
static int utf8_squash(const char *in, int len, char *out, int cap)
{
	int i = 0, o = 0;
	while (i < len && o < cap - 1) {
		unsigned char c = (unsigned char) in[i];
		unsigned int cp;
		int n;
		if (c < 0x80) { out[o++] = (char) c; i++; continue; }
		if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; n = 1; }
		else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; n = 2; }
		else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; n = 3; }
		else { out[o++] = '?'; i++; continue; }   /* stray continuation */
		if (i + n >= len) {   /* truncated sequence at the end of the run */
			out[o++] = '?';
			i = len;
			continue;
		}
		{
			int k;
			int ok = 1;
			for (k = 1; k <= n; k++) {
				unsigned char cc = (unsigned char) in[i + k];
				if ((cc & 0xc0) != 0x80) { ok = 0; break; }
				cp = (cp << 6) | (cc & 0x3f);
			}
			if (!ok) { out[o++] = '?'; i++; continue; }
			i += n + 1;
		}
		if (cp == 0x00a0) { out[o++] = ' '; continue; }        /* nbsp */
		if (cp == 0x00ad) { continue; }                        /* soft hyphen */
		if (cp < 0x100)   { out[o++] = (char) cp; continue; }   /* Latin-1 */
		switch (cp) {
		case 0x2010: case 0x2011: case 0x2012: case 0x2013:
		case 0x2014: case 0x2015: case 0x2212:
			out[o++] = '-'; break;
		case 0x2018: case 0x2019: case 0x201b: case 0x2032:
			out[o++] = '\''; break;
		case 0x201c: case 0x201d: case 0x201e: case 0x2033:
			out[o++] = '"'; break;
		case 0x2022: case 0x2023: case 0x25cf: case 0x25aa:
			out[o++] = (char) 0xb7; break;                 /* middle dot */
		case 0x2026:
			if (o + 3 < cap) { out[o++]='.'; out[o++]='.'; out[o++]='.'; }
			break;
		case 0x2039: out[o++] = '<'; break;
		case 0x203a: out[o++] = '>'; break;
		case 0x2190: if (o + 2 < cap) { out[o++]='<'; out[o++]='-'; } break;
		case 0x2192: if (o + 2 < cap) { out[o++]='-'; out[o++]='>'; } break;
		case 0x2248: out[o++] = '~'; break;                    /* approx */
		case 0x2260: if (o + 2 < cap) { out[o++]='!'; out[o++]='='; } break;
		case 0x2261: out[o++] = '='; break;                    /* identical to */
		case 0x2264: if (o + 2 < cap) { out[o++]='<'; out[o++]='='; } break;
		case 0x2265: if (o + 2 < cap) { out[o++]='>'; out[o++]='='; } break;
		case 0x2713: case 0x2714: out[o++] = 'v'; break;
		case 0x2717: case 0x2718: case 0x00d7: out[o++] = 'x'; break;
		case 0x200b: case 0x200c: case 0x200d: case 0xfeff:
			break;                                         /* zero width */
		case 0x2028: case 0x2029: out[o++] = ' '; break;
		default:
			/* Box-drawing and block elements: a terminal cursor drawn as
			 * '?' reads as an error rather than as a cursor, and this
			 * page puts one in its hero. Nothing in Latin-1 is a filled
			 * block, so '#' is the closest honest stand-in. */
			if (cp >= 0x2500 && cp <= 0x259f) out[o++] = '#';
			else out[o++] = '?';
			break;
		}
	}
	out[o] = '\0';
	return o;
}

/*
 * text-transform case mapping (#245). Byte-based, because the renderer and
 * everything upstream of it is byte-based Latin-1 after utf8_squash(): each
 * mapping turns one byte into exactly one byte, so a transformed word is the
 * same length as the original and the measured-equals-drawn invariant (#589)
 * is preserved for free. ASCII A-Z/a-z plus the Latin-1 letter blocks
 * (0xC0-0xDE upper / 0xE0-0xFE lower), skipping 0xD7 MULTIPLICATION and 0xF7
 * DIVISION, which sit inside those ranges but are not letters. German sharp s
 * (0xDF -> "SS") would change length and is deliberately left untouched.
 */
static unsigned char tt_upper(unsigned char c) {
	if (c >= 'a' && c <= 'z') return (unsigned char)(c - 32);
	if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (unsigned char)(c - 32);
	return c;
}
static unsigned char tt_lower(unsigned char c) {
	if (c >= 'A' && c <= 'Z') return (unsigned char)(c + 32);
	if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (unsigned char)(c + 32);
	return c;
}
static int tt_is_alpha(unsigned char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7);
}
/*
 * Apply a text-transform to one whitespace-delimited word IN PLACE. mode is
 * the estyle code: 1 uppercase, 2 lowercase, 3 capitalize. capitalize maps
 * the first LETTER of the word to upper and leaves the rest as authored
 * (CSS does not lower-case the tail). layout_text() tokenises on whitespace,
 * which is exactly CSS's notion of a word for capitalize, so this is correct
 * per word without any extra boundary tracking.
 */
static void apply_text_transform(char *word, int len, int mode) {
	int j;
	if (mode == 1) {
		for (j = 0; j < len; j++)
			word[j] = (char) tt_upper((unsigned char) word[j]);
	} else if (mode == 2) {
		for (j = 0; j < len; j++)
			word[j] = (char) tt_lower((unsigned char) word[j]);
	} else if (mode == 3) {
		for (j = 0; j < len; j++) {
			if (tt_is_alpha((unsigned char) word[j])) {
				word[j] = (char) tt_upper((unsigned char) word[j]);
				break;
			}
		}
	}
}

/* ---- text emission with word wrap ---- */

/*
 * Emit one already-folded word (`word`, length `wl`) at the pen, wrapping to a
 * new line first if `do_wrap` and it no longer fits. The measure / wrap-check /
 * emit / advance sequence, and the #589 measured==drawn invariant, live here so
 * both the collapse and the whitespace-preserving paths share ONE copy. `word`
 * is mutated in place by text-transform, so it is not const. Callers apply any
 * leading inter-word space and manage pending_space; this helper does not.
 */
static void emit_word(lstate *st, const estyle *e, char *word, int wl,
		int do_wrap)
{
	int ww;
	if (e->text_transform)
		apply_text_transform(word, wl, e->text_transform);
	ww = lmeasure(st, word, e->font_size, e->face, e->fstyle);
	if (ww <= 0) ww = wl * (e->font_size / 2 + 1);
	if (do_wrap && st->line_has_content && st->cursor_x + ww > st->line_right)
		line_break(st);
	/* visibility:hidden (#245 engfix6): reserve the advance, paint nothing. */
	if (!e->visibility)
		emit_run(st, word, wl, e->font_size, e->color, e->bold,
				e->italic, e->underline, e->face, e->fstyle);
	st->cursor_x += ww;
	if (e->line_height > st->line_height)
		st->line_height = e->line_height;
	st->line_has_content = true;
}

/*
 * Whitespace-COLLAPSING flow: runs of spaces/tabs/newlines fold to a single
 * inter-word space (the classic HTML rule). This is the legacy path; at
 * WS_NORMAL it is invoked with do_wrap=1, honor_nl=0 and is byte-identical to
 * the pre-engfix5 code (the nl counter is computed but unused, the do_wrap
 * guard is always true). `do_wrap`=0 gives white-space:nowrap (never wrap);
 * `honor_nl`=1 gives white-space:pre-line (a newline forces a line break while
 * spaces still collapse).
 */
static void layout_text_collapse(lstate *st, const char *text, size_t len,
		const estyle *e, int do_wrap, int honor_nl)
{
	size_t i = 0;
	int space_w = lmeasure(st, " ", e->font_size, e->face, e->fstyle);
	if (space_w <= 0) space_w = e->font_size / 3 + 1;

	while (i < len) {
		size_t ws, we;
		char word[LAYOUT_RUN_MAX];
		int wl;
		int nl = 0;

		/* skip whitespace (collapse), remembering it beyond this node */
		while (i < len && (text[i] == ' ' || text[i] == '\t' ||
				text[i] == '\n' || text[i] == '\r')) {
			if (text[i] == '\n') nl++;
			i++; st->pending_space = true;
		}
		/* pre-line: each newline in the run is a forced break */
		if (honor_nl && nl > 0) {
			int k;
			for (k = 0; k < nl; k++) {
				if (st->line_height < e->line_height)
					st->line_height = e->line_height;
				line_break(st);   /* also clears pending_space */
			}
		}
		if (i >= len) break;
		ws = i;
		while (i < len && !(text[i] == ' ' || text[i] == '\t' ||
				text[i] == '\n' || text[i] == '\r')) i++;
		we = i;
		wl = (int)(we - ws);
		if (wl > LAYOUT_RUN_MAX - 1) wl = LAYOUT_RUN_MAX - 1;
		/* Fold before measuring, so measured width == drawn width. */
		wl = utf8_squash(text + ws, wl, word, LAYOUT_RUN_MAX);
		if (wl <= 0) continue;

		/* leading space between words on the same line */
		if (st->pending_space && st->line_has_content)
			st->cursor_x += space_w;
		st->pending_space = false;

		emit_word(st, e, word, wl, do_wrap);
	}
}

/*
 * Whitespace-PRESERVING flow for white-space:pre (do_wrap=0) and pre-wrap
 * (do_wrap=1). Runs of spaces/tabs are kept and drawn literally (preserving
 * indentation), tabs expand to 8-column tab stops, and every newline forces a
 * line break (a blank line still advances one line box). pending_space is not
 * used: preserved whitespace is emitted as real space glyphs, so measured width
 * still equals drawn width (#589).
 */
static void layout_text_preserve(lstate *st, const char *text, size_t len,
		const estyle *e, int do_wrap)
{
	size_t i = 0;
	int col = 0;   /* column within the current line, for tab stops */
	st->pending_space = false;

	while (i < len) {
		unsigned char c = (unsigned char) text[i];
		if (c == '\r') { i++; continue; }
		if (c == '\n') {
			if (st->line_height < e->line_height)
				st->line_height = e->line_height;
			line_break(st);
			col = 0;
			i++;
			continue;
		}
		if (c == ' ' || c == '\t') {
			char sp[LAYOUT_RUN_MAX];
			int n = 0;
			while (i < len && (text[i] == ' ' || text[i] == '\t') &&
					n < LAYOUT_RUN_MAX - 9) {
				if (text[i] == '\t') {
					int adv = 8 - (col % 8);
					while (adv-- > 0 && n < LAYOUT_RUN_MAX - 1) {
						sp[n++] = ' '; col++;
					}
				} else {
					sp[n++] = ' '; col++;
				}
				i++;
			}
			if (n > 0)
				emit_word(st, e, sp, n, 0);   /* spaces never wrap */
			continue;
		}
		{
			size_t ws = i;
			char word[LAYOUT_RUN_MAX];
			int wl;
			while (i < len && text[i] != ' ' && text[i] != '\t' &&
					text[i] != '\n' && text[i] != '\r') i++;
			wl = (int)(i - ws);
			if (wl > LAYOUT_RUN_MAX - 1) wl = LAYOUT_RUN_MAX - 1;
			wl = utf8_squash(text + ws, wl, word, LAYOUT_RUN_MAX);
			if (wl <= 0) continue;
			col += wl;
			emit_word(st, e, word, wl, do_wrap);
		}
	}
}

/*
 * white-space dispatch (#245 engfix5). WS_NORMAL takes the legacy collapsing
 * path with wrap on and newlines ignored, so a normal paragraph renders exactly
 * as before this change (proven AE=0).
 */
static void layout_text(lstate *st, const char *text, size_t len,
		const estyle *e)
{
	int ws = e->white_space;
	if (ws == WS_PRE || ws == WS_PRE_WRAP) {
		layout_text_preserve(st, text, len, e, ws == WS_PRE_WRAP);
		return;
	}
	/* WS_NORMAL / WS_NOWRAP / WS_PRE_LINE all collapse; only wrap and
	 * newline-handling differ. */
	layout_text_collapse(st, text, len, e,
			ws != WS_NOWRAP,      /* do_wrap: off only for nowrap */
			ws == WS_PRE_LINE);   /* honor_nl: only pre-line here */
}

/* ---- form-control helpers ---- */
static int lz_len(const char *p) { int n = 0; while (p[n]) n++; return n; }

/* Case-insensitive match of an element's tag name against `name` (lowercase). */
static int node_is(dom_node *node, const char *name) {
	dom_string *nm = NULL;
	if (dom_node_get_node_name(node, &nm) != DOM_NO_ERR || nm == NULL) return 0;
	const char *d = dom_string_data(nm);
	size_t l = dom_string_byte_length(nm);
	int ok = 1; size_t i = 0;
	for (; i < l && name[i]; i++) {
		char c = d[i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
		if (c != name[i]) { ok = 0; break; }
	}
	if (ok && (name[i] != 0 || i != l)) ok = 0;
	dom_string_unref(nm);
	return ok;
}

/* Copy an element attribute value into out[cap]; out[0]=0 if absent. */
static void get_attr(dom_node *node, const char *name, char *out, int cap) {
	dom_string *an = NULL, *val = NULL;
	out[0] = 0;
	if (dom_string_create((const uint8_t *) name, (size_t) lz_len(name), &an)
			== DOM_NO_ERR && an) {
		if (dom_element_get_attribute((dom_element *) node, an, &val)
				== DOM_NO_ERR && val) {
			size_t l = dom_string_byte_length(val);
			if (l > (size_t) cap - 1) l = (size_t) cap - 1;
			memcpy(out, dom_string_data(val), l); out[l] = 0;
			dom_string_unref(val);
		}
		dom_string_unref(an);
	}
}

/* Walk up to the enclosing <form> and copy its action attribute. */
static void get_form_action(dom_node *node, char *out, int cap) {
	dom_node *cur = node; int owned = 0; int d;
	out[0] = 0;
	for (d = 0; d < 32 && cur; d++) {
		if (node_is(cur, "form")) {
			get_attr(cur, "action", out, cap);
			if (owned) dom_node_unref(cur);
			return;
		}
		dom_node *p = NULL;
		dom_node_get_parent_node(cur, &p);
		if (owned) dom_node_unref(cur);
		cur = p; owned = 1;
	}
	if (owned && cur) dom_node_unref(cur);
}

/*
 * Is this display value block-level for our purposes?
 *
 * FLEX and GRID are included, and that is a deliberate approximation with a
 * visible consequence: their children stack vertically instead of flowing in a
 * row or a grid. What it fixes is much worse than what it leaves: without it a
 * flex or grid container falls through to "inline", so a whole page section
 * built on `display: flex` generated no block break at all and its contents ran
 * together into one paragraph.
 */
static bool display_is_block(uint8_t d)
{
	return d == CSS_DISPLAY_BLOCK ||
	       d == CSS_DISPLAY_LIST_ITEM ||
	       d == CSS_DISPLAY_FLEX ||
	       d == CSS_DISPLAY_GRID ||
	       d == CSS_DISPLAY_TABLE ||
	       d == CSS_DISPLAY_TABLE_ROW ||
	       d == CSS_DISPLAY_TABLE_ROW_GROUP ||
	       d == CSS_DISPLAY_TABLE_HEADER_GROUP ||
	       d == CSS_DISPLAY_TABLE_FOOTER_GROUP ||
	       d == CSS_DISPLAY_TABLE_CELL ||
	       d == CSS_DISPLAY_TABLE_CAPTION;
}

static bool has_any_border(const estyle *e)
{
	return e->bw[0] || e->bw[1] || e->bw[2] || e->bw[3];
}

/*
 * Move the item at `from` back to index `to`, shifting the rest along. Used to
 * put an inline element's background box IN FRONT of the text runs it belongs
 * behind: the runs are emitted first (the box cannot be sized until the content
 * is laid out) but main.c paints in item order, so a box left at the end would
 * paint over its own text.
 */
/*
 * overflow:hidden support (#245 engoflow). Neutralise an item in place: it is
 * NEVER removed from the array (removal would shift indices that ancestor walk
 * frames have already captured), it is just made to paint nothing.
 */
static void neutralise_item(layout_item *it)
{
	it->has_bg = 0;
	it->bw[0] = it->bw[1] = it->bw[2] = it->bw[3] = 0;
	it->w = 0;
	it->h = 0;
	it->underline = 0;
	it->text[0] = '\0';
}

/*
 * Clip items [start, n_items) (skipping skip_idx, the block's own bg/border
 * box) to the rect [x0,y0)-(x1,y1). Item-granularity, single-level:
 *   - box (kind 1) / image (kind 3): geometry intersected with the rect, so a
 *     partially-outside box/image is cropped. A border on a cut edge is
 *     redrawn at the cut (a known minor limitation of clipping without a real
 *     paint-time clip region).
 *   - text (kind 0): kept if its origin is inside the rect, else dropped. This
 *     flat-item model has no sub-glyph clip, so a run starting inside but
 *     running past the edge still spills by up to its own width (noted).
 * A fully-outside item of any kind is neutralised.
 */
static void overflow_clip(lstate *st, int start, int skip_idx,
		int x0, int y0, int x1, int y1)
{
	int i, n = st->out->n_items;
	for (i = start; i < n; i++) {
		layout_item *it;
		int ix0, iy0, ix1, iy1;
		if (i == skip_idx) continue;
		it = &st->out->items[i];
		ix0 = it->x;
		iy0 = it->y;
		if (it->kind == 0) {
			ix1 = ix0 + lmeasure(st, it->text, it->size,
					it->face, it->fstyle);
			iy1 = iy0 + it->size;
		} else {
			ix1 = ix0 + it->w;
			iy1 = iy0 + it->h;
		}
		if (ix1 <= x0 || ix0 >= x1 || iy1 <= y0 || iy0 >= y1) {
			neutralise_item(it);
			continue;
		}
		if (it->kind == 1 || it->kind == 3) {
			int nx0 = ix0 < x0 ? x0 : ix0;
			int ny0 = iy0 < y0 ? y0 : iy0;
			int nx1 = ix1 > x1 ? x1 : ix1;
			int ny1 = iy1 > y1 ? y1 : iy1;
			it->x = nx0;
			it->y = ny0;
			it->w = nx1 > nx0 ? nx1 - nx0 : 0;
			it->h = ny1 > ny0 ? ny1 - ny0 : 0;
		} else if (ix0 < x0 || ix0 >= x1 || iy0 < y0 || iy0 >= y1) {
			neutralise_item(it);
		}
	}
}

static void rotate_item_to(lstate *st, int from, int to)
{
	layout_item tmp;
	if (from <= to || from >= st->out->n_items || to < 0) return;
	tmp = st->out->items[from];
	memmove(&st->out->items[to + 1], &st->out->items[to],
			(size_t)(from - to) * sizeof(layout_item));
	st->out->items[to] = tmp;
}

static void fill_box_style(layout_item *bx, const estyle *e)
{
	int i;
	bx->has_bg = e->has_bg;
	bx->bg = e->bg;
	bx->radius = e->radius;
	for (i = 0; i < 4; i++) {
		bx->bw[i] = (uint8_t) e->bw[i];
		bx->bcol[i] = e->bcol[i];
	}
}

/* ---- recursive box walk ---- */
static void walk(lstate *st, dom_node *node, const css_computed_style *pstyle,
		const estyle *pe, int cb_x, int cb_w, int flex_row);
/* Table box generation and layout; see the long note above its definition. */
static void tbl_walk_children(lstate *st, dom_node *node,
		const css_computed_style *style, const css_computed_style *pstyle,
		const estyle *e, const estyle *pe, int content_x, int content_w,
		int box_idx);

/*
 * `flex_row` is set when the PARENT is a row-direction flex container, which
 * makes this element flex-level rather than block-level: it flows on the
 * current line instead of forcing a break.
 *
 * This is the whole of our flexbox support and it is worth being exact about
 * what it buys and what it does not. It buys the single most visible thing a
 * row flex container does, which is putting its children beside each other:
 * a button row, a tag-chip row, a window-frame title bar, an icon beside a
 * label. Combined with the inline wrap already in layout_text(), it even
 * approximates `flex-wrap: wrap` correctly, because a child that does not fit
 * moves to the next line. On a SINGLE-LINE row it now also does justify-content
 * and align-items (flex_align), flex-grow (flex_grow_pass), and flex-basis +
 * flex-shrink (flex_basis_pass / flex_shrink_pass, #245 engflexsize); a column
 * direction keeps block stacking, which is already right. A container that
 * wrapped to multiple lines falls back to plain content-width packing (every
 * distribution pass bails on wrap), and gap is handled via st->flex_gap.
 */
/*
 * #245 list markers. The renderer draws one glyph PER BYTE, so a marker must be
 * ASCII (ordered N./a./iv.) or a single Latin-1 byte (disc 0xb7, circle 0xb0),
 * or a drawn box (square). Ordered markers count preceding <li> siblings, which
 * is correct for the common single-level list; <ol start>/<li value> and
 * nested-list counter scoping are follow-up (see BROWSER_ENGINE_CSS_GAPS #2).
 */
static int list_ordinal(dom_node *node)
{
	int n = 1;
	dom_node *cur = node;
	int owned = 0;
	for (;;) {
		dom_node *prev = NULL;
		if (dom_node_get_previous_sibling(cur, &prev) != DOM_NO_ERR)
			prev = NULL;
		if (owned) dom_node_unref(cur);
		if (!prev) break;
		cur = prev; owned = 1;
		{
			dom_node_type t;
			if (dom_node_get_node_type(cur, &t) == DOM_NO_ERR &&
					t == DOM_ELEMENT_NODE && node_is(cur, "li"))
				n++;
		}
	}
	return n;
}

/* Append the decimal spelling of a non-negative int to buf at *o. */
static void list_put_decimal(char *buf, int *o, int v)
{
	char tmp[12]; int t = 0;
	if (v <= 0) { buf[(*o)++] = '0'; return; }
	while (v > 0 && t < (int) sizeof tmp) { tmp[t++] = (char) ('0' + v % 10); v /= 10; }
	while (t > 0) buf[(*o)++] = tmp[--t];
}

/* Bijective base-26 (a,b,..,z,aa,ab,..); `base` is 'a' or 'A'. */
static void list_put_alpha(char *buf, int *o, int v, char base)
{
	char tmp[8]; int t = 0;
	if (v <= 0) v = 1;
	while (v > 0 && t < (int) sizeof tmp) {
		int r = (v - 1) % 26;
		tmp[t++] = (char) (base + r);
		v = (v - 1) / 26;
	}
	while (t > 0) buf[(*o)++] = tmp[--t];
}

/* Roman numerals (1..3999); `upper` selects case. ASCII, bounded. */
static void list_put_roman(char *buf, int *o, int v, int upper)
{
	static const int val[] = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
	static const char *sym_l[] = {"m","cm","d","cd","c","xc","l","xl","x","ix","v","iv","i"};
	static const char *sym_u[] = {"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};
	int i;
	if (v <= 0 || v > 3999) { list_put_decimal(buf, o, v); return; }
	for (i = 0; i < 13; i++) {
		const char *sy = upper ? sym_u[i] : sym_l[i];
		while (v >= val[i]) {
			int k = 0;
			while (sy[k]) buf[(*o)++] = sy[k++];
			v -= val[i];
		}
	}
}

/*
 * Effective list-style-type for a list item. Prefers the CSS-computed value,
 * but this port's libcss does not compute list-style-type from author styles
 * (measured #245: the getter always returns DISC, so even list-style-type:none
 * never took effect). So when the getter is at its DISC/inherit default we
 * consult the HTML list element: an <ol> is decimal (or its `type` attribute
 * a/A/i/I), a <ul> is disc/circle/square per `type`. This lights up the common
 * ordered list without depending on the broken getter; a non-DISC getter (if
 * libcss is later fixed) wins here.
 */
static uint8_t list_resolve_type(dom_node *node, uint8_t css_lt)
{
	if (css_lt != CSS_LIST_STYLE_TYPE_DISC &&
			css_lt != CSS_LIST_STYLE_TYPE_INHERIT)
		return css_lt;
	dom_node *par = NULL;
	uint8_t lt = css_lt;
	if (dom_node_get_parent_node(node, &par) == DOM_NO_ERR && par) {
		char ty[16];
		if (node_is(par, "ol")) {
			get_attr(par, "type", ty, sizeof ty);
			if (ty[0] == 'a') lt = CSS_LIST_STYLE_TYPE_LOWER_ALPHA;
			else if (ty[0] == 'A') lt = CSS_LIST_STYLE_TYPE_UPPER_ALPHA;
			else if (ty[0] == 'i') lt = CSS_LIST_STYLE_TYPE_LOWER_ROMAN;
			else if (ty[0] == 'I') lt = CSS_LIST_STYLE_TYPE_UPPER_ROMAN;
			else lt = CSS_LIST_STYLE_TYPE_DECIMAL;
		} else if (node_is(par, "ul")) {
			get_attr(par, "type", ty, sizeof ty);
			if (ty[0] == 'c' || ty[0] == 'C')
				lt = CSS_LIST_STYLE_TYPE_CIRCLE;
			else if (ty[0] == 's' || ty[0] == 'S')
				lt = CSS_LIST_STYLE_TYPE_SQUARE;
			else lt = CSS_LIST_STYLE_TYPE_DISC;
		}
		dom_node_unref(par);
	}
	return lt;
}

static void walk(lstate *st, dom_node *node, const css_computed_style *pstyle,
		const estyle *pe, int cb_x, int cb_w, int flex_row)
{
	dom_node_type type;
	if (node == NULL) return;
	if (dom_node_get_node_type(node, &type) != DOM_NO_ERR) return;

	if (type == DOM_TEXT_NODE) {
		dom_string *txt = NULL;
		unsigned long long _t0 = lrdtsc();
		g_lp_ntext++;
		if (dom_node_get_text_content(node, &txt) == DOM_NO_ERR && txt) {
			unsigned long long _t1 = lrdtsc();
			g_lp_text += _t1 - _t0;
			layout_text(st, dom_string_data(txt),
					dom_string_byte_length(txt), pe);
			g_lp_flow += lrdtsc() - _t1;
			dom_string_unref(txt);
		} else {
			g_lp_text += lrdtsc() - _t0;
		}
		return;
	}
	if (type != DOM_ELEMENT_NODE)
		return;

	/* compute this element's style (inherits from pstyle) */
	unsigned long long _ts = lrdtsc();
	g_lp_nelem++;
	css_computed_style *style = mcs_compute_style(st->css,
			(dom_element *) node, pstyle);
	g_lp_style += lrdtsc() - _ts;
	estyle e;
	if (style == NULL) {
		/* fall back to parent style */
		e = *pe;
		e.has_bg = 0;
		e.margin_top = e.margin_bottom = 0;
		e.pad[0] = e.pad[1] = e.pad[2] = e.pad[3] = 0;
		e.bw[0] = e.bw[1] = e.bw[2] = e.bw[3] = 0;
		e.width = e.max_width = e.min_width = -1;
		e.height = -1;
		e.margin_left = e.margin_right = 0;
	} else {
		read_style(style, pe, cb_w, &e);
	}

	/*
	 * DOM white-space override (#245 engfix5). white-space inherits, so
	 * setting it on the element covers its text and descendants. Only promote
	 * FROM normal: a non-normal value already came from the computed getter
	 * (preferred) or from an inherited ancestor and must not be downgraded.
	 * <pre> and its historical kin preserve whitespace; <nobr> suppresses
	 * wrapping. The HTML `nowrap` attribute on table cells is deferred (see
	 * CHANGELOG). This is inert on every ordinary element and cannot move a
	 * page that authors no <pre>/<nobr>.
	 */
	if (e.white_space == WS_NORMAL) {
		if (node_is(node, "pre") || node_is(node, "xmp") ||
				node_is(node, "listing") || node_is(node, "plaintext"))
			e.white_space = WS_PRE;
		else if (node_is(node, "nobr"))
			e.white_space = WS_NOWRAP;
	}

	/*
	 * float side (#245 engfloat). Read straight from the computed getter; the
	 * property does not inherit, so a NULL style (the fallback path) means none.
	 * 0 none, 1 left, 2 right.
	 */
	int fl_side = 0;
	if (style) {
		uint8_t fv = css_computed_float(style);
		if (fv == CSS_FLOAT_LEFT) fl_side = 1;
		else if (fv == CSS_FLOAT_RIGHT) fl_side = 2;
	}

	if (e.display == CSS_DISPLAY_NONE) {
		if (style) css_computed_style_destroy(style);
		return;
	}

	/*
	 * Canvas background propagation. CSS gives the canvas <html>'s
	 * background, or <body>'s if <html> declares none. Without this the
	 * document is painted onto the browser's hardcoded white sheet, so a
	 * dark site renders as dark rectangles on a white page with white
	 * gutters and a white scrollbar trough. First one wins, so html beats
	 * body by virtue of being reached first.
	 */
	if (!st->out->has_doc_bg && e.has_bg &&
			(node_is(node, "html") || node_is(node, "body"))) {
		st->out->doc_bg = e.bg;
		st->out->has_doc_bg = 1;
	}

	/* Capture an <a href> (or any element's href) so descendant text runs are
	 * tagged with the link target. Restored after the subtree is walked. */
	char href_saved[LAYOUT_HREF_MAX];
	int href_changed = 0;
	{
		dom_string *an = NULL, *val = NULL;
		unsigned long long _ta = lrdtsc();
		if (dom_string_create((const uint8_t *) "href", 4, &an) == DOM_NO_ERR && an) {
			if (dom_element_get_attribute((dom_element *) node, an, &val) == DOM_NO_ERR && val) {
				size_t l = dom_string_byte_length(val);
				if (l > LAYOUT_HREF_MAX - 1) l = LAYOUT_HREF_MAX - 1;
				memcpy(href_saved, st->cur_href, LAYOUT_HREF_MAX);
				href_changed = 1;
				memcpy(st->cur_href, dom_string_data(val), l);
				st->cur_href[l] = 0;
				dom_string_unref(val);
			}
			dom_string_unref(an);
		}
		g_lp_attr += lrdtsc() - _ta;
	}

	/*
	 * Resolve this element's box against its containing block. Even for
	 * inline elements the horizontal padding and borders are used, which is
	 * what makes a pill button or a code span look like one.
	 */
	int ml = e.margin_left  < 0 ? 0 : e.margin_left;
	int mr = e.margin_right < 0 ? 0 : e.margin_right;
	int bl = e.bw[LB_LEFT],  br = e.bw[LB_RIGHT];
	int pl = e.pad[LB_LEFT], pr = e.pad[LB_RIGHT];
	int content_w;

	if (e.width >= 0)
		content_w = e.width;
	else
		content_w = cb_w - ml - mr - bl - br - pl - pr;
	/*
	 * An ATOMIC INLINE box (a flex item, an inline-block, an inline-flex) is
	 * shrink-to-fit, not stretch-to-fill, and its real used width is only
	 * known after its content is laid out.
	 *
	 * The cap is the WHOLE line box, not the room left on the current line.
	 * That distinction is what makes wrapping correct: an item laid out
	 * against the remaining room would re-flow differently once it moved to a
	 * line of its own, so the engine could not simply translate it down. Laid
	 * out against the full line, its internal layout is position-independent
	 * and moving it is exact.
	 */
	int atomic = flex_row ||
			e.display == CSS_DISPLAY_INLINE_BLOCK ||
			e.display == CSS_DISPLAY_INLINE_FLEX ||
			e.display == CSS_DISPLAY_INLINE_GRID;
	if (atomic && e.width < 0) {
		int room = st->line_right - st->line_left - ml - mr - bl - br - pl - pr;
		if (room > 0 && room < content_w) content_w = room;
	}
	if (e.min_width >= 0 && content_w < e.min_width) content_w = e.min_width;
	if (e.max_width >= 0 && content_w > e.max_width)
		content_w = e.max_width;
	if (content_w < 1) content_w = 1;

	int outer_w = content_w + pl + pr + bl + br;
	int box_x;
	if (e.margin_left < 0 && e.margin_right < 0) {
		/* margin: 0 auto - the standard centring idiom. */
		int extra = cb_w - outer_w;
		if (extra < 0) extra = 0;
		box_x = cb_x + extra / 2;
	} else if (e.margin_left < 0) {
		int extra = cb_w - outer_w - mr;
		if (extra < 0) extra = 0;
		box_x = cb_x + extra;
	} else {
		box_x = cb_x + ml;
	}
	int content_x = box_x + bl + pl;

	/*
	 * A hard line break. <br> has no default style that says so, and nothing
	 * here handled it, so "system<br>that" rendered as "systemthat": no
	 * break AND no space, because a break is not whitespace in the source.
	 */
	if (node_is(node, "br")) {
		/* A <br> is never out of flow, so it needs no rewind, and it is
		 * handled above where the rewind macro is not in scope yet. */
		line_break(st);
		if (href_changed) memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);
		if (style) css_computed_style_destroy(style);
		return;
	}

	/*
	 * The off-screen positioning idiom. `position: absolute; left: -9999px`
	 * is how a skip-to-content link is hidden from sight while staying
	 * reachable by keyboard and screen reader. We do not implement out-of-flow
	 * positioning, so laid out in flow it becomes the first visible thing on
	 * the page. Recognising this ONE shape is honest: an element positioned
	 * a thousand pixels or more off the edge was put there to be unseen.
	 * Everything else positioned absolutely is still laid out in flow.
	 */
	/*
	 * An out-of-flow element is laid out where it starts and then puts the
	 * pen back, so siblings do not get pushed down by it and two
	 * `position: absolute; inset: 0` layers land on top of each other.
	 * The extent is remembered so the containing block still grows to hold
	 * it. This MUST run on every exit from walk(): when it lived only at the
	 * bottom, the early returns for <img> and form controls skipped it, and
	 * the site's two stacked hero screenshots rendered one below the other.
	 */
	int out_of_flow = 0;
	/*
	 * position: relative (#245 engpos). The element stays in normal flow;
	 * only its painted items (and its whole subtree) shift by the offset at
	 * every exit from walk(). rel_start is captured before this element emits
	 * any item, so [rel_start, n_items) is exactly its own subtree. rel_shift
	 * stays 0 for a static page, so the shift loop below never runs and normal
	 * flow is byte-identical (AE=0) by construction.
	 */
	int rel_shift = 0, rel_dx = 0, rel_dy = 0;
	int rel_start = st->out->n_items;
	/*
	 * engabspos (#245). z_layer/z_key stamp this element's own items with a
	 * stacking key at every walk() exit (folded into OOF_REWIND, like the
	 * relative shift), so positioned boxes paint after in-flow content ordered
	 * by z-index. Both stay 0 for non-positioned content, so a static page is
	 * byte-identical. pcb_s* save the inherited positioned containing block for
	 * restore on exit. abs_place_x/y and abs_box_x/y carry the resolved
	 * top/left placement of an absolutely positioned box into the layout below.
	 */
	int z_layer = 0, z_key = 0;
	int is_positioned = 0;
	int pcb_sx = st->pcb_x, pcb_sy = st->pcb_y;
	int pcb_sw = st->pcb_w, pcb_sh = st->pcb_h;
	int abs_place_x = 0, abs_place_y = 0, abs_box_x = 0, abs_box_y = 0;
	int oof_save_y = st->cursor_y, oof_save_x = st->cursor_x;
	int oof_save_lh = st->line_height;
	bool oof_save_hc = st->line_has_content;
#define OOF_REWIND() do { \
	if (out_of_flow) { \
		if (st->cursor_y > st->oof_max_y) st->oof_max_y = st->cursor_y; \
		st->cursor_y = oof_save_y; \
		st->cursor_x = oof_save_x; \
		st->line_height = oof_save_lh; \
		st->line_has_content = oof_save_hc; \
	} \
	if (rel_shift) { \
		int _ri; \
		for (_ri = rel_start; _ri < st->out->n_items; _ri++) { \
			st->out->items[_ri].x += rel_dx; \
			st->out->items[_ri].y += rel_dy; \
		} \
	} \
	if (z_layer) { \
		int _zi; \
		for (_zi = rel_start; _zi < st->out->n_items; _zi++) \
			if (st->out->items[_zi].zorder == 0) \
				st->out->items[_zi].zorder = z_key; \
	} \
	st->pcb_x = pcb_sx; st->pcb_y = pcb_sy; \
	st->pcb_w = pcb_sw; st->pcb_h = pcb_sh; \
} while (0)
	if (style) {
		uint8_t pos = css_computed_position(style);
		if (pos == CSS_POSITION_ABSOLUTE || pos == CSS_POSITION_FIXED) {
			css_fixed ov; css_unit ou;
			int off = 0;
			if (css_computed_left(style, &ov, &ou) == CSS_LEFT_SET &&
					fixed_px(ov, ou, e.font_size, cb_w) <= -1000)
				off = 1;
			if (css_computed_top(style, &ov, &ou) == CSS_TOP_SET &&
					fixed_px(ov, ou, e.font_size, cb_w) <= -1000)
				off = 1;
			if (off) {
				if (href_changed) memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);
				css_computed_style_destroy(style);
				return;
			}
			out_of_flow = 1;
			is_positioned = 1;
			/*
			 * engabspos (#245): resolve this box against its containing block
			 * (the nearest positioned ancestor's padding box in st->pcb_*, else
			 * the ICB). left/right resolve against the CB width, top/bottom
			 * against its height; left wins over right and top over bottom.
			 * right needs the box width (outer_w, known) and bottom needs the CB
			 * height AND the box height, so bottom is applied only when this box
			 * has an explicit height. Neither offset present leaves the box at
			 * its static position (the previous behaviour). The offsets are
			 * carried to the block-open / img-emit below, where they override the
			 * static origin before any item is emitted.
			 */
			{
				int cbx = st->pcb_x, cbw = st->pcb_w;
				int cby = st->pcb_y, cbh = st->pcb_h;
				int abs_h = (e.height >= 0)
					? e.height + e.pad[LB_TOP] + e.pad[LB_BOTTOM]
						+ e.bw[LB_TOP] + e.bw[LB_BOTTOM]
					: -1;
				css_fixed pv; css_unit pu;
				if (css_computed_left(style, &pv, &pu) == CSS_LEFT_SET) {
					abs_box_x = cbx + fixed_px(pv, pu, e.font_size, cbw);
					abs_place_x = 1;
				} else if (css_computed_right(style, &pv, &pu) == CSS_RIGHT_SET) {
					abs_box_x = cbx + cbw -
						fixed_px(pv, pu, e.font_size, cbw) - outer_w;
					abs_place_x = 1;
				}
				if (css_computed_top(style, &pv, &pu) == CSS_TOP_SET) {
					abs_box_y = cby + fixed_px(pv, pu, e.font_size, cbh);
					abs_place_y = 1;
				} else if (css_computed_bottom(style, &pv, &pu) ==
						CSS_BOTTOM_SET && cbh >= 0 && abs_h >= 0) {
					abs_box_y = cby + cbh -
						fixed_px(pv, pu, e.font_size, cbh) - abs_h;
					abs_place_y = 1;
				}
			}
			{
				int32_t zz = 0;
				int zc = (css_computed_z_index(style, &zz) ==
						CSS_Z_INDEX_SET) ? (int) zz : 0;
				if (zc > 1000000) zc = 1000000;
				if (zc < -1000000) zc = -1000000;
				z_key = ZORDER_BASE + zc;
				z_layer = 1;
			}
		} else if (pos == CSS_POSITION_RELATIVE) {
			is_positioned = 1;
			/*
			 * engabspos (#245): a relative box is a containing block for its
			 * absolute descendants, and with an EXPLICIT z-index it also joins
			 * the positioned paint layer. z-index:auto (the default, and what
			 * every existing relative page uses) leaves z_layer 0, so those
			 * pages stay byte-identical (AE=0).
			 */
			{
				int32_t rzz = 0;
				if (css_computed_z_index(style, &rzz) == CSS_Z_INDEX_SET) {
					int rzc = (int) rzz;
					if (rzc > 1000000) rzc = 1000000;
					if (rzc < -1000000) rzc = -1000000;
					z_key = ZORDER_BASE + rzc;
					z_layer = 1;
				}
			}
			/*
			 * Relative offsets shift paint only (CSS 2.1 9.4.3). left wins
			 * over right, top over bottom (LTR). left/right percentages
			 * resolve against the containing-block width; top/bottom
			 * percentages need the CB height, which this single-pass engine
			 * does not track, so they resolve to 0 (px/em, the common case,
			 * are exact).
			 */
			css_fixed ov; css_unit ou;
			if (css_computed_left(style, &ov, &ou) == CSS_LEFT_SET) {
				rel_dx = fixed_px(ov, ou, e.font_size, cb_w);
				rel_shift = 1;
			} else if (css_computed_right(style, &ov, &ou) == CSS_RIGHT_SET) {
				rel_dx = -fixed_px(ov, ou, e.font_size, cb_w);
				rel_shift = 1;
			}
			if (css_computed_top(style, &ov, &ou) == CSS_TOP_SET) {
				rel_dy = fixed_px(ov, ou, e.font_size, -1);
				rel_shift = 1;
			} else if (css_computed_bottom(style, &ov, &ou) == CSS_BOTTOM_SET) {
				rel_dy = -fixed_px(ov, ou, e.font_size, -1);
				rel_shift = 1;
			}
		}
	}

	/* Inline image: reserve a box; the browser fetches + decodes + blits it. */
	if (node_is(node, "img")) {
		char src[LAYOUT_HREF_MAX]; get_attr(node, "src", src, sizeof src);
		if (src[0]) {
			char ws[12], hs[12];
			get_attr(node, "width", ws, sizeof ws);
			get_attr(node, "height", hs, sizeof hs);
			int iw = 0, ih = 0;
			for (int i = 0; ws[i] >= '0' && ws[i] <= '9'; i++) iw = iw * 10 + (ws[i] - '0');
			for (int i = 0; hs[i] >= '0' && hs[i] <= '9'; i++) ih = ih * 10 + (hs[i] - '0');
			/*
			 * Sizing precedence: a CSS width wins over the HTML
			 * attribute, and an aspect ratio taken from the two HTML
			 * attributes is preserved when only one dimension is
			 * known. The browser corrects both against the decoded
			 * image's real dimensions once it has them.
			 */
			int bw2, bh2;
			if (e.width >= 0) {
				bw2 = e.width;
				bh2 = (iw > 0 && ih > 0) ? (bw2 * ih) / iw : (ih > 0 ? ih : (bw2 * 3) / 4);
			} else {
				bw2 = iw > 0 ? iw : 0;
				bh2 = ih > 0 ? ih : 0;
			}
			if (bw2 <= 0) bw2 = content_w > 0 ? content_w : 120;
			if (bh2 <= 0) bh2 = (bw2 * 3) / 4;
			if (bw2 > content_w) {
				/* max-width: 100% is in essentially every UA and
				 * author sheet; scale, do not clip. */
				if (bh2 > 0 && bw2 > 0) bh2 = (bh2 * content_w) / bw2;
				bw2 = content_w;
			}
			if (bh2 > 1200) bh2 = 1200;
			/*
			 * An image inside a row flex container is a flex ITEM: it
			 * sits beside its siblings. Breaking the line unconditionally
			 * put the site's 30px brand mark on its own line and dropped
			 * the wordmark on top of the nav below it.
			 */
			/*
			 * float (#245 engfloat / engfloat2). The commonest float by far is
			 * `img { float: left }` wrapping text. The image is placed beside any
			 * floats already open (packing left-to-right / right-to-left), a band
			 * is opened, and the pen is left BESIDE it so the following
			 * text/blocks flow alongside. With no float already open this reduces
			 * to the old single-float placement exactly. A float beyond FL_MAX
			 * falls through to normal flow, as a second float used to.
			 */
			if ((fl_side == 1 || fl_side == 2) && !flex_row &&
					st->fl_n < FL_MAX && bw2 > 0) {
				int cont_left = st->line_left, cont_right = st->line_right;
				int fml = ml, fmr = mr;
				int mbw = fml + bw2 + fmr;
				int ftop, fy, fx, ox0, ox1;
				line_break(st);
				ftop = st->cursor_y;
				if (st->fl_n == 0) {
					st->fl_cx0 = cont_left;
					st->fl_cx1 = cont_right;
				}
				fy = fl_fit(st, fl_side, ftop, mbw, &ox0, &ox1);
				if (fl_side == 1) {
					fx = ox0 + fml;
				} else {
					fx = ox0 + fml;
					if (fx < st->fl_cx0) fx = st->fl_cx0;
				}
				{
					layout_item *bx = e.visibility ? NULL : item_new(st);
					if (bx) {
						bx->kind = 3;
						bx->x = fx; bx->y = fy;
						bx->w = bw2; bx->h = bh2;
						int k = 0;
						while (src[k] && k < LAYOUT_HREF_MAX - 1) { bx->href[k] = src[k]; k++; }
						bx->href[k] = 0;
					}
				}
				fl_add(st, fl_side, ox0, ox1, fy + bh2 + 4);
				st->cursor_y = fy;
				fl_recompute(st, fy);
				st->cursor_x = st->line_left;
				st->line_has_content = false;
				st->line_height = 0;
				st->pending_space = false;
				st->line_first = st->out->n_items;
				OOF_REWIND();
				if (href_changed) memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);
				if (style) css_computed_style_destroy(style);
				return;
			}
			int img_x;
			if (flex_row) {
				if (st->line_has_content && st->cursor_x + bw2 > st->line_right)
					line_break(st);
				img_x = st->cursor_x;
			} else {
				line_break(st);
				img_x = content_x;
				/* engabspos (#245): place an absolutely positioned image at its
				 * containing-block offset, gated so a normal image is unchanged. */
				if (abs_place_x) img_x = abs_box_x;
				if (abs_place_y) st->cursor_y = abs_box_y;
			}
			{
				layout_item *bx = e.visibility ? NULL : item_new(st);
				if (bx) {
					bx->kind = 3;
					bx->x = img_x; bx->y = st->cursor_y;
					bx->w = bw2; bx->h = bh2;
					int k = 0;
					while (src[k] && k < LAYOUT_HREF_MAX - 1) { bx->href[k] = src[k]; k++; }
					bx->href[k] = 0;
				}
			}
			if (flex_row) {
				st->cursor_x += bw2;
				st->line_has_content = true;
				if (bh2 > st->line_height) st->line_height = bh2;
			} else {
				st->cursor_y += bh2 + 4;
				st->cursor_x = st->line_left;
				st->line_first = st->out->n_items;
			}
		}
		OOF_REWIND();
		if (href_changed) memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);
		if (style) css_computed_style_destroy(style);
		return;
	}

	/* Form controls: render a field/button box with its placeholder or value,
	 * then skip the subtree (we don't lay out their shadow DOM). */
	if (node_is(node, "input") || node_is(node, "textarea") ||
			node_is(node, "select") || node_is(node, "button")) {
		int is_btn = node_is(node, "button") || 0;
		char ty[24]; get_attr(node, "type", ty, sizeof ty);
		if ((ty[0] == 's' && ty[1] == 'u') || (ty[0] == 'b' && ty[1] == 'u'))
			is_btn = 1;   /* type=submit / type=button */
		if (ty[0] == 'h' && ty[1] == 'i') {   /* type=hidden: render nothing */
			OOF_REWIND();
			if (href_changed) memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);
			if (style) css_computed_style_destroy(style);
			return;
		}
		line_break(st);
		int fh = e.font_size + 10;
		int fw = is_btn ? 130 : 260;
		if (e.width >= 0) fw = e.width;
		if (fw > content_w) fw = content_w;
		char fname[64]; get_attr(node, "name", fname, sizeof fname);
		char faction[LAYOUT_HREF_MAX]; get_form_action(node, faction, sizeof faction);
		{
			layout_item *bx = e.visibility ? NULL : item_new(st);
			if (bx) {
				bx->kind = 1; bx->x = content_x; bx->y = st->cursor_y;
				bx->w = fw; bx->h = fh;
				bx->has_bg = 1; bx->bg = is_btn ? 0x00E2E2E2u : 0x00FFFFFFu;
				for (int i = 0; i < 4; i++) { bx->bw[i] = 1; bx->bcol[i] = 0x00909090u; }
				bx->form_kind = is_btn ? 2 : 1;
				{ int i = 0; while (fname[i] && i < 63) { bx->field_name[i] = fname[i]; i++; } bx->field_name[i] = 0; }
				{ int i = 0; while (faction[i] && i < LAYOUT_HREF_MAX - 1) { bx->href[i] = faction[i]; i++; } bx->href[i] = 0; }
			}
		}
		char label[96];
		get_attr(node, "placeholder", label, sizeof label);
		if (!label[0]) get_attr(node, "value", label, sizeof label);
		/*
		 * A <button>'s label is its CHILD CONTENT, not an attribute. The
		 * subtree is skipped here (we do not lay out a control's innards),
		 * so it has to be read explicitly. Without this every <button>
		 * without a value= read "Submit": the site's responsive nav toggle
		 * is <button>&#8801;</button> and rendered as a Submit box.
		 */
		if (!label[0] && node_is(node, "button")) {
			dom_string *txt = NULL;
			if (dom_node_get_text_content(node, &txt) == DOM_NO_ERR && txt) {
				const char *d = dom_string_data(txt);
				size_t l = dom_string_byte_length(txt);
				size_t a = 0, b = l;
				while (a < b && (d[a] == ' ' || d[a] == '\n' || d[a] == '\t' || d[a] == '\r')) a++;
				while (b > a && (d[b-1] == ' ' || d[b-1] == '\n' || d[b-1] == '\t' || d[b-1] == '\r')) b--;
				if (b > a) {
					int n2 = (int)(b - a);
					if (n2 > (int) sizeof(label) - 1) n2 = (int) sizeof(label) - 1;
					utf8_squash(d + a, n2, label, sizeof label);
				}
				dom_string_unref(txt);
			}
		}
		if (!label[0] && is_btn) { label[0] = 'S'; label[1] = 'u'; label[2] = 'b';
			label[3] = 'm'; label[4] = 'i'; label[5] = 't'; label[6] = 0; }
		if (label[0]) {
			char tmp[96];
			utf8_squash(label, lz_len(label), tmp, sizeof tmp);
			memcpy(label, tmp, sizeof label);
		}
		if (label[0] && !e.visibility) {
			st->cursor_x = content_x + 6;
			emit_run(st, label, lz_len(label), e.font_size,
					is_btn ? 0x00202020u : 0x00555555u, 0, 0, 0,
					e.face, e.fstyle);
		}
		st->cursor_x = st->line_left;
		st->cursor_y += fh + 4;
		OOF_REWIND();
		if (href_changed) memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);
		if (style) css_computed_style_destroy(style);
		return;
	}

	/*
	 * ATOMIC INLINES (#245). This used to read `display_is_block(e.display) &&
	 * !flex_row`, which made a flex item INLINE-LEVEL, and that is wrong in a
	 * way that is easy to see and was hard to attribute:
	 *
	 *   - An inline element does not establish its own line context, so a
	 *     BLOCK child inside a flex item called line_break() against the
	 *     CONTAINER's line, moving the container's pen down and left. The
	 *     site's advisory stat tiles (a flex row of boxes, each holding a
	 *     block number over a block label) rendered as a diagonal staircase of
	 *     overlapping boxes, one step down and right per tile.
	 *   - An inline element contributed only its LINE HEIGHT to the line, not
	 *     its padding and borders, so a padded pill overlapped whatever came
	 *     after it. The site's chip row overlapped itself by 18px per row and
	 *     its status badges were painted through by the paragraph below them.
	 *
	 * A flex item, an inline-block and an inline-flex are all the same thing:
	 * a box that lays its INSIDE out as a block and takes part in its parent's
	 * line as a single unit. That is what `atomic` is, and both faults are one
	 * fault once it exists.
	 */
	bool is_block = display_is_block(e.display) || atomic;

	/*
	 * A block float (#245 engfloat). Requires a declared width: an auto-width
	 * float would fill its container and leave nothing to wrap beside, and
	 * shrink-to-fit is out of scope for this bounded step. Single float only
	 * (fl_n < FL_MAX). It lays out through the ordinary block path and is turned
	 * into a float at block close.
	 */
	int is_float = (fl_side == 1 || fl_side == 2) && is_block && !atomic &&
			!flex_row && st->fl_n < FL_MAX && e.width >= 0;

	/*
	 * Does this element establish a TABLE? The trigger deliberately is not
	 * `display: table` alone: maytera.net styles its own requirements table
	 * `.prose table { display: block; overflow-x: auto }`, so the <table>
	 * element is a BLOCK and only its <tbody>/<tr>/<td> are table-internal.
	 * CSS 2.1 17.2.1 still lays that out as a table via anonymous box
	 * generation. See the note above tbl_walk_children().
	 */
	int table_ish = is_block &&
			(node_is(node, "table") ||
			 e.display == CSS_DISPLAY_TABLE ||
			 e.display == CSS_DISPLAY_INLINE_TABLE ||
			 e.display == CSS_DISPLAY_TABLE_ROW_GROUP ||
			 e.display == CSS_DISPLAY_TABLE_HEADER_GROUP ||
			 e.display == CSS_DISPLAY_TABLE_FOOTER_GROUP);

	/* Does THIS element make its children flex-level? */
	int child_flex_row = 0;
	if (style && (e.display == CSS_DISPLAY_FLEX ||
			e.display == CSS_DISPLAY_INLINE_FLEX)) {
		uint8_t dir = css_computed_flex_direction(style);
		if (dir == CSS_FLEX_DIRECTION_ROW || dir == CSS_FLEX_DIRECTION_ROW_REVERSE ||
				dir == CSS_FLEX_DIRECTION_INHERIT)
			child_flex_row = 1;
	}

	/*
	 * Grid tracks, arriving as multi-column properties. cssvar.c reduces
	 * `grid-template-columns` to a column count or a minimum column width
	 * because libcss has no grid properties at all and carrying them through
	 * a pinned third-party tree would be a fork. Read the header comment
	 * there before changing either side of this.
	 */
	int grid_cols = 0;
	int track_gap = 0;
	if (style) {
		css_fixed gv; css_unit gu;
		if (css_computed_column_gap(style, &gv, &gu) == CSS_COLUMN_GAP_SET) {
			track_gap = fixed_px(gv, gu, e.font_size, content_w);
			if (track_gap < 0) track_gap = 0;
			if (track_gap > 200) track_gap = 200;
		}
		if (e.display == CSS_DISPLAY_GRID) {
			int32_t cc = 0;
			css_fixed cw; css_unit cu;
			/*
			 * css_computed_column_count() declares `int32_t *` and hands
			 * back a css_fixed. The select handler stores the bytecode
			 * NUMBER verbatim (libcss src/select/properties/column_count.c
			 * keeps it in a `css_fixed count`), so `column-count: 2`
			 * arrives as 2048, not 2. Taking it at face value made every
			 * grid saturate at the column cap and produced 75px columns.
			 * The signature says integer; the value is not one.
			 */
			if (css_computed_column_count(style, &cc) == CSS_COLUMN_COUNT_SET &&
					cc > 0) {
				grid_cols = FIXTOINT((css_fixed) cc);
			} else if (css_computed_column_width(style, &cw, &cu) ==
					CSS_COLUMN_WIDTH_SET) {
				int minw = fixed_px(cw, cu, e.font_size, content_w);
				if (minw > 0) grid_cols = (content_w + track_gap) / (minw + track_gap);
			}
			if (grid_cols < 1) grid_cols = 1;
			if (grid_cols > 8) grid_cols = 8;
		}
	}

	/* saved line context, restored for inline elements */
	int saved_left = st->line_left, saved_right = st->line_right;
	int blk_items_start = st->out->n_items;
	int saved_align = st->line_align;
	int box_idx = -1;
	int bgimg_idx = -1;   /* #245 engbgimg: this block's background-image item, or -1 */
	int grad_idx = -1;    /* #245 enggrad: this block's gradient paint item, or -1 */
	int box_start_y = 0;
	int inline_start_x = st->cursor_x, inline_start_y = st->cursor_y;
	int inline_first = st->out->n_items;

	/* Atomic-inline bookkeeping: where the parent's line was when this box
	 * started, so the pen can be put back on it afterwards. */
	int ai_line_y = st->cursor_y;
	int ai_line_h = st->line_height;
	bool ai_line_had = st->line_has_content;
	int ai_first = st->out->n_items;

	if (is_block) {
		if (atomic) {
			/* No line break: an atomic inline JOINS the current line. The
			 * gap belongs between siblings, so it is spent only when
			 * something is already there. */
			if (st->line_has_content && st->flex_gap > 0)
				st->cursor_x += st->flex_gap;
			box_x = st->cursor_x + ml;
			content_x = box_x + bl + pl;
			ai_line_y = st->cursor_y;
		} else {
			line_break(st);
			st->cursor_y += e.margin_top;
			/*
			 * clear (#245 engfloat): a block that clears the active float side
			 * drops below the float before it is placed. Inert when no float is
			 * active or clear is none.
			 */
			if (st->fl_n > 0) {
				uint8_t clr = style ? css_computed_clear(style) : CSS_CLEAR_NONE;
				int target = st->cursor_y, i;
				for (i = 0; i < st->fl_n; i++) {
					int match = (clr == CSS_CLEAR_BOTH) ||
						(clr == CSS_CLEAR_LEFT && st->fl_bside[i] == 1) ||
						(clr == CSS_CLEAR_RIGHT && st->fl_bside[i] == 2);
					if (match && st->fl_bbot[i] > target)
						target = st->fl_bbot[i];
				}
				if (target > st->cursor_y) {
					st->cursor_y = target;
					fl_compact(st, st->cursor_y);
					fl_recompute(st, st->cursor_y);
				}
			}
		}
		box_start_y = st->cursor_y;
		/*
		 * engabspos (#245): an absolutely positioned block is laid out at its
		 * containing-block offset, not at the static pen. Override the box
		 * origin HERE, before the decorative box is emitted and before
		 * content_x/line_left/right derive from it, so the whole subtree lands
		 * at (abs_box_x, abs_box_y). An axis left unresolved keeps its static
		 * value. Gated on abs_place_*, only ever set for an absolute box.
		 */
		if (abs_place_x) { box_x = abs_box_x; content_x = box_x + bl + pl; }
		if (abs_place_y) { st->cursor_y = abs_box_y; box_start_y = abs_box_y; }
		if ((e.has_bg || has_any_border(&e)) && !e.visibility) {
			layout_item *bx = item_new(st);
			if (bx) {
				box_idx = st->out->n_items - 1;
				bx->kind = 1;
				bx->x = box_x; bx->y = box_start_y;
				bx->w = outer_w; bx->h = 0;
				fill_box_style(bx, &e);
			}
		}
		/*
		 * background-image (#245 engbgimg). A raster url() painted as this
		 * block's background: top-left, no-repeat, scaled to FIT the padding
		 * box (contain; aspect preserved by the shared decoder), behind all
		 * content. Emitted as an ordinary image item (kind 3) so it reuses the
		 * browser's <img> fetch + decode + blit path VERBATIM: no new image
		 * loader and no new paint mode. Sized to the PADDING box so a border
		 * (drawn by the kind-1 box above, which paints first) is not
		 * overpainted; x/y are known now, w/h are backfilled at block close
		 * like box_idx. GATED on a non-NULL url, so a block with no
		 * background-image emits nothing new and the item stream is
		 * byte-identical (AE=0). The distinguishing test is the url POINTER,
		 * not the return code (NONE and IMAGE share a value in this libcss
		 * build). url() gradients are a FUNCTION token this parser rejects and
		 * never reach here. DEFERRED: background-repeat tiling, -position,
		 * -size beyond contain-to-box, and background-clip/origin nuance.
		 */
		if (style && !e.visibility && !st->measuring) {
			lwc_string *bgurl = NULL;
			css_computed_background_image(style, &bgurl);
			if (bgurl != NULL && lwc_string_length(bgurl) > 0) {
				const char *u = lwc_string_data(bgurl);
				layout_item *bg = item_new(st);
				if (bg) {
					size_t k = 0, ul = lwc_string_length(bgurl);
					bgimg_idx = st->out->n_items - 1;
					bg->kind = 3;
					bg->x = box_x + e.bw[LB_LEFT];
					bg->y = box_start_y + e.bw[LB_TOP];
					bg->w = 0; bg->h = 0;
					while (k < ul && k < (size_t)(LAYOUT_HREF_MAX - 1)) {
						bg->href[k] = u[k]; k++;
					}
					bg->href[k] = 0;
				}
			}
		}
		/*
		 * background gradient (#245 enggrad). cssvar.c rewrote a
		 * `background`/`background-image` gradient onto the outline-color
		 * carrier: the computed outline-color, when it has the 0xC5 marker
		 * byte in its red channel, encodes an index into cssvar's gradient
		 * table (0xC5<index16>). Emit a kind-4 gradient paint item at the
		 * padding box (behind content, in front of the border box which paints
		 * first), sized like bgimg and backfilled at block close. The gradient
		 * table index rides in `bg`; main.c reads it and rasterises the ramp.
		 * GATED on the marker byte AND a valid table index, so any page that
		 * sets no gradient (outline-color default is `invert`, which never
		 * returns COLOR) emits nothing new and is byte-identical (AE=0).
		 */
		if (style && !e.visibility && !st->measuring) {
			css_color oc = 0;
			if (css_computed_outline_color(style, &oc) ==
					CSS_OUTLINE_COLOR_COLOR) {
				unsigned int v = (unsigned int) oc & 0x00FFFFFFu;
				if ((v >> 16) == 0xC5u) {
					int gi = (int) (v & 0xFFFFu);
					if (gi >= 0 && gi < cssvar_gradient_count()) {
						layout_item *gr = item_new(st);
						if (gr) {
							grad_idx = st->out->n_items - 1;
							gr->kind = 4;
							gr->x = box_x + e.bw[LB_LEFT];
							gr->y = box_start_y + e.bw[LB_TOP];
							gr->w = 0; gr->h = 0;
							gr->bg = (uint32_t) gi;
						}
					}
				}
			}
		}
		st->cursor_y += e.bw[LB_TOP] + e.pad[LB_TOP];
		st->line_left = content_x;
		st->line_right = content_x + content_w;
		st->cursor_x = content_x;
		/*
		 * engabspos (#245): a positioned block establishes the containing
		 * block for its absolutely positioned descendants: its PADDING box.
		 * pcb_h is known only when the block has an explicit height (an auto
		 * block is still growing), so bottom placement inside it applies only
		 * then. Restored to the inherited CB at every walk() exit (OOF_REWIND).
		 * A non-positioned block leaves st->pcb_* untouched, so descendants
		 * resolve against the nearest positioned ancestor / ICB.
		 */
		if (is_positioned && !atomic) {
			st->pcb_x = box_x + e.bw[LB_LEFT];
			st->pcb_y = box_start_y + e.bw[LB_TOP];
			st->pcb_w = content_w + e.pad[LB_LEFT] + e.pad[LB_RIGHT];
			st->pcb_h = (e.height >= 0)
				? e.height + e.pad[LB_TOP] + e.pad[LB_BOTTOM]
				: -1;
		}
		/*
		 * float (#245 engfloat): a following block still within an active
		 * float's band has its LINE boxes shortened so its text wraps beside
		 * the float (the block's own border box is intentionally left full
		 * width, which is what CSS 2.1 9.5 specifies). Inert unless a float is
		 * active. is_float is excluded so the float itself is never shifted.
		 */
		if (st->fl_n > 0 && !is_float) {
			int i;
			for (i = 0; i < st->fl_n; i++) {
				if (st->cursor_y >= st->fl_bbot[i]) continue;
				if (st->fl_bside[i] == 1) {
					if (st->fl_bx1[i] > st->line_left) {
						st->line_left = st->fl_bx1[i];
						st->cursor_x = st->fl_bx1[i];
					}
				} else {
					if (st->fl_bx0[i] < st->line_right)
						st->line_right = st->fl_bx0[i];
				}
			}
			if (st->line_left > st->line_right)
				st->line_left = st->line_right;
		}
		/*
		 * text-indent (#245 engfix6): shift only the FIRST line's start.
		 * line_left stays at content_x, so line_break() resets later lines
		 * to the margin, indenting the first line alone per CSS. Sign-correct
		 * (negative pulls left); a 0 indent adds nothing (inert).
		 */
		if (e.text_indent)
			st->cursor_x += e.text_indent;
		st->line_height = 0;
		st->line_has_content = false;
		st->pending_space = false;
		st->line_align = atomic ? 0 : e.text_align;
		st->line_first = st->out->n_items;
	} else {
		/*
		 * Inline box. Horizontal padding and borders move the pen; the
		 * decorative box, if any, is emitted after the children so it can
		 * be sized to what they actually occupied.
		 */
		if (flex_row && st->line_has_content && st->flex_gap > 0)
			st->cursor_x += st->flex_gap;
		st->cursor_x += ml + bl + pl;
		inline_start_x = st->cursor_x - bl - pl;
		inline_start_y = st->cursor_y;
		inline_first = st->out->n_items;
		/*
		 * A DECORATED INLINE OCCUPIES ITS PADDING AND BORDERS, not just its
		 * line height. Reserving only the line height is what let a padded
		 * badge be painted through by the paragraph on the next line: the box
		 * is drawn `line_height + padding + borders` tall and the line was
		 * only stepping by `line_height`.
		 */
		if (e.has_bg || has_any_border(&e)) {
			int ih = e.line_height + e.pad[LB_TOP] + e.pad[LB_BOTTOM]
				+ e.bw[LB_TOP] + e.bw[LB_BOTTOM];
			if (ih > st->line_height) st->line_height = ih;
		}
	}

	/* list-item marker (#245): number/letter/roman for ordered lists,
	 * disc/circle/square for unordered; none is handled above via list_none. */
	if (e.display == CSS_DISPLAY_LIST_ITEM && !e.list_none && !e.visibility) {
		char mk[24]; int mkn = 0; int drew_box = 0;
		uint8_t lt = list_resolve_type(node, e.list_type);
		switch (lt) {
		case CSS_LIST_STYLE_TYPE_DECIMAL:
		case CSS_LIST_STYLE_TYPE_DECIMAL_LEADING_ZERO:
			list_put_decimal(mk, &mkn, list_ordinal(node)); mk[mkn++] = '.';
			break;
		case CSS_LIST_STYLE_TYPE_LOWER_ALPHA:
		case CSS_LIST_STYLE_TYPE_LOWER_LATIN:
			list_put_alpha(mk, &mkn, list_ordinal(node), 'a'); mk[mkn++] = '.';
			break;
		case CSS_LIST_STYLE_TYPE_UPPER_ALPHA:
		case CSS_LIST_STYLE_TYPE_UPPER_LATIN:
			list_put_alpha(mk, &mkn, list_ordinal(node), 'A'); mk[mkn++] = '.';
			break;
		case CSS_LIST_STYLE_TYPE_LOWER_ROMAN:
			list_put_roman(mk, &mkn, list_ordinal(node), 0); mk[mkn++] = '.';
			break;
		case CSS_LIST_STYLE_TYPE_UPPER_ROMAN:
			list_put_roman(mk, &mkn, list_ordinal(node), 1); mk[mkn++] = '.';
			break;
		case CSS_LIST_STYLE_TYPE_CIRCLE:
			mk[mkn++] = (char) 0xb0;   /* Latin-1 DEGREE SIGN: hollow ring */
			break;
		case CSS_LIST_STYLE_TYPE_SQUARE:
			drew_box = 1;
			break;
		case CSS_LIST_STYLE_TYPE_DISC:
		default:
			/* U+2022 folded to Latin-1 MIDDLE DOT: the renderer is byte-based,
			 * so a UTF-8 bullet would draw as three separate glyphs. Unchanged
			 * from the original so a default disc list is byte-identical. */
			mk[mkn++] = (char) 0xb7;
			break;
		}
		if (drew_box) {
			/* No Latin-1 filled square exists; draw a small box, vertically
			 * centred on the line, in the item's text colour. */
			int sq = e.font_size / 2; if (sq < 4) sq = 4;
			layout_item *bx = item_new(st);
			if (bx) {
				bx->kind = 1;
				bx->x = st->cursor_x;
				bx->y = st->cursor_y + (e.font_size - sq) / 2;
				bx->w = sq; bx->h = sq;
				bx->bg = e.color & 0xffffffu; bx->has_bg = 1;
			}
			st->cursor_x += lmeasure(st, "\xb7 ", e.font_size, e.face, e.fstyle);
		} else {
			emit_run(st, mk, mkn, e.font_size, e.color, 0, 0, 0,
					e.face, e.fstyle);
			/* advance past the marker plus one trailing space */
			mk[mkn] = ' '; mk[mkn + 1] = '\0';
			st->cursor_x += lmeasure(st, mk, e.font_size, e.face, e.fstyle);
		}
		st->line_has_content = true;
		if (e.line_height > st->line_height)
			st->line_height = e.line_height;
	}

	/* children */
	int oof_outer = st->oof_max_y;
	int gap_outer = st->flex_gap;
	/* float (#245 engfloat2): floats opened by a child affect this block's
	 * later children but must not leak to this block's siblings, so the whole
	 * band set is snapshotted here and restored after the child loop. The arrays
	 * are only copied when a float is actually active on entry, so a float-free
	 * page pays nothing. fl_s_seq marks the seq allocator so floats opened by
	 * THIS block's children can be told apart from pre-existing ones (which the
	 * block already flows beside and must not grow to contain). */
	int fl_s_n = st->fl_n, fl_s_cx0 = st->fl_cx0, fl_s_cx1 = st->fl_cx1;
	int fl_s_seq = st->fl_next_seq;
	int8_t fl_s_side[FL_MAX];
	int fl_s_x0[FL_MAX], fl_s_x1[FL_MAX], fl_s_bot[FL_MAX], fl_s_bseq[FL_MAX];
	if (fl_s_n > 0) {
		memcpy(fl_s_side, st->fl_bside, sizeof fl_s_side);
		memcpy(fl_s_x0, st->fl_bx0, sizeof fl_s_x0);
		memcpy(fl_s_x1, st->fl_bx1, sizeof fl_s_x1);
		memcpy(fl_s_bot, st->fl_bbot, sizeof fl_s_bot);
		memcpy(fl_s_bseq, st->fl_bseq, sizeof fl_s_bseq);
	}
	st->oof_max_y = st->cursor_y;
	st->flex_gap = child_flex_row ? track_gap : 0;
	if (is_block && grid_cols > 1) {
		/*
		 * Row-major grid placement. Each item is laid out into its column's
		 * containing block by the same recursive walk, then the pen is
		 * rewound to the top of the row for the next column; the row ends at
		 * the tallest item. That is enough for the two shapes that matter,
		 * a card deck and a stat row, and it is honest about the rest:
		 * every track is the same width, there is no row-gap, and nothing
		 * spans.
		 */
		int gap = track_gap;
		int colw = (content_w - gap * (grid_cols - 1)) / grid_cols;
		if (colw < 60) { grid_cols = 1; colw = content_w; gap = 0; }
		{
			dom_node *child = NULL;
			int col = 0;
			int row_y = st->cursor_y;
			int row_max = row_y;
			if (dom_node_get_first_child(node, &child) == DOM_NO_ERR) {
				while (child) {
					dom_node *next = NULL;
					dom_node_type ct;
					if (dom_node_get_node_type(child, &ct) == DOM_NO_ERR &&
							ct == DOM_ELEMENT_NODE) {
						int cx = content_x + col * (colw + gap);
						st->cursor_y = row_y;
						st->cursor_x = cx;
						st->line_left = cx;
						st->line_right = cx + colw;
						st->line_height = 0;
						st->line_has_content = false;
						st->pending_space = false;
						st->line_first = st->out->n_items;
						walk(st, child, style ? style : pstyle,
								style ? &e : pe, cx, colw, 0);
						line_break(st);
						if (st->cursor_y > row_max) row_max = st->cursor_y;
						col++;
						if (col >= grid_cols) {
							col = 0;
							row_y = row_max + gap;
							row_max = row_y;
						}
					}
					dom_node_get_next_sibling(child, &next);
					dom_node_unref(child);
					child = next;
				}
			}
			st->cursor_y = (col == 0 && row_max > row_y) ? row_max
				: ((col == 0) ? (row_y - gap) : row_max);
			if (st->cursor_y < row_y) st->cursor_y = row_y;
			st->line_left = content_x;
			st->line_right = content_x + content_w;
			st->cursor_x = content_x;
			st->line_height = 0;
			st->line_has_content = false;
		}
	} else if (table_ish) {
		tbl_walk_children(st, node, style, pstyle, &e, pe,
				content_x, content_w, atomic ? -1 : box_idx);
	} else {
		dom_node *child = NULL;
		/*
		 * engflex (#245): a ROW flex container distributes free space
		 * (justify-content) and positions items on the cross axis
		 * (align-items). This is ARMED only when those properties ask for
		 * something other than the default start/top packing, so a default
		 * flex container takes the identical plain walk below and its item
		 * list is byte-identical. When armed, the walk is unchanged (same
		 * calls, same order); we only RECORD each flex item's item-array
		 * boundary and start line so flex_align() can translate them after.
		 */
		uint8_t fx_just = CSS_JUSTIFY_CONTENT_INHERIT;
		uint8_t fx_align = CSS_ALIGN_ITEMS_INHERIT;
		/*
		 * A ROW flex container: record each item's item-array boundary,
		 * start line and flex-grow factor during the (unchanged) child
		 * walk, then SIZE (flex-grow, #245 engflexgrow) and ALIGN
		 * (justify/align, engflex) the line. The recording changes no
		 * emission, and both post-passes are inert for a default
		 * container (no grow, default justify/align), so a plain flex
		 * container's item list is byte-identical.
		 */
		if (child_flex_row && style && !st->measuring) {
			int bnd[FLEX_MAX_ITEMS + 1];
			int cy[FLEX_MAX_ITEMS];
			css_fixed grow[FLEX_MAX_ITEMS];
			css_fixed shrink[FLEX_MAX_ITEMS];
			int basis[FLEX_MAX_ITEMS];
			int cnt = 0, ok = 1;
			int fstart = st->out->n_items;
			int ftop = st->cursor_y;
			fx_just = css_computed_justify_content(style);
			fx_align = css_computed_align_items(style);
			if (dom_node_get_first_child(node, &child) == DOM_NO_ERR) {
				while (child) {
					dom_node *next = NULL;
					dom_node_type ct;
					if (ok && dom_node_get_node_type(child, &ct) ==
							DOM_NO_ERR && ct == DOM_ELEMENT_NODE) {
						if (cnt < FLEX_MAX_ITEMS) {
							css_computed_style *ics;
							css_fixed gv = 0, sv = 0, bv = 0;
							css_unit bu = CSS_UNIT_PX;
							cy[cnt] = st->cursor_y;
							bnd[cnt] = (cnt == 0) ? fstart
								: st->out->n_items;
							grow[cnt] = 0;
							/* flex-shrink initial value is 1, not 0. */
							shrink[cnt] = INTTOFIX(1);
							basis[cnt] = -1;
							ics = mcs_compute_style(st->css,
									(dom_element *) child, style);
							if (ics) {
								if (css_computed_flex_grow(ics, &gv)
										== CSS_FLEX_GROW_SET)
									grow[cnt] = gv;
								if (css_computed_flex_shrink(ics, &sv)
										== CSS_FLEX_SHRINK_SET)
									shrink[cnt] = sv;
								if (css_computed_flex_basis(ics, &bv, &bu)
										== CSS_FLEX_BASIS_SET) {
									/* Only a positive length starts an
									 * item's main size here; a 0 basis
									 * (the flex:N seed) is left to grow. */
									int bpx = fixed_px(bv, bu,
											e.font_size, content_w);
									if (bpx > 0) basis[cnt] = bpx;
								}
								css_computed_style_destroy(ics);
							}
							cnt++;
						} else {
							ok = 0;
						}
					}
					walk(st, child, style ? style : pstyle,
							style ? &e : pe,
							content_x, content_w, child_flex_row);
					dom_node_get_next_sibling(child, &next);
					dom_node_unref(child);
					child = next;
				}
			}
			bnd[cnt] = st->out->n_items;
			if (ok && cnt > 0 &&
					css_computed_flex_wrap(style) ==
						CSS_FLEX_WRAP_WRAP) {
				/*
				 * flex-wrap:wrap (#245 engflexwrap). Break the packed
				 * items onto multiple lines and lay each line out with
				 * the single-line passes, stacking down the cross axis.
				 * Gated on WRAP so nowrap / wrap-reverse and non-flex
				 * pages never enter here and stay byte-identical (AE=0).
				 */
				flex_wrap_lines(st, ftop, content_x, content_w,
						fx_just, fx_align, st->flex_gap,
						bnd, grow, shrink, basis, cnt);
			} else if (ok && cnt > 0) {
				/*
				 * CSS flexbox order (#245 engflexsize): flex-basis
				 * sets each item's starting main size FIRST, then the
				 * single free-space distribution runs. flex-grow
				 * (free > 0) and flex-shrink (free < 0) are mutually
				 * exclusive on the same post-basis line end, so both
				 * are called and exactly one does work; justify-content
				 * (flex_align) then distributes only what is left.
				 * `based`/`grew`/`shrunk` accumulate the net px each
				 * pass moved the packed line end.
				 */
				int used, grew, shrunk;
				int based = flex_basis_pass(st, ftop, bnd, cy,
						basis, cnt);
				used = st->cursor_x + based;
				grew = flex_grow_pass(st, ftop, content_x,
						content_w, used, bnd, cy,
						grow, cnt);
				used += grew;
				shrunk = flex_shrink_pass(st, ftop, content_x,
						content_w, used, bnd, cy,
						shrink, cnt);
				used += shrunk;
				flex_align(st, fstart, st->out->n_items, ftop, content_x, content_w,
						used, fx_just, fx_align,
						bnd, cy, cnt);
			}
		} else if (dom_node_get_first_child(node, &child) == DOM_NO_ERR) {
			while (child) {
				dom_node *next = NULL;
				walk(st, child, style ? style : pstyle,
						style ? &e : pe,
						content_x, content_w, child_flex_row);
				dom_node_get_next_sibling(child, &next);
				dom_node_unref(child);
				child = next;
			}
		}
	}
	st->flex_gap = gap_outer;

	/* An absolutely positioned child does not push its siblings down, but it
	 * does still have to fit inside its containing block. */
	if (st->oof_max_y > st->cursor_y) st->cursor_y = st->oof_max_y;
	st->oof_max_y = oof_outer;
	/*
	 * float (#245 engfloat2): if this block's own children opened floats that
	 * are still live, grow the block to contain them (the clearfix idiom every
	 * modern reset applies), then restore the band set to what it was on entry
	 * so a following SIBLING of this block never sees them. Child-opened bands
	 * are those with a seq at or beyond the entry marker, which survives the
	 * compaction that may have reordered the array during the child walk.
	 */
	{
		int i, mb = st->cursor_y;
		for (i = 0; i < st->fl_n; i++)
			if (st->fl_bseq[i] >= fl_s_seq && st->fl_bbot[i] > mb)
				mb = st->fl_bbot[i];
		if (mb > st->cursor_y)
			st->cursor_y = mb;
	}
	st->fl_n = fl_s_n; st->fl_cx0 = fl_s_cx0; st->fl_cx1 = fl_s_cx1;
	if (fl_s_n > 0) {
		memcpy(st->fl_bside, fl_s_side, sizeof fl_s_side);
		memcpy(st->fl_bx0, fl_s_x0, sizeof fl_s_x0);
		memcpy(st->fl_bx1, fl_s_x1, sizeof fl_s_x1);
		memcpy(st->fl_bbot, fl_s_bot, sizeof fl_s_bot);
		memcpy(st->fl_bseq, fl_s_bseq, sizeof fl_s_bseq);
	}

	if (is_block && atomic) {
		line_break(st);
		st->cursor_y += e.pad[LB_BOTTOM] + e.bw[LB_BOTTOM];
		{
			/*
			 * Size the box to what its contents ACTUALLY occupied, then put
			 * the parent's pen back on the line it came from and step it
			 * across. `outer_w` was only ever the maximum this box was
			 * allowed; a chip is as wide as its word.
			 */
			int used_h = st->cursor_y - box_start_y;
			/* A declared height REPLACES the content-derived one. That
			 * is what CSS says: a fixed-height box does not grow with
			 * its content, the content overflows. It is also the only
			 * thing that makes an EMPTY box (a window pip, a rule, a
			 * spacer) have any size at all. */
			if (e.height >= 0)
				used_h = e.height + e.pad[LB_TOP] + e.pad[LB_BOTTOM]
					+ e.bw[LB_TOP] + e.bw[LB_BOTTOM];
			int inner_r = content_x;
			int used_w, i2;
			for (i2 = ai_first; i2 < st->out->n_items; i2++) {
				layout_item *q = &st->out->items[i2];
				int r;
				if (i2 == box_idx) continue;
				if (q->kind == 0)
					r = q->x + lmeasure(st, q->text, q->size, q->face, q->fstyle);
				else
					r = q->x + q->w;
				if (r > inner_r) inner_r = r;
			}
			used_w = (inner_r - content_x) + pl + pr + bl + br;
			if (e.width >= 0) used_w = outer_w;
			if (used_w > outer_w) used_w = outer_w;
			if (e.min_width >= 0 && used_w < e.min_width) used_w = e.min_width;
			if (used_w < 1) used_w = 1;

			/*
			 * WRAP. The box was laid out against the whole line box (see the
			 * shrink-to-fit note above), so its internals do not depend on
			 * where it sits and moving it is an exact translation rather than
			 * a re-flow. That is the entire reason for capping at the line
			 * rather than at the room remaining on it.
			 */
			if (ai_line_had && box_x + used_w > saved_right &&
					saved_left + used_w <= saved_right) {
				int dy = ai_line_h > 0 ? ai_line_h : used_h;
				int dx = (saved_left + ml) - box_x;
				for (i2 = ai_first; i2 < st->out->n_items; i2++) {
					st->out->items[i2].x += dx;
					st->out->items[i2].y += dy;
				}
				box_x += dx;
				ai_line_y += dy;
				ai_line_h = 0;
				ai_line_had = false;
			}

			if (box_idx >= 0) {
				st->out->items[box_idx].w = used_w;
				st->out->items[box_idx].h = used_h;
			}
			if (bgimg_idx >= 0) {
				int bgw = used_w - e.bw[LB_LEFT] - e.bw[LB_RIGHT];
				int bgh = used_h - e.bw[LB_TOP] - e.bw[LB_BOTTOM];
				if (bgh > 2000) bgh = 2000;   /* bound the decode buffer; a no-repeat bg shows once at the top */
				st->out->items[bgimg_idx].w = bgw > 0 ? bgw : 0;
				st->out->items[bgimg_idx].h = bgh > 0 ? bgh : 0;
			}
			if (grad_idx >= 0) {   /* #245 enggrad: gradient fills the padding box */
				int gw = used_w - e.bw[LB_LEFT] - e.bw[LB_RIGHT];
				int gh = used_h - e.bw[LB_TOP] - e.bw[LB_BOTTOM];
				if (gh > 4000) gh = 4000;   /* bound the ramp buffer */
				st->out->items[grad_idx].w = gw > 0 ? gw : 0;
				st->out->items[grad_idx].h = gh > 0 ? gh : 0;
			}
			st->cursor_y = ai_line_y;
			st->cursor_x = box_x + used_w + mr;
			st->line_has_content = true;
			st->line_height = ai_line_h;
			if (used_h > st->line_height) st->line_height = used_h;
			st->pending_space = false;
		}
		st->line_left = saved_left;
		st->line_right = saved_right;
		st->line_align = saved_align;
	} else if (is_block) {
		line_break(st);
		st->cursor_y += e.pad[LB_BOTTOM] + e.bw[LB_BOTTOM];
		/* Same rule as the atomic path above: a declared height is the
		 * used height, and the pen advances by it rather than by how far
		 * the content happened to reach. */
		if (e.height >= 0)
			st->cursor_y = box_start_y + e.height + e.pad[LB_TOP]
				+ e.pad[LB_BOTTOM] + e.bw[LB_TOP] + e.bw[LB_BOTTOM];
		if (box_idx >= 0)
			st->out->items[box_idx].h = st->cursor_y - box_start_y;
		if (bgimg_idx >= 0) {
			int bgw = outer_w - e.bw[LB_LEFT] - e.bw[LB_RIGHT];
			int bgh = (st->cursor_y - box_start_y) - e.bw[LB_TOP] - e.bw[LB_BOTTOM];
			if (bgh > 2000) bgh = 2000;
			st->out->items[bgimg_idx].w = bgw > 0 ? bgw : 0;
			st->out->items[bgimg_idx].h = bgh > 0 ? bgh : 0;
		}
		if (grad_idx >= 0) {   /* #245 enggrad: gradient fills the padding box */
			int gw = outer_w - e.bw[LB_LEFT] - e.bw[LB_RIGHT];
			int gh = (st->cursor_y - box_start_y) - e.bw[LB_TOP] - e.bw[LB_BOTTOM];
			if (gh > 4000) gh = 4000;
			st->out->items[grad_idx].w = gw > 0 ? gw : 0;
			st->out->items[grad_idx].h = gh > 0 ? gh : 0;
		}
		/*
		 * overflow:hidden clipping (#245 engoflow). SINGLE-LEVEL, paint-
		 * affecting, item-granularity. Armed ONLY when this block's computed
		 * overflow is a non-visible value (hidden/scroll/auto; overflow:clip
		 * is not a value this libcss build parses, so it falls through as
		 * visible and is deferred). A non-float block clips its descendants
		 * ([blk_items_start, n_items) minus its own box) to its content box.
		 * Horizontal is always bounded by content_w; vertical only when a
		 * height is declared (an auto-height block grows to its content and
		 * cannot overflow down). overflow:visible (the initial value) never
		 * enters here, so normal pages are byte-identical.
		 */
		if (!is_float && style && !st->measuring) {
			uint8_t ox = css_computed_overflow_x(style);
			uint8_t oy = css_computed_overflow_y(style);
			int clipx = (ox == CSS_OVERFLOW_HIDDEN ||
					ox == CSS_OVERFLOW_SCROLL ||
					ox == CSS_OVERFLOW_AUTO);
			int clipy = (oy == CSS_OVERFLOW_HIDDEN ||
					oy == CSS_OVERFLOW_SCROLL ||
					oy == CSS_OVERFLOW_AUTO) && e.height >= 0;
			int ctop = box_start_y + e.bw[LB_TOP] + e.pad[LB_TOP];
			/*
			 * engscroll (#245): overflow:scroll / overflow:auto VERTICAL
			 * scroll container. A non-atomic block with a declared height
			 * whose content is taller than its content box becomes a
			 * keyboard-scrollable box. Measure the natural content extent
			 * (max item bottom) BEFORE the clip mutates geometry, clamp the
			 * app-requested offset to it, shift the descendant CONTENT up by
			 * that offset, then fall through to the SAME clip overflow:hidden
			 * uses. The box background/border and any background-image or
			 * gradient item do NOT scroll (they belong to the box). The
			 * scrollbar is emitted AFTER the clip so this box does not clip
			 * its own bar; ancestor clips still do (correct nesting). The
			 * overflow:hidden path is byte-for-byte unchanged (AE=0).
			 */
			/*
			 * brhscroll (#245): overflow:scroll / overflow:auto on the VERTICAL
			 * axis (unchanged) and the HORIZONTAL axis (new). A non-atomic block
			 * with a declared height whose content is taller (vertical) or wider
			 * (horizontal) than its content box becomes keyboard-scrollable.
			 * Measure the natural extent BEFORE the clip mutates geometry, clamp
			 * the app-requested per-axis offset, shift the descendant CONTENT
			 * up/left, then fall through to the SAME clip overflow:hidden uses.
			 * The box bg/border/background-image/gradient do NOT scroll. The
			 * scrollbars are emitted AFTER the clip so the box does not clip its
			 * own bars; ancestor clips still do (correct nesting). A box with no
			 * horizontal overflow keeps the exact vertical behaviour (offset_x==0,
			 * so the x-shift is a no-op and no h-bar is emitted): AE=0.
			 */
			int sc_scroll_v = (!atomic) && e.height >= 0 &&
					(oy == CSS_OVERFLOW_SCROLL ||
					oy == CSS_OVERFLOW_AUTO);
			int sc_scroll_h = (!atomic) && e.height >= 0 &&
					(ox == CSS_OVERFLOW_SCROLL ||
					ox == CSS_OVERFLOW_AUTO);
			int sc_active = 0, sc_off = 0, sc_extent = 0;
			int sc_off_x = 0, sc_extent_w = 0, overflow_v = 0, overflow_h = 0;
			if (sc_scroll_v || sc_scroll_h) {
				int mb = ctop, mr = content_x, i2;
				for (i2 = blk_items_start; i2 < st->out->n_items; i2++) {
					layout_item *q;
					int bot, right;
					if (i2 == box_idx || i2 == bgimg_idx ||
							i2 == grad_idx) continue;
					q = &st->out->items[i2];
					bot = q->y + (q->kind == 0 ? q->size : q->h);
					if (q->kind == 0)
						right = q->x + lmeasure(st, q->text,
								q->size, q->face, q->fstyle);
					else
						right = q->x + q->w;
					if (bot > mb) mb = bot;
					if (right > mr) mr = right;
				}
				sc_extent = mb - ctop;
				sc_extent_w = mr - content_x;
				overflow_v = sc_scroll_v && sc_extent > e.height;
				overflow_h = sc_scroll_h && sc_extent_w > content_w;
				if (overflow_v) {
					int maxoff = sc_extent - e.height;
					sc_off = layout_get_scroll_request(st->out->n_scrolls);
					if (sc_off < 0) sc_off = 0;
					if (sc_off > maxoff) sc_off = maxoff;
				}
				if (overflow_h) {
					int maxoffx = sc_extent_w - content_w;
					sc_off_x = layout_get_scroll_request_x(st->out->n_scrolls);
					if (sc_off_x < 0) sc_off_x = 0;
					if (sc_off_x > maxoffx) sc_off_x = maxoffx;
				}
				if (overflow_v || overflow_h) {
					int i3;
					for (i3 = blk_items_start; i3 < st->out->n_items; i3++) {
						if (i3 == box_idx || i3 == bgimg_idx ||
								i3 == grad_idx) continue;
						st->out->items[i3].y -= sc_off;
						st->out->items[i3].x -= sc_off_x;
					}
					if (st->out->n_scrolls < LAYOUT_MAX_SCROLLS) {
						scroll_box *sb =
							&st->out->scrolls[st->out->n_scrolls];
						sb->content_x = content_x;
						sb->content_y = ctop;
						sb->content_w = content_w;
						sb->content_h = e.height;
						sb->extent_h = overflow_v ? sc_extent : e.height;
						sb->offset = sc_off;
						sb->extent_w = overflow_h ? sc_extent_w : content_w;
						sb->offset_x = sc_off_x;
						st->out->n_scrolls++;
						sc_active = 1;
					}
				}
			}
			if (clipx || clipy) {
				int x0 = clipx ? content_x : -0x3fffffff;
				int x1 = clipx ? content_x + content_w : 0x3fffffff;
				int y0 = clipy ? ctop : -0x3fffffff;
				int y1 = clipy ? ctop + e.height : 0x3fffffff;
				overflow_clip(st, blk_items_start, box_idx,
						x0, y0, x1, y1);
			}
			if (sc_active) {
				int sbw = 10;
				int need = (overflow_v ? 2 : 0) + (overflow_h ? 2 : 0);
				if (st->out->n_items + need <= LAYOUT_MAX_ITEMS) {
					/* reserve the bottom-right corner when both bars show */
					int vh = e.height - (overflow_h ? sbw : 0);
					int hw = content_w - (overflow_v ? sbw : 0);
					layout_item *trk, *thb;
					if (overflow_v) {
						int trx = content_x + content_w - sbw;
						int maxoff = sc_extent - e.height;
						int th = (int)((long)vh * e.height / sc_extent);
						int ty;
						if (th < 16) th = 16;
						if (th > vh) th = vh;
						ty = ctop + (maxoff > 0 ?
							(int)((long)(vh - th) * sc_off / maxoff) : 0);
						trk = &st->out->items[st->out->n_items++];
						memset(trk, 0, sizeof *trk);
						trk->kind = 1; trk->has_bg = 1; trk->bg = 0x00D4D0C8u;
						trk->x = trx; trk->y = ctop;
						trk->w = sbw; trk->h = vh;
						thb = &st->out->items[st->out->n_items++];
						memset(thb, 0, sizeof *thb);
						thb->kind = 1; thb->has_bg = 1; thb->bg = 0x00808080u;
						thb->x = trx + 1; thb->y = ty;
						thb->w = sbw - 2; thb->h = th;
					}
					if (overflow_h) {
						int hby = ctop + e.height - sbw;
						int maxoffx = sc_extent_w - content_w;
						int tw = (int)((long)hw * content_w / sc_extent_w);
						int tx;
						if (tw < 16) tw = 16;
						if (tw > hw) tw = hw;
						tx = content_x + (maxoffx > 0 ?
							(int)((long)(hw - tw) * sc_off_x / maxoffx) : 0);
						trk = &st->out->items[st->out->n_items++];
						memset(trk, 0, sizeof *trk);
						trk->kind = 1; trk->has_bg = 1; trk->bg = 0x00D4D0C8u;
						trk->x = content_x; trk->y = hby;
						trk->w = hw; trk->h = sbw;
						thb = &st->out->items[st->out->n_items++];
						memset(thb, 0, sizeof *thb);
						thb->kind = 1; thb->has_bg = 1; thb->bg = 0x00808080u;
						thb->x = tx; thb->y = hby + 1;
						thb->w = tw; thb->h = sbw - 2;
					}
				}
			}
		}
		st->cursor_y += e.margin_bottom;
		if (is_float) {
			/*
			 * float (#245 engfloat / engfloat2). The block laid itself out
			 * normally in the flow; now move it beside any floats already open
			 * (packing on its side, dropping below them if it does not fit) and
			 * open a band so following content flows beside it. The whole box
			 * (its decorative box + every child item) is translated by (dx,dy) as
			 * a unit; its internals are position-invariant because it was laid out
			 * against a fixed declared width. With no float already open this
			 * reduces to the old single-float translate exactly.
			 */
			int ftop = box_start_y;
			int fbot = st->cursor_y;
			int mbw = ml + outer_w + mr;
			int fy, ox0, ox1, dx, dy, i2;
			if (st->fl_n == 0) {
				st->fl_cx0 = saved_left;
				st->fl_cx1 = saved_right;
			}
			fy = fl_fit(st, fl_side, ftop, mbw, &ox0, &ox1);
			dx = (ox0 + ml) - box_x;
			dy = fy - ftop;
			if (dx != 0 || dy != 0) {
				for (i2 = blk_items_start; i2 < st->out->n_items; i2++) {
					st->out->items[i2].x += dx;
					st->out->items[i2].y += dy;
				}
			}
			fl_add(st, fl_side, ox0, ox1, fbot + dy);
			st->cursor_y = fy;
			fl_recompute(st, fy);
			st->cursor_x = st->line_left;
			st->line_has_content = false;
			st->line_height = 0;
			st->pending_space = false;
			st->line_first = st->out->n_items;
			st->line_align = saved_align;
		} else {
			st->line_left = saved_left;
			st->line_right = saved_right;
			st->line_align = saved_align;
			st->cursor_x = st->line_left;
		}
	} else {
		st->cursor_x += pr + br + mr;
		/*
		 * An inline box with a background or border, painted ONE BOX PER
		 * LINE from the runs that were actually emitted.
		 *
		 * The previous version drew a single box and gave up entirely if
		 * the content wrapped. That is not a cosmetic shortfall: a pill
		 * button styled `background: var(--accent); color: var(--btn-ink)`
		 * has deliberately dark text chosen to sit on a bright fill, so
		 * losing the fill does not make it look plain, it makes it
		 * INVISIBLE. Measured on maytera.net at 760px, the header "Get it"
		 * call to action wraps and did exactly that.
		 *
		 * The extents come from the runs themselves rather than from an
		 * assumed line step, so a line containing mixed sizes still gets a
		 * box that matches what was drawn.
		 */
		/*
		 * The span guard is not a tuning knob. Sizing the box means
		 * scanning this element's runs once per line, and rotating each
		 * finished box back in front of them is a memmove over the span.
		 * Both are fine for a pill or a code span, which is what an inline
		 * background IS. An inline element wrapping thousands of runs is
		 * either a page doing something strange or a page doing it on
		 * purpose, and neither is worth quadratic layout time.
		 */
		if ((e.has_bg || has_any_border(&e)) && !e.visibility &&
				st->out->n_items > inline_first &&
				st->out->n_items - inline_first <= 1024) {
			int first = inline_first;
			int last = st->out->n_items;
			int placed = 0;
			int done_y[16];
			int ndone = 0;
			int i2;
			int bh = e.line_height + e.pad[LB_TOP] + e.pad[LB_BOTTOM]
				+ e.bw[LB_TOP] + e.bw[LB_BOTTOM];
			for (i2 = first; i2 < last; i2++) {
				layout_item *r = &st->out->items[i2];
				int y, k, seen = 0, minx, maxx;
				if (r->kind != 0) continue;
				y = r->y;
				for (k = 0; k < ndone; k++)
					if (done_y[k] == y) { seen = 1; break; }
				if (seen) continue;
				if (ndone >= 16) break;
				done_y[ndone++] = y;
				minx = 0x3fffffff; maxx = -0x3fffffff;
				for (k = first; k < last; k++) {
					layout_item *q = &st->out->items[k];
					int qx2;
					if (q->kind != 0 || q->y != y) continue;
					if (q->x < minx) minx = q->x;
					qx2 = q->x + lmeasure(st, q->text, q->size, q->face, q->fstyle);
					if (qx2 > maxx) maxx = qx2;
				}
				if (maxx <= minx) continue;
				{
					layout_item *bx = item_new(st);
					if (!bx) break;
					bx->kind = 1;
					bx->x = minx - pl - bl;
					bx->y = y - e.pad[LB_TOP] - e.bw[LB_TOP];
					bx->w = (maxx - minx) + pl + pr + bl + br;
					bx->h = bh;
					fill_box_style(bx, &e);
				}
				rotate_item_to(st, st->out->n_items - 1, first + placed);
				placed++;
			}
			(void) inline_start_x;
			(void) inline_start_y;
		}
	}

	OOF_REWIND();

	if (href_changed)
		memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);

	if (style) css_computed_style_destroy(style);
}
#undef OOF_REWIND

/* ------------------------------------------------------------------ */
/* CSS table layout (#245)                                            */
/* ------------------------------------------------------------------ */
/*
 * Enough of CSS 2.1 chapter 17 to render a real table: box generation for
 * rows and cells, the automatic column-width algorithm, row heights taken
 * from the tallest cell, colspan, and border-collapse.
 *
 * WHY THE TRIGGER IS NOT SIMPLY `display: table`.
 *
 * maytera.net's own system-requirements table is styled
 *
 *     .prose table { width: 100%; border-collapse: collapse;
 *                    display: block; overflow-x: auto; }
 *
 * which is the ordinary idiom for letting a wide table scroll on a phone.
 * The TABLE element there is therefore a BLOCK; only its <tbody>, <tr> and
 * <td> descendants are table-internal. CSS 2.1 17.2.1 still lays that out as
 * a table, because a run of table-internal boxes whose parent is not a table
 * generates an ANONYMOUS table box around itself. Keying off `display: table`
 * would have fixed the security page's advisory table and left the homepage
 * table exactly as broken as it was. The trigger here is therefore "this
 * element has children that produce rows", which is what the anonymous-box
 * rule amounts to.
 *
 * NOT IMPLEMENTED, with the consequence stated:
 *   - rowspan. A cell always claims exactly one row, so a table using
 *     rowspan has its later rows shifted left by one column per open span.
 *     (colspan IS handled.)
 *   - vertical-align. Every cell is top-aligned. Both tables on the target
 *     site ask for `vertical-align: top` explicitly so this is currently
 *     exact, but the CSS initial value for a cell is `middle`.
 *   - <caption>, <col>, <colgroup>. A caption lays out as an ordinary block
 *     where it sits; column elements generate no boxes at all in CSS, so
 *     their width and background are ignored.
 *   - `table-layout: fixed`. Everything uses the automatic algorithm.
 *   - border-spacing. A non-collapsed table gets adjacent borders touching
 *     rather than separated.
 *   - A table wider than its containing block is SCALED DOWN to fit rather
 *     than overflowing, because there is no horizontal scroll to overflow
 *     into. That is a deliberate divergence from `overflow-x: auto`.
 */

#define TBL_MAX_COLS   24
#define TBL_MAX_ROWS   400
#define TBL_MAX_GROUPS 32
/* The width the max-content probe lays a cell out at. Wide enough that no
 * realistic cell wraps, small enough that an unsized <img> inside one cannot
 * produce a nonsense column demand. */
#define TBL_PROBE_MAX  4000

typedef struct {
	dom_node *node;     /* ref held until tbl_free() */
	int col;            /* first column index */
	int span;           /* colspan, >= 1 */
	int li;             /* left border+padding of the cell box, px */
	int lr;             /* left+right border+padding of the cell box, px */
	int minw;           /* CSS min-width in px, -1 for none */
	int cmin, cmax;     /* measured OUTER min/max-content widths */
	int box_idx;        /* index of the cell's own bg/border item, or -1 */
} tbl_cell;

typedef struct {
	dom_node *node;               /* ref held */
	css_computed_style *style;    /* owned, may be NULL */
	estyle e;
	int cell0, ncell;             /* slice of the flat cell array */
	int box_idx;
} tbl_row;

typedef struct {
	tbl_row  *rows;
	tbl_cell *cells;
	int nrows, cap_rows;
	int ncells, cap_cells;
	int ncols;
	css_computed_style *groups[TBL_MAX_GROUPS];  /* owned row-group styles */
	int ngroups;
} tbl_grid;

static void tbl_free(tbl_grid *g)
{
	int i;
	for (i = 0; i < g->nrows; i++) {
		if (g->rows[i].style) css_computed_style_destroy(g->rows[i].style);
		if (g->rows[i].node) dom_node_unref(g->rows[i].node);
	}
	for (i = 0; i < g->ncells; i++)
		if (g->cells[i].node) dom_node_unref(g->cells[i].node);
	for (i = 0; i < g->ngroups; i++)
		if (g->groups[i]) css_computed_style_destroy(g->groups[i]);
	free(g->rows);
	free(g->cells);
	memset(g, 0, sizeof(*g));
}

static int tbl_grow(tbl_grid *g, int want_rows, int want_cells)
{
	if (want_rows > g->cap_rows) {
		int nc = g->cap_rows ? g->cap_rows * 2 : 16;
		void *p;
		while (nc < want_rows) nc *= 2;
		if (nc > TBL_MAX_ROWS) nc = TBL_MAX_ROWS;
		if (want_rows > nc) return -1;
		p = realloc(g->rows, (size_t) nc * sizeof(tbl_row));
		if (!p) return -1;
		g->rows = p; g->cap_rows = nc;
	}
	if (want_cells > g->cap_cells) {
		int nc = g->cap_cells ? g->cap_cells * 2 : 64;
		void *p;
		while (nc < want_cells) nc *= 2;
		if (nc > TBL_MAX_ROWS * TBL_MAX_COLS) nc = TBL_MAX_ROWS * TBL_MAX_COLS;
		if (want_cells > nc) return -1;
		p = realloc(g->cells, (size_t) nc * sizeof(tbl_cell));
		if (!p) return -1;
		g->cells = p; g->cap_cells = nc;
	}
	return 0;
}

/* Read a non-negative integer HTML attribute; `dflt` when absent or unparseable. */
static int tbl_attr_int(dom_node *n, const char *name, int dflt)
{
	char buf[16];
	int v = 0, i = 0;
	get_attr(n, name, buf, sizeof buf);
	if (!buf[0]) return dflt;
	while (buf[i] >= '0' && buf[i] <= '9') { v = v * 10 + (buf[i] - '0'); i++; }
	if (i == 0 || v <= 0) return dflt;
	return v;
}

/*
 * Lay `cell` out into a scratch region `probe_w` wide and report how far right
 * its CONTENT actually reached, then throw the items away.
 *
 * This is the min/max-content measurement, and doing it by re-running the real
 * engine rather than by a separate width estimator is deliberate: a cell holds
 * inline code chips, <br> breaks, badges with their own padding and mixed font
 * faces, and any separate estimator would have to reimplement all of that and
 * would then drift from it. Running layout twice more per cell is affordable
 * because a cell is small.
 *
 * ONLY TEXT AND IMAGE ITEMS COUNT. Box items are skipped because a block-level
 * box stretches to its containing block by definition, so counting them would
 * make every cell report `probe_w` and every column come out identical, which
 * is the failure mode this whole function exists to avoid.
 */
static int tbl_probe(lstate *st, dom_node *cell, const css_computed_style *pstyle,
		const estyle *pe, int probe_w)
{
	int save_n = st->out->n_items;
	int save_of = st->out->overflowed;
	int save_cx = st->cursor_x, save_cy = st->cursor_y;
	int save_ll = st->line_left, save_lr = st->line_right;
	int save_lh = st->line_height;
	bool save_hc = st->line_has_content, save_ps = st->pending_space;
	int save_oof = st->oof_max_y, save_gap = st->flex_gap;
	int save_meas = st->measuring;
	int right = 0, i;

	st->cursor_x = 0; st->cursor_y = 0;
	st->line_left = 0; st->line_right = probe_w;
	st->line_height = 0;
	st->line_has_content = false; st->pending_space = false;
	st->oof_max_y = 0; st->flex_gap = 0;
	st->measuring = 1;

	walk(st, cell, pstyle, pe, 0, probe_w, 0);

	for (i = save_n; i < st->out->n_items; i++) {
		layout_item *q = &st->out->items[i];
		int r;
		if (q->kind == 0)
			r = q->x + lmeasure(st, q->text, q->size, q->face, q->fstyle);
		else if (q->kind == 3)
			r = q->x + q->w;
		else
			continue;
		if (r > right) right = r;
	}

	st->out->n_items = save_n;
	st->out->overflowed = save_of;
	st->cursor_x = save_cx; st->cursor_y = save_cy;
	st->line_left = save_ll; st->line_right = save_lr;
	st->line_height = save_lh;
	st->line_has_content = save_hc; st->pending_space = save_ps;
	st->oof_max_y = save_oof; st->flex_gap = save_gap;
	st->measuring = save_meas;
	return right;
}

/* Append one <tr> and its cells to the grid. */
static void tbl_add_row(lstate *st, tbl_grid *g, dom_node *rownode,
		const css_computed_style *pstyle, const estyle *pe, int cb_w)
{
	tbl_row *r;
	dom_node *child = NULL;
	int col = 0;

	if (tbl_grow(g, g->nrows + 1, g->ncells) != 0) return;
	r = &g->rows[g->nrows];
	memset(r, 0, sizeof(*r));
	r->box_idx = -1;
	r->cell0 = g->ncells;
	r->style = mcs_compute_style(st->css, (dom_element *) rownode, pstyle);
	if (r->style)
		read_style(r->style, pe, cb_w, &r->e);
	else
		r->e = *pe;
	/*
	 * `display: none` on the ROW. walk() checks this for the cell, but the
	 * row's own box is never walked here (the cells are laid out directly),
	 * so without this test a hidden row's cells rendered anyway. That is
	 * worse than a layout error: it is content the author hid becoming
	 * visible, which is how a tab-panel table or a filtered list leaks every
	 * row it was supposed to be hiding.
	 */
	if (r->e.display == CSS_DISPLAY_NONE) {
		if (r->style) css_computed_style_destroy(r->style);
		return;
	}
	dom_node_ref(rownode);
	r->node = rownode;
	g->nrows++;

	if (dom_node_get_first_child(rownode, &child) != DOM_NO_ERR) return;
	while (child) {
		dom_node *next = NULL;
		dom_node_type ct;
		if (dom_node_get_node_type(child, &ct) == DOM_NO_ERR &&
				ct == DOM_ELEMENT_NODE && col < TBL_MAX_COLS) {
			/*
			 * EVERY element child of a row is a cell. CSS 2.1 wraps a
			 * row's non-cell content in an anonymous table-cell, so
			 * treating them all as cells is both simpler and closer to
			 * the spec than filtering on `display: table-cell`.
			 */
			css_computed_style *cs;
			estyle ce;
			int span = tbl_attr_int(child, "colspan", 1);
			if (span > TBL_MAX_COLS - col) span = TBL_MAX_COLS - col;
			if (span < 1) span = 1;
			if (tbl_grow(g, g->nrows, g->ncells + 1) == 0) {
				tbl_cell *c = &g->cells[g->ncells];
				memset(c, 0, sizeof(*c));
				c->box_idx = -1;
				c->col = col;
				c->span = span;
				dom_node_ref(child);
				c->node = child;
				cs = mcs_compute_style(st->css, (dom_element *) child,
						r->style ? r->style : pstyle);
				if (cs) {
					read_style(cs, &r->e, cb_w, &ce);
					c->li = ce.bw[LB_LEFT] + ce.pad[LB_LEFT];
					c->lr = c->li + ce.bw[LB_RIGHT] + ce.pad[LB_RIGHT];
					c->minw = ce.min_width;
					css_computed_style_destroy(cs);
				} else {
					c->li = 0; c->lr = 0; c->minw = -1;
				}
				g->ncells++;
				r->ncell++;
				col += span;
				if (col > g->ncols) g->ncols = col;
			}
		}
		dom_node_get_next_sibling(child, &next);
		dom_node_unref(child);
		child = next;
	}
}

/*
 * Lay the collected grid out at `content_x`, `avail_w`, starting at the
 * current pen. `box_idx` is the TABLE element's own background/border item so
 * a shrink-to-fit table can be narrowed to what it actually used.
 */
static void tbl_layout(lstate *st, tbl_grid *g, const css_computed_style *tstyle,
		int content_x, int avail_w, int width_is_explicit, int box_idx)
{
	int colmin[TBL_MAX_COLS], colmax[TBL_MAX_COLS], colw[TBL_MAX_COLS];
	int i, j, k;
	int sum_min = 0, sum_max = 0, table_w;
	int collapse = 0;
	int row_y;

	if (g->nrows == 0 || g->ncols == 0) return;
	if (g->ncols > TBL_MAX_COLS) g->ncols = TBL_MAX_COLS;

	if (tstyle && css_computed_border_collapse(tstyle) ==
			CSS_BORDER_COLLAPSE_COLLAPSE)
		collapse = 1;

	for (j = 0; j < g->ncols; j++) { colmin[j] = 0; colmax[j] = 0; }

	/* ---- 1. per-cell min/max content widths ---- */
	for (i = 0; i < g->nrows; i++) {
		tbl_row *r = &g->rows[i];
		for (k = 0; k < r->ncell; k++) {
			tbl_cell *c = &g->cells[r->cell0 + k];
			const css_computed_style *ps = r->style ? r->style : tstyle;
			int wide = tbl_probe(st, c->node, ps, &r->e, TBL_PROBE_MAX);
			int narrow = tbl_probe(st, c->node, ps, &r->e, 1);
			c->cmax = (wide  - c->li) + c->lr;
			c->cmin = (narrow - c->li) + c->lr;
			if (c->cmax < c->lr) c->cmax = c->lr;
			if (c->cmin < c->lr) c->cmin = c->lr;
			if (c->cmin > c->cmax) c->cmax = c->cmin;
			if (c->minw >= 0) {
				if (c->cmin < c->minw) c->cmin = c->minw;
				if (c->cmax < c->minw) c->cmax = c->minw;
			}
		}
	}

	/* ---- 2. column demands: single-column cells first, then spans ---- */
	for (i = 0; i < g->nrows; i++) {
		tbl_row *r = &g->rows[i];
		for (k = 0; k < r->ncell; k++) {
			tbl_cell *c = &g->cells[r->cell0 + k];
			if (c->span != 1 || c->col >= g->ncols) continue;
			if (c->cmin > colmin[c->col]) colmin[c->col] = c->cmin;
			if (c->cmax > colmax[c->col]) colmax[c->col] = c->cmax;
		}
	}
	for (i = 0; i < g->nrows; i++) {
		tbl_row *r = &g->rows[i];
		for (k = 0; k < r->ncell; k++) {
			tbl_cell *c = &g->cells[r->cell0 + k];
			int have_min = 0, have_max = 0, n;
			if (c->span <= 1) continue;
			n = c->span;
			if (c->col + n > g->ncols) n = g->ncols - c->col;
			if (n <= 0) continue;
			for (j = 0; j < n; j++) {
				have_min += colmin[c->col + j];
				have_max += colmax[c->col + j];
			}
			/* Spread only the SHORTFALL, evenly. The exact CSS 2.1 rule
			 * weights by existing width; even spreading is within a few
			 * pixels and does not need a second pass. */
			if (c->cmin > have_min)
				for (j = 0; j < n; j++)
					colmin[c->col + j] += (c->cmin - have_min) / n;
			if (c->cmax > have_max)
				for (j = 0; j < n; j++)
					colmax[c->col + j] += (c->cmax - have_max) / n;
		}
	}
	for (j = 0; j < g->ncols; j++) {
		if (colmin[j] < 1) colmin[j] = 1;
		if (colmax[j] < colmin[j]) colmax[j] = colmin[j];
		sum_min += colmin[j];
		sum_max += colmax[j];
	}

	/* ---- 3. distribute across the available width (CSS 2.1 17.5.2.2) ---- */
	if (sum_max <= avail_w) {
		for (j = 0; j < g->ncols; j++) colw[j] = colmax[j];
		table_w = sum_max;
		if (width_is_explicit && sum_max > 0) {
			int extra = avail_w - sum_max, used = 0;
			for (j = 0; j < g->ncols; j++) {
				int add = (j == g->ncols - 1) ? (extra - used)
					: (int)(((long) extra * colmax[j]) / sum_max);
				colw[j] += add;
				used += add;
			}
			table_w = avail_w;
		}
	} else if (sum_min < avail_w) {
		int extra = avail_w - sum_min;
		int span_w = sum_max - sum_min;
		int used = 0;
		for (j = 0; j < g->ncols; j++) {
			int add;
			if (span_w <= 0)
				add = (j == g->ncols - 1) ? (extra - used) : extra / g->ncols;
			else
				add = (j == g->ncols - 1) ? (extra - used)
					: (int)(((long) extra * (colmax[j] - colmin[j])) / span_w);
			colw[j] = colmin[j] + add;
			used += add;
		}
		table_w = avail_w;
	} else {
		/* Even the minimum widths do not fit. A real browser overflows; we
		 * have no horizontal scroll to overflow into, so scale down. */
		int used = 0;
		for (j = 0; j < g->ncols; j++) {
			colw[j] = (j == g->ncols - 1) ? (avail_w - used)
				: (int)(((long) avail_w * colmin[j]) / sum_min);
			if (colw[j] < 1) colw[j] = 1;
			used += colw[j];
		}
		table_w = avail_w;
	}
	for (j = 0; j < g->ncols; j++) if (colw[j] < 1) colw[j] = 1;
	if (table_w < 1) table_w = 1;
	if (table_w > avail_w) table_w = avail_w;
	if (box_idx >= 0 && box_idx < st->out->n_items && table_w < avail_w)
		st->out->items[box_idx].w -= (avail_w - table_w);

	/* ---- 4. rows ---- */
	row_y = st->cursor_y;
	for (i = 0; i < g->nrows; i++) {
		tbl_row *r = &g->rows[i];
		int row_top = row_y, row_bot = row_y;
		int x = content_x;
		int last_row = (i == g->nrows - 1);

		if ((r->e.has_bg || has_any_border(&r->e)) && !r->e.visibility) {
			layout_item *bx = item_new(st);
			if (bx) {
				r->box_idx = st->out->n_items - 1;
				bx->kind = 1;
				bx->x = content_x; bx->y = row_top;
				bx->w = table_w; bx->h = 0;
				fill_box_style(bx, &r->e);
			}
		}

		for (k = 0; k < r->ncell; k++) {
			tbl_cell *c = &g->cells[r->cell0 + k];
			int cw = 0, before;
			int n = c->span;
			if (c->col >= g->ncols) continue;
			if (c->col + n > g->ncols) n = g->ncols - c->col;
			for (j = 0; j < n; j++) cw += colw[c->col + j];
			if (cw < 1) cw = 1;

			st->cursor_y = row_top;
			st->cursor_x = x;
			st->line_left = x;
			st->line_right = x + cw;
			st->line_height = 0;
			st->line_has_content = false;
			st->pending_space = false;

			before = st->out->n_items;
			walk(st, c->node, r->style ? r->style : tstyle, &r->e, x, cw, 0);
			line_break(st);
			if (st->cursor_y > row_bot) row_bot = st->cursor_y;

			if (st->out->n_items > before &&
					st->out->items[before].kind == 1 &&
					st->out->items[before].y == row_top &&
					st->out->items[before].x == x)
				c->box_idx = before;
			x += cw;
		}

		/*
		 * BORDER-COLLAPSE, done by suppression rather than by moving the
		 * boxes. Two adjacent cells each painting a 1px border at the edge
		 * they share draws a 2px rule, which is exactly what `collapse`
		 * exists to prevent. Dropping the right border of every cell except
		 * the last column, and the bottom border of every cell except the
		 * last row, leaves precisely one 1px line on every internal edge and
		 * a full frame around the outside.
		 */
		for (k = 0; k < r->ncell; k++) {
			tbl_cell *c = &g->cells[r->cell0 + k];
			layout_item *bx;
			if (c->box_idx < 0) continue;
			bx = &st->out->items[c->box_idx];
			bx->h = row_bot - row_top;
			if (collapse) {
				if (c->col + c->span < g->ncols) bx->bw[LB_RIGHT] = 0;
				if (!last_row) bx->bw[LB_BOTTOM] = 0;
			}
		}
		if (r->box_idx >= 0)
			st->out->items[r->box_idx].h = row_bot - row_top;

		row_y = row_bot;
	}

	st->cursor_y = row_y;
	st->cursor_x = content_x;
	st->line_left = content_x;
	st->line_right = content_x + avail_w;
	st->line_height = 0;
	st->line_has_content = false;
	st->pending_space = false;
}

/*
 * Walk `node`'s children, laying out each maximal run of row-producing children
 * as one table and everything else as ordinary blocks, in document order. This
 * is CSS 2.1 17.2.1's anonymous-table generation reduced to the case that
 * actually occurs: a <table> (or a `display: table` box) whose children are
 * rows or row groups.
 */
static void tbl_walk_children(lstate *st, dom_node *node,
		const css_computed_style *style, const css_computed_style *pstyle,
		const estyle *e, const estyle *pe, int content_x, int content_w,
		int box_idx)
{
	const css_computed_style *ts = style ? style : pstyle;
	const estyle *tes = style ? e : pe;
	dom_node *child = NULL;
	tbl_grid g;
	int width_is_explicit = (e->width >= 0);

	memset(&g, 0, sizeof(g));
	if (dom_node_get_first_child(node, &child) != DOM_NO_ERR) return;

	while (child) {
		dom_node *next = NULL;
		dom_node_type ct = DOM_TEXT_NODE;
		int kind = 0;

		if (dom_node_get_node_type(child, &ct) == DOM_NO_ERR &&
				ct == DOM_ELEMENT_NODE) {
			if (node_is(child, "tr")) {
				kind = 1;
			} else if (node_is(child, "thead") || node_is(child, "tbody") ||
					node_is(child, "tfoot")) {
				kind = 2;
			} else {
				/*
				 * Not an HTML table tag; ask the cascade. This is the
				 * `display: table-row` on a <div> case, and it is only
				 * reached inside an element already known to be
				 * table-ish, so the extra style computation is bounded.
				 */
				css_computed_style *cs = mcs_compute_style(st->css,
						(dom_element *) child, ts);
				if (cs) {
					uint8_t d = css_computed_display(cs, false);
					if (d == CSS_DISPLAY_TABLE_ROW) kind = 1;
					else if (d == CSS_DISPLAY_TABLE_ROW_GROUP ||
							d == CSS_DISPLAY_TABLE_HEADER_GROUP ||
							d == CSS_DISPLAY_TABLE_FOOTER_GROUP) kind = 2;
					else if (d == CSS_DISPLAY_TABLE_COLUMN ||
							d == CSS_DISPLAY_TABLE_COLUMN_GROUP) kind = 3;
					css_computed_style_destroy(cs);
				}
			}
		}

		if (kind == 1) {
			tbl_add_row(st, &g, child, ts, tes, content_w);
		} else if (kind == 2) {
			/*
			 * A row group contributes its rows and its own style to the
			 * inheritance chain, but generates no box of its own here.
			 */
			css_computed_style *gs = mcs_compute_style(st->css,
					(dom_element *) child, ts);
			estyle ge;
			dom_node *rc = NULL;
			int hidden = 0;
			if (gs) {
				read_style(gs, tes, content_w, &ge);
				hidden = (ge.display == CSS_DISPLAY_NONE);
				if (g.ngroups < TBL_MAX_GROUPS) g.groups[g.ngroups++] = gs;
				else { css_computed_style_destroy(gs); gs = NULL; ge = *tes; }
			} else {
				ge = *tes;
			}
			/* A hidden row GROUP hides every row in it. Same reasoning as
			 * the per-row test in tbl_add_row(). */
			if (!hidden && dom_node_get_first_child(child, &rc) == DOM_NO_ERR) {
				while (rc) {
					dom_node *rn = NULL;
					dom_node_type rt;
					if (dom_node_get_node_type(rc, &rt) == DOM_NO_ERR &&
							rt == DOM_ELEMENT_NODE)
						tbl_add_row(st, &g, rc, gs ? gs : ts,
								gs ? &ge : tes, content_w);
					dom_node_get_next_sibling(rc, &rn);
					dom_node_unref(rc);
					rc = rn;
				}
			}
		} else if (kind == 3) {
			/* <col>/<colgroup> generate no boxes. Nothing to do. */
		} else if (ct == DOM_ELEMENT_NODE || g.nrows == 0) {
			/*
			 * A non-row child. Flush any table collected so far so
			 * document order is preserved, then lay this child out
			 * normally. Whitespace text between rows falls here too and
			 * is harmless: it produces no runs.
			 */
			if (g.nrows > 0) {
				tbl_layout(st, &g, ts, content_x, content_w,
						width_is_explicit, box_idx);
				tbl_free(&g);
				box_idx = -1;   /* only the first run may narrow the box */
			}
			walk(st, child, ts, tes, content_x, content_w, 0);
		}

		dom_node_get_next_sibling(child, &next);
		dom_node_unref(child);
		child = next;
	}

	if (g.nrows > 0)
		tbl_layout(st, &g, ts, content_x, content_w,
				width_is_explicit, box_idx);
	tbl_free(&g);
}

/*
 * engabspos (#245): stable z-order key sort. Folding the original index into
 * the comparison key makes the order TOTAL, so the result does not depend on
 * whether the libc qsort happens to be stable.
 */
typedef struct { int key; int idx; } zsort_ent;
static int zsort_cmp(const void *a, const void *b)
{
	const zsort_ent *x = (const zsort_ent *) a;
	const zsort_ent *y = (const zsort_ent *) b;
	if (x->key != y->key) return x->key < y->key ? -1 : 1;
	return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

int layout_document(mcs_ctx *css, dom_document *doc, int content_width,
		int (*measure)(const char *, int, int, int), layout_result *out)
{
	lstate st;
	dom_node *root = NULL;
	estyle base;

	if (!css || !doc || !out || !out->items) return -1;

	fontmap_init();		/* #245: enumerate the installed faces once */
	lm_reset();		/* #245: font face may have changed since the last page */
	out->n_items = 0;
	out->has_doc_bg = 0;
	out->doc_bg = 0x00ffffffu;
	out->overflowed = 0;
	out->n_scrolls = 0;   /* engscroll (#245) */
	g_lp_style = g_lp_text = g_lp_flow = g_lp_meas = g_lp_attr = 0;
	g_lp_nelem = g_lp_ntext = 0;
	g_lp_walk = lrdtsc();
	out->content_height = 0;

	st.css = css;
	st.width = content_width > 40 ? content_width : 40;
	g_vp_w = st.width;
	g_vp_h = 600;   /* the browser viewport height; only vh/vmin/vmax use it */
	st.measure = measure;
	st.out = out;
	st.cursor_x = 0;
	st.cursor_y = 0;
	st.line_left = 0;
	st.line_right = st.width;
	st.line_height = 0;
	st.line_has_content = false;
	st.pending_space = false;
	st.oof_max_y = 0;
	st.flex_gap = 0;
	st.cur_href[0] = 0;
	st.line_align = 0;
	st.line_first = 0;
	st.measuring = 0;
	/*
	 * engfloat2 (#245): lstate is NOT memset, it is field-initialised, so the
	 * float band state MUST be zeroed explicitly here. fl_n drives the fit/
	 * recompute/compact scans and fl_add's write index; left as stack garbage it
	 * makes those run off the ends of the band arrays and corrupt the stack. The
	 * arrays themselves need no init because every read is gated on fl_n.
	 */
	st.fl_n = 0;
	st.fl_next_seq = 0;
	st.fl_cx0 = 0;
	st.fl_cx1 = 0;
	/*
	 * engabspos (#245): the initial containing block for absolute positioning
	 * is the viewport. pcb_h uses the viewport height so a top-level
	 * absolute with bottom/percentage resolves against it.
	 */
	st.pcb_x = 0;
	st.pcb_y = 0;
	st.pcb_w = st.width;
	st.pcb_h = g_vp_h;

	memset(&base, 0, sizeof(base));
	base.display = CSS_DISPLAY_BLOCK;
	base.font_size = 16;
	base.line_height = 19;
	base.color = 0x000000;
	base.face = 0;
	base.fstyle = 0;
	base.width = -1;
	base.min_width = -1;
	base.height = -1;
	base.max_width = -1;
	base.eff_bg = 0x00ffffffu;   /* the browser's default page sheet */

	if (dom_document_get_document_element(doc, &root) != DOM_NO_ERR ||
			root == NULL)
		return -1;

	walk(&st, root, NULL, &base, 0, st.width, 0);
	line_break(&st);

	/*
	 * Clamp every corner radius to half the shorter side, in ONE place, after
	 * the boxes have their final size. A box is sized when its subtree
	 * finishes, and that happens at three different points in walk() (block,
	 * atomic inline, and the per-line box of a wrapped inline), so clamping at
	 * each of them is three chances to forget. This is also what makes
	 * `border-radius: 999px` and `50%` mean "capsule" without either painter
	 * having to know that.
	 */
	{
		int i;
		for (i = 0; i < out->n_items; i++) {
			layout_item *it = &out->items[i];
			int lim;
			if (it->kind != 1 || it->radius <= 0) continue;
			lim = (it->w < it->h ? it->w : it->h) / 2;
			if (lim < 0) lim = 0;
			if (it->radius > lim) it->radius = lim;
		}
	}
	/*
	 * engabspos (#245): CSS stacking. Positioned boxes carry a nonzero zorder
	 * (ZORDER_BASE + z-index); in-flow content is 0. A stable sort by
	 * (zorder, original index) paints in-flow content first, then positioned
	 * boxes ordered by z-index with document order breaking ties, so a later
	 * or higher-z absolute box overlaps an earlier or lower one. The whole
	 * pass is SKIPPED unless some item is positioned, so a page with no
	 * positioned boxes is byte-identical (AE=0).
	 */
	{
		int i, any = 0, n = out->n_items;
		for (i = 0; i < n; i++)
			if (out->items[i].zorder != 0) { any = 1; break; }
		if (any && n > 1) {
			zsort_ent *zs = malloc((size_t) n * sizeof(zsort_ent));
			layout_item *tmp = malloc((size_t) n * sizeof(layout_item));
			if (zs && tmp) {
				for (i = 0; i < n; i++) {
					zs[i].key = out->items[i].zorder;
					zs[i].idx = i;
				}
				qsort(zs, (size_t) n, sizeof(zsort_ent), zsort_cmp);
				for (i = 0; i < n; i++)
					tmp[i] = out->items[zs[i].idx];
				memcpy(out->items, tmp, (size_t) n * sizeof(layout_item));
			}
			if (zs) free(zs);
			if (tmp) free(tmp);
		}
	}
	g_lp_walk = lrdtsc() - g_lp_walk;

	dom_node_unref(root);
	out->content_height = st.cursor_y;
	return 0;
}
