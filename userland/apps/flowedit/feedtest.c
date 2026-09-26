// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// feedtest.c - network-independent host unit test for the Maytera Flow feed
// client (#153). Compiled with the SYSTEM gcc + system libyaml (see the
// flowedit Makefile `feedtest` target), it drives the SAME logic the shipping
// /APPS/FLOW binary runs for the "Download example flows" path, MINUS the parts
// that need the live OS (HTTP fetch and the kernel signature check):
//
//   1. flowfeed_parse_manifest(): a known-good manifest yields the right
//      entries; an entry with a missing/short sha256 is DROPPED (never trusted);
//      a manifest with no flows[] array is rejected.
//   2. flowfeed_sanitize_name(): good names pass; traversal / slash / dotfile /
//      whitespace names are folded or refused (defence in depth on the install
//      path).
//   3. THE INTEGRITY GATE the install path uses: sha256(flow bytes) compared to
//      the manifest's per-entry hex. This is exactly what pkgsig_verify_package()
//      does (sha256 then hex compare, userland/libc/pkgsig.c); it is reproduced
//      here against the real sha256.c so the good/bad decision is proven.
//   4. flow_parse_bytes(): the runner's own parser accepts a valid flow and
//      REJECTS a truncated one, i.e. the "validate before install" guarantee.
//
// The live path (manifest SIGNATURE via pkgsig_verify_manifest -> kernel
// SYS_OTA_VERIFY_SIG, and the HTTP fetch) needs the OS + a server and is
// covered by code review, not here.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

#include "flowfeed.h"
#include "flow.h"
#include "sha256.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); fails++; } \
} while (0)

// ---- POSIX platform seam for flow.c (same as flowrun/hosttest.c) -----------
int flow_plat_write_file(const char *path, const char *text, int len,
                         int append, char *err, int errcap) {
    (void)path; (void)text; (void)append;
    if (err && errcap > 0) err[0] = 0;
    return len;   // feed validation never executes; parse-only
}
int flow_plat_llm(const char *s, const char *p, char *o, int oc, char *e, int ec) {
    (void)s; (void)p; if (o && oc) o[0] = 0; if (e && ec) snprintf(e, ec, "stub"); return -1;
}
int flow_plat_app_invoke(const char *a, int c, char **v, char *o, int oc) {
    (void)a; (void)c; (void)v; (void)o; (void)oc; return -1;
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

unsigned long flow_plat_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
}

static void hex_of(const uint8_t *d, int n, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i*2] = h[d[i] >> 4]; out[i*2+1] = h[d[i] & 15]; }
    out[n*2] = 0;
}

// Mirror of pkgsig_verify_package(): sha256 then hex compare. 0 == verified.
static int integrity_ok(const void *data, int len, const char *want_hex) {
    if (!want_hex || strlen(want_hex) != 64) return -1;
    uint8_t d[32]; char got[65];
    sha256(data, (size_t)len, d);
    hex_of(d, 32, got);
    for (int i = 0; i < 64; i++) {
        char a = got[i], b = want_hex[i];
        if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
        if (a != b) return -1;
    }
    return 0;
}

static const char *GOOD_FLOW =
    "name: web-to-file\n"
    "description: \"Fetch example\"\n"
    "nodes:\n"
    "  - id: t1\n"
    "    type: trigger\n"
    "    params: { kind: manual }\n"
    "  - id: x1\n"
    "    type: transform\n"
    "    params: { op: upper }\n"
    "edges:\n"
    "  - from: [t1, out]\n"
    "    to:   [x1, in]\n";

