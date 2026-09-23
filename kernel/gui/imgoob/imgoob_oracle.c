// imgoob_oracle.c - BMP decoder out-of-bounds regression oracle for the live
// image-load path (kernel/gui/image.c, bmp_decode_c). BUILD-CONTAINER ONLY:
// this is host user-space code, NOT part of kernel.elf. It is driven by
// imgoob-oracle.sh, which extracts the REAL, shipping bmp_decode_c() from
// kernel/gui/image.c into bmp_decode_c_real.inc and compiles it here as the
// GREEN arm, so the oracle can never silently rot away from the code it guards.
//
// WHY BMP: BMP is the most-exercised untrusted image decoder in the OS
// (wallpapers, BOOT.BMP, login background, UI assets) and image files arrive
// from sticks, downloads and the web, so a decoder over-read is a real
// trust-boundary bug. The LIVE dispatcher (bmp_decode, -DRUST_BMP default-on)
// routes to the Rust port; bmp_decode_c is the byte-identical C twin under a
// boot-time [RUST-DIFF] differential and the rollback arm (drop -DRUST_BMP).
// Both arms MUST reject the same malformed headers, so locking the C arm's
// bounds checks locks the contract the Rust twin is proven equal to.
//
// It proves, on the SAME crafted BMP files:
//   RED   = a faithful pre-hardening UNGUARDED BMP decode (no data_offset<len,
//           no data_offset+pixel_data_size<=len, no width*height capacity gate)
//           over-reads the source buffer (guard-page SIGSEGV).
//   GREEN = the real shipping bmp_decode_c() rejects every malformed header,
//           stays in bounds, AND still decodes a valid BMP to the exact
//           expected pixels (so the guards are proven to FIRE, not to be no-ops
//           that reject everything).
//
// Buffer backing: the BMP file bytes sit flush against a PROT_NONE guard page,
// so any source read at index >= len faults deterministically at a hard wall,
// regardless of the over-read distance. A guard page is strictly stronger and
// more deterministic than an AddressSanitizer redzone here (see imgoob-oracle.sh
// and the ext2oob precedent).
//
// No em-dashes anywhere by house style.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

// -------- image.h shims (exact packed on-disk layout) ----------------------
// Reproduced from kernel/gui/image.h so the extracted bmp_decode_c compiles
// unchanged. sizeof(bmp_file_header_t)==14 and sizeof(bmp_info_header_t)==40
// are what the extracted function relies on for its header offsets.
#define IMAGE_SUCCESS           0
#define IMAGE_ERR_NULL_PTR     -1
#define IMAGE_ERR_INVALID_SIG  -2
#define IMAGE_ERR_UNSUPPORTED  -3
#define IMAGE_ERR_NOMEM        -4
#define IMAGE_ERR_CORRUPT      -5
#define IMAGE_ERR_TOO_SMALL    -6

#define BMP_COMPRESSION_RGB       0
#define BMP_COMPRESSION_RLE8      1
#define BMP_COMPRESSION_RLE4      2
#define BMP_COMPRESSION_BITFIELDS 3
#define BMP_SIGNATURE 0x4D42  // "BM" little-endian

#pragma pack(push, 1)
typedef struct {
    uint16_t signature;
    uint32_t file_size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t data_offset;
} bmp_file_header_t;

