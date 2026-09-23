/*
 * rrect.h - rounded-rectangle span geometry, shared by the device painter and
 * the test harness (#245).
 *
 * WHY THIS IS ONE FILE AND NOT TWO IMPLEMENTATIONS. The harness exists to find
 * layout faults in seconds instead of via a golden build and a VM boot, and it
 * is only worth anything while it agrees with the device. Two hand-written
 * copies of a corner-inset calculation would drift, and the drift would show up
 * as "the harness rounds it differently", which is the least useful bug report
 * available. So the geometry lives here and both painters call it.
 *
 * The rectangle is emitted as HORIZONTAL SPANS with equal-inset rows coalesced,
 * because both painters ultimately reach a fill-rectangle call and a span per
 * scanline would be one call per pixel row. A 10px radius comes out as roughly
 * 13 spans instead of 40.
 *
 * Integer only, no libm: the kernel is soft-float and the browser has no
 * business needing a square root from libc either.
 */
#ifndef MAYTERA_RRECT_H
#define MAYTERA_RRECT_H

/*
 * Emit the spans covering the rounded rectangle (x, y, w, h) with corner radius
 * r. `span(ctx, sx, sy, sw, sh)` is called for each; the caller clips and
 * fills. r <= 0 emits the single rectangle, so a caller never needs to special
 * case a square box.
 */
void rrect_spans(int x, int y, int w, int h, int r,
		void (*span)(void *ctx, int sx, int sy, int sw, int sh), void *ctx);

/*
 * The RING between an outer rounded rect and the same rect inset by `bw` on all
 * four sides, for a bordered box with no background of its own (where filling
 * the outer shape in the border colour and the inner in the background is not
 * available, because there is no background to fill the inner with).
 */
void rrect_ring_spans(int x, int y, int w, int h, int r, int bw,
		void (*span)(void *ctx, int sx, int sy, int sw, int sh), void *ctx);

#endif
