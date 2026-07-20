#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// cert_store.c - X.509 Certificate Store for MayteraOS
// Provides certificate storage, parsing, and chain validation

#include "cert_store.h"
#include "../../crypto/crypto.h"
#include "../../crypto/rsa.h"
#include "../../crypto/ecdsa.h"
#include "../../string.h"
#include "../../mm/heap.h"
#include "../../serial.h"
#include "../../fs/netfs.h"   // vfs_open/vfs_read/vfs_close (#fix-tls-certverify: was
                              // implicitly declared with a bogus vfs_open(path,0) call)
#include "../../fs/fat.h"     // fat_read_file(&g_fat_fs, path, ...): the actual
                              // boot-root reader used throughout the kernel
                              // (desktop.c, editor.c, services.c, perms.c) -
                              // transparently redirects to the ext2 root when
                              // one is mounted (fs/fat.c g_root_ext2 check).
                              // net/fs/netfs.c's vfs_open() is a DIFFERENT,
                              // separate VFS that only knows about explicitly
                              // registered NFS/SMB mounts, not the boot root -
                              // it can't see /CONFIG/CACERTS.PEM at all, which
                              // is why cert_store_load_default_bundle() below
                              // uses fat_read_file() instead.

// RTC read (gui/clock.c) - used for real notBefore/notAfter validity checks
// instead of the old hardcoded "2026-01-15" placeholder clock.
extern void rtc_read_time(int *hour, int *minute, int *second);
extern void rtc_read_date(int *day, int *month, int *year, int *weekday);

// =============================================================================
// ASN.1 DER Parsing Helpers
// =============================================================================

#define ASN1_SEQUENCE       0x30
#define ASN1_SET            0x31
#define ASN1_INTEGER        0x02
#define ASN1_BIT_STRING     0x03
#define ASN1_OCTET_STRING   0x04
#define ASN1_NULL           0x05
#define ASN1_OID            0x06
#define ASN1_UTF8_STRING    0x0C
#define ASN1_PRINTABLE      0x13
#define ASN1_IA5_STRING     0x16
#define ASN1_UTC_TIME       0x17
#define ASN1_GENERALIZED_TIME 0x18
#define ASN1_CONTEXT_0      0xA0
#define ASN1_CONTEXT_3      0xA3

// ASN.1 parsing context
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} asn1_ctx_t;

// Trusted certificate store
static struct {
    cert_x509_t *certs[CERT_MAX_TRUSTED_CERTS];
    int count;
    int initialized;
} cert_store;

// Parse ASN.1 tag and length
static int asn1_get_tag_len(asn1_ctx_t *ctx, uint8_t *tag, size_t *content_len) {
    if (ctx->pos >= ctx->len) return -1;
    
    *tag = ctx->data[ctx->pos++];
    
    if (ctx->pos >= ctx->len) return -1;
    uint8_t len_byte = ctx->data[ctx->pos++];
    
    if (len_byte < 0x80) {
        *content_len = len_byte;
    } else {
        int num_bytes = len_byte & 0x7F;
        if (num_bytes > 4 || ctx->pos + num_bytes > ctx->len) return -1;
        
        *content_len = 0;
        for (int i = 0; i < num_bytes; i++) {
            *content_len = (*content_len << 8) | ctx->data[ctx->pos++];
        }
    }
    
    return 0;
}

// Skip ASN.1 element
static int asn1_skip(asn1_ctx_t *ctx) {
    uint8_t tag;
    size_t len;
    if (asn1_get_tag_len(ctx, &tag, &len) < 0) return -1;
    if (ctx->pos + len > ctx->len) return -1;
    ctx->pos += len;
    return 0;
}

// Enter ASN.1 sequence
static int asn1_enter_sequence(asn1_ctx_t *ctx, asn1_ctx_t *inner) {
    uint8_t tag;
    size_t len;
    size_t start = ctx->pos;
    
    if (asn1_get_tag_len(ctx, &tag, &len) < 0) return -1;
    if (tag != ASN1_SEQUENCE && tag != ASN1_SET) return -1;
    if (ctx->pos + len > ctx->len) return -1;
    
    inner->data = ctx->data + ctx->pos;
    inner->len = len;
    inner->pos = 0;
    
    ctx->pos += len;
    return 0;
}

// Get ASN.1 integer
static int asn1_get_integer(asn1_ctx_t *ctx, uint8_t **data, size_t *len) {
    uint8_t tag;
    if (asn1_get_tag_len(ctx, &tag, len) < 0) return -1;
    if (tag != ASN1_INTEGER) return -1;
    if (ctx->pos + *len > ctx->len) return -1;
    
    *data = (uint8_t *)(ctx->data + ctx->pos);
    ctx->pos += *len;
    return 0;
}

// Get ASN.1 bit string
static int asn1_get_bit_string(asn1_ctx_t *ctx, uint8_t **data, size_t *len) {
    uint8_t tag;
    if (asn1_get_tag_len(ctx, &tag, len) < 0) return -1;
    if (tag != ASN1_BIT_STRING) return -1;
    if (ctx->pos + *len > ctx->len || *len < 1) return -1;
    
    // Skip unused bits byte
    ctx->pos++;
    (*len)--;
    
    *data = (uint8_t *)(ctx->data + ctx->pos);
    ctx->pos += *len;
    return 0;
}

// Get ASN.1 OID
static int asn1_get_oid(asn1_ctx_t *ctx, uint8_t *oid_buf, size_t buf_len, size_t *oid_len) {
    uint8_t tag;
    if (asn1_get_tag_len(ctx, &tag, oid_len) < 0) return -1;
    if (tag != ASN1_OID) return -1;
    if (ctx->pos + *oid_len > ctx->len) return -1;
    
    if (*oid_len <= buf_len) {
        memcpy(oid_buf, ctx->data + ctx->pos, *oid_len);
    }
    ctx->pos += *oid_len;
    return 0;
}

// Get ASN.1 string (various types)
static int asn1_get_string(asn1_ctx_t *ctx, char *buf, size_t buf_len) {
    uint8_t tag;
    size_t len;
    if (asn1_get_tag_len(ctx, &tag, &len) < 0) return -1;
    if (ctx->pos + len > ctx->len) return -1;
    
    // Accept various string types
    if (tag != ASN1_UTF8_STRING && tag != ASN1_PRINTABLE && 
        tag != ASN1_IA5_STRING && tag != ASN1_OCTET_STRING) {
        return -1;
    }
    
    size_t copy_len = (len < buf_len - 1) ? len : buf_len - 1;
    memcpy(buf, ctx->data + ctx->pos, copy_len);
    buf[copy_len] = '\0';
    ctx->pos += len;
    return 0;
}

// Parse UTC time (YYMMDDhhmmssZ)
static int asn1_parse_utc_time(const uint8_t *data, size_t len, cert_time_t *time) {
    if (len < 13) return -1;
    
    int year = (data[0] - '0') * 10 + (data[1] - '0');
    time->year = (year >= 50) ? (1900 + year) : (2000 + year);
    time->month = (data[2] - '0') * 10 + (data[3] - '0');
    time->day = (data[4] - '0') * 10 + (data[5] - '0');
    time->hour = (data[6] - '0') * 10 + (data[7] - '0');
    time->minute = (data[8] - '0') * 10 + (data[9] - '0');
    time->second = (data[10] - '0') * 10 + (data[11] - '0');
    
    return 0;
}

// Parse Generalized time (YYYYMMDDhhmmssZ)
static int asn1_parse_gen_time(const uint8_t *data, size_t len, cert_time_t *time) {
    if (len < 15) return -1;
    
    time->year = (data[0] - '0') * 1000 + (data[1] - '0') * 100 +
                 (data[2] - '0') * 10 + (data[3] - '0');
    time->month = (data[4] - '0') * 10 + (data[5] - '0');
    time->day = (data[6] - '0') * 10 + (data[7] - '0');
    time->hour = (data[8] - '0') * 10 + (data[9] - '0');
    time->minute = (data[10] - '0') * 10 + (data[11] - '0');
    time->second = (data[12] - '0') * 10 + (data[13] - '0');
    
    return 0;
}

