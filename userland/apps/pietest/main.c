/* pietest - #640 stage 1: prove POSITION-INDEPENDENT execution end to end.
 *
 * WHAT THIS IS FOR. kernel/exec/elf.c has a full relocation engine
 * (elf_apply_all_relocations, handling R_X86_64_RELATIVE / GLOB_DAT /
 * JUMP_SLOT / 64 / TLS) that is invoked unconditionally on EVERY user launch.
 * It has never once applied a relocation, because nothing in the tree has a
 * PT_DYNAMIC: 0 of 43 built binaries are ET_DYN. "The code is there and it is
 * called" is not evidence that it WORKS. This app is the evidence.
 *
 * HOW IT CAN FAIL, which is the only thing that makes a pass meaningful. Every
 * pointer below is initialised at LINK time with a value relative to base 0.
 * If the kernel loads the image at 0x90000000 and applies nothing, those
 * pointers still hold their small link-time values and every check fails. The
 * range check runs FIRST and a pointer is only dereferenced after it passes,
 * so an unrelocated binary reports FAIL rather than dying on a fault and
 * reporting nothing.
 *
 * EVERY POINTER IS REACHED THROUGH AN ARRAY AND A RUNTIME INDEX ON PURPOSE. A
 * scalar `static const void *p = &p;` compiled at -O2 does NOT survive: gcc
 * knows the value, folds it into a RIP-relative lea and emits no relocation at
 * all, so the "check" would pass on a loader that does nothing. MEASURED by
 * counting .rela.dyn entries: the scalar version produced 6, this one produces
 * 11. The two ptrtab[] identity checks below may STILL be folded for the same
 * reason and are therefore treated as a bonus, not as the proof; the proof is
 * strtab[] and fntab[], every entry of which is reached through a runtime
 * index that gcc cannot constant-fold.
 *
 * No libc: see start.S.
 */

typedef unsigned long uint64;
typedef long          int64;

#define SYS_EXIT   0
#define SYS_WRITE  13

static int64 sys_write(int fd, const void *buf, uint64 n) {
    int64 ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(n)
                     : "rcx", "r11", "memory");
    return ret;
}

static uint64 slen(const char *s) { uint64 n = 0; while (s[n]) n++; return n; }
static void   put(const char *s)  { sys_write(1, s, slen(s)); }

static void puthex(uint64 v) {
    char b[19];
    const char *d = "0123456789ABCDEF";
    int i = 18;
    b[i--] = 0;
    if (!v) b[i--] = '0';
    while (v) { b[i--] = d[v & 0xF]; v >>= 4; }
    b[i--] = 'x'; b[i] = '0';
    put(&b[i]);
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- the relocation bait -------------------------------------------------
 * Each entry needs an R_X86_64_RELATIVE in .rela.dyn. A `static const char
 * *const` array lands in .data.rel.ro, which is exactly why user-pie.ld has to
 * name that section.
 */
static const char s_alpha[]   = "alpha";
static const char s_bravo[]   = "bravo";
static const char s_charlie[] = "charlie";

static int f_a(void) { return 0xA1; }
static int f_b(void) { return 0xB2; }
static int f_c(void) { return 0xC3; }

/* [3] points INTO an object rather than at its start, so its relocation carries
 * a non-zero addend: value = base + addend, not base + symbol. */
static const char *const strtab[] = { s_alpha, s_bravo, s_charlie, s_charlie + 4 };
static const char *const expect[] = { "alpha", "bravo", "charlie", "lie" };

static int (*const fntab[])(void) = { f_a, f_b, f_c };
static const int fnexp[] = { 0xA1, 0xB2, 0xC3 };

/* [1] points at its own array: correct under exactly one base, wrong under
 * every flat shift including "no relocation at all". */
static const void *const ptrtab[] = { (const void *)&strtab[0],
                                      (const void *)&ptrtab[0] };

extern char _start;

int pie_main(void);
int pie_main(void) {
    /* &_start is materialised RIP-relative under -fPIE, so this is the REAL
     * runtime base of the image, learned without asking the kernel. */
    uint64 base = (uint64)(unsigned long)&_start;
    uint64 span = 4u * 1024u * 1024u;   /* generous; the image is ~3KB */
    int fails = 0;
    unsigned i;

    put("[PIETEST] running at base=");
    puthex(base);
    put("\n");

    /* Phase 1: range. If the loader applied NOTHING these still hold link-time
     * values near 0. Nothing is dereferenced until this passes. */
    for (i = 0; i < 4; i++) {
        uint64 p = (uint64)(unsigned long)strtab[i];
        if (p < base || p >= base + span) {
            put("[PIETEST] FAIL strtab["); puthex(i); put("] = "); puthex(p);
            put(" is not inside the loaded image (unrelocated)\n");
            fails++;
        }
    }
    for (i = 0; i < 3; i++) {
        uint64 p = (uint64)(unsigned long)fntab[i];
        if (p < base || p >= base + span) {
            put("[PIETEST] FAIL fntab["); puthex(i); put("] = "); puthex(p);
            put(" is not inside the loaded image (unrelocated)\n");
            fails++;
        }
    }
    if (ptrtab[0] != (const void *)&strtab[0]) {
        put("[PIETEST] FAIL ptrtab[0] = ");
        puthex((uint64)(unsigned long)ptrtab[0]);
        put(" but &strtab[0] = ");
        puthex((uint64)(unsigned long)&strtab[0]); put("\n");
        fails++;
    }
    if (ptrtab[1] != (const void *)&ptrtab[0]) {
        put("[PIETEST] FAIL self-pointer ptrtab[1] = ");
        puthex((uint64)(unsigned long)ptrtab[1]);
        put(" but &ptrtab[0] = ");
        puthex((uint64)(unsigned long)&ptrtab[0]); put("\n");
        fails++;
    }
    if (fails) {
        put("[PIETEST] RESULT: FAIL (relocations not applied)\n");
        return 1;
    }

    /* Phase 2: the addresses are sane, so their contents can be trusted enough
     * to read and to call. */
    for (i = 0; i < 4; i++) {
        if (!streq(strtab[i], expect[i])) {
            put("[PIETEST] FAIL strtab["); puthex(i);
            put("] does not resolve to its string\n");
            fails++;
        }
    }
    for (i = 0; i < 3; i++) {
        if (fntab[i]() != fnexp[i]) {
            put("[PIETEST] FAIL call through relocated fntab["); puthex(i);
            put("] returned the wrong value\n");
            fails++;
        }
    }

    if (fails) {
        put("[PIETEST] RESULT: FAIL\n");
        return 1;
    }
    put("[PIETEST] RESULT: PASS - 4 string pointers (one with a non-zero "
        "addend, pointing into the middle of an object) and 3 function pointers "
        "read through a runtime index all land inside the image, resolve to "
        "their contents, and call correctly\n");
    return 0;
}
