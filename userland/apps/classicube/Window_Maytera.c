#include "Core.h"
#if defined CC_BUILD_MAYTERA
/*
   ClassiCube window + input backend for MayteraOS (task #28).

   Contract shape follows Window_X11.c. Everything below was derived by reading
   the MayteraOS sources, not by analogy:

     kernel/cpu/isr.h            translated key codes (0x80-0x9C)
     kernel/cpu/isr.c            scancode -> key code translation + which keys
                                 do and do not emit a release
     kernel/proc/syscall.c       SYS_INJECT_KEY  (press/release classification
                                 that userland actually receives)
                                 sys_win_create / sys_win_blit /
                                 sys_win_draw_image / sys_win_get_event /
                                 user_window_event_handler / user_window_handle_resize
     kernel/gui/window.c         wm_inject_app_mouse (button encoding)
     kernel/gui/fb_syscall.c     is_compositor() gate on the pointer syscalls
     userland/libc/gui.h         gui_event_t / EVENT_*
     userland/libc/syscall.h     win_*, get_global_mouse, fb_info, clipboard_*

   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
#include "_WindowBase.h"
#include "String_.h"
#include "Funcs.h"
#include "Bitmap.h"
#include "Errors.h"
#include "Options.h"
#include "Platform.h"

#include "syscall.h"
#include "gui.h"

/* THE FORMAT GATE, in the translation unit that owns the blit.
   Window_DrawFramebuffer below hands bmp->scan0 to SYS_WIN_BLIT /
   SYS_WIN_DRAW_IMAGE with NO conversion, which is correct only while
   ClassiCube's BitmapCol layout equals the MayteraOS 0xAARRGGBB window
   word. This turns a future divergence into a BUILD ERROR here rather
   than a world rendered with red and blue exchanged. Before #800 the
   macro existed but was invoked only by the offscreen test harness, so
   it guarded nothing that shipped. */
#include "gfx/mos_gfx.h"
MOS_GFX_ASSERT_PIXEL_FORMAT();

/*########################################################################################################################*
*------------------------------------------------------Window state-------------------------------------------------------*
*#########################################################################################################################*/
#define MOS_WINDOW_TITLE "ClassiCube"

static int  win_handle = -1;      /* MayteraOS window handle (0..MAX_USER_WINDOWS) */
static int  win_outer_w, win_outer_h;
static cc_bool win_nochrome;      /* win_set_nochrome() applied (one way, kernel side) */
static cc_bool win_closing;

static cc_bool TitleMatches(const char* title) {
	const char* want = MOS_WINDOW_TITLE;
	int i;
	for (i = 0; want[i]; i++) { if (title[i] != want[i]) return false; }
	return title[i] == '\0';
}

/* SYS_WIN_CREATE TAKES THE *OUTER* SIZE (frame + titlebar), not the content
   size: kernel/proc/syscall.c sys_win_create() passes width/height straight to
   window_create(), and window_get_content_bounds() then subtracts the chrome.
   With the default theme metrics (kernel/gui/window.h: TITLEBAR_HEIGHT 20,
   BORDER_WIDTH 2) the content area is (outer-4) x (outer-24).

   Those two numbers are THEME METRICS (TM_TITLEBAR_H / TM_BORDER_W), so they
   are not constants. We therefore ask for content+4/+24 and then immediately
   read back the TRUE content size with win_get_size(), which is authoritative
   whatever the theme says. Never assume the requested size was honoured. */
#define MOS_CHROME_W 4
#define MOS_CHROME_H 24

/*########################################################################################################################*
*----------------------------------------------Deferred key releases (see below)-----------------------------------------*
*#########################################################################################################################*/
/* kernel/cpu/isr.c only emits a release event for:
     - the four extended arrows      (0x90-0x93)
     - Ctrl / Shift / Alt            (0x94, 0x97, 0x98, 0x9C)
     - PRINTABLE ascii 0x20-0x7E     (char | 0x80)
   F1-F12, Super, and the ASCII control keys (Backspace 0x08, Tab 0x09,
   Enter 0x0A, Escape 0x1B) deliberately emit NOTHING on release, because their
   release byte would collide with the special-key range. Without compensation
   those keys latch down forever in Input.Pressed[]. We therefore queue a
   synthetic release, flushed at the start of the next Window_ProcessEvents so
   the key still reads as pressed for one full frame. */
#define MOS_DEFERRED_MAX 16
static int deferred_up[MOS_DEFERRED_MAX];
static int deferred_count;

