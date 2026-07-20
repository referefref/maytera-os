
#ifndef COMPAT_STDIO_H
#define COMPAT_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF    (-1)
#define BUFSIZ 512

/* Very small FILE using a kernel fd */
typedef struct _FILE {
    int  fd;
    int  eof;
    int  error;
    char pbuf;   /* one-char pushback */
    int  has_pb;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* Core I/O */
FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *fp);
int   fflush(FILE *fp);
int   feof(FILE *fp);
int   ferror(FILE *fp);
int   fgetc(FILE *fp);
int   fputc(int c, FILE *fp);
int   ungetc(int c, FILE *fp);
char *fgets(char *s, int n, FILE *fp);
int   fputs(const char *s, FILE *fp);
long  fread(void *ptr, long size, long nmemb, FILE *fp);
long  fwrite(const void *ptr, long size, long nmemb, FILE *fp);
int   fseek(FILE *fp, long offset, int whence);
long  ftell(FILE *fp);
void  rewind(FILE *fp);

/* Formatted I/O */
int printf(const char *fmt, ...);
int fprintf(FILE *fp, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int sscanf(const char *buf, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *fp, const char *fmt, va_list ap);

void perror(const char *msg);

#define getc(f)   fgetc(f)
#define putc(c,f) fputc(c,f)

#endif /* COMPAT_STDIO_H */
