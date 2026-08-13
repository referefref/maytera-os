/* main_maytera.c - MayteraOS Ring-3 driver for the offscreen graphics harness
 * (#28, graphics lane).
 *
 * PURPOSE: prove the ClassiCube software rasteriser compiles, links and runs
 * inside the REAL MayteraOS userland compilation model, using the SAME
 * gfxtest_scene.c / gfxtest_support.c the build-server harness runs. It needs
 * no window and no compositor: it rasterises into a buffer we own, checks
 * pixels numerically, and writes a BMP to disk.
 *
 * FLOAT NOTE (measured, not assumed): the soft-float / -mno-sse restriction is
 * a KERNEL constraint (kernel/Makefile:142-143). Ring-3 apps are the opposite:
 * SSE2 is the x86-64 ABI baseline, and compiling a float expression with the
 * plain userland flag set and NO -msse emits xmm instructions. The rasteriser's
 * floats are therefore hardware floats here, exactly as on the build server.
 *
 * This deliberately does NOT create a window. Presenting to a real window is
 * the window lane's job; see gfx/mos_gfx.h section 2 for the three functions
 * it must supply to replace the offscreen stand-in in gfxtest_support.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "Core.h"
#include "Bitmap.h"
#include "Platform.h"
#include "gfxtest.h"

extern int gt_count_drawn_pixels(void);

#define OUT_BMP "/CCGFX.BMP"

void gt_log(const char* msg) { printf("%s\n", msg); }

void Process_Abort2(cc_result result, const char* raw_msg) {
	printf("ABORT (%u): %s\n", (unsigned)result, raw_msg ? raw_msg : "(null)");
	exit(2);
}

cc_uint64 Stopwatch_Measure(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (cc_uint64)ts.tv_sec * 1000000000ULL + (cc_uint64)ts.tv_nsec;
}

/* 24bpp BMP. Byte order note: a MayteraOS pixel word is 0xAARRGGBB, whose
 * little-endian bytes are B,G,R,A, and BMP wants B,G,R on disk. So bytes
 * [0],[1],[2] copy straight across with no reordering. */
static int write_bmp24(const char* path, const BitmapCol* px, int w, int h) {
	FILE* f;
	unsigned char hdr[54];
	unsigned char* row;
	int y, x, row_bytes = w * 3, pad = (4 - (row_bytes & 3)) & 3;
	unsigned int data_size = (unsigned int)(row_bytes + pad) * (unsigned int)h;
	unsigned int file_size = 54 + data_size;

	f = fopen(path, "wb");
	if (!f) return -1;

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2]  = (unsigned char)(file_size);       hdr[3]  = (unsigned char)(file_size >> 8);
	hdr[4]  = (unsigned char)(file_size >> 16); hdr[5]  = (unsigned char)(file_size >> 24);
	hdr[10] = 54;
	hdr[14] = 40;
	hdr[18] = (unsigned char)(w);       hdr[19] = (unsigned char)(w >> 8);
	hdr[20] = (unsigned char)(w >> 16); hdr[21] = (unsigned char)(w >> 24);
	hdr[22] = (unsigned char)(h);       hdr[23] = (unsigned char)(h >> 8);
	hdr[24] = (unsigned char)(h >> 16); hdr[25] = (unsigned char)(h >> 24);
	hdr[26] = 1;
	hdr[28] = 24;
	hdr[34] = (unsigned char)(data_size);       hdr[35] = (unsigned char)(data_size >> 8);
	hdr[36] = (unsigned char)(data_size >> 16); hdr[37] = (unsigned char)(data_size >> 24);
	fwrite(hdr, 1, 54, f);

	/* Heap, not a stack or .bss array: a single 640-pixel row is small, but
	 * the rule is the rule and the fallback must be clean. */
	row = (unsigned char*)malloc((size_t)row_bytes + pad);
	if (!row) { fclose(f); return -1; }
	memset(row, 0, (size_t)row_bytes + pad);

	for (y = h - 1; y >= 0; y--) {          /* BMP scanlines are bottom-up */
		const unsigned char* src = (const unsigned char*)(px + (size_t)y * w);
		for (x = 0; x < w; x++) {
			row[x * 3 + 0] = src[x * 4 + 0];
			row[x * 3 + 1] = src[x * 4 + 1];
			row[x * 3 + 2] = src[x * 4 + 2];
		}
		fwrite(row, 1, (size_t)row_bytes + pad, f);
	}
	free(row);
	fclose(f);
	return 0;
}

int main(void) {
	struct gt_results res;
	int rc, drawn;
	cc_uint64 t0, t1;
	unsigned long total_us, per_frame_us;
	const int TIMED_FRAMES = 60;

	printf("ClassiCube SoftGPU offscreen harness (#28), %dx%d 32bpp\n",
	       GT_WIDTH, GT_HEIGHT);

	rc = gt_run(&res);

	gt_render_3d_frames(3);                 /* warm */
	drawn = gt_count_drawn_pixels();
	t0 = Stopwatch_Measure();
	gt_render_3d_frames(TIMED_FRAMES);
	t1 = Stopwatch_Measure();

	total_us     = (unsigned long)((t1 - t0) / 1000ULL);
	per_frame_us = total_us / (unsigned long)TIMED_FRAMES;

	if (write_bmp24(OUT_BMP, gt_framebuffer(), GT_WIDTH, GT_HEIGHT) == 0)
		printf("wrote %s\n", OUT_BMP);
	else
		printf("could not write %s\n", OUT_BMP);

	printf("checks run %d, failed %d\n", res.checks_run, res.checks_failed);
	printf("cube frame %lu us over %d frames, coverage %d px\n",
	       per_frame_us, TIMED_FRAMES, drawn);

	if (drawn < (GT_WIDTH * GT_HEIGHT) / 100) {
		printf("FAIL timing scene drew almost nothing\n");
		rc = 1;
	}
	printf("%s\n", rc == 0 ? "PASS" : "FAIL");
	return rc;
}
