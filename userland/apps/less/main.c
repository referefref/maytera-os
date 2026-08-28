// less - a file pager
// Usage: less [FILE...]
// Keys: SPACE/f/PageDown next page, b/PageUp previous page, j/Down/Enter next
//       line, k/Up previous line, g/Home top, G/End end, /RE forward search,
//       ?RE backward search, n/N repeat, q quit.
//
// ============================================================================
// A PAGER READING FROM A PIPE MUST NOT READ ITS KEYS FROM stdin
// ============================================================================
//
// REPORTED: `ls | less` printed the first screenful and returned straight to
// the prompt; no key would page it. `less FILE` worked. REPRODUCED on golden
// 2057 (VM <vmid>) before changing anything, and the cause is visible in the
// code that used to be here:
//
//   * with no file operands the content was slurped from fd 0, and
//   * every key was read with getchar(), which ALSO reads fd 0.
//
// So in a pipeline the pager's "keyboard" was the pipe. It had already been
// drained to EOF by the slurp, so the very first getchar() returned -1, the
// loop broke on `c < 0` and the process exited after exactly one page. The
// symptom was not a missing key binding; it was the pager reading its commands
// out of its own input data.
//
// The conventional fix is that CONTENT comes from stdin while KEYS come from
// the CONTROLLING TERMINAL, opened by name. Two file descriptors, two
// purposes. That name is /dev/tty, and it did not exist in this OS: nothing
// resolved to "whichever terminal the calling process is attached to", because
// no process recorded which terminal that was. It exists now (process_t.ctty +
// devtty_open() in kernel/drivers/pty.c), and this file is its first consumer.
//
// WHAT IS DELIBERATELY NOT ASSUMED. /dev/tty can legitimately fail to open: a
// process with no controlling terminal has none, and that is not an error to
// paper over. When no terminal can be reached AT ALL this program writes the
// whole input out and exits 0, which is what less(1) does when its output is
// not a terminal. Printing one page and vanishing, the old behaviour, is the
// one response that is never right.
//
// ============================================================================
// THE NAVIGATION KEYS WERE NOT "PARTLY" WIRED; THEY WERE ABSENT
// ============================================================================
//
// The owner also reported that arrows, PageUp/PageDown, Home and End did
// nothing. That was true in the FILE case too, and it was not a mapping bug:
// the command switch tested only single printable bytes, so an arrow key
// (ESC [ A) arrived as three bytes that matched no case at all and were
// silently dropped one at a time. Every one of those keys is decoded below.
//
// HOW THE SEQUENCES ARE READ, and why there is no timeout. kernel/drivers/tty.c
// tty_read() ignores VMIN/VTIME entirely: in non-canonical mode it blocks for
// the first byte and then hands back everything queued, up to `count`. The
// Terminal writes a whole escape sequence to the pty master in ONE write
// (userland/apps/terminal/term_pty.c key_event_to_bytes returns 3 or 4 bytes,
// written together), so one read() of a small buffer receives the complete
// sequence. That is why a bare ESC can be told apart from an arrow key without
// the usual VTIME dance, and why relying on it is safe HERE and would not be
// safe against an arbitrary tty driver. Both halves were read out of the
// kernel, not assumed.
//
// ============================================================================
// EARLIER WORK KEPT INTACT
// ============================================================================
//
// #745 (local 108): `/pattern` is a real POSIX regex via the shared musl
// libregex.a (a fourth CONSUMER, not a fourth copy), and the read/line index
// come from mtool.c so there is no silent 64 KB / 8192-line cap. #745 (local
// 99): cbreak mode, deliberately NOT cfmakeraw(), because clearing OPOST/ONLCR
// staircases every page. Both are preserved.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "errno.h"
#include "termios.h"   // #745 (local 99): cbreak mode for single-key paging
#include "sys/ioctl.h" // TIOCGWINSZ: the page size is the WINDOW, not a guess
#include "mtool.h"

#include <regex.h>

