// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// term_damage_hostio.c - host file I/O and reporting for the damage harness.
// Includes the HOST's <stdio.h> and NONE of MayteraOS's headers; see
// term_damage_hostio.h for why the two halves cannot share a translation unit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "term_damage_hostio.h"

void hio_die(const char *msg) { fprintf(stderr, "term_damage_test: %s\n", msg); exit(2); }

char *hio_slurp(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "term_damage_test: cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)*n + 1);
    if (!b) hio_die("out of memory");
    if (*n && fread(b, 1, (size_t)*n, f) != (size_t)*n) hio_die("short read");
    fclose(f); b[*n] = 0; return b;
}

// A frame plan is one integer per line: how many bytes of the stream arrived in
// one event-loop tick. Produced by tests/traces/mkframes.py from script(1)'s
// own timing file, so the split is the one a REAL pty produced at the
// terminal's 10 ms pump rate rather than an invented one.
long *hio_load_frames(const char *path, int *count) {
    long n; char *b = hio_slurp(path, &n);
    int cap = 4096, k = 0;
    long *v = (long *)malloc((size_t)cap * sizeof(long));
    char *p = b;
    while (*p) {
        while (*p == '\n' || *p == ' ' || *p == '\r' || *p == '\t') p++;
        if (!*p) break;
        if (k == cap) { cap *= 2; v = (long *)realloc(v, (size_t)cap * sizeof(long)); }
        v[k++] = strtol(p, &p, 10);
    }
    free(b); *count = k; return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <trace.bin> <frames.txt> [cols] [rows]\n", argv[0]);
        return 2;
    }
    int cols = (argc > 3) ? atoi(argv[3]) : 80;
    int rows = (argc > 4) ? atoi(argv[4]) : 24;
    long n; char *data = hio_slurp(argv[1], &n);
    int nf; long *frames = hio_load_frames(argv[2], &nf);

    dmg_result_t base, r;
    memset(&base, 0, sizeof base);
    memset(&r, 0, sizeof r);
    damage_run(data, n, frames, nf, cols, rows, 1, &base);   // pre-change renderer
    // damage_run() hands back a pointer to its own static, which the second
    // call overwrites, so the baseline's per-frame checksums are copied out
    // before the second arm runs.
    long bn = base.fb_frame_n;
    unsigned long *bsum = (unsigned long *)malloc((size_t)(bn ? bn : 1) * sizeof(unsigned long));
    if (!bsum) hio_die("out of memory");
    memcpy(bsum, base.fb_frame_sum, (size_t)bn * sizeof(unsigned long));
    damage_run(data, n, frames, nf, cols, rows, 0, &r);      // damage-tracked
    long first_bad = -1, nbad = 0;
    long cmp = (bn < r.fb_frame_n) ? bn : r.fb_frame_n;
    for (long i = 0; i < cmp; i++)
        if (bsum[i] != r.fb_frame_sum[i]) { if (first_bad < 0) first_bad = i; nbad++; }

    printf("trace            %s\n", argv[1]);
    printf("grid             %ld x %ld = %ld cells\n",
           r.grid_cols, r.grid_rows, r.grid_cols * r.grid_rows);
    printf("bytes            %ld\n", r.bytes);
    printf("frames           %ld   (painted nothing at all: %ld)\n", r.frames, r.frames_idle);
    printf("cells scanned    %-12ld <- what a full-grid repaint paints\n", r.cells_scanned);
    printf("cells painted    %-12ld <- what damage tracking paints\n", r.cells_painted);
    if (r.cells_painted > 0 && r.cells_scanned > 0) {
        long pct = (r.cells_painted * 10000) / r.cells_scanned;
        printf("REDUCTION        %ld.%02ldx   (painted %ld.%02ld%% of the cells)\n",
               r.cells_scanned / r.cells_painted,
               ((r.cells_scanned * 100) / r.cells_painted) % 100,
               pct / 100, pct % 100);
    }
    if (r.frames > 0)
        printf("per frame        scanned %ld, painted %ld\n",
               r.cells_scanned / r.frames, r.cells_painted / r.frames);
    printf("\n                        FULL REPAINT      DAMAGE TRACKED     RATIO\n");
    printf("cells painted           %-16ld  %-16ld", base.cells_painted, r.cells_painted);
    if (r.cells_painted) printf("  %ld.%02ldx", base.cells_painted / r.cells_painted,
                                ((base.cells_painted * 100) / r.cells_painted) % 100);
    printf("\n");
    printf("win_draw_rect           %-16ld  %-16ld", base.sc_rect, r.sc_rect);
    if (r.sc_rect) printf("  %ld.%02ldx", base.sc_rect / r.sc_rect,
                          ((base.sc_rect * 100) / r.sc_rect) % 100);
    printf("\n");
    printf("win_draw_text_ttf_ex    %-16ld  %-16ld", base.sc_text, r.sc_text);
    if (r.sc_text) printf("  %ld.%02ldx", base.sc_text / r.sc_text,
                          ((base.sc_text * 100) / r.sc_text) % 100);
    printf("\n");
    printf("win_draw_image          %-16ld  %-16ld", base.sc_image, r.sc_image);
    if (r.sc_image) printf("  %ld.%02ldx", base.sc_image / r.sc_image,
                           ((base.sc_image * 100) / r.sc_image) % 100);
    printf("\n");
    printf("win_invalidate          %-16ld  %-16ld\n", base.sc_invalidate, r.sc_invalidate);
    printf("\nPIXEL EQUIVALENCE (the emulated window, compared after EVERY frame)\n");
    printf("  frames compared          %ld\n", cmp);
    printf("  frames that differ       %ld%s\n", nbad,
           first_bad >= 0 ? "   *** MISMATCH ***" : "");
    if (first_bad >= 0) printf("  first differing frame    %ld\n", first_bad);
    printf("  final window checksum    %016lx vs %016lx\n", base.fb_sum, r.fb_sum);
    printf("  final differing pixels   %ld  ->  %s\n", r.fb_diff_vs_prev,
           r.fb_diff_vs_prev == 0 ? "IDENTICAL" : "*** MISMATCH ***");

    printf("\nEVERY win_draw_image is a WHOLE-WINDOW commit in the kernel\n");
    printf("(sys_win_draw_image ends in uw_commit_content + wm_invalidate_rect_async),\n");
    printf("so the win_draw_image column is a count of full content-buffer memcpys.\n");
    printf("presents         %ld   (win_invalidate; each is a synchronous window_draw\n", r.sc_invalidate);
    printf("                  plus a full content_w*content_h memcpy)\n");

    // The distribution, not just the mean. A TUI has two populations of frame
    // and a single average hides both of them: the frames that only move a
    // spinner are the ones the owner's complaint is about, and they are the
    // ones that used to cost a whole grid.
    if (r.per_frame_n > 0) {
        long grid = r.grid_cols * r.grid_rows;
        long buckets[6]; memset(buckets, 0, sizeof buckets);
        long worst = 0;
        for (long i = 0; i < r.per_frame_n; i++) {
            long v = r.per_frame[i];
            if (v > worst) worst = v;
            if (v == 0)              buckets[0]++;
            else if (v <= 4)         buckets[1]++;
            else if (v <= 32)        buckets[2]++;
            else if (v <= 200)       buckets[3]++;
            else if (v < grid)       buckets[4]++;
            else                     buckets[5]++;
        }
        printf("\nper-frame painted-cell distribution over %ld frames\n", r.per_frame_n);
        printf("  0 cells (no visible change)   %ld\n", buckets[0]);
        printf("  1-4 cells                     %ld\n", buckets[1]);
        printf("  5-32 cells                    %ld\n", buckets[2]);
        printf("  33-200 cells                  %ld\n", buckets[3]);
        printf("  201-%ld cells (part grid)  %ld\n", grid - 1, buckets[4]);
        printf("  >= %ld cells (whole grid)     %ld\n", grid, buckets[5]);
        printf("  worst frame                   %ld cells\n", worst);
    }
    return (r.fb_diff_vs_prev == 0 && nbad == 0) ? 0 : 1;
}
