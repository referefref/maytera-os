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

#endif
