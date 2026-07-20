// tls.h - TLS 1.2/1.3 client for MayteraOS
// Supports TLS 1.3 (RFC 8446) with X25519 key exchange
#ifndef TLS_H
#define TLS_H

#include "../../types.h"
#include "../../crypto/crypto.h"

// TLS versions
#define TLS_VERSION_1_0     0x0301
#define TLS_VERSION_1_1     0x0302
#define TLS_VERSION_1_2     0x0303
#define TLS_VERSION_1_3     0x0304

// TLS record types
#define TLS_CONTENT_CHANGE_CIPHER   20
#define TLS_CONTENT_ALERT           21
#define TLS_CONTENT_HANDSHAKE       22
#define TLS_CONTENT_APPLICATION     23

// TLS handshake types
#define TLS_HANDSHAKE_CLIENT_HELLO      1
#define TLS_HANDSHAKE_SERVER_HELLO      2
#define TLS_HANDSHAKE_NEW_SESSION_TICKET 4
#define TLS_HANDSHAKE_ENCRYPTED_EXTENSIONS 8
#define TLS_HANDSHAKE_CERTIFICATE       11
#define TLS_HANDSHAKE_SERVER_KEY_EXCHANGE   12
#define TLS_HANDSHAKE_CERTIFICATE_REQUEST   13
#define TLS_HANDSHAKE_SERVER_HELLO_DONE     14
#define TLS_HANDSHAKE_CERTIFICATE_VERIFY    15
#define TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE   16
#define TLS_HANDSHAKE_FINISHED              20

// TLS cipher suites (1.2)
#define TLS_RSA_WITH_AES_128_GCM_SHA256     0x009C
#define TLS_RSA_WITH_AES_256_GCM_SHA384     0x009D
#define TLS_RSA_WITH_AES_128_CBC_SHA256     0x003C
#define TLS_RSA_WITH_AES_256_CBC_SHA256     0x003D

// TLS 1.2 ECDHE cipher suites we actually implement (#502). These two are what
// the real 1.2-only hosts select from our ClientHello: xkcd.com -> 0xc02f,
// hnrss.org -> 0xc030. Both are AEAD (GCM), so there are no MAC keys and no CBC
// padding oracle surface.
#define TLS12_ECDHE_RSA_AES128_GCM_SHA256   0xC02F
#define TLS12_ECDHE_RSA_AES256_GCM_SHA384   0xC030

// Named groups (RFC 8446 4.2.7 / RFC 4492). hnrss.org negotiates secp384r1 ONLY
// (probed: x25519-only and P-256-only both get a fatal alert 40 from it), which
// is why the 1.2 path must support the NIST curves and not just x25519.
#define TLS_GROUP_X25519      0x001D
#define TLS_GROUP_SECP256R1   0x0017
#define TLS_GROUP_SECP384R1   0x0018

// TLS 1.3 cipher suites
#define TLS13_CIPHER_AES_128_GCM_SHA256     0x1301
#define TLS13_CIPHER_AES_256_GCM_SHA384     0x1302
#define TLS13_CIPHER_CHACHA20_POLY1305      0x1303

// TLS alert levels
#define TLS_ALERT_WARNING   1
#define TLS_ALERT_FATAL     2

// TLS alert descriptions
#define TLS_ALERT_CLOSE_NOTIFY              0
#define TLS_ALERT_UNEXPECTED_MESSAGE        10
#define TLS_ALERT_BAD_RECORD_MAC            20
#define TLS_ALERT_HANDSHAKE_FAILURE         40
#define TLS_ALERT_BAD_CERTIFICATE           42
#define TLS_ALERT_CERTIFICATE_EXPIRED       45
#define TLS_ALERT_CERTIFICATE_UNKNOWN       46
#define TLS_ALERT_ILLEGAL_PARAMETER         47
#define TLS_ALERT_DECODE_ERROR              50
#define TLS_ALERT_DECRYPT_ERROR             51
#define TLS_ALERT_PROTOCOL_VERSION          70
#define TLS_ALERT_INTERNAL_ERROR            80

