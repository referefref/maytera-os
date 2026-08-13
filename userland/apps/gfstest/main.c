// gfstest - Ring-3 proof that the GraphFS journal's tamper detection goes RED
// (#711 slice 1).
//
// A check that has only ever been seen to pass is indistinguishable from no
// check. This app runs from Ring 3, on the real machine, against the real
// journal, and tries FOUR different attacks on it. Each must be caught, and the
// reason each is caught must be the RIGHT one.
//
//   1. TRUNCATE  - drop the last record. The chain is still internally
//                  consistent, so only the seal can catch this.
//   2. REORDER   - swap two records. Breaks the hash chain.
//   3. EDIT      - flip one byte inside a record. Breaks that record's own hash.
//   4. ROLLBACK  - restore an EARLIER, genuine (log, seal) pair that the kernel
//                  itself wrote. Both files are perfectly self-consistent and
//                  agree with each other, so neither the chain nor the seal can
//                  see anything wrong. Only the kernel's in-memory head anchor
//                  can, and Ring 3 cannot reach kernel memory.
//
// Attack 4 is the interesting one: it is the classic rollback that unkeyed hash
// chains cannot detect, and it needs no crypto in Ring 3 at all, because the
// attacker is replaying the defender's own valid output.
//
// Results go to /GFSTEST.OUT. The kernel independently prints a
// "[GFSJ] VERIFY FAILED: <reason> at seq N" line on serial for each detection,
// so the evidence exists in two places that cannot be forged by this app.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "syscall.h"

#define SYS_GFS_VERIFY 360

// Mirrors gfsj_verify_t in kernel/fs/graphfs/journal.h. 80 bytes; the kernel
// side is _Static_assert'd and the syscall pointer validator is told the same
// number, so a drift here cannot silently under-read.
typedef struct {
    unsigned int  status;
    unsigned int  reason;
    unsigned int  degraded;
    unsigned int  pad;
    unsigned long long bad_seq;
    unsigned long long count;
    unsigned long long seal_count;
    unsigned long long boot_gen;
    unsigned char head_hash[32];
} gfsj_verify_t;

#define R_CHAIN_BREAK     4
#define R_SEQ_GAP         5
#define R_HASH_MISMATCH   6
#define R_TRUNCATED       9
#define R_EXTENDED        10
#define R_HEAD_MISMATCH   11
#define R_ANCHOR_COUNT    12
#define R_ANCHOR_MISMATCH 13

#define REC 160
#define LOG_PATH  "/GRAPHFS/JOURNAL.LOG"
#define SEAL_PATH "/GRAPHFS/JOURNAL.SEAL"
#define OUT_PATH  "/GFSTEST.OUT"

static char g_out[16384];
static int  g_outlen = 0;
static int  g_pass = 0, g_fail = 0;

static void say(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    printf("%s", line);
    if (g_outlen + n < (int)sizeof(g_out)) {
        memcpy(g_out + g_outlen, line, n);
        g_outlen += n;
    }
    // ALSO put every line on the serial console, through SYS_BOOTLOG_WRITE.
    // MEASURED reason, not belt-and-braces: /GFSTEST.OUT is written by this
    // process at exit and a hard `qm stop` shortly afterwards loses it, so a
    // test whose only evidence is that file can silently produce no evidence at
    // all. The kernel stamps each line "[BOOTLOG] [USERSPACE uid=N]", so the
    // results are timestamped in the same stream as the kernel's own [GFSF]
    // lines and the two can be read against each other.
    {
        char one[200];
        int k = 0;
        for (int i = 0; i < n && k < (int)sizeof(one) - 1; i++)
            if (line[i] != '\n' && line[i] != '\r') one[k++] = line[i];
        one[k] = 0;
        if (k) syscall1(298 /*SYS_BOOTLOG_WRITE*/, (long)one);
    }
}

