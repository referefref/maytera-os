// term_shell.c
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.

#include "term_common.h"
#include "term_util.h"
#include "term_grid.h"
#include "term_parse.h"
#include "term_render.h"
#include "term_pty.h"
#include "term_layout.h"

// The shell a pipeline is handed to. Named once: the COMPOSIT/COMPOSITOR name
// drift in this project came from an install name being spelled out at each
// use site.
#define MSH_PATH "/APPS/MSH"
#include "term_shell.h"
#include "term_layout.h"

// Input handling
char input_line[TERM_SHELL_INPUT_MAX];
int input_pos = 0;
char history[TERM_SHELL_HISTORY_SIZE][TERM_SHELL_INPUT_MAX];
int history_count = 0;
int history_pos = 0;

// Current working directory
char cwd[256] = "/";
// Environment variables (simple implementation)
#define MAX_ENV_VARS 32
// #112: the terminal's environment IS the process environment now. This used
// to be a private 64 x (64 + 256) table, a forked copy of what libc already
// had, and it never crossed a spawn: a `VAR=value` typed at this prompt, and
// the PATH/HOME/USER set at startup, were visible to this process alone.
// setenv_local/getenv_local below are adapters over setenv/getenv, so what the
// terminal sets is what libc's spawn wrappers hand to the kernel.
// Print the prompt
void print_prompt(void) {
    // Green color for prompt
    term_puts("\033[32muser@maytera\033[0m:\033[34m");
    term_puts(cwd);
    term_puts("\033[0m$ ");
}
// Set an environment variable. The name is kept for the call sites; what it
// writes is no longer local (#112).
void setenv_local(const char *name, const char *value) {
    setenv(name, value, 1);
}

const char *getenv_local(const char *name) {
    return getenv(name);
}

// Open a candidate path; on success copy it to out[256] and return 1.
static int try_open_path(const char *cand, char *out) {
    int fd = open(cand, 0);
    if (fd >= 0) { close(fd); str_copy(out, cand, 256); return 1; }
    return 0;
}

// Build "<dir>/<name>" into out[256]. up=1 uppercases name; ext=1 appends ".ELF".
static void make_candidate(char *out, const char *dir, int dirlen,
                           const char *name, int up, int ext) {
    int o = 0;
    for (int i = 0; i < dirlen && o < 255; i++) out[o++] = dir[i];
    if (o == 0 || out[o-1] != '/') { if (o < 255) out[o++] = '/'; }
    for (int i = 0; name[i] && o < 255; i++) {
        char c = name[i];
        if (up && c >= 'a' && c <= 'z') c -= 32;
        out[o++] = c;
    }
    if (ext) { const char *e = ".ELF"; for (int i = 0; e[i] && o < 255; i++) out[o++] = e[i]; }
    out[o] = '\0';
}

// Join a possibly-relative path onto the terminal's tracked cwd. An absolute
// path is returned unchanged; "./NAME", "SUB/NAME" and a bare "NAME" all
// resolve against cwd, because that is what a user who typed `cd /DOS/ALADDIN`
// and then `./ALADDIN.EXE` means. The terminal keeps the kernel's per-process
// cwd in sync (SYS_CHDIR in the `cd` builtin), but building the absolute path
// here rather than relying on the kernel to resolve "." keeps the string the
// user sees in messages identical to the string that was opened.
static void make_cwd_path(char *out, const char *rel) {
    if (rel[0] == '/') { str_copy(out, rel, 256); return; }
    if (rel[0] == '.' && rel[1] == '/') rel += 2;
    int o = 0;
    while (cwd[o] && o < 200) { out[o] = cwd[o]; o++; }
    if (o == 0 || out[o - 1] != '/') out[o++] = '/';
    for (int i = 0; rel[i] && o < 255; i++) out[o++] = rel[i];
    out[o] = '\0';
}

