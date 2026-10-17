/*
 * dom_hubbub_bind.c - MayteraOS hubbub->libdom tree-construction binding.
 * Licensed under the MIT License.
 *
 * Independent MIT reimplementation of the hubbub tree_handler callbacks that
 * NetSurf normally provides under GPL. Bridges hubbub's parse events to the
 * libdom core/HTML DOM API so a real HTML document tree is built.
 *
 * The "node" handles hubbub passes around are libdom dom_node pointers.
 * Reference counting maps directly onto dom_node_ref / dom_node_unref.
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <dom/dom.h>
#include <hubbub/hubbub.h>
#include <hubbub/parser.h>
#include <hubbub/tree.h>

#include <maytera-dom-limits.h>	/* MAYTERA_DOM_MAX_DEPTH (#245) */
#include "dom_hubbub_bind.h"

/* libdom internal HTML document factory (declared here to avoid pulling the
 * private src/html headers). Symbol confirmed present in libdom.a. */
extern dom_exception _dom_html_document_create(void *daf, void *daf_ctx,
		dom_html_document **doc);

struct mdb_parser {
	dom_html_document *html_doc; /* owning ref */
	dom_document     *doc;       /* same object, core view */
	hubbub_parser    *parser;
	hubbub_tree_handler tree;
};

/* ---- helpers ---- */

static dom_string *mk_dom_string(const hubbub_string *s)
{
	dom_string *out = NULL;
	if (s == NULL || s->ptr == NULL)
		return NULL;
	if (dom_string_create(s->ptr, s->len, &out) != DOM_NO_ERR)
		return NULL;
	return out;
}

/* ---- hubbub tree_handler callbacks ---- */

static hubbub_error cb_create_comment(void *ctx, const hubbub_string *data,
		void **result)
{
	struct mdb_parser *p = ctx;
	dom_string *s = mk_dom_string(data);
	dom_comment *c = NULL;
	dom_exception e;

	e = dom_document_create_comment(p->doc, s, &c);
	if (s) dom_string_unref(s);
	if (e != DOM_NO_ERR) { *result = NULL; return HUBBUB_UNKNOWN; }
	*result = c; /* create returns ref count 1 */
	return HUBBUB_OK;
}

/* Copy a hubbub_string into a freshly malloc'd NUL-terminated C string. */
static char *hs_to_cstr(const hubbub_string *s)
{
	char *out;
	if (s == NULL || s->ptr == NULL)
		return NULL;
	out = malloc(s->len + 1);
	if (out == NULL)
		return NULL;
	memcpy(out, s->ptr, s->len);
	out[s->len] = '\0';
	return out;
}

static hubbub_error cb_create_doctype(void *ctx, const hubbub_doctype *doctype,
		void **result)
{
	char *qname = hs_to_cstr(&doctype->name);
	char *pub = doctype->public_missing ? NULL
			: hs_to_cstr(&doctype->public_id);
	char *sys = doctype->system_missing ? NULL
			: hs_to_cstr(&doctype->system_id);
	dom_document_type *dt = NULL;
	dom_exception e;
	(void) ctx;

	e = dom_implementation_create_document_type(
			qname ? qname : "html", pub, sys, &dt);
	free(qname); free(pub); free(sys);
	if (e != DOM_NO_ERR) { *result = NULL; return HUBBUB_OK; /* tolerate */ }
	*result = dt;
	return HUBBUB_OK;
}

static hubbub_error cb_create_element(void *ctx, const hubbub_tag *tag,
		void **result)
{
	struct mdb_parser *p = ctx;
	dom_string *name = mk_dom_string(&tag->name);
	dom_element *el = NULL;
	dom_exception e;
	uint32_t i;

	/* Use the HTML document's create_element so the right element class is
	 * built; namespace handling is implied for HTML content. */
	e = dom_document_create_element(p->doc, name, &el);
	if (name) dom_string_unref(name);
	if (e != DOM_NO_ERR || el == NULL) { *result = NULL; return HUBBUB_UNKNOWN; }

	for (i = 0; i < tag->n_attributes; i++) {
		dom_string *an = mk_dom_string(&tag->attributes[i].name);
		dom_string *av = mk_dom_string(&tag->attributes[i].value);
		if (an && av)
			(void) dom_element_set_attribute(el, an, av);
		if (an) dom_string_unref(an);
		if (av) dom_string_unref(av);
	}

	*result = el;
	return HUBBUB_OK;
}

