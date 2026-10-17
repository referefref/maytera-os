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
#include <maytera-dom-limits.h>	/* MAYTERA_DOM_MAX_DEPTH (#245) */
/* #275 jsdom: mono_us() / SYS_MONO_US and sys_time(). The clock a script sees
 * is the ONLY way a page can time itself, and this port had nothing better than
 * whole seconds (see the performance.now comment below). syscall.h is safe to
 * include here: it is header-only inline wrappers and it does not define any of
 * the names duktape/shim owns (verified by building this file with and without
 * it, duk_dom.o md5 unchanged by the include alone). */
#include <syscall.h>

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

/* #245 deepdom: how deep `n` sits, counted no further than the cap.
 *
 * The walk is bounded, so this costs at most MAYTERA_DOM_MAX_DEPTH parent hops
 * however deep the tree really is. That matters: the whole point of the change
 * this belongs to is that no per-insertion step may be O(tree depth), or N
 * appendChild calls cost O(N^2) again. A bounded walk is O(1) in N.
 *
 * It also costs the node's ACTUAL depth, not the cap, so the ordinary case of
 * script appending a few levels below body pays a few hops. Deliberately NOT
 * memoised: a one-entry (node -> depth) cache would make the chained case O(1)
 * but a freed node's address can be reused by a new node at a different depth,
 * and a stale hit that under-reports the depth hands back exactly the unbounded
 * nesting this is here to prevent. A 255-hop ceiling is cheap; an unsound cap
 * is not. */
static int node_depth_capped(dom_node *n) {
    int d = 0;
    dom_node *cur = n;
    if (!cur) return 0;
    dom_node_ref(cur);
    while (cur && d < MAYTERA_DOM_MAX_DEPTH) {
        dom_node *p = 0;
        if (dom_node_get_parent_node(cur, &p) != DOM_NO_ERR) p = 0;
        dom_node_unref(cur);
        cur = p;
        if (!cur) break;
        d++;
    }
    if (cur) dom_node_unref(cur);
    return d;
}

static duk_ret_t m_appendChild(duk_context *ctx) {
    dom_node *parent = this_node(ctx), *child = arg_node(ctx, 0), *res = 0;
    /* #245 deepdom: refuse to nest past the one depth cap the engine honours.
     *
     * The HTML parser is clamped at MAYTERA_DOM_MAX_DEPTH (libhubbub
     * insert_element), but script reaches libdom directly and would otherwise
     * build a tree of unbounded depth. That is not merely unrenderable (the
     * layout walk drops everything below the same cap and getElementById
     * refuses to descend past it): libdom destroys a tree by RECURSING through
     * _dom_node_finalise, so a deep enough tree faults the process on teardown.
     * MEASURED with a 2 MB stack, the size of the Ring 3 user stack: a
     * 32000-deep tree segfaults on destruction, a 16000-deep one does not.
     * Nothing above closes that, because it is a stack bomb rather than a
     * complexity bug, so the cap is load-bearing and not just defence in depth.
     *
     * Failure mode matches the parser's: the append is ignored and the child is
     * returned, so a script that chains `cur = cur.appendChild(e)` keeps
     * running on a node that is simply not in the document. */
    if (parent && child && node_depth_capped(parent) >= MAYTERA_DOM_MAX_DEPTH - 1) {
        duk_dup(ctx, 0);
        return 1;
    }
    if (parent && child) dom_node_append_child(parent, child, &res);
    duk_dup(ctx, 0);            /* DOM appendChild returns the appended child */
    return 1;
}

/* ---- document ---- */

/* #245 dosproof: getElementById walks the FULL DOM tree, which libhubbub builds
 * with no nesting cap (its open-element stack grows unboundedly in 128-entry
 * chunks), so a page of N nested div elements makes find_by_id recurse N deep.
 * At a few tens of thousands of levels that overflows the 2 MB Ring-3 user
 * stack and faults the browser process (MEASURED). Cap the recursion, mirroring
 * layout's WALK_MAX_DEPTH (256, layout.c): content nested deeper than that is
 * DROPPED by the layout walk and never rendered, so an id below that depth could
 * not be seen on the page anyway, and refusing to descend past it loses nothing
 * a user could observe while removing the unbounded-recursion DoS. Blast radius
 * is one Ring-3 process either way; this turns a crash into a correct not-found. */
#define FIND_BY_ID_MAX_DEPTH MAYTERA_DOM_MAX_DEPTH

