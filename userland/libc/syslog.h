// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// syslog.h - minimal syslog() surface (#745 darkhttpd port).
//
// MayteraOS has no syslog daemon. openlog()/closelog() are no-ops and syslog()
// routes the message to stderr (posixextra.c), which is honest: the text is not
// silently dropped, it goes where a foreground service on this OS is read. The
// LOG_* constants are accepted and ignored. Priorities/facilities match the
// conventional values so a program that ORs them together still compiles.
#ifndef _SYSLOG_H
#define _SYSLOG_H

#include <stdarg.h>

// Options for openlog()
#define LOG_PID     0x01
#define LOG_CONS    0x02
#define LOG_NDELAY  0x08
#define LOG_NOWAIT  0x10

// Facilities
#define LOG_KERN    (0<<3)
#define LOG_USER    (1<<3)
#define LOG_DAEMON  (3<<3)
#define LOG_LOCAL0  (16<<3)

// Priorities
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

void openlog(const char *ident, int option, int facility);
void syslog(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
void vsyslog(int priority, const char *format, va_list ap);
void closelog(void);

#endif // _SYSLOG_H
