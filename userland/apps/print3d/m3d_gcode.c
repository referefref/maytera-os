// m3d_gcode.c - see m3d_gcode.h. Faithful port of M33-Fio gcode.cpp.
#include "m3d_gcode.h"

#include <string.h>

// ORDER string and orderToOffset table, straight from M33-Fio gcode.cpp.
static const char ORDER[GC_ORDER_LEN + 1] = "NMGXYZE FTSP    IJRD";

#define GC_INVALID 0xFF
static int order_to_offset(unsigned char c) {
    switch (c) {
        case 'D': return 19; case 'E': return 6;  case 'F': return 8;
        case 'G': return 2;  case 'I': return 16; case 'J': return 17;
        case 'M': return 1;  case 'N': return 0;  case 'P': return 11;
        case 'R': return 18; case 'S': return 10; case 'T': return 9;
        case 'X': return 3;  case 'Y': return 4;  case 'Z': return 5;
        default:  return -1;
    }
}
int m3d_gcode_offset(char parameter) { return order_to_offset((unsigned char)parameter); }

static int gc_isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static int gc_isupper(int c) { return c>='A' && c<='Z'; }

// ---------------------------------------------------------------------------
// Number helpers
// ---------------------------------------------------------------------------
void m3d_ltoa(long v, char *out) {
    char tmp[24]; int n = 0; int neg = 0;
    unsigned long u;
    if (v < 0) { neg = 1; u = (unsigned long)(-(v + 1)) + 1UL; } else u = (unsigned long)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    int o = 0;
    if (neg) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

// Format like printf("%f") / std::to_string(double): fixed 6 decimals.
// Uses round-half-away-from-zero, which matches glibc for every value that is
// not an exact 7th-decimal tie (computed print coordinates never hit ties).
void m3d_dtoa(double v, char *out) {
    int neg = 0;
    if (v < 0.0) { neg = 1; v = -v; }
    // scale to 6 decimals and round to nearest
    double scaled = v * 1000000.0 + 0.5;
    // guard against overflow of the integer conversion for huge values
    unsigned long long iscaled = (unsigned long long)scaled;
    unsigned long long ip = iscaled / 1000000ULL;
    unsigned long long fp = iscaled % 1000000ULL;

    char ipbuf[24]; int n = 0;
    if (ip == 0) ipbuf[n++] = '0';
    while (ip) { ipbuf[n++] = (char)('0' + (ip % 10)); ip /= 10; }

    int o = 0;
    if (neg) out[o++] = '-';
    while (n) out[o++] = ipbuf[--n];
    out[o++] = '.';
    // 6 fractional digits, zero padded
    char fbuf[6];
    for (int i = 5; i >= 0; i--) { fbuf[i] = (char)('0' + (fp % 10)); fp /= 10; }
    for (int i = 0; i < 6; i++) out[o++] = fbuf[i];
    out[o] = 0;
}

double m3d_atod(const char *s) {
    while (gc_isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+') s++; else if (*s == '-') { neg = 1; s++; }
    double ip = 0.0;
    while (*s >= '0' && *s <= '9') { ip = ip * 10.0 + (*s - '0'); s++; }
    double frac = 0.0, scale = 1.0;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { frac = frac * 10.0 + (*s - '0'); scale *= 10.0; s++; }
    }
    double val = ip + frac / scale;
    // optional exponent (g-code shouldn't have it, but be safe)
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0; if (*s=='+') s++; else if (*s=='-'){eneg=1;s++;}
        int e = 0; while (*s >= '0' && *s <= '9') { e = e*10 + (*s-'0'); s++; }
        double p = 1.0; for (int i=0;i<e;i++) p*=10.0;
        if (eneg) val /= p; else val *= p;
    }
    return neg ? -val : val;
}

