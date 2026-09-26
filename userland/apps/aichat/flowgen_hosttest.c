// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// flowgen_hosttest.c - network-independent host unit test for the aichat
// workflow-generation VALIDATION path (flowgen).
//
// Compiled with the SYSTEM gcc + the system libyaml (see the aichat Makefile
// `flowgen-hosttest` target), it drives the SAME flowgen.c extraction +
// validation logic (which calls the runner's own flow.c parser) that the
// MayteraOS aichat binary runs. This proves the KEY robustness requirement
// deterministically, with no OS and no network: a good YAML is accepted; a
// fenced/prose-wrapped YAML is extracted then accepted; malformed YAML, a
// zero-node workflow, and an edge-to-missing-node workflow are all REJECTED.
//
// The flow_plat_* seam is stubbed here (flowgen never executes a flow; only the
// parser is exercised), mirroring flowrun/hosttest.c.

#include <stdio.h>
#include <string.h>

#include "flow.h"
#include "flowgen.h"

// ---- flow.h platform seam (flow.c carries the executor; unused here) -------
int flow_plat_write_file(const char *path, const char *text, int len,
                         int append, char *err, int errcap) {
    (void)path; (void)text; (void)len; (void)append; (void)err; (void)errcap;
    return -1;
}
int flow_plat_llm(const char *system, const char *prompt,
                  char *out, int outcap, char *err, int errcap) {
    (void)system; (void)prompt; (void)err; (void)errcap;
    if (out && outcap > 0) out[0] = 0;
    return -1;
}
int flow_plat_app_invoke(const char *app, int argc, char **argv, char *out, int ocap) {
    (void)app; (void)argc; (void)argv; (void)out; (void)ocap;
    return -1;
}
// #469 AI-VISION: perceive / decide / act seams. This binary never executes a
// flow, so these exist to satisfy the link and refuse, with no message needed.
int flow_plat_capture(const char *target, int rx, int ry, int rw, int rh,
                      int max_w, int max_h, int quality, const char *path,
                      int *out_w, int *out_h, char *err, int errcap) {
    (void)target; (void)rx; (void)ry; (void)rw; (void)rh;
    (void)max_w; (void)max_h; (void)quality; (void)path; (void)err; (void)errcap;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    return -1;
}
int flow_plat_llm_image(const char *system, const char *prompt,
                        const char *image_path,
                        char *out, int outcap, char *err, int errcap) {
    (void)system; (void)prompt; (void)image_path; (void)err; (void)errcap;
    if (out && outcap > 0) out[0] = 0;
    return -1;
}
int flow_plat_input_key(const char *target, int keycode,
                        int hold_ms, int gap_ms, char *err, int errcap) {
    (void)target; (void)keycode; (void)hold_ms; (void)gap_ms; (void)err; (void)errcap;
    return -1;
}

unsigned long flow_plat_now_ms(void) { return 0; }

// ---------------------------------------------------------------------------
// local bounded copy so the test does not depend on strlcpy availability
static void bcpy_test(char *dst, const char *src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else      { printf("  FAIL %s\n", msg); fails++; } } while (0)

static const char GOOD_YAML[] =
"name: greet-file\n"
"nodes:\n"
"  - id: t1\n"
"    type: trigger\n"
"    params: { kind: manual }\n"
"  - id: x1\n"
"    type: transform\n"
"    params: { op: constant, text: \"Maytera Flow OK\" }\n"
"  - id: f1\n"
"    type: file\n"
"    params: { op: write, path: /HOME/OUT.TXT }\n"
"edges:\n"
"  - from: [t1, out]\n"
"    to:   [x1, in]\n"
"  - from: [x1, out]\n"
"    to:   [f1, text]\n";

// The same good YAML as an LLM typically emits it: chatty prose + a ```yaml
// fence. Extraction must recover exactly the YAML body.
static const char FENCED_REPLY[] =
"Sure! Here is a workflow that writes a greeting to a file:\n"
"\n"
"```yaml\n"
"name: greet-file\n"
"nodes:\n"
"  - id: t1\n"
"    type: trigger\n"
"    params: { kind: manual }\n"
"  - id: x1\n"
"    type: transform\n"
"    params: { op: constant, text: \"hi\" }\n"
"  - id: f1\n"
"    type: file\n"
"    params: { op: write, path: /HOME/OUT.TXT }\n"
"edges:\n"
"  - from: [t1, out]\n"
"    to:   [x1, in]\n"
"  - from: [x1, out]\n"
"    to:   [f1, text]\n"
"```\n"
"\n"
"Let me know if you want to change the output path!\n";

// LLM reply with leading prose but NO fence: extraction trims to the first
// top-level key.
static const char PROSE_REPLY[] =
"Here you go:\n"
"name: quicktest\n"
"nodes:\n"
"  - id: t1\n"
"    type: trigger\n"
"  - id: x1\n"
"    type: transform\n"
"    params: { op: constant, text: hey }\n";

static const char ZERO_NODE_YAML[] =
"name: empty\n"
"nodes: []\n";

