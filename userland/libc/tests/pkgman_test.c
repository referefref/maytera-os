// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pkgman_test.c - battery for pkgman.c, the .mpkg INSTALL manifest resolver
// that gained TREE (directory prefix) rules so a package can carry a directory
// tree instead of one hand-listed line per file.
//
// FOUR ARMS, all required. The first three follow the convention run_pkgdest.sh
// set in this directory; the fourth is specific to this change.
//
//   1 NEGATIVE CONTROL (-DPKGMAN_NAIVE) replaces tree resolution with the
//     obvious wrong implementation, a character-wise prefix match and a
//     concatenation with no member-name validation, and REQUIRES the hostile
//     vectors to ESCAPE the destination tree. Escape is judged by an
//     independent path resolver in this file, never by asking pkgman.c whether
//     pkgman.c was right. A vector that does not escape the naive version is
//     not hostile and proves nothing, so that is a failure too.
//
//   2 POSITIVE runs the real pkgman_dest_all() and requires every hostile
//     vector REFUSED and every benign vector mapped to exactly the expected
//     path, fan-out included.
//
//   3 CHAINED runs the real client order, pkgman_dest_all() then
//     pkgdest_confine(), and requires the surviving path to be inside the
//     sandbox. This is the arm that matters operationally: the installer never
//     calls either function alone.
//
//   4 CORPUS replays the INSTALL manifest and full member list of EVERY package
//     published on the live repo host (fixtures/mpkg-install-corpus.txt, 24
//     packages) and requires, for each member, that pkgman's FIRST destination
//     is byte-identical to what the OLD exact-match resolver produced. The old
//     resolver is embedded here verbatim as the ORACLE. A format change that
//     silently re-points an already-published package is the one failure mode
//     no unit vector would catch.
//
// pkgman.c and pkgdest.c are compiled freestanding with the real libc flags;
// only this harness uses the host libc.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../pkgman.h"
#include "../pkgdest.h"

static int fails;

// ---------------------------------------------------------------------------
// THE ORACLE for arm 1: resolve a path the way a filesystem would and say
// whether it is still under `root`. Independent of everything under test.
// ---------------------------------------------------------------------------
static int really_under(const char *p, const char *root) {
    if (!p || p[0] != '/') return 0;
    char comp[64][128];
    int n = 0;
    const char *q = p + 1;
    while (*q) {
        char c[128];
        int l = 0;
        while (*q && *q != '/' && l < 127) c[l++] = *q++;
        c[l] = 0;
        while (*q == '/') q++;
        if (l == 0 || strcmp(c, ".") == 0) continue;
        if (strcmp(c, "..") == 0) { if (n > 0) n--; continue; }
        if (n < 64) { strncpy(comp[n], c, 127); comp[n][127] = 0; n++; }
    }
    char flat[64 * 130];
    int w = 0;
    for (int i = 0; i < n; i++)
        w += snprintf(flat + w, sizeof(flat) - (size_t)w, "/%s", comp[i]);
    flat[w] = 0;
    size_t rl = strlen(root);
    return strncmp(flat, root, rl) == 0 && (flat[rl] == '/' || flat[rl] == 0);
}

// ---------------------------------------------------------------------------
// Arm 1's NAIVE resolver: tree rules done the way a first attempt does them.
// Character-wise prefix compare (so no component boundary), straight
// concatenation, and no member-name validation at all. Deliberately only
// understands the ONE tree rule the vectors use, because its job is to be
// wrong, not to be a second implementation.
// ---------------------------------------------------------------------------
#ifdef PKGMAN_NAIVE
static int naive_tree(const char *src, const char *dst, const char *rel,
                      char *out, size_t cap) {
    size_t sl = strlen(src);
    if (strncmp(rel, src, sl) != 0) return 0;
    const char *tail = rel + sl;
    while (*tail == '/') tail++;
    if (strlen(dst) + 1 + strlen(tail) + 1 > cap) return 0;
    strcpy(out, dst);
    strcat(out, "/");
    strcat(out, tail);
    return 1;
}
#endif

