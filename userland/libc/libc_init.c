// libc_init.c - libc startup / shutdown hooks
#include "stdio.h"

void __libc_init(void) {
    __stdio_init();
}

void __libc_fini(void) {
    fflush(0);
}
