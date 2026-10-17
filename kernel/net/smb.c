// smb.c - SMB/CIFS Client Implementation for MayteraOS
// Implements SMB2 client with NTLM authentication

#include "smb.h"
#include "tcp.h"
#include "ip.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../crypto/md4.h"
#include "../security/security.h"   // #smbnt: secure_zero()
#include "../crypto/md5.h"
#include "../crypto/hmac.h"
#include "../crypto/crypto.h" // #smbpreauth: sha512_ctx_t, for the size lock below
#include "../cpu/mono.h"   // #499: sched_now_ms() - THE shared real-elapsed-ms clock
#include "../fs/fat.h"     // /CONFIG/SMB.CFG: the signing-policy knob
#include "../fs/cfgread.h" // #192: is this config-read outcome worth a log line?
#include "../security/selftest_registry.h" // a measurement that did not run must SAY so, through the registry

// rustkern/smbpreauth.rs keeps a fixed local scratch of
// SMB_PREAUTH_SHA512_CTX_ROOM bytes for one sha512_ctx_t, because a Rust
// #[repr(C)] struct cannot name a C typedef's size. Locking it HERE means that
// if sha512_ctx_t ever grows, the build stops, rather than the Rust side
// quietly overwriting its own stack on every handshake message.
_Static_assert(sizeof(sha512_ctx_t) <= SMB_PREAUTH_SHA512_CTX_ROOM,
               "sha512_ctx_t outgrew the scratch in rustkern/smbpreauth.rs");

// External timer ticks
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;          // PIT frequency (default 250 Hz)
extern void net_poll(void);          // pump NIC RX/TX + DHCP
extern void tcp_timer(void);         // drive TCP retransmit/timeouts
extern void proc_sleep(uint32_t ms); // yield to scheduler

// task #317: milliseconds used to be converted to timer ticks using the REAL
// timer frequency. The original code assumed 18.2 Hz, but the PIT runs at
// g_timer_hz (250 Hz), so a "5000 ms" timeout was really ~0.36 s.
// #499: every deadline below is now measured on sched_now_ms(), REAL elapsed
// milliseconds, instead of timer_ticks (which counts ticks DELIVERED, so a KVM
// tick burst collapsed each SMB send/recv/connect budget to zero). This helper
// therefore became an identity with a 1ms floor, keeping the call sites intact.
// The filetime/nonce uses of timer_ticks further down are timestamps, NOT
// deadlines, and are deliberately left alone.
static inline uint64_t smb_ms_span(int ms) {
    return ms > 0 ? (uint64_t)ms : 1;
}

// Pump the network stack once and briefly yield. Mirrors the wget/https
// recv loop pattern; this is what was MISSING from the SMB recv/connect
// loops (the NIC was never polled, so no reply was ever delivered).
static inline void smb_net_pump(void) {
    net_poll();
    tcp_timer();
    // Yield to the scheduler (1 ms). This is the critical difference from a
    // busy io_wait spin: in this stack the NIC RX / TCP processing only makes
    // progress when the running context yields, so wget/https use proc_sleep
    // here. Without it, a tight loop never lets the ARP reply / SYN-ACK be
    // delivered and TCP exhausts its SYN retries before ARP resolves.
    proc_sleep(1);
}

// Connection and file tables
static smb_connection_t connections[SMB_MAX_CONNECTIONS];
static smb_file_t open_files[SMB_MAX_OPEN_FILES];

// Packet buffer (large enough for max transaction)
#define SMB_BUFFER_SIZE     (64 * 1024)
static uint8_t smb_send_buffer[SMB_BUFFER_SIZE];
static uint8_t smb_recv_buffer[SMB_BUFFER_SIZE];

// Byte order conversion
static inline uint16_t le16(uint16_t v) { return v; }
static inline uint32_t le32(uint32_t v) { return v; }
static inline uint64_t le64(uint64_t v) { return v; }

// Convert to big-endian for network byte order where needed
static inline uint16_t be16(uint16_t v) {
    return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF);
}

static inline uint32_t be32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

// ============================================================================
// Helper Functions
// ============================================================================

// Generate a random GUID
static void generate_guid(uint8_t *guid) {
    static uint32_t seed = 0x12345678;
    for (int i = 0; i < 16; i++) {
        seed = seed * 1103515245 + 12345;
        guid[i] = (seed >> 16) & 0xFF;
    }
}

// Convert ASCII to Unicode (UTF-16LE)
// NTLM_KAT_HELPER_BEGIN
static int ascii_to_unicode(const char *ascii, uint16_t *unicode, int max_chars) {
    int len = 0;
    while (*ascii && len < max_chars - 1) {
        unicode[len++] = (uint16_t)(unsigned char)*ascii++;
    }
    unicode[len] = 0;
    return len * 2;  // Return byte length
}
// NTLM_KAT_HELPER_END

// Convert Unicode to ASCII
static int unicode_to_ascii(const uint16_t *unicode, int unicode_len, char *ascii, int max_chars) {
    int chars = unicode_len / 2;
    int len = 0;
    while (len < chars && len < max_chars - 1) {
        uint16_t c = unicode[len];
        ascii[len] = (c < 256) ? (char)c : '?';
        len++;
    }
    ascii[len] = 0;
    return len;
}

// Windows FILETIME to Unix timestamp
uint64_t smb_filetime_to_unix(uint64_t filetime) {
    // FILETIME is 100-nanosecond intervals since Jan 1, 1601
    // Unix timestamp is seconds since Jan 1, 1970
    // Difference is 11644473600 seconds (369 years)
    if (filetime < 116444736000000000ULL) {
        return 0;
    }
    return (filetime - 116444736000000000ULL) / 10000000ULL;
}

// Unix timestamp to Windows FILETIME
uint64_t smb_unix_to_filetime(uint64_t unix_time) {
    return (unix_time * 10000000ULL) + 116444736000000000ULL;
}

// Find free connection slot
static int alloc_connection(void) {
    for (int i = 0; i < SMB_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            memset(&connections[i], 0, sizeof(smb_connection_t));
            return i;
        }
    }
    return -1;
}

// Find free file slot
static int alloc_file(void) {
    for (int i = 0; i < SMB_MAX_OPEN_FILES; i++) {
        if (!open_files[i].active) {
            memset(&open_files[i], 0, sizeof(smb_file_t));
            return i;
        }
    }
    return -1;
}

// Get connection by index
static smb_connection_t __attribute__((unused)) *get_connection(int idx) {
    if (idx < 0 || idx >= SMB_MAX_CONNECTIONS) return NULL;
    if (!connections[idx].active) return NULL;
    return &connections[idx];
}

// Get file by index
static smb_file_t *get_file(int fd) {
    if (fd < 0 || fd >= SMB_MAX_OPEN_FILES) return NULL;
    if (!open_files[fd].active) return NULL;
    return &open_files[fd];
}

// ============================================================================
// TCP Communication
// ============================================================================

// Robust send-all: pumps the network stack on WOULD_BLOCK / partial sends so
// the bytes actually leave the box (task #317). Returns 0 on success, -1 on
// timeout/error.
static int smb_send_all(smb_connection_t *conn, const uint8_t *p, uint32_t len) {
    uint32_t sent = 0;
    uint64_t start = sched_now_ms();
    uint64_t deadline_ms = smb_ms_span(10000);
    while (sent < len) {
        uint16_t chunk = (len - sent) > 1400 ? 1400 : (uint16_t)(len - sent);
        int s = tcp_send(conn->tcp_socket, p + sent, chunk);
        if (s > 0) {
            sent += s;
            start = sched_now_ms();
            smb_net_pump();
        } else if (s == TCP_ERR_WOULD_BLOCK) {
            smb_net_pump();
        } else {
            return -1;  // hard error
        }
        if (sched_now_ms() - start > deadline_ms) {
            kprintf("[SMB] send timeout (%u/%u)\n", sent, len);
            return -1;
        }
    }
    return 0;
}

// Send SMB packet with NetBIOS session header
//
// `data` is non-const because SIGNING MUTATES THE MESSAGE: MS-SMB2 3.1.4.1
// requires the Signature field to be written into the header that is about to
// go on the wire. Every caller passes smb_send_buffer, which is writable.
static int smb_send_packet(smb_connection_t *conn, void *data, uint32_t len) {
    // #749: ONE chokepoint for "this connection is not in a state to be used".
    // Checking here rather than at each of the eleven build_smb2_header()
    // callers is the whole point: a new SMB2 command added later is covered
    // without its author having to know the rule exists.
    uint16_t sf = smbseq_fault(&conn->seq);
    if (sf != SMB_SEQ_FAULT_NONE) {
        // This line used to end "This is a PROTOCOL SEQUENCING fault on socket
        // %d, not a credential problem". That was true of all four faults that
        // existed when it was written and became FALSE the moment
        // SMB_SEQ_FAULT_SIGNATURE was added: a response that failed its
        // signature is not a sequencing fault. Classifying the fault inside the
        // sentence, rather than letting smbseq_fault_text() speak, is the same
        // mistake one layer up as the #749 misdiagnosis this whole family of
        // messages exists to avoid. It now states only what it knows.
        kprintf("[SMB] NOT SENDING: this connection is marked bad on socket %d: "
                "%s. Nothing further goes out on it; the share must be "
                "reconnected.\n",
                conn->tcp_socket, smbseq_fault_text(sf));
        return -1;
    }

    // THE OUTBOUND SIGNING CHOKEPOINT.
    //
    // It sits here, in the one function every SMB2 request leaves through,
    // for the same reason the sequence check above does: there are eleven
    // build_smb2_header() callers and a twelfth will be added by someone who
    // has not read this file. Signing per command would mean the twelfth
    // command ships unsigned and nothing says so.
    //
    // smbsign_sign() returns R_NOT_ARMED (1) for NEGOTIATE, for SESSION_SETUP,
    // and for any connection that has no session key yet. That is the correct
    // answer, not a failure: those messages precede the key. Any OTHER
    // non-zero code is a refusal to produce a signature we are obliged to
    // produce, and sending unsigned in that case is precisely the silent
    // downgrade this work exists to remove.
    // THE OUTBOUND ENCRYPTION CHOKEPOINT, in the same function and for the
    // same reason as the signing one above: there are eleven
    // build_smb2_header() callers and a twelfth will be added by someone who
    // has not read this file. Encrypting per command would mean the twelfth
    // command ships readable and nothing says so.
    int sealing = smbcrypt_should_seal(&conn->crypt, (const uint8_t *)data,
                                       (size_t)len);

    // A MESSAGE THAT IS ENCRYPTED IS NOT ALSO SIGNED, and that is the spec,
    // not an omission: MS-SMB2 3.2.4.1.1 signs a request only when it is not
    // being encrypted, and MEASURED here, Samba's encrypted replies carry no
    // SMB2_FLAGS_SIGNED and an all-zero Signature field. The AEAD tag already
    // authenticates the whole message, including its SessionId, under a key
    // derived from the same session key, so a second MAC would add nothing.
    // The sentence to be suspicious of is "the signature check was skipped",
    // so: it is skipped here ONLY when something stronger covers the message.
    if (len >= SMB2_HEADER_SIZE && !sealing) {
        uint16_t sr = smbsign_sign(&conn->sign, (uint8_t *)data, (size_t)len);
        if (sr != 0 && sr != 1 /* not-armed */) {
            kprintf("[SMB] NOT SENDING: could not sign this request (%s). The "
                    "request is DROPPED rather than sent unsigned.\n",
                    smbsign_reason_text(sr));
            return -1;
        }
    }

    // Encryption happens IN PLACE, with the 52-byte transform header built
    // separately on the stack. The alternative, a second SMB_BUFFER_SIZE
    // staging buffer, would cost another 64 KB of permanently resident kernel
    // memory to hold a copy of something we already have.
    uint8_t tform[SMB2_TRANSFORM_HEADER_SIZE];
    uint32_t wire_len = len;
    if (sealing) {
        uint16_t er = smbcrypt_seal(&conn->crypt, (uint8_t *)data, (size_t)len,
                                    tform);
        if (er != 0) {
            kprintf("[SMB] NOT SENDING: could not encrypt this request (%s). "
                    "The request is DROPPED rather than sent in the clear.\n",
                    smbcrypt_reason_text(er));
            return -1;
        }
        wire_len = len + SMB2_TRANSFORM_HEADER_SIZE;
    }

    // NetBIOS session message header (4 bytes)
    uint8_t header[4];
    header[0] = 0x00;                        // Session message
    header[1] = (wire_len >> 16) & 0xFF;     // Length high byte
    header[2] = (wire_len >> 8) & 0xFF;
    header[3] = wire_len & 0xFF;

    if (smb_send_all(conn, header, 4) < 0) {
        kprintf("[SMB] Failed to send NetBIOS header\n");
        return -1;
    }
    if (sealing && smb_send_all(conn, tform, SMB2_TRANSFORM_HEADER_SIZE) < 0) {
        kprintf("[SMB] Failed to send the SMB2 TRANSFORM header\n");
        return -1;
    }
    if (smb_send_all(conn, (const uint8_t *)data, len) < 0) {
        kprintf("[SMB] Failed to send data (%u bytes)\n", len);
        return -1;
    }
    return len;
}

// Receive one SMB PDU off the wire (NetBIOS length prefix + payload).
// #749: this is the RAW read. The MessageId gate is in smb_recv_packet() below;
// the two deadline loops in here are unchanged on purpose (see the note there).
static int smb_recv_pdu(smb_connection_t *conn, void *buffer, uint32_t max_len, int timeout_ms) {
    uint8_t header[4];
    int total_recv = 0;
    uint64_t start = sched_now_ms();
    uint64_t budget_ms = smb_ms_span(timeout_ms);

    // Receive NetBIOS header. CRITICAL (task #317): pump the NIC each
    // iteration; tcp_recv only returns data that a prior net_poll buffered.
    while (total_recv < 4) {
        smb_net_pump();
        int r = tcp_recv(conn->tcp_socket, header + total_recv, 4 - total_recv);
        if (r > 0) {
            total_recv += r;
            start = sched_now_ms();
        } else if (r < 0 && r != TCP_ERR_WOULD_BLOCK) {
            return -1;
        }
        if (sched_now_ms() - start > budget_ms) {
            kprintf("[SMB] Receive timeout waiting for header\n");
            return -1;
        }
    }

    // Parse length
    uint32_t len = ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];
    if (len > max_len) {
        kprintf("[SMB] Packet too large: %u > %u\n", len, max_len);
        return -1;
    }

    // Receive data. tcp_recv length arg is uint16_t, so chunk large payloads.
    total_recv = 0;
    start = sched_now_ms();
    while ((uint32_t)total_recv < len) {
        smb_net_pump();
        uint32_t want = len - total_recv;
        if (want > 32000) want = 32000;
        int r = tcp_recv(conn->tcp_socket, (uint8_t*)buffer + total_recv, (uint16_t)want);
        if (r > 0) {
            total_recv += r;
            start = sched_now_ms();
        } else if (r < 0 && r != TCP_ERR_WOULD_BLOCK) {
            return -1;
        }
        if (sched_now_ms() - start > budget_ms) {
            kprintf("[SMB] Receive timeout waiting for data (%d/%u)\n", total_recv, len);
            return -1;
        }
    }

    return total_recv;
}

// #749: the MessageId gate on the receive side.
//
// smb_recv_pdu() above is unchanged, deliberately: its two deadline loops are
// reviewed entries in the concurrency lint's allowlist and this change has no
// business altering them. Everything new lives out here, where it can be read
// as one rule instead of being threaded through a polling loop.
//
// TWO things were missing before, and they compound:
//
//   1. Nothing checked that a response was the answer to the request we sent.
//      A connection the server had killed over a sequencing fault, or one whose
//      reply stream was offset by a timed-out request, handed the caller
//      someone else's status. smb2_session_setup() then printed it as
//      "Authentication failed: 0x%08x", and 0xC000006D really is
//      STATUS_LOGON_FAILURE, so the message was not even obviously wrong. That
//      cost an hour of looking at passwords before anyone read the server log.
//
//   2. A request that timed out left the connection usable. The reply may
//      still arrive, after which every response is one request behind forever.
//      A timeout therefore CONDEMNS the connection here, which is what makes
//      the next operation reconnect rather than inherit the offset.
// How many interim (STATUS_PENDING + ASYNC) responses to ride out before giving
// up on a request. Matches the guard smb2_read() already used for the \srvsvc
// pipe, so this is not a new number, it is the same one moved to where it
// covers every command.
#define SMB_MAX_INTERIM_RESPONSES 16

static int smb_recv_packet(smb_connection_t *conn, void *buffer, uint32_t max_len,
                           int timeout_ms) {
    for (int interim = 0; interim <= SMB_MAX_INTERIM_RESPONSES; interim++) {
        int n = smb_recv_pdu(conn, buffer, max_len, timeout_ms);
        if (n < 0) {
            if (smbseq_fault(&conn->seq) == SMB_SEQ_FAULT_NONE) {
                smbseq_condemn(&conn->seq, SMB_SEQ_FAULT_REPLY_ID);
                kprintf("[SMB] no answer to MessageId %llu; the reply stream "
                        "would be offset from here on, so this connection is "
                        "marked bad and must be reconnected\n",
                        (unsigned long long)smbseq_last_sent(&conn->seq));
            }
            return n;
        }
        // THE INBOUND DECRYPTION CHOKEPOINT, and it is NECESSARILY above the
        // SMB2 magic check below. An encrypted message has no SMB2 header to
        // look at: its first four bytes are 0xFD'SMB', not 0xFE'SMB', so
        // before this existed such a frame fell straight through that test
        // and was handed up as an unparseable reply.
        int was_encrypted = 0;
        if (smbcrypt_is_armed(&conn->crypt)
            && smbcrypt_pdu_is_transform((const uint8_t *)buffer, (size_t)n)) {
            uint32_t plain_len = 0;
            uint16_t ur = smbcrypt_unseal(&conn->crypt, (uint8_t *)buffer,
                                          (size_t)n, &plain_len);
            if (ur != 0) {
                uint32_t nse = 0, nun = 0, nrj = 0, nco = 0, nci = 0;
                smbcrypt_counts(&conn->crypt, &nse, &nun, &nrj, &nco, &nci);
                kprintf("[SMB] ENCRYPTED RESPONSE REJECTED on %s: %s\n",
                        conn->mount_point[0] ? conn->mount_point : "this share",
                        smbcrypt_reason_text(ur));
                kprintf("[SMB]   %d bytes on the wire, encryption=%s. The "
                        "contents are DISCARDED and the connection is marked "
                        "bad. (this connection: %u sealed, %u unsealed, %u "
                        "rejected, %u sent clear, %u received clear)\n",
                        n, smbcrypt_algo_text(&conn->crypt),
                        nse, nun, nrj, nco, nci);
                kprintf("[SMB]   This is NOT a wrong password, NOT a dropped "
                        "connection and NOT a sequencing fault. Either the "
                        "bytes that arrived are not the bytes the server sent, "
                        "or they did not come from a server holding this "
                        "session's key.\n");
                if (smbseq_fault(&conn->seq) == SMB_SEQ_FAULT_NONE) {
                    smbseq_condemn(&conn->seq, SMB_SEQ_FAULT_ENCRYPTION);
                }
                return -1;
            }
            n = (int)plain_len;
            was_encrypted = 1;
        }

        if ((uint32_t)n < SMB2_HEADER_SIZE) return n;

        const smb2_header_t *rh = (const smb2_header_t *)buffer;
        if (le32(rh->protocol_id) != SMB2_MAGIC) return n;

        // THE ANTI-DOWNGRADE GATE ON THE RECEIVE SIDE.
        //
        // Once a session is encrypted, a response that arrives in the CLEAR
        // must not be acted on. This is the same defect shape as the guest
        // downgrade closed the day before: a protection an attacker can strip
        // and still be believed is decorative. NEGOTIATE and SESSION_SETUP
        // are the only exceptions, because they precede every key.
        if (!was_encrypted) {
            uint16_t clr = smbcrypt_check_clear_response(
                &conn->crypt, (const uint8_t *)buffer, (size_t)n);
            if (clr != 0) {
                kprintf("[SMB] CLEARTEXT RESPONSE REFUSED on %s: %s\n",
                        conn->mount_point[0] ? conn->mount_point : "this share",
                        smbcrypt_reason_text(clr));
                kprintf("[SMB]   cmd 0x%04x, MessageId %llu, status 0x%08x, %d "
                        "bytes. This session is encrypted, so an unencrypted "
                        "answer either did not come from the server or was "
                        "stripped in flight. It is DISCARDED and the "
                        "connection is marked bad.\n",
                        le16(rh->command),
                        (unsigned long long)le64(rh->message_id),
                        le32(rh->status), n);
                if (smbseq_fault(&conn->seq) == SMB_SEQ_FAULT_NONE) {
                    smbseq_condemn(&conn->seq, SMB_SEQ_FAULT_ENCRYPTION);
                }
                return -1;
            }
        }

        // THE INBOUND SIGNATURE CHOKEPOINT, and it is deliberately the FIRST
        // thing that happens to a response after the magic check.
        //
        // Nothing below here may read a field of a response whose signature
        // has not been checked, which is why this sits ABOVE the MessageId
        // gate rather than after it. The MessageId is itself inside the signed
        // region, so verifying first means a forged MessageId is reported as
        // what it is, a bad signature, instead of being diagnosed as a
        // sequencing desync. #749's whole lesson was that a wrong diagnosis in
        // an error message is a defect in its own right.
        //
        // PRODUCING SIGNATURES WITHOUT CHECKING THEM WOULD BE THE WHOLE BUG.
        // An implementation that signs its own requests and trusts whatever
        // comes back has the appearance of protection and none of it: the
        // server is still unauthenticated, and every forged read result, every
        // forged directory listing and every forged write status still lands.
        // This is the half that actually closes the hole.
        // `!was_encrypted`: an encrypted message carries no signature and
        // is not supposed to (see the send path). Demanding one here would
        // refuse every response on an encrypted mount.
        if (!was_encrypted && smbsign_is_armed(&conn->sign)) {
            uint16_t vr = smbsign_check_response(&conn->sign, (uint8_t *)buffer,
                                                 (size_t)n);
            if (vr != 0) {
                uint32_t nsig = 0, nver = 0, nrej = 0, nuns = 0;
                smbsign_counts(&conn->sign, &nsig, &nver, &nrej, &nuns);
                kprintf("[SMB] RESPONSE REJECTED on %s: %s\n",
                        conn->mount_point[0] ? conn->mount_point : "this share",
                        smbsign_reason_text(vr));
                kprintf("[SMB]   cmd 0x%04x, MessageId %llu, status 0x%08x, "
                        "SessionId 0x%llx, %d bytes, signing=%s\n",
                        le16(rh->command),
                        (unsigned long long)le64(rh->message_id),
                        le32(rh->status),
                        (unsigned long long)smbsign_pdu_session_id(
                            (const uint8_t *)buffer, (size_t)n),
                        n, smbsign_algo_text(smbsign_algo(&conn->sign)));
                kprintf("[SMB]   This is NOT a wrong password, NOT a dropped "
                        "connection and NOT a sequencing fault. The bytes that "
                        "arrived are not the bytes the server sent, or they did "
                        "not come from a server holding this session's key. The "
                        "contents are DISCARDED and the connection is marked "
                        "bad. (this connection: %u requests signed, %u "
                        "responses verified, %u rejected, %u accepted UNSIGNED "
                        "under signing=enabled)\n",
                        nsig, nver, nrej, nuns);
                // Condemn the connection. A peer that is altering or
                // withholding signatures is not a peer we carry on talking to
                // for the remaining commands of this mount, and the existing
                // revive path (smb_conn_revive) is the honest way back: it
                // re-authenticates and derives a fresh key.
                if (smbseq_fault(&conn->seq) == SMB_SEQ_FAULT_NONE) {
                    smbseq_condemn(&conn->seq, SMB_SEQ_FAULT_SIGNATURE);
                }
                return -1;
            }
        }

        uint64_t rid = le64(rh->message_id);
        int cr = smbseq_check_reply(&conn->seq, rid, le32(rh->flags),
                                    le32(rh->status));
        if (cr < 0) {
            kprintf("[SMB] MessageId MISMATCH: response carries %llu but we "
                    "sent %llu (cmd 0x%04x, status 0x%08x). The connection's "
                    "message sequence has desynchronised, so that status is NOT "
                    "an answer about our request and must not be reported as "
                    "one.\n",
                    (unsigned long long)rid,
                    (unsigned long long)smbseq_last_sent(&conn->seq),
                    le16(rh->command), le32(rh->status));
            return -1;
        }

        if (cr == 1) {
            // An unsolicited server message: MS-SMB2 2.2.1.2 gives an oplock
            // break MessageId 0xFFFFFFFFFFFFFFFF. Not a desync and not ours.
            // We request SMB2_OPLOCK_LEVEL_NONE (smb2_create below), so a
            // conforming server has nothing to break; it is logged and handed
            // up, which is the pre-existing behaviour, rather than silently
            // swallowed.
            kprintf("[SMB] unsolicited server message (cmd 0x%04x); not a "
                    "reply to anything we sent\n", le16(rh->command));
            return n;
        }

        smbseq_note_grant(&conn->seq, le16(rh->credit_req_resp));

        if (cr == 2) {
            // Interim response: the server has our request and is still on it
            // (MS-SMB2 3.3.4.2). Keep receiving; the real answer carries the
            // same MessageId.
            //
            // This used to be handled in smb2_read() ALONE, added by #317 pass
            // 2 for the \srvsvc pipe because that is where it was hit. Every
            // other SMB2 command would have mistaken the interim for the
            // answer. Doing it here means a command added later inherits it.
            continue;
        }

        return n;   // cr == 0: the final answer to the request we sent
    }

    kprintf("[SMB] gave up after %d interim responses to MessageId %llu\n",
            SMB_MAX_INTERIM_RESPONSES,
            (unsigned long long)smbseq_last_sent(&conn->seq));
    smbseq_condemn(&conn->seq, SMB_SEQ_FAULT_REPLY_ID);
    return -1;
}