// ===========================================================================
// Vectors. One table, used by arms 1 to 3.
//
// kind 0 = HOSTILE: the real resolver must refuse it.
// kind 1 = BENIGN: must map to exactly `want`.
// kind 2 = UNREFERENCED: no rule covers it, must be skipped (0), not an error.
//
// A hostile vector also carries HOW it is hostile, and that field is set
// EXPLICITLY per vector, never derived from the string. Two different wrongs
// need two different demonstrations in the negative arm:
//
//   H_ESCAPE  the naive resolver must produce a path that RESOLVES OUTSIDE the
//             destination tree. This is the traversal class.
//   H_CLASS   the naive resolver must ACCEPT it and produce a destination, even
//             though the path it produces stays inside the tree. A backslash, a
//             colon or a control byte does not escape a POSIX path; it creates a
//             file whose name the package had no business choosing. Demanding an
//             escape here would be demanding the wrong proof, and a test that
//             demands the wrong proof gets "fixed" by weakening it.
#define H_ESCAPE 1
#define H_CLASS  2
// ===========================================================================
#define PKGID "assaultcube"
#define TREE_SRC "packages"
#define TREE_DST "/GAMES/ASSAULTCUBE/packages"

// The manifest the vectors are resolved against. One tree rule, one nested
// tree rule (to exercise longest-prefix), two exact rules, and one exact rule
// that deliberately re-points a single file that also falls inside the tree.
static const char MAN[] =
    "# AssaultCube install manifest\n"
    "ASSAULTCUBE -> /APPS/ASSAULTCUBE\n"
    "packages/ -> /GAMES/ASSAULTCUBE/packages/\n"
    "packages/audio/ -> /GAMES/ASSAULTCUBE/sounds/\n"
    "config/ -> /GAMES/ASSAULTCUBE/profile/config/\n"
    "packages/models/pinned.cfg -> /GAMES/ASSAULTCUBE/pinned.cfg\n";

typedef struct { int kind; int hclass; const char *member; const char *want; } vec;

static const vec V[] = {
  // --- benign tree mapping, including a deep path ---
  { 1, 0, PKGID "/packages/models/weapon/rifle/skin.jpg",
       "/GAMES/ASSAULTCUBE/packages/models/weapon/rifle/skin.jpg" },
  { 1, 0, PKGID "/packages/textures/wall.jpg",
       "/GAMES/ASSAULTCUBE/packages/textures/wall.jpg" },
  // --- longest matching prefix wins over the shorter tree rule ---
  { 1, 0, PKGID "/packages/audio/ambience/wind.ogg",
       "/GAMES/ASSAULTCUBE/sounds/ambience/wind.ogg" },
  // --- an exact rule beats any tree rule that also covers the member ---
  { 1, 0, PKGID "/packages/models/pinned.cfg", "/GAMES/ASSAULTCUBE/pinned.cfg" },
  // --- exact rule, the pre-existing form ---
  { 1, 0, PKGID "/ASSAULTCUBE", "/APPS/ASSAULTCUBE" },
  { 1, 0, PKGID "/config/menus.cfg", "/GAMES/ASSAULTCUBE/profile/config/menus.cfg" },

  // --- HOSTILE: traversal out of the tree via the member's own name ---
  { 0, H_ESCAPE, PKGID "/packages/../../../CONFIG/SHADOW", 0 },
  { 0, H_ESCAPE, PKGID "/packages/a/../../../../CONFIG/SHADOW", 0 },
  { 0, H_ESCAPE, PKGID "/packages/..", 0 },
  { 0, H_ESCAPE, PKGID "/packages/audio/../../../../KERNEL.ELF", 0 },
  // --- HOSTILE: alternate separators and volume prefixes. These do NOT escape
  //     a POSIX path, so the demonstration is that the naive version accepts
  //     them at all (see H_CLASS above). ---
  { 0, H_CLASS, PKGID "/packages/..\\..\\CONFIG\\SHADOW", 0 },
  { 0, H_CLASS, PKGID "/packages/C:/CONFIG/SHADOW", 0 },
  // --- HOSTILE: a control byte in a path that is otherwise fine ---
  { 0, H_CLASS, PKGID "/packages/wall\x01.jpg", 0 },

  // --- UNREFERENCED: component-boundary discipline. "packagesX" is NOT
  //     under the "packages/" tree, and a character-wise prefix match is
  //     exactly the bug that would put it there. ---
  { 2, 0, PKGID "/packagesEVIL/x.cfg", 0 },
  { 2, 0, PKGID "/INSTALL", 0 },
  { 2, 0, PKGID "/PKGINFO", 0 },
  { 2, 0, "otherpkg/packages/x.jpg", 0 },
  // --- UNREFERENCED: the bare directory member of a tree rule names no file ---
  { 2, 0, PKGID "/packages", 0 },
};
#define NV ((int)(sizeof(V) / sizeof(V[0])))

