// cdbench.c - [no-ticket]: sequential read benchmark over a mounted disc image.
//             See cdbench.h for what this is for and why it exists.
//
// LANGUAGE NOTE (standing Rust-first rule): every DERIVED QUANTITY printed here
// comes from rustkern/blkhist.rs (throughput, round trips per megabyte, the
// projection onto a known per-command cost). This file is C because it is I/O
// glue against fat_read_file(), diskimg_read_range_gen(), kmalloc() and
// kprintf(), exactly the split dos/usbvol.c already documents. There is no
// arithmetic here that is not first done in Rust.
#include "cdbench.h"
#include "diskimg.h"
#include "../types.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"

extern int   kprintf(const char *fmt, ...);
extern void *kmalloc(unsigned long n);
extern void  kfree(void *p);
extern void *fat_read_file(fat_fs_t *fs, const char *path, uint32_t *size_out);
extern fat_fs_t g_fat_fs;
extern uint64_t mono_us(void);
extern int64_t diskimg_read_range_gen(char letter, uint32_t gen, const char *relpath,
                                      uint64_t off, uint64_t len, void *dst);
extern int diskimg_is_mounted(char letter);
extern int diskimg_stat(char letter, const char *relpath, uint64_t *size_out, int *isdir_out);
extern void imgfile_readahead_set_disabled(int off);
extern void isomemo_set_disabled(int off);

// The block layer's own counters, so the benchmark reports what the DEVICE did
// rather than what it assumes the device did.
extern void blk_census_io(uint64_t *calls, uint64_t *sectors,
                          uint64_t *n_dev, uint64_t *t_dev);

// rustkern/blkhist.rs: every derived quantity.
extern uint64_t blkhist_xfers_per_mb_x10_rs(uint64_t xfers, uint64_t sectors);
extern uint64_t blkhist_sectors_per_xfer_x10_rs(uint64_t xfers, uint64_t sectors);
extern uint64_t blkhist_kbps_rs(uint64_t bytes, uint64_t us);
extern uint64_t blkhist_projected_ms_rs(uint64_t xfers, uint64_t cmd_us);

#define CDBENCH_DEF_MB    32u
#define CDBENCH_DEF_CHUNK 4096u
#define CDBENCH_MAX_CHUNK 65536u
// Mixed reads per arm of the agreement oracle. 3000 is enough to cover every
// (kind, seek) combination many times over and costs a couple of seconds.
#define CDBENCH_VERIFY_READS 3000

// Parse a bounded unsigned decimal. Returns the value; *p is advanced past it.
static uint32_t parse_u32(const char **p) {
    uint32_t v = 0;
    while (**p >= '0' && **p <= '9') {
        if (v > 100000000u) break;          // refuse to overflow, clamp instead
        v = v * 10u + (uint32_t)(**p - '0');
        (*p)++;
    }
    return v;
}

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

