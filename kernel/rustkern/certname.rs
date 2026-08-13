// rustkern/certname.rs - X.509 name matching and usage/constraint policy.
//
// WHY THIS IS RUST AND WHY IT IS ONE MODULE
//
// Every byte this file touches came off the network from a party we have not
// yet authenticated. That is the exact place memory safety is worth paying for,
// and the 2026-07-16 rule makes Rust the default for new kernel code anyway.
//
// It is ONE module rather than four because the four defects it closes are one
// defect wearing four hats: the certificate said something, and nobody checked
// it. Splitting the checks across call sites is how they went missing in the
// first place. cert_verify_chain() now reaches a single chokepoint here with the
// whole chain in hand, so a future certificate property cannot be "parsed but
// never verified" without someone deleting a line from THIS file.
//
// WHAT WAS WRONG, MEASURED (tools/tls-cert-harness, --expect-red, 6/6 reproduced
// against unmodified shipping code):
//
//  1. CN FALLBACK WITH SANs PRESENT. cert_verify_hostname() walked the SAN list
//     and then fell through to strcasecmp on subject.common_name unconditionally.
//     RFC 6125 section 6.4.4 is explicit that a certificate carrying a
//     subjectAltName must be matched on SANs ONLY. A certificate legitimately
//     issued for other.example, whose CN happens to read target.example, was
//     accepted for target.example.
//
//  2. EMBEDDED NUL IN A dNSName. cert_parse_san() memcpy'd the dNSName into a
//     char[256] and appended '\0', then hostname_match() used strcasecmp. A CA
//     that validates the trailing label will issue for
//     "target.example\0.attacker.example"; C string handling truncates it at the
//     NUL and matches "target.example". Everything below is length-delimited:
//     no byte of a SAN is ever handed to a NUL-terminated string function, and a
//     dNSName that CONTAINS a NUL is rejected outright rather than truncated,
//     because there is no legitimate reason for one to exist.
//
//  3. keyUsage AND extendedKeyUsage WERE NEVER PARSED AT ALL. This is worse than
//     the ticket said. `int key_usage; int ext_key_usage;` are fields of
//     cert_x509_t, cert_parse_extensions() handled exactly two OIDs
//     (basicConstraints and subjectAltName), and kzalloc left both fields 0
//     forever. So it was not "parsed but not checked": nothing ever read the
//     extensions. A certificate marked clientAuth-only was as good as a server
//     certificate. This tree has shipped the parsed-but-never-verified
//     confusion once already, in TLS 1.3 CertificateVerify (#510), which was a
//     full on-path MITM. That was the first occurrence. This is the second, so
//     the answer is a chokepoint, not another checklist item.
//
//  4. pathLenConstraint WAS PARSED AND IGNORED. cert->path_length was filled in
//     by cert_parse_extensions() and read by nothing. A CA constrained to issue
//     no intermediates could issue one and it was accepted.

use core::slice;

// ---------------------------------------------------------------------------
// FFI mirrors. Sizes are locked on the C side with _Static_assert.
// ---------------------------------------------------------------------------

pub const CERT_MAX_CN_LENGTH: usize = 256; // cert_store.h

pub const SAN_KIND_DNS: u32 = 0;
pub const SAN_KIND_IP: u32 = 1;
pub const SAN_KIND_EMAIL: u32 = 2;

/// #[repr(C)] mirror of cert_san_t (net/tls/cert_store.h).
///
/// `len` is AUTHORITATIVE and there is no NUL terminator, which is the whole
/// point: the previous layout was a `char[256]` that could only be read with
/// str* functions, so the embedded-NUL truncation was not a bug in the matcher,
/// it was baked into the data structure. A type that cannot represent the
/// attack is a better fix than a matcher that remembers to look for it.
#[repr(C)]
pub struct CertSan {
    pub kind: u32,
    pub len: u32,
    pub val: [u8; CERT_MAX_CN_LENGTH],
}

