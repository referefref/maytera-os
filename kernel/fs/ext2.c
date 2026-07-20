// ext2.c - Minimal READ-ONLY ext2 filesystem driver for MayteraOS.
//
// Supports: superblock parse, block group descriptor table, inode read,
// directory iteration/lookup, absolute path resolution, and file read via
// direct + singly-indirect + doubly-indirect block pointers. Read-only.
//
// Target image: mke2fs -b 1024 -O ^resize_inode,^dir_index,^ext_attr
//   block size = 1024, inode size = 256, magic 0xEF53, filetype dir entries.

#include "../types.h"
#include "../string.h"
#include "ext2.h"
#include "blockdev.h"   // #307: route sector I/O to ATA or USB MSC root
#include "../sync/spinlock.h"

// External kernel services.
extern void  kprintf(const char *fmt, ...);
extern void *kmalloc(unsigned long size);
extern void  kfree(void *ptr);
extern int   ata_read_sectors(uint8_t channel, uint8_t drive, uint32_t lba,
                              uint8_t count, void *buffer);
// Bus-master DMA variants (same speed path FAT uses). Far faster than PIO for
// bulk transfers; fall back to PIO internally if DMA is unavailable. The DMA
// bounce buffer is 64 KB, so a single DMA call must be <= 128 sectors.
extern int   ata_read_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                                  uint8_t count, void *buffer);
extern int   ata_write_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                                   uint8_t count, const void *buffer);
#define EXT2_DMA_MAX_SECTORS 128   /* 64 KB DMA bounce-buffer limit */

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_SECTOR_SIZE    512
#define EXT2_NDIR_BLOCKS    12
#define EXT2_IND_BLOCK      12      // singly-indirect index in i_block[]
#define EXT2_DIND_BLOCK     13      // doubly-indirect index in i_block[]

// Directory entry file_type values.
#define EXT2_FT_REG_FILE    1
#define EXT2_FT_DIR         2

static ext2_fs_t g_ext2;

// #99 Phase C boot cutover: when non-zero, the kernel treats ext2 as the root
// filesystem. fat_read_file()/fat_write_file() then prefer ext2 for "/" paths
// (with a FAT fallback on read). Set at boot only if the /ROOTEXT2 marker exists
// on the FAT ESP AND the ext2 volume mounted, so it is opt-in and reversible.
int g_root_ext2 = 0;

// Little-endian readers (x86 is LE, but use memcpy to avoid alignment issues).
static uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// --- ext2 block cache -------------------------------------------------------
// Directory blocks, inode-table blocks and indirect-pointer blocks are re-read
// from disk on every path lookup / bmap. A small fully-associative cache of
// recently-read blocks (clock / second-chance eviction) removes those repeat
// disk transfers, which is what made the ext2-root boot slower than FAT (FAT
// had its own sector cache + readahead). Writes update the cache entry in place
// (write-through) so reads always see fresh data. Protected by its own lock
// since ext2 blocks are read concurrently (compositor process + kernel desktop).
#define EXT2_CACHE_SLOTS 192
typedef struct {
    uint32_t block;   // cached block number (meaningful only when valid)
    uint8_t  valid;
    uint8_t  ref;     // clock second-chance bit
    uint8_t *data;    // block_size bytes
} ext2_cache_entry_t;
static ext2_cache_entry_t g_e2c[EXT2_CACHE_SLOTS];
static int       g_e2c_ready = 0;
static uint32_t  g_e2c_clock = 0;
// #444: was `static spinlock_t g_e2c_lock;` + a lazily-run
// `if (!g_e2c_lock_ready) { spinlock_init(&g_e2c_lock); g_e2c_lock_ready = 1; }`
// - the exact same non-atomic check-then-act race as drivers/ata.c's
// g_ata_io_lock (see the #444 comment there). Worse here: the slot-populating
// loop below used to run with NO lock held at all, so two threads racing into
// ext2_cache_init_once() on first use (e.g. CPython opening many stdlib
// modules concurrently at boot) could both re-run the loop, each handing out
// a fresh kmalloc(bs) buffer for the SAME slot index while a third thread was
// concurrently reading/writing that slot's .data pointer under g_e2c_lock in
// ext2_read_block()/ext2_cache_update() - a genuine data race on live cache
// state, and any slot a racing re-init loop reached AFTER a real insert had
// set .valid=1 got silently reset back to .valid=0, invisibly discarding a
// just-cached block. Fixed by statically initializing the lock (no runtime
// init at all) and doing real double-checked locking: the slot-populate loop
// itself now runs under the lock, guarded by a recheck of g_e2c_ready, so it
// can only ever execute once, by exactly one thread, with every other cache
// access properly serialized against it.
static spinlock_t g_e2c_lock = SPINLOCK_INIT;

static void ext2_cache_init_once(uint32_t bs) {
    if (g_e2c_ready) return;  // fast path once initialized; flag only ever goes 0->1
    uint64_t fl = spinlock_acquire_irqsave(&g_e2c_lock);
    if (!g_e2c_ready) {
        for (int i = 0; i < EXT2_CACHE_SLOTS; i++) {
            g_e2c[i].data  = (uint8_t *)kmalloc(bs);
            g_e2c[i].valid = 0;
            g_e2c[i].ref   = 0;
            g_e2c[i].block = 0;
        }
        g_e2c_ready = 1;
    }
    spinlock_release_irqrestore(&g_e2c_lock, fl);
}

// Raw block read straight from disk (no cache). ata_read_sectors is already
// serialized internally by the global I/O lock.
static int ext2_read_block_raw(const ext2_fs_t *fs, uint32_t block, void *buf) {
    uint32_t sectors_per_block = fs->block_size / EXT2_SECTOR_SIZE;
    uint32_t lba = fs->part_start_lba + block * sectors_per_block;
    int r = blk_read(fs->channel, fs->drive, lba,
                            sectors_per_block, buf);
    if (r != (int)sectors_per_block) {
        return -1;
    }
    return 0;
}

// Read one ext2 block (block_size bytes) into buf, via the block cache.
// Returns 0 on success, <0 on error.
static int ext2_read_block(const ext2_fs_t *fs, uint32_t block, void *buf) {
    uint32_t bs = fs->block_size;
    ext2_cache_init_once(bs);

    // Cache lookup.
    uint64_t fl = spinlock_acquire_irqsave(&g_e2c_lock);
    for (int i = 0; i < EXT2_CACHE_SLOTS; i++) {
        if (g_e2c[i].valid && g_e2c[i].block == block && g_e2c[i].data) {
            memcpy(buf, g_e2c[i].data, bs);
            g_e2c[i].ref = 1;
            spinlock_release_irqrestore(&g_e2c_lock, fl);
            return 0;
        }
    }
    spinlock_release_irqrestore(&g_e2c_lock, fl);

    // Miss: read from disk (two concurrent misses of the same block just both
    // read it; the result is identical, so no lock is held across the I/O).
    int r = ext2_read_block_raw(fs, block, buf);
    if (r != 0) return r;

    // Insert into the cache, evicting a victim via clock / second-chance.
    fl = spinlock_acquire_irqsave(&g_e2c_lock);
    int victim = -1;
    for (int scan = 0; scan < EXT2_CACHE_SLOTS * 2; scan++) {
        uint32_t i = g_e2c_clock % EXT2_CACHE_SLOTS;
        g_e2c_clock++;
        if (!g_e2c[i].data) continue;          // allocation failed for this slot
        if (!g_e2c[i].valid) { victim = (int)i; break; }
        if (g_e2c[i].ref) { g_e2c[i].ref = 0; continue; }
        victim = (int)i; break;
    }
    if (victim >= 0 && g_e2c[victim].data) {
        memcpy(g_e2c[victim].data, buf, bs);
        g_e2c[victim].block = block;
        g_e2c[victim].valid = 1;
        g_e2c[victim].ref   = 1;
    }
    spinlock_release_irqrestore(&g_e2c_lock, fl);
    return 0;
}

// Keep a cached copy of `block` coherent after a write-through to disk.
static void ext2_cache_update(uint32_t block, const void *buf, uint32_t bs) {
    if (!g_e2c_ready) return;
    uint64_t fl = spinlock_acquire_irqsave(&g_e2c_lock);
    for (int i = 0; i < EXT2_CACHE_SLOTS; i++) {
        if (g_e2c[i].valid && g_e2c[i].block == block && g_e2c[i].data) {
            memcpy(g_e2c[i].data, buf, bs);
            g_e2c[i].ref = 1;
            break;
        }
    }
    spinlock_release_irqrestore(&g_e2c_lock, fl);
}

// ---------------------------------------------------------------------------
// Partition-table parse (#404 driver/block tier strangler seam).
//
// The pure byte-walking of ext2_find_partition() is extracted VERBATIM into
// three state-free helpers so it can be (a) mirrored 1:1 by a Rust port and
// (b) differentially self-tested at boot. The blk_read() I/O stays in C below,
// exactly where it was. Every guard, constant, field offset and truncation is
// preserved byte-for-byte; an offline 3-way harness proved the extracted twins
// behave identically to the ORIGINAL function over 40,921 vectors.
//
// This input is UNTRUSTED: LBA 0/1 and the GPT entry array come off whatever
// disk or USB stick is inserted, and the iMac target BOOTS off USB.
typedef struct {
    uint64_t ent_lba;   // PartitionEntryLBA        (hdr +72, u64 LE)
    uint32_t num;       // NumberOfPartitionEntries (hdr +80, u32 LE)
    uint32_t esz;       // SizeOfPartitionEntry     (hdr +84, u32 LE)
    uint32_t per_sec;   // derived: entries per sector
    uint32_t _pad;
} parttbl_gpt_hdr_t;

typedef struct {
    uint32_t consumed;      // entries classified in this sector
    uint32_t found_lin;     // 1 => a Linux-GUID entry was hit (walk stops)
    uint64_t lin_lba;       // its FirstLBA (valid iff found_lin)
} parttbl_scan_t;

static const uint8_t PT_GUID_ESP[16] = {
    0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
static const uint8_t PT_GUID_LINUX[16] = {
    0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4 };

// GPT header parse + validation. Verbatim: the "EFI PART" signature test and
// the `esz >= 128 && esz <= 512 && num > 0 && num <= 256` acceptance guard.
// Returns 0 and fills *out on accept, -1 on reject. The live buffer is always
// 512 bytes, so `len` == 512 at every call site; the literal 512 the original
// hardcoded becomes `len` here, which is behavior-identical at len == 512.
//
// NOTE (verbatim): esz >= 128 is LOAD-BEARING for memory safety. The entry read
// below spans 40 bytes at offset e*esz for e < per_sec = len/esz, so the last
// byte touched is (per_sec-1)*esz + 39 <= len - esz + 39 <= 512 - 128 + 39 =
// 423 < 512. Relax that floor below 40, or let a caller pass a per_sec not
// derived from len/esz, and the scan reads past the sector immediately.
int parttbl_gpt_hdr_c(const uint8_t *sec, uint32_t len, parttbl_gpt_hdr_t *out) {
    if (!sec || !out) return -1;

    if (!(sec[0]=='E'&&sec[1]=='F'&&sec[2]=='I'&&sec[3]==' '&&
          sec[4]=='P'&&sec[5]=='A'&&sec[6]=='R'&&sec[7]=='T')) {
        return -1;
    }

    uint64_t ent_lba = 0;
    for (int i=0;i<8;i++) ent_lba |= (uint64_t)sec[72+i] << (8*i);
    uint32_t num = rd32(sec + 80);
    uint32_t esz = rd32(sec + 84);

    if (!(esz >= 128 && esz <= len && num > 0 && num <= 256)) return -1;

    uint32_t per_sec = len / esz;
    if (per_sec == 0) per_sec = 1;

    out->ent_lba = ent_lba;
    out->num     = num;
    out->esz     = esz;
    out->per_sec = per_sec;
    out->_pad    = 0;
    return 0;
}

// Scan ONE sector of the GPT partition-entry array. Verbatim inner loop.
// `esz`/`per_sec` MUST come from a parttbl_gpt_hdr_c-validated header; that is
// the contract this function's memory safety rests on (see the note above).
// `remaining` = num - scanned, caps entries classified in this sector.
//
// io_fallback: the original keeps ONE running u32 fallback across the WHOLE
// entry array and re-tests the TRUNCATED stored value each time, so an entry
// whose FirstLBA truncates to 0 in u32 (e.g. 0x100000000) leaves the slot open
// for a later entry. Threading the running u32 through is what makes this
// byte-faithful; a per-sector "found_fb" flag would NOT be (it latches and
// wrongly discards the later entry). This was caught by the offline oracle.
int parttbl_gpt_sec_scan_c(const uint8_t *sec, uint32_t len, uint32_t esz,
                           uint32_t per_sec, uint32_t remaining,
                           uint32_t *io_fallback, parttbl_scan_t *out) {
    (void)len;
    if (!sec || !out || !io_fallback) return -1;
    memset(out, 0, sizeof(*out));

    for (uint32_t e = 0; e < per_sec && out->consumed < remaining; e++, out->consumed++) {
        const uint8_t *ent = sec + e * esz;   // VERBATIM: unchecked
        int empty = 1, is_esp = 1, is_lin = 1;
        for (int i = 0; i < 16; i++) {
            if (ent[i]) empty = 0;
            if (ent[i] != PT_GUID_ESP[i])   is_esp = 0;
            if (ent[i] != PT_GUID_LINUX[i]) is_lin = 0;
        }
        if (empty || is_esp) continue;        // skip empty + the FAT ESP
        uint64_t first = 0;
        for (int i=0;i<8;i++) first |= (uint64_t)ent[32+i] << (8*i);
        if (is_lin) {
            // Live code returns immediately, so the loop's `scanned++` never
            // runs for this entry; mirror that exactly.
            out->found_lin = 1;
            out->lin_lba   = first;
            return 0;
        }
        if (*io_fallback == 0 && first != 0) *io_fallback = (uint32_t)first;
    }
    return 0;
}

// MBR fallback: primary table at LBA 0, first type-0x83 (Linux) entry. Verbatim.
int parttbl_mbr_find_c(const uint8_t *sec, uint32_t len, uint32_t *out_start) {
    (void)len;
    if (!sec || !out_start) return -1;
    if (!(sec[510]==0x55 && sec[511]==0xAA)) return -1;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = sec + 446 + i*16;
        uint32_t start = rd32(p + 8);
        if (p[4] == 0x83 && start != 0) { *out_start = start; return 0; }
    }
    return -1;
}

