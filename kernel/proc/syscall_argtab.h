// syscall_argtab.h - #500 / MAYTERA-SEC-2026-0016
//
// THE CHOKE POINT.
//
// The problem this solves is not "some syscalls forgot to validate". It is that
// ~110 syscalls each had to remember to validate, and all but five forgot, and
// nothing noticed for the entire life of the tree. Fixing 110 call sites fixes
// tonight; the 111th syscall reintroduces the hole next week. So the check does
// not live in the handlers. It lives once, in syscall_dispatch(), driven by a
// table that says what each syscall's arguments ARE.
//
// FEASIBILITY (the honest version, measured against a full inventory of all 239
// dispatcher cases and all 171 user-pointer arguments):
//
//   READ  + STRING   75      PTR_STR
//   WRITE + FIXED    48      PTR_W  + LEN_FIXED
//   WRITE + ARG      26      PTR_W  + LEN_ARG
//   READ  + ARG      11      PTR_R  + LEN_ARG
//   READ  + FIXED     6      PTR_R  + LEN_FIXED
//   READ  + ARGV      2      PTR_ARGV   (two-level: array of string pointers)
//   not dereferenced  2      PTR_NONE   (SYS_CLONE stack, SYS_FUTEX addr2)
//
// 169 of 171 (98.8%) are expressible with three kinds and three length sources,
// because a syscall's buffer length is essentially always one of: a compile-time
// constant, another argument, or NUL-termination. The dispatcher DOES know
// enough. That is why the table-driven shape is the right one here and not a
// compromise.
//
// The residue is handled honestly rather than hidden:
//   - PTR_ARGV gets a real two-level walk (validate the vector, then each string).
//   - PTR_CUSTOM means the length is genuinely not knowable at dispatch (it is
//     inside a user-supplied struct, or depends on runtime state). The handler
//     must then validate, and tools/syscall-ptr-lint FAILS THE BUILD if a
//     PTR_CUSTOM handler contains no validator call. It moves the check; it does
//     not excuse it.
//
// WHAT THIS DOES NOT SOLVE - TOCTOU. Validating a pointer and dereferencing it
// later is not atomic. Another thread in the same process (this kernel has
// threads and futexes) can unmap or remap the range between the check in the
// dispatcher and the handler's memcpy. This table therefore makes pointers
// VALID-AT-ENTRY, not safe-forever. The only complete fix is to copy user data
// into kernel memory under the check and have handlers use the copy, which is
// what copy_from_user() is for. See the note in validate.c: that API is now
// functional for the first time, but adopting it in the handlers is a separate,
// larger change and is NOT done. Do not read this table as a claim of TOCTOU
// safety.

#ifndef SYSCALL_ARGTAB_H
#define SYSCALL_ARGTAB_H

#include "../types.h"

// What an argument IS.
typedef enum {
    PTR_NONE = 0,   // scalar, or a pointer the kernel never dereferences
    PTR_R,          // user buffer the kernel READS   (info source)
    PTR_W,          // user buffer the kernel WRITES  (info sink; worse)
    PTR_RW,         // both
    PTR_STR,        // NUL-terminated user string, bounded scan
    PTR_ARGV,       // NULL-terminated vector of user string pointers
    PTR_CUSTOM,     // length unknowable at dispatch; handler MUST validate
} arg_kind_t;

// Where the LENGTH comes from. Length matters as much as the address: a valid
// base with an attacker-chosen length is the classic bypass, and this tree has
// already shipped that class twice (MAYTERA-SEC-2026-0014 total_length-ihl
// underflow; #489 p_filesz underflow).
typedef enum {
    LEN_NONE = 0,
    LEN_FIXED,      // .len is a byte count known at compile time
    LEN_ARG,        // .len is an ARGUMENT INDEX (1-6) holding the byte count
    LEN_ARG_ELEMS,  // .len is an arg index holding an ELEMENT COUNT; .elem_size scales it
    LEN_STR,        // .len is the maximum scan length for a string
} len_src_t;

typedef struct {
    uint8_t   kind;       // arg_kind_t
    uint8_t   len_src;    // len_src_t
    uint16_t  elem_size;  // for LEN_ARG_ELEMS: bytes per element
    uint32_t  len;        // FIXED byte count, ARG index, or STR max
} arg_desc_t;

typedef struct {
    uint16_t   num;           // syscall number
    const char *name;         // for diagnostics
    arg_desc_t args[6];       // arg1..arg6
} syscall_desc_t;

// #503: the 32-bit-compat pointer prefix. user.ld loads apps at 0x80000000,
// exactly the 2^31 boundary and therefore NEGATIVE as a signed 32-bit value, so
// a user pointer that round-trips through a 32-bit int arrives at the kernel
// sign-extended to 0xFFFFFFFF_8xxxxxxx. SANITIZE_USER_PTR (proc/syscall.c) masks
// it back down for the three handlers that hit this in practice; the browser
// depends on that. Named here rather than left as a literal because the argtab
// validator in rustkern.rs MUST apply the identical transform before validating
// (otherwise it proves an address the handler never touches), and a mirrored
// constant that drifts silently is exactly the class of bug the lock TU exists
// to catch. Asserted in proc/syscall_argtab_lock.c.
#define USER_PTR_SX_PREFIX 0xFFFFFFFF00000000ULL

// Validate every declared pointer argument of `num` against the CALLING
// process's address space. Returns 0 if all pointers are acceptable, or
// -EFAULT (-14) if any is not. A syscall with no descriptor is NOT validated
// (see the allowlist / lint): this returns 0 for it, so the table can be
// adopted incrementally without breaking the tree, while the lint prevents the
// undeclared set from ever growing.
int syscall_validate_args(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);

// #503: the table itself now lives in RUST (rustkern.rs, "#503 argtab"), per the
// 2026-07-16 all-new-kernel-code-in-Rust rule. This is NEW code, not a port, so
// there is no C twin and no strangler flag: there was never any C here to
// strangle - that absence IS the bug #500 documented. The arg_desc_t /
// syscall_desc_t shapes below are retained as the DOCUMENTED design and for the
// lint to read; no C code builds a table from them, so nothing crosses the FFI
// but scalars. What DOES need locking is the set of byte-size constants the Rust
// table hardcodes for fixed-length buffers, and those are locked by
// _Static_assert in proc/syscall_argtab_lock.c (public structs) and in
// proc/syscall.c (structs private to that TU).

// Does this syscall have a descriptor (i.e. is it actually validated)? Exposed
// so the in-kernel self-test and the lint can check the CLAIM against the built
// kernel rather than against the docs.
int syscall_desc_covers(uint64_t num);

// How many syscalls the table declares. Printed in the boot banner so coverage
// is a checkable fact about the running kernel, not a number in a changelog.
uint32_t syscall_desc_count(void);

#endif // SYSCALL_ARGTAB_H
