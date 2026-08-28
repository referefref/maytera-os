// usbvol.c - #740: probe the boot device's unpartitioned tail for ISO 9660 data
//            volumes and mount each on a CD drive letter. See usbvol.h for what
//            this is and rustkern/usbvol.rs for the on-disk contract.
//
// LANGUAGE NOTE (standing Rust-first rule): the POLICY and every piece of
// arithmetic over untrusted on-disk bytes is in rustkern/usbvol.rs. This file
// is C because it is I/O glue against blk_read(), usb_msc_get_device(),
// kmalloc() and kprintf(), exactly the split dos/imgfile.c already documents.
// There is no arithmetic here that is not first checked by a Rust function: the
// volume length comes from usbvol_iso_extent_rs(), every span is cleared by
// usbvol_range_ok_rs(), and the walk advances only by usbvol_next_off_rs().
#include "usbvol.h"
#include "diskimg.h"
#include "imgfile.h"
#include "../types.h"
#include "../string.h"
#include "../fs/blockdev.h"
#include "../fs/bootlog.h"    // bootlog_write: the owning header, never a private extern
#include "../drivers/usb_msc.h"

extern int  kprintf(const char *fmt, ...);
extern void *kmalloc(unsigned long n);
extern void  kfree(void *p);

// ---------------------------------------------------------------------------
// rustkern/usbvol.rs. THE constants and THE arithmetic; nothing is mirrored
// here. Fetching them at runtime instead of restating them in a #define is
// deliberate: a mirrored constant is a constant that can drift, and this file
// has no need to know the values at compile time.
// ---------------------------------------------------------------------------
extern long long usbvol_iso_extent_rs(const unsigned char *sec, unsigned int len);
extern int       usbvol_range_ok_rs(uint64_t off, uint64_t len, uint64_t dev_bytes);
extern long long usbvol_next_off_rs(uint64_t cur, uint64_t vol_bytes, uint64_t dev_bytes);
extern void      usbvol_consts_rs(uint64_t *base, uint64_t *align,
                                  uint64_t *probe_span, uint32_t *max);
extern int       usbvol_selftest_rs(uint32_t *out_checks);

static int g_first_idx = -1;

int usbvol_first_letter_idx(void) { return g_first_idx; }

void usbvol_selftest(void) {
    uint32_t checks = 0;
    int fails = usbvol_selftest_rs(&checks);
    kprintf("[USBVOL] geometry self-test: %u checks, %d failure(s)\n",
            (unsigned)checks, fails);
    bootlog_write("[USBVOL] geometry self-test: %u checks, %d failure(s)",
                  (unsigned)checks, fails);
}

// Append `digits` uppercase hex digits of `v` to b[*n].
static void put_hex(char *b, int *n, uint64_t v, int digits) {
    static const char H[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--)
        b[(*n)++] = H[(v >> (i * 4)) & 0xF];
}

// Build the synthetic path that tells imgfile_open() to read a raw device
// range. The format is defined by IMGF_BLKDEV_PREFIX in dos/imgfile.h; this is
// its only producer. `cap` must be at least 50.
static int make_blkdev_path(char *out, int cap, uint8_t ch, uint8_t dr,
                            uint64_t base, uint64_t len) {
    int n = 0;
    for (const char *p = IMGF_BLKDEV_PREFIX; *p; p++) {
        if (n >= cap - 1) return -1;
        out[n++] = *p;
    }
    if (n + 2 + 1 + 2 + 1 + 16 + 1 + 16 + 1 > cap) return -1;
    put_hex(out, &n, ch, 2);   out[n++] = ':';
    put_hex(out, &n, dr, 2);   out[n++] = ':';
    put_hex(out, &n, base, 16); out[n++] = ':';
    put_hex(out, &n, len, 16);
    out[n] = '\0';
    return n;
}

// Bounded directory-listing callback: prints the first few entries of a mounted
// volume so the serial log shows WHAT was found, not merely that something was.
typedef struct { int shown; int total; } listctx_t;
#define USBVOL_LIST_SHOW 12

static void list_cb(const char *name, int is_dir, unsigned int size, void *ud) {
    listctx_t *c = (listctx_t *)ud;
    c->total++;
    if (c->shown < USBVOL_LIST_SHOW) {
        c->shown++;
        kprintf("[USBVOL]     %s%s%s (%u bytes)\n",
                is_dir ? "[" : "", name, is_dir ? "]" : "", size);
    }
}

