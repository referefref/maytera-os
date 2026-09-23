# imgconv - changelog

## 2026-09-19 - initial release (Image Converter)
- New media utility: batch convert and resize a folder of images. Fills a
  real gap: the kernel decodes BMP/PNG/JPEG for any app (SYS_DECODE_IMAGE)
  and Maytera Studio can export the one document it has open, but nothing
  converted a folder, and the wallpaper picker (libc wallpapers.h) only
  enumerates *.BMP at the filesystem root, so a set of PNG or JPEG photos
  had no path to becoming wallpapers short of one Studio session per file.
  Confirmed absent before building: apps/convert is a UNIT converter.
- Scans a folder for .BMP / .PNG / .JPG / .JPEG (dirent opendir/readdir),
  lists them with a checkbox each in a shared gui_scroll viewport; All /
  None; Up; a single file via argv[1] (Files "Open With") lists just that
  file from its own folder.
- Output BMP (24-bit), PNG (RGB, stored deflate) or JPEG (baseline, quality
  60/75/85/95). Resize None / Fit within / Exact / Scale percent, presets
  for the real screen size (SYS_FB_INFO), 1280x800 and 640x480. Downscale
  is a box average, upscale bilinear, integer arithmetic.
- Output goes to <source>/CONVERTED by default (any folder can be typed;
  created with sys_mkdir), never overwriting the source file (a same-path
  result gets a -2 suffix). Optional "Install BMP results as wallpapers"
  also writes the BMP to / under a short uppercase 8-character name, which
  is exactly where wp_enumerate() scans, so the picker sees it at once. An
  existing root name is left alone and reported as "wp=".
- One file per event-loop tick, live progress bar, per-file "OK WxH size"
  or "FAIL: reason" note measured from the written file, Cancel between
  files; no busy-wait anywhere.
- JPEG output is apps/paint/jpegenc.c compiled BY PATH (a pure function
  over an ARGB buffer): Studio and this app cannot drift on JPEG output,
  and paint is not edited. BMP/PNG writers are imgenc.c here, shared with
  apps/sprite the same way; libc is the right home for both once permitted.
- Keyboard: Enter convert, Esc cancel/quit, A/N select all/none, B/P/J
  format, R rescan, U up, Tab focus the folder field, arrows/PgUp/PgDn
  scroll the list.
- Styled per docs/UI_STYLE_GUIDE.md: theme palette, gui_button /
  gui_checkbox / gui_progress / gui_textfield_tf primitives, TTF text,
  resizable.
- MEASURED: links to a static PIE ELF with gcc-12 against libc.a.
  NOT YET MEASURED: a boot-time run on a VM; behaviour is inferred from the
  same syscall and widget usage as Gallery / Snapshot.
