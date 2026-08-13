// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// conv.h - local 66: the AI Interface conversation store.
//
// WHAT THIS IS
// ------------
// The chat panel used to hold exactly one conversation, in RAM, seeded fresh on
// every launch. This module gives it N named conversations (the tabs) that
// SURVIVE A REBOOT and that live in the SESSION USER'S OWN home, not in a
// single tree-wide file.
//
// WHERE IT LIVES, AND WHY THAT PATH
// ---------------------------------
// There is already exactly one convention in this tree for "a file that belongs
// to the logged-in user": userland/libc/userconf.c (#683). A per-user name NAME
// resolves to <home>/CONFIG/NAME, where <home> comes from the passwd table, and
// userconf_open_write() creates <home>/CONFIG and userconf_finish_write()
// completes the write with a checked write+fsync+close (#743). This module uses
// that convention rather than inventing a second one, so:
//
//     index          <home>/CONFIG/AICONVIX.TXT
//     conversation N <home>/CONFIG/AICONV0N.TXT   (N = 1..CONV_MAX_TABS)
//
// All names are FAT 8.3 uppercase like the rest of the tree. Root's home is "/",
// so a root session resolves these to /CONFIG/..., exactly as any other root
// preference does today.
//
// HONEST STATEMENT ABOUT ISOLATION. READ THIS BEFORE DESCRIBING THE FEATURE.
// --------------------------------------------------------------------------
// Putting each user's conversations under that user's home is SEPARATION, NOT A
// SECURITY BOUNDARY, and it must never be described as private or secure while
// task 20 is open. Filesystem permission enforcement is presently INERT: the
// shipping golden autologins as root, and the permission check returns 0
// (allow) unconditionally for uid 0. NOTHING STOPS A PROCESS READING ANOTHER
// USER'S CONVERSATION FILE. What this module guarantees today is only that two
// users do not SHARE a conversation list and cannot overwrite each other's by
// accident. The structure is correct so that when 20 lands the enforcement
// applies to files that are already in the right place; until then, treat the
// contents as readable by anything on the machine.
#ifndef AICHAT_CONV_H
#define AICHAT_CONV_H

#include "aiclient.h"

#define CONV_MAX_TABS    8         // tab strip stays legible in a 380px panel
#define CONV_TITLE_MAX   28
#define CONV_TEXT_MAX    2048      // per-message cap WHEN PERSISTED (RAM is uncapped)
#define CONV_DRAFT_MAX   1024      // matches MAX_INPUT in main.c
#define CONV_FILE_MAX    262144    // refuse to load a file larger than this

// One conversation (one tab). `texts` are malloc'd copies owned by this module.
typedef struct {
    int   slot;                        // 1..CONV_MAX_TABS; picks the file name
    int   detached;                    // torn out into its own window elsewhere
    int   auto_title;                  // 1 = title still derived, 0 = user renamed it
    char  title[CONV_TITLE_MAX];
    int   nmsgs;
    int   roles[AICLIENT_MAX_MSGS];
    char *texts[AICLIENT_MAX_MSGS];
    int   scroll;                      // transcript scroll offset, per tab
    char  draft[CONV_DRAFT_MAX];       // unsent input, per tab (RAM only)
} conv_t;

// --- list accessors --------------------------------------------------------
int      conv_count(void);
conv_t  *conv_at(int i);
int      conv_active(void);            // index into the list, not a slot number
const char *conv_index_path(void);     // resolved index path (for the self-test)

// --- persistence -----------------------------------------------------------
// Load the index and every conversation it lists. If only_slot > 0, load ONLY
// that slot (the detached-child case) and ignore the rest of the index.
// Returns the number of conversations loaded.
int  conv_load(int only_slot);
// Write one conversation, or the index. 0 on success, -1 if the file on disk is
// NOT known to hold the data (userconf_finish_write semantics, #743).
int  conv_save_one(int i);
int  conv_save_index(void);
int  conv_save_all(void);
// Clear every detached flag and persist. A docked instance calls this at
// startup: a fresh session has no detached children, so a flag left behind by a
// crashed one must not hide the conversation forever.
void conv_clear_detached(void);
// A detached child holds ONE conversation. If it wrote the index it would erase
// every other tab, because its in-memory list has one entry. So index writes are
// gated on OWNERSHIP, and ownership is decided in ONE place: conv_load() with a
// slot argument (i.e. "I am a child") drops it. Enforcing it here rather than at
// each call site is deliberate: "remember not to save the index" is exactly the
// kind of rule one path forgets, and the failure mode is silent data loss.
int  conv_index_owned(void);
// Child exit path: clear this slot's detached flag in the index WITHOUT
// rewriting the index from the child's one-entry list. Read-modify-write of the
// on-disk text, so the docked instance's other tabs are untouched.
void conv_release_detached(int slot);

// --- editing ---------------------------------------------------------------
int  conv_new(const char *title);      // -> new index, or -1 when full
void conv_snapshot(int i);             // aiclient history -> tab i
void conv_restore(int i);              // tab i -> aiclient history, sets active
int  conv_close(int i);                // drop tab + delete its file; -> new active
int  conv_move(int i, int dir);        // reorder by dir (-1 left, +1 right); -> new index
void conv_rename(int i, const char *title);
// Derive a title from the first user turn, unless the user has renamed the tab.
void conv_autotitle(int i);

#endif // AICHAT_CONV_H
