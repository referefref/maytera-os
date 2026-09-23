// memhog - #dosmem: a deterministic reproducer for the still-open
// LARGE-ALLOCATION fault, and a boot-log instrument for it.
//
// THE DEFECT IT CHASES. Two independent findings, in two different apps, on
// two different builds:
//
//   * DOSUSER (golden 2346, the owner's own iMac stick): every DOS guest dies
//     at `rep stos` inside memset with RDX=0x100000 = DOS_MEM_SIZE, i.e.
//     dosexec.c's `memset(t->mem, 0, DOS_MEM_SIZE)`. err=0x6 = write / user /
//     NOT PRESENT, CR2=0x8040dee000, which is 14.6 MB into the libc heap
//     (HEAP_START = 0x8040000000).
//   * COMPCEIL (build 2346, 1 boot in 24): a 27.82 MB malloc returned
//     non-NULL, its own `test %rax,%rax; je` check passed, one sys_mmap
//     covered the whole range, and a write 11.67 MB in faulted at a
//     page-aligned address.
//
// Same shape both times: memory the allocator says it has, that demand paging
// then refuses to back, part of the way through. Neither app is the bug.
// ANYTHING that allocates large and touches all of it will hit this, which is
// exactly what makes it worth a dedicated reproducer rather than another
// round of reading a game's crash.
//
// WHY A SEPARATE APP AND NOT "run the game again". A game gives you one
// allocation, one size, at one moment, with a stripped binary. This gives a
// LADDER of sizes, writes EVERY page, verifies what it wrote, and says how far
// it got - so a failure names the size and the offset instead of a RIP. And it
// says all of that through sys_bootlog(), so it works on the owner's machines,
// which have no serial port.
//
// DELIBERATELY NO printf(): printf allocates, and the whole point here is to
// stress the allocator. Everything goes out through raw sys_write() and
// sys_bootlog(), the same discipline heaptest and the allocator's own
// heap_write_str() use.
//
// Launched via /CONFIG/AUTORUN.CFG containing "/APPS/MEMHOG".

#include "syscall.h"
#include "stdlib.h"
#include "string.h"

static void ws(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

static char *unum(char *b, unsigned long v) {
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) *b++ = t[--i];
    return b;
}

