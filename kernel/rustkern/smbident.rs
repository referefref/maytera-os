// rustkern/smbident.rs - #749 pass 2: WHO did the SMB session actually end up
// as, and is that who the caller asked for?
//
// NEW kernel code, so Rust per the 2026-07-16 rule. This is pure policy over
// two names and one 16-bit field: no floats, no allocation, no hardware, no
// performance argument for C. There is no C twin to strangle and no -DRUST_*
// flag, because before this module there was no such check anywhere in the
// kernel.
//
// ===========================================================================
// WHAT WENT WRONG, measured not assumed
// ---------------------------------------------------------------------------
// net/smb.c read SMB2_SESSION_FLAG_IS_GUEST out of the SESSION_SETUP response
// in exactly ONE place and did exactly ONE thing with it:
//
//     if (conn->session_flags & SMB2_SESSION_FLAG_IS_GUEST) {
//         kprintf("[SMB] Logged in as guest\n");
//     } else {
//         kprintf("[SMB] Authenticated as %s\\%s\n", ...);
//     }
//     return 0;                      // <-- SUCCESS, either way
//
// SMB2_SESSION_FLAG_IS_NULL (an anonymous session) was never read at all, in
// any file. So a server could answer our NTLM authentication for a named user
// with STATUS_SUCCESS plus "and by the way you are the guest account", and the
// mount came up, reported itself working, and every read and write on it ran
// as the server's guest account instead of as the user. The only trace was one
// line on the serial console, which on a desktop machine nobody is reading.
//
// This is not hypothetical and it is not a crafted packet. Samba ships the
// switch: `map to guest = Bad Password` makes smbd answer ANY wrong password
// for a VALID user with a successful guest session. Measured on 2026-10-04
// against Samba 4.17.12 with that setting, both with smbclient and with this
// kernel's own client.
//
// ===========================================================================
// THE RULE
// ---------------------------------------------------------------------------
// If the caller asked for a NAMED identity, the session must not come back
// anonymous. That is the whole policy, and it lives here so that every mount
// path shares one copy of it: the first mount, a reconnect, the on-demand
// /SMB/<server>/<share> mount and the share enumerator all reach the same
// SESSION_SETUP, and the check sits there rather than in any one of them.
//
// A caller that asked for an anonymous identity in the first place is NOT
// downgraded by being given one, so `/SMB/host/share` typed with no saved
// credentials still works exactly as it did.
//
// NOTHING IS MIRRORED INTO C. `smbident_check` returns 0 for allow and a
// non-zero reason code for refuse, and the C side prints
// `smbident_verdict_text()` of whatever it got. There are deliberately no
// SMB_IDENT_* constants in net/smb.h, because a mirrored constant is a thing
// that can drift and a `_Static_assert` cannot catch a wrong VALUE.

/// Longest name we will look at. `smb_connection_t::username` and `::domain`
/// are both `char[64]` in net/smb.h, so a NUL-terminated name cannot be longer
/// than 63 bytes; the scan stops at 64 either way rather than trusting it.
const NAME_MAX: usize = 64;

/// SMB2 SESSION_SETUP **response** flags, from net/smb.h.
///
/// FOOTGUN, and it is in the header: `SMB2_SESSION_FLAG_BINDING` is also
/// `0x0001`. That one is a REQUEST flag and this one is a RESPONSE flag, so
/// the same bit means two unrelated things depending on direction. We are
/// reading a response here.
const SMB2_SESSION_FLAG_IS_GUEST: u16 = 0x0001;
/// An anonymous (null) session. Windows sets this where Samba sets IS_GUEST.
/// Before this module, no code in the kernel read this bit at all.
const SMB2_SESSION_FLAG_IS_NULL: u16 = 0x0002;

/// Allowed: the session identity is the one that was asked for.
const VERDICT_OK: u16 = 0;
/// Refused: a named identity was asked for and the server granted a GUEST one.
const VERDICT_GUEST: u16 = 1;
/// Refused: a named identity was asked for and the server granted an ANONYMOUS
/// (null) one.
const VERDICT_NULL: u16 = 2;
/// Refused: the server granted a session that is BOTH guest and anonymous.
const VERDICT_BOTH: u16 = 3;

