// bootstage.h - #ASUSDIAG: ONE named checkpoint per boot step, fanned out to
// every diagnostic channel that happens to exist at that moment.
//
// THE PROBLEM THIS SOLVES. Every diagnostic this kernel emits goes to serial via
// kprintf, and every persistent copy (/BOOTLOG.TXT, /USBLOG.TXT, /AUDIOLOG.TXT,
// /boot/STAGE.TXT, /DEVLOG.TXT) only reaches a medium once a filesystem is
// mounted: bootlog_arm() is gated on g_fat_fs.mounted and panic_log_init() on
// the same. A LAPTOP HAS NO SERIAL PORT. So on a laptop a failure before the
// mount writes NOTHING, ANYWHERE, and the machine is indistinguishable from a
// brick. That is exactly the gap that made the iMac14,4 undiagnosable for days,
// and on unfamiliar hardware dying before the mount is a LIKELY outcome, not an
// edge case: an unenumerable USB controller, an absent 8254, a graphics mode we
// cannot render.
//
// Note also that fs/panic.c's stage_set()/boot_stage_t enum is DELIBERATELY a
// different thing: it starts at STAGE_FS_MOUNTED and covers late boot and
// service launch, because it can only persist once a filesystem exists. This
// facility covers the part BEFORE that, and its sinks are chosen so that each
// one works as early as it possibly can.
//
// WHAT ONE boot_stage() CALL DOES, in this order:
//   1. bootlog_write()      RAM buffer from instruction one, mirrored to serial,
//                           flushed to /BOOTLOG.TXT if a filesystem ever mounts.
//   2. fltrec_write()       raw LBA on the boot medium, no filesystem needed,
//                           live as soon as the USB block device answers.
//   3. early_fb_stage()     straight to the firmware framebuffer, live from the
//                           first instruction of kernel_main.
//   4. gfx_boot_log()       the kernel's own boot-log console, once it exists,
//                           followed by early_fb_banner_refresh() so the banner
//                           is repainted AFTER fb_swap_buffers() has wiped the
//                           front buffer. The banner is therefore always the
//                           most recent thing committed to the glass, which is
//                           the whole point: it is what a frozen machine is
//                           still showing when the user photographs it.
// A sink that is not up yet is a no-op, so a call site never has to know.
//
// WHY C. This file is glue: every line is a call into an existing C subsystem
// (bootlog, graphics, the Rust earlyfb FFI, fltrec). The bounds arithmetic that
// justifies Rust lives in rustkern/earlyfb.rs, which IS Rust. A Rust version of
// THIS file would be one FFI shim per call with no safety gained.
#ifndef BOOTSTAGE_H
#define BOOTSTAGE_H

#include "../types.h"

// The ordinal is what the user reads off a photograph of a hung screen, so it
// is stable and ascending in boot order. Insert new stages at the END of the
// group they belong to rather than renumbering: a number that means a different
// thing in two builds is worse than a gap.
typedef enum {
    BSTAGE_NONE = 0,
    BSTAGE_ENTRY,          // 1  kernel_main reached, before anything
    BSTAGE_SERIAL,         // 2  UART probed and latched
    BSTAGE_EARLYFB,        // 3  firmware framebuffer taken over
    BSTAGE_FORMATTER,      // 4  kformat_selftest passed
    BSTAGE_CANARY,         // 5  per-boot stack canary installed
    BSTAGE_BOOTINFO,       // 6  boot_info magic checked, globals set
    BSTAGE_GDT,            // 7
    BSTAGE_IDT,            // 8
    BSTAGE_PIC,            // 9
    BSTAGE_PIT,            // 10
    BSTAGE_MONO,           // 11 TSC calibration: the longest pre-video step
    BSTAGE_ISR,            // 12
    BSTAGE_SSE,            // 13
    BSTAGE_SYSCALL,        // 14
    BSTAGE_PMM,            // 15
    BSTAGE_VMM,            // 16
    BSTAGE_HEAP,           // 17
    BSTAGE_DEMAND,         // 18
    BSTAGE_CONSOLE,        // 19 fb_init + console_init: the kernel's own video
    BSTAGE_PCI,            // 20
    BSTAGE_GPU,            // 21
    BSTAGE_ATA,            // 22 legacy IDE + AHCI probe
    BSTAGE_USB,            // 23 xHCI bring-up and enumeration
    BSTAGE_USB_DONE,       // 24
    BSTAGE_FATINIT,        // 25
    BSTAGE_USBROOT,        // 26 USB-MSC root probe and FAT mount attempt
    BSTAGE_ACPI,           // 27
    BSTAGE_MADT,           // 28
    BSTAGE_SMPINIT,        // 29 LAPIC / IOAPIC
    BSTAGE_SELFTEST,       // 30 the block of Rust and subsystem self-tests
    BSTAGE_ATAROOT,        // 31 ATA fallback mount
    BSTAGE_EXT2,           // 32 ext2 root partition mount
    BSTAGE_FSCK,           // 33
    BSTAGE_TORAM,          // 34 copy the root device into RAM
    BSTAGE_ROOTFLAG,       // 35 /ROOTEXT2 read, path routing flipped
    BSTAGE_PANICLOG,       // 36 /boot/PANIC.TXT + /boot/STAGE.TXT armed
    BSTAGE_BOOTLOG_ARM,    // 37 the RAM log buffers finally reach the medium
    BSTAGE_DEVLOG,         // 38 hardware inventory written
    BSTAGE_SECURITY,       // 39 SMEP/SMAP: can reboot-loop on hostile firmware
    BSTAGE_SMP_AP,         // 40
    // #QUIETBOOT: THE LAST SEVEN WERE RENUMBERED INTO THE ORDER THEY ACTUALLY
    // FIRE IN. The measured order on a real boot was 41 NETWORK, 44
    // INPUT_DEVICES, 42 SCHEDULER, 43 INTERRUPTS_ON, 45 LOGIN, 47
    // DESKTOP_READY, 46 COMPOSITOR: the enum had been numbered from where the
    // stages were EXPECTED in kernel_main, and three of them sit elsewhere.
    // That was recorded as a known caveat rather than fixed, on this file's own
    // rule that "a number that means a different thing in two builds is worse
    // than a gap".
    //
    // That rule protects a history of photographs taken against older builds.
    // No such history exists: this instrument is one day old and has never been
    // in a golden image, so renumbering it now costs exactly nothing and is the
    // last moment at which that is true. The rule applies from here on.
    BSTAGE_NET,            // 41
    BSTAGE_INPUT,          // 42 keyboard and mouse
    BSTAGE_PROC,           // 43 scheduler and processes
    BSTAGE_STI,            // 44 interrupts enabled for the first time
    BSTAGE_LOGIN,          // 45
    BSTAGE_DESKTOP,        // 46 kernel_main is done and calls desktop_run()
    BSTAGE_COMPOSITOR,     // 47 the compositor actually connected (gui/fb_syscall.c)
    BSTAGE_MAX
} bootstage_id_t;

