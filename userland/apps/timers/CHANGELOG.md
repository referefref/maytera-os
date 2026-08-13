# timers - changelog

## 2026-07-12 - initial release
- New productivity app: Timers. Fills a real gap: the existing clock app only
  displays the time of day; nothing in the OS could actually time anything.
- Three tabs in one window:
  - Stopwatch: start/pause/resume, laps with split and total columns
    (newest first), tenth-of-a-second display.
  - Countdown timer: six presets (1/3/5/10/25/60 min), plus/minus adjust
    buttons that also work while running, progress bar, flashing
    "Time's up" state, and a notification toast via notify_post() on expiry.
  - Pomodoro: 25 min focus / 5 min short break / 15 min long break every
    4th session, cycle dots, session counter, skip button, phase-change
    notifications.
- All timing uses the monotonic uptime_ms() syscall (immune to RTC changes).
  No new kernel work needed.
- Styled per docs/UI_STYLE_GUIDE.md: theme-following palette (Dark, Light,
  Classic, Ocean, Nord), shared style-engine widgets (gui_button, gui_card,
  gui_progress, gui_fill_circle_aa), antialiased TTF text, live resize reflow,
  full keyboard support (1/2/3 or Left/Right tabs, Space start/pause, L lap,
  N skip, R reset).
- Event loop is fully blocking on win_get_event with a 100 ms timeout while
  any timer runs and 250 ms when idle; no busy-wait or spin loop.
