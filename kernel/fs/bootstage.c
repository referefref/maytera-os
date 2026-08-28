// bootstage.c - #ASUSDIAG: one named checkpoint per boot step, fanned out to
// every diagnostic channel that happens to exist at that moment.
// See bootstage.h for why this exists. Short version: a laptop has no serial
// port, and every persistent log this kernel writes is gated on a filesystem
// being mounted, so a pre-mount death currently leaves no evidence anywhere.
#include "bootstage.h"
#include "bootlog.h"
#include "../serial.h"
#include "../string.h"
#include "../video/graphics.h"
#include "../cpu/mono.h"      // the SHARED bounded delay primitive; see diag_pause_ms
#include <stdarg.h>

// rustkern/earlyfb.rs. Declared here rather than in a header the Rust side
// would have to agree with by convention: this is the only C file that talks to
// it, and the exported names are locked by kernel/rust-symbols.manifest.
extern void early_fb_stage(uint32_t stage, const char *name);
extern void early_fb_note(const char *msg);
extern void early_fb_banner_refresh(uint32_t stage, const char *name);
extern uint32_t early_fb_page(const char *body, const char *title,
                              uint32_t page, uint32_t pages);
extern int early_fb_ready(void);
extern int early_fb_force_arm(void);

// fs/fltrec.c, the raw-LBA flight recorder. Weakly referenced through a guard
// so this file links whether or not that module is present in a given build.
extern int  fltrec_armed(void) __attribute__((weak));
extern void fltrec_write(const char *line) __attribute__((weak));
extern void fltrec_flush(void) __attribute__((weak));
extern void fltrec_seal(int ok) __attribute__((weak));

// Ordinal to name. Kept in ONE place and index-checked, so a photograph of the
// screen and a line in /BOOTLOG.TXT always name the same thing.
static const char *const g_stage_names[BSTAGE_MAX] = {
    [BSTAGE_NONE]        = "NONE",
    [BSTAGE_ENTRY]       = "KERNEL_ENTRY",
    [BSTAGE_SERIAL]      = "SERIAL_PROBE",
    [BSTAGE_EARLYFB]     = "EARLY_FRAMEBUFFER",
    [BSTAGE_FORMATTER]   = "FORMATTER_SELFTEST",
    [BSTAGE_CANARY]      = "STACK_CANARY",
    [BSTAGE_BOOTINFO]    = "BOOT_INFO",
    [BSTAGE_GDT]         = "GDT",
    [BSTAGE_IDT]         = "IDT",
    [BSTAGE_PIC]         = "PIC",
    [BSTAGE_PIT]         = "PIT_8254",
    [BSTAGE_MONO]        = "TSC_CALIBRATION",
    [BSTAGE_ISR]         = "INTERRUPT_HANDLERS",
    [BSTAGE_SSE]         = "FPU_SSE",
    [BSTAGE_SYSCALL]     = "SYSCALL_MSRS",
    [BSTAGE_PMM]         = "PHYS_MEMORY",
    [BSTAGE_VMM]         = "VIRT_MEMORY",
    [BSTAGE_HEAP]        = "KERNEL_HEAP",
    [BSTAGE_DEMAND]      = "DEMAND_PAGING",
    [BSTAGE_CONSOLE]     = "VIDEO_CONSOLE",
    [BSTAGE_PCI]         = "PCI_SCAN",
    [BSTAGE_GPU]         = "GPU_DETECT",
    [BSTAGE_ATA]         = "ATA_AHCI_PROBE",
    [BSTAGE_USB]         = "USB_XHCI_INIT",
    [BSTAGE_USB_DONE]    = "USB_ENUM_DONE",
    [BSTAGE_FATINIT]     = "FAT_INIT",
    [BSTAGE_USBROOT]     = "USB_ROOT_MOUNT",
    [BSTAGE_ACPI]        = "ACPI",
    [BSTAGE_MADT]        = "ACPI_MADT",
    [BSTAGE_SMPINIT]     = "LAPIC_IOAPIC",
    [BSTAGE_SELFTEST]    = "SELFTESTS",
    [BSTAGE_ATAROOT]     = "ATA_ROOT_MOUNT",
    [BSTAGE_EXT2]        = "EXT2_ROOT_MOUNT",
    [BSTAGE_FSCK]        = "EXT2_FSCK",
    [BSTAGE_TORAM]       = "ROOT_TO_RAM",
    [BSTAGE_ROOTFLAG]    = "ROOT_ROUTING",
    [BSTAGE_PANICLOG]    = "PANIC_LOG_ARM",
    [BSTAGE_BOOTLOG_ARM] = "BOOTLOG_ARM",
    [BSTAGE_DEVLOG]      = "DEVICE_INVENTORY",
    [BSTAGE_SECURITY]    = "SMEP_SMAP",
    [BSTAGE_SMP_AP]      = "SMP_AP_START",
    [BSTAGE_NET]         = "NETWORK",
    [BSTAGE_PROC]        = "SCHEDULER",
    [BSTAGE_STI]         = "INTERRUPTS_ON",
    [BSTAGE_INPUT]       = "INPUT_DEVICES",
    [BSTAGE_LOGIN]       = "LOGIN",
    [BSTAGE_COMPOSITOR]  = "COMPOSITOR",
    // "DESKTOP_RUN", not "DESKTOP_READY". This fires in kernel_main immediately
    // BEFORE desktop_run(), so it means "the kernel finished its job", which is
    // not the same claim as "the desktop appeared". The old name asserted the
    // stronger one. BSTAGE_COMPOSITOR, set later from gui/fb_syscall.c, is the
    // stage that means the compositor really connected.
    [BSTAGE_DESKTOP]     = "DESKTOP_RUN",
};

