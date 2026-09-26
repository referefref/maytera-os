// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// flowgen.c - Maytera Flow chat-driven workflow generation (flowgen).
//
// Platform-neutral so the SAME code runs in the freestanding aichat binary and
// in the host gcc unit test (flowgen_hosttest.c). The include split mirrors
// userland/apps/flowrun/flow.c: FLOWRUN_HOSTTEST selects the system headers.
//
// See flowgen.h for the contract. The load-bearing function is
// flowgen_validate(): it parses a candidate with the RUNNER'S own parser
// (flow_parse_bytes) so nothing malformed is ever saved.

#ifdef FLOWRUN_HOSTTEST
#  include <stdio.h>
#  include <string.h>
#else
#  include "stdio.h"
#  include "string.h"
#endif

#include "flow.h"
#include "flowgen.h"

// ---- small bounded helpers (portable across host + MayteraOS) -------------
static void bcpy(char *dst, const char *src, int cap) {
    if (cap <= 0) return;
    int i = 0;
    if (src) { for (; src[i] && i < cap - 1; i++) dst[i] = src[i]; }
    dst[i] = 0;
}

// Append src to dst[] (which has total capacity cap) starting at *pos; advances
// *pos. Truncates safely. Returns 0 always (callers just chain).
static void app(char *dst, int cap, int *pos, const char *src) {
    if (!src) return;
    int p = *pos;
    while (*src && p < cap - 1) dst[p++] = *src++;
    dst[p] = 0;
    *pos = p;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Copy [s, s+len) into out[] trimming leading/trailing whitespace. Returns len.
static int copy_trim(char *out, int cap, const char *s, int len) {
    if (cap <= 0) return 0;
    while (len > 0 && is_space(*s)) { s++; len--; }
    while (len > 0 && is_space(s[len - 1])) len--;
    if (len > cap - 1) len = cap - 1;
    int i = 0;
    for (; i < len; i++) out[i] = s[i];
    out[i] = 0;
    return i;
}

// A node lookup local to this module (flow.c's own is static).
static const flow_node_t *fg_find_node(const flow_graph_t *g, const char *id) {
    if (!id || !id[0]) return 0;
    for (int i = 0; i < g->nnodes; i++)
        if (strcmp(g->nodes[i].id, id) == 0) return &g->nodes[i];
    return 0;
}

// ---- the embedded schema --------------------------------------------------
static const char SCHEMA[] =
"MAYTERA FLOW WORKFLOW SCHEMA (YAML):\n"
"name: <string>            # short lowercase-hyphen identifier\n"
"description: <string>     # one line: what this workflow does\n"
"nodes:                    # REQUIRED, a non-empty list\n"
"  - id: <unique string>\n"
"    type: <node type below>\n"
"    params: { key: value, ... }   # per-type scalar parameters\n"
"edges:                    # optional list, connects an output port to an input port\n"
"  - from: [<nodeId>, <outPort>]\n"
"    to:   [<nodeId>, <inPort>]\n"
"An edge side may be a bare <nodeId> (default port 'out' for from, 'in' for to).\n"
"A node runs only after every node feeding its inputs has run (topological order).\n"
"\n"
"EXECUTABLE NODE TYPES (use ONLY these):\n"
"- trigger:   params.kind = manual (default) | cron. Output 'out' = bool. Start every workflow with one manual trigger.\n"
"- transform: params.op = constant | upper | lower | concat | gte | gt | lte | lt.\n"
"    constant -> params.text ; upper/lower -> input port 'in' ; concat -> in + params.sep + in2 ;\n"
"    gte/gt/lte/lt -> input 'in' (int) vs params.value (int) -> bool. Output 'out'.\n"
"- llm:       params.system (optional), params.prompt (use {portName} placeholders filled from inbound edges). Output 'out' = model reply. Needs network.\n"
"- file:      params.op = write (default) | append ; params.path = destination. Text from inbound port 'text'/'in' or params.text. Output 'out' = bytes written.\n"
"- foreach (alias loop): params.items (a sep-separated string OR inbound 'items'), params.sep (default ','), params.max_iters.\n"
"    Body nodes declare params.loop = <thisLoopId>; each item binds outputs 'item'/'index'; the loop collects a body node feeding its 'collect' input. Outputs: results, count.\n"
"- app:       params.app = a contract app id, params.feature = an action, params.verb = call (default) | get | set. Input 'arg'/'in'. Output 'out' = parsed reply.\n"
"- agent:     a perceive/decide/act body (body nodes declare params.loop = <thisId>) run until a termination promise. params.objective, params.success (e.g. \"metric >= 5\"), params.max_iters, params.max_minutes, params.max_tokens. Outputs: done, iterations, tokens.\n"
"- subloop:   owns a body (params.loop = <thisId>); params.mode = await (default) | fire-and-forget ; params.every_ms. Outputs: result, metric, joined.\n"
"\n"
"RESERVED, NOT YET EXECUTABLE, do NOT emit: branch, merge, device, service, variable.\n"
"\n"
"HARD RULES:\n"
"- Output ONLY the YAML. No prose, no explanation, no markdown code fences, no ACTION lines.\n"
"- Include a top-level name: and a one-line description:.\n"
"- Every edge endpoint MUST reference a node id you defined above.\n"
"- Prefer the fewest, cleanest nodes that achieve the intent.\n";

const char *flowgen_schema_text(void) { return SCHEMA; }

// ---- prompt builders ------------------------------------------------------
int flowgen_build_prompt(flowgen_mode_t mode, const char *payload,
                         const char *context, char *out, int cap) {
    int p = 0;
    out[0] = 0;
    switch (mode) {
    case FLOWGEN_GENERATE:
        app(out, cap, &p, "You are Maytera Flow's workflow generator for MayteraOS.\n\n");
        app(out, cap, &p, SCHEMA);
        app(out, cap, &p, "\nTASK: Produce ONE workflow YAML that accomplishes the following.\n");
        if (context && context[0]) {
            app(out, cap, &p, "\nCONVERSATION CONTEXT (infer the user's intent from this chat):\n");
            app(out, cap, &p, context);
            app(out, cap, &p, "\n");
        }
        if (payload && payload[0]) {
            app(out, cap, &p, "\nUSER REQUEST: ");
            app(out, cap, &p, payload);
            app(out, cap, &p, "\n");
        }
        app(out, cap, &p, "\nOutput ONLY the workflow YAML now.");
        break;
    case FLOWGEN_OPTIMISE:
        app(out, cap, &p, "You are Maytera Flow's workflow optimiser for MayteraOS.\n\n");
        app(out, cap, &p, SCHEMA);
        app(out, cap, &p, "\nTASK: Rewrite the workflow below to be cleaner and use FEWER nodes WITHOUT changing what it does. Keep its intent, its name, and its description. Remove redundant nodes, merge trivial transforms, and simplify edges.\n\nEXISTING WORKFLOW:\n");
        app(out, cap, &p, payload ? payload : "");
        app(out, cap, &p, "\n\nOutput ONLY the optimised workflow YAML.");
        break;
    case FLOWGEN_MODIFY:
        app(out, cap, &p, "You are Maytera Flow's workflow editor for MayteraOS.\n\n");
        app(out, cap, &p, SCHEMA);
        app(out, cap, &p, "\nTASK: Apply the requested change to the workflow below, keeping everything else intact and valid. Keep the same name unless the change asks otherwise.\n\nREQUESTED CHANGE: ");
        app(out, cap, &p, (context && context[0]) ? context : "(no change specified)");
        app(out, cap, &p, "\n\nEXISTING WORKFLOW:\n");
        app(out, cap, &p, payload ? payload : "");
        app(out, cap, &p, "\n\nOutput ONLY the modified workflow YAML.");
        break;
    case FLOWGEN_FORK:
        app(out, cap, &p, "You are Maytera Flow's workflow author for MayteraOS.\n\n");
        app(out, cap, &p, SCHEMA);
        app(out, cap, &p, "\nTASK: Produce a VARIATION of the workflow below. ");
        if (context && context[0]) {
            app(out, cap, &p, "Variation: ");
            app(out, cap, &p, context);
            app(out, cap, &p, " ");
        }
        app(out, cap, &p, "Give the variation a NEW name:. Keep it valid.\n\nBASE WORKFLOW:\n");
        app(out, cap, &p, payload ? payload : "");
        app(out, cap, &p, "\n\nOutput ONLY the new workflow YAML.");
        break;
    }
    return p;
}

int flowgen_build_retry_prompt(const char *broken_yaml, const char *parse_err,
                               char *out, int cap) {
    int p = 0;
    out[0] = 0;
    app(out, cap, &p, "Your previous reply was NOT a valid Maytera Flow workflow.\n");
    app(out, cap, &p, "PARSER ERROR: ");
    app(out, cap, &p, parse_err ? parse_err : "(unknown)");
    app(out, cap, &p, "\n\nHere is exactly what you produced:\n");
    app(out, cap, &p, broken_yaml ? broken_yaml : "");
    app(out, cap, &p, "\n\nReturn a CORRECTED workflow YAML that fixes this error and conforms to the schema (a non-empty nodes: list; every edge endpoint referencing a defined node id). Output ONLY the YAML, with no prose and no markdown fences.");
    return p;
}

// ---- LLM reply -> YAML ----------------------------------------------------
// Find the first line that begins with one of the top-level YAML keys, so a
// reply with leading prose still yields the YAML body when there is no fence.
static const char *find_yaml_start(const char *s) {
    const char *best = 0;
    static const char *keys[] = { "name:", "description:", "nodes:" };
    for (int k = 0; k < 3; k++) {
        const char *p = s;
        while ((p = strstr(p, keys[k])) != 0) {
            if (p == s || p[-1] == '\n') { if (!best || p < best) best = p; break; }
            p += 1;
        }
    }
    return best;
}

int flowgen_extract_yaml(const char *reply, char *out, int cap) {
    if (!reply) { if (cap > 0) out[0] = 0; return 0; }

    // 1. Prefer a fenced code block: ``` or ```yaml ... ```
    const char *fence = strstr(reply, "```");
    if (fence) {
        const char *s = fence + 3;
        while (*s && *s != '\n') s++;      // skip an optional language tag
        if (*s == '\n') s++;
        const char *e = strstr(s, "```");
        int len = e ? (int)(e - s) : (int)strlen(s);
        return copy_trim(out, cap, s, len);
    }

    // 2. No fence: trim leading prose to the first top-level YAML key.
    const char *start = find_yaml_start(reply);
    if (start) return copy_trim(out, cap, start, (int)strlen(start));

    // 3. Fallback: the whole reply, trimmed.
    return copy_trim(out, cap, reply, (int)strlen(reply));
}

// ---- validation (the gate) ------------------------------------------------
int flowgen_validate(const char *yaml, int len, flow_graph_t *g,
                     char *err, int cap) {
    static flow_graph_t local;         // used only when the caller passes none
    flow_graph_t *gg = g ? g : &local;

    if (cap > 0) err[0] = 0;
    if (!yaml || len <= 0) { bcpy(err, "empty candidate (LLM produced nothing)", cap); return 1; }

    // The runner's OWN parser. It already rejects malformed YAML, a non-mapping
    // top level, and a zero-node workflow (err set for each).
    if (flow_parse_bytes(yaml, len, gg, err, cap) != 0) return 1;

    // Semantic check the parser does not do: every edge endpoint must resolve
    // to a defined node id. An edge to a phantom node is a silent runtime dead
    // end, so reject it here rather than let the runner skip it.
    for (int i = 0; i < gg->nedges; i++) {
        const flow_edge_t *e = &gg->edges[i];
        if (!fg_find_node(gg, e->from_node)) {
            snprintf(err, cap, "edge %d: 'from' references undefined node '%s'",
                     i + 1, e->from_node[0] ? e->from_node : "(empty)");
            return 1;
        }
        if (!fg_find_node(gg, e->to_node)) {
            snprintf(err, cap, "edge %d: 'to' references undefined node '%s'",
                     i + 1, e->to_node[0] ? e->to_node : "(empty)");
            return 1;
        }
    }
    return 0;
}

// ---- name helpers ---------------------------------------------------------
void flowgen_sanitize_name(const char *in, char *out, int cap) {
    if (cap <= 0) return;
    int o = 0, last_dash = 1;   // last_dash=1 suppresses a leading '-'
    for (const char *p = in; p && *p && o < cap - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[o++] = c; last_dash = 0;
        } else if (!last_dash) {
            out[o++] = '-'; last_dash = 1;
        }
    }
    while (o > 0 && out[o - 1] == '-') o--;   // trim trailing '-'
    out[o] = 0;
    if (o == 0) bcpy(out, "workflow", cap);
}

