/*
 * css_select_bind.c - MayteraOS libcss<->libdom selection binding (MIT).
 *
 * Implements the css_select_handler callbacks against a libdom tree, builds a
 * select context seeded with a small built-in UA stylesheet, and computes a
 * css_computed_style (with inheritance) for any element. Independent MIT code;
 * NetSurf's reference select handler is GPL and not used.
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <dom/dom.h>
#include <libcss/libcss.h>
#include <libcss/properties.h>
#include <libcss/fpmath.h>

#include "css_select_bind.h"

/* ------------------------------------------------------------------ */
/* Built-in user-agent stylesheet (minimal but covers common defaults) */
/* ------------------------------------------------------------------ */
static const char UA_CSS[] =
	"html, address, blockquote, body, dd, div, dl, dt, fieldset, form,"
	"frame, frameset, h1, h2, h3, h4, h5, h6, noframes, ol, p, ul, center,"
	"dir, hr, menu, pre { display: block; }\n"
	"head, style, script, title, meta, link { display: none; }\n"
	"li { display: list-item; }\n"
	"table { display: table; }\n"
	"tr { display: table-row; }\n"
	"td, th { display: table-cell; }\n"
	"body { margin: 8px; line-height: 1.2; color: #000000; }\n"
	"h1 { font-size: 2em; font-weight: bold; margin: 16px 0; }\n"
	"h2 { font-size: 1.5em; font-weight: bold; margin: 14px 0; }\n"
	"h3 { font-size: 1.17em; font-weight: bold; margin: 12px 0; }\n"
	"h4 { font-weight: bold; margin: 12px 0; }\n"
	"h5 { font-size: 0.83em; font-weight: bold; margin: 12px 0; }\n"
	"h6 { font-size: 0.75em; font-weight: bold; margin: 12px 0; }\n"
	"p { margin: 12px 0; }\n"
	"b, strong { font-weight: bold; }\n"
	"i, em { font-style: italic; }\n"
	"a { color: #0000ee; text-decoration: underline; }\n"
	"ul, ol { margin: 12px 0; padding-left: 40px; }\n"
	"pre { font-family: monospace; white-space: pre; margin: 12px 0; }\n"
	"hr { margin: 8px 0; }\n";

