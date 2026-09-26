// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// flow.c - Maytera Flow workflow runner core (Milestone 1).
//
// Platform-neutral: the YAML parse (via the ported libyaml event API), the
// typed-edge marshaller, the node-executor registry and the M1 node handlers
// live here so the SAME code runs under the MayteraOS binary (main.c) and the
// host gcc unit test (hosttest.c). The only platform-specific operations are
// behind the flow_plat_* seam in flow.h: file writing (capability gated on
// MayteraOS) and the LLM call.
//
// The parser is a small state machine over libyaml's event stream. It reads
// exactly the schema documented in docs/WORKFLOW_YAML_SCHEMA.md and skips any
// key it does not recognise, so an unknown-but-valid key is forward compatible
// rather than fatal.

#ifdef FLOWRUN_HOSTTEST
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <fcntl.h>
#  include <unistd.h>
#else
#  include "stdio.h"
#  include "stdlib.h"
#  include "string.h"
#  include "fcntl.h"
#  include "unistd.h"
#endif

#include "yaml.h"
#include "flow.h"
// #469: the ONE definition of every keycode, shared with the compositor
// and every app. Pure #defines, so the host unit test compiles it too.
// A guessed keycode ships a feature that renders and does nothing.
#include "../../libc/keys.h"

// ---- small bounded string copy (portable across host + MayteraOS) ---------
static void scpy(char *dst, const char *src, int cap) {
    if (cap <= 0) return;
    int i = 0;
    if (src) { for (; src[i] && i < cap - 1; i++) dst[i] = src[i]; }
    dst[i] = 0;
}

static int seq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

// ---- #469 AI-VISION forward decls -----------------------------------------
// The vision budget and the small param helper are used by h_llm, which is
// defined well above the capture/input handlers that also use them. One
// definition each, declared here so the order of the handlers stays readable.
static int s_vision_calls = 0;   // image-carrying model calls THIS run
static int param_int(const flow_node_t *n, const char *key, int dflt);

// ---- value helpers --------------------------------------------------------
void flow_value_set_str(flow_value_t *v, const char *s) {
    v->type = FV_STRING;
    scpy(v->s, s, FLOW_VAL_MAX);
    v->i = 0;
}

static void flow_value_set_int(flow_value_t *v, long n) {
    v->type = FV_INT;
    v->i = n;
    snprintf(v->s, FLOW_VAL_MAX, "%ld", n);
}

static void flow_value_set_bool(flow_value_t *v, int b) {
    v->type = FV_BOOL;
    v->i = b ? 1 : 0;
    scpy(v->s, b ? "true" : "false", FLOW_VAL_MAX);
}

void flow_value_as_str(const flow_value_t *v, char *out, int cap) {
    // M1 coercion rule: every value coerces to its text form, which is kept
    // authoritative at set time, so this is just a bounded copy.
    scpy(out, v ? v->s : "", cap);
}

// ---- node/graph accessors -------------------------------------------------
const char *flow_param(const flow_node_t *n, const char *key) {
    for (int i = 0; i < n->nparams; i++)
        if (seq(n->params[i].key, key)) return n->params[i].val;
    return 0;
}

static flow_node_t *flow_find_node(flow_graph_t *g, const char *id) {
    for (int i = 0; i < g->nnodes; i++)
        if (seq(g->nodes[i].id, id)) return &g->nodes[i];
    return 0;
}

void flow_set_output(flow_node_t *n, const char *port, const flow_value_t *v) {
    for (int i = 0; i < n->noutputs; i++) {
        if (seq(n->outputs[i].port, port)) { n->outputs[i].val = *v; return; }
    }
    if (n->noutputs >= FLOW_MAX_OUTPUTS) return;
    scpy(n->outputs[n->noutputs].port, port, FLOW_PORT_MAX);
    n->outputs[n->noutputs].val = *v;
    n->noutputs++;
}

static int node_output(const flow_node_t *n, const char *port, flow_value_t *out) {
    for (int i = 0; i < n->noutputs; i++)
        if (seq(n->outputs[i].port, port)) { *out = n->outputs[i].val; return 1; }
    return 0;
}

int flow_input(flow_graph_t *g, const flow_node_t *node, const char *port,
               flow_value_t *out) {
    for (int i = 0; i < g->nedges; i++) {
        flow_edge_t *e = &g->edges[i];
        if (seq(e->to_node, node->id) && seq(e->to_port, port)) {
            flow_node_t *src = flow_find_node(g, e->from_node);
            if (src && node_output(src, e->from_port, out)) return 1;
            return 0;
        }
    }
    return 0;
}

// ---- loop-body membership (M2) --------------------------------------------
// The model has no dedicated container pointer; loop membership REUSES the
// existing scalar-param mechanism: a body node carries `loop: <loopNodeId>`
// pointing at the foreach/loop node that owns it. That is "body nodes reference
// their container" with zero parser or struct change, and it is what the visual
// editor already emits per node.
static int idx_of_id(flow_graph_t *g, const char *id) {
    for (int i = 0; i < g->nnodes; i++)
        if (seq(g->nodes[i].id, id)) return i;
    return -1;
}

// A "foreach" (editor vocabulary) or "loop" (runner/docs vocabulary) node is a
// for-each region container. Both names map to the same executor.
static int is_loop_type(const char *t) { return seq(t, "foreach") || seq(t, "loop"); }

// Every node type that owns a body sub-region through the SAME `loop` membership
// mechanism: the for-each region (foreach/loop), the agent-promise loop (agent),
// and the async sub-loop (subloop). All three reuse run_loop_body() to run their
// body, so all three are valid `loop` targets for a body node.
static int is_container_type(const char *t) {
    return is_loop_type(t) || seq(t, "agent") || seq(t, "subloop");
}

// The index of the container node that owns body node i, or -1 if i is not a
// body member. Guards against a self-reference and a `loop` param that does not
// name an actual container (foreach/loop/agent/subloop).
static int loop_container_idx(flow_graph_t *g, int i) {
    const char *lp = flow_param(&g->nodes[i], "loop");
    if (!lp || !lp[0]) return -1;
    int L = idx_of_id(g, lp);
    if (L < 0 || L == i) return -1;
    if (!is_container_type(g->nodes[L].type)) return -1;
    return L;
}

// ===========================================================================
// YAML parse (libyaml event stream)
// ===========================================================================
typedef struct {
    yaml_parser_t parser;
    yaml_event_t  ev;
    int           evtype;   // last event type, or -1 on error
    int           err;
} yctx_t;

static int yp_advance(yctx_t *y) {
    yaml_event_delete(&y->ev);
    // Sticky terminal: once the stream has ended or errored, NEVER call libyaml
    // again. This is what stops a malformed/TRUNCATED document (e.g. an
    // unterminated flow collection `nodes: [ { id: a`) from spinning the parser
    // forever: after STREAM_END, libyaml returns YAML_NO_EVENT (0) on every
    // further parse, which the container-parsing loops below (that wait for a
    // SEQUENCE_END/MAPPING_END that a truncated document never produces) would
    // otherwise treat as "keep going". flowgen: an LLM readily emits truncated YAML
    // and aichat validates it in its own event loop, so a hang here is a freeze.
    if (y->err) { y->evtype = -1; return -1; }
    if (!yaml_parser_parse(&y->parser, &y->ev)) { y->err = 1; y->evtype = -1; return -1; }
    y->evtype = (int)y->ev.type;
    // A YAML_NO_EVENT (0) means there is nothing more to read: terminal.
    if (y->evtype == YAML_NO_EVENT) { y->err = 1; y->evtype = -1; return -1; }
    // STREAM_END is returned ONCE (flow_parse_bytes checks for it explicitly for
    // the empty-document case), but any advance past it is terminal.
    if (y->evtype == YAML_STREAM_END_EVENT) y->err = 1;
    return y->evtype;
}

static void yp_scalar(yctx_t *y, char *dst, int cap) {
    const char *s = (const char *)y->ev.data.scalar.value;
    scpy(dst, s ? s : "", cap);
}

// current event is the first event of a value; consume the whole value.
static void yp_skip(yctx_t *y) {
    if (y->evtype == YAML_MAPPING_START_EVENT ||
        y->evtype == YAML_SEQUENCE_START_EVENT) {
        int depth = 1;
        while (depth > 0) {
            int t = yp_advance(y);
            if (t < 0) return;
            if (t == YAML_MAPPING_START_EVENT || t == YAML_SEQUENCE_START_EVENT) depth++;
            else if (t == YAML_MAPPING_END_EVENT || t == YAML_SEQUENCE_END_EVENT) depth--;
        }
    }
    // SCALAR / ALIAS: nothing further to consume.
}

static int is_loop_field(const char *k) {
    return seq(k, "objective") || seq(k, "success") || seq(k, "max_iters") ||
           seq(k, "max_minutes") || seq(k, "max_tokens") || seq(k, "mode") ||
           seq(k, "every_ms");
}

