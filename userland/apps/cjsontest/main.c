// cjsontest - running proof for the mports cJSON port (userland/ports/cjson).
//
// Links the static libcjson.a that mports.sh built from the sha256-pinned
// upstream tarball and exercises the core parse/query/build/print surface the
// AI tool-contract and App Store code need. Not self-consistency only: it
// parses a fixed JSON literal and asserts the EXACT decoded values, then
// round-trips a built tree through PrintUnformatted + re-Parse.
//
// OUTPUT DISCIPLINE (see zlibtest): one write(2) per serial record.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "cJSON.h"

static int g_pass = 0, g_fail = 0;
static void line(const char *s){ write(2, s, strlen(s)); }
static void ck(const char *what, int ok){
    char b[256]; if(ok) g_pass++; else g_fail++;
    snprintf(b,sizeof(b),"[CJSONTEST] %s %s\n", ok?"PASS":"FAIL", what);
    line(b);
}

static const char *DOC =
  "{\"name\":\"maytera\",\"build\":984,\"ok\":true,"
  "\"ratio\":2.5,\"apps\":[\"files\",\"settings\",\"terminal\"]}";

int main(void){
    line("[CJSONTEST] start\n");
    line("[CJSONTEST] cJSON_Version="); line(cJSON_Version()); line("\n");

    cJSON *root = cJSON_Parse(DOC);
    ck("parse object", root != NULL);
    if(root){
        cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        ck("name is string", cJSON_IsString(name));
        ck("name==maytera", name && name->valuestring && strcmp(name->valuestring,"maytera")==0);
        cJSON *build = cJSON_GetObjectItemCaseSensitive(root, "build");
        ck("build is number", cJSON_IsNumber(build));
        ck("build==984", build && build->valueint==984);
        cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
        ck("ok is true", cJSON_IsBool(ok) && cJSON_IsTrue(ok));
        cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
        ck("apps is array", cJSON_IsArray(apps));
        ck("apps size==3", cJSON_GetArraySize(apps)==3);
        cJSON *a1 = cJSON_GetArrayItem(apps, 1);
        ck("apps[1]==settings", a1 && a1->valuestring && strcmp(a1->valuestring,"settings")==0);
        cJSON_Delete(root);
    }

    // Build a tree, print it, re-parse it: round-trip.
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "k", cJSON_CreateString("v"));
    cJSON_AddItemToObject(o, "n", cJSON_CreateNumber(42));
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(7));
    cJSON_AddItemToObject(o, "list", arr);
    char *out = cJSON_PrintUnformatted(o);
    ck("print produced text", out != NULL);
    if(out){ line("[CJSONTEST] printed="); line(out); line("\n"); }
    cJSON *re = out ? cJSON_Parse(out) : NULL;
    ck("reparse round-trip", re != NULL);
    if(re){
        cJSON *n = cJSON_GetObjectItemCaseSensitive(re,"n");
        ck("round-trip n==42", n && n->valueint==42);
        cJSON_Delete(re);
    }
    if(out) cJSON_free(out);
    cJSON_Delete(o);

    char b[128];
    snprintf(b,sizeof(b),"[CJSONTEST] done pass=%d fail=%d\n", g_pass, g_fail);
    line(b);
    return g_fail ? 1 : 0;
}
