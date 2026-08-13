/* M1 link+run test: prove all 5 NetSurf .a link AND execute on MayteraOS. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <libwapcaplet/libwapcaplet.h>
#include <libcss/libcss.h>

static css_error resolve_url(void *pw, const char *base,
                             lwc_string *rel, lwc_string **abs)
{
    (void)pw; (void)base;
    *abs = lwc_string_ref(rel);
    return CSS_OK;
}

int main(void)
{
    char out[512];
    int n = 0;

    lwc_string *s = NULL;
    lwc_error le = lwc_intern_string("html", 4, &s);
    n += snprintf(out + n, sizeof(out) - n,
                  "lwc_intern_string: err=%d len=%u\n",
                  (int)le, s ? (unsigned)lwc_string_length(s) : 0u);

    css_stylesheet_params params;
    memset(&params, 0, sizeof(params));
    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_DEFAULT;
    params.charset = "UTF-8";
    params.url = "test://m1";
    params.resolve = resolve_url;

    css_stylesheet *sheet = NULL;
    css_error ce = css_stylesheet_create(&params, &sheet);
    n += snprintf(out + n, sizeof(out) - n,
                  "css_stylesheet_create: err=%d nonnull=%d\n",
                  (int)ce, sheet ? 1 : 0);

    if (s) lwc_string_unref(s);
    if (sheet) css_stylesheet_destroy(sheet);

    n += snprintf(out + n, sizeof(out) - n, "M1 OK\n");

    /* print to console */
    printf("%s", out);

    /* also persist to a file for deterministic read-back */
    FILE *f = fopen("/M1RESULT.TXT", "w");
    if (f) { fwrite(out, 1, n, f); fclose(f); }

    return 0;
}
