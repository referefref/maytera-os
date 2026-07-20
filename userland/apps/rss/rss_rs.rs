// rss_rs.rs - MayteraOS USERLAND (Ring-3) Rust feed parser for the RSS reader.
//
// Task: port the RSS/Atom/RDF feed parser to Rust. This is the userland analog
// of the kernel Rust port + the Arena BSP parser (#491): a real Rust object
// compiled for the USERLAND ABI and linked into the rss app ELF, parsing
// UNTRUSTED network bytes (arbitrary feed URLs) with bounds-checked, panic-free
// code so a malformed/hostile feed can never OOB or crash the reader.
//
// Handles ALL common feed formats in ONE pass:
//   * RSS 2.0    <rss><channel><item>...
//   * Atom 1.0   <feed><entry>...
//   * RSS 1.0/RDF <rdf:RDF><channel/><item>...   (items are RDF children)
//
// BUILD (pinned, see rust-toolchain.toml -> rustc 1.97.0):
//   rustc --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
//         -C opt-level=2 -C panic=abort \
//         -C code-model=large -C relocation-model=static
// The code/relocation model overrides match the C app ABI (-mcmodel=large,
// -fno-pic, linked at 0x80000000). See ARENA_BSP_PLAN.md for the full rationale.
//
// FFI is repr(C), pointer/int only. All returned strings are malloc'd C strings
// on the ONE shared libc heap; rss_free() frees the whole tree. No f32 crosses
// the boundary, so the soft-float `core` ABI is never exercised.

#![no_std]

extern crate alloc;

use core::alloc::{GlobalAlloc, Layout};
use core::panic::PanicInfo;
use alloc::vec::Vec;

// ---- userland libc FFI (ONE shared heap) ----------------------------------
extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
    fn realloc(ptr: *mut u8, size: usize) -> *mut u8;
    fn abort() -> !;
}

const WORD: usize = core::mem::size_of::<usize>();

struct LibcAllocator;

// SAFETY: delegates to the userland libc heap. align<=16 uses malloc directly
// (16-byte guaranteed per stdlib.c); larger alignment over-allocates and stores
// the base pointer just before the aligned pointer. Identical to arena_rs.rs.
unsafe impl GlobalAlloc for LibcAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align();
        let size = layout.size();
        if align <= 16 {
            malloc(size)
        } else {
            let total = match size.checked_add(align).and_then(|v| v.checked_add(WORD)) {
                Some(t) => t,
                None => return core::ptr::null_mut(),
            };
            let raw = malloc(total);
            if raw.is_null() {
                return core::ptr::null_mut();
            }
            let raw_addr = raw as usize;
            let aligned = (raw_addr + WORD + align - 1) & !(align - 1);
            *((aligned - WORD) as *mut usize) = raw_addr;
            aligned as *mut u8
        }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.align() <= 16 {
            free(ptr);
        } else {
            let base = *((ptr as usize - WORD) as *const usize);
            free(base as *mut u8);
        }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= 16 {
            realloc(ptr, new_size)
        } else {
            let new_layout = match Layout::from_size_align(new_size, layout.align()) {
                Ok(l) => l,
                Err(_) => return core::ptr::null_mut(),
            };
            let new_ptr = self.alloc(new_layout);
            if !new_ptr.is_null() {
                let copy = if new_size < layout.size() { new_size } else { layout.size() };
                core::ptr::copy_nonoverlapping(ptr, new_ptr, copy);
                self.dealloc(ptr, layout);
            }
            new_ptr
        }
    }
}

#[global_allocator]
static ALLOCATOR: LibcAllocator = LibcAllocator;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // panic=abort: a Rust panic in Ring-3 routes to userland abort() (loud,
    // process-fatal), NEVER a silent busy-spin (#426). In practice the parser
    // is written to never panic on untrusted input; this is the backstop.
    unsafe { abort() }
}

// ---- limits (adversarial-input caps) --------------------------------------
const MAX_NODES: usize = 300_000; // total DOM elements
const MAX_DEPTH: usize = 512; // nesting depth on the open-element stack
const MAX_ITEMS: usize = 2000; // feed items returned to C
const MAX_TEXT: usize = 1 << 20; // 1 MiB cap on any single element's text

// ---- format codes (must match rss_rs.h) -----------------------------------
const FMT_UNKNOWN: i32 = 0;
const FMT_RSS2: i32 = 1;
const FMT_ATOM: i32 = 2;
const FMT_RDF: i32 = 3;

// ---- DOM node --------------------------------------------------------------
struct Node {
    name: Vec<u8>,                    // lowercased LOCAL name (namespace prefix stripped)
    text: Vec<u8>,                    // direct text content (entities decoded, CDATA literal)
    attrs: Vec<(Vec<u8>, Vec<u8>)>,   // (lowercased local attr name, decoded value)
    children: Vec<usize>,
    parent: usize,                    // usize::MAX for the synthetic root
}

impl Node {
    fn new(name: Vec<u8>, parent: usize) -> Node {
        Node { name, text: Vec::new(), attrs: Vec::new(), children: Vec::new(), parent }
    }
}

