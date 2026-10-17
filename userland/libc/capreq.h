// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// capreq.h - #peruserrows: ONE implementation of "ask the user for a kernel
// capability grant and wait for the verdict".
//
// WHY THIS FILE EXISTS. Stage 1 of the capability API (docs/
// SYSTEM_CAPABILITY_API.md) DELETED the ambient /SCREENSHOT.REQ file drop
// (MAYTERA-SEC-2026-0035) and replaced it with SYS_SCREENSHOT_REQUEST, gated on a
// live screen.capture grant. Two writers were never converted and kept creating
// a file nothing reads: msh's `screenshot` builtin and Maytera Snap's capture.
// Converting them needs the request-and-consent dance, and that dance already
// existed TWICE as a private static: ai_cap_acquire() in libc/aiclient.c and
// flow_acquire_cap() in apps/flowrun/main.c. Adding a third and a fourth copy is
// exactly what CLAUDE.md's reuse rule forbids, so the common core lives here and
// aiclient.c now calls it.
//
// WHAT IS DELIBERATELY *NOT* HERE, stated so the next person does not assume the
// de-duplication is finished: flowrun's flow_acquire_cap() KEEPS its own body. It
// is not the same primitive. A headless workflow runner has had no user input, so
// it gets CAP_ENOINPUT, and it answers that by creating its OWN window, focusing
// it, waiting for a real keypress to credit the request and asking again. That
// choreography belongs to a process that owns a window. Its request-building and
// verdict-waiting halves do overlap with cap_acquire() and routing them through
// it is a follow-up, recorded here rather than left as a thing somebody notices.
#ifndef _CAPREQ_H
#define _CAPREQ_H

// Raise the TRUSTED COMPOSITOR's consent prompt for `cap` and wait, bounded, for
// the user's verdict. The compositor draws and resolves the prompt; an app can
// neither spoof it nor synthesise the Allow. Always issues a FRESH request,
// because a grant held for a different path or window does not cover this one.
//
// The wait is sys_sleep polling of sys_cap_status(), bounded at ~3 minutes. That
// is the documented-correct use of a timeout and not a masked wake source: the
// thing being waited on is a human answering a modal, which is outside this
// program's control (CLAUDE.md's preference order, case 2).
//
// Returns 0 on GRANTED, otherwise a negative CAP_E* code.
long cap_acquire(unsigned int cap, unsigned int scope_kind,
                 const char *scope, const char *reason);

// Human-readable reason for a negative CAP_E* code. Never NULL.
const char *cap_err_str(long e);

// True when `e` is a refusal by a PERSON or by POLICY, as opposed to a malformed
// request. The distinction matters because the first must not be retried.
int cap_is_denial(long e);

// The whole screen.capture sequence, which is the one every caller got wrong
// independently: try SYS_SCREENSHOT_REQUEST first (so a second capture inside an
// existing grant window does NOT re-prompt), and only on CAP_EDENIED raise the
// consent prompt and try once more. Returns 0 when the request is QUEUED.
//
// IT BEING QUEUED IS NOT THE FILE EXISTING. SYS_SCREENSHOT_REQUEST only enqueues;
// the compositor writes the BMP on a later frame, so the caller must still wait
// for a COMPLETE file (compare the BMP header's own byte count with the size, as
// flowrun and aiclient both do) before decoding it.
//
// `path` MUST be a path THIS session can already write: the kernel re-applies
// perms_check() to the requester AND the grant does not widen it, and then the
// COMPOSITOR, running as the session user, has to create the file. Use
// userscratch_path() or userhome_path(); the ext2 root is root-owned 0755 and
// yields -14, a permission error that otherwise surfaces as a timeout.
// On a non-zero return, `why` (when non-NULL) is filled with a one-line reason.
long cap_capture_to(const char *path, char *why, unsigned long whycap);

// Reason text for a non-zero cap_capture_to()/sys_screenshot_request() result,
// including the kernel/gui/shotq.c refusal codes, which are distinct ON PURPOSE
// so that "the screenshot never appeared" does not have to be guessed at.
const char *cap_shot_err_str(long rc);

#endif
