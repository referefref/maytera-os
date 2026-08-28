// exec/le.c - #740 Milestones 1 and 2: the C surface of the Rust LE loader.
//
// This file does exactly three things, none of which is parsing:
//   * whole-file I/O (fat_read_file, the ONE routing whole-file read)
//   * printing (kprintf)
//   * arena allocation for the load test (kmalloc)
// Every byte of decoding happens in rustkern/le.rs. That split is the standing
// Rust-first rule; the justification for the C that IS here is that file I/O
// and the variadic formatter are C-only facilities in this kernel (see the
// "WHY C AND NOT RUST" note on kvformat in string.c for the same reasoning).
//
// WHY THE OUTPUT LOOKS LIKE THIS. The exit criterion for Milestone 1 is a
// serial dump cross-checked BYTE FOR BYTE against an independent hex dump, so
// every derived number is printed next to the raw bytes it came from: the page
// map entries are printed as raw bytes AND as a decoded page number, and the
// page-data base is printed as "mz + hdr[+0x80]" rather than as a bare result.
// A dump you cannot check against xxd is a dump you have to trust.
//
// AND THE FREE EXACT CHECK, from blame.md (2026-08-07): in a wbind'ed module
// the LE payload is last in the file, so filesize - ((numpages-1)*pagesize +
// lastpagesize) IS the page base. It is printed as "trailing after page data",
// which must be 0. A wrong page base is otherwise SILENT: it yields a module
// whose strings and interrupt histogram look perfectly healthy.

#include "le.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/fat.h"

extern fat_fs_t g_fat_fs;

// The default slide. MEASURED (docs/DOS4GW_LE_FORMAT.md section 5.2): every one
// of DOOM/DUKE3D/SW has an object covering the VGA aperture at 0xA0000 AND
// unrelocated literal 0xA0000 immediates in code that are NOT fixup sources.
// Both cannot be true at the linker's base, so the module must live above the
// low megabyte. Any page-aligned delta >= 1 MiB is correct as far as the module
// is concerned; what real DOS/4GW picks is still open.
#define LE_DEFAULT_DELTA 0x00100000u

// ---------------------------------------------------------------------------
// small printers
// ---------------------------------------------------------------------------

static void le_obj_attrs(uint32_t f, char *out, int outsz)
{
    int n = 0;
    out[0] = '\0';
    // R/W/X triple, then the two flags that actually change loader behaviour.
    if (n < outsz - 1) out[n++] = (f & LE_OBJ_READABLE)   ? 'R' : '-';
    if (n < outsz - 1) out[n++] = (f & LE_OBJ_WRITABLE)   ? 'W' : '-';
    if (n < outsz - 1) out[n++] = (f & LE_OBJ_EXECUTABLE) ? 'X' : '-';
    out[n] = '\0';
    if (f & LE_OBJ_BIG)         strncat(out, " BIG32",   (size_t)(outsz - 1 - (int)strlen(out)));
    if (f & LE_OBJ_ALIAS_16_16) strncat(out, " ALIAS16", (size_t)(outsz - 1 - (int)strlen(out)));
    if (f & LE_OBJ_PRELOAD)     strncat(out, " PRELOAD", (size_t)(outsz - 1 - (int)strlen(out)));
}

static const char *le_pageflag_name(uint32_t f)
{
    switch (f) {
    case 0: return "VALID";
    case 1: return "ITERATED";
    case 2: return "INVALID";
    case 3: return "ZEROED";
    case 4: return "RANGE";
    default: return "?";
    }
}

static const char *le_src_name(uint32_t t)
{
    switch (t) {
    case LE_SRC_BYTE:      return "byte";
    case LE_SRC_SEL16:     return "sel16";
    case LE_SRC_PTR16_16:  return "ptr16:16";
    case LE_SRC_OFF16:     return "off16";
    case LE_SRC_PTR16_32:  return "ptr16:32";
    case LE_SRC_OFF32:     return "off32";
    case LE_SRC_SELFREL32: return "selfrel32";
    default:               return "?";
    }
}

