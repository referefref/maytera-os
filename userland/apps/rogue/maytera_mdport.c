
/*
 * maytera_mdport.c - MayteraOS platform layer for Rogue
 * Replaces the original mdport.c.
 * All signatures match extern.h exactly.
 */

#include "../../libc/syscall.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "compat/stdio.h"
#include "compat/curses.h"

void md_init(void) { /* nothing */ }

void md_onsignal_default(void)   { }
void md_onsignal_exit(void)      { }
void md_onsignal_autosave(void)  { }
void md_ignoreallsignals(void)   { }
void md_normaluser(void)         { }
void md_tstpsignal(void)         { }
void md_tstphold(void)           { }
void md_tstpresume(void)         { }

int md_hasclreol(void)  { return 1; }
int md_getpid(void)     { return 1; }
int md_getuid(void)     { return 1000; }

/* Return int (matching extern.h prototypes) */
int md_erasechar(void)  { return '\b'; }
int md_killchar(void)   { return 'U' & 0x1F; }  /* Ctrl-U */
int md_suspchar(void)   { return 'Z' & 0x1F; }
int md_dsuspchar(void)  { return 'Y' & 0x1F; }
int md_setdsuspchar(int c) { (void)c; return 0; }
int md_setsuspchar(int c)  { (void)c; return 0; }

char *md_getusername(void) { return "player"; }
char *md_getrealname(int uid) { (void)uid; return "Player"; }
char *md_gethomedir(void)  { return "/SAVES"; }
char *md_getshell(void)    { return "/bin/sh"; }

/* md_getpass: returns char* per extern.h */
char *md_getpass(char *prompt) {
    static char buf[64];
    mvprintw(LINES - 1, 0, "%s", prompt);
    refresh();
    wgetstr(stdscr, buf);
    return buf;
}

void md_sleep(int s) { syscall1(SYS_SLEEP, (long)((unsigned int)s * 1000u)); }

int  md_unlink(char *path) { (void)path; return 0; }
int  md_unlink_open_file(char *path, FILE *inf) { (void)path; (void)inf; return 0; }

int  md_issymlink(char *path) { (void)path; return 0; }

char *md_crypt(char *pw, char *salt) { (void)salt; return pw; }

int  md_shellescape(void) { return 0; }

int  md_loadav(double *av) { *av = 0.0; return 0; }

void md_raw_standout(void) { standout(); }
void md_raw_standend(void) { standend(); }

/* md_readchar - called by io.c to get a key */
int md_readchar(void) { return getch(); }

/* md_putchar - called for direct char output */
int md_putchar(int c) { addch((unsigned long)c); return c; }

void md_start_checkout_timer(int s) { (void)s; }
void md_stop_checkout_timer(void)   { }

int md_chmod(char *path, int mode) { (void)path; (void)mode; return 0; }
