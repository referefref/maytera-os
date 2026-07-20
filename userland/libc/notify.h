#ifndef _MAYTERA_NOTIFY_H
#define _MAYTERA_NOTIFY_H
// MayteraOS userland notifications (#168).
// Any app or service posts a notification by appending one record to the spool
// file /CONFIG/NOTIFY.TXT; the compositor polls it and shows a toast + logs it
// in the notification center. Record format: "S|title|body\n" (S = severity).
#define NOTIFY_INFO     0
#define NOTIFY_SUCCESS  1
#define NOTIFY_WARNING  2
#define NOTIFY_ERROR    3
// Post a notification. title/body are sanitized (newlines/pipes -> spaces).
// Returns 0 on success, -1 on failure. Safe to call from any user process.
int notify_post(const char *title, const char *body, int severity);
#endif
