// unistd.h stub for DOOM
#ifndef DOOM_UNISTD_H
#define DOOM_UNISTD_H

#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000

// access() - check if file exists/readable
static inline int access(const char *path, int mode) {
    (void)mode;
    int fd = doom_open(path, 0);
    if (fd >= 0) {
        doom_close(fd);
        return 0;
    }
    return -1;
}

// Stat structure (minimal)
struct stat {
    unsigned long st_size;
};

static inline int fstat(int fd, struct stat *buf) {
    buf->st_size = doom_filelength(fd);
    return 0;
}

#endif // DOOM_UNISTD_H
