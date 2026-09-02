// drivers/testinput.c - Deterministic host->guest synthetic input channel (#334)
//
// A DEBUG-GATED serial (COM1) command channel that injects keyboard and mouse
// events DIRECTLY into the same in-kernel paths the PS/2 IRQ handlers feed, so
// the compositor and apps see injected input identically to real hardware
// input, while BYPASSING QEMU's PS/2 model. QEMU's 8042/PS-2 path intermittently
// drops rapidly injected `qm sendkey` / QMP input-send-event scancodes (the 8042
// output buffer is one byte deep and coalesces), which blocked automated GUI
// verification (see docs/GUI_TEST_INPUT.md).
//
// TRANSPORT: COM1. The host writes newline-terminated ASCII commands to the VM
// serial socket and reads back "OK ..."/"ERR ..." ACK lines on the same socket.
// Serial is reachable ONLY by whoever controls the hypervisor serial chardev
// (same trust level as the QMP monitor); it is NOT a network surface, so this
// does NOT reintroduce the removed RC-2323 remote-input hole (#566). It is also
// GATED OFF by default: the channel only arms when /TESTINPUT.TXT is present on
// the FAT ESP, so a shipping golden (no marker) never enables it and gains zero
// attack surface.
//
// #426 (no hand-rolled busy-wait): the host is an EXTERNAL wake source and the
// 16550 in this QEMU config does not reliably raise IRQ4 for sub-FIFO-trigger
// writes, so the worker uses the SANCTIONED bounded timed wait
// (wait_event_timeout) on a short cadence and drains the UART RX register when
// it wakes. This is the "wake source is outside our control + a timeout is the
// correct semantics" case the discipline explicitly allows, NOT a hand-rolled
// proc_sleep/proc_yield spin. Latency is one wait quantum (~12ms), which is far
// below any GUI settle time. Nothing else consumes COM1 RX in GUI mode, so the
// worker is the sole reader (no contention, no double-drain race).
//
// INJECTION POINTS (both pre-existing kernel functions, reused not forked):
//   keyboard: keyboard_process_scancode()  (cpu/isr.c) - byte-identical to IRQ1
//   mouse:    mouse_inject_button()         (drivers/mouse.c) - absolute click
//             the compositor replays through its normal SYS_GET_MOUSE poll,
//             routing to the correct window/focus exactly like a real click.

#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../sync/waitq.h"
#include "../fs/fat.h"
#include "../mm/heap.h"
#include "keyboard.h"
#include "testinput.h"

// ---- externs into the real input paths (reused, not forked) ----------------
extern fat_fs_t g_fat_fs;
extern void keyboard_process_scancode(uint8_t scancode);        // cpu/isr.c
extern int  keyboard_ascii_to_scancode(char c, int *need_shift);// cpu/isr.c (new)
extern int  keyboard_buffer_depth(void);                        // cpu/isr.c (new)
extern volatile uint64_t g_kbd_consumed;                        // cpu/isr.c (new)
extern volatile uint64_t g_kbd_irq_scancodes;                   // cpu/isr.c (new)
extern void mouse_inject_button(int32_t x, int32_t y, int down);// drivers/mouse.c
extern void mouse_inject_button_mask(int32_t x, int32_t y, uint32_t mask); // drivers/mouse.c (#speedcap)
extern void mouse_inject_move(int32_t x, int32_t y);            // drivers/mouse.c (Win16 SkiFree repro)
extern void mouse_get_position(int32_t *x, int32_t *y);         // drivers/mouse.c
extern volatile uint64_t g_mouse_poll_count;                    // gui/fb_syscall.c (new)
// #resizelag: kernel-side cost of user_window_handle_resize() (kmalloc +
// background fill + memcpy of the content buffer), proc/syscall.c.
extern uint64_t g_uwresize_calls;
extern uint64_t g_uwresize_total_us;
extern uint64_t g_uwresize_max_us;
extern int32_t  g_uwresize_last_cw;
extern int32_t  g_uwresize_last_ch;
// #resizelag: real outer window bounds for slot `idx` (proc/syscall.c),
// so a host-side drag probe can compute a resize-grip point exactly
// instead of guessing off a screenshot.
extern int testinput_win_query(int idx, int32_t *x, int32_t *y, int32_t *w,
                                int32_t *h, char *title, int titlesz);
extern int  proc_create(const char *name, void (*entry)(void *), void *arg, int prio);
extern void proc_sleep(uint32_t ms);

#define KEY_LSHIFT_MAKE  0x2A
#define KEY_LSHIFT_BREAK 0xAA

static wait_queue_head_t ti_wq;             // bounded-wait queue for the worker
static volatile int      ti_enabled = 0;
static int32_t           ti_last_x = 0, ti_last_y = 0;   // last injected pointer

