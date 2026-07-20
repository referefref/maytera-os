// irc.c - IRC Client for MayteraOS (user-mode)
// Multi-channel client with a channel list, per-channel scrollback and user
// lists, a separate Status window for server/system noise, live window resize,
// mouse-wheel scrollback and click-to-switch channels.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"

#ifndef TCP_STATE_ESTABLISHED
#define TCP_STATE_ESTABLISHED 4
#endif
#ifndef TCP_STATE_CLOSED
#define TCP_STATE_CLOSED 0
#endif

#define WIN_W 760
#define WIN_H 520
#define CHAR_W 8
#define CHAR_H 16

#define IRC_PORT 6667
#define MAX_MSG_LEN 512
#define MAX_NICK_LEN 32
#define MAX_CHANNEL_LEN 64
#define MAX_LINE_LEN 200
#define INPUT_MAX 256

// Per-channel limits (kept modest: total static footprint stays a few hundred KB)
#define MAX_CHANNELS 10
#define MAX_CHAT_LINES 120
#define MAX_USERS 64

// Connection states
enum { IRC_DISCONNECTED, IRC_CONNECTING, IRC_CONNECTED, IRC_REGISTERED };

// Layout constants (fixed sizes; positions are computed from the live window
// size so the client reflows on resize).
#define STATUS_H   20
#define INPUT_H    28
#define CHANLIST_W 120
#define USERLIST_W 130
#define CHANROW_H  18

// Colors
// Theme-aware roles (live updated by apply_theme()). Defaults = Dark theme.
// main window bg, chat area bg, input field bg, status bar bg, channel-list bg,
// active-channel highlight, user-list bg, primary text, borders/separators.
static uint32_t BG_COLOR     = 0x001E1E1E;
static uint32_t CHAT_BG      = 0x00282828;
static uint32_t INPUT_BG     = 0x00353535;
static uint32_t STATUS_BG    = 0x00404060;
static uint32_t CHANLIST_BG  = 0x00222230;
static uint32_t CHAN_SEL_BG  = 0x00385078;
static uint32_t USERLIST_BG  = 0x00252530;
static uint32_t TEXT_COLOR   = 0x00E0E0E0;
static uint32_t BORDER_COLOR = 0x00505050;
// Genuinely semantic colors: kept fixed across themes (per-role meaning).
#define NICK_COLOR      0x0066CCFF
#define JOIN_COLOR      0x0066FF66
#define QUIT_COLOR      0x00FF6666
#define SYSTEM_COLOR    0x00FFCC00
#define UNREAD_COLOR    0x00FFD060

// Live theme tracking. apply_theme() maps the kernel theme id (get_theme():
// 1=Dark, 2=Light, 4=Classic, 5=Ocean, 9=Nord) onto the role vars above using
// the shared MayteraOS palette literals. theme_color() is intentionally NOT
// used: its kernel table is indexed differently and returns wrong colors.
static int g_theme_last = -1;

static void apply_theme(void) {
    int kt = get_theme();
    switch (kt) {
        case 2:  // Light
            BG_COLOR=0x00FFFFFF; CHANLIST_BG=0x00F0F0F0; CHAT_BG=0x00F8F8F8;
            USERLIST_BG=0x00F0F0F0; STATUS_BG=0x00E8E8E8; INPUT_BG=0x00FFFFFF;
            TEXT_COLOR=0x00202020; BORDER_COLOR=0x00CCCCCC; CHAN_SEL_BG=0x00D6E4FB;
            break;
        case 4:  // Classic
            BG_COLOR=0x00C0C0C0; CHANLIST_BG=0x00C0C0C0; CHAT_BG=0x00D0D0D0;
            USERLIST_BG=0x00C0C0C0; STATUS_BG=0x00000080; INPUT_BG=0x00FFFFFF;
            TEXT_COLOR=0x00000000; BORDER_COLOR=0x00808080; CHAN_SEL_BG=0x00000080;
            break;
        case 5:  // Ocean
            BG_COLOR=0x00224455; CHANLIST_BG=0x001A3A4A; CHAT_BG=0x001E4050;
            USERLIST_BG=0x001A3A4A; STATUS_BG=0x00305060; INPUT_BG=0x00183040;
            TEXT_COLOR=0x00E0F0FF; BORDER_COLOR=0x00406070; CHAN_SEL_BG=0x00305060;
            break;
        case 9:  // Nord
            BG_COLOR=0x003B4252; CHANLIST_BG=0x002E3440; CHAT_BG=0x00343B49;
            USERLIST_BG=0x002E3440; STATUS_BG=0x00434C5E; INPUT_BG=0x002B303B;
            TEXT_COLOR=0x00ECEFF4; BORDER_COLOR=0x004C566A; CHAN_SEL_BG=0x00434C5E;
            break;
        default: // Dark
            BG_COLOR=0x001E1E1E; CHANLIST_BG=0x00222230; CHAT_BG=0x00282828;
            USERLIST_BG=0x00252530; STATUS_BG=0x00404060; INPUT_BG=0x00353535;
            TEXT_COLOR=0x00E0E0E0; BORDER_COLOR=0x00505050; CHAN_SEL_BG=0x00385078;
            break;
    }
    g_theme_last = kt;
}

