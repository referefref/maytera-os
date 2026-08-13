// ha_format.c - Home Assistant entity-state display formatter (#723).
// See ha_format.h for the contract. Implementation notes:
//
//  - Keyed on (domain, device_class), never on entity_id. #723's own audit of
//    the live instance (2033 entities, measured 2026-08-07) found "sensor"
//    alone is 76% of all entities and device_class "(none)" within it is 1285
//    entities (63% of the whole instance) - the fallback paths at the bottom
//    of this dispatcher are NOT an edge case, they are the majority case.
//  - No floating point. This target is soft-float with SSE disabled (see
//    CLAUDE.md); HA's raw state is already decimal text ("21.400000000000002"
//    is exactly what a Python float repr looks like), so round_decimal()
//    rounds the DIGIT STRING with integer carry arithmetic instead of ever
//    round-tripping through a binary float. That also means a malformed
//    "numeric-looking" string like a version number ("1.2.38", two dots)
//    correctly fails to parse and falls through to the text humanizer instead
//    of being silently truncated to "1.2".
//  - unavailable/unknown are handled ONCE, before any domain logic, because
//    they occur in every domain (buttons/lights/sensors alike) and must never
//    reach a domain-specific table that expects "on"/"off"/a number.
#include "ha_format.h"
#include "../../libc/string.h"
#include "../../libc/ctype.h"

static void set_text(ha_display_t *out, const char *s) {
    int i = 0;
    for (; s[i] && i < (int)sizeof(out->text) - 1; i++) out->text[i] = s[i];
    out->text[i] = 0;
}