static bootstage_id_t g_stage_cur;
static int            g_console_live;

// #QUIETBOOT: the SCREEN sinks only. See the header for why the persistent
// sinks are deliberately not gated by this. Default 0 = quiet, so a build that
// somehow never calls boot_stage_diag_set() is quiet rather than noisy, which
// is the direction a default should fail in.
static int            g_diag_screen;

void boot_stage_diag_set(int on) { g_diag_screen = on ? 1 : 0; }
int  boot_stage_diag_armed(void) { return g_diag_screen; }

const char *boot_stage_name(bootstage_id_t id) {
    if ((int)id < 0 || (int)id >= (int)BSTAGE_MAX) return "?";
    const char *n = g_stage_names[id];
    return n ? n : "?";
}

bootstage_id_t boot_stage_current(void) { return g_stage_cur; }

void boot_stage_console_live(void) { g_console_live = 1; }

void boot_stage(bootstage_id_t id) {
    const char *name = boot_stage_name(id);
    g_stage_cur = id;

    // 1. bootlog_write() already mirrors to serial, so do NOT also kprintf: one
    // event must produce one serial line, or a log becomes harder to read the
    // more instrumentation you add. It buffers in .bss from instruction one and
    // is flushed to /BOOTLOG.TXT by bootlog_arm() if a filesystem ever mounts.
    bootlog_write("[BSTAGE %02d] %s", (int)id, name);

    // 2. The raw-LBA flight recorder: no filesystem needed, live as soon as the
    // boot block device answers. Guarded because the module is optional.
    if (fltrec_write) {
        char fl[96];
        snprintf(fl, sizeof(fl), "[BSTAGE %02d] %s", (int)id, name);
        fltrec_write(fl);
    }

    // 3. and 4. The screen. Before the kernel's own framebuffer stack exists,
    // early_fb_stage() is the ONLY channel; after it exists, gfx_boot_log() owns
    // the display and we repaint the banner AFTERWARDS, because gfx_boot_log()
    // ends in fb_swap_buffers() which copies the whole back buffer over the
    // front one and would otherwise erase it. Repainting last is what makes the
    // banner the most recent thing committed to the glass, and therefore what a
    // frozen machine is still showing when it is photographed.
    //
    // #QUIETBOOT: BOTH of these are screen output and BOTH are gated. The
    // gfx_boot_log() branch matters as much as the early-framebuffer one: it
    // paints a dmesg-style line onto the boot splash, so leaving it ungated
    // would trade one ugly boot for a slightly less ugly one. early_fb_stage()
    // and early_fb_banner_refresh() are no-ops of their own accord on a quiet
    // boot (early_fb_init() never armed the screen), so the test here is
    // belt-and-braces for them and load-bearing for gfx_boot_log().
    if (g_diag_screen) {
        if (!g_console_live) {
            early_fb_stage((uint32_t)id, name);
        } else {
            char gl[96];
            snprintf(gl, sizeof(gl), "[%02d] %s", (int)id, name);
            gfx_boot_log(gl);
            early_fb_banner_refresh((uint32_t)id, name);
        }
    }

    // SEAL THE FLIGHT RECORDER ON A COMPLETED BOOT.
    //
    // Without this every slot reads "OPEN: this boot did not finish", including
    // the boots that finished perfectly, which destroys the one distinction the
    // record exists to make. Measured on the first VM boot of this work: three
    // consecutive successful boots all recorded as OPEN. A verdict that is
    // always the same is not a verdict.
    //
    // Sealed at DESKTOP_READY because that is the last thing kernel_main does
    // before handing the machine to the user, and it is the definition of
    // "this boot worked" that matters to the person holding the laptop.
    if (id == BSTAGE_DESKTOP && fltrec_seal) {
        fltrec_seal(1);
    }
}

