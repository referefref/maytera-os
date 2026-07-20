// rustkern/theme.rs - #404 batch-1 theme-file line tokenizer (gui/theme_parser.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ============================================================================
// #404 BATCH-1 SEAM 3/3: theme-file line tokenizer (gui/theme_parser.c)
// ============================================================================
// ============================================================================
// #404 Phase (theme): theme-file line-record parse seam, Rust port.
// Paste this block into rustkern.rs. gui/theme_parser.c routes its untrusted
// line tokenizer to theme_parse_line_rs under -DRUST_THEME_PARSE.
//
// Untrusted input: a downloaded/edited theme .ini file (task #141). This is the
// ATOM theme_parse_ini()'s file loop repeats: read ONE logical line out of the
// raw, NOT-NUL-terminated bytes `data[0..size]` and classify it into
// {skip | [section] | key=value}, hard-capping every fixed output field.
//
// The verbatim C (theme_parse_line_c) is already fully bounded: no reachable
// OOB was found (ASan clean over ~2.9M malformed vectors). This port is
// defense-in-depth: it removes the whole raw-pointer-scan class BY CONSTRUCTION
// (a slice of EXACTLY `size` bytes, every read via a saturating accessor, every
// out write index-checked against the fixed cap) and is byte-identical to the C
// on all well-formed and malformed input.
// ============================================================================

// Caps: byte-identical to gui/theme_parser.c #defines.
const TP_MAX_LINE_LEN: usize = 256;
const TP_MAX_SECTION_LEN: usize = 64;
const TP_MAX_KEY_LEN: usize = 64;
const TP_MAX_VALUE_LEN: usize = 128;

const TP_LINE_SKIP: i32 = 0;
const TP_LINE_SECTION: i32 = 1;
const TP_LINE_KEYVALUE: i32 = 2;

// #[repr(C)] mirror of theme_line_t. sizeof locked at 264 (see assert below).
#[repr(C)]
pub struct ThemeLine {
    pub kind: i32,
    pub consumed: u32,
    pub section: [u8; TP_MAX_SECTION_LEN],
    pub key: [u8; TP_MAX_KEY_LEN],
    pub value: [u8; TP_MAX_VALUE_LEN],
}

// Compile-time sizeof/layout lock (mirrors the C _Static_assert).
const _: () = {
    assert!(core::mem::size_of::<ThemeLine>() == 264);
    assert!(core::mem::align_of::<ThemeLine>() == 4);
};

// kernel string.h isspace(), verbatim (ASCII; bytes >= 0x80 are never space).
#[inline]
fn tp_isspace(c: u8) -> bool {
    c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' || c == 0x0c || c == 0x0b
}

// Copy a NUL-terminated run out of `src` into a fixed `dst` cap, stopping at the
// first 0 or the cap. Every write is dst[i] with i < N, so it can never overrun.
#[inline]
fn tp_publish(dst: &mut [u8], src: &[u8]) {
    let mut i = 0usize;
    while i < dst.len() {
        let b = if i < src.len() { src[i] } else { 0 };
        dst[i] = b;
        if b == 0 {
            break;
        }
        i += 1;
    }
}

/// theme_parse_line_rs: read one line from `data[0..size]` into `out`.
/// Returns bytes consumed (the advance), matching theme_parse_line_c.
///
/// SAFETY: `data` points to at least `size` readable bytes and `out` is a valid
/// writable ThemeLine*, per the C ABI contract (gui/theme_parser.c passes
/// `theme_file_bytes + p`, `remaining`, `&rec`). Null / zero-size is handled.
#[no_mangle]
pub unsafe extern "C" fn theme_parse_line_rs(
    data: *const u8,
    size: usize,
    out: *mut ThemeLine,
) -> u32 {
    if out.is_null() {
        return 0;
    }
    // POD zero-init: kind=SKIP, consumed=0, all strings empty.
    let o: &mut ThemeLine = &mut *out;
    o.kind = TP_LINE_SKIP;
    o.consumed = 0;
    o.section = [0; TP_MAX_SECTION_LEN];
    o.key = [0; TP_MAX_KEY_LEN];
    o.value = [0; TP_MAX_VALUE_LEN];

    if data.is_null() || size == 0 {
        return 0;
    }

    // The ONLY unsafe read: build a slice of EXACTLY `size` bytes. Everything
    // downstream indexes this slice (bounds-checked) - no raw pointer walks.
    let s: &[u8] = core::slice::from_raw_parts(data, size);

    theme_parse_line_impl(s, o)
}

