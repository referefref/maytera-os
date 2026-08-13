// diskimg_test.c - #196 proof code: the Rust/C ISO 9660 differential and the
// live mount/list/read/eject/swap boot harness.
//
// Kept out of diskimg.c so the production reader stays readable, and so that
// "does the parser agree" and "does a real disc mount" are two separate,
// separately-failing things.
#include "diskimg.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern

extern fat_fs_t g_fat_fs;
extern void *fat_read_file(fat_fs_t *fs, const char *path, unsigned int *size_out);
extern void *kmalloc(unsigned long n);
extern void  kfree(void *p);

static void log_line(const char *s) {
    kprintf("%s\n", s);
    bootlog_write(s);
    bootlog_write("\n");
}

// ===========================================================================
// PART 1: Rust vs C differential for the three ISO 9660 parse entry points.
//
// Both arms are compiled into every build (the live routing picks one; this
// calls BOTH explicitly), so this proves agreement on THIS build, not on a
// build someone else made. It deliberately includes malformed vectors: the
// whole reason the parser is in Rust is what happens on bad input, and a
// differential over well-formed input only would prove nothing about that.
//
// HONEST LIMIT, recorded because this tree has been burned by it (#490 class):
// a differential cannot catch a bug BOTH arms share. It proves the port is
// faithful, not that the format handling is right. The format handling is
// proved separately, by the harness below reading a real disc and matching a
// host-computed checksum.
// ===========================================================================

static uint32_t xs_state;
static uint32_t xs_next(void) {
    uint32_t x = xs_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    xs_state = x;
    return x;
}

static int vol_eq(const iso_vol_t *a, const iso_vol_t *b) {
    return a->root_lba == b->root_lba && a->root_len == b->root_len &&
           a->block_size == b->block_size && a->kind == b->kind &&
           a->joliet_ucs == b->joliet_ucs &&
           // The volume identifier is compared too, or the differential would
           // be BLIND to the field it was just given: an equivalence test only
           // covers what it looks at, and a new field nobody compares is a new
           // field nobody tests.
           memcmp(a->volid, b->volid, sizeof a->volid) == 0;
}
static int rec_eq(const iso_dirrec_t *a, const iso_dirrec_t *b) {
    return a->next == b->next && a->lba == b->lba && a->len == b->len &&
           a->is_dir == b->is_dir && a->multi == b->multi &&
           a->name_off == b->name_off && a->name_len == b->name_len;
}

// Build a plausible volume descriptor of the given type into a 2048-byte sector.
static void mk_vd(uint8_t *s, uint8_t type, const char *esc,
                  uint32_t bs, uint32_t root_lba, uint32_t root_len) {
    memset(s, 0, 2048);
    s[0] = type;
    s[1] = 'C'; s[2] = 'D'; s[3] = '0'; s[4] = '0'; s[5] = '1';
    s[6] = 1;
    if (esc) { s[88] = (uint8_t)esc[0]; s[89] = (uint8_t)esc[1]; s[90] = (uint8_t)esc[2]; }
    s[128] = (uint8_t)(bs & 0xFF); s[129] = (uint8_t)(bs >> 8);
    s[156] = 34;
    s[156 + 2] = (uint8_t)(root_lba); s[156 + 3] = (uint8_t)(root_lba >> 8);
    s[156 + 4] = (uint8_t)(root_lba >> 16); s[156 + 5] = (uint8_t)(root_lba >> 24);
    s[156 + 10] = (uint8_t)(root_len); s[156 + 11] = (uint8_t)(root_len >> 8);
    s[156 + 12] = (uint8_t)(root_len >> 16); s[156 + 13] = (uint8_t)(root_len >> 24);
    s[156 + 25] = 0x02;
    s[156 + 32] = 1;
}