// ---- byte helpers (all bounds-checked) ------------------------------------
#[inline]
fn lower(b: u8) -> u8 {
    if b >= b'A' && b <= b'Z' { b + 32 } else { b }
}

#[inline]
fn is_ws(b: u8) -> bool {
    b == b' ' || b == b'\t' || b == b'\r' || b == b'\n'
}

// True if `data[i..]` begins with `pat`, bounds-safe.
fn starts_with(data: &[u8], i: usize, pat: &[u8]) -> bool {
    if i > data.len() || pat.len() > data.len() - i {
        return false;
    }
    // slice::get keeps this bounds-safe even if the arithmetic above were wrong.
    match data.get(i..i + pat.len()) {
        Some(s) => s == pat,
        None => false,
    }
}

// Index of the first `b` at/after `from`, or None.
fn find_byte(data: &[u8], from: usize, b: u8) -> Option<usize> {
    let mut i = from;
    while i < data.len() {
        if data[i] == b {
            return Some(i);
        }
        i += 1;
    }
    None
}

// Index of the first occurrence of `pat` at/after `from`, or None.
fn find_seq(data: &[u8], from: usize, pat: &[u8]) -> Option<usize> {
    if pat.is_empty() || pat.len() > data.len() {
        return None;
    }
    let last = data.len() - pat.len();
    let mut i = from;
    while i <= last {
        if data.get(i..i + pat.len()) == Some(pat) {
            return Some(i);
        }
        i += 1;
    }
    None
}

fn bytes_eq(a: &[u8], b: &[u8]) -> bool {
    a == b
}

// Decode XML/HTML entities in `src` into `out` (appending). Handles the five
// predefined entities plus &nbsp; and numeric &#NN; / &#xNN;. Unknown entities
// are passed through literally (a bare '&' stays '&'), never dropped, so the
// output is always a superset-safe rendering. Never allocates unboundedly:
// caps total appended bytes at MAX_TEXT.
fn decode_entities_into(src: &[u8], out: &mut Vec<u8>) {
    let n = src.len();
    let mut i = 0usize;
    while i < n {
        if out.len() >= MAX_TEXT {
            return;
        }
        let c = src[i];
        if c != b'&' {
            out.push(c);
            i += 1;
            continue;
        }
        // Find the terminating ';' within a small window.
        let mut j = i + 1;
        let limit = if n - i > 12 { i + 12 } else { n };
        while j < limit && src[j] != b';' {
            j += 1;
        }
        if j >= n || src[j] != b';' {
            out.push(b'&'); // no terminator: literal ampersand
            i += 1;
            continue;
        }
        let ent = &src[i + 1..j]; // between & and ;
        if bytes_eq(ent, b"amp") {
            out.push(b'&');
        } else if bytes_eq(ent, b"lt") {
            out.push(b'<');
        } else if bytes_eq(ent, b"gt") {
            out.push(b'>');
        } else if bytes_eq(ent, b"quot") {
            out.push(b'"');
        } else if bytes_eq(ent, b"apos") {
            out.push(b'\'');
        } else if bytes_eq(ent, b"nbsp") {
            out.push(b' ');
        } else if !ent.is_empty() && ent[0] == b'#' {
            // numeric character reference
            let mut val: u32 = 0;
            let mut ok = false;
            if ent.len() >= 2 && (ent[1] == b'x' || ent[1] == b'X') {
                for &d in &ent[2..] {
                    let v = match d {
                        b'0'..=b'9' => (d - b'0') as u32,
                        b'a'..=b'f' => (d - b'a' + 10) as u32,
                        b'A'..=b'F' => (d - b'A' + 10) as u32,
                        _ => { ok = false; break; }
                    };
                    val = val.saturating_mul(16).saturating_add(v);
                    ok = true;
                }
            } else {
                for &d in &ent[1..] {
                    if d >= b'0' && d <= b'9' {
                        val = val.saturating_mul(10).saturating_add((d - b'0') as u32);
                        ok = true;
                    } else {
                        ok = false;
                        break;
                    }
                }
            }
            if ok {
                push_codepoint(val, out);
            } else {
                out.push(b'&'); // malformed numeric ref: literal
                i += 1;
                continue;
            }
        } else {
            // unknown named entity: keep it verbatim (&foo;)
            out.push(b'&');
            i += 1;
            continue;
        }
        i = j + 1;
    }
}

// UTF-8 encode a Unicode scalar (skipping NUL and surrogates to keep the C
// string clean and valid).
fn push_codepoint(cp: u32, out: &mut Vec<u8>) {
    if cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF {
        return;
    }
    if cp < 0x80 {
        out.push(cp as u8);
    } else if cp < 0x800 {
        out.push(0xC0 | (cp >> 6) as u8);
        out.push(0x80 | (cp & 0x3F) as u8);
    } else if cp < 0x10000 {
        out.push(0xE0 | (cp >> 12) as u8);
        out.push(0x80 | ((cp >> 6) & 0x3F) as u8);
        out.push(0x80 | (cp & 0x3F) as u8);
    } else {
        out.push(0xF0 | (cp >> 18) as u8);
        out.push(0x80 | ((cp >> 12) & 0x3F) as u8);
        out.push(0x80 | ((cp >> 6) & 0x3F) as u8);
        out.push(0x80 | (cp & 0x3F) as u8);
    }
}

