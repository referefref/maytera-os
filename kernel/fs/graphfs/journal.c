// journal.c - GraphFS tamper-evident journal, I/O glue (#711 slice 1).
//
// WHAT THIS FILE IS, AND WHY IT IS C.
// The format, the SHA-256 chain, the seal and every verification decision live
// in rustkern/gfsjournal.rs. This file does I/O and lifecycle only: read a file
// into a buffer, hand it to Rust, write back what Rust produced. It holds no
// format knowledge beyond the two wire sizes, which are asserted against the
// Rust crate at init.
//
// It is C because wait_event is a C macro over __wait_prepare, proc_create_ex
// takes a C function pointer, and fat_read_file/ext2_append_file are C with no
// Rust bindings in this tree. Writing this glue in Rust would mean inventing a
// Rust binding layer for the filesystem, which is a large seam of its own and
// the first thing that would break when either API moves. That is the stated
// justification the Rust-first rule requires; "the surrounding code is C" is
// not one and is not being claimed.
//
// SINGLE WRITER, BY DESIGN. Exactly one thread appends: gfs_journal_init()
// (before any worker exists) and thereafter the seclog worker, which is already
// the kernel's one blockable, PRIO_LOW, post-mount audit thread. Slice 1 does
// NOT create a second worker to do what an existing worker already does at the
// right priority in the right context (CLAUDE.md: improve the shared primitive,
// never fork a private copy). A re-entrancy guard below REFUSES a concurrent
// append rather than corrupting the chain, and counts the refusal so it can
// never be a silent loss.
//
// NO BUSY-WAIT. This file contains no loop that waits for anything. Waiting is
// the caller's problem and the callers use the wait-queue (seclog.c).

#include "journal.h"
#include "fold.h"
#include "../../types.h"
#include "../../string.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../fat.h"
#include "../ext2.h"

// The single mounted volume. Externed locally exactly as gui/theme.c,
// security/seclog.c and games/doom do; adding a 5th declaration site in a header
// would be the drift, not the fix.
extern fat_fs_t g_fat_fs;

// 250 Hz monotonic tick (cpu/isr.h). NOT a wall clock: this kernel has none
// (docs/GRAPHFS_DESIGN.md section 6.1), which is why records carry uptime
// labelled as uptime plus a persisted boot generation, and never a fabricated
// date. blob.c's fake 2023 epoch is the anti-pattern being avoided.
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;

// --- the Rust half ---------------------------------------------------------
extern unsigned long gfsj_encode(unsigned char *buf, unsigned long buf_len,
                                 uint64_t seq, uint64_t boot_gen, uint64_t mono_ms,
                                 uint32_t actor_kind, uint32_t actor_id,
                                 uint16_t op, uint16_t effect,
                                 const unsigned char *payload, unsigned long payload_len,
                                 const unsigned char *prev_hash, unsigned char *out_self);
extern unsigned long gfsj_seal_encode(unsigned char *buf, unsigned long buf_len,
                                      uint64_t count, uint64_t boot_gen,
                                      const unsigned char *head);
extern uint64_t gfsj_seal_bootgen(const unsigned char *seal, unsigned long seal_len);
extern uint32_t gfsj_verify(const unsigned char *log, unsigned long log_len,
                            const unsigned char *seal, unsigned long seal_len,
                            uint64_t anchor_count, const unsigned char *anchor_head,
                            gfsj_verify_t *out);
extern uint64_t gfsj_sizes(void);

#define GFSJ_DIR        "/GRAPHFS"
#define GFSJ_LOG_PATH   "/GRAPHFS/JOURNAL.LOG"
#define GFSJ_SEAL_PATH  "/GRAPHFS/JOURNAL.SEAL"
#define GFSJ_BAD_LOG    "/GRAPHFS/JOURNAL.BAD"
#define GFSJ_BAD_SEAL   "/GRAPHFS/SEAL.BAD"

// Slice-1 growth cap. An append-only log cannot be trimmed, and verification
// reads it whole, so it is bounded until slice 4 adds checkpoint + rotation.
// 16384 records = 2.6 MB. Hitting it is LOUD and COUNTED: a dropped audit
// record must never be a silent gap (that is the failure seclog.c's overrun
// counter exists to avoid).
#define GFSJ_MAX_RECORDS  16384

static int      g_ready = 0;
static int      g_degraded = 0;
static uint64_t g_boot_gen = 0;
static uint64_t g_count = 0;              // records this kernel believes exist
static uint8_t  g_head[32];               // in-kernel anchor: the real chain head
static volatile int g_in_append = 0;
static uint64_t g_dropped = 0;

static uint64_t gfsj_mono_ms(void) {
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    return (uint64_t)timer_ticks * 1000ULL / (uint64_t)hz;
}

