// i_maytera.h - MayteraOS platform definitions for DOOM
// Replaces standard library and provides MayteraOS integration

#ifndef I_MAYTERA_H
#define I_MAYTERA_H

// MayteraOS headers
#include "../../types.h"

// Undefine MayteraOS bool macros to avoid conflict with DOOM's boolean enum
#undef true
#undef false

#include "../../string.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../video/framebuffer.h"
#include "../../video/font.h"
#include "../../cpu/isr.h"
#include "../../drivers/mouse.h"
#include "../../drivers/sound.h"
#include "../../fs/fat.h"
#include "../../gui/window.h"
#include "../../gui/desktop.h"
#include "../../gui/syslog.h"
#include <stdarg.h>

// Dummy FILE type for DOOM (not actually used)
typedef void FILE;
#define stderr ((FILE*)0)
#define stdout ((FILE*)1)

// File access constants
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

// Forward declarations for doom_access
extern fat_fs_t g_fat_fs;
int fat_exists(fat_fs_t *fs, const char *path);

// access() function - check if file exists using FAT filesystem
static inline int doom_access(const char *path, int mode) {
    (void)mode;

    // Convert path to uppercase for FAT
    char fat_path[256];
    int j = 0;
    for (int i = 0; path[i] && j < 255; i++) {
        char c = path[i];
        if (c == '\\') c = '/';
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        fat_path[j++] = c;
    }
    fat_path[j] = '\0';

    // Prepend / if not present
    char final_path[260];
    if (fat_path[0] != '/') {
        final_path[0] = '/';
        strncpy(final_path + 1, fat_path, 258);
        final_path[259] = '\0';
    } else {
        strncpy(final_path, fat_path, 259);
        final_path[259] = '\0';
    }

    // Check if file exists using fat_exists
    int exists = fat_exists(&g_fat_fs, final_path);
    kprintf("[DOOM] access('%s') -> %s\n", final_path, exists ? "EXISTS" : "NOT FOUND");
    if (exists) {
        return 0;     // Success - file exists
    }
    return -1;        // File doesn't exist
}
#define access(path, mode) doom_access(path, mode)

// ctype.h functions (already in string.h but repeat here for safety)
#ifndef _CTYPE_DEFINED
#define _CTYPE_DEFINED
static inline int doom_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static inline int doom_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int doom_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int doom_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int doom_isalnum(int c) { return doom_isalpha(c) || doom_isdigit(c); }
static inline int doom_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static inline int doom_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int doom_islower(int c) { return c >= 'a' && c <= 'z'; }
#endif

// Timer from ISR
extern volatile uint64_t timer_ticks;

// Filesystem
extern fat_fs_t g_fat_fs;

// Standard library replacements
#define malloc(size)        kmalloc(size)
#define free(ptr)           kfree(ptr)
#define realloc(ptr, size)  krealloc(ptr, size)
#define calloc(n, size)     kzalloc((n) * (size))

// ============================================================================
// File I/O Wrappers (using FAT filesystem)
// ============================================================================

// File handle structure
typedef struct {
    fat_file_t fat_file;
    uint8_t *buffer;        // File contents loaded into memory
    uint32_t size;          // Total file size
    uint32_t pos;           // Current read position
    int valid;              // Is this handle valid?
} doom_file_t;

// Maximum open files
#define DOOM_MAX_FILES 16
extern doom_file_t doom_file_table[DOOM_MAX_FILES];

// File open flags
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_BINARY    0
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// Stub struct stat (for M_ReadFile)
struct stat {
    int st_mode;
    int st_size;
};

// stat/fstat stubs (always fail since we don't really support this)
#define stat(path, buf) (-1)
#define fstat(fd, buf)  (-1)

// File I/O functions
int doom_open(const char *path, int flags);
int doom_close(int fd);
int doom_read(int fd, void *buf, int count);
int doom_lseek(int fd, int offset, int whence);
int doom_filelength(int fd);

// Write stub (FAT is read-only for now)
static inline int doom_write(int fd, const void *buf, int count) {
    (void)fd; (void)buf; (void)count;
    return -1;  // Always fail - read only filesystem
}

