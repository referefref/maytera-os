/*
 * layout.c - MayteraOS minimal HTML layout engine (MIT, original code).
 * See layout.h. Integer-only fixed-point-free arithmetic.
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

#include "layout.h"
#include "css_select_bind.h"

/* ---- layout state ---- */
typedef struct {
	mcs_ctx *css;
	int width;                 /* content wrap width */
	int (*measure)(const char *, int);
	layout_result *out;

	int cursor_x;              /* current inline pen x */
	int cursor_y;              /* current top of the current line */
	int line_height;           /* tallest run on the current line */
	bool line_has_content;
	char cur_href[LAYOUT_HREF_MAX];  /* current <a> target, empty if none */
} lstate;

static void emit_run(lstate *st, const char *s, int len, int size,
		uint32_t color, int bold, int italic, int underline)
{
	layout_item *it;
	if (st->out->n_items >= LAYOUT_MAX_ITEMS) return;
	if (len <= 0) return;
	it = &st->out->items[st->out->n_items++];
	it->kind = 0; it->has_bg = 0; it->border_w = 0; it->form_kind = 0;
	if (len > LAYOUT_RUN_MAX - 1) len = LAYOUT_RUN_MAX - 1;
	memcpy(it->text, s, len);
	it->text[len] = '\0';
	it->x = st->cursor_x;
	it->y = st->cursor_y;
	it->size = size;
	it->color = color;
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

static void line_break(lstate *st)
{
	if (st->line_has_content || st->line_height > 0)
		st->cursor_y += st->line_height > 0 ? st->line_height : 18;
	st->cursor_x = 0;
	st->line_height = 0;
	st->line_has_content = false;
}

/* ---- style helpers ---- */
typedef struct {
	uint8_t display;
	int font_size;
	uint32_t color;
	int bold;
	int italic;
	int underline;
	int margin_top;
	int margin_bottom;
	uint32_t bg; int has_bg;
	uint32_t border_col; int border_w;
} estyle;

static int fixed_px(css_fixed v, css_unit unit, int font_size)
{
	/* Only px and em are common after compute_absolute; others approximated */
	switch (unit) {
	case CSS_UNIT_PX:  return FIXTOINT(v);
	case CSS_UNIT_EM:  return (FIXTOINT(v) * font_size);
	case CSS_UNIT_PT:  return (FIXTOINT(v) * 96) / 72;
	default:           return FIXTOINT(v);
	}
}

static void read_style(const css_computed_style *s, const estyle *parent,
		estyle *e)
{
	css_fixed fv; css_unit fu;
	css_color col;

	e->display = css_computed_display(s, false);

	css_computed_font_size(s, &fv, &fu);
	if (fu == CSS_UNIT_PX) e->font_size = FIXTOINT(fv);
	else e->font_size = parent ? parent->font_size : 16;
	if (e->font_size < 6) e->font_size = 6;
	if (e->font_size > 96) e->font_size = 96;

	if (css_computed_color(s, &col) == CSS_COLOR_COLOR)
		e->color = col & 0xffffff;
	else
		e->color = parent ? parent->color : 0x000000;

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
	{
		css_color bc;
		if (css_computed_background_color(s, &bc) == CSS_BACKGROUND_COLOR_COLOR
				&& (bc >> 24)) {
			e->has_bg = 1; e->bg = bc & 0xffffff;
		} else { e->has_bg = 0; e->bg = 0; }
	}
	{
		css_fixed bw; css_unit bu; css_color bcol;
		css_computed_border_top_width(s, &bw, &bu);
		int w = fixed_px(bw, bu, e->font_size);
		uint8_t bs = css_computed_border_top_style(s);
		if (w > 0 && bs != CSS_BORDER_STYLE_NONE && bs != CSS_BORDER_STYLE_HIDDEN
				&& css_computed_border_top_color(s, &bcol) == CSS_BORDER_COLOR_COLOR) {
			e->border_w = w > 6 ? 6 : w; e->border_col = bcol & 0xffffff;
		} else { e->border_w = 0; e->border_col = 0; }
	}
	{
		css_fixed mv; css_unit mu;
		css_computed_margin_top(s, &mv, &mu);
		e->margin_top = fixed_px(mv, mu, e->font_size);
		css_computed_margin_bottom(s, &mv, &mu);
		e->margin_bottom = fixed_px(mv, mu, e->font_size);
		if (e->margin_top < 0) e->margin_top = 0;
		if (e->margin_bottom < 0) e->margin_bottom = 0;
	}
}

/* #245 perf instrumentation: how many measure() calls one layout costs.
 * Each one is a SYS_MEASURE_TTF syscall, so this is a syscall count. */
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
	int  len;
	int  w;
	char k[LM_KEY];
} lm_cache[LM_SLOTS];

static void lm_reset(void)
{
	int i;
	for (i = 0; i < LM_SLOTS; i++) lm_cache[i].used = 0;
}

