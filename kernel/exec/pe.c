// pe.c - PE (Portable Executable) Loader Implementation
// Loads 32-bit Windows PE executables for emulation
#include "pe.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"

// ============================================================================
// Error Messages
// ============================================================================

static const char *pe_error_messages[] = {
    "Success",                          // PE_SUCCESS
    "NULL pointer",                     // PE_ERR_NULL_PTR
    "File too small",                   // PE_ERR_TOO_SMALL
    "Invalid DOS signature (not MZ)",   // PE_ERR_BAD_DOS_MAGIC
    "Invalid PE signature",             // PE_ERR_BAD_PE_MAGIC
    "Not a 32-bit PE file",             // PE_ERR_NOT_32BIT
    "Not an executable",                // PE_ERR_NOT_EXECUTABLE
    "No sections found",                // PE_ERR_NO_SECTIONS
    "Memory allocation failed",         // PE_ERR_ALLOC_FAILED
    "Failed to load section",           // PE_ERR_LOAD_FAILED
    "Unsupported feature",              // PE_ERR_UNSUPPORTED
};

#define PE_ERROR_COUNT (sizeof(pe_error_messages) / sizeof(pe_error_messages[0]))

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Get DOS header from PE data
 */
static inline const IMAGE_DOS_HEADER *pe_get_dos_header(const void *pe_data) {
    return (const IMAGE_DOS_HEADER *)pe_data;
}

/**
 * Get NT headers from PE data
 */
static inline const IMAGE_NT_HEADERS32 *pe_get_nt_headers(const void *pe_data) {
    const IMAGE_DOS_HEADER *dos = pe_get_dos_header(pe_data);
    return (const IMAGE_NT_HEADERS32 *)((const uint8_t *)pe_data + dos->e_lfanew);
}

/**
 * Get first section header
 */
static inline const IMAGE_SECTION_HEADER *pe_get_sections(const void *pe_data) {
    const IMAGE_NT_HEADERS32 *nt = pe_get_nt_headers(pe_data);
    return (const IMAGE_SECTION_HEADER *)((const uint8_t *)&nt->OptionalHeader +
                                          nt->FileHeader.SizeOfOptionalHeader);
}

// ============================================================================
// PE Validation
// ============================================================================

bool pe_is_mz(const void *data, uint32_t size) {
    if (!data || size < 2) return false;
    const uint8_t *bytes = (const uint8_t *)data;
    return (bytes[0] == 'M' && bytes[1] == 'Z');
}

// ============================================================================
// #404 / Phase R: pre-map validation seam (pe_validate_full_c / _rs).
//
// FFI lock: PeInfo (rustkern.rs) mirrors pe_info_t byte-for-byte; the header
// struct sizes asserted below feed both the C reference AND the Rust constants
// (PE_NT_HEADERS32_SIZE=248, DOS=64, section header=40), so a packed-layout
// change can never silently drift the FFI or the Rust field offsets.
// ============================================================================
_Static_assert(sizeof(pe_info_t) == 32, "pe_info_t must be 32 bytes for the Rust FFI");
_Static_assert(sizeof(IMAGE_DOS_HEADER) == 64, "IMAGE_DOS_HEADER must be 64 bytes");
_Static_assert(sizeof(IMAGE_FILE_HEADER) == 20, "IMAGE_FILE_HEADER must be 20 bytes");
_Static_assert(sizeof(IMAGE_NT_HEADERS32) == 248, "IMAGE_NT_HEADERS32 must be 248 bytes");
_Static_assert(sizeof(IMAGE_SECTION_HEADER) == 40, "IMAGE_SECTION_HEADER must be 40 bytes");