// ============================================================================
// NTLM Authentication
// ============================================================================

// NTLM_KAT_REGION_BEGIN
// Everything between these markers is extracted VERBATIM by
// crypto/ntlm_nthash_kat.sh and compiled against the real crypto/md4.c and
// crypto/hmac.c, so the oracle cannot drift from what ships. Keep it free of
// kernel-only dependencies (no kprintf, no kmalloc) or the oracle stops
// building, which is the point: this is the SMB auth trust anchor.

// Compute NTLM hash from password (MD4 of Unicode password).
// This is the ONLY place a cleartext SMB password is consumed. Everything
// downstream needs these 16 bytes and nothing more, which is what lets the
// connection struct and the VFS defaults store a hash instead (#smbnt).
static void ntlm_hash(const char *password, uint8_t *hash) {
    uint16_t unicode_pwd[128];
    int len = ascii_to_unicode(password, unicode_pwd, 128);
    md4((uint8_t*)unicode_pwd, len, hash);
}

// NTOWFv2: HMAC-MD5(NT hash, UPPERCASE(username) + domain), both UTF-16LE.
// Extracted from ntlm_compute_ntlmv2_response so it can be tested on its own
// against the published MS-NLMP 4.2.4 vector; it had been inline, which meant
// the one step that decides whether authentication succeeds had no test.
static void ntlm_v2_hash(const char *domain, const char *username,
                         const uint8_t *nt_hash, uint8_t *out) {
    uint16_t concat[128];
    char upper_user[64];
    int i;

    for (i = 0; username[i] && i < 63; i++) {
        upper_user[i] = (username[i] >= 'a' && username[i] <= 'z')
                        ? username[i] - 32 : username[i];
    }
    upper_user[i] = 0;

    int concat_len = ascii_to_unicode(upper_user, concat, 64);
    concat_len += ascii_to_unicode(domain, concat + concat_len / 2, 64);

    hmac_md5(nt_hash, 16, (uint8_t*)concat, concat_len, out);
}
// NTLM_KAT_REGION_END

// ---------------------------------------------------------------------------
// #smbdisk: the ON-DISK form of a saved credential, and the one place a
// password is turned into it.
//
// rustkern/ntsecret.rs owns the parse/format of `nt$<32 hex>`. It is the only
// part of this change that consumes attacker-influenceable bytes (a line of
// /CONFIG/NETMOUNTS.CFG), it is pure, and it needs no kernel services, so it is
// the part that is in Rust per the standing directive. See its header comment
// for the strictness rules and for WHY a tagged-but-malformed record is refused
// loudly rather than quietly treated as a password.
// ---------------------------------------------------------------------------
extern int nt_secret_is_tagged(const char *s);
extern int nt_secret_parse(const char *s, uint8_t *out16);
extern int nt_secret_format(const uint8_t *h, char *out, unsigned long cap);
extern int nt_secret_selftest_rs(void);
#define NT_SECRET_BUF 40   /* "nt$" + 32 hex + NUL = 36 */

// Public NT-hash derivation. Exactly one caller outside this file needs it:
// SYS_NET_CRED_TAG, which lets the Files app persist a hash instead of the
// password the user just typed. ntlm_hash() itself stays static and stays
// INSIDE the KAT region, so there is still exactly one derivation in the tree
// and crypto/ntlm_nthash_kat.sh still tests the thing that ships.
void smb_nt_hash(const char *password, uint8_t *out16) {
    ntlm_hash(password ? password : "", out16);
}

// Turn whatever a config line or a syscall handed us into the 16 bytes the
// protocol actually needs.
//   returns 1 = ok (out16 written), 0 = REFUSE (tagged nt$ but malformed).
// It deliberately does NOT also report "that was a legacy password": the
// migration path needs that answer per LINE of the file, not per mount, so it
// asks nt_secret_is_tagged() directly. An output parameter with no caller is
// prose that looks like code.
static int smb_secret_to_nthash(const char *secret, uint8_t *out16) {
    if (!secret) secret = "";
    if (nt_secret_is_tagged(secret)) {
        if (nt_secret_parse(secret, out16)) return 1;
        // Deliberately NOT a fallback. Treating a corrupt nt$ field as a
        // password would try to authenticate with the literal string "nt$..."
        // and present as a mystery logon failure, hiding a corrupt file.
        kprintf("[SMB] REFUSING a saved credential: field is tagged nt$ but "
                "malformed. Not falling back to treating it as a password.\n");
        return 0;
    }
    ntlm_hash(secret, out16);
    return 1;
}

// Boot self-test for the on-disk encoding. Runs on EVERY boot including the
// golden: crypto/run-all-kats.sh cannot host it (its oracles compile
// crypto/*.c with the host cc, this is Rust in the kernel crate), so the boot
// path is what makes it un-rottable. blame.md's standing lesson is that an
// inert check and no check are indistinguishable from outside.
void nt_secret_selftest(void) {
    int rc = nt_secret_selftest_rs();
    if (rc == 0)
        kprintf("[NTSECRET] PASS: nt$<32hex> round-trips the published MS-NLMP "
                "NTOWFv1 vector and rejects all 10 malformed forms\n");
    else
        kprintf("[NTSECRET] FAIL: case %d (see rustkern/ntsecret.rs "
                "nt_secret_selftest_rs for which rule that is)\n", rc);
}

// Compute NTLMv1 response
void ntlm_compute_response(const uint8_t *nt_hash, const uint8_t *challenge,
                           uint8_t *lm_response, uint8_t *nt_response) {

    // For NTLMv1, we use DES-based response, but for simplicity
    // we'll use a simpler approach compatible with many servers
    // that accept NTLMv2 or negotiate down

    // Zero-pad the hash to 21 bytes for DES keys
    uint8_t key_material[21];
    memset(key_material, 0, 21);
    memcpy(key_material, nt_hash, 16);

    // LM response (24 bytes) - for compatibility, send zeros or same as NT
    memset(lm_response, 0, 24);

    // NT response using simplified hash (not full DES for bare metal)
    // This works with servers that support NTLMv2 negotiation
    for (int i = 0; i < 24; i++) {
        nt_response[i] = nt_hash[i % 16] ^ challenge[i % 8];
    }
}

// Compute NTLMv2 response (more secure, widely supported)
void ntlm_compute_ntlmv2_response(const char *domain, const char *username,
                                   const uint8_t *nt_hash, const uint8_t *challenge,
                                   const uint8_t *target_info, uint32_t target_info_len,
                                   uint8_t *response, uint32_t *response_len,
                                   uint8_t *session_key) {
    uint8_t ntlmv2_hash[16];
    int i;   // used by the blob/response assembly below

    // Step 1 (the NT hash) happened at mount time; we never see the password.

    // Step 2: NTOWFv2 (see ntlm_v2_hash, which the KAT oracle tests directly).
    ntlm_v2_hash(domain, username, nt_hash, ntlmv2_hash);

    // Step 3: Build client blob
    uint8_t blob[256];
    uint32_t blob_len = 0;

    // Blob signature (0x01010000)
    blob[blob_len++] = 0x01;
    blob[blob_len++] = 0x01;
    blob[blob_len++] = 0x00;
    blob[blob_len++] = 0x00;

    // Reserved (0x00000000)
    memset(blob + blob_len, 0, 4);
    blob_len += 4;

    // Timestamp (current time as FILETIME)
    uint64_t timestamp = smb_unix_to_filetime(timer_ticks / 18);  // Approximate
    memcpy(blob + blob_len, &timestamp, 8);
    blob_len += 8;

    // Client nonce (8 bytes random)
    for (i = 0; i < 8; i++) {
        blob[blob_len++] = (timer_ticks >> (i * 4)) & 0xFF;
    }

    // Reserved (0x00000000)
    memset(blob + blob_len, 0, 4);
    blob_len += 4;

    // Target info (if provided)
    if (target_info && target_info_len > 0 && target_info_len < 200) {
        memcpy(blob + blob_len, target_info, target_info_len);
        blob_len += target_info_len;
    }

    // Reserved (0x00000000)
    memset(blob + blob_len, 0, 4);
    blob_len += 4;

    // Step 4: Compute NTProofStr = HMAC-MD5(NTLMv2 hash, challenge + blob)
    uint8_t temp[8 + 256];
    memcpy(temp, challenge, 8);
    memcpy(temp + 8, blob, blob_len);

    uint8_t nt_proof[16];
    hmac_md5(ntlmv2_hash, 16, temp, 8 + blob_len, nt_proof);

    // Step 5: Build response = NTProofStr + blob
    memcpy(response, nt_proof, 16);
    memcpy(response + 16, blob, blob_len);
    *response_len = 16 + blob_len;

    // Step 6: THE SESSION KEY, which this function used to throw away.
    //
    // MS-NLMP 3.3.2: for NTLMv2 the SessionBaseKey is
    // HMAC-MD5(NTOWFv2, NTProofStr). Because smb2_session_setup() does not
    // negotiate NTLMSSP_NEGOTIATE_KEY_EXCH, there is no EncryptedRandomSessionKey
    // and the ExportedSessionKey IS the SessionBaseKey (MS-NLMP 3.1.5.1.2). That
    // is the 16 bytes SMB2 message signing is keyed from.
    //
    // Every input needed for it was already sitting in this function as a
    // local; it was computed, used for the response, and dropped. That single
    // omission is why net/smb.c had a `session_key` field nothing wrote and a
    // `signing_required` field nothing read.
    if (session_key) {
        hmac_md5(ntlmv2_hash, 16, nt_proof, 16, session_key);
    }

    // The NTLMv2 hash is password-equivalent; do not leave it on the stack for
    // whatever runs next in this kernel thread.
    secure_zero(ntlmv2_hash, sizeof(ntlmv2_hash));
    secure_zero(temp, sizeof(temp));
}

// ============================================================================
// SMB2 Protocol Operations
// ============================================================================

// Build SMB2 header
//
// #749: the MessageId comes from the connection's sequence, which refuses to
// number a request at all when it is not bound to the socket we are about to
// send on. On a refusal the sequence records a FAULT and smb_send_packet()
// declines to transmit, so a desynchronised request never reaches the server.
// Nothing here invents an id to "keep going": a request the server would kill
// the connection over is worse than a request that is not sent.
static void build_smb2_header(smb_connection_t *conn, smb2_header_t *hdr,
                               uint16_t command, uint32_t tree_id) {
    memset(hdr, 0, sizeof(smb2_header_t));
    hdr->protocol_id = le32(SMB2_MAGIC);
    hdr->structure_size = le16(64);
    hdr->credit_charge = le16(1);
    hdr->command = le16(command);
    hdr->credit_req_resp = le16(31);  // Request credits
    int64_t mid = smbseq_next(&conn->seq, conn->tcp_socket);
    hdr->message_id = le64(mid < 0 ? 0 : (uint64_t)mid);
    hdr->tree_id = le32(tree_id);
    hdr->session_id = le64(conn->session_id);
}

// SMB2 Negotiate
int smb2_negotiate(smb_connection_t *conn) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    // Build header
    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_NEGOTIATE, 0);
    offset += SMB2_HEADER_SIZE;

    // Build negotiate request
    smb2_negotiate_req_t *req = (smb2_negotiate_req_t *)(buf + offset);
    req->structure_size = le16(36);
    // SMB 3.1.1 (0x0311) is now offered, which is what makes the opening
    // handshake authenticatable at all. Offering it costs nothing: a server
    // that does not support it simply selects something lower, and whether
    // that lower dialect is ACCEPTABLE is a separate decision taken by
    // smbpreauth_floor_check() below. Keeping those two apart is what stops
    // "we never asked" from passing itself off as "it was not available".
    const int smb_dialect_count = smbpreauth_offer_311() ? 5 : 4;
    req->dialect_count = le16((uint16_t)smb_dialect_count);
    // ANNOUNCE OUR OWN SIGNING REQUIREMENT, not just our capability.
    //
    // This field used to be a flat SMB2_NEGOTIATE_SIGNING_ENABLED, which says
    // "I can sign" and nothing more. A server set to `server signing = no`
    // (Samba's shipping default for a standalone server) hears that as "not
    // required" and answers unsigned, which measured out exactly that way on
    // the test rig. Setting SIGNING_REQUIRED here is what makes a merely
    // capable server actually sign its replies. The bit comes from policy, so
    // the weaker policy still only announces ENABLED.
    req->security_mode = le16(smbsign_client_security_mode());
    req->reserved = 0;
    // SMB2_GLOBAL_CAP_ENCRYPTION comes from the encryption policy, and it is
    // LOAD-BEARING rather than decorative: MEASURED against Samba 4.17.12,
    // the server sets that bit in its own NEGOTIATE response only if we set
    // it here (capabilities 0x47 with it, 0x07 without). Without this line
    // the server never offers encryption and a required-encryption policy
    // would refuse every mount for a reason that was our own doing.
    req->capabilities = le32(SMB2_GLOBAL_CAP_LARGE_MTU |
                             smbcrypt_client_capabilities());
    generate_guid(req->client_guid);
    // Zero by default; overwritten below if 3.1.1 contexts are appended.
    req->negotiate_context_offset = 0;
    req->negotiate_context_count = 0;
    req->reserved2 = 0;
    offset += sizeof(smb2_negotiate_req_t);

    // Add dialect array
    uint16_t *dialects = (uint16_t *)(buf + offset);
    dialects[0] = le16(SMB2_PROTOCOL);      // SMB 2.0.2
    dialects[1] = le16(SMB21_PROTOCOL);     // SMB 2.1
    dialects[2] = le16(SMB30_PROTOCOL);     // SMB 3.0
    dialects[3] = le16(SMB302_PROTOCOL);    // SMB 3.0.2
    if (smb_dialect_count >= 5) {
        dialects[4] = le16(SMB311_PROTOCOL);  // SMB 3.1.1
    }
    offset += 2 * smb_dialect_count;

    // ========================================================================
    // THE NEGOTIATE CONTEXTS. This is where pre-auth integrity starts.
    //
    // SMB 3.1.1 carries its extra negotiation in contexts appended after the
    // dialect array, and NegotiateContextOffset is counted FROM THE START OF
    // THE SMB2 HEADER (MEASURED off a real smbclient capture, not assumed from
    // the field's name) and must be 8-byte aligned, so the message needs
    // padding here. rustkern/smbpreauth.rs builds the blob and reports how many
    // pad bytes to insert, rather than this file computing an alignment that
    // the module would then have to agree with.
    //
    // The salt is 32 bytes of real randomness and is drawn HERE, because the
    // Rust side is deliberately a pure function of its inputs so the boot
    // self-test can pin it to a known-answer vector. A key-schedule input that
    // comes from a global RNG is a key-schedule input you cannot write a test
    // for.
    // ========================================================================
    if (smb_dialect_count >= 5) {
        uint8_t salt[32];
        if (rng_get_bytes(salt, sizeof(salt)) != 0) {
            // Without randomness we cannot build an honest PREAUTH context.
            // Fall back to NOT offering 3.1.1 rather than sending a predictable
            // salt, which would look like protection and be worth less than
            // none. The floor then decides whether the resulting lower dialect
            // is acceptable, so this degrades loudly, not quietly.
            kprintf("[SMB] no entropy for the SMB 3.1.1 pre-auth salt; NOT "
                    "offering 3.1.1 on this connection\n");
            req->dialect_count = le16(4);
            offset -= 2;
        } else {
            uint32_t ctx_pad = 0;
            uint16_t ctx_count = 0;
            int32_t ctx_len = smbpreauth_build_contexts(
                &conn->preauth, salt, sizeof(salt), (size_t)offset,
                buf + offset + 8, (size_t)(SMB_BUFFER_SIZE - offset - 8),
                &ctx_pad, &ctx_count);
            secure_zero(salt, sizeof(salt));
            if (ctx_len < 0) {
                kprintf("[SMB] could not build the SMB 3.1.1 negotiate "
                        "contexts: %s\n",
                        smbpreauth_reason_text((uint16_t)(-ctx_len)));
                return -1;
            }
            // build_contexts wrote at offset+8 so the blob could be moved into
            // place after the pad length was known without a second buffer.
            if (ctx_pad > 0) {
                memmove(buf + offset + ctx_pad, buf + offset + 8,
                        (size_t)ctx_len);
                memset(buf + offset, 0, ctx_pad);
            } else {
                memmove(buf + offset, buf + offset + 8, (size_t)ctx_len);
            }
            req->negotiate_context_offset = le32((uint32_t)offset + ctx_pad);
            req->negotiate_context_count = le16(ctx_count);
            offset += (int)ctx_pad + ctx_len;
        }
    }

    kprintf("[SMB] Sending NEGOTIATE request\n");

    // Fold the request we are about to send into the running pre-auth hash.
    // It has to happen HERE, on the exact bytes that go on the wire, and not
    // from a reconstruction afterwards: the server hashes what it receives, so
    // anything we hash that differs by one byte gives us a different key and
    // no way to tell why.
    //
    // Note it is folded BEFORE smb_send_packet(), because that function may
    // sign or seal the buffer in place, and what the far end folds is the
    // pre-signature bytes. For NEGOTIATE neither applies (no key exists yet),
    // but relying on that would make this correct by coincidence.
    if (smb_dialect_count >= 5) {
        uint16_t fr = smbpreauth_fold(&conn->preauth, buf, (size_t)offset);
        if (fr != 0) {
            kprintf("[SMB] pre-auth integrity: could not fold the NEGOTIATE "
                    "request: %s\n", smbpreauth_reason_text(fr));
            return -1;
        }
    }

    // Send request
    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    // Receive response
    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_negotiate_resp_t))) {
        kprintf("[SMB] Invalid negotiate response length: %d\n", recv_len);
        return -1;
    }

    // Parse response
    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    if (le32(resp_hdr->protocol_id) != SMB2_MAGIC) {
        kprintf("[SMB] Invalid protocol ID in response\n");
        return -1;
    }

    if (le32(resp_hdr->status) != STATUS_SUCCESS) {
        kprintf("[SMB] Negotiate failed with status 0x%08x\n", le32(resp_hdr->status));
        return -1;
    }

    smb2_negotiate_resp_t *resp = (smb2_negotiate_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);

    // Store negotiated parameters
    conn->dialect = le16(resp->dialect_revision);
    conn->max_transact_size = le32(resp->max_transact_size);
    conn->max_read_size = le32(resp->max_read_size);
    conn->max_write_size = le32(resp->max_write_size);
    conn->capabilities = le32(resp->capabilities);
    memcpy(conn->server_guid, resp->server_guid, 16);
    conn->signing_required = (le16(resp->security_mode) & SMB2_NEGOTIATE_SIGNING_REQUIRED) != 0;
    // #749: the credit grant is recorded in ONE place now, by
    // smb_recv_packet() for every response, instead of at eight
    // separate sites that each stored it into a field nothing read.

    kprintf("[SMB] Negotiated dialect: 0x%04x\n", conn->dialect);
    kprintf("[SMB] Max read size: %u, Max write size: %u\n",
            conn->max_read_size, conn->max_write_size);

    // ========================================================================
    // THE HANDSHAKE-INTEGRITY GATE. It fires BEFORE any credential leaves this
    // machine, for the same reason the two gates below it do.
    //
    // Three things happen here and they are three DIFFERENT checks:
    //
    //   (a) THE DIALECT FLOOR. We offered 3.1.1; if the server answered with
    //       anything lower, an attacker may simply have deleted 0x0311 from
    //       our dialect list, and at a lower dialect there is no pre-auth hash
    //       on EITHER side, so nothing can ever detect that. This is the only
    //       possible defence against a version downgrade and it is a client-
    //       side policy, not a protocol feature.
    //
    //   (b) FOLD the NEGOTIATE response into the running hash.
    //
    //   (c) VALIDATE what the server actually selected: SHA-512 for the hash,
    //       a cipher we offered, a signing algorithm we offered, and a
    //       PREAUTH_INTEGRITY context present at all, which 3.1.1 requires.
    //       This catches an HONEST server whose choices we cannot implement,
    //       which would otherwise surface several messages later as an
    //       unexplained decryption failure. An attack and a compatibility
    //       problem look identical on a console if you do not separate them.
    // ========================================================================
    {
        uint16_t fl = smbpreauth_floor_check(conn->dialect);
        if (fl != 0) {
            kprintf("[SMB] MOUNT REFUSED: %s (server selected dialect "
                    "0x%04x; we offered up to 0x%04x).\n",
                    smbpreauth_reason_text(fl), conn->dialect,
                    SMB311_PROTOCOL);
            kprintf("[SMB]   The OPENING HANDSHAKE of an SMB connection "
                    "decides the protocol version, whether messages are "
                    "signed, whether they are encrypted and with which "
                    "cipher. It happens in the clear, before any key exists. "
                    "Only SMB 3.1.1 authenticates it afterwards, by mixing a "
                    "hash of every handshake message into the session keys. "
                    "Below 3.1.1 an attacker who can modify packets can edit "
                    "that negotiation and NEITHER SIDE CAN TELL.\n");
            kprintf("[SMB]   This is the DEFAULT policy, not a hard rule. To "
                    "allow older servers, put\n"
                    "[SMB]       min_dialect=3.0.2\n"
                    "[SMB]   in %s. Under that setting signing and encryption "
                    "still work exactly as before; what you give up is any "
                    "ability to detect a tampered handshake.\n",
                    SMB_SIGN_CFG_PATH);
            return -1;
        }
        if (conn->preauth.offered) {
            uint16_t fr = smbpreauth_fold(&conn->preauth, smb_recv_buffer,
                                          (size_t)recv_len);
            if (fr != 0) {
                kprintf("[SMB] MOUNT REFUSED: pre-auth integrity could not "
                        "fold the NEGOTIATE response: %s\n",
                        smbpreauth_reason_text(fr));
                return -1;
            }
            uint16_t nr2 = smbpreauth_note_negotiate(&conn->preauth,
                                                     conn->dialect,
                                                     smb_recv_buffer,
                                                     (size_t)recv_len);
            if (nr2 != 0) {
                kprintf("[SMB] MOUNT REFUSED: %s\n",
                        smbpreauth_reason_text(nr2));
                return -1;
            }
        }
        if (smbpreauth_is_active(&conn->preauth)) {
            kprintf("[SMB] Pre-auth integrity: ACTIVE (SMB 3.1.1, SHA-512 over "
                    "the handshake); server chose cipher 0x%04x, signing "
                    "algorithm 0x%04x\n",
                    smbpreauth_cipher(&conn->preauth),
                    smbpreauth_signalg(&conn->preauth));
        } else {
            kprintf("[SMB] Pre-auth integrity: NOT ACTIVE at dialect 0x%04x. "
                    "The opening handshake of this connection is NOT "
                    "authenticated (min_dialect=%s)\n",
                    conn->dialect, smbpreauth_floor_text(smbpreauth_floor()));
        }
    }

    // THE SIGNING DOWNGRADE GATE, and it fires BEFORE any credential leaves
    // this machine.
    //
    // `conn->signing_required` above is the field that, until this change, was
    // written here and read by nothing in any file in the tree. It is now one
    // of the two inputs to this decision; the other is our own policy.
    //
    // Refusing here rather than at the first signed message is the point: a
    // server that cannot sign cannot be talked to safely, and finding that out
    // after sending an NTLM authentication is finding it out too late.
    {
        uint16_t nr = smbsign_note_negotiate(&conn->sign, conn->dialect,
                                             le16(resp->security_mode));
        if (nr != 0) {
            kprintf("[SMB] MOUNT REFUSED: %s (server security_mode=0x%04x, "
                    "dialect 0x%04x).\n", smbsign_reason_text(nr),
                    le16(resp->security_mode), conn->dialect);
            kprintf("[SMB]   Without message signing nothing authenticates "
                    "this file server: an attacker able to modify packets in "
                    "flight could change the contents of any file read from "
                    "it, and no layer below the application could tell. The "
                    "mount is refused rather than brought up unprotected.\n");
            kprintf("[SMB]   This is the DEFAULT policy, not a hard rule. To "
                    "allow unsigned mounts to SMB2 servers, put\n"
                    "[SMB]       signing=enabled\n"
                    "[SMB]   in %s. Signatures that ARE present are still "
                    "verified under that setting; only the refusal of a server "
                    "that offers none is relaxed.\n", SMB_SIGN_CFG_PATH);
            return -1;
        }
        kprintf("[SMB] Signing: policy=%s, server says %s\n",
                smbsign_policy() == SMB_SIGN_POLICY_REQUIRED ? "required" : "enabled",
                conn->signing_required ? "REQUIRED" : "supported");
    }

    // THE ENCRYPTION DOWNGRADE GATE, and like the signing one it fires BEFORE
    // any credential leaves this machine.
    //
    // Refusing here is the only place a downgrade CAN be refused at this
    // dialect. SMB 3.0.2 has no pre-auth integrity protection: this NEGOTIATE
    // exchange is unsigned, precedes every key, and nothing later
    // authenticates it (the running SHA-512 that fixes this arrives with
    // 3.1.1, which this client does not offer). So an attacker in the path
    // need only clear ONE BIT of the capabilities we just read, and an
    // opportunistic client carries on in plain text with no way to notice.
    // "Require it, and fail loudly" is what turns that from an invisible
    // success into a visible failure.
    {
        // ====================================================================
        // AT SMB 3.1.1 THE CAPABILITY BIT IS NOT HOW ENCRYPTION IS NEGOTIATED,
        // AND THIS WAS FOUND BY MEASUREMENT, NOT BY READING.
        //
        // MEASURED against Samba 4.17.12 on 2026-10-07: when the client sends
        // an ENCRYPTION_CAPABILITIES negotiate context, the server's NEGOTIATE
        // response capabilities come back as 0x0000000f with
        // SMB2_GLOBAL_CAP_ENCRYPTION (0x40) CLEAR, and its cipher choice is
        // reported in the context instead. Send NO encryption context and the
        // same server answers 0x0000004f, with the bit SET. So the bit means
        // "encryption, the 3.0.x way" and goes quiet once the 3.1.1 mechanism
        // is in use.
        //
        // Passing conn->capabilities through unchanged would therefore have
        // smbcrypt_note_negotiate() conclude "this server cannot encrypt" for
        // EVERY 3.1.1 mount, and under the default encrypt=required it would
        // refuse all of them, with a message blaming the server for something
        // the client had done to itself. That is the same failure mode as the
        // capabilities line a few dozen lines above this one, which records
        // that NOT setting the bit in our REQUEST made the server never offer
        // encryption and would have refused every mount "for a reason that was
        // our own doing".
        //
        // So the negotiated CIPHER is translated into the capability the
        // encryption module already understands. If 3.1.1 came back with no
        // cipher context at all, the server declined to encrypt, the bit stays
        // clear, and encrypt=required refuses it, which is correct.
        // ====================================================================
        uint32_t crypt_caps = conn->capabilities;
        if (smbpreauth_is_active(&conn->preauth)
            && smbpreauth_cipher(&conn->preauth) != 0xFFFFu) {
            crypt_caps |= SMB2_GLOBAL_CAP_ENCRYPTION;
        }
        uint16_t er = smbcrypt_note_negotiate(&conn->crypt, conn->dialect,
                                              crypt_caps);
        if (er != 0) {
            kprintf("[SMB] MOUNT REFUSED: %s (server capabilities=0x%08x, "
                    "as judged 0x%08x, dialect 0x%04x).\n",
                    smbcrypt_reason_text(er), conn->capabilities, crypt_caps,
                    conn->dialect);
            kprintf("[SMB]   Without encryption, every byte of every file read "
                    "from or written to this server crosses the network in "
                    "PLAIN TEXT, readable by anything on the path. Signing "
                    "proves who sent a message and that it was not altered; it "
                    "does nothing to stop it being read. The mount is refused "
                    "rather than brought up readable.\n");
            kprintf("[SMB]   This is the DEFAULT policy, not a hard rule. To "
                    "allow unencrypted mounts, put\n"
                    "[SMB]       encrypt=enabled\n"
                    "[SMB]   in %s (or `encrypt=off` to never encrypt). Under "
                    "`enabled` a server that CAN encrypt still does, and every "
                    "mount that ends up readable says so on this console.\n",
                    SMB_SIGN_CFG_PATH);
            return -1;
        }
        kprintf("[SMB] Encryption: policy=%s, server %s\n",
                smbcrypt_policy_text(smbcrypt_policy()),
                smbcrypt_server_capable(&conn->crypt) ? "OFFERS it"
                                                      : "does NOT offer it");
    }

    return 0;
}