// Resolve a command name to an executable path the way a real shell does:
// search each ':'-separated directory in $PATH (default "/APPS") for the name
// in its common forms (exact, uppercased, and with a ".ELF" extension), then
// fall back to the per-game nested convention /GAMES/<NAME>/<NAME>.ELF (games
// ship in their own directory next to their data files). A name containing '/'
// is treated as a literal path. No program name is ever special-cased: install
// anything into a $PATH directory and it runs by name. Returns 1 + out on hit.
static int resolve_program(const char *name, char *out) {
    if (!name || !name[0]) return 0;

    // A name containing '/' is a path, not a $PATH lookup. It may still be
    // RELATIVE: "./ALADDIN.EXE" is the single most obvious thing to type from
    // inside the program's own directory, and it used to be opened verbatim as
    // the literal string "./ALADDIN.EXE", which resolves to nothing.
    for (const char *q = name; *q; q++)
        if (*q == '/') {
            if (name[0] == '/') return try_open_path(name, out);   // absolute
            char abs[256];
            make_cwd_path(abs, name);
            if (try_open_path(abs, out)) return 1;
            // Uppercase the leaf too: these filesystems are full of 8.3 names.
            char up2[256]; int u = 0;
            for (; abs[u] && u < 255; u++) {
                char c = abs[u];
                up2[u] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            up2[u] = '\0';
            return try_open_path(up2, out);
        }

    const char *path = getenv_local("PATH");
    if (!path || !path[0]) path = "/APPS";

    char cand[256];
    const char *p = path;
    while (*p) {
        const char *start = p;
        while (*p && *p != ':') p++;
        int dirlen = (int)(p - start);
        if (dirlen > 0 && dirlen < 200) {
            make_candidate(cand, start, dirlen, name, 0, 0); if (try_open_path(cand, out)) return 1;
            make_candidate(cand, start, dirlen, name, 1, 0); if (try_open_path(cand, out)) return 1;
            make_candidate(cand, start, dirlen, name, 0, 1); if (try_open_path(cand, out)) return 1;
            make_candidate(cand, start, dirlen, name, 1, 1); if (try_open_path(cand, out)) return 1;
        }
        if (*p == ':') p++;
    }

    // Per-game nested convention: /GAMES/<NAME>/<NAME>.ELF
    char up[64]; int j = 0;
    for (; name[j] && j < 63; j++) { char c = name[j]; if (c >= 'a' && c <= 'z') c -= 32; up[j] = c; }
    up[j] = '\0';
    int o = 0; const char *pre = "/GAMES/";
    for (int i = 0; pre[i] && o < 255; i++) cand[o++] = pre[i];
    for (int i = 0; up[i] && o < 255; i++) cand[o++] = up[i];
    if (o < 255) cand[o++] = '/';
    for (int i = 0; up[i] && o < 255; i++) cand[o++] = up[i];
    { const char *e = ".ELF"; for (int i = 0; e[i] && o < 255; i++) cand[o++] = e[i]; }
    cand[o] = '\0';
    if (try_open_path(cand, out)) return 1;

    // LAST resort, deliberately last: the current directory. A POSIX shell does
    // NOT search '.', and the $PATH loop above does not either, so anything
    // installed in a $PATH directory still wins and nothing in the user's cwd
    // can shadow it. But this is a single-user desktop where `cd /DOS/ALADDIN`
    // followed by `ALADDIN.EXE` is the obvious thing to type, and telling
    // someone "command not found" about a file they can see in `ls` and launch
    // from the Start menu is a worse failure than the shadowing risk a
    // last-place cwd lookup carries.
    { char abs[256]; make_cwd_path(abs, name); if (try_open_path(abs, out)) return 1; }
    { char abs[256]; make_cwd_path(abs, up);   if (try_open_path(abs, out)) return 1; }

    return 0;
}

// A GUI app creates its own window and runs its own event loop forever
// (it does not exit and may not produce stdout). Such apps must be launched
// detached: no stdout pipe capture and no blocking waitpid, otherwise the
// terminal would block forever in waitpid and the app would stall once the
// captured-output pipe fills. Games under /GAMES/ are GUI apps.
static int is_gui_app(const char *path) {
    return str_starts(path, "/GAMES/");
}
// ---- What KIND of executable is this? --------------------------------------
//
// The terminal used to assume every resolved path was a native ELF and hand it
// straight to sys_spawn_args(). A DOS MZ binary is not an ELF, so typing
// ALADDIN.EXE could never work - and it failed as "command not found", which is
// the wrong thing to say about a file the user is looking at in `ls` and can
// launch from the Start menu. SYS_DOS_RUN (240) and dos_launch() have existed
// and worked the whole time; the only thing missing was the surface a human
// reaches them through.
//
// CLASSIFY BY MAGIC READ FROM THE FILE, NEVER BY EXTENSION. Extension-sniffing
// would misroute a native ELF that happens to be named *.EXE into the DOS
// interpreter, and this tree does ship programs with unusual names. The single
// exception is .COM: a .COM file is a raw memory image with NO magic of any
// kind, so there is nothing to read, and for that one case the extension is the
// only signal available. It is used strictly as a FALLBACK, after the header
// has already been shown to be neither ELF nor MZ.
//
// MZ on its own is not enough either. NE (Win16), PE (Win32/64) and LE/LX
// (DOS-extended, the #740 DOS/4GW class) all begin with an MZ stub, so an
// MZ-means-DOS rule would feed Word 6 to the DOS interpreter. The 4-byte
// e_lfanew at offset 0x3C points at the real header, so read that too.
#define EXK_ERROR   (-1)   // exists per the resolver but cannot be opened/read now
#define EXK_UNKNOWN 0      // no recognised header: data, text, script
#define EXK_ELF     1      // native MayteraOS Ring-3 app  -> sys_spawn_args()
#define EXK_DOS     2      // plain MZ, or a .COM image    -> dos_run() / SYS_DOS_RUN
#define EXK_NE      3      // Win16 New Executable         -> win16_run() / SYS_WIN16_RUN
#define EXK_PE      4      // Win32/Win64 PE               -> no subsystem yet (#288)
#define EXK_LE      5      // LE/LX linear executable      -> DOS layer (#740 DPMI work)

// Case-insensitive check that path ends with a 4-char extension like ".COM".
static int has_ext4(const char *path, const char *ext) {
    int n = 0; while (path[n]) n++;
    if (n < 4) return 0;
    for (int i = 0; i < 4; i++) {
        char c = path[n - 4 + i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != ext[i]) return 0;
    }
    return 1;
}

static int exec_kind(const char *path) {
    unsigned char h[64];
    int fd = open(path, 0);
    if (fd < 0) return EXK_ERROR;
    int n = read(fd, (char *)h, (int)sizeof(h));
    int kind = EXK_UNKNOWN;
    if (n >= 4 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
        kind = EXK_ELF;
    } else if (n >= 2 && h[0] == 'M' && h[1] == 'Z') {
        kind = EXK_DOS;                       // plain DOS MZ unless e_lfanew says otherwise
        if (n >= 0x40) {
            unsigned long nh = (unsigned long)h[0x3C]
                             | ((unsigned long)h[0x3D] << 8)
                             | ((unsigned long)h[0x3E] << 16)
                             | ((unsigned long)h[0x3F] << 24);
            unsigned char t[2];
            if (nh >= 0x40 && nh < 0x08000000
                && lseek(fd, (off_t)nh, SEEK_SET) == (off_t)nh
                && read(fd, (char *)t, 2) == 2) {
                if      (t[0] == 'N' && t[1] == 'E')                       kind = EXK_NE;
                else if (t[0] == 'P' && t[1] == 'E')                       kind = EXK_PE;
                else if (t[0] == 'L' && (t[1] == 'E' || t[1] == 'X'))      kind = EXK_LE;
            }
        }
    } else if (has_ext4(path, ".COM")) {
        // FALLBACK, and only here: a .COM has no magic to check.
        kind = EXK_DOS;
    }
    close(fd);
    return kind;
}

// Launch a path that is NOT a native ELF through the kernel subsystem that owns
// it, and say something true when there isn't one. Returns 1 if this function
// dealt with the path (launched it or explained why not), 0 only for EXK_ELF,
// meaning the caller should carry on with the normal spawn.
//
// had_redir is passed so a "> file" on a DOS line gets a note rather than
// silently producing an empty file: a DOS guest draws into its own window and
// writes nothing to our stdout, so there is nothing for the redirect to catch.
static int term_launch_foreign(const char *path, int kind, int had_redir) {
    switch (kind) {
    case EXK_DOS:
    case EXK_LE:
        if (had_redir)
            term_puts("\033[33mnote: a DOS program draws in its own window and writes "
                      "nothing to stdout; the redirection has no effect.\033[0m\n");
        if (kind == EXK_LE)
            term_puts("\033[33mnote: this is an LE/LX (DOS-extended) binary; it needs the "
                      "DPMI host that is still being built (#740).\033[0m\n");
        if (dos_run(path) == 0) {
            term_puts("Launched DOS program ");
            term_puts(path);
            term_puts(" in its own window.\n");
        } else {
            term_puts("\033[31mCould not start the DOS program: ");
            term_puts(path);
            term_puts("\033[0m\n");
            term_puts("Only one DOS program runs at a time - close the running one "
                      "and try again.\n");
        }
        return 1;
    case EXK_NE:
        if (win16_run(path) == 0) {
            term_puts("Launched Windows 3.x program ");
            term_puts(path);
            term_puts(" in its own window.\n");
        } else {
            term_puts("\033[31mCould not start the Windows 3.x program: ");
            term_puts(path);
            term_puts("\033[0m\n");
            term_puts("Only one Win16 program runs at a time - close the running one "
                      "and try again.\n");
        }
        return 1;
    case EXK_PE:
        term_puts("\033[31m");
        term_puts(path);
        term_puts(" is a 32/64-bit Windows program (PE).\033[0m\n");
        term_puts("MayteraOS runs DOS and Windows 3.x programs; there is no Win32 "
                  "subsystem yet (#288).\n");
        return 1;
    case EXK_UNKNOWN:
        term_puts("\033[31m");
        term_puts(path);
        term_puts(" is not a program.\033[0m\n");
        term_puts("It has no ELF, MZ or .COM header, so there is nothing to run. "
                  "Try 'cat' or 'less' to look at it.\n");
        return 1;
    case EXK_ERROR:
        term_puts("\033[31mCannot read ");
        term_puts(path);
        term_puts("\033[0m\n");
        return 1;
    default:
        return 0;   // EXK_ELF: the normal path
    }
}
// Execute a shell command
// pipe_resolve: tokenize a single command string in place and resolve its
// program path via the /APPS lookup. Returns 1 on success.
static int pipe_resolve(char *line, char **argv, int *argcp, char *pathout) {
    int argc = 0;
    char *p = line;
    while (*p && argc < 31) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = (void *)0;
    if (argc == 0) return 0;
    *argcp = argc;
    char *prog = argv[0];
    // #745: this hand-rolled resolver had "/APPS/" written into it TWICE and
    // consulted $PATH nowhere, so a command that ran fine on its own became
    // "Command not found in pipe" the moment it appeared in a pipeline. That
    // was already wrong for anything outside /APPS; with a per-user
    // application directory it would have been wrong for every app a user
    // installed for themselves. resolve_program() is the shared resolver that
    // already knows about $PATH, the ".ELF" suffix and the nested /GAMES
    // convention, so use it instead of maintaining a second, weaker copy.
    return resolve_program(prog, pathout);
}

// run_pipe: execute "c1 | c2 | ..." with each stage's stdout feeding the next
// stage's stdin, and the LAST stage's stdout captured for display.
//
// #745 local 108: this used to be a hand-rolled two-stage implementation right
// here, and it was the only working pipeline in the OS - msh's split on '|'
// and ran the stages unconnected. Rather than copy this code into msh, the
// mechanism moved to userland/libc/pipeline.c, gained N stages, and both
// callers now use it. The fd juggling, the save/restore and the "drop our end
// so EOF propagates" ordering all live there now, once.
//
// KNOWN LIMIT, unchanged by the move and stated so it is not rediscovered:
// mpipe_run() waits for every stage, and this reads the capture pipe only
// afterwards, so a pipeline whose FINAL output exceeds the 64 KB pipe buffer
// stalls (the last stage cannot drain into a buffer nobody is reading). That
// was true of the previous implementation too. The fix is to capture to a file
// via the stage's own outfile rather than to a pipe.
static void run_pipe(char *cmdline) {
    static char  pathbuf[MPIPE_MAX_STAGES][256];
    static char *argvbuf[MPIPE_MAX_STAGES][32];
    static char  segbuf[MPIPE_MAX_STAGES][256];
    mpipe_stage_t st[MPIPE_MAX_STAGES];
    int n = 0;

    // Split on '|' into per-stage buffers.
    {
        const char *q = cmdline;
        while (n < MPIPE_MAX_STAGES) {
            int k = 0;
            while (*q && *q != '|' && k < 255) segbuf[n][k++] = *q++;
            segbuf[n][k] = '\0';
            n++;
            if (*q != '|') break;
            q++;
        }
        if (*q == '|') {
            term_puts("\033[31mpipe: too many stages\033[0m\n");
            return;
        }
    }

    for (int i = 0; i < n; i++) {
        int argc = 0;
        if (!pipe_resolve(segbuf[i], argvbuf[i], &argc, pathbuf[i])) {
            term_puts("\033[31mCommand not found in pipe\033[0m\n");
            return;
        }
        // A DOS or Win16 guest is not a pipeline stage. It opens its OWN
        // window, its output never reaches our stdout, and the kernel launcher
        // returns as soon as the guest thread is created - so
        // `ALADDIN.EXE | grep x` would either fail to spawn (it is not an ELF)
        // or, worse, appear to do nothing at all. Say so instead.
        {
            int k = exec_kind(pathbuf[i]);
            if (k == EXK_DOS || k == EXK_NE || k == EXK_LE) {
                term_puts("\033[31m");
                term_puts(pathbuf[i]);
                term_puts(" runs in its own window, so it cannot be part of a "
                          "pipeline.\033[0m\n");
                term_puts("Run it on its own line (or 'dos ");
                term_puts(pathbuf[i]);
                term_puts("').\n");
                return;
            }
        }
        argvbuf[i][0] = pathbuf[i];
        st[i].path = pathbuf[i];
        st[i].argv = argvbuf[i];
        st[i].argc = argc;
        st[i].infile = 0; st[i].outfile = 0; st[i].append = 0;
        st[i].builtin = 0; st[i].builtin_ctx = 0;
    }

    // =====================================================================
    // A FOREGROUND PIPELINE IS EXACTLY AS INTERACTIVE AS A FOREGROUND SIMPLE
    // COMMAND, AND NOW RUNS THE SAME WAY.
    // =====================================================================
    //
    // REPORTED: `ls | less` printed one screenful and returned to the prompt,
    // and no key would page it. Fixing the pager alone could not fix that,
    // because of what this function used to do to every pipeline it ran:
    //
    //   1. the LAST stage's stdout was a CAPTURE PIPE, never a terminal, so a
    //      full-screen program had nothing to draw on and no size to draw at;
    //   2. mpipe_run() WAITS for every stage, and this is the terminal's only
    //      thread, so nothing could have pumped a pty even if one existed. An
    //      interactive final stage would have deadlocked, not merely misdrawn;
    //   3. the captured output was read only AFTER every stage exited, so a
    //      pipeline could not paint progressively at all.
    //
    // None of that was a decision about pipelines. It predates #586, which
    // gave simple foreground commands a real pty (that is what makes `vi` and
    // `top` work), and predates PHASE 1, which made that path non-blocking so
    // one pane's child cannot freeze the window. The pipeline path was simply
    // never moved over. Moving it is the fix, and it deletes a limitation
    // rather than adding a special case: the 64 KB capture-pipe stall
    // documented above this function is gone too, because there is no longer a
    // capture pipe to fill.
    //
    // The mechanism is `msh -c '<line>'` on that same pty. msh is our shell,
    // its -c was written for exactly this shape, and its pipeline runner is
    // mpipe_run - the SAME one this function was calling. So the stages are
    // still built by the one shared runner; what changes is that they are now
    // built by a process that OWNS a terminal, so mpipe_run's inherit-stdio
    // (-1, -1) hands the last stage a real tty instead of a pipe. Nothing here
    // reimplements piping, and msh does not gain a second copy of anything.
    //
    // The resolve/classify loop above is KEPT as a pre-flight check, so
    // "Command not found in pipe" and the DOS/Win16-cannot-be-a-stage message
    // are still reported by the terminal, in the terminal's own words, before
    // anything is spawned.
    {
        int mfd = open(MSH_PATH, O_RDONLY);
        if (mfd >= 0) {
            close(mfd);
            static char  msh_line[1024];
            static char *msh_argv[4];
            int li = 0;
            for (const char *q = cmdline; *q && li < 1023; q++) msh_line[li++] = *q;
            msh_line[li] = '\0';
            msh_argv[0] = (char *)MSH_PATH;
            msh_argv[1] = (char *)"-c";
            msh_argv[2] = msh_line;
            msh_argv[3] = (char *)0;
            term_layout_run_foreground(MSH_PATH, msh_argv, 3);
            return;
        }
        // msh absent: fall through to the pre-#lesspipe capture path below.
        // This is a fallback for an IMAGE that lacks /APPS/MSH, not a second
        // way of doing the job. It cannot page, for all the reasons listed
        // above, but a pipeline that produces output beats a pipeline that
        // reports a missing shell.
    }

    int CAP[2] = { -1, -1 };
    if (pipe(CAP) != 0) {
        term_puts("\033[31mpipe: could not create the capture pipe\033[0m\n");
        return;
    }

    int rc = mpipe_run(st, n, -1, CAP[1], (void *)0);

    // Drop our write end BEFORE reading, or the read never sees EOF.
    close(CAP[1]);

    char buf[512];
    int r;
    while ((r = read(CAP[0], buf, sizeof(buf) - 1)) > 0) {
        buf[r] = '\0';
        term_puts(buf);
    }
    close(CAP[0]);

    if (rc < 0) {
        term_puts("\033[31mpipe: ");
        term_puts(mpipe_error());
        term_puts("\033[0m\n");
    }
}
// AI prefix (#224/#292): a terminal line beginning with '?' is sent to the
// built-in Kimi assistant via the SHARED aiclient module (the SAME tools and
// permission gate as the aichat widget and msh). The conversation persists across
// '?' lines for this terminal session, so the AI's two-step confirm flow for
// high-risk writes works (it asks; you reply "? yes"; it acts). The call is
// bounded (capped tool actions + a kernel HTTPS deadline) so it cannot wedge the
// terminal.
static int g_ai_ready = 0;   // 0=uninit, 1=ready, -1=no key

static void term_ai_handle(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    if (!*rest) {
        term_puts("Usage: ? <question or command for the AI>\n");
        return;
    }
    if (g_ai_ready == 0) {
        term_puts("\033[2mAI: connecting...\033[0m\n");
        term_redraw();
        g_ai_ready = aiclient_init() ? 1 : -1;
        if (g_ai_ready == 1) aiclient_reset();   // seed system prompt once per session
    }
    if (g_ai_ready != 1) {
        term_puts("Set your API key in Settings > AI.\n");
        return;
    }
    aiclient_add(0, rest);
    static char answer[8192];
    term_puts("\033[2mAI: thinking...\033[0m\n");
    term_redraw();
    int rc = aiclient_run_turn(answer, sizeof(answer), 0);
    if (rc != 0) { term_puts("\033[31mAI error:\033[0m "); term_puts(answer); term_puts("\n"); }
    else         { term_puts("\033[36mAI:\033[0m ");       term_puts(answer); term_puts("\n"); }
}
// #610 on-device filesystem checker. Mirrors ext2_fsck_report_t in
// kernel/fs/ext2.h; the kernel refuses the call unless the buffer is >= 200
// bytes, and the layout is locked there by _Static_assert.
#define SYS_FSCK 356
typedef struct {
    unsigned int completed, inodes_used, blocks_used, dirs_used;
    unsigned int e_bad_block_ptr, e_dup_block, e_phantom_inode, e_leaked_inode;
    unsigned int e_block_used_bitmap_free, e_block_free_bitmap_used;
    unsigned int e_bad_dirent, e_orphan_inode, e_link_mismatch;
    unsigned int e_group_free_bad, e_sb_free_bad, e_bad_inode, e_io, total;
    unsigned char first_msg[128];
} term_fsck_report_t;

static void term_fsck_line(const char *label, unsigned int n) {
    if (!n) return;
    term_puts("  \033[31m");
    term_puts(label);
    term_puts("\033[0m ");
    term_put_int((int)n);
    term_puts("\n");
}

static void term_do_fsck(void) {
    term_fsck_report_t r;
    long st = syscall3(SYS_FSCK, (long)&r, (long)sizeof(r), 1);
    if (st == -19) { term_puts("\033[31mfsck: no ext2 volume mounted\033[0m\n"); return; }
    if (st >= 0) {
        term_puts("\033[1;33mfsck\033[0m  ext2 root, s_state at mount 0x");
        term_put_int((int)(st & 0xFF));
        term_puts(", mount count ");
        term_put_int((int)((st >> 8) & 0xFFFF));
        term_puts((st >> 24) ? "  \033[31m[volume wants checking]\033[0m\n"
                             : "  \033[32m[last unmount was clean]\033[0m\n");
    }
    term_puts("\033[2mREAD-ONLY report. Nothing is repaired, and there is no repair\n"
              "mode: a repairing fsck can destroy the data it is asked to save.\n"
              "The volume is mounted and live, so a write in flight can surface as\n"
              "a spurious mismatch; the boot-time check is the authoritative one.\033[0m\n");
    term_puts("Scanning...\n");
    long rc = syscall3(SYS_FSCK, (long)&r, (long)sizeof(r), 0);
    if (rc != 0 || !r.completed) {
        term_puts("\033[31mfsck: could NOT run (rc=");
        term_put_int((int)rc);
        term_puts("). That is not a clean result.\033[0m\n");
        return;
    }
    term_puts("inodes in use: ");   term_put_int((int)r.inodes_used);
    term_puts("   directories: "); term_put_int((int)r.dirs_used);
    term_puts("   blocks in use: "); term_put_int((int)r.blocks_used);
    term_puts("\n");
    if (!r.total) { term_puts("\033[32mCLEAN: no problems found.\033[0m\n"); return; }
    term_puts("\033[1;31m");
    term_put_int((int)r.total);
    term_puts(" problem(s) found:\033[0m\n");
    term_fsck_line("out-of-range block pointers ......", r.e_bad_block_ptr);
    term_fsck_line("multiply-claimed blocks .........", r.e_dup_block);
    term_fsck_line("phantom inodes (used, bmap free) .", r.e_phantom_inode);
    term_fsck_line("leaked inodes ...................", r.e_leaked_inode);
    term_fsck_line("blocks in use but free in bitmap .", r.e_block_used_bitmap_free);
    term_fsck_line("blocks marked used, unreferenced .", r.e_block_free_bitmap_used);
    term_fsck_line("corrupt directory entries .......", r.e_bad_dirent);
    term_fsck_line("orphan inodes (lost+found) ......", r.e_orphan_inode);
    term_fsck_line("wrong link counts ...............", r.e_link_mismatch);
    term_fsck_line("wrong group summary counts ......", r.e_group_free_bad);
    term_fsck_line("wrong superblock free counts ....", r.e_sb_free_bad);
    term_fsck_line("implausible inodes ..............", r.e_bad_inode);
    term_fsck_line("block read failures .............", r.e_io);
    if (r.first_msg[0]) {
        term_puts("  first: ");
        term_puts((const char *)r.first_msg);
        term_puts("\n");
    }
}
// ---- Built-in command table -----------------------------------------------
//
// This used to be a chain of `if (str_eq(cmd, "..."))`, and `help` printed a
// separate, hand-typed sentence describing it. The two were free to diverge,
// and had: `help` claimed a nonexistent `uptime` builtin and never mentioned
// `vi`, because nothing forced the blurb to agree with the dispatcher. Every
// built-in now lives in ONE table (g_builtins[] below); execute_command()
// dispatches through it and builtin_help() walks the SAME table to describe
// one, so a name cannot appear in one and be silent in the other.
//
// Each handler receives the text after the verb, already whitespace-trimmed
// and never NULL (an empty string for a bare "cd", "dos", etc).
typedef void (*builtin_fn_t)(const char *args);

static void builtin_help(const char *args);
static void builtin_clear(const char *args);
static void builtin_fsck(const char *args);
static void builtin_dos(const char *args);
static void builtin_pwd(const char *args);
static void builtin_history(const char *args);
static void builtin_export(const char *args);
static void builtin_cd(const char *args);
static void builtin_exit(const char *args);
static void builtin_termstat(const char *args);

typedef struct {
    const char   *name;
    const char   *desc;   // one line, shown by builtin_help()
    builtin_fn_t  fn;
} builtin_t;

static const builtin_t g_builtins[] = {
    { "cd",      "change directory (bare = /)",                          builtin_cd },
    { "pwd",     "print the working directory",                          builtin_pwd },
    { "clear",   "clear the visible screen (scrollback keeps the history)", builtin_clear },
    { "history", "list commands typed this session",                     builtin_history },
    { "export",  "export VAR=VALUE into the environment",                builtin_export },
    { "dos",     "launch a DOS/Win3.x program: dos <path>",              builtin_dos },
    { "fsck",    "check the ext2 root filesystem (read-only, never repairs)", builtin_fsck },
    { "help",    "show this list",                                       builtin_help },
    { "exit",    "close the terminal session",                           builtin_exit },
    { "termstat","repaint counters for this window (termstat reset to zero them)", builtin_termstat },
};
#define NUM_BUILTINS ((int)(sizeof(g_builtins) / sizeof(g_builtins[0])))

static void builtin_clear(const char *args) {
    (void)args;
    term_clear();
    // THE ESCAPE HATCH, and the reason it is here and not in term_clear().
    // term_clear() is also what ESC[2J reaches, and the first thing every
    // full-screen program does is ESC[?25l followed by ESC[2J - restoring the
    // cursor in term_clear() would force it back on over vi's own first
    // screen, which is a worse bug than the one being fixed. The `clear`
    // COMMAND is a different thing: it is this shell's own builtin, the user
    // typed it, and "my cursor is gone and I cannot get it back" has to have
    // an answer that is not "restart the terminal".
    cursor_visible = true;
}

// #610: on-device filesystem check. Report-only, by design.
static void builtin_fsck(const char *args) {
    (void)args;
    term_do_fsck();
}

// Explicit DOS launch. Header sniffing (exec_kind) already makes
// `ALADDIN.EXE` and `./ALADDIN.EXE` work on their own, but an explicit verb
// earns its place twice over: it is UNAMBIGUOUS when the sniff cannot be
// (a .COM with contents that look like something else, a DOS binary with no
// extension at all), and it makes the capability DISCOVERABLE, because a
// user who does not already know DOS programs are runnable will find it in
// `help` and nowhere else. This is a builtin rather than an /APPS program
// so it can share the terminal's cwd-aware resolver.
static void builtin_dos(const char *args) {
    if (!*args) {
        term_puts("Usage: dos <path to a DOS .EXE or .COM>\n");
        term_puts("Example: dos /DOS/ALADDIN/ALADDIN.EXE\n");
        term_puts("DOS programs also run just by naming them: from /DOS/ALADDIN,\n");
        term_puts("both ALADDIN.EXE and ./ALADDIN.EXE work.\n");
        return;
    }
    char dpath[256];
    if (!resolve_program(args, dpath)) {
        term_puts("\033[31mdos: no such file: ");
        term_puts(args);
        term_puts("\033[0m\n");
        return;
    }
    // Refuse only the case we KNOW is wrong. Anything else is forced to the
    // DOS layer on purpose: that is what an explicit verb is for.
    if (exec_kind(dpath) == EXK_ELF) {
        term_puts("\033[31mdos: ");
        term_puts(dpath);
        term_puts(" is a native MayteraOS program, not a DOS one.\033[0m\n");
        term_puts("Run it by name on its own.\n");
        return;
    }
    term_launch_foreign(dpath, EXK_DOS, 0);
}

static void builtin_pwd(const char *args) {
    (void)args;
    term_puts(cwd);
    term_puts("\n");
}

static void builtin_history(const char *args) {
    (void)args;
    if (history_count == 0) {
        term_puts("No commands in history.\n");
        return;
    }
    int start = history_count > TERM_SHELL_HISTORY_SIZE ? history_count - TERM_SHELL_HISTORY_SIZE : 0;
    for (int i = start; i < history_count; i++) {
        int idx = i % TERM_SHELL_HISTORY_SIZE;
        term_puts("  ");
        term_put_int(i + 1);
        term_puts("  ");
        term_puts(history[idx]);
        term_puts("\n");
    }
}

static void builtin_export(const char *args) {
    const char *eq = args;
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') {
        term_puts("\033[31mUsage: export VAR=VALUE\033[0m\n");
        return;
    }
    char varname[64];
    int i = 0;
    const char *p = args;
    while (p < eq && i < 63) { varname[i++] = *p++; }
    varname[i] = '\0';
    setenv_local(varname, eq + 1);
}

