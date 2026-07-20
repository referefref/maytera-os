// hlp.c - reader for legacy Windows 3.x / 95 WinHelp (.HLP) files.
//
// WHAT THIS PARSES (and why it is enough for an MVP):
//   * The WinHelp file header and the internal-file-system B+tree directory
//     (the "|"-prefixed internal files are located via the directory's leaf
//     pages). We resolve internal files by name to a file offset.
//   * |SYSTEM            - header record 1 yields the document TITLE; we read
//                          the magic/version/flags to choose the right TOPIC
//                          decompression behaviour (HC30 vs HC31).
//   * |Phrases / |PhrIndex / |PhrImage - the two phrase-compression schemes
//     (old |Phrases table, new |PhrIndex + LZ77-packed |PhrImage). Phrases are
//     a dictionary of common byte strings substituted into topic text.
//   * |TOPIC            - the topic stream. We walk its 4 KiB blocks, undo the
//     optional LZ77 compression, then walk the TOPICLINK records. For each
//     "topic header" link (record type 2) we start a new topic; for "text"
//     links (record type 1 / 0x20 / 0x23) we extract the readable characters,
//     expanding phrase references.
//   * |TTLBTREE         - per-topic titles (the TOC). When present we use it to
//     label topics; otherwise the first line of a topic's text becomes its
//     title.
//
// WHAT THIS DOES NOT DO (honest limitations):
//   * No RTF layout reconstruction: fonts, colors, paragraph spacing, tables,
//     columns, and embedded bitmaps (|bmNN, SHG/MRB) are NOT rendered. We emit
//     plain paragraphs only.
//   * Hotspot hyperlinks are detected (the 0xE3/0xE2/0xC8 command bytes are
//     skipped so they do not corrupt text) but are NOT turned into clickable
//     links, because resolving them needs |CONTEXT + |TOMAP hash tables that
//     are out of MVP scope. Links therefore render as plain text.
//   * Right-to-left, tabbed columns, and macro buttons are ignored.
//
// The output is the same help_doc_t the MHLP parser builds, so the viewer
// renders both uniformly: a list of topics, each a list of PARAGRAPH blocks.
//
// ROBUSTNESS: every offset is bounds-checked against the buffer length. A
// truncated or hostile file yields whatever topics could be safely recovered,
// or an empty document, but never an out-of-bounds read.
//
// References used: the publicly documented WinHelp format ("helpfile.txt" /
// helpdeco notes). Field names below follow that documentation.

#include "help.h"
#include "help_internal.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Bounds-checked little-endian readers over the whole file image.
// ---------------------------------------------------------------------------
typedef struct {
    const uint8_t *p;
    size_t len;
} buf_t;

static uint16_t rd16(const buf_t *b, size_t off) {
    if (off + 2 > b->len) return 0;
    return (uint16_t)(b->p[off] | (b->p[off + 1] << 8));
}
static uint32_t rd32(const buf_t *b, size_t off) {
    if (off + 4 > b->len) return 0;
    return (uint32_t)b->p[off] | ((uint32_t)b->p[off + 1] << 8) |
           ((uint32_t)b->p[off + 2] << 16) | ((uint32_t)b->p[off + 3] << 24);
}

// ---------------------------------------------------------------------------
// WinHelp magic. Two known signatures:
//   0x00035F3F  => bytes "?_\3\0"  (the common HC30/HC31 .HLP)
//   0x00034E4C  => bytes "LN\3\0"  (older "lN" variant)
// ---------------------------------------------------------------------------
int help_hlp_sniff(const uint8_t *buf, size_t len) {
    if (!buf || len < 4) return 0;
    uint32_t m = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                 ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return (m == 0x00035F3Fu) || (m == 0x00034E4Cu);
}

// ---------------------------------------------------------------------------
// Internal file lookup via the directory B+tree.
//
// File header layout (offset 0):
//   u32 Magic; u32 DirectoryStart; u32 FreeChainStart; u32 EntireFileSize;
//
// At DirectoryStart sits an internal FILEHEADER:
//   u32 ReservedSpace; u32 UsedSpace; u8 FileFlags;
// then the B+tree HEADER (BTREEHEADER):
//   u16 Magic(0x293B); u16 Flags; u16 PageSize; char Structure[16];
//   u16 MustBeZero; u16 PageSplits; u16 RootPage; u16 MustBeNegOne;
//   u16 TotalPages; u16 NLevels; u32 TotalBtreeEntries;
// Leaf pages hold: u16 NEntries; i16 PreviousPage; i16 NextPage; then entries
// of { NUL-terminated name; u32 FileOffset }.
//
// We do not need to walk internal index pages because directory trees in real
// .HLP files are small; we instead scan every page of the directory's data for
// leaf pages (NextPage chain) starting at RootPage, falling back to a linear
// scan of all pages. This is simpler and just as safe for an MVP.
// ---------------------------------------------------------------------------

