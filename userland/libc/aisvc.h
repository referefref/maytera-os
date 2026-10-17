// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aisvc.h - THE reader for the per-user AI service config, <home>/CONFIG/AISVC.CFG.
//
// WHY THIS FILE EXISTS, AND THE BUG THAT PAID FOR IT
//
// #684 moved the AI key out of /CONFIG/KIMI.KEY, a raw file whose ENTIRE
// CONTENT was the key, into the per-user AISVC.CFG, a key=value file whose
// api_key= line carries it. libc/aiclient.c was updated to PARSE the new file.
// userland/apps/paint/ai.c was pointed at the same path and left reading it the
// old way: it slurped the whole file, trimmed the trailing newline, and sent the
// result as the bearer token. Since the kernel's login provisioner writes
// exactly one line, "api_key=<key>", Maytera Studio has been sending the eight
// characters "api_key=" in front of every token it has ever presented.
//
// MEASURED on golden 2509, 2026-09-30: a flow's llm node answered a prompt
// correctly through aiclient.c, and Studio's own assistant answered
// "AI request refused by the API (HTTP 401)" on the same machine, the same
// user, the same key, seconds apart. The app's own comment called the duplicate
// parser "debt"; the debt was not that there were two readers, it was that the
// second one did not read.
//
// So the fix is not to correct the copy. It is that there is ONE reader, here,
// and both clients call it. A file format cannot change under one consumer and
// not the other if there is only one consumer.
//
// WHY LIBC AND NOT aiclient.c: Studio deliberately does NOT link aiclient's
// ReAct tool loop (its planner must not reach OS tools; see paint/ai.c). A
// separate translation unit lets it share the CONFIG without inheriting the
// LOOP, which is the whole reason the duplicate was created in the first place.

#ifndef _AISVC_H
#define _AISVC_H

#define AISVC_STYLE_BEARER     0   // OpenAI-compatible: Authorization: Bearer, /chat/completions
#define AISVC_STYLE_ANTHROPIC  1   // Anthropic Messages API: x-api-key, /v1/messages

#define AISVC_TC_AUTO 0
#define AISVC_TC_ON   1
#define AISVC_TC_OFF  2

typedef struct {
    char endpoint[256];
    char model[96];
    char api_key[256];
    int  api_style;     // AISVC_STYLE_*
    int  tool_calls;    // AISVC_TC_*
    int  have_key;      // 1 once api_key= supplied a non-empty value
} aisvc_cfg_t;

// Overlay the caller's per-user AISVC.CFG onto *cfg.
//
// THE CALLER SEEDS THE DEFAULTS and this function only overwrites what the file
// actually says. That is deliberate: a partial config (the provisioner writes
// api_key= alone) must leave the client's compiled endpoint/model exactly as
// they were, which is what keeps an existing install byte-for-byte unchanged.
//
// Returns 1 if the file was opened and read, 0 if it is absent or empty. A 0
// return is NOT an error: it means "no per-user config", and *cfg still holds
// whatever the caller seeded.
//
// The key is copied into cfg->api_key and is never logged, printed or echoed by
// anything in this module.
int aisvc_load(aisvc_cfg_t *cfg);

#endif
