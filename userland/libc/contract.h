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

// ===========================================================================
// THE CONTRACT TERMS: timing, success/failure, depth/promise (#<config-ref>).
//
// WHY THESE THREE AND NOT MORE. A row used to say WHAT a control is and WHO may
// touch it, and nothing about what happens when you touch it. Three specific
// consequences, each of which had already cost something:
//
//  TIMING. A caller could not tell a 2 ms toggle from a 40 s reindex, so it
//    could neither pick a timeout nor decide whether to wait at all. The
//    measured instance is the spawn path: contract_invoke() does an UNBOUNDED
//    sys_waitpid() (libc/ctinvoke.c, and the kernel ignores WNOHANG, see
//    libc/sys/wait.c), so an action that does not return promptly wedges the
//    caller with no timeout and no recovery. contract_declared() (#469m) closed
//    the "app not in the index" half of that. The half left open is an action
//    that is REACHABLE and simply does not finish, and the only thing that can
//    close it is the action SAYING SO IN ADVANCE. So CT_TIME_UNBOUNDED and
//    CT_TF_MODAL are REFUSED on the spawn path (contract.c ct_do_call) instead
//    of being allowed to block: an honest refusal a caller can read beats a
//    hang it cannot.
//
//  SUCCESS / FAILURE. "The action returned 0" is not the claim "the folder now
//    exists". A caller that cannot check the postcondition independently has to
//    trust the return code, and a return code is the one thing a broken action
//    is most likely to get right. success + success_ref name something the
//    caller may go and LOOK AT; failures[] names the modes this row can produce
//    so a caller can distinguish "refused" from "ran and could not".
//
//  DEPTH / PROMISE. How far the effect reaches, and the postcondition the
//    action commits to. The promise machinery ALREADY EXISTS, at the escrow
//    layer (libc/escrow.h, docs/AI_ESCROW_PROMISE.md, #712): declare
//    postconditions, run the work under a kernel-enforced scoped grant, then
//    VERIFY the postconditions and roll back if they do not hold. What it
//    lacked was a way for ONE MENU ACTION to state its own. So ct_promise_t is
//    deliberately shaped in the escrow's OWN vocabulary (a path that must
//    exist, a relocation that must have landed bytes-intact) and is handed
//    STRAIGHT to escrow_promise_add_folder() / escrow_promise_add_move() by
//    libc/ctpromise.c. There is no second promise system, no second verifier
//    and no second audit trail; a fourth .ICN loader is what that looks like.
//
// WHY MOST ROWS DECLARE NOTHING, AND THAT IS CORRECT. For a value row
// (CT_BOOL/CT_INT/CT_ENUM/CT_STR) all three terms are DERIVABLE from what the
// row already says: writing an int is instant, its postcondition is "read it
// back and it equals what you wrote", and its depth is "a persisted preference"
// exactly when it has a cfgkey. contract_terms_of() derives them, so Settings'
// 160-odd rows need no hand-written terms and therefore cannot carry a WRONG
// one. Only CT_ACTION rows must declare, because for an action none of the
// three is derivable, and contract-lint fails the build on an action row that
// does not. That split is the same judgement as contract_hash() and
// contract_save_cfg(): derive what is derivable, and require a declaration only
// where information genuinely has to be added.
// ===========================================================================

// ---- timing -----------------------------------------------------------
// The class is the bucket a caller reasons about; budget_ms is the number it
// uses. A class with budget_ms == 0 means "the class ceiling", not "unbounded".
enum {
    CT_TIME_UNSPEC    = 0,  // NOT DECLARED. contract-lint fails an action row.
    CT_TIME_INSTANT   = 1,  // <= 10 ms: a variable write, a mode toggle
    CT_TIME_FAST      = 2,  // <= 250 ms: a repaint, a small file
    CT_TIME_SLOW      = 3,  // <= 5 s: a per-pixel filter, a directory walk
    CT_TIME_LONG      = 4,  // <= 60 s: a reindex, a large transfer
    CT_TIME_UNBOUNDED = 5   // no bound the app can honestly state. A caller
                            // MUST NOT wait on this synchronously, and the
                            // spawn path REFUSES it rather than blocking.
};
#define CT_TIME_MS_INSTANT   10
#define CT_TIME_MS_FAST     250
#define CT_TIME_MS_SLOW    5000
#define CT_TIME_MS_LONG   60000

// timing flags
#define CT_TF_LIVE_ONLY 0x1  // meaningful only inside a RUNNING instance; a
                             // stateless spawn can answer but not achieve it