// ---- #197: click delivery ledger + latched click --------------------------
//
// THE BUG THIS REPLACES. CLICK used to be: set the physical button level, sleep
// a FIXED 40ms, clear it. That is not an event, it is a PULSE, and the
// compositor does not receive it - it SAMPLES the level once per frame in
// process_input() and computes the edge itself. If no frame boundary falls
// inside the pulse the compositor sees 0 before and 0 after, no edge exists,
// and the click is gone. Nothing detected that: the "OK CLICK x y" ACK was
// emitted unconditionally, so it described the PARSER, not delivery. A frame
// exceeding 40ms is not exotic, it is an ordinary full-screen repaint, which is
// why the loss rate tracked compositor load and read as "flaky".
//
// THE FIX. Hold the level until the ledger says the compositor sampled the
// edge, then move to the next edge. The wait is #426 BEST tier, not a timeout
// workaround: there are TWO independent wake sources, so no wake can be lost.
// sys_get_mouse() calls testinput_click_edge() on every observed edge, AND
// wait_event_timeout re-checks the counter on its own cadence; the CONDITION is
// a monotonic counter, so a missed wake costs one quantum and can never hang.
// The bound exists only so a dead compositor reports a failure instead of
// parking the channel forever - and that failure is exactly what the harness
// needs to be able to go RED.
//
// WHY NOT ROUTE THROUGH sys_inject_mouse() LIKE THE VNC PATH (#188/#440/#443).
// Because that path has a KNOWN DOUBLE-DELIVERY SHAPE. vnc_inject_pointer()
// (userland/apps/compositor/vnc.c) calls sys_inject_mouse(DOWN) AND pokes the
// physical level via set_mouse_buttons(); the compositor then samples that same
// level on its next frame and relays a SECOND DOWN. An app under the cursor
// gets two EVENT_MOUSE_DOWNs for one click, which is why #188 needed a
// workaround. Injecting the physical level ONLY - what this channel already did
// and still does - is the shape a real PS/2 or USB mouse has, and it delivers
// exactly once. The layer was right; the TIMING was wrong.
extern void     clickacct_note_inject_rs(int down);
extern uint64_t clickacct_get_rs(uint32_t which);
#define CA_INJ_DOWN 0u
#define CA_INJ_UP   1u
#define CA_SMP_DOWN 2u
#define CA_SMP_UP   3u
#define CA_ROUTED   4u
#define CA_HIT      5u
#define CA_POLLS    6u
#define CA_SMP_RDOWN 7u
#define CA_SMP_RUP   8u

static wait_queue_head_t ti_click_wq;         // woken on every sampled edge
static volatile int      ti_click_wq_ready = 0;
static int               ti_click_mode = 1;   // 1 = latched (#197), 0 = legacy pulse
static uint32_t          ti_click_ms   = 1500;// bound on one edge, ms

void testinput_click_edge(int edge) {
    (void)edge;
    if (!ti_click_wq_ready) return;
    wake_up_all(&ti_click_wq);
}

// Drive ONE button edge and return 1 if the compositor sampled it, 0 if it did
// not within the bound. In legacy pulse mode (ti_click_mode == 0) the edge is
// injected and the caller does the historical fixed sleep; the return is then
// the ledger's after-the-fact verdict, not a synchronisation.
static int ti_click_edge(int32_t x, int32_t y, int down) {
    uint32_t which  = down ? CA_SMP_DOWN : CA_SMP_UP;
    uint64_t before = clickacct_get_rs(which);
    mouse_inject_button(x, y, down);
    if (!ti_click_mode) return -1;            // pulse mode: no latch
    int rc = wait_event_timeout(&ti_click_wq,
                                clickacct_get_rs(which) != before,
                                wq_ms_to_ticks(ti_click_ms));
    return (rc == WAIT_OK) ? 1 : 0;
}

// (#speedcap) THE RIGHT-BUTTON EDGE, on the SAME latch as the left one.
//
// WHY THIS HAD TO EXIST AT ALL. Every per-window menu in this desktop opens on
// a RIGHT click: the taskbar tile popup (taskbar.c tbmenu), the XFCE dock's
// CTX_MODE_DOCK menu (contextmenu.c), and therefore the ONLY route to the #778
// DOS Speed dialog. This channel could inject only a left click, so no such
// menu had ever been opened under test, and #778 shipped with its own CHANGELOG
// entry admitting it had never been booted. A feature whose only entry point is
// unreachable by the test harness is a feature that ships unverified.
//
// It is NOT a fixed pulse, for the identical reason the left path is not (#197):
// the compositor SAMPLES the physical level once per frame and computes the edge
// itself, so a pulse shorter than a frame is silently lost and the ACK would
// describe the parser rather than delivery. The wait is #426-acceptable for the
// same reason ti_click_edge()'s is: two independent wake sources (the sampled
// edge wakes the queue, and wait_event_timeout re-checks on its own cadence) over
// a MONOTONIC counter, with the bound present only so a dead compositor reports
// a failure instead of parking the channel.
static int ti_rclick_edge(int32_t x, int32_t y, int down) {
    uint32_t which  = down ? CA_SMP_RDOWN : CA_SMP_RUP;
    uint64_t before = clickacct_get_rs(which);
    mouse_inject_button_mask(x, y, down ? 2u : 0u);
    if (!ti_click_mode) return -1;            // pulse mode: no latch
    int rc = wait_event_timeout(&ti_click_wq,
                                clickacct_get_rs(which) != before,
                                wq_ms_to_ticks(ti_click_ms));
    return (rc == WAIT_OK) ? 1 : 0;
}