static int win = -1;
static int sock = -1;
static int irc_state = IRC_DISCONNECTED;

// ---- Channel model -------------------------------------------------------
typedef struct {
    char     name[MAX_CHANNEL_LEN];   // "#chan"; status channel uses is_status
    int      is_status;               // 1 = the server/system pseudo-channel
    char     lines[MAX_CHAT_LINES][MAX_LINE_LEN];
    uint32_t colors[MAX_CHAT_LINES];
    int      count;
    int      scroll;                  // index of first visible line
    int      unread;                  // unseen lines while not focused
    char     users[MAX_USERS][MAX_NICK_LEN];
    int      user_count;
} channel_t;

static channel_t chans[MAX_CHANNELS];
static int chan_count = 0;
static int cur_chan = 0;

// Live layout (recomputed every frame from the window size)
static int g_w = WIN_W, g_h = WIN_H;
static int chat_x, chat_y, chat_w, chat_h;
static int ulist_x, ulist_y;

// Input
static char input_buf[INPUT_MAX];
static int input_len = 0;
static int input_cursor = 0;   // caret index into input_buf (task #244)

// Connection info
static char nickname[MAX_NICK_LEN] = "MayteraUser";
static char server_ip[64] = "irc.libera.chat";

// Dialog state
static int show_dialog = 1;
static int dialog_field = 0;
static char dialog_server[64] = "irc.libera.chat";
static char dialog_nick[MAX_NICK_LEN] = "MayteraOS";
static char dialog_channel[MAX_CHANNEL_LEN] = "#mayteraos";
static char join_channel[MAX_CHANNEL_LEN] = "#mayteraos"; // channel to auto-join after register

// Receive buffer for partial line assembly
static char recv_accum[2048];
static int recv_accum_len = 0;

static void draw_all(void);

