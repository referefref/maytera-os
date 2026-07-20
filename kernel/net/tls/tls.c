// tls.c - TLS 1.2/1.3 client implementation for MayteraOS
// TLS 1.2: disabled (see #fix-tls-certverify note below) - handshake fails
//          closed with a clear error instead of running a non-functional
//          static-RSA key exchange.
// TLS 1.3: X25519 key exchange with AES-128-GCM-SHA256 / AES-256-GCM-SHA384

#include "tls.h"
#include "tls13.h"
#include "cert_store.h"
#include "../../crypto/crypto.h"
#include "../../crypto/ecdsa.h"   // #502: ecdh_* for the TLS 1.2 ECDHE exchange
#include "../../crypto/rsa.h"     // #502: rsa_verify_pss_* for the 1.2 selftest
#include "../../cpu/mono.h"    // #525: the shared TSC-backed monotonic clock
#include "../../string.h"
#include "../../mm/heap.h"
#include "../../serial.h"
int g_tls_force_h1_alpn = 0;  // #185: when set, ClientHello advertises http/1.1-only ALPN (no h2) so https_post gets an HTTP/1.1 connection (our h2 client has no POST)

extern void net_poll(void);
extern void proc_sleep(uint32_t ms);
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;
int g_tls_dbg = 0;   /* #190: TLS recv instrumentation (off; flip to 1 to trace) */

// ============================================================================
// #404 / #502: TLS length-parse framing seam - C references + live dispatchers.
//
// These are the PURE byte-framing / length-bounding of untrusted wire input:
// the 5-byte record header, the 1-byte-type + 3-byte-length handshake message
// walk, and (in cert_store.h) the certificate_list walk. Each *_c below is the
// verbatim/hardened C reference kept for the boot-time differential AND for
// trivial rollback (drop -DRUST_TLS_PARSE and the dispatchers fall straight back
// to _c). Each dispatcher routes to the Rust port (rustkern.rs) under
// -DRUST_TLS_PARSE. NO crypto/AEAD/key-schedule/DER-parse crosses into Rust.
//
// SECURITY: the ORIGINAL inline TLS 1.2 plaintext handshake loop (the
// ServerHello-flight parser below, run for EVERY handshake) was
// `while (pos < length) { hs_type=data[pos];
//    hs_len=data[pos+1]<<16|data[pos+2]<<8|data[pos+3]; pos+=4;
//    ...process(data+pos, hs_len)...; pos+=hs_len; }`. The guard `pos < length`
// covers only data[pos] but the code then read data[pos+1..pos+3] (up to 3 bytes
// past the kmalloc(length) record buffer) AND passed an UNBOUNDED 3-byte hs_len
// into tls_process_server_hello() as its length, whose internal bounds then
// checked against the attacker-declared length instead of the real remaining
// bytes: a malicious/MITM server over-read the record heap allocation on its
// FIRST flight, before any certificate or crypto check. Routing that loop
// through tls_hs_next() (which checks BOTH the 4-byte header extent AND the body
// extent against the buffer) removes the class. C-fallback hardening of the same
// two bounds (so a -DRUST_TLS_PARSE rollback stays safe) is task #503.
// ============================================================================

// Lock the FFI struct layouts so the Rust #[repr(C)] mirrors can never drift.
_Static_assert(sizeof(tls_record_hdr_t) == 6, "tls_record_hdr_t must be 6 bytes (Rust FFI)");
_Static_assert(sizeof(tls_hs_msg_t) == 12, "tls_hs_msg_t must be 12 bytes (Rust FFI)");
_Static_assert(sizeof(tls_cert_ent_t) == 8, "tls_cert_ent_t must be 8 bytes (Rust FFI)");
_Static_assert(sizeof(tls12_ske_t) == 32, "tls12_ske_t must be 32 bytes (Rust FFI)");
_Static_assert(sizeof(tls13_cv_t) == 12, "tls13_cv_t must be 12 bytes (Rust FFI)");

// Record-header parse (verbatim behavior of the old inline tls_recv_record head:
// decode content_type/version/length, bound length <= TLS_MAX_RECORD_SIZE).
int tls_parse_record_header_c(const uint8_t *hdr, uint32_t len, tls_record_hdr_t *out) {
    if (!hdr || len < 5) return TLS_ERR_INVALID_PARAM;
    uint8_t content_type = hdr[0];
    uint16_t version = ((uint16_t)hdr[1] << 8) | hdr[2];
    uint16_t length  = ((uint16_t)hdr[3] << 8) | hdr[4];
    if (out) {
        out->content_type = content_type;
        out->reserved = 0;
        out->version = version;
        out->length = length;
    }
    if ((uint32_t)length > TLS_MAX_RECORD_SIZE) return TLS_ERR_INVALID_PARAM;
    return TLS_SUCCESS;
}

// Handshake-message walk step. 64-bit sums so the bound checks match the Rust
// checked-arithmetic form exactly for every u32 input (no wrap divergence).
int tls_hs_next_c(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_hs_msg_t *out) {
    if (!buf || !pos || !out) return -1;
    uint32_t p = *pos;
    if ((uint64_t)p + 4 > (uint64_t)len) return 0;   // header must fit -> clean end
    uint8_t  hs_type = buf[p];
    uint32_t hs_len  = ((uint32_t)buf[p + 1] << 16) |
                       ((uint32_t)buf[p + 2] << 8)  |
                       (uint32_t)buf[p + 3];
    if ((uint64_t)p + 4 + (uint64_t)hs_len > (uint64_t)len) return -1; // body overruns
    out->hs_type = hs_type;
    out->_pad[0] = out->_pad[1] = out->_pad[2] = 0;
    out->hs_len = hs_len;
    out->body_off = p + 4;
    *pos = p + 4 + hs_len;
    return 1;
}

// Bare TLS 1.2 certificate_list step (3-byte len + DER).
int tls_cert_next_c(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out) {
    if (!buf || !pos || !out) return -1;
    uint32_t p = *pos;
    if ((uint64_t)p + 3 > (uint64_t)len) return 0;
    uint32_t cert_len = ((uint32_t)buf[p] << 16) | ((uint32_t)buf[p + 1] << 8) | (uint32_t)buf[p + 2];
    if ((uint64_t)p + 3 + (uint64_t)cert_len > (uint64_t)len) return -1;
    out->cert_off = p + 3;
    out->cert_len = cert_len;
    *pos = p + 3 + cert_len;
    return 1;
}

// TLS 1.3 CertificateEntry step (3-byte cert len + DER + 2-byte ext len + ext).
int tls13_cert_next_c(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out) {
    if (!buf || !pos || !out) return -1;
    uint32_t p = *pos;
    if ((uint64_t)p + 3 > (uint64_t)len) return 0;
    uint32_t cert_len = ((uint32_t)buf[p] << 16) | ((uint32_t)buf[p + 1] << 8) | (uint32_t)buf[p + 2];
    uint64_t cert_end = (uint64_t)p + 3 + (uint64_t)cert_len;
    if (cert_end > (uint64_t)len) return -1;
    if (cert_end + 2 > (uint64_t)len) return -1;
    uint32_t ce = (uint32_t)cert_end;
    uint32_t ext_len = ((uint32_t)buf[ce] << 8) | (uint32_t)buf[ce + 1];
    uint64_t ext_end = cert_end + 2 + (uint64_t)ext_len;
    if (ext_end > (uint64_t)len) return -1;
    out->cert_off = p + 3;
    out->cert_len = cert_len;
    *pos = (uint32_t)ext_end;
    return 1;
}

// Live dispatchers. With -DRUST_TLS_PARSE (Makefile CFLAGS) the untrusted parse
// runs the Rust port; otherwise the _c reference above.
int tls_parse_record_header(const uint8_t *hdr, uint32_t len, tls_record_hdr_t *out) {
#ifdef RUST_TLS_PARSE
    return tls_parse_record_header_rs(hdr, len, out);
#else
    return tls_parse_record_header_c(hdr, len, out);
#endif
}
int tls_hs_next(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_hs_msg_t *out) {
#ifdef RUST_TLS_PARSE
    return tls_hs_next_rs(buf, len, pos, out);
#else
    return tls_hs_next_c(buf, len, pos, out);
#endif
}
int tls_cert_next(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out) {
#ifdef RUST_TLS_PARSE
    return tls_cert_next_rs(buf, len, pos, out);
#else
    return tls_cert_next_c(buf, len, pos, out);
#endif
}
int tls13_cert_next(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out) {
#ifdef RUST_TLS_PARSE
    return tls13_cert_next_rs(buf, len, pos, out);
#else
    return tls13_cert_next_c(buf, len, pos, out);
#endif
}

// Build TLS record header
// ============================================================================
// #502 / #525: bound TLS waits in REAL time, not in timer ticks.
//
// timer_ticks counts tick DELIVERY, not elapsed time. Under host load KVM
// reinjects a starved vCPU's missed ticks in a burst (6630 ticks delivered in
// 60ms of real time was measured on this host, min gap 3us against a nominal
// 4000us), so a `timer_ticks + N` deadline can expire in a few milliseconds of
// real time and abort a perfectly healthy handshake. The symptom is an
// intermittent TLS_ERR_TIMEOUT that looks exactly like a flaky server, which is
// about the worst way to lose an afternoon.
//
// This matters more for TLS 1.2 (#502) than it did before: the 1.2 ECDHE path
// adds two P-384 scalar multiplications and an RSA-PSS verify to the handshake,
// so there is real compute between record reads and less slack against a
// deadline that can collapse.
//
// Uses the ONE shared clock (cpu/mono.h, #525) rather than a private timer -
// hand-rolled per-subsystem clocks are exactly how this bug family spread. If
// calibration failed, mono_ready() is 0 and we fall back to the old tick
// behaviour, which is never worse than the status quo.
// ============================================================================
static void tls_set_deadline(tls_context_t *ctx, uint32_t seconds) {
    if (mono_ready()) {
        ctx->hs_deadline_ms = mono_ms() + (uint64_t)seconds * 1000;
        ctx->hs_deadline = 0;
    } else {
        ctx->hs_deadline_ms = 0;
        ctx->hs_deadline = timer_ticks + (uint64_t)(g_timer_hz ? g_timer_hz : 250) * seconds;
    }
}

static void tls_clear_deadline(tls_context_t *ctx) {
    ctx->hs_deadline = 0;
    ctx->hs_deadline_ms = 0;
}

static int tls_deadline_expired(tls_context_t *ctx) {
    if (ctx->hs_deadline_ms) return mono_ms() >= ctx->hs_deadline_ms;
    if (ctx->hs_deadline)    return (int64_t)(timer_ticks - ctx->hs_deadline) >= 0;
    return 0;   // no deadline armed
}

static void tls_build_record_header(uint8_t *header, uint8_t content_type,
                                     uint16_t version, uint16_t length) {
    header[0] = content_type;
    header[1] = (version >> 8) & 0xff;
    header[2] = version & 0xff;
    header[3] = (length >> 8) & 0xff;
    header[4] = length & 0xff;
}

// Send raw TLS record
static int tls_send_record(tls_context_t *ctx, uint8_t content_type,
                           const uint8_t *data, size_t length) {
    uint8_t header[5];
    tls_build_record_header(header, content_type, TLS_VERSION_1_2, length);

    int ret = ctx->send_func(ctx->user_data, header, 5);
    if (ret < 0) return ret;

    ret = ctx->send_func(ctx->user_data, data, length);
    if (ret < 0) return ret;

    return 0;
}

// Send encrypted TLS 1.2 record (after handshake)
static int tls_send_encrypted(tls_context_t *ctx, uint8_t content_type,
                               const uint8_t *data, size_t length) {
    uint8_t nonce[12];
    memcpy(nonce, ctx->client_write_iv, 4);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (ctx->client_seq >> (56 - i * 8)) & 0xff;
    }

    uint8_t aad[13];
    for (int i = 0; i < 8; i++) {
        aad[i] = (ctx->client_seq >> (56 - i * 8)) & 0xff;
    }
    aad[8] = content_type;
    aad[9] = 0x03;
    aad[10] = 0x03;
    aad[11] = (length >> 8) & 0xff;
    aad[12] = length & 0xff;

    size_t ciphertext_len = 8 + length + TLS_GCM_TAG_SIZE;
    uint8_t *ciphertext = kmalloc(ciphertext_len);
    if (!ciphertext) return TLS_ERR_NO_MEMORY;

    memcpy(ciphertext, nonce + 4, 8);

    aes_gcm_ctx_t gcm;
    aes_gcm_init(&gcm, ctx->client_write_key, ctx->key_size * 8, nonce, 12);
    aes_gcm_aad(&gcm, aad, 13);
    aes_gcm_encrypt(&gcm, data, ciphertext + 8, length);
    aes_gcm_final(&gcm, ciphertext + 8 + length, TLS_GCM_TAG_SIZE);

    int ret = tls_send_record(ctx, content_type, ciphertext, ciphertext_len);

    kfree(ciphertext);
    ctx->client_seq++;

    return ret;
}

// Receive TLS record
static int tls_recv_record(tls_context_t *ctx, uint8_t *content_type,
                            uint8_t **data, size_t *length) {
    uint8_t header[5];
    int ret;

    size_t received = 0;
    while (received < 5) {
        ret = ctx->recv_func(ctx->user_data, header + received, 5 - received);
        if (ret == TLS_ERR_WOULD_BLOCK) {
            /* #190: at a clean record boundary let the caller yield/retry; once a
               record header is partially read, NEVER abandon it (that desyncs the
               TLS stream) - keep polling until the rest arrives. */
            if (received == 0) return ret;
            net_poll(); proc_sleep(2);
            if (tls_deadline_expired(ctx))
                return TLS_ERR_TIMEOUT;   /* #277: stalled handshake -> abort */
            continue;
        }
        if (ret < 0) return ret;
        if (ret == 0) return TLS_ERR_CLOSED;
        received += ret;
    }

    // Record-header parse + length bound is the Rust framing seam
    // (tls_parse_record_header, #404/#502): content_type + version + the 2-byte
    // body length, bounded <= TLS_MAX_RECORD_SIZE before the body kmalloc below.
    tls_record_hdr_t rec;
    if (tls_parse_record_header(header, 5, &rec) != TLS_SUCCESS) {
        return TLS_ERR_INVALID_PARAM;
    }
    *content_type = rec.content_type;
    *length = rec.length;

    *data = kmalloc(*length);
    if (!*data) return TLS_ERR_NO_MEMORY;

    received = 0;
    while (received < *length) {
        ret = ctx->recv_func(ctx->user_data, *data + received, *length - received);
        if (ret == TLS_ERR_WOULD_BLOCK) {
            /* #190: mid-record body - do not abandon (would desync), keep polling. */
            net_poll(); proc_sleep(2);
            if (tls_deadline_expired(ctx))
                return TLS_ERR_TIMEOUT;   /* #277: stalled handshake -> abort */
            continue;
        }
        if (ret < 0) {
            kfree(*data);
            return ret;
        }
        if (ret == 0) {
            kfree(*data);
            return TLS_ERR_CLOSED;
        }
        received += ret;
    }

    if (g_tls_dbg) kprintf("[TLSDBG] rec type=%u len=%u\n", *content_type, (unsigned)*length);
    return 0;
}

// Receive and decrypt TLS 1.2 record
static int tls_recv_encrypted(tls_context_t *ctx, uint8_t *content_type,
                               uint8_t **data, size_t *length) {
    uint8_t *ciphertext;
    size_t cipher_len;

    int ret = tls_recv_record(ctx, content_type, &ciphertext, &cipher_len);
    if (ret < 0) return ret;

    if (*content_type == TLS_CONTENT_ALERT) {
        *data = ciphertext;
        *length = cipher_len;
        return 0;
    }

    if (cipher_len < 8 + TLS_GCM_TAG_SIZE) {
        kfree(ciphertext);
        return TLS_ERR_INVALID_PARAM;
    }

    size_t plaintext_len = cipher_len - 8 - TLS_GCM_TAG_SIZE;

    uint8_t nonce[12];
    memcpy(nonce, ctx->server_write_iv, 4);
    memcpy(nonce + 4, ciphertext, 8);

    uint8_t aad[13];
    for (int i = 0; i < 8; i++) {
        aad[i] = (ctx->server_seq >> (56 - i * 8)) & 0xff;
    }
    aad[8] = *content_type;
    aad[9] = 0x03;
    aad[10] = 0x03;
    aad[11] = (plaintext_len >> 8) & 0xff;
    aad[12] = plaintext_len & 0xff;

    *data = kmalloc(plaintext_len);
    if (!*data) {
        kfree(ciphertext);
        return TLS_ERR_NO_MEMORY;
    }

    aes_gcm_ctx_t gcm;
    aes_gcm_init(&gcm, ctx->server_write_key, ctx->key_size * 8, nonce, 12);
    aes_gcm_aad(&gcm, aad, 13);
    aes_gcm_decrypt(&gcm, ciphertext + 8, *data, plaintext_len);

    if (aes_gcm_verify(&gcm, ciphertext + 8 + plaintext_len, TLS_GCM_TAG_SIZE) != 0) {
        kfree(*data);
        kfree(ciphertext);
        return TLS_ERR_VERIFY;
    }

    *length = plaintext_len;
    ctx->server_seq++;

    kfree(ciphertext);
    return 0;
}

