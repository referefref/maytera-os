// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// hosttest.c - network-independent host unit test for the Maytera Flow runner.
//
// Compiled with the SYSTEM gcc + system libyaml (see the Makefile `hosttest`
// target), it drives the SAME flow.c parser + marshaller + executor the
// MayteraOS binary runs. Because a golden that predates this app cannot run
// /APPS/FLOWRUN, this proves the core logic end to end on the build host:
//   1. parse the SHIPPED examples/hello.yml, redirect its file node to a temp
//      path, execute, and assert the output file contains exactly the expected
//      string and that total bytes match.
//   2. assert an unsupported node type fails with an explicit error (no silent
//      success).
//
// The platform seam (flow_plat_*) is implemented here with plain POSIX file I/O
// and a stubbed LLM: the security capability gate lives in main.c and is not
// part of the logic under test here.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "flow.h"
// #469: the SAME key table flow.c compiles against, by explicit relative
// path rather than -I../../libc, so the host build keeps using the SYSTEM
// <string.h>/<stdio.h> and not the freestanding ones next to it.
#include "../../libc/keys.h"

// ---- POSIX platform seam --------------------------------------------------
int flow_plat_write_file(const char *path, const char *text, int len,
                         int append, char *err, int errcap) {
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = open(path, flags, 0644);
    if (fd < 0) { snprintf(err, errcap, "host: cannot open %s", path); return -1; }
    int w = 0;
    while (w < len) { int k = (int)write(fd, text + w, (size_t)(len - w)); if (k <= 0) break; w += k; }
    close(fd);
    if (w != len) { snprintf(err, errcap, "host: short write"); return -1; }
    return w;
}

int flow_plat_llm(const char *system, const char *prompt,
                  char *out, int outcap, char *err, int errcap) {
    (void)system; (void)prompt;
    if (out && outcap > 0) out[0] = 0;
    snprintf(err, errcap, "host: LLM stubbed (network-independent test)");
    return -1;
}

int flow_plat_app_invoke(const char *app, int argc, char **argv, char *out, int ocap) {
    (void)app; (void)argc; (void)argv; (void)out; (void)ocap;
    return -1;
}


// ---- #469 AI-VISION host stubs --------------------------------------------
// The real capture needs the compositor's screenshot queue, the real vision
// call needs the network and an API key, and the real inject needs a consented
// capability grant. None of those belong in a host unit test, so they are
// stubbed HERE, the same way the LLM and app-invoke seams already are. What IS
// under test is everything platform-neutral: the node vocabulary, the parameter
// validation, the key-name tokeniser, the budget arithmetic and the
// perceive -> decide -> act composition inside the agent loop.
//
// The model is SCRIPTED rather than random: it answers with a key and
// goal_met:false for the first (HT_GOAL_AT - 1) calls and goal_met:true on call
// HT_GOAL_AT, so the agent loop must terminate on OBJECTIVE at exactly that
// iteration. A test whose expected answer is "something happened" is not a test.
#define HT_GOAL_AT 3

static int  ht_cap_calls = 0;      // flow_plat_capture calls
static int  ht_vis_calls = 0;      // flow_plat_llm_image calls
static char ht_cap_target[128];
static int  ht_cap_rect[4];
static int  ht_cap_maxw, ht_cap_maxh, ht_cap_q;
static char ht_keys_log[256];      // every injected keycode, "<target>:<code> "
static int  ht_keys_sent = 0;
static int  ht_fail_capture = 0;   // 1 = the capture seam reports a failure

int flow_plat_capture(const char *target, int rx, int ry, int rw, int rh,
                      int max_w, int max_h, int quality, const char *path,
                      int *out_w, int *out_h, char *err, int errcap) {
    ht_cap_calls++;
    snprintf(ht_cap_target, sizeof(ht_cap_target), "%s", target ? target : "");
    ht_cap_rect[0] = rx; ht_cap_rect[1] = ry; ht_cap_rect[2] = rw; ht_cap_rect[3] = rh;
    ht_cap_maxw = max_w; ht_cap_maxh = max_h; ht_cap_q = quality;
    if (ht_fail_capture) {
        snprintf(err, errcap, "host: capture stubbed as FAILING");
        return -1;
    }
    // Write a marker file so the path really is produced and readable, but do
    // NOT pretend it is a JPEG: nothing downstream in this test decodes it, and
    // a fake image that looked real would be the kind of thing that later gets
    // mistaken for a passing capture.
    const char *marker = "host-stub-capture (NOT a real JPEG)\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { snprintf(err, errcap, "host: cannot write %s", path); return -1; }
    int n = (int)write(fd, marker, strlen(marker));
    close(fd);
    if (out_w) *out_w = max_w;
    if (out_h) *out_h = max_h;
    return n;
}

