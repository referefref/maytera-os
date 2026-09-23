// expattest - running proof for the mports Expat port (userland/ports/expat).
//
// Links the static libexpat.a that mports.sh built from the sha256-pinned
// upstream tarball and drives the push-parser over a fixed XML document,
// asserting the exact sequence of elements, an attribute value and the
// character data collected. Not self-consistency: the expected values are
// written out by hand from the input, so a parser that diverged would fail.
//
// OUTPUT DISCIPLINE (see zlibtest): one write(2) per serial record.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "expat.h"

static int g_pass = 0, g_fail = 0;
static void line(const char *s){ write(2, s, strlen(s)); }
static void ck(const char *what, int ok){
    char b[256]; if(ok) g_pass++; else g_fail++;
    snprintf(b,sizeof(b),"[EXPATTEST] %s %s\n", ok?"PASS":"FAIL", what);
    line(b);
}

static int g_elems = 0;
static char g_first_attr[64] = {0};
static char g_chars[256] = {0};
static int g_chars_len = 0;

static void XMLCALL on_start(void *ud, const XML_Char *name, const XML_Char **atts){
    (void)ud;
    g_elems++;
    if(strcmp(name,"item")==0 && atts[0] && strcmp(atts[0],"id")==0 && atts[1] && g_first_attr[0]==0){
        strncpy(g_first_attr, atts[1], sizeof(g_first_attr)-1);
    }
}
static void XMLCALL on_chars(void *ud, const XML_Char *s, int len){
    (void)ud;
    for(int i=0;i<len && g_chars_len<(int)sizeof(g_chars)-1;i++){
        char c=s[i];
        if(c!='\n' && c!=' ' && c!='\t') g_chars[g_chars_len++]=c;
    }
}

static const char *DOC =
  "<?xml version=\"1.0\"?>"
  "<catalog>"
    "<item id=\"a1\"><name>files</name></item>"
    "<item id=\"a2\"><name>settings</name></item>"
    "<item id=\"a3\"><name>terminal</name></item>"
  "</catalog>";

int main(void){
    line("[EXPATTEST] start\n");
    line("[EXPATTEST] XML_ExpatVersion="); line(XML_ExpatVersion()); line("\n");

    XML_Parser p = XML_ParserCreate(NULL);
    ck("parser created", p != NULL);
    if(p){
        XML_SetElementHandler(p, on_start, NULL);
        XML_SetCharacterDataHandler(p, on_chars);
        enum XML_Status st = XML_Parse(p, DOC, (int)strlen(DOC), 1);
        ck("parse ok (well-formed)", st == XML_STATUS_OK);
        if(st != XML_STATUS_OK){
            char b[256];
            snprintf(b,sizeof(b),"[EXPATTEST]      error=%s\n", XML_ErrorString(XML_GetErrorCode(p)));
            line(b);
        }
        // 1 catalog + 3 item + 3 name = 7 start elements
        char b[128];
        snprintf(b,sizeof(b),"[EXPATTEST]      elems=%d first_attr=%s chars=%s\n", g_elems, g_first_attr, g_chars);
        line(b);
        ck("7 start elements", g_elems == 7);
        ck("first item id==a1", strcmp(g_first_attr,"a1")==0);
        ck("chardata==filessettingsterminal", strcmp(g_chars,"filessettingsterminal")==0);
        XML_ParserFree(p);
    }

    // Negative: malformed XML must be rejected, proving the parser is real.
    XML_Parser q = XML_ParserCreate(NULL);
    enum XML_Status bad = XML_Parse(q, "<a><b></a>", 10, 1);
    ck("malformed rejected", bad != XML_STATUS_OK);
    XML_ParserFree(q);

    char b[128];
    snprintf(b,sizeof(b),"[EXPATTEST] done pass=%d fail=%d\n", g_pass, g_fail);
    line(b);
    return g_fail ? 1 : 0;
}
