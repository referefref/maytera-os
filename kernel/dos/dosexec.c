// dosexec.c - MS-DOS real-mode program loader + runner (#201)
//
// Goal: run real-mode 16-bit MS-DOS games (e.g. THE INCREDIBLE MACHINE, TIM.EXE)
// in a window on MayteraOS, using the existing x86_16 real-mode interpreter
// (exec/x86_16.c). This file provides:
//   - an MZ .EXE loader (header parse + relocation) and a .COM loader
//   - a PSP at the load segment so DOS programs have a valid environment
//   - INT 21h: a usable DOS API subset (file I/O on FAT, memory, dir, exit, ...)
//   - INT 10h: VGA BIOS, in particular set-mode 13h (320x200x256)
//   - INT 33h: Microsoft mouse driver subset (from the kernel cursor)
//   - INT 16h / INT 21h key fns: keyboard from the kernel keyboard buffer
//   - I/O port hooks: VGA DAC palette (0x3C8/0x3C9) + status reads (0x3DA)
//   - a present loop: expand 0xA0000 (320x200x8) through the palette into a
//     2x-scaled ARGB host-window content buffer the compositor draws.
//
// The interpreter exposes a single global int handler + io handlers, and win16
// already owns those while a Win16 app runs. DOS and Win16 are mutually
// exclusive at runtime (one foreground 16-bit task), which matches how the OS
// launches them (own kernel proc, one at a time). We use a private cpu + memory.

#include "dosexec.h"
#include "dospath.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../exec/x86_16.h"

// ---- kernel imports ------------------------------------------------------
extern fat_fs_t g_fat_fs;
extern void *fat_read_file(fat_fs_t *fs, const char *path, uint32_t *size_out);
extern int  proc_create(const char *name, void (*entry)(void *), void *arg, int prio);
extern void proc_sleep(uint32_t ms);

struct window;
extern int  win16_host_create(const char *title, int x, int y, int w, int h,
                              uint32_t **out_buf, int *out_w, int *out_h,
                              struct window **out_win);
extern int  win16_host_content_rect(int slot, int *ox, int *oy, int *ow, int *oh);
extern void win16_host_destroy(int slot);

// Global kernel input state (drivers/mouse.c, drivers/keyboard.c).
extern int32_t mouse_x;
extern int32_t mouse_y;
extern uint8_t mouse_buttons;
extern int keyboard_has_char(void);
extern int keyboard_get_char(void);

// Raw scancode tap (cpu/isr.c) for DOS games (#202).
extern volatile int g_dos_scancode_tap;
extern int  dos_scancode_get(void);
extern void dos_scancode_clear(void);

extern void x86_16_request_stop(void);

// Forward decls for the mem-hook trampolines (defined below).
struct x86_16_cpu;
static void     ega_mem_w(struct x86_16_cpu *c, uint32_t lin, uint16_t val, int width);
static uint16_t ega_mem_r(struct x86_16_cpu *c, uint32_t lin, int width);

#ifndef PRIO_NORMAL
#define PRIO_NORMAL 2   // #385 real enum: IDLE=0,LOW=1,NORMAL=2,HIGH=3
#endif
#ifndef PRIO_HIGH
#define PRIO_HIGH 3
#endif

// ---- DOS task state ------------------------------------------------------
#define DOS_MEM_SIZE   0x100000          // 1 MiB real-mode address space
#define DOS_PSP_SEG    0x0100            // PSP paragraph (so image loads at 0x0110)
#define DOS_LOAD_SEG   (DOS_PSP_SEG + 0x10) // program load segment (PSP is 0x100 bytes)
#define VGA_A000       0xA0000           // mode-13h linear framebuffer base (linear)
#define VGA_A000_END   0xB0000           // end of the 64KB EGA aperture
#define MODE13_W       320
#define MODE13_H       200
#define WIN_SCALE      2                 // 320x200 -> 640x400 on screen

// EGA mode 0Dh (320x200x16, 4 planar bitplanes). Used by Commander Keen and
// other id Galaxy-engine games. Each plane is 64KB; a CPU byte at 0xA0000+off
// maps to bit (7-(x&7)) across the 4 planes for pixel x = off*8 + (7-bit).
#define EGA_PLANE_SIZE 0x10000           // 64KB per plane

#define DOS_MAX_FH     32

typedef struct {
    int        in_use;
    int        is_console;               // 0/1/2 stdio
    fat_file_t fat;                       // backing FAT file (when !is_console)
    uint32_t   pos;
    uint32_t   size;
    int        writable;
} dos_fh_t;

typedef struct {
    x86_16_cpu_t cpu;
    uint8_t     *mem;                     // 1 MiB
    dos_fh_t     fh[DOS_MAX_FH];
    char         appdir[128];             // dir of the .EXE for relative opens
    uint16_t     alloc_top_para;          // next free paragraph for INT 21h 48h

    // VGA / mode 13h
    int          video_mode;             // current INT 10h mode (0x13 = mode 13, 0x0D = EGA)
    int          gfx_w, gfx_h;           // active graphics resolution
    uint8_t      pal[256][3];             // 6-bit DAC palette (r,g,b 0..63)
    uint16_t     dac_widx, dac_ridx;      // DAC write/read index latches
    int          dac_phase;               // 0=r,1=g,2=b within a triplet

    // EGA planar framebuffer (mode 0Dh). 4 hidden bitplanes; CPU sees one
    // address space at 0xA0000 but writes/reads are filtered by the VGA
    // sequencer + graphics-controller registers.
    uint8_t      ega_plane[4][EGA_PLANE_SIZE];
    uint8_t      ega_latch[4];           // per-plane read latches
    uint8_t      seq_idx;                // 0x3C4 index latch
    uint8_t      seq_map_mask;           // SEQ reg 2: which planes a write targets
    uint8_t      gc_idx;                 // 0x3CE index latch
    uint8_t      gc_set_reset;           // GC reg 0
    uint8_t      gc_en_set_reset;        // GC reg 1
    uint8_t      gc_color_cmp;           // GC reg 2
    uint8_t      gc_data_rotate;         // GC reg 3 (rotate count + function bits 3-4)
    uint8_t      gc_read_map;            // GC reg 4: plane selected for reads (mode 0)
    uint8_t      gc_mode;                // GC reg 5 (write mode 0-3 in bits 0-1, read mode bit 3)
    uint8_t      gc_misc;                // GC reg 6
    uint8_t      gc_color_dont_care;     // GC reg 7
    uint8_t      gc_bit_mask;            // GC reg 8
    // Attribute controller (0x3C0): 16 EGA palette regs -> 6-bit colour index
    uint8_t      atc_idx;                // 0x3C0 index latch
    int          atc_flipflop;          // 0=index next, 1=data next
    uint8_t      atc_pal[16];            // EGA palette registers (index into DAC)
    // CRTC (0x3D4 index / 0x3D5 data, mono mirror 0x3B4/0x3B5). Backed as a
    // plain register file so VGA-detection read-after-write probes succeed.
    uint8_t      crtc_idx;
    uint8_t      crtc[32];
    uint8_t      misc_out;               // Misc Output register (0x3C2 write / 0x3CC read)
    uint8_t      seq_reg[8];             // full sequencer register file (for readback)
    int          ega_dirty;             // a plane write happened since last present

    // Keyboard hardware emulation for INT 9 delivery (#202 Keen).
    uint8_t      kbd_port60;            // last scancode latched at port 0x60
    int          kbd_has_int9;         // guest installed its own INT 9 vector
    int          has_int8;             // guest installed its own INT 8 (timer) vector
    uint32_t     int8_accum;           // accumulator for INT 8 rate division

    // DTA (Disk Transfer Address) for findfirst/findnext results
    uint16_t     dta_seg, dta_off;
    // findfirst/next iteration state
    fat_file_t   find_dir;
    int          find_active;
    char         find_pat[16];            // 8.3 uppercase pattern (with * / ?)

    // Mouse (INT 33h) state, in mode-13h virtual coords
    int          mouse_on;
    int          mx, my, mbtn;            // current
    int          mouse_initialized;

    // window host
    int          host_slot;
    uint32_t    *win_buf;                 // ARGB content buffer
    int          win_w, win_h;            // content buffer size

    volatile int running;
} dos_task_t;

static dos_task_t g_dos;                  // single foreground DOS task
static volatile int g_dos_busy = 0;

// Standard 16-colour EGA/VGA default palette (6-bit DAC values per the default
// attribute-controller mapping). Defined here so INT 10h mode-set + present share it.
static const uint8_t ega_default_dac[16][3] = {
    { 0, 0, 0},{ 0, 0,42},{ 0,42, 0},{ 0,42,42},
    {42, 0, 0},{42, 0,42},{42,21, 0},{42,42,42},
    {21,21,21},{21,21,63},{21,63,21},{21,63,63},
    {63,21,21},{63,21,63},{63,63,21},{63,63,63},
};

// ---- small helpers -------------------------------------------------------
static inline uint8_t  rd8 (dos_task_t *t, uint16_t s, uint16_t o){ return x86_16_rd8 (&t->cpu,s,o);}
static inline uint16_t rd16(dos_task_t *t, uint16_t s, uint16_t o){ return x86_16_rd16(&t->cpu,s,o);}
static inline void     wr8 (dos_task_t *t, uint16_t s, uint16_t o, uint8_t v){ x86_16_wr8 (&t->cpu,s,o,v);}
static inline void     wr16(dos_task_t *t, uint16_t s, uint16_t o, uint16_t v){ x86_16_wr16(&t->cpu,s,o,v);}

#define SET_CF(c)   ((c)->flags |= 0x0001)
#define CLR_CF(c)   ((c)->flags &= ~0x0001)
#define AH_SET(c,v) ((c)->ax = (uint16_t)(((c)->ax & 0x00FF) | ((v) << 8)))
#define AL_SET(c,v) ((c)->ax = (uint16_t)(((c)->ax & 0xFF00) | ((v) & 0xFF)))
#define AH(c)       ((uint8_t)((c)->ax >> 8))
#define AL(c)       ((uint8_t)((c)->ax & 0xFF))

