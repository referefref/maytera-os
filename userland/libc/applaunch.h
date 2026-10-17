// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// applaunch.h - THE launch registry: "given an app id, start that app".
//
// WHY THIS IS IN LIBC AND NOT IN AN APP. This logic was born inside
// libc/aiclient.c as the executor for the AI chat `app.launch` tool (#292), as a
// block of `static` functions no other caller could reach. It is not an AI
// feature: it is the answer to "what apps does this machine have, and how do I
// start one", which the Start menu, the AI tool loop and a workflow runner all
// need. The SECOND caller (Maytera Flow's `launch` node) is what forced the
// move, because the alternative was a second copy of the manifest parser and the
// gui/win16/dos dispatch, which is exactly the duplication that put
// /APPS/COMPOSITOR in one table while the kernel launched /APPS/COMPOSIT.
//
// SOURCE OF TRUTH is the on-device app manifest: the .MENU fragments under
// /CONFIG/STARTMENU/SYSTEM.D that the compositor's Start menu reads
// (userland/apps/compositor/startmenu.c, startmenu_model.rs). There is ONE list.
// Adding an app to the manifest (build/assets/startmenu/system.d/*.MENU, or an
// App Store install writing a fragment through libc/startmenu_reg.c) makes it
// Start-menu visible AND launchable by every caller here, with no code change.
//
// Line format, per startmenu_model.rs:
//   item: <Display Name> | <exec path [args]> | icon=<name> [| type=native|win16|dos]
// The launch KIND is the explicit `type=` field (absent means native).
#ifndef MAYTERA_APPLAUNCH_H
#define MAYTERA_APPLAUNCH_H

// Launch kinds. Dispatch matches startmenu.c's do_launch():
//   GUI   -> sys_spawn()/sys_spawn_args()  (native ELF: Files, DOOM, gbemu, ...)
//   WIN16 -> win16_run()                   (Win16 NE .EXE: SkiFree, FreeCell, ...)
//   DOS   -> dos_run()                     (MS-DOS .EXE/.COM: Keen, TIM, ...)
enum { APPLAUNCH_GUI = 0, APPLAUNCH_WIN16 = 1, APPLAUNCH_DOS = 2 };

#define APPLAUNCH_MAX      160
#define APPLAUNCH_ID_MAX   48
#define APPLAUNCH_PATH_MAX 192
#define APPLAUNCH_NAME_MAX 64

typedef struct {
    char id[APPLAUNCH_ID_MAX];     // primary id: slug of the display name ("Game Boy" -> gameboy)
    char alt[APPLAUNCH_ID_MAX];    // alternate id: slug of the exec basename ("/APPS/gbemu" -> gbemu); "" when equal
    char path[APPLAUNCH_PATH_MAX]; // the full exec line (may carry args for win16/dos)
    char name[APPLAUNCH_NAME_MAX]; // display name
    int  kind;                     // APPLAUNCH_GUI / _WIN16 / _DOS
} applaunch_ent_t;

// (Re)read the manifest. Called automatically by every accessor below on first
// use, so a caller only calls this explicitly to pick up a newly installed app.
// Returns the number of entries.
int applaunch_load(void);

// Number of launchable apps (lazy-loads).
int applaunch_count(void);

// Entry i, or NULL when out of range. The pointer is to the registry's own
// storage and stays valid until the next applaunch_load().
const applaunch_ent_t *applaunch_at(int i);

// Resolve a caller-supplied id, case-insensitively, against the primary id OR
// the exec-basename alternate, so "gameboy" and "gbemu" both reach the Game Boy
// app with no per-app code. NULL when nothing matches.
const applaunch_ent_t *applaunch_find(const char *id);

// The bare binary path of an entry: the first whitespace-delimited token of its
// exec line ("/DOS/STUNTS/LOAD.EXE /u MCGA" -> "/DOS/STUNTS/LOAD.EXE").
void applaunch_bin(const applaunch_ent_t *e, char *out, int cap);

// Resolve a ROM/content name to a /ROMS path by case-insensitive substring
// match ("pokemon" -> "/ROMS/POKEMON_YELLOW.GB"). Returns 1 on a hit.
// Deliberately generic: nothing here knows what a Game Boy is.
int applaunch_resolve_rom(const char *query, char *out, int cap);

// What applaunch_run() did, for a caller that must report it honestly.
typedef struct {
    int  rc;                            // sys_spawn / win16_run / dos_run return
    int  kind;                          // APPLAUNCH_*
    char id[APPLAUNCH_ID_MAX];          // the entry's primary id
    char name[APPLAUNCH_NAME_MAX];      // its display name
    char path[APPLAUNCH_PATH_MAX];      // the binary (GUI) or exec line (win16/dos)
    char rom[APPLAUNCH_PATH_MAX];       // the RESOLVED rom path, or ""
    int  rom_requested;                 // a rom query was supplied
    int  rom_matched;                   // and it resolved to a real file
} applaunch_result_t;

// Start `id`. `rom_query` (may be NULL/"") is resolved against /ROMS and passed
// as argv[1]; when it matches nothing the app is launched BARE so it can show
// its own picker, and res->rom_matched says so rather than failing. `arg` (may
// be NULL/"") is passed as argv[1] verbatim and is ignored when a rom matched.
// Both apply to NATIVE apps only: win16/dos exec lines carry their own args in
// the manifest and the kernel re-splits them.
//
// Returns 0 on a successful launch, APPLAUNCH_ERR_UNKNOWN when no entry matches
// `id`, or APPLAUNCH_ERR_SPAWN when the launch itself failed (res->rc holds the
// underlying code). It does NOT wait for a window: a caller that needs one
// visible must wait for it, because only that caller knows how long is too long.
#define APPLAUNCH_ERR_UNKNOWN (-1)
#define APPLAUNCH_ERR_SPAWN   (-2)
int applaunch_run(const char *id, const char *rom_query, const char *arg,
                  applaunch_result_t *res);

#endif // MAYTERA_APPLAUNCH_H