// ============================================================================
// #fix-tls-certverify: certificate chain verification, wired into both the
// TLS 1.2 and TLS 1.3 handshake paths below. Previously the CERTIFICATE
// handshake message was received and just logged ("Received server
// certificate") - the bytes were thrown away and cert_verify_chain() in
// cert_store.c was never called from anywhere in the codebase.
// ============================================================================

// Run the actual chain-of-trust / hostname / validity check. Returns
// TLS_SUCCESS or TLS_ERR_CERTIFICATE. When ctx->verify_cert is 0 (opt-out,
// e.g. explicit non-HTTPS internal tooling) this is a no-op that trusts
// whatever was presented, matching the old behavior for that opt-out case.
static int tls_verify_chain(tls_context_t *ctx, cert_chain_t *chain) {
    if (!ctx->verify_cert) {
        return TLS_SUCCESS;
    }
    if (!chain || chain->count == 0) {
        kprintf("[TLS] Certificate verification: empty/unparseable chain - rejecting\n");
        return TLS_ERR_CERTIFICATE;
    }

    int ret = cert_verify_chain(chain, ctx->hostname[0] ? ctx->hostname : NULL);
    if (ret != CERT_SUCCESS) {
        kprintf("[TLS] Certificate chain verification FAILED (err=%d) host=%s leaf-CN=%s\n",
                ret, ctx->hostname, cert_get_cn(chain->certs[0]));
        return TLS_ERR_CERTIFICATE;
    }

    kprintf("[TLS] Certificate chain verified OK: host=%s leaf-CN=%s\n",
            ctx->hostname, cert_get_cn(chain->certs[0]));
    return TLS_SUCCESS;
}

// TLS 1.2 Certificate message body:
//   opaque certificate_list<0..2^24-1>  (3-byte length) of
//     opaque ASN1Cert<1..2^24-1>        (3-byte length each, no extensions)
// This is exactly what cert_store.c's cert_parse_chain() expects once the
// outer 3-byte total-length prefix is skipped.
static int tls12_parse_certificate_msg(const uint8_t *data, size_t len, cert_chain_t *chain) {
    chain->count = 0;
    if (len < 3) return -1;
    uint32_t list_len = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    size_t avail = len - 3;
    if (list_len > avail) list_len = (uint32_t)avail;
    return cert_parse_chain(data + 3, list_len, chain);
}

// TLS 1.3 Certificate message body (RFC 8446 4.4.2):
//   opaque certificate_request_context<0..2^8-1>   (1-byte length)
//   CertificateEntry certificate_list<0..2^24-1>    (3-byte length), each:
//     opaque cert_data<1..2^24-1>                   (3-byte length)
//     Extension extensions<0..2^16-1>               (2-byte length)
// The per-entry extensions field means this can't reuse cert_parse_chain()
// directly (that assumes bare [3-byte len][cert] with nothing in between).
static int tls13_parse_certificate_msg(const uint8_t *data, size_t len, cert_chain_t *chain) {
    chain->count = 0;
    if (len < 1) return -1;
    size_t pos = 0;
    uint8_t ctx_len = data[pos++];
    if (pos + ctx_len > len) return -1;
    pos += ctx_len;

    if (pos + 3 > len) return -1;
    uint32_t list_len = ((uint32_t)data[pos] << 16) | ((uint32_t)data[pos + 1] << 8) | data[pos + 2];
    pos += 3;
    size_t list_end = pos + list_len;
    if (list_end > len) list_end = len;

    // Per-CertificateEntry framing (3-byte cert len + DER + 2-byte ext len +
    // ext) is the Rust seam tls13_cert_next (#404/#502), walked over the
    // certificate_list region [pos, list_end). cert_parse_der (X.509) stays C.
    // Matches the old inline walk: a cert is added iff the WHOLE entry (cert +
    // extensions) is well-framed; a malformed length stops the walk (no add,
    // no over-read).
    const uint8_t *base = data + pos;
    uint32_t region = (uint32_t)(list_end - pos);
    uint32_t wp = 0;
    tls_cert_ent_t ent;
    while (chain->count < CERT_MAX_CHAIN_DEPTH &&
           tls13_cert_next(base, region, &wp, &ent) == 1) {
        cert_x509_t *cert = cert_parse_der(base + ent.cert_off, ent.cert_len);
        if (cert) chain->certs[chain->count++] = cert;
    }

    return (chain->count > 0) ? 0 : -1;
}

// Add data to handshake transcript (TLS 1.2)
static void tls_transcript_add(tls_context_t *ctx, const uint8_t *data, size_t length) {
    if (ctx->handshake_hash_len + length > ctx->handshake_hash_cap) {
        size_t new_cap = ctx->handshake_hash_cap * 2;
        if (new_cap < ctx->handshake_hash_len + length) {
            new_cap = ctx->handshake_hash_len + length + 1024;
        }
        uint8_t *new_buf = krealloc(ctx->handshake_hash_data, new_cap);
        if (!new_buf) return;
        ctx->handshake_hash_data = new_buf;
        ctx->handshake_hash_cap = new_cap;
    }
    memcpy(ctx->handshake_hash_data + ctx->handshake_hash_len, data, length);
    ctx->handshake_hash_len += length;
}

// ============================================================================
// TLS 1.3 Encrypted Record Layer
// ============================================================================

// Receive and decrypt a TLS 1.3 encrypted handshake record
static int tls13_recv_encrypted_hs(tls_context_t *ctx, uint8_t *inner_type,
                                    uint8_t **data, size_t *length) {
    uint8_t content_type;
    uint8_t *raw;
    size_t raw_len;

    int ret = tls_recv_record(ctx, &content_type, &raw, &raw_len);
    if (ret < 0) return ret;

    // Skip ChangeCipherSpec (middlebox compatibility)
    if (content_type == TLS_CONTENT_CHANGE_CIPHER) {
        kfree(raw);
        return tls13_recv_encrypted_hs(ctx, inner_type, data, length);
    }

    // Alerts may arrive unencrypted
    if (content_type == TLS_CONTENT_ALERT) {
        *inner_type = TLS_CONTENT_ALERT;
        *data = raw;
        *length = raw_len;
        return 0;
    }

    // TLS 1.3 encrypted records use application_data (0x17) as outer type
    if (content_type != TLS_CONTENT_APPLICATION) {
        kfree(raw);
        return TLS_ERR_HANDSHAKE;
    }

    // Decrypt with server handshake key
    *data = kmalloc(raw_len);
    if (!*data) {
        kfree(raw);
        return TLS_ERR_NO_MEMORY;
    }

    ret = tls13_decrypt_record(
        ctx->tls13_server_write_key,
        ctx->tls13_server_write_iv,
        ctx->tls13_server_seq++,
        raw, raw_len,
        *data, length, inner_type,
        ctx->tls13_cipher_suite
    );

    kfree(raw);
    if (ret != 0) {
        kfree(*data);
        *data = NULL;
        kprintf("[TLS1.3] Decryption failed (seq=%llu)\n",
                ctx->tls13_server_seq - 1);
        return TLS_ERR_VERIFY;
    }

    return 0;
}

