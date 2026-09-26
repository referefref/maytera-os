// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aitools.c - the model-facing OS tool surface, expanded from aitools.def.
//
// COMPILES BOTH WAYS ON PURPOSE. Ring 3 links it into libc.a; the build host
// compiles this same file (with -DAITOOLS_HOST) into
// tools/aitools-index/aitools-index.c to emit /AITOOLS/INDEX.yaml. So the
// index on the image and the schema the client sends are produced by the same
// code from the same table, and "the shipped index describes the running
// surface" becomes a property rather than a promise. The last index that was
// only promised advertised 18 of 29 tools (MEASURED, #469m section 3).
//
// That is why there is nothing here but strlen/memcpy. No stdio, no malloc, no
// syscalls, no libc-specific header. Anything richer would have to exist in
// both worlds, and the first thing that did not would end the guarantee.
//
// THREE PREPROCESSOR PASSES OVER aitools.def, because a macro cannot generate
// an identifier and then be referenced by it:
//   1. g_args[]      every argument of every tool, flattened in order.
//   2. g_tools[]     one row per tool (id, summary).
//   3. g_nargs_raw[] how many arguments each tool declared, same order.
// aitools_init() then walks 2 and 3 in lockstep once to slice g_args[].

#ifdef AITOOLS_HOST
#include <string.h>
#else
#include "string.h"
#endif
#include "aitools.h"

#define AI_T_STR 0
#define AI_T_INT 1

typedef struct { const char *name; int type; int required; } aitool_arg_t;
typedef struct {
    const char *id;
    const char *summary;
    const aitool_arg_t *args;
    int nargs;
} aitool_t;

// pass 1: every argument, flattened.
#define AI_TOOL(id, summary)
#define AI_ARG(nm, ty, req)  { nm, ty, req },
static const aitool_arg_t g_args[] = {
#include "aitools.def"
    { 0, 0, 0 }                      // sentinel: keeps the array non-empty
};
#undef AI_TOOL
#undef AI_ARG

// pass 2: one row per tool.
#define AI_TOOL(id, summary) { id, summary, 0, 0 },
#define AI_ARG(nm, ty, req)
static aitool_t g_tools[] = {
#include "aitools.def"
};
#undef AI_TOOL
#undef AI_ARG

// pass 3: the argument count per tool, as "0 +1 +1" arithmetic the compiler
// folds. A tool with no AI_ARG line contributes a bare 0.
//
// THE LEADING DUMMY IS LOAD-BEARING. Each term is "0 +1 +1", so the separator
// has to be emitted by AI_TOOL, and a macro cannot emit a comma before the
// FIRST element only. So AI_TOOL emits ", 0" and the array opens with a
// placeholder: tool i is at [i+1]. The _Static_assert below is what stops that
// off-by-one ever being wrong silently.
#define AI_TOOL(id, summary) , 0
#define AI_ARG(nm, ty, req)  +1
static const int g_nargs_raw[] = { 0
#include "aitools.def"
};
#undef AI_TOOL
#undef AI_ARG

// pass 4: the TOTAL argument count, as one constant expression, purely so the
// two argument passes can be compared at compile time. aitools_init() slices
// g_args[] using the per-tool counts from pass 3; if pass 1 and pass 3 ever
// expanded differently the slices would silently walk off the end of one tool
// into the next and the JSON emitter would attach the wrong argument names to
// the wrong function. That is a defect no test would obviously catch and the
// compiler can rule out for free.
#define AI_TOOL(id, summary)
#define AI_ARG(nm, ty, req)  +1
enum { AITOOLS_ARGS_TOTAL = 0
#include "aitools.def"
};
#undef AI_TOOL
#undef AI_ARG

#define AITOOLS_N ((int)(sizeof(g_tools) / sizeof(g_tools[0])))
#define AITOOL_NARGS(i) (g_nargs_raw[(i) + 1])