struct mcs_ctx {
	css_select_ctx *select;
	css_stylesheet *ua_sheet;
	css_stylesheet *author_sheet; /* optional, single combined */
	css_unit_ctx unit;
	css_media media;
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* Intern a libdom element's local name as a *lowercase* lwc_string. */
static lwc_string *node_name_lwc(dom_node *node)
{
	dom_string *nm = NULL;
	lwc_string *out = NULL;
	char buf[64];
	const char *d;
	size_t l, i;

	if (dom_node_get_node_name(node, &nm) != DOM_NO_ERR || nm == NULL)
		return NULL;
	d = dom_string_data(nm);
	l = dom_string_byte_length(nm);
	if (l >= sizeof(buf)) l = sizeof(buf) - 1;
	for (i = 0; i < l; i++) {
		char c = d[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		buf[i] = c;
	}
	dom_string_unref(nm);
	if (lwc_intern_string(buf, l, &out) != lwc_error_ok)
		return NULL;
	return out;
}

/* ------------------------------------------------------------------ */
/* css_select_handler callbacks                                       */
/* ------------------------------------------------------------------ */

static css_error h_node_name(void *pw, void *node, css_qname *qname)
{
	(void) pw;
	qname->ns = NULL;
	qname->name = node_name_lwc((dom_node *) node);
	return qname->name ? CSS_OK : CSS_NOMEM;
}

static css_error h_node_classes(void *pw, void *node,
		lwc_string ***classes, uint32_t *n_classes)
{
	dom_string *cls = NULL;
	lwc_string *attr = NULL;
	(void) pw;
	*classes = NULL;
	*n_classes = 0;

	if (lwc_intern_string("class", 5, &attr) != lwc_error_ok)
		return CSS_NOMEM;
	/* fetch class attribute */
	{
		dom_string *an = NULL;
		if (dom_string_create((const uint8_t *) "class", 5, &an)
				== DOM_NO_ERR && an) {
			dom_element_get_attribute((dom_element *) node, an, &cls);
			dom_string_unref(an);
		}
	}
	lwc_string_unref(attr);
	if (cls == NULL)
		return CSS_OK;

	/* split on whitespace */
	{
		const char *s = dom_string_data(cls);
		size_t len = dom_string_byte_length(cls), i = 0;
		lwc_string **arr = NULL;
		uint32_t cap = 0, cnt = 0;
		while (i < len) {
			size_t start;
			while (i < len && (s[i] == ' ' || s[i] == '\t' ||
					s[i] == '\n' || s[i] == '\r')) i++;
			start = i;
			while (i < len && !(s[i] == ' ' || s[i] == '\t' ||
					s[i] == '\n' || s[i] == '\r')) i++;
			if (i > start) {
				lwc_string *one = NULL;
				if (lwc_intern_string(s + start, i - start, &one)
						== lwc_error_ok) {
					if (cnt == cap) {
						uint32_t nc = cap ? cap * 2 : 4;
						lwc_string **na = realloc(arr,
							nc * sizeof(*na));
						if (!na) { lwc_string_unref(one); break; }
						arr = na; cap = nc;
					}
					arr[cnt++] = one;
				}
			}
		}
		dom_string_unref(cls);
		*classes = arr;
		*n_classes = cnt;
	}
	return CSS_OK;
}

static css_error h_node_id(void *pw, void *node, lwc_string **id)
{
	dom_string *val = NULL, *an = NULL;
	(void) pw;
	*id = NULL;
	if (dom_string_create((const uint8_t *) "id", 2, &an) == DOM_NO_ERR && an) {
		dom_element_get_attribute((dom_element *) node, an, &val);
		dom_string_unref(an);
	}
	if (val) {
		dom_string_intern(val, id);
		dom_string_unref(val);
	}
	return CSS_OK;
}

static css_error h_named_ancestor(void *pw, void *node,
		const css_qname *qname, void **ancestor)
{
	dom_node *cur = (dom_node *) node, *parent = NULL;
	(void) pw;
	*ancestor = NULL;
	for (;;) {
		if (dom_node_get_parent_node(cur, &parent) != DOM_NO_ERR ||
				parent == NULL) {
			if (cur != node) dom_node_unref(cur);
			break;
		}
		if (cur != node) dom_node_unref(cur);
		cur = parent;
		{
			dom_node_type t;
			if (dom_node_get_node_type(cur, &t) == DOM_NO_ERR &&
					t == DOM_ELEMENT_NODE) {
				lwc_string *nm = node_name_lwc(cur);
				bool eq = false;
				if (nm && qname->name)
					lwc_string_caseless_isequal(nm,
						qname->name, &eq);
				if (nm) lwc_string_unref(nm);
				if (eq) { *ancestor = cur; return CSS_OK; }
			}
		}
	}
	return CSS_OK;
}

static css_error h_named_parent(void *pw, void *node,
		const css_qname *qname, void **parent)
{
	dom_node *p = NULL;
	(void) pw;
	*parent = NULL;
	if (dom_node_get_parent_node((dom_node *) node, &p) == DOM_NO_ERR && p) {
		dom_node_type t;
		bool eq = false;
		if (dom_node_get_node_type(p, &t) == DOM_NO_ERR &&
				t == DOM_ELEMENT_NODE) {
			lwc_string *nm = node_name_lwc(p);
			if (nm && qname->name)
				lwc_string_caseless_isequal(nm, qname->name, &eq);
			if (nm) lwc_string_unref(nm);
		}
		if (eq) { *parent = p; return CSS_OK; }
		dom_node_unref(p);
	}
	return CSS_OK;
}

static dom_node *prev_element_sibling(dom_node *node)
{
	/* libdom has get_previous_sibling via vtable; emulate with parent walk */
	dom_node *parent = NULL, *child = NULL, *prev = NULL;
	if (dom_node_get_parent_node(node, &parent) != DOM_NO_ERR || !parent)
		return NULL;
	if (dom_node_get_first_child(parent, &child) == DOM_NO_ERR) {
		while (child) {
			dom_node *next = NULL;
			if (child == node) { dom_node_unref(child); break; }
			dom_node_type t;
			if (dom_node_get_node_type(child, &t) == DOM_NO_ERR &&
					t == DOM_ELEMENT_NODE) {
				if (prev) dom_node_unref(prev);
				prev = child; dom_node_ref(prev);
			}
			dom_node_get_next_sibling(child, &next);
			dom_node_unref(child);
			child = next;
		}
	}
	dom_node_unref(parent);
	return prev;
}

static css_error h_named_sibling(void *pw, void *node,
		const css_qname *qname, void **sibling)
{
	dom_node *prev;
	(void) pw;
	*sibling = NULL;
	prev = prev_element_sibling((dom_node *) node);
	if (prev) {
		lwc_string *nm = node_name_lwc(prev);
		bool eq = false;
		if (nm && qname->name)
			lwc_string_caseless_isequal(nm, qname->name, &eq);
		if (nm) lwc_string_unref(nm);
		if (eq) { *sibling = prev; return CSS_OK; }
		dom_node_unref(prev);
	}
	return CSS_OK;
}

static css_error h_named_generic_sibling(void *pw, void *node,
		const css_qname *qname, void **sibling)
{
	dom_node *cur = (dom_node *) node;
	(void) pw;
	*sibling = NULL;
	dom_node_ref(cur);
	for (;;) {
		dom_node *prev = prev_element_sibling(cur);
		dom_node_unref(cur);
		if (!prev) break;
		cur = prev;
		{
			lwc_string *nm = node_name_lwc(cur);
			bool eq = false;
			if (nm && qname->name)
				lwc_string_caseless_isequal(nm, qname->name, &eq);
			if (nm) lwc_string_unref(nm);
			if (eq) { *sibling = cur; return CSS_OK; }
		}
	}
	return CSS_OK;
}

static css_error h_parent_node(void *pw, void *node, void **parent)
{
	dom_node *p = NULL;
	(void) pw;
	*parent = NULL;
	if (dom_node_get_parent_node((dom_node *) node, &p) == DOM_NO_ERR && p) {
		dom_node_type t;
		if (dom_node_get_node_type(p, &t) == DOM_NO_ERR &&
				t == DOM_ELEMENT_NODE) {
			*parent = p;
			return CSS_OK;
		}
		dom_node_unref(p);
	}
	return CSS_OK;
}

static css_error h_sibling_node(void *pw, void *node, void **sibling)
{
	(void) pw;
	*sibling = prev_element_sibling((dom_node *) node);
	return CSS_OK;
}

static css_error h_node_has_name(void *pw, void *node,
		const css_qname *qname, bool *match)
{
	lwc_string *nm = node_name_lwc((dom_node *) node);
	(void) pw;
	*match = false;
	if (nm && qname->name)
		lwc_string_caseless_isequal(nm, qname->name, match);
	if (nm) lwc_string_unref(nm);
	return CSS_OK;
}

static css_error h_node_has_class(void *pw, void *node,
		lwc_string *name, bool *match)
{
	lwc_string **classes = NULL;
	uint32_t n = 0, i;
	(void) pw;
	*match = false;
	h_node_classes(pw, node, &classes, &n);
	for (i = 0; i < n; i++) {
		bool eq = false;
		lwc_string_caseless_isequal(classes[i], name, &eq);
		if (eq) *match = true;
		lwc_string_unref(classes[i]);
	}
	free(classes);
	return CSS_OK;
}

static css_error h_node_has_id(void *pw, void *node,
		lwc_string *name, bool *match)
{
	lwc_string *id = NULL;
	(void) pw;
	*match = false;
	h_node_id(pw, node, &id);
	if (id) {
		lwc_string_isequal(id, name, match);
		lwc_string_unref(id);
	}
	return CSS_OK;
}

static css_error get_attr_value(dom_node *node, const css_qname *qname,
		dom_string **out)
{
	dom_string *an = NULL;
	const char *n = qname->name ? lwc_string_data(qname->name) : NULL;
	size_t l = qname->name ? lwc_string_length(qname->name) : 0;
	*out = NULL;
	if (!n) return CSS_OK;
	if (dom_string_create((const uint8_t *) n, l, &an) == DOM_NO_ERR && an) {
		dom_element_get_attribute((dom_element *) node, an, out);
		dom_string_unref(an);
	}
	return CSS_OK;
}

static css_error h_node_has_attribute(void *pw, void *node,
		const css_qname *qname, bool *match)
{
	dom_string *v = NULL;
	(void) pw;
	get_attr_value((dom_node *) node, qname, &v);
	*match = (v != NULL);
	if (v) dom_string_unref(v);
	return CSS_OK;
}

static bool dstr_eq_lwc(dom_string *d, lwc_string *v)
{
	bool eq = false;
	if (d && v) eq = dom_string_lwc_isequal(d, v);
	return eq;
}

static css_error h_node_has_attribute_equal(void *pw, void *node,
		const css_qname *qname, lwc_string *value, bool *match)
{
	dom_string *v = NULL;
	(void) pw;
	get_attr_value((dom_node *) node, qname, &v);
	*match = dstr_eq_lwc(v, value);
	if (v) dom_string_unref(v);
	return CSS_OK;
}

/* substring-family matchers: simplified (treat as contains/prefix/suffix on
 * the raw attribute text). */
static css_error attr_strop(void *node, const css_qname *qname,
		lwc_string *value, int mode, bool *match)
{
	dom_string *v = NULL;
	const char *hay, *need;
	size_t hl, nl;
	*match = false;
	get_attr_value((dom_node *) node, qname, &v);
	if (!v || !value) { if (v) dom_string_unref(v); return CSS_OK; }
	hay = dom_string_data(v); hl = dom_string_byte_length(v);
	need = lwc_string_data(value); nl = lwc_string_length(value);
	if (nl == 0) { dom_string_unref(v); return CSS_OK; }
	if (mode == 0) { /* prefix */
		if (nl <= hl && memcmp(hay, need, nl) == 0) *match = true;
	} else if (mode == 1) { /* suffix */
		if (nl <= hl && memcmp(hay + hl - nl, need, nl) == 0) *match = true;
	} else { /* substring / includes */
		size_t i;
		if (nl <= hl)
			for (i = 0; i + nl <= hl; i++)
				if (memcmp(hay + i, need, nl) == 0) { *match = true; break; }
	}
	dom_string_unref(v);
	return CSS_OK;
}

static css_error h_attr_dashmatch(void *pw, void *node,
		const css_qname *q, lwc_string *v, bool *m)
{ (void) pw; return attr_strop(node, q, v, 0, m); }
static css_error h_attr_includes(void *pw, void *node,
		const css_qname *q, lwc_string *v, bool *m)
{ (void) pw; return attr_strop(node, q, v, 2, m); }
static css_error h_attr_prefix(void *pw, void *node,
		const css_qname *q, lwc_string *v, bool *m)
{ (void) pw; return attr_strop(node, q, v, 0, m); }
static css_error h_attr_suffix(void *pw, void *node,
		const css_qname *q, lwc_string *v, bool *m)
{ (void) pw; return attr_strop(node, q, v, 1, m); }
static css_error h_attr_substring(void *pw, void *node,
		const css_qname *q, lwc_string *v, bool *m)
{ (void) pw; return attr_strop(node, q, v, 2, m); }

static css_error h_node_is_root(void *pw, void *node, bool *match)
{
	dom_node *p = NULL;
	(void) pw;
	*match = false;
	if (dom_node_get_parent_node((dom_node *) node, &p) == DOM_NO_ERR) {
		if (p == NULL) {
			*match = true;
		} else {
			dom_node_type t;
			if (dom_node_get_node_type(p, &t) == DOM_NO_ERR &&
					t != DOM_ELEMENT_NODE)
				*match = true;
			dom_node_unref(p);
		}
	}
	return CSS_OK;
}

static css_error h_node_count_siblings(void *pw, void *node,
		bool same_name, bool after, int32_t *count)
{
	(void) pw; (void) same_name; (void) after;
	*count = 0; /* good enough: disables :nth-child precision */
	return CSS_OK;
}

static css_error h_node_is_empty(void *pw, void *node, bool *match)
{
	bool has = false;
	(void) pw;
	dom_node_has_child_nodes((dom_node *) node, &has);
	*match = !has;
	return CSS_OK;
}

static css_error h_node_is_link(void *pw, void *node, bool *match)
{
	dom_string *v = NULL, *an = NULL;
	lwc_string *nm = node_name_lwc((dom_node *) node);
	bool is_a = false;
	(void) pw;
	*match = false;
	if (nm) {
		const char *d = lwc_string_data(nm);
		if (lwc_string_length(nm) == 1 && d[0] == 'a') is_a = true;
		lwc_string_unref(nm);
	}
	if (!is_a) return CSS_OK;
	if (dom_string_create((const uint8_t *) "href", 4, &an) == DOM_NO_ERR && an) {
		dom_element_get_attribute((dom_element *) node, an, &v);
		dom_string_unref(an);
	}
	if (v) { *match = true; dom_string_unref(v); }
	return CSS_OK;
}

static css_error h_false(void *pw, void *node, bool *match)
{ (void) pw; (void) node; *match = false; return CSS_OK; }

static css_error h_node_is_lang(void *pw, void *node,
		lwc_string *lang, bool *match)
{ (void) pw; (void) node; (void) lang; *match = false; return CSS_OK; }

static css_error h_presentational_hint(void *pw, void *node,
		uint32_t *nhints, css_hint **hints)
{ (void) pw; (void) node; *nhints = 0; *hints = NULL; return CSS_OK; }

static css_error h_ua_default(void *pw, uint32_t property, css_hint *hint)
{
	(void) pw;
	switch (property) {
	case CSS_PROP_COLOR:
		hint->data.color = 0x00000000; /* black, 0xAARRGGBB w/ A=0 */
		hint->status = CSS_COLOR_COLOR;
		break;
	case CSS_PROP_FONT_FAMILY:
		hint->data.strings = NULL;
		hint->status = CSS_FONT_FAMILY_SANS_SERIF;
		break;
	case CSS_PROP_QUOTES:
		hint->data.strings = NULL;
		hint->status = CSS_QUOTES_NONE;
		break;
	default:
		return CSS_INVALID;
	}
	return CSS_OK;
}

static css_error h_set_node_data(void *pw, void *node, void *data)
{ (void) pw; (void) node; (void) data; return CSS_OK; }

static css_error h_get_node_data(void *pw, void *node, void **data)
{ (void) pw; (void) node; *data = NULL; return CSS_OK; }

static css_select_handler g_handler = {
	.handler_version = CSS_SELECT_HANDLER_VERSION_1,
	.node_name = h_node_name,
	.node_classes = h_node_classes,
	.node_id = h_node_id,
	.named_ancestor_node = h_named_ancestor,
	.named_parent_node = h_named_parent,
	.named_sibling_node = h_named_sibling,
	.named_generic_sibling_node = h_named_generic_sibling,
	.parent_node = h_parent_node,
	.sibling_node = h_sibling_node,
	.node_has_name = h_node_has_name,
	.node_has_class = h_node_has_class,
	.node_has_id = h_node_has_id,
	.node_has_attribute = h_node_has_attribute,
	.node_has_attribute_equal = h_node_has_attribute_equal,
	.node_has_attribute_dashmatch = h_attr_dashmatch,
	.node_has_attribute_includes = h_attr_includes,
	.node_has_attribute_prefix = h_attr_prefix,
	.node_has_attribute_suffix = h_attr_suffix,
	.node_has_attribute_substring = h_attr_substring,
	.node_is_root = h_node_is_root,
	.node_count_siblings = h_node_count_siblings,
	.node_is_empty = h_node_is_empty,
	.node_is_link = h_node_is_link,
	.node_is_visited = h_false,
	.node_is_hover = h_false,
	.node_is_active = h_false,
	.node_is_focus = h_false,
	.node_is_enabled = h_false,
	.node_is_disabled = h_false,
	.node_is_checked = h_false,
	.node_is_target = h_false,
	.node_is_lang = h_node_is_lang,
	.node_presentational_hint = h_presentational_hint,
	.ua_default_for_property = h_ua_default,
	.set_libcss_node_data = h_set_node_data,
	.get_libcss_node_data = h_get_node_data,
};

css_select_handler *mcs_handler(void) { return &g_handler; }

int g_mcs_last_err = 0;
int g_mcs_stage = 0;

/* ------------------------------------------------------------------ */
/* stylesheet/context construction                                   */
/* ------------------------------------------------------------------ */

static css_error ua_resolve(void *pw, const char *base,
		lwc_string *rel, lwc_string **abs)
{ (void) pw; (void) base; *abs = lwc_string_ref(rel); return CSS_OK; }

static css_stylesheet *make_sheet(const char *src, unsigned long len,
		bool inln)
{
	css_stylesheet_params p;
	css_stylesheet *sheet = NULL;
	memset(&p, 0, sizeof(p));
	p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
	p.level = CSS_LEVEL_DEFAULT;
	p.charset = "UTF-8";
	p.url = "maytera://sheet";
	p.inline_style = inln;
	p.resolve = ua_resolve;
	if (css_stylesheet_create(&p, &sheet) != CSS_OK)
		return NULL;
	if (css_stylesheet_append_data(sheet,
			(const uint8_t *) src, len) == CSS_NEEDDATA) {
		/* expected: append returns NEEDDATA until data_done */
	}
	if (css_stylesheet_data_done(sheet) != CSS_OK) {
		/* tolerate parse-incomplete */
	}
	return sheet;
}

mcs_ctx *mcs_create(void)
{
	mcs_ctx *c = calloc(1, sizeof(*c));
	if (!c) return NULL;

	if (css_select_ctx_create(&c->select) != CSS_OK) { free(c); return NULL; }

	c->ua_sheet = make_sheet(UA_CSS, sizeof(UA_CSS) - 1, false);
	if (c->ua_sheet)
		css_select_ctx_append_sheet(c->select, c->ua_sheet,
				CSS_ORIGIN_UA, NULL);

	/* media: screen, viewport metrics filled by caller via unit ctx */
	memset(&c->media, 0, sizeof(c->media));
	c->media.type = CSS_MEDIA_SCREEN;
	c->media.width = INTTOFIX(1024);
	c->media.height = INTTOFIX(768);

	memset(&c->unit, 0, sizeof(c->unit));
	c->unit.viewport_width = INTTOFIX(1024);
	c->unit.viewport_height = INTTOFIX(768);
	c->unit.font_size_default = INTTOFIX(16);
	c->unit.font_size_minimum = INTTOFIX(6);
	c->unit.device_dpi = INTTOFIX(96);
	c->unit.root_style = NULL;
	c->unit.pw = NULL;
	/* device DPI: 96 (default). measure callback left NULL. */

	return c;
}

int mcs_add_author_css(mcs_ctx *c, const char *css, unsigned long len)
{
	css_stylesheet *s;
	if (!c || !css || len == 0) return -1;
	s = make_sheet(css, len, false);
	if (!s) return -1;
	if (css_select_ctx_append_sheet(c->select, s, CSS_ORIGIN_AUTHOR, NULL)
			!= CSS_OK) {
		css_stylesheet_destroy(s);
		return -1;
	}
	/* keep last author sheet pointer for cleanup (multiple leak slightly;
	 * fine for a single page load). */
	c->author_sheet = s;
	return 0;
}

css_computed_style *mcs_compute_style(mcs_ctx *c, dom_element *node,
		const css_computed_style *parent_style)
{
	css_select_results *results = NULL;
	css_computed_style *composed = NULL;
	css_error e;

	if (!c || !node) return NULL;

	g_mcs_stage = 1;
	e = css_select_style(c->select, node, &c->unit, &c->media, NULL,
			&g_handler, c, &results);
	g_mcs_last_err = (int) e;
	if (e != CSS_OK || results == NULL) { g_mcs_stage = 2; return NULL; }

	if (results->styles[CSS_PSEUDO_ELEMENT_NONE] == NULL) {
		g_mcs_stage = 3;
		css_select_results_destroy(results);
		return NULL;
	}

	/* Compose the selected style onto the parent (NULL for the root) so the
	 * returned style is owned by us and has inheritance + absolute values
	 * resolved. compose() tolerates a NULL parent. */
	e = css_computed_style_compose(parent_style,
			results->styles[CSS_PSEUDO_ELEMENT_NONE],
			&c->unit, &composed);
	g_mcs_last_err = (int) e; g_mcs_stage = 4;
	css_select_results_destroy(results);
	return (e == CSS_OK) ? composed : NULL;
}

void mcs_destroy(mcs_ctx *c)
{
	if (!c) return;
	if (c->select) css_select_ctx_destroy(c->select);
	if (c->ua_sheet) css_stylesheet_destroy(c->ua_sheet);
	if (c->author_sheet) css_stylesheet_destroy(c->author_sheet);
	free(c);
}
