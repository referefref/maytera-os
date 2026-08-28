// startmenu_model.rs - MayteraOS USERLAND (Ring-3) Rust Start-menu content
// model: config parsing, two-layer additive merge, and existence-gated
// emission. This is the "hardcoded menu should never exist" fix.
//
// BEFORE THIS FILE, the entire default Start menu (58 add_item()/
// add_item_typed() calls across 5 categories) was compiled into
// startmenu.c's startmenu_init(), plus three more ad-hoc, mutually
// inconsistent config readers layered on top (/GAMES.CFG,
// /CONFIG/STARTMNU.YML, and a broken App Store registration pipe that wrote
// /APPS/REGINI.CFG to a file nothing read - desktop_menu_reload() in
// kernel/gui/desktop.c is a documented no-op). The directive: no hardcoded
// entry, ever, not even as a fallback when config is missing or unreadable;
// the menu content model rewritten in Rust; exactly two additive config
// layers (all-users "system", per-user "user"), each of which an app can
// gain an entry in at build time (a repo-tracked fragment shipped by
// build/build-golden.sh) or at install time (the App Store / auto-updater
// writing a fragment straight into the live filesystem).
//
// THE BOUNDARY: directory enumeration and file I/O stay in C (startmenu.c),
// which already has proven opendir/readdir/sys_open/access plumbing
// (userland/libc/dirent.c, posixextra.c) - reusing it rather than
// reimplementing POSIX-y I/O in Rust. This file owns EVERYTHING about what
// the config TEXT means: parsing, item/category identity, the two-layer
// additive merge with last-write-wins override semantics, hide/rename, and
// the missing-target-binary decision (skip it; see sm_finish() below). C
// feeds this file whole fragment files as byte buffers, in the two-layer
// order the directive requires (all system-layer fragments, filename-sorted,
// then all user-layer fragments, filename-sorted); this file calls back into
// C only to check whether an exec path exists and to populate the existing
// g_categories/g_menu_items arrays (add_category()/add_item_typed()) that
// the rest of startmenu.c (rendering, search, hit-testing, flyouts) already
// depends on - none of that surrounding ~1900 lines needed to change.
//
// FRAGMENT GRAMMAR (one directive per line; '#' and blank lines ignored):
//   category: <Label> [| expanded] [| id=<catid>]
//   item: <Name> | <exec path or @sentinel> [| id=<itemid>] [| icon=<name>]
//         [| type=native|win16|dos] [| mode=auto|real|pm]
//   rename: <id> | <New Name>
//   hide: <id>
//
// (#845) `mode=` is only meaningful when `type=win16`: "auto" (the default,
// same as omitting the field) lets the kernel derive real-vs-protected from
// the NE header at load time (ne.c's win16_decide_pmode()); "real"/"pm" force
// it explicitly, an explicit override always winning over the derived
// answer. This is the SAME per-app config both Win16 launch paths (Start
// menu and the /CONFIG/WIN16PM.RUN boot autolauncher) resolve through, so a
// fragment's choice here is what a menu click AND the autolauncher agree on.
//
// IDENTITY: an item/category's id is its explicit `id=` field, else a slug of
// its path (item) or label (category) - lowercase, non [a-z0-9] runs folded
// to '-'. Identity is what lets a later directive (typically in the user
// layer, or a later-sorted fragment in either layer) ADAPT an earlier one
// instead of duplicating it.
//
// MERGE SEMANTICS (the precedence contract this file implements):
//   - Directives are processed in one global order: every system-layer
//     fragment (filename-sorted), then every user-layer fragment
//     (filename-sorted). Within a fragment, top to bottom.
//   - `category:`/`item:` on an id already seen REPLACES that record's
//     fields wholesale (full overwrite, not a per-field patch - the whole
//     line must be complete even when overriding); POSITION (where it
//     appears in the final menu) stays at the record's FIRST occurrence, so
//     a later override never reshuffles the menu. This is deliberately the
//     one rule that governs every collision, system-vs-system,
//     user-vs-system, or user-vs-user: whoever is processed LAST wins the
//     VALUES, whoever came FIRST wins the POSITION.
//   - `rename:` is sugar for "override just the name field" without having
//     to repeat the exec path - the common single-field adaptation.
//   - `hide:` tombstones an id (last-write-wins alongside item:/rename:, so a
//     later `item:` for the same id un-hides it). A hidden item never
//     renders, regardless of which layer hid it.
//   - `item:` requires a `category:` already opened EARLIER IN THE SAME
//     FRAGMENT FILE (state resets at each fragment's start, never carries
//     over from a previous file) - matching the pre-existing
//     "item before any category: skip" rule this replaces, generalized to
//     every fragment rather than one hand-rolled loader per format. This is
//     why every App Store install fragment is two lines
//     ("category: Installed" then one "item:"), not one.
//   - A category with zero surviving items after hide + the existence check
//     below is not emitted (no empty collapsible headers).
//
// MISSING TARGET DECISION: after the merge, every surviving item whose path
// does not start with '@' (a pseudo-action sentinel, e.g. "@RECYCLE") is
// existence-checked (sm_c_path_exists(), which wraps access()); an item whose
// target does not exist on THIS disk is silently dropped, not shown broken
// and not shown grayed-out. This was a real, previously-unfixed bug class
// (see startmenu.c's own #537 comment on the old hardcoded DOOM path: a
// hardcoded entry pointed at a binary that had moved, and launched nothing
// for an unknown length of time because no one was checking). Data-driven
// entries would have silently reproduced the same failure mode without this
// check.
//
// NO FALLBACK, EVER: if both layers are empty, missing, or fail to parse,
// sm_finish() emits zero categories and zero items. That is not a bug to
// guard against; it is the entire point of the directive. There is no
// compiled-in default anywhere in this file.

