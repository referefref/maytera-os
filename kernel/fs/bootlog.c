// bootlog.c - Persistent, crash/hang-safe on-disk boot log.
//
// Real-hardware bring-up bug (#307 follow-up, iMac14,4 live-USB): the user
// has ZERO remote telemetry from the physical machine (no serial, no SSH), so
// the existing serial-only kprintf() log is useless for diagnosing a boot
// that hangs or reaches an unusable state. This module mirrors selected
// kprintf() diagnostics into /BOOTLOG.TXT on the FAT root so the user can
// retrieve it by plugging the USB stick into another computer after a failed
// boot.
//
// Durability design:
//   - bootlog_write() ALWAYS appends to an in-RAM buffer, from the very first
//     call in main() (before any filesystem exists). Nothing is lost even if
//     the disk never becomes writable.
//   - bootlog_arm(fs), called once as soon as the FAT root is mounted,
//     immediately flushes everything accumulated so far (this is how
//     pre-mount xHCI/USB-MSC enumeration details make it into the file even
//     though the file obviously could not be written before the mount that
//     it is itself reporting on).
//   - After arming, EVERY bootlog_write() call reaches the medium before it
//     returns, so a hard hang or power-cut immediately after any line still
//     leaves that line on disk. That per-line durability is the whole point of
//     the file and is NOT negotiable: this is the only post-mortem channel the
//     iMac14,4 has.
//   - #748: it reaches the medium as an INCREMENTAL APPEND of just the new
//     bytes (ext2_append_file, #598), not as a rewrite of the whole file. The
//     original text here read "A boot logs on the order of ~100-300 short
//     lines, so total write volume for one boot is a few hundred KB at most".
//     The line count was right and the conclusion was wrong: a full rewrite per
//     line makes one boot an O(n^2) series, and with the ~25x device
//     amplification measured on this stack that came to MOST of the 7.35 MB a
//     boot wrote. Appending keeps every durability property above and makes the
//     cost per line independent of the log so far. On FAT (the #348 single-
//     partition live-USB image) there is no append primitive and the old
//     whole-file rewrite is still used; see bootlog_persist().
// #693: the bootlog IS the persistence of last resort, so it is the one writer
// that CANNOT report its own failure into itself. Recursing into bootlog_write()
// from a failed bootlog flush would loop on a wedged disk. The only channel left
// is the serial console, and it is rate-limited to one line per path so a broken
// disk cannot turn the log into the flood.
// #742 completes that thought. Reporting on serial is necessary and was not
// sufficient: the breadcrumb file exists precisely BECAUSE serial is silent in
// GUI mode and on the real iMac, so "we told you on serial" tells the one reader
// who cannot listen. Three things are added here:
//
//   1. bootlog_write() and friends RETURN a status, so a caller that cares can
//      act. They are deliberately NOT MUST_CHECK: at 410 call sites, all of them
//      logging, an attribute would produce 410 IGNORE_RESULT ceremonies and
//      nobody would read any of them. The value is there for the callers who
//      want it, and bootlog_persist_failures() is there for the rest.
//
//   2. THE FAILURE IS RECORDED IN-BAND, in the RAM buffer. This works because of
//      a property of the design that is easy to miss: every flush rewrites the
//      WHOLE buffer, so a failed flush is SELF-HEALING - if any later flush
//      succeeds, everything the failed one would have written lands anyway. The
//      only permanent loss is a sink that never succeeds again. So the honest
//      artifact is a log that, once the medium recovers, CONTAINS the sentence
//      "this flush failed". We append that to the buffer and deliberately do NOT
//      re-flush it (that is the recursion the note below warns about).
//
//   3. bootlog_persist_failures() lets main.c, the shell, or a test say out loud
//      that the breadcrumb file is not trustworthy evidence.
// #748 WRITE COST. Measured on the golden with QEMU blockstats on the
// usb-storage device: 7.35 MB written before the desktop settled, then
// 2.49 MB and 3.28 MB over two untouched idle minutes, i.e. 43-57 KB/s to a
// USB flash stick at COMPLETE IDLE, indefinitely. Two DIFFERENT faults, and
// they need OPPOSITE fixes, so do not collapse them into one rule:
//
//   /BOOTLOG.TXT is the BOOT cost and writes ZERO at idle (138 writes, all
//   during boot). Its cost is that every line rewrote the WHOLE accumulated
//   file, so one boot is an O(n^2) series of full-file rewrites. Its VALUE is
//   surviving a boot that HANGS on a machine with no serial port, which is the
//   only post-mortem channel the iMac14,4 has, so per-line durability MUST
//   survive the fix. Fixed by making it an INCREMENTAL APPEND
//   (ext2_append_file, #598) instead of a rewrite: same line-by-line
//   durability, cost per line no longer proportional to the log so far.
//
//   /HEARTBEAT.TXT is the IDLE bleed. Its ring is correctly bounded (this was
//   already fixed once, for #373), but the whole ~4 KB of it was rewritten
//   EVERY 2 SECONDS, forever. It is a debug trace, and a wedge is already
//   recorded by /PANIC.TXT, so continuous persistence buys very little. Fixed
//   by keeping the ring in RAM and persisting it only on a schedule, on the
//   starvation anomaly it exists to catch, and on panic.
//
// WHY "PUT IT ON THE RAMDISK" IS NOT AN OPTION, do not re-propose it: there is
// no RAM-only path below the filesystem. blockdev.c's blk_write() calls
// usb_msc_write() UNCONDITIONALLY and only then updates the RAM copy for
// coherence, and its comment names these two files as the reason. The ring has
// to live ABOVE the filesystem, which is exactly where it already is.
//
// AND OPTIMISE FOR FEWER FLUSH EVENTS, NOT SMALLER ONES. Device traffic was
// measured at ~25x the logical bytes rewritten (0.5 rewrites/s of 4 KB versus
// 12.8-16.8 device write ops/s), the same amplification ext2.c:2063 records
// (151,826 blk_write() calls = 1,062,742 sectors). A change that halves the
// bytes but keeps a 2-second cadence is worth almost nothing.
static void bootlog_flush_failed(const char *path, int rc);
static void bootlog_write_esp_map(void);

#include "bootlog.h"
#include "ext2.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/mono.h"
#include "../sync/noblock.h"   // #745 (task #69): wq_noblock_reason()
#include <stdarg.h>

static void bootlog_append_raw(const char *s, uint32_t len);
static void bl_fault_drain_into_buf(void);

static uint32_t g_persist_failures = 0;
static int      g_last_persist_rc  = 0;

