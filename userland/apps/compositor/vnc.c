// vnc.c - Userland RFB (VNC) server for the MayteraOS compositor (#440).
//
// GOAL: view + control the live desktop over a real TCP socket (not the SSHD
// text shell, which caps a session at ~4KB - see remote-screenshot #439/golden
// the build server:<GOLDEN_PATH>). This reuses the same g_fb
// backbuffer access as screenshot.c, plus the #379 damage-rectangle list, plus
// the existing mouse/keyboard injection syscalls already used by the physical
// PS/2 path in main.c (sys_inject_mouse / sys_inject_key / set_mouse_pos).
//
// CONNECTION DIRECTION (read this before changing the connect logic):
// The current userland syscall surface has SYS_SOCKET/SYS_CONNECT/SYS_SEND/
// SYS_RECV/SYS_TCP_CLOSE/SYS_TCP_STATE only (see libc/syscall.h) - there is NO
// SYS_LISTEN / SYS_ACCEPT exposed to userland, even though the kernel already
// HAS tcp_listen()/tcp_accept() internally (net/tcp.c, used by the in-kernel
// sshd). Exposing them would need two small new syscalls in
// kernel/proc/syscall.c, which is explicitly out of scope for this task (#440
// is userland-only; a CPython kernel build may be in flight on the same tree).
//
// So this server uses the RFB spec's *reverse connection* / "listening
// viewer" mode (RFC 6143 7.1.2, supported natively by TigerVNC's
// `vncviewer -listen <port>`): the compositor is the RFB SERVER (owns the
// framebuffer, drives the handshake as server, sends FramebufferUpdates,
// applies PointerEvent/KeyEvent) but it CONNECTS OUT to a listening viewer,
// using only the pre-existing outbound tcp_socket()/tcp_connect() syscalls.
// Once the TCP connection exists the RFB byte stream is 100% normal RFB 3.8 -
// nothing about the protocol itself is nonstandard, only which side dials.
//
// Config: /CONFIG/VNC.CFG (plain text, "key=value" lines):
//   host=<BUILD_SERVER>
//   port=5500
// If the file is missing, the feature is inert (never opens a socket), so it
// cannot regress normal compositor behaviour when unconfigured.
//
// NO BUSY-WAIT (#426): vnc_poll() is called once per compositor frame from
// main.c's main loop, exactly like screenshot_poll(). Every socket call used
// here (tcp_connect/tcp_send/tcp_recv/tcp_get_state) is non-blocking at the
// kernel level (returns immediately - see proc/syscall.c tcp_*_kcr3 comments),
// so polling them once per frame is a bounded, cheap check, not a spin loop.
// Framebuffer sends are streamed a bounded number of small chunks per poll
// call (never the whole frame in one shot) so the compositor draw loop is
// never blocked waiting on the network.

#include "compositor.h"
#include "../../libc/syscall.h"

// ---------------------------------------------------------------------------
// TCP client wrapper constants (mirrors apps/nc/main.c - the only other
// userland TCP client in the tree).
// ---------------------------------------------------------------------------
#define TCP_STATE_CLOSED       0
#define TCP_STATE_ESTABLISHED  4
#define TCP_ERR_IN_PROGRESS   (-8)

// MayteraOS special-key codes (mirrors kernel/cpu/isr.h KEY_* - not visible to
// userland headers, so duplicated here; press codes 0x80-0x89, release = +0x10).
#define MK_UP      0x80
#define MK_DOWN    0x81
#define MK_LEFT    0x82
#define MK_RIGHT   0x83
#define MK_LCTRL   0x84
#define MK_F11     0x85
#define MK_F12     0x86
#define MK_LSHIFT  0x87
#define MK_RSHIFT  0x88
#define MK_ALT     0x89
#define MK_F6      0x8A

// ---------------------------------------------------------------------------
// Small helpers (no libc string/stdlib dependency - same pattern as
// profile.c/traymenu.c's local atoi helpers).
// ---------------------------------------------------------------------------
static int vnc_atoi(const char *s) {
    int v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

// Parse "a.b.c.d" into a network-order (first octet in MSB) uint32. 0 on error.
static unsigned int vnc_parse_ip(const char *s) {
    unsigned int parts[4] = {0, 0, 0, 0};
    int idx = 0, digits = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx] * 10 + (unsigned)(*p - '0');
            digits++;
            if (parts[idx] > 255) return 0;
        } else if (*p == '.' || *p == '\0' || *p == '\r' || *p == '\n') {
            if (digits == 0) return 0;
            digits = 0;
            if (*p != '.') {
                if (idx != 3) return 0;
                return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
            }
            if (++idx > 3) return 0;
        } else {
            return 0;
        }
    }
}