static char *uhex(char *b, unsigned long v) {
    *b++ = '0'; *b++ = 'x';
    int started = 0;
    for (int i = 15; i >= 0; i--) {
        int nib = (int)((v >> (i * 4)) & 0xF);
        if (!nib && !started && i) continue;
        started = 1;
        *b++ = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    return b;
}

static char *scat(char *b, const char *s) { while (*s) *b++ = *s++; return b; }

// One line to BOTH sinks. Serial for a VM run, /BOOTLOG.TXT for the owner's
// hardware. Neither is a substitute for the other: serial is silent in GUI
// mode, and the bootlog is the only thing that survives on his machines.
static void say(const char *msg) {
    ws(msg); ws("\n");
    sys_bootlog(msg);
}

static int g_fail = 0;

// Write a page-derived pattern over the WHOLE buffer, then read it all back.
// The pattern is derived from the byte's own offset, so a wrong value proves
// which page's contents were lost, not merely that something differed.
//
// The write is deliberately a plain byte loop and not memset: memset is what
// DOSUSER faulted inside, and an optimised `rep stos` gives no progress
// information at all when it dies. This loop's `last_ok` is the whole point.
static void touch_ladder(unsigned long mb) {
    char line[256], *p;
    unsigned long bytes = mb * 1024UL * 1024UL;

    p = line; p = scat(p, "[MEMHOG] malloc("); p = unum(p, mb);
    p = scat(p, " MB = "); p = unum(p, bytes); p = scat(p, " bytes)"); *p = 0;
    say(line);

    unsigned char *buf = (unsigned char *)malloc(bytes);
    if (!buf) {
        p = line; p = scat(p, "[MEMHOG] FAIL malloc("); p = unum(p, mb);
        p = scat(p, " MB) returned NULL - the allocator REFUSED, which is a "
                    "different (and honest) outcome from the defect"); *p = 0;
        say(line); g_fail++;
        return;
    }

    p = line; p = scat(p, "[MEMHOG] got buf="); p = uhex(p, (unsigned long)buf);
    p = scat(p, " end="); p = uhex(p, (unsigned long)buf + bytes);
    p = scat(p, " heap_hw="); p = unum(p, (unsigned long)malloc_heap_highwater() >> 20);
    p = scat(p, " MB"); *p = 0;
    say(line);

    // WRITE every byte. If this faults, the kernel's [VMFAULT] line names the
    // address and the reason, and the last "reached" line below names how far
    // we got - which together give the offset without any disassembly.
    for (unsigned long off = 0; off < bytes; off++) {
        buf[off] = (unsigned char)((off >> 12) ^ (off & 0xFF));
        if ((off & 0x3FFFFFUL) == 0 && off) {         // every 4 MB
            p = line; p = scat(p, "[MEMHOG] wrote "); p = unum(p, off >> 20);
            p = scat(p, " MB of "); p = unum(p, mb);
            p = scat(p, " MB, at "); p = uhex(p, (unsigned long)buf + off); *p = 0;
            say(line);
        }
    }

    // READ it all back. A page that faulted on write and was silently
    // re-satisfied with a FRESH zero page would pass the write loop and fail
    // here; that is the #510/#511 zero-fill class and it must not be confused
    // with the fault class.
    unsigned long bad = 0, first_bad = 0;
    for (unsigned long off = 0; off < bytes; off++) {
        unsigned char want = (unsigned char)((off >> 12) ^ (off & 0xFF));
        if (buf[off] != want) {
            if (!bad) first_bad = off;
            bad++;
        }
    }

    p = line;
    if (bad) {
        p = scat(p, "[MEMHOG] FAIL "); p = unum(p, mb);
        p = scat(p, " MB: "); p = unum(p, bad);
        p = scat(p, " byte(s) read back WRONG, first at offset ");
        p = uhex(p, first_bad); p = scat(p, " (addr ");
        p = uhex(p, (unsigned long)buf + first_bad);
        p = scat(p, ") - written pages were silently replaced, NOT a fault");
        g_fail++;
    } else {
        p = scat(p, "[MEMHOG] PASS "); p = unum(p, mb);
        p = scat(p, " MB written and verified byte-for-byte");
    }
    *p = 0;
    say(line);

    free(buf);
}

// The DOS shape exactly: one 1 MB block, memset to zero in one call, which is
// the instruction DOSUSER dies inside.
static void dos_shape(void) {
    char line[256], *p;
    const unsigned long DOS_MEM_SIZE = 0x100000UL;
    say("[MEMHOG] DOS shape: malloc(0x100000) + memset(0) - the exact "
        "dosexec.c:10677 pair every DOS guest dies on");
    unsigned char *m = (unsigned char *)malloc(DOS_MEM_SIZE);
    if (!m) { say("[MEMHOG] FAIL DOS shape: malloc(1 MB) returned NULL"); g_fail++; return; }
    p = line; p = scat(p, "[MEMHOG] DOS shape buf="); p = uhex(p, (unsigned long)m); *p = 0;
    say(line);
    memset(m, 0, DOS_MEM_SIZE);
    unsigned long nz = 0;
    for (unsigned long i = 0; i < DOS_MEM_SIZE; i++) if (m[i]) nz++;
    if (nz) { say("[MEMHOG] FAIL DOS shape: memset left non-zero bytes"); g_fail++; }
    else      say("[MEMHOG] PASS DOS shape: 1 MB memset survived and reads back zero");
    free(m);
}

// #dosmem: PROVE WHAT EXHAUSTION ACTUALLY LOOKS LIKE FROM RING 3.
//
// The whole open question about the owner's iMac fault is which of three
// things mm_fault() refused for. One of the three is "the demand allocation
// failed because the machine is out of physical memory", and the reason that
// one is worth proving rather than reasoning about is that its Ring-3
// SYMPTOM is counter-intuitive: malloc() does NOT return NULL. malloc only
// reserves address space (one sys_mmap of a lazy VMA); the physical pages are
// not allocated until the app WRITES them. So an out-of-memory machine hands
// back a perfectly good pointer, passes the app's own NULL check, and then
// kills it with a SIGSEGV part-way through the buffer - which is character for
// character the DOSUSER and COMPCEIL reports.
//
// This walks the heap up in chunks, touching every page of each, and says how
// far it got. On a machine with headroom it stops at the libc 512 MB ceiling
// with a clean malloc failure. On a machine without headroom it dies mid-write,
// and the kernel's [VMFAULT] line then says WHY=LAZY_OOM next to a pmm free
// count of ~0, which is the signature to compare against his next boot log.
static void drain(void) {
    char line[256], *p;
    const unsigned long CHUNK = 8UL * 1024 * 1024;
    unsigned long done = 0;
    say("[MEMHOG] drain: allocating and TOUCHING 8 MB at a time until the "
        "allocator refuses. malloc returning NULL here is the HEALTHY outcome.");
    for (int i = 0; i < 80; i++) {
        unsigned char *b = (unsigned char *)malloc(CHUNK);
        if (!b) {
            p = line; p = scat(p, "[MEMHOG] drain: malloc REFUSED after ");
            p = unum(p, done >> 20);
            p = scat(p, " MB touched - clean refusal, heap ceiling reached, no fault");
            *p = 0; say(line);
            return;
        }
        for (unsigned long o = 0; o < CHUNK; o += 4096) b[o] = (unsigned char)i;
        done += CHUNK;
        p = line; p = scat(p, "[MEMHOG] drain: touched "); p = unum(p, done >> 20);
        p = scat(p, " MB, last chunk at "); p = uhex(p, (unsigned long)b); *p = 0;
        say(line);
    }
    say("[MEMHOG] drain: stopped at the 80-chunk bound with no refusal and no fault");
}

int main(void) {
    char line[256], *p;
    say("[MEMHOG] start - reproducing the large-allocation demand-paging defect");

    dos_shape();

    // A ladder, because the two known failures were at 14.6 MB and 11.67 MB
    // into buffers of 1 MB and 27.82 MB respectively. If the boundary is a
    // SIZE, the ladder finds it; if it is a HEAP OFFSET, the ladder walks past
    // it because heap_end never shrinks, and the reported buf= addresses say
    // which.
    static const unsigned long ladder[] = { 1, 2, 4, 8, 16, 24, 28, 32, 48, 64 };
    for (unsigned i = 0; i < sizeof(ladder)/sizeof(ladder[0]); i++)
        touch_ladder(ladder[i]);

    drain();

    p = line; p = scat(p, "[MEMHOG] DONE failures="); p = unum(p, (unsigned long)g_fail);
    p = scat(p, " final_heap_hw="); p = unum(p, (unsigned long)malloc_heap_highwater() >> 20);
    p = scat(p, " MB"); *p = 0;
    say(line);
    return g_fail ? 1 : 0;
}
