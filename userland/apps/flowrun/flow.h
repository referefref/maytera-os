// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// flow.h - Maytera Flow workflow runner, Milestone 1 (headless executor).
//
// The in-memory node+edge graph model, the typed-edge value carried between
// ports, and the platform seam. Everything in flow.c is platform neutral so a
// host gcc unit test (hosttest.c) can drive the SAME parser + marshaller +
// executor that the MayteraOS binary (main.c) runs. The only two operations
// that must differ per platform are file writing (which is capability gated on
// MayteraOS) and the LLM call, and those are the flow_plat_* seam below.
//
// See docs/WORKFLOW_YAML_SCHEMA.md for the authored YAML schema and the M1
// support matrix (what runs for real, what is data-model-only, what errors).
#ifndef MAYTERA_FLOW_H
#define MAYTERA_FLOW_H

#define FLOW_MAX_NODES     64
#define FLOW_MAX_EDGES     128
#define FLOW_MAX_PARAMS    16
#define FLOW_MAX_OUTPUTS   8

// M2 foreach/loop executor: a hard cap on how many items one loop iterates, so
// a runaway or malformed list cannot spin the runner. A loop's own max_iters
// (parsed into the model) lowers this further; it never raises it.
#define FLOW_LOOP_MAX_ITERS 1024

// #469 AI-VISION: a HARD ceiling on model calls that carry an image, for the
// whole run, whatever the graph says. A vision call costs money and wall clock
// and, unlike a file write, a runaway one is not obvious from the outside. An
// `llm` node's own `max_calls` param lowers this further; it never raises it.
#define FLOW_MAX_VISION_CALLS 64

#define FLOW_ID_MAX        48
#define FLOW_TYPE_MAX      24
#define FLOW_PORT_MAX      32
#define FLOW_KEY_MAX       32
#define FLOW_VAL_MAX       512
#define FLOW_ERR_MAX       256

// ---- typed values carried along an edge (the marshaller) -----------------
// M1 stores every value as its text form plus an optional integer view. The
// declared type is carried so a future milestone can enforce port typing; the
// coercion rule for M1 is "any value coerces to text", which is what every
// current consumer (transform/file/llm) needs.
typedef enum {
    FV_NONE = 0,
    FV_STRING,
    FV_INT,
    FV_BOOL,
    FV_ENUM,
    FV_LIST,
    FV_OBJECT
} fv_type_t;

typedef struct {
    fv_type_t type;
    char      s[FLOW_VAL_MAX];   // text form (authoritative for M1)
    long      i;                 // integer view for FV_INT / FV_BOOL
} flow_value_t;

// ---- a node ---------------------------------------------------------------
typedef struct {
    char key[FLOW_KEY_MAX];
    char val[FLOW_VAL_MAX];
} flow_param_t;

typedef struct {
    char port[FLOW_PORT_MAX];
    flow_value_t val;
} flow_output_t;

typedef struct {
    char id[FLOW_ID_MAX];
    char type[FLOW_TYPE_MAX];

    flow_param_t params[FLOW_MAX_PARAMS];
    int          nparams;

    // Promise / budget + async-subloop fields. PARSED into the model in M1 so
    // the schema is stable. M2 wired max_iters into the foreach/loop executor;
    // M3 wired the REST into the agent-promise loop (objective/success/max_iters/
    // max_minutes/max_tokens) and the async sub-loop (mode/every_ms). Do not read
    // has_loop as "this ran a loop".
    int  has_loop;
    char objective[FLOW_VAL_MAX];
    char success[FLOW_VAL_MAX];
    int  max_iters;
    int  max_minutes;
    long max_tokens;
    char mode[FLOW_TYPE_MAX];
    int  every_ms;

    // runtime
    flow_output_t outputs[FLOW_MAX_OUTPUTS];
    int           noutputs;
    int           executed;
    // #469: how many times THIS node has called the model in this run.
    // Deliberately NOT reset by run_loop_body() (which resets outputs and
    // `executed` every iteration), because a per-call budget that resets each
    // time round the loop is not a budget. flow_execute() zeroes it once per
    // run. See the llm node's `max_calls` param.
    int           calls;
} flow_node_t;

// ---- an edge: (from node, from port) -> (to node, to port) ---------------
typedef struct {
    char from_node[FLOW_ID_MAX];
    char from_port[FLOW_PORT_MAX];
    char to_node[FLOW_ID_MAX];
    char to_port[FLOW_PORT_MAX];
} flow_edge_t;

// ---- the graph ------------------------------------------------------------
typedef struct {
    char        name[FLOW_ID_MAX];
    flow_node_t nodes[FLOW_MAX_NODES];
    int         nnodes;
    flow_edge_t edges[FLOW_MAX_EDGES];
    int         nedges;
} flow_graph_t;

// ---- execution context ----------------------------------------------------
typedef struct {
    flow_graph_t *g;
    long          total_written;   // sum of bytes written by file nodes
    char          err[FLOW_ERR_MAX];
} flow_ctx_t;

// ---- parse ----------------------------------------------------------------
// Parse YAML bytes into *g. Returns 0 on success, non-zero on error (err set).
int flow_parse_bytes(const char *buf, int len, flow_graph_t *g,
                     char *err, int errcap);
// Read `path` and parse it. Returns 0 on success, non-zero on error (err set).
int flow_parse_file(const char *path, flow_graph_t *g, char *err, int errcap);

// ---- execute --------------------------------------------------------------
// Execute the graph in topological order through the node-executor registry.
// Returns 0 on success; on failure returns non-zero and ctx->err holds the
// reason (including "unsupported node type: X" for any unimplemented type).
int flow_execute(flow_ctx_t *ctx);