_Static_assert(AITOOLS_ARGS_TOTAL == (int)(sizeof(g_args) / sizeof(g_args[0])) - 1,
               "aitools.def: the flattened argument array and the per-tool "
               "argument counts disagree. Every tool after the first mismatch "
               "would advertise another tool arguments.");

_Static_assert(sizeof(g_nargs_raw) / sizeof(g_nargs_raw[0])
                   == sizeof(g_tools) / sizeof(g_tools[0]) + 1,
               "aitools.def: the per-tool argument-count pass disagrees with the "
               "tool pass. The two #includes must expand the same number of "
               "AI_TOOL rows, plus the one leading placeholder.");

static int g_inited = 0;

static void aitools_init(void) {
    if (g_inited) return;
    int off = 0;
    for (int i = 0; i < AITOOLS_N; i++) {
        g_tools[i].nargs = AITOOL_NARGS(i);
        g_tools[i].args  = &g_args[off];
        off += AITOOL_NARGS(i);
    }
    g_inited = 1;
}

int aitools_count(void) { return AITOOLS_N; }

const char *aitools_id(int i) {
    if (i < 0 || i >= AITOOLS_N) return 0;
    return g_tools[i].id;
}

const char *aitools_summary(int i) {
    if (i < 0 || i >= AITOOLS_N) return 0;
    return g_tools[i].summary;
}

int aitools_nargs(int i) {
    if (i < 0 || i >= AITOOLS_N) return 0;
    aitools_init();
    return g_tools[i].nargs;
}

const char *aitools_arg_name(int i, int a) {
    if (i < 0 || i >= AITOOLS_N) return 0;
    aitools_init();
    if (a < 0 || a >= g_tools[i].nargs) return 0;
    return g_tools[i].args[a].name;
}

int aitools_arg_is_int(int i, int a) {
    if (i < 0 || i >= AITOOLS_N) return 0;
    aitools_init();
    if (a < 0 || a >= g_tools[i].nargs) return 0;
    return g_tools[i].args[a].type == AI_T_INT;
}

int aitools_arg_required(int i, int a) {
    if (i < 0 || i >= AITOOLS_N) return 0;
    aitools_init();
    if (a < 0 || a >= g_tools[i].nargs) return 0;
    return g_tools[i].args[a].required;
}

const char *aitools_fn_name(int i, char *out, int ocap) {
    if (!out || ocap <= 0) return out;
    out[0] = 0;
    if (i < 0 || i >= AITOOLS_N) return out;
    int o = 0;
    for (const char *s = g_tools[i].id; *s && o < ocap - 1; s++)
        out[o++] = (*s == '.') ? '_' : *s;
    out[o] = 0;
    return out;
}

// A REVERSE MAP AND NOT A STRING SUBSTITUTION. Replacing '_' with '.' is not
// the inverse of replacing '.' with '_': build_compile_app would come back as
// build.compile.app, which dispatch_tool() rejects as an unknown tool. Every
// native tool call would then fail for the four multi-word ids.
int aitools_index_of_fn(const char *fn) {
    if (!fn || !fn[0]) return -1;
    char buf[80];
    for (int i = 0; i < AITOOLS_N; i++) {
        aitools_fn_name(i, buf, (int)sizeof(buf));
        if (!strcmp(buf, fn)) return i;
    }
    return -1;
}

// --- a minimal bounded appender ---------------------------------------------
// Returns the new length, never exceeding cap-1, and raises *ovf if anything
// was dropped. The caller checks *ovf ONCE at the end rather than after every
// append: a JSON array that lost bytes anywhere is invalid everywhere, so the
// only safe reaction is to discard the whole thing.
static int ap(char *d, int n, int cap, const char *s, int *ovf) {
    if (!s) return n;
    int l = (int)strlen(s);
    if (n + l >= cap) { *ovf = 1; return n; }
    memcpy(d + n, s, (unsigned long)l);
    n += l;
    d[n] = 0;
    return n;
}

