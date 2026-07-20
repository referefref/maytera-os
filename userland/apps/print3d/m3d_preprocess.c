// m3d_preprocess.c - see m3d_preprocess.h. Faithful C port of M33-Fio
// preprocessor.cpp (donovan6000, GPL). Structure and control flow mirror the
// reference; comments cite the corresponding reference behavior.
#include "m3d_preprocess.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

// The freestanding MayteraOS libc math.h may not define NAN / isnan; the host
// build's <math.h> does. Provide portable fallbacks.
#ifndef NAN
#define NAN (__builtin_nanf(""))
#endif
#ifndef isnan
#define isnan(x) __builtin_isnan(x)
#endif

// ---- constants (preprocessor.cpp) ----
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif
#define DBL_MAX_ 1.7976931348623157e308
#define DBL_EPS  2.2204460492503131e-16

#define BED_LOW_MAX_X 106.0
#define BED_LOW_MIN_X -2.0
#define BED_LOW_MAX_Y 105.0
#define BED_LOW_MAX_Z 5.0
#define BED_LOW_MIN_Z 0.0
#define BED_MEDIUM_MAX_X 106.0
#define BED_MEDIUM_MIN_X -2.0
#define BED_MEDIUM_MAX_Y 105.0
#define BED_MEDIUM_MIN_Y -9.0
#define BED_HIGH_MAX_X 97.0
#define BED_HIGH_MIN_X 7.0
#define BED_HIGH_MAX_Y 85.0
#define BED_HIGH_MIN_Y 9.0
#define BED_WIDTH 121.0
#define BED_DEPTH 121.0
#define BED_CENTER_OFFSET_X 8.5005
#define BED_CENTER_OFFSET_Y 2.0005
#define WAVE_PERIOD 5.0
#define WAVE_PERIOD_QUARTER (WAVE_PERIOD / 4.0)
#define WAVE_SIZE 0.15
#define SEGMENT_LENGTH 2.0

// EEPROM offsets used by the "calibrate before print" preparation branch.
#define EEPROM_BED_HEIGHT_OFFSET_OFFSET 0x56
#define EEPROM_BED_HEIGHT_OFFSET_LENGTH 4

// directions
enum { DIR_POSITIVE, DIR_NEGATIVE, DIR_NEITHER };
// print tiers
enum { TIER_LOW, TIER_MEDIUM, TIER_HIGH };
// stages (skip levels)
enum { ST_NONE, ST_INPUT, ST_MID_PRINT, ST_CENTER, ST_VALIDATION, ST_PREPARATION,
       ST_WAVE, ST_THERMAL, ST_BED, ST_BACKLASH, ST_SKEW };

// bed bounds that depend on external bed height / expand region
typedef struct { double bedLowMinY, bedMediumMaxZ, bedHighMaxZ, bedHighMinZ; } bedbounds_t;

static double d_min(double a, double b) { return a < b ? a : b; }
static double d_max(double a, double b) { return a > b ? a : b; }

// ---------------- number/string helpers ----------------
static void set_val_d(m3d_gcode_t *g, char p, double v) { char b[40]; m3d_dtoa(v, b); m3d_gcode_set_value(g, p, b); }
static double gval(const m3d_gcode_t *g, char p) { return m3d_gcode_has_value(g,p) ? m3d_atod(m3d_gcode_get_value(g,p)) : 0.0; }

// ---------------- vector geometry (bed compensation) ----------------
typedef struct { double x, y, z, e; } vec_t;
static vec_t v_make(double x, double y, double z) { vec_t v = {x,y,z,0}; return v; }
static vec_t v_sub(vec_t a, vec_t b) { vec_t r = {a.x-b.x, a.y-b.y, a.z-b.z, a.e-b.e}; return r; }
static vec_t v_add(vec_t a, vec_t b) { vec_t r = {a.x+b.x, a.y+b.y, a.z+b.z, a.e+b.e}; return r; }
static vec_t v_scale(vec_t a, double s) { vec_t r = {a.x*s, a.y*s, a.z*s, a.e*s}; return r; }
static void  v_normalize(vec_t *v) {
    double len = sqrt(v->x*v->x + v->y*v->y + v->z*v->z + v->e*v->e);
    v->x/=len; v->y/=len; v->z/=len; v->e/=len;
}
static double v_index(vec_t v, int i) { return i==0?v.x:i==1?v.y:i==2?v.z:v.e; }

static vec_t calc_plane_normal(vec_t v1, vec_t v2, vec_t v3) {
    vec_t a = v_sub(v2, v1), b = v_sub(v3, v1), r;
    r.x = a.y*b.z - b.y*a.z;
    r.y = a.z*b.x - b.z*a.x;
    r.z = a.x*b.y - b.x*a.y;
    r.e = 0;
    return r;
}
static vec_t gen_plane_eq(vec_t v1, vec_t v2, vec_t v3) {
    vec_t n = calc_plane_normal(v1, v2, v3), r;
    r.x = n.x; r.y = n.y; r.z = n.z;
    r.e = -(r.x*v1.x + r.y*v1.y + r.z*v1.z);   // index 3 == e
    return r;
}
static double z_from_xy_plane(vec_t point, vec_t planeABC) {
    double c = v_index(planeABC, 2);
    return c ? (v_index(planeABC,0)*point.x + v_index(planeABC,1)*point.y + v_index(planeABC,3)) / -c : 0;
}
static double sign3(vec_t p1, vec_t p2, vec_t p3) {
    return (p1.x-p3.x)*(p2.y-p3.y) - (p2.x-p3.x)*(p1.y-p3.y);
}
static bool point_in_triangle(vec_t pt, vec_t v1, vec_t v2, vec_t v3) {
    vec_t vec, v2b, v3b, v4b;
    vec = v_add(v_sub(v1,v2), v_sub(v1,v3)); v_normalize(&vec); v2b = v_add(v1, v_scale(vec,0.01));
    vec = v_add(v_sub(v2,v1), v_sub(v2,v3)); v_normalize(&vec); v3b = v_add(v2, v_scale(vec,0.01));
    vec = v_add(v_sub(v3,v1), v_sub(v3,v2)); v_normalize(&vec); v4b = v_add(v3, v_scale(vec,0.01));
    bool f1 = sign3(pt,v2b,v3b) < 0, f2 = sign3(pt,v3b,v4b) < 0, f3 = sign3(pt,v4b,v2b) < 0;
    return f1==f2 && f2==f3;
}
static double get_height_adjustment(m3d_ctx_t *c, bedbounds_t *bb, double x, double y) {
    (void)bb;
    vec_t v  = v_make(99, 95, c->backRightOrientation + c->backRightOffset);
    vec_t v2 = v_make(9, 95, c->backLeftOrientation + c->backLeftOffset);
    vec_t v3 = v_make(9, 5, c->frontLeftOrientation + c->frontLeftOffset);
    vec_t v4 = v_make(99, 5, c->frontRightOrientation + c->frontRightOffset);
    vec_t v5 = v_make(54, 50, 0);
    vec_t planeABC = gen_plane_eq(v2, v, v5);
    vec_t v7 = gen_plane_eq(v2, v3, v5);
    vec_t v8 = gen_plane_eq(v, v4, v5);
    vec_t v9 = gen_plane_eq(v3, v4, v5);
    vec_t point = v_make(x, y, 0);
    if (x <= v3.x && y >= v.y)  return (z_from_xy_plane(point, planeABC) + z_from_xy_plane(point, v7)) / 2;
    if (x <= v3.x && y <= v3.y) return (z_from_xy_plane(point, v9) + z_from_xy_plane(point, v7)) / 2;
    if (x >= v4.x && y <= v3.y) return (z_from_xy_plane(point, v9) + z_from_xy_plane(point, v8)) / 2;
    if (x >= v4.x && y >= v.y)  return (z_from_xy_plane(point, planeABC) + z_from_xy_plane(point, v8)) / 2;
    if (x <= v3.x) return z_from_xy_plane(point, v7);
    if (x >= v4.x) return z_from_xy_plane(point, v8);
    if (y >= v.y)  return z_from_xy_plane(point, planeABC);
    if (y <= v3.y) return z_from_xy_plane(point, v9);
    if (point_in_triangle(point, v5, v3, v2)) return z_from_xy_plane(point, v7);
    if (point_in_triangle(point, v5, v4, v))  return z_from_xy_plane(point, v8);
    if (point_in_triangle(point, v5, v2, v))  return z_from_xy_plane(point, planeABC);
    return z_from_xy_plane(point, v9);
}