/// Read a NUL-terminated C name into a fixed buffer, ASCII-lowercased, with the
/// length. Anything past `NAME_MAX` bytes is not read.
///
/// # Safety
/// `p` must be null or point to at least one readable byte and be
/// NUL-terminated within `NAME_MAX` bytes, which is what a `char[64]` field in
/// `smb_connection_t` guarantees.
unsafe fn read_lc(p: *const u8) -> ([u8; NAME_MAX], usize) {
    let mut out = [0u8; NAME_MAX];
    let mut n = 0usize;
    if p.is_null() {
        return (out, 0);
    }
    while n < NAME_MAX {
        let c = *p.add(n);
        if c == 0 {
            break;
        }
        out[n] = if c.is_ascii_uppercase() { c + 32 } else { c };
        n += 1;
    }
    (out, n)
}

/// Is this name a request for an anonymous session rather than for a person?
///
/// The empty string is the honest case: net/smb.c's on-demand mount default is
/// literally `"guest"` with an empty NT hash, and `parse_smb_url` defaults to
/// `"guest"` too, so both must count as anonymous or every credential-free
/// /SMB path would start failing.
///
/// `anonymous` and `nobody` are included because they are the other two names
/// a server-side anonymous account is conventionally called, and treating one
/// of them as a person would produce a refusal where the user asked for
/// exactly what they got.
fn name_is_anon(lc: &[u8]) -> bool {
    matches!(lc, b"" | b"guest" | b"anonymous" | b"nobody")
}

/// 1 if `user` names an anonymous session rather than a person.
///
/// # Safety
/// As `read_lc`.
#[no_mangle]
pub extern "C" fn smbident_name_is_anonymous(user: *const u8) -> i32 {
    // SAFETY: caller contract documented above.
    let (lc, n) = unsafe { read_lc(user) };
    if name_is_anon(&lc[..n]) { 1 } else { 0 }
}

/// THE CHECK. Compare the identity the SESSION_SETUP response actually granted
/// against the identity the caller asked for.
///
/// Returns 0 to allow the session, or a non-zero reason code to REFUSE it. The
/// caller passes the code straight to `smbident_verdict_text()`; it is not
/// expected to know what any particular value means.
///
/// # Safety
/// As `read_lc`.
#[no_mangle]
pub extern "C" fn smbident_check(user: *const u8, session_flags: u16) -> u16 {
    // SAFETY: caller contract documented above.
    let (lc, n) = unsafe { read_lc(user) };
    let is_guest = (session_flags & SMB2_SESSION_FLAG_IS_GUEST) != 0;
    let is_null = (session_flags & SMB2_SESSION_FLAG_IS_NULL) != 0;

    if name_is_anon(&lc[..n]) {
        // An anonymous request answered with an anonymous session is exactly
        // what was asked for. An anonymous request answered with a NAMED
        // session is a server doing something odd in our favour, and refusing
        // it would break nothing-we-asked-for; it is not a downgrade, so it is
        // allowed and left to the share permissions.
        return VERDICT_OK;
    }

    match (is_guest, is_null) {
        (false, false) => VERDICT_OK,
        (true, false) => VERDICT_GUEST,
        (false, true) => VERDICT_NULL,
        (true, true) => VERDICT_BOTH,
    }
}

/// Are these two `domain\user` pairs the same SMB identity?
///
/// SMB user and domain names are case-insensitive, so the comparison is. All
/// of the anonymous spellings collapse to one identity, so a mount recorded
/// with `""` and a retry asking for `"guest"` are the SAME request and must
/// not be reported as a conflict.
///
/// The DOMAIN is only compared when both sides name a person: two anonymous
/// sessions are the same non-identity whatever workgroup string was attached.
///
/// # Safety
/// As `read_lc`, for all four pointers.
#[no_mangle]
pub extern "C" fn smbident_same(dom_a: *const u8, user_a: *const u8,
                                dom_b: *const u8, user_b: *const u8) -> i32 {
    // SAFETY: caller contract documented above.
    let (ua, na) = unsafe { read_lc(user_a) };
    // SAFETY: as above.
    let (ub, nb) = unsafe { read_lc(user_b) };
    let anon_a = name_is_anon(&ua[..na]);
    let anon_b = name_is_anon(&ub[..nb]);
    if anon_a || anon_b {
        return if anon_a && anon_b { 1 } else { 0 };
    }
    if ua[..na] != ub[..nb] {
        return 0;
    }
    // SAFETY: as above.
    let (da, nda) = unsafe { read_lc(dom_a) };
    // SAFETY: as above.
    let (db, ndb) = unsafe { read_lc(dom_b) };
    if da[..nda] == db[..ndb] { 1 } else { 0 }
}

