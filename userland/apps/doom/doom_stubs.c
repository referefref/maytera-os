// doom_stubs.c - Missing function implementations for userland DOOM

// Include syscall.h BEFORE i_maytera.h to avoid macro conflicts
#include "../../libc/syscall.h"

// Debug output (0=disabled for performance, 1=enabled)
#define DOOM_DEBUG 0
#include <stdarg.h>

#include <stddef.h>
// Simple debug helper that works before i_maytera.h
static void debug_char(char c) {
#if DOOM_DEBUG
    syscall1(40, c);  // SYS_PUTCHAR = 40
#else
    (void)c;
#endif
}

static void debug_str(const char *s) {
    while (*s) debug_char(*s++);
}

static void debug_hex(unsigned long val) {
    char buf[17];
    buf[16] = 0;
    for (int i = 15; i >= 0; i--) {
        int nibble = val & 0xF;
        buf[i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        val >>= 4;
    }
    debug_str(buf);
}

// Now we can define the POSIX wrapper functions BEFORE the macros are defined
long sys_file_read(int fd, void *buf, size_t count) {
    // Debug: show buffer address before syscall
    debug_str("[READ] fd=");
    debug_char('0' + fd);
    debug_str(" buf=0x");
    debug_hex((unsigned long)buf);
    debug_str(" cnt=");
    debug_hex(count);
    debug_str("\n");
    
    return sys_read(fd, buf, count);
}

long sys_file_lseek(int fd, long offset, int whence) {
    // Use seek syscall - SYS_SEEK = 14
    extern long syscall3(long num, long arg1, long arg2, long arg3);
    return syscall3(14, fd, offset, whence);
}

// Now include i_maytera.h which defines the macros
#include "i_maytera.h"

// FILE pointers
__attribute__((weak)) FILE *stdout = (FILE*)1;
__attribute__((weak)) FILE *stderr = (FILE*)2;

// toupper/tolower
int toupper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

int tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

// alloca - with debug
void *alloca(unsigned long size) {
    void *ptr = doom_malloc(size);
    debug_str("[ALLOCA] size=");
    debug_hex(size);
    debug_str(" ptr=0x");
    debug_hex((unsigned long)ptr);
    debug_str("\n");
    return ptr;
}

// File stubs
__attribute__((weak)) void *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
__attribute__((weak)) int fclose(void *f) { (void)f; return 0; }
__attribute__((weak)) int fprintf(void *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
__attribute__((weak)) int fseek(void *f, long offset, int whence) { (void)f; (void)offset; (void)whence; return 0; }
__attribute__((weak)) long ftell(void *f) { (void)f; return 0; }
__attribute__((weak)) size_t fread(void *ptr, size_t size, size_t nmemb, void *stream) { (void)ptr; (void)size; (void)nmemb; (void)stream; return 0; }
void setbuf(void *stream, char *buf) { (void)stream; (void)buf; }
static int doom_mkdir(const char *path, unsigned int mode) { (void)path; (void)mode; return 0; }
int fscanf(void *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
__attribute__((weak)) int feof(void *f) { (void)f; return 1; }
__attribute__((weak)) int sscanf(const char *str, const char *fmt, ...) { (void)str; (void)fmt; return 0; }

// doom_write
int doom_write(int fd, const void *buf, int count) { (void)fd; (void)buf; return count; }

// I_AllocLow
void *I_AllocLow(int size) { return doom_malloc(size); }

// kprintf
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }

// Variables
char *sndserver_filename = "";
int mb_used = 0;

// I_Tactile
void I_Tactile(int on, int off, int total) { (void)on; (void)off; (void)total; }

uint64_t timer_get_ticks(void) { static uint64_t t = 0; return ++t; }

// TLS stubs
void tls_free(void *ctx) { (void)ctx; }
void tls_close(void *ctx) { (void)ctx; }
int tls_send(void *ctx, const void *data, int len) { (void)ctx; (void)data; return len; }
int tls_recv(void *ctx, void *data, int len) { (void)ctx; (void)data; (void)len; return -1; }
void *tls_create(void) { return NULL; }
int tls_set_io(void *ctx, void *send, void *recv, void *arg) { (void)ctx; (void)send; (void)recv; (void)arg; return 0; }
int tls_set_hostname(void *ctx, const char *host) { (void)ctx; (void)host; return 0; }
int tls_set_verify(void *ctx, int verify) { (void)ctx; (void)verify; return 0; }
int tls_connect(void *ctx) { (void)ctx; return -1; }
const char *tls_strerror(int err) { (void)err; return "TLS stub"; }
int tls_is_connected(void *ctx) { (void)ctx; return 0; }
