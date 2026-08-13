// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// conv.c - local 66: per-user, reboot-surviving AI conversations (the tab model).
//
// See conv.h for the storage convention, and for the plain statement that this
// is SEPARATION AND NOT A SECURITY BOUNDARY while task 20 is open.
//
// REUSE NOTE. Nothing here opens or completes a file by hand. The path join,
// the <home>/CONFIG mkdir, and the checked write+fsync+close all come from
// userland/libc/userconf.c (#683/#743), which is the one implementation of
// "a file belonging to the session user" in this tree. A second copy of that
// logic living in the chat app is exactly how the compositor and the panel came
// to disagree about where AICHAT.CFG lived (#683), so there is not one.

#include "conv.h"
#include "userconf.h"
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"

#define IDX_NAME "AICONVIX.TXT"
#define IDX_MAGIC "AICONVIX1"
#define CNV_MAGIC "AICONV1"

static conv_t g_conv[CONV_MAX_TABS];
static int    g_nconv  = 0;
static int    g_active = 0;
static char   g_idx_path[256];
// See conv.h: only the instance that loaded the WHOLE index may rewrite it.
static int    g_index_owner = 1;

// ---------------------------------------------------------------------------
// list accessors
// ---------------------------------------------------------------------------
int conv_count(void) { return g_nconv; }
conv_t *conv_at(int i) { return (i >= 0 && i < g_nconv) ? &g_conv[i] : 0; }
int conv_active(void) { return g_active; }

const char *conv_index_path(void) {
    if (!g_idx_path[0]) {
        if (userconf_path(IDX_NAME, g_idx_path, sizeof(g_idx_path)) != 0)
            strlcpy(g_idx_path, "(path too long)", sizeof(g_idx_path));
    }
    return g_idx_path;
}

// "AICONV01.TXT" .. "AICONV08.TXT": FAT 8.3, uppercase, like the rest of the tree.
static void slot_name(int slot, char *out, unsigned long cap) {
    snprintf(out, cap, "AICONV%02d.TXT", slot);
}