/// One link of a chain, leaf first, trust anchor last, as handed to
/// cert_chain_policy_rs.
#[repr(C)]
pub struct CertNode {
    pub is_ca: i32,
    pub path_len: i32, // -1 when pathLenConstraint is absent
    pub key_usage: u32,
    pub eku: u32,
    pub ku_present: i32,
    pub eku_present: i32,
}

// keyUsage bit positions, RFC 5280 4.2.1.3. Bit 0 is the MOST significant bit
// of the first content byte of the BIT STRING, which is the usual place this
// gets encoded backwards.
pub const KU_DIGITAL_SIGNATURE: u32 = 1 << 0;
pub const KU_NON_REPUDIATION: u32 = 1 << 1;
pub const KU_KEY_ENCIPHERMENT: u32 = 1 << 2;
pub const KU_DATA_ENCIPHERMENT: u32 = 1 << 3;
pub const KU_KEY_AGREEMENT: u32 = 1 << 4;
pub const KU_KEY_CERT_SIGN: u32 = 1 << 5;
pub const KU_CRL_SIGN: u32 = 1 << 6;

// extendedKeyUsage purposes we recognise, RFC 5280 4.2.1.12.
pub const EKU_SERVER_AUTH: u32 = 1 << 0;
pub const EKU_CLIENT_AUTH: u32 = 1 << 1;
pub const EKU_CODE_SIGNING: u32 = 1 << 2;
pub const EKU_EMAIL_PROTECTION: u32 = 1 << 3;
pub const EKU_TIME_STAMPING: u32 = 1 << 4;
pub const EKU_OCSP_SIGNING: u32 = 1 << 5;
pub const EKU_ANY: u32 = 1 << 6;

// Policy verdicts. Negative values so a C caller can map them onto the
// CERT_ERR_* space without ambiguity with CERT_SUCCESS (0).
pub const POLICY_OK: i32 = 0;
pub const POLICY_BAD_USAGE: i32 = -1;
pub const POLICY_BAD_PATHLEN: i32 = -2;
pub const POLICY_NOT_CA: i32 = -3;

// ---------------------------------------------------------------------------
// Minimal DER reader
// ---------------------------------------------------------------------------
//
// Deliberately tiny and total: every accessor returns Option, there is no
// indexing that can panic, and a malformed length is None rather than a
// best-effort guess. Long-form lengths above 4 bytes are refused outright; a
// certificate extension needing a 2^40-byte value is not a certificate.

struct Der<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Der<'a> {
    fn new(buf: &'a [u8]) -> Self {
        Der { buf, pos: 0 }
    }

    fn remaining(&self) -> usize {
        self.buf.len().saturating_sub(self.pos)
    }

    /// Read one TLV header. Returns (tag, content) and advances past the
    /// content. None on any truncation or over-long length.
    fn tlv(&mut self) -> Option<(u8, &'a [u8])> {
        if self.remaining() < 2 {
            return None;
        }
        let tag = *self.buf.get(self.pos)?;
        self.pos += 1;
        let b0 = *self.buf.get(self.pos)?;
        self.pos += 1;

        let len: usize = if b0 & 0x80 == 0 {
            b0 as usize
        } else {
            let n = (b0 & 0x7f) as usize;
            // Indefinite length (n == 0) is not legal in DER, and anything
            // wider than 4 bytes cannot describe a real extension.
            if n == 0 || n > 4 || self.remaining() < n {
                return None;
            }
            let mut v: usize = 0;
            for i in 0..n {
                v = (v << 8) | (*self.buf.get(self.pos + i)? as usize);
            }
            self.pos += n;
            v
        };

        if len > self.remaining() {
            return None;
        }
        let content = self.buf.get(self.pos..self.pos + len)?;
        self.pos += len;
        Some((tag, content))
    }
}

// ---------------------------------------------------------------------------
// ASCII case-insensitive comparison, length-delimited
// ---------------------------------------------------------------------------
//
// Not strcasecmp, and not locale-aware: DNS names are ASCII (an
// internationalised name reaches us already punycoded), and a locale-sensitive
// fold is a way to make two different names compare equal.

