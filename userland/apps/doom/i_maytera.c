// i_maytera.c - MayteraOS USERLAND platform implementation for DOOM
#include <stdarg.h>
// Uses syscalls instead of kernel functions

#include "../../libc/stdio.h"
#include "i_maytera.h"
#include "d_main.h"
#include "m_argv.h"

// Syscall wrappers
#include "../../libc/syscall.h"
#include "../../libc/string.h"

// MAP_FAILED for mmap error checking
// Debug output (0=disabled for performance, 1=enabled)
#define DOOM_DEBUG 1

#define MAP_FAILED ((void *)-1)

// File handle table
doom_file_t doom_file_table[DOOM_MAX_FILES];
static int doom_file_init_done = 0;

// Debug printf using putchar syscall
static void doom_debug(const char *msg) {
#if DOOM_DEBUG
    while (*msg) {
        syscall1(SYS_PUTCHAR, *msg++);
    }
#else
    (void)msg;
#endif
}

// Simple number to string for debug
static void doom_debug_num(const char *prefix, int64_t val) {
    doom_debug(prefix);
    if (val < 0) {
        syscall1(SYS_PUTCHAR, '-');
        val = -val;
    }
    char buf[32];
    int i = 30;
    buf[31] = 0;
    if (val == 0) {
        buf[i--] = '0';
    } else {
        while (val > 0 && i >= 0) {
            buf[i--] = '0' + (val % 10);
            val /= 10;
        }
    }
    doom_debug(&buf[i+1]);
    doom_debug("\n");
}

// Debug hex pointer
static void doom_debug_hex(const char *prefix, uint64_t val) {
    doom_debug(prefix);
    doom_debug("0x");
    char hex[17];
    for (int i = 15; i >= 0; i--) {
        int digit = (val >> (i * 4)) & 0xF;
        hex[15-i] = digit < 10 ? 0x30 + digit : 0x61 + digit - 10;
    }
    hex[16] = 0;
    doom_debug(hex);
    doom_debug("\n");
}

// printf replacement using putchar syscall
int doom_printf(const char *fmt, ...) {
#if DOOM_DEBUG
    va_list ap;
    va_start(ap, fmt);
    int ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
#else
    (void)fmt;
    return 0;
#endif
}

// Initialize file table
static void doom_file_init(void) {
    if (doom_file_init_done) return;
    memset(doom_file_table, 0, sizeof(doom_file_table));
    doom_file_init_done = 1;
}

