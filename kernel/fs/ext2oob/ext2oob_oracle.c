// ext2oob_oracle.c - #476 heap-over-read regression oracle for the ext2
// directory-block entry walk (fs/ext2.c). BUILD-CONTAINER ONLY: this is host
// user-space code, NOT part of kernel.elf. It is driven by ext2oob-oracle.sh,
// which extracts the REAL, shipping ext2_dirblock_find_c() from fs/ext2.c into
// ext2_dirblock_find_real.inc and compiles it here as the GREEN arm, so the
// oracle can never silently rot away from the code it guards.
//
// It proves, on the SAME crafted directory blocks:
//   RED   = the historical pre-#476 UNGUARDED walk over-reads / DoS-loops
//           (guard-page SIGSEGV, or ASan heap-buffer-overflow, or a runaway
//           rec_len=0 loop caught by SIGALRM).
//   GREEN = the real shipping ext2_dirblock_find_c() rejects every malformed
//           entry, stays in bounds, AND still finds entries in a valid block
//           (so the guards are proven to FIRE, not to be no-ops that reject
//           everything).
//
// Buffer backing is selectable at compile time:
//   -DUSE_MMAP_GUARD : the block sits flush against a PROT_NONE guard page, so
//                      any read at index >= block_size faults deterministically.
//                      THIS is what ext2oob-oracle.sh builds: a hard wall at
//                      byte block_size catches an over-read of any distance,
//                      every run.
//   -DUSE_MALLOC     : the block is a plain malloc(block_size); intended for an
//                      -fsanitize=address build. Kept as an alternative but NOT
//                      used by the driver: an ASan redzone is a small fixed band
//                      a far read can clear undetected, and its fault address is
//                      allocator/ASLR dependent, i.e. non-deterministic here.
//
// No em-dashes anywhere by house style.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

// -------- shims for the extracted kernel function --------------------------
// fs/ext2.c reads little-endian on-disk fields through rd16/rd32 (memcpy-based
// there). Reproduce the exact little-endian semantics here.
static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// GREEN arm: the REAL ext2_dirblock_find_c(), extracted verbatim from
// fs/ext2.c by ext2oob-oracle.sh. If someone weakens the shipping guards this
// file changes and the oracle goes RED.
#include "ext2_dirblock_find_real.inc"

// -------- RED arm: historical pre-#476 UNGUARDED walk ----------------------
// Faithful reconstruction of the vulnerable loop described in blame/#476:
// unchecked 8-byte header read, unchecked name compare blk[off+8+i], and an
// unvalidated off += rec. No rec<8, no off+rec>block_size, no name-fits check.
static int ext2_dirblock_find_unguarded(const uint8_t *blk, uint32_t block_size,
                                        const char *name, uint32_t name_len,
                                        int ci, uint32_t *out_ino,
                                        uint8_t *out_type) {
    uint32_t off = 0;
    while (off < block_size) {                 // pre-#476: no header-fits guard
        uint32_t e_ino = rd32(blk + off + 0);  // may read past block tail
        uint16_t rec   = rd16(blk + off + 4);  // may read past block tail
        uint8_t  nlen  = blk[off + 6];
        uint8_t  ftype = blk[off + 7];
        if (e_ino != 0 && nlen == name_len) {
            int match = 1;
            for (uint32_t i = 0; i < name_len; i++) {   // may read past block
                char a = (char)blk[off + 8 + i];
                char b = name[i];
                if (ci) {
                    if (a >= 'a' && a <= 'z') a -= 32;
                    if (b >= 'a' && b <= 'z') b -= 32;
                }
                if (a != b) { match = 0; break; }
            }
            if (match) {
                if (out_ino)  *out_ino  = e_ino;
                if (out_type) *out_type = ftype;
                return 1;
            }
        }
        off += rec;                            // rec==0 -> infinite loop (DoS)
    }
    return 0;
}

// -------- block backing ----------------------------------------------------
#define BLOCK_SIZE 1024u
#define PAGE 4096u

