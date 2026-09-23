# Input and Screenshot Syscall Reference

Reference for every input-observation, input-injection, cursor, and screenshot
syscall MayteraOS exposes to Ring 3. It is grounded in the shipping code, not in
intent: each entry below was read out of `kernel/proc/syscall.c`,
`kernel/gui/fb_syscall.c`, `kernel/proc/caps.c`, and the libc wrappers in
`userland/libc/syscall.h`.

## Authoritative syscall numbers

`kernel/proc/syscall.h` is the authoritative table of syscall numbers. It is
locked by the syscall-number-lint, and `userland/libc/syscall.h` mirrors it (the
libc header carries the note "Numbers mirror kernel/proc/syscall.h (locked by
syscall-number-lint)"). A number that appears more than once in the libc header
is the same number defined near each of its wrappers, not a conflict; the kernel
header is the single source of truth. Every number in this document was
cross-checked between the two headers and matched.

## Three gating tiers

These syscalls fall into exactly three trust tiers. Which tier a syscall is in
is the single most important fact about it.

1. **Unprivileged.** Any Ring 3 process may call it. These reveal nothing about
   another process's input, or they change only the caller-visible or global
   settings state.
2. **Compositor-principal-gated.** The handler opens with `is_compositor()`
   (`kernel/gui/fb_syscall.c:100`) or `capgate_caller_is_compositor()`
   (`kernel/proc/syscall.c`, backed by `rustkern/capgate.rs`), and returns `-1`
   (or `-13` EACCES) to any caller that is not the process owning the
   framebuffer. This is a principal check ("are you the compositor"), not a
   capability grant. Ordinary apps never call these; the compositor relays
   hardware input into the kernel window manager through them.
3. **Capability-token-gated (SYSTEM CAPABILITY API, Stage 1 to Stage 3).** The
   handler is only reached after the capability chokepoint has confirmed the
   caller holds a live grant of the required `CAP_*` class with a matching
   scope. Without the grant the request fails with a negative `CAP_E*` code. The
   grant is obtained at runtime through the consent flow (`SYS_CAP_REQUEST` plus
   the compositor-driven `SYS_CAP_VIEW` / `SYS_CAP_RESOLVE`), not from a static
   manifest. See `docs/SYSTEM_CAPABILITY_API.md` and
   `docs/CONTRACTS_INDEX.md`.

## Tier 1: unprivileged input, cursor, and settings syscalls

