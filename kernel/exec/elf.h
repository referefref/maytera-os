// elf.h - ELF64 Header Structures and Loader API for MayteraOS
#ifndef ELF_H
#define ELF_H

#include "../types.h"

// ============================================================================
// ELF64 Magic Number
// ============================================================================

#define ELF_MAGIC_0     0x7F
#define ELF_MAGIC_1     'E'
#define ELF_MAGIC_2     'L'
#define ELF_MAGIC_3     'F'

// ============================================================================
// ELF Identification (e_ident) indices
// ============================================================================

#define EI_MAG0         0       // Magic number byte 0
#define EI_MAG1         1       // Magic number byte 1
#define EI_MAG2         2       // Magic number byte 2
#define EI_MAG3         3       // Magic number byte 3
#define EI_CLASS        4       // File class (32/64-bit)
#define EI_DATA         5       // Data encoding (endianness)
#define EI_VERSION      6       // ELF version
#define EI_OSABI        7       // OS/ABI identification
#define EI_ABIVERSION   8       // ABI version
#define EI_PAD          9       // Start of padding bytes
#define EI_NIDENT       16      // Size of e_ident[]

// ============================================================================
// ELF Class (e_ident[EI_CLASS])
// ============================================================================

#define ELFCLASSNONE    0       // Invalid class
#define ELFCLASS32      1       // 32-bit objects
#define ELFCLASS64      2       // 64-bit objects

// ============================================================================
// ELF Data Encoding (e_ident[EI_DATA])
// ============================================================================

#define ELFDATANONE     0       // Invalid data encoding
#define ELFDATA2LSB     1       // Little-endian
#define ELFDATA2MSB     2       // Big-endian

// ============================================================================
// ELF Version (e_ident[EI_VERSION] and e_version)
// ============================================================================

#define EV_NONE         0       // Invalid version
#define EV_CURRENT      1       // Current version

// ============================================================================
// ELF OS/ABI (e_ident[EI_OSABI])
// ============================================================================

#define ELFOSABI_NONE       0   // UNIX System V ABI
#define ELFOSABI_SYSV       0   // Alias for UNIX System V
#define ELFOSABI_LINUX      3   // Linux
#define ELFOSABI_FREEBSD    9   // FreeBSD

// ============================================================================
// ELF File Types (e_type)
// ============================================================================

#define ET_NONE         0       // No file type
#define ET_REL          1       // Relocatable file
#define ET_EXEC         2       // Executable file
#define ET_DYN          3       // Shared object file (also PIE executables)
#define ET_CORE         4       // Core file

// ============================================================================
// ELF Machine Types (e_machine)
// ============================================================================

#define EM_NONE         0       // No machine
#define EM_386          3       // Intel 80386
#define EM_X86_64       62      // AMD x86-64 architecture

// ============================================================================
// Program Header Types (p_type)
// ============================================================================

#define PT_NULL         0       // Unused entry
#define PT_LOAD         1       // Loadable segment
#define PT_DYNAMIC      2       // Dynamic linking information
#define PT_INTERP       3       // Program interpreter path
#define PT_NOTE         4       // Auxiliary information
#define PT_SHLIB        5       // Reserved
#define PT_PHDR         6       // Program header table itself
#define PT_TLS          7       // Thread-Local Storage template
#define PT_GNU_EH_FRAME 0x6474E550  // GCC .eh_frame_hdr segment
#define PT_GNU_STACK    0x6474E551  // Stack executability indicator
#define PT_GNU_RELRO    0x6474E552  // Read-only after relocation

// ============================================================================
// Program Header Flags (p_flags)
// ============================================================================

#define PF_X            0x1     // Segment is executable
#define PF_W            0x2     // Segment is writable
#define PF_R            0x4     // Segment is readable

// ============================================================================
// Section Header Types (sh_type)
// ============================================================================

#define SHT_NULL        0       // Inactive section
#define SHT_PROGBITS    1       // Program-defined data
#define SHT_SYMTAB      2       // Symbol table
#define SHT_STRTAB      3       // String table
#define SHT_RELA        4       // Relocation entries with addends
#define SHT_HASH        5       // Symbol hash table
#define SHT_DYNAMIC     6       // Dynamic linking information
#define SHT_NOTE        7       // Notes
#define SHT_NOBITS      8       // Uninitialized data (.bss)
#define SHT_REL         9       // Relocation entries without addends
#define SHT_DYNSYM      11      // Dynamic symbol table

// ============================================================================
// Section Header Flags (sh_flags)
// ============================================================================