// Returns a pointer to a BLOCK_SIZE region. With USE_MMAP_GUARD the region ends
// flush against a PROT_NONE page so blk[BLOCK_SIZE + k] faults. With USE_MALLOC
// it is a plain malloc(BLOCK_SIZE) for ASan redzone detection.
static uint8_t *alloc_block(void) {
#ifdef USE_MMAP_GUARD
    uint8_t *base = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(2); }
    if (mprotect(base + PAGE, PAGE, PROT_NONE) != 0) { perror("mprotect"); exit(2); }
    // Place the block so its last byte is the last byte of the first page.
    return base + (PAGE - BLOCK_SIZE);
#else
    uint8_t *b = malloc(BLOCK_SIZE);
    if (!b) { perror("malloc"); exit(2); }
    return b;
#endif
}

// -------- on-disk dirent writer (little-endian) ----------------------------
static void put_ent(uint8_t *blk, uint32_t off, uint32_t ino, uint16_t rec,
                    uint8_t nlen, uint8_t ftype, const char *name) {
    blk[off + 0] = ino & 0xFF;
    blk[off + 1] = (ino >> 8) & 0xFF;
    blk[off + 2] = (ino >> 16) & 0xFF;
    blk[off + 3] = (ino >> 24) & 0xFF;
    blk[off + 4] = rec & 0xFF;
    blk[off + 5] = (rec >> 8) & 0xFF;
    blk[off + 6] = nlen;
    blk[off + 7] = ftype;
    if (name) for (uint8_t i = 0; i < nlen; i++) blk[off + 8 + i] = (uint8_t)name[i];
}

// -------- fixtures ---------------------------------------------------------
// Each builder fills a BLOCK_SIZE block and returns the (name,name_len) the
// walk should search for. Malicious builders return the search target chosen to
// drive the pre-#476 walk out of bounds.

typedef void (*builder_t)(uint8_t *blk);

// VALID: ".", "..", "hello", "world"; rec chain lands exactly on BLOCK_SIZE.
static void b_valid(uint8_t *blk) {
    memset(blk, 0, BLOCK_SIZE);
    uint32_t off = 0;
    put_ent(blk, off, 2, 12, 1, 2, ".");         off += 12;
    put_ent(blk, off, 2, 12, 2, 2, "..");        off += 12;
    put_ent(blk, off, 11, 16, 5, 1, "hello");    off += 16;
    // last entry absorbs the tail to BLOCK_SIZE
    put_ent(blk, off, 12, (uint16_t)(BLOCK_SIZE - off), 5, 1, "world");
}

// MAL-A "header straddle": a first record consumes almost the whole block and
// leaves a stub header in the trailing < 8 bytes at off=1020. The unguarded
// loop (off < block_size) enters at 1020 and reads rec=rd16(blk+1024) off the
// end. The guarded loop (off + 8 <= block_size) stops.
static void b_mal_header_straddle(uint8_t *blk) {
    memset(blk, 0, BLOCK_SIZE);
    put_ent(blk, 0, 2, 1020, 1, 2, ".");   // one big record, off jumps to 1020
    // bytes [1020..1023] look like the start of another header; rec/nlen live
    // at [1024..], in the guard page.
    blk[1020] = 5; blk[1021] = 0; blk[1022] = 0; blk[1023] = 0; // ino != 0
}

// MAL-B "name past block": an entry near the end claims name_len == the search
// length but its name runs past BLOCK_SIZE. The unguarded name compare reads
// blk[off+8+i] into the guard page; the guarded walk rejects on
// off + 8 + name_len > block_size. The stored bytes that DO fit are filled with
// 'a' so the RED compare keeps matching and marches straight off the end (a
// compare that mismatched early would stop short of the boundary and hide the
// bug), which is exactly why the search target is a run of 'a's.
static void b_mal_name_past_block(uint8_t *blk) {
    memset(blk, 0, BLOCK_SIZE);
    put_ent(blk, 0, 2, 900, 1, 2, ".");            // filler, off -> 900
    // entry at 900: ino!=0, nlen=200, rec keeps off+rec == 1024 (passes the rec
    // guard) but off+8+200 = 1108 > 1024 (fails the name guard).
    put_ent(blk, 900, 7, 124, 200, 1, NULL);
    // fill the in-block portion of the name field with 'a' (offsets 908..1023).
    for (uint32_t o = 908; o < BLOCK_SIZE; o++) blk[o] = 'a';
}

