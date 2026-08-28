// exec/le.h - #740: LE (Linear Executable) loader, C-side surface.
//
// THE PARSER IS RUST. Everything in this header is either a #[repr(C)] mirror
// of a struct rustkern/le.rs owns, or a thin C entry point that does file I/O
// and printing (both of which are C-only facilities in this kernel) and calls
// into Rust for every byte of decoding.
//
// The split is deliberate and is the standing rule, not a preference: ALL NEW
// kernel code is Rust unless there is a stated performance reason. There is no
// `<fn>_c` twin and no -DRUST_LE strangler flag here because nothing is being
// replaced; the reference implementation is the HOST harness at
// tools/le-harness/le.c, which stays where it is.
//
// LAYOUT LOCK. Every struct below is all-uint32_t on purpose: no pointers, no
// mixed widths, so C and Rust cannot disagree about padding. The
// _Static_asserts at the bottom pin the sizes at COMPILE time, and
// le_boot_selftest() re-checks them against the Rust side's own sizeof at RUN
// time on the build that actually shipped. A silently divergent struct layout
// across an FFI is the kind of fault that produces plausible garbage rather
// than an error, which is the same failure shape as the wrong-MZ trap this
// loader exists to avoid.

#ifndef EXEC_LE_H
#define EXEC_LE_H

#include "../types.h"

#define LE_MAX_OBJECTS 64u

// Error codes. Numerically identical to le_err_t in tools/le-harness/le.h and
// to the constants in rustkern/le.rs. le_strerror() is the Rust one: there is
// exactly one table of these strings in the kernel.
#define LE_OK                 0
#define LE_E_TRUNCATED        1
#define LE_E_NO_MZ            2
#define LE_E_NO_LE            3
#define LE_E_LX_UNSUPPORTED   4
#define LE_E_BYTE_ORDER       5
#define LE_E_PAGE_SIZE        6
#define LE_E_PAGE_COUNT       7
#define LE_E_OBJECT_COUNT     8
#define LE_E_OBJECT_RANGE     9
#define LE_E_OBJECT_OVERLAP  10
#define LE_E_PAGE_NUM        11
#define LE_E_PAGE_FLAGS      12
#define LE_E_ITERATED        13
#define LE_E_PAGE_DATA       14
#define LE_E_FIXUP_TABLE     15
#define LE_E_FIXUP_RECORD    16
#define LE_E_FIXUP_TARGET    17
#define LE_E_SRC_TYPE        18
#define LE_E_IMPORTS         19
#define LE_E_ENTRY           20
#define LE_E_OVERFLOW        21
#define LE_E_MEM             22

// Object flags (LE object table, +0x08)
#define LE_OBJ_READABLE     0x0001u
#define LE_OBJ_WRITABLE     0x0002u
#define LE_OBJ_EXECUTABLE   0x0004u
#define LE_OBJ_PRELOAD      0x0040u
#define LE_OBJ_ALIAS_16_16  0x1000u
#define LE_OBJ_BIG          0x2000u   // 32-bit segment (D bit set)

// Fixup source types (low nibble of the source byte)
#define LE_SRC_BYTE       0u
#define LE_SRC_SEL16      2u
#define LE_SRC_PTR16_16   3u
#define LE_SRC_OFF16      5u
#define LE_SRC_PTR16_32   6u
#define LE_SRC_OFF32      7u
#define LE_SRC_SELFREL32  8u

typedef struct {
    uint32_t virt_size;
    uint32_t reloc_base;
    uint32_t flags;
    uint32_t page_index;   // 1-based index into the page map
    uint32_t page_count;
    uint32_t reserved;
} le_object_t;

typedef struct {
    uint32_t mz_off;       // the ANCHOR MZ; origin for page_data_abs
    uint32_t le_off;       // absolute file offset of the LE header
    uint32_t cpu_type;
    uint32_t os_type;
    uint32_t mod_flags;
    uint32_t num_pages;
    uint32_t page_size;
    uint32_t last_page_size;
    uint32_t eip_obj;      // 1-based
    uint32_t eip;
    uint32_t esp_obj;
    uint32_t esp;
    uint32_t num_objects;
    uint32_t obj_tab_off;         // LE-header-relative from here down
    uint32_t page_map_off;
    uint32_t fixup_page_tab_off;
    uint32_t fixup_rec_tab_off;
    uint32_t import_mod_tab_off;
    uint32_t import_proc_tab_off;
    uint32_t num_import_mod;
    uint32_t fixup_sect_size;
    uint32_t loader_sect_size;
    uint32_t page_data_abs;       // ABSOLUTE, already anchored on mz_off
    uint32_t page_data_len;
    uint32_t lin_lo, lin_hi;      // the guest linear span
    uint32_t reloc_delta;         // total delta applied so far
    uint32_t _pad;
    le_object_t obj[LE_MAX_OBJECTS];
} le_image_t;