int flowgen_name_from_yaml(const char *yaml, char *out, int cap) {
    if (cap > 0) out[0] = 0;
    if (!yaml) return 0;
    const char *p = yaml;
    while ((p = strstr(p, "name:")) != 0) {
        if (p == yaml || p[-1] == '\n') break;   // must be at line start (top level)
        p += 1;
    }
    if (!p) return 0;
    p += 5;                                       // past "name:"
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"' || *p == '\'') p++;             // strip an opening quote
    char raw[FLOW_ID_MAX];
    int i = 0;
    while (p[i] && p[i] != '\n' && p[i] != '"' && p[i] != '\'' && i < (int)sizeof(raw) - 1) { raw[i] = p[i]; i++; }
    raw[i] = 0;
    flowgen_sanitize_name(raw, out, cap);
    return out[0] ? 1 : 0;
}

int flowgen_ensure_description(char *yaml, int cap, const char *desc) {
    // Already has a top-level description:?
    const char *p = yaml;
    while ((p = strstr(p, "description:")) != 0) {
        if (p == yaml || p[-1] == '\n') return 0;   // present, nothing to do
        p += 1;
    }
    // Build a safe one-line description scalar.
    char line[256];
    int o = 0;
    const char *pfx = "description: \"";
    while (pfx[o]) { line[o] = pfx[o]; o++; }
    for (const char *d = desc; d && *d && o < (int)sizeof(line) - 4; d++) {
        char c = *d;
        if (c == '"' || c == '\n' || c == '\r' || c == '\t') c = ' ';
        line[o++] = c;
    }
    line[o++] = '"'; line[o++] = '\n'; line[o] = 0;

    int add = (int)strlen(line);
    int cur = (int)strlen(yaml);
    if (cur + add >= cap) return -1;               // would overflow: leave as-is
    // Prepend (a top-level mapping is order independent).
    for (int i = cur; i >= 0; i--) yaml[i + add] = yaml[i];
    for (int i = 0; i < add; i++) yaml[i] = line[i];
    return 1;
}