// ---------------- tack points / sharp corners ----------------
static double get_distance(const m3d_gcode_t *a, const m3d_gcode_t *b) {
    double ax = m3d_gcode_has_value(a,'X') ? m3d_atod(m3d_gcode_get_value(a,'X')) : 0;
    double ay = m3d_gcode_has_value(a,'Y') ? m3d_atod(m3d_gcode_get_value(a,'Y')) : 0;
    double bx = m3d_gcode_has_value(b,'X') ? m3d_atod(m3d_gcode_get_value(b,'X')) : 0;
    double by = m3d_gcode_has_value(b,'Y') ? m3d_atod(m3d_gcode_get_value(b,'Y')) : 0;
    return sqrt((ax-bx)*(ax-bx) + (ay-by)*(ay-by));
}
static void tack_thermal(const m3d_gcode_t *pt, const m3d_gcode_t *ref, double time, m3d_gcode_t *out) {
    m3d_gcode_clear(out);
    uint16_t distance = (uint16_t)ceil(get_distance(pt, ref));
    if (distance > time / 1000) {
        uint32_t seconds = (uint32_t)time;
        uint32_t ms = (uint32_t)((time - seconds) * 1000);
        char b[24];
        m3d_gcode_set_value(out, 'G', "4");
        m3d_ltoa(seconds, b); m3d_gcode_set_value(out, 'S', b);
        m3d_ltoa(ms, b);      m3d_gcode_set_value(out, 'P', b);
    }
}
static void tack_wave(const m3d_gcode_t *pt, const m3d_gcode_t *ref, m3d_gcode_t *out) {
    m3d_gcode_clear(out);
    uint16_t distance = (uint16_t)ceil(get_distance(pt, ref));
    if (distance > 5) {
        char b[24];
        m3d_gcode_set_value(out, 'G', "4");
        m3d_ltoa(distance, b); m3d_gcode_set_value(out, 'P', b);
    }
}
static bool sharp_corner_thermal(const m3d_gcode_t *pt, const m3d_gcode_t *ref, double angle) {
    double cx = m3d_gcode_has_value(pt,'X') ? m3d_atod(m3d_gcode_get_value(pt,'X')) : 0;
    double cy = m3d_gcode_has_value(pt,'Y') ? m3d_atod(m3d_gcode_get_value(pt,'Y')) : 0;
    double px = m3d_gcode_has_value(ref,'X') ? m3d_atod(m3d_gcode_get_value(ref,'X')) : 0;
    double py = m3d_gcode_has_value(ref,'Y') ? m3d_atod(m3d_gcode_get_value(ref,'Y')) : 0;
    double denom = pow(cx*cx + cy*cy, 2) * pow(px*px + py*py, 2);
    if (!denom) return false;
    double value = acos((cx*px + cy*py) / denom);
    if (isnan(value)) return false;
    return value > 0 && value < angle / 180 * M_PI;
}
static bool sharp_corner_wave(const m3d_gcode_t *pt, const m3d_gcode_t *ref) {
    double cx = m3d_gcode_has_value(pt,'X') ? m3d_atod(m3d_gcode_get_value(pt,'X')) : 0;
    double cy = m3d_gcode_has_value(pt,'Y') ? m3d_atod(m3d_gcode_get_value(pt,'Y')) : 0;
    double px = m3d_gcode_has_value(ref,'X') ? m3d_atod(m3d_gcode_get_value(ref,'X')) : 0;
    double py = m3d_gcode_has_value(ref,'Y') ? m3d_atod(m3d_gcode_get_value(ref,'Y')) : 0;
    // NOTE: reproduces the reference's exact (quirky) expression verbatim.
    double denom = pow(cx*cx + cy + cy, 2) * pow(px*px + py + py, 2);
    if (!denom) return false;
    double value = acos((cx*px + cy + py) / denom);
    if (isnan(value)) return false;
    return value > 0 && value < M_PI_2;
}
static double current_adjustment_z(m3d_ctx_t *c) {
    c->waveStep = (c->waveStep + 1) % 4;
    double adj = c->waveStep ? (c->waveStep != 2 ? 0 : -1.5) : 1;
    return adj * WAVE_SIZE;
}
static uint16_t bounded_temp(int temperature, int maxTemperature) {
    int lo = temperature > 150 ? temperature : 150;
    return (uint16_t)(lo < maxTemperature ? lo : maxTemperature);
}

// ---------------- command stack ----------------
typedef struct { char *line; int origin; int skip; } cmd_t;
typedef struct { cmd_t *a; int n, cap; } cstack_t;
static int cs_init(cstack_t *s) { s->n=0; s->cap=64; s->a=malloc(sizeof(cmd_t)*s->cap); return s->a?0:-1; }
static void cs_free(cstack_t *s) { for(int i=0;i<s->n;i++) free(s->a[i].line); free(s->a); }
static int cs_push(cstack_t *s, const char *line, int origin, int skip) {
    if (s->n == s->cap) { int nc=s->cap*2; cmd_t *na=realloc(s->a, sizeof(cmd_t)*nc); if(!na) return -1; s->a=na; s->cap=nc; }
    int len = (int)strlen(line);
    char *c = malloc(len+1); if(!c) return -1; memcpy(c,line,len+1);
    s->a[s->n].line=c; s->a[s->n].origin=origin; s->a[s->n].skip=skip; s->n++; return 0;
}
static bool cs_empty(cstack_t *s) { return s->n == 0; }
// pops top into caller-owned buffers; returns the malloc'd line (caller frees)
static char *cs_pop(cstack_t *s, int *origin, int *skip) {
    s->n--; *origin=s->a[s->n].origin; *skip=s->a[s->n].skip; return s->a[s->n].line;
}

// local injected-command list (kept in emit order, prepended after current)
typedef struct { char lines[64][GC_LINE_MAX*2]; int origin[64]; int skip[64]; int n; } newcmds_t;
static void nc_reset(newcmds_t *nc) { nc->n = 0; }
static void nc_push(newcmds_t *nc, const char *line, int origin, int skip) {
    if (nc->n < 64) { strncpy(nc->lines[nc->n], line, GC_LINE_MAX*2-1);
        nc->lines[nc->n][GC_LINE_MAX*2-1]=0; nc->origin[nc->n]=origin; nc->skip[nc->n]=skip; nc->n++; }
}
// re-push current (finish later) then prepend the new commands in order.
static int prepend(cstack_t *s, const char *curLine, int curOrigin, int curSkip, newcmds_t *nc) {
    if (cs_push(s, curLine, curOrigin, curSkip) < 0) return -1;
    for (int i = nc->n - 1; i >= 0; i--)
        if (cs_push(s, nc->lines[i], nc->origin[i], nc->skip[i]) < 0) return -1;
    return 0;
}

// ---------------- defaults / reset / setters ----------------
void m3d_ctx_defaults(m3d_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->filamentTemperature = 215;
    ctx->filamentType = M3D_FIL_PLA;
    ctx->firmwareType = M3D_FW_IME;
    ctx->heatbedTemperature = 70;
    ctx->backlashX = 0.3; ctx->backlashY = 0.6; ctx->backlashSpeed = 1500;
    ctx->backRightOrientation = ctx->backLeftOrientation = 0;
    ctx->frontLeftOrientation = ctx->frontRightOrientation = 0;
    m3d_reset_settings(ctx);
}
void m3d_reset_settings(m3d_ctx_t *ctx) {
    ctx->printingTestBorder = false;
    ctx->printingBacklashCalibration = false;
    ctx->printerColor = M3D_CLR_BLACK;
    ctx->currentE = 0; ctx->currentF[0]=0; ctx->currentZ = 0;
    ctx->layerDetectionRelativeMode = false;
    ctx->printedLayerCount = 0;
    ctx->onNewPrintedLayer = false;
    ctx->tackPointAngle = 0; ctx->tackPointTime = 0;
    ctx->temperatureStabalizationDelay = 0; ctx->fanSpeed = 0;
    ctx->firstLayerTemperatureChange = 0;
    ctx->midPrintFilamentChangeLayerCounter = 0;
    ctx->displacementX = 0; ctx->displacementY = 0;
    ctx->addedIntro = false; ctx->addedOutro = false; ctx->preparationLayerCounter = 0;
    ctx->calibrateBeforePrint = false;
    ctx->waveStep = 0; ctx->waveBondingRelativeMode = false; ctx->waveBondingLayerCounter = 0;
    ctx->waveBondingChangesPlane = false;
    ctx->waveBondingPositionRelativeX = ctx->waveBondingPositionRelativeY = 0;
    ctx->waveBondingPositionRelativeZ = ctx->waveBondingPositionRelativeE = 0;
    m3d_gcode_clear(&ctx->waveBondingPreviousGcode);
    m3d_gcode_clear(&ctx->waveBondingRefrenceGcode);
    ctx->thermalBondingRelativeMode = false; ctx->thermalBondingLayerCounter = 0;
    m3d_gcode_clear(&ctx->thermalBondingPreviousGcode);
    m3d_gcode_clear(&ctx->thermalBondingRefrenceGcode);
    ctx->bedCompensationRelativeMode = false; ctx->bedCompensationChangesPlane = false;
    ctx->bedCompensationPositionAbsoluteX = ctx->bedCompensationPositionAbsoluteY = 0;
    ctx->bedCompensationPositionRelativeX = ctx->bedCompensationPositionRelativeY = 0;
    ctx->bedCompensationPositionRelativeZ = ctx->bedCompensationPositionRelativeE = 0;
    ctx->backlashCompensationRelativeMode = false;
    strcpy(ctx->valueF, "1000");
    ctx->previousDirectionX = ctx->previousDirectionY = DIR_NEITHER;
    ctx->compensationX = ctx->compensationY = 0;
    ctx->backlashPositionRelativeX = ctx->backlashPositionRelativeY = 0;
    ctx->backlashPositionRelativeZ = ctx->backlashPositionRelativeE = 0;
    ctx->skewCompensationRelativeMode = false;
    ctx->skewCompensationPositionAbsoluteZ = ctx->skewCompensationPositionRelativeZ = 0;
}
void m3d_set_filament_type(m3d_ctx_t *ctx, const char *v) {
    if (!strcmp(v,"ABS")) ctx->filamentType = M3D_FIL_ABS;
    else if (!strcmp(v,"PLA")) ctx->filamentType = M3D_FIL_PLA;
    else if (!strcmp(v,"HIPS")) ctx->filamentType = M3D_FIL_HIPS;
    else if (!strcmp(v,"FLX")) ctx->filamentType = M3D_FIL_FLX;
    else if (!strcmp(v,"TGH")) ctx->filamentType = M3D_FIL_TGH;
    else if (!strcmp(v,"CAM")) ctx->filamentType = M3D_FIL_CAM;
    else if (!strcmp(v,"ABS-R")) ctx->filamentType = M3D_FIL_ABS_R;
    else ctx->filamentType = M3D_FIL_OTHER;
}
void m3d_set_firmware_type(m3d_ctx_t *ctx, const char *v) {
    if (!strcmp(v,"M3D")) ctx->firmwareType = M3D_FW_M3D;
    else if (!strcmp(v,"M3D Mod")) ctx->firmwareType = M3D_FW_M3D_MOD;
    else if (!strcmp(v,"iMe")) ctx->firmwareType = M3D_FW_IME;
    else ctx->firmwareType = M3D_FW_UNKNOWN;
}
void m3d_set_printer_color(m3d_ctx_t *ctx, const char *v) {
    if (!strcmp(v,"White")) ctx->printerColor = M3D_CLR_WHITE;
    else if (!strcmp(v,"Blue")) ctx->printerColor = M3D_CLR_BLUE;
    else if (!strcmp(v,"Green")) ctx->printerColor = M3D_CLR_GREEN;
    else if (!strcmp(v,"Orange")) ctx->printerColor = M3D_CLR_ORANGE;
    else if (!strcmp(v,"Clear")) ctx->printerColor = M3D_CLR_CLEAR;
    else if (!strcmp(v,"Silver")) ctx->printerColor = M3D_CLR_SILVER;
    else if (!strcmp(v,"Purple")) ctx->printerColor = M3D_CLR_PURPLE;
    else ctx->printerColor = M3D_CLR_BLACK;
}

