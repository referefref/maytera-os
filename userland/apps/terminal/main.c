// terminal - GUI Terminal for MayteraOS (user-space version)
// A full-featured terminal with shell functionality and ANSI escape support
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
//
// main.c now owns ONLY: window lifecycle, the event loop, and wiring. Every
// other concern lives in one of the modules included below, and each of those
// is a separate ownership boundary so parallel work does not collide.

#include "term_common.h"
#include "term_util.h"
#include "term_grid.h"
#include "term_scrollback.h"
#include "term_parse.h"
#include "term_render.h"
#include "term_theme.h"
#include "term_prefs.h"
#include "term_profile.h"
#include "term_pty.h"
#include "term_shell.h"
#include "term_notify.h"   // PHASE 1: Konsole-class notifications
#include "term_layout.h"   // PHASE 1: tabs + splits
#include "term_search.h"   // PHASE 1: find-in-scrollback
#include "term_menu.h"     // PHASE 1: the menu bar

// Window position for coordinate conversion
static int win_x = 100;
static int win_y = 50;

int main(int argc, char **argv) {
    // #241/#233: a contract invocation must never open a window, so this
    // check comes before anything else in main() (same convention as
    // calc/main.c and settings/main.c).
    if (contract_is_invocation(argc, argv))
        return contract_cli(argc, argv, &TERMINAL_CONTRACT);

    // #241: load (or, on a virgin image, CREATE with the dark/DejaVu-Sans-
    // Mono/14pt defaults) TERMPREF.CFG before the window is sized, so
    // TERM_WIDTH/TERM_HEIGHT below (which read term_char_w/term_char_h)
    // already reflect a non-default saved font from the very first frame.
    term_prefs_load();

    // Initialize default environment variables.
    //
    // #745: PATH now leads with the session user's own application directory,
    // so an app they installed for themselves runs by name. HOME and USER were
    // hardcoded to "/" and the literal string "user" - not defaults, just
    // wrong - which meant "cd ~" and "echo $HOME" in the terminal disagreed
    // with msh, with the Files app, and with the kernel's own idea of the
    // session. All three now come from the passwd table through the one shared
    // home join. Root's home is "/", so a root session gets exactly the values
    // it had before, with PATH deduped to the single "/APPS".
    {
        char hp[192], home[192], pathv[400];
        if (userhome_path(0, "APPS", hp, sizeof(hp)) == 0 && !str_eq(hp, "/APPS")) {
            int o = 0;
            for (int i = 0; hp[i] && o < (int)sizeof(pathv) - 8; i++) pathv[o++] = hp[i];
            pathv[o++] = ':';
            const char *sysdir = "/APPS";
            for (int i = 0; sysdir[i] && o < (int)sizeof(pathv) - 1; i++) pathv[o++] = sysdir[i];
            pathv[o] = '\0';
            setenv_local("PATH", pathv);
        } else {
            setenv_local("PATH", "/APPS");
        }
        // #228: userhome_root() essentially never fails (it falls back to "/"
        // INTERNALLY when there is no passwd entry, per its own comment), so
        // `home` is always a valid, NUL-terminated path here; the else branch
        // below is the one case (an implausibly small stack buffer) where it
        // is not, and it must still leave `home` itself usable for the cwd
        // copy just below, not only the env var.
        if (userhome_root(home, sizeof(home)) == 0) setenv_local("HOME", home);
        else { home[0] = '/'; home[1] = '\0'; setenv_local("HOME", "/"); }
        struct passwd *pw = getpwuid(getuid());
        setenv_local("USER", (pw && pw->pw_name[0]) ? pw->pw_name : "user");

        // #228: THIS is the bug the owner-visible report was actually about.
        // HOME (above) and USER were fixed by #745 to come from the passwd
        // table; `cwd` - the shell's OWN idea of where it starts, which drives
        // the \w prompt, `pwd`, `cd` with no argument, and every relative path
        // typed at this prompt - was never connected to it and stayed at its
        // "/" module-scope default (see its declaration) regardless of who
        // signed in. MEASURED (#228, golden 2031): a freshly created, fully
        // provisioned account with a real, writable /HOME/<user> still opened
        // Terminal at "/", the one directory that account cannot write to.
        // `home` here is exactly what HOME was just set to; start there.
        {
            int i = 0;
            while (home[i] && i < (int)sizeof(cwd) - 1) { cwd[i] = home[i]; i++; }
            cwd[i] = '\0';
        }
    }
    setenv_local("SHELL", "/bin/terminal");
    setenv_local("TERM", "maytera-256color");

    // Sync the kernel per-process working directory with our tracked cwd
    // (now the session user's home, not "/" - #228) at startup. Children we
    // spawn (ls, cat, ...) inherit the kernel cwd, and tools like "ls" with no
    // argument resolve "." via getcwd(); without this they would see whatever
    // cwd the compositor happened to launch us with.
    // profiles: the DEFAULT profile's starting directory, if it names one that
    // exists, overrides $HOME for this window. Applied here, immediately before
    // the chdir that was already syncing the kernel cwd, so there is still one
    // chdir and one idea of where the shell starts.
    term_profile_startup(cwd, (int)sizeof(cwd));
    syscall1(SYS_CHDIR, (long)cwd);

    // Create window
    window_handle = win_create("Terminal", win_x, win_y, TERM_WIDTH, TERM_HEIGHT);
    if (window_handle < 0) {
        return 1;
    }

    printf("Terminal window created (handle=%d)\n", window_handle);

    // PHASE 1: notifications. Must follow win_create(): it primes its window-
    // state cache off window_handle.
    term_notify_init();

    // Allocate the scrollback ring buffer (#206). malloc(), not a static
    // array: user.ld links this app as a single RWX PT_LOAD segment and a
    // large .bss breaks that loader (see blame.md). A failed allocation
    // leaves sb_lines NULL, which every scrollback entry point already checks
    // and treats as "scrollback disabled", not a crash.
    term_scrollback_alloc();

    // Establish term_cols/term_px_w from the same path EVENT_RESIZE uses,
    // rather than trusting the static TERM_INIT_COLS/TERM_INIT_ROWS default to
    // already have the scrollbar gutter reserved (#206). A real EVENT_RESIZE
    // reporting the compositor's actual content size normally follows very
    // quickly and reconciles this either way, but this removes any window
    // where a scrollbar could be drawn over unreserved text columns before
    // that first EVENT_RESIZE lands.
    // PHASE 1 (tabs/splits): pane geometry is term_layout.c's job now, and
    // there must be exactly ONE path to it (#220 multiplied by the pane
    // count). term_layout_init() adopts the globals set up above as tab 0 /
    // pane 0 and gives that pane its geometry through the same single
    // function every later resize, split and divider drag uses.
    // PHASE 1: TWO chrome modules now reserve vertical space, and they compose
    // rather than compete. The MENU BAR sets term_content_y, which the layout's
    // one geometry function starts from, so it is ADDED to the window height
    // asked for here. The FIND BAR reserves its own strip and is SUBTRACTED,
    // exactly as #221 wrote it. Order on screen, top to bottom: menu bar, tab
    // strip, panes, find bar.
    term_menu_init();
    // #221: prime the find bar with the window's FULL content size before the
    // layout takes its share, so opening the bar can re-run the layout with
    // the strip subtracted without needing to ask the compositor again.
    term_search_note_window(TERM_INIT_COLS * TERM_CHAR_W,
                            TERM_INIT_ROWS * TERM_CHAR_H + term_content_y);
    term_layout_init(TERM_INIT_COLS * TERM_CHAR_W,
                     TERM_INIT_ROWS * TERM_CHAR_H + term_content_y
                         - term_search_reserved_h());

    // Initialize terminal
    term_clear();

    // No startup banner. The "Type 'help' for available commands." line was
    // already removed at the owner's explicit request (docs/TERMINAL_PARITY.md);
    // the "MayteraOS Terminal v1.0" version line that used to print right
    // above it is removed here for the same reason (owner report). `help`
    // itself is unchanged and still listed by builtin_help(). The
    // command-not-found hint further down this file that also says "Type
    // 'help'..." is a DIFFERENT message (contextual help after a failed
    // command) and was kept.

    // Print initial prompt
    print_prompt();

    // profiles: run the active profile's start command once, through the
    // normal shell path, so it appears in the scrollback and in history
    // exactly as if it had been typed.
    {
        const char *sc = term_profile_start_cmd();
        if (sc && sc[0]) {
            term_puts(sc);
            term_puts("\n");
            add_to_history(sc);
            execute_command(sc);
            // GUARDED, exactly as the event loop's own post-command path does
            // (see term_layout_pane_busy() below). execute_command() is
            // ASYNCHRONOUS for an external command: it spawns the child and
            // returns, the output is pumped by the event loop, and the loop
            // prints the prompt when the pane stops being busy. Printing one
            // here unconditionally put a second prompt BETWEEN the echoed
            // command and its own output - visible on screen as
            // "user@maytera:/TPDIR$ STARTCMD-RAN". A builtin is synchronous
            // and leaves the pane idle, so it still gets its prompt from here.
            if (!term_layout_pane_busy()) print_prompt();
        }
    }
    term_redraw();

    // Event loop
    gui_event_t event;
    int running = 1;

    while (running) {
        // #221: gui_mods_next_event(), NOT raw win_get_event(). It feeds the
        // shared modifier tracker every event in order AND performs the resync
        // that heals a stuck modifier bit, which is only correct when the wait
        // expires with an empty queue. This kernel emits no focus/blur event,
        // so that expiry is the ONLY safe healing point; the timeout must
        // therefore stay finite.
        // The timeout is finite BECAUSE gui_mods_next_event()'s modifier resync
        // only happens when the wait expires with an empty queue (#221), and it
        // is term_layout_timeout_ms() rather than a literal because the pty
        // pump wants 10 ms while a child is running and 100 ms when none is -
        // the two rates the two loops had before they became one.
        int event_type = gui_mods_next_event(window_handle, &event,
                                             term_layout_timeout_ms());

        if (event_type == 0) {
            // Idle: pump EVERY pane's pty child, blink the FOCUSED pane's
            // cursor, poll TERMPREF.CFG and reflow every pane if it changed.
            // The blink counter and the prefs poll that used to sit here are
            // inside it: both are per-pane now, and there is no such thing as
            // "the grid" to redraw from here any more.
            term_layout_idle();
            // PHASE 1: notifications. Kept HERE, not inside term_layout_idle(),
            // because it is per-TAB state that the layout does not own, and
            // because this branch already exists for the 100 ms
            // gui_mods_next_event() timeout so it adds no wait of its own
            // (#426); it only ever reads state and returns.
            if (term_notify_tick()) term_layout_redraw_all();
            // #307: the menu bar's own healing point. Same reason the modifier
            // resync above lives on this expiry - no blur event exists - and
            // the same cost profile: it only ever reads state and returns.
            if (term_menu_tick()) term_layout_redraw_all();
            // #221: advance the incremental find-in-scrollback scan. Bounded
            // work per call and no wait of any kind, so it adds nothing to
            // this branch's cost when no search is open (#426).
            if (term_search_tick()) term_layout_redraw_all();
            continue;
        }

        // PHASE 1 (tabs/splits): the layout sees every event first. It consumes
        // everything that belongs to the chrome (tab strip, pane headers,
        // dividers), everything that belongs to a pane other than "the focused
        // one" (mouse hit-testing), and every keystroke destined for a pane's
        // running pty child. It returns 0 only for keystrokes the BUILT-IN
        // SHELL line editor below should handle, and it has already made that
        // pane the active one, so the editor code is unchanged.
        //
        // The EVENT_REDRAW, EVENT_RESIZE and EVENT_MOUSE_* cases that used to
        // sit in the switch below are DELETED rather than left unreachable:
        // each of them operated on "the one grid", and a second copy of
        // window-resize or scroll handling that silently never runs is exactly
        // the kind of debt this uplift is paying off.
        // PHASE 1: THE CHROME GETS FIRST REFUSAL, TOP-MOST FIRST, and then the
        // layout. The order is the on-screen stacking order, which is the only
        // order that cannot surprise anyone: an open menu popup is drawn OVER
        // the find bar, so it must also be offered the click and the keystroke
        // first. Each of these consumes only what is genuinely its own and
        // falls straight through otherwise, so the layout's contract below is
        // unchanged.
        //
        // #307: the MENU BAR owns the chrome band above the tab strip; an open
        // popup owns the keyboard outright; and it claims F10, Alt+<letter> and
        // the four Ctrl+Shift accelerators no other module binds.
        if (term_menu_event(&event)) {
            if (term_menu_quit) running = 0;
            continue;
        }

        // #221: the find bar. It owns the keyboard while it is open (a find bar
        // is modal over the pty), and while it is shut it claims only its own
        // open shortcuts and the strip it drew itself into.
        {
            int sr = TERM_SEARCH_PASS;
            if (event.type == EVENT_KEY_DOWN)        sr = term_search_key_event(&event);
            else if (event.type == EVENT_MOUSE_DOWN) sr = term_search_mouse_down(event.mouse_x, event.mouse_y);
            // TERM_SEARCH_BAR repaints 26px, not every pane of the window. A
            // full repaint per keystroke was measurably enough to overflow the
            // per-window event queue and lose typed characters.
            if (sr == TERM_SEARCH_REDRAW) { term_layout_redraw_all(); continue; }
            if (sr == TERM_SEARCH_BAR)    { term_search_overlay(); win_invalidate(window_handle); continue; }
        }

        {
            int consumed = term_layout_event(event.type, &event);
            if (consumed < 0) { running = 0; continue; }
            if (consumed > 0) continue;
        }

        switch (event.type) {
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_KEY_DOWN:
                {
                    char c = event.key_char;
                    uint32_t keycode = event.keycode;

                    // termscroll: any key that is not one of the four
                    // scrollback navigation keys snaps the view back to the
                    // live screen first, if it was scrolled away. This is the
                    // near-universal "scroll on keystroke" convention (xterm,
                    // gnome-terminal, iTerm2, Windows Terminal all do it): a
                    // user who starts typing wants to see what they type and
                    // its result, not the history they were just reading.
                    // PgUp/PgDn/Home/End are exempt because THEY are the keys
                    // that move the view on purpose.
                    //
                    // Deliberately NOT applied to new program output (see
                    // term_scroll_region()/term_clear(): output only re-pins the view
                    // if it was already at the bottom). Auto-scrolling on
                    // output while the user is mid-read of scrollback is the
                    // behaviour every modern terminal (gnome-terminal, iTerm2,
                    // Windows Terminal) deliberately does NOT do by default,
                    // because a background/piped command finishing would yank
                    // the view away from what the user is reading. Snapping on
                    // keystroke and NOT on output are two different rules for
                    // two different triggers, not an inconsistency.
                    if (!term_at_bottom &&
                        keycode != GUI_KEY_PGUP && keycode != GUI_KEY_PGDN &&
                        keycode != GUI_KEY_HOME && keycode != GUI_KEY_END) {
                        term_scrollback_reconfigure();
                        gui_scroll_set(&term_scroll_view, gui_scroll_max(&term_scroll_view));
                        term_scroll_sync_bottom();
                    }

                    // #241's F9 handler moved to term_layout.c. It had TWO
                    // copies (here and inside run_foreground_pty()'s nested
                    // loop) and only the second one re-issued TIOCSWINSZ. With
                    // N panes the reflow must reach every pane's child, so
                    // there is now exactly one F9 path and it does.

                    // Handle special keys
                    if (keycode == 0x1C || c == '\n' || c == '\r') {
                        // Enter - execute command
                        term_puts("\n");
                        input_line[input_pos] = '\0';

                        if (str_eq(input_line, "exit")) {
                            running = 0;
                        } else {
                            add_to_history(input_line);
                            execute_command(input_line);
                            // PHASE 1: an external command now STARTS rather
                            // than runs to completion (term_layout_run_
                            // foreground()). Printing a prompt here would put
                            // one above the child's own output. The pump
                            // prints it when the child's master reports EOF.
                            if (!term_layout_pane_busy()) print_prompt();
                        }

                        input_pos = 0;
                        input_line[0] = '\0';
                        history_pos = history_count;
                        term_redraw();
                    }
                    else if (c == '\b' || keycode == 0x0E) {
                        // Backspace
                        if (input_pos > 0) {
                            input_pos--;
                            input_line[input_pos] = '\0';
                            term_putc('\b');
                            term_putc(' ');
                            term_putc('\b');
                            term_redraw();
                        }
                    }
                    else if (c == 27) {
                        // ESC - clear input
                        while (input_pos > 0) {
                            term_putc('\b');
                            term_putc(' ');
                            term_putc('\b');
                            input_pos--;
                        }
                        input_line[0] = '\0';
                        term_redraw();
                    }
                    else if (keycode == 0x80) {
                        // Up arrow - history previous
                        if (history_pos > 0 && history_count > 0) {
                            // Clear current input
                            while (input_pos > 0) {
                                term_putc('\b');
                                term_putc(' ');
                                term_putc('\b');
                                input_pos--;
                            }
                            history_pos--;
                            int idx = history_pos % TERM_SHELL_HISTORY_SIZE;
                            input_pos = 0;
                            while (history[idx][input_pos]) {
                                input_line[input_pos] = history[idx][input_pos];
                                term_putc(input_line[input_pos]);
                                input_pos++;
                            }
                            input_line[input_pos] = '\0';
                            term_redraw();
                        }
                    }
                    else if (keycode == 0x81) {
                        // Down arrow - history next
                        if (history_pos < history_count) {
                            // Clear current input
                            while (input_pos > 0) {
                                term_putc('\b');
                                term_putc(' ');
                                term_putc('\b');
                                input_pos--;
                            }
                            history_pos++;
                            if (history_pos < history_count) {
                                int idx = history_pos % TERM_SHELL_HISTORY_SIZE;
                                while (history[idx][input_pos]) {
                                    input_line[input_pos] = history[idx][input_pos];
                                    term_putc(input_line[input_pos]);
                                    input_pos++;
                                }
                            }
                            input_line[input_pos] = '\0';
                            term_redraw();
                        }
                    }
                    else if (keycode == GUI_KEY_TAB) {
                        // [tabcomp]: command/path completion. Previously
                        // unhandled here (keycode 0x09, and key_char '\t' is
                        // below ' ' so it also missed the printable-char
                        // branch below) - Tab was a silent no-op. Nothing
                        // upstream of this switch (term_menu_event/
                        // term_search_key_event/term_layout_event, all
                        // checked above) claims GUI_KEY_TAB either; the only
                        // other GUI_KEY_TAB use in this app is term_prefs.c's
                        // own modal Preferences dialog, a separate event
                        // loop. See term_shell.c's term_shell_complete() for
                        // the completion rules and why they are what they
                        // are.
                        term_shell_complete();
                    }
                    else if (keycode == GUI_KEY_PGUP) {
                        // #206: page back through scrollback. Deliberately
                        // NOT routed through gui_scroll_key() - that function
                        // also owns GUI_KEY_UP/DOWN, which this app already
                        // uses for command-history recall (0x80/0x81 above);
                        // calling it wholesale would steal those.
                        term_scrollback_reconfigure();
                        if (gui_scroll_by(&term_scroll_view, -term_scroll_view.h)) {
                            term_scroll_sync_bottom();
                            term_redraw();
                        }
                    }
                    else if (keycode == GUI_KEY_PGDN) {
                        term_scrollback_reconfigure();
                        if (gui_scroll_by(&term_scroll_view, term_scroll_view.h)) {
                            term_scroll_sync_bottom();
                            term_redraw();
                        }
                    }
                    else if (keycode == GUI_KEY_HOME) {
                        // Jump to the oldest retained line. Plain Home/End
                        // (not Shift/Ctrl-Home) because this terminal has no
                        // existing binding for them to collide with: there is
                        // no in-line cursor movement, only append/backspace.
                        term_scrollback_reconfigure();
                        if (gui_scroll_set(&term_scroll_view, 0)) {
                            term_scroll_sync_bottom();
                            term_redraw();
                        }
                    }
                    else if (keycode == GUI_KEY_END) {
                        // Jump back to the live screen.
                        term_scrollback_reconfigure();
                        if (gui_scroll_set(&term_scroll_view, gui_scroll_max(&term_scroll_view))) {
                            term_scroll_sync_bottom();
                            term_redraw();
                        }
                    }
                    else if (c >= ' ' && c < 127) {
                        // Printable character
                        if (input_pos < TERM_SHELL_INPUT_MAX - 1) {
                            input_line[input_pos++] = c;
                            input_line[input_pos] = '\0';
                            term_putc(c);
                            term_redraw();
                        }
                    }
                }
                break;

            // The four EVENT_MOUSE_* cases that were here are gone: with
            // splits, a click must first be hit-tested against the tab strip,
            // the pane headers, the dividers and then a SPECIFIC pane's
            // scrollbar. term_layout_event() does that and consumes them all.

            default:
                break;
        }
    }

    // Cleanup
    win_destroy(window_handle);
    printf("Terminal closed\n");

    return 0;
}
