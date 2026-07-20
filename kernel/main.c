// main.c - MayteraOS 64-bit kernel main function
#include "types.h"
#include "boot_info.h"
#include "serial.h"
#include "string.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pic.h"
#include "cpu/mono.h"
#include "cpu/isr.h"
#include "cpu/sse.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "video/framebuffer.h"
#include "video/console.h"
#include "video/graphics.h"
#include "drivers/ata.h"
#include "drivers/pci.h"
#include "drivers/sound.h"
#include "drivers/audio.h"
#include "drivers/acpi.h"
#include "net/net.h"
#include "net/ip.h"
#include "net/icmp.h"
#include "net/arp.h"
#include "net/dhcp.h"
#include "net/remote_ctrl.h"
#include "net/https.h"
#include "fs/fat.h"
#include "fs/fat_readahead.h"
#include "fs/bootlog.h"           // #307 real-hw: persistent /BOOTLOG.TXT
#include "fs/blockdev.h"          // #307: ATA vs USB MSC root routing
#include "drivers/usb_msc.h"      // #307: USB thumb-drive root selection
#include "drivers/hotplug.h"      // #418 FAKE-audit fix: USB hotplug manager init
#include "drivers/usb_hid.h"      // #307 bootlog: keyboard/mouse presence summary
#include "drivers/mouse.h"
#include "gui/window.h"
#include "gui/desktop.h"
#include "gui/login.h"
#include "gui/syslog.h"
#include "proc/process.h"
#include "proc/syscall.h"
#include "fs/perms.h"
#include "proc/users.h"
#include "proc/services.h"
#include <stdarg.h>

// Global FAT filesystem (non-static for access from other modules)
fat_fs_t g_fat_fs;

// Current working directory
static char g_cwd[256] = "/";

// Global boot info pointer
boot_info_t *g_boot_info = NULL;

// Forward declarations
void print_memory_map(void);
void print_framebuffer_info(void);
void kernel_shell(void);

// ---- #373 heartbeat worker (restored b713): its definition AND its
// proc_create() startup call were both silently dropped in the b681->b702
// main.c worker-cluster refactor churn. Restored verbatim from the b691
// baseline. ----
// #373 heartbeat: low-priority kernel thread that writes a one-line "[HB]"
// record roughly every 2 seconds. On real hardware (iMac14,4) there is no serial
// console, so this is the ONLY telemetry from the physical machine.
//
// #373 real-HW freeze update: the v1.65 iMac boot log showed the heartbeat climb
// to ~62s then STOP right as the desktop rendered its first frame, with the gaps
// between beats GROWING (4s -> 27s) beforehand. The compositor is NOT on-demand
// (it free-runs a timer-driven animation on the dev VMs with zero input), so the
// freeze is a genuine wedge. Prime suspect: the OLD heartbeat used bootlog_write()
// which rewrites the WHOLE growing /BOOTLOG.TXT every beat - an O(n^2) series of
// full-file rewrites over the slow USB-MSC stack. This worker now uses the
// constant-cost bootlog_heartbeat() (separate bounded /HEARTBEAT.TXT) and also
// records the framebuffer flip count, so the next boot distinguishes: heartbeat
// keeps advancing = OS alive (compositor/display bug); heartbeat stops = kernel
// wedge (if it no longer wedges, the growing bootlog write WAS the culprit).
// tick and ctx come from independent sources so a stuck timer (tick frozen) is
// distinguishable from a stuck scheduler (ctx frozen); flips shows the compositor.
// #375 per-thread CPU accounting: sched_tick() already credits total_time (one
// per timer tick) to whichever thread is running, so total_time is a
// tick-sampled CPU counter. hb_top_consumers() diffs it across the ~2s heartbeat
// interval and formats the TOP consumers as "name:pct" (pct = share of one
// core). On the real iMac - where ONE core is pegged at idle but VMs idle low -
// this makes a SINGLE boot NAME the offending thread (e.g. a USB device poll, a
// service, the compositor) in /HEARTBEAT.TXT, instead of us guessing. The whole
// point: a hardware-specific busy-loop is otherwise invisible.
#define HB_MAXPROC 96
static void hb_append(char *out, int outsz, int *pos, const char *s) {
    while (*s && *pos < outsz - 1) out[(*pos)++] = *s++;
    out[*pos] = 0;
}
static void hb_top_consumers(char *out, int outsz, uint64_t interval_ticks) {
    static proc_info_t prev[HB_MAXPROC];
    static int prev_n = 0;
    proc_info_t cur[HB_MAXPROC];
    int n = proc_snapshot(cur, HB_MAXPROC);

    int best[3] = { -1, -1, -1 };
    uint64_t bestd[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        uint64_t d = 0; int found = 0;
        for (int j = 0; j < prev_n; j++) {
            if (prev[j].pid == cur[i].pid) {
                if (cur[i].cpu_ticks >= prev[j].cpu_ticks)
                    d = cur[i].cpu_ticks - prev[j].cpu_ticks;
                found = 1; break;
            }
        }
        if (!found) continue;   // new this interval: no baseline, skip
        for (int k = 0; k < 3; k++) {
            if (d > bestd[k]) {
                for (int m = 2; m > k; m--) { bestd[m] = bestd[m-1]; best[m] = best[m-1]; }
                bestd[k] = d; best[k] = i; break;
            }
        }
    }
    int pos = 0; out[0] = 0;
    hb_append(out, outsz, &pos, "top=");
    int wrote = 0;
    for (int k = 0; k < 3; k++) {
        if (best[k] < 0 || bestd[k] == 0) break;
        int pct = interval_ticks ? (int)(bestd[k] * 100 / interval_ticks) : 0;
        if (pct > 100) pct = 100;
        char piece[48];
        snprintf(piece, sizeof(piece), "%s%s:%d", wrote ? "," : "", cur[best[k]].name, pct);
        hb_append(out, outsz, &pos, piece);
        wrote++;
    }
    if (!wrote) hb_append(out, outsz, &pos, "-");

    for (int i = 0; i < n; i++) prev[i] = cur[i];
    prev_n = n;
}

static void heartbeat_worker(void *arg) {
    (void)arg;
    extern volatile uint64_t timer_ticks;      // cpu/isr.c, monotonic PIT tick
    extern volatile uint64_t g_ctx_switches;   // proc/process.c, scheduler switches
    extern volatile uint64_t g_fb_flip_count;  // gui/fb_syscall.c, presents so far
    extern uint32_t g_timer_hz;
    uint64_t n = 0;
    uint64_t last_ticks = timer_ticks;
    char topbuf[128];
    kprintf("[HB] heartbeat worker thread running\n");
    for (;;) {
        uint32_t hz = g_timer_hz ? g_timer_hz : 250;
        uint64_t ticks = timer_ticks;
        ++n;
        // #375: RAM block-cache stats. On a USB-root box, hits climbing while
        // misses stay ~flat proves runtime reads are served from RAM and the
        // stick is idle (the fix for the pegged core). blkc=hits/misses.
        uint64_t blkc_h = 0, blkc_m = 0; int blkc_on = 0;
        blk_cache_stats(&blkc_h, &blkc_m, &blkc_on);
        // #375: name the top CPU consumers over this interval (the decisive
        // real-hardware diagnostic for the pegged core).
        uint64_t interval = (ticks > last_ticks) ? (ticks - last_ticks) : 1;
        hb_top_consumers(topbuf, sizeof(topbuf), interval);
        last_ticks = ticks;
        // #373 real-HW freeze diagnostic. Include the framebuffer flip count so
        // the log tells us whether the compositor keeps presenting frames or
        // stopped after frame 1 (see gui/fb_syscall.c). Always mirror to serial.
        // #525 INSTRUMENT: `uptime` is ticks/hz, i.e. CIRCULAR - it can never
        // reveal a tick-delivery pathology because it is derived from the very
        // counter under suspicion. `mono` is the independent TSC-backed clock
        // (cpu/mono.h). Compare them: if uptime races ahead of mono, ticks are
        // arriving in a reinjected BURST and every timer_ticks deadline in the
        // kernel is being silently shortened (#524/#499). They should track.
        // #525: report the measured tick-DELIVERY pathology alongside the
        // counters it corrupts. burst=N/Uus means N ticks were delivered inside
        // U real microseconds, i.e. a `timer_ticks + N` deadline can be erased
        // in U microseconds of wall clock. mingap is the tightest inter-tick
        // spacing seen (nominal is 4000us at 250Hz).
        {
            extern uint64_t tickburst_max_run, tickburst_run_us, tickburst_min_gap;
            // Report ONLY on real pathology, and only when the worst case grows,
            // so a healthy boot stays silent and this can ship permanently. A
            // run of >=100 sub-millisecond-spaced ticks cannot be normal at any
            // sane tick rate: it means ticks are being REPLAYED from a backlog
            // rather than marking time, and every timer_ticks deadline in the
            // kernel is being shortened by exactly that much (#524/#499/#525).
            static uint64_t tb_reported = 0;
            if (tickburst_max_run >= 100 && tickburst_max_run > tb_reported) {
                tb_reported = tickburst_max_run;
                kprintf("[TICKBURST] %llu ticks delivered in %lluus of REAL time "
                        "(min gap %lluus, nominal %uus): timer_ticks is NOT a "
                        "wall clock here - a %llu-tick deadline would expire in "
                        "%lluus. Use cpu/mono.h for deadlines.\n",
                        tickburst_max_run, tickburst_run_us,
                        tickburst_min_gap == ~0ULL ? 0 : tickburst_min_gap,
                        (unsigned)(1000000u / (hz ? hz : 250)),
                        tickburst_max_run, tickburst_run_us);
            }
        }
        kprintf("[HB] tick=%lu uptime=%lus mono=%llums ctxsw=%lu flips=%lu hb=%lu blkc=%lu/%lu %s\n",
                (unsigned long)ticks, (unsigned long)(ticks / hz), mono_ms(),
                (unsigned long)g_ctx_switches,
                (unsigned long)g_fb_flip_count, (unsigned long)n,
                (unsigned long)blkc_h, (unsigned long)blkc_m, topbuf);
        // Use the LIGHTWEIGHT, constant-cost heartbeat writer (separate
        // /HEARTBEAT.TXT, bounded ring) instead of bootlog_write(). The old
        // bootlog_write() rewrote the whole growing /BOOTLOG.TXT every second,
        // which at USB-MSC speed grew to 4-27s per write and is the prime
        // suspect for wedging the iMac ~62s in (#373). bootlog_heartbeat() can
        // never grow without bound, so it cannot reproduce that starvation.
        if (bootlog_is_armed()) {
            char hb[224];
            snprintf(hb, sizeof(hb),
                     "[HB] tick=%lu uptime=%lus ctxsw=%lu flips=%lu hb=%lu blkc=%lu/%lu %s",
                     (unsigned long)ticks,
                     (unsigned long)(ticks / hz),
                     (unsigned long)g_ctx_switches,
                     (unsigned long)g_fb_flip_count,
                     (unsigned long)n,
                     (unsigned long)blkc_h, (unsigned long)blkc_m, topbuf);
            bootlog_heartbeat(hb);
        }
        proc_sleep(2000);   // ~2 s between heartbeats (lighter USB-MSC load)
    }
}