#define CT_TF_ASYNC     0x2  // returns once the work is ARMED, not once done;
                             // completion is driven by the app's own tick
#define CT_TF_MODAL     0x4  // opens a dialog and waits for a human. REFUSED on
                             // the spawn path: nobody is there to answer it.

// ---- success: what the caller may go and CHECK -------------------------
enum {
    CT_SUCC_UNSPEC = 0,  // NOT DECLARED. contract-lint fails an action row.
    CT_SUCC_RC     = 1,  // the return code is all there is. Honest, and weak:
                         // say so rather than implying a postcondition.
    CT_SUCC_ITEM   = 2,  // re-read contract item success_ref in THIS app
    CT_SUCC_PATH   = 3,  // the path success_ref (a template, see below) exists
    CT_SUCC_ARMED  = 4   // a state machine was armed; the RESULT is not
                         // observable from this call and success_ref names the
                         // item that will show it later
};

// ---- failure taxonomy: the modes THIS row can produce ------------------
// A bitmask, not a string, so a caller can branch on it. Distinct from the
// CT_ERR_* outcome codes, which say what the ENGINE decided; these say what the
// ACTION's own implementation can fail at.
#define CT_FAIL_DENIED      0x001  // a capability gate refused it
#define CT_FAIL_RANGE       0x002  // an argument was out of the declared range
#define CT_FAIL_NOTFOUND    0x004  // the thing it operates on is not there
#define CT_FAIL_BUSY        0x008  // a conflicting operation is in flight
#define CT_FAIL_NOSPACE     0x010  // out of disk or out of a fixed-size table
#define CT_FAIL_IO          0x020  // the read or write itself failed
#define CT_FAIL_NEEDSLIVE   0x040  // there is no live document to act on
#define CT_FAIL_TIMEOUT     0x080  // it gave up waiting on something external
#define CT_FAIL_UNSUPPORTED 0x100  // declared, but this build cannot do it
#define CT_FAIL_PARTIAL     0x200  // some of the work landed and some did not

// ---- depth: how far the effect reaches --------------------------------
enum {
    CT_DEPTH_UNSPEC = 0,  // NOT DECLARED. contract-lint fails an action row.
    CT_DEPTH_VIEW   = 1,  // the view only; nothing outlives the window
    CT_DEPTH_PREF   = 2,  // a persisted preference of this app
    CT_DEPTH_DOC    = 3,  // the open document
    CT_DEPTH_FS     = 4,  // the filesystem outside this app's own config
    CT_DEPTH_SYSTEM = 5   // system-wide, or another process
};

// ---- the promise: postconditions, in the ESCROW's own vocabulary -------
//
// Each post is something escrow_verify() (libc/escrow.c) ALREADY knows how to
// check, so libc/ctpromise.c is a translation and not a second oracle:
//   CT_POST_PATH  -> escrow_promise_add_folder(): this path must EXIST after
//   CT_POST_MOVED -> escrow_promise_add_move():   a must be GONE, b must EXIST
//                    with a's bytes
//   CT_POST_ITEM  -> NOT an escrow post. A contract item in this app must read
//                    ival after. Checked by contract.c re-reading the item,
//                    because the escrow oracle is a filesystem oracle and
//                    pretending otherwise would be the overstatement this file
//                    keeps warning about.
//
// PATH TEMPLATES use the SAME vocabulary as ct_cap_need_t.scope_tmpl, on
// purpose: one resolver, not two.
//   "self"        the app's own window (not a path; invalid for a post)
//   "doc"         the app's current-document path
//   "arg:<name>"  a path taken from the action's own named argument
//   "argv:<n>"    a path taken from the action's positional argument n
//   "home:<rel>"  <the calling user's home>/<rel>
//   "item:<name>" the value of a CT_STR item in this app, read AFTER the action
// A literal absolute path is also accepted, and is the one case to think twice
// about: a literal is something the declaration chose, not something derived
// from the call, which is exactly the shape a fictional manifest row has.
enum {
    CT_POST_END   = 0,   // terminator
    CT_POST_PATH  = 1,
    CT_POST_MOVED = 2,
    CT_POST_ITEM  = 3
};
typedef struct ct_post {
    unsigned char kind;   // CT_POST_*
    const char   *a;      // path template, or item name for CT_POST_ITEM
    const char   *b;      // destination template for CT_POST_MOVED, else 0
    int           ival;   // required value for CT_POST_ITEM
} ct_post_t;

