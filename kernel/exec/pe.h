// pe.h - PE (Portable Executable) Loader for Windows executables
// Supports loading 32-bit Windows PE files on MayteraOS
#ifndef PE_H
#define PE_H

#include "../types.h"

// ============================================================================
// DOS Header (MZ)
// ============================================================================

#define MZ_SIGNATURE    0x5A4D  // "MZ"

typedef struct {
    uint16_t    e_magic;        // Magic number (MZ_SIGNATURE)
    uint16_t    e_cblp;         // Bytes on last page of file
    uint16_t    e_cp;           // Pages in file
    uint16_t    e_crlc;         // Relocations
    uint16_t    e_cparhdr;      // Size of header in paragraphs
    uint16_t    e_minalloc;     // Minimum extra paragraphs needed
    uint16_t    e_maxalloc;     // Maximum extra paragraphs needed
    uint16_t    e_ss;           // Initial (relative) SS value
    uint16_t    e_sp;           // Initial SP value
    uint16_t    e_csum;         // Checksum
    uint16_t    e_ip;           // Initial IP value
    uint16_t    e_cs;           // Initial (relative) CS value
    uint16_t    e_lfarlc;       // File address of relocation table
    uint16_t    e_ovno;         // Overlay number
    uint16_t    e_res[4];       // Reserved words
    uint16_t    e_oemid;        // OEM identifier
    uint16_t    e_oeminfo;      // OEM information
    uint16_t    e_res2[10];     // Reserved words
    uint32_t    e_lfanew;       // File address of new exe header (PE)
} __attribute__((packed)) IMAGE_DOS_HEADER;

// ============================================================================
// PE Signature
// ============================================================================

#define PE_SIGNATURE    0x00004550  // "PE\0\0"

// ============================================================================
// COFF File Header
// ============================================================================

#define IMAGE_FILE_MACHINE_I386     0x014C  // x86
#define IMAGE_FILE_MACHINE_AMD64    0x8664  // x64

// File characteristics
#define IMAGE_FILE_EXECUTABLE_IMAGE     0x0002
#define IMAGE_FILE_LARGE_ADDRESS_AWARE  0x0020
#define IMAGE_FILE_32BIT_MACHINE        0x0100
#define IMAGE_FILE_DLL                  0x2000

typedef struct {
    uint16_t    Machine;            // Target machine type
    uint16_t    NumberOfSections;   // Number of sections
    uint32_t    TimeDateStamp;      // Time stamp
    uint32_t    PointerToSymbolTable;   // Symbol table offset
    uint32_t    NumberOfSymbols;    // Number of symbols
    uint16_t    SizeOfOptionalHeader;   // Size of optional header
    uint16_t    Characteristics;    // File characteristics
} __attribute__((packed)) IMAGE_FILE_HEADER;

// ============================================================================
// Optional Header (PE32)
// ============================================================================

#define IMAGE_NT_OPTIONAL_HDR32_MAGIC   0x10B   // PE32
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC   0x20B   // PE32+

// Subsystem values
#define IMAGE_SUBSYSTEM_UNKNOWN         0
#define IMAGE_SUBSYSTEM_NATIVE          1
#define IMAGE_SUBSYSTEM_WINDOWS_GUI     2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI     3

// DLL characteristics
#define IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE   0x0040  // ASLR
#define IMAGE_DLLCHARACTERISTICS_NX_COMPAT      0x0100  // DEP

// Number of directory entries
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES    16

typedef struct {
    uint32_t    VirtualAddress;
    uint32_t    Size;
} __attribute__((packed)) IMAGE_DATA_DIRECTORY;

