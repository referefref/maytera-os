// rustkern/smbseq.rs - #749: the SMB2 message sequence and credit window, owned
// by the TCP CONNECTION it belongs to and by nothing else.
//
// NEW kernel code, so Rust per the 2026-07-16 rule. This is a protocol state
// machine and a policy ("may this request be sent on this connection at all"),
// which is exactly the category the rule names. There is no C twin to strangle
// and no -DRUST_* flag: before this module there was no state machine, only a
// bare `conn->message_id++` in one place and a `conn->credits = <whatever the
// last response said>` that nothing ever read.
//
// ===========================================================================
// WHAT WENT WRONG, measured not assumed
// ---------------------------------------------------------------------------
// MS-SMB2 2.2.1.2: MessageId is scoped to the SMB2 TRANSPORT CONNECTION. The
// server keeps a window [seq_low, seq_low + granted) and TERMINATES the
// connection when a request falls outside it. Samba says so in its own log,
// and these two lines are from the test server's log.smbd, written by
// source3/smbd/smb2_server.c:663 and :677 while our client was talking to it:
//
//   bad message_id 0 (sequence id 0) (granted = 1, low = 2, range = 1)
//   bad message_id 3 (sequence id 3) (granted = 1, low = 1, range = 1)
//
// The first is our client RESTARTING its counter at 0 on a connection where the
// server had already consumed 0 and 1. The second is our client SKIPPING to 3
// where the server expected 1. Both are the same defect: the counter lived in
// `smb_connection_t`, the connection lived in the TCP layer, and nothing tied
// the two together, so any re-initialisation of one without the other
// desynchronised them permanently.
//
// Measured on the wire on 2026-10-03 (golden 2564, commit 0fe362a4, tcpdump on
// the server): `granted` really is 1 for the first two messages of every
// connection. The server grants 1 credit for NEGOTIATE and 1 for the first
// SESSION_SETUP, and only then 31. So during the handshake the window is
// EXACTLY ONE MESSAGE WIDE and a single slipped id is immediately fatal. That
// is why this defect presented as an authentication failure: the server killed
// the connection over a sequencing fault, and the client read whatever came
// back as a verdict on the password.
//
// ===========================================================================
// THE FIX IS THE BINDING, NOT A RESET CALL
// ---------------------------------------------------------------------------
// "Remember to reset the counter on reconnect" is a rule someone has to keep.
// A sequence that is UNUSABLE until it is bound to a specific socket is a rule
// the compiler and the machine keep instead:
//
//   * `smbseq_bind(seq, socket)` is the ONLY thing that makes a sequence able
//     to issue an id, and it always starts at 0 with a 1-credit window. It is
//     called at the one place a connection becomes usable (the moment TCP
//     reaches ESTABLISHED), so "re-established" and "reset" are the same event
//     and cannot drift apart.
//   * `smbseq_next` refuses if the sequence was never bound, or is bound to a
//     DIFFERENT socket than the one the caller is about to send on. A struct
//     that was zeroed under a live connection, or carried onto a new one,
//     cannot send a single byte.
//   * `smbseq_check_reply` refuses a response whose MessageId is not the one
//     we sent. This is the half that stops a misdiagnosis: a status code in a
//     packet that is not the answer to our request is not evidence about our
//     request.
//
// Every refusal records a FAULT on the sequence. A faulted sequence stays
// faulted until it is re-bound, so the connection is known-bad rather than
// quietly offset by one.