/// What to put in front of the user, in words that name the REAL cause.
///
/// #749's first half existed because "Authentication failed: 0xC000006D" was
/// printed for a protocol sequencing fault and sent an engineer to look at the
/// password. This is the mirror-image hazard: here the server said SUCCESS, so
/// any message about a failed password or a lost connection would be just as
/// wrong in the other direction. These strings say that the server ACCEPTED
/// the request and SUBSTITUTED the identity, because that is what happened.
#[no_mangle]
pub extern "C" fn smbident_verdict_text(code: u16) -> *const u8 {
    match code {
        VERDICT_OK => b"the session identity is the one that was requested\0".as_ptr(),
        VERDICT_GUEST => {
            b"the server accepted the request but granted a GUEST session instead of the requested account (SMB2_SESSION_FLAG_IS_GUEST). It did not fail: it substituted an anonymous identity, so reads and writes here would NOT be yours\0".as_ptr()
        }
        VERDICT_NULL => {
            b"the server accepted the request but granted an ANONYMOUS (null) session instead of the requested account (SMB2_SESSION_FLAG_IS_NULL). It did not fail: it substituted an identity with no user behind it\0".as_ptr()
        }
        VERDICT_BOTH => {
            b"the server accepted the request but granted a session flagged BOTH guest and anonymous instead of the requested account. It did not fail: it substituted an identity that is not the requested account\0".as_ptr()
        }
        _ => b"unknown identity verdict\0".as_ptr(),
    }
}

