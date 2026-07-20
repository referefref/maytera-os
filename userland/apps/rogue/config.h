
#ifndef CONFIG_H
#define CONFIG_H

/* Pull in FILE type before extern.h references it */
#include <stdio.h>

/* MayteraOS build - disable all POSIX feature detection */
#undef  HAVE_SYS_TYPES
#undef  HAVE_PWD_H
#undef  HAVE_SYS_UTSNAME
#undef  HAVE_ARPA_INET_H
#undef  HAVE_TERMIOS_H
#undef  HAVE_UNISTD_H
#undef  HAVE_TERM_H
#undef  HAVE_NCURSES_TERM_H
#undef  HAVE_WORKING_FORK
#undef  HAVE_ESCDELAY
#undef  HAVE_GETLOGIN
#undef  HAVE_PROCESS_H
#undef  NCURSES_VERSION

#define SAVEDIR     "/SAVES"
#define SCOREFILE   "/ROGUE.SCR"
#define SHELL       "/bin/sh"
#define WIZARD      0
#define NUMSCORES   10
#define NUMNAME     80
#define ALLOC(x)    malloc(x)
#define PACK_LIMIT  23
#define PATH_MAX    256

#ifndef NULL
#define NULL ((void*)0)
#endif

/* bool + TRUE/FALSE */
#include <stdbool.h>
#ifndef TRUE
#define TRUE  true
#define FALSE false
#endif

#endif /* CONFIG_H */
