// uiscale.c (Ring 3) - the cached factor and the agreement check.
#include "uiscale.h"
#include "stdio.h"

int g_ui_scale_pct = 100;
int g_ui_scale_gen = -1;

int ui_scale_refresh(void) {
    int gen = ui_scale_gen();
    if (gen == g_ui_scale_gen) return 0;
    g_ui_scale_gen = gen;
    int pct = ui_scale_pct();
    if (pct == g_ui_scale_pct) return 0;
    g_ui_scale_pct = pct;
    return 1;
}

// Ask the kernel for the same answers this file computes locally, over a range
// that includes every shape that has historically gone wrong: zero, one (the
// hairline that must never vanish), small paddings, type sizes, control
// heights, and full-screen coordinates. Also checks the round trip, which is
// the property that makes hit-testing safe, and the shared-edge property,
// which is what keeps fractional scales from looking uneven.
int ui_scale_selfcheck(void) {
    static const int probe[] = { 0, 1, 2, 3, 4, 6, 8, 10, 11, 12, 14, 15, 16, 18,
                                 20, 22, 24, 26, 28, 30, 32, 36, 40, 48, 64, 80,
                                 96, 100, 120, 128, 180, 200, 300, 480, 640, 720,
                                 800, 900, 1024, 1080, 1280, 1440, 1920 };
    const int n = (int)(sizeof(probe) / sizeof(probe[0]));
    int bad = 0;
    ui_scale_refresh();
    for (int i = 0; i < n; i++) {
        int v = probe[i];
        int kp = (int)syscall2(SYS_UI_SCALE, UISC_PX, (long)v);
        if (ui_px(v) != kp) {
            printf("[UISCALE] MISMATCH px(%d): userland %d, kernel %d\n", v, ui_px(v), kp);
            bad++;
        }
        int ku = (int)syscall2(SYS_UI_SCALE, UISC_UNPX, (long)v);
        if (ui_unpx(v) != ku) {
            printf("[UISCALE] MISMATCH unpx(%d): userland %d, kernel %d\n", v, ui_unpx(v), ku);
            bad++;
        }
        // Spans: the oracle packs origin and extent into one argument, so keep
        // both inside 16 bits.
        if (v <= 0xFFFF) {
            for (int e = 1; e <= 40; e += 7) {
                long packed = ((long)v << 16) | (long)e;
                int ks = (int)syscall2(SYS_UI_SCALE, UISC_SPAN, packed);
                if (ui_span(v, e) != ks) {
                    printf("[UISCALE] MISMATCH span(%d,%d): userland %d, kernel %d\n",
                           v, e, ui_span(v, e), ks);
                    bad++;
                }
                // The shared-edge property, checked locally: a box's scaled
                // right edge IS the scaled coordinate of its right edge.
                if (ui_px(v) + ui_span(v, e) != ui_px(v + e)) {
                    printf("[UISCALE] EDGE BROKEN at (%d,%d)\n", v, e);
                    bad++;
                }
            }
        }
        // The round trip: every physical pixel of a logical pixel comes back.
        if (v >= 0 && v < 2000) {
            int p0 = ui_px(v), p1 = ui_px(v + 1);
            for (int q = p0; q < p1; q++) {
                if (ui_unpx(q) != v) { bad++; break; }
            }
        }
    }
    return bad;
}
