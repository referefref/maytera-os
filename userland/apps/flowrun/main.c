// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// flowrun - Maytera Flow workflow RUNNER, Milestone 1 (headless executor).
//
// Usage:  /APPS/FLOWRUN <workflow-name>        (or a full path to a .yml)
//
// Reads /CONFIG/WORKFLOWS/<name>.yml, parses it into an in-memory node+edge
// graph, and executes the graph in topological order through a node-executor
// registry. It prints exactly ONE machine-readable result line to stdout (which
// the kernel also mirrors to the serial console), so a headless VM can read the
// verdict:
//     FLOWRUN-OK   <bytes-written>
//     FLOWRUN-FAIL <reason>
// with a non-zero exit on any failure. Per-node progress goes to stderr.
//
// The visual editor is a separate later milestone; this is the runner only.
//
// ===========================================================================
// WORKFLOW YAML SCHEMA (M1)   -- authoritative copy in docs/WORKFLOW_YAML_SCHEMA.md
// ===========================================================================
//
//   name: <string>                 # optional workflow name (display only)
//   nodes:                         # required, a list
//     - id: <string>               # required, unique within the workflow
//       type: <string>             # required, see the node types below
//       params: { <key>: <scalar>, ... }   # optional, per-type parameters
//       # optional loop/agent/subloop fields (PARSED into the model, NOT run
//       # in M1): objective, success, max_iters, max_minutes, max_tokens,
//       # mode, every_ms
//   edges:                         # optional, a list
//     - from: [<nodeId>, <portName>]     # source node + output port
//       to:   [<nodeId>, <portName>]     # target node + input port
//
// Ports are implicit per node type. A "typed-edge marshaller" carries a value
// (string / int / bool / enum / list / object) from an output port to an input
// port; M1 keeps every value's text form authoritative and coerces to text
// wherever a consumer needs text.
//
// NODE TYPES IMPLEMENTED FOR REAL IN M1:
//   trigger    params.kind = manual | cron
//              manual: fires once (output port "out"). cron: registered later
//              via the kernel cron_register_callback path; NOT fired on a
//              direct headless run (no scheduler is faked).
//   transform  params.op = constant | upper | lower | concat
//              constant: output = params.text (or params.value)
//              upper/lower: case of input port "in" (or params.text)
//              concat: input "in" + params.sep + input "in2" (or params.text)
//              output port "out"
//   llm        params.system (optional), params.prompt (with {portName}
//              placeholders substituted from inbound edges). Calls the shared
//              aiclient one-shot (userland/libc/aiclient.h). Output port "out".
//   file       params.op = write | append, params.path = <path>
//              text from inbound edge "text" (or "in"), else params.text.
//              The write is routed through the aicap capability gate + audit
//              (userland/libc/aicap.h). Output port "out" = bytes written.
//
// NODE TYPES ADDED IN M2:
//   foreach    (alias loop) iterate a body sub-region once per list item.
//              params.items (or inbound "items" edge), params.sep (default ,),
//              params.max_iters bounds it. Body = nodes with params.loop =
//              this node's id; each iteration binds outputs "item"/"index",
//              runs the body, and collects the value fed into this node's
//              "collect" port. Outputs "results" (list) and "count" (int).
//   app        dispatch to another app's tool contract (contract.h).
//              params.app + params.feature (or params.bind = app.feature),
//              params.verb (default call). The gate for the target action's
//              declared risk is applied in flow_plat_app_invoke below (SAFE
//              runs, GUARDED via aicap launch-consent, DENIED/unknown refuse).
//              Output "out" = parsed value, "reply" = the app's reply line.
//
// NODE TYPES ADDED IN M3:
//   agent      the agent-promise loop ("promise completion"). Owns a
//              perceive->decide->act BODY (nodes with params.loop = this node's
//              id, same mechanism as foreach) run repeatedly until a termination
//              promise is met: OBJECTIVE (a body node's goal_met wired back is
//              true), MEASURED (the metric input crosses success's threshold,
//              e.g. "metric >= 3200"), or BUDGET (max_iters / max_minutes /
//              max_tokens). Outputs done (reason), iterations, tokens (an
//              iteration proxy, since aiclient exposes no token count).
//   subloop    the async sub-loop. mode=await runs the sub-body to completion
//              then joins (fully real). mode=fire-and-forget ALSO runs to
//              completion at spawn TODAY (single-threaded runner, no kernel
//              threads): NO real background concurrency yet, every_ms/max_conc
//              are parsed but not scheduled. Outputs result, metric, joined.
//
// NODE TYPES NOT IMPLEMENTED (executor returns "unsupported node type: X" and
// exits non-zero, never a silent success): device, service, merge, branch,
// variable.
// ===========================================================================

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "syscall.h"

#include "flow.h"
#include "aiclient.h"
#include "aicap.h"
#include "contract.h"
#include "userconf.h"

// ---- platform seam: monotonic wall clock for the agent budget --------------
unsigned long flow_plat_now_ms(void) { return uptime_ms(); }