static void put_be16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)(v & 0xFF);
}
static void put_be32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static int          g_cfg_loaded = 0;
static unsigned int g_cfg_ip     = 0;
static int           g_cfg_port  = 0;

static void vnc_load_config(void) {
    g_cfg_loaded = 1;   // only try once per boot; edit + restart compositor to pick up changes
    g_cfg_ip = 0;
    g_cfg_port = 0;

    int fd = sys_open("/CONFIG/VNC.CFG", 0 /* O_RDONLY */);
    if (fd < 0) return;
    char buf[256];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char host[64]; host[0] = '\0';
    char portstr[16]; portstr[0] = '\0';

    int i = 0;
    while (buf[i]) {
        // Skip to start of a line already positioned at i.
        if (buf[i] == 'h' && buf[i+1] == 'o' && buf[i+2] == 's' && buf[i+3] == 't' && buf[i+4] == '=') {
            int j = i + 5, k = 0;
            while (buf[j] && buf[j] != '\r' && buf[j] != '\n' && k < (int)sizeof(host) - 1)
                host[k++] = buf[j++];
            host[k] = '\0';
        } else if (buf[i] == 'p' && buf[i+1] == 'o' && buf[i+2] == 'r' && buf[i+3] == 't' && buf[i+4] == '=') {
            int j = i + 5, k = 0;
            while (buf[j] && buf[j] != '\r' && buf[j] != '\n' && k < (int)sizeof(portstr) - 1)
                portstr[k++] = buf[j++];
            portstr[k] = '\0';
        }
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;
    }

    if (host[0]) g_cfg_ip = vnc_parse_ip(host);
    if (portstr[0]) g_cfg_port = vnc_atoi(portstr);
    if (g_cfg_port <= 0 || g_cfg_port > 65535) g_cfg_port = 5500;
}

// ---------------------------------------------------------------------------
// Connection + RFB handshake state machine
// ---------------------------------------------------------------------------
typedef enum {
    VS_OFF = 0,          // not configured / disabled
    VS_IDLE,              // configured, no socket yet (or backing off after a failure)
    VS_CONNECTING,        // tcp_connect() issued, waiting for ESTABLISHED
    VS_SEND_VERSION,
    VS_RECV_VERSION,
    VS_SEND_SECURITY,
    VS_RECV_SEC_CHOICE,
    VS_SEND_SEC_RESULT,
    VS_RECV_CLIENTINIT,
    VS_SEND_SERVERINIT,
    VS_RUNNING
} vnc_state_t;

static vnc_state_t g_state = VS_OFF;
static int         g_sock = -1;
static long        g_retry_at_ticks = 0;   // get_ticks() value to try again

// Small outbound message buffer (handshake messages only - a few dozen bytes).
static unsigned char g_out[64];
static int g_out_len = 0, g_out_off = 0;

static void out_queue(const void *data, int len) {
    if (len > (int)sizeof(g_out)) len = (int)sizeof(g_out);   // never happens for our messages
    memcpy(g_out, data, (unsigned long)len);
    g_out_len = len;
    g_out_off = 0;
}
// Returns 1 once fully flushed, 0 if more remains (retry next poll).
static int out_flush(void) {
    while (g_out_off < g_out_len) {
        int chunk = g_out_len - g_out_off;
        int n = tcp_send(g_sock, g_out + g_out_off, chunk);
        if (n <= 0) return 0;
        g_out_off += n;
    }
    g_out_len = 0;
    g_out_off = 0;
    return 1;
}

// Inbound byte buffer for handshake + client message parsing.
#define IN_BUF_SIZE 2048
static unsigned char g_in[IN_BUF_SIZE];
static int g_in_len = 0;

static void in_pump(void) {
    int space = IN_BUF_SIZE - g_in_len;
    if (space <= 0) return;
    int n = tcp_recv(g_sock, g_in + g_in_len, space);
    if (n > 0) g_in_len += n;
}
static void in_consume(int n) {
    if (n >= g_in_len) { g_in_len = 0; return; }
    memmove(g_in, g_in + n, (unsigned long)(g_in_len - n));
    g_in_len -= n;
}

// ClientCutText payloads bigger than our buffer are discarded byte-by-byte
// without ever landing in g_in (so an arbitrarily large clipboard paste from
// the viewer cannot overflow anything).
static long g_discard_remaining = 0;