// Only the last-resort default. The real value comes from the terminal's own
// row count (see size_page()). The old build hardcoded 23 unconditionally, so
// on the maximized 1280x800 Terminal used for verification a "page" was 23 of
// the ~57 visible rows and two thirds of the window sat empty.
#define PAGE_LINES_FALLBACK 23

static char   *buf;
static size_t  buf_len;
static size_t *line_off;
static size_t  line_count;

// ---------------------------------------------------------------------------
// The terminal. g_key is where COMMANDS are read from; g_out is where the
// PAGER UI is drawn. They are usually the same fd and need not be: `ls | less`
// under a shell that gives the pipeline a pty has a pipe on fd 0 and the tty on
// fd 1, and `less f > out` has the reverse.
// ---------------------------------------------------------------------------
static int g_key  = -1;   // fd for keystrokes, -1 = no terminal reachable
static int g_out  =  1;   // fd for the pager's own output
static int g_tty_opened = -1;  // fd we opened ourselves and must close
static int g_page = PAGE_LINES_FALLBACK;

static void print_line(size_t idx)
{
	if (idx >= line_count) return;
	size_t start = line_off[idx];
	size_t end = (idx + 1 < line_count) ? line_off[idx + 1] : buf_len;
	mtool_wall(g_out, buf + start, end - start);
	if (end > start && buf[end - 1] != '\n') mtool_wall(g_out, "\n", 1);
}

static void print_page(size_t start_line)
{
	for (int i = 0; i < g_page && start_line + (size_t)i < line_count; i++)
		print_line(start_line + (size_t)i);
}

static void show_status(size_t cur_line, const char *note)
{
	mtool_wfmt(g_out, "\033[7m -- less: line %lu/%lu%s%s (SPACE/PgDn=more b/PgUp=back /=search q=quit) --\033[0m",
	           (unsigned long)(cur_line + 1), (unsigned long)line_count,
	           note && *note ? " " : "", note ? note : "");
}

// One repaint for every movement key. The old code had three different repaint
// idioms (a bare page, a clear-then-page, and a single appended line for `j`),
// which is how `j` came to scroll by leaving the previous page above it. A
// full-screen pager repaints the screen; there is one way to do it here.
static void redraw(size_t cur_line, const char *note)
{
	mtool_wall(g_out, "\033[2J\033[H", 7);
	print_page(cur_line);
	show_status(cur_line, note);
}

// One line as a NUL-terminated C string, for regexec. Lines can be any length,
// so this grows a single scratch buffer rather than capping at 1024 the way
// the tools this ticket replaced did.
static char  *linebuf;
static size_t linecap;

static const char *line_cstr(size_t idx)
{
	if (idx >= line_count) return "";
	size_t start = line_off[idx];
	size_t end = (idx + 1 < line_count) ? line_off[idx + 1] : buf_len;
	if (end > start && buf[end - 1] == '\n') end--;
	size_t n = end - start;
	if (n + 1 > linecap) {
		size_t nc = linecap ? linecap : 256;
		while (nc < n + 1) nc *= 2;
		char *nb = (char *)realloc(linebuf, nc);
		if (!nb) return "";
		linebuf = nb;
		linecap = nc;
	}
	memcpy(linebuf, buf + start, n);
	linebuf[n] = '\0';
	return linebuf;
}

static regex_t g_re;
static int     g_have_re = 0;
static int     g_re_dir = 1;      // +1 forward, -1 backward

// Returns the matching line index, or (size_t)-1.
static size_t search_from(size_t from, int dir)
{
	if (!g_have_re) return (size_t)-1;
	if (dir > 0) {
		for (size_t i = from; i < line_count; i++)
			if (regexec(&g_re, line_cstr(i), 0, NULL, 0) == 0) return i;
	} else {
		for (size_t i = from + 1; i > 0; i--)
			if (regexec(&g_re, line_cstr(i - 1), 0, NULL, 0) == 0) return i - 1;
	}
	return (size_t)-1;
}

// ---------------------------------------------------------------------------
// Key input, read from g_key rather than from stdin.
//
// A tiny pushback buffer holds whatever one read() returned beyond the byte
// being consumed, so a 3-byte arrow sequence costs one read() and not three.
// ---------------------------------------------------------------------------
static unsigned char g_kbuf[32];
static int g_klen = 0, g_kpos = 0;