// TLS error codes
#define TLS_SUCCESS             0
#define TLS_ERR_NO_MEMORY       -1
#define TLS_ERR_INVALID_PARAM   -2
#define TLS_ERR_HANDSHAKE       -3
#define TLS_ERR_CERTIFICATE     -4
#define TLS_ERR_ALERT           -5
#define TLS_ERR_CLOSED          -6
#define TLS_ERR_WOULD_BLOCK     -7
#define TLS_ERR_TIMEOUT         -8
#define TLS_ERR_VERIFY          -9
#define TLS_ERR_NETWORK         -10

// TLS state
typedef enum {
    TLS_STATE_INIT,
    TLS_STATE_CLIENT_HELLO_SENT,
    TLS_STATE_SERVER_HELLO_RECEIVED,
    TLS_STATE_CERTIFICATE_RECEIVED,
    TLS_STATE_SERVER_DONE_RECEIVED,
    TLS_STATE_CLIENT_KEY_SENT,
    TLS_STATE_CHANGE_CIPHER_SENT,
    TLS_STATE_FINISHED_SENT,
    TLS_STATE_ESTABLISHED,
    TLS_STATE_CLOSING,
    TLS_STATE_CLOSED,
    TLS_STATE_ERROR
} tls_state_t;

// Maximum sizes
//
// #497 fault 2: TLS_MAX_RECORD_SIZE bounds the CIPHERTEXT record body (the
// 2-byte length in the 5-byte record header), NOT the plaintext. It used to be
// 2^14 = 16384, which is the PLAINTEXT limit, and tls_parse_record_header()
// rejects anything larger -> TLS_ERR_INVALID_PARAM.
//
// That is off by the AEAD expansion, so EVERY full-size record was rejected:
//   RFC 8446 (TLS 1.3) 5.1/5.2: TLSCiphertext.length <= 2^14 + 256 = 16640.
//     A full-size record is 2^14 plaintext + 1 content-type byte + 16 GCM tag
//     = 16401 > 16384 -> rejected.
//   RFC 5246 (TLS 1.2) 6.2.3:  TLSCiphertext.length <= 2^14 + 2048 = 18432.
//
// Effect: any server whose response was big enough to fill one record (i.e.
// any body over ~16 KB) had its FIRST full-size record refused, and the error
// surfaced far from its cause: the h2 client reported "done status=200 len=0"
// (headers fit in a small record, the body did not) and, for hosts whose first
// big record lands earlier, "no status (len=0)" - task #333. Small responses
// were unaffected, which is why this looked host-specific rather than
// size-specific for so long. Verified on the wire against feeds.bbci.co.uk.
//
// The bound is still a REAL bound (it caps the kmalloc in tls_recv_record from
// an attacker-declared 16-bit length); it is only corrected, not removed. The
// record header carries the legacy 0x0303 version in TLS 1.3, so the version
// cannot be used to pick the tighter 1.3 limit here; take the larger (1.2)
// value, which is what the record buffer must be able to hold anyway.
#define TLS_MAX_PLAINTEXT_SIZE  16384   // 2^14: max PLAINTEXT per record
#define TLS_MAX_RECORD_SIZE     18432   // 2^14 + 2048: max CIPHERTEXT record body
#define TLS_MAX_CERT_SIZE       8192
#define TLS_MASTER_SECRET_SIZE  48
#define TLS_MAX_KEY_SIZE        32
#define TLS_MAX_IV_SIZE         16
#define TLS_GCM_TAG_SIZE        16
#define TLS_GCM_NONCE_SIZE      12