// Write a directory record at `at`. Returns the record length used.
static uint32_t mk_rec(uint8_t *b, uint32_t at, uint32_t lba, uint32_t len,
                       uint8_t flags, const char *name, uint32_t namelen,
                       int corrupt_reclen) {
    uint32_t reclen = 33 + namelen;
    if (reclen & 1) reclen++;             // records are even-padded
    b[at] = corrupt_reclen ? (uint8_t)corrupt_reclen : (uint8_t)reclen;
    b[at + 2] = (uint8_t)lba; b[at + 3] = (uint8_t)(lba >> 8);
    b[at + 4] = (uint8_t)(lba >> 16); b[at + 5] = (uint8_t)(lba >> 24);
    b[at + 10] = (uint8_t)len; b[at + 11] = (uint8_t)(len >> 8);
    b[at + 12] = (uint8_t)(len >> 16); b[at + 13] = (uint8_t)(len >> 24);
    b[at + 25] = flags;
    b[at + 32] = (uint8_t)namelen;
    for (uint32_t i = 0; i < namelen; i++) b[at + 33 + i] = (uint8_t)name[i];
    return reclen;
}

void diskimg_iso_rust_selftest(void) {
    uint8_t *sec = (uint8_t *)kmalloc(2048);
    uint8_t *dir = (uint8_t *)kmalloc(2048);
    if (!sec || !dir) {
        if (sec) kfree(sec);
        if (dir) kfree(dir);
        log_line("[RUST-DIFF] iso9660: SKIPPED (no memory)");
        return;
    }

    unsigned long vectors = 0, mismatch = 0;
    unsigned long m_vd = 0, m_rec = 0, m_name = 0;

    // ---- volume descriptors: hand-built cases -----------------------------
    struct { uint8_t ty; const char *esc; uint32_t bs, lba, len; } vds[] = {
        { 1,   0,    2048, 23, 2048 },      // ordinary PVD
        { 2,   "%/@", 2048, 40, 4096 },     // Joliet level 1 (both target discs)
        { 2,   "%/C", 2048, 41, 2048 },     // Joliet level 2
        { 2,   "%/E", 2048, 42, 2048 },     // Joliet level 3
        { 2,   "%/X", 2048, 43, 2048 },     // supplementary, NOT Joliet -> skip
        { 2,   0,    2048, 44, 2048 },      // supplementary, no escape   -> skip
        { 0,   0,    2048, 45, 2048 },      // boot record                -> skip
        { 3,   0,    2048, 46, 2048 },      // partition descriptor       -> skip
        { 255, 0,    2048, 47, 2048 },      // terminator                 -> skip
        { 1,   0,    512,  23, 2048 },      // 512-byte blocks            -> skip
        { 1,   0,    2048, 23, 0 },         // zero-length root           -> skip
    };
    for (unsigned i = 0; i < sizeof(vds) / sizeof(vds[0]); i++) {
        mk_vd(sec, vds[i].ty, vds[i].esc, vds[i].bs, vds[i].lba, vds[i].len);
        iso_vol_t a, b;
        memset(&a, 0xAA, sizeof a); memset(&b, 0xBB, sizeof b);
        int ra = iso_vd_parse_c(sec, 2048, &a);
        int rb = iso_vd_parse_rs(sec, 2048, &b);
        vectors++;
        if (ra != rb || (ra == 1 && !vol_eq(&a, &b))) { mismatch++; m_vd++; }
    }
    // bad magic, and every truncation from 0 to 200 bytes
    mk_vd(sec, 1, 0, 2048, 23, 2048);
    sec[3] = 'X';
    { iso_vol_t a, b; int ra = iso_vd_parse_c(sec, 2048, &a), rb = iso_vd_parse_rs(sec, 2048, &b);
      vectors++; if (ra != rb) { mismatch++; m_vd++; } }
    mk_vd(sec, 1, 0, 2048, 23, 2048);
    for (uint32_t n = 0; n <= 200; n++) {
        iso_vol_t a, b;
        memset(&a, 0xAA, sizeof a); memset(&b, 0xBB, sizeof b);
        int ra = iso_vd_parse_c(sec, n, &a);
        int rb = iso_vd_parse_rs(sec, n, &b);
        vectors++;
        if (ra != rb || (ra == 1 && !vol_eq(&a, &b))) { mismatch++; m_vd++; }
    }

    // ---- directory records: hand-built cases ------------------------------
    memset(dir, 0, 2048);
    uint32_t at = 0;
    at += mk_rec(dir, at, 0, 2048, 0x02, "\0", 1, 0);          // "." self
    at += mk_rec(dir, at, 0, 2048, 0x02, "\1", 1, 0);          // ".." parent
    at += mk_rec(dir, at, 100, 4096, 0x02, "SUBDIR", 6, 0);
    at += mk_rec(dir, at, 200, 24366, 0x00, "README.TXT;1", 12, 0);
    at += mk_rec(dir, at, 300, 0x1B1B1B1B, 0x80, "BIG.MIX;1", 9, 0);  // multi-extent
    // corrupted tail cases, each parsed at its own position
    uint32_t corrupt_at = at;
    mk_rec(dir, corrupt_at, 400, 10, 0x00, "SHORT", 5, 20);    // reclen 20 (< 33)
    uint32_t corrupt2 = 1024;
    mk_rec(dir, corrupt2, 500, 10, 0x00, "OVER", 4, 250);      // reclen 250, namelen 4
    dir[1200] = 40; dir[1200 + 32] = 200;                      // namelen past reclen
    dir[1400] = 200;                                           // reclen runs past 2048? no
    dir[2040] = 60;                                            // record straddles the end

    for (uint32_t pos = 0; pos < 2048; pos++) {
        iso_dirrec_t a, b;
        memset(&a, 0xAA, sizeof a); memset(&b, 0xBB, sizeof b);
        int ra = iso_dirrec_at_c(dir, 2048, pos, &a);
        int rb = iso_dirrec_at_rs(dir, 2048, pos, &b);
        vectors++;
        if (ra != rb || (ra == 1 && !rec_eq(&a, &b))) { mismatch++; m_rec++; }
    }
    // short buffers (a partial sector), every length
    for (uint32_t n = 0; n <= 260; n++) {
        iso_dirrec_t a, b;
        memset(&a, 0xAA, sizeof a); memset(&b, 0xBB, sizeof b);
        int ra = iso_dirrec_at_c(dir, n, 0, &a);
        int rb = iso_dirrec_at_rs(dir, n, 0, &b);
        vectors++;
        if (ra != rb || (ra == 1 && !rec_eq(&a, &b))) { mismatch++; m_rec++; }
    }

    // ---- name decode: hand-built cases ------------------------------------
    {
        struct { const char *raw; uint32_t len; int joliet; uint32_t cap; } nms[] = {
            { "README.TXT;1", 12, 0, 64 },
            { "MAIN.MIX;1",   10, 0, 64 },
            { "SUBDIR",        6, 0, 64 },
            { "NOEXT.;1",      8, 0, 64 },     // trailing dot after version strip
            { "\0",            1, 0, 64 },     // self
            { "\1",            1, 0, 64 },     // parent
            { "A",             1, 0, 2  },     // cap exactly fits
            { "ABCDEFGH",      8, 0, 4  },     // truncation
            { "ABCDEFGH",      8, 0, 1  },     // cap 1: only the NUL fits
            { "\0R\0E\0A\0D",  8, 1, 64 },     // Joliet UCS-2BE "READ"
            { "\0R\0E\0A",     5, 1, 64 },     // odd length: trailing half pair
            { "\1\2\3\4",      4, 1, 64 },     // non-ASCII code points
            { "A\tB\nC",       5, 0, 64 },     // control bytes
            { "",              0, 0, 64 },
        };
        for (unsigned i = 0; i < sizeof(nms) / sizeof(nms[0]); i++) {
            uint8_t oa[80], ob[80];
            memset(oa, 0xAA, sizeof oa); memset(ob, 0xAA, sizeof ob);
            int ra = iso_name_decode_c((const uint8_t *)nms[i].raw, nms[i].len,
                                       nms[i].joliet, oa, nms[i].cap);
            int rb = iso_name_decode_rs((const uint8_t *)nms[i].raw, nms[i].len,
                                        nms[i].joliet, ob, nms[i].cap);
            vectors++;
            if (ra != rb) { mismatch++; m_name++; continue; }
            if (memcmp(oa, ob, sizeof oa) != 0) { mismatch++; m_name++; }
        }
    }

    // ---- fuzz: random bytes through all three -----------------------------
    xs_state = 0x196C0DE5u;
    for (unsigned iter = 0; iter < 4000; iter++) {
        for (unsigned i = 0; i < 2048; i += 4) {
            uint32_t r = xs_next();
            sec[i] = (uint8_t)r; sec[i+1] = (uint8_t)(r>>8);
            sec[i+2] = (uint8_t)(r>>16); sec[i+3] = (uint8_t)(r>>24);
        }
        // Half the iterations keep a valid signature so the fuzz reaches past
        // the magic check instead of always bouncing off it.
        if (iter & 1) {
            sec[1]='C'; sec[2]='D'; sec[3]='0'; sec[4]='0'; sec[5]='1';
            sec[0] = (uint8_t)(xs_next() % 4);
        }
        {
            iso_vol_t a, b;
            memset(&a, 0xAA, sizeof a); memset(&b, 0xBB, sizeof b);
            int ra = iso_vd_parse_c(sec, 2048, &a);
            int rb = iso_vd_parse_rs(sec, 2048, &b);
            vectors++;
            if (ra != rb || (ra == 1 && !vol_eq(&a, &b))) mismatch++;
        }
        {
            uint32_t pos = xs_next() % 2048;
            iso_dirrec_t a, b;
            memset(&a, 0xAA, sizeof a); memset(&b, 0xBB, sizeof b);
            int ra = iso_dirrec_at_c(sec, 2048, pos, &a);
            int rb = iso_dirrec_at_rs(sec, 2048, pos, &b);
            vectors++;
            if (ra != rb || (ra == 1 && !rec_eq(&a, &b))) mismatch++;
        }
        {
            uint32_t off = xs_next() % 1900;
            uint32_t len = xs_next() % 128;
            uint32_t cap = 1 + (xs_next() % 80);
            int jol = (int)(xs_next() & 1);
            uint8_t oa[96], ob[96];
            memset(oa, 0xAA, sizeof oa); memset(ob, 0xAA, sizeof ob);
            int ra = iso_name_decode_c(sec + off, len, jol, oa, cap);
            int rb = iso_name_decode_rs(sec + off, len, jol, ob, cap);
            vectors++;
            if (ra != rb) { mismatch++; m_name++; }
            else if (memcmp(oa, ob, sizeof oa) != 0) { mismatch++; m_name++; }
        }
    }

    kfree(sec);
    kfree(dir);

    char line[128];
    snprintf(line, sizeof line,
             "[RUST-DIFF] iso9660: %lu vectors, %lu mismatches "
             "(vd=%lu rec=%lu name=%lu) -> %s",
             vectors, mismatch, m_vd, m_rec, m_name, mismatch ? "FAIL" : "PASS");
    log_line(line);
}

