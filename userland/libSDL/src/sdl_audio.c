/* sdl_audio.c - SDL_OpenAudio and friends over MayteraOS's real Ring-3 PCM
 * syscalls. Part of the MayteraOS SDL 1.2 backend (task #745,
 * docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7).
 *
 * STATED UP FRONT, matching the honesty convention
 * userland/apps/classicube/Audio_Maytera.c already set for this exact
 * syscall trio: this is a REAL callback-pump audio backend, not a silent
 * stub, but it inherits the platform's real limits rather than hiding them.
 *   - kernel/drivers/audio_pcm.c has exactly ONE PCM stream
 *     (PCM_MAX_STREAMS == 1) with one owning process; SDL_OpenAudio can only
 *     ever open that one stream. A second concurrent SDL_OpenAudio caller
 *     (or a second app) gets a real error, not silence pretending to be
 *     success.
 *   - The device is fixed-format S16LE (kernel/drivers/audio_pcm.h: "format
 *     must be AUDIO_FORMAT_S16_LE"). AUDIO_U8/AUDIO_S8/AUDIO_S16MSB/
 *     AUDIO_U16* are accepted and converted in the pump thread; no other
 *     format is supported. No SDL_AudioCVT resampling exists (the pump asks
 *     the kernel to open the stream at the game's OWN requested rate; there
 *     is no evidence the kernel resamples either, so a rate the real audio
 *     hardware cannot do natively is the same open bet
 *     userland/apps/classicube/Audio_Maytera.c already takes).
 *   - SYS_AUDIO_PCM_WRITE blocks when the kernel ring is full (a proper
 *     wait_event wake, not a spin) and wakes from the PCM pump's own wait
 *     queue; that write always happens on OUR pump thread, never on the
 *     caller's thread, so a slow game loop cannot stall audio and a full
 *     ring cannot stall the game loop either.
 */
#include "sdl_priv.h"
#include "pthread.h"
#include <string.h>

#define MAYT_AUDIO_FORMAT_S16_LE 0x0002

static SDL_AudioSpec g_spec;
static int g_pcm_handle = -1;
static pthread_t g_pump_tid;
static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_audio_running = 0;
static volatile int g_audio_paused = 1;
static SDL_audiostatus g_audio_status = SDL_AUDIO_STOPPED;

static void convert_to_s16(const Uint8 *src, int nsamples, Uint16 srcfmt, Sint16 *out) {
    for (int i = 0; i < nsamples; i++) {
        switch (srcfmt) {
        case AUDIO_U8: out[i] = (Sint16)(((int)src[i] - 128) << 8); break;
        case AUDIO_S8: out[i] = (Sint16)((Sint8)src[i] << 8); break;
        case AUDIO_S16MSB: out[i] = (Sint16)(((Uint16)src[i * 2] << 8) | src[i * 2 + 1]); break;
        case AUDIO_U16MSB: out[i] = (Sint16)((((Uint16)src[i * 2] << 8) | src[i * 2 + 1]) - 32768); break;
        case AUDIO_U16LSB: out[i] = (Sint16)((((Uint16)src[i * 2 + 1] << 8) | src[i * 2]) - 32768); break;
        case AUDIO_S16LSB: default: out[i] = (Sint16)(((Uint16)src[i * 2 + 1] << 8) | src[i * 2]); break;
        }
    }
}

