// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// captest - Stage 1 capability API demonstration (unshipped dev app).
//
// docs/SYSTEM_CAPABILITY_API.md section 14 says the design was "read from code,
// not demonstrated on a VM ... one grade weaker than a run". This is the run
// for Stage 1's security property: A GRANT CANNOT BE OBTAINED WITHOUT CONSENT,
// AND the legitimate consent -> grant -> use -> revoke path.
//
// All output goes to the serial console (stdout -> /dev/console -> COM1), so the
// whole demonstration is readable over a serial socket with no GUI clicking
// (#334 records that is unreliable).
//
// DEFAULT MODE (from /CONFIG/AUTORUN.CFG, which passes no argv) runs BOTH:
//   1. the RED suite: an ordinary app with no window and no user intent tries
//      every route to a screen.capture grant and to using it, and each is
//      REFUSED. Proof that no grant appears without consent.
//   2. the GREEN path: create a window, receive a real key (INPUT CREDIT
//      delivered externally via the testinput channel on a throwaway VM),
//      request the capability, and let the COMPOSITOR's consent prompt resolve
//      it (a real Enter delivered to the modal). Then confirm the grant is held,
//      use it, revoke it, and confirm the revocation bit in flight.
// The GREEN path needs two real key events delivered from outside this process
// (input credit, then Allow); on a throwaway VM those come from testinput KEY
// verbs over serial. If none arrive it reports TIMED OUT and exits cleanly.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/unistd.h"
#include "../../libc/userconf.h"

static const char *RED_SHOT = "/HOME/CAPTEST.BMP";

static void fill_req(cap_req_t *r, unsigned int cap, const char *scope) {
    memset(r, 0, sizeof(*r));
    r->cap = cap;
    r->duration_ms = 120000;   // 2 minutes
    r->scope_kind = CAP_SCOPE_PATH;
    strncpy(r->reason, "capture the screen for the demo", sizeof(r->reason) - 1);
    r->reason_len = (unsigned int)strlen(r->reason);
    strncpy(r->scope, scope, sizeof(r->scope) - 1);
}

static const char *vd(long got, long want) {
    return got == want ? "RED-OK (refused as designed)" : "*** UNEXPECTED ***";
}