// Read an ASCIIZ string from guest memory into a host buffer.
static void rd_asciiz(dos_task_t *t, uint16_t seg, uint16_t off, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        uint8_t ch = rd8(t, seg, (uint16_t)(off + i));
        if (ch == 0) break;
        out[i] = (char)ch;
    }
    out[i] = '\0';
}

// Convert a DOS path ("C:\FOO\BAR.EXE" or "FOO.DAT") to a native FS path via the
// shared drive-letter FS layer (#257): explicit "X:" -> /WINDIR/DRIVE_X, a bare
// relative name -> the app directory (legacy behavior), a native "/" path is
// passed through. dos_resolve_path uppercases the result for the 8.3/ext2 root.
extern void dos_resolve_path(const char *in, const char *reldir, char *out, int outsz);
static void dos_to_fat_path(dos_task_t *t, const char *in, char *out, int max) {
    dos_resolve_path(in, t->appdir, out, max);
}

// ---- file handle table ---------------------------------------------------
static int dos_fh_alloc(dos_task_t *t) {
    for (int i = 5; i < DOS_MAX_FH; i++) if (!t->fh[i].in_use) return i;
    return -1;
}

static char dos_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

// Match a FAT 8.3 short name (e.g. "RESOURCE001" 11 chars from dir entry,
// space-padded) against a DOS wildcard pattern like "RESOURCE.*" or "*.001".
// We compare on the "NAME.EXT" rendered form, case-insensitively, with * and ?.
static int dos_wild_match(const char *pat, const char *name) {
    // both are NUL-terminated "NAME.EXT" forms here
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;             // trailing * matches rest
            // try to match remaining pattern at each position
            while (*name) {
                if (dos_wild_match(pat, name)) return 1;
                name++;
            }
            return dos_wild_match(pat, name); // allow * = empty at end
        } else if (*pat == '?') {
            if (!*name) return 0;
            pat++; name++;
        } else {
            if (dos_upper(*pat) != dos_upper(*name)) return 0;
            pat++; name++;
        }
    }
    return (*name == '\0');
}

// Render an 11-byte FAT entry name into "NAME.EXT".
static void fat11_to_dotname(const uint8_t *raw, char *out) {
    int o = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[o++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) out[o++] = (char)raw[i];
    }
    out[o] = '\0';
}

// Write a DOS find result into the guest DTA (43-byte FindFirst structure).
static void dos_write_find_result(dos_task_t *t, const char *dotname, uint32_t fsize, uint8_t attr) {
    uint16_t s = t->dta_seg, o = t->dta_off;
    wr8(t, s, (uint16_t)(o + 0x15), attr);
    wr16(t, s, (uint16_t)(o + 0x16), 0);        // time
    wr16(t, s, (uint16_t)(o + 0x18), 0);        // date
    wr16(t, s, (uint16_t)(o + 0x1A), (uint16_t)(fsize & 0xFFFF));
    wr16(t, s, (uint16_t)(o + 0x1C), (uint16_t)(fsize >> 16));
    int i = 0;
    for (; dotname[i] && i < 12; i++) wr8(t, s, (uint16_t)(o + 0x1E + i), (uint8_t)dotname[i]);
    wr8(t, s, (uint16_t)(o + 0x1E + i), 0);
}

// Advance the active find iteration to the next matching entry.
// Returns 0 and fills DTA on match, -1 on end-of-dir.
static int dos_find_step(dos_task_t *t) {
    if (!t->find_active) return -1;
    fat_dir_entry_t e;
    char namebuf[16];
    while (fat_readdir(&t->find_dir, &e, namebuf) == 0) {
        if (e.name[0] == 0x00) break;            // end of directory
        if ((uint8_t)e.name[0] == 0xE5) continue; // deleted
        if (e.attr & 0x08) continue;             // volume label
        if (e.attr & 0x10) continue;             // skip subdirs for simplicity
        char dot[16];
        fat11_to_dotname(e.name, dot);
        if (dot[0] == 0) continue;
        if (dos_wild_match(t->find_pat, dot)) {
            dos_write_find_result(t, dot, e.file_size, e.attr);
            return 0;
        }
    }
    t->find_active = 0;
    fat_close(&t->find_dir);
    return -1;
}

