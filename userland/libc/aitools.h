// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aitools.h - the model-facing OS tool surface, generated from aitools.def.
//
// ONE declaration (aitools.def), three consumers, so the shipped tool index,
// the system-prompt tool list and the native tool-calling schema cannot
// disagree. See the header comment in aitools.def for what went wrong when
// they could.
//
// This unit is deliberately dependency-free (strlen/memcpy and nothing else),
// because tools/aitools-index/aitools-index.c compiles the SAME .c file on the
// build host to emit /AITOOLS/INDEX.yaml. A host program and a freestanding
// Ring-3 app emitting byte-identical text is what makes "the file on the image
// describes the code that runs" a property rather than a promise.

#ifndef AITOOLS_H
#define AITOOLS_H

// Size of the built-in prose tool block. MEASURED at 5,238 bytes for the 29
// tools in aitools.def; the headroom is for the next few tools, and
// aitools-index.c --check fails the build long before a summary could silently
// fall off the end.
#define AITOOLS_PROSE_MAX 8192

// Size a caller should give aitools_emit_tools_json(). MEASURED at 6,486 bytes
// for the same 29 tools.
#define AITOOLS_JSON_MAX  12288

// How many tools the surface advertises.
int aitools_count(void);

// The dotted id of tool i ("files.list"), or 0 if i is out of range.
const char *aitools_id(int i);

// The one-line summary of tool i, as it reaches the model.
const char *aitools_summary(int i);

// Tool i's declared arguments, in declaration order.
int         aitools_nargs(int i);
const char *aitools_arg_name(int i, int a);
int         aitools_arg_is_int(int i, int a);
int         aitools_arg_required(int i, int a);

// The OpenAI-style function name of tool i: the dotted id with '.' replaced by
// '_', because the function-calling wire format forbids a dot in a name.
// Writes into out[] and returns out.
const char *aitools_fn_name(int i, char *out, int ocap);

// Reverse of aitools_fn_name(): the table index whose function name equals
// `fn`, or -1. A REVERSE MAP AND NOT A STRING SUBSTITUTION, because
// underscores-to-dots is not invertible: build_compile_app would come back as
// build.compile.app and dispatch_tool() would reject it as an unknown tool.
int aitools_index_of_fn(const char *fn);

// The prose "  <id>: <summary>\n" block the system prompt carries. This is the
// same text /AITOOLS/INDEX.yaml distils to, built in from the same table so a
// missing or truncated index file cannot silently shrink the advertised
// surface (the shipped one advertised 18 of 29, MEASURED, #469m section 3).
const char *aitools_prose_list(void);

// A COMPACT id-only roster: the 29 dotted ids, comma separated, on one line.
//
// It exists for the native tool-calling prompt. The function schemas carry
// every id already, so a roster is redundant in principle; MEASURED, it is not
// redundant in practice, because a model reading 29 separate function
// definitions does not see them as a closed set the way it sees a list. The
// full prose block buys accuracy and costs 1,129 prompt tokens; this buys most
// of it for about a third of that. Which one ships is decided by the numbers
// in the #469m CHANGELOG entry, not by this comment.
const char *aitools_id_list(void);

// Emit the native tool-calling `tools` array VALUE (the bracketed JSON array,
// no key) into out[]. Returns the number of bytes written, or 0 if it did not
// fit, in which case out[] is left empty rather than truncated: a half-written
// schema array is invalid JSON and the endpoint would reject the whole POST.
int aitools_emit_tools_json(char *out, int ocap);

#endif // AITOOLS_H