#define SHF_WRITE       0x1     // Section is writable
#define SHF_ALLOC       0x2     // Section occupies memory at runtime
#define SHF_EXECINSTR   0x4     // Section contains executable instructions

// ============================================================================
// ELF64 Header Structure
// ============================================================================

typedef struct {
    uint8_t     e_ident[EI_NIDENT]; // ELF identification
    uint16_t    e_type;             // Object file type
    uint16_t    e_machine;          // Machine type
    uint32_t    e_version;          // Object file version
    uint64_t    e_entry;            // Entry point virtual address
    uint64_t    e_phoff;            // Program header table file offset
    uint64_t    e_shoff;            // Section header table file offset
    uint32_t    e_flags;            // Processor-specific flags
    uint16_t    e_ehsize;           // ELF header size
    uint16_t    e_phentsize;        // Program header table entry size
    uint16_t    e_phnum;            // Number of program header entries
    uint16_t    e_shentsize;        // Section header table entry size
    uint16_t    e_shnum;            // Number of section header entries
    uint16_t    e_shstrndx;         // Section name string table index
} __attribute__((packed)) Elf64_Ehdr;

// ============================================================================
// ELF64 Program Header Structure
// ============================================================================

typedef struct {
    uint32_t    p_type;             // Segment type
    uint32_t    p_flags;            // Segment flags
    uint64_t    p_offset;           // Segment file offset
    uint64_t    p_vaddr;            // Segment virtual address
    uint64_t    p_paddr;            // Segment physical address (unused)
    uint64_t    p_filesz;           // Segment size in file
    uint64_t    p_memsz;            // Segment size in memory
    uint64_t    p_align;            // Segment alignment
} __attribute__((packed)) Elf64_Phdr;

// ============================================================================
// ELF64 Section Header Structure
// ============================================================================

typedef struct {
    uint32_t    sh_name;            // Section name (index into string table)
    uint32_t    sh_type;            // Section type
    uint64_t    sh_flags;           // Section flags
    uint64_t    sh_addr;            // Section virtual address
    uint64_t    sh_offset;          // Section file offset
    uint64_t    sh_size;            // Section size
    uint32_t    sh_link;            // Link to another section
    uint32_t    sh_info;            // Additional section information
    uint64_t    sh_addralign;       // Section alignment
    uint64_t    sh_entsize;         // Entry size if section holds table
} __attribute__((packed)) Elf64_Shdr;

// ============================================================================
// ELF64 Symbol Table Entry
// ============================================================================

typedef struct {
    uint32_t    st_name;            // Symbol name (index into string table)
    uint8_t     st_info;            // Symbol type and binding
    uint8_t     st_other;           // Symbol visibility
    uint16_t    st_shndx;           // Section index
    uint64_t    st_value;           // Symbol value
    uint64_t    st_size;            // Symbol size
} __attribute__((packed)) Elf64_Sym;

// ============================================================================
// ELF64 Relocation Entries
// ============================================================================

typedef struct {
    uint64_t    r_offset;           // Address of reference
    uint64_t    r_info;             // Symbol index and type of relocation
} __attribute__((packed)) Elf64_Rel;

typedef struct {
    uint64_t    r_offset;           // Address of reference
    uint64_t    r_info;             // Symbol index and type of relocation
    int64_t     r_addend;           // Constant addend
} __attribute__((packed)) Elf64_Rela;

// Relocation info macros
#define ELF64_R_SYM(i)      ((i) >> 32)
#define ELF64_R_TYPE(i)     ((i) & 0xffffffffL)
#define ELF64_R_INFO(s, t)  (((uint64_t)(s) << 32) + ((uint64_t)(t) & 0xffffffffL))

// ============================================================================
// ELF64 Dynamic Section Entry
// ============================================================================

typedef struct {
    int64_t     d_tag;              // Dynamic entry type
    union {
        uint64_t    d_val;          // Integer value
        uint64_t    d_ptr;          // Address value
    } d_un;
} __attribute__((packed)) Elf64_Dyn;

