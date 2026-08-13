// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pkgdest_test.c - #745 table-driven containment battery for pkgdest_confine().
//
// TWO ARMS, both required (the convention set by run.sh and run_malloc.sh in
// this directory):
//
//   NEGATIVE CONTROL  (-DPKGDEST_NAIVE) replaces the confinement with the
//                     obvious wrong implementation, a plain prefix
//                     concatenation with no canonicalization, and REQUIRES the
//                     hostile vectors to ESCAPE. If they do not escape under
//                     the naive join, the vector is not hostile and proves
//                     nothing, so that is a failure too.
//
//   POSITIVE          runs the real pkgdest.c and requires every hostile vector
//                     to be REFUSED and every benign vector to be rewritten to
//                     exactly the expected path.
//
// Compiled freestanding against the real ../pkgdest.c; only the harness itself
// uses the host libc.
#include <stdio.h>
#include <string.h>

#include "../pkgdest.h"

#ifdef PKGDEST_NAIVE
// The wrong implementation, verbatim as someone would first write it: prefix
// the sandbox onto the manifest destination and call it confined.
static int naive_confine(const char *sandbox, const char *dest, char *out,
                         unsigned long cap) {
    if (dest[0] != '/') return PKGDEST_E_NOTABS;
    unsigned long sl = strlen(sandbox);
    if (sl == 1 && sandbox[0] == '/') sl = 0;
    if (sl + strlen(dest) + 1 > cap) return PKGDEST_E_TOOLONG;
    memcpy(out, sandbox, sl);
    strcpy(out + sl, dest);
    return PKGDEST_OK;
}
#define CONFINE naive_confine
#else
#define CONFINE pkgdest_confine
#endif

#define HOME "/HOME/ADMIN"

static int fails;

// Does `p` actually lie inside `HOME`? This is the ORACLE, and it is
// deliberately computed by resolving the string the way a filesystem would,
// NOT by calling anything in pkgdest.c: a test that asks the unit under test
// whether the unit under test was right proves nothing.
static int really_inside(const char *p) {
    // Resolve with an independent stack-based walker.
    char comp[64][64];
    int n = 0;
    if (p[0] != '/') return 0;
    const char *q = p + 1;
    while (*q) {
        char c[64];
        int l = 0;
        while (*q && *q != '/' && l < 63) c[l++] = *q++;
        c[l] = 0;
        while (*q == '/') q++;
        if (l == 0 || strcmp(c, ".") == 0) continue;
        if (strcmp(c, "..") == 0) { if (n > 0) n--; continue; }
        if (n < 64) strcpy(comp[n++], c);
    }
    char flat[64 * 66];
    int w = 0;
    for (int i = 0; i < n; i++)
        w += snprintf(flat + w, sizeof(flat) - (size_t)w, "/%s", comp[i]);
    flat[w] = 0;
    return strncmp(flat, HOME "/", strlen(HOME) + 1) == 0;
}

// kind: 0 = must be REFUSED (hostile), 1 = must be ACCEPTED and equal `want`.
// neg:  1 = the NEGATIVE arm must be able to show this one ESCAPING a naive
//           prefix join. Set EXPLICITLY, never derived from the string: a
//           heuristic silently reclassified two allowlist vectors as
//           containment vectors on the first run and failed the negative arm
//           for the wrong reason.
struct tc { const char *dest; int kind; int neg; const char *want; const char *why; };