// One page map entry, printed as RAW BYTES and as the decoded 24-bit
// big-endian page number, so the decode can be checked against a hex dump
// without trusting this code.
static void le_print_page(const uint8_t *f, uint32_t flen, const le_image_t *img, uint32_t p)
{
    uint32_t num = 0, flags = 0, foff = 0, plen = 0, lin = 0;
    uint32_t raw = img->le_off + img->page_map_off + p * 4;
    // le_parse_rs already range-checked the whole page map, but this is the one
    // place C touches the file buffer directly, so it re-checks rather than
    // inheriting an invariant from another language across an FFI.
    if ((uint64_t)raw + 4 > (uint64_t)flen) {
        kprintf("[LE]   [%u] page map entry at 0x%08x is outside the file\n", p, raw);
        return;
    }
    int e = le_page_entry_rs(f, flen, img, p, &num, &flags);
    if (e != LE_OK) {
        kprintf("[LE]   [%u] page entry error: %s\n", p, le_strerror_rs(e));
        return;
    }
    int oi = le_page_object_rs(img, p);
    int el = le_page_linear_rs(img, p, &lin);
    int ef = le_page_file_off_rs(f, flen, img, p, &foff, &plen);
    kprintf("[LE]   [%u] @0x%08x raw=%02x %02x %02x %02x  num=%u flags=%u %s",
            p, raw,
            (unsigned)f[raw], (unsigned)f[raw + 1],
            (unsigned)f[raw + 2], (unsigned)f[raw + 3],
            num, flags, le_pageflag_name(flags));
    if (ef == LE_OK && plen)
        kprintf("  file=0x%08x len=%u", foff, plen);
    else if (ef == LE_OK)
        kprintf("  file=<none, zero-fill>");
    else
        kprintf("  file=<%s>", le_strerror_rs(ef));
    if (el == LE_OK)
        kprintf("  lin=0x%08x obj=%d", lin, oi + 1);
    kprintf("\n");
}