static hubbub_error cb_create_text(void *ctx, const hubbub_string *data,
		void **result)
{
	struct mdb_parser *p = ctx;
	dom_string *s = mk_dom_string(data);
	dom_text *t = NULL;
	dom_exception e;

	e = dom_document_create_text_node(p->doc, s, &t);
	if (s) dom_string_unref(s);
	if (e != DOM_NO_ERR) { *result = NULL; return HUBBUB_UNKNOWN; }
	*result = t;
	return HUBBUB_OK;
}

static hubbub_error cb_ref_node(void *ctx, void *node)
{
	(void) ctx;
	if (node) dom_node_ref((dom_node *) node);
	return HUBBUB_OK;
}

static hubbub_error cb_unref_node(void *ctx, void *node)
{
	(void) ctx;
	if (node) dom_node_unref((dom_node *) node);
	return HUBBUB_OK;
}

static hubbub_error cb_append_child(void *ctx, void *parent, void *child,
		void **result)
{
	dom_node *inserted = NULL;
	dom_exception e;
	(void) ctx;
	e = dom_node_append_child((dom_node *) parent, (dom_node *) child,
			&inserted);
	if (e != DOM_NO_ERR) { *result = NULL; return HUBBUB_UNKNOWN; }
	*result = inserted; /* ref count already increased by append */
	return HUBBUB_OK;
}

static hubbub_error cb_insert_before(void *ctx, void *parent, void *child,
		void *ref_child, void **result)
{
	dom_node *inserted = NULL;
	dom_exception e;
	(void) ctx;
	e = dom_node_insert_before((dom_node *) parent, (dom_node *) child,
			(dom_node *) ref_child, &inserted);
	if (e != DOM_NO_ERR) { *result = NULL; return HUBBUB_UNKNOWN; }
	*result = inserted;
	return HUBBUB_OK;
}

static hubbub_error cb_remove_child(void *ctx, void *parent, void *child,
		void **result)
{
	dom_node *removed = NULL;
	dom_exception e;
	(void) ctx;
	e = dom_node_remove_child((dom_node *) parent, (dom_node *) child,
			&removed);
	if (e != DOM_NO_ERR) { *result = NULL; return HUBBUB_UNKNOWN; }
	*result = removed;
	return HUBBUB_OK;
}

static hubbub_error cb_clone_node(void *ctx, void *node, bool deep,
		void **result)
{
	dom_node *clone = NULL;
	dom_exception e;
	(void) ctx;
	e = dom_node_clone_node((dom_node *) node, deep, &clone);
	if (e != DOM_NO_ERR) { *result = NULL; return HUBBUB_UNKNOWN; }
	*result = clone;
	return HUBBUB_OK;
}

static hubbub_error cb_reparent_children(void *ctx, void *node,
		void *new_parent)
{
	dom_node *child;
	dom_exception e;
	(void) ctx;

	/*
	 * #245 layoutsweep: PROGRESS HERE IS CONDITIONAL ON THE REMOVAL WORKING.
	 * Each pass re-reads the FIRST child and relies on the removal to make the
	 * next pass see a different one. A removal that fails leaves the same child
	 * in place, so the loop re-reads it forever, in Ring 3, on attacker-supplied
	 * HTML: this runs from the HTML5 adoption agency algorithm, which is driven
	 * entirely by mis-nested tags on the page.
	 *
	 * I COULD NOT CONSTRUCT AN INPUT THAT TRIGGERS IT, and say so rather than
	 * claim a bug I did not prove. _dom_node_remove_child() can return before
	 * detaching in three ways: a DOCUMENT parent refusing to drop an element or
	 * doctype, a readonly parent, or an allocation failure inside the
	 * DOMNodeRemoval dispatch. The first cannot happen here because libhubbub
	 * only ever passes stack[furthest_block].node (in_body.c:1779), which is at
	 * stack index >= 2 and therefore never the document; the second does not
	 * apply to elements hubbub builds. That leaves OOM, which would turn an
	 * allocation failure into a permanent spin instead of a failed page load.
	 *
	 * So this is a guard, not a fix for a demonstrated hang. It costs one
	 * comparison on a path that does not currently fire, and it converts the
	 * one remaining unguarded-progress loop in the engine's HTML tree
	 * construction into one that cannot spin whatever the callee does.
	 */
	for (;;) {
		dom_node *moved = NULL;
		e = dom_node_get_first_child((dom_node *) node, &child);
		if (e != DOM_NO_ERR || child == NULL)
			break;
		e = dom_node_remove_child((dom_node *) node, child, &moved);
		if (e != DOM_NO_ERR) {
			/* The child is still attached, so the next pass would read
			 * exactly this node again. Stop instead of spinning. */
			dom_node_unref(child);
			break;
		}
		if (moved != NULL) {
			dom_node *app = NULL;
			(void) dom_node_append_child((dom_node *) new_parent,
					moved, &app);
			if (app) dom_node_unref(app);
			dom_node_unref(moved);
		}
		dom_node_unref(child);
	}
	return HUBBUB_OK;
}

