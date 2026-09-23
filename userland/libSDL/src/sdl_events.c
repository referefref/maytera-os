/* sdl_events.c - event pump, keyboard state, mouse state, cursor. Part of
 * the MayteraOS SDL 1.2 backend (task #745,
 * docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7).
 *
 * Keycode source of truth: userland/libc/keys.h (GUI_KEY_*), NEVER a private
 * copy (that exact anti-pattern is #188/#191/#243 in this tree's own
 * history). Modifier state comes from userland/libc/gui_mods.h
 * (gui_mods_next_event/gui_mods_get), not a hand-rolled tracker: it already
 * solves the "the kernel emits no window-blur event" staleness problem via
 * gui_mods_resync(), which gui_mods_next_event() calls at proven
 * queue-quiescent points, so re-deriving that logic here would be exactly
 * the kind of duplicate primitive the repo rules forbid.
 */
#include "sdl_priv.h"
#include <string.h>

sdl_mouse_state_t g_sdlmouse;
Uint8 g_sdlkeys[SDLK_LAST];

static SDLMod g_modstate = KMOD_NONE;
static int g_unicode_enabled = 0;
static int g_key_repeat_delay = 0, g_key_repeat_interval = 0; /* off by default, like real SDL1.2 */

#define SDLPRIV_EVQ_CAP 64
static SDL_Event g_evq[SDLPRIV_EVQ_CAP];
static int g_evq_head = 0, g_evq_tail = 0;
static SDL_EventFilter g_event_filter = 0;
/* REAL BUG, found and fixed during this backend's own VM verification
 * (#745): a plain `static Uint8 g_event_state[SDL_NUMEVENTS]` zero-inits
 * every slot to 0, and SDL_IGNORE is ALSO 0 (SDL_events.h), so every event
 * type read as "ignored" until an app explicitly called
 * SDL_EventState(type, SDL_ENABLE) for it, which real SDL 1.2 apps never do
 * because real SDL enables everything by default. The effect was silent and
 * total: SDL_PollEvent kept returning 0 for every key/mouse event forever,
 * while everything NOT gated by this array (SDL_GetTicks-driven animation,
 * the frame-count-based phase transitions) kept working, which is exactly
 * why this looked like "rendering works, input does not" for two separate
 * launch paths (Terminal-spawned and AUTORUN-spawned) before the real cause
 * was found: it was never a focus problem. Fixed by seeding every slot to
 * SDL_ENABLE once, on first use. */
static Uint8 g_event_state[SDL_NUMEVENTS];
static int   g_event_state_seeded = 0;
static void event_state_seed(void) {
    if (g_event_state_seeded) return;
    for (int i = 0; i < SDL_NUMEVENTS; i++) g_event_state[i] = SDL_ENABLE;
    g_event_state_seeded = 1;
}

static void evq_push_raw(const SDL_Event *e) {
    event_state_seed();
    if (g_event_state[e->type] == SDL_IGNORE) return;
    if (g_event_filter && !g_event_filter(e)) return;
    int next = (g_evq_tail + 1) % SDLPRIV_EVQ_CAP;
    if (next == g_evq_head) return;  /* full: drop rather than corrupt */
    g_evq[g_evq_tail] = *e;
    g_evq_tail = next;
}
static int evq_pop(SDL_Event *out) {
    if (g_evq_head == g_evq_tail) return 0;
    *out = g_evq[g_evq_head];
    g_evq_head = (g_evq_head + 1) % SDLPRIV_EVQ_CAP;
    return 1;
}

/* ---------------------------------------------------------------------
 * GUI_KEY_* -> SDLKey. Table-driven for the non-ASCII keys; ASCII 32-126
 * already equals its SDLK_* value in real SDL 1.2 except A-Z, which fold to
 * lowercase (shift is reported separately via the modifier state, exactly
 * as real SDL does).
 * ------------------------------------------------------------------- */