// SMB2 Session Setup (NTLM authentication)
int smb2_session_setup(smb_connection_t *conn) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    // Phase 1: Send NTLM Negotiate message
    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_SESSION_SETUP, 0);
    offset += SMB2_HEADER_SIZE;

    smb2_session_setup_req_t *req = (smb2_session_setup_req_t *)(buf + offset);
    req->structure_size = le16(25);
    req->flags = 0;
    req->security_mode = SMB2_NEGOTIATE_SIGNING_ENABLED;
    req->capabilities = 0;
    req->channel = 0;
    req->previous_session_id = 0;
    offset += sizeof(smb2_session_setup_req_t);

    // Build NTLM Negotiate message
    req->security_buffer_offset = le16(offset);
    ntlm_negotiate_t *ntlm_neg = (ntlm_negotiate_t *)(buf + offset);
    memcpy(ntlm_neg->signature, NTLMSSP_SIGNATURE, 8);
    ntlm_neg->message_type = le32(NTLM_NEGOTIATE);
    ntlm_neg->negotiate_flags = le32(
        NTLMSSP_NEGOTIATE_UNICODE |
        NTLMSSP_NEGOTIATE_NTLM |
        NTLMSSP_REQUEST_TARGET |
        NTLMSSP_NEGOTIATE_ALWAYS_SIGN |
        NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY
    );
    ntlm_neg->domain_len = 0;
    ntlm_neg->domain_max_len = 0;
    ntlm_neg->domain_offset = 0;
    ntlm_neg->workstation_len = 0;
    ntlm_neg->workstation_max_len = 0;
    ntlm_neg->workstation_offset = 0;
    int ntlm_len = sizeof(ntlm_negotiate_t);
    req->security_buffer_length = le16(ntlm_len);
    offset += ntlm_len;

    kprintf("[SMB] Sending SESSION_SETUP (NTLM Negotiate)\n");

    // Fold SESSION_SETUP request #1. Before smb_send_packet(), on the exact
    // bytes that go on the wire.
    if (smbpreauth_is_active(&conn->preauth)) {
        uint16_t fr = smbpreauth_fold(&conn->preauth, buf, (size_t)offset);
        if (fr != 0) {
            kprintf("[SMB] pre-auth integrity: could not fold SESSION_SETUP "
                    "request 1: %s\n", smbpreauth_reason_text(fr));
            return -1;
        }
    }

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    // Receive challenge
    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_session_setup_resp_t))) {
        kprintf("[SMB] Invalid session setup response\n");
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status != STATUS_MORE_PROCESSING_REQUIRED) {
        if (status == STATUS_SUCCESS) {
            // Guest access granted
            conn->session_id = le64(resp_hdr->session_id);
            conn->session_established = true;
            kprintf("[SMB] Guest session established\n");
            return 0;
        }
        kprintf("[SMB] Session setup failed: 0x%08x\n", status);
        return -1;
    }

    // Store session ID for continuation
    conn->session_id = le64(resp_hdr->session_id);

    // Parse NTLM Challenge
    smb2_session_setup_resp_t *resp = (smb2_session_setup_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    uint16_t sec_offset = le16(resp->security_buffer_offset);
    uint16_t sec_len = le16(resp->security_buffer_length);

    if (sec_offset + sec_len > (uint16_t)recv_len) {
        kprintf("[SMB] Invalid security buffer offset/length\n");
        return -1;
    }

    ntlm_challenge_t *challenge = (ntlm_challenge_t *)(smb_recv_buffer + sec_offset);
    if (memcmp(challenge->signature, NTLMSSP_SIGNATURE, 8) != 0 ||
        le32(challenge->message_type) != NTLM_CHALLENGE) {
        kprintf("[SMB] Invalid NTLM challenge\n");
        return -1;
    }

    // Fold SESSION_SETUP response #1, the STATUS_MORE_PROCESSING_REQUIRED one.
    // MEASURED: this response IS folded; the FINAL one is not, and that
    // asymmetry is not arbitrary. The final response is the message whose
    // signature the derived key has to verify, so it cannot also be an input
    // to the key.
    if (smbpreauth_is_active(&conn->preauth)) {
        uint16_t fr = smbpreauth_fold(&conn->preauth, smb_recv_buffer,
                                      (size_t)recv_len);
        if (fr != 0) {
            kprintf("[SMB] pre-auth integrity: could not fold SESSION_SETUP "
                    "response 1: %s\n", smbpreauth_reason_text(fr));
            return -1;
        }
    }

    kprintf("[SMB] Received NTLM Challenge\n");

    // Phase 2: Send NTLM Authenticate message
    offset = 0;
    hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_SESSION_SETUP, 0);
    offset += SMB2_HEADER_SIZE;

    req = (smb2_session_setup_req_t *)(buf + offset);
    req->structure_size = le16(25);
    req->flags = 0;
    req->security_mode = SMB2_NEGOTIATE_SIGNING_ENABLED;
    req->capabilities = 0;
    req->channel = 0;
    req->previous_session_id = 0;
    offset += sizeof(smb2_session_setup_req_t);

    // Build NTLM Authenticate message
    req->security_buffer_offset = le16(offset);

    uint8_t *auth_buf = buf + offset;
    int auth_offset = 0;

    ntlm_authenticate_t *auth = (ntlm_authenticate_t *)auth_buf;
    memcpy(auth->signature, NTLMSSP_SIGNATURE, 8);
    auth->message_type = le32(NTLM_AUTHENTICATE);
    auth->negotiate_flags = le32(
        NTLMSSP_NEGOTIATE_UNICODE |
        NTLMSSP_NEGOTIATE_NTLM |
        NTLMSSP_NEGOTIATE_ALWAYS_SIGN |
        NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY
    );
    auth_offset = sizeof(ntlm_authenticate_t);

    // Get target info from challenge for NTLMv2
    uint8_t *target_info = NULL;
    uint32_t target_info_len = 0;
    if (le32(challenge->negotiate_flags) & NTLMSSP_NEGOTIATE_TARGET_INFO) {
        target_info = smb_recv_buffer + sec_offset + le32(challenge->target_info_offset);
        target_info_len = le16(challenge->target_info_len);
    }

    // Compute NTLMv2 response, AND the session key it falls out of. The key is
    // a local, not a field: it is consumed by smbsign_begin() below and wiped
    // on every path out of this function.
    uint8_t nt_response[256];
    uint32_t nt_response_len;
    uint8_t session_key[16];
    ntlm_compute_ntlmv2_response(conn->domain, conn->username, conn->nt_hash,
                                  challenge->server_challenge, target_info, target_info_len,
                                  nt_response, &nt_response_len, session_key);

    // LM response (empty for NTLMv2)
    auth->lm_response_offset = le32(auth_offset);
    auth->lm_response_len = 0;
    auth->lm_response_max_len = 0;

    // NT response
    auth->nt_response_offset = le32(auth_offset);
    auth->nt_response_len = le16(nt_response_len);
    auth->nt_response_max_len = le16(nt_response_len);
    memcpy(auth_buf + auth_offset, nt_response, nt_response_len);
    auth_offset += nt_response_len;

    // Domain (Unicode)
    auth->domain_offset = le32(auth_offset);
    int domain_len = ascii_to_unicode(conn->domain, (uint16_t*)(auth_buf + auth_offset), 64);
    auth->domain_len = le16(domain_len);
    auth->domain_max_len = le16(domain_len);
    auth_offset += domain_len;

    // Username (Unicode)
    auth->user_offset = le32(auth_offset);
    int user_len = ascii_to_unicode(conn->username, (uint16_t*)(auth_buf + auth_offset), 64);
    auth->user_len = le16(user_len);
    auth->user_max_len = le16(user_len);
    auth_offset += user_len;

    // Workstation (Unicode) - use "MAYTERA"
    auth->workstation_offset = le32(auth_offset);
    int ws_len = ascii_to_unicode("MAYTERA", (uint16_t*)(auth_buf + auth_offset), 16);
    auth->workstation_len = le16(ws_len);
    auth->workstation_max_len = le16(ws_len);
    auth_offset += ws_len;

    // Encrypted random session key (empty)
    auth->encrypted_random_offset = le32(auth_offset);
    auth->encrypted_random_len = 0;
    auth->encrypted_random_max_len = 0;

    req->security_buffer_length = le16(auth_offset);
    offset += auth_offset;

    kprintf("[SMB] Sending SESSION_SETUP (NTLM Authenticate)\n");

    // Fold the FINAL SESSION_SETUP request. This is the LAST message that goes
    // into the hash, and the hash as it stands after this fold is exactly what
    // both sides derive their keys from.
    //
    // MEASURED, and NOT what a first attempt assumed: a real client does NOT
    // sign this request. Its Flags field carried 0x00000010 with
    // SMB2_FLAGS_SIGNED (0x08) CLEAR and its Signature field all zero. Signing
    // it changes the bytes folded here by one bit in Flags, which changes every
    // derived key, and Samba 4.17.12 answers that by dropping the connection
    // with NT_STATUS_INVALID_HANDLE and "No signing key for SMB2 signing" --
    // an error that points nowhere near the cause. smbsign_sign() already
    // declines to sign SESSION_SETUP (R_NOT_ARMED for pre-session commands),
    // so the two agree by construction rather than by luck, but the hash is
    // taken here, before smb_send_packet() could change a byte of it.
    if (smbpreauth_is_active(&conn->preauth)) {
        uint16_t fr = smbpreauth_fold(&conn->preauth, buf, (size_t)offset);
        if (fr != 0) {
            kprintf("[SMB] pre-auth integrity: could not fold the final "
                    "SESSION_SETUP request: %s\n",
                    smbpreauth_reason_text(fr));
            secure_zero(session_key, sizeof(session_key));
            return -1;
        }
    }

    if (smb_send_packet(conn, buf, offset) < 0) {
        secure_zero(session_key, sizeof(session_key));
        return -1;
    }

    // Receive final response
    recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)SMB2_HEADER_SIZE) {
        secure_zero(session_key, sizeof(session_key));
        return -1;
    }

    resp_hdr = (smb2_header_t *)smb_recv_buffer;
    status = le32(resp_hdr->status);

    if (status != STATUS_SUCCESS) {
        // #749: do NOT blame the credentials unless the server actually
        // answered OUR request and said so. This line used to read
        // "Authentication failed: 0x%08x" for every non-success status,
        // including one read off a connection the server had already torn down
        // over a MessageId it rejected, and 0xC000006D (STATUS_LOGON_FAILURE)
        // reads exactly like a wrong password. A wrong diagnosis in an error
        // message is a defect in its own right: it sends the next person to
        // look at the password instead of at the sequence, and that is where
        // the hour went.
        //
        // smb_recv_packet() has already checked that this response answers the
        // MessageId we sent, so by the time we are here the status IS about
        // our request. The sequence fault is still reported first, because a
        // faulted sequence means the status we are holding is not trustworthy
        // evidence of anything.
        uint16_t sf = smbseq_fault(&conn->seq);
        if (sf != SMB_SEQ_FAULT_NONE) {
            kprintf("[SMB] SESSION_SETUP could not complete: %s. This is a "
                    "PROTOCOL SEQUENCING fault, NOT a credential problem. The "
                    "server status 0x%08x is not a verdict on the password.\n",
                    smbseq_fault_text(sf), status);
        } else if (status == STATUS_LOGON_FAILURE ||
                   status == STATUS_WRONG_PASSWORD ||
                   status == STATUS_NO_SUCH_USER ||
                   status == STATUS_ACCOUNT_DISABLED ||
                   status == STATUS_PASSWORD_EXPIRED) {
            kprintf("[SMB] Authentication REJECTED by the server for %s\\%s: "
                    "0x%08x (%s). The credentials are wrong.\n",
                    conn->domain, conn->username, status, smb_strerror((smb2_status_t)status));
        } else {
            kprintf("[SMB] SESSION_SETUP failed with 0x%08x (%s). This is NOT "
                    "an authentication rejection; the server refused the "
                    "request for another reason.\n",
                    status, smb_strerror((smb2_status_t)status));
        }
        secure_zero(session_key, sizeof(session_key));
        return -1;
    }

    conn->session_established = true;

    // ========================================================================
    // ARM SIGNING, AND VERIFY THIS VERY RESPONSE BEFORE READING A FIELD OF IT.
    //
    // This block sits ABOVE the session_flags read and the identity check on
    // purpose. The identity check (advisory 0044, rustkern/smbident.rs) reads
    // SMB2_SESSION_FLAG_IS_GUEST out of this response and refuses a session
    // that came back anonymous when a named one was asked for. That is a real
    // fix, and until this block existed it rested entirely on the server's own
    // unauthenticated word: an attacker in the network path had only to clear
    // one bit to turn a refused downgrade into an accepted one. Verifying the
    // signature on THIS message is what makes the flag it reads evidence.
    //
    // MS-SMB2 3.2.5.3 puts the check in the same place, for the same reason:
    // the final SESSION_SETUP response is the first message the server can
    // sign, and signing it proves the server holds the session key, which for
    // NTLMv2 means it holds the user's NT hash.
    // ========================================================================
    // Declared out here because the encryption summary at the END of this
    // function has to know whether keys were derived, and the block below is
    // the only place the session key exists.
    uint16_t crypt_begin_rc = 0;
    {
        uint16_t br;
        // ====================================================================
        // THE KEY SCHEDULE. At 3.0.2 both keys are functions of the session key
        // ALONE, which is why a tampered handshake still produced a working
        // session there. At 3.1.1 they are functions of the session key AND the
        // SHA-512 over every handshake message, so if an attacker edited any of
        // those messages our keys and the server's keys differ and the very
        // next signature check fails. That is the whole mechanism.
        // ====================================================================
        if (smbpreauth_is_active(&conn->preauth)) {
            uint8_t k_sign[16], k_enc[16], k_dec[16], k_app[16];
            uint16_t dr = smbpreauth_derive(&conn->preauth, session_key,
                                            k_sign, k_enc, k_dec, k_app);
            // The application key has no consumer in this kernel yet. It is
            // derived anyway: deriving three of the four and leaving a gap
            // invites someone to guess the fourth label later.
            secure_zero(k_app, sizeof(k_app));
            if (dr != 0) {
                br = dr;
                crypt_begin_rc = dr;
                secure_zero(k_sign, sizeof(k_sign));
                secure_zero(k_enc, sizeof(k_enc));
                secure_zero(k_dec, sizeof(k_dec));
                secure_zero(session_key, sizeof(session_key));
                kprintf("[SMB] MOUNT REFUSED: the SMB 3.1.1 key schedule "
                        "refused its inputs: %s\n",
                        smbpreauth_reason_text(dr));
                conn->session_established = false;
                conn->session_id = 0;
                smbsign_reset(&conn->sign);
                smbcrypt_reset(&conn->crypt);
                smbpreauth_reset(&conn->preauth);
                return -1;
            }
            br = smbsign_begin311(&conn->sign, k_sign);
            crypt_begin_rc = smbcrypt_begin311(&conn->crypt, k_enc, k_dec,
                                               conn->session_id);
            secure_zero(k_sign, sizeof(k_sign));
            secure_zero(k_enc, sizeof(k_enc));
            secure_zero(k_dec, sizeof(k_dec));
            // The session key is password-adjacent and is needed exactly once.
            secure_zero(session_key, sizeof(session_key));
            // The hash is frozen by smbpreauth_derive(), so the count can be
            // reported as final evidence rather than as a work in progress.
            // Five is the expected number; anything else means a message was
            // folded that does not belong, or one that does was missed.
            if (smbpreauth_folded(&conn->preauth) != SMB_PREAUTH_EXPECT_FOLDS) {
                kprintf("[SMB] MOUNT REFUSED: the pre-auth integrity hash "
                        "covers %u handshake messages, not %u. The hash does "
                        "not describe this handshake, so the keys derived from "
                        "it prove nothing.\n",
                        smbpreauth_folded(&conn->preauth),
                        SMB_PREAUTH_EXPECT_FOLDS);
                conn->session_established = false;
                conn->session_id = 0;
                smbsign_reset(&conn->sign);
                smbcrypt_reset(&conn->crypt);
                smbpreauth_reset(&conn->preauth);
                return -1;
            }
        } else {
        br = smbsign_begin(&conn->sign, conn->dialect, session_key);
        // THE ENCRYPTION KEYS COME OFF THE SAME SESSION KEY, so they are
        // derived HERE, inside the one window where that key exists, rather
        // than by keeping it alive for a later caller. conn->session_id is
        // already set (phase 1 above); it travels in the clear in every
        // transform header AND inside the AEAD's associated data, which is
        // what makes a frame re-aimed at another session detectable.
        crypt_begin_rc = smbcrypt_begin(&conn->crypt, conn->dialect,
                                        session_key, conn->session_id);
        // The session key is password-adjacent and is needed exactly once.
        secure_zero(session_key, sizeof(session_key));
        }

        if (br == 0) {
            // Read the unsigned-accepted counter before and after, because
            // smbsign_check_response() returns 0 BOTH for "verified" and for
            // "arrived unsigned and the policy permits that". Those two are the
            // same return value and completely different facts, and at 3.1.1
            // the difference decides whether the pre-auth hash was ever
            // compared to anything.
            uint32_t uin_before = 0;
            smbsign_counts(&conn->sign, NULL, NULL, NULL, &uin_before);
            uint16_t vr = smbsign_check_response(&conn->sign,
                                                 smb_recv_buffer,
                                                 (size_t)recv_len);
            uint32_t uin_after = 0;
            smbsign_counts(&conn->sign, NULL, NULL, NULL, &uin_after);
            if (vr == 0 && smbpreauth_is_active(&conn->preauth)
                && uin_after != uin_before) {
                // ============================================================
                // THE PRE-AUTH HASH IS ONLY EVER CHECKED BY THIS SIGNATURE.
                //
                // An unsigned final SESSION_SETUP response would leave the hash
                // COMPUTED and NEVER COMPARED, which is exactly the shape of a
                // control that looks present and does nothing. So at 3.1.1 this
                // message must be signed and must verify, REGARDLESS of the
                // signing policy: `signing=enabled` relaxes the refusal of a
                // server that offers no signatures at all, and it must not also
                // silently relax handshake integrity. That coupling, one
                // setting quietly carrying another control, is the exact fault
                // this whole change exists to remove; reintroducing it one
                // layer down would be worse than not fixing it.
                // ============================================================
                kprintf("[SMB] MOUNT REFUSED: the final SESSION_SETUP response "
                        "arrived UNSIGNED and SMB 3.1.1 pre-auth integrity is "
                        "active.\n");
                kprintf("[SMB]   That signature is the ONLY thing that ever "
                        "compares our hash of the opening handshake against "
                        "the server's. Without it the handshake would be "
                        "hashed and never checked, which protects nothing "
                        "while looking as though it does. This refusal is NOT "
                        "relaxed by signing=enabled.\n");
                conn->session_established = false;
                conn->session_id = 0;
                smbsign_reset(&conn->sign);
                smbcrypt_reset(&conn->crypt);
                smbpreauth_reset(&conn->preauth);
                return -1;
            }
            if (vr != 0) {
                kprintf("[SMB] SESSION_SETUP RESPONSE REJECTED: %s\n",
                        smbsign_reason_text(vr));
                kprintf("[SMB]   The server returned STATUS_SUCCESS for %s\\%s "
                        "but could not prove it holds this session's key. "
                        "Either the reply was altered in flight, or whatever "
                        "answered is not the server that holds the credential. "
                        "The session is ABANDONED; nothing about it, including "
                        "who it says we are, is treated as true.\n",
                        conn->domain, conn->username);
                conn->session_established = false;
                conn->session_id = 0;
                smbsign_reset(&conn->sign);
                return -1;
            }
            // THE HANDSHAKE IS NOW AUTHENTICATED, and this is the only point
            // at which that becomes true. Up to here the hash was computed; the
            // signature just verified is what compared it with the server's.
            if (smbpreauth_is_active(&conn->preauth)) {
                smbpreauth_note_verified(&conn->preauth);
                kprintf("[SMB] HANDSHAKE AUTHENTICATED: the SMB 3.1.1 pre-auth "
                        "integrity hash over %u handshake messages keyed this "
                        "session, and the server's signature on the final "
                        "SESSION_SETUP response verified against it. A "
                        "tampered negotiation could not have reached this "
                        "point.\n", smbpreauth_folded(&conn->preauth));
            }
            kprintf("[SMB] Signing ARMED for this session: %s\n",
                    smbsign_algo_text(smbsign_algo(&conn->sign)));
        } else if (smbident_name_is_anonymous(conn->username)) {
            // AN ANONYMOUS MOUNT CANNOT BE PROTECTED BY SIGNING, AND SAYING
            // OTHERWISE WOULD BE THE DISHONEST OPTION.
            //
            // A guest or null NTLM session produces no secret session key, so
            // there is nothing to key a MAC with that an attacker could not
            // key just as well by opening their own anonymous session to the
            // same server. Refusing the mount here would break the documented
            // /SMB/<server>/<share> no-credential flow in exchange for no
            // security at all, so it proceeds, loudly.
            //
            // This is NOT a downgrade an attacker can force. Reaching this
            // branch requires that the CALLER asked for an anonymous identity
            // (smbident_name_is_anonymous reads conn->username, which is local
            // and not from the wire). A named mount whose session came back
            // anonymous is refused a few lines below by the identity check.
            kprintf("[SMB] UNSIGNED session, and it cannot be otherwise: an "
                    "anonymous/guest mount produces no secret session key, so "
                    "message signing would protect nothing (any peer can "
                    "derive the same key). Reads from %s are NOT protected "
                    "against modification in flight. Use a named account if "
                    "that matters.\n",
                    conn->mount_point[0] ? conn->mount_point : "this share");
        } else {
            kprintf("[SMB] MOUNT REFUSED for %s\\%s: %s\n",
                    conn->domain, conn->username, smbsign_reason_text(br));
            kprintf("[SMB]   A named account authenticated successfully but "
                    "produced no usable signing key, so nothing can "
                    "authenticate the server from here on. This is not "
                    "expected against an SMB2 server and is refused rather "
                    "than run unprotected.\n");
            smb2_logoff(conn);
            conn->session_established = false;
            conn->session_id = 0;
            smbsign_reset(&conn->sign);
            return -1;
        }
    }

    resp = (smb2_session_setup_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    conn->session_flags = le16(resp->session_flags);

    // #749 pass 2: THE IDENTITY CHOKEPOINT. Every mount path in this file
    // reaches smb_conn_establish(), and smb_conn_establish() reaches here, so
    // this is the one place that can answer "who did we actually end up as"
    // for the first mount, a reconnect, the on-demand /SMB mount and the share
    // enumerator alike.
    //
    // This block used to be the two kprintf arms below and a `return 0`. The
    // guest flag was READ and then thrown away: a server that answered our
    // NTLM authentication for a named user with STATUS_SUCCESS plus "and you
    // are the guest account" got a working mount, reported as working, with
    // every read and write on it running as the server's guest account. The
    // only trace was one serial line on a desktop machine nobody is watching.
    // SMB2_SESSION_FLAG_IS_NULL was not read by any .c file in the kernel.
    //
    // The policy is in rustkern/smbident.rs, not here, because it is policy
    // and because the alternative is this rule existing once per mount path.
    {
        uint16_t idv = smbident_check(conn->username, conn->session_flags);
        if (idv != 0) {
            // NAME THE REAL CAUSE. The first half of #749 was a sequencing
            // fault reported as "Authentication failed: 0xC000006D", which
            // sent an engineer to look at the password. This is the same hazard
            // pointing the other way: the server returned SUCCESS, so a message
            // about a rejected password or a lost connection would be just as
            // wrong. Say that the server ACCEPTED the request and SUBSTITUTED
            // the identity.
            kprintf("[SMB] IDENTITY DOWNGRADE REFUSED on %s: you asked to "
                    "connect as %s\\%s and %s (session_flags=0x%04x). The "
                    "mount is REFUSED rather than brought up as somebody else. "
                    "This is NOT a wrong password and NOT a sequencing fault: "
                    "the server returned STATUS_SUCCESS. Either the account or "
                    "secret is not what this server expects for that name, or "
                    "the server is configured to map failures to its guest "
                    "account (Samba 'map to guest'), or it is not the server "
                    "you think it is.\n",
                    conn->mount_point[0] ? conn->mount_point : "this share",
                    conn->domain, conn->username, smbident_verdict_text(idv),
                    conn->session_flags);
            // Hand the session back politely, then make sure nothing can use
            // it: smb2_logoff() needs session_established set, and clears it.
            smb2_logoff(conn);
            conn->session_established = false;
            conn->session_id = 0;
            return -1;
        }
    }

    if (conn->session_flags & SMB2_SESSION_FLAG_IS_GUEST) {
        // Reached only when an anonymous session is what was ASKED for, which
        // is the normal case for /SMB/<server>/<share> with no saved
        // credentials. It is no longer a silent substitution, so say which.
        kprintf("[SMB] Logged in as guest (an anonymous session was what was "
                "requested, user=\"%s\")\n", conn->username);
    } else {
        kprintf("[SMB] Authenticated as %s\\%s\n", conn->domain, conn->username);
    }

    // ========================================================================
    // DECIDE WHETHER THIS SESSION IS ENCRYPTED, and say so either way.
    //
    // This sits AFTER the identity check deliberately: there is no point
    // discussing the protection of a session that has already been refused
    // for coming back as somebody else. It reads SessionFlags, which this
    // very response carried and whose signature has just been verified, so
    // the flag it reads is evidence rather than the server's unchecked word.
    // ========================================================================
    {
        uint16_t sr = smbcrypt_note_session_flags(&conn->crypt,
                                                  conn->session_flags);
        if (sr != 0) {
            kprintf("[SMB] MOUNT REFUSED for %s\\%s: %s\n",
                    conn->domain, conn->username, smbcrypt_reason_text(sr));
            kprintf("[SMB]   The server will not serve this session "
                    "unencrypted and the local policy will not encrypt it. "
                    "Refusing here, rather than at the first file operation, "
                    "is deliberate: the alternative is an unexplained "
                    "0xC0000022 three requests later.\n");
            smb2_logoff(conn);
            conn->session_established = false;
            conn->session_id = 0;
            smbcrypt_reset(&conn->crypt);
            smbpreauth_reset(&conn->preauth);
            return -1;
        }
        if (smbcrypt_is_active(&conn->crypt)) {
            kprintf("[SMB] ENCRYPTION ARMED for this session: %s%s\n",
                    smbcrypt_algo_text(&conn->crypt),
                    smbcrypt_server_requires(&conn->crypt)
                        ? ", and the server requires it" : "");
        } else if (crypt_begin_rc == 0
                   && smbcrypt_policy() == SMB_CRYPT_POLICY_OFF) {
            kprintf("[SMB] NOT ENCRYPTED, by local policy (encrypt=off in %s). "
                    "Everything read from or written to %s crosses the network "
                    "in PLAIN TEXT.\n", SMB_SIGN_CFG_PATH,
                    conn->mount_point[0] ? conn->mount_point : "this share");
        } else if (crypt_begin_rc != 0) {
            // No keys. For an anonymous mount this is unavoidable and is the
            // same honest limit signing has: a guest session yields no secret
            // session key, so "encryption" would run under a key any peer can
            // derive. The mount proceeds, loudly, exactly as the signing path
            // does, and only because the CALLER asked for anonymity; a named
            // mount that came back as guest was refused above.
            kprintf("[SMB] NOT ENCRYPTED, and it cannot be otherwise: %s. "
                    "Everything read from or written to %s crosses the network "
                    "in PLAIN TEXT. Use a named account if that matters.\n",
                    smbcrypt_reason_text(crypt_begin_rc),
                    conn->mount_point[0] ? conn->mount_point : "this share");
        } else {
            kprintf("[SMB] NOT ENCRYPTED: this server did not offer encryption "
                    "and the policy is encrypt=enabled, so the mount is "
                    "allowed. Everything read from or written to %s crosses "
                    "the network in PLAIN TEXT. `encrypt=required` in %s "
                    "refuses such a mount instead.\n",
                    conn->mount_point[0] ? conn->mount_point : "this share",
                    SMB_SIGN_CFG_PATH);
        }
    }

    return 0;
}

