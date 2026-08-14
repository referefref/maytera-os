// i_maytera.h - MayteraOS userland platform header for DOOM
#ifndef I_MAYTERA_H
#define I_MAYTERA_H

#include <stdint.h>
#include <stddef.h>

// FILE type for compatibility
#ifndef LIBC_STDIO_H
typedef void FILE;
extern FILE *stdout;
extern FILE *stderr;
#endif
// Basic types
typedef unsigned char byte;

// File handling
#define DOOM_MAX_FILES 32

typedef struct {
    uint8_t *buffer;
    uint32_t size;
    uint32_t pos;
    int valid;
} doom_file_t;

extern doom_file_t doom_file_table[DOOM_MAX_FILES];

// File operations
int doom_open(const char *path, int flags);
int doom_close(int fd);
int doom_read(int fd, void *buf, int count);
int doom_lseek(int fd, int offset, int whence);
int doom_filelength(int fd);
int doom_write(int fd, const void *buf, int count);

// POSIX-like file operations (map to syscalls)
// Note: 'open' and 'close' are enum values in p_spec.h, so we use sys_ prefix
long sys_file_read(int fd, void *buf, size_t count);
long sys_file_write(int fd, const void *buf, size_t count);
long sys_file_lseek(int fd, long offset, int whence);

// Map DOOM's read/lseek to our functions (open/close handled differently)
#define read doom_read
#define lseek doom_lseek

// Seek constants
#define O_RDONLY 0
#define O_BINARY 0
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// Memory
void *doom_malloc(size_t size);
void doom_free(void *ptr);
void *doom_realloc(void *ptr, size_t size);

#define malloc doom_malloc
#define free doom_free
#define realloc doom_realloc

// Alloca - stack allocation (actually uses heap)
void *alloca(unsigned long size);

// Time
uint32_t doom_gettime(void);

// Exit
void doom_exit(int code);
#define exit doom_exit

// String functions
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcat(char *dest, const char *src);
char *strrchr(const char *s, int c);
char *strchr(const char *s, int c);
int toupper(int c);
int tolower(int c);
int sprintf(char *str, const char *format, ...);
int sscanf(const char *str, const char *format, ...);

// Printf
int doom_printf(const char *fmt, ...);
#define printf doom_printf

// Launch
void doom_launch(void);


// Debug: write to kernel serial log (bypasses closed stdout)
static inline void klog(const char *msg) {
    __asm__ volatile("mov $186, %%rax\n\tsyscall" :: "D"(msg) : "rax","rcx","r11","memory");
}
static inline void klog_hex(const char *prefix, unsigned long long val) {
    char _kbuf[80];
    int i = 0;
    while (prefix[i] && i < 50) { _kbuf[i] = prefix[i]; i++; }
    _kbuf[i++] = '0'; _kbuf[i++] = 'x';
    int started = 0;
    for (int s = 60; s >= 0; s -= 4) {
        int d = (int)((val >> s) & 0xF);
        if (d || started || s == 0) { _kbuf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); started = 1; }
    }
    _kbuf[i++] = '\n'; _kbuf[i] = 0;
    klog(_kbuf);
}

// Log current RSP value
static inline void klog_rsp(const char *label) {
    unsigned long long rsp_val;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_val));
    klog_hex(label, rsp_val);
}

#endif // I_MAYTERA_H

