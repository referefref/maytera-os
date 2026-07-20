// errno.c - per-process errno and strerror
#include "errno.h"
#include "types.h"

static int __errno_val = 0;

int *__errno_location(void) {
    return &__errno_val;
}

static const char *err_msgs[] = {
    [0]   = "Success",
    [1]   = "Operation not permitted",
    [2]   = "No such file or directory",
    [3]   = "No such process",
    [4]   = "Interrupted system call",
    [5]   = "I/O error",
    [6]   = "No such device or address",
    [7]   = "Argument list too long",
    [8]   = "Exec format error",
    [9]   = "Bad file descriptor",
    [10]  = "No child processes",
    [11]  = "Try again",
    [12]  = "Out of memory",
    [13]  = "Permission denied",
    [14]  = "Bad address",
    [16]  = "Device or resource busy",
    [17]  = "File exists",
    [18]  = "Cross-device link",
    [19]  = "No such device",
    [20]  = "Not a directory",
    [21]  = "Is a directory",
    [22]  = "Invalid argument",
    [23]  = "File table overflow",
    [24]  = "Too many open files",
    [25]  = "Not a typewriter",
    [27]  = "File too large",
    [28]  = "No space left on device",
    [29]  = "Illegal seek",
    [30]  = "Read-only file system",
    [31]  = "Too many links",
    [32]  = "Broken pipe",
    [34]  = "Range error",
    [36]  = "File name too long",
    [38]  = "Function not implemented",
    [39]  = "Directory not empty",
    [40]  = "Too many symbolic links",
    [33]  = "Numerical argument out of domain",
    [35]  = "Resource deadlock avoided",
    [37]  = "No locks available",
    [42]  = "No message of desired type",
    [43]  = "Identifier removed",
    [75]  = "Value too large for data type",
    [84]  = "Invalid or incomplete multibyte or wide character",
    [95]  = "Operation not supported",
    [98]  = "Address already in use",
    [103] = "Software caused connection abort",
    [110] = "Connection timed out",
    [114] = "Operation already in progress",
    [115] = "Operation now in progress",
};

char *strerror(int err) {
    if (err < 0) err = -err;
    if (err >= 0 && err < (int)(sizeof(err_msgs)/sizeof(err_msgs[0])) && err_msgs[err]) {
        return (char *)err_msgs[err];
    }
    return (char *)"Unknown error";
}

// perror requires write/fd 2 - forward decl of syscall
extern long syscall3(long, long, long, long);
#define SYS_WRITE 13

void perror(const char *msg) {
    const char *e = strerror(__errno_val);
    if (msg && *msg) {
        size_t n = 0;
        while (msg[n]) n++;
        syscall3(SYS_WRITE, 2, (long)msg, n);
        syscall3(SYS_WRITE, 2, (long)": ", 2);
    }
    size_t m = 0;
    while (e[m]) m++;
    syscall3(SYS_WRITE, 2, (long)e, m);
    syscall3(SYS_WRITE, 2, (long)"\n", 1);
}
