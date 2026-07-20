// rustkern/elf.rs - #404 Phase Q / #499 ELF64 header + program-header validation
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ---------------------------------------------------------------------------
// #404 / #499 Phase Q: exec/elf.c ELF64 header + program-header VALIDATION
// (Tier 2, untrusted on-disk/loaded image: a user ELF is attacker-controllable
// - a downloaded/dropped app, or any /apps/* binary). This is the loader's
// PURE pre-map validation: e_ident magic/class/data/version + e_type + e_machine
// + the program-header table bounds + a PT_LOAD file-bounds walk. It does NOT
// map, allocate, CR3-switch, or copy a single byte - the segment copy + mmap +
// PT_DYNAMIC relocation engine stay in C (exec/elf.c), which is where the paging
// entanglement lives. exec/elf.c routes the live elf_validate() here under
// -DRUST_ELF (else elf_validate_full_c, the verbatim reference).
//
// SECURITY (this is the strongest win in the port series so far - genuinely
// REACHABLE, and not just a read): the verbatim C reference has THREE gaps on
// the untrusted header that the Rust removes BY CONSTRUCTION (slice of exactly
// filelen bytes, every field read + phdr access bounds-checked, no unchecked
// pointer arithmetic, correct no-underflow bounds math):
//   (1) OVERSIZED p_filesz. calculate_load_bounds() gates each PT_LOAD with
//       check_overflow_add(p_offset, p_filesz, size) == (p_offset > size -
//       p_filesz). When p_filesz > size the `size - p_filesz` UNDERFLOWS to a
//       near-2^64 value, so the guard returns false and the segment PASSES with
//       a p_filesz far past EOF; elf_load() then memcpy(dest, elf_data +
//       p_offset, p_filesz) (elf.c:771) -> OOB READ of the file heap buffer AND
//       OOB WRITE past the (memsz-sized) load allocation. Reachable on any
//       crafted ELF. ASan-PROVEN (heap-buffer-overflow READ of 4216 bytes in
//       the simulated loader copy). Rust checks `p_filesz <= size && p_offset <=
//       size - p_filesz` (no underflow) and rejects.
//   (2) UNDERSIZED e_phentsize. elf_validate bounds the phdr TABLE end with the
//       attacker's e_phentsize but never checks e_phentsize >= sizeof(Elf64_Phdr)
//       (56); every elf_get_phdr(i) then reads a full 56-byte Elf64_Phdr at
//       e_phoff + i*e_phentsize, which for e_phentsize < 56 reads past the
//       validated table and past EOF. ASan-PROVEN (heap-buffer-overflow READ of
//       8 bytes inside the phdr walk). Rust rejects e_phentsize < 56.
//   (3) p_memsz < p_filesz. The C never checks it; the segment copy writes
//       p_filesz bytes into a region sized by p_memsz (write-overflow). Latent
//       defense-in-depth (real ELFs never invert them). Rust rejects.
// HONEST: (1) and (2) are reachable OOBs in the live C loader today; the live
// -DRUST_ELF path CONFINES them (elf_load bails on the Rust reject before
// calculate_load_bounds / the copy run). This is a candidate plain-C fix too
// (like the ext2 #476 find); flagged in blame.md. On WELL-FORMED ELFs (every
// real /apps binary: e_phentsize==56, in-bounds segments, memsz>=filesz) the
// Rust returns SUCCESS byte-identically to the C, so the desktop boots.
// ElfValidated is #[repr(C)]; sizeof asserted == 40 in elf.h so the FFI struct
// can never silently drift.

use crate::common::{elf_rd_u16, elf_rd_u32, elf_rd_u64};

const ELF_SUCCESS_RS: i32 = 0;
const ELF_ERR_NULL_PTR_RS: i32 = -1;
const ELF_ERR_TOO_SMALL_RS: i32 = -2;
const ELF_ERR_BAD_MAGIC_RS: i32 = -3;
const ELF_ERR_NOT_64BIT_RS: i32 = -4;
const ELF_ERR_NOT_LE_RS: i32 = -5;
const ELF_ERR_BAD_VERSION_RS: i32 = -6;
const ELF_ERR_NOT_X86_64_RS: i32 = -7;
const ELF_ERR_NOT_EXEC_RS: i32 = -8;
const ELF_ERR_NO_PHDR_RS: i32 = -9;
const ELF_ERR_PHDR_OVERFLOW_RS: i32 = -10;
const ELF_ERR_SEGMENT_OVERFLOW_RS: i32 = -11;

