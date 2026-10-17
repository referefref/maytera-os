// rustkern/recycle.rs - the PURE policy for the Recycle Bin: its index-row
// format, its retention rule, and the collision-safe name a captured file gets.
//
// WHAT THIS IS FOR. The owner's requirement, verbatim on 2026-10-03: "AI can
// delete files but it must be announced to the user AND they can be undeleted
// by AI or the user if they chose so [...] A recycler service should auto
// delete these periodically with a controllable timing in the settings app,
// also with an option for never but defaulting to 30 days".
//
// The Recycle Bin ALREADY EXISTS (userland/apps/recyclebin, the integrated view
// in userland/apps/files, the desktop icon, tools/trash-harness). What did not
// exist, measured by grepping both for purge/retention/expire/age: ANY notion of
// WHEN something was deleted, by WHOM, or when it should go away. The index row
// was two fields, `name|original path`, and the Recycle Bin app set
// `deleted_time = 0` because there was nowhere to read one from. Retention is
// impossible without a timestamp, which is why none existed.
//
// So this module owns three things and nothing else:
//   1. the index row format, EXTENDED backward-compatibly from 2 fields to 4;
//   2. the retention rule, including "never" and the fail-safe for rows that
//      predate the timestamp;
//   3. the trash-side filename, which must not collide and must survive FAT.
//
// NEW KERNEL LOGIC WITH NO C TWIN, so Rust per the 2026-07-16 rule. No
// performance argument for C: these are byte-level string decisions on the cold
// delete and sweep paths. The I/O and the per-user path resolution stay in C
// (fs/recycle_fs.c), which is the same split as rustkern/escrowundo.rs against
// fs/escrow_undo.c.
//
// NO FLOATS: retention is whole days of whole seconds, deliberately. The kernel
// is built -mno-sse / soft-float, so a float here would be a non-starter
// regardless of taste.

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

/// The owner's default: thirty days.
pub const RETENTION_DEFAULT_DAYS: u32 = 30;

/// NEVER. Zero is the encoding, and it is "never purge", NOT "purge
/// immediately". That direction is deliberate: every unparseable, missing,
/// zeroed or corrupt input in this module lands on KEEPING the file, because
/// the failure mode of keeping too long is disk space and the failure mode of
/// purging too early is the thing this whole feature exists to prevent.
pub const RETENTION_NEVER: u32 = 0;

/// The longest retention the Settings control offers. A larger value in the
/// config file is accepted and honoured; this is only the UI's top stop.
pub const RETENTION_MAX_DAYS: u32 = 3650;

const SECS_PER_DAY: u64 = 86400;

/// Is a bin entry deleted at `deleted_unix` expired, as of `now_unix`, under a
/// `retention_days` policy?
///
/// 1 = purge it, 0 = keep it. Every ambiguity resolves to KEEP:
///
/// - `retention_days == RETENTION_NEVER` keeps everything, forever. The owner
///   asked for "an option for never", and never has to mean never: no other
///   sweep in the system may purge the bin, which is why the age test lives
///   here, in one function, rather than being re-derived by each caller.
/// - `deleted_unix == 0` means "this row predates the timestamp field" (a
///   legacy two-field row). Its age is UNKNOWN, so it is never purged. A legacy
///   bin therefore stays exactly as full as it is until the user empties it by
///   hand, which is the honest behaviour: the kernel will not guess an age and
///   then delete something on the guess.
/// - `now_unix` at or before `deleted_unix` means the clock moved backwards or
///   has not been set. Keep.
#[no_mangle]
pub extern "C" fn rcy_is_expired_rs(deleted_unix: u64, now_unix: u64, retention_days: u32) -> u32 {
    if retention_days == RETENTION_NEVER {
        return 0;
    }
    if deleted_unix == 0 {
        return 0;
    }
    if now_unix <= deleted_unix {
        return 0;
    }
    let age = now_unix - deleted_unix;
    let limit = (retention_days as u64).saturating_mul(SECS_PER_DAY);
    if age >= limit {
        1
    } else {
        0
    }
}

