// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pkgdest.h - #745: confining a package destination to one directory subtree.
//
// A signed .mpkg carries an INSTALL manifest of "<member> -> <absolute dest>"
// lines. Until now the ONLY validation applied to <dest> anywhere in the tree
// was `if (dest[0] != '/') return 1;` in the App Store's unpack sink, whose own
// comment read "destinations are absolute by design". That was defensible while
// every install ran as root and the manifest signature was the sole control:
// the package could name any path, and naming a path it should not have was a
// question for the signer, not for the client.
//
// It stops being defensible the moment the CLIENT starts REWRITING destinations
// into a per-user prefix. A rewrite that can be steered by the string it is
// rewriting is not a sandbox, it is a suggestion. "/APPS/../../CONFIG/SHADOW"
// naively prefixed with "/HOME/ADMIN" yields "/HOME/ADMIN/APPS/../../CONFIG/
// SHADOW", which the filesystem resolves to "/CONFIG/SHADOW": straight back out
// of the profile and onto the password database. So the client must prove the
// rewrite landed inside the sandbox, and that proof is this file.
//
// THE MECHANISM AND THE PROOF ARE SEPARATE, DELIBERATELY.
//
//   MECHANISM  the destination is canonicalized FIRST, against "/", which can
//              never rise above the root because ".." at the root pops nothing.
//              The sandbox prefix is only joined on AFTERWARDS. Escape is
//              therefore impossible by construction, the same way chroot(2) is
//              not implemented by string inspection.
//   PROOF      the joined path is canonicalized a SECOND time and required to
//              start with the sandbox followed by '/'. On correct input this is
//              a no-op. It exists so that a future bug in the mechanism fails
//              CLOSED and loudly rather than silently widening the sandbox.
//
// It is a shared library function and not three lines inside the App Store
// because the elevated system-wide install path (#745 flow B, not implemented)
// needs the identical rule against a different root, and a second copy of a
// containment check is a second thing to get wrong.
//
// NO libc DEPENDENCY. Everything here is self-contained so the host-side
// table-driven test (tests/run_pkgdest.sh) compiles this exact translation
// unit, not a re-implementation of it.
#ifndef _PKGDEST_H
#define _PKGDEST_H

// Longest path this module will produce. Comfortably above the App Store's
// 160-byte manifest destination plus a home prefix; anything longer is REFUSED
// rather than truncated, because a truncated path is a different file.
#define PKGDEST_MAX 256

#define PKGDEST_OK         0
#define PKGDEST_E_ARG     (-1)   // null/too-small buffer
#define PKGDEST_E_NOTABS  (-2)   // destination is not absolute
#define PKGDEST_E_TOOLONG (-3)   // would not fit; refused, never truncated
#define PKGDEST_E_ESCAPE  (-4)   // resolves outside the sandbox
#define PKGDEST_E_BOOT    (-5)   // resolves onto the boot medium
#define PKGDEST_E_PREFIX  (-6)   // not one of the installable top-level dirs

// Canonicalize an ABSOLUTE path: collapse "//", drop ".", pop "..", strip a
// trailing '/'. Never rises above "/". Returns PKGDEST_OK or a negative code.
int pkgdest_canon(const char *in, char *out, unsigned long cap);

// Rewrite `dest` (an absolute path named by a package manifest) so that it
// lands under `sandbox`, and PROVE it did. `sandbox` is an absolute path; "/"
// means "no rewrite", which is what a root session gets because root's home
// directory is "/" (see userconf.c). Returns PKGDEST_OK with the confined path
// in `out`, or a negative code, in which case `out` is not usable.
//
// A destination that canonicalizes to the bare root ("/", "/..", "/.") names no
// file and is refused. So is anything landing on the boot medium (/BOOT, /EFI,
// /KERNEL.ELF), mirroring the kernel's pkg_path_is_boot() rule, which refuses
// those to every Ring-3 caller INCLUDING root.
//
// THE INSTALLABLE-PREFIX ALLOWLIST, and why confinement alone is not enough.
// If the ONLY rule were "the result must land under the sandbox", then a
// per-user install of a package naming "/CONFIG/SHADOW" would quietly write
// "<home>/CONFIG/SHADOW", and one naming "/CONFIG/STARTMENU/X.MENU" would
// inject a Start-menu entry pointing anywhere it liked. Both stay inside the
// user's own authority, so neither is a privilege escalation, but neither is a
// thing a package install should be able to do silently either. So the FIRST
// component of the canonical destination must be one of:
//
//     APPS   GAMES   THEMES            (compared case-insensitively)
//
// plus ONE scope-dependent exception: when `sandbox` is "/" (a root session, so
// no rewrite is happening) a single root-level "*.BMP" is also allowed, because
// that is where every wallpaper package in the shipping repository installs and
// where wp_enumerate() in libc/wallpapers.c looks. See pkgdest.c for the
// measurement. A per-user install refuses it rather than writing a wallpaper
// into a home directory that the one and only enumerator does not scan.
//
// Anything else is refused with PKGDEST_E_PREFIX and named on the status line.
// This is the same allowlist the elevated system-wide path (#745 flow B) will
// need kernel-side, which is the other reason it lives in a shared function.
int pkgdest_confine(const char *sandbox, const char *dest, char *out, unsigned long cap);

// Human-readable reason, for a status line that has to say WHY.
const char *pkgdest_strerror(int rc);

#endif