static const char MISSING_NODE_YAML[] =
"name: broken-edge\n"
"nodes:\n"
"  - id: t1\n"
"    type: trigger\n"
"  - id: x1\n"
"    type: transform\n"
"    params: { op: upper }\n"
"edges:\n"
"  - from: [t1, out]\n"
"    to:   [ghost, in]\n";   // 'ghost' is never defined

// Top level is a sequence, not a mapping: the parser must reject it.
static const char NONMAP_YAML[] =
"- just\n"
"- a\n"
"- list\n";

// Genuinely broken YAML syntax (unbalanced flow collection).
static const char SYNTAX_YAML[] =
"name: x\n"
"nodes: [ { id: a, type: trigger \n";   // unterminated flow mapping/sequence

int main(void) {
    printf("=== flowgen validation host unit test (flowgen) ===\n");
    char err[FLOW_ERR_MAX];
    static flow_graph_t g;
    static char yaml[FLOWGEN_YAML_MAX];

    // 1. Good YAML validates.
    printf("[test] good YAML is accepted\n");
    CHECK(flowgen_validate(GOOD_YAML, (int)strlen(GOOD_YAML), &g, err, sizeof(err)) == 0,
          "GOOD_YAML validates");
    CHECK(g.nnodes == 3, "parsed 3 nodes");
    CHECK(g.nedges == 2, "parsed 2 edges");

    // 2. Fenced/prose-wrapped reply: extract then validate.
    printf("[test] fenced LLM reply extracts and validates\n");
    int n = flowgen_extract_yaml(FENCED_REPLY, yaml, sizeof(yaml));
    CHECK(n > 0, "extracted a non-empty body");
    CHECK(strstr(yaml, "```") == NULL, "extracted body has no fence markers");
    CHECK(strncmp(yaml, "name:", 5) == 0, "extracted body starts at the YAML");
    CHECK(flowgen_validate(yaml, n, &g, err, sizeof(err)) == 0, "extracted YAML validates");

    // 3. Prose (no fence) reply: extraction trims leading prose.
    printf("[test] prose (no fence) reply extracts and validates\n");
    n = flowgen_extract_yaml(PROSE_REPLY, yaml, sizeof(yaml));
    CHECK(strncmp(yaml, "name:", 5) == 0, "trimmed to first top-level key");
    CHECK(flowgen_validate(yaml, n, &g, err, sizeof(err)) == 0, "prose-trimmed YAML validates");

    // 4. Zero-node workflow is REJECTED.
    printf("[test] zero-node workflow is rejected\n");
    CHECK(flowgen_validate(ZERO_NODE_YAML, (int)strlen(ZERO_NODE_YAML), &g, err, sizeof(err)) != 0,
          "ZERO_NODE_YAML rejected");
    printf("       reason: %s\n", err);

    // 5. Edge to a missing node is REJECTED (the semantic check).
    printf("[test] edge to a missing node is rejected\n");
    CHECK(flowgen_validate(MISSING_NODE_YAML, (int)strlen(MISSING_NODE_YAML), &g, err, sizeof(err)) != 0,
          "MISSING_NODE_YAML rejected");
    CHECK(strstr(err, "ghost") != NULL, "error names the undefined node");
    printf("       reason: %s\n", err);

    // 6. Non-mapping top level is REJECTED.
    printf("[test] non-mapping top level is rejected\n");
    CHECK(flowgen_validate(NONMAP_YAML, (int)strlen(NONMAP_YAML), &g, err, sizeof(err)) != 0,
          "NONMAP_YAML rejected");
    printf("       reason: %s\n", err);

    // 7. Broken YAML syntax is REJECTED.
    printf("[test] malformed YAML syntax is rejected\n");
    CHECK(flowgen_validate(SYNTAX_YAML, (int)strlen(SYNTAX_YAML), &g, err, sizeof(err)) != 0,
          "SYNTAX_YAML rejected");
    printf("       reason: %s\n", err);

    // 8. Name helpers.
    printf("[test] name sanitisation + extraction\n");
    char nm[FLOW_ID_MAX];
    flowgen_sanitize_name("My Cool Workflow!! 2", nm, sizeof(nm));
    CHECK(strcmp(nm, "my-cool-workflow-2") == 0, "sanitised 'My Cool Workflow!! 2'");
    flowgen_sanitize_name("   ***   ", nm, sizeof(nm));
    CHECK(strcmp(nm, "workflow") == 0, "all-punctuation -> 'workflow'");
    CHECK(flowgen_name_from_yaml(GOOD_YAML, nm, sizeof(nm)) == 1 && strcmp(nm, "greet-file") == 0,
          "name_from_yaml reads 'greet-file'");

    // 9. Description injection.
    printf("[test] description injection\n");
    bcpy_test(yaml, GOOD_YAML, sizeof(yaml));
    int r = flowgen_ensure_description(yaml, sizeof(yaml), "writes a greeting");
    CHECK(r == 1, "description injected when absent");
    CHECK(strstr(yaml, "description:") != NULL, "buffer now has description:");
    CHECK(flowgen_validate(yaml, (int)strlen(yaml), &g, err, sizeof(err)) == 0,
          "YAML with injected description still validates");
    r = flowgen_ensure_description(yaml, sizeof(yaml), "again");
    CHECK(r == 0, "second call is a no-op (already present)");

    printf("=== %s (%d failure%s) ===\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
