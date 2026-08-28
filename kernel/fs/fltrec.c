// fltrec.c - RAW-BLOCK BOOT FLIGHT RECORDER: the thin C glue to the block
// layer. Design rationale, the on-disk layout and the coverage claim are in
// fltrec.h; the record encoding, the CRC32 and the append/dirty-sector
// arithmetic are in ../rustkern/fltrec.rs.
//
// EVERYTHING HERE RUNS WITH INTERRUPTS OFF, BEFORE THE SCHEDULER EXISTS.
// So: no sleeping, no wait queues, no heap allocation, no unbounded loop. The
// text buffer is static .bss (entry.asm zeroes .bss) and every loop in this
// file has a compile-time bound.

#include "fltrec.h"
#include "blockdev.h"
#include "bootlog.h"
#include "../string.h"
#include "../serial.h"
#include "../version.h"
#include "../cpu/mono.h"
#include "../drivers/usb_msc.h"

// ---------------------------------------------------------------------------
// The Rust half (rustkern/fltrec.rs). Declared here rather than in fltrec.h so
// the FFI surface has exactly one C-side declaration site.
// ---------------------------------------------------------------------------
extern uint32_t fltrec_crc32_rs(const uint8_t *data, uint32_t len);
extern int      fltrec_sb_encode_rs(uint8_t *buf, uint32_t buf_len, uint32_t head_slot, uint64_t boot_seq);
extern int      fltrec_sb_decode_rs(const uint8_t *buf, uint32_t buf_len, fltrec_sb_t *out);
extern int      fltrec_hdr_encode_rs(uint8_t *buf, uint32_t buf_len, const fltrec_hdr_t *h);
extern int      fltrec_hdr_decode_rs(const uint8_t *buf, uint32_t buf_len, fltrec_hdr_t *out);
extern uint32_t fltrec_append_rs(uint8_t *buf, uint32_t cap, uint32_t len,
                                 const uint8_t *src, uint32_t src_len, int add_nl);
extern int      fltrec_dirty_rs(uint32_t flushed, uint32_t len, uint32_t *first, uint32_t *last);
extern int      fltrec_parr_scan_rs(const uint8_t *sec, uint32_t sec_len, uint32_t esz,
                                    uint32_t per_sec, uint32_t remaining,
                                    uint64_t lo, uint64_t hi, uint32_t *out_consumed);
extern int      fltrec_gpt_geom_rs(const uint8_t *sec, uint32_t len, uint64_t *out_ent_lba,
                                   uint32_t *out_num, uint32_t *out_esz, uint32_t *out_per_sec);
extern int      fltrec_selftest_rs(uint32_t *out_checks);

// ---------------------------------------------------------------------------
// GEOMETRY LOCKS. The Rust module carries its own copies of these constants and
// fltrec_selftest_rs() asserts the same relationships at boot. Both halves have
// to be wrong in the same way for a bad edit to survive, and the consequence of
// getting the last one wrong is writing over the first partition.
// ---------------------------------------------------------------------------
_Static_assert(FLTREC_SLOT_BASE_LBA == FLTREC_SB_LBA + 1,
               "fltrec: slot 0 must start immediately after the superblock");
_Static_assert(FLTREC_SLOT_BASE_LBA + (uint64_t)FLTREC_SLOTS * FLTREC_SLOT_SECTORS
                   == FLTREC_REGION_HI,
               "fltrec: the four slots must end exactly at LBA 2046, one short of the "
               "2048-aligned first partition");
_Static_assert(FLTREC_TEXT_SECTORS == FLTREC_SLOT_SECTORS - 1u,
               "fltrec: one sector of each slot is its header");
_Static_assert(FLTREC_TEXT_CAP == 257024u, "fltrec: text capacity drifted");
_Static_assert(FLTREC_REGION_LO == FLTREC_SB_LBA, "fltrec: region starts at the superblock");

// FFI sizeof locks: the house pattern. A silently diverging layout would make
// the C read a different field than the Rust wrote.
_Static_assert(sizeof(fltrec_sb_t) == 40, "fltrec: FltSb layout drifted from Rust");
_Static_assert(sizeof(fltrec_hdr_t) == 80, "fltrec: FltHdr layout drifted from Rust");
_Static_assert(sizeof(uint64_t) == 8 && sizeof(uint32_t) == 4, "fltrec: FFI widths");

// ---------------------------------------------------------------------------
// State. All .bss, all zero at entry.
// ---------------------------------------------------------------------------