static void DeferRelease(int key) {
	if (!key) return;
	if (deferred_count >= MOS_DEFERRED_MAX) return;
	deferred_up[deferred_count++] = key;
}

static void FlushDeferredReleases(void) {
	int i;
	for (i = 0; i < deferred_count; i++) Input_SetReleased(deferred_up[i]);
	deferred_count = 0;
}

/*########################################################################################################################*
*--------------------------------------------------------Key mapping------------------------------------------------------*
*#########################################################################################################################*/
/* Special-key PRESS codes. Values from kernel/cpu/isr.h. Note the ordering is
   NOT sequential by function key number; it is exactly as the kernel defines
   it, and the gaps (0x8A = F6, 0x87 = F10) are real. */
static int MapSpecialKey(unsigned int code) {
	switch (code) {
	case 0x80: return CCKEY_UP;
	case 0x81: return CCKEY_DOWN;
	case 0x82: return CCKEY_LEFT;
	case 0x83: return CCKEY_RIGHT;
	case 0x84: return CCKEY_F5;
	case 0x85: return CCKEY_F11;
	case 0x86: return CCKEY_F12;
	case 0x87: return CCKEY_F10;
	case 0x88: return CCKEY_F1;
	case 0x89: return CCKEY_F2;
	case 0x8A: return CCKEY_F6;
	case 0x8B: return CCKEY_F3;
	case 0x8C: return CCKEY_F4;
	case 0x8D: return CCKEY_F7;
	case 0x8E: return CCKEY_F8;
	case 0x8F: return CCKEY_F9;
	/* Modifier PRESS codes. These live above 0x8F because 0x80-0x8F is fully
	   occupied; kernel/proc/syscall.c SYS_INJECT_KEY special-cases exactly
	   this set so they arrive as EVENT_KEY_DOWN and not as a bogus release. */
	case 0x95: return CCKEY_LSHIFT;
	case 0x96: return CCKEY_RSHIFT;
	case 0x99: return CCKEY_LCTRL;
	case 0x9A: return CCKEY_LALT;   /* left AND right Alt both arrive as 0x9A */
	case 0x9B: return CCKEY_LWIN;   /* Super/GUI, either side */
	}
	return INPUT_NONE;
}

/* The ASCII mapping below indexes the CCKEY_A.. and CCKEY_0.. runs by
   arithmetic. Input.h comments claim they are contiguous ("same as 'A'-'Z'",
   "same as '0'-'9'"); assert it so an upstream enum reshuffle is a build break
   instead of 26 silently wrong keys. */
_Static_assert(CCKEY_Z - CCKEY_A == 25, "CCKEY_A..CCKEY_Z must be contiguous");
_Static_assert(CCKEY_9 - CCKEY_0 == 9,  "CCKEY_0..CCKEY_9 must be contiguous");

/* Printable / control ASCII, used for BOTH press and release.
   Shifted and unshifted forms map to the same CC key on purpose: the kernel
   applies the shift table before we see the byte, so pressing 'a' and then
   releasing it while Shift happens to be held yields 0x61 then 0x41. Folding
   both onto CCKEY_A is what keeps press/release balanced. */
static int MapAsciiKey(unsigned int c) {
	if (c >= 'a' && c <= 'z') return CCKEY_A + (c - 'a');
	if (c >= 'A' && c <= 'Z') return CCKEY_A + (c - 'A');
	if (c >= '0' && c <= '9') return CCKEY_0 + (c - '0');

	switch (c) {
	case 0x08: return CCKEY_BACKSPACE;
	case 0x09: return CCKEY_TAB;
	case 0x0A: case 0x0D: return CCKEY_ENTER;
	case 0x1B: return CCKEY_ESCAPE;
	case ' ':  return CCKEY_SPACE;

	case '-': case '_':  return CCKEY_MINUS;
	case '=': case '+':  return CCKEY_EQUALS;
	case '[': case '{':  return CCKEY_LBRACKET;
	case ']': case '}':  return CCKEY_RBRACKET;
	case '\\': case '|': return CCKEY_BACKSLASH;
	case ';': case ':':  return CCKEY_SEMICOLON;
	case '\'': case '"': return CCKEY_QUOTE;
	case ',': case '<':  return CCKEY_COMMA;
	case '.': case '>':  return CCKEY_PERIOD;
	case '/': case '?':  return CCKEY_SLASH;
	case '`': case '~':  return CCKEY_TILDE;

	/* Shifted number row: the kernel already applied the shift table, so the
	   digit identity only survives here. */
	case '!': return CCKEY_1;
	case '@': return CCKEY_2;
	case '#': return CCKEY_3;
	case '$': return CCKEY_4;
	case '%': return CCKEY_5;
	case '^': return CCKEY_6;
	case '&': return CCKEY_7;
	case '*': return CCKEY_8;
	case '(': return CCKEY_9;
	case ')': return CCKEY_0;
	}
	return INPUT_NONE;
}

