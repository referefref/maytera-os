// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// ctpromise.c - hand a contract item's declared promise to the EXISTING escrow
// promise machinery (#<config-ref>). See ctpromise.h for what this is not.

#include "ctpromise.h"
#include "escrow.h"
#include "userconf.h"
#include "string.h"
#include "stdio.h"

// The escrow has ONE contract slot (escrow.c's g_contract), so one promised
// action at a time. That is the correct shape here: contract_run_verb() serves
// exactly one verb, and both the CLI and the live poll are single-threaded.
static escrow_contract_t  *g_open = 0;
static const ct_item_t    *g_open_it = 0;
// The call context, kept for LATE RESOLUTION (see register_posts). Valid only
// between ct_promise_open() and ct_promise_settle(), which contract.c calls
// inside one ct_do_call(), so the argv pointers cannot go stale in between.
static const ct_contract_t *g_open_c = 0;
static int                  g_open_argc = 0;
static char               **g_open_argv = 0;
// How many FILESYSTEM postconditions the row DECLARED. Kept because a promise
// that registered NONE of them verifies VACUOUSLY: see the note in
// ct_promise_settle, and the VM run that produced "promise=fulfilled paths=0/0"
// for an action that promised a file and produced nothing.
static int                  g_declared_fs = 0;

// ---------------------------------------------------------------------------
// Template resolution. ONE resolver for the vocabulary ct_cap_need_t.scope_tmpl
// and ct_promise_t already share.
// ---------------------------------------------------------------------------
// Resolve, then INSIST THE RESULT IS AN ABSOLUTE PATH.
//
// This was found by the host test (libc/tests/run_ctpromise.sh) on its first
// run, and it is worth recording because the failure looked like a bug in the
// escrow and was not. An app's CT_STR item reports a PLACEHOLDER when it has no
// value yet: Snapshot's file.last_saved answers "(unsaved)", the rig's answers
// "(none)". Resolving "item:file.last_saved" at OPEN time, before the action has
// chosen anything, therefore yielded the literal string "(none)", and the
// non-empty check accepted it. The escrow dutifully registered a postcondition
// on a path named "(none)", found it missing, and reported paths=1/3 on an action
// that had done everything right. An oracle fed a fabricated path answers
// correctly about the wrong question.
//
// An absoluteness check is the right guard rather than a placeholder denylist:
// "(none)", "(unsaved)", "" and every future spelling of "nothing yet" are all
// caught by the one rule that a postcondition is about a path, and a path starts
// at the root. An unresolved CT_POST_PATH is simply retried at settle time, by
// which point the action has chosen a real one.
static int ct_is_abs(const char *p) { return p && p[0] == '/'; }

int ct_promise_resolve(const ct_contract_t *c, const ct_item_t *it,
                       const char *tmpl, int argc, char **argv,
                       char *out, int ocap) {
    if (ocap > 0) out[0] = 0;
    if (!tmpl || !tmpl[0]) return 0;

    // A literal absolute path. Accepted, and the one form to be suspicious of:
    // a literal is something the DECLARATION chose rather than something derived
    // from the call, which is the shape a fictional manifest row has (#235). It
    // is legal because "the app's own fixed output folder" is a real case.
    if (tmpl[0] == '/') { strlcpy(out, tmpl, (size_t)ocap); return 1; }

    if (!strncmp(tmpl, "home:", 5)) {
        char dir[192];
        if (userhome_dir(tmpl + 5, dir, sizeof(dir)) != 0) return 0;
        strlcpy(out, dir, (size_t)ocap);
        return ct_is_abs(out);
    }
    if (!strncmp(tmpl, "argv:", 5)) {
        int n = 0; const char *p = tmpl + 5;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        if (n < 0 || n >= argc || !argv[n] || !argv[n][0]) return 0;
        strlcpy(out, argv[n], (size_t)ocap);
        return ct_is_abs(out);
    }
    if (!strncmp(tmpl, "arg:", 4)) {
        // "arg:<name>" matches a "<name>=<value>" argument. Deliberately the
        // same spelling ct_cap_need_t documents, so a reader learns it once.
        const char *want = tmpl + 4;
        unsigned long wl = strlen(want);
        for (int i = 0; i < argc; i++) {
            if (!argv[i]) continue;
            if (!strncmp(argv[i], want, wl) && argv[i][wl] == '=') {
                strlcpy(out, argv[i] + wl + 1, (size_t)ocap);
                return ct_is_abs(out);
            }
        }
        return 0;
    }
    if (!strncmp(tmpl, "item:", 5)) {
        // The value of a CT_STR item in THIS app, read through the contract so
        // it is the same answer a caller would get from `--contract get`.
        ct_item_t src;
        if (!c || !contract_find(c, tmpl + 5, &src)) return 0;
        if (src.type != CT_STR || !src.getstrfn) return 0;
        if (src.getstrfn(out, ocap) != 0) return 0;
        // NOT merely non-empty: see the note above ct_is_abs. A CT_STR item with
        // no value yet answers with a human placeholder, not a path.
        return ct_is_abs(out);
    }
    if (!strcmp(tmpl, "doc")) {
        // The app's current document, exposed through the contract as a CT_STR
        // item. No second convention: if the app does not declare one, the
        // template does not resolve and the promise is refused rather than
        // silently narrowed.
        ct_item_t src;
        if (c && contract_find(c, "doc.path", &src) && src.type == CT_STR && src.getstrfn
            && src.getstrfn(out, ocap) == 0 && ct_is_abs(out)) return 1;
        return 0;
    }
    (void)it;
    return 0;   // "self" is a window, not a path: invalid for a postcondition
}

