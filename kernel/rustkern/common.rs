// rustkern/common.rs - shared primitives used by more than one subsystem module.
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim.
//
// These bounds-checked little-endian readers were defined in the ELF validator
// and reached for by the PE, exFAT and BMP parsers too, which is why they carry
// an `elf_rd_` name they outgrew. They live here rather than being copied per
// module: a private fork of a shared primitive per subsystem is the exact
// disease CLAUDE.md forbids. Names are kept EXACTLY as-is so this refactor
// changes zero behavior; renaming them is a separate, reviewable change.
//
// They are `pub(crate)`: visible to sibling modules, never exported from the
// crate (no #[no_mangle], so they are NOT part of the FFI surface).

// Bounds-checked little-endian reads over a slice of exactly filelen bytes. Each
// returns None if the field would leave the slice (never happens on the checked
// path, but is a hard confinement: an out-of-range field can only produce a
// reject, never an out-of-bounds read).
#[inline]
pub(crate) fn elf_rd_u16(s: &[u8], off: u64) -> Option<u16> {
    let o = off as usize;
    if o.checked_add(2)? > s.len() {
        return None;
    }
    Some((s[o] as u16) | ((s[o + 1] as u16) << 8))
}
#[inline]
pub(crate) fn elf_rd_u32(s: &[u8], off: u64) -> Option<u32> {
    let o = off as usize;
    if o.checked_add(4)? > s.len() {
        return None;
    }
    Some((s[o] as u32) | ((s[o + 1] as u32) << 8) | ((s[o + 2] as u32) << 16) | ((s[o + 3] as u32) << 24))
}
#[inline]
pub(crate) fn elf_rd_u64(s: &[u8], off: u64) -> Option<u64> {
    let o = off as usize;
    if o.checked_add(8)? > s.len() {
        return None;
    }
    let mut r: u64 = 0;
    let mut i = 7i32;
    while i >= 0 {
        r = (r << 8) | (s[o + i as usize] as u64);
        i -= 1;
    }
    Some(r)
}
