// i_video_maytera.c - MayteraOS video implementation for DOOM
// Copyright (C) 1993-1996 by id Software, Inc.
// MayteraOS port

#include "i_maytera.h"
#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"
#include "doomdef.h"

// DOOM screen buffer (320x200 indexed color)
extern byte *screens[5];

// Palette (256 colors, RGB)
uint32_t doom_palette[256];

// MayteraOS window
window_t *doom_window = NULL;
int doom_running = 0;
int doom_app_id = -1;
static int doom_dock_index = -1;

// Multiply factor for scaling
static int multiply = 2;

// Exclusive fullscreen mode
static bool doom_exclusive_mode = true;
static int doom_screen_x = 0;
static int doom_screen_y = 0;
static uint32_t *doom_scaled_buffer = NULL;

// Key translation table
static int doom_keys[256];

// Initialize key translation
static void I_InitKeyTranslation(void)
{
    memset(doom_keys, 0, sizeof(doom_keys));

    // Letters (assuming ASCII input from keyboard_get_char)
    for (int i = 'a'; i <= 'z'; i++) {
        doom_keys[i] = i;
    }
    for (int i = 'A'; i <= 'Z'; i++) {
        doom_keys[i] = i - 'A' + 'a';
    }

    // Numbers
    for (int i = '0'; i <= '9'; i++) {
        doom_keys[i] = i;
    }

    // Special keys
    doom_keys[27] = KEY_ESCAPE;        // ESC
    doom_keys[13] = KEY_ENTER;         // Enter
    doom_keys[' '] = ' ';              // Space
    doom_keys['\t'] = KEY_TAB;         // Tab
    doom_keys[8] = KEY_BACKSPACE;      // Backspace

    // Arrow keys: MKEY_ values are now MayteraOS key codes (0x80-0x83)
    doom_keys[MKEY_UP] = KEY_UPARROW;
    doom_keys[MKEY_DOWN] = KEY_DOWNARROW;
    doom_keys[MKEY_LEFT] = KEY_LEFTARROW;
    doom_keys[MKEY_RIGHT] = KEY_RIGHTARROW;

    // Function keys: only F11 and F12 produce key codes from the ISR
    // F1-F10 are not available (MKEY_F1-F10 are 0x00, so these lines are no-ops)
    if (MKEY_F11) doom_keys[MKEY_F11] = KEY_F11;
    if (MKEY_F12) doom_keys[MKEY_F12] = KEY_F12;

    // Modifier keys (MayteraOS key codes 0x84-0x89)
    doom_keys[MKEY_LSHIFT] = KEY_RSHIFT;   // 0x87 -> DOOM shift
    doom_keys[MKEY_RSHIFT] = KEY_RSHIFT;   // 0x88 -> DOOM shift
    doom_keys[MKEY_LCTRL] = KEY_RCTRL;     // 0x84 -> DOOM ctrl
    doom_keys[MKEY_LALT] = KEY_RALT;       // 0x89 -> DOOM alt
}

int I_MayteraTranslateKey(int keycode)
{
    if (keycode >= 0 && keycode < 256) {
        return doom_keys[keycode];
    }
    return 0;
}

// Set palette from DOOM playpal
void I_SetPalette(byte *palette)
{
    for (int i = 0; i < 256; i++) {
        byte r = gammatable[usegamma][*palette++];
        byte g = gammatable[usegamma][*palette++];
        byte b = gammatable[usegamma][*palette++];
        // BGRA format for framebuffer
        doom_palette[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }
}

// Update screen - blit DOOM framebuffer to window
void I_FinishUpdate(void)
{
    if (!screens[0]) return;

    byte *src = screens[0];
    int dest_x = doom_screen_x;
    int dest_y = doom_screen_y;

    if (!doom_exclusive_mode && doom_window) {
        int32_t wx, wy, ww, wh;
        window_get_content_bounds(doom_window, &wx, &wy, &ww, &wh);
        dest_x = wx;
        dest_y = wy;
    }

    if (doom_scaled_buffer) {
        // Fast: scale to buffer then blit
        uint32_t *dst = doom_scaled_buffer;
        for (int y = 0; y < DOOM_SCREENHEIGHT; y++) {
            int sr = y * DOOM_SCREENWIDTH;
            int dr = (y * 2) * DOOM_WINDOW_WIDTH;
            for (int x = 0; x < DOOM_SCREENWIDTH; x++) {
                uint32_t c = doom_palette[src[sr + x]];
                int dc = x * 2;
                dst[dr + dc] = c;
                dst[dr + dc + 1] = c;
                dst[dr + DOOM_WINDOW_WIDTH + dc] = c;
                dst[dr + DOOM_WINDOW_WIDTH + dc + 1] = c;
            }
        }
        fb_blit(dest_x, dest_y, DOOM_WINDOW_WIDTH, DOOM_WINDOW_HEIGHT, doom_scaled_buffer);
    } else {
        // Slow: per-pixel
        for (int y = 0; y < DOOM_SCREENHEIGHT; y++) {
            for (int x = 0; x < DOOM_SCREENWIDTH; x++) {
                uint32_t c = doom_palette[src[y * DOOM_SCREENWIDTH + x]];
                for (int sy = 0; sy < 2; sy++)
                    for (int sx = 0; sx < 2; sx++)
                        fb_put_pixel(dest_x + x*2 + sx, dest_y + y*2 + sy, c);
            }
        }
    }

    // Present the frame. In exclusive mode, the desktop drawing loop
    // is skipped (desktop_process_tick returns early), so DOOM must
    // swap its own buffers.
    if (doom_exclusive_mode) {
        extern void fb_swap_buffers(void);
        fb_swap_buffers();
    }
}

// Read screen (for screenshots, wipes, etc.)
void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], DOOM_SCREENWIDTH * DOOM_SCREENHEIGHT);
}

