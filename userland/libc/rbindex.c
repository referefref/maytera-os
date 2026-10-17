// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// rbindex.c - the one implementation of the Recycle Bin's location, its index
// row format, and its retention rule. See rbindex.h for WHY this is shared
// rather than copied into each app.
//
// TWO HALVES, ONE FILE. Everything above the RBINDEX_PURE_ONLY guard is pure:
// no syscalls, no libc, no allocation, nothing but bytes in and bytes out.
// That is what lets tools/trash-harness/rbindex-selftest.sh compile this exact
// source with a host compiler and mutate it to prove the suite can go red.
// Below the guard is the I/O, which needs the real userland.
#include "rbindex.h"

// ===========================================================================
// PURE: the row format
// ===========================================================================

static int rb_is_digit(char c) { return c >= '0' && c <= '9'; }

static void rb_put(char *dst, int cap, const char *src, int n) {
    int i = 0;
    if (cap <= 0) return;
    while (i < n && i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int rbindex_actor_is_ai(const char *actor) {
    return actor && actor[0] == 'a' && actor[1] == 'i' && actor[2] == ':';
}

int rbindex_parse_row(const char *row, int len, rb_row_t *out) {
    int i, f[4], e[4], nf = 0, s;
    if (!row || !out || len <= 0) return 0;

    out->name[0] = 0;
    out->original_path[0] = 0;
    out->deleted_unix = 0;
    out->actor[0] = 'u'; out->actor[1] = 's'; out->actor[2] = 'e';
    out->actor[3] = 'r'; out->actor[4] = 0;

    // A trailing CR is normal on a file that has been through a text editor.
    while (len > 0 && (row[len - 1] == '\r' || row[len - 1] == '\n')) len--;
    if (len <= 0) return 0;

    // Split into at most four '|'-separated fields. THE PATH STOPS AT THE NEXT
    // '|', which is the whole point: the V1 readers ran to end of line, so
    // appending fields to a row would have corrupted every restore.
    s = 0;
    for (i = 0; i <= len && nf < 4; i++) {
        if (i == len || row[i] == '|') {
            f[nf] = s; e[nf] = i; nf++;
            s = i + 1;
            if (i == len) break;
        }
    }
    if (nf < 2) return 0;                       // no '|' at all: not a row
    if (e[0] <= f[0]) return 0;                 // empty name: not a row

    rb_put(out->name, (int)sizeof(out->name), row + f[0], e[0] - f[0]);
    rb_put(out->original_path, (int)sizeof(out->original_path),
           row + f[1], e[1] - f[1]);

    if (nf >= 3 && e[2] > f[2]) {
        unsigned long acc = 0;
        int ok = 1;
        for (i = f[2]; i < e[2]; i++) {
            if (!rb_is_digit(row[i])) { ok = 0; break; }
            acc = acc * 10u + (unsigned long)(row[i] - '0');
        }
        // A non-numeric third field leaves deleted_unix 0, i.e. AGE UNKNOWN,
        // i.e. never purged. Every parse failure lands on keeping the file.
        out->deleted_unix = ok ? acc : 0;
    }
    if (nf >= 4 && e[3] > f[3])
        rb_put(out->actor, (int)sizeof(out->actor), row + f[3], e[3] - f[3]);
    return 1;
}

int rbindex_row_build(const rb_row_t *r, char *out, int cap) {
    int n = 0, i;
    const char *actor;
    unsigned long v;
    char tmp[24];
    int t = 0;

    if (!r || !out || cap <= 0) return 0;

    for (i = 0; r->name[i]; i++) {
        char c = r->name[i];
        if (c == '|' || c == '\n' || c == '\r') continue;
        if (n >= cap) return 0;
        out[n++] = c;
    }
    if (n == 0 || n >= cap) return 0;
    out[n++] = '|';
    for (i = 0; r->original_path[i]; i++) {
        char c = r->original_path[i];
        if (c == '|' || c == '\n' || c == '\r') continue;
        if (n >= cap) return 0;
        out[n++] = c;
    }
    if (n >= cap) return 0;
    out[n++] = '|';

    v = r->deleted_unix;
    if (v == 0) { tmp[t++] = '0'; }
    else { while (v > 0 && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + (v % 10u)); v /= 10u; } }
    if (n + t > cap) return 0;
    while (t > 0) out[n++] = tmp[--t];

    if (n >= cap) return 0;
    out[n++] = '|';

    actor = (r->actor[0]) ? r->actor : "user";
    for (i = 0; actor[i]; i++) {
        char c = actor[i];
        if (c == '|' || c == '\n' || c == '\r') continue;
        if (n >= cap) return 0;
        out[n++] = c;
    }
    if (n >= cap) return 0;
    out[n++] = '\n';
    return n;
}

// ===========================================================================
// PURE: retention. Byte-for-byte the format kernel/rustkern/recycle.rs reads
// and writes (rcy_parse_retention_rs / rcy_format_retention_rs).
// ===========================================================================

#define RB_SECS_PER_DAY 86400UL

int rbindex_is_expired(unsigned long deleted_unix, unsigned long now_unix,
                       unsigned retention_days) {
    unsigned long age, limit;
    if (retention_days == RB_RETENTION_NEVER) return 0;  // never means never
    if (deleted_unix == 0) return 0;                     // age unknown: keep
    if (now_unix <= deleted_unix) return 0;              // clock went backwards
    age = now_unix - deleted_unix;
    limit = (unsigned long)retention_days * RB_SECS_PER_DAY;
    return (age >= limit) ? 1 : 0;
}

unsigned rbindex_parse_retention(const char *b, int len) {
    int i = 0;
    static const char key[] = "retention_days";
    const int keylen = 14;   // sizeof(key) - 1

    if (!b || len <= 0) return RB_RETENTION_DEFAULT_DAYS;

    while (i < len) {
        int start = i, e = i, s, end, k, m, v;
        while (e < len && b[e] != '\n') e++;
        i = (e < len) ? e + 1 : e;

        s = start; end = e;
        while (s < end && (b[s] == ' ' || b[s] == '\t')) s++;
        while (end > s && (b[end - 1] == ' ' || b[end - 1] == '\t' || b[end - 1] == '\r')) end--;
        if (s >= end || b[s] == '#') continue;
        if (end - s <= keylen) continue;

        m = 1;
        for (k = 0; k < keylen; k++) if (b[s + k] != key[k]) { m = 0; break; }
        if (!m) continue;

        // Whitespace is allowed on BOTH sides of the '='.
        //
        // MEASURED DIVERGENCE, 2026-10-03, reported to the kernel side rather
        // than copied: kernel/rustkern/recycle.rs's rcy_parse_retention_rs()
        // tests `b[s + key.len()] != b'='` with no space skip, so it reads
        // "retention_days = 7" as no setting at all and returns the default,
        // while its OWN self-test (the `w` case, chk! code -18) asserts that
        // exact input parses as 7. One of the two is wrong and the self-test
        // says which one was intended. This implementation does what the
        // kernel's test asserts, which is also the forgiving direction for a
        // hand-edited config: "retention_days = 7" means seven days, not
        // silently thirty.
        v = s + keylen;
        while (v < end && (b[v] == ' ' || b[v] == '\t')) v++;
        if (v >= end || b[v] != '=') continue;
        v++;
        while (v < end && (b[v] == ' ' || b[v] == '\t')) v++;
        if (v >= end) return RB_RETENTION_DEFAULT_DAYS;

        // "never", case-insensitive, is the explicit off switch.
        if (end - v == 5) {
            static const char nv[] = "never";
            int n = 1;
            for (k = 0; k < 5; k++) if ((char)(b[v + k] | 0x20) != nv[k]) { n = 0; break; }
            if (n) return RB_RETENTION_NEVER;
        }
        {
            unsigned long acc = 0;
            int digits = 0, j;
            for (j = v; j < end; j++) {
                if (!rb_is_digit(b[j])) { digits = 0; break; }
                acc = acc * 10u + (unsigned long)(b[j] - '0');
                if (acc > 0xFFFFFFFUL) acc = 0xFFFFFFFUL;
                digits++;
            }
            if (digits == 0) return RB_RETENTION_DEFAULT_DAYS;   // trailing junk
            if (acc == 0) return RB_RETENTION_NEVER;             // 0 == never
            if (acc > RB_RETENTION_MAX_DAYS) acc = RB_RETENTION_MAX_DAYS;
            return (unsigned)acc;
        }
    }
    return RB_RETENTION_DEFAULT_DAYS;
}

int rbindex_format_retention(unsigned days, char *out, int cap) {
    static const char hdr[] =
        "# MayteraOS Recycle Bin retention. 'never' or a number of days.\n";
    static const char kv[] = "retention_days=";
    int n = 0, i;
    if (!out || cap <= 0) return 0;
    for (i = 0; hdr[i]; i++) { if (n >= cap) return 0; out[n++] = hdr[i]; }
    for (i = 0; kv[i]; i++)  { if (n >= cap) return 0; out[n++] = kv[i]; }
    if (days == RB_RETENTION_NEVER) {
        static const char nv[] = "never";
        for (i = 0; nv[i]; i++) { if (n >= cap) return 0; out[n++] = nv[i]; }
    } else {
        char tmp[12]; int t = 0; unsigned d = days;
        while (d > 0 && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + (d % 10u)); d /= 10u; }
        if (n + t > cap) return 0;
        while (t > 0) out[n++] = tmp[--t];
    }
    if (n >= cap) return 0;
    out[n++] = '\n';
    return n;
}