// Reduce a raw tag/attr name to its lowercased local part (strip "prefix:").
fn local_lower(raw: &[u8]) -> Vec<u8> {
    let mut start = 0usize;
    for (k, &b) in raw.iter().enumerate() {
        if b == b':' {
            start = k + 1;
        }
    }
    let mut out = Vec::with_capacity(raw.len() - start);
    for &b in &raw[start..] {
        out.push(lower(b));
    }
    out
}

// Parse a start tag beginning at data[i] (== '<'). Returns
// (local_name, attrs, self_closing, next_index_after_'>'). Fully bounds-safe.
fn parse_start_tag(data: &[u8], i: usize) -> (Vec<u8>, Vec<Vec<u8>>, bool, bool, usize) {
    let n = data.len();
    let mut j = i + 1;
    // element name
    let ns = j;
    while j < n {
        let c = data[j];
        if is_ws(c) || c == b'/' || c == b'>' {
            break;
        }
        j += 1;
    }
    let name = local_lower(&data[ns..j.min(n)]);
    let mut attr_names: Vec<Vec<u8>> = Vec::new();
    let mut attr_vals: Vec<Vec<u8>> = Vec::new();
    let mut self_closing = false;
    loop {
        while j < n && is_ws(data[j]) {
            j += 1;
        }
        if j >= n {
            break;
        }
        let c = data[j];
        if c == b'>' {
            j += 1;
            break;
        }
        if c == b'/' {
            if j + 1 < n && data[j + 1] == b'>' {
                self_closing = true;
                j += 2;
                break;
            }
            j += 1;
            continue;
        }
        // attribute name
        let as_ = j;
        while j < n {
            let d = data[j];
            if is_ws(d) || d == b'=' || d == b'>' || d == b'/' {
                break;
            }
            j += 1;
        }
        let aname = local_lower(&data[as_..j.min(n)]);
        while j < n && is_ws(data[j]) {
            j += 1;
        }
        let mut aval: Vec<u8> = Vec::new();
        if j < n && data[j] == b'=' {
            j += 1;
            while j < n && is_ws(data[j]) {
                j += 1;
            }
            if j < n && (data[j] == b'"' || data[j] == b'\'') {
                let q = data[j];
                j += 1;
                let vs = j;
                while j < n && data[j] != q {
                    j += 1;
                }
                decode_entities_into(&data[vs..j.min(n)], &mut aval);
                if j < n {
                    j += 1; // consume closing quote
                }
            } else {
                let vs = j;
                while j < n {
                    let d = data[j];
                    if is_ws(d) || d == b'>' || d == b'/' {
                        break;
                    }
                    j += 1;
                }
                decode_entities_into(&data[vs..j.min(n)], &mut aval);
            }
        }
        if !aname.is_empty() {
            attr_names.push(aname);
            attr_vals.push(aval);
        }
    }
    // Zip attr names+vals into one Vec by interleaving is awkward for the
    // caller; instead we hand back attr_names and let caller pull vals. To keep
    // one return value we re-pack into a flat Vec<Vec<u8>> [n0,v0,n1,v1,...].
    let mut flat: Vec<Vec<u8>> = Vec::with_capacity(attr_names.len() * 2);
    for k in 0..attr_names.len() {
        flat.push(core::mem::take(&mut attr_names[k]));
        flat.push(core::mem::take(&mut attr_vals[k]));
    }
    (name, flat, self_closing, false, j)
}

// Parse an end tag name beginning at data[i] (== '<'), return (local_name,
// index_after_'>').
fn parse_end_tag(data: &[u8], i: usize) -> (Vec<u8>, usize) {
    let n = data.len();
    let mut j = i + 2; // skip "</"
    let ns = j;
    while j < n {
        let c = data[j];
        if is_ws(c) || c == b'>' {
            break;
        }
        j += 1;
    }
    let name = local_lower(&data[ns..j.min(n)]);
    let end = find_byte(data, j, b'>').map(|p| p + 1).unwrap_or(n);
    (name, end)
}

// Skip a "<!...>"-style declaration (DOCTYPE etc.), honoring a bracketed
// internal subset "[ ... ]" so a '>' inside it does not terminate early.
fn skip_decl(data: &[u8], i: usize) -> usize {
    let n = data.len();
    let mut j = i + 2; // skip "<!"
    let mut depth = 0i32;
    while j < n {
        match data[j] {
            b'[' => depth += 1,
            b']' => {
                if depth > 0 {
                    depth -= 1;
                }
            }
            b'>' if depth == 0 => return j + 1,
            _ => {}
        }
        j += 1;
    }
    n
}

