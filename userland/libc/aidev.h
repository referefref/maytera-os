// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aidev.h - MayteraOS per-device AI capability manifest (owner requirement #6, 2026-09-16).
//
// This EXTENDS the AI capability/consent/audit layer (aicap.h, #293) DOWN TO THE
// DEVICE TIER. The owner requirement: every device must have a manifest declaring
// what actions, if any, the AI may perform with it (read, write, interrupt,
// enable, disable, format, partition, ...). This file is the schema plus the
// enforcement checkpoint an AI-initiated device action MUST pass.
//
// TWO GATES, IN ORDER, AND THE ORDER IS THE POINT:
//
//   1. THE DEVICE MANIFEST (this file). For a (device class, device id, verb) it
//      answers FORBID / CONSENT / ALLOW from a per-class default table plus an
//      optional per-device override file. FORBID is refused HERE, before any
//      consent prompt: no amount of user clicking can make the AI format a device
//      whose manifest forbids format. This is the manifest's whole reason to
//      exist, and it is a strictly HARDER gate than consent.
//
//   2. THE EXISTING CAPABILITY/CONSENT/AUDIT GATE (aicap.h). For a CONSENT verb
//      the checkpoint DEFERS to aicap_authorize(), so a device action reuses the
//      same capability token, the same consent decision, and the same audit
//      trail as every other AI tool. We do NOT invent a second consent path.
//
// HONEST SCOPING (read this before believing any coverage claim):
//   - Enforcement lives in USERLAND, beside aicap.c, exactly as #293's does. It
//     is advisory against a Ring-3 process running as uid 0, same ceiling aicap
//     already documents. #305 will relocate this into the protected kernel core;
//     the API here is the single choke point so that move is mechanical.
//   - #708: the EJECT verb IS now wired to a real executor (aiclient.c
//     exec_device_eject -> the kernel safe-eject SYS_VOL_EJECT: flush + unmount +
//     STOP UNIT). The other raw-device verbs (format, partition, ...) still have
//     NO executor. This checkpoint is the MANDATORY gate any future device tool
//     must call, verified by a host unit test and, for eject, end to end on a VM
//     with a real removable device. Do not read the schema's breadth as enforced
//     breadth. See docs/AI_DEVICE_CAPABILITY_MANIFEST.md.
#ifndef AIDEV_H
#define AIDEV_H

// The verb set. Every AI-initiated device action names exactly one of these.
enum {
    AIDEV_VERB_READ = 0,   // read data or state from the device
    AIDEV_VERB_WRITE,      // write data to the device
    AIDEV_VERB_INTERRUPT,  // mask, unmask, reroute or reset the device IRQ
    AIDEV_VERB_ENABLE,     // bring the device online / power on
    AIDEV_VERB_DISABLE,    // take the device offline / power off
    AIDEV_VERB_FORMAT,     // create a new filesystem / erase (destructive)
    AIDEV_VERB_PARTITION,  // rewrite the partition table (destructive)
    AIDEV_VERB_EJECT,      // safe-remove removable media
    AIDEV_VERB_CONFIG,     // change device configuration (baud, IP, mode, ...)
    AIDEV_VERB__COUNT
};

// Policy levels a manifest assigns to a (class, verb). ALLOW and CONSENT are the
// two "permitted" outcomes; FORBID is the refusal. INHERIT appears only in an
// override record: it means "use the class default for this verb".
enum {
    AIDEV_FORBID  = 0,   // never; refused at the manifest before consent
    AIDEV_CONSENT = 1,   // only with a capability token + explicit user consent
    AIDEV_ALLOW   = 2,   // permitted, audited, no consent needed
    AIDEV_INHERIT = 3    // override records only: fall through to the class default
};

// Outcomes of the enforcement checkpoint.
enum {
    AIDEV_DECISION_ALLOW = 0,   // the action may run (already audited)
    AIDEV_DECISION_DENY  = 1    // refused (already audited); reason_out explains
};

// Map a verb name ("format") to its AIDEV_VERB_* value, or -1 if unknown. Used
// by the override-file parser and by callers that carry a verb as text.
int aidev_verb_from_name(const char *name);

// The canonical name of a verb ("format"), or "?" if out of range.
const char *aidev_verb_name(int verb);

// Resolve the manifest policy (AIDEV_FORBID / AIDEV_CONSENT / AIDEV_ALLOW) for a
// (device class, device id, verb). Consults the per-device override file
// (/CONFIG/AIDEVCAP.CFG) first, then the built-in class-default table. An unknown
// class fails closed (every verb FORBID). dev_id may be NULL or "" (matches only
// a class-wide "*" override, else the class default). This function performs NO
// consent and NO audit: it is the pure policy lookup, safe to call from a UI that
// wants to show what the AI is permitted to do.
int aidev_verb_policy(const char *dev_class, const char *dev_id, int verb);

// THE CHECKPOINT. Every AI-initiated device action MUST pass this before it runs.
//
//   AIDEV_FORBID  -> AIDEV_DECISION_DENY, audited "denied/manifest-forbid".
//                    The consent gate is NEVER consulted (the manifest is first).
//   AIDEV_ALLOW   -> AIDEV_DECISION_ALLOW, audited "ok/manifest-allow".
//   AIDEV_CONSENT -> defer to aicap_authorize() with tool id "device.<class>.<verb>"
//                    (a capability token + consent decision), then audit the
//                    outcome via aicap_audit(). ALLOW on grant, DENY on refusal.
//
// On a denial, reason_out (optional) receives a short English explanation.
// Returns AIDEV_DECISION_ALLOW (0) or AIDEV_DECISION_DENY (1).
int aidev_authorize(const char *dev_class, const char *dev_id, int verb,
                    char *reason_out, int reasoncap);

// The adoption point for the AI ReAct loop (aiclient.c). An AI ACTION whose tool
// id has the form "device.<class>.<verb>" (e.g. "device.block.format") is routed
// here. It parses the class and verb from the id, the device id from an optional
// args JSON field ("device", "dev" or "id"), runs aidev_authorize(), writes the
// OBSERVATION JSON into obs[], and returns an aicap outcome code (AICAP_ALLOW on
// grant, AICAP_DENIED on refusal) so it slots into aiclient_run_action()'s return
// contract. This is the live gate: an LLM can name a device verb, and the
// manifest decides it BEFORE any executor could run.
int aidev_authorize_tool(const char *tool_id, const char *args, char *obs, int ocap);

#endif // AIDEV_H