typedef struct {
    uint32_t src;
    uint32_t flags;
    uint32_t src_type;
    uint32_t tgt_type;
    int32_t  src_off;      // SIGNED: negative = straddling continuation
    uint32_t tgt_obj;
    uint32_t tgt_off;
    uint32_t rec_off;
    uint32_t rec_len;
    uint32_t page;
} le_fixup_t;

typedef struct {
    uint32_t records;
    uint32_t sources;
    uint32_t pages_with_fixups;
    uint32_t max_page_sources;
    uint32_t by_src[16];
    uint32_t by_tgt[4];
    uint32_t by_rec_len[16];
    uint32_t src_list_recs;
    uint32_t additive;
    uint32_t tgt_off32;
    uint32_t tgt_off16;
    uint32_t ord16;
    uint32_t neg_src_off;
    uint32_t straddling;
    uint32_t by_page_flag[8];
    uint32_t pages_no_data;
    uint32_t last_page_num;
} le_hist_t;

typedef struct {
    uint32_t pages_copied;
    uint32_t pages_zeroed;
    uint32_t pages_short;
    uint32_t fixups_applied;
    uint32_t fixups_by_src[16];
    uint32_t fixups_negative_off;
    uint32_t fixups_straddling;
    uint32_t fixups_off_object;
    uint32_t bytes_copied;
    uint32_t bytes_zeroed;
} le_stats_t;

typedef struct {
    uint32_t checked;
    uint32_t inside_object;
    uint32_t outside;
    uint32_t unreadable;
    uint32_t first_bad_lin;
    uint32_t first_bad_val;
} le_valid_t;

// ---------------------------------------------------------------------------
// The Rust FFI. Declared here so exactly one C header owns the surface (#742:
// the owning header, not a private extern scattered across call sites).
// ---------------------------------------------------------------------------
const char *le_strerror_rs(int e);
int le_find_rs(const uint8_t *file, uint32_t len, uint32_t *mz_off, uint32_t *le_off);
int le_parse_rs(const uint8_t *file, uint32_t len, le_image_t *img);
int le_page_entry_rs(const uint8_t *file, uint32_t len, const le_image_t *img,
                     uint32_t page, uint32_t *num, uint32_t *flags);
int le_page_file_off_rs(const uint8_t *file, uint32_t len, const le_image_t *img,
                        uint32_t page, uint32_t *off, uint32_t *plen);
int le_page_object_rs(const le_image_t *img, uint32_t page);
int le_page_linear_rs(const le_image_t *img, uint32_t page, uint32_t *lin);
int le_fixup_hist_rs(const uint8_t *file, uint32_t len, const le_image_t *img,
                     le_hist_t *hist);
int le_fixup_at_rs(const uint8_t *file, uint32_t len, const le_image_t *img,
                   uint32_t page, uint32_t index, le_fixup_t *out);
int le_fixup_page_range_rs(const uint8_t *file, uint32_t len, const le_image_t *img,
                           uint32_t page, uint32_t *s0, uint32_t *s1);
int le_relocate_rs(le_image_t *img, uint32_t delta);
int le_load_rs(const uint8_t *file, uint32_t len, const le_image_t *img,
               uint8_t *mem, uint32_t mem_size, uint32_t mem_base_lin,
               le_stats_t *st);
int le_validate_rs(const uint8_t *file, uint32_t len, const le_image_t *img,
                   const uint8_t *mem, uint32_t mem_size, uint32_t mem_base_lin,
                   le_valid_t *out);
int le_selftest_rs(uint8_t *buf, uint32_t buflen);
uint32_t le_sizeof_image_rs(void);
uint32_t le_sizeof_fixup_rs(void);
uint32_t le_sizeof_hist_rs(void);
uint32_t le_sizeof_stats_rs(void);
uint32_t le_sizeof_valid_rs(void);

// ---------------------------------------------------------------------------
// C entry points (exec/le.c)
// ---------------------------------------------------------------------------