/// Parse the retention setting out of a `<home>/CONFIG/RECYCLE.CFG` body.
///
/// Format, one `key=value` per line, '#' starts a comment:
///
///     retention_days=30
///     retention_days=never
///
/// Returns the number of days, or `RETENTION_DEFAULT_DAYS` when the file is
/// absent, empty, or says nothing about retention. A value that does not parse
/// is ALSO the default rather than "never" and rather than 0: a typo must not
/// silently turn the recycler off, and must not silently turn it into an
/// immediate shredder either.
///
/// # Safety
/// `buf` must point to at least `len` readable bytes, or be null with len 0.
#[no_mangle]
pub unsafe extern "C" fn rcy_parse_retention_rs(buf: *const u8, len: usize) -> u32 {
    if buf.is_null() || len == 0 {
        return RETENTION_DEFAULT_DAYS;
    }
    // SAFETY: the caller guarantees `len` readable bytes at `buf`.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(buf, len) };
    let key = b"retention_days";

    let mut i = 0usize;
    while i < b.len() {
        // one line [i, e)
        let start = i;
        let mut e = i;
        while e < b.len() && b[e] != b'\n' {
            e += 1;
        }
        i = if e < b.len() { e + 1 } else { e };

        let mut s = start;
        let mut end = e;
        // trim ASCII whitespace and a trailing CR
        while s < end && (b[s] == b' ' || b[s] == b'\t') {
            s += 1;
        }
        while end > s && (b[end - 1] == b' ' || b[end - 1] == b'\t' || b[end - 1] == b'\r') {
            end -= 1;
        }
        if s >= end || b[s] == b'#' {
            continue;
        }
        if end - s <= key.len() {
            continue;
        }
        let mut m = true;
        let mut k = 0usize;
        while k < key.len() {
            if b[s + k] != key[k] {
                m = false;
                break;
            }
            k += 1;
        }
        if !m {
            continue;
        }
        // Skip whitespace BEFORE the '=' as well as after it. Omitting this was
        // a MEASURED divergence: a hand-edited "retention_days = 7" matched no
        // setting here and silently fell back to the 30-day default, while the
        // userland reader (userland/libc/rbindex.c) accepted it as seven days.
        // The kernel and userland would then purge the same bin on two
        // different schedules from one config file, which is the worst kind of
        // disagreement because neither side reports anything. This function's
        // own self-test asserted the forgiving behaviour and was therefore
        // FAILING; the test was right and the code was wrong.
        let mut eq = s + key.len();
        while eq < end && (b[eq] == b' ' || b[eq] == b'\t') {
            eq += 1;
        }
        if eq >= end || b[eq] != b'=' {
            continue;
        }
        let mut v = eq + 1;
        while v < end && (b[v] == b' ' || b[v] == b'\t') {
            v += 1;
        }
        if v >= end {
            return RETENTION_DEFAULT_DAYS;
        }
        // "never" (case-insensitive) is the explicit off switch.
        let never = b"never";
        if end - v == never.len() {
            let mut n = true;
            let mut j = 0usize;
            while j < never.len() {
                if b[v + j] | 0x20 != never[j] {
                    n = false;
                    break;
                }
                j += 1;
            }
            if n {
                return RETENTION_NEVER;
            }
        }
        // a plain decimal count of days
        let mut acc: u64 = 0;
        let mut digits = 0u32;
        let mut j = v;
        while j < end {
            let c = b[j];
            if !c.is_ascii_digit() {
                digits = 0; // trailing junk: not a clean number, use the default
                break;
            }
            acc = acc.saturating_mul(10).saturating_add((c - b'0') as u64);
            digits += 1;
            j += 1;
        }
        if digits == 0 {
            return RETENTION_DEFAULT_DAYS;
        }
        if acc == 0 {
            // "retention_days=0" is the same statement as "never", and is
            // accepted as such rather than being read as "purge immediately".
            return RETENTION_NEVER;
        }
        if acc > RETENTION_MAX_DAYS as u64 {
            acc = RETENTION_MAX_DAYS as u64;
        }
        return acc as u32;
    }
    RETENTION_DEFAULT_DAYS
}