/* A key that produces no hardware release event, so we must synthesize one. */
static cc_bool KeyNeedsSyntheticRelease(unsigned int code) {
	if (code >= 0x84 && code <= 0x8F) return true;   /* F1-F12 */
	if (code == 0x9B) return true;                   /* Super */
	if (code < 0x20)  return true;                   /* Backspace/Tab/Enter/Esc */
	if (code > 0x7E && code < 0x80) return true;
	return false;
}

/* RELEASE codes as they actually reach userland.

   kernel/proc/syscall.c SYS_INJECT_KEY rewrites the raw byte before queueing:
     raw 0x90-0x98 -> EVENT_KEY_UP with keycode (raw - 0x10)
     raw > 0x98    -> EVENT_KEY_UP with keycode (raw & 0x7F)
   0x95/0x96/0x99/0x9A/0x9B are intercepted as presses first, so the release
   keycodes that can actually occur are:

     0x80..0x83  arrows          (round-trips correctly)
     0x84        Ctrl release    (raw 0x94, collides with the F5 PRESS code)
     0x87        LShift release  (raw 0x97, collides with the F10 PRESS code)
     0x88        RShift release  (raw 0x98, collides with the F1 PRESS code)
     0x1C        Alt release     (raw 0x9C)
     0x20..0x7E  ascii release

   The collisions are harmless ONLY because F1/F5/F10 emit no release of their
   own (scancode_to_ascii[] is 0 for every function key, so isr.c's
   "printable only" release filter drops them). That is verified, not assumed;
   if a future kernel starts emitting F-key releases this decode breaks and
   Ctrl/Shift will latch. Kept as a single function so there is one place to
   fix. */
static int MapReleaseKey(unsigned int code) {
	switch (code) {
	case 0x80: return CCKEY_UP;
	case 0x81: return CCKEY_DOWN;
	case 0x82: return CCKEY_LEFT;
	case 0x83: return CCKEY_RIGHT;
	case 0x84: return CCKEY_LCTRL;
	case 0x87: return CCKEY_LSHIFT;
	case 0x88: return CCKEY_RSHIFT;
	case 0x1C: return CCKEY_LALT;
	}
	if (code >= 0x20 && code <= 0x7E) return MapAsciiKey(code);
	return INPUT_NONE;
}

/*########################################################################################################################*
*----------------------------------------------------Raw mouse / mouselook------------------------------------------------*
*#########################################################################################################################*/
/* POINTER GRAB / WARP VERDICT (measured, not assumed):

   MayteraOS has exactly three pointer-control syscalls and ALL THREE are gated
   on is_compositor() in kernel/gui/fb_syscall.c, i.e. only the single process
   that first mapped the framebuffer may call them:

     SYS_SET_MOUSE   (211) set_mouse_pos()  - warp, returns -1 to a normal app
     SYS_GRAB_INPUT  (213) grab_input()     - returns -1 to a normal app
     SYS_GET_MOUSE   (210) get_mouse()      - returns -1 to a normal app

   So a ClassiCube process CANNOT warp or grab the pointer, and there is no
   protocol by which it can ask the compositor to do it on its behalf. This is
   the same wall userland/apps/assaultcube/sdlshim.cpp documents and the same
   one userland/apps/arena/main.c lives with.

   What we DO have, ungated for any process:
     SYS_GET_GLOBAL_MOUSE (264) get_global_mouse() - read-only absolute cursor.
   sys_get_global_mouse() in fb_syscall.c has no is_compositor() check.

   So raw look is implemented as frame-to-frame deltas of the ABSOLUTE screen
   cursor (Cursor_GetRawPos below), which is strictly better than deltas of the
   window-relative EVENT_MOUSE_MOVE stream because it does not depend on event
   delivery or on the cursor staying inside our window. It still has ONE real
   defect and it is not papered over: the kernel clamps the cursor to the
   screen edges (sys_set_mouse / the PS/2 path both clamp to g_fb_width/Height),
   so once the physical cursor reaches an edge, delta goes to zero and the view
   stops turning until the user drags back. On a 1280-wide screen that is about
   a 3.5 turn budget before you must re-centre by hand.

   Keyboard look (below) is the shipped fallback so the game is playable
   regardless. See the report / CHANGELOG for the exact compositor+kernel
   support that would remove the defect. */

