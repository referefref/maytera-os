// rustkern/pe.rs - #404 Phase R PE32 pre-map validation (exec/pe.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ============================================================================
// #404 / Phase R: exec/pe.c PE32 pre-map validation (Tier 2, UNTRUSTED on-disk
// image). A PE binary (DOS+PE header offsets, NumberOfSections, section
// RVAs/sizes) is fully attacker-controllable, the classic malformed-header OOB
// class. This is the Rust port of pe_validate_full_c (exec/pe.c) - the DOS + PE
// signature + COFF + OptionalHeader validation followed by the section-table
// raw/virtual-bounds walk - filling `out` with the parsed header summary. The
// section copy + import resolution + relocation stay in C (pe_load/pe_execute).
//
// The C loader has THREE OOB classes on the untrusted image that this Rust
// CONFINES by construction:
//   (1) NO section-table bounds check - pe_get_sections() locates the table at
//       &OptionalHeader + SizeOfOptionalHeader and pe_load reads sections[i]
//       blind, so a bogus SizeOfOptionalHeader / oversized NumberOfSections
//       OOB-reads the section table. Rust bounds the WHOLE table before reading.
//   (2) the per-section file-bounds PointerToRawData+SizeOfRawData > size is
//       computed in uint32 - an overflowing sum wraps below `size` and passes,
//       then the pe_load memcpy OOB-READs the file buffer. Rust uses widened u64.
//   (3) the per-section virtual bound VirtualAddress+SizeOfRawData > SizeOfImage
//       is likewise uint32 - an overflowing sum passes, then the pe_load memcpy
//       OOB-WRITES past the image allocation (the strongest, ELF-p_filesz-class).
// HONEST: pe_load has NO live caller today (Win32 #288 is unimplemented), so
// these are LATENT (real code, provable offline under ASan on an exact-size heap
// buffer) rather than reachable-from-boot. The seam pre-confines them for #288.
//
// PeInfo is #[repr(C)]; sizeof asserted == 32 in pe.c so the FFI struct can
// never drift. Reuses the bounds-checked LE readers elf_rd_u16/elf_rd_u32.
// ============================================================================

use crate::common::{elf_rd_u16, elf_rd_u32};

const PE_SUCCESS_RS: i32 = 0;
const PE_ERR_NULL_PTR_RS: i32 = -1;
const PE_ERR_TOO_SMALL_RS: i32 = -2;
const PE_ERR_BAD_DOS_MAGIC_RS: i32 = -3;
const PE_ERR_BAD_PE_MAGIC_RS: i32 = -4;
const PE_ERR_NOT_32BIT_RS: i32 = -5;
const PE_ERR_NOT_EXECUTABLE_RS: i32 = -6;
const PE_ERR_NO_SECTIONS_RS: i32 = -7;
const PE_ERR_LOAD_FAILED_RS: i32 = -9;

const MZ_SIGNATURE_RS: u16 = 0x5A4D;
const PE_SIGNATURE_RS: u32 = 0x0000_4550;
const IMAGE_FILE_MACHINE_I386_RS: u16 = 0x014C;
const IMAGE_NT_OPTIONAL_HDR32_MAGIC_RS: u16 = 0x010B;
const IMAGE_FILE_EXECUTABLE_IMAGE_RS: u16 = 0x0002;
const IMAGE_FILE_DLL_RS: u16 = 0x2000;
const PE_DOS_HEADER_SIZE: u64 = 64;
const PE_NT_HEADERS32_SIZE: u64 = 248;
const PE_SECTION_HEADER_SIZE: u64 = 40;

// #[repr(C)] mirror of pe_info_t (exec/pe.h). Layout asserted == 32 bytes on the
// C side (_Static_assert in pe.c) so this FFI struct can never silently drift.
#[repr(C)]
pub struct PeInfo {
    pub e_lfanew: u32,
    pub machine: u16,
    pub num_sections: u16,
    pub size_of_optional_header: u16,
    pub opt_magic: u16,
    pub characteristics: u16,
    pub is_dll: u16,
    pub entry_point: u32,
    pub image_base: u32,
    pub section_alignment: u32,
    pub size_of_image: u32,
}