static hubbub_error cb_get_parent(void *ctx, void *node, bool element_only,
		void **result)
{
	dom_node *parent = NULL;
	dom_exception e;
	(void) ctx;
	e = dom_node_get_parent_node((dom_node *) node, &parent);
	if (e != DOM_NO_ERR) { *result = NULL; return HUBBUB_UNKNOWN; }

	if (parent != NULL && element_only) {
		dom_node_type t;
		if (dom_node_get_node_type(parent, &t) == DOM_NO_ERR &&
				t != DOM_ELEMENT_NODE) {
			dom_node_unref(parent);
			parent = NULL;
		}
	}
	*result = parent; /* get_parent already refs */
	return HUBBUB_OK;
}

static hubbub_error cb_has_children(void *ctx, void *node, bool *result)
{
	dom_exception e;
	(void) ctx;
	e = dom_node_has_child_nodes((dom_node *) node, result);
	if (e != DOM_NO_ERR) { *result = false; return HUBBUB_UNKNOWN; }
	return HUBBUB_OK;
}

static hubbub_error cb_form_associate(void *ctx, void *form, void *node)
{
	(void) ctx; (void) form; (void) node;
	return HUBBUB_OK; /* form association not needed for rendering */
}

static hubbub_error cb_add_attributes(void *ctx, void *node,
		const hubbub_attribute *attributes, uint32_t n_attributes)
{
	uint32_t i;
	(void) ctx;
	for (i = 0; i < n_attributes; i++) {
		dom_string *an = mk_dom_string(&attributes[i].name);
		dom_string *av = mk_dom_string(&attributes[i].value);
		if (an && av)
			(void) dom_element_set_attribute((dom_element *) node,
					an, av);
		if (an) dom_string_unref(an);
		if (av) dom_string_unref(av);
	}
	return HUBBUB_OK;
}

static hubbub_error cb_set_quirks_mode(void *ctx, hubbub_quirks_mode mode)
{
	(void) ctx; (void) mode;
	return HUBBUB_OK;
}

static hubbub_error cb_encoding_change(void *ctx, const char *encname)
{
	(void) ctx; (void) encname;
	return HUBBUB_OK; /* keep current handler (UTF-8) */
}

static hubbub_error cb_complete_script(void *ctx, void *script)
{
	(void) ctx; (void) script;
	return HUBBUB_OK;
}

/* ---- public API ---- */

mdb_parser *mdb_create(void)
{
	struct mdb_parser *p;
	dom_exception de;
	hubbub_error he;
	hubbub_parser_optparams params;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;

	de = _dom_html_document_create(NULL, NULL, &p->html_doc);
	if (de != DOM_NO_ERR || p->html_doc == NULL) {
		free(p);
		return NULL;
	}
	p->doc = (dom_document *) p->html_doc;

	he = hubbub_parser_create("UTF-8", true, &p->parser);
	if (he != HUBBUB_OK) {
		dom_node_unref((dom_node *) p->html_doc);
		free(p);
		return NULL;
	}

	memset(&p->tree, 0, sizeof(p->tree));
	p->tree.create_comment    = cb_create_comment;
	p->tree.create_doctype    = cb_create_doctype;
	p->tree.create_element    = cb_create_element;
	p->tree.create_text       = cb_create_text;
	p->tree.ref_node          = cb_ref_node;
	p->tree.unref_node        = cb_unref_node;
	p->tree.append_child      = cb_append_child;
	p->tree.insert_before     = cb_insert_before;
	p->tree.remove_child      = cb_remove_child;
	p->tree.clone_node        = cb_clone_node;
	p->tree.reparent_children = cb_reparent_children;
	p->tree.get_parent        = cb_get_parent;
	p->tree.has_children      = cb_has_children;
	p->tree.form_associate    = cb_form_associate;
	p->tree.add_attributes    = cb_add_attributes;
	p->tree.set_quirks_mode   = cb_set_quirks_mode;
	p->tree.encoding_change   = cb_encoding_change;
	p->tree.complete_script   = cb_complete_script;
	p->tree.ctx               = p;

	params.tree_handler = &p->tree;
	he = hubbub_parser_setopt(p->parser, HUBBUB_PARSER_TREE_HANDLER,
			&params);
	if (he != HUBBUB_OK)
		goto fail;

	/* The document node is the root the tree builder appends to. hubbub
	 * refs it via ref_node, so this borrowed pointer is fine. */
	params.document_node = p->html_doc;
	he = hubbub_parser_setopt(p->parser, HUBBUB_PARSER_DOCUMENT_NODE,
			&params);
	if (he != HUBBUB_OK)
		goto fail;

	return p;

fail:
	hubbub_parser_destroy(p->parser);
	dom_node_unref((dom_node *) p->html_doc);
	free(p);
	return NULL;
}

