/* rrect.c - see rrect.h. Integer only, no libm, no allocation. */
#include "rrect.h"

/* Integer square root, Newton on integers. isqrt(n) = floor(sqrt(n)). */
static int rr_isqrt(int n)
{
	int x, y;
	if (n <= 0) return 0;
	x = n;
	y = (x + 1) / 2;
	while (y < x) { x = y; y = (x + n / x) / 2; }
	return x;
}

/*
 * Horizontal inset of the rounded corner at `dy` rows into the corner band of
 * radius r. Measured from the row's CENTRE (dy + 0.5, done as 2*dy + 1 over a
 * doubled radius) so the shape is symmetric top to bottom and does not lose a
 * pixel at the extremes.
 */
static int rr_inset(int r, int dy)
{
	int k, r2, s;
	if (r <= 0) return 0;
	if (dy < 0) dy = 0;
	if (dy >= r) return 0;
	/* distance from the corner circle's centre, doubled */
	k = 2 * (r - dy) - 1;
	r2 = 4 * r * r - k * k;
	s = rr_isqrt(r2) / 2;          /* half-chord, back to whole pixels */
	if (s > r) s = r;
	return r - s;
}

void rrect_spans(int x, int y, int w, int h, int r,
		void (*span)(void *ctx, int sx, int sy, int sw, int sh), void *ctx)
{
	int dy, run_start, run_inset;

	if (!span || w <= 0 || h <= 0) return;
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	if (r <= 0) { span(ctx, x, y, w, h); return; }

	/* top corner band, equal-inset rows coalesced into one span each */
	run_start = 0;
	run_inset = rr_inset(r, 0);
	for (dy = 1; dy <= r; dy++) {
		int in = (dy < r) ? rr_inset(r, dy) : -1;
		if (in != run_inset) {
			span(ctx, x + run_inset, y + run_start,
					w - 2 * run_inset, dy - run_start);
			run_start = dy;
			run_inset = in;
		}
	}

	/* straight middle */
	if (h - 2 * r > 0)
		span(ctx, x, y + r, w, h - 2 * r);

	/* bottom corner band, the top band mirrored */
	run_start = 0;
	run_inset = rr_inset(r, 0);
	for (dy = 1; dy <= r; dy++) {
		int in = (dy < r) ? rr_inset(r, dy) : -1;
		if (in != run_inset) {
			/* rows [run_start, dy) of the band, counted up from the
			 * bottom edge */
			span(ctx, x + run_inset, y + h - dy,
					w - 2 * run_inset, dy - run_start);
			run_start = dy;
			run_inset = in;
		}
	}
}

void rrect_ring_spans(int x, int y, int w, int h, int r, int bw,
		void (*span)(void *ctx, int sx, int sy, int sw, int sh), void *ctx)
{
	int dy;

	if (!span || w <= 0 || h <= 0 || bw <= 0) return;
	if (bw * 2 >= w || bw * 2 >= h) { rrect_spans(x, y, w, h, r, span, ctx); return; }
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;

	if (r <= 0) {
		span(ctx, x, y, w, bw);
		span(ctx, x, y + h - bw, w, bw);
		span(ctx, x, y + bw, bw, h - 2 * bw);
		span(ctx, x + w - bw, y + bw, bw, h - 2 * bw);
		return;
	}

	/* Corner bands: two spans per row, one per side. Not coalesced, because
	 * the outer and inner insets change at different rows and a run would
	 * have to agree on both. A ring only happens on a bordered box with no
	 * background, which is rare enough not to pay for the bookkeeping. */
	for (dy = 0; dy < r; dy++) {
		int o = rr_inset(r, dy);
		int ir = r - bw;
		int in = (dy < bw) ? (w / 2) : rr_inset(ir > 0 ? ir : 0, dy - bw) + bw;
		int left_w, right_x;
		if (in > w / 2) in = w / 2;
		left_w = in - o;
		if (left_w < 0) left_w = 0;
		right_x = x + w - in;
		if (left_w > 0) {
			span(ctx, x + o, y + dy, left_w, 1);
			span(ctx, right_x, y + dy, left_w, 1);
			span(ctx, x + o, y + h - 1 - dy, left_w, 1);
			span(ctx, right_x, y + h - 1 - dy, left_w, 1);
		}
	}
	/* straight sides */
	if (h - 2 * r > 0) {
		span(ctx, x, y + r, bw, h - 2 * r);
		span(ctx, x + w - bw, y + r, bw, h - 2 * r);
	}
}
