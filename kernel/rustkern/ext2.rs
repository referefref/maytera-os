// rustkern/ext2.rs - #404 Phase C / #485 ext2 directory-block entry scan
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase C port (#404 / #485): ext2 directory-block entry scan.
//
// Faithful, memory-safe Rust drop-in for the inner per-block entry loop of
// fs/ext2.c ext2_lookup() (the loop that parses fully disk-controlled rec_len /
// name_len - the classic untrusted-input OOB-read surface, hardened in plain C
// under #476). Same signature and return contract as the C reference
// ext2_dirblock_find_c(): returns 1 and fills *out_ino / *out_type if `name`
// (length name_len) is found in this directory block, 0 otherwise. `ci` != 0 =>
// case-insensitive ASCII compare (mirrors g_root_ext2).
//
// GUARD ALIGNMENT (important, honest note): the PoC port proved equivalence to a
// C extract on WELL-FORMED input only, and used a looser `rec == 0` termination.
// The LIVE C reference here is the #476-hardened loop, whose three guards are
// (1) the 8-byte header must fit (`off + 8 <= block_size`), (2) rec_len must be
// sane (`rec >= 8 && off + rec <= block_size`, else stop), (3) the name field
// must fit (`off + 8 + name_len <= block_size`, else stop). To be a BYTE-
// IDENTICAL drop-in for that live loop on BOTH well-formed AND malformed blocks
// (so the boot-time differential self-test passes by construction, not by luck),
// this Rust adopts those exact three guards. Memory safety is unchanged and
// independent of the guards: every read goes through a slice spanning exactly
// `block_size` bytes, so an out-of-range index can only ever panic-or-return,
// never read out of bounds.
#[no_mangle]
pub extern "C" fn ext2_dirblock_find_rs(
    blk: *const u8,
    block_size: u32,
    name: *const u8,
    name_len: u32,
    ci: i32,
    out_ino: *mut u32,
    out_type: *mut u8,
) -> i32 {
    let bs = block_size as usize;
    let nl = name_len as usize;

    // SAFETY: The caller guarantees `blk` points to at least `block_size`
    // contiguous readable bytes (the exact contract fs/ext2.c relies on: `blk`
    // is a freshly kmalloc(block_size) buffer filled by ext2_read_block) and
    // that `name` points to at least `name_len` readable bytes. We build two
    // slices spanning exactly those extents and from here on index ONLY through
    // them, so every access is bounds-checked by Rust: a disk-controlled
    // rec_len / name_len that would push an index out of range hits the explicit
    // guards below and returns safely, never an out-of-bounds read.
    let block: &[u8] = unsafe { core::slice::from_raw_parts(blk, bs) };
    let qname: &[u8] = unsafe { core::slice::from_raw_parts(name, nl) };

    let mut off: usize = 0;
    // Guard 1 (matches C `while (off + 8 <= block_size)`): the 8-byte entry
    // header must fit before we read it.
    while off + 8 <= bs {
        let e_ino = u32::from_le_bytes([block[off], block[off + 1], block[off + 2], block[off + 3]]);
        let rec = u16::from_le_bytes([block[off + 4], block[off + 5]]) as usize;
        let nlen = block[off + 6] as usize;
        let ftype = block[off + 7];

        // Guard 2 (matches C `if (rec < 8 || off + rec > block_size) break;`):
        // reject a malformed rec_len that would infinite-loop or overrun.
        if rec < 8 || off + rec > bs {
            break;
        }

        if e_ino != 0 && nlen == nl {
            // Guard 3 (matches C `if (off + 8 + name_len > block_size) break;`):
            // the name field must lie within the block.
            if off + 8 + nl > bs {
                break;
            }
            let mut matched = true;
            let mut i = 0usize;
            while i < nl {
                let mut a = block[off + 8 + i];
                let mut b = qname[i];
                if ci != 0 {
                    if a >= b'a' && a <= b'z' {
                        a -= 32;
                    }
                    if b >= b'a' && b <= b'z' {
                        b -= 32;
                    }
                }
                if a != b {
                    matched = false;
                    break;
                }
                i += 1;
            }
            if matched {
                // SAFETY: out_ino / out_type are either null or valid,
                // caller-provided writable pointers (same contract as the C
                // reference which writes *out_ino / *out_type). We write only
                // after a null check.
                unsafe {
                    if !out_ino.is_null() {
                        *out_ino = e_ino;
                    }
                    if !out_type.is_null() {
                        *out_type = ftype;
                    }
                }
                return 1;
            }
        }

        off += rec;
    }
    0
}
