// SPDX-License-Identifier: BSD-3-Clause
// sys/ttydefaults.h - system default terminal control characters, added for
// the libedit port (#745). libedit's tty.c uses the C* default control-char
// constants and CTRL(); editline/readline.h includes this for CTRL(). Values
// are the standard 4.4BSD defaults (the same file every BSD/Linux ships).
#ifndef _SYS_TTYDEFAULTS_H_
#define _SYS_TTYDEFAULTS_H_

#ifndef CTRL
#define CTRL(x) ((x) & 037)
#endif

#ifndef _POSIX_VDISABLE
#define _POSIX_VDISABLE 0xff
#endif

#define CEOF     CTRL('d')
#define CEOL     0xff            /* was _POSIX_VDISABLE */
#define CERASE   0177
#define CINTR    CTRL('c')
#define CSTATUS  CTRL('t')
#define CKILL    CTRL('u')
#define CMIN     1
#define CQUIT    034             /* FS, ^\ */
#define CSUSP    CTRL('z')
#define CTIME    0
#define CDSUSP   CTRL('y')
#define CSTART   CTRL('q')
#define CSTOP    CTRL('s')
#define CLNEXT   CTRL('v')
#define CDISCARD CTRL('o')
#define CWERASE  CTRL('w')
#define CREPRINT CTRL('r')
#define CEOT     CEOF
#define CBRK     CEOL
#define CRPRNT   CREPRINT
#define CFLUSH   CDISCARD

#endif /* _SYS_TTYDEFAULTS_H_ */
