// i_sound.c - MayteraOS sound stubs for DOOM
// Copyright (C) 1993-1996 by id Software, Inc.
// MayteraOS port - Sound disabled for initial port

#include "i_maytera.h"
#include <stdarg.h>

#include "z_zone.h"
#include "m_swap.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "doomdef.h"

// Sound is disabled for now
#define SOUND_DISABLED 1

// The number of internal mixing channels
#define NUM_CHANNELS 8

// Needed for calling the actual sound output.
#define SAMPLECOUNT 512
#define NUM_CHANNELS 8
#define BUFMUL 4
#define MIXBUFFERSIZE (SAMPLECOUNT*BUFMUL)

// The actual lengths of all sound effects.
int lengths[NUMSFX];

// The global mixing buffer (not used yet)
signed short mixbuffer[MIXBUFFERSIZE];

void I_SetChannels()
{
    // Sound disabled
}

void I_SetSfxVolume(int volume)
{
    (void)volume;
}

void I_SetMusicVolume(int volume)
{
    (void)volume;
}

// Get raw sample data from the lump
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

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    (void)id;
    (void)vol;
    (void)sep;
    (void)pitch;
    (void)priority;
    // Sound disabled
    return 0;
}

void I_StopSound(int handle)
{
    (void)handle;
}

int I_SoundIsPlaying(int handle)
{
    (void)handle;
    return 0;
}

void I_UpdateSound(void)
{
    // Sound disabled
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    (void)handle;
    (void)vol;
    (void)sep;
    (void)pitch;
    // Sound disabled
}

void I_SubmitSound(void)
{
    // Sound disabled
}

void I_ShutdownSound(void)
{
    kprintf("[DOOM] Sound shutdown\n");
}

void I_InitSound(void)
{
    kprintf("[DOOM] Sound: Disabled for initial port\n");
}

// Music functions - all stubs

void I_InitMusic(void)
{
    kprintf("[DOOM] Music: Disabled for initial port\n");
}

void I_ShutdownMusic(void)
{
}

void I_PlaySong(int handle, int looping)
{
    (void)handle;
    (void)looping;
}

void I_PauseSong(int handle)
{
    (void)handle;
}

void I_ResumeSong(int handle)
{
    (void)handle;
}

void I_StopSong(int handle)
{
    (void)handle;
}

void I_UnRegisterSong(int handle)
{
    (void)handle;
}

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
