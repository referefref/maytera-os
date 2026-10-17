// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// ctpromise.h - the ESCROW backend for a per-item contract promise
// (#<config-ref>, task 2 of docs/CONTRACT_COVERAGE_PLAN.md).
//
// WHAT THIS IS AND, MORE IMPORTANTLY, WHAT IT IS NOT.
//
// It is NOT a promise system. The promise system is libc/escrow.c (#712): it
// declares postconditions, runs the work under a KERNEL-enforced scoped grant
// that refuses every out-of-scope write and every delete, verifies the
// postconditions afterwards, and aborts (rolling the recorded reversible
// effects back) when they do not hold. All of that already existed and all of
// it is reached from here unchanged.
//
// What was missing was a way for ONE MENU ACTION to state its own
// postcondition. So this file is a TRANSLATION LAYER and nothing else: it
// resolves the path templates on a ct_promise_t (contract.h) and hands the
// results to escrow_request() / escrow_promise_add_folder() /
// escrow_promise_add_move() / escrow_verify() / escrow_close(). There is no
// second oracle, no second audit sink and no second verdict vocabulary here. If
// you are about to add one, the thing to do instead is to widen escrow.c so
// every caller gets it, which is the lesson of the second HMAC and the fourth
// .ICN loader.
//
// WHY IT IS A SEPARATE OBJECT FILE, MEASURED, NOT STYLISTIC. escrow_contract_t
// is about 330 KB of BSS (ESCROW_MAX_FILES 512 snapshot slots plus
// ESCROW_MAX_MOVES 384 move slots, each holding two 256-byte paths). Every app
// links libc.a statically and every app ELF is a single RWX PT_LOAD whose .bss
// growth has broken the loader before (see blame.md, user.ld). If contract.c
// referenced the escrow directly, all 259 apps would carry that BSS the moment
// they gained a contract. Instead an app that actually promises something calls
// contract_use_escrow_promises() and pays for it; an app that promises nothing
// never names this symbol and the linker never pulls escrow.o in.
//
// HONEST LIMIT, stated here because contract.h states the same one about its own
// gate: CT_POST_ITEM is NOT verified by the escrow. The escrow oracle is a
// FILESYSTEM oracle (paths and bytes), and claiming it could check "this app's
// setting now reads 3" would be exactly the kind of overstatement that makes a
// control worthless. CT_POST_ITEM is checked HERE by re-reading the item through
// contract_get(), and the verdict line says which check answered for what.
#ifndef _MAYTERA_CTPROMISE_H
#define _MAYTERA_CTPROMISE_H

#include "contract.h"

// Register this file as contract.c's promise backend. Call it in main() BEFORE
// contract_cli(), in any app that has an item declaring ct_terms_t.promise.
// contract-lint fails the build on an app that declares a promise without this
// call, because contract.c FAILS CLOSED in that case: the action is refused
// rather than run with its postconditions unchecked.
void contract_use_escrow_promises(void);

// The two halves, exposed for the host test rig (libc/tests/ctpromise_test.c)
// and for a caller that drives an action outside contract_cli(). Normal apps do
// not call these: they call contract_use_escrow_promises() and let the engine.
int ct_promise_open(const ct_contract_t *c, const ct_item_t *it,
                    int argc, char **argv, char *why, int wcap);
int ct_promise_settle(const ct_contract_t *c, const ct_item_t *it,
                      char *how, int hcap);

// Resolve ONE path template ("home:SCREENSHOTS", "argv:0", "arg:path", "doc",
// "item:file.last_saved", or a literal absolute path) against this call.
// Exposed because the template vocabulary is shared with ct_cap_need_t.scope_tmpl
// and a second resolver is the drift this whole ticket is about. Returns 1 on
// success, 0 when the template cannot be resolved from this call.
int ct_promise_resolve(const ct_contract_t *c, const ct_item_t *it,
                       const char *tmpl, int argc, char **argv,
                       char *out, int ocap);

#endif // _MAYTERA_CTPROMISE_H
