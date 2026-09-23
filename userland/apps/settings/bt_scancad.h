// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// bt_scancad.h - Bluetooth Settings panel scan-cadence device-list model
// (bt-settings-ux, 2026-09-16).
// =============================================================================
// Owner feedback from real-iMac testing: the Bluetooth panel was continually
// scanning and rebuilding its whole device list from scratch on every redraw,
// and the "Stop scanning" button did not actually stop anything (root cause
// and fix: kernel/bt/bt.c bt_scan_stop()/bt_scan_user_stopped() +
// kernel/bt/hci_usb.c's bt_worker idle-rescan guard + hci_classic_inquiry_
// cancel(), NOT this file).
//
// The requested cadence: on enable/start, scan ONCE for 10 seconds, then take
// one full census of every device found (name/class/etc captured once).
// After that, every 15 seconds, run one more short discovery burst whose ONLY
// job is to add newly-found devices and drop ones no longer seen - it must
// NOT rebuild the visible list or re-fetch metadata for devices already
// known.
//
// This header holds the PURE part of that design: a small persistent model
// (bt_ui_dev_t[], keyed by MAC address) and bt_ui_merge(), the MAC-keyed diff
// that decides what enters/leaves the model and what stays frozen. It has no
// dependency beyond bt_client.h (bt_addr_eq(), bt_device_t, bt_addr_t), so it
// compiles equally under the freestanding userland toolchain (included from
// userland/apps/settings/main.c) and a plain hosted gcc (included from
// bt_scancad_hosttest.c) - the SAME compiled logic is what both build, so the
// unit test cannot silently drift from what ships.
//
// The timer/cadence STATE MACHINE (bt_ui_scan_tick(), 10s/15s deadlines
// against uptime_ms(), and the bt_scan_start()/bt_scan_stop() calls that
// actually drive the kernel) stays in main.c: it needs real syscalls
// (uptime_ms, bt_scan_start/stop, bt_get_devices) that only exist in the
// freestanding build, so it is not a candidate for a host test the same way.
// =============================================================================
#ifndef BT_SCANCAD_H
#define BT_SCANCAD_H

#include "../../libc/bt_client.h"

#define BT_UI_MAX_DEVICES      BT_MAX_DEVICES   // same cap as the raw kernel list
#define BT_SCANCAD_INITIAL_MS  10000            // "scan ONCE for 10 seconds"
#define BT_SCANCAD_INTERVAL_MS 15000            // "every 15 seconds"
#define BT_SCANCAD_BURST_MS     5000            // length of each later burst;
                                                 // not specified by the owner,
                                                 // chosen long enough to catch
                                                 // at least one LE advertising
                                                 // interval from a still-present
                                                 // device without holding the
                                                 // radio scanning continuously
#define BT_SCANCAD_MISS_LIMIT      2            // consecutive missed bursts
                                                 // before an unpaired/
                                                 // unconnected row is dropped,
                                                 // so one unlucky burst does
                                                 // not flicker a real device
                                                 // out of the list

typedef struct {
    bt_addr_t   addr;
    int         used;
    bt_device_t dev;   // name/cls/is_le frozen at first sight; rssi/paired/
                        // connected/link refreshed every merge pass
    int         miss;  // consecutive add/remove passes this MAC was absent
} bt_ui_dev_t;

typedef enum {
    BT_SCANCAD_OFF = 0,     // Bluetooth disabled or no adapter: reset on next enable
    BT_SCANCAD_INITIAL,     // the one 10s discovery burst on enable/start
    BT_SCANCAD_SETTLED,     // between bursts; radio not scanning
    BT_SCANCAD_INCREMENTAL, // a short later burst, add/remove only
} bt_scancad_state_t;

// MAC-keyed diff of one raw kernel snapshot into the persistent model.
// allow_add_remove=0: refresh live fields (rssi/paired/connected/link) only,
//   for devices already in the model - never adds or removes a row. Safe and
//   cheap to call every draw.
// allow_add_remove=1: also add newly-seen MACs (capturing full metadata once)
//   and age out unpaired/unconnected rows absent for more than
//   BT_SCANCAD_MISS_LIMIT consecutive passes. Only called from
//   bt_ui_scan_tick() (in main.c) at a 10s/15s cadence boundary.
// Paired/connected rows are NEVER aged out this way (a user's known device,
// not just a transient discovery hit) - only Forget removes those.
static void bt_ui_merge(bt_ui_dev_t *model, int *pn, int max,
                        const bt_device_t *raw, int nraw, int allow_add_remove) {
    int seen[BT_UI_MAX_DEVICES];
    for (int i = 0; i < BT_UI_MAX_DEVICES; i++) seen[i] = 0;
    for (int i = 0; i < nraw; i++) {
        int found = -1;
        for (int j = 0; j < *pn; j++)
            if (bt_addr_eq(&model[j].addr, &raw[i].addr)) { found = j; break; }
        if (found >= 0) {
            model[found].dev.rssi      = raw[i].rssi;
            model[found].dev.paired    = raw[i].paired;
            model[found].dev.connected = raw[i].connected;
            model[found].dev.link      = raw[i].link;
            model[found].miss = 0;
            seen[found] = 1;
        } else if (allow_add_remove && *pn < max) {
            bt_ui_dev_t *slot = &model[*pn];
            slot->addr = raw[i].addr;
            slot->used = 1;
            slot->dev  = raw[i];   // full metadata captured once, here only
            slot->miss = 0;
            seen[*pn] = 1;
            (*pn)++;
        }
        // else: model already full (BT_UI_MAX_DEVICES == BT_MAX_DEVICES, so
        // this can only happen if the raw snapshot itself is already at cap
        // with distinct MACs the model has not seen yet); silently drop.
    }
    if (!allow_add_remove) return;
    int w = 0;
    for (int j = 0; j < *pn; j++) {
        if (!seen[j] && !model[j].dev.paired && !model[j].dev.connected) {
            model[j].miss++;
            if (model[j].miss > BT_SCANCAD_MISS_LIMIT) continue;   // drop the row
        }
        if (w != j) model[w] = model[j];
        w++;
    }
    *pn = w;
}

#endif // BT_SCANCAD_H
