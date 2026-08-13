// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pkgdest.c - #745: destination confinement for package installs.
// See pkgdest.h for the design and for why the mechanism and the proof are
// deliberately two separate steps.
//
// Self-contained on purpose: no string.h, no syscalls, no allocation. The host
// test (tests/run_pkgdest.sh) compiles THIS file and links it next to glibc, so
// what the table-driven hostile-input battery exercises is the shipping
// translation unit rather than a copy of it.
#include "pkgdest.h"

static unsigned long pd_len(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

// Case-insensitive prefix match that also requires a COMPONENT BOUNDARY, so
// "/BOOTLOG.TXT" does not match "/BOOT". This is a deliberate mirror of
// pkg_pfx_ci() in kernel/proc/syscall.c; the kernel copy is static and cannot
// be linked from Ring 3. If that rule changes, this must change with it.
static int pd_pfx_ci(const char *p, const char *pfx) {
    unsigned i = 0;
    for (; pfx[i]; i++) {
        char a = p[i], b = pfx[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return (p[i] == 0 || p[i] == '/');
}

// The kernel refuses SYS_PKG_WRITE to these for EVERY Ring-3 caller including
// root (pkg_path_is_boot(), kernel/proc/syscall.c). The App Store's member
// writes go through sys_open, which that rule does not cover, so a signed
// package naming /EFI/BOOT/BOOTX64.EFI would today be written by a root
// install. Refusing it here closes that specific hole for the install path.
static int pd_is_boot(const char *p) {
    if (pd_pfx_ci(p, "/BOOT")) return 1;
    if (pd_pfx_ci(p, "/EFI")) return 1;
    if (pd_pfx_ci(p, "/KERNEL.ELF")) return 1;
    return 0;
}

// The top-level directories a package may install into. See pkgdest.h for why
// confinement to the sandbox alone is not a sufficient rule.
static const char *const PD_ALLOW[] = { "APPS", "GAMES", "THEMES", 0 };

// Is the FIRST component of the canonical path `c` (which always begins with
// '/') one of the installable directories, with something after it?
static int pd_prefix_allowed(const char *c) {
    for (int i = 0; PD_ALLOW[i]; i++) {
        unsigned k = 0;
        const char *w = PD_ALLOW[i];
        for (; w[k]; k++) {
            char a = c[1 + k];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (a != w[k]) break;
        }
        // Matched the whole word AND stopped at a component boundary, so
        // "/APPSTORE" does not pass as "/APPS", and a bare "/APPS" (which names
        // a directory, not a file) does not pass either.
        if (!w[k] && c[1 + k] == '/') return 1;
    }
    return 0;
}

// THE WALLPAPER EXCEPTION, and it is measured, not assumed.
//
// Every wallpaper package in the shipping repository installs a single BMP at
// the FILESYSTEM ROOT: wp-maytera writes "/MAYTERA.BMP", wp-cyber writes
// "/CYBER.BMP", theme-darkmode writes "/DARKMODE.BMP". That is not a quirk of
// those packages, it is the OS convention: wp_enumerate() in
// userland/libc/wallpapers.c scans "/" for "*.BMP" and nothing else, and the
// golden's ext2 root holds ~40 of them. There is no /WALLPAPER directory.
//
// So the allowlist has to permit a root-level BMP or it BREAKS root wallpaper
// installs, which would be a regression introduced by a security control. It
// is permitted ONLY when no rewrite is happening (sandbox "/", i.e. a root
// session), because relocating a wallpaper into a home directory would write a
// file that the one and only enumerator cannot see. Installing something
// nothing can find is worse than refusing, so the per-user scope refuses it
// and says why.
static int pd_is_root_bmp(const char *c) {
    // Exactly one component, ending ".BMP" (case-insensitive).
    unsigned k = 1;
    while (c[k] && c[k] != '/') k++;
    if (c[k] == '/') return 0;                 // more than one component
    if (k < 1 + 5) return 0;                   // shorter than "x.BMP"
    const char *e = c + k - 4;
    char a = e[1], b = e[2], d = e[3];
    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    if (d >= 'a' && d <= 'z') d = (char)(d - 32);
    return e[0] == '.' && a == 'B' && b == 'M' && d == 'P';
}

int pkgdest_canon(const char *in, char *out, unsigned long cap) {
    if (!in || !out || cap < 2) return PKGDEST_E_ARG;
    if (in[0] != '/') return PKGDEST_E_NOTABS;

    unsigned long w = 0;
    out[w++] = '/';                 // the result is rooted, always

    unsigned long i = 0;
    while (in[i]) {
        while (in[i] == '/') i++;   // collapse runs of separators
        if (!in[i]) break;

        unsigned long cs = i;
        while (in[i] && in[i] != '/') i++;
        unsigned long clen = i - cs;

        if (clen == 1 && in[cs] == '.') continue;              // drop "."

        if (clen == 2 && in[cs] == '.' && in[cs + 1] == '.') {
            // Pop one component. At the root there is nothing to pop and the
            // path STAYS at the root: this is what makes the canonical form
            // incapable of naming anything above "/", and it is the property
            // the join in pkgdest_confine() relies on.
            while (w > 1 && out[w - 1] != '/') w--;
            if (w > 1) w--;                                     // eat the '/'
            if (w == 0) w = 1;
            continue;
        }

        if (w > 1) {
            if (w + 1 >= cap) return PKGDEST_E_TOOLONG;
            out[w++] = '/';
        }
        if (w + clen >= cap) return PKGDEST_E_TOOLONG;
        for (unsigned long k = 0; k < clen; k++) out[w++] = in[cs + k];
    }

    out[w] = '\0';
    return PKGDEST_OK;
}

int pkgdest_confine(const char *sandbox, const char *dest, char *out,
                    unsigned long cap) {
    if (!sandbox || !dest || !out || cap < 2) return PKGDEST_E_ARG;

    char sb[PKGDEST_MAX];
    int rc = pkgdest_canon(sandbox, sb, sizeof(sb));
    if (rc != PKGDEST_OK) return rc;

    // STEP 1, THE MECHANISM. Canonicalize the manifest-supplied destination
    // against the ROOT, before the sandbox prefix exists. ".." can pop
    // components of the destination itself but can never rise above "/", so
    // whatever comes out of here is a path that is safe to graft onto anything.
    char rel[PKGDEST_MAX];
    rc = pkgdest_canon(dest, rel, sizeof(rel));
    if (rc != PKGDEST_OK) return rc;

    // "/" names a directory, not a file. A manifest line that resolves to it
    // (e.g. "-> /", "-> /..", "-> /APPS/..") is malformed, and silently
    // ignoring it would let a package hide a member from an audit of the
    // manifest. Refuse.
    if (rel[1] == '\0') return PKGDEST_E_ESCAPE;

    // Root's home directory is "/" (see userconf.c), so a root session lands
    // here with an empty prefix and the confined path is byte-identical to the
    // manifest's own destination. That is what keeps a root install unchanged.
    unsigned long sl = pd_len(sb);
    if (sl == 1) sl = 0;

    // The boot medium is tested BEFORE the allowlist purely so the refusal
    // names the real reason: the allowlist would reject /BOOT and /EFI anyway,
    // but "destination is on the boot medium" is what an operator needs to
    // read. Tested on the CANONICAL form, so "/APPS/../BOOT/x" is caught.
    if (pd_is_boot(rel)) return PKGDEST_E_BOOT;

    // Applied to the CANONICAL form, never to the raw string, so
    // "/APPS/../CONFIG/SHADOW" is judged as the "/CONFIG/SHADOW" it actually
    // resolves to rather than as the "/APPS/..." it is spelled as. Checking a
    // prefix before resolving the path is the classic way to write a check
    // that does nothing.
    if (!pd_prefix_allowed(rel) && !(sl == 0 && pd_is_root_bmp(rel)))
        return PKGDEST_E_PREFIX;

    char j[PKGDEST_MAX];
    if (sl + pd_len(rel) + 1 > sizeof(j)) return PKGDEST_E_TOOLONG;
    unsigned long w = 0;
    for (unsigned long k = 0; k < sl; k++) j[w++] = sb[k];
    for (unsigned long k = 0; rel[k]; k++) j[w++] = rel[k];
    j[w] = '\0';

    rc = pkgdest_canon(j, out, cap);
    if (rc != PKGDEST_OK) return rc;

    // STEP 2, THE PROOF. On correct input this cannot fail, because step 1
    // made escape impossible. It is here so that if step 1 is ever broken by a
    // later edit, the result is a REFUSED install rather than a silently
    // widened sandbox. A check that only ever passes is still the check that
    // tells you when the thing it guards has changed.
    if (sl) {
        unsigned long ol = pd_len(out);
        if (ol <= sl) return PKGDEST_E_ESCAPE;
        for (unsigned long k = 0; k < sl; k++)
            if (out[k] != sb[k]) return PKGDEST_E_ESCAPE;
        if (out[sl] != '/') return PKGDEST_E_ESCAPE;
    }

    if (pd_is_boot(out)) return PKGDEST_E_BOOT;
    return PKGDEST_OK;
}

const char *pkgdest_strerror(int rc) {
    switch (rc) {
        case PKGDEST_OK:         return "ok";
        case PKGDEST_E_ARG:      return "bad argument";
        case PKGDEST_E_NOTABS:   return "destination not absolute";
        case PKGDEST_E_TOOLONG:  return "destination too long";
        case PKGDEST_E_ESCAPE:   return "destination escapes the profile";
        case PKGDEST_E_BOOT:     return "destination is on the boot medium";
        case PKGDEST_E_PREFIX:   return "destination is not an installable directory";
        default:                 return "unknown error";
    }
}