// Build the DOM iteratively (explicit open-element stack, no recursion, so a
// deeply nested hostile document cannot blow the Rust stack).
fn build_dom(data: &[u8]) -> Vec<Node> {
    let mut nodes: Vec<Node> = Vec::new();
    nodes.push(Node::new(b"#root".to_vec(), usize::MAX)); // synthetic root at 0
    let mut stack: Vec<usize> = Vec::new();
    stack.push(0);
    let n = data.len();
    let mut i = 0usize;
    while i < n {
        if nodes.len() >= MAX_NODES {
            break;
        }
        let c = data[i];
        if c == b'<' {
            if starts_with(data, i, b"<!--") {
                i = find_seq(data, i + 4, b"-->").map(|p| p + 3).unwrap_or(n);
            } else if starts_with(data, i, b"<![CDATA[") {
                let end = find_seq(data, i + 9, b"]]>").unwrap_or(n);
                let seg = &data[(i + 9).min(n)..end.min(n)];
                let cur = *stack.last().unwrap_or(&0);
                if nodes[cur].text.len() + seg.len() <= MAX_TEXT {
                    nodes[cur].text.extend_from_slice(seg);
                }
                i = if end < n { end + 3 } else { n };
            } else if starts_with(data, i, b"<!") {
                i = skip_decl(data, i);
            } else if starts_with(data, i, b"<?") {
                i = find_seq(data, i + 2, b"?>").map(|p| p + 2).unwrap_or(n);
            } else if i + 1 < n && data[i + 1] == b'/' {
                // end tag: pop to the matching open element (tolerant).
                let (name, next) = parse_end_tag(data, i);
                let mut hit = None;
                for k in (0..stack.len()).rev() {
                    if bytes_eq(&nodes[stack[k]].name, &name) {
                        hit = Some(k);
                        break;
                    }
                }
                if let Some(k) = hit {
                    if k >= 1 {
                        stack.truncate(k); // pop through the matched element
                    }
                }
                i = next;
            } else if i + 1 < n
                && (data[i + 1].is_ascii_alphabetic() || data[i + 1] == b'_' || data[i + 1] == b':')
            {
                // start tag
                let (name, flat_attrs, self_closing, _err, next) = parse_start_tag(data, i);
                let parent = *stack.last().unwrap_or(&0);
                let idx = nodes.len();
                let mut node = Node::new(name, parent);
                let mut a = 0usize;
                while a + 1 < flat_attrs.len() {
                    node.attrs.push((flat_attrs[a].clone(), flat_attrs[a + 1].clone()));
                    a += 2;
                }
                nodes.push(node);
                if parent < nodes.len() {
                    nodes[parent].children.push(idx);
                }
                if !self_closing && stack.len() < MAX_DEPTH {
                    stack.push(idx);
                }
                i = if next > i { next } else { i + 1 };
            } else {
                // stray '<' (e.g. in text): treat as literal text char.
                let cur = *stack.last().unwrap_or(&0);
                if nodes[cur].text.len() < MAX_TEXT {
                    nodes[cur].text.push(b'<');
                }
                i += 1;
            }
        } else {
            // text run up to the next '<'
            let end = find_byte(data, i, b'<').unwrap_or(n);
            let cur = *stack.last().unwrap_or(&0);
            if cur != 0 {
                decode_entities_into(&data[i..end.min(n)], &mut nodes[cur].text);
            }
            i = if end > i { end } else { i + 1 };
        }
    }
    nodes
}

// ---- extraction helpers ----------------------------------------------------
fn attr<'a>(node: &'a Node, key: &[u8]) -> Option<&'a [u8]> {
    for (k, v) in &node.attrs {
        if bytes_eq(k, key) {
            return Some(&v[..]);
        }
    }
    None
}

// Trim leading/trailing ASCII whitespace and collapse internal runs into one
// space, matching the reader's display expectation. Returns owned bytes.
fn tidy(src: &[u8]) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::with_capacity(src.len());
    let mut last_sp = true;
    for &b in src {
        if is_ws(b) {
            if !last_sp {
                out.push(b' ');
                last_sp = true;
            }
        } else {
            out.push(b);
            last_sp = false;
        }
    }
    while let Some(&b' ') = out.last() {
        out.pop();
    }
    out
}

// Direct text of the first child (of `idx`) whose local name is in `names`.
fn child_text(nodes: &[Node], idx: usize, names: &[&[u8]]) -> Vec<u8> {
    for name in names {
        for &c in &nodes[idx].children {
            if bytes_eq(&nodes[c].name, name) && !nodes[c].text.is_empty() {
                return tidy(&nodes[c].text);
            }
        }
    }
    Vec::new()
}

// Extract the best link URL for element `idx`:
//   RSS/RDF: <link>url</link>   (text)
//   Atom:    <link href="url" rel="alternate"/>  (attribute; prefer alternate)
fn get_link(nodes: &[Node], idx: usize) -> Vec<u8> {
    let mut text_link: Vec<u8> = Vec::new();
    let mut alt_href: Vec<u8> = Vec::new();
    let mut any_href: Vec<u8> = Vec::new();
    for &c in &nodes[idx].children {
        if !bytes_eq(&nodes[c].name, b"link") {
            continue;
        }
        if !nodes[c].text.is_empty() && text_link.is_empty() {
            text_link = tidy(&nodes[c].text);
        }
        if let Some(h) = attr(&nodes[c], b"href") {
            if any_href.is_empty() {
                any_href = h.to_vec();
            }
            let rel = attr(&nodes[c], b"rel");
            let is_alt = match rel {
                None => true,
                Some(r) => bytes_eq(r, b"alternate"),
            };
            if is_alt && alt_href.is_empty() {
                alt_href = h.to_vec();
            }
        }
    }
    if !text_link.is_empty() {
        return text_link;
    }
    if !alt_href.is_empty() {
        return alt_href;
    }
    any_href
}