// ===========================================================================
// Arm 1: NEGATIVE CONTROL
// ===========================================================================
#ifdef PKGMAN_NAIVE
static int arm_naive(void) {
    int bad = 0, shown = 0, escaped = 0;
    (void)MAN;   // arm 1 resolves with naive_tree(), not with the manifest
    printf("--- arm 1/4: NEGATIVE CONTROL (naive prefix+concat must be wrong) ---\n");
    for (int i = 0; i < NV; i++) {
        if (V[i].kind != 0) continue;
        const char *rel = strchr(V[i].member, '/');
        if (!rel) continue;
        rel++;
        char out[512];
        if (!naive_tree(TREE_SRC, TREE_DST, rel, out, sizeof(out))) {
            printf("  NOT DEMONSTRATED: naive already rejects %s\n", V[i].member);
            bad++;
            continue;
        }
        shown++;
        if (V[i].hclass == H_ESCAPE) {
            if (really_under(out, TREE_DST)) {
                printf("  NOT HOSTILE: %s -> %s stays inside %s\n",
                       V[i].member, out, TREE_DST);
                bad++;
            } else {
                escaped++;
                printf("  escapes (as required): %s -> %s\n", V[i].member, out);
            }
        } else {
            // H_CLASS: accepted by the naive version is the whole point. It
            // must still be REFUSED by the real one, which arm 2 asserts.
            printf("  accepted by naive (as required): %s -> %s\n", V[i].member, out);
        }
    }
    // The component-boundary vector must ALSO be shown to be mishandled by the
    // naive version, otherwise the boundary check in pkgman.c is unmotivated.
    {
        char out[512];
        if (naive_tree(TREE_SRC, TREE_DST, "packagesEVIL/x.cfg", out, sizeof(out))) {
            printf("  naive maps packagesEVIL/x.cfg -> %s (must not happen in the real one)\n", out);
            shown++;
        } else {
            printf("  NOT HOSTILE: naive already rejects packagesEVIL/x.cfg\n");
            bad++;
        }
    }
    if (escaped < 4) { printf("  too few vectors actually ESCAPED (%d)\n", escaped); bad++; }
    if (shown < 7)   { printf("  too few vectors demonstrated at all (%d)\n", shown); bad++; }
    printf("  naive arm: %d demonstrated (%d of them real escapes), %d problems\n",
           shown, escaped, bad);
    return bad;
}
#endif

