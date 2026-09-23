// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aidev.c - MayteraOS per-device AI capability manifest (owner requirement #6, 2026-09-16).
// See aidev.h for the model and docs/AI_DEVICE_CAPABILITY_MANIFEST.md for the
// full design. Pure userland, beside aicap.c, enforced at the aiclient dispatch
// boundary. #305 will relocate this into the protected kernel core alongside the
// caps.h grant model; the API here is the single choke point so that move is
// mechanical.
//
// The manifest is the FIRST of two gates and the harder one: a FORBID verb is
// refused here, before any consent prompt, so no amount of user clicking can
// make the AI format a device whose manifest forbids format. A CONSENT verb
// defers to the existing aicap_authorize() gate (capability token + consent +
// audit): we do NOT invent a second consent path.
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"
#include "aicap.h"
#include "aidev.h"

#define AIDEVCAP_CFG "/CONFIG/AIDEVCAP.CFG"   // per-device override manifest

// ---------------------------------------------------------------------------
// Verb names. Index MUST match the AIDEV_VERB_* enum order in aidev.h.
// ---------------------------------------------------------------------------
static const char *g_verb_names[AIDEV_VERB__COUNT] = {
    "read", "write", "interrupt", "enable", "disable",
    "format", "partition", "eject", "config"
};

int aidev_verb_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < AIDEV_VERB__COUNT; i++)
        if (!strcmp(name, g_verb_names[i])) return i;
    return -1;
}

const char *aidev_verb_name(int verb) {
    if (verb < 0 || verb >= AIDEV_VERB__COUNT) return "?";
    return g_verb_names[verb];
}

// ---------------------------------------------------------------------------
// Built-in class-default manifests. Two classes carry a fully-reasoned default
// for the bounded first step; every other / unknown class fails closed (all
// verbs FORBID) until a default is authored for it, which is a one-line addition
// to this table.
//
// A = AIDEV_ALLOW, C = AIDEV_CONSENT, F = AIDEV_FORBID. The array order matches
// the verb enum: read, write, interrupt, enable, disable, format, partition,
// eject, config.
//
// DESIGN NOTE (security specialist, deliberate and documented): the owner sketch
// said block "read/write/eject allowed, format/partition require consent". This
// default is stricter on write and eject (CONSENT, not ALLOW), because a raw
// device write and an eject of a mounted volume are destructive and a user
// should see them. The manifest is DATA: any verb can be relaxed to ALLOW (or
// tightened to FORBID) per device via /CONFIG/AIDEVCAP.CFG without a rebuild.
// The core requirement, that format and partition can never happen without an
// explicit decision and can be forbidden outright, holds regardless.
// ---------------------------------------------------------------------------
typedef struct {
    const char   *cls;
    unsigned char pol[AIDEV_VERB__COUNT];
} aidev_class_t;

static const aidev_class_t g_class_defaults[] = {
    // read      write         interrupt      enable         disable        format         partition      eject          config
    { "block", { AIDEV_ALLOW,  AIDEV_CONSENT, AIDEV_FORBID,  AIDEV_CONSENT, AIDEV_CONSENT, AIDEV_CONSENT, AIDEV_CONSENT, AIDEV_CONSENT, AIDEV_CONSENT } },
    // input is READ-ONLY to the AI: even reading input is keystroke observation,
    // so it is CONSENT-gated, and every mutating verb is FORBID outright.
    { "input", { AIDEV_CONSENT, AIDEV_FORBID, AIDEV_FORBID, AIDEV_FORBID,  AIDEV_FORBID,  AIDEV_FORBID,  AIDEV_FORBID,  AIDEV_FORBID,  AIDEV_FORBID } },
};
#define NCLASSES ((int)(sizeof(g_class_defaults)/sizeof(g_class_defaults[0])))

static const aidev_class_t *find_class(const char *cls) {
    if (!cls) return 0;
    for (int i = 0; i < NCLASSES; i++)
        if (!strcmp(g_class_defaults[i].cls, cls)) return &g_class_defaults[i];
    return 0;
}

// ---------------------------------------------------------------------------
// Per-device override file /CONFIG/AIDEVCAP.CFG, one record per line, '|'-sep:
//   class|dev_id|read|write|interrupt|enable|disable|format|partition|eject|config
// each policy field: a=allow, c=consent, f=forbid, -=inherit class default.
// dev_id "*" or empty = class-wide override. Lines beginning with '#' are
// comments. The most specific match wins: an exact dev_id record before a
// class-wide "*" record before the built-in class default.
// ---------------------------------------------------------------------------
static int policy_char(char c) {
    switch (c) {
        case 'a': case 'A': return AIDEV_ALLOW;
        case 'c': case 'C': return AIDEV_CONSENT;
        case 'f': case 'F': return AIDEV_FORBID;
        default:            return AIDEV_INHERIT;   // '-' or anything else
    }
}