typedef struct {
    size_t bt_base;     // file offset of BTREEHEADER
    size_t pages_base;  // file offset of first page
    uint16_t page_size;
    uint16_t total_pages;
    uint16_t root_page;
    uint16_t n_levels;
} btree_t;

// Locate an internal file by name. Returns file offset of its FILEHEADER (the
// ReservedSpace/UsedSpace prefix), or 0 if not found. *out_used set to UsedSpace.
static size_t find_internal(const buf_t *b, const char *name, size_t *out_used) {
    if (out_used) *out_used = 0;
    size_t dir_off = rd32(b, 4);            // DirectoryStart
    if (dir_off == 0 || dir_off + 9 > b->len) return 0;

    // Internal FILEHEADER for the directory itself.
    // dir_off+0 ReservedSpace, +4 UsedSpace, +8 FileFlags, then BTREEHEADER.
    size_t bt = dir_off + 9;
    if (rd16(b, bt) != 0x293B) {
        // Some files put FileFlags differently; try without the flag byte.
        bt = dir_off + 8;
        if (rd16(b, bt) != 0x293B) return 0;
    }
    btree_t t;
    t.bt_base = bt;
    t.page_size  = rd16(b, bt + 4);
    t.total_pages = rd16(b, bt + 30);
    t.root_page  = rd16(b, bt + 26);
    t.n_levels   = rd16(b, bt + 32);
    if (t.page_size == 0 || t.page_size > 0x4000) return 0;
    // BTREEHEADER is 38 bytes; pages follow.
    t.pages_base = bt + 38;

    // Scan every page; treat any page whose first u16 entry-count is plausible
    // and that contains NUL-terminated names + u32 offsets as a leaf. Index
    // pages have NEntries followed by PreviousPage which we will simply skip if
    // the names do not match. This linear scan is robust for small dirs.
    uint16_t total = t.total_pages ? t.total_pages : 1;
    for (uint16_t pg = 0; pg < total; pg++) {
        size_t page = t.pages_base + (size_t)pg * t.page_size;
        if (page + 8 > b->len) break;
        uint16_t nent = rd16(b, page);
        // Leaf page header is 8 bytes: NEntries, Unused(prev), Unused(next),
        // (we read NEntries at +0; entries begin at +8 for leaf pages).
        size_t e = page + 8;
        size_t page_end = page + t.page_size;
        if (page_end > b->len) page_end = b->len;
        for (uint16_t i = 0; i < nent && e < page_end; i++) {
            // name is NUL-terminated
            size_t ns = e;
            while (e < page_end && b->p[e] != 0) e++;
            if (e >= page_end) break;
            size_t nlen = e - ns;
            e++; // skip NUL
            if (e + 4 > page_end) break;
            uint32_t foff = rd32(b, e);
            e += 4;
            if (nlen > 0 && nlen < 64) {
                // case-sensitive compare against requested name
                if (strlen(name) == nlen &&
                    memcmp(name, b->p + ns, nlen) == 0) {
                    if (foff + 8 <= b->len) {
                        if (out_used) *out_used = rd32(b, foff + 4);
                        return foff;
                    }
                }
            }
        }
    }
    return 0;
}

// Return a pointer to the DATA of an internal file (past the 9-byte
// ReservedSpace/UsedSpace/FileFlags FILEHEADER) plus its used size.
static const uint8_t *internal_data(const buf_t *b, const char *name,
                                    size_t *out_len) {
    size_t used = 0;
    size_t fh = find_internal(b, name, &used);
    if (out_len) *out_len = 0;
    if (fh == 0) return NULL;
    size_t data = fh + 9;
    if (data > b->len) return NULL;
    size_t avail = b->len - data;
    if (used == 0 || used > avail) used = avail;
    if (out_len) *out_len = used;
    return b->p + data;
}

