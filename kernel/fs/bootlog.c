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
//   - After arming, EVERY bootlog_write() call re-writes the WHOLE
//     accumulated buffer to /BOOTLOG.TXT via fat_write_file(). This is a full
//     rewrite rather than a true incremental disk-append: the FAT driver's
//     writes are already synchronous with no write-back cache (see
//     fat_write_file/blk_write), so a full rewrite after each line is exactly
//     as durable as a true append would be, without adding new incremental-
//     append plumbing to the FAT driver while chasing a real-hardware bug.
//     A boot logs on the order of ~100-300 short lines, so total write volume
//     for one boot is a few hundred KB at most - negligible next to a single
//     USB-MSC bulk file transfer, and it does not meaningfully slow boot.
#include "bootlog.h"
#include "../serial.h"
#include "../string.h"
#include <stdarg.h>

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

static char     g_bootlog_buf[BOOTLOG_BUF_CAP];
static uint32_t g_bootlog_len = 0;
static int      g_bootlog_armed = 0;
static int      g_bootlog_full_warned = 0;
static fat_fs_t *g_bootlog_fs = 0;

static void bootlog_append_raw(const char *s, uint32_t len) {
    if (len == 0) return;
    if (g_bootlog_len + len >= BOOTLOG_BUF_CAP) {
        if (!g_bootlog_full_warned) {
            g_bootlog_full_warned = 1;
            kprintf("[BOOTLOG] in-RAM buffer full (%u bytes); further lines are serial-only\n",
                    (unsigned)BOOTLOG_BUF_CAP);
        }
        return;
    }
    memcpy(g_bootlog_buf + g_bootlog_len, s, len);
    g_bootlog_len += len;
}

int bootlog_is_armed(void) {
    return g_bootlog_armed;
}

