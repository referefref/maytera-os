// rustkern/fat.rs - #404 Phase S VFAT directory-entry + LFN parse (fs/fat.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ============================================================================
// #404 Phase S: fs/fat.c VFAT directory-entry + long-file-name (LFN) parse.
//
// This is a REACHABLE, LIVE-PATH untrusted-input parser: the golden is FAT-root,
// so fat_readdir_inner decodes attacker-supplyable directory bytes at every boot
// (ttf_init() scans /FONTS) and from userland via SYS_READDIR. fat_dir_step_rs
// is the pure per-entry decode + LFN reassembly, live under -DRUST_FAT via the
// C seam in fs/fat.c; fat_dir_step_c is the verbatim reference kept for rollback
// + differential. The cluster-chain walk + sector read (dir_read_entry) stay C.
//
// FatLfnState / FatParsedEntry are #[repr(C)] mirrors of the fs/fat.c structs;
// their sizes are asserted there via _Static_assert (264 / 292) so this FFI can
// never silently drift.
//
// CONFINEMENT: the C reference's internal longname[260] reassembly is itself
// bounded (the (seq-1)*13<255 guard caps the last write index at 259), but it
// can EMIT a name of up to 259 chars, which fat_readdir_inner then strcpy()s
// into caller buffers of 256 (SYS_READDIR name_buf), 64 (fat_delete) or 16
// (dosexec) bytes - a reachable stack/heap overflow on a crafted FAT directory.
// The Rust port (a) bounds every longname write as a checked slice index and
// (b) confines the emitted name to <=FAT_NAME_MAX_RS (255) chars so the
// downstream copy can never exceed the 256-byte contract. On real entries
// (names < 256 chars) it is byte-identical to the C.
// ============================================================================

const FAT_STEP_CONTINUE_RS: i32 = 0;
const FAT_STEP_SHORT_RS: i32 = 1;
const FAT_STEP_END_RS: i32 = 2;
const FAT_ATTR_LFN_RS: u8 = 0x0F;
// Downstream contract: caller name buffers are 256 bytes = 255 chars + NUL.
const FAT_NAME_MAX_RS: usize = 255;

#[repr(C)]
pub struct FatLfnState {
    pub longname: [u8; 260],
    pub have_long: i32,
}

#[repr(C)]
pub struct FatParsedEntry {
    pub raw: [u8; 32],
    pub name: [u8; 260],
}

// Faithful port of fs/fat.c lfn_extract: pull the (up to) 13 chars of one LFN
// entry (fixed UCS-2 slot offsets) into `out`, ASCII subset only, stopping at a
// 0x0000 or 0xFFFF code unit. Returns the count written.
fn lfn_extract_rs(e: &[u8; 32], out: &mut [u8; 16]) -> usize {
    const SLOT: [usize; 13] = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30];
    let mut n = 0usize;
    for k in 0..13 {
        let s = SLOT[k];
        let ch = (e[s] as u16) | ((e[s + 1] as u16) << 8);
        if ch == 0x0000 || ch == 0xFFFF {
            break;
        }
        out[n] = if ch < 0x80 { ch as u8 } else { b'_' };
        n += 1;
    }
    n
}

// Faithful port of fs/fat.c fat_name_to_str: decode the 11-byte 8.3 short name
// (8 name bytes then optional '.' + 3 ext bytes, trailing spaces trimmed) into a
// NUL-terminated string. Writes at most 12 chars + NUL.
fn fat_name_to_str_rs(e: &[u8; 32], out: &mut [u8; 260]) {
    let mut j = 0usize;
    let mut i = 0usize;
    while i < 8 && e[i] != b' ' {
        out[j] = e[i];
        j += 1;
        i += 1;
    }
    if e[8] != b' ' {
        out[j] = b'.';
        j += 1;
        let mut i2 = 8usize;
        while i2 < 11 && e[i2] != b' ' {
            out[j] = e[i2];
            j += 1;
            i2 += 1;
        }
    }
    out[j] = 0;
}

/// Rust port of the fs/fat.c per-entry directory decode + LFN reassembly. Pure:
/// no disk I/O, no FS mutation. Returns FAT_STEP_END/CONTINUE/SHORT identically
/// to fat_dir_step_c on real entries; confines the reachable over-long-name
/// class described above.
#[no_mangle]
pub extern "C" fn fat_dir_step_rs(
    st: *mut FatLfnState,
    e: *const u8,
    out: *mut FatParsedEntry,
) -> i32 {
    // fat_readdir_inner always passes valid, non-null st/e; guard defensively.
    if st.is_null() || e.is_null() {
        return FAT_STEP_END_RS;
    }
    // SAFETY: per the C ABI contract from fs/fat.c, `e` points to a readable
    // 32-byte directory entry, `st` to a writable FatLfnState (the persistent
    // accumulation state, a &stack local in fat_readdir_inner), and `out`, when
    // non-null, to a writable FatParsedEntry. `e` is read-only; `st`/`out` are
    // POD. We build a fixed [u8;32] view of `e` so every field access is a
    // bounds-checked index and no read can leave the entry.
    let ent: &[u8; 32] = unsafe { &*(e as *const [u8; 32]) };
    let state: &mut FatLfnState = unsafe { &mut *st };

    let b = ent[0];
    if b == 0x00 {
        return FAT_STEP_END_RS;
    }
    if b == 0xE5 {
        state.have_long = 0;
        return FAT_STEP_CONTINUE_RS;
    }
    if ent[11] == FAT_ATTR_LFN_RS {
        let seq = (ent[0] & 0x3F) as i32;
        if (ent[0] & 0x40) != 0 {
            state.have_long = 1;
            for x in state.longname.iter_mut() {
                *x = 0;
            }
        }
        // Same predicate the C reference has: seq >= 1 and (seq-1)*13 < 255
        // (so seq <= 20, last write index <= 259). Every write is ALSO a checked
        // slice index, so a crafted seq/fragment can never leave longname[260].
        if state.have_long == 1 && seq >= 1 && (seq - 1) * 13 < 255 {
            let mut piece = [0u8; 16];
            let n = lfn_extract_rs(ent, &mut piece);
            let base = ((seq - 1) * 13) as usize;
            for j in 0..n {
                let idx = base + j;
                if idx < state.longname.len() {
                    state.longname[idx] = piece[j];
                }
            }
        }
        return FAT_STEP_CONTINUE_RS;
    }

    // Short entry -> decode name (long if an LFN set was accumulated, else 8.3).
    if !out.is_null() {
        // SAFETY: out non-null + writable per the C ABI contract; POD write.
        let o: &mut FatParsedEntry = unsafe { &mut *out };
        o.raw.copy_from_slice(ent);
        if state.have_long == 1 {
            state.longname[259] = 0;
            // CONFINEMENT: copy at most FAT_NAME_MAX_RS (255) chars, stopping at
            // the first NUL, so the downstream strcpy(name_out, pe.name) cannot
            // exceed the 256-byte caller contract even on a crafted 256..259-char
            // name (where the C reference would over-produce and overflow).
            let mut k = 0usize;
            while k < FAT_NAME_MAX_RS && state.longname[k] != 0 {
                o.name[k] = state.longname[k];
                k += 1;
            }
            o.name[k] = 0;
        } else {
            let mut nm = [0u8; 260];
            fat_name_to_str_rs(ent, &mut nm);
            let mut k = 0usize;
            while k < FAT_NAME_MAX_RS && nm[k] != 0 {
                o.name[k] = nm[k];
                k += 1;
            }
            o.name[k] = 0;
        }
    }
    FAT_STEP_SHORT_RS
}
