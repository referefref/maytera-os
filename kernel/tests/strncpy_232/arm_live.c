/* LIVE post-#231 kernel/string.c strncpy, verbatim. */
char *mos_strncpy(char *dest, const char *src, unsigned long n) {
    unsigned long i = 0;
    for (; i < n && src[i] != 0; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = 0;
    return dest;
}
