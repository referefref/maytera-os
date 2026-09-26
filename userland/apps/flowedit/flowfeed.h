// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// flowfeed.h - Maytera Flow "download example flows" feed model (#153).
//
// The CLIENT side of update-server flow distribution for /APPS/FLOW. It reuses
// the App Store / updater trust model WITHOUT WEAKENING IT:
//
//   * the flows MANIFEST is a signed JSON document, authenticated with the SAME
//     detached-signature mechanism the App Store uses. Verification is done by
//     pkgsig_verify_manifest() (userland/libc/pkgsig.h), which hands sha256 of
//     the manifest to the kernel (SYS_OTA_VERIFY_SIG) to be checked against the
//     public key BAKED INTO the kernel image (kernel/proc/ota_pubkey.h). That is
//     the trust anchor: a Ring-3 app cannot swap the key, and the same signing
//     tool that signs the App Store manifest signs this one.
//   * every flow YAML carries a per-entry sha256 in the (now trusted) manifest,
//     verified with pkgsig_verify_package() before a single byte is installed.
//   * the YAML is then parse-VALIDATED with the runner's own flow_parse_bytes()
//     (flow.c, already linked into flowedit) before it is written to disk, so a
//     flow that would not run is never installed.
//
// This header declares only the NETWORK-INDEPENDENT logic (manifest JSON parse
// + install-name sanitisation + collision naming), so it is host-unit-testable
// (see feedtest.c). The HTTP fetch, signature/sha256 verify calls, UI and file
// install live in main.c and call these.

#ifndef _FLOWEDIT_FLOWFEED_H
#define _FLOWEDIT_FLOWFEED_H

// Maximum offered flows we will parse from one manifest.
#define FLOWFEED_MAX   64

// Field caps. name is kept short so "<name>.yml" and "<name>-NN.yml" both fit a
// FLOW_ID_MAX (48) install name with room to spare.
#define FF_NAME_MAX    41
#define FF_DESC_MAX    192
#define FF_PATH_MAX    224
#define FF_SHA_MAX     65    // 64 hex chars + NUL

typedef struct {
    char name[FF_NAME_MAX];   // logical flow name (also the install basename)
    char desc[FF_DESC_MAX];   // one-line human description
    char path[FF_PATH_MAX];   // repo-relative path to the flow YAML
    char sha256[FF_SHA_MAX];  // 64-char lowercase hex; per-entry integrity hash
} flowfeed_entry_t;

// Parse a SIGNATURE-VERIFIED flows manifest (JSON) into out[0..max).
// The manifest MUST already have passed pkgsig_verify_manifest(); this function
// only extracts fields and does NOT itself establish trust.
//
// Recognised shape (matches the App Store manifest conventions):
//   { "feed": "...", "version": 1,
//     "flows": [ { "name":"..", "description":"..", "path":"..",
//                  "sha256":"<64 hex>", "author":"..(optional).." }, ... ] }
//
// An entry is kept only if it has a non-empty name AND a 64-char sha256 (an
// entry we cannot integrity-check is dropped rather than trusted). Returns the
// number of usable entries (>= 0), or -1 if the manifest has no "flows" array
// (err set). err may be NULL.
int flowfeed_parse_manifest(const char *json, int len,
                            flowfeed_entry_t *out, int max,
                            char *err, int errcap);

// Turn a manifest-supplied name into a safe install BASENAME (no extension).
// Accepts only [A-Za-z0-9._-], collapses everything else to '-', refuses a
// leading '.', a "..", an empty result, and anything containing a path
// separator. This is defence in depth: the manifest is signed, but the name is
// used to build a filesystem path, so it must never be able to escape
// /CONFIG/WORKFLOWS/. Writes at most cap-1 chars. Returns 0 on success, -1 if
// the name cannot be made safe (out[0] = 0).
int flowfeed_sanitize_name(const char *in, char *out, int cap);

#endif // _FLOWEDIT_FLOWFEED_H