// Start frame
void I_StartFrame(void)
{
    // Nothing needed for MayteraOS
}

// Process events from MayteraOS
void I_StartTic(void)
{
    event_t event;

    // Poll keyboard
    while (keyboard_has_char()) {
        int key = keyboard_get_char();
        int doomkey = 0;
        int is_release = 0;

        // Check for special key releases (0x90-0x98 range)
        if (key == MKEY_UP_REL)     { doomkey = doom_keys[MKEY_UP]; is_release = 1; }
        else if (key == MKEY_DOWN_REL)  { doomkey = doom_keys[MKEY_DOWN]; is_release = 1; }
        else if (key == MKEY_LEFT_REL)  { doomkey = doom_keys[MKEY_LEFT]; is_release = 1; }
        else if (key == MKEY_RIGHT_REL) { doomkey = doom_keys[MKEY_RIGHT]; is_release = 1; }
        else if (key == MKEY_LCTRL_UP)  { doomkey = doom_keys[MKEY_LCTRL]; is_release = 1; }
        else if (key == MKEY_LSHIFT_UP) { doomkey = doom_keys[MKEY_LSHIFT]; is_release = 1; }
        else if (key == MKEY_RSHIFT_UP) { doomkey = doom_keys[MKEY_RSHIFT]; is_release = 1; }
        // ASCII key releases (base char | 0x80, produces 0xA0+ for printable chars)
        else if (key >= 0xA0 && key <= 0xFE) {
            int base = key & 0x7F;
            doomkey = I_MayteraTranslateKey(base);
            is_release = 1;
        }
        // Regular key press
        else {
            doomkey = I_MayteraTranslateKey(key);
        }

        if (doomkey) {
            event.type = is_release ? ev_keyup : ev_keydown;
            event.data1 = doomkey;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
        }
    }

    // Poll mouse (get state and clear accumulated deltas atomically)
    mouse_state_t ms;
    mouse_get_state_and_clear(&ms);

    static uint8_t last_buttons = 0;

    // Mouse movement
    if (ms.dx != 0 || ms.dy != 0) {
        event.type = ev_mouse;
        event.data1 = (ms.buttons & MOUSE_LEFT_BTN ? 1 : 0) |
                      (ms.buttons & MOUSE_RIGHT_BTN ? 2 : 0) |
                      (ms.buttons & MOUSE_MIDDLE_BTN ? 4 : 0);
        event.data2 = ms.dx * 4;  // Scale mouse movement
        event.data3 = -ms.dy * 4;
        D_PostEvent(&event);
    }

    // Mouse buttons
    if (ms.buttons != last_buttons) {
        event.type = ev_mouse;
        event.data1 = (ms.buttons & MOUSE_LEFT_BTN ? 1 : 0) |
                      (ms.buttons & MOUSE_RIGHT_BTN ? 2 : 0) |
                      (ms.buttons & MOUSE_MIDDLE_BTN ? 4 : 0);
        event.data2 = 0;
        event.data3 = 0;
        D_PostEvent(&event);
        last_buttons = ms.buttons;
    }
}

// Update input (called from game loop)
void I_UpdateNoBlit(void)
{
    // Nothing needed
}

// Initialize graphics
void I_InitGraphics(void)
{
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    kprintf("[DOOM] Init graphics %dx%d, screen %ux%u, exclusive=%d\n",
            DOOM_SCREENWIDTH, DOOM_SCREENHEIGHT, screen_w, screen_h, doom_exclusive_mode);

    I_InitKeyTranslation();

    // Calculate centered position
    doom_screen_x = (screen_w - DOOM_WINDOW_WIDTH) / 2;
    doom_screen_y = (screen_h - DOOM_WINDOW_HEIGHT) / 2;

    if (doom_exclusive_mode) {
        wm_enter_exclusive_mode();
        kprintf("[DOOM] Entered exclusive fullscreen\n");
    } else if (!doom_window) {
        doom_window = window_create("DOOM", doom_screen_x, doom_screen_y - 30,
                                    DOOM_WINDOW_WIDTH, DOOM_WINDOW_HEIGHT + TITLEBAR_HEIGHT);
        if (doom_window) doom_window->bg_color = 0x000000;
    }

    screens[0] = (byte *)malloc(DOOM_SCREENWIDTH * DOOM_SCREENHEIGHT);
    if (!screens[0]) I_Error("Failed to allocate screen buffer");
    memset(screens[0], 0, DOOM_SCREENWIDTH * DOOM_SCREENHEIGHT);

    doom_scaled_buffer = (uint32_t *)malloc(DOOM_WINDOW_WIDTH * DOOM_WINDOW_HEIGHT * 4);

    doom_running = 1;
}

// Shutdown graphics
void I_ShutdownGraphics(void)
{
    kprintf("[DOOM] Shutting down graphics\n");

    if (screens[0]) { free(screens[0]); screens[0] = NULL; }
    if (doom_scaled_buffer) { free(doom_scaled_buffer); doom_scaled_buffer = NULL; }

    if (doom_exclusive_mode) {
        wm_exit_exclusive_mode();
        kprintf("[DOOM] Exited exclusive fullscreen\n");
    }

    if (doom_window) {
        if (doom_app_id >= 0) { wm_unregister_app(doom_app_id); doom_app_id = -1; }
        if (doom_dock_index >= 0) { dock_remove_app(doom_dock_index); doom_dock_index = -1; }
        window_destroy(doom_window);
        doom_window = NULL;
    }

    doom_running = 0;
}

// Gamma table and usegamma are defined in v_video.c
extern byte gammatable[5][256];
extern int usegamma;
