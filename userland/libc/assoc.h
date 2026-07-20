#ifndef _MAYTERA_ASSOC_H
#define _MAYTERA_ASSOC_H
// OS-wide userland file associations (#84).
// Config file: /ASSOC.CFG, one "ext=apppath" per line (lowercase extension).
// All userland apps (Files, etc.) resolve the handler for a file through here,
// and the Settings "Default Apps" panel edits it.

// Resolve the app path that opens `filename` (by its extension). Writes into
// out (size outsz) and returns out. Never returns NULL; falls back to a built-in
// default table, and finally /APPS/editor for unknown types.
const char *assoc_app_for(const char *filename, char *out, int outsz);

// Set/replace the default app for an extension (e.g. "txt", "/APPS/editor").
// Persists to /ASSOC.CFG. Returns 0 on success.
int assoc_set_default(const char *ext, const char *apppath);

// File-type categories for the Settings "Default Apps" panel.
typedef struct {
    const char  *label;     // "Documents"
    const char  *exts;      // space-separated extensions this category owns
    const char **apps;      // candidate app paths to cycle through
    int          napps;
} assoc_category_t;

const assoc_category_t *assoc_categories(int *count);

// Current default app path for category `idx` (resolved via its first extension).
const char *assoc_category_current(int idx, char *out, int outsz);

#endif
