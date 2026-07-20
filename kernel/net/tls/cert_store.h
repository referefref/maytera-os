// cert_store.h - X.509 Certificate Store for MayteraOS
// Provides certificate storage, parsing, and chain validation
#ifndef CERT_STORE_H
#define CERT_STORE_H

#include "../../types.h"

// Certificate store errors
#define CERT_SUCCESS                 0
#define CERT_ERR_NO_MEMORY          -1
#define CERT_ERR_INVALID_FORMAT     -2
#define CERT_ERR_EXPIRED            -3
#define CERT_ERR_NOT_YET_VALID      -4
#define CERT_ERR_SIGNATURE          -5
#define CERT_ERR_NO_TRUST_ANCHOR    -6
#define CERT_ERR_CHAIN_TOO_LONG     -7
#define CERT_ERR_NAME_MISMATCH      -8
#define CERT_ERR_STORE_FULL         -9
#define CERT_ERR_NOT_FOUND          -10
#define CERT_ERR_UNSUPPORTED        -11

// Maximum sizes
#define CERT_MAX_CHAIN_DEPTH        10
#define CERT_MAX_CN_LENGTH          256
#define CERT_MAX_SAN_ENTRIES        32
// #fix-tls-certverify: bumped from 128 so the full curated CA bundle
// (/CONFIG/CACERTS.PEM, ~121 roots) fits with headroom.
#define CERT_MAX_TRUSTED_CERTS      192
#define CERT_MAX_NAME_LENGTH        64

// ============================================================================
// #404 / #502: TLS certificate_list walk framing seam (per-cert byte-range).
// The 3-byte cert_list length + per-cert 3-byte length walk is a notorious TLS
// overread surface. These steps bound each nested length to the buffer and hand
// the caller the DER byte-range only; cert_parse_der (the X.509 parse) stays C.
// Routed to Rust (rustkern.rs) under -DRUST_TLS_PARSE, else the verbatim C _c.
// ============================================================================
typedef struct {
    uint32_t cert_off;  // 0: offset of the DER cert within the walked buffer
    uint32_t cert_len;  // 4
} tls_cert_ent_t;

// Bare TLS 1.2 certificate_list step (3-byte len + DER). 1 = entry, 0 = clean
// end (< 3 header bytes remain), -1 = malformed (cert length overruns).
int tls_cert_next(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out);
int tls_cert_next_c(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out);
extern int tls_cert_next_rs(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out);

// TLS 1.3 CertificateEntry step (3-byte cert len + DER + 2-byte ext len + ext).
// Same 1/0/-1 convention; advances *pos past the per-entry extensions.
int tls13_cert_next(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out);
int tls13_cert_next_c(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out);
extern int tls13_cert_next_rs(const uint8_t *buf, uint32_t len, uint32_t *pos, tls_cert_ent_t *out);

// Certificate key types
typedef enum {
    CERT_KEY_RSA,
    CERT_KEY_ECDSA_P256,
    CERT_KEY_ECDSA_P384,
    CERT_KEY_ED25519
} cert_key_type_t;

// Certificate signature algorithms
typedef enum {
    CERT_SIG_RSA_SHA256,
    CERT_SIG_RSA_SHA384,
    CERT_SIG_RSA_SHA512,
    CERT_SIG_ECDSA_SHA256,
    CERT_SIG_ECDSA_SHA384,
    CERT_SIG_ED25519
} cert_sig_alg_t;

// Subject Alternative Name types
typedef enum {
    SAN_DNS_NAME,
    SAN_IP_ADDRESS,
    SAN_EMAIL
} san_type_t;

// Subject Alternative Name entry
typedef struct {
    san_type_t type;
    union {
        char dns_name[CERT_MAX_CN_LENGTH];
        uint8_t ip_addr[16];  // IPv4 or IPv6
        char email[CERT_MAX_CN_LENGTH];
    } value;
    int ip_len;  // 4 for IPv4, 16 for IPv6
} cert_san_t;

// Certificate validity period
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} cert_time_t;

// Distinguished Name
typedef struct {
    char common_name[CERT_MAX_CN_LENGTH];
    char organization[CERT_MAX_NAME_LENGTH];
    char organizational_unit[CERT_MAX_NAME_LENGTH];
    char country[4];
    char state[CERT_MAX_NAME_LENGTH];
    char locality[CERT_MAX_NAME_LENGTH];
} cert_name_t;

// RSA public key
typedef struct {
    uint8_t *modulus;
    size_t modulus_len;
    uint8_t *exponent;
    size_t exponent_len;
} cert_rsa_key_t;

// ECDSA public key
typedef struct {
    uint8_t *x;
    uint8_t *y;
    size_t coord_len;  // 32 for P-256, 48 for P-384
} cert_ecdsa_key_t;