static int  raw_prev_x, raw_prev_y;
static cc_bool raw_have_prev;

/* Keyboard look: while raw mode is on (i.e. the game has grabbed the camera),
   held arrow keys synthesize the same RawMoved deltas the mouse would.
   Routed through PointerEvents.RawMoved rather than the BIND_LOOK_* bindings
   because those default to INPUT_NONE (Input.c KeyBind_Defaults) and are
   overwritten when options load, so binding them here would silently stop
   working. This path cannot be clobbered by options. */
#define MOS_KBD_LOOK_SPEED 320.0f   /* pixels of equivalent mouse motion per second */

static void Cursor_GetRawPos(int* x, int* y) {
	int sx = 0, sy = 0;
	unsigned int buttons = 0;
	if (get_global_mouse(&sx, &sy, &buttons) != 0) { sx = 0; sy = 0; }
	*x = sx; *y = sy;
}

void Cursor_SetPosition(int x, int y) {
	/* Deliberately a no-op: set_mouse_pos() is compositor-only and returns -1
	   here. Doing it anyway would look like it worked while silently failing,
	   which is exactly the class of bug this port is meant to avoid. */
	(void)x; (void)y;
}

static void Cursor_DoSetVisible(cc_bool visible) {
	/* The cursor is drawn by the compositor and there is no per-app hide.
	   Recorded in DisplayInfo.CursorVisible by the caller; nothing to do. */
	(void)visible;
}

void Window_EnableRawMouse(void) {
	Input.RawMode = true;
	Cursor_GetRawPos(&raw_prev_x, &raw_prev_y);
	raw_have_prev = true;
	Cursor_SetVisible(false);
}

static void KeyboardLook(float delta);
static float raw_frame_delta;

void Window_UpdateRawMouse(void) {
	int x, y;
	Cursor_GetRawPos(&x, &y);
	if (raw_have_prev && (x != raw_prev_x || y != raw_prev_y)) {
		Event_RaiseRawMove(&PointerEvents.RawMoved,
			(float)(x - raw_prev_x), (float)(y - raw_prev_y));
	}
	raw_prev_x = x; raw_prev_y = y;
	raw_have_prev = true;
	/* No CentreMousePosition(): we cannot warp. See the verdict block above. */

	/* Keyboard look rides the same call. Camera.c invokes Window_UpdateRawMouse
	   only when (!Gui.InputGrab && Window_Main.Focused), which is exactly when
	   look input should be accepted, so hanging it here rather than off the
	   message pump means arrow keys do not steer the camera while a menu or
	   the chat input has the grab. */
	KeyboardLook(raw_frame_delta);
}

void Window_DisableRawMouse(void) {
	Input.RawMode = false;
	raw_have_prev = false;
	Cursor_SetVisible(true);
}

static void KeyboardLook(float delta) {
	float amount;
	float dx = 0, dy = 0;
	if (!Input.RawMode) return;
	if (delta <= 0) return;

	amount = MOS_KBD_LOOK_SPEED * delta;
	if (Input.Pressed[CCKEY_LEFT])  dx -= amount;
	if (Input.Pressed[CCKEY_RIGHT]) dx += amount;
	if (Input.Pressed[CCKEY_UP])    dy -= amount;
	if (Input.Pressed[CCKEY_DOWN])  dy += amount;

	if (dx != 0 || dy != 0) Event_RaiseRawMove(&PointerEvents.RawMoved, dx, dy);
}

/*########################################################################################################################*
*--------------------------------------------------------Window------------------------------------------------------------*
*#########################################################################################################################*/
void Window_PreInit(void) {
	DisplayInfo.CursorVisible = true;
}

void Window_Init(void) {
	fb_info_t fi;

	/* sys_fb_info() is NOT behind the is_compositor() gate (unlike sys_fb_map),
	   so a normal app may read the screen geometry. Checked in
	   kernel/gui/fb_syscall.c. */
	if (fb_info(&fi) == 0 && fi.width && fi.height) {
		DisplayInfo.Width  = (int)fi.width;
		DisplayInfo.Height = (int)fi.height;
		DisplayInfo.Depth  = (int)fi.bpp;
	} else {
		DisplayInfo.Width  = 1280;
		DisplayInfo.Height = 800;
		DisplayInfo.Depth  = 32;
	}
	DisplayInfo.ScaleX = 1;
	DisplayInfo.ScaleY = 1;
	Input.Sources = INPUT_SOURCE_NORMAL;
}

