// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// bt_scancad_hosttest.c - hosted unit test for the Bluetooth Settings panel's
// scan-cadence device-list diff logic (bt-settings-ux, 2026-09-16).
//
// Build+run on the userland CT with the host gcc, no VM/kernel/real Bluetooth
// hardware needed:
//   gcc -I../../libc -DBT_STUB_IMPL bt_scancad_hosttest.c -o bt_scancad_test
//   ./bt_scancad_test
//
// This exercises the EXACT bt_ui_merge() that ships inside
// userland/apps/settings/main.c (both include the same bt_scancad.h), not a
// reimplementation kept in a separate test file - so this test cannot
// silently drift from the logic actually running in the panel the way a
// hand-copied mirror could.
//
// -DBT_STUB_IMPL pulls in bt_client.h's honest-stub bodies (bt_addr_eq() in
// particular) since this is a hosted build with no SYS_BT syscall to link
// against; bt_ui_merge() itself never calls any other bt_* function.
//
// What real Bluetooth hardware is needed to confirm beyond this: this test
// proves the DIFF ALGORITHM (add/remove/freeze-metadata/miss-limit semantics)
// against synthetic snapshots. It cannot prove real LE/classic advertising
// timing, real RSSI behavior, or that a real controller's scan results feed
// bt_get_devices() the way these synthetic bt_device_t arrays assume - that
// needs the owner's iMac + a real dongle, per the task's own constraint.
#include "bt_scancad.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); fails++; } \
    else         { printf("ok:   %s\n", (msg)); } } while (0)

static bt_addr_t mk_addr(unsigned char last) {
    bt_addr_t a;
    for (int i = 0; i < 6; i++) a.b[i] = 0;
    a.b[0] = last;
    return a;
}

static bt_device_t mk_dev(unsigned char last, const char *name, int paired, int connected) {
    bt_device_t d;
    memset(&d, 0, sizeof(d));
    d.addr = mk_addr(last);
    int i = 0;
    for (; name[i] && i < BT_NAME_MAX - 1; i++) d.name[i] = name[i];
    d.name[i] = 0;
    d.cls       = BT_DEV_KEYBOARD;
    d.paired    = (unsigned char)paired;
    d.connected = (unsigned char)connected;
    d.link      = connected ? BT_LINK_CONNECTED : paired ? BT_LINK_PAIRED : BT_LINK_FOUND;
    d.rssi      = -50;
    return d;
}

int main(void) {
    bt_ui_dev_t model[BT_UI_MAX_DEVICES];
    int n = 0;

    // 1. Initial full-enumerate pass (allow_add_remove=1): two devices found,
    //    both added with full metadata captured.
    bt_device_t raw1[2] = { mk_dev(1, "Keyboard", 0, 0), mk_dev(2, "Mouse", 0, 0) };
    bt_ui_merge(model, &n, BT_UI_MAX_DEVICES, raw1, 2, 1);
    CHECK(n == 2, "initial enumerate adds both discovered devices");
    CHECK(strcmp(model[0].dev.name, "Keyboard") == 0, "first device metadata captured");
    CHECK(strcmp(model[1].dev.name, "Mouse") == 0, "second device metadata captured");

    // 2. Live-only refresh pass (allow_add_remove=0) with a NEW third device
    //    present in the raw snapshot: must NOT be added - only a cadence
    //    add/remove pass may grow the visible list.
    bt_device_t raw2[3] = { raw1[0], raw1[1], mk_dev(3, "Phone", 0, 0) };
    raw2[0].rssi = -70;   // also prove live refresh updates rssi
    bt_ui_merge(model, &n, BT_UI_MAX_DEVICES, raw2, 3, 0);
    CHECK(n == 2, "live-only refresh does not add a newly-seen device");
    CHECK(model[0].dev.rssi == -70, "live-only refresh DOES update rssi of a known device");
    CHECK(strcmp(model[0].dev.name, "Keyboard") == 0,
          "live-only refresh leaves identity metadata (name) untouched");

    // 3. A cadence add/remove pass now sees the third device: it is added,
    //    with full metadata captured at add time.
    bt_ui_merge(model, &n, BT_UI_MAX_DEVICES, raw2, 3, 1);
    CHECK(n == 3, "add/remove pass adds the newly-seen device");
    CHECK(strcmp(model[2].dev.name, "Phone") == 0, "newly-added device's metadata captured");

    // 4. A device "renamed" in a later raw snapshot must NOT change the
    //    frozen model metadata, even during an add/remove pass ("do not
    //    re-enumerate metadata for devices already known").
    bt_device_t raw3[3];
    memcpy(raw3, raw2, sizeof(raw3));
    strcpy(raw3[0].name, "RenamedKeyboard");
    bt_ui_merge(model, &n, BT_UI_MAX_DEVICES, raw3, 3, 1);
    CHECK(strcmp(model[0].dev.name, "Keyboard") == 0,
          "device identity metadata stays frozen across later passes");

    // 5. One add/remove pass where the unpaired "Phone" goes missing from the
    //    raw snapshot: not removed immediately (grace period / miss counter).
    bt_device_t raw4[2] = { raw3[0], raw3[1] };   // "Phone" no longer present
    bt_ui_merge(model, &n, BT_UI_MAX_DEVICES, raw4, 2, 1);
    CHECK(n == 3, "one missed add/remove pass does not remove an unpaired device yet");

    // 6. A second consecutive miss exceeds BT_SCANCAD_MISS_LIMIT (2): removed.
    bt_ui_merge(model, &n, BT_UI_MAX_DEVICES, raw4, 2, 1);
    bt_ui_merge(model, &n, BT_UI_MAX_DEVICES, raw4, 2, 1);
    CHECK(n == 2, "device removed once it exceeds the consecutive-miss limit");

    // 7. A PAIRED device must never be removed by the miss-counter timeout,
    //    even after many consecutive passes where it is absent - only an
    //    explicit Forget removes a paired/connected row.
    bt_ui_dev_t modelp[BT_UI_MAX_DEVICES];
    int np = 0;
    bt_device_t rawp[1] = { mk_dev(9, "TrustedKB", 1, 0) };
    bt_ui_merge(modelp, &np, BT_UI_MAX_DEVICES, rawp, 1, 1);
    CHECK(np == 1, "paired device added");
    for (int i = 0; i < 10; i++) bt_ui_merge(modelp, &np, BT_UI_MAX_DEVICES, NULL, 0, 1);
    CHECK(np == 1, "a paired device is never dropped by the scan-miss timeout");

    printf("\n%s (%d failing)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