// pe_validate_full_c is the reference. Its HEADER checks are VERBATIM: the DOS +
// PE-sig + COFF + OptionalHeader checks of the original pe_validate(), same order
// + same PE_ERR_* codes (0 drift over 1M vectors vs the pre-port original, 3-way
// audited 2026-07-16). Its SECTION walk is NOT verbatim and is not claimed to be:
// it is pe_load()'s per-section bounds, arithmetic-identical but REJECTING where
// pe_load() SKIPS. So pe_validate()'s CONTRACT CHANGED at b809, from
// header-only/accept to header+sections/reject; this is a deliberate, disclosed
// hardening, not drift, and it is UNREACHABLE today (pe_load() has zero callers,
// Win32 #288 unimplemented). Whoever wires #288 must know that an image with an
// out-of-bounds section which the pre-port loader would have loaded (skipping
// that section) is now REJECTED outright.
// It is PURE (no map/alloc/copy) and SILENT (no
// kprintf) so it can run in the boot differential without flooding serial. It
// HONESTLY retains the C loader's gaps on the untrusted header:
//   (1) NO section-table bounds check - pe_get_sections() locates the table at
//       &OptionalHeader + SizeOfOptionalHeader and this loop reads sections[i]
//       exactly as pe_load() does, so a bogus SizeOfOptionalHeader or oversized
//       NumberOfSections OOB-reads the section table;
//   (2) the per-section raw bound PointerToRawData+SizeOfRawData > size and the
//       virtual bound VirtualAddress+SizeOfRawData > SizeOfImage are computed in
//       the C loader's uint32 arithmetic, so an overflowing sum wraps below the
//       limit and PASSES - the same bypass that would OOB the pe_load() memcpy.
// pe_load() SKIPS a section that fails those bounds; this seam REJECTS it, so the
// seam is strictly stricter than the live loader (the security intent) while the
// arithmetic stays byte-identical to the C for the differential.
// pe_validate_full_rs (rustkern.rs) is the same validation with those gaps CLOSED
// by construction (slice of exactly `size` bytes, a section-table-fits bound, and
// widened per-section raw/virtual bounds). See rustkern.rs for the write-up.
int pe_validate_full_c(const uint8_t *buf, uint32_t size, pe_info_t *out) {
    if (out) {
        out->e_lfanew = 0; out->machine = 0; out->num_sections = 0;
        out->size_of_optional_header = 0; out->opt_magic = 0;
        out->characteristics = 0; out->is_dll = 0; out->entry_point = 0;
        out->image_base = 0; out->section_alignment = 0; out->size_of_image = 0;
    }
    const void *pe_data = (const void *)buf;
    if (!pe_data) return PE_ERR_NULL_PTR;
    if (size < sizeof(IMAGE_DOS_HEADER)) return PE_ERR_TOO_SMALL;

    const IMAGE_DOS_HEADER *dos = pe_get_dos_header(pe_data);
    if (dos->e_magic != MZ_SIGNATURE) return PE_ERR_BAD_DOS_MAGIC;
    if (dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > size) return PE_ERR_TOO_SMALL;

    const IMAGE_NT_HEADERS32 *nt = pe_get_nt_headers(pe_data);
    if (nt->Signature != PE_SIGNATURE) return PE_ERR_BAD_PE_MAGIC;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return PE_ERR_NOT_32BIT;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return PE_ERR_NOT_32BIT;
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE)) return PE_ERR_NOT_EXECUTABLE;
    if (nt->FileHeader.NumberOfSections == 0) return PE_ERR_NO_SECTIONS;

    uint32_t size_of_image = nt->OptionalHeader.SizeOfImage;

    // Section-table raw/virtual bounds walk: pe_load()'s per-section bounds,
    // retained ARITHMETIC-IDENTICAL (incl. the absent table-bounds check + the
    // uint32 overflow bypass) but REJECTING where pe_load() SKIPS (see the note
    // above). NOT "verbatim": the `continue` -> `return PE_ERR_LOAD_FAILED`
    // change is the deliberate stricter-than-the-loader design decision, and
    // calling this walk verbatim contradicted that note two lines up. Wording
    // corrected 2026-07-16 by the #404 3-way drift audit; the header checks above
    // ARE exact (0 drift / 1M vectors vs the pre-port original).
    const IMAGE_SECTION_HEADER *sections = pe_get_sections(pe_data);
    for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *sec = &sections[i];
        if (sec->SizeOfRawData == 0 || sec->PointerToRawData == 0) continue;
        if (sec->PointerToRawData + sec->SizeOfRawData > size) return PE_ERR_LOAD_FAILED;
        if (sec->VirtualAddress + sec->SizeOfRawData > size_of_image) return PE_ERR_LOAD_FAILED;
    }

    if (out) {
        out->e_lfanew = dos->e_lfanew;
        out->machine = nt->FileHeader.Machine;
        out->num_sections = nt->FileHeader.NumberOfSections;
        out->size_of_optional_header = nt->FileHeader.SizeOfOptionalHeader;
        out->opt_magic = nt->OptionalHeader.Magic;
        out->characteristics = nt->FileHeader.Characteristics;
        out->is_dll = (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) ? 1 : 0;
        out->entry_point = nt->OptionalHeader.AddressOfEntryPoint;
        out->image_base = nt->OptionalHeader.ImageBase;
        out->section_alignment = nt->OptionalHeader.SectionAlignment;
        out->size_of_image = nt->OptionalHeader.SizeOfImage;
    }
    return PE_SUCCESS;
}

