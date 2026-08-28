// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// contract.h - the per-app tool-contract API (#233).
//
// WHAT THIS IS FOR
//
// Three consumers, one description:
//   1. the AI tool layer, which needs to enumerate what an app can do;
//   2. a test harness, which needs to ask "did that control do anything?"
//      without injecting a pixel click and reading a screendump;
//   3. the app's OWN drawing, hit-testing and persistence, which is what
//      makes the first two trustworthy.
//
// TWO INVARIANTS. THE SECOND IS THE HARD ONE.
//
//   NON-DRIFT     the contract must not disagree with the app.
//   COMPLETENESS  the contract must cover EVERYTHING the app can do. No
//                 setting, action or readable state may exist in an app
//                 without appearing in its contract.
//
// A hand-written contract file satisfies neither for longer than one commit.
// It is wrong the first time somebody adds a control and forgets, and a
// linter only tells you afterwards. So the mechanism here is:
//
//   *** DERIVE THE CONTRACT FROM THE STRUCTURE THE APP ALREADY USES TO
//       DRAW AND HIT-TEST ITS CONTROLS. ***
//
// That is what ct_contract_t.project is for, and it is the preferred form.
// Calculator is the worked example: its every button already lives in one
// const btn_t table that BOTH draw_buttons() and hit_button() walk, so its
// contract is that table projected. A button absent from the table cannot be
// drawn, cannot be clicked, and cannot be described - completeness is
// STRUCTURAL, not checked, and no gate is needed because the fault is not
// expressible. This is the same move as #231 (widget-persistence hash derived
// from the serializer's own output instead of hand-maintained, after it had
// silently omitted three settings) and #227 (one shared geometry pass instead
// of separately computed draw and hit coordinates, after five controls had
// drifted out of reach).
//
// Static rows (ct_contract_t.items) exist for the case where an app's control
// surface is NOT yet expressed as one table - Settings today, 8809 lines of
// mostly bespoke drawing. There, completeness is GATED rather than structural:
// tools/contract-lint/contract-lint.py fails the build when a control the app
// can draw and hit has no contract row. That is strictly weaker, it is
// honestly labelled as weaker in docs/CONTRACT_API.md, and the path to making
// it structural is to finish the #227 conversion, which fixes hit-box drift
// and brings the panel under the contract in one move.
//
// WHAT WAS HERE BEFORE, so nobody re-adds it
//
// Four partial descriptions of the Settings surface, no two agreeing:
//   - settings_save()'s seven hand-written sv_putint() lines,
//   - tools/pref-reader-lint/READERS.tsv (#230),
//   - a hardcoded if-else chain in libc/aiclient.c's settings_apply(),
//     covering a DIFFERENT key set (theme/volume/brightness, not the clock
//     format or double-click speed) and reachable only from AI chat,
//   - userland/apps/*/manifest.json: 27 files declaring actions, parameters,
//     valid values and required capabilities in detail, read by NOTHING in
//     the tree (verified 2026-08-22: every manifest.json reference in the
//     codebase is the unrelated App Store repo manifest). clock/manifest.json
//     still advertises set_display_mode and set_time_format; the clock app
//     has neither, and never did.
// A declaration nothing reads is not a contract, it is a wish.
//
// #235 removed the last two of those four. The 27 manifests are DELETED, and
// tools/contract-lint check 4 fails the build if any per-app capability
// declaration file comes back, so the artifact is inexpressible rather than
// merely absent - it had regenerated the belief that every app was already
// contracted in every reader who found it. aiclient.c's chain now DELEGATES to
// this API through contract_invoke() below instead of carrying its own copy of
// the policy. ONE description of the Settings surface remains: SETTINGS_ITEMS[]
// in userland/apps/settings/main.c.
//
// GATING (docs/CONTRACT_API.md section 4)
//
// Every item carries a risk class, decided when the item is written:
//   CT_SAFE     invocable with no token. Reversible, cosmetic, and bounded by
//               the declared range/enum. Still audited.
//   CT_GUARDED  requires an aicap capability token or a user consent grant,
//               through libc/aicap.c (#293) - the SAME gate the AI tool loop
//               uses, not a second one. Fails CLOSED with no consent callback.
//   CT_DENIED   declared but never invocable. Present so enumeration is honest
//               about the surface existing and being refused, instead of the
//               API silently pretending it is not there.
//
// HONEST BOUNDARY, do not overstate it: enforcement is in-process, at the
// contract_authorize() call inside contract_cli(), exactly as aicap.h already
// records for the AI tool loop. It bounds what the AI and a test harness can
// do through this API and leaves a per-call audit trail. It is NOT a defence
// against a hostile Ring-3 binary, which can simply not call this code, and
// today's desktop runs as uid 0 so such a binary can write the state files
// directly. The real chokepoint is the escrow in docs/CONTRACT_ARCHITECTURE.md
// (#679 non-root sessions, then #305 immutable core). This API is deliberately
// shaped so that move is mechanical: contract_authorize() is the single site.