// Extract an author string: <author> text, or <author><name> (Atom), or
// <dc:creator> (-> local "creator").
fn get_author(nodes: &[Node], idx: usize) -> Vec<u8> {
    for &c in &nodes[idx].children {
        if bytes_eq(&nodes[c].name, b"author") {
            if !nodes[c].text.is_empty() {
                return tidy(&nodes[c].text);
            }
            // Atom: <author><name>...</name></author>
            for &g in &nodes[c].children {
                if bytes_eq(&nodes[g].name, b"name") && !nodes[g].text.is_empty() {
                    return tidy(&nodes[g].text);
                }
            }
        }
    }
    child_text(nodes, idx, &[b"creator"]) // dc:creator
}

// ---- image extraction ------------------------------------------------------
// Feeds carry an item's picture in one of three places, and all three are in
// live use: an <img> inside the HTML body (xkcd, where the comic IS the
// article, and most blogs), Media RSS (<media:content>/<media:thumbnail>, used
// by BBC and Reddit), or an RSS 2.0 <enclosure type="image/...">. We resolve
// them here, in the parser, because the reader's clean_text() deletes markup
// before anything downstream could see an <img>, and because the DOM (which
// carries media:*/enclosure attributes) is freed before C ever runs.

// Element names whose text may carry an HTML body. Mirrors the description
// lookup in rss_parse, so the image we find is the one in the text we show.
const DESC_NAMES: [&[u8]; 4] = [b"description", b"encoded", b"summary", b"content"];

fn eq_ci(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for k in 0..a.len() {
        if lower(a[k]) != lower(b[k]) {
            return false;
        }
    }
    true
}

fn starts_with_ci(a: &[u8], pat: &[u8]) -> bool {
    if a.len() < pat.len() {
        return false;
    }
    for k in 0..pat.len() {
        if lower(a[k]) != lower(pat[k]) {
            return false;
        }
    }
    true
}

// Look up a value in the flat [name0, val0, name1, val1, ...] vec that
// parse_start_tag hands back. Attribute names arrive already lowercased.
fn flat_attr<'a>(flat: &'a [Vec<u8>], key: &[u8]) -> Option<&'a [u8]> {
    let mut a = 0usize;
    while a + 1 < flat.len() {
        if bytes_eq(&flat[a], key) {
            return Some(&flat[a + 1][..]);
        }
        a += 2;
    }
    None
}

// Leading unsigned decimal, saturating. None if there is no digit at all.
fn parse_uint(v: &[u8]) -> Option<u32> {
    let mut out: u32 = 0;
    let mut any = false;
    for &b in v {
        if b.is_ascii_digit() {
            out = out.saturating_mul(10).saturating_add((b - b'0') as u32);
            any = true;
        } else if any {
            break;
        } else if !is_ws(b) {
            return None;
        }
    }
    if any {
        Some(out)
    } else {
        None
    }
}

// A declared width or height of a pixel or two is an analytics beacon, not a
// picture. Feedburner and WordPress feeds put one in nearly every item, and it
// is usually the FIRST img, so taking img[0] blindly would show a 1x1 dot
// instead of the article's image.
fn is_tracking_pixel(flat: &[Vec<u8>]) -> bool {
    for key in [b"width".as_ref(), b"height".as_ref()] {
        if let Some(v) = flat_attr(flat, key) {
            if let Some(px) = parse_uint(v) {
                if px <= 2 {
                    return true;
                }
            }
        }
    }
    false
}

// True if the URL's PATH ends in an image extension. Query strings routinely
// contain dots, so they are cut off first.
fn url_looks_like_image(u: &[u8]) -> bool {
    let mut end = u.len();
    for (k, &b) in u.iter().enumerate() {
        if b == b'?' || b == b'#' {
            end = k;
            break;
        }
    }
    let path = &u[..end];
    for ext in [
        b".png".as_ref(),
        b".jpg".as_ref(),
        b".jpeg".as_ref(),
        b".bmp".as_ref(),
        b".gif".as_ref(),
        b".webp".as_ref(),
    ] {
        if path.len() >= ext.len() && eq_ci(&path[path.len() - ext.len()..], ext) {
            return true;
        }
    }
    false
}

// First usable <img src> in an HTML fragment, with its alt and title text.
// Reuses parse_start_tag, the same bounds-safe scanner the DOM builder uses, so
// hostile markup cannot read out of bounds here either.
fn find_img_in_html(html: &[u8]) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
    let n = html.len();
    let mut i = 0usize;
    while i < n {
        if html[i] != b'<' || i + 1 >= n || !html[i + 1].is_ascii_alphabetic() {
            i += 1;
            continue;
        }
        let (name, flat, _self_closing, _err, next) = parse_start_tag(html, i);
        if bytes_eq(&name, b"img") {
            if let Some(src) = flat_attr(&flat, b"src") {
                if !src.is_empty() && !is_tracking_pixel(&flat) {
                    let src = src.to_vec();
                    let alt = flat_attr(&flat, b"alt").unwrap_or(&[]).to_vec();
                    let title = flat_attr(&flat, b"title").unwrap_or(&[]).to_vec();
                    return (src, alt, title);
                }
            }
        }
        i = if next > i { next } else { i + 1 };
    }
    (Vec::new(), Vec::new(), Vec::new())
}