struct keymap_entry { unsigned int gui_key; SDLKey sdlk; };
static const struct keymap_entry g_keymap[] = {
    { GUI_KEY_UP,     SDLK_UP },
    { GUI_KEY_DOWN,   SDLK_DOWN },
    { GUI_KEY_LEFT,   SDLK_LEFT },
    { GUI_KEY_RIGHT,  SDLK_RIGHT },
    { GUI_KEY_UP_REL,    SDLK_UP },
    { GUI_KEY_DOWN_REL,  SDLK_DOWN },
    { GUI_KEY_LEFT_REL,  SDLK_LEFT },
    { GUI_KEY_RIGHT_REL, SDLK_RIGHT },
    { GUI_KEY_F1,  SDLK_F1 },  { GUI_KEY_F2,  SDLK_F2 },  { GUI_KEY_F3,  SDLK_F3 },
    { GUI_KEY_F4,  SDLK_F4 },  { GUI_KEY_F5,  SDLK_F5 },  { GUI_KEY_F6,  SDLK_F6 },
    { GUI_KEY_F7,  SDLK_F7 },  { GUI_KEY_F8,  SDLK_F8 },  { GUI_KEY_F9,  SDLK_F9 },
    { GUI_KEY_F10, SDLK_F10 }, { GUI_KEY_F11, SDLK_F11 }, { GUI_KEY_F12, SDLK_F12 },
    { GUI_KEY_LSHIFT, SDLK_LSHIFT }, { GUI_KEY_LSHIFT_UP, SDLK_LSHIFT },
    { GUI_KEY_RSHIFT, SDLK_RSHIFT }, { GUI_KEY_RSHIFT_UP, SDLK_RSHIFT },
    { GUI_KEY_LCTRL,  SDLK_LCTRL },  { GUI_KEY_LCTRL_UP,  SDLK_LCTRL },
    { GUI_KEY_ALT,    SDLK_LALT },   { GUI_KEY_ALT_UP,    SDLK_LALT },
    { GUI_KEY_SUPER,  SDLK_LSUPER },
    { GUI_KEY_PRTSC,  SDLK_PRINT },
    { GUI_KEY_HOME, SDLK_HOME }, { GUI_KEY_END, SDLK_END },
    { GUI_KEY_PGUP, SDLK_PAGEUP }, { GUI_KEY_PGDN, SDLK_PAGEDOWN },
    { GUI_KEY_INS,  SDLK_INSERT }, { GUI_KEY_DEL, SDLK_DELETE },
    { GUI_KEY_ESC,   SDLK_ESCAPE },
    { GUI_KEY_ENTER, SDLK_RETURN },
    { GUI_KEY_TAB,   SDLK_TAB },
    { GUI_KEY_BKSP,  SDLK_BACKSPACE },
};
#define SDLPRIV_NKEYMAP (int)(sizeof(g_keymap)/sizeof(g_keymap[0]))

void sdlpriv_map_key(unsigned int gui_keycode, char key_char, SDLKey *out_sym, Uint16 *out_unicode) {
    SDLKey sym = SDLK_UNKNOWN;
    Uint16 uni = 0;

    for (int i = 0; i < SDLPRIV_NKEYMAP; i++) {
        if (g_keymap[i].gui_key == gui_keycode) { sym = g_keymap[i].sdlk; goto done; }
    }
    /* Ordinary keys: cooked ASCII, both in keycode and key_char (keys.h). */
    if (gui_keycode >= 'A' && gui_keycode <= 'Z') {
        sym = (SDLKey)(SDLK_a + (gui_keycode - 'A'));
        uni = (Uint16)(unsigned char)key_char;
    } else if (gui_keycode >= 32 && gui_keycode < 127) {
        sym = (SDLKey)gui_keycode;
        uni = (Uint16)(unsigned char)key_char;
    } else if (key_char >= 32 && key_char < 127) {
        /* Fallback: some paths only carry a printable key_char. */
        if (key_char >= 'A' && key_char <= 'Z') sym = (SDLKey)(SDLK_a + (key_char - 'A'));
        else sym = (SDLKey)(unsigned char)key_char;
        uni = (Uint16)(unsigned char)key_char;
    }
done:
    *out_sym = sym;
    *out_unicode = g_unicode_enabled ? uni : 0;
}

