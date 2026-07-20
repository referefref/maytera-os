// string.c - String functions implementation
#include "string.h"
#include "stdlib.h"

// Fast assembly implementations
extern void *memcpy_fast(void *dest, const void *src, size_t n);
extern void *memset_fast(void *dest, int c, size_t n);
extern void *memmove_fast(void *dest, const void *src, size_t n);



void *memset(void *s, int c, size_t n) {
    return memset_fast(s, c, n);
}

void *memcpy(void *dest, const void *src, size_t n) {
    return memcpy_fast(dest, src, n);
}

void *memmove(void *dest, const void *src, size_t n) {
    return memmove_fast(dest, src, n);
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char uc = (unsigned char)c;
    while (n--) {
        if (*p == uc) {
            return (void *)p;
        }
        p++;
    }
    return (void *)0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dest;
}

size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t src_len = strlen(src);
    if (size > 0) {
        size_t copy_len = (src_len >= size) ? size - 1 : src_len;
        memcpy(dest, src, copy_len);
        dest[copy_len] = '\0';
    }
    return src_len;
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
    if (n == 0) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline int tolower_char(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && (tolower_char(*s1) == tolower_char(*s2))) {
        s1++;
        s2++;
    }
    return tolower_char(*(unsigned char *)s1) - tolower_char(*(unsigned char *)s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (tolower_char(*s1) == tolower_char(*s2))) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return tolower_char(*(unsigned char *)s1) - tolower_char(*(unsigned char *)s2);
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && (*d++ = *src++));
    *d = '\0';
    return dest;
}

size_t strlcat(char *dest, const char *src, size_t size) {
    size_t dest_len = strlen(dest);
    size_t src_len = strlen(src);
    
    if (dest_len >= size) {
        return size + src_len;
    }
    
    size_t copy_len = size - dest_len - 1;
    if (copy_len > src_len) copy_len = src_len;
    
    memcpy(dest + dest_len, src, copy_len);
    dest[dest_len + copy_len] = '\0';
    
    return dest_len + src_len;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (void *)0;
}

char *strrchr(const char *s, int c) {
    const char *last = (void *)0;
    while (*s) {
        if (*s == c) last = s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        
        if (!*n) return (char *)haystack;
        haystack++;
    }
    return (void *)0;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        int found = 0;
        while (*a) {
            if (*s == *a) {
                found = 1;
                break;
            }
            a++;
        }
        if (!found) break;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r) {
            if (*s == *r) return count;
            r++;
        }
        count++;
        s++;
    }
    return count;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a) return (char *)s;
            a++;
        }
        s++;
    }
    return (void *)0;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *dup = malloc(len);
    if (dup) {
        memcpy(dup, s, len);
    }
    return dup;
}

char *strndup(const char *s, size_t n) {
    size_t len = strlen(s);
    if (len > n) len = n;
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}

// ===========================================================================
// Phase 1 (#422) additions: tokenizers and bounded helpers CPython and many
// ports need (strtok is used across configure-generated C and stdlib modules).
// ===========================================================================

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *s = str ? str : *saveptr;
    if (!s) return (void *)0;
    // skip leading delimiters
    while (*s && strchr(delim, *s)) s++;
    if (!*s) { *saveptr = s; return (void *)0; }
    char *tok = s;
    while (*s && !strchr(delim, *s)) s++;
    if (*s) { *s = '\0'; *saveptr = s + 1; }
    else    { *saveptr = s; }
    return tok;
}

char *strtok(char *str, const char *delim) {
    static char *saved;
    return strtok_r(str, delim, &saved);
}

char *strsep(char **stringp, const char *delim) {
    char *s = *stringp;
    if (!s) return (void *)0;
    char *p = s;
    while (*p && !strchr(delim, *p)) p++;
    if (*p) { *p = '\0'; *stringp = p + 1; }
    else    { *stringp = (void *)0; }
    return s;
}

void *memccpy(void *dest, const void *src, int c, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    unsigned char uc = (unsigned char)c;
    while (n--) {
        *d = *s;
        if (*s == uc) return d + 1;
        d++; s++;
    }
    return (void *)0;
}
