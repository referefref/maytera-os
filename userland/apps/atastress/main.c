// atastress - #444 verification harness: N concurrent Ring-3 processes hammer
// large sequential reads of a known-content file on the ext2 root through the
// SAME code path CPython used to trip (open/read syscalls -> VFS -> ext2 ->
// blk_read -> ata_read_sectors_dma_ext). Each worker re-derives the expected
// byte value for every offset from a pure formula (no stored file needed on
// the guest side beyond /BIGTEST.BIN itself) and reports zero-hole hits
// (bytes that came back 0 where a nonzero byte was expected -- the exact
// #444 symptom) separately from other corruption, plus overall PASS/FAIL.
// Runs as a background service (SERVICES.CFG); all output is mirrored to the
// serial log, so grep for "ATASTRESS:".
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"
#include "../../libc/fcntl.h"

// #444 v2: each worker reads its OWN file (/BIGTEST0.BIN.../BIGTEST3.BIN)
// instead of all 4 hammering the SAME inode. v1 (shared file) hit an
// unrelated same-inode-concurrent-open/read limitation in this kernel
// (read() started failing with rc=-1 on the 2nd concurrent open of the same
// file, with ZERO data-corruption observed on the reads that did succeed -
// a different bug from #444, out of scope here) before enough iterations
// could run to hammer the ATA DMA layer. Distinct files still force fully
// concurrent DMA transfers on the shared ATA channel/global lock/dma_buffer
// (the actual #444 mechanism) and is a closer match to the real CPython
// scenario anyway (many DIFFERENT stdlib .py files opened concurrently, not
// one file reopened by four processes).
#define FILE_SIZE    (8u * 1024u * 1024u)   // must match the generated files exactly
#define CHUNK        8192u                   // kept small: large static .bss buffers
                                              // hit R_X86_64_32S relocation-truncated
                                              // failures under this user.ld's small
                                              // code model (same limit CPython's port
                                              // hit); 8 KB matches the proven-safe
                                              // spindisk buffer size. Still spans many
                                              // ATA DMA transfers per file (1024
                                              // reads/iteration at 8 MB).
#define ITERS        30
#define NWORKERS     4

static char g_buf[CHUNK];

// Emit a whole formatted line in ONE SYS_WRITE so the "ATASTRESS:" markers
// stay contiguous in the serial log (matches pttest's outf() pattern).
static void outf(const char *fmt, ...) {
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    syscall3(SYS_WRITE, 1, (long)(unsigned long)buf, (long)n);
}

// Deterministic per-offset expected byte (per-worker salt so each worker's
// file has distinct content). Cheap 32-bit multiplicative hash + xor-shift
// so there are no long runs of the same value (a real zero-hole is
// unmistakable) and no dependency on any stored golden file on the guest.
// MUST match gen_bigtest_multi.py exactly.
static unsigned char expected_byte(unsigned long off, int wid) {
    unsigned int v = (unsigned int)(off + (unsigned long)wid * 104729u);
    v = v * 2654435761u;
    v ^= (v >> 15);
    return (unsigned char)(v & 0xFF);
}

// One worker: open+read the whole file in CHUNK-sized reads, ITERS times,
// verifying every byte. Returns 0 (pass) or 1 (fail) via exit status.
static int run_worker(int wid) {
    unsigned long total_bytes = 0;
    unsigned long zero_holes = 0;   // byte==0 but expected!=0 (the #444 symptom)
    unsigned long other_bad  = 0;   // byte!=0 but wrong value (generic corruption)
    int reported = 0;
    char path[32];
    int pn = snprintf(path, sizeof(path), "/BIGTEST%d.BIN", wid);
    (void)pn;

    for (int it = 0; it < ITERS; it++) {
        int fd = sys_open(path, O_RDONLY);
        if (fd < 0) {
            outf("ATASTRESS[%d]: FAIL open(%s) rc=%d iter=%d\n", wid, path, fd, it);
            return 1;
        }
        unsigned long off = 0;
        for (;;) {
            long n = sys_read(fd, g_buf, CHUNK);
            if (n < 0) {
                outf("ATASTRESS[%d]: FAIL read() rc=%ld off=%lu iter=%d\n", wid, n, off, it);
                sys_close(fd);
                return 1;
            }
            if (n == 0) break;  // EOF
            // NOTE: this app's base is 0x80000000 (user.ld). Indexing the
            // static g_buf[] array directly makes gcc embed g_buf's address
            // as a sign-extended disp32 in a SIB memory operand
            // (R_X86_64_32S) - the x86-64 ISA has no zero-extending disp32
            // form for memory operands - and 0x80000000 cannot be losslessly
            // sign-extended, so the linker fails with "relocation truncated
            // to fit" (the same class of bug noted for the CPython port).
            // Fix: materialize the base address into a register ONCE via a
            // local pointer, then index off that register (register+index
            // addressing needs no symbol relocation at all).
            unsigned char *bufp = (unsigned char *)g_buf;
            for (unsigned long i = 0; i < (unsigned long)n; i++) {
                unsigned char want = expected_byte(off + i, wid);
                unsigned char got  = bufp[i];
                if (got != want) {
                    if (got == 0 && want != 0) zero_holes++;
                    else other_bad++;
                    if (!reported || (zero_holes + other_bad) <= 8) {
                        outf("ATASTRESS[%d]: MISMATCH iter=%d off=%lu want=0x%02x got=0x%02x\n",
                             wid, it, off + (unsigned long)i, want, got);
                        reported = 1;
                    }
                }
            }
            off += (unsigned long)n;
            total_bytes += (unsigned long)n;
        }
        sys_close(fd);
        if (off != FILE_SIZE) {
            outf("ATASTRESS[%d]: FAIL short read iter=%d got %lu bytes, want %u\n",
                 wid, it, off, FILE_SIZE);
            return 1;
        }
    }

    outf("ATASTRESS[%d]: done bytes=%lu zero_holes=%lu other_bad=%lu\n",
         wid, total_bytes, zero_holes, other_bad);
    return (zero_holes == 0 && other_bad == 0) ? 0 : 1;
}

// #444 v3: NWORKERS independent OS processes (SERVICES.CFG spawns 4 separate
// ELF images atastress0..atastress3 via the normal proc_create path), not
// fork(). v2 (this app fork()-ing 4 workers) reproduced a DIFFERENT, striking
// bug: children ended up reading each OTHER's file content byte-for-byte
// (workers 0/1/2 all read worker 3's exact expected bytes) - a fork()/heap
// copy-on-write isolation issue, not the #444 ATA DMA race this harness is
// meant to test. That is a separate, out-of-scope finding (noted in the
// #444 report) worth its own investigation later. Using independently
// spawned processes (no fork, no shared heap ancestry) isolates the test to
// exactly the code path #444 fixed: concurrent open/read syscalls -> VFS ->
// ext2 -> blk_read -> ata_read_sectors_dma_ext -> the ATA global I/O lock.
#ifndef WID
#define WID 0
#endif

int main(void) {
    outf("ATASTRESS[%d]: === #444 ATA DMA concurrent-hammer verification start (pid=%d) ===\n",
         WID, sys_getpid());
    int rc = run_worker(WID);
    outf("ATASTRESS[%d]: RESULT %s\n", WID, rc == 0 ? "PASS" : "FAIL");
    return rc;
}