static const char *reason_name(unsigned int r) {
    switch (r) {
        case 0:  return "OK";
        case 1:  return "RAGGED";
        case 2:  return "BAD_MAGIC";
        case 3:  return "BAD_VERSION";
        case 4:  return "CHAIN_BREAK";
        case 5:  return "SEQ_GAP";
        case 6:  return "HASH_MISMATCH";
        case 7:  return "SEAL_MISSING";
        case 8:  return "SEAL_CORRUPT";
        case 9:  return "TRUNCATED";
        case 10: return "EXTENDED";
        case 11: return "HEAD_MISMATCH";
        case 12: return "ANCHOR_COUNT";
        case 13: return "ANCHOR_MISMATCH";
        case 14: return "ARG";
        case 15: return "NOT_READY";
        default: return "?";
    }
}

static int verify(gfsj_verify_t *v) {
    memset(v, 0, sizeof(*v));
    return (int)syscall1(SYS_GFS_VERIFY, (long)v);
}

// Whole-file read into a malloc'd buffer. Returns length, or -1.
static long slurp(const char *path, unsigned char **out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long cap = 1 << 20, len = 0;
    unsigned char *b = (unsigned char *)malloc(cap);
    if (!b) { close(fd); return -1; }
    for (;;) {
        int n = read(fd, b + len, 4096);
        if (n <= 0) break;
        len += n;
        if (len + 4096 > cap) break;
    }
    close(fd);
    *out = b;
    return len;
}

static int spew(const char *path, const unsigned char *data, long len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long off = 0;
    while (off < len) {
        int n = write(fd, data + off, (int)(len - off > 4096 ? 4096 : len - off));
        if (n <= 0) { close(fd); return -1; }
        off += n;
    }
    close(fd);
    return 0;
}

// Expect verification to FAIL, with one of two acceptable reasons.
static void expect_red(const char *what, unsigned int want, unsigned int want2) {
    gfsj_verify_t v;
    int rc = verify(&v);
    if (rc == 0) {
        say("FAIL  %-9s NOT DETECTED (verify still says OK, %llu records)\n",
            what, (unsigned long long)v.count);
        g_fail++;
        return;
    }
    if (v.reason != want && v.reason != want2) {
        say("FAIL  %-9s detected, but as %s(%u), expected %s(%u)\n",
            what, reason_name(v.reason), v.reason, reason_name(want), want);
        g_fail++;
        return;
    }
    say("PASS  %-9s RED: %s(%u) at seq %llu  [disk %llu, sealed %llu]\n",
        what, reason_name(v.reason), v.reason,
        (unsigned long long)v.bad_seq, (unsigned long long)v.count,
        (unsigned long long)v.seal_count);
    g_pass++;
}

// ===========================================================================
// SLICE 2: the fold (#711). Nodes, edges and contracts, seen from Ring 3
// through the READ-ONLY SYS_GFS_QUERY, plus the two boundary tests that matter:
// Ring 3 cannot mutate the graph, and Ring 3 cannot forge a contract by editing
// the journal FILE even though it can write that file freely.
// ===========================================================================
#define SYS_GFS_QUERY 363
#define Q_STATS      0
#define Q_NODES      1
#define Q_EDGES_OUT  2
#define Q_EDGES_IN   3
#define Q_CHECK      4

// Node ids are (kind << 24) | local: fs/graphfs/fold.h.
#define KIND_SYSTEM  1u
#define KIND_USER    2u
#define KIND_APP     3u
#define KIND_OBJECT  7u
#define NODE_ID(k, l)  (((unsigned)(k) << 24) | ((unsigned)(l) & 0x00FFFFFFu))
#define NODE_KERNEL   NODE_ID(KIND_SYSTEM, 1)
#define NODE_ESCROW   NODE_ID(KIND_SYSTEM, 2)
#define NODE_ST_APP   NODE_ID(KIND_APP, 1)
#define NODE_ST_OBJ   NODE_ID(KIND_OBJECT, 1)
#define SCOPE_SELFTEST 0x00FF