typedef struct {
    uint32_t header_size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t image_size;
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_info_header_t;
#pragma pack(pop)

_Static_assert(sizeof(bmp_file_header_t) == 14, "file header must be 14 bytes");
_Static_assert(sizeof(bmp_info_header_t) == 40, "info header must be 40 bytes");

// GREEN arm: the REAL bmp_decode_c(), extracted verbatim from kernel/gui/image.c
// by imgoob-oracle.sh. If someone weakens the shipping bounds/overflow guards
// this file changes and the oracle goes RED.
#include "bmp_decode_c_real.inc"

// -------- RED arm: faithful pre-hardening UNGUARDED BMP decode --------------
// The historical shape before the bounds/overflow guards were added: it parses
// the header and walks the pixel data with NO check that the pixel-data offset
// is inside the file, NO check that data_offset + pixel_data_size fits in len,
// and NO width*height output-capacity gate. It keeps the same row-stride and
// per-pixel indexing as the shipping decoder, so the ONLY difference is the
// missing guards. On a truncated / mis-offset / overflowed BMP it reads the
// source pixel bytes past the buffer tail, into the guard page.
static int bmp_decode_unguarded(const uint8_t *buf, uint32_t len,
                                uint32_t *out_px, uint32_t out_cap_px,
                                uint32_t *out_w, uint32_t *out_h) {
    (void)out_cap_px;                          // pre-hardening: no capacity gate
    if (!buf || !out_w || !out_h) return IMAGE_ERR_NULL_PTR;
    *out_w = 0; *out_h = 0;

    if (len < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t))
        return IMAGE_ERR_TOO_SMALL;

    const bmp_file_header_t *file_hdr = (const bmp_file_header_t *)buf;
    const bmp_info_header_t *info_hdr =
        (const bmp_info_header_t *)(buf + sizeof(bmp_file_header_t));

    if (file_hdr->signature != BMP_SIGNATURE) return IMAGE_ERR_INVALID_SIG;

    int32_t width = info_hdr->width;
    int32_t height = info_hdr->height;
    bool top_down = false;
    if (height < 0) { height = -height; top_down = true; }
    if (width <= 0 || height <= 0) return IMAGE_ERR_CORRUPT; // keep loops finite

    uint32_t bytes_per_pixel = info_hdr->bpp / 8;
    if (bytes_per_pixel == 0) return IMAGE_ERR_UNSUPPORTED;
    uint32_t row_size_unpadded = (uint32_t)width * bytes_per_pixel;   // MAY WRAP
    uint32_t row_padding = (4 - (row_size_unpadded % 4)) % 4;
    uint32_t row_size = row_size_unpadded + row_padding;

    *out_w = (uint32_t)width;
    *out_h = (uint32_t)height;
    if (!out_px) return IMAGE_SUCCESS;

    // MISSING: data_offset >= len check.
    // MISSING: data_offset + pixel_data_size > len check.
    // MISSING: (u64)width*height > out_cap_px capacity gate.
    const uint8_t *src_data = buf + file_hdr->data_offset;
    for (int32_t y = 0; y < height; y++) {
        int32_t src_y = top_down ? y : (height - 1 - y);
        const uint8_t *src_row = src_data + (uint32_t)src_y * row_size;
        uint32_t *dst_row = out_px + (uint32_t)y * (uint32_t)width;
        for (int32_t x = 0; x < width; x++) {
            const uint8_t *src_pixel = src_row + (uint32_t)x * bytes_per_pixel;
            uint8_t b = src_pixel[0];         // <- over-reads into guard page
            uint8_t g = src_pixel[1];
            uint8_t r = src_pixel[2];
            dst_row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    return IMAGE_SUCCESS;
}

// -------- source buffer backing: BMP bytes flush against a guard page -------
#define PAGE 4096u
static uint8_t  *g_region_base = NULL;   // 2-page mapping, page 2 is PROT_NONE
static uint32_t  g_region_len  = 0;      // bytes of BMP placed in page 1

// Copy `len` BMP bytes so their LAST byte is the last byte of the first page;
// the second page is PROT_NONE, so buf[len + k] faults for any k >= 0.
static const uint8_t *guarded_copy(const uint8_t *bmp, uint32_t len) {
    if (len > PAGE) { fprintf(stderr, "fixture too big for one page\n"); exit(2); }
    if (!g_region_base) {
        g_region_base = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_region_base == MAP_FAILED) { perror("mmap"); exit(2); }
        if (mprotect(g_region_base + PAGE, PAGE, PROT_NONE) != 0) {
            perror("mprotect"); exit(2);
        }
    }
    uint8_t *dst = g_region_base + (PAGE - len);
    memcpy(dst, bmp, len);
    g_region_len = len;
    return dst;
}

// -------- BMP fixture builders ---------------------------------------------
// Each returns the total BMP length and fills *buf (a >=PAGE staging array).
// bpp is bits-per-pixel (24 or 32). data_offset/width/height are set per case.

static uint32_t put16(uint8_t *p, uint32_t off, uint16_t v) {
    p[off] = v & 0xFF; p[off + 1] = (v >> 8) & 0xFF; return off + 2;
}
static uint32_t put32(uint8_t *p, uint32_t off, uint32_t v) {
    p[off] = v & 0xFF; p[off + 1] = (v >> 8) & 0xFF;
    p[off + 2] = (v >> 16) & 0xFF; p[off + 3] = (v >> 24) & 0xFF; return off + 4;
}

// Write the 14-byte file header + 40-byte info header. Returns 54.
static uint32_t write_headers(uint8_t *p, uint32_t data_offset,
                              int32_t width, int32_t height, uint16_t bpp,
                              uint32_t compression) {
    uint32_t o = 0;
    o = put16(p, o, BMP_SIGNATURE);   // signature
    o = put32(p, o, 0);               // file_size (ignored)
    o = put16(p, o, 0);               // reserved1
    o = put16(p, o, 0);               // reserved2
    o = put32(p, o, data_offset);     // data_offset
    // info header (40 bytes)
    o = put32(p, o, 40);              // header_size
    o = put32(p, o, (uint32_t)width);
    o = put32(p, o, (uint32_t)height);
    o = put16(p, o, 1);               // planes
    o = put16(p, o, bpp);
    o = put32(p, o, compression);
    o = put32(p, o, 0);               // image_size
    o = put32(p, o, 0);               // x_ppm
    o = put32(p, o, 0);               // y_ppm
    o = put32(p, o, 0);               // colors_used
    o = put32(p, o, 0);               // colors_important
    return o;                         // == 54
}

// VALID: 2x2, 24bpp, bottom-up. Known pixels so GREEN correctness is checkable.
// Output pixels (0x00RRGGBB): (0,0)=red (0,1)=green (1,0)=blue (1,1)=white.
static const uint32_t VALID_EXPECT[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };
static uint32_t f_valid(uint8_t *buf) {
    memset(buf, 0, PAGE);
    uint32_t o = write_headers(buf, 54, 2, 2, 24, BMP_COMPRESSION_RGB);
    // 2x2, row_size_unpadded=6, padded to 8. bottom-up: file row0 = image bottom.
    // file row0 (image y=1): blue, white  ; BGR bytes + 2 pad
    uint32_t p = o;
    buf[p++] = 0xFF; buf[p++] = 0x00; buf[p++] = 0x00;  // blue  (b,g,r)
    buf[p++] = 0xFF; buf[p++] = 0xFF; buf[p++] = 0xFF;  // white
    buf[p++] = 0x00; buf[p++] = 0x00;                   // pad
    // file row1 (image y=0): red, green
    buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0xFF;  // red
    buf[p++] = 0x00; buf[p++] = 0xFF; buf[p++] = 0x00;  // green
    buf[p++] = 0x00; buf[p++] = 0x00;                   // pad
    return p; // 54 + 16 = 70
}

// MAL-A "bfOffBits past EOF": data_offset == len, so src_data points AT the
// guard page. GREEN rejects on data_offset >= len. RED reads src_data[0] -> fault.
static uint32_t f_mal_offset_past_eof(uint8_t *buf) {
    memset(buf, 0, PAGE);
    uint32_t len = 70;
    write_headers(buf, len, 4, 4, 24, BMP_COMPRESSION_RGB); // data_offset = len
    return len;
}

// MAL-B "truncated pixel data (bottom-up)": headers claim 64x64x24 (needs
// 64*192 = 12288 pixel bytes) but the file ends right after the header. GREEN
// rejects on data_offset + pixel_data_size > len. RED (bottom-up) reads image
// row 0 from the LAST source row, far past the tail -> fault on the first pixel.
static uint32_t f_mal_truncated(uint8_t *buf) {
    memset(buf, 0, PAGE);
    uint32_t len = 54 + 8;   // header + a few stray bytes, nowhere near 12288
    write_headers(buf, 54, 64, 64, 24, BMP_COMPRESSION_RGB);
    return len;
}

// MAL-C "dimension overflow": width chosen so (uint32)width*bpp WRAPS to a tiny
// value, making row_size and pixel_data_size tiny (data_offset+pixel_data_size
// passes the len check), while the real per-row read is enormous. width =
// 0x40000004, bpp=32 -> row_size_unpadded = 0x40000004*4 = 0x100000010 -> 16.
// height=1. GREEN rejects on the (u64)width*height > out_cap_px capacity gate.
// RED walks x=0.. reading 16-byte-strided source and marches into the guard page.
static uint32_t f_mal_dim_overflow(uint8_t *buf) {
    memset(buf, 0, PAGE);
    uint32_t len = 54 + 16;                    // tiny wrapped pixel_data_size fits
    write_headers(buf, 54, (int32_t)0x40000004, 1, 32, BMP_COMPRESSION_RGB);
    return len;
}

// MAL-D "absurd height, tiny file (bottom-up)": 16x100000x24 claims ~4.8 MB of
// pixels; the file is a few bytes. Same guard (data_offset+pixel_data_size>len)
// as MAL-B but a different attacker shape (a lie in the height field rather than
// a genuinely truncated download). RED reads image row 0 from source row 99999.
static uint32_t f_mal_absurd_height(uint8_t *buf) {
    memset(buf, 0, PAGE);
    uint32_t len = 54 + 12;
    write_headers(buf, 54, 16, 100000, 24, BMP_COMPRESSION_RGB);
    return len;
}

typedef uint32_t (*builder_t)(uint8_t *);
typedef int (*decoder_t)(const uint8_t *, uint32_t, uint32_t *, uint32_t,
                         uint32_t *, uint32_t *);

struct testcase { const char *name; builder_t build; };

// Output buffer for both arms. RED faults on the SOURCE read after writing at
// most a handful of output pixels (0 for the truncated/offset cases, 4 for the
// overflow case), so a modest fixed capacity cannot mask the source over-read.
#define OUT_CAP_PX 4096u
static uint32_t g_out[OUT_CAP_PX];

// Run one decoder on one built fixture in a forked child so a RED guard-page
// fault does not take down the oracle. alarm() catches any runaway. Exit 0 =
// returned safely; killed by SIGSEGV/SIGBUS = source over-read.
static void run_child(decoder_t fn, builder_t build) {
    static uint8_t staging[PAGE];
    uint32_t len = build(staging);
    const uint8_t *bmp = guarded_copy(staging, len);
    alarm(3);
    uint32_t w = 0, h = 0;
    volatile int rc = fn(bmp, len, g_out, OUT_CAP_PX, &w, &h);
    (void)rc;
    _exit(0);
}

static const char *verdict(int status, int *safe) {
    *safe = 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        *safe = 1; return "SAFE (returned in bounds)";
    }
    if (WIFSIGNALED(status)) {
        int s = WTERMSIG(status);
        if (s == SIGSEGV || s == SIGBUS) return "OVER-READ (guard-page fault)";
        if (s == SIGALRM) return "RUNAWAY (SIGALRM)";
        return "killed by other signal";
    }
    return "unknown";
}