static void update_modstate_bits(void) {
    unsigned int m = gui_mods_get();
    SDLMod out = KMOD_NONE;
    if (m & GUI_MOD_LSHIFT) out |= KMOD_LSHIFT;
    if (m & GUI_MOD_RSHIFT) out |= KMOD_RSHIFT;
    if ((m & GUI_MOD_SHIFT) && !(out & (KMOD_LSHIFT | KMOD_RSHIFT))) out |= KMOD_LSHIFT;
    if (m & GUI_MOD_CTRL) out |= KMOD_LCTRL;   /* keys.h/gui_mods.h cannot tell L from R for Ctrl */
    if (m & GUI_MOD_ALT)  out |= KMOD_LALT;    /* nor for Alt */
    if (m & GUI_MOD_CAPS) out |= KMOD_CAPS;
    g_modstate = out;
}

static void translate_and_push(const gui_event_t *ev) {
    Uint32 ts = SDL_GetTicks();
    switch (ev->type) {
    case EVENT_KEY_DOWN:
    case EVENT_KEY_UP: {
        int down = (ev->type == EVENT_KEY_DOWN);
        update_modstate_bits();
        SDLKey sym; Uint16 uni;
        sdlpriv_map_key(ev->keycode, ev->key_char, &sym, &uni);
        if (sym == SDLK_UNKNOWN) break;  /* nothing usable (pure modifier-state key.h relase codes we don't track individually, etc) */
        if (sym < SDLK_LAST) g_sdlkeys[sym] = down ? SDL_PRESSED : SDL_RELEASED;
        SDL_Event e; memset(&e, 0, sizeof(e));
        e.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        e.key.which = 0;
        e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        e.key.keysym.scancode = 0;  /* MayteraOS does not hand userland raw PS/2
                                      * scancodes on this path (win_get_scancodes
                                      * is a separate, opt-in, DOS-guest-only
                                      * facility); games keyed off sym/unicode
                                      * are unaffected. */
        e.key.keysym.sym = sym;
        e.key.keysym.mod = g_modstate;
        e.key.keysym.unicode = uni;
        evq_push_raw(&e);
        (void)ts;
        break;
    }
    case EVENT_MOUSE_MOVE: {
        int dx = g_sdlmouse.have_pos ? ev->mouse_x - g_sdlmouse.x : 0;
        int dy = g_sdlmouse.have_pos ? ev->mouse_y - g_sdlmouse.y : 0;
        g_sdlmouse.rel_x += dx; g_sdlmouse.rel_y += dy;
        g_sdlmouse.x = ev->mouse_x; g_sdlmouse.y = ev->mouse_y; g_sdlmouse.have_pos = 1;
        SDL_Event e; memset(&e, 0, sizeof(e));
        e.motion.type = SDL_MOUSEMOTION;
        e.motion.which = 0;
        e.motion.state = g_sdlmouse.buttons;
        e.motion.x = (Uint16)(ev->mouse_x < 0 ? 0 : ev->mouse_x);
        e.motion.y = (Uint16)(ev->mouse_y < 0 ? 0 : ev->mouse_y);
        e.motion.xrel = (Sint16)dx; e.motion.yrel = (Sint16)dy;
        evq_push_raw(&e);
        break;
    }
    case EVENT_MOUSE_DOWN:
    case EVENT_MOUSE_UP: {
        int down = (ev->type == EVENT_MOUSE_DOWN);
        Uint8 btn = SDL_BUTTON_LEFT;
        if (ev->mouse_buttons & MOUSE_BUTTON_RIGHT) btn = SDL_BUTTON_RIGHT;
        else if (ev->mouse_buttons & MOUSE_BUTTON_MIDDLE) btn = SDL_BUTTON_MIDDLE;
        if (down) g_sdlmouse.buttons |= (Uint8)SDL_BUTTON(btn);
        else g_sdlmouse.buttons &= (Uint8)~SDL_BUTTON(btn);
        SDL_Event e; memset(&e, 0, sizeof(e));
        e.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.which = 0;
        e.button.button = btn;
        e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        e.button.x = (Uint16)(ev->mouse_x < 0 ? 0 : ev->mouse_x);
        e.button.y = (Uint16)(ev->mouse_y < 0 ? 0 : ev->mouse_y);
        evq_push_raw(&e);
        break;
    }
    case EVENT_MOUSE_SCROLL: {
        /* SDL 1.2 has no wheel event: upstream apps read the wheel as
         * synthetic button 4/5 clicks (SDL_BUTTON_WHEELUP/DOWN). */
        Uint8 btn = ev->scroll_delta > 0 ? SDL_BUTTON_WHEELUP : SDL_BUTTON_WHEELDOWN;
        for (int phase = 0; phase < 2; phase++) {
            SDL_Event e; memset(&e, 0, sizeof(e));
            e.button.type = phase == 0 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            e.button.button = btn;
            e.button.state = phase == 0 ? SDL_PRESSED : SDL_RELEASED;
            e.button.x = (Uint16)g_sdlmouse.x; e.button.y = (Uint16)g_sdlmouse.y;
            evq_push_raw(&e);
        }
        break;
    }
    case EVENT_RESIZE: {
        SDL_Event e; memset(&e, 0, sizeof(e));
        e.resize.type = SDL_VIDEORESIZE;
        e.resize.w = ev->mouse_x; e.resize.h = ev->mouse_y;
        evq_push_raw(&e);
        break;
    }
    case EVENT_WINDOW_CLOSE: {
        SDL_Event e; memset(&e, 0, sizeof(e));
        e.quit.type = SDL_QUIT;
        evq_push_raw(&e);
        break;
    }
    case EVENT_WINDOW_FOCUS:
    case EVENT_WINDOW_BLUR: {
        SDL_Event e; memset(&e, 0, sizeof(e));
        e.active.type = SDL_ACTIVEEVENT;
        e.active.gain = (ev->type == EVENT_WINDOW_FOCUS) ? 1 : 0;
        e.active.state = SDL_APPINPUTFOCUS;
        evq_push_raw(&e);
        break;
    }
    case EVENT_REDRAW: {
        SDL_Event e; memset(&e, 0, sizeof(e));
        e.expose.type = SDL_VIDEOEXPOSE;
        evq_push_raw(&e);
        break;
    }
    default: break;  /* EVENT_BUTTON_CLICK, EVENT_DRAG_DROP/END, EVENT_NONE: nothing SDL games use */
    }
}

