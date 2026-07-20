// aiclient.h - Shared MayteraOS AI (Kimi) client + ReAct tool loop.
//
// This is the single source of truth for the AI integration used by BOTH the
// aichat GUI widget and the terminal/msh "?" prefix, so the tool set and the
// permission gate never drift between them.
//
// The module talks to the Kimi (Moonshot) chat-completions API over the
// kernel's synchronous HTTPS POST syscall, runs the ACTION/OBSERVATION ReAct
// loop against the tools listed in /AITOOLS/INDEX.yaml, and enforces the interim
// human-in-the-loop permission gate for high-risk writes (settings.set, ...).
//
// Two ways to use it:
//   1. One-shot (terminal "?", headless): aiclient_init(); aiclient_ask(...).
//   2. Multi-turn GUI (aichat): aiclient_init() once, then per turn
//      aiclient_add(0, user_text); aiclient_run_turn(out, cap, verbose);
//      and render the transcript via aiclient_count()/aiclient_get().
#ifndef AICLIENT_H
#define AICLIENT_H

#define AICLIENT_MAX_MSGS   64

// One conversation turn. role: 0=user, 1=assistant, 2=system/error (display),
// 3=system prompt (sent, not shown), 4=internal ACTION, 5=internal OBSERVATION.
typedef struct {
    int   role;
    char *text;
} ai_msg_t;

// Initialize the client: allocate the request/response buffers and load the API
// key (/CONFIG/KIMI.KEY) and the tool index (/AITOOLS/INDEX.yaml). Safe to call
// more than once. Returns 1 if an API key is present, 0 otherwise.
int  aiclient_init(void);

// 1 if a usable API key was loaded, else 0.
int  aiclient_have_key(void);

// 1 if the tool index loaded at least one tool.
int  aiclient_have_tools(void);

// Reset the conversation to just the system prompt (tools + ACTION protocol).
void aiclient_reset(void);

// Append a message to the current conversation (role per ai_msg_t).
void aiclient_add(int role, const char *text);

// Conversation accessors (for the GUI transcript renderer).
int             aiclient_count(void);
const ai_msg_t *aiclient_get(int i);

// Run the ReAct tool loop against the CURRENT conversation. The final
// plain-language answer is written into out[]. If verbose != 0 each
// ACTION/OBSERVATION step is also printed via printf (headless harness).
// Returns 0 on success, <0 net error, >0 HTTP status; on failure out[] holds
// the error text.
int  aiclient_run_turn(char *out, int outcap, int verbose);

// Convenience one-shot: reset, add the prompt as a user turn, run the loop, and
// return the final answer in out[]. Returns the same code as aiclient_run_turn.
int  aiclient_ask(const char *prompt, char *out, int outcap, int verbose);

// Run ONE tool through the full #293 capability/consent gate + audit and write
// the OBSERVATION JSON into obs[]. Returns the aicap_authorize() outcome
// (0 == AICAP_ALLOW on success). Exposed for the #294 headless build self-test
// driver so it can drive build.compile_app/deploy_app deterministically through
// the same consent + audit path the AI uses.
int  aiclient_run_action(const char *id, const char *args, char *obs, int ocap);

#endif // AICLIENT_H