int main(void) {
    printf("flowfeed host unit test\n");

    // 1. Manifest parse: two good entries, one entry with a bad (short) hash.
    char goodhex[65]; {
        uint8_t d[32]; sha256(GOOD_FLOW, strlen(GOOD_FLOW), d); hex_of(d, 32, goodhex);
    }
    char manifest[2048];
    snprintf(manifest, sizeof(manifest),
        "{ \"feed\": \"maytera-flows\", \"version\": 1, \"flows\": [\n"
        "  { \"name\": \"web-to-file\", \"description\": \"Fetch a URL to a file\",\n"
        "    \"path\": \"flows/web-to-file.yml\", \"sha256\": \"%s\" },\n"
        "  { \"name\": \"nohash\", \"description\": \"missing hash\",\n"
        "    \"path\": \"flows/nohash.yml\", \"sha256\": \"deadbeef\" },\n"
        "  { \"name\": \"greet\", \"description\": \"say hi\",\n"
        "    \"path\": \"flows/greet.yml\", \"sha256\": \"%064d\" }\n"
        "] }\n", goodhex, 0);

    flowfeed_entry_t ents[FLOWFEED_MAX];
    char err[128];
    int n = flowfeed_parse_manifest(manifest, (int)strlen(manifest), ents, FLOWFEED_MAX, err, sizeof(err));
    CHECK(n == 2, "parse keeps 2 usable entries, drops the short-sha256 one");
    if (n >= 1) {
        CHECK(strcmp(ents[0].name, "web-to-file") == 0, "entry 0 name parsed");
        CHECK(strcmp(ents[0].path, "flows/web-to-file.yml") == 0, "entry 0 path parsed");
        CHECK(strcmp(ents[0].desc, "Fetch a URL to a file") == 0, "entry 0 description parsed");
        CHECK(strcmp(ents[0].sha256, goodhex) == 0, "entry 0 sha256 parsed");
    }
    if (n >= 2) CHECK(strcmp(ents[1].name, "greet") == 0, "entry 1 (after the dropped one) is greet");

    // 2. No flows[] array -> rejected.
    const char *bad = "{ \"feed\": \"maytera-flows\", \"packages\": [] }";
    int r = flowfeed_parse_manifest(bad, (int)strlen(bad), ents, FLOWFEED_MAX, err, sizeof(err));
    CHECK(r == -1, "manifest with no flows[] is rejected");

    // 3. sanitize_name.
    char nm[64];
    CHECK(flowfeed_sanitize_name("web-to-file", nm, sizeof(nm)) == 0 && strcmp(nm, "web-to-file") == 0, "good name passes unchanged");
    CHECK(flowfeed_sanitize_name("my flow v2", nm, sizeof(nm)) == 0 && strcmp(nm, "my-flow-v2") == 0, "spaces fold to single dash");
    CHECK(flowfeed_sanitize_name("../../etc/passwd", nm, sizeof(nm)) == -1, "path traversal refused");
    CHECK(flowfeed_sanitize_name("a/b", nm, sizeof(nm)) == 0 && strcmp(nm, "a-b") == 0, "embedded slash folds to dash (no separator survives)");
    CHECK(flowfeed_sanitize_name(".hidden", nm, sizeof(nm)) == -1, "leading-dot dotfile refused");
    CHECK(flowfeed_sanitize_name("", nm, sizeof(nm)) == -1, "empty name refused");

    // 4. Integrity gate (== pkgsig_verify_package): good matches, tamper fails.
    CHECK(integrity_ok(GOOD_FLOW, (int)strlen(GOOD_FLOW), goodhex) == 0, "sha256 of flow matches manifest hash");
    {
        char tampered[512]; snprintf(tampered, sizeof(tampered), "%sX", GOOD_FLOW);
        CHECK(integrity_ok(tampered, (int)strlen(tampered), goodhex) != 0, "tampered flow FAILS the sha256 gate");
    }

    // 5. Validate-before-install: parser accepts good, rejects truncated.
    {
        static flow_graph_t g; char e2[FLOW_ERR_MAX] = {0};
        int prc = flow_parse_bytes(GOOD_FLOW, (int)strlen(GOOD_FLOW), &g, e2, sizeof(e2));
        CHECK(prc == 0, "flow_parse_bytes accepts a valid flow");
        const char *trunc = "name: x\nnodes: [ { id: a, type: trigger";
        memset(&g, 0, sizeof(g)); e2[0] = 0;
        int trc = flow_parse_bytes(trunc, (int)strlen(trunc), &g, e2, sizeof(e2));
        CHECK(trc != 0, "flow_parse_bytes REJECTS a truncated flow (no hang, no install)");
    }

    if (fails == 0) { printf("HOSTTEST-OK all checks passed\n"); return 0; }
    printf("HOSTTEST-FAIL %d check(s) failed\n", fails);
    return 1;
}