// Is this <media:content> an image? medium/type say so when present; otherwise
// fall back to the URL's extension, because a media:content with neither is
// just as likely to be a video we cannot show.
fn media_is_image(nd: &Node, url: &[u8]) -> bool {
    if let Some(m) = attr(nd, b"medium") {
        return eq_ci(m, b"image");
    }
    if let Some(t) = attr(nd, b"type") {
        return starts_with_ci(t, b"image/");
    }
    url_looks_like_image(url)
}

// Best image for item `idx`, as (url, alt, title). The URL may be relative; the
// caller resolves it against the feed URL and enforces the scheme.
//
// Priority: the body <img> first (it is the article's own picture), then
// media:content (usually full size), then media:thumbnail (small by
// definition), then an image enclosure.
fn get_item_image(nodes: &[Node], idx: usize) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
    for name in DESC_NAMES.iter() {
        for &c in &nodes[idx].children {
            if bytes_eq(&nodes[c].name, name) && !nodes[c].text.is_empty() {
                let (src, alt, title) = find_img_in_html(&nodes[c].text);
                if !src.is_empty() {
                    return (src, alt, title);
                }
            }
        }
    }

    // local_lower() strips namespace prefixes, so <media:content> arrives here
    // as "content", the same local name as Atom's <content>. The url attribute
    // is what tells them apart: Atom's carries text, Media RSS carries a url.
    let mut thumb: Vec<u8> = Vec::new();
    for &c in &nodes[idx].children {
        let nd = &nodes[c];
        if bytes_eq(&nd.name, b"content") {
            if let Some(u) = attr(nd, b"url") {
                if !u.is_empty() && media_is_image(nd, u) {
                    return (u.to_vec(), Vec::new(), Vec::new());
                }
            }
        }
        if bytes_eq(&nd.name, b"thumbnail") && thumb.is_empty() {
            if let Some(u) = attr(nd, b"url") {
                if !u.is_empty() {
                    thumb = u.to_vec(); // media:thumbnail is an image by definition
                }
            }
        }
    }
    if !thumb.is_empty() {
        return (thumb, Vec::new(), Vec::new());
    }

    for &c in &nodes[idx].children {
        let nd = &nodes[c];
        if !bytes_eq(&nd.name, b"enclosure") {
            continue;
        }
        if let Some(u) = attr(nd, b"url") {
            if u.is_empty() {
                continue;
            }
            let is_img = match attr(nd, b"type") {
                Some(t) => starts_with_ci(t, b"image/"),
                None => url_looks_like_image(u),
            };
            if is_img {
                return (u.to_vec(), Vec::new(), Vec::new());
            }
        }
    }

    (Vec::new(), Vec::new(), Vec::new())
}

// Drop images that are feed chrome rather than the article's own picture.
//
// An image that shows up in most of a feed's items is boilerplate by
// definition: Slashdot puts the same twitter/facebook icons in every item's
// description, so taking img[0] blindly would badge every story with a Twitter
// logo. Frequency is the honest signal here; a URL blocklist would be a
// guessing game we would lose.
//
// Only runs for plausible item counts: MAX_ITEMS is a hostile-input ceiling
// (2000), and this is O(n^2) in byte compares, so it is not worth doing there.
const BOILERPLATE_MAX_ITEMS: usize = 256;

fn drop_boilerplate_images(imgs: &mut [(Vec<u8>, Vec<u8>, Vec<u8>)]) {
    let n = imgs.len();
    if n < 4 || n > BOILERPLATE_MAX_ITEMS {
        return; // too few items for "most of them" to mean anything
    }
    let mut drop_list: Vec<Vec<u8>> = Vec::new();
    for i in 0..n {
        if imgs[i].0.is_empty() {
            continue;
        }
        let mut seen = false;
        for d in &drop_list {
            if bytes_eq(d, &imgs[i].0) {
                seen = true;
                break;
            }
        }
        if seen {
            continue;
        }
        let mut count = 0usize;
        for j in 0..n {
            if bytes_eq(&imgs[j].0, &imgs[i].0) {
                count += 1;
            }
        }
        if count * 2 > n {
            drop_list.push(imgs[i].0.clone());
        }
    }
    for e in imgs.iter_mut() {
        for d in &drop_list {
            if bytes_eq(d, &e.0) {
                e.0.clear();
                e.1.clear();
                e.2.clear();
                break;
            }
        }
    }
}