static void bootlog_flush_failed(const char *path, int rc) {
    static const char *last_path = 0;
    g_persist_failures++;
    g_last_persist_rc = rc;
    // In-band, into the RAM buffer only. NEVER re-flush here: that is the loop
    // on a wedged disk the header warns about. A later successful flush of any
    // sink that shares this buffer carries the note to the medium for free.
    {
        char note[128];
        int n = snprintf(note, sizeof(note),
                         "[BOOTLOG] FLUSH FAILED rc=%d path=%s (this file was "
                         "INCOMPLETE at this point)\n", rc, path);
        if (n > 0) bootlog_append_raw(note, (uint32_t)((unsigned)n < sizeof(note) ? (unsigned)n : sizeof(note) - 1));
    }
    if (last_path == path) return;    // one SERIAL report per sink, not per call
    last_path = path;
    kprintf("[BOOTLOG] PERSIST FAILED rc=%d path=%s - this log is INCOMPLETE "
            "and must not be trusted as evidence\n", rc, path);
}

uint32_t bootlog_persist_failures(void) { return g_persist_failures; }
int      bootlog_last_persist_rc(void)  { return g_last_persist_rc; }

#define BOOTLOG_BUF_CAP  (96 * 1024)
#define BOOTLOG_PATH     "/BOOTLOG.TXT"
#define BOOTLOG_LINE_MAX 256

// NOTE: /BOOTLOG.TXT is a plain root-level path (deliberately, for easy
// discovery when the user plugs the USB stick into another computer), so on
// an ext2-root system (#99, /ROOTEXT2 marker present) fat_write_file() will
// route it to the ext2 volume once g_root_ext2 flips on, same as any other
// non-/boot non-/EFI path. The #348 live-USB image this feature targets has
// no ext2 partition at all (single FAT32 root, by design), so this does not
// affect the real-hardware scenario it was built for.

// ===========================================================================
// SILENT TRUNCATION IS HOW A DIAGNOSTIC LINE LIES. (deadport)
// ===========================================================================
// Every writer in this file formats into a fixed 256-byte buffer and used to
// clamp an over-long line with a bare
//
//     if ((uint32_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
//
// which drops the tail and says nothing. MEASURED on the owner's ASUS boot log
// from golden 2277 (/ssdmirror/asus-logs-2277/BOOTLOG.TXT): 14 lines were cut
// at exactly 255 characters, and the two that mattered most were among them.
//
//  1. The #307 xHCI give-up line is ~700 characters. It was cut mid-word at
//     "...(unplug/replug). Thi", so the operator lost the ENTIRE second half:
//     the /USBPORT.CFG override syntax, which the driver's own comment says was
//     spelled out on purpose "so an operator can act on this line without
//     reading any source". The intent was written down; the delivery failed
//     silently, and nothing in the log admitted it had.
//  2. The [xHCI-HB] per-port state map, the mechanism that makes a retired port
//     visible on EVERY scan, was cut at port 14 of the owner's 21.
//
// So the clamp now STAMPS the line with how many bytes went missing. It costs a
// few instructions on a path that runs a few times a second at most, and it
// turns an invisible failure into one that names itself. A reader who sees
// "~CUT n~" knows to go looking; a reader of a silently cut line does not know
// there is anything to look for. This does NOT fix an over-long line, and is
// not meant to: it makes the next one findable instead of undetectable.
//
// The counters are exported so the boot self-test can assert on them rather
// than anyone having to eyeball a log.
static uint32_t g_bl_trunc_lines = 0;
static uint32_t g_bl_trunc_bytes = 0;
uint32_t bootlog_truncated_lines(void) { return g_bl_trunc_lines; }
uint32_t bootlog_truncated_bytes(void) { return g_bl_trunc_bytes; }

// Record a truncated line, stamping the loss into the line itself. `n` is the
// bytes vsnprintf_dropped() actually wrote and `dropped` is what would not fit.
// Returns the usable length, or -1 if the format failed.
//
// CORRECTED after the first VM verification run, and the correction is the
// whole point of the mechanism: the first version of this function tested
// `if ((uint32_t)n >= cap)`, the C99 truncation idiom, AND THAT TEST CAN NEVER
// BE TRUE HERE. This kernel's vsnprintf returns bytes ACTUALLY WRITTEN, capped
// at cap-1 (see the note above vsnprintf_dropped in string.c). So the detector
// I wrote to catch silent truncation was itself silently incapable of firing,
// exactly like the four-copy clamp it replaced. MEASURED on the test boot that
// caught it: three lines sat at exactly 255 characters, cut mid-word, one of
// them the port-governor self-test line added by this very change, while the
// audit line confidently reported "0 line(s) truncated". A detector that cannot
// fire is worse than no detector, because it answers the question wrongly.
static int bl_clamp(char *line, uint32_t cap, int n, size_t dropped) {
    if (n < 0) return -1;
    if (dropped == 0) return n;               // fitted; not touched at all
    g_bl_trunc_lines++;
    g_bl_trunc_bytes += (uint32_t)dropped;
    // Overwrite the tail rather than append: there is by definition no room to
    // append.
    char tag[28];
    int t = snprintf(tag, sizeof(tag), "~CUT %lu~", (unsigned long)dropped);
    if (t > 0 && (uint32_t)t < cap && n >= t) {
        memcpy(line + (uint32_t)n - (uint32_t)t, tag, (uint32_t)t);
    }
    return n;
}

static char     g_bootlog_buf[BOOTLOG_BUF_CAP];
static uint32_t g_bootlog_len = 0;
static int      g_bootlog_armed = 0;
static int      g_bootlog_full_warned = 0;
static fat_fs_t *g_bootlog_fs = 0;
static uint32_t g_bootlog_persisted = 0;   // #748: bytes of g_bootlog_buf on disk

// ===========================================================================
// #748 THE ONE PERSIST PRIMITIVE for every breadcrumb file in this module.
// ===========================================================================
// Three of the four files (/BOOTLOG.TXT, /USBLOG.TXT, /AUDIOLOG.TXT) are
// APPEND-ONLY streams: their RAM buffer only ever grows, so the medium only
// ever needs the bytes it has not seen. `*persisted` is how many of those bytes
// are already down. Pass persisted = 0 for a file whose buffer is a RING whose
// content SHIFTS (/HEARTBEAT.TXT drops whole lines off the front), because an
// append is then simply the wrong operation and only a whole-file replace is
// correct.
//
// Three properties worth stating because each one was a bug in some earlier
// version of this file:
//
//   - THE FIRST WRITE IS ALWAYS A REPLACE (*persisted == 0). The file may not
//     exist, or may be a stale copy from the PREVIOUS boot; appending to that
//     would splice two boots together into one unreadable file.
//   - A FAILED APPEND FALLS BACK TO A REPLACE, which both retries and REPAIRS:
//     a replace writes the exact RAM content, so even a partial append is
//     corrected. This is the self-healing property #742 relies on. The one
//     case where it would DESTROY data is when the RAM buffer has stopped
//     growing (buffer full) while the medium is ahead of it, so the fallback
//     is suppressed there and the failure is reported instead.
//   - FAT GETS THE OLD BEHAVIOUR. ext2_append_file() (#598) is ext2-only and
//     the #348 live-USB image is single-FAT by design, so on FAT this stays a
//     whole-file rewrite exactly as before. Nothing regresses; ext2-root
//     (every golden) gets the O(n) path.
static int bootlog_persist(const char *path, const char *buf, uint32_t len,
                           uint32_t *persisted, int buf_full) {
    if (!g_bootlog_armed || !g_bootlog_fs) return 0;

    if (persisted) {
        if (*persisted > len) *persisted = 0;   // buffer rewound: replace it
        if (*persisted == len) return 0;        // nothing new to say
        if (*persisted > 0 && fat_path_on_ext2(g_bootlog_fs, path)) {
            int rc = ext2_append_file(fat_ext2_vol_path(path),
                                      buf + *persisted, len - *persisted);
            if (rc == 0) { *persisted = len; return 0; }
            if (buf_full) return rc;   // see comment above
        }
    }

    int rc = fat_write_file(g_bootlog_fs, path, buf, len);
    if (rc == 0 && persisted) *persisted = len;
    return rc;
}