// ---- platform seam: capability-gated file write ---------------------------
int flow_plat_write_file(const char *path, const char *text, int len,
                         int append, char *err, int errcap) {
    aicap_init();

    // The user launched this NAMED workflow, which DECLARES `path` as a file
    // node's output. M1 treats that launch as consent to the workflow's
    // declared writes: mint a single-use fs.write capability token scoped to
    // exactly this path, then route the write through the SAME aicap gate the
    // AI tool loop uses and audit the REAL outcome. This is not a bypass: the
    // grant is scoped to the one declared path, is single-use, and is audited;
    // a write to any other path is not covered by the token and is refused. A
    // later milestone replaces implicit launch-consent with an explicit
    // per-run grant in the visual editor.
    aicap_grant_scoped("fs.write", path, 0, 1, "flowrun");

    char reason[160] = {0}, how[64] = {0};
    int a = aicap_authorize("fs.write", path, reason, sizeof(reason), how, sizeof(how));
    const char *cap = aicap_cap_of("fs.write");
    if (a != AICAP_ALLOW) {
        aicap_audit("fs.write", cap, path, "denied", aicap_code(a));
        snprintf(err, errcap, "file write denied by capability gate: %s",
                 reason[0] ? reason : aicap_code(a));
        return -1;
    }

    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        aicap_audit("fs.write", cap, path, "error", "open failed");
        snprintf(err, errcap, "cannot open %s for write", path);
        return -1;
    }
    int w = 0;
    while (w < len) {
        int k = (int)write(fd, text + w, (unsigned)(len - w));
        if (k <= 0) break;
        w += k;
    }
    close(fd);
    if (w != len) {
        aicap_audit("fs.write", cap, path, "error", "short write");
        snprintf(err, errcap, "short write to %s (%d/%d bytes)", path, w, len);
        return -1;
    }
    aicap_audit("fs.write", cap, path, "ok", how[0] ? how : "token");
    return w;
}

// ---- platform seam: LLM text completion via the shared aiclient -----------
int flow_plat_llm(const char *system, const char *prompt,
                  char *out, int outcap, char *err, int errcap) {
    if (!aiclient_init() || !aiclient_have_key()) {
        snprintf(err, errcap, "AI unavailable: no API key for this user (add one in Settings > AI)");
        if (out && outcap > 0) out[0] = 0;
        return -1;
    }
    // aiclient_ask() is the shared one-shot text export (the same client the
    // aichat app drives). M1 folds the node's system instruction into the
    // one-shot prompt, since aiclient_ask() takes a single prompt.
    char full[FLOW_VAL_MAX * 2];
    if (system && system[0]) snprintf(full, sizeof(full), "%s\n\n%s", system, prompt);
    else snprintf(full, sizeof(full), "%s", prompt);

    int rc = aiclient_ask(full, out, outcap, 0);
    if (rc != 0) {
        snprintf(err, errcap, "AI request failed (rc=%d): %s", rc,
                 (out && out[0]) ? out : "(no detail)");
        return -1;
    }
    return 0;
}


// ===========================================================================
// #469 AI-VISION: perceive / decide / act on the real OS.
// ===========================================================================
// WHAT THIS IS. Three platform seams that let a workflow WATCH an application
// and DRIVE it: capture a window to a JPEG, ask the model about that JPEG, and
// press a key in that window. Composed inside the existing `agent` node they
// are a perceive -> decide -> act loop with a real budget.
//
// WHAT IT IS NOT, and this is the important half. Both halves are privileged
// and BOTH GO THROUGH THE HUMAN. Capturing the screen needs a consented
// screen.capture grant; driving another app's window needs a consented
// CAP_SCOPE_WINDOW_TARGET input.inject grant naming that exact window. The
// kernel will not raise either prompt unless THIS process has had real hardware
// input in the last few seconds, and only a real keypress can approve the
// prompt (a synthetic event is INPUT_SRC_SYNTHETIC and never stamps the input
// credit, by construction - see kernel/SECURITY_ADVISORIES.md
// MAYTERA-SEC-2026-0022/0023). So the code below CREATES A WINDOW AND ASKS,
// on the serial console and on screen, and fails with a clear reason if nobody
// answers. There is no auto-approve path here and there must never be one.

#include "gui.h"        // win_create / win_get_event: the consent surface
#include "jpegenc.h"    // the SHARED encoder (moved into libc by this change)
#include "aiclient.h"   // aiclient_ask_image + AICLIENT_IMAGE_MAX
#include "aicap.h"      // #469m aicap_metric: the SHARED AI instrumentation record

#define FLOW_MAX_WINS        48
#define FLOW_GRANT_MS        900000   // the kernel clamps to GRANT_MAX_TTL_MS (15 min)
#define FLOW_CONSENT_TENTHS  1200     // 120s to press a key / answer the prompt
#define FLOW_SHOT_TENTHS     120      // 12s for the compositor to write the BMP
// #flowcapscratch: THE INTERMEDIATE FULL-SCREEN FRAME IS SCRATCH, AND ITS NAME
// HAD TO SAY SO. It used to be FLOWSHOT.BMP, which reads like a workflow
// OUTPUT, sits in the user's own home, and is DELETED WITHOUT WARNING by
// flow_plat_capture() below ("so we cannot read a stale frame").
//
// MEASURED on golden 2472, and it cost a real investigation. A probe workflow
// named `aaseed` wrote /HOME/GBTEST/FLOWSHOT.BMP and logged `aaseed: OK`.
// Afterwards there was no such file and no PERMS.DB row for it, which reads
// exactly like a `file` node reporting success for work it did not do. It did
// not: run-all runs workflows in ASCENDING NAME ORDER, `aaseed` sorts before
// `visiongame`, and visiongame's capture node then unlink()ed that same path as
// its own scratch frame. sys_unlink() also calls perms_remove(), which is why
// the PERMS.DB row went with it, while FLOWALL.LOG in the same directory
// survived. The write happened; a later node in the same run deleted it.
//
// The runner must not silently destroy a name a person could plausibly choose
// for their own output. Scratch gets a scratch name.
#define FLOW_SHOT_NAME       "FLOWCAP.TMP"

// ---- small file helpers ---------------------------------------------------
static long flow_file_size(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    off_t n = lseek(fd, 0, SEEK_END);
    close(fd);
    return (long)n;
}

// Read a whole file into a malloc'd buffer. Refuses anything over `cap` rather
// than returning a prefix, because a prefix of an image is not an image.
static unsigned char *flow_read_whole(const char *path, long *out_len, long cap) {
    long sz = flow_file_size(path);
    if (sz <= 0 || sz > cap) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { close(fd); return 0; }
    long got = 0;
    while (got < sz) {
        int k = (int)read(fd, buf + got, (unsigned)(sz - got));
        if (k <= 0) break;
        got += k;
    }
    close(fd);
    if (got != sz) { free(buf); return 0; }
    *out_len = sz;
    return buf;
}