// ---- DOS INT 21h ---------------------------------------------------------
extern volatile int g_x86_dbgring;   // #385: reuse DOSDIAG.CFG gate for verbose keen traces
static int g_dos_trace21 = 0;   // #385 diag   // #202 bring-up: log every INT 21h call (off for ship)
static volatile int g_dos_sstep = 0;   // #202: single-step N instructions when >0
static void int21(dos_task_t *t) {
    x86_16_cpu_t *c = &t->cpu;
    uint8_t ah = AH(c);
    CLR_CF(c);
    if (g_dos_trace21)
        kprintf("[dos] INT21 AH=%02x al=%02x bx=%04x cx=%04x dx=%04x ds=%04x es=%04x cs:ip=%04x:%04x\n",
                ah, AL(c), c->bx, c->cx, c->dx, c->ds, c->es, c->cs, c->ip);

    switch (ah) {
    case 0x02:  // display character (DL)
        serial_write(COM1, (char)(c->dx & 0xFF));
        AL_SET(c, c->dx & 0xFF);
        break;

    case 0x06:  // direct console I/O
        if ((c->dx & 0xFF) == 0xFF) {
            if (keyboard_has_char()) { int ch = keyboard_get_char();
                AL_SET(c, ch & 0xFF); c->flags &= ~0x0040; }
            else { AL_SET(c, 0); c->flags |= 0x0040; }   // ZF=1 -> no char
        } else {
            serial_write(COM1, (char)(c->dx & 0xFF));
        }
        break;

    case 0x09: { // print $-terminated string at DS:DX
        char b[256];
        int i = 0;
        for (; i < (int)sizeof(b) - 1; i++) {
            uint8_t ch = rd8(t, c->ds, (uint16_t)(c->dx + i));
            if (ch == '$') break;
            b[i] = (char)ch;
        }
        b[i] = '\0';
        serial_puts(COM1, b);
        break;
    }

    case 0x0B:  // check stdin status
        AL_SET(c, keyboard_has_char() ? 0xFF : 0x00);
        break;

    case 0x0E:  // select default drive: DL=drive (0=A,2=C,4=E) -> AL=#drives (#257)
        dos_set_current_drive((char)('A' + (uint8_t)(c->dx & 0xFF)));
        AL_SET(c, (uint8_t)dos_drive_count());
        break;

    case 0x19:  // get current default drive -> AL (0=A,2=C,4=E) (#257)
        AL_SET(c, (uint8_t)(dos_current_drive() - 'A'));
        break;

    case 0x1A:  // set DTA (DS:DX)
        t->dta_seg = c->ds;
        t->dta_off = c->dx;
        break;

    case 0x25:  // set interrupt vector (AL=int, DS:DX=handler) -> write the IVT
        {
            uint8_t vec = AL(c);
            wr16(t, 0x0000, (uint16_t)(vec * 4),     c->dx);   // offset
            wr16(t, 0x0000, (uint16_t)(vec * 4 + 2), c->ds);   // segment
            if (vec == 0x09) {
                t->kbd_has_int9 = 1;
                g_dos_scancode_tap = 1;     // start mirroring raw scancodes
                dos_scancode_clear();
                kprintf("[dos] guest installed INT 9 -> %04x:%04x (raw kbd enabled)\n", c->ds, c->dx);
            } else if (vec == 0x08) {
                t->has_int8 = 1;            // guest timer ISR -> we must deliver IRQ0
                kprintf("[dos] guest installed INT 8 -> %04x:%04x (timer ticks enabled)\n", c->ds, c->dx);
            }
        }
        break;

    case 0x2A:  // get system date -> CX=year DH=month DL=day AL=dow
        c->cx = 1992; AH_SET(c, 11); /* DH/DL */ c->dx = (11 << 8) | 19; AL_SET(c, 4);
        break;

    case 0x2C:  // get system time -> CH:CL hour:min DH:DL sec:hundredths
        c->cx = 0; c->dx = 0;
        break;

    case 0x30:  // get DOS version -> AL=major BH=OEM
        c->ax = 0x0005;          // DOS 5.0 (AL=5, AH=0)
        c->bx = 0xFF00;
        c->cx = 0;
        break;

    case 0x35:  // get interrupt vector AL -> ES:BX from the IVT
        {
            uint8_t vec = AL(c);
            c->bx = rd16(t, 0x0000, (uint16_t)(vec * 4));
            c->es = rd16(t, 0x0000, (uint16_t)(vec * 4 + 2));
        }
        break;

    case 0x3C: { // create/truncate file DS:DX, attr CX -> AX=handle
        char dp[160], fp[160];
        rd_asciiz(t, c->ds, c->dx, dp, sizeof(dp));
        if (!dos_path_writable(dp)) { c->ax = 5; SET_CF(c); break; }  // (#257) E: is read-only
        dos_to_fat_path(t, dp, fp, sizeof(fp));
        // FAT write/create not supported here; allocate handle, mark writable.
        int h = dos_fh_alloc(t);
        if (h < 0) { c->ax = 4; SET_CF(c); break; }
        memset(&t->fh[h], 0, sizeof(t->fh[h]));
        t->fh[h].in_use = 1; t->fh[h].writable = 1;
        c->ax = (uint16_t)h;
        kprintf("[dos] 3Ch create '%s' -> h%d (write stub)\n", fp, h);
        break;
    }

    case 0x3D: { // open file DS:DX, AL=mode -> AX=handle / err
        char dp[160], fp[160];
        rd_asciiz(t, c->ds, c->dx, dp, sizeof(dp));
        dos_to_fat_path(t, dp, fp, sizeof(fp));
        int h = dos_fh_alloc(t);
        if (h < 0) { c->ax = 4; SET_CF(c); break; }
        dos_fh_t *fh = &t->fh[h];
        memset(fh, 0, sizeof(*fh));
        if (fat_open(&g_fat_fs, fp, &fh->fat) != 0) {
            c->ax = 2; SET_CF(c);   // file not found
            kprintf("[dos] 3Dh open FAIL '%s'\n", fp);
            // #202 diag: dump the caller's code so we can see how it branches on CF.
            {
                char hb[200]; int hp = 0; const char *hx = "0123456789abcdef";
                for (int k = 0; k < 48 && hp < (int)sizeof(hb) - 4; k++) {
                    uint8_t bv = rd8(t, c->cs, (uint16_t)(c->ip + k));
                    hb[hp++] = hx[bv >> 4]; hb[hp++] = hx[bv & 0xF]; hb[hp++] = ' ';
                }
                hb[hp] = 0;
                kprintf("[dos]   caller code@%04x:%04x flags=%04x: %s\n", c->cs, c->ip, c->flags, hb);
            }
            break;
        }
        fh->in_use = 1;
        fh->size = fh->fat.file_size;
        fh->pos = 0;
        c->ax = (uint16_t)h;
        kprintf("[dos] 3Dh open '%s' -> h%d size=%u\n", fp, h, fh->size);
        // #202 bring-up: arm single-step from the GAMEMAPS open (just before the
        // current derail during graphics/map decompression).
        {
            int isgm = 0;
            for (const char *q = fp; *q; q++)
                if ((q[0]=='G'||q[0]=='g') && (q[1]=='A'||q[1]=='a') && (q[2]=='M'||q[2]=='m')) { isgm = 1; break; }
            (void)isgm;   // single-step derail tracer disabled for ship
        }
        break;
    }

    case 0x3E: { // close handle BX
        uint16_t h = c->bx;
        if (h < DOS_MAX_FH && t->fh[h].in_use && !t->fh[h].is_console) {
            fat_close(&t->fh[h].fat);
        }
        if (h < DOS_MAX_FH) t->fh[h].in_use = 0;
        break;
    }

    case 0x3F: { // read CX bytes from handle BX to DS:DX -> AX=bytes read
        uint16_t h = c->bx, len = c->cx;
        if (h >= DOS_MAX_FH || (!t->fh[h].in_use && h > 4)) { c->ax = 6; SET_CF(c); break; }
        if (h == 0) {   // stdin from keyboard
            uint16_t got = 0;
            while (got < len && keyboard_has_char()) {
                int ch = keyboard_get_char();
                wr8(t, c->ds, (uint16_t)(c->dx + got), (uint8_t)ch);
                got++;
                if (ch == '\r') break;
            }
            c->ax = got; break;
        }
        dos_fh_t *fh = &t->fh[h];
        if (fh->is_console) { c->ax = 0; break; }
        if (fh->fat.position != fh->pos) fat_seek(&fh->fat, fh->pos);
        // Read in chunks into guest memory.
        static uint8_t buf[4096];
        uint16_t total = 0;
        while (total < len) {
            uint32_t remain = (uint32_t)(len - total);
            uint16_t want = (uint16_t)(remain > sizeof(buf) ? sizeof(buf) : remain);
            int r = fat_read(&fh->fat, buf, want);
            if (r <= 0) break;
            for (int i = 0; i < r; i++)
                wr8(t, c->ds, (uint16_t)(c->dx + total + i), buf[i]);
            total += (uint16_t)r;
            fh->pos += (uint32_t)r;
            if (r < (int)want) break;
        }
        c->ax = total;
        kprintf("[dos] 3Fh read h%u want=%u got=%u pos=%u\n", h, len, total, fh->pos);
        break;
    }

    case 0x40: { // write CX bytes from DS:DX to handle BX -> AX=written
        uint16_t h = c->bx, len = c->cx;
        if (h == 1 || h == 2) {  // stdout/stderr -> serial
            for (uint16_t i = 0; i < len; i++)
                serial_write(COM1, (char)rd8(t, c->ds, (uint16_t)(c->dx + i)));
            c->ax = len; break;
        }
        // file write unsupported; pretend success so apps proceed
        c->ax = len;
        break;
    }

    case 0x42: { // lseek handle BX, CX:DX offset, AL=whence -> DX:AX position
        uint16_t h = c->bx;
        if (h >= DOS_MAX_FH || !t->fh[h].in_use) { c->ax = 6; SET_CF(c); break; }
        dos_fh_t *fh = &t->fh[h];
        int32_t off = (int32_t)(((uint32_t)c->cx << 16) | c->dx);
        uint32_t np;
        if (AL(c) == 0)      np = (uint32_t)off;                  // SEEK_SET
        else if (AL(c) == 1) np = fh->pos + (uint32_t)off;        // SEEK_CUR
        else                 np = fh->size + (uint32_t)off;       // SEEK_END
        fh->pos = np;
        if (!fh->is_console) fat_seek(&fh->fat, np);
        c->ax = (uint16_t)(np & 0xFFFF);
        c->dx = (uint16_t)(np >> 16);
        break;
    }

    case 0x43: { // get/set file attributes (DS:DX path). AL=0 get, AL=1 set.
        char dp[160], fp[160];
        rd_asciiz(t, c->ds, c->dx, dp, sizeof(dp));
        dos_to_fat_path(t, dp, fp, sizeof(fp));
        if (AL(c) == 1) { CLR_CF(c); break; }   // set: accept silently
        // get: probe existence by opening.
        fat_file_t probe;
        if (fat_open(&g_fat_fs, fp, &probe) == 0) {
            fat_close(&probe);
            c->cx = 0x20;   // archive (normal file)
            CLR_CF(c);
        } else {
            c->ax = 2; SET_CF(c);   // not found
            kprintf("[dos] 43h getattr MISS '%s'\n", fp);
        }
        break;
    }

    case 0x44:  // IOCTL: report char device for stdin/stdout (AL=0 get info)
        if (AL(c) == 0) { c->dx = (c->bx <= 2) ? 0x0080 : 0x0000; }
        break;

    case 0x47: { // get current directory -> DS:SI = ASCIIZ (no drive, no leading \)
        // Return the app directory minus leading slash, backslash-separated.
        uint16_t off = c->si;
        const char *d = t->appdir;
        if (*d == '/') d++;
        int i = 0;
        for (; d[i]; i++)
            wr8(t, c->ds, (uint16_t)(off + i), (uint8_t)(d[i] == '/' ? '\\' : d[i]));
        wr8(t, c->ds, (uint16_t)(off + i), 0);
        c->ax = 0x0100;
        CLR_CF(c);
        break;
    }

    case 0x48: { // allocate BX paragraphs -> AX=segment ; on fail BX=largest avail
        uint16_t para = c->bx;
        uint16_t avail = (uint16_t)(0xA000 - t->alloc_top_para);  // free below VGA (0xA000=640KB)
        if (para > avail) {
            c->ax = 8; SET_CF(c);   // insufficient memory
            c->bx = avail;          // report largest available
            if (g_x86_dbgring) kprintf("[dos] 48h alloc req=%04x FAIL avail=%04x top=%04x ss=%04x\n", para, avail, t->alloc_top_para, c->ss);
            break;
        }
        c->ax = t->alloc_top_para;
        t->alloc_top_para = (uint16_t)(t->alloc_top_para + para);
        if (g_x86_dbgring) kprintf("[dos] 48h alloc req=%04x -> seg=%04x newtop=%04x ss=%04x\n", para, c->ax, t->alloc_top_para, c->ss);
        break;
    }

    case 0x49:  // free memory (ES) - no-op (bump allocator)
        if (g_x86_dbgring) kprintf("[dos] 49h free es=%04x (no-op)\n", c->es);
        CLR_CF(c);
        break;

    case 0x4A: { // resize block (ES, BX paragraphs). Report max free on shrink/grow.
        // We always "succeed" but also expose the conventional-memory ceiling so
        // the program's startup (which resizes its PSP block to the whole arena)
        // learns how much memory it really has.
        uint16_t maxpara = (uint16_t)(0xA000 - c->es);
        if (g_x86_dbgring) kprintf("[dos] 4Ah resize es=%04x req=%04x maxpara=%04x top=%04x ss=%04x\n", c->es, c->bx, maxpara, t->alloc_top_para, c->ss);
        if (c->bx > maxpara) {
            // grow request beyond arena: deny + report max (DOS convention),
            // many runtimes then retry with the reported size.
            c->ax = 8; SET_CF(c);
            c->bx = maxpara;
        } else {
            CLR_CF(c);
        }
        break;
    }

    case 0x4C:  // terminate with return code AL
        c->exit_code = AL(c);
        c->halted = 1;
        kprintf("[dos] INT 21h 4Ch exit code=%d\n", c->exit_code);
        break;

    case 0x4E: { // find first matching file (DS:DX spec, CX attr mask) -> DTA
        char dp[160], fp[160];
        rd_asciiz(t, c->ds, c->dx, dp, sizeof(dp));
        dos_to_fat_path(t, dp, fp, sizeof(fp));
        // Split fp into directory + filename pattern.
        char dirpath[160]; int slash = -1;
        for (int i = 0; fp[i]; i++) if (fp[i] == '/') slash = i;
        if (slash < 0) { dirpath[0] = '/'; dirpath[1] = '\0'; strncpy(t->find_pat, fp, sizeof(t->find_pat) - 1); }
        else {
            int n = slash > 0 ? slash : 1;
            for (int i = 0; i < n; i++) dirpath[i] = fp[i];
            dirpath[n] = '\0';
            strncpy(t->find_pat, fp + slash + 1, sizeof(t->find_pat) - 1);
        }
        t->find_pat[sizeof(t->find_pat) - 1] = '\0';
        if (t->find_active) { fat_close(&t->find_dir); t->find_active = 0; }
        if (fat_open(&g_fat_fs, dirpath, &t->find_dir) != 0) {
            c->ax = 18; SET_CF(c);
            kprintf("[dos] 4Eh findfirst dir MISS '%s'\n", dirpath);
            break;
        }
        t->find_active = 1;
        if (dos_find_step(t) == 0) { CLR_CF(c); c->ax = 0; }
        else { c->ax = 18; SET_CF(c); }
        kprintf("[dos] 4Eh findfirst dir='%s' pat='%s' -> %s\n",
                dirpath, t->find_pat, (c->flags & 0x0001) ? "none" : "hit");
        break;
    }
    case 0x4F:  // find next -> DTA or CF
        if (dos_find_step(t) == 0) { CLR_CF(c); c->ax = 0; }
        else { c->ax = 18; SET_CF(c); }
        break;

    case 0x62:  // get PSP segment -> BX
        c->bx = DOS_PSP_SEG;
        break;

    default:
        kprintf("[dos] INT 21h AH=%02x (unhandled) ax=%04x bx=%04x cx=%04x dx=%04x\n",
                ah, c->ax, c->bx, c->cx, c->dx);
        break;
    }
}