static void vnc_reset_connection(void) {
    if (g_sock >= 0) tcp_close(g_sock);
    g_sock = -1;
    g_in_len = 0;
    g_out_len = 0; g_out_off = 0;
    g_discard_remaining = 0;
    g_state = VS_IDLE;
    // Gentle backoff (~8s). If a sustained stream ends and the kernel TCP client
    // state needs a moment to settle, hammering reconnects every frame only
    // produces RST noise; a longer backoff keeps the retry attempts sane.
    g_retry_at_ticks = get_ticks() + 800;
}

// ---------------------------------------------------------------------------
// Damage / dirty-rect tracking for FramebufferUpdate (#379 reuse)
// ---------------------------------------------------------------------------
typedef struct { int x, y, w, h; } vnc_rect_t;

#define VNC_MAX_DIRTY 64
static vnc_rect_t g_dirty[VNC_MAX_DIRTY];
static int        g_dirty_count = 0;
static int        g_full_dirty = 1;   // start dirty so the first update is a full frame

void vnc_mark_full_dirty(void) {
    g_full_dirty = 1;
}
void vnc_mark_rect_dirty(int x, int y, int w, int h) {
    if (g_full_dirty) return;
    if (g_dirty_count >= VNC_MAX_DIRTY) { g_full_dirty = 1; return; }
    g_dirty[g_dirty_count].x = x;
    g_dirty[g_dirty_count].y = y;
    g_dirty[g_dirty_count].w = w;
    g_dirty[g_dirty_count].h = h;
    g_dirty_count++;
}

// Client's outstanding FramebufferUpdateRequest.
static int g_update_requested   = 0;
static int g_update_incremental = 0;

// ---------------------------------------------------------------------------
// FramebufferUpdate streaming state (drained a bounded chunk per poll, never
// the whole frame in one call).
// ---------------------------------------------------------------------------
typedef enum {
    SEND_IDLE = 0,
    SEND_MSG_HEADER,
    SEND_RECT_HEADER,
    SEND_ROWS
} send_state_t;

static send_state_t g_send_state = SEND_IDLE;
static vnc_rect_t    g_send_rects[VNC_MAX_DIRTY + 1];
static int           g_send_rect_count = 0;
static int           g_send_cur_rect = 0;
static int           g_send_cur_row = 0;
static int           g_send_row_off = 0;

#define VNC_SEND_CHUNK   1400   // stays under TCP_MSS(1460) / the kernel's 1600B kbuf
// Bounded work per poll. Each tcp_send() drains RX (ACKs) inside the kernel, so
// a larger budget lets a full 1280x800 frame (~4MB) finish in well under a
// second instead of ~13s, which both makes remote interaction feel live and
// lets the stream reach the idle state where only small damage rects are sent.
// Still bounded (never "send the whole frame in one call") so the draw loop is
// not blocked, per CLAUDE.md #426.
#define VNC_SENDS_PER_POLL 96

static void vnc_begin_update(void) {
    if (g_full_dirty || !g_update_incremental) {
        g_send_rects[0].x = 0;
        g_send_rects[0].y = 0;
        g_send_rects[0].w = g_fb_width;
        g_send_rects[0].h = g_fb_height;
        g_send_rect_count = 1;
    } else {
        g_send_rect_count = g_dirty_count;
        for (int i = 0; i < g_dirty_count; i++) g_send_rects[i] = g_dirty[i];
    }
    g_full_dirty = 0;
    g_dirty_count = 0;
    g_update_requested = 0;

    g_send_cur_rect = 0;
    g_send_cur_row = 0;
    g_send_row_off = 0;
    g_send_state = SEND_MSG_HEADER;
}