/// The SMB2 message sequence and credit window for ONE transport connection.
/// Mirrored field for field by `smb_seq_t` in net/smb.h and locked by a
/// `_Static_assert` on the size there.
///
/// A zeroed SmbSeq is the correct "never bound" state: `epoch == 0` means
/// unusable, which is what makes a `memset` of the owning struct fail closed.
/// That is deliberate, because a `memset` of the owning struct under a live
/// connection is precisely what happened (net/smb.c's `smb_init` zeroed the
/// whole connection table from three runtime mount paths).
#[repr(C)]
pub struct SmbSeq {
    /// MONOTONIC connection generation. Bumped by every bind and never reset,
    /// so it also answers the question a caller cannot otherwise answer: was
    /// this connection RE-ESTABLISHED in place, or silently REPLACED by a new
    /// one? Those are not the same thing, and the difference is not cosmetic:
    /// a replacement loses the session, and in net/smb.c's fallback path it
    /// also loses the credentials and logs in as guest. 0 means never bound.
    pub epoch: u64,
    /// The MessageId the next request on this connection will carry.
    pub next_id: u64,
    /// MessageId of the most recent request actually issued.
    pub last_sent_id: u64,
    /// The TCP socket descriptor this sequence belongs to, or -1 when it is
    /// not bound to one. An id may only be issued for THIS socket. This, not
    /// `epoch`, is what says "usable", because `epoch` has to stay monotonic.
    ///
    /// NOTE that a ZEROED struct has 0 here, which is a perfectly valid socket
    /// descriptor. `epoch == 0` is what makes a zeroed struct fail closed, so
    /// that check must come first and must stay.
    pub bound_socket: i32,
    /// Credits the server last told us we hold. Never less than 1.
    pub granted: u16,
    /// Requests issued and not yet answered. Must stay below `granted`.
    pub outstanding: u16,
    /// SMBSEQ_FAULT_*. Non-zero means this connection must not be used again
    /// until it is re-bound.
    pub fault: u16,
    pub _pad: u16,
}

// If a field is added or reordered on either side, this stops the build rather
// than silently reading the wrong offsets.
const _: () = assert!(core::mem::size_of::<SmbSeq>() == 40);
const _: () = assert!(core::mem::align_of::<SmbSeq>() == 8);

pub const SMBSEQ_FAULT_NONE: u16 = 0;
/// An id was asked for on a sequence that was never bound to a connection.
pub const SMBSEQ_FAULT_UNBOUND: u16 = 1;
/// The sequence is bound to a different socket than the caller is sending on.
pub const SMBSEQ_FAULT_SOCKET: u16 = 2;
/// Issuing the id would exceed the credits the server granted.
pub const SMBSEQ_FAULT_WINDOW: u16 = 3;
/// A response arrived whose MessageId is not the one we sent.
pub const SMBSEQ_FAULT_REPLY_ID: u16 = 4;
/// A response failed its MESSAGE SIGNATURE check (rustkern/smbsign.rs).
///
/// This has its own code rather than reusing REPLY_ID, which is what the
/// signing work first did, because the reason text is the whole point of this
/// enum: condemning the connection as "a response arrived for a different
/// MessageId (message sequence desynchronised)" after a signature failure
/// sends the next person to look at the sequencing, which is precisely the
/// misdiagnosis #749 exists to have stopped happening. A tampered or unsigned
/// response is not a desync.
pub const SMBSEQ_FAULT_SIGNATURE: u16 = 5;
/// A response FAILED ITS DECRYPTION, or arrived unencrypted on a session that
/// is encrypted (rustkern/smbcrypt.rs).
///
/// Its own code for the same reason SIGNATURE got one: the lesson recorded in
/// blame.md is that a sentence which classifies an enum value is correct only
/// until the enum grows, so each fault carries its own text. A frame whose
/// AEAD tag does not match is not a desync and not a signature failure.
pub const SMBSEQ_FAULT_ENCRYPTION: u16 = 6;

/// MS-SMB2 2.2.1.2: a server-initiated message (an oplock break notification)
/// carries this MessageId. It is not a reply to anything, so it must not be
/// mistaken for a desynchronised one.
const SMBSEQ_UNSOLICITED_ID: u64 = 0xFFFF_FFFF_FFFF_FFFF;

/// SMB2_FLAGS_ASYNC_COMMAND, from net/smb.h.
const SMB2_FLAGS_ASYNC_COMMAND: u32 = 0x0000_0002;
/// STATUS_PENDING, from net/smb.h.
const SMB2_STATUS_PENDING: u32 = 0x0000_0103;