// ---- INT 10h (VGA BIOS) --------------------------------------------------
static void int10(dos_task_t *t) {
    x86_16_cpu_t *c = &t->cpu;
    uint8_t ah = AH(c);
    switch (ah) {
    case 0x00: {  // set video mode AL
        uint8_t m = AL(c) & 0x7F;   // bit7 = "don't clear memory"
        t->video_mode = m;
        kprintf("[dos] INT 10h set mode 0x%02x\n", m);
        if (m == 0x13) {
            t->gfx_w = MODE13_W; t->gfx_h = MODE13_H;
            for (int i = 0; i < MODE13_W * MODE13_H; i++)
                t->mem[VGA_A000 + i] = 0;
        } else if (m == 0x0D || m == 0x0E || m == 0x10 || m == 0x12) {
            // EGA/VGA planar graphics modes.
            if (m == 0x0D)      { t->gfx_w = 320; t->gfx_h = 200; }
            else if (m == 0x0E) { t->gfx_w = 640; t->gfx_h = 200; }
            else if (m == 0x10) { t->gfx_w = 640; t->gfx_h = 350; }
            else                { t->gfx_w = 640; t->gfx_h = 480; }
            // clear all 4 planes; reset VGA register state to power-on defaults.
            for (int p = 0; p < 4; p++) memset(t->ega_plane[p], 0, EGA_PLANE_SIZE);
            t->seq_map_mask = 0x0F;
            t->gc_set_reset = t->gc_en_set_reset = 0;
            t->gc_data_rotate = 0; t->gc_read_map = 0; t->gc_mode = 0;
            t->gc_bit_mask = 0xFF; t->gc_color_dont_care = 0x0F;
            // seed the 16 attribute-controller palette regs with the EGA default
            // and matching DAC entries so something is visible immediately.
            for (int i = 0; i < 16; i++) {
                t->atc_pal[i] = (uint8_t)i;
                t->pal[i][0] = ega_default_dac[i][0];
                t->pal[i][1] = ega_default_dac[i][1];
                t->pal[i][2] = ega_default_dac[i][2];
            }
            t->ega_dirty = 1;
        }
        break;
    }
    case 0x0F:  // get video mode -> AL=mode, AH=cols
        AL_SET(c, t->video_mode);
        AH_SET(c, 40);
        break;

    case 0x12:  // alternate select / EGA-VGA info
        if (AL(c) == 0x10 || (c->bx & 0xFF) == 0x10) {
            // get EGA info: BH=colour mode (0), BL=memory (3=256KB), CX=switches/feature
            c->bx = (c->bx & 0xFF00) | 0x0003;   // BH=0 (colour), BL=3 (256KB)
            c->cx = 0x0009;
        }
        // other AL subfunctions: accept silently
        break;

    case 0x1A:  // display combination code (VGA BIOS)
        if (AL(c) == 0x00) {
            AL_SET(c, 0x1A);            // function supported
            c->bx = 0x0008;             // BL=8 (VGA colour analog), BH=0 (none)
        }
        break;

    case 0x1B:  // get functionality/state info -> AL=1B if supported (report not)
        break;
    case 0x10:  // palette / DAC functions
        if (AL(c) == 0x00) {        // set single EGA palette reg: BL=reg, BH=value
            uint8_t reg = (uint8_t)(c->bx & 0x0F);
            t->atc_pal[reg] = (uint8_t)((c->bx >> 8) & 0x3F);
        } else if (AL(c) == 0x02) { // set all 16 EGA palette regs + overscan from ES:DX (17 bytes)
            for (int i = 0; i < 16; i++)
                t->atc_pal[i] = rd8(t, c->es, (uint16_t)(c->dx + i)) & 0x3F;
        } else if (AL(c) == 0x10) {        // set single DAC register: BX=index, DH=r DL? -> CH=g CL=b DH=r
            uint16_t idx = c->bx & 0xFF;
            t->pal[idx][0] = (uint8_t)(c->dx >> 8) & 0x3F;   // DH = red
            t->pal[idx][1] = (uint8_t)(c->cx >> 8) & 0x3F;   // CH = green
            t->pal[idx][2] = (uint8_t)(c->cx & 0xFF) & 0x3F; // CL = blue
        } else if (AL(c) == 0x12) { // set block of DAC: BX=start, CX=count, ES:DX=table(3 bytes each)
            uint16_t start = c->bx, count = c->cx;
            for (uint16_t i = 0; i < count && (start + i) < 256; i++) {
                uint16_t o = (uint16_t)(c->dx + i * 3);
                t->pal[start + i][0] = rd8(t, c->es, o)     & 0x3F;
                t->pal[start + i][1] = rd8(t, c->es, (uint16_t)(o + 1)) & 0x3F;
                t->pal[start + i][2] = rd8(t, c->es, (uint16_t)(o + 2)) & 0x3F;
            }
        }
        break;
    default:
        // many INT 10h fns (cursor, teletype) are harmless to ignore
        break;
    }
}

// ---- INT 33h (mouse) -----------------------------------------------------
static void int33(dos_task_t *t) {
    x86_16_cpu_t *c = &t->cpu;
    switch (c->ax) {
    case 0x0000: // reset / installed?
        c->ax = 0xFFFF;   // installed
        c->bx = 2;        // 2 buttons
        t->mouse_initialized = 1;
        break;
    case 0x0001: t->mouse_on = 1; break;  // show cursor
    case 0x0002: t->mouse_on = 0; break;  // hide cursor
    case 0x0003: // get position/buttons -> CX=x DX=y BX=buttons
        c->cx = (uint16_t)t->mx;
        c->dx = (uint16_t)t->my;
        c->bx = (uint16_t)t->mbtn;
        break;
    case 0x0004: // set position CX=x DX=y
        t->mx = c->cx; t->my = c->dx;
        break;
    case 0x0007: case 0x0008: // set horiz/vert range -> ignore
        break;
    case 0x000B: // read motion counters -> CX=dx DX=dy (return 0)
        c->cx = 0; c->dx = 0;
        break;
    default:
        break;
    }
}

// ---- INT 16h (keyboard BIOS) ---------------------------------------------
static void int16(dos_task_t *t) {
    x86_16_cpu_t *c = &t->cpu;
    uint8_t ah = AH(c);
    switch (ah) {
    case 0x00: case 0x10: // read key (blocking) -> AL=ascii AH=scan
        if (keyboard_has_char()) { int ch = keyboard_get_char(); c->ax = (uint16_t)(ch & 0xFF); }
        else { c->ax = 0; }
        break;
    case 0x01: case 0x11: // check key -> ZF set if none
        if (keyboard_has_char()) { c->flags &= ~0x0040; c->ax = 0x0000; }
        else { c->flags |= 0x0040; c->ax = 0; }
        break;
    case 0x02: // shift status
        AL_SET(c, 0);
        break;
    default:
        break;
    }
}

// Master interrupt dispatcher for the DOS task.
static int dos_int_handler(x86_16_cpu_t *c, uint8_t intno) {
    dos_task_t *t = &g_dos;   // single foreground task
    (void)c;
    switch (intno) {
    case 0x20: t->cpu.halted = 1; t->cpu.exit_code = 0; return 0;  // legacy terminate
    case 0x21: int21(t); return 0;
    case 0x10: int10(t); return 0;
    case 0x33: int33(t); return 0;
    case 0x16: int16(t); return 0;
    case 0x1A: t->cpu.cx = 0; t->cpu.dx = 0; return 0;  // timer tick count
    case 0x67: // EMS (LIM) - report "not installed" so the app uses conventional mem
        AH_SET(c, 0x80);   // status: EMM software not present / general failure
        return 0;
    case 0x2F: // multiplex / XMS install check
        // AX=4300h: XMS driver install check. Real HIMEM returns AL=0x80. We
        // have no XMS, so return AL!=0x80 to report "not installed" (Keen then
        // falls back to conventional memory). All other 2Fh calls: not handled.
        if (c->ax == 0x4300) { AL_SET(c, 0x00); }
        return 0;
    default:
        // ignore other interrupts (vectors, DOS internal)
        kprintf("[dos] INT %02xh ax=%04x bx=%04x (ignored)\n", intno, c->ax, c->bx);
        return 0;
    }
}

// ---- EGA planar framebuffer (mode 0Dh) -----------------------------------
// Standard VGA/EGA write-mode + read-mode logic. The CPU writes a single byte
// to 0xA0000+off; the sequencer Map Mask + graphics-controller registers fan it
// out across the 4 hidden bitplanes. Commander Keen's renderer uses write mode 0
// (with map mask / set-reset for solid colours and bit mask for masked sprites)
// plus write mode 1 (latch copy) for fast plane-to-plane block copies/scrolling.

static uint8_t ega_rotate(dos_task_t *t, uint8_t v) {
    uint8_t rot = t->gc_data_rotate & 0x07;
    if (rot) v = (uint8_t)((v >> rot) | (v << (8 - rot)));
    return v;
}

// Apply the GC logical-operation (data rotate reg bits 3-4) between the CPU/ALU
// value and the corresponding plane latch.
static uint8_t ega_alu(dos_task_t *t, uint8_t val, uint8_t latch) {
    switch ((t->gc_data_rotate >> 3) & 0x03) {
        case 1: return (uint8_t)(val & latch);
        case 2: return (uint8_t)(val | latch);
        case 3: return (uint8_t)(val ^ latch);
        default: return val;
    }
}

