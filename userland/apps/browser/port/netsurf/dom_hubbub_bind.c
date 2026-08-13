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

#include <dom/dom.h>
#include <hubbub/hubbub.h>
#include <hubbub/parser.h>
#include <hubbub/tree.h>

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

	for (;;) {
		dom_node *moved = NULL;
		e = dom_node_get_first_child((dom_node *) node, &child);
		if (e != DOM_NO_ERR || child == NULL)
			break;
		e = dom_node_remove_child((dom_node *) node, child, &moved);
		if (e == DOM_NO_ERR && moved != NULL) {
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