// Get ASN.1 time
static int asn1_get_time(asn1_ctx_t *ctx, cert_time_t *time) {
    uint8_t tag;
    size_t len;
    if (asn1_get_tag_len(ctx, &tag, &len) < 0) return -1;
    if (ctx->pos + len > ctx->len) return -1;
    
    int ret;
    if (tag == ASN1_UTC_TIME) {
        ret = asn1_parse_utc_time(ctx->data + ctx->pos, len, time);
    } else if (tag == ASN1_GENERALIZED_TIME) {
        ret = asn1_parse_gen_time(ctx->data + ctx->pos, len, time);
    } else {
        return -1;
    }
    
    ctx->pos += len;
    return ret;
}

// =============================================================================
// OID Definitions
// =============================================================================

// Common OIDs (DER encoded, without tag and length)
static const uint8_t OID_RSA_ENCRYPTION[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
static const uint8_t OID_SHA256_RSA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B};
static const uint8_t OID_SHA384_RSA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C};
static const uint8_t OID_SHA512_RSA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D};
static const uint8_t OID_ECDSA_SHA256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
static const uint8_t OID_ECDSA_SHA384[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03};
static const uint8_t OID_EC_PUBLIC_KEY[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
static const uint8_t OID_PRIME256V1[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
static const uint8_t OID_SECP384R1[] = {0x2B, 0x81, 0x04, 0x00, 0x22};

// Name component OIDs
static const uint8_t OID_CN[] = {0x55, 0x04, 0x03};  // Common Name
static const uint8_t OID_O[] = {0x55, 0x04, 0x0A};   // Organization
static const uint8_t OID_OU[] = {0x55, 0x04, 0x0B};  // Organizational Unit
static const uint8_t OID_C[] = {0x55, 0x04, 0x06};   // Country
static const uint8_t OID_ST[] = {0x55, 0x04, 0x08};  // State
static const uint8_t OID_L[] = {0x55, 0x04, 0x07};   // Locality

// Extension OIDs
static const uint8_t OID_BASIC_CONSTRAINTS[] = {0x55, 0x1D, 0x13};
static const uint8_t OID_KEY_USAGE[] = {0x55, 0x1D, 0x0F};
static const uint8_t OID_EXT_KEY_USAGE[] = {0x55, 0x1D, 0x25};
static const uint8_t OID_SAN[] = {0x55, 0x1D, 0x11};  // Subject Alternative Name

// Compare OIDs
static int oid_compare(const uint8_t *oid1, size_t len1, const uint8_t *oid2, size_t len2) {
    if (len1 != len2) return 0;
    return memcmp(oid1, oid2, len1) == 0;
}

// =============================================================================
// Certificate Parsing
// =============================================================================

// Parse distinguished name
static int cert_parse_name(asn1_ctx_t *ctx, cert_name_t *name) {
    memset(name, 0, sizeof(*name));
    
    asn1_ctx_t name_ctx;
    if (asn1_enter_sequence(ctx, &name_ctx) < 0) return -1;
    
    while (name_ctx.pos < name_ctx.len) {
        asn1_ctx_t set_ctx, rdn_ctx;
        
        if (asn1_enter_sequence(&name_ctx, &set_ctx) < 0) break;
        if (asn1_enter_sequence(&set_ctx, &rdn_ctx) < 0) continue;
        
        uint8_t oid[16];
        size_t oid_len;
        if (asn1_get_oid(&rdn_ctx, oid, sizeof(oid), &oid_len) < 0) continue;
        
        char value[CERT_MAX_CN_LENGTH];
        if (asn1_get_string(&rdn_ctx, value, sizeof(value)) < 0) continue;
        
        if (oid_compare(oid, oid_len, OID_CN, sizeof(OID_CN))) {
            strncpy(name->common_name, value, sizeof(name->common_name) - 1);
        } else if (oid_compare(oid, oid_len, OID_O, sizeof(OID_O))) {
            strncpy(name->organization, value, sizeof(name->organization) - 1);
        } else if (oid_compare(oid, oid_len, OID_OU, sizeof(OID_OU))) {
            strncpy(name->organizational_unit, value, sizeof(name->organizational_unit) - 1);
        } else if (oid_compare(oid, oid_len, OID_C, sizeof(OID_C))) {
            strncpy(name->country, value, sizeof(name->country) - 1);
        } else if (oid_compare(oid, oid_len, OID_ST, sizeof(OID_ST))) {
            strncpy(name->state, value, sizeof(name->state) - 1);
        } else if (oid_compare(oid, oid_len, OID_L, sizeof(OID_L))) {
            strncpy(name->locality, value, sizeof(name->locality) - 1);
        }
    }
    
    return 0;
}

// Parse RSA public key
static int cert_parse_rsa_key(asn1_ctx_t *ctx, cert_rsa_key_t *key) {
    asn1_ctx_t seq_ctx;
    if (asn1_enter_sequence(ctx, &seq_ctx) < 0) return -1;
    
    // Modulus
    if (asn1_get_integer(&seq_ctx, &key->modulus, &key->modulus_len) < 0) return -1;
    
    // Skip leading zero if present
    if (key->modulus_len > 0 && key->modulus[0] == 0) {
        key->modulus++;
        key->modulus_len--;
    }
    
    // Exponent
    if (asn1_get_integer(&seq_ctx, &key->exponent, &key->exponent_len) < 0) return -1;
    
    return 0;
}

// Parse SubjectPublicKeyInfo
static int cert_parse_public_key(asn1_ctx_t *ctx, cert_x509_t *cert) {
    asn1_ctx_t spki_ctx;
    if (asn1_enter_sequence(ctx, &spki_ctx) < 0) return -1;
    
    // Algorithm identifier
    asn1_ctx_t alg_ctx;
    if (asn1_enter_sequence(&spki_ctx, &alg_ctx) < 0) return -1;
    
    uint8_t oid[16];
    size_t oid_len;
    if (asn1_get_oid(&alg_ctx, oid, sizeof(oid), &oid_len) < 0) return -1;
    
    // Subject public key (bit string)
    uint8_t *key_data;
    size_t key_len;
    if (asn1_get_bit_string(&spki_ctx, &key_data, &key_len) < 0) return -1;
    
    if (oid_compare(oid, oid_len, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION))) {
        cert->key_type = CERT_KEY_RSA;
        asn1_ctx_t key_ctx = { key_data, key_len, 0 };
        return cert_parse_rsa_key(&key_ctx, &cert->public_key.rsa);
    } else if (oid_compare(oid, oid_len, OID_EC_PUBLIC_KEY, sizeof(OID_EC_PUBLIC_KEY))) {
        // Get curve OID
        uint8_t curve_oid[16];
        size_t curve_len;
        if (asn1_get_oid(&alg_ctx, curve_oid, sizeof(curve_oid), &curve_len) < 0) return -1;
        
        if (oid_compare(curve_oid, curve_len, OID_PRIME256V1, sizeof(OID_PRIME256V1))) {
            cert->key_type = CERT_KEY_ECDSA_P256;
            cert->public_key.ecdsa.coord_len = 32;
        } else if (oid_compare(curve_oid, curve_len, OID_SECP384R1, sizeof(OID_SECP384R1))) {
            cert->key_type = CERT_KEY_ECDSA_P384;
            cert->public_key.ecdsa.coord_len = 48;
        } else {
            return CERT_ERR_UNSUPPORTED;
        }
        
        // EC point format: 0x04 | X | Y (uncompressed)
        if (key_len < 1 + 2 * cert->public_key.ecdsa.coord_len) return -1;
        if (key_data[0] != 0x04) return CERT_ERR_UNSUPPORTED;  // Only uncompressed
        
        cert->public_key.ecdsa.x = key_data + 1;
        cert->public_key.ecdsa.y = key_data + 1 + cert->public_key.ecdsa.coord_len;
    } else {
        return CERT_ERR_UNSUPPORTED;
    }
    
    return 0;
}

// Parse signature algorithm
static int cert_parse_sig_algorithm(asn1_ctx_t *ctx, cert_sig_alg_t *alg) {
    asn1_ctx_t alg_ctx;
    if (asn1_enter_sequence(ctx, &alg_ctx) < 0) return -1;
    
    uint8_t oid[16];
    size_t oid_len;
    if (asn1_get_oid(&alg_ctx, oid, sizeof(oid), &oid_len) < 0) return -1;
    
    if (oid_compare(oid, oid_len, OID_SHA256_RSA, sizeof(OID_SHA256_RSA))) {
        *alg = CERT_SIG_RSA_SHA256;
    } else if (oid_compare(oid, oid_len, OID_SHA384_RSA, sizeof(OID_SHA384_RSA))) {
        *alg = CERT_SIG_RSA_SHA384;
    } else if (oid_compare(oid, oid_len, OID_SHA512_RSA, sizeof(OID_SHA512_RSA))) {
        *alg = CERT_SIG_RSA_SHA512;
    } else if (oid_compare(oid, oid_len, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256))) {
        *alg = CERT_SIG_ECDSA_SHA256;
    } else if (oid_compare(oid, oid_len, OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384))) {
        *alg = CERT_SIG_ECDSA_SHA384;
    } else {
        return CERT_ERR_UNSUPPORTED;
    }
    
    return 0;
}