// Write a CPU byte to the EGA aperture at linear address `lin` (off into plane).
static void ega_write(dos_task_t *t, uint32_t lin, uint8_t cpu_val) {
    uint32_t off = lin - VGA_A000;
    if (off >= EGA_PLANE_SIZE) return;
    uint8_t wmode = t->gc_mode & 0x03;
    uint8_t bitmask = t->gc_bit_mask;
    t->ega_dirty = 1;

    for (int p = 0; p < 4; p++) {
        if (!(t->seq_map_mask & (1 << p))) continue;  // map mask gates writes per plane
        uint8_t latch = t->ega_latch[p];
        uint8_t res;
        switch (wmode) {
        case 0: {
            // write mode 0: rotate CPU value, then for planes whose enable-set/reset
            // bit is set use the set/reset colour byte (all 0s or all 1s) instead.
            uint8_t v;
            if (t->gc_en_set_reset & (1 << p))
                v = (t->gc_set_reset & (1 << p)) ? 0xFF : 0x00;
            else
                v = ega_rotate(t, cpu_val);
            v = ega_alu(t, v, latch);
            res = (uint8_t)((v & bitmask) | (latch & ~bitmask));
            break;
        }
        case 1:
            // write mode 1: copy the latches straight through (CPU value ignored).
            res = latch;
            break;
        case 2: {
            // write mode 2: each plane gets bit (cpu_val>>p)&1 expanded to 0x00/0xFF.
            uint8_t v = (cpu_val & (1 << p)) ? 0xFF : 0x00;
            v = ega_alu(t, v, latch);
            res = (uint8_t)((v & bitmask) | (latch & ~bitmask));
            break;
        }
        case 3: {
            // write mode 3: rotated CPU value ANDed with bit mask forms the mask;
            // colour comes from set/reset. (Rarely used by Keen but cheap to add.)
            uint8_t v = ega_rotate(t, cpu_val) & bitmask;
            uint8_t col = (t->gc_set_reset & (1 << p)) ? 0xFF : 0x00;
            res = (uint8_t)((col & v) | (latch & ~v));
            break;
        }
        default: res = latch; break;
        }
        t->ega_plane[p][off] = res;
    }
}

// Read a CPU byte from the EGA aperture. Always reloads all 4 latches (real
// hardware latches every plane on any read), then returns per the read mode.
static uint8_t ega_read(dos_task_t *t, uint32_t lin) {
    uint32_t off = lin - VGA_A000;
    if (off >= EGA_PLANE_SIZE) return 0xFF;
    for (int p = 0; p < 4; p++) t->ega_latch[p] = t->ega_plane[p][off];
    if (t->gc_mode & 0x08) {
        // read mode 1: colour-compare. Each result bit set where all planes
        // (masked by color-dont-care) match the color-compare register.
        uint8_t result = 0;
        for (int b = 0; b < 8; b++) {
            int match = 1;
            for (int p = 0; p < 4; p++) {
                if (!(t->gc_color_dont_care & (1 << p))) continue;
                int planebit = (t->ega_plane[p][off] >> b) & 1;
                int cmpbit   = (t->gc_color_cmp >> p) & 1;
                if (planebit != cmpbit) { match = 0; break; }
            }
            if (match) result |= (1 << b);
        }
        return result;
    }
    // read mode 0: return the plane selected by GC read-map.
    return t->ega_plane[t->gc_read_map & 3][off];
}

// Mem-hook trampolines registered with the interpreter.
static void ega_mem_w(x86_16_cpu_t *c, uint32_t lin, uint16_t val, int width) {
    (void)c;
    dos_task_t *t = &g_dos;
    if (t->video_mode == 0x13) {
        // Mode 13h is a plain linear byte buffer; write straight to mem[].
        if (lin < DOS_MEM_SIZE) t->mem[lin] = (uint8_t)(val & 0xFF);
        if (width == 2 && lin + 1 < DOS_MEM_SIZE) t->mem[lin + 1] = (uint8_t)(val >> 8);
        return;
    }
    ega_write(t, lin, (uint8_t)(val & 0xFF));
    if (width == 2) ega_write(t, lin + 1, (uint8_t)(val >> 8));
}
static uint16_t ega_mem_r(x86_16_cpu_t *c, uint32_t lin, int width) {
    (void)c;
    dos_task_t *t = &g_dos;
    if (t->video_mode == 0x13) {
        uint16_t v = (lin < DOS_MEM_SIZE) ? t->mem[lin] : 0xFF;
        if (width == 2) v |= (uint16_t)((lin + 1 < DOS_MEM_SIZE ? t->mem[lin + 1] : 0xFF) << 8);
        return v;
    }
    uint16_t v = ega_read(t, lin);
    if (width == 2) v |= (uint16_t)(ega_read(t, lin + 1) << 8);
    return v;
}

// ---- I/O port hooks (VGA DAC + status) -----------------------------------
static uint16_t dos_in(x86_16_cpu_t *c, uint16_t port, int width) {
    (void)c; (void)width;
    dos_task_t *t = &g_dos;
    if (port == 0x3DA || port == 0x3BA) {
        // VGA input status #1. Reading it resets the attribute-controller
        // flip-flop. We toggle a broad set of bits (display-enable 0x01,
        // vertical-retrace 0x08, plus 0x80) on every read so any "wait for the
        // status to change / wait for retrace" polling loop makes progress and
        // exits. id's VGA-detection routine watches bit 0x80 toggle.
        t->atc_flipflop = 0;
        static uint8_t tog = 0;
        tog = (uint8_t)(tog ^ 0x89);
        return tog;
    }
    if (port == 0x3C9) {   // DAC data read
        uint8_t v = t->pal[t->dac_ridx & 0xFF][t->dac_phase];
        t->dac_phase++;
        if (t->dac_phase >= 3) { t->dac_phase = 0; t->dac_ridx++; }
        return v;
    }
    if (port == 0x3CF) {   // graphics-controller data read
        switch (t->gc_idx) {
        case 0: return t->gc_set_reset;
        case 1: return t->gc_en_set_reset;
        case 2: return t->gc_color_cmp;
        case 3: return t->gc_data_rotate;
        case 4: return t->gc_read_map;
        case 5: return t->gc_mode;
        case 6: return t->gc_misc;
        case 7: return t->gc_color_dont_care;
        case 8: return t->gc_bit_mask;
        }
        return 0xFF;
    }
    if (port == 0x3C4) return t->seq_idx;
    if (port == 0x3CE) return t->gc_idx;
    if (port == 0x3D4 || port == 0x3B4) return t->crtc_idx;
    if (port == 0x3D5 || port == 0x3B5)            // CRTC data: read back the register file
        return t->crtc[t->crtc_idx & 0x1F];
    if (port == 0x3CC || port == 0x3C2) return t->misc_out;  // Misc Output read
    if (port == 0x3C5) { if (t->seq_idx < 8) return t->seq_reg[t->seq_idx]; return 0xFF; }
    if (port == 0x60) return t->kbd_port60;       // keyboard data port (last scancode)
    if (port == 0x64) return 0x14;                 // 8042 status: output buffer full + system flag
    return 0xFF;
}

static void dos_out(x86_16_cpu_t *c, uint16_t port, uint16_t val, int width) {
    (void)c; (void)width;
    dos_task_t *t = &g_dos;
    switch (port) {
    case 0x3C8:  // DAC write index
        t->dac_widx = (uint16_t)(val & 0xFF);
        t->dac_phase = 0;
        break;
    case 0x3C7:  // DAC read index
        t->dac_ridx = (uint16_t)(val & 0xFF);
        t->dac_phase = 0;
        break;
    case 0x3C9:  // DAC data write (r,g,b sequence, 6-bit each)
        t->pal[t->dac_widx & 0xFF][t->dac_phase] = (uint8_t)(val & 0x3F);
        t->dac_phase++;
        if (t->dac_phase >= 3) { t->dac_phase = 0; t->dac_widx++; }
        break;

    // ---- EGA sequencer (0x3C4 index / 0x3C5 data) ----
    case 0x3C4:
        t->seq_idx = (uint8_t)(val & 0x07);
        if (width == 2) {  // word OUT: high byte is the data
            uint8_t d = (uint8_t)(val >> 8);
            t->seq_reg[t->seq_idx] = d;
            if (t->seq_idx == 2) t->seq_map_mask = d & 0x0F;
        }
        break;
    case 0x3C5:
        t->seq_reg[t->seq_idx & 7] = (uint8_t)(val & 0xFF);
        if (t->seq_idx == 2) t->seq_map_mask = (uint8_t)(val & 0x0F);
        break;

    // ---- CRTC (0x3D4 index / 0x3D5 data, mono mirror 0x3B4/0x3B5) ----
    case 0x3D4:
    case 0x3B4:
        t->crtc_idx = (uint8_t)(val & 0x1F);
        if (width == 2) t->crtc[t->crtc_idx] = (uint8_t)(val >> 8);
        break;
    case 0x3D5:
    case 0x3B5:
        t->crtc[t->crtc_idx & 0x1F] = (uint8_t)(val & 0xFF);
        break;

    // ---- Misc Output register ----
    case 0x3C2:
        t->misc_out = (uint8_t)(val & 0xFF);
        break;

    // ---- EGA graphics controller (0x3CE index / 0x3CF data) ----
    case 0x3CE:
    case 0x3CF: {
        uint8_t d;
        if (port == 0x3CE) {
            t->gc_idx = (uint8_t)(val & 0xFF);
            if (width != 2) break;        // index-only write
            d = (uint8_t)(val >> 8);      // word OUT: high byte is the data
        } else {
            d = (uint8_t)(val & 0xFF);
        }
        switch (t->gc_idx) {
        case 0: t->gc_set_reset      = d & 0x0F; break;
        case 1: t->gc_en_set_reset   = d & 0x0F; break;
        case 2: t->gc_color_cmp      = d & 0x0F; break;
        case 3: t->gc_data_rotate    = d & 0x1F; break;
        case 4: t->gc_read_map       = d & 0x03; break;
        case 5: t->gc_mode           = d;        break;
        case 6: t->gc_misc           = d;        break;
        case 7: t->gc_color_dont_care= d & 0x0F; break;
        case 8: t->gc_bit_mask       = d;        break;
        }
        break;
    }

    // ---- EGA attribute controller (0x3C0 index+data, shared via flip-flop) ----
    case 0x3C0:
        if (t->atc_flipflop == 0) {
            t->atc_idx = (uint8_t)(val & 0x1F);   // bit5 = palette-address-source
            t->atc_flipflop = 1;
        } else {
            uint8_t d = (uint8_t)(val & 0xFF);
            if ((t->atc_idx & 0x1F) < 16)
                t->atc_pal[t->atc_idx & 0x0F] = d & 0x3F;
            t->atc_flipflop = 0;
        }
        break;

    default:
        break;
    }
}