static void builtin_cd(const char *args) {
    if (!*args || str_eq(args, "~")) {
        cwd[0] = '/';
        cwd[1] = '\0';
        return;
    }

    // Build the candidate path first, then validate it actually exists
    // (and is a directory) via SYS_CHDIR before committing to cwd. The old
    // code edited cwd unconditionally, so `cd /does/not/exist` silently
    // "succeeded".
    char cand[256];
    const char *dir = args;
    if (*dir == '/') {
        int i = 0;
        while (*dir && i < 255) { cand[i++] = *dir++; }
        cand[i] = '\0';
    } else if (str_eq(dir, "..")) {
        int len = 0;
        while (cwd[len] && len < 255) { cand[len] = cwd[len]; len++; }
        cand[len] = '\0';
        if (len > 1) {
            len--;
            while (len > 0 && cand[len] != '/') len--;
            if (len == 0) len = 1;
            cand[len] = '\0';
        }
    } else if (str_eq(dir, ".")) {
        return;
    } else {
        int len = 0;
        while (cwd[len] && len < 255) { cand[len] = cwd[len]; len++; }
        if (len > 1 && len < 255) cand[len++] = '/';
        while (*dir && len < 255) { cand[len++] = *dir++; }
        cand[len] = '\0';
    }

    if (syscall1(SYS_CHDIR, (long)cand) < 0) {
        term_puts("cd: ");
        term_puts(cand);
        term_puts(": No such file or directory\n");
        return;
    }
    { int i = 0; while (cand[i]) { cwd[i] = cand[i]; i++; } cwd[i] = '\0'; }
}