int mdb_parse_chunk(mdb_parser *p, const unsigned char *data, unsigned long len)
{
	if (p == NULL || data == NULL)
		return -1;
	return hubbub_parser_parse_chunk(p->parser, data, len) == HUBBUB_OK
			? 0 : -1;
}

int mdb_parse_complete(mdb_parser *p)
{
	if (p == NULL)
		return -1;
	return hubbub_parser_completed(p->parser) == HUBBUB_OK ? 0 : -1;
}

dom_document *mdb_document(mdb_parser *p)
{
	return p ? p->doc : NULL;
}

void mdb_destroy(mdb_parser *p)
{
	if (p == NULL)
		return;
	if (p->parser)
		hubbub_parser_destroy(p->parser);
	if (p->html_doc)
		dom_node_unref((dom_node *) p->html_doc);
	free(p);
}

/* ------------------------------------------------------------------ #245
 * deepdom: the in-OS launch canary for the deep-DOM depth cap.
 *
 * WHAT IT GUARDS. Building an n-deep DOM used to be O(n^2) in both the HTML
 * parser and the appendChild path, which pegged the browser at 100% CPU on a
 * page of nested div elements with no script on it at all. Four per-insertion
 * O(tree depth) walks were the cause (three in libdom, one in libhubbub) and
 * are now O(1); on top of that the parser clamps the depth it will build,
 * because libdom destroys a tree by RECURSING and a deep enough tree faults
 * the process on teardown no matter how fast it was built.
 *
 * The complexity fixes live in patched third-party source and cannot be
 * asserted from here. What CAN be asserted, cheaply, on every launch, is the
 * structural consequence that bounds the work: the parsed tree is clamped, and
 * clamping does not cost content. Both halves matter. A cap that dropped the
 * text would be a truncation bug wearing a fix's clothes, and the case just
 * under the cap proves the clamp is not firing on ordinary documents.
 *
 * Depths are small on purpose: this runs on every browser launch.
 */
static int mdb_st_depth(dom_node *n, int d, int *maxd, int *elems, int *texts)
{
	dom_node_type ty;
	dom_node *c = NULL;

	if (dom_node_get_node_type(n, &ty) == DOM_NO_ERR) {
		if (ty == DOM_ELEMENT_NODE) {
			(*elems)++;
			if (d > *maxd) *maxd = d;
		} else if (ty == DOM_TEXT_NODE) {
			(*texts)++;
		}
	}
	if (dom_node_get_first_child(n, &c) == DOM_NO_ERR) {
		while (c) {
			dom_node *nx = NULL;
			mdb_st_depth(c, d + 1, maxd, elems, texts);
			dom_node_get_next_sibling(c, &nx);
			dom_node_unref(c);
			c = nx;
		}
	}
	return 0;
}