static void le_hexdump16(const uint8_t *p, uint32_t lin)
{
    char asc[17];
    kprintf("[LE]   0x%08x:", lin);
    for (int i = 0; i < 16; i++) {
        kprintf(" %02x", (unsigned)p[i]);
        asc[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
    }
    asc[16] = '\0';
    kprintf("  |%s|\n", asc);
}

// ---------------------------------------------------------------------------
// Milestone 1: le-info
// ---------------------------------------------------------------------------

static int le_info_buf(const char *path, const uint8_t *f, uint32_t flen)
{
    le_image_t *img = (le_image_t *)kmalloc(sizeof(le_image_t));
    le_hist_t  *h   = (le_hist_t  *)kmalloc(sizeof(le_hist_t));
    if (!img || !h) {
        if (img) kfree(img);
        if (h) kfree(h);
        kprintf("[LE] out of memory\n");
        return LE_E_MEM;
    }

    uint32_t mz = 0, le = 0;
    int e = le_find_rs(f, flen, &mz, &le);
    if (e != LE_OK) {
        kprintf("[LE] %s: %s\n", path, le_strerror_rs(e));
        kfree(img); kfree(h);
        return e;
    }

    e = le_parse_rs(f, flen, img);
    if (e != LE_OK) {
        kprintf("[LE] %s: parse failed: %s\n", path, le_strerror_rs(e));
        kfree(img); kfree(h);
        return e;
    }

    // The outer MZ's e_lfanew, printed to make the trap visible: on a wbind'ed
    // game this is garbage and anchoring on it produces plausible nonsense.
    uint32_t outer_lfa = 0;
    if (flen >= 0x40 && f[0] == 'M' && f[1] == 'Z')
        outer_lfa = (uint32_t)f[0x3c] | ((uint32_t)f[0x3d] << 8) |
                    ((uint32_t)f[0x3e] << 16) | ((uint32_t)f[0x3f] << 24);

    kprintf("[LE] ===== %s (%u bytes) =====\n", path, flen);
    kprintf("[LE] outer MZ @0x0 e_lfanew=0x%08x (NOT the anchor; ignored)\n", outer_lfa);
    kprintf("[LE] ANCHOR MZ=0x%08x  LE header=0x%08x\n", img->mz_off, img->le_off);
    kprintf("[LE] cpu=%u os=%u modflags=0x%08x pages=%u pagesize=%u lastpage=%u\n",
            img->cpu_type, img->os_type, img->mod_flags, img->num_pages,
            img->page_size, img->last_page_size);
    kprintf("[LE] entry obj=%u eip=0x%08x   stack obj=%u esp=0x%08x\n",
            img->eip_obj, img->eip, img->esp_obj, img->esp);
    kprintf("[LE] objtab=+0x%08x n=%u  pagemap=+0x%08x  fixpagetab=+0x%08x  fixrectab=+0x%08x\n",
            img->obj_tab_off, img->num_objects, img->page_map_off,
            img->fixup_page_tab_off, img->fixup_rec_tab_off);
    kprintf("[LE] imports: modtab=+0x%08x n=%u proctab=+0x%08x  fixupsect=%u loadersect=%u\n",
            img->import_mod_tab_off, img->num_import_mod,
            img->import_proc_tab_off, img->fixup_sect_size, img->loader_sect_size);

    // Two origins, printed as the arithmetic so it can be checked by hand.
    uint32_t hdr80 = img->page_data_abs - img->mz_off;
    kprintf("[LE] page data abs=0x%08x = anchorMZ 0x%08x + hdr[+0x80] 0x%08x   len=%u\n",
            img->page_data_abs, img->mz_off, hdr80, img->page_data_len);
    int64_t trailing = (int64_t)flen - ((int64_t)img->page_data_abs + img->page_data_len);
    kprintf("[LE] trailing bytes after page data: %d   (0 = payload ends at EOF; "
            "NONZERO is not automatically wrong, SW.EXE has 445812 bytes of Watcom debug info)\n",
            (int)trailing);
    kprintf("[LE] guest linear span: 0x%08x .. 0x%08x  (%u KiB)\n",
            img->lin_lo, img->lin_hi, (img->lin_hi - img->lin_lo) / 1024);

    kprintf("[LE] object table (%u entries, 24 bytes each, at file 0x%08x):\n",
            img->num_objects, img->le_off + img->obj_tab_off);
    for (uint32_t i = 0; i < img->num_objects; i++) {
        char attrs[32];
        const le_object_t *o = &img->obj[i];
        le_obj_attrs(o->flags, attrs, (int)sizeof(attrs));
        kprintf("[LE]  #%u vsize=0x%08x base=0x%08x flags=0x%08x pages %u..%u (%u)  %s\n",
                i + 1, o->virt_size, o->reloc_base, o->flags,
                o->page_index, o->page_index + o->page_count - 1, o->page_count,
                attrs);
        uint32_t filebytes = o->page_count * img->page_size;
        if (o->virt_size > filebytes)
            kprintf("[LE]      zero-fill tail: %u bytes (virt_size beyond the file pages)\n",
                    o->virt_size - filebytes);
    }

    kprintf("[LE] page map (at file 0x%08x, 4 bytes/entry, 24-bit BIG-ENDIAN page number):\n",
            img->le_off + img->page_map_off);
    for (uint32_t p = 0; p < img->num_pages && p < 6; p++)
        le_print_page(f, flen, img, p);
    // The entries that SETTLE the endianness question. Only a module with more
    // than 255 pages can tell the readings apart; on a smaller one this prints
    // nothing and that fact is itself worth seeing.
    if (img->num_pages > 256) {
        kprintf("[LE]  --- the entries that settle the endianness (index 254..257) ---\n");
        for (uint32_t p = 254; p < 258 && p < img->num_pages; p++)
            le_print_page(f, flen, img, p);
    } else {
        kprintf("[LE]  --- module has %u pages (<=256): CANNOT distinguish 24-bit BE from "
                "any LE reading. Not evidence either way. ---\n", img->num_pages);
    }
    if (img->num_pages > 6) {
        kprintf("[LE]  --- last 2 entries ---\n");
        for (uint32_t p = img->num_pages - 2; p < img->num_pages; p++)
            le_print_page(f, flen, img, p);
    }

    kprintf("[LE] fixup page table (at file 0x%08x, %u u32 entries):\n",
            img->le_off + img->fixup_page_tab_off, img->num_pages + 1);
    for (uint32_t p = 0; p < img->num_pages && p < 6; p++) {
        uint32_t s0 = 0, s1 = 0;
        if (le_fixup_page_range_rs(f, flen, img, p, &s0, &s1) == LE_OK)
            kprintf("[LE]   page %u: records [0x%x,0x%x) = %u bytes\n", p, s0, s1, s1 - s0);
    }

    e = le_fixup_hist_rs(f, flen, img, h);
    if (e != LE_OK) {
        kprintf("[LE] fixup walk failed: %s\n", le_strerror_rs(e));
        kfree(img); kfree(h);
        return e;
    }

    kprintf("[LE] FIXUP HISTOGRAM: %u records -> %u source entries over %u pages "
            "(busiest page: %u sources)\n",
            h->records, h->sources, h->pages_with_fixups, h->max_page_sources);
    kprintf("[LE]   by source type:");
    for (uint32_t i = 0; i < 16; i++)
        if (h->by_src[i]) kprintf("  0x%02x %s=%u", i, le_src_name(i), h->by_src[i]);
    kprintf("\n");
    kprintf("[LE]   by target type: internal=%u imp_ord=%u imp_name=%u int_entry=%u\n",
            h->by_tgt[0], h->by_tgt[1], h->by_tgt[2], h->by_tgt[3]);
    kprintf("[LE]   by record length:");
    for (uint32_t i = 0; i < 16; i++)
        if (h->by_rec_len[i]) kprintf("  %u%s=%u", i, (i == 15) ? "+" : "", h->by_rec_len[i]);
    kprintf("\n");
    kprintf("[LE]   source lists=%u  additive=%u  ord16=%u  tgt_off32=%u  tgt_off16=%u\n",
            h->src_list_recs, h->additive, h->ord16, h->tgt_off32, h->tgt_off16);
    kprintf("[LE]   negative src_off=%u (page-straddling continuations)  straddling writes=%u\n",
            h->neg_src_off, h->straddling);
    kprintf("[LE]   page flags: VALID=%u ITERATED=%u INVALID=%u ZEROED=%u RANGE=%u  "
            "pages with no file data=%u\n",
            h->by_page_flag[0], h->by_page_flag[1], h->by_page_flag[2],
            h->by_page_flag[3], h->by_page_flag[4], h->pages_no_data);

    // A handful of decoded records next to their file offsets, so the record
    // decode itself can be checked against a hex dump.
    kprintf("[LE] first 4 fixup sources of page 0:\n");
    for (uint32_t i = 0; i < 4; i++) {
        le_fixup_t fx;
        if (le_fixup_at_rs(f, flen, img, 0, i, &fx) != LE_OK) break;
        kprintf("[LE]   rec@0x%08x len=%u src=0x%02x(%s) flags=0x%02x tgt=obj%u+0x%08x "
                "src_off=%d\n",
                fx.rec_off, fx.rec_len, fx.src, le_src_name(fx.src_type),
                fx.flags, fx.tgt_obj, fx.tgt_off, fx.src_off);
    }

    kfree(img);
    kfree(h);
    return LE_OK;
}

int le_info_dump(const char *path)
{
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[LE] cannot read '%s'\n", path);
        return LE_E_TRUNCATED;
    }
    int e = le_info_buf(path, (const uint8_t *)data, sz);
    kfree(data);
    return e;
}