// Copy the next '|'-delimited field of *pp into out, advancing *pp past it.
static void nfield(const char **pp, char *out, int ocap) {
    const char *p = *pp; int o = 0;
    while (*p && *p != '|' && *p != '\n' && *p != '\r' && o < ocap - 1) out[o++] = *p++;
    out[o] = 0;
    if (*p == '|') p++;
    *pp = p;
}

// Look up an override policy for (cls, dev_id, verb). Returns AIDEV_ALLOW/
// CONSENT/FORBID, or AIDEV_INHERIT if no override applies (caller falls back to
// the class default). dev_id may be NULL/"" (matches only a class-wide record).
static int override_policy(const char *cls, const char *dev_id, int verb) {
    int fd = sys_open(AIDEVCAP_CFG, O_RDONLY);
    if (fd < 0) return AIDEV_INHERIT;
    static char buf[4096];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return AIDEV_INHERIT;
    buf[n] = 0;

    int best = AIDEV_INHERIT;   // from the class default record
    int best_specific = 0;      // 1 if the winning record named this exact dev_id
    const char *p = buf;
    while (*p) {
        char line[512]; int ll = 0;
        while (*p && *p != '\n' && ll < (int)sizeof(line) - 1) line[ll++] = *p++;
        line[ll] = 0;
        while (*p == '\n' || *p == '\r') p++;
        if (!line[0] || line[0] == '#') continue;

        const char *q = line;
        char rc[64], rid[80];
        nfield(&q, rc, sizeof(rc));
        nfield(&q, rid, sizeof(rid));
        if (strcmp(rc, cls)) continue;

        int specific;
        if (!rid[0] || !strcmp(rid, "*")) {
            specific = 0;   // class-wide record
        } else if (dev_id && dev_id[0] && !strcmp(rid, dev_id)) {
            specific = 1;   // exact device match
        } else {
            continue;       // a different device's record
        }
        // Skip to the field for this verb (verb-many fields follow class+dev_id).
        char vf[8]; vf[0] = 0;
        for (int i = 0; i <= verb; i++) nfield(&q, vf, sizeof(vf));
        int pc = policy_char(vf[0]);
        if (pc == AIDEV_INHERIT) continue;   // '-' means inherit here too
        // A specific record always beats a class-wide one.
        if (specific >= best_specific) { best = pc; best_specific = specific; }
    }
    return best;
}

int aidev_verb_policy(const char *dev_class, const char *dev_id, int verb) {
    if (verb < 0 || verb >= AIDEV_VERB__COUNT) return AIDEV_FORBID;

    // Per-device override first (most specific wins inside override_policy).
    int ov = override_policy(dev_class, dev_id, verb);
    if (ov != AIDEV_INHERIT) return ov;

    // Then the built-in class default. Unknown class fails closed.
    const aidev_class_t *c = find_class(dev_class);
    if (!c) return AIDEV_FORBID;
    return c->pol[verb];
}

// ---------------------------------------------------------------------------
// The checkpoint
// ---------------------------------------------------------------------------
int aidev_authorize(const char *dev_class, const char *dev_id, int verb,
                    char *reason_out, int reasoncap) {
    if (reason_out && reasoncap) reason_out[0] = 0;
    aicap_init();   // so the audit trail is available

    const char *vn = aidev_verb_name(verb);
    const char *cn = dev_class ? dev_class : "?";
    const char *dn = (dev_id && dev_id[0]) ? dev_id : "*";

    // A stable synthesized tool id / capability for audit + consent routing.
    char tool[80];
    snprintf(tool, sizeof(tool), "device.%s.%s", cn, vn);
    // Args carry the device identity so the audit line and any future consent
    // prompt can name exactly which device is targeted.
    char aargs[128];
    snprintf(aargs, sizeof(aargs), "{\"device\":\"%s:%s\",\"verb\":\"%s\"}", cn, dn, vn);

    int pol = aidev_verb_policy(dev_class, dev_id, verb);

    if (pol == AIDEV_FORBID) {
        // Refused by the manifest. Consent is NEVER consulted: the manifest is
        // the first and harder gate. Audited as a manifest denial.
        aicap_audit(tool, tool, aargs, "denied", "manifest-forbid");
        if (reason_out)
            snprintf(reason_out, reasoncap,
                     "device manifest forbids '%s' on class '%s'", vn, cn);
        return AIDEV_DECISION_DENY;
    }

    if (pol == AIDEV_ALLOW) {
        aicap_audit(tool, tool, aargs, "ok", "manifest-allow");
        return AIDEV_DECISION_ALLOW;
    }

    // AIDEV_CONSENT: defer to the existing capability/consent/audit gate. An
    // unknown tool id classifies HIGH in aicap, so this raises consent (or
    // consumes a matching token) exactly like every other HIGH-risk AI tool.
    char reason[200], how[80];
    int az = aicap_authorize(tool, aargs, reason, sizeof(reason), how, sizeof(how));
    if (az == AICAP_ALLOW) {
        aicap_audit(tool, tool, aargs, "ok", how[0] ? how : "consent");
        return AIDEV_DECISION_ALLOW;
    }
    aicap_audit(tool, tool, aargs, "denied", aicap_code(az));
    if (reason_out)
        snprintf(reason_out, reasoncap, "%s: %s", aicap_code(az),
                 reason[0] ? reason : "consent required and not granted");
    return AIDEV_DECISION_DENY;
}