static const struct tc T[] = {
    // ---- benign, and what the rewrite must produce exactly ----------------
    { "/APPS/COUNTER",              1, 0, HOME "/APPS/COUNTER",        "plain app destination" },
    { "/APPS/counter",              1, 0, HOME "/APPS/counter",        "lowercase alias" },
    { "/GAMES/OPENARENA/baseoa/x.pk3", 1, 0, HOME "/GAMES/OPENARENA/baseoa/x.pk3", "nested game data" },
    { "/THEMES/dark.mtheme",        1, 0, HOME "/THEMES/dark.mtheme",  "theme payload" },
    { "//APPS//counter",            1, 0, HOME "/APPS/counter",        "duplicate separators collapse" },
    { "/APPS/./counter",            1, 0, HOME "/APPS/counter",        "dot component dropped" },
    { "/APPS/sub/../counter",       1, 0, HOME "/APPS/counter",        "interior dotdot resolves in-sandbox" },
    { "/APPS/counter/",             1, 0, HOME "/APPS/counter",        "trailing slash stripped" },

    // ---- hostile: traversal out of the profile ----------------------------
    { "/APPS/../../CONFIG/SHADOW",  0, 1, 0, "classic dotdot escape onto the password database" },
    { "/../CONFIG/SHADOW",          0, 1, 0, "dotdot at the first component" },
    { "/APPS/../../../../../../../../CONFIG/PASSWD", 0, 1, 0, "over-popping past the root" },
    { "/APPS/..%2f..%2fCONFIG",     1, 0, HOME "/APPS/..%2f..%2fCONFIG", "percent-encoding is NOT a separator here" },
    { "/./../APPS/../../CONFIG",    0, 1, 0, "dot and dotdot mixed" },
    { "/APPS/sub/../../../CONFIG/SHADOW", 0, 1, 0, "escape only after interior components" },

    // ---- hostile: absolute re-anchoring -----------------------------------
    // Confinement alone would ACCEPT these, rewriting them into the user's own
    // profile. The installable-prefix allowlist is what refuses them, and it is
    // applied to the CANONICAL form so a "/APPS/..." spelling cannot smuggle a
    // "/CONFIG" destination past it.
    { "/CONFIG/SHADOW",             0, 0, 0, "system config re-anchor" },
    { "/CONFIG/PERMS.DB",           0, 0, 0, "permission database re-anchor" },
    { "/CONFIG/STARTMENU/X.MENU",   0, 0, 0, "Start-menu fragment injection" },
    { "/APPS/../CONFIG/SHADOW",     0, 0, 0, "allowlisted spelling, non-allowlisted resolution" },
    { "/HOME/REF/APPS/evil",        0, 0, 0, "another user's profile" },
    { "/HOME/ADMIN/CONFIG/STORE.DB",0, 0, 0, "the user's own install registry" },
    { "/STOREDL.TMP",               0, 0, 0, "loose file at the root" },
    { "/APPSTORE/x",                0, 0, 0, "prefix that merely starts with APPS" },
    { "/APPS",                      0, 0, 0, "names the directory, not a file" },
    { "/MAYTERA.BMP",               0, 0, 0, "wallpaper: root-level BMP, system scope only" },
    { "/CONFIG/../MAYTERA.BMP",     0, 0, 0, "wallpaper by traversal, still per-user refused" },

    // ---- hostile: the boot medium, refused for every caller ---------------
    { "/BOOT/kernel.elf",           0, 0, 0, "boot path" },
    { "/boot/KERNEL.ELF",           0, 0, 0, "boot path, lowercased" },
    { "/EFI/BOOT/BOOTX64.EFI",      0, 0, 0, "UEFI loader" },
    { "/KERNEL.ELF",                0, 0, 0, "ESP-root kernel copy" },

    // ---- hostile: degenerate / malformed ----------------------------------
    { "/",                          0, 0, 0, "names no file" },
    { "/..",                        0, 1, 0, "resolves to the root" },
    { "/APPS/..",                   0, 1, 0, "resolves to the root after popping" },
    { "APPS/counter",               0, 0, 0, "not absolute" },
    { "../APPS/counter",            0, 0, 0, "relative traversal" },
    { "",                           0, 0, 0, "empty" },
};