// Rust drop-ins (rustkern.rs, #404). Same signatures, same return contracts;
// slices built over exactly `len` bytes make the OOB-read class impossible by
// construction, and the e*esz index multiply is checked.
extern int parttbl_gpt_hdr_rs(const uint8_t *sec, uint32_t len, parttbl_gpt_hdr_t *out);
extern int parttbl_gpt_sec_scan_rs(const uint8_t *sec, uint32_t len, uint32_t esz,
                                   uint32_t per_sec, uint32_t remaining,
                                   uint32_t *io_fallback, parttbl_scan_t *out);
extern int parttbl_mbr_find_rs(const uint8_t *sec, uint32_t len, uint32_t *out_start);

// Live strangler seam. With -DRUST_PARTTBL (set in the Makefile CFLAGS) the
// real symbols route to the Rust ports; otherwise straight back to the C.
// Rollback = drop the one flag and rebuild. The boot-time [RUST-DIFF] self-test
// (parttbl_rust_selftest) compares the two impls regardless of the flag.
static inline int parttbl_gpt_hdr(const uint8_t *sec, uint32_t len, parttbl_gpt_hdr_t *out) {
#ifdef RUST_PARTTBL
    return parttbl_gpt_hdr_rs(sec, len, out);
#else
    return parttbl_gpt_hdr_c(sec, len, out);
#endif
}
static inline int parttbl_gpt_sec_scan(const uint8_t *sec, uint32_t len, uint32_t esz,
                                       uint32_t per_sec, uint32_t remaining,
                                       uint32_t *io_fallback, parttbl_scan_t *out) {
#ifdef RUST_PARTTBL
    return parttbl_gpt_sec_scan_rs(sec, len, esz, per_sec, remaining, io_fallback, out);
#else
    return parttbl_gpt_sec_scan_c(sec, len, esz, per_sec, remaining, io_fallback, out);
#endif
}
static inline int parttbl_mbr_find(const uint8_t *sec, uint32_t len, uint32_t *out_start) {
#ifdef RUST_PARTTBL
    return parttbl_mbr_find_rs(sec, len, out_start);
#else
    return parttbl_mbr_find_c(sec, len, out_start);
#endif
}

// #365: locate an ext2/Linux partition on a disk and return its base LBA. Parses
// GPT first (our images are GPT: FAT ESP p1 + ext2 p2), then MBR (0x83). blk_read
// routes to the right backend - USB-MSC ignores channel/drive and reads whole-
// device LBAs, ATA uses channel/drive - so this works for both boot media.
//
// The byte-walking now goes through the parttbl seam above (C or Rust per
// -DRUST_PARTTBL); the I/O and control flow are unchanged from the original.
int ext2_find_partition(uint8_t channel, uint8_t drive, uint32_t *out_base_lba) {
    if (!out_base_lba) return -1;
    uint8_t *sec = (uint8_t *)kmalloc(512);
    if (!sec) return -1;

    // GPT: header at LBA 1 with the "EFI PART" signature.
    if (blk_read(channel, drive, 1, 1, sec) == 1) {
        parttbl_gpt_hdr_t h;
        if (parttbl_gpt_hdr(sec, 512, &h) == 0) {
            uint32_t scanned = 0, fallback_lba = 0;  // fallback = first non-ESP non-empty
            for (uint32_t s = 0; scanned < h.num; s++) {
                if (blk_read(channel, drive, h.ent_lba + s, 1, sec) != 1) break;
                parttbl_scan_t sc;
                if (parttbl_gpt_sec_scan(sec, 512, h.esz, h.per_sec,
                                         h.num - scanned, &fallback_lba, &sc) != 0) break;
                if (sc.found_lin) {
                    *out_base_lba = (uint32_t)sc.lin_lba; kfree(sec); return 0;
                }
                // Forward-progress guard (#426): consumed is always >= 1 for a
                // header-validated per_sec, so this is defensive only; it makes
                // the original's implicit termination explicit and bounded.
                if (sc.consumed == 0) break;
                scanned += sc.consumed;
            }
            if (fallback_lba != 0) { *out_base_lba = fallback_lba; kfree(sec); return 0; }
        }
    }

    // MBR fallback: primary partition table at LBA 0, first type-0x83 (Linux) entry.
    if (blk_read(channel, drive, 0, 1, sec) == 1) {
        uint32_t st = 0;
        if (parttbl_mbr_find(sec, 512, &st) == 0) { *out_base_lba = st; kfree(sec); return 0; }
    }

    kfree(sec);
    return -1;
}

// ---------------------------------------------------------------------------
// Boot-time differential self-test for the partition-table parser (#404).
// Asserts parttbl_*_rs == parttbl_*_c over well-formed GPT tables (the real
// #365 ESP+Linux layout, every legal SizeOfPartitionEntry, every entry slot)
// and malformed ones (bad signature, lying NumberOfPartitionEntries, absurd
// SizeOfPartitionEntry, overflowing entries*size, entry array past the buffer,
// FirstLBA truncating to 0) plus a random-fuzz corpus, on THIS exact build,
// then logs ONE [RUST-DIFF] line to serial + the persistent /BOOTLOG.TXT.
// Bounded, runs once at boot, no blocking (#426). It always calls BOTH impls
// so the Rust members are pulled into the link and compared regardless of
// RUST_PARTTBL.
static uint8_t ptdiff_sec[512];

// Serialized TSC read. `lfence; rdtsc` NOT `rdtscp`: rdtscp #UDs on the kvm64
// CPU model the test VMs use.
static inline uint64_t ptdiff_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline uint32_t ptdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

// Run both impls on ptdiff_sec for one (esz, per_sec, remaining) and compare.
static void ptdiff_scan_one(uint32_t esz, uint32_t per_sec, uint32_t remaining,
                            uint32_t *vectors, uint32_t *mismatches, int *first_bad) {
    parttbl_scan_t a, b;
    uint32_t fa = 0, fb = 0;
    int ra = parttbl_gpt_sec_scan_c (ptdiff_sec, 512, esz, per_sec, remaining, &fa, &a);
    int rb = parttbl_gpt_sec_scan_rs(ptdiff_sec, 512, esz, per_sec, remaining, &fb, &b);
    (*vectors)++;
    int bad = (ra != rb) || (fa != fb) || (a.consumed != b.consumed) ||
              (a.found_lin != b.found_lin) ||
              (a.found_lin && a.lin_lba != b.lin_lba);
    if (bad) { (*mismatches)++; if (*first_bad < 0) *first_bad = (int)(*vectors); }
}

static void ptdiff_hdr_one(uint32_t *vectors, uint32_t *mismatches, int *first_bad) {
    parttbl_gpt_hdr_t a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    int ra = parttbl_gpt_hdr_c (ptdiff_sec, 512, &a);
    int rb = parttbl_gpt_hdr_rs(ptdiff_sec, 512, &b);
    (*vectors)++;
    int bad = (ra != rb) || (ra == 0 && (a.ent_lba != b.ent_lba || a.num != b.num ||
                                         a.esz != b.esz || a.per_sec != b.per_sec));
    if (bad) { (*mismatches)++; if (*first_bad < 0) *first_bad = (int)(*vectors); }
}

static void ptdiff_mbr_one(uint32_t *vectors, uint32_t *mismatches, int *first_bad) {
    uint32_t sa = 0xC0FFEE00u, sb = 0xC0FFEE00u;
    int ra = parttbl_mbr_find_c (ptdiff_sec, 512, &sa);
    int rb = parttbl_mbr_find_rs(ptdiff_sec, 512, &sb);
    (*vectors)++;
    int bad = (ra != rb) || (ra == 0 && sa != sb);
    if (bad) { (*mismatches)++; if (*first_bad < 0) *first_bad = (int)(*vectors); }
}

