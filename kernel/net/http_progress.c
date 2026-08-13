/* http_progress.c - #25: see http_progress.h for the design rationale. */
#include "http_progress.h"
#include "../proc/process.h"
#include "../serial.h"

http_progress_t *net_progress_current(void) {
    process_t *p = proc_current();
    if (!p) return (http_progress_t *) 0;
    return (http_progress_t *) p->net_progress;
}

static const char *progress_phase_name(int phase) {
    switch (phase) {
        case HTTP_PHASE_IDLE:       return "idle";
        case HTTP_PHASE_RESOLVING:  return "resolving";
        case HTTP_PHASE_CONNECTING: return "connecting";
        case HTTP_PHASE_TLS:        return "tls";
        case HTTP_PHASE_SENDING:    return "sending";
        case HTTP_PHASE_RECEIVING:  return "receiving";
        case HTTP_PHASE_DONE:       return "done";
        case HTTP_PHASE_ERROR:      return "error";
        default:                    return "?";
    }
}

// #25: one line per REAL phase change (at most ~6 per fetch, never per byte),
// matching the existing [HTTPS]/[HTTP] kprintf convention in net/https.c and
// net/wget.c. Proves the transitions this file publishes are event-driven,
// not a fake timer - see CHANGELOG #25 for a captured boot log.
void net_progress_phase(int phase) {
    http_progress_t *pr = net_progress_current();
    if (pr && pr->phase != phase) {
        kprintf("[PROGRESS] phase %s -> %s\n",
                progress_phase_name(pr->phase), progress_phase_name(phase));
        pr->phase = phase;
    }
}

void net_progress_bytes(uint32_t bytes_recv) {
    http_progress_t *pr = net_progress_current();
    if (pr) pr->bytes_recv = bytes_recv;
}

void net_progress_content_len(uint32_t content_len) {
    http_progress_t *pr = net_progress_current();
    if (pr) pr->content_len = content_len;
}