#![no_std]

extern crate alloc;

// #220 instance 1: THE splitter for "which part of a launch line is the
// binary". include!()d rather than duplicated so the compositor and
// tools/launchline/conformance.sh's host harness compile the same source text.
// See launchline.rs for the whole story; the short version is that the
// existence check below used to hand the WHOLE field to access(), so any entry
// with an argument (the Stunts entry, since #172) was stat()ing a string that
// is not a file and was silently dropped.
include!("launchline.rs");

use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::panic::PanicInfo;

// ---- userland libc FFI (ONE shared heap, same allocator shim as
// rss_rs.rs / arena_rs.rs) -------------------------------------------------
extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
    fn realloc(ptr: *mut u8, size: usize) -> *mut u8;
    fn abort() -> !;
}

const WORD: usize = core::mem::size_of::<usize>();

struct LibcAllocator;

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
            // Matches rss_rs.rs/arena_rs.rs: over-aligned reallocation is rare
            // for this workload (fragment text and short id/name strings), so
            // fall back to alloc+copy+dealloc rather than special-casing it.
            let new_layout = Layout::from_size_align_unchecked(new_size, layout.align());
            let newp = self.alloc(new_layout);
            if !newp.is_null() {
                let oldsz = layout.size();
                let copy = if oldsz < new_size { oldsz } else { new_size };
                core::ptr::copy_nonoverlapping(ptr, newp, copy);
                self.dealloc(ptr, layout);
            }
            newp
        }
    }
}

#[global_allocator]
static ALLOCATOR: LibcAllocator = LibcAllocator;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe { abort() }
}

// ---- C callbacks this file drives -----------------------------------------
extern "C" {
    // Returns 1 if `path` exists on disk (or is a '@'-prefixed pseudo-action
    // sentinel, which is exempt), 0 otherwise. Never blocks longer than one
    // access() syscall (#426).
    fn sm_c_path_exists(path_ptr: *const u8, path_len: u32) -> i32;
    // #220 instance 3: report an item the existence check is about to DROP, so
    // a configured-and-shipped-but-unreachable entry stops being invisible.
    // Every drop here used to be silent, which is why the Stunts entry could be
    // missing from the Start menu for the whole life of #172 with nothing
    // anywhere saying a word about it. C owns the rate-limiting (the rebuild
    // runs about once a second); this side just reports the fact.
    fn sm_c_note_drop(name_ptr: *const u8, name_len: u32, bin_ptr: *const u8, bin_len: u32);
    // Emits one category in final display order, then (via sm_c_add_item)
    // every surviving item that belongs to it, before the next
    // sm_c_add_category call. C owns slot assignment/bounds (MAX_CATEGORIES);
    // this file never assumes a category index.
    fn sm_c_add_category(label_ptr: *const u8, label_len: u32, expanded: i32);
    // icon_ptr/icon_len may be empty (C substitutes its generic default).
    // launch_type: 0=native, 1=win16, 2=dos - mirrors startmenu.c's
    // LAUNCH_NATIVE/LAUNCH_WIN16/LAUNCH_DOS #defines exactly; if those values
    // are ever renumbered, sm_type_from_str() below must move with them.
    fn sm_c_add_item(
        name_ptr: *const u8,
        name_len: u32,
        path_ptr: *const u8,
        path_len: u32,
        icon_ptr: *const u8,
        icon_len: u32,
        launch_type: i32,
        win16_mode: i32,
    );
}