void usbvol_probe_and_mount(void) {
    uint64_t base = 0, align = 0, span = 0;
    uint32_t maxv = 0;
    usbvol_consts_rs(&base, &align, &span, &maxv);

    if (!blk_root_is_usb()) {
        kprintf("[USBVOL] root block device is not USB; tail data volumes not probed "
                "(see dos/usbvol.h for why this is USB-only)\n");
        return;
    }
    usb_msc_device_t *dev = usb_msc_get_device(blk_root_usb_index());
    if (!dev || !dev->ready) {
        kprintf("[USBVOL] USB root device not ready; not probed\n");
        return;
    }
    if (dev->block_size != 512) {
        kprintf("[USBVOL] USB root block size is %u, not 512; not probed\n",
                (unsigned)dev->block_size);
        return;
    }

    uint64_t dev_bytes = dev->num_blocks * 512ull;
    kprintf("[USBVOL] boot device %llu MB; probing tail from %llu MB, at most %u volume(s)\n",
            (unsigned long long)(dev_bytes / (1024 * 1024)),
            (unsigned long long)(base / (1024 * 1024)), (unsigned)maxv);

    if (dev_bytes <= base) {
        kprintf("[USBVOL] device is smaller than the %llu MB volume base; nothing to probe\n",
                (unsigned long long)(base / (1024 * 1024)));
        bootlog_write("[USBVOL] device %llu MB too small for a data volume",
                      (unsigned long long)(dev_bytes / (1024 * 1024)));
        return;
    }

    unsigned char *sec = (unsigned char *)kmalloc(2048);
    if (!sec) {
        kprintf("[USBVOL] out of memory for the probe buffer\n");
        return;
    }

    uint64_t off = base;
    int mounted = 0;

    // Bounded by maxv, and every iteration must advance strictly (enforced in
    // usbvol_next_off_rs). This is a scan, not a wait: it never polls, never
    // retries, and cannot be made to loop by anything on the medium.
    for (uint32_t i = 0; i < maxv; i++) {
        if (usbvol_range_ok_rs(off, span, dev_bytes) != 1) break;

        // ISO 9660 puts the primary volume descriptor at logical sector 16.
        uint64_t pvd_off = off + 16ull * 2048ull;
        if (blk_read(0, 0, pvd_off / 512ull, 4, sec) != 4) {
            kprintf("[USBVOL] read failed at %llu MB; stopping the scan\n",
                    (unsigned long long)(off / (1024 * 1024)));
            break;
        }

        long long ext = usbvol_iso_extent_rs(sec, 2048);
        if (ext < 0) {
            if (i == 0)
                kprintf("[USBVOL] no data volume at %llu MB (rc=%lld); tail is empty\n",
                        (unsigned long long)(off / (1024 * 1024)), ext);
            break;
        }
        if (usbvol_range_ok_rs(off, (uint64_t)ext, dev_bytes) != 1) {
            kprintf("[USBVOL] volume at %llu MB declares %llu bytes, which does not fit "
                    "the device; REFUSED\n",
                    (unsigned long long)(off / (1024 * 1024)), (unsigned long long)ext);
            break;
        }

        char path[64];
        if (make_blkdev_path(path, (int)sizeof path, 0, 0, off, (uint64_t)ext) < 0) break;

        // The block layer's counters, sampled around the mount. The mount reads
        // the descriptors and the root directory through imgfile, so the delta
        // is direct evidence of WHERE those bytes came from. This is the whole
        // claim of the feature, so it is measured rather than asserted.
        uint64_t h0 = 0, m0 = 0, h1 = 0, m1 = 0;
        int mode = 0;
        blk_cache_stats(&h0, &m0, &mode);

        int idx = diskimg_mount_idx(DISKIMG_LETTER_AUTO, path);
        if (idx < 0) {
            kprintf("[USBVOL] volume at %llu MB (%llu MB long): mount REFUSED rc=%d\n",
                    (unsigned long long)(off / (1024 * 1024)),
                    (unsigned long long)((uint64_t)ext / (1024 * 1024)), idx);
            break;
        }
        char letter = (char)('A' + idx);

        char label[16];
        if (!diskimg_volume_label(letter, label, (int)sizeof label)) label[0] = '\0';

        kprintf("[USBVOL] volume %d at %llu MB, %llu MB long, label '%s' -> mounted %c:\n",
                mounted + 1,
                (unsigned long long)(off / (1024 * 1024)),
                (unsigned long long)((uint64_t)ext / (1024 * 1024)),
                label, letter);

        listctx_t lc = { 0, 0 };
        int n = diskimg_listdir(letter, "/", list_cb, &lc);

        blk_cache_stats(&h1, &m1, &mode);
        kprintf("[USBVOL] %c: root has %d entries; block layer during mount+list: "
                "device sectors +%llu, RAM-copy hits +%llu (mode=%d)\n",
                letter, n,
                (unsigned long long)(m1 - m0), (unsigned long long)(h1 - h0), mode);
        bootlog_write("[USBVOL] %c: '%s' %llu MB at %llu MB, %d entries, "
                      "device sectors +%llu, RAM hits +%llu",
                      letter, label,
                      (unsigned long long)((uint64_t)ext / (1024 * 1024)),
                      (unsigned long long)(off / (1024 * 1024)), n,
                      (unsigned long long)(m1 - m0), (unsigned long long)(h1 - h0));

        if (g_first_idx < 0) g_first_idx = idx;
        mounted++;

        long long nx = usbvol_next_off_rs(off, (uint64_t)ext, dev_bytes);
        if (nx < 0) break;
        off = (uint64_t)nx;
    }

    kfree(sec);

    if (mounted == 0)
        kprintf("[USBVOL] no data volumes mounted\n");
    else
        kprintf("[USBVOL] %d data volume(s) mounted from the boot device tail\n", mounted);
}