#ifndef RBINDEX_PURE_ONLY
// ===========================================================================
// I/O
// ===========================================================================
#include "stdlib.h"     // open(), close()
#include "unistd.h"     // read(), write(), fsync(), unlink(), SEEK_END
#include "syscall.h"    // sys_seek()
#include "userconf.h"   // the per-user path rule (#683/#peruser2)

#define RB_O_WRONLY_CREAT 0x41

static int rb_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int  g_use_session;
static char g_dir[256];
static char g_idx[256];
static char g_cfg[256];
static int  g_dir_r, g_idx_r, g_cfg_r;

void rbindex_use_session_home(void) { g_use_session = 1; }

static int rb_resolve(const char *sub, const char *name, char *out, unsigned long cap) {
    if (g_use_session) return userhome_path_session(sub, name, out, cap);
    return userhome_path(sub, name, out, cap);
}

const char *rbindex_dir(void) {
    if (!g_dir_r) {
        if (rb_resolve("CONFIG", "RECYCLE", g_dir, sizeof(g_dir)) != 0) g_dir[0] = 0;
        g_dir_r = 1;
    }
    return g_dir[0] ? g_dir : "/CONFIG/RECYCLE";
}
const char *rbindex_path(void) {
    if (!g_idx_r) {
        if (rb_resolve("CONFIG", "RBINDEX.TXT", g_idx, sizeof(g_idx)) != 0) g_idx[0] = 0;
        g_idx_r = 1;
    }
    return g_idx[0] ? g_idx : "/CONFIG/RBINDEX.TXT";
}
const char *rbindex_cfg_path(void) {
    if (!g_cfg_r) {
        if (rb_resolve("CONFIG", "RECYCLE.CFG", g_cfg, sizeof(g_cfg)) != 0) g_cfg[0] = 0;
        g_cfg_r = 1;
    }
    return g_cfg[0] ? g_cfg : "/CONFIG/RECYCLE.CFG";
}