// Parse Subject Alternative Name extension
static int cert_parse_san(const uint8_t *data, size_t len, cert_x509_t *cert) {
    asn1_ctx_t ctx = { data, len, 0 };
    asn1_ctx_t san_seq;
    
    if (asn1_enter_sequence(&ctx, &san_seq) < 0) return -1;
    
    cert->san_count = 0;
    
    while (san_seq.pos < san_seq.len && cert->san_count < CERT_MAX_SAN_ENTRIES) {
        uint8_t tag;
        size_t entry_len;
        if (asn1_get_tag_len(&san_seq, &tag, &entry_len) < 0) break;
        if (san_seq.pos + entry_len > san_seq.len) break;
        
        cert_san_t *san = &cert->san[cert->san_count];
        
        // Context-specific tags: [2] = dNSName, [7] = iPAddress
        if (tag == 0x82) {  // dNSName
            san->type = SAN_DNS_NAME;
            size_t copy_len = (entry_len < sizeof(san->value.dns_name) - 1) 
                             ? entry_len : sizeof(san->value.dns_name) - 1;
            memcpy(san->value.dns_name, san_seq.data + san_seq.pos, copy_len);
            san->value.dns_name[copy_len] = '\0';
            cert->san_count++;
        } else if (tag == 0x87) {  // iPAddress
            san->type = SAN_IP_ADDRESS;
            san->ip_len = (entry_len == 4) ? 4 : 16;
            memcpy(san->value.ip_addr, san_seq.data + san_seq.pos, 
                   (entry_len <= 16) ? entry_len : 16);
            cert->san_count++;
        }
        
        san_seq.pos += entry_len;
    }
    
    return 0;
}

// Parse extensions
static int cert_parse_extensions(asn1_ctx_t *ctx, cert_x509_t *cert) {
    // Extensions are in context tag [3]
    if (ctx->pos >= ctx->len) return 0;
    
    uint8_t tag;
    size_t len;
    size_t saved_pos = ctx->pos;
    
    if (asn1_get_tag_len(ctx, &tag, &len) < 0) return 0;
    if (tag != ASN1_CONTEXT_3) {
        ctx->pos = saved_pos;
        return 0;
    }
    
    asn1_ctx_t ext_seq_ctx = { ctx->data + ctx->pos, len, 0 };
    asn1_ctx_t ext_list;
    
    if (asn1_enter_sequence(&ext_seq_ctx, &ext_list) < 0) return 0;
    
    while (ext_list.pos < ext_list.len) {
        asn1_ctx_t ext_ctx;
        if (asn1_enter_sequence(&ext_list, &ext_ctx) < 0) break;
        
        // Get extension OID
        uint8_t oid[16];
        size_t oid_len;
        if (asn1_get_oid(&ext_ctx, oid, sizeof(oid), &oid_len) < 0) continue;
        
        // Skip critical flag if present
        if (ext_ctx.pos < ext_ctx.len) {
            uint8_t peek_tag = ext_ctx.data[ext_ctx.pos];
            if (peek_tag == 0x01) {  // Boolean
                asn1_skip(&ext_ctx);
            }
        }
        
        // Get extension value (octet string)
        uint8_t ext_tag;
        size_t ext_len;
        if (asn1_get_tag_len(&ext_ctx, &ext_tag, &ext_len) < 0) continue;
        if (ext_tag != ASN1_OCTET_STRING) continue;
        
        const uint8_t *ext_data = ext_ctx.data + ext_ctx.pos;
        
        // Parse known extensions
        if (oid_compare(oid, oid_len, OID_BASIC_CONSTRAINTS, sizeof(OID_BASIC_CONSTRAINTS))) {
            // Basic Constraints
            asn1_ctx_t bc_ctx = { ext_data, ext_len, 0 };
            asn1_ctx_t bc_seq;
            if (asn1_enter_sequence(&bc_ctx, &bc_seq) == 0) {
                // Check for CA boolean
                if (bc_seq.pos < bc_seq.len && bc_seq.data[bc_seq.pos] == 0x01) {
                    bc_seq.pos++;
                    if (bc_seq.pos < bc_seq.len) {
                        size_t bool_len = bc_seq.data[bc_seq.pos++];
                        if (bool_len == 1 && bc_seq.pos < bc_seq.len) {
                            cert->is_ca = (bc_seq.data[bc_seq.pos] != 0);
                            bc_seq.pos++;
                        }
                    }
                }
                // Path length
                cert->path_length = -1;
                if (bc_seq.pos < bc_seq.len && bc_seq.data[bc_seq.pos] == ASN1_INTEGER) {
                    uint8_t *pl_data;
                    size_t pl_len;
                    if (asn1_get_integer(&bc_seq, &pl_data, &pl_len) == 0 && pl_len == 1) {
                        cert->path_length = pl_data[0];
                    }
                }
            }
        } else if (oid_compare(oid, oid_len, OID_SAN, sizeof(OID_SAN))) {
            cert_parse_san(ext_data, ext_len, cert);
        }
    }
    
    return 0;
}