/// Make a sequence usable for `socket`, starting at MessageId 0 with the
/// one-credit window every SMB2 server opens a connection with.
///
/// THIS IS THE RESET. It is called at the moment a TCP connection reaches
/// ESTABLISHED, which is the only moment "the connection was (re-)established"
/// is true, so there is no separate reset to forget.
///
/// # Safety
/// `s` must be a valid, writable `SmbSeq` or null.
#[no_mangle]
pub extern "C" fn smbseq_bind(s: *mut SmbSeq, socket: i32) -> i32 {
    if s.is_null() || socket < 0 {
        return -1;
    }
    // SAFETY: non-null, and the C side owns a correctly-sized SmbSeq (size
    // locked by _Static_assert in net/smb.h).
    let q = unsafe { &mut *s };
    q.epoch = q.epoch.wrapping_add(1);
    if q.epoch == 0 {
        // Wrap must never land on the "never bound" value.
        q.epoch = 1;
    }
    q.next_id = 0;
    q.last_sent_id = 0;
    q.bound_socket = socket;
    q.granted = 1;
    q.outstanding = 0;
    q.fault = SMBSEQ_FAULT_NONE;
    0
}

/// Make a sequence unusable again. Called when the socket is closed, so a
/// sequence can never outlive the connection it describes.
///
/// # Safety
/// `s` must be a valid, writable `SmbSeq` or null.
#[no_mangle]
pub extern "C" fn smbseq_unbind(s: *mut SmbSeq) {
    if s.is_null() {
        return;
    }
    // SAFETY: as smbseq_bind.
    let q = unsafe { &mut *s };
    // `epoch` is DELIBERATELY left alone: it is the monotonic generation count
    // and resetting it here would destroy the one piece of evidence that
    // distinguishes a re-established connection from a replaced one.
    q.bound_socket = -1;
    q.next_id = 0;
    q.last_sent_id = 0;
    q.granted = 0;
    q.outstanding = 0;
    q.fault = SMBSEQ_FAULT_NONE;
}

/// The MessageId for the next request on `socket`, or a negative value equal to
/// `-(fault code)` if this request must not be sent at all.
///
/// # Safety
/// `s` must be a valid, writable `SmbSeq` or null.
#[no_mangle]
pub extern "C" fn smbseq_next(s: *mut SmbSeq, socket: i32) -> i64 {
    if s.is_null() {
        return -(SMBSEQ_FAULT_UNBOUND as i64);
    }
    // SAFETY: as smbseq_bind.
    let q = unsafe { &mut *s };
    if q.fault != SMBSEQ_FAULT_NONE {
        return -(q.fault as i64);
    }
    if q.epoch == 0 || q.bound_socket < 0 {
        q.fault = SMBSEQ_FAULT_UNBOUND;
        return -(SMBSEQ_FAULT_UNBOUND as i64);
    }
    if q.bound_socket != socket {
        q.fault = SMBSEQ_FAULT_SOCKET;
        return -(SMBSEQ_FAULT_SOCKET as i64);
    }
    let granted = if q.granted == 0 { 1 } else { q.granted };
    if q.outstanding >= granted {
        q.fault = SMBSEQ_FAULT_WINDOW;
        return -(SMBSEQ_FAULT_WINDOW as i64);
    }
    let id = q.next_id;
    q.next_id = q.next_id.wrapping_add(1);
    q.last_sent_id = id;
    q.outstanding += 1;
    id as i64
}

/// Record the credits the server granted in a response.
///
/// A server that grants 0 is telling us to stop sending; we floor it at 1 so a
/// mount degrades to strictly-synchronous instead of wedging with no way to
/// ever ask again. Our client is request/reply anyway, so the floor changes
/// nothing it actually does, and the honest alternative (refuse forever) would
/// turn a server hiccup into a dead mount.
///
/// # Safety
/// `s` must be a valid, writable `SmbSeq` or null.
#[no_mangle]
pub extern "C" fn smbseq_note_grant(s: *mut SmbSeq, granted: u16) {
    if s.is_null() {
        return;
    }
    // SAFETY: as smbseq_bind.
    let q = unsafe { &mut *s };
    if q.epoch == 0 || q.bound_socket < 0 {
        return;
    }
    q.granted = if granted == 0 { 1 } else { granted };
}