const EI_CLASS_RS: usize = 4;
const EI_DATA_RS: usize = 5;
const EI_VERSION_RS: usize = 6;
const ELFCLASS64_RS: u8 = 2;
const ELFDATA2LSB_RS: u8 = 1;
const EV_CURRENT_RS: u32 = 1;
const EM_X86_64_RS: u16 = 62;
const ET_EXEC_RS: u16 = 2;
const ET_DYN_RS: u16 = 3;
const PT_LOAD_RS: u32 = 1;
const ELF64_EHDR_SIZE: u64 = 64;
const ELF64_PHDR_SIZE: u64 = 56;

// #[repr(C)] mirror of elf_validated_t (exec/elf.h). Layout asserted == 40 bytes
// on the C side (_Static_assert in elf.c) so this FFI struct can never drift.
#[repr(C)]
pub struct ElfValidated {
    pub e_type: u16,
    pub e_machine: u16,
    pub e_phnum: u16,
    pub e_phentsize: u16,
    pub n_load: u32,
    pub e_entry: u64,
    pub e_phoff: u64,
    pub first_load_vaddr: u64,
}


/// Rust port of the exec/elf.c ELF64 pre-map validation (Tier 2, untrusted
/// image). Validates e_ident/type/machine + the phdr table + every PT_LOAD's
/// file bounds and memsz>=filesz, filling `out` with the parsed header summary.
/// Returns ELF_SUCCESS(0) or a negative ELF_ERR_* code identical to the C. PURE:
/// never maps/allocates/copies/CR3-switches. Every read is a bounds-checked
/// index into a slice of exactly `size` bytes.
#[no_mangle]
pub extern "C" fn elf_validate_full_rs(buf: *const u8, size: u64, out: *mut ElfValidated) -> i32 {
    // Matches C `if (elf_data == NULL) return ELF_ERR_NULL_PTR;`.
    if buf.is_null() {
        return ELF_ERR_NULL_PTR_RS;
    }
    // Zero the out summary up front (POD write) if provided.
    // SAFETY: out, when non-null, is a valid writable ElfValidated* per the C ABI
    // contract (elf.c passes &stack_local). We only POD-write it.
    if !out.is_null() {
        unsafe {
            *out = ElfValidated {
                e_type: 0,
                e_machine: 0,
                e_phnum: 0,
                e_phentsize: 0,
                n_load: 0,
                e_entry: 0,
                e_phoff: 0,
                first_load_vaddr: 0,
            };
        }
    }
    // Minimum size for the ELF header. After this, every ehdr field (all within
    // the first 64 bytes) is in-slice.
    if size < ELF64_EHDR_SIZE {
        return ELF_ERR_TOO_SMALL_RS;
    }
    // SAFETY: buf is non-null and, per the C ABI contract from exec/elf.c, points
    // to at least `size` readable bytes (the on-disk image buffer). We build a
    // slice spanning EXACTLY those `size` bytes; every access below is a
    // bounds-checked index into it, so no read can leave the buffer even if a
    // header field (attacker-controlled) is nonsense.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(buf, size as usize) };

    // e_ident magic (0x7F 'E' 'L' 'F').
    if s[0] != 0x7F || s[1] != b'E' || s[2] != b'L' || s[3] != b'F' {
        return ELF_ERR_BAD_MAGIC_RS;
    }
    if s[EI_CLASS_RS] != ELFCLASS64_RS {
        return ELF_ERR_NOT_64BIT_RS;
    }
    if s[EI_DATA_RS] != ELFDATA2LSB_RS {
        return ELF_ERR_NOT_LE_RS;
    }
    let e_version = match elf_rd_u32(s, 20) {
        Some(v) => v,
        None => return ELF_ERR_TOO_SMALL_RS,
    };
    if s[EI_VERSION_RS] as u32 != EV_CURRENT_RS || e_version != EV_CURRENT_RS {
        return ELF_ERR_BAD_VERSION_RS;
    }
    let e_type = elf_rd_u16(s, 16).unwrap_or(0);
    let e_machine = elf_rd_u16(s, 18).unwrap_or(0);
    let e_phentsize = elf_rd_u16(s, 54).unwrap_or(0);
    let e_phnum = elf_rd_u16(s, 56).unwrap_or(0);
    let e_entry = elf_rd_u64(s, 24).unwrap_or(0);
    let e_phoff = elf_rd_u64(s, 32).unwrap_or(0);

    if e_machine != EM_X86_64_RS {
        return ELF_ERR_NOT_X86_64_RS;
    }
    if e_type != ET_EXEC_RS && e_type != ET_DYN_RS {
        return ELF_ERR_NOT_EXEC_RS;
    }
    if e_phnum == 0 {
        return ELF_ERR_NO_PHDR_RS;
    }
    // CONFINEMENT (2): reject an undersized phdr entry so every 56-byte phdr read
    // below stays inside the validated table (the C omits this - elf_get_phdr
    // then over-reads). Real ELF64 always uses e_phentsize == 56.
    if (e_phentsize as u64) < ELF64_PHDR_SIZE {
        return ELF_ERR_PHDR_OVERFLOW_RS;
    }
    // Program-header table bounds, computed WITHOUT overflow. e_phnum*e_phentsize
    // is at most 65535*65535 (< 2^32), so the product never overflows u64.
    let tbl = (e_phnum as u64) * (e_phentsize as u64);
    if e_phoff > size || tbl > size - e_phoff {
        return ELF_ERR_PHDR_OVERFLOW_RS;
    }

    // PT_LOAD walk. phentsize>=56 + table bounds guarantee each phdr's 56 bytes
    // are in-slice; the readers still bounds-check defensively.
    let mut n_load: u32 = 0;
    let mut first_v: u64 = 0;
    let mut found = false;
    let mut i: u64 = 0;
    while i < e_phnum as u64 {
        let po = e_phoff + i * (e_phentsize as u64);
        let p_type = match elf_rd_u32(s, po) {
            Some(v) => v,
            None => return ELF_ERR_PHDR_OVERFLOW_RS,
        };
        if p_type != PT_LOAD_RS {
            i += 1;
            continue;
        }
        let p_offset = match elf_rd_u64(s, po + 8) {
            Some(v) => v,
            None => return ELF_ERR_PHDR_OVERFLOW_RS,
        };
        let p_vaddr = match elf_rd_u64(s, po + 16) {
            Some(v) => v,
            None => return ELF_ERR_PHDR_OVERFLOW_RS,
        };
        let p_filesz = match elf_rd_u64(s, po + 32) {
            Some(v) => v,
            None => return ELF_ERR_PHDR_OVERFLOW_RS,
        };
        let p_memsz = match elf_rd_u64(s, po + 40) {
            Some(v) => v,
            None => return ELF_ERR_PHDR_OVERFLOW_RS,
        };
        // CONFINEMENT (1): correct file-bounds - NO underflow bypass. Reject any
        // segment whose [p_offset, p_offset+p_filesz) leaves the image.
        if p_filesz > size || p_offset > size - p_filesz {
            return ELF_ERR_SEGMENT_OVERFLOW_RS;
        }
        // CONFINEMENT (3): the segment copy writes p_filesz into a memsz-sized
        // region; p_filesz > p_memsz is a write-overflow in the C loader.
        if p_memsz < p_filesz {
            return ELF_ERR_SEGMENT_OVERFLOW_RS;
        }
        if !found {
            first_v = p_vaddr;
            found = true;
        }
        n_load += 1;
        i += 1;
    }
    if !found {
        // Mirrors calculate_load_bounds: no PT_LOAD segments found.
        return ELF_ERR_NO_PHDR_RS;
    }

    if !out.is_null() {
        // SAFETY: out non-null + valid writable per the C ABI contract; POD write.
        unsafe {
            *out = ElfValidated {
                e_type,
                e_machine,
                e_phnum,
                e_phentsize,
                n_load,
                e_entry,
                e_phoff,
                first_load_vaddr: first_v,
            };
        }
    }
    ELF_SUCCESS_RS
}
