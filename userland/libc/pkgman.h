// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pkgman.h - resolving a .mpkg archive member to its install destination(s),
// including TREE (directory prefix) rules.
//
// WHY THIS FILE EXISTS.
//
// A .mpkg is a tar.gz whose members are named "<pkgid>/<rel>", plus one member
// "<pkgid>/INSTALL" holding the install manifest. Until now every manifest line
// was an EXACT one-to-one mapping:
//
//     OPENARENA -> /APPS/OPENARENA
//     pak0.pk3  -> /GAMES/OPENARENA/baseoa/pak0.pk3
//
// and the resolver compared the member's relative name to the left-hand side
// with a single strcmp. That is correct and sufficient for a package that is a
// handful of named payload files, which is what every shipping package was.
//
// It does not scale to a package whose payload is a DIRECTORY TREE. AssaultCube
// ships roughly 1500 files under one data directory; Angband ships a lib/ tree;
// ClassiCube ships generated texpacks. Under a purely exact-match format those
// packages need one manifest line per file. Hand-listing 1500 lines is not a
// format, it is a workaround: the list is unreviewable, it goes stale the moment
// the upstream data set changes by one file, and a file present in the archive
// but missing from the list installs as silently nothing.
//
// THE PAYLOAD REPRESENTATION DID NOT NEED TO CHANGE, AND DELIBERATELY DID NOT.
//
// The obvious-looking move is to invent a tree payload format (a nested archive,
// a manifest-of-manifests, a blob plus an index). None of that is needed: a
// .mpkg is ALREADY a tar.gz, and a tar.gz already is a tree. The streaming
// extractor (arc_targz_extract_stream, #613) already walks arbitrarily deep
// member names in bounded memory (measured working set ~42KB, independent of
// archive size), already joins the ustar prefix field AND GNU 'L' long-name
// records so a deep tree's names survive, and already validates every name. The
// ONLY thing that could not express a tree was the manifest's mapping rule. So
// the change is confined to the mapping rule, and every other property of the
// format, notably the trust chain, is inherited unchanged. That matters: see
// below.
//
// THE RULE GRAMMAR.
//
//   EXACT   <rel>  -> <absolute dest>         neither side ends in '/'
//   TREE    <rel>/ -> <absolute dest>/        BOTH sides end in '/'
//
// A TREE rule maps every member under "<pkgid>/<rel>/" to "<dest>/" plus the
// remainder of the member's path, preserving the subtree shape. So
//
//     packages/ -> /GAMES/ASSAULTCUBE/packages/
//
// maps "assaultcube/packages/models/weapon/rifle/skin.jpg" to
// "/GAMES/ASSAULTCUBE/packages/models/weapon/rifle/skin.jpg", and it does so
// for the whole tree with one line, whatever the upstream data set contains.
//
// EXACT rules WIN over TREE rules, and among TREE rules the LONGEST matching
// prefix wins. That is the ordinary routing-table rule and it is the least
// surprising one: it lets a package ship a tree and redirect or re-point one
// file inside it without having to carve the tree into pieces.
//
// A rule with a trailing '/' on exactly ONE side is REFUSED, not guessed at.
// "packages/ -> /GAMES/AC/packages" is far more likely to be a typo than an
// intention, and the two readings of it (append or do not append a separator)
// differ by a path component.
//
// A bare-root tree rule ("/ -> /GAMES/X/", or any rule whose source prefix is
// empty) is REFUSED. It would map EVERY member of the archive including the
// INSTALL manifest itself, so a package would install a copy of its own
// manifest into the destination tree. Every real package has its payload under
// at least one named subdirectory, so nothing legitimate needs it.
//
// FAN-OUT: ONE MEMBER, SEVERAL DESTINATIONS, AND THE BUG THAT FOUND IT.
//
// Every shipping app package in the live repository already names the same
// payload file twice, e.g. aichat-1.0.0's INSTALL:
//
//     files/aichat -> /APPS/AICHAT
//     files/aichat -> /APPS/aichat
//
// (mediaplayer names three). The old resolver returned on the FIRST matching
// line, so the second and third destinations were NEVER WRITTEN, silently: the
// manifest said one thing and the installer did another. That is not cosmetic.
// The App Store's launch-after-install step (appstore/main.c unp_end) only
// arms itself on an all-lowercase basename under the applications directory,
// precisely the alias that was being dropped, so "Install and run" could not
// fire for any of those packages. Measured by extracting the INSTALL member of
// all 24 .mpkg files published on the repo host: 14 of them name at least one
// destination that the installer never created (11 name two, 3 name three).
//
// So resolution is a SET, not a single answer: pkgman_dest_all() returns every
// destination a member maps to, in manifest order, and the caller writes all of
// them. More matches than the caller's array can hold is PKGMAN_E_TOOMANY and
// fails the install; it is never silently truncated, because a truncated
// fan-out is exactly the bug above with a bigger limit.
//
// WHAT THIS FILE IS NOT RESPONSIBLE FOR, so that the layering is explicit.
//
//   THE TRUST ANCHOR is unchanged and is not here. The chain is: the signing
//   key signs manifest.json; manifest.json carries a sha256 PER PACKAGE; the
//   client hashes the downloaded bytes AND re-hashes the on-disk copy and
//   refuses both unless they equal that sha256, BEFORE a single member is
//   extracted (appstore/main.c acquire_verified_package). The digest is over
//   the WHOLE ARCHIVE, so the bytes of every file in a tree payload are covered
//   by the signature exactly as tightly as a single-file payload's were. There
//   is no per-member digest to add and adding one would not strengthen
//   anything: a member cannot be substituted without changing the archive.
//
//   THE SANDBOX is pkgdest_confine() in pkgdest.h, applied by the caller to
//   whatever this function returns. This function does not canonicalize, does
//   not know about the per-user home prefix, and does not enforce the
//   installable-prefix allowlist.
//
//   THE ARCHIVE-LEVEL NAME CHECK is arc_path_is_safe() in libarchive/arc.h.
//
// AND YET THIS FILE VALIDATES THE MEMBER NAME ANYWAY, on purpose. Under exact
// matching the member name was compared to a fixed string, so a hostile name
// simply failed to match and was skipped: the name never reached the
// destination. A TREE rule CONCATENATES the member's own tail onto a
// destination prefix, which makes the member name an input to the path for the
// first time. A guard that is one call site away in a different translation
// unit is not the place to rely on for that. So this module rejects an absolute
// name, any ".." component, control bytes, a backslash and a colon itself,
// which also makes the host-side hostile-input battery meaningful without
// having to link the whole archiver.
//
// SELF-CONTAINED, no libc and no syscalls, so tests/run_pkgman.sh compiles THIS
// translation unit with the shipping freestanding flags rather than a host
// flavoured copy of it. Same reason pkgdest.c is written that way.
#ifndef _PKGMAN_H
#define _PKGMAN_H