// current event is MAPPING_START of a node's params map.
static void parse_params(yctx_t *y, flow_node_t *n) {
    for (;;) {
        int t = yp_advance(y);
        if (t == YAML_MAPPING_END_EVENT || t < 0) return;
        if (t != YAML_SCALAR_EVENT) { yp_skip(y); continue; }
        char key[FLOW_KEY_MAX];
        yp_scalar(y, key, sizeof(key));
        t = yp_advance(y);
        if (t == YAML_SCALAR_EVENT) {
            if (n->nparams < FLOW_MAX_PARAMS) {
                scpy(n->params[n->nparams].key, key, FLOW_KEY_MAX);
                yp_scalar(y, n->params[n->nparams].val, FLOW_VAL_MAX);
                n->nparams++;
            }
        } else {
            yp_skip(y);   // params value must be scalar in M1; ignore structured values
        }
    }
}

// current event is MAPPING_START of a node.
static void parse_node(yctx_t *y, flow_node_t *n) {
    for (;;) {
        int t = yp_advance(y);
        if (t == YAML_MAPPING_END_EVENT || t < 0) return;
        if (t != YAML_SCALAR_EVENT) { yp_skip(y); continue; }
        char key[FLOW_KEY_MAX];
        yp_scalar(y, key, sizeof(key));
        t = yp_advance(y);   // value's first event
        if (seq(key, "id") && t == YAML_SCALAR_EVENT) {
            yp_scalar(y, n->id, FLOW_ID_MAX);
        } else if (seq(key, "type") && t == YAML_SCALAR_EVENT) {
            yp_scalar(y, n->type, FLOW_TYPE_MAX);
        } else if (seq(key, "params") && t == YAML_MAPPING_START_EVENT) {
            parse_params(y, n);
        } else if (is_loop_field(key) && t == YAML_SCALAR_EVENT) {
            char v[FLOW_VAL_MAX];
            yp_scalar(y, v, sizeof(v));
            n->has_loop = 1;
            if (seq(key, "objective"))        scpy(n->objective, v, FLOW_VAL_MAX);
            else if (seq(key, "success"))     scpy(n->success, v, FLOW_VAL_MAX);
            else if (seq(key, "max_iters"))   n->max_iters = atoi(v);
            else if (seq(key, "max_minutes")) n->max_minutes = atoi(v);
            else if (seq(key, "max_tokens"))  n->max_tokens = atol(v);
            else if (seq(key, "mode"))        scpy(n->mode, v, FLOW_TYPE_MAX);
            else if (seq(key, "every_ms"))    n->every_ms = atoi(v);
        } else {
            yp_skip(y);
        }
    }
}

// current event is SEQUENCE_START of a [node, port] pair. Reads up to two
// scalars, ignores extras, consumes through SEQUENCE_END.
static void parse_pair(yctx_t *y, char *node, int ncap, char *port, int pcap) {
    int idx = 0;
    for (;;) {
        int t = yp_advance(y);
        if (t == YAML_SEQUENCE_END_EVENT || t < 0) return;
        if (t == YAML_SCALAR_EVENT) {
            if (idx == 0)      yp_scalar(y, node, ncap);
            else if (idx == 1) yp_scalar(y, port, pcap);
            idx++;
        } else {
            yp_skip(y);
        }
    }
}

// current event is MAPPING_START of an edge.
static void parse_edge(yctx_t *y, flow_edge_t *e) {
    // Defaults if a side is given as a bare node scalar.
    scpy(e->from_port, "out", FLOW_PORT_MAX);
    scpy(e->to_port, "in", FLOW_PORT_MAX);
    for (;;) {
        int t = yp_advance(y);
        if (t == YAML_MAPPING_END_EVENT || t < 0) return;
        if (t != YAML_SCALAR_EVENT) { yp_skip(y); continue; }
        char key[FLOW_KEY_MAX];
        yp_scalar(y, key, sizeof(key));
        t = yp_advance(y);
        if (seq(key, "from")) {
            if (t == YAML_SEQUENCE_START_EVENT)
                parse_pair(y, e->from_node, FLOW_ID_MAX, e->from_port, FLOW_PORT_MAX);
            else if (t == YAML_SCALAR_EVENT)
                yp_scalar(y, e->from_node, FLOW_ID_MAX);
            else yp_skip(y);
        } else if (seq(key, "to")) {
            if (t == YAML_SEQUENCE_START_EVENT)
                parse_pair(y, e->to_node, FLOW_ID_MAX, e->to_port, FLOW_PORT_MAX);
            else if (t == YAML_SCALAR_EVENT)
                yp_scalar(y, e->to_node, FLOW_ID_MAX);
            else yp_skip(y);
        } else {
            yp_skip(y);
        }
    }
}

// current event is SEQUENCE_START of the top-level nodes list.
static void parse_nodes(yctx_t *y, flow_graph_t *g) {
    for (;;) {
        int t = yp_advance(y);
        if (t == YAML_SEQUENCE_END_EVENT || t < 0) return;
        if (t != YAML_MAPPING_START_EVENT) { yp_skip(y); continue; }
        if (g->nnodes < FLOW_MAX_NODES) {
            parse_node(y, &g->nodes[g->nnodes]);
            g->nnodes++;
        } else {
            yp_skip(y);   // over the node cap: consume and drop
        }
    }
}

static void parse_edges(yctx_t *y, flow_graph_t *g) {
    for (;;) {
        int t = yp_advance(y);
        if (t == YAML_SEQUENCE_END_EVENT || t < 0) return;
        if (t != YAML_MAPPING_START_EVENT) { yp_skip(y); continue; }
        if (g->nedges < FLOW_MAX_EDGES) {
            parse_edge(y, &g->edges[g->nedges]);
            g->nedges++;
        } else {
            yp_skip(y);
        }
    }
}

int flow_parse_bytes(const char *buf, int len, flow_graph_t *g,
                     char *err, int errcap) {
    memset(g, 0, sizeof(*g));
    yctx_t y;
    memset(&y, 0, sizeof(y));
    if (!yaml_parser_initialize(&y.parser)) {
        scpy(err, "libyaml: parser init failed", errcap);
        return 1;
    }
    yaml_parser_set_input_string(&y.parser, (const unsigned char *)buf, (size_t)len);

    int rc = 0;
    if (yp_advance(&y) != YAML_STREAM_START_EVENT) { scpy(err, "expected stream start", errcap); rc = 1; goto done; }
    int t = yp_advance(&y);
    if (t == YAML_STREAM_END_EVENT) { scpy(err, "empty document", errcap); rc = 1; goto done; }
    if (t != YAML_DOCUMENT_START_EVENT) { scpy(err, "expected document start", errcap); rc = 1; goto done; }
    if (yp_advance(&y) != YAML_MAPPING_START_EVENT) { scpy(err, "top level must be a mapping", errcap); rc = 1; goto done; }

    for (;;) {
        t = yp_advance(&y);
        if (t == YAML_MAPPING_END_EVENT || t < 0) break;
        if (t != YAML_SCALAR_EVENT) { yp_skip(&y); continue; }
        char key[FLOW_KEY_MAX];
        yp_scalar(&y, key, sizeof(key));
        t = yp_advance(&y);
        if (seq(key, "name") && t == YAML_SCALAR_EVENT) {
            yp_scalar(&y, g->name, FLOW_ID_MAX);
        } else if (seq(key, "nodes") && t == YAML_SEQUENCE_START_EVENT) {
            parse_nodes(&y, g);
        } else if (seq(key, "edges") && t == YAML_SEQUENCE_START_EVENT) {
            parse_edges(&y, g);
        } else {
            yp_skip(&y);
        }
    }

    if (y.err) { scpy(err, "libyaml: parse error (malformed YAML)", errcap); rc = 1; goto done; }
    if (g->nnodes == 0) { scpy(err, "workflow has no nodes", errcap); rc = 1; goto done; }

done:
    yaml_event_delete(&y.ev);
    yaml_parser_delete(&y.parser);
    return rc;
}

