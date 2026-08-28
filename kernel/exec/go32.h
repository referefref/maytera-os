// exec/go32.h - the C surface of the go32/DJGPP v2 COFF loader (#211).
//
// The implementation is rustkern/go32.rs; read its header comment first,
// because it states the two format rules that decide whether a load lands in
// the right place (where the COFF starts, and what a ZMAGIC section pointer is
// relative to) and the entry contract that decides whether the loaded program
// survives its first 0x180 bytes.
//
// This header is the FFI contract and nothing else. Every structural claim it
// makes is locked by a _Static_assert the compiler checks, plus one RUNTIME
// check of the stubinfo size against the number rustc computed, so a layout
// that drifts on one side cannot reach a boot.
#ifndef EXEC_GO32_H
#define EXEC_GO32_H

#include "../types.h"

#define GO32_OK           0
#define GO32_E_SHORT     -1
#define GO32_E_NOTMZ     -2
#define GO32_E_NOSTUB    -3
#define GO32_E_NOCOFF    -4
#define GO32_E_NOTZMAGIC -5
#define GO32_E_SECTIONS  -6
#define GO32_E_NOTEXT    -7
#define GO32_E_TOOBIG    -8
#define GO32_E_ARGS      -9

// Mirrors Go32Image in rustkern/go32.rs. `*_off` are FILE offsets already
// resolved from the ZMAGIC section pointers; `*_va` are addresses inside the
// program's own segment, NOT flat addresses. The host adds its load base.
typedef struct {
    uint32_t coff_off;
    uint32_t entry;
    uint32_t text_va, text_sz, text_off;
    uint32_t data_va, data_sz, data_off;
    uint32_t bss_va,  bss_sz;
    uint32_t image_top;   // bss end rounded up to 64 KiB
    uint32_t stub_off;    // file offset of the stubinfo TEMPLATE, 0 if absent
    uint32_t minstack;    // read from that template, never invented
    uint32_t minkeep;     // ditto: the DOS memory the program wants kept
    uint32_t nsections;
} go32_image_t;

_Static_assert(sizeof(go32_image_t) == 60, "go32_image_t layout");
_Static_assert(__builtin_offsetof(go32_image_t, entry) == 4, "go32: entry offset");
_Static_assert(__builtin_offsetof(go32_image_t, image_top) == 40, "go32: image_top offset");
_Static_assert(__builtin_offsetof(go32_image_t, minstack) == 48, "go32: minstack offset");

// The stubinfo is 0x54 bytes. This is not a number to adjust: djgpp's crt0
// copies exactly `size` bytes out of it and crt1 indexes fixed offsets inside
// it, so a different length is a different structure.
#define GO32_STUBINFO_SIZE 0x54

// ---------------------------------------------------------------------------
// rustkern/go32.rs
// ---------------------------------------------------------------------------

// Parse the MZ + COFF headers. Touches no memory outside `file` and PRINTS
// NOTHING, because dos_run_file() uses it as the format detector on every MZ:
// a plain DOS program must be refused quietly (GO32_E_NOSTUB), not narrated.
int go32_parse_rs(const uint8_t *file, uint32_t len, go32_image_t *out);

const char *go32_strerror_rs(int e);

// Lay the image out at `arena + base`. `seg_size` is the size of the program's
// SEGMENT (what stubinfo.initial_size will say), and every section is checked
// against it, so an image larger than the segment is refused rather than
// truncated into it.
int go32_load_rs(const go32_image_t *img, const uint8_t *file, uint32_t len,
                 uint8_t *arena, uint32_t arena_len, uint32_t base,
                 uint32_t seg_size);

uint32_t go32_stubinfo_size_rs(void);

// Build the 0x54-byte stubinfo. Everything passed is something only the host
// knows, because the host owns the memory and the descriptors. `argv0` is a
// NUL-terminated DOS path and is TRUNCATED to 15 characters plus a terminator,
// which is what the field is.
int go32_stubinfo_build_rs(uint8_t *dst, uint32_t dst_len,
                           uint32_t minstack, uint32_t memory_handle,
                           uint32_t initial_size, uint16_t minkeep,
                           uint16_t ds_selector, uint16_t ds_segment,
                           uint16_t psp_selector, uint16_t cs_selector,
                           uint16_t env_size, const char *argv0);

void go32_report_rs(const char *path, const go32_image_t *img);

// Self-test over a SYNTHETIC image built in caller-supplied scratch. Returns
// the number of FAILING checks and writes the number that RAN to *out_checks,
// so "passed" and "never ran" are distinguishable (#514). Needs 0x21000 bytes;
// with less it reports 0 checks, which the caller must print as SKIPPED.
//
// SCRATCH IS CALLER-SUPPLIED ON PURPOSE. A buffer this size on the DOS task's
// 64 KiB kernel stack is what #212 shipped and then had to withdraw.
uint32_t go32_selftest_rs(uint8_t *scratch, uint32_t scratch_len,
                          uint32_t *out_checks);

#endif // EXEC_GO32_H