#ifndef _MAYTERA_CONTRACT_H
#define _MAYTERA_CONTRACT_H

// ---- access flags -----------------------------------------------------
#define CT_READ    0x1
#define CT_WRITE   0x2
#define CT_RW      (CT_READ | CT_WRITE)

// ---- value types ------------------------------------------------------
enum {
    CT_BOOL   = 0,   // 0/1; lo/hi ignored
    CT_INT    = 1,   // integer in [lo,hi]
    CT_ENUM   = 2,   // index in [lo,hi]; options is "A|B|C"
    CT_ACTION = 3,   // not a value: invoked with argv, via actfn
    CT_STR    = 4    // read-only text state, via getstrfn
};

// ---- gating class -----------------------------------------------------
enum {
    CT_SAFE    = 0,
    CT_GUARDED = 1,
    CT_DENIED  = 2
};

// ---- outcome codes (contract_cli exit status, and the "code=" field) ---
enum {
    CT_OK          = 0,
    CT_ERR_UNKNOWN = 1,   // no such item
    CT_ERR_RANGE   = 2,   // value outside the declared range/enum
    CT_ERR_ACCESS  = 3,   // read-only item written, or unreadable item read
    CT_ERR_DENIED  = 4,   // CT_DENIED, or the capability gate refused
    CT_ERR_USAGE   = 5,   // malformed invocation
    CT_ERR_FAILED  = 6    // the setter/action ran and reported failure
};

#define CT_NAME_MAX  64
#define CT_DESC_MAX  128
#define CT_OPTS_MAX  192

// One row. EXACTLY ONE of {var, getfn/setfn, actfn, getstrfn} backs it.
//
// A projected row (see ct_contract_t.project) must own its strings for the
// life of the call; projecting from a const table in the app satisfies that
// trivially, which is another reason to project from the real table rather
// than build strings on a stack.
struct ct_item;

typedef struct ct_item {
    const char   *name;      // dotted, stable, e.g. "clock.use_24hour"
    unsigned char type;      // CT_BOOL / CT_INT / CT_ENUM / CT_ACTION / CT_STR
    unsigned char access;    // CT_READ | CT_WRITE
    unsigned char risk;      // CT_SAFE / CT_GUARDED / CT_DENIED
    char          cfgkey;    // single-char persistence key, 0 if not persisted
    int           lo, hi;    // inclusive bounds for CT_INT / CT_ENUM
    const char   *options;   // "A|B|C" for CT_ENUM, else 0
    int          *var;       // THE variable the app's own UI reads and writes
    int         (*getfn)(void);                       // used when var == 0
    int         (*setfn)(int v);                      // 0 on success
    int         (*actfn)(const struct ct_item *it, int argc, char **argv,
                         char *out, int ocap);
    int         (*getstrfn)(char *out, int ocap);     // CT_STR
    // Identity payload for a PROJECTED row: the app points this at the entry
    // in its own table that the row was projected from, so one shared actfn
    // can serve every projected action without a translation table (which
    // would itself be a new place to drift). Calculator points it at the
    // btn_t's action token.
    const void   *ctx;
    const char   *cap;       // aicap capability id; REQUIRED when risk==CT_GUARDED
    const char   *desc;      // one line, >= 12 chars (contract-lint enforces)
} ct_item_t;

typedef struct ct_contract {
    const char      *app;     // lowercase id, e.g. "settings"
    const char      *title;   // human name
    const char      *note;    // one line of honest scope, printed by describe

    // STATIC rows. Use only where the app's surface is not (yet) one table.
    const ct_item_t *items;
    int              n;

    // DYNAMIC PROJECTION - the preferred form. Fill *out for index idx and
    // return 1; return 0 when idx is past the end. Because the projection
    // reads the app's own draw/hit-test structure, a feature that is not in
    // that structure cannot be drawn AND cannot be described, so completeness
    // holds by construction rather than by inspection.
    int            (*project)(int idx, ct_item_t *out);

    void           (*load)(void);    // pull persisted state in (may be 0)
    void           (*commit)(void);  // persist + apply after a write (may be 0)
} ct_contract_t;