// Parse DER-encoded certificate
cert_x509_t *cert_parse_der(const uint8_t *data, size_t len) {
    cert_x509_t *cert = kzalloc(sizeof(cert_x509_t));
    if (!cert) return NULL;
    
    cert->path_length = -1;
    
    // Store raw data for signature verification
    cert->raw_data = kmalloc(len);
    if (!cert->raw_data) {
        kfree(cert);
        return NULL;
    }
    memcpy(cert->raw_data, data, len);
    cert->raw_len = len;
    
    asn1_ctx_t ctx = { data, len, 0 };
    asn1_ctx_t cert_ctx;
    
    // Certificate SEQUENCE
    if (asn1_enter_sequence(&ctx, &cert_ctx) < 0) {
        cert_free(cert);
        return NULL;
    }
    
    // TBSCertificate
    // #fix-tls-certverify BUG: tbs_start is an offset relative to cert_ctx's
    // OWN base (cert_ctx.data, which already sits past the outer Certificate
    // SEQUENCE's tag+length header), but the old code below computed
    // `data + tbs_start` using the ORIGINAL top-level `data` pointer instead
    // of `cert_ctx.data`. tbs_len came out right (both cert_ctx.pos and
    // tbs_start are consistently relative to cert_ctx), but tbs_data pointed
    // too far EARLY by exactly the size of the outer header (2-4 bytes,
    // depending on the certificate's total DER length encoding) - i.e. it
    // captured the tail of the outer SEQUENCE header + most-but-not-all of
    // TBSCertificate, shifted. Every signature check against real-world certs
    // therefore hashed the wrong bytes and failed with CERT_ERR_SIGNATURE
    // (confirmed with a native test harness against a live DigiCert-signed
    // Yahoo Finance certificate: rsa_verify_pkcs1's own RSA math + PKCS#1
    // padding decode were byte-perfect, but the locally recomputed TBS hash
    // didn't match the hash actually embedded in the signature - only after
    // fixing this pointer base did the two hashes match).
    size_t tbs_start = cert_ctx.pos;
    asn1_ctx_t tbs_ctx;
    if (asn1_enter_sequence(&cert_ctx, &tbs_ctx) < 0) {
        cert_free(cert);
        return NULL;
    }
    cert->tbs_data = (uint8_t *)(cert_ctx.data + tbs_start);
    cert->tbs_len = cert_ctx.pos - tbs_start;

    // Version (optional, context tag [0])
    cert->version = 0;
    if (tbs_ctx.pos < tbs_ctx.len) {
        uint8_t peek = tbs_ctx.data[tbs_ctx.pos];
        if (peek == ASN1_CONTEXT_0) {
            tbs_ctx.pos++;
            uint8_t ver_len = tbs_ctx.data[tbs_ctx.pos++];
            if (ver_len >= 3 && tbs_ctx.data[tbs_ctx.pos] == ASN1_INTEGER) {
                tbs_ctx.pos += 2;  // Skip tag and length
                cert->version = tbs_ctx.data[tbs_ctx.pos++];
            }
        }
    }
    
    // Serial number
    if (asn1_get_integer(&tbs_ctx, &cert->serial, &cert->serial_len) < 0) {
        cert_free(cert);
        return NULL;
    }
    
    // Signature algorithm (in TBS)
    cert_sig_alg_t tbs_sig_alg;
    if (cert_parse_sig_algorithm(&tbs_ctx, &tbs_sig_alg) < 0) {
        cert_free(cert);
        return NULL;
    }
    
    // Issuer
    if (cert_parse_name(&tbs_ctx, &cert->issuer) < 0) {
        cert_free(cert);
        return NULL;
    }
    
    // Validity
    asn1_ctx_t validity_ctx;
    if (asn1_enter_sequence(&tbs_ctx, &validity_ctx) < 0) {
        cert_free(cert);
        return NULL;
    }
    if (asn1_get_time(&validity_ctx, &cert->not_before) < 0 ||
        asn1_get_time(&validity_ctx, &cert->not_after) < 0) {
        cert_free(cert);
        return NULL;
    }
    
    // Subject
    if (cert_parse_name(&tbs_ctx, &cert->subject) < 0) {
        cert_free(cert);
        return NULL;
    }
    
    // Subject Public Key Info
    if (cert_parse_public_key(&tbs_ctx, cert) < 0) {
        kprintf("[CERT] Failed to parse public key\n");
        cert_free(cert);
        return NULL;
    }
    
    // Extensions (v3 only)
    if (cert->version >= 2) {
        cert_parse_extensions(&tbs_ctx, cert);
    }
    
    // Signature algorithm
    if (cert_parse_sig_algorithm(&cert_ctx, &cert->sig_algorithm) < 0) {
        cert_free(cert);
        return NULL;
    }
    
    // Signature value
    if (asn1_get_bit_string(&cert_ctx, &cert->signature, &cert->signature_len) < 0) {
        cert_free(cert);
        return NULL;
    }
    
    // Compute fingerprint
    sha256(data, len, cert->sha256_fingerprint);

    // #fix-tls-certverify CRITICAL BUG: every pointer-into-the-DER-buffer
    // field set above (serial, signature, tbs_data, and the RSA modulus/
    // exponent or ECDSA x/y coordinates inside public_key) points into the
    // CALLER-OWNED `data` buffer passed to this function, NOT into
    // `cert->raw_data` (the persistent copy made a few lines up). That's only
    // safe if the caller keeps `data` alive for as long as the resulting
    // cert_x509_t is alive.
    //
    // For certs parsed live off the wire during a TLS handshake that's true
    // (the handshake-message buffer isn't freed until after verification).
    // It is NOT true for the CA trust-store bundle: cert_parse_pem() below
    // kmalloc's a temporary base64-decode buffer, calls this function, and
    // frees that buffer immediately afterward - leaving every one of these
    // fields DANGLING. In practice that meant every trusted root's public
    // key (modulus/exponent or x/y) silently turned into whatever memory
    // was allocated next, so signature verification against a trust anchor
    // either failed outright (garbage key -> garbage math -> mismatch) or,
    // worse, could occasionally "succeed" against garbage. This was found by
    // comparing a native test harness (which called cert_parse_der()
    // directly, so never hit the free) against live kernel behavior loading
    // the real CA bundle, where every chain terminating at a trust-store
    // root failed even though the exact same RSA/ECDSA verify code was
    // proven correct in isolation against hand-extracted key bytes.
    //
    // Fix: rebase every such pointer from `data` onto `raw_data` (a byte-for-
    // byte copy at the same relative offsets) before returning, so the
    // parsed certificate is fully self-contained and safe to use for its
    // entire lifetime regardless of what the caller does with `data`.
    {
        intptr_t rebase = (intptr_t)cert->raw_data - (intptr_t)data;
        if (cert->serial) cert->serial = (uint8_t *)((intptr_t)cert->serial + rebase);
        if (cert->signature) cert->signature = (uint8_t *)((intptr_t)cert->signature + rebase);
        if (cert->tbs_data) cert->tbs_data = (uint8_t *)((intptr_t)cert->tbs_data + rebase);
        if (cert->key_type == CERT_KEY_RSA) {
            if (cert->public_key.rsa.modulus)
                cert->public_key.rsa.modulus = (uint8_t *)((intptr_t)cert->public_key.rsa.modulus + rebase);
            if (cert->public_key.rsa.exponent)
                cert->public_key.rsa.exponent = (uint8_t *)((intptr_t)cert->public_key.rsa.exponent + rebase);
        } else if (cert->key_type == CERT_KEY_ECDSA_P256 || cert->key_type == CERT_KEY_ECDSA_P384) {
            if (cert->public_key.ecdsa.x)
                cert->public_key.ecdsa.x = (uint8_t *)((intptr_t)cert->public_key.ecdsa.x + rebase);
            if (cert->public_key.ecdsa.y)
                cert->public_key.ecdsa.y = (uint8_t *)((intptr_t)cert->public_key.ecdsa.y + rebase);
        }
    }

    return cert;
}

// Base64 decode for PEM
static int base64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Verbatim C reference (unchanged body). Kept as the rollback path; under
// -DRUST_CERT_B64 the dispatcher below routes to the Rust port instead.
static size_t base64_decode_c(const char *in, size_t in_len, uint8_t *out, size_t out_len) {
    size_t written = 0;
    int acc = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len && written < out_len; i++) {
        if (in[i] == '\n' || in[i] == '\r' || in[i] == ' ') continue;
        if (in[i] == '=') break;

        int val = base64_decode_char(in[i]);
        if (val < 0) continue;

        acc = (acc << 6) | val;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out[written++] = (acc >> bits) & 0xFF;
        }
    }

    return written;
}

// #404 batch-2: route the live PEM base64->DER decoder to the Rust port
// (rustkern.rs) under -DRUST_CERT_B64; C kept as base64_decode_c for trivial
// rollback (drop the flag). The boot [RUST-DIFF] cert_b64 self-test re-proves
// rs==c before TLS is used. The -----BEGIN/END----- strstr framing scan stays
// in C in cert_parse_pem (not untrusted-hot).
extern int cert_base64_decode_rs(const uint8_t *in, uint32_t in_len,
                                 uint8_t *out, uint32_t out_cap,
                                 uint32_t *out_len);

static size_t base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_len) {
#ifdef RUST_CERT_B64
    uint32_t olen = 0;
    (void)cert_base64_decode_rs((const uint8_t *)in, (uint32_t)in_len,
                                out, (uint32_t)out_len, &olen);
    return (size_t)olen;
#else
    return base64_decode_c(in, in_len, out, out_len);
#endif
}