// Map standard names to doom_ versions
#define open(path, flags, ...)  doom_open(path, flags)
#define close(fd)               doom_close(fd)
#define read(fd, buf, count)    doom_read(fd, buf, count)
#define write(fd, buf, count)   doom_write(fd, buf, count)
#define lseek(fd, off, whence)  doom_lseek(fd, off, whence)
#define filelength(fd)          doom_filelength(fd)

// ============================================================================
// Printf / String Functions
// ============================================================================

// printf goes to serial
#define printf(...)     kprintf(__VA_ARGS__)
#define fprintf(f, ...) kprintf(__VA_ARGS__)
#define vfprintf(f, fmt, ap) kprintf(fmt)
#define fflush(f)       ((void)0)

// Simple sprintf implementation for DOOM
static inline int doom_sprintf(char *buf, const char *fmt, ...) {
    // Very simple implementation - just copy format string
    // DOOM mostly uses simple formats like "AMMNUM%d"
    va_list ap;
    va_start(ap, fmt);

    char *p = buf;
    const char *f = fmt;

    while (*f) {
        if (*f == '%') {
            f++;
            if (*f == 'd' || *f == 'i') {
                int val = va_arg(ap, int);
                // Convert int to string
                if (val < 0) { *p++ = '-'; val = -val; }
                char tmp[12];
                int i = 0;
                if (val == 0) tmp[i++] = '0';
                while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
                while (i > 0) *p++ = tmp[--i];
                f++;
            } else if (*f == 's') {
                char *s = va_arg(ap, char*);
                while (*s) *p++ = *s++;
                f++;
            } else if (*f == 'c') {
                *p++ = (char)va_arg(ap, int);
                f++;
            } else if (*f == '%') {
                *p++ = '%';
                f++;
            } else {
                *p++ = '%';
                *p++ = *f++;
            }
        } else {
            *p++ = *f++;
        }
    }
    *p = '\0';
    va_end(ap);
    return p - buf;
}
#define sprintf doom_sprintf

// String comparison functions
static inline int doom_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = doom_tolower((unsigned char)*s1);
        int c2 = doom_tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return doom_tolower((unsigned char)*s1) - doom_tolower((unsigned char)*s2);
}

static inline int doom_strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s2) {
        int c1 = doom_tolower((unsigned char)*s1);
        int c2 = doom_tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return doom_tolower((unsigned char)*s1) - doom_tolower((unsigned char)*s2);
}

#define strcasecmp(a, b)      doom_strcasecmp(a, b)
#define strncasecmp(a, b, n)  doom_strncasecmp(a, b, n)
#define strcmpi(a, b)         doom_strcasecmp(a, b)
#define stricmp(a, b)         doom_strcasecmp(a, b)

// alloca - use heap allocation (caller must free)
#define alloca(size)    kmalloc(size)

// ============================================================================
// Standard Library Stubs
// ============================================================================

// abs function
static inline int doom_abs(int x) { return x < 0 ? -x : x; }
#define abs(x)  doom_abs(x)

// exit - map to stopping doom
#define exit(code)  do { extern int doom_running; doom_running = 0; } while(0)

// mkdir - stub (read-only filesystem)
#define mkdir(path, mode)  (-1)

// setbuf - stub
#define setbuf(f, buf)  ((void)0)

// getchar - stub (no console input during game)
static inline int doom_getchar(void) { return -1; }
#define getchar()  doom_getchar()

// getenv - return appropriate paths for DOOM
static inline char* doom_getenv(const char *name) {
    if (strcmp(name, "DOOMWADDIR") == 0) {
        return "/GAMES/DOOM";  // WAD files are in /GAMES/DOOM directory
    }
    if (strcmp(name, "HOME") == 0) {
        return "/";
    }
    return (char*)0;  // NULL for unknown
}
#define getenv(name)  doom_getenv(name)

// ============================================================================
// FILE* I/O Stubs (config/save files not supported yet)
// ============================================================================

// fopen - always fail (no stdio FILE* support)
static inline FILE* doom_fopen(const char *path, const char *mode) {
    (void)path; (void)mode;
    return (FILE*)0;  // NULL = failure
}
#define fopen(path, mode)  doom_fopen(path, mode)

// fclose
static inline int doom_fclose(FILE *f) { (void)f; return 0; }
#define fclose(f)  doom_fclose(f)