fn ascii_lower(b: u8) -> u8 {
    if b.is_ascii_uppercase() {
        b + 32
    } else {
        b
    }
}

fn eq_ascii_ci(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for i in 0..a.len() {
        if ascii_lower(a[i]) != ascii_lower(b[i]) {
            return false;
        }
    }
    true
}

/// A dNSName or a queried hostname is usable only if it is plain ASCII with no
/// NUL and no embedded separators that would let one string stand for two.
/// This is where the embedded-NUL attack dies: not by truncating, by refusing.
fn is_sane_dns(name: &[u8]) -> bool {
    if name.is_empty() || name.len() > 255 {
        return false;
    }
    for &b in name {
        // Reject NUL explicitly and loudly, plus every other control byte and
        // anything non-ASCII. A legitimate dNSName has no business here.
        if b == 0 || b < 0x21 || b > 0x7e {
            return false;
        }
    }
    // A trailing dot would make "example.com." and "example.com" two spellings
    // of one name; a leading or doubled dot means an empty label.
    if name[0] == b'.' || name[name.len() - 1] == b'.' {
        return false;
    }
    let mut prev_dot = false;
    for &b in name {
        if b == b'.' {
            if prev_dot {
                return false;
            }
            prev_dot = true;
        } else {
            prev_dot = false;
        }
    }
    true
}

/// RFC 6125 wildcard matching, length-delimited.
///
/// A wildcard is honoured ONLY as a complete leftmost label ("*.example.com"),
/// never as a partial label ("w*.example.com") and never below the leftmost
/// position. It must also leave at least two labels behind it, so that
/// "*.com" cannot match every .com host. The old matcher did none of this: it
/// tested `pattern[0]=='*' && pattern[1]=='.'` and then strcasecmp'd the rest
/// against everything after the first dot in the hostname.
fn dns_match(pattern: &[u8], host: &[u8]) -> bool {
    if !is_sane_dns(pattern) || !is_sane_dns(host) {
        return false;
    }

    if pattern.len() >= 2 && pattern[0] == b'*' && pattern[1] == b'.' {
        let rest = &pattern[2..];
        // "*.com" must not match "anything.com": require >= 2 labels after the
        // wildcard, i.e. at least one dot in `rest`.
        if !rest.contains(&b'.') {
            return false;
        }
        // The wildcard covers exactly one label, so the host must have a
        // leftmost label and the remainder must match exactly.
        let dot = match host.iter().position(|&b| b == b'.') {
            Some(d) => d,
            None => return false,
        };
        if dot == 0 {
            return false; // empty leftmost label
        }
        return eq_ascii_ci(rest, &host[dot + 1..]);
    }

    // No wildcard anywhere else. A '*' in any other position is not a wildcard
    // and must not be treated as a literal that could match one either.
    if pattern.contains(&b'*') {
        return false;
    }

    eq_ascii_ci(pattern, host)
}

// ---------------------------------------------------------------------------
// Exported: hostname verification (RFC 6125)
// ---------------------------------------------------------------------------