// SMB2 Tree Connect
int smb2_tree_connect(smb_connection_t *conn, const char *share) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    // Build header
    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_TREE_CONNECT, 0);
    offset += SMB2_HEADER_SIZE;

    // Build tree connect request
    smb2_tree_connect_req_t *req = (smb2_tree_connect_req_t *)(buf + offset);
    req->structure_size = le16(9);
    req->flags = 0;
    offset += sizeof(smb2_tree_connect_req_t);

    // Add share path (Unicode)
    req->path_offset = le16(offset);
    int path_len = ascii_to_unicode(share, (uint16_t*)(buf + offset), SMB_MAX_PATH);
    req->path_length = le16(path_len);
    offset += path_len;

    kprintf("[SMB] Connecting to share: %s\n", share);

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    // Receive response
    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_tree_connect_resp_t))) {
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] Tree connect failed: 0x%08x\n", status);
        return -1;
    }

    conn->tree_id = le32(resp_hdr->tree_id);
    conn->tree_connected = true;

    smb2_tree_connect_resp_t *resp = (smb2_tree_connect_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    conn->share_type = resp->share_type;

    // A SHARE CAN DEMAND ENCRYPTION ON ITS OWN, and this is the half that is
    // easy to miss. MEASURED against Samba 4.17.12: a per-share
    // `smb encrypt = required` sets ShareFlags 0x8000 here and leaves the
    // SESSION flag clear, so a client that read only the session flag carries
    // on in the clear and is then refused at CREATE with 0xC0000022 and no
    // stated reason. Both flags feed the same decision in
    // rustkern/smbcrypt.rs.
    {
        uint16_t sfr = smbcrypt_note_share_flags(&conn->crypt,
                                                 le32(resp->share_flags));
        if (sfr != 0) {
            kprintf("[SMB] SHARE REFUSED (%s): %s\n", share,
                    smbcrypt_reason_text(sfr));
            kprintf("[SMB]   This SHARE requires encryption (ShareFlags "
                    "0x%08x) and the local policy will not encrypt.\n",
                    le32(resp->share_flags));
            conn->tree_connected = false;
            conn->tree_id = 0;
            return -1;
        }
        if (smbcrypt_is_active(&conn->crypt)
            && (le32(resp->share_flags) & SMB2_SHAREFLAG_ENCRYPT_DATA)) {
            kprintf("[SMB] this share requires encryption (ShareFlags "
                    "0x%08x); it is in force\n", le32(resp->share_flags));
        }
    }

    strncpy(conn->share_path, share, SMB_MAX_PATH - 1);

    kprintf("[SMB] Connected to share (tree_id=%u, type=%d)\n",
            conn->tree_id, conn->share_type);

    return 0;
}

// SMB2 Tree Disconnect
int smb2_tree_disconnect(smb_connection_t *conn) {
    if (!conn->tree_connected) return 0;

    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_TREE_DISCONNECT, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    // Tree disconnect has minimal body
    uint16_t *body = (uint16_t*)(buf + offset);
    body[0] = le16(4);  // Structure size
    body[1] = 0;        // Reserved
    offset += 4;

    smb_send_packet(conn, buf, offset);
    smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 2000);

    conn->tree_connected = false;
    conn->tree_id = 0;

    return 0;
}

// SMB2 Logoff
int smb2_logoff(smb_connection_t *conn) {
    if (!conn->session_established) return 0;

    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_LOGOFF, 0);
    offset += SMB2_HEADER_SIZE;

    uint16_t *body = (uint16_t*)(buf + offset);
    body[0] = le16(4);
    body[1] = 0;
    offset += 4;

    smb_send_packet(conn, buf, offset);
    smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 2000);

    conn->session_established = false;
    conn->session_id = 0;

    return 0;
}

// ============================================================================
// SMB2 File Operations
// ============================================================================

// Open/Create a file
static int smb2_create(smb_connection_t *conn, const char *path,
                       uint32_t desired_access, uint32_t disposition,
                       uint32_t create_options, smb_file_t *file) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_CREATE, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_create_req_t *req = (smb2_create_req_t *)(buf + offset);
    req->structure_size = le16(57);
    req->security_flags = 0;
    req->requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
    req->impersonation_level = le32(SMB2_IMPERSONATION_IMPERSONATION);
    req->smb_create_flags = 0;
    req->reserved = 0;
    req->desired_access = le32(desired_access);
    req->file_attributes = le32(FILE_ATTRIBUTE_NORMAL);
    req->share_access = le32(FILE_SHARE_READ | FILE_SHARE_WRITE);
    req->create_disposition = le32(disposition);
    req->create_options = le32(create_options);
    req->create_contexts_offset = 0;
    req->create_contexts_length = 0;
    offset += sizeof(smb2_create_req_t);

    // Add filename (Unicode)
    req->name_offset = le16(offset);
    int name_len = ascii_to_unicode(path, (uint16_t*)(buf + offset), SMB_MAX_PATH);
    req->name_length = le16(name_len);
    offset += name_len;

    // MS-SMB2: CREATE requires a 1-byte Buffer even with an empty name (e.g.
    // opening the share root). Without it Samba returns INVALID_PARAMETER.
    if (name_len == 0) buf[offset++] = 0;

    // Align to 8 bytes
    while (offset & 7) buf[offset++] = 0;

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)SMB2_HEADER_SIZE) {
        kprintf("[SMB] Create: short response %d\n", recv_len);
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] Create failed: 0x%08x\n", status);
        return -1;
    }
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_create_resp_t))) {
        kprintf("[SMB] Create: response too short to parse %d\n", recv_len);
        return -1;
    }

    smb2_create_resp_t *resp = (smb2_create_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);

    file->file_id_persistent = le64(resp->file_id_persistent);
    file->file_id_volatile = le64(resp->file_id_volatile);
    file->file_size = le64(resp->end_of_file);
    file->attributes = le32(resp->file_attributes);
    file->is_directory = (file->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    file->position = 0;
    file->conn = conn;
    file->access = desired_access;
    strncpy(file->path, path, SMB_MAX_PATH - 1);


    return 0;
}

// Close a file
static int smb2_close_file(smb_connection_t *conn, smb_file_t *file) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_CLOSE, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_close_req_t *req = (smb2_close_req_t *)(buf + offset);
    req->structure_size = le16(24);
    req->flags = 0;
    req->reserved = 0;
    req->file_id_persistent = le64(file->file_id_persistent);
    req->file_id_volatile = le64(file->file_id_volatile);
    offset += sizeof(smb2_close_req_t);

    smb_send_packet(conn, buf, offset);
    smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 2000);

    return 0;
}

// Read from file
static ssize_t smb2_read(smb_connection_t *conn, smb_file_t *file,
                         void *buffer, size_t count) {
    if (count == 0) return 0;
    if (count > conn->max_read_size) {
        count = conn->max_read_size;
    }

    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_READ, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_read_req_t *req = (smb2_read_req_t *)(buf + offset);
    req->structure_size = le16(49);
    req->padding = 0;
    req->flags = 0;
    req->length = le32(count);
    req->offset = le64(file->position);
    req->file_id_persistent = le64(file->file_id_persistent);
    req->file_id_volatile = le64(file->file_id_volatile);
    req->minimum_count = 0;
    req->channel = 0;
    req->remaining_bytes = 0;
    req->read_channel_info_offset = 0;
    req->read_channel_info_length = 0;
    req->buffer[0] = 0;
    // MS-SMB2: a READ request MUST carry a 1-byte Buffer even when empty
    // (StructureSize is 49 = 48 fixed + 1). Sending only 48 bytes makes Samba
    // reject the request with STATUS_INVALID_PARAMETER (task #317).
    offset += sizeof(smb2_read_req_t);  // include the mandatory 1-byte buffer

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 10000);
    if (recv_len < (int)SMB2_HEADER_SIZE) {
        kprintf("[SMB] Read: short response %d\n", recv_len);
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    // SMB2 async: a READ on a named pipe (srvsvc RPC) can return an interim
    // STATUS_PENDING (0x00000103) with the ASYNC flag; the real response follows
    // on the same socket. Skip pending frames and wait for the final one
    // (task #317 pass 2: needed for share enumeration over \srvsvc).
    {
        int guard = 0;
        while (status == 0x00000103 && guard++ < 16) {
            recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 10000);
            if (recv_len < (int)SMB2_HEADER_SIZE) {
                kprintf("[SMB] Read: short async response %d\n", recv_len);
                return -1;
            }
            resp_hdr = (smb2_header_t *)smb_recv_buffer;
            status = le32(resp_hdr->status);
        }
    }

    if (status == STATUS_END_OF_FILE) {
        return 0;
    }

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] Read failed: 0x%08x\n", status);
        return -1;
    }
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_read_resp_t))) {
        kprintf("[SMB] Read: response too short to parse %d\n", recv_len);
        return -1;
    }

    smb2_read_resp_t *resp = (smb2_read_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    uint32_t data_len = le32(resp->data_length);
    uint8_t data_offset = resp->data_offset;

    if (data_len > count) {
        data_len = count;
    }

    memcpy(buffer, smb_recv_buffer + data_offset, data_len);
    file->position += data_len;

    return data_len;
}

// Write to file
static ssize_t smb2_write(smb_connection_t *conn, smb_file_t *file,
                          const void *buffer, size_t count) {
    if (count == 0) return 0;
    if (count > conn->max_write_size) {
        count = conn->max_write_size;
    }

    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_WRITE, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_write_req_t *req = (smb2_write_req_t *)(buf + offset);
    req->structure_size = le16(49);
    req->data_offset = le16(SMB2_HEADER_SIZE + sizeof(smb2_write_req_t));
    req->length = le32(count);
    req->offset = le64(file->position);
    req->file_id_persistent = le64(file->file_id_persistent);
    req->file_id_volatile = le64(file->file_id_volatile);
    req->channel = 0;
    req->remaining_bytes = 0;
    req->write_channel_info_offset = 0;
    req->write_channel_info_length = 0;
    req->flags = 0;
    offset += sizeof(smb2_write_req_t);

    // Copy data
    memcpy(buf + offset, buffer, count);
    offset += count;

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 10000);
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_write_resp_t))) {
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] Write failed: 0x%08x\n", status);
        return -1;
    }

    smb2_write_resp_t *resp = (smb2_write_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    uint32_t written = le32(resp->count);

    file->position += written;

    return written;
}