int flow_plat_llm_image(const char *system, const char *prompt,
                        const char *image_path, char *out, int outcap,
                        char *err, int errcap) {
    (void)system; (void)prompt; (void)err; (void)errcap;
    // The image must actually exist: a decide step handed a path to nothing is
    // a wiring bug we want to see, not paper over.
    int fd = open(image_path, O_RDONLY);
    if (fd < 0) { snprintf(err, errcap, "host: no image at %s", image_path); return -1; }
    close(fd);
    ht_vis_calls++;
    if (ht_vis_calls >= HT_GOAL_AT)
        snprintf(out, outcap, "The new game has started.\ngoal_met: true\nkeys: none\n");
    else
        snprintf(out, outcap, "Title screen.\ngoal_met: false\nkeys: ENTER\n");
    return 0;
}

int flow_plat_input_key(const char *target, int keycode,
                        int hold_ms, int gap_ms, char *err, int errcap) {
    (void)hold_ms; (void)gap_ms; (void)err; (void)errcap;
    int l = (int)strlen(ht_keys_log);
    snprintf(ht_keys_log + l, sizeof(ht_keys_log) - l, "%s:0x%02X ",
             target ? target : "?", (unsigned)keycode);
    ht_keys_sent++;
    return 0;
}

unsigned long flow_plat_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
}

// ---------------------------------------------------------------------------
static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else      { printf("  FAIL %s\n", msg); fails++; } } while (0)

static int test_hello(const char *yml) {
    printf("[test] deterministic hello.yml end to end\n");
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    if (flow_parse_file(yml, &g, err, sizeof(err)) != 0) {
        printf("  FAIL parse %s: %s\n", yml, err); fails++; return 1;
    }
    CHECK(g.nnodes == 3, "parsed 3 nodes");
    CHECK(g.nedges == 2, "parsed 2 edges");

    // Redirect the file node from /HOME/... to a host temp path so we can read
    // it back. This tests the SHIPPED graph and output text; only the sink path
    // is host-local.
    const char *out_path = "/tmp/flowrun_hosttest.out";
    unlink(out_path);
    for (int i = 0; i < g.nnodes; i++) {
        if (strcmp(g.nodes[i].type, "file") != 0) continue;
        for (int p = 0; p < g.nodes[i].nparams; p++)
            if (strcmp(g.nodes[i].params[p].key, "path") == 0)
                snprintf(g.nodes[i].params[p].val, FLOW_VAL_MAX, "%s", out_path);
    }

    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc == 0, "flow_execute returned 0");
    if (rc != 0) { printf("  (err: %s)\n", ctx.err); return 1; }

    const char *expect = "Maytera Flow M1 OK";
    CHECK(ctx.total_written == (long)strlen(expect), "total_written == strlen(expected)");

    char buf[256] = {0};
    int fd = open(out_path, O_RDONLY);
    int n = (fd >= 0) ? (int)read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n >= 0) buf[n] = 0;
    CHECK(n == (int)strlen(expect) && strcmp(buf, expect) == 0,
          "output file contains exactly the expected text");
    printf("  wrote \"%s\" (%d bytes) to %s\n", buf, n, out_path);
    return 0;
}

