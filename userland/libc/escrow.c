// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// escrow.c - AI escrow contract, now a THIN CLIENT of the KERNEL-ENFORCED
// escrow (#246/#305 Stage 6, docs/CONTRACT_ENFORCEMENT_PLAN.md). See escrow.h.
//
// WHAT MOVED (Stage 6 Part A). Stages 1-5 landed the kernel enforcement: a
// process that enters escrow via SYS_ESCROW_ENTER becomes a marked actor whose
// FS mutations are checked AT RING 0 against a GraphFS grant (scope WRITE only,
// never DELETE, device-bound if the scope is removable), with every decision in
// the tamper-evident journal and a kernel rollback engine (SYS_ESCROW_ABORT).
// So this file no longer RE-IMPLEMENTS enforcement in userland. escrow_request()
// ENTERS the kernel contract (the kernel is the authority for scope + no-delete
// + device); the FS mutations run as ordinary syscalls that the kernel guard
// checks; and on a failed promise escrow_close() calls SYS_ESCROW_ABORT so the
// KERNEL rolls back the reversible effects.
//
// WHAT STAYS USERLAND, and honestly WHY. The PROMISE check (escrow_verify) is a
// postcondition ORACLE, not an enforcement boundary: it reads the after-state
// and reports whether the declared folders/moves/zero-deletes actually hold. It
// cannot be, and is not, the thing that stops an out-of-scope write - the kernel
// is. The BEFORE snapshot (delete counter) feeds that oracle. Removing them
// would remove the ability to say "the promise was kept", not any enforcement.
//
// NO SECOND SOURCE OF TRUTH. The old userland aicap grant/deny (allowed_paths
// scoping + fs.delete denylist) is GONE from this file: it duplicated what the
// kernel now enforces. aicap_audit() is retained only for the human-readable
// /CONFIG/AIAUDIT.LOG breadcrumb; the AUTHORITATIVE per-decision record is the
// kernel's tamper-evident journal.
//
// THE MARKED WINDOW (an honest operational note). Between escrow_request() and
// escrow_close() the calling process IS a marked kernel escrow actor, so it may
// not perform FS writes OUTSIDE the granted scope in that window. The organiser
// is self-contained (it only mkdirs/moves inside scope), and every /CONFIG audit
// write this file makes is done BEFORE the enter or AFTER the close, never while
// marked, precisely because an out-of-scope /CONFIG write would (correctly) be
// refused by the kernel guard.
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "aicap.h"
#include "escrow.h"

// Single active contract (the owner's scenario is one contract at a time). The
// struct is large (a bounded snapshot + promise), so it lives in BSS, never on
// the caller's stack.
static escrow_contract_t g_contract;
static int g_escrow_seq = 1;

// ---------------------------------------------------------------------------
// Small helpers (path + stat + content hash). None is a shared wait/lock/list
// primitive; they are file-scope conveniences over the real syscalls.
// ---------------------------------------------------------------------------
static long esc_now(void) {
    long t = sys_time();
    if (t > 1000000000L) return t;
    return 1700000000L + (long)(uptime_ms() / 1000);
}

static int esc_stat(const char *path, struct stat *st) {
    return sys_stat(path, st);
}
static int esc_exists(const char *path) {
    struct stat st;
    return esc_stat(path, &st) == 0;
}
static int esc_is_dir(const char *path) {
    struct stat st;
    return esc_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static int esc_is_file(const char *path) {
    struct stat st;
    return esc_stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// FNV-1a over the whole file. NON-cryptographic: this is a content-integrity
// check for the promise, not a security hash. A move is a pure rename so the
// bytes are identical by construction; the hash is a belt-and-braces verifier.
static int esc_file_hash(const char *path, unsigned int *hash_out, long *size_out) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned int h = 2166136261u;
    long total = 0;
    static char buf[4096];   // file-scope, not on the stack
    for (;;) {
        long n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            h ^= (unsigned char)buf[i];
            h *= 16777619u;
        }
        total += n;
    }
    sys_close(fd);
    if (hash_out) *hash_out = h;
    if (size_out) *size_out = total;
    return 0;
}

// ---------------------------------------------------------------------------
// Shared readdir helper (same raw-SYS_READDIR idiom as aiclient.c exec_files_list)
// ---------------------------------------------------------------------------
int escrow_list_dir(const char *dir, char (*names)[256], unsigned char *isdir,
                    unsigned int *sizes, int max) {
    int fd = sys_open(dir, 0);
    if (fd < 0) return -1;
    int n = 0;
    dirent_t e;
    while (n < max) {
        int r = (int)syscall2(SYS_READDIR, fd, (long)&e);
        if (r != 0) break;
        if (e.name[0] == 0) break;
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        strlcpy(names[n], e.name, 256);
        if (isdir) isdir[n] = DIRENT_IS_DIR(e) ? 1 : 0;
        if (sizes) sizes[n] = e.size;
        n++;
    }
    sys_close(fd);
    return n;
}

