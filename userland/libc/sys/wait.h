// sys/wait.h
#ifndef LIBC_SYS_WAIT_H
#define LIBC_SYS_WAIT_H

#include "../unistd.h"

#define WNOHANG    1
#define WUNTRACED  2

#define WEXITSTATUS(status)   (((status) >> 8) & 0xFF)
#define WTERMSIG(status)      ((status) & 0x7F)
#define WIFEXITED(status)     (WTERMSIG(status) == 0)
#define WIFSIGNALED(status)   (((status) & 0x7F) > 0 && (((status) & 0x7F) < 0x7F))
#define WIFSTOPPED(status)    (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status)      WEXITSTATUS(status)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif
