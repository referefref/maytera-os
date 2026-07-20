/* Freestanding <stdio.h> shim for libmad. Only timer.c uses sprintf, inside the
 * unused mad_timer_string(); stub it so we need no libc and emit no symbol. */
#ifndef MAYTERA_MAD_COMPAT_STDIO_H
#define MAYTERA_MAD_COMPAT_STDIO_H
#define sprintf(...) (0)
#endif
