// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// t2live - the tier-2 live-app action wire verification harness.
//   docs/AI_ACTION_CAPABILITY_BINDING.md section 5.2 (the Studio worked example).
//
// Launched from /CONFIG/AUTORUN.CFG on a THROWAWAY verification VM (never a
// shipping golden), configured for an autologin=root session so the harness and
// Studio share a writable home + /CONFIG (as a normal single-user desktop does
// once its user owns a home). It spawns Maytera Studio, then drives the LIVE
// contract path (contract_invoke_live) against that running instance:
//
//   1. a live READ (get doc.width/doc.layers) reaches the running instance's
//      document, proving the wire touches live state and not a spawn;
//   2. RED: the GUARDED edit `invert` with NO app.paint.edit grant is REFUSED
//      (CAPABILITY_DENIED) and the canvas stays white;
//   3. GREEN: after an app.paint.edit token is granted, the SAME `invert`
//      SUCCEEDS on the live canvas, which inverts white -> black (screendump);
//   4. a SAFE action (add_layer) works with no grant, doc.layers goes 1 -> 2;
//   5. a perceive action (screen_check) is refused by the KERNEL screen.capture
//      gate (the real teeth, 4.4), independent of the aicap gate above.
//
// OUTPUT DISCIPLINE: everything goes to fd 2. An autorun-spawned process's fd 1
// does not reliably reach the serial console (see apps/lockprobe/main.c).

#include "../../libc/syscall.h"
#include "../../libc/contract.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"

static void e(const char *s) {
    int n = 0; while (s[n]) n++;
    if (n) sys_write(2, s, (unsigned long)n);
}

static void call_live(const char *verb, const char *name) {
    char *av[3]; int ac = 0;
    av[ac++] = (char *)verb;
    if (name) av[ac++] = (char *)name;
    static char out[1024]; out[0] = 0;
    int rc = contract_invoke_live("paint", ac, av, out, (int)sizeof(out));
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "[T2LIVE] %s %s -> rc=%d reply: ",
             verb, name ? name : "", rc);
    e(hdr);
    if (out[0]) e(out); else e("(empty)\n");
}

int main(void) {
    e("\n[T2LIVE] ===== tier-2 live-app action wire verification =====\n");

    // The app.paint.edit grant is controlled by the IMAGE (a root-writable
    // /CONFIG/AICAPS.CFG), not by this uid-1000 harness which cannot write
    // /CONFIG - that inability is the fail-closed property. RED image: absent.
    // GREEN image: present. So invert is denied on one boot, applied on the next.
    int pid = sys_spawn("/APPS/PAINT");
    char b[64]; snprintf(b, sizeof(b), "[T2LIVE] spawned /APPS/PAINT pid=%d\n", pid);
    e(b);

    // Let Studio run its splash, build the one-white-layer document, and enter
    // its event loop where it polls the live mailbox.
    sys_sleep(9000);

    e("[T2LIVE] --- live READ of the running instance's document ---\n");
    call_live("get", "doc.width");
    call_live("get", "doc.layers");

    e("[T2LIVE] === BASELINE: SCREENDUMP NOW - canvas should be WHITE ===\n");
    sys_sleep(13000);

    e("[T2LIVE] --- GUARDED invert (app.paint.edit): RED image denies, GREEN image applies ---\n");
    call_live("call", "invert");
    e("[T2LIVE] === POST-INVERT: SCREENDUMP NOW ===\n");
    e("[T2LIVE]     RED image (no grant): reply capability-denied, canvas STILL WHITE\n");
    e("[T2LIVE]     GREEN image (grant present): reply ok, canvas now BLACK\n");
    sys_sleep(16000);

    e("[T2LIVE] --- SAFE positive control: add_layer (no grant needed) ---\n");
    call_live("call", "add_layer");
    call_live("get", "doc.layers");

    e("[T2LIVE] --- KERNEL TEETH: screen_check reaches the screen.capture gate ---\n");
    call_live("call", "screen_check");

    e("[T2LIVE] === DONE: holding for final screendump ===\n");
    sys_sleep(80000);
    e("[T2LIVE] done\n");
    return 0;
}