// One complete RIGHT click. Same 2-bit return as ti_click_once().
static int ti_rclick_once(int32_t x, int32_t y) {
    int m = 0;
    if (ti_rclick_edge(x, y, 1) == 1) m |= 1;
    if (ti_rclick_edge(x, y, 0) == 1) m |= 2;
    return m;
}

// One complete click. Returns a 2-bit mask: bit0 = press edge observed,
// bit1 = release edge observed. 3 means the compositor saw a whole click.
static int ti_click_once(int32_t x, int32_t y) {
    int m = 0;
    if (ti_click_mode) {
        if (ti_click_edge(x, y, 1) == 1) m |= 1;
        if (ti_click_edge(x, y, 0) == 1) m |= 2;
    } else {
        // LEGACY (RED) ARM, preserved verbatim so the old behaviour can be
        // reproduced back-to-back against the fix under identical load.
        uint64_t d0 = clickacct_get_rs(CA_SMP_DOWN);
        uint64_t u0 = clickacct_get_rs(CA_SMP_UP);
        mouse_inject_button(x, y, 1);
        proc_sleep(40);                       // >=1 compositor frame between edges
        mouse_inject_button(x, y, 0);
        proc_sleep(40);                       // settle, so the ledger read is fair
        if (clickacct_get_rs(CA_SMP_DOWN) != d0) m |= 1;
        if (clickacct_get_rs(CA_SMP_UP)   != u0) m |= 2;
    }
    return m;
}

// ---- reply helpers ---------------------------------------------------------
// Emit an ACK line ATOMICALLY. Serial output kernel-wide is unlocked, so the
// per-2s [HB] heartbeat (and other kprintf) can otherwise interleave at
// character granularity and split an "OK ..." line, breaking the host parser.
// Disabling interrupts across the write prevents THIS thread from being
// preempted mid-line, so every reply is contiguous (a concurrent [HB] line may
// get split instead, which the host harness simply ignores). Replies are short
// (<64 bytes, a few ms at 115200) and the host is draining TX, so the bounded
// serial_write spin returns immediately; holding IRQs off this briefly is fine
// for a debug-gated channel.
static void ti_reply(const char *s) {
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    serial_puts(COM1, s);
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

// ---- small parsers ---------------------------------------------------------
static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

static int parse_dec(const char **pp) {
    const char *p = skip_ws(*pp);
    int neg = 0; if (*p == '-') { neg = 1; p++; }
    int v = 0, got = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; got = 1; }
    *pp = p;
    if (!got) return -1000000;
    return neg ? -v : v;
}

static int parse_hex(const char **pp) {
    const char *p = skip_ws(*pp);
    int v = 0, got = 0;
    for (;;) {
        char c = *p; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + d; p++; got = 1;
    }
    *pp = p;
    return got ? v : -1;
}

// Inject one printable ASCII char as a make+break pair (with a bracketing
// shift make/break when the glyph needs it), exactly as the PS/2 IRQ would.
static int ti_type_char(char c) {
    int need_shift = 0;
    int sc = keyboard_ascii_to_scancode(c, &need_shift);
    if (sc < 0) return -1;
    if (need_shift) keyboard_process_scancode(KEY_LSHIFT_MAKE);
    keyboard_process_scancode((uint8_t)sc);
    keyboard_process_scancode((uint8_t)(sc | 0x80));
    if (need_shift) keyboard_process_scancode(KEY_LSHIFT_BREAK);
    return 0;
}

// ---- flat key=value file editing (#711) ------------------------------------
// SETKV <path> <key> <value>  /  GETKV <path> <key>
//
// The whole point of #711 is that the UI is driven by DATA on the image, so the
// acceptance test has to EDIT A FILE ON THE BOOTED SYSTEM and see the UI change
// with nothing rebuilt. The kernel's own serial shell is not reachable once
// desktop_run() has taken over (it never returns), and GUI mouse automation is
// unreliable here (#334), so this debug-gated channel - which already exists
// precisely to drive a booted system deterministically from the host - gets a
// generic flat-file key=value editor.
//
// It is GENERIC, not theme-specific: every config file in this OS is flat
// key=value, so this is the natural companion to that decision. It is armed by
// the same /TESTINPUT.TXT marker as the rest of the channel, so a shipping
// golden (no marker) has no editor and no added surface.
//
// Writes go through fat_write_file(), which routes "/" paths to the ext2 root
// (fs/fat.c), so /THEMES/*.mtheme is edited where it actually lives.
#define TI_KV_MAX  8192