static int run_case(decoder_t fn, struct testcase *tc) {
    pid_t pid = fork();
    if (pid == 0) { run_child(fn, tc->build); }
    int status = 0; waitpid(pid, &status, 0);
    int safe = 0;
    const char *v = verdict(status, &safe);
    printf("      %-26s -> %s\n", tc->name, v);
    return safe;
}

// GREEN must also REJECT (return negative) each malicious fixture, run in-process
// (GREEN never faults). Proves it does not merely avoid the fault by luck.
static int green_rejects(struct testcase *tc) {
    static uint8_t staging[PAGE];
    uint32_t len = tc->build(staging);
    uint32_t w = 0, h = 0;
    int rc = bmp_decode_c(staging, len, g_out, OUT_CAP_PX, &w, &h);
    printf("      %-26s -> rc=%d %s\n", tc->name, rc,
           rc == IMAGE_SUCCESS ? "(ACCEPTED - BUG)" : "(rejected)");
    return rc != IMAGE_SUCCESS;
}

// GREEN correctness on the VALID BMP: it must decode to the exact expected
// pixels (guards are load-bearing, not no-ops that reject everything).
static int green_valid_correctness(void) {
    static uint8_t staging[PAGE];
    uint32_t len = f_valid(staging);
    memset(g_out, 0xAB, sizeof(g_out));
    uint32_t w = 0, h = 0;
    int rc = bmp_decode_c(staging, len, g_out, OUT_CAP_PX, &w, &h);
    int ok = (rc == IMAGE_SUCCESS) && (w == 2) && (h == 2);
    for (int i = 0; i < 4 && ok; i++) {
        if ((g_out[i] & 0x00FFFFFF) != VALID_EXPECT[i]) ok = 0;
    }
    printf("      valid 2x2 BMP decode       -> rc=%d w=%u h=%u px=[%06X %06X %06X %06X] %s\n",
           rc, w, h, g_out[0] & 0xFFFFFF, g_out[1] & 0xFFFFFF,
           g_out[2] & 0xFFFFFF, g_out[3] & 0xFFFFFF, ok ? "OK" : "WRONG");
    return ok;
}