// Dynamic entry tags
#define DT_NULL         0           // End of dynamic section
#define DT_NEEDED       1           // Name of needed library
#define DT_PLTRELSZ     2           // Size of PLT relocs
#define DT_PLTGOT       3           // Processor-defined value
#define DT_HASH         4           // Symbol hash table address
#define DT_STRTAB       5           // String table address
#define DT_SYMTAB       6           // Symbol table address
#define DT_RELA         7           // Rela relocation table address
#define DT_RELASZ       8           // Size of Rela relocation table
#define DT_RELAENT      9           // Size of Rela relocation entry
#define DT_STRSZ        10          // Size of string table
#define DT_SYMENT       11          // Size of symbol table entry
#define DT_INIT         12          // Address of initialization function
#define DT_FINI         13          // Address of termination function
#define DT_SONAME       14          // Shared object name
#define DT_RPATH        15          // Library search path
#define DT_SYMBOLIC     16          // Alter symbol resolution algorithm
#define DT_REL          17          // Rel relocation table address
#define DT_RELSZ        18          // Size of Rel relocation table
#define DT_RELENT       19          // Size of Rel relocation entry
#define DT_PLTREL       20          // Type of relocation used for PLT
#define DT_DEBUG        21          // Reserved for debugger
#define DT_TEXTREL      22          // Relocations for non-writable segments
#define DT_JMPREL       23          // Address of PLT relocs
#define DT_BIND_NOW     24          // Process relocations of object at load time
#define DT_FLAGS        30          // Flags for the object being loaded
#define DT_RELACOUNT    0x6ffffff9  // Number of RELATIVE relocations in DT_RELA
#define DT_RELCOUNT     0x6ffffffa  // Number of RELATIVE relocations in DT_REL

// ============================================================================
// x86-64 Relocation Types (r_info low 32 bits, ELF64_R_TYPE)
//
// #427 FAKE-audit CRITICAL: only the constants actually needed to apply real
// relocations for -fPIC/-pie userland ELFs (gcc/ld default codegen) are used
// today, but the full common set is defined here for completeness.
// ============================================================================

#define R_X86_64_NONE       0   // No relocation
#define R_X86_64_64         1   // S + A (direct 64-bit)
#define R_X86_64_PC32       2   // S + A - P (PC-relative 32-bit)
#define R_X86_64_GOT32      3   // G + A
#define R_X86_64_PLT32      4   // L + A - P
#define R_X86_64_COPY       5   // Copy symbol at runtime
#define R_X86_64_GLOB_DAT   6   // S (set GOT entry to symbol's address)
#define R_X86_64_JUMP_SLOT  7   // S (set PLT entry to symbol's address)
#define R_X86_64_RELATIVE   8   // B + A (base address + addend)
#define R_X86_64_GOTPCREL   9   // G + GOT + A - P
#define R_X86_64_32         10  // S + A (32-bit, zero extend)
#define R_X86_64_32S        11  // S + A (32-bit, sign extend)
#define R_X86_64_16         12  // S + A (16-bit)
#define R_X86_64_PC16       13  // S + A - P (16-bit)
#define R_X86_64_8          14  // S + A (8-bit)
#define R_X86_64_PC8        15  // S + A - P (8-bit)
#define R_X86_64_DTPMOD64   16  // TLS: module ID
#define R_X86_64_DTPOFF64   17  // TLS: offset in module's TLS block
#define R_X86_64_TPOFF64    18  // TLS: offset relative to thread pointer
#define R_X86_64_TLSGD      19  // TLS: general dynamic
#define R_X86_64_TLSLD      20  // TLS: local dynamic
#define R_X86_64_DTPOFF32   21  // TLS: offset in module's TLS block (32-bit)
#define R_X86_64_GOTTPOFF   22  // TLS: GOT entry for negated static TLS offset
#define R_X86_64_TPOFF32    23  // TLS: offset relative to thread pointer (32-bit)
#define R_X86_64_IRELATIVE  37  // indirect (ifunc) - resolver at B + A

// Section header special index
#define SHN_UNDEF       0           // Undefined section reference

// ============================================================================
// ELF Loader Error Codes
// ============================================================================

#define ELF_SUCCESS             0   // Success
#define ELF_ERR_NULL_PTR       -1   // NULL pointer passed
#define ELF_ERR_TOO_SMALL      -2   // Data too small to contain ELF header
#define ELF_ERR_BAD_MAGIC      -3   // Invalid ELF magic number
#define ELF_ERR_NOT_64BIT      -4   // Not a 64-bit ELF file
#define ELF_ERR_NOT_LE         -5   // Not little-endian
#define ELF_ERR_BAD_VERSION    -6   // Invalid ELF version
#define ELF_ERR_NOT_X86_64     -7   // Not x86_64 architecture
#define ELF_ERR_NOT_EXEC       -8   // Not an executable or PIE
#define ELF_ERR_NO_PHDR        -9   // No program headers
#define ELF_ERR_PHDR_OVERFLOW  -10  // Program header offset/size overflow
#define ELF_ERR_SEGMENT_OVERFLOW -11 // Segment offset/size overflow
#define ELF_ERR_ALLOC_FAILED   -12  // Memory allocation failed
#define ELF_ERR_LOAD_FAILED    -13  // Failed to load segment

