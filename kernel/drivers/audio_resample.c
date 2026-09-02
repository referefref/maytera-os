// audio_resample.c - the shared streaming resampler.
//
// SPLIT OUT OF audio.c (#181 audio for the Ring-3 DOS host), and split rather
// than copied for one reason: the Ring-3 DOS interpreter (/APPS/DOSUSER) needs
// this exact resampler, and the house rule is that there is ONE implementation
// of a primitive in the tree. audio.c cannot be compiled into a Ring-3 app (it
// is a driver: HDA, AC97, SB16, the mixer, the FAT decoders). These two
// functions are pure fixed-point arithmetic over caller-owned buffers with no
// driver state and no kernel-only dependency, so they live in their own
// translation unit that BOTH the kernel build (drivers/*.c is wildcarded) and
// userland/apps/dosring3 (mkgen.sh copies this file verbatim) compile.
//
// The code below is UNCHANGED from audio.c; only its address moved. If it is
// ever edited, both paths get the edit, which is the entire point.
#include "audio.h"

// #181: the phase-carrying form of the resampler above. See audio.h for why
// it exists and why audio_resample() is left alone.
//
// THE VIRTUAL ARRAY. Interpolating across a chunk boundary needs the last frame
// of the previous chunk, so the function works over a virtual array
//     v[0]   = st->prev          (the carried frame)
//     v[1+i] = src[i]            for i in 0 .. src_frames-1
// and st->pos_fp is a 16.16 position inside THAT. After the chunk, the position
// is shifted down by src_frames and prev is replaced with the chunk's last
// frame, so the next call continues seamlessly. That is the whole trick, and it
// is why there is no click at a chunk boundary and no per-chunk drift.
void audio_resample_stream_init(audio_resample_state_t *st) {
    if (!st) return;
    st->pos_fp    = 0;
    st->have_prev = 0;
    st->prev[0]   = 0;
    st->prev[1]   = 0;
}

uint32_t audio_resample_stream(audio_resample_state_t *st,
                               const int16_t *src, uint32_t src_frames,
                               uint32_t src_rate,
                               int16_t *dst, uint32_t dst_cap, uint32_t dst_rate,
                               uint32_t channels) {
    if (!st || !src || !dst || src_frames == 0) return 0;
    if (src_rate == 0 || dst_rate == 0) return 0;
    if (channels < 1 || channels > 2) return 0;

    // Refuse rather than truncate: a short write here would drop source frames
    // that the caller has already counted as transferred, and the DMA position
    // it publishes to the guest would then be a lie.
    uint64_t need = ((uint64_t)src_frames * dst_rate) / src_rate + 2;
    if ((uint64_t)dst_cap < need) return 0;

    uint32_t ratio = (uint32_t)(((uint64_t)src_rate << 16) / dst_rate);
    if (ratio == 0) ratio = 1;

    if (!st->have_prev) {
        // First chunk: there is no previous frame, so start exactly ON src[0]
        // (v[1]) with prev seeded from it. Seeding prev with silence instead
        // would put a one-frame ramp from zero at the very start of every
        // transfer, which is an audible tick on a percussive sample.
        for (uint32_t c = 0; c < channels; c++) st->prev[c] = src[c];
        st->have_prev = 1;
        st->pos_fp = 1u << 16;
    }

    uint32_t out = 0;
    for (;;) {
        uint32_t i = st->pos_fp >> 16;
        if (i + 1 > src_frames) break;          // needs v[i] and v[i+1]
        uint32_t frac = st->pos_fp & 0xFFFF;
        for (uint32_t c = 0; c < channels; c++) {
            int32_t s0 = (i == 0) ? st->prev[c] : src[(i - 1) * channels + c];
            int32_t s1 = src[i * channels + c];
            dst[out * channels + c] = (int16_t)(s0 + (((s1 - s0) * (int32_t)frac) >> 16));
        }
        out++;
        st->pos_fp += ratio;
        if (out >= dst_cap) break;              // cannot happen: `need` above
    }

    // Shift the window down by one chunk and carry the boundary frame.
    uint32_t consumed = st->pos_fp >> 16;
    if (consumed > src_frames) consumed = src_frames;
    st->pos_fp -= (uint32_t)src_frames << 16;
    for (uint32_t c = 0; c < channels; c++)
        st->prev[c] = src[(src_frames - 1) * channels + c];
    (void)consumed;
    return out;
}