/// Does this response answer the request we issued, and does it COMPLETE it?
///
/// Returns:
///   *  0 - the final response to our request. The request is now complete.
///   *  1 - an unsolicited server message. Not a reply to anything; consumes
///          nothing.
///   *  2 - an INTERIM response. It matches our request but does NOT complete
///          it, so the caller must keep receiving. MS-SMB2 3.3.4.2: a server
///          that cannot answer inside its own deadline sends STATUS_PENDING
///          with SMB2_FLAGS_ASYNC_COMMAND and the SAME MessageId, then the
///          real answer later.
///   *  negative - `-(fault code)`; the MessageId does not match and the
///          sequence is now faulted.
///
/// THE INTERIM RULE IS NOT DECORATION, it was measured. The first build of this
/// module counted an interim response as the answer, and that broke SMB share
/// enumeration on the very next boot: a READ on the `\srvsvc` named pipe gets
/// STATUS_PENDING first and the data second, both on MessageId 6, so the real
/// response arrived with nothing outstanding and was reported as a desync.
/// net/smb.c had ALREADY been taught to skip pending frames, inside smb2_read()
/// only, by #317 pass 2. That is the shape of defect this whole module exists
/// to end: one fd/command family knowing a protocol rule that the others do
/// not. The rule lives here now, so it holds for every SMB2 command instead of
/// just the one whose author hit it.
///
/// The return value of 1 is also the reason a legitimate oplock-break
/// notification cannot be misread as a desync. KNOWN LIMIT, stated rather than
/// implied: net/smb.c delivers a 1 upward rather than re-receiving, which is
/// the pre-existing behaviour; we request SMB2_OPLOCK_LEVEL_NONE, so a
/// conforming server has nothing to break.
///
/// # Safety
/// `s` must be a valid, writable `SmbSeq` or null.
#[no_mangle]
pub extern "C" fn smbseq_check_reply(s: *mut SmbSeq, reply_id: u64,
                                     resp_flags: u32, resp_status: u32) -> i32 {
    if s.is_null() {
        return -(SMBSEQ_FAULT_UNBOUND as i32);
    }
    if reply_id == SMBSEQ_UNSOLICITED_ID {
        return 1;
    }
    // SAFETY: as smbseq_bind.
    let q = unsafe { &mut *s };
    if q.epoch == 0 || q.bound_socket < 0 {
        q.fault = SMBSEQ_FAULT_UNBOUND;
        return -(SMBSEQ_FAULT_UNBOUND as i32);
    }
    if q.outstanding == 0 || reply_id != q.last_sent_id {
        q.fault = SMBSEQ_FAULT_REPLY_ID;
        return -(SMBSEQ_FAULT_REPLY_ID as i32);
    }
    let interim = (resp_flags & SMB2_FLAGS_ASYNC_COMMAND) != 0
        && resp_status == SMB2_STATUS_PENDING;
    if interim {
        return 2;
    }
    q.outstanding -= 1;
    0
}

/// Mark this connection unusable, with a reason, from outside.
///
/// The C side needs this for the case the sequencer cannot see: a request was
/// sent and NO reply ever came (a receive timeout). The reply may still arrive
/// later, after which every response on the connection is one request behind
/// forever, so the connection has to be condemned even though nothing
/// mismatched yet. An already-faulted sequence keeps its FIRST reason, which is
/// the one worth reporting.
///
/// This exists so no caller writes `seq.fault` by hand. The struct is laid out
/// for C to read, but the rules about it live here and only here.
///
/// # Safety
/// `s` must be a valid, writable `SmbSeq` or null.
#[no_mangle]
pub extern "C" fn smbseq_condemn(s: *mut SmbSeq, code: u16) {
    if s.is_null() || code == SMBSEQ_FAULT_NONE {
        return;
    }
    // SAFETY: as smbseq_bind.
    let q = unsafe { &mut *s };
    if q.fault == SMBSEQ_FAULT_NONE {
        q.fault = code;
    }
}