// ============================================================================
// ELF Loader Structures
// ============================================================================

// Loaded segment information
typedef struct {
    uint64_t    vaddr;              // Virtual address where segment is loaded
    uint64_t    memsz;              // Size of segment in memory
    uint32_t    flags;              // Segment flags (PF_R, PF_W, PF_X)
} Elf64_LoadedSeg;

// ELF load result containing all loaded segment information
typedef struct {
    uint64_t        entry_point;    // Program entry point
    uint64_t        base_addr;      // Base address for PIE relocation
    void           *load_base;      // Pointer to allocated memory
    uint64_t        load_size;      // Total size of allocated memory
    Elf64_LoadedSeg *segments;      // Array of loaded segments
    uint32_t        segment_count;  // Number of loaded segments
    bool            is_pie;         // True if this is a PIE executable
} Elf64_LoadResult;

// ============================================================================
// #404 / #499 Phase Q: pre-map validation summary (Rust FFI out-struct)
// ============================================================================
// Parsed/validated ELF64 header fields produced by elf_validate_full_c (verbatim
// reference) and elf_validate_full_rs (Rust, rustkern.rs). #[repr(C)] mirror is
// ElfValidated in rustkern.rs; sizeof is asserted == 40 in elf.c so the FFI
// struct can never silently drift.
typedef struct {
    uint16_t e_type;
    uint16_t e_machine;
    uint16_t e_phnum;
    uint16_t e_phentsize;
    uint32_t n_load;            // number of PT_LOAD segments
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t first_load_vaddr;  // p_vaddr of the first PT_LOAD segment
} elf_validated_t;

// ============================================================================
// ELF Loader API
// ============================================================================

/**
 * Validate an ELF64 file image.
 *
 * @param elf_data  Pointer to ELF file data
 * @param size      Size of the ELF data
 * @return          ELF_SUCCESS on success, negative error code on failure
 *
 * CONTRACT CHANGE, #404 Phase Q (b808), recorded here because it reaches CALLERS
 * (found by the 3-way drift audit, 2026-07-16; previously undocumented):
 *
 * This function is NO LONGER header-only. Before b808 it validated the ehdr and
 * the phdr TABLE, and the per-PT_LOAD file-bounds check plus the "image has at
 * least one PT_LOAD" check lived in calculate_load_bounds(), a static helper the
 * three loaders call immediately afterwards. The Phase Q seam MERGED those two
 * checks into this function. It therefore now ALSO validates:
 *   - that at least one PT_LOAD segment exists       -> ELF_ERR_NO_PHDR
 *   - that every PT_LOAD's file extent is in-bounds  -> ELF_ERR_SEGMENT_OVERFLOW
 *
 * It is STRICTLY STRONGER: measured over 438,747 vectors, 48,130 change verdict
 * (16,896 -> NO_PHDR, 31,234 -> SEGMENT_OVERFLOW) and ZERO go the other way (there
 * is no image the pre-b808 elf_validate rejected that this one accepts). So it is
 * fail-closed, and no binary that used to load is now rejected.
 *
 * WHY CALLERS SHOULD CARE: elf_validate() is public and EIGHT callers use it
 * WITHOUT ever calling calculate_load_bounds(), so they inherit the stricter
 * verdict: proc/cron.c:419, proc/services.c:235, proc/syscall.c:1435,
 * proc/process.c:1682, desktop.c:96, gui/desktop.c:121,
 * net/remote_ctrl.c:1048/1070/1244, proc/selfupdate.c:173. Seven of the eight
 * validate-then-spawn, so their outcome is unchanged (elf_load would have
 * rejected the same image moments later with the same code, just later).
 * proc/selfupdate.c:173 is the one genuinely observable case: it uses this as a
 * shape gate on a new kernel image and never elf_load()s it, so it now
 * additionally requires >= 1 in-bounds PT_LOAD. A real kernel.elf passes, and
 * that path is SHA256 + RSA-signature gated, so this is a strictness improvement.
 *
 * Do not describe this function as "the same header check as before". It is not.
 */
int elf_validate(const void *elf_data, uint32_t size);