// Query directory
static int smb2_query_directory(smb_connection_t *conn, smb_file_t *dir,
                                 const char *pattern, uint8_t info_class,
                                 uint8_t flags, void *buffer, uint32_t buf_len,
                                 uint32_t *out_len) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_QUERY_DIRECTORY, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_query_directory_req_t *req = (smb2_query_directory_req_t *)(buf + offset);
    req->structure_size = le16(33);
    req->file_information_class = info_class;
    req->flags = flags;
    req->file_index = 0;
    req->file_id_persistent = le64(dir->file_id_persistent);
    req->file_id_volatile = le64(dir->file_id_volatile);
    req->output_buffer_length = le32(buf_len > 65536 ? 65536 : buf_len);
    offset += sizeof(smb2_query_directory_req_t);

    // Add search pattern (Unicode)
    req->file_name_offset = le16(offset);
    int pattern_len = ascii_to_unicode(pattern, (uint16_t*)(buf + offset), 256);
    req->file_name_length = le16(pattern_len);
    offset += pattern_len;

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)SMB2_HEADER_SIZE) {
        kprintf("[SMB] QueryDir: short response %d\n", recv_len);
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status == STATUS_NO_MORE_FILES) {
        *out_len = 0;
        return 1;  // End of directory
    }

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] QueryDir failed: 0x%08x\n", status);
        return -1;
    }
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_query_directory_resp_t))) {
        kprintf("[SMB] QueryDir: response too short to parse %d\n", recv_len);
        return -1;
    }

    smb2_query_directory_resp_t *resp = (smb2_query_directory_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    uint16_t data_offset = le16(resp->output_buffer_offset);
    uint32_t data_len = le32(resp->output_buffer_length);

    if (data_len > buf_len) {
        data_len = buf_len;
    }

    memcpy(buffer, smb_recv_buffer + data_offset, data_len);
    *out_len = data_len;

    return 0;
}

// ============================================================================
// Public API Implementation
// ============================================================================

// Initialize SMB subsystem
// #749: ONE-SHOT. This used to memset the whole connection table and the whole
// open-file table every time it was called, and it is called from FOUR places,
// THREE of them at runtime with shares already mounted: smb_vfs_mount_creds(),
// smb_list_shares() and smb_run_selftest().
//
// MEASURED on the wire, golden 2564 (tcpdump on the test server, 2026-10-03):
// the self-test's share connection sent 25 SMB2 messages and then received
// NEITHER a FIN NOR an RST from us. It was simply abandoned, mid-session, when
// smb_list_shares() called smb_init() and zeroed the table underneath it. The
// server kept that connection and its session open; our TCP socket was never
// closed, so it leaked too.
//
// That abandonment is the mechanism behind the Samba log lines in smb.h: a
// connection struct zeroed under a live connection has message_id back at 0
// while the server's window has moved on, and a struct zeroed while ANOTHER
// thread holds a pointer to it has two users incrementing one counter. Both
// produce exactly the observed "bad message_id 0 ... low = 2" and
// "bad message_id 3 ... low = 1".
//
// Initialising once is the fix. A caller that wants a clean slate has
// smb_unmount(), which disconnects properly. Resetting shared state as a
// side-effect of a mount is not a clean slate, it is data loss with a
// reassuring log line.
static bool smb_initialised = false;

void smb_init(void) {
    if (smb_initialised) return;
    memset(connections, 0, sizeof(connections));
    memset(open_files, 0, sizeof(open_files));
    smb_initialised = true;
    kprintf("[SMB] SMB client initialized\n");
}

// Parse SMB URL: smb://[user:pass@]server/share
static int parse_smb_url(const char *url, char *server, char *share,
                          char *domain, char *username, char *password) {
    // Default values
    server[0] = 0;
    share[0] = 0;
    strcpy(domain, "WORKGROUP");
    strcpy(username, "guest");
    password[0] = 0;

    // Skip protocol prefix
    if (strncmp(url, "smb://", 6) == 0) {
        url += 6;
    } else if (strncmp(url, "\\\\", 2) == 0) {
        url += 2;
    }

    // Check for credentials (user:pass@)
    const char *at = strchr(url, '@');
    if (at) {
        // Extract credentials
        const char *colon = strchr(url, ':');
        if (colon && colon < at) {
            // domain\user:pass@ format
            const char *backslash = strchr(url, '\\');
            if (backslash && backslash < colon) {
                int dom_len = backslash - url;
                strncpy(domain, url, dom_len < 63 ? dom_len : 63);
                domain[dom_len < 63 ? dom_len : 63] = 0;
                url = backslash + 1;
                colon = strchr(url, ':');
            }
            int user_len = colon - url;
            strncpy(username, url, user_len < 63 ? user_len : 63);
            username[user_len < 63 ? user_len : 63] = 0;
            int pass_len = at - colon - 1;
            strncpy(password, colon + 1, pass_len < 63 ? pass_len : 63);
            password[pass_len < 63 ? pass_len : 63] = 0;
        } else {
            // Just user@
            int user_len = at - url;
            strncpy(username, url, user_len < 63 ? user_len : 63);
            username[user_len < 63 ? user_len : 63] = 0;
        }
        url = at + 1;
    }

    // Extract server
    const char *slash = strchr(url, '/');
    if (!slash) slash = strchr(url, '\\');
    if (!slash) {
        // No share specified
        strncpy(server, url, 255);
        return 0;
    }

    int srv_len = slash - url;
    strncpy(server, url, srv_len < 255 ? srv_len : 255);
    server[srv_len < 255 ? srv_len : 255] = 0;

    // Extract share
    url = slash + 1;
    slash = strchr(url, '/');
    if (!slash) slash = strchr(url, '\\');
    if (slash) {
        int share_len = slash - url;
        strncpy(share, url, share_len < 255 ? share_len : 255);
        share[share_len < 255 ? share_len : 255] = 0;
    } else {
        strncpy(share, url, 255);
    }

    return 0;
}

// Resolve hostname to IP (simple implementation)
static uint32_t resolve_hostname(const char *hostname) {
    // Check if it's already an IP address
    uint32_t ip = 0;
    int octets[4] = {0};
    int n = 0;
    const char *p = hostname;

    while (*p && n < 4) {
        if (*p >= '0' && *p <= '9') {
            octets[n] = octets[n] * 10 + (*p - '0');
        } else if (*p == '.') {
            n++;
        } else {
            // Not a dotted-quad: resolve via the kernel DNS resolver (net/dns.c).
            // task #317 pass 4: hostnames (e.g. "fileserver" or "nas.local")
            // now work, not just raw IPs.
            uint32_t resolved = 0;
            extern int dns_resolve(const char *hostname, uint32_t *ip_out);
            if (dns_resolve(hostname, &resolved) == 0 && resolved != 0) {
                kprintf("[SMB] resolved %s -> %u.%u.%u.%u\n", hostname,
                        (resolved>>24)&0xFF, (resolved>>16)&0xFF,
                        (resolved>>8)&0xFF, resolved&0xFF);
                return resolved;
            }
            kprintf("[SMB] DNS resolution failed for %s\n", hostname);
            return 0;
        }
        p++;
    }

    if (n == 3) {
        ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
        return ip;
    }

    return 0;
}

// Mount SMB share
int smb_mount(const char *url, const char *mount_point) {
    char server[256], share[256], domain[64], username[64], password[64];

    if (parse_smb_url(url, server, share, domain, username, password) < 0) {
        kprintf("[SMB] Invalid URL: %s\n", url);
        return -1;
    }

    uint32_t server_ip = resolve_hostname(server);
    if (server_ip == 0) {
        kprintf("[SMB] Cannot resolve server: %s\n", server);
        return -1;
    }

    int r = smb_mount_auth(server_ip, share, domain, username, password, mount_point);
    // The URL carried a cleartext password into this frame. secure_zero, not
    // memset: a memset of a dead local is exactly what the compiler is allowed
    // to delete (same reasoning as ssh2_server.c:678).
    secure_zero(password, sizeof(password));
    return r;
}

// Mount with explicit credentials
// --- SMB connect resilience (#297/#317) ------------------------------------
// Rapid same-IP reboots and cold ARP make a flat 4-try connect fragile: the
// shared NIC/TCP/neighbour state can drop the SYN-ACK before ARP resolves.
// We retry more times with an ESCALATING settle (widening the settle for later
// attempts to ride out a slow neighbour-table/ARP warmup), re-arm ARP fresh
// each attempt, and use a fresh socket per attempt. Kept BOUNDED by both an
// attempt cap and an overall wall-clock deadline so a dead server still fails
// in reasonable time. No busy-spin: all waiting goes through smb_net_pump.
#define SMB_CONNECT_ATTEMPTS   10
#define SMB_CONNECT_TOTAL_MS   30000
#define SMB_CONNECT_TIMEOUT_MS 6000
#define SMB_ARP_REWARM_MS      2000

// Pump-count settle between connect attempts; widens with each attempt (each
// pump is about 1ms via smb_net_pump -> proc_sleep(1)). Capped so total time
// stays reasonable.
static inline int smb_connect_settle(int attempt) {
    int s = 40 + attempt * 60;   // 40, 100, 160, ... pumps (about ms)
    return s > 400 ? 400 : s;
}

static int smb_mount_auth_nt(uint32_t server_ip, const char *share,
                             const char *domain, const char *username,
                             const uint8_t *nt_hash, const char *mount_point);

// Public entry point, unchanged signature so no caller has to move. It hashes
// on the way in and wipes its own copy, so the cleartext exists only for the
// length of this frame and is never stored.
int smb_mount_auth(uint32_t server_ip, const char *share,
                   const char *domain, const char *username,
                   const char *password, const char *mount_point) {
    uint8_t nt[16];
    ntlm_hash(password ? password : "", nt);
    int r = smb_mount_auth_nt(server_ip, share, domain, username, nt, mount_point);
    secure_zero(nt, sizeof(nt));
    return r;
}

// #smbnt: the REAL implementation takes the NT hash. Callers that already hold
// a hash (the VFS default-credential paths) use this directly, so a hash never
// has to be turned back into anything password-shaped to satisfy an API.
// #749: connect + SMB2 handshake, in ONE place, shared by the first mount and
// by a reconnect.
//
// This body used to live inline in smb_mount_auth_nt(), which meant there was
// exactly one way to get a connection: mount a new one. When a server dropped
// an idle session, the mount stayed in the table with a dead socket and every
// later operation on that share failed forever with no way back. Reviving in
// place needs the same sixty lines, and this tree has already been bitten by
// near-verbatim copies of a network loop (blame.md records three copies of the
// send-all loop), so this is extracted rather than duplicated.
//
// It is also where the MessageId reset lives, inside the connect loop, so a
// revived connection cannot reach the server carrying the old count.
// THE SIGNING POLICY KNOB. Read once, from /CONFIG/SMB.CFG, on the first
// mount attempt of the boot.
//
// Why a lazy read here rather than at boot: /CONFIG lives on the ext2 root,
// which is mounted long after the early init that would otherwise be the
// natural home for this, and gui/clock.c already records what happens when a
// config read runs before its filesystem is up (it reported "not configured"
// on a machine that WAS configured, #192). A mount is by definition late
// enough.
//
// The parse itself is in rustkern/smbsign.rs so that the file format has one
// implementation and a self-test.
static void smbsign_policy_load_once(void) {
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;

    extern fat_fs_t g_fat_fs;

    // #192: no /CONFIG means storage is not where we think it is, which is not
    // the same as "no preference recorded". Say nothing and keep the default.
    if (fat_exists(&g_fat_fs, "/CONFIG") != 1) return;
    if (fat_exists(&g_fat_fs, SMB_SIGN_CFG_PATH) != 1) {
        if (cfgread_report_rs(SMB_SIGN_CFG_PATH, -1, CFG_OUTCOME_ABSENT) == CFG_LOG_NOTE) {
            kprintf("[SMB] no %s; SMB message signing AND encryption are both "
                    "REQUIRED (the defaults)\n", SMB_SIGN_CFG_PATH);
        }
        return;
    }

    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, SMB_SIGN_CFG_PATH, &size);
    if (!data || size == 0) {
        // Present a moment ago and unreadable now IS a fault, and it must not
        // quietly weaken the policy: the default stands.
        if (cfgread_report_rs(SMB_SIGN_CFG_PATH, -1, CFG_OUTCOME_IOERR) == CFG_LOG_WARN) {
            kprintf("[SMB] %s present but unreadable; keeping signing=required\n",
                    SMB_SIGN_CFG_PATH);
        }
        if (data) kfree(data);
        return;
    }

    int32_t p = smbsign_policy_parse((const uint8_t *)data, (int32_t)size);

    // THE ENCRYPTION POLICY LIVES IN THE SAME FILE and is read in the same
    // pass. One file, two independent settings: a mount can be signed and
    // unencrypted, which is what every mount was until this change.
    {
        int32_t e = smbcrypt_policy_parse((const uint8_t *)data, (int32_t)size);
    // THE DIALECT FLOOR, read from the SAME file as the other two settings.
    // Three settings in one file rather than three files, because three files
    // is three chances for one to be missing and for its default to be
    // mistaken for a decision.
    int32_t df = smbpreauth_floor_parse((const uint8_t *)data, (int32_t)size);
    if (df == 1) {
        smbpreauth_floor_set(SMB_PREAUTH_FLOOR_311);
        kprintf("[SMB] %s: min_dialect=3.1.1; the opening handshake will be "
                "AUTHENTICATED (pre-auth integrity) or the mount refused\n",
                SMB_SIGN_CFG_PATH);
    } else if (df == 0) {
        smbpreauth_floor_set(SMB_PREAUTH_FLOOR_NONE);
        kprintf("[SMB] %s: min_dialect=3.0.2. THE OPENING HANDSHAKE WILL NOT BE "
                "AUTHENTICATED on a server that selects a dialect below 3.1.1. "
                "Signing and encryption are unaffected; what is given up is "
                "any ability to DETECT a tampered negotiation.\n",
                SMB_SIGN_CFG_PATH);
    } else if (df == -2) {
        kprintf("[SMB] %s: a `min_dialect=` line with a value this kernel does "
                "not recognise (expected 3.1.1 or 3.0.2). Keeping "
                "min_dialect=3.1.1.\n", SMB_SIGN_CFG_PATH);
    }
        if (e == 2) {
            smbcrypt_policy_set(SMB_CRYPT_POLICY_REQUIRED);
            kprintf("[SMB] %s: encrypt=required\n", SMB_SIGN_CFG_PATH);
        } else if (e == 1) {
            smbcrypt_policy_set(SMB_CRYPT_POLICY_ENABLED);
            kprintf("[SMB] %s: encrypt=enabled. A server that CAN encrypt "
                    "still does; what is relaxed is the refusal of one that "
                    "cannot. Such a mount is NOT protected against being read "
                    "on the network, and every one says so when it comes "
                    "up.\n", SMB_SIGN_CFG_PATH);
        } else if (e == 0) {
            smbcrypt_policy_set(SMB_CRYPT_POLICY_OFF);
            kprintf("[SMB] %s: encrypt=off. NOTHING sent to or read from an "
                    "SMB share will be encrypted; it all crosses the network "
                    "in plain text.\n", SMB_SIGN_CFG_PATH);
        } else if (e == -2) {
            kprintf("[SMB] %s has an `encrypt=` line with a value this kernel "
                    "does not recognise. The only accepted values are "
                    "`required`, `enabled` and `off`. Keeping "
                    "encrypt=required.\n", SMB_SIGN_CFG_PATH);
        } else {
            kprintf("[SMB] %s has no `encrypt=` line; encryption is REQUIRED "
                    "(the default)\n", SMB_SIGN_CFG_PATH);
        }
    }

    kfree(data);

    if (p == 1) {
        smbsign_policy_set(SMB_SIGN_POLICY_REQUIRED);
        kprintf("[SMB] %s: signing=required\n", SMB_SIGN_CFG_PATH);
    } else if (p == 0) {
        smbsign_policy_set(SMB_SIGN_POLICY_ENABLED);
        kprintf("[SMB] %s: signing=enabled. Signatures that arrive are still "
                "VERIFIED; what is relaxed is the refusal of a server that "
                "offers no signing at all. Mounts to such a server are NOT "
                "protected against modification in flight.\n",
                SMB_SIGN_CFG_PATH);
    } else if (p == -2) {
        // A typo must NOT silently resolve to a policy. Name it and keep the
        // safer default, so the operator finds out from the log rather than
        // from an incident.
        kprintf("[SMB] %s has a `signing=` line with a value this kernel does "
                "not recognise. The only accepted values are `required` and "
                "`enabled`. Keeping signing=required.\n", SMB_SIGN_CFG_PATH);
    } else {
        kprintf("[SMB] %s has no `signing=` line; signing is REQUIRED (the "
                "default)\n", SMB_SIGN_CFG_PATH);
    }
}