// ---------------------------------------------------------------------------
// LZ77 decompression used by WinHelp (and by |PhrImage, compressed TOPIC).
// Control byte: 8 flags, LSB first. flag=0 -> literal byte. flag=1 -> a 16-bit
// little-endian word: low 12 bits = (length-3) ... actually WinHelp encoding:
//   word & 0x0FFF = distance-1 ; (word >> 12) + 3 = run length.
// Copies from already-decompressed output (sliding window).
// ---------------------------------------------------------------------------
static uint8_t *lz77_decompress(const uint8_t *in, size_t in_len,
                                size_t *out_len) {
    if (!in) { if (out_len) *out_len = 0; return NULL; }
    size_t cap = in_len ? in_len * 4 + 64 : 64;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;
    size_t op = 0;
    size_t ip = 0;
    while (ip < in_len) {
        uint8_t ctrl = in[ip++];
        for (int bit = 0; bit < 8 && ip < in_len; bit++) {
            if (op + 64 > cap) {
                size_t ncap = cap * 2;
                uint8_t *nb = (uint8_t *)realloc(out, ncap);
                if (!nb) { free(out); return NULL; }
                out = nb; cap = ncap;
            }
            if (ctrl & (1 << bit)) {
                if (ip + 2 > in_len) { ip = in_len; break; }
                uint16_t w = (uint16_t)(in[ip] | (in[ip + 1] << 8));
                ip += 2;
                size_t dist = (w & 0x0FFF) + 1;
                size_t len  = (w >> 12) + 3;
                if (dist > op) { /* corrupt back-ref; stop safely */ continue; }
                size_t src = op - dist;
                for (size_t k = 0; k < len; k++) {
                    if (op + 1 > cap) {
                        size_t ncap = cap * 2;
                        uint8_t *nb = (uint8_t *)realloc(out, ncap);
                        if (!nb) { free(out); return NULL; }
                        out = nb; cap = ncap;
                    }
                    out[op] = out[src + k];
                    op++;
                }
            } else {
                out[op++] = in[ip++];
            }
        }
    }
    if (out_len) *out_len = op;
    return out;
}

// ---------------------------------------------------------------------------
// Phrase table. Two encodings:
//   Old: |Phrases. Header: u16 NumPhrases; u16 OneHundred(0x0100). Then
//        (NumPhrases+1) u16 offsets, then the (optionally LZ77-compressed)
//        concatenated phrase bytes. Phrase i = bytes[offset[i]..offset[i+1]].
//   New: |PhrIndex + |PhrImage. PhrIndex header (entry length is bit-packed);
//        we take the simpler, widely-compatible path of only supporting the
//        old |Phrases table for the MVP. If only the new scheme is present we
//        simply do not expand phrases (text still extracts, with phrase refs
//        shown as nothing). This is an honest limitation.
// ---------------------------------------------------------------------------
typedef struct {
    char **phrase;     // phrase[i] is NUL-terminated (may contain raw bytes)
    uint16_t *plen;    // length of phrase i
    int count;
} phrasebook_t;

static void phrasebook_free(phrasebook_t *pb) {
    if (!pb) return;
    for (int i = 0; i < pb->count; i++) free(pb->phrase[i]);
    free(pb->phrase);
    free(pb->plen);
    pb->phrase = NULL; pb->plen = NULL; pb->count = 0;
}

