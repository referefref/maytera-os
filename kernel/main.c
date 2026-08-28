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
#include "net/https.h"
#include "fs/fat.h"
#include "fs/ext2.h"          // #610: on-device fsck + dirty detection
#include "fs/fat_readahead.h"
#include "fs/bootlog.h"           // #307 real-hw: persistent /BOOTLOG.TXT
#include "fs/bootstage.h"         // #ASUSDIAG: named checkpoints to every channel that exists yet
#include "fs/cfgread.h"           // #192: config-read log policy + its ABI lock
#include "cpu/tickwatch.h"        // #745 (#62): is the periodic tick delivered?
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
#include "security/selftest_registry.h"  // #PERMSKIP
#include "proc/users.h"
#include "proc/pwpolicy.h"
#include "proc/services.h"
#include <stdarg.h>
#include "cpu/dlprof.h"
#include "fs/userconf.h"   // #683/#684: per-user config paths
#include "dos/dos4gw.h"   // #740: the DOS/4GW bridge boot self-test

// Global FAT filesystem (non-static for access from other modules)
fat_fs_t g_fat_fs;

// Current working directory
static char g_cwd[256] = "/";

// Global boot info pointer
boot_info_t *g_boot_info = NULL;

// Forward declarations
void print_memory_map(void);
void print_framebuffer_info(void);
void print_video_mode_info(void);
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
//
// #178 THIS IS A STATED EXCEPTION TO THE ONE SHARED RANKING, NOT AN OVERSIGHT.
// Four userland programs also ranked processes by CPU delta; each had written
// its own copy, they drifted, and #145 fixed one defect in all of them and a
// second defect in only two, with two different bounds. Those four now all call
// userland/libc/proccpu.c, which holds the pid matching, the idle exclusion and
// the denominator policy exactly once. Read userland/libc/proccpu.h before
// touching the arithmetic below. This function does NOT call it, because:
//
//   (a) IT PHYSICALLY CANNOT. build/build-golden.sh ships the kernel build
//       container 'git archive $GIT_COMMIT kernel' and nothing else, so no file
//       outside kernel/ exists when kernel.elf is compiled. Putting a shared
//       helper where the kernel build cannot see it is #514 repeating: the
//       concurrency lint sat at <repo>/tools/ and could therefore never run.
//       This is also Ring 0 and cannot link the userland libc in any case.
//   (b) THE DENOMINATOR IS A DIFFERENT QUANTITY BY DESIGN. This divides by
//       interval_ticks, a FIXED wall-clock window, so "pct" here means share of
//       one core over the heartbeat interval. Userland divides by the sum of
//       every row's delta, so its percentage means share of total machine
//       capacity and agrees with SYS_GET_CPU_USAGE in the same window (#182).
//       Do not "unify" these two: they answer different questions and the
//       userland one has a footer it must not contradict.
//
// What IS shared, and must stay shared in meaning: idle is excluded from the
// candidates (PROC_INFO_F_IDLE, a kernel bit, never a name comparison), and
// baselines are matched BY PID against the previous snapshot, never indexed by
// pid. Both are the four lines below. If you change either, change proccpu.c
// too, and say so.
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
        // #145: an IDLE process is not a consumer. It accrues a tick for every
        // tick its core had nothing to do, so on any machine that is not
        // saturated it wins this ranking outright and "top=" answers "idle" -
        // which is the one answer this field exists to avoid printing. Skipping
        // it does not distort the percentages: pct is a share of
        // interval_ticks, a FIXED denominator, not a share of these rows.
        if (cur[i].flags & PROC_INFO_F_IDLE) continue;
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

// #684: copy the /CONFIG/KIMI.KEY deployment seed into the logged-in user's own
// protected AI settings, once. See the call site for the reasoning.
static void provision_ai_key(login_result_t *lr) {
    if (!lr || !g_fat_fs.mounted) return;

    char dest[256];
    if (userconf_kpath("AISVC.CFG", dest, sizeof(dest)) != 0) return;

    // Already provisioned, or the user has their own settings: never clobber.
    uint32_t esz = 0;
    void *existing = fat_read_file(&g_fat_fs, dest, &esz);
    if (existing) { kfree(existing); return; }

    uint32_t ksz = 0;
    char *seed = (char *)fat_read_file(&g_fat_fs, "/CONFIG/KIMI.KEY", &ksz);
    if (!seed) {
        // The normal deployment case. The user types their key into Settings.
        return;
    }

    // Trim to the first line and strip trailing whitespace, matching what the
    // deleted userland load_key() did, so a hand-edited seed behaves the same.
    uint32_t n = 0;
    while (n < ksz && seed[n] != '\n' && seed[n] != '\r') n++;
    while (n > 0 && (seed[n - 1] == ' ' || seed[n - 1] == '\t')) n--;
    if (n == 0 || n > 200) { kfree(seed); return; }

    char buf[256];
    const char *pfx = "api_key=";
    uint32_t w = 0;
    while (pfx[w]) { buf[w] = pfx[w]; w++; }
    for (uint32_t i = 0; i < n; i++) buf[w++] = seed[i];
    buf[w++] = '\n';
    kfree(seed);

    // Ensure <home>/CONFIG exists, then write and lock the file down to the
    // user: 0600, owned by them. perms_set (not perms_on_create) because this
    // must establish the mode even though Ring 0 just created the path.
    char dir[256];
    uint32_t cut = 0;
    for (uint32_t i = 0; dest[i]; i++) if (dest[i] == '/') cut = i;
    if (cut > 0) {
        for (uint32_t i = 0; i < cut; i++) dir[i] = dest[i];
        dir[cut] = 0;
        fat_mkdir(&g_fat_fs, dir);
        perms_on_create(dir, lr->uid, lr->gid, 1);
    }
    if (fat_write_file(&g_fat_fs, dest, buf, w) == 0) {
        perms_set(dest, lr->uid, lr->gid, 0600);
        if (perms_sync() != 0)
            kprintf("[PERMS] initial sync failed; permissions are NOT on disk\n");
        kprintf("[AI] #684: provisioned %s from the /CONFIG/KIMI.KEY seed "
                "(uid=%u, mode 0600)\n", dest, lr->uid);
    }
}