void boot_stage_note(const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    bootlog_write("[BSTAGE   ] %s", buf);
    if (fltrec_write) fltrec_write(buf);

    if (g_diag_screen) {
        if (!g_console_live) {
            early_fb_note(buf);
        } else {
            gfx_boot_log(buf);
            early_fb_banner_refresh((uint32_t)g_stage_cur, boot_stage_name(g_stage_cur));
        }
    }
}

// ---------------------------------------------------------------------------
// THE NO-MEDIUM REPORT
// ---------------------------------------------------------------------------
// The inventory can run to ~128 KB of text, which is many screens, so this
// pages it. Page starts are recorded on the first pass (early_fb_page returns
// the byte count it consumed) and then replayed, so the cycle costs no extra
// scanning and a page the user missed comes round again.
#define DIAG_MAX_PAGES   40
#define DIAG_PAGE_MS     8000   // long enough to read and photograph one screen

// Bounded delay in the one context where there is no alternative: no scheduler
// exists, interrupts are off, and the boot is over. mono_busy_delay_us() is the
// SHARED primitive for exactly this (cpu/mono.h) and is itself bounded and
// already reviewed; we do not hand-roll a second one here.
static void diag_pause_ms(uint32_t ms) {
    // #ASUSDIAG: INCLUDE THE HEADER, DO NOT HAND-DECLARE THE SYMBOL.
    // This was originally a private extern of mono_busy_delay_us,
    // and it FAILED THE LINK, because no such symbol exists: cpu/mono.h defines
    // mono_busy_delay_us as a static inline wrapper around the real export
    // mono_busy_delay_us_rs. A private extern cannot see that, so it declared a
    // function that is not there. The header is included at the top of this file
    // instead, which is the shared-primitive rule doing its job.
    // Split into 100ms slices so the total is expressed in terms of the shared
    // primitive rather than handed one enormous argument to saturate on.
    for (uint32_t i = 0; i < ms / 100; i++) mono_busy_delay_us(100000);
}

void boot_stage_report_forever(const char *title, const char *body) {
    if (!body) body = "(no inventory available)";

    // Every sink still gets the verdict, in case one of them is alive after all
    // (a serial-equipped VM, a flight recorder armed before the mount failed).
    bootlog_write("[BSTAGE] BOOT FAILED: no filesystem mounted. Paging the hardware "
                  "inventory to the screen forever; power off and photograph it.");
    if (fltrec_write) {
        fltrec_write("BOOT FAILED: no filesystem mounted");
        if (fltrec_flush) fltrec_flush();
    }

    // #QUIETBOOT: ARM THE SCREEN ON DEMAND, IGNORING THE QUIET DEFAULT.
    //
    // This function is reached only when NO FILESYSTEM MOUNTED AT ALL, which
    // means the boot has already failed and there is nothing left to continue
    // to: no /APPS, no compositor, and no log that can reach a disk. A quiet
    // boot in that state would be a black screen, which is exactly the brick
    // this instrumentation exists to prevent, and it is not what the user asked
    // for when he asked for a clean boot: this report CANNOT appear on a boot
    // that works. So failure outranks quiet here, and only here.
    //
    // early_fb_force_arm() is a no-op if the screen is already ours, and
    // returns 0 only if the firmware geometry was unusable in the first place.
    if (!early_fb_force_arm()) {
        // No screen and no disk. There is genuinely nothing left, so say it on
        // serial and halt rather than spinning silently forever.
        extern void kpanic(const char *fmt, ...) __attribute__((noreturn));
        kpanic("no root filesystem AND no usable framebuffer: no diagnostic channel exists");
    }

    static uint32_t starts[DIAG_MAX_PAGES];
    uint32_t npages = 0;
    uint32_t off = 0;

    // First pass: draw each page in turn and remember where it began.
    while (npages < DIAG_MAX_PAGES) {
        starts[npages] = off;
        uint32_t used = early_fb_page(body + off, title, npages + 1, 0);
        npages++;
        if (used == 0) break;             // nothing consumed: end of buffer
        off += used;
        if (body[off] == 0) break;        // consumed the lot
        diag_pause_ms(DIAG_PAGE_MS);
    }
    if (npages == 0) npages = 1;

    // Then cycle, now that the true page count is known so each page can say
    // "3/17" rather than "3/?".
    for (;;) {
        for (uint32_t p = 0; p < npages; p++) {
            early_fb_page(body + starts[p], title, p + 1, npages);
            // Page 1 holds longer. The inventory is ordered identity-first
            // (machine identity, CPU, framebuffer) and the memory map that
            // follows is by far the bulk of it, so with a uniform dwell the one
            // page that answers "what machine is this" flashes past once every
            // couple of minutes while a dozen pages of memory ranges do not.
            // The person doing this is standing at a laptop with a phone.
            diag_pause_ms(p == 0 ? DIAG_PAGE_MS * 2 : DIAG_PAGE_MS);
        }
    }
}