// ---------------------------------------------------------------------------
// raw file helpers (thin wrappers over the shared per-user primitives)
// ---------------------------------------------------------------------------
static char *read_all(const char *name, long *out_len) {
    int fd = userconf_open_read(name, 0);      // no legacy location: this is new
    if (fd < 0) return 0;
    long sz = sys_seek(fd, 0, 2 /*SEEK_END*/);
    if (sz <= 0 || sz > CONV_FILE_MAX) { sys_close(fd); return 0; }
    sys_seek(fd, 0, 0 /*SEEK_SET*/);
    char *buf = (char *)malloc((unsigned long)sz + 1);
    if (!buf) { sys_close(fd); return 0; }
    long got = 0;
    while (got < sz) {
        long n = sys_read(fd, buf + got, (unsigned long)(sz - got));
        if (n <= 0) break;
        got += n;
    }
    sys_close(fd);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

static int write_named(const char *name, const char *buf, unsigned long len) {
    int fd = userconf_open_write(name);        // creates <home>/CONFIG for us
    if (fd < 0) return -1;
    return userconf_finish_write(fd, buf, len); // write + fsync + close, checked
}

static void unlink_named(const char *name) {
    char p[256];
    if (userconf_path(name, p, sizeof(p)) == 0) sys_unlink(p);
}

// Consume one '\n'-terminated line. Returns the start of the next line (or 0 at
// end of buffer) and NUL-terminates the line in place.
static char *take_line(char *p, char **line) {
    if (!p || !*p) { *line = 0; return 0; }
    *line = p;
    while (*p && *p != '\n') p++;
    if (*p == '\n') { *p = 0; return p + 1; }
    return p;               // last line, unterminated
}

static long parse_long(const char **pp) {
    const char *p = *pp;
    while (*p == ' ') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *pp = p;
    return neg ? -v : v;
}

// A title is a single line of printable text. Anything else would corrupt the
// "T <auto> <title>" record on the way back out, so it is filtered on the way in
// rather than trusted on the way out.
static void sanitize_title(const char *in, char *out, unsigned long cap) {
    unsigned long o = 0;
    if (cap == 0) return;
    while (in && *in && o + 1 < cap) {
        unsigned char c = (unsigned char)*in++;
        if (c < 32 || c == 127) c = ' ';
        out[o++] = (char)c;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
    if (!out[0]) strlcpy(out, "Chat", cap);
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------
static void free_texts(conv_t *c) {
    for (int m = 0; m < c->nmsgs; m++) { if (c->texts[m]) free(c->texts[m]); c->texts[m] = 0; }
    c->nmsgs = 0;
}

static void conv_append(conv_t *c, int role, const char *text, unsigned long len) {
    if (c->nmsgs >= AICLIENT_MAX_MSGS) return;
    char *copy = (char *)malloc(len + 1);
    if (!copy) return;
    memcpy(copy, text, len);
    copy[len] = 0;
    c->roles[c->nmsgs] = role;
    c->texts[c->nmsgs] = copy;
    c->nmsgs++;
}

// Parse one conversation file into `c`. Returns 1 if the file existed and had a
// valid header, 0 otherwise (in which case `c` is left as an empty conversation).
static int load_slot(conv_t *c, int slot) {
    memset(c, 0, sizeof(*c));
    c->slot = slot;
    c->auto_title = 1;
    snprintf(c->title, sizeof(c->title), "Chat %d", slot);

    char name[24];
    slot_name(slot, name, sizeof(name));
    long len = 0;
    char *buf = read_all(name, &len);
    if (!buf) return 0;

    char *p = buf, *line = 0;
    p = take_line(p, &line);
    if (!line || strcmp(line, CNV_MAGIC) != 0) { free(buf); return 0; }

    while (p && *p) {
        p = take_line(p, &line);
        if (!line) break;
        if (line[0] == 'T' && line[1] == ' ') {
            const char *q = line + 2;
            long a = parse_long(&q);
            while (*q == ' ') q++;
            c->auto_title = a ? 1 : 0;
            sanitize_title(q, c->title, sizeof(c->title));
        } else if (line[0] == 'M' && line[1] == ' ') {
            const char *q = line + 2;
            long role = parse_long(&q);
            long mlen = parse_long(&q);
            if (mlen < 0 || mlen > CONV_TEXT_MAX) break;      // corrupt: stop here
            // The payload is the next `mlen` raw bytes, then one separator '\n'.
            long remain = len - (long)(p - buf);
            if (remain < mlen) break;
            conv_append(c, (int)role, p, (unsigned long)mlen);
            p += mlen;
            if (*p == '\n') p++;
        }
        // any other line: ignore, so a newer format can add records safely
    }
    free(buf);
    return 1;
}

int conv_load(int only_slot) {
    for (int i = 0; i < g_nconv; i++) free_texts(&g_conv[i]);
    g_nconv = 0;
    g_active = 0;

    if (only_slot > 0) {
        if (only_slot > CONV_MAX_TABS) only_slot = CONV_MAX_TABS;
        load_slot(&g_conv[0], only_slot);
        g_nconv = 1;
        g_index_owner = 0;     // a single-slot load means "I am a detached child"
        return 1;
    }
    g_index_owner = 1;

    long len = 0;
    char *buf = read_all(IDX_NAME, &len);
    int active_slot = 0;
    if (buf) {
        char *p = buf, *line = 0;
        p = take_line(p, &line);
        if (line && strcmp(line, IDX_MAGIC) == 0) {
            while (p && *p) {
                p = take_line(p, &line);
                if (!line) break;
                if (line[0] == 'A' && line[1] == ' ') {
                    const char *q = line + 2;
                    active_slot = (int)parse_long(&q);
                } else if (line[0] == 'S' && line[1] == ' ') {
                    const char *q = line + 2;
                    int slot = (int)parse_long(&q);
                    int det  = (int)parse_long(&q);
                    if (slot < 1 || slot > CONV_MAX_TABS) continue;
                    if (g_nconv >= CONV_MAX_TABS) continue;
                    int dup = 0;
                    for (int i = 0; i < g_nconv; i++) if (g_conv[i].slot == slot) dup = 1;
                    if (dup) continue;
                    if (load_slot(&g_conv[g_nconv], slot)) {
                        g_conv[g_nconv].detached = det ? 1 : 0;
                        g_nconv++;
                    }
                }
            }
        }
        free(buf);
    }

    if (g_nconv == 0) conv_new(0);
    for (int i = 0; i < g_nconv; i++)
        if (g_conv[i].slot == active_slot) g_active = i;
    return g_nconv;
}

void conv_clear_detached(void) {
    int changed = 0;
    for (int i = 0; i < g_nconv; i++)
        if (g_conv[i].detached) { g_conv[i].detached = 0; changed = 1; }
    if (changed) conv_save_index();
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------
int conv_save_one(int i) {
    if (i < 0 || i >= g_nconv) return -1;
    conv_t *c = &g_conv[i];

    unsigned long need = 64 + CONV_TITLE_MAX;
    for (int m = 0; m < c->nmsgs; m++) {
        unsigned long l = c->texts[m] ? strlen(c->texts[m]) : 0;
        if (l > CONV_TEXT_MAX) l = CONV_TEXT_MAX;
        need += 32 + l + 2;
    }
    char *buf = (char *)malloc(need + 1);
    if (!buf) return -1;

    int o = snprintf(buf, need, "%s\nT %d %s\n", CNV_MAGIC, c->auto_title, c->title);
    if (o < 0) { free(buf); return -1; }
    for (int m = 0; m < c->nmsgs; m++) {
        const char *t = c->texts[m] ? c->texts[m] : "";
        unsigned long l = strlen(t);
        if (l > CONV_TEXT_MAX) l = CONV_TEXT_MAX;   // truncate on disk, never in RAM
        int hn = snprintf(buf + o, need - (unsigned long)o, "M %d %lu\n", c->roles[m], l);
        if (hn < 0) break;
        o += hn;
        memcpy(buf + o, t, l);
        o += (int)l;
        buf[o++] = '\n';
    }

    char name[24];
    slot_name(c->slot, name, sizeof(name));
    int rc = write_named(name, buf, (unsigned long)o);
    free(buf);
    return rc;
}

int conv_index_owned(void) { return g_index_owner; }

int conv_save_index(void) {
    if (!g_index_owner) return 0;   // not ours to write; see conv.h
    char buf[64 + CONV_MAX_TABS * 24];
    int o = snprintf(buf, sizeof(buf), "%s\nA %d\n", IDX_MAGIC,
                     (g_active >= 0 && g_active < g_nconv) ? g_conv[g_active].slot : 0);
    if (o < 0) return -1;
    for (int i = 0; i < g_nconv; i++) {
        int n = snprintf(buf + o, sizeof(buf) - (unsigned long)o,
                         "S %d %d\n", g_conv[i].slot, g_conv[i].detached);
        if (n < 0) break;
        o += n;
    }
    return write_named(IDX_NAME, buf, (unsigned long)o);
}

int conv_save_all(void) {
    int bad = 0;
    for (int i = 0; i < g_nconv; i++) if (conv_save_one(i) != 0) bad = 1;
    if (conv_save_index() != 0) bad = 1;
    return bad ? -1 : 0;
}

// ---------------------------------------------------------------------------
// editing
// ---------------------------------------------------------------------------
int conv_new(const char *title) {
    if (g_nconv >= CONV_MAX_TABS) return -1;
    int slot = 0;
    for (int s = 1; s <= CONV_MAX_TABS && !slot; s++) {
        int used = 0;
        for (int i = 0; i < g_nconv; i++) if (g_conv[i].slot == s) used = 1;
        if (!used) slot = s;
    }
    if (!slot) return -1;

    conv_t *c = &g_conv[g_nconv];
    memset(c, 0, sizeof(*c));
    c->slot = slot;
    if (title && title[0]) { sanitize_title(title, c->title, sizeof(c->title)); c->auto_title = 0; }
    else { snprintf(c->title, sizeof(c->title), "Chat %d", slot); c->auto_title = 1; }
    return g_nconv++;
}

void conv_snapshot(int i) {
    conv_t *c = conv_at(i);
    if (!c) return;
    free_texts(c);
    int n = aiclient_count();
    for (int m = 0; m < n; m++) {
        const ai_msg_t *msg = aiclient_get(m);
        if (!msg || !msg->text) continue;
        if (msg->role == 3) continue;         // the system prompt is regenerated, not stored
        conv_append(c, msg->role, msg->text, strlen(msg->text));
    }
}

void conv_restore(int i) {
    conv_t *c = conv_at(i);
    if (!c) return;
    // aiclient_reset() re-seeds the role-3 system prompt (tool list + ACTION
    // protocol) from the CURRENT tool index, which is why the system message is
    // deliberately not persisted: a stored one would go stale as tools change.
    aiclient_reset();
    for (int m = 0; m < c->nmsgs; m++)
        if (c->texts[m]) aiclient_add(c->roles[m], c->texts[m]);
    g_active = i;
}

int conv_close(int i) {
    conv_t *c = conv_at(i);
    if (!c) return g_active;
    char name[24];
    slot_name(c->slot, name, sizeof(name));
    free_texts(c);
    unlink_named(name);
    for (int k = i; k < g_nconv - 1; k++) g_conv[k] = g_conv[k + 1];
    g_nconv--;
    memset(&g_conv[g_nconv], 0, sizeof(conv_t));
    if (g_nconv == 0) conv_new(0);
    if (g_active >= g_nconv) g_active = g_nconv - 1;
    if (g_active < 0) g_active = 0;
    conv_save_index();
    return g_active;
}

int conv_move(int i, int dir) {
    if (i < 0 || i >= g_nconv) return i;
    int j = i + (dir < 0 ? -1 : 1);
    if (j < 0 || j >= g_nconv) return i;
    conv_t tmp = g_conv[i];
    g_conv[i] = g_conv[j];
    g_conv[j] = tmp;
    if (g_active == i) g_active = j;
    else if (g_active == j) g_active = i;
    conv_save_index();
    return j;
}

void conv_rename(int i, const char *title) {
    conv_t *c = conv_at(i);
    if (!c) return;
    sanitize_title(title, c->title, sizeof(c->title));
    c->auto_title = 0;
    conv_save_one(i);
    conv_save_index();
}

void conv_autotitle(int i) {
    conv_t *c = conv_at(i);
    if (!c || !c->auto_title) return;
    for (int m = 0; m < c->nmsgs; m++) {
        if (c->roles[m] != 0 || !c->texts[m]) continue;
        char t[CONV_TITLE_MAX];
        unsigned long o = 0;
        const char *s = c->texts[m];
        while (*s && o + 1 < sizeof(t)) {
            unsigned char ch = (unsigned char)*s++;
            if (ch == '\n' || ch == '\r') break;
            if (ch < 32 || ch == 127) ch = ' ';
            t[o++] = (char)ch;
        }
        t[o] = 0;
        sanitize_title(t, c->title, sizeof(c->title));
        // still auto: a later, better first turn may replace it, and the user
        // has not claimed the name yet.
        return;
    }
}

// ---------------------------------------------------------------------------
// child exit: give the conversation back to the docked instance
// ---------------------------------------------------------------------------
void conv_release_detached(int slot) {
    if (slot < 1 || slot > CONV_MAX_TABS) return;
    long len = 0;
    char *buf = read_all(IDX_NAME, &len);
    if (!buf) return;

    // Rebuild the index text, clearing the detached flag on exactly this slot.
    // Everything else is copied through verbatim, so a tab the parent added
    // while we were running is preserved.
    char *out = (char *)malloc((unsigned long)len + 64);
    if (!out) { free(buf); return; }
    int o = 0;
    char *p = buf, *line = 0;
    while (p && *p) {
        p = take_line(p, &line);
        if (!line) break;
        if (line[0] == 'S' && line[1] == ' ') {
            const char *q = line + 2;
            int s = (int)parse_long(&q);
            int d = (int)parse_long(&q);
            if (s == slot) d = 0;
            o += snprintf(out + o, 32, "S %d %d\n", s, d);
        } else {
            int n = (int)strlen(line);
            memcpy(out + o, line, (unsigned long)n);
            o += n;
            out[o++] = '\n';
        }
    }
    free(buf);
    write_named(IDX_NAME, out, (unsigned long)o);
    free(out);
}