// ===========================================================================
// #134 THE ONE APPENDER for every breadcrumb sink in this file.
// ===========================================================================
// There were THREE copies of this function (bootlog / usblog / audiolog),
// identical apart from which buffer they wrote, and all three shared one
// defect: on reaching the cap they dropped every further line and said so with
// kprintf() ONLY. On the iMac14,4, which has no serial console, that is
// silence: the exact trap blame.md has recorded since #130.
//
// THE CAP IS NOT THEORETICAL. Measured on golden 1909 in a VM: the #135
// port-flap diagnostics cost ~1.5 KB of /BOOTLOG.TXT per disconnect+replug
// cycle (PORTSC was/now + PORTCTX + re-scan + detach + teardown + the whole
// re-enumeration). The owner's root port 4 flaps by itself every ~3 s, so the
// 96 KB buffer fills in MINUTES; from that moment g_bootlog_len stops growing,
// bootlog_persist() sees "nothing new to say" and returns 0, and /BOOTLOG.TXT
// is FROZEN at that byte for the rest of the session while the serial-only
// warning goes nowhere. #134 was reported at 6:50 uptime, i.e. long after the
// log had quietly stopped recording. That is why the freeze left no trace.
//
// TWO fixes, in this one place, for all three sinks:
//
//   1. NEVER STOP SILENTLY. The last BL_TRUNC_RESERVE bytes of every buffer are
//      held back for a truncation notice, so a file pulled off the stick ENDS
//      with a line that says it was truncated, instead of simply stopping
//      mid-session and looking exactly like a machine that died there.
//
//   2. NEVER REACH THE CAP ON AN APPEND-CAPABLE SINK. bootlog_persist() already
//      writes only the delta on ext2 (#748), so bytes already on the medium do
//      not need to stay in RAM. Dropping that persisted prefix turns the buffer
//      into a sliding window over the UNPERSISTED tail and the cap stops being
//      a countdown. BL_KEEP_PERSISTED bytes of the prefix are deliberately
//      RETAINED so *persisted stays > 0: bootlog_persist() reads *persisted == 0
//      as "first write, REPLACE the whole file", so compacting to zero would
//      truncate /BOOTLOG.TXT to its own tail. On FAT (the #348 single-partition
//      live USB) every flush is a whole-file rewrite, so the prefix is still
//      needed and compaction is correctly skipped there: that path keeps
//      exactly its old behaviour plus the notice from (1).
#define BL_TRUNC_RESERVE  192u
#define BL_KEEP_PERSISTED 512u

static uint64_t g_bl_dropped_bytes = 0;   // across all sinks, for reporting

// Set once a sink's RAM buffer has been compacted, i.e. once it no longer holds
// the whole file. Passed to bootlog_persist() as buf_full is, to suppress the
// whole-file replace fallback that would otherwise destroy the earlier content.
static int g_bootlog_compacted  = 0;
static int g_usblog_compacted   = 0;
static int g_audiolog_compacted = 0;

uint64_t bootlog_dropped_bytes(void) { return g_bl_dropped_bytes; }

static void bl_append(char *buf, uint32_t cap, uint32_t *len, uint32_t *persisted,
                      int *truncated, int *compacted, const char *name,
                      const char *path, const char *s, uint32_t slen) {
    if (slen == 0 || slen >= cap) return;

    if (*len + slen + BL_TRUNC_RESERVE < cap) {
        memcpy(buf + *len, s, slen);
        *len += slen;
        return;
    }

    // Reclaim the prefix that is already on the medium, if this sink appends.
    if (persisted && *persisted > BL_KEEP_PERSISTED && g_bootlog_armed &&
        g_bootlog_fs && fat_path_on_ext2(g_bootlog_fs, path)) {
        uint32_t drop = *persisted - BL_KEEP_PERSISTED;
        if (drop > *len) drop = *len;
        if (drop) {
            memmove(buf, buf + drop, *len - drop);
            *len       -= drop;
            *persisted -= drop;
            // ONCE THIS HAPPENS THE RAM BUFFER IS NO LONGER THE WHOLE FILE, so
            // bootlog_persist()'s repair fallback (a whole-file fat_write_file
            // replace when an ext2 append fails) would TRUNCATE /BOOTLOG.TXT to
            // the retained window. That fallback is already suppressed for the
            // buffer-full case for exactly this reason; compaction has the same
            // semantics, so it sets the same suppression. The failure is then
            // reported instead of silently destroying the evidence, which is
            // the whole point of the file.
            *compacted = 1;
        }
        if (*len + slen + BL_TRUNC_RESERVE < cap) {
            memcpy(buf + *len, s, slen);
            *len += slen;
            return;
        }
    }

    // No room even after compaction: say so IN the file, exactly once.
    g_bl_dropped_bytes += slen;
    if (!*truncated) {
        *truncated = 1;
        char note[BL_TRUNC_RESERVE];
        int n = snprintf(note, sizeof(note),
                         "[%s] *** IN-RAM BUFFER FULL (%u bytes). THIS FILE IS "
                         "TRUNCATED HERE - the machine did NOT stop at this "
                         "line. Further lines are serial-only. ***\n",
                         name, (unsigned)cap);
        if (n > 0 && (uint32_t)n < cap - *len) {
            memcpy(buf + *len, note, (uint32_t)n);
            *len += (uint32_t)n;
        }
        kprintf("[%s] in-RAM buffer full (%u bytes); further lines are serial-only\n",
                name, (unsigned)cap);
    }
}

static void bootlog_append_raw(const char *s, uint32_t len) {
    bl_append(g_bootlog_buf, BOOTLOG_BUF_CAP, &g_bootlog_len, &g_bootlog_persisted,
              &g_bootlog_full_warned, &g_bootlog_compacted, "BOOTLOG", BOOTLOG_PATH, s, len);
}

int bootlog_is_armed(void) {
    return g_bootlog_armed;
}