/// Returns 1 if `host` is authorised by this certificate's names, else 0.
///
/// THE RULE, which is the fix for defect 1: if the certificate presents ANY
/// dNSName SAN, the CN is not consulted at all, even when no SAN matches. The
/// CN is a fallback only for certificates that carry no dNSName SAN whatsoever.
///
/// # Safety
/// `sans` must point to `san_count` readable CertSan values (or be null when
/// `san_count` is 0); `cn` must point to `cn_len` readable bytes; `host` to
/// `host_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn cert_hostname_match_rs(
    sans: *const CertSan,
    san_count: u32,
    dns_san_present: i32,
    cn: *const u8,
    cn_len: u32,
    host: *const u8,
    host_len: u32,
) -> i32 {
    if host.is_null() || host_len == 0 {
        return 0;
    }
    let host = slice::from_raw_parts(host, host_len as usize);

    // `dns_san_present` is set by cert_san_parse_rs when the certificate
    // carried a dNSName entry AT ALL, including one it then refused to store.
    // That distinction is load-bearing: a certificate whose ONLY dNSName holds
    // an embedded NUL would otherwise end up with an empty SAN list, look like
    // a certificate with no SANs, and fall back to its CN. The attack would
    // survive the fix by deleting its own evidence. A dNSName we could not
    // represent faithfully still counts as "this certificate uses SANs".
    let mut saw_dns_san = dns_san_present != 0;

    if !sans.is_null() && san_count > 0 {
        let n = core::cmp::min(san_count as usize, 64);
        for i in 0..n {
            let s = &*sans.add(i);
            if s.kind != SAN_KIND_DNS {
                continue;
            }
            let l = s.len as usize;
            if l == 0 || l > CERT_MAX_CN_LENGTH {
                continue;
            }
            saw_dns_san = true;
            if dns_match(&s.val[..l], host) {
                return 1;
            }
        }
    }

    // Defect 1: this early return is the whole fix. Previously control fell
    // through to the CN unconditionally.
    if saw_dns_san {
        return 0;
    }

    if cn.is_null() || cn_len == 0 {
        return 0;
    }
    let cn = slice::from_raw_parts(cn, cn_len as usize);
    if dns_match(cn, host) {
        1
    } else {
        0
    }
}

// ---------------------------------------------------------------------------
// Exported: subjectAltName parsing, length-delimited
// ---------------------------------------------------------------------------

/// Parse a subjectAltName extension value (the DER inside the extnValue OCTET
/// STRING) into `out`. Returns the number of entries written, or -1 if the
/// outer structure is malformed. `*dns_seen` is set to 1 if the extension
/// carried any dNSName, whether or not it survived validation.
///
/// A malformed dNSName is DROPPED rather than failing the whole certificate.
/// Failing closed on the certificate would be simpler, but this is a live
/// client for real websites and a rare, harmless encoding quirk in one SAN
/// entry should not make a site unreachable. Dropping is safe here only
/// BECAUSE `dns_seen` still suppresses the CN fallback; without that pairing,
/// dropping would be the vulnerability.
///
/// Replaces cert_parse_san()'s memcpy-and-NUL-terminate. A dNSName that fails
/// is_sane_dns (embedded NUL, control bytes, empty labels) is DROPPED rather
/// than stored: a name we cannot represent faithfully must not become a name we
/// can match loosely.
///
/// # Safety
/// `der` must point to `len` readable bytes; `out` to `cap` writable CertSan;
/// `dns_seen` writable or null.
#[no_mangle]
pub unsafe extern "C" fn cert_san_parse_rs(
    der: *const u8,
    len: u32,
    out: *mut CertSan,
    cap: u32,
    dns_seen: *mut i32,
) -> i32 {
    if !dns_seen.is_null() {
        *dns_seen = 0;
    }
    if der.is_null() || out.is_null() || cap == 0 {
        return -1;
    }
    let buf = slice::from_raw_parts(der, len as usize);

    let mut outer = Der::new(buf);
    let seq = match outer.tlv() {
        // GeneralNames ::= SEQUENCE OF GeneralName
        Some((0x30, c)) => c,
        _ => return -1,
    };

    let mut d = Der::new(seq);
    let mut n: usize = 0;

    while n < cap as usize {
        let (tag, content) = match d.tlv() {
            Some(v) => v,
            None => break,
        };
        let slot = &mut *out.add(n);
        match tag {
            // [2] dNSName, IA5String, context-specific primitive
            0x82 => {
                // Record that a dNSName was PRESENT before deciding whether it
                // is usable, so that dropping a malformed one cannot restore
                // the CN fallback. See cert_hostname_match_rs.
                if !dns_seen.is_null() {
                    *dns_seen = 1;
                }
                if !is_sane_dns(content) || content.len() > CERT_MAX_CN_LENGTH {
                    continue; // drop, do not truncate, and do not fall back
                }
                slot.kind = SAN_KIND_DNS;
                slot.len = content.len() as u32;
                slot.val[..content.len()].copy_from_slice(content);
                n += 1;
            }
            // [7] iPAddress, OCTET STRING of exactly 4 or 16 bytes
            0x87 => {
                if content.len() != 4 && content.len() != 16 {
                    continue;
                }
                slot.kind = SAN_KIND_IP;
                slot.len = content.len() as u32;
                slot.val[..content.len()].copy_from_slice(content);
                n += 1;
            }
            // [1] rfc822Name. Stored so cert_print_info can show it; never
            // consulted for hostname authorisation.
            0x81 => {
                if content.is_empty() || content.len() > CERT_MAX_CN_LENGTH {
                    continue;
                }
                slot.kind = SAN_KIND_EMAIL;
                slot.len = content.len() as u32;
                slot.val[..content.len()].copy_from_slice(content);
                n += 1;
            }
            _ => {}
        }
    }

    n as i32
}

