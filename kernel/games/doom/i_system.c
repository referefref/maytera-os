// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_bbox.c,v 1.1 1997/02/03 22:45:10 b1 Exp $";


// MayteraOS port - no stdlib needed, using i_maytera.h
#include "i_maytera.h"

#include <stdarg.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"

#include "d_net.h"
#include "g_game.h"

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"




int	mb_used = 6;


void
I_Tactile
( int	on,
  int	off,
  int	total )
{
  // UNUSED.
  on = off = total = 0;
}

ticcmd_t	emptycmd;
ticcmd_t*	I_BaseTiccmd(void)
{
    return &emptycmd;
}


int  I_GetHeapSize (void)
{
    return mb_used*1024*1024;
}

byte* I_ZoneBase (int*	size)
{
    *size = mb_used*1024*1024;
    return (byte *) malloc (*size);
}



//
// I_GetTime
// returns time in 1/70th second tics
// MayteraOS: Use timer_ticks from ISR (100 Hz timer)
//
int  I_GetTime (void)
{
    static uint64_t basetime = 0;

    if (!basetime)
        basetime = timer_ticks;

    // timer_ticks is 100 Hz, TICRATE is 35
    // Convert: (elapsed_ticks * 35) / 100
    uint64_t elapsed = timer_ticks - basetime;
    return (int)((elapsed * TICRATE) / 100);
}



//
// I_Init
//
void I_Init (void)
{
    I_InitSound();
    //  I_InitGraphics();
}

//
// I_Quit
// MayteraOS: Close DOOM window instead of exit()
//
void I_Quit (void)
{
    D_QuitNetGame ();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults ();
    I_ShutdownGraphics();
    // MayteraOS: Signal doom to stop running
    doom_running = 0;
}

void I_WaitVBL(int count)
{
    // MayteraOS: Wait for count * (1/70) seconds using timer_ticks
    // 1/70 second = ~1.43 ticks at 100 Hz
    // Wait for approximately count * 1.43 ticks
    uint64_t wait_ticks = (count * 100) / 70;
    if (wait_ticks < 1) wait_ticks = 1;
    uint64_t target = timer_ticks + wait_ticks;
    while (timer_ticks < target) {
        // Busy wait - could add __asm__ volatile("pause") for power saving
        __asm__ volatile("pause");
    }
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte*	I_AllocLow(int length)
{
    byte*	mem;
        
    mem = (byte *)malloc (length);
    memset (mem,0,length);
    return mem;
}


//
// I_Error
// MayteraOS: Use kprintf instead of fprintf(stderr)
//
extern boolean demorecording;

void I_Error (char *error, ...)
{
    va_list argptr;
    char buf[512];

    // Format error message
    va_start(argptr, error);
    // Simple format - just print the format string for now
    kprintf("[DOOM] Error: %s\n", error);
    va_end(argptr);

    // Shutdown. Here might be other errors.
    if (demorecording)
        G_CheckDemoStatus();

    D_QuitNetGame();
    I_ShutdownGraphics();

    // MayteraOS: Signal doom to stop running instead of exit
    doom_running = 0;
}