static int test_unsupported(void) {
    printf("[test] unsupported node type errors (no silent success)\n");
    // `device` is still not implemented (agent/subloop landed in M3), so it must
    // still fail with an explicit error, never a silent success. Its
    // promise/budget fields are still parsed into the data model.
    const char *yml =
        "name: bad\n"
        "nodes:\n"
        "  - id: t1\n"
        "    type: trigger\n"
        "    params: { kind: manual }\n"
        "  - id: a1\n"
        "    type: device\n"
        "    objective: do a thing\n"
        "    max_iters: 5\n"
        "edges:\n"
        "  - from: [t1, out]\n"
        "    to: [a1, in]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of device workflow succeeds");
    CHECK(g.nnodes == 2, "parsed 2 nodes (incl. device)");
    // the device node's data-model fields must be parsed but not executed
    int found = 0;
    for (int i = 0; i < g.nnodes; i++)
        if (strcmp(g.nodes[i].type, "device") == 0) {
            found = 1;
            CHECK(g.nodes[i].has_loop == 1 && g.nodes[i].max_iters == 5,
                  "device node's objective/max_iters parsed into the model");
        }
    CHECK(found, "device node present");

    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc != 0, "flow_execute FAILS on the device node");
    CHECK(strstr(ctx.err, "unsupported node type") != NULL,
          "error message says 'unsupported node type'");
    printf("  (err: %s)\n", ctx.err);
    return 0;
}

// Read a named output port of a node into out[]; returns 1 if present.
static int read_output(flow_node_t *n, const char *port, char *out, int cap) {
    for (int p = 0; p < n->noutputs; p++)
        if (strcmp(n->outputs[p].port, port) == 0) {
            flow_value_as_str(&n->outputs[p].val, out, cap);
            return 1;
        }
    return 0;
}

static flow_node_t *find_node(flow_graph_t *g, const char *id) {
    for (int i = 0; i < g->nnodes; i++)
        if (strcmp(g->nodes[i].id, id) == 0) return &g->nodes[i];
    return 0;
}

// M3: agent-promise loop, OBJECTIVE termination. A deterministic decide node (a
// transform threshold on the agent's iteration index) sets goal_met true once
// index >= 2, i.e. on the 3rd pass. Asserts done=objective and iterations=3.
static int test_agent_objective(void) {
    printf("[test] agent-promise: OBJECTIVE (goal_met after N passes)\n");
    const char *yml =
        "name: agentobj\n"
        "nodes:\n"
        "  - id: t1\n"
        "    type: trigger\n"
        "    params: { kind: manual }\n"
        "  - id: ag\n"
        "    type: agent\n"
        "    objective: reach the goal\n"
        "    max_iters: 50\n"
        "    max_minutes: 60\n"
        "  - id: decide\n"
        "    type: transform\n"
        "    params: { op: gte, value: 2, loop: ag }\n"
        "edges:\n"
        "  - from: [t1, out]\n"
        "    to:   [ag, in]\n"
        "  - from: [ag, index]\n"
        "    to:   [decide, in]\n"
        "  - from: [decide, out]\n"
        "    to:   [ag, goal_met]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of agent objective workflow succeeds");
    if (prc != 0) { printf("  (parse err: %s)\n", err); return 1; }
    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc == 0, "flow_execute returned 0");
    if (rc != 0) { printf("  (err: %s)\n", ctx.err); return 1; }
    flow_node_t *ag = find_node(&g, "ag");
    char done[64] = {0}, iters[64] = {0};
    int gd = ag && read_output(ag, "done", done, sizeof(done));
    int gi = ag && read_output(ag, "iterations", iters, sizeof(iters));
    CHECK(gd && strcmp(done, "objective") == 0, "done == \"objective\"");
    CHECK(gi && strcmp(iters, "3") == 0, "iterations == 3 (goal_met at index 2)");
    printf("  done=%s iterations=%s\n", done, iters);
    return 0;
}

// M3: agent-promise loop, MEASURED termination. A perceive node passes the
// agent's iteration index through to the agent's metric input; success is
// "metric >= 3", so it crosses on the pass where index == 3, i.e. iterations 4.
static int test_agent_measured(void) {
    printf("[test] agent-promise: MEASURED (metric crosses success threshold)\n");
    const char *yml =
        "name: agentmeas\n"
        "nodes:\n"
        "  - id: t1\n"
        "    type: trigger\n"
        "    params: { kind: manual }\n"
        "  - id: ag\n"
        "    type: agent\n"
        "    objective: raise the metric\n"
        "    success: \"metric >= 3\"\n"
        "    max_iters: 50\n"
        "  - id: perceive\n"
        "    type: transform\n"
        "    params: { op: concat, loop: ag }\n"
        "edges:\n"
        "  - from: [t1, out]\n"
        "    to:   [ag, in]\n"
        "  - from: [ag, index]\n"
        "    to:   [perceive, in]\n"
        "  - from: [perceive, out]\n"
        "    to:   [ag, metric]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of agent measured workflow succeeds");
    if (prc != 0) { printf("  (parse err: %s)\n", err); return 1; }
    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc == 0, "flow_execute returned 0");
    if (rc != 0) { printf("  (err: %s)\n", ctx.err); return 1; }
    flow_node_t *ag = find_node(&g, "ag");
    char done[64] = {0}, iters[64] = {0};
    int gd = ag && read_output(ag, "done", done, sizeof(done));
    int gi = ag && read_output(ag, "iterations", iters, sizeof(iters));
    CHECK(gd && strcmp(done, "measured") == 0, "done == \"measured\"");
    CHECK(gi && strcmp(iters, "4") == 0, "iterations == 4 (metric crosses at index 3)");
    printf("  done=%s iterations=%s\n", done, iters);
    return 0;
}