typedef struct {                       // mirrors gfs_stats_t, 128 bytes
    unsigned int ready, degraded, overflow;
    unsigned int nodes, edges, node_cap, edge_cap;
    unsigned int grants_live, grants_revoked, grants_expired;
    unsigned int grants_unlimited_scope, grants_never_expire;
    unsigned int refused_not_escrow, refused_bad_scope, refused_bad_expiry;
    unsigned int pad;
    unsigned long long boot_gen, records_seen, applied, rejected;
    unsigned long long checks, allows, denies, grants_prior_boot;
} gfs_stats_t;

typedef struct {                       // mirrors gfs_node_view_t, 32 bytes
    unsigned int node_id;
    unsigned short kind, state;
    unsigned int parent, pad;
    unsigned long long born_seq, born_boot;
} gfs_node_view_t;

typedef struct {                       // mirrors gfs_edge_view_t, 48 bytes
    unsigned long long edge_seq, revoked_seq, boot_gen;
    unsigned int expiry_ms, from, to;
    unsigned short rel, scope;
    unsigned int flags, pad;
} gfs_edge_view_t;

#define EDGEFLAG_EXPIRED 1
#define EDGEFLAG_REVOKED 2

static long gq(int cmd, unsigned a1, unsigned a2, void *out, int cap, unsigned a3) {
    return (long)syscall6(SYS_GFS_QUERY, (long)cmd, (long)a1, (long)a2,
                          (long)out, (long)cap, (long)a3);
}

static void ok(const char *what, int cond, const char *fmt_ok, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt_ok);
    vsnprintf(line, sizeof(line), fmt_ok, ap);
    va_end(ap);
    if (cond) { say("PASS  %-9s %s\n", what, line); g_pass++; }
    else      { say("FAIL  %-9s %s\n", what, line); g_fail++; }
}