// ---- consent ---------------------------------------------------------------
// The window exists for ONE reason: a capability prompt may only follow real
// input to a window THIS process owns. Without it every request is CAP_ENOINPUT
// and the feature is unreachable. It is created lazily, so a workflow that
// never captures and never injects stays headless.
static int g_consent_win = -1;

#define FLOW_CONSENT_W 380
#define FLOW_CONSENT_H 110

static int flow_consent_window(void) {
    if (g_consent_win >= 0) return g_consent_win;
    // PARKED BOTTOM-RIGHT, on purpose. This window is in shot: the compositor
    // captures its whole backbuffer, so anything it covers is missing from
    // every capture this flow takes. Apps open near the top-left (GBEMU's own
    // default is 100,60), so the far corner is the least likely place to be
    // sitting on the thing the flow is trying to photograph. It is not a
    // guarantee: if it does overlap the target, move one of them.
    fb_info_t fb;
    memset(&fb, 0, sizeof(fb));
    int x = 700, y = 560;
    if (fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        x = (int)fb.width  - FLOW_CONSENT_W - 16;
        y = (int)fb.height - FLOW_CONSENT_H - 56;   // clear of the taskbar
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    }
    g_consent_win = win_create("Maytera Flow", x, y, FLOW_CONSENT_W, FLOW_CONSENT_H);
    if (g_consent_win < 0) return -1;
    win_draw_text(g_consent_win, 12, 18, "Maytera Flow needs your permission.", 0x000000);
    win_draw_text(g_consent_win, 12, 40, "1. Press any key in THIS window.", 0x000000);
    win_draw_text(g_consent_win, 12, 60, "2. Approve the prompt that appears.", 0x000000);
    win_invalidate(g_consent_win);
    return g_consent_win;
}

// Bring our own window back to the front before waiting for the input credit.
// win_create() focuses the new window, so the FIRST request is fine, but by the
// time a second capability is requested the consent prompt has come and gone and
// focus may well have landed somewhere else. A real key then goes to whatever
// has focus, the credit never arrives, and the run times out for a reason that
// looks like nothing at all. REUSES wm_focus()/wm_get_windows(); the caller's
// own window handle is matched through wm_window_info_t.uwin.
static void flow_focus_consent(void) {
    if (g_consent_win < 0) return;
    static wm_window_info_t wins[FLOW_MAX_WINS];
    int n = wm_get_windows(wins, FLOW_MAX_WINS);
    for (int i = 0; i < n; i++)
        if (wins[i].uwin == g_consent_win) { wm_focus(wins[i].id); return; }
}

// Wait, bounded, for one REAL key to our own window: the input credit. This is
// a blocking wait with a timeout on the window's event queue (win_get_event's
// own timeout), not a poll loop.
static int flow_wait_input_credit(int win, int tenths) {
    for (int i = 0; i < tenths; i++) {
        gui_event_t ev;
        memset(&ev, 0, sizeof(ev));
        if (win_get_event(win, &ev, 100) == EVENT_KEY_DOWN) return 1;
    }
    return 0;
}

static const char *flow_cap_name(unsigned int cap) {
    if (cap == CAP_SCREEN_CAPTURE) return "screen.capture";
    if (cap == CAP_INPUT_INJECT)   return "input.inject";
    return "capability";
}

// Obtain a grant, HONESTLY: request, let the human credit it with a real key,
// let the compositor's prompt be approved by a real key, report the verdict.
// Returns 0 only when the kernel says GRANTED. Never fabricates a grant and
// never retries past the human's answer: a DENY is final for this run.
static int flow_acquire_cap(unsigned int cap, unsigned int scope_kind,
                            const char *scope, const char *reason,
                            char *err, int errcap) {
    cap_state_t st;
    memset(&st, 0, sizeof(st));
    sys_cap_query(cap, &st);
    if (st.held) return 0;                      // a live grant already covers us

    int win = flow_consent_window();
    if (win < 0) {
        snprintf(err, errcap,
                 "cannot create the consent window (no compositor?); %s cannot be requested",
                 flow_cap_name(cap));
        return -1;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        fprintf(stderr,
                "flowrun: CONSENT NEEDED for %s (scope %s). Press a key in the "
                "\"Maytera Flow\" window, then approve the prompt.\n",
                flow_cap_name(cap), scope);
        cap_req_t r;
        memset(&r, 0, sizeof(r));
        r.cap = cap;
        r.duration_ms = FLOW_GRANT_MS;
        r.scope_kind = scope_kind;
        strncpy(r.reason, reason, sizeof(r.reason) - 1);
        r.reason_len = (unsigned int)strlen(r.reason);
        strncpy(r.scope, scope, sizeof(r.scope) - 1);
        long seq = sys_cap_request(&r);

        if (seq == CAP_ENOINPUT) {
            // Expected on the first try: a headless runner has had no input.
            flow_focus_consent();
            if (!flow_wait_input_credit(win, FLOW_CONSENT_TENTHS)) {
                snprintf(err, errcap,
                         "no consent for %s: no real keypress reached the Maytera Flow "
                         "window within %ds", flow_cap_name(cap), FLOW_CONSENT_TENTHS / 10);
                return -1;
            }
            continue;                           // credited; ask again
        }
        if (seq <= 0) {
            snprintf(err, errcap,
                     "%s request refused by the kernel (%ld; -6 = bad/dead scope, "
                     "-2 = another prompt is open, -7 = not issuable)",
                     flow_cap_name(cap), seq);
            return -1;
        }

        long verdict = CAP_ST_OPEN;
        for (int i = 0; i < FLOW_CONSENT_TENTHS && verdict == (long)CAP_ST_OPEN; i++) {
            gui_event_t ev;
            memset(&ev, 0, sizeof(ev));
            win_get_event(win, &ev, 100);       // bounded blocking wait, not a spin
            verdict = sys_cap_status((unsigned long long)seq);
        }
        if (verdict == (long)CAP_ST_GRANTED) {
            fprintf(stderr, "flowrun: %s GRANTED for scope %s\n", flow_cap_name(cap), scope);
            return 0;
        }
        snprintf(err, errcap, "%s consent %s", flow_cap_name(cap),
                 verdict == (long)CAP_ST_DENIED ? "was DENIED" : "prompt timed out unanswered");
        return -1;
    }
    snprintf(err, errcap, "could not obtain %s consent (input credit lapsed twice)",
             flow_cap_name(cap));
    return -1;
}