// ============================================================================
// #404 / #502: TLS length-parse framing seam (record header + handshake walk).
// The PURE byte-framing / length-bounding of untrusted wire input, extracted so
// every attacker-influenced length field is validated in ONE audited place.
// Routed to Rust (rustkern.rs) under -DRUST_TLS_PARSE (Makefile CFLAGS), else the
// verbatim C reference (_c). The crypto / AEAD / key schedule / transcript hash
// / X.509 DER parse all stay in C: only the length arithmetic is the seam. See
// the security note in tls.c (a REMOTE-reachable over-read the routing removes;
// C-fallback hardening tracked as #503). The cert_list walk lives in
// cert_store.h.
// ============================================================================

// Parsed 5-byte TLS record header. Layout locked to 6 bytes (see _Static_assert
// in tls.c) so the Rust #[repr(C)] mirror (TlsRecordHdr) can never silently drift.
typedef struct {
    uint8_t  content_type;  // 0
    uint8_t  reserved;      // 1
    uint16_t version;       // 2: host order
    uint16_t length;        // 4: record body length (<= TLS_MAX_RECORD_SIZE)
} tls_record_hdr_t;

// One handshake message header (1-byte type + 3-byte length) plus the byte
// offset of its body within the walked buffer. Layout locked to 12 bytes.
typedef struct {
    uint8_t  hs_type;       // 0
    uint8_t  _pad[3];       // 1..3
    uint32_t hs_len;        // 4: 3-byte body length
    uint32_t body_off;      // 8: offset of the body within the buffer (== pos+4)
} tls_hs_msg_t;

// Record-header parse: 0 (TLS_SUCCESS) with *out filled, or TLS_ERR_INVALID_PARAM.
int tls_parse_record_header(const uint8_t *hdr, uint32_t len, tls_record_hdr_t *out);
int tls_parse_record_header_c(const uint8_t *hdr, uint32_t len, tls_record_hdr_t *out);
extern int tls_parse_record_header_rs(const uint8_t *hdr, uint32_t len, tls_record_hdr_t *out);

// Handshake-message walk step: 1 = message (out filled, *pos advanced past it),
// 0 = clean end (< 4 header bytes remain), -1 = malformed (length overruns len).
int tls_hs_next(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_hs_msg_t *out);
int tls_hs_next_c(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_hs_msg_t *out);
extern int tls_hs_next_rs(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_hs_msg_t *out);

// Boot-time differential + perf + security self-test (logs [RUST-DIFF] tls_parse,
// [RUST-PERF] tls_parse, [RUST-SEC] tls_parse). Runs regardless of the flag.
void tls_parse_rust_selftest(void);

// ============================================================================
// #502: TLS 1.2 (RFC 5246) ECDHE. Parsed ServerKeyExchange; layout locked to 32
// bytes by a _Static_assert in tls.c so the Rust #[repr(C)] mirror (Tls12Ske in
// rustkern.rs) can never silently drift.
// ============================================================================
typedef struct {
    uint8_t  curve_type;    // 0:  must be 3 (named_curve)
    uint8_t  _pad0;         // 1
    uint16_t named_curve;   // 2:  TLS_GROUP_*
    uint32_t pub_off;       // 4:  offset of the server's ECDHE public key
    uint32_t pub_len;       // 8
    uint16_t sig_alg;       // 12: TLS SignatureScheme
    uint16_t _pad1;         // 14
    uint32_t sig_off;       // 16: offset of the signature
    uint32_t sig_len;       // 20
    uint32_t params_len;    // 24: signed ECParams extent, from offset 0
    uint32_t _pad2;         // 28
} tls12_ske_t;

// All #502 helpers are NEW code and live in Rust (rustkern.rs). There is no C
// fallback and no -DRUST_* flag for these: a flag exists to roll a PORT back to
// its C reference, and there is no C reference for code that was never C.
// Validated offline against independent oracles (scapy's TLS PRF; the openssl
// CLI's PSS signer; real ServerKeyExchange bytes captured from xkcd.com and
// hnrss.org) - see the #502 CHANGELOG entry.
extern int tls12_ske_parse_rs(const uint8_t *buf, uint32_t len, tls12_ske_t *out);
extern int tls12_master_secret_rs(const uint8_t *pms, size_t pms_len, int ems,
                                  const uint8_t *client_random, const uint8_t *server_random,
                                  const uint8_t *session_hash, size_t session_hash_len,
                                  size_t hash_len, uint8_t *out);