// #404 Phase Q pre-map validation seam. elf_validate_full_c = verbatim reference
// (ehdr + phdr-table + PT_LOAD file-bounds walk, retaining the C's gaps);
// elf_validate_full_rs (Rust) = the same, plus the confinement gates. Both are
// PURE (no map/alloc/CR3/copy). The live elf_validate() routes to _rs under
// -DRUST_ELF, else _c. elf_validate_full_c is also the differential reference.
int elf_validate_full_c(const void *elf_data, uint32_t size, elf_validated_t *out);
int elf_validate_full_rs(const uint8_t *buf, uint64_t size, elf_validated_t *out);
void elf_rust_selftest(void);

/**
 * Load an ELF64 executable into memory
 *
 * This function parses the ELF headers, allocates memory for all PT_LOAD
 * segments, and copies the segment data into the allocated memory.
 * For PIE executables, segments are loaded at a kernel-determined base address.
 *
 * @param elf_data      Pointer to ELF file data
 * @param size          Size of the ELF data
 * @param entry_point   Output: virtual address of the entry point
 * @return              ELF_SUCCESS on success, negative error code on failure
 */
int elf_load(void *elf_data, uint32_t size, uint64_t *entry_point);

/**
 * Load an ELF64 executable with full result information
 *
 * Similar to elf_load(), but returns detailed information about all loaded
 * segments. The caller is responsible for freeing result->segments and
 * result->load_base when done.
 *
 * @param elf_data  Pointer to ELF file data
 * @param size      Size of the ELF data
 * @param result    Output: detailed load result (must be freed by caller)
 * @return          ELF_SUCCESS on success, negative error code on failure
 */
int elf_load_full(void *elf_data, uint32_t size, Elf64_LoadResult *result);

/**
 * Unload a previously loaded ELF executable
 *
 * Frees all memory associated with a loaded ELF executable.
 *
 * @param result    Pointer to load result from elf_load_full()
 */
void elf_unload(Elf64_LoadResult *result);

/**
 * Get a human-readable error message for an ELF error code
 *
 * @param error     Error code returned by ELF functions
 * @return          Pointer to static error message string
 */
const char *elf_strerror(int error);

/**
 * Get the ELF header from file data
 *
 * @param elf_data  Pointer to ELF file data
 * @return          Pointer to ELF header (cast of elf_data)
 */
static inline const Elf64_Ehdr *elf_get_header(const void *elf_data) {
    return (const Elf64_Ehdr *)elf_data;
}

/**
 * Get a program header from file data
 *
 * @param elf_data  Pointer to ELF file data
 * @param index     Program header index
 * @return          Pointer to program header, or NULL if invalid
 */
static inline const Elf64_Phdr *elf_get_phdr(const void *elf_data, uint16_t index) {
    const Elf64_Ehdr *ehdr = elf_get_header(elf_data);
    if (index >= ehdr->e_phnum) {
        return NULL;
    }
    return (const Elf64_Phdr *)((const uint8_t *)elf_data + ehdr->e_phoff +
                                 index * ehdr->e_phentsize);
}

/**
 * Get a section header from file data
 *
 * @param elf_data  Pointer to ELF file data
 * @param index     Section header index
 * @return          Pointer to section header, or NULL if invalid
 */
static inline const Elf64_Shdr *elf_get_shdr(const void *elf_data, uint16_t index) {
    const Elf64_Ehdr *ehdr = elf_get_header(elf_data);
    if (index >= ehdr->e_shnum) {
        return NULL;
    }
    return (const Elf64_Shdr *)((const uint8_t *)elf_data + ehdr->e_shoff +
                                 index * ehdr->e_shentsize);
}

/**
 * Load an ELF64 executable into a user address space
 *
 * This function loads ELF segments into a specified address space using
 * user-accessible page mappings. Used for loading user-mode applications.
 *
 * @param elf_data      Pointer to ELF file data (in kernel memory)
 * @param size          Size of the ELF data
 * @param pml4_phys     Physical address of target address space (CR3)
 * @param entry_point   Output: virtual address of the entry point
 * @param load_base     Output: base virtual address where code is loaded
 * @param load_end      Output: end virtual address (highest mapped address + 1)
 * @return              ELF_SUCCESS on success, negative error code on failure
 */
int elf_load_user(void *elf_data, uint32_t size, uint64_t pml4_phys,
                  uint64_t *entry_point, uint64_t *load_base, uint64_t *load_end);

/**
 * Debug: print ELF header information
 *
 * @param elf_data  Pointer to ELF file data
 */
void elf_print_header(const void *elf_data);

/**
 * Debug: print all program headers
 *
 * @param elf_data  Pointer to ELF file data
 */
void elf_print_phdrs(const void *elf_data);

#endif // ELF_H
