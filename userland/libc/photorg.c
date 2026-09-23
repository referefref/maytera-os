// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// photorg.c - photo organiser under an AI escrow contract (#712, #246/#305).
// See photorg.h and escrow.h. It drives the KERNEL-ENFORCED escrow thin client
// (escrow.c): request enters the kernel contract, the organiser mkdirs/moves
// inside the marked window (each checked at Ring 0), verify is the postcondition
// oracle, close revokes early on fulfilment or aborts (kernel rollback) on a
// failed promise. Nothing here deletes a file.
//
// #713: the grouping date is now the real photo CAPTURE date (EXIF
// DateTimeOriginal) when the file carries one, falling back to the file
// modification time (mtime) exactly as before when it does not.
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"
#include "aiclient.h"     // aiclient_run_action, AICAP_ALLOW via aicap.h
#include "aicap.h"
#include "escrow.h"
#include "exifdate.h"     // #713 EXIF DateTimeOriginal capture date
#include "photorg.h"

// ---------------------------------------------------------------------------
// Small local helpers.
// ---------------------------------------------------------------------------
static int ci_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}
static void join2(const char *a, const char *b, char *out, int ocap) {
    int n = (int)strlen(a);
    if (n > 0 && a[n - 1] == '/') snprintf(out, ocap, "%s%s", a, b);
    else                          snprintf(out, ocap, "%s/%s", a, b);
}
static int path_exists(const char *p) { struct stat st; return sys_stat(p, &st) == 0; }

// Is `name` an image by extension (case-insensitive)?
static int is_image(const char *name) {
    const char *dot = 0;
    for (const char *s = name; *s; s++) if (*s == '.') dot = s;
    if (!dot || !dot[1]) return 0;
    static const char *ext[] = { "jpg","jpeg","png","bmp","gif","webp",
                                 "heic","tiff","tif",0 };
    for (int i = 0; ext[i]; i++) if (ci_eq(dot + 1, ext[i])) return 1;
    return 0;
}

// Does this extension name a JPEG (the only container EXIF DateTimeOriginal is
// read from here)? Case-insensitive.
static int is_jpeg(const char *name) {
    const char *dot = 0;
    for (const char *s = name; *s; s++) if (*s == '.') dot = s;
    if (!dot || !dot[1]) return 0;
    return ci_eq(dot + 1, "jpg") || ci_eq(dot + 1, "jpeg");
}

// Civil calendar from a UNIX epoch second (UTC). Howard Hinnant's algorithm,
// pure integer math (the kernel is soft-float; this stays integer regardless).
static void ymd_from_epoch(long secs, int *y, int *m, int *d) {
    long z = secs / 86400;
    if (secs < 0 && (secs % 86400)) z--;   // floor for negative epochs
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yr = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp < 10 ? mp + 3 : mp - 9;
    yr += (mm <= 2);
    *y = (int)yr; *m = (int)mm; *d = (int)dd;
}

// Folder name for an already-resolved Y/M/D at the requested granularity.
static void folder_name_ymd(int y, int m, int d, const char *gb,
                            char *out, int ocap) {
    int gran = 2;   // 1=YYYY, 2=YYYY-MM, 3=YYYY-MM-DD
    if (gb && gb[0]) {
        if (strstr(gb, "DD") || strstr(gb, "dd"))      gran = 3;
        else if (strstr(gb, "MM") || strstr(gb, "mm")) gran = 2;
        else if (strstr(gb, "YYYY") || strstr(gb, "yyyy")) gran = 1;
    }
    if (gran == 1)      snprintf(out, ocap, "%04d", y);
    else if (gran == 3) snprintf(out, ocap, "%04d-%02d-%02d", y, m, d);
    else                snprintf(out, ocap, "%04d-%02d", y, m);
}

// Resolve the grouping Y/M/D for one photo: prefer the EXIF capture date
// (DateTimeOriginal) when the file is a JPEG that carries one, otherwise fall
// back to the file modification time. `src` is the full path; `name` decides
// whether EXIF is even worth attempting; `mtime` is the already-stat'd mtime.
static void resolve_group_ymd(const char *src, const char *name, long mtime,
                              int *y, int *m, int *d) {
    if (is_jpeg(name) && exifdate_from_file(src, y, m, d)) return;  // #713 capture date
    ymd_from_epoch(mtime, y, m, d);                                 // mtime fallback
}