void Window_Free(void) { }

static void DoCreateWindow(int width, int height) {
	int cw = 0, ch = 0;

	if (width  < 64) width  = 64;
	if (height < 64) height = 64;

	win_outer_w = width  + MOS_CHROME_W;
	win_outer_h = height + MOS_CHROME_H;
	if (win_outer_w > DisplayInfo.Width)  win_outer_w = DisplayInfo.Width;
	if (win_outer_h > DisplayInfo.Height) win_outer_h = DisplayInfo.Height;

	win_handle = win_create(MOS_WINDOW_TITLE,
		Display_CentreX(win_outer_w), Display_CentreY(win_outer_h),
		win_outer_w, win_outer_h);
	if (win_handle < 0) Process_Abort("Failed to create window");

	/* Authoritative content size, whatever the theme's chrome metrics are. */
	if (win_get_size(win_handle, &cw, &ch) != 0 || cw <= 0 || ch <= 0) {
		cw = width; ch = height;
	}

	Window_Main.Handle.val = win_handle;
	Window_Main.Width      = cw;
	Window_Main.Height     = ch;
	Window_Main.Exists     = true;
	Window_Main.Focused    = true;   /* sys_win_create() calls wm_focus_window() */
	Window_Main.UIScaleX   = DEFAULT_UI_SCALE_X;
	Window_Main.UIScaleY   = DEFAULT_UI_SCALE_Y;
	win_closing = false;
}

void Window_Create2D(int width, int height) {
	Window_Main.Is3D = false;
	DoCreateWindow(width, height);
}

void Window_Create3D(int width, int height) {
	Window_Main.Is3D = true;
	DoCreateWindow(width, height);
}

/* DOES NOT clear Window_Main.Exists, and that is deliberate.

   main_impl.h's RunLauncher() and RunGame() BOTH end by calling this, and
   main()'s single-process loop re-runs RunProgram while Window_Main.Exists is
   true. Clearing it here (which is what Window_X11.c does) made the launcher's
   "Singleplayer" button exit the whole process with status 0 and no error,
   because the loop condition went false the instant the launcher finished.
   Window_X11.c can afford it: on POSIX the launcher forks and execs a second
   copy of the program, so its process is supposed to die. Ours is
   PLAT_FLAG_SINGLE_PROCESS and Process_StartGame2 is SetGameArgs, so the same
   process must carry on into the game. The console backends, the other
   single-process platforms, do it this way too (xbox/Window_Xbox.c never
   clears Exists).

   Window_RequestClose() is what clears Exists now: it is the one event that
   really means the user is leaving. */
void Window_Destroy(void) {
	if (win_handle >= 0) win_destroy(win_handle);
	win_handle = -1;
}

/* MayteraOS has no set-title syscall. sys_win_create() copies the title into the
   window once, and kernel/proc/syscall.c exposes no SYS_WIN_SET_TITLE. Grepping
   userland/libc/syscall.h for the win_ family gives exactly: win_create,
   win_destroy, win_draw_rect, win_draw_pixel, win_draw_text, win_draw_text_small,
   win_draw_text_ttf, win_draw_text_ttf_ex, win_draw_image, win_get_event,
   win_invalidate, win_get_size, win_get_pos, win_move, win_move_by,
   win_set_nochrome, and SYS_WIN_BLIT. There is no title setter, so the titlebar
   keeps whatever was passed at create time. Not faked, not silently swallowed. */
void Window_SetTitle(const cc_string* title) {
	(void)title;
}

void Window_Show(void) {
	/* sys_win_create() already calls window_show() + wm_focus_window(). */
}

void Window_SetSize(int width, int height) {
	/* No app-side resize syscall exists (see Window_SetTitle note). The window
	   can only change size when the USER drags the resize grip, which arrives
	   as EVENT_RESIZE. Report the request as unhonoured by leaving
	   Window_Main.Width/Height alone; ClassiCube copes with that. */
	(void)width; (void)height;
}

