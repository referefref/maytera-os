/* main_host.c - build-server driver for the offscreen graphics harness (#28).
 *
 * Runs the SAME gfxtest_scene.c / gfxtest_support.c the MayteraOS ELF runs,
 * then dumps the rasterised buffer to a BMP and measures frame time. Existing
 * so that rendering is provable without a window, a compositor, or a VM.
 *
 * BMP CHANNEL NOTE (this is itself evidence, not a convenience):
 * a 24bpp BMP stores each pixel as three bytes B,G,R in that on-disk order.
 * A MayteraOS framebuffer word is 0xAARRGGBB, whose little-endian memory bytes
 * are B,G,R,A. So writing bytes [0],[1],[2] of each word STRAIGHT OUT with no
 * reordering produces a correct BMP. If the pixel format were anything else,
 * the dumped image would come out channel-swapped and the numeric checks in
 * gfxtest_scene.c would have already failed. The dump and the assertions agree
 * or neither is trusted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "Core.h"
#include "Bitmap.h"
#include "Platform.h"
#include "gfxtest.h"

extern int gt_count_drawn_pixels(void);

void gt_log(const char* msg) { printf("%s\n", msg); }

void Process_Abort2(cc_result result, const char* raw_msg) {
	fprintf(stderr, "ABORT (%u): %s\n", result, raw_msg ? raw_msg : "(null)");
	exit(2);
}

cc_uint64 Stopwatch_Measure(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (cc_uint64)ts.tv_sec * 1000000000ULL + (cc_uint64)ts.tv_nsec;
}

static cc_uint64 now_ns(void) { return Stopwatch_Measure(); }

/* ---------------------------------------------------------------------- */
static int write_bmp24(const char* path, const BitmapCol* px, int w, int h) {
	FILE* f = fopen(path, "wb");
	unsigned char hdr[54];
	unsigned char* row;
	int y, x, row_bytes = w * 3, pad = (4 - (row_bytes & 3)) & 3;
	unsigned int data_size = (unsigned int)(row_bytes + pad) * (unsigned int)h;
	unsigned int file_size = 54 + data_size;

	if (!f) return -1;
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2] = (unsigned char)(file_size); hdr[3] = (unsigned char)(file_size >> 8);
	hdr[4] = (unsigned char)(file_size >> 16); hdr[5] = (unsigned char)(file_size >> 24);
	hdr[10] = 54;                     /* pixel data offset */
	hdr[14] = 40;                     /* BITMAPINFOHEADER  */
	hdr[18] = (unsigned char)(w); hdr[19] = (unsigned char)(w >> 8);
	hdr[20] = (unsigned char)(w >> 16); hdr[21] = (unsigned char)(w >> 24);
	hdr[22] = (unsigned char)(h); hdr[23] = (unsigned char)(h >> 8);
	hdr[24] = (unsigned char)(h >> 16); hdr[25] = (unsigned char)(h >> 24);
	hdr[26] = 1;                      /* planes */
	hdr[28] = 24;                     /* bpp    */
	hdr[34] = (unsigned char)(data_size); hdr[35] = (unsigned char)(data_size >> 8);
	hdr[36] = (unsigned char)(data_size >> 16); hdr[37] = (unsigned char)(data_size >> 24);
	fwrite(hdr, 1, 54, f);

	row = (unsigned char*)malloc((size_t)row_bytes + pad);
	if (!row) { fclose(f); return -1; }
	memset(row, 0, (size_t)row_bytes + pad);

	/* BMP scanlines run bottom-up. */
	for (y = h - 1; y >= 0; y--) {
		const unsigned char* src = (const unsigned char*)(px + (size_t)y * w);
		for (x = 0; x < w; x++) {
			row[x * 3 + 0] = src[x * 4 + 0];   /* B */
			row[x * 3 + 1] = src[x * 4 + 1];   /* G */
			row[x * 3 + 2] = src[x * 4 + 2];   /* R */
		}
		fwrite(row, 1, (size_t)row_bytes + pad, f);
	}
	free(row);
	fclose(f);
	return 0;
}

