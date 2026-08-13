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
// Launch from a RING-3 caller (SYS_DOS_RUN). The guest runs as the calling
// process's uid/gid; the launch is refused if there is no Ring-3 caller (#708).
int dos_launch(const char *path);

// Launch from a SERVICE with no Ring-3 caller (the /CONFIG/DOSRUN.CFG boot
// harness). The guest runs as the authenticated desktop session; the launch is
// refused if no session has authenticated yet (#708). Kernel callers must use
// THIS one: dos_launch() would find no caller identity and refuse.
int dos_launch_kernel(const char *path);

// Blocking run used by the proc entry (and RC `dos` command). Loads the file,
// sets up PSP + registers, and runs to completion. Returns program exit code,
// or <0 on load failure.
int dos_run_file(const char *path);

#endif // DOSEXEC_H