// ---- Merge state ------------------------------------------------------

struct CatRec {
    id: String,
    label: String,
    expanded: bool,
}

struct ItemRec {
    id: String,
    cat_id: String,
    name: String,
    path: String,
    icon: String,
    launch_type: i32,
    win16_mode: i32,   // (#845) -1=auto, 0=force real, 1=force protected
    hidden: bool,
}

struct Model {
    cats: Vec<CatRec>,
    items: Vec<ItemRec>,
}

static mut MODEL: Option<Model> = None;

// Accessed only through a raw pointer (never `&MODEL`/`&mut MODEL` directly),
// per the 2024-edition static-mut-references lint: this crate has exactly one
// call path into this file (the compositor's single draw/poll thread calling
// sm_model_reset() -> sm_model_add_fragment() -> sm_model_finish() in
// sequence, never concurrently), so the actual aliasing this lint guards
// against cannot happen; the raw-pointer form just stops the compiler from
// having to take that on faith.
fn model() -> &'static mut Model {
    unsafe {
        let ptr = core::ptr::addr_of_mut!(MODEL);
        if (*ptr).is_none() {
            *ptr = Some(Model { cats: Vec::new(), items: Vec::new() });
        }
        (*ptr).as_mut().unwrap()
    }
}

fn slugify(s: &str) -> String {
    let mut out = String::new();
    let mut last_dash = true; // suppress a leading '-'
    for ch in s.chars() {
        let lower = ch.to_ascii_lowercase();
        if lower.is_ascii_alphanumeric() {
            out.push(lower);
            last_dash = false;
        } else if !last_dash {
            out.push('-');
            last_dash = true;
        }
    }
    while out.ends_with('-') {
        out.pop();
    }
    if out.is_empty() {
        out.push_str("item");
    }
    out
}

fn find_cat_mut<'a>(cats: &'a mut Vec<CatRec>, id: &str) -> Option<&'a mut CatRec> {
    cats.iter_mut().find(|c| c.id == id)
}

fn find_item_mut<'a>(items: &'a mut Vec<ItemRec>, id: &str) -> Option<&'a mut ItemRec> {
    items.iter_mut().find(|i| i.id == id)
}

// One `key=value` (or bare `key`) field split on '='.
fn split_field(field: &str) -> (&str, &str) {
    match field.find('=') {
        Some(idx) => (field[..idx].trim(), field[idx + 1..].trim()),
        None => (field.trim(), ""),
    }
}

fn sm_type_from_str(s: &str) -> i32 {
    match s {
        "win16" => 1,
        "dos" => 2,
        _ => 0, // "native" or unspecified/unknown - never guess a launcher
                // more privileged than plain sys_spawn.
    }
}

// (#845) Per-app Win16 execution mode override, only meaningful when
// type=win16. -1 (auto) lets the kernel derive the mode from the NE header
// (ne.c's win16_decide_pmode()); "real"/"pm" force it explicitly, an explicit
// override always winning over the derived answer. Same "never guess"
// philosophy as sm_type_from_str: an unrecognized or absent value is auto,
// not a silent force.
fn sm_mode_from_str(s: &str) -> i32 {
    match s {
        "real" => 0,
        "pm" | "protected" => 1,
        _ => -1,
    }
}

