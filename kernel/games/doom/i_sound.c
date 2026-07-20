// i_sound.c - MayteraOS DOOM sound implementation
// Software mixer: 8 channels of 8-bit PCM mixed to S16_LE mono via audio.h

#include "i_maytera.h"
#include "../../drivers/audio.h"
#include <stdarg.h>

#include "z_zone.h"
#include "m_swap.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "doomdef.h"
#include "sounds.h"

// ============================================================================
// Mixer constants
// ============================================================================

#define NUM_CHANNELS    8
#define MIX_RATE        11025
#define MIX_FRAMES      512

// ============================================================================
// Channel state
// ============================================================================

typedef struct {
    const uint8_t *data;    // raw 8-bit unsigned PCM (after 8-byte DOOM header)
    int length;             // total sample count
    int pos;                // current playback position
    int vol;                // per-channel volume 0-127
    int active;             // 1 if currently playing
    int handle;             // handle returned to DOOM (channel index + 1)
} sfx_chan_t;

static sfx_chan_t g_channels[NUM_CHANNELS];
static audio_stream_t *g_stream = NULL;
static int g_sfx_vol = 127;         // master SFX volume 0-127
static int16_t g_mixbuf[MIX_FRAMES];

// Kept for ABI compatibility (referenced in original interface)
int lengths[NUMSFX];

// ============================================================================
// WAD lump helpers
// ============================================================================

void *I_GetSfxLumpData(int sfxlump, int *len)
{
    *len = W_LumpLength(sfxlump);
    return W_CacheLumpNum(sfxlump, PU_STATIC);
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

// ============================================================================
// Init / Shutdown
// ============================================================================

void I_InitSound(void)
{
    int i;
    kprintf("[DOOM] I_InitSound: opening audio stream at %d Hz mono\n", MIX_RATE);

    audio_config_t cfg;
    cfg.format      = AUDIO_FORMAT_S16_LE;
    cfg.sample_rate = MIX_RATE;
    cfg.channels    = AUDIO_CHANNELS_MONO;
    cfg.buffer_size = 0;
    cfg.period_size = 0;

    g_stream = audio_open(&cfg);
    if (!g_stream) {
        kprintf("[DOOM] I_InitSound: audio_open failed, sound disabled\n");
        return;
    }

    // Pre-fill with silence so the AC97 DMA ring has something to play
    {
        int16_t silence[256];
        int avail;
        for (i = 0; i < 256; i++) silence[i] = 0;
        avail = audio_avail(g_stream);
        while (avail > 0) {
            int n = (avail > 256) ? 256 : avail;
            audio_write_nonblock(g_stream, silence, (uint32_t)n);
            avail -= n;
        }
    }

    audio_start(g_stream);

    for (i = 0; i < NUM_CHANNELS; i++) {
        g_channels[i].active = 0;
        g_channels[i].data   = 0;
        g_channels[i].pos    = 0;
        g_channels[i].length = 0;
        g_channels[i].vol    = 0;
        g_channels[i].handle = i + 1;
    }

    kprintf("[DOOM] I_InitSound: ready\n");
}

void I_ShutdownSound(void)
{
    if (g_stream) {
        audio_close(g_stream);
        g_stream = NULL;
    }
    kprintf("[DOOM] Sound shutdown\n");
}

// ============================================================================
// Volume control
// ============================================================================

void I_SetChannels(void)
{
    // Nothing needed for software mixer
}

void I_SetSfxVolume(int volume)
{
    g_sfx_vol = volume;    // 0-127
}

void I_SetMusicVolume(int volume)
{
    (void)volume;
}

// ============================================================================
// Playback
// ============================================================================

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    int i, ch;
    (void)sep;
    (void)pitch;
    (void)priority;

    if (!g_stream) return 0;
    if (id <= 0 || id >= NUMSFX) return 0;

    // Resolve lump number on first use
    if (S_sfx[id].lumpnum < 0)
        S_sfx[id].lumpnum = I_GetSfxLumpNum(&S_sfx[id]);
    if (S_sfx[id].lumpnum < 0)
        return 0;

    {
        int lump_len = 0;
        const uint8_t *lump = (const uint8_t *)I_GetSfxLumpData(S_sfx[id].lumpnum, &lump_len);
        int num_samples;
        if (!lump || lump_len < 9) return 0;

        // DOOM sound lump header (8 bytes):
        //   uint16 format  (3 = raw PCM)
        //   uint16 sample_rate
        //   uint32 num_samples
        // Followed immediately by raw 8-bit unsigned PCM data
        num_samples = (int)(
              ((uint32_t)lump[4])
            | ((uint32_t)lump[5] <<  8)
            | ((uint32_t)lump[6] << 16)
            | ((uint32_t)lump[7] << 24));
        if (num_samples <= 0 || lump_len < 8 + num_samples)
            num_samples = lump_len - 8;
        if (num_samples <= 0) return 0;

        // Find a free channel; steal channel 0 as a last resort
        ch = -1;
        for (i = 0; i < NUM_CHANNELS; i++) {
            if (!g_channels[i].active) { ch = i; break; }
        }
        if (ch < 0) ch = 0;

        g_channels[ch].data   = lump + 8;
        g_channels[ch].length = num_samples;
        g_channels[ch].pos    = 0;
        g_channels[ch].vol    = vol;
        g_channels[ch].active = 1;
    }

    return g_channels[ch].handle;
}

