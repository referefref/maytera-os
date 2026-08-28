// devlog.h - #388 comprehensive boot-time device inventory
//
// TWO ENTRY POINTS, ONE BUILDER (the shared-primitive rule). devlog_build()
// renders the inventory into this module's static buffer and returns it;
// devlog_dump() is a thin wrapper that builds and then writes /DEVLOG.TXT.
// Anything else that needs the inventory (e.g. painting it on the framebuffer
// when NOTHING mounts, which on a laptop with no serial port is the only
// channel left) calls devlog_build() and renders the same bytes. There is
// deliberately no second builder.
#ifndef DEVLOG_H
#define DEVLOG_H

#include "../types.h"
#include "fat.h"

// Build the complete device inventory into the module's static buffer and
// return a pointer to it. The buffer is NUL-terminated, so it can be treated
// as a C string by a screen renderer; *out_len (optional) receives the length
// in bytes, not counting the terminator.
//
// CONTRACT, relied on by the no-storage screen-render path:
//   - NO filesystem access at all. Safe to call before any mount, and safe on
//     a machine where storage never comes up.
//   - NO heap allocation (the buffer is static .bss) and no blocking, so it is
//     safe with interrupts off and before the scheduler exists.
//   - Every device poll it makes is bounded. The HD Audio section, which is
//     the one part that could not honour that, is OPT-IN and OFF by default;
//     see devlog_set_include_hda().
//   - Idempotent: each call rebuilds from scratch, overwriting the previous
//     contents. The returned pointer is stable across calls.
const char *devlog_build(uint32_t *out_len);

// Build (via devlog_build) and write the result to /DEVLOG.TXT on the FAT
// root. Non-fatal: if fs is NULL or not mounted, the inventory is still BUILT
// (so a caller can render it) and only the write is skipped. Echoes a short
// summary to serial.
void devlog_dump(fat_fs_t *fs);

// HD AUDIO SECTION OPT-IN. Default OFF, and it must stay OFF on any boot whose
// job is to come up. hda_devlog_scan() GCTL-resets every HDA controller on the
// machine and then issues the full widget-graph verb sweep at the DEFAULT
// codec-command spin cap (~200ms per timed-out verb, hundreds of verbs), which
// is what wedged the real iMac14,4 Cirrus CS4208 on b730/b733 and got this
// whole file un-wired from main.c in b734. Enabling it is a deliberate
// diagnostic choice for a machine you are willing to hang. When it is off,
// devlog_build() emits an explicit SKIPPED line saying so, so the absence of
// the section is never mistaken for "no audio hardware".
void devlog_set_include_hda(int enable);
int  devlog_get_include_hda(void);

#endif // DEVLOG_H
