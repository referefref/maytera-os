// syscall_mprotect_lock.c - #404 SYS_MPROTECT constant lock.
//
// kernel/rustkern/mprotect.rs validates the Ring-3 arguments of mprotect(). It
// cannot see a C #define, so the address-space bounds, the page size, the
// protection bit values and the refusal codes are DUPLICATED constants on the
// Rust side. A duplicated constant that drifts is not cosmetic here:
//
//   * USER_SPACE_END shrinking in C but not in Rust would let Ring 3 name a
//     range the kernel no longer considers user memory, and the validator
//     would wave it through to do_mprotect().
//   * USER_SPACE_START growing in C but not in Rust would do the reverse and
//     silently reject legal calls, which reads as "mprotect is broken" and
//     gets debugged in the wrong file.
//   * A PROT_* bit changing value would leave the validator's W^X check and
//     do_mprotect()'s VMA_* decode disagreeing about which bit means what,
//     so the W^X refusal would guard the wrong bit while still looking alive.
//
// So the numbers are locked. This translation unit contains NO CODE: it exists
// only so that changing any of them FAILS THE BUILD, loudly, in the file that
// tells you what to do about it. Same _Static_assert pattern as
// proc/syscall_argtab_lock.c (#503).
//
// If you are here because an assert below fired: update the matching const in
// kernel/rustkern/mprotect.rs to the new value, and re-read mprotect_validate_rs
// to check the change does not also need a new check. Do NOT just bump the
// number on one side.

#include "../types.h"
#include "../security/validate.h"   // USER_SPACE_START / USER_SPACE_END
#include "../mm/vmm.h"              // VMM_PAGE_SIZE_4K
#include "../mm/demand.h"           // VMA_READ / VMA_WRITE / VMA_EXEC
#include "syscall.h"                // SYS_MPROTECT, MP_* refusal codes

// --- Address-space bounds (mirrored as USER_SPACE_START / USER_SPACE_END) ---
_Static_assert(USER_SPACE_START == 0x0000000000400000ULL,
               "#404 mprotect: USER_SPACE_START in rustkern/mprotect.rs is stale");
_Static_assert(USER_SPACE_END == 0x00007FFFFFFFFFFFULL,
               "#404 mprotect: USER_SPACE_END in rustkern/mprotect.rs is stale");

// --- Page size (mirrored as PAGE_SIZE, and PAGE_MASK is PAGE_SIZE-1) --------
_Static_assert(VMM_PAGE_SIZE_4K == 4096ULL,
               "#404 mprotect: PAGE_SIZE in rustkern/mprotect.rs is stale");

// --- Protection bits -------------------------------------------------------
// do_mprotect() (mm/demand.c) decodes prot bit 0/1/2 into VMA_READ/WRITE/EXEC.
// The validator's PROT_KNOWN mask and its W^X check assume the same numbering,
// so lock the VMA_* side too: these are the bits prot is translated INTO.
_Static_assert(VMA_READ == (1 << 0),
               "#404 mprotect: PROT_READ/VMA_READ numbering changed, see rustkern/mprotect.rs");
_Static_assert(VMA_WRITE == (1 << 1),
               "#404 mprotect: PROT_WRITE/VMA_WRITE numbering changed, see rustkern/mprotect.rs");
_Static_assert(VMA_EXEC == (1 << 2),
               "#404 mprotect: PROT_EXEC/VMA_EXEC numbering changed, see rustkern/mprotect.rs");

// --- Refusal codes (mirrored as the MP_E_* consts in rustkern/mprotect.rs) --
// These cross the syscall boundary to Ring 3 and are asserted individually by
// /APPS/MMTEST subtest (8), so a drift here silently turns a passing test into
// a test that proves the wrong guard fired.
_Static_assert(MP_OK == 0,          "#404 mprotect: MP_OK drifted from rustkern/mprotect.rs");
_Static_assert(MP_E_PROT_BITS == -1, "#404 mprotect: MP_E_PROT_BITS drifted from rustkern/mprotect.rs");
_Static_assert(MP_E_LEN == -2,      "#404 mprotect: MP_E_LEN drifted from rustkern/mprotect.rs");
_Static_assert(MP_E_ALIGN == -3,    "#404 mprotect: MP_E_ALIGN drifted from rustkern/mprotect.rs");
_Static_assert(MP_E_WX == -4,       "#404 mprotect: MP_E_WX drifted from rustkern/mprotect.rs");
_Static_assert(MP_E_OVERFLOW == -5, "#404 mprotect: MP_E_OVERFLOW drifted from rustkern/mprotect.rs");
_Static_assert(MP_E_RANGE == -6,    "#404 mprotect: MP_E_RANGE drifted from rustkern/mprotect.rs");

// --- The syscall number ----------------------------------------------------
// Locked so the number cannot be edited in the kernel header alone: the libc
// twin and userland/apps/mmtest/main.c both hardcode 23, and syscall-number-lint
// rule 5 checks the app copies against the headers.
_Static_assert(SYS_MPROTECT == 23,
               "#404 mprotect: SYS_MPROTECT changed; update userland/libc/syscall.h and userland/apps/mmtest/main.c");