extern int tls12_key_block_rs(const uint8_t *master, const uint8_t *client_random,
                              const uint8_t *server_random, size_t key_size, size_t hash_len,
                              uint8_t *ck, uint8_t *sk, uint8_t *civ, uint8_t *siv);
extern int tls12_finished_rs(const uint8_t *master, int is_client,
                             const uint8_t *transcript_hash, size_t hash_len, uint8_t *out);
extern int tls12_downgrade_check_rs(const uint8_t *server_random,
                                    uint16_t negotiated_version, int offered_13);

// #502 boot-time self-test (logs [TLS1.2-SELFTEST]). Spec-derived vectors, run
// offline, so the 1.2 key schedule cannot silently rot. See net/tls/tls.c.
void tls12_selftest(void);

// ============================================================================
// #510 / MAYTERA-SEC-2026-0017: TLS 1.3 CertificateVerify (RFC 8446 4.4.3).
// Parsed CertificateVerify; layout locked to 12 bytes by a _Static_assert in
// tls.c so the Rust #[repr(C)] mirror (Tls13Cv in rustkern.rs) cannot drift.
//
// WHY THIS MESSAGE MATTERS: it is the server's ONLY proof of POSSESSION of the
// private key for the certificate it sent. Chain verification (#232) proves the
// certificate is genuine and trusted; it does NOT prove the peer owns it, and
// certificates are public. Without this check an on-path attacker replays the
// real chain, runs its own ECDHE, and Finished still matches (Finished is keyed
// off the ECDHE, not the certificate). CHAIN != POSSESSION.
// ============================================================================
typedef struct {
    uint16_t sig_scheme;    // 0: TLS SignatureScheme from the wire
    uint16_t _pad;          // 2
    uint32_t sig_off;       // 4: offset of the signature within the CV body
    uint32_t sig_len;       // 8
} tls13_cv_t;

// NEW code, so Rust-only: no C twin and no -DRUST_* fallback (see the #502 note
// above; a fallback here would just be a second, weaker way to authenticate a
// peer). Validated against REAL CertificateVerify signatures captured from
// feeds.bbci.co.uk, lobste.rs, reddit.com, lwn.net and api.moonshot.ai and
// checked by an independent verifier (python `cryptography`) - see the #510
// CHANGELOG entry.
//
// tls13_cv_parse_rs:   1 = parsed (*out filled), -1 = malformed/short/trailing.
// tls13_cv_content_rs: returns 98 + th_len (130 or 146) bytes written, or -1.
//   `th` MUST be the NEGOTIATED CIPHER SUITE's transcript hash (32 or 48), NOT
//   the signature scheme's hash; real hosts negotiate a SHA-384 suite while
//   signing with a SHA-256 scheme.
extern int tls13_cv_parse_rs(const uint8_t *buf, uint32_t len, tls13_cv_t *out);
extern int tls13_cv_content_rs(const uint8_t *th, uint32_t th_len,
                               uint8_t *out, uint32_t out_cap);

// Forward declarations
struct tls_context;
typedef struct tls_context tls_context_t;
// #510: the context retains the leaf cert between Certificate and
// CertificateVerify. Declared incomplete here so tls.h need not pull in
// cert_store.h; net/tls/tls.c includes both.
struct cert_x509;

// Socket I/O callbacks
typedef int (*tls_send_func)(void *user_data, const void *data, size_t length);
typedef int (*tls_recv_func)(void *user_data, void *buffer, size_t length);

// TLS context structure
struct tls_context {
    tls_state_t state;

    // Socket I/O
    tls_send_func send_func;
    tls_recv_func recv_func;
    void *user_data;