typedef struct ct_promise {
    const char      *note;        // recorded verbatim in the audit trail
    const char      *scope_tmpl;  // the escrow SCOPE: the one prefix the work
                                  // may write inside. REQUIRED when any post is
                                  // CT_POST_PATH/CT_POST_MOVED, because the
                                  // kernel escrow grant is scoped and there is
                                  // deliberately no unscoped mode.
    long             ttl_secs;    // 0 = ESCROW_TTL_DEFAULT
    const ct_post_t *posts;       // terminated by {CT_POST_END}
} ct_promise_t;

typedef struct ct_terms {
    unsigned char       timing;       // CT_TIME_*
    unsigned char       tflags;       // CT_TF_*
    unsigned short      budget_ms;    // 0 = the class ceiling
    unsigned char       success;      // CT_SUCC_*
    unsigned char       depth;        // CT_DEPTH_*
    unsigned int        failures;     // CT_FAIL_* bitmask
    const char         *success_ref;  // item name or path template
    const ct_promise_t *promise;      // 0 = this action promises nothing
} ct_terms_t;

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
    // THE CONTRACT TERMS (timing / success+failure / depth+promise). APPENDED
    // AT THE END OF THE STRUCT DELIBERATELY: every shipped row initialiser is
    // POSITIONAL (measured: SETTINGS_ITEMS[], PAINT_ITEMS[], TERMINAL_ITEMS[],
    // SNAPSHOT_ITEMS[] are all positional, not C99-designated), so a field
    // inserted anywhere else would silently shift `desc` into `needs` and pass
    // the compiler. Appending leaves every existing row with terms == 0, which
    // reads as "not declared" and is exactly right: a value row has its terms
    // DERIVED (contract_terms_of), and an action row without them fails
    // contract-lint. 0 never reads as a false claim.
    const ct_terms_t *terms;
    // THE ROW'S OWN PERSISTENCE WRITER, called by contract_cli()'s set path
    // immediately before ct_contract_t.commit(). APPENDED AT THE END for the
    // same reason `terms` was: every shipped row initialiser is POSITIONAL, so
    // a field inserted anywhere else would silently shift the one after it.
    //
    // WHY IT EXISTS, and it is a real gap rather than a convenience.
    // ct_contract_t.commit() is ONE hook for a whole app, which is correct for
    // an app whose settings all live in one file. Settings does not:
    // SETTINGS.CFG, ALERTS.CFG and STARTMENU.CFG each have their own writer,
    // and Terminal has three. A projected row therefore has to be able to say
    // WHICH writer persists it. Without this, a contract write moved the
    // variable, the app-wide commit wrote a file that did not contain that key,
    // and the change was silently lost at the next launch: a control both
    // un-applied and un-saved, which is #786's exact shape.
    //
    // 0 means "commit() is all there is", which is every pre-existing row, so
    // nothing changes for an app that does not need it.
    void (*commitfn)(void);
} ct_item_t;

// ===========================================================================
// CT_CHECK_ACTFN(fn) - a leading-parameter mismatch is a WARNING, not an
// ERROR, and that gap is real (task 3 phase 3, blame.md 2026-10-01).
//
// actfn's signature carries `it` FIRST: int (*)(const struct ct_item *it,
// int argc, char **argv, char *out, int ocap). A row whose handler was
// written as int fn(int argc, char **argv, char *out, int ocap) - the
// easy mistake when an app has ONE actfn per row instead of ONE SHARED
// actfn reading it->ctx (contract.h's own preferred shape) - still compiles:
// assigning a 4-parameter function pointer into a 5-parameter field is
// -Wincompatible-pointer-types, which `-Wall -Wextra` (every app Makefile)
// reports but does not fail the build on, because none of them also pass
// -Werror. MEASURED: writer/sheets/slides/browser's main.c each produced
// one such warning per row (24-26 lines) on the FIRST compile of this
// tranche, and the binary still linked. On x86-64 System V, the callee then
// reads `it` out of the register the caller never loaded with it, and reads
// `argc`/`argv`/`out`/`ocap` one slot early - a silently wrong call, not a
// crash, which is the worst kind of wrong call to ship.
//
// CT_CHECK_ACTFN(fn) turns that warning into a build-stopping error,
// regardless of the app's Makefile flags, because _Static_assert is not
// gated by -Werror. Place one line per actfn, anywhere at file scope AFTER
// the function is declared:
//
//   static int run_thing(const ct_item_t *it, int argc, char **argv,
//                         char *out, int ocap) { ... }
//   CT_CHECK_ACTFN(run_thing);
//
// PROVEN BOTH WAYS (docs/CONTRACT_COVERAGE_PLAN.md section 11): reverting
// one wrapper to the 4-parameter shape and keeping its CT_CHECK_ACTFN line
// turns the prior silent warning into
//   error: static assertion failed: "run_thing does not match ..."
// and restoring the correct signature compiles clean again.
//
// NOT YET WIRED INTO EVERY CONTRACTED APP. Applying it is mechanical (one
// line per actfn) and is done for writer/sheets/slides/browser as of this
// commit; calc/convert/files/paint/settings/snapshot/terminal/timers
// predate it and are a named backlog item, not silently assumed covered.
// A contract-lint rule requiring a CT_CHECK_ACTFN line beside every actfn
// assignment would close that gap structurally instead of per-app; recorded
// here rather than built, because it needs the same "scoped to this app's
// own source, comments/strings blanked" care check 8 needed, and that is
// real work this tranche did not have room for.
#define CT_CHECK_ACTFN(fn) \
    _Static_assert(__builtin_types_compatible_p(__typeof__(&(fn)), \
        int (*)(const struct ct_item *, int, char **, char *, int)), \
        #fn " must be declared int " #fn \
        "(const struct ct_item *it, int argc, char **argv, char *out, int ocap) " \
        "- actfn takes the ct_item* FIRST, see contract.h CT_CHECK_ACTFN")

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
// the per-user AIAUDIT.LOG through aicap_audit(). Returns CT_OK or CT_ERR_DENIED.
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