static void slice2(const unsigned char *good_log, long good_len) {
    gfs_stats_t s0, s1;
    gfs_node_view_t nv[32];
    gfs_edge_view_t ev[32];

    say("-- slice 2: the fold (nodes, edges, contracts) --\n");

    // 1. The fold RAN. Not "the symbol is linked": it replayed records, it is
    //    ready, and it is not degraded.
    long n = gq(Q_STATS, 0, 0, &s0, (int)sizeof(s0), 0);
    if (n != (long)sizeof(s0)) {
        say("FAIL  fold-live SYS_GFS_QUERY(STATS) returned %ld, expected %d\n",
            n, (int)sizeof(s0));
        g_fail++;
        return;
    }
    ok("fold-live", s0.ready == 1 && s0.degraded == 0 && s0.applied > 0,
       "ready=%u degraded=%u overflow=%u, %llu record(s) folded, %u node(s), %u edge(s), boot gen %llu",
       s0.ready, s0.degraded, s0.overflow,
       (unsigned long long)s0.applied, s0.nodes, s0.edges,
       (unsigned long long)s0.boot_gen);

    // 2. The identities are real. The system nodes exist AND at least one USER
    //    node, which is a genuine account on this machine, not an invented one.
    int have_kernel = 0, have_escrow = 0, users = 0;
    long nb = gq(Q_NODES, 0, 0, nv, (int)sizeof(nv), 0);
    int nn = (int)(nb / (long)sizeof(nv[0]));
    for (int i = 0; i < nn; i++) {
        if (nv[i].node_id == NODE_KERNEL) have_kernel = 1;
        if (nv[i].node_id == NODE_ESCROW) have_escrow = 1;
        if (nv[i].kind == KIND_USER) users++;
    }
    ok("identities", have_kernel && have_escrow && users > 0,
       "%d node(s): kernel=%d escrow=%d, %d USER identit(ies) from the real account database",
       nn, have_kernel, have_escrow, users);

    // 3. The enforcement check both ALLOWED and DENIED this boot, and every
    //    refusal rule fired. These counters are the Ring-3-visible evidence that
    //    the kernel's boot self-test really exercised the path; a check only
    //    ever seen to pass is indistinguishable from a constant.
    ok("enforced", s0.checks > 0 && s0.allows > 0 && s0.denies > 0 &&
                   s0.refused_not_escrow > 0 && s0.refused_bad_scope > 0 &&
                   s0.refused_bad_expiry > 0,
       "%llu check(s): %llu allowed, %llu denied; refusals fired: not-escrow=%u bad-scope=%u bad-expiry=%u",
       (unsigned long long)s0.checks, (unsigned long long)s0.allows,
       (unsigned long long)s0.denies, s0.refused_not_escrow,
       s0.refused_bad_scope, s0.refused_bad_expiry);

    // 4. Q1 from Ring 3 DENIES: the boot self-test revoked its own grant, so the
    //    very next check must say no. This is the revocation property, observed
    //    from outside the kernel.
    ok("q1-revoked", gq(Q_CHECK, NODE_ST_APP, NODE_ST_OBJ, 0, 0, SCOPE_SELFTEST) == 0,
       "the self-test's revoked contract does not grant: check(app -> object, scope %u) = DENY",
       SCOPE_SELFTEST);

    // 5. Q1 denies for identities that do not exist at all.
    ok("q1-unknown", gq(Q_CHECK, 0x03BADBADu, 0x07BADBADu, 0, 0, 1) == 0,
       "check(unknown -> unknown) = DENY");

    // 6. Q3: the revoked contract is still VISIBLE, flagged revoked. Revocation
    //    removes authority, not evidence; an audit trail that forgets is not one.
    long eb = gq(Q_EDGES_IN, NODE_ST_OBJ, 0, ev, (int)sizeof(ev), 0);
    int ne = (int)(eb / (long)sizeof(ev[0])), revoked_seen = 0;
    for (int i = 0; i < ne; i++)
        if (ev[i].from == NODE_ST_APP && (ev[i].flags & EDGEFLAG_REVOKED)) revoked_seen = 1;
    ok("q3-visible", ne > 0 && revoked_seen,
       "%d edge(s) into the object, the revoked grant still listed with its revoking seq",
       ne);

    // 7. BOUNDARY: there is NO append syscall. Sweep every command id the
    //    read-only interface does not define and prove none of them mutates.
    for (int cmd = 5; cmd < 24; cmd++)
        (void)gq(cmd, NODE_ST_APP, NODE_ST_OBJ, nv, (int)sizeof(nv), 1);
    gq(Q_STATS, 0, 0, &s1, (int)sizeof(s1), 0);
    ok("no-append", s1.nodes == s0.nodes && s1.edges == s0.edges &&
                    s1.applied == s0.applied,
       "19 undefined command ids changed nothing: %u node(s), %u edge(s), %llu record(s) folded",
       s1.nodes, s1.edges, (unsigned long long)s1.applied);

    // 8. THE BOUNDARY THAT MATTERS. Ring 3 can write /GRAPHFS/JOURNAL.LOG: it is
    //    an ordinary ext2 file. So forge an EDGE_ADD GRANT record in it, from the
    //    escrow, giving this app's identity an unlimited contract, and append it.
    //    If the graph were read from the file, this would be a total bypass of
    //    the contract model. It is not: the fold is memory the kernel builds from
    //    records IT appended, so the forgery grants nothing, and the tamper is
    //    detected as tampering.
    if (good_len >= REC) {
        unsigned char *forged = (unsigned char *)malloc(good_len + REC);
        memcpy(forged, good_log, good_len);
        unsigned char *r = forged + good_len;
        memcpy(r, good_log, REC);                       // start from a real record
        *(unsigned long long *)(r + 8)  = (unsigned long long)(good_len / REC); // seq
        *(unsigned int *)(r + 32) = 2;                  // actor_kind = NODE
        *(unsigned int *)(r + 36) = NODE_ESCROW;        // actor      = the escrow
        *(unsigned short *)(r + 40) = 18;               // op         = EDGE_ADD
        *(unsigned int *)(r + 44) = 16;                 // payload_len
        *(unsigned int *)(r + 112) = NODE_ST_APP;       // from
        *(unsigned int *)(r + 116) = NODE_ST_OBJ;       // to
        *(unsigned int *)(r + 120) = 0xFFFFFFFFu;       // expiry: never
        *(unsigned short *)(r + 124) = 1;               // rel = GRANT
        *(unsigned short *)(r + 126) = 0xFFFF;          // scope = ANY
        spew(LOG_PATH, forged, good_len + REC);

        int granted = (int)gq(Q_CHECK, NODE_ST_APP, NODE_ST_OBJ, 0, 0, SCOPE_SELFTEST);
        int granted_any = (int)gq(Q_CHECK, NODE_ST_APP, NODE_ST_OBJ, 0, 0, 1);
        gfsj_verify_t vv;
        int vrc = verify(&vv);
        ok("forge-grant", granted == 0 && granted_any == 0,
           "a forged escrow GRANT written straight into the journal file grants NOTHING (check=%d/%d)",
           granted, granted_any);
        ok("forge-seen", vrc != 0,
           "and the forgery is detected: %s(%u) at seq %llu",
           reason_name(vv.reason), vv.reason, (unsigned long long)vv.bad_seq);

        spew(LOG_PATH, good_log, good_len);
        free(forged);
    }

    // 9. Cross-reboot: on the second and later boots the fold must have EXPIRED
    //    every grant from a previous boot generation, because this kernel has no
    //    trusted clock and an expiry it cannot evaluate must fail closed.
    if (s0.boot_gen > 1) {
        ok("prior-boot", s0.grants_prior_boot > 0,
           "boot generation %llu: %llu grant(s) from earlier boots expired by the fold (no trusted clock)",
           (unsigned long long)s0.boot_gen, (unsigned long long)s0.grants_prior_boot);
    } else {
        say("INFO  prior-boot first boot generation; the cross-reboot expiry test needs boot 2\n");
    }
}

