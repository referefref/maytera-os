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
//
// #742 RETURNS: 0 if the line is safe (either flushed to the medium, or buffered
// with the log not yet armed, in which case bootlog_arm() will flush it);
// negative if the log IS armed and the flush FAILED, meaning this line exists
// only in RAM. Deliberately NOT MUST_CHECK - see the rationale in bootlog.c.
// Use bootlog_persist_failures() if you want the aggregate rather than the
// per-call answer.
//
// DO NOT re-declare this (or any other function here) with a private `extern`
// in your own .c file. kernel/tools/persist-extern-gate FAILS THE BUILD if you
// do, because a private extern silently opts the whole file out of this
// header's MUST_CHECK attributes AND compiles happily against the wrong
// signature. Two files really did declare this one as non-variadic
// `extern void bootlog_write(const char *s)` and linked without a diagnostic.
int bootlog_write(const char *fmt, ...);

// #742: how many bootlog/heartbeat/usblog/audiolog flushes have FAILED this
// boot, and the last failing return code. Non-zero means the on-disk breadcrumb
// files are not trustworthy as evidence. Note the self-healing property: every
// flush rewrites the WHOLE buffer, so a non-zero count with a later successful
// flush means the content did land; it is the count, not the file, that records
// that the medium misbehaved.
uint32_t bootlog_persist_failures(void);
int      bootlog_last_persist_rc(void);

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

// #748: the heartbeat ring is RAM-resident and reaches the medium only on a
// 30-minute schedule, on a late-beat anomaly (the #373 starvation signature),
// and on panic. Rationale in bootlog.c above HB_FLUSH_MS. These three let a
// caller take part in that.
//
// bootlog_heartbeat_flush() persists the ring NOW and returns 0 if there was
// nothing pending or the write succeeded, negative if the write FAILED. It
// deliberately returns a value rather than being void: bootlog_write() is void-
// like at 410 logging call sites where a status would be ceremony, but a
// DEFERRED flush is different - it is the one write whose failure nobody would
// otherwise notice, so the caller that asked for it gets the answer.
int      bootlog_heartbeat_flush(void);
// Beats recorded vs times the ring was actually written to the device. The
// ratio IS the fix, and it is the honest way to state it (a claim that writes
// went down should be measurable from inside the running system, not only from
// the hypervisor's block counters).
void     bootlog_heartbeat_stats(uint64_t *beats, uint64_t *flushes);
// The live in-RAM ring, for a reader that wants it without touching the disk
// (the kernel shell's `hblog`). Returns its length; *out points at the ring.
uint32_t bootlog_heartbeat_ring(const char **out);

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

// #745 (task #62): OPEN A WINDOW IN WHICH LOGGING MUST NOT TOUCH THE MEDIUM.
//
// Take this around any work that the log's own flush path runs THROUGH. The
// motivating case is usb_msc_transport(): /BOOTLOG.TXT lives on the USB-MSC
// root device, so a bootlog_write() issued from inside a SCSI command re-enters
// usb_msc_transport() and blocks on its own non-recursive command lock, at
// which point that thread reparks every MSC_CMD_BLOCK_MS forever.
//
// Inside the window, bootlog_write() still writes to SERIAL and to the RAM
// buffer; only the device flush is skipped, and the next call from a safe
// context carries the accumulated delta down. Nesting is counted, so
// begin/end pairs may nest. ALWAYS pair them on every return path.
// #134 FAULT-CONTEXT ONLY. Records a line with NO lock, NO allocation and NO
// filesystem access, mirroring to kprintf_nolock(); a later safe context
// flushes it to /BOOTLOG.TXT. Use this and NOT bootlog_write() from an
// exception handler, an ISR, or anywhere holding the console lock:
// bootlog_write() calls kprintf() (which takes g_console_lock, the exact
// deadlock 240dc9f fixed) and its flush enters the storage stack. See the
// block comment in bootlog.c for what it does and does not cover (a KERNEL
// -mode fault halts the CPU, so /boot/PANIC.TXT remains that case's record).
void bootlog_fault_write(const char *fmt, ...);
// Drain the fault ring into /BOOTLOG.TXT. SAFE CONTEXTS ONLY (it calls
// bootlog_write). Called from bootlog_heartbeat(); a caller that has just
// survived a fault may call it to get the record down immediately.
int  bootlog_fault_flush(void);
// Bytes a fault context could not record because the ring was full, and bytes
// any sink dropped because its RAM buffer was full. Both are zero on a healthy
// boot; non-zero means the on-disk breadcrumb files are incomplete.
uint32_t bootlog_fault_lost(void);
uint64_t bootlog_dropped_bytes(void);

void bootlog_defer_begin(void);
void bootlog_defer_end(void);

// #71: bracket a multi-line audiolog dump (e.g. hda_audiolog_report()) so the
// whole report is flushed to /AUDIOLOG.TXT in ONE write instead of a full-file
// rewrite per line (which is O(n^2) over the slow USB-MSC stack).
void audiolog_begin_batch(void);
void audiolog_end_batch(void);

#endif // BOOTLOG_H