// Present EGA mode 0Dh: combine the 4 planes into 4-bit pixels, map through the
// attribute-controller palette + DAC, scale into the host window.
static void dos_present_ega(dos_task_t *t) {
    int W = t->gfx_w ? t->gfx_w : MODE13_W;
    int H = t->gfx_h ? t->gfx_h : MODE13_H;
    int sw = t->win_w, sh = t->win_h;
    // #385: honour the CRTC logical line width (Offset reg 0x13, in words) and the
    // display start address (0x0C hi / 0x0D lo, in the same word units). The id
    // Galaxy engine draws into an offscreen page in a virtual screen that is
    // WIDER than the visible 320 px (for smooth scrolling) and pans by moving the
    // start address, so a present that assumes width=40 bytes @ offset 0 shows a
    // sheared/garbled image.
    // #385: the id Galaxy engine renders into a virtual screen whose scanline
    // stride is the CRTC Offset register (0x13, in WORDS -> *2 bytes) and displays
    // a page selected by the start-address register (0x0C hi/0x0D lo). In EGA
    // planar mode that address is a direct BYTE offset into each plane (NOT
    // doubled). A present that assumes 40 bytes/row @ offset 0 shears the image
    // and shows the wrong (often empty) page.
    int bytes_per_row = t->crtc[0x13] ? (t->crtc[0x13] * 2) : (W / 8);
    uint32_t start_off = ((uint32_t)t->crtc[0x0C] << 8) | t->crtc[0x0D];
    // 16-entry ARGB LUT. atc_pal[i] selects a DAC index; if the game never
    // programmed that DAC entry (all-zero), fall back to the EGA default.
    uint32_t lut[16];
    for (int i = 0; i < 16; i++) {
        uint8_t di = t->atc_pal[i] & 0x3F;
        uint8_t r6 = t->pal[di][0], g6 = t->pal[di][1], b6 = t->pal[di][2];
        if (r6 == 0 && g6 == 0 && b6 == 0) {
            // DAC not set for this entry: use default EGA colour for index i.
            r6 = ega_default_dac[i][0]; g6 = ega_default_dac[i][1]; b6 = ega_default_dac[i][2];
        }
        uint32_t r = (uint32_t)r6 * 255 / 63;
        uint32_t g = (uint32_t)g6 * 255 / 63;
        uint32_t b = (uint32_t)b6 * 255 / 63;
        lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    for (int dy = 0; dy < sh; dy++) {
        int sy = dy * H / sh;
        if (sy >= H) sy = H - 1;
        uint32_t *drow = t->win_buf + (size_t)dy * sw;
        uint32_t rowoff = start_off + (uint32_t)sy * bytes_per_row;
        for (int dx = 0; dx < sw; dx++) {
            int sx = dx * W / sw;
            if (sx >= W) sx = W - 1;
            uint32_t bo = (rowoff + (sx >> 3)) & (EGA_PLANE_SIZE - 1);
            int bit = 7 - (sx & 7);
            int pix = ((t->ega_plane[0][bo] >> bit) & 1)
                    | (((t->ega_plane[1][bo] >> bit) & 1) << 1)
                    | (((t->ega_plane[2][bo] >> bit) & 1) << 2)
                    | (((t->ega_plane[3][bo] >> bit) & 1) << 3);
            drow[dx] = lut[pix];
        }
    }
}

// ---- present: 0xA0000 (320x200x8) -> ARGB host window (scaled 2x) ---------
static void dos_present(dos_task_t *t) {
    if (!t->win_buf) return;
    if (t->video_mode == 0x0D || t->video_mode == 0x0E ||
        t->video_mode == 0x10 || t->video_mode == 0x12) {
        dos_present_ega(t);
        return;
    }
    if (t->video_mode != 0x13) return;
    int sw = t->win_w, sh = t->win_h;             // host content size (>=640x400)
    const uint8_t *vga = &t->mem[VGA_A000];
    // Build an ARGB LUT from the 6-bit palette.
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) {
        uint32_t r = (uint32_t)t->pal[i][0] * 255 / 63;
        uint32_t g = (uint32_t)t->pal[i][1] * 255 / 63;
        uint32_t b = (uint32_t)t->pal[i][2] * 255 / 63;
        lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    // Nearest-neighbour scale 320x200 into sw x sh.
    for (int dy = 0; dy < sh; dy++) {
        int sy = dy * MODE13_H / sh;
        if (sy >= MODE13_H) sy = MODE13_H - 1;
        const uint8_t *srow = vga + sy * MODE13_W;
        uint32_t *drow = t->win_buf + (size_t)dy * sw;
        for (int dx = 0; dx < sw; dx++) {
            int sx = dx * MODE13_W / sw;
            if (sx >= MODE13_W) sx = MODE13_W - 1;
            drow[dx] = lut[srow[sx]];
        }
    }
}

// ---- input forwarding: kernel cursor/keys -> DOS mouse state -------------
static void dos_pump_input(dos_task_t *t) {
    int ox, oy, ow, oh;
    if (win16_host_content_rect(t->host_slot, &ox, &oy, &ow, &oh) == 0 && ow > 0 && oh > 0) {
        int cx = (int)mouse_x - ox;
        int cy = (int)mouse_y - oy;
        if (cx < 0) cx = 0;
        if (cx >= ow) cx = ow - 1;
        if (cy < 0) cy = 0;
        if (cy >= oh) cy = oh - 1;
        // map content coords -> mode-13h virtual coords (0..639 x, 0..199 y typical)
        t->mx = cx * (MODE13_W * 2) / ow;   // DOS mouse uses 640-wide virtual range
        t->my = cy * MODE13_H / oh;
        int b = 0;
        if (mouse_buttons & 0x01) b |= 0x01;   // left
        if (mouse_buttons & 0x02) b |= 0x02;   // right
        t->mbtn = b;
    }
}

// ---- INT 9 (keyboard IRQ) delivery (#202 Keen) ---------------------------
// We have no real IRQs in the interpreter, so we synthesize them: when a raw
// scancode is available and the guest installed its own INT 9 handler, we latch
// the scancode at the emulated port 0x60, build a hardware-interrupt frame
// (push FLAGS, CS, IP) on the guest stack, and vector to the handler. The
// handler reads port 0x60, updates its Keyboard[] state, ACKs the PIC (we
// ignore the OUT to 0x20) and IRETs back to the interrupted code. We deliver at
// most a few per slice so a key cannot starve the game loop.
static void dos_push16(dos_task_t *t, uint16_t v) {
    t->cpu.sp = (uint16_t)(t->cpu.sp - 2);
    wr16(t, t->cpu.ss, t->cpu.sp, v);
}

// Deliver a synthesized hardware interrupt `vec` to the guest: push the IRET
// frame and vector to the installed handler, then run a bounded burst so the
// handler runs and IRETs back to the interrupted code.
static void dos_deliver_int(dos_task_t *t, uint8_t vec, unsigned long budget) {
    x86_16_cpu_t *c = &t->cpu;
    uint16_t voff = rd16(t, 0x0000, (uint16_t)(vec * 4));
    uint16_t vseg = rd16(t, 0x0000, (uint16_t)(vec * 4 + 2));
    if (vseg == 0 && voff == 0) return;
    dos_push16(t, c->flags);
    dos_push16(t, c->cs);
    dos_push16(t, c->ip);
    c->flags &= ~0x0200;   // CLI during ISR
    c->cs = vseg;
    c->ip = voff;
    x86_16_run(&t->cpu, budget);
}
static void dos_deliver_int9(dos_task_t *t) {
    if (!t->kbd_has_int9) return;
    x86_16_cpu_t *c = &t->cpu;
    int delivered = 0;
    while (delivered < 8) {
        int sc = dos_scancode_get();
        if (sc < 0) break;
        t->kbd_port60 = (uint8_t)sc;
        // read the guest INT 9 vector (IVT entry 9 -> linear 0x24).
        uint16_t voff = rd16(t, 0x0000, 0x0024);
        uint16_t vseg = rd16(t, 0x0000, 0x0026);
        if (vseg == 0 && voff == 0) break;
        // push hardware-interrupt frame: FLAGS, CS, IP (IRET pops IP, CS, FLAGS).
        dos_push16(t, c->flags);
        dos_push16(t, c->cs);
        dos_push16(t, c->ip);
        c->flags &= ~0x0200;   // CLI during ISR (IF cleared)
        c->cs = vseg;
        c->ip = voff;
        // Run the handler to completion. The IRET we pushed for restores cs:ip,
        // so the burst returns to the interrupted code. Keyboard ISRs are tiny.
        x86_16_run(&t->cpu, 20000);
        delivered++;
        if (t->cpu.halted) break;
    }
}

// ---- MZ / COM loader -----------------------------------------------------
// Returns 0 and sets initial cpu regs, <0 on error.
static int dos_load_image(dos_task_t *t, const uint8_t *f, uint32_t size) {
    if (size >= 2 && f[0] == 'M' && f[1] == 'Z') {
        // MZ header fields (all little-endian words):
        uint16_t bytes_last = f[2]  | (f[3]  << 8);   // bytes in last page
        uint16_t pages      = f[4]  | (f[5]  << 8);   // 512-byte pages
        uint16_t nreloc     = f[6]  | (f[7]  << 8);
        uint16_t hdr_para   = f[8]  | (f[9]  << 8);   // header size in paragraphs
        uint16_t ss         = f[14] | (f[15] << 8);   // initial SS (relative)
        uint16_t sp         = f[16] | (f[17] << 8);
        uint16_t ip         = f[20] | (f[21] << 8);
        uint16_t cs         = f[22] | (f[23] << 8);   // initial CS (relative)
        uint16_t reloc_off  = f[24] | (f[25] << 8);

        uint32_t hdr_bytes = (uint32_t)hdr_para * 16;
        uint32_t img_bytes = (uint32_t)pages * 512;
        if (bytes_last) img_bytes = img_bytes - 512 + bytes_last;
        if (img_bytes > size) img_bytes = size;
        uint32_t load_bytes = (img_bytes > hdr_bytes) ? (img_bytes - hdr_bytes) : 0;

        // Copy program image to DOS_LOAD_SEG:0000.
        uint32_t base_lin = (uint32_t)DOS_LOAD_SEG << 4;
        if (base_lin + load_bytes > VGA_A000) {
            kprintf("[dos] image too large (%u bytes)\n", load_bytes);
            return -1;
        }
        for (uint32_t i = 0; i < load_bytes; i++)
            t->mem[base_lin + i] = f[hdr_bytes + i];

        // Apply relocations: each is a word offset + word segment, relative to
        // the load segment. Add DOS_LOAD_SEG to the word at that location.
        for (uint16_t r = 0; r < nreloc; r++) {
            uint32_t e = reloc_off + (uint32_t)r * 4;
            if (e + 4 > size) break;
            uint16_t roff = f[e]   | (f[e + 1] << 8);
            uint16_t rseg = f[e + 2] | (f[e + 3] << 8);
            uint16_t fixseg = (uint16_t)(DOS_LOAD_SEG + rseg);
            uint16_t cur = rd16(t, fixseg, roff);
            wr16(t, fixseg, roff, (uint16_t)(cur + DOS_LOAD_SEG));
        }

        // alloc bump starts ABOVE the whole program block. The program's own
        // stack (SS:SP) usually sits high inside its block, above the image, so
        // the free pool for INT 21h 48h must begin past max(image_end, stack_top).
        uint16_t img_end_para  = (uint16_t)(DOS_LOAD_SEG + ((load_bytes + 15) >> 4));
        uint16_t stack_seg     = (uint16_t)(DOS_LOAD_SEG + ss);
        uint16_t stack_top_para = (uint16_t)(stack_seg + ((sp + 15) >> 4) + 1);
        uint16_t top = (img_end_para > stack_top_para) ? img_end_para : stack_top_para;
        t->alloc_top_para = (uint16_t)(top + 0x10);

        t->cpu.cs = (uint16_t)(DOS_LOAD_SEG + cs);
        t->cpu.ip = ip;
        t->cpu.ss = (uint16_t)(DOS_LOAD_SEG + ss);
        t->cpu.sp = sp;
        t->cpu.ds = DOS_PSP_SEG;
        t->cpu.es = DOS_PSP_SEG;
        kprintf("[dos] MZ loaded: img=%u reloc=%u entry=%04x:%04x ss:sp=%04x:%04x\n",
                load_bytes, nreloc, t->cpu.cs, t->cpu.ip, t->cpu.ss, t->cpu.sp);
        return 0;
    }

    // .COM: load at PSP:0100, all segs = PSP.
    uint32_t n = size; if (n > 0xFE00) n = 0xFE00;
    uint32_t base_lin = ((uint32_t)DOS_PSP_SEG << 4) + 0x100;
    for (uint32_t i = 0; i < n; i++) t->mem[base_lin + i] = f[i];
    t->cpu.cs = t->cpu.ds = t->cpu.es = t->cpu.ss = DOS_PSP_SEG;
    t->cpu.ip = 0x100;
    t->cpu.sp = 0xFFFE;
    t->alloc_top_para = DOS_PSP_SEG + 0x1000;
    kprintf("[dos] COM loaded: %u bytes at %04x:0100\n", n, DOS_PSP_SEG);
    return 0;
}

// Build a minimal PSP at DOS_PSP_SEG.
static void dos_build_psp(dos_task_t *t) {
    // PSP[0..1] = INT 20h (CD 20), PSP[0x80] = cmdline length, PSP[0x81]= CR.
    wr8(t, DOS_PSP_SEG, 0x00, 0xCD);
    wr8(t, DOS_PSP_SEG, 0x01, 0x20);
    wr16(t, DOS_PSP_SEG, 0x02, 0x9FFF);  // top of memory segment
    wr8(t, DOS_PSP_SEG, 0x80, 0x00);     // empty command tail
    wr8(t, DOS_PSP_SEG, 0x81, 0x0D);
}

// ---- run -----------------------------------------------------------------
int dos_run_file(const char *path) {
    dos_task_t *t = &g_dos;
    memset(t, 0, sizeof(*t));

    t->mem = (uint8_t *)kmalloc(DOS_MEM_SIZE);
    if (!t->mem) { kprintf("[dos] OOM allocating 1MB\n"); return -1; }
    memset(t->mem, 0, DOS_MEM_SIZE);

    // appdir from path
    {
        int last = -1;
        for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
        if (last <= 0) { t->appdir[0] = '/'; t->appdir[1] = '\0'; }
        else {
            int n = last; if (n > (int)sizeof(t->appdir) - 1) n = sizeof(t->appdir) - 1;
            for (int i = 0; i < n; i++) t->appdir[i] = path[i];
            t->appdir[n] = '\0';
        }
    }

    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, path, &size);
    if (!data || size == 0) {
        kprintf("[dos] cannot read %s\n", path);
        kfree(t->mem); return -1;
    }

    x86_16_init(&t->cpu, t->mem);
    dos_build_psp(t);
    if (dos_load_image(t, (const uint8_t *)data, size) != 0) {
        kfree(data); kfree(t->mem); return -1;
    }
    kfree(data);

    // default grayscale palette so something is visible before the app sets it
    for (int i = 0; i < 256; i++) { t->pal[i][0] = t->pal[i][1] = t->pal[i][2] = (uint8_t)(i >> 2); }

    x86_16_set_int_handler(dos_int_handler);
    x86_16_set_io_handlers(dos_in, dos_out);
    // Route 0xA0000-0xAFFFF through the EGA planar emulation (#202).
    x86_16_set_mem_hook(VGA_A000, VGA_A000_END, ega_mem_w, ega_mem_r);
    // EGA defaults until the game sets a mode.
    t->seq_map_mask = 0x0F; t->gc_bit_mask = 0xFF; t->gc_color_dont_care = 0x0F;
    for (int i = 0; i < 16; i++) t->atc_pal[i] = (uint8_t)i;

    // Create the host window (compositor draws it).
    int cw = MODE13_W * WIN_SCALE, ch = MODE13_H * WIN_SCALE;
    t->host_slot = win16_host_create("DOS", 80, 60, cw, ch + 4,
                                     &t->win_buf, &t->win_w, &t->win_h, 0);
    if (t->host_slot < 0) {
        kprintf("[dos] host window create failed\n");
        x86_16_set_io_handlers(0, 0);
        kfree(t->mem); return -1;
    }
    kprintf("[dos] window slot=%d buf=%dx%d\n", t->host_slot, t->win_w, t->win_h);

    // Seed the BIOS data area: equipment word, base memory size (640KB), and the
    // timer-tick dword at 0040:006C (many DOS programs busy-wait on it for timing).
    wr16(t, 0x0040, 0x0013, 640);          // base memory in KB
    wr16(t, 0x0040, 0x006C, 0);            // timer ticks low
    wr16(t, 0x0040, 0x006E, 0);            // timer ticks high

    // #385: Seed the IVT with valid default handlers. Real BIOS/DOS point every
    // vector at a routine; a program that hooks a hardware IRQ (INT 8 timer,
    // INT 9 keyboard, INT 1C user-tick, ...) FIRST saves the previous vector via
    // INT 21h AH=35h and CHAINS to it (pushf; call far [old]). If we leave the
    // IVT zeroed, that saved "old handler" is 0000:0000 and the chain call
    // derails into the IVT. Point a small IRET stub at F000:FF53 (the classic
    // BIOS dummy-IRET address) and default every otherwise-empty vector to it, so
    // a chained call returns cleanly (IRET pops the pushf FLAGS + return CS:IP).
    {
        // IRET stub in the reserved BIOS ROM region (never touched by the game).
        wr8(t, 0xF000, 0xFF53, 0xCF);      // IRET
        for (int v = 0; v < 256; v++) {
            uint16_t off = rd16(t, 0x0000, (uint16_t)(v * 4));
            uint16_t seg = rd16(t, 0x0000, (uint16_t)(v * 4 + 2));
            if (seg == 0 && off == 0) {
                wr16(t, 0x0000, (uint16_t)(v * 4),     0xFF53);
                wr16(t, 0x0000, (uint16_t)(v * 4 + 2), 0xF000);
            }
        }
    }

    t->running = 1;
    {   /* #201 derail ring: only when /CONFIG/DOSDIAG.CFG is present */
        extern volatile int g_x86_dbgring;
        uint32_t _dz = 0;
        void *_dc = fat_read_file(&g_fat_fs, "/CONFIG/DOSDIAG.CFG", &_dz);
        if (_dc) { kfree(_dc); g_x86_dbgring = 1; }
    }
    // Run in slices so we can pump input + present frames between bursts.
    unsigned long slice = 100000UL;   // instructions per slice
    int frames = 0;
    uint32_t bios_ticks = 0;
    uint16_t prev_cs = 0, prev_ip = 0, prev_cs2 = 0, prev_ip2 = 0;
    while (!t->cpu.halted && t->running) {
        dos_pump_input(t);
        dos_deliver_int9(t);   // synthesize keyboard IRQs for the guest ISR (#202)
        // Synthesize a timer IRQ0 (INT 8) per slice so the guest's TimeCount
        // advances; id's Galaxy engine busy-waits on TimeCount for all timing.
        if (t->has_int8 && !t->cpu.halted)
            dos_deliver_int(t, 0x08, 20000);
        // Advance the BIOS timer tick (~18.2 Hz). One tick per slice keeps
        // tick-polling delay loops progressing without spinning forever.
        bios_ticks++;
        wr16(t, 0x0040, 0x006C, (uint16_t)(bios_ticks & 0xFFFF));
        wr16(t, 0x0040, 0x006E, (uint16_t)(bios_ticks >> 16));
        // #201 derail diagnosis: single-step near the known derail (~3.3M insns)
        // so the ring buffer captures the exact transfer into zeroed memory.
        if (0) g_dos_sstep = 1;   /* disabled: use interpreter g_x86_dbgring */
        int r;
        if (g_dos_sstep != 0) {
            // Single-step until the derail (op 00 00). Keep a ring of the last 24
            // instructions and dump it when control wanders into zeros, so we see
            // the exact transition that caused the derail (#202).
            g_dos_sstep = 0;
            #define SSRING 8000
            static char ring[SSRING][160];
            int rh = 0, filled = 0;
            r = 1;
            unsigned long guard = 0;
            for (;;) {
                if (t->cpu.halted) { r = 0; break; }
                uint8_t op0 = rd8(t, t->cpu.cs, t->cpu.ip);
                uint8_t op1 = rd8(t, t->cpu.cs, (uint16_t)(t->cpu.ip + 1));
                if (op0 == 0x00 && op1 == 0x00) {
                    kprintf("[dos] === DERAIL ring dump (oldest first) ===\n");
                    int start = filled ? rh : 0;
                    for (int k = 0; k < (filled ? SSRING : rh); k++)
                        kprintf("%s", ring[(start + k) % SSRING]);
                    kprintf("[dos] DERAIL at cs:ip=%04x:%04x es=%04x\n", t->cpu.cs, t->cpu.ip, t->cpu.es);
                    break;
                }
                snprintf(ring[rh], sizeof(ring[rh]),
                    "[dos] SS %04x:%04x sp=%04x bp=%04x op=%02x%02x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x ds=%04x es=%04x fl=%04x\n",
                    t->cpu.cs, t->cpu.ip, t->cpu.sp, t->cpu.bp, op0, op1,
                    t->cpu.ax, t->cpu.bx, t->cpu.cx, t->cpu.dx,
                    t->cpu.si, t->cpu.di, t->cpu.ds, t->cpu.es, t->cpu.flags);
                rh = (rh + 1) % SSRING; if (rh == 0) filled = 1;
                r = x86_16_run(&t->cpu, 1);
                if (r < 0) break;
                if ((guard % 100000UL) == 0) {   // keep BIOS tick advancing like slice mode
                    bios_ticks++;
                    wr16(t, 0x0040, 0x006C, (uint16_t)(bios_ticks & 0xFFFF));
                    wr16(t, 0x0040, 0x006E, (uint16_t)(bios_ticks >> 16));
                }
                if (++guard > 6000000UL) { kprintf("[dos] sstep guard hit\n"); break; }
            }
            #undef SSRING
        } else {
            r = x86_16_run(&t->cpu, slice);
        }
        prev_cs2 = prev_cs; prev_ip2 = prev_ip;
        prev_cs = t->cpu.cs; prev_ip = t->cpu.ip;
        dos_present(t);
        frames++;
        // #385: periodic where-am-I so a busy-wait loop shows as a repeated cs:ip.
        if (g_x86_dbgring && (frames & 0x3F) == 0) {
            kprintf("[dos] @frame%d cs:ip=%04x:%04x op=%02x%02x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x ds=%04x es=%04x mode=%02x t=%u\n",
                    frames, t->cpu.cs, t->cpu.ip,
                    rd8(t, t->cpu.cs, t->cpu.ip), rd8(t, t->cpu.cs, (uint16_t)(t->cpu.ip+1)),
                    t->cpu.ax, t->cpu.bx, t->cpu.cx, t->cpu.dx, t->cpu.si, t->cpu.di,
                    t->cpu.ds, t->cpu.es, t->video_mode, (unsigned)rd16(t,0x0040,0x006C));
        }
        // Periodic where-am-I trace (#202 diagnostics): every ~64 slices print
        // cs:ip + key VGA state so we can locate busy-wait loops during bring-up.
        // Runaway/derail detector: if the CPU is executing 0x00 opcodes (it has
        // wandered into zeroed memory) stop early so the log isn't flooded.
        if (rd8(t, t->cpu.cs, t->cpu.ip) == 0x00 &&
            rd8(t, t->cpu.cs, (uint16_t)(t->cpu.ip + 1)) == 0x00) {
            kprintf("[dos] DERAIL: zeros at cs:ip=%04x:%04x (prev=%04x:%04x prev2=%04x:%04x) ss:sp=%04x:%04x ds=%04x es=%04x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x insns=%lu\n",
                    t->cpu.cs, t->cpu.ip, prev_cs2, prev_ip2, prev_cs, prev_ip,
                    t->cpu.ss, t->cpu.sp, t->cpu.ds, t->cpu.es,
                    t->cpu.ax, t->cpu.bx, t->cpu.cx, t->cpu.dx, t->cpu.si, t->cpu.di,
                    t->cpu.bp, t->cpu.insn_count);
            // Single-step the NEXT run from prev_cs2 region won't help (already
            // derailed); instead arm a re-run hint by dumping stack near sp.
            break;
        }
        if (r < 0) {
            kprintf("[dos] interpreter stop r=%d at %04x:%04x insns=%lu\n",
                    r, t->cpu.cs, t->cpu.ip, t->cpu.insn_count);
            break;
        }
        if (r == 0) break;   // halted normally
        // Safety cap so a runaway/busy-wait program cannot pin a CPU forever.
        // Generous so an interactive game (Keen) can run a long session; only a
        // truly hung program (mode 0x03, no graphics) trips it early.
        if (frames > 200000) {
            kprintf("[dos] frame cap reached insns=%lu mode=0x%02x\n",
                    t->cpu.insn_count, t->video_mode);
            break;
        }
        // r == 1: hit slice cap, yield then continue
        proc_sleep(15);
    }

    kprintf("[dos] '%s' finished exit=%d insns=%lu frames=%d mode=0x%02x\n",
            path, t->cpu.exit_code, t->cpu.insn_count, frames, t->video_mode);

    // Keep the final frame visible for a moment, then tear down.
    proc_sleep(2000);
    g_dos_scancode_tap = 0;
    x86_16_set_mem_hook(0, 0, 0, 0);
    x86_16_set_int_handler(0);
    x86_16_set_io_handlers(0, 0);
    win16_host_destroy(t->host_slot);
    kfree(t->mem); t->mem = NULL;
    g_dos_busy = 0;
    return t->cpu.exit_code;
}