/* ---------------------------------------------------------------------- */
int main(int argc, char** argv) {
	struct gt_results res;
	const char* outdir = argc > 1 ? argv[1] : ".";
	char path[512];
	int rc, drawn, i;
	cc_uint64 t0, t1;
	double ms;
	static const int TIMED_FRAMES = 120;

	printf("== ClassiCube SoftGPU offscreen harness (#28) ==\n");
	printf("   surface: %dx%d, 32bpp, word 0xAARRGGBB\n", GT_WIDTH, GT_HEIGHT);

	rc = gt_run(&res);

	/* Dump the depth/3D frame that gt_run left in the buffer. */
	snprintf(path, sizeof(path), "%s/gfxtest_depth.bmp", outdir);
	if (write_bmp24(path, gt_framebuffer(), GT_WIDTH, GT_HEIGHT) == 0)
		printf("   wrote %s\n", path);

	/* Dump the 2D conformance frame too. */
	gt_render_2d_frame();
	snprintf(path, sizeof(path), "%s/gfxtest_2d.bmp", outdir);
	if (write_bmp24(path, gt_framebuffer(), GT_WIDTH, GT_HEIGHT) == 0)
		printf("   wrote %s\n", path);

	/* Timing: representative textured, depth-tested 3D workload. */
	gt_render_3d_frames(5);                 /* warm caches */
	drawn = gt_count_drawn_pixels();
	t0 = now_ns();
	gt_render_3d_frames(TIMED_FRAMES);
	t1 = now_ns();
	ms = (double)(t1 - t0) / 1e6 / TIMED_FRAMES;

	snprintf(path, sizeof(path), "%s/gfxtest_cube.bmp", outdir);
	if (write_bmp24(path, gt_framebuffer(), GT_WIDTH, GT_HEIGHT) == 0)
		printf("   wrote %s\n", path);

	/* Fill-rate: the defensible number. 1x = every pixel shaded once,
	 * 3x = a plausible world frame with overdraw. */
	{
		int od;
		printf("\n-- fill rate (640x480 textured, depth-tested, 3D path) --\n");
		for (od = 1; od <= 3; od += 2) {
			double fms;
			gt_render_fill_frames(3, od);
			t0 = now_ns();
			gt_render_fill_frames(30, od);
			t1 = now_ns();
			fms = (double)(t1 - t0) / 1e6 / 30.0;
			printf("   overdraw %dx : %.2f ms/frame (%.0f fps), %.0f Mpixel/s\n",
			       od, fms, 1000.0 / fms,
			       (double)GT_WIDTH * GT_HEIGHT * od / (fms / 1000.0) / 1e6);
		}
		snprintf(path, sizeof(path), "%s/gfxtest_fill.bmp", outdir);
		write_bmp24(path, gt_framebuffer(), GT_WIDTH, GT_HEIGHT);
	}

	printf("\n-- results --\n");
	printf("   checks run    : %d\n", res.checks_run);
	printf("   checks failed : %d\n", res.checks_failed);
	printf("   cube frame     : %.3f ms  (%.1f fps) over %d frames\n",
	       ms, 1000.0 / ms, TIMED_FRAMES);
	printf("   cube coverage  : %d / %d px non-background (%.1f%%)\n",
	       drawn, GT_WIDTH * GT_HEIGHT, 100.0 * drawn / (GT_WIDTH * GT_HEIGHT));

	/* A timing number for a scene that drew nothing is worthless. */
	if (drawn < (GT_WIDTH * GT_HEIGHT) / 100) {
		printf("   FAIL timing scene drew almost nothing; frame time is meaningless\n");
		rc = 1;
	}

	printf("\n%s\n", rc == 0 ? "PASS" : "FAIL");
	(void)i;
	return rc;
}