// ===========================================================================
// PART 2: live mount / list / read / EJECT / re-mount harness.
//
// Driven by /CDTEST.TXT on the root filesystem so it costs nothing on a normal
// boot (absent file = immediate return) and so the images it names are never
// baked into the kernel. This is the same shape as the other boot markers this
// tree already uses (/TORAMOFF.TXT, /ROOTEXT2, /CONFIG/WIN16PM.RUN).
//
// One line per disc:
//     <image path>|<file inside the image>|<offset>|<length>
// length 0 means "to the end of the file". Each line MOUNTS on E: (which ejects
// whatever the previous line mounted), lists the root directory, and checksums
// the named range. Two lines therefore prove a live disc swap: no reboot
// happens between them.
//
// The checksum is FNV-1a 64-bit, chosen because it is three lines of code here
// and one line of Python on the host, so "byte-correct against the host" is a
// comparison of two numbers rather than an assertion of trust.
// ===========================================================================

#define FNV64_OFF 1469598103934665603ULL
#define FNV64_PRM 1099511628211ULL

struct listctx { int shown; int total; };
static void harness_list_cb(const char *name, int is_dir, unsigned int size, void *ud) {
    struct listctx *c = (struct listctx *)ud;
    c->total++;
    // Cap the printed entries: a root directory can be long and this is boot
    // serial output, not a file manager.
    if (c->shown < 40) {
        kprintf("[CDTEST]   %-16s %s %u\n", name, is_dir ? "DIR " : "FILE", size);
        c->shown++;
    }
}