static dom_node *find_by_id_d(dom_node *node, const char *id, int depth) {
    if (depth >= FIND_BY_ID_MAX_DEPTH) return 0;
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
            dom_node *f = find_by_id_d(child, id, depth + 1);
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
        found = find_by_id_d((dom_node *) root, id, 0);
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

/* ---- the clock a page can actually measure with (#275 jsdom) ----
 *
 * WHAT WAS WRONG, and it is NOT that these APIs were missing. Duktape 2.7 ships
 * both Date.now() and a performance.now() (DUK_USE_PERFORMANCE_BUILTIN, on in
 * this duk_config.h; verified by building this file WITHOUT the code below and
 * finding typeof performance === "object" with a "now" property). What is wrong
 * is their RESOLUTION. duk_config.h autodetects the platform, and
 * duktape/build.sh deliberately undefines __linux__ and friends (see the long
 * comment there), so this port takes the Generic fallback and lands on
 * DUK_USE_DATE_NOW_TIME, which is
 *
 *     time(NULL) * 1000.0
 *
 * i.e. WHOLE SECONDS multiplied by a thousand. performance.now() is worse off
 * than Date.now(), because its own provider (DUK_USE_GET_MONOTONIC_TIME) has no
 * platform branch here either and duk_config.h falls it back to the same place.
 * So a page that times itself, animates on a clock, or rate-limits anything saw
 * a clock that sat still for a second and then jumped by 1000. MEASURED in the
 * OS: 30000 back-to-back Date.now() calls returned 1 distinct value.
 *
 * It is also why #245's own in-page scaling measurements had to use batches of
 * 100000 nodes and still called the result "about 5 s, so 4-8 s": one second
 * was the smallest number the page could report.
 *
 * WHAT THIS DOES. SYS_MONO_US is TSC-backed microseconds since boot and is the
 * clock the rest of userland already uses for anything timed (syscall.h says
 * why, and blame.md's timer-ticks-is-not-a-wall-clock entry says why the
 * tick-derived clocks are not usable for this). So:
 *
 *   performance.now()  milliseconds as a FRACTIONAL double, monotonic from the
 *                      first call in this context. This is what a page should
 *                      time with and it is now microsecond-resolution.
 *   Date.now()         epoch milliseconds, FLOORED TO A WHOLE MILLISECOND. The
 *                      spec says Date.now() is an integral millisecond count
 *                      and every real engine returns one, so this does NOT hand
 *                      back a fraction; the gain is that it now advances every
 *                      millisecond instead of every thousand.
 *
 * NOT FIXED, STATED PLAINLY: `new Date()` and the Date constructor still go
 * through Duktape's own DUK_USE_DATE_GET_NOW, so `new Date().getTime()` is still
 * second-granular. The complete fix is to define DUK_USE_DATE_GET_NOW for the
 * freestanding build, and that CANNOT be done with a -D: duk_config.h does an
 * unconditional `#undef DUK_USE_DATE_GET_NOW` in its generated defaults block
 * before the "external provider already defined" check runs (duk_config.h:2930
 * vs :3119), so a command-line definition is erased. Doing it properly means a
 * patch to the vendored duk_config.h and a drift-check marker for it, which is a
 * separate change. Date.now() and performance.now() are what a page TIMES with;
 * the constructor is what a page FORMATS with.
 *
 * The anchor is taken ONCE per context, so the two clocks cannot disagree with
 * each other within one page and a hostile page cannot make the browser issue a
 * SYS_TIME per loop iteration. */
static unsigned long long g_mono_base = 0;   /* mono_us at context setup */
static double g_epoch_ms_base = 0.0;         /* epoch ms at that same instant */

/* Microseconds elapsed since this context's clock was anchored. Monotonic, so
 * the subtraction cannot go backwards; still written defensively, because a
 * hostile page gets to call this as often as it likes and must never be able to
 * make it produce a negative number or a NaN. */
static double since_anchor_ms(void) {
    unsigned long long now = mono_us();
    unsigned long long d = (now >= g_mono_base) ? (now - g_mono_base) : 0ULL;
    return (double) d / 1000.0;
}

static duk_ret_t perf_now(duk_context *ctx) {
    duk_push_number(ctx, since_anchor_ms());
    return 1;
}

static duk_ret_t date_now_ms(duk_context *ctx) {
    double ms = g_epoch_ms_base + since_anchor_ms();
    /* Floor, not round: Date.now() is an integral millisecond count. */
    duk_push_number(ctx, (double) (long long) ms);
    return 1;
}

static void setup_clock(duk_context *ctx) {
    g_mono_base = mono_us();
    /* sys_time() is seconds since the UNIX epoch (syscall.h, #113). */
    g_epoch_ms_base = (double) sys_time() * 1000.0;

    duk_push_global_object(ctx);                        /* [global] */
    duk_push_object(ctx);                               /* [global perf] */
    duk_push_c_function(ctx, perf_now, 0);
    duk_put_prop_string(ctx, -2, "now");
    duk_put_prop_string(ctx, -2, "performance");        /* [global] */

    /* Replace the built-in Date.now. If the Date object is somehow not there,
     * the page simply keeps the second-granular built-in rather than this
     * failing the page load. */
    if (duk_get_prop_string(ctx, -1, "Date") && duk_is_object(ctx, -1)) {
        duk_push_c_function(ctx, date_now_ms, 0);
        duk_put_prop_string(ctx, -2, "now");
    }
    duk_pop(ctx);                                       /* [global] */
    duk_pop(ctx);                                       /* [] */
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
    setup_clock(ctx);           /* #275 jsdom: performance.now + sub-ms Date.now */

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