// ---- table walkers the app itself uses (this is what kills the drift) ---

// Total row count: static rows plus projected rows.
int  contract_count(const ct_contract_t *c);

// Fetch row idx into *out (static rows first, then projected). 0 if idx is
// out of range.
int  contract_at(const ct_contract_t *c, int idx, ct_item_t *out);

// Find by dotted name / by persistence key. Fills *out; returns 0 if absent.
int  contract_find(const ct_contract_t *c, const char *name, ct_item_t *out);
int  contract_by_key(const ct_contract_t *c, char cfgkey, ct_item_t *out);

// Read/write an item's backing store WITHOUT gating or auditing. These are
// for the owning app's own persistence path (settings_save/settings_load),
// which is inside the trust boundary by definition. Outside callers go
// through contract_cli().
int  contract_get(const ct_item_t *it);
int  contract_put(const ct_item_t *it, int v);

// A change signature over every persisted (cfgkey != 0) item, for autosave.
// DERIVED, so adding a row cannot leave a hand-written hash behind - the
// exact fault #231 removed from the widget serializer.
int  contract_hash(const ct_contract_t *c);

// ---- the invoke path ---------------------------------------------------

// True if argv contains "--contract". Lets an app bail out before it creates
// a window, so a contract call never paints anything and never needs the
// compositor.
int  contract_is_invocation(int argc, char **argv);

// The single authorization site. SAFE allows; GUARDED goes to aicap_authorize()
// (libc/aicap.c, #293); DENIED refuses. Every outcome is audited to
// /CONFIG/AIAUDIT.LOG through aicap_audit(). Returns CT_OK or CT_ERR_DENIED.
// Exposed so an in-process caller (the AI tool loop) uses the same gate as
// the CLI rather than a second copy of the policy.
int  contract_authorize(const ct_contract_t *c, const ct_item_t *it,
                        const char *args, char *reason, int rcap);

// Run the contract verb in argv and return an exit code (CT_*). Only call
// when contract_is_invocation() is true. Writes machine-readable lines to
// stdout, which the kernel's fd-1 fallback (kernel/proc/fdlayer.c:1468) also
// puts on the serial console, so a harness with no GUI can read the result.
//
// Verbs:
//   --contract probe                    "contract: <app> items=<n>"
//   --contract list                     one "<name>" per line
//   --contract describe                 the YAML-subset declaration
//   --contract get  <name>
//   --contract set  <name> <value>
//   --contract call <name> [args...]
int  contract_cli(int argc, char **argv, const ct_contract_t *c);

// ---- the CALL path: reaching ANOTHER app's contract (#235) --------------
//
// The mirror of contract_cli(). contract_cli() is how an app ANSWERS; this is
// how a caller ASKS. It spawns the target app's own binary with
// `--contract <verb> ...`, captures stdout and hands back the app's own reply
// lines verbatim.
//
// This lives in libc, not in ctl, because there are now two callers - the ctl
// debug client and libc/aiclient.c's settings.get/settings.set tools - and a
// second copy of "how do I reach an app's contract" would be the same fault
// this whole ticket is about. A third caller (a test harness) gets it free.
//
// app       lowercase contract id ("settings"), resolved to a binary through
//           /AITOOLS/INDEX.YML, falling back to the /APPS/<UPPERCASE> launch
//           convention. Do NOT pass a path; deriving it from the convention
//           rather than a second name table is deliberate (a hardcoded map is
//           the phantom that shipped /APPS/COMPOSITOR while the kernel
//           launched /APPS/COMPOSIT).
// argv/argc the verb and its arguments, WITHOUT the leading "--contract".
// out/ocap  the app's stdout, NUL-terminated and truncated to fit.
//
// Returns the app's exit status (a CT_* code), or CT_ERR_USAGE if the binary
// could not be spawned at all. A caller that needs to distinguish "the app said
// err" from "the app was not there" checks for a leading "err " in out.
//
// NOT for the app's own controls: an app reaches its OWN contract through
// contract_find()/contract_get()/contract_put(), in-process, with no spawn.
int  contract_invoke(const char *app, int argc, char **argv, char *out, int ocap);

// The binary contract_invoke() would spawn for `app`. Exposed so `ctl path`
// and any diagnostic report the same answer the call path actually uses,
// rather than recomputing the convention.
void contract_app_path(const char *app, char *out, int ocap);

#endif // _MAYTERA_CONTRACT_H