// #469: one token -> the keycode the compositor delivers for that real key, or
// -1 for "this is not a key". Accepted: a NAME (UP/DOWN/ENTER/ESC/F1/RSHIFT...,
// case insensitive), a SINGLE printable character (which is itself), or 0xNN.
// Exposed so the host unit test can assert the vocabulary against the shared
// libc/keys.h rather than against a copy of it.
int flow_keycode_of(const char *tok);

// ---- value helpers (used by node handlers) --------------------------------
void flow_value_set_str(flow_value_t *v, const char *s);
void flow_value_as_str(const flow_value_t *v, char *out, int cap);

// Resolve the value arriving on (node->id, port) by following the inbound edge
// to its source node's output. Returns 1 and fills *out if an edge supplies it,
// else 0.
int flow_input(flow_graph_t *g, const flow_node_t *node, const char *port,
               flow_value_t *out);

// Set a named output port value on a node (creates or overwrites).
void flow_set_output(flow_node_t *n, const char *port, const flow_value_t *v);

// Find a node's param by key; returns the value string or NULL.
const char *flow_param(const flow_node_t *n, const char *key);

// ---- platform seam (implemented by main.c on MayteraOS, hosttest.c on host)
// Write `len` bytes of `text` to `path`. append != 0 appends, else truncates.
// On MayteraOS this routes through the aicap capability gate + audit. Returns
// the number of bytes written (>= 0) on success, or < 0 on error (err set).
int flow_plat_write_file(const char *path, const char *text, int len,
                         int append, char *err, int errcap);

// One-shot text completion. system may be NULL/empty. Writes the model reply
// into out[]. Returns 0 on success, < 0 on error (err set). On MayteraOS this
// calls the shared aiclient one-shot; the host test stubs it as unavailable.
int flow_plat_llm(const char *system, const char *prompt,
                  char *out, int outcap, char *err, int errcap);

// "app" node seam. Drives another app's tool contract. On MayteraOS this wraps
// contract_invoke() and applies the capability gate for the target action's
// declared risk (SAFE runs, GUARDED goes through aicap_authorize + aicap_audit
// under the launch-consent model, DENIED refuses) BEFORE it spawns; the target
// app's own contract_cli is the authoritative single-site gate and audits
// again. The host test stubs it as unavailable (< 0). `argc`/`argv` are the
// contract verb and its arguments (e.g. {"call","value","100"}); the child's
// reply line is written into out[]. Returns the child's CT_* code, or < 0 when
// the app could not be reached or the flowrun-side gate refused.
int flow_plat_app_invoke(const char *app, int argc, char **argv,
                         char *out, int ocap);

// Monotonic milliseconds since boot, for the agent-promise loop's wall-clock
// (max_minutes) budget. On MayteraOS this is uptime_ms(); the host test uses a
// POSIX monotonic clock. It is read, never waited on: the agent loop does bounded
// work and checks a deadline, it never sleeps or busy-waits.
unsigned long flow_plat_now_ms(void);


// ===========================================================================
// #469 AI-VISION platform seam: perceive (capture), decide (vision), act (input)
// ===========================================================================
// All three need the real OS (the compositor's screenshot queue, the capability
// gate, the network). They are seams for the same reason the others are: so the
// node vocabulary, the parameter validation, the key tokenising and the budget
// arithmetic are all in flow.c and are all exercised by the host test.

// PERCEIVE. Capture `target`, crop it, downscale it to fit max_w x max_h,
// JPEG-encode it at `quality` and write it to `path`.
//
//   target  "screen"                 the whole screen
//           "window:<title substr>"  the first visible window whose title matches
//           "app:<APPID>"            ... whose owning binary basename matches
//           "win:<n>"                ... whose owning app's window handle is n
//           (a bare string is treated as a title substring)
//   rx,ry,rw,rh  a crop IN THE TARGET'S OWN COORDINATES (0,0,0,0 = no crop):
//           for "screen" that is absolute screen pixels; for a window it is
//           relative to that window's top-left, so a YAML rect keeps working
//           when the window moves.
//
// Returns the number of JPEG bytes written (>= 0) and fills *out_w/*out_h with
// the encoded image's size, or < 0 with err set. It NEVER writes a truncated or
// placeholder image and calls it a capture.
int flow_plat_capture(const char *target, int rx, int ry, int rw, int rh,
                      int max_w, int max_h, int quality, const char *path,
                      int *out_w, int *out_h, char *err, int errcap);

// DECIDE. One-shot multimodal completion: the JPEG at `image_path` plus a
// prompt. Returns 0 on success, < 0 with err set. On MayteraOS this is the
// shared aiclient_ask_image(); the host test stubs it.
int flow_plat_llm_image(const char *system, const char *prompt,
                        const char *image_path,
                        char *out, int outcap, char *err, int errcap);

// ACT. Deliver ONE key to `target` as a press and then a RELEASE, waiting
// hold_ms between them and gap_ms afterwards. The release is not optional: an
// app that edge-detects input (an emulator, a menu) that is sent a press with
// no release has that button held for the rest of the session and can never be
// sent it again.
//
// The platform layer is responsible for obtaining the consented, time-bounded
// input.inject grant for the target window, and must fail (< 0, err set) rather
// than proceed if the human does not grant it. NOTHING in this seam may
// approve, bypass or simulate consent.
int flow_plat_input_key(const char *target, int keycode,
                        int hold_ms, int gap_ms, char *err, int errcap);

#endif // MAYTERA_FLOW_H