// ---------------------------------------------------------------------------
// #745 (task #62): THE LOGGER MUST NEVER WRITE TO THE MEDIUM FROM INSIDE THE
// STORAGE STACK. See the ticket comment in usb_msc.c for the exact deadlock
// this closes; the short version is that /BOOTLOG.TXT lives on the very device
// whose driver was logging its own transfer errors, so a log line issued from
// inside a SCSI command re-entered that driver's non-recursive command lock.
//
// TWO INDEPENDENT GUARDS, either of which alone breaks the loop:
//
//   g_bl_defer   an explicit window a caller opens around work that must not
//                be logged-to-disk underneath (usb_msc_transport takes it
//                around a whole command). Counted, not just flagged.
//   g_bl_in_persist  a re-entrancy guard on the flush itself, so no future
//                path can recurse into a flush regardless of who calls what.
//
// NEITHER LOSES A LINE. The line always reaches serial and the RAM buffer; only
// the device write is skipped, and the next bootlog_write() from a safe context
// persists the whole accumulated delta. bootlog_arm() and the panic path flush
// the entire buffer unconditionally.
// ---------------------------------------------------------------------------
// #745 (task #69): THE THIRD GUARD, AND THE ONLY ONE THAT NEEDS NO CALLER TO
// REMEMBER ANYTHING.
//
// The two guards above are both CALLER-DRIVEN: g_bl_defer only fires because
// usb_msc_transport() remembers to open the window, and g_bl_in_persist only
// fires on re-entry through the flush itself. Neither one can see the OTHER
// way into the storage stack, which is a caller that is not in the storage
// stack at all but holds a lock with interrupts off:
//
//   nic_send()                       kernel/net/net.c  - takes net_lock(),
//                                    which does `cli` and keeps IF clear for
//                                    the whole hold
//     -> usb_eth_send()              kernel/drivers/usb_ecm.c
//       -> usbnet_bulk_out()         calls bootlog_write() on its diagnostic
//                                    and TX-submit-failure paths
//         -> bootlog_persist()       -> fat/ext2 write -> blk_write()
//           -> usb_msc_transport()   -> msc_cmd_lock()
//             -> msc_cmd_lock_noblock()   because wq_may_block() is FALSE here
//
// msc_cmd_lock_noblock() is a `while (test_and_set)` with NO bound: it waits
// for a lock that a DIFFERENT thread holds, from a context that cannot be
// preempted (IF=0) and must not park. That is an unbounded wait entered with a
// global lock held and interrupts disabled, i.e. the whole machine stops until
// the other thread happens to finish - and the scheduler cannot run to let it.
// usb_msc.c's own comment asserts this branch is "unreachable by construction
// ... because every runtime caller reaches msc_cmd_lock() with interrupts on".
// That claim is false, and this is the path that falsifies it.
//
// WHY THE GUARD LIVES HERE AND NOT AT THE CALL SITE. Removing the
// bootlog_write() from usbnet_bulk_out() (also done, #745 task #69) fixes ONE
// caller. The rule is "nothing may enter the storage stack from a context that
// cannot afford an unbounded wait", and the only place that can be enforced for
// every present and future caller is the logger's own device-write decision.
// Same reasoning as #514: a stated rule is not a control.
//
// THE CONDITION. wq_noblock_reason() (sync/noblock.h) is the canonical
// statement of "may this context park", so this reuses it rather than growing a
// second private notion of the same thing. We defer on IRQ_OFF, but ONLY once
// the scheduler is live: before proc_init() the kernel is single-threaded, so
// msc_cmd_lock() is UNCONTENDED by construction and the write is safe. That
// exception is load-bearing - deferring the whole pre-scheduler phase would
// mean a machine that hangs during boot writes no /BOOTLOG.TXT at all, and on
// the real iMac that file is the only telemetry there is.
//
// NOTHING IS LOST. The line still reaches serial and the RAM buffer; only the
// device write is skipped, and the next bootlog_write() from a safe context
// persists the whole accumulated delta, exactly like the other two guards.
static volatile int      g_bl_defer     = 0;
static volatile int      g_bl_in_persist = 0;
uint64_t g_bootlog_flushes_deferred = 0;   // device writes suppressed by a guard

// #745 (task #69): device writes declined specifically because the CALLING
// CONTEXT had interrupts off with the scheduler live. Separate from
// g_bootlog_flushes_deferred on purpose: bldef= counts the storage stack
// protecting itself (expected, benign), and this counts a no-block context
// being turned away at the door (a real #426 site, and until it is non-zero
// nobody has watched this guard fire). Reported on [HB] as blgnb=.
uint64_t g_bootlog_noblock_defers = 0;
// Return address of the first caller ever turned away, for addr2line. One
// address is enough: it names the offending path, and a second field that is
// almost always the same value is noise on a 416-byte heartbeat record.
uint64_t g_bootlog_noblock_ra = 0;

void bootlog_defer_begin(void) { g_bl_defer++; }
void bootlog_defer_end(void)   { if (g_bl_defer > 0) g_bl_defer--; }

// ===========================================================================
// #134 FAULT-CONTEXT LOGGING: a lock-free ring a later safe context flushes.
// ===========================================================================
// WHY bootlog_write() CANNOT BE CALLED FROM A FAULT HANDLER, and why this is
// not a style preference. cpu/idt.c's exception_fatal() was DELIBERATELY
// converted to kprintf_nolock() in 240dc9f (#130) because a core that faults
// while holding g_console_lock re-enters the handler, calls kprintf(), and
// blocks on a lock it already owns with interrupts off. MEASURED: four CPUs
// spinning at spinlock.h:251 on g_console_lock, desktop never reached, not one
// line of output. bootlog_write() would reintroduce that deadlock and add two
// more of its own: it calls kprintf() (the console lock), and its flush enters
// fat/ext2 -> blk_write -> usb_msc_transport, which takes the MSC command lock
// and can park - from a context that must never park, on the very device the
// faulting thread may already have been inside.
//
// So the fault path gets its OWN sink with exactly three properties:
//   - NO LOCK. Space is reserved with a compare-and-swap on g_fault_len. A CAS
//     loop makes unconditional progress; it is not a wait, so there is nothing
//     for it to deadlock against.
//   - NO ALLOCATION and NO FILESYSTEM. The text is formatted into a stack
//     buffer and memcpy'd into a static ring. Nothing reaches the medium here.
//   - MIRRORED WITH kprintf_nolock(), the lock-free console writer 240dc9f
//     already chose for this context, so a developer with a serial cable sees
//     the same line at the same instant.
//
// A LATER SAFE CONTEXT FLUSHES IT. bootlog_write() drains the ring into the
// ordinary /BOOTLOG.TXT buffer on its next call, and bootlog_heartbeat() (a
// normal kernel thread, every 2 s) calls bootlog_fault_flush() so a fault is
// on disk within about two seconds even if nothing else logs.
//
// KNOWN AND DELIBERATE LIMIT, do not read more into this than it does: for a
// KERNEL-mode fault the CPU halts inside the handler, so no safe context ever
// runs and this ring dies with it. That case is already covered by
// panic_log_write() -> /boot/PANIC.TXT (#418), which is a raw unlocked
// single-sector write and is the only thing that CAN work there. What this
// closes is the USER-mode fault (the #153 "app launches and instantly dies"
// shape), where the kernel keeps running and, until now, left no record on a
// machine with no serial port.
#define BL_FAULT_CAP  2048u
static char              g_fault_buf[BL_FAULT_CAP];
static volatile uint32_t g_fault_len  = 0;
static volatile uint32_t g_fault_lost = 0;
uint32_t bootlog_fault_lost(void) { return g_fault_lost; }