// ---------------------------------------------------------------------------
// Milestone 2: load, relocate, and prove it with the post-load invariant
// ---------------------------------------------------------------------------

// Read, parse, relocate and materialise a module. The arena is kmalloc'd and
// handed to the caller; nothing is allocated on any error path.
//
// This is the seam an execution core takes over from. It exists as a separate
// function from le_load_test() so that the 32-bit interpreter can get the same
// arena the boot self-test proved, without a second implementation and without
// re-deriving the entry point.
int le_load_into(const char *tag, const uint8_t *file, uint32_t size, uint32_t delta,
                 uint8_t *arena, uint32_t arena_size, uint32_t arena_base_lin,
                 le_module_t *out)
{
    if (!out || !file || !arena || size == 0 || arena_size == 0) return LE_E_MEM;
    memset(out, 0, sizeof(*out));

    int e = le_parse_rs(file, size, &out->img);
    if (e != LE_OK) {
        kprintf("[LE] LOAD %s: parse failed: %s\n", tag, le_strerror_rs(e));
        return e;
    }

    if (delta == 0) delta = LE_DEFAULT_DELTA;
    kprintf("[LE] LOAD %s: relocating by 0x%08x (linker base 0x%08x -> 0x%08x)\n",
            tag, delta, out->img.lin_lo, out->img.lin_lo + delta);
    e = le_relocate_rs(&out->img, delta);
    if (e != LE_OK) {
        kprintf("[LE] relocate failed: %s\n", le_strerror_rs(e));
        return e;
    }
    out->delta = delta;

    // The arena has to actually contain the relocated module. Checked HERE, in
    // the shared path, rather than trusted from a caller that computed a size:
    // a module whose top is one page past the buffer would otherwise be caught
    // only by le_load_rs refusing individual pages, which reads as a corrupt
    // file rather than as a caller's arithmetic error.
    if (out->img.lin_lo < arena_base_lin ||
        (uint64_t)out->img.lin_hi > (uint64_t)arena_base_lin + arena_size) {
        kprintf("[LE] arena 0x%08x..0x%08x does not contain the relocated module "
                "0x%08x..0x%08x\n",
                arena_base_lin, arena_base_lin + arena_size,
                out->img.lin_lo, out->img.lin_hi);
        return LE_E_OBJECT_RANGE;
    }

    e = le_load_rs(file, size, &out->img, arena, arena_size, arena_base_lin, &out->st);
    if (e != LE_OK) {
        kprintf("[LE] load FAILED: %s\n", le_strerror_rs(e));
        return e;
    }

    // THE POST-LOAD INVARIANT. One cheap readback that fails if the page base,
    // the page numbering, the record decode or the signed src_off is wrong,
    // because each of those scatters the targets.
    e = le_validate_rs(file, size, &out->img, arena, arena_size, arena_base_lin, &out->va);
    if (e != LE_OK) {
        kprintf("[LE] validate walk failed: %s\n", le_strerror_rs(e));
        return e;
    }

    out->arena = arena;
    out->arena_size = arena_size;
    out->base_lin = arena_base_lin;
    out->owns_arena = 0;          // BORROWED: le_free_module() must not free it
    out->lin_lo = out->img.lin_lo;
    out->lin_hi = out->img.lin_hi;
    out->entry_lin = out->img.obj[out->img.eip_obj - 1].reloc_base + out->img.eip;
    out->stack_lin = out->img.esp_obj
        ? (out->img.obj[out->img.esp_obj - 1].reloc_base + out->img.esp) : 0;
    return LE_OK;
}