// ---- async launch --------------------------------------------------------
static char g_dos_path[128];
static void dos_proc_entry(void *arg) {
    (void)arg;
    dos_run_file(g_dos_path);
}

// ---- boot-gated launch (RC-independent test harness, #201/#276) ----------
// Reads /CONFIG/DOSRUN.CFG (a single path line, e.g. "/DOS/TIM/TIM.EXE") and
// launches it a few seconds after boot, so a DOS game can be brought up and its
// serial trace captured without depending on the RC channel or a GUI launcher.
static void dos_deferred_entry(void *arg) {
    (void)arg;
    kprintf("[dos] deferred_entry ENTERED (task385 keen), sleeping 3s\n");
    proc_sleep(3000);   // let the desktop/compositor come up first
    kprintf("[dos] deferred_entry past sleep (task385 keen)\n");
    // #385: the FAT config read can transiently fail in a post-boot proc context
    // (concurrent FS activity from widgets). Retry a bounded number of times and
    // trace each attempt so a failure is diagnosable rather than a silent return.
    void *cfg = 0; uint32_t sz = 0;
    for (int attempt = 0; attempt < 30; attempt++) {
        cfg = fat_read_file(&g_fat_fs, "/CONFIG/DOSRUN.CFG", &sz);
        kprintf("[dos] DOSRUN.CFG read attempt %d: cfg=%p sz=%u\n", attempt, cfg, sz);
        if (cfg && sz > 0) break;
        if (cfg) { kfree(cfg); cfg = 0; }
        proc_sleep(1000);
    }
    if (!cfg || sz == 0) { if (cfg) kfree(cfg); kprintf("[dos] DOSRUN.CFG unreadable, giving up\n"); return; }
    char path[128];
    int n = 0;
    const char *p = (const char *)cfg;
    for (uint32_t i = 0; i < sz && n < (int)sizeof(path) - 1; i++) {
        char ch = p[i];
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') break;
        path[n++] = ch;
    }
    path[n] = '\0';
    kfree(cfg);
    if (n == 0) { kprintf("[dos] DOSRUN.CFG empty path\n"); return; }
    kprintf("[dos] DOSRUN.CFG -> launching '%s'\n", path);
    dos_launch(path);
}

void dos_start_deferred_launch(void) {
    uint32_t sz = 0;
    void *cfg = fat_read_file(&g_fat_fs, "/CONFIG/DOSRUN.CFG", &sz);
    if (!cfg) return;
    kfree(cfg);
    proc_create("dosrun", dos_deferred_entry, NULL, PRIO_HIGH);
}

int dos_launch(const char *path) {
    if (g_dos_busy) { kprintf("[dos] busy (a DOS task is already running)\n"); return -1; }
    int i = 0;
    for (; i < (int)sizeof(g_dos_path) - 1 && path[i]; i++) g_dos_path[i] = path[i];
    g_dos_path[i] = '\0';
    g_dos_busy = 1;
    if (proc_create("dos", dos_proc_entry, NULL, PRIO_HIGH) < 0) {
        g_dos_busy = 0;
        return -1;
    }
    kprintf("[dos] launched '%s'\n", g_dos_path);
    return 0;
}
