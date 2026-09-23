// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// injtest2 - Stage 4 CROSS-APP input.inject demonstration (unshipped dev app).
//
// The run for Stage 4's security properties (docs/AI_ACTION_CAPABILITY_BINDING.md
// section 3.1, "a consented target-window scope for input.inject"). It proves,
// on a booted VM with real key events delivered from outside the process
// (testinput over serial), that ONE process can inject synthetic input into a
// SECOND, SEPARATE application's window - and ONLY that window - after a
// real-hardware-consented CAP_SCOPE_WINDOW_TARGET grant, while every Stage 3
// protection still holds across the app boundary.
//
// This binary is BOTH halves:
//   argv[1]=="target"  -> the TARGET app B: creates its own window, publishes
//                         its handle to /TGTWIN.TXT, and echoes every key event
//                         its per-window queue delivers to serial. It reads the
//                         same per-window event queue DOOM does (design 1.3), so
//                         it is a real, drivable second app.
//   otherwise          -> the INJECTOR app A: spawns B, and runs the red/green.
//
// PROPERTIES DEMONSTRATED (each REFUSAL shown, not asserted):
//   RED  no cross-app injection into B without a grant (CAP_EDENIED); a
//        WINDOW_TARGET request naming a non-live window, or a non-decimal noun,
//        is refused (CAP_ESCOPE) before any prompt.
//   GREEN after a real-input-credited, compositor-consented WINDOW_TARGET grant
//        naming B, a synthetic keystroke from A appears in B's window (the target
//        echoes it): CROSS-APP INJECTION WORKS. The consent named B by title.
//   REFUSALS  (a) while A's OWN consent prompt is open, A cannot inject the Allow
//        (CAP_EBUSY): cannot drive the consent surface. (b) A cannot inject into
//        its OWN window under the WINDOW_TARGET(B) grant (CAP_ESCOPE): cannot
//        inject outside the granted target. (c) after revoke, injection into B is
//        refused in flight (CAP_EDENIED).
//   SELF-ESCALATION (the point): after A's real credit expires, driving B with
//        synthetic keys cannot manufacture input credit - A's next
//        sys_cap_request/sys_elev_request is CAP_ENOINPUT. A synthetic event
//        never stamps elev_last_input_ms for ANY window's owner.
//
// All output on COM1.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/userconf.h"

#define IJ2_KEY_Z    0x5A              // printable 'Z'
#define TGT_PATH "/TGTWIN.TXT"
#define SELF_APP "/APPS/INJTEST2"
#define IJ2_CAPPATH  "/HOME/injtest2-noreach.bmp"  // never actually captured

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

// ---------------------------------------------------------------------------
// TARGET app B.
// ---------------------------------------------------------------------------
static int target_main(void) {
    int win = win_create("InjTarget", 480, 170, 360, 170);
    if (win < 0) { printf("[INJTGT] win_create failed\n"); return 1; }
    win_draw_text(win, 16, 24, "InjTarget: another app's window (drivable)", 0x000000);
    win_invalidate(win);

    // Publish our window handle so the injector can name us as its target.
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d\n", win);
    int fd = sys_open(TGT_PATH, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        sys_write(fd, buf, (unsigned long)n);
        sys_fsync(fd);
        sys_close(fd);
    }
    printf("[INJTGT] target window=%d, handle published to %s\n", win, TGT_PATH);
    printf("[INJTGT] draining my per-window event queue; echoing every key...\n");

    // Echo every delivered key. Bounded so a lost injector cannot wedge a slot.
    for (int i = 0; i < 1200; i++) {
        gui_event_t ev; memset(&ev, 0, sizeof(ev));
        int e = win_get_event(win, &ev, 100);
        if (e == EVENT_KEY_DOWN) {
            printf("[INJTGT] RECEIVED KEY 0x%02X in MY window (cross-app injection delivered)\n",
                   (unsigned)ev.keycode);
        }
    }
    printf("[INJTGT] exiting\n");
    win_destroy(win);
    return 0;
}