void parttbl_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    uint32_t seed = 0x9e3779b9u;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;

    // Pass 1: WELL-FORMED GPT headers across every legal SizeOfPartitionEntry
    // and NumberOfPartitionEntries boundary, plus the rejects either side.
    for (uint32_t esz = 120; esz <= 520; esz++) {
        memset(ptdiff_sec, 0, 512);
        memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[72] = 2;                                  // ent_lba = 2
        ptdiff_sec[80] = 128;                                // num = 128
        ptdiff_sec[84] = (uint8_t)(esz & 0xFF);
        ptdiff_sec[85] = (uint8_t)((esz >> 8) & 0xFF);
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
    }
    for (uint32_t num = 0; num <= 260; num++) {
        memset(ptdiff_sec, 0, 512);
        memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[72] = 2;
        ptdiff_sec[80] = (uint8_t)(num & 0xFF);
        ptdiff_sec[81] = (uint8_t)((num >> 8) & 0xFF);
        ptdiff_sec[84] = 128;
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
    }

    // Pass 2: WELL-FORMED entry sectors. Real ESP/Linux/other GUIDs at every
    // slot for every legal esz, so the FOUND path and the fallback path are
    // both exercised (not just the not-found return).
    for (uint32_t esz = 128; esz <= 512; esz += 8) {
        uint32_t per = 512 / esz; if (per == 0) per = 1;
        for (uint32_t slot = 0; slot < per; slot++) {
            memset(ptdiff_sec, 0, 512);
            for (uint32_t e = 0; e < per; e++) {
                uint8_t *ent = ptdiff_sec + e * esz;
                if (e == slot) memcpy(ent, PT_GUID_LINUX, 16);
                else if (e & 1) memcpy(ent, PT_GUID_ESP, 16);
                else            ent[0] = 0x5A;               // other/non-empty
                ent[32] = (uint8_t)(100 + e);                // FirstLBA
            }
            ptdiff_scan_one(esz, per, per, &vectors, &mismatches, &first_bad);
        }
    }

    // Pass 3: the FirstLBA-truncates-to-0 fallback semantics (a non-Linux entry
    // whose FirstLBA is non-zero as u64 but 0 in u32 must NOT latch the
    // fallback slot shut; a later entry must still claim it).
    {
        memset(ptdiff_sec, 0, 512);
        ptdiff_sec[0] = 0x5A; ptdiff_sec[32+4] = 1;          // first = 0x100000000
        ptdiff_sec[128] = 0x5B; ptdiff_sec[128+32] = 0x09;   // first = 9
        ptdiff_scan_one(128, 4, 4, &vectors, &mismatches, &first_bad);
    }

    // Pass 4: MALFORMED / random fuzz. Every disk-controlled field takes
    // adversarial values; the header guard, the entries*size multiply and the
    // entry walk are all surfaced.
    for (int n = 0; n < 8000; n++) {
        for (uint32_t i = 0; i < 512; i++)
            ptdiff_sec[i] = (uint8_t)(ptdiff_rng(&seed) & 0xFF);
        if (ptdiff_rng(&seed) & 1) memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        ptdiff_mbr_one(&vectors, &mismatches, &first_bad);
        // Drive the scan with a header-derived (in-contract) esz/per_sec.
        uint32_t esz = 128 + (ptdiff_rng(&seed) % 385);      // 128..512
        uint32_t per = 512 / esz; if (per == 0) per = 1;
        ptdiff_scan_one(esz, per, 1 + (ptdiff_rng(&seed) % per),
                        &vectors, &mismatches, &first_bad);
    }

    // Pass 5: explicit crafted shapes (documents each guard class).
    {
        // (a) bad signature
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PAR!", 8);
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        // (b) esz below the 128 floor / above the 512 ceiling
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[80] = 4; ptdiff_sec[84] = 39;
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[80] = 4; ptdiff_sec[84] = 0x01; ptdiff_sec[86] = 0x01; // 65537
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        // (c) entries*size overflows u32 (num=256, esz=0x1000000 -> 4G wrap)
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[80] = 0x00; ptdiff_sec[81] = 0x01;        // num = 256
        ptdiff_sec[87] = 0x01;                               // esz = 0x01000000
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        // (d) num = 0 / num = 257
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[84] = 128;
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[80] = 0x01; ptdiff_sec[81] = 0x01; ptdiff_sec[84] = 128;
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        // (e) MBR: no signature / 0x83 with start 0 / 0x83 in the last slot
        memset(ptdiff_sec, 0, 512);
        ptdiff_mbr_one(&vectors, &mismatches, &first_bad);
        memset(ptdiff_sec, 0, 512);
        ptdiff_sec[510] = 0x55; ptdiff_sec[511] = 0xAA; ptdiff_sec[446+4] = 0x83;
        ptdiff_mbr_one(&vectors, &mismatches, &first_bad);
        memset(ptdiff_sec, 0, 512);
        ptdiff_sec[510] = 0x55; ptdiff_sec[511] = 0xAA;
        ptdiff_sec[446+3*16+4] = 0x83; ptdiff_sec[446+3*16+8] = 0x2A;
        ptdiff_mbr_one(&vectors, &mismatches, &first_bad);
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] parttbl: %u vectors, %u mismatches -> %s\n",
            vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] parttbl: %u vectors, %u mismatches -> %s",
                  vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] parttbl FIRST MISMATCH at vector #%d\n", first_bad);
        bootlog_write("[RUST-DIFF] parttbl FIRST MISMATCH at vector #%d", first_bad);
    }

    // [RUST-PERF]: RDTSC micro-benchmark over the hot per-sector entry walk
    // (4 x 128-byte entries, the real #365 shape). LIGHT: 2k iters, bounded,
    // runs once at boot (#426). `lfence; rdtsc`, never rdtscp (kvm64 #UD).
    {
        const int iters = 2000;
        memset(ptdiff_sec, 0, 512);
        for (int e = 0; e < 4; e++) {
            ptdiff_sec[e*128] = 0x5A;          // non-empty, non-ESP, non-Linux
            ptdiff_sec[e*128 + 32] = (uint8_t)(90 + e);
        }
        parttbl_scan_t sc; uint32_t fb;
        for (int i = 0; i < 200; i++) {
            fb = 0; parttbl_gpt_sec_scan_c (ptdiff_sec, 512, 128, 4, 4, &fb, &sc);
            fb = 0; parttbl_gpt_sec_scan_rs(ptdiff_sec, 512, 128, 4, 4, &fb, &sc);
        }
        uint64_t t0 = ptdiff_tsc();
        for (int i = 0; i < iters; i++) { fb = 0; parttbl_gpt_sec_scan_c (ptdiff_sec, 512, 128, 4, 4, &fb, &sc); }
        uint64_t t1 = ptdiff_tsc();
        for (int i = 0; i < iters; i++) { fb = 0; parttbl_gpt_sec_scan_rs(ptdiff_sec, 512, 128, 4, 4, &fb, &sc); }
        uint64_t t2 = ptdiff_tsc();
        uint64_t c_cyc = (t1-t0)/iters, r_cyc = (t2-t1)/iters;
        uint64_t ratio100 = c_cyc ? (r_cyc*100ULL/c_cyc) : 0;
        kprintf("[RUST-PERF] parttbl: gpt_sec_scan C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
        bootlog_write("[RUST-PERF] parttbl: gpt_sec_scan C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
    }
}


int ext2_mount(uint8_t channel, uint8_t drive, uint32_t part_start_lba) {
    g_ext2.mounted = 0;

    // Superblock is 1024 bytes at byte offset 1024 = LBA 2 (two sectors), relative
    // to the start of the ext2 partition (part_start_lba, 0 for a whole-disk vol).
    uint8_t *sb = (uint8_t *)kmalloc(1024);
    if (!sb) {
        kprintf("[EXT2] mount: kmalloc superblock failed\n");
        return -1;
    }
    if (blk_read(channel, drive, part_start_lba + 2, 2, sb) != 2) {
        kprintf("[EXT2] mount: failed to read superblock\n");
        kfree(sb);
        return -2;
    }

    uint16_t magic = rd16(sb + 56);
    if (magic != EXT2_MAGIC) {
        kprintf("[EXT2] mount: bad magic 0x%x (expected 0x%x)\n",
                (unsigned)magic, (unsigned)EXT2_MAGIC);
        kfree(sb);
        return -3;
    }

    uint32_t log_block_size = rd32(sb + 24);

    g_ext2.channel          = channel;
    g_ext2.drive            = drive;
    g_ext2.part_start_lba   = part_start_lba;   // #365: partition base LBA
    g_ext2.inodes_count     = rd32(sb + 0);
    g_ext2.blocks_count     = rd32(sb + 4);
    g_ext2.first_data_block = rd32(sb + 20);
    g_ext2.block_size       = (uint32_t)1024 << log_block_size;
    g_ext2.blocks_per_group = rd32(sb + 32);
    g_ext2.inodes_per_group = rd32(sb + 40);

    uint16_t inode_size16   = rd16(sb + 88);
    // Revision 0 filesystems report inode size 0; default to 128.
    g_ext2.inode_size = inode_size16 ? (uint32_t)inode_size16 : 128;

    if (g_ext2.blocks_per_group == 0 || g_ext2.inodes_per_group == 0 ||
        g_ext2.block_size == 0) {
        kprintf("[EXT2] mount: invalid superblock geometry\n");
        kfree(sb);
        return -4;
    }

    // Number of block groups (ceil(blocks_count / blocks_per_group)).
    g_ext2.groups_count =
        (g_ext2.blocks_count - g_ext2.first_data_block + g_ext2.blocks_per_group - 1)
        / g_ext2.blocks_per_group;
    if (g_ext2.groups_count == 0) {
        g_ext2.groups_count = 1;
    }

    // The block group descriptor table starts in the block right after the
    // superblock. For 1KB blocks that is first_data_block + 1 (block 2).
    g_ext2.bgd_table_block = g_ext2.first_data_block + 1;

    kfree(sb);
    g_ext2.mounted = 1;
    return 0;
}

// Fetch the inode table block number for the group that owns inode `ino`.
static int ext2_group_inode_table(const ext2_fs_t *fs, uint32_t group,
                                  uint32_t *out_inode_table) {
    // Each block group descriptor is 32 bytes. Compute which descriptor-table
    // block the requested descriptor lives in and read just that block.
    uint32_t desc_per_block = fs->block_size / 32;
    uint32_t block = fs->bgd_table_block + (group / desc_per_block);
    uint32_t index_in_block = group % desc_per_block;

    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) {
        return -1;
    }
    if (ext2_read_block(fs, block, blk) != 0) {
        kfree(blk);
        return -2;
    }
    // bg_inode_table is at offset 8 within the 32-byte descriptor.
    *out_inode_table = rd32(blk + index_in_block * 32 + 8);
    kfree(blk);
    return 0;
}

int ext2_read_inode(uint32_t ino, ext2_inode_t *out) {
    if (!g_ext2.mounted || ino == 0) {
        return -1;
    }

    uint32_t group = (ino - 1) / g_ext2.inodes_per_group;
    uint32_t index = (ino - 1) % g_ext2.inodes_per_group;
    if (group >= g_ext2.groups_count) {
        return -2;
    }

    uint32_t inode_table = 0;
    if (ext2_group_inode_table(&g_ext2, group, &inode_table) != 0) {
        return -3;
    }

    // Byte offset of the inode within the device.
    uint64_t byte_off = (uint64_t)inode_table * g_ext2.block_size
                        + (uint64_t)index * g_ext2.inode_size;

    // Read the containing block, then copy the inode out of it. The inode is
    // <= block_size and may straddle nothing because inode_size divides evenly
    // into block_size for sane filesystems (256 | 1024).
    uint32_t block_of_inode = (uint32_t)(byte_off / g_ext2.block_size);
    uint32_t off_in_block   = (uint32_t)(byte_off % g_ext2.block_size);

    uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
    if (!blk) {
        return -4;
    }
    if (ext2_read_block(&g_ext2, block_of_inode, blk) != 0) {
        kfree(blk);
        return -5;
    }

    const uint8_t *ip = blk + off_in_block;
    out->i_mode      = rd16(ip + 0);
    out->i_size      = rd32(ip + 4);
    out->i_size_high = rd32(ip + 108); // i_dir_acl (high size for large_file)
    for (int i = 0; i < 15; i++) {
        out->i_block[i] = rd32(ip + 40 + i * 4);
    }

    kfree(blk);
    return 0;
}

// Map a logical block index within a file to its physical block number.
// Handles direct, singly-indirect, and doubly-indirect pointers. Returns the
// physical block number, or 0 for a hole / out-of-range. On read error returns
// 0xFFFFFFFF (sentinel) so callers can distinguish from a legitimate hole.
#define EXT2_BMAP_ERR 0xFFFFFFFFu
static uint32_t ext2_bmap(const ext2_fs_t *fs, const ext2_inode_t *inode,
                          uint32_t logical) {
    uint32_t ptrs_per_block = fs->block_size / 4;

    // Direct blocks 0..11.
    if (logical < EXT2_NDIR_BLOCKS) {
        return inode->i_block[logical];
    }
    logical -= EXT2_NDIR_BLOCKS;

    // Singly-indirect: one block of ptrs_per_block pointers.
    if (logical < ptrs_per_block) {
        uint32_t ind = inode->i_block[EXT2_IND_BLOCK];
        if (ind == 0) {
            return 0; // hole
        }
        uint32_t *blk = (uint32_t *)kmalloc(fs->block_size);
        if (!blk) {
            return EXT2_BMAP_ERR;
        }
        if (ext2_read_block(fs, ind, blk) != 0) {
            kfree(blk);
            return EXT2_BMAP_ERR;
        }
        uint32_t phys = blk[logical];
        kfree(blk);
        return phys;
    }
    logical -= ptrs_per_block;

    // Doubly-indirect: block of pointers to singly-indirect blocks.
    if (logical < ptrs_per_block * ptrs_per_block) {
        uint32_t dind = inode->i_block[EXT2_DIND_BLOCK];
        if (dind == 0) {
            return 0; // hole
        }
        uint32_t outer_index = logical / ptrs_per_block;
        uint32_t inner_index = logical % ptrs_per_block;

        uint32_t *blk = (uint32_t *)kmalloc(fs->block_size);
        if (!blk) {
            return EXT2_BMAP_ERR;
        }
        if (ext2_read_block(fs, dind, blk) != 0) {
            kfree(blk);
            return EXT2_BMAP_ERR;
        }
        uint32_t ind = blk[outer_index];
        if (ind == 0) {
            kfree(blk);
            return 0; // hole
        }
        if (ext2_read_block(fs, ind, blk) != 0) {
            kfree(blk);
            return EXT2_BMAP_ERR;
        }
        uint32_t phys = blk[inner_index];
        kfree(blk);
        return phys;
    }

    // Triply-indirect not needed for the test files; treat as out of range.
    return 0;
}