fn process_category_line(rest: &str, m: &mut Model, current_cat: &mut Option<String>) {
    let mut parts = rest.split('|');
    let label = match parts.next() {
        Some(l) => l.trim(),
        None => return,
    };
    if label.is_empty() {
        return;
    }
    let mut expanded = false;
    let mut explicit_id: Option<String> = None;
    for field in parts {
        let (k, v) = split_field(field);
        match k {
            "expanded" => expanded = v.is_empty() || v == "1" || v == "true",
            "id" if !v.is_empty() => explicit_id = Some(slugify(v)),
            _ => {}
        }
    }
    let id = explicit_id.unwrap_or_else(|| slugify(label));
    if let Some(existing) = find_cat_mut(&mut m.cats, &id) {
        existing.label = String::from(label);
        existing.expanded = expanded;
    } else {
        m.cats.push(CatRec { id: id.clone(), label: String::from(label), expanded });
    }
    *current_cat = Some(id);
}

fn process_item_line(rest: &str, m: &mut Model, current_cat: &Option<String>) {
    let cat_id = match current_cat {
        Some(c) => c.clone(),
        // No category: seen yet in THIS fragment - dropped, matching the
        // pre-existing "item before any category: skip" rule (#208/#454),
        // generalized to every fragment source instead of duplicated per
        // hand-rolled loader.
        None => return,
    };
    let mut parts = rest.split('|');
    let name = match parts.next() {
        Some(n) => n.trim(),
        None => return,
    };
    let path = match parts.next() {
        Some(p) => p.trim(),
        None => return,
    };
    if name.is_empty() || path.is_empty() {
        return;
    }
    let mut explicit_id: Option<String> = None;
    let mut icon = String::new();
    let mut launch_type = 0i32;
    let mut win16_mode = -1i32;
    for field in parts {
        let (k, v) = split_field(field);
        match k {
            "id" if !v.is_empty() => explicit_id = Some(slugify(v)),
            "icon" => icon = String::from(v),
            "type" => launch_type = sm_type_from_str(v),
            "mode" => win16_mode = sm_mode_from_str(v),
            _ => {}
        }
    }
    let id = explicit_id.unwrap_or_else(|| slugify(path));
    if let Some(existing) = find_item_mut(&mut m.items, &id) {
        existing.cat_id = cat_id;
        existing.name = String::from(name);
        existing.path = String::from(path);
        existing.icon = icon;
        existing.launch_type = launch_type;
        existing.win16_mode = win16_mode;
        existing.hidden = false;
    } else {
        m.items.push(ItemRec {
            id,
            cat_id,
            name: String::from(name),
            path: String::from(path),
            icon,
            launch_type,
            win16_mode,
            hidden: false,
        });
    }
}

fn process_rename_line(rest: &str, m: &mut Model) {
    let mut parts = rest.splitn(2, '|');
    let id = match parts.next() {
        Some(i) => slugify(i.trim()),
        None => return,
    };
    let newname = match parts.next() {
        Some(n) => n.trim(),
        None => return,
    };
    if newname.is_empty() {
        return;
    }
    if let Some(existing) = find_item_mut(&mut m.items, &id) {
        existing.name = String::from(newname);
        existing.hidden = false;
    }
    // A rename: naming an id that has not been seen yet is a no-op (nothing
    // to rename) rather than creating a phantom item with no real path.
}

fn process_hide_line(rest: &str, m: &mut Model) {
    let id = slugify(rest.trim());
    if id.is_empty() {
        return;
    }
    if let Some(existing) = find_item_mut(&mut m.items, &id) {
        existing.hidden = true;
    } else {
        // Tombstone an id we have not seen yet, so a hide: that is processed
        // BEFORE the item: it targets (a system default hidden by a
        // filename-earlier user fragment than the one defining it, an
        // unusual but legal ordering) still wins - a later item:/rename: for
        // this id un-hides it (sets hidden = false), per the file's general
        // last-write-wins rule.
        m.items.push(ItemRec {
            id,
            cat_id: String::new(),
            name: String::new(),
            path: String::new(),
            icon: String::new(),
            launch_type: 0,
            win16_mode: -1,   // (#845) tombstone entry, never launched
            hidden: true,
        });
    }
}

/// Reset the whole merge model. Called once at the start of every rebuild
/// (startmenu.c's sm_rust_rebuild()), before any fragment is fed in.
#[no_mangle]
pub extern "C" fn sm_model_reset() {
    unsafe {
        let ptr = core::ptr::addr_of_mut!(MODEL);
        *ptr = Some(Model { cats: Vec::new(), items: Vec::new() });
    }
}