// Streams as much of the current update as the per-poll budget allows.
// Returns nothing; state persists across calls until SEND_IDLE.
static void vnc_stream_update(void) {
    int budget = VNC_SENDS_PER_POLL;

    while (budget > 0) {
        if (g_send_state == SEND_MSG_HEADER) {
            if (g_out_len == 0) {
                unsigned char m[4];
                m[0] = 0;              // FramebufferUpdate
                m[1] = 0;              // padding
                put_be16(m + 2, (unsigned)g_send_rect_count);
                out_queue(m, 4);
            }
            if (!out_flush()) return;   // retry next poll
            g_send_state = SEND_RECT_HEADER;
            continue;
        }

        if (g_send_state == SEND_RECT_HEADER) {
            if (g_send_cur_rect >= g_send_rect_count) {
                g_send_state = SEND_IDLE;
                return;
            }
            if (g_out_len == 0) {
                vnc_rect_t *r = &g_send_rects[g_send_cur_rect];
                unsigned char m[12];
                put_be16(m + 0, (unsigned)r->x);
                put_be16(m + 2, (unsigned)r->y);
                put_be16(m + 4, (unsigned)r->w);
                put_be16(m + 6, (unsigned)r->h);
                put_be32(m + 8, 0);   // encoding = Raw
                out_queue(m, 12);
            }
            if (!out_flush()) return;
            g_send_cur_row = 0;
            g_send_row_off = 0;
            g_send_state = SEND_ROWS;
            continue;
        }

        if (g_send_state == SEND_ROWS) {
            vnc_rect_t *r = &g_send_rects[g_send_cur_rect];
            if (r->w <= 0 || r->h <= 0 || g_send_cur_row >= r->h) {
                g_send_cur_rect++;
                g_send_state = SEND_RECT_HEADER;
                continue;
            }
            // Clip the rect to the current framebuffer bounds defensively.
            int rx = r->x, rw = r->w;
            if (rx < 0) { rw += rx; rx = 0; }
            if (rx + rw > g_fb_width) rw = g_fb_width - rx;
            if (rw < 0) rw = 0;

            const unsigned char *row_ptr =
                (const unsigned char *)&g_fb[(r->y + g_send_cur_row) * g_fb_pitch + rx];
            int row_bytes = rw * 4;
            int remaining = row_bytes - g_send_row_off;
            if (remaining <= 0) {
                g_send_cur_row++;
                g_send_row_off = 0;
                continue;
            }
            int chunk = remaining;
            if (chunk > VNC_SEND_CHUNK) chunk = VNC_SEND_CHUNK;

            int n = tcp_send(g_sock, row_ptr + g_send_row_off, chunk);
            if (n <= 0) return;   // backpressure - retry next poll
            g_send_row_off += n;
            budget--;
            continue;
        }

        break;
    }
}

// ---------------------------------------------------------------------------
// Input translation -> existing compositor injection syscalls
// ---------------------------------------------------------------------------
static int g_last_button_mask = 0;

static void vnc_inject_pointer(int x, int y, int mask) {
    if (x < 0) x = 0;
    if (x >= g_fb_width) x = g_fb_width - 1;
    if (y < 0) y = 0;
    if (y >= g_fb_height) y = g_fb_height - 1;

    set_mouse_pos(x, y);
    sys_inject_mouse(x, y, MOUSE_EVENT_MOVE, 0);

    int left = mask & 0x01, pleft = g_last_button_mask & 0x01;
    int right = mask & 0x04, pright = g_last_button_mask & 0x04;
    if (left && !pleft) sys_inject_mouse(x, y, MOUSE_EVENT_DOWN, 1);
    if (!left && pleft) sys_inject_mouse(x, y, MOUSE_EVENT_UP, 1);
    if (right && !pright) sys_inject_mouse(x, y, MOUSE_EVENT_DOWN, 2);
    if (!right && pright) sys_inject_mouse(x, y, MOUSE_EVENT_UP, 2);

    g_last_button_mask = mask;
}

// Maps an X11 keysym (RFB KeyEvent) to a MayteraOS key code and injects it via
// the same sys_inject_key() the physical PS/2 keyboard path uses (main.c).
static void vnc_inject_key(unsigned int keysym, int down) {
    int mkey;
    int is_special = 0;

    if (keysym >= 0x20 && keysym <= 0x7E) {
        mkey = (int)keysym;   // printable Latin-1 == ASCII for this range
    } else {
        switch (keysym) {
            case 0xFF08: mkey = 0x08; break;              // BackSpace
            case 0xFF09: mkey = 0x09; break;              // Tab
            case 0xFF0D: mkey = 0x0D; break;              // Return
            case 0xFF1B: mkey = 0x1B; break;              // Escape
            case 0xFF52: mkey = MK_UP;     is_special = 1; break;
            case 0xFF54: mkey = MK_DOWN;   is_special = 1; break;
            case 0xFF51: mkey = MK_LEFT;   is_special = 1; break;
            case 0xFF53: mkey = MK_RIGHT;  is_special = 1; break;
            case 0xFFE1: mkey = MK_LSHIFT; is_special = 1; break;
            case 0xFFE2: mkey = MK_RSHIFT; is_special = 1; break;
            case 0xFFE3: case 0xFFE4: mkey = MK_LCTRL; is_special = 1; break;
            case 0xFFE9: case 0xFFEA: mkey = MK_ALT;   is_special = 1; break;
            case 0xFFC3: mkey = MK_F6;  is_special = 1; break;
            case 0xFFC8: mkey = MK_F11; is_special = 1; break;
            case 0xFFC9: mkey = MK_F12; is_special = 1; break;
            default: return;   // unmapped keysym: ignore rather than misinject
        }
    }

    if (down) {
        sys_inject_key(mkey);
    } else if (is_special) {
        sys_inject_key(mkey + 0x10);
    } else {
        sys_inject_key(mkey | 0x80);
    }
}

