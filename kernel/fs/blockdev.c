// blockdev.c - #307 block-device routing (ATA disk vs USB Mass Storage root)
//              + #375 USB "to-RAM" root so a slow USB stick is read once.
//              + #417 size that RAM copy off ACTUAL USED filesystem data
//                instead of raw device/partition capacity (see blk_try_toram).
//
// Single choke point for FS-layer sector I/O. Default = ATA (no behavior change
// for existing ATA-disk VMs). When a USB thumb drive carrying a valid MayteraOS
// layout is selected as root at boot, all FAT/ext2 sector reads/writes route to
// that USB MSC device instead, enabling a USB-only boot on real hardware.
//
// #375 TO-RAM ROOT (the slow-image-loading fix): the FAT/exFAT/ext2 drivers read
// the medium one 512-byte sector at a time with no cache (FAT-table walks, dir
// scans, file cluster reads), so on a SLOW USB stick (the real iMac) opening any
// image/app is a long stream of tiny USB round-trips and feels sluggish; VMs hide
// it because their virtual USB is instant. The clean fix is to copy the ENTIRE
// root block device into a RAM region ONCE at boot - with LARGE sequential
// multi-sector transfers - and then serve every subsequent sector read from RAM
// at memory speed, never touching the stick again for reads. The image is ~904 MB
// and the machine has 16 GB, so it fits trivially. Writes are WRITE-THROUGH (RAM
// + stick): the RAM copy stays coherent so reads remain instant, and on-disk data
// (settings, notes, and crucially the /HEARTBEAT.TXT + /BOOTLOG.TXT diagnostics)
// still persists to the physical stick so it survives a power-off. If the full
// RAM image does not fit (or the device is too large), we transparently fall back
// to a bounded demand block cache (still a big win for repeated reads). Both are
// active ONLY for a USB root; ATA-disk VMs are byte-for-byte unaffected.

#include "blockdev.h"
#include "../drivers/usb_msc.h"
#include "../mm/pmm.h"
#include "../string.h"

extern int kprintf(const char *fmt, ...);
extern void proc_yield(void);
extern void gfx_boot_progress(int percent);
extern void gfx_boot_log(const char *message);

// ATA DMA sector I/O (implemented in drivers/ata.c).
extern int ata_read_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                                uint8_t count, void *buffer);
extern int ata_write_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                                 uint8_t count, const void *buffer);

// #550: maximum number of CONSECUTIVE sectors coalesced into one SCSI READ(10)
// on the demand-cache miss path. This was 1, so every cache miss cost a separate
// USB Bulk-Only Transport command (CBW + data + CSW = three xHCI waits, and
// usb_msc serialises them all on one global g_msc_cmd_busy). A 17 KB WAV was 35
// commands; that is where the multi-hundred-ms stalls on the real iMac came from.
//
// NOTE this is a MAXIMUM, not a fixed count. It CANNOT simply be raised in place:
// the old loop passed it as the sector count while advancing the destination by
// exactly ONE sector per iteration, so any value above 1 would have overrun the
// caller's buffer. blk_read now coalesces an actual run of consecutive misses and
// reads it directly into the (contiguous) caller buffer. 64 sectors = 32 KB, the
// same transfer size the TO-RAM bulk fill has been using reliably (BLK_FILL_SECS).
#define BLK_USB_CHUNK 64

static int g_root_usb = 0;
static int g_root_usb_index = -1;

// =============================================================================
// #375: shared 2 MB RAM-chunk store (used by both TO-RAM and the demand cache).
// =============================================================================

