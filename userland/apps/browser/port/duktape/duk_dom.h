/* duk_dom.h - Duktape <-> libdom binding for the MayteraOS browser.
 * Licensed under the MIT License.
 *
 * Runs the <script> elements of a parsed libdom document under Duktape with a
 * minimal but real DOM bound (document.getElementById, element textContent/
 * innerHTML/getAttribute/setAttribute, console.log). DOM mutations made by the
 * scripts are visible to the subsequent layout pass.
 */
#ifndef MAYTERA_DUK_DOM_H
#define MAYTERA_DUK_DOM_H

#include <dom/dom.h>

/* Execute every <script> in `doc` (document order) under a fresh JS context
 * with the DOM bound. console.log output is appended to logbuf (NUL-terminated,
 * capped at logcap) when non-NULL. Returns the number of scripts executed, or
 * -1 on setup failure. */
int js_run_document(dom_document *doc, char *logbuf, int logcap);

/* #245 layoutsweep: launch-time canary over the non-finite inputs of the
 * Duktape libm shim (duk_support.c). Declared here rather than in a new
 * duk_support.h because main.c already includes this header, and port/duktape is
 * compared file by file against the live engine tree by
 * build/browser-port-drift.sh, so one fewer file is one fewer thing to keep in
 * two places. Returns the number of failing cases, 0 when all pass, and reports
 * each case through the callback BEFORE running it, so a case that hangs is
 * named by the last line on serial. See the comment above its definition. */
int dsmath_selftest(void (*report)(const char *line));
int dsmath_selftest_count(void);

#endif