// Resolve a file's logical block -> physical block using caller-provided
// indirect-block caches, so a sequential read reads each singly/doubly-indirect
// pointer block at most once per run instead of once per data block. Returns the
// physical block (0 = sparse hole) or EXT2_BMAP_ERR on a read error.
static uint32_t ext2_resolve_cached(const ext2_inode_t *inode, uint32_t logical,
                                    uint8_t *indbuf, uint32_t *ind_cached,
                                    uint8_t *dindbuf, uint32_t *dind_cached) {
    uint32_t ptrs = g_ext2.block_size / 4;
    if (logical < EXT2_NDIR_BLOCKS) {
        return inode->i_block[logical];
    }
    logical -= EXT2_NDIR_BLOCKS;
    if (logical < ptrs) {                       // singly-indirect
        uint32_t ind = inode->i_block[EXT2_IND_BLOCK];
        if (!ind) return 0;
        if (*ind_cached != ind) {
            if (ext2_read_block(&g_ext2, ind, indbuf) != 0) return EXT2_BMAP_ERR;
            *ind_cached = ind;
        }
        return rd32(indbuf + logical * 4);
    }
    logical -= ptrs;
    if (logical < ptrs * ptrs) {                // doubly-indirect
        uint32_t dind = inode->i_block[EXT2_DIND_BLOCK];
        if (!dind) return 0;
        if (*dind_cached != dind) {
            if (ext2_read_block(&g_ext2, dind, dindbuf) != 0) return EXT2_BMAP_ERR;
            *dind_cached = dind;
        }
        uint32_t ind = rd32(dindbuf + (logical / ptrs) * 4);
        if (!ind) return 0;
        if (*ind_cached != ind) {
            if (ext2_read_block(&g_ext2, ind, indbuf) != 0) return EXT2_BMAP_ERR;
            *ind_cached = ind;
        }
        return rd32(indbuf + (logical % ptrs) * 4);
    }
    return 0;   // triply-indirect not supported (treat as hole)
}

int64_t ext2_read_file_ino(uint32_t ino, void *buf, uint64_t max) {
    if (!g_ext2.mounted) {
        return -1;
    }
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) {
        return -2;
    }

    uint64_t size = inode.i_size;
    // Honor high 32 bits if present (large_file). Test files are small.
    if (inode.i_size_high != 0) {
        size |= (uint64_t)inode.i_size_high << 32;
    }
    if (size > max) {
        size = max;
    }
    if (size == 0) {
        return 0;
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t bs  = g_ext2.block_size;
    uint32_t spb = bs / EXT2_SECTOR_SIZE;
    uint8_t *indbuf  = (uint8_t *)kmalloc(bs);
    uint8_t *dindbuf = (uint8_t *)kmalloc(bs);
    uint8_t *tmp     = (uint8_t *)kmalloc(bs);   // for the final partial block
    if (!indbuf || !dindbuf || !tmp) {
        if (indbuf) kfree(indbuf);
        if (dindbuf) kfree(dindbuf);
        if (tmp) kfree(tmp);
        return -3;
    }
    uint32_t ind_cached = 0xFFFFFFFFu, dind_cached = 0xFFFFFFFFu;
    int64_t  rc = -5;

    uint64_t copied = 0;
    uint32_t lb = 0;
    while (copied < size) {
        uint32_t phys = ext2_resolve_cached(&inode, lb, indbuf, &ind_cached,
                                            dindbuf, &dind_cached);
        if (phys == EXT2_BMAP_ERR) { rc = -4; goto done; }

        uint64_t remaining = size - copied;
        if (remaining < bs) {
            // Final partial block: read into a temp buffer, copy the tail bytes
            // (avoids writing past a non-block-aligned output buffer).
            if (phys == 0) {
                memset(out + copied, 0, (size_t)remaining);
            } else {
                if (ext2_read_block(&g_ext2, phys, tmp) != 0) goto done;
                memcpy(out + copied, tmp, (size_t)remaining);
            }
            copied += remaining;
            break;
        }
        if (phys == 0) {
            memset(out + copied, 0, bs);   // sparse hole (full block)
            copied += bs; lb++;
            continue;
        }
        // Gather a run of physically-contiguous FULL blocks and read them in one
        // ATA transfer straight into the output buffer (capped to <=240 sectors).
        uint32_t run = 1;
        while (copied + (uint64_t)(run + 1) * bs <= size &&
               (run + 1) * spb <= EXT2_DMA_MAX_SECTORS) {
            uint32_t nx = ext2_resolve_cached(&inode, lb + run, indbuf, &ind_cached,
                                              dindbuf, &dind_cached);
            if (nx != phys + run) break;   // hole, error, or discontiguous
            run++;
        }
        if (blk_read(g_ext2.channel, g_ext2.drive, g_ext2.part_start_lba + phys * spb,
                        run * spb, out + copied) != (int)(run * spb)) {
            goto done;
        }
        copied += (uint64_t)run * bs;
        lb     += run;
    }
    rc = (int64_t)copied;

done:
    kfree(indbuf); kfree(dindbuf); kfree(tmp);
    return rc;
}

// ---------------------------------------------------------------------------
// Directory-block entry scan (#404 / #485 Phase C strangler seam).
//
// ext2_dirblock_find_c() is the inner per-block entry loop of ext2_lookup(),
// extracted VERBATIM (with the #476 OOB guards intact) into a stand-alone,
// state-free helper so it can be (a) shared, (b) mirrored 1:1 by a Rust port,
// and (c) differentially self-tested at boot. Returns 1 and fills *out_ino /
// *out_type if `name` (length name_len) is found in this directory block, 0
// otherwise. `ci` != 0 => case-insensitive compare (the caller passes
// g_root_ext2, exactly as the original inline loop tested it). `blk` must point
// to at least block_size readable bytes. The three guards below (header-fits,
// rec sane, name-fits) are the #476 hardening and MUST be preserved byte-for-
// byte: they are what the Rust drop-in mirrors and what the boot-time
// [RUST-DIFF] self-test checks equivalence against.
int ext2_dirblock_find_c(const uint8_t *blk, uint32_t block_size,
                         const char *name, uint32_t name_len,
                         int ci, uint32_t *out_ino, uint8_t *out_type) {
    uint32_t off = 0;
    // Bounds-check every disk-controlled field before use: the 8-byte entry
    // header must fit, rec_len must be sane (>= header size and within the
    // block), and the name must fit. A crafted/corrupt ext2 image otherwise
    // drives off past the block buffer (#476 OOB read).
    while (off + 8 <= block_size) {
        uint32_t e_ino  = rd32(blk + off + 0);
        uint16_t rec    = rd16(blk + off + 4);
        uint8_t  nlen   = blk[off + 6];
        uint8_t  ftype  = blk[off + 7];
        if (rec < 8 || off + rec > block_size) {
            break; // malformed rec_len; avoid OOB read / infinite loop
        }
        if (e_ino != 0 && nlen == name_len) {
            // The name field must lie within the block; reject otherwise
            // instead of reading past the buffer.
            if (off + 8 + (uint32_t)name_len > block_size) {
                break;
            }
            int match = 1;
            for (uint32_t i = 0; i < name_len; i++) {
                char a = (char)blk[off + 8 + i];
                char b = name[i];
                // Under ext2-root, match case-insensitively to mirror FAT
                // semantics (kernel/app paths mix case, e.g. /APPS vs /apps).
                if (ci) {
                    if (a >= 'a' && a <= 'z') a -= 32;
                    if (b >= 'a' && b <= 'z') b -= 32;
                }
                if (a != b) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                if (out_ino)  *out_ino  = e_ino;
                if (out_type) *out_type = ftype;
                return 1;
            }
        }
        off += rec;
    }
    return 0;
}

// Rust drop-in for ext2_dirblock_find_c (rustkern.rs, #404 / #485 Phase C).
// Same signature, same return contract; a slice built over exactly block_size
// bytes makes the OOB read class impossible by construction. `const char *`
// here and `*const u8` in Rust are ABI-identical (both a pointer); int == i32.
extern int ext2_dirblock_find_rs(const uint8_t *blk, uint32_t block_size,
                                 const char *name, uint32_t name_len,
                                 int ci, uint32_t *out_ino, uint8_t *out_type);

// Live strangler seam: ext2_lookup() calls ext2_dirblock_find(). With
// -DRUST_EXT2_DIRFIND (set in the Makefile CFLAGS for build 794), the real
// symbol routes to the Rust port; otherwise it falls straight back to the C.
// Rollback = drop the one flag and rebuild. The boot-time [RUST-DIFF] self-test
// (ext2_dir_rust_selftest) compares the two impls regardless of the flag.
int ext2_dirblock_find(const uint8_t *blk, uint32_t block_size,
                       const char *name, uint32_t name_len,
                       int ci, uint32_t *out_ino, uint8_t *out_type) {
#ifdef RUST_EXT2_DIRFIND
    return ext2_dirblock_find_rs(blk, block_size, name, name_len, ci, out_ino, out_type);
#else
    return ext2_dirblock_find_c(blk, block_size, name, name_len, ci, out_ino, out_type);
#endif
}

int ext2_lookup(uint32_t dir_ino, const char *name,
                uint32_t *out_ino, uint8_t *out_type) {
    if (!g_ext2.mounted) {
        return -1;
    }
    ext2_inode_t inode;
    if (ext2_read_inode(dir_ino, &inode) != 0) {
        return -2;
    }

    uint32_t name_len = (uint32_t)strlen(name);
    uint64_t size = inode.i_size;
    uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
    if (!blk) {
        return -3;
    }

    uint64_t consumed = 0;
    uint32_t logical = 0;
    while (consumed < size) {
        uint32_t phys = ext2_bmap(&g_ext2, &inode, logical);
        if (phys == EXT2_BMAP_ERR) {
            kfree(blk);
            return -4;
        }
        if (phys != 0) {
            if (ext2_read_block(&g_ext2, phys, blk) != 0) {
                kfree(blk);
                return -5;
            }
            // Scan this directory block via the strangler seam (Rust under
            // -DRUST_EXT2_DIRFIND, else the #476-hardened C). ci = g_root_ext2,
            // exactly as the original inline loop tested it.
            if (ext2_dirblock_find(blk, g_ext2.block_size, name, name_len,
                                   g_root_ext2, out_ino, out_type)) {
                kfree(blk);
                return 0;
            }
        }
        consumed += g_ext2.block_size;
        logical  += 1;
    }

    kfree(blk);
    return 1; // not found
}

// ---------------------------------------------------------------------------
// Boot-time differential self-test for the ext2 directory-block parser
// (#404 / #485 Phase C). Asserts ext2_dirblock_find_rs == ext2_dirblock_find_c
// over WELL-FORMED and MALFORMED directory blocks (truncated header at the
// block tail, zero rec_len, rec_len < 8, oversized / boundary-straddling
// rec_len, name_len that straddles the block end) plus a large random-fuzz
// corpus, on THIS exact build, then logs ONE [RUST-DIFF] line to serial
// (kprintf) + the persistent /BOOTLOG.TXT (bootlog_write). Bounded, runs once
// at boot, no blocking (#426). Any mismatch => FAIL (reported; the golden fold
// stays human-gated). It always calls BOTH impls (and references
// ext2_dirblock_find_rs) so the Rust member is pulled into the link and the two
// are compared regardless of RUST_EXT2_DIRFIND.
#define EXT2DIFF_MAXBS 4096
static uint8_t ext2diff_blk[EXT2DIFF_MAXBS];

static inline uint32_t ext2diff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

// Run both impls on ext2diff_blk[0..bs] for (name,name_len,ci); compare the
// return value and, on a hit, the written out_ino/out_type. Bump counters.
static void ext2diff_one(uint32_t bs, const char *name, uint32_t name_len, int ci,
                         uint32_t *vectors, uint32_t *mismatches, int *first_bad) {
    uint32_t ino_c = 0xC0FFEE00u, ino_rs = 0xC0FFEE00u;
    uint8_t  ty_c  = 0xAB, ty_rs = 0xAB;
    int rc_c  = ext2_dirblock_find_c (ext2diff_blk, bs, name, name_len, ci, &ino_c,  &ty_c);
    int rc_rs = ext2_dirblock_find_rs(ext2diff_blk, bs, name, name_len, ci, &ino_rs, &ty_rs);
    (*vectors)++;
    int bad = (rc_c != rc_rs) || (rc_c == 1 && (ino_c != ino_rs || ty_c != ty_rs));
    if (bad) {
        (*mismatches)++;
        if (*first_bad < 0) *first_bad = (int)(*vectors);
    }
}