#ifndef PKGMAN_NAIVE
// ===========================================================================
// Arm 2: POSITIVE
// ===========================================================================
static int arm_positive(void) {
    int bad = 0;
    printf("--- arm 2/4: POSITIVE (pkgman_dest_all) ---\n");
    // The manifest itself must be well formed.
    int line = -1;
    int mrc = pkgman_check_manifest(MAN, sizeof(MAN) - 1, &line);
    if (mrc != 0) { printf("  FAIL: the test manifest is rejected (line %d)\n", line); bad++; }

    for (int i = 0; i < NV; i++) {
        char d[PKGMAN_MAX_DESTS][PKGMAN_DEST_MAX];
        int rc = pkgman_dest_all(MAN, sizeof(MAN) - 1, PKGID, V[i].member,
                                 d, PKGMAN_MAX_DESTS);
        if (V[i].kind == 0) {
            if (rc >= 0) {
                printf("  FAIL hostile ACCEPTED: %s -> rc=%d %s\n",
                       V[i].member, rc, rc > 0 ? d[0] : "(none)");
                bad++;
            } else {
                printf("  ok refused (%s): %s\n", pkgman_strerror(rc), V[i].member);
            }
        } else if (V[i].kind == 1) {
            if (rc != 1) {
                printf("  FAIL benign: %s rc=%d (want exactly 1 destination)\n",
                       V[i].member, rc);
                bad++;
            } else if (strcmp(d[0], V[i].want) != 0) {
                printf("  FAIL benign: %s -> %s, want %s\n", V[i].member, d[0], V[i].want);
                bad++;
            } else {
                printf("  ok %s -> %s\n", V[i].member, d[0]);
            }
        } else {
            if (rc != PKGMAN_UNREFERENCED) {
                printf("  FAIL unreferenced: %s rc=%d %s\n",
                       V[i].member, rc, rc > 0 ? d[0] : "");
                bad++;
            } else {
                printf("  ok skipped: %s\n", V[i].member);
            }
        }
    }

    // --- fan-out: the same member named by several exact rules -------------
    {
        static const char FAN[] =
            "files/aichat -> /APPS/AICHAT\n"
            "files/aichat -> /APPS/aichat\n";
        char d[PKGMAN_MAX_DESTS][PKGMAN_DEST_MAX];
        int rc = pkgman_dest_all(FAN, sizeof(FAN) - 1, "aichat", "aichat/files/aichat",
                                 d, PKGMAN_MAX_DESTS);
        if (rc != 2 || strcmp(d[0], "/APPS/AICHAT") != 0 || strcmp(d[1], "/APPS/aichat") != 0) {
            printf("  FAIL fan-out: rc=%d [%s] [%s]\n", rc, rc > 0 ? d[0] : "", rc > 1 ? d[1] : "");
            bad++;
        } else {
            printf("  ok fan-out: 2 destinations, both named\n");
        }
        // A fan-out wider than the caller's array must FAIL, never truncate.
        char one[1][PKGMAN_DEST_MAX];
        int rc1 = pkgman_dest_all(FAN, sizeof(FAN) - 1, "aichat", "aichat/files/aichat", one, 1);
        if (rc1 != PKGMAN_E_TOOMANY) {
            printf("  FAIL over-wide fan-out was not refused: rc=%d\n", rc1);
            bad++;
        } else {
            printf("  ok over-wide fan-out refused rather than truncated\n");
        }
    }

    // --- malformed rules must be named, not guessed at ---------------------
    {
        struct { const char *man; const char *why; } bads[] = {
            { "packages/ -> /GAMES/AC/packages\n",  "trailing slash on one side only" },
            { "packages -> /GAMES/AC/packages/\n",  "trailing slash on one side only" },
            { "/ -> /GAMES/AC/\n",                  "bare-root tree rule" },
            { "packages/ -> packages/\n",           "destination not absolute" },
            { "../etc/ -> /GAMES/AC/x/\n",          "traversal in the rule source" },
            { "x -> /GAMES/../../CONFIG/SHADOW\n",  "traversal in the rule destination" },
            { "x -> /APPS/X junk\n",                "trailing junk" },
            { "x ->\n",                             "no destination" },
        };
        for (unsigned k = 0; k < sizeof(bads) / sizeof(bads[0]); k++) {
            int ln = 0;
            int rc = pkgman_check_manifest(bads[k].man, strlen(bads[k].man), &ln);
            if (rc != PKGMAN_E_RULE) {
                printf("  FAIL bad rule ACCEPTED (%s): %s", bads[k].why, bads[k].man);
                bad++;
            } else {
                printf("  ok bad rule refused at line %d (%s)\n", ln, bads[k].why);
            }
        }
    }
    return bad;
}

