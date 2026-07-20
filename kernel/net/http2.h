// http2.h - Minimal HTTP/2 (h2 over TLS) client for MayteraOS
// Single GET over a single stream with HPACK decode. Used as a fallback path
// from https_get when the server negotiates ALPN "h2".
#ifndef HTTP2_H
#define HTTP2_H

#include "../types.h"
#include "tls/tls.h"

// Perform a single HTTP/2 GET over an already-established TLS connection.
// host:  authority (e.g. "www.google.com")
// path:  request target (e.g. "/")
// On success returns 0; *body_out is a kmalloc'd buffer (caller kfree()s),
// *body_len_out the length, *status_out the HTTP status code.
// Returns <0 on error.
int http2_get(tls_context_t *tls, const char *host, const char *path,
              uint8_t **body_out, uint32_t *body_len_out, int *status_out,
              char *loc_out, int loc_cap);

// #404 HTTP/2 frame framing seam (flag -DRUST_HTTP2_FRAME).
// #[repr(C)] mirror of the Rust H2Frame. sizeof-locked to 28 bytes / align 4.
typedef struct {
    uint32_t length;      // 24-bit frame payload length
    uint32_t stream_id;   // 31-bit stream id (reserved bit masked off)
    uint32_t hdr_off;     // offset of the 9-byte header within buf
    uint32_t payload_off; // hdr_off + 9
    uint32_t data_off;    // meaningful payload start (after pad/priority)
    uint32_t data_len;    // meaningful payload length
    uint8_t  type_;
    uint8_t  flags;
    uint8_t  pad_len;
    uint8_t  _reserved;
} H2Frame;
_Static_assert(sizeof(H2Frame) == 28, "H2Frame sizeof locked at 28");

// Bounded HTTP/2 frame iterator: 1 = one frame decoded (*pos advanced),
// 0 = clean end, -1 = malformed / cannot decode safely. Verbatim C reference
// (net/http2_frame_c.c) and Rust port (rustkern.rs) share this ABI.
int http2_frame_next_c (const uint8_t *buf, uint32_t len, uint32_t *pos_io, H2Frame *out);
int http2_frame_next_rs(const uint8_t *buf, uint32_t len, uint32_t *pos_io, H2Frame *out);

#endif // HTTP2_H