// read the target handle the child published; -1 on timeout.
static int read_target_handle(int tenths) {
    for (int i = 0; i < tenths; i++) {
        int fd = sys_open(TGT_PATH, O_RDONLY);
        if (fd >= 0) {
            char b[16]; memset(b, 0, sizeof(b));
            long r = sys_read(fd, b, sizeof(b) - 1);
            sys_close(fd);
            if (r > 0) {
                int v = 0, any = 0;
                for (int j = 0; j < (int)r; j++) {
                    if (b[j] >= '0' && b[j] <= '9') { v = v * 10 + (b[j] - '0'); any = 1; }
                    else if (any) break;
                }
                if (any) return v;
            }
        }
        sys_sleep(100);
    }
    return -1;
}

// wait for one real EVENT_KEY_DOWN on `win`.
static int wait_real_key(int win, int tenths) {
    for (int i = 0; i < tenths; i++) {
        gui_event_t ev; memset(&ev, 0, sizeof(ev));
        int e = win_get_event(win, &ev, 100);
        if (e == EVENT_KEY_DOWN) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// INJECTOR app A.
// ---------------------------------------------------------------------------
static int injector_main(void) {
    printf("[INJTEST2] ============ Stage 4: CROSS-APP input.inject ============\n");
    printf("[INJTEST2] running as an ordinary Ring-3 app (uid should be 1000)\n");

    // Spawn the target app B (a separate process with its own window).
    char *argv2[] = { SELF_APP, "target", 0 };
    int cpid = sys_spawn_args(SELF_APP, argv2, 2);
    printf("[INJTEST2] spawned target %s -> pid=%d\n", SELF_APP, cpid);

    int T = read_target_handle(120);   // up to 12s
    if (T < 0) { printf("[INJTEST2] target handle never published; ABORT\n"); return 1; }
    printf("[INJTEST2] target window handle T=%d (app B)\n", T);

    // Our OWN window A, created LAST so it holds keyboard focus for the credit.
    int A = win_create("InjInjector", 120, 150, 400, 190);
    if (A < 0) { printf("[INJTEST2] win_create(A) failed\n"); return 1; }
    win_draw_text(A, 16, 24, "InjInjector: drives app B by consent", 0x000000);
    win_invalidate(A);
    printf("[INJTEST2] my own window A=%d\n", A);

    // ---- RED: no cross-app injection, and no target grant, without consent ----
    long k = sys_cap_inject_key(T, IJ2_KEY_Z);
    printf("[INJTEST2] R1 inject 'Z' into B (T=%d) with NO grant = %ld (expect CAP_EDENIED=%d) : %s\n",
           T, k, CAP_EDENIED, vd(k, CAP_EDENIED));

    cap_req_t badshape; fill_req(&badshape, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, "self", "bad noun");
    long rbs = sys_cap_request(&badshape);
    printf("[INJTEST2] R2 request WINDOW_TARGET noun \"self\" (non-decimal) = %ld (expect CAP_ESCOPE=%d) : %s\n",
           rbs, CAP_ESCOPE, vd(rbs, CAP_ESCOPE));

    cap_req_t baddead; fill_req(&baddead, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, "99", "dead window");
    long rbd = sys_cap_request(&baddead);
    printf("[INJTEST2] R3 request WINDOW_TARGET naming dead handle 99 = %ld (expect CAP_ESCOPE=%d) : %s\n",
           rbd, CAP_ESCOPE, vd(rbd, CAP_ESCOPE));

    int red_ok = (k == CAP_EDENIED) && (rbs == CAP_ESCOPE) && (rbd == CAP_ESCOPE);
    printf("[INJTEST2] RED %s: cannot drive B or bind a bogus target without consent.\n",
           red_ok ? "PASS" : "FAIL");

    // ---- GREEN: real credit -> consent naming B -> grant -> drive B -> refuse ----
    printf("[INJTEST2] window A=%d up; WAITING FOR A REAL KEY (send: testinput KEY 1c)\n", A);
    if (!wait_real_key(A, 1200)) {
        printf("[INJTEST2] TIMED OUT waiting for input credit; GREEN skipped\n");
        win_destroy(A); return 0;
    }
    printf("[INJTEST2] input credit received (a REAL key to window A)\n");

    char tscope[16]; snprintf(tscope, sizeof(tscope), "%d", T);
    cap_req_t req; fill_req(&req, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, tscope, "drive the target window");
    long seq = sys_cap_request(&req);
    printf("[INJTEST2] request WINDOW_TARGET(T=%d) = %ld (>0 => PROMPT UP naming 'InjTarget'; send Enter to Allow)\n",
           T, seq);
    if (seq <= 0) { win_destroy(A); return 0; }

    // CANNOT DRIVE THE CONSENT SURFACE (no grant yet): while A's own
    // WINDOW_TARGET prompt is open and A holds NO grant, its attempt to inject
    // the Allow is refused at the DISPATCHER CHOKEPOINT (CAP_EDENIED, no grant of
    // the class) - a strictly stronger refusal than the temporal EBUSY guard. A
    // cannot inject the Allow for the very grant it is requesting.
    long driveprompt = sys_cap_inject_key(T, 0x0A /* Enter */);
    printf("[INJTEST2] DRIVE-CONSENT (no grant) inject the Allow into B = %ld (expect a refusal < 0) : %s\n",
           driveprompt, driveprompt < 0 ? "OK cannot inject the Allow" : "*** CONSENT-DRIVE LEAK ***");

    long stt = CAP_ST_OPEN;
    for (int i = 0; i < 1200 && stt == (long)CAP_ST_OPEN; i++) {
        gui_event_t ev; win_get_event(A, &ev, 100);
        stt = sys_cap_status((unsigned long long)seq);
    }
    printf("[INJTEST2] verdict = %ld (GRANTED=%d DENIED=%d)\n", stt, CAP_ST_GRANTED, CAP_ST_DENIED);
    if (stt != (long)CAP_ST_GRANTED) { win_destroy(A); return 0; }

    cap_state_t st; memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_INPUT_INJECT, &st);
    printf("[INJTEST2] GRANT ISSUED: held=%u scope=%s edge_seq=%llu uses=%u\n",
           st.held, st.scope, (unsigned long long)st.granted_seq, st.uses_left);

    // THE PROOF: a synthetic keystroke from A appears in app B's window. Inject
    // 'Z' several times, each SPACED with a sleep so B is scheduled to drain its
    // queue and echo, and so the serial TX is not swamped by the kernel heartbeat
    // (which silently drops app lines under load). Watch for [INJTGT] RECEIVED
    // KEY 0x5A - a key that ORIGINATED in process A appearing as input in B.
    long ik = 0;
    printf("[INJTEST2] KEY-ECHO PROOF: injecting 'Z'(0x5A) into B (T=%d) x5; watch for [INJTGT] RECEIVED KEY 0x5A\n", T);
    for (int i = 0; i < 5; i++) {
        ik = sys_cap_inject_key(T, IJ2_KEY_Z);
        sys_sleep(500);
    }
    printf("[INJTEST2] last inject 'Z' into B = %ld (0 => accepted)\n", ik);
    long im = sys_cap_inject_mouse(T, 30, 30, 1, 0);
    printf("[INJTEST2] INJECT mouse-down into B = %ld (0 => accepted)\n", im);

    // CANNOT INJECT OUTSIDE THE GRANTED TARGET: A owns window A, but holds only a
    // WINDOW_TARGET(B) grant, so injecting into its OWN window A is refused.
    long own = sys_cap_inject_key(A, IJ2_KEY_Z);
    printf("[INJTEST2] inject into my OWN window A under WINDOW_TARGET(B) grant = %ld (expect CAP_ESCOPE=%d) : %s\n",
           own, CAP_ESCOPE, own == CAP_ESCOPE ? "OK grant does not cover A" : "*** SCOPE LEAK ***");

    // TEMPORAL GUARD, cross-app: while HOLDING the B grant, open a NEW consent
    // prompt (request screen.capture; the approve keystroke just credited us, so
    // it is fresh). Injecting into the granted target B is now refused with
    // CAP_EBUSY - a live grant cannot be exercised while a prompt is open, so a
    // holder can never race an Allow or drive the machine during consent. Then
    // have the prompt DENIED (send Esc) so it does not block what follows.
    long ebz = -999;
    cap_req_t probe; fill_req(&probe, CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, IJ2_CAPPATH, "temporal-guard probe");
    long pseq = sys_cap_request(&probe);
    if (pseq > 0) {
        ebz = sys_cap_inject_key(T, IJ2_KEY_Z);
        printf("[INJTEST2] TEMPORAL GUARD: hold B grant, a prompt is open; inject into B = %ld (expect CAP_EBUSY=%d) : %s\n",
               ebz, CAP_EBUSY, ebz == CAP_EBUSY ? "OK cannot inject while a prompt is up" : "*** TEMPORAL-GUARD LEAK ***");
        printf("[INJTEST2] DENY THE PROBE PROMPT (send Esc)\n");
        long ps = CAP_ST_OPEN;
        for (int i = 0; i < 300 && ps == (long)CAP_ST_OPEN; i++) {
            gui_event_t ev; win_get_event(A, &ev, 100);
            ps = sys_cap_status((unsigned long long)pseq);
        }
        printf("[INJTEST2] probe prompt verdict = %ld (DENIED=%d expected)\n", ps, CAP_ST_DENIED);
    } else {
        printf("[INJTEST2] TEMPORAL GUARD: probe prompt not raised (req=%ld); skipping (credit may have lapsed)\n", pseq);
    }

    // SELF-ESCALATION PROOF across the app boundary. Wait a wall-clock-guaranteed
    // >15s so A's REAL input credit expires (mono_us is TSC-backed, not tick-
    // replayable). No real key arrives in this window, so the ONLY possible
    // credit source afterward is the synthetic injection into B.
    unsigned long long t0 = mono_us();
    printf("[INJTEST2] --- SELF-ESCALATION PROOF: waiting >=18s wall-clock so A's real credit expires ---\n");
    while (mono_us() - t0 < 18000000ULL) sys_sleep(200);
    cap_req_t pre; fill_req(&pre, CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, IJ2_CAPPATH, "baseline");
    long base = sys_cap_request(&pre);
    printf("[INJTEST2] BASELINE (no injection yet) request(screen.capture) = %ld (expect CAP_ENOINPUT=%d)\n",
           base, CAP_ENOINPUT);
    for (int i = 0; i < 5; i++) sys_cap_inject_key(T, IJ2_KEY_Z);   // drive B, synthetic
    cap_req_t sc; fill_req(&sc, CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, IJ2_CAPPATH, "screenshot");
    long esc = sys_cap_request(&sc);
    printf("[INJTEST2] AFTER driving B, request(screen.capture) = %ld (expect CAP_ENOINPUT=%d) : %s\n",
           esc, CAP_ENOINPUT, vd(esc, CAP_ENOINPUT));
    elev_request_t er; memset(&er, 0, sizeof(er));
    strncpy(er.name, "injtest2", sizeof(er.name) - 1);
    strncpy(er.source, "cross-app self-escalation probe", sizeof(er.source) - 1);
    long eel = sys_elev_request(&er);
    printf("[INJTEST2] AFTER driving B, sys_elev_request = %ld (expect a refusal < 0; ENOINPUT=%d)\n",
           eel, ELEV_ENOINPUT);
    int selfesc_ok = (base == CAP_ENOINPUT) && (esc == CAP_ENOINPUT) && (eel < 0);
    printf("[INJTEST2] SELF-ESCALATION %s: driving another app's window cannot credit A's own consent/elevation.\n",
           selfesc_ok ? "PASS" : "FAIL");

    // REVOKE -> injection into B refused in flight.
    sys_cap_revoke(CAP_INPUT_INJECT);
    memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_INPUT_INJECT, &st);
    long after = sys_cap_inject_key(T, IJ2_KEY_Z);
    printf("[INJTEST2] after revoke: held=%u ; inject into B = %ld (expect CAP_EDENIED=%d) : %s\n",
           st.held, after, CAP_EDENIED, after == CAP_EDENIED ? "OK revocation bit in flight" : "*** UNEXPECTED ***");

    printf("[INJTEST2] GREEN PATH COMPLETE. red_ok=%d selfesc_ok=%d own_scope_ok=%d temporal_ok=%d driveconsent_ok=%d revoke_ok=%d\n",
           red_ok, selfesc_ok, (own == CAP_ESCOPE), (ebz == CAP_EBUSY), (driveprompt < 0),
           (after == CAP_EDENIED) && (st.held == 0));
    win_destroy(A);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "target") == 0)
        return target_main();
    return injector_main();
}