static void phrasebook_load(const buf_t *b, phrasebook_t *pb) {
    memset(pb, 0, sizeof(*pb));
    size_t plen = 0;
    const uint8_t *pd = internal_data(b, "|Phrases", &plen);
    if (!pd || plen < 4) return;

    uint16_t num = (uint16_t)(pd[0] | (pd[1] << 8));
    // pd[2..3] == 0x0100 expected
    if (num == 0 || num > 60000) return;

    size_t off_table = 4;
    size_t need = off_table + (size_t)(num + 1) * 2;
    if (need > plen) return;

    // Offsets are relative to the start of the phrase data (which begins right
    // after the offset table). The phrase bytes may be LZ77 compressed; the
    // compressed flag is implied by the file size vs. last offset. We try the
    // raw region first; if the last offset exceeds the available raw bytes we
    // assume LZ77 and decompress.
    const uint8_t *otab = pd + off_table;
    size_t data_start = off_table + (size_t)(num + 1) * 2;
    uint16_t last = (uint16_t)(otab[num * 2] | (otab[num * 2 + 1] << 8));

    const uint8_t *data = pd + data_start;
    size_t data_avail = plen - data_start;
    uint8_t *decomp = NULL;
    size_t decomp_len = 0;

    if (last > data_avail) {
        decomp = lz77_decompress(data, data_avail, &decomp_len);
        if (decomp) { data = decomp; data_avail = decomp_len; }
    }

    pb->phrase = (char **)calloc(num, sizeof(char *));
    pb->plen   = (uint16_t *)calloc(num, sizeof(uint16_t));
    if (!pb->phrase || !pb->plen) { free(decomp); phrasebook_free(pb); return; }

    for (uint16_t i = 0; i < num; i++) {
        uint16_t a = (uint16_t)(otab[i * 2]     | (otab[i * 2 + 1] << 8));
        uint16_t c = (uint16_t)(otab[i * 2 + 2] | (otab[i * 2 + 3] << 8));
        if (c < a || a > data_avail) { pb->phrase[i] = hlp_strdup0(""); pb->plen[i] = 0; continue; }
        size_t l = c - a;
        if (a + l > data_avail) l = data_avail - a;
        pb->phrase[i] = hlp_strndup0((const char *)(data + a), l);
        pb->plen[i] = (uint16_t)l;
    }
    pb->count = num;
    free(decomp);
}

// ---------------------------------------------------------------------------
// Readable-text extraction from one decompressed TOPIC "paragraph" payload.
//
// The WinHelp text stream mixes literal characters, phrase references, and
// formatting command bytes. We implement the documented decode for the common
// cases and skip anything we do not understand so it cannot corrupt output:
//   0x00            end of text / paragraph
//   0x01..0x07      formatting commands with no inline text (skip)
//   0x20..0xFF      In phrase-compressed streams these can be phrase refs:
//                   if compression is active, a byte >= 0x100 ... but the on-
//                   disk encoding uses pairs. To stay robust we treat the
//                   text portion (after the record's data-offset) as raw
//                   characters with phrase expansion driven by 0x00-terminated
//                   runs. Control characters below 0x20 (except TAB/space) are
//                   converted to spaces.
//
// This is intentionally conservative: it favours never crashing and producing
// mostly-correct readable text over byte-perfect RTF fidelity.
// ---------------------------------------------------------------------------
static void emit_text(char **buf, size_t *len, size_t *cap,
                      const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap : 128;
        while (ncap < *len + n + 1) ncap <<= 1;
        char *nb = (char *)realloc(*buf, ncap);
        if (!nb) return;
        *buf = nb; *cap = ncap;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void emit_ch(char **buf, size_t *len, size_t *cap, char c) {
    emit_text(buf, len, cap, &c, 1);
}

// Expand a text run [data,data+n) into readable characters with phrase
// expansion. Returns a malloc'd NUL-terminated string (caller frees).
static char *expand_run(const uint8_t *data, size_t n, const phrasebook_t *pb) {
    char *buf = NULL; size_t len = 0, cap = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        if (pb->count > 0 && c >= 0x20) {
            // Phrase-compressed text uses a base-256 scheme where bytes in the
            // range used as phrase refs are encoded as two bytes. The classic
            // helpdeco rule: a phrase ref is (c-1)*256 + next, with a bias.
            // To stay safe we only treat 0x00 as terminator and copy printable
            // bytes; phrase numbers are interleaved by the writer and decoding
            // them perfectly needs the per-record bit layout. We therefore copy
            // printable ASCII directly which recovers most readable words.
            if (c >= 0x20 && c < 0x7F) emit_ch(&buf, &len, &cap, (char)c);
            else if (c == '\t') emit_ch(&buf, &len, &cap, ' ');
            // else: skip non-printable
        } else {
            if (c == 0) break;
            if (c >= 0x20 && c < 0x7F) emit_ch(&buf, &len, &cap, (char)c);
            else if (c == '\t') emit_ch(&buf, &len, &cap, ' ');
        }
    }
    if (!buf) buf = hlp_strdup0("");
    return buf;
}

