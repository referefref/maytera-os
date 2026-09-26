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
    CT_ERR_FAILED  = 6,   // the setter/action ran and reported failure
    // ---- live-instance delivery (tier 2 wire) outcomes -------------------
    CT_LIVE_NONE    = 7,  // no running instance of the app is listening
    CT_LIVE_TIMEOUT = 8   // a live instance exists but did not answer in time
};

#define CT_NAME_MAX  64
#define CT_DESC_MAX  128
#define CT_OPTS_MAX  192

// ---- capability binding (tier 2, docs/AI_ACTION_CAPABILITY_BINDING.md 3.3) --
//
// The kernel-capability an action's IMPLEMENTATION actually reaches, declared
// on the row so the tier-2 union (3.4) is machine-readable and so a lint can
// fail the build on a declared-but-unreached capability (#235 fictional-token
// re-armed). This is NOT the same field as ct_item.cap: `cap` is the aicap id
// that bounds what the AI may do THROUGH the contract API (the in-process
// gate); `needs` is the KERNEL capability the app's own code calls a gated
// syscall for (the real teeth, 4.4). A cooperative EDIT action (add_text,
// invert) that the app performs through its own primitives needs NO system
// capability, so its `needs` list is empty - that emptiness is section 0.1
// made machine-readable. A PERCEIVE action (screen_check) needs
// `screen.capture`, and THAT is where the kernel gate bites.
//
// scope_tmpl is a TEMPLATE the caller/kernel resolves at grant time from a
// source the app does not freely control, never a literal the manifest picked:
//   "self"       -> the app's own window
//   "doc"        -> the app's current-document path
//   "arg:<name>" -> a path taken from the action's own named argument
// All-zero (needs == NULL) means "needs nothing", which every pre-tier-2 row
// already is, so the field is backward compatible.
typedef struct ct_cap_need {
    unsigned char cap;         // kernel CAP_* class (kernel/proc/caps.h), or 0
    unsigned char scope_kind;  // kernel CAP_SCOPE_* kind, or 0
    const char   *scope_tmpl;  // "self" / "doc" / "arg:<name>", or 0
} ct_cap_need_t;

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
    // tier 2: the kernel capability the action's own code reaches, 0-terminated
    // by {cap==0}. NULL means "needs nothing" (every pre-tier-2 row). See
    // ct_cap_need_t above and docs/AI_ACTION_CAPABILITY_BINDING.md 3.3/3.4.
    const ct_cap_need_t *needs;
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

// ---- derived per-app persistence (#239) --------------------------------
//
// Write / read every cfgkey row as one "<key>=<int>" line in the per-user
// preference file <home>/CONFIG/<name>, through libc/userconf.c - the ONE
// place that knows where a per-user preference lives.
//
// DERIVED FROM THE TABLE, for the same reason contract_hash() is: a row that
// gains a cfgkey is persisted and restored in the same commit, with no second
// key list to update. That is the fault #231 removed from the widget
// serializer (a hand-maintained hash had silently stopped watching three
// settings) and the fault #230 removed from the preference readers.
//
// This lives in libc rather than in an app because #239 wired THREE apps at
// once (timers, convert, snapshot) and each of them needed exactly this. Three
// private copies of "save my preferences" is how the four disagreeing
// descriptions of the Settings surface came to exist in the first place.
//
// contract_load_cfg() RANGE-CHECKS every value against the row's own declared
// lo/hi before writing it, because a config file is INPUT: convert's unit
// indices address a per-category table of 3 to 8 entries, and an unchecked 7
// read from a corrupt file would index a 3-entry table. Rows are applied in
// TABLE ORDER, so a row whose range depends on an earlier row (convert's
// from_unit depends on category) must appear after it.
//
// Rows that have no integer backing store (CT_STR, CT_ACTION) are skipped;
// give them cfgkey 0.
//
// Returns 0 on success, -1 on failure. A failed save means the file on disk is
// NOT known to hold the values.
int  contract_save_cfg(const ct_contract_t *c, const char *name);
int  contract_load_cfg(const ct_contract_t *c, const char *name,
                       const char *legacy);

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