static int smb_conn_establish(smb_connection_t *conn, const char *share) {
    uint32_t server_ip = conn->server_ip;

    smbsign_policy_load_once();

    // Pre-resolve ARP for the server BEFORE connecting (task #317). The TCP
    // layer gives up after 5 SYN retransmits (~0.9s); if ARP for the peer is
    // not yet cached, every SYN is dropped and the connection fails before ARP
    // completes. Warming the ARP cache first makes the very first SYN go out
    // with a known MAC, so connect is reliable.
    {
        extern int arp_resolve(uint32_t ip, uint8_t *mac);
        uint8_t pmac[6];
        uint64_t astart = sched_now_ms();
        while (!arp_resolve(server_ip, pmac)) {
            smb_net_pump();
            if (sched_now_ms() - astart > smb_ms_span(5000)) {
                kprintf("[SMB] ARP for server timed out\n");
                break;
            }
        }
    }

    // Connect to server, with retries and an ESCALATING backoff (#297/#317).
    // The compositor calls net_poll() every frame; when our worker also pumps
    // net_poll concurrently they can race the shared NIC/TCP state and drop our
    // SYN-ACK. Under rapid same-IP reboots the peer's neighbour/TCP state churns
    // and ARP may need to re-resolve. Each attempt therefore: (1) re-checks and
    // re-arms ARP if the entry is not cached, (2) opens a FRESH socket and closes
    // any failed one (no fd leak), (3) waits out a settle that widens for later
    // attempts. Bounded by SMB_CONNECT_ATTEMPTS and an overall deadline so a
    // genuinely dead server still fails in reasonable time.
    int connected = 0;
    uint64_t connect_deadline = sched_now_ms() + smb_ms_span(SMB_CONNECT_TOTAL_MS);
    for (int attempt = 0; attempt < SMB_CONNECT_ATTEMPTS && !connected; attempt++) {
        if (sched_now_ms() >= connect_deadline) break;   // overall hard bound

        // Re-arm ARP fresh if the neighbour entry is not (or no longer) cached.
        // arp_resolve() returns the cached MAC or fires a fresh who-has request
        // (a normal request, NOT a gratuitous announce, so it is safe on real
        // LAN hardware, #380) and returns 0. A failed prior attempt or a same-IP
        // reboot may have left no usable entry; warm it again before we open a
        // socket so the first SYN of this attempt goes out with a known MAC.
        {
            extern int arp_lookup_cached(uint32_t ip, uint8_t *mac);
            extern int arp_resolve(uint32_t ip, uint8_t *mac);
            uint8_t pmac[6];
            if (!arp_lookup_cached(server_ip, pmac)) {
                uint64_t astart = sched_now_ms();
                while (!arp_resolve(server_ip, pmac)) {
                    smb_net_pump();
                    if (sched_now_ms() - astart > smb_ms_span(SMB_ARP_REWARM_MS)) break;
                }
            }
        }

        conn->tcp_socket = tcp_socket();
        if (conn->tcp_socket < 0) {
            kprintf("[SMB] Failed to create TCP socket\n");
            break;
        }
        kprintf("[SMB] Connecting to ");
        ip_print(server_ip);
        kprintf(":%d (attempt %d/%d)\n", SMB_PORT, attempt + 1, SMB_CONNECT_ATTEMPTS);

        int cr = tcp_connect(conn->tcp_socket, server_ip, SMB_PORT);
        if (cr < 0 && cr != TCP_ERR_IN_PROGRESS) {
            tcp_close(conn->tcp_socket);
            smbseq_unbind(&conn->seq);   // #749: a sequence must never outlive its socket
            conn->tcp_socket = -1;
            int settle = smb_connect_settle(attempt);
            for (int k = 0; k < settle; k++) smb_net_pump();
            continue;
        }

        uint64_t cstart = sched_now_ms();
        uint64_t ctimeout = smb_ms_span(SMB_CONNECT_TIMEOUT_MS);
        while (!tcp_is_connected(conn->tcp_socket)) {
            smb_net_pump();
            if (tcp_get_state(conn->tcp_socket) == TCP_STATE_CLOSED) break;
            if (sched_now_ms() - cstart > ctimeout) break;
        }

        if (tcp_get_state(conn->tcp_socket) == TCP_STATE_ESTABLISHED) {
            // #749: THIS IS THE RESET, and it is the only one. MessageId and
            // the credit window are per TRANSPORT CONNECTION (MS-SMB2
            // 2.2.1.2), so the sequence is bound here, at the one instant a
            // connection becomes usable. "Re-established" and "reset" are
            // therefore the same event and cannot drift apart, which is the
            // difference between this and a reset call someone has to remember
            // to make. Until this runs, smbseq_next() refuses to number a
            // single request.
            // THE SIGNING KEY IS RESET AT THE SAME INSTANT, and for the same
            // reason: a signing key is scoped to an SMB2 SESSION, and a new
            // transport connection means a new session. Clearing it here, at
            // the one point a connection becomes usable, is what stops a
            // revived mount (smb_conn_revive) from carrying the dead session's
            // key onto a fresh one, where it would verify nothing the new
            // server signs and would make every response look tampered with.
            // Binding it to the same event as the MessageId reset is
            // deliberate: it cannot drift from "the connection is new".
            smbsign_reset(&conn->sign);
            // THE ENCRYPTION KEYS GO AT THE SAME INSTANT, for the same
            // reason AND one more: a nonce must never repeat under a key,
            // so a reset that threw away the counter but kept the keys
            // would be worse than no reset at all. smbcrypt_reset() clears
            // both together, and is bound to the same event as the
            // MessageId reset so it cannot drift from "the connection is
            // new".
            smbcrypt_reset(&conn->crypt);
            // THE PRE-AUTH INTEGRITY HASH GOES AT THE SAME INSTANT, and this
            // one is not optional for a reason the others are not: after the
            // keys are derived the hash is FROZEN, and a frozen hash refuses
            // every further fold by design, so a reconnect that kept it could
            // not even begin a new handshake.
            //
            // MEASURED, and this is how the bug was found: the first version
            // of this change put the reset in the wrong function, and the
            // #749 reconnect sub-test of the in-kernel SMB harness went from
            // PASS to FAIL with
            //   "could not fold the NEGOTIATE request: a message was folded
            //    into the pre-auth hash after the session keys were already
            //    derived from it"
            // on the SECOND connection. Nothing else noticed: the first mount
            // worked perfectly, every boot self-test passed, and the tamper
            // arms all behaved. Only an existing test that drops the transport
            // and reconnects caught it. Binding the reset to the same event as
            // the other three is what stops it drifting from "the connection
            // is new" again.
            smbpreauth_reset(&conn->preauth);
#ifndef SMBSEQ_NORESET
            smbseq_bind(&conn->seq, conn->tcp_socket);
#else
            // `make SMBSEQNORESET=1`: the #749 DEFECT, deliberately restored,
            // so the RED arm can be SEEN against a real server. Bind to the
            // new socket but CARRY THE OLD MessageId, which is exactly what a
            // bare `conn->message_id++` field did across a reconnect. Never in
            // a golden; see the Makefile block beside NOBLOCKTEST.
            {
                uint64_t carried = conn->seq.next_id;
                smbseq_bind(&conn->seq, conn->tcp_socket);
                if (carried != 0) {
                    conn->seq.next_id = carried;
                    kprintf("[SMB] SMBSEQ_NORESET: carrying MessageId %llu onto "
                            "the NEW connection (deliberate #749 defect)\n",
                            (unsigned long long)carried);
                }
            }
#endif
            connected = 1;
            break;
        }
        tcp_close(conn->tcp_socket);
        smbseq_unbind(&conn->seq);
        conn->tcp_socket = -1;
        int settle = smb_connect_settle(attempt);   // escalating settle
        for (int k = 0; k < settle; k++) smb_net_pump();
    }

    if (!connected) {
        kprintf("[SMB] Connection failed (all retries)\n");
        conn->active = false;
        return -1;
    }

    kprintf("[SMB] TCP connected\n");

    // Negotiate protocol
    if (smb2_negotiate(conn) < 0) {
        tcp_close(conn->tcp_socket);
        smbseq_unbind(&conn->seq);   // #749: a sequence must never outlive its socket
        conn->active = false;
        return -1;
    }

    // Session setup
    if (smb2_session_setup(conn) < 0) {
        tcp_close(conn->tcp_socket);
        smbseq_unbind(&conn->seq);   // #749: a sequence must never outlive its socket
        conn->active = false;
        return -1;
    }

    // Build UNC path: \\server\share
    char unc_path[SMB_MAX_PATH];
    snprintf(unc_path, SMB_MAX_PATH, "\\\\%d.%d.%d.%d\\%s",
             (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
             (server_ip >> 8) & 0xFF, server_ip & 0xFF, share);

    // Tree connect
    if (smb2_tree_connect(conn, unc_path) < 0) {
        smb2_logoff(conn);
        tcp_close(conn->tcp_socket);
        smbseq_unbind(&conn->seq);   // #749: a sequence must never outlive its socket
        conn->active = false;
        return -1;
    }

    return 0;
}

// #749: is the transport under this mount still usable?
//
// A mount is not alive just because it is in the table. The socket can be gone
// (the server closed an idle session; Samba does exactly that on its own
// `deadtime`), and the message sequence can be faulted by an unanswered
// request or a mismatched reply. Either way the next request must not go out
// on it.
static int smb_conn_transport_ok(const smb_connection_t *conn) {
    if (!conn->active || conn->tcp_socket < 0) return 0;
    if (tcp_get_state(conn->tcp_socket) != TCP_STATE_ESTABLISHED) return 0;
    if (!smbseq_is_healthy(&conn->seq)) return 0;
    return 1;
}

// #749: re-establish a mount whose transport has gone, keeping its mount point
// and credentials. THIS is the reconnect the MessageId reset exists for, and
// before this change the client had no such path at all: a dropped session
// left a mount that failed every operation forever.
//
// Open file handles do NOT survive. An SMB2 FileId is scoped to the session
// that created it and we do not request durable or persistent handles, so a
// stale FileId sent to a fresh session is a request for whatever file that id
// now means. They are invalidated here, loudly, rather than silently reused.
static int smb_conn_revive(smb_connection_t *conn) {
    kprintf("[SMB] %s: transport is gone (socket %d, sequence: %s); "
            "reconnecting\n", conn->mount_point, conn->tcp_socket,
            smbseq_fault_text(smbseq_fault(&conn->seq)));

    for (int j = 0; j < SMB_MAX_OPEN_FILES; j++) {
        if (open_files[j].active && open_files[j].conn == conn) {
            kprintf("[SMB] invalidating handle for %s: its SMB2 session is "
                    "gone, so its FileId no longer means that file\n",
                    open_files[j].path);
            open_files[j].active = false;
        }
    }

    if (conn->tcp_socket >= 0) {
        tcp_close(conn->tcp_socket);
#ifndef SMBSEQ_NORESET
        smbseq_unbind(&conn->seq);   // #749: a sequence must never outlive its socket
#else
        // `make SMBSEQNORESET=1`: do NOT unbind, so the old MessageId
        // survives into the new connection. The unbind is part of the
        // fix, not scaffolding around it: without it the counter has
        // nowhere to be forgotten, which is the defect.
#endif
        conn->tcp_socket = -1;
    }
    conn->session_established = false;
    conn->session_id = 0;
    conn->tree_connected = false;
    conn->tree_id = 0;

    if (smb_conn_establish(conn, conn->share_name) < 0) {
        // #749 pass 2: KEEP THE MOUNT, and keep its credentials with it.
        //
        // smb_conn_establish() clears `active` on every failure path, which is
        // right for a first mount (release the slot) and WRONG here: it took
        // the mount out of the table, so the next touch of that path fell
        // through smb_vfs_ensure() to an on-demand mount built from
        // g_smb_def_user, which outside the self-test is the hard-coded
        // "guest". The share then reappeared at the same path, worked, and
        // belonged to somebody else. Re-arming `active` keeps the configured
        // identity in the table so the next attempt retries as the SAME user.
        conn->active = true;
        kprintf("[SMB] %s: reconnect FAILED. The mount is KEPT (as %s\\%s) so a "
                "later attempt retries with the same identity; it is NOT "
                "replaced by an anonymous one. Operations on it fail until the "
                "server answers.\n",
                conn->mount_point, conn->domain, conn->username);
        return -1;
    }
    kprintf("[SMB] %s: reconnected; MessageId restarted at 0 for the new "
            "connection\n", conn->mount_point);
    return 0;
}

static int smb_mount_auth_nt(uint32_t server_ip, const char *share,
                             const char *domain, const char *username,
                             const uint8_t *nt_hash, const char *mount_point) {
    // #749 pass 2: a mount point belongs to ONE identity, enforced at the one
    // place a connection is allocated, so no caller can take a path over.
    //
    // Belt to smb_conn_revive()'s braces: that keeps a failed mount in the
    // table, and this refuses to put a second, differently-authenticated mount
    // on the same path even if some future caller manages to get past it. The
    // check is smbident_same() rather than strcmp so that "", "guest" and
    // "GUEST" are one identity and a retry is not mistaken for a takeover.
    {
        smb_connection_t *held = smb_find_connection(mount_point);
        if (held && !smbident_same(held->domain, held->username,
                                   domain ? domain : "WORKGROUP",
                                   username ? username : "guest")) {
            kprintf("[SMB] REFUSING to mount %s as %s\\%s: that path is already "
                    "mounted as %s\\%s. A mount point belongs to one identity; "
                    "replacing it in place would silently change who every "
                    "read and write on that path belongs to. Unmount it first "
                    "if that is what you want.\n",
                    mount_point, domain ? domain : "WORKGROUP",
                    username ? username : "guest",
                    held->domain, held->username);
            return -1;
        }
    }

    // Allocate connection
    int conn_idx = alloc_connection();
    if (conn_idx < 0) {
        kprintf("[SMB] No free connection slots\n");
        return -1;
    }

    smb_connection_t *conn = &connections[conn_idx];
    conn->active = true;
    conn->server_ip = server_ip;
    conn->server_port = SMB_PORT;
    strncpy(conn->domain, domain ? domain : "WORKGROUP", 63);
    strncpy(conn->username, username ? username : "guest", 63);
    memcpy(conn->nt_hash, nt_hash, 16);
    strncpy(conn->mount_point, mount_point, SMB_MAX_PATH - 1);

    strncpy(conn->share_name, share ? share : "", sizeof(conn->share_name) - 1);

    if (smb_conn_establish(conn, share) < 0) return -1;

    kprintf("[SMB] Share mounted at %s\n", mount_point);

    return conn_idx;
}

// Unmount SMB share
int smb_unmount(const char *mount_point) {
    for (int i = 0; i < SMB_MAX_CONNECTIONS; i++) {
        if (connections[i].active &&
            strcmp(connections[i].mount_point, mount_point) == 0) {

            smb_connection_t *conn = &connections[i];

            // Close all files on this connection
            for (int j = 0; j < SMB_MAX_OPEN_FILES; j++) {
                if (open_files[j].active && open_files[j].conn == conn) {
                    smb2_close_file(conn, &open_files[j]);
                    open_files[j].active = false;
                }
            }

            // Disconnect
            smb2_tree_disconnect(conn);
            smb2_logoff(conn);
            tcp_close(conn->tcp_socket);
            smbseq_unbind(&conn->seq);   // #749: a sequence must never outlive its socket
            conn->active = false;

            kprintf("[SMB] Unmounted %s\n", mount_point);
            return 0;
        }
    }

    return -1;
}

// Find connection by path
smb_connection_t *smb_find_connection(const char *path) {
    for (int i = 0; i < SMB_MAX_CONNECTIONS; i++) {
        if (connections[i].active) {
            int len = strlen(connections[i].mount_point);
            if (strncmp(path, connections[i].mount_point, len) == 0 &&
                (path[len] == '/' || path[len] == '\\' || path[len] == 0)) {
                return &connections[i];
            }
        }
    }
    return NULL;
}

// Check if path is on SMB mount
bool smb_is_smb_path(const char *path) {
    // Deliberately smb_find_connection(), NOT the reviving form below: this is
    // a cheap predicate that path routing calls on every open, and it must
    // never trigger a network reconnect as a side effect of being asked a
    // question.
    return smb_find_connection(path) != NULL;
}

// #749: find the connection for a path AND make sure it can actually be used.
//
// This is the chokepoint every operation that starts a new request goes
// through. smb_find_connection() answers "is this path on a mount", which is
// not the same question as "can I send a request right now", and conflating
// the two is what left a share permanently broken after the server dropped its
// session: the mount was still in the table, so every operation found it, used
// its dead socket, and failed.
//
// One place, so a new operation added later inherits the recovery instead of
// having to know it exists.
static smb_connection_t *smb_find_connection_live(const char *path) {
    smb_connection_t *conn = smb_find_connection(path);
    if (!conn) return NULL;
    if (smb_conn_transport_ok(conn)) return conn;
    if (smb_conn_revive(conn) < 0) return NULL;
    return conn;
}

// Get relative path within share
static const char *get_relative_path(smb_connection_t *conn, const char *path) {
    int len = strlen(conn->mount_point);
    if (strncmp(path, conn->mount_point, len) != 0) {
        return path;
    }
    path += len;
    while (*path == '/' || *path == '\\') path++;
    return path;
}

// Convert forward slashes to backslashes
static void normalize_path(const char *src, char *dst, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = (src[i] == '/') ? '\\' : src[i];
        i++;
    }
    dst[i] = 0;
}

// ============================================================================
// File Operations
// ============================================================================

// Open a file
int smb_open(const char *path, uint32_t desired_access, uint32_t disposition) {
    smb_connection_t *conn = smb_find_connection_live(path);
    if (!conn) {
        kprintf("[SMB] No mount found for path: %s\n", path);
        return -1;
    }

    int fd = alloc_file();
    if (fd < 0) {
        kprintf("[SMB] No free file slots\n");
        return -1;
    }

    smb_file_t *file = &open_files[fd];

    // Get relative path and normalize
    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    // Determine create options
    uint32_t create_options = FILE_NON_DIRECTORY_FILE;

    if (smb2_create(conn, norm_path, desired_access, disposition,
                    create_options, file) < 0) {
        return -1;
    }

    file->active = true;
    return fd;
}

// Close a file
int smb_close(int fd) {
    smb_file_t *file = get_file(fd);
    if (!file) return -1;

    smb2_close_file(file->conn, file);
    file->active = false;
    return 0;
}

// Read from file
ssize_t smb_read(int fd, void *buffer, size_t count) {
    smb_file_t *file = get_file(fd);
    if (!file) return -1;
    return smb2_read(file->conn, file, buffer, count);
}

// Write to file
ssize_t smb_write(int fd, const void *buffer, size_t count) {
    smb_file_t *file = get_file(fd);
    if (!file) return -1;
    return smb2_write(file->conn, file, buffer, count);
}

// Seek
int64_t smb_seek(int fd, int64_t offset, int whence) {
    smb_file_t *file = get_file(fd);
    if (!file) return -1;

    int64_t new_pos;
    switch (whence) {
        case SMB_SEEK_SET:
            new_pos = offset;
            break;
        case SMB_SEEK_CUR:
            new_pos = file->position + offset;
            break;
        case SMB_SEEK_END:
            new_pos = file->file_size + offset;
            break;
        default:
            return -1;
    }

    if (new_pos < 0) new_pos = 0;
    file->position = new_pos;
    return new_pos;
}

// ============================================================================
// Directory Operations
// ============================================================================

// Directory handle uses file slot with search state
typedef struct {
    smb_file_t dir_file;
    uint8_t enum_buffer[8192];
    uint32_t enum_offset;
    uint32_t enum_len;
    bool first_query;
} smb_dir_handle_t;

static smb_dir_handle_t dir_handles[16];
static int dir_handle_count = 0;

// Open directory
int smb_opendir(const char *path) {
    smb_connection_t *conn = smb_find_connection_live(path);
    if (!conn) return -1;

    // Reuse a freed slot. Pass 1 monotonically bumped dir_handle_count and
    // never reclaimed on closedir, so after 16 opendir calls (even with
    // matching closedirs) opendir failed. The Files app re-lists a directory on
    // every refresh, so slot reuse is mandatory (task #317 pass 2).
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (!dir_handles[i].dir_file.active) { slot = i; break; }
    }
    if (slot < 0) return -1;
    if (slot >= dir_handle_count) dir_handle_count = slot + 1;

    smb_dir_handle_t *dh = &dir_handles[slot];
    memset(dh, 0, sizeof(smb_dir_handle_t));

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    // SMB2 opens the share root with an EMPTY relative path (not "."); some
    // servers reject "." with OBJECT_NAME_INVALID.
    if (smb2_create(conn, norm_path,
                    FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                    FILE_OPEN,
                    FILE_DIRECTORY_FILE,
                    &dh->dir_file) < 0) {
        return -1;
    }

    dh->dir_file.active = true;
    dh->first_query = true;
    dh->enum_offset = 0;
    dh->enum_len = 0;

    return slot;
}