void cdbench_maybe_run(void) {
    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/CDBENCH.CFG", &sz);
    if (!cfg || sz == 0) { if (cfg) kfree(cfg); return; }

    // Copy to a NUL-terminated, single-line buffer.
    char line[192];
    uint32_t n = 0;
    for (; n < sz && n < sizeof(line) - 1; n++) {
        char ch = cfg[n];
        if (ch == '\r' || ch == '\n') break;
        line[n] = ch;
    }
    line[n] = '\0';
    kfree(cfg);

    const char *p = line;
    skip_ws(&p);
    char letter = *p;
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'Z') {
        kprintf("[CDBENCH] CDBENCH.CFG: first field is not a drive letter; ignored\n");
        return;
    }
    p++;
    if (*p == ':') p++;
    skip_ws(&p);

    char rel[128];
    uint32_t rn = 0;
    while (*p && *p != ' ' && *p != '\t' && rn < sizeof(rel) - 1) rel[rn++] = *p++;
    rel[rn] = '\0';
    if (rn == 0) {
        kprintf("[CDBENCH] CDBENCH.CFG: no path given; ignored\n");
        return;
    }

    skip_ws(&p);
    uint32_t want_mb = parse_u32(&p);
    if (want_mb == 0) want_mb = CDBENCH_DEF_MB;
    skip_ws(&p);
    uint32_t chunk = parse_u32(&p);
    if (chunk == 0) chunk = CDBENCH_DEF_CHUNK;
    if (chunk > CDBENCH_MAX_CHUNK) chunk = CDBENCH_MAX_CHUNK;

    if (!diskimg_is_mounted(letter)) {
        kprintf("[CDBENCH] %c: is not mounted; nothing to measure\n", letter);
        bootlog_write("[CDBENCH] %c: not mounted", letter);
        return;
    }

    uint64_t fsize = 0; int isdir = 1;
    if (!diskimg_stat(letter, rel, &fsize, &isdir) || isdir) {
        kprintf("[CDBENCH] %c:%s not found (or is a directory)\n", letter, rel);
        bootlog_write("[CDBENCH] %c:%s not found", letter, rel);
        return;
    }

    uint64_t want = (uint64_t)want_mb * 1024ull * 1024ull;
    if (want > fsize) want = fsize;

    uint8_t *buf = (uint8_t *)kmalloc(chunk);
    if (!buf) { kprintf("[CDBENCH] out of memory for a %u-byte chunk\n", chunk); return; }

    // TWO ARMS, ONE KERNEL, ONE BOOT, TWO COLD REGIONS OF THE SAME FILE.
    //
    // Arm A pins the readahead window to one block and forces every read to
    // re-walk the ISO directory tree, which is EXACTLY the behaviour before
    // this change. Arm B is the shipped behaviour. Nothing else differs: same
    // medium, same file, same chunk size, same boot, same host load.
    //
    // The arms read DIFFERENT regions on purpose. The block layer's demand
    // cache is hundreds of megabytes, so a second pass over the same offsets
    // would be served from RAM and would measure the cache, not the device.
    // Arm A takes the file from 0; arm B starts one arm-length further in, so
    // both are cold. If the file is too short to hold two disjoint regions the
    // second arm is skipped and says so, rather than quietly measuring RAM.
    uint64_t armA_off = 0;
    uint64_t armB_off = want;
    int two_arms = (armB_off + want <= fsize);

    for (int arm = 0; arm < 2; arm++) {
        if (arm == 1 && !two_arms) {
            kprintf("[CDBENCH] file is %llu MB: too short for two disjoint cold "
                    "regions of %llu MB, so only the control arm ran. Halve the "
                    "megabyte field to get both.\n",
                    (unsigned long long)(fsize / (1024 * 1024)),
                    (unsigned long long)(want / (1024 * 1024)));
            break;
        }
        const char *name = (arm == 0) ? "A/BEFORE (readahead off, memo off)"
                                      : "B/AFTER  (readahead on,  memo on)";
        imgfile_readahead_set_disabled(arm == 0);
        isomemo_set_disabled(arm == 0);

        uint64_t base = (arm == 0) ? armA_off : armB_off;

        uint64_t c0 = 0, s0 = 0, d0 = 0, t0dev = 0;
        blk_census_io(&c0, &s0, &d0, &t0dev);

        uint64_t t0 = mono_us();
        uint64_t off = 0;
        uint64_t got_total = 0;
        int failed = 0;
        while (off < want) {
            uint64_t nreq = want - off;
            if (nreq > chunk) nreq = chunk;
            int64_t got = diskimg_read_range_gen(letter, 0, rel, base + off, nreq, buf);
            if (got <= 0) { failed = 1; break; }
            off += (uint64_t)got;
            got_total += (uint64_t)got;
            if ((uint64_t)got < nreq) break;          // EOF
        }
        uint64_t t1 = mono_us();

        uint64_t c1 = 0, s1 = 0, d1 = 0, t1dev = 0;
        blk_census_io(&c1, &s1, &d1, &t1dev);

        uint64_t wall   = (t1 > t0) ? (t1 - t0) : 1;
        uint64_t calls  = c1 - c0;
        uint64_t sect   = s1 - s0;
        uint64_t xfers  = d1 - d0;
        uint64_t devus  = t1dev - t0dev;

        uint64_t kbps     = blkhist_kbps_rs(got_total, wall);
        uint64_t spx_x10  = blkhist_sectors_per_xfer_x10_rs(xfers, sect);
        uint64_t mb_deliv = got_total / (1024 * 1024);
        uint64_t xpm      = mb_deliv ? (xfers / mb_deliv) : 0;

        kprintf("[CDBENCH] %s\n", name);
        kprintf("[CDBENCH]   %c:%s at +%llu MB: %llu KB in %llu reads of %u bytes"
                " -> %llu KB/s, wall %llu us, device %llu us (%llu%% of wall)%s\n",
                letter, rel, (unsigned long long)(base / (1024 * 1024)),
                (unsigned long long)(got_total / 1024),
                (unsigned long long)((got_total + chunk - 1) / chunk), chunk,
                (unsigned long long)kbps,
                (unsigned long long)wall, (unsigned long long)devus,
                (unsigned long long)(wall ? devus * 100 / wall : 0),
                failed ? "  [READ FAILED PART WAY]" : "");
        // ROUND TRIPS is the device-independent half and the mechanism this
        // change is about: it is the same number in a VM and on the owner's
        // laptop, whereas KB/s above is only true of the hardware it was taken
        // on. The projection makes the count a time on a device whose fixed
        // per-command cost is known (his stick measured 121 us).
        kprintf("[CDBENCH]   ROUND TRIPS: %llu device xfers, %llu sectors, %llu "
                "blk_read calls -> %llu.%llu sectors/xfer, %llu xfers per MB of "
                "file | at 121us/cmd = %llu ms of pure round trip\n",
                (unsigned long long)xfers, (unsigned long long)sect,
                (unsigned long long)calls,
                (unsigned long long)(spx_x10 / 10), (unsigned long long)(spx_x10 % 10),
                (unsigned long long)xpm,
                (unsigned long long)blkhist_projected_ms_rs(xfers, 121));
        bootlog_write("[CDBENCH] arm%c %c:%s %lluKB %lluKB/s %lluxfers %llu/MB "
                      "%llu.%llu sec/xfer",
                      arm == 0 ? 'A' : 'B', letter, rel,
                      (unsigned long long)(got_total / 1024),
                      (unsigned long long)kbps, (unsigned long long)xfers,
                      (unsigned long long)xpm,
                      (unsigned long long)(spx_x10 / 10),
                      (unsigned long long)(spx_x10 % 10));
    }

    // -----------------------------------------------------------------------
    // AGREEMENT ORACLE. Speed proved above; this proves the bytes.
    //
    // Readahead changes WHICH blocks are fetched, WHERE they are placed and
    // WHAT gets evicted, and the resolved-extent memo changes whether the
    // directory is walked. None of that is allowed to change a single byte the
    // caller receives. So: run one FIXED, deterministic sequence of reads with
    // both off, digest every byte, then run the IDENTICAL sequence with both
    // on, and require the digests to match.
    //
    // It needs no table of expected values, so it cannot be wrong about what
    // the answer should be; it fails the moment two code paths that must answer
    // identically stop doing so. That is the shape worth asserting (probe_agree,
    // blame.md).
    //
    // The sequence deliberately mixes SEQUENTIAL runs (which engage readahead)
    // with BACKWARD and RANDOM seeks (which reset it, evict its blocks, and
    // exercise the partial-block and boundary cases), plus reads that straddle
    // cache-block boundaries and the last partial block of the file. A purely
    // sequential oracle would never touch the paths most likely to be wrong.
    {
        uint8_t *vb = (uint8_t *)kmalloc(CDBENCH_MAX_CHUNK);
        if (vb) {
            uint64_t digest[2] = { 0, 0 };
            uint64_t nbytes[2] = { 0, 0 };
            uint64_t nreads[2] = { 0, 0 };
            for (int arm = 0; arm < 2; arm++) {
                imgfile_readahead_set_disabled(arm == 0);
                isomemo_set_disabled(arm == 0);
                uint64_t seed = 0x9E3779B97F4A7C15ull;   // same both arms
                uint64_t h = 1469598103934665603ull;     // FNV-1a 64 offset
                uint64_t pos = 0;
                for (int i = 0; i < CDBENCH_VERIFY_READS; i++) {
                    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                    uint32_t kind = (uint32_t)(seed >> 60) & 3u;
                    uint64_t len;
                    switch (kind) {
                        case 0:  len = 4096; break;                    // the DOS chunk
                        case 1:  len = 8192 + 1; break;                // straddles a block
                        case 2:  len = 512; break;                     // sub-block
                        default: len = 1 + ((seed >> 20) % 20000); break;
                    }
                    if (kind == 3) {
                        // Seek somewhere else entirely: resets the stream.
                        pos = (seed >> 12) % (fsize ? fsize : 1);
                    } else if (i % 7 == 6) {
                        // Backward seek: the case a forward-only predictor gets
                        // wrong and a stale prediction would answer for.
                        pos = (pos > 100000) ? pos - 100000 : 0;
                    }
                    if (len > CDBENCH_MAX_CHUNK) len = CDBENCH_MAX_CHUNK;
                    int64_t got = diskimg_read_range_gen(letter, 0, rel, pos, len, vb);
                    if (got < 0) { h ^= 0xDEADBEEFull; h *= 1099511628211ull; continue; }
                    for (int64_t k = 0; k < got; k++) {
                        h ^= vb[k];
                        h *= 1099511628211ull;
                    }
                    // The LENGTH is part of the answer: two paths that return
                    // the same bytes but disagree about how many have still
                    // disagreed, and a bytes-only digest would hide it.
                    h ^= (uint64_t)got; h *= 1099511628211ull;
                    nbytes[arm] += (uint64_t)got;
                    nreads[arm]++;
                    pos += (uint64_t)got;
                    if (pos >= fsize) pos = 0;
                }
                digest[arm] = h;
            }
            // The last partial block of the file, both ways, explicitly: the
            // one place a multi-block fetch can over-tag a slot that holds
            // fewer bytes than it claims.
            uint64_t tailoff = (fsize > 20000) ? (fsize - 20000) : 0;
            uint64_t tailh[2] = { 0, 0 };
            for (int arm = 0; arm < 2; arm++) {
                imgfile_readahead_set_disabled(arm == 0);
                isomemo_set_disabled(arm == 0);
                uint64_t h = 1469598103934665603ull;
                for (uint64_t o = tailoff; o < fsize; o += 4096) {
                    uint64_t len = fsize - o; if (len > 4096) len = 4096;
                    int64_t got = diskimg_read_range_gen(letter, 0, rel, o, len, vb);
                    if (got < 0) { h ^= 0xBADull; h *= 1099511628211ull; continue; }
                    for (int64_t k = 0; k < got; k++) { h ^= vb[k]; h *= 1099511628211ull; }
                    h ^= (uint64_t)got; h *= 1099511628211ull;
                }
                tailh[arm] = h;
            }
            kfree(vb);

            int ok = (digest[0] == digest[1]) && (nbytes[0] == nbytes[1])
                  && (nreads[0] == nreads[1]) && (tailh[0] == tailh[1]);
            kprintf("[CDBENCH] AGREEMENT: %d mixed reads %llu bytes, digest "
                    "off=%llx on=%llx | EOF tail off=%llx on=%llx -> %s\n",
                    CDBENCH_VERIFY_READS, (unsigned long long)nbytes[0],
                    (unsigned long long)digest[0], (unsigned long long)digest[1],
                    (unsigned long long)tailh[0], (unsigned long long)tailh[1],
                    ok ? "AGREE" : "*** DISAGREE ***");
            bootlog_write("[CDBENCH] AGREEMENT %s digest %llx/%llx tail %llx/%llx",
                          ok ? "AGREE" : "DISAGREE",
                          (unsigned long long)digest[0], (unsigned long long)digest[1],
                          (unsigned long long)tailh[0], (unsigned long long)tailh[1]);
        } else {
            kprintf("[CDBENCH] AGREEMENT: SKIPPED (no memory for the verify buffer)\n");
        }
    }

    // Leave the machine in the SHIPPED configuration whatever the arms did, or
    // a benchmark run would silently reconfigure the rest of the boot. The
    // config marker, if present, is re-applied by the caller.
    imgfile_readahead_set_disabled(0);
    isomemo_set_disabled(0);
    {
        uint32_t mz = 0;
        void *m = fat_read_file(&g_fat_fs, "/CONFIG/CDRAOFF.CFG", &mz);
        if (m) { kfree(m);
                 imgfile_readahead_set_disabled(1);
                 isomemo_set_disabled(1);
                 kprintf("[CDBENCH] /CONFIG/CDRAOFF.CFG present: readahead and "
                         "memo left OFF for this boot\n"); }
    }

    kfree(buf);
}