// M3: agent-promise loop, BUDGET termination. No objective wiring and no metric,
// so the loop runs to its max_iters budget. Asserts done=max_iters, iterations=4.
static int test_agent_maxiters(void) {
    printf("[test] agent-promise: BUDGET (max_iters reached)\n");
    const char *yml =
        "name: agentbudget\n"
        "nodes:\n"
        "  - id: t1\n"
        "    type: trigger\n"
        "    params: { kind: manual }\n"
        "  - id: ag\n"
        "    type: agent\n"
        "    objective: try forever\n"
        "    max_iters: 4\n"
        "  - id: act\n"
        "    type: transform\n"
        "    params: { op: constant, text: tick, loop: ag }\n"
        "edges:\n"
        "  - from: [t1, out]\n"
        "    to:   [ag, in]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of agent budget workflow succeeds");
    if (prc != 0) { printf("  (parse err: %s)\n", err); return 1; }
    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc == 0, "flow_execute returned 0");
    if (rc != 0) { printf("  (err: %s)\n", ctx.err); return 1; }
    flow_node_t *ag = find_node(&g, "ag");
    char done[64] = {0}, iters[64] = {0}, toks[64] = {0};
    int gd = ag && read_output(ag, "done", done, sizeof(done));
    int gi = ag && read_output(ag, "iterations", iters, sizeof(iters));
    int gt = ag && read_output(ag, "tokens", toks, sizeof(toks));
    CHECK(gd && strcmp(done, "max_iters") == 0, "done == \"max_iters\"");
    CHECK(gi && strcmp(iters, "4") == 0, "iterations == 4 (budget)");
    CHECK(gt && strcmp(toks, "4") == 0, "tokens == 4 (iteration proxy)");
    printf("  done=%s iterations=%s tokens=%s\n", done, iters, toks);
    return 0;
}

// M3: async sub-loop, AWAIT path. Runs the sub-body to completion and joins.
// Asserts the joined flag and the collected result.
static int test_subloop_await(void) {
    printf("[test] async sub-loop: AWAIT (run to completion + join)\n");
    const char *yml =
        "name: subaw\n"
        "nodes:\n"
        "  - id: t1\n"
        "    type: trigger\n"
        "    params: { kind: manual }\n"
        "  - id: sl\n"
        "    type: subloop\n"
        "    mode: await\n"
        "    every_ms: 500\n"
        "    params: { max_conc: 2 }\n"
        "  - id: work\n"
        "    type: transform\n"
        "    params: { op: constant, text: \"sub-body result\", loop: sl }\n"
        "edges:\n"
        "  - from: [t1, out]\n"
        "    to:   [sl, in]\n"
        "  - from: [work, out]\n"
        "    to:   [sl, collect]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of subloop await workflow succeeds");
    if (prc != 0) { printf("  (parse err: %s)\n", err); return 1; }
    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc == 0, "flow_execute returned 0");
    if (rc != 0) { printf("  (err: %s)\n", ctx.err); return 1; }
    flow_node_t *sl = find_node(&g, "sl");
    char joined[64] = {0}, result[FLOW_VAL_MAX] = {0};
    int gj = sl && read_output(sl, "joined", joined, sizeof(joined));
    int gr = sl && read_output(sl, "result", result, sizeof(result));
    CHECK(gj && strcmp(joined, "true") == 0, "joined == true");
    CHECK(gr && strcmp(result, "sub-body result") == 0,
          "result == \"sub-body result\" (collected from the sub-body)");
    printf("  joined=%s result=\"%s\"\n", joined, result);
    return 0;
}