// ---------------------------------------------------------------------------
// #469m IS THIS APP ACTUALLY DECLARED? The guard between a model-supplied name
// and an unbounded wait.
//
// contract_app_path() deliberately falls back to the /APPS/<UPPERCASE> naming
// convention when the generated index does not list an app, which is right for
// a HUMAN typing `ctl <name>`. It is dangerous for a name the MODEL chose,
// because contract_invoke() spawns that path with --contract and then does an
// UNBOUNDED sys_waitpid() (the kernel ignores WNOHANG, see libc/sys/wait.c:33).
// An app that is not contract-aware ignores the flag, opens its window, and
// never exits, so the whole AI tool loop hangs with no timeout and no recovery.
//
// MEASURED, on the first turn of the first baseline run (#469m): asked to
// invert the image in Maytera Studio, the model emitted
// app.action {"app":"sprite", ...}; the convention resolved that to
// /APPS/SPRITE, a real GUI binary; it launched and never returned. One
// hallucinated argument wedged the loop indefinitely.
//
// Returns 1 if `app` appears in the generated index. Returns 1 ALSO when the
// index file is ABSENT, because an image without the index must keep working
// exactly as before; the guard is for a PRESENT index that does not name the
// app. Returns 0 only for "the index exists and this app is not in it".
int contract_declared(const char *app);

// Comma-separated list of the apps the generated index declares, for an error
// message that tells the caller what it COULD have asked for. Writes "" and
// returns 0 if the index is missing or empty.
int contract_declared_list(char *out, int ocap);

// ---- the LIVE-INSTANCE call path (tier 2 wire, #<config-ref>) -------------
//
// docs/AI_ACTION_CAPABILITY_BINDING.md section 4.2. contract_invoke() above
// SPAWNS a fresh process, which exits before drawing a window and so can only
// touch a stateless copy of the app (CONTRACT_API.md section 3). The live path
// delivers the SAME contract verb to an app instance ALREADY running with a
// window, which runs the action's actfn against its live document and returns
// the SAME machine-readable line contract_cli() would. It reuses the SAME
// contract engine (contract_run_verb) and therefore the SAME gate and audit -
// no drift, no second policy. Transport is a per-app request/reply mailbox in
// the caller's home; see ctlive.c for the honest ceiling (it is the same
// #679 uid-0 ceiling the whole contract API already states) and the seam to a
// kernel-gated per-window mailbox syscall pair.
//
// contract_run_verb: run ONE already-parsed verb against a contract, writing
// the same lines contract_cli() writes to whatever sink is active (fd 1, or a
// capture buffer set by contract_capture_begin). argv[0] is the item NAME for
// get/set/call; for probe/list/describe argc may be 0. Returns a CT_* code.
int  contract_run_verb(const ct_contract_t *c, const char *verb,
                       int argc, char **argv);

// Redirect contract_cli/contract_run_verb output into buf until
// contract_capture_end(). Not re-entrant (one capture at a time), which the
// single-threaded event-loop serve path guarantees.
void contract_capture_begin(char *buf, int cap);
int  contract_capture_end(void);   // returns bytes captured

// contract_live_poll: an app that opts into live delivery calls this ONCE per
// event-loop tick (after win_get_event, on the timeout path too). It checks
// the app's mailbox for a pending request keyed to c->app, and if one is
// present dispatches it through contract_run_verb against LIVE state, writes
// the reply, and returns 1 (so the caller can repaint). Returns 0 when there
// was nothing to serve. Non-blocking; never spins.
int  contract_live_poll(const ct_contract_t *c);

// contract_invoke_live: the CLIENT half. Deliver `argv` (a verb and its args,
// WITHOUT a leading "--contract") to a RUNNING instance of `app` and capture
// its reply into out[]. Returns the app's CT_* reply code, or CT_LIVE_NONE if
// no live instance is listening (the caller may then fall back to
// contract_invoke()), or CT_LIVE_TIMEOUT if one exists but did not answer.
int  contract_invoke_live(const char *app, int argc, char **argv,
                          char *out, int ocap);

#endif // _MAYTERA_CONTRACT_H