// #404 batch-2 differential self-test + RDTSC micro-benchmark. Proves rs==c and
// round-trip on this exact build; only touches local stack buffers; runs once at
// boot before TLS is used. One [RUST-DIFF] cert_b64 line to serial + /BOOTLOG.
static inline uint64_t cert_b64_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void cert_b64_perf(const char *b64, int len) {
    uint8_t out[64]; uint32_t rl = 0; volatile uint32_t sink = 0;
    for (int i = 0; i < 200; i++) { (void)base64_decode_c(b64, len, out, sizeof(out)); }
    uint64_t t0 = cert_b64_rdtsc();
    for (int i = 0; i < 20000; i++) { sink += (uint32_t)base64_decode_c(b64, len, out, sizeof(out)); }
    uint64_t t1 = cert_b64_rdtsc();
    for (int i = 0; i < 20000; i++) { cert_base64_decode_rs((const uint8_t *)b64, len, out, sizeof(out), &rl); sink += rl; }
    uint64_t t2 = cert_b64_rdtsc();
    kprintf("[RUST-PERF] cert_b64 C=%llu RS=%llu cyc/20k (rs/c x100=%llu)\n",
            (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
            (unsigned long long)((t2 - t1) * 100 / ((t1 - t0) ? (t1 - t0) : 1)));
    (void)sink;
}

void cert_b64_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    static const uint8_t PLAIN[48] = {
        0x30,0x82,0x01,0x0a,0x02,0x82,0x01,0x01,0x00,0xd5,0xa1,0x7e,
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,
        0xcc,0xdd,0xee,0xff,0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
        0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,0x0a,0x0d,0x20,0x3d };
    static const char *B64A =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char b64[80]; int o = 0;
    for (int i = 0; i < 48; i += 3) {
        uint32_t v = (PLAIN[i] << 16) | (PLAIN[i+1] << 8) | PLAIN[i+2];
        b64[o++] = B64A[(v>>18)&63]; b64[o++] = B64A[(v>>12)&63];
        b64[o++] = B64A[(v>>6)&63];  b64[o++] = B64A[v&63];
    }
    b64[o] = 0;
    uint8_t dc[64], dr[64]; uint32_t lr = 0;
    size_t lc = base64_decode_c(b64, (size_t)o, dc, sizeof(dc));
    (void)cert_base64_decode_rs((const uint8_t *)b64, (uint32_t)o, dr, sizeof(dr), &lr);
    int ok = (lc == (size_t)lr) && (lc == 48) && (memcmp(dc, dr, lc) == 0)
             && (memcmp(dc, PLAIN, 48) == 0);
    kprintf("[RUST-DIFF] cert_b64: c_len=%u rs_len=%u %s (LIVE=%s)\n",
            (unsigned)lc, (unsigned)lr, ok ? "MATCH" : "*** MISMATCH ***",
#ifdef RUST_CERT_B64
            "rust");
#else
            "c");
#endif
    bootlog_write("[RUST-DIFF] cert_b64: c_len=%u rs_len=%u %s",
                  (unsigned)lc, (unsigned)lr, ok ? "MATCH" : "MISMATCH");
    kprintf("[RUST-SEC]  cert_b64: output bounded by out_cap by construction; "
            "Rust removes the C signed-shift UB (defense-in-depth)\n");
    cert_b64_perf(b64, o);
}

// Parse PEM-encoded certificate
cert_x509_t *cert_parse_pem(const char *data, size_t len) {
    // Find BEGIN marker
    const char *begin = "-----BEGIN CERTIFICATE-----";
    const char *end = "-----END CERTIFICATE-----";
    
    const char *start = strstr(data, begin);
    if (!start) return NULL;
    start += strlen(begin);
    
    // Skip whitespace
    while (*start == '\n' || *start == '\r') start++;
    
    const char *finish = strstr(start, end);
    if (!finish) return NULL;
    
    // Decode base64
    size_t b64_len = finish - start;
    size_t der_max = (b64_len * 3) / 4 + 4;
    uint8_t *der = kmalloc(der_max);
    if (!der) return NULL;
    
    size_t der_len = base64_decode(start, b64_len, der, der_max);
    
    cert_x509_t *cert = cert_parse_der(der, der_len);
    kfree(der);
    
    return cert;
}

// Free certificate
void cert_free(cert_x509_t *cert) {
    if (!cert) return;
    if (cert->raw_data) kfree(cert->raw_data);
    kfree(cert);
}

// =============================================================================
// Certificate Store
// =============================================================================

int cert_store_init(void) {
    memset(&cert_store, 0, sizeof(cert_store));
    cert_store.initialized = 1;
    kprintf("[CERT] Certificate store initialized\n");
    return CERT_SUCCESS;
}

int cert_add_trusted(const uint8_t *cert_der, size_t len) {
    if (!cert_store.initialized) cert_store_init();
    if (cert_store.count >= CERT_MAX_TRUSTED_CERTS) return CERT_ERR_STORE_FULL;
    
    cert_x509_t *cert = cert_parse_der(cert_der, len);
    if (!cert) return CERT_ERR_INVALID_FORMAT;
    
    cert_store.certs[cert_store.count++] = cert;
    kprintf("[CERT] Added trusted: %s\n", cert->subject.common_name);
    
    return CERT_SUCCESS;
}

int cert_add_trusted_pem(const char *cert_pem, size_t len) {
    if (!cert_store.initialized) cert_store_init();
    if (cert_store.count >= CERT_MAX_TRUSTED_CERTS) return CERT_ERR_STORE_FULL;
    
    cert_x509_t *cert = cert_parse_pem(cert_pem, len);
    if (!cert) return CERT_ERR_INVALID_FORMAT;
    
    cert_store.certs[cert_store.count++] = cert;
    kprintf("[CERT] Added trusted: %s\n", cert->subject.common_name);
    
    return CERT_SUCCESS;
}

int cert_store_load_bundle(const char *pem_data, size_t len) {
    if (!cert_store.initialized) cert_store_init();
    
    const char *begin = "-----BEGIN CERTIFICATE-----";
    const char *pos = pem_data;
    const char *end_data = pem_data + len;
    int loaded = 0;
    
    while (pos < end_data) {
        const char *cert_start = strstr(pos, begin);
        if (!cert_start || cert_start >= end_data) break;
        
        const char *cert_end = strstr(cert_start + 1, begin);
        size_t cert_len = cert_end ? (size_t)(cert_end - cert_start) : (size_t)(end_data - cert_start);
        
        if (cert_add_trusted_pem(cert_start, cert_len) == CERT_SUCCESS) {
            loaded++;
        }
        
        pos = cert_start + cert_len;
    }
    
    kprintf("[CERT] Loaded %d certificates from bundle\n", loaded);
    return loaded > 0 ? CERT_SUCCESS : CERT_ERR_INVALID_FORMAT;
}

int cert_store_load_file(const char *path) {
    // Read file. #fix-tls-certverify: vfs_open() takes a mode STRING ("r"),
    // not an int - the old `vfs_open(path, 0)` passed a NULL mode, which
    // vfs_open() feeds straight into strchr(mode, 'r') -> crash. This path
    // had no callers yet, so the bug was latent; wiring it up for the CA
    // bundle load is what would have hit it.
    int fd = vfs_open(path, "r");
    if (fd < 0) {
        kprintf("[CERT] Failed to open %s\n", path);
        return CERT_ERR_NOT_FOUND;
    }

    // Get file size (read until EOF). 512KB covers a full curl-style Mozilla
    // CA bundle (~190KB for ~120 roots) with headroom.
    size_t cap = 512 * 1024;
    uint8_t *buffer = kmalloc(cap);
    if (!buffer) {
        vfs_close(fd);
        return CERT_ERR_NO_MEMORY;
    }

    size_t total = 0;
    ssize_t n;
    while ((n = vfs_read(fd, buffer + total, 4096)) > 0) {
        total += (size_t)n;
        if (total >= cap - 4096) break;
    }
    vfs_close(fd);

    int ret = cert_store_load_bundle((const char *)buffer, total);
    kfree(buffer);

    return ret;
}

// #fix-tls-certverify: wires up a real CA bundle load at boot. Uses
// fat_read_file(&g_fat_fs, ...) - the same reader desktop.c/editor.c/
// services.c/perms.c all use for boot-time config - NOT cert_store_load_file()
// (which goes through net/fs/netfs.c's vfs_open(); that is a *different*,
// separate VFS that only resolves explicitly-registered NFS/SMB mounts and
// has no notion of the boot root at all, so it can never find this file).
// fat_read_file() transparently redirects to the ext2 root when one is
// mounted (fs/fat.c's g_root_ext2 check), so this works whether the running
// VM's /CONFIG lives on the FAT ESP or an ext2 root disk.
int cert_store_load_default_bundle(void) {
    extern fat_fs_t g_fat_fs;
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, "/CONFIG/CACERTS.PEM", &size);
    if (data && size > 0) {
        int ret = cert_store_load_bundle((const char *)data, size);
        kfree(data);
        if (ret == CERT_SUCCESS) {
            kprintf("[CERT] Trust store loaded from /CONFIG/CACERTS.PEM (%d certs)\n",
                    cert_store_count());
            return CERT_SUCCESS;
        }
    } else if (data) {
        kfree(data);
    }
    kprintf("[CERT] WARNING: no CA bundle found (/CONFIG/CACERTS.PEM) - "
            "HTTPS certificate verification will reject every server "
            "(no trust anchors loaded)\n");
    return CERT_ERR_NOT_FOUND;
}

