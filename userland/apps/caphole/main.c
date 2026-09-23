// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// caphole - artefact-level demonstration of the five ungated cross-process /
// cross-principal holes that docs/SYSTEM_CAPABILITY_API.md Stage 0 closes.
//
// WHY IT EXISTS. Section 1 of that document MEASURED these holes by reading
// code, and section 14 is explicit that "the Stage 0 defects generally are read
// from code, not demonstrated on a VM ... one grade weaker than a run". This is
// the run. Every probe here has a RED arm: it CREATES the abusive state and
// reports the hole OPEN if the kernel permits it, before any fix is in. On an
// unpatched kernel every probe reports OPEN; on a Stage-0 kernel every probe
// reports CLOSED. That asymmetry, on the SAME binary, is the evidence.
//
// A GUARD THAT NEVER FIRES AND AN ABSENT GUARD LOOK IDENTICAL (blame.md, many
// times). So the cross-process probes (D3, D4) do the reach from a SEPARATE
// process with a distinct pid AND tgid: the PARENT creates and OWNS the
// resource, then spawns a CHILD that reaches across for it by raw index. On an
// unpatched kernel the reach SUCCEEDS; on a Stage-0 kernel capgate refuses it.
// The child reports its finding to the parent as its EXIT CODE (uid 1000 cannot
// write coordination files into "/", so files are not usable here) and also
// prints a human-readable line to the serial console, which every process can
// write regardless of uid.
//
// Defects, keyed to docs/SYSTEM_CAPABILITY_API.md section 1:
//   D1  1.2  SYS_INJECT_KEY (197) ungated: a non-compositor posts a synthetic
//            key to the focused window (the elevation-credit forgery primitive).
//   D2  1.2  SYS_GET_KEYBOARD (195) ungated: a non-compositor drains the global
//            cooked key ring (the password-readback primitive).
//   D3  1.4  legacy TCP raw-index family: tear down ANOTHER process's socket by
//            passing its 0..63 table index.
//   D4  1.7  sys_shm_map: map ANOTHER process's SHM region by its 0..63 index
//            and read its contents.
//   D5  1.5  /dev/ node open bypassed perms_check entirely (fdlayer ordering).
//
// Modes (argv):
//   caphole                    run the whole suite (the AUTORUN.CFG entry point)
//   caphole atk_tcp <idx>      child: try to tcp_close a socket it does NOT own
//   caphole atk_shm <id>       child: try to shm_map a region it does NOT own
// The child exit codes are RES_OPEN / RES_CLOSED / RES_INCONC below.
//
// Verification aid, not a shipped app: listed in build/unshipped-apps.list.
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "syscall.h"
#include "fcntl.h"
#include "sys/wait.h"

// Child exit codes -> parent verdict. Small, distinct, non-overlapping with the
// 0/1 an ordinary program uses, so a crash (which exits differently) is not read
// as a verdict.
#define RES_OPEN    10   // the kernel PERMITTED the cross-process abuse
#define RES_CLOSED  11   // the kernel REFUSED it (Stage-0 fix in force)
#define RES_INCONC  12   // could not set up the test

#define SHM_SECRET_MAGIC 0xC0DEFACEu
#define CHILD_MARKER     0xBADC0DEu   // what a foreign mapper stamps to PROVE the write landed

// ---------------------------------------------------------------------------
// Verdict tally.
// ---------------------------------------------------------------------------
static int g_open = 0, g_probes = 0, g_inconc = 0;

static void verdict(const char *id, const char *name, int state, const char *detail) {
    // state: RES_OPEN / RES_CLOSED / RES_INCONC
    g_probes++;
    const char *w;
    if (state == RES_OPEN)   { g_open++;  w = "OPEN"; }
    else if (state == RES_CLOSED) { w = "CLOSED"; }
    else { g_inconc++; w = "INCONC"; }
    printf("[CAPHOLE] %-4s %-7s %s  (%s)\n", id, w, name, detail ? detail : "");
}

// ===========================================================================
// CHILD ATTACKERS (distinct pid + tgid from the creator)
// ===========================================================================

