
/*
 * rogue_stdio.c - RETIRED (MayteraOS port layer, not upstream Rogue).
 *
 * This used to be a minimal, hand-rolled FILE implementation (a fixed
 * pool of 8 FILE slots, fopen/fclose/fread/fwrite/printf/etc, and a
 * stdout that wrote through the raw SYS_PUTCHAR syscall while stdin
 * read through curses getch()).
 *
 * It duplicated functionality the shared libc now provides for real
 * (libc/stdio.h + libc/stdio_file.c, task #422): every MayteraOS app
 * already links a full FILE-based stdio unconditionally, because crt0
 * calls __libc_init() -> __stdio_init() (libc/libc_init.c), which lives
 * in the same object as fopen/fclose/fread/fwrite/stdin/stdout/stderr.
 * Once that libc stdio grew those symbols, this file's copies collided
 * with them at link time ("multiple definition of `stdout'", etc).
 *
 * Rogue's actual game screen I/O never went through stdio to begin
 * with: curses.c (also our port layer) talks to the compositor
 * directly via SYS_WIN_* syscalls, and maytera_mdport.c's
 * md_readchar()/md_putchar() route through curses getch()/addch(), not
 * through stdin/stdout. The few real stdio uses in upstream Rogue
 * (save.c/rip.c file I/O, an fgets(stdin) prompt on the death screen)
 * work fine against the libc's real, fd-backed implementation, so this
 * file now contributes nothing and is no longer compiled (see the
 * Makefile). Left in the tree, empty, so its history and this note are
 * not lost.
 */