// Open a file
int doom_open(const char *path, int flags) {
    (void)flags;
    doom_file_init();

    doom_debug("[DOOM] doom_open: ");
    doom_debug(path);
    doom_debug("\n");

    // Find free slot
    int slot = -1;
    for (int i = 0; i < DOOM_MAX_FILES; i++) {
        if (!doom_file_table[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        doom_debug("[DOOM] No free file slots!\n");
        return -1;
    }

    // Convert path to uppercase FAT format
    char fat_path[256];
    int j = 0;
    for (int i = 0; path[i] && j < 255; i++) {
        char c = path[i];
        if (c == '\\') c = '/';
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        fat_path[j++] = c;
    }
    fat_path[j] = '\0';

    // Prepend /GAMES/DOOM/ if not absolute
    char final_path[300];
    if (fat_path[0] != '/') {
        strcpy(final_path, "/GAMES/DOOM/");
        strcat(final_path, fat_path);
    } else {
        strcpy(final_path, fat_path);
    }

    doom_debug("[DOOM] Opening: ");
    doom_debug(final_path);
    doom_debug("\n");

    // Open via syscall
    int fd = sys_open(final_path, 0);
    doom_debug_num("[DOOM] sys_open returned fd=", fd);
    
    if (fd < 0) {
        doom_debug("[DOOM] sys_open failed!\n");
        return -1;
    }

    doom_file_t *df = &doom_file_table[slot];
    
    // Allocate buffer (16MB max for WAD)
    void *buf = sys_mmap(NULL, 16 * 1024 * 1024, 3, 0);
    doom_debug_num("[DOOM] sys_mmap returned ", (int64_t)buf);
    
    if (buf == MAP_FAILED || buf == NULL) {
        doom_debug("[DOOM] sys_mmap failed!\n");
        sys_close(fd);
        return -1;
    }
    df->buffer = (uint8_t *)buf;

    // Read entire file
    uint32_t total = 0;
    int64_t n;
    while ((n = sys_read(fd, df->buffer + total, 65536)) > 0) {
        total += n;
        if (total >= 16 * 1024 * 1024) break;
    }
    sys_close(fd);

    doom_debug_num("[DOOM] Read total bytes: ", total);

    df->size = total;
    df->pos = 0;
    df->valid = 1;

    return slot;
}

// Close a file
int doom_close(int fd) {
    if (fd < 0 || fd >= DOOM_MAX_FILES) return -1;
    doom_file_t *df = &doom_file_table[fd];
    if (!df->valid) return -1;

    if (df->buffer) {
        sys_munmap(df->buffer, 16 * 1024 * 1024);
        df->buffer = NULL;
    }
    df->valid = 0;
    return 0;
}

// Read from file
int doom_read(int fd, void *buf, int count) {
    if (fd < 0 || fd >= DOOM_MAX_FILES) return -1;
    doom_file_t *df = &doom_file_table[fd];
    if (!df->valid || !df->buffer) return -1;

    if (df->pos >= df->size) return 0;
    
    int avail = df->size - df->pos;
    if (count > avail) count = avail;
    
    memcpy(buf, df->buffer + df->pos, count);
    df->pos += count;
    return count;
}

// Seek in file
int doom_lseek(int fd, int offset, int whence) {
    if (fd < 0 || fd >= DOOM_MAX_FILES) return -1;
    doom_file_t *df = &doom_file_table[fd];
    if (!df->valid) return -1;

    int newpos = df->pos;
    if (whence == 0) newpos = offset;           // SEEK_SET
    else if (whence == 1) newpos = df->pos + offset;  // SEEK_CUR
    else if (whence == 2) newpos = df->size + offset; // SEEK_END

    if (newpos < 0) newpos = 0;
    if (newpos > (int)df->size) newpos = df->size;
    
    df->pos = newpos;
    return newpos;
}

// File size
int doom_filelength(int fd) {
    if (fd < 0 || fd >= DOOM_MAX_FILES) return -1;
    doom_file_t *df = &doom_file_table[fd];
    if (!df->valid) return -1;
    return df->size;
}

// Memory allocation - use mmap
static uint8_t *heap_base = NULL;
static uint32_t heap_used = 0;
#define HEAP_SIZE (32 * 1024 * 1024)

void *doom_malloc(size_t size) {
    if (!heap_base) {
        void *p = sys_mmap(NULL, HEAP_SIZE, 3, 0);
        if (p == MAP_FAILED || p == NULL) {
            doom_debug("[DOOM] heap mmap failed!\n");
            return NULL;
        }
        heap_base = (uint8_t *)p;
        doom_debug_num("[DOOM] Heap allocated at ", (int64_t)heap_base);
    }
    
    // Align to 16 bytes
    size = (size + 15) & ~15;
    
    if (heap_used + size > HEAP_SIZE) {
        doom_debug("[DOOM] Heap exhausted!\n");
        return NULL;
    }
    
    void *ptr = heap_base + heap_used;
    doom_debug("[MALLOC] allocating\n");
    heap_used += size;
    return ptr;
}

void doom_free(void *ptr) {
    // Simple bump allocator - no freeing
    (void)ptr;
}

void *doom_realloc(void *ptr, size_t size) {
    if (!ptr) return doom_malloc(size);
    // Just allocate new and copy
    void *new_ptr = doom_malloc(size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, size); // May copy too much but safe
    }
    return new_ptr;
}

// Time - use syscall
uint32_t doom_gettime(void) {
    return (uint32_t)sys_clock();
}

// Exit
void doom_exit(int code) {
    sys_exit(code);
}

// Launch DOOM
void doom_launch(void) {
    doom_debug("[DOOM] Userland DOOM starting...\n");
    
    // Set up DOOM arguments
    static char *doom_argv[] = {
        "doom",
        "-iwad",
        "/GAMES/DOOM/DOOM1.WAD",
        NULL
    };
    
    myargc = 3;
    myargv = doom_argv;
    
    doom_debug("[DOOM] Calling D_DoomMain...\n");
    
    // Call DOOM main
    D_DoomMain();
}

// Entry point for userland
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    doom_launch();
    return 0;
}
