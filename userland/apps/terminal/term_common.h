// term_common.h
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
#ifndef TERM_COMMON_H
#define TERM_COMMON_H

// The exact include set the single-file terminal used, kept verbatim and in
// order so every module compiles in the same preprocessor environment the
// original main.c did.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/aiclient.h"
#include "../../libc/fcntl.h"     // #586: O_RDWR / O_NONBLOCK for /dev/ptmx
#include "../../libc/termios.h"   // #586: TIOCGPTN / TIOCSWINSZ / struct winsize
#include "../../libc/sys/ioctl.h" // #586: ioctl()
#include "../../libc/unistd.h"    // #745: getuid() (was an implicit declaration)
#include "../../libc/pwd.h"       // #745: the session user's real name and home
#include "../../libc/userconf.h"  // #745: <home>/APPS on PATH, and a real HOME/USER
#include "../../libc/pipeline.h"  // #745 local 108: the ONE pipeline runner
#include "../../libc/gui_scroll.h" // #206: shared scrollable-viewport primitive
#include "../../libc/dirent.h"    // termscroll: live /APPS listing for `help`
#include "../../libc/gui_list.h"  // #241: shared listbox, used by the theme picker
#include "../../libc/gui_style.h" // #241: gui_button() for the preferences dialog
#include "../../libc/gui_font.h"  // #241: shared ChooseFont dialog (reused, not forked)
#include "../../libc/gui_theme.h" // #241: gui_theme_list() - the real installed-theme set
#include "../../libc/gui_palette.h" // tier 2 (docs/TERMINAL_PARITY.md): TERMINAL COLOUR SCHEME, distinct from gui_theme.h's OS theme
#include "../../libc/contract.h"  // #241 (#233): terminal.* contract rows

#endif // TERM_COMMON_H