// ---- target resolution -----------------------------------------------------
// REUSES the existing enumeration, SYS_WM_GET_WINDOWS / wm_get_windows(). No
// new enumeration syscall: the struct simply now also carries the owning app's
// own window handle (wm_window_info_t.uwin), which is the id space
// SYS_CAP_INJECT_* and a WINDOW_TARGET consent request address.
typedef struct {
    int  handle;            // user_windows[] slot: what inject/consent want, -1 = none
    int  x, y, w, h;        // OUTER bounds, screen coordinates
    char title[64];
} flow_target_t;

static int flow_ciup(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int flow_ci_contains(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && flow_ciup(hay[i + j]) == flow_ciup(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static int flow_resolve_target(const char *target, flow_target_t *t,
                               char *err, int errcap) {
    memset(t, 0, sizeof(*t));
    t->handle = -1;

    fb_info_t fb;
    memset(&fb, 0, sizeof(fb));
    if (fb_info(&fb) != 0 || fb.width == 0 || fb.height == 0) {
        snprintf(err, errcap, "cannot read the framebuffer geometry");
        return -1;
    }
    if (!target || !target[0] || strcmp(target, "screen") == 0) {
        t->x = 0; t->y = 0;
        t->w = (int)fb.width; t->h = (int)fb.height;
        snprintf(t->title, sizeof(t->title), "screen");
        return 0;
    }

    const char *want = target;
    int by_handle = -1, by_app = 0;
    if (strncmp(target, "win:", 4) == 0)         by_handle = atoi(target + 4);
    else if (strncmp(target, "app:", 4) == 0)    { by_app = 1; want = target + 4; }
    else if (strncmp(target, "window:", 7) == 0) want = target + 7;

    static wm_window_info_t wins[FLOW_MAX_WINS];
    int n = wm_get_windows(wins, FLOW_MAX_WINS);
    if (n <= 0) {
        snprintf(err, errcap, "no windows to target (wm_get_windows returned %d)", n);
        return -1;
    }
    int best = -1;
    for (int i = 0; i < n; i++) {
        // Skip our own consent window: a flow driving its own prompt surface is
        // exactly what the capability design refuses, so do not even offer it.
        if (wins[i].uwin >= 0 && wins[i].uwin == g_consent_win) continue;
        if (!wins[i].visible || wins[i].minimized) continue;
        int hit;
        if (by_handle >= 0)   hit = (wins[i].uwin == by_handle);
        else if (by_app)      hit = (wins[i].app_id[0] && flow_ci_contains(wins[i].app_id, want));
        else                  hit = flow_ci_contains(wins[i].title, want);
        if (hit) { best = i; break; }
    }
    if (best < 0) {
        snprintf(err, errcap,
                 "no visible window matches target '%s' (%d window(s) open)", target, n);
        return -1;
    }
    t->handle = wins[best].uwin;
    t->x = wins[best].x; t->y = wins[best].y;
    t->w = wins[best].width; t->h = wins[best].height;
    snprintf(t->title, sizeof(t->title), "%s", wins[best].title);
    return 0;
}

// ---- capture ---------------------------------------------------------------
// Box-average downscale. Averaging rather than point-sampling matters here: the
// consumer is a vision model reading a small UI, and nearest-neighbour
// shimmer destroys thin glyphs at exactly the scales we use.
static void flow_box_downscale(const uint32_t *src, int sw, int sh,
                               uint32_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            unsigned long r = 0, g = 0, b = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t p = src[(long)yy * sw + xx];
                    r += (p >> 16) & 0xFFu;
                    g += (p >> 8) & 0xFFu;
                    b += p & 0xFFu;
                    cnt++;
                }
            if (!cnt) cnt = 1;
            dst[(long)y * dw + x] = 0xFF000000u |
                ((uint32_t)(r / cnt) << 16) | ((uint32_t)(g / cnt) << 8) | (uint32_t)(b / cnt);
        }
    }
}

// Where the intermediate full-screen BMP goes. It must be a path THIS user can
// already write (the screenshot syscall runs perms_check on it and the grant
// does not widen that), so it lives in the session home like FLOWALL.LOG does.
static const char *flow_shot_path(void) {
    static char p[256];
    if (p[0]) return p;
    if (getuid() == 0 || userhome_path(0, FLOW_SHOT_NAME, p, sizeof(p)) != 0)
        snprintf(p, sizeof(p), "/HOME/%s", FLOW_SHOT_NAME);
    return p;
}

// The compositor writes the BMP asynchronously (SYS_SCREENSHOT_REQUEST only
// ENQUEUES; sys_screenshot_poll is compositor-only). Wait for a COMPLETE file:
// the BMP header's own byte count must match the file's size. Checking the
// header rather than "the size stopped changing" is what stops us decoding a
// half-written image and calling the result a screenshot.
static int flow_wait_for_bmp(const char *path, int tenths) {
    for (int i = 0; i < tenths; i++) {
        sys_sleep(100);
        long sz = flow_file_size(path);
        if (sz < 54) continue;                  // not even a BMP header yet
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        unsigned char h[14];
        int k = (int)read(fd, h, sizeof(h));
        close(fd);
        if (k != (int)sizeof(h)) continue;
        if (h[0] != 'B' || h[1] != 'M') continue;
        long declared = (long)h[2] | ((long)h[3] << 8) | ((long)h[4] << 16) | ((long)h[5] << 24);
        if (declared > 0 && declared == sz) return 0;
    }
    return -1;
}

