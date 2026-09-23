// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// sertest - Stage 2 serial.port capability demonstration (unshipped dev app).
//
// The run for Stage 2's security property, the same shape captest is for
// screen.capture: a serial.port grant CANNOT be obtained without consent, the
// legitimate consent -> grant -> open -> use -> revoke path works, and the OLD
// ambient route (raw open of /dev/ttyACM0) is now REFUSED where it once
// succeeded. docs/SYSTEM_CAPABILITY_API.md sections 1.5, 4.2, Stage 4.
//
// All output goes to the serial console (COM1). The GREEN path needs two real
// key events delivered from outside this process on a throwaway VM: one for
// input credit, one Enter to Allow the compositor's modal (testinput KEY verbs
// over serial). The green OPEN targets ttyS1 (COM2 on the VM); the bytes it
// writes appear on that second serial, which is the positive proof the open
// actually reached hardware.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/unistd.h"
#include "../../libc/userconf.h"

#define O_RDWR 2

static const char *PORT = "ttyS1";      // COM2 on the test VM
static const char *MARKER = "MAYTERA-SERIAL-CAP-OK\r\n";

static void fill_req(cap_req_t *r, unsigned int cap, unsigned int scope_kind,
                     const char *scope) {
    memset(r, 0, sizeof(*r));
    r->cap = cap;
    r->duration_ms = 120000;
    r->scope_kind = scope_kind;
    strncpy(r->reason, "talk to the 3D printer", sizeof(r->reason) - 1);
    r->reason_len = (unsigned int)strlen(r->reason);
    strncpy(r->scope, scope, sizeof(r->scope) - 1);
}

static const char *vd(long got, long want) {
    return got == want ? "RED-OK (refused as designed)" : "*** UNEXPECTED ***";
}

// ---- ambient closure: the raw /dev/ttyACM0 route must now FAIL --------------
static int ambient_check(void) {
    printf("[SERTEST] ===== AMBIENT CLOSURE: raw /dev/ttyACM0 must be refused =====\n");
    long fd = sys_open("/dev/ttyACM0", O_RDWR);
    // Before Stage 2 the desktop uid could open this (0666). Now the seed is
    // root-owned 0600, so perms_check denies it: a negative return. If a printer
    // were attached before, this is exactly the route that stops being ambient.
    int ok = (fd < 0);
    if (fd >= 0) sys_close((int)fd);
    printf("[SERTEST] A0 open(/dev/ttyACM0, O_RDWR) = %ld  (expect < 0, refused) : %s\n",
           fd, ok ? "OK ambient path closed" : "*** STILL AMBIENT ***");
    return ok ? 0 : 1;
}

static void list_ports(void) {
    serial_pub_t ports[4];
    memset(ports, 0, sizeof(ports));
    long n = sys_serial_list(ports, 4);
    printf("[SERTEST] sys_serial_list = %ld published port(s)\n", n);
    for (long i = 0; i < n && i < 4; i++)
        printf("[SERTEST]    port[%ld] name='%s' class=%u flags=%u\n",
               i, ports[i].name, ports[i].cls, ports[i].flags);
}