// Split `name` into stem + extension (ext includes the dot, or "").
static void split_ext(const char *name, char *stem, int scap, char *ext, int ecap) {
    const char *dot = 0;
    for (const char *s = name; *s; s++) if (*s == '.') dot = s;
    if (!dot) { strlcpy(stem, name, scap); ext[0] = 0; return; }
    int n = (int)(dot - name);
    if (n > scap - 1) n = scap - 1;
    memcpy(stem, name, n); stem[n] = 0;
    strlcpy(ext, dot, ecap);
}

// Is this destination already claimed by an earlier planned move?
static int dst_claimed(escrow_contract_t *c, const char *dst) {
    for (int i = 0; i < c->n_moves; i++)
        if (!strcmp(c->moves[i].dst, dst)) return 1;
    return 0;
}

// Compute a collision-safe destination inside `folderpath` for `name`: never an
// existing file, never a path already claimed in this plan. Appends _1, _2, ...
// before the extension. Guarantees we never overwrite or lose a file (and so
// never triggers a rename-over, which FAT cannot do anyway).
static void unique_dst(escrow_contract_t *c, const char *folderpath,
                       const char *name, char *out, int ocap) {
    char cand[256];
    join2(folderpath, name, cand, sizeof(cand));
    if (!path_exists(cand) && !dst_claimed(c, cand)) { strlcpy(out, cand, ocap); return; }
    char stem[192], ext[32];
    split_ext(name, stem, sizeof(stem), ext, sizeof(ext));
    for (int k = 1; k < 10000; k++) {
        char nm[256];
        snprintf(nm, sizeof(nm), "%s_%d%s", stem, k, ext);
        join2(folderpath, nm, cand, sizeof(cand));
        if (!path_exists(cand) && !dst_claimed(c, cand)) { strlcpy(out, cand, ocap); return; }
    }
    strlcpy(out, cand, ocap);   // pathological fallback (>10000 collisions)
}