// Kernel main entry point
// Called from entry.asm with boot_info pointer in RDI
void kernel_main(boot_info_t *boot_info) {
    // Initialize serial port first for debugging
    serial_init(COM1, 115200);

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  MayteraOS 64-bit Kernel v1.0\n");
    kprintf("========================================\n\n");

    kprintf("========================================\n");
    kprintf("  MayteraOS 64-bit Kernel v1.0\n");
    kprintf("========================================\n\n");

    kprintf("[BOOT] Kernel entry point: 0x%p\n", (void*)kernel_main);
    kprintf("[BOOT] Boot info address:  0x%p\n", (void*)boot_info);
    kprintf("[BOOT] Initializing kernel subsystems...\n");

    // Validate boot info
    if (boot_info == NULL) {
        kprintf("[ERROR] Boot info is NULL!\n");
        goto halt;
    }

    // Verify magic number
    if (boot_info->magic != BOOT_INFO_MAGIC) {
        kprintf("[ERROR] Invalid boot info magic: 0x%lx (expected 0x%lx)\n",
                boot_info->magic, BOOT_INFO_MAGIC);
        kprintf("[KERNEL] Continuing without boot info validation...\n");
    } else {
        kprintf("[KERNEL] Boot info magic verified\n");
    }

    // Save global boot info
    g_boot_info = boot_info;

    // Print system information
    kprintf("\n[MEMORY] Total RAM: %lu MB\n", boot_info->total_memory / MB);
    kprintf("[MEMORY] Memory map entries: %u\n", boot_info->memory_map_entries);

    // Print framebuffer info
    int spinner_frame = 0;  // For boot spinner animation
    if (boot_info->framebuffer.address != 0) {
        print_framebuffer_info();
    }

    // Print ACPI info
    if (boot_info->acpi.rsdp_address != 0) {
        kprintf("\n[ACPI] RSDP at 0x%lx (version %u)\n",
                boot_info->acpi.rsdp_address, boot_info->acpi.rsdp_version);
    }

    // Initialize CPU subsystem
    kprintf("\n");
    gdt_init();     // Global Descriptor Table
    idt_init();     // Interrupt Descriptor Table
    pic_init();     // Programmable Interrupt Controller
    pit_init(250);  // 250 Hz default, games can boost to 1000
    // #525: start THE shared monotonic clock (cpu/mono.h) the moment the PIT
    // is programmed. It must be ready before usb_init() enumerates the xHCI,
    // whose transfer deadlines depend on it; calibration reads PIT channel 0's
    // counter directly, so it works here with interrupts still off. Deadlines
    // must never be measured in timer_ticks: ticks count DELIVERY, not TIME,
    // and KVM reinjects a starved vCPU's missed ticks in a ~1250-tick burst
    // (a nominal 5s at 250Hz) in ~15ms of real time (#524/#499).
    {
        uint64_t mono_khz = mono_init(g_timer_hz);
        if (mono_khz) {
            kprintf("[MONO] TSC calibrated: %llu kHz (~%llu MHz) via PIT ch0\n",
                    mono_khz, mono_khz / 1000);
            bootlog_write("[MONO] TSC calibrated %llu kHz (~%llu MHz)",
                          mono_khz, mono_khz / 1000);
        } else {
            // Not fatal: every mono reader reports not-ready and callers keep
            // their previous tick behaviour, so this is never worse than before.
            kprintf("[MONO] TSC calibration FAILED - deadlines fall back to ticks\n");
            bootlog_write("[MONO] TSC calibration FAILED");
        }
    }
    isr_init();     // Interrupt Service Routines
    sse_init();     // SSE/FPU support
    syscall_init(); // SYSCALL/SYSRET support
    { extern void smp_cpu_local_init(uint32_t); smp_cpu_local_init(0); } // #279 3b-1.5 BSP GS base

    // Initialize memory management
    kprintf("\n");
    pmm_init(boot_info->memory_map_address, boot_info->memory_map_entries);
    vmm_init();     // Virtual Memory Manager
    heap_init();    // Kernel Heap
    // #429 (restored b713): demand paging / COW init, dropped in the churn.
    { extern void demand_init(void); demand_init(); }

    // Initialize graphics subsystem
    kprintf("\n");
    if (boot_info->framebuffer.address != 0) {
        console_init(&boot_info->framebuffer);
        // Enable direct mode to draw boot splash directly to screen (no buffering)
        // Use double buffering with explicit swap instead of direct mode
        kprintf("[KERNEL] Framebuffer initialized, showing boot screen...\n");
        // Show simple boot screen with spinner (before filesystem)
        gfx_boot_simple();
        // Animate spinner during early boot
        for (int k = 0; k < 8; k++) { gfx_boot_spinner(k); for (int j = 0; j < 500000; j++) io_wait(); }
    } else {
        kprintf("[VIDEO] WARNING: No framebuffer available\n");
    }

    // Initialize PCI bus (needed for storage controller detection)
    gfx_boot_spinner(spinner_frame++); // Scanning PCI
    pci_init();
    syslog_log(LOG_INFO, "PCI bus scan complete");
    gfx_boot_spinner(spinner_frame++);

    // Initialize storage drivers
    gfx_boot_spinner(spinner_frame++); // Storage init
    ata_init();
    syslog_log(LOG_INFO, "ATA storage initialized");

    // Test ATA read on first available drive
    uint8_t ata_channel = 0, ata_drive = 0;
    int ata_found = ata_get_first_drive(&ata_channel, &ata_drive);
    if (ata_found == 0) {
        gfx_boot_spinner(spinner_frame++); // ATA test
        ata_test_read(ata_channel, ata_drive);
    }
    gfx_boot_spinner(spinner_frame++);

    // #323: Initialize USB subsystem (xHCI) and enumerate devices. This brings
    // up a passed-through USB Audio Class DAC (NuForce uDAC) for real sound out.
    // Runs here (after PCI/ATA, interrupts still off) so control transfers are
    // synchronous and the sine table is built before preemption starts.
    gfx_boot_spinner(spinner_frame++); // USB init
    { extern void usb_init(void); usb_init(); }
    syslog_log(LOG_INFO, "USB subsystem initialized");
    // #307 real-hardware bring-up: this can't hit disk yet (no filesystem is
    // mounted this early), but bootlog_write() buffers in RAM regardless and
    // bootlog_arm() (once FAT mounts) flushes it all - so this summary still
    // ends up in /BOOTLOG.TXT even though it happens before the mount.
    bootlog_write("[MAIN] USB init complete: %d MSC device(s), keyboard=%s, mouse=%s",
                  usb_msc_get_device_count(),
                  usb_hid_get_keyboard() ? "yes" : "no",
                  usb_hid_get_mouse() ? "yes" : "no");
    {   // #366: same summary on screen (survives when storage never mounts)
        char ul[96];
        snprintf(ul, sizeof(ul), "[USB] MSC=%d kbd=%s mouse=%s",
                 usb_msc_get_device_count(),
                 usb_hid_get_keyboard() ? "yes" : "no",
                 usb_hid_get_mouse() ? "yes" : "no");
        gfx_boot_log(ul);
    }
    gfx_boot_spinner(spinner_frame++);

    // #307: Prefer a USB thumb drive as the root filesystem when one carries a
    // valid MayteraOS layout (a FAT ESP at LBA 2048 containing /boot/kernel.elf).
    // This is what makes a USB-booted stick self-hosting on real hardware.
    //
    // RESTORED 2026-07-09 (#307): this ~90-line USB-MSC root-mount block was
    // deleted from main.c as collateral damage of the #71/#702 audio-deferral
    // refactor (build b694-b702 range), which now occupies the region right
    // above. Its loss made the real iMac14,4 (no legacy IDE, boots purely from
    // a USB-MSC stick behind xHCI) fall through to the ATA-only mount path,
    // which found no drive, left g_fat_fs unmounted, and dropped login to the
    // unfinished "Select User" stub. QEMU IDE VMs kept working, hiding it.
    //
    // IMPORTANT: do this immediately after usb_init() and BEFORE ACPI/SMP
    // bring-up. usb_init() enumerated the xHCI with interrupts off and the
    // controller in a known-good polled-transfer state; the SCSI READ(10)
    // transfers below reuse exactly that state. Deferring the mount until after
    // apic_init()/smp_start_aps() (which reprogram the IOAPIC and mask legacy
    // IRQs) leaves the xHCI interrupter in a state where only the first bulk
    // transfer completes and every subsequent one times out, so the root mount
    // must happen here. If no valid USB root is found we fall through to the ATA
    // disk later, so existing ATA-only VMs boot exactly as before (no regression).
    fat_init();   // trivial (prints banner); the FAT driver has no other state

    // #418 FAKE-audit CRITICAL fix: hotplug_init() (drivers/hotplug.c) registers
    // hotplug_handle_usb_event() with the USB-MSC driver so a USB drive plugged
    // in AFTER boot gets auto-mounted and a desktop icon added. It only registers
    // a callback and touches no disk itself, so it is safe to call here right
    // after usb_init() (xHCI is up) and fat_init() (FAT/FS types are ready).
    hotplug_init();

    int fs_mounted_usb = 0;
    // #539: FAT ESP used-data hint, carried out of the USB-mount block so the
    // USB TO-RAM trigger can be DEFERRED until after the ext2 ROOT partition is
    // located, then sized to span the whole two-partition image. 0 = no USB root
    // or no hint (single-FAT / ATA VM paths are unaffected: see the deferred
    // trigger after the ext2 mount block below).
    uint64_t fat_used_hint = 0;
    {
        int nusb = usb_msc_get_device_count();
        kprintf("[MAIN] #307: %d USB MSC device(s) present\n", nusb);
        for (int ui = 0; ui < nusb && !fs_mounted_usb; ui++) {
            usb_msc_device_t *d = usb_msc_get_device(ui);
            if (!d || !d->ready) continue;
            uint32_t bs = d->block_size ? d->block_size : d->luns[0].block_size;
            if (bs != 512) {
                kprintf("[MAIN] #307: USB MSC %d block size %u != 512, not usable as root\n", ui, bs);
                continue;
            }
            kprintf("[MAIN] #307: probing USB MSC device %d (%llu blocks) as root...\n",
                    ui, (unsigned long long)d->num_blocks);
            blk_set_root_usb(ui);
            // #307/#433: real xHCI bulk transfers can miss on the first touch
            // (Enable Slot / Address Device / first READ(10) transiently fail
            // then succeed a few ms later). Bounded-retry the USB root mount a
            // few times with a short busy-wait backoff (interrupts are still off
            // here, so no scheduler sleep is available). The ATA fallback path
            // below is NOT retried - only this USB path needs it.
            int usb_mounted = 0;
            for (int attempt = 0; attempt < 3 && !usb_mounted; attempt++) {
                if (attempt > 0) {
                    kprintf("[MAIN] #307: USB MSC %d root mount retry %d/3...\n", ui, attempt + 1);
                    for (volatile unsigned long b = 0; b < 20000000UL; b++) { }  // short backoff
                }
                if (fat_mount_lba(0, 2048, &g_fat_fs) == 0 &&
                    fat_exists(&g_fat_fs, "/boot/kernel.elf")) {
                    usb_mounted = 1;
                }
            }
            if (usb_mounted) {
                fs_mounted_usb = 1;
                gfx_boot_spinner(spinner_frame++);
                kprintf("[MAIN] #307: root filesystem is USB MSC device %d (GPT ESP @LBA2048)\n", ui);
                syslog_log(LOG_INFO, "FAT filesystem mounted from USB (GPT)");
                gfx_boot_log("[BOOT] root: USB thumb drive");
                fat_print_info(&g_fat_fs);
                if (fat_readahead_init(RA_DEFAULT_BUFFER_SIZE) == 0)
                    kprintf("[BOOT] FAT read-ahead cache initialized (256KB)\n");
                if (gfx_load_boot_image_from_disk() == 0) {
                    gfx_boot_log("[BOOT] Boot image loaded from disk");
                    gfx_boot_splash();
                    fb_swap_buffers();
                    gfx_boot_progress(60);
                }
                // #375/#417: copy the USB root into RAM (one big sequential read
                // with progress on the splash), so every later file read is RAM
                // speed and the slow stick is never touched for reads again.
                // #417: size the RAM window off ACTUAL USED data, not the raw
                // partition/device capacity (a live-USB stick's GPT can be
                // expanded to fill a much bigger drive than the real payload).
                // #539: the ACTUAL trigger is DEFERRED to after the ext2 ROOT
                // partition is located (see below), because on the two-partition
                // layout the FAT ESP hint alone (<=256 MB) leaves the entire
                // ext2 root OUTSIDE the RAM window. Here we ONLY compute the FAT
                // ESP hint and the /TORAMOFF.TXT config, and carry them forward.
                if (fat_exists(&g_fat_fs, "/TORAMOFF.TXT")) {
                    kprintf("[MAIN] #417: /TORAMOFF.TXT present, TO-RAM disabled by config\n");
                    blk_toram_set_disabled(1);
                }
                uint64_t used_bytes_hint = 0;
                if (g_fat_fs.cluster_count > 0 && g_fat_fs.sectors_per_cluster > 0 &&
                    g_fat_fs.bytes_per_sector > 0) {
                    uint32_t free_clusters = g_fat_fs.free_cluster_count;
                    uint64_t used_clusters = (g_fat_fs.cluster_count > free_clusters)
                        ? (uint64_t)(g_fat_fs.cluster_count - free_clusters) : 0;
                    uint64_t cluster_bytes = (uint64_t)g_fat_fs.sectors_per_cluster * g_fat_fs.bytes_per_sector;
                    uint64_t meta_bytes = (uint64_t)(g_fat_fs.part_start_lba + g_fat_fs.data_start_lba) *
                                          g_fat_fs.bytes_per_sector;
                    used_bytes_hint = meta_bytes + used_clusters * cluster_bytes;
                    kprintf("[MAIN] #417: FAT used-data hint: %llu/%u clusters used (~%llu MB incl. metadata)\n",
                            (unsigned long long)used_clusters, g_fat_fs.cluster_count,
                            (unsigned long long)(used_bytes_hint / (1024 * 1024)));
                }
                // #539: DEFER the actual TO-RAM copy. Carry the FAT ESP hint to
                // the post-ext2-mount trigger below; do NOT call blk_root_to_ram
                // here (it would size the window off the FAT ESP alone and no-op
                // the second, correctly-sized call). Interrupts are still off and
                // the scheduler is not up between here and there, so the deferred
                // bulk USB fill runs in the exact same polling regime as before.
                fat_used_hint = used_bytes_hint;
                fb_swap_buffers();
            } else {
                kprintf("[MAIN] #307: USB MSC %d has no MayteraOS root, reverting to ATA\n", ui);
                blk_clear_root_usb();
                g_fat_fs.mounted = 0;
            }
        }
    }

    // #71/#702 real-hardware defensive hardening: audio hardware probing
    // (HDA controller reset, CORB/RIRB bring-up, codec/widget-graph parsing,
    // MSI arming) used to run synchronously right here, on the critical boot
    // path, long before proc_init()/login/the compositor. On real Cirrus/Intel
    // HDA silicon (e.g. the iMac14,4's CS4208, which reports PCI_INTERRUPT_LINE=0)
    // codec-verb round trips and controller bring-up can behave very
    // differently -- and much more slowly, or not at all -- than in QEMU, and
    // a synchronous call here means any such misbehavior directly delays or
    // blocks reaching the login screen and spawning the compositor. audio_init()
    // is now started from a deferred, low-priority background worker further
    // down (after proc_init()/interrupts/the scheduler are live), so it runs
    // CONCURRENTLY with login_run()/desktop_run() instead of gating them. See
    // audio_start_deferred_init() in drivers/audio.c for the full contract.
    gfx_boot_spinner(spinner_frame++); // Audio detect (now deferred)

    // Initialize ACPI subsystem
    gfx_boot_spinner(spinner_frame++); // ACPI init
    acpi_init();
    // #298: arm the ACPI power button so host "qm shutdown" (an ACPI power
    // button press / SCI) is caught and turned into an orderly flush+power-off
    // instead of timing out. IDT/PIC/ISR are already up at this point.
    { extern void acpi_enable_power_button(void); acpi_enable_power_button(); }

    // #279: bring up application processors (was written but never called).
    // lapic_init is now PIC-preserving so the BSP keeps its timer/keyboard.
    {
        extern int madt_init(void);
        extern int smp_init(void);
        extern int smp_start_aps(void);
        extern void smp_selftest(void);
        madt_init();   // parse MADT (CPU list) - was never called
        extern int g_smp_user_sched;
        if (smp_init() == 0 && g_smp_user_sched) {   // #279: only bring up APs when SMP scheduling is enabled
            smp_start_aps();
            // Give the AP a moment to finish lapic/gdt/idt/sse setup and reach
            // its work loop, then run the parallel self-test (proves the AP
            // executes real kernel work in parallel with the BSP).
            for (volatile unsigned long d = 0; d < 200000000UL; d++) { }
            smp_selftest();
        }
    }
    // #71: HDA's real MSI interrupt (needs the Local APIC, which just came up
    // inside smp_init() above) is now armed from the SAME deferred background
    // worker that runs audio_init(), right after that call completes -- not
    // here. See #702 comment above and audio_start_deferred_init() in
    // drivers/audio.c. Purely a matter of WHEN this runs; the LAPIC is already
    // initialized by the time the deferred worker executes, since that only
    // ever happens after this point in boot.

    syslog_log(LOG_INFO, "ACPI initialized");
    gfx_boot_spinner(spinner_frame++);

    // Initialize filesystem
    gfx_boot_spinner(spinner_frame++); // Filesystem init
    fat_init();

    // ext2 read-only driver self-test (runs once at boot, after ATA is up).
    // Probes the ext2 fs on channel 0, drive 1 (primary slave) and logs results.
    extern void ext2_selftest(void);
    ext2_selftest();

    // (#542) OS-wide system clipboard self-test: proves the kernel-held store
    // round-trips, size-queries, truncates and clears. One line PASS/FAIL.
    {
        extern long clip_selftest_rs(void);
        long cr = clip_selftest_rs();
        kprintf("[CLIP-SELFTEST] %s (mask=0x%lx)\n", cr == 0 ? "PASS" : "FAIL", cr);
    }

    // (#194) x86_16 interpreter 386-opcode self-test (one line PASS/FAIL at boot).
    extern int x86_16_selftest_386(void);
    x86_16_selftest_386();

    // Compute drive ID from channel/drive (0=Primary Master, 1=Primary Slave, etc.)
    int drive_id = (ata_channel << 1) | ata_drive;
    kprintf("[MAIN] ata_found=%d, drive_id=%d\n", ata_found, drive_id);

    // #307: USB root wins when present (fs_mounted_usb set above); the ATA/IDE
    // mount is only the fallback for VMs/boxes with a legacy IDE disk. This is
    // exactly the historical b665/b693 behavior; it keeps IDE VMs booting while
    // letting the real iMac boot from its USB-MSC stick.
    if (!fs_mounted_usb && ata_found == 0) {
        gfx_boot_spinner(spinner_frame++); // Mounting
        kprintf("[MAIN] Using drive ID %d for filesystem\n", drive_id);

        // Try GPT partition at LBA 2048 directly (EFI System Partition)
        kprintf("[MAIN] Trying GPT partition at LBA 2048...\n");
        if (fat_mount_lba(drive_id, 2048, &g_fat_fs) == 0) {
            // Filesystem mounted - continue spinner until boot image loads
            gfx_boot_spinner(spinner_frame++);
            // Log will be shown after boot image loads
            kprintf("[BOOT] Filesystem mounted (GPT)");
            kprintf("[MAIN] GPT mount success!\n");
            syslog_log(LOG_INFO, "FAT filesystem mounted (GPT)");
            fat_print_info(&g_fat_fs);

            // Initialize read-ahead cache for better disk performance
            if (fat_readahead_init(RA_DEFAULT_BUFFER_SIZE) == 0) {
                kprintf("[BOOT] FAT read-ahead cache initialized (256KB)\n");
            }

            // Try to load boot splash image from disk
            if (gfx_load_boot_image_from_disk() == 0) {
                gfx_boot_log("[BOOT] Boot image loaded from disk");
                gfx_boot_log_clear();
                gfx_boot_log("[BOOT] Filesystem mounted");
                // Redraw boot splash with the newly loaded image
                gfx_boot_splash();
                fb_swap_buffers();
                gfx_boot_progress(60);
            }
        } else {
            // Fall back to MBR partition 0
            kprintf("[MAIN] GPT mount failed, trying MBR partition 0...\n");
            if (fat_mount(drive_id, 0, &g_fat_fs) == 0) {
                // Filesystem mounted - switch to full boot splash
                gfx_boot_splash();
                fb_swap_buffers();
                gfx_boot_log_clear();
                gfx_boot_log("[BOOT] Filesystem mounted (MBR)");
                fat_print_info(&g_fat_fs);

            // Initialize read-ahead cache for better disk performance
            if (fat_readahead_init(RA_DEFAULT_BUFFER_SIZE) == 0) {
                kprintf("[BOOT] FAT read-ahead cache initialized (256KB)\n");
            }

                // Try to load boot splash image from disk
                if (gfx_load_boot_image_from_disk() == 0) {
                    gfx_boot_log("[BOOT] Boot image loaded from disk");
                    gfx_boot_splash();
                fb_swap_buffers();
                    gfx_boot_progress(60);
                }
            } else {
                gfx_boot_log("[BOOT] WARNING: Failed to mount filesystem");
                kprintf("[MAIN] Failed to mount any filesystem\n");
            }
        }
    } else if (!fs_mounted_usb) {
        gfx_boot_log("[BOOT] No storage devices found");
        kprintf("[MAIN] No ATA drives found (ata_found=%d), skipping filesystem mount\n", ata_found);
    }

    // #365: single-disk two-partition layout. If ext2 was not already mounted as a
    // whole-disk volume on the primary IDE slave (the two-disk dev layout, via
    // ext2_selftest above), look for an ext2 partition on the SAME disk the FAT ESP
    // booted from and mount it at its base LBA. This enables the proper single-disk
    // layout (GPT: FAT ESP p1 + ext2 p2). Works for a legacy IDE boot disk AND the
    // USB-MSC device (blk_read routes USB by whole-device LBA, ignoring channel/drive).
    {
        extern int ext2_is_mounted(void);
        extern int ext2_find_partition(uint8_t channel, uint8_t drive, uint32_t *out_base_lba);
        extern int ext2_mount(uint8_t channel, uint8_t drive, uint32_t part_start_lba);
        if (g_fat_fs.mounted && !ext2_is_mounted()) {
            uint8_t ec = fs_mounted_usb ? 0 : (uint8_t)ata_channel;
            uint8_t ed = fs_mounted_usb ? 0 : (uint8_t)ata_drive;
            uint32_t p2 = 0;
            if (ext2_find_partition(ec, ed, &p2) == 0 && p2 != 0 &&
                ext2_mount(ec, ed, p2) == 0) {
                kprintf("[MAIN] #365: ext2 mounted from partition at LBA %u (dev ch%d drv%d)\n",
                        (unsigned)p2, ec, ed);
                gfx_boot_log("[BOOT] ext2 root partition mounted");
            }
        }
    }

    // #539: DEFERRED USB TO-RAM trigger. Fixes a shipping perf regression on the
    // two-partition golden (small FAT ESP + big ext2 ROOT). TO-RAM was triggered
    // in the USB-mount block above with a hint sized off the FAT ESP alone
    // (<=256 MB), so the RAM window covered roughly LBA 0..600k. But the ext2
    // ROOT partition starts at LBA ~526336 and runs to ~1.8 GB, so the BULK of
    // userland (apps, fonts, icons) sat OUTSIDE the window and every read fell
    // through to slow LIVE USB. On the real iMac that makes the whole system
    // crawl; a VM's fast virtual USB hides it. The old single-partition FAT image
    // had its whole root inside one window, so it was fully RAM-resident.
    //
    // Fix: run the single TO-RAM call HERE, after the ext2 ROOT is located, and
    // size the window to span the WHOLE used image: max(FAT ESP hint, ext2
    // partition END = part_start_lba*512 + blocks_count*block_size). For a
    // single-FAT image (no ext2 mounted) this is exactly the old FAT-only hint,
    // so nothing regresses. blk_root_to_ram() no-ops for a non-USB (ATA VM)
    // root, and is guarded above to run exactly ONCE on the final hint.
    if (fs_mounted_usb) {
        extern uint64_t ext2_end_bytes(void);
        extern int ext2_is_mounted(void);
        uint64_t final_hint = fat_used_hint;
        if (ext2_is_mounted()) {
            uint64_t e2end = ext2_end_bytes();
            if (e2end > final_hint) final_hint = e2end;
            kprintf("[MAIN] #539: ext2 root ends at %llu MB; TO-RAM window hint = max(FAT %llu MB, ext2 %llu MB) = %llu MB\n",
                    (unsigned long long)(e2end / (1024 * 1024)),
                    (unsigned long long)(fat_used_hint / (1024 * 1024)),
                    (unsigned long long)(e2end / (1024 * 1024)),
                    (unsigned long long)(final_hint / (1024 * 1024)));
        } else {
            kprintf("[MAIN] #539: no ext2 root mounted; TO-RAM window hint = FAT %llu MB (single-partition image)\n",
                    (unsigned long long)(final_hint / (1024 * 1024)));
        }
        if (blk_root_to_ram(final_hint))
            kprintf("[MAIN] #375/#417/#539: root filesystem now served from RAM (TO-RAM)\n");
        else
            kprintf("[MAIN] #375/#417/#539: TO-RAM not used; demand cache / passthrough active\n");
        fb_swap_buffers();
    }

    // #99 Phase C: opt-in ext2-as-root. If the /ROOTEXT2 marker exists on the FAT
    // ESP AND the ext2 volume is mounted, switch the kernel's root reads to ext2
    // (FAT stays the UEFI ESP + read fallback). Reversible: remove the marker to
    // revert to pure FAT. g_root_ext2 is still 0 here, so this read is raw FAT.
    {
        extern int g_root_ext2;
        extern int ext2_is_mounted(void);
        if (g_fat_fs.mounted && ext2_is_mounted()) {
            uint32_t msz = 0;
            void *mk = fat_read_file(&g_fat_fs, "/ROOTEXT2", &msz);
            if (mk) {
                kfree(mk);
                g_root_ext2 = 1;
                kprintf("[MAIN] #99: ext2 is now the ROOT filesystem (/ROOTEXT2 present)\n");
                gfx_boot_log("[BOOT] root filesystem: ext2");
            }
        }
    }

    // #418 (restored b713): arm the crash logger now that the FAT root is
    // mounted. Without this panic_log_write() early-returns (armed==0) so
    // /PANIC.TXT and /STAGE.TXT are NEVER written on a fault. Its startup
    // call was silently dropped in the b681->b702 refactor churn.
    { extern void panic_log_init(fat_fs_t *fs);
      if (g_fat_fs.mounted) panic_log_init(&g_fat_fs); }

    // #433 (restored b714): flush the RAM-buffered boot/USB/audio diagnostic
    // logs to /BOOTLOG.TXT + /USBLOG.TXT + /AUDIOLOG.TXT now that the FAT root
    // is mounted. bootlog_arm()'s startup call was silently dropped in the
    // b681->b702 churn (only referenced in comments), leaving USB enumeration
    // undiagnosable over SSH. Same casualty class as panic_log_init / the
    // USB-MSC mount block. Pure diagnostic: no enumeration behaviour changes.
    { extern void bootlog_arm(fat_fs_t *fs);
      if (g_fat_fs.mounted) bootlog_arm(&g_fat_fs); }

    // #433/#373: opt-in USB HID input diagnostics. If /CONFIG/USBDEBUG.CFG is
    // present on the boot disk, turn on the per-report HID trace + per-transfer
    // xHCI event trace. This makes "does a real keypress produce an interrupt-IN
    // report on the keyboard's endpoint" answerable on REAL hardware (the
    // low-speed composite keyboard behind a high-speed TT hub that no QEMU VM
    // reproduces) without editing the source and rebuilding the kernel each
    // time. Default absent -> both stay OFF, so production boots are unaffected.
    { extern volatile int usb_hid_report_log; extern volatile int xhci_xfer_log;
      if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/CONFIG/USBDEBUG.CFG")) {
          usb_hid_report_log = 1;
          xhci_xfer_log = 1;
          kprintf("[USB] /CONFIG/USBDEBUG.CFG present: USB HID input diagnostics ON\n");
          bootlog_write("[USB] USBDEBUG.CFG present: HID report + xHCI xfer tracing enabled");
      } }


    // #308: verify userland directory open + readdir works on the ext2 root.
    { extern void ext2_dir_open_selftest(void); ext2_dir_open_selftest(); }

    // #383 (restored b713): RTL8812BU WiFi firmware upload, deferred from the
    // usb_init() probe until the root filesystem is mounted (so /FIRMWARE/
    // RTL8812B.BIN is readable). No-op if no RTL88x2BU adapter was bound.
    // Dropped in the b681->b702 refactor churn.
    { extern int rtl8812bu_late_init(void); rtl8812bu_late_init(); }



    // (#257) WINE-style drive-letter FS layer: create /WINDIR/DRIVE_{A,C,E} +
    // seed C:\WINDOWS now that the root FS (ext2 or FAT) is selected. DOS + Win16
    // file APIs map drive-letter paths here via dos_resolve_path.
    {
        extern void dos_windir_init(void);
        if (g_fat_fs.mounted) dos_windir_init();
    }
    gfx_boot_progress(70);

    // Initialize network stack
    // (boot-log completeness) The CPU, memory and storage/audio stages run
    // before the boot image clears the log, so summarize them here on the
    // splash now that the full boot screen is up. Network and later stages
    // log themselves at their own sites below.
    gfx_boot_log("[BOOT] CPU: GDT, IDT, PIC, PIT 250Hz, SSE, SYSCALL");
    gfx_boot_log("[BOOT] Memory: PMM, VMM, kernel heap");
    gfx_boot_log("[BOOT] PCI bus enumerated");
    gfx_boot_log("[BOOT] ATA/IDE storage initialized");
    gfx_boot_log("[BOOT] Audio device (HDA/AC97) initialized");
    gfx_boot_log("[BOOT] ACPI initialized");
    gfx_boot_log("[BOOT] Filesystems: FAT + ext2 drivers ready");
    gfx_boot_log("[BOOT] DOS / C:\\WINDOWS environment ready");

    // #404 / #479 Phase B: prove the Rust ip_checksum == the C ip_checksum on
    // THIS build before the network stack (which now computes/validates IP
    // checksums via ip_checksum_rs under -DRUST_IP_CHECKSUM) is brought up.
    // Bounded, runs once, logs one [RUST-DIFF] line to serial + /BOOTLOG.
    ip_checksum_rust_selftest();

    // #404 / #485 Phase C: prove the Rust ext2 directory-block parser
    // (ext2_dirblock_find_rs, live under -DRUST_EXT2_DIRFIND) == the #476-
    // hardened C over valid AND malformed blocks on THIS build, before any
    // ext2 path lookup runs. Bounded, once, one [RUST-DIFF] ext2_dir line.
    extern void ext2_dir_rust_selftest(void);
    ext2_dir_rust_selftest();

    // #404 / #486 Phase D: prove the Rust transport checksums == their C
    // references on THIS build before the network stack uses them. TCP is LIVE
    // under -DRUST_TCP_CHECKSUM (tcp_checksum routes to tcp_checksum_rs). UDP is
    // STAGED: udp_checksum_rs is proven here but not yet on the live send path
    // (udp_send still emits checksum 0, unchanged). Bounded, once, one
    // [RUST-DIFF] line each to serial + /BOOTLOG (#426, no busy-wait).
    extern void tcp_checksum_rust_selftest(void);
    tcp_checksum_rust_selftest();
    extern void udp_checksum_rust_selftest(void);
    udp_checksum_rust_selftest();

    // #404 / #487 Phase E: prove the Rust SHA-256 block compression core ==
    // the C reference on THIS build BEFORE any crypto (TLS/HMAC/CSPRNG) uses it.
    // Under -DRUST_SHA256 the live sha256() compresses in Rust; the self-test
    // runs the NIST known-answer vectors through the live sha256() API AND a
    // 20000-vector direct sha256_transform_rs vs sha256_transform_c differential.
    // Bounded, once, one [RUST-DIFF] sha256 line to serial + /BOOTLOG (#426).
    extern void sha256_rust_selftest(void);
    sha256_rust_selftest();

    // #404 / #488 Phase F: prove the Rust SHA-512 block compression core ==
    // the C reference on THIS build BEFORE any crypto (TLS 1.3 SHA-384 suite)
    // uses it. Under -DRUST_SHA512 the live sha512 path compresses in Rust; the
    // self-test runs the NIST known-answer vectors through the live sha512
    // init/update/final path, a 20000-vector direct sha512_transform_rs vs
    // sha512_transform_c differential, AND an RDTSC micro-benchmark of the two.
    // Bounded, once, one [RUST-DIFF] sha512 + one [RUST-PERF] sha512 line (#426).
    extern void sha512_rust_selftest(void);
    sha512_rust_selftest();

    // #404 / #489 Phase G: prove the Rust MD5 block compression core == the C
    // reference on THIS build BEFORE any consumer (HMAC-MD5 / NTLM in net/smb.c,
    // crypto/hmac.c) uses it. Under -DRUST_MD5 the live md5() path compresses in
    // Rust; the self-test runs the RFC 1321 known-answer vectors through the live
    // md5() API, a 20000-vector direct md5_transform_rs vs md5_transform_c
    // differential, AND an RDTSC micro-benchmark of the two. Bounded, once, one
    // [RUST-DIFF] md5 + one [RUST-PERF] md5 line to serial + /BOOTLOG (#426).
    extern void md5_rust_selftest(void);
    md5_rust_selftest();

    // #404 / #490 Phase H: prove the Rust MD4 block compression core == the C
    // reference on THIS build BEFORE any consumer (NTLM auth in net/smb.c) uses
    // it. Under -DRUST_MD4 the live md4() path compresses in Rust; the self-test
    // runs the RFC 1320 known-answer vectors through the live md4() API, a
    // 20000-vector direct md4_transform_rs vs md4_transform_c differential, AND
    // an RDTSC micro-benchmark of the two. Bounded, once, one [RUST-DIFF] md4 +
    // one [RUST-PERF] md4 line to serial + /BOOTLOG (#426).
    extern void md4_rust_selftest(void);
    md4_rust_selftest();

    // #404 / #491 Phase I: prove the Rust ChaCha20 block core == the C reference
    // on THIS build BEFORE any consumer (TLS ChaCha20-Poly1305) uses it. Under
    // -DRUST_CHACHA20 the live chacha20_block() generates the keystream in Rust;
    // the self-test runs the RFC 8439 section 2.3.2 known-answer keystream block
    // through the live chacha20_init/chacha20_block path, a 20000-vector direct
    // chacha20_block_rs vs chacha20_block_c differential, AND an RDTSC micro-
    // benchmark of the two. Bounded, once, one [RUST-DIFF] chacha20 + one
    // [RUST-PERF] chacha20 line to serial + /BOOTLOG (#426, no busy-wait).
    extern void chacha20_rust_selftest(void);
    chacha20_rust_selftest();

    // #404 / #492 Phase J: prove the Rust AES block cores == the C reference on
    // THIS build BEFORE any consumer (TLS AES-GCM, SSH aes-ctr) uses them. Under
    // -DRUST_AES the live aes_encrypt_block()/aes_decrypt_block() run in Rust; the
    // self-test runs the FIPS-197 AES-128 + AES-256 known-answer vectors through
    // the live AES API (encrypt + decrypt round-trip), a 20000-vector direct
    // aes_*_block_rs vs aes_*_block_c differential over random (state, round-keys,
    // Nr), AND an RDTSC micro-benchmark. Bounded, once, one [RUST-DIFF] aes + one
    // [RUST-PERF] aes line to serial + /BOOTLOG (#426, no busy-wait).
    extern void aes_rust_selftest(void);
    aes_rust_selftest();

    // #404 / #493 Phase K: prove the Rust HMAC construction == the C reference on
    // THIS build BEFORE any consumer (TLS 1.3 Finished key schedule, CSPRNG
    // HMAC-DRBG one-shot, NTLM/SMB HMAC-MD5) uses it. Under -DRUST_HMAC the live
    // hmac_sha256()/hmac_sha384()/hmac_md5() one-shots run the ipad/opad wrapper
    // in Rust (reaching the already-Rust hash cores via the C hash glue); the
    // self-test replays the RFC 4231 (SHA-256/384) + RFC 2202 (MD5) known-answer
    // vectors through the live API, runs a 21006-vector hmac_*_rs vs hmac_*_c
    // differential across all three variants, AND an RDTSC micro-benchmark of
    // HMAC-SHA256. Bounded, once, one [RUST-DIFF] hmac + one [RUST-PERF] hmac line
    // to serial + /BOOTLOG (#426, no busy-wait).
    extern void hmac_rust_selftest(void);
    hmac_rust_selftest();

    // #404/#494 Phase L: prove the Rust incoming-ICMP parse (icmp_parse_rs, live
    // under -DRUST_ICMP) == the verbatim C reference on the live agreement domain
    // (valid echo request/reply every length 8..1500 + too-short + bad-checksum)
    // BEFORE any ICMP echo is handled. First Tier-2 untrusted-wire parser: also
    // logs a [RUST-SEC] icmp line (oversize confinement the C never bounded) and
    // a [RUST-PERF] icmp RDTSC bench. Bounded, once, to serial + /BOOTLOG (#426).
    extern void icmp_rust_selftest(void);
    icmp_rust_selftest();

    // #404/#495 Phase M: prove the Rust incoming-ARP parse (arp_parse_rs, live
    // under -DRUST_ARP) == the verbatim C reference on the live agreement domain
    // (~512 vectors: well-formed Ethernet/IPv4 ARP request/reply + too-short + bad
    // hw/proto type + bad hlen/plen) BEFORE any ARP frame is handled. Second Tier-2
    // untrusted-wire parser: also logs a [RUST-SEC] arp line (the C is already
    // length-gated so there is no reachable OOB; C and Rust reject the malformed
    // corpus identically, and the Rust removes the pointer-arithmetic CLASS by
    // construction) and a [RUST-PERF] arp RDTSC bench. Bounded, once (#426).
    extern void arp_rust_selftest(void);
    arp_rust_selftest();

    // #404/#496 Phase N: prove the Rust incoming-DNS-response parse
    // (dns_parse_response_rs, live under -DRUST_DNS) == the verbatim C reference
    // on the live agreement domain (~512 vectors: well-formed A / CNAME+A /
    // AAAA-only / ancount=0 / rcode-error / not-a-response + malformed cases that
    // must TERMINATE: truncated header, truncated mid-answer, compression-pointer
    // loop, OOB pointer, oversized rdlength, label-past-end, random) BEFORE any
    // DNS response is handled. Third Tier-2 untrusted-wire parser: also logs a
    // [RUST-SEC] dns line (the C is already bounded - dns_skip_name cannot loop or
    // read past msglen and every field read is length-gated, so no reachable OOB;
    // the Rust removes the class by construction and pre-confines the pointer-
    // FOLLOW class a name-decode feature would make reachable) and a [RUST-PERF]
    // dns RDTSC bench. Bounded, once, to serial + /BOOTLOG (#426, no busy-wait).
    extern void dns_rust_selftest(void);
    dns_rust_selftest();

    // #404/#497 Phase O: prove the Rust incoming-DHCP reply parse (dhcp_parse_rs,
    // live under -DRUST_DHCP) == the verbatim C reference on the live agreement
    // domain (~512 vectors: well-formed OFFER/ACK/NAK, END-terminated, with/without
    // each option + PAD + unknown options + too-short + bad-op + bad-magic) BEFORE
    // any DHCP reply is handled. FOURTH Tier-2 untrusted-wire parser and the FIRST
    // with a genuinely REACHABLE over-read in its C reference: the C option walk
    // runs over the fixed 308-byte options[] IGNORING len, so a crafted runt OFFER
    // reads past the received bytes. The [RUST-SEC] dhcp line counts how many
    // crafted OFFERs the C over-reads (extracting attacker-adjacent bytes as
    // config) where the Rust, confined to len, does not. Also a [RUST-PERF] dhcp
    // RDTSC bench. Bounded, once, to serial + /BOOTLOG (#426, no busy-wait).
    extern void dhcp_rust_selftest(void);
    dhcp_rust_selftest();

    // #404/#498 Phase P: prove the Rust URL-string parse (url_parse_rs, live under
    // -DRUST_URL) == the verbatim C reference on ~512 agreement vectors (curated
    // valid http/https/ftp/ws/wss/file/mailto/tel + IPv6 literals + reject cases:
    // empty, ws-only, over-long fields, huge port, unterminated IPv6 + random http
    // + structural fuzz). FIFTH Tier-2 untrusted-input parser; the untrusted input
    // is the URL string from redirect Location headers + the address bar. HONEST
    // security: the C already CAPS every out-field copy (no reachable overflow),
    // so [RUST-SEC] url reports over-long fields rejected identically by both;
    // the Rust adds the C's missing input-scan upper bound (URL_MAX_INPUT=8192).
    // Also a [RUST-PERF] url RDTSC bench. Bounded, once, to serial + /BOOTLOG.
    extern void url_rust_selftest(void);
    url_rust_selftest();

    // #404/#499 Phase Q: prove the Rust ELF64 header + program-header validation
    // (elf_validate_full_rs, live under -DRUST_ELF) == the verbatim C reference on
    // ~256 agreement vectors (well-formed synthetic ELF images + shared ehdr/phdr-
    // table reject mutations, identical shape to every /apps binary). SIXTH Tier-2
    // untrusted-input parser and the STRONGEST security win: the untrusted input is
    // an on-disk/loaded ELF, and the verbatim C has two ASan-proven REACHABLE OOBs
    // (an oversized p_filesz bypasses the underflowing check_overflow_add -> OOB
    // read+write in the segment memcpy; an undersized e_phentsize over-reads in
    // elf_get_phdr) plus a latent p_memsz<p_filesz write-overflow, ALL confined by
    // the Rust by construction. [RUST-SEC] elf reports each; [RUST-PERF] elf an
    // RDTSC bench. A full desktop = every real ELF (compositor + apps) accepted by
    // elf_validate_full_rs. Bounded, once, to serial + /BOOTLOG.
    extern void elf_rust_selftest(void);
    elf_rust_selftest();

    // #404 / Phase R: prove the Rust PE32 pre-map validation (pe_validate_full_rs,
    // live under -DRUST_PE) == the verbatim pe_validate_full_c on ~256 agreement
    // vectors (well-formed PE32 + shared DOS/PE/COFF/section reject mutations),
    // log a [RUST-SEC] pe line (three LATENT OOB classes the C has vs the Rust
    // confinement; pe_load is unwired - Win32 #288) and a [RUST-PERF] pe RDTSC
    // bench. Bounded, once (#426).
    extern void pe_rust_selftest(void);
    pe_rust_selftest();

    // #404 Phase S: prove fat_dir_step_rs (live under -DRUST_FAT) == the verbatim
    // fat_dir_step_c on well-formed VFAT directories (LFN sets + 8.3 + 0xE5 +
    // orphan LFN + bad seq), log [RUST-SEC] fat (the reachable over-long-LFN-name
    // overflow the C has vs the Rust confinement) and a [RUST-PERF] fat RDTSC
    // walk. Bounded, once (#426). fat_readdir_inner runs the Rust parse live.
    extern void fat_rust_selftest(void);
    fat_rust_selftest();

    // #404 Phase T: prove exfat_dir_step_rs (live under -DRUST_EXFAT) == the
    // verbatim exfat_dir_step_c on well-formed exFAT File entry sets
    // (File(0x85)+Stream(0xC0)+Name(0xC1)), log [RUST-SEC] exfat (the crafted
    // NameLength stale-stack info-leak the C has vs the Rust confinement; the
    // exFAT readdir path is REACHABLE via USB hot-plug but LATENT - no live
    // caller lists an exFAT dir today) and a [RUST-PERF] exfat RDTSC walk.
    // Bounded, once (#426). exfat_readdir runs the Rust parse live when reached.
    extern void exfat_rust_selftest(void);
    exfat_rust_selftest();

    // #404 Phase U: prove bmp_decode_rs (live under -DRUST_BMP) ==
    // bmp_decode_c on well-formed 24/32bpp BMPs; log [RUST-SEC] bmp (the
    // C uint32 size-arith wrap it accepts at parse vs the Rust u64
    // confinement - LATENT/defense-in-depth, the live C fails safe at the
    // 64-bit kmalloc) and [RUST-PERF] bmp. Bounded, once (#426).
    // image_load_bmp runs the Rust decode LIVE (wallpaper + BOOT BMP).
    extern void bmp_rust_selftest(void);
    bmp_rust_selftest();

    // #404 Phase V: prove png_parse_ihdr_rs + png_defilter_rs (live under
    // -DRUST_PNG) == the C references on well-formed IHDRs + defilter inputs;
    // log [RUST-SEC] png (the REACHABLE integer-overflow OOB class: a crafted
    // IHDR wraps the C's uint32 scanline_len/raw_size and the downstream decode
    // loops OOB read+WRITE past the wrapped-small allocations - the Rust confines
    // it at parse) and [RUST-PERF] png. Bounded, once (#426). image_load_png runs
    // the Rust IHDR-parse + defilter LIVE for every PNG asset (icons, previews).
    extern void png_rust_selftest(void);
    png_rust_selftest();

    // #404 Phase W: prove jpeg_parse_headers_rs (live under -DRUST_JPEG) ==
    // jpeg_parse_headers_c (verbatim reference) on a real baseline JPEG header +
    // its truncations; log [RUST-SEC] jpeg (THREE REACHABLE OOBs the Rust
    // confines: SOF0 comp_qt / SOS comp_dc-ac unvalidated -> downstream quant[]/
    // huff_fast[] OOB READ, and DHT sum-of-counts>256 -> huff_vals[256] OOB
    // WRITE) and [RUST-PERF] jpeg. Bounded, once (#426). image_load_jpeg parses
    // every JPEG's headers through the Rust seam LIVE (album art, previews, img).
    extern void jpeg_rust_selftest(void);
    jpeg_rust_selftest();

    // #404 Phase X: prove inflate_rs (live under -DRUST_INFLATE) == inflate_c
    // (verbatim reference) on embedded RAW-DEFLATE vectors (fixed/dynamic/stored
    // blocks + overlapping back-references) at exact + mutated caps + 4 crafted
    // hostile back-reference attacks; log [RUST-SEC] inflate (the classic LZ77
    // OOB surface: the C back-ref copy ALREADY bounds dist>out_pos and out_pos+
    // len>dst_cap, so no reachable OOB - ASan 3M+ hostile clean OFFLINE; LATENT/
    // defense-in-depth, the Rust confines by construction) and [RUST-PERF]
    // inflate. Bounded, once (#426). image_load_png decompresses every PNG's IDAT
    // stream through the Rust inflate_rs LIVE (icons, previews, downloaded PNGs).
    extern void inflate_rust_selftest(void);
    inflate_rust_selftest();

    // #404 / #502 Phase Y: prove the Rust TLS length-parse framing seam
    // (tls_parse_record_header_rs / tls_hs_next_rs / tls_cert_next_rs /
    // tls13_cert_next_rs, all LIVE under -DRUST_TLS_PARSE) == their C references
    // on THIS build over well-formed AND malformed record headers, coalesced
    // handshake messages, and certificate lists, BEFORE any HTTPS handshake
    // parses attacker-influenced length fields off the wire. Also confirms the
    // seam refuses the exact crafted input that over-read the ORIGINAL inline
    // TLS 1.2 handshake loop (remote-reachable; offline ASan-proven; #503).
    // Bounded, once (#426). One [RUST-DIFF]/[RUST-PERF]/[RUST-SEC] tls_parse line.
    extern void tls_parse_rust_selftest(void);
    tls_parse_rust_selftest();

    // #502: prove the TLS 1.2 key schedule on THIS build before any 1.2
    // handshake derives a real key. Known-answer vectors whose answers come from
    // INDEPENDENT implementations captured offline (scapy TLS PRF for
    // PRF/EMS/key-block/Finished; a real openssl PSS signature), never from our
    // own code: an equivalence test against ourselves cannot catch a fault we
    // share with ourselves (exactly how [RUST-DIFF] tls_parse stayed green while
    // asserting a bug). Includes negative controls: a FORGED PSS signature must
    // be REJECTED and the RFC 8446 4.1.3 downgrade sentinel must be DETECTED.
    // Bounded, once (#426). One [TLS1.2-SELFTEST] line.
    extern void tls12_selftest(void);
    tls12_selftest();

    // #404 / #504 Phase Y: prove the Rust HTTP response length-parse seam
    // (https_dechunk_rs / https_chunk_complete_rs / http_decode_chunked_rs /
    // http_find_header_end_rs / https_content_length_rs / http_parse_uint_rs, all
    // LIVE under -DRUST_HTTP_PARSE) == their verbatim C references on well-formed
    // HTTP responses (chunked bodies, incremental gate prefixes, header framing,
    // Content-Length), and witness the REACHABLE https_dechunk u32-overflow OOB
    // (MAYTERA-SEC-2026-0008) being confined by the Rust seam. Runs before any
    // browser / Kimi API / update / widget fetch parses an attacker-influenced
    // chunk size. Bounded, once (#426). One [RUST-DIFF]/[RUST-PERF]/[RUST-SEC].
    extern void http_parse_rust_selftest(void);
    http_parse_rust_selftest();

    // #404 / #505 Phase Z: prove mp4_parse_rs (LIVE under -DRUST_MP4) ==
    // mp4_parse_c (verbatim reference) on well-formed .m4a atom trees (mixed
    // stco/co64, uniform + per-sample stsz) and reject cases, and witness the
    // REACHABLE ISO-BMFF sample-table over-read (MAYTERA-SEC-2026-0009: a crafted
    // .m4a whose stsz/stco/stsc declared counts exceed their tables drives the C
    // chunk/sample walk past the kmalloc'd file buffer -> heap OOB READ) being
    // confined by the Rust seam. Reachable from Ring-3: SYS_PLAY_WAV -> sys_play_wav
    // -> audio_play_file -> fat_read_file -> audio_decode_open -> aac_create ->
    // mp4_parse. Bounded, once (#426). One [RUST-DIFF]/[RUST-PERF]/[RUST-SEC] mp4.
    extern void mp4_rust_selftest(void);
    mp4_rust_selftest();

    // #404 batch-1 seam 1/3: prove jpeg_dequant_idct_rs (LIVE under
    // -DRUST_JPEG_ENTROPY) == jpeg_dequant_idct_c on 2000 realistic sparse-DCT
    // blocks with the real luma quant table (0 mism) + an overflow-block parity
    // vector; the seam has NO reachable memory OOB (defense-in-depth) but the
    // Rust wrapping_* WELL-DEFINES a REACHABLE CWE-190 signed-overflow UB in the
    // C IDCT (#507 plain-C hardening). Bounded, once (#426). decode_block routes
    // every JPEG's dequant+IDCT through the Rust seam LIVE (album art, previews,
    // browser <img>). One [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] jpeg_idct line.
    extern void jpeg_entropy_rust_selftest(void);
    jpeg_entropy_rust_selftest();

    // #404 batch-1 seam 2/3 / MAYTERA-SEC-2026-0010: prove http2_frame_next_rs
    // (LIVE under -DRUST_HTTP2_FRAME) == http2_frame_next_c on representative
    // well-formed frames, and that BOTH REJECT the zero-length PADDED frame that
    // the old inline http2_get framing NULL-dereferenced (payload[0], payload==
    // NULL when flen==0) -> remote pre-auth whole-OS DoS. The seam now decides
    // each frame's extent once, safely. Bounded, once (#426). Runs before any
    // h2 HTTPS fetch. One [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] http2_frame line.
    extern void http2_frame_selftest(void);
    http2_frame_selftest();

    // #404 batch-1 seam 3/3: prove theme_parse_line_rs (LIVE under
    // -DRUST_THEME_PARSE) == theme_parse_line_c on KAT vectors incl. the
    // embedded-NUL divergence found + fixed offline. Themes are downloadable/
    // editable user content (#141), so the tokenizer input is untrusted; the C
    // is already fully bounded (defense-in-depth port). Bounded, once (#426).
    // theme_parse_ini routes every theme file's line parse through the Rust seam
    // LIVE. One [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] theme_parse line.
    extern void theme_parse_rust_selftest(void);
    theme_parse_rust_selftest();

    // #404 batch-2 seam 1/3: prove wav_parse_header_rs (LIVE under
    // -DRUST_WAV_PARSE) == wav_parse_header_c on well-formed AND malformed
    // RIFF/WAVE headers (field-by-field WavInfo compare), before any WAV decode.
    // Latent/defense-in-depth: the C walk is already bounded. One [RUST-DIFF] +
    // [RUST-SEC] wav line. wav_create parses every .wav header via the seam LIVE.
    extern void wav_rust_selftest(void);
    wav_rust_selftest();

    // #404 batch-2 seam 2/3: prove cert_base64_decode_rs (LIVE under
    // -DRUST_CERT_B64) == base64_decode_c and round-trips a synthetic DER,
    // before any TLS certificate is parsed. Latent/defense-in-depth: the C is
    // already output-bounded; the Rust also removes a benign signed-shift UB.
    // One [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] cert_b64 line. cert_parse_pem
    // decodes every PEM CERTIFICATE body via the seam LIVE.
    extern void cert_b64_selftest(void);
    cert_b64_selftest();

    // #404 batch-2 seam 3/3 / MAYTERA-SEC-2026-0011: prove xattr_entry_next_rs
    // (LIVE under -DRUST_XATTR) == xattr_entry_next_c on a well-formed block, and
    // witness the confinement of the REACHABLE on-disk xattr entry-walk heap
    // over-read (CWE-125): a crafted /.xat block's name_len/value_len drives the
    // C get/list walk past the kmalloc'd buffer (sys_getxattr/listxattr); the
    // Rust rejects before dereferencing. One [RUST-DIFF]/[RUST-SEC] xattr_entry
    // line. xattr_get/xattr_list walk every /.xattr block via the seam LIVE.
    extern void xattr_entry_selftest(void);
    xattr_entry_selftest();

    // #404 batch-3 (LAST parser-tier batch): prove the 2 final live Rust seams ==
    // their verbatim C references at boot. (1) unpack25519_rs (LIVE under
    // -DRUST_ED25519_DECODE): ed25519 point-decode rs==c (defense-in-depth). (2)
    // xdr_decode_*_rs (LIVE under -DRUST_XDR): a real NFS reply decode rs==c
    // (defense-in-depth; the SEPARATE net/nfs.c destination over-write
    // MAYTERA-SEC-2026-0012 is fixed directly in nfs.c, not this seam). A third
    // seam (SSH framing) was dropped: its target net/ssh/ssh_transport.c is dead
    // scaffolding and the live ssh2.c already rejects the underflow (no advisory).
    // Bounded, once, one [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] line each (#426).
    extern void ed25519_decode_selftest(void);
    ed25519_decode_selftest();
    extern void xdr_rust_selftest(void);
    xdr_rust_selftest();

    // #404 / #487 / #349 taskmgr_core (Task Manager data seam): one [RUST-DIFF]
    // + one [RUST-SEC] taskmgr_core line to serial + /BOOTLOG. Proves
    // perf_ring_stats_rs + taskmgr_sort_rows_rs (LIVE under -DRUST_TASKMGR_CORE)
    // == the in-file C twins on well-formed vectors, and confines the naive C
    // sort's count>cap OOB-write (CWE-787) + the ring's cap==0 div-by-zero.
    // Defense-in-depth (new data core, no shipping C consumer feeds those).
    // Bounded, runs once (#426). Uses lfence;rdtsc (NOT rdtscp, #UDs on kvm64).
    extern void taskmgr_core_selftest(void);
    taskmgr_core_selftest();

    // #487/#349 Task Manager kernel accessor tier: per-process memory
    // accounting differential. Proves proc_mem_account_rs (LIVE under
    // -DRUST_PROC_MEM, and now the source of proc_snapshot's mem_kb) ==
    // proc_mem_account_c over a corpus built to REACH the states a naive
    // implementation gets wrong (brk below brk_start, kernel procs, inverted
    // VMA extents, a CYCLIC vma_list, resident-page saturation), with a
    // coverage line so a corpus regression shows as a coverage collapse rather
    // than a silent PASS. Also emits [RUST-SEC] witnessing that the bounded
    // walk RETURNS on a cyclic list where the pre-existing unbounded C walkers
    // would spin forever in Ring 0. Bounded, runs once (#426).
    extern void proc_mem_selftest(void);
    proc_mem_selftest();

    // #487/#349: open-handle path recording (file_t.path). Proves
    // vfs_path_store_rs (LIVE under -DRUST_VFS_PATH, and the store used by
    // every fat_vfs_open/ext2_vfs_open/dev_open) == vfs_path_store_c over
    // truncation, exact-fit, off-by-one and NON-NUL-TERMINATED sources, with an
    // 8-byte canary past every destination proving neither writes past `cap`.
    extern void vfs_path_selftest(void);
    vfs_path_selftest();

    // #487/#349: per-process network attribution (tcp_conn_t.owner_pid).
    extern void conn_owner_selftest(void);
    conn_owner_selftest();

    // #487/#349: the Ring-3 introspection builders behind SYS_PROC_HANDLES /
    // SYS_SVC_LIST. Proves handles_build_rs / svc_build_rs (LIVE under
    // -DRUST_PROCINFO) == their C twins over NULL, UNTERMINATED and
    // over-long source strings and over more rows than the destination can
    // hold, with a canary row past cap.
    extern void procinfo_selftest(void);
    procinfo_selftest();

    // #404 driver/block tier: partition-table (MBR/GPT) parse differential.
    // Proves parttbl_gpt_hdr_rs / parttbl_gpt_sec_scan_rs / parttbl_mbr_find_rs
    // (LIVE under -DRUST_PARTTBL) == the fs/ext2.c C twins over well-formed GPT
    // across every legal SizeOfPartitionEntry and entry slot, malformed headers,
    // and random fuzz. ext2_find_partition() (#365 root discovery) parses an
    // UNTRUSTED partition table off whatever disk or USB stick is inserted, on
    // EVERY boot; the iMac target boots off USB. The C is genuinely bounded, so
    // this is defense-in-depth: its safety rests entirely on ONE guard (esz>=128
    // with per_sec = 512/esz), and the Rust removes that dependency by
    // construction (slice bounds + checked e*esz). Bounded, once (#426).
    // One [RUST-DIFF] + one [RUST-PERF] parttbl line.
    extern void parttbl_rust_selftest(void);
    parttbl_rust_selftest();

    // #404 driver tier: USB Audio Class config-descriptor parse differential.
    // Proves uac_parse_config_rs (LIVE under -DRUST_USB_DESC) == the
    // drivers/usb_audio.c C twin on well-formed descriptors. The descriptor is
    // UNTRUSTED (whatever the attached USB device returns). The C reads
    // cfg[i+2/+3/+6] guarded only by blen>=2, so a crafted lying 2-byte
    // INTERFACE/ENDPOINT over-reads up to 5 bytes past len; that IS reachable
    // (total clamps to 512 = sizeof(cfg) in xhci.c) but lands in adjacent .bss
    // with negligible impact, hence defense-in-depth and NO advisory. Bounded,
    // once (#426). One [RUST-DIFF] + one [RUST-SEC] usb_desc line.
    extern void usb_desc_rust_selftest(void);
    usb_desc_rust_selftest();

    gfx_boot_log("[BOOT] Initializing network stack...");
    if (net_init() == 0) {
        gfx_boot_log("[BOOT] Network initialized");
        syslog_log(LOG_INFO, "Network stack initialized");

        // net_init() already applied a file-provided static config if present
        // (net_apply_static_config in net/net.c reads /NETCFG.TXT then
        // /CONFIG/NETIP.CFG). When NO such file exists, DEFAULT TO DHCP: leave the
        // address unset (ip == 0) so the background net worker runs DORA, with its
        // own carrier-gated DAD-verified .200-.209 static fallback if no lease
        // arrives (~12s). This is the default for test VMs and any image without a
        // static netcfg file; the iMac USB carries /NETCFG.TXT to pin its
        // ICS-segment static (<ICS_HOST>) instead. Previously this branch
        // hard-set a static .200, which suppressed DHCP entirely.
        extern int g_net_static_configured;
        if (!g_net_static_configured) {
            gfx_boot_log("[BOOT] No static netcfg; using DHCP");
            kprintf("[NET] No static config file; deferring to DHCP (net worker DORA)\n");
        } else {
            gfx_boot_log("[BOOT] Static IP from netcfg file");
        }

        // Test outbound ping from MayteraOS to gateway (SKIPPED for fast boot)
        if (0) { kprintf("[NET] *** OUTBOUND PING TEST ***\n");
        gfx_boot_log("[BOOT] Testing outbound ping...");
        extern int icmp_ping(uint32_t dest_ip);
        extern void net_poll(void);
        extern int icmp_get_ping_reply(uint32_t *src_ip, uint16_t *seq, uint16_t *time_ms);

        int sent = 0, received = 0;
        for (int attempt = 0; attempt < 4; attempt++) {
            kprintf("[NET] Ping #%d to 192.0.2.1...\n", attempt + 1);

            // Try to send (may fail first time due to ARP)
            int result = icmp_ping(0xC0A80101);
            if (result < 0) {
                kprintf("[NET] Send failed (ARP), waiting...\n");
                for (int i = 0; i < 50; i++) {
                    net_poll();
                    for (int j = 0; j < 5000; j++) io_wait();
                }
                result = icmp_ping(0xC0A80101);
            }

            if (result >= 0) {
                sent++;
                kprintf("[NET] Ping sent, waiting for reply...\n");

                // Wait for reply
                int got_reply = 0;
                for (int i = 0; i < 200 && !got_reply; i++) {
                    net_poll();
                    for (int j = 0; j < 2000; j++) io_wait();

                    uint32_t reply_ip;
                    uint16_t seq, time_ms;
                    if (icmp_get_ping_reply(&reply_ip, &seq, &time_ms)) {
                        uint8_t *rp = (uint8_t *)&reply_ip;
                        kprintf("[NET] REPLY from %d.%d.%d.%d: seq=%d time=%dms\n",
                                rp[3], rp[2], rp[1], rp[0], seq, time_ms);
                        received++;
                        got_reply = 1;
                    }
                }
                if (!got_reply) {
                    kprintf("[NET] Request timed out\n");
                }
            }
        }
        kprintf("[NET] *** PING RESULT: %d sent, %d received ***\n", sent, received);
        if (received > 0) {
            gfx_boot_log("[BOOT] Outbound ping SUCCESS!");
        } else {
            gfx_boot_log("[BOOT] Outbound ping FAILED");
        }
        } // end if(0) ping skip

    } else {
        gfx_boot_log("[BOOT] Network initialization failed");
    }
    gfx_boot_progress(85);

    // Initialize mouse driver
    gfx_boot_log("[BOOT] Initializing input devices...");
    mouse_init();

    // Initialize GUI subsystem
    gfx_boot_log("[BOOT] Initializing window manager...");
    wm_init();
    desktop_init();
    kprintf("[GUI] Window manager and desktop initialized\n");

    // Initialize multi-user subsystems
    perms_init();
    gfx_boot_log("[BOOT] Permissions database initialized");
    kprintf("[KERNEL] Permissions database initialized\n");
    users_init();
    gfx_boot_log("[BOOT] User database initialized");
    kprintf("[KERNEL] User database initialized\n");

    gfx_boot_progress(95);

    // Initialize in-kernel /dev namespace (Phase A2). Must run before
    // proc_init so that the idle process -- and every process spawned
    // afterward -- can pre-open /dev/console on fds 0/1/2.
    extern void dev_init(void);
    dev_init();
    gfx_boot_log("[BOOT] Device layer (/dev) initialized");
    kprintf("[KERNEL] /dev namespace initialized\n");

    // Initialize PTY subsystem (must be after dev_init)
    extern void pty_init(void);
    pty_init();
    gfx_boot_log("[BOOT] PTY subsystem initialized");
    kprintf("[KERNEL] PTY subsystem initialized\n");

    // Initialize process management
    gfx_boot_log("[BOOT] Initializing process manager...");
    proc_init();
    kprintf("[KERNEL] Process subsystem initialized\n");
    // #430 (restored b713): init the futex layer now that the process table
    // exists (enables userland pthread mutex/cond/join). Dropped in the churn.
    { extern void futex_init(void); futex_init(); }

    // #703 (#71 iMac): (re)start the USB HID input poll worker now that
    // proc_init() has run. On the real iMac the keyboard/mouse are always
    // present, so xhci.c enumerates them during usb_init() ABOVE (well before
    // proc_init) and calls usb_hid_start_poll_thread() there -- which proc_
    // create()s the worker AND sets its one-shot started-guard. proc_init()
    // then memset()s the whole proc_table, erasing that early worker, while the
    // usb_hid.c guard stays set, so the worker can never be recreated: HID is
    // never polled and the kernel user-select/login screen never receives a
    // keypress (the userland compositor never starts). This mirrors the
    // #699/#702 hda_start_poll_worker_deferred() ordering fix. Idempotent:
    // usb_hid_reset_poll_thread_guard() clears the stale flag exactly once so
    // the now-wiped early worker is recreated in the correct post-proc_init
    // order; usb_hid_start_poll_thread() itself still no-ops if a worker is
    // already running or no HID enumerated. Safe against double-spawn: the
    // scheduler is not started until later in this function, so no worker is
    // actually executing yet, and the single worker polls every hid_devices[].
    {
        extern void usb_hid_reset_poll_thread_guard(void);
        extern void usb_hid_start_poll_thread(void);
        usb_hid_reset_poll_thread_guard();
        usb_hid_start_poll_thread();
    }

    // #433 (restored b713): start the periodic xHCI port re-scan worker. It
    // retries ports that lost the bounded boot enumeration race (real-HW USB
    // HID enumeration is timing dependent, e.g. a Low-Speed keyboard behind a
    // hub) and enumerates devices hot-plugged after boot, so a keyboard that
    // failed to come up at boot is recovered without a cold power cycle. Runs
    // on its own thread that sleeps between scans (never busy-polls). Its one
    // startup call was silently dropped in the b681->b702 refactor churn.
    {
        extern void xhci_start_rescan_thread(void);
        xhci_start_rescan_thread();
    }

    // #699/#702: the HDA LPIB-poll worker is started from audio_init_worker()
    // (drivers/audio.c), right after audio_init() itself completes, since
    // that is now also deferred to a background worker (see #702 comment at
    // the audio_start_deferred_init() call site below) -- audio_init() has
    // not run yet at this point in boot, so hda_state.initialized would
    // always read false here. Calling hda_start_poll_worker_deferred() from
    // inside audio_init_worker() keeps the #699 ordering guarantee (poll
    // worker only ever starts after proc_init()) while also keeping it
    // correctly sequenced after audio_init() actually ran.

    // Initialize IPC subsystem (must be before proc_init)
    extern void ipc_init(void);
    ipc_init();
    gfx_boot_log("[BOOT] IPC subsystem initialized");
    kprintf("[KERNEL] IPC subsystem initialized\n");

    // Enable interrupts
    gfx_boot_log("[BOOT] Enabling interrupts...");
    kprintf("\n[KERNEL] Enabling interrupts...\n");
    sti();

    // Enable preemptive multitasking
    sched_set_preemption(true);
   // Start TCP remote control service (port 2323)
    remote_ctrl_init();
    { extern void win16_autolaunch_init(void); win16_autolaunch_init(); }
    gfx_boot_log("[BOOT] Remote control service started on port 2323");
    // Start the SSH server (port 22) if a host key is present (/CONFIG/SSHHOST.KEY).
    { extern void ssh_server_start(void); ssh_server_start(); }
    gfx_boot_log("[BOOT] SSH server starting on port 22");
    gfx_boot_log("[BOOT] Preemptive multitasking enabled");
    kprintf("[KERNEL] Preemptive multitasking enabled\n");

    // task #317: kick off the SMB network-filesystem self-test in a deferred
    // kernel worker (runs ~12s after boot, when the net stack is fully up).
    // No-op unless /CONFIG/SMBTEST.CFG is present.
    {
        extern void smb_start_deferred_selftest(void);
        smb_start_deferred_selftest();
    }

    // #333/#497: the network (HTTPS/h2/TLS) boot self-test. It is gated on
    // /CONFIG/NETTEST.CFG and is a no-op without it.
    //
    // This call site DID NOT EXIST until b831: net/https.c has defined
    // nettest_start_deferred_selftest() since #333 but NOTHING ever called it,
    // so the h2 regression self-test that #333 was closed against has never run
    // once in a shipped kernel. Same class as the #433 / #381 net_start_worker
    // drop noted below: a worker that is written, reviewed and dead. If you are
    // here because a self-test "passes", first confirm its start call still
    // exists - absence of output from a self-test is NOT evidence of health.
    {
        extern void nettest_start_deferred_selftest(void);
        nettest_start_deferred_selftest();
    }

    // #323: USB audio boot self-test. If a USB DAC enumerated, a deferred worker
    // plays a 440 Hz tone then a short arpeggio clip so the user can hear it.
    {
        extern void uac_start_boot_selftest(void);
        uac_start_boot_selftest();
    }

    // #381 (restored b713): background net worker. Owns USB carrier polling
    // (the slow, cable-less stalling PHY read - kept OFF the compositor/
    // net_poll path) and async DHCP + RFC 5227 static fallback, so a dongle
    // with no cable never freezes the UI and boot never blocks on the network.
    // No-op-cheap when there is no carrier. Its startup call was silently
    // dropped in the b681->b702 refactor churn (same class as #433 above).
    {
        extern void net_start_worker(void);
        net_start_worker();
    }

    // task #306 (restored b713): deferred OS install-to-disk worker. No-op
    // unless /CONFIG/AUTOINST.CFG is present. Dropped in the churn.
    {
        extern void installer_start_deferred_autoinstall(void);
        installer_start_deferred_autoinstall();
    }

    // #373 heartbeat (restored b713): start the alive-vs-hung heartbeat thread
    // (PRIO_NORMAL so it is actually scheduled). It appends a one-line "[HB]"
    // record to /HEARTBEAT.TXT ~every 2s; if those keep advancing after the
    // desktop appears the OS is alive, if they stop the scheduler wedged. Both
    // its definition and this call were dropped in the b681->b702 churn.
    {
        int hbpid = proc_create("heartbeat", heartbeat_worker, NULL, PRIO_NORMAL);
        kprintf("[HB] heartbeat thread created, pid=%d\n", hbpid);
    }

    // #372 (restored b713): start the Bluetooth worker thread. Gated behind
    // g_bt_enable (default 0); nothing here touches the radio, so it can never
    // affect boot. The worker makes the enable decision post-boot. Dropped in
    // the churn.
    {
        extern void bt_start_worker(void);
        bt_start_worker();
    }

    // #383 phase 3 (restored b713): spawn the RTL8812BU WiFi passive scan
    // worker (RF was powered up in rtl8812bu_late_init; the worker sweeps
    // 2.4GHz for beacons). Dropped in the churn.
    {
        extern void rtl8812bu_run_scan(void);
        rtl8812bu_run_scan();
    }

    // #201/#276 (restored b713): boot-gated DOS game launcher. No-op unless
    // /CONFIG/DOSRUN.CFG is present (contains a single path launched a few
    // seconds after boot). Dropped in the churn.
    {
        extern void dos_start_deferred_launch(void);
        dos_start_deferred_launch();
    }

    // #265 (restored b713): cron-like timer/scheduler. Build the registry
    // (loads /CONFIG/CRON.CFG) and start the worker kernel process that fires
    // due jobs. The whole cron boot block was silently dropped in the b681->
    // b702 refactor churn (same class as #433/#381); restored here after
    // preemption is enabled so the worker's blocking actions (filesystem I/O)
    // are safe. (Gated CRONTEST self-test intentionally not restored.)
    {
        extern void cron_init(void);
        extern void cron_start_worker(void);
        cron_init();
        cron_start_worker();
    }

    // #702 real-hardware defensive hardening: kick off audio hardware
    // probing (HDA controller reset/CORB-RIRB/codec parse, #71 MSI arm) on
    // its own low-priority background worker here, now that interrupts and
    // the scheduler are live. It runs CONCURRENTLY with login_run()/
    // desktop_run() below rather than gating them -- see the longer comment
    // above audio_init_worker() in drivers/audio.c for the full rationale.
    {
        extern void audio_start_deferred_init(void);
        audio_start_deferred_init();
    }

    // #426: prove the timed wait-queue primitive on THIS build at boot. Spawns
    // one bounded worker (~4.5s, mostly a settle sleep) that exercises all three
    // outcomes of wait_event_timeout() and logs a single "[WAITQ] self-test:
    // 3/3 PASS" line. It must live in this late cluster, not in an early
    // late_init: a worker spawned before preemption is enabled is created but
    // never scheduled, and would silently prove nothing.
    {
        extern void waitq_start_selftest(void);
        waitq_start_selftest();
    }

    gfx_boot_log("[BOOT] System initialization complete!");
    syslog_log(LOG_INFO, "System initialization complete");
    gfx_boot_progress(100);
    kprintf("[KERNEL] System initialization complete!\n\n");

    // Boot sound (async kernel thread; plays while the desktop loads).
    // #702: audio_play_file_async() is (despite its name) actually
    // SYNCHRONOUS -- it just calls audio_play_file() directly -- so calling
    // it here would block the boot path for the whole clip's duration and,
    // now that audio_init() is itself deferred to a background worker (see
    // above), would very likely find audio not ready yet and silently skip
    // the chime. Use audio_start_boot_sound() instead: a real background
    // worker (drivers/audio.c) that cooperatively waits for audio to become
    // available before playing, without blocking anything here.
    {
        extern void audio_start_boot_sound(void);
        audio_start_boot_sound();
    }
    syslog_log(LOG_INFO, "System initialization complete");

    // Start desktop
    if (boot_info->framebuffer.address != 0) {
        gfx_boot_log("[BOOT] Preparing desktop environment...");
        extern void net_poll(void); /* Poll network during startup */
        // Progress updates during startup
        gfx_boot_log("[BOOT] Loading window manager...");
        gfx_boot_log("[BOOT] Initializing GUI components...");
        gfx_boot_log("[BOOT] Loading desktop icons...");
        gfx_boot_log("[BOOT] Starting desktop services...");

        // #492 Stage 1a self-update isolation harness. Compiled out of golden
        // kernels: the boot-time /SELFUPD.REQ scaffold exists only when the
        // build defines SELFUPDATE_ISOLATION_HARNESS. In Stage 1b the live
        // trigger is the userland OTA daemon via SYS_KERNEL_SELFUPDATE, not a
        // disk-file scaffold, so a shipping kernel has no boot-time updater path.
#ifdef SELFUPDATE_ISOLATION_HARNESS
        { extern void selfupdate_boot_check(fat_fs_t *fs);
          if (g_fat_fs.mounted) selfupdate_boot_check(&g_fat_fs); }
#endif

        // Run login screen before desktop
        kprintf("[GUI] Starting login screen...\n");
        login_init();
        login_result_t login_result;
        gfx_boot_log("[BOOT] Starting login screen...");
        login_run(&login_result);
        gfx_boot_log("[BOOT] Login complete, starting desktop...");

        // Set session identity and create home directory
        desktop_set_session(login_result.uid, login_result.gid);
        if (login_result.home[0]) {
            fat_mkdir(&g_fat_fs, login_result.home);
        }

        // #95: build the background services registry now (parses
        // /CONFIG/SERVICES.CFG). Actually starting the services is deferred
        // to desktop_run(), after the compositor has launched: spawning a
        // user process this early (before any other user process exists)
        // lands on physical pages that are not yet safely writable through
        // the kernel identity map, faulting in elf_load_user. Once the
        // compositor is up the allocator is in the same steady state used
        // for every on-demand app launch, so service spawns are safe.
        svc_init();
    gfx_boot_log("[BOOT] Background services registry built");

#ifdef SECTEST_CV_FETCH
        // ------------------------------------------------------------------
        // #510 VERIFICATION BUILD ONLY (-DSECTEST_CV_FETCH via `make CVTEST=1`;
        // compiled out of every normal/golden kernel). Drives a REAL TLS 1.3
        // handshake to each real host BEFORE the desktop starts, so the
        // CertificateVerify verdict is observable on serial without needing the
        // GUI or the (keyboard-only, desktop-owned) kernel_shell.
        //
        // Paired with `make CVTAMPER=1`, which flips one bit of the server's
        // CertificateVerify signature: with the tamper on, every fetch here MUST
        // abort. That pairing is the whole point - a positive-only result cannot
        // distinguish a working check from no check at all.
        // ------------------------------------------------------------------
        {
            extern int dhcp_get_state(void);
            extern void net_poll(void);
            // Bounded wait for a DHCP lease, mirroring the idiom this same file
            // already uses for the shell `net` command. Test-only code.
            kprintf("[CV-FETCH] waiting for DHCP lease...\n");
            for (int i = 0; i < 4000 && dhcp_get_state() != 3; i++) {
                net_poll();
                for (int j = 0; j < 20000; j++) io_wait();
            }
            // Settle: a bound lease is not the same as a usable route/DNS. The
            // first run of this test fetched before the lease landed and got
            // DNS -3 on the first two hosts, which reads exactly like a TLS
            // failure in the RESULT line but is not one. Wait, then re-poll.
            for (int i = 0; i < 400; i++) {
                net_poll();
                for (int j = 0; j < 20000; j++) io_wait();
            }
            kprintf("[CV-FETCH] dhcp_state=%d (3=BOUND)\n", dhcp_get_state());

            static const char *cv_urls[] = {
                // TLS 1.3 (this task). The 2x2 that matters: the transcript hash
                // is the CIPHER SUITE's, the signature hash is the SCHEME's, and
                // on 3 of these they DIFFER (SHA384 suite signing with SHA256).
                "https://feeds.bbci.co.uk/news/rss.xml",  // SHA384 suite, ECDSA 0x0403
                "https://lobste.rs/",                     // SHA256 suite, ECDSA 0x0403
                "https://www.reddit.com/",                // SHA256 suite, RSA-PSS 0x0804
                "https://lwn.net/",                       // SHA384 suite, ECDSA 0x0403
                "https://api.moonshot.ai/",               // SHA384 suite, RSA-PSS 0x0804
                // TLS 1.2 (#502) - regression control. These do NOT go through
                // the 1.3 CertificateVerify path at all; they must keep working
                // exactly as before, proving #510 stayed inside the 1.3 branch.
                "https://xkcd.com/rss.xml",               // 1.2, 0xc02f
                "https://hnrss.org/frontpage",            // 1.2, 0xc030, secp384r1
            };
            for (int i = 0; i < 7; i++) {
                uint8_t *cvb = NULL; uint32_t cvl = 0; int cvst = 0;
                int cvr = https_get(cv_urls[i], &cvb, &cvl, &cvst);
                kprintf("[CV-FETCH] RESULT %s ret=%d status=%d len=%u\n",
                        cv_urls[i], cvr, cvst, cvl);
                if (cvb) kfree(cvb);
            }
            kprintf("[CV-FETCH] done\n");
        }
#endif

        // Start GUI desktop
        kprintf("[GUI] Starting desktop environment...\n");
        syslog_log(LOG_INFO, "Desktop environment starting");
        desktop_run();

        // If GUI exits, fall back to shell
        kprintf("[GUI] Desktop exited, falling back to shell\n");
        console_clear();
        console_set_colors(FB_CYAN, FB_BLACK);
        console_puts("========================================\n");
        console_puts("  MayteraOS 64-bit Kernel v1.0\n");
        console_puts("========================================\n\n");
        console_set_colors(FB_WHITE, FB_BLACK);
    }

    // Run simple shell (fallback or no framebuffer)
    kernel_shell();

