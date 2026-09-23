// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// injtest - Stage 3 input.inject capability demonstration (unshipped dev app).
//
// The run for Stage 3's security properties, the same shape captest/sertest are
// for screen.capture / serial.port. It proves, on a booted VM with real key
// events delivered from outside the process (testinput over serial):
//
//   1. RED: an ordinary Ring-3 app CANNOT inject input. sys_cap_inject_key/mouse
//      is refused (CAP_EDENIED) with no grant; the RAW compositor-only
//      SYS_INJECT_KEY (197) is refused too, so there is no ambient inject path
//      beside the capability.
//   2. GREEN: after a real-input-credited, compositor-consented input.inject
//      grant (scope=self), injection into a window THIS app OWNS works and is
//      delivered to that window; after revoke it is refused again in flight.
//   3. THE SELF-ESCALATION PROOF (the point of the stage): a synthetic event
//      NEVER stamps the elevation/capability input credit. After the real
//      credit has expired, injecting keys and then requesting ANOTHER
//      capability returns CAP_ENOINPUT: the app cannot inject the keystroke that
//      credits its own next grant or elevation.
//   4. THE PROMPT CANNOT BE SELF-APPROVED: while the app's OWN input.inject
//      consent prompt is open, its attempt to inject the Allow is refused (it
//      holds no grant yet), so injected input cannot answer the very prompt that
//      would issue it.
//
// docs/SYSTEM_CAPABILITY_API.md section 10, Stage 3. All output on COM1.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/unistd.h"
#include "../../libc/userconf.h"

#define SYS_INJECT_KEY_RAW   197   // compositor-only since Stage 0 (proc/syscall.h)
#define INJ_KEY_Z    0x5A              // printable 'Z'
#define INPUT_WINDOW_MS 10000      // must match kernel CAP_INPUT_WINDOW_MS

static const char *SELF = "self";
static const char *CAPPATH = "/HOME/injtest-noreach.bmp"; // never actually captured

static void fill_req(cap_req_t *r, unsigned int cap, unsigned int scope_kind,
                     const char *scope, const char *reason) {
    memset(r, 0, sizeof(*r));
    r->cap = cap;
    r->duration_ms = 120000;
    r->scope_kind = scope_kind;
    strncpy(r->reason, reason, sizeof(r->reason) - 1);
    r->reason_len = (unsigned int)strlen(r->reason);
    strncpy(r->scope, scope, sizeof(r->scope) - 1);
}

static const char *vd(long got, long want) {
    return got == want ? "RED-OK (refused as designed)" : "*** UNEXPECTED ***";
}