void bootlog_fault_write(const char *fmt, ...) {
    char line[BOOTLOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    size_t bl_dropped = 0;
    int n = vsnprintf_dropped(line, sizeof(line), fmt, args, &bl_dropped);
    va_end(args);
    if (n < 0) return;
    n = bl_clamp(line, (uint32_t)sizeof(line), n, bl_dropped);
    line[n] = '\n';
    uint32_t need = (uint32_t)n + 1;

    // Lock-free reservation, and deliberately LOOP-FREE: one atomic add claims
    // the space or overshoots the ring, and an overshoot is simply recorded as
    // lost. A retry loop here would be a hand-rolled wait in a fault handler,
    // which is the one place that must never wait for anything - and the
    // concurrency lint is right to reject it. g_fault_len is allowed to run
    // past the cap; the drain clamps it and resets it to zero.
    uint32_t off = __sync_fetch_and_add(&g_fault_len, need);
    if (off + need <= BL_FAULT_CAP) memcpy(g_fault_buf + off, line, need);
    else __sync_fetch_and_add(&g_fault_lost, need);

    // The lock-free console writer 240dc9f already chose for this context.
    kprintf_nolock("[FAULTLOG] %s", line);
}

// Move whatever a fault context recorded into the ordinary /BOOTLOG.TXT RAM
// buffer. Safe contexts only; does NOT touch the medium (the caller's own
// bootlog_write() persists the whole accumulated delta straight afterwards).
// NUL bytes are skipped: a reservation whose memcpy had not landed yet reads as
// zeros, and a text log should not carry them.
static void bl_fault_drain_into_buf(void) {
    if (!g_fault_len) return;
    uint32_t n = __sync_lock_test_and_set(&g_fault_len, 0);
    if (n > BL_FAULT_CAP) n = BL_FAULT_CAP;
    uint32_t start = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (g_fault_buf[i] == '\0') {
            if (i > start) bootlog_append_raw(g_fault_buf + start, i - start);
            start = i + 1;
        }
    }
    if (n > start) bootlog_append_raw(g_fault_buf + start, n - start);
}

int bootlog_fault_flush(void) {
    if (!g_fault_len) return 0;
    uint32_t lost = g_fault_lost;
    bl_fault_drain_into_buf();
    // Reuses bootlog_write() wholesale, so the fault text inherits every guard
    // (defer / in-persist / no-block) and every durability property the normal
    // path has, and costs ONE extra line rather than a second write path.
    return bootlog_write("[BOOTLOG] the [FAULTLOG] line(s) above were recorded from a "
                         "FAULT context (no lock, no filesystem) and flushed here%s",
                         lost ? " - SOME WERE LOST, the fault ring overflowed" : "");
}