    // Connection info
    char hostname[256];
    uint16_t cipher_suite;
    uint16_t version;
    // #277: handshake/record deadline. TWO fields because there are two clocks:
    //   hs_deadline_ms - #525 monotonic (cpu/mono.h) REAL-time deadline, in ms.
    //                    This is the one that is armed whenever mono_ready().
    //   hs_deadline    - legacy timer_ticks deadline, used ONLY as the fallback
    //                    when TSC calibration failed. timer_ticks counts tick
    //                    DELIVERY, not time (a KVM burst delivered 6630 ticks in
    //                    60ms of real time), so a tick deadline can expire
    //                    almost instantly under host load and fake a timeout.
    // Exactly one is non-zero at a time; 0/0 = no deadline armed. Always go
    // through tls_set_deadline / tls_deadline_expired / tls_clear_deadline.
    uint64_t hs_deadline;
    uint64_t hs_deadline_ms;

    // Random values
    uint8_t client_random[32];
    uint8_t server_random[32];

    // Session keys (TLS 1.2)
    uint8_t master_secret[TLS_MASTER_SECRET_SIZE];
    uint8_t client_write_key[TLS_MAX_KEY_SIZE];
    uint8_t server_write_key[TLS_MAX_KEY_SIZE];
    uint8_t client_write_iv[TLS_MAX_IV_SIZE];
    uint8_t server_write_iv[TLS_MAX_IV_SIZE];
    int key_size;
    int iv_size;

    // Sequence numbers (for GCM nonce, TLS 1.2)
    uint64_t client_seq;
    uint64_t server_seq;

    // Handshake transcript (for Finished message, TLS 1.2)
    uint8_t *handshake_hash_data;
    size_t handshake_hash_len;
    size_t handshake_hash_cap;

    // Receive buffer
    uint8_t *recv_buffer;
    size_t recv_buffer_len;
    size_t recv_buffer_cap;

    // Decrypted application data buffer
    uint8_t *app_buffer;
    size_t app_buffer_len;
    size_t app_buffer_cap;

    // Server certificate (for validation)
    uint8_t *server_cert;
    size_t server_cert_len;

    // Error info
    int last_error;
    uint8_t alert_level;
    uint8_t alert_desc;

    // Flags
    int verify_cert;        // Verify server certificate
    int allow_self_signed;  // Allow self-signed certificates

    // TLS 1.3 state
    int is_tls13;                        // Negotiated TLS 1.3
    uint16_t tls13_cipher_suite;         // Negotiated TLS 1.3 cipher suite
    uint8_t tls13_privkey[32];           // Our X25519 private key
    uint8_t tls13_pubkey[32];            // Our X25519 public key
    uint8_t tls13_client_hs_traffic[48]; // Client handshake traffic secret
    uint8_t tls13_server_hs_traffic[48]; // Server handshake traffic secret
    uint8_t tls13_client_app_traffic[48]; // Client app traffic secret
    uint8_t tls13_server_app_traffic[48]; // Server app traffic secret
    uint8_t tls13_client_write_key[32];  // Client write key (handshake or app)
    uint8_t tls13_server_write_key[32];  // Server write key (handshake or app)
    uint8_t tls13_client_write_iv[12];   // Client write IV
    uint8_t tls13_server_write_iv[12];   // Server write IV
    uint8_t tls13_hs_secret[48];         // Handshake secret
    sha256_ctx_t transcript_hash;        // Running SHA-256 of handshake messages
    sha512_ctx_t transcript_hash384;     // Running SHA-384 of handshake messages
                                         // (for the AES-256-GCM-SHA384 suite;
                                         //  both run in parallel since the
                                         //  negotiated hash is unknown until
                                         //  ServerHello)
    int tls13_hs_encrypted;              // Handshake encryption active
    int tls13_app_encrypted;             // Application encryption active
    uint64_t tls13_server_seq;           // Server record sequence number
    uint64_t tls13_client_seq;           // Client record sequence number