// ===========================================================================
// Arm 3: CHAINED, the real client order.
// ===========================================================================
static int arm_chained(void) {
    int bad = 0;
    const char *HOME = "/HOME/PLAYER";
    printf("--- arm 3/4: CHAINED (pkgman_dest_all then pkgdest_confine) ---\n");
    for (int i = 0; i < NV; i++) {
        char d[PKGMAN_MAX_DESTS][PKGMAN_DEST_MAX];
        int rc = pkgman_dest_all(MAN, sizeof(MAN) - 1, PKGID, V[i].member,
                                 d, PKGMAN_MAX_DESTS);
        if (rc <= 0) continue;                 // refused or skipped upstream
        for (int k = 0; k < rc; k++) {
            char c[PKGDEST_MAX];
            int crc = pkgdest_confine(HOME, d[k], c, sizeof(c));
            if (V[i].kind == 0) {
                printf("  FAIL: a hostile vector reached the confinement stage: %s\n",
                       V[i].member);
                bad++;
                continue;
            }
            if (crc != PKGDEST_OK) {
                printf("  FAIL benign refused by confinement (%s): %s -> %s\n",
                       pkgdest_strerror(crc), V[i].member, d[k]);
                bad++;
            } else if (!really_under(c, HOME)) {
                printf("  FAIL confinement escaped: %s -> %s\n", d[k], c);
                bad++;
            } else {
                printf("  ok %s -> %s\n", d[k], c);
            }
        }
    }
    // And the belt-and-braces case the two layers exist for: even if a future
    // pkgman bug let a traversal destination through, confinement must catch it.
    {
        char c[PKGDEST_MAX];
        int crc = pkgdest_confine(HOME, "/GAMES/ASSAULTCUBE/../../CONFIG/SHADOW",
                                  c, sizeof(c));
        if (crc == PKGDEST_OK && !really_under(c, HOME)) {
            printf("  FAIL: confinement let a traversal destination out: %s\n", c);
            bad++;
        } else {
            printf("  ok second layer holds: traversal destination -> %s\n",
                   crc == PKGDEST_OK ? c : pkgdest_strerror(crc));
        }
    }
    return bad;
}

// ===========================================================================
// Arm 4: CORPUS. The OLD resolver, verbatim, as the oracle.
// ===========================================================================
// Copied unchanged from userland/apps/appstore/main.c's install_dest_for() as
// it stood before this change. It is the definition of "what the 24 published
// packages already do", so it is what the new resolver must agree with.
static int old_install_dest_for(const char *man, size_t mlen, const char *pkgid,
                                const char *member, char *dest, int destcap) {
    int idl = (int)strlen(pkgid);
    if (strncmp(member, pkgid, (size_t)idl) != 0 || member[idl] != '/') return 0;
    const char *rel = member + idl + 1;
    size_t p = 0;
    while (p < mlen) {
        char line[256]; int ll = 0;
        while (p < mlen && man[p] != '\n' && ll < 255) line[ll++] = man[p++];
        if (p < mlen) p++;
        line[ll] = 0;
        if (line[0] == '#' || line[0] == 0) continue;
        char *arrow = strstr(line, "->");
        if (!arrow) continue;
        char src[128]; int si = 0;
        char *s = line;
        while (s < arrow && *s == ' ') s++;
        while (s < arrow && *s != ' ' && si < 127) src[si++] = *s++;
        src[si] = 0;
        if (strcmp(src, rel) != 0) continue;
        char *d = arrow + 2;
        while (*d == ' ') d++;
        int di = 0;
        while (*d && *d != ' ' && *d != '\r' && di < destcap - 1) dest[di++] = *d++;
        dest[di] = 0;
        return dest[0] ? 1 : 0;
    }
    return 0;
}

#define MAXMEM 4096
#define MEMLEN 320
static char g_members[MAXMEM][MEMLEN];
static int  g_nmem;
static char g_install[65536];
static size_t g_ilen;
static char g_pkgid[128];