/// Rust port of exec/pe.c PE32 pre-map validation. Returns PE_SUCCESS(0) or a
/// negative PE_ERR_* code identical to the C. PURE: never maps/allocates/copies.
/// Every read is a bounds-checked index into a slice of exactly `size` bytes.
#[no_mangle]
pub extern "C" fn pe_validate_full_rs(buf: *const u8, size: u64, out: *mut PeInfo) -> i32 {
    // Matches C `if (!pe_data) return PE_ERR_NULL_PTR;`.
    if buf.is_null() {
        return PE_ERR_NULL_PTR_RS;
    }
    // SAFETY: out, when non-null, is a valid writable PeInfo* per the C ABI
    // contract (pe.c passes &stack_local). POD-write only.
    if !out.is_null() {
        unsafe {
            *out = PeInfo {
                e_lfanew: 0,
                machine: 0,
                num_sections: 0,
                size_of_optional_header: 0,
                opt_magic: 0,
                characteristics: 0,
                is_dll: 0,
                entry_point: 0,
                image_base: 0,
                section_alignment: 0,
                size_of_image: 0,
            };
        }
    }
    // Minimum size for the DOS header.
    if size < PE_DOS_HEADER_SIZE {
        return PE_ERR_TOO_SMALL_RS;
    }
    // SAFETY: buf non-null and, per the C ABI contract from exec/pe.c, points to
    // at least `size` readable bytes (the on-disk image buffer). We build a slice
    // spanning EXACTLY those `size` bytes; every access below is a bounds-checked
    // index into it (elf_rd_* return None past the end), so no read can leave the
    // buffer even if an attacker-controlled header field is nonsense.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(buf, size as usize) };

    // DOS signature "MZ".
    let e_magic = elf_rd_u16(s, 0).unwrap_or(0);
    if e_magic != MZ_SIGNATURE_RS {
        return PE_ERR_BAD_DOS_MAGIC_RS;
    }
    let e_lfanew = elf_rd_u32(s, 60).unwrap_or(0);
    // Whole NT header block must fit (matches C: e_lfanew + sizeof(NT32) > size).
    // 64-bit math: e_lfanew <= 2^32, no overflow.
    if (e_lfanew as u64) + PE_NT_HEADERS32_SIZE > size {
        return PE_ERR_TOO_SMALL_RS;
    }
    let l = e_lfanew as u64;

    // PE signature "PE\0\0".
    let signature = match elf_rd_u32(s, l) {
        Some(v) => v,
        None => return PE_ERR_TOO_SMALL_RS,
    };
    if signature != PE_SIGNATURE_RS {
        return PE_ERR_BAD_PE_MAGIC_RS;
    }
    // COFF Machine (i386 only) then OptionalHeader.Magic (PE32 only) - both map
    // to PE_ERR_NOT_32BIT exactly like the C, in the same order.
    let machine = elf_rd_u16(s, l + 4).unwrap_or(0);
    if machine != IMAGE_FILE_MACHINE_I386_RS {
        return PE_ERR_NOT_32BIT_RS;
    }
    let opt_magic = elf_rd_u16(s, l + 24).unwrap_or(0);
    if opt_magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC_RS {
        return PE_ERR_NOT_32BIT_RS;
    }
    let characteristics = elf_rd_u16(s, l + 22).unwrap_or(0);
    if (characteristics & IMAGE_FILE_EXECUTABLE_IMAGE_RS) == 0 {
        return PE_ERR_NOT_EXECUTABLE_RS;
    }
    let num_sections = elf_rd_u16(s, l + 6).unwrap_or(0);
    if num_sections == 0 {
        return PE_ERR_NO_SECTIONS_RS;
    }

    let size_of_optional_header = elf_rd_u16(s, l + 20).unwrap_or(0);
    let entry_point = elf_rd_u32(s, l + 40).unwrap_or(0);
    let image_base = elf_rd_u32(s, l + 52).unwrap_or(0);
    let section_alignment = elf_rd_u32(s, l + 56).unwrap_or(0);
    let size_of_image = elf_rd_u32(s, l + 80).unwrap_or(0);

    // CONFINEMENT (1): the section table lives at &OptionalHeader +
    // SizeOfOptionalHeader = l + 24 + size_of_optional_header. Bound the WHOLE
    // table (num_sections * 40) inside the image BEFORE any section read - the C
    // omits this. num_sections <= 65535, *40 < 2^32, no overflow.
    let sect_base = l + 24 + (size_of_optional_header as u64);
    let tbl = (num_sections as u64) * PE_SECTION_HEADER_SIZE;
    if sect_base > size || tbl > size - sect_base {
        return PE_ERR_LOAD_FAILED_RS;
    }

    // Per-section raw/virtual bounds (matches pe_load's checks) computed with
    // WIDENED (u64) arithmetic - no uint32 overflow bypass. CONFINEMENT (2)+(3):
    // reject a section whose file raw range leaves the image (would OOB the
    // pe_load memcpy READ) or whose virtual range leaves SizeOfImage (would OOB
    // the memcpy WRITE). Sections with no raw data are skipped, exactly like C.
    let mut i: u64 = 0;
    while i < num_sections as u64 {
        let so = sect_base + i * PE_SECTION_HEADER_SIZE;
        let virtual_address = match elf_rd_u32(s, so + 12) {
            Some(v) => v,
            None => return PE_ERR_LOAD_FAILED_RS,
        };
        let size_of_raw_data = match elf_rd_u32(s, so + 16) {
            Some(v) => v,
            None => return PE_ERR_LOAD_FAILED_RS,
        };
        let pointer_to_raw_data = match elf_rd_u32(s, so + 20) {
            Some(v) => v,
            None => return PE_ERR_LOAD_FAILED_RS,
        };
        if size_of_raw_data == 0 || pointer_to_raw_data == 0 {
            i += 1;
            continue;
        }
        if (pointer_to_raw_data as u64) + (size_of_raw_data as u64) > size {
            return PE_ERR_LOAD_FAILED_RS;
        }
        if (virtual_address as u64) + (size_of_raw_data as u64) > size_of_image as u64 {
            return PE_ERR_LOAD_FAILED_RS;
        }
        i += 1;
    }

    if !out.is_null() {
        // SAFETY: out non-null + valid writable per the C ABI contract; POD write.
        unsafe {
            *out = PeInfo {
                e_lfanew,
                machine,
                num_sections,
                size_of_optional_header,
                opt_magic,
                characteristics,
                is_dll: if (characteristics & IMAGE_FILE_DLL_RS) != 0 { 1 } else { 0 },
                entry_point,
                image_base,
                section_alignment,
                size_of_image,
            };
        }
    }
    PE_SUCCESS_RS
}