static int lmeasure(lstate *st, const char *s, int size)
{
	g_layout_measure_words++;
	unsigned int h = 2166136261u;
	int len = 0, i;
	int slot, w;

	while (s[len]) len++;
	if (len >= LM_KEY) {		/* too long to key: ask the kernel */
		g_layout_measure_calls++;
		{ unsigned long long _t = lrdtsc(); int _w = st->measure(s, size);
		  g_lp_meas += lrdtsc() - _t; return _w; }
	}
	for (i = 0; i < len; i++) {	/* FNV-1a over the bytes, then the size */
		h ^= (unsigned char) s[i];
		h *= 16777619u;
	}
	h ^= (unsigned int) size;
	h *= 16777619u;
	slot = (int) (h & (LM_SLOTS - 1));

	if (lm_cache[slot].used && lm_cache[slot].size == size &&
			lm_cache[slot].len == len) {
		for (i = 0; i < len; i++)
			if (lm_cache[slot].k[i] != s[i]) break;
		if (i == len) return lm_cache[slot].w;	/* exact key match */
	}

	g_layout_measure_calls++;
	{ unsigned long long _t = lrdtsc();
	  w = st->measure(s, size);
	  g_lp_meas += lrdtsc() - _t; }
	lm_cache[slot].used = 1;
	lm_cache[slot].size = size;
	lm_cache[slot].len  = len;
	lm_cache[slot].w    = w;
	for (i = 0; i < len; i++) lm_cache[slot].k[i] = s[i];
	return w;
}