// The RAM mirror of this boot's slot text. Sized to the FULL slot capacity on
// purpose: making the buffer and the slot the same size means there is exactly
// ONE limit in the system, so "it fit in RAM but not on disk" cannot happen.
// Page-aligned because blk_write's no-bounce fallback path hands the caller's
// pointer straight to the transport; .bss is identity-mapped and physically
// contiguous, and the alignment keeps any single sector inside one page.
static char     g_text[FLTREC_TEXT_CAP] __attribute__((aligned(4096)));

// The last FLT_RESERVE bytes are held back from normal breadcrumbs so that the
// "slot full" marker is GUARANTEED to fit. Without the reserve the truncation
// notice is the one line that can never be written, because it is longer than
// whatever line just failed to fit.
#define FLT_RESERVE   64u
#define FLT_USER_CAP  (FLTREC_TEXT_CAP - FLT_RESERVE)

static uint8_t  g_sec[FLTREC_SECTOR]  __attribute__((aligned(8)));  // encode / read scratch
static uint8_t  g_sec2[FLTREC_SECTOR] __attribute__((aligned(8)));  // readback compare

static uint32_t g_len       = 0;   // bytes of text in RAM
static uint32_t g_flushed   = 0;   // bytes of text known to be on the medium
static uint32_t g_slot      = 0;   // this boot's slot index
static uint64_t g_seq       = 0;   // this boot's sequence number
static uint32_t g_verdict   = FLTREC_VERDICT_OPEN;
static uint64_t g_seal_ms   = 0;
static int      g_armed     = 0;
static int      g_dead      = 0;   // medium refused: never touch it again
static int      g_full      = 0;   // the "slot full" marker has been written
static char     g_ident[32];

static uint64_t g_sectors   = 0;
static uint32_t g_failures  = 0;
static uint64_t g_dropped   = 0;
static uint64_t g_deferred  = 0;

// Re-entrancy guards. See the fltrec_defer_begin() comment in fltrec.h: our own
// flush path runs through usb_msc_transport()'s NON-RECURSIVE command lock, so
// a write issued from inside a SCSI command would wait forever on a lock the
// same thread already holds. g_in_io closes that structurally against ourselves;
// g_defer is the window a caller opens around code our flush runs through.
static volatile int g_in_io = 0;
static volatile int g_defer = 0;

void fltrec_defer_begin(void) { g_defer++; }
void fltrec_defer_end(void)   { if (g_defer > 0) g_defer--; }

int fltrec_armed(void) { return g_armed; }

// ---------------------------------------------------------------------------
// LBA arithmetic. Both are pure functions of the compile-time geometry above.
// ---------------------------------------------------------------------------
static uint64_t flt_hdr_lba(uint32_t slot) {
    return FLTREC_SLOT_BASE_LBA + (uint64_t)slot * FLTREC_SLOT_SECTORS;
}
static uint64_t flt_txt_lba(uint32_t slot) {
    return flt_hdr_lba(slot) + 1u;
}

// ---------------------------------------------------------------------------
// Device I/O. Every device touch in this file goes through these two, which is
// what makes the re-entrancy guard and the accounting complete rather than
// nearly complete.
//
// bootlog_defer_begin/end brackets the write for a reason worth stating: our
// flush is the code that runs when something has already gone wrong, and a
// /BOOTLOG.TXT flush triggered from inside it would issue MORE SCSI commands on
// the same device at exactly that moment. Buffered bootlog lines are not lost;
// they reach the disk on the next bootlog_write() from a safe context.
// ---------------------------------------------------------------------------
static int flt_wr(uint64_t lba, uint32_t count, const void *buf) {
    int rc;
    g_in_io++;
    bootlog_defer_begin();
    rc = blk_write(0, 0, lba, count, buf);
    bootlog_defer_end();
    g_in_io--;
    if (rc != (int)count) { g_failures++; return -1; }
    g_sectors += count;
    return 0;
}

static int flt_rd(uint64_t lba, uint32_t count, void *buf) {
    int rc;
    g_in_io++;
    bootlog_defer_begin();
    rc = blk_read(0, 0, lba, count, buf);
    bootlog_defer_end();
    g_in_io--;
    if (rc != (int)count) { g_failures++; return -1; }
    return 0;
}