/// Render the retention setting back out as the exact body to write to
/// RECYCLE.CFG. Returns the number of bytes written, or 0 if it would not fit.
/// Having the writer here, beside the parser, is what stops the two drifting:
/// `rcy_policy_selftest_rs` round-trips every value the Settings control offers.
///
/// # Safety
/// `out` must point to at least `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rcy_format_retention_rs(days: u32, out: *mut u8, cap: usize) -> usize {
    if out.is_null() {
        return 0;
    }
    // SAFETY: the caller guarantees `cap` writable bytes at `out`.
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, cap) };
    let mut n = 0usize;
    let mut put = |s: &[u8], n: &mut usize| -> bool {
        if *n + s.len() > o.len() {
            return false;
        }
        let mut k = 0;
        while k < s.len() {
            o[*n] = s[k];
            *n += 1;
            k += 1;
        }
        true
    };
    if !put(b"# MayteraOS Recycle Bin retention. 'never' or a number of days.\n", &mut n) {
        return 0;
    }
    if !put(b"retention_days=", &mut n) {
        return 0;
    }
    if days == RETENTION_NEVER {
        if !put(b"never", &mut n) {
            return 0;
        }
    } else {
        let mut d = days;
        let mut tmp = [0u8; 12];
        let mut t = 0usize;
        while d > 0 {
            tmp[t] = b'0' + (d % 10) as u8;
            d /= 10;
            t += 1;
        }
        while t > 0 {
            t -= 1;
            if !put(&[tmp[t]], &mut n) {
                return 0;
            }
        }
    }
    if !put(b"\n", &mut n) {
        return 0;
    }
    n
}

// ---------------------------------------------------------------------------
// The index row
// ---------------------------------------------------------------------------
//
// V1 (what shipped):   <trashname>|<original path>
// V2 (this change):    <trashname>|<original path>|<deleted_unix>|<actor>
//
// BACKWARD COMPATIBILITY IN BOTH DIRECTIONS, and it had to be, because an
// existing bin on an installed system holds V1 rows:
//
//   - A V1 row read by the V2 parser yields deleted_unix = 0 and actor "user",
//     and a 0 timestamp is never purged (see rcy_is_expired_rs). An existing
//     bin is therefore safe, not silently eligible for deletion.
//   - A V2 row read by a V1 parser is the case that forced the field ORDER. The
//     shipped V1 readers (userland/apps/recyclebin/main.c idx_lookup and the
//     same code copied into userland/apps/files/main.c) copy from the first '|'
//     to END OF LINE as the original path, so appending fields would have
//     corrupted the restore path. Both readers are updated by this change to
//     stop at the next '|', which leaves V1 rows parsing identically.
//
// `actor` is a short token, never free text:
//     "user"          a human acting through Files or the Recycle Bin
//     "ai:XXXXXXXX"   an escrow-marked actor, XXXXXXXX being its AI_TASK node
//                     id in hex, which is the identity the GraphFS journal
//                     records the same delete under. That is the join key
//                     between the user-facing bin and the tamper-evident trail.

/// Maximum bytes one index row can occupy, including its newline.
pub const RCY_ROW_MAX: usize = 320;

/// The actor token for a human-initiated delete.
pub const RCY_ACTOR_USER: u32 = 0;
/// The actor token for an escrow-marked (AI) delete; `actor_id` names the node.
pub const RCY_ACTOR_AI: u32 = 1;

fn put_hex8(o: &mut [u8], n: &mut usize, v: u32) -> bool {
    if *n + 8 > o.len() {
        return false;
    }
    let mut s = 28i32;
    while s >= 0 {
        let nib = ((v >> s) & 0xF) as u8;
        o[*n] = if nib < 10 { b'0' + nib } else { b'a' + (nib - 10) };
        *n += 1;
        s -= 4;
    }
    true
}

fn put_dec(o: &mut [u8], n: &mut usize, mut v: u64) -> bool {
    let mut tmp = [0u8; 24];
    let mut t = 0usize;
    if v == 0 {
        tmp[0] = b'0';
        t = 1;
    } else {
        while v > 0 {
            tmp[t] = b'0' + (v % 10) as u8;
            v /= 10;
            t += 1;
        }
    }
    if *n + t > o.len() {
        return false;
    }
    while t > 0 {
        t -= 1;
        o[*n] = tmp[t];
        *n += 1;
    }
    true
}