typedef struct {
    uint16_t    Magic;              // Magic number (0x10B for PE32)
    uint8_t     MajorLinkerVersion;
    uint8_t     MinorLinkerVersion;
    uint32_t    SizeOfCode;
    uint32_t    SizeOfInitializedData;
    uint32_t    SizeOfUninitializedData;
    uint32_t    AddressOfEntryPoint;    // RVA of entry point
    uint32_t    BaseOfCode;
    uint32_t    BaseOfData;
    // NT additional fields
    uint32_t    ImageBase;          // Preferred load address
    uint32_t    SectionAlignment;   // Section alignment in memory
    uint32_t    FileAlignment;      // Section alignment in file
    uint16_t    MajorOperatingSystemVersion;
    uint16_t    MinorOperatingSystemVersion;
    uint16_t    MajorImageVersion;
    uint16_t    MinorImageVersion;
    uint16_t    MajorSubsystemVersion;
    uint16_t    MinorSubsystemVersion;
    uint32_t    Win32VersionValue;
    uint32_t    SizeOfImage;        // Size of image when loaded
    uint32_t    SizeOfHeaders;      // Size of headers
    uint32_t    CheckSum;
    uint16_t    Subsystem;          // Subsystem type
    uint16_t    DllCharacteristics;
    uint32_t    SizeOfStackReserve;
    uint32_t    SizeOfStackCommit;
    uint32_t    SizeOfHeapReserve;
    uint32_t    SizeOfHeapCommit;
    uint32_t    LoaderFlags;
    uint32_t    NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} __attribute__((packed)) IMAGE_OPTIONAL_HEADER32;

// ============================================================================
// Section Header
// ============================================================================

#define IMAGE_SIZEOF_SHORT_NAME     8

// Section characteristics
#define IMAGE_SCN_CNT_CODE              0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA  0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_MEM_EXECUTE           0x20000000
#define IMAGE_SCN_MEM_READ              0x40000000
#define IMAGE_SCN_MEM_WRITE             0x80000000

typedef struct {
    char        Name[IMAGE_SIZEOF_SHORT_NAME];
    union {
        uint32_t    PhysicalAddress;
        uint32_t    VirtualSize;
    } Misc;
    uint32_t    VirtualAddress;         // RVA when loaded
    uint32_t    SizeOfRawData;          // Size on disk
    uint32_t    PointerToRawData;       // File offset
    uint32_t    PointerToRelocations;
    uint32_t    PointerToLinenumbers;
    uint16_t    NumberOfRelocations;
    uint16_t    NumberOfLinenumbers;
    uint32_t    Characteristics;
} __attribute__((packed)) IMAGE_SECTION_HEADER;

// ============================================================================
// NT Headers (combined)
// ============================================================================

typedef struct {
    uint32_t            Signature;      // PE signature
    IMAGE_FILE_HEADER   FileHeader;
    IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} __attribute__((packed)) IMAGE_NT_HEADERS32;

// ============================================================================
// Import Directory
// ============================================================================

typedef struct {
    uint32_t    OriginalFirstThunk; // RVA to INT
    uint32_t    TimeDateStamp;
    uint32_t    ForwarderChain;
    uint32_t    Name;               // RVA to DLL name
    uint32_t    FirstThunk;         // RVA to IAT
} __attribute__((packed)) IMAGE_IMPORT_DESCRIPTOR;

// Data directory indices
#define IMAGE_DIRECTORY_ENTRY_EXPORT        0
#define IMAGE_DIRECTORY_ENTRY_IMPORT        1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE      2
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION     3
#define IMAGE_DIRECTORY_ENTRY_SECURITY      4
#define IMAGE_DIRECTORY_ENTRY_BASERELOC     5
#define IMAGE_DIRECTORY_ENTRY_DEBUG         6
#define IMAGE_DIRECTORY_ENTRY_TLS           9
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG   10
#define IMAGE_DIRECTORY_ENTRY_IAT           12

// ============================================================================
// PE Loader Error Codes
// ============================================================================

#define PE_SUCCESS              0
#define PE_ERR_NULL_PTR        -1
#define PE_ERR_TOO_SMALL       -2
#define PE_ERR_BAD_DOS_MAGIC   -3
#define PE_ERR_BAD_PE_MAGIC    -4
#define PE_ERR_NOT_32BIT       -5
#define PE_ERR_NOT_EXECUTABLE  -6
#define PE_ERR_NO_SECTIONS     -7
#define PE_ERR_ALLOC_FAILED    -8
#define PE_ERR_LOAD_FAILED     -9
#define PE_ERR_UNSUPPORTED     -10

// ============================================================================
// PE Loader Structures
// ============================================================================