// ---------------------------------------------------------------------------
// The orchestrator.
// ---------------------------------------------------------------------------
int photorg_organize(const char *scope, const char *group_by, char *summary, int scap) {
    if (summary && scap) summary[0] = 0;
    if (!scope || !scope[0]) {
        if (summary) strlcpy(summary, "No device path was given to organise.", scap);
        return ESCROW_ERROR;
    }
    const char *gb = (group_by && group_by[0]) ? group_by : "YYYY-MM";

    char note[192];
    snprintf(note, sizeof(note),
             "organise images under %s into %s date folders; create folders, "
             "move photos bytes-intact, delete nothing", scope, gb);

    escrow_contract_t *c = escrow_request(scope, note, ESCROW_TTL_DEFAULT);
    if (!c) {
        if (summary) snprintf(summary, scap,
            "Could not open an escrow contract for %s.", scope);
        return ESCROW_ERROR;
    }

    // ---- PLAN + declare the promise ---------------------------------------
    char (*names)[256] = malloc(sizeof(char[256]) * 256);
    if (!names) {
        if (summary) strlcpy(summary, "Out of memory planning the organise.", scap);
        escrow_close(c);
        return ESCROW_ERROR;
    }
    unsigned char isd[256];
    unsigned int szs[256];
    int n = escrow_list_dir(scope, names, isd, szs, 256);
    int skipped_nonimg = 0, skipped_dir = 0, planned = 0, plan_full = 0;
    for (int i = 0; i < n; i++) {
        if (isd[i]) { skipped_dir++; continue; }
        if (!is_image(names[i])) { skipped_nonimg++; continue; }
        char src[256];
        join2(scope, names[i], src, sizeof(src));
        struct stat st;
        long mt = 0;
        if (sys_stat(src, &st) == 0) mt = (long)st.st_mtime;
        // #713: group by the EXIF capture date when present, else by mtime.
        int gy, gm, gd;
        resolve_group_ymd(src, names[i], mt, &gy, &gm, &gd);
        char folder[40], folderpath[256], dst[256];
        folder_name_ymd(gy, gm, gd, gb, folder, sizeof(folder));
        join2(scope, folder, folderpath, sizeof(folderpath));
        // Skip a photo that is already sitting in its correct date folder.
        {
            char already[256];
            join2(folderpath, names[i], already, sizeof(already));
            if (!strcmp(already, src)) continue;
        }
        escrow_promise_add_folder(c, folderpath);
        unique_dst(c, folderpath, names[i], dst, sizeof(dst));
        if (escrow_promise_add_move(c, src, dst) == 0) planned++;
        else plan_full++;
    }
    free(names);

    // ---- EXECUTE inside the KERNEL-ENFORCED marked window --------------------
    // Each mkdir / move is an ordinary FS syscall that the KERNEL escrow guard
    // (escrow_fs_guard) checks against the live grant (scope + no-delete +
    // device) and records in the tamper-evident journal. The kernel is the
    // authority, so there is no userland gate to duplicate here; escrow_do_mkdir
    // / escrow_do_move are thin syscall wrappers and write no mid-window audit.
    for (int i = 0; i < c->n_folders; i++)
        escrow_do_mkdir(c, c->folders[i].path);   // already-exists resolved by verify
    for (int i = 0; i < c->n_moves; i++) {
        escrow_move_t *m = &c->moves[i];
        m->executed = (escrow_do_move(c, m->src, m->dst) == 0);
    }

    // ---- VERIFY + CLOSE ----------------------------------------------------
    escrow_verify_t vr;
    int verdict = escrow_verify(c, &vr);
    int cl = escrow_close(c);

    // ---- HUMAN SUMMARY -----------------------------------------------------
    // Sequential snprintf, always clamped so `o` never runs past the buffer
    // (a truncated snprintf returns the would-be length, not what it wrote).
    if (summary && scap) {
        int o = 0;
        #define SREM (o < scap ? scap - o : 0)
        #define SADV(r) do { int _r = (r); if (_r > 0) o += _r; if (o > scap) o = scap; } while (0)
        SADV(snprintf(summary + o, SREM,
            "I organised the photos on %s under a time-boxed escrow contract "
            "(1 hour, closing early the moment the promise is kept).\n"
            "The contract PROMISED: create the %s date folders, move each photo "
            "into its folder WITHOUT copying, and DELETE NOTHING, all inside %s.\n"
            "Photos are grouped by their EXIF capture date when present, "
            "otherwise by file modification time.\n",
            scope, gb, scope));
        if (verdict == ESCROW_FULFILLED) {
            SADV(snprintf(summary + o, SREM,
                "Promise: FULFILLED. Created %d date folder(s); relocated %d photo(s) "
                "bytes-identical with the originals gone; %d file(s) deleted.\n",
                vr.folders_ok, vr.moves_ok, vr.delete_count));
            SADV(snprintf(summary + o, SREM,
                "Skipped %d non-image file(s) and %d existing folder(s), untouched.\n",
                skipped_nonimg, skipped_dir));
            SADV(snprintf(summary + o, SREM,
                "%s The write/move/mkdir grant was KERNEL-ENFORCED and scoped to "
                "%s only, with no delete permission, and was REVOKED early on "
                "fulfilment (before the 1-hour limit). Enforcement is at Ring 0 "
                "with a tamper-evident audit trail.\n",
                cl == ESCROW_CLOSED_EARLY ? "Closed EARLY." : "Closed.", scope));
        } else {
            SADV(snprintf(summary + o, SREM,
                "Promise: NOT fully kept (PARTIAL). Created %d/%d folder(s), moved "
                "%d/%d photo(s), %d file(s) deleted. Unmet:\n",
                vr.folders_ok, vr.folders_total, vr.moves_ok, vr.moves_total,
                vr.delete_count));
            for (int i = 0; i < vr.n_unmet && o < scap - 80; i++)
                SADV(snprintf(summary + o, SREM, "  - %s\n", vr.unmet[i]));
            SADV(snprintf(summary + o, SREM,
                "The contract was ABORTED: the kernel rolled back its reversible "
                "effects (moves back, empty date folders removed) and revoked the "
                "grant, so the device is left as it was found. Enforcement is at "
                "Ring 0 with a tamper-evident audit trail.\n"));
        }
        if (plan_full)
            SADV(snprintf(summary + o, SREM,
                "(Note: %d photo(s) exceeded the per-contract plan cap and were "
                "not scheduled this run.)\n", plan_full));
        #undef SREM
        #undef SADV
    }
    return verdict;
}