static void join2(const char *a, const char *b, char *out, int ocap) {
    int n = (int)strlen(a);
    if (n > 0 && a[n - 1] == '/') snprintf(out, ocap, "%s%s", a, b);
    else                          snprintf(out, ocap, "%s/%s", a, b);
}

// ---------------------------------------------------------------------------
// Recursive BEFORE snapshot: every regular file under scope (bounded), for the
// delete counter. Iterative BFS with a heap queue so no big arrays sit on the
// stack and no unbounded recursion runs. This runs BEFORE the kernel contract
// is entered, so its reads are ordinary reads (and reads are never guarded).
// ---------------------------------------------------------------------------
static void snap_recursive(escrow_contract_t *c, const char *root) {
    char (*q)[256] = malloc(sizeof(char[256]) * ESCROW_MAX_DIRS);
    char (*names)[256] = malloc(sizeof(char[256]) * 128);
    if (!q || !names) {
        // Degrade to top-level only rather than fail the contract.
        if (names) {
            unsigned char isd[128]; unsigned int szs[128];
            int n = escrow_list_dir(root, names, isd, szs, 128);
            for (int i = 0; i < n && c->n_before < ESCROW_MAX_FILES; i++) {
                if (isd[i]) continue;
                escrow_file_t *f = &c->before[c->n_before++];
                join2(root, names[i], f->path, sizeof(f->path));
                f->size = (long)szs[i]; f->is_dir = 0; f->seen_after = 0;
            }
        }
        free(q); free(names);
        return;
    }
    int qh = 0, qt = 0;
    strlcpy(q[qt++], root, 256);
    unsigned char isd[128];
    unsigned int szs[128];
    while (qh < qt) {
        char dir[256];
        strlcpy(dir, q[qh++], sizeof(dir));
        int n = escrow_list_dir(dir, names, isd, szs, 128);
        if (n < 0) continue;
        for (int i = 0; i < n; i++) {
            char full[256];
            join2(dir, names[i], full, sizeof(full));
            if (isd[i]) {
                if (qt < ESCROW_MAX_DIRS) strlcpy(q[qt++], full, 256);
            } else if (c->n_before < ESCROW_MAX_FILES) {
                escrow_file_t *f = &c->before[c->n_before++];
                strlcpy(f->path, full, sizeof(f->path));
                f->size = (long)szs[i]; f->is_dir = 0; f->seen_after = 0;
            }
        }
    }
    free(q);
    free(names);
}

// ---------------------------------------------------------------------------
// Request: enter the KERNEL escrow contract (the enforcement authority).
// ---------------------------------------------------------------------------
escrow_contract_t *escrow_request(const char *scope_path,
                                  const char *promise_note, long ttl_secs) {
    if (!scope_path || !scope_path[0]) return 0;
    escrow_contract_t *c = &g_contract;
    memset(c, 0, sizeof(*c));
    c->used = 1;
    strlcpy(c->scope_path, scope_path, sizeof(c->scope_path));
    if (promise_note) strlcpy(c->note, promise_note, sizeof(c->note));
    c->ttl_secs = ttl_secs > 0 ? ttl_secs : ESCROW_TTL_DEFAULT;
    c->verdict = ESCROW_PARTIAL;
    snprintf(c->tag, sizeof(c->tag), "escrow_%d_%ld", g_escrow_seq++, esc_now() & 0xffff);

    // BEFORE snapshot for the promise oracle's delete counter. UNMARKED (reads).
    snap_recursive(c, scope_path);

    // Audit the request + the grant we are ABOUT to enter, BEFORE entering: once
    // marked, an out-of-scope /CONFIG audit write would itself be (correctly)
    // refused by the kernel guard, so all breadcrumbs are written outside the
    // marked window.
    aicap_audit("escrow.request", "escrow", scope_path, "requested",
                c->note[0] ? c->note : c->tag);
    aicap_audit("escrow.grant", "kernel:WRITE+MKDIR+MOVE;no-delete;device-scoped",
                scope_path, "entering", c->tag);

    // ENTER the kernel escrow contract. From here the KERNEL is the enforcement
    // authority: the grant is WRITE-only and scoped to scope_path (kernel refuses
    // any out-of-scope write and ANY delete, even to a raw uid-0 syscall), it is
    // TTL-bounded, it is bound to the backing device identity if scope_path is on
    // a removable volume (Stage 4), and every decision lands in the tamper-evident
    // journal (Stage 3). The userland aicap grant/deny is deliberately NOT minted:
    // there is exactly one source of truth now, and it is the kernel.
    unsigned long ttl_ms = (unsigned long)c->ttl_secs * 1000UL;
    int rc = escrow_enter(scope_path, ttl_ms);
    if (rc != 0) {
        // Fail closed: without the kernel contract there is no enforcement, so
        // there is no contract. Never silently degrade to an advisory run.
        char how[48];
        snprintf(how, sizeof(how), "kernel-enter-failed rc=%d", rc);
        aicap_audit("escrow.grant", "escrow", scope_path, "denied", how);
        c->used = 0;
        return 0;
    }
    c->active_grant = 1;   // the KERNEL escrow contract is live for this process
    return c;
}

