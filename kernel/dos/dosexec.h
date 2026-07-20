// dosexec.h - MS-DOS real-mode .EXE/.COM loader + runner (#201)
// Runs a 16-bit MS-DOS program on the existing x86_16 real-mode interpreter,
// providing INT 21h (DOS API), INT 10h (VGA BIOS / mode 13h), INT 33h (mouse)
// and INT 16h (keyboard). Mode 13h (320x200x256) is captured from the 0xA0000
// linear framebuffer and blitted (scaled 2x, through the VGA DAC palette) into a
// MayteraOS host window so the compositor composites it like any app.
#ifndef DOSEXEC_H
#define DOSEXEC_H

#include "../types.h"

// Launch a DOS program in its own kernel proc + host window (non-blocking).
// path is a FAT path (e.g. "/DOS/TIM/TIM.EXE"). Returns 0 on spawn, <0 on error.
int dos_launch(const char *path);

// Blocking run used by the proc entry (and RC `dos` command). Loads the file,
// sets up PSP + registers, and runs to completion. Returns program exit code,
// or <0 on load failure.
int dos_run_file(const char *path);

#endif // DOSEXEC_H