// Raw next byte, or -1 on EOF/error. Blocks.
static int key_raw(void)
{
	if (g_kpos < g_klen) return g_kbuf[g_kpos++];
	if (g_key < 0) return -1;
	for (;;) {
		long n = read(g_key, g_kbuf, sizeof g_kbuf);
		if (n > 0) { g_klen = (int)n; g_kpos = 1; return g_kbuf[0]; }
		if (n == 0) return -1;                 // hangup
		if (errno == EINTR) continue;          // ^C handled by the signal
		return -1;
	}
}

// A byte that is ALREADY buffered, or -1. Never blocks and never reads. This
// is what makes a lone ESC unambiguous: see the header note on tty_read().
static int key_buffered(void)
{
	return (g_kpos < g_klen) ? g_kbuf[g_kpos++] : -1;
}

// Logical keys. Values above 255 cannot collide with a byte.
enum {
	K_UP = 256, K_DOWN, K_LEFT, K_RIGHT,
	K_PGUP, K_PGDN, K_HOME, K_END, K_UNKNOWN
};

static int key_get(void)
{
	int c = key_raw();
	if (c != 0x1b) return c;

	// ESC with nothing behind it is the ESC key itself.
	int c1 = key_buffered();
	if (c1 < 0) return 0x1b;

	// Both the CSI form (ESC [ ...) and the SS3 form (ESC O ...) that some
	// terminals send for arrows/Home/End in application-cursor mode.
	if (c1 != '[' && c1 != 'O') return K_UNKNOWN;

	int c2 = key_buffered();
	if (c2 < 0) return K_UNKNOWN;

	switch (c2) {
		case 'A': return K_UP;
		case 'B': return K_DOWN;
		case 'C': return K_RIGHT;
		case 'D': return K_LEFT;
		case 'H': return K_HOME;   // xterm's Home
		case 'F': return K_END;    // xterm's End
		default: break;
	}

	// vt220 numeric form: ESC [ <digits> ~. The mapping is the same one
	// userland/apps/terminal/term_pty.c EMITS and userland/apps/vi's
	// decode_csi() consumes: 1/7 Home, 4/8 End, 5 PageUp, 6 PageDown.
	if (c2 >= '0' && c2 <= '9') {
		int num = c2 - '0';
		for (;;) {
			int d = key_buffered();
			if (d < 0) return K_UNKNOWN;          // truncated
			if (d == '~') break;
			if (d < '0' || d > '9') return K_UNKNOWN;
			num = num * 10 + (d - '0');
			if (num > 99) return K_UNKNOWN;
		}
		switch (num) {
			case 1: case 7: return K_HOME;
			case 4: case 8: return K_END;
			case 5:         return K_PGUP;
			case 6:         return K_PGDN;
			default:        return K_UNKNOWN;
		}
	}
	return K_UNKNOWN;
}

static int read_pattern(int prompt, char *out, int cap)
{
	mtool_wfmt(g_out, "\r\033[K%c", prompt);
	int pi = 0;
	for (;;) {
		int sc = key_get();
		if (sc < 0) return -1;
		if (sc == '\n' || sc == '\r') break;
		if (sc == '\b' || sc == 127) {
			if (pi > 0) { pi--; mtool_wall(g_out, "\b \b", 3); }
			continue;
		}
		if (sc >= ' ' && sc < 127 && pi < cap - 1) {
			out[pi++] = (char)sc;
			char ch = (char)sc;
			mtool_wall(g_out, &ch, 1);
		}
	}
	out[pi] = '\0';
	return pi;
}