// ===========================================================================
// THE TERMS API (#<config-ref>)
// ===========================================================================

// The terms for `it`, DECLARED or DERIVED. Fills *out and returns 1 when the
// terms are known, 0 when they are not (an action row with no terms, which
// contract-lint fails the build on, and which the engine treats as unbounded
// and therefore refuses on the spawn path rather than guessing).
//
// DERIVATION, and the reason it is derivation and not a default: for a value
// row every term follows from what the row already says, so writing them out by
// hand would add a place to be wrong without adding information.
//   CT_BOOL/CT_INT/CT_ENUM  timing INSTANT, success CT_SUCC_ITEM on ITSELF
//                           (read it back), depth PREF when cfgkey != 0 else
//                           VIEW, failures RANGE|DENIED.
//   CT_STR (read-only)      timing INSTANT, success CT_SUCC_RC, depth VIEW.
//   CT_ACTION               NOT derivable. Returns 0 unless declared.
int contract_terms_of(const ct_item_t *it, ct_terms_t *out);

// The millisecond budget a caller should allow for `it`: the declared
// budget_ms, else the ceiling of the declared class, else 0 meaning "do not
// wait synchronously". ONE definition, so the CLI, the AI loop and a harness
// cannot disagree about how long to give an action.
int contract_budget_ms(const ct_item_t *it);

// "denied|range|io" for a failures bitmask, into out[]. Returns out.
const char *contract_failures_str(unsigned int mask, char *out, int ocap);

// ---- the PROMISE BACKEND seam ------------------------------------------
//
// contract.c does NOT call the escrow directly, and the reason is measurable
// rather than stylistic: escrow_contract_t is ~330 KB of BSS (512 file slots +
// 384 move slots), every app links libc.a statically, and app ELFs are a single
// RWX PT_LOAD whose .bss growth has broken the loader before. So the escrow is
// reached through a registered backend: an app with a promised item calls
// contract_use_escrow_promises() (libc/ctpromise.c) and pays for the escrow;
// an app with none never references it and does not.
//
// FAIL CLOSED, and this is the part that makes the promise real rather than
// decorative: if a row declares a promise with filesystem postconditions and NO
// backend is registered, ct_do_call() REFUSES the action. It does not run it
// unverified. A promise nothing checks is worse than no promise, because the
// declaration is what a caller believes.
typedef int (*ct_promise_open_fn)(const ct_contract_t *c, const ct_item_t *it,
                                 int argc, char **argv, char *why, int wcap);
typedef int (*ct_promise_settle_fn)(const ct_contract_t *c, const ct_item_t *it,
                                    char *how, int hcap);
void contract_set_promise_backend(ct_promise_open_fn open_fn,
                                  ct_promise_settle_fn settle_fn);

// True while contract_run_verb() is serving a LIVE instance (set by
// contract_live_poll). The spawn path and the live path must answer the same
// verb identically EXCEPT where liveness is the whole question, which is
// exactly CT_TIME_UNBOUNDED / CT_TF_MODAL / CT_TF_LIVE_ONLY. One flag, set at
// the one dispatch site, rather than a second engine.
int  contract_serving_live(void);
void contract_set_serving_live(int on);

#endif // _MAYTERA_CONTRACT_H
