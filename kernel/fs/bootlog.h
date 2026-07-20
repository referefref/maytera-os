// bootlog.h - Persistent, crash/hang-safe on-disk boot log (#307 real-hardware
// debugging aid). See bootlog.c for the durability design.
#ifndef BOOTLOG_H
#define BOOTLOG_H

#include "../types.h"
#include "fat.h"

// Append a diagnostic line. Safe to call from the very first line of main()
// (before any filesystem exists) all the way through userland/login.
//
// - Always mirrors the line to the existing serial kprintf() log (this is
//   ADDITIONAL logging, never a replacement for serial).
// - Always appends to an in-RAM buffer, so nothing is lost even if storage
//   never becomes writable.
// - Once bootlog_arm() has been called, ALSO flush-writes the whole
//   accumulated buffer to /BOOTLOG.TXT on every call, so a hard hang or
//   power-cut immediately after any line still leaves that line (and
//   everything before it) durably on disk.
//
// Keep call sites to significant checkpoints/events, not per-instruction or
// per-packet tracing - this is a lightweight text log, not a full trace.
void bootlog_write(const char *fmt, ...);

// Call once, as soon as the FAT root filesystem is mounted and writable
// (works for both the #307 USB-MSC root path and the classic ATA root path).
// Immediately flushes everything logged so far - including xHCI/USB
// enumeration, which necessarily happened before any filesystem existed -
// and arms live flush-on-every-call bootlog_write() from then on.
void bootlog_arm(fat_fs_t *fs);

// True once bootlog_arm() has succeeded (used by callers that want to avoid
// pointless work, e.g. the userland syscall wrapper's early-boot no-op case).
int bootlog_is_armed(void);

// #373 real-HW freeze diagnostic: a LIGHTWEIGHT, constant-cost heartbeat writer.
// Unlike bootlog_write() (which rewrites the whole ever-growing /BOOTLOG.TXT on
// every call - an O(n^2) series of full-file rewrites that, at 1 Hz over the
// slow USB-MSC stack, grew to 4-27s per write and wedged the iMac ~62s in), this
// keeps only a small SIZE-BOUNDED ring of the most recent beats and rewrites a
// SEPARATE fixed file (/HEARTBEAT.TXT). Every call therefore costs a constant,
// tiny write that cannot grow without bound. /HEARTBEAT.TXT advancing on the
// next boot proves the OS is alive; where it stops is the last uptime reached.
void bootlog_heartbeat(const char *line);

// #433 (re-scoped) USB descriptor / HID-enumeration diagnostic. Appends a line
// to /USBLOG.TXT (own RAM buffer, flushed by the same bootlog_arm()). Use for
// per-device descriptor dumps and the runtime HID enumeration decisions
// (SET_PROTOCOL/SET_IDLE sent + result, Configure-Endpoint result, interface
// binding) so the real keyboard's descriptors are readable over SSH via
// `cat /USBLOG.TXT`. Mirrors to serial like bootlog_write().
void usblog_write(const char *fmt, ...);

// #71 / Cirrus CS4208 real-HW audio diagnostic. Appends a line to /AUDIOLOG.TXT
// (own RAM buffer, flushed by the same bootlog_arm()). Use for the HD Audio
// codec identity, the output-relevant widget graph (DAC/pin config-default/amp/
// EAPD), the codec GPIO mask/dir/data, and the output-stream descriptor state
// (format, BDL, SDnCTL RUN bit, SDnSTS, link position), so `cat /AUDIOLOG.TXT`
// over SSH on the iMac shows exactly what the codec is, whether the speaker amp
// is enabled, and whether the output DMA runs. Mirrors to serial like
// usblog_write(). Same durability design as /USBLOG.TXT.
void audiolog_write(const char *fmt, ...);

// #71: bracket a multi-line audiolog dump (e.g. hda_audiolog_report()) so the
// whole report is flushed to /AUDIOLOG.TXT in ONE write instead of a full-file
// rewrite per line (which is O(n^2) over the slow USB-MSC stack).
void audiolog_begin_batch(void);
void audiolog_end_batch(void);

#endif // BOOTLOG_H