// atk_tcp: the socket at index `idx` is owned by our PARENT, not us. Try to
// tear it down. On an unpatched kernel tcp_close succeeds (0); on a Stage-0
// kernel tcp_close_kcr3 refuses because tcp_sock_owned_by_caller() is false.
static int mode_atk_tcp(int idx) {
    int st = tcp_get_state(idx);
    int cl = tcp_close(idx);
    printf("[CAPHOLE-ATCP] foreign sock idx=%d: tcp_get_state=%d tcp_close=%d (my uid=%d)\n",
           idx, st, cl, getuid());
    // close returning 0 == we tore down a socket we do not own == OPEN.
    if (cl == 0) return RES_OPEN;
    return RES_CLOSED;
}

// atk_shm: the region `id` is owned by our PARENT. Try to map it and read the
// first word. On an unpatched kernel the map succeeds and we read the parent's
// secret; on a Stage-0 kernel sys_shm_map refuses (creator-only).
static int mode_atk_shm(int id) {
    void *addr = 0;
    int mr = shm_map(id, &addr);
    if (mr == 0 && addr) {
        volatile unsigned int *w = (volatile unsigned int *)addr;
        unsigned int seen = w[0];
        w[0] = CHILD_MARKER;   // stamp it: the parent watches word[0] as ground truth
        printf("[CAPHOLE-ASHM] mapped foreign region %d, read 0x%08x%s, stamped 0x%08x (my uid=%d)\n",
               id, seen, seen == SHM_SECRET_MAGIC ? " == parent's SECRET" : "", CHILD_MARKER, getuid());
        return RES_OPEN;
    }
    printf("[CAPHOLE-ASHM] shm_map(foreign id=%d) refused rc=%d (my uid=%d)\n",
           id, mr, getuid());
    return RES_CLOSED;
}

// Spawn one attacker child with a single integer argument. Fire and forget: the
// parent does NOT read the child's exit code (a separate spawned process is not
// reliably reaped by this parent here), it observes GROUND TRUTH instead - the
// child's effect on the resource the parent still holds. Returns the child pid,
// or <=0 on spawn failure.
static int spawn_attacker(const char *mode, int arg) {
    char abuf[16];
    { int v = arg, m = 0; char tt[12]; int neg = v < 0; if (neg) v = -v;
      if (v==0) tt[m++]='0'; while (v>0){tt[m++]=(char)('0'+v%10);v/=10;}
      int n=0; if (neg) abuf[n++]='-'; while (m>0) abuf[n++]=tt[--m]; abuf[n]=0; }
    char *argv[3]; argv[0] = "/APPS/CAPHOLE"; argv[1] = (char *)mode; argv[2] = abuf;
    return sys_spawn_args("/APPS/CAPHOLE", argv, 3);
}

// ===========================================================================
// PARENT PROBES
// ===========================================================================

static void probe_inject_key(void) {
    // D1: non-compositor posting a synthetic key. Unpatched: returns 0 (event
    // dispatched). Stage 0: capgate refuses, returns -1.
    int r = sys_inject_key('A');
    char d[80];
    if (r == 0) {
        snprintf(d, sizeof(d), "sys_inject_key returned 0 from a NON-compositor: key dispatched");
        verdict("D1", "injkey", RES_OPEN, d);
    } else {
        snprintf(d, sizeof(d), "sys_inject_key refused (rc=%d)", r);
        verdict("D1", "injkey", RES_CLOSED, d);
    }
}

static void probe_get_keyboard(void) {
    // D2: non-compositor draining the global key ring. Stage 0 gate returns -13
    // (EACCES) BEFORE the ring; unpatched returns -1 (empty) or a keycode,
    // either of which means the body ran and the ring was reachable.
    int r = sys_get_keyboard();
    char d[96];
    if (r == -13) {
        snprintf(d, sizeof(d), "sys_get_keyboard refused (rc=-13 EACCES) before the ring");
        verdict("D2", "getkbd", RES_CLOSED, d);
    } else {
        snprintf(d, sizeof(d), "reached the key ring from a NON-compositor (rc=%d: %s)",
                 r, r >= 0 ? "STOLE a buffered keystroke" : "ring empty but body ran");
        verdict("D2", "getkbd", RES_OPEN, d);
    }
}