static int red_suite(void) {
    printf("[CAPTEST] ============ RED SUITE: no grant without consent ============\n");
    printf("[CAPTEST] running as an ordinary Ring-3 app, no window, no user intent\n");
    cap_state_t st; memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_SCREEN_CAPTURE, &st);
    printf("[CAPTEST] R0 sys_cap_query(screen.capture).held = %u  (expect 0) : %s\n",
           st.held, st.held == 0 ? "OK no grant held" : "*** UNEXPECTED ***");
    cap_req_t req; fill_req(&req, CAP_SCREEN_CAPTURE, RED_SHOT);
    long r = sys_cap_request(&req);
    printf("[CAPTEST] R1 sys_cap_request(screen.capture) = %ld  (expect CAP_ENOINPUT=%d) : %s\n",
           r, CAP_ENOINPUT, vd(r, CAP_ENOINPUT));
    long s = sys_screenshot_request(RED_SHOT);
    printf("[CAPTEST] R2 sys_screenshot_request (no grant) = %ld  (expect CAP_EDENIED=%d) : %s\n",
           s, CAP_EDENIED, vd(s, CAP_EDENIED));
    long v = sys_cap_resolve(1, CAP_ACT_APPROVE);
    printf("[CAPTEST] R3 sys_cap_resolve(APPROVE) as non-compositor = %ld  (expect CAP_EPERM=%d) : %s\n",
           v, CAP_EPERM, vd(v, CAP_EPERM));
    cap_view_t view; memset(&view, 0, sizeof(view));
    long vw = sys_cap_view(&view);
    printf("[CAPTEST] R4 sys_cap_view as non-compositor = %ld  (expect CAP_EPERM=%d) : %s\n",
           vw, CAP_EPERM, vd(vw, CAP_EPERM));
    // R5: a capability that is DEFINED but NOT ISSUABLE is refused by policy
    // (CAP_EPOLICY), before any scope/consent check. NOTE: this used to request
    // input.inject, but Stage 3 (docs/SYSTEM_CAPABILITY_API.md section 10) made
    // input.inject issuable, so it now proceeds past the policy gate. net.connect
    // is still deferred (Stage 4), so it is the current stand-in for "not issuable
    // in this stage" and keeps this check testing the policy refusal it always
    // meant to.
    cap_req_t ir; fill_req(&ir, CAP_NET_CONNECT, RED_SHOT);
    long p = sys_cap_request(&ir);
    printf("[CAPTEST] R5 sys_cap_request(net.connect, not issuable) = %ld  (expect CAP_EPOLICY=%d) : %s\n",
           p, CAP_EPOLICY, vd(p, CAP_EPOLICY));
    memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_SCREEN_CAPTURE, &st);
    printf("[CAPTEST] R6 sys_cap_query(screen.capture).held = %u  (expect 0) : %s\n",
           st.held, st.held == 0 ? "OK still no grant" : "*** UNEXPECTED ***");
    int ok = (st.held == 0) && (r == CAP_ENOINPUT) && (s == CAP_EDENIED)
             && (v == CAP_EPERM) && (vw == CAP_EPERM) && (p == CAP_EPOLICY);
    printf("[CAPTEST] RED SUITE %s: a grant cannot be obtained without consent.\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static void gui_green(void) {
    printf("[CAPTEST-GUI] ========= GREEN: consent -> grant -> use -> revoke =========\n");
    char scope[64];
    if (userhome_path(NULL, "CAPTEST.BMP", scope, sizeof(scope)) != 0)
        strncpy(scope, "/HOME/CAPTEST.BMP", sizeof(scope) - 1);
    printf("[CAPTEST-GUI] scope (session home) = %s\n", scope);

    int win = win_create("CapTest", 140, 140, 360, 200);
    if (win < 0) { printf("[CAPTEST-GUI] win_create failed\n"); return; }
    win_draw_text(win, 20, 24, "CapTest: send a key (input credit), then Allow", 0x000000);
    win_invalidate(win);
    printf("[CAPTEST-GUI] window %d up; WAITING FOR A REAL KEY (send: testinput KEY 1c)\n", win);

    int got = 0;
    for (int i = 0; i < 1200 && !got; i++) {
        gui_event_t ev; memset(&ev, 0, sizeof(ev));
        int e = win_get_event(win, &ev, 100);
        if (e == EVENT_KEY_DOWN) got = 1;
    }
    if (!got) { printf("[CAPTEST-GUI] TIMED OUT waiting for input credit; GREEN skipped\n"); win_destroy(win); return; }
    printf("[CAPTEST-GUI] input credit received\n");

    cap_req_t req; fill_req(&req, CAP_SCREEN_CAPTURE, scope);
    long seq = sys_cap_request(&req);
    printf("[CAPTEST-GUI] sys_cap_request(screen.capture) = %ld  (>0 => PROMPT IS UP; send Enter to Allow)\n", seq);
    if (seq <= 0) { win_destroy(win); return; }

    long stt = CAP_ST_OPEN;
    for (int i = 0; i < 1200 && stt == (long)CAP_ST_OPEN; i++) {
        gui_event_t ev; win_get_event(win, &ev, 100);
        stt = sys_cap_status((unsigned long long)seq);
    }
    printf("[CAPTEST-GUI] verdict = %ld  (GRANTED=%d DENIED=%d)\n", stt, CAP_ST_GRANTED, CAP_ST_DENIED);
    if (stt != (long)CAP_ST_GRANTED) { win_destroy(win); return; }

    cap_state_t st; memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_SCREEN_CAPTURE, &st);
    printf("[CAPTEST-GUI] GRANT ISSUED: held=%u scope=%s edge_seq=%llu uses=%u\n",
           st.held, st.scope, (unsigned long long)st.granted_seq, st.uses_left);

    long sh = sys_screenshot_request(scope);
    printf("[CAPTEST-GUI] sys_screenshot_request = %ld  (0=captured; not CAP_EDENIED=-3 => the gate OPENED)\n", sh);
    sys_sleep(800);

    sys_cap_revoke(CAP_SCREEN_CAPTURE);
    memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_SCREEN_CAPTURE, &st);
    printf("[CAPTEST-GUI] after revoke: held=%u  (expect 0)\n", st.held);
    long sh2 = sys_screenshot_request(scope);
    printf("[CAPTEST-GUI] after revoke sys_screenshot_request = %ld  (expect CAP_EDENIED=%d) : %s\n",
           sh2, CAP_EDENIED, sh2 == CAP_EDENIED ? "OK revocation bit in flight" : "*** UNEXPECTED ***");
    printf("[CAPTEST-GUI] GREEN PATH COMPLETE\n");
    win_destroy(win);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    red_suite();
    // Small settle so the RED lines flush and the compositor is fully up before
    // the window is created for the GREEN path.
    sys_sleep(1500);
    gui_green();
    return 0;
}