    // ------------------------------------------------------------------
    // #510 TLS 1.3 CertificateVerify state
    // ------------------------------------------------------------------
    // The leaf certificate, retained from the Certificate message ONLY so its
    // public key is still available when CertificateVerify arrives one message
    // later. Owned by the context: freed as soon as CertificateVerify has been
    // checked, and again in tls_free() for the paths that never get there.
    // (Before #510 the whole chain was freed immediately after the chain check,
    // which is why the key was gone by CV time.)
    struct cert_x509 *tls13_leaf;
    // CertificateVerify seen AND signature verified. Mirrors #502's
    // tls12_got_ske. This flag, not the per-message check, is what closes the
    // hole: a per-message check alone is trivially bypassed by an attacker who
    // simply OMITS CertificateVerify. tls_connect() refuses to finish the
    // handshake unless this is set (when verify_cert is on).
    int tls13_got_cv;

    // ------------------------------------------------------------------
    // TLS 1.2 ECDHE state (#502)
    // ------------------------------------------------------------------
    uint16_t tls12_group;         // server-selected named curve (TLS_GROUP_*)
    // Our ephemeral ECDHE public key, held only between ServerKeyExchange (where
    // it is generated) and ClientKeyExchange (where it is sent). Uncompressed
    // P-384 is the largest at 1 + 2*48 = 97 bytes. The matching PRIVATE scalar
    // is a local in tls12_process_ske() and is wiped before it returns; it is
    // deliberately NOT kept in the context.
    uint8_t  tls12_our_pub[97];
    size_t   tls12_our_pub_len;
    // Premaster secret (ECDH output): 32 bytes for x25519/P-256, 48 for P-384.
    // Wiped as soon as the master secret is derived.
    uint8_t  tls12_pms[48];
    size_t   tls12_pms_len;
    // RFC 7627 extended master secret: set when the server ECHOES ext 23. We
    // always advertise it, and both real 1.2 targets echo it, so this is the
    // normal path. Getting it wrong changes the master secret and shows up only
    // as an unexplained Finished mismatch.
    int      tls12_ems;
    size_t   tls12_hash_len;      // negotiated PRF/transcript hash: 32 or 48
    size_t   tls12_hs_len_at_cke; // transcript length after ClientKeyExchange,
                                  // i.e. the RFC 7627 session_hash boundary
    int      tls12_got_ske;       // ServerKeyExchange seen AND signature verified

    // ALPN: 1 if the server selected "h2" (HTTP/2), else 0 (HTTP/1.1).
    int alpn_is_h2;
};

// Create TLS context
tls_context_t *tls_create(void);

// Free TLS context
void tls_free(tls_context_t *ctx);

// Set I/O callbacks
void tls_set_io(tls_context_t *ctx,
                tls_send_func send_func,
                tls_recv_func recv_func,
                void *user_data);

// Set hostname for SNI (Server Name Indication)
void tls_set_hostname(tls_context_t *ctx, const char *hostname);

// Set certificate verification mode
void tls_set_verify(tls_context_t *ctx, int verify, int allow_self_signed);

// Perform TLS handshake
// Returns: 0 on success, negative error code on failure
int tls_connect(tls_context_t *ctx);

// Send encrypted data
// Returns: Number of bytes sent, or negative error code
int tls_send(tls_context_t *ctx, const void *data, size_t length);

// Receive decrypted data
// Returns: Number of bytes received, or negative error code
int tls_recv(tls_context_t *ctx, void *buffer, size_t length);

// Close TLS connection gracefully
int tls_close(tls_context_t *ctx);

// Get connection state
tls_state_t tls_get_state(tls_context_t *ctx);

// Get last error code
int tls_get_error(tls_context_t *ctx);

// Get error string
const char *tls_strerror(int error);

// Check if connection is established
int tls_is_connected(tls_context_t *ctx);

// Returns 1 if the negotiated ALPN protocol is HTTP/2 ("h2"), else 0.
int tls_alpn_is_h2(tls_context_t *ctx);

#endif // TLS_H
