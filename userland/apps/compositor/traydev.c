// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// traydev.c - removable-devices system-tray widget MODEL (owner req #5,
// deferred from the removdev pass - see docs/REMOVABLE_DEVICES_TRAY_PLAN.md).
//
// WHAT THIS FILE OWNS: the JOIN of the two already-shipped device sources
// into one tray-ready row list, and the per-class eject decision (the
// "matrix"). It owns no drawing and no click handling - see taskbar.c's
// "#traydev" section for the tray icon glyph, the popup card, and the Eject
// button wiring. Same model/draw split as desktop.c (icon model) vs draw.c
// (primitives) elsewhere in this tree; struct + prototypes live in
// compositor.h next to desktop.c's volume/USB declarations.
//
// SOURCES (both already shipped, #removdev, ea c812de):
//   vol_list()          SYS_VOL_LIST=283  removable STORAGE volumes (USB
//                        drives + mounted disk images). Every entry already
//                        carries MOSVOL_REMOVABLE (kernel/proc/syscall.h:
//                        "always set today; every entry is USB").
//   sys_dev_usb_list()   SYS_DEV_USB_LIST=273  every enumerated USB device,
//                        storage included, from the xHCI append-only enum
//                        registry (hotplug included).
//
// DEDUP: a physical USB mass-storage stick appears in BOTH lists - once as a
// volume, once as its raw USB device row. desktop_usbdev_tick() already
// solved exactly this for the arrival-toast case by deferring bDeviceClass
// 0x00 (per-interface/composite, the usual stick) and 0x08 (explicit mass
// storage) to the volume side, and skipping 0x09 (hub, infrastructure). This
// file applies the IDENTICAL skip so the tray never lists a storage device
// twice under two different names; usb_class_label() (desktop.c, now
// non-static) is reused verbatim for the same reason.
//
// THE MATRIX (per the plan doc): every VOLUME row is eject-capable - mass
// storage, optical, and floppy all already go through the one proven
// vol_eject() path (desktop_icon_eject() uses it identically). No USB-device
// row is, because none of today's non-storage classes (network adapter, HID,
// audio, printer, imaging, video, wireless, vendor-specific) has a safe-
// remove concept. traydev_class_ejectable() is still written as a per-class
// switch, not a blanket "kind == storage" test, so a future class that DOES
// need an eject affordance is a one-line addition here, not a rule change
// somewhere else.
//
// THROTTLING (#426: the draw thread must never block, and never poll the
// kernel from a render path). One 1s-throttled poll, the SAME cadence and
// shape as desktop_volumes_tick()/desktop_usbdev_tick() - two syscalls that
// write at most SC_VOL_MAX+32 records and touch no hardware. Called from the
// input tick (main.c), never from a render path. traydev_eject() re-polls
// once, synchronously, for the SAME reason desktop_icon_eject() does: so the
// ejected row disappears on the same frame as the confirmation rather than
// waiting out the throttle window.

#include "compositor.h"
#include "../../libc/syscall.h"
// devinfo.h drags in libc/types.h, whose bool block would re-typedef bool as
// _Bool and clash with compositor.h (typedef int bool). Same guard desktop.c
// already carries for the identical include (see its own comment there).
#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1
#endif
#include "../../libc/devinfo.h"
#include "../../libc/stdio.h"

#define TRAYDEV_MAX_ROWS (SC_VOL_MAX + 32)

static traydev_row_t g_rows[TRAYDEV_MAX_ROWS];
static int           g_row_n = 0;

// The per-class eject matrix. See file header for why this is a switch and
// not a blanket rule: today every branch (the default included) says no, but
// the shape is here so a future non-storage eject-capable class is a single
// added case, not a redesign.
static int traydev_class_ejectable(uint8_t dev_class) {
    switch (dev_class) {
        default: return 0;   // network / HID / audio / printer / imaging / video / vendor: no safe-remove concept today
    }
}

static void row_add_volume(const sc_volume_t *v) {
    if (g_row_n >= TRAYDEV_MAX_ROWS) return;
    traydev_row_t *r = &g_rows[g_row_n++];
    r->kind      = TRAYDEV_STORAGE;
    r->vol_index = v->index;
    r->can_eject = 1;   // matrix: mass storage / optical / floppy -> Eject
    if (v->name[0]) { strncpy(r->name, v->name, sizeof(r->name) - 1); r->name[sizeof(r->name) - 1] = '\0'; }
    else            { strncpy(r->name, v->mount, sizeof(r->name) - 1); r->name[sizeof(r->name) - 1] = '\0'; }
    const char *what = (v->flags & MOSVOL_OPTICAL) ? "Optical disc"
                      : (v->flags & MOSVOL_FLOPPY)  ? "Floppy disk"
                                                     : "USB drive";
    snprintf(r->subtitle, sizeof(r->subtitle), "%s - %s", what, v->mount);
}

static void row_add_usb(const devinfo_usb_t *d) {
    if (g_row_n >= TRAYDEV_MAX_ROWS) return;
    traydev_row_t *r = &g_rows[g_row_n++];
    r->kind      = TRAYDEV_USBOTHER;
    r->vol_index = -1;
    r->can_eject = traydev_class_ejectable(d->dev_class);   // matrix: no -> info only
    snprintf(r->name, sizeof(r->name), "%s", usb_class_label(d->dev_class));
    snprintf(r->subtitle, sizeof(r->subtitle), "USB %04x:%04x", d->vendor_id, d->product_id);
}

// Shared by traydev_poll() and traydev_eject()'s immediate re-poll - ONE
// rebuild routine, so the two call sites can never apply the dedup/matrix
// rule differently.
static void traydev_rebuild(void) {
    sc_volume_t vols[SC_VOL_MAX];
    int nv = vol_list(vols, SC_VOL_MAX);
    if (nv < 0) nv = 0;

    devinfo_usb_t devs[32];
    int nd = sys_dev_usb_list(devs, 32);
    if (nd < 0) nd = 0;
    if (nd > 32) nd = 32;

    g_row_n = 0;
    for (int i = 0; i < nv; i++) row_add_volume(&vols[i]);
    for (int i = 0; i < nd; i++) {
        const devinfo_usb_t *d = &devs[i];
        if (d->is_controller) continue;
        // #removtray dedup: defer storage classes to the volume rows above,
        // skip hubs (infrastructure) - identical rule to desktop_usbdev_tick().
        if (d->dev_class == 0x00 || d->dev_class == 0x08 || d->dev_class == 0x09) continue;
        row_add_usb(d);
    }
}

void traydev_poll(void) {
    static uint64_t s_last;
    if (desktop_is_dragging()) return;   // never rebuild mid-drag, same rule as desktop_volumes_tick()
    uint64_t now = uptime_ms();
    if (s_last != 0 && (now - s_last) < 1000) return;
    s_last = now;
    traydev_rebuild();
}

int traydev_present(void) { return g_row_n > 0; }

int traydev_rows(const traydev_row_t **out) {
    *out = g_rows;
    return g_row_n;
}

int traydev_eject(int vol_index) {
    if (vol_eject(vol_index) != 0) return -1;
    traydev_rebuild();   // #426: cheap (same two syscalls the throttled tick uses), not a poll loop
    return 0;
}