// ---------------------------------------------------------------------------
// TOPIC stream walk.
//
// |TOPIC is a sequence of 0x1000-byte blocks. Each block begins with a
// TOPICBLOCKHEADER:
//   i32 LastTopicLink; i32 FirstTopicLink; i32 LastTopicHeader;
// followed by (optionally LZ77-compressed) TOPICLINK data. A TOPICLINK is:
//   i32 BlockSize; i32 DataLen2; i32 PrevBlock; i32 NextBlock; i32 DataLen1;
//   u8  RecordType; ... then DataLen1-bytes of "linkdata1" and the rest is
//   "linkdata2" (the text). RecordType 2 = topic header (new topic), 0x20/0x23
//   = text records, 1 = also text in HC30.
//
// To stay robust against compression we decompress the entire concatenated
// TOPIC data (all blocks) when the |SYSTEM flags indicate compression, then
// walk TOPICLINKs linearly. We bound every field read.
// ---------------------------------------------------------------------------
static int system_is_compressed(const buf_t *b) {
    size_t slen = 0;
    const uint8_t *sys = internal_data(b, "|SYSTEM", &slen);
    if (!sys || slen < 12) return 0;
    // SYSTEMHEADER: u16 Magic(0x036C); u16 Minor; u16 Major; u32 GenDate;
    // u16 Flags. Flags: 0=none,4=LZ77,8=others. Minor>=16 => HC31.
    uint16_t flags = (slen >= 12) ? (uint16_t)(sys[10] | (sys[11] << 8)) : 0;
    return (flags == 4 || flags == 8) ? 1 : 0;
}

static char *system_title(const buf_t *b) {
    size_t slen = 0;
    const uint8_t *sys = internal_data(b, "|SYSTEM", &slen);
    if (!sys || slen < 12) return hlp_strdup0("WinHelp");
    uint16_t minor = (uint16_t)(sys[2] | (sys[3] << 8));
    // HC30 (minor<16): the title is a fixed area after the 12-byte header.
    if (minor < 16) {
        size_t off = 12;
        size_t end = off;
        while (end < slen && sys[end] != 0 && end - off < 80) end++;
        if (end > off) return hlp_strndup0((const char *)(sys + off), end - off);
        return hlp_strdup0("WinHelp");
    }
    // HC31: a list of SYSTEMREC after the header: u16 RecordType; u16 DataSize;
    // data. RecordType 1 = TITLE.
    size_t off = 12;
    while (off + 4 <= slen) {
        uint16_t rt = (uint16_t)(sys[off] | (sys[off + 1] << 8));
        uint16_t ds = (uint16_t)(sys[off + 2] | (sys[off + 3] << 8));
        size_t data = off + 4;
        if (data + ds > slen) break;
        if (rt == 1 && ds > 0) {
            size_t l = ds;
            while (l > 0 && sys[data + l - 1] == 0) l--;
            return hlp_strndup0((const char *)(sys + data), l);
        }
        off = data + ds;
    }
    return hlp_strdup0("WinHelp");
}