// Loaded PE information
typedef struct {
    void        *image_base;        // Allocated memory for image
    uint32_t    image_size;         // Size of loaded image
    uint32_t    entry_point;        // Entry point RVA
    uint32_t    preferred_base;     // Preferred load address
    uint16_t    subsystem;          // Subsystem type
    bool        is_dll;             // True if DLL
} PE_LoadResult;

// ============================================================================
// Parsed/validated PE header fields produced by pe_validate_full_c (verbatim
// reference) and pe_validate_full_rs (Rust, rustkern.rs). #[repr(C)] mirror is
// PeInfo in rustkern.rs; sizeof is asserted == 32 in pe.c so the FFI struct can
// never silently drift. Fields are the untrusted-header values the loader needs
// (offsets/sizes/RVAs) WITHOUT mapping or allocating anything.
// ============================================================================
typedef struct {
    uint32_t e_lfanew;                  // DOS header: PE header file offset
    uint16_t machine;                   // COFF FileHeader.Machine
    uint16_t num_sections;              // COFF FileHeader.NumberOfSections
    uint16_t size_of_optional_header;   // COFF FileHeader.SizeOfOptionalHeader
    uint16_t opt_magic;                 // OptionalHeader.Magic (PE32 / PE32+)
    uint16_t characteristics;           // FileHeader.Characteristics
    uint16_t is_dll;                    // (Characteristics & IMAGE_FILE_DLL) != 0
    uint32_t entry_point;               // OptionalHeader.AddressOfEntryPoint (RVA)
    uint32_t image_base;                // OptionalHeader.ImageBase
    uint32_t section_alignment;         // OptionalHeader.SectionAlignment
    uint32_t size_of_image;             // OptionalHeader.SizeOfImage
} pe_info_t;

// ============================================================================
// PE Loader API
// ============================================================================

/**
 * Validate a PE file
 *
 * @param pe_data   Pointer to PE file data
 * @param size      Size of the PE data
 * @return          PE_SUCCESS on success, negative error code on failure
 */
int pe_validate(const void *pe_data, uint32_t size);

// #404 Phase R pre-map validation seam. pe_validate_full_c = verbatim reference
// (DOS + PE-sig + COFF + OptionalHeader header checks of the original
// pe_validate(), same order + same PE_ERR_* codes, followed by the section-table
// raw/virtual-bounds walk lifted from pe_load()'s per-section checks, retaining
// the C's exact uint32 arithmetic + its absent section-table bounds check as the
// honest reference); pe_validate_full_rs (Rust) = the same, plus the confinement
// gates (slice of exactly `size` bytes, a section-table-fits bound, and widened
// per-section raw/virtual bounds). Both are PURE (no map/alloc/copy). The live
// pe_validate() routes to _rs under -DRUST_PE, else _c. pe_validate_full_c is
// also the differential reference.
int pe_validate_full_c(const uint8_t *buf, uint32_t size, pe_info_t *out);
int pe_validate_full_rs(const uint8_t *buf, uint64_t size, pe_info_t *out);
void pe_rust_selftest(void);

/**
 * Load a PE executable into memory
 *
 * @param pe_data   Pointer to PE file data
 * @param size      Size of the PE data
 * @param result    Output: load result information
 * @return          PE_SUCCESS on success, negative error code on failure
 */
int pe_load(void *pe_data, uint32_t size, PE_LoadResult *result);

/**
 * Unload a previously loaded PE image
 *
 * @param result    Pointer to load result from pe_load()
 */
void pe_unload(PE_LoadResult *result);

/**
 * Execute a loaded PE executable
 * Note: This requires Windows API emulation to work properly
 *
 * @param result    Pointer to loaded PE information
 * @return          Return code from executable
 */
int pe_execute(PE_LoadResult *result);

/**
 * Get error message for PE error code
 *
 * @param error     Error code from PE functions
 * @return          Pointer to error message string
 */
const char *pe_strerror(int error);

/**
 * Check if data starts with MZ header (DOS/PE executable)
 *
 * @param data      Pointer to file data
 * @param size      Size of data
 * @return          true if MZ header found
 */
bool pe_is_mz(const void *data, uint32_t size);

/**
 * Print PE header information (for debugging)
 *
 * @param pe_data   Pointer to PE file data
 */
void pe_print_info(const void *pe_data);

#endif // PE_H