static const char *gfsj_reason_name(uint32_t r) {
    switch (r) {
        case GFSJ_OK:                return "OK";
        case GFSJ_R_RAGGED:          return "RAGGED (file is not whole records)";
        case GFSJ_R_BAD_MAGIC:       return "BAD_MAGIC";
        case GFSJ_R_BAD_VERSION:     return "BAD_VERSION";
        case GFSJ_R_CHAIN_BREAK:     return "CHAIN_BREAK (reordered or spliced)";
        case GFSJ_R_SEQ_GAP:         return "SEQ_GAP (records reordered)";
        case GFSJ_R_HASH_MISMATCH:   return "HASH_MISMATCH (a record was edited)";
        case GFSJ_R_SEAL_MISSING:    return "SEAL_MISSING";
        case GFSJ_R_SEAL_CORRUPT:    return "SEAL_CORRUPT";
        case GFSJ_R_TRUNCATED:       return "TRUNCATED (records removed)";
        case GFSJ_R_EXTENDED:        return "EXTENDED (records past the seal)";
        case GFSJ_R_HEAD_MISMATCH:   return "HEAD_MISMATCH";
        case GFSJ_R_ANCHOR_COUNT:    return "ANCHOR_COUNT (differs from kernel memory)";
        case GFSJ_R_ANCHOR_MISMATCH: return "ANCHOR_MISMATCH (rewritten under a live kernel)";
        case GFSJ_R_ARG:             return "ARG";
        case GFSJ_R_NOT_READY:       return "NOT_READY";
        default:                     return "UNKNOWN";
    }
}

int gfs_journal_ready(void) { return g_ready; }

// ---------------------------------------------------------------------------
// Append
// ---------------------------------------------------------------------------
int gfs_journal_append(uint32_t actor_kind, uint32_t actor_id,
                       uint16_t op, uint16_t effect,
                       const void *payload, uint32_t payload_len) {
    return gfs_journal_append_seq(actor_kind, actor_id, op, effect,
                                  payload, payload_len, 0);
}