halt:
    // Halt the CPU
    kprintf("\n[KERNEL] Halting CPU...\n");
    cli();
    for (;;) {
        hlt();
    }
}

// Helper: print to both serial and console
static void shell_print(const char *str) {
    kprintf("%s", str);
    if (g_boot_info && g_boot_info->framebuffer.address) {
        console_puts(str);
    }
}

static void shell_printf(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);

    // Simple format
    char *out = buf;
    char *end = buf + sizeof(buf) - 1;

    while (*fmt && out < end) {
        if (*fmt != '%') {
            *out++ = *fmt++;
            continue;
        }
        fmt++;
        switch (*fmt) {
            case 's': {
                const char *s = va_arg(args, const char *);
                while (s && *s && out < end) *out++ = *s++;
                break;
            }
            case 'd': {
                int v = va_arg(args, int);
                char num[32]; int i = 0;
                int neg = v < 0;
                if (neg) v = -v;
                do { num[i++] = '0' + v % 10; v /= 10; } while (v);
                if (neg) num[i++] = '-';
                while (i-- > 0 && out < end) *out++ = num[i];
                break;
            }
            case 'u': {
                unsigned int v = va_arg(args, unsigned int);
                char num[32]; int i = 0;
                do { num[i++] = '0' + v % 10; v /= 10; } while (v);
                while (i-- > 0 && out < end) *out++ = num[i];
                break;
            }
            case 'l': {
                fmt++;
                uint64_t v = va_arg(args, uint64_t);
                char num[32]; int i = 0;
                do { num[i++] = '0' + v % 10; v /= 10; } while (v);
                while (i-- > 0 && out < end) *out++ = num[i];
                break;
            }
            case 'x': {
                unsigned int v = va_arg(args, unsigned int);
                char num[32]; int i = 0;
                do { num[i++] = "0123456789abcdef"[v & 0xf]; v >>= 4; } while (v);
                while (i-- > 0 && out < end) *out++ = num[i];
                break;
            }
            case 'c': {
                int c = va_arg(args, int);
                if (out < end) *out++ = (char)c;
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)va_arg(args, void*);
                *out++ = '0'; *out++ = 'x';
                char num[32]; int i = 0;
                do { num[i++] = "0123456789abcdef"[v & 0xf]; v >>= 4; } while (v);
                while (i-- > 0 && out < end) *out++ = num[i];
                break;
            }
            default:
                if (out < end) *out++ = *fmt;
                break;
        }
        fmt++;
    }
    *out = '\0';
    va_end(args);

    kprintf("%s", buf);
    if (g_boot_info && g_boot_info->framebuffer.address) {
        console_puts(buf);
    }
}