int flow_plat_capture(const char *target, int rx, int ry, int rw, int rh,
                      int max_w, int max_h, int quality, const char *path,
                      int *out_w, int *out_h, char *err, int errcap) {
    // #469m PERCEIVE-HALF INSTRUMENTATION. The phases below are the ones the
    // AI_LOOP_EFFICIENCY_BASELINE question turns on: if capture-and-encode
    // dominates, a faster DECISION model saves nothing and shrinking the frame
    // is the entire win. Timed with mono_us() (TSC-backed), never uptime_ms().
    // Emitted as ONE op="capture" record on the success path; a failure emits
    // nothing rather than a half-filled row that would pollute the medians.
    aicap_metric_t cm;
    memset(&cm, 0, sizeof(cm));
    cm.op = "capture"; cm.model = ""; cm.style = ""; cm.outcome = "ok";
    cm.tool = "flow.capture"; cm.tok_in = -1; cm.tok_out = -1;
    unsigned long long m_c0 = mono_us();

    flow_target_t t;
    if (flow_resolve_target(target, &t, err, errcap) != 0) return -1;

    const char *shot = flow_shot_path();
    if (flow_acquire_cap(CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, shot,
                         "capture the screen for a workflow", err, errcap) != 0)
        return -1;

    // #flowcapscratch: this DESTROYS whatever is at `shot`, and it is why the
    // scratch name is FLOWCAP.TMP and not something a workflow author would
    // reach for. See the comment on FLOW_SHOT_NAME.
    unlink(shot);                                // so we cannot read a stale frame
    long sr = sys_screenshot_request(shot);
    if (sr != 0) {
        // #shotwriter: name the reason. These codes are distinct in
        // kernel/gui/shotq.c precisely so a refusal does not have to be
        // guessed at from a 12-second timeout twelve seconds later.
        const char *why =
            (sr == -3)  ? "the capability grant does not cover this path" :
            (sr == -13) ? "THIS process has no write permission there" :
            (sr == -14) ? "the COMPOSITOR, which actually writes the file, has no write "
                          "permission there (it runs as the session user, which may not be you)" :
            (sr == -15) ? "no compositor owns the framebuffer, so nothing would ever write it" :
            (sr == -11) ? "the kernel screenshot queue is full; try again" :
                          "unknown refusal";
        snprintf(err, errcap, "screenshot request refused (%ld): %s: %s", sr, why, shot);
        return -1;
    }
    if (flow_wait_for_bmp(shot, FLOW_SHOT_TENTHS) != 0) {
        snprintf(err, errcap,
                 "the compositor did not write a complete %s within %ds",
                 shot, FLOW_SHOT_TENTHS / 10);
        return -1;
    }
    // Capture phase = consent/grant + the screenshot request + the wait for the
    // compositor to finish writing it. Those are one unit from the caller's
    // point of view and the wait dominates, so splitting them further would be
    // precision the number does not have.
    unsigned long long m_cap = mono_us();
    cm.us_capture = (unsigned long)(m_cap - m_c0);

    // Decode the full-resolution frame. Cropping BEFORE any scaling is the
    // whole point: the region of interest is usually a small part of a big
    // screen, and scaling first would throw away the pixels we need.
    long flen = 0;
    unsigned char *fbuf = flow_read_whole(shot, &flen, 32L * 1024 * 1024);
    if (!fbuf) {
        snprintf(err, errcap, "cannot read the captured frame %s", shot);
        return -1;
    }
    fb_info_t fb;
    memset(&fb, 0, sizeof(fb));
    fb_info(&fb);
    int sw = (int)fb.width, sh = (int)fb.height;
    unsigned long cap_bytes = (unsigned long)sw * (unsigned long)sh * 4u;
    uint32_t *full = (uint32_t *)malloc((size_t)cap_bytes);
    if (!full) {
        free(fbuf);
        snprintf(err, errcap, "out of memory decoding the %dx%d frame", sw, sh);
        return -1;
    }
    int dims[2] = {0, 0};
    unsigned long long m_dec0 = mono_us();
    cm.us_io = (unsigned long)(m_dec0 - m_cap);   // reading the BMP off disk
    int dn = decode_image(fbuf, (unsigned)flen, sw, sh, full, (unsigned)cap_bytes, dims);
    cm.us_decode = (unsigned long)(mono_us() - m_dec0);
    free(fbuf);
    if (dn <= 0 || dims[0] < 1 || dims[1] < 1) {
        free(full);
        snprintf(err, errcap, "could not decode the captured frame %s", shot);
        return -1;
    }
    int iw = dims[0], ih = dims[1];

    // The crop is in the TARGET's coordinates; clamp it to the decoded frame.
    int cx = t.x + rx, cy = t.y + ry;
    int cw = (rw > 0) ? rw : t.w;
    int ch = (rh > 0) ? rh : t.h;
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx > iw - 1) cx = iw - 1;
    if (cy > ih - 1) cy = ih - 1;
    if (cw > iw - cx) cw = iw - cx;
    if (ch > ih - cy) ch = ih - cy;
    if (cw < 1 || ch < 1) {
        free(full);
        snprintf(err, errcap,
                 "crop rect is empty after clamping to the %dx%d frame "
                 "(target '%s' at %d,%d %dx%d)", iw, ih, target, t.x, t.y, t.w, t.h);
        return -1;
    }

    unsigned long long m_sc0 = mono_us();
    uint32_t *crop = (uint32_t *)malloc((size_t)((long)cw * ch * 4));
    if (!crop) {
        free(full);
        snprintf(err, errcap, "out of memory cropping %dx%d", cw, ch);
        return -1;
    }
    for (int y = 0; y < ch; y++)
        memcpy(crop + (long)y * cw, full + (long)(cy + y) * iw + cx, (size_t)cw * 4);
    free(full);

    // Downscale only if it is actually too big. A 160x144 LCD stays 160x144.
    int dw = cw, dh = ch;
    uint32_t *img = crop;
    if (cw > max_w || ch > max_h) {
        long n1 = (long)cw * max_h, n2 = (long)ch * max_w;
        if (n1 > n2) { dw = max_w; dh = (int)((long)ch * max_w / cw); }
        else         { dh = max_h; dw = (int)((long)cw * max_h / ch); }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        img = (uint32_t *)malloc((size_t)((long)dw * dh * 4));
        if (!img) {
            free(crop);
            snprintf(err, errcap, "out of memory scaling to %dx%d", dw, dh);
            return -1;
        }
        flow_box_downscale(crop, cw, ch, img, dw, dh);
        free(crop);
    }

    unsigned long long m_je0 = mono_us();
    cm.us_scale = (unsigned long)(m_je0 - m_sc0);   // crop + any downscale

    unsigned char *jpg = 0;
    long jlen = 0;
    int je = jpeg_encode_argb(img, dw, dh, quality, 0, &jpg, &jlen);
    cm.us_encode = (unsigned long)(mono_us() - m_je0);
    free(img);
    if (je != 0 || !jpg || jlen <= 0) {
        if (jpg) free(jpg);
        snprintf(err, errcap, "JPEG encode failed for %dx%d", dw, dh);
        return -1;
    }
    // FAIL, do not truncate: an image the request cannot carry is a capture
    // that must be made smaller (a tighter rect, a lower max_width/quality),
    // and saying so is more useful than posting a corrupt body.
    if (jlen > AICLIENT_IMAGE_MAX) {
        snprintf(err, errcap,
                 "captured JPEG is %ld bytes, over the %d-byte model request limit "
                 "(%dx%d at quality %d): tighten rect, or lower max_width/quality",
                 jlen, AICLIENT_IMAGE_MAX, dw, dh, quality);
        free(jpg);
        return -1;
    }

    // Route the write through the SAME capability-gated, audited file seam the
    // file node uses. The bytes are binary; flow_plat_write_file takes a length
    // and never calls strlen on the payload.
    unsigned long long m_w0 = mono_us();
    int w = flow_plat_write_file(path, (const char *)jpg, (int)jlen, 0, err, errcap);
    cm.us_io += (unsigned long)(mono_us() - m_w0);   // + writing the JPEG back
    free(jpg);
    if (w < 0) return -1;

    cm.img_bytes = jlen;
    cm.us_total  = (unsigned long)(mono_us() - m_c0);
    aicap_metric(&cm);

    if (out_w) *out_w = dw;
    if (out_h) *out_h = dh;
    return w;
}

