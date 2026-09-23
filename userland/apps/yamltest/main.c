// yamltest - running proof for the mports libyaml port (userland/ports/libyaml).
//
// Links the static libyaml.a that mports.sh built from the sha256-pinned
// upstream tarball and drives the event-based parser over a fixed YAML document,
// collecting every SCALAR value and asserting the exact expected sequence. The
// expected values are written out by hand from the input, so a parser that
// diverged would fail.
//
// OUTPUT DISCIPLINE (see zlibtest): one write(2) per serial record.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "yaml.h"

static int g_pass = 0, g_fail = 0;
static void line(const char *s){ write(2, s, strlen(s)); }
static void ck(const char *what, int ok){
    char b[256]; if(ok) g_pass++; else g_fail++;
    snprintf(b,sizeof(b),"[YAMLTEST] %s %s\n", ok?"PASS":"FAIL", what);
    line(b);
}

static const char *DOC =
  "name: maytera\n"
  "build: 984\n"
  "apps:\n"
  "  - files\n"
  "  - settings\n"
  "  - terminal\n";

int main(void){
    line("[YAMLTEST] start\n");
    line("[YAMLTEST] yaml_get_version_string="); line(yaml_get_version_string()); line("\n");

    yaml_parser_t parser;
    ck("parser initialized", yaml_parser_initialize(&parser) != 0);
    yaml_parser_set_input_string(&parser, (const unsigned char*)DOC, strlen(DOC));

    char scalars[16][64];
    int ns = 0, done = 0, err = 0;
    while(!done){
        yaml_event_t ev;
        if(!yaml_parser_parse(&parser, &ev)){ err = 1; break; }
        if(ev.type == YAML_SCALAR_EVENT){
            if(ns < 16){
                strncpy(scalars[ns], (const char*)ev.data.scalar.value, 63);
                scalars[ns][63]=0; ns++;
            }
        } else if(ev.type == YAML_STREAM_END_EVENT){
            done = 1;
        }
        yaml_event_delete(&ev);
    }
    ck("no parse error", err == 0);
    yaml_parser_delete(&parser);

    char b[256];
    snprintf(b,sizeof(b),"[YAMLTEST]      scalars=%d\n", ns);
    line(b);
    ck("8 scalars", ns == 8);
    ck("scalar[0]==name", ns>0 && strcmp(scalars[0],"name")==0);
    ck("scalar[1]==maytera", ns>1 && strcmp(scalars[1],"maytera")==0);
    ck("scalar[3]==984", ns>3 && strcmp(scalars[3],"984")==0);
    ck("scalar[5]==files", ns>5 && strcmp(scalars[5],"files")==0);
    ck("scalar[6]==settings", ns>6 && strcmp(scalars[6],"settings")==0);
    ck("scalar[7]==terminal", ns>7 && strcmp(scalars[7],"terminal")==0);

    snprintf(b,sizeof(b),"[YAMLTEST] done pass=%d fail=%d\n", g_pass, g_fail);
    line(b);
    return g_fail ? 1 : 0;
}