// ---------------------------------------------------------------------------
// Client -> server message parsing (VS_RUNNING)
// ---------------------------------------------------------------------------
static void vnc_process_messages(void) {
    for (;;) {
        // Bulk-discard an oversized ClientCutText payload without buffering it.
        if (g_discard_remaining > 0) {
            unsigned char scratch[256];
            int want = (g_discard_remaining > (long)sizeof(scratch))
                           ? (int)sizeof(scratch) : (int)g_discard_remaining;
            int n = tcp_recv(g_sock, scratch, want);
            if (n <= 0) return;
            g_discard_remaining -= n;
            continue;
        }

        in_pump();
        if (g_in_len < 1) return;

        unsigned char t = g_in[0];
        switch (t) {
            case 0: {   // SetPixelFormat (20 bytes). We always serve our fixed
                        // native true-color 32bpp format regardless of what the
                        // client asks for; consume + ignore to stay in sync.
                if (g_in_len < 20) return;
                in_consume(20);
                break;
            }
            case 2: {   // SetEncodings: type(1) pad(1) count(2BE) count*int32
                if (g_in_len < 4) return;
                int count = (g_in[2] << 8) | g_in[3];
                int total = 4 + count * 4;
                if (total > IN_BUF_SIZE) { vnc_reset_connection(); return; }
                if (g_in_len < total) return;
                in_consume(total);   // we always send Raw regardless of the list
                break;
            }
            case 3: {   // FramebufferUpdateRequest: type incr x y w h (10 bytes)
                if (g_in_len < 10) return;
                g_update_incremental = g_in[1];
                g_update_requested = 1;
                in_consume(10);
                break;
            }
            case 4: {   // KeyEvent: type down-flag(1) pad(2) keysym(4BE) = 8 bytes
                if (g_in_len < 8) return;
                int down = g_in[1];
                unsigned int keysym = ((unsigned)g_in[4] << 24) | ((unsigned)g_in[5] << 16) |
                                       ((unsigned)g_in[6] << 8) | (unsigned)g_in[7];
                vnc_inject_key(keysym, down);
                in_consume(8);
                break;
            }
            case 5: {   // PointerEvent: type mask(1) x(2BE) y(2BE) = 6 bytes
                if (g_in_len < 6) return;
                int mask = g_in[1];
                int x = (g_in[2] << 8) | g_in[3];
                int y = (g_in[4] << 8) | g_in[5];
                vnc_inject_pointer(x, y, mask);
                in_consume(6);
                break;
            }
            case 6: {   // ClientCutText: type pad(3) len(4BE) len bytes
                if (g_in_len < 8) return;
                unsigned int len = ((unsigned)g_in[4] << 24) | ((unsigned)g_in[5] << 16) |
                                    ((unsigned)g_in[6] << 8) | (unsigned)g_in[7];
                if (8 + (long)len <= IN_BUF_SIZE) {
                    if ((unsigned)g_in_len < 8 + len) return;
                    in_consume((int)(8 + len));
                } else {
                    in_consume(8);
                    g_discard_remaining = (long)len;
                }
                break;
            }
            default:
                // Cannot safely resynchronize on an unknown message type.
                vnc_reset_connection();
                return;
        }
    }
}