// Process TLS 1.3 encrypted handshake (after ServerHello)
static int tls13_process_handshake(tls_context_t *ctx) {
    while (ctx->state != TLS_STATE_ESTABLISHED) {
        uint8_t inner_type;
        uint8_t *data;
        size_t length;

        int ret = tls13_recv_encrypted_hs(ctx, &inner_type, &data, &length);
        if (ret < 0) return ret;

        if (inner_type == TLS_CONTENT_ALERT) {
            if (length >= 2) {
                kprintf("[TLS1.3] Alert: level=%d desc=%d\n", data[0], data[1]);
            }
            kfree(data);
            return TLS_ERR_ALERT;
        }

        if (inner_type != TLS_CONTENT_HANDSHAKE) {
            kfree(data);
            continue;
        }

        // Parse handshake messages (may be coalesced in one record). The 1-byte
        // type + 3-byte length header read and its bound are the Rust framing
        // seam (tls_hs_next, #404/#502): != 1 stops the walk cleanly (no over-
        // read on a truncated or overrunning length).
        uint32_t pos = 0;
        tls_hs_msg_t hm;
        while (tls_hs_next(data, (uint32_t)length, &pos, &hm) == 1) {
            uint8_t hs_type = hm.hs_type;
            uint32_t hs_len = hm.hs_len;
            const uint8_t *hbody = data + hm.body_off;  // body (== old data+pos after +=4)

            // #fix-tls-certverify: snapshot the transcript hash state from
            // BEFORE this message is appended. Needed to verify the server's
            // Finished verify_data, which per RFC 8446 4.4.4 covers the
            // transcript up to but NOT including the Finished message itself.
            sha256_ctx_t pre_hash256 = ctx->transcript_hash;
            sha512_ctx_t pre_hash384 = ctx->transcript_hash384;

            // Add this handshake message (header + body) to both candidate
            // transcript hashes. hm.body_off == pos+4, so body_off-4 is the start.
            sha256_update(&ctx->transcript_hash, data + hm.body_off - 4, 4 + hs_len);
            sha512_update(&ctx->transcript_hash384, data + hm.body_off - 4, 4 + hs_len);

            switch (hs_type) {
                case TLS_HANDSHAKE_ENCRYPTED_EXTENSIONS: {
                    kprintf("[TLS1.3] EncryptedExtensions (%u bytes)\n", hs_len);
                    // Parse extensions to find the server-selected ALPN protocol.
                    // EE body: u16 extensions_length, then a list of extensions
                    // (u16 type, u16 len, body). ALPN ext type = 16 (0x0010);
                    // body = u16 proto-list-length, u8 proto-length, proto bytes.
                    const uint8_t *ee = hbody;
                    if (hs_len >= 2) {
                        uint32_t ext_total = ((uint32_t)ee[0] << 8) | ee[1];
                        uint32_t ep = 2;
                        if (ext_total + 2 > hs_len) ext_total = (hs_len >= 2) ? (hs_len - 2) : 0;
                        while (ep + 4 <= 2 + ext_total) {
                            uint16_t etype = ((uint16_t)ee[ep] << 8) | ee[ep+1];
                            uint16_t elen  = ((uint16_t)ee[ep+2] << 8) | ee[ep+3];
                            uint32_t ebody = ep + 4;
                            if (ebody + elen > hs_len) break;
                            if (etype == 16 && elen >= 3) {
                                // u16 list length, u8 proto length, proto bytes
                                uint8_t plen = ee[ebody + 2];
                                if (plen == 2 && ee[ebody+3] == 'h' && ee[ebody+4] == '2') {
                                    ctx->alpn_is_h2 = 1;
                                }
                                kprintf("[TLS] ALPN h2=%d (plen=%u)\n", ctx->alpn_is_h2, plen);
                            }
                            ep = ebody + elen;
                        }
                    }
                    break;
                }

                case TLS_HANDSHAKE_CERTIFICATE: {
                    kprintf("[TLS1.3] Certificate (%u bytes)\n", hs_len);

                    // #fix-tls-certverify: parse + verify the chain instead of
                    // discarding it. A parse failure while verification is
                    // required is treated as a hard failure (fail closed) -
                    // only a well-formed chain that also cryptographically
                    // verifies is accepted.
                    cert_chain_t chain;
                    if (tls13_parse_certificate_msg(hbody, hs_len, &chain) == 0) {
                        int vret = tls_verify_chain(ctx, &chain);
                        // #510: RETAIN the leaf. Its public key is what verifies
                        // CertificateVerify, which arrives as the very next
                        // message. Previously the whole chain was freed here, so
                        // the key was already gone by then. Ownership of
                        // certs[0] transfers to the context; the rest of the
                        // chain (the intermediates/root, whose only job was the
                        // chain check that just ran) is freed now.
                        if (vret == TLS_SUCCESS && chain.count > 0) {
                            if (ctx->tls13_leaf) cert_free(ctx->tls13_leaf);
                            ctx->tls13_leaf = chain.certs[0];
                            chain.certs[0] = NULL;   // don't free what we kept
                        }
                        for (int ci = 0; ci < chain.count; ci++) {
                            if (chain.certs[ci]) cert_free(chain.certs[ci]);
                        }
                        chain.count = 0;
                        if (vret != TLS_SUCCESS) {
                            kfree(data);
                            return vret;
                        }
                    } else if (ctx->verify_cert) {
                        kprintf("[TLS1.3] Failed to parse server certificate "
                                "chain - rejecting\n");
                        kfree(data);
                        return TLS_ERR_CERTIFICATE;
                    }
                    break;
                }

                case TLS_HANDSHAKE_CERTIFICATE_VERIFY: {
                    kprintf("[TLS1.3] CertificateVerify (%u bytes)\n", hs_len);

#ifdef SECTEST_CV_BASELINE
                    // #510 BASELINE control (`make CVBASELINE=1`, never
                    // shipped): behave exactly like the PRE-#510 kernel, i.e.
                    // parse the message and throw it away. Exists so that a
                    // host which fails under the CVTEST build can be attributed
                    // to #510 or EXONERATED on an otherwise byte-identical
                    // build, instead of being argued about. (feeds.bbci.co.uk
                    // fails at DNS before any TLS; this control proves that.)
                    ctx->tls13_got_cv = 1;   // satisfy the Finished gate
                    break;
#endif

                    // #510 / MAYTERA-SEC-2026-0017: actually CHECK it. This
                    // message is the server's only proof that it POSSESSES the
                    // private key for the certificate it presented (RFC 8446
                    // 4.4.3). Without this the chain check alone is worthless
                    // against an on-path attacker: certificates are public, so
                    // it can replay the real chain, run its own ECDHE, and
                    // Finished still matches (Finished is keyed off the ECDHE,
                    // not the certificate). Was: parse and discard.
                    if (!ctx->verify_cert) {
                        // Verification explicitly disabled by the caller. Match
                        // the chain check's behavior and skip; got_cv stays 0
                        // and the Finished gate below also honors verify_cert.
                        break;
                    }

                    // Fail closed: no retained leaf means either no Certificate
                    // arrived or it failed to parse. Never verify against
                    // nothing, and never let CV be "checked" without a key.
                    if (!ctx->tls13_leaf) {
                        kprintf("[TLS1.3] CertificateVerify with no server "
                                "certificate - aborting\n");
                        kfree(data);
                        return TLS_ERR_CERTIFICATE;
                    }

                    tls13_cv_t cv;
                    if (tls13_cv_parse_rs(hbody, hs_len, &cv) != 1) {
                        kprintf("[TLS1.3] CertificateVerify malformed - "
                                "aborting\n");
                        kfree(data);
                        return TLS_ERR_VERIFY;
                    }

                    // The transcript hash for CV is Transcript-Hash(ClientHello
                    // ..Certificate): up to but NOT including CV itself, so use
                    // the pre-message snapshot taken at the top of this loop.
                    // The hash is the NEGOTIATED CIPHER SUITE's (48 for the
                    // SHA-384 suite), NOT the signature scheme's: real hosts
                    // (feeds.bbci.co.uk, lwn.net, api.moonshot.ai) negotiate a
                    // SHA-384 suite yet sign with a SHA-256 scheme, and using
                    // the scheme's hash here would still pass against a
                    // SHA-256-suite server and fail only against those.
                    int cvhlen = (ctx->tls13_cipher_suite ==
                                  TLS13_AES_256_GCM_SHA384) ? 48 : 32;
                    uint8_t cv_th[48];
                    if (cvhlen == 48) {
                        sha512_ctx_t tmp = pre_hash384;
                        sha384_final(&tmp, cv_th);
                    } else {
                        sha256_ctx_t tmp = pre_hash256;
                        sha256_final(&tmp, cv_th);
                    }

                    // 64 spaces + "TLS 1.3, server CertificateVerify" + 0x00 +
                    // transcript hash = 98 + cvhlen (130 or 146).
                    uint8_t cv_content[146];
                    int cv_clen = tls13_cv_content_rs(cv_th, (uint32_t)cvhlen,
                                                      cv_content,
                                                      (uint32_t)sizeof(cv_content));
                    if (cv_clen <= 0) {
                        kprintf("[TLS1.3] CertificateVerify content build "
                                "failed - aborting\n");
                        kfree(data);
                        return TLS_ERR_VERIFY;
                    }

#ifdef SECTEST_CV_TAMPER
                    // #510 NEGATIVE CONTROL (`make CVTAMPER=1`, never shipped):
                    // flip ONE bit of the server's CertificateVerify signature.
                    // If the handshake still completes, this check is
                    // decorative and the hole is still open. Flipped AFTER the
                    // transcript hashes consumed these bytes at the top of the
                    // loop, so the running transcript is untouched and an abort
                    // can only come from the CV check itself - not from a
                    // knock-on Finished mismatch. `data` is our own kmalloc'd
                    // record buffer, so the const cast is safe.
                    ((uint8_t *)data)[hm.body_off + cv.sig_off + (cv.sig_len / 2)] ^= 0x01;
                    kprintf("[TLS1.3] SECTEST: flipped 1 bit of the CV signature "
                            "(byte %u of %u) - this handshake MUST abort\n",
                            cv.sig_len / 2, cv.sig_len);
#endif
                    int cvr = cert_verify_tls_signature(ctx->tls13_leaf,
                                                        cv.sig_scheme,
                                                        cv_content,
                                                        (size_t)cv_clen,
                                                        hbody + cv.sig_off,
                                                        cv.sig_len);
                    if (cvr != CERT_SUCCESS) {
                        kprintf("[TLS1.3] CertificateVerify signature INVALID "
                                "(scheme=0x%04x err=%d) - aborting handshake "
                                "(server does not possess the private key for "
                                "the certificate it presented)\n",
                                cv.sig_scheme, cvr);
                        kfree(data);
                        return TLS_ERR_VERIFY;
                    }

                    ctx->tls13_got_cv = 1;
                    // The leaf has done its job; drop it now rather than at
                    // tls_free() so a long-lived connection doesn't hold it.
                    cert_free(ctx->tls13_leaf);
                    ctx->tls13_leaf = NULL;
                    kprintf("[TLS1.3] CertificateVerify OK (scheme=0x%04x): "
                            "server proved possession of the certificate's "
                            "private key\n", cv.sig_scheme);
                    break;
                }

                case TLS_HANDSHAKE_FINISHED: {
                    kprintf("[TLS1.3] Server Finished\n");

                    // #510: THE GATE. Verifying CertificateVerify when it shows
                    // up is NOT enough on its own: an attacker who simply OMITS
                    // the message would skip the check entirely and land right
                    // back in the MITM hole. Server auth is mandatory in TLS 1.3
                    // (RFC 8446 4.4.3 / 9.2) and this client never offers a
                    // pre_shared_key extension (only psk_key_exchange_modes), so
                    // the server CANNOT select PSK and Certificate +
                    // CertificateVerify are mandatory on every handshake we can
                    // negotiate. Refuse to finish without proof of possession.
                    // NOTE: if PSK/session resumption is ever implemented, that
                    // path legitimately has no CertificateVerify and must gate
                    // on the resumption binder instead - do not just drop this.
                    if (ctx->verify_cert && !ctx->tls13_got_cv) {
                        kprintf("[TLS1.3] Server Finished with NO verified "
                                "CertificateVerify - aborting (server never "
                                "proved possession of its certificate's "
                                "private key)\n");
                        kfree(data);
                        return TLS_ERR_VERIFY;
                    }

                    // Negotiated hash length (48 for AES-256-GCM-SHA384)
                    int fhlen = (ctx->tls13_cipher_suite ==
                                 TLS13_AES_256_GCM_SHA384) ? 48 : 32;

                    // Derive finished_key from server handshake traffic secret
                    uint8_t finished_key[48];
                    hkdf_expand_label(ctx->tls13_server_hs_traffic, fhlen,
                                     "finished", NULL, 0,
                                     finished_key, fhlen, fhlen);

                    // #fix-tls-certverify: actually check verify_data, using
                    // the pre-Finished transcript snapshot taken above (was:
                    // "skip verify_data check for now ... in practice the
                    // server Finished is correct if decryption succeeded" -
                    // that only proves the record's AEAD tag was valid, not
                    // that the handshake transcript (ServerHello, Certificate,
                    // CertificateVerify, negotiated params) wasn't tampered
                    // with or downgraded before encryption kicked in).
                    uint8_t transcript_before_finished[48];
                    if (fhlen == 48) {
                        sha512_ctx_t tmp_pre = pre_hash384;
                        sha384_final(&tmp_pre, transcript_before_finished);
                    } else {
                        sha256_ctx_t tmp_pre = pre_hash256;
                        sha256_final(&tmp_pre, transcript_before_finished);
                    }

                    uint8_t expected_verify_data[48];
                    if (fhlen == 48) {
                        hmac_sha384(finished_key, fhlen, transcript_before_finished,
                                    fhlen, expected_verify_data);
                    } else {
                        hmac_sha256(finished_key, fhlen, transcript_before_finished,
                                    fhlen, expected_verify_data);
                    }

                    if (hs_len != (uint32_t)fhlen ||
                        crypto_memcmp(expected_verify_data, hbody, (size_t)fhlen) != 0) {
                        kprintf("[TLS1.3] Server Finished verify_data MISMATCH - "
                                "aborting handshake (possible tamper/downgrade)\n");
                        crypto_zero(finished_key, sizeof(finished_key));
                        kfree(data);
                        return TLS_ERR_VERIFY;
                    }
                    kprintf("[TLS1.3] Server Finished verify_data OK\n");

                    // Compute transcript hash with everything up to and
                    // including server Finished, using the negotiated hash.
                    uint8_t server_finished_hash[48];
                    if (fhlen == 48) {
                        sha512_ctx_t tmp_hash;
                        memcpy(&tmp_hash, &ctx->transcript_hash384,
                               sizeof(sha512_ctx_t));
                        sha384_final(&tmp_hash, server_finished_hash);
                    } else {
                        sha256_ctx_t tmp_hash;
                        memcpy(&tmp_hash, &ctx->transcript_hash,
                               sizeof(sha256_ctx_t));
                        sha256_final(&tmp_hash, server_finished_hash);
                    }

                    // Build and send client Finished
                    uint8_t client_finished_key[48];
                    hkdf_expand_label(ctx->tls13_client_hs_traffic, fhlen,
                                     "finished", NULL, 0,
                                     client_finished_key, fhlen, fhlen);

                    uint8_t client_verify[48];
                    if (fhlen == 48) {
                        hmac_sha384(client_finished_key, fhlen,
                                    server_finished_hash, fhlen,
                                    client_verify);
                    } else {
                        hmac_sha256(client_finished_key, fhlen,
                                    server_finished_hash, fhlen,
                                    client_verify);
                    }

                    // Finished message: type(1) + length(3) + verify_data(hlen)
                    uint8_t fin_msg[4 + 48];
                    fin_msg[0] = TLS_HANDSHAKE_FINISHED;
                    fin_msg[1] = 0;
                    fin_msg[2] = 0;
                    fin_msg[3] = (uint8_t)fhlen;
                    memcpy(fin_msg + 4, client_verify, fhlen);

                    // Encrypt with client handshake key
                    uint8_t enc_buf[128];
                    size_t enc_len;
                    tls13_encrypt_record(
                        ctx->tls13_client_write_key,
                        ctx->tls13_client_write_iv,
                        ctx->tls13_client_seq++,
                        TLS_CONTENT_HANDSHAKE,
                        fin_msg, 4 + fhlen, enc_buf, &enc_len,
                        ctx->tls13_cipher_suite
                    );

                    // Send as application_data (TLS 1.3 encrypted record)
                    tls_send_record(ctx, TLS_CONTENT_APPLICATION,
                                    enc_buf, enc_len);

                    kprintf("[TLS1.3] Sent client Finished\n");

                    // Derive application traffic keys using the full
                    // handshake transcript (includes server Finished)
                    tls13_key_schedule_t app_ks;
                    memset(&app_ks, 0, sizeof(app_ks));
                    app_ks.hash_len = fhlen;

                    // Copy handshake secret so we can derive master secret
                    memcpy(app_ks.handshake_secret,
                           ctx->tls13_hs_secret, 48);

                    tls13_derive_app_keys(&app_ks, server_finished_hash,
                                          ctx->tls13_cipher_suite);

                    // Install application keys
                    memcpy(ctx->tls13_client_write_key,
                           app_ks.client_write_key, 32);
                    memcpy(ctx->tls13_server_write_key,
                           app_ks.server_write_key, 32);
                    memcpy(ctx->tls13_client_write_iv,
                           app_ks.client_write_iv, 12);
                    memcpy(ctx->tls13_server_write_iv,
                           app_ks.server_write_iv, 12);

                    // Reset sequence numbers for application data
                    ctx->tls13_server_seq = 0;
                    ctx->tls13_client_seq = 0;
                    ctx->tls13_app_encrypted = 1;

                    ctx->state = TLS_STATE_ESTABLISHED;
                    kprintf("[TLS] Handshake complete (TLS 1.3)\n");

                    crypto_zero(&app_ks, sizeof(app_ks));
                    crypto_zero(finished_key, 48);
                    crypto_zero(client_finished_key, 48);
                    break;
                }

                case TLS_HANDSHAKE_NEW_SESSION_TICKET:
                    kprintf("[TLS1.3] NewSessionTicket (ignored)\n");
                    break;

                // #fix-tls-certverify: RFC 8879 CompressedCertificate. We no
                // longer advertise the compress_certificate extension (see
                // tls_send_client_hello), so a compliant server should never
                // send this - but fail closed instead of silently skipping
                // verification if a non-compliant one does anyway. Skipping
                // it here (like the old default case did for every unknown
                // type) would mean the server proved nothing about its
                // certificate at all.
                case 25:
                    if (ctx->verify_cert) {
                        kprintf("[TLS1.3] Server sent CompressedCertificate "
                                "(unsupported, and unrequested) - rejecting\n");
                        kfree(data);
                        return TLS_ERR_CERTIFICATE;
                    }
                    kprintf("[TLS1.3] CompressedCertificate (%u bytes, verify "
                            "disabled - ignoring)\n", hs_len);
                    break;

                default:
                    kprintf("[TLS1.3] Unknown handshake type: %d\n", hs_type);
                    break;
            }
            // tls_hs_next advanced `pos` past this whole message already.
        }

        kfree(data);
    }

    return 0;
}

// ============================================================================
// ClientHello (TLS 1.3 compatible)
// ============================================================================