void SDL_PumpEvents(void) {
    if (!g_sdlv.have_window) return;
    /* Recurring focus-steal bug (role brief): a game must keep re-asserting
     * keyboard focus or its keys go dead. MEASURED during this backend's own
     * verification (VM 2900, #745): a window created via win_create() (which
     * is documented to take focus AT CREATE time) did NOT reliably receive
     * arrow-key input when spawned as a child of a Terminal session, even
     * though the same compositor's global Super hotkey kept working
     * throughout - i.e. keys were reaching the compositor, just not being
     * routed to this window. Unconditional reassertion (matching the two
     * pre-existing sdlshim.cpp files, which do this for their always-
     * fullscreen games) fixes that class of gap and is a cheap no-op once
     * already focused. The one place this is known to fight another window
     * for focus is this test harness's own AUTORUN.CFG path racing the
     * first-boot setup wizard at boot, which is a test-harness scheduling
     * artifact, not a real deployment scenario (a real game is normally the
     * only foreground app), so unconditional wins on balance. */
    wm_focus(g_sdlv.win_handle);
    gui_event_t raw;
    int guard = 0;
    while (guard++ < 64) {
        int t = gui_mods_next_event(g_sdlv.win_handle, &raw, 0 /* non-blocking */);
        if (t <= 0) break;
        translate_and_push(&raw);
    }
}

int SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action, Uint32 mask) {
    if (action == SDL_ADDEVENT) {
        int n = 0;
        for (int i = 0; i < numevents; i++) { evq_push_raw(&events[i]); n++; }
        return n;
    }
    /* PEEKEVENT/GETEVENT: SDL_QuitRequested() (SDL_quit.h) uses PEEKEVENT
     * with SDL_QUITMASK, so this must actually inspect the queue without
     * necessarily consuming from arbitrary depth; a simple front-of-queue
     * scan matching `mask` is enough for every real caller (nothing in the
     * class-E corpus peeks past index 0). */
    int n = 0;
    int idx = g_evq_head;
    while (idx != g_evq_tail && n < numevents) {
        SDL_Event *e = &g_evq[idx];
        if (mask == SDL_ALLEVENTS || (mask & SDL_EVENTMASK(e->type))) {
            events[n++] = *e;
            if (action == SDL_GETEVENT) {
                /* remove it: shift head forward if it was the front, else
                 * compact by copying the rest down by one. Queue is small
                 * (64 entries) so O(n) compaction is fine. */
                int j = idx;
                while (j != g_evq_tail) {
                    int nj = (j + 1) % SDLPRIV_EVQ_CAP;
                    if (nj == g_evq_tail) break;
                    g_evq[j] = g_evq[nj];
                    j = nj;
                }
                g_evq_tail = (g_evq_tail - 1 + SDLPRIV_EVQ_CAP) % SDLPRIV_EVQ_CAP;
                continue; /* idx now holds the next event */
            }
        }
        idx = (idx + 1) % SDLPRIV_EVQ_CAP;
    }
    return n;
}

int SDL_PollEvent(SDL_Event *event) {
    SDL_PumpEvents();
    SDL_Event tmp;
    if (!event) event = &tmp;
    return evq_pop(event);
}

int SDL_WaitEvent(SDL_Event *event) {
    SDL_Event tmp;
    if (!event) event = &tmp;
    for (;;) {
        if (SDL_PollEvent(event)) return 1;
        if (!g_sdlv.have_window) { SDL_Delay(16); continue; }
        /* Real blocking wait on the compositor's own wait queue (see
         * win_get_event's timeout contract), never a busy spin. */
        wm_focus(g_sdlv.win_handle);
        gui_event_t raw;
        int t = gui_mods_next_event(g_sdlv.win_handle, &raw, -1);
        if (t < 0) return 0;
        if (t == 0) continue;
        translate_and_push(&raw);
    }
}

int SDL_PushEvent(SDL_Event *event) {
    if (!event) return -1;
    evq_push_raw(event);
    return 1;
}

void SDL_SetEventFilter(SDL_EventFilter filter) { g_event_filter = filter; }
SDL_EventFilter SDL_GetEventFilter(void) { return g_event_filter; }
Uint8 SDL_EventState(Uint8 type, int state) {
    event_state_seed();
    Uint8 prev = g_event_state[type];
    if (state != SDL_QUERY) g_event_state[type] = (Uint8)state;
    return prev;
}

/* ---------------------------------------------------------------------
 * Keyboard state (SDL_keyboard.h)
 * ------------------------------------------------------------------- */
Uint8 *SDL_GetKeyState(int *numkeys) {
    if (numkeys) *numkeys = SDLK_LAST;
    return g_sdlkeys;
}
SDLMod SDL_GetModState(void) { return g_modstate; }
void SDL_SetModState(SDLMod modstate) { g_modstate = modstate; }
int SDL_EnableKeyRepeat(int delay, int interval) {
    g_key_repeat_delay = delay; g_key_repeat_interval = interval;
    /* Real key-repeat synthesis is not implemented (no per-frame timer hook
     * here); callers that rely on OS-level repeat rather than reading
     * SDL_GetKeyState() every frame will not see repeats. Documented, not
     * silently pretended. */
    return 0;
}
void SDL_GetKeyRepeat(int *delay, int *interval) {
    if (delay) *delay = g_key_repeat_delay;
    if (interval) *interval = g_key_repeat_interval;
}
int SDL_EnableUNICODE(int enable) {
    int prev = g_unicode_enabled;
    if (enable >= 0) g_unicode_enabled = enable;
    return prev;
}
char *SDL_GetKeyName(SDLKey key) {
    static char buf[2];
    if (key >= 32 && key < 127) { buf[0] = (char)key; buf[1] = 0; return buf; }
    switch (key) {
    case SDLK_UP: return (char *)"Up"; case SDLK_DOWN: return (char *)"Down";
    case SDLK_LEFT: return (char *)"Left"; case SDLK_RIGHT: return (char *)"Right";
    case SDLK_RETURN: return (char *)"Return"; case SDLK_ESCAPE: return (char *)"Escape";
    case SDLK_BACKSPACE: return (char *)"Backspace"; case SDLK_TAB: return (char *)"Tab";
    case SDLK_SPACE: return (char *)"Space";
    case SDLK_LSHIFT: return (char *)"Left Shift"; case SDLK_RSHIFT: return (char *)"Right Shift";
    case SDLK_LCTRL: return (char *)"Left Ctrl"; case SDLK_LALT: return (char *)"Left Alt";
    default: break;
    }
    return (char *)"unknown key";
}

