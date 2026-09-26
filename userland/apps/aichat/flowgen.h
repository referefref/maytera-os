// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// flowgen.h - Maytera Flow: chat-driven workflow GENERATION (flowgen).
//
// This module turns an AI Chat conversation (or a typed description) into a
// Maytera Flow workflow YAML, and AI-optimises / AI-modifies / AI-forks an
// existing one. It is the AUTHORING half; the runner (/APPS/FLOWRUN) and the
// visual editor (/APPS/FLOW) are unchanged.
//
// EVERYTHING IN HERE IS PLATFORM NEUTRAL so a host gcc unit test
// (flowgen_hosttest.c) can drive the SAME prompt-building, LLM-reply extraction,
// and VALIDATION logic the MayteraOS aichat binary runs. The LLM call itself and
// the file save live in main.c (they need aiclient + the fs) and are NOT here.
//
// THE KEY ROBUSTNESS REQUIREMENT: an LLM's YAML is frequently malformed or
// wrapped in prose/markdown fences. Nothing this module produces is ever saved
// until flowgen_validate() has PARSED it with the runner's OWN parser
// (flow_parse_bytes from userland/apps/flowrun/flow.c) and confirmed it has at
// least one node and that every edge references a node that exists. Same parser
// the executor uses, so "validated here" means "the runner will parse it".
#ifndef AICHAT_FLOWGEN_H
#define AICHAT_FLOWGEN_H

#include "flow.h"   // flow_graph_t, flow_parse_bytes, FLOW_ERR_MAX

// Candidate-YAML working buffer size. A workflow tops out at FLOW_MAX_NODES(64)
// nodes + FLOW_MAX_EDGES(128) edges; 16 KiB comfortably holds the largest legal
// one plus the LLM's formatting slack. All callers use static storage of this
// size (freestanding apps have a small stack).
#define FLOWGEN_YAML_MAX   16384
// Prompt working buffer: the embedded schema (~3 KiB) + task + payload/context.
#define FLOWGEN_PROMPT_MAX 32768

// The four authoring operations. GENERATE builds a new workflow from a
// conversation/description; the other three rewrite an existing one.
typedef enum {
    FLOWGEN_GENERATE = 0,
    FLOWGEN_OPTIMISE,
    FLOWGEN_MODIFY,
    FLOWGEN_FORK
} flowgen_mode_t;

// The concise Maytera Flow schema embedded in every generation prompt: the node
// types, their params, and the edge format. A single source of truth so the
// four prompt builders stay consistent. Returns a static string.
const char *flowgen_schema_text(void);

// Build the full LLM prompt for `mode`.
//   GENERATE: `payload` is the user's description (may be empty), `context` is
//             the prior chat transcript (may be empty/NULL). One of the two
//             should be non-empty.
//   OPTIMISE/MODIFY/FORK: `payload` is the EXISTING workflow's YAML; `context`
//             carries the user's change request (MODIFY) or fork variation note
//             (FORK), and is ignored for OPTIMISE.
// Writes into out[] (bounded); returns the byte length written (>= 0).
int flowgen_build_prompt(flowgen_mode_t mode, const char *payload,
                         const char *context, char *out, int cap);

// When a first attempt fails validation, this builds a SHORTER re-prompt that
// hands the model back its own broken YAML plus the exact parser error and asks
// for a corrected, YAML-only reply. Returns bytes written.
int flowgen_build_retry_prompt(const char *broken_yaml, const char *parse_err,
                               char *out, int cap);

// Extract the YAML body from a raw LLM reply. Strips a ``` / ```yaml fenced
// block if present; otherwise trims to the first line that starts a YAML mapping
// (`name:` or `nodes:`). Copies into out[] (bounded, NUL-terminated). Returns
// the byte length written (>= 0).
int flowgen_extract_yaml(const char *reply, char *out, int cap);

// VALIDATE a candidate workflow YAML against the flow schema using the runner's
// own parser. Returns 0 if valid; non-zero (err set) if REJECTED. Rejects:
//   - malformed YAML / not a top-level mapping (parser error),
//   - a workflow with zero nodes,
//   - an edge whose `from` or `to` node id is not defined by any node.
// On success *g (if non-NULL) holds the parsed graph. This is the ONLY gate a
// generated file passes through before being saved.
int flowgen_validate(const char *yaml, int len, flow_graph_t *g,
                     char *err, int cap);

// Sanitise a proposed workflow name to a lower-case, filesystem-safe token
// ([a-z0-9-], others dropped, runs of '-' collapsed, <= cap-1 chars). Empty or
// all-punctuation input yields "workflow". Matches the /APPS/FLOW editor's
// lower-case long-name convention for /CONFIG/WORKFLOWS/<name>.yml.
void flowgen_sanitize_name(const char *in, char *out, int cap);

// Pull the workflow's declared `name:` scalar out of a YAML buffer into out[]
// (sanitised). Returns 1 if a name was found, 0 otherwise (out set to "").
int flowgen_name_from_yaml(const char *yaml, char *out, int cap);

// Ensure the YAML carries a top-level `description:` line (the editor's Flow
// Library shows it). If one is already present, the buffer is returned
// unchanged and 0 is returned. If absent, a `description: <desc>` line is
// inserted right after the opening (or prepended) and 1 is returned. Operates
// in place on yaml[] (cap is its capacity); safe if it would overflow (no-op,
// returns -1).
int flowgen_ensure_description(char *yaml, int cap, const char *desc);

#endif // AICHAT_FLOWGEN_H