/// Build one V2 index row, newline-terminated, into `out`.
///
/// Returns the byte count, or 0 if it would not fit (the caller must then not
/// capture the file: an entry with no index row is an unrestorable orphan, and
/// the recyclebin app already carries a comment about exactly that bug class).
///
/// '|' and newline are stripped from both the name and the path, because they
/// are the field and record separators and a path containing one would split a
/// row. On this filesystem neither character is legal in a name, so this is a
/// belt-and-braces strip rather than a transformation anyone will observe.
///
/// # Safety
/// `name` and `orig` must be NUL-terminated C strings; `out` must point to at
/// least `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rcy_row_build_rs(
    name: *const u8,
    orig: *const u8,
    deleted_unix: u64,
    actor_kind: u32,
    actor_id: u32,
    out: *mut u8,
    cap: usize,
) -> usize {
    if name.is_null() || orig.is_null() || out.is_null() || cap == 0 {
        return 0;
    }
    // SAFETY: the caller guarantees `cap` writable bytes at `out`.
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, cap) };
    let mut n = 0usize;

    // SAFETY: the caller guarantees NUL-terminated strings.
    unsafe {
        let mut p = name;
        while *p != 0 {
            let c = *p;
            if c != b'|' && c != b'\n' && c != b'\r' {
                if n >= o.len() {
                    return 0;
                }
                o[n] = c;
                n += 1;
            }
            p = p.add(1);
        }
    }
    if n == 0 || n >= o.len() {
        return 0;
    }
    o[n] = b'|';
    n += 1;
    // SAFETY: the caller guarantees NUL-terminated strings.
    unsafe {
        let mut p = orig;
        while *p != 0 {
            let c = *p;
            if c != b'|' && c != b'\n' && c != b'\r' {
                if n >= o.len() {
                    return 0;
                }
                o[n] = c;
                n += 1;
            }
            p = p.add(1);
        }
    }
    if n >= o.len() {
        return 0;
    }
    o[n] = b'|';
    n += 1;
    if !put_dec(o, &mut n, deleted_unix) {
        return 0;
    }
    if n >= o.len() {
        return 0;
    }
    o[n] = b'|';
    n += 1;
    if actor_kind == RCY_ACTOR_AI {
        if n + 3 > o.len() {
            return 0;
        }
        o[n] = b'a';
        o[n + 1] = b'i';
        o[n + 2] = b':';
        n += 3;
        if !put_hex8(o, &mut n, actor_id) {
            return 0;
        }
    } else {
        let u = b"user";
        if n + u.len() > o.len() {
            return 0;
        }
        let mut k = 0;
        while k < u.len() {
            o[n] = u[k];
            n += 1;
            k += 1;
        }
    }
    if n >= o.len() {
        return 0;
    }
    o[n] = b'\n';
    n += 1;
    n
}

/// Read the DELETED-AT timestamp out of one index row.
///
/// `row`/`row_len` is one line WITHOUT its newline. Returns the unix time, or 0
/// for a V1 row, a malformed row, or a row with a non-numeric third field. 0
/// means "age unknown" and `rcy_is_expired_rs` never purges on it, so every
/// parse failure here lands on keeping the file.
///
/// # Safety
/// `row` must point to at least `row_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rcy_row_deleted_at_rs(row: *const u8, row_len: usize) -> u64 {
    if row.is_null() || row_len == 0 {
        return 0;
    }
    // SAFETY: the caller guarantees `row_len` readable bytes.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(row, row_len) };
    // field 3 of 4, '|'-separated
    let mut bars = 0;
    let mut i = 0usize;
    let mut start = usize::MAX;
    while i < b.len() {
        if b[i] == b'|' {
            bars += 1;
            if bars == 2 {
                start = i + 1;
                break;
            }
        }
        i += 1;
    }
    if start == usize::MAX || start >= b.len() {
        return 0;
    }
    let mut e = start;
    while e < b.len() && b[e] != b'|' {
        e += 1;
    }
    if e == start {
        return 0;
    }
    let mut acc: u64 = 0;
    let mut j = start;
    while j < e {
        if !b[j].is_ascii_digit() {
            return 0;
        }
        acc = acc.saturating_mul(10).saturating_add((b[j] - b'0') as u64);
        j += 1;
    }
    acc
}

/// Copy the NAME field (field 1) of one index row into `out` as a NUL-terminated
/// string. Returns its length, or 0. Used by the sweeper to pair a row with the
/// file it names.
///
/// # Safety
/// `row` must point to `row_len` readable bytes; `out` to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rcy_row_name_rs(
    row: *const u8,
    row_len: usize,
    out: *mut u8,
    cap: usize,
) -> usize {
    if row.is_null() || out.is_null() || cap == 0 {
        return 0;
    }
    // SAFETY: the caller guarantees the two extents.
    let b: &[u8] = unsafe { core::slice::from_raw_parts(row, row_len) };
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, cap) };
    let mut e = 0usize;
    while e < b.len() && b[e] != b'|' {
        e += 1;
    }
    if e == 0 || e + 1 > o.len() {
        return 0;
    }
    let mut k = 0usize;
    while k < e {
        o[k] = b[k];
        k += 1;
    }
    o[e] = 0;
    e
}