// Longest destination this module will produce. Deliberately equal to
// PKGDEST_MAX, because the destination goes straight into pkgdest_confine().
#define PKGMAN_DEST_MAX 256

// Longest left-hand side a manifest rule may name. A longer one is REFUSED
// rather than truncated: truncating the source of a TREE rule would WIDEN the
// set of members it matches, which is a containment failure, not a cosmetic one.
#define PKGMAN_SRC_MAX  192

// Longest manifest line considered. A longer one is refused for the same
// reason.
#define PKGMAN_LINE_MAX 384

// How many destinations one member may fan out to. Four covers every shipping
// manifest (the widest is mediaplayer's three) with room to spare. Exceeding it
// is an error, never a silent drop.
#define PKGMAN_MAX_DESTS 4

// ---------------------------------------------------------------------------
// FORMAT GENERATION, AND THE NEGOTIATION THAT DID NOT EXIST.
//
// When TREE rules were added, the format had no way for a package to say which
// client it needs, and no way for a client to say it does not understand one.
// The consequence is specific and was measured, not guessed: hand a TREE rule
// to a client that matches members with a single strcmp and EVERY member of the
// tree is unreferenced, so the install writes nothing and reports "Nothing
// installed". A total failure that reads as a broken package rather than as a
// client too old to understand it.
//
// The answer is one optional directive line in the INSTALL manifest:
//
//     #!mpkg-min-client 2
//
// It begins with '#', so every parser that ever shipped reads it as a comment
// and skips it. A client that knows the directive reads the number and refuses
// the package with a reason a human can act on.
//
// BE HONEST ABOUT WHAT THIS CAN AND CANNOT DO. It CANNOT help the clients that
// already shipped: they will keep silently installing nothing from a tree
// package, because you cannot retrofit a parser that is already in the field.
// That is precisely why it is added NOW, at the first bump, instead of at the
// next one: generation 3 will be refused politely by every generation-2 client,
// and generation 2 is the last bump that has to be handled by convention.
//
// THE CONVENTION FOR GENERATION 2, until no generation-1 client matters: a
// package whose manifest uses TREE rules MUST declare `#!mpkg-min-client 2`.
// tools/mpkg/build-mpkg.sh refuses to emit one that does not, so the
// declaration is not a thing to remember.
//
//   generation 1  EXACT rules only. Every client before userland/libc/pkgman.c.
//   generation 2  + TREE rules, + fan-out to several destinations per member.
#define PKGMAN_CLIENT_GEN 2