// ---------------------------------------------------------------------------
// Execution primitives the organiser drives, run inside the marked window.
// These are ORDINARY FS syscalls: the KERNEL escrow guard (escrow_fs_guard) is
// what checks them (scope + no-delete + device) and records each decision in the
// tamper-evident journal, and sys_rename/sys_mkdir record the confirmed effect
// for SYS_ESCROW_ABORT rollback. There is NO userland enforcement or /CONFIG
// audit here; that would duplicate the kernel and would be refused mid-window.
// ---------------------------------------------------------------------------
int escrow_do_mkdir(escrow_contract_t *c, const char *path) {
    (void)c;
    if (!path || !path[0]) return -1;
    long r = syscall2(SYS_MKDIR, (long)path, 0755);
    // 0 = created; a non-zero here is typically "already exists", which the
    // promise oracle resolves for truth (escrow_verify checks is_dir).
    return (r == 0) ? 0 : (int)r;
}
int escrow_do_move(escrow_contract_t *c, const char *src, const char *dst) {
    (void)c;
    if (!src || !dst) return -1;
    return (syscall2(SYS_RENAME, (long)src, (long)dst) == 0) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Promise declaration (userland postcondition oracle; not an enforcement path).
// ---------------------------------------------------------------------------
int escrow_promise_add_folder(escrow_contract_t *c, const char *folder) {
    if (!c || !c->used || !folder || !folder[0]) return -1;
    for (int i = 0; i < c->n_folders; i++)
        if (!strcmp(c->folders[i].path, folder)) return 0;   // dedup
    if (c->n_folders >= ESCROW_MAX_FOLDERS) return -1;
    strlcpy(c->folders[c->n_folders].path, folder, sizeof(c->folders[0].path));
    c->n_folders++;
    return 0;
}

int escrow_promise_add_move(escrow_contract_t *c, const char *src, const char *dst) {
    if (!c || !c->used || !src || !src[0] || !dst || !dst[0]) return -1;
    for (int i = 0; i < c->n_moves; i++)
        if (!strcmp(c->moves[i].src, src)) return 0;         // dedup by source
    if (c->n_moves >= ESCROW_MAX_MOVES) return -1;
    escrow_move_t *m = &c->moves[c->n_moves];
    memset(m, 0, sizeof(*m));
    strlcpy(m->src, src, sizeof(m->src));
    strlcpy(m->dst, dst, sizeof(m->dst));
    if (esc_file_hash(src, &m->hash, &m->size) != 0) return -1;  // src unreadable
    c->n_moves++;
    return 0;
}

// ---------------------------------------------------------------------------
// Verify: the userland POSTCONDITION ORACLE. Reads only; it does not, and cannot,
// enforce anything. It reports whether the declared promise actually holds in the
// after-state. It writes NO /CONFIG audit (it runs inside the marked window); the
// verdict is audited by escrow_close() after the window closes.
// ---------------------------------------------------------------------------
static void add_unmet(escrow_verify_t *v, const char *fmt, const char *a) {
    if (v->n_unmet >= ESCROW_MAX_UNMET) return;
    snprintf(v->unmet[v->n_unmet], sizeof(v->unmet[0]), fmt, a);
    v->n_unmet++;
}

int escrow_verify(escrow_contract_t *c, escrow_verify_t *out) {
    if (!c || !c->used) return ESCROW_ERROR;
    escrow_verify_t v;
    memset(&v, 0, sizeof(v));
    int ok = 1;

    // (a) every promised date folder exists as a directory.
    v.folders_total = c->n_folders;
    for (int i = 0; i < c->n_folders; i++) {
        if (esc_is_dir(c->folders[i].path)) v.folders_ok++;
        else { add_unmet(&v, "date folder not created: %s", c->folders[i].path); ok = 0; }
    }

    // (b) every promised move landed bytes-intact with its source gone.
    v.moves_total = c->n_moves;
    for (int i = 0; i < c->n_moves; i++) {
        escrow_move_t *m = &c->moves[i];
        int good = 1;
        if (!esc_is_file(m->dst)) { add_unmet(&v, "photo not at destination: %s", m->dst); ok = 0; good = 0; }
        if (esc_exists(m->src))   { add_unmet(&v, "original still present: %s", m->src); ok = 0; good = 0; }
        if (good) {
            unsigned int dh = 0; long dsz = 0;
            if (esc_file_hash(m->dst, &dh, &dsz) != 0 || dsz != m->size || dh != m->hash) {
                add_unmet(&v, "moved photo bytes differ: %s", m->dst); ok = 0; good = 0;
            }
        }
        if (good) v.moves_ok++;
    }

    // (c) no pre-existing file vanished except as a tracked move. Every file in
    // the before-snapshot must be present after, either at its original path or
    // relocated to a promised move's destination that now exists.
    for (int i = 0; i < c->n_before; i++) {
        escrow_file_t *f = &c->before[i];
        if (esc_exists(f->path)) continue;                  // untouched, still there
        int tracked = 0;
        for (int j = 0; j < c->n_moves; j++) {
            if (!strcmp(c->moves[j].src, f->path) && esc_exists(c->moves[j].dst)) { tracked = 1; break; }
        }
        if (!tracked) {
            v.delete_count++; ok = 0;
            add_unmet(&v, "file vanished (not a tracked move): %s", f->path);
        }
    }
    if (v.delete_count != 0) ok = 0;

    v.verdict = ok ? ESCROW_FULFILLED : ESCROW_PARTIAL;
    c->verdict = v.verdict;
    // Stash the counts for escrow_close() to audit AFTER the marked window closes.
    snprintf(c->last_how, sizeof(c->last_how), "folders=%d/%d moves=%d/%d deletes=%d",
             v.folders_ok, v.folders_total, v.moves_ok, v.moves_total, v.delete_count);

    if (out) *out = v;
    return v.verdict;
}

// ---------------------------------------------------------------------------
// Close: shut the KERNEL escrow contract, THEN audit (unmarked again).
//   FULFILLED -> SYS_ESCROW_EXIT: revoke the grant EARLY (before the TTL).
//   NOT kept  -> SYS_ESCROW_ABORT: the KERNEL rolls back the recorded reversible
//                effects (moves back, empty dirs removed) then revokes + clears.
// This is docs/CONTRACT_ARCHITECTURE.md section 7's "on failure, a task
// auto-reverts", now wired to the userland promise verdict.
// ---------------------------------------------------------------------------
int escrow_close(escrow_contract_t *c) {
    if (!c || !c->used) return ESCROW_ERROR;

    if (c->verdict == ESCROW_FULFILLED) {
        // Promise kept: close the kernel contract early (grant revoked NOW).
        int rc = c->active_grant ? escrow_exit() : 0;   // SYS_ESCROW_EXIT
        c->active_grant = 0;
        // UNMARKED again: /CONFIG audit writes are permitted once more.
        aicap_audit("escrow.verify", "escrow", c->scope_path, "FULFILLED", c->last_how);
        char how[80];
        snprintf(how, sizeof(how), "closed-early:kernel-grant-revoked(rc=%d)", rc);
        aicap_audit("escrow.close", "escrow", c->scope_path, "fulfilled", how);
        c->used = 0;   // free the slot
        return ESCROW_CLOSED_EARLY;
    }

    // Promise NOT kept: ABORT so the KERNEL reverts the reversible effects and
    // revokes the grant. Fail-safe: never widen access on an unmet promise, and
    // never leave the reversible half-done work behind.
    int reverted = c->active_grant ? escrow_abort() : 0;   // SYS_ESCROW_ABORT
    c->active_grant = 0;
    if (reverted < 0) reverted = 0;
    aicap_audit("escrow.verify", "escrow", c->scope_path, "PARTIAL", c->last_how);
    char how[96];
    snprintf(how, sizeof(how),
             "aborted:kernel-rolled-back %d reversible-effect(s), grant revoked", reverted);
    aicap_audit("escrow.close", "escrow", c->scope_path, "partial", how);
    c->used = 0;   // the kernel contract is closed; free the slot
    return ESCROW_OPEN_TTL;
}