// Pure, panic-free core over a bounded slice. Split out so the offline harness
// (and a future cargo test) can call it without FFI.
fn theme_parse_line_impl(s: &[u8], o: &mut ThemeLine) -> u32 {
    // ---- line reader (verbatim theme_parse_ini bounds) ----
    // Copy into a fixed 256-byte line buffer, stopping at '\n', EOF (slice end),
    // or LINE_LEN-1. NUL-terminate. `line_len` can never exceed cap-1.
    let mut line = [0u8; TP_MAX_LINE_LEN];
    let mut line_len = 0usize;
    let mut p = 0usize;
    while p < s.len() && s[p] != b'\n' && line_len < TP_MAX_LINE_LEN - 1 {
        line[line_len] = s[p];
        line_len += 1;
        p += 1;
    }
    // line[line_len] stays 0 (NUL terminator); line_len <= 255 so index valid.

    // Skip one newline if present (verbatim: only when NOT truncated past cap).
    if p < s.len() && s[p] == b'\n' {
        p += 1;
    }
    o.consumed = p as u32;

    // The verbatim C classifiers (is_comment_or_empty / parse_section_header /
    // parse_key_value) all scan the NUL-terminated `line[]` with `while (*p...)`,
    // so an EMBEDDED 0x00 byte in the input terminates the logical line for
    // classification (even though the line reader counted it toward `consumed`).
    // Mirror that exactly: the classified view ends at the first embedded NUL.
    let mut eff = 0usize;
    while eff < line_len && line[eff] != 0 {
        eff += 1;
    }
    let ln: &[u8] = &line[..eff];

    // ---- is_comment_or_empty ----
    let t = skip_ws(ln);
    if t.is_empty() || t[0] == b'#' || t[0] == b';' {
        o.kind = TP_LINE_SKIP;
        return o.consumed;
    }

    // ---- parse_section_header ----
    if let Some(section) = parse_section_header(ln) {
        o.kind = TP_LINE_SECTION;
        tp_publish(&mut o.section, &section.0[..section.1]);
        return o.consumed;
    }

    // ---- parse_key_value ----
    if let Some((klen, key, vlen, val)) = parse_key_value(ln) {
        o.kind = TP_LINE_KEYVALUE;
        tp_publish(&mut o.key, &key[..klen]);
        tp_publish(&mut o.value, &val[..vlen]);
        return o.consumed;
    }

    o.kind = TP_LINE_SKIP;
    o.consumed
}

// Return the sub-slice after leading whitespace.
#[inline]
fn skip_ws(mut s: &[u8]) -> &[u8] {
    while !s.is_empty() && tp_isspace(s[0]) {
        s = &s[1..];
    }
    s
}

// Trim trailing whitespace: return the used length of `buf[..len]`.
#[inline]
fn trim_trailing(buf: &[u8], mut len: usize) -> usize {
    while len > 0 && tp_isspace(buf[len - 1]) {
        len -= 1;
    }
    len
}

// parse_section_header: returns (section_buf, used_len) if `[name]`, else None.
// Every push is guarded by count < CAP-1 so the fixed buffer never overruns.
fn parse_section_header(line: &[u8]) -> Option<([u8; TP_MAX_SECTION_LEN], usize)> {
    let mut s = skip_ws(line);
    if s.is_empty() || s[0] != b'[' {
        return None;
    }
    s = &s[1..]; // skip '['

    let mut out = [0u8; TP_MAX_SECTION_LEN];
    let mut count = 0usize;
    let mut i = 0usize;
    while i < s.len() && s[i] != b']' && count < TP_MAX_SECTION_LEN - 1 {
        out[count] = s[i];
        count += 1;
        i += 1;
    }
    // require a closing ']' at the current scan position
    if i >= s.len() || s[i] != b']' {
        return None;
    }
    let used = trim_trailing(&out, count);
    Some((out, used))
}

// parse_key_value: returns (key_len, key_buf, val_len, val_buf) or None.
type Kv = (usize, [u8; TP_MAX_KEY_LEN], usize, [u8; TP_MAX_VALUE_LEN]);
fn parse_key_value(line: &[u8]) -> Option<Kv> {
    let mut s = skip_ws(line);

    // key: until '=', whitespace, or cap-1
    let mut key = [0u8; TP_MAX_KEY_LEN];
    let mut kcount = 0usize;
    let mut i = 0usize;
    while i < s.len() && s[i] != b'=' && !tp_isspace(s[i]) && kcount < TP_MAX_KEY_LEN - 1 {
        key[kcount] = s[i];
        kcount += 1;
        i += 1;
    }
    if kcount == 0 {
        return None;
    }
    s = &s[i..];

    // skip whitespace, require '='
    s = skip_ws(s);
    if s.is_empty() || s[0] != b'=' {
        return None;
    }
    s = &s[1..];
    s = skip_ws(s);

    // value: until '#', ';', EOL, or cap-1
    let mut val = [0u8; TP_MAX_VALUE_LEN];
    let mut vcount = 0usize;
    let mut j = 0usize;
    while j < s.len() && s[j] != b'#' && s[j] != b';' && vcount < TP_MAX_VALUE_LEN - 1 {
        val[vcount] = s[j];
        vcount += 1;
        j += 1;
    }
    let vused = trim_trailing(&val, vcount);
    Some((kcount, key, vused, val))
}