void rbindex_join(char *out, int outsz, const char *dir, const char *name) {
    int j = 0, i;
    if (!out || outsz <= 0) return;
    for (i = 0; dir && dir[i] && j < outsz - 1; i++) out[j++] = dir[i];
    if (j > 0 && out[j - 1] != '/' && j < outsz - 1) out[j++] = '/';
    for (i = 0; name && name[i] && j < outsz - 1; i++) out[j++] = name[i];
    out[j] = 0;
}

// The raw index text. Deliberately NOT a parsed rb_row_t[]: 256 parsed rows
// would be 70 KB of .bss in every app that touches the bin, and these apps
// walk the index linearly anyway. rbindex_at() parses on demand into one
// shared scratch row.
//
// RB_INDEX_MAX MUST MATCH THE KERNEL'S WRITER. kernel/fs/recycle_fs.c caps
// RBINDEX.TXT at 65536 bytes (RCY_INDEX_MAX) and refuses a capture past that,
// so the kernel can legitimately hand us a 64 KiB index. The readers this file
// replaced carried idx_buf[8192], which would have silently stopped seeing
// entries at roughly the fiftieth: not an error, just a bin that quietly
// forgets the rest of its contents, which then look like files with no row and
// become unrestorable in the UI. A reader narrower than its writer is a data
// loss bug with no error path, so the two numbers are now the same number.
#define RB_INDEX_MAX 65536
static char g_buf[RB_INDEX_MAX + 1];
static int  g_len;
static int  g_loaded;
static rb_row_t g_scratch;
// Scratch for the rewrite in rbindex_remove(). STATIC, not a local: a 64 KiB
// stack array in an app whose thread stack is 16 KiB is a stack overflow, and
// this code runs in Files and the Recycle Bin, not only in the daemon.
static char g_rewrite[RB_INDEX_MAX + 1];

