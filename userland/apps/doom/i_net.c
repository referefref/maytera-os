// i_net.c - MayteraOS network stubs for DOOM
// Copyright (C) 1993-1996 by id Software, Inc.
// MayteraOS port - Multiplayer disabled for single player mode

#include "i_maytera.h"
#include "i_system.h"
#include "d_event.h"
#include "d_net.h"
#include "m_argv.h"
#include "doomstat.h"

// Stub implementation - no network support for now

void I_InitNetwork(void)
{
    doomcom = malloc(sizeof(*doomcom));
    memset(doomcom, 0, sizeof(*doomcom));

    // Single player mode
    doomcom->id = DOOMCOM_ID;
    doomcom->ticdup = 1;
    doomcom->extratics = 0;
    doomcom->numnodes = 1;
    doomcom->numplayers = 1;
    doomcom->consoleplayer = 0;

    kprintf("[DOOM] Network: Single player mode (multiplayer disabled)\n");
}

void I_NetCmd(void)
{
    // No-op for single player
}