// Live dispatcher. Keeps the original int(const void*,uint32_t) signature every
// caller uses (pe_load). With -DRUST_PE (set in the Makefile) the untrusted-image
// validation runs in Rust (pe_validate_full_rs, rustkern.rs) - which CONFINES the
// section-table OOB-read + the uint32 raw/virtual overflow bypass before pe_load()
// allocates/copies anything; drop the flag + rebuild to roll straight back to the
// verbatim C. A one-line summary preserves the original diagnostics.
int pe_validate(const void *pe_data, uint32_t size) {
    pe_info_t info;
#ifdef RUST_PE
    int r = pe_validate_full_rs((const uint8_t *)pe_data, (uint64_t)size, &info);
#else
    int r = pe_validate_full_c((const uint8_t *)pe_data, size, &info);
#endif
    if (r == PE_SUCCESS) {
        kprintf("[PE] Validation passed: %s, machine 0x%04X, %u sections\n",
                info.is_dll ? "DLL" : "executable", info.machine, info.num_sections);
    } else {
        kprintf("[PE] Validation failed: %s (%d)\n", pe_strerror(r), r);
    }
    return r;
}

// ============================================================================
// PE Loading
// ============================================================================

int pe_load(void *pe_data, uint32_t size, PE_LoadResult *result) {
    if (!pe_data || !result) {
        return PE_ERR_NULL_PTR;
    }

    // Validate the PE file
    int err = pe_validate(pe_data, size);
    if (err != PE_SUCCESS) {
        return err;
    }

    // Get headers
    const IMAGE_NT_HEADERS32 *nt = pe_get_nt_headers(pe_data);
    const IMAGE_SECTION_HEADER *sections = pe_get_sections(pe_data);

    // Initialize result
    memset(result, 0, sizeof(PE_LoadResult));
    result->entry_point = nt->OptionalHeader.AddressOfEntryPoint;
    result->preferred_base = nt->OptionalHeader.ImageBase;
    result->image_size = nt->OptionalHeader.SizeOfImage;
    result->subsystem = nt->OptionalHeader.Subsystem;
    result->is_dll = (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;

    kprintf("[PE] Loading PE executable:\n");
    kprintf("[PE]   Preferred base: 0x%08X\n", result->preferred_base);
    kprintf("[PE]   Image size: %u bytes\n", result->image_size);
    kprintf("[PE]   Entry point: 0x%08X\n", result->entry_point);
    kprintf("[PE]   Sections: %u\n", nt->FileHeader.NumberOfSections);
    kprintf("[PE]   Subsystem: %s\n",
            result->subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI ? "GUI" :
            result->subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI ? "Console" : "Other");

    // Allocate memory for the image
    // We allocate at a fixed address for now (PE emulation area)
    result->image_base = kmalloc(result->image_size);
    if (!result->image_base) {
        kprintf("[PE] Failed to allocate %u bytes for image\n", result->image_size);
        return PE_ERR_ALLOC_FAILED;
    }

    // Clear the image memory
    memset(result->image_base, 0, result->image_size);

    // Copy headers
    uint32_t header_size = nt->OptionalHeader.SizeOfHeaders;
    if (header_size > size) {
        header_size = size;
    }
    memcpy(result->image_base, pe_data, header_size);

    // Copy sections
    for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *sec = &sections[i];

        // Get section name (may not be null-terminated)
        char name[9] = {0};
        memcpy(name, sec->Name, 8);

        kprintf("[PE]   Section %u: %-8s VA=0x%08X Size=%u Raw=0x%08X\n",
                i, name, sec->VirtualAddress, sec->Misc.VirtualSize,
                sec->PointerToRawData);

        // Skip if no data to copy
        if (sec->SizeOfRawData == 0 || sec->PointerToRawData == 0) {
            continue;
        }

        // Validate offsets
        if (sec->PointerToRawData + sec->SizeOfRawData > size) {
            kprintf("[PE] Section %s data exceeds file size\n", name);
            continue;
        }

        if (sec->VirtualAddress + sec->SizeOfRawData > result->image_size) {
            kprintf("[PE] Section %s exceeds image size\n", name);
            continue;
        }

        // Copy section data
        uint8_t *dest = (uint8_t *)result->image_base + sec->VirtualAddress;
        const uint8_t *src = (const uint8_t *)pe_data + sec->PointerToRawData;
        uint32_t copy_size = sec->SizeOfRawData;
        if (sec->Misc.VirtualSize < copy_size) {
            copy_size = sec->Misc.VirtualSize;
        }
        memcpy(dest, src, copy_size);
    }

    kprintf("[PE] Image loaded at 0x%p\n", result->image_base);
    return PE_SUCCESS;
}