// ---------------------------------------------------------------------------
// THE PER-FRAGMENT PARSE STATE, HOISTED OUT OF THE PARSE LOOP, AND WHY.
//
// `current_cat` (the category a bare `item:` line attaches to) was a LOCAL in
// sm_model_add_fragment(). That is a small-looking detail with a large
// consequence: because the state lived inside one call, the caller had to hand
// the WHOLE fragment file over in ONE buffer, and startmenu.c's buffer was a
// static char[8192] filled by a single sys_read(). A fragment larger than that
// lost every item past byte 8191, silently: the parser never saw those lines,
// so the "dropped" count that #220 added could not report them either.
//
// That is not hypothetical. build/assets/startmenu/system.d/03-games.MENU
// crossed 8191 bytes on 2026-08-23 and the four items at its end - Rogue,
// Rogue (DOS), Discworld II and Pyro! (floppy) - stopped existing. The owner
// reported Discworld II missing from the Start menu; the CD volume was mounted,
// the fragment line was on disk and correct, and the item had simply been read
// off the end of a buffer.
//
// Holding it here instead lets a fragment be fed ONE LINE AT A TIME
// (sm_model_frag_begin / sm_model_frag_line / sm_model_frag_end), so the caller
// streams the file through a small fixed chunk buffer and there is NO
// whole-file size limit anywhere to outgrow. sm_model_add_fragment() below is
// kept as a thin wrapper over the same three calls, so there is still exactly
// ONE implementation of the parse.
//
// Single-threaded by the same argument as MODEL above: one call path, the
// compositor's draw/poll thread, never concurrent.
static mut FRAG_CAT: Option<String> = None;

fn frag_cat() -> &'static mut Option<String> {
    // SAFETY: same single-call-path argument as model(); the raw-pointer form
    // satisfies the 2024-edition static-mut-references lint.
    unsafe { &mut *core::ptr::addr_of_mut!(FRAG_CAT) }
}

/// Begin one fragment. Resets the per-fragment category scope, which is what
/// makes a bare `item:` before any `category:` a dropped item in THIS fragment
/// rather than an item attached to the previous fragment's last category.
#[no_mangle]
pub extern "C" fn sm_model_frag_begin() {
    *frag_cat() = None;
}

/// Feed ONE line of the fragment currently open. `text_ptr` need not be
/// NUL-terminated and need not be a whole line's worth of anything else: the
/// caller has already split on newlines. Invalid UTF-8 is replaced (lossy),
/// never a reason to abort the rebuild over one malformed line.
///
/// # Safety
/// `text_ptr` must be valid for `text_len` bytes for the duration of this call.
#[no_mangle]
pub unsafe extern "C" fn sm_model_frag_line(text_ptr: *const u8, text_len: u32) {
    if text_ptr.is_null() || text_len == 0 {
        return;
    }
    let bytes = core::slice::from_raw_parts(text_ptr, text_len as usize);
    let text = String::from_utf8_lossy(bytes);
    let line = text.trim();
    if line.is_empty() || line.starts_with('#') {
        return;
    }
    let m = model();
    let cc = frag_cat();
    if let Some(rest) = line.strip_prefix("category:") {
        process_category_line(rest, m, cc);
    } else if let Some(rest) = line.strip_prefix("item:") {
        process_item_line(rest, m, &*cc);
    } else if let Some(rest) = line.strip_prefix("rename:") {
        process_rename_line(rest, m);
    } else if let Some(rest) = line.strip_prefix("hide:") {
        process_hide_line(rest, m);
    }
    // Any other directive keyword is ignored rather than treated as an
    // error - a newer fragment format understood by a future compositor
    // build degrades gracefully on an older one instead of losing the
    // whole fragment.
}

/// End the fragment opened by sm_model_frag_begin(). Dropping the category
/// scope here rather than only at the next begin() means a caller that begins a
/// fragment and then fails part-way cannot leak that scope into the next one.
#[no_mangle]
pub extern "C" fn sm_model_frag_end() {
    *frag_cat() = None;
}

