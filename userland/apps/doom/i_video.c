// i_video.c - DOOM video for MayteraOS userland

#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "d_main.h"
#include "../../libc/syscall.h"

// Screen buffer
static uint32_t *screen_buffer = NULL;
static int win_handle = -1;

#define SCREENWIDTH 320
#define SCREENHEIGHT 200
#define SCALE 2

// Fullscreen mode
static int fullscreen_mode = 0;  // Start windowed, F11 for fullscreen
static uint32_t *framebuffer = NULL;
static int fb_width = 0;
static int fb_height = 0;
static int fb_pitch = 0;

// Fullscreen framebuffer functions (syscall wrappers)
// fb_map and fb_flip are in syscall.h; the rest are defined here.

static inline long fb_get_info(int *width, int *height, int *pitch) {
    fb_info_t info;  // fb_info_t defined in syscall.h
    long r = syscall1(201, (long)&info);  // SYS_FB_INFO
    if (r == 0) {
        if (width)  *width  = info.width;
        if (height) *height = info.height;
        if (pitch)  *pitch  = info.pitch;
    }
    return r;
}

static inline void fb_set_exclusive(int enable) {
    syscall1(213, (long)enable);  // SYS_GRAB_INPUT
}

// win_capture_mouse: syscall 90 (no-op stub in kernel)
static inline int win_capture_mouse(int handle, int capture) {
    return (int)syscall2(90, (long)handle, (long)capture);
}

// DOOM palette
static uint32_t doom_palette[256];

void I_InitGraphics(void) {
    // Boost timer to 1000Hz for smooth gameplay
    syscall1(94, 1000);  // SYS_TIMER_BOOST
    if (fullscreen_mode) {
        // Enter exclusive fullscreen mode
        fb_set_exclusive(1);
        
        // Get framebuffer info
        fb_get_info(&fb_width, &fb_height, &fb_pitch);
        printf("[DOOM] Fullscreen: %dx%d pitch=%d\n", fb_width, fb_height, fb_pitch);
        
        // Map framebuffer
        framebuffer = (uint32_t *)fb_map();
        if (!framebuffer) {
            I_Error("Failed to map framebuffer");
        }
    } else {
        // Create window (windowed mode)
        win_handle = win_create("DOOM", 100, 100, SCREENWIDTH * SCALE, SCREENHEIGHT * SCALE);
        if (win_handle >= 0) {
            // win_maximize(win_handle);  // Start at normal size, user can maximize
            win_capture_mouse(win_handle, 1);  // Capture mouse
        }
        if (win_handle < 0) {
            I_Error("Failed to create window");
        }
    }
    
    // Allocate screen buffer for palette conversion
    screen_buffer = (uint32_t *)sys_mmap(NULL, SCREENWIDTH * SCREENHEIGHT * 4, 3, 0);
    if (!screen_buffer) {
        I_Error("Failed to allocate screen buffer");
    }
}

void I_ShutdownGraphics(void) {
    // Release mouse capture
    if (win_handle >= 0) {
        win_capture_mouse(win_handle, 0);
    }
    // Restore timer to default 250Hz
    syscall1(94, 250);  // SYS_TIMER_BOOST
    if (fullscreen_mode) {
        // Exit exclusive mode
        fb_set_exclusive(0);
        framebuffer = NULL;
    } else {
        if (win_handle >= 0) {
            win_destroy(win_handle);
            win_handle = -1;
        }
    }
    if (screen_buffer) {
        sys_munmap(screen_buffer, SCREENWIDTH * SCREENHEIGHT * 4);
        screen_buffer = NULL;
    }
}

void I_SetPalette(byte *palette) {
    for (int i = 0; i < 256; i++) {
        int r = gammatable[usegamma][palette[i*3+0]];
        int g = gammatable[usegamma][palette[i*3+1]];
        int b = gammatable[usegamma][palette[i*3+2]];
        doom_palette[i] = (r << 16) | (g << 8) | b;
    }
}

void I_UpdateNoBlit(void) {
    // Nothing
}

void I_FinishUpdate(void) {
    if (!screen_buffer) return;
    
    // Convert indexed color to RGB - optimized single loop with unrolling
    byte *src = screens[0];
    uint32_t *dst = screen_buffer;
    int count = SCREENWIDTH * SCREENHEIGHT;
    
    // Process 8 pixels at a time
    int i = 0;
    for (; i + 7 < count; i += 8) {
        dst[i]   = doom_palette[src[i]];
        dst[i+1] = doom_palette[src[i+1]];
        dst[i+2] = doom_palette[src[i+2]];
        dst[i+3] = doom_palette[src[i+3]];
        dst[i+4] = doom_palette[src[i+4]];
        dst[i+5] = doom_palette[src[i+5]];
        dst[i+6] = doom_palette[src[i+6]];
        dst[i+7] = doom_palette[src[i+7]];
    }
    // Handle remaining pixels
    for (; i < count; i++) {
        dst[i] = doom_palette[src[i]];
    }
    
    if (fullscreen_mode && framebuffer) {
        // Direct framebuffer rendering with scaling
        // Calculate scale to fit screen while maintaining aspect ratio
        int scale_x = fb_width / SCREENWIDTH;
        int scale_y = fb_height / SCREENHEIGHT;
        int scale = (scale_x < scale_y) ? scale_x : scale_y;
        if (scale < 1) scale = 1;
        
        // Center the image
        int offset_x = (fb_width - SCREENWIDTH * scale) / 2;
        int offset_y = (fb_height - SCREENHEIGHT * scale) / 2;
        
        // Scale and blit to framebuffer
        int pitch_pixels = fb_pitch / 4;
        for (int sy = 0; sy < SCREENHEIGHT; sy++) {
            for (int sx = 0; sx < SCREENWIDTH; sx++) {
                uint32_t pixel = screen_buffer[sy * SCREENWIDTH + sx];
                // Draw scaled pixel
                for (int dy = 0; dy < scale; dy++) {
                    int fb_y = offset_y + sy * scale + dy;
                    if (fb_y >= 0 && fb_y < fb_height) {
                        for (int dx = 0; dx < scale; dx++) {
                            int fb_x = offset_x + sx * scale + dx;
                            if (fb_x >= 0 && fb_x < fb_width) {
                                framebuffer[fb_y * pitch_pixels + fb_x] = pixel;
                            }
                        }
                    }
                }
            }
        }
        
        // Present the frame
        fb_flip();
    } else if (win_handle >= 0) {
        // Windowed mode - blit to window
        syscall5(SYS_WIN_BLIT, win_handle, 0, 0, 
                 (uint32_t)SCREENWIDTH | ((uint32_t)SCREENHEIGHT << 16),
                 (uint64_t)screen_buffer);
        win_invalidate(win_handle);
    }
}