/// The fault code, or 0 when the sequence is healthy.
///
/// # Safety
/// `s` must be a valid, readable `SmbSeq` or null.
#[no_mangle]
pub extern "C" fn smbseq_fault(s: *const SmbSeq) -> u16 {
    if s.is_null() {
        return SMBSEQ_FAULT_UNBOUND;
    }
    // SAFETY: read-only access to a correctly-sized SmbSeq.
    unsafe { (*s).fault }
}

/// 1 when a request may be issued on this sequence right now.
///
/// # Safety
/// `s` must be a valid, readable `SmbSeq` or null.
#[no_mangle]
pub extern "C" fn smbseq_is_healthy(s: *const SmbSeq) -> i32 {
    if s.is_null() {
        return 0;
    }
    // SAFETY: read-only access to a correctly-sized SmbSeq.
    let q = unsafe { &*s };
    if q.epoch != 0 && q.bound_socket >= 0 && q.fault == SMBSEQ_FAULT_NONE {
        1
    } else {
        0
    }
}

/// The MessageId the last issued request carried. For diagnostics only.
///
/// # Safety
/// `s` must be a valid, readable `SmbSeq` or null.
#[no_mangle]
pub extern "C" fn smbseq_last_sent(s: *const SmbSeq) -> u64 {
    if s.is_null() {
        return 0;
    }
    // SAFETY: read-only access to a correctly-sized SmbSeq.
    unsafe { (*s).last_sent_id }
}

/// A short NUL-terminated description of a fault code, for the message the user
/// actually sees. The whole point of #749's second half is that the user is not
/// told "authentication failed" about a sequencing fault, so these strings say
/// what the fault IS and stay free of the word "authentication".
#[no_mangle]
pub extern "C" fn smbseq_fault_text(code: u16) -> *const u8 {
    match code {
        SMBSEQ_FAULT_NONE => b"no fault\0".as_ptr(),
        SMBSEQ_FAULT_UNBOUND => {
            b"not bound to a connection (the connection state was reset underneath it)\0".as_ptr()
        }
        SMBSEQ_FAULT_SOCKET => {
            b"bound to a different socket than this request (connection state reused across connections)\0".as_ptr()
        }
        SMBSEQ_FAULT_WINDOW => {
            b"outside the credit window the server granted\0".as_ptr()
        }
        SMBSEQ_FAULT_REPLY_ID => {
            b"a response arrived for a different MessageId (message sequence desynchronised)\0".as_ptr()
        }
        SMBSEQ_FAULT_SIGNATURE => {
            b"a response FAILED ITS MESSAGE SIGNATURE (altered in flight, or not from a server holding this session key)\0".as_ptr()
        }
        SMBSEQ_FAULT_ENCRYPTION => {
            b"a response FAILED ITS DECRYPTION or arrived UNENCRYPTED on an encrypted session (altered in flight, replayed, or not from a server holding this session key)\0".as_ptr()
        }
        _ => b"unknown fault\0".as_ptr(),
    }
}

