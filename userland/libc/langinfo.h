// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// langinfo.h - POSIX nl_langinfo(), for MayteraOS userland.
//
// Added for the Angband port (mports userland/ports/angband, #745 Tier 3 #15).
// main.c does `if (setlocale(LC_CTYPE, "")) { if (!streq(nl_langinfo(CODESET),
// "UTF-8")) quit(...); }` before starting any frontend, so a program that
// calls setlocale("") successfully (ours does, see locale.h) and then finds
// nl_langinfo() undeclared fails to compile, and one that finds it declared
// but wrong fails to start.
//
// Per docs/MPORTS.md's owner rule 1 ("extend the shared libc, do not reinvent
// a wheel inside our own project"), this is the SHARED answer, not a private
// shim in userland/apps/angband: any future ncurses/GCU-style port (NetHack
// included) hits the identical setlocale()+nl_langinfo() idiom.
//
// THE ANSWER IS "UTF-8", HONESTLY. This is not a locale claim (locale.h is
// explicit that MayteraOS has exactly one locale, the C locale); CODESET asks
// what BYTE ENCODING the terminal transport uses, which is a different
// question. userland/apps/terminal/term_parse.c is a real VT220/xterm-class
// ANSI parser that maps the DEC special-graphics set to Unicode codepoints and
// the compositor renders text through antialiased TrueType glyphs
// (SYS_WIN_DRAW_TTF), so the terminal's actual output encoding IS UTF-8; there
// is no separate legacy 8-bit codeset to fall back to. Returning anything else
// here would be the lie, not the other way around.
#ifndef LIBC_LANGINFO_H
#define LIBC_LANGINFO_H

typedef int nl_item;

// Only the item Angband (and POSIX in general) actually asks for.
#define CODESET 0

// Always returns a non-NULL, static "UTF-8" string. There is no other
// codeset on this OS to report.
char *nl_langinfo(nl_item item);

#endif // LIBC_LANGINFO_H