static int corpus_one(void) {
    int bad = 0, fanout = 0, mapped = 0;
    int line = 0;
    if (pkgman_check_manifest(g_install, g_ilen, &line) != 0) {
        printf("  FAIL %s: pkgman rejects the SHIPPING manifest at line %d\n",
               g_pkgid, line);
        return 1;
    }
    for (int i = 0; i < g_nmem; i++) {
        char want[PKGMAN_DEST_MAX]; want[0] = 0;
        int oldrc = old_install_dest_for(g_install, g_ilen, g_pkgid,
                                        g_members[i], want, (int)sizeof(want));
        char d[PKGMAN_MAX_DESTS][PKGMAN_DEST_MAX];
        int rc = pkgman_dest_all(g_install, g_ilen, g_pkgid, g_members[i],
                                 d, PKGMAN_MAX_DESTS);
        if (rc < 0) {
            printf("  FAIL %s member %s: rc=%d %s\n", g_pkgid, g_members[i],
                   rc, pkgman_strerror(rc));
            bad++;
            continue;
        }
        if (oldrc == 1) {
            if (rc < 1) {
                printf("  FAIL %s: %s mapped to %s before, nothing now\n",
                       g_pkgid, g_members[i], want);
                bad++;
            } else if (strcmp(d[0], want) != 0) {
                printf("  FAIL %s: %s -> %s now, %s before\n",
                       g_pkgid, g_members[i], d[0], want);
                bad++;
            } else {
                mapped++;
                if (rc > 1) fanout += rc - 1;
            }
        } else if (rc > 0) {
            printf("  FAIL %s: %s was UNREFERENCED before, maps to %s now\n",
                   g_pkgid, g_members[i], d[0]);
            bad++;
        }
    }
    printf("  %-22s %2d members mapped identically, %d extra destination(s) now honoured\n",
           g_pkgid, mapped, fanout);
    return bad;
}

static int arm_corpus(const char *path) {
    printf("--- arm 4/4: CORPUS (24 published packages, old resolver as oracle) ---\n");
    FILE *f = fopen(path, "r");
    if (!f) { printf("  FAIL: cannot open %s\n", path); return 1; }
    int bad = 0, npkg = 0, state = 0;   // 0 idle, 1 members, 2 install
    char ln[1024];
    g_nmem = 0; g_ilen = 0; g_pkgid[0] = 0;
    while (fgets(ln, sizeof(ln), f)) {
        size_t l = strlen(ln);
        while (l && (ln[l - 1] == '\n' || ln[l - 1] == '\r')) ln[--l] = 0;
        if (strncmp(ln, "=== PKG ", 8) == 0) {
            strncpy(g_pkgid, ln + 8, sizeof(g_pkgid) - 1);
            g_pkgid[sizeof(g_pkgid) - 1] = 0;
            g_nmem = 0; g_ilen = 0; state = 0;
            continue;
        }
        if (strcmp(ln, "--- MEMBERS") == 0) { state = 1; continue; }
        if (strcmp(ln, "--- INSTALL") == 0) { state = 2; continue; }
        if (strcmp(ln, "--- END") == 0) {
            if (g_pkgid[0] && g_ilen) { bad += corpus_one(); npkg++; }
            state = 0;
            continue;
        }
        if (state == 1) {
            if (g_nmem < MAXMEM && l > 0 && l < MEMLEN) {
                strcpy(g_members[g_nmem], ln);
                g_nmem++;
            }
            continue;
        }
        if (state == 2) {
            if (g_ilen + l + 1 < sizeof(g_install)) {
                memcpy(g_install + g_ilen, ln, l);
                g_ilen += l;
                g_install[g_ilen++] = '\n';
            }
            continue;
        }
    }
    fclose(f);
    printf("  %d packages replayed\n", npkg);
    if (npkg < 20) { printf("  FAIL: corpus too small to mean anything (%d)\n", npkg); bad++; }
    return bad;
}