// ---------------------------------------------------------------------------
// GATE 1: is the root device addressed in 512-byte sectors?
//
// blk_read/blk_write are 512-byte units by definition of the block layer, and
// main.c already refuses a USB root whose logical block size is not 512. This
// re-checks it rather than inheriting the assumption, because the consequence
// of being wrong is writing 512-byte records at 4096-byte LBAs, which lands in
// the middle of the partition table.
// ---------------------------------------------------------------------------
static int flt_geometry_ok(void) {
    usb_msc_device_t *d;
    uint32_t bs;
    if (!blk_root_is_usb()) {
        // ATA path (the VM case). blk_write routes to ata_write_sectors_dma,
        // which is 512-byte sectors by construction.
        return 1;
    }
    d = usb_msc_get_device(blk_root_usb_index());
    if (!d || !d->ready) return 0;
    bs = d->block_size ? d->block_size : d->luns[0].block_size;
    return bs == FLTREC_SECTOR;
}

// ---------------------------------------------------------------------------
// GATE 2: does anything own LBA 34..2047 on THIS medium?
//
// We are about to write to the boot device, and the kernel selects a USB root
// before it has proved the root is ours. So arming is gated on a structural
// fact: a valid GPT header must be present AND every non-empty partition entry
// must start at or after LBA 2048.
//
// A disk with no valid GPT header is refused outright and that is deliberate:
// on an MBR disk the post-MBR gap is exactly where GRUB embeds core.img, and
// there is no cheap way to tell an empty gap from an embedded stage 1.5.
//
// Bounded by construction: at most FLT_PARR_MAX_SECS single-sector reads, and
// the walk refuses rather than continuing if a scan makes no progress.
// ---------------------------------------------------------------------------
#define FLT_PARR_MAX_SECS 64u