void ext2_dir_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    uint32_t seed = 0x9e3779b9u;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;
    const uint32_t bslist[3] = {256u, 512u, 1024u};

    // Pass 1: WELL-FORMED blocks. Lay out random entries, then query every
    // present name (case-sensitive AND case-insensitive) plus random absent
    // names. This is what exercises the FOUND path so out_ino/out_type equality
    // is actually checked, not just the not-found return.
    for (int r = 0; r < 400; r++) {
        uint32_t bs = bslist[ext2diff_rng(&seed) % 3];
        memset(ext2diff_blk, 0, bs);
        char     names[16][16];
        int nent = 0;
        uint32_t off = 0;
        while (off + 12 <= bs && nent < 16) {
            uint32_t nl = 1 + (ext2diff_rng(&seed) % 10);
            uint32_t need = 8 + ((nl + 3u) & ~3u); // ext2 4-byte alignment
            if (off + need > bs) break;
            uint32_t ino = 1 + (ext2diff_rng(&seed) & 0x7fffffu);
            uint8_t  ty  = (uint8_t)(1 + (ext2diff_rng(&seed) % 7));
            for (uint32_t i = 0; i < nl; i++) {
                char c = (char)('a' + (ext2diff_rng(&seed) % 26));
                if (ext2diff_rng(&seed) & 1) c = (char)(c - 32); // mixed case
                names[nent][i] = c;
                ext2diff_blk[off + 8 + i] = (uint8_t)c;
            }
            names[nent][nl] = 0;
            ext2diff_blk[off + 0] = (uint8_t)(ino & 0xFF);
            ext2diff_blk[off + 1] = (uint8_t)((ino >> 8) & 0xFF);
            ext2diff_blk[off + 2] = (uint8_t)((ino >> 16) & 0xFF);
            ext2diff_blk[off + 3] = (uint8_t)((ino >> 24) & 0xFF);
            ext2diff_blk[off + 4] = (uint8_t)(need & 0xFF);
            ext2diff_blk[off + 5] = (uint8_t)((need >> 8) & 0xFF);
            ext2diff_blk[off + 6] = (uint8_t)nl;
            ext2diff_blk[off + 7] = ty;
            off += need;
            nent++;
        }
        for (int e = 0; e < nent; e++) {
            uint32_t nl = (uint32_t)strlen(names[e]);
            ext2diff_one(bs, names[e], nl, 0, &vectors, &mismatches, &first_bad);
            ext2diff_one(bs, names[e], nl, 1, &vectors, &mismatches, &first_bad);
        }
        // A few absent queries (random letters) both ways.
        for (int a = 0; a < 4; a++) {
            char q[16];
            uint32_t nl = 1 + (ext2diff_rng(&seed) % 12);
            for (uint32_t i = 0; i < nl; i++)
                q[i] = (char)('a' + (ext2diff_rng(&seed) % 26));
            q[nl] = 0;
            ext2diff_one(bs, q, nl, ext2diff_rng(&seed) & 1, &vectors, &mismatches, &first_bad);
        }
    }

    // Pass 2: MALFORMED / random fuzz. Fill the block with random bytes so every
    // disk-controlled rec_len / name_len / off takes adversarial values (zero
    // rec_len, rec_len < 8, oversized rec_len, boundary-straddling names,
    // truncated tail headers) and query a random name. Any guard divergence
    // between the C and Rust surfaces here.
    for (int n = 0; n < 24000; n++) {
        uint32_t bs = bslist[ext2diff_rng(&seed) % 3];
        for (uint32_t i = 0; i < bs; i++)
            ext2diff_blk[i] = (uint8_t)(ext2diff_rng(&seed) & 0xFF);
        char q[20];
        uint32_t nl = 1 + (ext2diff_rng(&seed) % 18);
        for (uint32_t i = 0; i < nl; i++)
            q[i] = (char)(ext2diff_rng(&seed) & 0xFF); // full byte range, incl >=0x80
        q[nl] = 0;
        ext2diff_one(bs, q, nl, ext2diff_rng(&seed) & 1, &vectors, &mismatches, &first_bad);
    }

    // Pass 3: explicit crafted malformed shapes (documents each guard class).
    {
        uint32_t bs = 1024;
        // (a) zero rec_len
        memset(ext2diff_blk, 0, bs);
        ext2diff_blk[0] = 5; ext2diff_blk[6] = 3; // e_ino=5, nlen=3, rec=0
        ext2diff_blk[8] = 'a'; ext2diff_blk[9] = 'b'; ext2diff_blk[10] = 'c';
        ext2diff_one(bs, "abc", 3, 0, &vectors, &mismatches, &first_bad);
        // (b) rec_len < 8
        memset(ext2diff_blk, 0, bs);
        ext2diff_blk[0] = 5; ext2diff_blk[4] = 5; ext2diff_blk[6] = 3;
        ext2diff_blk[8] = 'a'; ext2diff_blk[9] = 'b'; ext2diff_blk[10] = 'c';
        ext2diff_one(bs, "abc", 3, 0, &vectors, &mismatches, &first_bad);
        // (c) oversized rec_len (off+rec > bs)
        memset(ext2diff_blk, 0, bs);
        ext2diff_blk[0] = 5; ext2diff_blk[4] = 0xD0; ext2diff_blk[5] = 0x07; // rec=2000
        ext2diff_blk[6] = 3;
        ext2diff_blk[8] = 'a'; ext2diff_blk[9] = 'b'; ext2diff_blk[10] = 'c';
        ext2diff_one(bs, "abc", 3, 0, &vectors, &mismatches, &first_bad);
        // (d) name_len straddles block end (header fits, name does not)
        memset(ext2diff_blk, 0, bs);
        {
            uint32_t off = bs - 12; // 1012; off+8 = 1020 <= bs
            ext2diff_blk[off + 0] = 5;      // e_ino
            ext2diff_blk[off + 4] = 12;     // rec=12 -> off+rec=1024 <= bs (ok)
            ext2diff_blk[off + 6] = 10;     // nlen=10 -> off+8+10=1030 > bs
            for (int i = 0; i < 4; i++) ext2diff_blk[off + 8 + i] = 'x';
        }
        ext2diff_one(bs, "xxxxxxxxxx", 10, 0, &vectors, &mismatches, &first_bad);
        // (e) truncated header at the tail (off+8 > bs handled by loop guard)
        memset(ext2diff_blk, 0, bs);
        ext2diff_blk[0] = 5; ext2diff_blk[4] = (uint8_t)((bs - 4) & 0xFF);
        ext2diff_blk[5] = (uint8_t)(((bs - 4) >> 8) & 0xFF); ext2diff_blk[6] = 3;
        ext2diff_blk[8] = 'a'; ext2diff_blk[9] = 'b'; ext2diff_blk[10] = 'c';
        ext2diff_one(bs, "abc", 3, 0, &vectors, &mismatches, &first_bad);
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] ext2_dir: %u vectors, %u mismatches -> %s\n",
            vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] ext2_dir: %u vectors, %u mismatches -> %s",
                  vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] ext2_dir FIRST MISMATCH at vector #%d\n", first_bad);
        bootlog_write("[RUST-DIFF] ext2_dir FIRST MISMATCH at vector #%d", first_bad);
    }
}