// ---------------------------------------------------------------------------
// Exported: keyUsage / extendedKeyUsage parsing
// ---------------------------------------------------------------------------

/// Parse a keyUsage extension value (a DER BIT STRING) into a bitmask.
/// Returns 1 and writes `*out` on success, 0 if the extension is absent or
/// unparseable (the caller then treats keyUsage as NOT PRESENT, which per
/// RFC 5280 means unrestricted, and NOT as "no bits set", which would reject
/// everything).
///
/// # Safety
/// `der` must point to `len` readable bytes; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn cert_ku_parse_rs(der: *const u8, len: u32, out: *mut u32) -> i32 {
    if der.is_null() || out.is_null() {
        return 0;
    }
    let buf = slice::from_raw_parts(der, len as usize);
    let mut d = Der::new(buf);
    let content = match d.tlv() {
        Some((0x03, c)) => c, // BIT STRING
        _ => return 0,
    };
    if content.is_empty() {
        return 0;
    }
    let unused = content[0];
    if unused > 7 {
        return 0;
    }
    let bits = &content[1..];

    // RFC 5280 numbers keyUsage bits from the MOST significant bit of the first
    // content byte. Getting this backwards silently turns digitalSignature into
    // decipherOnly, so it is spelled out rather than done with a clever shift.
    let mut mask: u32 = 0;
    for (byte_idx, &b) in bits.iter().enumerate() {
        for bit in 0..8u32 {
            let n = (byte_idx as u32) * 8 + bit;
            if n > 8 {
                break; // only bits 0..8 are defined
            }
            // Bit `bit` counted from the MSB of this byte.
            if b & (0x80 >> bit) != 0 {
                mask |= 1 << n;
            }
        }
    }
    *out = mask;
    1
}

// EKU OID bodies (the DER content of the OBJECT IDENTIFIER, header stripped).
// id-kp = 1.3.6.1.5.5.7.3 = 2B 06 01 05 05 07 03, then one purpose byte.
const OID_KP_PREFIX: [u8; 7] = [0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03];
// anyExtendedKeyUsage = 2.5.29.37.0
const OID_EKU_ANY: [u8; 4] = [0x55, 0x1D, 0x25, 0x00];

/// Parse an extendedKeyUsage extension value (SEQUENCE OF OID) into a purpose
/// mask. Returns 1 and writes `*out` on success, 0 if absent or unparseable.
///
/// Unrecognised purpose OIDs are ignored rather than treated as permissive:
/// the mask simply does not gain EKU_SERVER_AUTH, so a certificate listing only
/// exotic purposes fails the server-authentication test, which is correct.
///
/// # Safety
/// `der` must point to `len` readable bytes; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn cert_eku_parse_rs(der: *const u8, len: u32, out: *mut u32) -> i32 {
    if der.is_null() || out.is_null() {
        return 0;
    }
    let buf = slice::from_raw_parts(der, len as usize);
    let mut outer = Der::new(buf);
    let seq = match outer.tlv() {
        Some((0x30, c)) => c,
        _ => return 0,
    };

    let mut d = Der::new(seq);
    let mut mask: u32 = 0;
    while let Some((tag, oid)) = d.tlv() {
        if tag != 0x06 {
            continue; // not an OBJECT IDENTIFIER
        }
        if oid == OID_EKU_ANY {
            mask |= EKU_ANY;
            continue;
        }
        if oid.len() == 8 && oid[..7] == OID_KP_PREFIX {
            mask |= match oid[7] {
                1 => EKU_SERVER_AUTH,
                2 => EKU_CLIENT_AUTH,
                3 => EKU_CODE_SIGNING,
                4 => EKU_EMAIL_PROTECTION,
                8 => EKU_TIME_STAMPING,
                9 => EKU_OCSP_SIGNING,
                _ => 0,
            };
        }
    }
    *out = mask;
    1
}