void I_ReadScreen(byte *scr) {
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

// GUI event structure (must match kernel's gui_event_t EXACTLY)
typedef struct {
    int type;               // event_type_t (4 bytes)
    unsigned int target_id; // uint32_t (4 bytes)
    int mouse_x;            // int32_t (4 bytes)
    int mouse_y;            // int32_t (4 bytes)
    unsigned int mouse_buttons; // uint32_t (4 bytes)
    signed char scroll_delta;   // int8_t (1 byte)
    // 3 bytes padding here
    unsigned int keycode;   // uint32_t (4 bytes)
    char key_char;          // char (1 byte)
    // 3 bytes padding
} win_event_t;

// Event types
#define EVENT_KEY_DOWN 5
#define EVENT_KEY_UP 6
#define EVENT_WINDOW_CLOSE 7

void I_StartTic(void) {
    static int tic_count = 0;
    if (++tic_count <= 2) {
        puts("[DOOM] I_StartTic called");
    }
    if (win_handle < 0) return;
    
    // Poll window events (non-blocking, timeout=0)
    win_event_t win_ev;
    event_t event;
    static int debug_count = 0;
    
    // SYS_WIN_GET_EVENT = 36
    int result;
    while ((result = syscall3(36, win_handle, (uint64_t)&win_ev, 0)) > 0) {
        // Debug: print first 10 events
        if (debug_count < 10) {
            printf("[DOOM] Event type=%d keycode=%d\n", win_ev.type, win_ev.keycode);
            debug_count++;
        }
        // Only handle keyboard events
        if (win_ev.type == EVENT_KEY_DOWN) {
            event.type = ev_keydown;
        } else if (win_ev.type == EVENT_KEY_UP) {
            event.type = ev_keyup;
        } else if (win_ev.type == EVENT_WINDOW_CLOSE) {
            sys_exit(0);
        } else {
            continue;  // Skip mouse events etc
        }
        
        int keycode = win_ev.keycode;
        
        // Convert keycode to DOOM key
        switch (keycode) {
            case 0x1B: event.data1 = KEY_ESCAPE; break;
            case 0x0D: case 0x0A: event.data1 = KEY_ENTER; break;  // Enter/LF
            case 0x20: event.data1 = ' '; break;
            // Arrow keys (may come as special codes)
            case 130: event.data1 = KEY_LEFTARROW; break;   // Left (KEY_LEFT = 0x82)
            case 128: event.data1 = KEY_UPARROW; break;     // Up (KEY_UP = 0x80)
            case 131: event.data1 = KEY_RIGHTARROW; break;  // Right (KEY_RIGHT = 0x83)
            case 129: event.data1 = KEY_DOWNARROW; break;   // Down (KEY_DOWN = 0x81)
            case 0x84: event.data1 = KEY_RCTRL; break;  // KEY_LCTRL = fire
            case 0x87: event.data1 = KEY_RSHIFT; break; // KEY_LSHIFT = run  
            case 0x88: event.data1 = KEY_RSHIFT; break; // KEY_RSHIFT = run
            // Q key for fire (alternate)
            case 'q': case 'Q': event.data1 = KEY_RCTRL; break;
            // WASD movement keys
            case 'w': case 'W': event.data1 = KEY_UPARROW; break;
            case 'a': case 'A': event.data1 = KEY_LEFTARROW; break;
            case 's': case 'S': event.data1 = KEY_DOWNARROW; break;
            case 'd': case 'D': event.data1 = KEY_RIGHTARROW; break;
            default:
                if (keycode >= 'a' && keycode <= 'z') {
                    event.data1 = keycode;
                } else if (keycode >= 'A' && keycode <= 'Z') {
                    event.data1 = keycode + 32;  // lowercase
                } else if (keycode >= 32 && keycode < 127) {
                    event.data1 = keycode;  // Printable ASCII
                } else {
                    continue;  // Skip unknown keys
                }
                break;
        }
        
        D_PostEvent(&event);
    }
}

void I_StartFrame(void) {
    // Nothing
}