uint32_t ext2_resolve_path(const char *path) {
    if (!g_ext2.mounted || !path || path[0] != '/') {
        return 0;
    }

    uint32_t cur = EXT2_ROOT_INO;
    const char *p = path + 1; // skip leading '/'

    char component[256];
    while (*p) {
        // Extract next path component.
        uint32_t len = 0;
        while (*p && *p != '/') {
            if (len < sizeof(component) - 1) {
                component[len++] = *p;
            }
            p++;
        }
        component[len] = '\0';

        // Skip empty components (e.g. trailing slash or "//").
        if (len == 0) {
            if (*p == '/') p++;
            continue;
        }

        uint32_t next = 0;
        uint8_t  type = 0;
        if (ext2_lookup(cur, component, &next, &type) != 0) {
            return 0; // not found
        }
        cur = next;

        if (*p == '/') p++;
    }
    return cur;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

static void ext2_print_text(const char *label, const char *data, uint64_t len) {
    kprintf("[EXT2] %s (%lu bytes):\n", label, (unsigned long)len);
    kprintf("[EXT2] ----- begin -----\n");
    // Print the content. Add an [EXT2] prefix on each new line for grepping.
    kprintf("[EXT2] ");
    for (uint64_t i = 0; i < len; i++) {
        char c = data[i];
        kprintf("%c", c);
        if (c == '\n' && i + 1 < len) {
            kprintf("[EXT2] ");
        }
    }
    if (len == 0 || data[len - 1] != '\n') {
        kprintf("\n");
    }
    kprintf("[EXT2] ----- end -----\n");
}

void ext2_selftest(void) {
    kprintf("[EXT2] === ext2 read-only self-test (ch0, drive1) ===\n");

    int r = ext2_mount(0, 1, 0);   // whole-disk ext2 on the primary IDE slave
    if (r != 0) {
        kprintf("[EXT2] mount FAILED (rc=%d). Aborting self-test.\n", r);
        return;
    }
    kprintf("[EXT2] mount OK\n");
    kprintf("[EXT2] superblock: block_size=%u inode_size=%u\n",
            (unsigned)g_ext2.block_size, (unsigned)g_ext2.inode_size);
    kprintf("[EXT2] superblock: inodes_count=%u blocks_count=%u\n",
            (unsigned)g_ext2.inodes_count, (unsigned)g_ext2.blocks_count);
    kprintf("[EXT2] superblock: inodes_per_group=%u blocks_per_group=%u groups=%u\n",
            (unsigned)g_ext2.inodes_per_group,
            (unsigned)g_ext2.blocks_per_group,
            (unsigned)g_ext2.groups_count);

    // List the root directory.
    kprintf("[EXT2] --- root directory listing (inode 2) ---\n");
    {
        ext2_inode_t root;
        if (ext2_read_inode(EXT2_ROOT_INO, &root) == 0) {
            uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
            if (blk) {
                uint64_t size = root.i_size;
                uint64_t consumed = 0;
                uint32_t logical = 0;
                while (consumed < size) {
                    uint32_t phys = ext2_bmap(&g_ext2, &root, logical);
                    if (phys != 0 && phys != EXT2_BMAP_ERR &&
                        ext2_read_block(&g_ext2, phys, blk) == 0) {
                        uint32_t off = 0;
                        while (off < g_ext2.block_size) {
                            uint32_t e_ino = rd32(blk + off + 0);
                            uint16_t rec   = rd16(blk + off + 4);
                            uint8_t  nlen  = blk[off + 6];
                            uint8_t  ftype = blk[off + 7];
                            if (rec == 0) break;
                            if (e_ino != 0) {
                                char nm[256];
                                uint32_t k = 0;
                                for (; k < nlen && k < sizeof(nm) - 1; k++) {
                                    nm[k] = (char)blk[off + 8 + k];
                                }
                                nm[k] = '\0';
                                const char *ts = (ftype == EXT2_FT_DIR) ? "dir " :
                                                 (ftype == EXT2_FT_REG_FILE) ? "file" :
                                                 "othr";
                                kprintf("[EXT2]   %s  ino=%u  type=%u(%s)  name='%s'\n",
                                        ts, (unsigned)e_ino, (unsigned)ftype, ts, nm);
                            }
                            off += rec;
                        }
                    }
                    consumed += g_ext2.block_size;
                    logical  += 1;
                }
                kfree(blk);
            }
        } else {
            kprintf("[EXT2]   (failed to read root inode)\n");
        }
    }

    // Read /this-is-a-long-filename.txt
    {
        const char *path = "/this-is-a-long-filename.txt";
        uint32_t ino = ext2_resolve_path(path);
        kprintf("[EXT2] resolve '%s' -> inode %u\n", path, (unsigned)ino);
        if (ino) {
            uint64_t cap = 4096;
            char *buf = (char *)kmalloc(cap);
            if (buf) {
                int64_t n = ext2_read_file_ino(ino, buf, cap);
                if (n >= 0) {
                    ext2_print_text("this-is-a-long-filename.txt", buf, (uint64_t)n);
                } else {
                    kprintf("[EXT2] read failed rc=%d\n", (int)n);
                }
                kfree(buf);
            }
        }
    }

    // Read /big-indirect-test.dat (exercises singly/doubly indirect blocks).
    {
        const char *path = "/big-indirect-test.dat";
        uint32_t ino = ext2_resolve_path(path);
        kprintf("[EXT2] resolve '%s' -> inode %u\n", path, (unsigned)ino);
        if (ino) {
            uint64_t cap = 64 * 1024;
            uint8_t *buf = (uint8_t *)kmalloc(cap);
            if (buf) {
                int64_t n = ext2_read_file_ino(ino, buf, cap);
                if (n >= 0) {
                    uint32_t checksum = 0;
                    for (int64_t i = 0; i < n; i++) {
                        checksum = (checksum + buf[i]) & 0xFFFF;
                    }
                    kprintf("[EXT2] big-indirect-test.dat: read %lu bytes, "
                            "checksum(sum mod 65536)=%u\n",
                            (unsigned long)n, (unsigned)checksum);
                } else {
                    kprintf("[EXT2] read failed rc=%d\n", (int)n);
                }
                kfree(buf);
            } else {
                kprintf("[EXT2] kmalloc 64KB failed for big file\n");
            }
        }
    }

    // Read /subdir/note.md
    {
        const char *path = "/subdir/note.md";
        uint32_t ino = ext2_resolve_path(path);
        kprintf("[EXT2] resolve '%s' -> inode %u\n", path, (unsigned)ino);
        if (ino) {
            uint64_t cap = 4096;
            char *buf = (char *)kmalloc(cap);
            if (buf) {
                int64_t n = ext2_read_file_ino(ino, buf, cap);
                if (n >= 0) {
                    ext2_print_text("subdir/note.md", buf, (uint64_t)n);
                } else {
                    kprintf("[EXT2] read failed rc=%d\n", (int)n);
                }
                kfree(buf);
            }
        }
    }

    kprintf("[EXT2] === self-test complete ===\n");
}

// ===========================================================================
// WRITE SUPPORT (#99 Phase A). On-disk bookkeeping: block/inode bitmaps,
// inode write-back, block allocation (direct + single-indirect), directory
// entry insertion, and file/dir creation. Updates the PRIMARY superblock +
// primary block-group descriptor free counts to stay consistent with bitmaps.
// Target geometry: 1KB blocks, 256B inodes, filetype dir entries.
// ===========================================================================
extern int ata_write_sectors(uint8_t channel, uint8_t drive, uint32_t lba,
                             uint8_t count, void *buffer);

// Forward declarations for the delete/overwrite path (#99 Phase C prereq).
static void ext2_free_block(ext2_fs_t *fs, uint32_t blk);
static void ext2_free_inode(ext2_fs_t *fs, uint32_t ino, int is_dir);
static void ext2_truncate_inode(ext2_fs_t *fs, uint8_t *ri);
static void ext2_rollback_inode(ext2_fs_t *fs, uint32_t ino, int free_ino);
static int  ext2_write_data_to_inode(ext2_fs_t *fs, uint32_t ino,
                                      const void *data, uint32_t len);

static int ext2_write_block(const ext2_fs_t *fs, uint32_t block, const void *buf) {
    uint32_t spb = fs->block_size / EXT2_SECTOR_SIZE;
    int r = blk_write(fs->channel, fs->drive, fs->part_start_lba + block * spb,
                             spb, buf);
    if (r != (int)spb) return -1;
    ext2_cache_update(block, buf, fs->block_size);   // keep cache coherent
    return 0;
}

// Adjust primary superblock free-block / free-inode counts (signed deltas).
static int ext2_sb_adjust(ext2_fs_t *fs, int32_t dblocks, int32_t dinodes) {
    uint8_t *sb = (uint8_t *)kmalloc(1024);
    if (!sb) return -1;
    if (blk_read(fs->channel, fs->drive, fs->part_start_lba + 2, 2, sb) != 2) { kfree(sb); return -1; }
    uint32_t fb = rd32(sb + 12), fi = rd32(sb + 16);
    fb = (uint32_t)((int32_t)fb + dblocks);
    fi = (uint32_t)((int32_t)fi + dinodes);
    memcpy(sb + 12, &fb, 4); memcpy(sb + 16, &fi, 4);
    int rc = (blk_write(fs->channel, fs->drive, fs->part_start_lba + 2, 2, sb) == 2) ? 0 : -1;
    kfree(sb);
    return rc;
}

// Read a 32-bit field from a block group descriptor (off within the 32-byte desc).
static uint32_t ext2_bgd_get32(ext2_fs_t *fs, uint32_t group, int off) {
    uint32_t dpb = fs->block_size / 32;
    uint32_t block = fs->bgd_table_block + group / dpb;
    uint32_t idx = group % dpb;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return 0;
    if (ext2_read_block(fs, block, blk) != 0) { kfree(blk); return 0; }
    uint32_t v = rd32(blk + idx * 32 + off);
    kfree(blk);
    return v;
}

// Adjust a block group descriptor's free-block / free-inode / used-dir counts.
static int ext2_bgd_adjust(ext2_fs_t *fs, uint32_t group, int32_t db, int32_t di, int32_t dd) {
    uint32_t dpb = fs->block_size / 32;
    uint32_t block = fs->bgd_table_block + group / dpb;
    uint32_t idx = group % dpb;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -1;
    if (ext2_read_block(fs, block, blk) != 0) { kfree(blk); return -1; }
    uint8_t *d = blk + idx * 32;
    uint16_t fbc = rd16(d + 12), fic = rd16(d + 14), ud = rd16(d + 16);
    fbc = (uint16_t)(fbc + db); fic = (uint16_t)(fic + di); ud = (uint16_t)(ud + dd);
    memcpy(d + 12, &fbc, 2); memcpy(d + 14, &fic, 2); memcpy(d + 16, &ud, 2);
    int rc = ext2_write_block(fs, block, blk);
    kfree(blk);
    return rc;
}

// Allocate a free data block (zeroed). Returns block number or 0 on failure.
// Allocate a free block. When `zero` is set the new block is zeroed on disk
// (required for indirect-pointer blocks, which are read before being fully
// populated). Data blocks pass zero=0: the caller always overwrites the whole
// block, so the extra zero-write would be pure waste (it roughly doubled the
// write cost of large files).
static uint32_t ext2_alloc_block(ext2_fs_t *fs, int zero) {
    for (uint32_t g = 0; g < fs->groups_count; g++) {
        uint32_t bbmp = ext2_bgd_get32(fs, g, 0);  // bg_block_bitmap
        if (!bbmp) continue;
        uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
        if (!bm) return 0;
        if (ext2_read_block(fs, bbmp, bm) != 0) { kfree(bm); continue; }
        for (uint32_t i = 0; i < fs->blocks_per_group; i++) {
            if (!(bm[i / 8] & (1 << (i % 8)))) {
                uint32_t blkno = fs->first_data_block + g * fs->blocks_per_group + i;
                if (blkno >= fs->blocks_count) { kfree(bm); return 0; }
                bm[i / 8] |= (uint8_t)(1 << (i % 8));
                if (ext2_write_block(fs, bbmp, bm) != 0) { kfree(bm); return 0; }
                kfree(bm);
                if (zero) {
                    uint8_t *z = (uint8_t *)kmalloc(fs->block_size);
                    if (z) { memset(z, 0, fs->block_size); ext2_write_block(fs, blkno, z); kfree(z); }
                }
                ext2_bgd_adjust(fs, g, -1, 0, 0);
                ext2_sb_adjust(fs, -1, 0);
                return blkno;
            }
        }
        kfree(bm);
    }
    return 0;
}

// Allocate a free inode. Returns inode number or 0 on failure.
static uint32_t ext2_alloc_inode(ext2_fs_t *fs, int is_dir) {
    for (uint32_t g = 0; g < fs->groups_count; g++) {
        uint32_t ibmp = ext2_bgd_get32(fs, g, 4);  // bg_inode_bitmap
        if (!ibmp) continue;
        uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
        if (!bm) return 0;
        if (ext2_read_block(fs, ibmp, bm) != 0) { kfree(bm); continue; }
        for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
            if (!(bm[i / 8] & (1 << (i % 8)))) {
                uint32_t ino = g * fs->inodes_per_group + i + 1;
                bm[i / 8] |= (uint8_t)(1 << (i % 8));
                if (ext2_write_block(fs, ibmp, bm) != 0) { kfree(bm); return 0; }
                kfree(bm);
                ext2_bgd_adjust(fs, g, 0, -1, is_dir ? 1 : 0);
                ext2_sb_adjust(fs, 0, -1);
                return ino;
            }
        }
        kfree(bm);
    }
    return 0;
}

// Raw inode read/write (inode_size bytes) for read-modify-write of any field.
static int ext2_inode_raw(ext2_fs_t *fs, uint32_t ino, uint8_t *buf, int write) {
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->groups_count) return -1;
    uint32_t itable = 0;
    if (ext2_group_inode_table(fs, group, &itable) != 0) return -1;
    uint64_t byte_off = (uint64_t)itable * fs->block_size + (uint64_t)index * fs->inode_size;
    uint32_t blk_of = (uint32_t)(byte_off / fs->block_size);
    uint32_t off    = (uint32_t)(byte_off % fs->block_size);
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -1;
    if (ext2_read_block(fs, blk_of, blk) != 0) { kfree(blk); return -1; }
    if (write) { memcpy(blk + off, buf, fs->inode_size); int rc = ext2_write_block(fs, blk_of, blk); kfree(blk); return rc; }
    memcpy(buf, blk + off, fs->inode_size);
    kfree(blk);
    return 0;
}

static inline uint32_t e2_round4(uint32_t n) { return (n + 3) & ~3u; }

// Append a freshly-allocated data block as the next logical block of a file/dir
// inode (direct + single-indirect). Updates i_block[]/i_blocks via raw inode.
// Returns the new physical block number, or 0 on failure.
static uint32_t ext2_inode_append_block(ext2_fs_t *fs, uint32_t ino, uint32_t logical) {
    uint32_t ptrs = fs->block_size / 4;
    uint8_t *ribuf = (uint8_t *)kmalloc(fs->inode_size);
    if (!ribuf) return 0;
    if (ext2_inode_raw(fs, ino, ribuf, 0) != 0) { kfree(ribuf); return 0; }
    uint32_t newb = ext2_alloc_block(fs, 0);   // data block: caller overwrites it fully
    if (!newb) { kfree(ribuf); return 0; }

    uint32_t add_meta  = 0;          // extra (indirect) blocks allocated this call
    uint32_t new_ind   = 0;          // freshly-allocated single-indirect block
    uint32_t new_dind  = 0;          // freshly-allocated double-indirect block
    uint8_t *db = 0, *ib = 0;        // scratch buffers (freed in cleanup)

    if (logical < EXT2_NDIR_BLOCKS) {
        memcpy(ribuf + 40 + logical * 4, &newb, 4);
    } else if (logical - EXT2_NDIR_BLOCKS < ptrs) {
        // ---- single indirect ----
        uint32_t li  = logical - EXT2_NDIR_BLOCKS;
        uint32_t ind = rd32(ribuf + 40 + EXT2_IND_BLOCK * 4);
        if (ind == 0) {
            ind = ext2_alloc_block(fs, 1);          // zeroed by ext2_alloc_block
            if (!ind) goto fail;
            new_ind = ind;
            add_meta = 1;
        }
        ib = (uint8_t *)kmalloc(fs->block_size);
        if (!ib) goto fail;
        if (ext2_read_block(fs, ind, ib) != 0) goto fail;
        memcpy(ib + li * 4, &newb, 4);
        if (ext2_write_block(fs, ind, ib) != 0) goto fail;
        kfree(ib); ib = 0;
        if (new_ind) memcpy(ribuf + 40 + EXT2_IND_BLOCK * 4, &ind, 4);
    } else if (logical - EXT2_NDIR_BLOCKS - ptrs < ptrs * ptrs) {
        // ---- double indirect ----
        // Allocate every level first; persist only after they all succeed, so no
        // on-disk pointer ever references a block we might still roll back.
        uint32_t li2   = logical - EXT2_NDIR_BLOCKS - ptrs;
        uint32_t outer = li2 / ptrs;             // index into double-indirect block
        uint32_t inner = li2 % ptrs;             // index into single-indirect block
        uint32_t dind  = rd32(ribuf + 40 + EXT2_DIND_BLOCK * 4);
        if (dind == 0) {
            dind = ext2_alloc_block(fs, 1);
            if (!dind) goto fail;
            new_dind = dind;
            add_meta++;
        }
        db = (uint8_t *)kmalloc(fs->block_size);
        if (!db) goto fail;
        if (ext2_read_block(fs, dind, db) != 0) goto fail;
        uint32_t ind = rd32(db + outer * 4);
        if (ind == 0) {
            ind = ext2_alloc_block(fs, 1);
            if (!ind) goto fail;
            new_ind = ind;
            add_meta++;
            memcpy(db + outer * 4, &ind, 4);     // in-memory until persisted below
        }
        ib = (uint8_t *)kmalloc(fs->block_size);
        if (!ib) goto fail;
        if (ext2_read_block(fs, ind, ib) != 0) goto fail;
        memcpy(ib + inner * 4, &newb, 4);
        if (ext2_write_block(fs, ind, ib) != 0) goto fail;            // data-index block
        kfree(ib); ib = 0;
        if (new_ind && ext2_write_block(fs, dind, db) != 0) goto fail; // dind->ind link
        kfree(db); db = 0;
        if (new_dind) memcpy(ribuf + 40 + EXT2_DIND_BLOCK * 4, &dind, 4);
    } else {
        goto fail;   // beyond double-indirect (triple not supported)
    }

    // bump i_blocks (in 512-byte units) and persist the inode pointers.
    {
        uint32_t iblocks = rd32(ribuf + 28);
        iblocks += (1 + add_meta) * (fs->block_size / 512);
        memcpy(ribuf + 28, &iblocks, 4);
    }
    ext2_inode_raw(fs, ino, ribuf, 1);
    kfree(ribuf);
    return newb;

fail:
    // Roll back everything reserved this call so the bitmaps stay consistent.
    if (ib) kfree(ib);
    if (db) kfree(db);
    if (new_ind)  ext2_free_block(fs, new_ind);
    if (new_dind) ext2_free_block(fs, new_dind);
    ext2_free_block(fs, newb);
    kfree(ribuf);
    return 0;
}

