// ne.c - Windows 3.x (NE) + 16-bit DOS (.COM) loader for MayteraOS (#129).
// Loads a 16-bit executable into a 1 MiB real-mode image and runs it on the
// x86_16 interpreter, servicing DOS INT 21h and the Win16 API thunk (far calls
// into WIN16_THUNK_SEG -> exec/win16api.c).
//
// Phase 5 (builds 72+): generic NE RESOURCE loading and NE DLL loading.
//  * The loaded module images (the app and any dependent DLLs) are retained for
//    the lifetime of the run in a small module registry so that resource APIs
//    (LoadBitmap/LoadString/...) and cross-module imports can be served.
//  * When an app imports from a module that is NOT a built-in (KERNEL/USER/GDI/
//    WIN87EM/SOUND/...), and a matching .DLL is present on disk, that DLL is
//    loaded as a real NE module, its LibEntry is run, and the import is resolved
//    to a real far pointer into the DLL's exported entry (instead of an API
//    thunk). This is what lets CARDS.DLL (FreeCell/Golf/Tut's Tomb) etc. work.
#include "../types.h"
#include "../serial.h"
#include "../fs/fat.h"
#include "../mm/heap.h"
#include "x86_16.h"
#include "win16api.h"
#include "ne.h"

extern fat_fs_t g_fat_fs;

#define WIN16_MEM_SIZE 0x100000          // 1 MiB real-mode address space
#define WIN16_LOAD_SEG 0x1000            // paragraph where we load the app image
#define WIN16_THUNK_SEG 0xF000           // synthetic segment for imported API thunks

static uint8_t      g_win16_mem[WIN16_MEM_SIZE];
static x86_16_cpu_t g_win16_cpu;

// (#289 Phase1) When non-zero, the NEXT win16_run_file loads its NE under the
// protected-mode selector/LDT model (see x86_16.c). Default 0 = real mode, so
// every existing app path is unaffected. Set by the RC `win16pm` launcher (or a
// future per-app policy). It is consumed (left as set by the caller) each run.
int g_win16_want_pmode = 0;
char g_win16_cmdtail[128] = {0};   // (EP3) optional NE command tail (argv)
// (#345) Win16 .SCR screensaver mode: set when the NE is launched with a "/s"
// command tail (SCRNSAVE.LIB WinMain then runs the fullscreen saver). Gated so
// ordinary Win16 apps/games are byte-identical (they never pass /s).
int g_win16_scrsave = 0;
volatile unsigned long long g_win16_scrsave_start = 0;

// ---------------------------------------------------------------------------
// Imported-API thunk table. Each NE import that resolves to a BUILT-IN module is
// turned into a far pointer WIN16_THUNK_SEG:id, where `id` indexes this table.
// ---------------------------------------------------------------------------
#define WIN16_MAX_IMPORTS 8192   /* (#278 Word6: WINWORD has thousands of import relocs; dedup keeps distinct count low) */

static win16_import_t g_imports[WIN16_MAX_IMPORTS];
static int            g_import_count = 0;

static int ci_streq(const char *a, const char *b);  /* (#278) fwd decl for dedup */
static int win16_add_import(const char *module, const char *name,
                            uint16_t ordinal, int by_ordinal) {
    // (#278 Word6) DEDUP: WINWORD.EXE + the OLE2 DLLs carry thousands of import
    // relocations, but they reference only a few hundred DISTINCT (module,ordinal)
    // / (module,name) pairs. Without dedup the thunk table overflows almost
    // immediately and later relocations are left as the 0000:ffff placeholder,
    // crashing on the first far-call into an unresolved import. Reuse an existing
    // thunk slot when the same import recurs.
    for (int e = 0; e < g_import_count; e++) {
        win16_import_t *ex = &g_imports[e];
        if (ex->by_ordinal != by_ordinal) continue;
        if (!ci_streq(ex->module, module ? module : "")) continue;
        if (by_ordinal) {
            if (ex->ordinal == ordinal) return e;
        } else {
            const char *n = name ? name : "";
            int i = 0; int eq = 1;
            for (; ex->name[i] && n[i]; i++) { if (ex->name[i] != n[i]) { eq = 0; break; } }
            if (eq && ex->name[i] == 0 && n[i] == 0) return e;
        }
    }
    if (g_import_count >= WIN16_MAX_IMPORTS) return -1;
    int id = g_import_count++;
    win16_import_t *im = &g_imports[id];
    int i;
    for (i = 0; module && module[i] && i < WIN16_NAME_MAX - 1; i++) im->module[i] = module[i];
    im->module[i] = '\0';
    if (name && name[0]) {
        for (i = 0; name[i] && i < WIN16_NAME_MAX - 1; i++) im->name[i] = name[i];
        im->name[i] = '\0';
    } else {
        im->name[0] = '\0';
    }
    im->ordinal = ordinal;
    im->by_ordinal = by_ordinal;
    return id;
}

// ---------------------------------------------------------------------------
// Loaded-module registry (the app is module 0; dependent DLLs follow).
// ---------------------------------------------------------------------------
#define WIN16_MAX_SEGS    256   /* (#278 Word6: WINWORD.EXE has 235 segments) */
#define WIN16_MAX_MODULES 32

typedef struct {
    int      used;
    int      is_dll;
    char     name[WIN16_NAME_MAX];        // uppercase module name (no extension)
    uint8_t *data;                        // retained raw file image (kfree at reset)
    uint32_t size;
    uint32_t ne_off;                      // lfanew (NE header offset within data)
    uint16_t shift;                       // segment alignment shift
    uint16_t segcount;
    uint16_t segbase[WIN16_MAX_SEGS];     // loaded paragraph for each segment
    uint32_t seg_foff[WIN16_MAX_SEGS];    // file offset of raw segment data
    uint16_t seg_dlen[WIN16_MAX_SEGS];    // raw data length
    uint16_t seg_flags[WIN16_MAX_SEGS];   // segment-table flags
    uint16_t entrytab, entrytablen;       // entry table (rel NE hdr)
    uint16_t modreftab, modrefcnt;
    uint16_t impnametab;
    uint16_t rsctab;                      // resource table offset (rel NE hdr), 0=none
    uint16_t autodata;
    uint16_t ne_heap, ne_stack;
    uint16_t cs_idx, ip, ss_idx, sp;      // raw entry indices/offsets
    uint16_t cs_para, ds_para, ss_para;   // resolved paragraphs
    uint16_t hinstance;                   // DGROUP paragraph == module hInstance
    uint16_t dll_entry_cs, dll_entry_ip;  // DLL LibEntry far ptr
} win16_module_t;

static win16_module_t g_modules[WIN16_MAX_MODULES];
static int            g_module_count = 0;
static char           g_appdir[96] = "/";   // directory the launched app lives in

// Forward decls.
static void rd_cstr(x86_16_cpu_t *c, uint16_t seg, uint16_t off, char term,
                    char *out, int outsz);

static void str_upper_copy(char *dst, const char *src, int max) {
    int i = 0;
    for (; src[i] && i < max - 1; i++) {
        char ch = src[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        dst[i] = ch;
    }
    dst[i] = '\0';
}

static int ci_streq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

// Built-in modules served by the API dispatcher (imports -> thunk segment).
static int is_builtin_module(const char *mod) {
    static const char *bi[] = {
        "KERNEL", "USER", "GDI", "WIN87EM", "SOUND", "KEYBOARD", "SHELL",
        "MMSYSTEM", "COMMDLG", "TOOLHELP", "DDEML", "LZEXPAND", "VER",
        "WINSPOOL", "PENWIN", "DISPLAY", "SYSTEM", "WINOLDAP", "MOUSE",
        "OLECLI", "OLESVR", "STRESS", "WING", 0
    };
    for (int i = 0; bi[i]; i++) if (ci_streq(mod, bi[i])) return 1;
    return 0;
}

static win16_module_t *module_by_name(const char *name) {
    for (int i = 0; i < g_module_count; i++)
        if (g_modules[i].used && ci_streq(g_modules[i].name, name))
            return &g_modules[i];
    return 0;
}

static win16_module_t *module_by_hinstance(uint16_t hinst) {
    for (int i = 0; i < g_module_count; i++)
        if (g_modules[i].used &&
            (g_modules[i].hinstance == hinst || g_modules[i].cs_para == hinst))
            return &g_modules[i];
    return 0;
}

static void registry_reset(void) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i].data) { kfree(g_modules[i].data); g_modules[i].data = 0; }
        g_modules[i].used = 0;
    }
    g_module_count = 0;
}

// ---------------------------------------------------------------------------
// NE table readers.
// ---------------------------------------------------------------------------
static void ne_read_lenstr(uint8_t *ne, uint16_t imptab, uint16_t name_off, char *out) {
    out[0] = '\0';
    uint32_t p = (uint32_t)imptab + (uint32_t)name_off;
    uint8_t len = ne[p];
    int i;
    for (i = 0; i < len && i < WIN16_NAME_MAX - 1; i++) out[i] = (char)ne[p + 1 + i];
    out[i] = '\0';
}

static void ne_modref_name(uint8_t *ne, uint16_t modreftab, uint16_t modrefcnt,
                           uint16_t impnametab, uint16_t modref_idx, char *out) {
    out[0] = '\0';
    if (modref_idx < 1 || modref_idx > modrefcnt) return;
    uint32_t e = (uint32_t)modreftab + (uint32_t)(modref_idx - 1) * 2;
    uint16_t name_off = (uint16_t)(ne[e] | (ne[e + 1] << 8));
    ne_read_lenstr(ne, impnametab, name_off, out);
}