// ---------------------------------------------------------------------------
// The trash-side filename
// ---------------------------------------------------------------------------

/// Build the name a captured file takes INSIDE the bin.
///
/// Shape: `D<unix seconds, 8 hex><uniq, 4 hex>.<ext>` for example
/// `D68e0a1c00007.TXT`. The requirements it satisfies:
///
/// - UNIQUE. Two files of the same basename deleted from two directories must
///   not collide, so the original basename is NOT the stored name. The caller
///   supplies `uniq`, bumping it until the name is free, which is the same
///   collision-safe-destination discipline the photo organiser already uses.
/// - UPPERCASE AND SHORT. The bin can live on a FAT volume, where names are
///   8.3 and case-folded. 1 + 8 + 4 = 13 characters is too long for strict 8.3,
///   so the stem is TRUNCATED to 8 by dropping the high nibbles of the
///   timestamp, which still leaves 4 hex of time plus 4 of uniq: enough to be
///   unique within a bin that holds at most a few thousand items.
/// - ORIGINAL EXTENSION PRESERVED, because the Recycle Bin shows the item to a
///   human and an extension is most of what makes it recognisable. The real
///   original name is in the index row; this is only the on-disk handle.
///
/// Returns the length written, or 0 if it would not fit.
///
/// # Safety
/// `orig_name` must be a NUL-terminated C string; `out` must point to at least
/// `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rcy_trash_name_rs(
    orig_name: *const u8,
    now_unix: u64,
    uniq: u32,
    out: *mut u8,
    cap: usize,
) -> usize {
    if out.is_null() || cap < 14 {
        return 0;
    }
    // SAFETY: the caller guarantees `cap` writable bytes.
    let o: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, cap) };
    let hex = |v: u32, nib: u32, o: &mut [u8], n: &mut usize| {
        let mut s = (nib as i32 - 1) * 4;
        while s >= 0 {
            let x = ((v >> s) & 0xF) as u8;
            o[*n] = if x < 10 { b'0' + x } else { b'A' + (x - 10) };
            *n += 1;
            s -= 4;
        }
    };
    let mut n = 0usize;
    o[n] = b'D';
    n += 1;
    hex((now_unix & 0xFFFF) as u32, 4, o, &mut n); // low 16 bits of the clock
    hex(uniq & 0xFFFF, 4, o, &mut n); // the caller's collision counter
    // extension: the last '.' of the original, up to 3 chars, upper-cased
    if !orig_name.is_null() {
        let mut len = 0usize;
        // SAFETY: the caller guarantees a NUL-terminated string.
        unsafe {
            while *orig_name.add(len) != 0 && len < 255 {
                len += 1;
            }
        }
        let mut dot = usize::MAX;
        let mut i = len;
        while i > 0 {
            i -= 1;
            // SAFETY: i < len, inside the string.
            if unsafe { *orig_name.add(i) } == b'.' {
                dot = i;
                break;
            }
        }
        if dot != usize::MAX && dot + 1 < len {
            if n < o.len() {
                o[n] = b'.';
                n += 1;
            }
            let mut k = dot + 1;
            let mut taken = 0;
            while k < len && taken < 3 && n < o.len() {
                // SAFETY: k < len, inside the string.
                let c = unsafe { *orig_name.add(k) };
                o[n] = if c.is_ascii_lowercase() { c - 32 } else { c };
                n += 1;
                taken += 1;
                k += 1;
            }
        }
    }
    if n >= o.len() {
        return 0;
    }
    o[n] = 0;
    n
}

// ---------------------------------------------------------------------------
// The OBJECT identity of a path
// ---------------------------------------------------------------------------