int bootlog_write(const char *fmt, ...) {
    // #134: carry anything a fault context left in the lock-free ring down with
    // this line, so it costs no extra device write.
    bl_fault_drain_into_buf();

    char line[BOOTLOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    size_t bl_dropped = 0;
    int n = vsnprintf_dropped(line, sizeof(line), fmt, args, &bl_dropped);
    va_end(args);
    if (n < 0) return -1;
    n = bl_clamp(line, (uint32_t)sizeof(line), n, bl_dropped);

    // Mirror to the existing serial log (additive, never replaces it).
    kprintf("[BOOTLOG] %s\n", line);

    // Always buffer in RAM, armed or not - see file header. The newline is
    // written INTO `line` (vsnprintf capped n at sizeof(line)-1, so index n is
    // always spare) so the RAM append and the disk append are the same single
    // contiguous run of bytes: on ext2 that is ONE ext2_append_file()
    // transaction per line rather than two.
    line[n] = '\n';
    bootlog_append_raw(line, (uint32_t)n + 1);

    // #745 (task #62): if the storage stack is busy underneath us, or we are
    // already inside a flush, the line stays in RAM + serial and the medium is
    // left alone. This is the whole fix: the recursion cannot start.
    if (g_bl_defer || g_bl_in_persist) {
        g_bootlog_flushes_deferred++;
        return 0;
    }

    // #745 (task #69): third guard, see the block comment above. A context with
    // interrupts off (and the scheduler live, so contention is possible) must
    // not enter the storage stack, because msc_cmd_lock()'s no-block fallback
    // there is an UNBOUNDED wait it cannot be preempted out of.
    {
        uint32_t _nb = wq_noblock_reason();
        if ((_nb & WQ_NB_IRQ_OFF) && !(_nb & WQ_NB_NO_SCHED)) {
            if (g_bootlog_noblock_defers++ == 0) {
                g_bootlog_noblock_ra = (uint64_t)__builtin_return_address(0);
                // Serial only, and once: we are inside somebody's cli region.
                kprintf("[BLGNB] #745/#69: bootlog device write declined - "
                        "caller has IRQs off (ra=%p). The line is in RAM+serial "
                        "and persists from the next safe context. This guard is "
                        "NOT dead code.\n", (void *)g_bootlog_noblock_ra);
            }
            g_bootlog_flushes_deferred++;
            return 0;
        }
    }

    if (g_bootlog_armed && g_bootlog_fs) {
        // #745 (task #62): count what this write is about to cost the medium.
        // On ext2 bootlog_persist() appends only the delta; if it has to fall
        // back to fat_write_file() it rewrites the WHOLE buffer, and the
        // difference between those two is exactly the #373 mechanism. Charging
        // the delta here and the full length on the rewrite path would need
        // bootlog_persist to report which it did; charging the delta is the
        // honest floor, and a full-rewrite storm still shows up in
        // g_blk_write_sectors on the same heartbeat line.
        {
            uint32_t _d = (g_bootlog_persisted <= g_bootlog_len)
                        ? (g_bootlog_len - g_bootlog_persisted) : g_bootlog_len;
            extern uint64_t g_bootlog_bytes_written;
            g_bootlog_bytes_written += _d;
        }
        g_bl_in_persist++;
        int _rc = bootlog_persist(BOOTLOG_PATH, g_bootlog_buf, g_bootlog_len,
                                  &g_bootlog_persisted,
                                  g_bootlog_full_warned || g_bootlog_compacted);
        g_bl_in_persist--;
        if (_rc != 0) { bootlog_flush_failed(BOOTLOG_PATH, _rc); return _rc; }
    }
    // Not armed yet is NOT a failure: the line is in the RAM buffer and
    // bootlog_arm() flushes the whole buffer the moment a filesystem exists.
    return 0;
}

// #373 real-HW freeze diagnostic: constant-cost heartbeat writer. See the header
// comment in bootlog.h for why the 1 Hz bootlog_write() full-buffer rewrite is
// believed to have starved/wedged the iMac. This writes ONLY a small bounded
// ring (the most recent beats, capped at HB_RING_CAP) to a SEPARATE fixed file,
// so each write is constant and tiny no matter how long the machine runs.
#define HB_PATH      "/HEARTBEAT.TXT"
// #745 (task #62): 4 KB held about 30 beats of the OLD, shorter record. The
// enriched record is roughly twice as long, and the fault being chased is a
// ~60 s ramp that must be visible IN FULL alongside the beats before it. 16 KB
// holds ~50 enriched beats = ~100 s at the 2 s cadence. The write stays
// CONSTANT-cost (that is what #373 required); it is simply a bigger constant,
// and it is paid only on the rare flush schedule below, not per beat.
#define HB_RING_CAP  (16 * 1024)
static char     g_hb_ring[HB_RING_CAP];
static uint32_t g_hb_len = 0;

// #748: the ring is bounded, which made every write CONSTANT. It did not make
// the writes RARE, and rare is what a flash stick needs: a constant 4 KB
// rewritten every 2 s forever is 43-57 KB/s of device traffic at complete idle
// (see the file header). The ring now lives in RAM and reaches the medium on
// three occasions, and each one is a deliberate answer to "what question is
// this file being asked?":
//
//   1. ON A SCHEDULE (HB_FLUSH_MS, 30 min). The user asked for exactly this:
//      "we could either do a periodic write to usb (every 30 minutes or so)
//      for debugging, or just leave it on ramdisk only". This is the floor of
//      "how recently was this machine alive", and 30 min of granularity is a
//      fair price for ~1800x fewer write events.
//   2. ON THE ANOMALY IT EXISTS TO CATCH. #373 was a machine being starved to
//      death: the beats stretched 4s -> 27s and THEN stopped. A schedule can
//      miss that entirely; an anomaly trigger cannot. A beat that arrives late
//      by HB_ANOMALY_GAP_MS persists the ring immediately, so the widening
//      gaps are on disk BEFORE the wedge, which is the whole point. Rate
//      limited by HB_ANOMALY_MIN_MS so a permanently slow machine cannot turn
//      the diagnostic back into the storm.
//   3. ON PANIC, from panic_log_write(), after the panic record itself is
//      safely down.
//
// Deliberately NOT flushed on every beat during boot either: a boot-time wedge
// leaves /BOOTLOG.TXT (per-line durable, see above) and /PANIC.TXT, and case 2
// covers the starvation signature on any timeline.
#define HB_FLUSH_MS        (30u * 60u * 1000u)
#define HB_ANOMALY_GAP_MS  6000u     // nominal beat is 2000ms
// #745 (task #62): 60 s was too coarse for the reported fault. The machine
// becomes unusable inside a minute, so a single anomaly flush followed by a
// 60 s lockout can leave the ENTIRE degradation window in RAM and lose it to
// the power switch. 15 s admits at most 4 flushes a minute, and only while the
// machine is ALREADY pathologically slow - which is exactly when the evidence
// is worth more than the write. A healthy machine never trips it at all.
#define HB_ANOMALY_MIN_MS  15000u    // at most four anomaly flushes per minute
static uint64_t g_hb_last_flush_ms = 0;
static uint64_t g_hb_last_beat_ms  = 0;
static uint64_t g_hb_last_anom_ms  = 0;
static int      g_hb_dirty         = 0;   // ring has beats not yet on the medium
static uint64_t g_hb_flushes       = 0;
static uint64_t g_hb_beats         = 0;

// Persist the ring NOW. Whole-file replace, never an append: the ring drops
// whole lines off its FRONT, so its bytes shift and an append would duplicate.
// Returns 0 if there was nothing to do or the write succeeded.
// #745 (task #62): how many bytes has the LOGGING SUBSYSTEM ITSELF pushed at
// the medium this boot? #373 was the logger starving the machine it was
// logging, so a diagnostic that cannot exonerate itself is not a diagnostic.
uint64_t g_bootlog_bytes_written = 0;

int bootlog_heartbeat_flush(void) {
    if (!g_hb_dirty || !g_bootlog_armed || !g_bootlog_fs) return 0;
    // #745 (task #62): same guard as bootlog_write(). The anomaly flush fires
    // exactly when the machine is already struggling, which is exactly when
    // re-entering the storage stack is least affordable.
    if (g_bl_defer || g_bl_in_persist) { g_bootlog_flushes_deferred++; return 0; }
    g_bootlog_bytes_written += g_hb_len;
    g_bl_in_persist++;
    int rc = bootlog_persist(HB_PATH, g_hb_ring, g_hb_len, 0, 0);
    g_bl_in_persist--;
    if (rc != 0) { bootlog_flush_failed(HB_PATH, rc); return rc; }
    g_hb_dirty = 0;
    g_hb_flushes++;
    if (mono_ready()) g_hb_last_flush_ms = mono_ms();
    return 0;
}

void bootlog_heartbeat_stats(uint64_t *beats, uint64_t *flushes) {
    if (beats)   *beats   = g_hb_beats;
    if (flushes) *flushes = g_hb_flushes;
}

uint32_t bootlog_heartbeat_ring(const char **out) {
    if (out) *out = g_hb_ring;
    return g_hb_len;
}

void bootlog_heartbeat(const char *line) {
    if (!line) return;
    // #134: the heartbeat is an ordinary kernel thread with interrupts on, and
    // it runs every 2 s whatever else the machine is doing. That makes it the
    // right safe context to get a fault record onto the medium promptly when
    // nothing else happens to log.
    if (g_fault_len) (void)bootlog_fault_flush();
    uint32_t n = (uint32_t)strlen(line);
    if (n > HB_RING_CAP - 2) n = HB_RING_CAP - 2;

    // If appending would overflow the fixed ring, drop WHOLE oldest lines from
    // the front until it fits. The ring therefore stays a small constant size
    // (the last ~30 beats), keeping the on-disk write constant-cost forever.
    if (g_hb_len + n + 1 >= HB_RING_CAP) {
        uint32_t drop = 0;
        while (drop < g_hb_len && (g_hb_len - drop) + n + 1 >= HB_RING_CAP) {
            while (drop < g_hb_len && g_hb_ring[drop] != '\n') drop++;
            if (drop < g_hb_len) drop++;   // consume the newline too
        }
        if (drop >= g_hb_len) {
            g_hb_len = 0;
        } else {
            memmove(g_hb_ring, g_hb_ring + drop, g_hb_len - drop);
            g_hb_len -= drop;
        }
    }

    memcpy(g_hb_ring + g_hb_len, line, n);
    g_hb_len += n;
    g_hb_ring[g_hb_len++] = '\n';

    // Always mirror to serial (proves liveness even if the on-disk write wedges).
    kprintf("[HBLOG] %s\n", line);
    g_hb_dirty = 1;
    g_hb_beats++;

    // #748: decide whether this beat is worth a device write. See the block
    // comment above HB_FLUSH_MS for why these three cases and no others.
    if (!g_bootlog_armed || !g_bootlog_fs) return;
    if (!mono_ready()) {
        // No real clock yet, so no schedule and no gap measurement is possible.
        // Persist once, so the file exists and says the machine got this far,
        // and then stay quiet until the clock is up.
        if (g_hb_last_flush_ms == 0 && g_hb_flushes == 0) bootlog_heartbeat_flush();
        return;
    }
    uint64_t now = mono_ms();
    int due = 0;
    if (g_hb_flushes == 0) {
        due = 1;                                     // first persisted beat
    } else if (now - g_hb_last_flush_ms >= HB_FLUSH_MS) {
        due = 1;                                     // the 30-minute schedule
    } else if (g_hb_last_beat_ms &&
               now - g_hb_last_beat_ms >= HB_ANOMALY_GAP_MS &&
               (g_hb_last_anom_ms == 0 || now - g_hb_last_anom_ms >= HB_ANOMALY_MIN_MS)) {
        // The #373 starvation signature. Say so IN the artifact: a reader who
        // finds this file after a wedge needs to know the gap was measured, not
        // inferred from a missing line.
        char note[96];
        int nn = snprintf(note, sizeof(note),
                          "[HBLOG] LATE BEAT: %llums gap (nominal 2000ms) - persisting ring now",
                          (unsigned long long)(now - g_hb_last_beat_ms));
        if (nn > 0) {
            kprintf("%s\n", note);
            uint32_t nl = (uint32_t)nn;
            if (nl < HB_RING_CAP - g_hb_len - 1) {
                memcpy(g_hb_ring + g_hb_len, note, nl);
                g_hb_len += nl;
                g_hb_ring[g_hb_len++] = '\n';
            }
        }
        g_hb_last_anom_ms = now;
        due = 1;
    }
    g_hb_last_beat_ms = now;
    if (due) bootlog_heartbeat_flush();
}

// =============================================================================
// #433 (re-scoped) USB descriptor / HID-enumeration log -> /USBLOG.TXT
// =============================================================================
// The keyboard-specific iMac failure ("a USB mouse works in any port, the USB
// keyboard works in no port") can only be diagnosed by seeing the KEYBOARD's
// real descriptors and the runtime enumeration decisions the kernel made about
// it. /DEVLOG.TXT already dumps the static descriptor tree; this file adds the
// DYNAMIC picture the class driver alone can produce: for every enumerated USB
// device its VID:PID / class / bNumConfigurations, every interface + endpoint of
// the chosen config, and - crucially - whether the boot-keyboard/mouse interface
// was found, whether SET_PROTOCOL(boot)/SET_IDLE were sent and their results,
// the Configure-Endpoint result per interrupt-IN endpoint, and whether polling
// started. Same durability design as /BOOTLOG.TXT: buffer in RAM from the first
// call, and once bootlog_arm() has mounted the FAT root, re-write the whole
// buffer to /USBLOG.TXT on every call so `ssh <imac> "cat /USBLOG.TXT"` returns
// the real keyboard descriptors even if a later step hangs.
#define USBLOG_BUF_CAP  (64 * 1024)
#define USBLOG_PATH     "/USBLOG.TXT"
#define USBLOG_LINE_MAX 256

static char     g_usblog_buf[USBLOG_BUF_CAP];
static uint32_t g_usblog_len = 0;
static int      g_usblog_full_warned = 0;
static uint32_t g_usblog_persisted = 0;   // #748: bytes already on the medium

static void usblog_append_raw(const char *s, uint32_t len) {
    bl_append(g_usblog_buf, USBLOG_BUF_CAP, &g_usblog_len, &g_usblog_persisted,
              &g_usblog_full_warned, &g_usblog_compacted, "USBLOG", USBLOG_PATH, s, len);
}

void usblog_write(const char *fmt, ...) {
    char line[USBLOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    size_t bl_dropped = 0;
    int n = vsnprintf_dropped(line, sizeof(line), fmt, args, &bl_dropped);
    va_end(args);
    if (n < 0) return;
    n = bl_clamp(line, (uint32_t)sizeof(line), n, bl_dropped);

    // Mirror to serial (additive), same as bootlog_write().
    kprintf("[USBLOG] %s\n", line);

    line[n] = '\n';                       // #748: one contiguous append, see bootlog_write()
    usblog_append_raw(line, (uint32_t)n + 1);

    if (g_bootlog_armed && g_bootlog_fs) {
        { int _rc = bootlog_persist(USBLOG_PATH, g_usblog_buf, g_usblog_len,
                                    &g_usblog_persisted,
                                    g_usblog_full_warned || g_usblog_compacted);
          if (_rc != 0) bootlog_flush_failed(USBLOG_PATH, _rc); }
    }
}

// =============================================================================
// #71 / Cirrus CS4208 HD Audio diagnostic -> /AUDIOLOG.TXT
// =============================================================================
// The iMac14,4's internal speakers are silent for two reasons (see hda.c): the
// HDA output-stream DMA must actually run, and the Apple Cirrus CS4208's
// internal-speaker amplifier must be powered via EAPD + a codec GPIO. The user
// has no serial on the physical machine, so this file mirrors the HD Audio
// codec dump + output-path state to /AUDIOLOG.TXT for retrieval over SSH. Same
// durability + RAM-buffer design as /USBLOG.TXT above.
#define AUDIOLOG_BUF_CAP  (64 * 1024)
#define AUDIOLOG_PATH     "/AUDIOLOG.TXT"
#define AUDIOLOG_LINE_MAX 256

static char     g_audiolog_buf[AUDIOLOG_BUF_CAP];
static uint32_t g_audiolog_len = 0;
static int      g_audiolog_full_warned = 0;
static uint32_t g_audiolog_persisted = 0;   // #748: bytes already on the medium
// #71: when set, audiolog_write() accumulates in RAM only and skips the
// per-call full-file flush; audiolog_end_batch() does one flush at the end.
// audiolog_write() rewrites the whole growing file on every call, so per-line
// flushing a ~40-120 line report is O(n^2) full-file rewrites - many seconds of
// disk thrash over the slow USB-MSC stack. Batching makes the whole dump ONE
// write. Each line still fits the AUDIOLOG_LINE_MAX vsnprintf buffer.
static int      g_audiolog_defer = 0;

static void audiolog_append_raw(const char *s, uint32_t len) {
    bl_append(g_audiolog_buf, AUDIOLOG_BUF_CAP, &g_audiolog_len, &g_audiolog_persisted,
              &g_audiolog_full_warned, &g_audiolog_compacted, "AUDIOLOG", AUDIOLOG_PATH, s, len);
}

void audiolog_write(const char *fmt, ...) {
    char line[AUDIOLOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    size_t bl_dropped = 0;
    int n = vsnprintf_dropped(line, sizeof(line), fmt, args, &bl_dropped);
    va_end(args);
    if (n < 0) return;
    n = bl_clamp(line, (uint32_t)sizeof(line), n, bl_dropped);

    // Mirror to serial (additive), same as usblog_write().
    kprintf("[AUDIOLOG] %s\n", line);

    line[n] = '\n';                       // #748: one contiguous append, see bootlog_write()
    audiolog_append_raw(line, (uint32_t)n + 1);

    if (!g_audiolog_defer && g_bootlog_armed && g_bootlog_fs) {
        { int _rc = bootlog_persist(AUDIOLOG_PATH, g_audiolog_buf, g_audiolog_len,
                                    &g_audiolog_persisted,
                                    g_audiolog_full_warned || g_audiolog_compacted);
          if (_rc != 0) bootlog_flush_failed(AUDIOLOG_PATH, _rc); }
    }
}

// #71: bracket a multi-line audiolog dump so it flushes to disk exactly once.
// Safe to call unarmed (the end flush is a no-op until bootlog_arm()).
void audiolog_begin_batch(void) { g_audiolog_defer = 1; }
void audiolog_end_batch(void) {
    g_audiolog_defer = 0;
    if (g_bootlog_armed && g_bootlog_fs) {
        // #748: the whole deferred batch is exactly the un-persisted tail, so
        // the batch still costs ONE write and it is now an append rather than a
        // rewrite of the whole report.
        { int _rc = bootlog_persist(AUDIOLOG_PATH, g_audiolog_buf, g_audiolog_len,
                                    &g_audiolog_persisted,
                                    g_audiolog_full_warned || g_audiolog_compacted);
          if (_rc != 0) bootlog_flush_failed(AUDIOLOG_PATH, _rc); }
    }
}

void bootlog_arm(fat_fs_t *fs) {
    if (g_bootlog_armed || !fs) return;
    g_bootlog_fs = fs;
    g_bootlog_armed = 1;
    // Flush everything logged since early boot (xHCI/USB enumeration, MSC
    // root-mount probing, etc.) in one shot; bootlog_write() stays live from
    // here on.
    { int _rc = bootlog_persist(BOOTLOG_PATH, g_bootlog_buf, g_bootlog_len,
                                &g_bootlog_persisted,
                                g_bootlog_full_warned || g_bootlog_compacted);
      if (_rc != 0) bootlog_flush_failed(BOOTLOG_PATH, _rc); }
    // Same one-shot flush for the USB descriptor log accumulated so far (all of
    // xHCI/HID enumeration happens before any filesystem exists).
    { int _rc = bootlog_persist(USBLOG_PATH, g_usblog_buf, g_usblog_len,
                                &g_usblog_persisted,
                                g_usblog_full_warned || g_usblog_compacted);
      if (_rc != 0) bootlog_flush_failed(USBLOG_PATH, _rc); }
    // #71 / Cirrus: flush the HD Audio diagnostic accumulated during audio_init
    // (codec probe happens well before the FAT root is writable).
    { int _rc = bootlog_persist(AUDIOLOG_PATH, g_audiolog_buf, g_audiolog_len,
                                &g_audiolog_persisted,
                                g_audiolog_full_warned || g_audiolog_compacted);
      if (_rc != 0) bootlog_flush_failed(AUDIOLOG_PATH, _rc); }
    kprintf("[BOOTLOG] armed: /BOOTLOG.TXT (%u bytes) + /USBLOG.TXT (%u bytes) + /AUDIOLOG.TXT (%u bytes) now live\n",
            (unsigned)g_bootlog_len, (unsigned)g_usblog_len, (unsigned)g_audiolog_len);
    bootlog_write_esp_map();
}

// ===========================================================================
// #307: THE BREADCRUMB THAT SAYS WHICH PARTITION THE EVIDENCE IS ON.
// ===========================================================================
// On a two-partition golden the FAT ESP is NOT the root the kernel reads and
// writes. fat_path_on_ext2() (fs/fat.c) routes every "/" path to the ext2 root
// EXCEPT "/boot/..." and "/EFI/...", so /BOOTLOG.TXT, /USBLOG.TXT,
// /AUDIOLOG.TXT, /NETCFG.TXT and /CONFIG/... all live on PARTITION 2, and a
// file dropped at the root of the FAT ESP is never read by anything.
//
// THIS COST A WHOLE MISDIAGNOSIS (2026-08-25). An investigation into the
// owner's iMac14,4 inspected partition 1 of his boot stick, found no
// /BOOTLOG.TXT, and concluded from that absence that the kernel had never
// mounted the filesystem, which in turn "explained" the wrong IP and the
// unresponsive desktop. Every step of that was wrong: the kernel had mounted
// fine and had written 268 KB of /BOOTLOG.TXT to partition 2, and the wrong IP
// was a /NETCFG.TXT that had been placed on partition 1 where nothing reads it.
// A VM boot of the same golden reproduces the "missing" file exactly, so the
// absence was never evidence of anything.
//
// The durable fix is not a longer checklist, it is a note in the place the
// person is already looking. /boot/ is one of the two ESP-exempt prefixes, so
// this file lands on the FAT partition, next to kernel.elf, where anyone who
// mounts the stick on another machine will see it. It is written once per boot,
// costs one small FAT write, and reads nothing back.
//
// Kept in C: this is three statements inside an existing C function, using this
// file's own static state (g_bootlog_fs) and the C FAT API. A Rust boundary
// here would be a larger FFI surface than the change itself.
static void bootlog_write_esp_map(void) {
    if (!g_bootlog_armed || !g_bootlog_fs) return;
    static const char map[] =
        "MayteraOS: WHERE THE LOGS AND CONFIG FILES ACTUALLY ARE\n"
        "\n"
        "This is PARTITION 1, the FAT ESP. It holds boot assets ONLY:\n"
        "  /boot/kernel.elf  /EFI/BOOT/BOOTX64.EFI  /KERNEL.ELF  /BOOT.BMP\n"
        "  /FONT.TTF  /FONTS/  /BUILDINFO.TXT  /ROOTEXT2\n"
        "  /boot/STAGE.TXT and /boot/PANIC.TXT (crash breadcrumbs)\n"
        "  /boot/LOGS.TXT (this file)\n"
        "\n"
        "EVERY OTHER \"/\" PATH THE KERNEL READS OR WRITES IS ON PARTITION 2\n"
        "(the ext2 root). That includes:\n"
        "  /BOOTLOG.TXT   /USBLOG.TXT   /AUDIOLOG.TXT   /HEARTBEAT.TXT\n"
        "  /NETCFG.TXT    /CONFIG/...   /APPS/...       /HOME/...\n"
        "\n"
        "A FILE DROPPED AT THE ROOT OF THIS FAT PARTITION IS NEVER READ BY THE\n"
        "KERNEL. To supply a static IP, or to read the boot log, mount\n"
        "PARTITION 2, not this one.\n"
        "\n"
        "Absence of /BOOTLOG.TXT here is NORMAL and proves nothing.\n";
    int rc = fat_write_file(g_bootlog_fs, "/boot/LOGS.TXT", map, sizeof(map) - 1);
    if (rc != 0)
        kprintf("[BOOTLOG] /boot/LOGS.TXT partition-map note not written (rc=%d)\n", rc);
}