int le_load_module(const char *path, uint32_t delta, le_module_t *out)
{
    if (!out) return LE_E_MEM;
    memset(out, 0, sizeof(*out));

    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[LE] cannot read '%s'\n", path);
        return LE_E_TRUNCATED;
    }
    const uint8_t *f = (const uint8_t *)data;

    // Sizing needs the header, and the header is parsed by le_load_into(). So
    // parse once here to size the arena, then let the shared path parse again
    // into `out`. The second parse is a few microseconds over an in-RAM buffer
    // and it keeps ONE sequence of parse/relocate/load/validate; a
    // parse-here-and-pass-the-image variant would have to keep the relocation
    // state consistent across the two halves by hand.
    le_image_t probe;
    int e = le_parse_rs(f, sz, &probe);
    if (e != LE_OK) {
        kprintf("[LE] LOAD %s: parse failed: %s\n", path, le_strerror_rs(e));
        kfree(data);
        return e;
    }
    uint32_t d = delta ? delta : LE_DEFAULT_DELTA;
    e = le_relocate_rs(&probe, d);
    if (e != LE_OK) {
        kprintf("[LE] relocate failed: %s\n", le_strerror_rs(e));
        kfree(data);
        return e;
    }

    uint32_t psz = probe.page_size;
    uint32_t base = probe.lin_lo & ~(psz - 1);
    // Round the arena up to a page so a short final page cannot make a
    // legitimate write fall outside the buffer.
    uint32_t need = ((probe.lin_hi - base) + psz - 1) & ~(psz - 1);
    uint8_t *arena = (uint8_t *)kmalloc(need);
    if (!arena) {
        kprintf("[LE] arena kmalloc(%u) FAILED\n", need);
        kfree(data);
        return LE_E_MEM;
    }
    memset(arena, 0, need);
    kprintf("[LE] arena: %u bytes representing guest linear 0x%08x..0x%08x\n",
            need, base, base + need);

    e = le_load_into(path, f, sz, d, arena, need, base, out);
    if (e != LE_OK) {
        kfree(arena);
        kfree(data);
        return e;
    }
    out->owns_arena = 1;   // WE allocated it, so le_free_module() frees it

    kfree(data);   // the file buffer is not needed once the arena is populated
    return LE_OK;
}