static int tls_send_client_hello(tls_context_t *ctx) {
    // Generate client random
    rng_get_bytes(ctx->client_random, 32);

    // Generate X25519 keypair for TLS 1.3 key_share
    x25519_keypair_t kp;
    x25519_generate_keypair(&kp);
    memcpy(ctx->tls13_privkey, kp.private_key, 32);
    memcpy(ctx->tls13_pubkey, kp.public_key, 32);
    crypto_zero(&kp, sizeof(kp));

    // Build a Chrome-like ClientHello (#190 JA3/JA4 fingerprint mimic).
    // GREASE values (any of 0x0a0a..0xfafa); Chrome scatters one GREASE value
    // across cipher suites, supported_groups, supported_versions, key_share,
    // and a leading GREASE extension. The two GREASE *extensions* (leading and
    // trailing) MUST use DISTINCT codepoints: two extensions of the same type
    // is a duplicate extension and strict servers (e.g. Cloudflare) answer with
    // a fatal decode_error (alert 50). Chrome always uses two different GREASE
    // values for these two extensions.
    const uint16_t GREASE = 0x0a0a;
    const uint16_t GREASE2 = 0x1a1a;
    uint8_t hello[2048];
    int pos = 0;

    // Handshake header (filled in later)
    pos += 4;

    // Client version: 0x0303 (TLS 1.2, for middlebox compat)
    hello[pos++] = 0x03;
    hello[pos++] = 0x03;

    // Client random
    memcpy(hello + pos, ctx->client_random, 32);
    pos += 32;

    // Session ID: 32 random bytes (TLS 1.3 middlebox compatibility)
    hello[pos++] = 32;
    rng_get_bytes(hello + pos, 32);
    pos += 32;

    // Cipher suites: leading GREASE + Chrome's TLS1.3 and TLS1.2 suites.
    // NOTE: TLS1.3 key derivation here is SHA-256 only, so the server can only
    // safely negotiate 0x1301 or 0x1303. We advertise 0x1302 for fingerprint
    // shape; servers honor their own preference and pick 0x1301 normally. The
    // TLS1.2 suites are never selected because we force TLS1.3 via key_share +
    // supported_versions.
    {
        static const uint16_t suites[] = {
            0x1301, 0x1302, 0x1303,
            0xc02b, 0xc02f, 0xc02c, 0xc030,
            0xcca9, 0xcca8,
            0xc013, 0xc014,
            0x009c, 0x009d, 0x002f, 0x0035,
        };
        int nsuites = (int)(sizeof(suites) / sizeof(suites[0]));
        int cs_bytes = (nsuites + 1) * 2;  // +1 for leading GREASE
        hello[pos++] = (cs_bytes >> 8) & 0xff;
        hello[pos++] = cs_bytes & 0xff;
        hello[pos++] = (GREASE >> 8) & 0xff;
        hello[pos++] = GREASE & 0xff;
        for (int i = 0; i < nsuites; i++) {
            hello[pos++] = (suites[i] >> 8) & 0xff;
            hello[pos++] = suites[i] & 0xff;
        }
    }

    // Compression methods: null only
    hello[pos++] = 1;
    hello[pos++] = 0x00;

    // Extensions
    int ext_start = pos;
    pos += 2;  // Extension length (filled in later)

    // --- Extension: GREASE (empty) ---
    hello[pos++] = (GREASE >> 8) & 0xff;
    hello[pos++] = GREASE & 0xff;
    hello[pos++] = 0x00;
    hello[pos++] = 0x00;

    // --- Extension: server_name / SNI (type 0) ---
    if (ctx->hostname[0]) {
        int hostname_len = strlen(ctx->hostname);
        hello[pos++] = 0x00;
        hello[pos++] = 0x00;  // Extension type: server_name
        int sni_data_len = hostname_len + 5;
        hello[pos++] = (sni_data_len >> 8) & 0xff;
        hello[pos++] = sni_data_len & 0xff;
        int sni_list_len = hostname_len + 3;
        hello[pos++] = (sni_list_len >> 8) & 0xff;
        hello[pos++] = sni_list_len & 0xff;
        hello[pos++] = 0;    // Name type: host_name
        hello[pos++] = (hostname_len >> 8) & 0xff;
        hello[pos++] = hostname_len & 0xff;
        memcpy(hello + pos, ctx->hostname, hostname_len);
        pos += hostname_len;
    }

    // --- Extension: extended_master_secret (type 23, empty) ---
    hello[pos++] = 0x00;
    hello[pos++] = 0x17;
    hello[pos++] = 0x00;
    hello[pos++] = 0x00;

    // --- Extension: renegotiation_info (type 0xff01, 1 byte 0x00) ---
    hello[pos++] = 0xff;
    hello[pos++] = 0x01;
    hello[pos++] = 0x00;
    hello[pos++] = 0x01;
    hello[pos++] = 0x00;

    // --- Extension: supported_groups (type 10): GREASE, x25519, secp256r1, secp384r1 ---
    hello[pos++] = 0x00;
    hello[pos++] = 0x0a;
    hello[pos++] = 0x00;
    hello[pos++] = 0x0a;  // ext data length = 10
    hello[pos++] = 0x00;
    hello[pos++] = 0x08;  // named group list length = 8
    hello[pos++] = (GREASE >> 8) & 0xff;
    hello[pos++] = GREASE & 0xff;
    hello[pos++] = 0x00;
    hello[pos++] = 0x1d;  // x25519
    hello[pos++] = 0x00;
    hello[pos++] = 0x17;  // secp256r1
    hello[pos++] = 0x00;
    hello[pos++] = 0x18;  // secp384r1

    // --- Extension: ec_point_formats (type 11): uncompressed only ---
    hello[pos++] = 0x00;
    hello[pos++] = 0x0b;
    hello[pos++] = 0x00;
    hello[pos++] = 0x02;  // ext data length = 2
    hello[pos++] = 0x01;  // formats length = 1
    hello[pos++] = 0x00;  // uncompressed

    // --- Extension: session_ticket (type 35, empty) ---
    hello[pos++] = 0x00;
    hello[pos++] = 0x23;
    hello[pos++] = 0x00;
    hello[pos++] = 0x00;

    // --- Extension: ALPN (type 16): h2 + http/1.1 ---
    // Advertise HTTP/2 first, then HTTP/1.1 as a fallback. The server-selected
    // protocol is parsed from EncryptedExtensions (see tls_alpn_is_h2). When the
    // server picks h2 the caller (https_get) routes through the HTTP/2 client.
    // ALPN proto list = [0x02 'h' '2'][0x08 "http/1.1"] = 14 bytes.
    // list length = 14, ext data length = 16.
    hello[pos++] = 0x00;
    hello[pos++] = 0x10;
    if (g_tls_force_h1_alpn) {
        // #185: http/1.1 ONLY (no h2) so the server serves HTTP/1.1 for POST.
        hello[pos++] = 0x00; hello[pos++] = 0x0b;  // ext data length = 11
        hello[pos++] = 0x00; hello[pos++] = 0x09;  // ALPN list length = 9
        hello[pos++] = 0x08;                        // proto len 8: "http/1.1"
        hello[pos++] = 'h'; hello[pos++] = 't'; hello[pos++] = 't'; hello[pos++] = 'p';
        hello[pos++] = '/'; hello[pos++] = '1'; hello[pos++] = '.'; hello[pos++] = '1';
    } else {
        hello[pos++] = 0x00; hello[pos++] = 0x0e;  // ext data length = 14
        hello[pos++] = 0x00; hello[pos++] = 0x0c;  // ALPN list length = 12
        hello[pos++] = 0x02; hello[pos++] = 'h'; hello[pos++] = '2';  // "h2"
        hello[pos++] = 0x08;                        // "http/1.1"
        hello[pos++] = 'h'; hello[pos++] = 't'; hello[pos++] = 't'; hello[pos++] = 'p';
        hello[pos++] = '/'; hello[pos++] = '1'; hello[pos++] = '.'; hello[pos++] = '1';
    }

    // --- Extension: status_request (type 5): OCSP ---
    hello[pos++] = 0x00;
    hello[pos++] = 0x05;
    hello[pos++] = 0x00;
    hello[pos++] = 0x05;  // ext data length = 5
    hello[pos++] = 0x01;  // status type: OCSP
    hello[pos++] = 0x00;  // responder id list length (0)
    hello[pos++] = 0x00;
    hello[pos++] = 0x00;  // request extensions length (0)
    hello[pos++] = 0x00;

    // --- Extension: signature_algorithms (type 13) ---
    {
        static const uint16_t sigalgs[] = {
            0x0403, 0x0804, 0x0401,
            0x0503, 0x0805, 0x0501,
            0x0806, 0x0601,
        };
        int nsig = (int)(sizeof(sigalgs) / sizeof(sigalgs[0]));
        int list_len = nsig * 2;
        int ext_data_len = list_len + 2;
        hello[pos++] = 0x00;
        hello[pos++] = 0x0d;
        hello[pos++] = (ext_data_len >> 8) & 0xff;
        hello[pos++] = ext_data_len & 0xff;
        hello[pos++] = (list_len >> 8) & 0xff;
        hello[pos++] = list_len & 0xff;
        for (int i = 0; i < nsig; i++) {
            hello[pos++] = (sigalgs[i] >> 8) & 0xff;
            hello[pos++] = sigalgs[i] & 0xff;
        }
    }

    // --- Extension: signed_certificate_timestamp (type 18, empty) ---
    hello[pos++] = 0x00;
    hello[pos++] = 0x12;
    hello[pos++] = 0x00;
    hello[pos++] = 0x00;

    // --- Extension: key_share (type 51): GREASE (1-byte payload) + x25519 ---
    {
        // entries: GREASE(group 2 + len 2 + 1 payload = 5) + x25519(2+2+32 = 36)
        int client_shares_len = 5 + 36;
        int ext_data_len = client_shares_len + 2;
        hello[pos++] = 0x00;
        hello[pos++] = 0x33;
        hello[pos++] = (ext_data_len >> 8) & 0xff;
        hello[pos++] = ext_data_len & 0xff;
        hello[pos++] = (client_shares_len >> 8) & 0xff;
        hello[pos++] = client_shares_len & 0xff;
        // GREASE key share entry (1-byte payload)
        hello[pos++] = (GREASE >> 8) & 0xff;
        hello[pos++] = GREASE & 0xff;
        hello[pos++] = 0x00;
        hello[pos++] = 0x01;  // key length: 1
        hello[pos++] = 0x00;  // payload
        // x25519 key share entry
        hello[pos++] = 0x00;
        hello[pos++] = 0x1d;
        hello[pos++] = 0x00;
        hello[pos++] = 0x20;  // 32
        memcpy(hello + pos, ctx->tls13_pubkey, 32);
        pos += 32;
    }

    // --- Extension: psk_key_exchange_modes (type 45): psk_dhe_ke ---
    hello[pos++] = 0x00;
    hello[pos++] = 0x2d;
    hello[pos++] = 0x00;
    hello[pos++] = 0x02;  // ext data length = 2
    hello[pos++] = 0x01;  // modes length = 1
    hello[pos++] = 0x01;  // psk_dhe_ke

    // --- Extension: supported_versions (type 43): GREASE, TLS1.3, TLS1.2 ---
    hello[pos++] = 0x00;
    hello[pos++] = 0x2b;
    hello[pos++] = 0x00;
    hello[pos++] = 0x07;  // ext data length = 7
    hello[pos++] = 0x06;  // list length = 6
    hello[pos++] = (GREASE >> 8) & 0xff;
    hello[pos++] = GREASE & 0xff;
    hello[pos++] = 0x03;
    hello[pos++] = 0x04;  // TLS 1.3
    hello[pos++] = 0x03;
    hello[pos++] = 0x03;  // TLS 1.2

    // #fix-tls-certverify: the compress_certificate extension (type 27) was
    // REMOVED here. Advertising it made compression-capable servers (e.g.
    // Cloudflare, which fronts api.coingecko.com) reply with a
    // "CompressedCertificate" handshake message (type 25) instead of a plain
    // Certificate (type 11). This client never implemented decompression, so
    // that message hit the `default: kprintf("Unknown handshake type")`
    // branch in tls13_process_handshake() and was silently skipped - which
    // meant chain verification never ran at all for any such server, and the
    // handshake completed anyway. Not advertising this extension means the
    // server falls back to the mandatory plain Certificate message, which we
    // parse and verify correctly.

    // --- Extension: trailing GREASE (empty, DISTINCT codepoint from leading) ---
    hello[pos++] = (GREASE2 >> 8) & 0xff;
    hello[pos++] = GREASE2 & 0xff;
    hello[pos++] = 0x00;
    hello[pos++] = 0x00;

    // Fill in extension length
    int ext_len = pos - ext_start - 2;
    hello[ext_start] = (ext_len >> 8) & 0xff;
    hello[ext_start + 1] = ext_len & 0xff;

    // Fill in handshake header
    int hello_len = pos - 4;
    hello[0] = TLS_HANDSHAKE_CLIENT_HELLO;
    hello[1] = (hello_len >> 16) & 0xff;
    hello[2] = (hello_len >> 8) & 0xff;
    hello[3] = hello_len & 0xff;

    // Add to TLS 1.2 transcript buffer
    tls_transcript_add(ctx, hello, pos);

    // Add to TLS 1.3 running hash (both candidate hashes)
    sha256_update(&ctx->transcript_hash, hello, pos);
    sha512_update(&ctx->transcript_hash384, hello, pos);

    // Send
    int ret = tls_send_record(ctx, TLS_CONTENT_HANDSHAKE, hello, pos);
    if (ret < 0) return ret;

    ctx->state = TLS_STATE_CLIENT_HELLO_SENT;
    return 0;
}


// ============================================================================
// #502: TLS 1.2 boot-time self-test.
//
// CRITICAL - why these vectors look the way they do. The [RUST-DIFF] tls_parse
// differential was green for months while BOTH arms carried the same wrong
// constant AND the vector generator hardcoded that same constant: the test
// asserted the bug. An equivalence differential cannot find a fault both arms
// share, and a generator that derives its expectations from the code under test
// proves nothing at all.
//
// So every expected value below is the output of an INDEPENDENT implementation,
// captured offline and frozen here:
//   - the PRF / master secret / extended master secret / key block / Finished
//     answers come from scapy's TLS PRF (a separate codebase);
//   - the PSS vector is a real signature produced by the `openssl dgst -sigopt
//     rsa_padding_mode:pss` CLI, verified here against OUR modexp + OUR PSS
//     decode.
// If our key schedule ever drifts from the RFC, these FAIL. If we had generated
// them from our own PRF, they never could.
//
// A negative control is included: a corrupted signature MUST be rejected. A
// verifier that returns success unconditionally passes every positive test.
// ============================================================================
// GENERATED by scratchpad/h/gen_kat.py - answers from scapy + openssl.
// DO NOT hand-edit: these are the INDEPENDENT oracle's outputs.
static const uint8_t kat_pms[48] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,
    0x0f,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,
    0x1d,0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2a,
    0x2b,0x2c,0x2d,0x2e,0x2f,0x30,
};
static const uint8_t kat_cr[32] = {
    0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,0xac,0xad,
    0xae,0xaf,0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,
    0xac,0xad,0xae,0xaf,
};
static const uint8_t kat_sr[32] = {
    0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xbb,0xbc,0xbd,
    0xbe,0xbf,0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xbb,
    0xbc,0xbd,0xbe,0xbf,
};
static const uint8_t kat_sh256[32] = {
    0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xcb,0xcc,0xcd,
    0xce,0xcf,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xcb,
    0xcc,0xcd,0xce,0xcf,
};
static const uint8_t kat_ms256[48] = {
    0xd7,0xfb,0x1b,0x99,0x4d,0x71,0x83,0x48,0x33,0xaf,0x02,0x1f,0x63,0xc2,
    0xc1,0xfc,0x15,0xa4,0x53,0x07,0x76,0x79,0x09,0xfc,0xf9,0x0f,0xfa,0xec,
    0x8a,0xc8,0x69,0x1c,0x56,0x6e,0xbe,0xd1,0x2e,0x9b,0xce,0x33,0xb4,0x5b,
    0x0c,0x5d,0xbb,0x31,0x66,0xd9,
};
static const uint8_t kat_ms384[48] = {
    0x6a,0x4e,0x89,0xb7,0xd8,0xcd,0x7c,0x94,0xdf,0x9d,0x81,0xcf,0x2b,0xce,
    0x39,0x36,0x5a,0x8c,0xc4,0x9c,0xe2,0x66,0x16,0x3d,0xac,0xdd,0xf3,0x35,
    0xb4,0x12,0x07,0x70,0xe7,0x00,0x8e,0x02,0x9d,0x1c,0x85,0x4d,0x74,0xbd,
    0x3d,0xc4,0x95,0xd6,0x68,0xa1,
};
static const uint8_t kat_ems256[48] = {
    0x5d,0x90,0x91,0x97,0xe6,0xec,0x8a,0x1d,0x00,0x41,0xc7,0x75,0x8a,0x52,
    0xeb,0x1b,0xd0,0xc8,0x3d,0x9f,0x47,0xbd,0x7a,0xa5,0x6e,0xbd,0xc3,0x69,
    0x4d,0x5e,0xca,0xdf,0x6e,0x6a,0xb5,0x6b,0x28,0x2b,0xa3,0x16,0x3e,0x72,
    0x0a,0xd0,0x2b,0x9b,0x36,0x63,
};
static const uint8_t kat_kb256_aes128[40] = {
    0x58,0x36,0x9e,0x0b,0x47,0xa4,0x1e,0x0a,0xbf,0xb8,0x65,0x0d,0x45,0xe9,
    0x5b,0x8c,0xd9,0xf8,0xb4,0x38,0xef,0xe3,0x9e,0xbe,0x2f,0x03,0xdc,0x8c,
    0x67,0x75,0x1a,0x6f,0xdf,0xb1,0x11,0xfb,0x0f,0x98,0x58,0xef,
};
static const uint8_t kat_fin_th256[32] = {
    0xc6,0x93,0x2b,0x8b,0x68,0x9c,0x67,0x5b,0x47,0x2c,0x28,0x3f,0x59,0xa3,
    0xcb,0xfc,0xc1,0x26,0x31,0x72,0x91,0xfb,0x64,0xac,0xdf,0x75,0xf6,0x2f,
    0x1c,0x36,0x3d,0x90,
};
static const uint8_t kat_fin_client256[12] = {
    0xf6,0xf0,0x49,0x0d,0xda,0x59,0x62,0x6c,0x84,0x7e,0x5f,0x26,
};
static const uint8_t kat_fin_server256[12] = {
    0x71,0xea,0x41,0x1e,0xf3,0x54,0xe5,0x2e,0x92,0x25,0x9e,0xc3,
};
static const uint8_t kat_rsa_n[256] = {
    0xd9,0x2d,0x87,0x11,0xa5,0xa1,0x5e,0x0a,0x95,0xef,0x48,0x5e,0x1e,0x29,
    0x12,0x13,0x1d,0x18,0xbb,0x03,0x6f,0x17,0xb0,0x7e,0x78,0x1e,0x21,0xcf,
    0x74,0x08,0x8a,0x0e,0xe1,0x8b,0x78,0xf3,0xc6,0xc7,0xa1,0x2b,0xa3,0x9f,
    0x7b,0x45,0x8c,0x20,0x7b,0xba,0xb7,0x27,0xd8,0x31,0x8d,0xf2,0x77,0x68,
    0x93,0x20,0xac,0x37,0xcd,0xbe,0xa5,0xc0,0xf8,0xf1,0x74,0xc1,0xd0,0x12,
    0x13,0x86,0x82,0xa7,0x41,0xde,0xa9,0x73,0x5c,0xd4,0x9f,0x57,0x60,0xb8,
    0x35,0x61,0x04,0xdb,0xb2,0x0e,0x89,0xce,0x07,0x82,0x02,0x0f,0xb4,0xf4,
    0x00,0x2d,0x3e,0x1a,0x58,0x4b,0xc6,0x55,0x15,0x85,0x33,0xd9,0x0b,0x33,
    0x84,0x81,0xdc,0xad,0x34,0xa5,0xdd,0x79,0x0c,0xae,0xb1,0x64,0x2f,0x01,
    0x4c,0xc1,0x6a,0x92,0x14,0x52,0x54,0xe4,0xa6,0xee,0x6e,0xbf,0xf5,0xe6,
    0x57,0xec,0x41,0xd2,0xf8,0x5d,0x2e,0x9a,0xb7,0xfa,0xf4,0x00,0xd6,0x26,
    0xdd,0x03,0xaf,0x8c,0x98,0x37,0xc4,0x2f,0xbb,0xe6,0xc1,0x07,0x17,0x6e,
    0x04,0x68,0x14,0xeb,0x77,0xb1,0xbb,0x06,0x25,0x69,0x94,0x1d,0xf7,0x32,
    0xc9,0xac,0xd5,0x36,0xad,0x59,0x0d,0x31,0xec,0xd3,0x71,0xfb,0x11,0x9f,
    0x74,0xfd,0x41,0x6c,0xe0,0xce,0xe8,0x85,0x74,0x3c,0xae,0x5e,0xa6,0x29,
    0xca,0x60,0x31,0xa4,0x11,0xdb,0xe8,0xc4,0x92,0x89,0xcd,0x46,0xb7,0x1e,
    0x4c,0xb4,0x28,0x7d,0xa7,0xd0,0x54,0xc5,0xe8,0x6f,0x36,0xb9,0x37,0x05,
    0xec,0xbe,0x5f,0x4c,0x68,0xc2,0x7f,0xa2,0xe0,0x77,0x67,0x9c,0x75,0x83,
    0x97,0xa4,0x38,0x01,
};
static const uint8_t kat_rsa_e[3] = {
    0x01,0x00,0x01,
};
static const uint8_t kat_rsa_hash[32] = {
    0x58,0xfb,0xfd,0x29,0x23,0x59,0x6e,0x07,0x73,0x69,0x4b,0x4b,0x95,0x02,
    0x86,0x31,0xb3,0xc6,0x47,0x35,0x2f,0xb1,0xe4,0xa0,0x05,0xdb,0x82,0x15,
    0x47,0x1d,0x1e,0xae,
};
static const uint8_t kat_rsa_sig[256] = {
    0x65,0x9d,0x5b,0xd9,0xbc,0x8a,0xaf,0x85,0x6d,0x51,0x78,0xa5,0x01,0xe0,
    0x15,0x7d,0xe4,0x26,0x03,0xd7,0x81,0x2b,0x4a,0xf6,0xd1,0x54,0xe8,0x88,
    0x37,0x08,0xa3,0x28,0x7b,0xb8,0xdf,0xdf,0xc2,0xda,0x34,0x67,0xa9,0xcb,
    0xad,0xc4,0xb7,0xc0,0xfd,0x78,0x75,0x58,0x53,0xc2,0x82,0xa3,0x17,0xad,
    0x5e,0x5d,0x1e,0x1b,0x15,0xe5,0x0c,0x21,0xc5,0xd2,0xed,0x4e,0xad,0x02,
    0x3e,0xd0,0x85,0xb1,0x3b,0xbe,0x67,0xbc,0xa8,0x1c,0x18,0x50,0xa8,0xe8,
    0xc4,0x77,0x09,0x16,0xc0,0x8b,0x41,0x7b,0x02,0x3d,0x18,0xf8,0x52,0x55,
    0xe2,0x9b,0xe9,0xeb,0x60,0xe7,0xdb,0xd5,0x3a,0x03,0x2d,0x65,0xa9,0xcf,
    0x07,0xa7,0x9c,0x37,0x09,0xcf,0x48,0x3b,0x8b,0x83,0xe8,0xb9,0x88,0x67,
    0xbd,0xa7,0x12,0x81,0xa1,0x83,0x09,0xd4,0x4f,0x97,0xdd,0x2c,0x1c,0x04,
    0xfe,0x8f,0xd4,0xb8,0xe7,0x85,0x08,0xe5,0x3b,0x62,0x98,0xd1,0x77,0x99,
    0xa8,0xa8,0x2f,0x93,0x58,0x78,0xc1,0xe5,0xf0,0xc0,0x3f,0x4c,0xb1,0xbd,
    0x8e,0x34,0x33,0x02,0x6a,0x9a,0x1a,0x34,0x4d,0x3c,0xe1,0xc9,0x4b,0xc5,
    0x88,0xe3,0x1b,0x13,0x81,0x95,0x3c,0xd4,0xd6,0xae,0x66,0x79,0xea,0x04,
    0x01,0x50,0x66,0xe6,0x7d,0xfb,0xc3,0x66,0xb1,0x34,0xa9,0x44,0x78,0x8f,
    0xa5,0xc4,0x0e,0x6c,0x9e,0x3d,0xc9,0xa4,0xca,0xaf,0x08,0x9b,0xcc,0x15,
    0x8a,0x8c,0xe5,0x64,0x41,0x83,0x6a,0x5c,0x3e,0xc2,0x5d,0xc2,0xb8,0x45,
    0x5f,0xa5,0xad,0x3e,0xf1,0x75,0x94,0x5a,0xbe,0x1a,0x14,0xd2,0xfc,0x7e,
    0xfa,0x1b,0xed,0x54,
};