// ---- decide: the vision call ----------------------------------------------
int flow_plat_llm_image(const char *system, const char *prompt,
                        const char *image_path,
                        char *out, int outcap, char *err, int errcap) {
    if (!aiclient_init() || !aiclient_have_key()) {
        snprintf(err, errcap,
                 "AI unavailable: no API key for this user (add one in Settings > AI)");
        if (out && outcap > 0) out[0] = 0;
        return -1;
    }
    long len = 0;
    unsigned char *jpg = flow_read_whole(image_path, &len, AICLIENT_IMAGE_MAX);
    if (!jpg) {
        snprintf(err, errcap,
                 "cannot read the captured image %s (missing, empty, or over %d bytes)",
                 image_path, AICLIENT_IMAGE_MAX);
        return -1;
    }
    int rc = aiclient_ask_image(system, prompt, jpg, len, out, outcap);
    free(jpg);
    if (rc != 0) {
        snprintf(err, errcap, "AI vision request failed (rc=%d): %s", rc,
                 (out && out[0]) ? out : "(no detail)");
        return -1;
    }
    return 0;
}

// ---- act: one consented keystroke into another app's window ----------------
// The grant is per TARGET WINDOW and is acquired lazily on first use. If the
// target changes we revoke and ask again rather than reaching for a grant the
// human approved for a different window: the kernel would refuse it
// (CAP_ESCOPE) anyway, and asking is the honest behaviour.
static int g_inject_win = -1;

static int flow_acquire_inject(const flow_target_t *t, char *err, int errcap) {
    if (g_inject_win == t->handle) {
        cap_state_t st;
        memset(&st, 0, sizeof(st));
        sys_cap_query(CAP_INPUT_INJECT, &st);
        if (st.held) return 0;                  // still live for this window
    } else if (g_inject_win >= 0) {
        sys_cap_revoke(CAP_INPUT_INJECT);       // a grant for a DIFFERENT window
        g_inject_win = -1;
    }
    char scope[24];
    snprintf(scope, sizeof(scope), "%d", t->handle);
    char reason[120];
    snprintf(reason, sizeof(reason), "let a workflow press keys in %s", t->title);
    if (flow_acquire_cap(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, scope,
                         reason, err, errcap) != 0)
        return -1;
    g_inject_win = t->handle;
    return 0;
}

int flow_plat_input_key(const char *target, int keycode,
                        int hold_ms, int gap_ms, char *err, int errcap) {
    flow_target_t t;
    if (flow_resolve_target(target, &t, err, errcap) != 0) return -1;
    if (t.handle < 0) {
        snprintf(err, errcap,
                 "target '%s' (\"%s\") has no Ring-3 owner, so it cannot be driven",
                 target, t.title);
        return -1;
    }
    if (flow_acquire_inject(&t, err, errcap) != 0) return -1;

    long d = sys_cap_inject_key(t.handle, keycode);
    if (d != 0) {
        snprintf(err, errcap,
                 "input.inject refused the press (%ld; -3 = no grant / revoked, "
                 "-6 = grant does not cover this window, -2 = a consent prompt is open "
                 "or the session is locked)", d);
        return -1;
    }
    if (hold_ms > 0) sys_sleep((unsigned)hold_ms);
    // THE RELEASE IS NOT OPTIONAL. Without it the app sees the button go down
    // and never come up: it is held for the rest of the session and a second
    // press of the same key is unobservable to anything that edge-detects.
    long u = sys_cap_inject_key_up(t.handle, keycode);
    if (u != 0) {
        snprintf(err, errcap, "input.inject refused the release (%ld); the key is "
                              "now stuck down in \"%s\"", u, t.title);
        return -1;
    }
    if (gap_ms > 0) sys_sleep((unsigned)gap_ms);
    return 0;
}

