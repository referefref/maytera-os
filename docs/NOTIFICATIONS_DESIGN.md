# MayteraOS Notifications Subsystem - design

> STATUS: SUPERSEDED (2026-08-22, #242) — this design specified kernel syscalls SYS_NOTIFY_* 238-241 that do not exist; notifications shipped userland-only (#168, `libc/notify.c`).


Goal: an OS notification service that lets any app push alerts. Alerts surface as
(1) a tray bell with a popup history list and (2) transient toast cards, are
user-controlled per-app in a Settings "Alerts" tab, and respect fullscreen apps.

## Requirements (from user)

- Apps register/push to a notification feed (a service).
- User controls alerts per-app in a Settings "Alerts" tab.
- Tray shows a Zest BELL icon as the FIRST tray icon (before the widgets icon).
- Click the bell -> popup list of alerts: scrollable, interactive (app deep
  links), per-item dismiss, and "Clear all".
- On receipt, show a TOAST card bottom-right that slides off to the right and
  disappears after a configurable timeout (Alerts tab; default 3s) - UNLESS a
  fullscreen-drawing app is running that does NOT have "enable notifications
  display" set (then suppress the toast; it still lands in the bell list).

## Architecture (grounded in existing code)

### A. Notification store + service (kernel)
- Live in/under `kernel/proc/services.c` (#95 services subsystem) as a built-in
  "notifications" service, OR a small `kernel/proc/notify.c` it owns.
- Ring buffer of N (e.g. 64) notifications. Each record:
  `{ id (u32, monotonic), app_id (str, from caller), title, body, icon (str ->
  /ICONS name), action (deep-link string, see below), timestamp, flags
  (read/dismissed), level (info|warn|error|success) }`.
- Per-app enable flag (default on) + global Do-Not-Disturb, persisted in
  `/CONFIG/ALERTS.CFG` (8.3). The kernel store holds the live state; Settings
  edits the file + a syscall refreshes.

### B. Syscalls (new, 238+)
- `SYS_NOTIFY_POST (238)` (title, body, icon, action, level) -> id. Records the
  caller's app_id (from the process). Drops/queues per the per-app enable flag.
- `SYS_NOTIFY_LIST (239)` (buf, max) -> count; fills records for the bell popup.
- `SYS_NOTIFY_ACTION (240)` (id, verb) where verb = dismiss | mark_read |
  clear_all | get_pending_toast. `get_pending_toast` returns the next
  not-yet-toasted record (the compositor polls this each frame, like it already
  polls net/events).
- `SYS_NOTIFY_CONFIG (241)` (op) refresh per-app/DND/duration from ALERTS.CFG.

### C. libc API (apps)
- `notify(const char *title, const char *body, const char *icon, const char
  *action, int level)` -> id (wraps SYS_NOTIFY_POST). Plus `notify_simple(title,
  body)`. Document in maytera.h.
- Deep link / action string format: `"app:/APPS/IRC?chan=#mayteraos"` or
  `"focus:<window-title>"` - the compositor parses it on click: launch the app
  (sys_spawn) or focus an existing window, optionally passing the query as argv.

### D. Compositor (userland) - tray bell, popup, toasts
- TRAY: add a BELL slot as tray icon #0 (before widgets). Bump `TRAY_N` 3->4,
  shift positions so bell is first. Tint per theme (readable_ink, like the other
  tray icons). Badge: small accent dot/count when unread > 0.
- BELL POPUP (on bell click): a panel (styled with the engine: rounded card,
  TTF) listing notifications newest-first: each row = icon + title + body
  (1-2 lines) + relative time + an x (dismiss). Scrollable (reuse the dropdown
  scroll pattern). Footer: "Clear all". Clicking a row runs its action
  (deep link) and marks read. Empty state: "No notifications".
- TOASTS: poll `get_pending_toast` each frame. For a new one, decide:
  - SUPPRESS if a fullscreen app is focused/drawing AND that app lacks "enable
    notifications display" (see permissions). Otherwise SHOW.
  - Show a toast CARD bottom-right (engine card: icon + title + body + accent by
    level), slide IN from the right, hold `toast_secs` (default 3, from config),
    then slide OUT to the right and vanish. Stack multiple vertically. Clicking a
    toast runs its action; it also lands in the bell list regardless.
- The compositor owns animation/timing (it redraws each frame), consistent with
  widgets/shadows.

### E. Settings - "Alerts" tab
- New panel: master toggle (notifications on/off), Do-Not-Disturb toggle, toast
  duration (slider/dropdown, default 3s), and a per-app list (enumerated from
  apps that have posted / declared notifications) each with an enable toggle.
- Writes `/CONFIG/ALERTS.CFG`; calls SYS_NOTIFY_CONFIG to apply live.

### F. Permissions: fullscreen + "enable notifications display"
- A fullscreen-drawing app (e.g. DOOM, a Win16 game) can declare
  `notifications_display: true|false` in its per-app options
  (the `window-options.yaml` from task #165, or app manifest). Default false for
  fullscreen apps (do not interrupt games), true otherwise.
- Toast suppression check: if the focused/top app is fullscreen AND its
  `notifications_display` is false -> suppress toast (still store it). Else toast.

## Assets
- Generate `BELL.ICN` from a Zest bell SVG (MICO recipe), version-control in
  assets/icons + deploy to /ICONS. (Also a "bell-off"/DND variant optional.)

## Phases
1. Kernel store + service + syscalls (POST/LIST/ACTION/CONFIG) + ALERTS.CFG.
2. libc `notify()` API + maytera.h docs; wire one demo poster (e.g. IRC mention,
   or a `notify-send` RC/CLI) to prove the pipe.
3. Compositor: BELL tray icon (#0) + popup list (scroll/dismiss/clear all/deep
   link).
4. Compositor: toast cards (slide in/hold/slide out) + fullscreen suppression.
5. Settings "Alerts" tab (per-app + DND + duration) -> ALERTS.CFG live apply.
6. Deep-link action handling (launch/focus from popup + toast).

## Notes / ties
- Reuses #95 services, the style engine (cards/TTF/rounded), the tray, and the
  per-app options file from #165. Deep links reuse sys_spawn/window-focus.
- Keep the store in the kernel so notifications survive the posting app exiting
  and any app can post without the compositor being the broker.