// ---- RED suite: no injection, no grant, without consent --------------------
static int red_suite(int win) {
    printf("[INJTEST] ============ RED SUITE: no injection without a grant ============\n");
    printf("[INJTEST] running as an ordinary Ring-3 app (uid should be 1000), own window=%d\n", win);

    cap_state_t st; memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_INPUT_INJECT, &st);
    printf("[INJTEST] R0 sys_cap_query(input.inject).held = %u  (expect 0) : %s\n",
           st.held, st.held == 0 ? "OK no grant held" : "*** UNEXPECTED ***");

    long k = sys_cap_inject_key(win, INJ_KEY_Z);
    printf("[INJTEST] R1 sys_cap_inject_key(win,'Z') no grant = %ld  (expect CAP_EDENIED=%d) : %s\n",
           k, CAP_EDENIED, vd(k, CAP_EDENIED));

    long m = sys_cap_inject_mouse(win, 10, 10, 1, 0);
    printf("[INJTEST] R2 sys_cap_inject_mouse(win,...) no grant = %ld  (expect CAP_EDENIED=%d) : %s\n",
           m, CAP_EDENIED, vd(m, CAP_EDENIED));

    // The RAW compositor-only inject syscall (Stage 0) must also refuse us.
    long raw = syscall1(SYS_INJECT_KEY_RAW, (long)INJ_KEY_Z);
    printf("[INJTEST] R3 raw SYS_INJECT_KEY(197) as non-compositor = %ld  (expect -1) : %s\n",
           raw, vd(raw, -1));

    // scope validation: wrong kind and wrong token are refused BEFORE input credit.
    cap_req_t bad; fill_req(&bad, CAP_INPUT_INJECT, CAP_SCOPE_PATH, SELF, "drive myself");
    long rb = sys_cap_request(&bad);
    printf("[INJTEST] R4 sys_cap_request(input.inject, PATH kind) = %ld  (expect CAP_ESCOPE=%d) : %s\n",
           rb, CAP_ESCOPE, vd(rb, CAP_ESCOPE));

    cap_req_t bad2; fill_req(&bad2, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, "other", "drive myself");
    long rb2 = sys_cap_request(&bad2);
    printf("[INJTEST] R5 sys_cap_request(input.inject, WINDOW 'other') = %ld  (expect CAP_ESCOPE=%d) : %s\n",
           rb2, CAP_ESCOPE, vd(rb2, CAP_ESCOPE));

    // spontaneous, correctly-scoped request with no input credit -> ENOINPUT.
    cap_req_t req; fill_req(&req, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, SELF, "drive myself");
    long r = sys_cap_request(&req);
    printf("[INJTEST] R6 sys_cap_request(input.inject,self) spontaneous = %ld  (expect CAP_ENOINPUT=%d) : %s\n",
           r, CAP_ENOINPUT, vd(r, CAP_ENOINPUT));

    long v = sys_cap_resolve(1, CAP_ACT_APPROVE);
    printf("[INJTEST] R7 sys_cap_resolve(APPROVE) as non-compositor = %ld  (expect CAP_EPERM=%d) : %s\n",
           v, CAP_EPERM, vd(v, CAP_EPERM));

    memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_INPUT_INJECT, &st);
    int ok = (st.held == 0) && (k == CAP_EDENIED) && (m == CAP_EDENIED) && (raw == -1)
             && (rb == CAP_ESCOPE) && (rb2 == CAP_ESCOPE) && (r == CAP_ENOINPUT) && (v == CAP_EPERM);
    printf("[INJTEST] RED SUITE %s: input.inject cannot be obtained or used without consent.\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// wait for one real EVENT_KEY_DOWN on `win`, returning 1 on success.
static int wait_real_key(int win, int tenths) {
    for (int i = 0; i < tenths; i++) {
        gui_event_t ev; memset(&ev, 0, sizeof(ev));
        int e = win_get_event(win, &ev, 100);
        if (e == EVENT_KEY_DOWN) return 1;
    }
    return 0;
}

// drain the window queue looking for an injected EVENT_KEY_DOWN carrying `code`.
static int read_back_key(int win, unsigned int code) {
    for (int i = 0; i < 20; i++) {
        gui_event_t ev; memset(&ev, 0, sizeof(ev));
        int e = win_get_event(win, &ev, 50);
        if (e == EVENT_KEY_DOWN && ev.keycode == code) return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    int win = win_create("InjTest", 150, 150, 420, 200);
    if (win < 0) { printf("[INJTEST] win_create failed\n"); return 1; }
    win_draw_text(win, 16, 24, "InjTest: send a key (credit), then Allow", 0x000000);
    win_invalidate(win);

    red_suite(win);

    printf("[INJTEST] ======= GREEN: consent -> grant -> inject-into-own-window -> revoke =======\n");
    printf("[INJTEST] window %d up; WAITING FOR A REAL KEY (send: testinput KEY 1c)\n", win);
    if (!wait_real_key(win, 1200)) {
        printf("[INJTEST] TIMED OUT waiting for input credit; GREEN skipped\n");
        win_destroy(win); return 0;
    }
    printf("[INJTEST] input credit received (a REAL key)\n");

    cap_req_t req; fill_req(&req, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, SELF, "drive my own window");
    long seq = sys_cap_request(&req);
    printf("[INJTEST] sys_cap_request(input.inject,self) = %ld  (>0 => PROMPT IS UP; send Enter to Allow)\n", seq);
    if (seq <= 0) { win_destroy(win); return 0; }

    // (4) THE PROMPT CANNOT BE SELF-APPROVED. The app holds NO grant yet, so its
    // attempt to inject the Allow keystroke is refused: injected input cannot
    // answer the very prompt that would issue input.inject.
    long selfapprove = sys_cap_inject_key(win, 0x0A /* Enter */);
    printf("[INJTEST] SELF-APPROVE ATTEMPT while own prompt open: sys_cap_inject_key = %ld  (expect <0, refused) : %s\n",
           selfapprove, selfapprove < 0 ? "OK cannot inject own Allow" : "*** SELF-APPROVAL LEAK ***");

    long stt = CAP_ST_OPEN;
    for (int i = 0; i < 1200 && stt == (long)CAP_ST_OPEN; i++) {
        gui_event_t ev; win_get_event(win, &ev, 100);
        stt = sys_cap_status((unsigned long long)seq);
    }
    printf("[INJTEST] verdict = %ld  (GRANTED=%d DENIED=%d)\n", stt, CAP_ST_GRANTED, CAP_ST_DENIED);
    if (stt != (long)CAP_ST_GRANTED) { win_destroy(win); return 0; }

    cap_state_t st; memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_INPUT_INJECT, &st);
    printf("[INJTEST] GRANT ISSUED: held=%u scope=%s edge_seq=%llu uses=%u\n",
           st.held, st.scope, (unsigned long long)st.granted_seq, st.uses_left);

    // (2)+(positive control) inject into the OWN window and read it back.
    long ik = sys_cap_inject_key(win, INJ_KEY_Z);
    int gotk = read_back_key(win, INJ_KEY_Z);
    printf("[INJTEST] INJECT key 'Z' into own window = %ld ; read back EVENT_KEY_DOWN(0x5A) = %s\n",
           ik, gotk ? "YES (injection WORKS)" : "*** NOT DELIVERED ***");
    long im = sys_cap_inject_mouse(win, 40, 40, 1, 0);
    printf("[INJTEST] INJECT mouse-down into own window = %ld  (0 => accepted)\n", im);

    // (3) THE SELF-ESCALATION PROOF. Let the REAL input credit expire, then
    // inject synthetic keys and try to obtain ANOTHER capability. A synthetic
    // event does not stamp the credit, so the request must be refused ENOINPUT.
    // Wait a WALL-CLOCK-guaranteed >15s (> INPUT_WINDOW_MS=10s) so the REAL
    // input credit from the grant keys definitely expires. sys_sleep alone is
    // unreliable under KVM tick-replay (a deadline can fire early), so we spin
    // on sys_mono_us (TSC-backed), which cannot be short-changed. No real key
    // is delivered during this window, so afterwards the ONLY possible source
    // of input credit is the synthetic injection below.
    unsigned long long t0 = mono_us();
    unsigned long long need = 18000000ULL; // 18 s in microseconds
    printf("[INJTEST] --- SELF-ESCALATION PROOF: waiting >=15s wall-clock (mono_us) so REAL credit expires ---\n");
    while (mono_us() - t0 < need) sys_sleep(200);
    printf("[INJTEST] waited %llu ms wall-clock; mono_us and uptime_ms now: %llu / %lu\n",
           (unsigned long long)((mono_us() - t0) / 1000ULL),
           (unsigned long long)mono_us(), (unsigned long)uptime_ms());
    // BASELINE probe: BEFORE any synthetic injection, is the REAL credit expired?
    // If this returns CAP_ENOINPUT, the credit correctly expired and any later
    // success is the injection's doing. If it returns >0, a real key kept the
    // credit alive (a harness/timing confound, not a provenance failure).
    cap_req_t pre; fill_req(&pre, CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, CAPPATH, "baseline");
    long base = sys_cap_request(&pre);
    printf("[INJTEST] BASELINE (NO injection yet) sys_cap_request(screen.capture) = %ld  (expect CAP_ENOINPUT=%d)\n",
           base, CAP_ENOINPUT);
    for (int i = 0; i < 5; i++) sys_cap_inject_key(win, INJ_KEY_Z);   // synthetic, credit-free
    read_back_key(win, INJ_KEY_Z);                                    // drain
    cap_req_t sc; fill_req(&sc, CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, CAPPATH, "screenshot");
    long esc = sys_cap_request(&sc);
    printf("[INJTEST] AFTER synthetic injection, sys_cap_request(screen.capture) = %ld  (expect CAP_ENOINPUT=%d) : %s\n",
           esc, CAP_ENOINPUT, vd(esc, CAP_ENOINPUT));
    elev_request_t er; memset(&er, 0, sizeof(er));
    strncpy(er.name, "injtest", sizeof(er.name) - 1);
    strncpy(er.source, "self-escalation probe", sizeof(er.source) - 1);
    long eel = sys_elev_request(&er);
    printf("[INJTEST] AFTER synthetic injection, sys_elev_request = %ld  (expect a refusal < 0; ENOINPUT=%d)\n",
           eel, ELEV_ENOINPUT);
    int selfesc_ok = (esc == CAP_ENOINPUT) && (eel < 0) && (selfapprove < 0);
    printf("[INJTEST] SELF-ESCALATION %s: an injected event cannot manufacture input credit or answer a prompt.\n",
           selfesc_ok ? "PASS" : "FAIL");

    // (2, revoke arm) revoke, then injection is refused IN FLIGHT.
    sys_cap_revoke(CAP_INPUT_INJECT);
    memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_INPUT_INJECT, &st);
    long after = sys_cap_inject_key(win, INJ_KEY_Z);
    printf("[INJTEST] after revoke: held=%u ; sys_cap_inject_key = %ld  (expect CAP_EDENIED=%d) : %s\n",
           st.held, after, CAP_EDENIED, after == CAP_EDENIED ? "OK revocation bit in flight" : "*** UNEXPECTED ***");

    printf("[INJTEST] GREEN PATH COMPLETE. gotk=%d injectworks_and_selfesc_holds=%d\n",
           gotk, selfesc_ok && gotk && (st.held == 0) && (after == CAP_EDENIED));
    win_destroy(win);
    return 0;
}