int cert_store_count(void) {
    return cert_store.count;
}

// Do two certificates carry the identical public key? Used by cert_is_trusted
// below for cross-signed-root compatibility (see comment there).
static int cert_keys_equal(const cert_x509_t *a, const cert_x509_t *b) {
    if (a->key_type != b->key_type) return 0;
    switch (a->key_type) {
        case CERT_KEY_RSA:
            return a->public_key.rsa.modulus_len == b->public_key.rsa.modulus_len &&
                   a->public_key.rsa.exponent_len == b->public_key.rsa.exponent_len &&
                   memcmp(a->public_key.rsa.modulus, b->public_key.rsa.modulus,
                          a->public_key.rsa.modulus_len) == 0 &&
                   memcmp(a->public_key.rsa.exponent, b->public_key.rsa.exponent,
                          a->public_key.rsa.exponent_len) == 0;
        case CERT_KEY_ECDSA_P256:
        case CERT_KEY_ECDSA_P384:
            return a->public_key.ecdsa.coord_len == b->public_key.ecdsa.coord_len &&
                   memcmp(a->public_key.ecdsa.x, b->public_key.ecdsa.x,
                          a->public_key.ecdsa.coord_len) == 0 &&
                   memcmp(a->public_key.ecdsa.y, b->public_key.ecdsa.y,
                          a->public_key.ecdsa.coord_len) == 0;
        default:
            return 0;
    }
}

int cert_is_trusted(const cert_x509_t *cert) {
    for (int i = 0; i < cert_store.count; i++) {
        if (memcmp(cert->sha256_fingerprint,
                   cert_store.certs[i]->sha256_fingerprint, 32) == 0) {
            return 1;
        }
        // #fix-tls-certverify: cross-signed root compatibility. Some servers
        // present a root CA wrapped in a cross-signing certificate for
        // legacy-client compatibility - e.g. Google's "GTS Root R4" served
        // signed BY GlobalSign's older root, instead of (or alongside) its
        // own self-signed form. That cross-signed certificate has entirely
        // different bytes (different issuer, different signature) from the
        // self-signed "GTS Root R4" in our CA bundle, so the fingerprint
        // check above never matches it even though it carries the exact
        // same public key. Per RFC 5280, a trust anchor is fundamentally a
        // (name, public key) pair, not one specific DER encoding - so also
        // trust a presented cert whose key matches a bundled root's key.
        // (Real-world impact without this: chains through GTS Root R4,
        // and other CAs with legacy cross-signed variants, failed with
        // CERT_ERR_NO_TRUST_ANCHOR against sites that are otherwise
        // completely legitimate, e.g. api.coingecko.com behind Cloudflare/
        // Google Trust Services.)
        if (cert_keys_equal(cert, cert_store.certs[i])) {
            return 1;
        }
    }
    return 0;
}

// =============================================================================
// Certificate Validation
// =============================================================================

// Wildcard hostname matching
static int hostname_match(const char *pattern, const char *hostname) {
    // Check for wildcard
    if (pattern[0] == '*' && pattern[1] == '.') {
        // Find first dot in hostname
        const char *dot = strchr(hostname, '.');
        if (!dot) return 0;
        // Compare rest
        return strcasecmp(pattern + 2, dot + 1) == 0;
    }
    return strcasecmp(pattern, hostname) == 0;
}

int cert_verify_hostname(const cert_x509_t *cert, const char *hostname) {
    // Check Subject Alternative Names first
    for (int i = 0; i < cert->san_count; i++) {
        if (cert->san[i].type == SAN_DNS_NAME) {
            if (hostname_match(cert->san[i].value.dns_name, hostname)) {
                return CERT_SUCCESS;
            }
        }
    }
    
    // Fall back to Common Name
    if (hostname_match(cert->subject.common_name, hostname)) {
        return CERT_SUCCESS;
    }
    
    return CERT_ERR_NAME_MISMATCH;
}

int cert_time_compare(const cert_time_t *a, const cert_time_t *b) {
    if (a->year != b->year) return (a->year < b->year) ? -1 : 1;
    if (a->month != b->month) return (a->month < b->month) ? -1 : 1;
    if (a->day != b->day) return (a->day < b->day) ? -1 : 1;
    if (a->hour != b->hour) return (a->hour < b->hour) ? -1 : 1;
    if (a->minute != b->minute) return (a->minute < b->minute) ? -1 : 1;
    if (a->second != b->second) return (a->second < b->second) ? -1 : 1;
    return 0;
}

// Get current time from the real-time clock (CMOS RTC). #fix-tls-certverify:
// this used to be a hardcoded "2026-01-15 12:00:00" constant, which meant
// notBefore/notAfter checks never actually reflected wall-clock time (an
// expired OR not-yet-valid certificate could pass, or a certificate that
// just expired could still be silently treated as valid forever).
void cert_get_current_time(cert_time_t *time) {
    int hour, minute, second;
    int day, month, year, weekday;
    rtc_read_time(&hour, &minute, &second);
    rtc_read_date(&day, &month, &year, &weekday);

    time->year = (uint16_t)year;
    time->month = (uint8_t)month;
    time->day = (uint8_t)day;
    time->hour = (uint8_t)hour;
    time->minute = (uint8_t)minute;
    time->second = (uint8_t)second;
}

int cert_verify_validity(const cert_x509_t *cert) {
    cert_time_t now;
    cert_get_current_time(&now);
    
    if (cert_time_compare(&now, &cert->not_before) < 0) {
        return CERT_ERR_NOT_YET_VALID;
    }
    
    if (cert_time_compare(&now, &cert->not_after) > 0) {
        return CERT_ERR_EXPIRED;
    }
    
    return CERT_SUCCESS;
}

// RSA PKCS#1 v1.5 signature verification. #fix-tls-certverify: this used to be
// a local stub that copied bytes around and unconditionally `return 0`
// (success) WITHOUT ever doing the modular exponentiation - it rubber-stamped
// every RSA signature. Now it's a thin adapter onto the real, working
// bignum-based RSA verify in crypto/rsa.c (already used and exercised by the
// SSH server/client's pubkey auth).
static int rsa_verify_pkcs1(const cert_rsa_key_t *key, const uint8_t *hash,
                            size_t hash_len, const uint8_t *sig, size_t sig_len) {
    rsa_public_key_t pub;
    pub.n = key->modulus;
    pub.n_len = key->modulus_len;
    pub.e = key->exponent;
    pub.e_len = key->exponent_len;

    int ret;
    switch (hash_len) {
        case 32: ret = rsa_verify_pkcs1_sha256(&pub, hash, hash_len, sig, sig_len); break;
        case 48: ret = rsa_verify_pkcs1_sha384(&pub, hash, hash_len, sig, sig_len); break;
        case 64: ret = rsa_verify_pkcs1_sha512(&pub, hash, hash_len, sig, sig_len); break;
        default: return CERT_ERR_UNSUPPORTED;
    }
    return (ret == RSA_SUCCESS) ? CERT_SUCCESS : CERT_ERR_SIGNATURE;
}