// MAL-C "rec_len zero": an empty entry (ino==0) with rec == 0. The search
// target is absent, so the unguarded loop never matches and never advances off
// -> infinite loop (DoS), caught by SIGALRM. The guarded walk rejects on
// rec < 8 and returns cleanly.
static void b_mal_rec_zero(uint8_t *blk) {
    memset(blk, 0, BLOCK_SIZE);
    put_ent(blk, 0, 2, 12, 1, 2, ".");
    put_ent(blk, 12, 0, 0, 0, 0, NULL);     // ino=0, rec_len=0 -> spin
}

// MAL-D "rec_len past end": rec_len that runs off the block end. The guarded
// walk rejects on off + rec > block_size. (The unguarded walk here does not
// necessarily fault, it mis-walks; included to prove the guard rejects it.)
static void b_mal_rec_past_end(uint8_t *blk) {
    memset(blk, 0, BLOCK_SIZE);
    put_ent(blk, 0, 2, 12, 1, 2, ".");
    put_ent(blk, 12, 9, 60000, 4, 1, "big"); // rec_len far past block end
}

// -------- per-case search targets ------------------------------------------
typedef int (*finder_t)(const uint8_t *, uint32_t, const char *, uint32_t,
                        int, uint32_t *, uint8_t *);

struct testcase {
    const char *name;
    builder_t   build;
    const char *search;      // name to look up
    uint32_t    search_len;
    int         valid_expect_found; // for the VALID block under GREEN
};

// -------- child runner: run one finder on one built block ------------------
// Runs in a forked child so a RED SIGSEGV or DoS does not take down the oracle.
// Exit 0 = returned safely. Killed by SIGSEGV/SIGBUS = over-read. Killed by
// SIGALRM = runaway loop (DoS).
static void run_child(finder_t fn, builder_t build, const char *search,
                      uint32_t search_len) {
    uint8_t *blk = alloc_block();
    build(blk);
    alarm(3); // DoS guard for rec_len==0
    uint32_t ino = 0; uint8_t ty = 0;
    volatile int r = fn(blk, BLOCK_SIZE, search, search_len, 1, &ino, &ty);
    (void)r;
    _exit(0);
}

// classify a child's wait status into a short verdict string.
static const char *verdict(int status, int *safe) {
    *safe = 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) { *safe = 1; return "SAFE (returned in bounds)"; }
    if (WIFSIGNALED(status)) {
        int s = WTERMSIG(status);
        if (s == SIGSEGV || s == SIGBUS) return "OVER-READ (guard-page fault)";
        if (s == SIGALRM) return "DoS (runaway loop, SIGALRM)";
        if (s == SIGABRT) return "OVER-READ (ASan abort)";
        return "killed by other signal";
    }
    return "unknown";
}

static int run_case(finder_t fn, struct testcase *tc) {
    pid_t pid = fork();
    if (pid == 0) { run_child(fn, tc->build, tc->search, tc->search_len); }
    int status = 0; waitpid(pid, &status, 0);
    int safe = 0;
    const char *v = verdict(status, &safe);
    printf("      %-26s -> %s\n", tc->name, v);
    return safe;
}

