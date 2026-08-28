// term_damage_hostio.h - the ONLY thing that crosses between the two halves of
// this harness.
//
// The measurement half includes MayteraOS's freestanding libc headers; the I/O
// half includes the HOST's <stdio.h>. Those two cannot be in one translation
// unit: both define FILE, off_t, fopen and forty other names, and gcc rejects
// the pair outright. So they are two files, and this header - which uses only
// built-in types - is the whole interface between them.
#ifndef TERM_DAMAGE_HOSTIO_H
#define TERM_DAMAGE_HOSTIO_H

typedef struct {
    long grid_cols, grid_rows;
    long bytes, frames, frames_idle;
    long cells_scanned, cells_painted;
    long sc_rect, sc_text, sc_image, sc_invalidate;
    // Per-frame painted-cell counts, so the two populations a real TUI has
    // (frames that only move a spinner, and frames that scroll the transcript)
    // can be told apart instead of averaged into one meaningless number.
    long *per_frame;
    long  per_frame_n;
    // A 32-bit checksum of the EMULATED WINDOW after the last frame, plus the
    // number of pixels that differ from the other arm's. See fb_* in
    // term_damage_test.c: the two arms must end with byte-identical pixels.
    unsigned long fb_sum;
    long          fb_diff_vs_prev;
    // A checksum of the emulated window after EVERY frame, not just the last.
    // "Identical at the end" would pass a bug that showed a stale cell for two
    // hundred frames and then healed, which is exactly the bug this feature can
    // introduce and exactly the one a user would report.
    const unsigned long *fb_frame_sum;
    long                 fb_frame_n;
} dmg_result_t;

// Implemented in the I/O half.
char *hio_slurp(const char *path, long *n);
long *hio_load_frames(const char *path, int *count);
void  hio_die(const char *msg);

// Implemented in the measurement half.
// `no_damage` reruns the SAME trace with damage tracking switched off, which
// is the pre-change renderer exactly (term_shadow == NULL is already the
// "this pane repaints in full every frame" path, so this is not a second
// implementation of the old behaviour, it is the old behaviour). Running both
// arms over one trace in one process is what makes the syscall counts
// comparable: a TUI replayed twice is not the same workload twice.
void damage_run(const char *trace, long tracelen,
                const long *frames, int nframes,
                int cols, int rows, int no_damage, dmg_result_t *out);

#endif
