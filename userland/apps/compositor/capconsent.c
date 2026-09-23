// capconsent.c - Stage 1 capability consent prompt (compositor side).
//
// The DELIBERATELY MINIMAL Stage 1 surface. Stage 2 generalises the #745
// elevation modal into a shared, styled prompt with the tray indicator and the
// Settings manager (docs/SYSTEM_CAPABILITY_API.md section 12, Stage 2); this
// file is only enough to prove the end-to-end legitimate path: the COMPOSITOR,
// and only the compositor, draws the question and resolves it.
//
// THE TRUST PROPERTY, unchanged from #745 and from elevate.c: the requesting
// app never draws this and cannot resolve it. sys_cap_view / sys_cap_resolve
// are compositor-only in the kernel (fb_owner_is), so an app that is not the
// latched framebuffer owner gets CAP_EPERM. The kernel holds the grant; a
// resolve here writes it onto the REQUESTER's process_t, never this process's.
//
// A capability consent is a CONSENT, not an AUTHENTICATION: unlike elevation
// there is no password field. Enter = Allow, Esc = Deny, and the initial
// settling window discards type-ahead so a buffered keystroke cannot approve.
#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"

#define CAPC_SETTLE_MS 250

static int        g_capc_open;
static cap_view_t g_capc;
static unsigned long long g_capc_shown_ms;

int capconsent_open(void) { return g_capc_open; }

static const char *capc_verb(unsigned int cap)
{
    switch (cap) {
    case CAP_SCREEN_CAPTURE: return "capture your screen";
    case CAP_SCREEN_STREAM:  return "record your screen";
    case CAP_INPUT_INJECT:   return "control your keyboard and mouse";
    case CAP_INPUT_OBSERVE:  return "see everything you type";
    case CAP_AUDIO_OUTPUT:   return "change the system volume";
    case CAP_SERIAL_PORT:    return "use a serial port";
    case CAP_NET_CONNECT:    return "make network connections";
    default:                 return "use a system capability";
    }
}

// Mirror the kernel's live request. Called once per compositor frame from the
// main loop (not a busy-wait: it rides the frame cadence; returns immediately
// when there is nothing open).
void capconsent_poll(void)
{
    cap_view_t v;
    memset(&v, 0, sizeof(v));
    long r = sys_cap_view(&v);
    if (r != 1) {
        if (g_capc_open) { g_capc_open = 0; g_needs_redraw = true; }
        return;
    }
    if (!g_capc_open || v.seq != g_capc.seq) {
        g_capc = v;
        g_capc_open = 1;
        g_capc_shown_ms = uptime_ms();
        g_needs_redraw = true;
    } else {
        g_capc = v;
    }
}