// Everything up to the last '/' of `path`, i.e. the folder that must exist for
// a promised FILE to be creatable in it.
static void parent_of(const char *path, char *out, int ocap) {
    strlcpy(out, path, (size_t)ocap);
    int last = -1;
    for (int i = 0; out[i]; i++) if (out[i] == '/') last = i;
    if (last > 0) out[last] = 0;
    else if (last == 0) out[1] = 0;
}


// Register the FILESYSTEM postconditions with the escrow's OWN oracle.
//
// LATE RESOLUTION, and why it is not a hack. Some postconditions name a path the
// action itself CHOOSES: Snapshot's file.save picks the next free
// SNAP-<timestamp>.BMP slot and then reports it through its own
// file.last_saved item, so "item:file.last_saved" cannot resolve until the
// action has run. So this is called TWICE: once at open (which is when a path
// known in advance, such as the output folder, must be registered, because the
// escrow's BEFORE-snapshot is taken at open) and again at settle, before
// escrow_verify(), for the posts that could not resolve yet.
// escrow_promise_add_folder()/add_move() dedup by path, so a post that resolved
// at open is not added twice. Returns the number still unresolved.
//
// A promised FILE registers its PARENT as well as itself, which is not
// redundant: it separates "the folder was never created" from "the folder is
// there and the write failed", which is the difference between a permissions
// problem and a disk problem, and the caller has to be able to tell them apart.
static int register_posts(escrow_contract_t *ec, const ct_contract_t *c,
                         const ct_item_t *it, const ct_promise_t *pr,
                         int argc, char **argv, int at_open) {
    int unresolved = 0;
    if (!pr->posts) return 0;
    for (const ct_post_t *ps = pr->posts; ps->kind != CT_POST_END; ps++) {
        char a[256], b[256];
        if (ps->kind == CT_POST_PATH) {
            if (!ct_promise_resolve(c, it, ps->a, argc, argv, a, (int)sizeof(a))) { unresolved++; continue; }
            char par[256]; parent_of(a, par, (int)sizeof(par));
            // The PARENT must be a directory; the promised path itself need only
            // EXIST, because a contract action usually promises a FILE.
            // escrow_promise_add_path() is the widened shared primitive
            // (#<config-ref>, escrow.h), not a second checker here.
            if (par[0] && strcmp(par, a) != 0) escrow_promise_add_folder(ec, par);
            escrow_promise_add_path(ec, a);
        } else if (ps->kind == CT_POST_MOVED) {
            // A MOVE CANNOT BE REGISTERED LATE, and this is a property of the
            // escrow rather than a choice made here: escrow_promise_add_move()
            // reads the SOURCE now, to capture its size and content hash, so the
            // later check can prove the destination holds those exact bytes. By
            // settle time the source is (correctly) gone and there is nothing
            // left to hash, so a late registration silently degrades from
            // "bytes-intact" to nothing. Found by the host test: the late path
            // reported moves=0/1 deletes=1 on a move that had worked perfectly.
            //
            // So both templates must resolve at OPEN. That is not a restriction
            // in practice: a move's destination is something the CALLER names,
            // not something the action discovers. If it does not resolve, the
            // promise is REFUSED (see ct_promise_open) rather than weakened.
            if (!at_open) {
                // At SETTLE a move cannot be registered (the source is gone and
                // its bytes can no longer be hashed), but it still has to be
                // COUNTED: an unregistered move is an unmet postcondition, not an
                // absent one. Registered-at-open moves are already in the table,
                // so a nonzero n_moves means this one was handled.
                if (ec->n_moves <= 0) unresolved++;
                continue;
            }
            if (!ct_promise_resolve(c, it, ps->a, argc, argv, a, (int)sizeof(a)) ||
                !ct_promise_resolve(c, it, ps->b, argc, argv, b, (int)sizeof(b))) {
                unresolved++; continue;
            }
            if (escrow_promise_add_move(ec, a, b) != 0) unresolved++;
        }
        // CT_POST_ITEM is NOT an escrow post: see ctpromise.h. Settled separately.
    }
    return unresolved;
}