// ---------------------------------------------------------------------------
// Exported: the chain policy chokepoint
// ---------------------------------------------------------------------------

/// Check usage and basic-constraints policy over a whole chain.
///
/// `nodes[0]` is the end-entity certificate and `nodes[count-1]` is the trust
/// anchor. Returns POLICY_OK or one of the POLICY_* rejections.
///
/// THE THREE RULES, all of which were absent:
///
///   LEAF USAGE. If extendedKeyUsage is present it must permit serverAuth (or
///   anyExtendedKeyUsage). If keyUsage is present it must permit at least one
///   of digitalSignature, keyEncipherment or keyAgreement, which are the three
///   ways a TLS server certificate is actually used across our supported
///   suites. "Present" matters: RFC 5280 says an ABSENT extension imposes no
///   restriction, so absence must not be read as a mask of zero.
///
///   CA-NESS. Every certificate above the leaf must assert basicConstraints
///   CA:TRUE, and if it carries keyUsage that keyUsage must include
///   keyCertSign. Without the second half, a certificate marked CA:TRUE but
///   restricted to cRLSign could still mint certificates.
///
///   pathLenConstraint. For the CA at index i, the certificates that follow it
///   toward the leaf are indices i-1 down to 0, of which the INTERMEDIATE CAs
///   are indices i-1 down to 1 (index 0 is the end entity and is not an
///   intermediate). So exactly `i - 1` intermediates follow it, and that is what
///   pathLenConstraint bounds. Worked through both directions:
///     [leaf, root(pathlen=0)]                -> i=1, 0 intermediates, 0 <= 0, OK
///     [leaf, inter, root(pathlen=0)]         -> i=2, 1 intermediate,  1 >  0, REJECT
///     [leaf, inter(pathlen=0), root(path=1)] -> i=2, 1 <= 1 OK; i=1, 0 <= 0 OK
///   The middle line is the defect case; the last is the over-rejection guard,
///   and both are in the harness precisely because an off-by-one here would
///   otherwise look like a working fix.
///
/// # Safety
/// `nodes` must point to `count` readable CertNode values.
#[no_mangle]
pub unsafe extern "C" fn cert_chain_policy_rs(nodes: *const CertNode, count: u32) -> i32 {
    if nodes.is_null() || count == 0 {
        return POLICY_NOT_CA;
    }
    let n = count as usize;

    // --- leaf ---
    let leaf = &*nodes;
    if leaf.eku_present != 0 {
        let eku = leaf.eku;
        if eku & (EKU_SERVER_AUTH | EKU_ANY) == 0 {
            return POLICY_BAD_USAGE;
        }
    }
    if leaf.ku_present != 0 {
        let ku = leaf.key_usage;
        if ku & (KU_DIGITAL_SIGNATURE | KU_KEY_ENCIPHERMENT | KU_KEY_AGREEMENT) == 0 {
            return POLICY_BAD_USAGE;
        }
    }

    // --- every CA above the leaf ---
    for i in 1..n {
        let ca = &*nodes.add(i);
        if ca.is_ca == 0 {
            return POLICY_NOT_CA;
        }
        if ca.ku_present != 0 && ca.key_usage & KU_KEY_CERT_SIGN == 0 {
            return POLICY_BAD_USAGE;
        }
        if ca.path_len >= 0 {
            let intermediates_below = (i - 1) as i32;
            if intermediates_below > ca.path_len {
                return POLICY_BAD_PATHLEN;
            }
        }
    }

    POLICY_OK
}