static int tls12_kat_fail;

static void tls12_kat_chk(const char *what, const uint8_t *got,
                          const uint8_t *want, size_t n) {
    if (crypto_memcmp(got, want, n) != 0) {
        tls12_kat_fail++;
        kprintf("[TLS1.2-SELFTEST] FAIL %s\n", what);
    }
}

void tls12_selftest(void) {
    tls12_kat_fail = 0;
    uint8_t out[64];

    // --- master secret, SHA-256 and SHA-384 PRF (vs scapy) ---
    if (tls12_master_secret_rs(kat_pms, sizeof(kat_pms), 0, kat_cr, kat_sr,
                               NULL, 0, 32, out) != 0) tls12_kat_fail++;
    else tls12_kat_chk("master_secret SHA-256", out, kat_ms256, 48);

    if (tls12_master_secret_rs(kat_pms, sizeof(kat_pms), 0, kat_cr, kat_sr,
                               NULL, 0, 48, out) != 0) tls12_kat_fail++;
    else tls12_kat_chk("master_secret SHA-384", out, kat_ms384, 48);

    // --- RFC 7627 extended master secret (the real 1.2 hosts all use it) ---
    if (tls12_master_secret_rs(kat_pms, sizeof(kat_pms), 1, kat_cr, kat_sr,
                               kat_sh256, 32, 32, out) != 0) tls12_kat_fail++;
    else tls12_kat_chk("extended_master_secret SHA-256", out, kat_ems256, 48);

    // --- key block: the seed order (server_random FIRST) is the classic bug ---
    {
        uint8_t ck[32], sk[32], civ[4], siv[4];
        if (tls12_key_block_rs(kat_ms256, kat_cr, kat_sr, 16, 32,
                               ck, sk, civ, siv) != 0) {
            tls12_kat_fail++;
        } else {
            uint8_t kb[2 * 16 + 8];
            memcpy(kb, ck, 16);
            memcpy(kb + 16, sk, 16);
            memcpy(kb + 32, civ, 4);
            memcpy(kb + 36, siv, 4);
            tls12_kat_chk("key_block AES-128 SHA-256", kb, kat_kb256_aes128, sizeof(kb));
        }
    }

    // --- Finished verify_data, both directions ---
    if (tls12_finished_rs(kat_ms256, 1, kat_fin_th256, 32, out) != 0) tls12_kat_fail++;
    else tls12_kat_chk("Finished client SHA-256", out, kat_fin_client256, 12);
    if (tls12_finished_rs(kat_ms256, 0, kat_fin_th256, 32, out) != 0) tls12_kat_fail++;
    else tls12_kat_chk("Finished server SHA-256", out, kat_fin_server256, 12);

    // --- downgrade sentinel (RFC 8446 4.1.3): present MUST abort, absent OK ---
    {
        uint8_t sr_dg[32];
        memset(sr_dg, 0x5a, 32);
        if (tls12_downgrade_check_rs(sr_dg, TLS_VERSION_1_2, 1) != 0) {
            tls12_kat_fail++;
            kprintf("[TLS1.2-SELFTEST] FAIL downgrade false-positive\n");
        }
        // "DOWNGRD\x01" in the last 8 bytes = a 1.3-capable server forced to 1.2
        static const uint8_t sentinel[8] =
            { 0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01 };
        memcpy(sr_dg + 24, sentinel, 8);
        if (tls12_downgrade_check_rs(sr_dg, TLS_VERSION_1_2, 1) == 0) {
            tls12_kat_fail++;
            kprintf("[TLS1.2-SELFTEST] FAIL downgrade sentinel NOT detected\n");
        }
    }

    // --- RSA-PSS against a real openssl signature, + a negative control ---
    {
        rsa_public_key_t pub;
        pub.n = (uint8_t *)kat_rsa_n;   pub.n_len = sizeof(kat_rsa_n);
        pub.e = (uint8_t *)kat_rsa_e;   pub.e_len = sizeof(kat_rsa_e);
        if (rsa_verify_pss_sha256(&pub, kat_rsa_hash, 32,
                                  kat_rsa_sig, sizeof(kat_rsa_sig)) != RSA_SUCCESS) {
            tls12_kat_fail++;
            kprintf("[TLS1.2-SELFTEST] FAIL PSS genuine signature REJECTED\n");
        }
        // NEGATIVE CONTROL: a verifier that always says yes passes everything
        // above. Corrupt one byte; it MUST now say no.
        uint8_t bad[sizeof(kat_rsa_sig)];
        memcpy(bad, kat_rsa_sig, sizeof(bad));
        bad[sizeof(bad) / 2] ^= 0x01;
        if (rsa_verify_pss_sha256(&pub, kat_rsa_hash, 32, bad, sizeof(bad)) == RSA_SUCCESS) {
            tls12_kat_fail++;
            kprintf("[TLS1.2-SELFTEST] FAIL PSS FORGED signature ACCEPTED\n");
        }
    }

    if (tls12_kat_fail == 0) {
        kprintf("[TLS1.2-SELFTEST] PASS: PRF/EMS/key-block/Finished match scapy; "
                "PSS accepts a real openssl signature and rejects a forged one; "
                "downgrade sentinel detected\n");
    } else {
        kprintf("[TLS1.2-SELFTEST] %d FAILURES - TLS 1.2 key schedule is WRONG\n",
                tls12_kat_fail);
    }
}

// ============================================================================
// #502: TLS 1.2 (RFC 5246) ECDHE handshake.
//
// The key schedule (PRF / master secret / key block / Finished), the
// ServerKeyExchange framing and the downgrade sentinel are all NEW code and
// live in Rust (rustkern.rs); what follows is the message sequencing, which is
// entangled with the existing C record layer, the kmalloc'd record buffers and
// tls_context_t. See the #502 CHANGELOG entry for the language split.
// ============================================================================

// Hash the accumulated 1.2 handshake transcript with the NEGOTIATED PRF hash
// (SHA-256 for 0xc02f, SHA-384 for 0xc030). The raw transcript bytes are what
// tls_transcript_add() accumulates, which is exactly why that buffer exists:
// the hash cannot be chosen until ServerHello names the cipher suite.
static void tls12_transcript_hash(tls_context_t *ctx, uint8_t *out) {
    if (ctx->tls12_hash_len == 48) {
        sha384(ctx->handshake_hash_data, ctx->handshake_hash_len, out);
    } else {
        sha256(ctx->handshake_hash_data, ctx->handshake_hash_len, out);
    }
}

// Process ServerKeyExchange: verify the signature, then do our half of the
// ECDHE.
//
// THE SIGNATURE CHECK HERE IS THE WHOLE POINT. The certificate chain proves who
// the server is; this signature is the ONLY thing proving that the ephemeral
// ECDHE public key in this very message came from that same server. Skip it and
// any on-path attacker substitutes their own ECDHE key, learns the premaster,
// and reads/rewrites everything while the certificate still validates perfectly.
// A 1.2 stack that negotiates but does not do this is strictly worse than no 1.2
// at all (#232).
static int tls12_process_ske(tls_context_t *ctx, const uint8_t *body,
                             uint32_t body_len) {
    tls12_ske_t ske;
    memset(&ske, 0, sizeof(ske));

    // Framing of this attacker-controlled message is the Rust seam: every
    // nested length is checked against the real buffer there.
    if (tls12_ske_parse_rs(body, body_len, &ske) != 1) {
        kprintf("[TLS1.2] Malformed ServerKeyExchange (%u bytes) - rejecting\n",
                body_len);
        return TLS_ERR_HANDSHAKE;
    }

    // The server must have picked a group we actually offered.
    if (ske.named_curve != TLS_GROUP_X25519 &&
        ske.named_curve != TLS_GROUP_SECP256R1 &&
        ske.named_curve != TLS_GROUP_SECP384R1) {
        kprintf("[TLS1.2] Server chose group 0x%04x which we never offered - "
                "rejecting\n", ske.named_curve);
        return TLS_ERR_HANDSHAKE;
    }
    ctx->tls12_group = ske.named_curve;

    // ---- Signature over client_random || server_random || ECParams ----
    if (ctx->server_cert && ctx->server_cert_len) {
        cert_x509_t *leaf = cert_parse_der(ctx->server_cert, ctx->server_cert_len);
        if (!leaf) {
            kprintf("[TLS1.2] Cannot re-parse leaf certificate - rejecting\n");
            return TLS_ERR_CERTIFICATE;
        }
        size_t sd_len = 64 + (size_t)ske.params_len;
        uint8_t *sd = kmalloc(sd_len);
        if (!sd) { cert_free(leaf); return TLS_ERR_NO_MEMORY; }
        memcpy(sd, ctx->client_random, 32);
        memcpy(sd + 32, ctx->server_random, 32);
        // The ECParams bytes EXACTLY as received. Re-encoding them from the
        // parsed fields is how this check silently starts verifying something
        // other than what the server signed.
        memcpy(sd + 64, body, ske.params_len);

        int vr = cert_verify_tls_signature(leaf, ske.sig_alg, sd, sd_len,
                                           body + ske.sig_off, ske.sig_len);
        crypto_zero(sd, sd_len);
        kfree(sd);
        cert_free(leaf);
        if (vr != CERT_SUCCESS) {
            kprintf("[TLS1.2] ServerKeyExchange signature INVALID (scheme "
                    "0x%04x, rc=%d) - ABORTING (possible MITM)\n",
                    ske.sig_alg, vr);
            return TLS_ERR_VERIFY;
        }
        kprintf("[TLS1.2] ServerKeyExchange signature OK (scheme 0x%04x, "
                "group 0x%04x)\n", ske.sig_alg, ske.named_curve);
    } else if (ctx->verify_cert) {
        kprintf("[TLS1.2] ServerKeyExchange with no server certificate - "
                "rejecting\n");
        return TLS_ERR_CERTIFICATE;
    } else {
        // Explicit opt-out (tls_set_verify(ctx,0,..)); HTTPS always sets 1.
        kprintf("[TLS1.2] verify disabled: ServerKeyExchange signature NOT "
                "checked\n");
    }

    // ---- Our ephemeral key + the ECDH shared secret (the premaster) ----
    const uint8_t *peer = body + ske.pub_off;
    uint32_t peer_len = ske.pub_len;

    if (ske.named_curve == TLS_GROUP_X25519) {
        if (peer_len != 32) {
            kprintf("[TLS1.2] x25519 peer key is %u bytes, expected 32\n", peer_len);
            return TLS_ERR_HANDSHAKE;
        }
        x25519_keypair_t kp;
        x25519_generate_keypair(&kp);
        memcpy(ctx->tls12_our_pub, kp.public_key, 32);
        ctx->tls12_our_pub_len = 32;
        x25519_shared_secret(peer, kp.private_key, ctx->tls12_pms);
        ctx->tls12_pms_len = 32;
        crypto_zero(&kp, sizeof(kp));

        // RFC 7748 6.1: an all-zero X25519 output means the peer sent a
        // small-order point. Accepting it would make the "shared secret"
        // attacker-known. Reject.
        uint8_t acc = 0;
        for (int i = 0; i < 32; i++) acc |= ctx->tls12_pms[i];
        if (acc == 0) {
            kprintf("[TLS1.2] x25519 shared secret is all-zero (small-order "
                    "peer point) - rejecting\n");
            crypto_zero(ctx->tls12_pms, sizeof(ctx->tls12_pms));
            return TLS_ERR_HANDSHAKE;
        }
    } else {
        ecdsa_curve_id_t curve = (ske.named_curve == TLS_GROUP_SECP384R1)
                                   ? ECDSA_CURVE_P384 : ECDSA_CURVE_P256;
        size_t cl = ecdh_coord_len(curve);
        uint8_t priv[48];
        size_t publen = 0;
        if (ecdh_generate_keypair(curve, priv, sizeof(priv),
                                  ctx->tls12_our_pub, sizeof(ctx->tls12_our_pub),
                                  &publen) != 0) {
            crypto_zero(priv, sizeof(priv));
            kprintf("[TLS1.2] ECDH keygen failed (group 0x%04x)\n", ske.named_curve);
            return TLS_ERR_HANDSHAKE;
        }
        ctx->tls12_our_pub_len = publen;
        // ecdh_compute_shared validates the peer point (on-curve, in range, not
        // infinity) - the invalid-curve defense.
        if (ecdh_compute_shared(curve, priv, cl, peer, peer_len,
                                ctx->tls12_pms, sizeof(ctx->tls12_pms)) != 0) {
            crypto_zero(priv, sizeof(priv));
            crypto_zero(ctx->tls12_pms, sizeof(ctx->tls12_pms));
            kprintf("[TLS1.2] ECDH failed: server point rejected (group 0x%04x, "
                    "%u bytes)\n", ske.named_curve, peer_len);
            return TLS_ERR_HANDSHAKE;
        }
        ctx->tls12_pms_len = cl;
        crypto_zero(priv, sizeof(priv));   // the private scalar never persists
    }

    ctx->tls12_got_ske = 1;
    return 0;
}