void pe_unload(PE_LoadResult *result) {
    if (!result) return;

    if (result->image_base) {
        kfree(result->image_base);
        result->image_base = NULL;
    }

    result->image_size = 0;
}

// ============================================================================
// PE Execution (requires Windows API emulation)
// ============================================================================

int pe_execute(PE_LoadResult *result) {
    if (!result || !result->image_base) {
        return -1;
    }

    kprintf("[PE] Execution requested for PE at 0x%p\n", result->image_base);
    kprintf("[PE] Entry point offset: 0x%08X\n", result->entry_point);

    // Calculate actual entry point address
    uint8_t *entry = (uint8_t *)result->image_base + result->entry_point;
    kprintf("[PE] Actual entry point: 0x%p\n", entry);

    // Note: Actually executing requires:
    // 1. Windows API emulation (kernel32.dll, user32.dll, etc.)
    // 2. PE relocation processing
    // 3. Import table resolution
    // 4. Proper 32-bit execution environment

    kprintf("[PE] WARNING: PE execution not fully implemented\n");
    kprintf("[PE] This PE file requires Windows API emulation\n");

    // For now, just analyze what the PE needs
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)
        ((uint8_t *)result->image_base +
         ((IMAGE_DOS_HEADER *)result->image_base)->e_lfanew);

    // Check imports
    IMAGE_DATA_DIRECTORY *import_dir =
        (IMAGE_DATA_DIRECTORY *)&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (import_dir->VirtualAddress && import_dir->Size) {
        kprintf("[PE] Import directory at RVA 0x%08X (size %u)\n",
                import_dir->VirtualAddress, import_dir->Size);

        IMAGE_IMPORT_DESCRIPTOR *imports = (IMAGE_IMPORT_DESCRIPTOR *)
            ((uint8_t *)result->image_base + import_dir->VirtualAddress);

        kprintf("[PE] Required DLLs:\n");
        while (imports->Name != 0) {
            const char *dll_name = (const char *)
                ((uint8_t *)result->image_base + imports->Name);
            kprintf("[PE]   - %s\n", dll_name);
            imports++;
        }
    }

    return -1;  // Not yet implemented
}

// ============================================================================
// Utility Functions
// ============================================================================

const char *pe_strerror(int error) {
    if (error >= 0) {
        return pe_error_messages[0];  // Success
    }
    int index = -error;
    if ((uint32_t)index < PE_ERROR_COUNT) {
        return pe_error_messages[index];
    }
    return "Unknown error";
}

