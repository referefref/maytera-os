/* duk_dom.c - Duktape <-> libdom binding for the MayteraOS browser (MIT).
 *
 * Binds a minimal but real DOM into a Duktape context and runs the inline
 * <script> elements of a parsed document. Scripts can read/mutate the tree
 * (textContent, innerHTML, attributes, createElement/appendChild); the changes
 * are visible to the layout pass that runs afterwards.
 *
 * Ref-counting note: the browser intentionally leaks the parsed document for
 * the lifetime of the page, so tree-walk helpers here favour simplicity over
 * strict unref balancing (short-lived refs from get_first_child/next_sibling
 * are left for the document teardown to reclaim).
 */
#include "duktape.h"
#include "duk_dom.h"
#include <dom/dom.h>

extern unsigned long strlen(const char *);
extern int memcmp(const void *, const void *, unsigned long);

#define NODEPTR "\xFF" "nodeptr"

static dom_document *g_doc = 0;
static char *g_log = 0;
static int g_logcap = 0;
static int g_loglen = 0;

static void log_append(const char *s, int n) {
    if (!g_log) return;
    for (int i = 0; i < n && g_loglen < g_logcap - 1; i++) g_log[g_loglen++] = s[i];
    if (g_logcap > 0) g_log[g_loglen] = 0;
}

static dom_string *mkstr(const char *s) {
    dom_string *d = 0;
    if (dom_string_create((const uint8_t *) s, (size_t) strlen(s), &d) != DOM_NO_ERR) return 0;
    return d;
}

static void push_domstr(duk_context *ctx, dom_string *d) {
    if (!d) { duk_push_string(ctx, ""); return; }
    duk_push_lstring(ctx, dom_string_data(d), dom_string_byte_length(d));
}

/* ---- node object plumbing ---- */

static dom_node *this_node(duk_context *ctx) {
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, NODEPTR);
    dom_node *n = (dom_node *) duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    return n;
}

static dom_node *arg_node(duk_context *ctx, int idx) {
    if (!duk_is_object(ctx, idx)) return 0;
    duk_get_prop_string(ctx, idx, NODEPTR);
    dom_node *n = (dom_node *) duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    return n;
}

static void wrap_node(duk_context *ctx, dom_node *n) {
    if (!n) { duk_push_null(ctx); return; }
    duk_push_object(ctx);                       /* [obj] */
    duk_push_pointer(ctx, n);
    duk_put_prop_string(ctx, -2, NODEPTR);      /* [obj] */
    duk_push_global_stash(ctx);                 /* [obj stash] */
    duk_get_prop_string(ctx, -1, "nodeproto");  /* [obj stash proto] */
    duk_remove(ctx, -2);                        /* [obj proto] */
    duk_set_prototype(ctx, -2);                 /* proto applied to obj, popped -> [obj] */
}

/* ---- element accessors / methods ---- */

static duk_ret_t get_textContent(duk_context *ctx) {
    dom_node *n = this_node(ctx); dom_string *t = 0;
    if (n && dom_node_get_text_content(n, &t) == DOM_NO_ERR) {
        push_domstr(ctx, t); if (t) dom_string_unref(t);
    } else duk_push_string(ctx, "");
    return 1;
}

static duk_ret_t set_textContent(duk_context *ctx) {
    dom_node *n = this_node(ctx); const char *v = duk_safe_to_string(ctx, 0);
    if (n) { dom_string *d = mkstr(v); if (d) { dom_node_set_text_content(n, d); dom_string_unref(d); } }
    return 0;
}

/* innerHTML: get returns text content; set replaces children with a text node.
 * (Full HTML fragment parsing on set is a later step.) */
static duk_ret_t get_innerHTML(duk_context *ctx) { return get_textContent(ctx); }
static duk_ret_t set_innerHTML(duk_context *ctx) { return set_textContent(ctx); }

static duk_ret_t get_tagName(duk_context *ctx) {
    dom_node *n = this_node(ctx); dom_string *t = 0;
    if (n && dom_node_get_node_name(n, &t) == DOM_NO_ERR) {
        push_domstr(ctx, t); if (t) dom_string_unref(t);
    } else duk_push_string(ctx, "");
    return 1;
}