void bootlog_write(const char *fmt, ...) {
    char line[BOOTLOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((uint32_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

    // Mirror to the existing serial log (additive, never replaces it).
    kprintf("[BOOTLOG] %s\n", line);

    // Always buffer in RAM, armed or not - see file header.
    bootlog_append_raw(line, (uint32_t)n);
    bootlog_append_raw("\n", 1);

    if (g_bootlog_armed && g_bootlog_fs) {
        fat_write_file(g_bootlog_fs, BOOTLOG_PATH, g_bootlog_buf, g_bootlog_len);
    }
}

// #373 real-HW freeze diagnostic: constant-cost heartbeat writer. See the header
// comment in bootlog.h for why the 1 Hz bootlog_write() full-buffer rewrite is
// believed to have starved/wedged the iMac. This writes ONLY a small bounded
// ring (the most recent beats, capped at HB_RING_CAP) to a SEPARATE fixed file,
// so each write is constant and tiny no matter how long the machine runs.
#define HB_PATH      "/HEARTBEAT.TXT"
#define HB_RING_CAP  (4 * 1024)
static char     g_hb_ring[HB_RING_CAP];
static uint32_t g_hb_len = 0;

void bootlog_heartbeat(const char *line) {
    if (!line) return;
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

    // Constant-size write to the SEPARATE heartbeat file; never touches the big
    // /BOOTLOG.TXT buffer, so this can never become the growing O(n^2) write.
    if (g_bootlog_armed && g_bootlog_fs) {
        fat_write_file(g_bootlog_fs, HB_PATH, g_hb_ring, g_hb_len);
    }
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

static void usblog_append_raw(const char *s, uint32_t len) {
    if (len == 0) return;
    if (g_usblog_len + len >= USBLOG_BUF_CAP) {
        if (!g_usblog_full_warned) {
            g_usblog_full_warned = 1;
            kprintf("[USBLOG] in-RAM buffer full (%u bytes); further lines are serial-only\n",
                    (unsigned)USBLOG_BUF_CAP);
        }
        return;
    }
    memcpy(g_usblog_buf + g_usblog_len, s, len);
    g_usblog_len += len;
}

void usblog_write(const char *fmt, ...) {
    char line[USBLOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((uint32_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

    // Mirror to serial (additive), same as bootlog_write().
    kprintf("[USBLOG] %s\n", line);

    usblog_append_raw(line, (uint32_t)n);
    usblog_append_raw("\n", 1);

    if (g_bootlog_armed && g_bootlog_fs) {
        fat_write_file(g_bootlog_fs, USBLOG_PATH, g_usblog_buf, g_usblog_len);
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
// #71: when set, audiolog_write() accumulates in RAM only and skips the
// per-call full-file flush; audiolog_end_batch() does one flush at the end.
// audiolog_write() rewrites the whole growing file on every call, so per-line
// flushing a ~40-120 line report is O(n^2) full-file rewrites - many seconds of
// disk thrash over the slow USB-MSC stack. Batching makes the whole dump ONE
// write. Each line still fits the AUDIOLOG_LINE_MAX vsnprintf buffer.
static int      g_audiolog_defer = 0;

static void audiolog_append_raw(const char *s, uint32_t len) {
    if (len == 0) return;
    if (g_audiolog_len + len >= AUDIOLOG_BUF_CAP) {
        if (!g_audiolog_full_warned) {
            g_audiolog_full_warned = 1;
            kprintf("[AUDIOLOG] in-RAM buffer full (%u bytes); further lines are serial-only\n",
                    (unsigned)AUDIOLOG_BUF_CAP);
        }
        return;
    }
    memcpy(g_audiolog_buf + g_audiolog_len, s, len);
    g_audiolog_len += len;
}

void audiolog_write(const char *fmt, ...) {
    char line[AUDIOLOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((uint32_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

    // Mirror to serial (additive), same as usblog_write().
    kprintf("[AUDIOLOG] %s\n", line);

    audiolog_append_raw(line, (uint32_t)n);
    audiolog_append_raw("\n", 1);

    if (!g_audiolog_defer && g_bootlog_armed && g_bootlog_fs) {
        fat_write_file(g_bootlog_fs, AUDIOLOG_PATH, g_audiolog_buf, g_audiolog_len);
    }
}

// #71: bracket a multi-line audiolog dump so it flushes to disk exactly once.
// Safe to call unarmed (the end flush is a no-op until bootlog_arm()).
void audiolog_begin_batch(void) { g_audiolog_defer = 1; }
void audiolog_end_batch(void) {
    g_audiolog_defer = 0;
    if (g_bootlog_armed && g_bootlog_fs) {
        fat_write_file(g_bootlog_fs, AUDIOLOG_PATH, g_audiolog_buf, g_audiolog_len);
    }
}

void bootlog_arm(fat_fs_t *fs) {
    if (g_bootlog_armed || !fs) return;
    g_bootlog_fs = fs;
    g_bootlog_armed = 1;
    // Flush everything logged since early boot (xHCI/USB enumeration, MSC
    // root-mount probing, etc.) in one shot; bootlog_write() stays live from
    // here on.
    fat_write_file(fs, BOOTLOG_PATH, g_bootlog_buf, g_bootlog_len);
    // Same one-shot flush for the USB descriptor log accumulated so far (all of
    // xHCI/HID enumeration happens before any filesystem exists).
    fat_write_file(fs, USBLOG_PATH, g_usblog_buf, g_usblog_len);
    // #71 / Cirrus: flush the HD Audio diagnostic accumulated during audio_init
    // (codec probe happens well before the FAT root is writable).
    fat_write_file(fs, AUDIOLOG_PATH, g_audiolog_buf, g_audiolog_len);
    kprintf("[BOOTLOG] armed: /BOOTLOG.TXT (%u bytes) + /USBLOG.TXT (%u bytes) + /AUDIOLOG.TXT (%u bytes) now live\n",
            (unsigned)g_bootlog_len, (unsigned)g_usblog_len, (unsigned)g_audiolog_len);
}