// ---- "app" node seam, wired to the real contract API + capability gate -----
//
// Read the target action's DECLARED risk from the app's own contract (the
// `describe` verb is SAFE and read-only), so the gate here matches what the app
// itself will enforce. This mirrors the file node: SAFE runs (still audited),
// GUARDED goes through the SAME aicap gate under the launch-consent model (the
// user launched this named workflow, which DECLARES this app action), and
// DENIED refuses before anything is spawned. Enforcement is defence in depth:
// the target app's own contract_cli() is the authoritative single-site gate and
// audits again; this never bypasses it, and an unknown action fails closed.
static int flow_app_risk(const char *app, const char *feature) {
    // Ask the app to describe its contract, then find the feature's `risk:`.
    static char desc[4096];
    desc[0] = 0;
    char *dav[1] = { (char *)"describe" };
    if (contract_invoke(app, 1, dav, desc, (int)sizeof(desc)) != CT_OK)
        return -1;                               // could not read the contract
    // Each item is one line: "  - {name: <f>, ... risk: <r>, ...}".
    char needle[FLOW_PORT_MAX + 8];
    snprintf(needle, sizeof(needle), "name: %s,", feature);
    const char *line = strstr(desc, needle);
    if (!line) return -1;                        // no such action declared
    const char *nl = strchr(line, '\n');
    const char *rk = strstr(line, "risk: ");
    if (!rk || (nl && rk > nl)) return -1;       // risk not on this item's line
    rk += 6;
    if (strncmp(rk, "safe", 4) == 0)    return CT_SAFE;
    if (strncmp(rk, "guarded", 7) == 0) return CT_GUARDED;
    if (strncmp(rk, "denied", 6) == 0)  return CT_DENIED;
    return -1;
}

int flow_plat_app_invoke(const char *app, int argc, char **argv,
                         char *out, int ocap) {
    // Only call/get/set are item-addressed (argv[0]=verb, argv[1]=item). A
    // probe/list/describe carries no item and is inherently read-only.
    const char *verb    = (argc > 0 && argv[0]) ? argv[0] : "";
    const char *feature = (argc > 1 && argv[1]) ? argv[1] : "";
    int item_addressed = (strcmp(verb, "call") == 0 || strcmp(verb, "get") == 0 ||
                          strcmp(verb, "set") == 0);
    if (!item_addressed)
        return contract_invoke(app, argc, argv, out, ocap);

    aicap_init();
    char tool[CT_NAME_MAX];
    snprintf(tool, sizeof(tool), "%s.%s", app, feature);

    int risk = flow_app_risk(app, feature);
    if (risk == CT_DENIED) {
        aicap_audit(tool, tool, feature, "denied", "contract:denied");
        snprintf(out, ocap, "err code=denied detail=action-declared-denied name=%s\n", feature);
        return -1;
    }
    if (risk < 0) {
        // Unknown action / unreadable contract: fail closed, like aicap does
        // for an unknown tool id. Do not spawn a call we cannot classify.
        aicap_audit(tool, tool, feature, "denied", "risk-unknown");
        snprintf(out, ocap, "err code=denied detail=action-risk-unknown name=%s\n", feature);
        return -1;
    }
    if (risk == CT_GUARDED) {
        // Launch-consent: mint a single-use scoped grant for the action's
        // capability and authorize through the SAME aicap gate the file node
        // uses. The grant is scoped to this action and single-use; a different
        // action is not covered.
        aicap_grant_scoped(tool, feature, 0, 1, "flowrun");
        char reason[160] = {0}, how[64] = {0};
        int a = aicap_authorize(tool, feature, reason, sizeof(reason), how, sizeof(how));
        const char *cap = aicap_cap_of(tool);
        if (a != AICAP_ALLOW) {
            aicap_audit(tool, cap, feature, "denied", aicap_code(a));
            snprintf(out, ocap, "err code=denied detail=%s name=%s\n",
                     reason[0] ? reason : aicap_code(a), feature);
            return -1;
        }
        int rc = contract_invoke(app, argc, argv, out, ocap);
        aicap_audit(tool, cap, feature, rc == CT_OK ? "ok" : "error",
                    how[0] ? how : "token");
        return rc;
    }
    // CT_SAFE: runs with no token, still audited with the real outcome.
    int rc = contract_invoke(app, argc, argv, out, ocap);
    aicap_audit(tool, tool, feature, rc == CT_OK ? "ok" : "error", "safe");
    return rc;
}

// ---------------------------------------------------------------------------
// Run a single named workflow through the SAME parse + execute path main()
// uses. On success returns 0 and *bytes = bytes written; on failure returns -1
// and `reason` holds the (parse or execute) error. Shared by the single-arg
// mode and the "run all" driver so both stay identical.
static int flow_run_one(const char *name, char *reason, int rcap, long *bytes) {
    char path[256];
    if (strchr(name, '/')) {
        snprintf(path, sizeof(path), "%s", name);   // an explicit path
    } else {
        snprintf(path, sizeof(path), "/CONFIG/WORKFLOWS/%s.yml", name);
    }

    static flow_graph_t g;   // large; static, and calls here are sequential
    char err[FLOW_ERR_MAX] = {0};
    if (flow_parse_file(path, &g, err, sizeof(err)) != 0) {
        snprintf(reason, rcap, "parse: %s", err);
        return -1;
    }
    fprintf(stderr, "flowrun: workflow '%s' parsed: %d node(s), %d edge(s)\n",
            g.name[0] ? g.name : name, g.nnodes, g.nedges);

    flow_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.g = &g;
    if (flow_execute(&ctx) != 0) {
        snprintf(reason, rcap, "%s", ctx.err);
        return -1;
    }
    *bytes = ctx.total_written;
    return 0;
}