static duk_ret_t m_getAttribute(duk_context *ctx) {
    dom_node *n = this_node(ctx); const char *name = duk_safe_to_string(ctx, 0);
    dom_string *an = mkstr(name), *val = 0;
    if (n && an && dom_element_get_attribute((dom_element *) n, an, &val) == DOM_NO_ERR && val) {
        push_domstr(ctx, val); dom_string_unref(val);
    } else duk_push_null(ctx);
    if (an) dom_string_unref(an);
    return 1;
}

static duk_ret_t m_setAttribute(duk_context *ctx) {
    dom_node *n = this_node(ctx);
    const char *name = duk_safe_to_string(ctx, 0);
    const char *v = duk_safe_to_string(ctx, 1);
    dom_string *an = mkstr(name), *av = mkstr(v);
    if (n && an && av) dom_element_set_attribute((dom_element *) n, an, av);
    if (an) dom_string_unref(an);
    if (av) dom_string_unref(av);
    return 0;
}

static duk_ret_t m_appendChild(duk_context *ctx) {
    dom_node *parent = this_node(ctx), *child = arg_node(ctx, 0), *res = 0;
    if (parent && child) dom_node_append_child(parent, child, &res);
    duk_dup(ctx, 0);            /* DOM appendChild returns the appended child */
    return 1;
}

/* ---- document ---- */

static dom_node *find_by_id(dom_node *node, const char *id) {
    dom_node_type ty;
    if (dom_node_get_node_type(node, &ty) == DOM_NO_ERR && ty == DOM_ELEMENT_NODE) {
        dom_string *an = mkstr("id"), *val = 0;
        if (an && dom_element_get_attribute((dom_element *) node, an, &val) == DOM_NO_ERR && val) {
            size_t l = dom_string_byte_length(val);
            if (strlen(id) == l && memcmp(dom_string_data(val), id, l) == 0) {
                dom_string_unref(val); if (an) dom_string_unref(an);
                return node;
            }
            dom_string_unref(val);
        }
        if (an) dom_string_unref(an);
    }
    dom_node *child = 0;
    if (dom_node_get_first_child(node, &child) == DOM_NO_ERR) {
        while (child) {
            dom_node *f = find_by_id(child, id);
            if (f) return f;
            dom_node *next = 0; dom_node_get_next_sibling(child, &next);
            child = next;
        }
    }
    return 0;
}

static duk_ret_t doc_getElementById(duk_context *ctx) {
    const char *id = duk_safe_to_string(ctx, 0);
    dom_element *root = 0; dom_node *found = 0;
    if (g_doc && dom_document_get_document_element(g_doc, &root) == DOM_NO_ERR && root)
        found = find_by_id((dom_node *) root, id);
    wrap_node(ctx, found);
    return 1;
}

static duk_ret_t doc_createElement(duk_context *ctx) {
    const char *name = duk_safe_to_string(ctx, 0);
    dom_string *dn = mkstr(name); dom_element *el = 0;
    if (g_doc && dn && dom_document_create_element(g_doc, dn, &el) == DOM_NO_ERR && el) {
        if (dn) dom_string_unref(dn);
        wrap_node(ctx, (dom_node *) el);
    } else {
        if (dn) dom_string_unref(dn);
        duk_push_null(ctx);
    }
    return 1;
}

static duk_ret_t doc_write(duk_context *ctx) {
    int n = duk_get_top(ctx);
    for (int i = 0; i < n; i++) {
        const char *s = duk_safe_to_string(ctx, i);
        log_append(s, (int) strlen(s));
    }
    return 0;
}

static duk_ret_t native_log(duk_context *ctx) {
    int n = duk_get_top(ctx);
    for (int i = 0; i < n; i++) {
        if (i) log_append(" ", 1);
        const char *s = duk_safe_to_string(ctx, i);
        log_append(s, (int) strlen(s));
    }
    log_append("\n", 1);
    return 0;
}

