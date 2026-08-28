// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_mods.c - #221 phase 0. THE modifier tracker. Read gui_mods.h first: it
// carries the measurement this file is built on and the reasoning about where
// the tracking is exact and where it is not. This file is deliberately small
// and dull; all the argument lives in the header.

#include "gui.h"
#include "keys.h"
#include "syscall.h"

// The whole state. One process, one keyboard, one focused window at a time,
// so one global is the right shape; a per-window tracker would have to be
// resynced on every focus change, which is precisely the event the kernel
// does not emit.
static unsigned int g_mods = 0;

unsigned int gui_mods_get(void) {
    return g_mods;
}

void gui_mods_clear(void) {
    g_mods = 0;
}

void gui_mods_resync(void) {
    // The kernel keeps ONE shift flag, not one per key, so a resync cannot
    // reconstruct which Shift is down. Keep whichever side was already
    // tracked if the kernel agrees shift is held, and claim neither if it
    // does not know; GUI_MOD_SHIFT stays authoritative either way.
    unsigned int k = sys_key_mods();
    unsigned int m = k & (GUI_MOD_SHIFT | GUI_MOD_CTRL | GUI_MOD_ALT | GUI_MOD_CAPS);
    if (m & GUI_MOD_SHIFT) {
        m |= (g_mods & (GUI_MOD_LSHIFT | GUI_MOD_RSHIFT));
    }
    g_mods = m;
}

int gui_mods_feed(const gui_event_t *ev) {
    if (!ev) return 0;

    if (ev->type == EVENT_KEY_DOWN) {
        switch (ev->keycode) {
            case GUI_KEY_LSHIFT: g_mods |= GUI_MOD_LSHIFT | GUI_MOD_SHIFT; return 1;
            case GUI_KEY_RSHIFT: g_mods |= GUI_MOD_RSHIFT | GUI_MOD_SHIFT; return 1;
            case GUI_KEY_LCTRL:  g_mods |= GUI_MOD_CTRL;                   return 1;
            case GUI_KEY_ALT:    g_mods |= GUI_MOD_ALT;                    return 1;
            // GUI_KEY_SUPER is NOT here on purpose: it has no release code,
            // so a bit set for it could only ever get stuck. See gui_mods.h.
            default: break;
        }
        return 0;
    }

    if (ev->type == EVENT_KEY_UP) {
        // These are the DELIVERED release values, which are NOT the
        // GUI_KEY_*_UP values the kernel pushes - SYS_INJECT_KEY rewrites them
        // on the way into gui_event_t. Matching GUI_KEY_LSHIFT_UP here would
        // simply never fire. keys.h has the measurement.
        switch (ev->keycode) {
            case GUI_KEY_LSHIFT_DELIVERED_REL:
                g_mods &= ~GUI_MOD_LSHIFT;
                if (!(g_mods & GUI_MOD_RSHIFT)) g_mods &= ~GUI_MOD_SHIFT;
                return 1;
            case GUI_KEY_RSHIFT_DELIVERED_REL:
                g_mods &= ~GUI_MOD_RSHIFT;
                if (!(g_mods & GUI_MOD_LSHIFT)) g_mods &= ~GUI_MOD_SHIFT;
                return 1;
            case GUI_KEY_LCTRL_DELIVERED_REL:
                g_mods &= ~GUI_MOD_CTRL;
                return 1;
            case GUI_KEY_ALT_DELIVERED_REL:
                g_mods &= ~GUI_MOD_ALT;
                return 1;
            default: break;
        }
        return 0;
    }

    // The day the kernel starts emitting focus events (it emits neither today;
    // EVENT_WINDOW_BLUR has exactly one occurrence in the kernel tree and it is
    // the enum declaration), these two lines are already right: regaining focus
    // is a quiescent moment, and losing it means every held bit is now
    // unobservable and must not be believed.
    if (ev->type == EVENT_WINDOW_FOCUS) { gui_mods_resync(); return 0; }
    if (ev->type == EVENT_WINDOW_BLUR)  { gui_mods_clear();  return 0; }

    return 0;
}

int gui_mods_next_event(int handle, gui_event_t *ev, int timeout) {
    int t = win_get_event(handle, ev, timeout);
    if (t > 0) {
        gui_mods_feed(ev);
        return t;
    }
    // t == 0 means the queue was empty and the wait expired (kernel
    // sys_win_get_event returns 0 for exactly that). THAT is the proven
    // quiescent moment, and the only one at which a live read of the physical
    // modifier state cannot contradict an event still queued behind it. A
    // negative t is a bad handle; resyncing then would be harmless but
    // pointless, so do not.
    if (t == 0) gui_mods_resync();
    return t;
}

int gui_mods_is(unsigned int want) {
    return (g_mods & GUI_MOD_CHORD) == (want & GUI_MOD_CHORD);
}

int gui_mods_letter(const gui_event_t *ev) {
    if (!ev) return 0;
    if (ev->type != EVENT_KEY_DOWN) return 0;
    unsigned char c = (unsigned char)ev->key_char;
    // Ctrl folded it to a control character (cpu/isr.c: c - 'a' + 1). This is
    // the ONLY branch that recovers a letter from a non-letter, and it is why
    // a shortcut table must not compare key_char directly.
    //
    // GATED ON CTRL BEING HELD, which matters: Tab, Enter and Backspace ARE
    // 0x09/0x0A/0x08 with no Ctrl anywhere near them, and ungated this would
    // report a bare Tab press as the letter 'i'. The kernel only produces
    // 0x01-0x1A from a LETTER when ctrl_pressed, so the gate is exact.
    //
    // WHAT REMAINS AMBIGUOUS, and is not fixable here: Ctrl+I and Tab are both
    // 0x09, Ctrl+J and Enter are both 0x0A, Ctrl+H and Backspace are both
    // 0x08, Ctrl+[ and Esc are both 0x1B. Every terminal ever written has this
    // ambiguity for the same reason. If you need Tab specifically, match
    // ev->keycode against GUI_KEY_TAB BEFORE asking for a letter.
    if ((g_mods & GUI_MOD_CTRL) && c >= 0x01 && c <= 0x1A) return 'a' + (c - 1);
    if (c >= 'A' && c <= 'Z')   return 'a' + (c - 'A');
    if (c >= 'a' && c <= 'z')   return (int)c;
    return 0;
}