// M2: the FOR-EACH executor, host-tested end to end with a constant list and a
// per-item transform, asserting the exact collected output. Deterministic and
// network-independent: the body is a single upper-case transform, and the loop
// collects each iteration's result back through the loop node's "collect" port.
static int test_foreach(void) {
    printf("[test] foreach loop: constant list -> per-item transform -> results\n");
    const char *yml =
        "name: loopdemo\n"
        "nodes:\n"
        "  - id: t1\n"
        "    type: trigger\n"
        "    params: { kind: manual }\n"
        "  - id: lp\n"
        "    type: foreach\n"
        "    params: { items: \"alpha,beta,gamma\", max_iters: 100 }\n"
        "  - id: up\n"
        "    type: transform\n"
        "    params: { op: upper, loop: lp }\n"
        "edges:\n"
        "  - from: [t1, out]\n"
        "    to:   [lp, in]\n"
        "  - from: [lp, item]\n"
        "    to:   [up, in]\n"
        "  - from: [up, out]\n"
        "    to:   [lp, collect]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of foreach workflow succeeds");
    if (prc != 0) { printf("  (parse err: %s)\n", err); return 1; }
    CHECK(g.nnodes == 3, "parsed 3 nodes (trigger, foreach, body transform)");

    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc == 0, "flow_execute returned 0");
    if (rc != 0) { printf("  (err: %s)\n", ctx.err); return 1; }

    // Find the loop node and read its collected output ports directly.
    flow_node_t *lp = 0;
    for (int i = 0; i < g.nnodes; i++)
        if (strcmp(g.nodes[i].type, "foreach") == 0) lp = &g.nodes[i];
    CHECK(lp != NULL, "foreach node present");
    char res[FLOW_VAL_MAX] = {0}, cnt[FLOW_VAL_MAX] = {0};
    int got_res = 0, got_cnt = 0;
    if (lp) {
        for (int p = 0; p < lp->noutputs; p++) {
            if (strcmp(lp->outputs[p].port, "results") == 0) {
                flow_value_as_str(&lp->outputs[p].val, res, sizeof(res)); got_res = 1;
            } else if (strcmp(lp->outputs[p].port, "count") == 0) {
                flow_value_as_str(&lp->outputs[p].val, cnt, sizeof(cnt)); got_cnt = 1;
            }
        }
    }
    CHECK(got_res && strcmp(res, "ALPHA,BETA,GAMMA") == 0,
          "results == \"ALPHA,BETA,GAMMA\" (3 items, per-item upper, sep-joined)");
    CHECK(got_cnt && strcmp(cnt, "3") == 0, "count == 3");
    printf("  results=\"%s\" count=%s\n", res, cnt);
    return 0;
}

// A malformed graph with a top-level cycle must FAIL CLEANLY (never crash, hang,
// or silently succeed). Two transforms feeding each other have no zero-in-degree
// node, so Kahn processes nothing and the executor must report a cycle. This
// guards flow_execute()'s cycle detection across the index-cache refactor.
static int test_cycle(void) {
    printf("[test] top-level cycle fails cleanly (not a DAG)\n");
    const char *yml =
        "name: cyc\n"
        "nodes:\n"
        "  - id: a\n"
        "    type: transform\n"
        "    params: { op: upper }\n"
        "  - id: b\n"
        "    type: transform\n"
        "    params: { op: upper }\n"
        "edges:\n"
        "  - from: [a, out]\n"
        "    to:   [b, in]\n"
        "  - from: [b, out]\n"
        "    to:   [a, in]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of cyclic workflow succeeds");
    if (prc != 0) { printf("  (parse err: %s)\n", err); return 1; }
    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc != 0, "flow_execute FAILS on a top-level cycle");
    CHECK(strstr(ctx.err, "cycle") != NULL, "error message says 'cycle'");
    printf("  (err: %s)\n", ctx.err);
    return 0;
}

