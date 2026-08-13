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
extern void mouse_inject_move(int32_t x, int32_t y);            // drivers/mouse.c (Win16 SkiFree repro)
extern void mouse_get_position(int32_t *x, int32_t *y);         // drivers/mouse.c
extern volatile uint64_t g_mouse_poll_count;                    // gui/fb_syscall.c (new)
extern int  proc_create(const char *name, void (*entry)(void *), void *arg, int prio);
extern void proc_sleep(uint32_t ms);

#define KEY_LSHIFT_MAKE  0x2A
#define KEY_LSHIFT_BREAK 0xAA

static wait_queue_head_t ti_wq;             // bounded-wait queue for the worker
static volatile int      ti_enabled = 0;
static int32_t           ti_last_x = 0, ti_last_y = 0;   // last injected pointer

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
    char buf[128];
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
    if (!strncmp(p, "CLICK", 5)) {               // CLICK <x> <y>  (down+up)
        p += 5; int x = parse_dec(&p); int y = parse_dec(&p);
        if (x <= -1000000 || y <= -1000000) { ti_reply("ERR CLICK badarg\n"); return; }
        ti_last_x = x; ti_last_y = y;
        mouse_inject_button(x, y, 1);
        proc_sleep(40);                          // >=1 compositor frame between edges
        mouse_inject_button(x, y, 0);
        snprintf(buf, sizeof(buf), "OK CLICK %d %d polls=%lu\n",
                 x, y, (unsigned long)g_mouse_poll_count);
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
