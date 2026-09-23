// escrow_core.h - #305 IMMUTABLE SECURITY CORE (Stage 5, part A of
// docs/CONTRACT_ENFORCEMENT_PLAN.md).
//
// WHAT THIS ADDS. Stages 1-4 put the escrow enforcement in the kernel, but the
// escrow PRINCIPAL (the only identity that may issue a GRANT) and the code that
// used it were ordinary, WRITABLE kernel memory: a kernel-level exploit by
// AI-authored code could patch them. This is the crown-jewels problem
// (docs/CONTRACT_ARCHITECTURE.md section 5): "it must be unreachable by the AI,
// not merely off-limits by convention".
//
// THE MECHANISM. A dedicated, page-aligned, page-SIZED kernel object holds the
// escrow principal id plus a magic + canary. After boot init it is populated,
// then (under -DESCROW_IMMUTABLE_SEAL) its page is made READ-ONLY in the kernel
// page tables, reusing the proven vmm_map_page_in() downgrade path (which splits
// the containing 2MB huge page preserving every sibling's flags, and does the
// SMP-safe TLB shootdown). Because the object is aligned(4096) and sized 4096 it
// is the SOLE occupant of its page, so making that one page read-only affects
// NOTHING else (a bounded blast radius, the safety-critical property). CR0.WP is
// set on this kernel, so a supervisor write to a read-only page FAULTS.
//
// THE AUTHORITY LIVES HERE. The kernel's escrow issuance (fs/escrow_guard.c)
// reads the principal from THIS core and re-verifies it on every issue, so:
//   - a corrupted principal fails closed (the value handed to gfs_grant_issue is
//     no longer GFS_NODE_ESCROW, so the fold refuses the grant), and
//   - when sealed, the principal cannot even be written (the store faults).
//
// HONEST BOUNDARY (do NOT overstate). This slice protects the principal DATA. The
// enforcement DECISION CODE (the fold's compiled `actor == NODE_ESCROW` compare,
// and escrow_fs_guard's .text) is not itself relocated into a read-only code
// region in this slice; protecting kernel .text is the documented next step. The
// RO seal is BUILD-FLAG gated and OFF by default so the production golden's boot
// path is byte-for-byte unchanged until the seal is proven on real hardware; the
// authority-source + tamper-check are always on.
#ifndef SECURITY_ESCROW_CORE_H
#define SECURITY_ESCROW_CORE_H

#include "../types.h"

// Populate the core with the escrow principal + canary, then (only if built with
// -DESCROW_IMMUTABLE_SEAL) make its page read-only. Call ONCE at boot, after
// paging + the PMM are up and BEFORE any escrow contract can be issued.
// Idempotent. NEVER panics: a seal that does not take leaves the core writable
// but still the authority source and tamper-checked (a failed seal must not brick
// boot).
void escrow_core_init(void);

// The escrow principal id, read from the (read-only-when-sealed) core. Returns 0
// if the core is not initialised or its magic/canary is broken. 0 is never a
// valid GRANT issuer, so a broken core fails closed: the fold refuses every
// grant issued with it.
uint32_t escrow_core_principal(void);

// 1 if the core is intact (magic + canary + principal all as populated), else 0.
// Cheap; called on the escrow issue path so a tampered core denies.
int escrow_core_verify(void);

// 1 if the core page is currently mapped READ-ONLY in the kernel address space
// (so a supervisor write to it faults), else 0 (seal not built, or did not take).
int escrow_core_is_sealed(void);
// 1 if the enforcement code (kernel .text) is mapped READ-ONLY (a write to it
// faults). Built/effective only under -DESCROW_TEXT_SEAL.
int escrow_core_is_text_sealed(void);
// Seal the enforcement CODE (kernel .text) read-only after init. Call LATE in
// boot, after all subsystem init. No-op unless built with -DESCROW_TEXT_SEAL.
void escrow_core_seal_text(void);

// Gated boot self-test (built only under -DESCROW_CORE_SELFTEST). Proves the core
// holds the escrow principal, verify() passes, CR0.WP is set, and (when sealed)
// the page's effective PTE lacks WRITABLE so a post-boot write would fault. No-op
// otherwise.
void escrow_core_selftest(void);

#endif // SECURITY_ESCROW_CORE_H
