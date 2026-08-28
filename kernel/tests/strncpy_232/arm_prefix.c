/* FROZEN pre-#231 kernel/string.c strncpy, verbatim shape. */
char *mos_strncpy(char *dest, const char *src, unsigned long n) {
    char *d = dest;
    while (n && (*d++ = *src++)) { n--; }
    while (n--) { *d++ = 0; }
    return dest;
}