static int ti_copy_tok(const char **pp, char *out, int cap) {
    const char *p = skip_ws(*pp);
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && n < cap - 1) out[n++] = *p++;
    out[n] = 0;
    *pp = p;
    return n;
}

// Rewrite (or append) "<key>=<value>" in a flat key=value file. Returns 0 on
// success, negative on failure. Bounded: refuses a file over TI_KV_MAX.
static int ti_setkv(const char *path, const char *key, const char *value) {
    uint32_t sz = 0;
    char *in = (char *)fat_read_file(&g_fat_fs, path, &sz);
    if (!in) return -1;
    if (sz > TI_KV_MAX) { kfree(in); return -2; }

    int klen = 0; while (key[klen]) klen++;
    int vlen = 0; while (value[vlen]) vlen++;

    char *out = (char *)kmalloc(sz + (uint32_t)klen + (uint32_t)vlen + 4);
    if (!out) { kfree(in); return -3; }

    uint32_t i = 0, o = 0;
    int replaced = 0;
    while (i < sz) {
        uint32_t ls = i;
        while (i < sz && in[i] != '\n') i++;
        uint32_t le = i;                     // exclusive, excludes the '\n'
        if (i < sz) i++;                     // step over '\n'
        uint32_t ll = le - ls;
        if (ll > 0 && in[ls + ll - 1] == '\r') ll--;

        int match = (!replaced && ll > (uint32_t)klen && in[ls + klen] == '=' &&
                     strncmp(in + ls, key, (size_t)klen) == 0);
        if (match) {
            for (int k = 0; k < klen; k++) out[o++] = key[k];
            out[o++] = '=';
            for (int k = 0; k < vlen; k++) out[o++] = value[k];
            out[o++] = '\n';
            replaced = 1;
        } else {
            for (uint32_t k = 0; k < ll; k++) out[o++] = in[ls + k];
            out[o++] = '\n';
        }
    }
    if (!replaced) {
        for (int k = 0; k < klen; k++) out[o++] = key[k];
        out[o++] = '=';
        for (int k = 0; k < vlen; k++) out[o++] = value[k];
        out[o++] = '\n';
    }
    int rc = fat_write_file(&g_fat_fs, path, out, o);
    kfree(in);
    kfree(out);
    return (rc < 0) ? -4 : (int)o;
}

// Read one key back out of a flat key=value file, into out (NUL-terminated).
static int ti_getkv(const char *path, const char *key, char *out, int cap) {
    uint32_t sz = 0;
    char *in = (char *)fat_read_file(&g_fat_fs, path, &sz);
    if (!in) return -1;
    int klen = 0; while (key[klen]) klen++;
    uint32_t i = 0;
    int found = -1;
    while (i < sz) {
        uint32_t ls = i;
        while (i < sz && in[i] != '\n') i++;
        uint32_t le = i;
        if (i < sz) i++;
        uint32_t ll = le - ls;
        if (ll > 0 && in[ls + ll - 1] == '\r') ll--;
        if (ll > (uint32_t)klen && in[ls + klen] == '=' &&
            strncmp(in + ls, key, (size_t)klen) == 0) {
            uint32_t vs = ls + (uint32_t)klen + 1;
            int n = 0;
            while (vs < ls + ll && n < cap - 1) out[n++] = in[vs++];
            out[n] = 0;
            found = n;
            break;
        }
    }
    kfree(in);
    return found;
}

