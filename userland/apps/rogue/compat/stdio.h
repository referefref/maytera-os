
#ifndef COMPAT_STDIO_H
#define COMPAT_STDIO_H

/* MayteraOS port note: this used to define its own FILE struct and a
 * full set of fopen/fclose/fread/fwrite/printf/etc (see the retired
 * rogue_stdio.c). The shared libc (libc/stdio.h + libc/stdio_file.c,
 * #422) now provides a complete, real stdio implementation that every
 * app already links against unconditionally (crt0 -> __libc_init ->
 * __stdio_init lives in libc's stdio_file.o), so rogue's own copy of
 * fopen/fclose/stdin/stdout/stderr collided with the libc's at link
 * time ("multiple definition of `stdout'" etc). Rogue's actual game
 * screen I/O goes through curses.c's own window syscalls, not stdio
 * (see md_readchar/md_putchar in maytera_mdport.c), so nothing here
 * needs a rogue-specific FILE; forward to the real implementation
 * instead of duplicating it. */
#include "../../../libc/stdio.h"

/* rogue upstream (save.c) uses the traditional getc()/putc() macros,
 * which libc/stdio.h does not define (it only has the fgetc/fputc
 * function forms). */
#define getc(f)   fgetc(f)
#define putc(c,f) fputc(c,f)

#endif /* COMPAT_STDIO_H */
