// libctest - exercises the Phase 1 libc completion (#422 / CPython #359).
// Runs as a background service (no PTY) so printf() goes to the kernel serial
// console. Prints a PASS/FAIL tally plus sample values.
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "ctype.h"
#include "math.h"
#include "time.h"

static int g_pass = 0, g_fail = 0;

static void ck(const char *what, int cond) {
    if (cond) { g_pass++; printf("  PASS %s\n", what); }
    else      { g_fail++; printf("  FAIL %s\n", what); }
}

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[128];

    printf("\n==== LIBCTEST_BEGIN ====\n");

    // ---- printf float ----
    snprintf(buf, sizeof buf, "%.4f", 3.14159265);   ck("printf %.4f=3.1416", streq(buf, "3.1416"));
    snprintf(buf, sizeof buf, "%g", 0.0001);          ck("printf %g=0.0001", streq(buf, "0.0001"));
    snprintf(buf, sizeof buf, "%e", 6.022e23);        ck("printf %e=6.022000e+23", streq(buf, "6.022000e+23"));
    snprintf(buf, sizeof buf, "%+08.2f", 3.5);        ck("printf %+08.2f=+0003.50", streq(buf, "+0003.50"));
    snprintf(buf, sizeof buf, "%#o %#x", 64, 255);    ck("printf octal/hex alt", streq(buf, "0100 0xff"));
    snprintf(buf, sizeof buf, "%5.2f|%-5.2f|", 1.5, 1.5); ck("printf width/left", streq(buf, " 1.50|1.50 |"));
    printf("  sample: pi=%.10f e=%.6g big=%e\n", 3.14159265358979, 2.718281828, 1.5e300);

    // ---- strtod ----
    { char *e; double d = strtod("2.5e3xyz", &e);
      ck("strtod value", d == 2500.0);
      ck("strtod endptr", streq(e, "xyz")); }
    { double d = strtod("0.1", 0); ck("strtod 0.1", d > 0.0999999 && d < 0.1000001); }
    ck("atof", atof("-42.25") == -42.25);

    // ---- strtol / strtoll ----
    ck("strtol hex", strtol("0xFF", 0, 0) == 255);
    ck("strtol neg", strtol("  -123", 0, 10) == -123);
    ck("strtoll big", strtoll("9223372036854775807", 0, 10) == 9223372036854775807LL);

    // ---- qsort ----
    { int a[10] = {5,3,8,1,9,2,7,4,6,0};
      qsort(a, 10, sizeof(int), cmp_int);
      int ok = 1; for (int i = 0; i < 10; i++) if (a[i] != i) ok = 0;
      ck("qsort sorts 0..9", ok); }

    // ---- getenv / setenv ----
    ck("getenv unset==NULL", getenv("NOPE_XYZ") == 0);
    setenv("PYTHONHOME", "/lib/python", 1);
    ck("setenv/getenv roundtrip", getenv("PYTHONHOME") && streq(getenv("PYTHONHOME"), "/lib/python"));
    setenv("PYTHONHOME", "/other", 0);   // no-overwrite
    ck("setenv no-overwrite", streq(getenv("PYTHONHOME"), "/lib/python"));
    unsetenv("PYTHONHOME");
    ck("unsetenv", getenv("PYTHONHOME") == 0);

    // ---- ctype ----
    ck("isdigit", isdigit('7') && !isdigit('a'));
    ck("isalpha", isalpha('Z') && !isalpha('9'));
    ck("isspace", isspace(' ') && isspace('\t') && !isspace('x'));
    ck("toupper/tolower", toupper('a') == 'A' && tolower('X') == 'x');
    ck("isxdigit", isxdigit('F') && isxdigit('9') && !isxdigit('g'));

    // ---- malloc alignment ----
    { int aligned = 1;
      for (int i = 1; i <= 64; i++) {
          void *p = malloc(i * 7 + 3);
          if (((unsigned long)p & 15) != 0) aligned = 0;
      }
      ck("malloc 16-byte aligned", aligned); }
    { void *p = malloc(100); void *q = realloc(p, 5000);
      ck("realloc grows", q != 0); free(q); }

    // ---- math ----
    ck("sqrt", fabs(sqrt(2.0) - 1.41421356237) < 1e-9);
    ck("pow", fabs(pow(2.0, 10.0) - 1024.0) < 1e-6);
    ck("log/exp", fabs(exp(log(5.0)) - 5.0) < 1e-9);
    ck("log10", fabs(log10(1000.0) - 3.0) < 1e-9);
    ck("sin/cos", fabs(sin(0.0)) < 1e-12 && fabs(cos(0.0) - 1.0) < 1e-12);
    ck("round", round(2.5) == 3.0 && round(-2.5) == -3.0);
    ck("trunc/floor/ceil", trunc(3.7) == 3.0 && floor(-1.2) == -2.0 && ceil(1.1) == 2.0);
    ck("hypot", fabs(hypot(3.0, 4.0) - 5.0) < 1e-9);
    ck("copysign", copysign(3.0, -1.0) == -3.0);
    ck("isnan/isinf", isnan(0.0/0.0) && isinf(1.0/0.0) && !isnan(1.0));
    printf("  sample: sqrt2=%.6f pow2_10=%.1f atan2=%.6f\n", sqrt(2.0), pow(2.0,10.0), atan2(1.0,1.0));

    // ---- time / strftime ----
    { time_t t = 1234567890;   // 2009-02-13 23:31:30 UTC
      struct tm *tm = gmtime(&t);
      strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", tm);
      ck("gmtime/strftime", streq(buf, "2009-02-13 23:31:30"));
      ck("mktime roundtrip", mktime(tm) == 1234567890);
      printf("  sample: 1234567890 -> %s\n", buf);
      strftime(buf, sizeof buf, "%A %B %d", tm);
      printf("  sample: %s\n", buf); }

    // ---- sscanf ----
    { int a=0,b=0; double f=0; char word[32]={0};
      int n = sscanf("42 -7 3.5 hello", "%d %d %lf %s", &a, &b, &f, word);
      ck("sscanf count", n == 4);
      ck("sscanf ints", a == 42 && b == -7);
      ck("sscanf float", f > 3.49 && f < 3.51);
      ck("sscanf string", streq(word, "hello")); }
    { unsigned x=0; sscanf("0x1F3", "%x", &x); ck("sscanf hex", x == 0x1F3); }

    // ---- string ----
    { char s[] = "a,bb,,ccc"; char *save; int cnt = 0;
      char *tok = strtok_r(s, ",", &save);
      const char *exp[] = {"a","bb","ccc"};
      int ok = 1;
      while (tok) { if (cnt < 3 && !streq(tok, exp[cnt])) ok = 0; cnt++; tok = strtok_r(0, ",", &save); }
      ck("strtok_r", ok && cnt == 3); }
    ck("strnlen", strnlen("hello", 3) == 3 && strnlen("hi", 10) == 2);

    printf("==== LIBCTEST_RESULT pass=%d fail=%d ====\n", g_pass, g_fail);
    printf("==== LIBCTEST_END ====\n\n");
    return 0;
}