int m3d_atoi_g(const char *s) {
    while (gc_isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+') s++; else if (*s == '-') { neg = 1; s++; }
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return (int)(neg ? -v : v);
}

// ---------------------------------------------------------------------------
// Gcode
// ---------------------------------------------------------------------------
void m3d_gcode_clear(m3d_gcode_t *g) {
    g->dataType = GC_EMPTY_DATATYPE;
    for (int i = 0; i < GC_ORDER_LEN; i++) g->param[i][0] = 0;
    g->hostCommand[0] = 0;
    g->original[0] = 0;
}

static void gc_setparam(m3d_gcode_t *g, int off, const char *val) {
    int i = 0;
    while (val[i] && i < GC_VAL_MAX - 1) { g->param[off][i] = val[i]; i++; }
    g->param[off][i] = 0;
}

bool m3d_gcode_parse(m3d_gcode_t *g, const char *line) {
    m3d_gcode_clear(g);

    const char *lineStart = line, *lineEnd;
    while (gc_isspace((unsigned char)*lineStart)) lineStart++;

    for (lineEnd = lineStart; *lineEnd && *lineEnd != ';' && *lineEnd != '*'; lineEnd++);
    lineEnd--;
    while (lineEnd >= lineStart && gc_isspace((unsigned char)*lineEnd)) lineEnd--;

    lineEnd++;
    if (lineEnd == lineStart) { g->original[0] = 0; return false; }

    int cmdLen = (int)(lineEnd - lineStart);
    if (cmdLen > GC_LINE_MAX - 1) cmdLen = GC_LINE_MAX - 1;
    char command[GC_LINE_MAX];
    memcpy(command, lineStart, cmdLen);
    command[cmdLen] = 0;

    // original command
    memcpy(g->original, command, cmdLen + 1);

    // host command
    if (command[0] == '@') {
        memcpy(g->hostCommand, command, cmdLen + 1);
        return true;
    }

    char currentValue[GC_VAL_MAX]; int cvn = 0;
    unsigned char parameterIdentifier = 0;
    int parameterOffset;

    for (int i = 0; i <= cmdLen; i++) {
        if (i == 0 || gc_isupper(command[i]) || command[i] == ' ' || !command[i]) {
            if (i && (parameterOffset = order_to_offset(parameterIdentifier)) != -1) {
                g->dataType |= (1u << parameterOffset);
                currentValue[cvn] = 0;
                gc_setparam(g, parameterOffset, currentValue);
            }
            cvn = 0;

            // string-carrying M commands: M23/28/29/30/32/117
            if (parameterIdentifier == 'M' &&
                (!strcmp(g->param[1], "23") || !strcmp(g->param[1], "28") ||
                 !strcmp(g->param[1], "29") || !strcmp(g->param[1], "30") ||
                 !strcmp(g->param[1], "32") || !strcmp(g->param[1], "117"))) {
                int k = 0;
                for (; i < cmdLen; i++) if (k < GC_VAL_MAX - 1) currentValue[k++] = command[i];
                currentValue[k] = 0;
                // strip leading whitespace
                int off = 0; while (currentValue[off] && gc_isspace((unsigned char)currentValue[off])) off++;
                gc_setparam(g, GC_STRING, currentValue + off);
                if (g->param[GC_STRING][0]) g->dataType |= (1u << 15);
                break;
            }
            parameterIdentifier = (unsigned char)command[i];
        } else if (!gc_isspace((unsigned char)command[i])) {
            if (cvn < GC_VAL_MAX - 1) currentValue[cvn++] = command[i];
        }
    }
    return g->dataType != GC_EMPTY_DATATYPE;
}

bool m3d_gcode_is_empty(const m3d_gcode_t *g) { return g->dataType == GC_EMPTY_DATATYPE; }
bool m3d_gcode_is_host(const m3d_gcode_t *g)  { return g->hostCommand[0] != 0; }

bool m3d_gcode_has_parameter(const m3d_gcode_t *g, char parameter) {
    int off = order_to_offset((unsigned char)parameter);
    if (off != -1) return (g->dataType & (1u << off)) != 0;
    return false;
}
bool m3d_gcode_has_value(const m3d_gcode_t *g, char parameter) {
    int off = order_to_offset((unsigned char)parameter);
    if (off != -1) return g->param[off][0] != 0;
    return false;
}
const char *m3d_gcode_get_value(const m3d_gcode_t *g, char parameter) {
    int off = order_to_offset((unsigned char)parameter);
    if (off != -1) return g->param[off];
    return "";
}
void m3d_gcode_set_value(m3d_gcode_t *g, char parameter, const char *value) {
    int off = order_to_offset((unsigned char)parameter);
    if (off != -1) { g->dataType |= (1u << off); gc_setparam(g, off, value); }
}
void m3d_gcode_remove_parameter(m3d_gcode_t *g, char parameter) {
    int off = order_to_offset((unsigned char)parameter);
    if (off != -1) {
        g->dataType &= ~(1u << off);
        g->param[off][0] = 0;
        if (g->dataType == GC_EMPTY_DATATYPE) g->original[0] = 0;
    }
}

void m3d_gcode_get_ascii(const m3d_gcode_t *g, char *out) {
    if (g->hostCommand[0]) { strcpy(out, g->hostCommand); return; }
    int o = 0;
    for (int i = 0; i < GC_ORDER_LEN; i++) {
        if (i == 7 || i == 12 || i == 13 || i == 14 || i == 15) continue;
        if (g->dataType & (1u << i)) {
            out[o++] = ORDER[i];
            const char *v = g->param[i];
            while (*v) out[o++] = *v++;
            out[o++] = ' ';
            if (i == 1 && (g->dataType & (1u << 15))) {
                const char *s = g->param[GC_STRING];
                while (*s) out[o++] = *s++;
                out[o++] = ' ';
            }
        }
    }
    if (o > 0) o--;      // drop trailing space
    out[o] = 0;
}

// ---------------------------------------------------------------------------
// Binary framing (M3D wire packet)
// ---------------------------------------------------------------------------
static int push8(uint8_t *b, int *n, int cap, int v) {
    if (*n >= cap) return -1;
    b[(*n)++] = (uint8_t)(v & 0xFF);
    return 0;
}
static int push_i16(uint8_t *b, int *n, int cap, const char *val) {
    int32_t t = m3d_atoi_g(val);
    if (push8(b,n,cap,t) < 0) return -1;
    if (push8(b,n,cap,t >> 8) < 0) return -1;
    return 0;
}
static int push_i32(uint8_t *b, int *n, int cap, const char *val) {
    int32_t t = m3d_atoi_g(val);
    if (push8(b,n,cap,t) < 0) return -1;
    if (push8(b,n,cap,t >> 8) < 0) return -1;
    if (push8(b,n,cap,t >> 16) < 0) return -1;
    if (push8(b,n,cap,t >> 24) < 0) return -1;
    return 0;
}
static int push_f32(uint8_t *b, int *n, int cap, const char *val) {
    float f = (float)m3d_atod(val);
    uint32_t u; memcpy(&u, &f, 4);
    if (push8(b,n,cap,u) < 0) return -1;
    if (push8(b,n,cap,u >> 8) < 0) return -1;
    if (push8(b,n,cap,u >> 16) < 0) return -1;
    if (push8(b,n,cap,u >> 24) < 0) return -1;
    return 0;
}

int m3d_gcode_get_binary(const m3d_gcode_t *g, uint8_t *out, int cap) {
    int n = 0;

    if (g->hostCommand[0]) {
        for (const char *p = g->hostCommand; *p; p++)
            if (push8(out, &n, cap, *p) < 0) return -1;
        return n;
    }

    uint32_t dt = g->dataType;
    if (push8(out,&n,cap,dt) < 0) return -1;
    if (push8(out,&n,cap,dt >> 8) < 0) return -1;
    if (push8(out,&n,cap,dt >> 16) < 0) return -1;
    if (push8(out,&n,cap,dt >> 24) < 0) return -1;

    if (g->param[GC_STRING][0])
        if (push8(out,&n,cap,(int)strlen(g->param[GC_STRING])) < 0) return -1;

    if (g->param[GC_N][0]) if (push_i16(out,&n,cap,g->param[GC_N]) < 0) return -1;
    if (g->param[GC_M][0]) if (push_i16(out,&n,cap,g->param[GC_M]) < 0) return -1;
    if (g->param[GC_G][0]) if (push_i16(out,&n,cap,g->param[GC_G]) < 0) return -1;
    if (g->param[GC_X][0]) if (push_f32(out,&n,cap,g->param[GC_X]) < 0) return -1;
    if (g->param[GC_Y][0]) if (push_f32(out,&n,cap,g->param[GC_Y]) < 0) return -1;
    if (g->param[GC_Z][0]) if (push_f32(out,&n,cap,g->param[GC_Z]) < 0) return -1;
    if (g->param[GC_E][0]) if (push_f32(out,&n,cap,g->param[GC_E]) < 0) return -1;
    if (g->param[GC_F][0]) if (push_f32(out,&n,cap,g->param[GC_F]) < 0) return -1;
    if (g->param[GC_T][0]) if (push8   (out,&n,cap,m3d_atoi_g(g->param[GC_T])) < 0) return -1;
    if (g->param[GC_S][0]) if (push_i32(out,&n,cap,g->param[GC_S]) < 0) return -1;
    if (g->param[GC_P][0]) if (push_i32(out,&n,cap,g->param[GC_P]) < 0) return -1;
    if (g->param[GC_I][0]) if (push_f32(out,&n,cap,g->param[GC_I]) < 0) return -1;
    if (g->param[GC_J][0]) if (push_f32(out,&n,cap,g->param[GC_J]) < 0) return -1;
    if (g->param[GC_R][0]) if (push_f32(out,&n,cap,g->param[GC_R]) < 0) return -1;
    if (g->param[GC_D][0]) if (push_f32(out,&n,cap,g->param[GC_D]) < 0) return -1;

    if (g->param[GC_STRING][0])
        for (const char *p = g->param[GC_STRING]; *p; p++)
            if (push8(out,&n,cap,*p) < 0) return -1;

    // Fletcher-16 over the packet body.
    uint16_t s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { s1 = (s1 + out[i]) % 0xFF; s2 = (s1 + s2) % 0xFF; }
    if (push8(out,&n,cap,s1) < 0) return -1;
    if (push8(out,&n,cap,s2) < 0) return -1;
    return n;
}

// ---------------------------------------------------------------------------
// M3D Pro binary framing (Repetier "Binary V2") - see m3d_gcode.h.
// Field/flag bits follow the decompiled RepetierHost.model.GCode (16-bit mask).
// ---------------------------------------------------------------------------
#define PV_N  0x0001u
#define PV_M  0x0002u
#define PV_G  0x0004u
#define PV_X  0x0008u
#define PV_Y  0x0010u
#define PV_Z  0x0020u
#define PV_E  0x0040u
#define PV_SENT 0x0080u   // sentinel (always set)
#define PV_F  0x0100u
#define PV_T  0x0200u
#define PV_S  0x0400u
#define PV_P  0x0800u
#define PV_V2 0x1000u
#define PV_TEXT 0x8000u
#define PV2_I 0x0001u
#define PV2_J 0x0002u
#define PV2_R 0x0004u

int m3d_gcode_get_binary_v2(const m3d_gcode_t *g, uint8_t *out, int cap) {
    int n = 0;

    if (g->hostCommand[0]) {
        for (const char *p = g->hostCommand; *p; p++)
            if (push8(out, &n, cap, *p) < 0) return -1;
        return n;
    }

    // Build the 16-bit RepetierHost field masks from the present parameters.
    uint16_t fields = PV_SENT | PV_V2;
    uint16_t fields2 = 0;
    if (g->param[GC_N][0]) fields |= PV_N;
    if (g->param[GC_M][0]) fields |= PV_M;
    if (g->param[GC_G][0]) fields |= PV_G;
    if (g->param[GC_X][0]) fields |= PV_X;
    if (g->param[GC_Y][0]) fields |= PV_Y;
    if (g->param[GC_Z][0]) fields |= PV_Z;
    if (g->param[GC_E][0]) fields |= PV_E;
    if (g->param[GC_F][0]) fields |= PV_F;
    if (g->param[GC_T][0]) fields |= PV_T;
    if (g->param[GC_S][0]) fields |= PV_S;
    if (g->param[GC_P][0]) fields |= PV_P;
    if (g->param[GC_STRING][0]) fields |= PV_TEXT;
    if (g->param[GC_I][0]) fields2 |= PV2_I;
    if (g->param[GC_J][0]) fields2 |= PV2_J;
    if (g->param[GC_R][0]) fields2 |= PV2_R;

    // Header: fields (2), fields2 (2), optional string length (1).
    if (push8(out,&n,cap,fields) < 0) return -1;
    if (push8(out,&n,cap,fields >> 8) < 0) return -1;
    if (push8(out,&n,cap,fields2) < 0) return -1;
    if (push8(out,&n,cap,fields2 >> 8) < 0) return -1;
    if (g->param[GC_STRING][0])
        if (push8(out,&n,cap,(int)strlen(g->param[GC_STRING])) < 0) return -1;

    // N uint16; M uint16; G uint16 (all 2-byte in V2).
    if (g->param[GC_N][0]) if (push_i16(out,&n,cap,g->param[GC_N]) < 0) return -1;
    if (g->param[GC_M][0]) if (push_i16(out,&n,cap,g->param[GC_M]) < 0) return -1;
    if (g->param[GC_G][0]) if (push_i16(out,&n,cap,g->param[GC_G]) < 0) return -1;

    // X,Y,Z,E,F float32; T uint8; S,P int32; I,J,R float32.
    if (g->param[GC_X][0]) if (push_f32(out,&n,cap,g->param[GC_X]) < 0) return -1;
    if (g->param[GC_Y][0]) if (push_f32(out,&n,cap,g->param[GC_Y]) < 0) return -1;
    if (g->param[GC_Z][0]) if (push_f32(out,&n,cap,g->param[GC_Z]) < 0) return -1;
    if (g->param[GC_E][0]) if (push_f32(out,&n,cap,g->param[GC_E]) < 0) return -1;
    if (g->param[GC_F][0]) if (push_f32(out,&n,cap,g->param[GC_F]) < 0) return -1;
    if (g->param[GC_T][0]) if (push8   (out,&n,cap,m3d_atoi_g(g->param[GC_T])) < 0) return -1;
    if (g->param[GC_S][0]) if (push_i32(out,&n,cap,g->param[GC_S]) < 0) return -1;
    if (g->param[GC_P][0]) if (push_i32(out,&n,cap,g->param[GC_P]) < 0) return -1;
    if (g->param[GC_I][0]) if (push_f32(out,&n,cap,g->param[GC_I]) < 0) return -1;
    if (g->param[GC_J][0]) if (push_f32(out,&n,cap,g->param[GC_J]) < 0) return -1;
    if (g->param[GC_R][0]) if (push_f32(out,&n,cap,g->param[GC_R]) < 0) return -1;

    // Text (length-prefixed above; not 16-byte padded in V2).
    if (g->param[GC_STRING][0])
        for (const char *p = g->param[GC_STRING]; *p; p++)
            if (push8(out,&n,cap,*p) < 0) return -1;

    // Fletcher-16 over the packet body.
    uint16_t s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { s1 = (s1 + out[i]) % 0xFF; s2 = (s1 + s2) % 0xFF; }
    if (push8(out,&n,cap,s1) < 0) return -1;
    if (push8(out,&n,cap,s2) < 0) return -1;
    return n;
}
