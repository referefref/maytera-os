// unistd.c - POSIX unistd wrappers
#include "unistd.h"
#include "syscall.h"
#include "errno.h"

pid_t getpid(void)  { return (pid_t)syscall0(SYS_GETPID); }
pid_t getppid(void) { return (pid_t)syscall0(SYS_GETPPID); }

pid_t fork(void) {
    long r = syscall0(SYS_FORK);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (pid_t)r;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    long r = syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, 0);
}

// Minimal PATH lookup: try file literally; if it has no '/', also try /APPS/<upper>.ELF
static int to_upper_app(const char *file, char *out, int outsz) {
    int i = 0;
    int n = 0;
    while (file[n]) n++;
    const char prefix[] = "/APPS/";
    const char suffix[] = ".ELF";
    int plen = sizeof(prefix) - 1;
    int slen = sizeof(suffix) - 1;
    if (plen + n + slen + 1 > outsz) return -1;
    for (i = 0; i < plen; i++) out[i] = prefix[i];
    int j;
    for (j = 0; j < n; j++) {
        char c = file[j];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i++] = c;
    }
    for (j = 0; j < slen; j++) out[i++] = suffix[j];
    out[i] = 0;
    return 0;
}

int execvp(const char *file, char *const argv[]) {
    // Literal path first
    execv(file, argv);
    // Try /APPS/<upper> and /APPS/<upper>.ELF if no slash
    int has_slash = 0;
    for (const char *p = file; *p; p++) if (*p == '/') { has_slash = 1; break; }
    if (!has_slash) {
        char buf[128];
        // /APPS/<UPPER>
        int i = 0;
        const char pref[] = "/APPS/";
        while (pref[i]) { buf[i] = pref[i]; i++; }
        int j = 0;
        while (file[j] && i < 120) {
            char c = file[j++];
            if (c >= 'a' && c <= 'z') c -= 32;
            buf[i++] = c;
        }
        buf[i] = 0;
        execv(buf, argv);
        // /APPS/<UPPER>.ELF
        if (to_upper_app(file, buf, sizeof(buf)) == 0) {
            execv(buf, argv);
        }
    }
    errno = ENOENT;
    return -1;
}

// _exit() provided by crt0.S

// read/write already defined in stdlib.c; we do not redefine them here.

// close() defined in stdlib.c (pre-existing).

off_t lseek(int fd, off_t off, int whence) {
    long r = syscall3(SYS_SEEK, fd, off, whence);
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

int dup(int fd) {
    long r = syscall1(SYS_DUP, fd);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

int dup2(int oldfd, int newfd) {
    long r = syscall2(SYS_DUP2, oldfd, newfd);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

int pipe(int fds[2]) {
    long r = syscall1(SYS_PIPE, (long)fds);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int chdir(const char *path) {
    long r = syscall1(SYS_CHDIR, (long)path);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

char *getcwd(char *buf, size_t size) {
    long r = syscall2(SYS_GETCWD, (long)buf, size);
    if (r < 0) { errno = (int)(-r); return 0; }
    return buf;
}

int isatty(int fd) {
    // Query via TIOCGPGRP; if it succeeds, fd is a tty. Use 0x540F.
    int pgrp = 0;
    long r = syscall3(SYS_IOCTL, fd, 0x540F, (long)&pgrp);
    if (r < 0) { errno = (int)(-r); return 0; }
    return 1;
}

int unlink(const char *path) {
    return (int)syscall1(SYS_UNLINK, (long)path);
}

pid_t setsid(void) {
    long r = syscall0(SYS_SETSID);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (pid_t)r;
}

pid_t getsid(pid_t pid) {
    long r = syscall1(SYS_GETSID, pid);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (pid_t)r;
}

int setpgid(pid_t pid, pid_t pgid) {
    long r = syscall2(SYS_SETPGID, pid, pgid);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

pid_t getpgid(pid_t pid) {
    long r = syscall1(SYS_GETPGID, pid);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (pid_t)r;
}

pid_t getpgrp(void) {
    return getpgid(0);
}

unsigned sleep(unsigned sec) {
    syscall1(SYS_SLEEP, (long)sec * 1000);
    return 0;
}

int usleep(unsigned long us) {
    unsigned long ms = (us + 999) / 1000;
    if (ms == 0) ms = 1;
    syscall1(SYS_SLEEP, (long)ms);
    return 0;
}

int mkdir(const char *path, int mode) {
    return (int)syscall2(SYS_MKDIR, (long)path, mode);
}

int rmdir(const char *path) {
    return (int)syscall1(SYS_RMDIR, (long)path);
}

int rename(const char *oldpath, const char *newpath) {
    return (int)syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
}

long time(long *t) {
    long val = syscall0(SYS_TIME);
    if (t) *t = val;
    return val;
}

// ============================================================================
// User identity functions
// ============================================================================

uid_t getuid(void)  { return (uid_t)syscall0(SYS_GETUID); }
uid_t geteuid(void) { return (uid_t)syscall0(SYS_GETEUID); }
gid_t getgid(void)  { return (gid_t)syscall0(SYS_GETGID); }
gid_t getegid(void) { return (gid_t)syscall0(SYS_GETEGID); }
int setuid(uid_t uid)   { return (int)syscall1(SYS_SETUID, uid); }
int seteuid(uid_t euid) { return (int)syscall1(SYS_SETEUID, euid); }
int setgid(gid_t gid)   { return (int)syscall1(SYS_SETGID, gid); }
int setegid(gid_t egid) { return (int)syscall1(SYS_SETEGID, egid); }
int chmod(const char *path, mode_t mode) {
    return (int)syscall2(SYS_CHMOD, (long)path, mode);
}
int chown(const char *path, uid_t uid, gid_t gid) {
    return (int)syscall3(SYS_CHOWN, (long)path, uid, gid);
}
