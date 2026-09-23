// escrow_device.h - #246/#305 AI escrow Stage 4: CAP_SCOPE_DEVICE.
//
// docs/CONTRACT_ENFORCEMENT_PLAN.md section 4 / docs/CONTRACT_ARCHITECTURE.md
// section 9. Stage 1-3 scoped an escrow grant to a PATH PREFIX. Stage 4 binds
// the grant's OBJECT to a VALIDATED DEVICE IDENTITY, so a scope means "this
// removable device", not "this mount path", and survives a remount at a
// different path AND fails closed when the device's identity changes under it
// (BadUSB / re-enumeration).
//
// THE SPLIT. The PURE identity policy (fingerprint derivation, logical-key
// derivation, confidence class, TOFU verdict) lives in Rust
// (rustkern/capdev.rs). THIS file holds the STATE and the C entanglement glue:
//   - the KNOWN_HOSTS / TOFU store (trust-on-first-use), kernel RAM +
//     best-effort /CONFIG/AIDEVKH persistence, so it is unforgeable by Ring 3;
//   - the per-contract device binding table (keyed by the AI_TASK node id);
//   - the path -> device-identity glue over the existing hotplug/usb_msc layer.
// It is C for the same reason fs/graphfs/fold.c is: it touches the C hotplug
// device tables, the C process_t contract state and fat I/O, none of which Rust
// reaches in this tree. That is the stated justification, not "the surrounding
// code is C".
//
// PRIVILEGE BOUNDARY. Everything here is kernel-side. Ring 3 has NO syscall that
// writes the TOFU store or the binding table, and it cannot choose the
// descriptors a device reports (those are read from the enumerated hardware,
// kernel-side), so it cannot forge a device identity or widen a device-scoped
// grant to a different device.
#ifndef FS_ESCROW_DEVICE_H
#define FS_ESCROW_DEVICE_H

#include "../types.h"

// TOFU verdicts. MIRRORED in rustkern/capdev.rs.
#define ESCDEV_TOFU_MISMATCH  (-1)  // known logical key, DIFFERENT fingerprint
#define ESCDEV_TOFU_NEW         0   // first sight of this logical key (recorded)
#define ESCDEV_TOFU_OK          1   // known logical key, SAME fingerprint

// Confidence classes. MIRRORED in rustkern/capdev.rs. We produce MODEL, never
// SERIAL (no unique serial is read from the descriptors in this stage).
#define ESCDEV_CONF_NONE    0
#define ESCDEV_CONF_MODEL   1
#define ESCDEV_CONF_SERIAL  2

// Boot init: load the persistent KNOWN_HOSTS/TOFU store from /CONFIG. Call once,
// after the root fs is mounted. Best-effort: a missing/short file is a clean
// empty store, never a failure.
void escdev_init(void);

// Compute the device identity fingerprint + logical key for the removable
// volume backing `path`. Returns 1 and fills the outputs when `path` is on a
// mounted removable volume; 0 when it is on the root / a non-removable fs (in
// which case there is NO device identity). The full fingerprint is never 0 for
// a real device. Any out pointer may be NULL.
int escdev_fp_for_path(const char *path, uint64_t *fp_out, uint64_t *logical_out,
                       int *conf_out);

// TOFU: observe (logical_key, fp) for a device being bound. First sight records
// it and returns ESCDEV_TOFU_NEW; a later exact match returns ESCDEV_TOFU_OK; a
// later fingerprint change for a KNOWN logical key returns ESCDEV_TOFU_MISMATCH
// and does NOT overwrite the trusted record. Persists (best-effort) on a new
// record.
int escdev_tofu_observe(uint64_t logical_key, uint64_t fp);

// Per-contract device binding (CAP_SCOPE_DEVICE), keyed by the AI_TASK node id
// (unique per contract; a forked child shares the node so it inherits the
// binding for free, exactly like the grant). Returns 0 on success, -1 if the
// binding table is full (the caller then fails the enter closed).
int  escdev_bind_set(uint32_t task_node, uint64_t fp);
// 1 and fills *fp_out if the contract is device-scoped, else 0.
int  escdev_bind_get(uint32_t task_node, uint64_t *fp_out);
// Clear a contract's binding on close. Idempotent.
void escdev_bind_clear(uint32_t task_node);

// The device-scope enforcement decision for an escrow-actor write, called from
// escrow_fs_guard() AFTER the path/grant checks pass. Returns 1 to ALLOW, 0 to
// DENY. A contract that is NOT device-scoped returns 1 immediately (Stage 1-3
// behaviour is unchanged). A device-scoped contract returns 1 only if `path`
// resides on the SAME device the grant bound to and that device's identity
// still matches; a path off the bound device, the bound device being gone, or
// its identity having changed (BadUSB) all return 0 (fail closed).
int escdev_check_write(uint32_t task_node, const char *path);

// TEST HOOK. Overrides the live fingerprint escdev_fp_for_path derives, to model
// a device re-enumerating with a changed identity (BadUSB) or a different device
// at the same mount, DETERMINISTICALLY in a headless VM where a real re-plug is
// not scriptable. `present`==0 models the bound device being gone. Compiled to a
// no-op body unless the build flag ESCROW_DEVICE_SELFTEST is set, so it cannot
// affect a production kernel.
void escdev_test_set_override(int enable, uint64_t fp, int present);

// Gated boot self-test: proves fingerprint determinism + BadUSB detection + the
// TOFU verdicts + the guard decision (allow on the bound device, deny on a
// different device, fail closed on identity change / device gone), and, when a
// real removable volume is mounted, derives its REAL fingerprint to prove the
// identity source. No-op unless the build flag ESCROW_DEVICE_SELFTEST is set.
void escdev_selftest(void);

#endif // FS_ESCROW_DEVICE_H
