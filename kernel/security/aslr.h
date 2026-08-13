// aslr.h - address-space randomisation support for MayteraOS.
//
// #646 HONESTY PASS (2026-08-05). What this module ACTUALLY does today, as
// measured by grepping the whole kernel tree for call sites, not as previously
// described by the API surface:
//
//   LIVE   - aslr_init() detects the hardware entropy source for REPORTING and
//            offers boot-time samples to crypto/csprng.c. Called from
//            security_init() (kernel/security/security.c), which main.c calls.
//   LIVE   - aslr_get_random()/aslr_get_random_range() draw from the shared
//            HMAC-DRBG. Two real consumers: exec/elf.c randomises the load base
//            of every PIE user image, and stack_guard.c's canary path.
//   NOT    - stack, heap and mmap randomisation. The per-process ASLR API that
//            used to live in this header (aslr_process_state_t,
//            aslr_init_process, aslr_get_{stack,heap,mmap,exec}_base,
//            aslr_randomize_stack_pointer) had ZERO callers from the day it was
//            written and has been DELETED rather than left standing as an
//            implied feature. Its ASLR_MMAP_BASE (0x0000700000000000) was also
//            outside the reachable user window, so it could never have worked
//            as written.
//   NOT    - KASLR. aslr_get_kernel_offset() returned a g_kernel_slide that
//            nothing ever assigned, i.e. a hard-coded 0 dressed as a feature.
//            DELETED.
//   NOT    - aslr_get_random_bytes(): zero callers and a duplicate of
//            csprng_bytes(). DELETED; call crypto/csprng.c directly.
//
// If you add stack/heap randomisation later, add the function AND its call site
// in the same change. A randomisation API with no caller randomises nothing.
#ifndef SECURITY_ASLR_H
#define SECURITY_ASLR_H

#include "../types.h"

// Detected hardware entropy source. Reporting only: the bytes themselves come
// from crypto/csprng.c regardless of what is detected here.
typedef enum {
    ASLR_ENTROPY_NONE = 0,      // No hardware RNG available
    ASLR_ENTROPY_RDRAND,        // Intel RDRAND instruction
    ASLR_ENTROPY_RDSEED,        // Intel RDSEED instruction
    ASLR_ENTROPY_TSC            // Fallback: TSC jitter only
} aslr_entropy_source_t;

// Detect the entropy source and offer boot samples to the CSPRNG.
void aslr_init(void);

// The global on/off policy knob.
//
// KNOWN GAP, stated because pretending otherwise is the #646 defect: the ONE
// live consumer of randomisation, the PIE load-base slot in exec/elf.c, does
// NOT consult this. Turning ASLR "off" here therefore does not currently turn
// off PIE base randomisation. Fixing that means an aslr_enabled() test in
// exec/elf.c, which is outside this module.
bool aslr_enabled(void);
void aslr_set_enabled(bool enable);

// Random draws, backed by crypto/csprng.c (HMAC-DRBG).
uint64_t aslr_get_random(void);
// Rejection-sampled, so free of the modulo bias a plain % would introduce.
uint64_t aslr_get_random_range(uint64_t max);

// Offer entropy to the system DRBG. There is no private pool any more (#654).
void aslr_add_entropy(const void *data, size_t size);

// Print the measured randomisation posture.
void aslr_print_info(void);

#endif // SECURITY_ASLR_H