// ---------------------------------------------------------------------------
// Find the terminal. Order matters and is deliberate.
// ---------------------------------------------------------------------------
static void find_terminal(void)
{
	// 1. stdin, when it IS a terminal. This is the `less FILE` case, which
	//    already worked; keeping it on exactly the same path means this
	//    change cannot regress it.
	if (isatty(0)) {
		g_key = 0;
	} else {
		// 2. The controlling terminal by name. This is the whole point: our
		//    stdin is a pipe, so the keyboard has to be found some other way.
		int fd = open("/dev/tty", O_RDWR);
		if (fd >= 0) { g_key = fd; g_tty_opened = fd; }
		// 3. stdout, if it happens to be a terminal and /dev/tty was not
		//    available. A pty slave here is readable as well as writable, so
		//    this is a real fallback rather than a hopeful one, and it keeps
		//    the pager working on a kernel without /dev/tty.
		else if (isatty(1)) g_key = 1;
	}

	// Output goes to the terminal, wherever that is: stdout when stdout is the
	// terminal, otherwise the tty we just found. `ls | less > file` therefore
	// still paints its UI on the screen and still writes nothing to the file
	// but content, rather than spraying escape codes into it.
	if (isatty(1))        g_out = 1;
	else if (g_key >= 0)  g_out = g_key;
	else                  g_out = 1;
}

