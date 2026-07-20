
#ifndef COMPAT_UNISTD_H
#define COMPAT_UNISTD_H

#include "../../../libc/syscall.h"

#define R_OK 4
#define W_OK 2
#define F_OK 0

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static inline int getuid(void)  { return 1000; }
static inline int getgid(void)  { return 1000; }
static inline int getpid(void)  { return (int)syscall0(SYS_GETPID); }

static inline char *getenv(const char *name) { (void)name; return ((void*)0); }
static inline int   setenv(const char *n, const char *v, int ov) {
    (void)n;(void)v;(void)ov; return 0;
}
static inline int putenv(char *s) { (void)s; return 0; }

static inline int access(const char *p, int m) {
    (void)m;
    int fd = (int)syscall2(SYS_OPEN, (long)p, 0);
    if (fd >= 0) { syscall1(SYS_CLOSE, fd); return 0; }
    return -1;
}

static inline int unlink(const char *p) { (void)p; return 0; }
static inline int rename(const char *o, const char *n) { (void)o;(void)n; return -1; }

static inline unsigned int sleep(unsigned int s) {
    syscall1(SYS_SLEEP, (long)(s * 1000));
    return 0;
}
static inline void usleep(unsigned long us) {
    syscall1(SYS_SLEEP, (long)(us / 1000 + 1));
}

static inline long lseek(int fd, long off, int whence) {
    return syscall3(SYS_SEEK, fd, off, whence);
}

static inline long read(int fd, void *buf, unsigned long n) {
    return syscall3(SYS_READ, fd, (long)buf, (long)n);
}
static inline long write(int fd, const void *buf, unsigned long n) {
    return syscall3(SYS_WRITE, fd, (long)buf, (long)n);
}
static inline int close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

#endif