/// Feed one whole fragment file's text. Layer (system vs. user) is not
/// distinguished here - the CALLER's feed order across fragments (system,
/// filename-sorted, then user, filename-sorted) is what gives the system
/// layer its "processed first, can be overridden" position and the user
/// layer its "processed last, wins collisions" precedence. `text_ptr` need
/// not be NUL-terminated; invalid UTF-8 bytes are replaced (lossy), never a
/// reason to abort the whole rebuild over one malformed fragment.
///
/// Retained as a WRAPPER over begin/line/end so that the streaming caller and
/// any whole-buffer caller share one parse, and so this entry point keeps
/// working for anything that already has the text in hand (the harness does).
///
/// # Safety
/// `text_ptr` must be valid for `text_len` bytes for the duration of this
/// call.
#[no_mangle]
pub unsafe extern "C" fn sm_model_add_fragment(text_ptr: *const u8, text_len: u32) {
    if text_ptr.is_null() || text_len == 0 {
        return;
    }
    let bytes = core::slice::from_raw_parts(text_ptr, text_len as usize);
    let text = String::from_utf8_lossy(bytes);
    sm_model_frag_begin();
    for raw_line in text.lines() {
        sm_model_frag_line(raw_line.as_ptr(), raw_line.len() as u32);
    }
    sm_model_frag_end();
}

/// Run the existence check and emit the final merged menu via
/// sm_c_add_category()/sm_c_add_item(), in final category order (each
/// category immediately followed by all of its surviving items, matching
/// startmenu.c's add_item_typed() "attaches to the most recently added
/// category" contract). Returns the number of items actually emitted, for
/// diagnostics/logging only.
///
/// If BOTH layers were empty, missing, or produced nothing that survives the
/// existence check, this emits zero categories and zero items. That is
/// correct, not a bug: see the "NO FALLBACK, EVER" note at the top of this
/// file.
#[no_mangle]
pub extern "C" fn sm_model_finish() -> i32 {
    let m = model();
    let mut emitted = 0i32;
    for cat in m.cats.iter() {
        // Items in this category, in first-occurrence order, surviving
        // hide + the existence check.
        let mut cat_items: Vec<&ItemRec> = m
            .items
            .iter()
            .filter(|it| it.cat_id == cat.id && !it.hidden)
            .collect();
        cat_items.retain(|it| {
            if it.path.starts_with('@') {
                return true; // pseudo-action sentinel: never disk-checked
            }
            // #220: the field is a LAUNCH LINE, not a bare path. Check the
            // BINARY. Handing the whole line to access() is what made the
            // Stunts entry ("/DOS/STUNTS/LOAD.EXE /u MCGA") invisible while
            // /DOS/STUNTS/LOAD.EXE was sitting on the ext2 root. The full,
            // UNSPLIT line still travels to the launcher (sm_c_add_item below
            // passes it.path verbatim) because the kernel splits it again on
            // its side; splitting for the CHECK must never become splitting
            // for the LAUNCH, or the argument would be lost.
            let bin = launch_binary(&it.path);
            if bin.is_empty() {
                return false;   // no resolvable target at all
            }
            let pb = bin.as_bytes();
            let alive = unsafe { sm_c_path_exists(pb.as_ptr(), pb.len() as u32) != 0 };
            if !alive {
                let nb = it.name.as_bytes();
                unsafe {
                    sm_c_note_drop(nb.as_ptr(), nb.len() as u32, pb.as_ptr(), pb.len() as u32);
                }
            }
            alive
        });
        if cat_items.is_empty() {
            continue; // no empty collapsible category headers
        }
        let lb = cat.label.as_bytes();
        unsafe {
            sm_c_add_category(lb.as_ptr(), lb.len() as u32, if cat.expanded { 1 } else { 0 });
        }
        for it in cat_items {
            let nb = it.name.as_bytes();
            let pb = it.path.as_bytes();
            let ib = it.icon.as_bytes();
            unsafe {
                sm_c_add_item(
                    nb.as_ptr(),
                    nb.len() as u32,
                    pb.as_ptr(),
                    pb.len() as u32,
                    ib.as_ptr(),
                    ib.len() as u32,
                    it.launch_type,
                    it.win16_mode,
                );
            }
            emitted += 1;
        }
    }
    emitted
}