// fseek, ftell, fread
static inline int doom_fseek(FILE *f, long offset, int whence) {
    (void)f; (void)offset; (void)whence; return -1;
}
static inline long doom_ftell(FILE *f) { (void)f; return -1; }
static inline size_t doom_fread(void *buf, size_t size, size_t count, FILE *f) {
    (void)buf; (void)size; (void)count; (void)f; return 0;
}
#define fseek(f, off, whence)  doom_fseek(f, off, whence)
#define ftell(f)               doom_ftell(f)
#define fread(buf, sz, cnt, f) doom_fread(buf, sz, cnt, f)

// feof, fscanf
static inline int doom_feof(FILE *f) { (void)f; return 1; }
#define feof(f)  doom_feof(f)

// fscanf/sscanf - always fail
#define fscanf(...)  (-1)
#define sscanf(...)  (-1)

// Disable features not available in kernel
#undef SNDSERV
#define SNDSERV 0
#undef SNDINTR
#undef NORMALUNIX
#undef LINUX

// values.h replacements
#ifndef MAXINT
#define MAXINT  0x7FFFFFFF
#endif
#ifndef MININT
#define MININT  (-MAXINT - 1)
#endif

// DOOM screen dimensions
#define DOOM_SCREENWIDTH    320
#define DOOM_SCREENHEIGHT   200

// Window dimensions (scaled 2x)
#define DOOM_WINDOW_WIDTH   640
#define DOOM_WINDOW_HEIGHT  400

// Palette
extern uint32_t doom_palette[256];

// Global DOOM window
extern window_t *doom_window;
extern int doom_running;
extern int doom_app_id;

// Platform functions
void I_MayteraInit(void);
void I_MayteraShutdown(void);
void I_MayteraPumpEvents(void);

// Key translation
int I_MayteraTranslateKey(int keycode);

// ============================================================================
// MayteraOS Key Codes (as returned by keyboard_get_char() in isr.c)
// Using MKEY_ prefix to avoid conflict with DOOM's KEY_ definitions
// NOTE: These are NOT raw PS/2 scancodes. The ISR converts scancodes to
// MayteraOS key codes before putting them in the keyboard buffer.
// ============================================================================

// Arrow keys (MayteraOS codes from isr.h)
#define MKEY_UP          0x80    // KEY_UP
#define MKEY_DOWN        0x81    // KEY_DOWN
#define MKEY_LEFT        0x82    // KEY_LEFT
#define MKEY_RIGHT       0x83    // KEY_RIGHT

// Modifier keys (MayteraOS codes from isr.h)
#define MKEY_LSHIFT      0x87    // KEY_LSHIFT
#define MKEY_RSHIFT      0x88    // KEY_RSHIFT
#define MKEY_LCTRL       0x84    // KEY_LCTRL
#define MKEY_LALT        0x89    // KEY_ALT

// Key release codes (MayteraOS codes from isr.h)
#define MKEY_UP_REL      0x90    // KEY_UP_REL
#define MKEY_DOWN_REL    0x91    // KEY_DOWN_REL
#define MKEY_LEFT_REL    0x92    // KEY_LEFT_REL
#define MKEY_RIGHT_REL   0x93    // KEY_RIGHT_REL
#define MKEY_LCTRL_UP    0x94    // KEY_LCTRL_UP
#define MKEY_LSHIFT_UP   0x97    // KEY_LSHIFT_UP
#define MKEY_RSHIFT_UP   0x98    // KEY_RSHIFT_UP

// Function keys: only F11 (0x85) and F12 (0x86) produce key codes from ISR.
// F1-F10 do not produce codes (their PS/2 scancodes map to 0 in scancode_to_ascii).
#define MKEY_F1          0x00    // Not available
#define MKEY_F2          0x00    // Not available
#define MKEY_F3          0x00    // Not available
#define MKEY_F4          0x00    // Not available
#define MKEY_F5          0x00    // Not available
#define MKEY_F6          0x00    // Not available
#define MKEY_F7          0x00    // Not available
#define MKEY_F8          0x00    // Not available
#define MKEY_F9          0x00    // Not available
#define MKEY_F10         0x00    // Not available
#define MKEY_F11         0x85    // KEY_F11 from isr.h
#define MKEY_F12         0x86    // KEY_F12 from isr.h

#endif // I_MAYTERA_H