static void builtin_exit(const char *args) {
    (void)args;
    term_puts("Goodbye!\n");
}

// qsort() comparator for the external-app listing.
static int term_help_name_cmp(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa, sb);
}

// List every regular file in /APPS, sorted, in aligned columns. Derived LIVE
// from the real directory (opendir/readdir) every time `help` runs, never
// from a maintained list: a name appears here if and only if readdir()
// actually returned it, so a binary that is missing cannot be listed and one
// that exists cannot be silently omitted. This is what makes the #240 report
// against the old blurb structurally impossible to repeat: it named a
// nonexistent "uptime" and never mentioned "vi", because it was prose, not a
// query against the filesystem.
#define TERM_HELP_MAX_APPS 512
static void term_help_list_apps(void) {
    static char names[TERM_HELP_MAX_APPS][64];
    static char *ptrs[TERM_HELP_MAX_APPS];
    int n = 0;

    DIR *d = opendir("/APPS");
    if (!d) {
        term_puts("\033[31m(could not read /APPS)\033[0m\n");
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < TERM_HELP_MAX_APPS) {
        if (de->d_type == DT_DIR) continue;   // no subdirectories today; skip defensively
        if (de->d_name[0] == '.') continue;   // "." / ".." defensiveness
        str_copy(names[n], de->d_name, 64);
        ptrs[n] = names[n];
        n++;
    }
    closedir(d);

    if (n == 0) {
        term_puts("(no entries found in /APPS)\n");
        return;
    }

    qsort(ptrs, (unsigned long)n, sizeof(ptrs[0]), term_help_name_cmp);

    // Column layout: the widest name in THIS listing sets the column width;
    // the current window width (in character cells, so it tracks a resize
    // and the font-size picker) decides how many columns fit. Column-major
    // (down, then across), matching the `ls -C` convention rather than
    // dumping 176+ names as one unbroken wall of text.
    int maxlen = 0;
    for (int i = 0; i < n; i++) {
        int l = 0; while (ptrs[i][l]) l++;
        if (l > maxlen) maxlen = l;
    }
    int colw = maxlen + 2;
    int cols = term_cols / colw;
    if (cols < 1) cols = 1;
    int rows = (n + cols - 1) / cols;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = c * rows + r;
            if (idx >= n) continue;
            term_puts(ptrs[idx]);
            int l = 0; while (ptrs[idx][l]) l++;
            for (int pad = l; pad < colw; pad++) term_puts(" ");
        }
        term_puts("\n");
    }
    term_puts("\033[2m");
    term_put_int(n);
    term_puts(" entries, listed live from /APPS.\033[0m\n");
}

