// panic.h - #418 on-fault persistent panic log + late-boot stage breadcrumbs.
//
// Real-hardware diagnostic (iMac14,4 instant hard-crash at desktop-ready, no
// serial available on the physical machine). See the #418 ANALYSIS.md for the
// full investigation. Two independent but related facilities live here:
//
//   1. Stage breadcrumbs (/STAGE.TXT) - a small ring of the last few late-boot
//      / service-launch checkpoints reached, updated via a fixed-size,
//      constant-cost single-sector rewrite (same idea as bootlog_heartbeat(),
//      just tracking discrete named stages instead of a periodic tick).
//   2. On-fault panic record (/PANIC.TXT) - written directly from
//      cpu/idt.c's exception handler, before calling into
//      crashhandler_report()/crashhandler_show_dialog() (which may themselves
//      fault again - see finding #1 in ANALYSIS.md). Captures RIP/CR2/error
//      code/CR3/last-known-stage/kernel version.
//
// Both files are PRE-ALLOCATED ONCE at boot (panic_log_init(), called right
// after bootlog_arm()) via the normal locked fat_write_file() path, then
// updated ONLY via a raw, unlocked, single-sector overwrite
// (fat_write_sector()) for the rest of the boot/run. This deliberately avoids
// fat_write_file_inner()'s delete-existing -> create-new -> write -> patch-
// directory-entry-size sequence: a reset at ANY point in that four-step
// sequence reads back as a 0-byte file regardless of how much earlier content
// had accumulated (see ANALYSIS.md finding #1b - this is believed to be
// exactly why /BOOTLOG.TXT read back empty after a boot that visibly reached
// "Desktop ready"). A single 512-byte sector write is as atomic as the
// storage hardware provides and can never regress to that failure mode: the
// file size never changes after pre-allocation, so there is no size-patch
// step to interrupt, and content that arrives mid-write is delimited to a
// single sector already reserved for exactly this data.
//
// The on-fault write path additionally takes NO lock (fat_lock() may already
// be held by whatever the faulting context was doing), allocates no heap
// memory, and never touches the framebuffer/GUI - it is called with
// interrupts disabled, in exception context, and must not be able to
// recursively trigger the very bug class this task exists to diagnose.
#ifndef PANIC_H
#define PANIC_H

#include "../types.h"
#include "fat.h"

// Late-boot / service-launch stage checkpoints. Deliberately starts AFTER the
// FAT root is mounted (both this facility and /BOOTLOG.TXT can only persist
// to disk once a filesystem exists; the pre-mount portion of boot is already
// covered by /BOOTLOG.TXT's in-RAM-buffered-then-flushed design and
// /DEVLOG.TXT's PCI/USB/HDA snapshot). This is the "late boot + service
// launch" portion #418 specifically asked to be covered, and matches the
// exact moment (compositor + service spawn) the iMac crash was observed at.
typedef enum {
    STAGE_NONE = 0,
    STAGE_FS_MOUNTED,
    STAGE_DEVLOG_WRITTEN,
    STAGE_NIC_INIT_DONE,
    STAGE_LOGIN_START,
    STAGE_LOGIN_DONE,
    STAGE_SVC_REGISTRY_BUILT,
    STAGE_COMPOSITOR_LAUNCH,
    STAGE_COMPOSITOR_UP,
    STAGE_SVC_SPAWN,
    STAGE_DESKTOP_READY,
    STAGE_COUNT
} boot_stage_t;

// Call once, right after bootlog_arm(fs). Pre-allocates BOTH /STAGE.TXT and
// /PANIC.TXT at a fixed size (one sector each) using the normal locked
// fat_write_file() path (safe here - this only ever runs once, early, never
// from fault context) and resolves+caches each file's first-cluster sector
// number so later updates can go straight to fat_write_sector(). Also flushes
// any stage_set() calls that happened before the FAT root existed (mirrors
// bootlog_arm()'s initial flush of pre-mount content).
void panic_log_init(fat_fs_t *fs);

// Record a stage transition, with an optional short detail string (e.g. a
// service name: stage_set(STAGE_SVC_SPAWN, "haservice")). Safe to call before
// panic_log_init() runs (buffered in RAM only until then, like
// bootlog_write()); after init, each call is one constant-cost, single-sector
// disk write. `detail` may be NULL.
void stage_set(boot_stage_t stage, const char *detail);

// Called from cpu/idt.c's exception handler with interrupts already
// disabled, for BOTH the kernel-mode-halt branch and the user-mode-fault
// branch - in the user-mode case, BEFORE crashhandler_report()/
// crashhandler_show_dialog() run (those may fault again; this must land on
// disk first). `user_mode` selects the label written ("KERNEL" vs "USER").
// Must not take any lock, allocate heap memory, or touch the
// framebuffer/GUI - see file header.
void panic_log_write(uint64_t rip, uint64_t cr2, uint64_t error_code,
                      uint64_t cr3, const char *exception_name, int user_mode);

// ---------------------------------------------------------------------------
// #480 Canonical kernel panic primitive.
//
// Before #480 the kernel had NO single fatal-handling symbol: fs/panic.c only
// persisted /PANIC.TXT (no print, no halt), cpu/idt.c hand-rolled a log+halt
// inline, desktop.c did bare `cli; hlt`, and the Rust landing pad
// (rust_kernel_panic) was a bespoke shim because there was nothing canonical to
// call. kpanic() is that one canonical primitive; every fatal path routes here.
//
// kpanic():
//   1. disables interrupts,
//   2. kprintf()s a loud "[PANIC] " banner with the formatted message (reuses
//      the shared kprintf/vsnprintf/va_list - NO hand-rolled formatting),
//   3. captures the caller via __builtin_return_address(0) for context,
//   4. persists a record via panic_log_write() above (which no-ops safely if
//      the FAT panic slot was never armed),
//   5. enters the shared terminal halt (kpanic_halt): a real cli+hlt idle loop
//      (#426: a terminal halt, never a busy-spin), after releasing the BKL so a
//      dead CPU never deadlocks the rest of an SMP system.
// noreturn so callers (and the Rust `-> !` panic handler via rust_kernel_panic)
// can rely on it never returning.
void kpanic(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

// The shared terminal halt tail: release the BKL, then permanently cli+hlt.
// Exposed so a fatal path that has ALREADY written its own detailed record
// (cpu/idt.c, which owns the full fault frame rip/cr2/error_code) can reuse the
// one canonical halt instead of duplicating an inline hlt loop, WITHOUT
// re-writing /PANIC.TXT and clobbering its richer record.
void kpanic_halt(void) __attribute__((noreturn));

#endif // PANIC_H