static void shell_putc(char c) {
    kputc(c);
    if (g_boot_info && g_boot_info->framebuffer.address) {
        console_putc(c);
    }
}

// Command history for shell
#define SHELL_HISTORY_SIZE 10
static char shell_history[SHELL_HISTORY_SIZE][256];
static int shell_history_count = 0;
static int shell_history_pos = 0;

static void shell_add_history(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    // Don't add duplicates
    if (shell_history_count > 0 &&
        strcmp(shell_history[(shell_history_count - 1) % SHELL_HISTORY_SIZE], cmd) == 0) {
        return;
    }
    strcpy(shell_history[shell_history_count % SHELL_HISTORY_SIZE], cmd);
    shell_history_count++;
    shell_history_pos = shell_history_count;
}

static void shell_clear_line(int len) {
    // Move cursor back and clear
    for (int i = 0; i < len; i++) {
        shell_print("\b \b");
    }
}

// Simple kernel shell
void kernel_shell(void) {
    static char command_buffer[256];
    int cmd_pos = 0;

    shell_print("MayteraOS Shell - Type 'help' for commands\n");
    shell_print("maytera> ");

    while (1) {
        // Wait for keyboard input
        if (keyboard_has_char()) {
            int c = keyboard_get_char();

            if (c == '\n') {
                // Execute command
                shell_print("\n");
                command_buffer[cmd_pos] = '\0';

                if (cmd_pos > 0) {
                    // Add to history before processing
                    shell_add_history(command_buffer);

                    // Process command
                    if (strcmp(command_buffer, "help") == 0) {
                        shell_print("Available commands:\n");
                        shell_print("  help    - Show this help\n");
                        shell_print("  mem     - Show memory info\n");
                        shell_print("  pmm     - Show physical memory stats\n");
                        shell_print("  heap    - Show heap stats\n");
                        shell_print("  fb      - Show framebuffer info\n");
                        shell_print("  ticks   - Show timer ticks\n");
                        shell_print("  alloc   - Test allocate 1KB\n");
                        shell_print("  clear   - Clear screen\n");
                        shell_print("  gfx     - Graphics test pattern\n");
                        shell_print("  disk    - Show disk info\n");
                        shell_print("  read    - Read sector 0\n");
                        shell_print("  net     - Show network status\n");
                        shell_print("  ping    - Ping gateway\n");
                        shell_print("  dhcp    - Start DHCP discovery\n");
                        shell_print("  arp     - Show ARP table\n");
                        shell_print("  pci     - Show PCI devices\n");
                        shell_print("  ps      - Show running processes\n");
                        shell_print("Audio:\n");
                        shell_print("  sound   - Show sound card info\n");
                        shell_print("  beep    - Play test tone\n");
                        shell_print("Filesystem:\n");
                        shell_print("  ls      - List directory (ls /path)\n");
                        shell_print("  cd      - Change directory\n");
                        shell_print("  pwd     - Print working directory\n");
                        shell_print("  cat     - Show file contents\n");
                        shell_print("  mount   - Mount FAT partition\n");
                        shell_print("System:\n");
                        shell_print("  shutdown - Power off system (ACPI)\n");
                        shell_print("  reboot  - Reboot system\n");
                        shell_print("GUI:\n");
                        shell_print("  gui     - Start desktop environment\n");
                        shell_print("  wget    - Fetch URL (wget <url>)\n");
                        shell_print("  splash  - Show boot splash screen\n");
                        shell_print("\nPress Ctrl+C to cancel running commands\n");
                    } else if (strcmp(command_buffer, "mem") == 0) {
                        shell_printf("Total RAM: %lu MB\n", g_boot_info->total_memory / MB);
                        shell_printf("Memory map entries: %u\n", g_boot_info->memory_map_entries);
                    } else if (strcmp(command_buffer, "pmm") == 0) {
                        kprintf_set_dual_output(1);
                        pmm_print_stats();
                        kprintf_set_dual_output(0);
                    } else if (strcmp(command_buffer, "heap") == 0) {
                        kprintf_set_dual_output(1);
                        heap_print_stats();
                        kprintf_set_dual_output(0);
                    } else if (strcmp(command_buffer, "fb") == 0) {
                        kprintf_set_dual_output(1);
                        print_framebuffer_info();
                        kprintf_set_dual_output(0);
                    } else if (strcmp(command_buffer, "ticks") == 0) {
                        shell_printf("Timer ticks: %lu\n", timer_ticks);

                    } else if (strcmp(command_buffer, "alloc") == 0) {
                        void *ptr = kmalloc(1024);
                        shell_printf("Allocated 1KB at %p\n", ptr);
                        if (ptr) {
                            kfree(ptr);
                            shell_print("Freed allocation\n");
                        }
                    } else if (strcmp(command_buffer, "clear") == 0) {
                        if (g_boot_info && g_boot_info->framebuffer.address) {
                            console_clear();
                        }
                    } else if (strcmp(command_buffer, "gfx") == 0) {
                        if (g_boot_info && g_boot_info->framebuffer.address) {
                            shell_print("Drawing test pattern...\n");
                            gfx_test_pattern();
                        } else {
                            shell_print("No framebuffer available\n");
                        }
                    } else if (strcmp(command_buffer, "disk") == 0) {
                        kprintf_set_dual_output(1);
                        ata_print_info();
                        kprintf_set_dual_output(0);
                    } else if (strcmp(command_buffer, "read") == 0) {
                        // Read first sector from primary master
                        static uint8_t sector_buf[512];
                        ata_drive_t *drv = ata_get_drive(0, 0);
                        if (drv) {
                            shell_print("Reading sector 0...\n");
                            if (ata_read_sectors(0, 0, 0, 1, sector_buf) > 0) {
                                shell_print("First 64 bytes (hex):\n");
                                for (int i = 0; i < 64; i++) {
                                    shell_printf("%x ", sector_buf[i]);
                                    if ((i + 1) % 16 == 0) shell_print("\n");
                                }
                            } else {
                                shell_print("Read failed!\n");
                            }
                        } else {
                            shell_print("No ATA drive found\n");
                        }
                    } else if (strcmp(command_buffer, "net") == 0) {
                        // Print network status to both serial and console
                        shell_print("\n[NET] Network Status:\n");
                        uint8_t mac[6];
                        nic_get_mac(mac);
                        shell_printf("  MAC Address:  %x:%x:%x:%x:%x:%x\n",
                                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                        uint32_t ip = ip_get_address();
                        uint32_t gw = ip_get_gateway();
                        uint32_t nm = ip_get_netmask();
                        uint8_t *p;
                        p = (uint8_t *)&ip;
                        shell_printf("  IP Address:   %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
                        p = (uint8_t *)&nm;
                        shell_printf("  Netmask:      %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
                        p = (uint8_t *)&gw;
                        shell_printf("  Gateway:      %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
                        shell_printf("  Link Status:  %s\n", nic_link_up() ? "Up" : "Down");
                    } else if (strcmp(command_buffer, "ping") == 0) {
                        // Ping gateway
                        uint32_t gateway = ip_get_gateway();
                        if (gateway != 0) {
                            extern int icmp_get_ping_reply(uint32_t *src_ip, uint16_t *seq, uint16_t *time_ms);
                            uint8_t *gp = (uint8_t *)&gateway;
                            shell_printf("Pinging gateway %d.%d.%d.%d...\n", gp[3], gp[2], gp[1], gp[0]);

                            int sent = 0, received = 0;
                            clear_interrupt();
                            for (int ping_num = 0; ping_num < 4; ping_num++) {
                                if (check_interrupt()) {
                                    shell_print("^C\n");
                                    break;
                                }

                                icmp_ping(gateway);
                                sent++;

                                int got_reply = 0;
                                for (int i = 0; i < 200 && !got_reply; i++) {
                                    if (check_interrupt()) break;
                                    net_poll();

                                    uint32_t reply_ip;
                                    uint16_t seq, time_ms;
                                    if (icmp_get_ping_reply(&reply_ip, &seq, &time_ms)) {
                                        uint8_t *rp = (uint8_t *)&reply_ip;
                                        shell_printf("Reply from %d.%d.%d.%d: seq=%d time=%dms\n",
                                                    rp[3], rp[2], rp[1], rp[0], seq, time_ms);
                                        received++;
                                        got_reply = 1;
                                    }

                                    for (int j = 0; j < 1000; j++) io_wait();
                                }

                                if (!got_reply && !check_interrupt()) {
                                    shell_print("Request timed out.\n");
                                }
                            }

                            shell_printf("\n--- ping statistics ---\n");
                            shell_printf("%d packets sent, %d received, %d%% loss\n",
                                        sent, received, sent > 0 ? ((sent - received) * 100 / sent) : 0);
                        } else {
                            shell_print("No gateway configured. Use: ping <ip>\n");
                        }
                    } else if (strncmp(command_buffer, "ping ", 5) == 0) {
                        // Parse IP address from "ping x.x.x.x"
                        const char *ip_str = &command_buffer[5];
                        uint32_t target_ip = 0;
                        uint8_t octets[4] = {0, 0, 0, 0};
                        int octet_idx = 0;
                        int value = 0;
                        bool valid = true;

                        for (const char *c = ip_str; *c && octet_idx < 4; c++) {
                            if (*c >= '0' && *c <= '9') {
                                value = value * 10 + (*c - '0');
                                if (value > 255) { valid = false; break; }
                            } else if (*c == '.') {
                                octets[octet_idx++] = value;
                                value = 0;
                            } else if (*c == ' ' || *c == '\0') {
                                break;
                            } else {
                                valid = false;
                                break;
                            }
                        }
                        if (octet_idx < 4 && valid) {
                            octets[octet_idx] = value;
                            octet_idx++;
                        }

                        if (valid && octet_idx == 4) {
                            // Build IP in host byte order (high byte = first octet)
                            target_ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
                            shell_printf("Pinging %d.%d.%d.%d...\n",
                                        octets[0], octets[1], octets[2], octets[3]);

                            extern int icmp_get_ping_reply(uint32_t *src_ip, uint16_t *seq, uint16_t *time_ms);

                            // Send 4 pings
                            int sent = 0, received = 0;
                            clear_interrupt();
                            for (int ping_num = 0; ping_num < 4; ping_num++) {
                                if (check_interrupt()) {
                                    shell_print("^C\n");
                                    break;
                                }

                                // Try to send ping (may fail if ARP not resolved)
                                int send_result = icmp_ping(target_ip);
                                if (send_result < 0) {
                                    // ARP not resolved, poll to receive ARP reply and retry
                                    for (int arp_wait = 0; arp_wait < 50; arp_wait++) {
                                        net_poll();
                                        for (int j = 0; j < 1000; j++) io_wait();
                                    }
                                    // Retry the ping
                                    send_result = icmp_ping(target_ip);
                                }
                                if (send_result >= 0) {
                                    sent++;
                                }

                                // Wait for reply (up to ~2 seconds)
                                int got_reply = 0;
                                for (int i = 0; i < 200 && !got_reply; i++) {
                                    if (check_interrupt()) break;
                                    net_poll();

                                    uint32_t reply_ip;
                                    uint16_t seq, time_ms;
                                    if (icmp_get_ping_reply(&reply_ip, &seq, &time_ms)) {
                                        uint8_t *rp = (uint8_t *)&reply_ip;
                                        shell_printf("Reply from %d.%d.%d.%d: seq=%d time=%dms\n",
                                                    rp[3], rp[2], rp[1], rp[0], seq, time_ms);
                                        received++;
                                        got_reply = 1;
                                    }

                                    for (int j = 0; j < 1000; j++) io_wait();
                                }

                                if (!got_reply && !check_interrupt()) {
                                    shell_print("Request timed out.\n");
                                }
                            }

                            // Summary
                            shell_printf("\n--- %d.%d.%d.%d ping statistics ---\n",
                                        octets[0], octets[1], octets[2], octets[3]);
                            shell_printf("%d packets sent, %d received, %d%% loss\n",
                                        sent, received, sent > 0 ? ((sent - received) * 100 / sent) : 0);
                        } else {
                            shell_print("Invalid IP address. Usage: ping <ip>\n");
                            shell_print("Example: ping 1.1.1.1\n");
                        }
                    } else if (strcmp(command_buffer, "dhcp") == 0) {
                        extern int dhcp_get_state(void);
                        extern int nic_link_up(void);

                        if (!nic_link_up()) {
                            shell_print("Network link is down\n");
                        } else {
                            shell_print("Starting DHCP discovery...\n");
                            clear_interrupt();
                            net_dhcp();

                            // Poll for response with shorter timeout (5 seconds)
                            // Show progress every second
                            shell_print("Waiting for DHCP server (5s timeout)");
                            for (int i = 0; i < 50; i++) {  // 50 * 100ms = 5 seconds
                                if (check_interrupt()) {
                                    shell_print("\n^C - Cancelled\n");
                                    break;
                                }

                                net_poll();

                                // Check if DHCP succeeded (state == BOUND == 3)
                                if (dhcp_get_state() == 3) {
                                    shell_print("\nDHCP successful!\n");
                                    break;
                                }

                                // Progress dot every second
                                if (i % 10 == 9) {
                                    shell_print(".");
                                }

                                // ~100ms delay
                                for (int j = 0; j < 1000; j++) {
                                    io_wait();
                                }
                            }

                            // Show result
                            if (dhcp_get_state() != 3) {
                                shell_print("\nDHCP failed (no server responded)\n");
                            }

                            // Print current IP status
                            uint32_t ip = ip_get_address();
                            if (ip != 0) {
                                uint8_t *p = (uint8_t *)&ip;
                                kprintf("IP: %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
                            }
                        }
                    } else if (strcmp(command_buffer, "arp") == 0) {
                        arp_print_table();
                    } else if (strcmp(command_buffer, "pci") == 0) {
                        kprintf_set_dual_output(1);
                        pci_print_devices();
                        kprintf_set_dual_output(0);
                    } else if (strcmp(command_buffer, "ps") == 0) {
                        kprintf_set_dual_output(1);
                        proc_print_list();
                        kprintf_set_dual_output(0);
                    } else if (strcmp(command_buffer, "sound") == 0) {
                        kprintf_set_dual_output(1);
                        sound_print_info();
                        kprintf_set_dual_output(0);
                    } else if (strcmp(command_buffer, "beep") == 0) {
                        shell_print("Playing test tone (440 Hz, 500ms)...\n");
                        sound_play_tone(440, 500);
                        shell_print("Done.\n");
                    } else if (strncmp(command_buffer, "beep ", 5) == 0) {
                        // Parse frequency from "beep <freq>"
                        const char *freq_str = &command_buffer[5];
                        uint32_t freq = 0;
                        while (*freq_str >= '0' && *freq_str <= '9') {
                            freq = freq * 10 + (*freq_str - '0');
                            freq_str++;
                        }
                        if (freq >= 20 && freq <= 20000) {
                            shell_printf("Playing tone at %u Hz...\n", freq);
                            sound_play_tone(freq, 500);
                            shell_print("Done.\n");
                        } else {
                            shell_print("Usage: beep [frequency]\n");
                            shell_print("  frequency: 20-20000 Hz (default: 440)\n");
                        }
                    } else if (strncmp(command_buffer, "ls", 2) == 0 &&
                               (command_buffer[2] == '\0' || command_buffer[2] == ' ')) {
                        const char *path = g_cwd;
                        if (command_buffer[2] == ' ' && command_buffer[3]) {
                            path = &command_buffer[3];
                        }
                        if (g_fat_fs.mounted) {
                            fat_list_dir(&g_fat_fs, path);
                        } else {
                            shell_print("No filesystem mounted\n");
                        }
                    } else if (strncmp(command_buffer, "cat ", 4) == 0) {
                        const char *path = &command_buffer[4];
                        if (g_fat_fs.mounted) {
                            uint32_t size;
                            void *data = fat_read_file(&g_fat_fs, path, &size);
                            if (data) {
                                shell_printf("--- %s (%u bytes) ---\n", path, size);
                                // Print as text (limit to 4KB for display)
                                char *text = (char *)data;
                                for (uint32_t i = 0; i < size && i < 4096; i++) {
                                    shell_putc(text[i]);
                                }
                                if (size > 4096) {
                                    shell_print("\n... (truncated)\n");
                                }
                                shell_print("\n--- EOF ---\n");
                                kfree(data);
                            } else {
                                shell_printf("Cannot read: %s\n", path);
                            }
                        } else {
                            shell_print("No filesystem mounted\n");
                        }
                    } else if (strncmp(command_buffer, "mount ", 6) == 0) {
                        int part = command_buffer[6] - '0';
                        if (part >= 0 && part <= 3) {
                            if (g_fat_fs.mounted) {
                                fat_unmount(&g_fat_fs);
                            }
                            if (fat_mount(0, part, &g_fat_fs) == 0) {
                                fat_print_info(&g_fat_fs);

            // Initialize read-ahead cache for better disk performance
            if (fat_readahead_init(RA_DEFAULT_BUFFER_SIZE) == 0) {
                kprintf("[BOOT] FAT read-ahead cache initialized (256KB)\n");
            }
                            }
                        } else {
                            shell_print("Usage: mount <0-3>\n");
                        }
                    } else if (strcmp(command_buffer, "mount") == 0) {
                        if (g_fat_fs.mounted) {
                            fat_print_info(&g_fat_fs);

            // Initialize read-ahead cache for better disk performance
            if (fat_readahead_init(RA_DEFAULT_BUFFER_SIZE) == 0) {
                kprintf("[BOOT] FAT read-ahead cache initialized (256KB)\n");
            }
                        } else {
                            shell_print("No filesystem mounted. Use: mount <partition>\n");
                        }
                    } else if (strcmp(command_buffer, "pwd") == 0) {
                        shell_printf("%s\n", g_cwd);
                    } else if (strncmp(command_buffer, "cd ", 3) == 0) {
                        const char *path = &command_buffer[3];
                        // Handle special cases
                        if (strcmp(path, "/") == 0) {
                            strcpy(g_cwd, "/");
                        } else if (strcmp(path, "..") == 0) {
                            // Go up one directory
                            char *last = strrchr(g_cwd, '/');
                            if (last && last != g_cwd) {
                                *last = '\0';
                            } else {
                                strcpy(g_cwd, "/");
                            }
                        } else {
                            // Build new path
                            char newpath[256];
                            if (path[0] == '/') {
                                strcpy(newpath, path);
                            } else {
                                if (strcmp(g_cwd, "/") == 0) {
                                    strcpy(newpath, "/");
                                    strcat(newpath, path);
                                } else {
                                    strcpy(newpath, g_cwd);
                                    strcat(newpath, "/");
                                    strcat(newpath, path);
                                }
                            }
                            // Verify directory exists
                            if (g_fat_fs.mounted) {
                                fat_file_t dir;
                                if (fat_open(&g_fat_fs, newpath, &dir) == 0 && dir.is_dir) {
                                    strcpy(g_cwd, newpath);
                                    fat_close(&dir);
                                } else {
                                    shell_printf("cd: %s: Not a directory\n", path);
                                }
                            } else {
                                strcpy(g_cwd, newpath);  // No FS to verify
                            }
                        }
                    } else if (strcmp(command_buffer, "cd") == 0) {
                        strcpy(g_cwd, "/");
                    } else if (strcmp(command_buffer, "shutdown") == 0) {
                        shell_print("Shutting down...\n");
                        acpi_shutdown();
                        // If ACPI shutdown failed, inform user
                        shell_print("ACPI shutdown failed. Try 'reboot' instead.\n");
                    } else if (strcmp(command_buffer, "reboot") == 0) {
                        shell_print("Rebooting...\n");
                        acpi_reboot();
                        // If ACPI reboot failed (shouldn't reach here normally)
                    } else if (strncmp(command_buffer, "wget ", 5) == 0) {
                        const char *wurl = command_buffer + 5;
                        while (*wurl == ' ') wurl++;
                        if (*wurl == '\0') {
                            shell_print("Usage: wget <url>\n");
                        } else {
                            kprintf("[SHELL] wget: %s\n", wurl);
                            shell_printf("Fetching: %s\n", wurl);
                            uint8_t *body = NULL;
                            uint32_t body_len = 0;
                            int status = 0;
                            int ret = https_get(wurl, &body, &body_len, &status);
                            if (ret == 0) {
                                shell_printf("HTTP %d, %u bytes received\n", status, body_len);
                                uint32_t show = (body_len > 512) ? 512 : body_len;
                                for (uint32_t bi = 0; bi < show; bi++) {
                                    char ch = (char)body[bi];
                                    if (ch >= 32 && ch < 127) shell_printf("%c", ch);
                                    else if (ch == '\n') shell_print("\n");
                                }
                                shell_print("\n");
                                if (body) kfree(body);
                            } else {
                                shell_printf("HTTPS error: %s\n", https_strerror(ret));
                            }
                        }
                    } else if (strcmp(command_buffer, "gui") == 0) {
                        shell_print("Starting GUI desktop...\n");
                        // Draw boot splash first
                        gfx_boot_simple();
                        for (int i = 0; i <= 100; i += 10) {
                            gfx_boot_progress(i);
                            for (int j = 0; j < 50000; j++) io_wait();
                        }
                        // Short delay
                        for (int j = 0; j < 500000; j++) io_wait();
                        // Start desktop
                        desktop_run();
                        // When desktop exits, redraw console
                        console_clear();
                        shell_print("GUI exited. Type 'gui' to restart.\n");
                    } else if (strcmp(command_buffer, "splash") == 0) {
                        shell_print("Showing boot splash...\n");
                        gfx_boot_simple();
                        for (int i = 0; i <= 100; i += 5) {
                            gfx_boot_progress(i);
                            for (int j = 0; j < 30000; j++) io_wait();
                        }
                    } else {
                        shell_printf("Unknown command: %s\n", command_buffer);
                    }
                }

                cmd_pos = 0;
                shell_print("maytera> ");
            } else if (c == '\b') {
                // Backspace
                if (cmd_pos > 0) {
                    cmd_pos--;
                    shell_print("\b \b");  // Erase character
                }
            } else if (c == KEY_UP) {
                // Up arrow - previous history
                if (shell_history_pos > 0 && shell_history_count > 0) {
                    shell_history_pos--;
                    int idx = shell_history_pos % SHELL_HISTORY_SIZE;
                    // Clear current line
                    shell_clear_line(cmd_pos);
                    // Copy history entry
                    strcpy(command_buffer, shell_history[idx]);
                    cmd_pos = strlen(command_buffer);
                    // Display it
                    shell_print(command_buffer);
                }
            } else if (c == KEY_DOWN) {
                // Down arrow - next history
                if (shell_history_pos < shell_history_count) {
                    shell_history_pos++;
                    // Clear current line
                    shell_clear_line(cmd_pos);
                    if (shell_history_pos < shell_history_count) {
                        int idx = shell_history_pos % SHELL_HISTORY_SIZE;
                        strcpy(command_buffer, shell_history[idx]);
                        cmd_pos = strlen(command_buffer);
                        shell_print(command_buffer);
                    } else {
                        // Past end of history - clear line
                        command_buffer[0] = '\0';
                        cmd_pos = 0;
                    }
                }
            } else if (c >= ' ' && c < 127) {
                // Printable character
                if (cmd_pos < 255) {
                    command_buffer[cmd_pos++] = c;
                    shell_putc(c);  // Echo
                }
            }
        } else {
            // No input, poll network and halt until next interrupt
            net_poll();
            hlt();
        }
    }
}

// Print framebuffer info
void print_framebuffer_info(void) {
    framebuffer_info_t *fb = &g_boot_info->framebuffer;

    kprintf("\n[FRAMEBUFFER]\n");
    kprintf("  Address:    0x%lx\n", fb->address);
    kprintf("  Resolution: %ux%u\n", fb->width, fb->height);
    kprintf("  Pitch:      %u bytes/line\n", fb->pitch);
    kprintf("  BPP:        %u bits\n", fb->bpp);

    const char *format_str;
    switch (fb->pixel_format) {
        case PIXEL_FORMAT_RGB:  format_str = "RGB"; break;
        case PIXEL_FORMAT_BGR:  format_str = "BGR"; break;
        case PIXEL_FORMAT_MASK: format_str = "Bitmask"; break;
        default:                format_str = "Unknown"; break;
    }
    kprintf("  Format:     %s\n", format_str);

    // Calculate total framebuffer size
    uint64_t fb_size = (uint64_t)fb->pitch * fb->height;
    kprintf("  Size:       %lu KB\n", fb_size / KB);
}

// Print memory map
void print_memory_map(void) {
    kprintf("\n[MEMORY MAP]\n");
    kprintf("  Base               Length             Pages        Type\n");
    kprintf("  ----               ------             -----        ----\n");

    memory_map_entry_t *entries = (memory_map_entry_t *)g_boot_info->memory_map_address;
    uint32_t count = g_boot_info->memory_map_entries;

    // Limit output for readability
    if (count > 20) {
        kprintf("  (Showing first 20 of %u entries)\n", count);
        count = 20;
    }

    for (uint32_t i = 0; i < count; i++) {
        const char *type_str;
        switch (entries[i].type) {
            case MEMORY_TYPE_USABLE:           type_str = "Usable"; break;
            case MEMORY_TYPE_RESERVED:         type_str = "Reserved"; break;
            case MEMORY_TYPE_ACPI_RECLAIMABLE: type_str = "ACPI Reclaim"; break;
            case MEMORY_TYPE_ACPI_NVS:         type_str = "ACPI NVS"; break;
            case MEMORY_TYPE_BAD:              type_str = "Bad"; break;
            case MEMORY_TYPE_BOOTLOADER:       type_str = "Bootloader"; break;
            case MEMORY_TYPE_KERNEL:           type_str = "Kernel"; break;
            case MEMORY_TYPE_FRAMEBUFFER:      type_str = "Framebuffer"; break;
            default:                           type_str = "Unknown"; break;
        }

        kprintf("  0x%016lx 0x%016lx %12lu %s\n",
                entries[i].base,
                entries[i].length,
                entries[i].length / PAGE_SIZE,
                type_str);
    }
}

// Boot info helper functions
void boot_info_init(boot_info_t *info) {
    g_boot_info = info;
}

void boot_info_print(void) {
    if (g_boot_info) {
        print_memory_map();
        print_framebuffer_info();
    }
}

uint64_t boot_info_get_total_memory(void) {
    return g_boot_info ? g_boot_info->total_memory : 0;
}

memory_map_entry_t* boot_info_get_memory_map(uint32_t *count) {
    if (g_boot_info && count) {
        *count = g_boot_info->memory_map_entries;
        return (memory_map_entry_t *)g_boot_info->memory_map_address;
    }
    if (count) *count = 0;
    return NULL;
}

framebuffer_info_t* boot_info_get_framebuffer(void) {
    return g_boot_info ? &g_boot_info->framebuffer : NULL;
}

/* ==========================================================================
 * #404 / #478 Phase A: Rust #[panic_handler] landing pad.
 *
 * rustkern.rs's panic handler calls this so a FUTURE Rust-side panic logs
 * loudly and halts, instead of silently spinning in a bare Rust `loop {}`.
 * It mirrors the kernel-mode fatal path in cpu/idt.c: kprintf a loud banner
 * (serial + dual output) and persist a record to /PANIC.TXT via the existing
 * fs/panic.c raw-sector writer, then disable interrupts and halt.
 *
 * Reuses existing shared primitives only (kprintf, panic_log_write); adds no
 * new subsystem. In Phase A this is UNREACHABLE at runtime (no Rust code path
 * can panic yet), so it introduces zero behavioral change; it exists purely so
 * Rust-in-the-image is safe by construction before any behavior swap.
 * Never returns.
 * ========================================================================== */
void rust_kernel_panic(const char *msg)
{
    /* #480: route the Rust #[panic_handler] through the ONE canonical kernel
     * panic primitive (fs/panic.c kpanic), so a Rust-side panic now logs the
     * loud "[PANIC] " banner, persists a /PANIC.TXT record, and enters the
     * shared terminal halt exactly like every other fatal path - no bespoke
     * shim. kpanic() is noreturn, matching the Rust side's `-> !` contract.
     * The Rust FFI signature (extern "C" rust_kernel_panic(*const u8) -> !) is
     * unchanged; only this C body changed. */
    extern void kpanic(const char *fmt, ...)
        __attribute__((noreturn, format(printf, 1, 2)));

    kpanic("RUST PANIC: %s", msg ? msg : "(null)");
}
