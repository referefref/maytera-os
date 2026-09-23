// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// contract.c - Maytera Studio's tool contract (#233 surface, tier-2 wire).
//   docs/AI_ACTION_CAPABILITY_BINDING.md sections 1.7, 3.3, 5.2.
//
// Studio already parses model output as strict JSON DATA and applies it through
// its own primitives (ai.c parse_plan/apply_step) - the safe act-half tier 2 is
// built on. This file EXPOSES a few of those primitives as contract actions the
// OS AI can invoke on the LIVE canvas: the same layer_add()/filter_apply()/
// undo_push() the menus call, run against the live g_doc.
//
// TWO GATES, by the design of CONTRACT_API.md section 11 and this doc's 4.4:
//   - `cap` (aicap id) + `risk` bound what the AI may do THROUGH the contract
//     API, checked in-process by contract_authorize()->aicap_authorize(). This
//     is the demonstrable red/green: a CT_GUARDED edit is refused with no grant
//     and allowed with one. It is exactly the tier-2-cooperative gate, because a
//     cooperative edit reaches NO gated syscall (its `needs` list is empty).
//   - `needs` is the KERNEL capability an action's own code reaches. screen_check
//     calls sys_screenshot_request(), which the kernel gates at syscall_cap_check
//     regardless of anything here: THAT is the real teeth (4.4). Its `needs`
//     entry declares the alignment so the tier-2 union (3.4) is machine-readable.
//
// A stateless SPAWN of Studio (contract_invoke fallback) has NO document, so the
// edit actions guard on g_doc.nlayers and answer "no-live-document" - which is
// precisely why the LIVE path (contract_live_poll) exists.

#include "studio.h"
#include "../../libc/contract.h"
#include "../../libc/syscall.h"
#include "../../libc/userconf.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"

// ---- read-only live state (serial-verifiable without a screendump) ---------
static int cg_width(void)    { return g_doc.w; }
static int cg_height(void)   { return g_doc.h; }
static int cg_layers(void)   { return g_doc.nlayers; }
static int cg_active(void)   { return g_doc.active; }
static int cg_modified(void) { return g_doc.modified ? 1 : 0; }

static int have_doc(char *out, int ocap) {
    if (g_doc.nlayers > 0) return 1;
    strlcpy(out, "no-live-document (spawned copy has no canvas; a running Studio is required)", (size_t)ocap);
    return 0;
}

// ---- actions on the LIVE document ------------------------------------------
static int ca_add_layer(const ct_item_t *it, int argc, char **argv,
                        char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!have_doc(out, ocap)) return CT_ERR_FAILED;
    int idx = layer_add("AI layer", 0);
    if (idx < 0) { strlcpy(out, "layer-add-failed (max layers?)", (size_t)ocap); return CT_ERR_FAILED; }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    ui_status("MayterAI: added a layer");
    ui_full_redraw();
    snprintf(out, ocap, "added-layer index=%d layers=%d", idx, g_doc.nlayers);
    return 0;
}

static int ca_invert(const ct_item_t *it, int argc, char **argv,
                     char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!have_doc(out, ocap)) return CT_ERR_FAILED;
    undo_push("AI invert");
    filter_apply(F_INVERT, 0, 0, 0);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    ui_status("MayterAI: inverted the active layer");
    ui_full_redraw();
    strlcpy(out, "inverted-active-layer", (size_t)ocap);
    return 0;
}

static int ca_grayscale(const ct_item_t *it, int argc, char **argv,
                        char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!have_doc(out, ocap)) return CT_ERR_FAILED;
    undo_push("AI grayscale");
    filter_apply(F_GRAYSCALE, 0, 0, 0);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    ui_status("MayterAI: grayscaled the active layer");
    ui_full_redraw();
    strlcpy(out, "grayscaled-active-layer", (size_t)ocap);
    return 0;
}

// PERCEIVE action: capture the live canvas. The KERNEL gates
// sys_screenshot_request via screen.capture (CAP_SCREEN_CAPTURE); without a
// grant it returns CAP_EDENIED, and no manifest field here can change that -
// this is where the kernel teeth (4.4) bite for a cooperative app.
static int ca_screen_check(const ct_item_t *it, int argc, char **argv,
                           char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    char path[160];
    if (userhome_path(0, "STUDIOAI.BMP", path, sizeof(path)) != 0)
        strlcpy(path, "/HOME/STUDIOAI.BMP", sizeof(path));
    long r = sys_screenshot_request(path);
    if (r == (long)CAP_EDENIED) {
        snprintf(out, ocap,
                 "capability-denied: the kernel refused screen.capture (no grant); path=%s", path);
        return CT_ERR_FAILED;
    }
    if (r != 0) { snprintf(out, ocap, "capture-failed rc=%ld path=%s", r, path); return CT_ERR_FAILED; }
    snprintf(out, ocap, "capture-queued path=%s", path);
    return 0;
}