int rbindex_load(void) {
    int fd;
    g_len = 0; g_buf[0] = 0; g_loaded = 1;
    fd = open(rbindex_path(), 0);
    if (fd < 0) return 0;
    for (;;) {
        char tmp[512];
        long n = read(fd, tmp, sizeof(tmp));
        int i;
        if (n <= 0) break;
        for (i = 0; i < (int)n && g_len < (int)sizeof(g_buf) - 1; i++) g_buf[g_len++] = tmp[i];
    }
    g_buf[g_len] = 0;
    close(fd);
    return rbindex_count();
}

// Walk to line `want` (0-based). Returns its [start,end) or 0 if there is no
// such line. A blank line is skipped rather than counted, so an index that has
// grown a stray newline does not produce a phantom row.
static int rb_line(int want, int *ls, int *le) {
    int i = 0, n = 0;
    while (i < g_len) {
        int s = i, e;
        while (i < g_len && g_buf[i] != '\n') i++;
        e = i;
        if (i < g_len) i++;
        if (e <= s) continue;
        if (n == want) { *ls = s; *le = e; return 1; }
        n++;
    }
    return 0;
}

int rbindex_count(void) {
    int i = 0, n = 0;
    if (!g_loaded) rbindex_load();
    while (i < g_len) {
        int s = i, e;
        while (i < g_len && g_buf[i] != '\n') i++;
        e = i;
        if (i < g_len) i++;
        if (e > s) n++;
    }
    return n;
}

const rb_row_t *rbindex_at(int idx) {
    int s, e;
    if (!g_loaded) rbindex_load();
    if (idx < 0 || !rb_line(idx, &s, &e)) return 0;
    if (!rbindex_parse_row(g_buf + s, e - s, &g_scratch)) return 0;
    return &g_scratch;
}

int rbindex_lookup(const char *name, rb_row_t *out) {
    int i = 0;
    if (!name || !out) return 0;
    if (!g_loaded) rbindex_load();
    while (i < g_len) {
        int s = i, e;
        while (i < g_len && g_buf[i] != '\n') i++;
        e = i;
        if (i < g_len) i++;
        if (e <= s) continue;
        if (!rbindex_parse_row(g_buf + s, e - s, out)) continue;
        if (rb_eq(out->name, name)) return 1;
    }
    out->name[0] = 0;
    return 0;
}

// Find the NEWEST row whose original_path is `path`. "Newest" is by recorded
// deletion time, with a later row in the file winning a tie, because that is
// the order rows are appended in and it is the only ordering the file carries.
// A row with an UNKNOWN age (deleted_unix 0) is the weakest candidate, taken
// only when nothing dated matches: picking it over a dated row would mean
// restoring the older copy of a file that has been deleted twice.
int rbindex_find_by_original(const char *path, rb_row_t *out) {
    int i = 0, found = 0;
    rb_row_t cur;
    if (!path || !out) return 0;
    if (!g_loaded) rbindex_load();
    while (i < g_len) {
        int s = i, e;
        while (i < g_len && g_buf[i] != '\n') i++;
        e = i;
        if (i < g_len) i++;
        if (e <= s) continue;
        if (!rbindex_parse_row(g_buf + s, e - s, &cur)) continue;
        if (!rb_eq(cur.original_path, path)) continue;
        if (!found || cur.deleted_unix >= out->deleted_unix) { *out = cur; found = 1; }
    }
    return found;
}