static bool is_fw_m3d(m3d_ctx_t *c) { return c->firmwareType == M3D_FW_M3D || c->firmwareType == M3D_FW_M3D_MOD; }

// =====================================================================
// collectPrintInformation
// =====================================================================
bool m3d_collect_print_information(m3d_ctx_t *c, const char *const *lines, int nlines, bool applyPreprocessors) {
    m3d_gcode_t gcode;
    int tier = TIER_LOW;
    bool relativeMode = false;
    double localX = NAN, localY = NAN, localZ = NAN;

    bedbounds_t bb;
    bb.bedMediumMaxZ = 73.5 - c->externalBedHeight;
    bb.bedHighMaxZ   = 112.0 - c->externalBedHeight;
    bb.bedHighMinZ   = bb.bedMediumMaxZ;
    bb.bedLowMinY    = c->expandPrintableRegion ? BED_MEDIUM_MIN_Y : -2.0;

    c->detectedFanSpeed = -1;
    c->detectedMidPrintFilamentChange = false;
    c->objectSuccessfullyCentered = true;

    c->maxXExtruderLow = c->maxXExtruderMedium = c->maxXExtruderHigh = -DBL_MAX_;
    c->maxYExtruderLow = c->maxYExtruderMedium = c->maxYExtruderHigh = -DBL_MAX_;
    c->maxZExtruder = -DBL_MAX_;
    c->minXExtruderLow = c->minXExtruderMedium = c->minXExtruderHigh = DBL_MAX_;
    c->minYExtruderLow = c->minYExtruderMedium = c->minYExtruderHigh = DBL_MAX_;
    c->minZExtruder = DBL_MAX_;

    for (int li = 0; li < nlines; li++) {
        if (!m3d_gcode_parse(&gcode, lines[li])) continue;

        if (c->detectedFanSpeed == -1 && m3d_gcode_has_value(&gcode,'M') && !strcmp(m3d_gcode_get_value(&gcode,'M'),"106")) {
            if (m3d_gcode_has_value(&gcode,'S'))
                c->detectedFanSpeed = (int16_t)d_max(m3d_atoi_g(m3d_gcode_get_value(&gcode,'S')), 255);
            else if (m3d_gcode_has_value(&gcode,'P'))
                c->detectedFanSpeed = (int16_t)d_max(m3d_atoi_g(m3d_gcode_get_value(&gcode,'P')), 255);
            else c->detectedFanSpeed = 0;
        }
        else if (!c->detectedMidPrintFilamentChange && m3d_gcode_has_value(&gcode,'M') && !strcmp(m3d_gcode_get_value(&gcode,'M'),"600"))
            c->detectedMidPrintFilamentChange = true;
        else if (m3d_gcode_has_value(&gcode,'G')) {
            int g = m3d_atoi_g(m3d_gcode_get_value(&gcode,'G'));
            if (g == 0 || g == 1) {
                if (m3d_gcode_has_value(&gcode,'X')) {
                    double cx = m3d_atod(m3d_gcode_get_value(&gcode,'X'));
                    localX = relativeMode ? (isnan(localX)?54:localX) + cx : cx;
                }
                if (m3d_gcode_has_value(&gcode,'Y')) {
                    double cy = m3d_atod(m3d_gcode_get_value(&gcode,'Y'));
                    localY = relativeMode ? (isnan(localY)?50:localY) + cy : cy;
                }
                if (m3d_gcode_has_value(&gcode,'Z')) {
                    double cz = m3d_atod(m3d_gcode_get_value(&gcode,'Z'));
                    localZ = relativeMode ? (isnan(localZ)?0.4:localZ) + cz : cz;
                    if (applyPreprocessors && !c->ignorePrintDimensionLimitations && !c->printingTestBorder &&
                        !c->printingBacklashCalibration && (localZ < BED_LOW_MIN_Z || localZ > bb.bedHighMaxZ))
                        return false;
                    if (localZ < BED_LOW_MAX_Z) tier = TIER_LOW;
                    else if (localZ < bb.bedMediumMaxZ) tier = TIER_MEDIUM;
                    else tier = TIER_HIGH;
                }
                if (tier == TIER_LOW) {
                    if (applyPreprocessors && !c->ignorePrintDimensionLimitations && !c->printingTestBorder && !c->printingBacklashCalibration && !c->useCenterModelPreprocessor &&
                        ((!isnan(localX) && (localX < BED_LOW_MIN_X || localX > BED_LOW_MAX_X)) || (!isnan(localY) && (localY < bb.bedLowMinY || localY > BED_LOW_MAX_Y))))
                        return false;
                    if (!isnan(localX)) { c->minXExtruderLow = d_min(c->minXExtruderLow, localX); c->maxXExtruderLow = d_max(c->maxXExtruderLow, localX); }
                    if (!isnan(localY)) { c->minYExtruderLow = d_min(c->minYExtruderLow, localY); c->maxYExtruderLow = d_max(c->maxYExtruderLow, localY); }
                } else if (tier == TIER_MEDIUM) {
                    if (applyPreprocessors && !c->ignorePrintDimensionLimitations && !c->printingTestBorder && !c->printingBacklashCalibration && !c->useCenterModelPreprocessor &&
                        ((!isnan(localX) && (localX < BED_MEDIUM_MIN_X || localX > BED_MEDIUM_MAX_X)) || (!isnan(localY) && (localY < BED_MEDIUM_MIN_Y || localY > BED_MEDIUM_MAX_Y))))
                        return false;
                    if (!isnan(localX)) { c->minXExtruderMedium = d_min(c->minXExtruderMedium, localX); c->maxXExtruderMedium = d_max(c->maxXExtruderMedium, localX); }
                    if (!isnan(localY)) { c->minYExtruderMedium = d_min(c->minYExtruderMedium, localY); c->maxYExtruderMedium = d_max(c->maxYExtruderMedium, localY); }
                } else {
                    if (applyPreprocessors && !c->ignorePrintDimensionLimitations && !c->printingTestBorder && !c->printingBacklashCalibration && !c->useCenterModelPreprocessor &&
                        ((!isnan(localX) && (localX < BED_HIGH_MIN_X || localX > BED_HIGH_MAX_X)) || (!isnan(localY) && (localY < BED_HIGH_MIN_Y || localY > BED_HIGH_MAX_Y))))
                        return false;
                    if (!isnan(localX)) { c->minXExtruderHigh = d_min(c->minXExtruderHigh, localX); c->maxXExtruderHigh = d_max(c->maxXExtruderHigh, localX); }
                    if (!isnan(localY)) { c->minYExtruderHigh = d_min(c->minYExtruderHigh, localY); c->maxYExtruderHigh = d_max(c->maxYExtruderHigh, localY); }
                }
                if (!isnan(localZ)) { c->minZExtruder = d_min(c->minZExtruder, localZ); c->maxZExtruder = d_max(c->maxZExtruder, localZ); }
            } else if (g == 28) { localX = 54; localY = 50; }
            else if (g == 90) relativeMode = false;
            else if (g == 91) relativeMode = true;
            else if (g == 92) {
                if (!m3d_gcode_has_value(&gcode,'X') && !m3d_gcode_has_value(&gcode,'Y') && !m3d_gcode_has_value(&gcode,'Z') && !m3d_gcode_has_value(&gcode,'E')) {
                    m3d_gcode_set_value(&gcode,'X',"0"); m3d_gcode_set_value(&gcode,'Y',"0");
                    m3d_gcode_set_value(&gcode,'Z',"0"); m3d_gcode_set_value(&gcode,'E',"0");
                }
                if (!is_fw_m3d(c)) {
                    if (m3d_gcode_has_value(&gcode,'X')) localX = m3d_atod(m3d_gcode_get_value(&gcode,'X'));
                    if (m3d_gcode_has_value(&gcode,'Y')) localY = m3d_atod(m3d_gcode_get_value(&gcode,'Y'));
                    if (m3d_gcode_has_value(&gcode,'Z')) localZ = m3d_atod(m3d_gcode_get_value(&gcode,'Z'));
                }
            }
        }
    }

    if (applyPreprocessors && c->useCenterModelPreprocessor && !c->printingTestBorder && !c->printingBacklashCalibration) {
        c->displacementX = (BED_WIDTH - BED_CENTER_OFFSET_X - d_max(c->maxXExtruderLow, d_max(c->maxXExtruderMedium, c->maxXExtruderHigh)) - d_min(c->minXExtruderLow, d_min(c->minXExtruderMedium, c->minXExtruderHigh)) - BED_CENTER_OFFSET_X) / 2;
        c->displacementY = (BED_DEPTH - BED_CENTER_OFFSET_Y - d_max(c->maxYExtruderLow, d_max(c->maxYExtruderMedium, c->maxYExtruderHigh)) - d_min(c->minYExtruderLow, d_min(c->minYExtruderMedium, c->minYExtruderHigh)) - BED_CENTER_OFFSET_Y) / 2;

        #define ADJ_MAXX(v) do{ if(c->v != -DBL_MAX_) c->v += c->displacementX; }while(0)
        #define ADJ_MAXY(v) do{ if(c->v != -DBL_MAX_) c->v += c->displacementY; }while(0)
        #define ADJ_MINX(v) do{ if(c->v != DBL_MAX_)  c->v += c->displacementX; }while(0)
        #define ADJ_MINY(v) do{ if(c->v != DBL_MAX_)  c->v += c->displacementY; }while(0)
        ADJ_MAXX(maxXExtruderLow); ADJ_MAXX(maxXExtruderMedium); ADJ_MAXX(maxXExtruderHigh);
        ADJ_MAXY(maxYExtruderLow); ADJ_MAXY(maxYExtruderMedium); ADJ_MAXY(maxYExtruderHigh);
        ADJ_MINX(minXExtruderLow); ADJ_MINX(minXExtruderMedium); ADJ_MINX(minXExtruderHigh);
        ADJ_MINY(minYExtruderLow); ADJ_MINY(minYExtruderMedium); ADJ_MINY(minYExtruderHigh);

        double negX = 0;
        negX = d_max(c->maxXExtruderLow - BED_LOW_MAX_X, negX);
        negX = d_max(c->maxXExtruderMedium - BED_MEDIUM_MAX_X, negX);
        negX = d_max(c->maxXExtruderHigh - BED_HIGH_MAX_X, negX);
        double posX = 0;
        posX = d_max(BED_LOW_MIN_X - c->minXExtruderLow, posX);
        posX = d_max(BED_MEDIUM_MIN_X - c->minXExtruderMedium, posX);
        posX = d_max(BED_HIGH_MIN_X - c->minXExtruderHigh, posX);
        double addX = 0;
        if (negX > 0 && posX <= 0) addX = -negX;
        else if (posX > 0 && negX <= 0) addX = posX;

        double negY = 0;
        negY = d_max(c->maxYExtruderLow - BED_LOW_MAX_Y, negY);
        negY = d_max(c->maxYExtruderMedium - BED_MEDIUM_MAX_Y, negY);
        negY = d_max(c->maxYExtruderHigh - BED_HIGH_MAX_Y, negY);
        double posY = 0;
        posY = d_max(bb.bedLowMinY - c->minYExtruderLow, posY);
        posY = d_max(BED_MEDIUM_MIN_Y - c->minYExtruderMedium, posY);
        posY = d_max(BED_HIGH_MIN_Y - c->minYExtruderHigh, posY);
        double addY = 0;
        if (negY > 0 && posY <= 0) addY = -negY;
        else if (posY > 0 && negY <= 0) addY = posY;

        if (addX != 0 || addY != 0) {
            c->objectSuccessfullyCentered = false;
            c->displacementX += addX; c->displacementY += addY;
            if (c->maxXExtruderLow != -DBL_MAX_) c->maxXExtruderLow += addX;
            if (c->maxXExtruderMedium != -DBL_MAX_) c->maxXExtruderMedium += addX;
            if (c->maxXExtruderHigh != -DBL_MAX_) c->maxXExtruderHigh += addX;
            if (c->maxYExtruderLow != -DBL_MAX_) c->maxYExtruderLow += addY;
            if (c->maxYExtruderMedium != -DBL_MAX_) c->maxYExtruderMedium += addY;
            if (c->maxYExtruderHigh != -DBL_MAX_) c->maxYExtruderHigh += addY;
            if (c->minXExtruderLow != DBL_MAX_) c->minXExtruderLow += addX;
            if (c->minXExtruderMedium != DBL_MAX_) c->minXExtruderMedium += addX;
            if (c->minXExtruderHigh != DBL_MAX_) c->minXExtruderHigh += addX;
            if (c->minYExtruderLow != DBL_MAX_) c->minYExtruderLow += addY;
            if (c->minYExtruderMedium != DBL_MAX_) c->minYExtruderMedium += addY;
            if (c->minYExtruderHigh != DBL_MAX_) c->minYExtruderHigh += addY;
        }

        if (!c->ignorePrintDimensionLimitations && (c->minZExtruder < BED_LOW_MIN_Z || c->maxZExtruder > bb.bedHighMaxZ ||
            c->maxXExtruderLow > BED_LOW_MAX_X || c->maxXExtruderMedium > BED_MEDIUM_MAX_X || c->maxXExtruderHigh > BED_HIGH_MAX_X ||
            c->maxYExtruderLow > BED_LOW_MAX_Y || c->maxYExtruderMedium > BED_MEDIUM_MAX_Y || c->maxYExtruderHigh > BED_HIGH_MAX_Y ||
            c->minXExtruderLow < BED_LOW_MIN_X || c->minXExtruderMedium < BED_MEDIUM_MIN_X || c->minXExtruderHigh < BED_HIGH_MIN_X ||
            c->minYExtruderLow < bb.bedLowMinY || c->minYExtruderMedium < BED_MEDIUM_MIN_Y || c->minYExtruderHigh < BED_HIGH_MIN_Y))
            return false;
    }

    int ft = c->filamentType;
    if (ft==M3D_FIL_PLA||ft==M3D_FIL_CAM||ft==M3D_FIL_ABS||ft==M3D_FIL_HIPS||ft==M3D_FIL_FLX||ft==M3D_FIL_TGH||ft==M3D_FIL_ABS_R) {
        c->tackPointAngle = 90; c->tackPointTime = 0.01;
        c->temperatureStabalizationDelay = (ft==M3D_FIL_PLA||ft==M3D_FIL_CAM||ft==M3D_FIL_FLX||ft==M3D_FIL_TGH) ? 15 : 10;
        c->fanSpeed = (ft==M3D_FIL_PLA||ft==M3D_FIL_CAM||ft==M3D_FIL_FLX||ft==M3D_FIL_TGH) ? 255 : 50;
        if (ft==M3D_FIL_PLA||ft==M3D_FIL_CAM) c->firstLayerTemperatureChange = 10;
        else if (ft==M3D_FIL_FLX||ft==M3D_FIL_TGH) c->firstLayerTemperatureChange = -5;
        else if (ft==M3D_FIL_ABS||ft==M3D_FIL_HIPS||ft==M3D_FIL_ABS_R) c->firstLayerTemperatureChange = 15;
    }
    if (c->detectedFanSpeed == -1 || c->removeFanCommands) c->detectedFanSpeed = 0;
    if (c->usePreparationPreprocessor || c->printingTestBorder || c->printingBacklashCalibration) c->detectedFanSpeed = c->fanSpeed;
    if (c->midPrintFilamentChangeLayerCount) c->detectedMidPrintFilamentChange = true;
    return true;
}