// #711 slice 2: the same append, reporting the seq of the record it wrote. The
// seq IS an edge's identity (design section 5.3), so a caller that issues a
// contract learns its id from the position it occupies in the tamper-evident
// chain rather than from a separate counter that could disagree with the log.
int gfs_journal_append_seq(uint32_t actor_kind, uint32_t actor_id,
                           uint16_t op, uint16_t effect,
                           const void *payload, uint32_t payload_len,
                           uint64_t *out_seq) {
    if (!g_ready) return -1;
    // Any payload length is accepted: the record binds SHA-256 of ALL of it and
    // inlines the first GFSJ_INLINE_MAX bytes. Storing the bytes themselves in
    // the content-addressed blob store is slice 3; until then a long payload is
    // attested by the journal while living in the file that produced it.

    if (g_count >= GFSJ_MAX_RECORDS) {
        g_dropped++;
        if (g_dropped <= 4 || (g_dropped % 256) == 0)
            kprintf("[GFSJ] journal FULL at %lu records: DROPPED %lu record(s). "
                    "Rotation/checkpointing is not implemented yet; this is a GAP "
                    "in the audit trail, not a trim.\n",
                    (unsigned long)g_count, (unsigned long)g_dropped);
        return -3;
    }

    // Single-writer invariant. Refuse rather than interleave two records: a
    // half-chained journal would be indistinguishable from tampering, which is
    // exactly the signal this file exists to keep meaningful.
    if (g_in_append) {
        g_dropped++;
        kprintf("[GFSJ] REFUSED a concurrent append (single-writer invariant "
                "broken by a new caller); %lu record(s) now missing\n",
                (unsigned long)g_dropped);
        return -4;
    }
    g_in_append = 1;

    unsigned char rec[GFSJ_REC_SIZE];
    unsigned char self[32];
    unsigned long n = gfsj_encode(rec, sizeof(rec), g_count, g_boot_gen,
                                  gfsj_mono_ms(), actor_kind, actor_id, op, effect,
                                  (const unsigned char *)payload,
                                  (unsigned long)payload_len, g_head, self);
    if (n != GFSJ_REC_SIZE) {
        kprintf("[GFSJ] encode refused (op=%u len=%u)\n", (unsigned)op, (unsigned)payload_len);
        g_in_append = 0;
        return -5;
    }

    // Record first, seal second. A crash between them leaves one record past
    // the seal, which verify reports as EXTENDED: a distinct, diagnosable state
    // rather than an ambiguous one. The reverse order would make a crash look
    // like truncation, i.e. like an attack.
    if (ext2_append_file(GFSJ_LOG_PATH, rec, GFSJ_REC_SIZE) != 0) {
        kprintf("[GFSJ] FAILED to append record %lu to %s: the journal has a GAP\n",
                (unsigned long)g_count, GFSJ_LOG_PATH);
        g_in_append = 0;
        return -6;
    }
    uint64_t written_seq = g_count;
    memcpy(g_head, self, 32);
    g_count++;

    // #711 slice 2: THE record just became durable, so fold it. This is the ONLY
    // call site, which is what makes "the graph is a fold of the journal" true
    // by construction rather than by convention: there is no way to change the
    // graph that does not first put a record in the audit trail, and no record
    // in the audit trail that the graph does not see.
    gfs_fold_on_record(rec, g_boot_gen);
    if (out_seq) *out_seq = written_seq;

    unsigned char seal[GFSJ_SEAL_SIZE];
    if (gfsj_seal_encode(seal, sizeof(seal), g_count, g_boot_gen, g_head) != GFSJ_SEAL_SIZE ||
        fat_write_file(&g_fat_fs, GFSJ_SEAL_PATH, seal, GFSJ_SEAL_SIZE) != 0) {
        kprintf("[GFSJ] FAILED to write the seal after record %lu: the on-disk "
                "head is now behind the log (verify will report EXTENDED)\n",
                (unsigned long)(g_count - 1));
    }

    g_in_append = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Verify
// ---------------------------------------------------------------------------
int gfs_journal_verify(gfsj_verify_t *out) {
    if (!out) return GFSJ_R_ARG;
    memset(out, 0, sizeof(*out));
    if (!g_ready) {
        out->status = GFSJ_R_NOT_READY;
        out->reason = GFSJ_R_NOT_READY;
        return GFSJ_R_NOT_READY;
    }

    uint32_t log_sz = 0, seal_sz = 0;
    void *log = fat_read_file(&g_fat_fs, GFSJ_LOG_PATH, &log_sz);
    void *seal = fat_read_file(&g_fat_fs, GFSJ_SEAL_PATH, &seal_sz);

    uint32_t rc = gfsj_verify((const unsigned char *)log, (unsigned long)(log ? log_sz : 0),
                              (const unsigned char *)seal, (unsigned long)(seal ? seal_sz : 0),
                              g_count, g_head, out);
    out->degraded = (uint32_t)g_degraded;

    if (log) kfree(log);
    if (seal) kfree(seal);

    if (rc != GFSJ_OK)
        kprintf("[GFSJ] VERIFY FAILED: %s at seq %lu (on disk %lu, sealed %lu, "
                "kernel %lu)\n", gfsj_reason_name(rc), (unsigned long)out->bad_seq,
                (unsigned long)out->count, (unsigned long)out->seal_count,
                (unsigned long)g_count);
    return (int)rc;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void gfs_journal_init(void) {
    // FFI size lock. A silent layout drift between the Rust record writer and
    // the C ABI would produce a journal that verifies against itself and means
    // nothing. Fail hard instead.
    uint64_t sz = gfsj_sizes();
    if ((sz & 0xFFFF) != GFSJ_REC_SIZE ||
        ((sz >> 16) & 0xFFFF) != GFSJ_SEAL_SIZE ||
        ((sz >> 32) & 0xFFFF) != sizeof(gfsj_verify_t)) {
        kprintf("[GFSJ] REFUSING to start: Rust/C size mismatch "
                "(rec %lu/%d, seal %lu/%d, verify %lu/%d)\n",
                (unsigned long)(sz & 0xFFFF), GFSJ_REC_SIZE,
                (unsigned long)((sz >> 16) & 0xFFFF), GFSJ_SEAL_SIZE,
                (unsigned long)((sz >> 32) & 0xFFFF), (int)sizeof(gfsj_verify_t));
        return;
    }

    // ext2 root only. The FAT ESP is the firmware's and carries boot assets
    // ONLY; build/invariant-gate.sh fails any image that breaks that, so a blob
    // or journal landing there would make the golden unbuildable. Fail closed
    // rather than write to the wrong volume.
    if (!g_root_ext2) {
        kprintf("[GFSJ] not starting: the root filesystem is not ext2, and the "
                "journal must never be written to the FAT ESP\n");
        return;
    }

    int mk = fat_mkdir(&g_fat_fs, GFSJ_DIR);
    if (mk != 0 && mk != -17) {   // -17 == EEXIST (fs/fat.c)
        kprintf("[GFSJ] cannot create %s (rc=%d); journal disabled\n", GFSJ_DIR, mk);
        return;
    }

    uint32_t log_sz = 0, seal_sz = 0;
    void *log = fat_read_file(&g_fat_fs, GFSJ_LOG_PATH, &log_sz);
    void *seal = fat_read_file(&g_fat_fs, GFSJ_SEAL_PATH, &seal_sz);

    g_boot_gen = gfsj_seal_bootgen((const unsigned char *)seal,
                                   (unsigned long)(seal ? seal_sz : 0)) + 1;

    gfsj_verify_t v;
    memset(&v, 0, sizeof(v));
    // Anchor disabled (0/NULL): nothing has been appended this boot, so there is
    // no in-kernel head to compare against yet.
    uint32_t rc = gfsj_verify((const unsigned char *)log, (unsigned long)(log ? log_sz : 0),
                              (const unsigned char *)seal, (unsigned long)(seal ? seal_sz : 0),
                              0, 0, &v);

    if (rc == GFSJ_OK) {
        g_count = v.count;
        memcpy(g_head, v.head_hash, 32);
        kprintf("[GFSJ] journal verified: %lu record(s), boot generation %lu\n",
                (unsigned long)g_count, (unsigned long)g_boot_gen);
    } else {
        // FAIL CLOSED (CONTRACT_ARCHITECTURE.md 12.7), but do not destroy the
        // evidence and do not chain new records onto a broken file. Move both
        // files aside and start a fresh chain whose FIRST record is a checkpoint
        // naming what was found. The degraded flag is sticky for this boot and
        // is reported to every caller of gfs_journal_verify().
        g_degraded = 1;
        kprintf("[GFSJ] TAMPER/CORRUPTION DETECTED at startup: %s at seq %lu "
                "(on disk %lu record(s), seal claims %lu). Preserving the "
                "evidence as %s and starting a new chain.\n",
                gfsj_reason_name(rc), (unsigned long)v.bad_seq,
                (unsigned long)v.count, (unsigned long)v.seal_count, GFSJ_BAD_LOG);
        fat_delete(&g_fat_fs, GFSJ_BAD_LOG);
        fat_delete(&g_fat_fs, GFSJ_BAD_SEAL);
        if (log) fat_rename(&g_fat_fs, GFSJ_LOG_PATH, GFSJ_BAD_LOG);
        if (seal) fat_rename(&g_fat_fs, GFSJ_SEAL_PATH, GFSJ_BAD_SEAL);
        fat_delete(&g_fat_fs, GFSJ_LOG_PATH);
        fat_delete(&g_fat_fs, GFSJ_SEAL_PATH);
        g_count = 0;
        memset(g_head, 0, sizeof(g_head));
    }

    // #711 slice 2: rebuild the graph from the record stream, from the EXACT
    // bytes that were just verified above, before this boot appends anything.
    // In the degraded case the chain was broken and the file has been moved
    // aside as evidence, so there is nothing that can honestly be folded: the
    // fold starts empty and DEGRADED, and every grant check will deny.
    if (rc == GFSJ_OK)
        gfs_fold_replay(log, log ? log_sz : 0, g_boot_gen, 0);
    else
        gfs_fold_replay(0, 0, g_boot_gen, 1);

    if (log) kfree(log);
    if (seal) kfree(seal);

    g_ready = 1;

    if (g_degraded) {
        // 12 bytes: reason then the seq it was found at. Inline, so this record
        // needs no blob store.
        unsigned char why[GFSJ_INLINE_MAX];
        memset(why, 0, sizeof(why));
        why[0] = (unsigned char)(rc & 0xFF);
        for (int i = 0; i < 8; i++) why[4 + i] = (unsigned char)((v.bad_seq >> (8 * i)) & 0xFF);
        (void)gfs_journal_append(GFSJ_ACTOR_KERNEL, 0, GFSJ_OP_CHECKPOINT,
                                 GFSJ_EFFECT_NA, why, 12);
    }

    // Every boot leaves a record. This is also the proof that the subsystem
    // RAN rather than merely linked, which is the failure mode this ticket
    // exists to stop repeating (#710 async_io, #712 virtio, validate_user_ptr,
    // sse_save, and graphfs itself).
    unsigned char bg[GFSJ_INLINE_MAX];
    memset(bg, 0, sizeof(bg));
    for (int i = 0; i < 8; i++) bg[i] = (unsigned char)((g_boot_gen >> (8 * i)) & 0xFF);
    if (gfs_journal_append(GFSJ_ACTOR_KERNEL, 0, GFSJ_OP_BOOT, GFSJ_EFFECT_NA, bg, 8) != 0)
        kprintf("[GFSJ] FAILED to write the boot record; the journal is up but "
                "not writable\n");
    else
        kprintf("[GFSJ] up: %s (%lu record(s)), seal %s\n",
                GFSJ_LOG_PATH, (unsigned long)g_count, GFSJ_SEAL_PATH);
}
