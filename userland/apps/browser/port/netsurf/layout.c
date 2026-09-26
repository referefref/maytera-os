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
 * engwalkstack (#245): THE SOURCE STAMP.
 *
 * WHY THIS EXISTS. The previous pass on this file measured a change that was
 * never compiled into the binary it measured, and its positive control came
 * back byte-identical, which is indistinguishable from a change that compiled
 * and did nothing (blame.md). Its check was "the two binaries differ", which
 * answers "is there ANY change in there" and says nothing about whether the
 * particular edit under measurement is. That gap only exists because nothing
 * in the artifact names the source it came from.
 *
 * The build passes -DENGWALK_LAYOUT_STAMP="<md5 of this file>" and the app
 * prints it at startup, so the serial log of any run states the md5 of the
 * layout.c that is actually executing, and it can be compared against the file
 * on disk. An unstamped build says so rather than lying.
 */
#ifndef ENGWALK_LAYOUT_STAMP
#define ENGWALK_LAYOUT_STAMP "unstamped"
#endif
const char *layout_src_stamp(void) { return ENGWALK_LAYOUT_STAMP; }

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
	 * Set the moment an item carrying a non-baseline vertical-align is emitted
	 * onto the line being filled, cleared every time a line closes (#245
	 * engvalign). It is the AE=0 gate: on a page that never authors
	 * vertical-align this stays false for every line and valign_line() returns
	 * before touching a single item.
	 */
	bool line_has_valign;
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
	/*
	 * engwalkstack (#245): walk() RECURSION DEPTH, and the flex working-set
	 * pool. Both exist to keep the layout walk inside the 2 MB user stack.
	 *
	 * depth is bumped by the walk() wrapper on entry and dropped on return,
	 * so it is exactly the number of walk frames live right now. A subtree
	 * deeper than WALK_MAX_DEPTH is DROPPED rather than allowed to overflow
	 * the stack; depth_peak records how close a page came.
	 *
	 * fp_free / fp_live are the LIFO pool the seven per-item flex arrays now
	 * live in, instead of walk()'s frame. Flex container nesting is strictly
	 * LIFO (a nested container's frame is released before its parent's), so
	 * the pool needs nothing more than a singly-linked free stack: no free
	 * list search, no fragmentation, no per-page reallocation.
	 */
	int depth;
	int depth_peak;
	struct flex_frame *fp_free;
	int fp_live;
	int fp_peak;
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

/*
 * engflex (#245): cap on flex items a single row container can align in a
 * paint pass; a container with more than this is left packed at the start
 * (correct, just unaligned) rather than aligned.
 *
 * RAISED 64 -> 256 (#245 engflexclose). 64 was sized for "real nav/toolbar
 * rows", and it does cover those, but a 100-cell product grid or icon wall is
 * an ordinary page and it fell off the cliff: the walk sets `ok = 0` at the
 * 65th element, which does not degrade the alignment, it turns off the ENTIRE
 * flex pipeline for that container. No flex-basis, no grow, no shrink, no
 * justify-content, no align-items, and no WRAP, so a `flex-wrap: wrap` grid of
 * 100 cells did not wrap as a flex container at all.
 *
 * THE COST IS STACK, AND IT IS PAID PER DOM NESTING LEVEL BY EVERY PAGE.
 * The six per-item arrays (bnd, cy, grow, shrink, basis, xc) live in walk()'s
 * frame, and walk() RECURSES once per element depth, so the cap multiplies the
 * frame whether or not the page contains a single flex container. MEASURED
 * with `gcc -fstack-usage` on this exact source, and the slope is exactly
 * 40 bytes per item of cap:
 *
 *   cap     walk() frame   flex_wrap_lines   max walk depth in a 2MB stack
 *    64       3936 B          3088 B            533
 *   128       6496 B          5904 B            323
 *   256      11616 B         11536 B            180
 *   512      21856 B         22800 B             96
 *  1024      42336 B         45328 B             49
 *
 * USER_STACK_SIZE is 2 MB (kernel/proc/process.h:29) and walk() has NO
 * RECURSION DEPTH GUARD, so that last column is a real cliff, not a budget.
 * 256 keeps a >4x margin over the deepest DOM a real page produces (the HTTP
 * Archive's extreme tail is around 100 levels) while covering a 250-cell grid.
 * 512 and beyond do NOT have that margin and must not be reached by editing
 * this number.
 *
 * THE THREE OPTIONS, and why this one:
 *   (a) RAISE THE CAP, this change. Memory cost: 40 bytes x cap x DOM depth of
 *       stack, on every page. Bounded and measured above. Cheap to 256.
 *   (b) FALL BACK GRACEFULLY past the cap, e.g. align the first N and leave the
 *       rest. Memory cost: ZERO. Rejected: it does not render a 100-item grid
 *       correctly either, it renders the first 64 cells aligned and the other
 *       36 somewhere else, which is worse to look at and much worse to debug
 *       than the uniform packing we have today.
 *   (c) HANDLE THEM PROPERLY: move the six per-item arrays off walk()'s frame
 *       into a LIFO bump pool on lstate (flex nesting is strictly LIFO, so a
 *       pool needs no free list). Memory cost: walk()'s frame DROPS to about
 *       1376 B, better than today's 3936, so the depth budget goes UP to
 *       ~1500 levels, and the cap stops being a stack question at all; the
 *       pool itself is 32 bytes x cap x flex NESTING depth of heap, which is
 *       kilobytes. That is the right end state and it is DEFERRED, not
 *       dismissed. Its gate: it changes no rendering at all when the pool
 *       allocation succeeds and the cap is unchanged, so its AE argument is
 *       the byte-identical dump of the whole existing corpus at cap 256,
 *       plus a new fixture at a cap the pool raises and walk's frame cannot.
 *
 * The one-shot leaf frames (flex_wrap_lines, flex_grow_pass, flex_shrink_pass)
 * also grow with the cap but are NOT multiplied by depth: flex_wrap_lines is
 * called after the child walk has returned, so at most one is ever live.
 */
#define FLEX_MAX_ITEMS 256

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

/*
 * Emit one positioned text run. Returns the item so a caller that knows
 * something extra about the run (engletsp: its letter-spacing) can stamp it,
 * or NULL when nothing was emitted. Callers that do not care ignore the
 * return, which is why adding it changed no existing call site.
 */
static layout_item *emit_run(lstate *st, const char *s, int len, int size,
		uint32_t color, int bold, int italic, int underline,
		int face, int fstyle)
{
	layout_item *it;
	if (len <= 0) return NULL;
	it = item_new(st);
	if (!it) return NULL;
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
	return it;
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
 * A flex line's CROSS SIZE: the largest bottom edge any of its items reaches,
 * measured from the line's top. The ONE place this engine computes that
 * number (#245 engnowrapstretch); flex_align, the wrap driver's per-line loop
 * and the nowrap stretch pass all call it, so the three cannot drift apart.
 * Split out unchanged from the two identical loops that were already here, so
 * the extraction is byte-identical by construction.
 *
 * A text run carries no h, so its cross size is its font size; a box carries
 * its border-box h. items[lo..hi) is the range to measure, which the callers
 * pass either as a whole flex container or as one line's slice of it.
 */
static int flex_line_cross(lstate *st, int lo, int hi, int line_top)
{
	int i, lh = 0;

	for (i = lo; i < hi; i++) {
		layout_item *it = &st->out->items[i];
		int ih = (it->kind == 0) ? it->size : it->h;
		int bot = (it->y - line_top) + ih;
		if (bot > lh)
			lh = bot;
	}
	return lh;
}

/*
 * Per-item CROSS-AXIS inputs for align-items:stretch (#245 engstretchitems)
 * and for per-item POSITIONAL align-self (#245 engflexclose).
 * Gathered from each flex item's OWN computed style during the (unchanged)
 * child walk, because by the time the wrap driver runs the items are a flat
 * array of positioned boxes and their styles no longer exist anywhere.
 *
 * INERT AT ZERO, DELIBERATELY. `stretch` is our own boolean, set only after a
 * POSITIVE check that the item's resolved align-self is stretch AND its cross
 * size is auto; it is never a raw libcss enum. libcss numbers this family
 * INHERIT = 0x0 with the INITIAL value at 0x1, so a raw enum living anywhere
 * that starts life zeroed reads as "inherit" and 0 stops meaning "do nothing".
 * That trap has bitten this engine twice, and it is at its most dangerous
 * here, because the INITIAL value of align-items is the ACTIVE one. min_h and
 * max_h are -1 for "not declared", so a zeroed struct also clamps nothing.
 *
 * `pos` is the same discipline applied to POSITIONAL align-self: FLEX_SELF_AUTO
 * is 0 and means "this item said nothing, use the line's keyword", which is
 * exactly what `align-self: auto`, the property's INITIAL value, means in CSS.
 * So a page that writes no align-self leaves every `pos` at 0 and cannot reach
 * a single new branch. A raw CSS_ALIGN_SELF_* here would be catastrophic:
 * CSS_ALIGN_SELF_INHERIT is 0x0 and CSS_ALIGN_SELF_STRETCH (the value the
 * cascade actually resolves `auto` to on most pages) is 0x1, so the zeroed
 * default would mean something and the initial value would arm the path.
 */
#define FLEX_SELF_AUTO   0   /* item said nothing: defer to the line keyword */
#define FLEX_SELF_START  1   /* top-packed: flex-start, baseline, stretch */
#define FLEX_SELF_CENTER 2
#define FLEX_SELF_END    3

typedef struct {
	int stretch;   /* 1 = grow this item's box to its line's cross size */
	int min_h;     /* px, -1 = none. Applied AFTER max_h, so it wins. */
	int max_h;     /* px, -1 = none */
	uint8_t pos;   /* FLEX_SELF_*, 0 = AUTO = use the container's align-items */
} flex_cross_t;

/*
 * engwalkstack (#245): THE FLEX PER-ITEM WORKING SET, OFF walk()'s FRAME.
 *
 * THE PROBLEM THIS SOLVES. The seven per-item arrays a row flex container
 * needs (bnd, cy, grow, shrink, basis, minf, xc) used to be plain locals in
 * walk(). walk() RECURSES once per DOM nesting level, so their combined size,
 * exactly 40 bytes per item of FLEX_MAX_ITEMS, was multiplied by the depth of
 * the page, and was paid by EVERY page whether or not it contained a single
 * flex container, because a frame is reserved on entry and gcc does not sink
 * an alloca-free array into the branch that uses it. MEASURED with
 * `gcc -fstack-usage` on this file: walk() was 3936 B at cap 64 and 11616 B at
 * cap 256, which took the deepest DOM that fits the 2 MB USER_STACK_SIZE from
 * 533 levels to 180. That made FLEX_MAX_ITEMS a stack question, and a cap that
 * is a stack question cannot be raised to cover a 250-cell product grid.
 *
 * WHY A LIFO POOL AND NOT PLAIN malloc/free PER CONTAINER. Flex containers
 * nest strictly: an inner container's frame is acquired after and released
 * before its parent's. So a checked-in frame can always be handed straight
 * back out, and the free list is a stack. A page with N nested flex containers
 * allocates N frames ONCE and reuses them for every sibling container after,
 * so a 500-container page still performs at most (max nesting) mallocs.
 *
 * WHAT IT COSTS. sizeof(flex_frame) is about 10 KB at cap 256, on the HEAP,
 * bounded by FLEX_MAX_NEST frames. A page with no flex container at all never
 * calls malloc here, which is what keeps the no-flex corpus byte-identical.
 *
 * WHAT HAPPENS WHEN IT CANNOT ALLOCATE. flex_frame_get() returns NULL and the
 * container takes the plain child walk: items are packed at the start,
 * correct but unaligned and unwrapped. That is EXACTLY the pre-existing
 * over-cap fallback (`ok = 0`), so the degraded rendering is one already-known
 * behaviour rather than a new one, and it is never a crash.
 */
#define FLEX_MAX_NEST 32

typedef struct flex_frame {
	struct flex_frame *link;   /* free-list next; dead while checked out */
	int          bnd[FLEX_MAX_ITEMS + 1];
	int          cy[FLEX_MAX_ITEMS];
	css_fixed    grow[FLEX_MAX_ITEMS];
	css_fixed    shrink[FLEX_MAX_ITEMS];
	int          basis[FLEX_MAX_ITEMS];
	int          minf[FLEX_MAX_ITEMS];
	flex_cross_t xc[FLEX_MAX_ITEMS];
} flex_frame;

static flex_frame *flex_frame_get(lstate *st)
{
	flex_frame *f;
	if (st->fp_live >= FLEX_MAX_NEST) return NULL;
	f = st->fp_free;
	if (f) {
		st->fp_free = f->link;
	} else {
		f = (flex_frame *) malloc(sizeof *f);
		if (!f) return NULL;
	}
	f->link = NULL;
	st->fp_live++;
	if (st->fp_live > st->fp_peak) st->fp_peak = st->fp_live;
	return f;
}

static void flex_frame_put(lstate *st, flex_frame *f)
{
	if (!f) return;
	f->link = st->fp_free;
	st->fp_free = f;
	st->fp_live--;
}

static void flex_pool_drain(lstate *st)
{
	while (st->fp_free) {
		flex_frame *f = st->fp_free;
		st->fp_free = f->link;
		free(f);
	}
	st->fp_live = 0;
}

/*
 * engwalkstack (#245): THE walk() RECURSION DEPTH CEILING.
 *
 * walk() had no depth guard at any cap, so a page nested deeply enough ran the
 * user stack off its end: not a diagnosable failure, a fault at whatever
 * unrelated code the corrupted frame returned into.
 *
 * SIZED AGAINST THE FRAME THIS CHANGE PRODUCES, not the old one, and against
 * a MEASURED frame rather than an estimated one. `gcc -fstack-usage` on this
 * file, after the pool, reports:
 *
 *   cap     walk_node frame   flex_wrap_lines
 *     64        2544 B            3088 B
 *    128        2544 B            5904 B
 *    256        2544 B           11536 B   <- shipped
 *    512        2544 B           22800 B
 *   1024        2544 B           45328 B
 *   4096        2544 B          180496 B
 *
 * The recursive frame's slope against the cap is now exactly ZERO, where it
 * used to be 40 bytes per item. FLEX_MAX_ITEMS has stopped being a stack
 * question, which was the whole point. (flex_wrap_lines still scales, but it
 * runs AFTER the child walk has returned, so at most ONE is ever live no
 * matter how deep or how nested the page: it is a one-off, not a per-level
 * cost. Nothing it calls re-enters walk. It is what bounds how far the cap
 * could be pushed from here, and that bound is now thousands, not hundreds.)
 *
 * THE ARITHMETIC FOR 256. USER_STACK_SIZE is 2 MB (kernel/proc/process.h:29).
 * 256 x 2544 = 651,264 B, 31% of it. The remaining 1.4 MB covers the single
 * live flex_wrap_lines, the leaf passes under it (flex_shrink_pass is the
 * largest at 7248 B), the text/measure path, and every frame beneath
 * layout_document in the app. A third of the stack for the recursion is a
 * margin, not a budget that just happens to fit.
 *
 * AND 256 IS MORE THAN THE PAGE COULD SURVIVE BEFORE THIS CHANGE: at cap 256
 * with the arrays on the frame, 2 MB ran out at 180 levels, with nothing
 * checking. It is also ~2.5x the deepest DOM real pages produce (the HTTP
 * Archive's extreme tail sits near 100 levels). A page that trips this is
 * pathological or hostile, which is exactly the case that must fail cleanly
 * instead of running the stack off its end.
 */
#define WALK_MAX_DEPTH 256

/*
 * Map the container's align-items keyword into the same FLEX_SELF_* space the
 * per-item override lives in, so flex_cross_place compares ONE kind of value.
 * Everything this engine does not place (flex-start, baseline, stretch, and
 * any future libcss value) lands on FLEX_SELF_START, which is the top-packed
 * behaviour that was already there.
 */
static uint8_t flex_self_of_align(uint8_t align)
{
	if (align == CSS_ALIGN_ITEMS_CENTER)   return FLEX_SELF_CENTER;
	if (align == CSS_ALIGN_ITEMS_FLEX_END) return FLEX_SELF_END;
	return FLEX_SELF_START;
}

/*
 * Place a flex line's items on the CROSS axis inside a line of a GIVEN cross
 * size, per align-items, WITH A PER-ITEM align-self OVERRIDE (#245
 * engflexclose). Only center and flex-end MOVE anything; flex-start,
 * baseline and stretch keep the top-packed placement. That is still right for
 * stretch now that it GROWS the items (#245 engstretchitems): a stretched item
 * is exactly as tall as its line, so it starts at the line top whatever this
 * function would compute. Growing it is flex_stretch_items' job, above.
 *
 * PER ITEM, NOT PER LINE (#245 engflexclose). The alignment applied to item k
 * is `xc[k].pos` when the item declared a positional align-self, and the
 * container's align-items otherwise. That is the whole of what align-self
 * does on the cross axis, and it is an EXTENSION of this one primitive rather
 * than a second placement formula: the dy arithmetic below is untouched, only
 * the keyword feeding it is now chosen per item. `xc` may be NULL for a caller
 * that has no per-item data, in which case every item takes the line keyword
 * and the behaviour is exactly what it was.
 *
 * AE=0 BY CONSTRUCTION, and this is the unusual case where the gate SURVIVES
 * the implementation rather than being spent by it: `auto` is align-self's
 * initial value and means "use the container's", so a page that does not write
 * the property computes FLEX_SELF_AUTO on every item, the early-return below
 * fires under exactly the old condition, and each surviving item takes exactly
 * the keyword it took before. There is no keyword a page can stop writing to
 * get the old behaviour back, because the old behaviour IS the no-keyword
 * behaviour.
 *
 * Split out of flex_align (#245 engstretch) so align-content:stretch, which
 * GROWS a line's cross size, can re-place that line's items against the new
 * height with the SAME arithmetic rather than a second copy of it. There is
 * one cross-placement formula in this engine, not two.
 *
 * The placement is ABSOLUTE, not incremental: dy is measured from each item's
 * own current bounding box to the target, so calling this twice on a line with
 * the same line_cross is a no-op, and calling it again with a LARGER one lands
 * the items exactly where a single call with that larger value would have.
 * That is what makes the stretch path safe to run after flex_align has already
 * centred the line at its natural height.
 *
 * bnd[0..cnt] are this line's item boundaries; the caller may pass a slice of a
 * larger array (bnd + line_start), because only the differences matter.
 */
static void flex_cross_place(lstate *st, int line_top, int line_cross,
		uint8_t align, const flex_cross_t *xc, const int *bnd, int cnt)
{
	int i, k;
	uint8_t lineal = flex_self_of_align(align);
	int any = (lineal != FLEX_SELF_START);

	/* Early out under EXACTLY the old condition when no item overrides:
	 * the line keyword places nothing and no align-self asks for anything
	 * either, so there is nothing to do. This is the branch every page that
	 * writes neither property takes. */
	for (k = 0; !any && xc && k < cnt; k++)
		if (xc[k].pos == FLEX_SELF_CENTER || xc[k].pos == FLEX_SELF_END)
			any = 1;
	if (!any)
		return;
	for (k = 0; k < cnt; k++) {
		int top = 0x3fffffff, bot = 0, item_cross, dy;
		uint8_t a = (xc && xc[k].pos != FLEX_SELF_AUTO)
			? xc[k].pos : lineal;
		if (a != FLEX_SELF_CENTER && a != FLEX_SELF_END)
			continue;
		for (i = bnd[k]; i < bnd[k + 1]; i++) {
			layout_item *it = &st->out->items[i];
			int ih = (it->kind == 0) ? it->size : it->h;
			if (it->y < top) top = it->y;
			if (it->y + ih > bot) bot = it->y + ih;
		}
		if (bot <= top)
			continue;
		item_cross = bot - top;
		dy = (a == FLEX_SELF_CENTER)
			? (line_top + (line_cross - item_cross) / 2) - top
			: (line_top + (line_cross - item_cross)) - top;
		if (dy)
			for (i = bnd[k]; i < bnd[k + 1]; i++)
				st->out->items[i].y += dy;
	}
}

/*
 * align-items / align-self STRETCH: grow a stretching item's principal box on
 * the CROSS axis so it fills its line (#245 engstretchitems). This is the step
 * that makes the items on a flex line the same height, which is what CSS does
 * BY DEFAULT, because stretch is align-items' INITIAL value.
 *
 * Until now an item's cross size was whatever the item measured, and growing a
 * LINE (align-content:stretch, #245 engstretch) moved the items inside it but
 * never grew them. Growing the line and growing the item are two different
 * steps; this is the second one.
 *
 * ABSOLUTE, NOT INCREMENTAL, AND GROW-ONLY, which is what makes it safe to
 * call twice on the same line: the target is measured from the item's own box
 * top to the line's bottom edge, so a second call with a LARGER line_cross
 * lands the box exactly where a single call with that value would have, and a
 * second call with the same one does nothing. align-content:stretch depends on
 * that, because it grows the line AFTER the per-line pass already filled it.
 * It is the same absolute-placement property flex_cross_place has, for the
 * same reason.
 *
 * BOUNDED:
 *   - only an item whose xc[k].stretch the caller POSITIVELY set, i.e. whose
 *     cross size is AUTO and whose resolved align-self is stretch. An item
 *     with a declared height keeps it (CSS: a definite cross size does not
 *     stretch), and an explicit align-self of flex-start / center / flex-end
 *     opts that item out even inside a stretch container.
 *   - only the item's PRINCIPAL BOX, its first kind-1 box. A text-only item
 *     has no box to grow, and a REPLACED item (kind 3 image) is deliberately
 *     left alone rather than stretched out of its aspect ratio; both are on
 *     the deferred list in docs/BROWSER_ENGINE_FLEXWRAP_PLAN.md.
 *   - max-height clamps the growth DOWN and min-height then floors it, IN
 *     THAT ORDER, because CSS applies the minimum last and it wins.
 *   - h only ever INCREASES, so an item already reaching the line's bottom is
 *     untouched.
 *
 * The item box's `h` is its BORDER box, so min/max-height are spent here as
 * border-box lengths: exact under box-sizing:border-box, and off by the item's
 * vertical padding+border otherwise. That is the same bounded approximation
 * flex_basis_pass documents for flex-basis on the main axis.
 *
 * RETURNS the number of boxes it actually grew (#245 engnowrapstretch), so a
 * caller can tell "this pass changed nothing" from "this pass ran". The nowrap
 * call site uses it to decide whether the container's pen has to be raised;
 * the wrap driver ignores it and is unaffected.
 */
static int flex_stretch_items(lstate *st, int line_top, int line_cross,
		const int *bnd, const flex_cross_t *xc, int ks, int ke)
{
	int k, i, n = 0;

	if (st->measuring)
		return 0;
	for (k = ks; k < ke; k++) {
		int bi = -1, target;
		if (!xc[k].stretch)
			continue;
		for (i = bnd[k]; i < bnd[k + 1]; i++)
			if (st->out->items[i].kind == 1) { bi = i; break; }
		if (bi < 0)
			continue;
		target = (line_top + line_cross) - st->out->items[bi].y;
		if (xc[k].max_h >= 0 && target > xc[k].max_h)
			target = xc[k].max_h;
		if (xc[k].min_h >= 0 && target < xc[k].min_h)
			target = xc[k].min_h;
		if (target > st->out->items[bi].h) {
			st->out->items[bi].h = target;
			n++;
		}
	}
	return n;
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
		const int *bnd, const int *cy, const flex_cross_t *xc, int cnt)
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

	/* Cross axis: position each item within the line per align-items, with a
	 * per-item align-self override (#245 engflexclose). Only center and
	 * flex-end move anything; stretch/flex-start/baseline keep the existing
	 * top-packed behaviour. A flex item's cross-size is the bounding box of
	 * its own items (text runs carry no h, so their cross-size is the font
	 * size).
	 *
	 * THE ARMING TEST IS NOW "THE LINE KEYWORD PLACES, OR SOME ITEM DOES",
	 * not just the line keyword, because align-self:center inside a default
	 * (stretch) container has to reach the primitive. flex_cross_place makes
	 * exactly the same test again and returns immediately when it fails, so
	 * this one only saves the flex_line_cross scan; the two cannot disagree
	 * about when placement happens because the condition is written once
	 * here and once there in the same terms. */
	{
		int place = (flex_self_of_align(align) != FLEX_SELF_START);
		for (k = 0; !place && xc && k < cnt; k++)
			if (xc[k].pos == FLEX_SELF_CENTER ||
					xc[k].pos == FLEX_SELF_END)
				place = 1;
		if (place) {
			int line_cross = flex_line_cross(st, fstart, fend, line_top);
			flex_cross_place(st, line_top, line_cross, align,
					xc, bnd, cnt);
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
 *   - the AUTOMATIC MINIMUM SIZE (#245 engflexmin). CSS floors every flex item
 *     at min-width:auto = its min-content size, and the distribution that
 *     honours it is a FREEZE-AT-MIN LOOP, not a single clamp. minf[k] carries
 *     that floor, measured by flex_basis0_pass BEFORE it collapsed the box
 *     (the same flex_item_min_content() primitive the shrink pass uses), and is
 *     0 for every item the seed did not collapse. A zero floor can never bind
 *     here because growth is non-negative and a non-seeded item enters at its
 *     content width, so the loop resolves on round 0 with exactly the
 *     pre-engflexmin arithmetic and the item list is byte-identical.
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
		const css_fixed *grow, const int *minf, int cnt)
{
	int k, i, freev, cum, distributed, last_grow;
	int last_free, remaining, iter;
	css_fixed sum;
	int ek[FLEX_MAX_ITEMS], bi[FLEX_MAX_ITEMS];
	int base[FLEX_MAX_ITEMS], froz[FLEX_MAX_ITEMS];

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

	/* Each item's principal box and its FLEX BASE SIZE, the width it carries
	 * on entry. flex_basis0_pass collapsed every `flex: N` seeded box to 0, so
	 * for those the base is 0 and the whole container main size arrives here
	 * as free space; that is precisely the case a floor has to police. */
	for (k = 0; k < cnt; k++) {
		bi[k] = -1;
		base[k] = 0;
		froz[k] = 0;
		for (i = bnd[k]; i < bnd[k + 1]; i++) {
			layout_item *it = &st->out->items[i];
			if (it->kind == 1 || it->kind == 3) { bi[k] = i; break; }
		}
		if (bi[k] >= 0)
			base[k] = st->out->items[bi[k]].w;
	}

	/*
	 * Per-item growth in integer px, floored at the AUTOMATIC MINIMUM SIZE
	 * (#245 engflexmin; CSS flexbox 9.7 "resolve flexible lengths", step 4).
	 * This is the FULL freeze-at-min loop, not a single clamp: an item whose
	 * proportional share falls below minf[k] is set to its floor and FROZEN,
	 * and because a frozen item then eats MORE than its share, the space left
	 * for the rest shrinks, which can push a SECOND item under its own floor.
	 * Repeat until a round finds no new violation.
	 *
	 * HARD BOUND: a round that continues freezes at least one item, and there
	 * are at most cnt <= FLEX_MAX_ITEMS items, so cnt + 1 rounds is a bound
	 * the loop cannot need, let alone exceed.
	 *
	 * INERT WITHOUT A FLOOR, which is the whole AE=0 argument: minf[k] is 0
	 * for every item flex_basis0_pass did not collapse and base[k] >= 0, so
	 * `need = minf[k] - base[k]` is <= 0 and can never exceed a non-negative
	 * share. Round 0 therefore finds no violation, with remaining == freev and
	 * live == sum, which is exactly the single-shot formula this pass used
	 * before, and breaks.
	 */
	remaining = freev;
	for (k = 0; k < cnt; k++)
		ek[k] = 0;
	for (iter = 0; iter <= cnt; iter++) {
		css_fixed live = 0;
		int viol = 0;
		for (k = 0; k < cnt; k++)
			if (grow[k] > 0 && bnd[k + 1] > bnd[k] && !froz[k])
				live += grow[k];
		if (live <= 0)
			break;
		for (k = 0; k < cnt; k++) {
			int share, need;
			if (froz[k] || grow[k] <= 0 || bnd[k + 1] <= bnd[k])
				continue;
			share = (remaining > 0)
				? FIXTOINT(FDIV(FMUL(INTTOFIX(remaining),
						grow[k]), live))
				: 0;
			if (share < 0) share = 0;
			need = minf[k] - base[k];
			if (need > share) {
				ek[k] = need;
				froz[k] = 1;
				viol += need;
			} else {
				ek[k] = share;
			}
		}
		if (!viol)
			break;
		remaining -= viol;
	}

	/* The rounding remainder goes to the last STILL-FLEXIBLE growing item so
	 * the line fills the container exactly (no residual gap). With no floor
	 * nothing is frozen, so that is the last growing item, exactly as before.
	 * If the floors already overshot the container the line legitimately
	 * OVERFLOWS, which is what a real browser does, and nothing is added. */
	distributed = 0;
	last_free = -1;
	for (k = 0; k < cnt; k++) {
		if (grow[k] > 0 && bnd[k + 1] > bnd[k]) {
			distributed += ek[k];
			if (!froz[k])
				last_free = k;
		}
	}
	if (distributed < freev)
		ek[(last_free >= 0) ? last_free : last_grow] +=
			(freev - distributed);

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
			if (bi[k] >= 0)
				st->out->items[bi[k]].w += ek[k];
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
 *     initial value) OR resolves to < 0. Those keep their content-based size,
 *     so a container with no explicit positive flex-basis is byte-identical.
 *     A basis of exactly 0 carries the distinct 0 sentinel (the `flex: N`
 *     shorthand seed) and is likewise NOT touched here: `basis[k] > 0` is
 *     false for it, and flex_basis0_pass handles it on the single-line path
 *     only. A page's item list therefore cannot change here unless it authored
 *     a positive flex-basis this engine previously ignored.
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

/*
 * Forward: the bounded automatic-minimum-size measure, defined below next to
 * the shrink pass that has always used it. The basis-0 seed has to take each
 * item's floor BEFORE it collapses the box to zero, so it needs the prototype
 * here (#245 engflexmin). ONE definition, two callers; no private copy.
 */
static int flex_item_min_content(lstate *st, int lo, int hi, int bi, int curw);

/*
 * Flexbox main-axis SIZING: the basis-0 GROW SEED of the `flex: N` shorthand
 * (#245 engflexbasis). `flex: 1` expands to `flex-grow:1; flex-shrink:1;
 * flex-basis:0%`, and the ZERO basis is the whole point of the idiom: every
 * item starts at zero main size, so the ENTIRE container main size is handed to
 * the grow factors and a row of unequal text comes out as EQUAL columns.
 * Seeding from content width instead (what this engine did before) leaves
 * `flex: 1` items content-sized with only the leftover distributed, which is
 * visibly wrong on the commonest flexbox idiom on the real web.
 *
 * Runs AFTER flex_basis_pass (which handles an explicit POSITIVE length and
 * deliberately ignores a 0) and BEFORE flex_grow_pass: it collapses each seeded
 * item's principal box to zero width and shifts the following items left by the
 * running total, and grow then immediately redistributes the whole container
 * main size by grow factor.
 *
 * BOUNDED, and never a regression by CONSTRUCTION:
 *   - basis[k] == 0 is a sentinel the caller sets ONLY for an item whose
 *     computed flex-basis is CSS_FLEX_BASIS_SET, resolves to exactly 0px AND
 *     whose flex-grow is > 0. Longhand `flex-grow: 1` leaves flex-basis AUTO
 *     (libcss reports AUTO, not SET), and so do `flex: auto` and `flex: none`,
 *     so none of them reach here. Every other item keeps the -1 (auto/content)
 *     sentinel and this returns 0 having touched nothing, so a page that never
 *     writes the `flex` shorthand is byte-identical.
 *   - ALL OR NOTHING. If any seeded item has no principal box to collapse (a
 *     text-only flex item), the line cannot be equalised correctly, so the seed
 *     is abandoned entirely and the line renders exactly as it did before.
 *   - NEVER LEAVES A COLLAPSED LINE. The seed is applied only when the
 *     resulting line leaves STRICTLY POSITIVE free space, i.e. only when
 *     flex_grow_pass is guaranteed to fire and hand that space straight back.
 *     Otherwise this returns 0 and the old content-sized behaviour stands, so
 *     an over-constrained container can never be left with zero-width boxes.
 *   - ONE LINE PER CALL: bail if any item started below line_top. The
 *     single-line caller passes the whole container; flex_wrap_lines calls
 *     this ONCE PER LINE, on that line's sub-slice, after it has repacked the
 *     line at line_top (#245 engflexwrap2). Wrapping on the HYPOTHETICAL main
 *     size, which is what made the wrap path wait for engflexmin, is done by
 *     the caller: it measures the same floor before it breaks lines and uses
 *     max(basis, floor) there.
 *   - an item's INNER content is not re-wrapped or re-centred at the new width,
 *     the same bound every other pass here carries.
 *
 * MEASURES THE AUTOMATIC MINIMUM SIZE ON THE WAY PAST (#245 engflexmin). A
 * collapsed item's floor MUST be taken here and not in flex_grow_pass, because
 * flex_item_min_content() derives the item's own chrome from its border-box
 * width and clamps the answer to it; at w == 0 both are meaningless. minf[k]
 * is written ONLY for an item this pass actually collapsed and only on the
 * apply path, so every early return leaves the caller's zero-filled array
 * untouched and grow cannot see a floor that does not exist.
 * Returns the net px the packed line end moved (<= 0).
 */
static int flex_basis0_pass(lstate *st, int line_top, int content_x,
		int content_w, int used_x, const int *bnd, const int *cy,
		const int *basis, const css_fixed *grow, int cnt, int *minf)
{
	int k, i, cum = 0, nseed = 0, total = 0;
	int bx[FLEX_MAX_ITEMS];

	if (st->measuring || cnt < 1)
		return 0;
	for (k = 0; k < cnt; k++)
		if (cy[k] != line_top)
			return 0;

	/* Collect the seeded items and their principal boxes. An empty item
	 * range (display:none) is simply not a flex item and is skipped, exactly
	 * as the grow pass skips it; a seeded item with NO box aborts the whole
	 * seed rather than half-applying it. */
	for (k = 0; k < cnt; k++) {
		bx[k] = -1;
		if (basis[k] != 0 || grow[k] <= 0 || bnd[k + 1] <= bnd[k])
			continue;
		for (i = bnd[k]; i < bnd[k + 1]; i++) {
			layout_item *it = &st->out->items[i];
			if (it->kind == 1 || it->kind == 3) { bx[k] = i; break; }
		}
		if (bx[k] < 0)
			return 0;
		total += st->out->items[bx[k]].w;
		nseed++;
	}
	if (nseed == 0 || total <= 0)
		return 0;
	/* Only seed when grow is then guaranteed to give the space back. */
	if (content_w - ((used_x - total) - content_x) <= 0)
		return 0;

	for (k = 0; k < cnt; k++) {
		if (cum)
			for (i = bnd[k]; i < bnd[k + 1]; i++)
				st->out->items[i].x += cum;
		if (bx[k] >= 0) {
			/* Floor first, while the box still has its content
			 * width; then collapse. (#245 engflexmin) */
			minf[k] = flex_item_min_content(st, bnd[k], bnd[k + 1],
					bx[k], st->out->items[bx[k]].w);
			cum -= st->out->items[bx[k]].w;
			st->out->items[bx[k]].w = 0;
		}
	}
	return cum;
}

/* Forward: text measure (defined later); needed by the min-content floor. */
static int lmeasure(lstate *st, const char *s, int size, int face, int fstyle);

/*
 * The DRAWN width of an already-emitted text run (#245 engletsp).
 *
 * Several passes (flex spans, overflow clipping, table column probes) recover
 * a run's right edge by re-measuring its text. Once letter-spacing exists, the
 * bare measure is no longer the width the painter will cover, so every one of
 * those sites has to ask this instead or a tracked heading reads as narrower
 * than it is drawn and gets clipped or mis-packed.
 *
 * STRICTLY EQUAL to lmeasure() when letter_spacing is 0, which is every run on
 * a page that never authors the property, so the substitution is inert there.
 * The glyph count is the byte length on purpose: the run text is single-byte
 * Latin-1 after utf8_squash() and the rasteriser indexes it by byte.
 */
static int item_run_w(lstate *st, const layout_item *it)
{
	int w = lmeasure(st, it->text, it->size, it->face, it->fstyle);
	if (it->letter_spacing) {
		int n = 0;
		while (it->text[n]) n++;
		w += it->letter_spacing * n;
	}
	return w;
}

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
					/* engletsp: an unbreakable word is drawn
					 * with the run's tracking, so the floor
					 * has to include it. Inert at 0. */
					if (it->letter_spacing)
						ww += it->letter_spacing * n;
					if (ww > widest) widest = ww;
				}
				a = (s[b] == ' ') ? b + 1 : b;
			}
			r = it->x + item_run_w(st, it);
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
			r = it->x + item_run_w(st, it);   /* engletsp */
		else
			r = it->x + it->w;
		if (l < minx) minx = l;
		if (r > maxx) maxx = r;
	}
	*pminx = minx; *pmaxx = maxx;
}

/*
 * FLEX-DIRECTION: ROW-REVERSE (#245 engrowrev), as a MAIN-AXIS REFLECTION of
 * the finished `row` layout. This is the main-axis mirror image of the
 * cross-axis reflection engwraprev added for flex-wrap:wrap-reverse, and it is
 * the same trick for the same reason.
 *
 * Per CSS Flexbox 1 section 5.1, row-reverse uses the SAME axis as row and
 * differs only in that main-start and main-end are SWAPPED. Nothing about
 * SIZING changes: flex-basis, the automatic minimum size, grow, shrink and the
 * greedy line break all read main SIZES, never main positions, so they produce
 * the identical numbers. Nothing on the CROSS axis changes either: align-items,
 * align-self, align-content, per-item stretch and the row gap are untouched.
 * Only where each item SITS along the main axis differs, so reflecting the
 * finished forward result is not an approximation of a second code path, it IS
 * the second code path.
 *
 * THE EQUIVALENCE IS EXACT, INCLUDING THE INTEGER ROUNDING. Every placement in
 * this engine truncates toward zero, so the thing worth checking rather than
 * assuming is whether "run the formulas then reflect" differs from "run the
 * formulas in the reflected frame". It does not. Every main-axis pass computes
 * one number for item k: the distance from MAIN-START to the item's LEADING
 * edge, `(packed offset) + dx(k)`. A forward pass adds that to content_x and
 * the leading edge is the left one; a reverse pass subtracts it from
 * content_x + content_w and the leading edge is the right one. The reflection
 * below maps the first to the second exactly, because reflecting an interval
 * maps its left edge's distance from the left of the box to its right edge's
 * distance from the right of the box. The truncation happens in dx(k), before
 * either frame is chosen, so there is nothing left to round differently.
 *
 *   justify-content:center in a 47px box on an 18px item. Forward
 *   dx = (47 - 18) / 2 = 14, so the item occupies offsets 14..32. Reflected it
 *   occupies 47 - 32 .. 47 - 14 = 15..33. A native reverse pass computes the
 *   same (47 - 18) / 2 = 14 from the RIGHT edge, putting the leading (right)
 *   edge at 47 - 14 = 33 and the left at 15: offsets 15..33. Identical, with
 *   the odd spare pixel correctly moved to the other end. Fixture sections 3
 *   and 4 measure exactly this pair.
 *   space-between anchors item 0 flush with main-start and the last item flush
 *   with main-end; after reflection that is the right and left edges, which is
 *   what CSS asks for. space-around and space-evenly are symmetric
 *   distributions of the same running-total shape.
 *
 * OVERFLOW REFLECTS TOO, and is right for free: a packed line wider than the
 * container overflows to the RIGHT forward, and the reflection turns that into
 * an overflow to the LEFT, which is main-end under row-reverse.
 *
 * REFLECTING THE POSITIONS IS NOT THE SAME AS REFLECTING EVERY EMITTED ITEM,
 * and that is the one real trap, exactly as it was on the cross axis. A flex
 * item is a RANGE of layout_items, bnd[k]..bnd[k+1], holding its principal
 * box, its text runs and any nested boxes. Reflecting each layout_item about
 * the container midline individually would mirror the item's text runs among
 * themselves, turning the first word of a line into its last: correct for the
 * item as a whole, catastrophic inside it, and MORE visible here than on the
 * cross axis because words differ in width where lines of text do not differ
 * in height. So the reflection is computed on each ITEM's bounding box, with
 * the flex_item_span() primitive the wrap driver already measures items with,
 * and applied as a RIGID TRANSLATION of that item's whole range. Text inside
 * an item stays left-to-right, which is what a real browser does:
 * flex-direction does not reverse text.
 *
 * WHAT IT REFLECTS ABOUT is [content_x, content_x + content_w), which is this
 * element's own main-axis content box and is the SAME pair flex_align() already
 * measures justify-content's free space against. There is one notion of the
 * container's main box in this engine, not two.
 *
 * THERE IS NO PEN TO REPAIR, which is where this is genuinely simpler than the
 * cross-axis case. engwraprev had to recompute last_top/last_h because the
 * cross axis IS the block-progression axis; the main axis is not, the
 * container's used width and height are unchanged by a reflection, and
 * cursor_y / line_height are untouched. That is also why this can live at ONE
 * call site covering BOTH the wrap and the nowrap paths rather than being
 * plumbed into flex_wrap_lines(): the reflection is per ITEM and every line of
 * a wrap container spans the same main-axis interval.
 *
 * ANONYMOUS FLEX ITEMS: bare text directly inside a flex container gets no
 * bnd[] entry of its own, so it is not an item here. It is NOT, however, left
 * behind: the recording loop sets bnd[k] to n_items as element k OPENS, so a
 * bare run between two elements falls inside the PRECEDING item's range and is
 * dragged rigidly with it (and a run before the first element falls inside
 * item 0's, because bnd[0] is fstart). It therefore moves, and it widens that
 * neighbour's measured span. Fixture section 12 measures this rather than
 * assuming it. See docs/BROWSER_ENGINE_FLEXWRAP_PLAN.md for the deferral and
 * its gate.
 */
static void flex_main_reflect(lstate *st, int content_x, int content_w,
		const int *bnd, int cnt)
{
	int k, i;
	/* x + x' == axis for any reflected edge. */
	int axis = 2 * content_x + content_w;

	if (st->measuring || cnt < 1)
		return;

	for (k = 0; k < cnt; k++) {
		int minx, maxx, dx;
		if (bnd[k] >= bnd[k + 1])
			continue;
		flex_item_span(st, bnd[k], bnd[k + 1], &minx, &maxx);
		if (maxx <= minx)
			continue;
		dx = axis - maxx - minx;
		if (dx)
			for (i = bnd[k]; i < bnd[k + 1]; i++)
				st->out->items[i].x += dx;
	}
}

/*
 * align-content (#245 engaligncontent): the CROSS-AXIS keyword that says what
 * a multi-line flex container does with the space its LINES do not fill.
 *
 * OUR OWN CONSTANTS, WITH THE INERT CASE AT 0, DELIBERATELY. libcss numbers
 * this enum CSS_ALIGN_CONTENT_INHERIT = 0x0 with the INITIAL value (STRETCH)
 * at 0x1, so a raw libcss enum carried anywhere that starts life zeroed reads
 * as "inherit" rather than as "nothing to do", and 0 stops meaning inert. The
 * constants below put DO-NOTHING at 0 and everything this engine does not
 * handle falls through the switch's default onto it.
 *
 * STRETCH NOW SHIPS (#245 engstretch) and is the one value that is not a
 * rigid translation: it GROWS each line's cross size by an equal share of the
 * leftover space. It is align-content's INITIAL value, so it is the one
 * keyword here that arms WITHOUT being written, and FLEX_AC_START stops being
 * the value an unstyled container maps to. What still holds the blast radius
 * down is the OTHER gate, cross_h >= 0: a flex container with an auto height
 * grows to exactly its content and has no leftover cross space at all, so
 * every flex container that does not declare a height is untouched. What
 * stretch does NOT yet do is grow the ITEMS inside the grown line, because
 * align-items:stretch cross-SIZE growth is still deferred; see the limit note
 * on flex_wrap_lines and in docs/BROWSER_ENGINE_FLEXWRAP_PLAN.md.
 */
#define FLEX_AC_START   0
#define FLEX_AC_END     1
#define FLEX_AC_CENTER  2
#define FLEX_AC_BETWEEN 3
#define FLEX_AC_AROUND  4
#define FLEX_AC_EVENLY  5
#define FLEX_AC_STRETCH 6

/*
 * CROSS-AXIS DIRECTION for flex_wrap_lines (#245 engwraprev). flex-wrap:wrap
 * puts cross-start at the TOP and stacks the lines downwards; wrap-reverse
 * SWAPS the cross-start and cross-end edges, so the lines stack bottom-to-top
 * and flex-start / flex-end mean the opposite ends of both align-items and
 * align-content.
 *
 * OUR OWN CONSTANTS, WITH THE INERT CASE AT 0, for the same reason FLEX_AC_*
 * above has its own: libcss numbers this family CSS_FLEX_WRAP_INHERIT = 0x0
 * with the INITIAL value (NOWRAP) at 0x1, so a raw libcss enum living anywhere
 * that starts life zeroed reads as "inherit" rather than "nothing to do" and 0
 * stops meaning inert. FLEX_WRAP_FWD is today's behaviour exactly.
 */
#define FLEX_WRAP_FWD   0
#define FLEX_WRAP_REV   1

/*
 * MAIN-AXIS DIRECTION (#245 engrowrev). flex-direction:row runs the main axis
 * left-to-right; row-reverse SWAPS main-start and main-end, so items are
 * placed right-to-left and justify-content:flex-start means the RIGHT edge.
 *
 * OUR OWN CONSTANTS, WITH THE INERT CASE AT 0, for the same reason FLEX_AC_*
 * and FLEX_WRAP_* above have their own: libcss numbers this family
 * CSS_FLEX_DIRECTION_INHERIT = 0x0 with the INITIAL value (ROW) at 0x1, so a
 * raw libcss enum living anywhere that starts life zeroed reads as "inherit"
 * rather than "nothing to do" and 0 stops meaning inert. FLEX_ROW_FWD is
 * today's behaviour exactly.
 */
#define FLEX_ROW_FWD    0
#define FLEX_ROW_REV    1

static uint8_t flex_align_content_of(uint8_t v)
{
	switch (v) {
	case CSS_ALIGN_CONTENT_FLEX_END:      return FLEX_AC_END;
	case CSS_ALIGN_CONTENT_CENTER:        return FLEX_AC_CENTER;
	case CSS_ALIGN_CONTENT_SPACE_BETWEEN: return FLEX_AC_BETWEEN;
	case CSS_ALIGN_CONTENT_SPACE_AROUND:  return FLEX_AC_AROUND;
	case CSS_ALIGN_CONTENT_SPACE_EVENLY:  return FLEX_AC_EVENLY;
	case CSS_ALIGN_CONTENT_STRETCH:       return FLEX_AC_STRETCH;
	/* INHERIT and FLEX_START keep the existing top-packed stack. The inert
	 * default is the point: any keyword this engine does not model, present
	 * or future, lands on today's behaviour rather than on an arbitrary one.
	 * Note that a COMPUTED style never carries INHERIT, so the 0x0 case is
	 * belt and braces, not a live path. */
	default:                              return FLEX_AC_START;
	}
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
 * GATED: only ever called when css_computed_flex_wrap is WRAP or (since #245
 * engwraprev) WRAP-REVERSE, so a nowrap/default flex row and non-flex pages
 * never enter here and stay byte-identical (AE=0). A single item wider than
 * the container keeps its own line (CSS: an item that cannot fit is not
 * split). Lines stack top-to-bottom, or bottom-to-top under wrap-reverse.
 *
 * FLEX-WRAP:WRAP-REVERSE (#245 engwraprev) is `reverse`, and it is the LAST
 * thing this function does: a cross-axis REFLECTION of the finished forward
 * layout. wrap-reverse wraps into exactly the same lines as wrap and differs
 * only in that cross-start and cross-end are swapped, so a reflection is not
 * an approximation of a separate bottom-up pass, it IS that pass, down to the
 * integer rounding. See the long note at the reflection itself. `reverse` is
 * FLEX_WRAP_FWD (0) for every wrap container, so that path is inert.
 *
 * THE `flex: N` BASIS-0 SEED ON THE WRAP PATH (#245 engflexwrap2). CSS breaks
 * lines on each item's HYPOTHETICAL MAIN SIZE, which is the flex base size
 * clamped UP by the AUTOMATIC MINIMUM SIZE. For a `flex: N` item the base size
 * is 0, so its hypothetical main size is exactly its automatic minimum size.
 * Until engflexmin this engine had no such minimum, which is why the seed was
 * deliberately kept off this path (a zero basis with no clamp packs every
 * `flex: 1` item onto one line). It now has one, from the SAME
 * flex_item_min_content() primitive the shrink pass and the single-line seed
 * use, so this pass:
 *   1. measures each seeded item's floor F[k] HERE, BEFORE any line breaking
 *      and BEFORE any collapse, while every box still carries its content
 *      width. That ORDER IS THE WHOLE POINT: flex_item_min_content() derives
 *      the item's chrome from its border-box width AND clamps its answer to
 *      it, so asking after flex_basis0_pass has set w = 0 returns 0, which is
 *      inert and INDISTINGUISHABLE from "no item hit its minimum".
 *   2. breaks lines on max(basis, floor), i.e. on F[k] for a seeded item and
 *      on the measured post-basis span for every other item.
 *   3. runs flex_basis0_pass per LINE, so each line's seeded items collapse
 *      and flex_grow_pass hands that line's whole main size back by grow
 *      factor, with that line's floors as the freeze-at-min array.
 * ARMED ONLY when at least one item carries the basis-0 sentinel AND every
 * such item has a principal box (the same ALL-OR-NOTHING rule the single-line
 * seed uses, applied container-wide so line breaking and seeding can never
 * disagree about which items are seeded). Otherwise F[] stays all-zero, the
 * seed is not called, and this pass is byte-identical to engflexwrap.
 *
 * ALIGN-CONTENT (#245 engaligncontent, closed out by #245 engstretch). Once
 * the lines are stacked, the leftover CROSS-axis space is distributed between
 * and around them per align-content: flex-end, center, space-between,
 * space-around, space-evenly and now STRETCH, with the same per-value
 * arithmetic flex_align uses on the main axis and the LINE count in place of
 * the item count. For the first five it is a RIGID TRANSLATION of each line's
 * already-emitted items; STRETCH also GROWS each line's cross size by an equal
 * share and re-places that line's items inside the taller line, which is why
 * it is the only value here that is not a pure translate. It requires a
 * DEFINITE cross size (cross_h, the container's declared content height in px,
 * -1 for auto), because an auto-height flex container grows to its content and
 * by definition has no leftover space.
 *
 * NEGATIVE free space now follows the CSS fallbacks (#245 engstretch) instead
 * of leaving everything packed at the start: space-between and stretch behave
 * as flex-start, space-around and space-evenly as center, and flex-end and
 * center overflow at the TOP.
 *
 * ALIGN-ITEMS:STRETCH, PER LINE (#245 engstretchitems). Each line now GROWS
 * the items on it to its own cross size, which closes the limit the paragraph
 * above used to record: growing the LINE and growing the ITEM are two separate
 * steps and both now exist. They compose without double-counting because
 * flex_stretch_items is absolute and grow-only, so the align-content:stretch
 * path simply re-runs it against the grown line. A ONE-line container with
 * top-aligned items now fills that line with its items instead of leaving
 * them at their measured heights.
 *
 * THE BLAST RADIUS IS REAL AND DELIBERATE: stretch is align-items' INITIAL
 * value, so unlike every earlier step in this series there is NO keyword a
 * page has to write to arm it. What bounds it instead is that this whole
 * function is only ever reached from a flex-wrap:wrap container, or (since
 * #245 engwraprev) a wrap-reverse one, that an item
 * needs an AUTO cross size to stretch at all, and that a line whose items are
 * already the same height grows nothing. A nowrap flex container is NOT
 * stretched (the single-line path is untouched); see the DEFERRED list in
 * docs/BROWSER_ENGINE_FLEXWRAP_PLAN.md, which names the gate that will have to
 * carry the no-regression argument when that deferral is lifted.
 *
 * DEFERRED (docs/BROWSER_ENGINE_FLEXWRAP_PLAN.md): cross-axis stretch on the
 * SINGLE-LINE (nowrap) path, and per-item POSITIONAL align-self (an item that
 * opts out of stretching keeps the container's align-items placement).
 *
 * ROW-GAP DISTINCT FROM COLUMN-GAP (#245 enggap). `gap` is the MAIN-axis
 * (column) gap spent between items on a line; `row_gap` is the CROSS-axis gap
 * spent between the lines. They were one number until now, so `gap: 20px 8px`
 * put 8px between the lines as well as between the cells, and a bare
 * `column-gap` opened a row gap nothing had asked for. libcss has no row-gap
 * property at all, so a row gap reaches computed style on a CARRIER; see the
 * carrier note in cssvar.c.
 *
 * THE ALIGN-CONTENT INTERACTION IS THE PART THAT IS EASY TO GET WRONG, and it
 * comes out right BY CONSTRUCTION rather than by a second sum: the leftover
 * cross space below is measured from the STACKED POSITIONS
 * (last_top + last_h - ftop), and line_top has already spent row_gap after
 * every line but the last, so `extent` INCLUDES the gaps and `freev` is what
 * is left AFTER them. Summing the line heights independently and subtracting
 * would have handed align-content space the gaps had already consumed, and
 * every value would have pushed the last line past the bottom of the box.
 *
 * bnd[0..cnt] are the item boundaries in items[]; grow/shrink/basis are the
 * per-item factors the caller already gathered. ftop is the y all items
 * currently sit on (the packed line). On return, st->cursor_y is the last
 * line's top and st->line_height its height, so the caller's block close lands
 * the pen at the bottom of the wrapped container.
 */
static void flex_wrap_lines(lstate *st, int ftop, int content_x,
		int content_w, uint8_t justify, uint8_t align,
		uint8_t acontent, int cross_h, int reverse,
		int gap, int row_gap,
		const int *bnd, const css_fixed *grow, const css_fixed *shrink,
		const int *basis, const flex_cross_t *xc, int cnt)
{
	int k, i, l, nlines;
	int W[FLEX_MAX_ITEMS];
	int F[FLEX_MAX_ITEMS];
	int lstart[FLEX_MAX_ITEMS + 1];
	int allcy[FLEX_MAX_ITEMS];
	/* #245 engstretch: each line's final top and cross size. align-content
	 * needed only the LAST line's pair while every value was a rigid
	 * translation; stretch changes each line's SIZE, so it needs all of
	 * them. nlines <= cnt <= FLEX_MAX_ITEMS. */
	int ltop[FLEX_MAX_ITEMS];
	int lhgt[FLEX_MAX_ITEMS];
	int line_top, last_top = ftop, last_h = 0;
	int do_seed, nseed = 0;

	if (st->measuring || cnt < 1)
		return;

	/* flex-basis first: resize items to their hypothetical main size on the
	 * still-packed single line, so the wrap decision below sees basis sizes. */
	for (k = 0; k < cnt; k++)
		allcy[k] = ftop;
	flex_basis_pass(st, ftop, bnd, allcy, basis, cnt);

	/*
	 * STEP 1 (#245 engflexwrap2): the AUTOMATIC MINIMUM SIZE of every
	 * basis-0 seeded item, measured NOW, on the still-packed line, before
	 * any break and before any collapse. See the ORDER note in the header:
	 * taken after a collapse this returns 0 and does nothing, silently.
	 *
	 * ALL OR NOTHING, container-wide: a seeded item with no principal box
	 * cannot be collapsed, so the single-line seed abandons the whole line.
	 * Detecting it here disarms line breaking too, which keeps the break
	 * decision and the per-line seed in agreement about which items are
	 * seeded. F[] then stays all-zero and this pass is byte-identical to
	 * the pre-engflexwrap2 code.
	 */
	do_seed = 1;
	for (k = 0; k < cnt; k++) {
		int bx = -1;
		F[k] = 0;
		if (basis[k] != 0 || grow[k] <= 0 || bnd[k + 1] <= bnd[k])
			continue;
		for (i = bnd[k]; i < bnd[k + 1]; i++) {
			layout_item *it = &st->out->items[i];
			if (it->kind == 1 || it->kind == 3) { bx = i; break; }
		}
		if (bx < 0) { do_seed = 0; break; }
		F[k] = flex_item_min_content(st, bnd[k], bnd[k + 1], bx,
				st->out->items[bx].w);
		nseed++;
	}
	if (!do_seed || nseed == 0) {
		do_seed = 0;
		for (k = 0; k < cnt; k++)
			F[k] = 0;
	}

	/*
	 * STEP 2: each item's HYPOTHETICAL MAIN SIZE for line breaking, which
	 * is max(flex base size, automatic minimum size). A seeded item's base
	 * size is 0, so that is F[k]; every other item keeps its measured
	 * post-basis span and F[k] is 0, so an unseeded container breaks lines
	 * exactly where it did before.
	 */
	for (k = 0; k < cnt; k++) {
		int minx, maxx;
		flex_item_span(st, bnd[k], bnd[k + 1], &minx, &maxx);
		W[k] = (maxx > minx) ? (maxx - minx) : 0;
		if (do_seed && F[k] > 0)
			W[k] = F[k];
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
		/* #245 engflexwrap2: this line's slice of the automatic
		 * minimum size. flex_basis0_pass fills it on its apply path
		 * (from the same primitive, re-measured on the repacked line,
		 * where a pure translation leaves both the box width and the
		 * content extent unchanged, so it agrees with F[] above).
		 * Zero-filled first, so an early return in the seed leaves
		 * flex_grow_pass with no floor at all and the exact
		 * pre-engflexmin single-shot arithmetic. */
		int ml[FLEX_MAX_ITEMS];
		if (m <= 0) {
			/* unreachable (lstart is strictly increasing), but leave
			 * the arrays defined rather than let a future change read
			 * an uninitialised top. */
			ltop[l] = line_top;
			lhgt[l] = 0;
			continue;
		}

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
			ml[j] = 0;
		}
		bl[m] = bnd[ke];
		/*
		 * STEP 3 (#245 engflexwrap2): the basis-0 seed, PER LINE, with
		 * this line's slice of the recorded basis sentinels. Inert
		 * unless do_seed armed it above, so a wrap container that never
		 * writes the `flex` shorthand is byte-identical. The seed writes
		 * this line's floors into ml[] as it collapses each box, which
		 * is what turns flex_grow_pass's freeze-at-min loop on.
		 */
		if (do_seed)
			used_x += flex_basis0_pass(st, line_top, content_x,
					content_w, used_x, bl, cyl,
					basis + ks, gl, m, ml);
		grew = flex_grow_pass(st, line_top, content_x, content_w,
				used_x, bl, cyl, gl, ml, m);
		used_x += grew;
		/*
		 * #245 engflexmin, held PER LINE: grow and shrink stay mutually
		 * exclusive. Without floors grow distributes EXACTLY the free
		 * space, so the line ends flush, shrink saw freev == 0 and
		 * returned 0, and skipping it there is inert. WITH a floor grow
		 * may legitimately overflow, and letting shrink claw that back
		 * would undo the very minimum CSS just imposed.
		 */
		shrunk = (grew > 0) ? 0 :
			flex_shrink_pass(st, line_top, content_x, content_w,
				used_x, bl, cyl, sl, m);
		used_x += shrunk;
		/*
		 * THE LINE'S CROSS SIZE = the tallest item's cross size. This
		 * loop used to sit BELOW flex_align; #245 engstretchitems moved
		 * it above, and the number is the same on either side of that
		 * call. Every item's top is at line_top here, and
		 * flex_cross_place only ever moves an item DOWN into the band
		 * [line_top, line_top + lh], with the tallest item (whose
		 * item_cross IS lh) getting dy == 0, so the maximum bottom,
		 * which is the only thing this loop reads, cannot change.
		 *
		 * It has to be known BEFORE flex_align now, because a stretched
		 * item's cross size IS the line's: stretch first, and an item
		 * that fills the line is then centred or end-placed with
		 * dy == 0, instead of being moved down and then grown from
		 * wherever the move left it.
		 */
		lh = flex_line_cross(st, bnd[ks], bnd[ke], line_top);
		/*
		 * #245 engstretchitems: a stretching item's HYPOTHETICAL cross
		 * size is its content height clamped by its own min/max-height,
		 * and the line is as tall as the tallest of those, so a
		 * min-height ABOVE the natural stack grows the LINE and not
		 * just the one item. Only a STRETCHING item is consulted: this
		 * engine does not otherwise implement min-height, and making it
		 * bind for items that do not stretch would be a far wider
		 * change than this one.
		 */
		for (k = ks; k < ke; k++)
			if (xc[k].stretch && xc[k].min_h > lh)
				lh = xc[k].min_h;
		/* #245 engstretchitems: fill the line on the cross axis. */
		flex_stretch_items(st, line_top, lh, bnd, xc, ks, ke);
		/* #245 engflexclose: xc is a CONTAINER-wide array indexed by the
		 * item's container index, while bl/cyl are this line's repacked
		 * slices, so the align-self slice offset is ks. Passing the whole
		 * array here would align line 2's items by line 1's keywords. */
		flex_align(st, bnd[ks], bnd[ke], line_top, content_x, content_w,
				used_x, justify, align, bl, cyl, xc + ks, m);
		last_top = line_top;
		last_h = lh;
		ltop[l] = line_top;   /* #245 engstretch */
		lhgt[l] = lh;
		/* #245 enggap: the CROSS-axis gap, which is `gap` only when the
		 * author wrote a single-value `gap`. */
		line_top += lh + row_gap;
	}

	/*
	 * ALIGN-CONTENT (#245 engaligncontent, closed out by #245 engstretch).
	 * The lines are stacked at their own heights from ftop; now hand them
	 * whatever cross-axis space the container has left over.
	 *
	 * GATED TWO WAYS:
	 *   - acontent != FLEX_AC_START. An explicit flex-start, an `inherit`
	 *     and every keyword this engine does not model map to the inert
	 *     constant and never reach here. STRETCH, the INITIAL value, DOES
	 *     now reach here, which is exactly what #245 engstretch changed:
	 *     align-content's initial value stopped being inert.
	 *   - cross_h >= 0. The container needs a DEFINITE cross size (its
	 *     declared, non-percentage content height in px; -1 for auto). An
	 *     auto-height flex container grows to exactly its content, so it has
	 *     no leftover cross space at all; that is CSS, not a shortcut. This
	 *     gate is also what keeps arming the initial value from moving the
	 *     great majority of pages: almost no flex container declares a
	 *     height, and one that does not cannot enter this block whatever
	 *     align-content says.
	 *
	 * ftop IS the content-box top (the block open advanced the pen by the
	 * top border and padding before the child walk), so the stacked extent
	 * measured against it is directly comparable to the declared content
	 * height.
	 *
	 * #245 enggap: line_top spent row_gap after every line but the last, so
	 * the stacked extent already has the row gaps in it and freev is the
	 * space left OVER them. Summing the line heights independently and
	 * subtracting would hand align-content space the gaps already ate.
	 *
	 * THE OVERFLOW FALLBACKS (#245 engstretch). When freev is NEGATIVE the
	 * lines do not fit, and CSS does not just run the same formula with a
	 * negative number: space-between behaves as flex-start, space-around and
	 * space-evenly behave as center, and flex-end and center let the lines
	 * overflow at the START edge (the top) rather than the end. stretch has
	 * nothing to distribute, so it is flex-start too. It is done as a keyword
	 * REMAP, once, up front, so there is still exactly ONE arithmetic formula
	 * per value below rather than a positive and a negative variant of each.
	 * Before this, every value stayed packed at the start on overflow, which
	 * was right for space-between by accident and wrong for the other four.
	 *
	 * STRETCH is the one value that is NOT a rigid translation of already-
	 * emitted items. Each line's CROSS SIZE grows by an equal share of freev,
	 * so line l's TOP moves down by the shares of the l lines above it,
	 * (freev * l) / nlines. That is the same running-total shape the other
	 * values use, and taking each line's own share as the difference between
	 * consecutive boundaries distributes freev EXACTLY, with the rounding
	 * remainder spread rather than dumped on the last line. The line then has
	 * a new cross size, so its items are re-placed inside it by
	 * flex_cross_place(), the SAME primitive flex_align uses, against the
	 * grown height; that call is absolute rather than incremental, so running
	 * it after flex_align already placed the line at its natural height lands
	 * the items exactly where one call at the grown height would have.
	 *
	 * THE HONEST LIMIT ON STRETCH. Growing a line does not yet grow the ITEMS
	 * in it, because align-items:stretch cross-SIZE growth is still deferred.
	 * So for align-items center / flex-end the items visibly move down inside
	 * the taller line, and for flex-start (and for align-items:stretch, the
	 * initial value) flex_cross_place returns immediately and the items keep
	 * their top placement. On a MULTI-line container the re-stacking is still
	 * plainly visible, because every line after the first moves down by its
	 * predecessors' shares. On a container with exactly ONE line and
	 * top-aligned items, stretch correctly grows the single line to the whole
	 * box and correctly moves nothing. That case is in the fixture as a
	 * control so the limit is on the record rather than mistaken for a bug.
	 */
	if (acontent != FLEX_AC_START && cross_h >= 0 && nlines > 0) {
		int extent = (last_top + last_h) - ftop;
		int freev = cross_h - extent;
		uint8_t ac = acontent;

		if (freev < 0) {
			if (ac == FLEX_AC_BETWEEN || ac == FLEX_AC_STRETCH)
				ac = FLEX_AC_START;
			else if (ac == FLEX_AC_AROUND || ac == FLEX_AC_EVENLY)
				ac = FLEX_AC_CENTER;
		}
		if (freev != 0 && ac != FLEX_AC_START) {
			for (l = 0; l < nlines; l++) {
				int dy, lcross = lhgt[l];
				switch (ac) {
				case FLEX_AC_END:
					dy = freev; break;
				case FLEX_AC_CENTER:
					dy = freev / 2; break;
				case FLEX_AC_BETWEEN:
					dy = (nlines > 1)
						? (freev * l) / (nlines - 1)
						: 0;
					break;
				case FLEX_AC_AROUND:
					dy = (freev * (2 * l + 1)) / (2 * nlines);
					break;
				case FLEX_AC_EVENLY:
					dy = (freev * (l + 1)) / (nlines + 1);
					break;
				case FLEX_AC_STRETCH:
					/* this line's top carries the shares of
					 * every line above it; its own share is
					 * the step to the next boundary, so the
					 * shares sum to exactly freev. */
					dy = (freev * l) / nlines;
					lcross += (freev * (l + 1)) / nlines - dy;
					break;
				default: dy = 0; break;
				}
				if (dy)
					for (i = bnd[lstart[l]];
							i < bnd[lstart[l + 1]]; i++)
						st->out->items[i].y += dy;
				/* the grown line: re-place its items against the
				 * NEW cross size, per align-items, and re-grow
				 * the ones that stretch so they fill it. */
				if (lcross != lhgt[l]) {
					flex_cross_place(st, ltop[l] + dy, lcross,
						align, xc + lstart[l],
						bnd + lstart[l],
						lstart[l + 1] - lstart[l]);
					/*
					 * #245 engstretchitems. The two steps
					 * COMPOSE rather than double-count:
					 * flex_stretch_items is absolute and
					 * grow-only, so re-running it here with
					 * the GROWN cross size sets exactly the
					 * height one call at that size would
					 * have set, whatever the per-line call
					 * already did. dy has already moved the
					 * item boxes, and the target is measured
					 * from the item's own (moved) top to the
					 * moved line's bottom, so the shift
					 * cancels and no gap is ever eaten.
					 */
					flex_stretch_items(st, ltop[l] + dy,
						lcross, bnd, xc,
						lstart[l], lstart[l + 1]);
				}
				/*
				 * #245 engwraprev: keep EVERY line's final top
				 * and cross size, not just the last pair. The
				 * reflection below needs line 0's, because
				 * line 0 is the one that ends up at the bottom.
				 * Purely additive: ltop[]/lhgt[] were dead
				 * after this loop before, and the last-line
				 * pair below is left exactly as it was.
				 */
				ltop[l] += dy;
				lhgt[l] = lcross;
				if (l == nlines - 1) {
					last_top += dy;
					last_h = lcross;
				}
			}
		}
	}

	/*
	 * FLEX-WRAP: WRAP-REVERSE (#245 engwraprev), as a CROSS-AXIS REFLECTION
	 * of the finished `wrap` layout.
	 *
	 * Per CSS Flexbox 1 section 5.2, wrap-reverse wraps into EXACTLY the
	 * same lines as wrap; the only difference is that cross-start and
	 * cross-end are SWAPPED. So the lines stack bottom-to-top, and
	 * flex-start / flex-end mean the opposite ends for both align-items and
	 * align-content. Nothing on the MAIN axis changes: item order, line
	 * membership, flex-basis, grow, shrink and justify-content are all
	 * untouched, which is why reflecting the finished result is not an
	 * approximation of a second code path but IS the second code path.
	 *
	 * THE EQUIVALENCE IS EXACT, INCLUDING THE INTEGER ROUNDING, and that is
	 * worth spelling out because the obvious worry is that truncation would
	 * make "run the formulas then reflect" differ from "run the formulas in
	 * the reflected frame". It does not, because reflecting a placement
	 * measured from the start edge gives exactly the placement the same
	 * expression measures from the end edge:
	 *
	 *   align-items:center. Forward puts the item at (line - item) / 2 from
	 *   the line top. Reflected, its offset from the line top becomes
	 *   line - item - (line - item) / 2, which is what a bottom-up pass
	 *   computing (line - item) / 2 from the BOTTOM leaves. For line 49 and
	 *   item 18 both give offsets 16..34, not 15..33.
	 *   align-content:center. Same argument with freev in place of
	 *   (line - item).
	 *   align-content:stretch. Line l's share is
	 *   freev*(l+1)/nlines - freev*l/nlines. Reflection keeps each line's
	 *   own share and puts line 0 at cross-start, which is where a bottom-up
	 *   pass would have started handing shares out.
	 *   space-between / space-around / space-evenly. The first two are
	 *   symmetric distributions; space-between anchors line 0 flush with
	 *   cross-start and the last line flush with cross-end, which after
	 *   reflection is bottom and top respectively, as CSS requires.
	 *
	 * So there is ONE stacking path in this engine, not two, and
	 * wrap-reverse inherits align-content, align-items:stretch, row-gap and
	 * the overflow fallbacks for free rather than reimplementing any of them.
	 *
	 * REFLECTING THE POSITIONS IS NOT THE SAME AS REFLECTING EVERY EMITTED
	 * ITEM, and that distinction is the one real trap here. A flex item is a
	 * RANGE of layout_items, bnd[k]..bnd[k+1], holding its box, its text
	 * runs and any nested boxes. Reflecting each layout_item about the
	 * container midline individually would turn the item's first line of
	 * text into its last: correct for the item, catastrophic inside it. So
	 * the reflection is computed on each ITEM's bounding box and applied as
	 * a RIGID TRANSLATION of that item's whole range, which is the same
	 * shape every other pass in this file uses.
	 *
	 * WHAT IS REFLECTED ABOUT. The container's cross size: cross_h when the
	 * height is definite, otherwise the stacked extent, which IS the
	 * auto-height container's content height. In the definite case this is
	 * also what makes align-content:flex-start come out right for free: the
	 * lines are left packed at ftop by the (inert) FLEX_AC_START path, and
	 * the reflection then lands them packed at the BOTTOM, which is
	 * cross-start under wrap-reverse. Overflow reflects too: a stack taller
	 * than cross_h overflows at the TOP, which is cross-end, as CSS says.
	 *
	 * THE PEN. Line 0 is the first line, so it sits at cross-start, which is
	 * now the bottom, and it is the line the block close must land below.
	 * The total extent is preserved by a reflection, so an auto-height
	 * wrap-reverse container is exactly as tall as the same wrap container.
	 */
	if (reverse && nlines > 0) {
		int H = (cross_h >= 0) ? cross_h : (last_top + last_h) - ftop;
		/* y + y' == axis for any reflected edge. */
		int axis = 2 * ftop + H;

		for (k = 0; k < cnt; k++) {
			int top = 0x3fffffff, bot = 0, dy;
			for (i = bnd[k]; i < bnd[k + 1]; i++) {
				layout_item *it = &st->out->items[i];
				int ih = (it->kind == 0) ? it->size : it->h;
				if (it->y < top) top = it->y;
				if (it->y + ih > bot) bot = it->y + ih;
			}
			if (bot <= top)
				continue;
			dy = axis - bot - top;
			if (dy)
				for (i = bnd[k]; i < bnd[k + 1]; i++)
					st->out->items[i].y += dy;
		}
		last_top = axis - (ltop[0] + lhgt[0]);
		last_h = lhgt[0];
	}

	/* leave the pen at the LAST line's top with its height, so the caller's
	 * block-close line_break() advances to the bottom of the wrapped set and
	 * the container grows to contain every line. Under wrap-reverse the
	 * bottom-most line is line 0, and the block above has already put its
	 * reflected top and height here. */
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

/*
 * vertical-align (#245 engvalign): the per-item shift, in pixels, positive
 * DOWN. `h` is the item's own inline box height, `size` its font size and `H`
 * the final height of the line box it sits in.
 *
 * THE MODEL THIS ENGINE ACTUALLY HAS. Every inline run on a line is emitted at
 * `y = cursor_y`, the TOP of the line box; there is no real baseline table and
 * no inline formatting context. So `baseline` (the initial value) is where
 * everything already is, and every other keyword is expressed as a shift away
 * from that. Two consequences, stated rather than hidden:
 *
 *   - `top` and `text-top` both resolve to 0. In a model that top-aligns by
 *     default they are already satisfied, and they are supported in the sense
 *     that authoring them is correct and produces the right pixels, not in the
 *     sense that they execute any code.
 *   - `middle` centres the element's box in the line box rather than aligning
 *     its midpoint with the parent's baseline plus half an x-height, which is
 *     what CSS says. The two agree on the case that matters (a short inline in
 *     a taller line) and the x-height term is not available without font
 *     metrics this engine does not read.
 *
 * `sub` and `super` are fractions of the font size because that is the only
 * metric available: 1/5 em down and 1/3 em up are the conventional fallbacks a
 * renderer uses when the font supplies no subscript/superscript offsets.
 */
static int valign_dy(int mode, int px, int h, int size, int H)
{
	int slack = H - h;
	if (slack < 0) slack = 0;
	switch (mode) {
	case LAYOUT_VA_SUB:         return (size + 4) / 5;
	case LAYOUT_VA_SUPER:       return -((size + 2) / 3);
	case LAYOUT_VA_TOP:
	case LAYOUT_VA_TEXT_TOP:    return 0;
	case LAYOUT_VA_MIDDLE:      return slack / 2;
	case LAYOUT_VA_BOTTOM:
	case LAYOUT_VA_TEXT_BOTTOM: return slack;
	case LAYOUT_VA_LENGTH:      return -px;   /* a positive length RAISES */
	default:                    return 0;
	}
}

/*
 * Resolve vertical-align for the line that is about to close (#245 engvalign).
 *
 * WHY THIS IS A LINE-CLOSE PASS AND NOT AN EMIT-TIME ADJUSTMENT. Four of the
 * keywords are defined against the LINE BOX, whose height is only final once
 * every run on the line has been emitted: a 12px span asking for `bottom` has
 * to know that a 32px span later on the same line made the box 38px tall. The
 * engine already has exactly this shape for the horizontal axis in
 * align_line(), which is called from the same place for the same reason, so
 * this follows that precedent rather than inventing a second one.
 *
 * LINE HEIGHT: THIS GROWS THE LINE BOX TO CONTAIN SHIFTED CONTENT. That is the
 * correct half of the choice and it is the reason the pass is not three lines
 * long. A `super` on an ordinary line raises the run above the line box top;
 * the box has to grow UPWARD, and because the box top is pinned at cursor_y in
 * this coordinate system, growing upward means pushing every item on the line
 * DOWN by the deficit and adding the same amount to line_height. Downward
 * overflow (a `sub`, or a negative length) just extends line_height. The
 * alternative, leaving line_height alone, would have let a superscript paint
 * over the descenders of the line above, and a large `vertical-align: -2em`
 * paint over the line below.
 *
 * WHAT IT DOES NOT DO: it never reflows. Nothing changes width, nothing
 * re-wraps, no item moves horizontally, and the shift is applied after the
 * wrap decisions for the line have all been taken. That keeps the #589
 * measured == drawn invariant intact by construction, because vertical-align
 * has no business in a measurement.
 *
 * AE=0: the whole function is behind st->line_has_valign, which is only ever
 * set by an item carrying a non-baseline mode. A page that authors no
 * vertical-align never reaches the first loop.
 */
static void valign_line(lstate *st)
{
	int i, n, H, rise = 0, drop, lift;

	if (!st->line_has_valign || st->measuring)
		goto done;
	n = st->out->n_items;
	if (st->line_first >= n)
		goto done;
	H = st->line_height;
	drop = H;

	/*
	 * Resolve each shifted item. Only inline content moves: a block
	 * background or border (kind 1) keeps the geometry the flow gave it,
	 * which is the same rule align_line() applies on the x axis.
	 *
	 * The mode is reset to BASELINE as it is consumed, so an item can never be
	 * shifted twice. That matters because a handful of paths close a line by
	 * zeroing line_height directly instead of calling line_break(), which can
	 * leave line_first pointing into an already-processed span; with the mode
	 * cleared, the worst such a path can now cost is a re-scan that moves
	 * nothing.
	 */
	for (i = st->line_first; i < n; i++) {
		layout_item *it = &st->out->items[i];
		int dy, bot;
		if (it->valign == LAYOUT_VA_BASELINE)
			continue;
		if (it->kind != 0 && it->kind != 3)
			continue;
		dy = valign_dy(it->valign, it->valign_px, it->valign_h,
				it->size, H);
		it->y += dy;
		it->valign = LAYOUT_VA_BASELINE;
		if (dy < rise)
			rise = dy;
		bot = dy + it->valign_h;
		if (bot > drop)
			drop = bot;
	}

	lift = -rise;
	if (lift > 0) {
		for (i = st->line_first; i < n; i++) {
			layout_item *it = &st->out->items[i];
			if (it->kind == 0 || it->kind == 3)
				it->y += lift;
		}
		drop += lift;
	}
	if (drop > st->line_height)
		st->line_height = drop;
done:
	st->line_has_valign = false;
}

static void line_break(lstate *st)
{
	valign_line(st);   /* #245 engvalign: before the pen advances past this line */
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
	/*
	 * CSS outline (#245 engoutline). NOT INHERITED and PAINT-ONLY: read in
	 * read_style(), copied onto the emitted box by fill_box_style(), and read
	 * nowhere else in this file. Deliberately absent from has_any_border(),
	 * from every outer_w/outer_h sum and from the inline line-height growth,
	 * because an outline occupies no layout space.
	 * ol_style is a LAYOUT_OL_* value; 0 (LAYOUT_OL_NONE) is inert.
	 */
	int ol_w;
	int ol_style;
	uint32_t ol_col;
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
	/*
	 * letter-spacing / word-spacing in px (#245 engletsp). Both INHERITED,
	 * both inert at their initial `normal`, which resolves to 0. Both may be
	 * negative. letter_spacing is added after every glyph of a run (and is
	 * carried on the emitted layout_item so the painter applies the same
	 * amount); word_spacing is added to the inter-word gap only.
	 */
	int letter_spacing;
	int word_spacing;
	/*
	 * vertical-align (#245 engvalign), as a LAYOUT_VA_* mode plus the resolved
	 * raise in px for LAYOUT_VA_LENGTH. NOT INHERITED: unlike white-space,
	 * visibility and letter-spacing, which this reader deliberately propagates
	 * from the parent estyle, CSS gives vertical-align an initial value of
	 * `baseline` and no inheritance, so a child of a superscripted span is NOT
	 * itself superscripted. Defaulting it to the parent's value would raise a
	 * whole subtree.
	 */
	int valign;
	int valign_px;
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

	/*
	 * letter-spacing and word-spacing (#245 engletsp). Both INHERITED, and
	 * read with the same defend-against-a-lying-getter pattern white-space
	 * and visibility use (engfix4): start from the parent's already-resolved
	 * value and only override when the getter reports an explicit length
	 * (CSS_*_SPACING_SET). A NORMAL / INHERIT reading is treated as "no
	 * information" rather than as a reset to zero, so a spaced heading keeps
	 * its tracking across the inline children libcss may not compose down.
	 *
	 * CONSEQUENCE, stated rather than hidden: a descendant that explicitly
	 * writes `letter-spacing: normal` to CANCEL an ancestor's tracking is not
	 * honoured, because the getter reports it identically to the unset case.
	 * That is the same documented trade the visibility reader makes.
	 *
	 * The percentage basis is -1 on purpose: neither property accepts a
	 * percentage, so a stray one resolves to 0 instead of to a fraction of
	 * the containing block. Resolution is whole pixels, so tracking below
	 * about 0.06em at a 16px body size rounds away entirely.
	 *
	 * Inert at the initial value: a tree that authors neither property
	 * resolves both to 0 everywhere, every `if (ls)` / `if (ws)` gate below
	 * is false, and the pre-engletsp code path runs unchanged.
	 */
	e->letter_spacing = parent ? parent->letter_spacing : 0;
	{
		css_fixed lv; css_unit lu;
		if (css_computed_letter_spacing(s, &lv, &lu) ==
				CSS_LETTER_SPACING_SET)
			e->letter_spacing = fixed_px(lv, lu, e->font_size, -1);
	}
	e->word_spacing = parent ? parent->word_spacing : 0;
	{
		css_fixed wv; css_unit wu;
		if (css_computed_word_spacing(s, &wv, &wu) ==
				CSS_WORD_SPACING_SET)
			e->word_spacing = fixed_px(wv, wu, e->font_size, -1);
	}

	/*
	 * vertical-align (#245 engvalign). NOT inherited and NOT defaulted from
	 * the parent: see the estyle comment. The switch maps the libcss keyword
	 * enum onto this engine's LAYOUT_VA_* modes, and anything it does not name
	 * (BASELINE, INHERIT, and any value a future libcss adds) falls to the
	 * default and stays at baseline, which is inert.
	 *
	 * The percentage basis is e->line_height, which CSS requires for this one
	 * property, and which is why this block has to sit BELOW the line-height
	 * resolution earlier in this function rather than beside the other
	 * length reads. A length is resolved once, here, against the element's own
	 * font size, so `vertical-align: 0.4em` means 0.4 of the shifted element's
	 * em and not of its parent's.
	 *
	 * A LENGTH that rounds to zero pixels is left at BASELINE rather than
	 * recorded as a zero-pixel LENGTH shift, so it cannot arm the line-close
	 * pass for a line where nothing will actually move.
	 */
	e->valign = LAYOUT_VA_BASELINE;
	e->valign_px = 0;
	{
		css_fixed vv = 0; css_unit vu = CSS_UNIT_PX;
		switch (css_computed_vertical_align(s, &vv, &vu)) {
		case CSS_VERTICAL_ALIGN_SUB:
			e->valign = LAYOUT_VA_SUB; break;
		case CSS_VERTICAL_ALIGN_SUPER:
			e->valign = LAYOUT_VA_SUPER; break;
		case CSS_VERTICAL_ALIGN_TOP:
			e->valign = LAYOUT_VA_TOP; break;
		case CSS_VERTICAL_ALIGN_TEXT_TOP:
			e->valign = LAYOUT_VA_TEXT_TOP; break;
		case CSS_VERTICAL_ALIGN_MIDDLE:
			e->valign = LAYOUT_VA_MIDDLE; break;
		case CSS_VERTICAL_ALIGN_BOTTOM:
			e->valign = LAYOUT_VA_BOTTOM; break;
		case CSS_VERTICAL_ALIGN_TEXT_BOTTOM:
			e->valign = LAYOUT_VA_TEXT_BOTTOM; break;
		case CSS_VERTICAL_ALIGN_SET:
			e->valign_px = fixed_px(vv, vu, e->font_size,
					e->line_height);
			if (e->valign_px)
				e->valign = LAYOUT_VA_LENGTH;
			break;
		default:
			break;
		}
	}

	for (i = 0; i < 4; i++)
		e->bw[i] = read_border_side(s, i, e->font_size, e->eff_bg, &e->bcol[i]);

	/*
	 * OUTLINE (#245 engoutline). Read here, carried on the emitted box, and
	 * painted outside the border edge. It is deliberately NOT folded into
	 * e->bw[] and NOT added to any box size: CSS says an outline takes no
	 * layout space, so a page that adds one must not reflow by a pixel.
	 *
	 * NOT INHERITED. Unlike white-space, visibility and letter-spacing, which
	 * this reader seeds from `parent`, outline has an initial value of
	 * none/medium/invert and no inheritance, so these three are written
	 * unconditionally on every element.
	 *
	 * The libcss enums are mapped into LAYOUT_OL_* with an INERT default,
	 * never stored raw: CSS_OUTLINE_STYLE_INHERIT is 0x0 and
	 * CSS_OUTLINE_STYLE_NONE is 0x1, so a raw store would arm the paint path
	 * on every untouched element of every page.
	 *
	 * NOTE the collision documented at the colour read below: outline-COLOR is
	 * this engine's gradient carrier. outline-width and outline-style are not
	 * carriers for anything, so the style/width gate is unaffected.
	 */
	e->ol_w = 0;
	e->ol_style = LAYOUT_OL_NONE;
	e->ol_col = 0;
	switch (css_computed_outline_style(s)) {
	case CSS_OUTLINE_STYLE_SOLID:
	/* double/groove/ridge/inset/outset need a multi-pass or shaded edge this
	 * painter has no primitive for; solid is the honest approximation and is
	 * what every one of them degrades to most recognisably. */
	case CSS_OUTLINE_STYLE_DOUBLE:
	case CSS_OUTLINE_STYLE_GROOVE:
	case CSS_OUTLINE_STYLE_RIDGE:
	case CSS_OUTLINE_STYLE_INSET:
	case CSS_OUTLINE_STYLE_OUTSET:
		e->ol_style = LAYOUT_OL_SOLID;
		break;
	case CSS_OUTLINE_STYLE_DASHED:
		e->ol_style = LAYOUT_OL_DASHED;
		break;
	case CSS_OUTLINE_STYLE_DOTTED:
		e->ol_style = LAYOUT_OL_DOTTED;
		break;
	default:
		/* none, hidden, inherit and anything libcss grows later: inert. */
		e->ol_style = LAYOUT_OL_NONE;
		break;
	}
	if (e->ol_style != LAYOUT_OL_NONE) {
		/*
		 * MEASURED, not assumed: this libcss's css_computed_outline_width()
		 * (src/select/computed.c) ALWAYS returns CSS_BORDER_WIDTH_WIDTH. It
		 * resolves the `medium` keyword to 2px itself and reports it as a
		 * length; it never returns THIN, MEDIUM, THICK or INHERIT. The three
		 * keyword cases below are therefore unreachable in this build and are
		 * kept only so a future libcss that does report them is handled.
		 *
		 * THE HAZARD, and the reason these two are INITIALISED: the underlying
		 * get_outline_width() writes *length and *unit ONLY when the stored
		 * type is WIDTH, while the wrapper returns WIDTH regardless. So for
		 * `outline-width: thin` and `: thick` the caller is handed back
		 * whatever was already in its own variables. Left uninitialised, that
		 * is a garbage width painted on a real page. Seeded with the `medium`
		 * value, a thin or thick outline lands on 2px instead, which is the
		 * closest thing recoverable through this API: thin and thick are
		 * INDISTINGUISHABLE from each other here, because the one value that
		 * would tell them apart is the enum the wrapper throws away.
		 */
		css_fixed ov = INTTOFIX(2); css_unit ou = CSS_UNIT_PX;
		int ow = 0;
		switch (css_computed_outline_width(s, &ov, &ou)) {
		/* Unreachable in this libcss; see above. The CSS2.1 UA-default keyword
		 * widths, for a libcss that does report the keyword. */
		case CSS_OUTLINE_WIDTH_THIN:   ow = 1; break;
		case CSS_OUTLINE_WIDTH_MEDIUM: ow = 3; break;
		case CSS_OUTLINE_WIDTH_THICK:  ow = 5; break;
		case CSS_OUTLINE_WIDTH_WIDTH:
			/* No percentage basis: outline-width is a <length> only. */
			ow = fixed_px(ov, ou, e->font_size, -1);
			break;
		default: ow = 0; break;   /* inherit */
		}
		if (ow < 0) ow = 0;
		/* Same 8px ceiling read_border_side() applies, and for the same
		 * reason: ol_w is a uint8_t on the item and a runaway width would
		 * otherwise paint a slab over the page. */
		if (ow > 8) ow = 8;
		if (ow == 0) {
			e->ol_style = LAYOUT_OL_NONE;
		} else {
			css_color oc = 0;
			uint8_t ct = css_computed_outline_color(s, &oc);
			uint32_t rgb, a;
			int carrier = 0;
			/*
			 * OUTLINE-COLOR IS ALREADY TAKEN. #245 enggrad rewrites a CSS
			 * gradient background onto `outline-color: #C5<index>` in
			 * cssvar_preprocess(), because libcss rejects gradient FUNCTION
			 * tokens; layout reads that carrier back further down this file to
			 * emit the gradient paint item. So on any element with a gradient
			 * background the computed outline-color is the carrier, not an
			 * author colour, and painting it would draw a ring in an arbitrary
			 * #C5xxxx blue-grey.
			 *
			 * Detected with EXACTLY the same test as the readback site (marker
			 * byte 0xC5 in the top octet of the RGB, AND a live table index),
			 * so the two cannot drift into disagreeing about what a carrier
			 * is. A carrier is treated as "no author colour" and falls through
			 * to the currentColor path below.
			 */
			if (ct == CSS_OUTLINE_COLOR_COLOR) {
				unsigned int cv = (unsigned int) oc & 0x00FFFFFFu;
				if ((cv >> 16) == 0xC5u) {
					int gi = (int) (cv & 0xFFFFu);
					if (gi >= 0 && gi < cssvar_gradient_count())
						carrier = 1;
				}
			}
			if (ct == CSS_OUTLINE_COLOR_COLOR && !carrier) {
				a = (oc >> 24) & 0xffu;
				rgb = oc & 0xffffffu;
			} else {
				/*
				 * `invert` (the CSS initial value) and `currentColor` both
				 * land here. A true invert is a read-modify-write of the
				 * framebuffer and this painter has no such primitive, only an
				 * opaque rect fill, so both fall back to the element's
				 * computed text colour. For currentColor that is exact; for
				 * invert it is a visible, readable stand-in rather than a
				 * silent no-paint. Stated in the plan doc as a known limit.
				 */
				a = 255;
				rgb = e->color & 0xffffffu;
			}
			if (a == 0) {
				e->ol_style = LAYOUT_OL_NONE;   /* transparent: paint nothing */
			} else {
				e->ol_col = (a == 255) ? rgb
					: blend_rgb(rgb, e->eff_bg, a);
				e->ol_w = ow;
			}
		}
	}

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
 *
 * `ls` is the per-glyph spacing to apply to THIS run (#245 engletsp). It is
 * normally e->letter_spacing; the preserved-whitespace path passes
 * letter_spacing + word_spacing for a run made entirely of space characters,
 * because every glyph in such a run is a word separator. It is a parameter
 * rather than read from `e` so that decision stays at the call site where it
 * is legible.
 *
 * MEASURED WIDTH MUST EQUAL DRAWN WIDTH (#589). The spacing is added to the
 * advance here AND stamped on the emitted item, and the painter advances its
 * pen by the same amount per glyph. Widening only the advance would leave the
 * glyphs drawn tight at the left of a box laid out wide.
 */
static void emit_word(lstate *st, const estyle *e, char *word, int wl,
		int do_wrap, int ls)
{
	int ww;
	layout_item *it;
	if (e->text_transform)
		apply_text_transform(word, wl, e->text_transform);
	/* Spacing is applied AFTER the memoised measure, never folded into the
	 * key, so the lmeasure cache stays a pure function of (text, size, face,
	 * style) and a tracked run cannot poison it for an untracked one. */
	ww = lmeasure(st, word, e->font_size, e->face, e->fstyle);
	if (ww <= 0) ww = wl * (e->font_size / 2 + 1);
	if (ls) {
		/* CSS adds letter-spacing after EVERY character including the
		 * last. wl is the glyph count exactly: utf8_squash() already
		 * folded this word to single-byte Latin-1 and the rasteriser
		 * indexes it by byte. Negative tracking is legal, so clamp the
		 * ADVANCE at zero: the pen may stop moving, it may never move
		 * backwards and rewind over the previous word. */
		ww += ls * wl;
		if (ww < 0) ww = 0;
	}
	if (do_wrap && st->line_has_content && st->cursor_x + ww > st->line_right)
		line_break(st);
	/* visibility:hidden (#245 engfix6): reserve the advance, paint nothing. */
	if (!e->visibility) {
		it = emit_run(st, word, wl, e->font_size, e->color, e->bold,
				e->italic, e->underline, e->face, e->fstyle);
		if (it) {
			it->letter_spacing = ls;
			/*
			 * #245 engvalign: carry the run's vertical-align to the
			 * line-close pass, which is the first moment the line box
			 * height is known. The advance below is NOT touched: this
			 * property moves a run's origin and nothing else, so
			 * measured width still equals drawn width (#589).
			 * visibility:hidden takes the other branch and emits no
			 * item, so a hidden run cannot arm the pass either.
			 */
			if (e->valign != LAYOUT_VA_BASELINE) {
				it->valign = e->valign;
				it->valign_h = e->line_height;
				it->valign_px = e->valign_px;
				st->line_has_valign = true;
			}
		}
	}
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
	/*
	 * engletsp: the collapsed inter-word gap is ONE space character, so it
	 * takes word-spacing (which applies to word separators) AND
	 * letter-spacing (which CSS applies to every character, the space
	 * included). That is the choice this port makes, and it matches what
	 * a browser does for `letter-spacing: 3px` on a paragraph.
	 *
	 * This gap is pure cursor advance: no glyph is emitted for a collapsed
	 * space, so there is nothing for the painter to widen and the #589
	 * invariant is not in play here. Clamped at zero so negative tracking
	 * can close the gap but never run two words together backwards.
	 * Inert when both properties are 0: space_w is untouched.
	 */
	if (e->word_spacing || e->letter_spacing) {
		space_w += e->word_spacing + e->letter_spacing;
		if (space_w < 0) space_w = 0;
	}

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

		emit_word(st, e, word, wl, do_wrap, e->letter_spacing);
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
				/* engletsp: every glyph in this run IS a word
				 * separator, so it carries word-spacing as well
				 * as letter-spacing. Preserved spaces are real
				 * glyphs (unlike the collapsed gap above), so
				 * the painter widens them identically and #589
				 * holds. */
				emit_word(st, e, sp, n, 0,
						e->letter_spacing + e->word_spacing);
			/* spaces never wrap */
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
			emit_word(st, e, word, wl, do_wrap, e->letter_spacing);
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
 * #245 engoutline. An element with ONLY an outline (no background, no border)
 * still needs a kind-1 box emitted to hang the outline on, so this joins the
 * has_bg/has_any_border gate at each box-emission site. It is deliberately a
 * SEPARATE predicate from has_any_border(): every caller of that one also
 * feeds a size or a line height, and an outline must feed neither.
 */
static bool has_outline(const estyle *e)
{
	return e->ol_w > 0;
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
	it->ol_w = 0;   /* #245 engoutline: a clipped-away box paints no outline */
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
			ix1 = ix0 + item_run_w(st, it);   /* engletsp */
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
	/* #245 engoutline: paint-only, so this is the ONLY place the outline
	 * crosses from the cascade to an item. All three are 0 when the element
	 * authors no outline. */
	bx->ol_w = (uint8_t) e->ol_w;
	bx->ol_style = (uint8_t) e->ol_style;
	bx->ol_col = e->ol_col;
}

/* ---- recursive box walk ---- */
static void walk(lstate *st, dom_node *node, const css_computed_style *pstyle,
		const estyle *pe, int cb_x, int cb_w, int flex_row);
static void walk_node(lstate *st, dom_node *node, const css_computed_style *pstyle,
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

/*
 * engwalkstack (#245): the depth-counting shell around the real walk.
 *
 * WHY A WRAPPER AND NOT A COUNTER INSIDE walk_node. walk_node has eleven
 * `return` statements; a decrement at each is eleven chances to leak a level,
 * and a leaked level is a ceiling that creeps down over a long page until the
 * guard trips on a shallow tree. One entry and one exit cannot get this wrong.
 *
 * WHAT THE PAGE DOES WHEN THE CEILING IS HIT, and this is deliberate rather
 * than whatever fell out: the offending node and its entire subtree emit NO
 * layout items, so nothing below the limit paints. Everything shallower than
 * the limit is laid out and painted exactly as it would have been, because the
 * pen (cursor_x/cursor_y/line state) is untouched by a refused node, so the
 * page renders as itself with the over-deep branch missing. out->deep_truncated
 * latches so the app can SAY the page was truncated instead of quietly showing
 * a short one, which is the same contract out->overflowed already has.
 *
 * NOT RESTORED BY tbl_probe. tbl_probe saves and restores out->overflowed
 * because it throws its own items away, but a probe that hits the ceiling
 * proves the tree really is that deep and the real walk of the same cell will
 * hit it too, so latching is the honest signal and un-latching would hide it.
 */
static void walk(lstate *st, dom_node *node, const css_computed_style *pstyle,
		const estyle *pe, int cb_x, int cb_w, int flex_row)
{
	if (st->depth >= WALK_MAX_DEPTH) {
		st->out->deep_truncated = 1;
		return;
	}
	st->depth++;
	if (st->depth > st->depth_peak) st->depth_peak = st->depth;
	walk_node(st, node, pstyle, pe, cb_x, cb_w, flex_row);
	st->depth--;
}

static void walk_node(lstate *st, dom_node *node, const css_computed_style *pstyle,
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
				/* #245 engoutline. The control's chrome is hardcoded above
				 * rather than taken from the cascade, so fill_box_style() is
				 * never called here; the outline is stamped by hand so that
				 * `input { outline: 2px solid ... }` works on the element the
				 * property is authored on most often. Inert at ol_w == 0. */
				bx->ol_w = (uint8_t) e.ol_w;
				bx->ol_style = (uint8_t) e.ol_style;
				bx->ol_col = e.ol_col;
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
	/*
	 * #245 engrowrev: does it run that main axis BACKWARDS? Own flag, set
	 * explicitly from the computed keyword at this one place and never
	 * carried as a raw libcss enum, because CSS_FLEX_DIRECTION_INHERIT is
	 * 0x0 and the INITIAL value ROW is 0x1. FLEX_ROW_FWD (0) is today's
	 * behaviour, so every page that does not write `row-reverse` is
	 * byte-identical by construction.
	 */
	int child_flex_rowrev = FLEX_ROW_FWD;
	if (style && (e.display == CSS_DISPLAY_FLEX ||
			e.display == CSS_DISPLAY_INLINE_FLEX)) {
		uint8_t dir = css_computed_flex_direction(style);
		if (dir == CSS_FLEX_DIRECTION_ROW || dir == CSS_FLEX_DIRECTION_ROW_REVERSE ||
				dir == CSS_FLEX_DIRECTION_INHERIT)
			child_flex_row = 1;
		if (dir == CSS_FLEX_DIRECTION_ROW_REVERSE)
			child_flex_rowrev = FLEX_ROW_REV;
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
	/*
	 * #245 enggap: the CROSS-axis gap, a SEPARATE number from track_gap.
	 * libcss has no row-gap and no gap property, so cssvar.c carries the row
	 * component on `clip` (a dead, non-inherited, four-length property this
	 * engine never paints); read the carrier note there before changing
	 * either side. 0 is the inert value and is what every page that does not
	 * author a row gap produces, which is why this is byte-identical to the
	 * shared-gap code on such a page.
	 */
	int row_gap = 0;
	if (style) {
		css_fixed gv; css_unit gu;
		if (css_computed_column_gap(style, &gv, &gu) == CSS_COLUMN_GAP_SET) {
			track_gap = fixed_px(gv, gu, e.font_size, content_w);
			if (track_gap < 0) track_gap = 0;
			if (track_gap > 200) track_gap = 200;
		}
		{
			/*
			 * SEEDED IN FULL, including the units: css_computed_clip()
			 * writes the rect only for the RECT case, and reading an
			 * unwritten css_unit was the engoutline uninitialised-width
			 * bug. The initial value is CSS_CLIP_AUTO (0x1), not RECT,
			 * so an unstyled element never gets past the test.
			 *
			 * THE THIRD COMPONENT, NOT THE FIRST, AND THAT IS NOT A
			 * TASTE DECISION. libcss's GENERATED getter has an operator
			 * precedence bug in exactly two of its four unit fields
			 * (libcss/src/select/autogenerated_propget.h, get_clip):
			 *
			 *     rect->tunit = bits & 0x3e00000 >> 21;
			 *     rect->runit = bits & 0x1f0000 >> 16;
			 *     rect->bunit = (bits & 0xf800) >> 11;
			 *     rect->lunit = (bits & 0x7c0) >> 6;
			 *
			 * `>>` binds tighter than `&`, so the first two read
			 * `bits & 0x1f`, which is the TYPE and the four auto flags,
			 * never the unit. For a plain `rect(Npx,...)` that is the
			 * constant 2, and CSS_UNIT_EM is 2, so the top and right
			 * lengths of EVERY clip rect come back as em. MEASURED here
			 * before the carrier was moved: a 10px row gap laid out as
			 * 150px, which is 10 * the 15px font size, and 20px and
			 * above all hit the 200px clamp. The bottom and left fields
			 * are parenthesised correctly, so the bottom slot reports
			 * px as px.
			 *
			 * libcss is NOT in this repo (only our own port glue is), so
			 * a two-character fix there would live in an untracked tree
			 * and the tracked engine would silently depend on it. That
			 * is the divergence trap this project keeps paying for, so
			 * the carrier moves instead and the bug is recorded.
			 */
			css_computed_clip_rect cr = { 0, 0, 0, 0,
					CSS_UNIT_PX, CSS_UNIT_PX,
					CSS_UNIT_PX, CSS_UNIT_PX,
					false, false, false, false };
			if (css_computed_clip(style, &cr) == CSS_CLIP_RECT &&
					!cr.bottom_auto) {
				row_gap = fixed_px(cr.bottom, cr.bunit,
						e.font_size, content_w);
				if (row_gap < 0) row_gap = 0;
				if (row_gap > 200) row_gap = 200;
			}
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
		if ((e.has_bg || has_any_border(&e) || has_outline(&e)) &&
				!e.visibility) {
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
		/* #245 engoutline: has_outline() is deliberately ABSENT from this
		 * test and e.ol_w from this sum. An outline takes no layout space,
		 * so an outlined inline must not grow its line box; it is allowed to
		 * overhang the neighbouring line, which is what CSS specifies. */
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
		 * every track is the same width and nothing spans.
		 *
		 * The ROW gap is its own number since #245 enggap: `gap: A B` puts
		 * A between the rows and B between the columns. A one-value `gap`
		 * still sets both to the same number, and the narrow-grid collapse
		 * below still zeroes both, so a grid that does not write the
		 * two-value form is byte-identical to before.
		 */
		int gap = track_gap;
		int rgap = row_gap;
		int colw = (content_w - gap * (grid_cols - 1)) / grid_cols;
		if (colw < 60) { grid_cols = 1; colw = content_w; gap = 0; rgap = 0; }
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
							row_y = row_max + rgap;
							row_max = row_y;
						}
					}
					dom_node_get_next_sibling(child, &next);
					dom_node_unref(child);
					child = next;
				}
			}
			st->cursor_y = (col == 0 && row_max > row_y) ? row_max
				: ((col == 0) ? (row_y - rgap) : row_max);
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
		/* #245 engwraprev: the container's computed flex-wrap, read
		 * once so the wrap gate below can admit BOTH wrap and
		 * wrap-reverse and tell flex_wrap_lines which it got. */
		uint8_t fx_wrap = CSS_FLEX_WRAP_INHERIT;
		/*
		 * A ROW flex container: record each item's item-array boundary,
		 * start line and flex-grow factor during the (unchanged) child
		 * walk, then SIZE (flex-grow, #245 engflexgrow) and ALIGN
		 * (justify/align, engflex) the line. The recording changes no
		 * emission, and both post-passes are inert for a default
		 * container (no grow, default justify/align), so a plain flex
		 * container's item list is byte-identical.
		 */
		/*
		 * engwalkstack (#245): the seven per-item arrays are checked out
		 * of the LIFO pool on lstate instead of being declared here, where
		 * they cost 40 bytes per item of cap on EVERY walk frame of EVERY
		 * page. A NULL frame (pool exhausted, or nesting past
		 * FLEX_MAX_NEST) takes the plain child walk below, which is the
		 * same packed-but-unaligned fallback the over-cap `ok = 0` path
		 * already produced.
		 */
		flex_frame *ff = (child_flex_row && style && !st->measuring)
				? flex_frame_get(st) : NULL;
		if (ff) {
			int *bnd = ff->bnd;
			int *cy = ff->cy;
			css_fixed *grow = ff->grow;
			css_fixed *shrink = ff->shrink;
			int *basis = ff->basis;
			/* #245 engstretchitems: per-item cross-axis inputs for
			 * align-items:stretch, read from each item's own style
			 * below because the wrap driver cannot get at it. */
			flex_cross_t *xc = ff->xc;
			int cnt = 0, ok = 1;
			int fstart = st->out->n_items;
			int ftop = st->cursor_y;
			fx_just = css_computed_justify_content(style);
			fx_align = css_computed_align_items(style);
			fx_wrap = css_computed_flex_wrap(style);
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
							/* #245 engstretchitems: inert
							 * until positively armed below. */
							xc[cnt].stretch = 0;
							xc[cnt].min_h = -1;
							xc[cnt].max_h = -1;
							/* #245 engflexclose: AUTO = 0 =
							 * "use the container's". */
							xc[cnt].pos = FLEX_SELF_AUTO;
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
									/* A positive length starts the item's
									 * main size in flex_basis_pass. A basis
									 * that resolves to exactly 0 WITH a
									 * positive flex-grow is the `flex: N`
									 * shorthand's seed (#245 engflexbasis):
									 * recorded as the distinct 0 sentinel
									 * flex_basis0_pass consumes.
									 * flex_basis_pass tests `basis[k] > 0`
									 * so it ignores the 0; the wrap driver
									 * consumes the same sentinel per line
									 * (#245 engflexwrap2). A negative
									 * length stays auto (-1). */
									int bpx = fixed_px(bv, bu,
											e.font_size, content_w);
									if (bpx > 0) basis[cnt] = bpx;
									else if (bpx == 0 && grow[cnt] > 0)
										basis[cnt] = 0;
								}
								/*
								 * #245 engstretchitems: does
								 * this item stretch on the cross
								 * axis, and what clamps it if it
								 * does?
								 *
								 * align-self OVERRIDES align-items
								 * per item; its initial value is
								 * auto, which means "whatever the
								 * container's align-items says".
								 * Resolving it here is what lets an
								 * item opt OUT of a stretch that is
								 * on by default, which matters
								 * precisely because stretch is the
								 * initial value.
								 *
								 * A PERCENTAGE height counts as
								 * AUTO, exactly as read_style
								 * treats it: it resolves against a
								 * containing-block height this
								 * engine does not track, so the box
								 * really is content-sized and
								 * stretching it is closer to right
								 * than leaving it behind.
								 *
								 * The css_fixed / css_unit locals
								 * are SEEDED before every call
								 * because some libcss getters write
								 * their out-params only for
								 * particular stored types.
								 *
								 * em resolves against e.font_size,
								 * the CONTAINER's font size, the
								 * same approximation the flex-basis
								 * read above already makes.
								 */
								{
								uint8_t asf = css_computed_align_self(ics);
								uint8_t rax = (asf == CSS_ALIGN_SELF_AUTO ||
										asf == CSS_ALIGN_SELF_INHERIT)
									? fx_align : asf;
								css_fixed cv = 0;
								css_unit cu = CSS_UNIT_PX;
								uint8_t ht = css_computed_height(ics, &cv, &cu);
								if (rax == CSS_ALIGN_ITEMS_STRETCH &&
										(ht != CSS_HEIGHT_SET ||
										 cu == CSS_UNIT_PCT))
									xc[cnt].stretch = 1;
								/*
								 * #245 engflexclose: PER-ITEM POSITIONAL
								 * align-self. Recorded ONLY when the item
								 * DECLARED the property; auto and inherit
								 * leave pos at FLEX_SELF_AUTO, which is 0
								 * and means "defer to the line keyword".
								 * auto IS the initial value, so a page
								 * that does not write align-self records
								 * nothing here and reaches no new branch.
								 *
								 * Everything that is not center or
								 * flex-end maps to FLEX_SELF_START,
								 * because top-packed is what this engine
								 * does with flex-start, baseline and
								 * stretch on the cross axis. That is NOT
								 * the same as AUTO: an item writing
								 * align-self:flex-start inside an
								 * align-items:center container must stay
								 * at the top, and only a value DISTINCT
								 * from AUTO can say so.
								 *
								 * `rax` above resolves auto against the
								 * container for the STRETCH decision,
								 * which is right there and would be wrong
								 * here. pos must record what the ITEM
								 * said, not what it resolves to, or an
								 * unwritten align-self would "override"
								 * the container with the container's own
								 * value and the inert path would stop
								 * being inert.
								 */
								if (asf != CSS_ALIGN_SELF_AUTO &&
										asf != CSS_ALIGN_SELF_INHERIT)
									xc[cnt].pos =
										(asf == CSS_ALIGN_SELF_CENTER)
											? FLEX_SELF_CENTER :
										(asf == CSS_ALIGN_SELF_FLEX_END)
											? FLEX_SELF_END
											: FLEX_SELF_START;
								cv = 0; cu = CSS_UNIT_PX;
								if (css_computed_min_height(ics, &cv, &cu)
										== CSS_MIN_HEIGHT_SET &&
										cu != CSS_UNIT_PCT) {
									int mp = fixed_px(cv, cu,
											e.font_size, -1);
									xc[cnt].min_h = (mp > 0) ? mp : -1;
								}
								cv = 0; cu = CSS_UNIT_PX;
								if (css_computed_max_height(ics, &cv, &cu)
										== CSS_MAX_HEIGHT_SET &&
										cu != CSS_UNIT_PCT) {
									int mp = fixed_px(cv, cu,
											e.font_size, -1);
									xc[cnt].max_h = (mp > 0) ? mp : -1;
								}
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
					(fx_wrap == CSS_FLEX_WRAP_WRAP ||
					 fx_wrap == CSS_FLEX_WRAP_WRAP_REVERSE)) {
				/*
				 * flex-wrap:wrap (#245 engflexwrap). Break the packed
				 * items onto multiple lines and lay each line out with
				 * the single-line passes, stacking down the cross axis.
				 * Gated on WRAP so nowrap and non-flex pages never
				 * enter here and stay byte-identical (AE=0).
				 *
				 * #245 engwraprev: WRAP-REVERSE now enters here TOO,
				 * because it wraps into exactly the same lines; only
				 * the CROSS axis is inverted, which flex_wrap_lines
				 * applies as a reflection at the very end. NOWRAP (the
				 * INITIAL value) and every page that does not write
				 * the keyword still cannot reach the reflection, and
				 * FLEX_WRAP_FWD is the inert value, so a `wrap`
				 * container is byte-identical by construction.
				 *
				 * align-content (#245 engaligncontent) rides along:
				 * the mapped keyword and the container's DEFINITE
				 * content cross size (e.height, -1 when height is
				 * auto) are all flex_wrap_lines needs to distribute
				 * the leftover cross space between the lines. Both
				 * are inert at their initial values.
				 */
				flex_wrap_lines(st, ftop, content_x, content_w,
						fx_just, fx_align,
						flex_align_content_of(
							css_computed_align_content(style)),
						e.height,
						(fx_wrap == CSS_FLEX_WRAP_WRAP_REVERSE)
							? FLEX_WRAP_REV : FLEX_WRAP_FWD,
						st->flex_gap, row_gap,
						bnd, grow, shrink, basis, xc, cnt);
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
				int used, grew, shrunk, seeded, mk;
				/* #245 engflexmin: each item's automatic minimum
				 * size, 0 (= no floor) for every item the
				 * basis-0 seed below does not collapse.
				 * #245 engwalkstack: pooled, same as the rest. */
				int *minf = ff->minf;
				int based = flex_basis_pass(st, ftop, bnd, cy,
						basis, cnt);
				used = st->cursor_x + based;
				for (mk = 0; mk < cnt; mk++)
					minf[mk] = 0;
				/*
				 * #245 engflexbasis: the `flex: N` shorthand's
				 * basis-0 grow seed. Inert unless an item carries
				 * the 0 sentinel above, so every page that does
				 * not write the shorthand is byte-identical.
				 */
				seeded = flex_basis0_pass(st, ftop, content_x,
						content_w, used, bnd, cy,
						basis, grow, cnt, minf);
				used += seeded;
				grew = flex_grow_pass(st, ftop, content_x,
						content_w, used, bnd, cy,
						grow, minf, cnt);
				used += grew;
				/*
				 * #245 engflexmin: grow and shrink must stay
				 * mutually exclusive. WITHOUT floors grow
				 * distributes EXACTLY the free space, so the line
				 * ends flush and shrink already saw freev == 0
				 * and returned 0 having touched nothing; skipping
				 * it there is inert. WITH a floor grow may
				 * legitimately overflow the container, and
				 * letting shrink claw that back would undo the
				 * very minimum CSS just imposed.
				 */
				shrunk = (grew > 0) ? 0 :
					flex_shrink_pass(st, ftop, content_x,
						content_w, used, bnd, cy,
						shrink, cnt);
				used += shrunk;
				/*
				 * ALIGN-ITEMS:STRETCH ON THE NOWRAP PATH
				 * (#245 engnowrapstretch). The single most
				 * common flex container on the real web is
				 * `display: flex` with nothing else authored,
				 * and BOTH of its defaults are the active
				 * value: align-items' initial value is stretch
				 * and flex-wrap's is nowrap. Until now stretch
				 * lived only inside flex_wrap_lines(), which a
				 * nowrap container cannot enter, so the common
				 * case did not stretch at all.
				 *
				 * SAME PRIMITIVE, SECOND CALLER. This grows
				 * nothing itself: it works out the single
				 * line's cross size and hands it to
				 * flex_stretch_items(), which is ABSOLUTE and
				 * GROW-ONLY, the property that makes it safe
				 * to call from more than one place. There is
				 * no second cross-growth path.
				 *
				 * THE LINE'S CROSS SIZE, both cases stated:
				 *   - AUTO container height (e.height < 0, the
				 *     overwhelming majority). The single line's
				 *     cross size is its own content, i.e. the
				 *     tallest item. Stretching the others up to
				 *     it is the whole point and is NOT a no-op:
				 *     it is what makes the cells of a flex row
				 *     the same height.
				 *   - DEFINITE container height. CSS Flexbox 1
				 *     section 9.4: a single-line flex container
				 *     with a definite cross size has ONE line
				 *     whose cross size IS that. So the declared
				 *     content height raises the target. It can
				 *     only ever RAISE it here, because the
				 *     primitive is grow-only: a container
				 *     SHORTER than its content leaves the items
				 *     at their natural size and overflows,
				 *     rather than shrinking them.
				 *
				 * A stretching item's min-height also raises the
				 * LINE, exactly as on the wrap path, because the
				 * hypothetical cross size a line is measured
				 * against is the item's content height clamped
				 * by its own min/max.
				 *
				 * THE PEN. The wrap driver sets cursor_y and
				 * line_height itself, so a raised line grows the
				 * container there. Here the walk already left
				 * line_height at the tallest item's height, so a
				 * line raised ABOVE that (only a min-height or a
				 * declared container height can do it) has to
				 * raise the pen too, or the grown cell would
				 * paint outside its own container. Re-measured
				 * rather than assumed, and only when something
				 * actually grew, so a pass that changes no box
				 * cannot move the pen by a single pixel.
				 *
				 * BOUNDED:
				 *   - `any`: at least one item positively armed
				 *     xc[].stretch. An all-definite-height row
				 *     never enters, and neither does a container
				 *     whose align-items is not stretch.
				 *   - `multi`: the SAME wrap guard flex_align
				 *     applies two lines below. If the plain flow
				 *     put an item on a later line, this is not
				 *     one flex line and single-line reasoning
				 *     does not hold, so nothing is touched.
				 *   - grow-only, auto cross size only, principal
				 *     box only: all inherited from the shared
				 *     primitive.
				 */
				{
					int sk, any = 0, multi = 0;
					for (sk = 0; sk < cnt; sk++)
						if (xc[sk].stretch) { any = 1; break; }
					for (sk = 0; any && sk < cnt; sk++)
						if (cy[sk] != ftop) { multi = 1; break; }
					if (any && !multi) {
						int lh = flex_line_cross(st, fstart,
								st->out->n_items, ftop);
						for (sk = 0; sk < cnt; sk++)
							if (xc[sk].stretch &&
									xc[sk].min_h > lh)
								lh = xc[sk].min_h;
						/* e.height is the DECLARED, non-percentage
						 * content height in px, and -1 for auto,
						 * so the auto case is inert here. */
						if (e.height > lh)
							lh = e.height;
						if (flex_stretch_items(st, ftop, lh,
								bnd, xc, 0, cnt) > 0) {
							int nh = flex_line_cross(st, fstart,
									st->out->n_items, ftop);
							if (nh > st->line_height)
								st->line_height = nh;
						}
					}
				}
				flex_align(st, fstart, st->out->n_items, ftop, content_x, content_w,
						used, fx_just, fx_align,
						bnd, cy, xc, cnt);
			}
			/*
			 * #245 engrowrev: flex-direction:row-reverse, LAST, as a
			 * main-axis reflection of whichever path above ran. It is
			 * deliberately OUTSIDE both branches and not plumbed into
			 * flex_wrap_lines(): the reflection is per ITEM about the
			 * container's main-axis content box, every line of a wrap
			 * container spans that same interval, and nothing after
			 * this reads an item's x. So one call covers the wrap, the
			 * wrap-reverse and the nowrap paths, and there is exactly
			 * one main-axis direction formula in this file.
			 *
			 * The gate is a REAL KEYWORD: flex-direction's initial
			 * value is `row`, `column` never sets child_flex_row at
			 * all, and child_flex_rowrev is an own flag whose inert
			 * value is 0, so no page that does not write
			 * `flex-direction: row-reverse` can reach this.
			 */
			if (ok && cnt > 0 && child_flex_rowrev == FLEX_ROW_REV)
				flex_main_reflect(st, content_x, content_w,
						bnd, cnt);
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
		/* engwalkstack (#245): LIFO release. Unconditional and on the one
		 * path out of both branches, so the frame cannot leak; a no-op for
		 * the NULL (not a flex container, or pool exhausted) case. */
		flex_frame_put(st, ff);
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
					r = q->x + item_run_w(st, q);   /* engletsp */
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
						right = q->x + item_run_w(st, q);   /* engletsp */
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
		if ((e.has_bg || has_any_border(&e) || has_outline(&e)) &&
				!e.visibility &&
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
					qx2 = q->x + item_run_w(st, q);   /* engletsp */
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
	bool save_hv = st->line_has_valign;   /* #245 engvalign */
	int save_oof = st->oof_max_y, save_gap = st->flex_gap;
	int save_meas = st->measuring;
	int right = 0, i;

	st->cursor_x = 0; st->cursor_y = 0;
	st->line_left = 0; st->line_right = probe_w;
	st->line_height = 0;
	st->line_has_content = false; st->pending_space = false;
	st->line_has_valign = false;   /* #245 engvalign */
	st->oof_max_y = 0; st->flex_gap = 0;
	st->measuring = 1;

	walk(st, cell, pstyle, pe, 0, probe_w, 0);

	for (i = save_n; i < st->out->n_items; i++) {
		layout_item *q = &st->out->items[i];
		int r;
		if (q->kind == 0)
			r = q->x + item_run_w(st, q);   /* engletsp */
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
	st->line_has_valign = save_hv;   /* #245 engvalign */
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

		if ((r->e.has_bg || has_any_border(&r->e) || has_outline(&r->e)) &&
				!r->e.visibility) {
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
	out->deep_truncated = 0;   /* engwalkstack (#245) */
	out->walk_depth_peak = 0;  /* engwalkstack (#245) */
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
	st.line_has_valign = false;   /* #245 engvalign; lstate is NOT memset */
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
	/*
	 * engwalkstack (#245): lstate is field-initialised, NOT memset (see the
	 * engfloat2 note above), so the depth counter and the pool head MUST be
	 * zeroed here. A garbage `depth` would refuse to walk any page at all or
	 * refuse nothing; a garbage `fp_free` would be freed as a pointer.
	 */
	st.depth = 0;
	st.depth_peak = 0;
	st.fp_free = NULL;
	st.fp_live = 0;
	st.fp_peak = 0;

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
	/* engwalkstack (#245): hand the pool back to the heap. The frames are
	 * per-document scratch, not a cache: a page keeps them for the whole
	 * walk and nothing outside it may read them. */
	flex_pool_drain(&st);
	out->walk_depth_peak = st.depth_peak;

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
