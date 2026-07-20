/* ctype.h - C/POSIX-locale (ASCII) character classification for MayteraOS.
 * GNU grep uses these heavily; the MayteraOS libc does not ship <ctype.h>. */
#ifndef COMPAT_CTYPE_H
#define COMPAT_CTYPE_H

int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int iscntrl(int c);
int isprint(int c);
int isgraph(int c);
int ispunct(int c);
int isblank(int c);
int isascii(int c);
int toupper(int c);
int tolower(int c);
int toascii(int c);

#endif /* COMPAT_CTYPE_H */