static void size_page(void)
{
	struct winsize ws;
	int fd = (g_out >= 0) ? g_out : g_key;
	if (fd >= 0 && ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2)
		g_page = (int)ws.ws_row - 1;   // one row reserved for the status line
	else
		g_page = PAGE_LINES_FALLBACK;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);

	// Operands: concatenate every named file, exactly as less(1) does when it
	// is given several. The old one paged argv[1] and dropped the rest.
	buf = NULL;
	buf_len = 0;
	int nfiles = argc - 1;
	if (nfiles <= 0) {
		int fd = 0;
		buf = mtool_slurp_fd(fd, &buf_len);
		if (!buf) return MTOOL_EX_FAIL;
	} else {
		for (int i = 1; i < argc; i++) {
			int fd = mtool_open_read(argv[i]);
			if (fd < 0) return MTOOL_EX_FAIL;
			size_t n = 0;
			char *b = mtool_slurp_fd(fd, &n);
			mtool_close_read(fd);
			if (!b) return MTOOL_EX_FAIL;
			char *nb = (char *)realloc(buf, buf_len + n + 1);
			if (!nb) { mtool_warn("out of memory"); return MTOOL_EX_FAIL; }
			buf = nb;
			memcpy(buf + buf_len, b, n);
			buf_len += n;
			buf[buf_len] = '\0';
			free(b);
		}
	}

	if (buf_len == 0) return MTOOL_EX_OK;

	find_terminal();
	size_page();

	// NO TERMINAL AT ALL (`ls | less | wc -l`, or a process with no ctty).
	// Behave as cat. The content must still all come out: the defect being
	// fixed truncated the user's data at one screenful, and a pager that
	// silently drops the tail of a pipeline is worse than no pager.
	if (g_key < 0) {
		mtool_wall(1, buf, buf_len);
		return MTOOL_EX_OK;
	}

	line_off = mtool_index_lines(buf, buf_len, &line_count);
	if (!line_off) return MTOOL_EX_FAIL;

	if (line_count <= (size_t)g_page) {
		mtool_wall(g_out, buf, buf_len);
		if (g_tty_opened >= 0) close(g_tty_opened);
		return MTOOL_EX_OK;
	}

	// #745 (local 99): CBREAK MODE. A pager whose commands are single letters
	// must not be left in the tty's CANONICAL mode, where the line discipline
	// holds every byte until Enter: SPACE and q did nothing until the user also
	// pressed Return.
	//
	// Deliberately NOT cfmakeraw(): that clears OPOST/ONLCR, and every page
	// this app prints ends its lines with a bare '\n'. Without ONLCR those
	// render as line-feed with no carriage return, i.e. staircased text. Only
	// ICANON and ECHO have to go. ISIG stays on so ^C still reaches this
	// process through the foreground process group.
	//
	// Applied to g_key, which is the fd the line discipline actually governs
	// for our reads. Under a pipe that is NOT fd 0, and setting it on fd 0
	// would have configured the pipe (a no-op) while leaving the terminal
	// canonical.
	struct termios less_saved, less_cbreak;
	int less_raw = 0;
	if (tcgetattr(g_key, &less_saved) == 0) {
		less_cbreak = less_saved;
		less_cbreak.c_lflag &= ~(ICANON | ECHO);
		less_cbreak.c_cc[VMIN]  = 1;
		less_cbreak.c_cc[VTIME] = 0;
		if (tcsetattr(g_key, TCSANOW, &less_cbreak) == 0) less_raw = 1;
	}

	size_t cur_line = 0;
	redraw(cur_line, "");

	for (;;) {
		int c = key_get();
		if (c < 0 || c == 'q' || c == 'Q') { mtool_wall(g_out, "\r\033[K", 4); break; }

		if (c == ' ' || c == 'f' || c == 6 /*^F*/ || c == K_PGDN) {
			cur_line += (size_t)g_page;
			if (cur_line >= line_count) cur_line = line_count - 1;
			redraw(cur_line, "");
		} else if (c == 'b' || c == 2 /*^B*/ || c == K_PGUP) {
			cur_line = (cur_line > (size_t)g_page) ? cur_line - (size_t)g_page : 0;
			redraw(cur_line, "");
		} else if (c == '\n' || c == '\r' || c == 'j' || c == K_DOWN) {
			if (cur_line < line_count - 1) { cur_line++; redraw(cur_line, ""); }
		} else if (c == 'k' || c == K_UP) {
			if (cur_line > 0) { cur_line--; redraw(cur_line, ""); }
		} else if (c == 'g' || c == K_HOME) {
			cur_line = 0;
			redraw(cur_line, "");
		} else if (c == 'G' || c == K_END) {
			cur_line = line_count > (size_t)g_page ? line_count - (size_t)g_page : 0;
			redraw(cur_line, "");
		} else if (c == '/' || c == '?') {
			char pattern[256];
			if (read_pattern(c, pattern, sizeof pattern) < 0) break;
			if (pattern[0]) {
				if (g_have_re) { regfree(&g_re); g_have_re = 0; }
				int rc = regcomp(&g_re, pattern, REG_NEWLINE);
				if (rc != 0) {
					// A BAD PATTERN IS AN ERROR, not "no matches". The old
					// pager could not tell the difference because it had no
					// pattern language at all.
					char msg[256], note[300];
					regerror(rc, &g_re, msg, sizeof msg);
					// OUR OWN prefix, then the engine's words. The prefix is
					// what a test can assert on: the message text belongs to
					// whichever regex implementation is linked, and asserting
					// on that would be asserting that glibc and musl word
					// their errors identically.
					snprintf(note, sizeof note, "[bad pattern: %s]", msg);
					redraw(cur_line, note);
					continue;
				}
				g_have_re = 1;
				g_re_dir = (c == '/') ? 1 : -1;
			}
			size_t hit = (g_re_dir > 0)
			           ? search_from(cur_line + 1, 1)
			           : search_from(cur_line ? cur_line - 1 : 0, -1);
			if (hit == (size_t)-1) {
				redraw(cur_line, "[pattern not found]");
			} else {
				cur_line = hit;
				redraw(cur_line, "");
			}
		} else if (c == 'n' || c == 'N') {
			int dir = (c == 'n') ? g_re_dir : -g_re_dir;
			size_t hit = (dir > 0)
			           ? search_from(cur_line + 1, 1)
			           : search_from(cur_line ? cur_line - 1 : 0, -1);
			if (hit == (size_t)-1) {
				redraw(cur_line, g_have_re ? "[pattern not found]"
				                           : "[no previous pattern]");
			} else {
				cur_line = hit;
				redraw(cur_line, "");
			}
		}
	}

	// Hand the terminal back the way we found it. A pager that exits leaving
	// ICANON/ECHO off returns the user to a shell that neither echoes nor
	// line-edits, which looks exactly like a hung terminal.
	if (less_raw) tcsetattr(g_key, TCSANOW, &less_saved);
	if (g_tty_opened >= 0) close(g_tty_opened);
	return MTOOL_EX_OK;
}