const char *aitools_prose_list(void) {
    static char buf[AITOOLS_PROSE_MAX];
    static int built = 0;
    if (built) return buf;
    aitools_init();
    int ovf = 0, n = 0;
    buf[0] = 0;
    for (int i = 0; i < AITOOLS_N; i++) {
        n = ap(buf, n, (int)sizeof(buf), "  ", &ovf);
        n = ap(buf, n, (int)sizeof(buf), g_tools[i].id, &ovf);
        n = ap(buf, n, (int)sizeof(buf), ": ", &ovf);
        n = ap(buf, n, (int)sizeof(buf), g_tools[i].summary, &ovf);
        n = ap(buf, n, (int)sizeof(buf), "\n", &ovf);
    }
    // A truncated PROSE list is degraded text but still valid text, unlike the
    // JSON array below, so it is kept and simply ends early. The buffer is
    // sized from the table with headroom; the flag exists so a future tool
    // that overruns it is visible to a reader rather than silent.
    (void)ovf;
    built = 1;
    return buf;
}

const char *aitools_id_list(void) {
    static char buf[1024];
    static int built = 0;
    if (built) return buf;
    aitools_init();
    int ovf = 0, n = 0;
    buf[0] = 0;
    for (int i = 0; i < AITOOLS_N; i++) {
        if (i) n = ap(buf, n, (int)sizeof(buf), ", ", &ovf);
        n = ap(buf, n, (int)sizeof(buf), g_tools[i].id, &ovf);
    }
    (void)ovf;
    built = 1;
    return buf;
}

int aitools_emit_tools_json(char *out, int ocap) {
    if (!out || ocap < 4) return 0;
    aitools_init();
    int ovf = 0, n = 0;
    out[0] = 0;
    n = ap(out, n, ocap, "[", &ovf);
    for (int i = 0; i < AITOOLS_N; i++) {
        char fn[80];
        aitools_fn_name(i, fn, (int)sizeof(fn));
        if (i) n = ap(out, n, ocap, ",", &ovf);
        n = ap(out, n, ocap, "{\"type\":\"function\",\"function\":{\"name\":\"", &ovf);
        n = ap(out, n, ocap, fn, &ovf);
        n = ap(out, n, ocap, "\",\"description\":\"", &ovf);
        // aitools.def forbids a double quote in a summary precisely so this
        // append needs no JSON escaper in a file that must also compile
        // freestanding. tools/aitools-index --check enforces that rule.
        n = ap(out, n, ocap, g_tools[i].summary, &ovf);
        n = ap(out, n, ocap, "\",\"parameters\":{\"type\":\"object\",\"properties\":{", &ovf);
        for (int a = 0; a < g_tools[i].nargs; a++) {
            if (a) n = ap(out, n, ocap, ",", &ovf);
            n = ap(out, n, ocap, "\"", &ovf);
            n = ap(out, n, ocap, g_tools[i].args[a].name, &ovf);
            n = ap(out, n, ocap,
                   g_tools[i].args[a].type == AI_T_INT
                     ? "\":{\"type\":\"integer\"}" : "\":{\"type\":\"string\"}", &ovf);
        }
        n = ap(out, n, ocap, "},\"required\":[", &ovf);
        int r = 0;
        for (int a = 0; a < g_tools[i].nargs; a++) {
            if (!g_tools[i].args[a].required) continue;
            if (r++) n = ap(out, n, ocap, ",", &ovf);
            n = ap(out, n, ocap, "\"", &ovf);
            n = ap(out, n, ocap, g_tools[i].args[a].name, &ovf);
            n = ap(out, n, ocap, "\"", &ovf);
        }
        n = ap(out, n, ocap, "]}}}", &ovf);
    }
    n = ap(out, n, ocap, "]", &ovf);
    // ALL OR NOTHING. A truncated array is invalid JSON, and an endpoint
    // rejecting the whole POST with a 400 is a much worse failure than falling
    // back to the prose protocol, which the caller does when this returns 0.
    if (ovf) { out[0] = 0; return 0; }
    return n;
}