// The AI task node id out of an "ai:XXXXXXXX" actor token, or 0. 0 is also what
// a user row gives, which is correct: there is no task behind a human delete.
unsigned long rbindex_actor_id(const char *actor) {
    unsigned long v = 0;
    int i;
    if (!rbindex_actor_is_ai(actor)) return 0;
    for (i = 3; actor[i]; i++) {
        char c = actor[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;                 // a malformed id is no id, not a partial one
        v = (v << 4) | (unsigned long)d;
    }
    return v;
}

// Copy src to dst, durably. Returns 0 ONLY if every byte is on the medium: the
// caller unlinks the SOURCE on a 0, so an unchecked close() here is a delete of
// the only copy. On a full volume the write can truncate the destination before
// it discovers there is no room and leave a valid ZERO-BYTE file behind.
static int rb_copy_file(const char *src, const char *dst) {
    int in = open(src, 0);
    int out, rc = 0;
    long n = 0;
    if (in < 0) return -1;
    out = open(dst, 0x241);            // O_WRONLY|O_CREAT|O_TRUNC
    if (out < 0) { close(in); return -1; }
    for (;;) {
        char b[4096];
        long w = 0;
        n = read(in, b, sizeof(b));
        if (n <= 0) break;
        while (w < n) {
            long k = write(out, b + w, (unsigned long)(n - w));
            if (k <= 0) { rc = -1; break; }
            w += k;
        }
        if (rc) break;
    }
    if (n < 0) rc = -1;
    if (rc == 0 && fsync(out) != 0) rc = -1;
    if (close(out) != 0) rc = -1;
    close(in);
    return rc;
}

int rbindex_restore(const rb_row_t *e, int flags) {
    char src[320];
    if (!e || !e->name[0]) return -1;
    // A row with no recorded origin cannot be restored anywhere. Refuse rather
    // than invent a destination.
    if (!e->original_path[0] || rb_eq(e->original_path, "(unknown)")) return -1;

    if (flags & RB_RESTORE_NOCLOBBER) {
        int fd = open(e->original_path, 0);
        if (fd >= 0) { close(fd); return -2; }   // something is already there
    }
    rbindex_join(src, (int)sizeof(src), rbindex_dir(), e->name);

    // (#239) ext2 rename() is unreliable, so fall back to copy+unlink. The copy
    // is fsync'd before the bin copy goes, so there is never a moment with no
    // durable copy of the file.
    if (rename(src, e->original_path) != 0) {
        if (rb_copy_file(src, e->original_path) != 0) return -1;
        if (unlink(src) != 0) return -1;         // the restored copy IS real; say so
    }
    // #742: the row goes LAST, and only if it can be dropped durably. A restore
    // whose row removal fails leaves the entry listed in the bin pointing at a
    // file that is now back where it belongs, which the next sweep reports as an
    // orphan row. The opposite order would drop the only record of the move.
    if (rbindex_remove(e->name) != 0) return -3;
    return 0;
}

int rbindex_append(const char *name, const char *orig,
                   unsigned long deleted_unix, const char *actor) {
    rb_row_t r;
    char line[RB_ROW_MAX];
    int l, fd, rc = 0;

    if (!name || !orig) return -1;
    r.deleted_unix = deleted_unix;
    rb_put(r.name, (int)sizeof(r.name), name, (int)sizeof(r.name));
    rb_put(r.original_path, (int)sizeof(r.original_path), orig, (int)sizeof(r.original_path));
    rb_put(r.actor, (int)sizeof(r.actor), (actor && actor[0]) ? actor : "user",
           (int)sizeof(r.actor));

    l = rbindex_row_build(&r, line, (int)sizeof(line));
    if (l <= 0) return -1;

    // An APPEND, so NOT the O_TRUNC opener. The parent still has to exist on a
    // home that predates <home>/CONFIG.
    userconf_ensure_parent_dir(rbindex_path());
    fd = open(rbindex_path(), RB_O_WRONLY_CREAT);
    if (fd < 0) return -1;
    if (sys_seek(fd, 0, SEEK_END) < 0) rc = -1;
    if (rc == 0 && write(fd, line, (unsigned long)l) != l) rc = -1;
    if (rc == 0 && fsync(fd) != 0) rc = -1;          // #742: unflushed is not written
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && g_loaded) {
        int k;
        for (k = 0; k < l && g_len < (int)sizeof(g_buf) - 1; k++) g_buf[g_len++] = line[k];
        g_buf[g_len] = 0;
    }
    return rc;
}