// A cycle AMONG loop-body nodes must also fail cleanly, exercising the
// per-iteration run_loop_body() scheduler (the hottest path, and the one the
// index-cache refactor changed). The foreach iterates once, then its body's
// Kahn schedule finds no zero-in-degree node and reports a body cycle.
static int test_loopbody_cycle(void) {
    printf("[test] loop-body cycle fails cleanly (not a DAG)\n");
    const char *yml =
        "name: bodycyc\n"
        "nodes:\n"
        "  - id: t1\n"
        "    type: trigger\n"
        "    params: { kind: manual }\n"
        "  - id: lp\n"
        "    type: foreach\n"
        "    params: { items: \"x\" }\n"
        "  - id: a\n"
        "    type: transform\n"
        "    params: { op: upper, loop: lp }\n"
        "  - id: b\n"
        "    type: transform\n"
        "    params: { op: upper, loop: lp }\n"
        "edges:\n"
        "  - from: [t1, out]\n"
        "    to:   [lp, in]\n"
        "  - from: [a, out]\n"
        "    to:   [b, in]\n"
        "  - from: [b, out]\n"
        "    to:   [a, in]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of loop-body cycle workflow succeeds");
    if (prc != 0) { printf("  (parse err: %s)\n", err); return 1; }
    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc != 0, "flow_execute FAILS on a loop-body cycle");
    CHECK(strstr(ctx.err, "cycle") != NULL, "error message says 'cycle'");
    printf("  (err: %s)\n", ctx.err);
    return 0;
}


// ---------------------------------------------------------------------------
// #469 AI-VISION: the perceive -> decide -> act loop, end to end.
//
// capture -> llm(image) -> input, all three inside the existing agent node, the
// decide step's goal_met wired back to the agent. With the scripted model above
// the objective is met on pass HT_GOAL_AT, so the loop must stop there and NOT
// run to max_iters. Asserts the whole composition: three captures, three vision
// calls, two injected keys (the third pass answers "none"), the right keycode,
// and done=objective.
static int test_vision_agent(void) {
    printf("[test] #469 vision agent: capture -> llm(image) -> input, objective\n");
    ht_cap_calls = ht_vis_calls = ht_keys_sent = 0;
    ht_keys_log[0] = 0;
    ht_fail_capture = 0;
    const char *yml =
        "name: visionagent\n"
        "nodes:\n"
        "  - id: t1\n"
        "    type: trigger\n"
        "    params: { kind: manual }\n"
        "  - id: ag\n"
        "    type: agent\n"
        "    objective: a new game has started\n"
        "    max_iters: 8\n"
        "  - id: shot\n"
        "    type: capture\n"
        "    params:\n"
        "      loop: ag\n"
        "      target: \"window:Game\"\n"
        "      rect: \"16,40,320,288\"\n"
        "      max_width: 480\n"
        "      max_height: 480\n"
        "      quality: 70\n"
        "      path: /tmp/flowrun_vision_test.jpg\n"
        "  - id: see\n"
        "    type: llm\n"
        "    params:\n"
        "      loop: ag\n"
        "      max_calls: 8\n"
        "      system: You are driving an app.\n"
        "      prompt: 'Look at {image} and answer.'\n"
        "  - id: press\n"
        "    type: input\n"
        "    params:\n"
        "      loop: ag\n"
        "      target: \"window:Game\"\n"
        "      after: 'keys:'\n"
        "      hold_ms: 0\n"
        "      gap_ms: 0\n"
        "edges:\n"
        "  - from: [t1, out]\n"
        "    to:   [ag, in]\n"
        "  - from: [shot, image]\n"
        "    to:   [see, image]\n"
        "  - from: [see, out]\n"
        "    to:   [press, keys]\n"
        "  - from: [see, goal_met]\n"
        "    to:   [ag, goal_met]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of the vision agent workflow succeeds");
    if (prc != 0) { printf("  (parse err: %s)\n", err); return 1; }

    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc == 0, "flow_execute returns 0");
    if (rc != 0) { printf("  (err: %s)\n", ctx.err); return 1; }

    flow_node_t *ag = find_node(&g, "ag");
    char done[64] = {0}, iters[64] = {0};
    read_output(ag, "done", done, sizeof(done));
    read_output(ag, "iterations", iters, sizeof(iters));
    CHECK(strcmp(done, "objective") == 0, "agent stopped on OBJECTIVE, not the budget");
    CHECK(atoi(iters) == HT_GOAL_AT, "agent ran exactly HT_GOAL_AT passes");
    CHECK(ht_cap_calls == HT_GOAL_AT, "capture ran once per pass");
    CHECK(ht_vis_calls == HT_GOAL_AT, "the model was shown the image once per pass");
    // The capture node's parameters reached the platform seam intact.
    CHECK(strcmp(ht_cap_target, "window:Game") == 0, "target reached the capture seam");
    CHECK(ht_cap_rect[0] == 16 && ht_cap_rect[1] == 40 &&
          ht_cap_rect[2] == 320 && ht_cap_rect[3] == 288, "rect parsed to 16,40,320,288");
    CHECK(ht_cap_maxw == 480 && ht_cap_maxh == 480 && ht_cap_q == 70,
          "max_width/max_height/quality reached the capture seam");
    // Two presses: passes 1 and 2 answered "ENTER"; pass 3 answered "none",
    // which is not a key name and is skipped rather than guessed at.
    CHECK(ht_keys_sent == 2, "two keys injected (the final pass pressed nothing)");
    CHECK(strstr(ht_keys_log, "window:Game:0x0A") != NULL,
          "the injected keycode is GUI_KEY_ENTER (0x0A) from the shared keys.h");
    printf("  keys: %s\n", ht_keys_log);
    printf("  done=%s iterations=%s captures=%d vision=%d\n",
           done, iters, ht_cap_calls, ht_vis_calls);
    return 0;
}