#define PKGMAN_UNREFERENCED  0   // no rule covers this member; skip it
#define PKGMAN_MAPPED        1   // one destination (pkgman_dest_for)
#define PKGMAN_E_ARG       (-1)  // null argument or a destination buffer too small
#define PKGMAN_E_MEMBER    (-2)  // the member name is not usable as a path tail
#define PKGMAN_E_RULE      (-3)  // a malformed or over-long manifest rule
#define PKGMAN_E_TOOLONG   (-4)  // the mapped destination would not fit
#define PKGMAN_E_TOOMANY   (-5)  // more destinations than the caller can hold
#define PKGMAN_E_NEWER    (-6)  // the manifest needs a newer client than this one

// Resolve one archive member against the INSTALL manifest text, returning EVERY
// destination it maps to.
//
// man/mlen  the INSTALL manifest, as lifted out of the archive. Not NUL
//           terminated by contract; mlen is authoritative.
// pkgid     the package id. A member not under "<pkgid>/" is UNREFERENCED.
// member    the archive member name, relative, as the extractor reports it.
// dests     receives up to `max_dests` absolute destinations, in manifest order.
//
// Returns the NUMBER of destinations (>= 1), PKGMAN_UNREFERENCED (0) when no
// rule covers the member, or a negative PKGMAN_E_* code.
//
// FAIL CLOSED: every negative return must abort the whole install. A member
// that matched a rule but could not be expressed as a path is NOT a member to
// skip, because a package that half-installed is not a package.
int pkgman_dest_all(const char *man, unsigned long mlen,
                    const char *pkgid, const char *member,
                    char dests[][PKGMAN_DEST_MAX], int max_dests);

// Single-destination convenience wrapper: the FIRST destination only. Returns
// PKGMAN_MAPPED, PKGMAN_UNREFERENCED, or a negative code. Callers that install
// files must use pkgman_dest_all(); this exists for probes and for tests.
int pkgman_dest_for(const char *man, unsigned long mlen,
                    const char *pkgid, const char *member,
                    char *dest, unsigned long cap);

// Validate a whole manifest up front, independently of any member, so a
// malformed rule is reported once with its line number instead of surfacing as
// a mystery mid-extraction. Returns 0 when every rule is well formed, or
// PKGMAN_E_RULE. On PKGMAN_E_RULE, *out_line receives the 1-based line number
// (0 if out_line is null).
int pkgman_check_manifest(const char *man, unsigned long mlen, int *out_line);

// The minimum client generation this manifest declares, from an optional
// "#!mpkg-min-client <n>" directive. Returns 1 when the manifest declares none,
// which is every manifest written before the directive existed and is the only
// answer that keeps those packages installable. A malformed or out-of-range
// declaration is PKGMAN_E_RULE: it is refused rather than read as "1", because
// treating an unreadable requirement as "no requirement" is the failure this
// directive exists to prevent.
//
// A caller that gets back more than PKGMAN_CLIENT_GEN must REFUSE the package
// and say so; it must not attempt the install and report what it managed.
int pkgman_min_client(const char *man, unsigned long mlen);

// Human-readable reason, for a status line that has to say WHY.
const char *pkgman_strerror(int rc);

#endif