static int red_suite(void) {
    printf("[SERTEST] ============ RED SUITE: no grant without consent ============\n");
    printf("[SERTEST] running as an ordinary Ring-3 app, no window, no user intent\n");
    list_ports();

    cap_state_t st; memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_SERIAL_PORT, &st);
    printf("[SERTEST] R0 sys_cap_query(serial.port).held = %u  (expect 0) : %s\n",
           st.held, st.held == 0 ? "OK no grant held" : "*** UNEXPECTED ***");

    long o = sys_serial_open(PORT, O_RDWR);
    printf("[SERTEST] R1 sys_serial_open(%s) no grant = %ld  (expect CAP_EDENIED=%d) : %s\n",
           PORT, o, CAP_EDENIED, vd(o, CAP_EDENIED));

    cap_req_t req; fill_req(&req, CAP_SERIAL_PORT, CAP_SCOPE_PORT, PORT);
    long r = sys_cap_request(&req);
    printf("[SERTEST] R2 sys_cap_request(serial.port,%s) spontaneous = %ld  (expect CAP_ENOINPUT=%d) : %s\n",
           PORT, r, CAP_ENOINPUT, vd(r, CAP_ENOINPUT));

    cap_req_t bad; fill_req(&bad, CAP_SERIAL_PORT, CAP_SCOPE_PORT, "nosuchport");
    long rb = sys_cap_request(&bad);
    printf("[SERTEST] R3 sys_cap_request(serial.port,nosuchport) = %ld  (expect CAP_ESCOPE=%d) : %s\n",
           rb, CAP_ESCOPE, vd(rb, CAP_ESCOPE));

    cap_req_t pathkind; fill_req(&pathkind, CAP_SERIAL_PORT, CAP_SCOPE_PATH, PORT);
    long rp = sys_cap_request(&pathkind);
    printf("[SERTEST] R4 sys_cap_request(serial.port, PATH kind) = %ld  (expect CAP_ESCOPE=%d) : %s\n",
           rp, CAP_ESCOPE, vd(rp, CAP_ESCOPE));

    long v = sys_cap_resolve(1, CAP_ACT_APPROVE);
    printf("[SERTEST] R5 sys_cap_resolve(APPROVE) as non-compositor = %ld  (expect CAP_EPERM=%d) : %s\n",
           v, CAP_EPERM, vd(v, CAP_EPERM));

    memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_SERIAL_PORT, &st);
    printf("[SERTEST] R6 sys_cap_query(serial.port).held = %u  (expect 0) : %s\n",
           st.held, st.held == 0 ? "OK still no grant" : "*** UNEXPECTED ***");

    int ok = (st.held == 0) && (o == CAP_EDENIED) && (r == CAP_ENOINPUT)
             && (rb == CAP_ESCOPE) && (rp == CAP_ESCOPE) && (v == CAP_EPERM);
    printf("[SERTEST] RED SUITE %s: a serial.port grant cannot be obtained without consent.\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static void gui_green(void) {
    printf("[SERTEST-GUI] ======= GREEN: consent -> grant -> open -> use -> revoke =======\n");
    int win = win_create("SerTest", 140, 140, 380, 200);
    if (win < 0) { printf("[SERTEST-GUI] win_create failed\n"); return; }
    win_draw_text(win, 20, 24, "SerTest: send a key (input credit), then Allow", 0x000000);
    win_invalidate(win);
    printf("[SERTEST-GUI] window %d up; WAITING FOR A REAL KEY (send: testinput KEY 1c)\n", win);

    int got = 0;
    for (int i = 0; i < 1200 && !got; i++) {
        gui_event_t ev; memset(&ev, 0, sizeof(ev));
        int e = win_get_event(win, &ev, 100);
        if (e == EVENT_KEY_DOWN) got = 1;
    }
    if (!got) { printf("[SERTEST-GUI] TIMED OUT waiting for input credit; GREEN skipped\n"); win_destroy(win); return; }
    printf("[SERTEST-GUI] input credit received\n");

    cap_req_t req; fill_req(&req, CAP_SERIAL_PORT, CAP_SCOPE_PORT, PORT);
    long seq = sys_cap_request(&req);
    printf("[SERTEST-GUI] sys_cap_request(serial.port,%s) = %ld  (>0 => PROMPT IS UP; send Enter to Allow)\n", PORT, seq);
    if (seq <= 0) { win_destroy(win); return; }

    long stt = CAP_ST_OPEN;
    for (int i = 0; i < 1200 && stt == (long)CAP_ST_OPEN; i++) {
        gui_event_t ev; win_get_event(win, &ev, 100);
        stt = sys_cap_status((unsigned long long)seq);
    }
    printf("[SERTEST-GUI] verdict = %ld  (GRANTED=%d DENIED=%d)\n", stt, CAP_ST_GRANTED, CAP_ST_DENIED);
    if (stt != (long)CAP_ST_GRANTED) { win_destroy(win); return; }

    cap_state_t st; memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_SERIAL_PORT, &st);
    printf("[SERTEST-GUI] GRANT ISSUED: held=%u scope=%s edge_seq=%llu uses=%u\n",
           st.held, st.scope, (unsigned long long)st.granted_seq, st.uses_left);

    long fd = sys_serial_open(PORT, O_RDWR);
    printf("[SERTEST-GUI] sys_serial_open(%s) = %ld  (>=0 => the gate OPENED; fd installed)\n", PORT, fd);
    if (fd >= 0) {
        long w = sys_write((int)fd, MARKER, (unsigned long)strlen(MARKER));
        printf("[SERTEST-GUI] sys_write(fd,marker) = %ld bytes (watch the ttyS1 / COM2 serial socket)\n", w);
        sys_sleep(300);
        sys_close((int)fd);
    }

    sys_cap_revoke(CAP_SERIAL_PORT);
    memset(&st, 0, sizeof(st));
    sys_cap_query(CAP_SERIAL_PORT, &st);
    printf("[SERTEST-GUI] after revoke: held=%u  (expect 0)\n", st.held);
    long o2 = sys_serial_open(PORT, O_RDWR);
    printf("[SERTEST-GUI] after revoke sys_serial_open(%s) = %ld  (expect CAP_EDENIED=%d) : %s\n",
           PORT, o2, CAP_EDENIED, o2 == CAP_EDENIED ? "OK revocation bit in flight" : "*** UNEXPECTED ***");
    if (o2 >= 0) sys_close((int)o2);
    printf("[SERTEST-GUI] GREEN PATH COMPLETE\n");
    win_destroy(win);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    ambient_check();
    red_suite();
    sys_sleep(1500);
    gui_green();
    return 0;
}