// GREEN correctness on the VALID block: it must FIND an entry that is present,
// and NOT find one that is absent. Run in-process (GREEN never faults).
static int green_valid_correctness(void) {
    uint8_t *blk = alloc_block();
    b_valid(blk);
    uint32_t ino = 0; uint8_t ty = 0;
    int found_hello = ext2_dirblock_find_c(blk, BLOCK_SIZE, "hello", 5, 1, &ino, &ty);
    uint32_t ino2 = 0;
    int found_world = ext2_dirblock_find_c(blk, BLOCK_SIZE, "world", 5, 1, &ino2, &ty);
    int found_nope  = ext2_dirblock_find_c(blk, BLOCK_SIZE, "nope", 4, 1, &ino, &ty);
    printf("      valid-block lookups        -> hello=%s(ino=%u) world=%s(ino=%u) nope=%s\n",
           found_hello ? "FOUND" : "MISS", found_hello ? ino : 0,
           found_world ? "FOUND" : "MISS", found_world ? ino2 : 0,
           found_nope ? "FOUND(BUG)" : "not-found");
    return found_hello && ino == 11 && found_world && ino2 == 12 && !found_nope;
}

// Build the shared case table. `aaa` must live for the process lifetime.
static char g_aaa[201];
static struct testcase *build_cases(int *n) {
    static struct testcase cases[4];
    for (int i = 0; i < 200; i++) g_aaa[i] = 'a';
    g_aaa[200] = 0;
    cases[0] = (struct testcase){ "MAL-A header-straddle",  b_mal_header_straddle, "a",    1,   0 };
    cases[1] = (struct testcase){ "MAL-B name-past-block",  b_mal_name_past_block, g_aaa,  200, 0 };
    cases[2] = (struct testcase){ "MAL-C rec_len-zero",     b_mal_rec_zero,        "zzzz", 4,   0 };
    cases[3] = (struct testcase){ "MAL-D rec_len-past-end", b_mal_rec_past_end,    "big",  3,   0 };
    *n = 4;
    return cases;
}

int main(void) {
    int n = 0;
    struct testcase *cases = build_cases(&n);

#ifdef USE_MMAP_GUARD
    const char *backing = "mmap guard-page";
#else
    const char *backing = "malloc + ASan";
#endif

    printf("=== ext2 #476 dir-walk over-read oracle (backing: %s) ===\n", backing);

    printf("\n[RED]  historical pre-#476 UNGUARDED walk (must over-read / DoS):\n");
    int red_bad = 0;   // count of cases where RED demonstrably went wrong
    for (int i = 0; i < n; i++) {
        int safe = run_case(ext2_dirblock_find_unguarded, &cases[i]);
        if (!safe) red_bad++;
    }

    printf("\n[GREEN] real shipping ext2_dirblock_find_c() (must stay in bounds):\n");
    int green_all_safe = 1;
    for (int i = 0; i < n; i++) {
        int safe = run_case(ext2_dirblock_find_c, &cases[i]);
        if (!safe) green_all_safe = 0;
    }

    printf("\n[GREEN] guards are not no-ops (valid block must still resolve):\n");
    int green_correct = green_valid_correctness();

    printf("\n=== summary ===\n");
    printf("  RED  over-read/DoS on %d of %d malicious blocks\n", red_bad, n);
    printf("  GREEN safe on all malicious blocks : %s\n", green_all_safe ? "YES" : "NO");
    printf("  GREEN correct on the valid block   : %s\n", green_correct ? "YES" : "NO");

    // The oracle PASSES only if:
    //  - RED genuinely misbehaved on at least the two guard-page cases (proves
    //    the vulnerability is real and the checks are load-bearing), AND
    //  - GREEN was safe on every malicious block, AND
    //  - GREEN still resolved the valid block (guards fire, are not no-ops).
    // With USE_MMAP_GUARD we require >= 3 RED failures (A,B,C fault/DoS; D may
    // only mis-walk). With ASan (USE_MALLOC) A and B over-read the malloc
    // redzone; require >= 2.
#ifdef USE_MMAP_GUARD
    int red_needed = 3;
#else
    int red_needed = 2;
#endif
    int pass = (red_bad >= red_needed) && green_all_safe && green_correct;
    printf("\nORACLE: %s\n", pass ? "PASS (RED over-reads, GREEN safe + correct)" : "FAIL");
    return pass ? 0 : 1;
}