// Add a directory entry (name -> child_ino, type) to directory `dir_ino`.
static int ext2_dir_add(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                        uint32_t child_ino, uint8_t ftype) {
    int nlen = 0; while (name[nlen]) nlen++;
    uint32_t need = e2_round4((uint32_t)(8 + nlen));
    ext2_inode_t di;
    if (ext2_read_inode(dir_ino, &di) != 0) return -1;
    uint32_t nblocks = di.i_size / fs->block_size;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -1;

    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = ext2_bmap(fs, &di, lb);
        if (phys == 0 || phys == EXT2_BMAP_ERR) continue;
        if (ext2_read_block(fs, phys, blk) != 0) continue;
        uint32_t off = 0;
        // off + 8 <= block_size guarantees the entry header read below is in
        // bounds before rec_len is validated (#476).
        while (off + 8 <= fs->block_size) {
            uint32_t e_ino = rd32(blk + off);
            uint16_t rec   = rd16(blk + off + 4);
            uint8_t  e_nl  = blk[off + 6];
            if (rec < 8 || off + rec > fs->block_size) break;  // corrupt; bail this block
            uint32_t used = (e_ino == 0) ? 0 : e2_round4((uint32_t)(8 + e_nl));
            uint32_t slack = rec - used;
            if (slack >= need) {
                uint32_t newoff;
                if (e_ino == 0) {
                    newoff = off;                 // reuse empty slot
                    need = rec;                   // take whole record
                } else {
                    uint16_t shrunk = (uint16_t)used;
                    memcpy(blk + off + 4, &shrunk, 2);  // shrink existing
                    newoff = off + used;
                    need = (uint16_t)slack;       // new entry spans the rest
                }
                memcpy(blk + newoff + 0, &child_ino, 4);
                uint16_t nrec = (uint16_t)need;
                memcpy(blk + newoff + 4, &nrec, 2);
                blk[newoff + 6] = (uint8_t)nlen;
                blk[newoff + 7] = ftype;
                memcpy(blk + newoff + 8, name, nlen);
                int rc = ext2_write_block(fs, phys, blk);
                kfree(blk);
                return rc;
            }
            off += rec;
        }
    }

    // No room: append a new directory block holding a single full-block entry.
    uint32_t newb = ext2_inode_append_block(fs, dir_ino, nblocks);
    if (!newb) { kfree(blk); return -1; }
    memset(blk, 0, fs->block_size);
    memcpy(blk + 0, &child_ino, 4);
    uint16_t rec = (uint16_t)fs->block_size;
    memcpy(blk + 4, &rec, 2);
    blk[6] = (uint8_t)nlen;
    blk[7] = ftype;
    memcpy(blk + 8, name, nlen);
    int rc = ext2_write_block(fs, newb, blk);
    kfree(blk);
    if (rc != 0) return -1;
    // grow dir size by one block
    uint8_t *ri = (uint8_t *)kmalloc(fs->inode_size);
    if (ri && ext2_inode_raw(fs, dir_ino, ri, 0) == 0) {
        uint32_t sz = rd32(ri + 4) + fs->block_size;
        memcpy(ri + 4, &sz, 4);
        ext2_inode_raw(fs, dir_ino, ri, 1);
    }
    if (ri) kfree(ri);
    return 0;
}

// Create a regular file with `data` under directory `dir_ino`. Returns ino/0.
static uint32_t ext2_create_file_ino(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                                     const void *data, uint32_t len) {
    uint32_t ino = ext2_alloc_inode(fs, 0);
    if (!ino) return 0;
    // zero a fresh inode then fill: mode 0100644, links 1.
    uint8_t *ri = (uint8_t *)kmalloc(fs->inode_size);
    if (!ri) return 0;
    memset(ri, 0, fs->inode_size);
    uint16_t mode = 0x81A4; memcpy(ri + 0, &mode, 2);
    memcpy(ri + 4, &len, 4);
    uint16_t links = 1; memcpy(ri + 26, &links, 2);
    ext2_inode_raw(fs, ino, ri, 1);
    kfree(ri);
    // write data block by block
    const uint8_t *p = (const uint8_t *)data;
    uint32_t remaining = len, lb = 0;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return 0;
    while (remaining > 0) {
        uint32_t phys = ext2_inode_append_block(fs, ino, lb);
        if (!phys) {
            // Out of space mid-write: free the partial blocks + the inode so we
            // never leave an orphan (the dir entry was not added yet).
            kfree(blk);
            ext2_rollback_inode(fs, ino, 1);
            return 0;
        }
        uint32_t chunk = remaining < fs->block_size ? remaining : fs->block_size;
        memset(blk, 0, fs->block_size);
        memcpy(blk, p, chunk);
        ext2_write_block(fs, phys, blk);
        p += chunk; remaining -= chunk; lb++;
    }
    kfree(blk);
    if (ext2_dir_add(fs, dir_ino, name, ino, EXT2_FT_REG_FILE) != 0) {
        ext2_rollback_inode(fs, ino, 1);   // could not link it; reclaim everything
        return 0;
    }
    return ino;
}

// ---- path-based public API ------------------------------------------------
// Split "/a/b/c" -> parent ino (resolve "/a/b") + basename "c".
static uint32_t ext2_parent_and_base(const char *path, char *base_out, int base_sz) {
    int len = 0; while (path[len]) len++;
    int slash = -1;
    for (int i = len - 1; i >= 0; i--) if (path[i] == '/') { slash = i; break; }
    if (slash < 0) { // no slash: relative to root
        int j = 0; for (; path[j] && j < base_sz - 1; j++) base_out[j] = path[j];
        base_out[j] = 0;
        return EXT2_ROOT_INO;
    }
    int bl = len - slash - 1; if (bl >= base_sz) bl = base_sz - 1;
    for (int i = 0; i < bl; i++) base_out[i] = path[slash + 1 + i];
    base_out[bl] = 0;
    if (slash == 0) return EXT2_ROOT_INO;
    char parent[256]; int pl = slash < 255 ? slash : 255;
    for (int i = 0; i < pl; i++) parent[i] = path[i];
    parent[pl] = 0;
    return ext2_resolve_path(parent);
}

// Write (create/overwrite-by-create) a file at an absolute ext2 path.
int ext2_write_file(const char *path, const void *data, uint32_t len) {
    if (!g_ext2.mounted) return -1;
    char base[256];
    uint32_t parent = ext2_parent_and_base(path, base, sizeof(base));
    if (!parent || !base[0]) return -1;
    // If it already exists: overwrite in place (truncate the inode, keep its
    // number + directory entry, write the new contents). #99 Phase C.
    uint32_t existing = 0; uint8_t t;
    if (ext2_lookup(parent, base, &existing, &t) == 0) {
        if (t == EXT2_FT_DIR) return -2;   // never clobber a directory
        uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
        if (!ri) return -3;
        if (ext2_inode_raw(&g_ext2, existing, ri, 0) != 0) { kfree(ri); return -3; }
        ext2_truncate_inode(&g_ext2, ri);          // frees old data blocks
        uint32_t sz = len; memcpy(ri + 4, &sz, 4);  // new i_size
        ext2_inode_raw(&g_ext2, existing, ri, 1);
        kfree(ri);
        return ext2_write_data_to_inode(&g_ext2, existing, data, len);
    }
    uint32_t ino = ext2_create_file_ino(&g_ext2, parent, base, data, len);
    return ino ? 0 : -3;
}

// Create a directory at an absolute ext2 path.
int ext2_mkdir(const char *path) {
    if (!g_ext2.mounted) return -1;
    char base[256];
    uint32_t parent = ext2_parent_and_base(path, base, sizeof(base));
    if (!parent || !base[0]) return -1;
    uint32_t existing = 0; uint8_t t;
    if (ext2_lookup(parent, base, &existing, &t) == 0) return -2;
    uint32_t ino = ext2_alloc_inode(&g_ext2, 1);
    if (!ino) return -3;
    // inode: dir mode 040755, links 2 (self + "."), size = 1 block
    uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (!ri) return -3;
    memset(ri, 0, g_ext2.inode_size);
    uint16_t mode = 0x41ED; memcpy(ri + 0, &mode, 2);
    uint32_t sz = g_ext2.block_size; memcpy(ri + 4, &sz, 4);
    uint16_t links = 2; memcpy(ri + 26, &links, 2);
    ext2_inode_raw(&g_ext2, ino, ri, 1);
    kfree(ri);
    // first data block with "." and ".."
    uint32_t db = ext2_inode_append_block(&g_ext2, ino, 0);
    if (!db) return -3;
    uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
    if (!blk) return -3;
    memset(blk, 0, g_ext2.block_size);
    // "."
    memcpy(blk + 0, &ino, 4);
    uint16_t r1 = 12; memcpy(blk + 4, &r1, 2);
    blk[6] = 1; blk[7] = EXT2_FT_DIR; blk[8] = '.';
    // ".." spans rest of block
    memcpy(blk + 12 + 0, &parent, 4);
    uint16_t r2 = (uint16_t)(g_ext2.block_size - 12); memcpy(blk + 12 + 4, &r2, 2);
    blk[12 + 6] = 2; blk[12 + 7] = EXT2_FT_DIR; blk[12 + 8] = '.'; blk[12 + 9] = '.';
    ext2_write_block(&g_ext2, db, blk);
    kfree(blk);
    // link into parent + bump parent links_count (for "..")
    if (ext2_dir_add(&g_ext2, parent, base, ino, EXT2_FT_DIR) != 0) return -3;
    uint8_t *pri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (pri && ext2_inode_raw(&g_ext2, parent, pri, 0) == 0) {
        uint16_t pl = rd16(pri + 26); pl++; memcpy(pri + 26, &pl, 2);
        ext2_inode_raw(&g_ext2, parent, pri, 1);
    }
    if (pri) kfree(pri);
    return 0;
}

