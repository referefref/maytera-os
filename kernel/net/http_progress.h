/* http_progress.h - #25: real per-fetch progress for the browser chrome.
 *
 * WHY. The browser's "Loading..." status used to be the whole story: no
 * phase, no byte count, no content-length. This gives the HTTPS/HTTP clients
 * (net/https.c, net/wget.c) a cheap way to publish REAL state as they work
 * through DNS/TCP/TLS/send/receive, without threading a new parameter
 * through the connect/acquire/receive call chains those files already have
 * (https_connect -> https_conn_acquire -> https_get_ex, and the equivalent
 * wget.c chain), and without adding a global that would race between the
 * several async fetch worker threads that can run concurrently (proc/
 * syscall.c allows up to ASYNC_FETCH_MAX in flight at once).
 *
 * MECHANISM. Each in-flight async fetch job (proc/syscall.c: async_fetch_t)
 * owns one http_progress_t, embedded directly in the job slot (not a stack
 * pointer, so its lifetime is never in question). The job's dedicated worker
 * thread stashes a pointer to it on ITS OWN process_t (process_t::
 * net_progress, see proc/process.h) for the duration of the blocking https_
 * get()/wget_fetch() call. https.c/wget.c call net_progress_current(), which
 * just reads that field off proc_current(), and update it in place. Every
 * other caller of https_get()/wget_fetch() (the HA widget, App Store, pip,
 * the synchronous SYS_HTTP_FETCH path, ...) never sets net_progress, so
 * net_progress_current() returns NULL for them and the three update helpers
 * below are one branch and nothing else - zero behavior change for callers
 * that never opted in.
 *
 * The terminal DONE/ERROR phase is set by the CALLER (async_fetch_worker)
 * once https_get()/wget_fetch() returns, from the real return code - not by
 * https.c/wget.c, which have too many internal early-return error paths to
 * instrument individually without materially increasing the risk of this
 * change to code that many other subsystems depend on.
 */
#ifndef HTTP_PROGRESS_H
#define HTTP_PROGRESS_H

#include "../types.h"

typedef enum {
    HTTP_PHASE_IDLE = 0,
    HTTP_PHASE_RESOLVING,     /* DNS lookup in flight */
    HTTP_PHASE_CONNECTING,    /* TCP handshake in flight */
    HTTP_PHASE_TLS,           /* TLS handshake in flight (HTTPS only) */
    HTTP_PHASE_SENDING,       /* request written, or connection reused */
    HTTP_PHASE_RECEIVING,     /* response bytes arriving */
    HTTP_PHASE_DONE,          /* fetch finished successfully */
    HTTP_PHASE_ERROR,         /* fetch finished with an error */
} http_phase_t;

typedef struct {
    volatile int      phase;        /* an http_phase_t value */
    volatile uint32_t bytes_recv;   /* body bytes received so far (0 until body starts) */
    volatile uint32_t content_len;  /* 0 = unknown until Content-Length is parsed */
} http_progress_t;

/* Returns the http_progress_t* the CURRENT process opted into (via its
 * net_progress field), or NULL. Never blocks, never allocates, safe to call
 * from any kernel context that has a current process. */
http_progress_t *net_progress_current(void);

/* No-ops when net_progress_current() is NULL. */
void net_progress_phase(int phase);
void net_progress_bytes(uint32_t bytes_recv);
void net_progress_content_len(uint32_t content_len);

#endif /* HTTP_PROGRESS_H */
