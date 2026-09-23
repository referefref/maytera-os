// md4ctest - running proof for the mports MD4C port (userland/ports/md4c).
//
// Links the static libmd4c.a that mports.sh built from the sha256-pinned upstream
// tarball and renders a fixed Markdown document to HTML via md_html(), asserting
// the output contains the expected HTML fragments (h1, strong, list, code). The
// expected fragments are CommonMark ground truth, so a renderer that diverged
// would fail.
//
// OUTPUT DISCIPLINE (see zlibtest): one write(2) per serial record.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "md4c-html.h"

static int g_pass = 0, g_fail = 0;
static void line(const char *s){ write(2, s, strlen(s)); }
static void ck(const char *what, int ok){
    char b[256]; if(ok) g_pass++; else g_fail++;
    snprintf(b,sizeof(b),"[MD4CTEST] %s %s\n", ok?"PASS":"FAIL", what);
    line(b);
}

static char g_out[8192];
static int g_out_len = 0;
static void sink(const MD_CHAR *text, MD_SIZE size, void *ud){
    (void)ud;
    for(MD_SIZE i=0;i<size && g_out_len<(int)sizeof(g_out)-1;i++)
        g_out[g_out_len++] = text[i];
    g_out[g_out_len] = 0;
}

static const char *MD =
  "# Hello\n"
  "\n"
  "This is **bold** and `code`.\n"
  "\n"
  "- one\n"
  "- two\n";

int main(void){
    line("[MD4CTEST] start\n");
    int rc = md_html(MD, (MD_SIZE)strlen(MD), sink, NULL, 0, 0);
    ck("md_html returned 0", rc == 0);
    // Emit the rendered HTML (single write) for the record.
    line("[MD4CTEST] html-begin\n");
    { char *p=g_out; while(*p){ char ln[256]; int n=0; while(*p && *p!='\n' && n<250) ln[n++]=*p++; if(*p=='\n')p++; ln[n++]='\n'; ln[n]=0; write(2,ln,n);} }
    line("[MD4CTEST] html-end\n");
    ck("contains <h1>Hello</h1>", strstr(g_out,"<h1>Hello</h1>") != NULL);
    ck("contains <strong>bold</strong>", strstr(g_out,"<strong>bold</strong>") != NULL);
    ck("contains <code>code</code>", strstr(g_out,"<code>code</code>") != NULL);
    ck("contains <ul>", strstr(g_out,"<ul>") != NULL);
    ck("contains <li>one</li>", strstr(g_out,"<li>one</li>") != NULL);

    char b[128];
    snprintf(b,sizeof(b),"[MD4CTEST] done pass=%d fail=%d html_len=%d\n", g_pass, g_fail, g_out_len);
    line(b);
    return g_fail ? 1 : 0;
}