void Window_RequestClose(void) {
	win_closing = true;
	/* Ends main()'s single-process launcher -> game -> launcher loop. Every
	   caller of this function is a genuine "the user is leaving" signal: the
	   compositor's EVENT_WINDOW_CLOSE, the titlebar X (detected by the window
	   vanishing from sys_wm_get_windows), or the engine asking to quit. The
	   per-screen teardown in Window_Destroy() must NOT clear this, or the
	   launcher can never hand over to the game. */
	Window_Main.Exists = false;
	Event_RaiseVoid(&WindowEvents.Closing);
}

int Window_GetWindowState(void) {
	return win_nochrome ? WINDOW_STATE_FULLSCREEN : WINDOW_STATE_NORMAL;
}

cc_result Window_EnterFullscreen(void) {
	int cw = 0, ch = 0;
	if (win_handle < 0) return ERR_NOT_SUPPORTED;
	if (win_nochrome)   return 0;

	/* win_set_nochrome() drops the border + titlebar, so the content area grows
	   to the FULL outer rect (kernel window_get_content_bounds() returns
	   bounds.* verbatim for WINDOW_FLAG_NOCHROME). Moving to 0,0 first means
	   a window created at screen size then covers the whole screen. */
	win_move(win_handle, 0, 0);
	if (win_set_nochrome(win_handle) != 0) return ERR_NOT_SUPPORTED;
	win_nochrome = true;

	if (win_get_size(win_handle, &cw, &ch) == 0 && cw > 0 && ch > 0) {
		Window_Main.Width  = cw;
		Window_Main.Height = ch;
	}
	Event_RaiseVoid(&WindowEvents.Resized);
	Event_RaiseVoid(&WindowEvents.StateChanged);
	return 0;
}

cc_result Window_ExitFullscreen(void) {
	/* sys_win_set_nochrome() only ever ORs WINDOW_FLAG_NOCHROME in; there is no
	   clear path in the kernel, so this is genuinely one-way. Reported as
	   unsupported rather than silently doing nothing and claiming success. */
	return ERR_NOT_SUPPORTED;
}

int Window_IsObscured(void) { return 0; }

void Window_LockLandscapeOrientation(cc_bool lock) { (void)lock; }

/*########################################################################################################################*
*-------------------------------------------------------Message pump------------------------------------------------------*
*#########################################################################################################################*/
static void HandleKeyDown(const gui_event_t* ev) {
	unsigned int code = ev->keycode;
	int key;

	if (code >= 0x80) {
		key = MapSpecialKey(code);
	} else {
		key = MapAsciiKey(code);
	}
	if (key == INPUT_NONE) return;

	Input_SetPressed(key);
	if (KeyNeedsSyntheticRelease(code)) DeferRelease(key);

	/* Text input. key_char is only populated by the kernel for the plain ASCII
	   branch (kernel/proc/syscall.c SYS_INJECT_KEY), which is exactly the set
	   we want to feed to the character event. */
	if (ev->key_char >= 0x20 && (unsigned char)ev->key_char < 0x7F) {
		Event_RaiseInt(&InputEvents.Press, (unsigned char)ev->key_char);
	}
}

static void HandleKeyUp(const gui_event_t* ev) {
	int key = MapReleaseKey(ev->keycode);
	if (key != INPUT_NONE) Input_SetReleased(key);
}

static void HandleMouseButton(const gui_event_t* ev, cc_bool down) {
	/* kernel/gui/window.c wm_inject_app_mouse() collapses the button to a
	   single bit: MOUSE_BUTTON_RIGHT when the compositor passed button==2,
	   MOUSE_BUTTON_LEFT for everything else. There is therefore NO way to
	   receive a middle click through this path, and MOUSE_BUTTON_MIDDLE never
	   appears in an app-bound event. Not emulated. */
	int btn = (ev->mouse_buttons & MOUSE_BUTTON_RIGHT) ? CCMOUSE_R : CCMOUSE_L;
	if (down) Input_SetPressed(btn); else Input_SetReleased(btn);
}