/// ABSOLUTE assertions over the sequencer, not a differential: there is no C
/// twin, and the thing being removed is the ABSENCE of a rule, so an
/// agreement test between two implementations of nothing would pass.
///
/// Returns 0 on PASS, otherwise the number of the first failing case, so a
/// failure on serial names the exact rule that broke.
///
/// Case 4 IS the ticket. Case 6 is the Samba window rule that made the ticket
/// fatal rather than merely wrong. Case 8 is the misdiagnosis.
#[no_mangle]
pub extern "C" fn smbseq_selftest_rs() -> u32 {
    let mut q = SmbSeq {
        epoch: 0,
        next_id: 0,
        last_sent_id: 0,
        bound_socket: 0,
        granted: 0,
        outstanding: 0,
        fault: 0,
        _pad: 0,
    };

    // ---- 1. A zeroed sequence cannot issue an id. This is what makes a
    // memset of the owning connection struct fail CLOSED instead of silently
    // restarting at 0 on a live connection (the `bad message_id 0 ... low = 2`
    // line in the server log).
    if smbseq_next(&mut q, 7) != -(SMBSEQ_FAULT_UNBOUND as i64) {
        return 1;
    }
    if smbseq_is_healthy(&q) != 0 {
        return 1;
    }

    // ---- 2. A bound sequence starts at 0, which is where every SMB2 server
    // opens its window.
    if smbseq_bind(&mut q, 7) != 0 {
        return 2;
    }
    if smbseq_is_healthy(&q) != 1 {
        return 2;
    }
    if smbseq_next(&mut q, 7) != 0 {
        return 2;
    }

    // ---- 3. Request/reply advances by exactly one, and the server's larger
    // grant is honoured.
    if smbseq_check_reply(&mut q, 0, 0, 0) != 0 {
        return 3;
    }
    smbseq_note_grant(&mut q, 31);
    if smbseq_next(&mut q, 7) != 1 {
        return 3;
    }
    if smbseq_check_reply(&mut q, 1, 0, 0) != 0 {
        return 3;
    }
    if smbseq_next(&mut q, 7) != 2 {
        return 3;
    }
    if smbseq_check_reply(&mut q, 2, 0, 0) != 0 {
        return 3;
    }

    // ---- 4. THE TICKET. Drive the sequence up to 136, the id the server
    // actually rejected, then RE-ESTABLISH the connection on a new socket. The
    // next id must be 0, not 136.
    for want in 3..136i64 {
        if smbseq_next(&mut q, 7) != want {
            return 4;
        }
        if smbseq_check_reply(&mut q, want as u64, 0, 0) != 0 {
            return 4;
        }
    }
    if q.next_id != 136 {
        return 4;
    }
    let epoch_before_rebind = q.epoch;
    if smbseq_bind(&mut q, 9) != 0 {
        return 4;
    }
    // The generation must ADVANCE, not restart: this is how a caller tells a
    // connection that came back from one that was quietly replaced.
    if q.epoch != epoch_before_rebind + 1 {
        return 4;
    }
    if smbseq_next(&mut q, 9) != 0 {
        return 4;
    }
    if smbseq_check_reply(&mut q, 0, 0, 0) != 0 {
        return 4;
    }

    // ---- 5. A sequence bound to one socket refuses to number a request for
    // another. This is the cross-connection reuse case, which no amount of
    // "remember to reset" discipline would catch.
    if smbseq_next(&mut q, 7) != -(SMBSEQ_FAULT_SOCKET as i64) {
        return 5;
    }
    if smbseq_fault(&q) != SMBSEQ_FAULT_SOCKET {
        return 5;
    }
    // Faulted stays faulted: the connection is known-bad, not offset by one.
    if smbseq_next(&mut q, 9) != -(SMBSEQ_FAULT_SOCKET as i64) {
        return 5;
    }

    // ---- 6. The credit window. Measured on the wire: the server grants ONE
    // credit for the first two messages of every connection, so a second
    // request before the first is answered is exactly the
    // `granted = 1, low = N, range = 1` rejection.
    if smbseq_bind(&mut q, 11) != 0 {
        return 6;
    }
    if smbseq_next(&mut q, 11) != 0 {
        return 6;
    }
    if smbseq_next(&mut q, 11) != -(SMBSEQ_FAULT_WINDOW as i64) {
        return 6;
    }

    // ---- 7. With a 31-credit grant, 31 requests may be outstanding and the
    // 32nd may not.
    if smbseq_bind(&mut q, 12) != 0 {
        return 7;
    }
    smbseq_note_grant(&mut q, 31);
    for i in 0..31i64 {
        if smbseq_next(&mut q, 12) != i {
            return 7;
        }
    }
    if smbseq_next(&mut q, 12) != -(SMBSEQ_FAULT_WINDOW as i64) {
        return 7;
    }

    // ---- 8. THE MISDIAGNOSIS. A response carrying someone else's MessageId
    // is reported as a mismatch, so the status inside it is never read as a
    // verdict on our request. Before this, a connection the server had killed
    // over a sequencing fault produced "Authentication failed: 0xC000006D".
    if smbseq_bind(&mut q, 13) != 0 {
        return 8;
    }
    if smbseq_next(&mut q, 13) != 0 {
        return 8;
    }
    if smbseq_check_reply(&mut q, 7, 0, 0) != -(SMBSEQ_FAULT_REPLY_ID as i32) {
        return 8;
    }
    if smbseq_fault(&q) != SMBSEQ_FAULT_REPLY_ID {
        return 8;
    }

    // ---- 9. An unsolicited server notification is NOT a desync, and does not
    // consume the request we are still waiting for.
    if smbseq_bind(&mut q, 14) != 0 {
        return 9;
    }
    if smbseq_next(&mut q, 14) != 0 {
        return 9;
    }
    if smbseq_check_reply(&mut q, SMBSEQ_UNSOLICITED_ID, 0, 0) != 1 {
        return 9;
    }
    if smbseq_fault(&q) != SMBSEQ_FAULT_NONE {
        return 9;
    }
    if q.outstanding != 1 {
        return 9;
    }
    if smbseq_check_reply(&mut q, 0, 0, 0) != 0 {
        return 9;
    }

    // ---- 10. unbind makes it unusable again, so a sequence cannot outlive
    // the socket it describes.
    let epoch_before_unbind = q.epoch;
    smbseq_unbind(&mut q);
    if smbseq_is_healthy(&q) != 0 {
        return 10;
    }
    if smbseq_next(&mut q, 14) != -(SMBSEQ_FAULT_UNBOUND as i64) {
        return 10;
    }
    // ... and the generation survives, because net/smb.c unbinds BEFORE it
    // reconnects, and an epoch reset there would make a successful reconnect
    // indistinguishable from a fresh mount.
    if q.epoch != epoch_before_unbind {
        return 10;
    }

    // ---- 11. THE INTERIM RULE, and it is here because leaving it out broke
    // share enumeration on the first boot of this change. An interim async
    // response matches but does NOT complete the request, so the real answer
    // that follows on the SAME MessageId must still be accepted. Case 11 is
    // the regression test for that, with the exact MessageId (6) and command
    // (READ on \srvsvc) the live failure used.
    if smbseq_bind(&mut q, 15) != 0 {
        return 11;
    }
    smbseq_note_grant(&mut q, 31);
    for i in 0..6i64 {
        if smbseq_next(&mut q, 15) != i {
            return 11;
        }
        if smbseq_check_reply(&mut q, i as u64, 0, 0) != 0 {
            return 11;
        }
    }
    if smbseq_next(&mut q, 15) != 6 {
        return 11;
    }
    // interim: STATUS_PENDING with the ASYNC flag, same MessageId
    if smbseq_check_reply(&mut q, 6, SMB2_FLAGS_ASYNC_COMMAND, SMB2_STATUS_PENDING) != 2 {
        return 11;
    }
    if q.outstanding != 1 {
        return 11;   // an interim must not complete the request
    }
    // the real answer, same MessageId
    if smbseq_check_reply(&mut q, 6, 0, 0) != 0 {
        return 11;
    }
    if q.outstanding != 0 || smbseq_fault(&q) != SMBSEQ_FAULT_NONE {
        return 11;
    }
    // STATUS_PENDING WITHOUT the async flag is not an interim response; it is
    // a final one that happens to carry that status, and treating it as
    // interim would hang the caller waiting for an answer already given.
    if smbseq_next(&mut q, 15) != 7 {
        return 11;
    }
    if smbseq_check_reply(&mut q, 7, 0, SMB2_STATUS_PENDING) != 0 {
        return 11;
    }

    0
}