void le_free_module(le_module_t *m)
{
    if (!m) return;
    // Only free what WE allocated. le_load_into() borrows the caller's arena
    // and leaves owns_arena at 0, so a caller that loaded into its own buffer
    // and then called this out of habit gets a no-op instead of a double free.
    if (m->arena && m->owns_arena) kfree(m->arena);
    m->arena = 0;
    m->arena_size = 0;
    m->owns_arena = 0;
}

int le_load_test(const char *path, uint32_t delta,
                 uint32_t *out_entry_lin, uint32_t *out_stack_lin)
{
    le_module_t *m = (le_module_t *)kmalloc(sizeof(le_module_t));
    if (!m) {
        kprintf("[LE] out of memory\n");
        return LE_E_MEM;
    }
    int e = le_load_module(path, delta, m);
    if (e != LE_OK) {
        kfree(m);
        return e;
    }

    kprintf("[LE] loaded: %u pages copied (%u bytes), %u zero-fill pages, %u short page, "
            "%u bytes pre-zeroed\n",
            m->st.pages_copied, m->st.bytes_copied, m->st.pages_zeroed,
            m->st.pages_short, m->st.bytes_zeroed);
    kprintf("[LE] fixups applied: %u  (negative src_off %u, straddling %u, "
            "target past object end %u)\n",
            m->st.fixups_applied, m->st.fixups_negative_off, m->st.fixups_straddling,
            m->st.fixups_off_object);
    kprintf("[LE]   applied by source type:");
    for (uint32_t i = 0; i < 16; i++)
        if (m->st.fixups_by_src[i])
            kprintf("  0x%02x %s=%u", i, le_src_name(i), m->st.fixups_by_src[i]);
    kprintf("\n");

    kprintf("[LE] POST-LOAD INVARIANT: %u of %u 32-bit-offset fixups point inside a "
            "declared object (outside=%u, unreadable=%u)\n",
            m->va.inside_object, m->va.checked, m->va.outside, m->va.unreadable);
    if (m->va.outside)
        kprintf("[LE]   first bad: source lin 0x%08x wrote 0x%08x\n",
                m->va.first_bad_lin, m->va.first_bad_val);

    kprintf("[LE] entry CS:EIP = guest linear 0x%08x   SS:ESP = 0x%08x\n",
            m->entry_lin, m->stack_lin);

    // CONTENT proof that the page base is right. Only one of the three
    // candidate bases puts the Watcom runtime stub at the entry point, and it
    // does so in all three measured games. Arithmetic cannot tell you this;
    // bytes can.
    if (m->entry_lin >= m->base_lin &&
        (uint64_t)m->entry_lin + 16 <= (uint64_t)m->base_lin + m->arena_size) {
        kprintf("[LE] bytes at the entry point (expect EB xx 'WATCOM C/C++32' for a "
                "Watcom-linked module):\n");
        le_hexdump16(m->arena + (m->entry_lin - m->base_lin), m->entry_lin);
    }

    if (out_entry_lin) *out_entry_lin = m->entry_lin;
    if (out_stack_lin) *out_stack_lin = m->stack_lin;

    int ok = (m->va.checked > 0 && m->va.outside == 0 && m->va.unreadable == 0);
    kprintf("[LE] LOAD RESULT: %s\n", ok ? "PASS (loaded, relocated, invariant satisfied)"
                                         : "FAIL");
    le_free_module(m);
    kfree(m);
    return ok ? LE_OK : LE_E_FIXUP_TARGET;
}

