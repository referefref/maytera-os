// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// libgen.h - POSIX basename()/dirname() for MayteraOS userland.
//
// THESE ARE THE POSIX FORMS AND THEY MAY MODIFY THE STRING YOU PASS. That is
// not an implementation shortcut, it is the specified behaviour: dirname()
// truncates in place and basename() strips trailing slashes in place. Pass a
// copy if you still need the original. The GNU basename() from <string.h>,
// which takes a const char * and never writes, is deliberately NOT provided:
// having both under one name is how a port ends up calling the one it did not
// mean to.
//
// Both may also return a pointer to static storage ("." or "/") for the
// degenerate inputs, so do not free the result and do not assume it points
// into your buffer.
#ifndef LIBC_LIBGEN_H
#define LIBC_LIBGEN_H

// basename("/usr/lib") == "lib"      basename("/usr/") == "usr"
// basename("/")        == "/"        basename("")      == "."
// basename(NULL)       == "."
char *basename(char *path);

// dirname("/usr/lib") == "/usr"      dirname("/usr/")  == "/"
// dirname("usr")      == "."         dirname("/")      == "/"
// dirname("")         == "."         dirname(NULL)     == "."
char *dirname(char *path);

#endif // LIBC_LIBGEN_H