void pe_print_info(const void *pe_data) {
    if (!pe_data) {
        kprintf("[PE] NULL data\n");
        return;
    }

    const IMAGE_DOS_HEADER *dos = pe_get_dos_header(pe_data);
    if (dos->e_magic != MZ_SIGNATURE) {
        kprintf("[PE] Not a valid PE file (no MZ signature)\n");
        return;
    }

    const IMAGE_NT_HEADERS32 *nt = pe_get_nt_headers(pe_data);
    if (nt->Signature != PE_SIGNATURE) {
        kprintf("[PE] DOS executable (no PE header)\n");
        kprintf("[PE] This is likely a real-mode DOS program\n");
        return;
    }

    kprintf("\n=== PE File Information ===\n");
    kprintf("DOS Header:\n");
    kprintf("  e_magic:    0x%04X (MZ)\n", dos->e_magic);
    kprintf("  e_lfanew:   0x%08X (PE header offset)\n", dos->e_lfanew);

    kprintf("\nPE Header:\n");
    kprintf("  Signature:  0x%08X (PE\\0\\0)\n", nt->Signature);
    kprintf("  Machine:    0x%04X (%s)\n", nt->FileHeader.Machine,
            nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 ? "i386" :
            nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 ? "x64" : "Other");
    kprintf("  Sections:   %u\n", nt->FileHeader.NumberOfSections);
    kprintf("  Timestamp:  0x%08X\n", nt->FileHeader.TimeDateStamp);
    kprintf("  Flags:      0x%04X\n", nt->FileHeader.Characteristics);

    if (nt->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE)
        kprintf("              - Executable\n");
    if (nt->FileHeader.Characteristics & IMAGE_FILE_DLL)
        kprintf("              - DLL\n");
    if (nt->FileHeader.Characteristics & IMAGE_FILE_32BIT_MACHINE)
        kprintf("              - 32-bit\n");

    kprintf("\nOptional Header:\n");
    kprintf("  Magic:          0x%04X (%s)\n", nt->OptionalHeader.Magic,
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC ? "PE32" :
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? "PE32+" : "Unknown");
    kprintf("  Entry Point:    0x%08X\n", nt->OptionalHeader.AddressOfEntryPoint);
    kprintf("  Image Base:     0x%08X\n", nt->OptionalHeader.ImageBase);
    kprintf("  Image Size:     %u bytes\n", nt->OptionalHeader.SizeOfImage);
    kprintf("  Section Align:  0x%08X\n", nt->OptionalHeader.SectionAlignment);
    kprintf("  File Align:     0x%08X\n", nt->OptionalHeader.FileAlignment);
    kprintf("  Subsystem:      %u (%s)\n", nt->OptionalHeader.Subsystem,
            nt->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI ? "GUI" :
            nt->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI ? "Console" :
            nt->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_NATIVE ? "Native" : "Other");

    // Print sections
    const IMAGE_SECTION_HEADER *sections = pe_get_sections(pe_data);
    kprintf("\nSections:\n");
    for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[9] = {0};
        memcpy(name, sections[i].Name, 8);
        kprintf("  [%u] %-8s VA=0x%08X Size=0x%08X Flags=0x%08X\n",
                i, name, sections[i].VirtualAddress,
                sections[i].Misc.VirtualSize, sections[i].Characteristics);
    }
    kprintf("\n");
}