#define BLK_SECTOR         512u
#define BLK_CHUNK_PAGES    512u                          // 2 MB per data chunk
#define BLK_CHUNK_BYTES    (BLK_CHUNK_PAGES * PMM_PAGE_SIZE)
#define BLK_CHUNK_SECS     (BLK_CHUNK_BYTES / BLK_SECTOR)  // 4096 sectors/chunk
#define BLK_MAX_CHUNKS     1280u                          // up to ~2.5 GB device
// Keep this many pages (256 MB) free after reserving RAM, so the OS is not
// starved. If the full TO-RAM image would breach it we fall back to the cache.
#define BLK_KEEP_FREE_PAGES 65536u
// TO-RAM bulk fill transfer size: 64 sectors = 32 KB per SCSI READ(10) (fast:
// 64x fewer USB commands than the 1-sector cold path). An xHCI Transfer TRB must
// NOT cross a 64 KB boundary (xhci_bulk_transfer emits ONE TRB), so each 2 MB
// chunk is 64 KB-ALIGNED (see blk_alloc_chunks) and 32 KB transfers step within
// each 64 KB window, so a fill transfer never crosses a 64 KB boundary. 64
// divides BLK_CHUNK_SECS (4096) evenly so a transfer never crosses a chunk either.
#define BLK_FILL_SECS      64u
#define BLK_ALIGN64K       0x10000ULL
// Over-allocate each chunk by 64 KB (16 pages) so it can be aligned up to 64 KB.
#define BLK_CHUNK_ALLOC_PAGES (BLK_CHUNK_PAGES + 16u)
// CRITICAL: the kernel HEAP is at VIRTUAL base HEAP_VIRT_BASE=0x10000000 (256 MB)
// and vmm-remaps that virtual window to arbitrary physical pages (see mm/heap.c).
// So the identity-mapped virtual address of any PHYSICAL page in
// [0x10000000, 0x10000000 + HEAP_MAX_SIZE) is HIJACKED by the heap: writing to
// such a page via an identity pointer actually clobbers the live heap. TO-RAM
// spans this range, so chunks whose usable region overlaps the heap window MUST
// be skipped, or the heap free-list is corrupted (observed as a #GP in
// remove_from_free_list right after a "100%" copy). HEAP_MAX_SIZE is 256 MB.
#define BLK_HEAP_WIN_LO    0x10000000ULL
#define BLK_HEAP_WIN_HI    0x20000000ULL   // 0x10000000 + HEAP_MAX_SIZE (256 MB)
#define BLK_MAX_PARKED     160u            // 256 MB window / 2 MB, + slack

static uint8_t  *g_chunk[BLK_MAX_CHUNKS];        // 64 KB-aligned usable base
static uint64_t  g_chunk_alloc[BLK_MAX_CHUNKS];  // raw pmm base (for freeing)
static uint32_t  g_nchunks = 0;
// Heap-window physical pages we permanently RESERVE (keep allocated) so that
// nothing - not TO-RAM, not a later AP stack or GUI buffer - ever uses a page
// whose identity virtual address is hijacked by the heap remap. Freed only at
// teardown. Leaving these floating (freeing them) let a post-copy allocation
// grab a window page and #GP jumping to garbage (SMP trampoline addr 0x8008).
static uint64_t  g_parked[BLK_MAX_PARKED];
static uint32_t  g_nparked = 0;

// Mode: 0 = off (pass-through to USB), 1 = TO-RAM, 2 = demand cache.
#define BLK_MODE_OFF   0
#define BLK_MODE_TORAM 1
#define BLK_MODE_CACHE 2
static int g_mode = BLK_MODE_OFF;

// #417: forced off by a boot-time config marker (/TORAMOFF.TXT). Demand cache
// still applies in this case, so a stick can still get *some* caching benefit
// with TO-RAM's bulk RAM copy disabled.
static int g_toram_disabled = 0;

// TO-RAM: g_chunk[] holds device sectors 0 .. g_ram_sectors-1 contiguously.
static uint64_t g_ram_sectors = 0;

// Demand cache (fallback): direct-mapped over g_chunk[], tag = resident LBA.
#define BLK_TAG_INVALID 0xFFFFFFFFFFFFFFFFULL
static uint64_t *g_tag = 0;
static uint32_t  g_slots = 0;
static uint32_t  g_mask = 0;

// Simple yielding lock (tiny holds; matches msc_cmd_lock pattern in the tree).
static volatile int g_blk_busy = 0;
static void blk_lk(void)   { while (__sync_lock_test_and_set(&g_blk_busy, 1)) proc_yield(); }
static void blk_ul(void)   { __sync_lock_release(&g_blk_busy); }