// Extract an enclosure URL: <enclosure url="..."> or <link rel="enclosure" href>.
fn get_enclosure(nodes: &[Node], idx: usize) -> Vec<u8> {
    for &c in &nodes[idx].children {
        if bytes_eq(&nodes[c].name, b"enclosure") {
            if let Some(u) = attr(&nodes[c], b"url") {
                return u.to_vec();
            }
        }
        if bytes_eq(&nodes[c].name, b"link") {
            if let Some(r) = attr(&nodes[c], b"rel") {
                if bytes_eq(r, b"enclosure") {
                    if let Some(h) = attr(&nodes[c], b"href") {
                        return h.to_vec();
                    }
                }
            }
        }
    }
    Vec::new()
}

// ---- FFI structs (must match rss_rs.h exactly) ----------------------------
#[repr(C)]
pub struct CItem {
    title: *mut u8,
    link: *mut u8,
    description: *mut u8,
    pub_date: *mut u8,
    author: *mut u8,
    enclosure: *mut u8,
    guid: *mut u8,
    // Inline image (may be a relative URL; the caller resolves and scheme-checks
    // it). image_url is "" when the item has no picture.
    image_url: *mut u8,
    image_alt: *mut u8,
    image_title: *mut u8,
}

#[repr(C)]
pub struct CFeed {
    format: i32,
    error: i32,
    title: *mut u8,
    link: *mut u8,
    description: *mut u8,
    items: *mut CItem,
    item_count: i32,
}

// Allocate a NUL-terminated C string copy of `v` on the shared heap.
unsafe fn cstr(v: &[u8]) -> *mut u8 {
    let p = malloc(v.len() + 1);
    if p.is_null() {
        return p;
    }
    if !v.is_empty() {
        core::ptr::copy_nonoverlapping(v.as_ptr(), p, v.len());
    }
    *p.add(v.len()) = 0;
    p
}

unsafe fn empty_feed(format: i32, error: i32) -> *mut CFeed {
    let f = malloc(core::mem::size_of::<CFeed>()) as *mut CFeed;
    if f.is_null() {
        return f;
    }
    core::ptr::write(
        f,
        CFeed {
            format,
            error,
            title: cstr(b""),
            link: cstr(b""),
            description: cstr(b""),
            items: core::ptr::null_mut(),
            item_count: 0,
        },
    );
    f
}

/// Parse untrusted feed bytes into a heap-allocated CFeed. `data`/`len` describe
/// the raw HTTP body. Returns NULL only on allocation failure; otherwise a valid
/// CFeed whose `error` is nonzero if nothing parseable was found (partial data
/// may still be present). NEVER panics or reads out of bounds on any input.
///
/// # Safety
/// `data` must point to `len` readable bytes (or be null with len 0). The
/// returned pointer must be released with rss_free().
#[no_mangle]
pub unsafe extern "C" fn rss_parse(data: *const u8, len: usize) -> *mut CFeed {
    if data.is_null() || len == 0 {
        return empty_feed(FMT_UNKNOWN, 1);
    }
    let slice = core::slice::from_raw_parts(data, len);
    let nodes = build_dom(slice);

    // Detect the format from the first recognised root-ish element.
    let mut format = FMT_UNKNOWN;
    for nd in &nodes {
        if bytes_eq(&nd.name, b"rss") {
            format = FMT_RSS2;
            break;
        } else if bytes_eq(&nd.name, b"feed") {
            format = FMT_ATOM;
            break;
        } else if bytes_eq(&nd.name, b"rdf") {
            format = FMT_RDF;
            break;
        }
    }

    // Channel/meta element: <channel> (RSS2/RDF) or the <feed> root (Atom).
    let mut chan: Option<usize> = None;
    for (k, nd) in nodes.iter().enumerate() {
        if bytes_eq(&nd.name, b"channel") {
            chan = Some(k);
            break;
        }
    }
    if chan.is_none() {
        for (k, nd) in nodes.iter().enumerate() {
            if bytes_eq(&nd.name, b"feed") {
                chan = Some(k);
                break;
            }
        }
    }

    let (mut ftitle, mut flink, mut fdesc) = (Vec::new(), Vec::new(), Vec::new());
    if let Some(ci) = chan {
        ftitle = child_text(&nodes, ci, &[b"title"]);
        flink = get_link(&nodes, ci);
        fdesc = child_text(&nodes, ci, &[b"description", b"subtitle", b"tagline"]);
    }

    // Collect items: <item>/<entry> whose parent is a channel/feed/rdf/rss.
    // Item images are gathered alongside, index-aligned with `items`, so the
    // feed-chrome filter can see all of them at once before they are committed
    // to the C strings.
    let mut items: Vec<CItem> = Vec::new();
    let mut imgs: Vec<(Vec<u8>, Vec<u8>, Vec<u8>)> = Vec::new();
    for (k, nd) in nodes.iter().enumerate() {
        if items.len() >= MAX_ITEMS {
            break;
        }
        let is_item = bytes_eq(&nd.name, b"item") || bytes_eq(&nd.name, b"entry");
        if !is_item {
            continue;
        }
        if nd.parent == usize::MAX || nd.parent >= nodes.len() {
            continue;
        }
        let pn = &nodes[nd.parent].name;
        let ok_parent = bytes_eq(pn, b"channel")
            || bytes_eq(pn, b"feed")
            || bytes_eq(pn, b"rdf")
            || bytes_eq(pn, b"rss")
            || bytes_eq(pn, b"#root");
        if !ok_parent {
            continue;
        }

        let title = child_text(&nodes, k, &[b"title"]);
        let link = get_link(&nodes, k);
        let desc = child_text(
            &nodes,
            k,
            &[b"description", b"encoded", b"summary", b"content"],
        );
        let date = child_text(
            &nodes,
            k,
            &[b"pubdate", b"date", b"updated", b"published", b"issued"],
        );
        let author = get_author(&nodes, k);
        let enclosure = get_enclosure(&nodes, k);
        let guid = child_text(&nodes, k, &[b"guid", b"id"]);
        imgs.push(get_item_image(&nodes, k));

        items.push(CItem {
            title: cstr(&title),
            link: cstr(&link),
            description: cstr(&desc),
            pub_date: cstr(&date),
            author: cstr(&author),
            enclosure: cstr(&enclosure),
            guid: cstr(&guid),
            // filled in below, once every item's image is known
            image_url: core::ptr::null_mut(),
            image_alt: core::ptr::null_mut(),
            image_title: core::ptr::null_mut(),
        });
    }

    // Now that every item's image is known, drop the ones that are feed chrome,
    // then commit the survivors. Done before any early return below, so the
    // image fields are never left null for a caller (or for the free paths).
    drop_boilerplate_images(&mut imgs);
    for (k, im) in imgs.iter().enumerate() {
        items[k].image_url = cstr(&im.0);
        items[k].image_alt = cstr(&im.1);
        items[k].image_title = cstr(&im.2);
    }

    let count = items.len();
    let items_ptr = if count == 0 {
        core::ptr::null_mut()
    } else {
        let arr = malloc(count * core::mem::size_of::<CItem>()) as *mut CItem;
        if arr.is_null() {
            // free the strings we already built, then bail with an error feed.
            for it in &items {
                free(it.title);
                free(it.link);
                free(it.description);
                free(it.pub_date);
                free(it.author);
                free(it.enclosure);
                free(it.guid);
                free(it.image_url);
                free(it.image_alt);
                free(it.image_title);
            }
            return empty_feed(format, 2);
        }
        for (k, it) in items.iter().enumerate() {
            core::ptr::write(arr.add(k), CItem_copy(it));
        }
        arr
    };

    let f = malloc(core::mem::size_of::<CFeed>()) as *mut CFeed;
    if f.is_null() {
        return core::ptr::null_mut();
    }
    let error = if count == 0 && ftitle.is_empty() { 3 } else { 0 };
    core::ptr::write(
        f,
        CFeed {
            format,
            error,
            title: cstr(&ftitle),
            link: cstr(&flink),
            description: cstr(&fdesc),
            items: items_ptr,
            item_count: count as i32,
        },
    );
    f
}