static void probe_tcp_hijack(void) {
    // D3: we create and OWN a socket, then a foreign child (distinct pid+tgid)
    // tries to close it by raw index. GROUND TRUTH: our own socket. If a foreign
    // process can tear it down, tcp_get_state() on it flips from CLOSED(0) to
    // the inactive-slot answer (-1). We poll our own socket rather than trust the
    // child's report.
    int s = tcp_socket();
    if (s < 0) { verdict("D3", "tcphij", RES_INCONC, "could not create a socket to defend"); return; }
    // Make it a LISTENER (state LISTEN=1). tcp_get_state() returns CLOSED(0) for
    // BOTH an active-CLOSED socket and a freed slot, so a never-connected socket
    // gives no observable change when torn down. A listener's state is 1, and a
    // successful foreign close frees the slot -> tcp_get_state() reads 0. That is
    // the ground truth the parent watches. We own s, so our own listen is allowed.
    tcp_listen(s, 40010, 4);
    int st_before = tcp_get_state(s);   // LISTEN (1) if the listen took
    int child = spawn_attacker("atk_tcp", s);
    if (child <= 0) { verdict("D3", "tcphij", RES_INCONC, "could not spawn attacker child"); tcp_close(s); return; }
    int waited = 0, destroyed = 0;
    while (waited < 4000) {
        int st = tcp_get_state(s);
        if (st != st_before) { destroyed = 1; break; }
        sys_sleep(100); waited += 100;
    }
    int st_after = tcp_get_state(s);
    char d[128];
    if (destroyed) {
        snprintf(d, sizeof(d), "a foreign process tore down our socket (state %d -> %d)", st_before, st_after);
        verdict("D3", "tcphij", RES_OPEN, d);
    } else {
        snprintf(d, sizeof(d), "foreign close refused; our socket survived (state stayed %d)", st_before);
        verdict("D3", "tcphij", RES_CLOSED, d);
    }
    tcp_close(s);
}

static void probe_shm_map(void) {
    // D4: we create and OWN a region, stamp a secret, then a foreign child tries
    // to map and read it.
    int id = shm_create(4096, SHM_FLAG_NONE);
    if (id < 0) { verdict("D4", "shmmap", RES_INCONC, "could not create a region"); return; }
    void *addr = 0;
    if (shm_map(id, &addr) != 0 || !addr) {
        verdict("D4", "shmmap", RES_INCONC, "could not map our own region");
        shm_destroy(id); return;
    }
    volatile unsigned int *p = (volatile unsigned int *)addr;
    for (int i = 0; i < 16; i++) p[i] = SHM_SECRET_MAGIC;
    // GROUND TRUTH: our own mapping. If a foreign process can map this region it
    // will overwrite word[0] with CHILD_MARKER (SHM is shared physical memory),
    // and we will SEE that in our own mapping. We watch our memory rather than
    // trust the child's report.
    int child = spawn_attacker("atk_shm", id);
    char d[112];
    if (child <= 0) {
        verdict("D4", "shmmap", RES_INCONC, "could not spawn attacker child");
    } else {
        int waited = 0, stamped = 0;
        while (waited < 4000) {
            if (p[0] == CHILD_MARKER) { stamped = 1; break; }
            sys_sleep(100); waited += 100;
        }
        if (stamped) {
            snprintf(d, sizeof(d), "a foreign process mapped our region and wrote into it (word0=0x%08x)", p[0]);
            verdict("D4", "shmmap", RES_OPEN, d);
        } else {
            snprintf(d, sizeof(d), "foreign map refused: creator-only enforced (word0 still 0x%08x)", p[0]);
            verdict("D4", "shmmap", RES_CLOSED, d);
        }
    }
    shm_unmap(id);
    shm_destroy(id);
}