// ============================================================================
// #404 / Phase R boot-time self-test: prove pe_validate_full_rs (Rust, live
// under -DRUST_PE) == pe_validate_full_c (verbatim reference) on the agreement
// domain (well-formed synthetic PE32 images + shared DOS/PE/COFF/section reject
// mutations), characterize the SECURITY divergence classes HONESTLY (the C's
// latent OOBs vs the Rust confinement), and micro-benchmark both. LIGHT (#426,
// bounded, runs once): ~256 differential vectors + a small security sweep + a
// ~5k-iter RDTSC bench. Heavy work (2M-vector differential + ASan/UBSan) is the
// OFFLINE pre-flight. One [RUST-DIFF] pe, one [RUST-SEC] pe, one [RUST-PERF] pe.
//
// SAFETY of running the C reference over MALFORMED vectors at boot: all vectors
// live in a 4096-byte static buffer, zeroed before each build, and the logical
// `size` passed is always <= 1024. The security-sweep table-overrun class first
// zeroes buf[312..] so the over-read section headers are all-zero (SizeOfRawData
// ==0 -> skipped by the C -> ACCEPT), so a bounded over-read stays inside the
// backing buffer (never a live OOB); the uint32-overflow classes only make the C
// ACCEPT (its own reads stay in-buffer; the OOB is in pe_load()'s memcpy, which
// this self-test never calls). pe_load has NO live callers today (Win32 #288 is
// unimplemented), so these C OOBs are LATENT, not reachable-from-boot.
// ============================================================================

static uint32_t pediff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static inline uint64_t pe_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static int pe_info_eq(int rc_a, const pe_info_t *a, int rc_b, const pe_info_t *b) {
    if (rc_a != rc_b) return 1;
    if (rc_a != PE_SUCCESS) return 0;   // both rejected identically: fields N/A
    return !(a->e_lfanew == b->e_lfanew && a->machine == b->machine &&
             a->num_sections == b->num_sections &&
             a->size_of_optional_header == b->size_of_optional_header &&
             a->opt_magic == b->opt_magic && a->characteristics == b->characteristics &&
             a->is_dll == b->is_dll && a->entry_point == b->entry_point &&
             a->image_base == b->image_base &&
             a->section_alignment == b->section_alignment &&
             a->size_of_image == b->size_of_image);
}

// Build a well-formed PE32 image (DOS header + NT headers @ offset 64 + N
// in-bounds sections) into buf. Returns the logical length. Structurally-real
// PE32 header shape (identical field layout to a genuine 32-bit .exe/.dll).
static uint32_t pe_build_wellformed(uint8_t *buf, uint32_t cap, uint32_t *seed) {
    for (uint32_t i = 0; i < cap; i++) buf[i] = 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)buf;
    dos->e_magic = MZ_SIGNATURE;
    dos->e_lfanew = 64;

    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(buf + 64);
    nt->Signature = PE_SIGNATURE;
    uint16_t nsec = 1 + (uint16_t)(pediff_rng(seed) % 3);   // 1..3
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = nsec;
    nt->FileHeader.SizeOfOptionalHeader = (uint16_t)sizeof(IMAGE_OPTIONAL_HEADER32); // 224
    uint16_t chars = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE;
    if (pediff_rng(seed) & 1) chars |= IMAGE_FILE_DLL;
    nt->FileHeader.Characteristics = chars;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.AddressOfEntryPoint = 0x1000 + (pediff_rng(seed) % 0x800);
    nt->OptionalHeader.ImageBase = 0x00400000;
    nt->OptionalHeader.SectionAlignment = 0x1000;
    nt->OptionalHeader.FileAlignment = 0x200;
    nt->OptionalHeader.SizeOfImage = 0x00010000;   // 64 KiB
    nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;

    uint32_t sect_base = 64 + (uint32_t)sizeof(IMAGE_NT_HEADERS32);   // 312
    IMAGE_SECTION_HEADER *sec = (IMAGE_SECTION_HEADER *)(buf + sect_base);
    uint32_t raw_off = sect_base + (uint32_t)nsec * (uint32_t)sizeof(IMAGE_SECTION_HEADER);
    raw_off = (raw_off + 15u) & ~15u;   // align file raw-data start
    for (uint16_t i = 0; i < nsec; i++) {
        sec[i].Name[0] = '.'; sec[i].Name[1] = 's'; sec[i].Name[2] = (char)('0' + i);
        uint32_t rd = pediff_rng(seed) % 128;          // 0..127 (0 => skipped by both)
        sec[i].Misc.VirtualSize = rd;
        sec[i].VirtualAddress = 0x1000u * (uint32_t)(i + 1);
        sec[i].SizeOfRawData = rd;
        sec[i].PointerToRawData = rd ? raw_off : 0;
        sec[i].Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA;
        if (rd) raw_off += rd;
    }
    uint32_t tbl_end = sect_base + (uint32_t)nsec * 40u;
    return (raw_off > tbl_end) ? raw_off : tbl_end;
}