int main(void) {
    gfsj_verify_t v;

    say("== gfstest: GraphFS journal tamper detection, from Ring 3 (#711) ==\n");

    // ---- 0. baseline -------------------------------------------------------
    int rc = verify(&v);
    if (rc != 0) {
        say("ABORT the journal does not verify before we touch it: %s(%u) at seq %llu\n",
            reason_name(v.reason), v.reason, (unsigned long long)v.bad_seq);
        spew(OUT_PATH, (const unsigned char *)g_out, g_outlen);
        return 1;
    }
    say("PASS  baseline  OK: %llu records, boot generation %llu, degraded=%u\n",
        (unsigned long long)v.count, (unsigned long long)v.boot_gen, v.degraded);
    g_pass++;
    unsigned long long base_count = v.count;

    // ---- 1. snapshot the genuine files, for attack 4 and for the restore ---
    unsigned char *log0 = 0, *seal0 = 0;
    long log0n = slurp(LOG_PATH, &log0);
    long seal0n = slurp(SEAL_PATH, &seal0);
    if (log0n <= 0 || seal0n <= 0) {
        say("ABORT cannot read the journal files (%ld, %ld)\n", log0n, seal0n);
        spew(OUT_PATH, (const unsigned char *)g_out, g_outlen);
        return 1;
    }
    say("INFO  snapshot: log %ld bytes (%ld records), seal %ld bytes\n",
        log0n, log0n / REC, seal0n);

    // ---- 2. make the kernel append, from Ring 3 ---------------------------
    // A syscall carrying a kernel-space pointer is rejected by the #500
    // validator, which raises AUDIT_PTR_INVALID; the seclog worker drains that
    // and appends a journal record. So this Ring-3 call, and nothing else,
    // grows the tamper-evident journal.
    (void)syscall2(238 /*SYS_PROC_LIST*/, (long)0x100000L, 1L);
    (void)syscall2(238, (long)0x100000L, 1L);

    unsigned long long grown = base_count;
    for (int i = 0; i < 40 && grown == base_count; i++) {
        usleep(100000);
        if (verify(&v) == 0) grown = v.count;
    }
    if (grown > base_count) {
        say("PASS  producer  a Ring-3 syscall grew the journal: %llu -> %llu records\n",
            base_count, grown);
        g_pass++;
    } else {
        say("FAIL  producer  journal did not grow after a rejected user pointer "
            "(still %llu records)\n", base_count);
        g_fail++;
    }

    // Re-snapshot the CURRENT genuine state so we can restore it at the end.
    unsigned char *log1 = 0, *seal1 = 0;
    long log1n = slurp(LOG_PATH, &log1);
    long seal1n = slurp(SEAL_PATH, &seal1);
    if (log1n <= 0 || seal1n <= 0) {
        say("ABORT lost the journal files mid-test\n");
        spew(OUT_PATH, (const unsigned char *)g_out, g_outlen);
        return 1;
    }

    // ---- attack 1: TRUNCATE -----------------------------------------------
    spew(LOG_PATH, log1, log1n - REC);
    expect_red("truncate", R_TRUNCATED, R_TRUNCATED);
    spew(LOG_PATH, log1, log1n);

    // ---- attack 2: REORDER -------------------------------------------------
    if (log1n >= 2 * REC) {
        unsigned char *swapped = (unsigned char *)malloc(log1n);
        memcpy(swapped, log1, log1n);
        memcpy(swapped, log1 + REC, REC);
        memcpy(swapped + REC, log1, REC);
        spew(LOG_PATH, swapped, log1n);
        expect_red("reorder", R_CHAIN_BREAK, R_SEQ_GAP);
        free(swapped);
        spew(LOG_PATH, log1, log1n);
    } else {
        say("SKIP  reorder   needs at least 2 records, have %ld\n", log1n / REC);
    }

    // ---- attack 3: EDIT ----------------------------------------------------
    {
        unsigned char *edited = (unsigned char *)malloc(log1n);
        memcpy(edited, log1, log1n);
        edited[112] ^= 0xFF;          // one byte of record 0's inline payload
        spew(LOG_PATH, edited, log1n);
        expect_red("edit", R_HASH_MISMATCH, R_HASH_MISMATCH);
        free(edited);
        spew(LOG_PATH, log1, log1n);
    }

    // ---- attack 4: ROLLBACK (the adaptive one) ----------------------------
    // Restore the kernel's OWN earlier output: log0 + seal0. Self-consistent,
    // mutually consistent, genuinely signed by nothing at all. The chain cannot
    // object and the seal cannot object. The in-kernel anchor can.
    spew(LOG_PATH, log0, log0n);
    spew(SEAL_PATH, seal0, seal0n);
    expect_red("rollback", R_ANCHOR_COUNT, R_ANCHOR_MISMATCH);

    // ---- restore and re-verify --------------------------------------------
    spew(LOG_PATH, log1, log1n);
    spew(SEAL_PATH, seal1, seal1n);
    if (verify(&v) == 0) {
        say("PASS  restore   journal verifies again: %llu records\n",
            (unsigned long long)v.count);
        g_pass++;
    } else {
        say("FAIL  restore   journal does NOT verify after restore: %s(%u) at seq %llu\n",
            reason_name(v.reason), v.reason, (unsigned long long)v.bad_seq);
        g_fail++;
    }

    // ---- SLICE 2: the fold, the contract graph, and its boundary ----------
    slice2(log1, log1n);

    say("== gfstest: %d passed, %d failed ==\n", g_pass, g_fail);
    spew(OUT_PATH, (const unsigned char *)g_out, g_outlen);
    free(log0); free(seal0); free(log1); free(seal1);
    return g_fail ? 1 : 0;
}
