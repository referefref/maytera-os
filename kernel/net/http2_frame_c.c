/* http2_frame_next_c - extraction of the net/http2.c inline framing (the
   http2_get() read loop). #404 HTTP/2 frame-parse seam.

   NOT A VERBATIM TRANSLITERATION, and NOT A FAITHFUL ROLLBACK TARGET. Corrected
   2026-07-16 by the #404 3-way drift audit; this header previously called itself
   a "verbatim-faithful extraction", which overstated it. Read the FRAME
   RE-REPRESENTATION note at the bottom of this comment before relying on it.

   This mirrors the LIVE per-frame-type handling, including the PADDED pad-length
   read that is performed WITHOUT first checking that the frame has at least one
   payload byte:

       // net/http2.c DATA case (flen==0 => payload==NULL):
       if (fflags & 0x08) { uint8_t padlen = payload[0]; ... }
       // net/http2.c HEADERS case (flen==0 => pp==NULL):
       if (fflags & 0x08) { padlen = pp[off]; off += 1; }

   In the running kernel `payload`/`pp` is NULL when flen==0 (the payload is
   only kmalloc'd when flen>0), so payload[0] is a NULL/OOB read
   (MAYTERA-SEC-2026-0010, CWE-476 rooted in CWE-125). This contiguous-buffer C
   reference keeps the SAME missing bound so the offline differential harness can
   prove the C read is out of bounds while the Rust seam confines it. It is kept
   only as the -DRUST_HTTP2_FRAME=off rollback path + the boot differential; the
   LIVE kernel path routes to http2_frame_next_rs and never reaches the bug.

   ---------------------------------------------------------------------------
   FRAME RE-REPRESENTATION: why this is NOT a faithful rollback (audit finding).

   The b822 extraction changed the frame's REPRESENTATION AT THE CALL SITE, not
   just the code that walks it. Pre-b822, http2_get() held the 9-byte header and a
   SEPARATELY kmalloc'd payload that was NULL when flen==0. It now assembles both
   into ONE contiguous kmalloc(9 + flen) (net/http2.c, the http2_frame_next call
   site) and hands that to this seam. That is a caller-side change, so this
   function CANNOT reproduce the original's behavior no matter how it is written:
   the NULL pointer the original dereferenced does not exist here.

   Concretely, for a zero-length PADDED DATA/HEADERS frame:
     ORIGINAL: payload == NULL, reads payload[0]  -> NULL deref -> KERNEL PANIC
               (deterministic; 6,916 SIGSEGV-confirmed traps in the audit).
     THIS C  : reads buf[payload_off] == buf[9] of a 9-byte allocation -> a 1-byte
               HEAP OVER-READ that SUCCEEDS, and the walk CONTINUES with an
               attacker-influenced pad_len. ASan: "READ of size 1 ... 0 bytes to
               the right of 9-byte region".
   So dropping -DRUST_HTTP2_FRAME converts a deterministic panic into a SILENT
   adjacent-heap read whose value an attacker influences. That is a different
   failure mode from the original and, in the silent-corruption sense, a worse
   one. It is retained deliberately (it is what lets the differential witness
   advisory 0010's missing bound), but it must not be described as "what the C
   did". Only reachable if someone drops the flag; the shipped path is Rust and
   was independently verified clean over 1.35M vectors.

   If a genuine behavioral rollback is ever needed here, reverting this seam is
   NOT sufficient: the caller's contiguous-buffer assembly must be reverted too.
   --------------------------------------------------------------------------- */

#include "http2.h"

int http2_frame_next_c(const uint8_t *buf, uint32_t len,
                       uint32_t *pos_io, H2Frame *out) {
    if (!buf || !pos_io || !out) return -1;
    uint32_t pos = *pos_io;
    if (pos == len) return 0;
    if (pos > len) return -1;
    uint32_t avail = len - pos;
    if (avail < 9) return -1;                       /* truncated header */

    uint32_t flen = ((uint32_t)buf[pos] << 16) |
                    ((uint32_t)buf[pos + 1] << 8) |
                     (uint32_t)buf[pos + 2];
    uint8_t  ftype  = buf[pos + 3];
    uint8_t  fflags = buf[pos + 4];
    uint32_t stream = (((uint32_t)buf[pos + 5] << 24) |
                       ((uint32_t)buf[pos + 6] << 16) |
                       ((uint32_t)buf[pos + 7] << 8) |
                        (uint32_t)buf[pos + 8]) & 0x7fffffff;

    if ((uint64_t)9 + flen > (uint64_t)avail) return -1;  /* length past buffer */

    uint32_t payload_off = pos + 9;
    uint32_t data_off = payload_off, data_len = flen, pad_len = 0;

    if (ftype == 0x00) {                            /* DATA */
        if (fflags & 0x08) {
            pad_len = buf[payload_off];             /* <-- UNCHECKED read (live bug) */
            uint32_t off = 1;
            if ((uint64_t)off + pad_len <= flen) {
                data_off = payload_off + off;
                data_len = flen - off - pad_len;
            } else {
                data_off = payload_off; data_len = 0;
            }
        }
    } else if (ftype == 0x01) {                     /* HEADERS */
        uint32_t off = 0;
        if (fflags & 0x08) {
            pad_len = buf[payload_off];             /* <-- UNCHECKED read (live bug) */
            off += 1;
        }
        if (fflags & 0x20) off += 5;                /* PRIORITY */
        if ((uint64_t)off + pad_len <= flen) {
            data_off = payload_off + off;
            data_len = flen - off - pad_len;
        } else {
            data_off = payload_off; data_len = 0;
        }
    }

    out->length = flen; out->stream_id = stream; out->hdr_off = pos;
    out->payload_off = payload_off; out->data_off = data_off; out->data_len = data_len;
    out->type_ = ftype; out->flags = fflags; out->pad_len = (uint8_t)pad_len;
    out->_reserved = 0;
    *pos_io = pos + 9 + flen;
    return 1;
}
