# libhelp - MayteraOS help subsystem (task #267)

A self-contained static library (`libhelp.a`) plus a Help viewer app that give
MayteraOS apps:

- A human-authorable help format, **.MHLP** (Maytera Help).
- A legacy **WinHelp .HLP** reader (MVP: topic list + readable text).
- A uniform in-memory document model so the viewer renders both identically.
- GUI help primitives: hover tooltips, a context-help "?" icon, and an
  F1 / Help-menu launcher.

Everything is freestanding C, no C++, no libm. Robust against malformed input:
a bad help file never crashes the caller.

## Files

- `help.h` - public document-model API (open/close, topics, blocks, search).
- `help_internal.h` - shared builder/string helpers (not public).
- `help_doc.c` - doc model, inline-markup parser, accessors, teardown.
- `mhlp.c` - the .MHLP parser (grammar documented at the top of the file).
- `hlp.c` - the WinHelp .HLP reader (coverage documented at the top).
- `help_open.c` - file load + magic sniff + format dispatch.
- `help_search.c` - case-insensitive full-text search.
- `help_ui.h` / `help_ui.c` - GUI help primitives (built on libc gui/style).
- `host_test/` - host-gcc unit tests for the parsers (no VM needed).

## The .MHLP format (quick reference)

```
@title <document title>
@topic <id> <Display Title...>
# Heading           ## Sub-heading
- list item
```                 (fenced code block; verbatim until the next ```)
![alt](path)        image reference (own line)
<blank line>        paragraph break
```

Inline markup inside headings / paragraphs / list items:

```
**bold**   *italic*   [label](#topic-id)   [label](http://...)
```

Lines beginning with `;` are comments. See `../../HELP/SYSTEM.MHLP` for a full
example.

## What the .HLP reader does / does not do

Parses: the file header and internal-file-system B+tree directory; `|SYSTEM`
(document title, compression flags); `|Phrases` phrase table; LZ77
decompression; the `|TOPIC` stream (TOPICLINK records) to recover per-topic
readable text and titles for a TOC.

Does NOT do: RTF layout (fonts/colors/tables/bitmaps are not rendered); the
newer `|PhrIndex`/`|PhrImage` phrase scheme (text still extracts, phrases are
not expanded); turning hotspots into clickable links (they render as plain
text). See the comment block at the top of `hlp.c` for the honest details.

## Three-line adoption pattern for any app

To add help to an app you only need three things in your own code:

```c
/* 1) include the header (adjust the relative path for your app's depth) */
#include "../../libhelp/help_ui.h"

/* 2) register a tooltip per widget once per redraw (window-relative rect) */
help_ui_register(win, x, y, w, h, "What this control does");

/* 3) in your event loop: */
//   mouse move:  help_ui_tick(local_x, local_y, uptime_ms());
//   each redraw: help_ui_draw(win);            // paint tooltip on top
//   F1 keydown:  if (help_ui_is_f1(ev.keycode))
//                    help_ui_open_topic("/HELP/SYSTEM.MHLP", "desktop");
```

Optionally draw the context-help icon and hit-test it:

```c
help_ui_question_icon(win, ICON_X, ICON_Y, 20);          // in draw
if (help_ui_question_hit(ICON_X, ICON_Y, 20, mx, my))    // in mouse-down
    help_ui_open_topic("/HELP/SYSTEM.MHLP", topic_id);
```

Add the library to your app Makefile: add `-I../../libhelp` to `CFLAGS`, add
`../../libhelp/libhelp.a` to the link line **before** `libc.a`.

The Settings app (`userland/apps/settings/main.c`) is the reference
integration: it registers a tooltip for every sidebar panel, draws the "?"
icon top-right of the content area, opens panel-specific help on F1 or a click
on "?", and reveals tooltips after a ~600 ms hover (driven from both mouse-move
events and the 100 ms idle timeout so a tooltip appears even when the cursor
is still).

## Building

```
make -C userland/libhelp                 # libhelp.a (freestanding)
make -C userland/apps/help               # the Help viewer ELF ("help")
make -C userland/apps/settings           # settings, now linking libhelp
make -C userland/libhelp/host_test test  # host unit tests (no VM)
```