// Send ClientKeyExchange + ChangeCipherSpec + (encrypted) Finished, deriving the
// master secret and the key block along the way.
static int tls12_send_client_flight(tls_context_t *ctx) {
    // An ECDHE suite REQUIRES a ServerKeyExchange. Reaching ServerHelloDone
    // without one (or with one whose signature failed) must never proceed.
    if (!ctx->tls12_got_ske) {
        kprintf("[TLS1.2] ServerHelloDone without a verified ServerKeyExchange "
                "- rejecting\n");
        return TLS_ERR_HANDSHAKE;
    }

    // ---- ClientKeyExchange: our ECDHE public key ----
    uint8_t cke[5 + 97];
    size_t pl = ctx->tls12_our_pub_len;
    if (pl == 0 || pl > 97) return TLS_ERR_HANDSHAKE;
    cke[0] = TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE;
    cke[1] = 0;
    cke[2] = 0;
    cke[3] = (uint8_t)(1 + pl);
    cke[4] = (uint8_t)pl;
    memcpy(cke + 5, ctx->tls12_our_pub, pl);
    size_t cke_len = 5 + pl;
    tls_transcript_add(ctx, cke, cke_len);
    int ret = tls_send_record(ctx, TLS_CONTENT_HANDSHAKE, cke, cke_len);
    if (ret < 0) return ret;

    // ---- session_hash = Hash(ClientHello .. ClientKeyExchange) (RFC 7627) ----
    // This is ALSO exactly the transcript the client Finished covers, so the one
    // value serves both purposes.
    uint8_t session_hash[48];
    tls12_transcript_hash(ctx, session_hash);
    ctx->tls12_hs_len_at_cke = ctx->handshake_hash_len;

    // ---- master secret ----
    if (tls12_master_secret_rs(ctx->tls12_pms, ctx->tls12_pms_len, ctx->tls12_ems,
                               ctx->client_random, ctx->server_random,
                               session_hash, ctx->tls12_hash_len,
                               ctx->tls12_hash_len, ctx->master_secret) != 0) {
        kprintf("[TLS1.2] master secret derivation failed\n");
        return TLS_ERR_HANDSHAKE;
    }
    crypto_zero(ctx->tls12_pms, sizeof(ctx->tls12_pms));
    ctx->tls12_pms_len = 0;

    // ---- key block -> the four AEAD keys/salts ----
    if (tls12_key_block_rs(ctx->master_secret, ctx->client_random,
                           ctx->server_random, (size_t)ctx->key_size,
                           ctx->tls12_hash_len,
                           ctx->client_write_key, ctx->server_write_key,
                           ctx->client_write_iv, ctx->server_write_iv) != 0) {
        kprintf("[TLS1.2] key block derivation failed\n");
        return TLS_ERR_HANDSHAKE;
    }

    // ---- ChangeCipherSpec ----
    // Deliberately NOT added to the transcript: CCS is its own content type and
    // is not a handshake message (RFC 5246 7.4.9 covers handshake messages only).
    uint8_t ccs = 1;
    ret = tls_send_record(ctx, TLS_CONTENT_CHANGE_CIPHER, &ccs, 1);
    if (ret < 0) return ret;
    ctx->client_seq = 0;   // record sequence restarts at the cipher change

    // ---- client Finished (the first record under the new keys) ----
    uint8_t vd[12];
    if (tls12_finished_rs(ctx->master_secret, 1, session_hash,
                          ctx->tls12_hash_len, vd) != 0) {
        return TLS_ERR_HANDSHAKE;
    }
    uint8_t fin[4 + 12];
    fin[0] = TLS_HANDSHAKE_FINISHED;
    fin[1] = 0;
    fin[2] = 0;
    fin[3] = 12;
    memcpy(fin + 4, vd, 12);
    // The PLAINTEXT Finished joins the transcript (the server's Finished
    // verify_data covers it); only the wire copy is encrypted.
    tls_transcript_add(ctx, fin, sizeof(fin));
    ret = tls_send_encrypted(ctx, TLS_CONTENT_HANDSHAKE, fin, sizeof(fin));
    if (ret < 0) return ret;

    ctx->state = TLS_STATE_FINISHED_SENT;
    kprintf("[TLS1.2] Sent ClientKeyExchange + CCS + Finished\n");
    return 0;
}

// Receive [NewSessionTicket], ChangeCipherSpec, and the encrypted server
// Finished; verify verify_data.
static int tls12_recv_server_finished(tls_context_t *ctx) {
    int server_ccs = 0;

    while (1) {
        uint8_t ct;
        uint8_t *data;
        size_t len;
        int ret;

        // Before the server's CCS its records are plaintext; after it they are
        // AEAD-protected. Reading the wrong one desyncs the stream.
        if (server_ccs) ret = tls_recv_encrypted(ctx, &ct, &data, &len);
        else            ret = tls_recv_record(ctx, &ct, &data, &len);
        if (ret < 0) return ret;

        if (ct == TLS_CONTENT_ALERT) {
            if (len >= 2) {
                ctx->alert_level = data[0];
                ctx->alert_desc = data[1];
                kprintf("[TLS1.2] Alert: level=%d desc=%d\n", data[0], data[1]);
            }
            kfree(data);
            return TLS_ERR_ALERT;
        }

        if (ct == TLS_CONTENT_CHANGE_CIPHER) {
            kfree(data);
            server_ccs = 1;
            ctx->server_seq = 0;   // sequence restarts at the cipher change
            continue;
        }

        if (ct != TLS_CONTENT_HANDSHAKE) {
            kfree(data);
            continue;
        }

        uint32_t pos = 0;
        tls_hs_msg_t hm;
        while (tls_hs_next(data, (uint32_t)len, &pos, &hm) == 1) {
            const uint8_t *hb = data + hm.body_off;

            if (hm.hs_type == TLS_HANDSHAKE_FINISHED) {
                if (!server_ccs) {
                    // A Finished that never went through a CCS is unencrypted
                    // and therefore unauthenticated. Refuse it.
                    kprintf("[TLS1.2] Server Finished before ChangeCipherSpec - "
                            "rejecting\n");
                    kfree(data);
                    return TLS_ERR_HANDSHAKE;
                }
                // verify_data covers everything up to but NOT including this
                // message, so hash the transcript before appending it.
                uint8_t th[48];
                tls12_transcript_hash(ctx, th);
                uint8_t expect[12];
                if (tls12_finished_rs(ctx->master_secret, 0, th,
                                      ctx->tls12_hash_len, expect) != 0) {
                    kfree(data);
                    return TLS_ERR_HANDSHAKE;
                }
                if (hm.hs_len != 12 ||
                    crypto_memcmp(expect, hb, 12) != 0) {
                    kprintf("[TLS1.2] Server Finished verify_data MISMATCH - "
                            "aborting (tamper/downgrade)\n");
                    kfree(data);
                    return TLS_ERR_VERIFY;
                }
                kprintf("[TLS1.2] Server Finished verify_data OK\n");
                ctx->state = TLS_STATE_ESTABLISHED;
                kfree(data);
                return 0;
            }

            // Anything the server sends before its Finished (in practice
            // NewSessionTicket) IS covered by that Finished, so it must go into
            // the transcript.
            if (hm.hs_type == TLS_HANDSHAKE_NEW_SESSION_TICKET) {
                kprintf("[TLS1.2] NewSessionTicket (%u bytes)\n", hm.hs_len);
            } else {
                kprintf("[TLS1.2] handshake type %u before Finished\n", hm.hs_type);
            }
            tls_transcript_add(ctx, data + hm.body_off - 4, 4 + hm.hs_len);
        }
        kfree(data);
    }
}

// ============================================================================
// ServerHello processing (handles both TLS 1.2 and 1.3)
// ============================================================================

// Process ServerHello, including TLS 1.3 extension parsing
// full_msg points to the full handshake message (type + length + body)
// full_msg_len is the total length including the 4-byte header
static int tls_process_server_hello(tls_context_t *ctx,
                                     const uint8_t *data, size_t length,
                                     const uint8_t *full_msg, size_t full_msg_len) {
    if (length < 38) return TLS_ERR_HANDSHAKE;

    // Server version (will be 0x0303 for both TLS 1.2 and 1.3)
    ctx->version = (data[0] << 8) | data[1];
    if (ctx->version < TLS_VERSION_1_2) {
        kprintf("[TLS] Server version too old: 0x%04x\n", ctx->version);
        return TLS_ERR_HANDSHAKE;
    }

    // Server random
    memcpy(ctx->server_random, data + 2, 32);

    // Session ID
    int session_id_len = data[34];
    int pos = 35 + session_id_len;

    if (pos + 3 > (int)length) return TLS_ERR_HANDSHAKE;

    // Cipher suite
    ctx->cipher_suite = (data[pos] << 8) | data[pos + 1];
    pos += 2;

    kprintf("[TLS] ServerHello cipher: 0x%04x\n", ctx->cipher_suite);

    // Compression
    pos += 1;  // skip compression byte

    // Parse extensions (if present)
    uint8_t server_x25519_pubkey[32];
    int got_x25519_key = 0;

    if (pos + 2 <= (int)length) {
        uint16_t ext_total_len = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        int ext_end = pos + ext_total_len;
        if (ext_end > (int)length) ext_end = (int)length;

        while (pos + 4 <= ext_end) {
            uint16_t ext_type = (data[pos] << 8) | data[pos + 1];
            uint16_t ext_len = (data[pos + 2] << 8) | data[pos + 3];
            pos += 4;

            if (pos + ext_len > ext_end) break;

            if (ext_type == 43) {
                // supported_versions extension
                if (ext_len >= 2) {
                    uint16_t sv = (data[pos] << 8) | data[pos + 1];
                    if (sv == TLS_VERSION_1_3) {
                        ctx->is_tls13 = 1;
                        kprintf("[TLS] Server selected TLS 1.3\n");
                    }
                }
            } else if (ext_type == 51) {
                // key_share extension
                if (ext_len >= 36) {
                    uint16_t group = (data[pos] << 8) | data[pos + 1];
                    uint16_t key_len = (data[pos + 2] << 8) | data[pos + 3];
                    if (group == 0x001D && key_len == 32) {
                        memcpy(server_x25519_pubkey, data + pos + 4, 32);
                        got_x25519_key = 1;
                    }
                }
            } else if (ext_type == 23) {
                // #502: extended_master_secret (RFC 7627). We always advertise
                // it; the server echoing it means BOTH sides must derive the
                // master secret from session_hash instead of the randoms.
                // Ignoring this echo derives a different master secret and the
                // only symptom is an unexplained Finished mismatch. Both real
                // 1.2 targets (xkcd.com, hnrss.org) echo it.
                ctx->tls12_ems = 1;
            } else if (ext_type == 16) {
                // #502: ALPN. In TLS 1.2 the selected protocol arrives HERE, in
                // the ServerHello, not in EncryptedExtensions like 1.3. Without
                // this, a 1.2 server selecting h2 would be spoken HTTP/1.1 at.
                // (Observed: both 1.2 targets pick http/1.1, but do not rely on
                // that.) Body: u16 list_len, u8 proto_len, proto.
                if (ext_len >= 5 && data[pos + 2] == 2 &&
                    data[pos + 3] == 'h' && data[pos + 4] == '2') {
                    ctx->alpn_is_h2 = 1;
                }
            }

            pos += ext_len;
        }
    }

    if (ctx->is_tls13 && got_x25519_key) {
        ctx->tls13_cipher_suite = ctx->cipher_suite;

        // Add ServerHello to both candidate TLS 1.3 transcript hashes
        sha256_update(&ctx->transcript_hash, full_msg, full_msg_len);
        sha512_update(&ctx->transcript_hash384, full_msg, full_msg_len);

        // Select the negotiated hash length: SHA-384 for AES-256-GCM-SHA384.
        int hlen = (ctx->tls13_cipher_suite == TLS13_AES_256_GCM_SHA384) ? 48 : 32;

        // Compute X25519 shared secret
        uint8_t shared_secret[32];
        x25519_shared_secret(server_x25519_pubkey,
                             ctx->tls13_privkey,
                             shared_secret);

        // Get hello_hash = Hash(ClientHello + ServerHello) using the
        // negotiated hash function.
        uint8_t hello_hash[48];
        if (hlen == 48) {
            sha512_ctx_t tmp_hash;
            memcpy(&tmp_hash, &ctx->transcript_hash384, sizeof(sha512_ctx_t));
            sha384_final(&tmp_hash, hello_hash);
        } else {
            sha256_ctx_t tmp_hash;
            memcpy(&tmp_hash, &ctx->transcript_hash, sizeof(sha256_ctx_t));
            sha256_final(&tmp_hash, hello_hash);
        }

        // Derive handshake secrets
        tls13_key_schedule_t ks;
        memset(&ks, 0, sizeof(ks));
        tls13_derive_secrets(&ks, shared_secret, 32, hello_hash, hlen);

        // Save handshake secret for later app key derivation
        memcpy(ctx->tls13_hs_secret, ks.handshake_secret, 48);

        // Save traffic secrets for Finished message computation
        memcpy(ctx->tls13_client_hs_traffic,
               ks.client_handshake_traffic, 48);
        memcpy(ctx->tls13_server_hs_traffic,
               ks.server_handshake_traffic, 48);

        // Derive handshake encryption keys
        tls13_derive_handshake_keys(&ks, ctx->tls13_cipher_suite);

        // Install handshake keys
        memcpy(ctx->tls13_client_write_key, ks.client_write_key, 32);
        memcpy(ctx->tls13_server_write_key, ks.server_write_key, 32);
        memcpy(ctx->tls13_client_write_iv, ks.client_write_iv, 12);
        memcpy(ctx->tls13_server_write_iv, ks.server_write_iv, 12);

        ctx->tls13_hs_encrypted = 1;
        ctx->tls13_server_seq = 0;
        ctx->tls13_client_seq = 0;

        kprintf("[TLS1.3] Handshake keys derived\n");

        crypto_zero(shared_secret, 32);
        crypto_zero(&ks, sizeof(ks));
    } else if (!ctx->is_tls13) {
        // ------------------------------------------------------------------
        // #502: TLS 1.2. Reached when the server does NOT send
        // supported_versions=1.3, i.e. a genuinely 1.2-max server. This used to
        // fail closed here ("only TLS 1.3 is supported"), which killed Hacker
        // News (hnrss.org, 0xc030) and xkcd (0xc02f) outright. Negotiating
        // harder is not possible: forcing 1.3 on those hosts returns a fatal
        // protocol_version alert - they really are 1.2-max.
        // ------------------------------------------------------------------

        // DOWNGRADE PROTECTION (RFC 8446 4.1.3) FIRST, before we act on
        // anything the server said. We ALWAYS offer TLS 1.3, so a 1.3-capable
        // server that lands on 1.2 must stamp the sentinel into the last 8
        // bytes of ServerHello.random. If it is there, this 1.2 negotiation was
        // forced by something on the path, not chosen by a 1.2-only server ->
        // abort. A real 1.2-only server never sets it, so xkcd/hnrss are
        // unaffected. This is what stops "we support 1.2" from becoming "an
        // attacker can strip every connection to 1.2".
        if (tls12_downgrade_check_rs(ctx->server_random, ctx->version, 1) != 0) {
            kprintf("[TLS1.2] DOWNGRADE SENTINEL in ServerHello.random: a TLS 1.3"
                    "-capable server was forced down to 1.2 - ABORTING\n");
            return TLS_ERR_HANDSHAKE;
        }

        if (ctx->version != TLS_VERSION_1_2) {
            kprintf("[TLS1.2] Server version 0x%04x unsupported (1.2 only)\n",
                    ctx->version);
            return TLS_ERR_HANDSHAKE;
        }

        // Only the two AEAD ECDHE suites we implement. Anything else - notably
        // a static-RSA or CBC suite - is refused rather than half-supported.
        if (ctx->cipher_suite == TLS12_ECDHE_RSA_AES128_GCM_SHA256) {
            ctx->key_size = 16;
            ctx->tls12_hash_len = 32;
        } else if (ctx->cipher_suite == TLS12_ECDHE_RSA_AES256_GCM_SHA384) {
            ctx->key_size = 32;
            ctx->tls12_hash_len = 48;
        } else {
            kprintf("[TLS1.2] Server selected cipher 0x%04x - unsupported "
                    "(only 0xc02f / 0xc030) - rejecting\n", ctx->cipher_suite);
            return TLS_ERR_HANDSHAKE;
        }
        // GCM: 4-byte implicit (salt) IV half; the explicit half is per-record
        // and built by the record layer. Sequence numbers restart at each CCS.
        ctx->iv_size = 4;
        ctx->client_seq = 0;
        ctx->server_seq = 0;

        kprintf("[TLS1.2] Negotiated cipher 0x%04x (AES-%d-GCM, PRF SHA-%u, "
                "ems=%d, alpn_h2=%d)\n",
                ctx->cipher_suite, ctx->key_size * 8,
                (unsigned)(ctx->tls12_hash_len * 8), ctx->tls12_ems,
                ctx->alpn_is_h2);
    }

    ctx->state = TLS_STATE_SERVER_HELLO_RECEIVED;
    return 0;
}

// #fix-tls-certverify: removed the TLS 1.2 static-RSA plumbing that used to
// live here (a "Minimal RSA encryption placeholder" that did no real RSA
// math and just memcpy'd the PKCS#1 padding straight through as if it were
// ciphertext, plus tls_derive_keys()/tls_prf() key-block derivation for a
// key-exchange path that never sent a real encrypted premaster secret to the
// server in the first place). TLS 1.2 is now rejected at ServerHello (see
// tls_process_server_hello() above) and at ServerHelloDone (see
// tls_connect() below) instead of running this dead-end exchange.

