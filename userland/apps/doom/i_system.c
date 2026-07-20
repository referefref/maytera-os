// i_system.c - System interface for DOOM (userland)

#include "doomdef.h"
#include <stdarg.h>
#include "doomstat.h"
#include "d_event.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"
#include "i_video.h"
#include "m_misc.h"
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"

// Timer base
static uint32_t basetime = 0;

// Debug print via syscall
// Debug output (0=disabled for performance, 1=enabled)
#define DOOM_DEBUG 0

static void debug_print(const char *msg) {
#if DOOM_DEBUG
    while (*msg) {
        syscall1(SYS_PUTCHAR, *msg++);
    }
#else
    (void)msg;
#endif
}

static void debug_hex(uint64_t val) {
    char buf[20];
    char *p = buf + 18;
    *--p = 0;
    *--p = '\n';
    if (val == 0) {
        *--p = '0';
    } else {
        while (val) {
            int d = val & 0xF;
            *--p = d < 10 ? '0' + d : 'a' + d - 10;
            val >>= 4;
        }
    }
    *--p = 'x';
    *--p = '0';
    debug_print(p);
}

void I_Init(void) {
    debug_print("[DOOM] I_Init called\n");
    basetime = (uint32_t)sys_clock();
}

// Returns time in 1/35th second tics.
// sys_clock() returns raw kernel PIT ticks at 250 Hz (not milliseconds),
// so divide by the actual timer rate to convert ticks to seconds.
#define KERNEL_TIMER_HZ 250
int I_GetTime(void) {
    uint32_t now = (uint32_t)sys_clock();
    return ((now - basetime) * TICRATE) / KERNEL_TIMER_HZ;
}

byte *I_ZoneBase(int *size) {
    debug_print("[DOOM] I_ZoneBase: Requesting zone memory\n");
    
    // Request 8MB for the zone
    int zone_size = 8 * 1024 * 1024;
    *size = zone_size;
    
    debug_print("[DOOM] I_ZoneBase: Calling sys_mmap for 8MB...\n");
    
    void *ptr = sys_mmap(NULL, zone_size, 3, 0);
    
    debug_print("[DOOM] I_ZoneBase: sys_mmap returned: ");
    debug_hex((uint64_t)ptr);
    
    // Check for failure
    if (ptr == (void*)-1 || ptr == NULL) {
        debug_print("[DOOM] I_ZoneBase: FAILED! Returning NULL\n");
        *size = 0;
        return NULL;
    }
    
    debug_print("[DOOM] I_ZoneBase: SUCCESS! Size set to: ");
    debug_hex(*size);
    
    memset(ptr, 0, zone_size);
    return (byte *)ptr;
}

void I_Quit(void) {
    I_ShutdownGraphics();
    sys_exit(0);
}

void I_WaitVBL(int count) {
    sys_sleep(count * 1000 / 70);
}

void I_BeginRead(void) {
    // Nothing
}

void I_EndRead(void) {
    // Nothing
}

void I_Error(char *error, ...) {
    va_list ap;
    va_start(ap, error);
    printf("[DOOM ERROR] ");
    vprintf(error, ap);
    printf("\n");
    va_end(ap);
    I_ShutdownGraphics();
    sys_exit(1);
}

ticcmd_t emptycmd;

ticcmd_t *I_BaseTiccmd(void) {
    return &emptycmd;
}