// Iterate directory entries by byte position (#99 Phase B). *pos = byte offset to
// resume from; on success returns 0, fills name_out/ino_out/type_out and advances
// *pos. Skips inode-0 slots and "." / "..". Returns -1 at end / on error.
int ext2_readdir_ino(uint32_t dir_ino, uint32_t *pos, char *name_out, int name_max,
                     uint32_t *ino_out, uint8_t *type_out) {
    if (!g_ext2.mounted || !pos || !name_out || name_max < 2) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(dir_ino, &inode) != 0) return -1;
    uint64_t size = inode.i_size;
    uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
    if (!blk) return -1;
    uint32_t p = *pos;
    while (p < size) {
        uint32_t logical = p / g_ext2.block_size;
        uint32_t off = p % g_ext2.block_size;
        uint32_t phys = ext2_bmap(&g_ext2, &inode, logical);
        if (phys == 0 || phys == EXT2_BMAP_ERR) { p = (logical + 1) * g_ext2.block_size; continue; }
        if (ext2_read_block(&g_ext2, phys, blk) != 0) { kfree(blk); return -1; }
        // The 8-byte entry header must fit before we read it; a corrupt block
        // that parks off in the trailing < 8 bytes otherwise over-reads (#476).
        if (off + 8 > g_ext2.block_size) { p = (logical + 1) * g_ext2.block_size; continue; }
        uint32_t e_ino = rd32(blk + off + 0);
        uint16_t rec   = rd16(blk + off + 4);
        uint8_t  nlen  = blk[off + 6];
        uint8_t  ftype = blk[off + 7];
        if (rec < 8 || off + rec > g_ext2.block_size) { kfree(blk); return -1; }
        if ((uint32_t)off + 8 + nlen > g_ext2.block_size) { kfree(blk); return -1; }
        p += rec;
        if (e_ino != 0 && nlen > 0 &&
            !(nlen == 1 && blk[off + 8] == '.') &&
            !(nlen == 2 && blk[off + 8] == '.' && blk[off + 9] == '.')) {
            int n = nlen; if (n >= name_max) n = name_max - 1;
            for (int i = 0; i < n; i++) name_out[i] = (char)blk[off + 8 + i];
            name_out[n] = 0;
            if (ino_out)  *ino_out  = e_ino;
            if (type_out) *type_out = ftype;
            *pos = p;
            kfree(blk);
            return 0;
        }
    }
    kfree(blk);
    return -1;
}

// ===========================================================================
// DELETE / OVERWRITE support (#99 Phase C). Free blocks + inodes back into the
// bitmaps, truncate an inode's data (direct + single + double indirect), unlink
// a regular file (free its data + inode, then clear its directory entry).
// ===========================================================================

// Clear one data/metadata block in its group block-bitmap, bump free counts.
static void ext2_free_block(ext2_fs_t *fs, uint32_t blk) {
    if (blk < fs->first_data_block) return;
    uint32_t rel = blk - fs->first_data_block;
    uint32_t g   = rel / fs->blocks_per_group;
    uint32_t idx = rel % fs->blocks_per_group;
    uint32_t bbmp = ext2_bgd_get32(fs, g, 0);   // bg_block_bitmap
    if (!bbmp) return;
    uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm) return;
    if (ext2_read_block(fs, bbmp, bm) == 0) {
        if (bm[idx / 8] & (1 << (idx % 8))) {
            bm[idx / 8] &= ~(1 << (idx % 8));
            ext2_write_block(fs, bbmp, bm);
            ext2_bgd_adjust(fs, g, +1, 0, 0);
            ext2_sb_adjust(fs, +1, 0);
        }
    }
    kfree(bm);
}

// Clear an inode bit in its group inode-bitmap, bump free counts.
static void ext2_free_inode(ext2_fs_t *fs, uint32_t ino, int is_dir) {
    if (ino == 0) return;
    uint32_t g   = (ino - 1) / fs->inodes_per_group;
    uint32_t idx = (ino - 1) % fs->inodes_per_group;
    uint32_t ibmp = ext2_bgd_get32(fs, g, 4);   // bg_inode_bitmap
    if (!ibmp) return;
    uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm) return;
    if (ext2_read_block(fs, ibmp, bm) == 0) {
        if (bm[idx / 8] & (1 << (idx % 8))) {
            bm[idx / 8] &= ~(1 << (idx % 8));
            ext2_write_block(fs, ibmp, bm);
            ext2_bgd_adjust(fs, g, 0, +1, is_dir ? -1 : 0);
            ext2_sb_adjust(fs, 0, +1);
        }
    }
    kfree(bm);
}

// Free every data block referenced by a raw inode (in `ri`) and zero out its
// i_block[] / i_size / i_blocks. Handles direct, single- and double-indirect.
static void ext2_truncate_inode(ext2_fs_t *fs, uint8_t *ri) {
    uint32_t ptrs = fs->block_size / 4;
    uint32_t zero = 0;
    // direct blocks
    for (int i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        uint32_t b = rd32(ri + 40 + i * 4);
        if (b) ext2_free_block(fs, b);
        memcpy(ri + 40 + i * 4, &zero, 4);
    }
    // single indirect
    uint32_t ind = rd32(ri + 40 + EXT2_IND_BLOCK * 4);
    if (ind) {
        uint8_t *ib = (uint8_t *)kmalloc(fs->block_size);
        if (ib) {
            if (ext2_read_block(fs, ind, ib) == 0)
                for (uint32_t k = 0; k < ptrs; k++) {
                    uint32_t b = rd32(ib + k * 4);
                    if (b) ext2_free_block(fs, b);
                }
            kfree(ib);
        }
        ext2_free_block(fs, ind);
        memcpy(ri + 40 + EXT2_IND_BLOCK * 4, &zero, 4);
    }
    // double indirect
    uint32_t dind = rd32(ri + 40 + EXT2_DIND_BLOCK * 4);
    if (dind) {
        uint8_t *ob = (uint8_t *)kmalloc(fs->block_size);
        if (ob) {
            if (ext2_read_block(fs, dind, ob) == 0)
                for (uint32_t o = 0; o < ptrs; o++) {
                    uint32_t ind2 = rd32(ob + o * 4);
                    if (!ind2) continue;
                    uint8_t *ib = (uint8_t *)kmalloc(fs->block_size);
                    if (ib) {
                        if (ext2_read_block(fs, ind2, ib) == 0)
                            for (uint32_t k = 0; k < ptrs; k++) {
                                uint32_t b = rd32(ib + k * 4);
                                if (b) ext2_free_block(fs, b);
                            }
                        kfree(ib);
                    }
                    ext2_free_block(fs, ind2);
                }
            kfree(ob);
        }
        ext2_free_block(fs, dind);
        memcpy(ri + 40 + EXT2_DIND_BLOCK * 4, &zero, 4);
    }
    memcpy(ri + 4,  &zero, 4);   // i_size  = 0
    memcpy(ri + 28, &zero, 4);   // i_blocks = 0
}

// Roll an inode back to empty (free its data + indirect blocks, zero its size).
// When free_ino is set, also release the inode itself; used to clean up after a
// write that failed part-way so the filesystem is never left inconsistent.
static void ext2_rollback_inode(ext2_fs_t *fs, uint32_t ino, int free_ino) {
    uint8_t *ri = (uint8_t *)kmalloc(fs->inode_size);
    if (ri && ext2_inode_raw(fs, ino, ri, 0) == 0) {
        ext2_truncate_inode(fs, ri);
        ext2_inode_raw(fs, ino, ri, 1);
    }
    if (ri) kfree(ri);
    if (free_ino) ext2_free_inode(fs, ino, 0);
}

// Write `len` bytes into an (already-sized) inode, growing blocks as needed.
static int ext2_write_data_to_inode(ext2_fs_t *fs, uint32_t ino,
                                    const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t remaining = len, lb = 0;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -3;
    while (remaining > 0) {
        uint32_t phys = ext2_inode_append_block(fs, ino, lb);
        if (!phys) {
            // Out of space mid-overwrite: roll the inode back to a valid empty
            // file (data lost, but the filesystem stays consistent) rather than
            // leaving a half-written file with dangling size.
            kfree(blk);
            ext2_rollback_inode(fs, ino, 0);
            return -3;
        }
        uint32_t chunk = remaining < fs->block_size ? remaining : fs->block_size;
        memset(blk, 0, fs->block_size);
        memcpy(blk, p, chunk);
        ext2_write_block(fs, phys, blk);
        p += chunk; remaining -= chunk; lb++;
    }
    kfree(blk);
    return 0;
}

// Remove the directory entry for `name` in `dir_ino` (clear its inode field,
// merging its space into the previous entry's rec_len). Returns 0 on success.
static int ext2_dir_remove(ext2_fs_t *fs, uint32_t dir_ino, const char *name) {
    int nlen = 0; while (name[nlen]) nlen++;
    ext2_inode_t di;
    if (ext2_read_inode(dir_ino, &di) != 0) return -1;
    uint32_t nblocks = di.i_size / fs->block_size;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -1;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = ext2_bmap(fs, &di, lb);
        if (!phys) continue;
        if (ext2_read_block(fs, phys, blk) != 0) continue;
        uint32_t off = 0, prev = 0xFFFFFFFF;
        // off + 8 <= block_size keeps the header read in bounds; rec_len and the
        // name field are then validated before use (#476).
        while (off + 8 <= fs->block_size) {
            uint32_t ino = rd32(blk + off);
            uint16_t rec_len = blk[off + 4] | (blk[off + 5] << 8);
            uint8_t  name_len = blk[off + 6];
            if (rec_len < 8 || off + rec_len > fs->block_size) break;
            if (ino != 0 && name_len == nlen &&
                off + 8 + (uint32_t)nlen <= fs->block_size &&
                memcmp(blk + off + 8, name, nlen) == 0) {
                if (prev != 0xFFFFFFFF) {
                    // absorb this slot into the previous entry
                    uint16_t pl = blk[prev + 4] | (blk[prev + 5] << 8);
                    pl += rec_len;
                    blk[prev + 4] = pl & 0xFF; blk[prev + 5] = (pl >> 8) & 0xFF;
                } else {
                    uint32_t z = 0; memcpy(blk + off, &z, 4);  // first entry: just void inode
                }
                ext2_write_block(fs, phys, blk);
                kfree(blk);
                return 0;
            }
            prev = off;
            off += rec_len;
        }
    }
    kfree(blk);
    return -1;
}

// Public: delete a regular file at an absolute ext2 path.
int ext2_unlink(const char *path) {
    if (!g_ext2.mounted) return -1;
    char base[256];
    uint32_t parent = ext2_parent_and_base(path, base, sizeof(base));
    if (!parent || !base[0]) return -1;
    uint32_t ino = 0; uint8_t ft = 0;
    if (ext2_lookup(parent, base, &ino, &ft) != 0) return -1;  // not found
    if (ft == EXT2_FT_DIR) return -2;                          // rmdir not supported
    // free data blocks then the inode itself
    uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (ri) {
        if (ext2_inode_raw(&g_ext2, ino, ri, 0) == 0) {
            ext2_truncate_inode(&g_ext2, ri);
            uint16_t links = 0; memcpy(ri + 26, &links, 2);  // i_links_count = 0
            // i_dtime (offset 20) must be non-zero on a deleted inode, else
            // e2fsck reports "deleted inode has zero dtime".
            uint32_t dtime = 1781000000u; memcpy(ri + 20, &dtime, 4);
            ext2_inode_raw(&g_ext2, ino, ri, 1);
        }
        kfree(ri);
    }
    ext2_free_inode(&g_ext2, ino, 0);
    return ext2_dir_remove(&g_ext2, parent, base);
}

// ===========================================================================
// Root-cutover helper (#99 Phase C). Read an entire regular file from the
// mounted ext2 volume into a freshly kmalloc'd buffer. Returns NULL if the
// volume is not mounted, the path is missing, or it is a directory. Used by the
// fat_read_file() ext2-first hook when g_root_ext2 is set.
// ===========================================================================
void *ext2_read_whole(const char *path, uint32_t *size_out) {
    if (!g_ext2.mounted || !path) return NULL;
    uint32_t ino = ext2_resolve_path(path);
    if (!ino) return NULL;
    ext2_inode_t in;
    if (ext2_read_inode(ino, &in) != 0) return NULL;
    if ((in.i_mode & 0xF000) == 0x4000) return NULL;   // directory, not a file
    uint32_t sz = in.i_size;
    void *buf = (void *)kmalloc(sz ? sz : 1);
    if (!buf) return NULL;
    int64_t n = sz ? ext2_read_file_ino(ino, buf, sz) : 0;
    if (n < 0) { kfree(buf); return NULL; }
    if (size_out) *size_out = (uint32_t)n;
    return buf;
}

// Is the ext2 volume currently mounted? (for the root-cutover marker check)
int ext2_is_mounted(void) { return g_ext2.mounted; }

// #539: absolute device byte offset just past the END of the mounted ext2
// volume, i.e. part_start_lba*512 + blocks_count*block_size. Used by main.c to
// size the USB TO-RAM window so it spans the whole two-partition image (FAT ESP
// + ext2 ROOT), not just the FAT ESP. Returns 0 if no ext2 volume is mounted.
uint64_t ext2_end_bytes(void) {
    if (!g_ext2.mounted) return 0;
    return (uint64_t)g_ext2.part_start_lba * EXT2_SECTOR_SIZE +
           (uint64_t)g_ext2.blocks_count * (uint64_t)g_ext2.block_size;
}