// Case-insensitive substring test (device_class strings are short, plain
// ASCII words like "temperature" - a full-string strcasecmp would also
// reject legitimate multi-word/vendor-suffixed classes like HA's own
// "battery_charging" if matched against "battery" wrongly, hence substring
// with strncasecmp, not strcasecmp).
static int has_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return 1;
    }
    return 0;
}
static int eq_ci(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

// Humanize a raw snake_case/kebab-case token into "Title Case Words".
static void humanize(const char *raw, char *out, int cap) {
    int oi = 0, start_of_word = 1;
    for (int i = 0; raw[i] && oi < cap - 1; i++) {
        char c = raw[i];
        if (c == '_' || c == '-' || c == ' ') {
            if (oi > 0 && out[oi - 1] != ' ') out[oi++] = ' ';
            start_of_word = 1;
            continue;
        }
        if (start_of_word) { out[oi++] = (char)toupper((unsigned char)c); start_of_word = 0; }
        else out[oi++] = (char)tolower((unsigned char)c);
    }
    while (oi > 0 && out[oi - 1] == ' ') oi--;
    out[oi] = 0;
    if (oi == 0) { out[0] = '?'; out[1] = 0; }
}

// Round the decimal digit string `s` to `prec` fractional digits (half-up),
// optionally stripping trailing fractional zeros. Returns 0 (and leaves out
// untouched) if `s` is not a plain "[-]digits[.digits]" number.
static int round_decimal(const char *s, int prec, int strip_zeros, char *out, int cap) {
    int i = 0, neg = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    if (!isdigit((unsigned char)s[i])) return 0;
    char ip[24]; int ipn = 0;
    while (isdigit((unsigned char)s[i]) && ipn < (int)sizeof(ip) - 2) ip[ipn++] = s[i++];
    char fp[24]; int fpn = 0;
    if (s[i] == '.') {
        i++;
        while (isdigit((unsigned char)s[i]) && fpn < (int)sizeof(fp) - 1) fp[fpn++] = s[i++];
    }
    if (s[i] != 0) return 0;   // trailing junk (e.g. a 2nd '.') -> not a plain number
    if (ipn == 0) return 0;

    int carry = 0;
    if (fpn > prec) { carry = (fp[prec] >= '5'); fpn = prec; }
    while (fpn < prec) fp[fpn++] = '0';
    fp[fpn] = 0;
    if (carry) {
        int k = fpn - 1, c2 = 1;
        while (k >= 0 && c2) {
            if (fp[k] == '9') { fp[k] = '0'; k--; }
            else { fp[k]++; c2 = 0; }
        }
        if (c2) {
            int j = ipn - 1;
            while (j >= 0 && c2) {
                if (ip[j] == '9') { ip[j] = '0'; j--; }
                else { ip[j]++; c2 = 0; }
            }
            if (c2 && ipn < (int)sizeof(ip) - 2) {
                for (int m = ipn; m > 0; m--) ip[m] = ip[m - 1];
                ip[0] = '1'; ipn++;
            }
        }
    }
    if (strip_zeros) while (fpn > 0 && fp[fpn - 1] == '0') fp[--fpn] = 0;

    int oi = 0;
    int all_zero = (fpn == 0);
    for (int j = 0; j < ipn && all_zero; j++) if (ip[j] != '0') all_zero = 0;
    if (neg && !all_zero && oi < cap - 1) out[oi++] = '-';
    for (int j = 0; j < ipn && oi < cap - 1; j++) out[oi++] = ip[j];
    if (fpn > 0 && oi < cap - 1) {
        out[oi++] = '.';
        for (int j = 0; j < fpn && oi < cap - 1; j++) out[oi++] = fp[j];
    }
    out[oi] = 0;
    return 1;
}

// Leading integer value of a decimal string (for threshold checks like "is
// this battery reading low"). 0 if not numeric-leading.
static long leading_int(const char *s) {
    int i = 0, neg = 0; if (s[0] == '-') { neg = 1; i = 1; }
    long v = 0; int any = 0;
    while (isdigit((unsigned char)s[i])) { v = v * 10 + (s[i] - '0'); i++; any = 1; }
    if (!any) return 0;
    return neg ? -v : v;
}

// Precision + trailing-zero policy per device_class. See ha_format.c header
// comment: temperature/monetary get a FIXED decimal count (a thermometer
// reading "22" vs "22.4" jumping precision frame to frame looks broken);
// everything else strips trailing zeros so integer-valued custom sensors
// (github star counts, etc.) never grow a fake ".0".
static int numeric_prec(const char *dclass, int *strip_zeros) {
    if (has_ci(dclass, "temperature")) { *strip_zeros = 0; return 1; }
    if (has_ci(dclass, "monetary"))    { *strip_zeros = 0; return 2; }
    if (has_ci(dclass, "battery") || has_ci(dclass, "humidity") ||
        has_ci(dclass, "pm25"))        { *strip_zeros = 0; return 0; }
    *strip_zeros = 1; return 2;
}

static icon_id_t numeric_icon(const char *dclass) {
    if (has_ci(dclass, "temperature"))                                   return ICON_HA_THERMOMETER;
    if (has_ci(dclass, "humidity") || has_ci(dclass, "pm25") ||
        has_ci(dclass, "moisture"))                                      return ICON_HA_DROPLET;
    if (has_ci(dclass, "illuminance"))                                   return ICON_HA_SUN;
    if (has_ci(dclass, "battery"))                                       return ICON_HA_BATTERY;
    if (has_ci(dclass, "power") || has_ci(dclass, "energy") ||
        has_ci(dclass, "current") || has_ci(dclass, "voltage") ||
        has_ci(dclass, "data_rate") || has_ci(dclass, "frequency"))      return ICON_HA_BOLT;
    // Documented gap (#723): the long tail of vendor/custom units (data
    // size, duration, monetary, distance, weight, and the huge
    // device_class-less "sensor" bucket) has no bespoke icon. Rather than
    // invent 15 more single-use glyphs for classes with a handful of
    // entities each, they fall back to the shared generic marker - the same
    // choice ICON_INFO_CIRCLE already serves elsewhere in this icon set.
    return ICON_INFO_CIRCLE;
}

// binary_sensor device_class -> (on_word, off_word, on_is_problem).
// on_is_problem: true means ON is the thing worth flagging (problem/smoke/
// gas/tamper/moisture/safety/cold/heat/battery-low); for the rest ON is just
// "active" (motion detected, door open, connected), not inherently bad.
// Wording matches Home Assistant's own frontend convention for these classes
// so the OS reads the same way HA's app does.
typedef struct { const char *dc, *on_word, *off_word; int on_is_problem; } bsmap_t;
static const bsmap_t BSMAP[] = {
    { "door",             "Open",      "Closed",       0 },
    { "garage_door",      "Open",      "Closed",       0 },
    { "window",           "Open",      "Closed",       0 },
    { "opening",          "Open",      "Closed",       0 },
    { "lock",              "Unlocked", "Locked",       0 },
    { "motion",            "Detected", "Clear",        0 },
    { "occupancy",         "Detected", "Clear",        0 },
    { "vibration",         "Detected", "Clear",        0 },
    { "sound",             "Detected", "Clear",        0 },
    { "moisture",          "Wet",      "Dry",          1 },
    { "smoke",              "Detected","Clear",        1 },
    { "gas",                "Detected","Clear",        1 },
    { "problem",             "Problem","OK",           1 },
    { "safety",              "Unsafe", "Safe",         1 },
    { "tamper",              "Tampered","Clear",       1 },
    { "cold",                "Cold",   "Normal",       1 },
    { "heat",                "Hot",    "Normal",       1 },
    { "connectivity",         "Connected","Disconnected",0 },
    { "battery_charging",     "Charging","Not Charging",0 },
    { "battery",              "Low",    "Normal",      1 },
    { "running",              "Running","Not Running", 0 },
    { "presence",             "Home",   "Away",        0 },
    { "plug",                 "Plugged In","Unplugged",0 },
    { "power",                "Plugged In","Unplugged",0 },
    { "light",                "Light",  "Dark",        0 },
    { "update",               "Update Available","Up To Date",0 },
};
#define BSMAP_N (int)(sizeof(BSMAP)/sizeof(BSMAP[0]))

static icon_id_t binary_icon(const char *dclass, int on) {
    if (has_ci(dclass,"door")||has_ci(dclass,"garage")||has_ci(dclass,"window")||has_ci(dclass,"opening"))
        return on ? ICON_HA_DOOR_OPEN : ICON_HA_DOOR_CLOSED;
    if (has_ci(dclass,"lock"))                                    return on ? ICON_HA_LOCK_UNLOCKED : ICON_HA_LOCK_LOCKED;
    if (has_ci(dclass,"motion")||has_ci(dclass,"occupancy")||has_ci(dclass,"vibration")||has_ci(dclass,"sound"))
        return ICON_HA_MOTION;
    if (has_ci(dclass,"moisture"))                                return ICON_HA_DROPLET;
    if (has_ci(dclass,"smoke")||has_ci(dclass,"gas")||has_ci(dclass,"problem")||
        has_ci(dclass,"safety")||has_ci(dclass,"tamper"))         return on ? ICON_HA_WARN : ICON_HA_CHECK;
    if (has_ci(dclass,"battery"))                                 return ICON_HA_BATTERY;
    if (has_ci(dclass,"connectivity"))                            return on ? ICON_HA_CHECK : ICON_HA_WARN;
    if (has_ci(dclass,"plug")||has_ci(dclass,"power"))            return ICON_HA_BOLT;
    if (has_ci(dclass,"light"))                                   return ICON_HA_SUN;
    return on ? ICON_HA_CHECK : ICON_INFO_CIRCLE;
}

void ha_format_state(const char *domain, const char *device_class,
                      const char *raw_state, const char *unit,
                      ha_display_t *out) {
    out->icon = ICON_INFO_CIRCLE;
    out->sem  = HA_SEM_INFO;
    if (!domain) domain = "";
    if (!device_class) device_class = "";
    if (!raw_state) raw_state = "";
    if (!unit) unit = "";

    // ---- universal: unavailable / unknown / empty, before anything else ----
    if (raw_state[0] == 0 || eq_ci(raw_state, "unknown")) {
        set_text(out, "Unknown");
        out->icon = ICON_INFO_CIRCLE;
        out->sem  = HA_SEM_NEUTRAL;
        return;
    }
    if (eq_ci(raw_state, "unavailable")) {
        set_text(out, "Unavailable");
        out->icon = ICON_HA_WARN;          // shape says "not a real reading"...
        out->sem  = HA_SEM_UNAVAILABLE;    // ...color stays calm (not an alarm)
        return;
    }

    // ---- domain: binary_sensor (on/off -> device_class-specific wording) ----
    if (eq_ci(domain, "binary_sensor")) {
        int on = eq_ci(raw_state, "on");
        int off = eq_ci(raw_state, "off");
        if (on || off) {
            for (int i = 0; i < BSMAP_N; i++) {
                if (has_ci(device_class, BSMAP[i].dc)) {
                    set_text(out, on ? BSMAP[i].on_word : BSMAP[i].off_word);
                    out->icon = binary_icon(device_class, on);
                    if (BSMAP[i].on_is_problem) out->sem = on ? HA_SEM_ALERT : HA_SEM_ACTIVE;
                    else                        out->sem = on ? HA_SEM_ACTIVE : HA_SEM_NEUTRAL;
                    return;
                }
            }
            // no device_class (or one we don't special-case): default HA wording.
            set_text(out, on ? "On" : "Off");
            out->icon = on ? ICON_HA_CHECK : ICON_INFO_CIRCLE;
            out->sem  = on ? HA_SEM_ACTIVE : HA_SEM_NEUTRAL;
            return;
        }
        // fall through to generic humanize for anything odd
    }

    // ---- domain: cover (garage/shade/blind/curtain/shutter/damper/...) ----
    if (eq_ci(domain, "cover") || eq_ci(domain, "valve")) {
        int water = has_ci(device_class, "water");
        if (eq_ci(raw_state, "open"))    { set_text(out, "Open");        out->icon = water?ICON_HA_DROPLET:ICON_HA_DOOR_OPEN;   out->sem = HA_SEM_ACTIVE;  return; }
        if (eq_ci(raw_state, "closed"))  { set_text(out, "Closed");      out->icon = water?ICON_HA_DROPLET:ICON_HA_DOOR_CLOSED; out->sem = HA_SEM_NEUTRAL; return; }
        if (eq_ci(raw_state, "opening")) { set_text(out, "Opening..."); out->icon = ICON_HA_DOOR_OPEN;   out->sem = HA_SEM_ACTIVE;  return; }
        if (eq_ci(raw_state, "closing")) { set_text(out, "Closing..."); out->icon = ICON_HA_DOOR_CLOSED; out->sem = HA_SEM_ACTIVE;  return; }
        // else: fall through (e.g. "stopped") to generic humanize below
    }

    // ---- domain: lock ----
    if (eq_ci(domain, "lock")) {
        if (eq_ci(raw_state, "locked"))    { set_text(out, "Locked");    out->icon = ICON_HA_LOCK_LOCKED;   out->sem = HA_SEM_NEUTRAL; return; }
        if (eq_ci(raw_state, "unlocked"))  { set_text(out, "Unlocked");  out->icon = ICON_HA_LOCK_UNLOCKED; out->sem = HA_SEM_ACTIVE;  return; }
        if (eq_ci(raw_state, "jammed"))    { set_text(out, "Jammed");    out->icon = ICON_HA_WARN;          out->sem = HA_SEM_ALERT;   return; }
        if (has_ci(raw_state, "locking")||has_ci(raw_state,"unlocking")) { humanize(raw_state, out->text, sizeof(out->text)); out->icon = ICON_HA_LOCK_UNLOCKED; out->sem = HA_SEM_ACTIVE; return; }
    }

    // ---- domain: alarm_control_panel ----
    if (eq_ci(domain, "alarm_control_panel")) {
        if (has_ci(raw_state, "armed"))    { humanize(raw_state, out->text, sizeof(out->text)); out->icon = ICON_HA_LOCK_LOCKED;   out->sem = HA_SEM_NEUTRAL; return; }
        if (eq_ci(raw_state, "disarmed"))  { set_text(out, "Disarmed"); out->icon = ICON_HA_LOCK_UNLOCKED; out->sem = HA_SEM_ACTIVE; return; }
        if (eq_ci(raw_state, "triggered")) { set_text(out, "Triggered"); out->icon = ICON_HA_WARN; out->sem = HA_SEM_ALERT; return; }
        if (has_ci(raw_state, "pending")||has_ci(raw_state,"arming"))
                                            { humanize(raw_state, out->text, sizeof(out->text)); out->icon = ICON_HA_WARN; out->sem = HA_SEM_WARN; return; }
    }

    // ---- domain: sun ----
    if (eq_ci(domain, "sun")) {
        if (eq_ci(raw_state, "above_horizon")) { set_text(out, "Above Horizon"); out->icon = ICON_HA_SUN;  out->sem = HA_SEM_INFO; return; }
        if (eq_ci(raw_state, "below_horizon")) { set_text(out, "Below Horizon"); out->icon = ICON_HA_MOON; out->sem = HA_SEM_INFO; return; }
    }

    // ---- domain: weather (HA condition strings are hyphenated, some with
    // no separator at all - "partlycloudy" - so a small explicit table
    // reads far better than the generic humanizer) ----
    if (eq_ci(domain, "weather")) {
        struct { const char *cond, *text; icon_id_t icon; } W[] = {
            { "clear-night",     "Clear (Night)",  ICON_HA_MOON },
            { "cloudy",          "Cloudy",         ICON_INFO_CIRCLE },
            { "fog",             "Foggy",          ICON_INFO_CIRCLE },
            { "hail",            "Hail",           ICON_HA_WARN },
            { "lightning-rainy", "Thunderstorms",  ICON_HA_WARN },
            { "lightning",       "Lightning",      ICON_HA_WARN },
            { "partlycloudy",    "Partly Cloudy",  ICON_HA_SUN },
            { "pouring",         "Pouring Rain",   ICON_HA_DROPLET },
            { "rainy",           "Rainy",          ICON_HA_DROPLET },
            { "snowy-rainy",     "Snow & Rain",     ICON_HA_DROPLET },
            { "snowy",           "Snowy",          ICON_INFO_CIRCLE },
            { "sunny",           "Sunny",          ICON_HA_SUN },
            { "windy-variant",   "Windy",          ICON_INFO_CIRCLE },
            { "windy",           "Windy",          ICON_INFO_CIRCLE },
            { "exceptional",     "Severe Weather", ICON_HA_WARN },
        };
        for (unsigned i = 0; i < sizeof(W)/sizeof(W[0]); i++) {
            if (eq_ci(raw_state, W[i].cond)) {
                set_text(out, W[i].text); out->icon = W[i].icon; out->sem = HA_SEM_INFO; return;
            }
        }
    }

    // ---- domain: person / device_tracker (home/not_home/zone name) ----
    if (eq_ci(domain, "person") || eq_ci(domain, "device_tracker")) {
        if (eq_ci(raw_state, "home"))     { set_text(out, "Home"); out->icon = ICON_HOME;     out->sem = HA_SEM_ACTIVE;  return; }
        if (eq_ci(raw_state, "not_home")) { set_text(out, "Away"); out->icon = ICON_INFO_CIRCLE; out->sem = HA_SEM_NEUTRAL; return; }
        // a named zone ("Work", "School", ...): already human, just Title Case.
        humanize(raw_state, out->text, sizeof(out->text));
        out->icon = ICON_INFO_CIRCLE; out->sem = HA_SEM_INFO; return;
    }

    // ---- domain: update (on = update available, off = up to date) ----
    if (eq_ci(domain, "update")) {
        if (eq_ci(raw_state, "on"))  { set_text(out, "Update Available"); out->icon = ICON_HA_WARN;  out->sem = HA_SEM_WARN;   return; }
        if (eq_ci(raw_state, "off")) { set_text(out, "Up To Date");       out->icon = ICON_HA_CHECK; out->sem = HA_SEM_ACTIVE; return; }
    }

    // ---- domain: light / switch / fan / humidifier (plain on/off) ----
    if (eq_ci(domain, "light") || eq_ci(domain, "switch") || eq_ci(domain, "fan") ||
        eq_ci(domain, "humidifier") || eq_ci(domain, "siren")) {
        if (eq_ci(raw_state, "on") || eq_ci(raw_state, "off")) {
            int on = eq_ci(raw_state, "on");
            set_text(out, on ? "On" : "Off");
            if (eq_ci(domain, "light"))            out->icon = ICON_HA_BULB;
            else if (eq_ci(domain, "humidifier"))  out->icon = ICON_HA_DROPLET;
            else                                   out->icon = ICON_HA_BOLT;   // switch/fan/siren
            out->sem = on ? HA_SEM_ACTIVE : HA_SEM_NEUTRAL;
            return;
        }
    }

    // ---- domain: media_player ----
    if (eq_ci(domain, "media_player")) {
        if (has_ci(raw_state,"playing")) { set_text(out,"Playing"); out->icon = ICON_MUSIC; out->sem = HA_SEM_ACTIVE;  return; }
        if (has_ci(raw_state,"paused"))  { set_text(out,"Paused");  out->icon = ICON_MUSIC; out->sem = HA_SEM_NEUTRAL; return; }
        if (has_ci(raw_state,"idle"))    { set_text(out,"Idle");    out->icon = ICON_MUSIC; out->sem = HA_SEM_NEUTRAL; return; }
        if (eq_ci(raw_state,"on")||eq_ci(raw_state,"off")) { set_text(out, eq_ci(raw_state,"on")?"On":"Off"); out->icon = ICON_MUSIC; out->sem = eq_ci(raw_state,"on")?HA_SEM_ACTIVE:HA_SEM_NEUTRAL; return; }
    }

    // ---- domain: climate (heat/cool/auto/off/heat_cool/fan_only/dry) ----
    if (eq_ci(domain, "climate")) {
        if (eq_ci(raw_state, "heat_cool")) { set_text(out, "Heat & Cool"); out->icon = ICON_HA_THERMOMETER; out->sem = HA_SEM_ACTIVE; return; }
        if (eq_ci(raw_state, "off"))       { set_text(out, "Off");        out->icon = ICON_HA_THERMOMETER; out->sem = HA_SEM_NEUTRAL; return; }
        humanize(raw_state, out->text, sizeof(out->text));
        out->icon = ICON_HA_THERMOMETER; out->sem = HA_SEM_ACTIVE; return;
    }

    // ---- domain: camera / image (visual devices) ----
    if (eq_ci(domain, "camera") || eq_ci(domain, "image")) {
        humanize(raw_state, out->text, sizeof(out->text));
        out->icon = ICON_IMAGE; out->sem = has_ci(raw_state,"idle") ? HA_SEM_NEUTRAL : HA_SEM_INFO;
        return;
    }

    // ---- sensor dc=timestamp (ISO 8601 -> "Aug 7, 22:34"; no relative-time
    // math since that needs a trusted wall clock reading, out of scope here) ----
    if (has_ci(device_class, "timestamp") && strlen(raw_state) >= 10 && raw_state[4]=='-' && raw_state[7]=='-') {
        static const char *MON[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        int mo = (raw_state[5]-'0')*10 + (raw_state[6]-'0');
        int dy = (raw_state[8]-'0')*10 + (raw_state[9]-'0');
        char buf[32]; int oi = 0;
        const char *mn = (mo>=1&&mo<=12) ? MON[mo-1] : "?";
        for (int i=0; mn[i]; i++) buf[oi++]=mn[i];
        buf[oi++]=' ';
        if (dy>=10) buf[oi++] = (char)('0'+dy/10);
        buf[oi++] = (char)('0'+dy%10);
        if (strlen(raw_state) >= 16 && raw_state[10]=='T') {
            buf[oi++]=','; buf[oi++]=' ';
            buf[oi++]=raw_state[11]; buf[oi++]=raw_state[12]; buf[oi++]=':'; buf[oi++]=raw_state[14]; buf[oi++]=raw_state[15];
        }
        buf[oi]=0;
        set_text(out, buf); out->icon = ICON_CLOCK; out->sem = HA_SEM_INFO; return;
    }

    // ---- generic numeric (sensor/number domains, or anything else that
    // happens to be a plain decimal) ----
    {
        int strip = 1, prec = numeric_prec(device_class, &strip);
        char num[24];
        if (round_decimal(raw_state, prec, strip, num, sizeof(num))) {
            char full[48]; int oi = 0;
            for (int i = 0; num[i] && oi < (int)sizeof(full)-1; i++) full[oi++] = num[i];
            if (unit[0]) {
                if (oi < (int)sizeof(full)-1) full[oi++] = ' ';
                for (int i = 0; unit[i] && oi < (int)sizeof(full)-1; i++) full[oi++] = unit[i];
            }
            full[oi] = 0;
            set_text(out, full);
            out->icon = numeric_icon(device_class);
            if (has_ci(device_class, "battery") && leading_int(raw_state) < 20 && leading_int(raw_state) >= 0)
                out->sem = HA_SEM_WARN;
            else
                out->sem = HA_SEM_INFO;
            return;
        }
    }

    // ---- last resort: humanize whatever text we got (enum states, custom
    // integrations, select/number option text, todo/calendar on-off, etc.) ----
    humanize(raw_state, out->text, sizeof(out->text));
    out->icon = ICON_INFO_CIRCLE;
    out->sem  = HA_SEM_INFO;
}