void Window_ProcessEvents(float delta) {
	gui_event_t ev;
	int type;
	int guard;

	FlushDeferredReleases();

	if (win_handle < 0) return;

	/* Non-blocking drain (timeout 0). kernel sys_win_get_event() returns 0
	   immediately when the queue is empty and only sleeps on the per-window
	   wait queue for a non-zero timeout, so this never spins and never blocks
	   the render loop. Bounded so a flood cannot stall a frame. */
	for (guard = 0; guard < 256; guard++) {
		type = win_get_event(win_handle, &ev, 0);
		if (type <= 0) break;

		switch (type) {
		case EVENT_KEY_DOWN:
			HandleKeyDown(&ev);
			break;
		case EVENT_KEY_UP:
			HandleKeyUp(&ev);
			break;
		case EVENT_MOUSE_MOVE:
			/* Already translated to CONTENT coordinates by the kernel
			   (user_window_event_handler subtracts the content origin). */
			Pointer_SetPosition(0, ev.mouse_x, ev.mouse_y);
			break;
		case EVENT_MOUSE_DOWN:
			Pointer_SetPosition(0, ev.mouse_x, ev.mouse_y);
			HandleMouseButton(&ev, true);
			break;
		case EVENT_MOUSE_UP:
			Pointer_SetPosition(0, ev.mouse_x, ev.mouse_y);
			HandleMouseButton(&ev, false);
			break;
		case EVENT_MOUSE_SCROLL:
			/* MayteraOS: positive scroll_delta means scroll DOWN (verified
			   against files/main.c and browser/main.c, both of which increase
			   their scroll offset for delta > 0). ClassiCube: positive means
			   wheel UP (Window_X11.c maps X11 button 4 to +1). Hence negate.
			   Getting this wrong silently inverts hotbar scrolling and zoom. */
			Mouse_ScrollVWheel(-(float)ev.scroll_delta);
			break;
		case EVENT_RESIZE:
			/* kernel user_window_handle_resize() puts the new CONTENT width in
			   mouse_x and the new CONTENT height in mouse_y. */
			if (ev.mouse_x > 0 && ev.mouse_y > 0) {
				Window_Main.Width  = ev.mouse_x;
				Window_Main.Height = ev.mouse_y;
				Event_RaiseVoid(&WindowEvents.Resized);
			}
			break;
		case EVENT_REDRAW:
			Event_RaiseVoid(&WindowEvents.RedrawNeeded);
			break;
		case EVENT_WINDOW_FOCUS:
			Window_Main.Focused = true;
			Event_RaiseVoid(&WindowEvents.FocusChanged);
			break;
		case EVENT_WINDOW_BLUR:
			Window_Main.Focused = false;
			Event_RaiseVoid(&WindowEvents.FocusChanged);
			break;
		case EVENT_WINDOW_CLOSE:
			/* NOTE: under the userland compositor this is currently unreachable.
			   kernel/gui/window.c posts EVENT_WINDOW_CLOSE with wm_queue_event()
			   (the global WM queue) rather than to the per-window app queue, and
			   nothing drains that queue while the compositor holds exclusive
			   mode; win->on_close is NULL for windows made by sys_win_create(),
			   so the kernel's default action is to HIDE the window. The
			   TitlebarCloseFallback() below is what actually catches it. */
			Window_RequestClose();
			break;
		}
	}

	/* Titlebar X fallback + focus tracking.

	   The kernel hides the window instead of telling us (see the
	   EVENT_WINDOW_CLOSE note above), and it never queues FOCUS/BLUR to a user
	   window either, so both facts have to be read back out of the window
	   manager. sys_wm_get_windows() (kernel/gui/window.c) is read-only and
	   ungated, and it lists hidden windows too, with visible=0 -- which is
	   exactly the state the titlebar X leaves us in.

	   MATCHED BY TITLE, NOT BY HANDLE, deliberately: win_create() returns an
	   index into the kernel's user_windows[] table, while wm_window_info_t.id
	   is window_t::id. They are different numbering spaces and assuming they
	   were the same would have made this silently match the wrong window.

	   Throttled on uptime_ms(), not on sys_clock() ticks: timer_ticks is not a
	   wall clock (KVM replays lost ticks in bursts), so a tick-based deadline
	   can fire many times in an instant. */
	{
		static unsigned long last_check;
		static int miss_streak;
		unsigned long now = uptime_ms();
		if (!win_closing && (now - last_check) >= 500) {
			wm_window_info_t wins[24];
			int n = wm_get_windows(wins, 24);
			int i, found = 0;
			last_check = now;

			for (i = 0; i < n; i++) {
				if (!TitleMatches(wins[i].title)) continue;
				found = 1;
				Window_Main.Focused = wins[i].focused ? true : false;
				if (!wins[i].visible && !wins[i].minimized) Window_RequestClose();
				break;
			}
			/* Two consecutive misses before believing it: a single poll that
			   races a window-list mutation must not close the game. */
			if (n > 0 && !found) {
				if (++miss_streak >= 2) Window_RequestClose();
			} else {
				miss_streak = 0;
			}
		}
	}

	raw_frame_delta = delta;
}