static void heartbeat_worker(void *arg) {
    (void)arg;
    extern volatile uint64_t timer_ticks;      // cpu/isr.c, monotonic PIT tick
    extern volatile uint64_t g_ctx_switches;   // proc/process.c, scheduler switches
    extern volatile uint64_t g_fb_flip_count;  // gui/fb_syscall.c, presents so far
    extern uint32_t g_timer_hz;
    uint64_t n = 0;
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
        // #745 (#62): divide by SAMPLES TAKEN, not by ticks elapsed. Per-process
        // CPU time is incremented once per sched_tick() call; timer_ticks can
        // advance faster than sched_tick() runs once the #62 redundant tick
        // source is carrying the system (it fires at 50 Hz and advances
        // timer_ticks by what real time says has elapsed). Dividing by ticks
        // then under-reports every percentage by that ratio, and this file is
        // the one telemetry we are asking the owner to send back from a machine
        // that may well be running on exactly that path.
        extern volatile uint64_t g_sched_tick_samples;
        static uint64_t last_samples = 0;
        uint64_t samples_now = g_sched_tick_samples;
        uint64_t interval = (samples_now > last_samples) ? (samples_now - last_samples) : 1;
        last_samples = samples_now;
        hb_top_consumers(topbuf, sizeof(topbuf), interval);
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
        // #610: wqr = wait-queue un-park rescues. Each one is an instance of
        // the lost-wakeup race (a wait_event() whose condition came true after
        // the waiter had already been parked). Before the fix in
        // sync/waitq.c each of these permanently deleted a thread from the
        // scheduler; a non-zero count here means the race is live on this
        // workload and the fix is the only thing preventing the hang.
        extern volatile uint64_t g_wq_unpark_rescues;
        // #254/#617: promo = anti-starvation promotions performed, e2w =
        // contended ext2-lock acquisitions. Both are cumulative. They make the
        // two fixes auditable from an ordinary boot log instead of needing a
        // special build: promo climbing means the scheduler had to rescue a
        // passed-over thread, e2w climbing means the ext2 ticket queue is
        // actually being used.
        extern volatile uint64_t g_sched_promotions;
        extern volatile uint64_t g_ext2_lock_waits;
        // #621: blkstale = blk_read() demand-cache installs DECLINED because a
        // write bumped the block-layer write generation while our device read
        // was in flight (#617). mscnb = iterations ever executed by the USB-MSC
        // no-block command-lock fallback (#617). Both were added as "measured
        // rather than asserted" counters and then read by NOBODY: blk_stale_skips()
        // had ZERO callers in the whole kernel, and g_msc_noblock_spins had only
        // a one-shot kprintf even though its own comment claimed it was
        // "readable from the heartbeat". That is this project's recurring
        // zero-callers trap: the counter exists, so everyone assumes it is being
        // watched, and nobody is watching.
        //
        // They are printed UNCONDITIONALLY, including when zero, on purpose.
        // "blkstale=0" and a MISSING blkstale field mean completely different
        // things (measured-and-no-overlap vs this-kernel-never-measured-it),
        // and collapsing that distinction is exactly what went wrong here. Two
        // tokens on a line that already carries eight is not spam; the LOUD
        // reporting is the one-shot [BLKSTALE] line just below.
        extern uint64_t blk_stale_skips(void);
        extern volatile uint64_t g_msc_noblock_spins;
        uint64_t stale_now = blk_stale_skips();
        // One-shot-per-increase loud line, in the [TICKBURST] shape: a healthy
        // system stays quiet, and a system where the #617 race is actually live
        // says so in words rather than leaving a reader to notice a number moved.
        static uint64_t stale_reported = 0;
        if (stale_now > stale_reported) {
            stale_reported = stale_now;
            kprintf("[BLKSTALE] #617: %llu demand-cache install(s) declined - a "
                    "write landed on the root device while a read was in flight. "
                    "The generation guard is doing real work on this workload; "
                    "without it those sectors would have been cached STALE.\n",
                    (unsigned long long)stale_now);
        }
        // #632: the sys_fb_flip interrupts-off window and the net_poll drain it
        // used to contain. g_net_poll_* already existed and were already
        // incremented, but were surfaced only by the gated [DLPROF] line, which
        // stays silent unless a download is running: instrumented and unread,
        // the same zero-readers pattern as blk_stale_skips (#621). Printed here
        // UNCONDITIONALLY, including zero, so an idle desktop states its own
        // interrupt-off cost instead of leaving it to be assumed.
        {
            extern uint64_t g_net_poll_calls, g_net_poll_pkts, g_net_poll_max;
            extern volatile uint64_t g_flip_cli_max_cyc, g_flip_net_max_cyc;
            extern volatile uint64_t g_flip_cpy_max_cyc, g_flip_cli_over1ms;
            extern volatile uint64_t g_flip_net_calls;
            extern volatile uint64_t g_flip_cli_tot_cyc, g_flip_net_tot_cyc;
            extern volatile uint64_t g_flip_cpy_tot_cyc;
            extern volatile uint64_t g_fb_flip_count;
            uint64_t _khz = mono_tsc_khz(); if (!_khz) _khz = 1;
            // Per-INTERVAL, not lifetime: a lifetime maximum set once during
            // boot hides everything that happens afterwards, and a lifetime
            // total cannot be turned into a percentage of wall clock. The
            // maxima are read-and-reset so each line describes its own second.
            static uint64_t _p_ms, _p_flips, _p_cli, _p_net, _p_cpy, _p_pkts, _p_npoll;
            uint64_t _ms = mono_ms();
            uint64_t _dms = _ms - _p_ms; _p_ms = _ms;
            uint64_t _dflips = g_fb_flip_count - _p_flips; _p_flips = g_fb_flip_count;
            uint64_t _dcli = g_flip_cli_tot_cyc - _p_cli;  _p_cli = g_flip_cli_tot_cyc;
            uint64_t _dnet = g_flip_net_tot_cyc - _p_net;  _p_net = g_flip_net_tot_cyc;
            uint64_t _dcpy = g_flip_cpy_tot_cyc - _p_cpy;  _p_cpy = g_flip_cpy_tot_cyc;
            uint64_t _dpkts = g_net_poll_pkts - _p_pkts;   _p_pkts = g_net_poll_pkts;
            uint64_t _dnp = g_net_poll_calls - _p_npoll;   _p_npoll = g_net_poll_calls;
            // #COMPIDLE: the workload behind the cost. flips= alone cannot
            // tell a whole-screen present from a 40x16 clock rect, and on
            // real hardware those differ by four orders of magnitude in bus
            // traffic. bytes/s is the number that transfers between machines.
            extern uint64_t g_fb_front_bytes, g_fb_full_presents, g_fb_part_presents;
            static uint64_t _p_fbytes, _p_ffull, _p_fpart;
            uint64_t _dfbytes = g_fb_front_bytes   - _p_fbytes; _p_fbytes = g_fb_front_bytes;
            uint64_t _dffull  = g_fb_full_presents - _p_ffull;  _p_ffull  = g_fb_full_presents;
            uint64_t _dfpart  = g_fb_part_presents - _p_fpart;  _p_fpart  = g_fb_part_presents;
            uint64_t _climax = g_flip_cli_max_cyc; g_flip_cli_max_cyc = 0;
            uint64_t _netmax = g_flip_net_max_cyc; g_flip_net_max_cyc = 0;
            uint64_t _cpymax = g_flip_cpy_max_cyc; g_flip_cpy_max_cyc = 0;
            uint64_t _den = _khz * (_dms ? _dms : 1);   // cycles of one core this interval
            kprintf("[FLIPPROF] dt=%llums flips=%llu npoll=%llu pkts=%llu drainmax=%llu "
                    "| CLI-OFF %llu%% avg=%lluus max=%lluus over1ms=%llu "
                    "| net %llu%% max=%lluus n=%llu | cpy %llu%% max=%lluus "
                    "| px full=%llu part=%llu %lluKB/s\n",
                    (unsigned long long)_dms,
                    (unsigned long long)_dflips,
                    (unsigned long long)_dnp,
                    (unsigned long long)_dpkts,
                    (unsigned long long)g_net_poll_max,
                    (unsigned long long)(_dcli * 100ULL / _den),
                    (unsigned long long)(_dflips ? (_dcli * 1000ULL / _khz) / _dflips : 0),
                    (unsigned long long)(_climax * 1000ULL / _khz),
                    (unsigned long long)g_flip_cli_over1ms,
                    (unsigned long long)(_dnet * 100ULL / _den),
                    (unsigned long long)(_netmax * 1000ULL / _khz),
                    (unsigned long long)g_flip_net_calls,
                    (unsigned long long)(_dcpy * 100ULL / _den),
                    (unsigned long long)(_cpymax * 1000ULL / _khz),
                    (unsigned long long)_dffull,
                    (unsigned long long)_dfpart,
                    (unsigned long long)(_dms ? (_dfbytes * 1000ULL / _dms) / 1024ULL : 0));
        }
        // #745 (local 102): the measured cost of the rotated present copy,
        // MEASURED not asserted (ticket 102's own requirement). Silent on
        // every machine that is not rotated - fb_get_rotation() == NONE means
        // fb_present_rect_rotated() is never called, so there is nothing to
        // report and printing zeros every heartbeat would just be noise.
        if (fb_get_rotation() != FB_ROTATE_NONE) {
            static uint64_t _p_rot_tot, _p_rot_calls, _p_rot_px;
            uint64_t tot, mx, calls, px;
            fb_rotate_profile_get(&tot, &mx, &calls, &px);
            uint64_t _khz2 = mono_tsc_khz(); if (!_khz2) _khz2 = 1;
            uint64_t dtot = tot - _p_rot_tot;       _p_rot_tot = tot;
            uint64_t dcalls = calls - _p_rot_calls; _p_rot_calls = calls;
            uint64_t dpx = px - _p_rot_px;          _p_rot_px = px;
            // cyc/px scaled by 1000, same convention #642 uses for cyc/byte
            // above (pre2_mcpb/post_mcpb) - avoids needing a fixed-point type.
            uint64_t cpp_x1000 = dpx ? (dtot * 1000ULL) / dpx : 0;
            kprintf("[ROTPROF] rotation=%u calls=%llu avg=%lluus max=%lluus "
                    "px=%llu cyc/px=%llu.%03llu\n",
                    (unsigned)fb_get_rotation(),
                    (unsigned long long)dcalls,
                    (unsigned long long)(dcalls ? (dtot * 1000ULL / _khz2) / dcalls : 0),
                    (unsigned long long)(mx * 1000ULL / _khz2),
                    (unsigned long long)dpx,
                    (unsigned long long)(cpp_x1000 / 1000ULL),
                    (unsigned long long)(cpp_x1000 % 1000ULL));
        }
        // -------------------------------------------------------------------
        // #745 (task #62): CAN THE NETWORK STARVE THE DESKTOP?  Two lines that
        // turn "it feels laggy while the network is broken" into numbers you
        // can read off a serial capture on the real machine.
        //
        // [NETSTARVE] answers "did the desktop stall, and was it the network?"
        //   gapmax   - longest interval between two consecutive frame presents
        //              THIS interval. This is the user's symptom, in
        //              milliseconds. A healthy desktop is tens of ms; the
        //              reported fault is thousands.
        //   flipskip - presents that declined to pump the stack because
        //              net_lock was busy. BEFORE this change those presents
        //              blocked on that lock with interrupts off. Growing
        //              flipskip on a laggy box = network/UI contention is real;
        //              flipskip stuck at 0 while gapmax is huge = look
        //              elsewhere, it is not the network lock.
        //   lockwait - longest single blocking net_lock acquisition, in us.
        //              This is what a present USED to be able to pay.
        //   txmax    - longest single NIC transmit, in us. #549 made USB TX
        //              fire-and-forget after it had been measured at ~40ms per
        //              packet on the iMac dongle; this is the standing check
        //              that it stayed that way on real hardware. Tens of us =
        //              async as intended. Tens of thousands = a wait is back.
        //   pump     - pumps done by the dedicated netpump thread, and the
        //              wakeups it correctly declined as too early (a large
        //              early count means proc_sleep is collapsing, #577).
        //
        // [NETDIAG] answers "why is the network down at all?" - see
        // net_diag_line() in net/net.c.
        // -------------------------------------------------------------------
        {
            extern volatile uint64_t g_flip_gap_max_cyc, g_flip_net_skips;
            extern volatile uint64_t g_net_lock_wait_max_cyc, g_net_lock_contended;
            extern uint64_t g_nic_tx_ok, g_nic_tx_fail, g_nic_tx_max_cyc;
            extern uint64_t g_net_pump_runs, g_net_pump_early;
            extern int net_diag_line(char *buf, unsigned long len);
            uint64_t _khz2 = mono_tsc_khz(); if (!_khz2) _khz2 = 1;
            uint64_t _gap = g_flip_gap_max_cyc; g_flip_gap_max_cyc = 0;
            uint64_t _lw  = g_net_lock_wait_max_cyc; g_net_lock_wait_max_cyc = 0;
            uint64_t _tx  = g_nic_tx_max_cyc; g_nic_tx_max_cyc = 0;
            // #745 (task #69): holdmax is the FREEZE metric, lockwait is only
            // the contention metric. net_lock keeps interrupts off for the
            // whole hold, so on one core a holder is not preemptible and no
            // second thread can be running to spin: lockwait reads 0 while the
            // machine is frozen. holdmax cannot. holdra names the caller.
            extern volatile uint64_t g_net_lock_hold_max_cyc;
            extern volatile uint64_t g_net_lock_hold_max_ra;
            extern volatile uint64_t g_net_lock_over_budget, g_net_lock_holds;
            uint64_t _hm = g_net_lock_hold_max_cyc; g_net_lock_hold_max_cyc = 0;
            kprintf("[NETSTARVE] gapmax=%luus flipskip=%lu lockwait=%luus "
                    "holdmax=%luus holdra=%p holdn=%lu overbudget=%lu "
                    "txmax=%luus tx=%lu/%lu pump=%lu early=%lu contend=%lu\n",
                    (unsigned long)(_gap * 1000ULL / _khz2),
                    (unsigned long)g_flip_net_skips,
                    (unsigned long)(_lw * 1000ULL / _khz2),
                    (unsigned long)(_hm * 1000ULL / _khz2),
                    (void *)g_net_lock_hold_max_ra,
                    (unsigned long)g_net_lock_holds,
                    (unsigned long)g_net_lock_over_budget,
                    (unsigned long)(_tx * 1000ULL / _khz2),
                    (unsigned long)g_nic_tx_ok, (unsigned long)g_nic_tx_fail,
                    (unsigned long)g_net_pump_runs,
                    (unsigned long)g_net_pump_early,
                    (unsigned long)g_net_lock_contended);
            // #69: THE OUTER INTERRUPTS-OFF WINDOW, per site, as MAXus/AVGus/N.
            //
            // holdmax= above is the time net_lock itself was held. It is a
            // SUBSET of the time interrupts were off on every path that does
            // its own `cli` to switch CR3 for the NIC rings, and it is not even
            // a subset for the five socket.c sites, which take no net_lock at
            // all and so contribute ZERO to it. Read this line, not holdmax=,
            // to answer "how long can the network stop the timer tick".
            //
            // Compare the largest value here against holdmax= on the same line:
            // if they are close, net_lock IS the window and the lock is the
            // thing to fix; if this line is much larger, the lock was never the
            // problem and the `cli` around it is.
            {
                extern int irqwin_report(char *buf, unsigned long len);
                char iwbuf[320];
                int iwn = irqwin_report(iwbuf, sizeof(iwbuf));
                if (iwn > 0) kprintf("[NETIRQ] %s\n", iwbuf);
            }
            // #745 (task #69): the console's own cost, on the same line as the
            // network's, because until now nothing measured it and it is the
            // larger of the two on real hardware.
            {
                extern volatile uint64_t g_serial_tx_drops, g_serial_tx_max_us;
                extern volatile uint64_t g_serial_tx_suppressed;
                extern volatile int      g_serial_present;
                extern volatile uint32_t g_serial_lsr_boot, g_serial_lb_read;
                extern uint64_t g_tcp_bad_cksum;
                extern uint64_t g_icmp_rx, g_icmp_rx_reject;
                kprintf("[CONSOLE] present=%d lsr0=0x%02x lb=0x%02x "
                        "serdrop=%lu sersupp=%lu serus=%lu "
                        "| rx icmp=%lu/%lu tcpcksum=%lu\n",
                        g_serial_present,
                        (unsigned)(g_serial_lsr_boot & 0xFF),
                        (unsigned)(g_serial_lb_read & 0xFF),
                        (unsigned long)g_serial_tx_drops,
                        (unsigned long)g_serial_tx_suppressed,
                        (unsigned long)g_serial_tx_max_us,
                        (unsigned long)g_icmp_rx,
                        (unsigned long)g_icmp_rx_reject,
                        (unsigned long)g_tcp_bad_cksum);
            }
            // #155: 384, not 256. net_diag_line() grew the USB-Ethernet
            // pipeline counters (usbq/usbrx/usbdry/usbfd/usbrsy) and the line
            // was already ~200 chars: at 256 the new fields would have been
            // silently truncated by snprintf, which is exactly the failure mode
            // where you conclude the counters are zero.
            static char _ndbuf[384];
            if (net_diag_line(_ndbuf, sizeof(_ndbuf)) > 0)
                kprintf("[NETDIAG] %s\n", _ndbuf);
        }
        // #167: enq=refused/lost/dropped/fixed and sweep=.
        //   refused  owed enqueues the #75 funnel deferred
        //   lost     owed WAKES the drain dropped (a task deleted from the
        //            scheduler; must be 0)
        //   dropped  owed requeues the drain dropped (correct, task re-blocked)
        //   fixed    deferred wakes whose state transition the funnel performed
        //   sweep    wake_sleeping_procs() calls / sleepers woken
        //   now=     the sweep's OWN clock (sched_now_ms(), ms) and, sampled at
        //            the same instant, sched_ticks. #165 left "why are expired
        //            deadlines unwoken" open; if these two disagree the deadline
        //            arithmetic is against the wrong clock (blame.md:
        //            timer-ticks-is-not-a-wall-clock) and nothing else matters.
        //   left=    sleepers left asleep on the last pass / the EARLIEST
        //            deadline among them. left>0 with minleft BELOW now= is a
        //            contradiction and means the sweep is not reaching them.
        { extern volatile uint64_t g_enq_refused, g_enq_lost, g_enq_dropped;
          extern volatile uint64_t g_enq_wakefix, g_sweep_n, g_sweep_woke;
          extern volatile uint64_t g_defer_strand, g_reap_deferred;
          extern volatile uint64_t g_sweep_now, g_sweep_ticks, g_sweep_minleft;
          extern volatile uint32_t g_sweep_left;
        kprintf("[HB] tick=%lu uptime=%lus mono=%llums ctxsw=%lu flips=%lu hb=%lu blkc=%lu/%lu wqr=%lu promo=%lu e2w=%lu blkstale=%lu mscnb=%lu enq=%lu/%lu/%lu/%lu strand=%lu/%lu sweep=%lu/%lu now=%lums/st%lu left=%u/%lums %s\n",
                (unsigned long)ticks, (unsigned long)(ticks / hz), mono_ms(),
                (unsigned long)g_ctx_switches,
                (unsigned long)g_fb_flip_count, (unsigned long)n,
                (unsigned long)blkc_h, (unsigned long)blkc_m,
                (unsigned long)g_wq_unpark_rescues,
                (unsigned long)g_sched_promotions,
                (unsigned long)g_ext2_lock_waits,
                (unsigned long)stale_now,
                (unsigned long)g_msc_noblock_spins,
                (unsigned long)g_enq_refused, (unsigned long)g_enq_lost,
                (unsigned long)g_enq_dropped, (unsigned long)g_enq_wakefix,
                (unsigned long)g_defer_strand, (unsigned long)g_reap_deferred,
                (unsigned long)g_sweep_n, (unsigned long)g_sweep_woke,
                (unsigned long)g_sweep_now, (unsigned long)g_sweep_ticks,
                (unsigned)g_sweep_left, (unsigned long)g_sweep_minleft,
                topbuf); }
        dlprof_report();

        // #679: coalesced write-back of runtime permission changes (ownership
        // recorded at file-create time). No-ops unless the table is dirty; see
        // perms_sync_if_dirty() in fs/perms.c for why it lives on this thread.
        { // #693: the janitor thread is the only recipient; it logs and retries next tick
          if (perms_sync_if_dirty() != 0)
              kprintf("[PERMS] deferred sync failed; still dirty, will retry\n"); }
        // Use the LIGHTWEIGHT, constant-cost heartbeat writer (separate
        // /HEARTBEAT.TXT, bounded ring) instead of bootlog_write(). The old
        // bootlog_write() rewrote the whole growing /BOOTLOG.TXT every second,
        // which at USB-MSC speed grew to 4-27s per write and is the prime
        // suspect for wedging the iMac ~62s in (#373). bootlog_heartbeat() can
        // never grow without bound, so it cannot reproduce that starvation.
        if (bootlog_is_armed()) {
            // ---------------------------------------------------------------
            // #745 (task #62): THE ENRICHED HEARTBEAT RECORD.
            //
            // WHY. On the real iMac there is no serial console, so
            // /HEARTBEAT.TXT on the stick is the ONLY telemetry. The reported
            // fault is progressive: responsive at boot, unusable inside a
            // minute, getting worse throughout. That is an ACCUMULATION
            // signature - something is growing without bound - and the old
            // record could not name it. It proved the machine slowed (ctxsw,
            // flips, and top=) but carried no accumulator, so every candidate
            // had to be re-argued from source instead of read off the disk.
            //
            // Each field below exists to CONFIRM OR ELIMINATE one candidate,
            // and elimination is worth as much as confirmation here:
            //
            //   gap=    REAL milliseconds since the previous beat, from
            //           mono_ms() and never from timer_ticks (which is not a
            //           wall clock - KVM replays lost ticks in bursts). The
            //           nominal beat is 2000. #373's signature was this number
            //           climbing 4000 -> 27000 and then stopping. This is the
            //           single most important field: it says WHEN the machine
            //           started dying, and every other field on the same line
            //           is then read as "what was true at that moment".
            //   fgap=   longest gap between two frame PRESENTS in this
            //           interval. gap= is the kernel's own thread; fgap= is
            //           the user's actual symptom ("the cursor only responds
            //           one frame in a few seconds") as a number.
            //   procs=  live process count. A process leak makes every
            //           scheduler pass and every snapshot walk longer, which
            //           degrades progressively and looks exactly like this.
            //   heapKB= kernel heap in use. The generic leak detector: if this
            //           climbs monotonically the runaway is an allocation.
            //   tcp=    active TCP connection-table entries. A failing network
            //           retries; if failed connects leak entries, tcp_timer()
            //           (called from every pump loop) walks a growing table.
            //   blkw=   device WRITE calls, and blks= sectors. #373 was a
            //           write storm over USB-MSC. Nothing previously counted
            //           writes at all - blkc=h/m is the READ cache.
            //   blgKB=  bytes the LOGGING subsystem itself has sent to the
            //           medium. A diagnostic that cannot exonerate itself is
            //           not a diagnostic, and #373 was precisely the logger
            //           starving the machine it was logging.
            //   nskip=  frame presents that declined to service the network
            //           because net_lock was busy, and txus= the longest
            //           single NIC transmit. Together these settle the
            //           original network hypothesis: both near zero means the
            //           network did not starve the desktop, whatever else did.
            //
            // COST. All of these are already-maintained counters plus two
            // bounded table walks (proc_count, tcp_diag_snapshot) at 0.5 Hz.
            // The record is longer, which is why the ring grew to 16 KB.
            // ---------------------------------------------------------------
            extern uint32_t proc_count(void);
            extern size_t   heap_get_used_size(void);
            extern int      tcp_diag_snapshot(int *out_active, int *c, int nc);
            extern uint64_t g_blk_write_calls, g_blk_write_sectors;
            extern uint64_t g_bootlog_bytes_written;
extern uint64_t g_xhci_cmd_contended;      // #134
extern uint64_t g_xhci_cmd_noblock_refused; // #134
            // #745 (task #62): mscerr= is the trigger for the deadlock this
            // change closes, and bldef= is the guard firing. mscerr climbing
            // while the machine slows is the confirmation; mscerr staying 0
            // eliminates the whole hypothesis on that boot.
            extern uint64_t g_msc_xfer_errors, g_bootlog_flushes_deferred;
            extern volatile uint64_t g_flip_gap_max_hb, g_flip_net_skips;
            extern uint64_t g_nic_tx_max_hb;
            // #745 (task #69): the three fields that make a 30-second freeze
            // NAME ITSELF on a machine with no serial console.
            //   nhold=  longest net_lock hold this interval, in us. The lock
            //           keeps IF clear for its whole hold, so this is
            //           milliseconds of "the scheduler did not run", which is
            //           exactly why the user's "surely the scheduler fixes
            //           this" intuition does not apply. Compare it against
            //           fgap= on the same line: if nhold ~ fgap, the network
            //           lock IS the freeze; if nhold is small while fgap is
            //           huge, it is not, and that eliminates a whole family.
            //   nhra=   the return address that held it for nhold. addr2line
            //           against this build's kernel.elf.
            //   blgnb=  bootlog device writes refused because the caller had
            //           IRQs off. Non-zero means the storage-stack-under-
            //           net_lock path (the one that could wait unboundedly)
            //           really is being taken, and the guard is earning its
            //           keep rather than being dead code.
            //   serdrop= characters the UART refused to accept inside its
            //           bounded budget, and serus= the longest single-character
            //           wait. Before #745 task #69 that wait was capped at
            //           200,000 port-I/O reads PER CHARACTER, which is not a
            //           time bound at all: on hardware where TX never drains it
            //           was worth ~0.2s per character, and kprintf runs inside
            //           net_lock with interrupts off. A non-zero serdrop on a
            //           machine that stalls says the freeze was the console,
            //           not the network.
            //   ser=P/L/B  P = the loopback presence verdict (1 present, 0
            //           absent/dead), L = the RAW LSR byte read at serial_init
            //           BEFORE anything was written, B = the byte the loopback
            //           test read back where 0xAE was expected. THIS IS THE
            //           FIELD THAT ANSWERS "is the freeze the console?" on a
            //           machine with no serial console: 0xFF means the port
            //           floats (characters were dropped cheaply), 0x00 means it
            //           decodes but never sets THRE (every character was paying
            //           the full poll).
            extern volatile uint64_t g_net_lock_hold_max_hb, g_net_lock_hold_max_ra;
            extern uint64_t g_bootlog_noblock_defers;
            extern volatile uint64_t g_serial_tx_drops, g_serial_tx_max_us;
            extern volatile uint64_t g_serial_tx_suppressed;
            extern volatile int      g_serial_present;
            extern volatile uint32_t g_serial_lsr_boot, g_serial_lb_read;

            static uint64_t s_prev_beat_ms = 0;
            uint64_t _now_ms = mono_ms();
            uint64_t _gap = s_prev_beat_ms ? (_now_ms - s_prev_beat_ms) : 0;
            s_prev_beat_ms = _now_ms;

            int _tcpact = 0;
            (void)tcp_diag_snapshot(&_tcpact, 0, 0);
            uint64_t _khz3 = mono_tsc_khz(); if (!_khz3) _khz3 = 1;
            // Read-and-reset, so each record describes ITS OWN interval. A
            // lifetime maximum set once during boot hides everything after it.
            uint64_t _fgap = g_flip_gap_max_hb; g_flip_gap_max_hb = 0;
            uint64_t _txc  = g_nic_tx_max_hb;   g_nic_tx_max_hb  = 0;
            uint64_t _nhold = g_net_lock_hold_max_hb; g_net_lock_hold_max_hb = 0;

            // #745 (#62): the tick-source verdict for THIS window. Judged on
            // the NATIVE tick only (total minus any failover ticks), against
            // real mono milliseconds. This is the field that says whether the
            // machine has a working clock, and it is the one thing no other
            // field on this line can imply: every other counter here advances
            // fine on a machine whose desktop is frozen, because they are all
            // counted by whatever code does manage to run.
            char _tw[176];
            (void)tickwatch_poll(ticks, _now_ms, hz);
            if (tickwatch_hb_field(_tw, sizeof(_tw)) <= 0) _tw[0] = '\0';

            // #745 (#62): 1024, not 704. Worst-case expansion of the format
            // below is ~393 bytes of fixed fields plus up to 176 for the
            // tick-source field and 127 for topbuf, i.e. ~696 - which left
            // NINE bytes of margin. snprintf truncates SILENTLY, and topbuf
            // (`top=name:pct`, the field that names what is eating the CPU) is
            // last, so the first thing a too-small buffer would delete is the
            // diagnostic most likely to be wanted. Size it so the worst case
            // cannot reach it rather than so the typical case fits.
            char hb[1024];
            // #COMPIDLE: per-BEAT deltas, not lifetime totals - a lifetime
            // byte count cannot be turned into a rate, and a rate is the only
            // form of this number that can be compared between two machines.
            unsigned long _hb_fbKBs = 0, _hb_ffull = 0, _hb_fpart = 0;
            unsigned long _hb_fcpy = 0, _hb_fwc = 0;
            unsigned long _hb_fclius = 0, _hb_fcli1 = 0;
            {
                extern uint64_t g_fb_front_bytes, g_fb_full_presents, g_fb_part_presents;
                extern uint64_t g_fb_wc_bytes, g_fb_wc_span;
                extern volatile uint64_t g_flip_cpy_tot_cyc;
                // #COMPIDLE / audio boundary: the present holds interrupts OFF
                // for the whole back->front copy (#307 requires it - the copy
                // runs on the kernel CR3 while the caller is the compositor).
                // On the owner's 3840x2160 ASUS that copy moves 33.2 MB, and
                // his own boot log measures the pre-#642 rate at 9.77 cycles
                // per byte, so that window was about 125 ms with interrupts
                // masked, per present. #642 brings it to about 10 ms and the
                // tail-granule split to about 3.7 ms. At a 250 Hz tick, 10 ms
                // masked is 2.5 timer ticks LOST per present, which is the
                // documented mechanism behind "timer_ticks is not a wall
                // clock" and the audio pacing fault. [FLIPPROF] already
                // reports this to SERIAL, which does not exist on a laptop.
                // These two fields put it on the durable record so the number
                // can be read off the machine that has the problem.
                extern volatile uint64_t g_flip_cli_over1ms;
                static uint64_t _hp_cli1ms;
                static uint64_t _hp_bytes, _hp_full, _hp_part, _hp_cpy, _hp_ms;
                uint64_t _now_ms = mono_ms();
                uint64_t _d_ms   = _now_ms - _hp_ms; _hp_ms = _now_ms;
                uint64_t _d_by   = g_fb_front_bytes   - _hp_bytes; _hp_bytes = g_fb_front_bytes;
                uint64_t _d_fu   = g_fb_full_presents - _hp_full;  _hp_full  = g_fb_full_presents;
                uint64_t _d_pa   = g_fb_part_presents - _hp_part;  _hp_part  = g_fb_part_presents;
                uint64_t _d_cy   = g_flip_cpy_tot_cyc - _hp_cpy;   _hp_cpy   = g_flip_cpy_tot_cyc;
                uint64_t _k      = mono_tsc_khz(); if (!_k) _k = 1;
                if (_d_ms) {
                    _hb_fbKBs = (unsigned long)((_d_by * 1000ULL / _d_ms) / 1024ULL);
                    _hb_fcpy  = (unsigned long)(_d_cy * 100ULL / (_k * _d_ms));
                }
                _hb_ffull = (unsigned long)_d_fu;
                _hb_fpart = (unsigned long)_d_pa;
                _hb_fwc   = g_fb_wc_span
                          ? (unsigned long)(g_fb_wc_bytes * 100ULL / g_fb_wc_span) : 0;
                // Longest single interrupts-off present window this beat, in
                // microseconds, read-and-reset so each record describes its
                // own interval, plus the running count of presents that held
                // interrupts off for more than a millisecond.
                {
                    extern volatile uint64_t g_flip_cli_max_hb_cyc;
                    uint64_t _cm = g_flip_cli_max_hb_cyc; g_flip_cli_max_hb_cyc = 0;
                    _hb_fclius = (unsigned long)(_cm * 1000ULL / _k);
                    _hb_fcli1 = (unsigned long)(g_flip_cli_over1ms - _hp_cli1ms);
                    _hp_cli1ms = g_flip_cli_over1ms;
                }
            }
            snprintf(hb, sizeof(hb),
                     "[HB] tick=%lu uptime=%lus gap=%lums ctxsw=%lu flips=%lu "
                     "fgapus=%lu hb=%lu blkc=%lu/%lu procs=%lu heapKB=%lu "
                     "tcp=%lu blkw=%lu blks=%lu stgstl=%lu blgKB=%lu nskip=%lu txus=%lu "
                     "mscerr=%lu bldef=%lu nhold=%luus nhra=%p blgnb=%lu "
                     "xcmd=%lu/%lu blgdrop=%lu fltlost=%lu "
                     // #COMPIDLE: the frame workload, DURABLY. Serial is
                     // silent in GUI mode, so [FLIPPROF] cannot be read on
                     // the owner's laptop; /HEARTBEAT.TXT can. fbKBs is
                     // bytes/s written to the FRONT buffer (on real hardware,
                     // across the display aperture), ffull/fpart split those
                     // presents by kind, fcpy is the percentage of ONE core
                     // spent inside the back->front copy, and fwc is the
                     // percentage of the front buffer that actually carries a
                     // write-combining memory type. Those four answer "is the
                     // desktop presenting at idle, how much, and is it paying
                     // uncached prices for it" without a serial cable.
                     "fbKBs=%lu ffull=%lu fpart=%lu fcpy=%lu%% fwc=%lu%% "
                     "fclius=%lu fcli1ms=%lu "
                     "ser=%d/0x%02x/0x%02x serdrop=%lu sersupp=%lu serus=%lu %s %s",
                     (unsigned long)ticks,
                     (unsigned long)(ticks / hz),
                     (unsigned long)_gap,
                     (unsigned long)g_ctx_switches,
                     (unsigned long)g_fb_flip_count,
                     (unsigned long)(_fgap * 1000ULL / _khz3),
                     (unsigned long)n,
                     (unsigned long)blkc_h, (unsigned long)blkc_m,
                     (unsigned long)proc_count(),
                     (unsigned long)(heap_get_used_size() / 1024),
                     (unsigned long)_tcpact,
                     (unsigned long)g_blk_write_calls,
                     (unsigned long)g_blk_write_sectors,
                     // #DIAGLOG: block write-staging THEFT, per heartbeat. The
                     // once-per-boot [BLKSTAGE] line says whether the guard is
                     // installed; this says whether it has caught anything
                     // SINCE, which is the half a stick brought back off a
                     // laptop can answer. MUST be 0 (see blame.md, stagesb).
                     (unsigned long)g_blk_seal_broken,
                     (unsigned long)(g_bootlog_bytes_written / 1024),
                     (unsigned long)g_flip_net_skips,
                     (unsigned long)(_txc * 1000ULL / _khz3),
                     (unsigned long)g_msc_xfer_errors,
                     (unsigned long)g_bootlog_flushes_deferred,
                     (unsigned long)(_nhold * 1000ULL / _khz3),
                     (void *)g_net_lock_hold_max_ra,
                     (unsigned long)g_bootlog_noblock_defers,
                     // #134: xcmd=<contended>/<refused> is the measurement that
                     // says whether two threads really do issue xHCI controller
                     // commands at the same time on THIS machine. It was
                     // unserialised until #134 and the reported unplug freeze is
                     // the shape that race produces. blgdrop/fltlost say whether
                     // the breadcrumb files themselves lost anything, so a short
                     // /BOOTLOG.TXT can be told apart from a machine that
                     // stopped.
                     (unsigned long)g_xhci_cmd_contended,
                     (unsigned long)g_xhci_cmd_noblock_refused,
                     (unsigned long)bootlog_dropped_bytes(),
                     (unsigned long)bootlog_fault_lost(),
                     (unsigned long)_hb_fbKBs,
                     (unsigned long)_hb_ffull,
                     (unsigned long)_hb_fpart,
                     (unsigned long)_hb_fcpy,
                     (unsigned long)_hb_fwc,
                     (unsigned long)_hb_fclius,
                     (unsigned long)_hb_fcli1,
                     g_serial_present,
                     (unsigned)(g_serial_lsr_boot & 0xFF),
                     (unsigned)(g_serial_lb_read & 0xFF),
                     (unsigned long)g_serial_tx_drops,
                     (unsigned long)g_serial_tx_suppressed,
                     (unsigned long)g_serial_tx_max_us,
                     _tw,
                     topbuf);
            bootlog_heartbeat(hb);

            // #745 (task #62): A FIRST USB-MSC TRANSFER ERROR MUST REACH THE
            // DISK. The ring normally persists on a 30-minute schedule or on
            // the late-beat anomaly, which is right for a slow decline but can
            // lose the FIRST error - the trigger for the log-recursion
            // deadlock this change closes, and the single most valuable record
            // in the file. Flush the moment the count moves. This runs on the
            // heartbeat thread, which is never inside the storage stack, so it
            // cannot re-enter the guard it is reporting on. Rate-limited by
            // construction: the count moves at most once per beat here.
            {
                static uint64_t s_prev_mscerr = 0;
                if (g_msc_xfer_errors != s_prev_mscerr) {
                    s_prev_mscerr = g_msc_xfer_errors;
                    (void)bootlog_heartbeat_flush();
                }
            }
        }

#ifdef TICKWATCH_FAULT_TEST
        // #745 (#62) FAULT INJECTION (`make TICKFAULT=1`, never in a golden).
        // Once the desktop is well up, MASK IRQ0 at the 8259. That reproduces
        // the iMac symptom exactly - a PIT that is programmed and counting,
        // whose interrupt never reaches the CPU - on hardware that would
        // otherwise never fail, because QEMU always delivers.
        //
        // The deadline is REAL time from mono_ms(), not timer_ticks: the whole
        // point of the test is that timer_ticks is about to stop, and a
        // tick-derived deadline would then never be reached again, so the
        // injector would look like it had failed to fire.
        {
            static int s_injected = 0;
            if (!s_injected && mono_ms() >= 90000ULL) {
                s_injected = 1;
                tickwatch_fault_inject_kill_irq0();
            }
        }
#endif
        proc_sleep(2000);   // ~2 s between heartbeats (lighter USB-MSC load)
    }
}