void I_StopSound(int handle)
{
    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (g_channels[i].handle == handle) {
            g_channels[i].active = 0;
            break;
        }
    }
}

int I_SoundIsPlaying(int handle)
{
    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (g_channels[i].handle == handle && g_channels[i].active)
            return 1;
    }
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    int i;
    (void)sep;
    (void)pitch;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (g_channels[i].handle == handle) {
            g_channels[i].vol = vol;
            break;
        }
    }
}

// ============================================================================
// Mixer: mix active channels and push to audio device
// Called once per game tic (~35 Hz) from DOOM's main loop
// ============================================================================

void I_UpdateSound(void)
{
    int avail;
    if (!g_stream) return;

    avail = audio_avail(g_stream);
    if (avail <= 0) return;

    while (avail > 0) {
        int frames = (avail > MIX_FRAMES) ? MIX_FRAMES : avail;
        int f, ch;

        for (f = 0; f < frames; f++) {
            int32_t acc = 0;

            for (ch = 0; ch < NUM_CHANNELS; ch++) {
                int16_t samp;
                if (!g_channels[ch].active) continue;
                if (g_channels[ch].pos >= g_channels[ch].length) {
                    g_channels[ch].active = 0;
                    continue;
                }

                // Convert 8-bit unsigned to signed 16-bit, scale by volume.
                // Left-shift by 7 puts the signal near 16-bit range before
                // the volume divide.
                samp = (int16_t)(((int)g_channels[ch].data[g_channels[ch].pos] - 128) << 7);
                acc += (samp * g_channels[ch].vol) >> 7;
                g_channels[ch].pos++;
            }

            // Apply master SFX volume and clip to int16 range
            acc = (acc * g_sfx_vol) >> 7;
            if (acc >  32767) acc =  32767;
            if (acc < -32768) acc = -32768;
            g_mixbuf[f] = (int16_t)acc;
        }

        audio_write_nonblock(g_stream, g_mixbuf, (uint32_t)frames);
        avail -= frames;
    }
}

void I_SubmitSound(void)
{
    // I_UpdateSound handles all submission
}

// ============================================================================
// Music stubs (MIDI not implemented)
// ============================================================================

void I_InitMusic(void)
{
    kprintf("[DOOM] Music: not implemented\n");
}

void I_ShutdownMusic(void) {}

void I_PlaySong(int handle, int looping)
{
    (void)handle;
    (void)looping;
}

void I_PauseSong(int handle)      { (void)handle; }
void I_ResumeSong(int handle)     { (void)handle; }
void I_StopSong(int handle)       { (void)handle; }
void I_UnRegisterSong(int handle) { (void)handle; }

int I_RegisterSong(void *data)
{
    (void)data;
    return 0;
}

int I_QrySongPlaying(int handle)
{
    (void)handle;
    return 0;
}