// ---- command dispatch ------------------------------------------------------
static void ti_process_line(const char *line) {
    // #197: 256, not 128. The CLICKS ledger reply is 58 bytes of fixed text
    // plus SEVEN unsigned 64-bit counters. snprintf truncates safely, but a
    // truncated ACK loses its trailing newline and jams the host line parser -
    // which is exactly the silent-failure class this ticket exists to remove.
    char buf[256];
    const char *p = skip_ws(line);
    if (*p == 0) return;

    if (!strncmp(p, "PING", 4)) {
        ti_reply("OK PING\n");
        return;
    }
    if (!strncmp(p, "KBQ", 3)) {
        snprintf(buf, sizeof(buf), "OK KBQ depth=%d consumed=%lu irq=%lu\n",
                 keyboard_buffer_depth(), (unsigned long)g_kbd_consumed,
                 (unsigned long)g_kbd_irq_scancodes);
        ti_reply(buf);
        return;
    }
    // #162: read back the AUTHORITATIVE volume state, so a test can assert the
    // exact level and mute flag rather than inferring them from pixels. Same
    // packed word SYS_VOL_STATE returns (rustkern/sysvol.rs), so this reports
    // what the compositor and the tray slider see, not a second opinion.
    // Debug-gated with the rest of this file: no /TESTINPUT.TXT, no worker, no
    // command surface at all on a shipping golden.
    if (!strncmp(p, "VOL", 3)) {
        extern int sysvol_get_rs(void);
        extern int sysvol_muted_rs(void);
        extern uint64_t sysvol_state_rs(void);
        uint64_t st = sysvol_state_rs();
        snprintf(buf, sizeof(buf),
                 "OK VOL level=%d muted=%d seq=%u keyseq=%u\n",
                 sysvol_get_rs(), sysvol_muted_rs(),
                 (unsigned)((st >> 16) & 0xFFFF), (unsigned)((st >> 32) & 0xFFFF));
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "MPOLL", 5)) {
        snprintf(buf, sizeof(buf), "OK MPOLL polls=%lu\n",
                 (unsigned long)g_mouse_poll_count);
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "KEY", 3)) {                 // KEY <scancode-hex>
        p += 3; int sc = parse_hex(&p);
        if (sc < 0 || sc > 0xFF) { ti_reply("ERR KEY badarg\n"); return; }
        keyboard_process_scancode((uint8_t)sc);
        snprintf(buf, sizeof(buf), "OK KEY %02x depth=%d\n", sc, keyboard_buffer_depth());
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "TYPE ", 5)) {               // TYPE <text...> (verbatim rest)
        p += 5;
        int n = 0, bad = 0;
        while (*p) { if (ti_type_char(*p) == 0) n++; else bad++; p++; }
        snprintf(buf, sizeof(buf), "OK TYPE typed=%d skipped=%d depth=%d\n",
                 n, bad, keyboard_buffer_depth());
        ti_reply(buf);
        return;
    }
    // #197: CLICKS - read the whole click ledger. The ONLY honest answer to
    // "did that click land"; the OK line below is a parser receipt, these are
    // kernel-side monotonic counters that HAVE to advance (blame.md's rule from
    // the wedged-guest false alarm, where an absent ACK was mistaken for a dead
    // guest while the guest was consuming every event).
    if (!strncmp(p, "CLICKS", 6)) {
        snprintf(buf, sizeof(buf),
                 "OK CLICKS inj_d=%lu inj_u=%lu smp_d=%lu smp_u=%lu "
                 "rsmp_d=%lu rsmp_u=%lu routed=%lu hit=%lu polls=%lu\n",
                 (unsigned long)clickacct_get_rs(CA_INJ_DOWN),
                 (unsigned long)clickacct_get_rs(CA_INJ_UP),
                 (unsigned long)clickacct_get_rs(CA_SMP_DOWN),
                 (unsigned long)clickacct_get_rs(CA_SMP_UP),
                 (unsigned long)clickacct_get_rs(CA_SMP_RDOWN),
                 (unsigned long)clickacct_get_rs(CA_SMP_RUP),
                 (unsigned long)clickacct_get_rs(CA_ROUTED),
                 (unsigned long)clickacct_get_rs(CA_HIT),
                 (unsigned long)clickacct_get_rs(CA_POLLS));
        ti_reply(buf);
        return;
    }
    // CLICKMODE <0|1> [ms] - 1 = latched (#197 default), 0 = the pre-#197 fixed
    // 40ms pulse. Kept so the broken arm can be re-run on the SAME binary under
    // the SAME load as the fix, which is a stronger comparison than two builds.
    if (!strncmp(p, "CLICKMODE", 9)) {
        p += 9; int m = parse_dec(&p); int ms = parse_dec(&p);
        if (m == 0 || m == 1) ti_click_mode = m;
        if (ms > 0 && ms <= 30000) ti_click_ms = (uint32_t)ms;
        snprintf(buf, sizeof(buf), "OK CLICKMODE %s ms=%u\n",
                 ti_click_mode ? "latch" : "pulse", (unsigned)ti_click_ms);
        ti_reply(buf);
        return;
    }
    // CLICKN <n> <x> <y> [gapms] - the RELIABILITY SELF-TEST. Sends n clicks at
    // one point and reports how many the compositor actually SAMPLED, straight
    // from the ledger. It can fail: on the pulse arm it under-counts, and the
    // host asserts sampled == n.
    if (!strncmp(p, "CLICKN", 6)) {
        p += 6; int n = parse_dec(&p); int x = parse_dec(&p); int y = parse_dec(&p);
        int gap = parse_dec(&p);
        if (n <= 0 || n > 500 || x <= -1000000 || y <= -1000000) {
            ti_reply("ERR CLICKN badarg\n"); return;
        }
        if (gap <= -1000000 || gap < 0) gap = 0;
        if (gap > 2000) gap = 2000;
        uint64_t d0 = clickacct_get_rs(CA_SMP_DOWN);
        uint64_t u0 = clickacct_get_rs(CA_SMP_UP);
        uint64_t r0 = clickacct_get_rs(CA_ROUTED);
        uint64_t h0 = clickacct_get_rs(CA_HIT);
        ti_last_x = x; ti_last_y = y;
        int full = 0;
        for (int i = 0; i < n; i++) {
            if (ti_click_once(x, y) == 3) full++;
            if (gap) proc_sleep((uint32_t)gap);
        }
        snprintf(buf, sizeof(buf),
                 "OK CLICKN n=%d mode=%s full=%d smp_d=%lu smp_u=%lu routed=%lu hit=%lu\n",
                 n, ti_click_mode ? "latch" : "pulse", full,
                 (unsigned long)(clickacct_get_rs(CA_SMP_DOWN) - d0),
                 (unsigned long)(clickacct_get_rs(CA_SMP_UP)   - u0),
                 (unsigned long)(clickacct_get_rs(CA_ROUTED)   - r0),
                 (unsigned long)(clickacct_get_rs(CA_HIT)      - h0));
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "CLICK", 5)) {               // CLICK <x> <y>  (down+up)
        p += 5; int x = parse_dec(&p); int y = parse_dec(&p);
        if (x <= -1000000 || y <= -1000000) { ti_reply("ERR CLICK badarg\n"); return; }
        uint64_t r0 = clickacct_get_rs(CA_ROUTED), h0 = clickacct_get_rs(CA_HIT);
        ti_last_x = x; ti_last_y = y;
        int m = ti_click_once(x, y);
        snprintf(buf, sizeof(buf),
                 "OK CLICK %d %d mode=%s down=%d up=%d routed=%lu hit=%lu polls=%lu\n",
                 x, y, ti_click_mode ? "latch" : "pulse", (m & 1) ? 1 : 0, (m & 2) ? 1 : 0,
                 (unsigned long)(clickacct_get_rs(CA_ROUTED) - r0),
                 (unsigned long)(clickacct_get_rs(CA_HIT) - h0),
                 (unsigned long)g_mouse_poll_count);
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "RCLICK", 6)) {              // RCLICK <x> <y>  (right down+up)
        p += 6; int x = parse_dec(&p); int y = parse_dec(&p);
        if (x <= -1000000 || y <= -1000000) { ti_reply("ERR RCLICK badarg\n"); return; }
        ti_last_x = x; ti_last_y = y;
        int m = ti_rclick_once(x, y);
        // No routed=/hit= here, and that absence is deliberate rather than an
        // omission: clickacct's ROUTED/HIT legs count sys_inject_mouse relays of
        // a LEFT down only (see clickacct.rs), and a right click opens compositor
        // chrome instead of being relayed to a window, so those two counters
        // would be a constant 0 and would read as a failure.
        snprintf(buf, sizeof(buf),
                 "OK RCLICK %d %d mode=%s down=%d up=%d rsmp_d=%lu rsmp_u=%lu polls=%lu\n",
                 x, y, ti_click_mode ? "latch" : "pulse",
                 (m & 1) ? 1 : 0, (m & 2) ? 1 : 0,
                 (unsigned long)clickacct_get_rs(CA_SMP_RDOWN),
                 (unsigned long)clickacct_get_rs(CA_SMP_RUP),
                 (unsigned long)g_mouse_poll_count);
        ti_reply(buf);
        return;
    }
    // (#speedcap) WRITEF <path> <text...> - replace a whole small file's CONTENT.
    //
    // SETKV above edits ONE key= line in a flat key=value file, which is every
    // config file in this OS except the ones that are a single bare token. The
    // DOS speed cap's <program dir>/SPEED.CFG is exactly that shape (one decimal,
    // or "off"), so SETKV cannot write it: it would append "cycles=500" to a file
    // dos_cycles_parse() reads from byte 0. Without this verb the #778 LIVE
    // re-poll could not be exercised from the host at all, because the golden
    // ships no sshd and the only other writer is the compositor dialog under
    // test, so the mechanism and its UI could never be separated.
    //
    // Same fat_write_file() route SETKV uses (routes "/" to the ext2 root), same
    // /TESTINPUT.TXT arming, same TI_KV_MAX bound. Unlike SETKV it MAY create the
    // file, which is the point: SPEED.CFG is absent for most titles.
    if (!strncmp(p, "WRITEF ", 7)) {
        p += 7;
        char path[96];
        if (ti_copy_tok(&p, path, sizeof(path)) <= 0) { ti_reply("ERR WRITEF badarg\n"); return; }
        const char *body = skip_ws(p);
        int n = 0; while (body[n] && body[n] != '\r' && body[n] != '\n') n++;
        if (n > 512) { ti_reply("ERR WRITEF toolong\n"); return; }
        int rc = fat_write_file(&g_fat_fs, path, body, (uint32_t)n);
        if (rc < 0) snprintf(buf, sizeof(buf), "ERR WRITEF rc=%d\n", rc);
        else        snprintf(buf, sizeof(buf), "OK WRITEF %s bytes=%d\n", path, n);
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "MDOWN", 5)) {               // MDOWN <x> <y>
        p += 5; int x = parse_dec(&p); int y = parse_dec(&p);
        if (x <= -1000000 || y <= -1000000) { ti_reply("ERR MDOWN badarg\n"); return; }
        ti_last_x = x; ti_last_y = y;
        mouse_inject_button(x, y, 1);
        snprintf(buf, sizeof(buf), "OK MDOWN %d %d\n", x, y);
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "MOVE", 4)) {                // MOVE <x> <y>  (position only, no click)
        p += 4; int x = parse_dec(&p); int y = parse_dec(&p);
        if (x <= -1000000 || y <= -1000000) { ti_reply("ERR MOVE badarg\n"); return; }
        ti_last_x = x; ti_last_y = y;
        mouse_inject_move(x, y);
        snprintf(buf, sizeof(buf), "OK MOVE %d %d\n", x, y);
        ti_reply(buf);
        return;
    }
    // #resizelag: read the resize-cost ledger. RSTAT resets nothing (so a
    // test can poll mid-drag); RSTATZ reads then zeroes it (so a test can
    // isolate exactly one drag from a clean baseline).
    if (!strncmp(p, "RSTATZ", 6) || !strncmp(p, "RSTAT", 5)) {
        int zero = !strncmp(p, "RSTATZ", 6);
        snprintf(buf, sizeof(buf),
                 "OK RSTAT calls=%lu total_us=%lu max_us=%lu avg_us=%lu last=%dx%d\n",
                 (unsigned long)g_uwresize_calls,
                 (unsigned long)g_uwresize_total_us,
                 (unsigned long)g_uwresize_max_us,
                 (unsigned long)(g_uwresize_calls ? g_uwresize_total_us / g_uwresize_calls : 0),
                 g_uwresize_last_cw, g_uwresize_last_ch);
        ti_reply(buf);
        if (zero) {
            g_uwresize_calls = 0; g_uwresize_total_us = 0; g_uwresize_max_us = 0;
        }
        return;
    }
    // #resizelag: WINQ <idx> - outer bounds of user_windows[idx], for
    // computing an accurate resize-grip click point from the host.
    if (!strncmp(p, "WINQ", 4)) {
        p += 4;
        int idx = parse_dec(&p);
        if (idx <= -1000000) idx = 0;
        int32_t wx = 0, wy = 0, ww = 0, wh = 0;
        char title[32];
        int ok = testinput_win_query(idx, &wx, &wy, &ww, &wh, title, sizeof(title));
        if (!ok) { ti_reply("ERR WINQ noWindow\n"); return; }
        snprintf(buf, sizeof(buf), "OK WINQ %d x=%d y=%d w=%d h=%d title=%s\n",
                 idx, wx, wy, ww, wh, title);
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "MUP", 3)) {                 // MUP  (release at last point)
        mouse_inject_button(ti_last_x, ti_last_y, 0);
        snprintf(buf, sizeof(buf), "OK MUP %d %d\n", ti_last_x, ti_last_y);
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "SETKV ", 6)) {              // SETKV <path> <key> <value>
        p += 6;
        char path[96], key[64], val[64];
        if (ti_copy_tok(&p, path, sizeof(path)) <= 0 ||
            ti_copy_tok(&p, key, sizeof(key)) <= 0 ||
            ti_copy_tok(&p, val, sizeof(val)) <= 0) {
            ti_reply("ERR SETKV badarg\n");
            return;
        }
        int rc = ti_setkv(path, key, val);
        if (rc < 0) snprintf(buf, sizeof(buf), "ERR SETKV rc=%d\n", rc);
        else        snprintf(buf, sizeof(buf), "OK SETKV %s %s=%s bytes=%d\n", path, key, val, rc);
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "GETKV ", 6)) {              // GETKV <path> <key>
        p += 6;
        char path[96], key[64], val[80];
        if (ti_copy_tok(&p, path, sizeof(path)) <= 0 ||
            ti_copy_tok(&p, key, sizeof(key)) <= 0) {
            ti_reply("ERR GETKV badarg\n");
            return;
        }
        int rc = ti_getkv(path, key, val, sizeof(val));
        if (rc < 0) snprintf(buf, sizeof(buf), "ERR GETKV missing\n");
        else        snprintf(buf, sizeof(buf), "OK GETKV %s=%s\n", key, val);
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "METRIC ", 7)) {             // METRIC <id>  (live theme table)
        p += 7;
        int id = parse_dec(&p);
        extern int32_t theme_get_metric_by_id(int theme_id, int metric_id);
        snprintf(buf, sizeof(buf), "OK METRIC %d=%d\n", id,
                 (int)theme_get_metric_by_id(-1, id));
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "AUTOSCALE ", 10)) {   // AUTOSCALE <w> <h> <laptop>
        // Ask the SHIPPED KERNEL what auto-detection would decide for a given
        // framebuffer geometry. This exists because the display the answer
        // matters most for - the owner's 3840x2160 laptop panel - cannot be
        // reproduced in QEMU at all: OVMF's mode table tops out at 2560x1600,
        // so -device VGA,xres=3840 silently falls back. Without this, the 4K
        // answer could only be shown by a host-compiled copy of the same source,
        // which is evidence about the SOURCE, not about the binary that ships.
        p += 10;
        int w = parse_dec(&p); while (*p == ' ') p++;
        int h = parse_dec(&p); while (*p == ' ') p++;
        int lap = 0;
        if (*p == '-') { p++; lap = -parse_dec(&p); } else { lap = parse_dec(&p); }
        extern int32_t uiscale_auto_pct_rs(int32_t, int32_t, int32_t);
        extern int32_t uiscale_max_pct_rs(int32_t, int32_t);
        snprintf(buf, sizeof(buf), "OK AUTOSCALE %dx%d laptop=%d auto=%d max=%d\n",
                 w, h, lap, (int)uiscale_auto_pct_rs(w, h, lap),
                 (int)uiscale_max_pct_rs(w, h));
        ti_reply(buf);
        return;
    }
    if (!strncmp(p, "UISCALE", 7)) {        // UISCALE  -> the LIVE state
        extern int32_t uiscale_pct_rs(void);
        extern int32_t uiscale_src_rs(void);
        extern int32_t uiscale_auto_pct(void);
        extern int32_t uiscale_max_pct(void);
        extern int32_t uiscale_is_laptop(void);
        extern uint32_t fb_get_width(void);
        extern uint32_t fb_get_height(void);
        snprintf(buf, sizeof(buf),
                 "OK UISCALE pct=%d src=%d auto=%d max=%d laptop=%d fb=%ux%u\n",
                 (int)uiscale_pct_rs(), (int)uiscale_src_rs(),
                 (int)uiscale_auto_pct(), (int)uiscale_max_pct(),
                 (int)uiscale_is_laptop(), fb_get_width(), fb_get_height());
        ti_reply(buf);
        return;
    }
    ti_reply("ERR unknown\n");
}