/// ABSOLUTE assertions over the policy, not a differential: there is no C twin,
/// and what is being removed is the ABSENCE of a check, so a two-arm agreement
/// test would pass while proving nothing.
///
/// Returns 0 on PASS, otherwise the number of the first failing case, so a
/// failure on serial names the exact rule that broke.
///
/// Case 3 IS the ticket: a named user answered with a guest session. Case 7 is
/// the half that was never read by any code in the kernel. Case 9 is the one
/// that stops this check becoming a false alarm on an ordinary credential-free
/// mount.
#[no_mangle]
pub extern "C" fn smbident_selftest_rs() -> u32 {
    let wg = b"WORKGROUP\0".as_ptr();

    // ---- 1. A named user with a clean session is allowed.
    if smbident_check(b"james\0".as_ptr(), 0) != VERDICT_OK {
        return 1;
    }

    // ---- 2. Case does not matter: SMB names are case-insensitive, so an
    // uppercase name must take the same path as a lowercase one.
    if smbident_check(b"JAMES\0".as_ptr(), SMB2_SESSION_FLAG_IS_GUEST) != VERDICT_GUEST {
        return 2;
    }

    // ---- 3. THE TICKET. A named user, a successful SESSION_SETUP, and the
    // guest flag set. Before this module net/smb.c printed one line and
    // returned 0 here, and the mount came up as the server's guest account.
    if smbident_check(b"james\0".as_ptr(), SMB2_SESSION_FLAG_IS_GUEST) != VERDICT_GUEST {
        return 3;
    }

    // ---- 4. Other flags present alongside it must not mask it. ENCRYPT_DATA
    // (0x0004) is routinely set by a Samba configured for encryption.
    if smbident_check(b"james\0".as_ptr(), SMB2_SESSION_FLAG_IS_GUEST | 0x0004) != VERDICT_GUEST {
        return 4;
    }

    // ---- 5. An unrelated flag on its own is not a downgrade.
    if smbident_check(b"james\0".as_ptr(), 0x0004) != VERDICT_OK {
        return 5;
    }

    // ---- 6. A 64-byte field with no NUL in it must not be run off the end,
    // and must still be treated as a named user.
    {
        let runon = [b'a'; NAME_MAX];
        if smbident_check(runon.as_ptr(), SMB2_SESSION_FLAG_IS_GUEST) != VERDICT_GUEST {
            return 6;
        }
    }

    // ---- 7. THE BIT NOTHING READ. SMB2_SESSION_FLAG_IS_NULL appeared in
    // net/smb.h and in no .c file anywhere in the kernel.
    if smbident_check(b"james\0".as_ptr(), SMB2_SESSION_FLAG_IS_NULL) != VERDICT_NULL {
        return 7;
    }
    if smbident_check(b"james\0".as_ptr(),
                      SMB2_SESSION_FLAG_IS_GUEST | SMB2_SESSION_FLAG_IS_NULL) != VERDICT_BOTH {
        return 7;
    }

    // ---- 8. Every refusal code must have its own text, and no refusal text
    // may claim the request failed or blame a credential, because the server
    // returned STATUS_SUCCESS. This is checked rather than asserted in prose:
    // the first half of #749 was a wrong diagnosis in an error message.
    for code in [VERDICT_GUEST, VERDICT_NULL, VERDICT_BOTH] {
        let t = smbident_verdict_text(code);
        // SAFETY: every arm returns a NUL-terminated literal.
        let (lc, n) = unsafe { read_lc(t) };
        let s = &lc[..n];
        if n == 0 {
            return 8;
        }
        if find(s, b"password").is_some() || find(s, b"authentication failed").is_some() {
            return 8;
        }
        // It must say the server ACCEPTED it, or the message is as misleading
        // as the one this ticket exists to stop.
        if find(s, b"accepted").is_none() {
            return 8;
        }
    }

    // ---- 9. THE FALSE-ALARM GUARD. net/smb.c's on-demand mount default is
    // "guest" with an empty NT hash, and parse_smb_url defaults to "guest"
    // too. If those counted as downgraded, every credential-free /SMB path in
    // the system would start refusing to mount.
    for u in [b"guest\0".as_ptr(), b"\0".as_ptr(), b"anonymous\0".as_ptr(),
              b"nobody\0".as_ptr(), b"GUEST\0".as_ptr()] {
        if smbident_name_is_anonymous(u) != 1 {
            return 9;
        }
        if smbident_check(u, SMB2_SESSION_FLAG_IS_GUEST) != VERDICT_OK {
            return 9;
        }
        if smbident_check(u, SMB2_SESSION_FLAG_IS_NULL) != VERDICT_OK {
            return 9;
        }
    }
    if smbident_name_is_anonymous(b"james\0".as_ptr()) != 0 {
        return 9;
    }
    // A name that merely CONTAINS an anonymous name is a person.
    if smbident_name_is_anonymous(b"guest2\0".as_ptr()) != 0 {
        return 9;
    }
    if smbident_check(b"guest2\0".as_ptr(), SMB2_SESSION_FLAG_IS_GUEST) != VERDICT_GUEST {
        return 9;
    }

    // ---- 10. IDENTITY EQUALITY, which is what stops a mount point being
    // quietly reused by a different user. THIS IS THE SECOND HALF OF THE
    // TICKET: a share whose reconnect failed was re-mounted at the same path
    // with the hard-coded "guest" default, so the path kept working and
    // belonged to somebody else.
    if smbident_same(wg, b"james\0".as_ptr(), wg, b"james\0".as_ptr()) != 1 {
        return 10;
    }
    if smbident_same(wg, b"JAMES\0".as_ptr(), wg, b"james\0".as_ptr()) != 1 {
        return 10;
    }
    if smbident_same(wg, b"james\0".as_ptr(), wg, b"guest\0".as_ptr()) != 0 {
        return 10;
    }
    if smbident_same(wg, b"james\0".as_ptr(), wg, b"\0".as_ptr()) != 0 {
        return 10;
    }
    if smbident_same(wg, b"james\0".as_ptr(), b"OTHERDOM\0".as_ptr(), b"james\0".as_ptr()) != 0 {
        return 10;
    }
    // All the anonymous spellings are ONE non-identity, so a retry that asks
    // for "guest" where "" was recorded is not a conflict.
    if smbident_same(wg, b"guest\0".as_ptr(), b"ANYTHING\0".as_ptr(), b"\0".as_ptr()) != 1 {
        return 10;
    }
    // A prefix must not compare equal to the longer name it is a prefix of.
    if smbident_same(wg, b"jam\0".as_ptr(), wg, b"james\0".as_ptr()) != 0 {
        return 10;
    }

    0
}

/// `s.find(needle)` without `std`.
fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || needle.len() > hay.len() {
        return None;
    }
    for i in 0..=(hay.len() - needle.len()) {
        if &hay[i..i + needle.len()] == needle {
            return Some(i);
        }
    }
    None
}