// Copy up to cap-1 bytes of the field starting at *p and ending at '|' or the
// end of line. Advances *p past the separator. Returns 1 if a field was taken.
static int take_field(const char **p, char *out, int cap) {
    const char *s = *p;
    if (!*s || *s == '\n' || *s == '\r') return 0;
    int n = 0;
    while (*s && *s != '|' && *s != '\n' && *s != '\r') {
        if (n < cap - 1) out[n++] = *s;
        s++;
    }
    out[n] = 0;
    if (*s == '|') s++;
    *p = s;
    return 1;
}

static uint64_t parse_u64(const char *s) {
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
    return v;
}

void diskimg_boot_harness(void) {
    unsigned int sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CDTEST.TXT", &sz);
    if (!cfg || sz == 0) { if (cfg) kfree(cfg); return; }

    log_line("[CDTEST] #196 disk-image harness: /CDTEST.TXT found");

    uint8_t *chunk = (uint8_t *)kmalloc(65536);
    if (!chunk) { kfree(cfg); log_line("[CDTEST] no memory for read buffer"); return; }

    const char *p = cfg;
    const char *end = cfg + sz;
    int disc = 0, keep = 0;
    while (p < end) {
        // Skip blank lines and comments.
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        if (*p == '#') { while (p < end && *p != '\n') p++; continue; }

        char imgpath[160], relpath[160], offs[32], lens[32];
        const char *lp = p;
        if (!take_field(&lp, imgpath, sizeof imgpath)) break;
        // "KEEP" as the whole line means: stop here and leave the last image
        // mounted, so a later stage (the deferred DOS launch from
        // /CONFIG/DOSRUN.CFG) still has a disc in the drive. Without it the
        // harness ejects, which is the right default but makes a DOS-guest
        // test impossible.
        if (imgpath[0] == 'K' && imgpath[1] == 'E' && imgpath[2] == 'E' &&
            imgpath[3] == 'P' && imgpath[4] == 0) {
            while (lp < end && *lp != '\n') lp++;
            p = lp;
            keep = 1;
            continue;
        }
        if (!take_field(&lp, relpath, sizeof relpath)) relpath[0] = 0;
        if (!take_field(&lp, offs, sizeof offs)) offs[0] = 0;
        if (!take_field(&lp, lens, sizeof lens)) lens[0] = 0;
        while (lp < end && *lp != '\n') lp++;
        p = lp;
        disc++;

        kprintf("[CDTEST] --- disc %d: mounting %s on E: ---\n", disc, imgpath);
        int mr = diskimg_mount('E', imgpath);
        if (mr != 0) {
            kprintf("[CDTEST] MOUNT FAILED rc=%d\n", mr);
            continue;
        }
        kprintf("[CDTEST] mounted='%s' fmt=%d size=%llu joliet=%d\n",
                diskimg_mounted_name('E'), diskimg_format('E'),
                (unsigned long long)diskimg_image_size('E'),
                diskimg_has_joliet('E'));

        struct listctx lc = { 0, 0 };
        int n = diskimg_listdir('E', "/", harness_list_cb, &lc);
        kprintf("[CDTEST] root listing: %d entries (shown %d)\n", n, lc.shown);

        // The GUEST path. Everything above went through diskimg_* directly;
        // this goes through fat_open()/fat_readdir_n()/fat_seek()/fat_read() on
        // a "/WINDIR/DRIVE_E/..." path, which is EXACTLY what DOS INT 21h
        // 3Dh/3Fh/42h/4Eh and the Win16 KERNEL file APIs call. Proving the
        // diskimg layer works proves nothing about guest visibility; this does.
        {
            fat_file_t fd;
            int dents = 0;
            if (fat_open(&g_fat_fs, "/WINDIR/DRIVE_E", &fd) == 0) {
                fat_dir_entry_t de; char nm[128];
                while (fat_readdir_n(&fd, &de, nm, sizeof nm) == 0 && dents < 4096) dents++;
                fat_close(&fd);
            }
            kprintf("[CDTEST] fat_open(/WINDIR/DRIVE_E) readdir -> %d entries\n", dents);
        }
        if (relpath[0]) {
            char gpath[224];
            int gp = 0;
            const char *gp1 = "/WINDIR/DRIVE_E/";
            for (int i = 0; gp1[i] && gp < (int)sizeof gpath - 1; i++) gpath[gp++] = gp1[i];
            for (int i = 0; relpath[i] && gp < (int)sizeof gpath - 1; i++) gpath[gp++] = relpath[i];
            gpath[gp] = 0;
            fat_file_t ff;
            if (fat_open(&g_fat_fs, gpath, &ff) != 0) {
                kprintf("[CDTEST] GUEST fat_open('%s') FAILED\n", gpath);
            } else {
                uint64_t goff = parse_u64(offs);
                uint64_t glen = parse_u64(lens);
                uint32_t fsz32 = fat_size(&ff);
                if (glen == 0) glen = (goff < fsz32) ? (fsz32 - goff) : 0;
                fat_seek(&ff, (uint32_t)goff);
                uint64_t gh = FNV64_OFF, gdone = 0;
                while (gdone < glen) {
                    uint32_t want = (glen - gdone > 65536) ? 65536u : (uint32_t)(glen - gdone);
                    int got = fat_read(&ff, chunk, want);
                    if (got <= 0) break;
                    for (int i = 0; i < got; i++) { gh ^= (uint64_t)chunk[i]; gh *= FNV64_PRM; }
                    gdone += (uint64_t)got;
                }
                fat_close(&ff);
                kprintf("[CDTEST] GUEST fat_open('%s') size=%u read=%llu fnv1a64=%llx\n",
                        gpath, fsz32, (unsigned long long)gdone,
                        (unsigned long long)gh);
            }
        }

        if (relpath[0]) {
            uint64_t fsz = 0; int isdir = 0;
            if (!diskimg_stat('E', relpath, &fsz, &isdir)) {
                kprintf("[CDTEST] STAT MISS %s\n", relpath);
            } else {
                uint64_t off = parse_u64(offs);
                uint64_t len = parse_u64(lens);
                if (len == 0) len = (off < fsz) ? (fsz - off) : 0;
                kprintf("[CDTEST] %s size=%llu isdir=%d; checksumming %llu bytes at %llu\n",
                        relpath, (unsigned long long)fsz, isdir,
                        (unsigned long long)len, (unsigned long long)off);

                uint64_t h = FNV64_OFF, done = 0;
                int failed = 0;
                while (done < len) {
                    uint64_t want = len - done;
                    if (want > 65536) want = 65536;
                    int64_t got = diskimg_read_range('E', relpath, off + done, want, chunk);
                    if (got <= 0) { failed = 1; break; }
                    for (int64_t i = 0; i < got; i++) {
                        h ^= (uint64_t)chunk[i];
                        h *= FNV64_PRM;
                    }
                    done += (uint64_t)got;
                }
                if (failed)
                    kprintf("[CDTEST] READ FAILED after %llu bytes\n",
                            (unsigned long long)done);
                else
                    kprintf("[CDTEST] READ OK bytes=%llu fnv1a64=%llx\n",
                            (unsigned long long)done, (unsigned long long)h);
                char bl[160];
                snprintf(bl, sizeof bl, "[CDTEST] disc %d %s bytes=%llu fnv=%llx",
                         disc, relpath, (unsigned long long)done,
                         (unsigned long long)h);
                bootlog_write(bl); bootlog_write("\n");
            }
        }
    }

    // Default: leave the drive empty. The harness proves eject works and does
    // not leave a test disc mounted into the running system. A trailing KEEP
    // line overrides that for the DOS-guest stage.
    if (!keep) diskimg_eject('E');
    kprintf("[CDTEST] done, %d disc(s); keep=%d; E: mounted=%d name='%s'\n",
            disc, keep, diskimg_is_mounted('E'), diskimg_mounted_name('E'));
    kfree(chunk);
    kfree(cfg);
}