// ---------------------------------------------------------------------------
// boot hook
// ---------------------------------------------------------------------------

// Runs the Rust fixture self-test, then anything /CONFIG/LEINFO.CFG asks for.
// Config format, one directive per line:
//     # comment
//     INFO /DOS/DOOM.EXE      parse and print (Milestone 1)
//     LOAD /DOS/DOOM.EXE      also load, relocate and validate (Milestone 2)
//     /DOS/DOOM.EXE           bare path = INFO
// Absent the file this prints one line and returns, so the shipping golden
// pays the fixture self-test and nothing else.
void le_boot_selftest(void)
{
    // Struct layout, checked at RUN time on the build that shipped. The
    // _Static_asserts in le.h pin the C side at compile time; this pins Rust to
    // the same numbers, which is the half a header cannot prove on its own.
    uint32_t ri = le_sizeof_image_rs(), rf = le_sizeof_fixup_rs();
    uint32_t rh = le_sizeof_hist_rs(), rs = le_sizeof_stats_rs();
    uint32_t rv = le_sizeof_valid_rs();
    int layout_ok = (ri == sizeof(le_image_t) && rf == sizeof(le_fixup_t) &&
                     rh == sizeof(le_hist_t) && rs == sizeof(le_stats_t) &&
                     rv == sizeof(le_valid_t));
    kprintf("[LE-SELFTEST] struct layout C/Rust: image %u/%u fixup %u/%u hist %u/%u "
            "stats %u/%u valid %u/%u -> %s\n",
            (uint32_t)sizeof(le_image_t), ri, (uint32_t)sizeof(le_fixup_t), rf,
            (uint32_t)sizeof(le_hist_t), rh, (uint32_t)sizeof(le_stats_t), rs,
            (uint32_t)sizeof(le_valid_t), rv, layout_ok ? "MATCH" : "MISMATCH");

    uint8_t *scratch = (uint8_t *)kmalloc(0x4000);
    if (!scratch) {
        kprintf("[LE-SELFTEST] scratch kmalloc failed; fixtures SKIPPED\n");
    } else {
        int r = le_selftest_rs(scratch, 0x4000);
        kfree(scratch);
        kprintf("[LE-SELFTEST] fixtures: %s (code %d) - last-MZ anchor, 24-bit BE page "
                "number at index 256, signed src_off, 7- vs 9-byte record length, "
                "truncated record rejected\n",
                (r == 0) ? "PASS" : "FAIL", r);
    }

    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/LEINFO.CFG", &sz);
    if (!cfg || sz == 0) {
        if (cfg) kfree(cfg);
        kprintf("[LE] no /CONFIG/LEINFO.CFG; not dumping any module this boot\n");
        return;
    }

    uint32_t i = 0;
    while (i < sz) {
        char line[160];
        uint32_t n = 0;
        while (i < sz && cfg[i] != '\n' && cfg[i] != '\r') {
            if (n < sizeof(line) - 1) line[n++] = cfg[i];
            i++;
        }
        while (i < sz && (cfg[i] == '\n' || cfg[i] == '\r')) i++;
        line[n] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        int do_load = 0;
        if (strncmp(p, "LOAD ", 5) == 0) { do_load = 1; p += 5; }
        else if (strncmp(p, "INFO ", 5) == 0) { p += 5; }
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;
        // trim trailing whitespace
        int L = (int)strlen(p);
        while (L > 0 && (p[L - 1] == ' ' || p[L - 1] == '\t')) p[--L] = '\0';

        le_info_dump(p);
        if (do_load) le_load_test(p, 0, 0, 0);
    }
    kfree(cfg);
}