// ---------------------------------------------------------------------------
// "Run all" mode (NO argument, or the argument "all"). Enumerates every
// top-level workflow in /CONFIG/WORKFLOWS/*.yml and runs each through the SAME
// flow_run_one() path the single-workflow mode uses, in sorted (deterministic)
// order. This is the headless entry cron triggers: cron's launch action passes
// NO workflow argument (its target is one whitespace-free path), and the GUI
// Run button needs the mouse (#334), so `ONESHOT <n> launch /APPS/FLOWRUN uid=0`
// is the only way to run the seeded workflows without a human.
//
// SCOPE: only the top-level *.yml files run. The examples/ SUBDIR is
// deliberately SKIPPED: sys_readdir reports it as a directory entry (type == 1)
// and we drop directories, so the bundled read-only templates are never run.
//
// LOG: results are written to FLOWALL.LOG (created/truncated at the start of
// the run, one appended line per workflow: "<name>: OK" or
// "<name>: FAIL <reason>"), through the SAME capability-gated
// flow_plat_write_file() the file node uses. That mints a scoped, single-use
// fs.write grant for exactly that path, so no separate capability wiring is
// needed and the write is audited like any other flow write.
//
// WHERE: root (uid 0) keeps /HOME/FLOWALL.LOG, the path existing docs and
// tests read. Any other user writes <their home>/FLOWALL.LOG via the shared
// userhome_path(): /HOME is root-owned, so a uid 1000 cron run could write its
// flow outputs but silently never its run-all log (measured on golden 2470).
//
// BOUND: enumeration is capped at FLOWALL_MAX so a huge directory cannot run
// unbounded, and one failing workflow never aborts the rest (each outcome is
// recorded in the log and on stdout).
#define FLOWALL_MAX 64
#define FLOWALL_LOG_ROOT "/HOME/FLOWALL.LOG"
#define FLOWALL_LOG_NAME "FLOWALL.LOG"

// Resolved once per run; see WHERE above.
static const char *flowall_log_path(void) {
    static char path[256];
    if (path[0]) return path;
    if (getuid() == 0 || userhome_path(0, FLOWALL_LOG_NAME, path, sizeof(path)) != 0)
        snprintf(path, sizeof(path), "%s", FLOWALL_LOG_ROOT);
    return path;
}

static void flowall_log(const char *line, int append) {
    char lerr[FLOW_ERR_MAX] = {0};
    int len = (int)strlen(line);
    const char *lp = flowall_log_path();
    if (flow_plat_write_file(lp, line, len, append, lerr, sizeof(lerr)) < 0)
        fprintf(stderr, "flowrun: log write to %s failed: %s\n", lp, lerr);
}

static int run_all(void) {
    static char names[FLOWALL_MAX][FLOW_ID_MAX];
    int n = 0;
    dirent_t e;
    for (int idx = 0; n < FLOWALL_MAX; idx++) {
        if (sys_readdir("/CONFIG/WORKFLOWS", idx, &e) != 0) break;  // end/err
        if (e.type == 1) continue;                     // skip subdirs (examples/)
        int L = (int)strlen(e.name);
        if (L < 5 || strcmp(e.name + L - 4, ".yml") != 0) continue;
        int nl = L - 4;
        if (nl > FLOW_ID_MAX - 1) nl = FLOW_ID_MAX - 1;
        memcpy(names[n], e.name, nl);
        names[n][nl] = 0;
        n++;
    }

    // Deterministic order: ascending by name (insertion sort, n <= FLOWALL_MAX).
    for (int i = 1; i < n; i++) {
        char tmp[FLOW_ID_MAX];
        snprintf(tmp, sizeof(tmp), "%s", names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            snprintf(names[j + 1], FLOW_ID_MAX, "%s", names[j]);
            j--;
        }
        snprintf(names[j + 1], FLOW_ID_MAX, "%s", tmp);
    }

    // Create/truncate the log up front so a readback reflects exactly THIS run.
    flowall_log("", 0);
    fprintf(stderr, "flowrun: run-all found %d workflow(s) in /CONFIG/WORKFLOWS\n", n);

    int ok = 0, fail = 0;
    for (int i = 0; i < n; i++) {
        char reason[FLOW_ERR_MAX] = {0};
        char logline[FLOW_ERR_MAX + FLOW_ID_MAX + 16];
        long bytes = 0;
        if (flow_run_one(names[i], reason, sizeof(reason), &bytes) == 0) {
            printf("%s FLOWRUN-OK %ld\n", names[i], bytes);
            snprintf(logline, sizeof(logline), "%s: OK\n", names[i]);
            ok++;
        } else {
            printf("%s FLOWRUN-FAIL %s\n", names[i], reason);
            snprintf(logline, sizeof(logline), "%s: FAIL %s\n", names[i], reason);
            fail++;
        }
        flowall_log(logline, 1);                        // append per workflow
    }

    // One machine-readable summary line for a headless reader.
    printf("FLOWALL-DONE %d ok %d fail %d total\n", ok, fail, n);
    return fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    // No workflow argument, or the explicit keyword "all": run every seeded
    // workflow (the headless cron entry). Otherwise run the single named one.
    if (argc < 2 || !argv[1] || !argv[1][0] || strcmp(argv[1], "all") == 0)
        return run_all();

    const char *name = argv[1];
    char reason[FLOW_ERR_MAX] = {0};
    long bytes = 0;
    if (flow_run_one(name, reason, sizeof(reason), &bytes) != 0) {
        printf("FLOWRUN-FAIL %s\n", reason);
        return 1;
    }
    printf("FLOWRUN-OK %ld\n", bytes);
    return 0;
}