// ---------------------------------------------------------------------------
// Tool-id adoption for the AI ReAct loop
// ---------------------------------------------------------------------------
// Grab a flat-JSON string field into out[] (mirrors aicap.c's jget).
static int devjget(const char *json, const char *key, char *out, int ocap) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = json ? strstr(json, pat) : 0;
    if (!k) { out[0] = 0; return 0; }
    const char *v = k + strlen(pat);
    while (*v && *v != ':') v++;
    if (*v != ':') { out[0] = 0; return 0; }
    v++;
    while (*v == ' ' || *v == '\t') v++;
    if (*v != '"') { out[0] = 0; return 0; }
    v++;
    int o = 0;
    while (*v && *v != '"' && o < ocap - 1) {
        if (*v == '\\' && v[1]) v++;
        out[o++] = *v++;
    }
    out[o] = 0;
    return 1;
}

int aidev_authorize_tool(const char *tool_id, const char *args, char *obs, int ocap) {
    // Parse "device.<class>.<verb>".
    const char *p = tool_id;
    if (strncmp(p, "device.", 7)) {
        snprintf(obs, ocap, "{\"error\":\"bad-device-tool\",\"id\":\"%s\"}", tool_id);
        return AICAP_DENIED;
    }
    p += 7;
    char cls[64]; int o = 0;
    while (*p && *p != '.' && o < (int)sizeof(cls) - 1) cls[o++] = *p++;
    cls[o] = 0;
    if (*p != '.' || !cls[0]) {
        snprintf(obs, ocap, "{\"error\":\"bad-device-tool\",\"id\":\"%s\"}", tool_id);
        return AICAP_DENIED;
    }
    p++;
    int verb = aidev_verb_from_name(p);
    if (verb < 0) {
        snprintf(obs, ocap, "{\"error\":\"unknown-device-verb\",\"id\":\"%s\"}", tool_id);
        return AICAP_DENIED;
    }

    char dev_id[80];
    if (!devjget(args, "device", dev_id, sizeof(dev_id)) &&
        !devjget(args, "dev",    dev_id, sizeof(dev_id)) &&
        !devjget(args, "id",     dev_id, sizeof(dev_id)))
        dev_id[0] = 0;

    char reason[224];
    int d = aidev_authorize(cls, dev_id, verb, reason, sizeof(reason));
    if (d == AIDEV_DECISION_ALLOW) {
        // The manifest (and, for a CONSENT verb, the user) permit the verb.
        // #708: the EJECT verb now has a REAL executor (aiclient.c
        // exec_device_eject -> the kernel safe-eject SYS_VOL_EJECT), which runs
        // immediately AFTER this gate and REPLACES this observation with the real
        // result; this placeholder is only ever seen if that executor is not
        // reached. Every OTHER device verb (format, partition, ...) still has NO
        // executor, so for those we report the grant honestly rather than pretend
        // an action ran.
        if (verb == AIDEV_VERB_EJECT)
            snprintf(obs, ocap,
                     "{\"ok\":true,\"device\":\"%s\",\"verb\":\"eject\","
                     "\"note\":\"manifest permits; safe-eject executor runs next\"}",
                     cls);
        else
            snprintf(obs, ocap,
                     "{\"ok\":true,\"device\":\"%s\",\"verb\":\"%s\","
                     "\"note\":\"manifest permits; no executor for this verb yet\"}",
                     cls, aidev_verb_name(verb));
        return AICAP_ALLOW;
    }
    snprintf(obs, ocap,
             "{\"error\":\"device-denied\",\"device\":\"%s\",\"verb\":\"%s\",\"reason\":\"%s\"}",
             cls, aidev_verb_name(verb), reason);
    return AICAP_DENIED;
}