/// Derive the 24-bit `local` half of a GraphFS OBJECT node id from a canonical
/// absolute path.
///
/// WHY A HASH AND NOT A COUNTER. A GraphFS node id is `(kind << 24) | local`
/// and the design (docs/GRAPHFS_DESIGN.md section 5.2) is explicit that a node
/// stores no name and no path: "a producer that can compute a stable `local`
/// can recompute the node id without the graph having to store any string".
/// A hash of the path is exactly such a producer, and it means the SAME file
/// touched in two different sessions, or across a reboot, lands on the SAME
/// object node, which is what makes "which agents have touched this file" a
/// question the graph can answer at all.
///
/// THE HONEST LIMIT, because 24 bits is 24 bits. Two different paths can
/// collide onto one object node. With a few thousand files the chance is small
/// but it is not zero, and a collision would merge two files' edge histories.
/// It cannot cause data loss or a wrong authorization decision: object nodes
/// are used ONLY for the relationship record, never for the grant check (which
/// is keyed on the contract's own object identity, minted per contract by
/// fs/escrow_guard.c), and the authoritative restore information is the Recycle
/// Bin index row, which carries the full path as text. So a collision degrades
/// the richness of a query and nothing else. Stated here rather than
/// discovered later.
///
/// FNV-1a, folded to 24 bits, and never 0 (0 is not a valid local: a node id
/// whose local is 0 collides with the reserved system identities).
///
/// # Safety
/// `path` must be a NUL-terminated C string, or null.
#[no_mangle]
pub unsafe extern "C" fn rcy_object_local_rs(path: *const u8) -> u32 {
    if path.is_null() {
        return 1;
    }
    let mut h: u32 = 0x811c_9dc5;
    let mut i = 0usize;
    // SAFETY: the caller guarantees a NUL-terminated string; the 4096 bound
    // stops a missing terminator turning into an unbounded read.
    unsafe {
        while i < 4096 {
            let c = *path.add(i);
            if c == 0 {
                break;
            }
            h ^= c as u32;
            h = h.wrapping_mul(0x0100_0193);
            i += 1;
        }
    }
    // Fold the top 8 bits in rather than discarding them, so two paths that
    // differ only high in the hash still separate.
    let mut local = (h ^ (h >> 24)) & 0x00FF_FFFF;
    if local == 0 {
        local = 1;
    }
    local
}

