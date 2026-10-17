// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// capreq.c - see capreq.h for why this is shared rather than per-app.

#include "capreq.h"
#include "syscall.h"
#include "string.h"
#include "stdio.h"

long cap_acquire(unsigned int cap, unsigned int scope_kind,
                 const char *scope, const char *reason)
{
    cap_req_t r;
    memset(&r, 0, sizeof(r));
    r.cap = cap;
    r.duration_ms = 120000;                 // 2 min window; unlimited uses within
    r.scope_kind = scope_kind;
    strlcpy(r.reason, reason ? reason : "", sizeof(r.reason));
    r.reason_len = (unsigned int)strlen(r.reason);
    strlcpy(r.scope, scope ? scope : "", sizeof(r.scope));

    long seq = sys_cap_request(&r);
    if (seq <= 0) return (seq < 0) ? seq : (long)CAP_EDENIED;

    long stt = CAP_ST_OPEN;
    for (int i = 0; i < 1800 && stt == (long)CAP_ST_OPEN; i++) {
        sys_sleep(100);
        stt = sys_cap_status((unsigned long long)seq);
    }
    return (stt == (long)CAP_ST_GRANTED) ? 0 : (long)CAP_EDENIED;
}

const char *cap_err_str(long e)
{
    switch (e) {
        case CAP_ENOINPUT:   return "no recent user input to authorize the request";
        case CAP_EBUSY:      return "a consent prompt is already open or the screen is locked";
        case CAP_ESCOPE:     return "the target window or scope was rejected";
        case CAP_ENOCONSENT: return "no compositor is available to ask the user";
        case CAP_EPOLICY:    return "refused by policy";
        case CAP_EDENIED:    return "the user denied the request";
        case CAP_EMAX:       return "the capability grant table is full";
        default:             return "the request was refused";
    }
}

int cap_is_denial(long e)
{
    return e == (long)CAP_EDENIED   || e == (long)CAP_ESCOPE ||
           e == (long)CAP_ENOINPUT  || e == (long)CAP_EBUSY  ||
           e == (long)CAP_ENOCONSENT|| e == (long)CAP_EPOLICY||
           e == (long)CAP_EMAX;
}

// The shotq refusal codes are kernel/gui/shotq.c's, deliberately distinct:
//   -3  CAP_EDENIED       no live grant covers this path
//   -11 queue full
//   -13 the REQUESTER cannot write there
//   -14 the COMPOSITOR, the process that actually writes it, cannot
//   -15 nothing owns the framebuffer, so nothing would ever write it
const char *cap_shot_err_str(long rc)
{
    switch (rc) {
        case -3:  return "no capability grant covers this path";
        case -11: return "the kernel screenshot queue is full; try again";
        case -13: return "this process has no write permission there";
        case -14: return "the compositor, which actually writes the file, has no write "
                         "permission there (it runs as the session user)";
        case -15: return "no compositor owns the framebuffer, so nothing would write it";
        default:  return "the screenshot request was refused";
    }
}

long cap_capture_to(const char *path, char *why, unsigned long whycap)
{
    if (!path || !path[0]) {
        if (why && whycap) strlcpy(why, "no output path", whycap);
        return (long)CAP_EARG;
    }

    long r = sys_screenshot_request(path);
    if (r == (long)CAP_EDENIED) {
        long g = cap_acquire(CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, path,
                             "capture the whole screen to an image file");
        if (g != 0) {
            if (why && whycap) strlcpy(why, cap_err_str(g), whycap);
            return g;
        }
        r = sys_screenshot_request(path);
    }
    if (r != 0 && why && whycap) strlcpy(why, cap_shot_err_str(r), whycap);
    return r;
}