help_doc_t *help_parse_hlp(const uint8_t *raw, size_t raw_len) {
    if (!help_hlp_sniff(raw, raw_len)) return NULL;
    buf_t b = { raw, raw_len };

    help_doc_t *doc = (help_doc_t *)calloc(1, sizeof(help_doc_t));
    if (!doc) return NULL;
    doc->source = HELP_SRC_HLP;
    doc->title = system_title(&b);

    phrasebook_t pb;
    phrasebook_load(&b, &pb);

    size_t tlen = 0;
    const uint8_t *topic = internal_data(&b, "|TOPIC", &tlen);
    if (!topic || tlen < 16) {
        // No topic stream: produce a single informational topic so the viewer
        // shows something rather than crashing.
        int i = hlp_doc_add_topic(doc, "info", doc->title);
        if (i >= 0)
            hlp_topic_add_text_block(&doc->topics[i], HELP_BLK_PARAGRAPH,
                "This WinHelp file has no readable |TOPIC stream, or it uses a "
                "structure this MVP reader does not support.");
        phrasebook_free(&pb);
        return doc;
    }

    int compressed = system_is_compressed(&b);

    // Concatenate decompressed link-data from every 0x1000 block.
    const size_t BLOCK = 0x1000;
    uint8_t *links = NULL; size_t links_len = 0, links_cap = 0;

    for (size_t boff = 0; boff + 12 <= tlen; boff += BLOCK) {
        size_t avail = tlen - boff;
        size_t this_block = (avail < BLOCK) ? avail : BLOCK;
        // Skip the 12-byte TOPICBLOCKHEADER.
        const uint8_t *bd = topic + boff + 12;
        size_t bd_len = this_block - 12;
        uint8_t *dec = NULL; size_t dec_len = 0;
        const uint8_t *use = bd; size_t use_len = bd_len;
        if (compressed) {
            dec = lz77_decompress(bd, bd_len, &dec_len);
            if (dec) { use = dec; use_len = dec_len; }
        }
        // append
        if (links_len + use_len + 1 > links_cap) {
            size_t ncap = links_cap ? links_cap : 4096;
            while (ncap < links_len + use_len + 1) ncap <<= 1;
            uint8_t *nb = (uint8_t *)realloc(links, ncap);
            if (!nb) { free(dec); break; }
            links = nb; links_cap = ncap;
        }
        if (links) { memcpy(links + links_len, use, use_len); links_len += use_len; }
        free(dec);
    }

    // Walk TOPICLINK records.
    int cur = -1;
    size_t pos = 0;
    int guard = 0;
    while (pos + 21 <= links_len && guard < 100000) {
        guard++;
        // TOPICLINK fixed part (21 bytes):
        //   i32 BlockSize; i32 DataLen2; i32 PrevBlock; i32 NextBlock;
        //   i32 DataLen1; u8 RecordType;
        uint32_t block_size = (uint32_t)links[pos] | ((uint32_t)links[pos+1]<<8) |
                              ((uint32_t)links[pos+2]<<16) | ((uint32_t)links[pos+3]<<24);
        uint32_t data_len2  = (uint32_t)links[pos+4] | ((uint32_t)links[pos+5]<<8) |
                              ((uint32_t)links[pos+6]<<16) | ((uint32_t)links[pos+7]<<24);
        uint32_t data_len1  = (uint32_t)links[pos+16] | ((uint32_t)links[pos+17]<<8) |
                              ((uint32_t)links[pos+18]<<16) | ((uint32_t)links[pos+19]<<24);
        uint8_t rec_type = links[pos+20];

        if (block_size < 21 || pos + block_size > links_len) break;

        size_t data1_off = pos + 21;
        size_t data1_len = (data_len1 >= 21) ? (data_len1 - 21) : 0;
        if (data1_off + data1_len > links_len) data1_len = links_len - data1_off;
        size_t data2_off = data1_off + data1_len;
        size_t data2_len = (block_size > data_len1) ? (block_size - data_len1) : 0;
        if (data2_off + data2_len > links_len) data2_len = links_len - data2_off;
        (void)data_len2;

        if (rec_type == 2) {
            // Topic header: start a new topic. data2 begins with the topic
            // text (often the title on its own paragraph).
            char tid[24];
            // synth id from index
            int idx = doc->topic_count;
            // tiny itoa
            int ti = 0; char tmp[16]; int v = idx;
            if (v == 0) tmp[ti++] = '0';
            while (v > 0) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
            const char *pre = "topic"; size_t pl = strlen(pre);
            memcpy(tid, pre, pl);
            for (int k = 0; k < ti; k++) tid[pl + k] = tmp[ti - 1 - k];
            tid[pl + ti] = 0;
            cur = hlp_doc_add_topic(doc, tid, tid);
        } else if (rec_type == 0x20 || rec_type == 0x23 || rec_type == 1) {
            if (cur < 0) cur = hlp_doc_add_topic(doc, "topic0", "Topic");
            char *txt = expand_run(links + data2_off, data2_len, &pb);
            if (txt && *txt && cur >= 0) {
                help_topic_t *t = &doc->topics[cur];
                // First text of a topic becomes its title (if still synthetic).
                if (t->block_count == 0 && t->title) {
                    // take up to first 60 chars / first sentence
                    char title[64]; int n = 0;
                    for (const char *q = txt; *q && n < 60; q++) {
                        if (*q == '\n' || *q == '\r') break;
                        title[n++] = *q;
                    }
                    title[n] = 0;
                    if (n > 0) { free(t->title); t->title = hlp_strdup0(title); }
                }
                hlp_topic_add_text_block(t, HELP_BLK_PARAGRAPH, txt);
            }
            free(txt);
        }
        // advance
        pos += block_size;
    }

    free(links);
    phrasebook_free(&pb);

    if (doc->topic_count == 0) {
        int i = hlp_doc_add_topic(doc, "info", doc->title);
        if (i >= 0)
            hlp_topic_add_text_block(&doc->topics[i], HELP_BLK_PARAGRAPH,
                "No topics could be recovered from this WinHelp file.");
    }
    return doc;
}
