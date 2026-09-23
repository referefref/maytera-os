// sys/stat.h stub
#ifndef SYS_STAT_H
#define SYS_STAT_H

struct stat {
    unsigned long st_size;
};

int fstat(int fd, struct stat *buf);

#endif
