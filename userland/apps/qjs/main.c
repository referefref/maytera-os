/* userland/apps/qjs - the QuickJS-ng command-line JavaScript engine, /APPS/QJS
 * (#745, Tier 2 item #8 of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md).
 *
 * This is a small, first-party CLI over libquickjs.a (built by the mports recipe
 * at userland/ports/quickjs). It deliberately does NOT use upstream's qjs.c,
 * because that main() depends on gen/repl.c and gen/standalone.c - C sources the
 * upstream build GENERATES by running the qjsc bytecode compiler on repl.js at
 * build time. Reproducing that would need a host-side qjsc bootstrap for no gain
 * here: this CLI provides its own file/eval/REPL driver and a print binding, and
 * links only the pure-computation engine (no quickjs-libc std/os OS surface).
 *
 * Usage:
 *   qjs FILE.js            evaluate a script file
 *   qjs -e 'CODE'          evaluate CODE from the argument
 *   qjs                    read-eval-print loop on stdin (line at a time)
 *   qjs -h | --help        usage
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

/* print(...) / console.log(...) / console.error(...): space-separated args,
 * trailing newline, to stdout. */
static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        if (i) putchar(' ');
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) return JS_EXCEPTION;
        fputs(s, stdout);
        JS_FreeCString(ctx, s);
    }
    putchar('\n');
    return JS_UNDEFINED;
}

static void install_globals(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "print",
                      JS_NewCFunction(ctx, js_print, "print", 1));
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_print, "log", 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_print, "error", 1));
    JS_SetPropertyStr(ctx, console, "info",
                      JS_NewCFunction(ctx, js_print, "info", 1));
    JS_SetPropertyStr(ctx, console, "warn",
                      JS_NewCFunction(ctx, js_print, "warn", 1));
    JS_SetPropertyStr(ctx, g, "console", console);
    JS_FreeValue(ctx, g);
}

/* Print a pending exception (message + stack) to stderr. */
static void dump_exception(JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, exc);
    if (s) {
        fprintf(stderr, "%s\n", s);
        JS_FreeCString(ctx, s);
    } else {
        fprintf(stderr, "(uncaught exception, not stringifiable)\n");
    }
    if (JS_IsError(exc)) {
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *st = JS_ToCString(ctx, stack);
            if (st) {
                fprintf(stderr, "%s\n", st);
                JS_FreeCString(ctx, st);
            }
        }
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, exc);
}

/* Evaluate one buffer. Returns 0 on success, -1 on an uncaught exception.
 * If echo_result is nonzero (the REPL), the result value is printed. */
static int eval_buf(JSContext *ctx, const char *buf, size_t len,
                    const char *filename, int echo_result)
{
    JSValue val = JS_Eval(ctx, buf, len, filename, JS_EVAL_TYPE_GLOBAL);
    int rc = 0;
    if (JS_IsException(val)) {
        dump_exception(ctx);
        rc = -1;
    } else if (echo_result && !JS_IsUndefined(val)) {
        const char *s = JS_ToCString(ctx, val);
        if (s) {
            printf("%s\n", s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, val);
    return rc;
}

/* Read an entire file into a NUL-terminated malloc buffer. */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static int run_file(JSContext *ctx, const char *path)
{
    size_t len;
    char *buf = read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "qjs: cannot read %s\n", path);
        return -1;
    }
    /* Skip a leading shebang line so a #!/APPS/QJS script evaluates. */
    char *src = buf;
    if (len >= 2 && src[0] == '#' && src[1] == '!') {
        char *nl = strchr(src, '\n');
        if (nl) { size_t skip = (size_t)(nl - src); src = nl; len -= skip; }
    }
    int rc = eval_buf(ctx, src, len, path, 0);
    free(buf);
    return rc;
}

static int run_repl(JSContext *ctx)
{
    char line[4096];
    fputs("QuickJS-ng (MayteraOS). Type JavaScript; Ctrl-D to exit.\n", stdout);
    for (;;) {
        fputs("qjs> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;                     /* EOF */
        size_t len = strlen(line);
        if (len == 0) continue;
        /* Evaluate the line; a syntax/runtime error is reported but the REPL
         * keeps going, which is the whole point of a REPL. */
        eval_buf(ctx, line, len, "<repl>", 1);
    }
    fputs("\n", stdout);
    return 0;
}

static void usage(void)
{
    fputs("usage: qjs [FILE.js]        evaluate a script file\n"
          "       qjs -e 'CODE'        evaluate CODE\n"
          "       qjs                  read-eval-print loop on stdin\n"
          "       qjs -h | --help      this message\n", stdout);
}

int main(int argc, char **argv)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "qjs: JS_NewRuntime failed\n"); return 1; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "qjs: JS_NewContext failed\n"); JS_FreeRuntime(rt); return 1; }
    install_globals(ctx);

    int rc = 0, handled = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            handled = 1;
            break;
        } else if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--eval")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "qjs: %s needs an argument\n", argv[i]);
                rc = 1;
            } else {
                const char *code = argv[++i];
                rc = eval_buf(ctx, code, strlen(code), "<cmdline>", 0) ? 1 : rc;
            }
            handled = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "qjs: unknown option %s\n", argv[i]);
            usage();
            rc = 1;
            handled = 1;
            break;
        } else {
            rc = run_file(ctx, argv[i]) ? 1 : rc;
            handled = 1;
        }
    }

    if (!handled)
        rc = run_repl(ctx);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}