// Shallow copy of a CItem's raw pointers (ownership moves to the array slot).
#[allow(non_snake_case)]
fn CItem_copy(it: &CItem) -> CItem {
    CItem {
        title: it.title,
        link: it.link,
        description: it.description,
        pub_date: it.pub_date,
        author: it.author,
        enclosure: it.enclosure,
        guid: it.guid,
        image_url: it.image_url,
        image_alt: it.image_alt,
        image_title: it.image_title,
    }
}

/// Free a CFeed previously returned by rss_parse (all strings + the item array).
///
/// # Safety
/// `feed` must be null or a pointer returned by rss_parse and not freed before.
#[no_mangle]
pub unsafe extern "C" fn rss_free(feed: *mut CFeed) {
    if feed.is_null() {
        return;
    }
    let f = &*feed;
    if !f.items.is_null() && f.item_count > 0 {
        for k in 0..f.item_count as isize {
            let it = &*f.items.offset(k);
            free_if(it.title);
            free_if(it.link);
            free_if(it.description);
            free_if(it.pub_date);
            free_if(it.author);
            free_if(it.enclosure);
            free_if(it.guid);
            free_if(it.image_url);
            free_if(it.image_alt);
            free_if(it.image_title);
        }
        free(f.items as *mut u8);
    }
    free_if(f.title);
    free_if(f.link);
    free_if(f.description);
    free(feed as *mut u8);
}

unsafe fn free_if(p: *mut u8) {
    if !p.is_null() {
        free(p);
    }
}

/// size_of::<CItem>() as Rust sees it. The C header declares this struct
/// independently, so a field added on one side only would silently misalign
/// every string pointer. main.c checks this against sizeof(rss_item_t) once at
/// startup: the FFI sizeof-lock the kernel does with _Static_assert, done at
/// runtime because the two definitions live in different compilers.
#[no_mangle]
pub extern "C" fn rss_abi_item_size() -> u32 {
    core::mem::size_of::<CItem>() as u32
}

/// Human-readable format name (static, never freed).
#[no_mangle]
pub extern "C" fn rss_format_name(fmt: i32) -> *const u8 {
    match fmt {
        FMT_RSS2 => b"RSS 2.0\0".as_ptr(),
        FMT_ATOM => b"Atom 1.0\0".as_ptr(),
        FMT_RDF => b"RSS 1.0/RDF\0".as_ptr(),
        _ => b"Unknown\0".as_ptr(),
    }
}
