# sprite - changelog

## 2026-09-19 - initial release (Sprite Studio)
- New media app: a pixel-art and sprite-animation editor. Fills a real gap:
  Maytera Studio (apps/paint) is a free-size raster editor with brushes and
  filters; nothing in the tree offered a magnified fixed grid, hard
  single-pixel tools, frames, onion skin or a sprite-sheet export
  (confirmed by grep before building: no source mentioned a sprite sheet,
  onion skin or frame strip).
- Canvas 8x8 to 128x128 (presets 8/16/32/64 plus W/H +-8 steps), up to 32
  frames, alpha-0 transparency over a checkerboard. Tools: pencil, eraser,
  flood fill, line, rectangle, filled box, eyedropper; right mouse button
  always erases; X-mirror mode; grid overlay; sixteen-deep undo ring.
- Four built-in 16-colour palettes (PICO-8, DB16, Sweetie 16, Endesga 16)
  and free colour by hex entry through the shared textfield.h widget (so
  the hex box has a caret, selection and Ctrl+C/V like every other field).
- Frame strip with add / duplicate / delete and thumbnails; onion skin of
  the previous frame; playback at 4 / 8 / 12 / 24 fps into a 4x preview.
  Playback advances from the event loop's timeout tick (uptime_ms
  deadline); idle the app parks in win_get_event(). No busy-wait.
- Save writes an RGBA PNG SPRITE SHEET (frames left to right) under
  <home>/SPRITES with a local-time stamped name (tz_local_stamp +
  userhome_path, the same shape Snapshot uses), or back to the opened PNG.
  Export writes the current frame as a 24-bit BMP with transparent pixels
  flattened to the magenta key colour FF00FF. Opening a PNG/BMP (argv[1])
  through SYS_DECODE_IMAGE splits a sheet whose width is a whole multiple
  of its height back into frames; magenta in a BMP is read back as clear.
- Encoders come from apps/imgconv/imgenc.c (BMP24 + stored-deflate PNG),
  compiled by path so there is one implementation. libc is the right home
  for that file; it was outside this batch's permitted scope.
- Keyboard: 1-7 tools, M mirror, G grid, O onion, Z/Ctrl+Z undo, S save,
  E export, Space play/stop, [ ] or arrows change frame, N new frame,
  D duplicate, + / - / 0 zoom, # focus the hex box, Esc quit.
- Styled per docs/UI_STYLE_GUIDE.md: theme palette via gui_set_palette,
  gui_button / gui_textfield2 primitives, TTF text, resizable with reflow.
- MEASURED: links to a static PIE ELF with gcc-12 against libc.a.
  NOT YET MEASURED: a boot-time run on a VM; behaviour is inferred from the
  same syscall and widget usage as Snapshot / Image Viewer.