// ---- worker: bounded-wait, drain the UART, assemble lines, dispatch --------
static void ti_worker(void *arg) {
    (void)arg;
    static char line[256];
    int len = 0;
    ti_reply("\nREADY TESTINPUT #334\n");
    for (;;) {
        // Sanctioned bounded wait (#426): sleep ~3 ticks, then poll the UART.
        // The condition is intentionally the constant false 'ti_enabled==2'
        // (never true) so we always take the timeout path; wait_event_timeout
        // yields the CPU for the interval rather than busy-spinning.
        (void)wait_event_timeout(&ti_wq, ti_enabled == 2, 3);
        while (serial_received(COM1)) {
            uint8_t b = inb(COM1 + SERIAL_DATA);
            if (b == '\r') continue;
            if (b == '\n') { line[len] = 0; ti_process_line(line); len = 0; }
            else if (len < (int)sizeof(line) - 1) line[len++] = (char)b;
            else { len = 0; ti_reply("ERR overflow\n"); }
        }
    }
}

// ---- arm (only when /TESTINPUT.TXT present on the ESP) ---------------------
void testinput_init(void) {
    // #197: the click wait queue is armed UNCONDITIONALLY, before the marker
    // gate. sys_get_mouse() calls testinput_click_edge() on every sampled edge
    // and must never touch an uninitialised queue; with no marker there is no
    // worker and nothing ever waits, so this costs one zeroed struct and adds
    // no command surface to a shipping golden.
    wait_queue_head_init(&ti_click_wq);
    ti_click_wq_ready = 1;

    if (!g_fat_fs.mounted || !fat_exists(&g_fat_fs, "/TESTINPUT.TXT")) {
        // Default OFF: a shipping golden has no marker, so no worker exists and
        // COM1 RX is not consumed. Zero added surface.
        return;
    }
    wait_queue_head_init(&ti_wq);
    ti_enabled = 1;
    proc_create("testinput", ti_worker, NULL, 2 /*PRIO_NORMAL*/);
    kprintf("[TESTINPUT] serial input-injection channel ENABLED (#334)\n");
}
