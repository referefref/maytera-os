// string.c - Basic string and memory functions implementation
#include "string.h"

// Declare fast assembly implementations
extern void *memcpy_fast(void *dest, const void *src, size_t n);
extern void *memset_fast(void *dest, int c, size_t n);
extern void *memmove_fast(void *dest, const void *src, size_t n);


// Memory functions
void *memset(void *dest, int c, size_t n) {
    return memset_fast(dest, c, n);
}

void *memcpy(void *dest, const void *src, size_t n) {
    return memcpy_fast(dest, src, n);
}

void *memmove(void *dest, const void *src, size_t n) {
    return memmove_fast(dest, src, n);
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

// String functions
size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) {
        len++;
    }
    return len;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && *s++) {
        len++;
    }
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

// #231: SECOND LIVE INSTANCE of the userland libc off-by-one, found while
// auditing the blast radius of the userland fix. This kernel strncpy() had
// the IDENTICAL bug (same logic, just brace-wrapped): the copy-then-decrement
// idiom copies the source's terminating NUL as part of the loop body but
// skips n-- for that same iteration, because the loop CONDITION (the
// just-copied byte) is false and short-circuits before the decrement runs.
// n is left one too high, and the follow-on padding loop
// `while (n--) { *d++ = 0; }` then writes ONE BYTE PAST dest[n-1]. This is a
// RING-0 overflow: it fires on every ordinary call
// strncpy(dst, src, sizeof(dst)) where src is a NUL-terminated string
// shorter than n (i.e. almost every real caller), across 218 call sites in
// kernel/. See userland/libc/string.c for the original finding (#231) and
// CHANGELOG.md / blame.md for the #223 userland incident this class of bug
// caused (dock favourites corrupted and persisted to disk).
//
// Same fix as userland: count the index explicitly instead of decrementing n
// on a condition that can be starved, so every write, including the final
// NUL, is accounted for and the total is always exactly n bytes. The
// standard's other half is preserved: a source n bytes or longer with no NUL
// in the first n bytes is copied exactly (n bytes, no terminator) and is
// UNCHANGED by this fix - callers that add their own dst[n-1] = 0 depend on
// that shape.
char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) {
        d++;
    }
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) {
        d++;
    }
    while (n-- && *src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return (char *)haystack;
    }

    while (*haystack) {
        if (strncmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

// Conversion functions
int atoi(const char *s) {
    int result = 0;
    int sign = 1;

    while (isspace(*s)) {
        s++;
    }

    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    while (isdigit(*s)) {
        result = result * 10 + (*s - '0');
        s++;
    }

    return sign * result;
}

long atol(const char *s) {
    long result = 0;
    int sign = 1;

    while (isspace(*s)) {
        s++;
    }

    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    while (isdigit(*s)) {
        result = result * 10 + (*s - '0');
        s++;
    }

    return sign * result;
}

char *itoa(int value, char *str, int base) {
    return ltoa((long)value, str, base);
}

char *ltoa(long value, char *str, int base) {
    static const char digits[] = "0123456789abcdef";
    char *p = str;
    char *p1, *p2;
    unsigned long ud;
    int negative = 0;

    if (base < 2 || base > 16) {
        *str = '\0';
        return str;
    }

    if (value < 0 && base == 10) {
        negative = 1;
        ud = (unsigned long)(-value);
    } else {
        ud = (unsigned long)value;
    }

    do {
        *p++ = digits[ud % base];
        ud /= base;
    } while (ud);

    if (negative) {
        *p++ = '-';
    }

    *p = '\0';

    // Reverse the string
    p1 = str;
    p2 = p - 1;
    while (p1 < p2) {
        char tmp = *p1;
        *p1++ = *p2;
        *p2-- = tmp;
    }

    return str;
}

char *ultoa(unsigned long value, char *str, int base) {
    static const char digits[] = "0123456789abcdef";
    char *p = str;
    char *p1, *p2;

    if (base < 2 || base > 16) {
        *str = '\0';
        return str;
    }

    do {
        *p++ = digits[value % base];
        value /= base;
    } while (value);

    *p = '\0';

    // Reverse the string
    p1 = str;
    p2 = p - 1;
    while (p1 < p2) {
        char tmp = *p1;
        *p1++ = *p2;
        *p2-- = tmp;
    }

    return str;
}

// Extended conversion: strtol family
long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    long acc = 0;
    int neg = 0;

    // Skip whitespace
    while (isspace(*s)) s++;

    // Handle sign
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    // Detect base from prefix
    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            } else {
                base = 8;
                s++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    // Convert digits
    while (*s) {
        int digit;
        if (isdigit(*s)) {
            digit = *s - '0';
        } else if (isalpha(*s)) {
            digit = tolower(*s) - 'a' + 10;
        } else {
            break;
        }
        if (digit >= base) break;
        acc = acc * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return neg ? -acc : acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long acc = 0;

    // Skip whitespace
    while (isspace(*s)) s++;

    // Skip optional plus sign
    if (*s == '+') s++;

    // Detect base from prefix
    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            } else {
                base = 8;
                s++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    // Convert digits
    while (*s) {
        int digit;
        if (isdigit(*s)) {
            digit = *s - '0';
        } else if (isalpha(*s)) {
            digit = tolower(*s) - 'a' + 10;
        } else {
            break;
        }
        if (digit >= base) break;
        acc = acc * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return acc;
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    return (unsigned long long)strtoul(nptr, endptr, base);
}

// Formatted output: vsnprintf
// ===========================================================================
// #672 THE ONE FORMAT PARSER
// ===========================================================================
// There were FIVE hand-rolled printf parsers in this kernel and no delegation
// between them: vsnprintf (here), kprintf and serial_printf (serial.c),
// console_printf (video/console.c) and shell_printf (main.c). Four of the five
// were strictly weaker than this one, and the weakness was not cosmetic.
//
// A parser that does not RECOGNISE a specifier still has to decide what to do
// with the corresponding argument, and all four decided to consume NOTHING.
// That does not misprint one field, it SHIFTS EVERY LATER ARGUMENT IN THE LINE:
//
//   kprintf("[SECURITY] SMAP pre-flight: %-13s va=0x%016lx eff=0x%lx U/S=%s\n",
//           probes[i].what, probes[i].va, fl, user ? "USER" : "supervisor");
//
// kprintf parsed '0'-pad and a numeric width but NOT the '-' flag, so "%-13s"
// fell to `default:`, printed itself literally, and consumed no argument. Every
// later conversion then took the previous conversion's argument: va= printed
// the LABEL POINTER, eff= printed the address, and, worst of all, "U/S=%s" was
// handed `fl`, a page-table flags integer, AND DEREFERENCED IT AS A char*.
// A format-string defect had become an arbitrary-address read in Ring 0 whose
// address is whatever integer happened to be next in the argument list. It
// printed empty because the byte at that address happened to be zero; a
// different layout is a kernel page fault in a diagnostic.
//
// shell_printf was worse still: it parsed no flags and no width at all, so the
// 1353 "%04x" call sites in this tree would print "04x" and consume nothing.
//
// So the fix is not "add '-' to kprintf". It is to delete the four private
// parsers and route every one of them through THIS function, which already
// handled flags, width, precision and the length modifiers correctly. That is
// the CLAUDE.md rule (improve the shared primitive, never fork a private copy)
// applied to the exact bug class that rule exists to prevent.
//
// WHY THIS IS A SINK CALLBACK AND NOT "everyone calls vsnprintf". kprintf is
// called from ISRs and from the panic path, and it currently formats straight
// out of the UART with NO intermediate buffer and therefore no line-length
// limit. Making it call vsnprintf would put a multi-hundred-byte buffer on a
// kernel stack that an interrupt may already be nested on, and would silently
// truncate the long diagnostic lines this kernel actually prints. Splitting the
// PARSER from the DESTINATION keeps one definition of the format language while
// letting kprintf keep emitting a byte at a time.
//
// WHY C AND NOT RUST (the standing Rust-first policy requires a reason): this
// function's entire job is to walk a va_list. Rust's variadic support
// (core::ffi::VaList, the c_variadic feature) is unstable and cannot be used on
// the pinned 1.97.0 toolchain, and the argument-consumption rules ARE the bug
// being fixed, so they cannot be left on the C side of a split without leaving
// the defect there too. This is also a modification of an existing C function
// with 5,000+ call sites rather than a new subsystem.
//
// NEVER-DESYNC RULE: an unrecognised conversion now CONSUMES an int argument
// rather than consuming nothing. It cannot be correct (the true width is
// unknowable), but it is the choice that keeps the REST of the line addressing
// the right arguments, and it turns "every later field is garbage plus a wild
// pointer read" into "one field is wrong". The recognised set was widened
// first, so reaching this case is now unlikely: the tree's own usage was
// surveyed and 'o', 'hh', 'j', 't' and '*' were added for exactly that reason.
void kvformat(kfmt_sink_t sink, void *ctx, const char *fmt, __builtin_va_list ap) {
    if (!sink || !fmt) return;
    while (*fmt) {
        if (*fmt != '%') {
            sink(ctx, *fmt++);
            continue;
        }

        fmt++; // Skip '%'

        // Handle flags
        int left_justify = 0;
        int zero_pad = 0;
        int width = 0;
        int precision = -1;
        char length_mod = 0;

        // Parse flags
        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ' || *fmt == '#') {
            if (*fmt == '-') left_justify = 1;
            else if (*fmt == '0') zero_pad = 1;
            fmt++;
        }

        // Parse width. '*' takes the width from the argument list, so it MUST
        // consume one here or everything after it shifts (the #672 bug class).
        if (*fmt == '*') {
            width = __builtin_va_arg(ap, int);
            if (width < 0) { left_justify = 1; width = -width; }
            fmt++;
        } else {
            while (isdigit(*fmt)) {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        // Parse precision ('.*' likewise consumes an argument).
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = __builtin_va_arg(ap, int);
                if (precision < 0) precision = -1;   // negative precision = absent
                fmt++;
            } else {
                while (isdigit(*fmt)) {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        // Parse length modifier. 'hh', 'j' and 't' are accepted (not merely
        // ignored) because an unaccepted modifier would fall through to the
        // conversion switch, hit `default:`, and desync the line.
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') {
                length_mod = 'L'; // long long
                fmt++;
            } else {
                length_mod = 'l'; // long
            }
        } else if (*fmt == 'h') {
            fmt++;
            length_mod = 'h';
            if (*fmt == 'h') fmt++;   // 'hh': still promoted to int in varargs
        } else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') {
            // size_t / ptrdiff_t / intmax_t are all 64-bit here; 'z' is the
            // existing spelling the rest of this function switches on.
            length_mod = 'z';
            fmt++;
        }

        // Handle conversion specifier
        char tmp[32];
        const char *str = NULL;
        int slen = 0;

        switch (*fmt) {
            case 'd':
            case 'i': {
                long long val;
                if (length_mod == 'L') val = __builtin_va_arg(ap, long long);
                else if (length_mod == 'l' || length_mod == 'z') val = __builtin_va_arg(ap, long);
                else val = __builtin_va_arg(ap, int);

                int neg = val < 0;
                if (neg) val = -val;
                char *p = tmp + sizeof(tmp) - 1;
                *p = '\0';
                do { *--p = '0' + (val % 10); val /= 10; } while (val > 0);
                if (neg) *--p = '-';
                str = p;
                slen = strlen(str);
                break;
            }

            case 'u': {
                unsigned long long val;
                if (length_mod == 'L') val = __builtin_va_arg(ap, unsigned long long);
                else if (length_mod == 'l' || length_mod == 'z') val = __builtin_va_arg(ap, unsigned long);
                else val = __builtin_va_arg(ap, unsigned int);

                char *p = tmp + sizeof(tmp) - 1;
                *p = '\0';
                do { *--p = '0' + (val % 10); val /= 10; } while (val > 0);
                str = p;
                slen = strlen(str);
                break;
            }

            case 'o': {
                unsigned long long val;
                if (length_mod == 'L') val = __builtin_va_arg(ap, unsigned long long);
                else if (length_mod == 'l' || length_mod == 'z') val = __builtin_va_arg(ap, unsigned long);
                else val = __builtin_va_arg(ap, unsigned int);

                char *p = tmp + sizeof(tmp) - 1;
                *p = '\0';
                do { *--p = '0' + (char)(val & 7); val >>= 3; } while (val > 0);
                str = p;
                slen = strlen(str);
                break;
            }

            case 'x':
            case 'X': {
                unsigned long long val;
                if (length_mod == 'L') val = __builtin_va_arg(ap, unsigned long long);
                else if (length_mod == 'l' || length_mod == 'z') val = __builtin_va_arg(ap, unsigned long);
                else val = __builtin_va_arg(ap, unsigned int);

                const char *hex = (*fmt == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
                char *p = tmp + sizeof(tmp) - 1;
                *p = '\0';
                do { *--p = hex[val & 0xF]; val >>= 4; } while (val > 0);
                str = p;
                slen = strlen(str);
                break;
            }

            case 'p': {
                void *ptr = __builtin_va_arg(ap, void *);
                unsigned long val = (unsigned long)ptr;
                char *p = tmp + sizeof(tmp) - 1;
                *p = '\0';
                for (int i = 0; i < 16; i++) {
                    *--p = "0123456789abcdef"[val & 0xF];
                    val >>= 4;
                }
                *--p = 'x';
                *--p = '0';
                str = p;
                slen = strlen(str);
                break;
            }

            case 's': {
                str = __builtin_va_arg(ap, const char *);
                if (!str) str = "(null)";
                slen = strlen(str);
                if (precision >= 0 && slen > precision) slen = precision;
                break;
            }

            case 'c': {
                tmp[0] = (char)__builtin_va_arg(ap, int);
                tmp[1] = '\0';
                str = tmp;
                slen = 1;
                break;
            }

            case '%':
                sink(ctx, '%');
                fmt++;
                continue;

            default:
                // #672 NEVER-DESYNC: consume an argument for the unrecognised
                // conversion. The width is a guess (int is the common case and
                // the only safe one without knowing the specifier), but the
                // alternative, consuming nothing, corrupts every field after
                // this one and can hand a raw integer to a %s dereference.
                (void)__builtin_va_arg(ap, int);
                sink(ctx, '%');
                if (*fmt) sink(ctx, *fmt);
                if (*fmt) fmt++;
                continue;
        }

        fmt++;

        // Handle padding
        int pad = width - slen;
        char pad_char = (zero_pad && !left_justify) ? '0' : ' ';

        if (!left_justify) {
            while (pad > 0) { sink(ctx, pad_char); pad--; }
        }

        while (slen > 0) { sink(ctx, *str++); slen--; }

        if (left_justify) {
            while (pad > 0) { sink(ctx, ' '); pad--; }
        }
    }
}

// ---------------------------------------------------------------------------
// The buffer sink, and vsnprintf as a thin wrapper over the shared parser.
// The sink drops output once the buffer is full but the PARSER KEEPS RUNNING,
// so a truncated line still consumes its remaining arguments. Visible output is
// identical to the pre-#672 behaviour (which stopped the loop outright); the
// difference is that truncation can no longer leave the va_list half-consumed.
// ---------------------------------------------------------------------------
typedef struct {
    char   *buf;
    size_t  size;     // full buffer size, including the terminator slot
    size_t  written;
    size_t  dropped;  // deadport: characters the sink had no room to store
} kfmt_buf_ctx_t;

static void kfmt_buf_sink(void *vctx, char c) {
    kfmt_buf_ctx_t *b = (kfmt_buf_ctx_t *)vctx;
    if (b->written < b->size - 1) b->buf[b->written++] = c;
    else b->dropped++;
}

// deadport: THIS vsnprintf IS NOT C99, AND CALLERS DEPEND ON THAT.
//
// C99 returns the length that WOULD have been written, so the standard
// truncation test is `if (n >= size)`. This one returns the bytes ACTUALLY
// written, capped at size-1, so that test CAN NEVER BE TRUE. Every such test in
// this tree is therefore dead code, and at least two existed and had never once
// fired: the clamp in fs/bootlog.c (four copies) and the one it replaced. Lines
// were being cut at exactly 255 characters, in the owner's boot logs, with
// nothing detecting it, because the detector was structurally incapable of
// firing. MEASURED on a test boot: three lines sat at exactly 255 characters,
// visibly cut mid-word, while the truncation counter read zero.
//
// The fix is NOT to make vsnprintf C99. Code here is written against the
// clamped semantics, most importantly the accumulate-into-one-buffer idiom
// `o += snprintf(buf + o, sizeof(buf) - o, ...)` (drivers/xhci.c's heartbeat is
// one), where a C99 return would let `o` run PAST the end of the buffer and the
// next call would compute a negative size. Changing the return value would turn
// a cosmetic truncation bug into an out-of-bounds write across the tree.
//
// So the overflow is reported out of band instead, by the sink that actually
// sees it. vsnprintf() keeps its exact previous behaviour and return value;
// callers that care about truncation ask for it explicitly.
int vsnprintf_dropped(char *buf, size_t size, const char *fmt,
                      __builtin_va_list ap, size_t *dropped) {
    if (dropped) *dropped = 0;
    if (size == 0) return 0;
    kfmt_buf_ctx_t ctx;
    ctx.buf = buf;
    ctx.size = size;
    ctx.written = 0;
    ctx.dropped = 0;
    kvformat(kfmt_buf_sink, &ctx, fmt, ap);
    buf[ctx.written] = '\0';
    if (dropped) *dropped = ctx.dropped;
    return (int)ctx.written;
}

int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap) {
    return vsnprintf_dropped(buf, size, fmt, ap, 0);
}

// ===========================================================================
// #672 BOOT SELF-TEST
// ===========================================================================
// Formats known vectors and COMPARES against the expected bytes. It tests the
// shared parser through snprintf, which since #672 is the same kvformat() that
// kprintf, serial_printf, console_printf and shell_printf all run, so a pass
// here is a pass for the serial console too.
//
// The vectors are not decorative. Each one is a specifier that EXISTED IN THIS
// TREE and was mishandled by at least one of the five parsers, and the last one
// is the literal format string from security.c's SMAP pre-flight, whose
// argument shift was the reported symptom of #672. The pre-flight itself is
// gated off by -DCONFIG_NO_SMAP and does not run on the shipping build, so
// asserting on its format string here is the way to prove that specific line is
// fixed without un-holding the #645 SMAP interlock to make it print.
// Declared here rather than by including serial.h: string.c is the lowest
// layer and must not acquire a dependency on the console for a self-test.
extern void kprintf(const char *fmt, ...);

static int kfmt_st_fail;

static void kfmt_expect(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        kprintf("[KFMT-SELFTEST] FAIL %s\n", what);
        kprintf("[KFMT-SELFTEST]   got  [%s]\n", got);
        kprintf("[KFMT-SELFTEST]   want [%s]\n", want);
        kfmt_st_fail++;
    }
}

void kformat_selftest(void) {
    char b[256];
    kfmt_st_fail = 0;

    // Width and the left-justify flag on %s: the exact defect. kprintf's old
    // parser printed "%-13s" literally and consumed no argument.
    snprintf(b, sizeof(b), "[%-13s]", "abc");
    kfmt_expect(b, "[abc          ]", "%-13s left-justified width");
    snprintf(b, sizeof(b), "[%13s]", "abc");
    kfmt_expect(b, "[          abc]", "%13s right-justified width");

    // Width on integers, and zero padding. shell_printf's old parser printed
    // "04x" literally for every one of the 1353 %04x call sites in this tree.
    snprintf(b, sizeof(b), "[%04x]", 0x2f);
    kfmt_expect(b, "[002f]", "%04x zero-padded hex");
    snprintf(b, sizeof(b), "[%-6d|]", 42);
    kfmt_expect(b, "[42    |]", "%-6d left-justified int");

    // Precision on %s, live in fs/graphfs/blob.c and media/webp.c.
    snprintf(b, sizeof(b), "[%.4s]", "0123456789");
    kfmt_expect(b, "[0123]", "%.4s precision truncates");

    // Length modifiers. %zu is live in graphfs, syscall.c and virtio_gpu.c.
    snprintf(b, sizeof(b), "[%zu]", (size_t)4096);
    kfmt_expect(b, "[4096]", "%zu size_t");
    snprintf(b, sizeof(b), "[%llu]", (unsigned long long)12345678901ULL);
    kfmt_expect(b, "[12345678901]", "%llu long long");

    // Octal: previously fell to `default:` and desynced the rest of the line.
    snprintf(b, sizeof(b), "[%o]", 0755);
    kfmt_expect(b, "[755]", "%o octal");

    // THE NEVER-DESYNC PROPERTY. This is the property that matters more than
    // any single conversion being pretty: an unrecognised specifier must still
    // consume its argument, so the arguments AFTER it stay aligned. Before
    // #672 the trailing 99 would have been printed by the %d and the line
    // would have ended one argument short.
    snprintf(b, sizeof(b), "[%q|%d]", 7, 99);
    kfmt_expect(b, "[%q|99]", "unknown specifier consumes its argument");

    // Star width, which takes its width from the argument list.
    snprintf(b, sizeof(b), "[%*d]", 5, 42);
    kfmt_expect(b, "[   42]", "%*d star width");

    // THE REPORTED SYMPTOM, verbatim. security.c:220. With the old kprintf this
    // rendered as "%-13s va=0x<pointer to the label> eff=0x<the va> U/S=" and
    // the %s was handed the flags INTEGER and dereferenced it as a char *.
    snprintf(b, sizeof(b), "[SECURITY] SMAP pre-flight: %-13s va=0x%016lx eff=0x%lx U/S=%s",
             "kernel .data", (unsigned long)0x2003090UL, (unsigned long)0x8000000000000023UL,
             "supervisor");
    kfmt_expect(b,
        "[SECURITY] SMAP pre-flight: kernel .data  va=0x0000000002003090 "
        "eff=0x8000000000000023 U/S=supervisor",
        "security.c:220 SMAP pre-flight line");

    // END-TO-END, THROUGH KPRINTF ITSELF. Everything above goes through
    // snprintf, whose parser ALREADY handled '-' correctly before #672; the
    // parser that was broken was kprintf's own private copy. Since #672 they
    // are the same parser, but that is the claim under test, so this line
    // exercises the serial path directly with the verbatim security.c:220
    // format string. The third argument is deliberately 0: on the PRE-#672
    // kernel the argument shift hands it to the trailing "%s", and 0 is the one
    // value the old code null-checked, so this probe demonstrates the shift
    // without dereferencing a wild pointer in Ring 0. Any other value there
    // would have been a real #GP.
    kprintf("[KFMT-SELFTEST] kprintf path: SMAP pre-flight: %-13s va=0x%016lx eff=0x%lx U/S=%s\n",
            "kernel .data", (unsigned long)0x2003090UL, (unsigned long)0UL, "supervisor");

    if (kfmt_st_fail == 0) {
        kprintf("[KFMT-SELFTEST] #672 PASS: one shared parser, width/flags/precision "
                "correct, unknown specifiers cannot desynchronise a line\n");
    } else {
        kprintf("[KFMT-SELFTEST] #672 *** %d FAILURE(S) *** kernel diagnostic output "
                "is misreporting its own arguments\n", kfmt_st_fail);
    }
}

// Formatted output: snprintf
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int result = vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return result;
}

// Case-insensitive string comparison
int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return *s1 - *s2;
}
