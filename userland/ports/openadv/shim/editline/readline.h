/* editline/readline.h - MayteraOS shim, OUR FILE not upstream.
 *
 * open-adventure's cheat.c/main.c/misc.c include <editline/readline.h> (the
 * conventional path many distros install libedit's readline-compat header
 * at). This tree's libedit port (userland/ports/libedit) installs only
 * histedit.h (`headers=src/histedit.h`), not this compatibility path, and
 * open-adventure needs exactly two functions from it: readline() and
 * add_history(). Both signatures below are copied verbatim from libedit's
 * own src/editline/readline.h (built by this tree's libedit port) rather
 * than guessed, so there is no prototype/definition mismatch at link time.
 */
#ifndef MAYTERA_OPENADV_EDITLINE_READLINE_H
#define MAYTERA_OPENADV_EDITLINE_READLINE_H

char *readline(const char *);
int   add_history(const char *);

#endif