// (#damage) THE MEASUREMENT, not an impression. `scanned` is the number of
// cells the pre-damage-tracking renderer painted for exactly this session
// (term_rows * term_cols, once per term_redraw()); `painted` is what this one
// actually painted. Both are counted by the SAME build on the SAME workload,
// which is the only honest way to compare them: a TUI is not deterministic
// enough for two separate runs of two separate binaries to be comparable.
//
// Run a real full-screen program, quit it, then type `termstat`.
static void builtin_termstat(const char *args) {
    while (*args == ' ') args++;
    if (str_eq(args, "reset")) {
        term_stat_reset();
        term_puts("termstat: counters zeroed\n");
        return;
    }
    char line[160];
    unsigned long sc = term_stat_cells_scanned;
    unsigned long pa = term_stat_cells_painted;
    snprintf(line, sizeof(line), "frames        %lu  (full %lu, painted nothing %lu)\n",
             term_stat_frames, term_stat_full_frames, term_stat_frames_idle);
    term_puts(line);
    snprintf(line, sizeof(line), "cells scanned %lu   (what a full-grid repaint would paint)\n", sc);
    term_puts(line);
    snprintf(line, sizeof(line), "cells painted %lu\n", pa);
    term_puts(line);
    if (sc > 0) {
        // Integer arithmetic only: this is a terminal, and a ratio printed to
        // two decimals from integers is exact where a float would invite a
        // question about rounding.
        unsigned long pct100 = (pa * 10000ul) / sc;
        snprintf(line, sizeof(line), "painted/scanned  %lu.%02lu%%   reduction %lux\n",
                 pct100 / 100ul, pct100 % 100ul, pa ? (sc / pa) : sc);
        term_puts(line);
    }
    snprintf(line, sizeof(line), "presents      %lu  (win_invalidate calls)\n",
             term_stat_invalidates);
    term_puts(line);
    snprintf(line, sizeof(line), "grid          %d cols x %d rows = %d cells\n",
             term_cols, term_rows, term_cols * term_rows);
    term_puts(line);
}