int main(void) {
    static struct testcase cases[4] = {
        { "MAL-A offset-past-eof",  f_mal_offset_past_eof },
        { "MAL-B truncated-pixels", f_mal_truncated },
        { "MAL-C dimension-overflow", f_mal_dim_overflow },
        { "MAL-D absurd-height",    f_mal_absurd_height },
    };
    int n = 4;

    printf("=== BMP decoder over-read oracle (backing: mmap guard-page) ===\n");

    printf("\n[RED]  pre-hardening UNGUARDED bmp decode (must over-read):\n");
    int red_bad = 0;
    for (int i = 0; i < n; i++)
        if (!run_case(bmp_decode_unguarded, &cases[i])) red_bad++;

    printf("\n[GREEN] real shipping bmp_decode_c() (must stay in bounds):\n");
    int green_all_safe = 1;
    for (int i = 0; i < n; i++)
        if (!run_case(bmp_decode_c, &cases[i])) green_all_safe = 0;

    printf("\n[GREEN] real bmp_decode_c() must REJECT each malicious BMP:\n");
    int green_all_reject = 1;
    for (int i = 0; i < n; i++)
        if (!green_rejects(&cases[i])) green_all_reject = 0;

    printf("\n[GREEN] guards are not no-ops (valid BMP must still decode right):\n");
    int green_correct = green_valid_correctness();

    printf("\n=== summary ===\n");
    printf("  RED  over-read on %d of %d malicious BMPs\n", red_bad, n);
    printf("  GREEN safe on all malicious BMPs   : %s\n", green_all_safe ? "YES" : "NO");
    printf("  GREEN rejected all malicious BMPs  : %s\n", green_all_reject ? "YES" : "NO");
    printf("  GREEN correct on the valid BMP     : %s\n", green_correct ? "YES" : "NO");

    // PASS requires: RED demonstrably over-read on all four crafted BMPs (the
    // guards are load-bearing), GREEN stayed in bounds and rejected every one,
    // and GREEN still decoded the valid BMP to the exact pixels.
    int pass = (red_bad == n) && green_all_safe && green_all_reject && green_correct;
    printf("\nORACLE: %s\n",
           pass ? "PASS (RED over-reads, GREEN safe + rejects + correct)" : "FAIL");
    return pass ? 0 : 1;
}