// Milestone 1: parse `path` and PRINT header, object table, page map, fixup
// page table and the decoded fixup histogram. No execution, no guest arena.
// Returns 0 on success, or the le error code.
int le_info_dump(const char *path);

// A loaded, relocated module, ready for an execution core. `arena` is kmalloc'd
// and OWNED BY THE CALLER: free it with le_free_module().
//
// This is the handoff seam for the 32-bit execution path. It is not a
// speculative API with no callers (this codebase's characteristic failure):
// le_load_test() below is built on top of it, so every field here is filled by
// the code path that actually runs at boot.
typedef struct {
    uint8_t *arena;      // arena[0] represents guest linear base_lin
    uint32_t arena_size;
    uint32_t base_lin;
    uint32_t entry_lin;  // CS:EIP, flat, guest linear
    uint32_t stack_lin;  // SS:ESP, flat, guest linear (0 if the module has none)
    uint32_t lin_lo, lin_hi;
    uint32_t delta;      // the slide that was applied
    // Who frees `arena`. le_load_module() allocates it and sets this;
    // le_load_into() borrows the CALLER'S buffer and leaves it 0, so
    // le_free_module() cannot free memory it did not allocate. Without this
    // flag the two entry points would need two different teardown rules, which
    // is the shape a double free comes in.
    uint32_t owns_arena;
    le_image_t img;
    le_stats_t st;
    le_valid_t va;
} le_module_t;

// Read, parse, relocate by `delta` (0 = the default 1 MiB slide), allocate the
// arena, load every page, apply every fixup and run the post-load invariant.
// Returns LE_OK only if the load succeeded; the invariant result is in out->va
// and is the caller's to judge. On any error nothing is allocated.
int le_load_module(const char *path, uint32_t delta, le_module_t *out);
void le_free_module(le_module_t *m);

// The same load, into a CALLER-PROVIDED arena, from bytes the caller already
// read. Added for #740's execution path, which needs the module to land inside
// a LARGER flat space that also contains the guest's first megabyte (the PSP,
// the real-mode transfer buffer and the VGA aperture at 0xA0000), rather than
// in a private buffer sized to the module.
//
// It is a factoring, not a fork: le_load_module() is now a thin wrapper that
// reads the file, sizes and allocates an arena, and calls this. One parse, one
// relocate, one load, one invariant walk, reached two ways. `arena_base_lin` is
// the guest linear address that arena[0] represents, so passing 0 with a
// whole-space arena is the flat case and passing img.lin_lo & ~(page-1) with a
// module-sized arena is the original one.
//
// `tag` is only used in the log lines, so a caller with no filename can pass
// any short identifier. Returns LE_OK or an LE_E_* code; nothing is allocated
// or freed here on any path.
int le_load_into(const char *tag, const uint8_t *file, uint32_t size, uint32_t delta,
                 uint8_t *arena, uint32_t arena_size, uint32_t arena_base_lin,
                 le_module_t *out);

// Milestone 2: parse, relocate by `delta` (0 selects the default 1 MiB slide),
// materialise into a kmalloc'd arena, apply every fixup, then run the post-load
// readback invariant and print the result. Frees the arena before returning.
// Returns 0 only if the load succeeded AND every checked fixup landed inside a
// declared object. `out_entry_lin` / `out_stack_lin`, when non-NULL, receive
// the guest linear CS:EIP and SS:ESP the module should start at.
int le_load_test(const char *path, uint32_t delta,
                 uint32_t *out_entry_lin, uint32_t *out_stack_lin);

// Boot hook. Runs the Rust fixture self-test (always), then, IF
// /CONFIG/LEINFO.CFG exists, dumps every path listed in it, one per line.
// Absent that file it prints one line and does nothing else, so the shipping
// golden pays a fixture self-test and nothing more.
void le_boot_selftest(void);

// Compile-time layout lock. If Rust and C ever disagree, the build stops here
// rather than at a wrong number on a serial line six months later.
_Static_assert(sizeof(le_object_t) == 24, "le_object_t layout");
_Static_assert(sizeof(le_image_t) == 112 + 64 * 24, "le_image_t layout");
_Static_assert(sizeof(le_fixup_t) == 40, "le_fixup_t layout");
_Static_assert(sizeof(le_hist_t) == 228, "le_hist_t layout");
_Static_assert(sizeof(le_stats_t) == 100, "le_stats_t layout");
_Static_assert(sizeof(le_valid_t) == 24, "le_valid_t layout");

#endif // EXEC_LE_H