// Parse an ECDSA-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER } (the DER blob
// carried inside the certificate's signature BIT STRING for ECDSA-signed certs).
static int parse_ecdsa_sig_value(const uint8_t *der, size_t len,
                                  uint8_t **r, size_t *r_len,
                                  uint8_t **s, size_t *s_len) {
    asn1_ctx_t ctx = { der, len, 0 };
    asn1_ctx_t seq;
    if (asn1_enter_sequence(&ctx, &seq) < 0) return -1;
    if (asn1_get_integer(&seq, r, r_len) < 0) return -1;
    if (asn1_get_integer(&seq, s, s_len) < 0) return -1;
    // Strip a leading 0x00 sign-guard byte, if present (ASN.1 INTEGER pads a
    // positive value whose MSB is set with 0x00 so it isn't read as negative).
    if (*r_len > 1 && (*r)[0] == 0) { (*r)++; (*r_len)--; }
    if (*s_len > 1 && (*s)[0] == 0) { (*s)++; (*s_len)--; }
    return 0;
}

// ECDSA signature verification (P-256 / P-384), real EC point math via
// crypto/ecdsa.c. #fix-tls-certverify: this used to be a "// Placeholder"
// that unconditionally `return 0` (success) - ANY ECDSA-signed certificate
// (and by extension any impersonation using one) verified as genuine.
static int ecdsa_verify_cert(const cert_ecdsa_key_t *key, cert_key_type_t key_type,
                              const uint8_t *hash, size_t hash_len,
                              const uint8_t *sig, size_t sig_len) {
    uint8_t *r, *s;
    size_t r_len, s_len;
    if (parse_ecdsa_sig_value(sig, sig_len, &r, &r_len, &s, &s_len) < 0) {
        return CERT_ERR_INVALID_FORMAT;
    }

    ecdsa_curve_id_t curve = (key_type == CERT_KEY_ECDSA_P384) ?
                              ECDSA_CURVE_P384 : ECDSA_CURVE_P256;

    int ok = ecdsa_verify(curve, hash, hash_len, r, r_len, s, s_len,
                          key->x, key->y, key->coord_len);
    return ok ? CERT_SUCCESS : CERT_ERR_SIGNATURE;
}

// =============================================================================
// #502: TLS "digitally-signed" verification against a certificate public key.
//
// This is the check that makes TLS 1.2 ECDHE safe: the ServerKeyExchange
// signature is the ONLY thing tying the server's ephemeral ECDHE public key to
// the certificate chain we just validated. Skipping it (or letting it fail
// open) means any on-path attacker can swap in their own ECDHE key and read
// everything, while the certificate still checks out. That is exactly the class
// of hole #232 was raised for, so every path below either verifies or fails.
//
// C justification (CLAUDE.md rule: new kernel code is Rust unless entangled):
// this is a dispatcher over cert_x509_t's key union and the existing C verify
// primitives (rsa_verify_pkcs1_*, rsa_verify_pss_*, ecdsa_verify, the ASN.1
// helpers above). Every one of those is C and lives in this file's neighborhood;
// re-expressing the dispatcher in Rust would mean FFI-ing the cert key union and
// the ASN.1 walker, i.e. forking shared primitives for no safety gain. The parts
// where bounds actually bite - the EMSA-PSS decode and the ServerKeyExchange
// framing - ARE in Rust (emsa_pss_verify_rs, tls12_ske_parse_rs).
// =============================================================================
int cert_verify_tls_signature(const cert_x509_t *cert, uint16_t sig_scheme,
                              const uint8_t *data, size_t data_len,
                              const uint8_t *sig, size_t sig_len) {
    if (!cert || !data || !sig || sig_len == 0) return CERT_ERR_INVALID_FORMAT;

    // 1. Which digest does this scheme sign with?
    size_t hash_len;
    switch (sig_scheme) {
        case 0x0401: case 0x0403: case 0x0804: hash_len = 32; break;
        case 0x0501: case 0x0503: case 0x0805: hash_len = 48; break;
        case 0x0601:                           hash_len = 64; break;
        // 0x0806 rsa_pss_rsae_sha512: advertised but our PSS MGF1 is SHA-256/384
        // only. Fail closed rather than pretend.
        default:
            kprintf("[CERT] TLS signature scheme 0x%04x unsupported - rejecting\n",
                    sig_scheme);
            return CERT_ERR_UNSUPPORTED;
    }

    uint8_t hash[64];
    if (hash_len == 32) {
        sha256(data, data_len, hash);
    } else if (hash_len == 48) {
        sha384(data, data_len, hash);
    } else {
        sha512_ctx_t sctx;
        sha512_init(&sctx);
        sha512_update(&sctx, data, data_len);
        sha512_final(&sctx, hash);
    }

    // 2. Dispatch on the scheme's signature algorithm, checking it matches the
    //    certificate's actual key type (an RSA scheme against an ECDSA key, or
    //    vice versa, is a confusion attempt -> reject).
    switch (sig_scheme) {
        case 0x0401: case 0x0501: case 0x0601: {   // RSASSA-PKCS1-v1_5
            if (cert->key_type != CERT_KEY_RSA) return CERT_ERR_SIGNATURE;
            const cert_rsa_key_t *k = &cert->public_key.rsa;
            rsa_public_key_t pub = { k->modulus, k->modulus_len, k->exponent, k->exponent_len };
            int ret;
            if (hash_len == 32)      ret = rsa_verify_pkcs1_sha256(&pub, hash, 32, sig, sig_len);
            else if (hash_len == 48) ret = rsa_verify_pkcs1_sha384(&pub, hash, 48, sig, sig_len);
            else                     ret = rsa_verify_pkcs1_sha512(&pub, hash, 64, sig, sig_len);
            return (ret == RSA_SUCCESS) ? CERT_SUCCESS : CERT_ERR_SIGNATURE;
        }
        case 0x0804: case 0x0805: {                // RSASSA-PSS (rsae)
            if (cert->key_type != CERT_KEY_RSA) return CERT_ERR_SIGNATURE;
            const cert_rsa_key_t *k = &cert->public_key.rsa;
            rsa_public_key_t pub = { k->modulus, k->modulus_len, k->exponent, k->exponent_len };
            int ret = (hash_len == 32)
                        ? rsa_verify_pss_sha256(&pub, hash, 32, sig, sig_len)
                        : rsa_verify_pss_sha384(&pub, hash, 48, sig, sig_len);
            return (ret == RSA_SUCCESS) ? CERT_SUCCESS : CERT_ERR_SIGNATURE;
        }
        case 0x0403: case 0x0503: {                // ECDSA
            // The scheme pins the curve: ecdsa_secp256r1_sha256 REQUIRES a P-256
            // key, ecdsa_secp384r1_sha384 a P-384 key.
            cert_key_type_t want = (sig_scheme == 0x0403) ? CERT_KEY_ECDSA_P256
                                                          : CERT_KEY_ECDSA_P384;
            if (cert->key_type != want) return CERT_ERR_SIGNATURE;
            uint8_t *r, *s; size_t r_len, s_len;
            if (parse_ecdsa_sig_value(sig, sig_len, &r, &r_len, &s, &s_len) < 0) {
                return CERT_ERR_INVALID_FORMAT;
            }
            const cert_ecdsa_key_t *k = &cert->public_key.ecdsa;
            ecdsa_curve_id_t curve = (want == CERT_KEY_ECDSA_P384) ? ECDSA_CURVE_P384
                                                                   : ECDSA_CURVE_P256;
            int ok = ecdsa_verify(curve, hash, hash_len, r, r_len, s, s_len,
                                  k->x, k->y, k->coord_len);
            return ok ? CERT_SUCCESS : CERT_ERR_SIGNATURE;
        }
        default:
            return CERT_ERR_UNSUPPORTED;
    }
}