void capconsent_render(void)
{
    if (!g_capc_open) return;
    fb_info_t fi;
    fb_info(&fi);
    int sw = (int)fi.width, sh = (int)fi.height;

    int pw = 540, ph = 210;
    if (pw > sw - 20) pw = sw - 20;
    if (ph > sh - 20) ph = sh - 20;
    int px = (sw - pw) / 2; if (px < 0) px = 0;
    int py = ((sh - ph) * 38) / 100; if (py < 40) py = 40;

    // Scrim: nothing behind is interactive while this is up, so nothing behind
    // should look interactive.
    g_draw_blend = 180;
    draw_fill_rect(0, 0, sw, sh, 0xFF101018);
    g_draw_blend = 255;

    // Panel with a two-tone edge.
    draw_fill_rect(px + 3, py + 3, pw, ph, 0xFF000000);
    draw_fill_rect(px, py, pw, ph, 0xFF2B2B34);
    draw_fill_rect(px, py, pw, 1, 0xFF000000);
    draw_fill_rect(px, py + ph - 1, pw, 1, 0xFF000000);
    draw_fill_rect(px, py, 1, ph, 0xFF000000);
    draw_fill_rect(px + pw - 1, py, 1, ph, 0xFF000000);

    // Title is FIXED CHROME: never contains app-supplied text, so the first
    // thing the eye lands on is a sentence the app could not write.
    draw_text_ttf(px + 24, py + 26, "Permission request", 22, 0xFFFFFFFF);
    draw_fill_rect(px + 24, py + 44, pw - 48, 1, 0xFF555560);

    // The one place app-supplied text appears: the app name, kernel-sanitised.
    char line[240];
    const char *app = g_capc.app[0] ? g_capc.app : "An application";
    // #capstage4: a cross-app input.inject (WINDOW_TARGET) is named for what it
    // really is - driving ANOTHER app's window - so the human is not told merely
    // "control your keyboard and mouse" when the real ask is to drive one named
    // window belonging to a different app.
    const char *verb =
        (g_capc.cap == CAP_INPUT_INJECT && g_capc.scope_kind == CAP_SCOPE_WINDOW_TARGET)
            ? "control another app's window"
            : capc_verb(g_capc.cap);
    snprintf(line, sizeof(line), "%s wants to %s.", app, verb);
    draw_text_ttf(px + 24, py + 74, line, 16, 0xFFDDDDDD);

    if (g_capc.scope[0]) {
        char sline[220];
        // The label follows the scope KIND. A WINDOW_TARGET scope is the bound
        // "<winid>:<title>" the KERNEL authored from the target window's own
        // title; show just the title so the human sees exactly WHICH window the
        // app is being allowed to drive - named by the kernel, not by the app.
        // A path is where output is saved; a port name is which serial port.
        if (g_capc.scope_kind == CAP_SCOPE_WINDOW_TARGET) {
            const char *title = g_capc.scope;
            const char *colon = title;
            while (*colon && *colon != ':') colon++;
            if (*colon == ':') title = colon + 1;
            snprintf(sline, sizeof(sline), "Target window: %s",
                     title[0] ? title : g_capc.scope);
        } else {
            const char *label = (g_capc.scope_kind == CAP_SCOPE_PORT) ? "Port: "
                                                                      : "Saving to: ";
            snprintf(sline, sizeof(sline), "%s%s", label, g_capc.scope);
        }
        draw_text_ttf(px + 24, py + 100, sline, 14, 0xFFAAAAAA);
    }

    char dline[80];
    if (g_capc.duration_ms == 0)
        snprintf(dline, sizeof(dline), "Duration: once");
    else
        snprintf(dline, sizeof(dline), "Duration: %u minute(s)",
                 (unsigned)(g_capc.duration_ms / 60000u > 0 ? g_capc.duration_ms / 60000u : 1u));
    draw_text_ttf(px + 24, py + 124, dline, 14, 0xFFAAAAAA);

    draw_text_ttf(px + 24, py + ph - 34, "[Enter] Allow        [Esc] Deny", 15, 0xFFCFCFE0);
}

// Modal-group key handler. Enter approves, Esc denies. Everything in the first
// CAPC_SETTLE_MS is discarded so buffered type-ahead cannot approve.
int capconsent_handle_key(int key)
{
    if (!g_capc_open) return 0;
    if (uptime_ms() - g_capc_shown_ms < CAPC_SETTLE_MS) return 1;

    if (key == 0x1B) {   // Esc always denies
        sys_cap_resolve(g_capc.seq, CAP_ACT_DENY);
        g_capc_open = 0;
        g_needs_redraw = true;
        return 1;
    }
    if (key == '\n' || key == '\r') {
        sys_cap_resolve(g_capc.seq, CAP_ACT_APPROVE);
        g_capc_open = 0;
        g_needs_redraw = true;
        return 1;
    }
    return 1;   // swallow every other key while the modal is up
}

// Exclusive modal: swallow the pointer too, but it has no clickable controls in
// Stage 1 (keyboard-driven). A click does nothing rather than leaking to a
// window behind the scrim.
int capconsent_handle_mouse(int32_t x, int32_t y, int clicked)
{
    (void)x; (void)y; (void)clicked;
    return g_capc_open ? 1 : 0;
}