static int flt_region_safe(void) {
    uint64_t ent_lba = 0;
    uint32_t num = 0, esz = 0, per_sec = 0;
    uint32_t secs, remaining, i;

    if (flt_rd(1, 1, g_sec) != 0) {
        kprintf("[FLTREC] cannot read the GPT header at LBA 1; not arming\n");
        return 0;
    }
    if (fltrec_gpt_geom_rs(g_sec, FLTREC_SECTOR, &ent_lba, &num, &esz, &per_sec) != 0) {
        kprintf("[FLTREC] no valid GPT on the root device; not arming (an MBR disk's "
                "post-MBR gap can hold an embedded bootloader)\n");
        return 0;
    }
    // per_sec is >= 1 by the Rust accept guard, so this cannot divide by zero
    // and cannot wrap for num <= 256.
    secs = (num + per_sec - 1u) / per_sec;
    if (secs == 0 || secs > FLT_PARR_MAX_SECS) {
        kprintf("[FLTREC] GPT entry array spans %u sectors (max %u); not arming\n",
                secs, FLT_PARR_MAX_SECS);
        return 0;
    }
    // The entry array itself must end before our region begins. On every image
    // this kernel produces it is LBA 2..33.
    if (ent_lba + secs > FLTREC_REGION_LO) {
        kprintf("[FLTREC] GPT entry array at LBA %llu..%llu reaches our region; not arming\n",
                (unsigned long long)ent_lba, (unsigned long long)(ent_lba + secs - 1));
        return 0;
    }

    remaining = num;
    for (i = 0; i < secs && remaining > 0; i++) {
        uint32_t consumed = 0;
        int v;
        if (flt_rd(ent_lba + i, 1, g_sec) != 0) return 0;
        v = fltrec_parr_scan_rs(g_sec, FLTREC_SECTOR, esz, per_sec, remaining,
                                FLTREC_REGION_LO, FLTREC_REGION_HI, &consumed);
        if (v != 1) {
            kprintf("[FLTREC] a GPT partition overlaps LBA %llu..%llu on this medium; "
                    "NOT arming (this is not one of our images)\n",
                    (unsigned long long)FLTREC_REGION_LO, (unsigned long long)FLTREC_REGION_HI);
            return 0;
        }
        if (consumed == 0) return 0;   // no progress: refuse rather than spin
        remaining -= consumed;
    }
    if (remaining != 0) {
        kprintf("[FLTREC] only examined %u of %u GPT entries; not arming\n",
                num - remaining, num);
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Record writers
// ---------------------------------------------------------------------------

// The header's text_len is always g_flushed, NEVER g_len: a header on the
// medium must never claim more text than is actually there, or a reader would
// print whatever stale bytes follow.
static int flt_write_hdr(void) {
    fltrec_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.boot_seq   = g_seq;
    h.seal_ms    = g_seal_ms;
    h.version    = 1;
    h.slot_index = g_slot;
    h.build      = (uint32_t)MAYTERA_BUILD_NUMBER;
    h.verdict    = g_verdict;
    h.text_len   = g_flushed;
    h.text_cap   = FLTREC_TEXT_CAP;
    h.text_crc   = fltrec_crc32_rs((const uint8_t *)g_text, g_flushed);
    memcpy(h.ident, g_ident, sizeof(h.ident));
    if (fltrec_hdr_encode_rs(g_sec, FLTREC_SECTOR, &h) != 0) { g_failures++; return -1; }
    return flt_wr(flt_hdr_lba(g_slot), 1, g_sec);
}

// Write only the text sectors that changed since the last flush. Normally ONE.
static void flt_flush_text(void) {
    uint32_t first = 0, last = 0, n;
    int r = fltrec_dirty_rs(g_flushed, g_len, &first, &last);
    if (r < 0) { g_failures++; return; }   // impossible arguments: never an LBA
    if (r == 0) return;
    n = last - first + 1u;
    if (flt_wr(flt_txt_lba(g_slot) + first, n,
               g_text + (uint64_t)first * FLTREC_SECTOR) != 0) {
        // Do NOT advance g_flushed: the bytes are not on the medium, and the
        // next flush must try them again.
        return;
    }
    g_flushed = g_len;
}

// ---------------------------------------------------------------------------
// Identity string. THIS KERNEL HAS NO COMMIT MACRO: there is no
// MAYTERA_GIT_COMMIT or equivalent anywhere in kernel/, so the field the design
// calls "the commit string" carries the best identity actually available at
// compile time. fltrec_set_ident() lets a caller replace it if a real commit
// string is ever plumbed into the build.
// ---------------------------------------------------------------------------
void fltrec_set_ident(const char *s) {
    uint32_t i;
    memset(g_ident, 0, sizeof(g_ident));
    if (!s) return;
    for (i = 0; i + 1u < sizeof(g_ident) && s[i]; i++) g_ident[i] = s[i];
}

static void flt_default_ident(void) {
    if (g_ident[0]) return;
    snprintf(g_ident, sizeof(g_ident), "v%s b%u %s",
             MAYTERA_VERSION_STRING, (unsigned)MAYTERA_BUILD_NUMBER, MAYTERA_BUILD_DATE);
}

// ---------------------------------------------------------------------------
// THE API
// ---------------------------------------------------------------------------

int fltrec_arm(void) {
    fltrec_sb_t sb;
    uint32_t head = 0;
    uint64_t seq  = 1;
    char msg[96];

    if (g_armed) return 1;
    if (g_dead)  return 0;

    flt_default_ident();

    if (!flt_geometry_ok()) {
        kprintf("[FLTREC] root device is not 512-byte sectors; not arming\n");
        g_dead = 1;
        return 0;
    }
    if (!flt_region_safe()) {
        g_dead = 1;
        return 0;
    }

    // Read the existing superblock. A read failure, a foreign record, a corrupt
    // record and a virgin all-zero region are all the SAME case here: start a
    // fresh ring at slot 0, sequence 1. The decoder rejects rather than trusts,
    // which is the whole reason it is a decoder and not a cast.
    if (flt_rd(FLTREC_SB_LBA, 1, g_sec) == 0 &&
        fltrec_sb_decode_rs(g_sec, FLTREC_SECTOR, &sb) == 0) {
        head = (sb.head_slot + 1u) % FLTREC_SLOTS;
        seq  = sb.boot_seq + 1u;
    }

    if (fltrec_sb_encode_rs(g_sec, FLTREC_SECTOR, head, seq) != 0) {
        kprintf("[FLTREC] superblock encode refused; not arming\n");
        g_dead = 1;
        return 0;
    }
    if (flt_wr(FLTREC_SB_LBA, 1, g_sec) != 0) {
        kprintf("[FLTREC] superblock write failed; medium not writable, not arming\n");
        g_dead = 1;
        return 0;
    }

    // READ IT BACK. This is the ONLY thing that proves the medium really took
    // the write, and it is only a real proof BEFORE blk_root_to_ram(): once
    // TO-RAM or the demand cache is on, blk_write() installs the new bytes into
    // that RAM copy on success and the readback would agree with itself even if
    // the device silently dropped it. See the ordering note in fltrec.h.
    if (flt_rd(FLTREC_SB_LBA, 1, g_sec2) != 0 ||
        memcmp(g_sec, g_sec2, FLTREC_SECTOR) != 0) {
        kprintf("[FLTREC] superblock readback MISMATCH; the medium is not durably "
                "writable, not arming\n");
        g_dead = 1;
        return 0;
    }

    g_slot    = head;
    g_seq     = seq;
    g_verdict = FLTREC_VERDICT_OPEN;
    g_seal_ms = 0;

    // Lay down the WHOLE slot text area in one write. This does two jobs at
    // once: it carries down everything fltrec_write() accumulated before arming
    // (the pre-mount breadcrumbs, which is the point of accumulating), and it
    // ZEROES the rest of the slot, which is what lets the host reader trim on
    // trailing NULs and never show a previous boot's tail as if it were this
    // boot's. 502 sectors is 8 SCSI commands through blk_write's 32 KB bounce.
    if (flt_wr(flt_txt_lba(head), FLTREC_TEXT_SECTORS, g_text) != 0) {
        kprintf("[FLTREC] slot text write failed; not arming\n");
        g_dead = 1;
        return 0;
    }
    g_flushed = g_len;

    if (flt_write_hdr() != 0) {
        kprintf("[FLTREC] slot header write failed; not arming\n");
        g_dead = 1;
        return 0;
    }

    g_armed = 1;
    kprintf("[FLTREC] armed: slot %u/%u, seq %llu, LBA %llu..%llu, %u bytes carried over\n",
            head, FLTREC_SLOTS, (unsigned long long)seq,
            (unsigned long long)flt_hdr_lba(head),
            (unsigned long long)(flt_hdr_lba(head) + FLTREC_SLOT_SECTORS - 1),
            g_flushed);
    snprintf(msg, sizeof(msg), "[FLTREC] armed slot=%u seq=%llu %s",
             head, (unsigned long long)seq, g_ident);
    fltrec_write(msg);
    return 1;
}

void fltrec_write(const char *line) {
    uint32_t n, before;

    if (!line) return;
    n = (uint32_t)strlen(line);
    if (n) {
        before = g_len;
        g_len = fltrec_append_rs((uint8_t *)g_text, FLT_USER_CAP, g_len,
                                 (const uint8_t *)line, n, 1);
        if (g_len == before) {
            // Did not fit. The reserve above FLT_USER_CAP exists precisely so
            // the truncation notice can always be written; without it, the one
            // line that can never fit is the one that says lines are being lost.
            g_dropped += n;
            if (!g_full) {
                g_full = 1;
                g_len = fltrec_append_rs((uint8_t *)g_text, FLTREC_TEXT_CAP, g_len,
                                         (const uint8_t *)"[FLTREC] SLOT FULL - later lines dropped",
                                         39u, 1);
            }
        }
    }

    if (!g_armed || g_dead) return;              // still accumulated, see fltrec.h
    if (g_in_io || g_defer) { g_deferred++; return; }
    flt_flush_text();
}

void fltrec_flush(void) {
    if (!g_armed || g_dead) return;
    if (g_in_io || g_defer) { g_deferred++; return; }
    flt_flush_text();
    (void)flt_write_hdr();
}

void fltrec_seal(int ok) {
    if (!g_armed || g_dead) return;
    if (g_in_io || g_defer) { g_deferred++; return; }
    g_verdict = ok ? FLTREC_VERDICT_OK : FLTREC_VERDICT_FAIL;
    g_seal_ms = mono_ready() ? mono_ms() : 0;
    flt_flush_text();
    (void)flt_write_hdr();
    kprintf("[FLTREC] sealed slot %u seq %llu verdict %s (%u bytes, %llu sectors, "
            "%u write failures, %llu bytes dropped, %llu flushes deferred)\n",
            g_slot, (unsigned long long)g_seq, ok ? "OK" : "FAIL", g_flushed,
            (unsigned long long)g_sectors, g_failures,
            (unsigned long long)g_dropped, (unsigned long long)g_deferred);
}

void fltrec_stats(uint64_t *sectors, uint32_t *failures, uint64_t *dropped) {
    if (sectors)  *sectors  = g_sectors;
    if (failures) *failures = g_failures;
    if (dropped)  *dropped  = g_dropped;
}

int fltrec_selftest(uint32_t *checks) {
    uint32_t n = 0;
    int r = fltrec_selftest_rs(&n);
    if (checks) *checks = n;
    // ANTI-VACUITY. A harness that executed zero assertions and returned 0 is
    // the shape this tree has been burned by repeatedly, so zero checks is a
    // FAIL here whatever the Rust said.
    if (n == 0) return -1;
    return r;
}
