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

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*d++ = *src++)) {
        n--;
    }
    while (n--) {
        *d++ = '\0';
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
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap) {
    size_t written = 0;

    if (size == 0) return 0;

    while (*fmt && written < size - 1) {
        if (*fmt != '%') {
            buf[written++] = *fmt++;
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

        // Parse width
        while (isdigit(*fmt)) {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        // Parse precision
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (isdigit(*fmt)) {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        // Parse length modifier
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') {
                length_mod = 'L'; // long long
                fmt++;
            } else {
                length_mod = 'l'; // long
            }
        } else if (*fmt == 'h') {
            length_mod = 'h';
            fmt++;
        } else if (*fmt == 'z') {
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
                else if (length_mod == 'l') val = __builtin_va_arg(ap, long);
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
                buf[written++] = '%';
                fmt++;
                continue;

            default:
                buf[written++] = '%';
                if (written < size - 1) buf[written++] = *fmt;
                fmt++;
                continue;
        }

        fmt++;

        // Handle padding
        int pad = width - slen;
        char pad_char = (zero_pad && !left_justify) ? '0' : ' ';

        if (!left_justify) {
            while (pad > 0 && written < size - 1) {
                buf[written++] = pad_char;
                pad--;
            }
        }

        // Copy string
        while (slen > 0 && written < size - 1) {
            buf[written++] = *str++;
            slen--;
        }

        // Right padding
        if (left_justify) {
            while (pad > 0 && written < size - 1) {
                buf[written++] = ' ';
                pad--;
            }
        }
    }

    buf[written] = '\0';
    return (int)written;
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