// ---------------------------------------------------------------------------
// #QUIETBOOT: ON-SCREEN DIAGNOSTICS ARE OFF BY DEFAULT
// ---------------------------------------------------------------------------
// A normal boot shows the splash and nothing else. The two SCREEN sinks above
// (early_fb_stage / gfx_boot_log) are armed only when \boot\DIAG.TXT is present
// on the FAT ESP, which the UEFI bootloader reads before ExitBootServices and
// hands over in boot_info_t.diag_flags (kernel/boot_info.h). main.c calls
// boot_stage_diag_set() with that bit as the first thing it does.
//
// THE TWO PERSISTENT SINKS ARE NOT GATED AND MUST NOT BE. bootlog_write() (the
// RAM buffer, the serial mirror, and /BOOTLOG.TXT once a filesystem mounts) and
// fltrec_write() (the raw-LBA flight recorder) run on EVERY boot. They are
// invisible to the user, they cost a memcpy and at most one sector write, and
// they are the entire reason the ASUS bring-up produced a diagnosis instead of
// a shrug. Silencing the screen is the request; silencing the evidence is not.
//
// WHY A FILE RATHER THAN A REBUILD. The machine you need this on is the one in
// front of you that will not boot, and it is not the machine you build on.
// Commenting the instrument out, or hiding it behind a -D, means the next
// unknown machine costs what this one cost. A marker file is armable with a
// USB stick and any other computer.
//
// AND WHY THE BOOT SAYS WHICH MODE IT IS IN. This project has two harnesses,
// diskimg_boot_harness() and img_shadow_selftest(), that are compiled, linked
// and CALLED ON EVERY BOOT and return at their first line because the markers
// that arm them (/CDTEST.TXT, /IMG193.TXT) exist on no image and in no manifest.
// Every dead-code check comes back green because the function HAS a caller;
// what is missing is a FILE, and nothing anywhere says so. So the loader prints
// what it sent, main.c prints what it received, and once the ESP is mounted
// main.c cross-checks the flag against the file that is actually there. Three
// independent statements, one line each, on a channel nobody looks at unless
// something is wrong.
void boot_stage_diag_set(int on);
int  boot_stage_diag_armed(void);

// Record and display one boot stage. Safe from the first instruction of
// kernel_main: every sink that is not up yet is a no-op.
void boot_stage(bootstage_id_t id);

// Append a detail line under the current stage. Same sinks, no ordinal.
void boot_stage_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Tell the fan-out that the kernel's own framebuffer console is live, so
// subsequent stages route through gfx_boot_log() and then repaint the banner.
// Call once, immediately after console_init().
void boot_stage_console_live(void);

// The last stage entered, and its name. boot_stage_name() is total: an out of
// range id yields "?" rather than reading off the end of the table.
bootstage_id_t boot_stage_current(void);
const char    *boot_stage_name(bootstage_id_t id);

// LAST RESORT: page a text buffer to the screen, forever, and never return.
//
// Call this ONLY when the boot has definitively failed in a way that leaves no
// writable medium, i.e. no filesystem mounted at all. In that state there is
// nothing to continue TO (no /APPS, no compositor, no logs that can reach a
// disk) and the pre-existing behaviour was to carry on into a half-dead system
// that told the user nothing. The screen is the only channel left on a machine
// with no serial port, so we spend it: the full hardware inventory is paged
// across the display on a timer so it can be photographed, and the cycle
// repeats so a missed page comes round again.
//
// `title` labels every page. `body` is NUL-terminated text; newlines break
// rows and over-long rows wrap rather than clip, because a truncated PCI line
// is a missing device.
void boot_stage_report_forever(const char *title, const char *body)
    __attribute__((noreturn));

#endif // BOOTSTAGE_H