static void builtin_help(const char *args) {
    (void)args;
    term_puts("\033[1;33mMayteraOS Terminal - Commands:\033[0m\n\n");
    term_puts("Built-in:\n");
    for (int i = 0; i < NUM_BUILTINS; i++) {
        term_puts("  ");
        term_puts(g_builtins[i].name);
        int l = 0; while (g_builtins[i].name[l]) l++;
        for (int pad = l; pad < 10; pad++) term_puts(" ");
        term_puts(g_builtins[i].desc);
        term_puts("\n");
    }
    term_puts("\n\033[2mAI: a line starting with '?' is sent to the built-in assistant.\n");
    term_puts("Scrollback: PageUp/PageDown/Home/End or the mouse wheel; drag the\n");
    term_puts("  scrollbar when one is shown. F9 opens Terminal Preferences.\n");
    term_puts("Use UP/DOWN arrows for command history.\033[0m\n");
    term_puts("\nExternal, read live from /APPS (not a hand-typed list):\n");
    term_help_list_apps();
}

void execute_command(const char *cmd) {
    // Skip leading whitespace
    while (*cmd == ' ') cmd++;

    // Empty command
    if (!*cmd) return;

    // AI prefix: a line starting with '?' goes to the built-in assistant.
    if (*cmd == '?') { term_ai_handle(cmd + 1); return; }

    // Split into a verb (first word) and the remainder, trimmed of leading
    // spaces. The dispatch loop below matches only the verb against
    // g_builtins[], so `cd /DOS`, `cd  /DOS` and `cd` (bare) all reach
    // builtin_cd() with the right `args`, exactly as the old
    // str_starts(cmd, "cd ") / str_eq(cmd, "cd") pair used to, just without
    // needing two entries per builtin.
    char verb[32];
    {
        int vi = 0;
        const char *q = cmd;
        while (*q && *q != ' ' && vi < 31) verb[vi++] = *q++;
        verb[vi] = '\0';
    }
    const char *args = cmd;
    while (*args && *args != ' ') args++;
    while (*args == ' ') args++;

    for (int i = 0; i < NUM_BUILTINS; i++) {
        if (str_eq(verb, g_builtins[i].name)) {
            g_builtins[i].fn(args);
            return;
        }
    }

    // Pipe: "cmd1 | cmd2 | ..." (#745 local 108: N stages, was two).
    {
        const char *bar = (void *)0;
        for (const char *q = cmd; *q; q++) { if (*q == '|') { bar = q; break; } }
        if (bar) {
            static char plbuf[1024];
            int li = 0;
            for (const char *q = cmd; *q && li < 1023; q++) plbuf[li++] = *q;
            plbuf[li] = '\0';
            run_pipe(plbuf);
            return;
        }
    }

    // All other commands: try to run as external program with arguments
    {
        // Parse command into argv
        char cmd_copy[512];
        int ci = 0;
        while (cmd[ci] && ci < 511) { cmd_copy[ci] = cmd[ci]; ci++; }
        cmd_copy[ci] = '\0';

        char *argv_ptrs[32];
        int argc = 0;
        char *p = cmd_copy;
        while (*p && argc < 31) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                argv_ptrs[argc++] = p;
                while (*p && *p != q) p++;
                if (*p) *p++ = '\0';
            } else {
                argv_ptrs[argc++] = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) *p++ = '\0';
            }
        }
        argv_ptrs[argc] = (void *)0;

        if (argc == 0) return;

        // --- I/O redirection: pull >, >>, <, 2> (and their filenames) out of
        // argv so they are not passed to the child. Open flags are numeric to
        // avoid header deps: O_WRONLY=0x1 O_CREAT=0x40 O_TRUNC=0x200 O_APPEND=0x400.
        const char *redir_out = (void *)0; int redir_append = 0;
        const char *redir_in  = (void *)0;
        const char *redir_err = (void *)0;
        {
            int w = 1;  // keep argv[0]
            for (int r = 1; r < argc; r++) {
                char *t = argv_ptrs[r];
                int is_out = str_eq(t, ">");
                int is_app = str_eq(t, ">>");
                int is_in  = str_eq(t, "<");
                int is_err = str_eq(t, "2>");
                if ((is_out || is_app || is_in || is_err) && r + 1 < argc) {
                    const char *fn = argv_ptrs[++r];
                    if (is_in)       redir_in = fn;
                    else if (is_err) redir_err = fn;
                    else { redir_out = fn; redir_append = is_app; }
                } else {
                    argv_ptrs[w++] = t;
                }
            }
            argv_ptrs[w] = (void *)0;
            argc = w;
        }
        if (argc == 0) return;

        // Resolve command path via $PATH (default /APPS) + nested-game
        // convention. Fully generic: no program name is special-cased.
        char prog_name[64];
        str_copy(prog_name, argv_ptrs[0], 64);

        char path[256];
        int found = resolve_program(prog_name, path);

        if (!found) {
            term_puts("\033[31mCommand not found: ");
            term_puts(prog_name);
            term_puts("\033[0m\n");
            // There is no `run` verb in this shell and inventing one is not the
            // fix. But `run ALADDIN.EXE` is what a person reasonably tries when
            // naming the file alone has just failed, and when the FIRST
            // ARGUMENT resolves to a real DOS/Win16 binary we know exactly what
            // was meant, so say it rather than leaving them to guess again.
            if (argc > 1) {
                char apath[256];
                if (resolve_program(argv_ptrs[1], apath)) {
                    int ak = exec_kind(apath);
                    if (ak == EXK_DOS || ak == EXK_NE || ak == EXK_LE) {
                        term_puts("Did you mean:  dos ");
                        term_puts(apath);
                        term_puts("\n");
                        return;
                    }
                }
            }
            term_puts("Type 'help' for available commands.\n");
            return;
        }

        // Update argv[0] to the resolved path
        argv_ptrs[0] = path;

        // Not everything executable on this system is an ELF. Classify the file
        // by its header and route DOS (.EXE/.COM) and Win16 (NE) to the kernel
        // launchers that own them, BEFORE the sys_spawn_args() ELF path below.
        // This is also what turns the old, misleading "command not found" into
        // a true statement for a file that exists but is not runnable here.
        {
            int kind = exec_kind(path);
            int had_redir = (redir_out || redir_in || redir_err) ? 1 : 0;
            if (term_launch_foreign(path, kind, had_redir)) return;
        }

        // GUI apps (e.g. games under /GAMES/) create their own window and run
        // forever. Launch them detached: no stdout pipe, no waitpid. The child
        // inherits the terminal's fd 1 (NULL -> kernel routes putchar to the
        // serial console), exactly like a compositor icon launch.
        if (is_gui_app(path)) {
            int pid = sys_spawn_args(path, argv_ptrs, argc);
            if (pid > 0) {
                term_puts("Launched ");
                term_puts(path);
                term_puts("\n");
            } else {
                term_puts("\033[31mFailed to run: ");
                term_puts(path);
                term_puts("\033[0m\n");
            }
            return;
        }

        // #586: a simple foreground command (no file redirection) runs on a
        // real pty so it can read the keyboard on stdin - this is what makes
        // interactive TUIs like vi work. Redirections ("> file", "< file")
        // still use the capture path below.
        if (!redir_out && !redir_in && !redir_err) {
            // PHASE 1 (tabs/splits): this was run_foreground_pty(), which did
            // not return until the child exited because it owned a nested
            // event loop. With splits that froze every other pane. The child
            // is now STARTED here and pumped by the ONE event loop; the next
            // shell prompt is printed by that pump when the child's master
            // reports EOF, not by this caller's return (see main.c's
            // term_layout_pane_busy() check).
            term_layout_run_foreground(path, argv_ptrs, argc);
            return;
        }

        // Capture the child's stdout through a pipe (pipes, dup2 and spawn fd
        // inheritance all share the VFS per-process fd table). For "> file" we
        // buffer the captured output and write it after the child exits.
        //
        // #FDNS (2026-08-23): this comment used to state an ORDERING RULE - the
        // pipe had to be closed BEFORE the file was opened, because a legacy
        // (FAT/ext2) file fd could be handed the same small integer as a still
        // open VFS pipe fd, and sys_write probes VFS first, so the write landed
        // on the pipe. That rule was the write-side face of a kernel defect,
        // and its read-side face is what printed /CONFIG/PASSWD into this
        // window in place of a command's output. The kernel now gives the two
        // fd families DISJOINT number ranges (kernel/proc/fdlayer.c,
        // LEGACY_FD_BASE), so the collision cannot be expressed and the
        // ordering is no longer load-bearing. The order below is unchanged
        // because buffering-then-writing is still the simplest thing that
        // works, NOT because anything depends on it any more.
        // ("< file" and "2> file" are parsed but not yet wired.)
        int pipefd[2] = {-1, -1};
        int use_pipe = (pipe(pipefd) == 0);
        if (use_pipe) {
            dup2(pipefd[1], 1);  // fd 1 = pipe write end
            close(pipefd[1]);
            pipefd[1] = -1;
        }

        int pid = sys_spawn_args(path, argv_ptrs, argc);

        if (use_pipe) {
            close(1);       // drop our write end so the child's reads see EOF
            // #745 (local 99): AND PUT fd 1 BACK. Closing it here and never
            // reopening it left the Terminal running with NO stdout for the
            // rest of its life, and the damage landed on the NEXT command
            // rather than this one: run_foreground_pty() starts with
            // `int s1 = dup(1)`, which fails on a closed fd and yields -1, so
            // its matching `dup2(s1, 1)` restore is a no-op and fd 1 is left
            // pointing at the PTY SLAVE. The Terminal then holds a slave
            // reference forever, slave_refs never reaches 0, the master never
            // reports EOF, and run_foreground_pty() pumps for ever: the child
            // has exited but the window never returns to a prompt.
            //
            // MEASURED on golden build 1872 and again on 1873 before this
            // fix: `cat`+^D returns to the prompt; then ANY redirection
            // (`echo x > /r.txt`); then `cat`+^D wedges the window
            // permanently. First command fine, every one after a redirect
            // dead, which is why this read as an intermittent pty bug.
            //
            // fd 2 is untouched by this path and is the same /dev/console the
            // kernel pre-opened on 0/1/2, so duplicating it back is exactly
            // the state fd 1 was in before. This is the idiom run_pipe()
            // already uses for its own restore; not a second convention.
            dup2(2, 1);
        }

        static char capbuf[65536];   // captured stdout when redirecting to a file
        int caplen = 0;

        if (pid > 0) {
            int status = 0;
            sys_waitpid(pid, &status, 0);

            if (use_pipe && pipefd[0] >= 0) {
                char buf[512];
                int n;
                while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
                    if (redir_out) {
                        for (int i = 0; i < n && caplen < (int)sizeof(capbuf); i++)
                            capbuf[caplen++] = buf[i];
                    } else {
                        buf[n] = '\0';
                        term_puts(buf);
                    }
                }
            }
        } else {
            term_puts("\033[31mFailed to run: ");
            term_puts(path);
            term_puts("\033[0m\n");
        }

        if (pipefd[0] >= 0) close(pipefd[0]);  // done reading the captured output

        // Write the captured output to the redirection target.
        if (redir_out && pid > 0) {
            if (!redir_append) sys_unlink(redir_out);     // truncate: drop old file
            int outfd = open(redir_out, 0x41);            // O_WRONLY | O_CREAT
            if (outfd >= 0) {
                if (redir_append) sys_seek(outfd, 0, 2);  // SEEK_END
                if (caplen > 0) sys_write(outfd, capbuf, caplen);
                close(outfd);
            } else {
                term_puts("\033[31mcannot open: ");
                term_puts(redir_out);
                term_puts("\033[0m\n");
            }
        }
    }
}
// Add command to history
void add_to_history(const char *cmd) {
    if (!*cmd) return;  // Don't add empty commands

    // Don't add duplicates
    if (history_count > 0) {
        int last = (history_count - 1) % TERM_SHELL_HISTORY_SIZE;
        if (str_eq(history[last], cmd)) return;
    }

    int idx = history_count % TERM_SHELL_HISTORY_SIZE;
    int i = 0;
    while (cmd[i] && i < TERM_SHELL_INPUT_MAX - 1) {
        history[idx][i] = cmd[i];
        i++;
    }
    history[idx][i] = '\0';
    history_count++;
}