/* Build "<html><body>" + n x "<div>" + "X" + n x "</div>" and measure it. */
static int mdb_st_nest(int n, int *maxd, int *elems, int *texts)
{
	/* 6 bytes per open tag + 6 per close + slack; n is bounded below. */
	static char buf[16384];
	int i, k = 0;
	mdb_parser *p;
	dom_document *doc;
	dom_element *root = NULL;

	*maxd = *elems = *texts = 0;
	if (n < 0 || (size_t) (n * 12 + 64) >= sizeof buf)
		return -1;
	for (i = 0; i < 12; i++) buf[k++] = "<html><body>"[i];
	for (i = 0; i < n; i++) {
		buf[k++] = '<'; buf[k++] = 'd'; buf[k++] = 'i';
		buf[k++] = 'v'; buf[k++] = '>';
	}
	buf[k++] = 'X';
	for (i = 0; i < n; i++) {
		buf[k++] = '<'; buf[k++] = '/'; buf[k++] = 'd';
		buf[k++] = 'i'; buf[k++] = 'v'; buf[k++] = '>';
	}
	buf[k] = 0;

	p = mdb_create();
	if (p == NULL)
		return -1;
	if (mdb_parse_chunk(p, (const unsigned char *) buf, (unsigned long) k) != 0 ||
			mdb_parse_complete(p) != 0) {
		mdb_destroy(p);
		return -1;
	}
	doc = mdb_document(p);
	if (doc == NULL ||
	    dom_document_get_document_element(doc, &root) != DOM_NO_ERR ||
	    root == NULL) {
		mdb_destroy(p);
		return -1;
	}
	mdb_st_depth((dom_node *) root, 0, maxd, elems, texts);
	dom_node_unref((dom_node *) root);
	mdb_destroy(p);
	return 0;
}

static int g_mdb_st_cases;

int mdb_depth_selftest_count(void) { return 5; }

int mdb_depth_selftest(void (*report)(const char *line))
{
	int fail = 0, maxd = 0, el = 0, tx = 0;
	char line[160];
	const int cap = MAYTERA_DOM_MAX_DEPTH;
	/* Well under the cap: nothing may be clamped and nothing lost. */
	const int shallow = 40;
	/* Comfortably over it, but small enough to parse in microseconds. */
	const int deep = cap + 200;

	g_mdb_st_cases = 0;

#define MDB_CHK(what, got, want) do {                                        \
		g_mdb_st_cases++;                                            \
		if ((got) != (want)) {                                       \
			fail++;                                              \
			if (report) {                                        \
				snprintf(line, sizeof line,                  \
					"[DEEPDOM]   FAIL %s: got %d want %d",\
					(what), (int) (got), (int) (want));  \
				report(line);                                \
			}                                                    \
		} else if (report) {                                          \
			snprintf(line, sizeof line,                          \
				"[DEEPDOM]   ok   %s = %d", (what),          \
				(int) (got));                                \
			report(line);                                        \
		}                                                            \
	} while (0)

	/* 1 + 2: a document that does NOT reach the cap is untouched.
	 * html is at depth 0, body at 1, so the deepest div sits at 1+shallow.
	 * The element count is html + head + body + shallow divs: the tree
	 * construction algorithm inserts an implied HEAD even though the source
	 * has none, which is why this is 3 and not 2. (Written as 2 first; the
	 * canary caught it on its first run in the OS, which is the only reason
	 * to have a canary.) */
	if (mdb_st_nest(shallow, &maxd, &el, &tx) != 0) {
		fail++;
		if (report) report("[DEEPDOM]   FAIL shallow parse errored");
		g_mdb_st_cases += 2;
	} else {
		MDB_CHK("shallow maxdepth", maxd, 1 + shallow);
		MDB_CHK("shallow elements", el, 3 + shallow);
	}

	/* 3 + 4: over the cap the tree is clamped, and the text survives. The
	 * open-element stack holds html at index 0 and body at 1, so the last
	 * div the parser accepts leaves current_node at cap-1, which is a DOM
	 * depth of cap-1. Pinning the exact value is the point: an off-by-one
	 * here is the difference between a cap and a truncation. */
	if (mdb_st_nest(deep, &maxd, &el, &tx) != 0) {
		fail++;
		if (report) report("[DEEPDOM]   FAIL deep parse errored");
		g_mdb_st_cases += 2;
	} else {
		MDB_CHK("deep maxdepth clamped", maxd, cap - 1);
		MDB_CHK("deep text nodes kept", tx, 1);
		/* And the clamp is a CLAMP, not a truncation of the element
		 * count: html + head at depths 0 and 1, body at 1, divs at 2
		 * through cap-1, so cap-2 divs are accepted and the total is
		 * 2 + 1 + (cap - 2) = cap + 1. Pinning this separately from
		 * maxdepth is what distinguishes "stopped nesting" from
		 * "stopped parsing". */
		MDB_CHK("deep elements accepted", el, cap + 1);
	}
#undef MDB_CHK
	return fail;
}

int mdb_depth_selftest_cases(void) { return g_mdb_st_cases; }