// ============================================================================
// Public API
// ============================================================================

tls_context_t *tls_create(void) {
    tls_context_t *ctx = kzalloc(sizeof(tls_context_t));
    if (!ctx) return NULL;

    ctx->state = TLS_STATE_INIT;
    ctx->version = TLS_VERSION_1_2;
    ctx->verify_cert = 0;
    ctx->allow_self_signed = 1;

    ctx->handshake_hash_cap = 4096;
    ctx->handshake_hash_data = kmalloc(ctx->handshake_hash_cap);
    if (!ctx->handshake_hash_data) {
        kfree(ctx);
        return NULL;
    }

    ctx->recv_buffer_cap = 4096;
    ctx->recv_buffer = kmalloc(ctx->recv_buffer_cap);

    ctx->app_buffer_cap = 4096;
    ctx->app_buffer = kmalloc(ctx->app_buffer_cap);

    // Initialize TLS 1.3 transcript hash. Run both SHA-256 and SHA-384 in
    // parallel: the negotiated cipher suite (and thus the transcript hash) is
    // not known until ServerHello, but the transcript starts at ClientHello.
    sha256_init(&ctx->transcript_hash);
    sha384_init(&ctx->transcript_hash384);

    return ctx;
}

void tls_free(tls_context_t *ctx) {
    if (!ctx) return;

    crypto_zero(ctx->master_secret, sizeof(ctx->master_secret));
    crypto_zero(ctx->client_write_key, sizeof(ctx->client_write_key));
    crypto_zero(ctx->server_write_key, sizeof(ctx->server_write_key));
    crypto_zero(ctx->tls13_privkey, sizeof(ctx->tls13_privkey));
    crypto_zero(ctx->tls13_client_write_key, sizeof(ctx->tls13_client_write_key));
    crypto_zero(ctx->tls13_server_write_key, sizeof(ctx->tls13_server_write_key));
    crypto_zero(ctx->tls13_hs_secret, sizeof(ctx->tls13_hs_secret));

    if (ctx->handshake_hash_data) kfree(ctx->handshake_hash_data);
    if (ctx->recv_buffer) kfree(ctx->recv_buffer);
    if (ctx->app_buffer) kfree(ctx->app_buffer);
    if (ctx->server_cert) kfree(ctx->server_cert);
    // #510: normally freed the moment CertificateVerify is checked; this covers
    // every path that aborts between Certificate and CertificateVerify (which
    // is most of the new failure paths above, and is exactly the case an
    // attacker can drive at will by hanging up after the chain).
    if (ctx->tls13_leaf) cert_free(ctx->tls13_leaf);

    kfree(ctx);
}

void tls_set_io(tls_context_t *ctx, tls_send_func send_func,
                tls_recv_func recv_func, void *user_data) {
    ctx->send_func = send_func;
    ctx->recv_func = recv_func;
    ctx->user_data = user_data;
}

void tls_set_hostname(tls_context_t *ctx, const char *hostname) {
    strncpy(ctx->hostname, hostname, sizeof(ctx->hostname) - 1);
    ctx->hostname[sizeof(ctx->hostname) - 1] = '\0';
}

void tls_set_verify(tls_context_t *ctx, int verify, int allow_self_signed) {
    ctx->verify_cert = verify;
    ctx->allow_self_signed = allow_self_signed;
}

int tls_connect(tls_context_t *ctx) {
    int ret;

    tls_set_deadline(ctx, 20);   // #277/#525: bound the handshake in REAL time
    kprintf("[TLS] Starting handshake with %s\n", ctx->hostname);

    // Send ClientHello (TLS 1.3 compatible)
    ret = tls_send_client_hello(ctx);
    if (ret < 0) return ret;

    // Receive and process server messages
    while (ctx->state != TLS_STATE_ESTABLISHED && ctx->state != TLS_STATE_ERROR) {
        uint8_t content_type;
        uint8_t *data;
        size_t length;

        ret = tls_recv_record(ctx, &content_type, &data, &length);
        if (ret < 0) return ret;

        if (content_type == TLS_CONTENT_ALERT) {
            ctx->alert_level = data[0];
            ctx->alert_desc = data[1];
            kprintf("[TLS] Alert: level=%d desc=%d\n",
                    ctx->alert_level, ctx->alert_desc);
            kfree(data);
            return TLS_ERR_ALERT;
        }

        // Skip ChangeCipherSpec (TLS 1.3 middlebox compat)
        if (content_type == TLS_CONTENT_CHANGE_CIPHER) {
            kfree(data);
            continue;
        }

        if (content_type != TLS_CONTENT_HANDSHAKE) {
            kfree(data);
            continue;
        }

        // Add to TLS 1.2 transcript
        tls_transcript_add(ctx, data, length);

        // Process handshake messages. The 1-byte type + 3-byte length header
        // read and its bound are the Rust framing seam (tls_hs_next, #404/#502).
        // The ORIGINAL inline loop here was `while (pos < length)`, which guarded
        // only data[pos] yet read data[pos+1..pos+3] and passed an UNBOUNDED
        // hs_len to tls_process_server_hello(): a remote/MITM server's crafted
        // ServerHello flight over-read the record heap allocation (see #503).
        // tls_hs_next confines BOTH the header read and the body extent, and
        // because hs_len is now guaranteed <= the remaining record bytes, the
        // ServerHello parse below can no longer read past the buffer either.
        uint32_t pos = 0;
        tls_hs_msg_t hm;
        while (tls_hs_next(data, (uint32_t)length, &pos, &hm) == 1) {
            uint8_t hs_type = hm.hs_type;
            uint32_t hs_len = hm.hs_len;
            const uint8_t *hbody = data + hm.body_off;  // body (== old data+pos)

            switch (hs_type) {
                case TLS_HANDSHAKE_SERVER_HELLO:
                    ret = tls_process_server_hello(ctx, hbody, hs_len,
                                                   hbody - 4,
                                                   4 + hs_len);
                    if (ret < 0) {
                        kfree(data);
                        return ret;
                    }

                    if (ctx->is_tls13) {
                        kfree(data);
                        // TLS 1.3: all further messages are encrypted
                        ret = tls13_process_handshake(ctx);
                        tls_clear_deadline(ctx);   // #277: TLS1.3 handshake done
                        return ret;
                    }
                    break;

                case TLS_HANDSHAKE_CERTIFICATE: {
                    kprintf("[TLS] Received server certificate\n");

                    // #fix-tls-certverify: parse + verify the chain (was:
                    // logged and discarded). Even though the TLS 1.2
                    // static-RSA key exchange below is now disabled (see
                    // TLS_STATE_SERVER_DONE_RECEIVED handling), we still
                    // validate the chain here so a bad/expired/wrong-host
                    // certificate is rejected as early as possible and shows
                    // up as a certificate error rather than a generic
                    // handshake failure.
                    cert_chain_t chain;
                    if (tls12_parse_certificate_msg(hbody, hs_len, &chain) == 0) {
                        int vret = tls_verify_chain(ctx, &chain);
                        // #502: keep a copy of the LEAF DER. The
                        // ServerKeyExchange that arrives next must have its
                        // signature checked against this certificate's public
                        // key - that is the only thing binding the ephemeral
                        // ECDHE key to the chain we just validated.
                        if (chain.count > 0 && chain.certs[0] &&
                            chain.certs[0]->raw_data && chain.certs[0]->raw_len) {
                            if (ctx->server_cert) kfree(ctx->server_cert);
                            ctx->server_cert = kmalloc(chain.certs[0]->raw_len);
                            if (ctx->server_cert) {
                                memcpy(ctx->server_cert, chain.certs[0]->raw_data,
                                       chain.certs[0]->raw_len);
                                ctx->server_cert_len = chain.certs[0]->raw_len;
                            } else {
                                ctx->server_cert_len = 0;
                            }
                        }
                        cert_chain_free(&chain);
                        if (vret != TLS_SUCCESS) {
                            kfree(data);
                            return vret;
                        }
                    } else if (ctx->verify_cert) {
                        kprintf("[TLS] Failed to parse server certificate "
                                "chain - rejecting\n");
                        kfree(data);
                        return TLS_ERR_CERTIFICATE;
                    }

                    ctx->state = TLS_STATE_CERTIFICATE_RECEIVED;
                    break;
                }

                case TLS_HANDSHAKE_SERVER_KEY_EXCHANGE:
                    // #502: ECDHE params + the signature binding them to the
                    // certificate. Fails closed on a bad signature.
                    ret = tls12_process_ske(ctx, hbody, hs_len);
                    if (ret < 0) {
                        kfree(data);
                        return ret;
                    }
                    break;

                case TLS_HANDSHAKE_CERTIFICATE_REQUEST:
                    // #502: client certificates are not implemented. Answering
                    // a CertificateRequest by ignoring it makes the server fail
                    // us at Finished anyway; say so plainly instead. No public
                    // HTTPS host we target does this.
                    kprintf("[TLS1.2] Server requested a client certificate - "
                            "unsupported, rejecting\n");
                    kfree(data);
                    return TLS_ERR_HANDSHAKE;

                case TLS_HANDSHAKE_SERVER_HELLO_DONE:
                    kprintf("[TLS] Server hello done\n");
                    ctx->state = TLS_STATE_SERVER_DONE_RECEIVED;
                    break;

                case TLS_HANDSHAKE_FINISHED:
                    // #502: a plaintext Finished is unauthenticated. The real
                    // 1.2 server Finished is encrypted and is handled (and
                    // VERIFIED) in tls12_recv_server_finished(). This used to
                    // set TLS_STATE_ESTABLISHED unconditionally, i.e. any peer
                    // could declare the handshake complete without proving
                    // anything.
                    kprintf("[TLS1.2] Unencrypted Finished - rejecting\n");
                    kfree(data);
                    return TLS_ERR_HANDSHAKE;
            }
            // tls_hs_next advanced `pos` past this whole message already.
        }

        kfree(data);

        // #502: ServerHelloDone -> run the client half of the TLS 1.2 ECDHE
        // handshake. This replaces the old static-RSA dead end, which sent 128
        // bytes of raw RNG output labeled as an "encrypted premaster secret"
        // (no RSA encryption was ever performed) while deriving the local master
        // secret from a different, never-transmitted random premaster - two
        // unrelated secrets, so it could never have interoperated, and was
        // disabled outright rather than fixed. What runs now is a real ECDHE
        // exchange: signature-verified ServerKeyExchange, real ECDH, PRF key
        // schedule, and a CHECKED Finished in both directions.
        if (ctx->state == TLS_STATE_SERVER_DONE_RECEIVED) {
            ret = tls12_send_client_flight(ctx);
            if (ret < 0) {
                ctx->state = TLS_STATE_ERROR;
                return ret;
            }
            ret = tls12_recv_server_finished(ctx);
            if (ret < 0) {
                ctx->state = TLS_STATE_ERROR;
                return ret;
            }
            tls_clear_deadline(ctx);
            kprintf("[TLS] Handshake complete (TLS 1.2, cipher 0x%04x, "
                    "group 0x%04x)\n", ctx->cipher_suite, ctx->tls12_group);
            return 0;
        }
    }

    tls_clear_deadline(ctx);   /* #277: handshake done; app-data recv has its own deadline */
    return (ctx->state == TLS_STATE_ESTABLISHED) ? 0 : TLS_ERR_HANDSHAKE;
}

int tls_send(tls_context_t *ctx, const void *data, size_t length) {
    if (ctx->state != TLS_STATE_ESTABLISHED) {
        return TLS_ERR_INVALID_PARAM;
    }

    if (ctx->is_tls13) {
        uint8_t *enc = kmalloc(length + 64);
        if (!enc) return TLS_ERR_NO_MEMORY;

        size_t enc_len;
        int ret = tls13_encrypt_record(
            ctx->tls13_client_write_key,
            ctx->tls13_client_write_iv,
            ctx->tls13_client_seq++,
            TLS_CONTENT_APPLICATION,
            data, length, enc, &enc_len,
            ctx->tls13_cipher_suite
        );
        if (ret != 0) {
            kfree(enc);
            return TLS_ERR_NETWORK;
        }

        ret = tls_send_record(ctx, TLS_CONTENT_APPLICATION, enc, enc_len);
        kfree(enc);
        return (ret < 0) ? ret : (int)length;
    }

    // TLS 1.2 path
    int ret = tls_send_encrypted(ctx, TLS_CONTENT_APPLICATION, data, length);
    return (ret < 0) ? ret : (int)length;
}

int tls_recv(tls_context_t *ctx, void *buffer, size_t length) {
    if (ctx->state != TLS_STATE_ESTABLISHED) {
        return TLS_ERR_INVALID_PARAM;
    }
    tls_set_deadline(ctx, 30);   // #277/#525: bound app-data record recv in REAL time too

    // Return buffered data first
    if (ctx->app_buffer_len > 0) {
        size_t copy = (length < ctx->app_buffer_len) ? length : ctx->app_buffer_len;
        memcpy(buffer, ctx->app_buffer, copy);
        memmove(ctx->app_buffer, ctx->app_buffer + copy,
                ctx->app_buffer_len - copy);
        ctx->app_buffer_len -= copy;
        return (int)copy;
    }

    if (ctx->is_tls13) {
        uint8_t content_type;
        uint8_t *raw;
        size_t raw_len;

        int ret = tls_recv_record(ctx, &content_type, &raw, &raw_len);
        if (ret < 0) return ret;

        // Skip ChangeCipherSpec
        if (content_type == TLS_CONTENT_CHANGE_CIPHER) {
            kfree(raw);
            return tls_recv(ctx, buffer, length);
        }

        // Handle unencrypted alerts
        if (content_type == TLS_CONTENT_ALERT) {
            if (raw_len >= 2) {
                ctx->alert_level = raw[0];
                ctx->alert_desc = raw[1];
                kfree(raw);
                if (ctx->alert_desc == TLS_ALERT_CLOSE_NOTIFY) {
                    ctx->state = TLS_STATE_CLOSED;
                    return TLS_ERR_CLOSED;
                }
                return TLS_ERR_ALERT;
            }
            kfree(raw);
            return TLS_ERR_ALERT;
        }

        if (content_type != TLS_CONTENT_APPLICATION) {
            kfree(raw);
            return 0;
        }

        // Decrypt TLS 1.3 record
        uint8_t *plain = kmalloc(raw_len);
        if (!plain) {
            kfree(raw);
            return TLS_ERR_NO_MEMORY;
        }

        size_t plain_len;
        uint8_t inner_type;
        ret = tls13_decrypt_record(
            ctx->tls13_server_write_key,
            ctx->tls13_server_write_iv,
            ctx->tls13_server_seq++,
            raw, raw_len,
            plain, &plain_len, &inner_type,
            ctx->tls13_cipher_suite
        );
        kfree(raw);

        if (ret != 0) {
            kfree(plain);
            kprintf("[TLS1.3] App data decryption failed (seq=%llu)\n",
                    ctx->tls13_server_seq - 1);
            return TLS_ERR_VERIFY;
        }
        if (g_tls_dbg) kprintf("[TLSDBG] dec ok seq=%llu inner=%u plen=%u\n",
                    ctx->tls13_server_seq - 1, inner_type, (unsigned)plain_len);

        // Handle decrypted alerts
        if (inner_type == TLS_CONTENT_ALERT) {
            if (plain_len >= 2 && plain[1] == TLS_ALERT_CLOSE_NOTIFY) {
                kfree(plain);
                ctx->state = TLS_STATE_CLOSED;
                return TLS_ERR_CLOSED;
            }
            kfree(plain);
            return TLS_ERR_ALERT;
        }

        // Skip post-handshake messages (NewSessionTicket, etc.)
        if (inner_type == TLS_CONTENT_HANDSHAKE) {
            kfree(plain);
            return tls_recv(ctx, buffer, length);
        }

        if (inner_type != TLS_CONTENT_APPLICATION) {
            kfree(plain);
            return 0;
        }

        // Copy to output buffer
        size_t copy = (length < plain_len) ? length : plain_len;
        memcpy(buffer, plain, copy);

        // Buffer remainder
        if (plain_len > copy) {
            size_t remaining = plain_len - copy;
            if (remaining > ctx->app_buffer_cap) {
                ctx->app_buffer = krealloc(ctx->app_buffer, remaining);
                ctx->app_buffer_cap = remaining;
            }
            memcpy(ctx->app_buffer, plain + copy, remaining);
            ctx->app_buffer_len = remaining;
        }

        kfree(plain);
        return (int)copy;
    }

    // TLS 1.2 path
    uint8_t content_type;
    uint8_t *data;
    size_t data_len;

    int ret = tls_recv_encrypted(ctx, &content_type, &data, &data_len);
    if (ret < 0) return ret;

    if (content_type == TLS_CONTENT_ALERT) {
        ctx->alert_level = data[0];
        ctx->alert_desc = data[1];
        kfree(data);
        if (ctx->alert_desc == TLS_ALERT_CLOSE_NOTIFY) {
            ctx->state = TLS_STATE_CLOSED;
            return TLS_ERR_CLOSED;
        }
        return TLS_ERR_ALERT;
    }

    if (content_type != TLS_CONTENT_APPLICATION) {
        kfree(data);
        return 0;
    }

    size_t copy = (length < data_len) ? length : data_len;
    memcpy(buffer, data, copy);

    if (data_len > copy) {
        size_t remaining = data_len - copy;
        if (remaining > ctx->app_buffer_cap) {
            ctx->app_buffer = krealloc(ctx->app_buffer, remaining);
            ctx->app_buffer_cap = remaining;
        }
        memcpy(ctx->app_buffer, data + copy, remaining);
        ctx->app_buffer_len = remaining;
    }

    kfree(data);
    return (int)copy;
}