// ---------------------------------------------------------------------------
// Top-level per-frame poll
// ---------------------------------------------------------------------------
void vnc_poll(void) {
    if (!g_cfg_loaded) vnc_load_config();

    if (g_state == VS_OFF) {
        if (g_cfg_ip == 0 || g_cfg_port == 0) return;   // not configured: inert
        g_state = VS_IDLE;
    }

    if (g_state == VS_IDLE) {
        long now = get_ticks();
        if (now < g_retry_at_ticks) return;
        int sock = tcp_socket();
        if (sock < 0) { g_retry_at_ticks = now + 800; return; }
        int rc = tcp_connect(sock, g_cfg_ip, g_cfg_port);
        if (rc < 0 && rc != TCP_ERR_IN_PROGRESS) {
            tcp_close(sock);
            g_retry_at_ticks = now + 800;
            return;
        }
        g_sock = sock;
        g_state = VS_CONNECTING;
        return;
    }

    if (g_state == VS_CONNECTING) {
        int st = tcp_get_state(g_sock);
        if (st == TCP_STATE_ESTABLISHED) {
            g_in_len = 0;
            g_out_len = 0; g_out_off = 0;
            g_last_button_mask = 0;
            g_full_dirty = 1;
            g_dirty_count = 0;
            g_update_requested = 0;
            g_send_state = SEND_IDLE;
            g_state = VS_SEND_VERSION;
        } else if (st == TCP_STATE_CLOSED) {
            vnc_reset_connection();
        }
        return;   // give it more polls either way
    }

    if (g_state == VS_SEND_VERSION) {
        if (g_out_len == 0) out_queue("RFB 003.008\n", 12);
        if (out_flush()) g_state = VS_RECV_VERSION;
        return;
    }
    if (g_state == VS_RECV_VERSION) {
        in_pump();
        if (g_in_len >= 12) { in_consume(12); g_state = VS_SEND_SECURITY; }
        return;
    }
    if (g_state == VS_SEND_SECURITY) {
        if (g_out_len == 0) { unsigned char m[2] = {1, 1}; out_queue(m, 2); }  // 1 type: None
        if (out_flush()) g_state = VS_RECV_SEC_CHOICE;
        return;
    }
    if (g_state == VS_RECV_SEC_CHOICE) {
        in_pump();
        if (g_in_len >= 1) { in_consume(1); g_state = VS_SEND_SEC_RESULT; }
        return;
    }
    if (g_state == VS_SEND_SEC_RESULT) {
        if (g_out_len == 0) { unsigned char m[4] = {0, 0, 0, 0}; out_queue(m, 4); }  // OK
        if (out_flush()) g_state = VS_RECV_CLIENTINIT;
        return;
    }
    if (g_state == VS_RECV_CLIENTINIT) {
        in_pump();
        if (g_in_len >= 1) { in_consume(1); g_state = VS_SEND_SERVERINIT; }
        return;
    }
    if (g_state == VS_SEND_SERVERINIT) {
        if (g_out_len == 0) {
            static const char name[] = "MayteraOS";
            // width(2)+height(2)+pixelformat(16)+namelen(4) = 24 fixed bytes,
            // then the name itself (sizeof(name)-1, excluding the NUL).
            unsigned char m[24 + sizeof(name) - 1];
            put_be16(m + 0, (unsigned)g_fb_width);
            put_be16(m + 2, (unsigned)g_fb_height);
            // PixelFormat (16 bytes): 32bpp/24-depth true-color, little-endian,
            // matching g_fb's native in-memory 0x00RRGGBB uint32 layout exactly
            // (so RAW rect bytes can be streamed straight from g_fb, no swap).
            m[4] = 32;             // bits-per-pixel
            m[5] = 24;             // depth
            m[6] = 0;              // big-endian-flag = 0 (little-endian wire)
            m[7] = 1;              // true-colour-flag
            put_be16(m + 8, 255);  // red-max
            put_be16(m + 10, 255); // green-max
            put_be16(m + 12, 255); // blue-max
            m[14] = 16;            // red-shift
            m[15] = 8;             // green-shift
            m[16] = 0;             // blue-shift
            m[17] = 0; m[18] = 0; m[19] = 0;   // padding
            put_be32(m + 20, sizeof(name) - 1);
            memcpy(m + 24, name, sizeof(name) - 1);
            out_queue(m, (int)sizeof(m));
        }
        if (out_flush()) {
            g_full_dirty = 1;   // guarantee the first FramebufferUpdate is a full frame
            g_state = VS_RUNNING;
        }
        return;
    }

    if (g_state == VS_RUNNING) {
        // Detect peer close early (tcp_recv/tcp_send already handle this, but
        // check the connection state so a dead peer frees the socket promptly).
        int st = tcp_get_state(g_sock);
        if (st == TCP_STATE_CLOSED) { vnc_reset_connection(); return; }

        vnc_process_messages();

        if (g_send_state == SEND_IDLE && g_update_requested &&
            (!g_update_incremental || g_full_dirty || g_dirty_count > 0)) {
            vnc_begin_update();
        }
        if (g_send_state != SEND_IDLE) {
            vnc_stream_update();
        }
    }
}