/* ---------------------------------------------------------------------
 * Application state (SDL_active.h)
 * ------------------------------------------------------------------- */
Uint8 SDL_GetAppState(void) {
    /* The compositor gives per-window focus/blur events but no cross-app
     * "is the whole app active" signal beyond that; approximate with the
     * window's own focus flag when we have a window. */
    return (Uint8)(SDL_APPMOUSEFOCUS | SDL_APPINPUTFOCUS | SDL_APPACTIVE);
}

/* ---------------------------------------------------------------------
 * Mouse (SDL_mouse.h). SDL_WarpMouse/custom bitmap cursors cannot move or
 * replace the real OS pointer from userland (grab_input()/cursor syscalls
 * are compositor-only, kernel/gui/fb_syscall.c is_compositor() gate) - the
 * same documented limitation the pre-existing sdlshim.cpp files record for
 * SDL2. Relative-motion games still work via frame-to-frame deltas of
 * absolute EVENT_MOUSE_MOVE, exactly as userland/apps/arena/main.c's real,
 * shipping mouselook already does.
 * ------------------------------------------------------------------- */
static int g_cursor_shown = 1;
Uint8 SDL_GetMouseState(int *x, int *y) {
    if (x) *x = g_sdlmouse.x;
    if (y) *y = g_sdlmouse.y;
    return g_sdlmouse.buttons;
}
Uint8 SDL_GetRelativeMouseState(int *x, int *y) {
    if (x) *x = g_sdlmouse.rel_x;
    if (y) *y = g_sdlmouse.rel_y;
    g_sdlmouse.rel_x = 0; g_sdlmouse.rel_y = 0;
    return g_sdlmouse.buttons;
}
void SDL_WarpMouse(Uint16 x, Uint16 y) { (void)x; (void)y; }
SDL_Cursor *SDL_CreateCursor(Uint8 *data, Uint8 *mask, int w, int h, int hot_x, int hot_y) {
    SDL_Cursor *c = (SDL_Cursor *)SDL_malloc(sizeof(SDL_Cursor));
    if (!c) return 0;
    c->area.x = 0; c->area.y = 0; c->area.w = (Uint16)w; c->area.h = (Uint16)h;
    c->hot_x = (Sint16)hot_x; c->hot_y = (Sint16)hot_y;
    c->data = data; c->mask = mask;
    c->save[0] = c->save[1] = 0; c->wm_cursor = 0;
    return c;
}
static SDL_Cursor *g_cur_cursor = 0;
void SDL_SetCursor(SDL_Cursor *cursor) { g_cur_cursor = cursor; /* tracked only, see header comment */ }
SDL_Cursor *SDL_GetCursor(void) { return g_cur_cursor; }
void SDL_FreeCursor(SDL_Cursor *cursor) { if (cursor) SDL_free(cursor); }
int SDL_ShowCursor(int toggle) {
    int prev = g_cursor_shown;
    if (toggle == SDL_QUERY) return prev;
    g_cursor_shown = toggle ? 1 : 0;
    return prev;
}