| Name | # | libc wrapper | Args | Return | Behavior |
|------|---|--------------|------|--------|----------|
| `SYS_KEY_MODS` | 400 | `unsigned int key_modifiers(void)` | none | live modifier bitmask | Returns the current physical modifier bitmask via `keyboard_get_modifiers()`. Deliberately unprivileged: whether Shift/Ctrl/Alt is down is information every keystroke the process already receives carries anyway, and it reveals nothing about other processes (`kernel/proc/syscall.c` `case SYS_KEY_MODS`). Exists so an app can resync modifier state across a focus change it is never told about (#221). |
| `SYS_GET_GLOBAL_MOUSE` | 264 | `int get_global_mouse(int *x, int *y, unsigned int *buttons)` | out `x`, `y`, `buttons` | 0/-1 | Read-only global cursor position and button mask for any process (#185). Ungated by design: `sys_get_global_mouse()` (`kernel/gui/fb_syscall.c:850`) has no `is_compositor()` check. It applies the caller's UI-scale conversion so the coordinates match the caller's own coordinate space. |
| `SYS_GET_MOUSE_SCROLL` | 263 | `int get_mouse_scroll(void)` | none | signed wheel delta | OS-wide mouse wheel: returns and clears the kernel scroll delta via `mouse_get_scroll()`. Read-and-clear, so it is a shared global counter; the caller consumes whatever wheel motion has accumulated. |
| `SYS_SET_MOUSE_SPEED` | 140 | `int set_mouse_speed(int speed)` | `speed` 1-10 | 0 | Sets pointer sensitivity via `mouse_set_sensitivity()` (`kernel/proc/syscall.c:12266`). A settings-tier syscall; ungated. |
| `SYS_GET_MOUSE_SPEED` | 141 | `int get_mouse_speed(void)` | none | 1-10 | Current pointer sensitivity. |
| `SYS_SET_CURSOR` | 248 | `int set_cursor(int style, int size)` | `style`, `size` | 0/-1 | Sets the live mouse-cursor style and size (#116). |
| `SYS_GET_CURSOR` | 249 | `int get_cursor(void)` | none | current cursor | Reads the live cursor style/size. |
| `SYS_SET_CURSOR_THEME` | 148 | `int set_cursor_theme(int theme)` | `theme` 0=Retro 1=Light 2=Dark | 0/-1 | Selects the cursor theme. |
| `SYS_GET_CURSOR_THEME` | 149 | `int get_cursor_theme(void)` | none | 0/1/2 | Current cursor theme. |
| `SYS_PRINT_SCREEN` | 297 | (no libc inline; raw `SYS_PRINT_SCREEN`) | `const char *printer` | 0/-1 | Prints the current framebuffer to the named printer through the print subsystem (`sys_print_screen()`, `kernel/proc/syscall.c:6532`, #318). This is "print the screen to paper", not "capture the screen to a file"; for capture use `SYS_SCREENSHOT_REQUEST`. |

## Tier 2: compositor-principal-gated input relay

These all reject a non-compositor caller. Ordinary apps must never call them;
they are the compositor's private channel for relaying hardware input into the
kernel window manager (or, historically, for the kernel-side desktop fallback).

| Name | # | libc wrapper | Args | Return | Gate + behavior |
|------|---|--------------|------|--------|-----------------|
| `SYS_GET_MOUSE` | 210 | `int get_mouse_pos(int *x, int *y, int *buttons)` / `get_mouse_evt()` | out `x`, `y`, `buttons` | 0/-1 | `is_compositor()` (`fb_syscall.c:773`, returns `-1` otherwise). Reads the sampled cursor; bumps `g_mouse_poll_count` (#334). |
| `SYS_SET_MOUSE` | 211 | `int set_mouse_pos(int x, int y)` | `x`, `y` | 0/-1 | `is_compositor()`. Warps the cursor. |
| `SYS_GET_KEY` | 212 | `int get_key_evt(key_evt_t *k)` | out `event` | 0/-1 | `is_compositor()` (`fb_syscall.c:906`, returns `-1`). Dequeues one key event. |
| `SYS_GRAB_INPUT` | 213 | `int grab_input(int grab)` | `grab` 0/1 | 0/-1 | `is_compositor()` (`fb_syscall.c:1036`). Enters/exits WM exclusive mode. |
| `SYS_INJECT_MOUSE` | 214 | `int inject_mouse(int x, int y, int type, int button)` | `x`, `y`, `type` (0=move,1=down,2=up), `button` mask | 1 if a DOWN landed on a window, else 0, or -1 | `is_compositor()` (`fb_syscall.c:1064`). Relays a mouse event into the kernel WM handlers (drag, title-bar buttons, resize grips, click-to-focus). |
| `SYS_SET_MOUSE_BUTTONS` | 305 | `int set_mouse_buttons(unsigned int mask)` | `mask` | 0/-1 | `is_compositor()` (`fb_syscall.c:898`). Compositor-only button-state relay. |
| `SYS_GET_KEYBOARD` | 195 | `int get_keyboard_char(void)` | none | char, or -1 | `capgate_caller_is_compositor()` (`kernel/proc/syscall.c`, returns `-13` EACCES otherwise). Reads a raw key from the hardware queue. Returns `-1` while a Win16 app owns the screen (its own pump is the sole consumer). |
| `SYS_INJECT_KEY` | 197 | `int inject_key(int key)` | `key` keycode | 0/-1 | `capgate_caller_is_compositor()` (returns `-1` otherwise). Forwards a raw keycode from the hardware queue to the focused KWM window via `wm_dispatch_event()`. |

### Why 195 and 197 use `capgate_caller_is_compositor()` and not `is_compositor()`

`SYS_INJECT_KEY` and `SYS_GET_KEYBOARD` deliberately use the capgate compositor
principal check rather than `fb_syscall.c`'s claiming `is_compositor()`. The
distinction is recorded in `docs/SYSTEM_CAPABILITY_API.md` (Stage 0 Defect 2): a
claiming check "are you a compositor" would let a process become the
principal by claiming it, which for an injection primitive is itself an
escalation. `capgate_caller_is_compositor()` (in `rustkern/capgate.rs`) checks
the established compositor principal without a claim path.

### Stage 0 history: the ungated `SYS_INJECT_KEY`

`SYS_INJECT_KEY` (197) was UNGATED until Stage 0 of the capability work
(landed on `dev` at `f2230772`). Ungated, any Ring 3 process could post a
synthetic `EVENT_KEY_DOWN` to the focused window, which (a) manufactured
elevation input credit (`sys_elev_request()` refuses unless the WM recently
delivered a real input event to a window the requester owns, so a self-injected
key let an app raise a password prompt the user never asked for), and (b) drove
any other app's UI, since the event landed on whatever window had focus. Its
matched pair `SYS_INJECT_MOUSE` (214) had been `is_compositor()`-gated since it
was written, while 197 was not, even though the header described both as
"compositor only". The asymmetry (one half of a matched pair gated, the other
not) is the shape to grep for; it is recorded in `blame.md`.

## Tier 3: capability-token-gated capture and injection (SYSTEM CAPABILITY API)

An app does not learn a new API to USE a capability. It makes the request it
always made and, once a grant is held, that request starts succeeding. The new
surface an ordinary app touches is only `SYS_CAP_QUERY` / `SYS_CAP_REQUEST` /
`SYS_CAP_STATUS`; `SYS_CAP_VIEW` / `SYS_CAP_RESOLVE` are compositor-only (the
consent prompt). All handlers live in `kernel/proc/caps.c`.

### Screen capture (`CAP_SCREEN_CAPTURE`)

| Name | # | libc wrapper | Args | Return | Behavior |
|------|---|--------------|------|--------|----------|
| `SYS_SCREENSHOT_REQUEST` | 435 | `long sys_screenshot_request(const char *path)` | `path` | 0, or negative `CAP_E*`/-1 | Gated by the `screen.capture` capability chokepoint before the handler runs; the handler then does the scope match, `perms_check`, and enqueue. Wired end to end. |
| `SYS_SCREENSHOT_POLL` | 436 | `long sys_screenshot_poll(char *out, int cap)` | out buffer, `cap` size | status | Polls for the completed capture written by the request above. |

### Input injection (`CAP_INPUT_INJECT`, window-scoped)

| Name | # | libc wrapper | Args | Return | Behavior |
|------|---|--------------|------|--------|----------|
| `SYS_CAP_INJECT_KEY` | 439 | `long sys_cap_inject_key(int win, int keycode)` | `win`, `keycode` | 0, or negative `CAP_E*`/-1 | Gated by the `input.inject` chokepoint, then `caps_inject_authorize()` (`caps.c`) enforces window ownership scope, the consent-surface / lock-screen guard (`CAP_EBUSY` while a prompt or the lock screen is up), and one-use consume. The event is delivered ONLY to `win`, which the caller must own under a `CAP_SCOPE_WINDOW` ("self") grant (Stage 3), or hold a user-consented `CAP_SCOPE_WINDOW_TARGET` grant for that window (Stage 4 cross-app). |
| `SYS_CAP_INJECT_MOUSE` | 440 | `long sys_cap_inject_mouse(int win, int x, int y, int type, unsigned int button)` | `win`, `x`, `y`, `type`, `button` | 0, or negative `CAP_E*`/-1 | Same gating and scope model as `SYS_CAP_INJECT_KEY`. |

The critical property of both: the injected event is marked
`INPUT_SRC_SYNTHETIC`, so it never stamps the elevation input credit for any
target. This is precisely the escalation the ungated `SYS_INJECT_KEY` (197)
allowed and that Tier 3 closes: a capability-gated inject cannot forge user
intent.

### Capability request/consent surface (used to obtain the grants above)

| Name | # | libc wrapper | Caller | Notes |
|------|---|--------------|--------|-------|
| `SYS_CAP_QUERY` | 429 | `sys_cap_query(cap, cap_state_t *out)` | app | Do I hold this capability, and with what scope/expiry? |
| `SYS_CAP_REQUEST` | 430 | `sys_cap_request(const cap_req_t *r)` | app | Open a consent request for a `CAP_*` class with a reason and scope. |
| `SYS_CAP_STATUS` | 431 | `sys_cap_status(seq)` | app | Poll an open request by sequence. |
| `SYS_CAP_VIEW` | 432 | `sys_cap_view(cap_view_t *out)` | compositor only | Read the next pending consent request to render the prompt. |
| `SYS_CAP_RESOLVE` | 433 | `sys_cap_resolve(seq, action)` | compositor only | Approve or deny a request. |
| `SYS_CAP_REVOKE` | 434 | `sys_cap_revoke(cap)` | app | Drop a held grant. |

Capability classes (`CAP_INPUT_INJECT=1`, `CAP_INPUT_OBSERVE=2`,
`CAP_SCREEN_CAPTURE=3`, `CAP_SCREEN_STREAM=4`, `CAP_AUDIO_OUTPUT=5`,
`CAP_SERIAL_PORT=6`, `CAP_NET_CONNECT=7`, `CAP_NET_LISTEN=8`) and scope kinds
(`CAP_SCOPE_PATH`, `CAP_SCOPE_PORT`, `CAP_SCOPE_WINDOW`,
`CAP_SCOPE_WINDOW_TARGET`) are defined in `userland/libc/syscall.h` and mirror
`kernel/proc/caps.h` / `rustkern/caps.rs`.

## Fullscreen window control

`SYS_WM_FULLSCREEN_*` control an app's own fullscreen presentation. They are not
input syscalls but are grouped here because games and full-screen apps use them
alongside `SYS_GRAB_INPUT`.

| Name | # | libc wrapper | Return | Behavior |
|------|---|--------------|--------|----------|
| `SYS_WM_FULLSCREEN_ENTER` | 389 | `sys_wm_fullscreen_enter()` | 0/-1 | Enter fullscreen for the caller's OWN focused window only. |
| `SYS_WM_FULLSCREEN_EXIT` | 390 | `sys_wm_fullscreen_exit()` | 0/-1 | Exit fullscreen; unconditional and safe to call from anywhere. |
| `SYS_WM_FULLSCREEN_RENDER` | 391 | `sys_wm_fullscreen_render()` | 0/-1 | Compositor per-frame fast-path blit of the fullscreen surface. |
| `SYS_WM_FULLSCREEN_STATUS` | 392 | `sys_wm_fullscreen_status()` | `(id<<32)|commit_seq`, or -1 | Watchdog probe of fullscreen state. |

## Testing caveat: programmatic mouse-click injection in headless VMs (#334)

Mouse-click automation does not reliably land in headless VM testing. This is a
hypervisor-level limitation, not a kernel or compositor bug: QEMU pointer
injection over the monitor/serial path does not deliver clicks that the
compositor samples as real button transitions. The compositor does sample the
injected cursor position (`g_mouse_poll_count`, #334), but a synthetic click
frequently does not register. Drive GUI tests through the keyboard and the
serial console instead of simulated mouse clicks. See
`docs/GUI_TESTING.md` and `blame.md` (`gui-testing-mouse-unreliable`).