// Does the promise declare a CT_POST_MOVED? Those are the posts that must
// resolve at open, so an unresolved one is fatal rather than deferred.
static int has_move_post(const ct_promise_t *pr) {
    if (!pr->posts) return 0;
    for (const ct_post_t *ps = pr->posts; ps->kind != CT_POST_END; ps++)
        if (ps->kind == CT_POST_MOVED) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// OPEN: enter the escrow and register the declared postconditions.
// ---------------------------------------------------------------------------
int ct_promise_open(const ct_contract_t *c, const ct_item_t *it,
                    int argc, char **argv, char *why, int wcap) {
    if (wcap > 0) why[0] = 0;
    ct_terms_t t;
    if (!contract_terms_of(it, &t) || !t.promise) {
        snprintf(why, (size_t)wcap, "no promise declared");
        return -1;
    }
    const ct_promise_t *pr = t.promise;

    // Does the promise have any FILESYSTEM postcondition? A promise made only of
    // CT_POST_ITEM needs no escrow: there is nothing for a scoped write grant to
    // scope, and entering one would be theatre.
    int fs_posts = 0;
    if (pr->posts)
        for (const ct_post_t *ps = pr->posts; ps->kind != CT_POST_END; ps++)
            if (ps->kind == CT_POST_PATH || ps->kind == CT_POST_MOVED) fs_posts++;
    if (!fs_posts) { g_open = 0; g_open_it = it; return 0; }

    char scope[192];
    if (!ct_promise_resolve(c, it, pr->scope_tmpl, argc, argv, scope, (int)sizeof(scope))) {
        snprintf(why, (size_t)wcap,
                 "promise scope template '%s' does not resolve from this call; the "
                 "kernel escrow grant is scoped and there is no unscoped mode",
                 pr->scope_tmpl ? pr->scope_tmpl : "(none)");
        return -1;
    }

    // The scope prefix must EXIST before the grant can be device-bound and
    // before the BEFORE-snapshot means anything. Creating it here, OUTSIDE the
    // marked window, is correct: inside it the kernel would have to allow a
    // mkdir of the scope root itself, which is the one write that cannot be
    // checked against the scope.
    escrow_do_mkdir(0, scope);

    escrow_contract_t *ec = escrow_request(scope, pr->note ? pr->note : it->name,
                                           pr->ttl_secs);
    if (!ec) {
        snprintf(why, (size_t)wcap,
                 "kernel escrow refused the contract for scope %s (fail closed: "
                 "without the kernel grant there is no enforcement)", scope);
        return -1;
    }

    // Register the postconditions with the escrow's OWN oracle. A promised FILE
    // becomes two postconditions: its parent folder must exist (which is what
    // escrow_promise_add_folder checks) and, because the oracle has no
    // "file appears" primitive, the file path itself is registered the same way
    // and verified by the same existence check. Registering the parent as well
    // is not redundant: it distinguishes "the folder was never created" from
    // "the folder is there and the write failed", which is the difference
    // between a permissions problem and a disk problem.
    int unresolved = register_posts(ec, c, it, pr, argc, argv, 1 /* at_open */);
    g_declared_fs = fs_posts;
    if (unresolved && has_move_post(pr)) {
        // A relocation promise whose source or destination could not be pinned
        // down NOW can never be verified bytes-intact. Refuse rather than run it
        // under a promise that has quietly become weaker than it reads.
        snprintf(why, (size_t)wcap,
                 "a CT_POST_MOVED postcondition did not resolve at open time, so "
                 "the source bytes cannot be captured and the move could never be "
                 "verified intact; name both paths from the call, not from state "
                 "the action has not produced yet");
        escrow_close(ec);
        return -1;
    }

    g_open = ec;
    g_open_it = it;
    g_open_c = c;
    g_open_argc = argc;
    g_open_argv = argv;
    return 0;
}

// ---------------------------------------------------------------------------
// SETTLE: verify, report, close. Returns 0 only when EVERY declared
// postcondition holds.
// ---------------------------------------------------------------------------
int ct_promise_settle(const ct_contract_t *c, const ct_item_t *it,
                      char *how, int hcap) {
    if (hcap > 0) how[0] = 0;
    ct_terms_t t;
    int have = contract_terms_of(it, &t);
    const ct_promise_t *pr = have ? t.promise : 0;

    // ---- the CT_POST_ITEM half, checked HERE because the escrow oracle is a
    // filesystem oracle and must not be described as anything else.
    int item_total = 0, item_ok = 0;
    if (pr && pr->posts) {
        for (const ct_post_t *ps = pr->posts; ps->kind != CT_POST_END; ps++) {
            if (ps->kind != CT_POST_ITEM) continue;
            item_total++;
            ct_item_t target;
            if (c && contract_find(c, ps->a, &target) && contract_get(&target) == ps->ival)
                item_ok++;
        }
    }

    // ---- the FILESYSTEM half, checked by the escrow's own oracle.
    if (!g_open) {
        g_open_it = 0;
        if (item_total == 0) { strlcpy(how, "nothing-to-verify", (size_t)hcap); return 0; }
        snprintf(how, (size_t)hcap, "items=%d/%d fs=none", item_ok, item_total);
        return (item_ok == item_total) ? 0 : 1;
    }

    // LATE RESOLUTION: register the posts whose path the action itself chose and
    // which therefore could not resolve at open. This runs BEFORE verify, inside
    // the still-open contract, and dedups against what open already registered.
    int unresolved = 0;
    if (pr) unresolved = register_posts(g_open, g_open_c ? g_open_c : c, it, pr,
                                       g_open_argc, g_open_argv, 0 /* settle */);

    escrow_verify_t v;
    int verdict = escrow_verify(g_open, &v);

    // ---- A PROMISE WITH NO POSTCONDITIONS LEFT VERIFIES VACUOUSLY -----------
    //
    // MEASURED ON A BOOTED VM (golden 2508), and this is the fault this whole
    // ticket exists to remove, reappearing one level up. Snapshot's file.save was
    // called on a spawned copy with no capture loaded. act_save() found nothing to
    // save, returned 0, and never set the path it reports through
    // file.last_saved. So "item:file.last_saved" still read "(unsaved)" at settle,
    // did not resolve, and NO postcondition was ever registered. escrow_verify()
    // was then asked whether zero postconditions hold, answered yes, and the reply
    // line said
    //
    //     ok name=file.save ... promise=fulfilled paths=0/0 moves=0/0 deletes=0
    //
    // for an action that promised a file and produced none. The oracle was not
    // wrong; it was asked nothing.
    //
    // A promise is a statement about a NUMBER of postconditions. If the row
    // declared N filesystem postconditions and fewer than N could be pinned to a
    // real path by the time the work finished, the promise DID NOT HOLD, whatever
    // the oracle says about the ones that were registered. Reporting that as
    // fulfilled would be exactly the "returned 0 therefore it worked" claim the
    // success and promise terms exist to replace.
    if (unresolved > 0) {
        verdict = ESCROW_PARTIAL;
        // AND THE ESCROW MUST BE TOLD, not just the caller. escrow_close() reads
        // c->verdict to choose SYS_ESCROW_EXIT (fulfilled: revoke the grant early)
        // versus SYS_ESCROW_ABORT (not fulfilled: the KERNEL rolls back the
        // recorded reversible effects, then revokes). Overriding the verdict only
        // in a local would have left a vacuously-fulfilled contract taking the
        // EARLY-CLOSE path, so the work would have been kept. escrow_verify()
        // cannot reach this conclusion itself: how many postconditions the ROW
        // DECLARED is contract-layer knowledge the escrow never sees, which is the
        // one and only reason this field is written from here.
        g_open->verdict = ESCROW_PARTIAL;
    }
    int closed  = escrow_close(g_open);
    // Snapshot the declared count BEFORE clearing the contract state: the report
    // below reads it, and resetting first would have printed declared_fs=0 on
    // every line, which is the same shape of quiet zero the vacuous-fulfilled
    // fault above was made of.
    int declared_fs = g_declared_fs;
    g_open = 0; g_open_it = 0; g_open_c = 0; g_open_argc = 0; g_open_argv = 0;
    g_declared_fs = 0;

    // The verdict line is what the caller reads INSTEAD of trusting the return
    // code, so it carries the counts, not an adjective.
    int o = snprintf(how, (size_t)hcap,
                     "%s paths=%d/%d moves=%d/%d deletes=%d declared_fs=%d unresolved=%d",
                     verdict == ESCROW_FULFILLED ? "fulfilled" : "unmet",
                     v.folders_ok, v.folders_total, v.moves_ok, v.moves_total,
                     v.delete_count, declared_fs, unresolved);
    if (item_total) o += snprintf(how + o, (size_t)(hcap - o), " items=%d/%d", item_ok, item_total);
    if (v.n_unmet > 0 && o < hcap - 8)
        snprintf(how + o, (size_t)(hcap - o), " first-unmet=%s", v.unmet[0]);
    (void)closed;

    if (verdict != ESCROW_FULFILLED) return 1;
    if (item_total && item_ok != item_total) return 1;
    return 0;
}

void contract_use_escrow_promises(void) {
    contract_set_promise_backend(ct_promise_open, ct_promise_settle);
}