// Parsed X.509 certificate
typedef struct cert_x509 {
    // Version (0 = v1, 1 = v2, 2 = v3)
    int version;
    
    // Serial number
    uint8_t *serial;
    size_t serial_len;
    
    // Issuer and subject
    cert_name_t issuer;
    cert_name_t subject;
    
    // Validity period
    cert_time_t not_before;
    cert_time_t not_after;
    
    // Public key
    cert_key_type_t key_type;
    union {
        cert_rsa_key_t rsa;
        cert_ecdsa_key_t ecdsa;
    } public_key;
    
    // Signature
    cert_sig_alg_t sig_algorithm;
    uint8_t *signature;
    size_t signature_len;
    
    // To Be Signed (TBS) data - for verification
    uint8_t *tbs_data;
    size_t tbs_len;
    
    // Extensions
    int is_ca;
    int path_length;  // -1 if not set
    int key_usage;
    int ext_key_usage;
    
    // Subject Alternative Names
    cert_san_t san[CERT_MAX_SAN_ENTRIES];
    int san_count;
    
    // Raw DER data (for caching)
    uint8_t *raw_data;
    size_t raw_len;
    
    // Computed fingerprints
    uint8_t sha256_fingerprint[32];
    
    struct cert_x509 *next;  // For chain/list
} cert_x509_t;

// Certificate chain
typedef struct {
    cert_x509_t *certs[CERT_MAX_CHAIN_DEPTH];
    int count;
} cert_chain_t;

// =============================================================================
// Certificate Store API
// =============================================================================

// Initialize certificate store
int cert_store_init(void);

// Load trusted CA certificates from PEM bundle
int cert_store_load_bundle(const char *pem_data, size_t len);

// Load trusted CA certificates from file
int cert_store_load_file(const char *path);

// Load the bundled default CA trust store (/CONFIG/CACERTS.PEM on the boot
// disk). Called once at network init (net/https.c https_init()). Logs and
// returns a negative error if the bundle is missing/unparseable - callers
// should treat that as "no CAs trusted" (verification then fails closed for
// every server, rather than silently trusting nothing being mistaken for
// trusting everything).
int cert_store_load_default_bundle(void);

// Add a trusted CA certificate (DER format)
int cert_add_trusted(const uint8_t *cert_der, size_t len);

// Add a trusted CA certificate (PEM format)
int cert_add_trusted_pem(const char *cert_pem, size_t len);

// Remove a trusted certificate by fingerprint
int cert_remove_trusted(const uint8_t fingerprint[32]);

// Get number of trusted certificates
int cert_store_count(void);

// Check if certificate is trusted
int cert_is_trusted(const cert_x509_t *cert);

// =============================================================================
// Certificate Parsing
// =============================================================================

// Parse DER-encoded certificate
cert_x509_t *cert_parse_der(const uint8_t *data, size_t len);

// Parse PEM-encoded certificate
cert_x509_t *cert_parse_pem(const char *data, size_t len);

// Parse certificate chain (DER)
int cert_parse_chain(const uint8_t *data, size_t len, cert_chain_t *chain);

// Free parsed certificate
void cert_free(cert_x509_t *cert);

// Free certificate chain
void cert_chain_free(cert_chain_t *chain);

// =============================================================================
// Certificate Validation
// =============================================================================

// Verify certificate chain
// Returns 0 on success, negative error code on failure
int cert_verify_chain(const cert_chain_t *chain, const char *hostname);

// =============================================================================
// #502: verify a TLS "digitally-signed" blob against a certificate's public key.
//
// `sig_scheme` is a TLS SignatureScheme code point (RFC 8446 4.2.3), which is
// also how TLS 1.2 encodes it on the wire for the PSS schemes. Supported (this
// list is deliberately EXACTLY the set our ClientHello advertises - a server
// answering with anything else is rejected rather than accommodated):
//   0x0401 rsa_pkcs1_sha256      0x0501 rsa_pkcs1_sha384   0x0601 rsa_pkcs1_sha512
//   0x0403 ecdsa_secp256r1_sha256  0x0503 ecdsa_secp384r1_sha384
//   0x0804 rsa_pss_rsae_sha256   0x0805 rsa_pss_rsae_sha384
// 0x0806 (rsa_pss_rsae_sha512) is advertised but NOT implemented (our PSS
// MGF1 covers SHA-256/384 only) and is REJECTED - fail closed, never silently
// accept.
//
// Used by the TLS 1.2 ServerKeyExchange check (net/tls/tls.c), which is what
// binds the ephemeral ECDHE public key to the authenticated certificate. It is
// also the right entry point for a future TLS 1.3 CertificateVerify check.
//
// Returns CERT_SUCCESS only if the signature genuinely verifies.
int cert_verify_tls_signature(const cert_x509_t *cert, uint16_t sig_scheme,
                              const uint8_t *data, size_t data_len,
                              const uint8_t *sig, size_t sig_len);

// Verify single certificate against issuer
int cert_verify_signature(const cert_x509_t *cert, const cert_x509_t *issuer);

// Check if certificate is valid for hostname
int cert_verify_hostname(const cert_x509_t *cert, const char *hostname);

// Check if certificate is currently valid (time-based)
int cert_verify_validity(const cert_x509_t *cert);

// =============================================================================
// Certificate Information
// =============================================================================

// Get certificate common name
const char *cert_get_cn(const cert_x509_t *cert);

// Get certificate fingerprint (SHA-256)
void cert_get_fingerprint(const cert_x509_t *cert, uint8_t fingerprint[32]);

// Print certificate info (debug)
void cert_print_info(const cert_x509_t *cert);

// =============================================================================
// Time Functions (for certificate validation)
// =============================================================================

// Get current time
void cert_get_current_time(cert_time_t *time);

// Compare times (-1: a < b, 0: a == b, 1: a > b)
int cert_time_compare(const cert_time_t *a, const cert_time_t *b);

#endif // CERT_STORE_H