/* ---- text emission with word wrap ---- */
static void layout_text(lstate *st, const char *text, size_t len,
		const estyle *e)
{
	size_t i = 0;
	int space_w = lmeasure(st, " ", e->font_size);
	if (space_w <= 0) space_w = e->font_size / 3 + 1;

	while (i < len) {
		size_t ws, we;
		char word[LAYOUT_RUN_MAX];
		int wl, ww;
		bool had_space = false;

		/* skip whitespace (collapse) */
		while (i < len && (text[i] == ' ' || text[i] == '\t' ||
				text[i] == '\n' || text[i] == '\r')) {
			i++; had_space = true;
		}
		if (i >= len) break;
		ws = i;
		while (i < len && !(text[i] == ' ' || text[i] == '\t' ||
				text[i] == '\n' || text[i] == '\r')) i++;
		we = i;
		wl = (int)(we - ws);
		if (wl > LAYOUT_RUN_MAX - 1) wl = LAYOUT_RUN_MAX - 1;
		memcpy(word, text + ws, wl);
		word[wl] = '\0';
		ww = lmeasure(st, word, e->font_size);
		if (ww <= 0) ww = wl * (e->font_size / 2 + 1);

		/* leading space between words on same line */
		if (had_space && st->line_has_content)
			st->cursor_x += space_w;

		/* wrap if needed */
		if (st->line_has_content && st->cursor_x + ww > st->width) {
			line_break(st);
		}

		emit_run(st, word, wl, e->font_size, e->color, e->bold,
				e->italic, e->underline);
		st->cursor_x += ww;
		if (e->font_size + 4 > st->line_height)
			st->line_height = e->font_size + 4;
		st->line_has_content = true;
	}
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

/* ---- recursive box walk ---- */
static void walk(lstate *st, dom_node *node, const css_computed_style *pstyle,
		const estyle *pe)
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
		style = NULL;
	} else {
		read_style(style, pe, &e);
	}

	if (e.display == CSS_DISPLAY_NONE) {
		if (style) css_computed_style_destroy(style);
		return;
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
			int bw = iw > 0 ? iw : 120, bh = ih > 0 ? ih : 90;
			if (bw > st->width) bw = st->width;
			if (bw > 600) bw = 600;
			if (bh > 500) bh = 500;
			line_break(st);
			if (st->out->n_items < LAYOUT_MAX_ITEMS) {
				layout_item *bx = &st->out->items[st->out->n_items++];
				bx->kind = 3; bx->x = 0; bx->y = st->cursor_y; bx->w = bw; bx->h = bh;
				bx->has_bg = 0; bx->border_w = 0; bx->form_kind = 0; bx->text[0] = 0;
				int k = 0;
				while (src[k] && k < LAYOUT_HREF_MAX - 1) { bx->href[k] = src[k]; k++; }
				bx->href[k] = 0;
			}
			st->cursor_y += bh + 4;
		}
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
			if (href_changed) memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);
			if (style) css_computed_style_destroy(style);
			return;
		}
		line_break(st);
		int fh = e.font_size + 10;
		int fw = is_btn ? 130 : 260;
		char fname[64]; get_attr(node, "name", fname, sizeof fname);
		char faction[LAYOUT_HREF_MAX]; get_form_action(node, faction, sizeof faction);
		if (st->out->n_items < LAYOUT_MAX_ITEMS) {
			layout_item *bx = &st->out->items[st->out->n_items++];
			bx->kind = 1; bx->x = 0; bx->y = st->cursor_y; bx->w = fw; bx->h = fh;
			bx->has_bg = 1; bx->bg = is_btn ? 0x00E2E2E2u : 0x00FFFFFFu;
			bx->border_col = 0x00909090u; bx->border_w = 1;
			bx->text[0] = 0;
			bx->form_kind = is_btn ? 2 : 1;
			{ int i = 0; while (fname[i] && i < 63) { bx->field_name[i] = fname[i]; i++; } bx->field_name[i] = 0; }
			{ int i = 0; while (faction[i] && i < LAYOUT_HREF_MAX - 1) { bx->href[i] = faction[i]; i++; } bx->href[i] = 0; }
		}
		char label[96];
		get_attr(node, "placeholder", label, sizeof label);
		if (!label[0]) get_attr(node, "value", label, sizeof label);
		if (!label[0] && is_btn) { label[0] = 'S'; label[1] = 'u'; label[2] = 'b';
			label[3] = 'm'; label[4] = 'i'; label[5] = 't'; label[6] = 0; }
		if (label[0]) {
			st->cursor_x = 6;
			emit_run(st, label, lz_len(label), e.font_size,
					is_btn ? 0x00202020u : 0x00555555u, 0, 0, 0);
		}
		st->cursor_x = 0;
		st->cursor_y += fh + 4;
		if (href_changed) memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);
		if (style) css_computed_style_destroy(style);
		return;
	}

	bool is_block = (e.display == CSS_DISPLAY_BLOCK ||
			e.display == CSS_DISPLAY_LIST_ITEM ||
			e.display == CSS_DISPLAY_TABLE ||
			e.display == CSS_DISPLAY_TABLE_ROW ||
			e.display == CSS_DISPLAY_TABLE_CELL);

	if (is_block) {
		line_break(st);
		st->cursor_y += e.margin_top;
	}

	int box_idx = -1;
	int box_start_y = st->cursor_y;
	if (is_block && (e.has_bg || e.border_w) && st->out->n_items < LAYOUT_MAX_ITEMS) {
		box_idx = (int) st->out->n_items++;
		layout_item *bx = &st->out->items[box_idx];
		bx->kind = 1; bx->x = 0; bx->y = box_start_y; bx->w = st->width; bx->h = 0;
		bx->has_bg = e.has_bg; bx->bg = e.bg;
		bx->border_col = e.border_col; bx->border_w = e.border_w;
		bx->text[0] = 0; bx->href[0] = 0; bx->form_kind = 0;
	}

	/* list-item bullet */
	if (e.display == CSS_DISPLAY_LIST_ITEM) {
		emit_run(st, "\x95", 1, e.font_size, e.color, 0, 0, 0);
		st->cursor_x += lmeasure(st, "- ", e.font_size);
		st->line_has_content = true;
		if (e.font_size + 4 > st->line_height)
			st->line_height = e.font_size + 4;
	}

	/* children */
	{
		dom_node *child = NULL;
		if (dom_node_get_first_child(node, &child) == DOM_NO_ERR) {
			while (child) {
				dom_node *next = NULL;
				walk(st, child, style ? style : pstyle,
						style ? &e : pe);
				dom_node_get_next_sibling(child, &next);
				dom_node_unref(child);
				child = next;
			}
		}
	}

	if (box_idx >= 0) {
		st->out->items[box_idx].h = st->cursor_y - box_start_y;
	}

	if (is_block) {
		line_break(st);
		st->cursor_y += e.margin_bottom;
	}

	if (href_changed) {
		memcpy(st->cur_href, href_saved, LAYOUT_HREF_MAX);
	}

	if (style) css_computed_style_destroy(style);
}

int layout_document(mcs_ctx *css, dom_document *doc, int content_width,
		int (*measure)(const char *, int), layout_result *out)
{
	lstate st;
	dom_node *root = NULL;
	estyle base;

	if (!css || !doc || !out || !out->items) return -1;

	lm_reset();		/* #245: font face may have changed since the last page */
	out->n_items = 0;
	g_lp_style = g_lp_text = g_lp_flow = g_lp_meas = g_lp_attr = 0;
	g_lp_nelem = g_lp_ntext = 0;
	g_lp_walk = lrdtsc();
	out->content_height = 0;

	st.css = css;
	st.width = content_width > 40 ? content_width : 40;
	st.measure = measure;
	st.out = out;
	st.cursor_x = 0;
	st.cursor_y = 0;
	st.line_height = 0;
	st.line_has_content = false;
	st.cur_href[0] = 0;

	memset(&base, 0, sizeof(base));
	base.display = CSS_DISPLAY_BLOCK;
	base.font_size = 16;
	base.color = 0x000000;

	if (dom_document_get_document_element(doc, &root) != DOM_NO_ERR ||
			root == NULL)
		return -1;

	walk(&st, root, NULL, &base);
	line_break(&st);
	g_lp_walk = lrdtsc() - g_lp_walk;

	dom_node_unref(root);
	out->content_height = st.cursor_y;
	return 0;
}