// Read directory entry
int smb_readdir(int dirfd, smb_dirent_t *entry) {
    if (dirfd < 0 || dirfd >= 16) return -1;

    smb_dir_handle_t *dh = &dir_handles[dirfd];
    if (!dh->dir_file.active) return -1;

    // Need more data?
    if (dh->enum_offset >= dh->enum_len) {
        uint8_t flags = dh->first_query ? SMB2_RESTART_SCANS : 0;
        dh->first_query = false;

        int ret = smb2_query_directory(dh->dir_file.conn, &dh->dir_file,
                                        "*", FileBothDirectoryInformation, flags,
                                        dh->enum_buffer, sizeof(dh->enum_buffer),
                                        &dh->enum_len);
        if (ret != 0) {
            return ret;  // 1 = end, -1 = error
        }
        dh->enum_offset = 0;
    }

    // Parse entry
    file_both_dir_info_t *info = (file_both_dir_info_t *)(dh->enum_buffer + dh->enum_offset);

    // Convert name from Unicode
    int name_chars __attribute__((unused)) = le32(info->file_name_length) / 2;
    uint16_t *name_unicode = (uint16_t *)((uint8_t*)info + sizeof(file_both_dir_info_t));
    unicode_to_ascii(name_unicode, le32(info->file_name_length), entry->name, SMB_MAX_NAME);

    entry->size = le64(info->end_of_file);
    entry->creation_time = smb_filetime_to_unix(le64(info->creation_time));
    entry->last_access_time = smb_filetime_to_unix(le64(info->last_access_time));
    entry->last_write_time = smb_filetime_to_unix(le64(info->last_write_time));
    entry->attributes = le32(info->file_attributes);
    entry->is_directory = (entry->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    // Move to next entry
    if (le32(info->next_entry_offset) == 0) {
        dh->enum_offset = dh->enum_len;  // Force refetch on next call
    } else {
        dh->enum_offset += le32(info->next_entry_offset);
    }

    return 0;
}

// Close directory
int smb_closedir(int dirfd) {
    if (dirfd < 0 || dirfd >= 16) return -1;

    smb_dir_handle_t *dh = &dir_handles[dirfd];
    if (!dh->dir_file.active) return -1;

    smb2_close_file(dh->dir_file.conn, &dh->dir_file);
    dh->dir_file.active = false;

    return 0;
}

// Create directory
int smb_mkdir(const char *path) {
    smb_connection_t *conn = smb_find_connection_live(path);
    if (!conn) return -1;

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    smb_file_t temp_file;
    if (smb2_create(conn, norm_path,
                    FILE_READ_ATTRIBUTES,
                    FILE_CREATE,
                    FILE_DIRECTORY_FILE,
                    &temp_file) < 0) {
        return -1;
    }

    smb2_close_file(conn, &temp_file);
    return 0;
}

// Remove directory
int smb_rmdir(const char *path) {
    smb_connection_t *conn = smb_find_connection_live(path);
    if (!conn) return -1;

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    smb_file_t temp_file;
    if (smb2_create(conn, norm_path,
                    DELETE,
                    FILE_OPEN,
                    FILE_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE,
                    &temp_file) < 0) {
        return -1;
    }

    smb2_close_file(conn, &temp_file);
    return 0;
}

// Delete file
int smb_delete(const char *path) {
    smb_connection_t *conn = smb_find_connection_live(path);
    if (!conn) return -1;

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    smb_file_t temp_file;
    if (smb2_create(conn, norm_path,
                    DELETE,
                    FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE,
                    &temp_file) < 0) {
        return -1;
    }

    smb2_close_file(conn, &temp_file);
    return 0;
}

// Get file info
int smb_stat(const char *path, smb_dirent_t *info) {
    smb_connection_t *conn = smb_find_connection_live(path);
    if (!conn) return -1;

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    smb_file_t temp_file;
    if (smb2_create(conn, norm_path,
                    FILE_READ_ATTRIBUTES,
                    FILE_OPEN,
                    0,  // Will auto-detect file/dir
                    &temp_file) < 0) {
        return -1;
    }

    // Extract info from create response
    info->size = temp_file.file_size;
    info->attributes = temp_file.attributes;
    info->is_directory = temp_file.is_directory;

    // Get the filename part
    const char *name = strrchr(path, '/');
    if (!name) name = strrchr(path, '\\');
    if (name) name++;
    else name = path;
    strncpy(info->name, name, SMB_MAX_NAME - 1);

    smb2_close_file(conn, &temp_file);
    return 0;
}

// ============================================================================
// Utility Functions
// ============================================================================

// Convert status to string
const char *smb_strerror(smb2_status_t status) {
    switch (status) {
        case STATUS_SUCCESS: return "Success";
        case STATUS_MORE_PROCESSING_REQUIRED: return "More processing required";
        case STATUS_INVALID_PARAMETER: return "Invalid parameter";
        case STATUS_NO_SUCH_FILE: return "No such file";
        case STATUS_END_OF_FILE: return "End of file";
        case STATUS_ACCESS_DENIED: return "Access denied";
        case STATUS_OBJECT_NAME_NOT_FOUND: return "Object name not found";
        case STATUS_OBJECT_NAME_COLLISION: return "Object name collision";
        case STATUS_OBJECT_PATH_NOT_FOUND: return "Object path not found";
        case STATUS_SHARING_VIOLATION: return "Sharing violation";
        case STATUS_LOGON_FAILURE: return "Logon failure";
        case STATUS_WRONG_PASSWORD: return "Wrong password";
        case STATUS_NO_SUCH_USER: return "No such user";
        case STATUS_BAD_NETWORK_NAME: return "Bad network name";
        case STATUS_NOT_SUPPORTED: return "Not supported";
        default: return "Unknown error";
    }
}

// Print mount info
void smb_print_mounts(void) {
    kprintf("SMB Mounts:\n");
    for (int i = 0; i < SMB_MAX_CONNECTIONS; i++) {
        if (connections[i].active) {
            kprintf("  %s -> \\\\", connections[i].mount_point);
            ip_print(connections[i].server_ip);
            kprintf("\\%s (dialect=0x%04x)\n",
                    connections[i].share_path, connections[i].dialect);
        }
    }
}

// ============================================================================
// VFS routing layer (task #317)
//
// Routes kernel/userland file access on the "/SMB/<server>/<share>/<path>"
// virtual prefix through the SMB2 client. A share is mounted on demand the
// first time it is touched. If the self-test (or a future NETMOUNTS.CFG
// consumer) already established an authenticated connection for that
// <server>/<share>, smb_find_connection reuses it; otherwise we fall back to
// a guest mount. This mirrors the ext2 "/ext2" prefix routing in fs/fat.c.
// ============================================================================

#include "../fs/fat.h"
extern fat_fs_t g_fat_fs;

// Default credentials applied to on-demand guest mounts can be overridden by
// the most recent explicit smb_mount_auth (stored per connection). Pass 2 will
// load these from /CONFIG/NETMOUNTS.CFG.
static char g_smb_def_user[64] = "guest";
// #smbnt: WAS `static char g_smb_def_pass[64]`. A cleartext SMB password sat in
// kernel BSS for the entire life of the boot, so it was readable in any crash
// dump or memory image of a running machine, quite apart from what was on disk.
// Now only the NT hash lives here.
static uint8_t g_smb_def_nthash[16] = {0};
static char g_smb_def_domain[64] = "WORKGROUP";

void smb_vfs_set_default_creds(const char *domain, const char *user, const char *secret) {
    strncpy(g_smb_def_domain, domain ? domain : "WORKGROUP", 63);
    strncpy(g_smb_def_user, user ? user : "guest", 63);
    // #smbdisk: same chokepoint, so a caller holding a persisted nt$ secret
    // does not have to turn it back into something password-shaped to satisfy
    // this API. A malformed tagged value falls back to the EMPTY password (the
    // guest default) rather than to the literal text, and says so.
    uint8_t nt[16];
    if (!smb_secret_to_nthash(secret, nt)) ntlm_hash("", nt);
    memcpy(g_smb_def_nthash, nt, 16);
    secure_zero(nt, sizeof(nt));
}

bool smb_vfs_is_smb_path(const char *path) {
    return path && (strncmp(path, "/SMB/", 5) == 0 || strncmp(path, "/smb/", 5) == 0);
}

// Ensure a connection exists for the share referenced by an /SMB path.
static smb_connection_t *smb_vfs_ensure(const char *path) {
    if (!smb_vfs_is_smb_path(path)) return NULL;

    smb_connection_t *c = smb_find_connection_live(path);
    if (c) return c;

    // #749 pass 2: THIS FALL-THROUGH IS THE DOWNGRADE, so it must only ever
    // run when there is no mount here at all.
    //
    // smb_find_connection_live() returns NULL for two completely different
    // reasons: "nothing is mounted on this path" (mount it on demand, which is
    // what the rest of this function is for) and "something IS mounted here
    // but could not be revived". Treating the second as the first is what
    // re-created the share as an anonymous guest mount at the same path: the
    // on-demand mount below uses g_smb_def_user, which outside the self-test
    // is the hard-coded "guest" with an empty NT hash.
    if (smb_find_connection(path)) {
        kprintf("[SMB] %s: the share is mounted but its connection could not "
                "be re-established. REFUSING to re-mount it on demand, because "
                "an on-demand mount uses the default (anonymous) credentials "
                "and this path is mounted as a named user. The reason the "
                "reconnect failed is logged above this line.\n", path);
        return NULL;
    }

    // Parse /SMB/<server>/<share>/...
    const char *p = path + 5;
    char server[64]; int si = 0;
    while (*p && *p != '/' && si < 63) server[si++] = *p++;
    server[si] = 0;
    if (*p != '/') return NULL;
    p++;
    char share[64]; int hi = 0;
    while (*p && *p != '/' && hi < 63) share[hi++] = *p++;
    share[hi] = 0;
    if (si == 0 || hi == 0) return NULL;

    uint32_t ip = resolve_hostname(server);
    if (!ip) return NULL;

    char mp[SMB_MAX_PATH];
    snprintf(mp, sizeof(mp), "/SMB/%s/%s", server, share);

    int idx = smb_mount_auth_nt(ip, share, g_smb_def_domain, g_smb_def_user,
                                g_smb_def_nthash, mp);
    if (idx < 0) return NULL;
    return &connections[idx];
}

// Whole-file read, fat_read_file-shaped (kmalloc'd, NUL-terminated buffer).
void *smb_vfs_read_whole(const char *path, uint32_t *size_out) {
    if (size_out) *size_out = 0;
    if (!smb_vfs_ensure(path)) return NULL;

    int fd = smb_open(path, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN);
    if (fd < 0) return NULL;

    smb_file_t *f = get_file(fd);
    uint64_t fsize64 = f ? f->file_size : 0;
    uint32_t cap = (uint32_t)fsize64;
    if (fsize64 > 0xF0000000ULL) { smb_close(fd); return NULL; }

    uint8_t *buf = (uint8_t *)kmalloc(cap + 1);
    if (!buf) { smb_close(fd); return NULL; }

    uint32_t got = 0;
    while (got < cap) {
        uint32_t want = cap - got;
        if (want > 16384) want = 16384;
        ssize_t r = smb_read(fd, buf + got, want);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    buf[got] = 0;
    smb_close(fd);
    if (size_out) *size_out = got;
    return buf;
}

// opendir that auto-mounts; readdir/closedir reuse smb_readdir/smb_closedir.
int smb_vfs_opendir(const char *path) {
    if (!smb_vfs_ensure(path)) return -1;
    return smb_opendir(path);
}

// Public IP resolver wrapper (resolve_hostname is file-static). Used by the
// share-enumeration syscall (task #317 pass 2).
uint32_t smb_resolve_ip(const char *host) {
    return host ? resolve_hostname(host) : 0;
}

// Mount an SMB share with explicit credentials, keyed by /SMB/<server>/<share>.
// Reuses an existing connection if already mounted. Returns 0 / -1. The Files
// app calls this (via SYS_NET_MOUNT) before navigating into a saved network
// location so the configured user/secret are used rather than the guest default
// (task #317 pass 2).
//
// #smbdisk: THIS IS THE SHARED CHOKEPOINT for both persisted credential forms.
// `secret` is either a cleartext password (legacy records, and whatever a
// caller passes straight from a dialog) or the `nt$<32 hex>` NT hash. Every
// path that arrives with a SAVED credential comes through here - the boot-time
// /CONFIG/NETMOUNTS.CFG loader and SYS_NET_MOUNT from the Files app - so the
// new form is understood ONCE, not once per path.
int smb_vfs_mount_creds(const char *server, const char *share,
                        const char *user, const char *secret) {
    if (!server || !share || !server[0] || !share[0]) return -1;

    char mp[SMB_MAX_PATH];
    snprintf(mp, sizeof(mp), "/SMB/%s/%s", server, share);
    if (smb_find_connection(mp)) return 0;   // already mounted

    uint32_t ip = resolve_hostname(server);
    if (!ip) return -1;

    smb_init();
    // Warm the net stack / ARP so the first SYN goes out with a known MAC.
    for (int i = 0; i < 100; i++) smb_net_pump();

    uint8_t nt[16];
    if (!smb_secret_to_nthash(secret, nt)) return -1;
    int idx = smb_mount_auth_nt(ip, share, "WORKGROUP",
                                (user && user[0]) ? user : "guest", nt, mp);
    secure_zero(nt, sizeof(nt));
    return idx >= 0 ? 0 : -1;
}

// ============================================================================
// Share enumeration via srvsvc RPC over the IPC$ \srvsvc named pipe
// (task #317 pass 2). Implements just enough DCE/RPC (BIND + one NetrShareEnum
// level-1 REQUEST) and a tolerant NDR walk of the response to pull out share
// names. Best-effort: returns NULL on any failure so callers fall back to
// manual entry.
// ============================================================================

static int rpc_put32(uint8_t *b, int o, uint32_t v) {
    b[o] = v & 0xFF; b[o+1] = (v>>8)&0xFF; b[o+2] = (v>>16)&0xFF; b[o+3] = (v>>24)&0xFF;
    return o + 4;
}
static int rpc_put16(uint8_t *b, int o, uint16_t v) {
    b[o] = v & 0xFF; b[o+1] = (v>>8)&0xFF; return o + 2;
}
static uint32_t rpc_get32(const uint8_t *b, int o) {
    return (uint32_t)b[o] | ((uint32_t)b[o+1]<<8) | ((uint32_t)b[o+2]<<16) | ((uint32_t)b[o+3]<<24);
}

char **smb_list_shares(uint32_t server_ip, int *count) {
    if (count) *count = 0;
    if (!server_ip) return NULL;

    smb_init();
    for (int i = 0; i < 100; i++) smb_net_pump();

    char mp[SMB_MAX_PATH];
    snprintf(mp, sizeof(mp), "/SMB-IPC/%08x", (unsigned)server_ip);
    int idx = smb_mount_auth_nt(server_ip, "IPC$", g_smb_def_domain,
                                g_smb_def_user, g_smb_def_nthash, mp);
    if (idx < 0) { kprintf("[SMB] list_shares: IPC$ connect failed\n"); return NULL; }
    smb_connection_t *conn = &connections[idx];

    smb_file_t pipe;
    memset(&pipe, 0, sizeof(pipe));
    if (smb2_create(conn, "srvsvc",
                    FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
                    FILE_OPEN, 0, &pipe) < 0) {
        kprintf("[SMB] list_shares: open \\srvsvc failed\n");
        smb_unmount(mp); return NULL;
    }
    pipe.active = true;

    // ---- DCE/RPC BIND --------------------------------------------------------
    static uint8_t rb[2048];
    int o = 0;
    rb[o++] = 5; rb[o++] = 0; rb[o++] = 11; rb[o++] = 0x03;      // ver5.0, BIND, first+last
    rb[o++] = 0x10; rb[o++] = 0; rb[o++] = 0; rb[o++] = 0;       // DREP little-endian
    int frag_off = o; o = rpc_put16(rb, o, 0);                   // frag_length (patch)
    o = rpc_put16(rb, o, 0);                                     // auth_length
    o = rpc_put32(rb, o, 1);                                     // call_id
    o = rpc_put16(rb, o, 4280);                                  // max_xmit
    o = rpc_put16(rb, o, 4280);                                  // max_recv
    o = rpc_put32(rb, o, 0);                                     // assoc_group
    rb[o++] = 1; rb[o++] = 0; rb[o++] = 0; rb[o++] = 0;          // num_ctx=1 + reserved
    o = rpc_put16(rb, o, 0);                                     // context_id=0
    rb[o++] = 1; rb[o++] = 0;                                    // num_trans=1 + reserved
    // SRVSVC interface UUID 4b324fc8-1670-01d3-1278-5a47bf6ee188 (little-endian
    // Data1/2/3). A prior typo (0x4F written as 0xFC) made the BIND get rejected
    // with "abstract syntax not supported" -> nca_unk_if (0x1c010003).
    { static const uint8_t srv[16] = {0xC8,0x4F,0x32,0x4B,0x70,0x16,0xD3,0x01,
                                       0x12,0x78,0x5A,0x47,0xBF,0x6E,0xE1,0x88};
      for (int i=0;i<16;i++) rb[o++]=srv[i]; }                   // SRVSVC UUID
    o = rpc_put16(rb, o, 3); o = rpc_put16(rb, o, 0);           // version 3.0
    { static const uint8_t ndr[16] = {0x04,0x5D,0x88,0x8A,0xEB,0x1C,0xC9,0x11,
                                       0x9F,0xE8,0x08,0x00,0x2B,0x10,0x48,0x60};
      for (int i=0;i<16;i++) rb[o++]=ndr[i]; }                   // NDR UUID
    o = rpc_put32(rb, o, 2);                                     // NDR version 2
    rpc_put16(rb, frag_off, (uint16_t)o);

    if (smb2_write(conn, &pipe, rb, o) < 0) { smb2_close_file(conn,&pipe); smb_unmount(mp); return NULL; }
    pipe.position = 0;
    static uint8_t resp[8192];
    ssize_t rn = smb2_read(conn, &pipe, resp, sizeof(resp));
    if (rn < 16 || resp[2] != 12) {   // 12 = BIND_ACK
        kprintf("[SMB] list_shares: bind failed (ptype=%d n=%d)\n", rn>2?resp[2]:-1, (int)rn);
        smb2_close_file(conn,&pipe); smb_unmount(mp); return NULL;
    }
    // A BIND_ACK can still REJECT the presentation context (e.g. wrong interface
    // UUID/version -> ack_result=2 "provider rejection", later nca_unk_if). Parse
    // the first result's ack_result and bail clearly if it is not acceptance (0).
    {
        int sec_len = (rn > 25) ? (resp[24] | (resp[25] << 8)) : 0;
        int rp = (26 + sec_len + 3) & ~3;          // skip sec_addr, align to 4
        if (rp + 6 <= (int)rn) {
            int ack = resp[rp+4] | (resp[rp+5] << 8);
            if (ack != 0) {
                kprintf("[SMB] list_shares: bind rejected (ack_result=%d)\n", ack);
                smb2_close_file(conn,&pipe); smb_unmount(mp); return NULL;
            }
        }
    }

    // ---- NetrShareEnum REQUEST (opnum 15, level 1) ---------------------------
    o = 0;
    rb[o++] = 5; rb[o++] = 0; rb[o++] = 0; rb[o++] = 0x03;       // REQUEST, first+last
    rb[o++] = 0x10; rb[o++] = 0; rb[o++] = 0; rb[o++] = 0;
    int frag_off2 = o; o = rpc_put16(rb, o, 0);                  // frag_length (patch)
    o = rpc_put16(rb, o, 0);                                     // auth_length
    o = rpc_put32(rb, o, 2);                                     // call_id
    int alloc_off = o; o = rpc_put32(rb, o, 0);                  // alloc_hint (patch)
    o = rpc_put16(rb, o, 0);                                     // context_id
    // opnum 36 = NetShareEnum on Samba's srvsvc interface (verified against
    // Samba 4.17 with rpcclient netshareenumall; opnum 15 returned
    // nca_s_fault_ndr because it maps to a different function here).
    o = rpc_put16(rb, o, 36);                                    // opnum = NetShareEnum
    int stub_start = o;
    // ServerName: unique ptr -> conformant-varying wchar string "\\<ip>".
    // (A NULL ServerName makes Samba return nca_s_fault_ndr.)
    char sname[40];
    snprintf(sname, sizeof(sname), "\\\\%u.%u.%u.%u",
             (server_ip>>24)&0xFF, (server_ip>>16)&0xFF,
             (server_ip>>8)&0xFF, server_ip&0xFF);
    int slen = (int)strlen(sname) + 1;                          // include NUL terminator
    o = rpc_put32(rb, o, 0x00020000);                          // ServerName referent (non-null)
    o = rpc_put32(rb, o, (uint32_t)slen);                      // max_count (wchars)
    o = rpc_put32(rb, o, 0);                                    // offset
    o = rpc_put32(rb, o, (uint32_t)slen);                      // actual_count
    for (int i = 0; i < slen; i++) { rb[o++] = sname[i]; rb[o++] = 0; }  // UTF-16LE incl NUL
    while (o & 3) rb[o++] = 0;                                  // align to 4 bytes
    o = rpc_put32(rb, o, 1);                                     // Level = 1
    o = rpc_put32(rb, o, 1);                                     // union discriminant = 1
    o = rpc_put32(rb, o, 0x00020004);                          // CONTAINER referent (non-null)
    o = rpc_put32(rb, o, 0);                                     // EntriesRead = 0
    o = rpc_put32(rb, o, 0);                                     // Buffer = NULL
    o = rpc_put32(rb, o, 0xFFFFFFFF);                           // PreferredMaximumLength
    // ResumeHandle as a NULL unique pointer (no referent, no value), matching
    // the proven rpcclient request layout.
    o = rpc_put32(rb, o, 0);                                     // ResumeHandle = NULL
    rpc_put32(rb, alloc_off, (uint32_t)(o - stub_start));
    rpc_put16(rb, frag_off2, (uint16_t)o);

    if (smb2_write(conn, &pipe, rb, o) < 0) { smb2_close_file(conn,&pipe); smb_unmount(mp); return NULL; }
    pipe.position = 0;
    rn = smb2_read(conn, &pipe, resp, sizeof(resp));
    smb2_close_file(conn, &pipe);
    smb_unmount(mp);
    if (rn < 24) { kprintf("[SMB] list_shares: short response %d\n", (int)rn); return NULL; }

    // A DCE/RPC FAULT response (ptype 3) means the server rejected the request
    // (e.g. nca_s_fault_ndr if the input NDR is malformed). Bail gracefully.
    if (resp[2] == 3) {
        kprintf("[SMB] list_shares: server FAULT (status 0x%08x)\n",
                (unsigned)rpc_get32(resp, 24));
        return NULL;
    }

    // ---- tolerant NDR walk ---------------------------------------------------
    int p = 24;                                                  // skip RPC response header
    if (p + 24 > (int)rn) return NULL;
    p += 4;                                                      // Level
    p += 4;                                                      // union discriminant tag
    p += 4;                                                      // container referent id
    uint32_t entries = rpc_get32(resp, p); p += 4;              // EntriesRead
    p += 4;                                                      // Buffer referent id
    if (entries == 0 || entries > 256) return NULL;
    p += 4;                                                      // array max_count
    p += (int)(entries * 12);                                    // skip SHARE_INFO_1[] structs

    char **out = (char **)kmalloc(sizeof(char *) * entries);
    if (!out) return NULL;
    int found = 0;
    for (uint32_t s = 0; s < entries * 2 && p + 12 <= (int)rn; s++) {
        uint32_t actc = rpc_get32(resp, p + 8);                 // actual_count
        p += 12;
        if (actc > 1024) break;
        int bytes = (int)actc * 2;
        if (p + bytes > (int)rn) break;
        if ((s & 1) == 0 && found < (int)entries) {             // even = netname
            char nm[SMB_MAX_NAME];
            unicode_to_ascii((const uint16_t *)(resp + p), bytes, nm, sizeof(nm));
            int L = strlen(nm);
            char *dup = (char *)kmalloc(L + 1);
            if (dup) { for (int i=0;i<=L;i++) dup[i]=nm[i]; out[found++] = dup; }
        }
        p += bytes;
        if (p & 3) p += 4 - (p & 3);                            // 4-byte align
    }
    if (found == 0) { kfree(out); return NULL; }
    if (count) *count = found;
    return out;
}

void smb_free_shares(char **shares, int count) {
    if (!shares) return;
    for (int i = 0; i < count; i++) if (shares[i]) kfree(shares[i]);
    kfree(shares);
}

// Public on-demand mount for an /SMB path (task #317 pass 2). Returns 0 if a
// connection for the share is available (mounting it if needed), -1 otherwise.
// The syscall layer calls this before smb_stat/smb_open/smb_opendir so the
// connection is guaranteed to exist.
int smb_vfs_ensure_mount(const char *path) {
    return smb_vfs_ensure(path) ? 0 : -1;
}

// Whole-file write/upload, fat_create+write-shaped (task #317 pass 2). Truncates
// or creates the target (FILE_OVERWRITE_IF) and streams `data`[len] to it in
// chunks bounded by the negotiated max_write_size AND the 64 KB send buffer.
// Returns 0 on success, -1 on error. This is the upload chokepoint used by the
// syscall write-on-close path so userland copy_file()/editors write to /SMB.
int smb_vfs_write_whole(const char *path, const void *data, uint32_t len) {
    if (!smb_vfs_ensure(path)) return -1;

    int fd = smb_open(path, FILE_WRITE_DATA | FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                      FILE_OVERWRITE_IF);
    if (fd < 0) return -1;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t off = 0;
    int ok = 1;
    while (off < len) {
        uint32_t want = len - off;
        if (want > 32768) want = 32768;   // stay well within smb_send_buffer (64 KB)
        ssize_t w = smb_write(fd, p + off, want);
        if (w <= 0) { ok = 0; break; }
        off += (uint32_t)w;
    }
    smb_close(fd);
    return ok ? 0 : -1;
}

// ============================================================================
// Boot self-test (task #317) - gated on /CONFIG/SMBTEST.CFG.
// Format (one key=value per line):
//   ip=192.0.2.219
//   share=share
//   user=maytera
//   pass=maytera
//   file=TEST.TXT
// Prints clearly-delimited results to serial. Safe to leave compiled in:
// does nothing when the config file is absent.
// ============================================================================

static void smbtest_get(const char *buf, const char *key, char *out, int outsz) {
    out[0] = 0;
    int klen = strlen(key);
    const char *p = buf;
    while (*p) {
        // start of line
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            int i = 0;
            while (*p && *p != '\n' && *p != '\r' && i < outsz - 1) out[i++] = *p++;
            out[i] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

// ============================================================================
// Persistent network mounts (task #317 pass 4) - /CONFIG/NETMOUNTS.CFG.
// One mount per line, pipe-delimited, so the Files "Network" menu can append
// entries that re-mount automatically on the next boot:
//   SMB|server|share|user|pass        (user/pass optional -> guest)
//   NFS|server|/export/path
//   label|server|share|user|pass      (legacy Files "Add" format = SMB)
// Lines starting with '#' are comments. Loaded once by the deferred net worker
// (scheduler + net up), pre-creating the connections so /SMB/<server>/<share>
// and /NFS/<server>/<export-basename> resolve without an explicit mount.
// ============================================================================
#define NET_MAX_MOUNTS 16
static char g_net_mount_paths[NET_MAX_MOUNTS][160];
static int  g_net_mount_count = 0;
int net_mounts_count(void) { return g_net_mount_count; }
const char *net_mounts_path(int i) {
    return (i >= 0 && i < g_net_mount_count) ? g_net_mount_paths[i] : 0;
}

// ---------------------------------------------------------------------------
// #smbdisk: migrate /CONFIG/NETMOUNTS.CFG from cleartext passwords to nt$.
//
// MIGRATION IS THE RISKY HALF OF THIS CHANGE, so the rules are explicit:
//
//   - NOTHING IS EVER UNLINKED. #peruser2 had to remove exactly that pattern
//     at five sites: unlink-then-write turns a failed migration into DATA LOSS
//     on the failure path.
//   - THE TRANSFORM IS LINE-PRESERVING. Only the 5th pipe-separated field of a
//     5-field record changes. Comments, blank lines, NFS records and anything
//     this code does not recognise are copied through BYTE FOR BYTE. In
//     particular the new file is NOT rebuilt from the parsed mount table: that
//     table is capped at NET_MAX_MOUNTS (16) while the Files app allows 24
//     records, so rebuilding from it would silently DELETE the 17th saved
//     location onwards. That would be losing a mount to a credential change.
//   - THE RESULT IS VERIFIED, AND ROLLED BACK IF IT IS WRONG. After the write
//     the file is read back and compared; on any mismatch the ORIGINAL bytes,
//     still in memory, are written back. The only unrecoverable case left is
//     both writes failing, which is a failing disk rather than a logic error.
//   - It runs at BOOT, so an existing install converts on its first boot with
//     this kernel instead of whenever somebody next opens the Files app. A
//     credential-at-rest fix that waits for a GUI visit is not a fix yet.
//
// fat_write_file() is the established kernel-side whole-file config write in
// this tree (/CONFIG/PASSWD, /CONFIG/SHADOW, /CONFIG/GROUP, /CONFIG/PERMS.DB
// all use it, and it routes to ext2 when the path lives there); it is reused
// rather than replaced.
// ---------------------------------------------------------------------------
static int netmounts_migrate_text(const char *in, uint32_t insz,
                                  char *out, uint32_t outcap, int *changed) {
    uint32_t o = 0, i = 0;
    *changed = 0;
    while (i < insz) {
        uint32_t ls = i;
        while (i < insz && in[i] != '\n' && in[i] != '\r') i++;
        uint32_t le = i;                      /* end of the text of the line */
        uint32_t eol = i;
        while (eol < insz && (in[eol] == '\n' || in[eol] == '\r')) eol++;

        int emitted = 0;
        if (le > ls && in[ls] != '#') {
            uint32_t p[8]; int np = 0;
            for (uint32_t k = ls; k < le && np < 8; k++) if (in[k] == '|') p[np++] = k;
            /* 5 fields = 4 pipes; the secret is field 5. Both accepted 5-field
               forms put it there: "SMB|server|share|user|secret" and the legacy
               Files form "label|server|share|user|secret". */
            if (np >= 4) {
                uint32_t fs = p[3] + 1, fe = (np >= 5) ? p[4] : le;
                uint32_t flen = fe - fs;
                int is_nfs = (le - ls >= 4 && in[ls] == 'N' && in[ls+1] == 'F' &&
                              in[ls+2] == 'S' && in[ls+3] == '|');
                char fld[128];
                if (!is_nfs && flen > 0 && flen < sizeof(fld)) {
                    memcpy(fld, in + fs, flen); fld[flen] = 0;
                    if (!nt_secret_is_tagged(fld)) {
                        uint8_t nt[16]; char tag[NT_SECRET_BUF];
                        ntlm_hash(fld, nt);
                        if (nt_secret_format(nt, tag, sizeof(tag))) {
                            uint32_t tl = (uint32_t)strlen(tag);
                            uint32_t need = (fs - ls) + tl + (le - fe) + (eol - le);
                            if (o + need + 1 >= outcap) {
                                secure_zero(nt, sizeof(nt));
                                secure_zero(fld, sizeof(fld));
                                return -1;
                            }
                            memcpy(out + o, in + ls, fs - ls); o += fs - ls;
                            memcpy(out + o, tag, tl);          o += tl;
                            memcpy(out + o, in + fe, le - fe); o += le - fe;
                            memcpy(out + o, in + le, eol - le); o += eol - le;
                            emitted = 1; *changed = 1;
                        }
                        secure_zero(nt, sizeof(nt));
                    }
                    secure_zero(fld, sizeof(fld));
                }
            }
        }
        if (!emitted) {
            uint32_t need = eol - ls;
            if (o + need + 1 >= outcap) return -1;
            memcpy(out + o, in + ls, need); o += need;
        }
        i = eol;
    }
    out[o] = 0;
    return (int)o;
}

static void netmounts_migrate_on_disk(const char *cfg, uint32_t sz) {
    uint32_t cap = sz * 2 + 256;   /* nt$ + 32 hex is longer than most passwords */
    char *nb = (char *)kmalloc(cap);
    if (!nb) {
        kprintf("[NETMOUNTS] migration SKIPPED: no memory. File left exactly as it was.\n");
        return;
    }
    int changed = 0;
    int nlen = netmounts_migrate_text(cfg, sz, nb, cap, &changed);
    if (nlen < 0) {
        kprintf("[NETMOUNTS] migration REFUSED: the converted file would not fit. "
                "File left exactly as it was.\n");
    } else if (!changed) {
        kprintf("[NETMOUNTS] on-disk credentials are already hashed (nt$); nothing to migrate\n");
    } else if (fat_write_file(&g_fat_fs, "/CONFIG/NETMOUNTS.CFG", nb, (uint32_t)nlen) != 0) {
        kprintf("[NETMOUNTS] MIGRATION WRITE FAILED. Nothing was unlinked; the saved "
                "locations are still mounted from the copy already in memory.\n");
    } else {
        uint32_t vsz = 0;
        char *v = (char *)fat_read_file(&g_fat_fs, "/CONFIG/NETMOUNTS.CFG", &vsz);
        int ok = (v && vsz == (uint32_t)nlen && memcmp(v, nb, vsz) == 0);
        if (v) kfree(v);
        if (ok) {
            kprintf("[NETMOUNTS] MIGRATED cleartext -> nt$ (%u -> %d bytes), "
                    "verified by readback\n", sz, nlen);
        } else {
            kprintf("[NETMOUNTS] MIGRATION READBACK MISMATCH - rolling back to the "
                    "original bytes\n");
            if (fat_write_file(&g_fat_fs, "/CONFIG/NETMOUNTS.CFG", cfg, sz) == 0)
                kprintf("[NETMOUNTS] rollback OK: no saved location was lost\n");
            else
                kprintf("[NETMOUNTS] ROLLBACK ALSO FAILED - /CONFIG/NETMOUNTS.CFG may "
                        "be damaged. The mounts for THIS boot are unaffected.\n");
        }
    }
    secure_zero(nb, cap);
    kfree(nb);
}

void net_mounts_load(void) {
    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/NETMOUNTS.CFG", &sz);
    if (!cfg) return;  // no persisted mounts -> silent

    kprintf("\n========== NETMOUNTS (task #317 pass 4) ==========\n");
    smb_init();
    for (int i = 0; i < 100; i++) smb_net_pump();   // warm ARP/net

    char *p = cfg;
    while (*p) {
        char line[256]; int ll = 0;
        while (*p && *p != '\n' && *p != '\r' && ll < 255) line[ll++] = *p++;
        line[ll] = 0;
        while (*p == '\n' || *p == '\r') p++;
        if (line[0] == 0 || line[0] == '#') continue;

        char *f[6]; int nf = 0; char *q = line;
        f[nf++] = q;
        while (*q && nf < 6) { if (*q == '|') { *q = 0; q++; f[nf++] = q; } else q++; }

        if (strcmp(f[0], "SMB") == 0 && nf >= 3) {
            const char *server = f[1], *share = f[2];
            const char *user = (nf >= 4) ? f[3] : "guest";
            const char *pass = (nf >= 5) ? f[4] : "";
            if (smb_vfs_mount_creds(server, share, user, pass) == 0) {
                if (g_net_mount_count < NET_MAX_MOUNTS)
                    snprintf(g_net_mount_paths[g_net_mount_count++], 160,
                             "/SMB/%s/%s", server, share);
                kprintf("[NETMOUNTS] SMB %s/%s mounted\n", server, share);
            } else {
                kprintf("[NETMOUNTS] SMB %s/%s FAILED\n", server, share);
            }
        } else if (strcmp(f[0], "NFS") == 0 && nf >= 3) {
            const char *server = f[1], *expt = f[2];
            char mp[160];
            extern int nfs_vfs_mount(const char *, const char *, char *, int);
            if (nfs_vfs_mount(server, expt, mp, sizeof(mp)) == 0) {
                if (g_net_mount_count < NET_MAX_MOUNTS) {
                    char *d = g_net_mount_paths[g_net_mount_count++];
                    int i = 0; while (mp[i] && i < 159) { d[i] = mp[i]; i++; } d[i] = 0;
                }
                kprintf("[NETMOUNTS] NFS %s:%s mounted at %s\n", server, expt, mp);
            } else {
                kprintf("[NETMOUNTS] NFS %s:%s FAILED\n", server, expt);
            }
        } else if (nf >= 3) {
            // Legacy Files "Add" format: label|server|share|user|pass (= SMB).
            const char *server = f[1], *share = f[2];
            const char *user = (nf >= 4) ? f[3] : "guest";
            const char *pass = (nf >= 5) ? f[4] : "";
            if (smb_vfs_mount_creds(server, share, user, pass) == 0) {
                if (g_net_mount_count < NET_MAX_MOUNTS)
                    snprintf(g_net_mount_paths[g_net_mount_count++], 160,
                             "/SMB/%s/%s", server, share);
                kprintf("[NETMOUNTS] SMB(legacy) %s/%s mounted\n", server, share);
            } else {
                kprintf("[NETMOUNTS] SMB(legacy) %s/%s FAILED\n", server, share);
            }
        }
    }
    // #smbdisk: the mounts above are already established from the copy in
    // memory, so a migration failure below cannot cost this boot a mount.
    netmounts_migrate_on_disk(cfg, sz);
    kfree(cfg);
    kprintf("[NETMOUNTS] %d mount(s) loaded\n", g_net_mount_count);
    kprintf("==================================================\n\n");
}

void smb_run_selftest(void) {
    uint32_t cfgsz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/SMBTEST.CFG", &cfgsz);
    if (!cfg) return;  // no config -> silent

    char ipstr[64], share[64], user[64], pass[64], file[128], bigfile[128];
    smbtest_get(cfg, "ip", ipstr, sizeof(ipstr));
    smbtest_get(cfg, "share", share, sizeof(share));
    smbtest_get(cfg, "user", user, sizeof(user));
    smbtest_get(cfg, "pass", pass, sizeof(pass));
    smbtest_get(cfg, "file", file, sizeof(file));
    // Optional. Absent means the timed bulk read is skipped entirely, so a rig
    // that does not have a large file is unaffected.
    smbtest_get(cfg, "bigfile", bigfile, sizeof(bigfile));
    kfree(cfg);

    if (!ipstr[0] || !share[0]) {
        kprintf("[SMBTEST] config present but missing ip/share\n");
        return;
    }
    if (!user[0]) strcpy(user, "guest");
    if (!file[0]) strcpy(file, "TEST.TXT");

    kprintf("\n========== SMB SELFTEST (task #317) ==========\n");
    kprintf("[SMBTEST] target=%s share=%s user=%s file=%s\n",
            ipstr, share, user, file);

    smb_init();

    uint32_t ip = resolve_hostname(ipstr);
    if (!ip) { kprintf("[SMBTEST] bad ip\n========== SMB SELFTEST: FAIL ==========\n"); return; }

    // Warm up the stack so ARP for the server settles.
    for (int i = 0; i < 200; i++) smb_net_pump();

    char mp[SMB_MAX_PATH];
    snprintf(mp, sizeof(mp), "/SMB/%s/%s", ipstr, share);

    // 1) Mount: TCP connect + NEGOTIATE + SESSION_SETUP(NTLM) + TREE_CONNECT
    int idx = smb_mount_auth(ip, share, "WORKGROUP", user, pass, mp);
    if (idx < 0) {
        kprintf("[SMBTEST] mount FAILED\n========== SMB SELFTEST: FAIL ==========\n");
        return;
    }
    smb_connection_t *conn = &connections[idx];
    kprintf("[SMBTEST] MOUNT OK: dialect=0x%04x tree_id=%u max_read=%u\n",
            conn->dialect, conn->tree_id, conn->max_read_size);

    // 2) Enumerate the share root.
    kprintf("[SMBTEST] --- directory listing of %s ---\n", mp);
    int dir = smb_opendir(mp);
    int nent = 0;
    if (dir >= 0) {
        smb_dirent_t de;
        int r;
        while ((r = smb_readdir(dir, &de)) == 0 && nent < 64) {
            kprintf("[SMBTEST]   %s %s  %u bytes\n",
                    de.is_directory ? "<DIR> " : "<FILE>", de.name,
                    (uint32_t)de.size);
            nent++;
        }
        smb_closedir(dir);
        kprintf("[SMBTEST] enumerated %d entries\n", nent);
    } else {
        kprintf("[SMBTEST] opendir FAILED\n");
    }

    // 3) Read the test file directly via smb_open/smb_read.
    char fpath[SMB_MAX_PATH];
    snprintf(fpath, sizeof(fpath), "%s/%s", mp, file);
    kprintf("[SMBTEST] --- reading %s ---\n", fpath);
    int read_ok = 0;
    // #smbdisk: hoisted out of the block so step 4 can compare the two
    // independent read paths byte for byte. `got > 0` alone would pass on a
    // truncated or wrong-content read, which is most of what could break.
    static uint8_t fbuf[4096];
    ssize_t direct_len = 0;
    int fd = smb_open(fpath, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN);
    if (fd >= 0) {
        ssize_t got = smb_read(fd, fbuf, sizeof(fbuf) - 1);
        if (got > 0) {
            fbuf[got] = 0;
            kprintf("[SMBTEST] read %d bytes:\n", (int)got);
            kprintf("----8<----\n%s\n----8<----\n", (char *)fbuf);
            read_ok = 1;
            direct_len = got;
        } else {
            kprintf("[SMBTEST] read returned %d\n", (int)got);
        }
        smb_close(fd);
    } else {
        kprintf("[SMBTEST] open FAILED\n");
    }

    // 3b) TIMED BULK READ, so the COST of encryption is a measured number.
    //
    // It reads one large file in fixed chunks and reports bytes, milliseconds
    // and KB/s, together with whether encryption was in force. Run the same
    // image twice, once with `encrypt=off` in /CONFIG/SMB.CFG and once without
    // it, and the difference between the two lines IS the cost of encryption
    // on a real read of a real file from a real server: same kernel, same VM,
    // same share, one variable.
    //
    // Reported rather than asserted. A throughput threshold in a self-test is
    // a flaky test on a shared host, and this host routinely runs at a load
    // average above 20.
    if (bigfile[0]) {
        char bpath[SMB_MAX_PATH];
        snprintf(bpath, sizeof(bpath), "%s/%s", mp, bigfile);
        int bfd = smb_open(bpath, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN);
        if (bfd >= 0) {
            static uint8_t chunk[32768];
            uint64_t t0 = sched_now_ms();
            uint64_t total = 0;
            int reads = 0;
            for (;;) {
                ssize_t got = smb_read(bfd, chunk, sizeof(chunk));
                if (got <= 0) break;
                total += (uint64_t)got;
                reads++;
                if (reads > 4096) break;   // a bound, not a poll: see above
            }
            uint64_t ms = sched_now_ms() - t0;
            smb_close(bfd);
            uint32_t kbs = (ms > 0) ? (uint32_t)((total * 1000ull) / (ms * 1024ull))
                                    : 0;
            smb_connection_t *bc = (idx >= 0) ? &connections[idx] : NULL;
            uint32_t nse = 0, nun = 0, nrj = 0, nco = 0, nci = 0;
            if (bc) smbcrypt_counts(&bc->crypt, &nse, &nun, &nrj, &nco, &nci);
            kprintf("[SMBTEST] BULKREAD %s: %llu bytes in %llu ms over %d "
                    "reads = %u KB/s, encryption=%s (%s), sealed=%u "
                    "unsealed=%u clear_out=%u clear_in=%u\n",
                    bigfile, (unsigned long long)total,
                    (unsigned long long)ms, reads, kbs,
                    (bc && smbcrypt_is_active(&bc->crypt)) ? "ON" : "OFF",
                    bc ? smbcrypt_algo_text(&bc->crypt) : "no connection",
                    nse, nun, nco, nci);
        } else {
            // Through selftest_notrun(), not a kprintf: a measurement that
            // declines to run has to reach /BOOTLOG.TXT and the end-of-boot
            // [SELFTEST] summary, because serial is absent on the laptop and
            // on the iMac, which is exactly where the evidence is needed. The
            // skipgate enforces this and caught the first version of this line.
            selftest_notrun("SMBTEST/bulkread",
                            "the bigfile named in /CONFIG/SMBTEST.CFG could not be opened");
        }
    }

    // 4) Read the SAME file through the VFS routing hook (fat_read_file on an
    //    /SMB path) to prove the VFS integration works end-to-end.
    kprintf("[SMBTEST] --- VFS read via fat_read_file(%s) ---\n", fpath);
    uint32_t vsz = 0;
    char *vbuf = (char *)fat_read_file(&g_fat_fs, fpath, &vsz);
    int vfs_ok = 0;
    if (vbuf) {
        kprintf("[SMBTEST] VFS read %u bytes:\n----8<----\n%s\n----8<----\n", vsz, vbuf);
        if (read_ok && vsz == (uint32_t)direct_len && memcmp(vbuf, fbuf, vsz) == 0) {
            vfs_ok = 1;
            kprintf("[SMBTEST] VFS read is BYTE-IDENTICAL to the direct smb_read "
                    "(%u bytes)\n", vsz);
        } else {
            kprintf("[SMBTEST] VFS read DIFFERS from the direct smb_read "
                    "(vfs=%u direct=%d) - FAIL\n", vsz, (int)direct_len);
        }
        kfree(vbuf);
    } else {
        kprintf("[SMBTEST] VFS read FAILED\n");
    }

    // 5) WRITE/UPLOAD test through the SYSCALL path (sys_open/sys_write/sys_close)
    //    to prove task #317 pass-2 FS wiring + smb_vfs_write_whole upload work.
    int write_ok = 0;
    {
        // #smbdisk: THIS SITE CALLED sys_open() WITH A KERNEL POINTER, so it
        // could never succeed and this whole self-test could only ever print
        // FAIL. sys_open() is the USER boundary: it runs sc_path_from_user() on
        // its argument, which correctly rejects a kernel address, so the SMB
        // write code below was never reached even once. #567 fixed exactly this
        // defect in netfs_fdpath_selftest() and ext2_dir_open_selftest() and
        // MISSED this one; measured on golden 2491, where the only line this
        // step produced was a bare "write-open FAILED" with no diagnosis.
        extern int64_t sys_open_k(const char *, int);
        extern int64_t sys_close(int);
        char wpath[SMB_MAX_PATH];
        snprintf(wpath, sizeof(wpath), "%s/P2WRITE.TXT", mp);
        const char *payload = "MayteraOS task #317 pass-2 SMB upload OK\n";
        uint32_t plen = (uint32_t)strlen(payload);
        kprintf("[SMBTEST] --- WRITE via syscall path %s ---\n", wpath);
        int64_t wfd = sys_open_k(wpath, 0x41);   // O_CREAT|O_WRONLY
        int open_ok = (wfd >= 0);
        if (open_ok) {
            kprintf("[SMBTEST] fd-layer write-open OK (fd=%d)\n", (int)wfd);
            sys_close((int)wfd);
        } else {
            kprintf("[SMBTEST] fd-layer write-open FAILED (%d)\n", (int)wfd);
        }
        // The PAYLOAD cannot go through sys_write() from here: sys_write copies
        // with copy_from_user(), which correctly refuses a kernel pointer, and
        // there is no sys_write_k() core (sys_open_k and sys_readdir_k exist;
        // the write side was never given one). Rather than leave the upload
        // untested, drive it through smb_vfs_write_whole(), which is the exact
        // function sys_close() calls to perform the upload. What is NOT covered
        // is therefore only the fd payload buffering, and that is said here
        // instead of being implied by a green line.
        int upload_ok = (smb_vfs_write_whole(wpath, payload, plen) == 0);
        kprintf("[SMBTEST] upload via smb_vfs_write_whole: %s\n",
                upload_ok ? "OK" : "FAILED");
        uint32_t rb = 0;
        char *back = (char *)fat_read_file(&g_fat_fs, wpath, &rb);
        if (back) {
            kprintf("[SMBTEST] readback %u bytes:\n----8<----\n%s----8<----\n", rb, back);
            if (rb == plen && memcmp(back, payload, plen) == 0) write_ok = 1;
            else kprintf("[SMBTEST] readback is NOT byte-identical to what was "
                         "written (%u vs %u) - FAIL\n", rb, plen);
            kfree(back);
        } else {
            kprintf("[SMBTEST] readback FAILED\n");
        }
        write_ok = write_ok && open_ok && upload_ok;
    }

    // 5b) #749: RECONNECT. Everything above runs on the FIRST connection, so
    //     none of it can tell you whether a re-established one works. Drop the
    //     transport the way a server's idle timeout does (Samba closes idle
    //     sessions on its own `deadtime`), then do an ordinary read.
    //
    //     The MessageId and the credit window are per CONNECTION (MS-SMB2
    //     2.2.1.2). Before #749 the connection struct kept its counter, so the
    //     first request on the NEW connection carried the old count; the server
    //     logged "bad message_id N (granted = 1, low = 0, range = 1)" and
    //     terminated the connection, and this client then printed
    //     "Authentication failed: 0xC000006D", sending whoever hit it to look
    //     at the password. `make SMBSEQNORESET=1` restores exactly that, so
    //     this step can be watched going RED.
    int reconnect_ok = 0;
    {
        kprintf("[SMBTEST] --- #749 RECONNECT: dropping the transport under "
                "%s ---\n", mp);
        smb_connection_t *live = smb_find_connection(mp);
        if (!live) {
            kprintf("[SMBTEST] no live connection to drop - FAIL\n");
        } else {
            // #749: the pass criterion is that THIS connection came back,
            // not that something somewhere could still read the file. Without
            // the epoch check this step passes on a FAILED reconnect, because
            // smb_vfs_ensure() then mounts a brand-new connection and the read
            // succeeds anyway. Measured: on `make SMBSEQNORESET=1` the fallback
            // mount even logged in as GUEST instead of the configured user,
            // so a dropped session silently changed WHO we were.
            uint64_t epoch_before = live->seq.epoch;
            kprintf("[SMBTEST] before drop: socket=%d next MessageId=%llu "
                    "epoch=%llu\n", live->tcp_socket,
                    (unsigned long long)live->seq.next_id,
                    (unsigned long long)epoch_before);
            // tcp_close() takes the socket out of ESTABLISHED on the
            // spot, which is all smb_conn_transport_ok() looks at, so
            // there is nothing to wait for here. No settle loop: a new
            // poll loop is exactly what the concurrency lint exists to
            // stop, and this one would have had nothing to poll for.
            tcp_close(live->tcp_socket);

            uint32_t rsz = 0;
            char *rbuf = (char *)fat_read_file(&g_fat_fs, fpath, &rsz);
            smb_connection_t *after = smb_find_connection(mp);
            int revived_in_place = (after == live &&
                                    after->seq.epoch == epoch_before + 1);
            if (rbuf) {
                if (!revived_in_place) {
                    kprintf("[SMBTEST] the read SUCCEEDED but this connection "
                            "was NOT re-established: it was replaced by a new "
                            "mount (epoch %llu, expected %llu). A reconnect "
                            "that silently becomes a remount loses the "
                            "session and the credentials - FAIL\n",
                            after ? (unsigned long long)after->seq.epoch : 0ULL,
                            (unsigned long long)(epoch_before + 1));
                } else if (read_ok && rsz == (uint32_t)direct_len &&
                           memcmp(rbuf, fbuf, rsz) == 0) {
                    reconnect_ok = 1;
                    kprintf("[SMBTEST] RECONNECT OK: this connection was "
                            "re-established in place (epoch %llu) and read %u "
                            "bytes byte-identical to the pre-drop read\n",
                            (unsigned long long)after->seq.epoch, rsz);
                } else {
                    kprintf("[SMBTEST] RECONNECT read came back WRONG "
                            "(%u bytes vs %d) - FAIL\n", rsz, (int)direct_len);
                }
                kfree(rbuf);
            } else {
                kprintf("[SMBTEST] RECONNECT read FAILED\n");
            }
            if (after) {
                kprintf("[SMBTEST] after reconnect: socket=%d next "
                        "MessageId=%llu sequence=%s\n", after->tcp_socket,
                        (unsigned long long)after->seq.next_id,
                        smbseq_fault_text(smbseq_fault(&after->seq)));
            }
        }
    }

    // 6) SHARE ENUMERATION (srvsvc over IPC$).
    {
        smb_vfs_set_default_creds("WORKGROUP", user, pass);
        kprintf("[SMBTEST] --- share enumeration on %s ---\n", ipstr);
        int sc = 0;
        char **shs = smb_list_shares(ip, &sc);
        if (shs) {
            for (int i = 0; i < sc; i++) kprintf("[SMBTEST]   share: %s\n", shs[i]);
            kprintf("[SMBTEST] enumerated %d shares\n", sc);
            smb_free_shares(shs, sc);
        } else {
            kprintf("[SMBTEST] share enumeration returned none (srvsvc)\n");
        }
    }

    if (read_ok && nent > 0 && vfs_ok && write_ok && reconnect_ok) {
        kprintf("========== SMB SELFTEST: PASS ==========\n\n");
    } else {
        kprintf("========== SMB SELFTEST: FAIL (read=%d entries=%d vfs=%d "
                "write=%d reconnect=%d) ==========\n\n",
                read_ok, nent, vfs_ok, write_ok, reconnect_ok);
    }
}

// Deferred self-test worker: runs a few seconds after boot, in a normal
// kernel process context (scheduler + net services up = the known-good net
// path used by the browser/HTTP). Mirrors output to serial.
static void smb_selftest_worker(void *arg) {
    (void)arg;
    extern void kprintf_set_dual_output(int on);
    proc_sleep(12000);           // let the desktop + net stack settle
    kprintf_set_dual_output(1);
    // task #317 pass 4: prove the SMB layer resolves HOSTNAMES via the kernel DNS
    // resolver (resolve_hostname -> dns_resolve), not just dotted-quad IPs.
    {
        uint32_t t = smb_resolve_ip("dns.google");
        if (t) kprintf("[DNSTEST] smb_resolve_ip(dns.google) = %u.%u.%u.%u (hostname OK)\n",
                       (t>>24)&0xFF, (t>>16)&0xFF, (t>>8)&0xFF, t&0xFF);
        else kprintf("[DNSTEST] smb_resolve_ip(dns.google) failed\n");
    }
    // task #317 pass 4: re-create persisted mounts from /CONFIG/NETMOUNTS.CFG
    // BEFORE the self-tests, so the fd-path test can read them without an
    // explicit mount (proving cross-reboot persistence).
    net_mounts_load();
    smb_run_selftest();
    // task #317 pass 3: NFS self-test (gated on /CONFIG/NFSTEST.CFG).
    { extern void nfs_run_selftest(void); nfs_run_selftest(); }
    // task #317 pass 4: terminal cat/ls path (sys_open/read/readdir) over the
    // persisted /SMB and /NFS mounts.
    { extern void netfs_fdpath_selftest(void); netfs_fdpath_selftest(); }
    kprintf_set_dual_output(0);
    // fall through: worker proc returns and is reaped
}

void smb_start_deferred_selftest(void) {
    extern int proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                              int priority, uint32_t stack_size);
    // PRIO_LOW = 1; 256 KB stack for the net call chain (#264 guidance).
    proc_create_ex("smbtest", smb_selftest_worker, 0, 1, 256 * 1024);
}