static void safe_strcpy(char *dst, const char *src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ieq(const char *a, const char *b) {
    while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == *b;
}

// ---- Channel helpers -----------------------------------------------------
static int chan_create(const char *name, int is_status) {
    if (chan_count >= MAX_CHANNELS) return -1;
    channel_t *c = &chans[chan_count];
    safe_strcpy(c->name, name, MAX_CHANNEL_LEN);
    c->is_status = is_status;
    c->count = 0; c->scroll = 0; c->unread = 0; c->user_count = 0;
    return chan_count++;
}

static int chan_index(const char *name) {
    for (int i = 0; i < chan_count; i++)
        if (!chans[i].is_status && ieq(chans[i].name, name)) return i;
    return -1;
}

static int chan_get(const char *name) {
    int i = chan_index(name);
    if (i >= 0) return i;
    return chan_create(name, 0);
}

static int chat_visible_lines(void) {
    int v = chat_h / CHAR_H;
    return v < 1 ? 1 : v;
}

static void chan_add(int ci, const char *text, uint32_t color) {
    if (ci < 0 || ci >= chan_count) ci = 0;
    channel_t *c = &chans[ci];
    if (c->count < MAX_CHAT_LINES) {
        safe_strcpy(c->lines[c->count], text, MAX_LINE_LEN);
        c->colors[c->count] = color;
        c->count++;
    } else {
        for (int i = 0; i < MAX_CHAT_LINES - 1; i++) {
            safe_strcpy(c->lines[i], c->lines[i + 1], MAX_LINE_LEN);
            c->colors[i] = c->colors[i + 1];
        }
        safe_strcpy(c->lines[MAX_CHAT_LINES - 1], text, MAX_LINE_LEN);
        c->colors[MAX_CHAT_LINES - 1] = color;
    }
    // Auto-scroll to bottom for the channel being added to.
    int visible = chat_visible_lines();
    c->scroll = (c->count > visible) ? c->count - visible : 0;
    if (ci != cur_chan) c->unread++;
}

static void add_status(const char *text, uint32_t color) { chan_add(0, text, color); }

static void chan_user_add(channel_t *c, const char *nick) {
    for (int i = 0; i < c->user_count; i++)
        if (ieq(c->users[i], nick)) return;
    if (c->user_count < MAX_USERS) safe_strcpy(c->users[c->user_count++], nick, MAX_NICK_LEN);
}

static void chan_user_del(channel_t *c, const char *nick) {
    for (int i = 0; i < c->user_count; i++) {
        if (ieq(c->users[i], nick)) {
            for (int j = i; j < c->user_count - 1; j++)
                safe_strcpy(c->users[j], c->users[j + 1], MAX_NICK_LEN);
            c->user_count--;
            return;
        }
    }
}

static void switch_to(int ci) {
    if (ci < 0 || ci >= chan_count) return;
    cur_chan = ci;
    chans[ci].unread = 0;
}

// ---- Networking ----------------------------------------------------------
static void irc_send(const char *msg) {
    if (sock < 0) return;
    int len = 0; while (msg[len]) len++;
    tcp_send(sock, msg, len);
}

static void irc_cmd(const char *cmd) {
    char buf[MAX_MSG_LEN];
    int i = 0;
    while (cmd[i] && i < MAX_MSG_LEN - 3) { buf[i] = cmd[i]; i++; }
    buf[i++] = '\r'; buf[i++] = '\n'; buf[i] = '\0';
    irc_send(buf);
}

static void irc_connect(void) {
    add_status("Connecting to server...", SYSTEM_COLOR);
    sock = tcp_socket();
    if (sock < 0) { add_status("Failed to create socket", QUIT_COLOR); return; }

    add_status("Resolving server...", SYSTEM_COLOR);
    draw_all(); win_invalidate(win);
    uint32_t ip = 0;
    int dr = dns_start(server_ip, &ip);
    if (dr == 0) {
        for (int i = 0; i < 100 && dr == 0; i++) {
            gui_event_t dummy; win_get_event(win, &dummy, 50);
            dr = dns_poll(&ip);
        }
    }
    if (dr != 1 || ip == 0) {
        add_status("DNS resolution failed", QUIT_COLOR);
        tcp_close(sock); sock = -1; return;
    }

    int ret = tcp_connect(sock, ip, IRC_PORT);
    if (ret < 0 && ret != -8) {
        add_status("Connection failed", QUIT_COLOR);
        tcp_close(sock); sock = -1; return;
    }

    add_status("Waiting for TCP handshake...", SYSTEM_COLOR);
    irc_state = IRC_CONNECTING;
    draw_all(); win_invalidate(win);

    int connected = 0;
    for (int attempt = 0; attempt < 200; attempt++) {
        int state = (int)tcp_get_state(sock);
        if (state == TCP_STATE_ESTABLISHED) { connected = 1; break; }
        if (state == TCP_STATE_CLOSED) break;
        gui_event_t dummy; win_get_event(win, &dummy, 50);
    }
    if (!connected) {
        add_status("Connection timed out", QUIT_COLOR);
        tcp_close(sock); sock = -1; irc_state = IRC_DISCONNECTED; return;
    }

    add_status("Connected, registering...", SYSTEM_COLOR);
    char buf[MAX_MSG_LEN];
    snprintf(buf, sizeof(buf), "NICK %s", nickname); irc_cmd(buf);
    snprintf(buf, sizeof(buf), "USER %s 0 * :MayteraOS IRC Client", nickname); irc_cmd(buf);
}

// Rotate through public IRC networks. NOTE: never auto-join anything but the
// user's channel; testing policy is #mayteraos only and never efnet.
static const char *g_servers[] = {
    "irc.libera.chat", "irc.oftc.net", "irc.austnet.net"
};
#define IRC_NUM_SERVERS 3
static int g_server_idx = 0;

static void irc_connect_rotating(void) {
    for (int t = 0; t < IRC_NUM_SERVERS; t++) {
        irc_connect();
        if (sock >= 0) return;
        g_server_idx = (g_server_idx + 1) % IRC_NUM_SERVERS;
        safe_strcpy(server_ip, g_servers[g_server_idx], 64);
        char m[80];
        snprintf(m, sizeof(m), "Trying next server: %s", server_ip);
        add_status(m, SYSTEM_COLOR);
    }
    add_status("All servers failed.", QUIT_COLOR);
}

// ---- IRC line parsing ----------------------------------------------------
static void handle_irc_line(const char *line) {
    char buf[MAX_LINE_LEN];

    if (line[0]=='P'&&line[1]=='I'&&line[2]=='N'&&line[3]=='G') {
        char pong[MAX_MSG_LEN];
        snprintf(pong, sizeof(pong), "PONG %s", line + 5);
        irc_cmd(pong);
        return;
    }

    if (line[0] != ':') { add_status(line, TEXT_COLOR); return; }

    const char *p = line + 1;
    char sender[MAX_NICK_LEN] = "";
    int si = 0;
    while (*p && *p != '!' && *p != ' ' && si < MAX_NICK_LEN - 1) sender[si++] = *p++;
    sender[si] = '\0';
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;

    char cmd[32] = "";
    int ci = 0;
    while (*p && *p != ' ' && ci < 31) cmd[ci++] = *p++;
    cmd[ci] = '\0';
    while (*p == ' ') p++;

    if (strcmp(cmd, "PRIVMSG") == 0) {
        // target is the first param
        char target[MAX_CHANNEL_LEN]; int ti = 0;
        while (*p && *p != ' ' && ti < MAX_CHANNEL_LEN - 1) target[ti++] = *p++;
        target[ti] = '\0';
        while (*p == ' ') p++;
        if (*p == ':') p++;
        // CTCP ACTION (/me)
        if (p[0] == '\x01' && strncmp(p + 1, "ACTION ", 7) == 0) {
            const char *act = p + 8;
            snprintf(buf, sizeof(buf), "* %s %s", sender, act);
            // strip trailing \x01
            for (int i = 0; buf[i]; i++) if (buf[i] == '\x01') { buf[i] = '\0'; break; }
        } else {
            snprintf(buf, sizeof(buf), "<%s> %s", sender, p);
        }
        if (target[0] == '#' || target[0] == '&') {
            chan_add(chan_get(target), buf, TEXT_COLOR);
        } else {
            // private message to us: keep it in Status so it is never missed
            char pm[MAX_LINE_LEN];
            snprintf(pm, sizeof(pm), "[PM] %s", buf);
            add_status(pm, NICK_COLOR);
        }
    } else if (strcmp(cmd, "JOIN") == 0) {
        char target[MAX_CHANNEL_LEN]; int ti = 0;
        if (*p == ':') p++;
        while (*p && *p != ' ' && *p != '\r' && ti < MAX_CHANNEL_LEN - 1) target[ti++] = *p++;
        target[ti] = '\0';
        int idx = chan_get(target);
        if (ieq(sender, nickname)) {
            switch_to(idx);                       // we joined: focus the new channel
            snprintf(buf, sizeof(buf), "Now talking in %s", target);
            chan_add(idx, buf, SYSTEM_COLOR);
        } else {
            chan_user_add(&chans[idx], sender);
            snprintf(buf, sizeof(buf), "* %s has joined", sender);
            chan_add(idx, buf, JOIN_COLOR);
        }
    } else if (strcmp(cmd, "PART") == 0) {
        char target[MAX_CHANNEL_LEN]; int ti = 0;
        while (*p && *p != ' ' && ti < MAX_CHANNEL_LEN - 1) target[ti++] = *p++;
        target[ti] = '\0';
        int idx = chan_index(target);
        if (idx >= 0) {
            chan_user_del(&chans[idx], sender);
            snprintf(buf, sizeof(buf), "* %s has left", sender);
            chan_add(idx, buf, QUIT_COLOR);
        }
    } else if (strcmp(cmd, "QUIT") == 0) {
        snprintf(buf, sizeof(buf), "* %s has quit", sender);
        for (int i = 1; i < chan_count; i++) {
            int was = chans[i].user_count;
            chan_user_del(&chans[i], sender);
            if (chans[i].user_count != was) chan_add(i, buf, QUIT_COLOR);
        }
    } else if (strcmp(cmd, "353") == 0) {
        // NAMES: ":me = #chan :nick nick ..."  find the channel token then names
        char chn[MAX_CHANNEL_LEN] = "";
        const char *q = p;
        while (*q && *q != '#' && *q != '&' && *q != ':') q++;
        if (*q == '#' || *q == '&') {
            int k = 0; while (*q && *q != ' ' && k < MAX_CHANNEL_LEN - 1) chn[k++] = *q++;
            chn[k] = '\0';
        }
        const char *names = p;
        while (*names && *names != ':') names++;
        if (*names == ':') names++;
        int idx = (chn[0]) ? chan_get(chn) : -1;
        if (idx >= 0) {
            chans[idx].user_count = 0;
            while (*names && chans[idx].user_count < MAX_USERS) {
                while (*names == ' ' || *names == '@' || *names == '+') names++;
                if (!*names) break;
                int ni = 0; char nk[MAX_NICK_LEN];
                while (*names && *names != ' ' && ni < MAX_NICK_LEN - 1) nk[ni++] = *names++;
                nk[ni] = '\0';
                if (ni > 0) safe_strcpy(chans[idx].users[chans[idx].user_count++], nk, MAX_NICK_LEN);
            }
        }
    } else if (strcmp(cmd, "366") == 0) {
        // end of NAMES: ignore (no noise)
    } else if (strcmp(cmd, "001") == 0) {
        irc_state = IRC_REGISTERED;
        add_status("Registered successfully!", SYSTEM_COLOR);
        snprintf(buf, sizeof(buf), "JOIN %s", join_channel); irc_cmd(buf);
    } else if (strcmp(cmd, "376") == 0 || strcmp(cmd, "422") == 0) {
        if (irc_state < IRC_REGISTERED) {
            irc_state = IRC_REGISTERED;
            snprintf(buf, sizeof(buf), "JOIN %s", join_channel); irc_cmd(buf);
        }
    } else if (strcmp(cmd, "322") == 0) {
        // RPL_LIST: "<client> <channel> <#visible> :<topic>"
        const char *q = p;
        while (*q && *q != ' ') q++;            // skip client token
        while (*q == ' ') q++;
        char chn[48]; int k = 0;
        while (*q && *q != ' ' && k < 47) chn[k++] = *q++; chn[k] = '\0';
        while (*q == ' ') q++;
        char uc[12]; k = 0;
        while (*q && *q != ' ' && k < 11) uc[k++] = *q++; uc[k] = '\0';
        while (*q == ' ') q++;
        if (*q == ':') q++;
        // Build "channel<pad>users  topic" without printf field widths.
        char row[MAX_LINE_LEN]; int r = 0;
        for (int i = 0; chn[i] && r < MAX_LINE_LEN - 1; i++) row[r++] = chn[i];
        while (r < 20 && r < MAX_LINE_LEN - 1) row[r++] = ' ';
        for (int i = 0; uc[i] && r < MAX_LINE_LEN - 1; i++) row[r++] = uc[i];
        if (r < MAX_LINE_LEN - 2) { row[r++] = ' '; row[r++] = ' '; }
        for (const char *t = q; *t && r < MAX_LINE_LEN - 1; t++) row[r++] = *t;
        row[r] = '\0';
        int li = chan_index("Channels"); if (li < 0) li = chan_get("Channels");
        chan_add(li, row, TEXT_COLOR);
    } else if (strcmp(cmd, "321") == 0) {
        int li = chan_index("Channels"); if (li < 0) li = chan_get("Channels");
        chan_add(li, "Channel             Users  Topic", SYSTEM_COLOR);
    } else if (strcmp(cmd, "323") == 0) {
        int li = chan_index("Channels");
        if (li >= 0) chan_add(li, "-- end of channel list --", SYSTEM_COLOR);
    } else if (cmd[0] >= '0' && cmd[0] <= '9') {
        // any other numeric -> Status window only (keeps channels clean)
        const char *text = p;
        while (*text && *text != ':') text++;
        if (*text == ':') text++;
        if (*text) add_status(text, SYSTEM_COLOR);
    } else if (strcmp(cmd, "NICK") == 0) {
        if (*p == ':') p++;
        snprintf(buf, sizeof(buf), "* %s is now known as %s", sender, p);
        for (int i = 1; i < chan_count; i++)
            for (int u = 0; u < chans[i].user_count; u++)
                if (ieq(chans[i].users[u], sender)) { safe_strcpy(chans[i].users[u], p, MAX_NICK_LEN); chan_add(i, buf, SYSTEM_COLOR); break; }
        add_status(buf, SYSTEM_COLOR);
    }
}

static void process_recv(const char *data, int len) {
    for (int i = 0; i < len; i++) {
        if (data[i] == '\r') continue;
        if (data[i] == '\n') {
            recv_accum[recv_accum_len] = '\0';
            if (recv_accum_len > 0) handle_irc_line(recv_accum);
            recv_accum_len = 0;
        } else if (recv_accum_len < (int)sizeof(recv_accum) - 1) {
            recv_accum[recv_accum_len++] = data[i];
        }
    }
}

// ---- Input ---------------------------------------------------------------
static void clear_input(void) { input_len = 0; input_cursor = 0; input_buf[0] = '\0'; }

static void process_input(void) {
    if (input_len == 0) return;
    input_buf[input_len] = '\0';
    char buf[MAX_MSG_LEN];
    channel_t *cc = &chans[cur_chan];

    if (input_buf[0] == '/') {
        if (strncmp(input_buf, "/join ", 6) == 0) {
            const char *ch = input_buf + 6;
            safe_strcpy(join_channel, ch, MAX_CHANNEL_LEN);
            if (irc_state >= IRC_CONNECTED) { snprintf(buf, sizeof(buf), "JOIN %s", ch); irc_cmd(buf); }
            else switch_to(chan_get(ch));
        } else if (strncmp(input_buf, "/nick ", 6) == 0) {
            safe_strcpy(nickname, input_buf + 6, MAX_NICK_LEN);
            snprintf(buf, sizeof(buf), "NICK %s", nickname); irc_cmd(buf);
        } else if (strncmp(input_buf, "/msg ", 5) == 0) {
            char tmp[MAX_MSG_LEN]; safe_strcpy(tmp, input_buf + 5, MAX_MSG_LEN);
            char *target = tmp, *msg = tmp;
            while (*msg && *msg != ' ') msg++;
            if (*msg == ' ') { *msg = '\0'; msg++; }
            snprintf(buf, sizeof(buf), "PRIVMSG %s :%s", target, msg); irc_cmd(buf);
            snprintf(buf, sizeof(buf), "-> %s: %s", target, msg); add_status(buf, NICK_COLOR);
        } else if (strncmp(input_buf, "/me ", 4) == 0) {
            if (!cc->is_status) {
                snprintf(buf, sizeof(buf), "PRIVMSG %s :\x01" "ACTION %s\x01", cc->name, input_buf + 4); irc_cmd(buf);
                snprintf(buf, sizeof(buf), "* %s %s", nickname, input_buf + 4); chan_add(cur_chan, buf, SYSTEM_COLOR);
            }
        } else if (strcmp(input_buf, "/part") == 0 || strncmp(input_buf, "/part ", 6) == 0) {
            if (!cc->is_status) {
                snprintf(buf, sizeof(buf), "PART %s", cc->name); irc_cmd(buf);
                snprintf(buf, sizeof(buf), "Left %s", cc->name); add_status(buf, SYSTEM_COLOR);
                switch_to(0);
            }
        } else if (strcmp(input_buf, "/close") == 0) {
            if (!cc->is_status) {
                // remove the current channel from the list
                for (int i = cur_chan; i < chan_count - 1; i++) chans[i] = chans[i + 1];
                chan_count--;
                switch_to(0);
            }
        } else if (strcmp(input_buf, "/quit") == 0) {
            irc_cmd("QUIT :Leaving");
            if (sock >= 0) { tcp_close(sock); sock = -1; }
            irc_state = IRC_DISCONNECTED;
            add_status("Disconnected.", QUIT_COLOR);
        } else if (strncmp(input_buf, "/server ", 8) == 0) {
            safe_strcpy(server_ip, input_buf + 8, 64);
            irc_connect_rotating();
        } else if (strcmp(input_buf, "/list") == 0 || strncmp(input_buf, "/list ", 6) == 0) {
            if (irc_state >= IRC_REGISTERED) {
                const char *arg = (input_buf[5] == ' ') ? input_buf + 6 : "";
                snprintf(buf, sizeof(buf), "LIST %s", arg); irc_cmd(buf);
                int li = chan_get("Channels");
                chans[li].count = 0; chans[li].scroll = 0;
                chan_add(li, "Fetching channel list...", SYSTEM_COLOR);
                switch_to(li);
            } else {
                add_status("Not connected. /server first.", QUIT_COLOR);
            }
        } else if (strcmp(input_buf, "/next") == 0 || strcmp(input_buf, "/n") == 0) {
            if (chan_count > 1) switch_to((cur_chan + 1) % chan_count);
        } else if (strcmp(input_buf, "/prev") == 0 || strcmp(input_buf, "/p") == 0) {
            if (chan_count > 1) switch_to((cur_chan + chan_count - 1) % chan_count);
        } else if (input_buf[1] >= '1' && input_buf[1] <= '9' && input_buf[2] == '\0') {
            int n2 = input_buf[1] - '1';
            if (n2 < chan_count) switch_to(n2);
        } else {
            snprintf(buf, sizeof(buf), "Unknown command: %s", input_buf);
            add_status(buf, QUIT_COLOR);
        }
    } else {
        if (cc->is_status) {
            add_status("Not in a channel. Use /join #channel", QUIT_COLOR);
        } else {
            snprintf(buf, sizeof(buf), "PRIVMSG %s :%s", cc->name, input_buf); irc_cmd(buf);
            snprintf(buf, sizeof(buf), "<%s> %s", nickname, input_buf); chan_add(cur_chan, buf, NICK_COLOR);
        }
    }
    clear_input();
}

// ---- Layout + drawing ----------------------------------------------------
static void recompute_layout(void) {
    int w = WIN_W, h = WIN_H;
    win_get_size(win, &w, &h);
    if (w < 320) w = 320;
    if (h < 200) h = 200;
    g_w = w; g_h = h;

    int body_h = g_h - STATUS_H - INPUT_H;
    if (body_h < CHAR_H) body_h = CHAR_H;

    int ul_w = USERLIST_W;
    if (g_w < CHANLIST_W + 200 + ul_w) ul_w = 0;   // drop user list on narrow windows
    ulist_x = g_w - ul_w;
    ulist_y = STATUS_H;

    chat_x = CHANLIST_W;
    chat_y = STATUS_H;
    chat_w = g_w - CHANLIST_W - ul_w;
    if (chat_w < CHAR_W * 10) chat_w = CHAR_W * 10;
    chat_h = body_h;
}

static void draw_status_bar(void) {
    win_draw_rect(win, 0, 0, g_w, STATUS_H, STATUS_BG);
    const char *st = "Disconnected";
    if (irc_state == IRC_CONNECTING) st = "Connecting...";
    else if (irc_state == IRC_CONNECTED) st = "Connected";
    else if (irc_state == IRC_REGISTERED) st = "Online";
    char s[160];
    snprintf(s, sizeof(s), " %s | %s | %s", st, server_ip, chans[cur_chan].name);
    win_draw_text(win, 4, 2, s, TEXT_COLOR);
}

static void draw_chanlist(void) {
    win_draw_rect(win, 0, STATUS_H, CHANLIST_W, chat_h, CHANLIST_BG);
    win_draw_rect(win, CHANLIST_W - 1, STATUS_H, 1, chat_h, BORDER_COLOR);
    for (int i = 0; i < chan_count; i++) {
        int ry = STATUS_H + i * CHANROW_H;
        if (ry + CHANROW_H > STATUS_H + chat_h) break;
        if (i == cur_chan) win_draw_rect(win, 0, ry, CHANLIST_W - 1, CHANROW_H, CHAN_SEL_BG);
        char row[40];
        const char *nm = chans[i].is_status ? "Status" : chans[i].name;
        if (chans[i].unread > 0 && i != cur_chan)
            snprintf(row, sizeof(row), "%s (%d)", nm, chans[i].unread);
        else
            snprintf(row, sizeof(row), "%s", nm);
        uint32_t col = (i == cur_chan) ? 0x00FFFFFF
                     : (chans[i].unread > 0 ? UNREAD_COLOR : TEXT_COLOR);
        win_draw_text(win, 6, ry + 1, row, col);
    }
}

// How many wrapped display rows a source line needs at the given width.
static int wrapped_rows(const char *s, int maxc) {
    int len = (int)strlen(s);
    if (len == 0) return 1;
    int rows = (len + maxc - 1) / maxc;
    return rows < 1 ? 1 : rows;
}

static void draw_chat(void) {
    win_draw_rect(win, chat_x, chat_y, chat_w, chat_h, CHAT_BG);
    channel_t *c = &chans[cur_chan];
    int visible = chat_visible_lines();
    int max_chars = (chat_w - 8) / CHAR_W;
    if (max_chars < 8) max_chars = 8;

    // Tail-fit: choose the first source line so the newest lines stay visible
    // even when long messages wrap onto several rows.
    int used = 0, fit = c->count;
    while (fit > 0) {
        int r = wrapped_rows(c->lines[fit - 1], max_chars);
        if (used + r > visible) break;
        used += r;
        fit--;
    }
    int start = c->scroll;
    if (start < 0) start = 0;
    if (start > fit) start = fit;          // never scroll past the tail-fit point

    int row = 0;
    for (int li = start; li < c->count && row < visible; li++) {
        const char *s = c->lines[li];
        uint32_t col = c->colors[li];
        int len = (int)strlen(s);
        if (len == 0) { row++; continue; }
        int pos = 0;
        while (pos < len && row < visible) {
            int take = len - pos;
            if (take > max_chars) take = max_chars;
            // Prefer to break at a space (word wrap) when the line continues.
            if (pos + take < len) {
                int br = -1;
                for (int k = take; k > take / 2; k--)
                    if (s[pos + k - 1] == ' ') { br = k; break; }
                if (br > 0) take = br;
            }
            char tmp[MAX_LINE_LEN];
            int t = 0;
            for (int k = 0; k < take && t < MAX_LINE_LEN - 1; k++) tmp[t++] = s[pos + k];
            tmp[t] = '\0';
            win_draw_text(win, chat_x + 4, chat_y + row * CHAR_H, tmp, col);
            pos += take;
            while (pos < len && s[pos] == ' ') pos++;   // swallow the break space
            row++;
        }
    }
}

static void draw_userlist(void) {
    int uw = g_w - ulist_x;
    if (uw <= 0) return;
    win_draw_rect(win, ulist_x, ulist_y, uw, chat_h, USERLIST_BG);
    win_draw_rect(win, ulist_x, ulist_y, 1, chat_h, BORDER_COLOR);
    channel_t *c = &chans[cur_chan];
    char hdr[24]; snprintf(hdr, sizeof(hdr), "Users (%d)", c->user_count);
    win_draw_text(win, ulist_x + 4, ulist_y + 2, hdr, SYSTEM_COLOR);
    win_draw_rect(win, ulist_x, ulist_y + CHAR_H + 2, uw, 1, BORDER_COLOR);
    int rows = chat_h / CHAR_H - 2;
    for (int i = 0; i < c->user_count && i < rows; i++)
        win_draw_text(win, ulist_x + 4, ulist_y + CHAR_H + 6 + i * CHAR_H, c->users[i], TEXT_COLOR);
}

static void draw_input(void) {
    int iy = g_h - INPUT_H;
    win_draw_rect(win, 0, iy, g_w, INPUT_H, INPUT_BG);
    win_draw_rect(win, 0, iy, g_w, 1, BORDER_COLOR);
    char display[INPUT_MAX + 8];
    int max_chars = (g_w - 16) / CHAR_W;
    // Horizontal scroll so the caret stays visible.
    int start = 0;
    if (max_chars > 2 && input_len > max_chars - 2) {
        // Keep the caret within the visible window.
        if (input_cursor > max_chars - 2) start = input_cursor - (max_chars - 2);
        if (start > input_len) start = input_len;
    }
    const char *shown = input_buf + start;
    snprintf(display, sizeof(display), "> %s", shown);
    win_draw_text(win, 4, iy + 6, display, TEXT_COLOR);
    // Caret: a vertical bar at the cursor column ("> " prefix is 2 chars).
    int caret_col = 2 + (input_cursor - start);
    int caret_x = 4 + caret_col * CHAR_W;
    win_draw_rect(win, caret_x, iy + 4, 1, CHAR_H + 2, TEXT_COLOR);
}

static void draw_dialog(void) {
    win_draw_rect(win, 0, 0, g_w, g_h, BG_COLOR);
    int dw = 400, dh = 230;
    int dx = (g_w - dw) / 2, dy = (g_h - dh) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    win_draw_rect(win, dx, dy, dw, dh, CHAT_BG);
    win_draw_rect(win, dx, dy, dw, 1, BORDER_COLOR);
    win_draw_rect(win, dx, dy + dh - 1, dw, 1, BORDER_COLOR);
    win_draw_rect(win, dx, dy, 1, dh, BORDER_COLOR);
    win_draw_rect(win, dx + dw - 1, dy, 1, dh, BORDER_COLOR);
    win_draw_text(win, dx + 140, dy + 10, "Connect to IRC", SYSTEM_COLOR);

    uint32_t c0 = (dialog_field == 0) ? NICK_COLOR : TEXT_COLOR;
    win_draw_text(win, dx + 20, dy + 50, "Server:", c0);
    win_draw_rect(win, dx + 100, dy + 48, 280, 20, INPUT_BG);
    win_draw_text(win, dx + 104, dy + 50, dialog_server, TEXT_COLOR);

    uint32_t c1 = (dialog_field == 1) ? NICK_COLOR : TEXT_COLOR;
    win_draw_text(win, dx + 20, dy + 80, "Nick:", c1);
    win_draw_rect(win, dx + 100, dy + 78, 280, 20, INPUT_BG);
    win_draw_text(win, dx + 104, dy + 80, dialog_nick, TEXT_COLOR);

    uint32_t c2 = (dialog_field == 2) ? NICK_COLOR : TEXT_COLOR;
    win_draw_text(win, dx + 20, dy + 110, "Channel:", c2);
    win_draw_rect(win, dx + 100, dy + 108, 280, 20, INPUT_BG);
    win_draw_text(win, dx + 104, dy + 110, dialog_channel, TEXT_COLOR);

    win_draw_text(win, dx + 70, dy + 160, "Tab: next field   Enter: connect", TEXT_COLOR);
    win_draw_text(win, dx + 120, dy + 185, "Esc: cancel", TEXT_COLOR);
}

static void draw_all(void) {
    recompute_layout();
    if (show_dialog) { draw_dialog(); return; }
    win_draw_rect(win, 0, 0, g_w, g_h, BG_COLOR);
    draw_status_bar();
    draw_chanlist();
    draw_chat();
    draw_userlist();
    draw_input();
}

static void dialog_key(uint32_t keycode, char ch) {
    char *field; int maxlen;
    if (dialog_field == 0) { field = dialog_server; maxlen = 63; }
    else if (dialog_field == 1) { field = dialog_nick; maxlen = MAX_NICK_LEN - 1; }
    else { field = dialog_channel; maxlen = MAX_CHANNEL_LEN - 1; }
    int len = 0; while (field[len]) len++;

    if (keycode == 0x0F || ch == '\t') {
        dialog_field = (dialog_field + 1) % 3;
    } else if (keycode == 0x1C || ch == '\n' || ch == '\r') {
        safe_strcpy(server_ip, dialog_server, 64);
        safe_strcpy(nickname, dialog_nick, MAX_NICK_LEN);
        safe_strcpy(join_channel, dialog_channel, MAX_CHANNEL_LEN);
        show_dialog = 0;
        irc_connect_rotating();
    } else if (keycode == 0x01) {
        show_dialog = 0;
    } else if (keycode == 0x0E || ch == 8) {
        if (len > 0) field[len - 1] = '\0';
    } else if (ch >= 32 && ch < 127 && len < maxlen) {
        field[len] = ch; field[len + 1] = '\0';
    }
}

// Click in the channel list switches channels.
static void handle_click(int local_x, int local_y) {
    if (show_dialog) return;
    if (local_x >= 0 && local_x < CHANLIST_W && local_y >= STATUS_H) {
        int idx = (local_y - STATUS_H) / CHANROW_H;
        if (idx >= 0 && idx < chan_count) switch_to(idx);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    win = win_create("IRC Client", 80, 60, WIN_W, WIN_H);
    if (win < 0) return 1;

    apply_theme();  // map current kernel theme onto color roles before first draw

    chan_create("Status", 1);
    cur_chan = 0;
    add_status("MayteraOS IRC Client", SYSTEM_COLOR);
    add_status("Connect dialog: fill in and press Enter.", TEXT_COLOR);
    add_status("Ctrl+N reopens it. Click a channel or press Tab to switch.", TEXT_COLOR);
    add_status("Commands: /join /part /list /next /prev /1../9 /msg /me /nick /quit", TEXT_COLOR);

    int win_x = 80, win_y = 60;
    int running = 1;
    while (running) {
        // Live theme polling: re-map colors and redraw when the theme changes.
        { int th = get_theme(); if (th != g_theme_last) { apply_theme(); draw_all(); win_invalidate(win); } }

        gui_event_t ev;
        int ret = win_get_event(win, &ev, 50);
        if (ret > 0) {
            switch (ev.type) {
                case EVENT_WINDOW_CLOSE:
                    running = 0; break;
                case EVENT_RESIZE:
                    // layout is recomputed every draw; nothing to store
                    break;
                case EVENT_KEY_DOWN:
                    if (show_dialog) {
                        dialog_key(ev.keycode, ev.key_char);
                    } else {
                        if (ev.key_char == 14) {            // Ctrl+N: connect dialog
                            show_dialog = 1;
                        } else if (ev.keycode == 0x01) {     // Esc
                            running = 0;
                        } else if (ev.keycode == 0x1C || ev.key_char == '\n') {
                            process_input();
                        } else if (ev.keycode == 0x0F || ev.key_char == '\t') {
                            if (chan_count > 1) switch_to((cur_chan + 1) % chan_count);  // Tab: next channel
                        } else {
                            // Caret-aware editing (Left/Right/Home/End/Delete/
                            // Backspace/insert) via the shared textfield helper.
                            textfield_t tf;
                            tf_attach(&tf, input_buf, INPUT_MAX, input_len, input_cursor);
                            if (tf_handle_key(&tf, &ev)) {
                                input_len = tf.len;
                                input_cursor = tf.cursor;
                            }
                        }
                    }
                    break;
                case EVENT_MOUSE_DOWN: {
                    win_get_pos(win, &win_x, &win_y);
                    int lx = ev.mouse_x;
                    int ly = ev.mouse_y;
                    handle_click(lx, ly);
                    break;
                }
                case EVENT_MOUSE_SCROLL:
                    if (!show_dialog) {
                        channel_t *c = &chans[cur_chan];
                        c->scroll -= ev.scroll_delta * 3;
                        int max_scroll = c->count - chat_visible_lines();
                        if (max_scroll < 0) max_scroll = 0;
                        if (c->scroll < 0) c->scroll = 0;
                        if (c->scroll > max_scroll) c->scroll = max_scroll;
                    }
                    break;
                default: break;
            }
        }

        if (sock >= 0 && irc_state >= IRC_CONNECTING) {
            char recv_buf[1024];
            int n = tcp_recv(sock, recv_buf, sizeof(recv_buf) - 1);
            if (n > 0) process_recv(recv_buf, n);
        }

        draw_all();
        win_invalidate(win);
    }

    if (sock >= 0) { irc_cmd("QUIT :Leaving"); tcp_close(sock); }
    win_destroy(win);
    return 0;
}