static void probe_dev_bypass(void) {
    // D5: the /dev/ prefix used to be handled BEFORE perms_check, so every
    // device node was exempt from the permission model. This test image's
    // /CONFIG/PERMS.DB carries an OPERATOR policy row "/DEV/TTYACM0:0:0:0600"
    // (root-only) - exactly the row the fdlayer.c comment says "got exactly
    // nothing" before Stage 0. We are uid 1000 (a desktop session is non-root),
    // so:
    //   unpatched: /dev handled first, perms_check BYPASSED, the 0600 policy is
    //              ignored -> dev_open("ttyACM0") (no device attached) -> -1
    //   Stage 0  : perms_check runs first, ENFORCES the 0600 policy for a
    //              non-root W_OK -> -13 (EACCES), before dev_open is reached
    // -13 is only reachable once perms_check is actually consulted for /dev.
    // DIAGNOSTIC control: /CONFIG/SHADOW is 0600 root-only. If perms_check is
    // consulted for a non-root caller at all, this read is refused. This tells us
    // whether the app is genuinely non-root and whether perms_check runs here.
    int ctl = open("/CONFIG/SHADOW", O_RDONLY);
    printf("[CAPHOLE] D5 control: uid=%d open(/CONFIG/SHADOW,O_RDONLY)=%d (expect <0 if non-root perms enforced)\n",
           getuid(), ctl);
    if (ctl >= 0) close(ctl);
    // sys_open() AND NOT open(). THIS IS THE WHOLE TEST.
    //
    // libc's open() (stdlib.c:991) does `if (r < 0) { errno = -r; return -1; }`,
    // so the kernel's -13 (EACCES) is collapsed to -1 and is INDISTINGUISHABLE
    // from the "no such device" -1 a pre-fix kernel returns. Using open() here
    // made this probe report OPEN on a kernel where the fix provably works: the
    // GREEN run emitted `[PERMS-DENY] ... want=-w- path=/dev/ttyACM0` and
    // counted `dev=1` refusal in the [CAPGATE] ledger, both impossible pre-fix,
    // while this line still printed -1.
    //
    // Any probe whose verdict depends on WHICH error the kernel returned must
    // call the raw syscall wrapper. The convenience wrapper is lossy by design.
    int r = sys_open("/dev/ttyACM0", O_WRONLY);
    if (r >= 0) close(r);
    char d[112];
    if (r == -13) {
        snprintf(d, sizeof(d), "perms_check consulted for /dev (non-root WRITE -> -13 EACCES)");
        verdict("D5", "devperm", RES_CLOSED, d);
    } else {
        snprintf(d, sizeof(d),
                 "/dev open bypassed perms_check (rc=%d; expected -13 once consulted)", r);
        verdict("D5", "devperm", RES_OPEN, d);
    }
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "atk_tcp") == 0) return mode_atk_tcp(atoi(argv[2]));
    if (argc >= 3 && strcmp(argv[1], "atk_shm") == 0) return mode_atk_shm(atoi(argv[2]));

    printf("\n[CAPHOLE] ===== Stage 0 cross-process/principal hole demonstration =====\n");
    printf("[CAPHOLE] my uid=%d. OPEN == defect reachable, CLOSED == Stage-0 fix in force.\n",
           getuid());

    probe_inject_key();     // D1
    probe_get_keyboard();   // D2
    probe_tcp_hijack();     // D3
    probe_shm_map();        // D4
    probe_dev_bypass();     // D5

    printf("[CAPHOLE] ----------------------------------------------------------\n");
    printf("[CAPHOLE] SUITE: %d probes, %d OPEN, %d inconclusive\n", g_probes, g_open, g_inconc);
    if (g_open == 0 && g_inconc == 0)
        printf("[CAPHOLE] RESULT: GREEN - every Stage-0 hole is CLOSED\n");
    else if (g_open > 0)
        printf("[CAPHOLE] RESULT: RED - %d Stage-0 hole(s) OPEN\n", g_open);
    else
        printf("[CAPHOLE] RESULT: INCONCLUSIVE - re-run\n");
    return 0;
}
