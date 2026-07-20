// jpeg_selftest.c - Gated in-OS JPEG decoder self-test (#332).
//
// No-op unless /CONFIG/JPGTEST.CFG exists on the boot disk. When armed, a
// deferred kernel worker decodes a small embedded baseline JPEG (16x16, 4:2:0)
// through the REAL kernel decode path (image_load_jpeg) and prints the result,
// dimensions and a checksum to serial. This proves gui/jpeg.c no longer returns
// -1 for baseline JPEGs (the #332 regression). Mirrors the audio/devinfo gated
// self-tests.

#include "jpeg.h"
#include "image.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"

// Externs provided by other subsystems (see proc/devinfo.c for the pattern).
extern int  proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                           int priority, uint32_t stack_size);
extern void proc_sleep(uint32_t ms);
extern void kprintf_set_dual_output(int on);
extern fat_fs_t g_fat_fs;

// 16x16 baseline JPEG (4:2:0), generated with ffmpeg (yuvj420p, q5). 435 bytes.
static const unsigned char jpeg_selftest_img[] = {
255,216,255,224,0,16,74,70,73,70,0,1,2,0,0,1,0,1,0,0,255,254,0,16,76,97,118,99,
53,57,46,51,55,46,49,48,48,0,255,219,0,67,0,8,10,10,11,10,11,13,13,13,13,13,13,
16,15,16,16,16,16,16,16,16,16,16,16,16,18,18,18,21,21,21,18,18,18,16,16,18,18,20,
20,21,21,23,23,23,21,21,21,21,23,23,25,25,25,30,30,28,28,35,35,36,43,43,51,255,
196,0,109,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,6,3,5,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,1,2,16,0,3,0,2,2,1,4,2,3,1,0,0,0,0,0,0,3,2,1,4,17,5,18,0,6,33,49,19,65,66,20,
35,50,34,17,0,1,3,2,7,1,1,1,0,0,0,0,0,0,0,0,1,4,17,3,18,2,20,33,5,19,34,50,35,0,
68,65,255,192,0,17,8,0,16,0,16,3,1,34,0,2,17,0,3,17,0,255,218,0,12,3,1,0,2,17,3,
17,0,63,0,51,113,143,194,240,56,132,17,156,70,56,147,34,144,36,117,177,75,80,131,
88,210,37,150,12,186,121,63,107,102,236,215,141,185,126,77,61,47,200,8,248,225,16,
192,164,79,184,34,18,72,195,191,90,191,69,148,114,60,87,118,79,250,147,190,187,
110,121,149,146,60,62,79,132,193,95,231,99,138,8,24,248,236,221,145,228,250,144,
105,217,191,177,122,246,163,190,215,227,122,221,215,145,245,38,62,111,57,150,124,
100,196,200,31,93,105,148,100,47,111,241,125,150,34,254,19,127,55,218,249,51,50,
241,166,221,167,101,176,86,99,127,57,184,248,51,137,40,51,183,62,181,210,255,0,
199,248,140,139,214,166,70,165,140,246,226,119,4,141,65,201,104,73,232,124,239,
32,20,187,92,141,52,216,221,50,255,217
};

static void jpeg_selftest_worker(void *arg) {
    (void)arg;
    proc_sleep(12000);   // let drivers + desktop settle
    uint32_t cfgsz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/JPGTEST.CFG", &cfgsz);
    if (!cfg) return;    // no flag -> silent no-op
    kfree(cfg);

    kprintf_set_dual_output(1);
    kprintf("\n========== JPEG DECODER SELFTEST (#332) ==========\n");
    kprintf("[JPGTEST] embedded baseline JPEG: %u bytes, is_jpeg=%d\n",
            (unsigned)sizeof(jpeg_selftest_img),
            image_is_jpeg(jpeg_selftest_img, sizeof(jpeg_selftest_img)));

    image_t img;
    img.pixels = 0; img.width = 0; img.height = 0;
    int r = image_load_jpeg(jpeg_selftest_img, sizeof(jpeg_selftest_img), &img);

    if (r == JPEG_SUCCESS && img.pixels && img.width && img.height) {
        uint32_t sum = 2166136261u;     // FNV-1a-ish rolling checksum
        for (uint32_t i = 0; i < img.width * img.height; i++) {
            sum ^= img.pixels[i];
            sum *= 16777619u;
        }
        kprintf("[JPGTEST] decode OK: ret=%d w=%u h=%u checksum=0x%08x\n",
                r, img.width, img.height, sum);
        kprintf("[JPGTEST] RESULT: PASS (no longer returns -1)\n");
        kfree(img.pixels);
    } else {
        kprintf("[JPGTEST] decode FAILED: ret=%d (%s) w=%u h=%u\n",
                r, jpeg_error_string(r), img.width, img.height);
        kprintf("[JPGTEST] RESULT: FAIL\n");
        if (img.pixels) kfree(img.pixels);
    }
    kprintf("========== JPEG DECODER SELFTEST: DONE ==========\n");
    kprintf_set_dual_output(0);
}

void jpeg_start_deferred_selftest(void) {
    proc_create_ex("jpgtest", jpeg_selftest_worker, 0, 1, 256 * 1024);
}