/*########################################################################################################################*
*-------------------------------------------------------Framebuffer-------------------------------------------------------*
*#########################################################################################################################*/
/* PIXEL FORMAT: ClassiCube's default BitmapCol packs B at shift 0, G at 8,
   R at 16, A at 24 (Bitmap.h), i.e. a cc_uint32 of 0xAARRGGBB. MayteraOS window
   content buffers are uint32 ARGB in exactly the same order (see
   kernel/proc/syscall.c sys_win_blit / sys_win_draw_image, which copy the word
   verbatim). So bmp->scan0 is passed through with NO conversion. */
void Window_AllocFramebuffer(struct Bitmap* bmp, int width, int height) {
	bmp->scan0  = (BitmapCol*)Mem_Alloc(width * height, BITMAPCOLOR_SIZE, "window pixels");
	bmp->width  = width;
	bmp->height = height;
}

void Window_DrawFramebuffer(Rect2D r, struct Bitmap* bmp) {
	if (win_handle < 0 || !bmp->scan0) return;

	/* Two present paths, both existing kernel syscalls:

	   SYS_WIN_BLIT scales the whole source to the whole content rect with a
	   nearest-neighbour resample and invalidates the window itself. It reads
	   the user buffer directly (no bounce copy), which makes it the cheap path
	   for a full-frame present, and it is inherently safe when our bitmap size
	   and the content size disagree (e.g. the frame between the user dragging
	   the resize grip and ClassiCube reallocating).

	   SYS_WIN_DRAW_IMAGE is 1:1 with clipping and supports a sub-rect, but it
	   bounce-copies the region through kmalloc/copy_from_user. It is only
	   usable when the rows are contiguous, i.e. a full-width band, and when our
	   bitmap width already matches the content width.

	   Prefer the band path for partial updates (the launcher redraws small
	   strips), and the full blit otherwise. */
	if (r.x == 0 && r.width == bmp->width && r.y >= 0 &&
		r.height > 0 && r.y + r.height <= bmp->height &&
		bmp->width == Window_Main.Width && bmp->height <= Window_Main.Height) {

		win_draw_image(win_handle, 0, r.y, bmp->width, r.height,
			(unsigned int*)(bmp->scan0 + (size_t)r.y * bmp->width));
	} else {
		syscall5(SYS_WIN_BLIT, win_handle, 0, 0,
			(bmp->width & 0xFFFF) | ((bmp->height & 0xFFFF) << 16),
			(long)bmp->scan0);
	}
}

void Window_FreeFramebuffer(struct Bitmap* bmp) {
	Mem_Free(bmp->scan0);
	bmp->scan0 = NULL;
}

/*########################################################################################################################*
*--------------------------------------------------------Clipboard--------------------------------------------------------*
*#########################################################################################################################*/
void Clipboard_GetText(cc_string* value) {
	char buf[512];
	int n = clipboard_get_text(buf, sizeof(buf));
	int i;
	if (n <= 0) return;
	for (i = 0; i < n && buf[i]; i++) String_Append(value, buf[i]);
}

void Clipboard_SetText(const cc_string* value) {
	char str[NATIVE_STR_LEN];
	String_EncodeUtf8(str, value);
	clipboard_set_text(str);
}

/*########################################################################################################################*
*----------------------------------------------------Dialogs / keyboard---------------------------------------------------*
*#########################################################################################################################*/
static void ShowDialogCore(const char* title, const char* msg) {
	/* No blocking system message box exists for a userland app. Surface it on
	   the log instead of pretending a dialog appeared. */
	Platform_LogConst(title);
	Platform_LogConst(msg);
}

cc_result Window_OpenFileDialog(const struct OpenFileDialogArgs* args) {
	(void)args; return ERR_NOT_SUPPORTED;
}

cc_result Window_SaveFileDialog(const struct SaveFileDialogArgs* args) {
	(void)args; return ERR_NOT_SUPPORTED;
}

void OnscreenKeyboard_Open(struct OpenKeyboardArgs* args) { (void)args; }
void OnscreenKeyboard_SetText(const cc_string* text)      { (void)text; }
void OnscreenKeyboard_Close(void)                         { }

/*########################################################################################################################*
*---------------------------------------------------------Gamepads--------------------------------------------------------*
*#########################################################################################################################*/
void Gamepads_PreInit(void) { }
void Gamepads_Init(void)    { }
void Gamepads_Process(float delta) { (void)delta; }

#endif