void pe_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    static uint8_t buf[4096];
    uint32_t seed = 0x504504e0u;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;

    // Force-reference the Rust symbol so its archive member is always linked
    // (matches the icmp/arp/dns/dhcp/url/elf pattern), regardless of -DRUST_PE.
    { pe_info_t t; pe_build_wellformed(buf, sizeof(buf), &seed);
      pe_validate_full_rs(buf, 512, &t); }

    // Part 1: agreement domain (~256 vectors: well-formed + shared DOS/PE/COFF/
    // OptionalHeader/section-table reject mutations). Both sides must agree
    // exactly (int + fields). All C reads stay inside buf on these classes.
    seed = 0x504504e0u;
    for (uint32_t iter = 0; iter < 256; iter++) {
        pe_info_t vc, vr;
        uint32_t len = pe_build_wellformed(buf, sizeof(buf), &seed);
        uint32_t plen = len;
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)buf;
        IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(buf + 64);
        IMAGE_SECTION_HEADER *sec = (IMAGE_SECTION_HEADER *)(buf + 64 + sizeof(IMAGE_NT_HEADERS32));
        uint32_t klass = pediff_rng(&seed) % 11;
        switch (klass) {
            case 0: break;                                                    // pristine well-formed
            case 1: buf[0] ^= 0xFF; break;                                    // bad MZ
            case 2: dos->e_lfanew = len + 100; break;                         // e_lfanew past EOF
            case 3: nt->Signature ^= 0xFFFFFFFFu; break;                      // bad PE signature
            case 4: nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64; break; // wrong machine
            case 5: nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC; break; // PE32+ -> NOT_32BIT
            case 6: nt->FileHeader.Characteristics &= ~IMAGE_FILE_EXECUTABLE_IMAGE; break; // not exec
            case 7: nt->FileHeader.NumberOfSections = 0; break;               // no sections
            case 8: plen = 100; break;                                        // truncated (< NT headers)
            case 9: plen = 32; break;                                         // < DOS header
            case 10: sec[0].SizeOfRawData = 64; sec[0].PointerToRawData = len; // section raw past EOF
                     nt->FileHeader.NumberOfSections = 1; break;
        }
        int rc = pe_validate_full_c(buf, plen, &vc);
        int rr = pe_validate_full_rs(buf, (uint64_t)plen, &vr);
        vectors++;
        if (pe_info_eq(rc, &vc, rr, &vr)) {
            mismatches++;
            if (first_bad < 0) first_bad = (int)iter;
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] pe: %u vectors, %u mismatches -> %s\n", vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] pe: %u vectors, %u mismatches -> %s", vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] pe FIRST MISMATCH iter=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] pe FIRST MISMATCH iter=%d", first_bad);
    }

    // Part 2: SECURITY posture (HONEST). The verbatim C has three OOB classes on
    // the untrusted header/section-table; sweep each crafted class and count how
    // often C accepts/over-reads while Rust confines. LATENT (pe_load unwired).
    {
        uint32_t n_tbl = 0, c_tbl = 0;   // section table extends past size (OOB read of table)
        uint32_t n_raw = 0, c_raw = 0;   // PointerToRawData+SizeOfRawData uint32 overflow (memcpy OOB read)
        uint32_t n_va  = 0, c_va  = 0;   // VirtualAddress+SizeOfRawData uint32 overflow (memcpy OOB write)
        uint32_t s2 = 0x5ec0de22u;
        for (uint32_t r = 0; r < 300; r++) {
            pe_info_t vc, vr;
            uint32_t len = pe_build_wellformed(buf, sizeof(buf), &s2);
            if (len > 1024) len = 1024;
            IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(buf + 64);
            IMAGE_SECTION_HEADER *sec = (IMAGE_SECTION_HEADER *)(buf + 64 + sizeof(IMAGE_NT_HEADERS32));
            uint32_t klass = r % 3;
            int rc, rr;
            if (klass == 0) {
                // Zero everything from the section table onward so the over-read
                // section headers are all-zero (SizeOfRawData==0 -> C skips ->
                // ACCEPT), then declare a huge NumberOfSections whose table end
                // (312 + 40*big) stays < 4096 but far exceeds `len`.
                for (uint32_t z = 312; z < sizeof(buf); z++) buf[z] = 0;
                uint16_t big = 60 + (uint16_t)(pediff_rng(&s2) % 20);   // table end ~2712..3472
                nt->FileHeader.NumberOfSections = big;
                rc = pe_validate_full_c(buf, len, &vc);
                rr = pe_validate_full_rs(buf, (uint64_t)len, &vr);
                n_tbl++;
                if (rc == PE_SUCCESS && rr == PE_ERR_LOAD_FAILED) c_tbl++;
            } else if (klass == 1) {
                nt->FileHeader.NumberOfSections = 1;
                sec[0].PointerToRawData = 0xFFFFFF00u;   // + 0x200 wraps to 0x100 <= len
                sec[0].SizeOfRawData = 0x200;
                sec[0].Misc.VirtualSize = 0x200;
                sec[0].VirtualAddress = 0x1000;          // 0x1200 <= SizeOfImage(0x10000)
                rc = pe_validate_full_c(buf, len, &vc);
                rr = pe_validate_full_rs(buf, (uint64_t)len, &vr);
                n_raw++;
                if (rc == PE_SUCCESS && rr == PE_ERR_LOAD_FAILED) c_raw++;
            } else {
                nt->FileHeader.NumberOfSections = 1;
                sec[0].PointerToRawData = 312;           // in-bounds raw
                sec[0].SizeOfRawData = 0x100;
                sec[0].Misc.VirtualSize = 0x100;
                sec[0].VirtualAddress = 0xFFFFFF80u;     // + 0x100 wraps to 0x80 <= SizeOfImage
                if (len < 312 + 0x100) len = 312 + 0x100;
                rc = pe_validate_full_c(buf, len, &vc);
                rr = pe_validate_full_rs(buf, (uint64_t)len, &vr);
                n_va++;
                if (rc == PE_SUCCESS && rr == PE_ERR_LOAD_FAILED) c_va++;
            }
        }
        kprintf("[RUST-SEC] pe: LATENT OOBs in the C confined by Rust (pe_load unwired) - "
                "section-table-overrun(OOB read) %u/%u; raw uint32-overflow(memcpy OOB read) %u/%u; "
                "vaddr uint32-overflow(memcpy OOB write) %u/%u\n",
                c_tbl, n_tbl, c_raw, n_raw, c_va, n_va);
        bootlog_write("[RUST-SEC] pe: C sect-table-overrun %u/%u + raw-ovf %u/%u + vaddr-ovf %u/%u confined by Rust (LATENT: pe_load unwired; ASan-proven offline)",
                      c_tbl, n_tbl, c_raw, n_raw, c_va, n_va);
    }

    // Part 3: RDTSC micro-benchmark over a fixed well-formed PE. LIGHT: 5k.
    {
        const int iters = 5000;
        pe_info_t o;
        uint32_t s3 = 0x1234abcd;
        uint32_t len = pe_build_wellformed(buf, sizeof(buf), &s3);

        for (int i = 0; i < 300; i++) {
            pe_validate_full_c(buf, len, &o);
            pe_validate_full_rs(buf, (uint64_t)len, &o);
        }
        uint64_t t0 = pe_tsc_serialized();
        for (int i = 0; i < iters; i++) pe_validate_full_c(buf, len, &o);
        uint64_t t1 = pe_tsc_serialized();
        for (int i = 0; i < iters; i++) pe_validate_full_rs(buf, (uint64_t)len, &o);
        uint64_t t2 = pe_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] pe: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] pe: C=%llu RS=%llu ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