// The vision BUDGET is real: an llm node with max_calls below what the loop
// needs must FAIL the run, loudly, not quietly stop perceiving.
static int test_vision_budget(void) {
    printf("[test] #469 vision budget: max_calls is enforced and fails closed\n");
    ht_cap_calls = ht_vis_calls = ht_keys_sent = 0;
    ht_keys_log[0] = 0;
    ht_fail_capture = 0;
    const char *yml =
        "name: visionbudget\n"
        "nodes:\n"
        "  - id: ag\n"
        "    type: agent\n"
        "    max_iters: 8\n"
        "  - id: shot\n"
        "    type: capture\n"
        "    params: { loop: ag, target: screen, path: /tmp/flowrun_vision_test.jpg }\n"
        "  - id: see\n"
        "    type: llm\n"
        "    params: { loop: ag, max_calls: 2, prompt: 'look' }\n"
        "edges:\n"
        "  - from: [shot, image]\n"
        "    to:   [see, image]\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    int prc = flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err));
    CHECK(prc == 0, "parse of the budget workflow succeeds");
    if (prc != 0) { printf("  (parse err: %s)\n", err); return 1; }
    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc != 0, "flow_execute FAILS once the vision budget is spent");
    CHECK(strstr(ctx.err, "vision budget exhausted") != NULL,
          "the error names the exhausted budget");
    CHECK(ht_vis_calls == 2, "exactly max_calls vision calls were made, no more");
    printf("  (err: %s)\n", ctx.err);
    return 0;
}

// A malformed rect is an ERROR, not a silently-ignored crop: a flow that
// thought it was photographing a 160x144 LCD and actually photographed the
// whole desktop would produce a plausible-looking wrong answer.
static int test_capture_badrect(void) {
    printf("[test] #469 capture: a malformed rect fails, it is not ignored\n");
    ht_cap_calls = 0;
    const char *yml =
        "name: badrect\n"
        "nodes:\n"
        "  - id: shot\n"
        "    type: capture\n"
        "    params: { target: screen, rect: \"16,40,320\", path: /tmp/x.jpg }\n";
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    CHECK(flow_parse_bytes(yml, (int)strlen(yml), &g, err, sizeof(err)) == 0, "parse succeeds");
    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc != 0, "flow_execute FAILS on a three-number rect");
    CHECK(strstr(ctx.err, "rect must be") != NULL, "the error says what a rect must look like");
    CHECK(ht_cap_calls == 0, "the capture seam was never reached");
    printf("  (err: %s)\n", ctx.err);
    return 0;
}

// The key-name vocabulary is generic and shared. Nothing here knows what a
// Game Boy is: these are the OS's own keycodes from libc/keys.h.
static int test_keynames(void) {
    printf("[test] #469 key names resolve to the SHARED keys.h codes\n");
    CHECK(flow_keycode_of("ENTER")  == GUI_KEY_ENTER, "ENTER  -> GUI_KEY_ENTER");
    CHECK(flow_keycode_of("enter")  == GUI_KEY_ENTER, "case insensitive");
    CHECK(flow_keycode_of("DOWN")   == GUI_KEY_DOWN,  "DOWN   -> GUI_KEY_DOWN");
    CHECK(flow_keycode_of("RSHIFT") == GUI_KEY_RSHIFT,"RSHIFT -> GUI_KEY_RSHIFT");
    CHECK(flow_keycode_of("x")      == 'x',           "a single character is itself");
    CHECK(flow_keycode_of("0x5A")   == 0x5A,          "0xNN is an explicit keycode");
    CHECK(flow_keycode_of("press")  == -1,            "a word that is not a key is refused");
    CHECK(flow_keycode_of("")       == -1,            "empty is refused");
    return 0;
}