// Kernel main entry point
// Called from entry.asm with boot_info pointer in RDI
// #624 step 2: no_stack_protector is REQUIRED, not cosmetic. kernel_main is
// still on the stack when security_canary_init() replaces __stack_chk_guard,
// and GCC's epilogue re-reads the global to compare against the copy its
// prologue saved. A protected kernel_main would therefore trip a FALSE
// stack-smash panic the moment it returned. It never returns in practice, but
// depending on that would be depending on an accident.
__attribute__((no_stack_protector))
void kernel_main(boot_info_t *boot_info) {
    // #ASUSDIAG: TAKE THE SCREEN BEFORE ANYTHING ELSE CAN GO WRONG.
    //
    // This is the FIRST executable statement of the kernel, ahead even of
    // serial_init(), and it is here because of a hard property of the target
    // hardware: a LAPTOP HAS NO SERIAL PORT. Every diagnostic below goes to
    // serial via kprintf, and every persistent copy of it (/BOOTLOG.TXT,
    // /USBLOG.TXT, /boot/STAGE.TXT, /DEVLOG.TXT) is gated on a filesystem being
    // mounted, which does not happen until line ~1100 at the earliest. So on a
    // laptop, everything between here and there currently writes NOTHING,
    // ANYWHERE, and a death in that window is indistinguishable from a brick.
    // That is the same gap that made the iMac14,4 undiagnosable for days.
    //
    // The UEFI GOP framebuffer is already live: the bootloader captured its
    // address, geometry and pixel format into boot_info before ExitBootServices,
    // and vmm_init() never replaces the firmware page tables (it adopts them),
    // so the aperture the console will use at line ~961 is writable right now.
    // early_fb_init() therefore needs no heap, no PMM, no IDT and no console.
    //
    // It is ordered before serial_init() deliberately: serial_init() is
    // straight-line port I/O with no loops, so nothing here can hang, and
    // putting the screen first means a machine that dies in the very next
    // statement still SAYS SO on the glass.
    //
    // #QUIETBOOT: AND IT IS OFF BY DEFAULT. Everything in the paragraph above is
    // still true, and none of it is a reason to put a diagnostic page in front
    // of every ordinary user. The SCREEN half is armed only by \boot\DIAG.TXT
    // on the ESP, which the bootloader read before ExitBootServices and handed
    // over in boot_info->diag_flags. The PERSISTENT half (/BOOTLOG.TXT, the
    // raw-LBA flight recorder, serial) is not gated at all and records this boot
    // exactly as it did before. See fs/bootstage.h.
    {
        extern int early_fb_init(uint64_t addr, uint32_t w, uint32_t h,
                                 uint32_t pitch, uint32_t bpp, uint32_t pixfmt,
                                 uint32_t arm);
        // Read the flag DEFENSIVELY: a null boot_info, or a magic that has not
        // been checked yet (it is checked ~100 lines below), must not be able to
        // turn the screen diagnostics on by accident. Absent evidence = quiet.
        int diag_on = (boot_info && boot_info->magic == BOOT_INFO_MAGIC &&
                       (boot_info->diag_flags & BOOT_DIAG_SCREEN)) ? 1 : 0;
        boot_stage_diag_set(diag_on);
        if (boot_info && boot_info->framebuffer.address != 0) {
            early_fb_init(boot_info->framebuffer.address,
                          boot_info->framebuffer.width,
                          boot_info->framebuffer.height,
                          boot_info->framebuffer.pitch,
                          boot_info->framebuffer.bpp,
                          boot_info->framebuffer.pixel_format,
                          (uint32_t)diag_on);
        }
    }

    // Initialize serial port first for debugging
    {
        // #745 (task #69): CONSUME THE PRESENCE VERDICT. This call has always
        // returned -1 for an absent or dead UART and the result has always been
        // discarded; serial.c now latches it, and this says so out loud once.
        // The kprintf is harmless either way: if there is no UART it goes
        // nowhere, and the same facts ride every [HB] record as ser=P/L/B.
        extern volatile uint32_t g_serial_lsr_boot, g_serial_lb_read;
        int _ser = serial_init(COM1, 115200);
        kprintf("[SERIAL] probe: rc=%d lsr0=0x%02x loopback_read=0x%02x -> %s\n",
                _ser, (unsigned)(g_serial_lsr_boot & 0xFF),
                (unsigned)(g_serial_lb_read & 0xFF),
                _ser == 0 ? "UART PRESENT" : "NO UART (output latched off)");
    }

    // #ASUSDIAG: the first three checkpoints. From here every boot step names
    // itself on the screen, in the RAM log, and (once the block device answers)
    // on raw sectors of the boot medium. See fs/bootstage.h.
    // #QUIETBOOT: SAY WHICH MODE THIS BOOT IS IN, IN ONE LINE, ON EVERY BOOT.
    //
    // A marker-gated diagnostic that never says whether its marker is present
    // is how /CDTEST.TXT and /IMG193.TXT became dead code that passes every
    // dead-code check: both harnesses are compiled, linked and called on every
    // boot, and both return at their first line because the file that arms them
    // is on no image and in no manifest. Nothing anywhere reports that. One
    // line costs nothing and makes the whole class impossible to miss.
    //
    // bootlog_write() and NOT kprintf(): bootlog_write already mirrors to
    // serial, so one event produces one serial line, and this line also lands
    // in /BOOTLOG.TXT where the quiet boots that matter can be read back.
    bootlog_write("[DIAG] %s",
                  boot_stage_diag_armed()
                      ? "ARMED via /boot/DIAG.TXT: on-screen boot diagnostics ON"
                      : "quiet (no /boot/DIAG.TXT): on-screen boot diagnostics OFF, "
                        "persistent logs unaffected");

    boot_stage(BSTAGE_ENTRY);
    boot_stage(BSTAGE_SERIAL);
    {
        // #QUIETBOOT: early_fb_valid(), NOT early_fb_ready(). "Quiet" and
        // "blind" are different facts about the machine: valid means the screen
        // COULD be used as a diagnostic channel, ready means we are painting on
        // it. Testing ready here would write "EARLY FRAMEBUFFER UNAVAILABLE -
        // running blind" into /BOOTLOG.TXT on every ordinary boot, which is a
        // lie, and the one place it would be believed is the boot where it is
        // true. The geometry note itself is not gated: it costs one buffered
        // line and it is exactly what a mis-rendered panel needs.
        extern int early_fb_valid(void);
        if (early_fb_valid()) {
            boot_stage(BSTAGE_EARLYFB);
            boot_stage_note("fb %ux%u pitch=%u bpp=%u fmt=%u @0x%llx",
                            boot_info ? boot_info->framebuffer.width : 0,
                            boot_info ? boot_info->framebuffer.height : 0,
                            boot_info ? boot_info->framebuffer.pitch : 0,
                            boot_info ? boot_info->framebuffer.bpp : 0,
                            boot_info ? boot_info->framebuffer.pixel_format : 0,
                            (unsigned long long)(boot_info ? boot_info->framebuffer.address : 0));
        } else {
            // The one case this instrumentation cannot cover. Say it out loud
            // rather than leaving a silent absence of evidence: with no usable
            // GOP mode there is no channel at all on a machine with no UART.
            boot_stage_note("EARLY FRAMEBUFFER UNAVAILABLE - running blind");
        }
    }

    // #672: FIRST diagnostic of the boot, deliberately. Everything printed
    // after this line is read as evidence, and until #672 the formatter could
    // silently shift a line's arguments (printing one field's value under
    // another field's label, and handing an integer to a %s to dereference).
    // Proving the formatter before trusting anything it prints is the only
    // order that makes sense.
    kformat_selftest();
    boot_stage(BSTAGE_FORMATTER);

    // #ASUSDIAG: the flight recorder's record layout, CRC and dirty-sector
    // arithmetic, proven on THIS build before anything depends on it. Pure
    // arithmetic over stack buffers: it touches no device and needs no mount,
    // so it can run here, long before there is a medium to arm.
    // BOTH numbers are printed on purpose. A PASS with zero checks is vacuous,
    // and this is the failure mode a self-test nobody has watched go red
    // actually has; fltrec_selftest() returns -1 if zero assertions ran, which
    // is the right way round. Build with FLTTESTFAIL=1 to see it go red.
    {
        extern int fltrec_selftest(uint32_t *checks);
        uint32_t fchecks = 0;
        int frc = fltrec_selftest(&fchecks);
        kprintf("[FLTREC] selftest %s (%u checks)\n", frc == 0 ? "PASS" : "FAIL", fchecks);
        bootlog_write("[FLTREC] selftest %s checks=%u", frc == 0 ? "PASS" : "FAIL", fchecks);
    }

    // #624 step 2: install the real per-boot stack canary IMMEDIATELY, while
    // the call stack is only entry.asm -> kernel_main (both unprotected), so no
    // protected frame is holding the old value. Everything compiled after this
    // point runs against a random 56-bit canary instead of the compile-time
    // placeholder. Deliberately before almost all other init: a canary that is
    // installed late protects nothing that ran early.
    { extern void security_canary_init(void); security_canary_init(); }
    boot_stage(BSTAGE_CANARY);

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

    boot_stage(BSTAGE_BOOTINFO);
    // #ASUSDIAG: the bootloader keeps its OWN copy of boot_info_t and the two
    // are kept in sync only by convention, with no shared header. Print the
    // kernel's sizeof next to the bootloader's console line so a drift between
    // the two copies is visible in ONE boot instead of presenting as garbage in
    // whichever field happens to sit past the divergence.
    boot_stage_note("boot_info: sizeof=%u magic=%s",
                    (unsigned)sizeof(boot_info_t),
                    (boot_info && boot_info->magic == BOOT_INFO_MAGIC) ? "OK" : "BAD");

    // Print system information
    kprintf("\n[MEMORY] Total RAM: %lu MB\n", boot_info->total_memory / MB);
    kprintf("[MEMORY] Memory map entries: %u\n", boot_info->memory_map_entries);

    // Print framebuffer info
    int spinner_frame = 0;  // For boot spinner animation
    if (boot_info->framebuffer.address != 0) {
        print_framebuffer_info();
    }

    // #modeset: report the video-mode selection UNCONDITIONALLY, outside the
    // framebuffer.address gate above. Two reasons it is not folded into
    // print_framebuffer_info():
    //  - address == 0 is precisely the black-screen case, and it is the case
    //    where knowing what the loader tried to do matters MOST, so a report
    //    that is skipped exactly then is the wrong shape.
    //  - one line always printed, armed or not, is the whole discipline here.
    //    diskimg_boot_harness() and img_shadow_selftest() are compiled, linked
    //    and CALLED every boot and both return at their first line because
    //    their marker files exist on no image; every dead-code check passes
    //    because the CALLER runs. Silence when unarmed is what made those two
    //    indistinguishable from working code for months.
    print_video_mode_info();

    // Print ACPI info
    if (boot_info->acpi.rsdp_address != 0) {
        kprintf("\n[ACPI] RSDP at 0x%lx (version %u)\n",
                boot_info->acpi.rsdp_address, boot_info->acpi.rsdp_version);
    }

    // Initialize CPU subsystem
    kprintf("\n");
    boot_stage(BSTAGE_GDT);  gdt_init();     // Global Descriptor Table
    boot_stage(BSTAGE_IDT);  idt_init();     // Interrupt Descriptor Table
    boot_stage(BSTAGE_PIC);  pic_init();     // Programmable Interrupt Controller
    boot_stage(BSTAGE_PIT);  pit_init(250);  // 250 Hz default, games can boost to 1000
    // #525: start THE shared monotonic clock (cpu/mono.h) the moment the PIT
    // is programmed. It must be ready before usb_init() enumerates the xHCI,
    // whose transfer deadlines depend on it; calibration reads PIT channel 0's
    // counter directly, so it works here with interrupts still off. Deadlines
    // must never be measured in timer_ticks: ticks count DELIVERY, not TIME,
    // and KVM reinjects a starved vCPU's missed ticks in a ~1250-tick burst
    // (a nominal 5s at 250Hz) in ~15ms of real time (#524/#499).
    {
        // #ASUSDIAG: this is the longest step before anything reaches the
        // screen, and on a machine with no working 8254 it used to be able to
        // spend MINUTES here with nothing displayed. It is now probed first and
        // has a CPUID fallback (rustkern/mono.rs), but it still gets its own
        // checkpoint so that if it ever is the slow step, the screen says so.
        boot_stage(BSTAGE_MONO);
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
        // #ASUSDIAG: say WHICH source produced the number and what the 8254
        // actually did. Two independent sources that agree are evidence; one
        // number with nothing to check it against is how a varying-garbage PIT
        // hands the whole kernel a silently wrong clock.
        {
            extern uint32_t mono_pit_state_rs(void);
            extern uint32_t mono_source_rs(void);
            extern uint64_t mono_cpuid_khz_rs(void);
            extern uint64_t mono_pit_khz_rs(void);
            static const char *const pitst[4] = { "COUNTING", "FROZEN", "FLOATING(no 8254)", "UNPROBED" };
            static const char *const srcn[4]  = { "NONE", "PIT-8254", "CPUID-0x15", "CPUID-0x16" };
            uint32_t ps = mono_pit_state_rs(); if (ps > 3) ps = 3;
            uint32_t sc = mono_source_rs();    if (sc > 3) sc = 0;
            kprintf("[MONO] pit=%s source=%s pit_khz=%llu cpuid_khz=%llu\n",
                    pitst[ps], srcn[sc],
                    (unsigned long long)mono_pit_khz_rs(),
                    (unsigned long long)mono_cpuid_khz_rs());
            boot_stage_note("clock: pit=%s src=%s pit=%lluk cpuid=%lluk",
                            pitst[ps], srcn[sc],
                            (unsigned long long)mono_pit_khz_rs(),
                            (unsigned long long)mono_cpuid_khz_rs());
        }
    }
    boot_stage(BSTAGE_ISR);     isr_init();     // Interrupt Service Routines
    boot_stage(BSTAGE_SSE);     sse_init();     // SSE/FPU support
    boot_stage(BSTAGE_SYSCALL); syscall_init(); // SYSCALL/SYSRET support
    { extern void smp_cpu_local_init(uint32_t); smp_cpu_local_init(0); } // #279 3b-1.5 BSP GS base

    // Initialize memory management
    kprintf("\n");
    boot_stage(BSTAGE_PMM);
    boot_stage_note("memmap: %u entries of %u bytes, %llu MB usable, %llu dropped",
                    boot_info->memory_map_entries, boot_info->memory_map_entry_size,
                    (unsigned long long)(boot_info->total_memory / MB),
                    (unsigned long long)boot_info->memory_map_dropped);
    // A dropped descriptor is not cosmetic: it is RAM, or an MMIO range, that
    // the physical allocator is about to be told does not exist. Say so loudly
    // rather than leaving it as a number in a line nobody reads.
    if (boot_info->memory_map_dropped) {
        boot_stage_note("WARNING: bootloader dropped %llu memory-map descriptors",
                        (unsigned long long)boot_info->memory_map_dropped);
    }
    pmm_init(boot_info->memory_map_address, boot_info->memory_map_entries);
    boot_stage(BSTAGE_VMM);   vmm_init();     // Virtual Memory Manager
    boot_stage(BSTAGE_HEAP);  heap_init();    // Kernel Heap
    // #429 (restored b713): demand paging / COW init, dropped in the churn.
    boot_stage(BSTAGE_DEMAND);
    { extern void demand_init(void); demand_init(); }

    // Initialize graphics subsystem
    kprintf("\n");
    if (boot_info->framebuffer.address != 0) {
#ifdef DIAG_SELF_DEMO
        // `make DIAG_SELF_DEMO=1` builds a THROWAWAY kernel that can be WATCHED
        // doing the two things this instrumentation exists for. It is not in any
        // shipping image and it is not a debug flag: it is the same argument as
        // RTCLKTESTFAIL / KTZTESTFAIL / CFGTESTFAIL / FLTTESTFAIL elsewhere in
        // this tree. A diagnostic nobody has SEEN work is indistinguishable from
        // one that does not, and the early-boot window is far too short to catch
        // with a screendump: on a VM everything from kernel_main to console_init
        // takes well under a second.
        //
        // Part 1: hold here, with the early framebuffer still owning the screen,
        // so the stage list and the bottom banner can actually be looked at
        // before gfx_boot_simple() paints the splash over them.
        boot_stage_note("DIAG_SELF_DEMO: holding 12s so this screen can be seen");
        for (int _d = 0; _d < 120; _d++) mono_busy_delay_us(100000);
#endif
        boot_stage(BSTAGE_CONSOLE);
        console_init(&boot_info->framebuffer);
        // #ASUSDIAG: from here the kernel's own boot-log console owns the
        // screen, so checkpoints route through gfx_boot_log() and the early
        // banner is repainted AFTER each fb_swap_buffers() rather than being
        // wiped by it. See fs/bootstage.c.
        boot_stage_console_live();
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
    boot_stage(BSTAGE_PCI);
    pci_init();
    boot_stage_note("PCI: %d function(s) recorded", pci_get_device_count());
    syslog_log(LOG_INFO, "PCI bus scan complete");

    // Identify the Intel integrated GPU, if present. DETECTION ONLY: this
    // reads PCI config values pci_init() already fetched and prints them.
    // It performs no MMIO, maps no BAR and writes no display register, so
    // it cannot affect the UEFI GOP framebuffer the console and compositor
    // are using. See drivers/intel_gpu_detect.c for why it stops there, and
    // the audit block in drivers/intel_gpu.c for why intel_gpu_init() is
    // NOT called (it does not terminate when the first display device is
    // not Intel, which is every QEMU VM).
    boot_stage(BSTAGE_GPU);
    { extern void intel_gpu_detect(void); intel_gpu_detect(); }
    gfx_boot_spinner(spinner_frame++);

    // Initialize storage drivers
    gfx_boot_spinner(spinner_frame++); // Storage init
    // #ASUSDIAG: AHCI probing is bounded but SLOW on unfamiliar silicon (up
    // to a few hundred ms per implemented port, and a controller can report
    // 32 of them). Its own checkpoint, so a long pause here reads as "still
    // probing storage" rather than "hung".
    boot_stage(BSTAGE_ATA);
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
    // #763: usb_hid_init() had ZERO CALLERS. It was declared in usb_hid.h,
    // defined in usb_hid.c, and never invoked by anything, which is why its
    // one-line boot output had never been seen in a log. Nothing had broken
    // because its two jobs were both covered by accident: the device table is
    // a file static (already zero) and usb_hid_attach() re-wires the key
    // callback with the comment "ensure the sink is wired". An init function
    // that is only correct because nothing needed it is one edit away from
    // being wrong, so it is called now, and it is called BEFORE usb_init()
    // enumerates: it clears the device table, so calling it afterwards would
    // erase the very devices enumeration just found.
    { extern void usb_hid_init(void); usb_hid_init(); }
    boot_stage(BSTAGE_USB);
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
    boot_stage(BSTAGE_USB_DONE);
    boot_stage(BSTAGE_FATINIT);
    fat_init();   // trivial (prints banner); the FAT driver has no other state

    // #418 FAKE-audit CRITICAL fix: hotplug_init() (drivers/hotplug.c) registers
    // hotplug_handle_usb_event() with the USB-MSC driver so a USB drive plugged
    // in AFTER boot gets auto-mounted and a desktop icon added. It only registers
    // a callback and touches no disk itself, so it is safe to call here right
    // after usb_init() (xHCI is up) and fat_init() (FAT/FS types are ready).
    hotplug_init();

    boot_stage(BSTAGE_USBROOT);
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
            // #ASUSDIAG: ARM THE RAW-LBA FLIGHT RECORDER HERE, AND THE
            // "HERE" IS LOAD-BEARING.
            //
            // fltrec_arm() proves the medium is really writable by writing its
            // superblock to LBA 34 and READING IT BACK. At this point the block
            // layer is still BLK_MODE_OFF, so that readback is a real SCSI
            // READ(10) and the comparison means something. After
            // blk_root_to_ram() further down, blk_write() has just installed
            // those same bytes into the RAM copy, so the readback would be
            // served from RAM and would agree with itself EVEN IF THE DEVICE
            // SILENTLY DROPPED THE WRITE. Arming late would not break the
            // recorder; it would silently vacate the one check that proves the
            // recorder works, which is the same shape of fault as a self-test
            // that cannot go red.
            //
            // It is also before the three-attempt fat_mount_lba() retry below,
            // so a failure in the MOUNT PATH itself, which is a likely outcome
            // on an unfamiliar USB controller, still leaves a record.
            //
            // Safe to run before the root is proven to be ours: fltrec_arm()
            // refuses unless LBA 1 holds a valid GPT header and every non-empty
            // partition starts at or after LBA 2048. An MBR disk is refused
            // outright and deliberately, because that gap is where GRUB embeds
            // core.img. Declining prints the reason and writes nothing.
            {
                extern int fltrec_arm(void);
                int fr = fltrec_arm();
                boot_stage_note("flight recorder: %s (usb msc %d)",
                                fr ? "ARMED at LBA 34" : "declined", ui);
            }
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
    boot_stage(BSTAGE_ACPI);
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
        (void)smp_start_aps; (void)smp_selftest;   // used at the #67 site below
        boot_stage(BSTAGE_MADT);
        madt_init();   // parse MADT (CPU list) - was never called
        boot_stage(BSTAGE_SMPINIT);
        // #67: smp_init() (LAPIC bring-up, needed by #71's HDA MSI) still runs
        // HERE. The AP START has MOVED to after the FAT root is mounted, so the
        // /SMPSCHED.TXT escape hatch is readable and one kernel binary can be
        // tested with AP user scheduling both on and off. This changes nothing
        // in the shipping build: g_smp_user_sched is 0, so smp_start_aps() was
        // already unreachable at this point (see cpu/smp.c and the
        // concurrency-lint allowlist, which both record it).
        smp_init();
    }
    // #745 (#62): arm the REDUNDANT tick source now that the Local APIC is up.
    // Unconditional and always-armed, which is CLAUDE.md's preferred shape for
    // a wake that must never be lost. On this machine the wake in question is
    // the system clock itself: the owner's iMac14,4 shows a desktop that only
    // advances while something is running, which is what a machine looks like
    // when its one periodic interrupt is not arriving. The PIT stays primary;
    // this source costs one comparison per interrupt while the PIT is healthy
    // and only advances time when the PIT stops.
    { extern void tick_redundant_arm(void); tick_redundant_arm(); }
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
    // #115: prove the ONE calendar-time converter on THIS build before any
    // filesystem stamps a file with it. Prints the number of assertions that
    // actually ran, so "passed" is distinguishable from "never ran" (the
    // zero-callers lesson in blame.md).
    {
        boot_stage(BSTAGE_SELFTEST);
        extern int32_t ktime_selftest_rs(uint32_t *out_checks);
        extern int64_t wallclock_now_unix_rs(void);
        uint32_t kchecks = 0;
        int32_t krc = ktime_selftest_rs(&kchecks);
        int64_t now = wallclock_now_unix_rs();
        kprintf("[KTIME] converter selftest: %s (%u checks); RTC wall clock = %lld\n",
                krc == 0 ? "PASS" : "FAIL", kchecks, (long long)now);
        bootlog_write("[KTIME] selftest %s checks=%u wallclock=%lld",
                      krc == 0 ? "PASS" : "FAIL", kchecks, (long long)now);

        // #113: the realtime clock that time()/gettimeofday() now sit on.
        // Printed next to the raw RTC read above ON PURPOSE: the two numbers
        // come from different code paths (a direct CMOS read vs the anchored
        // TSC extrapolation) and must agree to within a second. If they ever
        // disagree, the instrument says so here rather than every app quietly
        // disagreeing about what time it is.
        //
        // delta_us is how long the selftest itself took. MEASURED on a real
        // boot it is 0: 23 integer assertions complete inside one microsecond,
        // so this number does NOT demonstrate sub-second advancement and must
        // not be quoted as if it did. (An earlier version of this comment
        // claimed exactly that, and the first boot falsified it.) The actual
        // sub-second proof is /APPS/t113.elf, which samples gettimeofday() 400
        // times across a real interval: 399 of 399 steps were sub-second on the
        // fixed kernel against 3 of 399 on the old one. What delta_us IS good
        // for is catching a clock that JUMPS during those two adjacent reads.
        extern int32_t rtclock_selftest_rs(uint32_t *out_checks);
        extern int64_t realtime_us_rs(void);
        int64_t rt0 = realtime_us_rs();
        uint32_t rchecks = 0;
        int32_t rrc = rtclock_selftest_rs(&rchecks);
        int64_t rt1 = realtime_us_rs();
        kprintf("[RTCLK] selftest %s (%u checks); epoch_us=%lld sec=%lld "
                "delta_us=%lld rtc_sec=%lld\n",
                rrc == 0 ? "PASS" : "FAIL", rchecks, (long long)rt1,
                (long long)(rt1 / 1000000), (long long)(rt1 - rt0),
                (long long)now);
        bootlog_write("[RTCLK] selftest %s checks=%u epoch_us=%lld sec=%lld "
                      "delta_us=%lld rtc_sec=%lld",
                      rrc == 0 ? "PASS" : "FAIL", rchecks, (long long)rt1,
                      (long long)(rt1 / 1000000), (long long)(rt1 - rt0),
                      (long long)now);

        // #86: the kernel's TZ.CFG parser, plus what it actually read off THIS
        // machine's disk. The offset and the is-set flag are printed
        // SEPARATELY on purpose: "offset 0" and "no zone configured" are
        // different states that look identical in a rendered clock, and
        // telling them apart is the whole reason ktz_is_set() exists.
        extern int32_t ktz_selftest_rs(uint32_t *out_checks);
        extern int ktz_offset_minutes(void);   // gui/clock.h
        extern int ktz_is_set(void);           // gui/clock.h
        uint32_t zchecks = 0;
        int32_t zrc = ktz_selftest_rs(&zchecks);
        int zoff = ktz_offset_minutes();
        int zset = ktz_is_set();
        kprintf("[KTZ] parser selftest %s (%u checks); TZ.CFG offset=%+d min "
                "configured=%s\n",
                zrc == 0 ? "PASS" : "FAIL", zchecks, zoff, zset ? "yes" : "no");
        bootlog_write("[KTZ] selftest %s checks=%u offset=%+d configured=%s",
                      zrc == 0 ? "PASS" : "FAIL", zchecks, zoff,
                      zset ? "yes" : "no");

        // #192: the config-read LOG POLICY self-test, and the ABI lock between
        // fs/cfgread.h's constants and rustkern/cfgread.rs's. The policy is
        // what stops an absent /CONFIG file being reported as a fault forever,
        // and what keeps a PRESENT-but-unreadable one loud. If the two sets of
        // constants ever drift, every genuine fault silently downgrades to a
        // note, so the constants are CHECKED here rather than assumed equal.
        // Provably RED: `make CFGTESTFAIL=1`.
        uint32_t cchecks = 0;
        int32_t crc  = cfgread_selftest_rs(&cchecks);
        int32_t cabi = cfgread_abi_check_rs(CFG_OUTCOME_OK, CFG_OUTCOME_ABSENT,
                                            CFG_OUTCOME_IOERR, CFG_LOG_NONE,
                                            CFG_LOG_NOTE, CFG_LOG_WARN,
                                            CFG_LOG_SUPPRESSED);
        kprintf("[CFG] log-policy selftest %s (%u checks); C/Rust ABI %s\n",
                crc == 0 ? "PASS" : "FAIL", cchecks,
                cabi == 0 ? "OK" : "MISMATCH");
        bootlog_write("[CFG] log-policy selftest %s checks=%u abi=%s",
                      crc == 0 ? "PASS" : "FAIL", cchecks,
                      cabi == 0 ? "OK" : "MISMATCH");
    }

    // #135: the RTC codec self-test AND the chip's actual mode, both to serial
    // and to /BOOTLOG.TXT. The bootlog half is the point: the iMac this bug was
    // reported from has NO SERIAL PORT, and the whole diagnosis turns on one bit
    // (Status Register B bit 2). The raw register bytes go out too, so the
    // encoding is checkable without trusting our own decoder.
    {
        extern void rtc_mode_report(void);
        rtc_mode_report();
#ifdef RTC_LIVE_TEST
        // `make RTCBINTEST=1` only. Writes Status Register B and the time,
        // restores both. Never in the golden; see drivers/rtc.c.
        extern void rtc_live_test(void);
        rtc_live_test();
#endif
    }

    {   // #120: the fd-KIND classifier behind SYS_FSTAT. ABSOLUTE assertions,
        // not an equivalence differential: there is no C twin, and a
        // differential whose two arms share one wrong assumption passes while
        // being wrong (blame.md, #433).
        extern uint32_t fstat_kind_selftest_rs(void);
        uint32_t fbad = fstat_kind_selftest_rs();
        kprintf("[FSTAT] fd-kind selftest: %s (%u mismatches)\n",
                fbad == 0 ? "PASS" : "FAIL", fbad);
        bootlog_write("[FSTAT] kind selftest %s mism=%u",
                      fbad == 0 ? "PASS" : "FAIL", fbad);
    }
    {   // #111: the pipe WRITE-side decision machine (rustkern/pipewr.rs).
        // ABSOLUTE assertions, not a differential: there is no C twin (the old
        // write side had no state machine at all), and a differential whose two
        // arms share one wrong assumption passes while being wrong (blame.md,
        // #433). Case 5 is the one worth having: "reader gone but bytes already
        // written" must report the SHORT COUNT, not EPIPE, and the obvious
        // implementation gets it backwards.
        extern uint32_t pipe_write_step_selftest_rs(void);
        uint32_t pbad = pipe_write_step_selftest_rs();
        kprintf("[PIPE] write-step selftest: %s (first failing case %u)\n",
                pbad == 0 ? "PASS" : "FAIL", pbad);
        bootlog_write("[PIPE] write-step selftest %s case=%u",
                      pbad == 0 ? "PASS" : "FAIL", pbad);
    }
    // #EXT2SELFTEST: MOUNT ORDERING IS DELIBERATE AND UNCHANGED HERE.
    //
    // This point in boot used to call ext2_selftest(), whose FIRST act was
    // ext2_mount(0, 1, 0): a whole-disk ext2 volume on the primary IDE slave,
    // which is the two-disk DEVELOPMENT layout. That mount was a SIDE EFFECT of
    // a self-test, and the #365 block further down depends on it: it mounts the
    // ext2 partition of the boot disk only `if (!ext2_is_mounted())`, so on a
    // dev machine the whole-disk volume claimed the slot first.
    //
    // Moving the self-test without moving this would have changed which volume
    // a dev machine ends up with. So the MOUNT ATTEMPT stays exactly here, in
    // the same order relative to everything else, and only the reporting and
    // probing move to after the #365 mount, where they can test the volume the
    // product actually ships. On a shipping golden this attempt fails (the
    // superblock magic at LBA 2 of the whole device is not 0xEF53) exactly as
    // it did before, and costs the same one sector read.
    {
        extern int ext2_is_mounted(void);
        extern int ext2_mount(uint8_t channel, uint8_t drive, uint32_t part_start_lba);
        if (!ext2_is_mounted() && ext2_mount(0, 1, 0) == 0) {
            kprintf("[MAIN] ext2: whole-disk volume mounted on ch0 drive1 "
                    "(two-disk dev layout)\n");
        }
    }

    // (#542) OS-wide system clipboard self-test: proves the kernel-held store
    // round-trips, size-queries, truncates and clears. One line PASS/FAIL.
    {
        extern long clip_selftest_rs(void);
        long cr = clip_selftest_rs();
        kprintf("[CLIP-SELFTEST] %s (mask=0x%lx)\n", cr == 0 ? "PASS" : "FAIL", cr);
    }

    // Cross-window drag ("docking") session self-test. Asserts every REFUSAL
    // as well as every success: an over-length payload refused rather than
    // truncated, a second concurrent drag refused, a claim before the button
    // is up refused, a claim by a window that is not the resolved target
    // refused, and a dead source killing the session while a dead target only
    // clears the target. A protocol whose refusals have never been seen to
    // fire is an intention, not a protocol (#514/#665).
    {
        extern long drag_selftest_rs(void);
        long dr = drag_selftest_rs();
        kprintf("[DRAG-SELFTEST] %s (mask=0x%lx)\n", dr == 0 ? "PASS" : "FAIL", dr);
    }

    // (#194) x86_16 interpreter 386-opcode self-test (one line PASS/FAIL at boot).
    extern int x86_16_selftest_386(void);
    x86_16_selftest_386();

    // (#740) DPMI host core (INT 31h) self-test. Nothing WIRES the 32-bit
    // execution core to this dispatcher yet (the core landed the same day and
    // exits to its caller on INT n; the bridge that turns that into a
    // dos_svc_int21() call is still to come), so nothing else reaches
    // dpmi_int31_rs(); this drives the REAL dispatcher with synthesised
    // register files and asserts the returned selectors, bases, limits and
    // flags. A linked symbol proves nothing in this tree (validate_user_ptr,
    // sse_save, graphfs), so the host is made to RUN on every boot. See
    // dos/dpmi.c and rustkern/dpmi.rs.
    {
        extern void dpmi_selftest_report(void);
        dpmi_selftest_report();
    }
    // (#740) The DOS/4GW BRIDGE, which is the seam that turns the three pieces
    // above into one running guest: it marshals the 32-bit core's register file
    // into the ONE INT 21h service core and into the DPMI dispatcher, and it
    // owns the DPMI 05xx memory services through the host's extension hook.
    // Driven at boot for the same reason the line above it is: the marshalling
    // is only reached when a guest makes a service call, so without this it is
    // exactly the kind of code that links and never executes. The two checks
    // that earn the line are the exhi[] register round trip (a wrong index puts
    // the high half of EAX into ECX and is invisible to any 16-bit-only test)
    // and the proof that a MISS applies its stub effect to the GUEST'S OWN CF
    // and AX rather than only writing a log line. See dos/dos4gw.c.
    dos4gw_selftest_report();
    // (#740) The 32-bit protected-mode guest core. Two lines, and both are
    // runtime artifacts rather than build artifacts, which is the whole point:
    //   [X8632] oracle: N vectors vs native 32-bit execution ...
    //       every expected value was read out of a REAL 32-bit execution of the
    //       same bytes on the build host, not written by hand. See
    //       tools/x86-32-oracle and kernel/exec/x86_32_test.c.
    //   [X8632] guest INT 21h AH=09h ... / hello: retired N guest instructions
    //       a 32-bit protected-mode program printing through INT 21h and
    //       exiting through AH=4Ch, executed HERE, in this kernel.
    { extern int x86_32_oracle_selftest(void);
      extern int x86_32_hello_selftest(void);
      extern int x86_32_miss_selftest(void);
      x86_32_oracle_selftest();
      x86_32_miss_selftest();
      x86_32_hello_selftest(); }

    // Compute drive ID from channel/drive (0=Primary Master, 1=Primary Slave, etc.)
    int drive_id = (ata_channel << 1) | ata_drive;
    kprintf("[MAIN] ata_found=%d, drive_id=%d\n", ata_found, drive_id);

    // #307: USB root wins when present (fs_mounted_usb set above); the ATA/IDE
    // mount is only the fallback for VMs/boxes with a legacy IDE disk. This is
    // exactly the historical b665/b693 behavior; it keeps IDE VMs booting while
    // letting the real iMac boot from its USB-MSC stick.
    if (!fs_mounted_usb && ata_found == 0) {
        boot_stage(BSTAGE_ATAROOT);
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
    // whole-disk volume on the primary IDE slave (the two-disk dev layout, by the
    // mount attempt above), look for an ext2 partition on the SAME disk the FAT
    // ESP booted from and mount it at its base LBA. This enables the proper single-disk
    // layout (GPT: FAT ESP p1 + ext2 p2). Works for a legacy IDE boot disk AND the
    // USB-MSC device (blk_read routes USB by whole-device LBA, ignoring channel/drive).
    {
        extern int ext2_is_mounted(void);
        extern int ext2_find_partition(uint8_t channel, uint8_t drive, uint32_t *out_base_lba);
        extern int ext2_mount(uint8_t channel, uint8_t drive, uint32_t part_start_lba);
        if (g_fat_fs.mounted && !ext2_is_mounted()) {
            boot_stage(BSTAGE_EXT2);
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

    // #610: ON-DEVICE FILESYSTEM CHECK. Runs ONLY when the superblock says the
    // volume was not cleanly unmounted, has a recorded error, or hit its max
    // mount count. It is READ-ONLY: it reports and (only when it verified the
    // whole volume clean) clears the flags it just verified. It cannot make the
    // machine unbootable; the worst case is a slower boot after an unclean stop.
    // Escape hatch: an empty /NOFSCK file on the FAT ESP skips it permanently.
    // This runs HERE, before userland, because that is the only moment nothing
    // is writing and the answer is therefore authoritative.
    {
        boot_stage(BSTAGE_FSCK);
        ext2_fsck_selftest();
        int skip  = (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/NOFSCK")) ? 1 : 0;
        // #610: /FSCKBENCH on the ESP runs the check twice (display on, then
        // off) to MEASURE what the progress display costs. Off by default.
        int bench = (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/FSCKBENCH")) ? 1 : 0;
        ext2_fsck_boot_check(skip, bench);
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
        boot_stage(BSTAGE_TORAM);
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
                boot_stage(BSTAGE_ROOTFLAG);
                // #120: the [PROPDATE] witness moved to AFTER this line.
                // Placed next to the other boot self-tests first, it ran BEFORE
                // ext2_mount, so /APPS/CAT came back OPEN-FAILED and only the
                // FAT half of the fix was witnessed. Measured, then moved.
    {   // #120: THE "1980-00-00 00:00" WITNESS.
        //
        // gui/properties.c renders "Modified:" as
        // properties_format_date(dlg->mod_date, dlg->mod_time), and those two
        // came from two hardcoded zeros, which that formatter turns into year
        // 1980 + month 0 + day 0. Both halves are fixed (fat_open() now stamps
        // an ext2-backed handle; the dialog reads the stamp), but the DIALOG is
        // behind a mouse click and GUI clicking is not reliable here (#334).
        //
        // So the PIPELINE is witnessed instead, end to end, with the same two
        // functions the dialog calls and on a real file: fat_open() -> the
        // handle's mtime fields -> properties_format_date(). A line reading
        // 1980-00-00 means the regression is back.
        extern int fat_open(fat_fs_t *fs, const char *path, fat_file_t *file);
        extern void fat_close(fat_file_t *file);
        extern const char *properties_format_date(uint16_t date, uint16_t time);
        static const char *const probes[2] = { "/APPS/CAT", "/FONT.TTF" };
        for (int pi = 0; pi < 2; pi++) {
            fat_file_t pf;
            if (fat_open(&g_fat_fs, probes[pi], &pf) == 0) {
                const char *ds = properties_format_date(pf.mtime_date, pf.mtime_time);
                kprintf("[PROPDATE] %s -> \"%s\"\n", probes[pi], ds);
                bootlog_write("[PROPDATE] %s -> %s", probes[pi], ds);
                if (pf.open) fat_close(&pf);
            } else {
                bootlog_write("[PROPDATE] %s -> OPEN-FAILED", probes[pi]);
            }
        }
    }
                kprintf("[MAIN] #99: ext2 is now the ROOT filesystem (/ROOTEXT2 present)\n");
                gfx_boot_log("[BOOT] root filesystem: ext2");
            }
        }
    }

    // #EXT2SELFTEST: verify the ext2 ROOT, here, because this is the first
    // point in boot at which the volume the product uses is both mounted and
    // known to be the root. Called unconditionally: if nothing is mounted the
    // self-test says so through selftest_notrun(), which lands in /BOOTLOG.TXT
    // and in the end-of-boot [SELFTEST] summary, rather than returning quietly.
    //
    // It is READ-ONLY. It runs AFTER the #610 boot fsck and after TO-RAM, so it
    // reads at RAM speed and cannot slow the boot noticeably; a failed check
    // calls ext2_mark_error(), which is what the #610 gate reads, so a volume
    // this condemns gets the full check on the NEXT boot.
    {
        extern void ext2_selftest(void);
        ext2_selftest();
    }

    // #418 (restored b713): arm the crash logger now that the FAT root is
    // mounted. Without this panic_log_write() early-returns (armed==0) so
    // /PANIC.TXT and /STAGE.TXT are NEVER written on a fault. Its startup
    // call was silently dropped in the b681->b702 refactor churn.
    boot_stage(BSTAGE_PANICLOG);
    { extern void panic_log_init(fat_fs_t *fs);
      if (g_fat_fs.mounted) panic_log_init(&g_fat_fs); }

    // #433 (restored b714): flush the RAM-buffered boot/USB/audio diagnostic
    // logs to /BOOTLOG.TXT + /USBLOG.TXT + /AUDIOLOG.TXT now that the FAT root
    // is mounted. bootlog_arm()'s startup call was silently dropped in the
    // b681->b702 churn (only referenced in comments), leaving USB enumeration
    // undiagnosable over SSH. Same casualty class as panic_log_init / the
    // USB-MSC mount block. Pure diagnostic: no enumeration behaviour changes.
    boot_stage(BSTAGE_BOOTLOG_ARM);
    { extern void bootlog_arm(fat_fs_t *fs);
      if (g_fat_fs.mounted) bootlog_arm(&g_fat_fs); }

    // #QUIETBOOT: TWO INDEPENDENT READERS OF THE SAME MARKER, CROSS-CHECKED.
    //
    // The bootloader decided whether diagnostics were armed by opening
    // \boot\DIAG.TXT over UEFI's FAT driver. The kernel can now ask its OWN FAT
    // driver the same question. If the two disagree, the hand-off is broken -
    // a boot_info_t layout drift between the loader's copy of the struct and
    // the kernel's, a loader that stopped reading the file, a path that is being
    // redirected to the ext2 volume - and the whole mechanism has quietly become
    // an instrument that can never be turned on. That is precisely the failure
    // /CDTEST.TXT has been in for months without anyone noticing, and it is
    // detectable in one boot for the cost of one fat_exists() and one line.
    //
    // /boot/DIAG.TXT and not /DIAG.TXT: fat_path_on_ext2() redirects any path
    // that is not under /boot or /EFI to the ext2 root volume, so \boot is the
    // one spelling that means the same file to the loader and to us.
    if (g_fat_fs.mounted) {
        int on_esp = fat_exists(&g_fat_fs, "/boot/DIAG.TXT") ? 1 : 0;
        int armed  = boot_stage_diag_armed();
        if (on_esp == armed) {
            bootlog_write("[DIAG] marker cross-check OK: /boot/DIAG.TXT %s, screen diagnostics %s",
                          on_esp ? "present" : "absent", armed ? "ARMED" : "quiet");
        } else {
            bootlog_write("[DIAG] MARKER CROSS-CHECK FAILED: /boot/DIAG.TXT %s on the ESP "
                          "but the bootloader handed over %s. The arming hand-off "
                          "(uefi/bootloader.c -> boot_info_t.diag_flags) is BROKEN: "
                          "on-screen boot diagnostics cannot be turned on.",
                          on_esp ? "IS present" : "is NOT present",
                          armed ? "ARMED" : "quiet");
        }
    }

    // #ASUSDIAG: WRITE THE HARDWARE INVENTORY, OR SHOW IT.
    //
    // devlog_dump() has had ZERO CALLERS since b734. It was wired in at b730 and
    // pulled out again at b734 because its HD Audio section drove hda_devlog_scan()
    // against the real iMac's Cirrus CS4208 and wedged the machine in hundreds of
    // 200ms codec-verb spins. So the most complete description this kernel can give
    // of the machine it is running on has not been written to a single image since.
    // On unfamiliar hardware that is the one file we would most want.
    //
    // It is safe to call now because the dangerous half is OFF by default:
    // devlog_set_include_hda() defaults to 0 and the buffer prints a SKIPPED block
    // naming the reason and the audio-controller count, so an absent section can
    // never be misread as absent hardware. Everything else it reads is already in
    // memory.
    //
    // The two branches matter equally:
    //   mounted     -> /DEVLOG.TXT on the root partition, read it later over USB.
    //   NOT mounted -> there is no medium to write to and nothing to continue TO
    //                  (no /APPS, no compositor, no log that can reach a disk), so
    //                  the same inventory is paged to the SCREEN forever. On a
    //                  laptop with no serial port that is the only channel left,
    //                  and a photograph of it is the bug report.
    boot_stage(BSTAGE_DEVLOG);
    {
        extern const char *devlog_build(uint32_t *out_len);
        extern void devlog_dump(fat_fs_t *fs);
        // Part 2 of DIAG_SELF_DEMO: force the no-root-filesystem branch, so the
        // paged hardware report can be seen rendering on a machine that actually
        // mounted fine. Without it that branch is only reachable on hardware we
        // do not have, which is the definition of a path nobody has watched run.
        int diag_mounted = g_fat_fs.mounted;
#ifdef DIAG_SELF_DEMO
        boot_stage_note("DIAG_SELF_DEMO: forcing the no-root report path");
        diag_mounted = 0;
#endif
        if (diag_mounted) {
            devlog_dump(&g_fat_fs);
        } else {
            uint32_t inv_len = 0;
            const char *inv = devlog_build(&inv_len);
            boot_stage_note("NO ROOT FILESYSTEM. Paging %u bytes of hardware "
                            "inventory to the screen.", (unsigned)inv_len);
            boot_stage_report_forever(
                "MAYTERAOS: NO ROOT FILESYSTEM FOUND - photograph these pages", inv);
        }
    }

    // #624: CALL THE SECURITY CORE. kernel/security/security_init() existed
    // with ZERO CALLERS, so SMEP was never enabled while the code's existence
    // implied otherwise. THIS IS THE CALL SITE. Placed here deliberately:
    //   - AFTER panic_log_init()/bootlog_arm(), so that if SMEP does fault a
    //     latent Ring-0-executes-a-user-page bug, the fault is actually
    //     recorded to /PANIC.TXT and /BOOTLOG.TXT instead of being lost;
    //   - AFTER the FAT root is mounted, so the /NOSMEP.TXT no-rebuild escape
    //     hatch is readable;
    //   - well BEFORE proc_init() (~line 1495) and any Ring-3 entry, which is
    //     the only point at which SMEP semantics start to matter.
    {
        extern void security_init(void);
        extern void security_smep_set_requested(bool on);
        if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/NOSMEP.TXT")) {
            kprintf("[MAIN] #624: /NOSMEP.TXT present, SMEP disabled by config\n");
            security_smep_set_requested(false);
        }
        /* #645: the SMAP counterpart. This escape hatch matters MORE than the
         * SMEP one. A missed user-memory access under SMAP is a #PF on a path
         * the kernel takes in normal operation, so the failure mode is "this
         * machine no longer boots", and the fix must not require the toolchain
         * that produced the broken image. Same no-rebuild contract: drop an
         * empty /NOSMAP.TXT at the root of the FAT ESP. */
        extern void security_smap_set_requested(bool on);
        if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/NOSMAP.TXT")) {
            kprintf("[MAIN] #645: /NOSMAP.TXT present, SMAP disabled by config\n");
            security_smap_set_requested(false);
        }
        boot_stage(BSTAGE_SECURITY);
        security_init();

        /* #67: AP BRING-UP + THE SMP USER-SCHEDULING ESCAPE HATCH.
         *
         * g_smp_user_sched gates whether ANY application processor is started
         * (see cpu/smp.c: the old claim that "APs still run kernel jobs in
         * parallel" was false, which is why a 2-vCPU VM pins exactly one core).
         * It defaults to 0 and SHIPS at 0 until the redesigned per-cpu run
         * queue + safe context-switch handoff has been proven against a real
         * multi-process load, because the failure it guards is a SILENT wedge.
         *
         * Drop an empty /SMPSCHED.TXT at the root of the FAT ESP to enable it
         * for that boot with no rebuild. Placed here, and not at the original
         * call site further up, purely so the FAT root is mounted and this file
         * is readable; smp_init() (which the LAPIC and #71's HDA MSI depend on)
         * still runs at the original point. */
        {
            extern int  g_smp_user_sched;
            extern void smp_user_sched_enable(int on);
            extern int  smp_start_aps(void);
            extern void smp_selftest(void);
            extern uint32_t smp_get_cpu_count(void);
            extern uint32_t smp_get_online_count(void);
            if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/SMPSCHED.TXT")) {
                kprintf("[MAIN] #67: /SMPSCHED.TXT present, ENABLING AP user "
                        "scheduling (per-cpu run queues, safe handoff)\n");
                smp_user_sched_enable(1);
                // #130 (2026-08-15): RECORD IT WHERE A MACHINE WITH NO SERIAL
                // CAN BE READ. Every SMP message in this kernel was kprintf-only
                // (smp.c: 40 kprintf, ZERO bootlog_write), and kprintf goes to
                // serial. The real iMac has no serial console, so on that
                // hardware there has NEVER been any record of whether SMP came
                // up - the gate file's presence was the only observable, and a
                // file being present says nothing about what the kernel then
                // did with it. That gap is why SMP could be armed on the target
                // machine and its status still be genuinely unknown afterwards.
                bootlog_write("[SMP] gate: /SMPSCHED.TXT PRESENT -> AP user "
                              "scheduling ENABLED");
            } else {
                bootlog_write("[SMP] gate: /SMPSCHED.TXT absent -> AP user "
                              "scheduling OFF (BSP only)");
            }
            // #167 CONTROL GATE. The defect under test is a lost wakeup in the
            // deferred-enqueue path, so the fix has to be testable against the
            // SAME binary, on the same image, in the same boot order. Dropping
            // an empty /NOWAKEFIX.TXT on the ESP restores the old behaviour for
            // that boot. Default is ON: a fix that ships off by default is not
            // shipped.
            { extern int g_wake_defer_fix;
              if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/NOWAKEFIX.TXT")) {
                  g_wake_defer_fix = 0;
                  kprintf("[MAIN] #167: /NOWAKEFIX.TXT present, the deferred-wake "
                          "state transition is DISABLED for this boot (control arm)\n");
                  bootlog_write("[167] gate: /NOWAKEFIX.TXT PRESENT -> deferred-wake "
                                "fix OFF (control arm)");
              } else {
                  bootlog_write("[167] gate: deferred-wake fix ON (default)");
              } }
            // #75 CONTROL GATE 1: refusing to queue a task another core has
            // already SELECTED (pop -> switch window). Default ON; a boot with
            // /NOPINFIX.TXT on the ESP is the control arm, same binary.
            { extern int g_enq_pin_fix;
              if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/NOPINFIX.TXT")) {
                  g_enq_pin_fix = 0;
                  kprintf("[MAIN] #75: /NOPINFIX.TXT present, the selection-pin "
                          "enqueue refusal is DISABLED for this boot (control arm)\n");
                  bootlog_write("[75] gate: /NOPINFIX.TXT PRESENT -> pin-enqueue "
                                "refusal OFF (control arm)");
              } else {
                  bootlog_write("[75] gate: pin-enqueue refusal ON (default)");
              } }
            // #75 CONTROL GATE 2: sched_on_cpu (cpu id + 1) encoding on the
            // outgoing task. Default ON; /NOCPUFIX.TXT restores the literal 1.
            { extern int g_enq_cpu_fix;
              if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/NOCPUFIX.TXT")) {
                  g_enq_cpu_fix = 0;
                  kprintf("[MAIN] #75: /NOCPUFIX.TXT present, the outgoing "
                          "sched_on_cpu keeps the literal 1 (control arm)\n");
                  bootlog_write("[75] gate: /NOCPUFIX.TXT PRESENT -> on_cpu "
                                "encoding fix OFF (control arm)");
              } else {
                  bootlog_write("[75] gate: on_cpu encoding fix ON (default)");
              } }
            { extern int g_wakeloss_gate;
              if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/WAKELOSS.TXT")) {
                  g_wakeloss_gate = 1;
                  kprintf("[MAIN] #167: /WAKELOSS.TXT present, the block/wake race "
                          "reproducer will start with the deferred device init\n");
              } }
            // #167 (c) CONTROL GATE. Separate file from /NOWAKEFIX.TXT because
            // it is a separate defect and the two must stay independently
            // testable; #165's conflation of (b) and (c) evidence is exactly
            // what this pass had to unpick.
            { extern int g_exit_stack_guard;
              if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/NOSTACKGUARD.TXT")) {
                  g_exit_stack_guard = 0;
                  kprintf("[MAIN] #167(c): /NOSTACKGUARD.TXT present, a ZOMBIE's "
                          "kernel stack may be freed while a core is on it "
                          "(control arm)\n");
                  bootlog_write("[167c] gate: stack guard OFF (control arm)");
              } else {
                  bootlog_write("[167c] gate: exit stack guard ON (default)");
              } }
            { extern void wakeloss_selftest(void); wakeloss_selftest(); }  // #167
            sched_smp_selftest();   // policy self-test, both gate states
            { extern void schedrace_selftest(void); schedrace_selftest(); }  // #75
            { extern void bkl_probe_selftest(void); extern int g_smp_bkl_full;
              if (g_smp_bkl_full) bkl_probe_selftest(); }   // #67 pass 9
            // #121: the syscall profiler is independent of the BKL, so its
            // probe runs whether or not whole-kernel locking is enabled.
            { extern void scp_selftest(void); scp_selftest(); }
            // #122: the block-layer census validates its own probe here too.
            { extern void blk_census_selftest(void); blk_census_selftest(); }
            if (g_smp_user_sched) {
                boot_stage(BSTAGE_SMP_AP);
                smp_start_aps();
                /* Bounded settle before the parallel self-test. This is a
                 * pre-scheduler context (proc_init() has not run), so there is
                 * no thread to sleep and wait_event is unavailable; the loop is
                 * bounded by BOTH a fixed iteration cap and the condition it
                 * waits for, and it exits as soon as the APs report online. */
                for (unsigned long d = 0; d < 40000000UL; d++) {
                    if (smp_get_online_count() >= smp_get_cpu_count()) break;
                    __asm__ volatile("pause");
                }
                kprintf("[MAIN] #67: %u of %u cores online\n",
                        smp_get_online_count(), smp_get_cpu_count());
                smp_selftest();
            } else {
                kprintf("[MAIN] #67: AP user scheduling OFF (default); user "
                        "processes run on the BSP only. Add /SMPSCHED.TXT to "
                        "the ESP to enable.\n");
            }
            // #130: the OUTCOME, not the intent. How many cores actually came
            // online, and is the AP scheduler actually live? Written to the
            // persistent log so a serial-less machine can answer "is SMP
            // running?" from the stick after the fact.
            {
                extern int g_smp_user_sched;
                bootlog_write("[SMP] result: %u of %u core(s) online, "
                              "AP user scheduling %s",
                              smp_get_online_count(), smp_get_cpu_count(),
                              g_smp_user_sched ? "LIVE" : "off");
            }
        }

        /* #653: the security event sink used to be started HERE, and that was
         * wrong. seclog_init() creates a worker thread, and this block runs
         * well before proc_init() (see the comment above), so proc_create_ex()
         * had nothing to create into. The thread never ran a single
         * instruction and /CONFIG/SECURITY.LOG was never written. The call has
         * moved to just after proc_init(); events raised before that point sit
         * in the audit ring and are drained on the worker's first wake, which
         * is what the ring is for. */
    }

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

    // #196 / #404: prove the Rust ISO 9660 parsers (live under -DRUST_ISO9660)
    // == the C references on THIS build, over malformed vectors and a fuzz
    // corpus, BEFORE any disc image is mounted. Bounded, runs once, logs one
    // [RUST-DIFF] iso9660 line to serial + /BOOTLOG.
    { extern void diskimg_iso_rust_selftest(void); diskimg_iso_rust_selftest(); }
    // #739: the drive-letter policy's own property test. One line, every
    // boot, so a rule that stops holding shows up in the boot log rather
    // than as a drive that quietly went missing.
    { extern void diskimg_drvmap_selftest(void); diskimg_drvmap_selftest(); }