// ---- the needs bindings (tier 2, 3.3/3.4) ----------------------------------
// Edit actions reach no gated syscall: their needs list is empty. screen_check
// reaches the kernel screen.capture gate on the current-document scope.
static const ct_cap_need_t k_needs_none[]    = { { 0, 0, 0 } };
static const ct_cap_need_t k_needs_capture[]  = {
    { (unsigned char)CAP_SCREEN_CAPTURE, (unsigned char)CAP_SCOPE_PATH, "doc" },
    { 0, 0, 0 }
};

// ---- the contract ----------------------------------------------------------
// Static items[] (Studio's control surface is not yet one table). Rows here are
// the AI-drivable actions + read-only state, NOT the whole 200-op menu system;
// that broader coverage is future work, honestly bounded, not claimed complete.
static const ct_item_t PAINT_ITEMS[] = {
    // read-only live state
    { "doc.width",        CT_INT, CT_READ, CT_SAFE, 0, 0, 100000, 0, 0, cg_width,    0, 0, 0, 0, 0,
      "active document width in pixels", k_needs_none },
    { "doc.height",       CT_INT, CT_READ, CT_SAFE, 0, 0, 100000, 0, 0, cg_height,   0, 0, 0, 0, 0,
      "active document height in pixels", k_needs_none },
    { "doc.layers",       CT_INT, CT_READ, CT_SAFE, 0, 0, 64, 0, 0, cg_layers,   0, 0, 0, 0, 0,
      "number of layers in the live document", k_needs_none },
    { "doc.active_layer", CT_INT, CT_READ, CT_SAFE, 0, 0, 64, 0, 0, cg_active,   0, 0, 0, 0, 0,
      "index of the active layer", k_needs_none },
    { "doc.modified",     CT_BOOL, CT_READ, CT_SAFE, 0, 0, 1, 0, 0, cg_modified, 0, 0, 0, 0, 0,
      "1 if the document has unsaved changes", k_needs_none },

    // SAFE actions (positive control: work with no grant, verifiable via
    // doc.layers over serial without a screendump)
    { "add_layer", CT_ACTION, CT_WRITE, CT_SAFE, 0, 0, 0, 0, 0, 0, 0, ca_add_layer, 0, 0, 0,
      "add a new empty layer above the active one", k_needs_none },
    { "grayscale", CT_ACTION, CT_WRITE, CT_SAFE, 0, 0, 0, 0, 0, 0, 0, ca_grayscale, 0, 0, 0,
      "desaturate the active layer to grayscale", k_needs_none },

    // GUARDED edit action (the red/green: refused with no aicap grant, allowed
    // with one). cap app.paint.edit is the aicap id; needs is empty because the
    // app performs it through its own filter_apply(), reaching no gated syscall.
    { "invert", CT_ACTION, CT_WRITE, CT_GUARDED, 0, 0, 0, 0, 0, 0, 0, ca_invert, 0, 0,
      "app.paint.edit", "invert the colors of the active layer", k_needs_none },

    // GUARDED perceive action: its teeth are the KERNEL screen.capture gate,
    // declared in needs. aicap id app.paint.observe bounds the AI half.
    { "screen_check", CT_ACTION, CT_WRITE, CT_GUARDED, 0, 0, 0, 0, 0, 0, 0, ca_screen_check, 0, 0,
      "app.paint.observe", "capture the live canvas for the AI to inspect", k_needs_capture },
};

const ct_contract_t PAINT_CONTRACT = {
    .app   = "paint",
    .title = "Maytera Studio",
    .note  = "AI-drivable actions on the live canvas; edits are CT_GUARDED "
             "(aicap app.paint.edit), screen_check is kernel-gated on screen.capture",
    .items = PAINT_ITEMS,
    .n     = (int)(sizeof(PAINT_ITEMS) / sizeof(PAINT_ITEMS[0])),
    .project = 0,
    .load  = 0,     // the live document is the truth; do NOT reload over it
    .commit = 0,    // each action repaints itself via ui_full_redraw()
};