// The SHIPPED example flow, parsed and executed end to end against the scripted
// model. This is the one that stops examples/visiongame.yml rotting: a wrong
// port name, a renamed param or a broken edge would all still parse as valid
// YAML and then quietly do nothing on a VM, which is the failure mode this
// project has hit most often. Only the two output PATHS are redirected (the
// host has no /HOME); the graph, the ports and the budgets are the shipped ones.
static int test_example_visiongame(void) {
    printf("[test] #469 the SHIPPED examples/visiongame.yml runs end to end\n");
    ht_cap_calls = ht_vis_calls = ht_keys_sent = 0;
    ht_keys_log[0] = 0;
    ht_fail_capture = 0;
    static flow_graph_t g;
    char err[FLOW_ERR_MAX] = {0};
    if (flow_parse_file("examples/visiongame.yml", &g, err, sizeof(err)) != 0) {
        printf("  FAIL parse examples/visiongame.yml: %s\n", err); fails++; return 1;
    }
    CHECK(g.nnodes == 6, "6 nodes (trigger, agent, capture, llm, input, file)");
    CHECK(g.nedges == 5, "5 edges");

    const char *shot = "/tmp/flowrun_visiongame.jpg";
    const char *res  = "/tmp/flowrun_visiongame.txt";
    unlink(res);
    for (int i = 0; i < g.nnodes; i++)
        for (int p = 0; p < g.nodes[i].nparams; p++)
            if (strcmp(g.nodes[i].params[p].key, "path") == 0)
                snprintf(g.nodes[i].params[p].val, FLOW_VAL_MAX, "%s",
                         strcmp(g.nodes[i].type, "capture") == 0 ? shot : res);

    flow_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.g = &g;
    int rc = flow_execute(&ctx);
    CHECK(rc == 0, "flow_execute returns 0");
    if (rc != 0) { printf("  (err: %s)\n", ctx.err); return 1; }

    flow_node_t *ag = find_node(&g, "ag");
    char done[64] = {0};
    read_output(ag, "done", done, sizeof(done));
    CHECK(strcmp(done, "objective") == 0, "the shipped flow terminates on OBJECTIVE");
    CHECK(ht_vis_calls <= 8, "AT MOST 8 vision calls, the stated budget");
    CHECK(ht_cap_calls == ht_vis_calls, "one capture per vision call");
    CHECK(ht_keys_sent > 0, "keys were actually injected (the act step is wired)");
    CHECK(strstr(ht_cap_target, "GBEMU") != NULL, "the capture targets the emulator");
    // The verdict really lands in a file, which is what a headless reader checks.
    char buf[64] = {0};
    int fd = open(res, O_RDONLY);
    int n = (fd >= 0) ? (int)read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n > 0) buf[n] = 0;
    CHECK(n > 0 && strcmp(buf, "objective") == 0, "the result file holds the verdict");
    printf("  done=%s vision=%d captures=%d keys=%d result=\"%s\"\n",
           done, ht_vis_calls, ht_cap_calls, ht_keys_sent, buf);
    return 0;
}

int main(int argc, char **argv) {
    const char *yml = (argc > 1) ? argv[1] : "examples/hello.yml";
    printf("flowrun host unit test\n");
    test_hello(yml);
    test_unsupported();
    test_foreach();
    test_agent_objective();
    test_agent_measured();
    test_agent_maxiters();
    test_subloop_await();
    test_cycle();
    test_loopbody_cycle();
    test_keynames();
    test_capture_badrect();
    test_vision_agent();
    test_vision_budget();
    test_example_visiongame();
    if (fails == 0) { printf("HOSTTEST-OK all checks passed\n"); return 0; }
    printf("HOSTTEST-FAIL %d check(s) failed\n", fails);
    return 1;
}
