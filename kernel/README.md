# DOOM Port for MayteraOS

This directory will contain the port of id Software's DOOM to MayteraOS.

## Source
Official DOOM source code: https://github.com/id-Software/DOOM
Directory: linuxdoom-1.10

## Files to Port

### Core Engine (copy directly)
- `am_map.c/h` - Automap
- `d_items.c/h` - Item definitions
- `d_main.c/h` - Main game logic
- `d_net.c/h` - Network/demo playback
- `doomdef.c/h` - Core definitions
- `doomstat.c/h` - Game state
- `f_finale.c/h` - End screen
- `f_wipe.c/h` - Screen wipe effects
- `g_game.c/h` - Game logic
- `hu_lib.c/h` - HUD library
- `hu_stuff.c/h` - HUD rendering
- `info.c/h` - Thing info
- `m_*.c/h` - Miscellaneous (argv, bbox, cheat, fixed, menu, misc, random, swap)
- `p_*.c/h` - Playsim (ceiling, doors, enemy, floor, inter, lights, map, mobj, plats, pspr, saveg, setup, sight, spec, switch, telept, tick, user)
- `r_*.c/h` - Renderer (bsp, data, draw, main, plane, segs, sky, things)
- `s_sound.c/h` - Sound system
- `sounds.c/h` - Sound definitions
- `st_lib.c/h` - Status bar library
- `st_stuff.c/h` - Status bar
- `tables.c/h` - Trig tables
- `v_video.c/h` - Video primitives
- `w_wad.c/h` - WAD file loader
- `wi_stuff.c/h` - Intermission screen
- `z_zone.c/h` - Zone memory allocator

### Platform Layer (must be rewritten for MayteraOS)
- `i_main.c` - Entry point (replace with MayteraOS launcher)
- `i_net.c/h` - Network (stub or use MayteraOS network stack)
- `i_sound.c/h` - Sound (use MayteraOS sound driver)
- `i_system.c/h` - System calls (adapt for MayteraOS)
- `i_video.c/h` - Video output (use MayteraOS framebuffer)

## MayteraOS Platform Layer

### i_video.c - Video Output
```c
// Use MayteraOS framebuffer
extern void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
extern uint32_t fb_get_width(void);
extern uint32_t fb_get_height(void);

// DOOM uses 320x200 palette mode, scale to window
void I_FinishUpdate(void) {
    // Blit DOOM's screens[0] to MayteraOS window
    // Scale 320x200 to window size
    // Convert palette indices to RGB
}
```

### i_system.c - System Interface
```c
// Memory allocation - use MayteraOS heap
void* I_AllocLow(int length) { return kmalloc(length); }

// Time - use MayteraOS timer
int I_GetTime(void) { return timer_ticks * TICRATE / 1000; }

// Exit - return to desktop
void I_Quit(void) { /* close DOOM window */ }
```

### i_sound.c - Sound Output
```c
// Use MayteraOS sound driver (sound.c)
// DOOM uses 11025Hz 8-bit mono samples
void I_UpdateSound(void) {
    // Mix channels and output to sound driver
}
```

### i_main.c - Entry Point
```c
// Replace with MayteraOS GUI launcher
void doom_launch(void) {
    // Create window
    // Load DOOM1.WAD from filesystem
    // Call D_DoomMain()
}
```

## WAD File

DOOM requires a WAD file (DOOM1.WAD for shareware, DOOM.WAD for full game).
This should be loaded from the FAT filesystem.

## Build Integration

Add to kernel/Makefile:
```makefile
DOOM_DIR = games/doom
DOOM_SOURCES = $(wildcard $(DOOM_DIR)/*.c)
```

## Steps to Port

1. Copy all .c/.h files from linuxdoom-1.10
2. Create MayteraOS platform layer (i_video.c, i_system.c, i_sound.c)
3. Remove i_main.c, replace with doom_launch.c
4. Fix any libc dependencies (use MayteraOS string.h, etc.)
5. Add WAD loading from FAT filesystem
6. Integrate with GUI window manager
7. Test with DOOM1.WAD (shareware)

## License

DOOM source code is GPLv2. See original repository for license terms.