// Stats: RAM-served sectors vs USB-served sectors (verification).
static uint64_t g_ram_hits = 0;
static uint64_t g_usb_reads = 0;

static inline uint8_t *chunk_sector(uint64_t sec) {
    return g_chunk[sec / BLK_CHUNK_SECS] + (sec % BLK_CHUNK_SECS) * BLK_SECTOR;
}

static void blk_free_chunks(void) {
    for (uint32_t i = 0; i < g_nchunks; i++) {
        if (g_chunk_alloc[i]) { pmm_free_pages(g_chunk_alloc[i], BLK_CHUNK_ALLOC_PAGES); }
        g_chunk_alloc[i] = 0; g_chunk[i] = 0;
    }
    g_nchunks = 0;
    for (uint32_t j = 0; j < g_nparked; j++)
        if (g_parked[j]) pmm_free_pages(g_parked[j], BLK_CHUNK_ALLOC_PAGES);
    g_nparked = 0;
    if (g_tag) {
        pmm_free_pages((uint64_t)g_tag, (g_slots * sizeof(uint64_t) + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
        g_tag = 0;
    }
    g_slots = 0; g_mask = 0; g_ram_sectors = 0;
    g_mode = BLK_MODE_OFF;
}

// Allocate `n` 2 MB chunks. Returns number actually allocated (may be < n).
static uint32_t blk_alloc_chunks(uint32_t n) {
    if (n > BLK_MAX_CHUNKS) n = BLK_MAX_CHUNKS;
    // Chunks whose usable region overlaps the heap virtual window are unusable
    // via identity pointers (they alias the heap); PARK them (keep them allocated
    // so pmm's first-fit walks past them) and KEEP them reserved for the whole
    // session so no later allocation reuses a window page either.
    uint32_t got = 0;
    for (uint32_t i = 0; i < n; ) {
        uint64_t raw = pmm_alloc_pages(BLK_CHUNK_ALLOC_PAGES);
        if (!raw) break;
        uint64_t aligned = (raw + BLK_ALIGN64K - 1) & ~(BLK_ALIGN64K - 1);
        uint64_t use_lo = aligned;
        uint64_t use_hi = aligned + (uint64_t)BLK_CHUNK_PAGES * PMM_PAGE_SIZE; // +2 MB usable
        // Overlaps the heap window? [use_lo,use_hi) vs [WIN_LO,WIN_HI)
        if (use_lo < BLK_HEAP_WIN_HI && use_hi > BLK_HEAP_WIN_LO) {
            if (g_nparked < BLK_MAX_PARKED) { g_parked[g_nparked++] = raw; continue; }
            pmm_free_pages(raw, BLK_CHUNK_ALLOC_PAGES);   // out of park slots: stop
            break;
        }
        g_chunk_alloc[i] = raw;
        g_chunk[i] = (uint8_t *)aligned;   // 64 KB-aligned, TRB-safe, heap-safe
        got++;
        i++;
    }
    g_nchunks = got;
    return got;   // parked chunks stay reserved until blk_free_chunks()
}

// -----------------------------------------------------------------------------
// TO-RAM: copy the USB root into RAM with large sequential reads. The window
// size is derived from ACTUAL USED filesystem data (used_bytes_hint), not the
// raw device/partition capacity: a live-USB stick's GPT+FAT32 can be expanded
// to fill a much larger drive than the real payload (e.g. 116 GB volume, ~340
// MB of real files), and sizing off the raw capacity made TO-RAM bail out to a
// tiny demand cache on every such stick even though the real data would have
// fit trivially. Returns 1 on success (mode = TO-RAM), 0 if it did not fit /
// failed.
// -----------------------------------------------------------------------------
static int blk_try_toram(usb_msc_device_t *dev, uint64_t used_bytes_hint) {
    uint64_t dev_total = dev->num_blocks;
    if (dev_total == 0) return 0;

    uint64_t total;
    if (used_bytes_hint > 0) {
        // Sectors needed to hold the real data, plus a generous +50% safety
        // margin (fragmentation headroom + room for the writes a live session
        // makes: settings, notes, /HEARTBEAT.TXT, /BOOTLOG.TXT, ...), rounded
        // up to a whole 2 MB chunk. Anything beyond this window still works
        // correctly (blk_read/blk_write fall through to a live USB access for
        // any LBA outside it); this just decides how much rides in RAM.
        uint64_t need_sectors = (used_bytes_hint + BLK_SECTOR - 1) / BLK_SECTOR;
        uint64_t margin = need_sectors / 2 + (16ull * 1024 * 1024) / BLK_SECTOR;  // +50% + 16MB floor
        need_sectors += margin;
        need_sectors = ((need_sectors + BLK_CHUNK_SECS - 1) / BLK_CHUNK_SECS) * BLK_CHUNK_SECS;
        total = (need_sectors < dev_total) ? need_sectors : dev_total;
        kprintf("[TORAM] used-data hint %llu MB -> RAM window %llu MB (device is %llu MB)\n",
                (unsigned long long)(used_bytes_hint / (1024 * 1024)),
                (unsigned long long)((total * BLK_SECTOR) / (1024 * 1024)),
                (unsigned long long)((dev_total * BLK_SECTOR) / (1024 * 1024)));
    } else {
        // No filesystem-level hint available (caller doesn't know / legacy
        // call site): fall back to the original whole-device sizing so
        // behavior is unchanged for callers that can't compute a hint.
        total = dev_total;
        kprintf("[TORAM] no used-data hint; sizing off whole device (%llu MB)\n",
                (unsigned long long)((dev_total * BLK_SECTOR) / (1024 * 1024)));
    }

    // #539: the +50% write-margin can push `total` past the chunk cap when the
    // hint already approximates the whole image (e.g. the two-partition golden
    // written to a stick MUCH larger than the ~1.8 GB payload). We ALWAYS fill
    // from LBA 0 upward and the payload lives at the FRONT of the device, so if
    // the REAL data (the un-margined hint) fits inside the cap window, clamp the
    // window to the cap instead of falling back to the demand cache: a cap-sized
    // (2.5 GB) window still holds the whole <=2.5 GB image in RAM, and only the
    // never-read empty tail past the cap is excluded. Gated on a real hint so the
    // no-hint whole-device path (data location unknown) keeps the old behavior.
    uint64_t cap_sectors = (uint64_t)BLK_MAX_CHUNKS * BLK_CHUNK_SECS;
    if (used_bytes_hint > 0 && total > cap_sectors) {
        uint64_t data_sectors = (used_bytes_hint + BLK_SECTOR - 1) / BLK_SECTOR;
        if (data_sectors <= cap_sectors) {
            total = cap_sectors;
            kprintf("[TORAM] window clamped to cap %llu MB (payload %llu MB is at front of device)\n",
                    (unsigned long long)((total * BLK_SECTOR) / (1024 * 1024)),
                    (unsigned long long)(used_bytes_hint / (1024 * 1024)));
        }
    }

    uint32_t need_chunks = (uint32_t)((total + BLK_CHUNK_SECS - 1) / BLK_CHUNK_SECS);
    if (need_chunks > BLK_MAX_CHUNKS) {
        kprintf("[TORAM] RAM window too large (%llu sectors, ~%llu MB), using demand cache\n",
                (unsigned long long)total, (unsigned long long)((total * BLK_SECTOR) / (1024 * 1024)));
        return 0;
    }
    uint64_t need_pages = (uint64_t)need_chunks * BLK_CHUNK_ALLOC_PAGES;
    uint64_t free_pages = pmm_get_free_pages();
    if (free_pages < need_pages + BLK_KEEP_FREE_PAGES) {
        kprintf("[TORAM] not enough RAM (%llu free, need %llu pages), using demand cache\n",
                (unsigned long long)free_pages, (unsigned long long)need_pages);
        return 0;
    }

    uint32_t mb = (uint32_t)((total * BLK_SECTOR) / (1024 * 1024));
    kprintf("[TORAM] copying %u MB root to RAM (%u chunks, %llu free pages)...\n",
            mb, need_chunks, (unsigned long long)free_pages);
    gfx_boot_log("[BOOT] Copying root to RAM (fast reads)...");

    if (blk_alloc_chunks(need_chunks) < need_chunks) {
        kprintf("[TORAM] chunk allocation short (%u/%u), reverting to cache\n",
                g_nchunks, need_chunks);
        blk_free_chunks();
        return 0;
    }
    g_ram_sectors = (uint64_t)need_chunks * BLK_CHUNK_SECS;

    // Bulk sequential fill. Large 32 KB reads; on any read error retry that span
    // one sector at a time, and if even that fails, abort TO-RAM (fall back).
    uint64_t base = 0;
    int last_pct = -1;
    while (base < total) {
        uint32_t n = (uint32_t)((total - base) < BLK_FILL_SECS ? (total - base) : BLK_FILL_SECS);
        uint8_t *dst = chunk_sector(base);
        if (usb_msc_read(dev, 0, base, dst, n) != 0) {
            int ok = 1;
            for (uint32_t k = 0; k < n; k++) {
                if (usb_msc_read(dev, 0, base + k, chunk_sector(base + k), 1) != 0) { ok = 0; break; }
            }
            if (!ok) {
                kprintf("[TORAM] read failed at LBA %llu, reverting to demand cache\n",
                        (unsigned long long)(base));
                blk_free_chunks();
                return 0;
            }
        }
        base += n;
        int pct = (int)((base * 100) / total);
        if (pct != last_pct && (pct % 2) == 0) {
            last_pct = pct;
            gfx_boot_progress(60 + pct * 38 / 100);   // map fill onto 60..98%
            kprintf("[TORAM] fill %d%% (%llu/%llu sectors)\n",
                    pct, (unsigned long long)base, (unsigned long long)total);
        }
    }

    g_mode = BLK_MODE_TORAM;
    g_ram_hits = 0;
    g_usb_reads = 0;
    kprintf("[TORAM] root fully in RAM (%u MB); stick idle for reads now\n", mb);
    gfx_boot_log("[BOOT] Root loaded to RAM");
    gfx_boot_progress(98);
    return 1;
}

// -----------------------------------------------------------------------------
// Demand cache fallback: bounded direct-mapped RAM cache of 512-byte sectors.
// -----------------------------------------------------------------------------
// #539: raised from 64 (128 MB) to 256 (512 MB). When TO-RAM cannot fit the
// whole two-partition image, the ext2 ROOT should still get a meaningful cache
// rather than only 128 MB. This is only a CEILING: blk_enable_cache() below
// trims `want` down to the free-RAM budget (leaving BLK_KEEP_FREE_PAGES), so a
// low-RAM box never OOMs; it just gets a smaller cache.
#define BLK_CACHE_CHUNKS 256u   // up to 512 MB cache when TO-RAM does not fit
static void blk_enable_cache(void) {
    uint32_t want = BLK_CACHE_CHUNKS;
    uint64_t free_pages = pmm_get_free_pages();
    // Trim to leave the keep-free margin.
    uint64_t budget = (free_pages > BLK_KEEP_FREE_PAGES) ? (free_pages - BLK_KEEP_FREE_PAGES) : 0;
    uint32_t fit = (uint32_t)(budget / BLK_CHUNK_ALLOC_PAGES);
    if (fit < want) want = fit;
    if (want == 0) { kprintf("[BLKC] no RAM for cache; passthrough\n"); return; }

    if (blk_alloc_chunks(want) == 0) { blk_free_chunks(); return; }
    g_slots = g_nchunks * BLK_CHUNK_SECS;        // power of two (chunks*4096)
    g_mask  = g_slots - 1;
    uint32_t tag_pages = (uint32_t)((g_slots * sizeof(uint64_t) + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    g_tag = (uint64_t *)pmm_alloc_pages(tag_pages);
    if (!g_tag) { blk_free_chunks(); kprintf("[BLKC] tag alloc failed; passthrough\n"); return; }
    memset(g_tag, 0xFF, g_slots * sizeof(uint64_t));
    g_mode = BLK_MODE_CACHE;
    g_ram_hits = 0; g_usb_reads = 0;
    kprintf("[BLKC] demand cache: %u MB (%u chunks)\n",
            (g_slots * BLK_SECTOR) / (1024 * 1024), g_nchunks);
}

// =============================================================================

void blk_set_root_usb(int usb_msc_index) {
    g_root_usb = 1;
    g_root_usb_index = usb_msc_index;
    kprintf("[BLK] root block device -> USB MSC index %d\n", usb_msc_index);
}

// #417: force TO-RAM off (demand cache still applies). Called from main.c
// when /TORAMOFF.TXT is present at the FAT root.
void blk_toram_set_disabled(int disabled) {
    g_toram_disabled = disabled;
    if (disabled) kprintf("[TORAM] disabled by /TORAMOFF.TXT config marker\n");
}

// #375/#417: called by main.c AFTER the USB root is verified as a MayteraOS
// root. `used_bytes_hint` (bytes of real filesystem data, 0 if unknown) sizes
// the RAM window; see blk_try_toram(). Attempts TO-RAM; on failure (or when
// disabled by config) enables the demand cache. Returns 1 if the sized root
// is now in RAM, 0 if using the (still helpful) demand cache / passthrough.
int blk_root_to_ram(uint64_t used_bytes_hint) {
    if (!g_root_usb || g_mode != BLK_MODE_OFF) return (g_mode == BLK_MODE_TORAM);
    usb_msc_device_t *dev = usb_msc_get_device(g_root_usb_index);
    if (!dev || !dev->ready) return 0;
    if (!g_toram_disabled && blk_try_toram(dev, used_bytes_hint)) return 1;
    blk_enable_cache();
    return 0;
}

void blk_clear_root_usb(void) {
    g_root_usb = 0;
    g_root_usb_index = -1;
    blk_free_chunks();
}

int blk_root_is_usb(void) { return g_root_usb; }

void blk_cache_stats(uint64_t *hits, uint64_t *misses, int *enabled) {
    if (hits) *hits = g_ram_hits;
    if (misses) *misses = g_usb_reads;
    if (enabled) *enabled = g_mode;   // 0 off, 1 toram, 2 cache
}

int blk_read(uint8_t channel, uint8_t drive, uint64_t lba, uint32_t count, void *buf) {
    if (g_root_usb) {
        // TO-RAM: serve entirely from RAM (the win). Stick untouched.
        if (g_mode == BLK_MODE_TORAM && lba + count <= g_ram_sectors) {
            uint8_t *p = (uint8_t *)buf;
            blk_lk();
            for (uint32_t i = 0; i < count; i++)
                memcpy(p + (uint64_t)i * BLK_SECTOR, chunk_sector(lba + i), BLK_SECTOR);
            g_ram_hits += count;
            blk_ul();
            return (int)count;
        }
        usb_msc_device_t *dev = usb_msc_get_device(g_root_usb_index);
        if (!dev || !dev->ready) return -1;
        uint8_t *p = (uint8_t *)buf;
        for (uint32_t i = 0; i < count; ) {
            uint64_t lba_i = lba + i;
            uint8_t *dst = p + (uint64_t)i * BLK_SECTOR;
            // Demand cache hit? Serve this one sector from RAM and move on.
            if (g_mode == BLK_MODE_CACHE) {
                uint32_t idx = (uint32_t)lba_i & g_mask;
                blk_lk();
                if (g_tag[idx] == lba_i) {
                    memcpy(dst, chunk_sector(idx), BLK_SECTOR);
                    g_ram_hits++;
                    blk_ul();
                    i++;
                    continue;
                }
                blk_ul();
            }
            // #550 MISS: coalesce the run of consecutive sectors that ALSO miss
            // and fetch the whole run in ONE SCSI READ(10), straight into the
            // caller's buffer (which is contiguous, so no bounce buffer needed).
            // Stop the run at the first sector already cached: re-reading it from
            // USB would be slower than the memcpy we would otherwise do.
            uint32_t run = 1;
            while (run < BLK_USB_CHUNK && i + run < count) {
                if (g_mode == BLK_MODE_CACHE) {
                    uint64_t l2 = lba + i + run;
                    uint32_t idx2 = (uint32_t)l2 & g_mask;
                    int hit;
                    blk_lk(); hit = (g_tag[idx2] == l2); blk_ul();
                    if (hit) break;
                }
                run++;
            }
            // An xHCI Transfer TRB must NOT cross a 64 KB boundary (xhci_bulk_
            // transfer emits ONE TRB). The TO-RAM fill path gets this for free
            // because its chunks are 64 KB-aligned, but the CALLER's buffer here
            // has arbitrary alignment, so clip the run at the next 64 KB edge.
            uint64_t off  = (uint64_t)dst & (BLK_ALIGN64K - 1);
            uint32_t room = (uint32_t)((BLK_ALIGN64K - off) / BLK_SECTOR);
            if (room == 0) room = 1;
            if (run > room) run = room;

            if (usb_msc_read(dev, 0, lba_i, dst, run) != 0) return -1;
            g_usb_reads += run;
            if (g_mode == BLK_MODE_CACHE) {
                blk_lk();
                for (uint32_t k = 0; k < run; k++) {
                    uint32_t idx = (uint32_t)(lba_i + k) & g_mask;
                    memcpy(chunk_sector(idx), dst + (uint64_t)k * BLK_SECTOR,
                           BLK_SECTOR);
                    g_tag[idx] = lba_i + k;
                }
                blk_ul();
            }
            i += run;
        }
        return (int)count;
    }
    return ata_read_sectors_dma(channel, drive, (uint32_t)lba, (uint8_t)count, buf);
}

int blk_write(uint8_t channel, uint8_t drive, uint64_t lba, uint32_t count, const void *buf) {
    if (g_root_usb) {
        usb_msc_device_t *dev = usb_msc_get_device(g_root_usb_index);
        if (!dev || !dev->ready) return -1;
        const uint8_t *p = (const uint8_t *)buf;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t lba_i = lba + i;
            const uint8_t *src = p + (uint64_t)i * BLK_SECTOR;
            // WRITE-THROUGH: persist to the physical stick (settings, notes, and
            // the /HEARTBEAT.TXT + /BOOTLOG.TXT diagnostics must survive power-off),
            // then keep the RAM copy coherent so reads stay instant.
            // #550: single sector, ALWAYS. Do NOT substitute BLK_USB_CHUNK here:
            // src advances by exactly one sector per iteration, so any count > 1
            // would read past the caller's buffer and write garbage to the stick.
            if (usb_msc_write(dev, 0, lba_i, src, 1) != 0) {
                // Failed: RAM still holds the pre-write (on-disk) data, which is
                // correct, so leave TO-RAM untouched. For the demand cache, drop
                // any stale copy of this LBA so we don't serve unwritten data.
                if (g_mode == BLK_MODE_CACHE) {
                    uint32_t idx = (uint32_t)lba_i & g_mask;
                    blk_lk(); if (g_tag[idx] == lba_i) g_tag[idx] = BLK_TAG_INVALID; blk_ul();
                }
                return -1;
            }
            if (g_mode == BLK_MODE_TORAM && lba_i < g_ram_sectors) {
                blk_lk(); memcpy(chunk_sector(lba_i), src, BLK_SECTOR); blk_ul();
            } else if (g_mode == BLK_MODE_CACHE) {
                uint32_t idx = (uint32_t)lba_i & g_mask;
                blk_lk(); memcpy(chunk_sector(idx), src, BLK_SECTOR); g_tag[idx] = lba_i; blk_ul();
            }
        }
        return (int)count;
    }
    return ata_write_sectors_dma(channel, drive, (uint32_t)lba, (uint8_t)count, buf);
}
