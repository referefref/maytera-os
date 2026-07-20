// unistd.h - POSIX unistd for MayteraOS userland
#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

#include "types.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// whence for lseek
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

// pid_t, ssize_t, off_t provided by types.h

pid_t getpid(void);
pid_t getppid(void);
pid_t fork(void);
int   execve(const char *path, char *const argv[], char *const envp[]);
int   execv(const char *path, char *const argv[]);
int   execvp(const char *file, char *const argv[]);
void  _exit(int status) __attribute__((noreturn));

// read/write/close come from stdlib.h (historical); lseek is new.
long    read(int fd, void *buf, size_t count);
long    write(int fd, const void *buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     dup(int fd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     isatty(int fd);
int     unlink(const char *path);

pid_t   setsid(void);
pid_t   getsid(pid_t pid);
int     setpgid(pid_t pid, pid_t pgid);
pid_t   getpgid(pid_t pid);
pid_t   getpgrp(void);

unsigned sleep(unsigned sec);
int      usleep(unsigned long us);

// User/group identity types
#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef unsigned int uid_t;
#endif
#ifndef _GID_T_DEFINED
#define _GID_T_DEFINED
typedef unsigned int gid_t;
#endif
#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned int mode_t;
#endif

// Access check flags
#define R_OK    4
#define W_OK    2
#define X_OK    1
#define F_OK    0

// User identity functions
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
int     setuid(uid_t uid);
int     seteuid(uid_t euid);
int     setgid(gid_t gid);
int     setegid(gid_t egid);

// #359 Phase 2: filesystem + sysconf helpers
int access(const char *path, int mode);
int ftruncate(int fd, long length);
int truncate(const char *path, long length);
long sysconf(int name);
int getpagesize(void);

#define _SC_ARG_MAX          0
#define _SC_CHILD_MAX        1
#define _SC_CLK_TCK          2
#define _SC_NGROUPS_MAX      3
#define _SC_OPEN_MAX         4
#define _SC_PAGESIZE         30
#define _SC_PAGE_SIZE        30
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES       85
#define _SC_AVPHYS_PAGES     86

// File permission functions
int     chmod(const char *path, mode_t mode);
int     chown(const char *path, uid_t uid, gid_t gid);

// #359 Phase 3a: prototypes for functions unistd.c already defines but
// never declared (implicit-declaration under -Werror broke os.mkdir/
// os.rename/os.rmdir once posixmodule.c got rebuilt). Additive only, no
// change to the existing implementations in unistd.c.
int mkdir(const char *path, int mode);
int rmdir(const char *path);
int rename(const char *oldpath, const char *newpath);

// #359 Phase 3a: legacy BSD utime(2) (time_t[2] form, NULL = "set to now").
// MayteraOS's filesystem layer has no mtime-set syscall yet; implemented as
// a no-op stub in the CPython port's compat supplement (compatsupp/compat.c),
// same pattern as the Phase 2 ftruncate/truncate no-op.
int utime(const char *path, const long *times);

#endif