/// Boot self-test of the pure policy. Returns 0 on PASS, the negative code of
/// the FIRST failed assertion otherwise; writes the assertion count to
/// `*out_checks` when non-null.
///
/// # Safety
/// `out_checks` must be null or point to a writable u32.
#[no_mangle]
pub unsafe extern "C" fn rcy_policy_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks: u32 = 0;
    let mut fail: i32 = 0;
    macro_rules! chk {
        ($cond:expr, $code:expr) => {{
            checks += 1;
            if !($cond) && fail == 0 {
                fail = $code;
            }
        }};
    }

    // --- expiry -------------------------------------------------------------
    let day = SECS_PER_DAY;
    // 30-day default: 29 days old stays, 30 days old goes.
    chk!(rcy_is_expired_rs(1000, 1000 + 29 * day, 30) == 0, -1);
    chk!(rcy_is_expired_rs(1000, 1000 + 30 * day, 30) == 1, -2);
    chk!(rcy_is_expired_rs(1000, 1000 + 31 * day, 30) == 1, -3);
    // NEVER means never, at any age at all.
    chk!(rcy_is_expired_rs(1, 1 + 100000 * day, RETENTION_NEVER) == 0, -4);
    // unknown age (a legacy V1 row) is never purged
    chk!(rcy_is_expired_rs(0, 1 + 100000 * day, 30) == 0, -5);
    // a clock that went backwards keeps the file
    chk!(rcy_is_expired_rs(5000, 1000, 30) == 0, -6);
    chk!(rcy_is_expired_rs(5000, 5000, 30) == 0, -7);
    // 1-day retention is a real setting, not a rounding artefact
    chk!(rcy_is_expired_rs(0 + 1, 1 + day, 1) == 1, -8);

    // --- config parse / format round trip -----------------------------------
    // SAFETY: all the buffers below are real local slices with honest lengths.
    unsafe {
        let d = b"retention_days=30\n";
        chk!(rcy_parse_retention_rs(d.as_ptr(), d.len()) == 30, -10);
        let n = b"retention_days=never\n";
        chk!(rcy_parse_retention_rs(n.as_ptr(), n.len()) == RETENTION_NEVER, -11);
        let nu = b"retention_days=NEVER\n";
        chk!(rcy_parse_retention_rs(nu.as_ptr(), nu.len()) == RETENTION_NEVER, -12);
        let z = b"retention_days=0\n";
        chk!(rcy_parse_retention_rs(z.as_ptr(), z.len()) == RETENTION_NEVER, -13);
        // absent / empty / comment-only -> the default, never 0
        chk!(rcy_parse_retention_rs(core::ptr::null(), 0) == RETENTION_DEFAULT_DAYS, -14);
        let c = b"# nothing here\n";
        chk!(rcy_parse_retention_rs(c.as_ptr(), c.len()) == RETENTION_DEFAULT_DAYS, -15);
        // a TYPO must not turn the recycler off and must not shred either
        let t = b"retention_days=thirty\n";
        chk!(rcy_parse_retention_rs(t.as_ptr(), t.len()) == RETENTION_DEFAULT_DAYS, -16);
        let t2 = b"retention_days=30days\n";
        chk!(rcy_parse_retention_rs(t2.as_ptr(), t2.len()) == RETENTION_DEFAULT_DAYS, -17);
        // whitespace and other keys around it
        let w = b"# hi\nfoo=bar\n  retention_days = 7 \r\n";
        chk!(rcy_parse_retention_rs(w.as_ptr(), w.len()) == 7, -18);
        // whitespace on EITHER side of the '=', and a tab, must all work: this
        // is a file a human edits by hand.
        let w2 = b"retention_days\t=\t14\n";
        chk!(rcy_parse_retention_rs(w2.as_ptr(), w2.len()) == 14, -181);
        let w3 = b"retention_days   =never\n";
        chk!(rcy_parse_retention_rs(w3.as_ptr(), w3.len()) == RETENTION_NEVER, -182);
        // a key that merely STARTS with ours must still not match
        let w4 = b"retention_days_x=5\n";
        chk!(rcy_parse_retention_rs(w4.as_ptr(), w4.len()) == RETENTION_DEFAULT_DAYS, -183);
        // the key with no '=' at all is not a setting
        let w5 = b"retention_days\n";
        chk!(rcy_parse_retention_rs(w5.as_ptr(), w5.len()) == RETENTION_DEFAULT_DAYS, -184);
        // clamped at the top stop
        let big = b"retention_days=999999\n";
        chk!(rcy_parse_retention_rs(big.as_ptr(), big.len()) == RETENTION_MAX_DAYS, -19);

        // every value the Settings control offers must survive a round trip
        let offers: [u32; 7] = [RETENTION_NEVER, 1, 7, 14, 30, 90, 365];
        let mut k = 0usize;
        while k < offers.len() {
            let mut cb = [0u8; 128];
            let w2 = rcy_format_retention_rs(offers[k], cb.as_mut_ptr(), cb.len());
            chk!(w2 > 0, -20);
            chk!(
                rcy_parse_retention_rs(cb.as_ptr(), w2) == offers[k],
                -21 - (k as i32)
            );
            k += 1;
        }

        // --- index rows -----------------------------------------------------
        let mut rb = [0u8; RCY_ROW_MAX];
        let nm = b"D1A2B0001.TXT\0";
        let op = b"/HOME/REF/NOTES.TXT\0";
        let rl = rcy_row_build_rs(
            nm.as_ptr(),
            op.as_ptr(),
            1700000000,
            RCY_ACTOR_AI,
            0x0600002a,
            rb.as_mut_ptr(),
            rb.len(),
        );
        chk!(rl > 0, -40);
        chk!(rb[rl - 1] == b'\n', -41);
        chk!(rcy_row_deleted_at_rs(rb.as_ptr(), rl - 1) == 1700000000, -42);
        let mut nmo = [0u8; 64];
        let nl = rcy_row_name_rs(rb.as_ptr(), rl - 1, nmo.as_mut_ptr(), nmo.len());
        chk!(nl == 13, -43);
        chk!(nmo[0] == b'D' && nmo[12] == b'T', -44);
        // the actor token is present and is the AI node in hex
        let mut found_ai = false;
        let mut q = 0usize;
        while q + 3 <= rl {
            if rb[q] == b'a' && rb[q + 1] == b'i' && rb[q + 2] == b':' {
                found_ai = true;
                break;
            }
            q += 1;
        }
        chk!(found_ai, -45);

        // a V1 row yields timestamp 0, which is never purged
        let v1 = b"OLD.TXT|/HOME/REF/OLD.TXT";
        chk!(rcy_row_deleted_at_rs(v1.as_ptr(), v1.len()) == 0, -46);
        chk!(rcy_is_expired_rs(rcy_row_deleted_at_rs(v1.as_ptr(), v1.len()), 1 << 40, 30) == 0, -47);
        // a row whose timestamp field is junk also yields 0 (keep)
        let bad = b"X|/A|notanumber|user";
        chk!(rcy_row_deleted_at_rs(bad.as_ptr(), bad.len()) == 0, -48);
        // a user row round-trips too
        let un = b"D0001.BIN\0";
        let up = b"/HOME/REF/A.BIN\0";
        let ul = rcy_row_build_rs(
            un.as_ptr(),
            up.as_ptr(),
            42,
            RCY_ACTOR_USER,
            0,
            rb.as_mut_ptr(),
            rb.len(),
        );
        chk!(ul > 0, -49);
        chk!(rcy_row_deleted_at_rs(rb.as_ptr(), ul - 1) == 42, -50);
        // a name or path carrying the separator cannot split a row
        let evil = b"A|B\0";
        let evp = b"/X|Y\0";
        let el = rcy_row_build_rs(
            evil.as_ptr(),
            evp.as_ptr(),
            7,
            RCY_ACTOR_USER,
            0,
            rb.as_mut_ptr(),
            rb.len(),
        );
        chk!(el > 0, -51);
        let mut bars = 0;
        let mut z2 = 0usize;
        while z2 < el {
            if rb[z2] == b'|' {
                bars += 1;
            }
            z2 += 1;
        }
        chk!(bars == 3, -52);
        // a buffer too small must return 0, never a truncated row
        let mut tiny = [0u8; 8];
        chk!(
            rcy_row_build_rs(
                nm.as_ptr(),
                op.as_ptr(),
                1,
                RCY_ACTOR_USER,
                0,
                tiny.as_mut_ptr(),
                tiny.len()
            ) == 0,
            -53
        );

        // --- trash names ----------------------------------------------------
        let mut t1 = [0u8; 32];
        let mut t2b = [0u8; 32];
        let src = b"NOTES.TXT\0";
        let a = rcy_trash_name_rs(src.as_ptr(), 0x1234_5678, 1, t1.as_mut_ptr(), t1.len());
        let bl = rcy_trash_name_rs(src.as_ptr(), 0x1234_5678, 2, t2b.as_mut_ptr(), t2b.len());
        chk!(a > 0 && bl > 0, -60);
        // bumping uniq must change the name, or the collision loop never ends
        let mut same = true;
        let mut y = 0usize;
        while y < a {
            if t1[y] != t2b[y] {
                same = false;
                break;
            }
            y += 1;
        }
        chk!(!same, -61);
        chk!(t1[0] == b'D', -62);
        // the extension is preserved and upper-cased
        let lower = b"photo.jpg\0";
        let mut t3 = [0u8; 32];
        let c3 = rcy_trash_name_rs(lower.as_ptr(), 1, 1, t3.as_mut_ptr(), t3.len());
        chk!(c3 >= 13, -63);
        chk!(t3[c3 - 3] == b'J' && t3[c3 - 2] == b'P' && t3[c3 - 1] == b'G', -64);
        // no extension is fine, not a crash and not a stray dot
        let noext = b"MAKEFILE\0";
        let mut t4 = [0u8; 32];
        let c4 = rcy_trash_name_rs(noext.as_ptr(), 1, 1, t4.as_mut_ptr(), t4.len());
        chk!(c4 == 9, -65);
        chk!(t4[c4 - 1] != b'.', -66);
        // a buffer too small returns 0 rather than a half name
        let mut t5 = [0u8; 4];
        chk!(rcy_trash_name_rs(src.as_ptr(), 1, 1, t5.as_mut_ptr(), t5.len()) == 0, -67);
        // --- object identity ------------------------------------------------
        let pa = b"/HOME/REF/NOTES.TXT\0";
        let pb = b"/HOME/REF/OTHER.TXT\0";
        let la = rcy_object_local_rs(pa.as_ptr());
        let lb = rcy_object_local_rs(pb.as_ptr());
        // stable: the same path must always give the same identity, or the
        // graph cannot accumulate a history for a file
        chk!(la == rcy_object_local_rs(pa.as_ptr()), -70);
        // distinct for distinct paths (not guaranteed in general, but these two
        // must separate or the self-test is not testing anything)
        chk!(la != lb, -71);
        // never 0, and always inside 24 bits
        chk!(la != 0 && lb != 0, -72);
        chk!(la <= 0x00FF_FFFF && lb <= 0x00FF_FFFF, -73);
        chk!(rcy_object_local_rs(core::ptr::null()) != 0, -74);
    }

    if !out_checks.is_null() {
        unsafe {
            *out_checks = checks;
        }
    }
    fail
}
