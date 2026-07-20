// ctype.h - character classification for MayteraOS userland (#422).
// Previously missing entirely, which forced vendored ports (NetSurf, etc.) to
// ship private ctype shims. C locale ("C"/POSIX) semantics only.
#ifndef LIBC_CTYPE_H
#define LIBC_CTYPE_H

int isalnum(int c);
int isalpha(int c);
int isascii(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);
int toascii(int c);

#endif // LIBC_CTYPE_H