// small helper: append "<key><double>" style commands with std::to_string ints
static void fmt_i(char *dst, const char *prefix, long v) {
    char nb[24]; m3d_ltoa(v, nb);
    int o=0; while(prefix[o]){dst[o]=prefix[o];o++;} int k=0; while(nb[k]){dst[o++]=nb[k++];} dst[o]=0;
}
// =====================================================================
// preprocess
// =====================================================================
int m3d_preprocess(m3d_ctx_t *c, const char *const *lines, int nlines,
                   void (*emit)(void *user, const char *ascii_line), void *user) {
    cstack_t stack;
    if (cs_init(&stack) < 0) return -1;
    int cursor = 0;
    m3d_gcode_t gcode;
    char asc[GC_LINE_MAX*2];
    char linebuf[GC_LINE_MAX*2];

    bedbounds_t bb;
    bb.bedMediumMaxZ = 73.5 - c->externalBedHeight;
    bb.bedHighMaxZ   = 112.0 - c->externalBedHeight;
    bb.bedHighMinZ   = bb.bedMediumMaxZ;
    bb.bedLowMinY    = c->expandPrintableRegion ? BED_MEDIUM_MIN_Y : -2.0;

    for (;;) {
        if (cs_empty(&stack)) {
            if (cursor < nlines) { if (cs_push(&stack, lines[cursor++], ST_INPUT, ST_NONE) < 0) { cs_free(&stack); return -1; } }
            else break;
        }
        int origin, skip;
        char *popped = cs_pop(&stack, &origin, &skip);
        strncpy(linebuf, popped, sizeof(linebuf)-1); linebuf[sizeof(linebuf)-1]=0;
        free(popped);

        m3d_gcode_parse(&gcode, linebuf);
        if (!m3d_gcode_is_empty(&gcode)) {
            m3d_gcode_remove_parameter(&gcode, 'N');

            // ---- layer detection ----
            if (origin == ST_INPUT && skip == ST_NONE && m3d_gcode_has_value(&gcode,'G')) {
                double newE = c->currentE;
                int g = m3d_atoi_g(m3d_gcode_get_value(&gcode,'G'));
                if (g == 0 || g == 1) {
                    if (m3d_gcode_has_value(&gcode,'Z'))
                        c->currentZ = c->layerDetectionRelativeMode ? c->currentZ + gval(&gcode,'Z') : gval(&gcode,'Z');
                    if (m3d_gcode_has_value(&gcode,'E'))
                        newE = c->layerDetectionRelativeMode ? newE + gval(&gcode,'E') : gval(&gcode,'E');
                    if (newE > c->currentE) {
                        bool found = false;
                        for (int i=0;i<c->printedLayerCount;i++) if (c->printedLayers[i]==c->currentZ) { found=true; break; }
                        if (!found) {
                            if (c->printedLayerCount < (int)(sizeof(c->printedLayers)/sizeof(double)))
                                c->printedLayers[c->printedLayerCount++] = c->currentZ;
                            c->onNewPrintedLayer = true;
                        }
                    }
                } else if (g == 90) c->layerDetectionRelativeMode = false;
                else if (g == 91) c->layerDetectionRelativeMode = true;
                else if (g == 92) {
                    if (!m3d_gcode_has_value(&gcode,'X') && !m3d_gcode_has_value(&gcode,'Y') && !m3d_gcode_has_value(&gcode,'Z') && !m3d_gcode_has_value(&gcode,'E')) {
                        m3d_gcode_set_value(&gcode,'X',"0"); m3d_gcode_set_value(&gcode,'Y',"0");
                        m3d_gcode_set_value(&gcode,'Z',"0"); m3d_gcode_set_value(&gcode,'E',"0");
                    }
                    if (m3d_gcode_has_value(&gcode,'Z')) c->currentZ = gval(&gcode,'Z');
                    if (m3d_gcode_has_value(&gcode,'E')) newE = gval(&gcode,'E');
                }
                c->currentE = newE;
            }
        }

        newcmds_t nc;

        // ---- mid-print filament change ----
        if (!c->printingTestBorder && !c->printingBacklashCalibration && c->midPrintFilamentChangeLayerCount && skip < ST_MID_PRINT) {
            skip = ST_MID_PRINT;
            if (c->onNewPrintedLayer) {
                c->midPrintFilamentChangeLayerCounter++;
                bool at = false;
                for (int i=0;i<c->midPrintFilamentChangeLayerCount;i++) if (c->midPrintFilamentChangeLayers[i]==c->midPrintFilamentChangeLayerCounter){at=true;break;}
                if (at) {
                    nc_reset(&nc);
                    nc_push(&nc, "M600", ST_MID_PRINT, ST_MID_PRINT);
                    if (!m3d_gcode_is_empty(&gcode)) { m3d_gcode_get_ascii(&gcode, asc); if (prepend(&stack, asc, origin, skip, &nc) < 0) { cs_free(&stack); return -1; } }
                    else if (prepend(&stack, linebuf, origin, skip, &nc) < 0) { cs_free(&stack); return -1; }
                    continue;
                }
            }
        }

        // ---- center model ----
        if (!c->printingTestBorder && !c->printingBacklashCalibration && c->useCenterModelPreprocessor && skip < ST_CENTER) {
            skip = ST_CENTER;
            if (!m3d_gcode_is_empty(&gcode) && m3d_gcode_has_value(&gcode,'G') &&
                (!strcmp(m3d_gcode_get_value(&gcode,'G'),"0") || !strcmp(m3d_gcode_get_value(&gcode,'G'),"1"))) {
                if (m3d_gcode_has_value(&gcode,'X')) set_val_d(&gcode,'X', gval(&gcode,'X') + c->displacementX);
                if (m3d_gcode_has_value(&gcode,'Y')) set_val_d(&gcode,'Y', gval(&gcode,'Y') + c->displacementY);
            }
        }

        // ---- validation ----
        if ((c->printingTestBorder || c->printingBacklashCalibration || c->useValidationPreprocessor) && skip < ST_VALIDATION) {
            skip = ST_VALIDATION;
            if (!m3d_gcode_is_empty(&gcode)) {
                const char *mv = m3d_gcode_get_value(&gcode,'M');
                if (m3d_gcode_has_value(&gcode,'M') && (!strcmp(mv,"82")||!strcmp(mv,"83")||!strcmp(mv,"84")||!strcmp(mv,"117")|| (is_fw_m3d(c) && !strcmp(mv,"105")))) continue;
                const char *gv = m3d_gcode_get_value(&gcode,'G');
                if (m3d_gcode_has_value(&gcode,'G') && (!strcmp(gv,"21")||!strcmp(gv,"28"))) continue;
                if (m3d_gcode_has_parameter(&gcode,'T')) { m3d_gcode_remove_parameter(&gcode,'T'); if (m3d_gcode_is_empty(&gcode)) continue; }
                if (c->removeFanCommands && m3d_gcode_has_value(&gcode,'M') && (!strcmp(mv,"106")||!strcmp(mv,"107"))) continue;
                if (c->removeTemperatureCommands && m3d_gcode_has_value(&gcode,'M') && (!strcmp(mv,"104")||!strcmp(mv,"109")||!strcmp(mv,"140")||!strcmp(mv,"190"))) continue;
            }
        }

        // ---- preparation ----
        if ((c->printingTestBorder || c->printingBacklashCalibration || c->usePreparationPreprocessor) && skip < ST_PREPARATION) {
            skip = ST_PREPARATION;
            if (!c->addedIntro) {
                c->addedIntro = true;
                nc_reset(&nc);
                double cornerX = 0, cornerY = 0, cornerZ = 0;
                if (!c->printingTestBorder) {
                    if (c->minXExtruderLow > BED_LOW_MIN_X) cornerX = -(BED_LOW_MAX_X - BED_LOW_MIN_X) / 2;
                    else if (c->maxXExtruderLow < BED_LOW_MAX_X) cornerX = (BED_LOW_MAX_X - BED_LOW_MIN_X) / 2;
                    if (c->minYExtruderLow > bb.bedLowMinY) cornerY = -(BED_LOW_MAX_Y - bb.bedLowMinY - 10) / 2;
                    else if (c->maxYExtruderLow < BED_LOW_MAX_Y) cornerY = (BED_LOW_MAX_Y - bb.bedLowMinY - 10) / 2;
                }
                if (is_fw_m3d(c) && cornerX && cornerY) {
                    if (cornerX > 0 && cornerY > 0) cornerZ = c->backRightOrientation + c->backRightOffset;
                    else if (cornerX < 0 && cornerY > 0) cornerZ = c->backLeftOrientation + c->backLeftOffset;
                    else if (cornerX < 0 && cornerY < 0) cornerZ = c->frontLeftOrientation + c->frontLeftOffset;
                    else if (cornerX > 0 && cornerY < 0) cornerZ = c->frontRightOrientation + c->frontRightOffset;
                }
                // NOTE: pushed in reference order; nc preserves that order.
                nc_push(&nc, "G90", ST_PREPARATION, ST_PREPARATION);
                if (c->changeLedBrightness) nc_push(&nc, "M420 T1", ST_PREPARATION, ST_PREPARATION);
                if (c->calibrateBeforePrint) {
                    char b[64];
                    fmt_i(b, "M618 S", EEPROM_BED_HEIGHT_OFFSET_OFFSET);
                    { char t[24]; m3d_ltoa(EEPROM_BED_HEIGHT_OFFSET_LENGTH, t); strcat(b," T"); strcat(b,t); strcat(b," P"); char p[24]; m3d_ltoa(0,p); strcat(b,p);}
                    nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION);
                    fmt_i(b, "M619 S", EEPROM_BED_HEIGHT_OFFSET_OFFSET);
                    { char t[24]; m3d_ltoa(EEPROM_BED_HEIGHT_OFFSET_LENGTH, t); strcat(b," T"); strcat(b,t);}
                    nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION);
                    c->bedHeightOffset = 0;
                    nc_push(&nc, "G91", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G0 Z3 F90", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G90", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "M109 S150", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "M104 S0", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "M107", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G30", ST_PREPARATION, ST_PREPARATION);
                }
                nc_push(&nc, "M106 S1", ST_PREPARATION, ST_PREPARATION);
                nc_push(&nc, "M17", ST_PREPARATION, ST_PREPARATION);
                nc_push(&nc, "G90", ST_PREPARATION, ST_PREPARATION);
                { char b[32]; fmt_i(b, "M104 S", c->filamentTemperature); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION); }
                nc_push(&nc, "G0 Z5 F48", ST_PREPARATION, ST_PREPARATION);
                if (is_fw_m3d(c)) nc_push(&nc, "G28", ST_PREPARATION, ST_PREPARATION);
                if (!cornerX || !cornerY) {
                    nc_push(&nc, "M18", ST_PREPARATION, ST_PREPARATION);
                    if (c->usingHeatbed) { char b[32]; fmt_i(b,"M190 S", c->heatbedTemperature); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION); }
                    { char b[32]; fmt_i(b,"M109 S", c->filamentTemperature); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION); }
                    if (c->temperatureStabalizationDelay) { char b[32]; fmt_i(b,"G4 S", c->temperatureStabalizationDelay); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION); }
                    { char b[32]; fmt_i(b,"M106 S", c->fanSpeed); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION); }
                    nc_push(&nc, "M17", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G92 E0", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G0 Z0.4 F48", ST_PREPARATION, ST_PREPARATION);
                } else {
                    char b[96];
                    { char x[40],y[40]; m3d_dtoa(54 + cornerX, x); m3d_dtoa(50 + cornerY, y);
                      strcpy(b,"G0 X"); strcat(b,x); strcat(b," Y"); strcat(b,y); strcat(b," F1800"); }
                    nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "M18", ST_PREPARATION, ST_PREPARATION);
                    if (c->usingHeatbed) { char h[32]; fmt_i(h,"M190 S", c->heatbedTemperature); nc_push(&nc, h, ST_PREPARATION, ST_PREPARATION); }
                    { char h[32]; fmt_i(h,"M109 S", c->filamentTemperature); nc_push(&nc, h, ST_PREPARATION, ST_PREPARATION); }
                    if (c->temperatureStabalizationDelay) { char h[32]; fmt_i(h,"G4 S", c->temperatureStabalizationDelay); nc_push(&nc, h, ST_PREPARATION, ST_PREPARATION); }
                    { char h[32]; fmt_i(h,"M106 S", c->fanSpeed); nc_push(&nc, h, ST_PREPARATION, ST_PREPARATION); }
                    nc_push(&nc, "M17", ST_PREPARATION, ST_PREPARATION);
                    { char z[40]; m3d_dtoa(cornerZ + 3, z); strcpy(b,"G0 Z"); strcat(b,z); strcat(b," F48"); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION); }
                    nc_push(&nc, "G92 E0", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G0 E10 F360", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G4 S3", ST_PREPARATION, ST_PREPARATION);
                    { char x[40],y[40],z[40]; m3d_dtoa(54 + cornerX - cornerX*0.1, x); m3d_dtoa(50 + cornerY - cornerY*0.1, y); m3d_dtoa(cornerZ + 0.5, z);
                      strcpy(b,"G0 X"); strcat(b,x); strcat(b," Y"); strcat(b,y); strcat(b," Z"); strcat(b,z); strcat(b," F400"); }
                    nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G92 E0", ST_PREPARATION, ST_PREPARATION);
                }
                if (!m3d_gcode_is_empty(&gcode)) { m3d_gcode_get_ascii(&gcode, asc); if (prepend(&stack, asc, origin, skip, &nc) < 0) { cs_free(&stack); return -1; } }
                else if (prepend(&stack, linebuf, origin, skip, &nc) < 0) { cs_free(&stack); return -1; }
                continue;
            }

            if (!c->addedOutro && cs_empty(&stack) && cursor >= nlines) {
                c->addedOutro = true;
                nc_reset(&nc);
                double moveZ = c->maxZExtruder + 10;
                if (moveZ > bb.bedHighMaxZ) moveZ = bb.bedHighMaxZ;
                double startingMoveY = c->maxYExtruderLow;
                double maxMoveY = BED_LOW_MAX_Y;
                if (moveZ >= bb.bedMediumMaxZ) {
                    if (c->maxYExtruderMedium != -DBL_MAX_) startingMoveY = d_max(startingMoveY, c->maxYExtruderMedium);
                    if (c->maxYExtruderHigh != -DBL_MAX_) startingMoveY = d_max(startingMoveY, c->maxYExtruderHigh);
                    maxMoveY = BED_HIGH_MAX_Y;
                } else if (moveZ >= BED_LOW_MAX_Z) {
                    if (c->maxYExtruderMedium != -DBL_MAX_) startingMoveY = d_max(startingMoveY, c->maxYExtruderMedium);
                    maxMoveY = BED_MEDIUM_MAX_Y;
                }
                double moveY = startingMoveY + 20;
                if (moveY > maxMoveY) moveY = maxMoveY;
                nc_push(&nc, "G90", ST_PREPARATION, ST_PREPARATION);
                nc_push(&nc, "G92 E0", ST_PREPARATION, ST_PREPARATION);
                { char b[96], y[40], z[40]; m3d_dtoa(moveY, y); m3d_dtoa(moveZ, z);
                  strcpy(b,"G0 Y"); strcat(b,y); strcat(b," Z"); strcat(b,z); strcat(b," E-8 F1800"); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION); }
                nc_push(&nc, "M104 S0", ST_PREPARATION, ST_PREPARATION);
                if (c->usingHeatbed) nc_push(&nc, "M140 S0", ST_PREPARATION, ST_PREPARATION);
                if (c->useGpio) nc_push(&nc, "M107 T1", ST_PREPARATION, ST_PREPARATION);
                nc_push(&nc, "M18", ST_PREPARATION, ST_PREPARATION);
                nc_push(&nc, "M107", ST_PREPARATION, ST_PREPARATION);
                if (c->changeLedBrightness) {
                    const char *tt = (c->printerColor == M3D_CLR_CLEAR) ? "20" : "100";
                    char b[24];
                    strcpy(b,"M420 T"); strcat(b,tt); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G4 P500", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "M420 T1", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G4 P500", ST_PREPARATION, ST_PREPARATION);
                    strcpy(b,"M420 T"); strcat(b,tt); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G4 P500", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "M420 T1", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G4 P500", ST_PREPARATION, ST_PREPARATION);
                    strcpy(b,"M420 T"); strcat(b,tt); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G4 P500", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "M420 T1", ST_PREPARATION, ST_PREPARATION);
                    nc_push(&nc, "G4 P500", ST_PREPARATION, ST_PREPARATION);
                    strcpy(b,"M420 T"); strcat(b,tt); nc_push(&nc, b, ST_PREPARATION, ST_PREPARATION);
                }
                // append outro to front (does NOT re-push current, does NOT continue)
                for (int i = nc.n - 1; i >= 0; i--)
                    if (cs_push(&stack, nc.lines[i], nc.origin[i], nc.skip[i]) < 0) { cs_free(&stack); return -1; }
            }
            else if (c->onNewPrintedLayer) {
                c->preparationLayerCounter++;
                if (c->useGpio && c->preparationLayerCounter == c->gpioLayer) {
                    nc_reset(&nc);
                    nc_push(&nc, "M106 T1", ST_PREPARATION, ST_PREPARATION);
                    if (!m3d_gcode_is_empty(&gcode)) { m3d_gcode_get_ascii(&gcode, asc); if (prepend(&stack, asc, origin, skip, &nc) < 0) { cs_free(&stack); return -1; } }
                    else if (prepend(&stack, linebuf, origin, skip, &nc) < 0) { cs_free(&stack); return -1; }
                    continue;
                }
            }
        }

        // ---- wave bonding ----
        if (!c->printingTestBorder && !c->printingBacklashCalibration && c->useWaveBondingPreprocessor && skip < ST_WAVE) {
            skip = ST_WAVE;
            nc_reset(&nc);
            if (c->waveBondingLayerCounter < 2 && c->onNewPrintedLayer) c->waveBondingLayerCounter++;
            if (c->waveBondingLayerCounter == 1 && !m3d_gcode_is_empty(&gcode) && m3d_gcode_has_value(&gcode,'G')) {
                const char *gv = m3d_gcode_get_value(&gcode,'G');
                if ((!strcmp(gv,"0")||!strcmp(gv,"1")) && !c->waveBondingRelativeMode) {
                    if (m3d_gcode_has_value(&gcode,'X') || m3d_gcode_has_value(&gcode,'Y')) c->waveBondingChangesPlane = true;
                    double deltaX = !m3d_gcode_has_value(&gcode,'X') ? 0 : gval(&gcode,'X') - c->waveBondingPositionRelativeX;
                    double deltaY = !m3d_gcode_has_value(&gcode,'Y') ? 0 : gval(&gcode,'Y') - c->waveBondingPositionRelativeY;
                    double deltaZ = !m3d_gcode_has_value(&gcode,'Z') ? 0 : gval(&gcode,'Z') - c->waveBondingPositionRelativeZ;
                    double deltaE = !m3d_gcode_has_value(&gcode,'E') ? 0 : gval(&gcode,'E') - c->waveBondingPositionRelativeE;
                    c->waveBondingPositionRelativeX += deltaX; c->waveBondingPositionRelativeY += deltaY;
                    c->waveBondingPositionRelativeZ += deltaZ; c->waveBondingPositionRelativeE += deltaE;
                    double distance = sqrt(deltaX*deltaX + deltaY*deltaY);
                    uint32_t waveRatio = distance > WAVE_PERIOD_QUARTER ? (uint32_t)(distance / WAVE_PERIOD_QUARTER) : 1;
                    double rdX = c->waveBondingPositionRelativeX - deltaX, rdY = c->waveBondingPositionRelativeY - deltaY;
                    double rdZ = c->waveBondingPositionRelativeZ - deltaZ, rdE = c->waveBondingPositionRelativeE - deltaE;
                    double drX, drY, drZ, drE;
                    if (distance) { drX=deltaX/distance; drY=deltaY/distance; drZ=deltaZ/distance; drE=deltaE/distance; }
                    else { drX=drY=drZ=drE=0; }
                    if (deltaE > 0) {
                        if (!m3d_gcode_is_empty(&c->waveBondingPreviousGcode) && sharp_corner_wave(&gcode, &c->waveBondingPreviousGcode)) {
                            m3d_gcode_t tp; tack_wave(&gcode, &c->waveBondingRefrenceGcode, &tp);
                            if (!m3d_gcode_is_empty(&tp)) { m3d_gcode_get_ascii(&tp, asc); nc_push(&nc, asc, ST_WAVE, ST_WAVE); }
                            c->waveBondingRefrenceGcode = gcode;
                        }
                        for (uint32_t i = 1; i <= waveRatio; i++) {
                            double tX,tY,tZ,tE;
                            if (i == waveRatio) { tX=c->waveBondingPositionRelativeX; tY=c->waveBondingPositionRelativeY; tZ=c->waveBondingPositionRelativeZ; tE=c->waveBondingPositionRelativeE; }
                            else { tX=rdX + i*WAVE_PERIOD_QUARTER*drX; tY=rdY + i*WAVE_PERIOD_QUARTER*drY; tZ=rdZ + i*WAVE_PERIOD_QUARTER*drZ; tE=rdE + i*WAVE_PERIOD_QUARTER*drE; }
                            if (i != waveRatio) {
                                m3d_gcode_t ex; m3d_gcode_clear(&ex);
                                m3d_gcode_set_value(&ex,'G', m3d_gcode_get_value(&gcode,'G'));
                                if (m3d_gcode_has_value(&gcode,'X')) set_val_d(&ex,'X', c->waveBondingPositionRelativeX - deltaX + tX - rdX);
                                if (m3d_gcode_has_value(&gcode,'Y')) set_val_d(&ex,'Y', c->waveBondingPositionRelativeY - deltaY + tY - rdY);
                                if (m3d_gcode_has_value(&gcode,'F') && i == 1) m3d_gcode_set_value(&ex,'F', m3d_gcode_get_value(&gcode,'F'));
                                if (c->waveBondingChangesPlane) set_val_d(&ex,'Z', c->waveBondingPositionRelativeZ - deltaZ + tZ - rdZ + current_adjustment_z(c));
                                else if (m3d_gcode_has_value(&gcode,'Z') && deltaZ != DBL_EPS) set_val_d(&ex,'Z', c->waveBondingPositionRelativeZ - deltaZ + tZ - rdZ);
                                set_val_d(&ex,'E', c->waveBondingPositionRelativeE - deltaE + tE - rdE);
                                m3d_gcode_get_ascii(&ex, asc); nc_push(&nc, asc, ST_WAVE, ST_WAVE);
                            } else if (c->waveBondingChangesPlane) {
                                if (m3d_gcode_has_value(&gcode,'Z')) set_val_d(&gcode,'Z', gval(&gcode,'Z') + current_adjustment_z(c));
                                else set_val_d(&gcode,'Z', rdZ + deltaZ + current_adjustment_z(c));
                            }
                        }
                    }
                    c->waveBondingPreviousGcode = gcode;
                    if (m3d_gcode_is_empty(&c->waveBondingRefrenceGcode)) c->waveBondingRefrenceGcode = gcode;
                } else if (!strcmp(gv,"28")) { c->waveBondingPositionRelativeX = 54; c->waveBondingPositionRelativeY = 50; }
                else if (!strcmp(gv,"90")) c->waveBondingRelativeMode = false;
                else if (!strcmp(gv,"91")) c->waveBondingRelativeMode = true;
                else if (!strcmp(gv,"92")) {
                    if (!m3d_gcode_has_value(&gcode,'X') && !m3d_gcode_has_value(&gcode,'Y') && !m3d_gcode_has_value(&gcode,'Z') && !m3d_gcode_has_value(&gcode,'E')) {
                        m3d_gcode_set_value(&gcode,'X',"0"); m3d_gcode_set_value(&gcode,'Y',"0"); m3d_gcode_set_value(&gcode,'Z',"0"); m3d_gcode_set_value(&gcode,'E',"0");
                    }
                    if (!is_fw_m3d(c)) {
                        if (m3d_gcode_has_value(&gcode,'X')) c->waveBondingPositionRelativeX = gval(&gcode,'X');
                        if (m3d_gcode_has_value(&gcode,'Y')) c->waveBondingPositionRelativeY = gval(&gcode,'Y');
                        if (m3d_gcode_has_value(&gcode,'Z')) c->waveBondingPositionRelativeZ = gval(&gcode,'Z');
                    }
                    if (m3d_gcode_has_value(&gcode,'E')) c->waveBondingPositionRelativeE = gval(&gcode,'E');
                }
            }
            if (nc.n) {
                if (!m3d_gcode_is_empty(&gcode)) { m3d_gcode_get_ascii(&gcode, asc); if (prepend(&stack, asc, origin, skip, &nc) < 0) { cs_free(&stack); return -1; } }
                else if (prepend(&stack, linebuf, origin, skip, &nc) < 0) { cs_free(&stack); return -1; }
                continue;
            }
        }

        // ---- thermal bonding ----
        if ((c->printingTestBorder || c->printingBacklashCalibration || c->useThermalBondingPreprocessor) && skip < ST_THERMAL) {
            skip = ST_THERMAL;
            nc_reset(&nc);
            if (c->thermalBondingLayerCounter < 2 && c->onNewPrintedLayer) {
                c->thermalBondingLayerCounter++;
                if (c->thermalBondingLayerCounter == 1) {
                    char b[32]; fmt_i(b,"M109 S", bounded_temp(c->filamentTemperature + c->firstLayerTemperatureChange, c->firmwareType == M3D_FW_M3D_MOD ? 315 : 285));
                    nc_push(&nc, b, ST_THERMAL, ST_THERMAL);
                } else { char b[32]; fmt_i(b,"M104 S", c->filamentTemperature); nc_push(&nc, b, ST_THERMAL, ST_THERMAL); }
            }
            if (c->thermalBondingLayerCounter == 1 && !m3d_gcode_is_empty(&gcode)) {
                if ((c->printingTestBorder || !c->useWaveBondingPreprocessor) && m3d_gcode_has_value(&gcode,'G')) {
                    const char *gv = m3d_gcode_get_value(&gcode,'G');
                    if ((!strcmp(gv,"0")||!strcmp(gv,"1")) && !c->thermalBondingRelativeMode) {
                        if (c->tackPointAngle != 0 && c->tackPointTime >= 0.001) {
                            if (!m3d_gcode_is_empty(&c->thermalBondingPreviousGcode) && sharp_corner_thermal(&gcode, &c->thermalBondingPreviousGcode, c->tackPointAngle)) {
                                m3d_gcode_t tp; tack_thermal(&gcode, &c->thermalBondingRefrenceGcode, c->tackPointTime, &tp);
                                if (!m3d_gcode_is_empty(&tp)) { m3d_gcode_get_ascii(&tp, asc); nc_push(&nc, asc, ST_THERMAL, ST_THERMAL); }
                                c->thermalBondingRefrenceGcode = gcode;
                            }
                            c->thermalBondingPreviousGcode = gcode;
                            if (m3d_gcode_is_empty(&c->thermalBondingRefrenceGcode)) c->thermalBondingRefrenceGcode = gcode;
                        }
                    } else if (!strcmp(gv,"90")) c->thermalBondingRelativeMode = false;
                    else if (!strcmp(gv,"91")) c->thermalBondingRelativeMode = true;
                }
            }
            if (nc.n) {
                if (!m3d_gcode_is_empty(&gcode)) { m3d_gcode_get_ascii(&gcode, asc); if (prepend(&stack, asc, origin, skip, &nc) < 0) { cs_free(&stack); return -1; } }
                else if (prepend(&stack, linebuf, origin, skip, &nc) < 0) { cs_free(&stack); return -1; }
                continue;
            }
        }

        // ---- bed compensation (M3D/M3D Mod firmware) ----
        if (is_fw_m3d(c) && (c->printingTestBorder || c->printingBacklashCalibration || c->useBedCompensationPreprocessor) && skip < ST_BED) {
            skip = ST_BED;
            nc_reset(&nc);
            if (!m3d_gcode_is_empty(&gcode) && m3d_gcode_has_value(&gcode,'G')) {
                const char *gv = m3d_gcode_get_value(&gcode,'G');
                if ((!strcmp(gv,"0")||!strcmp(gv,"1")) && !c->bedCompensationRelativeMode) {
                    if (m3d_gcode_has_value(&gcode,'X') || m3d_gcode_has_value(&gcode,'Y')) c->bedCompensationChangesPlane = true;
                    if (m3d_gcode_has_value(&gcode,'Z')) set_val_d(&gcode,'Z', gval(&gcode,'Z') + c->bedHeightOffset);
                    double deltaX = !m3d_gcode_has_value(&gcode,'X') ? 0 : gval(&gcode,'X') - c->bedCompensationPositionRelativeX;
                    double deltaY = !m3d_gcode_has_value(&gcode,'Y') ? 0 : gval(&gcode,'Y') - c->bedCompensationPositionRelativeY;
                    double deltaZ = !m3d_gcode_has_value(&gcode,'Z') ? 0 : gval(&gcode,'Z') - c->bedCompensationPositionRelativeZ;
                    double deltaE = !m3d_gcode_has_value(&gcode,'E') ? 0 : gval(&gcode,'E') - c->bedCompensationPositionRelativeE;
                    c->bedCompensationPositionAbsoluteX += deltaX; c->bedCompensationPositionAbsoluteY += deltaY;
                    c->bedCompensationPositionRelativeX += deltaX; c->bedCompensationPositionRelativeY += deltaY;
                    c->bedCompensationPositionRelativeZ += deltaZ; c->bedCompensationPositionRelativeE += deltaE;
                    double distance = sqrt(deltaX*deltaX + deltaY*deltaY);
                    uint32_t seg = distance > SEGMENT_LENGTH ? (uint32_t)(distance / SEGMENT_LENGTH) : 1;
                    double adX = c->bedCompensationPositionAbsoluteX - deltaX, adY = c->bedCompensationPositionAbsoluteY - deltaY;
                    double rdX = c->bedCompensationPositionRelativeX - deltaX, rdY = c->bedCompensationPositionRelativeY - deltaY;
                    double rdZ = c->bedCompensationPositionRelativeZ - deltaZ, rdE = c->bedCompensationPositionRelativeE - deltaE;
                    double drX, drY, drZ, drE;
                    if (distance) { drX=deltaX/distance; drY=deltaY/distance; drZ=deltaZ/distance; drE=deltaE/distance; }
                    else { drX=drY=drZ=drE=0; }
                    if (deltaE > 0) {
                        for (uint32_t i = 1; i <= seg; i++) {
                            double taX,taY,tX,tY,tZ,tE;
                            if (i == seg) { taX=c->bedCompensationPositionAbsoluteX; taY=c->bedCompensationPositionAbsoluteY; tX=c->bedCompensationPositionRelativeX; tY=c->bedCompensationPositionRelativeY; tZ=c->bedCompensationPositionRelativeZ; tE=c->bedCompensationPositionRelativeE; }
                            else { taX=adX+i*SEGMENT_LENGTH*drX; taY=adY+i*SEGMENT_LENGTH*drY; tX=rdX+i*SEGMENT_LENGTH*drX; tY=rdY+i*SEGMENT_LENGTH*drY; tZ=rdZ+i*SEGMENT_LENGTH*drZ; tE=rdE+i*SEGMENT_LENGTH*drE; }
                            double ha = get_height_adjustment(c, &bb, taX, taY);
                            if (i != seg) {
                                m3d_gcode_t ex; m3d_gcode_clear(&ex);
                                m3d_gcode_set_value(&ex,'G', m3d_gcode_get_value(&gcode,'G'));
                                if (m3d_gcode_has_value(&gcode,'X')) set_val_d(&ex,'X', c->bedCompensationPositionRelativeX - deltaX + tX - rdX);
                                if (m3d_gcode_has_value(&gcode,'Y')) set_val_d(&ex,'Y', c->bedCompensationPositionRelativeY - deltaY + tY - rdY);
                                if (m3d_gcode_has_value(&gcode,'F') && i == 1) m3d_gcode_set_value(&ex,'F', m3d_gcode_get_value(&gcode,'F'));
                                if (c->bedCompensationChangesPlane) set_val_d(&ex,'Z', c->bedCompensationPositionRelativeZ - deltaZ + tZ - rdZ + ha);
                                else if (m3d_gcode_has_value(&gcode,'Z') && deltaZ != DBL_EPS) set_val_d(&ex,'Z', c->bedCompensationPositionRelativeZ - deltaZ + tZ - rdZ);
                                set_val_d(&ex,'E', c->bedCompensationPositionRelativeE - deltaE + tE - rdE);
                                m3d_gcode_get_ascii(&ex, asc); nc_push(&nc, asc, ST_BED, ST_BED);
                            } else if (c->bedCompensationChangesPlane) {
                                if (m3d_gcode_has_value(&gcode,'Z')) set_val_d(&gcode,'Z', gval(&gcode,'Z') + ha);
                                else set_val_d(&gcode,'Z', rdZ + deltaZ + ha);
                            }
                        }
                    } else if (c->bedCompensationChangesPlane) {
                        double ha = get_height_adjustment(c, &bb, c->bedCompensationPositionAbsoluteX, c->bedCompensationPositionAbsoluteY);
                        if (m3d_gcode_has_value(&gcode,'Z')) set_val_d(&gcode,'Z', gval(&gcode,'Z') + ha);
                        else set_val_d(&gcode,'Z', c->bedCompensationPositionRelativeZ + ha);
                    }
                } else if (!strcmp(gv,"28")) { c->bedCompensationPositionRelativeX = c->bedCompensationPositionAbsoluteX = 54; c->bedCompensationPositionRelativeY = c->bedCompensationPositionAbsoluteY = 50; }
                else if (!strcmp(gv,"90")) c->bedCompensationRelativeMode = false;
                else if (!strcmp(gv,"91")) c->bedCompensationRelativeMode = true;
                else if (!strcmp(gv,"92")) {
                    if (!m3d_gcode_has_value(&gcode,'X') && !m3d_gcode_has_value(&gcode,'Y') && !m3d_gcode_has_value(&gcode,'Z') && !m3d_gcode_has_value(&gcode,'E')) {
                        m3d_gcode_set_value(&gcode,'X',"0"); m3d_gcode_set_value(&gcode,'Y',"0"); m3d_gcode_set_value(&gcode,'Z',"0"); m3d_gcode_set_value(&gcode,'E',"0");
                    }
                    if (!is_fw_m3d(c)) {
                        if (m3d_gcode_has_value(&gcode,'X')) c->bedCompensationPositionRelativeX = c->bedCompensationPositionAbsoluteX = gval(&gcode,'X');
                        if (m3d_gcode_has_value(&gcode,'Y')) c->bedCompensationPositionRelativeY = c->bedCompensationPositionAbsoluteY = gval(&gcode,'Y');
                        if (m3d_gcode_has_value(&gcode,'Z')) c->bedCompensationPositionRelativeZ = gval(&gcode,'Z');
                    }
                    if (m3d_gcode_has_value(&gcode,'E')) c->bedCompensationPositionRelativeE = gval(&gcode,'E');
                }
            }
            if (nc.n) {
                if (!m3d_gcode_is_empty(&gcode)) { m3d_gcode_get_ascii(&gcode, asc); if (prepend(&stack, asc, origin, skip, &nc) < 0) { cs_free(&stack); return -1; } }
                else if (prepend(&stack, linebuf, origin, skip, &nc) < 0) { cs_free(&stack); return -1; }
                continue;
            }
        }

        // ---- backlash compensation (M3D/M3D Mod firmware) ----
        if (is_fw_m3d(c) && (c->printingTestBorder || c->printingBacklashCalibration || c->useBacklashCompensationPreprocessor) && skip < ST_BACKLASH) {
            skip = ST_BACKLASH;
            nc_reset(&nc);
            if (!m3d_gcode_is_empty(&gcode) && m3d_gcode_has_value(&gcode,'G')) {
                const char *gv = m3d_gcode_get_value(&gcode,'G');
                if ((!strcmp(gv,"0")||!strcmp(gv,"1")) && !c->backlashCompensationRelativeMode) {
                    if (m3d_gcode_has_value(&gcode,'F')) { strncpy(c->valueF, m3d_gcode_get_value(&gcode,'F'), sizeof(c->valueF)-1); c->valueF[sizeof(c->valueF)-1]=0; }
                    double deltaX = !m3d_gcode_has_value(&gcode,'X') ? 0 : gval(&gcode,'X') - c->backlashPositionRelativeX;
                    double deltaY = !m3d_gcode_has_value(&gcode,'Y') ? 0 : gval(&gcode,'Y') - c->backlashPositionRelativeY;
                    double deltaZ = !m3d_gcode_has_value(&gcode,'Z') ? 0 : gval(&gcode,'Z') - c->backlashPositionRelativeZ;
                    double deltaE = !m3d_gcode_has_value(&gcode,'E') ? 0 : gval(&gcode,'E') - c->backlashPositionRelativeE;
                    int dirX = deltaX > DBL_EPS ? DIR_POSITIVE : deltaX < -DBL_EPS ? DIR_NEGATIVE : c->previousDirectionX;
                    int dirY = deltaY > DBL_EPS ? DIR_POSITIVE : deltaY < -DBL_EPS ? DIR_NEGATIVE : c->previousDirectionY;
                    if ((dirX != c->previousDirectionX && c->previousDirectionX != DIR_NEITHER) || (dirY != c->previousDirectionY && c->previousDirectionY != DIR_NEITHER)) {
                        m3d_gcode_t ex; m3d_gcode_clear(&ex);
                        m3d_gcode_set_value(&ex,'G', m3d_gcode_get_value(&gcode,'G'));
                        if (dirX != c->previousDirectionX && c->previousDirectionX != DIR_NEITHER) c->compensationX += c->backlashX * (dirX == DIR_POSITIVE ? 1 : -1);
                        if (dirY != c->previousDirectionY && c->previousDirectionY != DIR_NEITHER) c->compensationY += c->backlashY * (dirY == DIR_POSITIVE ? 1 : -1);
                        set_val_d(&ex,'X', c->backlashPositionRelativeX + c->compensationX);
                        set_val_d(&ex,'Y', c->backlashPositionRelativeY + c->compensationY);
                        set_val_d(&ex,'F', c->backlashSpeed);
                        m3d_gcode_get_ascii(&ex, asc); nc_push(&nc, asc, ST_BACKLASH, ST_BACKLASH);
                        m3d_gcode_set_value(&gcode,'F', c->valueF);
                    }
                    if (m3d_gcode_has_value(&gcode,'X')) set_val_d(&gcode,'X', gval(&gcode,'X') + c->compensationX);
                    if (m3d_gcode_has_value(&gcode,'Y')) set_val_d(&gcode,'Y', gval(&gcode,'Y') + c->compensationY);
                    c->backlashPositionRelativeX += deltaX; c->backlashPositionRelativeY += deltaY;
                    c->backlashPositionRelativeZ += deltaZ; c->backlashPositionRelativeE += deltaE;
                    c->previousDirectionX = dirX; c->previousDirectionY = dirY;
                } else if (!strcmp(gv,"28")) { c->backlashPositionRelativeX = 54; c->backlashPositionRelativeY = 50; c->previousDirectionX = c->previousDirectionY = DIR_NEITHER; c->compensationX = c->compensationY = 0; }
                else if (!strcmp(gv,"90")) c->backlashCompensationRelativeMode = false;
                else if (!strcmp(gv,"91")) c->backlashCompensationRelativeMode = true;
                else if (!strcmp(gv,"92")) {
                    if (!m3d_gcode_has_value(&gcode,'X') && !m3d_gcode_has_value(&gcode,'Y') && !m3d_gcode_has_value(&gcode,'Z') && !m3d_gcode_has_value(&gcode,'E')) {
                        m3d_gcode_set_value(&gcode,'X',"0"); m3d_gcode_set_value(&gcode,'Y',"0"); m3d_gcode_set_value(&gcode,'Z',"0"); m3d_gcode_set_value(&gcode,'E',"0");
                    }
                    if (!is_fw_m3d(c)) {
                        if (m3d_gcode_has_value(&gcode,'X')) c->backlashPositionRelativeX = gval(&gcode,'X');
                        if (m3d_gcode_has_value(&gcode,'Y')) c->backlashPositionRelativeY = gval(&gcode,'Y');
                        if (m3d_gcode_has_value(&gcode,'Z')) c->backlashPositionRelativeZ = gval(&gcode,'Z');
                    }
                    if (m3d_gcode_has_value(&gcode,'E')) c->backlashPositionRelativeE = gval(&gcode,'E');
                }
            }
            if (nc.n) {
                if (!m3d_gcode_is_empty(&gcode)) { m3d_gcode_get_ascii(&gcode, asc); if (prepend(&stack, asc, origin, skip, &nc) < 0) { cs_free(&stack); return -1; } }
                else if (prepend(&stack, linebuf, origin, skip, &nc) < 0) { cs_free(&stack); return -1; }
                continue;
            }
        }

        // ---- skew compensation (M3D/M3D Mod firmware) ----
        if (is_fw_m3d(c) && (c->printingTestBorder || c->printingBacklashCalibration || c->useSkewCompensationPreprocessor) && skip < ST_SKEW) {
            skip = ST_SKEW;
            if (!m3d_gcode_is_empty(&gcode) && m3d_gcode_has_value(&gcode,'G')) {
                const char *gv = m3d_gcode_get_value(&gcode,'G');
                if ((!strcmp(gv,"0")||!strcmp(gv,"1")) && !c->skewCompensationRelativeMode) {
                    double deltaZ = !m3d_gcode_has_value(&gcode,'Z') ? 0 : gval(&gcode,'Z') - c->skewCompensationPositionRelativeZ;
                    c->skewCompensationPositionAbsoluteZ += deltaZ; c->skewCompensationPositionRelativeZ += deltaZ;
                    if (m3d_gcode_has_value(&gcode,'X')) set_val_d(&gcode,'X', gval(&gcode,'X') + c->skewCompensationPositionAbsoluteZ / (bb.bedHighMaxZ - BED_LOW_MIN_Z) * -c->skewX);
                    if (m3d_gcode_has_value(&gcode,'Y')) set_val_d(&gcode,'Y', gval(&gcode,'Y') + c->skewCompensationPositionAbsoluteZ / (bb.bedHighMaxZ - BED_LOW_MIN_Z) * -c->skewY);
                } else if (!strcmp(gv,"90")) c->skewCompensationRelativeMode = false;
                else if (!strcmp(gv,"91")) c->skewCompensationRelativeMode = true;
                else if (!strcmp(gv,"92")) {
                    if (!m3d_gcode_has_value(&gcode,'X') && !m3d_gcode_has_value(&gcode,'Y') && !m3d_gcode_has_value(&gcode,'Z') && !m3d_gcode_has_value(&gcode,'E')) {
                        m3d_gcode_set_value(&gcode,'X',"0"); m3d_gcode_set_value(&gcode,'Y',"0"); m3d_gcode_set_value(&gcode,'Z',"0"); m3d_gcode_set_value(&gcode,'E',"0");
                    }
                    if (!is_fw_m3d(c)) { if (m3d_gcode_has_value(&gcode,'Z')) c->skewCompensationPositionRelativeZ = gval(&gcode,'Z'); }
                }
            }
        }

        // ---- feed-rate persistence + emit ----
        if (!m3d_gcode_is_empty(&gcode)) {
            if (m3d_gcode_has_value(&gcode,'G') && (!strcmp(m3d_gcode_get_value(&gcode,'G'),"0") || !strcmp(m3d_gcode_get_value(&gcode,'G'),"1"))) {
                if (m3d_gcode_has_value(&gcode,'F')) { strncpy(c->currentF, m3d_gcode_get_value(&gcode,'F'), sizeof(c->currentF)-1); c->currentF[sizeof(c->currentF)-1]=0; }
                else if (c->currentF[0]) m3d_gcode_set_value(&gcode,'F', c->currentF);
            }
            m3d_gcode_get_ascii(&gcode, asc);
            emit(user, asc);
        }
        c->onNewPrintedLayer = false;
    }
    cs_free(&stack);
    return 0;
}