// ---------------------------------------------------------------------------
// Arm 5: the "#!mpkg-min-client <n>" declaration.
//
// It exists so that a package can say which client it needs and a client can
// refuse one it does not understand, INSTEAD of installing nothing and calling
// that a result. So the cases that matter are the ones where a wrong answer is
// indistinguishable from no requirement at all: a malformed number, trailing
// text after it, a near-miss marker. Every one of those must be REFUSED, never
// read as generation 1, because "I could not read the requirement" and "there
// is no requirement" lead to opposite actions.
// ---------------------------------------------------------------------------
static int arm_minclient(void) {
    struct { const char *man; int want; const char *why; } T[] = {
      { "a -> /APPS/A\n",                          1, "no directive: generation 1" },
      { "#!mpkg-min-client 2\na/ -> /GAMES/A/\n",  2, "plain declaration" },
      { "# a comment\n#!mpkg-min-client 2\n",      2, "not required to be first" },
      { "   #!mpkg-min-client 3\n",                 3, "leading blanks tolerated" },
      { "#!mpkg-min-client 2\r\n",                 2, "CRLF manifest" },
      { "#!mpkg-min-client 2   \n",                 2, "trailing blanks tolerated" },
      { "#!mpkg-min-client 2\n#!mpkg-min-client 2\n", 2, "repeated, same value" },
      { "#!mpkg-min-client\n",       PKGMAN_E_RULE, "marker with no number" },
      { "#!mpkg-min-client \n",      PKGMAN_E_RULE, "marker with blank number" },
      { "#!mpkg-min-client x\n",     PKGMAN_E_RULE, "non-numeric" },
      { "#!mpkg-min-client 2x\n",    PKGMAN_E_RULE, "trailing junk" },
      { "#!mpkg-min-client 2 or later\n", PKGMAN_E_RULE, "prose after the number" },
      { "#!mpkg-min-client 0\n",     PKGMAN_E_RULE, "generation 0 is not a generation" },
      { "#!mpkg-min-client 100\n",   PKGMAN_E_RULE, "implausible generation" },
      { "#!mpkg-min-client2\n",      PKGMAN_E_RULE, "no separator after the marker" },
      { "#!mpkg-min-client 2\n#!mpkg-min-client 3\n", PKGMAN_E_RULE, "two different answers" },
      { "#!mpkg-min-clientele 2\n",  PKGMAN_E_RULE, "marker is a prefix of a longer word" },
    };
    int bad = 0;
    printf("--- arm 5/5: #!mpkg-min-client declaration ---\n");
    for (unsigned i = 0; i < sizeof(T)/sizeof(T[0]); i++) {
        int got = pkgman_min_client(T[i].man, (unsigned long)strlen(T[i].man));
        if (got != T[i].want) {
            printf("  FAIL want=%d got=%d : %s\n", T[i].want, got, T[i].why);
            bad++;
        } else {
            printf("  ok   %-3d %s\n", got, T[i].why);
        }
    }
    // The whole point: a generation above this client's is actionable.
    if (pkgman_min_client("#!mpkg-min-client 3\n", 20) <= PKGMAN_CLIENT_GEN) {
        printf("  FAIL: generation 3 did not read as newer than this client (%d)\n",
               PKGMAN_CLIENT_GEN);
        bad++;
    } else {
        printf("  ok   a generation-3 package reads as NEWER than this client (%d)\n",
               PKGMAN_CLIENT_GEN);
    }
    return bad;
}

#endif // !PKGMAN_NAIVE

int main(int argc, char **argv) {
#ifdef PKGMAN_NAIVE
    (void)argc; (void)argv;
    fails = arm_naive();
    printf("%s\n", fails ? "NEGATIVE ARM: FAIL" : "NEGATIVE ARM: PASS");
    return fails ? 1 : 0;
#else
    const char *corpus = (argc > 1) ? argv[1] : "fixtures/mpkg-install-corpus.txt";
    fails  = arm_positive();
    printf("\n");
    fails += arm_chained();
    printf("\n");
    fails += arm_corpus(corpus);
    printf("\n");
    fails += arm_minclient();
    printf("\n%s (%d failures)\n", fails ? "POSITIVE ARMS: FAIL" : "POSITIVE ARMS: PASS", fails);
    return fails ? 1 : 0;
#endif
}