// Resolve an entry-table ordinal (1-based) of `mod` to a far ptr seg:off.
static int entry_resolve_mod(win16_module_t *mod, uint16_t ordinal,
                             uint16_t *out_seg, uint16_t *out_off) {
    if (ordinal == 0) return 0;
    uint8_t *ne = mod->data + mod->ne_off;
    uint32_t p   = (uint32_t)mod->entrytab;
    uint32_t end = (uint32_t)mod->entrytab + mod->entrytablen;
    uint16_t cur = 1;
    while (p < end) {
        uint8_t count = ne[p++];
        if (count == 0) break;
        uint8_t indic = ne[p++];
        if (indic == 0x00) { cur += count; continue; }
        if (indic == 0xFF) {                  // movable (6 bytes each)
            for (uint8_t i = 0; i < count; i++, cur++) {
                if (cur == ordinal) {
                    uint8_t  segnum = ne[p + 3];
                    uint16_t off    = (uint16_t)(ne[p + 4] | (ne[p + 5] << 8));
                    if (segnum >= 1 && segnum <= mod->segcount) {
                        *out_seg = mod->segbase[segnum - 1]; *out_off = off; return 1;
                    }
                    return 0;
                }
                p += 6;
            }
        } else {                              // fixed (3 bytes each)
            for (uint8_t i = 0; i < count; i++, cur++) {
                if (cur == ordinal) {
                    uint16_t off = (uint16_t)(ne[p + 1] | (ne[p + 2] << 8));
                    if (indic >= 1 && indic <= mod->segcount) {
                        *out_seg = mod->segbase[indic - 1]; *out_off = off; return 1;
                    }
                    return 0;
                }
                p += 3;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Fixup application.
// ---------------------------------------------------------------------------
static void win16_apply_fixup(x86_16_cpu_t *c, uint16_t seg, uint16_t off,
                              uint8_t addr_type, uint16_t rval_seg,
                              uint16_t rval_off, int additive) {
    switch (addr_type) {
        case 0: { // LOBYTE
            uint8_t v = (uint8_t)(rval_off & 0xFF);
            if (additive) v = (uint8_t)(x86_16_rd8(c, seg, off) + v);
            x86_16_wr8(c, seg, off, v);
            break;
        }
        case 2: { // SEGMENT
            uint16_t v = rval_seg;
            if (additive) v = (uint16_t)(x86_16_rd16(c, seg, off) + v);
            x86_16_wr16(c, seg, off, v);
            break;
        }
        case 3: { // FAR_ADDR
            uint16_t vo = rval_off, vs = rval_seg;
            if (additive) {
                vo = (uint16_t)(x86_16_rd16(c, seg, off) + vo);
                vs = (uint16_t)(x86_16_rd16(c, seg, (uint16_t)(off + 2)) + vs);
            }
            x86_16_wr16(c, seg, off, vo);
            x86_16_wr16(c, seg, (uint16_t)(off + 2), vs);
            break;
        }
        case 5:   // OFFSET
        default: {
            uint16_t v = rval_off;
            if (additive) v = (uint16_t)(x86_16_rd16(c, seg, off) + v);
            x86_16_wr16(c, seg, off, v);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Load one NE module's header + segments at *next_para. Relocations are applied
// in a later pass (so cross-module imports can resolve). Returns 0 on success.
// ---------------------------------------------------------------------------
static int load_ne_module(win16_module_t *mod, const char *name,
                          uint8_t *f, uint32_t size, uint16_t *next_para,
                          int is_dll) {
    if (size < 0x40 || f[0] != 'M' || f[1] != 'Z') return -1;
    uint32_t lfanew = (uint32_t)f[0x3C] | ((uint32_t)f[0x3D] << 8) |
                      ((uint32_t)f[0x3E] << 16) | ((uint32_t)f[0x3F] << 24);
    if (lfanew + 0x40 > size || f[lfanew] != 'N' || f[lfanew + 1] != 'E') return -1;
    uint8_t *ne = f + lfanew;

    mod->used = 1; mod->is_dll = is_dll; mod->data = f; mod->size = size;
    mod->ne_off = lfanew;
    str_upper_copy(mod->name, name, WIN16_NAME_MAX);
    mod->shift      = (uint16_t)(ne[0x32] | (ne[0x33] << 8));
    uint16_t segtab = (uint16_t)(ne[0x22] | (ne[0x23] << 8));
    mod->segcount   = (uint16_t)(ne[0x1C] | (ne[0x1D] << 8));
    mod->autodata   = (uint16_t)(ne[0x0E] | (ne[0x0F] << 8));
    mod->ne_heap    = (uint16_t)(ne[0x10] | (ne[0x11] << 8));
    mod->ne_stack   = (uint16_t)(ne[0x12] | (ne[0x13] << 8));
    mod->modreftab  = (uint16_t)(ne[0x28] | (ne[0x29] << 8));
    mod->modrefcnt  = (uint16_t)(ne[0x1E] | (ne[0x1F] << 8));
    mod->impnametab = (uint16_t)(ne[0x2A] | (ne[0x2B] << 8));
    mod->entrytab   = (uint16_t)(ne[0x04] | (ne[0x05] << 8));
    mod->entrytablen= (uint16_t)(ne[0x06] | (ne[0x07] << 8));
    mod->rsctab     = (uint16_t)(ne[0x24] | (ne[0x25] << 8));
    uint16_t resnametab = (uint16_t)(ne[0x26] | (ne[0x27] << 8));
    if (mod->rsctab == resnametab) mod->rsctab = 0;   // no resource table

    uint32_t csip = (uint32_t)ne[0x14] | ((uint32_t)ne[0x15] << 8) |
                    ((uint32_t)ne[0x16] << 16) | ((uint32_t)ne[0x17] << 24);
    uint32_t sssp = (uint32_t)ne[0x18] | ((uint32_t)ne[0x19] << 8) |
                    ((uint32_t)ne[0x1A] << 16) | ((uint32_t)ne[0x1B] << 24);
    mod->cs_idx = (uint16_t)(csip >> 16); mod->ip = (uint16_t)(csip & 0xFFFF);
    mod->ss_idx = (uint16_t)(sssp >> 16); mod->sp = (uint16_t)(sssp & 0xFFFF);

    if (mod->segcount > 255) { kprintf("[win16] %s: too many segs %u\n", name, mod->segcount); return -1; }

    uint16_t cur_para = *next_para;
    for (uint16_t si = 0; si < mod->segcount; si++) {
        uint8_t *se = ne + segtab + (uint32_t)si * 8;
        uint16_t soff   = (uint16_t)(se[0] | (se[1] << 8));
        uint16_t slen   = (uint16_t)(se[2] | (se[3] << 8));
        uint16_t sflags = (uint16_t)(se[4] | (se[5] << 8));
        uint16_t minall = (uint16_t)(se[6] | (se[7] << 8));
        uint32_t foff   = (uint32_t)soff << mod->shift;
        mod->segbase[si]  = cur_para;
        mod->seg_foff[si] = foff;
        mod->seg_dlen[si] = slen;
        mod->seg_flags[si]= sflags;
        uint32_t span = (minall > slen) ? minall : slen;
        if (span == 0) span = 0x1000;
        if ((uint16_t)(si + 1) == mod->autodata) {
            uint32_t dgroup = (((uint32_t)slen + 1u) & ~1u)
                              + (uint32_t)mod->ne_heap + (uint32_t)mod->ne_stack;
            if (dgroup > 0xFFF0) dgroup = 0xFFF0;
            // (#278 Word6 pass20) Large SS==DS apps (Word, 235 segs) need a real
            // 64 KiB automatic-data segment: stack at the very top, local heap
            // growing UP below it. A 0xa5b2-sized DGROUP squeezes the near heap to
            // ~15 KB so deep app-init allocations fail once the heap base is placed
            // correctly above the static data. Give the full 64 KiB (matches the
            // automatic-data segment a real Windows loader allocates).
            if (mod->autodata == mod->ss_idx && mod->ne_heap >= 0x1000 &&
                mod->segcount >= 100)
                dgroup = 0x10000;
            if (dgroup > span) span = dgroup;
        }

        // (#289 Phase1) PROTECTED-MODE segment placement: allocate ONE LDT
        // selector per segment, based into the large arena, instead of packing
        // paragraphs into the 1 MiB real-mode buffer. segbase[si] holds the
        // SELECTOR; lin() resolves it through the LDT, so the zero/load writes
        // below and all later relocation + register loads work unchanged.
        if (g_win16_pmode) {
            int is_code = !(sflags & 0x0001);     // NE seg flag bit0 = DATA
            uint32_t ablk = win16_arena_alloc(span, 16);
            uint16_t sel  = ablk ? ldt_alloc(ablk, span - 1, is_code) : 0;
            if (sel == 0) {
                kprintf("[win16] %s: pmode arena/LDT full at seg %u\n", name, si);
                return -1;
            }
            mod->segbase[si] = sel;
            for (uint32_t i = 0; i < span; i++)
                x86_16_wr8(&g_win16_cpu, sel, (uint16_t)(i & 0xFFFF), 0);
            if (soff != 0) {
                for (uint32_t i = 0; i < slen && (foff + i) < size; i++)
                    x86_16_wr8(&g_win16_cpu, sel, (uint16_t)(i & 0xFFFF), f[foff + i]);
            }
            continue;   // do NOT advance cur_para / hit the 1 MiB limit in pmode
        }

        // (real-mode) segbase already set to cur_para above.
        // Zero the WHOLE segment span FIRST so the uninitialised (BSS) tail of a
        // data segment, and any segment with no file image (soff==0), reads as 0.
        // The MS-C runtime startup (and SkiFree's assert helpers) test DGROUP
        // words against 0; stale garbage left by a previous Win16 run there made
        // those tests fail and recurse infinitely. (#200 SkiFree no-window bug.)
        for (uint32_t i = 0; i < span; i++)
            x86_16_wr8(&g_win16_cpu, cur_para, (uint16_t)i, 0);
        if (soff != 0) {
            for (uint32_t i = 0; i < slen && (foff + i) < size; i++)
                x86_16_wr8(&g_win16_cpu, cur_para, (uint16_t)i, f[foff + i]);
        }
        cur_para += (uint16_t)((span + 15) >> 4);
        if (cur_para >= WIN16_THUNK_SEG - 0x40) {
            kprintf("[win16] %s: out of real-mode memory\n", name);
            return -1;
        }
    }
    *next_para = cur_para;

    mod->cs_para = (mod->cs_idx >= 1 && mod->cs_idx <= mod->segcount)
                   ? mod->segbase[mod->cs_idx - 1] : mod->segbase[0];
    mod->ds_para = (mod->autodata >= 1 && mod->autodata <= mod->segcount)
                   ? mod->segbase[mod->autodata - 1] : mod->segbase[0];
    mod->ss_para = (mod->ss_idx >= 1 && mod->ss_idx <= mod->segcount)
                   ? mod->segbase[mod->ss_idx - 1] : mod->ds_para;
    mod->hinstance = mod->ds_para;
    mod->dll_entry_cs = mod->cs_para;
    mod->dll_entry_ip = mod->ip;
    return 0;
}

// Compare a length-counted name against a NUL-terminated name, case-insensitive.
static int name_ci_eq_n(const char *a, int alen, const char *b) {
    int i = 0;
    for (; i < alen && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return i == alen && b[i] == '\0';
}

// Resolve an exported function NAME of a DLL module to its ordinal by scanning
// the resident-name table (rel to NE header) and the non-resident-name table
// (absolute file offset). Each entry: BYTE len, len name bytes, WORD ordinal.
// The first entry in each table is the module's own name (ordinal 0) and is
// skipped. Returns 0 if not found.
static uint16_t dll_export_ordinal(win16_module_t *dep, const char *name) {
    uint8_t *ne = dep->data + dep->ne_off;
    // Resident-name table (offset relative to the NE header).
    uint16_t rnt = (uint16_t)(ne[0x26] | (ne[0x27] << 8));
    uint32_t p = dep->ne_off + rnt;
    int first = 1;
    while (p + 1 < dep->size) {
        uint8_t len = dep->data[p];
        if (len == 0) break;
        if (p + 1 + len + 2 > dep->size) break;
        const char *nm = (const char *)&dep->data[p + 1];
        uint16_t ord = (uint16_t)(dep->data[p + 1 + len] | (dep->data[p + 1 + len + 1] << 8));
        if (!first && name_ci_eq_n(nm, len, name)) return ord;
        first = 0;
        p += 1 + len + 2;
    }
    // Non-resident-name table (absolute file offset at 0x2C, size at 0x20).
    uint32_t nrt = (uint32_t)ne[0x2C] | ((uint32_t)ne[0x2D] << 8) |
                   ((uint32_t)ne[0x2E] << 16) | ((uint32_t)ne[0x2F] << 24);
    uint16_t nrtsz = (uint16_t)(ne[0x20] | (ne[0x21] << 8));
    uint32_t end = nrt + nrtsz;
    p = nrt; first = 1;
    while (p + 1 < dep->size && p < end) {
        uint8_t len = dep->data[p];
        if (len == 0) break;
        if (p + 1 + len + 2 > dep->size) break;
        const char *nm = (const char *)&dep->data[p + 1];
        uint16_t ord = (uint16_t)(dep->data[p + 1 + len] | (dep->data[p + 1 + len + 1] << 8));
        if (!first && name_ci_eq_n(nm, len, name)) return ord;
        first = 0;
        p += 1 + len + 2;
    }
    return 0;
}

// Apply per-segment relocations for `mod`, resolving imports across the registry.
static void apply_module_relocs(win16_module_t *mod) {
    uint8_t *f  = mod->data;
    uint32_t size = mod->size;
    uint8_t *ne = mod->data + mod->ne_off;
    for (uint16_t si = 0; si < mod->segcount; si++) {
        if (!(mod->seg_flags[si] & 0x0100)) continue;
        if (mod->seg_foff[si] == 0) continue;
        uint32_t rbase = mod->seg_foff[si] + mod->seg_dlen[si];
        if (rbase + 2 > size) continue;
        uint16_t rcount = (uint16_t)(f[rbase] | (f[rbase + 1] << 8));
        uint32_t rp = rbase + 2;
        uint16_t target_seg = mod->segbase[si];

        for (uint16_t ri = 0; ri < rcount; ri++) {
            if (rp + 8 > size) break;
            uint8_t  addr_type = f[rp + 0];
            uint8_t  rel_type  = f[rp + 1];
            uint16_t src_off   = (uint16_t)(f[rp + 2] | (f[rp + 3] << 8));
            uint8_t  t0 = f[rp + 4];
            uint16_t t_word4 = (uint16_t)(t0 | (f[rp + 5] << 8));
            uint16_t t_word6 = (uint16_t)(f[rp + 6] | (f[rp + 7] << 8));
            rp += 8;

            int additive = (rel_type & 0x04) ? 1 : 0;
            int reftype  = rel_type & 0x03;
            uint16_t rval_seg = 0, rval_off = 0;
            int resolved = 0;

            if (reftype == 0) {                 // INTERNALREF (own segments)
                if (t0 == 0xFF) {
                    uint16_t ms = 0, mo = 0;
                    if (entry_resolve_mod(mod, t_word6, &ms, &mo)) {
                        rval_seg = ms; rval_off = mo; resolved = 1;
                    }
                } else {
                    uint16_t tseg = t0;
                    if (tseg >= 1 && tseg <= mod->segcount) {
                        rval_seg = mod->segbase[tseg - 1]; rval_off = t_word6; resolved = 1;
                    }
                }
            } else if (reftype == 1 || reftype == 2) {   // IMPORTORDINAL / IMPORTNAME
                char modn[WIN16_NAME_MAX];
                ne_modref_name(ne, mod->modreftab, mod->modrefcnt, mod->impnametab,
                               t_word4, modn);
                win16_module_t *dep = module_by_name(modn);
                if (dep) {
                    // Resolve to the dependent DLL's real exported entry. By
                    // ordinal: t_word6 is the ordinal. By name: look the name up
                    // in the DLL's name tables to get the ordinal first.
                    uint16_t ord = 0;
                    if (reftype == 1) {
                        ord = t_word6;
                    } else {
                        char fn[WIN16_NAME_MAX];
                        ne_read_lenstr(ne, mod->impnametab, t_word6, fn);
                        ord = dll_export_ordinal(dep, fn);
                    }
                    uint16_t ds = 0, doff = 0;
                    if (ord && entry_resolve_mod(dep, ord, &ds, &doff)) {
                        rval_seg = ds; rval_off = doff; resolved = 1;
                        // Capture CARDS.cdtInit (ordinal 1) so the interpreter can
                        // trace it (diagnose garbage card width). Diagnostic only.
                        extern uint16_t g_cdt_init_seg, g_cdt_init_off;
                        if (ci_streq(modn, "CARDS") && ord == 1) {
                            g_cdt_init_seg = ds; g_cdt_init_off = doff;
                        }
                    }
                    // Record non-system (app DLL, e.g. CARDS) import resolution to a
                    // side buffer (g_trace is reset later in win16_api_begin); it is
                    // emitted into the run trace by win16_api_begin so we can confirm
                    // cdtInit/cdtDraw point at real DLL code, not a thunk stub.
                    if (!ci_streq(modn, "KERNEL") && !ci_streq(modn, "USER") &&
                        !ci_streq(modn, "GDI")) {
                        char fn[WIN16_NAME_MAX]; fn[0] = '\0';
                        if (reftype == 2)
                            ne_read_lenstr(ne, mod->impnametab, t_word6, fn);
                        win16_reloc_log(modn, fn, ord, resolved, ds, doff);
                    }
                }
                // (#188 Word6) KERNEL equate imports are CONSTANTS the real loader
                // patches inline (they are not callable APIs). __AHSHIFT (KERNEL.113)
                // and __AHINCR (KERNEL.114) describe the selector layout; __WINFLAGS
                // (KERNEL.178) is the WinFlags dword. Resolving these as far-call
                // thunks corrupts code that does `sel >> __AHSHIFT` etc, so patch the
                // literal value here instead. (Wine: krnl386.exe16.spec equates.)
                if (!resolved && reftype == 1 && ci_streq(modn, "KERNEL")) {
                    int is_eq = 1; uint32_t eqv = 0;
                    if      (t_word6 == 113) eqv = 3;            // __AHSHIFT
                    else if (t_word6 == 114) eqv = 8;            // __AHINCR
                    else if (t_word6 == 178) eqv = 0x0413;       // __WINFLAGS (WF_PMODE|WF_CPU286|WF_80x87)
                    else is_eq = 0;
                    if (is_eq) {
                        rval_off = (uint16_t)(eqv & 0xFFFF);
                        rval_seg = (uint16_t)(eqv >> 16);
                        resolved = 1;
                    }
                }
                if (!resolved) {
                    // Fall back to a built-in API thunk (logs name on call).
                    int id;
                    if (reftype == 1) {
                        id = win16_add_import(modn, 0, t_word6, 1);
                    } else {
                        char fn[WIN16_NAME_MAX];
                        ne_read_lenstr(ne, mod->impnametab, t_word6, fn);
                        id = win16_add_import(modn, fn, 0, 0);
                    }
                    if (id >= 0) { rval_seg = WIN16_THUNK_SEG; rval_off = (uint16_t)id; resolved = 1; }
                }
            } else {
                continue;   // OSFIXUP
            }
            if (!resolved) continue;

            if (additive) {
                win16_apply_fixup(&g_win16_cpu, target_seg, src_off,
                                  addr_type, rval_seg, rval_off, 1);
            } else {
                uint16_t cur = src_off;
                int guard = 0;
                while (cur != 0xFFFF && guard++ < 8192) {
                    uint16_t next = x86_16_rd16(&g_win16_cpu, target_seg, cur);
                    win16_apply_fixup(&g_win16_cpu, target_seg, cur,
                                      addr_type, rval_seg, rval_off, 0);
                    cur = next;
                }
            }
        }
    }
}

// Compute and apply the SP fix-up for a module whose header SP == 0.
static void module_fix_sp(win16_module_t *mod) {
    if (mod->sp != 0) return;
    uint16_t adata_len = (mod->ss_idx >= 1 && mod->ss_idx <= mod->segcount)
                         ? mod->seg_dlen[mod->ss_idx - 1]
                         : ((mod->autodata >= 1 && mod->autodata <= mod->segcount)
                            ? mod->seg_dlen[mod->autodata - 1] : 0);
    uint32_t top = ((uint32_t)adata_len + 1u) & ~1u;
    top += mod->ne_heap; top += mod->ne_stack;
    if (top < 0x0100) top = 0x1000;
    if (top > 0xFFFE) top = 0xFFFE;
    // (#278 Word6 pass20) Large SS==DS apps get a full 64 KiB DGROUP (see ne.c
    // autodata sizing): place the stack at the very top so the near heap has room
    // to grow up below it.
    if (mod->autodata == mod->ss_idx && mod->ne_heap >= 0x1000 && mod->segcount >= 100)
        top = 0xFFFE;
    mod->sp = (uint16_t)top;
    if (mod->ss_idx >= 1 && mod->ss_idx <= mod->segcount) {
        uint16_t need_paras = (uint16_t)((mod->sp + 15) >> 4);
        uint16_t have_end   = (uint16_t)(mod->segbase[mod->ss_idx - 1] + need_paras);
        if (mod->ss_idx < mod->segcount && have_end > mod->segbase[mod->ss_idx])
            mod->sp = (uint16_t)(((uint32_t)(mod->segbase[mod->ss_idx] -
                                  mod->segbase[mod->ss_idx - 1]) << 4) - 2);
    }
}

// ---------------------------------------------------------------------------
// DLL discovery: try appdir/NAME.DLL, /WIN16/SYSTEM/NAME.DLL, /WIN16/NAME.DLL,
// /NAME.DLL. Returns a kmalloc'd image (caller owns) + size, or 0.
// ---------------------------------------------------------------------------
static uint8_t *find_dll_file(const char *modname, uint32_t *out_size) {
    char up[WIN16_NAME_MAX];
    str_upper_copy(up, modname, sizeof(up));
    char path[160];
    const char *dirs[5];
    dirs[0] = g_appdir; dirs[1] = "/WIN16/SYSTEM"; dirs[2] = "/WIN16";
    dirs[3] = "/WIN16/MSEP"; dirs[4] = "/";
    for (int d = 0; d < 5; d++) {
        int n = 0;
        const char *dir = dirs[d];
        for (int i = 0; dir[i] && n < (int)sizeof(path) - 16; i++) path[n++] = dir[i];
        if (n == 0 || path[n - 1] != '/') path[n++] = '/';
        for (int i = 0; up[i]; i++) path[n++] = up[i];
        path[n++] = '.'; path[n++] = 'D'; path[n++] = 'L'; path[n++] = 'L';
        path[n] = '\0';
        uint32_t sz = 0;
        void *data = fat_read_file(&g_fat_fs, path, &sz);
        if (data && sz > 0) {
            kprintf("[win16] loaded dependency %s from %s (%u bytes)\n", up, path, sz);
            *out_size = sz;
            return (uint8_t *)data;
        }
    }
    return 0;
}

// Recursively ensure all non-builtin module references of `mod` are loaded as
// real DLLs (depth-limited). next_para advances as segments are placed.
static void load_dependencies(win16_module_t *mod, uint16_t *next_para, int depth) {
    if (depth > 4) return;
    uint8_t *ne = mod->data + mod->ne_off;
    for (uint16_t idx = 1; idx <= mod->modrefcnt; idx++) {
        char modn[WIN16_NAME_MAX];
        ne_modref_name(ne, mod->modreftab, mod->modrefcnt, mod->impnametab, idx, modn);
        if (!modn[0]) continue;
        if (is_builtin_module(modn)) continue;
        if (module_by_name(modn)) continue;          // already loaded
        if (g_module_count >= WIN16_MAX_MODULES) continue;
        uint32_t dsz = 0;
        uint8_t *dd = find_dll_file(modn, &dsz);
        if (!dd) continue;                            // fall back to API thunks
        win16_module_t *dm = &g_modules[g_module_count];
        if (load_ne_module(dm, modn, dd, dsz, next_para, 1) != 0) {
            kfree(dd); continue;
        }
        g_module_count++;
        module_fix_sp(dm);
        load_dependencies(dm, next_para, depth + 1);  // DLLs may need DLLs
    }
}

// Run a loaded DLL's LibEntry once (Win16 init contract). Safe no-op if the DLL
// has no entry. The interpreter's far-call API trap still services KERNEL/USER/
// GDI calls the DLL makes during init.
static void run_dll_init(win16_module_t *dm) {
    if (!dm->is_dll) return;
    if (dm->dll_entry_cs == 0 && dm->dll_entry_ip == 0) return;
    // LibEntry register contract: DI=hModule, DS=DGROUP, CX=heap size,
    // ES:SI=command line (0:0). LibEntry pushes these as LibMain args.
    g_win16_cpu.di = dm->hinstance;
    g_win16_cpu.ds = dm->ds_para;
    g_win16_cpu.es = 0;
    g_win16_cpu.si = 0;
    g_win16_cpu.cx = dm->ne_heap;
    g_win16_cpu.ss = dm->ss_para ? dm->ss_para : dm->ds_para;
    g_win16_cpu.sp = dm->sp ? dm->sp : 0x0F00;
    g_win16_cpu.bp = 0;
    uint16_t ax = 0, dx = 0;
    int r = x86_16_call_far(&g_win16_cpu, dm->dll_entry_cs, dm->dll_entry_ip,
                            0, 0, &ax, &dx, 1500000UL);
    kprintf("[win16] DLL %s LibEntry %04x:%04x -> r=%d ax=%04x\n",
            dm->name, dm->dll_entry_cs, dm->dll_entry_ip, r, ax);
    win16_trace("DLL %s LibEntry %04x:%04x -> r=%d ax=%04x\n",
                dm->name, dm->dll_entry_cs, dm->dll_entry_ip, r, ax);
}

// ---------------------------------------------------------------------------
// Windows patches the prologue of exported DLL functions so each one reloads DS
// to the DLL's own automatic data segment (DGROUP). The MS-C placeholder
// prologue is `push ds; pop ax; nop` (1E 58 90), which KEEPS the caller's DS;
// the real loader rewrites those 3 bytes to `mov ax, DGROUP` (B8 lo hi). Without
// this, an exported DLL function (e.g. CARDS.cdtInit) runs on the calling app's
// DS and reads its own globals from the wrong segment, returning garbage (the
// cause of the bogus card width 0x6C69). We do the same patch here, for every
// exported entry of a DLL that has an auto-data segment, but only when its
// prologue is exactly the 1E 58 90 placeholder (so window-proc style callbacks,
// which legitimately keep the caller's DS, are never touched). The main EXE is
// never patched: its window procs are invoked by the host with DS already set.
static void patch_dll_entry_prologues(win16_module_t *mod) {
    if (!mod->is_dll || mod->autodata == 0) return;
    uint16_t dg = mod->hinstance;                 // DGROUP paragraph
    uint8_t *ne = mod->data + mod->ne_off;
    uint32_t p = mod->entrytab, end = mod->entrytab + mod->entrytablen;
    uint16_t cur = 1; int patched = 0;
    while (p < end) {
        uint8_t count = ne[p++]; if (count == 0) break;
        uint8_t indic = ne[p++];
        if (indic == 0x00) { cur += count; continue; }   // gap (unused ordinals)
        int movable = (indic == 0xFF);
        for (uint8_t i = 0; i < count; i++, cur++) {
            uint8_t flags; uint8_t segnum; uint16_t off;
            if (movable) { flags = ne[p]; segnum = ne[p + 3];
                           off = (uint16_t)(ne[p + 4] | (ne[p + 5] << 8)); p += 6; }
            else         { flags = ne[p]; segnum = indic;
                           off = (uint16_t)(ne[p + 1] | (ne[p + 2] << 8)); p += 3; }
            if (!(flags & 0x01)) continue;               // not exported
            if (segnum < 1 || segnum > mod->segcount) continue;
            uint16_t seg = mod->segbase[segnum - 1];
            if (x86_16_rd8(&g_win16_cpu, seg, off)               == 0x1E &&
                x86_16_rd8(&g_win16_cpu, seg, (uint16_t)(off+1))  == 0x58 &&
                x86_16_rd8(&g_win16_cpu, seg, (uint16_t)(off+2))  == 0x90) {
                x86_16_wr8(&g_win16_cpu, seg, off,              0xB8);          // mov ax,
                x86_16_wr8(&g_win16_cpu, seg, (uint16_t)(off+1), (uint8_t)(dg & 0xFF));
                x86_16_wr8(&g_win16_cpu, seg, (uint16_t)(off+2), (uint8_t)(dg >> 8));
                patched++;
            }
        }
    }
    win16_trace("DLL %s: patched %d exported prologues (DGROUP=%04x)\n",
                mod->name, patched, dg);
}

// ---------------------------------------------------------------------------
// Resource access (exposed via ne.h to win16api.c). type/id are integer ids
// (without the 0x8000 flag). hinst selects the module (app or a DLL). Returns a
// pointer into the retained module image + the resource length, or 0.
// ---------------------------------------------------------------------------
// Look up an integer-id resource in ONE module's resource table. Returns a
// pointer into the retained module image (+ length) or 0 if not present.
static const uint8_t *res_in_module(win16_module_t *mod, uint16_t type,
                                    uint16_t id, uint32_t *out_len) {
    if (!mod || !mod->rsctab || !mod->data) return 0;
    uint8_t *ne = mod->data + mod->ne_off;
    uint32_t p = mod->rsctab;
    uint16_t ashift = (uint16_t)(ne[p] | (ne[p + 1] << 8));
    p += 2;
    for (;;) {
        uint16_t tid = (uint16_t)(ne[p] | (ne[p + 1] << 8));
        if (tid == 0) break;
        uint16_t cnt = (uint16_t)(ne[p + 2] | (ne[p + 3] << 8));
        uint32_t np = p + 8;
        int type_match = (tid & 0x8000) && ((tid & 0x7FFF) == type);
        for (uint16_t i = 0; i < cnt; i++) {
            uint8_t *e = ne + np + (uint32_t)i * 12;
            uint16_t roff  = (uint16_t)(e[0] | (e[1] << 8));
            uint16_t rlen  = (uint16_t)(e[2] | (e[3] << 8));
            uint16_t rid   = (uint16_t)(e[6] | (e[7] << 8));
            if (type_match && (rid & 0x8000) && ((rid & 0x7FFF) == id)) {
                // Resource offsets are from the start of the FILE, not the NE hdr.
                uint32_t foff = (uint32_t)roff << ashift;
                uint32_t flen = (uint32_t)rlen << ashift;
                if (foff + flen > mod->size) {
                    if (foff >= mod->size) return 0;
                    flen = mod->size - foff;
                }
                if (out_len) *out_len = flen;
                return mod->data + foff;
            }
        }
        p = np + (uint32_t)cnt * 12;
    }
    return 0;
}

const uint8_t *win16_get_resource(uint16_t hinst, uint16_t type, uint16_t id,
                                  uint32_t *out_len) {
    // 1) Prefer the module that owns the supplied hInstance.
    win16_module_t *mod = module_by_hinstance(hinst);
    if (mod) {
        const uint8_t *r = res_in_module(mod, type, id, out_len);
        if (r) return r;
    }
    // 2) Fall back to scanning every loaded module (app + DLLs). Apps frequently
    // pass a stale/wrong hInstance, and shared DLLs (e.g. CARDS.DLL) hold the
    // bitmaps/strings the app draws via DLL calls. Whichever module actually
    // contains the resource wins. This is what makes the card games render.
    for (int i = 0; i < g_module_count; i++) {
        if (&g_modules[i] == mod) continue;   // already tried
        const uint8_t *r = res_in_module(&g_modules[i], type, id, out_len);
        if (r) return r;
    }
    return 0;
}

// Return the FIRST resource of a given type (any id/name) in the module that
// owns hinst (or the app module if unknown). Used for singletons like the
// accelerator table, whose name may be a string we cannot match by integer id.
const uint8_t *win16_get_resource_first(uint16_t hinst, uint16_t type,
                                        uint32_t *out_len) {
    win16_module_t *mod = module_by_hinstance(hinst);
    if (!mod) { if (g_module_count > 0) mod = &g_modules[0]; else return 0; }
    if (!mod->rsctab || !mod->data) return 0;
    uint8_t *ne = mod->data + mod->ne_off;
    uint32_t p = mod->rsctab;
    uint16_t ashift = (uint16_t)(ne[p] | (ne[p + 1] << 8));
    p += 2;
    for (;;) {
        uint16_t tid = (uint16_t)(ne[p] | (ne[p + 1] << 8));
        if (tid == 0) break;
        uint16_t cnt = (uint16_t)(ne[p + 2] | (ne[p + 3] << 8));
        uint32_t np = p + 8;
        int type_match = (tid & 0x8000) && ((tid & 0x7FFF) == type);
        if (type_match && cnt > 0) {
            uint8_t *e = ne + np;  // first entry
            uint16_t roff = (uint16_t)(e[0] | (e[1] << 8));
            uint16_t rlen = (uint16_t)(e[2] | (e[3] << 8));
            uint32_t foff = (uint32_t)roff << ashift;
            uint32_t flen = (uint32_t)rlen << ashift;
            if (foff >= mod->size) return 0;
            if (foff + flen > mod->size) flen = mod->size - foff;
            if (out_len) *out_len = flen;
            return mod->data + foff;
        }
        p = np + (uint32_t)cnt * 12;
    }
    return 0;
}

// Look up a STRING-NAMED resource in ONE module. Many apps (e.g. Chips Challenge)
// call LoadBitmap/FindResource with a string name, not MAKEINTRESOURCE(id). In the
// NE resource table, a resource (or type) whose high bit is CLEAR stores a byte
// offset, relative to the start of the resource table, to a Pascal (length-prefix)
// string. Win16 string-name matching is case-insensitive; NE stores names upper.
static const uint8_t *res_by_name_in_module(win16_module_t *mod, uint16_t type,
                                            const char *want, uint32_t *out_len) {
    if (!mod || !mod->rsctab || !mod->data || !want || !want[0]) return 0;
    uint8_t *ne = mod->data + mod->ne_off;
    uint32_t rtab = mod->rsctab;
    uint32_t p = rtab;
    uint16_t ashift = (uint16_t)(ne[p] | (ne[p + 1] << 8));
    p += 2;
    for (;;) {
        uint16_t tid = (uint16_t)(ne[p] | (ne[p + 1] << 8));
        if (tid == 0) break;
        uint16_t cnt = (uint16_t)(ne[p + 2] | (ne[p + 3] << 8));
        uint32_t np = p + 8;
        int type_match = (tid & 0x8000) && ((tid & 0x7FFF) == type);
        for (uint16_t i = 0; type_match && i < cnt; i++) {
            uint8_t *e = ne + np + (uint32_t)i * 12;
            uint16_t roff = (uint16_t)(e[0] | (e[1] << 8));
            uint16_t rlen = (uint16_t)(e[2] | (e[3] << 8));
            uint16_t rid  = (uint16_t)(e[6] | (e[7] << 8));
            if (rid & 0x8000) continue;            // integer id, not a name
            uint32_t noff = rtab + rid;            // -> Pascal string in ne[]
            if (noff >= (uint32_t)(mod->size - mod->ne_off)) continue;
            uint8_t nlen = ne[noff];
            const char *nm = (const char *)(ne + noff + 1);
            int j = 0, ok = 1;
            for (; j < nlen; j++) {
                char a = nm[j], b = want[j];
                if (b == 0) { ok = 0; break; }
                if (a >= 'a' && a <= 'z') a -= 32;
                if (b >= 'a' && b <= 'z') b -= 32;
                if (a != b) { ok = 0; break; }
            }
            if (ok && want[j] == 0) {
                uint32_t foff = (uint32_t)roff << ashift;
                uint32_t flen = (uint32_t)rlen << ashift;
                if (foff >= mod->size) return 0;
                if (foff + flen > mod->size) flen = mod->size - foff;
                if (out_len) *out_len = flen;
                return mod->data + foff;
            }
        }
        p = np + (uint32_t)cnt * 12;
    }
    return 0;
}

const uint8_t *win16_get_resource_by_name(uint16_t hinst, uint16_t type,
                                          const char *name, uint32_t *out_len) {
    win16_module_t *mod = module_by_hinstance(hinst);
    if (mod) {
        const uint8_t *r = res_by_name_in_module(mod, type, name, out_len);
        if (r) return r;
    }
    for (int i = 0; i < g_module_count; i++) {
        if (&g_modules[i] == mod) continue;
        const uint8_t *r = res_by_name_in_module(&g_modules[i], type, name, out_len);
        if (r) return r;
    }
    return 0;
}

// Expose the app's working directory for relative file opens.
const char *win16_get_appdir(void) { return g_appdir; }

// Strip any path and ".DLL"/".EXE" extension from a LoadLibrary name so it can be
// matched against the loaded-module table (whose names are bare, e.g. "WEPUTIL").
static void lib_basename(const char *in, char *out, int outsz) {
    const char *b = in;
    for (const char *p = in; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    int n = 0;
    for (; b[n] && b[n] != '.' && n < outsz - 1; n++) out[n] = b[n];
    out[n] = 0;
}

// LoadLibrary (KERNEL.95): return a handle for an already-loaded module so a later
// GetProcAddress can resolve real exports. Modules statically referenced by the
// app (and their dependencies) are already in the registry; we match by basename.
// If the module is not loaded we return a small fake success handle (>=32) so the
// caller does not treat it as an error, but GetProcAddress on it yields 0:0.
uint16_t win16_load_library(const char *name) {
    char base[WIN16_NAME_MAX];
    lib_basename(name, base, sizeof(base));
    win16_module_t *m = module_by_name(base);
    if (m) return m->hinstance;
    return 0x0040;   // not loaded: harmless non-error handle
}

// GetProcAddress (KERNEL.50): resolve an export of the module identified by
// hmodule to a far pointer (seg:off). lpProcName is either a name string
// (name != 0) or, when the high word of the pointer is 0, an integer ordinal
// (ordinal != 0). Returns 1 and fills seg/off on success, 0 otherwise.
int win16_get_proc_address(uint16_t hmodule, const char *name, uint16_t ordinal,
                           uint16_t *out_seg, uint16_t *out_off) {
    win16_module_t *m = module_by_hinstance(hmodule);
    if (!m) return 0;
    uint16_t ord = ordinal;
    if (name && name[0]) ord = dll_export_ordinal(m, name);
    if (ord == 0) return 0;
    return entry_resolve_mod(m, ord, out_seg, out_off);
}

// (#278 pass32) Resolve an exported ordinal of a loaded module by NAME (works
// for DATA-NONE DLLs whose hinstance is 0, e.g. SDM.DLL). See ne.h.
int win16_module_export(const char *module, uint16_t ordinal,
                        uint16_t *out_seg, uint16_t *out_off) {
    win16_module_t *m = module_by_name(module);
    if (!m || ordinal == 0) return 0;
    return entry_resolve_mod(m, ordinal, out_seg, out_off);
}

// ---------------------------------------------------------------------------
// INT handler: DOS INT 21h subset + legacy Win16 MVP thunk (INT 80h).
// ---------------------------------------------------------------------------
static void rd_cstr(x86_16_cpu_t *c, uint16_t seg, uint16_t off, char term,
                    char *out, int outsz) {
    int i = 0;
    for (; i < outsz - 1; i++) {
        uint8_t ch = x86_16_rd8(c, seg, (uint16_t)(off + i));
        if (ch == (uint8_t)term) break;
        out[i] = (char)ch;
    }
    out[i] = '\0';
}

static char g_win16_title[64] = "Win16 Application";
__attribute__((weak)) void win16_gui_messagebox(const char *title, const char *text) {
    (void)title; (void)text;
}

// (#339 Sensei) DOS INT 21h read-only file table (see win16_int AH=3D..44).
extern void dos_resolve_path(const char *in, const char *reldir, char *out, int outsz);
extern fat_fs_t g_fat_fs;
#define DOSF_MAX 16
typedef struct { int used; uint8_t *data; uint32_t size, pos; } dos_file_t;
static dos_file_t g_dosf[DOSF_MAX];

static int win16_int(x86_16_cpu_t *c, uint8_t intno) {
    if (intno == 0x21) {
        uint8_t ah = (uint8_t)(c->ax >> 8);
        if (ah == 0x09) { char b[256]; rd_cstr(c, c->ds, c->dx, '$', b, sizeof(b)); kprintf("%s", b); }
        else if (ah == 0x02) { kprintf("%c", (char)(c->dx & 0xFF)); }
        else if (ah == 0x4C) { c->exit_code = (int)(c->ax & 0xFF); c->halted = 1; }
        else if (ah == 0x35) { c->es = 0; c->bx = 0; }   // GetIntVector -> NULL
        else if (ah == 0x25) { /* SetIntVector: no real IVT, accept silently */ }
        else if (ah == 0x30) { c->ax = 0x0A03; }         // GetDOSVersion -> 3.10-ish
        else if (ah == 0x62) { c->bx = 0x0080; }         // GetPSP -> our PSP seg
        // (#339 Sensei) Minimal read-only DOS file I/O so Win16 2.x apps that
        // open their data files via raw INT 21h handles (Sensei Calculus reads
        // CALCVGA.PIC / TOOLRSRC.SRF this way) can load content. Files are read
        // whole via fat_read_file (FAT + ext2 hook) on open; writes to file
        // handles are accepted but discarded (content files are read-only). Card
        // games (FreeCell etc.) never issue these calls, so they stay byte-exact.
        else if (ah == 0x3D) {   // open existing file, DS:DX=name -> AX=handle
            char dn[128]; rd_cstr(c, c->ds, c->dx, 0, dn, sizeof(dn));
            char path[160]; dos_resolve_path(dn, win16_get_appdir(), path, sizeof(path));
            uint32_t sz = 0; void *d = fat_read_file(&g_fat_fs, path, &sz);
            if (!d) { c->ax = 0x0002; c->flags |= 1; kprintf("[win16] DOS open '%s' -> not found\n", path); }
            else { int h = -1; for (int i = 0; i < DOSF_MAX; i++) if (!g_dosf[i].used) { h = i; break; }
                if (h < 0) { kfree(d); c->ax = 0x0004; c->flags |= 1; }
                else { g_dosf[h].used = 1; g_dosf[h].data = (uint8_t*)d; g_dosf[h].size = sz; g_dosf[h].pos = 0;
                    c->ax = (uint16_t)(h + 5); c->flags &= ~1u;
                    kprintf("[win16] DOS open '%s' -> h%d size %u\n", path, h + 5, sz); } }
        }
        else if (ah == 0x3F) {   // read BX=handle CX=count DS:DX=buf -> AX=bytes
            int h = (int)c->bx - 5;
            if (h < 0 || h >= DOSF_MAX || !g_dosf[h].used) { c->ax = 0x0006; c->flags |= 1; }
            else { dos_file_t *f = &g_dosf[h]; uint32_t n = c->cx;
                if (f->pos > f->size) f->pos = f->size;
                if (n > f->size - f->pos) n = f->size - f->pos;
                for (uint32_t i = 0; i < n; i++) x86_16_wr8(c, c->ds, (uint16_t)(c->dx + i), f->data[f->pos + i]);
                f->pos += n; c->ax = (uint16_t)n; c->flags &= ~1u; }
        }
        else if (ah == 0x40) {   // write BX=handle CX=count DS:DX=buf
            if (c->bx == 1 || c->bx == 2) {   // stdout/stderr -> serial
                for (uint32_t i = 0; i < c->cx; i++) kprintf("%c", (char)x86_16_rd8(c, c->ds, (uint16_t)(c->dx + i)));
                c->ax = c->cx; c->flags &= ~1u;
            } else { int h = (int)c->bx - 5;
                if (h >= 0 && h < DOSF_MAX && g_dosf[h].used) { c->ax = c->cx; c->flags &= ~1u; }  // discard, report OK
                else { c->ax = 0x0006; c->flags |= 1; } }
        }
        else if (ah == 0x42) {   // lseek AL=mode BX=handle CX:DX=off -> DX:AX=pos
            int h = (int)c->bx - 5;
            if (h < 0 || h >= DOSF_MAX || !g_dosf[h].used) { c->ax = 0x0006; c->flags |= 1; }
            else { dos_file_t *f = &g_dosf[h];
                int32_t off = (int32_t)(((uint32_t)c->cx << 16) | c->dx);
                int mode = (int)(c->ax & 0xFF);
                int64_t base = (mode == 1) ? (int64_t)f->pos : (mode == 2) ? (int64_t)f->size : 0;
                int64_t np = base + off; if (np < 0) np = 0; if (np > (int64_t)f->size) np = f->size;
                f->pos = (uint32_t)np; c->ax = (uint16_t)(f->pos & 0xFFFF); c->dx = (uint16_t)(f->pos >> 16); c->flags &= ~1u; }
        }
        else if (ah == 0x3E) {   // close BX=handle
            int h = (int)c->bx - 5;
            if (h >= 0 && h < DOSF_MAX && g_dosf[h].used) { kfree(g_dosf[h].data); g_dosf[h].used = 0; g_dosf[h].data = 0; }
            c->ax = 0; c->flags &= ~1u;
        }
        else if (ah == 0x44) {   // IOCTL: report file handle (not a char device)
            c->ax = 0; c->dx = 0; c->flags &= ~1u;
        }
        else { kprintf("[win16] INT 21h AH=%02x (ignored)\n", ah); }
        return 0;
    }
    if (intno == 0x80) {
        if (c->ax == 0) { c->halted = 1; return 0; }
        if (c->ax == 1) { char b[256]; rd_cstr(c, c->ds, c->dx, '\0', b, sizeof(b));
                          kprintf("[Win16 MessageBox] %s\n", b);
                          win16_gui_messagebox(g_win16_title, b); return 0; }
        kprintf("[win16] INT 80h AX=%04x (ignored)\n", c->ax);
        return 0;
    }
    // (#278 b653) INT 03 spin guard. Word 6's MS-C runtime (seg231, cs=073f) can
    // run away into a tight loop that repeatedly hits an int3 byte at the same
    // cs:ip (currently 073f:066e) when its local-heap arena geometry diverges
    // from real Windows. Without a guard the interpreter prints "unhandled INT 03"
    // hundreds of thousands of times, hogging the CPU and briefly wedging the host
    // VM. Detect a repeated INT 03 at the same address and halt the win16 task
    // cleanly (the same crash-guard philosophy as the WILDCS guard) so the OS
    // stays live. This is a SAFETY NET, not a fix for the underlying runaway
    // (RE'd as the next blocker; see cl_word6.md).
    if (intno == 0x03) {
        static uint16_t last_cs = 0, last_ip = 0; static unsigned spin = 0;
        if (c->cs == last_cs && c->ip == last_ip) {
            if (++spin > 64) {
                kprintf("[win16] INT 03 spin guard: halting at %04x:%04x (>%u repeats)\n",
                        c->cs, c->ip, spin);
                c->exit_code = 0; c->halted = 1;
                return 1;
            }
        } else { last_cs = c->cs; last_ip = c->ip; spin = 0; }
        return 0;   // first few: ignore (continue past the int3)
    }
    if (intno == 0x2f) {
        extern int g_w6life;
        static int n2f=0;
        if (g_w6life && n2f<40) { n2f++;
            kprintf("[W6INT2F] ax=%04x bx=%04x cx=%04x dx=%04x es=%04x di=%04x at %04x:%04x\n",
                    c->ax, c->bx, c->cx, c->dx, c->es, c->di, c->cs, c->ip); }
        return 0;   // leave regs (AX=1500h drive-check: BX stays 0 = no CD-ROM)
    }
    if (intno == 0x00) {
        // (#390 Corel) Divide-error vector (#DE from div/idiv-by-zero or AAM 0).
        // Real Win16 routes INT 0 to the KERNEL's default handler, which -- with
        // no app-installed fault handler -- lets the task continue rather than
        // tearing down the whole system. Faithfully continue with regs unchanged
        // instead of aborting the run, so DLL LibEntry init proceeds past a stray
        // divide error. Bounded log so a runaway divide cannot flood the serial.
        static unsigned de_n = 0;
        if (de_n < 16) { de_n++;
            kprintf("[win16] INT 00 divide-error at %04x:%04x (handled, continue)\n",
                    c->cs, c->ip); }
        return 0;
    }
    kprintf("[win16] unhandled INT %02x at %04x:%04x\n", intno, c->cs, c->ip);
    return 0;
}

// ---------------------------------------------------------------------------
// Load + run a 16-bit executable. Returns 0 on success.
// ---------------------------------------------------------------------------
// CARDS.cdtInit far address, captured during import resolution (diagnostic trace).
uint16_t g_cdt_init_seg = 0, g_cdt_init_off = 0;
// Set to 1 (e.g. from a debugger) to instruction-trace cdtInit into /WIN16LOG.TXT.
int g_win16_fntrace = 0;

// (#256 VB1) Abort predicate for x86_16_call_far's resume loop: stop resuming a
// long-running callee (e.g. the VB runtime's in-wndproc message loop) when a
// close has been latched (titlebar-X / F4 / PostQuitMessage).
extern volatile int g_win16_close_requested;
extern int g_quit_posted_get(void);
static int win16_callfar_should_abort(void) {
    return g_win16_close_requested || g_quit_posted_get();
}

int win16_run_file(const char *path) {
    g_cdt_init_seg = 0; g_cdt_init_off = 0;   // reset per run (stale-arm guard)
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, path, &size);
    if (!data || size == 0) {
        kprintf("[win16] cannot read %s\n", path);
        return -1;
    }
    uint8_t *f = (uint8_t *)data;

    // (#289 Phase1) Decide real-mode vs protected-mode for THIS run. Default is
    // real mode (regression-safe). A caller sets g_win16_want_pmode=1 (e.g. the
    // RC `win16pm` command) to load this NE under the selector/LDT model. We
    // enable BEFORE x86_16_init / segment load so segbase[] gets selectors and
    // every memory access this run routes through the LDT + arena.
    extern int g_win16_want_pmode;
    win16_pmode_enable(g_win16_want_pmode ? 1 : 0);   // resets LDT+arena when on

    // (#345) Screensaver mode. A "/s" (or "-s") command tail means run the NE as a
    // fullscreen screen saver. SCRNSAVE.LIB WinMain parses the tail: "/s"=run,
    // "/c"/none=config, "/p"=preview. We inject "/s" (WIN16ARG.TXT or the idle
    // launcher) so the saver's fullscreen path runs. Reset every run so a later
    // ordinary app is unaffected.
    {
        extern char g_win16_cmdtail[128];
        const char *ct = g_win16_cmdtail;
        while (*ct == ' ' || *ct == '\t') ct++;
        g_win16_scrsave = ((ct[0] == '/' || ct[0] == '-') &&
                           (ct[1] == 's' || ct[1] == 'S')) ? 1 : 0;
        if (g_win16_scrsave) {
            extern volatile unsigned long long timer_ticks;
            g_win16_scrsave_start = timer_ticks;
            kprintf("[win16] .SCR screensaver mode (/s): fullscreen top-most\n");
        }
    }

    for (uint32_t i = 0; i < WIN16_MEM_SIZE; i++) g_win16_mem[i] = 0;
    x86_16_init(&g_win16_cpu, g_win16_mem);
    x86_16_set_int_handler(win16_int);
    x86_16_set_farcall_trap(WIN16_THUNK_SEG, win16_api_dispatch);
    // (#256 VB1) Let a titlebar-X / F4 close break out of a long-running wndproc
    // (the VB runtime runs its message loop INSIDE the window procedure).
    x86_16_set_callfar_abort(win16_callfar_should_abort);
    g_import_count = 0;
    registry_reset();

    // Record the app directory (for relative file opens + DLL lookup).
    {
        int last = -1;
        for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
        if (last <= 0) { g_appdir[0] = '/'; g_appdir[1] = '\0'; }
        else {
            int n = last;
            if (n > (int)sizeof(g_appdir) - 1) n = sizeof(g_appdir) - 1;
            for (int i = 0; i < n; i++) g_appdir[i] = path[i];
            g_appdir[n] = '\0';
        }
    }
    // (#188 Word6) Publish the DOS-form module path so GetModuleFileName can return
    // it; installers parse it to find their source directory + SETUP.LST/*.INF.
    {
        extern void win16_set_dos_module_path(const char *native);
        win16_set_dos_module_path(path);
    }

    int is_ne = 0;

    if (size >= 0x40 && f[0] == 'M' && f[1] == 'Z') {
        uint32_t lfanew = (uint32_t)f[0x3C] | ((uint32_t)f[0x3D] << 8) |
                          ((uint32_t)f[0x3E] << 16) | ((uint32_t)f[0x3F] << 24);
        if (lfanew + 0x40 <= size && f[lfanew] == 'N' && f[lfanew + 1] == 'E') {
            is_ne = 1;
            // Derive the module base name from the path (for the registry).
            char base[WIN16_NAME_MAX]; int bn = 0, last = -1;
            for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
            for (int i = last + 1; path[i] && path[i] != '.' && bn < WIN16_NAME_MAX - 1; i++)
                base[bn++] = path[i];
            base[bn] = '\0';

            uint16_t next_para = WIN16_LOAD_SEG;
            win16_module_t *app = &g_modules[0];
            if (load_ne_module(app, base, f, size, &next_para, 0) != 0) {
                kprintf("[win16] NE load failed for %s\n", path);
                registry_reset(); return -1;
            }
            g_module_count = 1;

            // Pull in dependent DLLs, then apply ALL relocations (so app imports
            // can resolve to real DLL exports), then run DLL inits.
            load_dependencies(app, &next_para, 0);
            for (int i = 0; i < g_module_count; i++) apply_module_relocs(&g_modules[i]);
            { if (g_win16_pmode) {
                for (int mi = 0; mi < g_module_count; mi++) {
                    win16_module_t *mm = &g_modules[mi];
                    kprintf("[SEGB] %s segcount=%u autodata=%u cs_idx=%u ss_idx=%u\n", mm->name, mm->segcount, mm->autodata, mm->cs_idx, mm->ss_idx);
                    for (uint16_t s2 = 0; s2 < mm->segcount; s2++)
                        kprintf("[SEGB]   %s seg%u sel=%04x flags=%04x\n", mm->name, s2+1, mm->segbase[s2], mm->seg_flags[s2]);
                    if (ci_streq(mm->name, "COMPOBJ") && mm->segcount >= 17) {
                        uint16_t s1sel = mm->segbase[0];
                        kprintf("[RELCHK] COMPOBJ seg1 sel=%04x [0x36df]=%04x [0x1d7d]=%04x (expect DGROUP sel %04x)\n",
                                s1sel,
                                x86_16_rd16(&g_win16_cpu, s1sel, 0x36df),
                                x86_16_rd16(&g_win16_cpu, s1sel, 0x1d7d),
                                mm->segbase[16]);
                    }
                }
            } }
            module_fix_sp(app);
            for (int i = 0; i < g_module_count; i++) module_fix_sp(&g_modules[i]);

            kprintf("[win16] NE '%s': %u segs, entry %04x:%04x ss:sp %04x:%04x ds %04x, %d modules\n",
                    path, app->segcount, app->cs_para, app->ip, app->ss_para, app->sp,
                    app->ds_para, g_module_count);

            // Empty command line. In real mode it lives at a fixed low paragraph;
            // in pmode (#289) 0x0080 is not a valid selector, so back it with a
            // small arena-backed selector instead.
            // (EP3) Installers (BINXZ) LocalAlloc more than their NE-declared
            // heap; real Win16 grows the DGROUP on demand. Give the installer its
            // full 64 KiB DGROUP (stack at top, big local heap). Name-gated so
            // ordinary game apps keep their byte-identical geometry.
            if (ci_streq(app->name, "BINXZ")) app->sp = 0xFFFE;
            uint16_t cmdline_seg = 0x0080;
            if (g_win16_pmode) {
                uint32_t cb = win16_arena_alloc(0x100, 16);
                uint16_t cs = cb ? ldt_alloc(cb, 0xFF, 0) : 0;
                if (cs) cmdline_seg = cs;
            }
            {
                extern char g_win16_cmdtail[128];
                int _ct = 0; while (g_win16_cmdtail[_ct] && _ct < 126) _ct++;
                if (_ct > 0) {
                    for (int _i = 0; _i < _ct; _i++)
                        x86_16_wr8(&g_win16_cpu, cmdline_seg, (uint16_t)_i, (uint8_t)g_win16_cmdtail[_i]);
                    x86_16_wr8(&g_win16_cpu, cmdline_seg, (uint16_t)_ct, 0);
                    x86_16_wr8(&g_win16_cpu, 0x0080, 0x80, (uint8_t)_ct);
                    for (int _i = 0; _i < _ct; _i++)
                        x86_16_wr8(&g_win16_cpu, 0x0080, (uint16_t)(0x81 + _i), (uint8_t)g_win16_cmdtail[_i]);
                    x86_16_wr8(&g_win16_cpu, 0x0080, (uint16_t)(0x81 + _ct), 0x0D);
                    kprintf("[win16] injected cmdtail: '%s'\n", g_win16_cmdtail);
                } else {
                    x86_16_wr8(&g_win16_cpu, cmdline_seg, 0, 0);
                }
            }
            win16_loader_info_t li;
            li.hinstance = app->ds_para; li.hprev = 0;
            li.module_handle = app->cs_para;
            li.ss = app->ss_para; li.sp = app->sp; li.ds = app->ds_para;
            li.ne_stack = app->ne_stack;
            li.segcount = app->segcount;
            li.cmdline_seg = cmdline_seg; li.cmdline_off = 0;
            // Local (near) heap region inside DGROUP: starts just past the static
            // data and spans the NE-declared local-heap reservation, but never
            // crosses into the stack at the top of the segment (#200 SkiFree:
            // LocalAlloc must return real near offsets or the C startup writes its
            // object tables through a NULL base and trashes DGROUP + the stack).
            {
                uint16_t adata_dlen =
                    (app->autodata >= 1 && app->autodata <= app->segcount)
                    ? app->seg_dlen[app->autodata - 1] : 0;
                uint32_t lbase = ((uint32_t)adata_dlen + 0x0F) & ~0x0Fu;
                // (#256 VB1) A local heap must NEVER start at DGROUP offset 0:
                // offset 0 is the near-NULL pointer, so the first LocalAlloc there
                // returns 0, which the MS-C runtime reads as an allocation FAILURE
                // and aborts via FatalAppExit("C RUNTIME ERROR"). VB1.0 apps
                // (RODENT/GOFIGURE/TICTACDP) keep their data in VBRUN100, so their
                // own DGROUP has ~0 static data (adata_dlen==0 -> lbase==0). Reserve
                // the first paragraph so LocalAlloc returns a non-zero near offset.
                // Apps with real static data (FreeCell/Tetris/Chips/Word) already
                // have lbase>=0x10, so this guard is a no-op for them (byte-identical).
                if (lbase < 0x10) lbase = 0x10;
                uint32_t ltop  = lbase + (uint32_t)app->ne_heap;
                // (#148 Chips) Whether the stack lives inside DGROUP decides how far
                // the local heap may grow. When SS == DS (small/medium model) the
                // stack sits at the top of DGROUP and the heap must stop below it.
                // When the app has a SEPARATE stack segment (SS != DS) there is no
                // in-segment stack, so the local heap may run nearly to the top of
                // the 64 KB DGROUP. Chips Challenge declares a near-zero ne_heap but
                // calls LocalAlloc in WinMain; with the old (SS-relative) clamp its
                // heap collapsed to empty (lheap_top == lheap_base), LocalAlloc
                // returned 0, and Chips aborted with FatalAppExit "no main
                // procedure". Give every app a real, non-empty near heap.
                int stack_in_dgroup = (app->ss_para == app->ds_para);
                uint32_t ceiling;
                if (stack_in_dgroup) {
                    // Keep a guard below the initial SP so heap and stack don't meet.
                    ceiling = (app->sp > 0x400) ? (uint32_t)app->sp - 0x400 : 0;
                } else {
                    ceiling = 0xFFF0;   // no in-segment stack; use the whole segment
                }
                if (ceiling > 0xFFF0) ceiling = 0xFFF0;
                // (#278 Word6) When the stack lives inside DGROUP (SS==DS) and the NE
                // already declares a substantial local heap, DO NOT extend the heap up
                // into the stack region: the MS-C runtime's __LInit derives its arena
                // end marker (pLast) from GlobalSize(DGROUP), and an arena that runs
                // over the stack leaves a permanent gap the heap-walk never crosses
                // (Word6 local-heap-over-stack hang). Keep ltop = data_len + ne_heap
                // (which sits below the stack). Only extend when the NE heap is tiny
                // (e.g. Chips Challenge declares a near-zero ne_heap but still calls
                // LocalAlloc in WinMain), so those apps still get a usable near heap.
                int _ep_inst = ci_streq(app->name, "BINXZ");
                int heap_is_substantial = !_ep_inst && stack_in_dgroup &&
                    ((uint32_t)app->ne_heap >= 0x1000) && (ltop < ceiling);
                if (ltop < ceiling && !heap_is_substantial) ltop = ceiling;
                if (_ep_inst && app->sp > 0x2000) ltop = (uint32_t)app->sp - 0x2000;
                // (#278 Word6 pass13) Large apps (Word6 = 235 segs) exhaust the modest
                // ne_heap reserve deep in app-init; the MS-C runtime's malloc then tries
                // to GROW the DGROUP near heap, but for SS==DS the heap cannot extend past
                // the in-segment stack and the grow fails (GlobalReAlloc returns 0) ->
                // malloc(720) at seg196:0x444 returns NULL -> app-init returns failure ->
                // FatalAppExit "no main procedure". Give such large apps a much bigger
                // near-heap reserve, ending a safe 8 KB guard below the initial SP (the
                // observed Word app-init stack floor is ~SP-0x1400). pLast stays below the
                // stack so the arena remains coherent (LHFIX bridges the reserve as one
                // big FREE block; GlobalSize reports g_dgroup_heap_top = this ltop).
                // Gated to large apps so the tiny games (SkiFree/FreeCell/Chips, <50 segs)
                // keep their existing geometry.
                if (heap_is_substantial && app->segcount >= 100 && app->sp > 0x2800) {
                    // (#278 p20) Full 64 KiB DGROUP: the heap may grow UP to a small
                    // guard below the stack region. The stack occupies [sp-ne_stack, sp);
                    // keep ltop below that so heap and stack never meet. This replaces
                    // the old sp-0x2000 clamp, which (with the stack now at 0xFFFE)
                    // would have overlapped the stack.
                    uint32_t stack_floor = ((uint32_t)app->sp > (uint32_t)app->ne_stack)
                                           ? (uint32_t)app->sp - (uint32_t)app->ne_stack
                                           : (uint32_t)app->sp;
                    uint32_t big = (stack_floor > 0x800) ? stack_floor - 0x800 : stack_floor;
                    if (big > ltop) ltop = big;
                }
                if (lbase > 0xFFF0) lbase = 0xFFF0;
                if (ltop > 0xFFF0) ltop = 0xFFF0;
                if (ltop < lbase) ltop = lbase;
                // (#278 Word6 pass26) Carve a dedicated scratch window from the TOP
                // of the (over-large, mostly free) MS-C near heap for Word's shared
                // bump-allocator (cursor [DGROUP:0x0a]). The scratch occupies
                // [ltop-WIN16_W6_SCRATCH, ltop); reduce ltop so the MS-C heap and the
                // scratch are disjoint. The KERNEL.5 arena in win16_api_begin starts at
                // lheap_top+WIN16_W6_SCRATCH (= the OLD ltop), so all four regions are
                // disjoint: [static][MS-C heap][scratch][KERNEL.5 arena][stack].
                if (app->segcount >= 100 &&
                    ltop > lbase + WIN16_W6_SCRATCH + 0x1000) {
                    ltop -= WIN16_W6_SCRATCH;
                }
                li.lheap_base = (uint16_t)lbase;
                li.lheap_top  = (uint16_t)ltop;
            }
            // (#278 P55) synthetic printer-driver export thunk slots (handlers in
            // win16api.c). Registered before win16_api_begin so g_import_count covers them.
            win16_add_import("PRNDRV", "DEVICEMODE", 0, 0);
            win16_add_import("PRNDRV", "EXTDEVICEMODE", 0, 0);
            win16_add_import("PRNDRV", "DEVICECAPABILITIES", 0, 0);
            win16_add_import("PRNDRV", "GENERIC", 0, 0);
            win16_api_begin(&li, g_imports, g_import_count);

            // (#278 Word6 pass25) Seed the auto-data segment's MS-C near-heap base.
            // Word's __LInit (seg231:0) for the DGROUP (DS==SS) reads its near-heap
            // pStart from [DGROUP:0x0e] and stores the LOCALINFO descriptor pointer
            // there ([DGROUP:0x16] = [DGROUP:0x0e]). The auto-data file image has
            // [0x0e]=0 (it is a runtime-seeded value) and Word's C startup never
            // writes it, so the real Win16 loader must seed it. Without this the
            // DGROUP local-heap descriptor is built at offset 0 -> the MS-C near
            // malloc (seg231:0xff) returns 0 -> Word's object-table create
            // (seg85:0x1000 -> [DGROUP:0x2d8]) returns NULL -> a later teardown walk
            // over the NULL table recurses until the DGROUP stack overflows (the
            // pass-25 seg132/seg85 object-tree "recursion"). Gated to large apps
            // (Word = 235 segs) so the small games keep their existing geometry; the
            // near-heap window [lheap_base, lheap_top) is disjoint from the pass-24
            // dedicated C-runtime arena at [lheap_top, stack_floor).
            if (li.ds && li.segcount >= 100 && li.lheap_base) {
                x86_16_wr16(&g_win16_cpu, li.ds, 0x0e, li.lheap_base);
                // (#278 Word6 pass26) Seed the shared scratch bump-allocator cursor
                // [DGROUP:0x0a] to the carved scratch window base (= li.lheap_top).
                // Word's seg132:0x108e / WWINTL seg2:0xc2 bump-allocate from this
                // cursor; uninitialised (0) it handed back near-null DGROUP pointers
                // that callers memcpy'd into, clobbering [0x0e]/[0x16] -> seg231 INT 03.
                x86_16_wr16(&g_win16_cpu, li.ds, 0x0a, li.lheap_top);
            }

            // Classify the app so win16api's autostart picks the right strategy:
            // card games (loaded CARDS.DLL) deal via their F2 accelerator; TETRIS
            // uses its bespoke level/spawn kick; everything else stays generic.
            extern int g_win16_app_kind;
            if (module_by_name("CARDS")) g_win16_app_kind = 2;
            else if (ci_streq(app->name, "TETRIS")) g_win16_app_kind = 1;
            else if (ci_streq(app->name, "SKI")) g_win16_app_kind = 3; // SkiFree (#200)
            else g_win16_app_kind = 0;

            // Record the module map in the trace (begin resets the trace buffer).
            for (int i = 0; i < g_module_count; i++) {
                win16_module_t *m = &g_modules[i];
                win16_trace("module %d %-9s %s segs=%u rsctab=%u hinst=%04x cs=%04x:%04x\n",
                            i, m->name, m->is_dll ? "DLL" : "EXE", m->segcount,
                            m->rsctab, m->hinstance, m->cs_para, m->ip);
            }

            // Initialise dependent DLLs (LibEntry/LibMain) before the app runs.
            // x86_16_call_far saves/restores the register file around each call,
            // so set the app's initial state AFTER all DLL inits have run.
            for (int i = 1; i < g_module_count; i++) patch_dll_entry_prologues(&g_modules[i]);
            for (int i = 1; i < g_module_count; i++) run_dll_init(&g_modules[i]);

            g_win16_cpu.cs = app->cs_para; g_win16_cpu.ip = app->ip;
            g_win16_cpu.ds = app->ds_para; g_win16_cpu.es = app->ds_para;
            g_win16_cpu.ss = app->ss_para; g_win16_cpu.sp = app->sp;
            g_win16_cpu.ax = g_win16_cpu.bx = g_win16_cpu.cx = g_win16_cpu.dx = 0;
            g_win16_cpu.si = g_win16_cpu.di = g_win16_cpu.bp = 0;
            g_win16_cpu.halted = 0;
        } else {
            kprintf("[win16] DOS MZ .EXE not supported (use .COM or NE)\n");
            kfree(data); return -1;
        }
    } else {
        uint32_t n = size; if (n > 0xFE00) n = 0xFE00;
        for (uint32_t i = 0; i < n; i++)
            x86_16_wr8(&g_win16_cpu, WIN16_LOAD_SEG, (uint16_t)(0x100 + i), f[i]);
        g_win16_cpu.cs = WIN16_LOAD_SEG; g_win16_cpu.ip = 0x100;
        g_win16_cpu.ds = WIN16_LOAD_SEG; g_win16_cpu.es = WIN16_LOAD_SEG;
        g_win16_cpu.ss = WIN16_LOAD_SEG; g_win16_cpu.sp = 0xFFFE;
        kprintf("[win16] COM '%s': %u bytes at %04x:0100\n", path, size, WIN16_LOAD_SEG);
        kfree(data);
    }

    // Arm the cdtInit instruction trace only when explicitly enabled (it fills the
    // trace buffer). The DS-load fix is in place; leave the capability for future
    // diagnosis but off by default so the normal call trace stays readable.
    extern int g_win16_fntrace;
    if (is_ne && g_win16_fntrace && (g_cdt_init_seg || g_cdt_init_off))
        x86_16_set_fn_trace(g_cdt_init_seg, g_cdt_init_off);
    extern int g_win16_iptrace;
    if (is_ne && g_win16_fntrace) g_win16_iptrace = 1;

    // (#289 DIAG) far-call target log for pmode OLE2 probes.
    { extern int g_ole2_farlog; extern int g_ole2_k334log; (void)g_ole2_farlog; (void)g_ole2_k334log; /* (#278 pass36) diagnostics stripped: gates left OFF; the pass-13..36 FIX blocks gate on g_win16_pmode, not these */ }
    int r;
    // (#289 b468) Total-instruction runaway guard. A genuinely-stuck NE (e.g. an
    // OLE2 probe that crashes into a null far-call spin executing op=00 forever)
    // would otherwise loop here issuing 4M-insn slices indefinitely and starve
    // the whole OS (desktop + net wedged). Cap the cumulative instruction budget
    // for a single run so a runaway proc self-terminates and the OS stays live.
    unsigned long ne_budget = 0;
    const unsigned long NE_RUN_CAP = 80000000UL;   // ~80M insns WITHOUT progress
    // (#278 pass62) Forward-progress signal: every serviced Win16 API call bumps
    // g_win16_api_progress (win16api.c). The cap is meant to self-terminate a
    // genuinely-stuck runaway (a null far-call spin executing op=00 forever, which
    // makes NO API calls). A HEALTHY interactive app keeps pumping messages /
    // calling the API; so whenever progress advances during a slice we reset the
    // budget, letting the app run indefinitely (until the user closes it). This is
    // what keeps Word 6 alive past ~150s so its message pump stays live and typed
    // keystrokes reach the edit pane. Only a ZERO-progress slice counts toward the
    // cap, so a real runaway still dies.
    extern volatile unsigned long g_win16_api_progress;
    unsigned long last_progress = g_win16_api_progress;
    for (;;) {
        r = x86_16_run(&g_win16_cpu, 4000000UL);
        if (r != 1) break;                 // 0 = halted/exited, <0 = error
        if (!is_ne) break;                 // DOS/COM: keep old one-shot behaviour
        if (g_win16_api_progress != last_progress) {
            last_progress = g_win16_api_progress;   // app made API calls -> not stuck
            ne_budget = 0;
        } else {
            ne_budget += 4000000UL;                 // no API call this slice
        }
        if (ne_budget >= NE_RUN_CAP) {
            kprintf("[win16] NE run budget cap (%lu insns) reached at cs:ip=%04x:%04x -> halting (runaway/crash)\n",
                    NE_RUN_CAP, g_win16_cpu.cs, g_win16_cpu.ip);
            g_win16_cpu.halted = 1;
            r = 0;
            break;
        }
        if (g_win16_close_requested || g_quit_posted_get()) {
            // A quit is pending; give the guest a bounded chance to observe the
            // WM_QUIT and unwind WinMain cleanly, then stop regardless.
            int spins = 0;
            while (x86_16_run(&g_win16_cpu, 1000000UL) == 1 && spins++ < 8) { }
            break;
        }
    }
    if (is_ne)
        win16_trace("=== run end: r=%d exit=%d insns=%lu cs=%04x:%04x ===\n",
                    r, g_win16_cpu.exit_code, g_win16_cpu.insn_count,
                    g_win16_cpu.cs, g_win16_cpu.ip);
    if (is_ne) win16_api_end();
    kprintf("[win16] '%s' finished: r=%d exit=%d insns=%lu type=%s\n",
            path, r, g_win16_cpu.exit_code, g_win16_cpu.insn_count,
            is_ne ? "NE/Win16" : "COM/DOS");
    if (is_ne) registry_reset();   // frees app + DLL images
    return (r < 0) ? -1 : 0;
}