int main(void) {
    int n = (int)(sizeof(T) / sizeof(T[0]));

#ifdef PKGDEST_NAIVE
    printf("== NEGATIVE CONTROL: naive prefix join, traversal vectors MUST escape ==\n");
    int escapes = 0, traversals = 0;
    for (int i = 0; i < n; i++) {
        if (!T[i].neg) continue;
        traversals++;
        char out[PKGDEST_MAX];
        int rc = CONFINE(HOME, T[i].dest, out, sizeof(out));
        int inside = (rc == PKGDEST_OK) && really_inside(out);
        if (rc == PKGDEST_OK && !inside) {
            escapes++;
            printf("  ESCAPED (as required): %-46s -> %s\n", T[i].dest, out);
        } else {
            printf("  NOT ESCAPED: %-46s rc=%d out=%s [%s]\n",
                   T[i].dest, rc, rc == PKGDEST_OK ? out : "-", T[i].why);
            fails++;
        }
    }
    printf("  %d/%d traversal vectors escaped the naive join\n", escapes, traversals);
    if (traversals == 0) { printf("  no traversal vectors: the table proves nothing\n"); fails++; }
#else
    printf("== POSITIVE: pkgdest_confine(), sandbox %s ==\n", HOME);
    for (int i = 0; i < n; i++) {
        char out[PKGDEST_MAX];
        out[0] = 0;
        int rc = CONFINE(HOME, T[i].dest, out, sizeof(out));
        if (T[i].kind == 1) {
            if (rc != PKGDEST_OK || strcmp(out, T[i].want) != 0) {
                printf("  FAIL %-46s rc=%d out=%s want=%s [%s]\n",
                       T[i].dest, rc, out, T[i].want, T[i].why);
                fails++;
                continue;
            }
            // The oracle must agree that the accepted path really is inside.
            if (!really_inside(out)) {
                printf("  FAIL %-46s accepted but the oracle says it is OUTSIDE: %s\n",
                       T[i].dest, out);
                fails++;
                continue;
            }
            printf("  ok   %-46s -> %s\n", T[i].dest, out);
        } else {
            if (rc == PKGDEST_OK) {
                printf("  FAIL %-46s ACCEPTED as %s [%s]\n", T[i].dest, out, T[i].why);
                fails++;
                continue;
            }
            printf("  ok   %-46s REFUSED: %s [%s]\n",
                   T[i].dest, pkgdest_strerror(rc), T[i].why);
        }
    }

    // ---- root session: home is "/", so confinement must be the identity ---
    printf("== POSITIVE: sandbox \"/\" (a root session) is a no-op rewrite ==\n");
    // The last four are the REAL destinations from the shipping repository's
    // wallpaper and theme packages (wp-maytera, wp-cyber, theme-darkmode,
    // theme-cyberpunk), read out of the .mpkg INSTALL manifests. A root install
    // of any of them must still work exactly as it does today.
    static const char *rootcases[] = { "/APPS/COUNTER", "/GAMES/OPENARENA/baseoa/pak0.pk3",
                                       "/APPS/counter", "/THEMES/cyberpunk.mtheme",
                                       "/MAYTERA.BMP", "/CYBER.BMP", "/DARKMODE.BMP",
                                       "/OCEAN.BMP" };
    for (int i = 0; i < 8; i++) {
        char out[PKGDEST_MAX];
        int rc = pkgdest_confine("/", rootcases[i], out, sizeof(out));
        if (rc != PKGDEST_OK || strcmp(out, rootcases[i]) != 0) {
            printf("  FAIL %-32s rc=%d out=%s (root behaviour must be unchanged)\n",
                   rootcases[i], rc, rc == PKGDEST_OK ? out : "-");
            fails++;
        } else {
            printf("  ok   %-32s -> %s (unchanged)\n", rootcases[i], out);
        }
    }
    // ...but the boot medium is refused even for root, matching the kernel's
    // pkg_path_is_boot() rule.
    {
        char out[PKGDEST_MAX];
        int rc = pkgdest_confine("/", "/EFI/BOOT/BOOTX64.EFI", out, sizeof(out));
        if (rc != PKGDEST_E_BOOT) {
            printf("  FAIL root /EFI/BOOT/BOOTX64.EFI rc=%d (want PKGDEST_E_BOOT)\n", rc);
            fails++;
        } else {
            printf("  ok   root /EFI/BOOT/BOOTX64.EFI  REFUSED: %s\n", pkgdest_strerror(rc));
        }
    }

    // ---- length: refuse, never truncate ----------------------------------
    {
        char big[PKGDEST_MAX * 2];
        int w = 0;
        big[w++] = '/';
        for (int i = 0; i < PKGDEST_MAX; i++) big[w++] = 'A';
        big[w] = 0;
        char out[PKGDEST_MAX];
        int rc = pkgdest_confine(HOME, big, out, sizeof(out));
        if (rc != PKGDEST_E_TOOLONG) {
            printf("  FAIL overlong destination rc=%d (want PKGDEST_E_TOOLONG)\n", rc);
            fails++;
        } else {
            printf("  ok   overlong destination REFUSED: %s\n", pkgdest_strerror(rc));
        }
    }
#endif

    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