int rbindex_remove(const char *name) {
    char *nb = g_rewrite;
    int nl = 0, i = 0, fd, rc = 0, j;
    const int nbcap = (int)sizeof(g_rewrite);

    if (!name) return -1;
    if (!g_loaded) rbindex_load();

    while (i < g_len) {
        int s = i, e, keep = 1;
        rb_row_t r;
        while (i < g_len && g_buf[i] != '\n') i++;
        e = i;
        if (i < g_len) i++;
        if (e <= s) continue;
        if (rbindex_parse_row(g_buf + s, e - s, &r) && rb_eq(r.name, name)) keep = 0;
        if (keep) {
            for (j = s; j < e && nl < nbcap - 1; j++) nb[nl++] = g_buf[j];
            if (nl < nbcap - 1) nb[nl++] = '\n';
        }
    }

    // #peruser2: userconf_open_write_path() is open(0x241) plus the two things
    // a per-user destination needs: it creates <home>/CONFIG, and it reports a
    // refusal through sys_bootlog instead of returning -1 into silence.
    // #742: O_TRUNC, never unlink-then-create. There is no window in which the
    // index does not exist.
    fd = userconf_open_write_path(rbindex_path());
    if (fd < 0) return -1;
    if (nl && write(fd, nb, (unsigned long)nl) != nl) rc = -1;
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc != 0) return rc;                 // leave RAM matching what is on disk
    for (j = 0; j < nl; j++) g_buf[j] = nb[j];
    g_len = nl; g_buf[nl] = 0;
    return 0;
}

int rbindex_clear(void) {
    int fd = userconf_open_write_path(rbindex_path());
    int rc = (fd < 0) ? -1 : 0;
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (fd >= 0 && close(fd) != 0) rc = -1;
    if (rc == 0) { g_len = 0; g_buf[0] = 0; g_loaded = 1; }
    return rc;
}

// ---------------------------------------------------------------------------
// The retention setting on disk
// ---------------------------------------------------------------------------

const unsigned rb_retention_choices[7] = { RB_RETENTION_NEVER, 1, 7, 14, 30, 90, 365 };

int rbindex_retention_choice_index(unsigned days) {
    int i;
    for (i = 0; i < 7; i++) if (rb_retention_choices[i] == days) return i;
    // An out-of-list value from a hand-edited RECYCLE.CFG is HONOURED by the
    // sweeper; the control simply shows the nearest offered value at or below
    // it, so opening Settings does not silently rewrite the user's choice
    // until they actually touch the control.
    if (days == RB_RETENTION_NEVER) return 0;
    for (i = 6; i >= 1; i--) if (rb_retention_choices[i] <= days) return i;
    return 4;   // the default, 30 days
}

const char *rbindex_retention_label(unsigned days) {
    switch (days) {
    case RB_RETENTION_NEVER: return "Never";
    case 1:   return "1 day";
    case 7:   return "7 days";
    case 14:  return "14 days";
    case 30:  return "30 days";
    case 90:  return "90 days";
    case 365: return "365 days";
    default:  return "Custom";
    }
}

unsigned rbindex_read_retention(void) {
    char buf[512];
    int n = 0, fd = open(rbindex_cfg_path(), 0);
    if (fd < 0) return RB_RETENTION_DEFAULT_DAYS;
    for (;;) {
        long r = read(fd, buf + n, sizeof(buf) - (unsigned long)n);
        if (r <= 0) break;
        n += (int)r;
        if (n >= (int)sizeof(buf)) break;
    }
    close(fd);
    return rbindex_parse_retention(buf, n);
}

int rbindex_write_retention(unsigned days) {
    char body[128];
    int n = rbindex_format_retention(days, body, (int)sizeof(body));
    int fd, rc = 0;
    if (n <= 0) return -1;
    fd = userconf_open_write_path(rbindex_cfg_path());
    if (fd < 0) return -1;
    if (write(fd, body, (unsigned long)n) != n) rc = -1;
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    return rc;
}
#endif // RBINDEX_PURE_ONLY