static void setup_nodeproto(duk_context *ctx) {
    duk_push_global_stash(ctx);                 /* [stash] */
    duk_push_object(ctx);                       /* [stash proto] */
    duk_push_c_function(ctx, m_getAttribute, 1); duk_put_prop_string(ctx, -2, "getAttribute");
    duk_push_c_function(ctx, m_setAttribute, 2); duk_put_prop_string(ctx, -2, "setAttribute");
    duk_push_c_function(ctx, m_appendChild, 1);  duk_put_prop_string(ctx, -2, "appendChild");
    duk_push_string(ctx, "textContent");
    duk_push_c_function(ctx, get_textContent, 0);
    duk_push_c_function(ctx, set_textContent, 1);
    duk_def_prop(ctx, -4, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER);
    duk_push_string(ctx, "innerHTML");
    duk_push_c_function(ctx, get_innerHTML, 0);
    duk_push_c_function(ctx, set_innerHTML, 1);
    duk_def_prop(ctx, -4, DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER);
    duk_push_string(ctx, "tagName");
    duk_push_c_function(ctx, get_tagName, 0);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER);
    duk_put_prop_string(ctx, -2, "nodeproto"); /* [stash] */
    duk_pop(ctx);
}

static void setup_globals(duk_context *ctx) {
    duk_push_global_object(ctx);                /* [global] */
    /* console.log */
    duk_push_object(ctx);
    duk_push_c_function(ctx, native_log, DUK_VARARGS); duk_put_prop_string(ctx, -2, "log");
    duk_put_prop_string(ctx, -2, "console");
    /* document */
    duk_push_object(ctx);
    duk_push_c_function(ctx, doc_getElementById, 1); duk_put_prop_string(ctx, -2, "getElementById");
    duk_push_c_function(ctx, doc_createElement, 1);  duk_put_prop_string(ctx, -2, "createElement");
    duk_push_c_function(ctx, doc_write, DUK_VARARGS); duk_put_prop_string(ctx, -2, "write");
    duk_put_prop_string(ctx, -2, "document");
    /* window === globalThis */
    duk_push_global_object(ctx); duk_put_prop_string(ctx, -2, "window");
    duk_pop(ctx);
}

int js_run_document(dom_document *doc, char *logbuf, int logcap) {
    g_doc = doc; g_log = logbuf; g_logcap = logcap; g_loglen = 0;
    if (g_log && logcap > 0) g_log[0] = 0;

    duk_context *ctx = duk_create_heap_default();
    if (!ctx) { g_doc = 0; g_log = 0; return -1; }
    setup_nodeproto(ctx);
    setup_globals(ctx);

    int ran = 0;
    dom_string *tag = mkstr("script");
    dom_nodelist *list = 0;
    if (tag && dom_document_get_elements_by_tag_name(doc, tag, &list) == DOM_NO_ERR && list) {
        uint32_t len = 0;
        dom_nodelist_get_length(list, &len);
        for (uint32_t i = 0; i < len; i++) {
            dom_node *s = 0;
            if (dom_nodelist_item(list, i, &s) != DOM_NO_ERR || !s) continue;
            /* Skip external (src=) scripts for now; only run inline code. */
            dom_string *src_n = mkstr("src"), *src_v = 0;
            int has_src = 0;
            if (src_n && dom_element_get_attribute((dom_element *) s, src_n, &src_v) == DOM_NO_ERR && src_v) {
                has_src = 1; dom_string_unref(src_v);
            }
            if (src_n) dom_string_unref(src_n);
            if (!has_src) {
                dom_string *code = 0;
                if (dom_node_get_text_content(s, &code) == DOM_NO_ERR && code) {
                    if (dom_string_byte_length(code) > 0) {
                        duk_push_lstring(ctx, dom_string_data(code), dom_string_byte_length(code));
                        if (duk_peval(ctx) != 0) {
                            log_append("[JS ERROR] ", 11);
                            const char *e = duk_safe_to_string(ctx, -1);
                            log_append(e, (int) strlen(e));
                            log_append("\n", 1);
                        }
                        duk_pop(ctx);
                        ran++;
                    }
                    dom_string_unref(code);
                }
            }
            dom_node_unref(s);
        }
    }
    if (tag) dom_string_unref(tag);

    duk_destroy_heap(ctx);
    g_doc = 0; g_log = 0;
    return ran;
}