static void *audio_pump_thread(void *arg) {
    (void)arg;
    int frames = g_spec.samples;
    int channels = g_spec.channels;
    int bytes_per_sample = (g_spec.format == AUDIO_U8 || g_spec.format == AUDIO_S8) ? 1 : 2;
    int src_bytes = frames * channels * bytes_per_sample;
    Uint8 *srcbuf = (Uint8 *)SDL_malloc((size_t)src_bytes);
    Sint16 *s16buf = (Sint16 *)SDL_malloc((size_t)frames * channels * sizeof(Sint16));
    if (!srcbuf || !s16buf) { SDL_free(srcbuf); SDL_free(s16buf); return 0; }

    while (g_audio_running) {
        if (g_audio_paused || !g_spec.callback) { sys_sleep(10); continue; }
        pthread_mutex_lock(&g_audio_lock);
        memset(srcbuf, (int)g_spec.silence, (size_t)src_bytes);
        g_spec.callback(g_spec.userdata, srcbuf, src_bytes);
        pthread_mutex_unlock(&g_audio_lock);
        if (!g_audio_running) break;
        if (g_spec.format == AUDIO_S16LSB) {
            SDL_memcpy(s16buf, srcbuf, (size_t)src_bytes);
        } else {
            convert_to_s16(srcbuf, frames * channels, g_spec.format, s16buf);
        }
        /* Blocks on our OWN thread only, waking from the kernel PCM pump's
         * wait queue when the ring has room (never a spin). */
        sys_audio_pcm_write(g_pcm_handle, s16buf, (unsigned)frames);
    }
    SDL_free(srcbuf); SDL_free(s16buf);
    return 0;
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) {
    if (!desired || !desired->callback) { sdlpriv_set_error("SDL_OpenAudio: NULL spec or callback"); return -1; }
    if (g_pcm_handle > 0) { sdlpriv_set_error("Audio device already open (MayteraOS has one PCM stream)"); return -1; }

    int h = sys_audio_pcm_open((unsigned)desired->freq, (unsigned)desired->channels, MAYT_AUDIO_FORMAT_S16_LE);
    if (h < 1) { sdlpriv_set_error("Couldn't open MayteraOS PCM device (rc=%d)", h); return -1; }

    g_spec = *desired;
    if (g_spec.samples == 0) g_spec.samples = 1024;
    g_spec.size = (Uint32)(g_spec.samples * g_spec.channels *
                            ((g_spec.format == AUDIO_U8 || g_spec.format == AUDIO_S8) ? 1 : 2));
    g_spec.silence = (g_spec.format == AUDIO_U8) ? 128 : 0;
    g_pcm_handle = h;

    if (obtained) *obtained = g_spec;

    g_audio_running = 1;
    g_audio_paused = 1;  /* real SDL1.2 starts an opened device PAUSED */
    g_audio_status = SDL_AUDIO_PAUSED;
    if (pthread_create(&g_pump_tid, 0, audio_pump_thread, 0) != 0) {
        sys_audio_pcm_close(h); g_pcm_handle = -1; g_audio_running = 0;
        sdlpriv_set_error("pthread_create failed for the audio pump");
        return -1;
    }
    return 0;
}
void SDL_PauseAudio(int pause_on) {
    g_audio_paused = pause_on ? 1 : 0;
    g_audio_status = g_audio_paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}
SDL_audiostatus SDL_GetAudioStatus(void) { return g_audio_status; }
void SDL_LockAudio(void) { pthread_mutex_lock(&g_audio_lock); }
void SDL_UnlockAudio(void) { pthread_mutex_unlock(&g_audio_lock); }
void SDL_CloseAudio(void) {
    if (g_pcm_handle <= 0) return;
    g_audio_running = 0;
    pthread_join(g_pump_tid, 0);
    sys_audio_pcm_close(g_pcm_handle);  /* blocks until the ring drains */
    g_pcm_handle = -1;
    g_audio_status = SDL_AUDIO_STOPPED;
}
int SDL_AudioInit(const char *driver_name) { (void)driver_name; return 0; }
void SDL_AudioQuit(void) { SDL_CloseAudio(); }
char *SDL_AudioDriverName(char *namebuf, int maxlen) {
    if (namebuf && maxlen > 0) { int n = 0; const char *s = "maytera_pcm"; while (s[n] && n < maxlen - 1) { namebuf[n] = s[n]; n++; } namebuf[n] = 0; }
    return namebuf;
}

/* WAV loading: PCM-only (the overwhelming majority of SDL1.2 game assets).
 * ADPCM/extensible WAV are refused with a clear error. */
SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *src, int freesrc, SDL_AudioSpec *spec, Uint8 **audio_buf, Uint32 *audio_len) {
    if (!src || !spec || !audio_buf || !audio_len) return 0;
    Uint8 riff[12];
    SDL_AudioSpec *ret = 0;
    if (SDL_RWread(src, riff, 12, 1) != 1 || SDL_memcmp(riff, "RIFF", 4) != 0 || SDL_memcmp(riff + 8, "WAVE", 4) != 0) {
        sdlpriv_set_error("Not a RIFF/WAVE file"); goto done;
    }
    {
        int have_fmt = 0;
        Uint16 fmt_tag = 0, channels = 1, bits = 16;
        Uint32 rate = 22050;
        Uint8 *data = 0; Uint32 data_len = 0;
        for (;;) {
            Uint8 chunk_id[4]; Uint32 chunk_len;
            if (SDL_RWread(src, chunk_id, 4, 1) != 1) break;
            chunk_len = SDL_ReadLE32(src);
            if (SDL_memcmp(chunk_id, "fmt ", 4) == 0) {
                fmt_tag = SDL_ReadLE16(src);
                channels = SDL_ReadLE16(src);
                rate = SDL_ReadLE32(src);
                SDL_ReadLE32(src);      /* byte rate, derivable, ignored */
                SDL_ReadLE16(src);      /* block align, ignored */
                bits = SDL_ReadLE16(src);
                if (chunk_len > 16) SDL_RWseek(src, (int)(chunk_len - 16), RW_SEEK_CUR);
                have_fmt = 1;
            } else if (SDL_memcmp(chunk_id, "data", 4) == 0) {
                data = (Uint8 *)SDL_malloc(chunk_len);
                if (!data) { sdlpriv_set_error("Out of memory"); goto done; }
                if (SDL_RWread(src, data, 1, (int)chunk_len) != (int)chunk_len) { SDL_free(data); sdlpriv_set_error("Truncated WAV data"); goto done; }
                data_len = chunk_len;
                break;  /* data is conventionally last; stop here */
            } else {
                SDL_RWseek(src, (int)chunk_len + (int)(chunk_len & 1), RW_SEEK_CUR);
            }
        }
        if (!have_fmt || !data) { sdlpriv_set_error("WAV file missing fmt or data chunk"); goto done; }
        if (fmt_tag != 1 /* WAVE_FORMAT_PCM */) { SDL_free(data); sdlpriv_set_error("Only uncompressed PCM WAV is supported"); goto done; }
        memset(spec, 0, sizeof(*spec));
        spec->freq = (int)rate;
        spec->channels = (Uint8)channels;
        spec->format = (bits == 8) ? AUDIO_U8 : AUDIO_S16LSB;
        spec->samples = 4096;
        *audio_buf = data;
        *audio_len = data_len;
        ret = spec;
    }
done:
    if (freesrc) SDL_RWclose(src);
    return ret;
}
void SDL_FreeWAV(Uint8 *audio_buf) { SDL_free(audio_buf); }

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, Uint16 src_format, Uint8 src_channels, int src_rate,
                       Uint16 dst_format, Uint8 dst_channels, int dst_rate) {
    memset(cvt, 0, sizeof(*cvt));
    cvt->src_format = src_format; cvt->dst_format = dst_format;
    cvt->len_mult = 1; cvt->len_ratio = 1.0;
    if (src_format == dst_format && src_channels == dst_channels && src_rate == dst_rate) { cvt->needed = 0; return 0; }
    /* A real resampling/format-converting filter chain is not implemented;
     * refusing honestly (matches this backend's stated audio scope) rather
     * than silently returning a no-op conversion that corrupts the stream. */
    sdlpriv_set_error("SDL_BuildAudioCVT: format/rate conversion is not implemented on MayteraOS");
    return -1;
}
int SDL_ConvertAudio(SDL_AudioCVT *cvt) { (void)cvt; sdlpriv_set_error("SDL_ConvertAudio: not implemented"); return -1; }

void SDL_MixAudio(Uint8 *dst, const Uint8 *src, Uint32 len, int volume) {
    if (volume <= 0) return;
    if (volume > SDL_MIX_MAXVOLUME) volume = SDL_MIX_MAXVOLUME;
    /* S16LE mixing (the format every path in this backend actually produces
     * on the wire); an odd trailing byte, if any, is left untouched. */
    Uint32 nsamples = len / 2;
    Sint16 *d = (Sint16 *)dst; const Sint16 *s = (const Sint16 *)src;
    for (Uint32 i = 0; i < nsamples; i++) {
        int sv = (s[i] * volume) / SDL_MIX_MAXVOLUME;
        int mixed = (int)d[i] + sv;
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        d[i] = (Sint16)mixed;
    }
}
