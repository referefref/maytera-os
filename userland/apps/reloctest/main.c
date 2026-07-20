/* reloctest - #427 ELF relocation-engine proof.
 *
 * Built as a REAL -fPIC -pie ("static PIE": ET_DYN, PT_DYNAMIC with a
 * DT_RELA table, no PT_INTERP / no NEEDED libraries) ELF, deliberately
 * self-contained (no crt0.o / libc.a) so nothing else in the build can be
 * blamed for the result.
 *
 * g_funcs[] and g_strings[] are arrays of absolute pointers (to functions
 * and to string literals) stored in initialized data. Because the binary is
 * position-independent, the static linker cannot know the final runtime
 * addresses of say_hello/say_world/"ALPHA"/"BRAVO" at link time - it emits
 * R_X86_64_RELATIVE relocations in .rela.dyn (DT_RELA) that must be applied
 * by the loader once the real load base is known.
 *
 * Before the #427 fix, MayteraOS's ELF loader applied ZERO relocations for
 * PIE binaries: it only added a flat offset to segment vaddrs/entry point.
 * Under the OLD loader, g_funcs[]/g_strings[] would still contain the raw
 * link-time addresses (typically small, near 0, since this links as a
 * static-pie with no fixed base) - calling through them from the *real*
 * runtime load address would read unmapped/wrong memory and either fault
 * (process killed) or print garbage. Under the FIXED loader, the relocations
 * are applied and the output below is fully correct.
 */

typedef unsigned long ulong;

static long raw_syscall3(long num, long a1, long a2, long a3) {
    long ret;
    register long r10 __asm__("r10") = 0;
    (void)r10;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void sys_write(const char *buf, ulong len) {
    raw_syscall3(13 /* SYS_WRITE */, 1 /* fd stdout */, (long)buf, (long)len);
}

static void sys_exit(int code) {
    raw_syscall3(0 /* SYS_EXIT */, code, 0, 0);
    for (;;) { }
}

static ulong my_strlen(const char *s) {
    ulong n = 0;
    while (s[n]) n++;
    return n;
}

static void puts_(const char *s) {
    sys_write(s, my_strlen(s));
}

/* Targets of the pointer tables below. */
static void say_hello(void) { puts_("RELOC_OK: say_hello() called through g_funcs[0]\n"); }
static void say_world(void) { puts_("RELOC_OK: say_world() called through g_funcs[1]\n"); }

typedef void (*fnptr_t)(void);

/* Initialized data holding absolute pointers -> forces R_X86_64_RELATIVE
 * fixups in a PIE build. This is exactly the "global function-pointer
 * table" / "global string pointer" case #427 calls out. */
static const fnptr_t g_funcs[] = { say_hello, say_world, (fnptr_t)0 };
static const char *const g_strings[] = { "ALPHA", "BRAVO", (const char *)0 };

void _start(void) {
    puts_("RELOC_TEST v1: starting (PIE, PT_DYNAMIC present)\n");

    for (int i = 0; g_funcs[i]; i++) {
        g_funcs[i]();
    }

    for (int i = 0; g_strings[i]; i++) {
        puts_("RELOC_OK: g_strings[");
        char idx[2] = { (char)('0' + i), 0 };
        puts_(idx);
        puts_("] = ");
        puts_(g_strings[i]);
        puts_("\n");
    }

    puts_("RELOC_TEST v1: PASS - all pointer-table entries resolved correctly\n");
    sys_exit(42);
}