int cert_verify_signature(const cert_x509_t *cert, const cert_x509_t *issuer) {
    // Hash the TBS data with the algorithm the certificate says it was signed with.
    uint8_t hash[64];  // Max SHA-512
    size_t hash_len;

    switch (cert->sig_algorithm) {
        case CERT_SIG_RSA_SHA256:
        case CERT_SIG_ECDSA_SHA256:
            sha256(cert->tbs_data, cert->tbs_len, hash);
            hash_len = 32;
            break;
        case CERT_SIG_RSA_SHA384:
        case CERT_SIG_ECDSA_SHA384:
            sha384(cert->tbs_data, cert->tbs_len, hash);
            hash_len = 48;
            break;
        case CERT_SIG_RSA_SHA512: {
            // No one-shot sha512() helper exists (only sha384() does) - use
            // the init/update/final trio directly.
            sha512_ctx_t sctx;
            sha512_init(&sctx);
            sha512_update(&sctx, cert->tbs_data, cert->tbs_len);
            sha512_final(&sctx, hash);
            hash_len = 64;
            break;
        }
        default:
            // Includes CERT_SIG_ED25519: cert_parse_sig_algorithm() never
            // actually recognizes the Ed25519 OID (no case for it), so an
            // Ed25519-signed cert already fails to parse before reaching
            // here. Fail closed for anything else too.
            return CERT_ERR_UNSUPPORTED;
    }

    // Verify based on issuer's key type
    switch (issuer->key_type) {
        case CERT_KEY_RSA:
            return rsa_verify_pkcs1(&issuer->public_key.rsa, hash, hash_len,
                                   cert->signature, cert->signature_len);
        case CERT_KEY_ECDSA_P256:
        case CERT_KEY_ECDSA_P384:
            return ecdsa_verify_cert(&issuer->public_key.ecdsa, issuer->key_type,
                                     hash, hash_len, cert->signature, cert->signature_len);
        default:
            return CERT_ERR_UNSUPPORTED;
    }
}

// Find the Nth trust-store cert whose subject CN matches `cert`'s issuer CN.
// *iter is the search cursor; call repeatedly until it returns NULL.
//
// #502: this REPLACES a cert_find_issuer() that matched on common_name and
// returned the FIRST hit. A CN alone does not identify a CA. GlobalSign ships
// FIVE roots whose subject CN is the bare string "GlobalSign", distinguished
// only by OU, and they are not even all the same key type:
//   OU=GlobalSign ECC Root CA - R4   ECDSA
//   OU=GlobalSign ECC Root CA - R5   ECDSA
//   OU=GlobalSign Root CA - R3       RSA
//   OU=GlobalSign Root CA - R6       RSA
// In our shipped /CONFIG/CACERTS.PEM the ECC R4 root sorts FIRST, so every
// chain rooted at GlobalSign R3 (xkcd.com's, among many) was matched to the
// ECDSA R4 root, cert_verify_signature() dispatched on the issuer's key type to
// the ECDSA path, parse_ecdsa_sig_value() choked on an RSA PKCS#1 signature
// blob, and the chain was rejected with CERT_ERR_INVALID_FORMAT. The single
// candidate was simply the wrong certificate.
//
// This failed CLOSED (valid sites rejected, nothing forged accepted), so it was
// an availability bug rather than a hole - but it silently made a major CA
// unreachable, and it looked like a TLS bug, not a trust-store bug.
//
// The fix is to treat the CN match as a CANDIDATE FILTER, not an answer, and
// let the signature decide. Security is unchanged: every candidate must still
// pass a real cert_verify_signature() AND cert_is_trusted() before it is
// accepted. The search got exhaustive; the acceptance test did not move.
static cert_x509_t *cert_next_issuer_candidate(const cert_x509_t *cert, int *iter) {
    while (*iter < cert_store.count) {
        cert_x509_t *ca = cert_store.certs[(*iter)++];
        if (strcmp(ca->subject.common_name, cert->issuer.common_name) == 0) {
            return ca;
        }
    }
    return NULL;
}

int cert_verify_chain(const cert_chain_t *chain, const char *hostname) {
    if (!chain || chain->count == 0) return CERT_ERR_INVALID_FORMAT;
    
    // Verify end-entity certificate hostname
    if (hostname) {
        int ret = cert_verify_hostname(chain->certs[0], hostname);
        if (ret != CERT_SUCCESS) return ret;
    }
    
    // Verify each certificate in chain
    for (int i = 0; i < chain->count; i++) {
        cert_x509_t *cert = chain->certs[i];

        // Check validity period
        int ret = cert_verify_validity(cert);
        if (ret != CERT_SUCCESS) return ret;

        if (i + 1 < chain->count) {
            // Issuer is the next certificate the server presented.
            cert_x509_t *issuer = chain->certs[i + 1];
            if (!issuer->is_ca) return CERT_ERR_SIGNATURE;
            ret = cert_verify_signature(cert, issuer);
            if (ret != CERT_SUCCESS) return ret;
            // Reached a trusted root?
            if (cert_is_trusted(issuer)) return CERT_SUCCESS;
            continue;
        }

        // Last certificate presented: its issuer must come from the trust store.
        // #502: try EVERY store cert whose subject CN matches, not just the
        // first - a CN can be shared by several unrelated roots with different
        // key types (see cert_next_issuer_candidate). Accept only on a real
        // signature verify against a genuinely trusted cert.
        int iter = 0;
        cert_x509_t *ca;
        int saw_candidate = 0;
        while ((ca = cert_next_issuer_candidate(cert, &iter)) != NULL) {
            saw_candidate = 1;
            if (cert_verify_signature(cert, ca) == CERT_SUCCESS && cert_is_trusted(ca)) {
                return CERT_SUCCESS;
            }
        }
        // Self-signed and directly trusted (a pinned root presented by the peer).
        if (cert_is_trusted(cert)) return CERT_SUCCESS;
        // A candidate existed but none verified: the chain does not actually
        // chain to anything we trust.
        return saw_candidate ? CERT_ERR_SIGNATURE : CERT_ERR_NO_TRUST_ANCHOR;
    }

    return CERT_ERR_NO_TRUST_ANCHOR;
}

// Parse certificate chain from server
int cert_parse_chain(const uint8_t *data, size_t len, cert_chain_t *chain) {
    chain->count = 0;

    // Per-cert framing (3-byte length + DER) is the Rust seam tls_cert_next
    // (#404/#502): each step bounds the 3-byte cert length to the buffer and
    // hands back the DER byte-range; a step returns 0 at a clean end or -1 on a
    // length that overruns (stops the walk, no over-read). cert_parse_der (the
    // X.509 parse) stays in C. Same accept/reject + same per-cert bounds as the
    // old inline loop on every well-formed AND malformed certificate_list.
    uint32_t pos = 0;
    tls_cert_ent_t ent;
    while (chain->count < CERT_MAX_CHAIN_DEPTH &&
           tls_cert_next(data, (uint32_t)len, &pos, &ent) == 1) {
        cert_x509_t *cert = cert_parse_der(data + ent.cert_off, ent.cert_len);
        if (cert) {
            chain->certs[chain->count++] = cert;
        }
    }

    return (chain->count > 0) ? CERT_SUCCESS : CERT_ERR_INVALID_FORMAT;
}

void cert_chain_free(cert_chain_t *chain) {
    for (int i = 0; i < chain->count; i++) {
        cert_free(chain->certs[i]);
    }
    chain->count = 0;
}

// =============================================================================
// Debug/Info Functions
// =============================================================================

const char *cert_get_cn(const cert_x509_t *cert) {
    return cert->subject.common_name;
}

void cert_get_fingerprint(const cert_x509_t *cert, uint8_t fingerprint[32]) {
    memcpy(fingerprint, cert->sha256_fingerprint, 32);
}

void cert_print_info(const cert_x509_t *cert) {
    kprintf("Certificate:\n");
    kprintf("  Subject: %s\n", cert->subject.common_name);
    kprintf("  Issuer: %s\n", cert->issuer.common_name);
    kprintf("  Valid: %04d-%02d-%02d to %04d-%02d-%02d\n",
            cert->not_before.year, cert->not_before.month, cert->not_before.day,
            cert->not_after.year, cert->not_after.month, cert->not_after.day);
    kprintf("  Key type: %s\n", 
            cert->key_type == CERT_KEY_RSA ? "RSA" : 
            cert->key_type == CERT_KEY_ECDSA_P256 ? "ECDSA P-256" :
            cert->key_type == CERT_KEY_ECDSA_P384 ? "ECDSA P-384" : "Unknown");
    kprintf("  Is CA: %s\n", cert->is_ca ? "Yes" : "No");
    
    if (cert->san_count > 0) {
        kprintf("  SANs:\n");
        for (int i = 0; i < cert->san_count; i++) {
            if (cert->san[i].type == SAN_DNS_NAME) {
                kprintf("    DNS: %s\n", cert->san[i].value.dns_name);
            }
        }
    }
}
