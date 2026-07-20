# Wallpapers

The MayteraOS kernel looks for a handful of BMP files at the root of the
boot FAT32 partition. They are not shipped in this repository (together
they are over 100 MiB of binary data that does not belong in git).

To include them in your disk image, point `stage-disk.sh` at a directory
that holds them:

```
sudo WALLPAPERS_DIR=/path/to/bmps ./stage-disk.sh
```

All `*.bmp` / `*.BMP` files in that directory are copied to the FAT32
root with upper-case filenames.

## Files the kernel references

### Boot / default

- `BOOT.BMP` - full-screen boot splash (rendered by `kernel/video/graphics.c`)
- `BACK.BMP` - default desktop wallpaper (the "Default Blue" entry in the
  wallpaper picker)

Both of these are optional: when they are missing the kernel falls back
to a procedurally drawn gradient.

### Selectable wallpapers

The wallpaper picker in the settings app walks the table in
`kernel/gui/desktop.c` (look for `g_wallpapers[]`). The current set is:

```
EBERG01.BMP .. EBERG13.BMP
EBERG15.BMP .. EBERG22.BMP
EBERG25.BMP .. EBERG30.BMP
OCEAN01.BMP .. OCEAN08.BMP
OCEAN10.BMP
OCEAN12.BMP .. OCEAN14.BMP
MACRO01.BMP, MACRO02.BMP
MACRO05.BMP .. MACRO08.BMP
MACRO11.BMP .. MACRO17.BMP
MACRO19.BMP, MACRO20.BMP
```

Any filename not present in your `WALLPAPERS_DIR` simply will not be
selectable at runtime; the wallpaper picker tolerates missing files.

## Image format

- 1280x720 recommended (matches the default framebuffer mode)
- 24-bit BGR Windows BMP (`BM` magic, `biBitCount=24`)
- Bottom-up row order (standard BMP)

Tools like GIMP or ImageMagick can export this format directly:

```
convert input.jpg -resize 1280x720 -type TrueColor BMP3:OUTPUT.BMP
```