#ifdef DPMI_RMCS_SELFTEST
    // #740: DPMI 0300h (simulate real-mode interrupt) driven against the real
    // INT 21h service core. `make DPMITEST=1` only; never in a golden (see the
    // DPMITEST block in kernel/Makefile for why). Placed here because it needs
    // a mounted root: it opens, writes and deletes a real file.
    { extern void dpmi_rmcs_selftest(void); dpmi_rmcs_selftest(); }
#endif

    // #196 live proof harness. No-op unless /CDTEST.TXT exists on the root FS,
    // so a normal boot pays one failed file open. When present it mounts each
    // named image on E:, lists its root, checksums a byte range, then mounts the
    // NEXT image over the top, which is the live disc-swap path. See
    // dos/diskimg_test.c.
    { extern void diskimg_boot_harness(void); diskimg_boot_harness(); }
    // #193: proves which FILESYSTEM answers a path that a mounted disk
    // image and the folder underneath it both contain. No-op unless
    // /IMG193.TXT names an image.
    { extern void img_shadow_selftest(void); img_shadow_selftest(); }

    // #740: data volumes in the boot device's unpartitioned TAIL. The golden's
    // ext2 root is 1.62 GB with 1.13 GB free and Discworld 2's data is 1.54 GB
    // compressed, so large game data cannot live in the image and has to be
    // read off the stick instead. A volume is a raw ISO 9660 image at a
    // 1 MiB-aligned offset above 4 GiB, which is above the #375 TO-RAM window,
    // so every read of it is a real device read and not a RAM-copy hit.
    //
    // Placed HERE, immediately after the #196 harness, deliberately: both mount
    // through the same dos/diskimg.c layer onto the same CD letters, and both
    // must be finished before dos_start_deferred_launch() (further down this
    // function) can start a DOS guest that expects a disc in the drive.
    //
    // Bounded and side-effect-free when there is nothing there: the property
    // test is pure, and the probe is at most 8 reads of 2 KiB that stop at the
    // first region that is not an ISO 9660 volume (an unwritten tail is zeros,
    // which is rejected). See dos/usbvol.h and rustkern/usbvol.rs.
    { extern void usbvol_selftest(void); usbvol_selftest(); }
    { extern void usbvol_probe_and_mount(void); usbvol_probe_and_mount(); }

    // #404 / #479 Phase B: prove the Rust ip_checksum == the C ip_checksum on
    // THIS build before the network stack (which now computes/validates IP
    // checksums via ip_checksum_rs under -DRUST_IP_CHECKSUM) is brought up.
    // Bounded, runs once, logs one [RUST-DIFF] line to serial + /BOOTLOG.
    ip_checksum_rust_selftest();

    // #745 (task #36): prove the async HTTP job-slot OWNERSHIP state machine
    // (rustkern/fetchown.rs) on THIS build, before any app can start a fetch,
    // and prove the Rust module and proc/syscall.c agree about how many slots
    // each table has. The two properties it enforces are both NEGATIVE (a
    // non-owner is refused; a dead owner's slot returns to the pool), and an
    // unobserved negative property is indistinguishable from an absent one.
    // Bounded, once, one [RUST-SEC] fetchown line.
    { extern void fetchown_boot_check(void); fetchown_boot_check(); }
    // #fdguard: prove the legacy-fd and /dev/pts ownership guards
    { extern void fdown_boot_check(void); fdown_boot_check(); }
    { extern void ptsown_boot_check(void); ptsown_boot_check(); }
    // #745 task #59: prove the framebuffer ownership rules before anything can
    // claim the screen (arm/claim/release/re-arm, and the released-means-
    // disarmed case that is the whole fix).
    { extern void fbown_boot_check(void); fbown_boot_check(); }

    // #404 / #485 Phase C: prove the Rust ext2 directory-block parser
    // (ext2_dirblock_find_rs, live under -DRUST_EXT2_DIRFIND) == the #476-
    // hardened C over valid AND malformed blocks on THIS build, before any
    // ext2 path lookup runs. Bounded, once, one [RUST-DIFF] ext2_dir line.
    extern void ext2_dir_rust_selftest(void);
    ext2_dir_rust_selftest();

    // #605 (extends the #485 seam): prove the Rust ext2 directory-entry INSERT
    // (ext2_dirblock_insert_rs, live under -DRUST_EXT2_DIRADD) produces a
    // BYTE-IDENTICAL resulting block and an identical return code to the C
    // reference over generated and fuzzed directory blocks, on THIS build,
    // before anything creates a file. Also replays the #597 hostile block
    // (a record whose name_len does not fit its own rec_len, which made the
    // pre-#597 C underflow `rec - used` and memcpy past the block buffer).
    // Bounded, once, one [RUST-DIFF] + one [RUST-SEC] line each.
    extern void ext2_dirinsert_rust_selftest(void);
    ext2_dirinsert_rust_selftest();
    // #746: the block-level REPOINT that makes rename() atomic over an existing
    // destination. Same [RUST-DIFF] discipline as the two above.
    extern void ext2_dirrepoint_rust_selftest(void);
    ext2_dirrepoint_rust_selftest();

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

    // #615: prove the Rust 4-bit-table GHASH == the bit-serial C reference on
    // THIS build BEFORE TLS uses AES-GCM. The bit-serial multiply was measured
    // at 36-40% of one core for an entire App Store download (build 975
    // [DLPROF]); the table method is an INDEPENDENT algorithm, so this
    // differential is a genuine cross-check. 4,177 vectors (random single-block
    // multiplies + every stream length 0..80, which covers the zero-padded
    // partial tail) plus the NIST GCM AES-128 Test Case 4 known-answer vector
    // through the LIVE aes_gcm_* API in both directions, plus an RDTSC
    // benchmark. Bounded, once, no waiting (#426).
    extern void ghash_rust_selftest(void);
    ghash_rust_selftest();

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
    extern void tlspool_rust_selftest(void);   /* #616 */
    tlspool_rust_selftest();

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
    boot_stage(BSTAGE_NET);
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
        // ICS-segment static (192.0.2.1) instead. Previously this branch
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
            int result = icmp_ping(0xC0000201);
            if (result < 0) {
                kprintf("[NET] Send failed (ARP), waiting...\n");
                for (int i = 0; i < 50; i++) {
                    net_poll();
                    for (int j = 0; j < 5000; j++) io_wait();
                }
                result = icmp_ping(0xC0000201);
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
    boot_stage(BSTAGE_INPUT);
    mouse_init();

    // Initialize GUI subsystem
    gfx_boot_log("[BOOT] Initializing window manager...");
    wm_init();
    desktop_init();
    kprintf("[GUI] Window manager and desktop initialized\n");

    // Initialize multi-user subsystems
    perms_init();
    // #736: the guest scratch location. AFTER perms_init(), because it fills an
    // absent PERMS.DB entry and the database has just been loaded.
    { extern void dos_scratch_perms(void); dos_scratch_perms(); }
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
    boot_stage(BSTAGE_PROC);
    proc_init();

    /* #745 (#75) MEASURED, build 1876: WITHOUT THIS, /SMPSCHED.TXT DOES NOTHING.
     *
     * smp_start_aps() runs ~700 lines above this, as soon as the FAT root is
     * mounted and the gate file is readable, and proc_init() is HERE. So an AP
     * reaches sched_ap_enter() before the process table exists, is correctly
     * refused by the #75 part-4 g_proc_init_done guard, falls through to the
     * kernel-job-only loop, finds the work ring empty and HALTS. Its only wake
     * is an SMP_WAKE_VECTOR IPI, which is sent by smp_work_submit()/
     * sched_rq_push() - and sched_rq_push() never targets a core that is not a
     * consumer. So the refusal was a one-way door: the AP never retried and
     * never joined.
     *
     * MEASURED on three gate-ON boots of build 1876 on throwaway VM <vmid>:
     * "consumers=0x1" on every one of 40 [SCHEDCORE] lines, with
     * "[SCHED] cpu 1 reached the scheduler before proc_init()" once per boot
     * and zero contended BKL acquisitions all boot. The gate was ON and the
     * kernel was single-core-scheduled anyway, which means every gate-ON figure
     * measured on this tree so far was measuring a kernel with one scheduling
     * core.
     *
     * One directed kick, once, at the only moment the answer can change. */
    { extern int g_smp_user_sched; extern void smp_wake_aps(void);
      if (g_smp_user_sched) {
          kprintf("[MAIN] #75: process table is up; waking APs so they can "
                  "retry sched_ap_enter()\n");
          smp_wake_aps();
      } }

    /* #653: NOW the security event sink can start - proc_init() is done, so
     * the worker thread can actually be created. Everything security_audit()
     * recorded during early boot is still in the ring and gets drained on the
     * first wake. */
    // #711: GraphFS journal up BEFORE the audit sink, so the sink's very
    // first drain already has somewhere tamper-evident to write. Refuses
    // itself (loudly) unless the root is ext2.
    { extern void gfs_journal_init(void); gfs_journal_init(); }
    // #711 slice 2: the graph is a fold of the journal, so it comes up
    // immediately after the journal and before anything can ask it a question.
    { extern void gfs_fold_init(void); gfs_fold_init(); }
    { extern void seclog_init(void); seclog_init(); }

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

    // #139: arm the xHCI MSI. This kernel has never had an xHCI interrupt at
    // all - USBCMD.INTE and Interrupter 0's IMAN.IE were set at init, but no
    // PCI-level delivery was ever enabled, so the controller could not raise
    // one and every USB completion waited for a timer-driven drain pass. Must
    // run after lapic_init() (far above), same contract as
    // hda_setup_interrupt(). Costs nothing and changes nothing on a controller
    // with no MSI capability: the drain workers remain the backstop either way.
    {
        extern void xhci_setup_interrupt(void);
        xhci_setup_interrupt();
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
    boot_stage(BSTAGE_STI);
    sti();

    // #507 DELAY SELF-CHECK. A delay that lies does not fail loudly: it makes
    // USB enumeration intermittent (#433/#373/#366), which is how the PIT
    // mode-3 halving survived so long. So measure the shared busy-delay against
    // an INDEPENDENT reference and print the result every boot.
    //
    // The reference is IRQ0 DELIVERY (timer_ticks), which is the right control
    // precisely because the mode-3 bug could not affect it: mode 3 divides the
    // input clock by the full reload value to produce its OUTPUT, so the 250 Hz
    // interrupt rate was always correct. Only direct reads of the COUNTDOWN
    // REGISTER were 2x fast, and that is what mono_busy_delay_us() is built on.
    // Two independent mechanisms agreeing is evidence; the clock agreeing with
    // itself would not be.
    //
    // This is the one place ticks are the RIGHT tool: we are deliberately
    // measuring tick delivery, not using ticks as a deadline (cpu/mono.h).
    // Cost is the 200 ms it measures, on a ~40 s boot.
    {
        extern volatile uint64_t timer_ticks;

        // SETTLE FIRST, or the reference lies. MEASURED on build 980: running
        // this measurement immediately after sti() reported 15642 ticks for a
        // 200 ms delay instead of ~50 - a 313x overcount. That is not a bad
        // delay, it is KVM handing back every tick the guest missed. The PIT
        // ran the whole boot with interrupts off, kvm-pit's default
        // lost_tick_policy="delay" queued every one of those ~10-15k ticks, and
        // sti() releases the entire backlog at once (it drained at ~78k
        // ticks/sec). This is the #524/#499 reinjection burst, caught in the
        // act, and it is exactly why nothing in this kernel may use ticks as a
        // clock. Here we WANT tick delivery as the reference, so we simply wait
        // for the backlog to clear before trusting it.
        //
        // Bounded (#426): at most ~20 x 100 ms. This is a measurement
        // precondition, not a device poll - there is nothing to wait ON, and a
        // machine whose tick rate never settles just proceeds and reports what
        // it saw rather than hanging.
        for (int i = 0; i < 20; i++) {
            uint64_t a = timer_ticks;
            mono_busy_delay_ms(100);
            if ((timer_ticks - a) * 10ull < (uint64_t)g_timer_hz * 2ull) break;
        }

        uint64_t t0 = timer_ticks, m0 = mono_ms();
        mono_busy_delay_ms(200);
        uint64_t dt = timer_ticks - t0, dm = mono_ms() - m0;
        uint64_t expect = (uint64_t)g_timer_hz * 200ull / 1000ull;
        kprintf("[DELAY] mono_busy_delay_ms(200): %llu ticks (expect ~%llu at %u Hz), %llu ms on mono\n",
                dt, expect, g_timer_hz, dm);
        bootlog_write("[DELAY] 200ms -> %llu ticks (expect ~%llu), %llu ms mono", dt, expect, dm);
        // Half or double the expected tick count means the delay is not the
        // duration it claims. Say so loudly rather than leaving it to be
        // rediscovered as flaky USB enumeration months later.
        if (dt < expect * 3 / 4 || dt > expect * 5 / 4) {
            kprintf("[DELAY] *** WARNING: busy-delay is NOT wall-clock accurate ***\n");
            bootlog_write("[DELAY] WARNING busy-delay inaccurate: %llu vs %llu", dt, expect);
        }
    }

    // #745 (#62): THE TICK-SOURCE REPORT.
    //
    // The [DELAY] check above measures whether mono_busy_delay_ms() lasts as
    // long as it claims. This measures the other direction, which nothing in
    // the tree did before: whether the periodic TICK is being DELIVERED at all,
    // judged against mono (TSC) because timer_ticks cannot be its own
    // reference. On the owner's iMac14,4 the desktop only advances while
    // something is running - "if i stop moving the mouse the timers all hang,
    // uptime stops counting" - which is a claim about interrupt delivery that
    // `uptime` (ticks/hz, circular) can never confirm or refute.
    //
    // It runs HERE, on the boot path, with no scheduler dependency, because a
    // machine whose tick never starts may never run the heartbeat thread at all
    // and would otherwise write no telemetry whatsoever. It also snapshots the
    // 8259 mask, the LAPIC LINT0 virtual-wire routing and the I/O APIC
    // redirection entries for GSI0/GSI2, so a dead tick can be ATTRIBUTED to a
    // mask rather than guessed at from a symptom.
    tickwatch_boot_report();

    // Enable preemptive multitasking
    sched_set_preemption(true);
    // #566 decision 2: RC-2323 (hardcoded admin/maytera Ring-0 remote shell +
    // GUI click-injection) is REMOVED from the build entirely. The SSH server
    // started below is the sanctioned remote path. No listener on 2323, and no
    // compile flag that could reintroduce it into a golden.
    { extern void win16_autolaunch_init(void); win16_autolaunch_init(); }
    // #697: Offer to start the SSH server. It starts ONLY if /CONFIG/SSHD.CFG
    // says enable=1; absent config means no listener, no socket, no thread.
    //
    // This call used to be guarded by "is /CONFIG/SSHHOST.KEY present", and it
    // was, on every shipping image, so every shipping image listened on port 22
    // and (with the compiled-in credential that also lived in ssh2_server.c)
    // handed out a uid 0 shell to anyone on the network. A host key existing is
    // not a decision to expose a network service. The decision now lives in
    // exactly one place, ssh_server_start(), and it fails closed.
#ifdef MAYTERA_SSHD
    // #785: compiled out entirely unless the kernel was built MAYTERA_SSHD=1.
    // A kernel without it has no server code on the image at all, so there is
    // nothing for a future config or credential regression to expose.
    { extern void ssh_server_start(void); ssh_server_start(); }
#else
    kprintf("[SSHD] not built into this kernel (build with MAYTERA_SSHD=1)\n");
#endif
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

    // #332: JPEG decoder boot self-test (gated on /CONFIG/JPGTEST.CFG). Decodes
    // an embedded baseline plus two PROGRESSIVE (SOF2) JPEGs through the real
    // image_load_jpeg path and checks dims + sample pixels vs libjpeg ground
    // truth. SAME dead-code class as nettest above: jpeg_selftest.c has defined
    // jpeg_start_deferred_selftest() since #332 but NOTHING ever called it, so
    // the harness never ran once. Wired here (no-op without the CFG flag).
    {
        extern void jpeg_start_deferred_selftest(void);
        jpeg_start_deferred_selftest();
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
    // #745 (task #70): START THE ASYNCHRONOUS CONSOLE DRAIN.
    //
    // Here and not earlier, deliberately. g_console_async is set by the drain
    // thread ITSELF as its first action, so there is never a window in which
    // kprintf queues output with no thread to write it - which is the "boot
    // path before any thread exists" risk, closed by construction rather than
    // by a comment. Everything printed above this line went out synchronously,
    // exactly as before.
    //
    // /CONSYNC.TXT on the FAT ESP forces the old synchronous console for the
    // whole boot. That is the CONTROL ARM: it lets the same kernel binary be
    // measured both ways, the way /SMPSCHED.TXT already gates AP scheduling,
    // so an A/B comparison cannot be confounded by a rebuild.
    {
        extern volatile int g_console_async_wanted;
        extern void console_drain_worker(void *arg);
        if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/CONSYNC.TXT")) {
            g_console_async_wanted = 0;
            kprintf("[CONSOLE] /CONSYNC.TXT present: SYNCHRONOUS console "
                    "(control arm for #745 task #70)\n");
        }
        int cdpid = proc_create("condrain", console_drain_worker, NULL, PRIO_NORMAL);
        kprintf("[CONDRAIN] drain thread created, pid=%d\n", cdpid);

        // #745 (task #70) DESTRUCTIVE PROOFS, both off unless their marker file
        // is on the ESP. /CONPANIC.TXT proves a panic still prints everything
        // the ring was holding; /CONBURST.TXT proves an overflow announces
        // itself instead of eating lines quietly. A shipping golden has
        // neither file and never creates this thread.
        {
            extern volatile int g_console_test_panic;
            extern volatile int g_console_test_burst;
            extern void console_selftest_worker(void *arg);
            if (fat_exists(&g_fat_fs, "/CONPANIC.TXT")) g_console_test_panic = 1;
            if (fat_exists(&g_fat_fs, "/CONBURST.TXT")) g_console_test_burst = 1;
            if (g_console_test_panic || g_console_test_burst) {
                int tp = proc_create("contest", console_selftest_worker, NULL,
                                     PRIO_NORMAL);
                kprintf("[CONTEST] destructive console proof armed "
                        "(panic=%d burst=%d), pid=%d\n",
                        g_console_test_panic, g_console_test_burst, tp);
            }
        }
    }

#ifdef FPU_YMM_SELFTEST
    // #745 local 107: does a context switch preserve the upper 128 bits of the
    // YMM registers? Debug builds only (`make YMMTEST=1` / `make YMMTEST=red`);
    // compiles to nothing in a shipping kernel. See proc/fpu_ymm_test.c.
    { extern void fpu_ymm_selftest_start(void); fpu_ymm_selftest_start(); }
#endif

#ifdef FAT_LOCK_SELFTEST
    // #746 fatlock: does the converted FAT lock still EXCLUDE? Debug builds
    // only (`make fatlock-selftest` / `make fatlock-selftest-red`); compiles to
    // nothing in a shipping kernel. See fs/fat_lock_test.c.
    { extern void fat_lock_selftest_start(void); fat_lock_selftest_start(); }
#endif

    // #334: arm the debug-gated serial (COM1) synthetic-input channel. No-op
    // unless /TESTINPUT.TXT is present on the FAT ESP, so a shipping golden
    // gains zero surface. Lets host test scripts inject keyboard+mouse events
    // deterministically, bypassing QEMU's lossy PS/2 model, with a serial ACK.
    { extern void testinput_init(void); testinput_init(); }

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

    // #692: prove the spawn-identity policy is LIVE on this build before
    // anything uses it, rather than merely compiled in. One serial line.
    {
        extern void spawn_ident_selftest(void);
        spawn_ident_selftest();
        // #112: the initial-stack / environment-block layout policy
        // (rustkern/envblock.rs). Same reason as the line above: prove it is
        // LIVE on this build, not merely compiled in.
        extern void envblock_selftest(void);
        envblock_selftest();
        // #162: the volume/mute state machine and the HID report-descriptor
        // parser. Both are pure logic with vector tests, and both test things
        // NO VM can exercise: no QEMU device presents a USB consumer-control
        // collection, and a lossless mute round trip is invisible without an
        // amplifier. Proven on THIS build, or it would ship having never run.
        {
            extern void sysvol_selftest(void);
            sysvol_selftest();
        }
        // #162: the deferred-apply worker. A media key can land in hard IRQ
        // context, so the IRQ side only moves atomics and this thread does the
        // codec write. Started here because it needs proc_init() to have run.
        {
            extern void sysvol_start_worker(void);
            sysvol_start_worker();
        }
        // #745: prove the session-lock and account-uid policy on THIS build,
        // rather than trusting that it was compiled in. Loud on failure.
        // #745 (local 82): the POSIX process-group/session rules and the
        // poll(2) event classification. Both are pure Rust policy, and both
        // are mostly REFUSALS, which is exactly the kind of code that can sit
        // compiled-in and never once run. Proven on THIS build, loudly.
        {
            extern uint32_t pgrp_selftest_rs(void);
            extern uint32_t pollsys_selftest_rs(void);
            extern uint32_t poll_bits_check(void);
            uint32_t pf = pgrp_selftest_rs();
            uint32_t qf = pollsys_selftest_rs();
            uint32_t bf = poll_bits_check();
            kprintf("[PGRP] policy self-test %s mask=0x%x\n", pf ? "FAIL" : "PASS", pf);
            kprintf("[POLL] classify self-test %s mask=0x%x, vfs bits %s mask=0x%x\n",
                    qf ? "FAIL" : "PASS", qf, bf ? "MISMATCH" : "match", bf);
            bootlog_write("[PGRP] self-test %s mask=0x%x; [POLL] self-test %s mask=0x%x bits=0x%x",
                          pf ? "FAIL" : "PASS", pf, qf ? "FAIL" : "PASS", qf, bf);
        }
        {
            // #126: the session-teardown attribution rule. Almost every clause
            // is a REFUSAL, which is precisely the kind of code that can sit
            // compiled in and never once run, so prove it on THIS build.
            extern int sessend_selftest_rs(void);
            int ef = sessend_selftest_rs();
            kprintf("[SESSEND] teardown policy self-test %s case=%d\n",
                    ef ? "FAIL" : "PASS", ef);
            bootlog_write("[SESSEND] teardown policy self-test %s case=%d",
                          ef ? "FAIL" : "PASS", ef);
        }
        {
            extern uint32_t sessionid_selftest_rs(void);
            uint32_t sfails = sessionid_selftest_rs();
            if (sfails == 0) kprintf("[SESSIONID] policy self-test PASS (rust)\n");
            else             kprintf("[SESSIONID] policy self-test FAIL mask=0x%x\n", sfails);
            bootlog_write("[SESSIONID] policy self-test %s mask=0x%x",
                          sfails ? "FAIL" : "PASS", sfails);
        }
        // #745: the elevation mechanism. Proves on THIS build that the grant's
        // path-coverage test refuses "/APPS/../CONFIG/SHADOW", that the
        // sanitiser strips control bytes and truncates, and that the state
        // machine really is one-at-a-time / three-attempts / expiry-DENIES.
        // Every one of those is a refusal, and a refusal that was never
        // exercised is a claim, not a control.
        {
            extern void elevate_selftest(void);
            elevate_selftest();
        }
        // #745: the /CONFIG/LOGIN.CFG contract. Proves on THIS build that the
        // sign-in mode parses the one way it is allowed to, and - the case that
        // matters - that composing the file for an autologin change PRESERVES
        // the login_mode line instead of erasing it. A silent erase there would
        // look exactly like "the setting did not stick".
        // #229: the first-run (OOBE) state chokepoint. Every assertion in it
        // is about a REFUSAL or a CONSUMING read - mark-done on a machine with
        // no owner, an unknown op, the handover firing twice - which are
        // exactly the behaviours a normal boot never exercises and therefore
        // the ones that rot unnoticed. It also asserts the six FR_* op numbers
        // on THIS build, because the wizard is a no_std Rust app that cannot
        // include syscall.h and so keeps a fifth copy of them.
        {
            extern uint32_t firstrun_selftest_rs(uint32_t *out_checks);
            uint32_t fchecks = 0;
            uint32_t ffails = firstrun_selftest_rs(&fchecks);
            if (ffails == 0) kprintf("[FIRSTRUN] #229 state self-test PASS (rust, %u checks)\n", fchecks);
            else             kprintf("[FIRSTRUN] #229 state self-test FAIL mask=0x%x (%u checks)\n", ffails, fchecks);
            bootlog_write("[FIRSTRUN] #229 state self-test %s mask=0x%x checks=%u",
                          ffails ? "FAIL" : "PASS", ffails, fchecks);
        }
        {
            extern uint32_t loginmode_selftest_rs(void);
            uint32_t lfails = loginmode_selftest_rs();
            if (lfails == 0) kprintf("[LOGINMODE] LOGIN.CFG contract self-test PASS (rust)\n");
            else             kprintf("[LOGINMODE] LOGIN.CFG contract self-test FAIL mask=0x%x\n", lfails);
            bootlog_write("[LOGINMODE] LOGIN.CFG contract self-test %s mask=0x%x",
                          lfails ? "FAIL" : "PASS", lfails);
        }
        // Password strength + breached-password policy. Proves on THIS build
        // that a known-breached password is refused and a strong one is
        // accepted, and that the 66 KB breach table is actually linked in: a
        // missing or malformed table would disable the breach check with no
        // other symptom at all.
        {
            uint32_t pfails = pwpolicy_selftest_rs();
            pw_policy_info_t pi;
            pw_policy_info_rs(&pi);
            kprintf("[PWPOLICY] self-test %s mask=0x%x; min=%u max=%u distinct=%u; "
                    "breach table %u entries (%u bytes, min_len %u)\n",
                    pfails ? "FAIL" : "PASS", pfails,
                    pi.min_len, pi.max_len, pi.min_distinct,
                    pi.table_entries, pi.table_bytes, pi.table_min_len);
            bootlog_write("[PWPOLICY] self-test %s mask=0x%x; breach table %u entries",
                          pfails ? "FAIL" : "PASS", pfails, pi.table_entries);
        }
    }

    // #708: same reasoning for the DOS/Win16 guest filesystem gate. It asserts
    // the fail-closed default (an unarmed slot DENIES), that an ordinary uid is
    // refused /CONFIG/SHADOW, and that root is not, i.e. that the gate really
    // is calling perms_check() rather than denying everything unconditionally.
    {
        extern void guestfs_boot_selftest(void);
        guestfs_boot_selftest();
    }

    // #rawrite: the DOS per-user write overlay is pure path policy, so what it
    // gets is a PROPERTY test rather than a differential (there is no C twin to
    // differ from; rustkern/drvmap.rs made the same call). It pins the two
    // properties a wrong answer here would break silently: that /DOS/RANGER is
    // not treated as living under /DOS/RA, and that an undersized buffer fails
    // closed instead of truncating into a shorter path naming another directory.
    {
        extern unsigned int dosovl_selftest_rs(void);
        unsigned int ofails = dosovl_selftest_rs();
        kprintf("[DOSOVL] self-test %s mask=0x%x (per-user DOS write overlay)\n",
                ofails ? "FAIL" : "PASS", ofails);
        bootlog_write("[DOSOVL] self-test %s mask=0x%x",
                      ofails ? "FAIL" : "PASS", ofails);
    }

    // #740 Milestones 1/2: the LE (Linear Executable) loader for DOS/4GW.
    // Runs the Rust fixture self-test unconditionally (it costs one 16 KiB
    // scratch buffer and it pins the three traps that are INVISIBLE on a small
    // or well-formed corpus: the anchor MZ, the 24-bit big-endian page number
    // at index 256, and the signed src_off), then dumps whatever
    // /CONFIG/LEINFO.CFG asks for. Absent that file it prints one line and
    // stops, so the shipping golden pays the fixture and nothing else.
    //
    // Placed here rather than beside the pure-vector [RUST-DIFF] self-tests
    // because it reads real modules off the filesystem, which the vector
    // self-tests do not.
    {
        extern void le_boot_selftest(void);
        le_boot_selftest();
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

    // #fdguard: gated cross-process exploit test launcher. No-op unless
    // /CONFIG/FDGUARD.TEST exists (present on no production golden). Launches
    // /APPS/FDXTEST and /APPS/PTSXTEST to prove the fd/pts ownership guards.
    { extern void fdguard_start_deferred_test(void); fdguard_start_deferred_test(); }

    // #702 real-hardware defensive hardening: kick off audio hardware
    // probing (HDA controller reset/CORB-RIRB/codec parse, #71 MSI arm) on
    // its own low-priority background worker here, now that interrupts and
    // the scheduler are live. It runs CONCURRENTLY with login_run()/
    // desktop_run() below rather than gating them -- see the longer comment
    // above audio_init_worker() in drivers/audio.c for the full rationale.
    {
        extern void audio_start_deferred_init(void);
        audio_start_deferred_init();
        // #167: start the reproducer alongside 'audioinit', deliberately. The
        // defect being reproduced is a property of THIS phase of boot - short
        // lived kernel threads that block and exit while other cores are
        // actively scheduling - and starting it here puts it in the same
        // regime rather than a quiet one. No-op unless /WAKELOSS.TXT is present.
        { extern void wakeloss_start(void); wakeloss_start(); }
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

        // #566 LOGIN GATE LOOP. Once any session authenticates this boot, an
        // exited/dead desktop returns HERE (re-show login), never to the
        // unauthenticated Ring-0 kernel_shell() below (decision 3, design 3.3a).
        login_result_t login_result;
        for (;;) {
        // Run login screen before desktop
        kprintf("[GUI] Starting login screen...\n");
        login_init();
        // #157: login_init() no longer releases the display (it moved to the
        // first frame login_draw() actually paints), so from here to the
        // compositor's first present these lines REACH THE SCREEN. On an
        // autologin image login_run() returns without drawing anything, and
        // before #157 that made every one of the following stages invisible.
        // #SB: the block-write staging counters, printed every boot whether or
        // not anything is wrong. seal_broken MUST be 0. A non-zero value means
        // the medium is being handed bytes the filesystem did not write, which
        // is the fault that destroyed an ext2 primary superblock on golden
        // build 2215. Printed here because by now the boot has done all of its
        // ext2 metadata, bootlog, heartbeat and PROPDATE writes.
        blk_stage_report();
#ifdef BLKSTAGE_TEST
        blk_stage_selftest();
#endif
        gfx_boot_log("[BOOT] Starting login screen...");
        boot_stage(BSTAGE_LOGIN);
        login_run(&login_result);
        {
            char _lb[96];
            snprintf(_lb, sizeof(_lb),
                     "[BOOT] Login complete (user '%s', uid %u), starting desktop...",
                     login_result.username[0] ? login_result.username : "?",
                     (unsigned)login_result.uid);
            gfx_boot_log(_lb);
        }

        // Set session identity and create home directory
        desktop_set_session(login_result.uid, login_result.gid);
        if (login_result.home[0]) {
            fat_mkdir(&g_fat_fs, login_result.home);
            // #745: mkdir alone left the home ROOT-OWNED, so at uid 0 it worked
            // by accident and at any other uid the user could not write their
            // own home. Claim it for the session user, but only when it is not
            // already theirs: perms_set marks PERMS.DB dirty, and rewriting it
            // on every single login is a disk write for nothing.
            //
            // Deliberately not applied to root's home "/": the filesystem root
            // is not a home directory and must keep its own 0755 root ownership.
            if (!(login_result.home[0] == '/' && login_result.home[1] == '\0')) {
                uint32_t howner = 0, hgroup = 0; uint16_t hmode = 0;
                int have = perms_get(login_result.home, &howner, &hgroup, &hmode);
                if (have != 0 || howner != login_result.uid || hgroup != login_result.gid) {
                    perms_set(login_result.home, login_result.uid, login_result.gid, 0750);
                    bootlog_write("[SESSION] home '%s' claimed for %u:%u (#745)",
                                  login_result.home,
                                  (unsigned)login_result.uid, (unsigned)login_result.gid);
                }
                // The skeleton subdirectories are created no-op-if-present and
                // are stamped with the same ownership, so a home that predates
                // this change is repaired rather than left half-owned.
                users_make_home_skeleton(login_result.home,
                                         login_result.uid, login_result.gid);
            }
        }

        // #PERMSKIP: THE DEFINITIVE RUN OF THE PERMISSION SELF-TEST.
        //
        // perms_init() calls perms_selftest() far above, before users_init(),
        // before any session: at that point the only homes it can test are
        // whatever /CONFIG/PERMS.DB already happens to hold. Until this change
        // it did not even do that - it looked up the literal string
        // "/HOME/ADMIN" and printed
        //
        //     [PERMS-SELFTEST] SKIP traversal vectors (/HOME/ADMIN not 1000:0750)
        //
        // forever, on every CORRECTLY PROVISIONED machine, because the wizard
        // lets the owner name the account and the owner is not called "admin".
        //
        // HERE is where the kernel knows who is actually logged in and where
        // their home actually is, and it has just claimed that home for them
        // three lines above, so this is the run that tests the real thing:
        // the real user's real home, under the real uid/gid, whatever it is
        // called. A session that cannot be tested (root, whose home is "/" and
        // is not a home directory; or a home with no PERMS.DB entry) is
        // reported as a LOUD not-run through the register, not as a quiet SKIP.
        perms_selftest_session(login_result.home,
                               login_result.uid, login_result.gid);

        // And the one line that says whether anything on this machine declined
        // to verify itself this boot. Placed after the last self-test, because
        // this is the last point at which a group can still declare itself.
        selftest_notrun_report();

        // #684: PROVISION the AI key from the seed, ONCE, as root.
        //
        // The user's decision is that /CONFIG/KIMI.KEY is a deployment SEED,
        // not something an app reads: "this kimi.key file should only be used
        // to write that value to settings (in a way that's protected from other
        // users and apps) not be called directly by the ai chat app". So this,
        // running in Ring 0 at session start, is the ONLY reader of that file
        // in the whole system, and it copies the value into the user's own
        // protected settings.
        //
        // Every branch here is a no-op on a normal deployment: the seed is
        // ABSENT (nothing to do), or the user already has settings (never
        // overwrite what they typed into Settings). It is deliberately not a
        // "sync": a user who clears their key must not have it restored on the
        // next login.
        //
        // #OOBEAUTH GUARD: skip entirely when there is no home yet. This is
        // the #OOBEAUTH first-boot bootstrap session (login_result.home is
        // deliberately "" - see gui/login.c) running as uid FIRST_ADMIN_UID
        // with no matching user-table entry, and userconf_kpath() resolves an
        // unknown uid's home to "/" - root's own home. Without this guard a
        // deployment that ships /CONFIG/KIMI.KEY would have this bootstrap
        // session write and chmod a path under /CONFIG (fat_mkdir + perms_
        // on_create on the fallback "/" + "/CONFIG" join) as uid 1000, which
        // is exactly the kind of write this whole change is supposed to keep
        // out of that session's reach. The seed is simply provisioned one
        // boot later, at the first REAL login this account makes (same
        // no-op-if-already-set logic applies then), which costs nothing:
        // nobody can read Settings during a session with no desktop chrome
        // anyway (g_setup_pending).
        if (login_result.home[0]) provision_ai_key(&login_result);

        // #95: build the background services registry now (parses
        // /CONFIG/SERVICES.CFG). Actually starting the services is deferred
        // to desktop_run(), after the compositor has launched: spawning a
        // user process this early (before any other user process exists)
        // lands on physical pages that are not yet safely writable through
        // the kernel identity map, faulting in elf_load_user. Once the
        // compositor is up the allocator is in the same steady state used
        // for every on-demand app launch, so service spawns are safe.
        // Guarded: build the registry once, not on every login-gate iteration.
        { static int s_svc_inited = 0; if (!s_svc_inited) { svc_init(); s_svc_inited = 1; } }
    gfx_boot_log("[BOOT] Background services registry built");
        // #157: the last kernel stage before the Ring-3 handoff. If the machine
        // stops with THIS as the final line, the fault is in desktop_init()/
        // desktop_run() before the compositor spawn; if it stops on one of
        // desktop_run()'s own lines, the fault is past it. That distinction did
        // not exist before #157 - everything from login_init() onward presented
        // as the same frozen "Starting desktop services..." screen.
        gfx_boot_log("[BOOT] Entering desktop environment...");

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
        boot_stage(BSTAGE_DESKTOP);
        desktop_run();

        // #566 decision 3: the desktop process exited. DO NOT fall through to an
        // unauthenticated Ring-0 shell. Loop back to the login gate so the next
        // thing on screen is authentication, exactly like a fresh boot. On a
        // framebuffer machine this loop never terminates.
        kprintf("[GUI] Desktop exited; returning to login gate (no shell fallback)\n");
        syslog_log(LOG_INFO, "Desktop exited; returning to login gate");
        gfx_boot_log("[BOOT] Desktop exited; re-showing login gate");
        } // end #566 login-gate for(;;)
    }

    // The kernel shell is reachable ONLY when there is no framebuffer at all
    // (headless/serial bring-up, before any GUI session can exist). After a GUI
    // session has authenticated it is unreachable (see the login-gate loop).
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
    // #672: was the weakest of the five parsers, with no flags and no width at
    // all, so every "%04x" in this kernel printed "04x" and consumed nothing.
    // Routed through the one shared parser.
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
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
                        shell_print("  security- Show security posture (#646)\n");
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
                        shell_print("  fsck    - Check the ext2 root (READ-ONLY, no repair)\n");
                        shell_print("  leinfo  - Dump an LE/DOS4GW executable (leinfo /DOS/DOOM.EXE)\n");
                        shell_print("  leload  - Load+relocate an LE, check the post-load invariant\n");
                        shell_print("Diagnostics:\n");
                        shell_print("  hblog   - Show the in-RAM heartbeat ring (no disk write)\n");
                        shell_print("  logflush - Persist the heartbeat ring to /HEARTBEAT.TXT now\n");
                        shell_print("System:\n");
                        shell_print("  shutdown - Power off system (ACPI)\n");
                        shell_print("  reboot  - Reboot system\n");
                        shell_print("GUI:\n");
                        shell_print("  gui     - Start desktop environment\n");
                        shell_print("  wget    - Fetch URL (wget <url>)\n");
                        shell_print("  splash  - Show boot splash screen\n");
                        shell_print("\nPress Ctrl+C to cancel running commands\n");
                    } else if (strcmp(command_buffer, "hblog") == 0) {
                        // #748: the heartbeat ring is RAM-resident between its
                        // 30-minute flushes, so give it a reader that does not
                        // touch the disk at all. Printing it is free; the point
                        // of the change was to stop WRITING it 1800 times an
                        // hour, not to stop anyone looking at it.
                        const char *ring = 0;
                        uint32_t rl = bootlog_heartbeat_ring(&ring);
                        uint64_t beats = 0, flushes = 0;
                        bootlog_heartbeat_stats(&beats, &flushes);
                        uint32_t i = 0;
                        while (i < rl) {
                            char lb[240];
                            uint32_t j = 0;
                            while (i < rl && ring[i] != '\n' && j < sizeof(lb) - 2) lb[j++] = ring[i++];
                            while (i < rl && ring[i] != '\n') i++;   // drop an over-long tail
                            if (i < rl) i++;                          // consume the newline
                            lb[j++] = '\n'; lb[j] = 0;
                            shell_print(lb);
                        }
                        shell_printf("[hblog] %lu bytes, %lu beats recorded, "
                                     "%lu device writes\n",
                                     (unsigned long)rl, (unsigned long)beats,
                                     (unsigned long)flushes);
                    } else if (strcmp(command_buffer, "logflush") == 0) {
                        int rc = bootlog_heartbeat_flush();
                        if (rc == 0) shell_print("[logflush] /HEARTBEAT.TXT persisted\n");
                        else         shell_printf("[logflush] FAILED rc=%d - the ring is "
                                                  "still only in RAM\n", rc);
                    } else if (strcmp(command_buffer, "mem") == 0) {
                        shell_printf("Total RAM: %lu MB\n", g_boot_info->total_memory / MB);
                        shell_printf("Memory map entries: %u\n", g_boot_info->memory_map_entries);
                    } else if (strcmp(command_buffer, "security") == 0) {
                        // #646: security_print_status() gathers the whole
                        // posture into one report and had ZERO callers, so the
                        // report was unreachable and its three sub-printers
                        // with it. Given a call site rather than deleted,
                        // because after the honesty pass it now states the
                        // MEASURED posture (what is randomised, what is not,
                        // SMAP held off, Nova loaded but not enforcing) rather
                        // than reciting feature names.
                        extern void security_print_status(void);
                        security_print_status();
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
                    } else if (strncmp(command_buffer, "leinfo ", 7) == 0) {
                        // #740 Milestone 1. Prints header, object table, page
                        // map, fixup page table and the fixup histogram of an
                        // LE/DOS4GW module. Everything it prints is decoded by
                        // rustkern/le.rs; this arm only routes the path.
                        extern int le_info_dump(const char *path);
                        le_info_dump(&command_buffer[7]);
                    } else if (strncmp(command_buffer, "leload ", 7) == 0) {
                        // #740 Milestone 2. Relocates above the low megabyte,
                        // materialises the module into a kmalloc'd arena,
                        // applies every fixup, then runs the post-load readback
                        // invariant. Prints PASS only if every checked fixup
                        // landed inside a declared object.
                        extern int le_load_test(const char *path, uint32_t delta,
                                                uint32_t *entry, uint32_t *stack);
                        le_load_test(&command_buffer[7], 0, 0, 0);
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
                    } else if (strcmp(command_buffer, "fsck") == 0) {
                        // #610: user-runnable, REPORT-ONLY ext2 check. There is
                        // deliberately no repair mode and no --fix flag: #609 is
                        // this project's own proof that a repairing fsck can
                        // destroy the very data it is asked to save.
                        if (!ext2_is_mounted()) {
                            shell_print("fsck: no ext2 volume mounted\n");
                        } else {
                            uint32_t why = ext2_fsck_needed();
                            shell_printf("fsck: ext2 root; s_state at mount 0x%x, mount count %u%s\n",
                                         (unsigned)ext2_state_at_mount(),
                                         (unsigned)ext2_mnt_count(),
                                         why ? "  [volume wants checking]" : "  [last unmount was clean]");
                            shell_print("fsck: READ-ONLY report. Nothing is repaired.\n");
                            shell_print("fsck: the volume is MOUNTED and live, so a write in flight can\n");
                            shell_print("      surface as a spurious mismatch; the boot-time check (run\n");
                            shell_print("      before userland starts) is the authoritative one.\n");
                            ext2_fsck_report_t *rep =
                                (ext2_fsck_report_t *)kmalloc(sizeof(ext2_fsck_report_t));
                            if (!rep) {
                                shell_print("fsck: out of memory\n");
                            } else {
                                int frc = ext2_fsck_run(rep);
                                if (frc != 0 || !rep->completed) {
                                    shell_printf("fsck: could NOT run (rc=%d). This is not a clean result.\n", frc);
                                } else {
                                    kprintf_set_dual_output(1);
                                    ext2_fsck_print(rep, 0);
                                    kprintf_set_dual_output(0);
                                    shell_printf("fsck: %u problem(s) found\n", (unsigned)rep->total);
                                }
                                kfree(rep);
                            }
                        }
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

// #modeset: what the UEFI loader's GOP enumeration and \boot\MODE.TXT
// selection did. See kernel/boot_info.h for why a count of ZERO means the
// BOOTLOADER is stale rather than the firmware being mode-less.
//
// #ASUSMODE: EVERY LINE HERE GOES TO bootlog_write(), NOT kprintf().
//
// MEASURED: a user booted from a USB stick on an ASUS laptop with "1920x1080"
// in \boot\MODE.TXT and got a 3840x2160 framebuffer. Nothing about that
// decision was recoverable afterwards. This function used kprintf only, so its
// output existed on the serial console and NOWHERE ELSE, and a laptop has no
// serial console; gop_select_mode() Print()s its whole reasoning, but to the
// UEFI console, which scrolls away before the kernel starts. The one artifact
// the user CAN send back is /BOOTLOG.TXT off the stick, and it had nothing.
//
// bootlog_write() ALWAYS mirrors to kprintf, so nothing that used to reach
// serial stops reaching it; the lines are additionally buffered in RAM and land
// on disk when bootlog_arm() runs later in boot. Calling it this early (from
// kernel_main, long before any filesystem is mounted) is the designed use, per
// kernel/fs/bootlog.h. Line length is capped at BOOTLOG_LINE_MAX (256), which
// is why the mode list below is emitted as MANY SHORT LINES and never as one
// long one: a truncated line would silently lose the tail of the evidence.
void print_video_mode_info(void) {
    if (!g_boot_info) return;

    uint64_t n      = g_boot_info->video_mode_count;
    uint64_t cur    = g_boot_info->video_mode_current;
    uint64_t status = g_boot_info->video_mode_status;

    if (n == 0) {
        bootlog_write("[VIDEOMODE] bootloader predates mode selection "
                      "(video_mode_count=0); firmware mode used as-is");
        return;
    }

    const char *what;
    switch (status) {
        case MODESEL_NONE:      what = "no /boot/MODE.TXT, firmware mode kept"; break;
        case MODESEL_APPLIED:   what = "MODE.TXT applied";                      break;
        case MODESEL_REFUSED:   what = "MODE.TXT REFUSED, firmware mode kept";  break;
        case MODESEL_SETFAILED: what = "SetMode FAILED, firmware mode kept";    break;
        case MODESEL_REVERTED:  what = "set but unusable, REVERTED";            break;
        case MODESEL_READERR:   what = "/boot/MODE.TXT PRESENT but unreadable or "
                                       "empty, firmware mode kept";              break;
        default:                what = "unknown status";                        break;
    }
    bootlog_write("[VIDEOMODE] mode %lu of %lu offered by firmware: %s", cur, n, what);

    // ---- the enumerated mode list ------------------------------------------
    // FOUR CHECKS BEFORE ANY DEREFERENCE, and none of them is paranoia. The
    // address arrives in what used to be boot_info.reserved[2], and a reserved
    // slot is NOT reliably zero: an older loader leaves whatever follows
    // g_boot_info in its own BSS there (g_memory_map[] is declared next), so a
    // plain non-zero test would happily dereference a memory-map entry. The tag
    // is the in-band handshake that says the address was actually written by a
    // loader that knows about the blob; hdr->magic then says the memory at that
    // address is the blob and not something that merely followed it; the count
    // cross-check against video_mode_count says the two halves of the loader
    // agree; and the ceiling says a garbage count cannot walk memory. A failure
    // of any of them is LOGGED as a rejection, because "the list is missing" is
    // itself a finding, and silently skipping would look identical to a loader
    // that never had the feature.
    uint64_t tag  = g_boot_info->video_mode_list_tag;
    uint64_t addr = g_boot_info->video_mode_list;
    if (tag != MODELIST_MAGIC || addr == 0) {
        bootlog_write("[VIDEOMODE] mode list REJECTED (no tag: tag=0x%lx addr=0x%lx); "
                      "loader predates the mode list", tag, addr);
        return;
    }

    const modelist_hdr_t *h = (const modelist_hdr_t *)(uintptr_t)addr;
    if (h->magic != MODELIST_MAGIC) {
        bootlog_write("[VIDEOMODE] mode list REJECTED (blob magic 0x%lx at 0x%lx)",
                      h->magic, addr);
        return;
    }
    if (h->count > (uint32_t)MODELIST_MAX) {
        bootlog_write("[VIDEOMODE] mode list REJECTED (count %u exceeds ceiling %u)",
                      h->count, (uint32_t)MODELIST_MAX);
        return;
    }
    if ((uint64_t)h->count != n) {
        bootlog_write("[VIDEOMODE] mode list REJECTED (count %u != video_mode_count %lu)",
                      h->count, n);
        return;
    }
    // The blob carries its own copy of the verdict so the list and the summary
    // cannot drift apart. If they ever do, SAY SO rather than printing both as
    // though they agreed: a diagnostic that quietly contradicts itself is worse
    // than one that is missing.
    if ((uint64_t)h->status != status) {
        bootlog_write("[VIDEOMODE] WARNING: blob status %u disagrees with "
                      "video_mode_status %lu", h->status, status);
    }

    if (h->req_w != 0 && h->req_h != 0) {
        bootlog_write("[VIDEOMODE] MODE.TXT requested %ux%u", h->req_w, h->req_h);
    } else if (h->req_idx != MODELIST_NO_REQ) {
        bootlog_write("[VIDEOMODE] MODE.TXT requested mode index %u", h->req_idx);
    } else {
        // #DIAGLOG: name WHICH of the three it was. The old line listed all
        // three possibilities and left the reader to guess, which is the same
        // failure as not logging it: someone who has just written MODE.TXT onto
        // a stick cannot tell "you spelled the path wrong" from "your file is
        // there and does not parse" from "I could not read your filesystem".
        const char *why =
            (status == MODESEL_NONE)    ? "no such file on the ESP" :
            (status == MODESEL_READERR) ? "file present but unreadable or empty" :
            (status == MODESEL_REFUSED) ? "file present and did NOT parse" :
                                          "no request recorded";
        bootlog_write("[VIDEOMODE] MODE.TXT: no usable request (%s)", why);
    }

    // The RAW list, in firmware order, deduplicated NOWHERE. Duplicates and
    // oddities are exactly what we are looking for, so nothing is collapsed and
    // nothing is sorted. A trailing "*" marks the mode that was current on entry
    // to the loader's selection; "/fmtN" appears only for a pixel format that is
    // not one of the two ordinary 8-8-8-8 kinds (2 = bitmask, 3 = BltOnly, which
    // has no linear framebuffer at all), because printing the same expected
    // value on every line is thirty repetitions of nothing.
    const modelist_ent_t *e = (const modelist_ent_t *)(h + 1);
    uint32_t i = 0;
    while (i < h->count) {
        char body[160];
        char tok[64];
        uint32_t first = i;
        uint32_t k     = 0;
        size_t   bo    = 0;
        body[0] = '\0';
        // Two independent bounds, six tokens and 110 characters, so a firmware
        // reporting implausibly large numbers shortens the line instead of
        // pushing it past BOOTLOG_LINE_MAX and losing the tail.
        while (i < h->count && k < 6 && bo < 110) {
            if (e[i].flags & MODELIST_F_QUERYFAIL) {
                snprintf(tok, sizeof(tok), "%u:query-failed ", i);
            } else if (e[i].pixel_format > 1) {
                snprintf(tok, sizeof(tok), "%u%s:%ux%u/fmt%u ", i,
                         (e[i].flags & MODELIST_F_CURRENT) ? "*" : "",
                         e[i].width, e[i].height, e[i].pixel_format);
            } else {
                snprintf(tok, sizeof(tok), "%u%s:%ux%u ", i,
                         (e[i].flags & MODELIST_F_CURRENT) ? "*" : "",
                         e[i].width, e[i].height);
            }
            snprintf(body + bo, sizeof(body) - bo, "%s", tok);
            bo = strlen(body);   // measured, not assumed from a return value
            i++;
            k++;
        }
        bootlog_write("[VIDEOMODE] modes %u-%u: %s", first, i - 1u, body);
    }

    // ---- the one line that settles the ASUS question -----------------------
    // Everything above is context; this says whether the firmware ever offered
    // what was asked for. "NOT OFFERED" means the refusal was correct and the
    // panel is the constraint; "PRESENT" with a non-APPLIED status above means
    // the loader had the mode available and still did not end up on it, which is
    // a bug in the selection and not in the hardware.
    if (h->req_w != 0 && h->req_h != 0) {
        uint32_t found = MODELIST_NO_REQ;
        uint32_t ffmt  = 0;
        for (i = 0; i < h->count; i++) {
            if (e[i].flags & MODELIST_F_QUERYFAIL) continue;
            if (e[i].width == h->req_w && e[i].height == h->req_h) {
                found = i;
                ffmt  = e[i].pixel_format;
                break;
            }
        }
        if (found != MODELIST_NO_REQ) {
            bootlog_write("[VIDEOMODE] requested %ux%u: PRESENT at index %u (fmt=%u)",
                          h->req_w, h->req_h, found, ffmt);
        } else {
            bootlog_write("[VIDEOMODE] requested %ux%u: NOT OFFERED by this firmware",
                          h->req_w, h->req_h);
        }
    } else if (h->req_idx != MODELIST_NO_REQ) {
        if (h->req_idx < h->count) {
            bootlog_write("[VIDEOMODE] requested index %u: PRESENT, %ux%u (fmt=%u)",
                          h->req_idx, e[h->req_idx].width, e[h->req_idx].height,
                          e[h->req_idx].pixel_format);
        } else {
            bootlog_write("[VIDEOMODE] requested index %u: OUT OF RANGE "
                          "(firmware offers 0..%u)", h->req_idx, h->count - 1u);
        }
    }
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