int tls_close(tls_context_t *ctx) {
    if (ctx->state == TLS_STATE_ESTABLISHED) {
        uint8_t alert[2] = { TLS_ALERT_WARNING, TLS_ALERT_CLOSE_NOTIFY };

        if (ctx->is_tls13) {
            uint8_t enc[64];
            size_t enc_len;
            tls13_encrypt_record(ctx->tls13_client_write_key,
                ctx->tls13_client_write_iv,
                ctx->tls13_client_seq++,
                TLS_CONTENT_ALERT, alert, 2,
                enc, &enc_len,
                ctx->tls13_cipher_suite);
            tls_send_record(ctx, TLS_CONTENT_APPLICATION, enc, enc_len);
        } else {
            tls_send_encrypted(ctx, TLS_CONTENT_ALERT, alert, 2);
        }
    }
    ctx->state = TLS_STATE_CLOSED;
    return 0;
}

tls_state_t tls_get_state(tls_context_t *ctx) {
    return ctx->state;
}

int tls_get_error(tls_context_t *ctx) {
    return ctx->last_error;
}

int tls_is_connected(tls_context_t *ctx) {
    return ctx->state == TLS_STATE_ESTABLISHED;
}

int tls_alpn_is_h2(tls_context_t *ctx) {
    return ctx ? ctx->alpn_is_h2 : 0;
}

const char *tls_strerror(int error) {
    switch (error) {
        case TLS_SUCCESS:           return "Success";
        case TLS_ERR_NO_MEMORY:     return "Out of memory";
        case TLS_ERR_INVALID_PARAM: return "Invalid parameter";
        case TLS_ERR_HANDSHAKE:     return "Handshake failed";
        case TLS_ERR_CERTIFICATE:   return "Certificate error";
        case TLS_ERR_ALERT:         return "TLS alert received";
        case TLS_ERR_CLOSED:        return "Connection closed";
        case TLS_ERR_WOULD_BLOCK:   return "Would block";
        case TLS_ERR_TIMEOUT:       return "Timeout";
        case TLS_ERR_VERIFY:        return "Verification failed";
        case TLS_ERR_NETWORK:       return "Network error";
        default:                    return "Unknown error";
    }
}

// ============================================================================
// #404 / #502: boot-time differential + perf + security self-test for the TLS
// length-parse framing seam. Proves the Rust ports (tls_parse_record_header_rs,
// tls_hs_next_rs, tls_cert_next_rs, tls13_cert_next_rs) == their C references on
// THIS build over well-formed AND malformed record headers / coalesced
// handshake messages / certificate lists, BEFORE any HTTPS handshake runs (they
// are already LIVE under -DRUST_TLS_PARSE). Then it exercises the exact crafted
// input that over-read the ORIGINAL inline TLS 1.2 handshake loop and shows the
// seam refuses it cleanly. LIGHT (a few hundred differential vectors + ~2k-iter
// RDTSC), bounded, runs once (#426, no busy-wait). One [RUST-DIFF] tls_parse,
// one [RUST-PERF] tls_parse, one [RUST-SEC] tls_parse line to serial + /BOOTLOG.
static uint32_t tlsp_rng(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
static inline uint64_t tlsp_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Compare a full handshake-message walk (_rs vs _c) over buf[0..len). Returns 0
// if every step's return code AND (hs_type, hs_len, body_off, advanced pos) are
// identical until both terminate; 1 on any divergence.
static int tlsp_hs_walk_eq(const uint8_t *buf, uint32_t len) {
    uint32_t rp = 0, cp = 0;
    int steps = 0;
    for (;;) {
        tls_hs_msg_t rm, cm;
        int rrc = tls_hs_next_rs(buf, len, &rp, &rm);
        int crc = tls_hs_next_c(buf, len, &cp, &cm);
        if (rrc != crc) return 1;
        if (rrc != 1) return 0;
        if (rp != cp || rm.hs_type != cm.hs_type || rm.hs_len != cm.hs_len ||
            rm.body_off != cm.body_off) return 1;
        if (++steps > 256) return 0;
    }
}
static int tlsp_cert_walk_eq(const uint8_t *buf, uint32_t len, int is13) {
    uint32_t rp = 0, cp = 0;
    int steps = 0;
    for (;;) {
        tls_cert_ent_t re, ce;
        int rrc = is13 ? tls13_cert_next_rs(buf, len, &rp, &re)
                       : tls_cert_next_rs(buf, len, &rp, &re);
        int crc = is13 ? tls13_cert_next_c(buf, len, &cp, &ce)
                       : tls_cert_next_c(buf, len, &cp, &ce);
        if (rrc != crc) return 1;
        if (rrc != 1) return 0;
        if (rp != cp || re.cert_off != ce.cert_off || re.cert_len != ce.cert_len) return 1;
        if (++steps > 256) return 0;
    }
}

void tls_parse_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    static uint8_t buf[512];
    uint32_t seed = 0x7f1c9a3b;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;

    // Force-reference every Rust symbol so its archive member always links.
    { tls_record_hdr_t r; tls_hs_msg_t m; tls_cert_ent_t e; uint32_t p = 0;
      tls_parse_record_header_rs(buf, 0, &r);
      tls_hs_next_rs(buf, 0, &p, &m);
      p = 0; tls_cert_next_rs(buf, 0, &p, &e);
      p = 0; tls13_cert_next_rs(buf, 0, &p, &e); }

    // Family 1: record header (random 5B, valid header w/ in-bound+oversized
    // body length, lengths that STRADDLE the accept/reject boundary, and short
    // buffers).
    //
    // #497: these vectors used to hardcode 2^14 as the boundary (kind 1 drew
    // `% 16385` = "must accept", kind 2 drew `16385 +` = "must reject"), i.e.
    // the generator asserted the very off-by-AEAD-expansion contract that made
    // every full-size record fail (see tls.h TLS_MAX_RECORD_SIZE). It could
    // never have caught it: both arms shared the wrong constant, so both agreed
    // and the differential stayed green. Straddle the REAL boundary instead, and
    // derive it from the header constant so the two can never drift apart again.
    // Same lesson as blame.md's "the generator is part of the test".
    for (uint32_t i = 0; i < 96; i++) {
        uint32_t kind = tlsp_rng(&seed) % 5, len;
        if (kind == 0) { len = 5; for (int j = 0; j < 5; j++) buf[j] = (uint8_t)tlsp_rng(&seed); }
        else if (kind == 1) { len = 5; buf[0] = (uint8_t)(20 + tlsp_rng(&seed) % 4); buf[1] = 3; buf[2] = 3;
            uint16_t bl = (uint16_t)(tlsp_rng(&seed) % (TLS_MAX_RECORD_SIZE + 1)); buf[3] = bl >> 8; buf[4] = bl & 0xff; }
        else if (kind == 2) { len = 5; buf[0] = 23; buf[1] = 3; buf[2] = 3;
            uint16_t bl = (uint16_t)(TLS_MAX_RECORD_SIZE + 1 +
                                     tlsp_rng(&seed) % (65535u - TLS_MAX_RECORD_SIZE));
            buf[3] = bl >> 8; buf[4] = bl & 0xff; }   // > max ciphertext record
        else if (kind == 3) { // boundary: the exact accept/reject edge +/- 4
            len = 5; buf[0] = 23; buf[1] = 3; buf[2] = 3;
            uint16_t bl = (uint16_t)(TLS_MAX_RECORD_SIZE - 4 + tlsp_rng(&seed) % 9);
            buf[3] = bl >> 8; buf[4] = bl & 0xff; }
        else { len = tlsp_rng(&seed) % 5; for (uint32_t j = 0; j < len; j++) buf[j] = (uint8_t)tlsp_rng(&seed); }
        tls_record_hdr_t ro = {0}, co = {0};
        int rrc = tls_parse_record_header_rs(buf, len, &ro);
        int crc = tls_parse_record_header_c(buf, len, &co);
        int bad = (rrc != crc);
        if (!bad && rrc == TLS_SUCCESS)
            bad = (ro.content_type != co.content_type) || (ro.version != co.version) || (ro.length != co.length);
        vectors++; if (bad) { mismatches++; if (first_bad < 0) first_bad = (int)vectors; }
    }

    // Family 2: handshake-message walk. Random buffers, half seeded with valid
    // coalesced messages (so successful multi-step walks are compared), half raw
    // (truncated headers / overrunning 3-byte lengths -> both must reject alike).
    for (uint32_t i = 0; i < 128; i++) {
        uint32_t len = tlsp_rng(&seed) % 400;
        for (uint32_t j = 0; j < len; j++) buf[j] = (uint8_t)tlsp_rng(&seed);
        if ((tlsp_rng(&seed) & 1) && len >= 8) {
            uint32_t p = 0;
            while (p + 4 <= len) {
                uint32_t remain = len - (p + 4);
                uint32_t bl = remain ? (tlsp_rng(&seed) % (remain + 1)) : 0;
                buf[p] = (uint8_t)tlsp_rng(&seed);
                buf[p + 1] = (bl >> 16) & 0xff; buf[p + 2] = (bl >> 8) & 0xff; buf[p + 3] = bl & 0xff;
                p += 4 + bl;
                if ((tlsp_rng(&seed) % 3) == 0) break;
            }
        }
        vectors++;
        if (tlsp_hs_walk_eq(buf, len)) { mismatches++; if (first_bad < 0) first_bad = (int)vectors; }
    }

    // Family 3 + 4: bare (1.2) and CertificateEntry (1.3) certificate-list walks.
    for (uint32_t i = 0; i < 128; i++) {
        int is13 = (int)(tlsp_rng(&seed) & 1);
        uint32_t len = tlsp_rng(&seed) % 400;
        for (uint32_t j = 0; j < len; j++) buf[j] = (uint8_t)tlsp_rng(&seed);
        if ((tlsp_rng(&seed) & 1) && len >= 8) {
            uint32_t p = 0;
            while (p + (is13 ? 5u : 3u) <= len) {
                uint32_t remain = len - (p + 3);
                uint32_t cl = remain ? (tlsp_rng(&seed) % (remain / 2 + 1)) : 0;
                buf[p] = (cl >> 16) & 0xff; buf[p + 1] = (cl >> 8) & 0xff; buf[p + 2] = cl & 0xff;
                p += 3 + cl;
                if (is13) {
                    if (p + 2 > len) break;
                    uint32_t er = len - (p + 2);
                    uint32_t el = er ? (tlsp_rng(&seed) % (er + 1)) : 0;
                    buf[p] = (el >> 8) & 0xff; buf[p + 1] = el & 0xff;
                    p += 2 + el;
                }
                if ((tlsp_rng(&seed) % 3) == 0) break;
            }
        }
        vectors++;
        if (tlsp_cert_walk_eq(buf, len, is13)) { mismatches++; if (first_bad < 0) first_bad = (int)vectors; }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] tls_parse: %u vectors (record+hs+cert12+cert13), %u mismatches -> %s\n",
            vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] tls_parse: %u vectors, %u mismatches -> %s", vectors, mismatches, verdict);
    if (mismatches) {
        kprintf("[RUST-DIFF] tls_parse FIRST MISMATCH vector=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] tls_parse FIRST MISMATCH vector=%d", first_bad);
    }

    // [RUST-SEC]: the crafted inputs that over-read the ORIGINAL inline TLS 1.2
    // handshake loop (a handshake record body of 1..3 bytes -> the loop read
    // data[pos+1..pos+3] past kmalloc(len); and a ServerHello whose 3-byte hs_len
    // exceeds the record -> unbounded downstream read). The seam must refuse both
    // (return != 1) rather than index past the buffer. Also count C-vs-Rust
    // verdict divergences over a malformed sweep (both are hardened -> expect 0).
    {
        int confined = 1;
        for (uint32_t l = 1; l <= 3; l++) {           // 1..3-byte record bodies
            uint32_t p = 0; tls_hs_msg_t m;
            for (uint32_t j = 0; j < l; j++) buf[j] = 0xAA;
            if (tls_hs_next_rs(buf, l, &p, &m) == 1) confined = 0;
        }
        // ServerHello header with hs_len far exceeding the buffer.
        { uint32_t p = 0; tls_hs_msg_t m;
          buf[0] = TLS_HANDSHAKE_SERVER_HELLO; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0xFF;
          if (tls_hs_next_rs(buf, 4, &p, &m) == 1) confined = 0; }

        uint32_t sec_n = 0, divergences = 0, s2 = 0x1234abcd;
        for (uint32_t k = 0; k < 512; k++) {
            uint32_t len = tlsp_rng(&s2) % 40;
            for (uint32_t j = 0; j < len; j++) buf[j] = (uint8_t)tlsp_rng(&s2);
            uint32_t rp = 0, cp = 0; tls_hs_msg_t rm, cm;
            int rrc = tls_hs_next_rs(buf, len, &rp, &rm);
            int crc = tls_hs_next_c(buf, len, &cp, &cm);
            sec_n++; if ((rrc == 1) != (crc == 1)) divergences++;
        }
        kprintf("[RUST-SEC] tls_parse: REMOTE-reachable over-read in the ORIGINAL inline TLS1.2 "
                "handshake loop (guarded pos<len but read pos+1..3 + passed UNBOUNDED hs_len "
                "downstream) - REMOVED by routing through tls_hs_next; crafted-input confinement %s; "
                "%u/%u malformed hs verdicts identical (_rs vs _c), %u divergences; see task #503\n",
                confined ? "OK" : "FAIL", sec_n - divergences, sec_n, divergences);
        bootlog_write("[RUST-SEC] tls_parse: original TLS1.2 hs loop had a remote-reachable over-read; "
                      "seam confines it (crafted=%s), %u/%u malformed verdicts identical, %u div; #503",
                      confined ? "OK" : "FAIL", sec_n - divergences, sec_n, divergences);
    }

    // [RUST-PERF]: RDTSC over tls_hs_next on a fixed 3-message buffer (walk to
    // completion each iter). LIGHT: 2000 iters, warm first.
    {
        const int iters = 2000;
        // Three back-to-back messages: [type,00 00 08][8B][type,00 00 04][4B][type,00 00 00]
        uint32_t n = 0;
        buf[n++] = 8; buf[n++] = 0; buf[n++] = 0; buf[n++] = 8; for (int k = 0; k < 8; k++) buf[n++] = (uint8_t)k;
        buf[n++] = 11; buf[n++] = 0; buf[n++] = 0; buf[n++] = 4; for (int k = 0; k < 4; k++) buf[n++] = (uint8_t)k;
        buf[n++] = 20; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
        uint32_t blen = n;
        tls_hs_msg_t m; uint32_t p;
        for (int i = 0; i < 200; i++) { p = 0; while (tls_hs_next_c(buf, blen, &p, &m) == 1); p = 0; while (tls_hs_next_rs(buf, blen, &p, &m) == 1); }
        uint64_t t0 = tlsp_tsc();
        for (int i = 0; i < iters; i++) { p = 0; while (tls_hs_next_c(buf, blen, &p, &m) == 1); }
        uint64_t t1 = tlsp_tsc();
        for (int i = 0; i < iters; i++) { p = 0; while (tls_hs_next_rs(buf, blen, &p, &m) == 1); }
        uint64_t t2 = tlsp_tsc();
        uint64_t c_cyc = (t1 - t0) / iters, r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = c_cyc ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] tls_parse (hs walk, 3 msgs): C=%llu cyc RS=%llu cyc ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] tls_parse: C=%llu RS=%llu cyc/walk ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