int flow_parse_file(const char *path, flow_graph_t *g, char *err, int errcap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err, errcap, "cannot open %s", path); return 1; }
    static char buf[65536];
    int total = 0, k;
    while (total < (int)sizeof(buf) - 1 &&
           (k = (int)read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
        total += k;
    close(fd);
    buf[total] = 0;
    if (total == 0) { snprintf(err, errcap, "empty file %s", path); return 1; }
    return flow_parse_bytes(buf, total, g, err, errcap);
}

// ===========================================================================
// Node-executor registry and M1 handlers
// ===========================================================================
typedef int (*flow_handler_fn)(flow_ctx_t *ctx, flow_node_t *n);

// Forward declaration: the loop executor and its body sub-scheduler dispatch
// body nodes through the SAME registry the top-level executor uses.
static flow_handler_fn lookup_handler(const char *type);

static void upcase(char *s)  { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }
static void downcase(char *s){ for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

static int h_trigger(flow_ctx_t *ctx, flow_node_t *n) {
    const char *kind = flow_param(n, "kind");
    if (!kind) kind = "manual";
    if (seq(kind, "manual")) {
        flow_value_t v; flow_value_set_bool(&v, 1);
        flow_set_output(n, "out", &v);
        fprintf(stderr, "flowrun: trigger '%s' kind=manual fired\n", n->id);
        return 0;
    }
    if (seq(kind, "cron")) {
        // M1 does NOT run a scheduler. A cron trigger is registered later via
        // the kernel cron_register_callback path; a direct headless run does
        // not fire it, so it produces no output and downstream nodes see no
        // input. This is deliberate: we do not fake a scheduler.
        fprintf(stderr, "flowrun: trigger '%s' kind=cron registered for scheduler "
                        "(cron_register_callback); not fired on direct run\n", n->id);
        return 0;
    }
    snprintf(ctx->err, FLOW_ERR_MAX, "trigger '%s': unknown kind '%s'", n->id, kind);
    return -1;
}

static int h_transform(flow_ctx_t *ctx, flow_node_t *n) {
    const char *op = flow_param(n, "op");
    if (!op) { snprintf(ctx->err, FLOW_ERR_MAX, "transform '%s': missing 'op'", n->id); return -1; }

    flow_value_t out; out.type = FV_STRING; out.s[0] = 0; out.i = 0;

    if (seq(op, "constant")) {
        const char *txt = flow_param(n, "text");
        if (!txt) txt = flow_param(n, "value");
        flow_value_set_str(&out, txt ? txt : "");
    } else if (seq(op, "upper") || seq(op, "lower")) {
        flow_value_t in;
        char s[FLOW_VAL_MAX] = {0};
        if (flow_input(ctx->g, n, "in", &in)) flow_value_as_str(&in, s, sizeof(s));
        else { const char *t = flow_param(n, "text"); scpy(s, t ? t : "", sizeof(s)); }
        if (seq(op, "upper")) upcase(s); else downcase(s);
        flow_value_set_str(&out, s);
    } else if (seq(op, "concat")) {
        char s[FLOW_VAL_MAX] = {0};
        flow_value_t a, b;
        if (flow_input(ctx->g, n, "in", &a))  flow_value_as_str(&a, s, sizeof(s));
        const char *sep = flow_param(n, "sep");
        if (sep && s[0]) { int l = (int)strlen(s); scpy(s + l, sep, (int)sizeof(s) - l); }
        char tail[FLOW_VAL_MAX] = {0};
        if (flow_input(ctx->g, n, "in2", &b)) flow_value_as_str(&b, tail, sizeof(tail));
        else { const char *t = flow_param(n, "text"); scpy(tail, t ? t : "", sizeof(tail)); }
        int l = (int)strlen(s); scpy(s + l, tail, (int)sizeof(s) - l);
        flow_value_set_str(&out, s);
    } else if (seq(op, "gte") || seq(op, "gt") || seq(op, "lte") || seq(op, "lt")) {
        // Integer threshold ops. lhs comes from input port "in" (else param
        // "text"), rhs from param "value". Output is a bool. These let a purely
        // deterministic agent/subloop body decide goal_met from a measured or
        // iteration-index signal, with no network in the loop under test.
        flow_value_t in; long lhs = 0;
        if (flow_input(ctx->g, n, "in", &in))
            lhs = (in.type == FV_INT || in.type == FV_BOOL) ? in.i : atol(in.s);
        else { const char *t = flow_param(n, "text"); lhs = t ? atol(t) : 0; }
        const char *vs = flow_param(n, "value");
        long rhs = vs ? atol(vs) : 0;
        int r = seq(op, "gte") ? (lhs >= rhs) :
                seq(op, "gt")  ? (lhs >  rhs) :
                seq(op, "lte") ? (lhs <= rhs) : (lhs < rhs);
        flow_value_set_bool(&out, r);
    } else {
        snprintf(ctx->err, FLOW_ERR_MAX, "transform '%s': unknown op '%s'", n->id, op);
        return -1;
    }

    flow_set_output(n, "out", &out);
    fprintf(stderr, "flowrun: transform '%s' op=%s -> \"%s\"\n", n->id, op, out.s);
    return 0;
}

static int h_file(flow_ctx_t *ctx, flow_node_t *n) {
    const char *op = flow_param(n, "op");
    if (!op) op = "write";
    int append;
    if (seq(op, "write"))       append = 0;
    else if (seq(op, "append")) append = 1;
    else { snprintf(ctx->err, FLOW_ERR_MAX, "file '%s': op must be write|append, got '%s'", n->id, op); return -1; }

    const char *path = flow_param(n, "path");
    if (!path || !path[0]) { snprintf(ctx->err, FLOW_ERR_MAX, "file '%s': missing 'path'", n->id); return -1; }

    char text[FLOW_VAL_MAX] = {0};
    flow_value_t in;
    if (flow_input(ctx->g, n, "text", &in))    flow_value_as_str(&in, text, sizeof(text));
    else if (flow_input(ctx->g, n, "in", &in)) flow_value_as_str(&in, text, sizeof(text));
    else { const char *t = flow_param(n, "text"); scpy(text, t ? t : "", sizeof(text)); }

    int bytes = flow_plat_write_file(path, text, (int)strlen(text), append,
                                     ctx->err, FLOW_ERR_MAX);
    if (bytes < 0) return -1;   // err already set by the platform layer
    ctx->total_written += bytes;

    flow_value_t out; flow_value_set_int(&out, bytes);
    flow_set_output(n, "out", &out);
    fprintf(stderr, "flowrun: file '%s' %s %d byte(s) -> %s\n",
            n->id, append ? "append" : "write", bytes, path);
    return 0;
}

// Substitute {portName} placeholders in `tmpl` with the text arriving on that
// input port of node n. An unresolved placeholder is left empty.
static void subst_placeholders(flow_ctx_t *ctx, flow_node_t *n,
                               const char *tmpl, char *out, int cap) {
    int o = 0;
    for (int i = 0; tmpl[i] && o < cap - 1; i++) {
        if (tmpl[i] == '{') {
            char port[FLOW_PORT_MAX]; int p = 0;
            int j = i + 1;
            while (tmpl[j] && tmpl[j] != '}' && p < FLOW_PORT_MAX - 1) port[p++] = tmpl[j++];
            port[p] = 0;
            if (tmpl[j] == '}') {
                flow_value_t v; char sv[FLOW_VAL_MAX] = {0};
                if (flow_input(ctx->g, n, port, &v)) flow_value_as_str(&v, sv, sizeof(sv));
                for (int k = 0; sv[k] && o < cap - 1; k++) out[o++] = sv[k];
                i = j;
                continue;
            }
        }
        out[o++] = tmpl[i];
    }
    out[o] = 0;
}

static int h_llm(flow_ctx_t *ctx, flow_node_t *n) {
    const char *system = flow_param(n, "system");
    const char *ptmpl  = flow_param(n, "prompt");
    if (!ptmpl) { snprintf(ctx->err, FLOW_ERR_MAX, "llm '%s': missing 'prompt'", n->id); return -1; }

    char prompt[FLOW_VAL_MAX];
    subst_placeholders(ctx, n, ptmpl, prompt, sizeof(prompt));

    // #469 AI-VISION: an optional IMAGE, from the "image" input port (what a
    // capture node feeds it) or params.image. With one, this is a multimodal
    // completion; without one it is the text completion it always was. The
    // goal_met parsing below is shared, so the same decide node reads a picture
    // or reads text and reports the same verdict.
    char image[FLOW_VAL_MAX] = {0};
    flow_value_t imv;
    if (flow_input(ctx->g, n, "image", &imv)) flow_value_as_str(&imv, image, sizeof(image));
    else { const char *ip = flow_param(n, "image"); scpy(image, ip ? ip : "", sizeof(image)); }

    static char reply[8192];
    reply[0] = 0;
    int rc;
    if (image[0]) {
        // BUDGET, checked BEFORE the call, not after. `max_calls` is this
        // node's own ceiling across the whole run (it survives loop
        // iterations); FLOW_MAX_VISION_CALLS is the runner's hard ceiling that
        // no graph can raise. Exceeding either is a FAILURE, not a quiet stop:
        // a loop that silently stopped perceiving would keep acting blind.
        int max_calls = param_int(n, "max_calls", 0);
        if (max_calls > 0 && n->calls >= max_calls) {
            snprintf(ctx->err, FLOW_ERR_MAX,
                     "llm '%s': vision budget exhausted (max_calls=%d)", n->id, max_calls);
            return -1;
        }
        if (s_vision_calls >= FLOW_MAX_VISION_CALLS) {
            snprintf(ctx->err, FLOW_ERR_MAX,
                     "llm '%s': run-wide vision ceiling reached (%d calls)",
                     n->id, FLOW_MAX_VISION_CALLS);
            return -1;
        }
        n->calls++;
        s_vision_calls++;
        fprintf(stderr, "flowrun: llm '%s' VISION call %d (node) / %d (run) image=%s\n",
                n->id, n->calls, s_vision_calls, image);
        rc = flow_plat_llm_image(system, prompt, image, reply, (int)sizeof(reply),
                                 ctx->err, FLOW_ERR_MAX);
    } else {
        rc = flow_plat_llm(system, prompt, reply, (int)sizeof(reply), ctx->err, FLOW_ERR_MAX);
    }
    if (rc < 0) return -1;

    flow_value_t out; flow_value_set_str(&out, reply);
    flow_set_output(n, "out", &out);

    // Structured output: if the reply declares a goal_met verdict, expose it as
    // a bool on port "goal_met" so an agent body's decide (llm) node can wire it
    // back to the agent-promise loop. This is best-effort structure over the
    // free-text reply (find "goal_met" then the nearer of true/false); a decide
    // prompt should instruct the model to emit "goal_met: true|false". When the
    // reply declares nothing, no goal_met output is set (the agent then treats
    // the objective as unmet for that iteration).
    {
        static char low[8192];
        int i = 0;
        for (; reply[i] && i < (int)sizeof(low) - 1; i++) {
            char c = reply[i];
            low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        low[i] = 0;
        const char *gm = strstr(low, "goal_met");
        if (gm) {
            const char *t = strstr(gm, "true");
            const char *f = strstr(gm, "false");
            int have = 0, val = 0;
            if (t && (!f || t < f)) { val = 1; have = 1; }
            else if (f)             { val = 0; have = 1; }
            if (have) {
                flow_value_t gb; flow_value_set_bool(&gb, val);
                flow_set_output(n, "goal_met", &gb);
            }
        }
    }
    fprintf(stderr, "flowrun: llm '%s' produced %d byte(s)\n", n->id, (int)strlen(reply));
    return 0;
}

// ---- M2: app node -- dispatch to another app's tool contract --------------
// An app node's bind is "app.feature". The visual editor stores the two halves
// as params `app` and `feature`; a combined `bind: app.feature` param is also
// accepted. The handler marshals the node's input port(s) into the contract
// call's argv, dispatches through the flow_plat_app_invoke seam (main.c wraps
// contract_invoke() and applies the capability gate; hosttest.c stubs it), then
// parses the child's reply line back into the node's output ports. It NEVER
// fakes success: a non-zero child code or an "err ..." reply fails the node.
static int h_app(flow_ctx_t *ctx, flow_node_t *n) {
    // ---- resolve app + feature (the contract item name) --------------------
    char app[FLOW_TYPE_MAX] = {0};
    char feature[FLOW_PORT_MAX] = {0};
    const char *pa = flow_param(n, "app");
    const char *pf = flow_param(n, "feature");
    if (pa && pa[0] && pf && pf[0]) {
        scpy(app, pa, FLOW_TYPE_MAX);
        scpy(feature, pf, FLOW_PORT_MAX);
    } else {
        const char *bind = flow_param(n, "bind");
        if (!bind || !bind[0]) {
            snprintf(ctx->err, FLOW_ERR_MAX,
                     "app '%s': needs params app+feature (or bind: app.feature)", n->id);
            return -1;
        }
        const char *dot = strchr(bind, '.');
        if (!dot || dot == bind || !dot[1]) {
            snprintf(ctx->err, FLOW_ERR_MAX,
                     "app '%s': bind '%s' must be app.feature", n->id, bind);
            return -1;
        }
        int al = (int)(dot - bind);
        if (al > FLOW_TYPE_MAX - 1) al = FLOW_TYPE_MAX - 1;
        for (int i = 0; i < al; i++) app[i] = bind[i];
        app[al] = 0;
        scpy(feature, dot + 1, FLOW_PORT_MAX);
    }
    // The editor's placeholder catalog stores `feature` app-prefixed
    // ("files.write_text"); the contract item name is the bare tail. Strip a
    // leading "<app>." so both "value" and "convert.value" resolve correctly.
    {
        int al = (int)strlen(app);
        if ((int)strlen(feature) > al + 1 &&
            strncmp(feature, app, al) == 0 && feature[al] == '.') {
            char tail[FLOW_PORT_MAX];
            scpy(tail, feature + al + 1, FLOW_PORT_MAX);
            scpy(feature, tail, FLOW_PORT_MAX);
        }
    }

    const char *verb = flow_param(n, "verb");
    if (!verb || !verb[0]) verb = "call";

    // ---- marshal input ports -> argv (verb, item, args...) ----------------
    char av0[FLOW_VAL_MAX] = {0};   // primary argument, from an input port
    char *av[8];
    int ac = 0;
    av[ac++] = (char *)verb;
    av[ac++] = feature;
    flow_value_t in;
    if (flow_input(ctx->g, n, "arg", &in) || flow_input(ctx->g, n, "in", &in)) {
        flow_value_as_str(&in, av0, sizeof(av0));
        av[ac++] = av0;
    } else {
        const char *pav = flow_param(n, "arg");
        if (pav && pav[0]) { scpy(av0, pav, sizeof(av0)); av[ac++] = av0; }
    }
    // Optional extra positional args from params arg2/arg3 (point at the param
    // strings, which live in the node for the duration of the call).
    const char *a2 = flow_param(n, "arg2");
    if (a2 && a2[0] && ac < 7) av[ac++] = (char *)a2;
    const char *a3 = flow_param(n, "arg3");
    if (a3 && a3[0] && ac < 7) av[ac++] = (char *)a3;

    // ---- dispatch through the platform seam -------------------------------
    static char reply[1024];
    reply[0] = 0;
    int rc = flow_plat_app_invoke(app, ac, av, reply, (int)sizeof(reply));
    // trim a trailing newline the child's reply line carries.
    int rl = (int)strlen(reply);
    while (rl > 0 && (reply[rl - 1] == '\n' || reply[rl - 1] == '\r')) reply[--rl] = 0;

    if (rc != 0 || strncmp(reply, "err", 3) == 0) {
        snprintf(ctx->err, FLOW_ERR_MAX, "app '%s' (%s.%s): %s",
                 n->id, app, feature, reply[0] ? reply : "invoke failed");
        return -1;
    }

    // ---- parse the reply -> output ports (typed-edge marshaller) ----------
    // A "call" reply is "ok name=<f> result=<text>"; a "get" reply is
    // "ok name=<f> type=string value=<text>". Expose the extracted value on
    // "out" and the whole verbatim reply on "reply".
    char val[FLOW_VAL_MAX] = {0};
    const char *res = strstr(reply, "result=");
    if (res) {
        scpy(val, res + 7, sizeof(val));
    } else {
        const char *vp = strstr(reply, "value=");
        if (vp) scpy(val, vp + 6, sizeof(val));
        else    scpy(val, reply, sizeof(val));
    }
    flow_value_t out; flow_value_set_str(&out, val);
    flow_set_output(n, "out", &out);
    flow_value_t raw; flow_value_set_str(&raw, reply);
    flow_set_output(n, "reply", &raw);
    fprintf(stderr, "flowrun: app '%s' %s %s.%s -> \"%s\"\n",
            n->id, verb, app, feature, val);
    return 0;
}

// ---- edge/membership index cache (computed once per flow_execute() run) -----
// The loop-body scheduler (run_loop_body) is re-run once PER ITERATION of a
// foreach/agent/subloop, and it walks every edge twice per pass. Re-resolving an
// edge endpoint id (idx_of_id, O(V)) and a node's container (loop_container_idx,
// O(V)+param scan) on every one of those walks made a loop O(iters * E * V).
// The endpoint indices and container membership do NOT change during a run, so
// we resolve them ONCE at flow_execute() entry into these caches and the hot
// loops then do O(1) reads. The runner is single-threaded and every handler that
// reaches run_loop_body is dispatched from flow_execute() after the cache is
// filled, so file scope is safe and always current.
static int s_edge_from[FLOW_MAX_EDGES];   // node index of each edge's from-node, or -1
static int s_edge_to[FLOW_MAX_EDGES];     // node index of each edge's to-node, or -1
static int s_loop_of[FLOW_MAX_NODES];     // container node index owning node i, or -1

static void precompute_indices(flow_graph_t *g) {
    for (int e = 0; e < g->nedges; e++) {
        s_edge_from[e] = idx_of_id(g, g->edges[e].from_node);
        s_edge_to[e]   = idx_of_id(g, g->edges[e].to_node);
    }
    for (int i = 0; i < g->nnodes; i++)
        s_loop_of[i] = loop_container_idx(g, i);
}

// ---- M2: foreach/loop node -- run a body sub-region once per item ----------
// Run the body nodes of loop L in topological order for the CURRENT iteration,
// using only edges INTERNAL to the body (edges among body nodes). Edges from the
// loop node (item/index) or from outside are already satisfied when the loop
// runs, so they do not gate the body order; body nodes read them through
// flow_input at run time. Body outputs are reset each iteration so a node
// recomputes per item rather than reusing the previous item's value.
static int run_loop_body(flow_ctx_t *ctx, int L, const int *body, int nbody) {
    flow_graph_t *g = ctx->g;
    for (int b = 0; b < nbody; b++) {
        flow_node_t *n = &g->nodes[body[b]];
        n->noutputs = 0;
        n->executed = 0;
    }

    int indeg[FLOW_MAX_NODES];
    for (int b = 0; b < nbody; b++) indeg[body[b]] = 0;
    for (int e = 0; e < g->nedges; e++) {
        int ti = s_edge_to[e];
        int fi = s_edge_from[e];
        if (ti < 0 || fi < 0 || ti == fi) continue;
        if (s_loop_of[ti] != L) continue;   // to-node must be a body of L
        if (s_loop_of[fi] != L) continue;   // from-node must be a body of L
        indeg[ti]++;
    }

    int queue[FLOW_MAX_NODES], qh = 0, qt = 0;
    for (int b = 0; b < nbody; b++) if (indeg[body[b]] == 0) queue[qt++] = body[b];

    int processed = 0;
    while (qh < qt) {
        int idx = queue[qh++];
        flow_node_t *n = &g->nodes[idx];
        flow_handler_fn fn = lookup_handler(n->type);
        if (!fn) {
            snprintf(ctx->err, FLOW_ERR_MAX,
                     "unsupported node type: %s (loop body node '%s')",
                     n->type[0] ? n->type : "(none)", n->id);
            return -1;
        }
        if (fn(ctx, n) != 0) return -1;   // ctx->err set by the handler
        n->executed = 1;
        processed++;
        for (int e = 0; e < g->nedges; e++) {
            if (s_edge_from[e] != idx) continue;
            int ti = s_edge_to[e];
            if (ti < 0 || ti == idx) continue;
            if (s_loop_of[ti] != L) continue;
            if (--indeg[ti] == 0) queue[qt++] = ti;
        }
    }
    if (processed < nbody) {
        snprintf(ctx->err, FLOW_ERR_MAX,
                 "foreach body of '%s' has a cycle (not a DAG)", g->nodes[L].id);
        return -1;
    }
    return 0;
}

static int h_foreach(flow_ctx_t *ctx, flow_node_t *n) {
    flow_graph_t *g = ctx->g;
    int L = idx_of_id(g, n->id);
    if (L < 0) { snprintf(ctx->err, FLOW_ERR_MAX, "foreach '%s': not in graph", n->id); return -1; }

    // Separator that splits the items list AND joins the collected results.
    const char *sepp = flow_param(n, "sep");
    char sepc = (sepp && sepp[0]) ? sepp[0] : ',';

    // Items: from an inbound edge on port "items" (a list value), else the
    // `items` param. Empty / whitespace-only means zero iterations.
    char items[FLOW_VAL_MAX] = {0};
    flow_value_t iv;
    if (flow_input(g, n, "items", &iv)) flow_value_as_str(&iv, items, sizeof(items));
    else { const char *p = flow_param(n, "items"); scpy(items, p ? p : "", sizeof(items)); }

    // The body: every node whose `loop` param points at this loop node.
    int body[FLOW_MAX_NODES], nbody = 0;
    for (int i = 0; i < g->nnodes; i++)
        if (loop_container_idx(g, i) == L) body[nbody++] = i;

    // M2 runs SEQUENTIALLY. A `parallel` param is honored only as a documented
    // TODO: real concurrency is deferred (there is no fake parallelism).
    const char *par = flow_param(n, "parallel");
    if (par && (seq(par, "true") || seq(par, "1") || seq(par, "yes")))
        fprintf(stderr, "flowrun: foreach '%s': parallel requested but M2 runs "
                        "SEQUENTIALLY (parallel is a documented TODO)\n", n->id);

    // Iteration bound: the global cap, lowered (never raised) by max_iters.
    int cap = FLOW_LOOP_MAX_ITERS;
    if (n->max_iters > 0 && n->max_iters < cap) cap = n->max_iters;

    char results[FLOW_VAL_MAX]; results[0] = 0;
    int rlen = 0;
    int count = 0;

    int i = 0;
    while (items[i]) {
        while (items[i] == sepc) i++;              // skip separators
        if (!items[i]) break;
        int s = i;
        while (items[i] && items[i] != sepc) i++;
        int e = i;
        while (s < e && items[s] == ' ') s++;      // trim
        while (e > s && items[e - 1] == ' ') e--;
        if (e <= s) continue;                      // empty token after trim: skip

        if (count >= cap) {
            snprintf(ctx->err, FLOW_ERR_MAX,
                     "foreach '%s': item count exceeds cap %d", n->id, cap);
            return -1;
        }

        char item[FLOW_VAL_MAX];
        int m = e - s; if (m > FLOW_VAL_MAX - 1) m = FLOW_VAL_MAX - 1;
        for (int k = 0; k < m; k++) item[k] = items[s + k];
        item[m] = 0;

        // Bind the per-iteration outputs the body consumes via edges from this
        // loop node's "item" / "index" ports.
        flow_value_t vitem; flow_value_set_str(&vitem, item);
        flow_set_output(n, "item", &vitem);
        flow_value_t vidx; flow_value_set_int(&vidx, count);
        flow_set_output(n, "index", &vidx);

        if (run_loop_body(ctx, L, body, nbody) != 0) return -1;   // ctx->err set

        // This iteration's result: whatever a body node feeds back into this
        // loop node's "collect" input. With no collect edge, echo the item.
        char cs[FLOW_VAL_MAX] = {0};
        flow_value_t cv;
        if (flow_input(g, n, "collect", &cv)) flow_value_as_str(&cv, cs, sizeof(cs));
        else scpy(cs, item, sizeof(cs));

        if (count > 0 && rlen < FLOW_VAL_MAX - 1) results[rlen++] = sepc;
        for (int k = 0; cs[k] && rlen < FLOW_VAL_MAX - 1; k++) results[rlen++] = cs[k];
        results[rlen] = 0;
        count++;
    }

    // results is a LIST value (text form is the sep-joined collection); count
    // is an int. Downstream nodes read "results" (or "count").
    flow_value_t rout; rout.type = FV_LIST; scpy(rout.s, results, FLOW_VAL_MAX); rout.i = count;
    flow_set_output(n, "results", &rout);
    flow_value_t cout; flow_value_set_int(&cout, count);
    flow_set_output(n, "count", &cout);
    fprintf(stderr, "flowrun: foreach '%s' ran %d iteration(s) -> results=\"%s\"\n",
            n->id, count, results);
    return 0;
}

// ---- M3: shared helpers for the agent-promise + async-subloop executors ----
// A value is "true" if it is a non-zero bool/int, or one of the usual truthy
// text forms. Used to read an agent's goal_met / a subloop's spawn gate.
static int flow_truthy(const flow_value_t *v) {
    if (!v) return 0;
    if (v->type == FV_BOOL || v->type == FV_INT) return v->i != 0;
    return seq(v->s, "true") || seq(v->s, "1") || seq(v->s, "yes") || seq(v->s, "on");
}

static long flow_as_int(const flow_value_t *v) {
    if (!v) return 0;
    if (v->type == FV_INT || v->type == FV_BOOL) return v->i;
    return atol(v->s);
}

// Parse a simple measured target of the form "<lhs> <op> <int>" (e.g.
// "metric >= 3200") out of an agent's `success` note. The left-hand side is
// ignored (assumed to name the metric input). op codes: 1 >=, 2 <=, 3 >, 4 <,
// 5 ==. Returns 1 and fills *op/*rhs on success, else 0 (measured disabled: the
// success note is then treated as a human description only, not a threshold).
static int parse_metric_target(const char *success, int *op, long *rhs) {
    if (!success || !success[0]) return 0;
    const char *p = success;
    int found = 0;
    for (; *p; p++) {
        if (p[0] == '>' && p[1] == '=') { *op = 1; p += 2; found = 1; break; }
        if (p[0] == '<' && p[1] == '=') { *op = 2; p += 2; found = 1; break; }
        if (p[0] == '=' && p[1] == '=') { *op = 5; p += 2; found = 1; break; }
        if (p[0] == '>')                { *op = 3; p += 1; found = 1; break; }
        if (p[0] == '<')                { *op = 4; p += 1; found = 1; break; }
        if (p[0] == '=')                { *op = 5; p += 1; found = 1; break; }
    }
    if (!found) return 0;
    while (*p == ' ') p++;
    if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return 0;   // no numeric rhs
    *rhs = atol(p);
    return 1;
}

static int cmp_op(long lhs, int op, long rhs) {
    switch (op) {
        case 1: return lhs >= rhs;
        case 2: return lhs <= rhs;
        case 3: return lhs >  rhs;
        case 4: return lhs <  rhs;
        case 5: return lhs == rhs;
    }
    return 0;
}

// ---- M3: agent-promise loop ("promise completion") ------------------------
// An agent node owns a perceive->decide->act BODY (nodes whose params.loop point
// at it, the SAME membership mechanism as foreach) which is ONE pass. The agent
// runs that body repeatedly until a TERMINATION PROMISE is met, then reports the
// reason. The three promise kinds, checked in this priority order after each
// pass:
//   (a) OBJECTIVE - a body node's `goal_met` (bool) wired back to the agent is
//       true. In the real path this is an `llm` decide node's goal_met output
//       (h_llm parses it from the model reply); in the deterministic host test a
//       transform threshold on the agent's own iteration index stands in.
//   (b) MEASURED  - the `metric` input crosses the target parsed from `success`
//       (e.g. "metric >= 3200"). Disabled if `success` has no numeric threshold
//       or no metric input is wired.
//   (c) BUDGET    - iterations >= max_iters (also the hard runaway cap),
//       elapsed >= max_minutes (monotonic wall clock via flow_plat_now_ms), or
//       accumulated tokens >= max_tokens.
// `done` is set to objective|measured|max_iters|timeout|token_budget.
//
// TOKEN ACCOUNTING: the shared aiclient (aiclient_ask) exposes no token count,
// so tokens are accounted as an ITERATION PROXY (+1 per pass). This is stated
// here, in the CHANGELOG, and in the runtime stderr line; it is not presented as
// a real token measurement. When the client later exposes usage, sum it here.
//
// Before each pass the agent binds its 0-based iteration count on output port
// "index" so the body (and, via a self-edge, the agent's own metric input) can
// react to progress. Outputs: "done" (enum reason), "iterations" (int),
// "tokens" (int).
static int h_agent(flow_ctx_t *ctx, flow_node_t *n) {
    flow_graph_t *g = ctx->g;
    int L = idx_of_id(g, n->id);
    if (L < 0) { snprintf(ctx->err, FLOW_ERR_MAX, "agent '%s': not in graph", n->id); return -1; }

    // Body = perceive->decide->act nodes for one pass.
    int body[FLOW_MAX_NODES], nbody = 0;
    for (int i = 0; i < g->nnodes; i++)
        if (loop_container_idx(g, i) == L) body[nbody++] = i;

    // Budgets. max_iters is ALSO the hard runaway cap: even with no budget set,
    // the loop can never exceed FLOW_LOOP_MAX_ITERS.
    int cap = FLOW_LOOP_MAX_ITERS;
    int max_iters = (n->max_iters > 0 && n->max_iters < cap) ? n->max_iters : cap;
    long max_tokens = n->max_tokens;   // 0 = no token budget

    int mop = 0; long mrhs = 0;
    int have_metric_target = parse_metric_target(n->success, &mop, &mrhs);

    unsigned long start_ms = flow_plat_now_ms();
    int have_deadline = (n->max_minutes > 0);
    unsigned long deadline_ms = start_ms + (unsigned long)n->max_minutes * 60UL * 1000UL;

    long iterations = 0;
    long tokens = 0;                   // iteration proxy (see the note above)
    const char *done = "max_iters";

    for (;;) {
        // Per-iteration binding the body (and a metric self-edge) can read.
        flow_value_t vidx; flow_value_set_int(&vidx, iterations);
        flow_set_output(n, "index", &vidx);

        if (nbody > 0 && run_loop_body(ctx, L, body, nbody) != 0)
            return -1;                 // ctx->err set by the body

        iterations++;
        tokens += 1;                   // proxy: aiclient exposes no token count

        // (a) OBJECTIVE
        flow_value_t gm;
        if (flow_input(g, n, "goal_met", &gm) && flow_truthy(&gm)) { done = "objective"; break; }

        // (b) MEASURED
        if (have_metric_target) {
            flow_value_t mv;
            if (flow_input(g, n, "metric", &mv) && cmp_op(flow_as_int(&mv), mop, mrhs)) {
                done = "measured"; break;
            }
        }

        // (c) BUDGET
        if (iterations >= max_iters) { done = "max_iters"; break; }
        if (have_deadline && flow_plat_now_ms() >= deadline_ms) { done = "timeout"; break; }
        if (max_tokens > 0 && tokens >= max_tokens) { done = "token_budget"; break; }
    }

    flow_value_t vd; flow_value_set_str(&vd, done);        flow_set_output(n, "done", &vd);
    flow_value_t vi; flow_value_set_int(&vi, iterations);  flow_set_output(n, "iterations", &vi);
    flow_value_t vt; flow_value_set_int(&vt, tokens);      flow_set_output(n, "tokens", &vt);
    fprintf(stderr, "flowrun: agent '%s' done=%s iterations=%ld tokens=%ld "
                    "(tokens is an iteration proxy; aiclient exposes no usage count)\n",
            n->id, done, iterations, tokens);
    return 0;
}

// ---- M3: async sub-loop ----------------------------------------------------
// A subloop node owns a BODY (same `loop` membership) and has params mode
// (await | fire-and-forget), every_ms, max_conc, an optional `spawn` (bool)
// input gate, and outputs result / metric / joined.
//
// WHAT EACH MODE DOES TODAY (honest):
//   await            - runs the sub-body to completion, then fires `joined` and
//                      continues. Semantically complete and fully real.
//   fire-and-forget  - the runner is single-threaded and we add NO kernel
//                      threads, so this ALSO runs the sub-body to completion at
//                      spawn RIGHT NOW. It does NOT run in the background: there
//                      is no real concurrency yet. every_ms and max_conc are
//                      PARSED but NOT scheduled. True background concurrency is a
//                      later kernel-threads item. We do not pretend otherwise;
//                      the only observable difference today is the stderr note.
//
// The `spawn` input, if wired and false, skips the run (joined=false). Result is
// whatever a body node feeds into the subloop's "collect" (or "result") input;
// metric likewise from "metric". Reuses run_loop_body(), like a foreach of one.
static int h_subloop(flow_ctx_t *ctx, flow_node_t *n) {
    flow_graph_t *g = ctx->g;
    int L = idx_of_id(g, n->id);
    if (L < 0) { snprintf(ctx->err, FLOW_ERR_MAX, "subloop '%s': not in graph", n->id); return -1; }

    const char *mode = n->mode[0] ? n->mode : flow_param(n, "mode");
    if (!mode || !mode[0]) mode = "await";
    int fireforget = seq(mode, "fire-and-forget") || seq(mode, "fireforget") ||
                     seq(mode, "async");

    // Spawn gate: if a `spawn` input is wired and false, do not run this time.
    flow_value_t sp;
    if (flow_input(g, n, "spawn", &sp) && !flow_truthy(&sp)) {
        flow_value_t jf; flow_value_set_bool(&jf, 0); flow_set_output(n, "joined", &jf);
        flow_value_t rf; flow_value_set_str(&rf, "");  flow_set_output(n, "result", &rf);
        flow_value_t mf; flow_value_set_int(&mf, 0);   flow_set_output(n, "metric", &mf);
        fprintf(stderr, "flowrun: subloop '%s' spawn=false -> not run this step\n", n->id);
        return 0;
    }

    int body[FLOW_MAX_NODES], nbody = 0;
    for (int i = 0; i < g->nnodes; i++)
        if (loop_container_idx(g, i) == L) body[nbody++] = i;

    flow_value_t vidx; flow_value_set_int(&vidx, 0); flow_set_output(n, "index", &vidx);
    if (nbody > 0 && run_loop_body(ctx, L, body, nbody) != 0)
        return -1;                     // ctx->err set by the body

    char rs[FLOW_VAL_MAX] = {0};
    flow_value_t cv;
    if (flow_input(g, n, "collect", &cv) || flow_input(g, n, "result", &cv))
        flow_value_as_str(&cv, rs, sizeof(rs));
    flow_value_t rout; flow_value_set_str(&rout, rs); flow_set_output(n, "result", &rout);

    long metric = 0;
    flow_value_t mv;
    if (flow_input(g, n, "metric", &mv)) metric = flow_as_int(&mv);
    flow_value_t mout; flow_value_set_int(&mout, metric); flow_set_output(n, "metric", &mout);

    flow_value_t jt; flow_value_set_bool(&jt, 1); flow_set_output(n, "joined", &jt);

    if (fireforget) {
        const char *mc = flow_param(n, "max_conc");
        fprintf(stderr, "flowrun: subloop '%s' mode=fire-and-forget ran the sub-body "
                        "TO COMPLETION synchronously at spawn (NO real background "
                        "concurrency yet; every_ms=%d max_conc=%s parsed but not "
                        "scheduled - true async is a later kernel-threads item)\n",
                n->id, n->every_ms, mc ? mc : "1");
    } else {
        fprintf(stderr, "flowrun: subloop '%s' mode=await ran sub-body to completion "
                        "and joined\n", n->id);
    }
    return 0;
}

// ===========================================================================
// #469 AI-VISION: perceive (capture) -> decide (llm+image) -> act (input)
// ===========================================================================
// The node vocabulary is deliberately GENERIC. Nothing below knows what a Game
// Boy is: `capture` photographs a window, `llm` reads a picture, `input` presses
// a key. The Game Boy is one flow's worth of YAML, not a node type.

// s_vision_calls (declared at the top of this file) counts image-carrying model
// calls for the whole RUN, against the hard FLOW_MAX_VISION_CALLS ceiling. It is
// reset once per flow_execute(), like the edge caches; the runner is single
// threaded.
static int param_int(const flow_node_t *n, const char *key, int dflt) {
    const char *v = flow_param(n, key);
    if (!v || !v[0]) return dflt;
    return atoi(v);
}

// Parse "x,y,w,h" (commas and/or spaces) into four ints. Returns 1 on success,
// 0 if the text is not exactly four integers - which is an ERROR at the call
// site, never a silently-ignored rect.
static int parse_rect(const char *s, int *x, int *y, int *w, int *h) {
    int v[4] = {0, 0, 0, 0}, n = 0;
    const char *p = s;
    if (!p) return 0;
    while (*p && n < 4) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        if (*p < '0' || *p > '9') return 0;
        int acc = 0;
        while (*p >= '0' && *p <= '9') acc = acc * 10 + (*p++ - '0');
        v[n++] = neg ? -acc : acc;
    }
    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    if (n != 4 || *p) return 0;
    *x = v[0]; *y = v[1]; *w = v[2]; *h = v[3];
    return 1;
}

// A named key -> the keycode the compositor delivers for that real key. Every
// value is a GUI_KEY_* from the shared keys.h or a literal printable ASCII
// character (which is exactly what a real letter key delivers), so an app that
// matches on keycode AND an app that matches on key_char both see what they
// would see from the hardware.
typedef struct { const char *name; int code; } flow_keyname_t;
static const flow_keyname_t FLOW_KEYNAMES[] = {
    { "UP",        GUI_KEY_UP    }, { "DOWN",      GUI_KEY_DOWN  },
    { "LEFT",      GUI_KEY_LEFT  }, { "RIGHT",     GUI_KEY_RIGHT },
    { "ENTER",     GUI_KEY_ENTER }, { "RETURN",    GUI_KEY_ENTER },
    { "ESC",       GUI_KEY_ESC   }, { "ESCAPE",    GUI_KEY_ESC   },
    { "TAB",       GUI_KEY_TAB   }, { "BACKSPACE", GUI_KEY_BKSP  },
    { "SPACE",     ' '           },
    { "HOME",      GUI_KEY_HOME  }, { "END",       GUI_KEY_END   },
    { "PGUP",      GUI_KEY_PGUP  }, { "PGDN",      GUI_KEY_PGDN  },
    { "INSERT",    GUI_KEY_INS   }, { "DELETE",    GUI_KEY_DEL   },
    { "LSHIFT",    GUI_KEY_LSHIFT}, { "RSHIFT",    GUI_KEY_RSHIFT},
    { "LCTRL",     GUI_KEY_LCTRL }, { "ALT",       GUI_KEY_ALT   },
    { "F1", GUI_KEY_F1 }, { "F2", GUI_KEY_F2 }, { "F3", GUI_KEY_F3 },
    { "F4", GUI_KEY_F4 }, { "F5", GUI_KEY_F5 }, { "F6", GUI_KEY_F6 },
    { "F7", GUI_KEY_F7 }, { "F8", GUI_KEY_F8 }, { "F9", GUI_KEY_F9 },
    { "F10", GUI_KEY_F10 }, { "F11", GUI_KEY_F11 }, { "F12", GUI_KEY_F12 },
};

static int ciequ(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

// One token -> a keycode, or -1 for "this is not a key". Accepted forms:
//   a NAME from the table above (case insensitive)
//   a SINGLE printable character, which is itself ('x', '5', '.')
//   0xNN, an explicit keycode, for anything the table does not name
int flow_keycode_of(const char *tok) {
    if (!tok || !tok[0]) return -1;
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X') && tok[2]) {
        int v = 0;
        for (const char *p = tok + 2; *p; p++) {
            int d;
            if (*p >= '0' && *p <= '9')      d = *p - '0';
            else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
            else return -1;
            v = v * 16 + d;
            if (v > 0x1FF) return -1;
        }
        return v;
    }
    if (!tok[1] && tok[0] >= 0x20 && tok[0] <= 0x7E) return (unsigned char)tok[0];
    for (int i = 0; i < (int)(sizeof(FLOW_KEYNAMES) / sizeof(FLOW_KEYNAMES[0])); i++)
        if (ciequ(FLOW_KEYNAMES[i].name, tok)) return FLOW_KEYNAMES[i].code;
    return -1;
}

// ---- capture --------------------------------------------------------------
// params: target, rect ("x,y,w,h" in the target's own coordinates), max_width,
//         max_height, quality, path
// inputs: "target" (optional, overrides the param, so one node can photograph
//         whatever an earlier node named)
// outputs: "image" (the JPEG's path) and "out" (the same, so a bare edge
//         works), "w", "h", "bytes"
static int h_capture(flow_ctx_t *ctx, flow_node_t *n) {
    char target[FLOW_VAL_MAX];
    const char *tp = flow_param(n, "target");
    scpy(target, (tp && tp[0]) ? tp : "screen", sizeof(target));
    flow_value_t tv;
    if (flow_input(ctx->g, n, "target", &tv)) {
        char t2[FLOW_VAL_MAX];
        flow_value_as_str(&tv, t2, sizeof(t2));
        if (t2[0]) scpy(target, t2, sizeof(target));
    }

    const char *path = flow_param(n, "path");
    if (!path || !path[0]) {
        snprintf(ctx->err, FLOW_ERR_MAX,
                 "capture '%s': missing 'path' (where to write the JPEG)", n->id);
        return -1;
    }

    int rx = 0, ry = 0, rw = 0, rh = 0;
    const char *rect = flow_param(n, "rect");
    if (rect && rect[0] && !parse_rect(rect, &rx, &ry, &rw, &rh)) {
        snprintf(ctx->err, FLOW_ERR_MAX,
                 "capture '%s': rect must be \"x,y,w,h\", got '%s'", n->id, rect);
        return -1;
    }
    if (rw < 0 || rh < 0) {
        snprintf(ctx->err, FLOW_ERR_MAX, "capture '%s': rect width/height cannot be negative", n->id);
        return -1;
    }
    int max_w = param_int(n, "max_width", 512);
    int max_h = param_int(n, "max_height", 512);
    int quality = param_int(n, "quality", 70);
    if (max_w < 8 || max_h < 8 || max_w > 4096 || max_h > 4096) {
        snprintf(ctx->err, FLOW_ERR_MAX,
                 "capture '%s': max_width/max_height must be 8..4096", n->id);
        return -1;
    }
    if (quality < 1 || quality > 100) {
        snprintf(ctx->err, FLOW_ERR_MAX, "capture '%s': quality must be 1..100", n->id);
        return -1;
    }

    int ow = 0, oh = 0;
    int bytes = flow_plat_capture(target, rx, ry, rw, rh, max_w, max_h, quality,
                                  path, &ow, &oh, ctx->err, FLOW_ERR_MAX);
    if (bytes < 0) return -1;              // err already set by the platform
    ctx->total_written += bytes;

    flow_value_t v; flow_value_set_str(&v, path);
    flow_set_output(n, "image", &v);
    flow_set_output(n, "out", &v);
    flow_value_t vw; flow_value_set_int(&vw, ow);    flow_set_output(n, "w", &vw);
    flow_value_t vh; flow_value_set_int(&vh, oh);    flow_set_output(n, "h", &vh);
    flow_value_t vb; flow_value_set_int(&vb, bytes); flow_set_output(n, "bytes", &vb);
    fprintf(stderr, "flowrun: capture '%s' target=%s -> %s (%dx%d, %d byte JPEG)\n",
            n->id, target, path, ow, oh, bytes);
    return 0;
}

// ---- input ----------------------------------------------------------------
// params: target, keys, hold_ms, gap_ms, max_keys
// inputs: "keys" (or "in") overrides params.keys, which is what makes an agent
//         loop possible: the decide step's reply IS the key list.
//         "target" likewise overrides params.target.
// outputs: "out"/"sent" = keys delivered (int), "skipped" = tokens that were
//         not keys.
//
// A token that is not a key is SKIPPED and counted, not fatal: in an agent loop
// the key list comes from a language model, and a model that answers "press
// ENTER" instead of "ENTER" is a prompting problem, not a reason to abort a
// whole workflow. An EMPTY key list IS fatal, because that is a wiring mistake.
static int h_input(flow_ctx_t *ctx, flow_node_t *n) {
    char target[FLOW_VAL_MAX];
    const char *tp = flow_param(n, "target");
    scpy(target, tp ? tp : "", sizeof(target));
    flow_value_t tv;
    if (flow_input(ctx->g, n, "target", &tv)) {
        char t2[FLOW_VAL_MAX];
        flow_value_as_str(&tv, t2, sizeof(t2));
        if (t2[0]) scpy(target, t2, sizeof(target));
    }
    if (!target[0]) {
        snprintf(ctx->err, FLOW_ERR_MAX,
                 "input '%s': missing 'target' (which window to drive)", n->id);
        return -1;
    }

    char keys[FLOW_VAL_MAX] = {0};
    flow_value_t kv;
    if (flow_input(ctx->g, n, "keys", &kv) || flow_input(ctx->g, n, "in", &kv))
        flow_value_as_str(&kv, keys, sizeof(keys));
    else { const char *k = flow_param(n, "keys"); scpy(keys, k ? k : "", sizeof(keys)); }
    if (!keys[0]) {
        snprintf(ctx->err, FLOW_ERR_MAX,
                 "input '%s': no keys (wire a 'keys' input or set params.keys)", n->id);
        return -1;
    }

    // #469: `after` is the answer to prose. In an agent loop the key list comes
    // from a language model, whose reply is usually a sentence with the keys
    // somewhere in it, and a bare single letter inside that sentence ("a new
    // game") would otherwise be tokenised as a keypress. With `after` set, only
    // the text following the FIRST occurrence of that marker, to the end of
    // THAT LINE, is read as keys. If the marker is absent the node presses
    // NOTHING (and says so) rather than falling back to scanning the prose:
    // when the model did not answer in the agreed format, doing nothing this
    // pass is the safe reading, not guessing.
    const char *after = flow_param(n, "after");
    if (after && after[0]) {
        char *m = strstr(keys, after);
        if (!m) {
            fprintf(stderr, "flowrun: input '%s': marker \"%s\" not found in the "
                            "key text; pressing nothing this pass\n", n->id, after);
            keys[0] = 0;
        } else {
            m += strlen(after);
            char line[FLOW_VAL_MAX]; int L = 0;
            while (*m && *m != '\n' && *m != '\r' && L < FLOW_VAL_MAX - 1) line[L++] = *m++;
            line[L] = 0;
            scpy(keys, line, sizeof(keys));
        }
    }

    int hold_ms  = param_int(n, "hold_ms", 60);
    int gap_ms   = param_int(n, "gap_ms", 120);
    int max_keys = param_int(n, "max_keys", 8);
    if (max_keys < 1) max_keys = 1;
    if (max_keys > 64) max_keys = 64;
    if (hold_ms < 0) hold_ms = 0;
    if (gap_ms < 0) gap_ms = 0;

    int sent = 0, skipped = 0;
    int i = 0;
    while (keys[i] && sent < max_keys) {
        while (keys[i] == ' ' || keys[i] == ',' || keys[i] == '\n' ||
               keys[i] == '\r' || keys[i] == '\t' || keys[i] == ';') i++;
        if (!keys[i]) break;
        char tok[FLOW_KEY_MAX]; int t = 0;
        while (keys[i] && keys[i] != ' ' && keys[i] != ',' && keys[i] != '\n' &&
               keys[i] != '\r' && keys[i] != '\t' && keys[i] != ';') {
            if (t < FLOW_KEY_MAX - 1) tok[t++] = keys[i];
            i++;
        }
        tok[t] = 0;
        int kc = flow_keycode_of(tok);
        if (kc < 0) {
            skipped++;
            fprintf(stderr, "flowrun: input '%s': token \"%s\" is not a key, skipped\n", n->id, tok);
            continue;
        }
        if (flow_plat_input_key(target, kc, hold_ms, gap_ms, ctx->err, FLOW_ERR_MAX) != 0)
            return -1;                     // err set by the platform (no grant, bad target, ...)
        sent++;
    }

    flow_value_t vs; flow_value_set_int(&vs, sent);
    flow_set_output(n, "out", &vs);
    flow_set_output(n, "sent", &vs);
    flow_value_t vk; flow_value_set_int(&vk, skipped);
    flow_set_output(n, "skipped", &vk);
    fprintf(stderr, "flowrun: input '%s' target=%s sent %d key(s), skipped %d token(s)\n",
            n->id, target, sent, skipped);
    return 0;
}

typedef struct { const char *type; flow_handler_fn fn; } registry_ent_t;
static const registry_ent_t g_registry[] = {
    { "trigger",   h_trigger   },
    { "transform", h_transform },
    { "file",      h_file      },
    { "llm",       h_llm       },
    // M2: the FOR-EACH executor and app-contract dispatch.
    { "foreach",   h_foreach   },   // the visual editor's "For Each" node type
    { "loop",      h_foreach   },   // runner/docs vocabulary alias for foreach
    { "app",       h_app       },
    // M3 (this change): the agent-promise loop and the async sub-loop.
    { "agent",     h_agent     },
    { "subloop",   h_subloop   },
    // #469 AI-VISION: perceive and act. (Decide is the existing llm node,
    // which grew an `image` input port rather than becoming a second node
    // type: a vision call is a completion, not a new kind of thing.)
    { "capture",   h_capture   },
    { "input",     h_input     },
    // STILL NOT registered, and intentionally so: device, service, merge,
    // branch, variable. The executor returns an explicit "unsupported node
    // type: X" for any type without a handler (see below). We do not fake them.
};

static flow_handler_fn lookup_handler(const char *type) {
    int n = (int)(sizeof(g_registry) / sizeof(g_registry[0]));
    for (int i = 0; i < n; i++)
        if (seq(g_registry[i].type, type)) return g_registry[i].fn;
    return 0;
}

// ---- topological execution (Kahn) -----------------------------------------
// A foreach/loop node plus its body is a SUB-REGION: the loop node is scheduled
// at the top level like any other node, and its handler (h_foreach) runs the
// whole body once per item. So body nodes are excluded from the top-level Kahn
// schedule, and each edge is mapped to its EFFECTIVE top-level endpoints:
//   - an edge INTO a body node is internal to a loop (skipped at top level);
//   - an edge OUT OF a body node is treated as coming FROM that body's loop
//     node, so a node downstream of the loop waits for the loop to finish.
// With no loops present, loop_of[] is all -1 and this reduces to the original
// flat Kahn.
int flow_execute(flow_ctx_t *ctx) {
    flow_graph_t *g = ctx->g;
    ctx->total_written = 0;
    ctx->err[0] = 0;

    // Resolve every edge endpoint id and every node's container ONCE; the
    // top-level schedule and every per-iteration loop body then read the cache.
    precompute_indices(g);

    // #469: per-RUN budgets. Node `calls` is deliberately not touched by
    // run_loop_body(), so it must be zeroed exactly here, once.
    s_vision_calls = 0;
    for (int i = 0; i < g->nnodes; i++) g->nodes[i].calls = 0;

    int indeg[FLOW_MAX_NODES];
    for (int i = 0; i < g->nnodes; i++) indeg[i] = 0;
    for (int e = 0; e < g->nedges; e++) {
        int ti = s_edge_to[e];
        if (ti < 0 || s_loop_of[ti] >= 0) continue;  // edge into a body node: internal
        int fi = s_edge_from[e];
        int eff_from = (fi >= 0 && s_loop_of[fi] >= 0) ? s_loop_of[fi] : fi;
        if (eff_from == ti) continue;                // self-edge (e.g. body -> its loop's collect)
        indeg[ti]++;
    }

    int queue[FLOW_MAX_NODES], qh = 0, qt = 0;
    int top_count = 0;
    for (int i = 0; i < g->nnodes; i++) {
        if (s_loop_of[i] >= 0) continue;             // body node: run inside its loop
        top_count++;
        if (indeg[i] == 0) queue[qt++] = i;
    }

    int processed = 0;
    while (qh < qt) {
        int idx = queue[qh++];
        flow_node_t *n = &g->nodes[idx];

        flow_handler_fn fn = lookup_handler(n->type);
        if (!fn) {
            snprintf(ctx->err, FLOW_ERR_MAX, "unsupported node type: %s (node '%s')",
                     n->type[0] ? n->type : "(none)", n->id);
            return 1;
        }
        if (fn(ctx, n) != 0) return 1;   // ctx->err set by the handler
        n->executed = 1;
        processed++;

        // Relax effective outgoing edges (edges whose effective source is idx).
        for (int e = 0; e < g->nedges; e++) {
            int fi = s_edge_from[e];
            int eff_from = (fi >= 0 && s_loop_of[fi] >= 0) ? s_loop_of[fi] : fi;
            if (eff_from != idx) continue;
            int ti = s_edge_to[e];
            if (ti < 0 || s_loop_of[ti] >= 0 || ti == idx) continue;
            if (--indeg[ti] == 0) queue[qt++] = ti;
        }
    }

    if (processed < top_count) {
        scpy(ctx->err, "workflow graph has a cycle (not a DAG)", FLOW_ERR_MAX);
        return 1;
    }
    return 0;
}
