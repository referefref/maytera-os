// arc.c - MayteraOS archiver core. Pure C, no syscalls. See arc.h.
// Inflate algorithm is adapted from the kernel PNG decoder (gui/png.c).
#include "arc.h"
#include "arc_port.h"

// ========================================================================
// Checksums
// ========================================================================
static uint32_t crc_table[256];
static int      crc_table_done = 0;

static void crc_init(void) {
    for (int n = 0; n < 256; n++) {
        uint32_t c = (uint32_t)n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_table_done = 1;
}

uint32_t arc_crc32(uint32_t crc, const uint8_t *data, size_t len) {
    if (!crc_table_done) crc_init();
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t arc_adler32(uint32_t adler, const uint8_t *data, size_t len) {
    uint32_t a = adler & 0xFFFF, b = (adler >> 16) & 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// ========================================================================
// INFLATE (raw DEFLATE -> caller buffer of known capacity)
// ========================================================================
typedef struct {
    const uint8_t *data;
    size_t   size;
    size_t   pos;
    uint32_t bit_buf;
    int      bit_count;
} instate;

static int getbit(instate *s) {
    if (s->bit_count == 0) {
        if (s->pos >= s->size) return -1;
        s->bit_buf = s->data[s->pos++];
        s->bit_count = 8;
    }
    int bit = s->bit_buf & 1;
    s->bit_buf >>= 1;
    s->bit_count--;
    return bit;
}

static int getbits(instate *s, int n) {
    int value = 0;
    for (int i = 0; i < n; i++) {
        int bit = getbit(s);
        if (bit < 0) return -1;
        value |= (bit << i);
    }
    return value;
}

static const int code_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} huff_t;

static void build_huff(huff_t *h, const int *lengths, int n) {
    for (int i = 0; i < 16; i++) h->counts[i] = 0;
    for (int i = 0; i < n; i++)
        if (lengths[i] > 0 && lengths[i] < 16) h->counts[lengths[i]]++;
    uint16_t offsets[16];
    offsets[0] = 0;
    for (int i = 1; i < 16; i++) offsets[i] = offsets[i-1] + h->counts[i-1];
    for (int i = 0; i < n; i++)
        if (lengths[i] > 0 && lengths[i] < 16)
            h->symbols[offsets[lengths[i]]++] = (uint16_t)i;
}

static int decode_sym(instate *s, huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int bit = getbit(s);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        int count = h->counts[len];
        if (code - count < first) return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
    }
    return -1;
}

static const int length_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const int length_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const int dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const int dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

int arc_inflate(const uint8_t *src, size_t src_len,
                uint8_t *dst, size_t dst_cap, size_t *out_len) {
    instate s;
    s.data = src; s.size = src_len; s.pos = 0; s.bit_buf = 0; s.bit_count = 0;
    size_t out = 0;
    int bfinal;
    do {
        bfinal = getbit(&s);
        if (bfinal < 0) return -1;
        int btype = getbits(&s, 2);
        if (btype < 0) return -1;

        if (btype == 0) {
            s.bit_count = 0;
            if (s.pos + 4 > s.size) return -1;
            uint16_t len = s.data[s.pos] | (s.data[s.pos+1] << 8);
            s.pos += 4;
            if (s.pos + len > s.size) return -1;
            if (out + len > dst_cap) return -1;
            for (uint16_t i = 0; i < len; i++) dst[out++] = s.data[s.pos++];
        } else if (btype == 1 || btype == 2) {
            huff_t lit_h, dist_h;
            int lit_len[288], dist_len[32];
            if (btype == 1) {
                for (int i = 0;   i <= 143; i++) lit_len[i] = 8;
                for (int i = 144; i <= 255; i++) lit_len[i] = 9;
                for (int i = 256; i <= 279; i++) lit_len[i] = 7;
                for (int i = 280; i <= 287; i++) lit_len[i] = 8;
                for (int i = 0; i < 32; i++) dist_len[i] = 5;
            } else {
                int hlit  = getbits(&s, 5) + 257;
                int hdist = getbits(&s, 5) + 1;
                int hclen = getbits(&s, 4) + 4;
                if (hlit < 257 || hdist < 1 || hclen < 4) return -1;
                int cl[19];
                for (int i = 0; i < 19; i++) cl[i] = 0;
                for (int i = 0; i < hclen; i++) {
                    int v = getbits(&s, 3);
                    if (v < 0) return -1;
                    cl[code_order[i]] = v;
                }
                huff_t code_h;
                build_huff(&code_h, cl, 19);
                int comb[288 + 32];
                int n = 0;
                while (n < hlit + hdist) {
                    int sym = decode_sym(&s, &code_h);
                    if (sym < 0) return -1;
                    if (sym < 16) comb[n++] = sym;
                    else if (sym == 16) {
                        int c = getbits(&s, 2) + 3;
                        if (n == 0) return -1;
                        int v = comb[n-1];
                        while (c-- > 0 && n < hlit + hdist) comb[n++] = v;
                    } else if (sym == 17) {
                        int c = getbits(&s, 3) + 3;
                        while (c-- > 0 && n < hlit + hdist) comb[n++] = 0;
                    } else if (sym == 18) {
                        int c = getbits(&s, 7) + 11;
                        while (c-- > 0 && n < hlit + hdist) comb[n++] = 0;
                    }
                }
                for (int i = 0; i < 288; i++) lit_len[i]  = (i < hlit)  ? comb[i] : 0;
                for (int i = 0; i < 32;  i++) dist_len[i] = (i < hdist) ? comb[hlit + i] : 0;
            }
            build_huff(&lit_h, lit_len, 288);
            build_huff(&dist_h, dist_len, 32);
            for (;;) {
                int sym = decode_sym(&s, &lit_h);
                if (sym < 0) return -1;
                if (sym < 256) {
                    if (out >= dst_cap) return -1;
                    dst[out++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return -1;
                    int len = length_base[sym];
                    if (length_extra[sym]) {
                        int e = getbits(&s, length_extra[sym]);
                        if (e < 0) return -1;
                        len += e;
                    }
                    int dsym = decode_sym(&s, &dist_h);
                    if (dsym < 0 || dsym >= 30) return -1;
                    int dist = dist_base[dsym];
                    if (dist_extra[dsym]) {
                        int e = getbits(&s, dist_extra[dsym]);
                        if (e < 0) return -1;
                        dist += e;
                    }
                    if ((size_t)dist > out) return -1;
                    if (out + len > dst_cap) return -1;
                    for (int i = 0; i < len; i++) { dst[out] = dst[out - dist]; out++; }
                }
            }
        } else {
            return -1;
        }
    } while (!bfinal);
    *out_len = out;
    return 0;
}

// ========================================================================
// DEFLATE (fixed-Huffman, LZ77 hash-chain matcher)
// ========================================================================
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    uint32_t bitbuf;
    int      bitcnt;
    int      err;
} bitout;

static void bo_putbits(bitout *b, uint32_t val, int n) {
    if (b->err) return;
    b->bitbuf |= (val & ((1u << n) - 1)) << b->bitcnt;
    b->bitcnt += n;
    while (b->bitcnt >= 8) {
        if (b->len + 1 > b->cap) {
            size_t nc = b->cap ? b->cap * 2 : 256;
            uint8_t *nb = (uint8_t *)ARC_REALLOC(b->buf, nc);
            if (!nb) { b->err = 1; return; }
            b->buf = nb; b->cap = nc;
        }
        b->buf[b->len++] = b->bitbuf & 0xFF;
        b->bitbuf >>= 8;
        b->bitcnt -= 8;
    }
}

static uint32_t rev_bits(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

static void bo_puthuff(bitout *b, uint32_t code, int n) {
    bo_putbits(b, rev_bits(code, n), n);
}

static void emit_litlen(bitout *b, int sym) {
    uint32_t code; int nbits;
    if (sym <= 143)      { code = 0x30 + sym;        nbits = 8; }
    else if (sym <= 255) { code = 0x190 + (sym-144); nbits = 9; }
    else if (sym <= 279) { code = sym - 256;         nbits = 7; }
    else                 { code = 0xC0 + (sym-280);  nbits = 8; }
    bo_puthuff(b, code, nbits);
}

#define ARC_HASH_BITS 15
#define ARC_HASH_SIZE (1 << ARC_HASH_BITS)
#define ARC_WSIZE     32768
#define ARC_MIN_MATCH 3
#define ARC_MAX_MATCH 258
#define ARC_MAX_CHAIN 256

static uint32_t arc_hash3(const uint8_t *p) {
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2])
           & (ARC_HASH_SIZE - 1);
}

uint8_t *arc_deflate(const uint8_t *src, size_t src_len, size_t *out_len) {
    bitout b;
    b.buf = NULL; b.cap = 0; b.len = 0; b.bitbuf = 0; b.bitcnt = 0; b.err = 0;

    int *head = (int *)ARC_MALLOC(sizeof(int) * ARC_HASH_SIZE);
    int *prev = (int *)ARC_MALLOC(sizeof(int) * ARC_WSIZE);
    if (!head || !prev) { if (head) ARC_FREE(head); if (prev) ARC_FREE(prev); return NULL; }
    for (int i = 0; i < ARC_HASH_SIZE; i++) head[i] = 0;
    for (int i = 0; i < ARC_WSIZE; i++) prev[i] = 0;

    // Single fixed-Huffman final block.
    bo_putbits(&b, 1, 1); // BFINAL = 1
    bo_putbits(&b, 1, 2); // BTYPE  = 01 (fixed)

    size_t pos = 0;
    while (pos < src_len) {
        int best_len = 0, best_dist = 0;
        if (pos + ARC_MIN_MATCH <= src_len) {
            uint32_t h = arc_hash3(src + pos);
            int chain = head[h];
            int attempts = ARC_MAX_CHAIN;
            size_t maxlen = src_len - pos;
            if (maxlen > ARC_MAX_MATCH) maxlen = ARC_MAX_MATCH;
            while (chain != 0 && attempts-- > 0) {
                size_t cand = (size_t)(chain - 1);
                if (pos - cand > ARC_WSIZE) break;
                size_t ml = 0;
                while (ml < maxlen && src[cand + ml] == src[pos + ml]) ml++;
                if ((int)ml > best_len) {
                    best_len = (int)ml;
                    best_dist = (int)(pos - cand);
                    if (ml >= maxlen) break;
                }
                chain = prev[cand & (ARC_WSIZE - 1)];
            }
            prev[pos & (ARC_WSIZE - 1)] = head[h];
            head[h] = (int)(pos + 1);
        }

        if (best_len >= ARC_MIN_MATCH) {
            // length symbol
            int l = 0;
            while (l < 28 && length_base[l + 1] <= best_len) l++;
            emit_litlen(&b, 257 + l);
            if (length_extra[l]) bo_putbits(&b, best_len - length_base[l], length_extra[l]);
            // distance symbol (fixed: 5-bit value == symbol, MSB first)
            int d = 0;
            while (d < 29 && dist_base[d + 1] <= best_dist) d++;
            bo_puthuff(&b, d, 5);
            if (dist_extra[d]) bo_putbits(&b, best_dist - dist_base[d], dist_extra[d]);
            // insert the interior positions of the match into the hash chains
            for (int k = 1; k < best_len; k++) {
                size_t p = pos + k;
                if (p + ARC_MIN_MATCH <= src_len) {
                    uint32_t h = arc_hash3(src + p);
                    prev[p & (ARC_WSIZE - 1)] = head[h];
                    head[h] = (int)(p + 1);
                }
            }
            pos += best_len;
        } else {
            emit_litlen(&b, src[pos]);
            pos += 1;
        }
    }
    emit_litlen(&b, 256); // end of block
    // flush partial byte
    if (b.bitcnt > 0) bo_putbits(&b, 0, 8 - b.bitcnt);

    ARC_FREE(head);
    ARC_FREE(prev);
    if (b.err) { if (b.buf) ARC_FREE(b.buf); return NULL; }
    *out_len = b.len;
    return b.buf;
}

// ========================================================================
// gzip
// ========================================================================
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static void put_le16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

uint8_t *arc_gzip_compress(const uint8_t *data, size_t len,
                           const char *name, size_t *out_len) {
    size_t dlen = 0;
    uint8_t *def = arc_deflate(data, len, &dlen);
    if (!def) return NULL;

    size_t namelen = 0;
    if (name && name[0]) { while (name[namelen]) namelen++; }
    int flg = namelen ? 0x08 : 0x00;

    size_t hdr = 10 + (namelen ? namelen + 1 : 0);
    size_t total = hdr + dlen + 8;
    uint8_t *out = (uint8_t *)ARC_MALLOC(total);
    if (!out) { ARC_FREE(def); return NULL; }

    out[0] = 0x1F; out[1] = 0x8B; out[2] = 0x08; out[3] = (uint8_t)flg;
    put_le32(out + 4, 0);   // MTIME = 0
    out[8] = 0x00;          // XFL
    out[9] = 0x03;          // OS = unix
    size_t o = 10;
    if (namelen) {
        for (size_t i = 0; i < namelen; i++) out[o++] = (uint8_t)name[i];
        out[o++] = 0;
    }
    memcpy(out + o, def, dlen); o += dlen;
    put_le32(out + o, arc_crc32(0, data, len)); o += 4;
    put_le32(out + o, (uint32_t)(len & 0xFFFFFFFFu)); o += 4;

    ARC_FREE(def);
    *out_len = total;
    return out;
}

uint8_t *arc_gzip_decompress(const uint8_t *gz, size_t gz_len, size_t *out_len) {
    if (gz_len < 18) return NULL;
    if (gz[0] != 0x1F || gz[1] != 0x8B || gz[2] != 0x08) return NULL;
    uint8_t flg = gz[3];
    size_t p = 10;
    if (flg & 0x04) { // FEXTRA
        if (p + 2 > gz_len) return NULL;
        uint16_t xlen = get_le16(gz + p);
        p += 2 + xlen;
    }
    if (flg & 0x08) { while (p < gz_len && gz[p]) p++; p++; } // FNAME
    if (flg & 0x10) { while (p < gz_len && gz[p]) p++; p++; } // FCOMMENT
    if (flg & 0x02) p += 2;                                   // FHCRC
    if (p + 8 > gz_len) return NULL;

    uint32_t crc_want = get_le32(gz + gz_len - 8);
    uint32_t isize    = get_le32(gz + gz_len - 4);

    size_t cap = isize ? isize : 1; // allocate at least 1 byte
    uint8_t *out = (uint8_t *)ARC_MALLOC(cap);
    if (!out) return NULL;
    size_t got = 0;
    if (arc_inflate(gz + p, gz_len - p - 8, out, cap, &got) != 0) { ARC_FREE(out); return NULL; }
    if (got != isize) { ARC_FREE(out); return NULL; }
    if (arc_crc32(0, out, got) != crc_want) { ARC_FREE(out); return NULL; }
    *out_len = got;
    return out;
}

// ========================================================================
// tar (ustar)
// ========================================================================
static size_t arc_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static void octal(char *dst, int width, uint32_t val) {
    // width includes the trailing NUL slot; write (width-1) octal digits + NUL.
    int digits = width - 1;
    for (int i = digits - 1; i >= 0; i--) { dst[i] = (char)('0' + (val & 7)); val >>= 3; }
    dst[digits] = '\0';
}

static uint32_t parse_octal(const char *s, int n) {
    uint32_t v = 0;
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\0')) i++;
    for (; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = (v << 3) + (uint32_t)(s[i] - '0');
    return v;
}

uint8_t *arc_tar_create(const arc_entry *ents, int n, size_t *out_len) {
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        total += 512; // header
        if (!ents[i].is_dir) total += (ents[i].size + 511) & ~((size_t)511);
    }
    total += 1024; // two zero blocks
    uint8_t *out = (uint8_t *)ARC_MALLOC(total);
    if (!out) return NULL;
    memset(out, 0, total);

    size_t o = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *h = out + o;
        const char *name = ents[i].name;
        size_t nl = arc_strlen(name);
        // ustar name/prefix split (best effort for long names)
        if (nl <= 100) {
            memcpy(h, name, nl);
        } else {
            size_t split = nl - 100;
            while (split < nl && name[split] != '/') split++;
            if (split >= nl) split = nl - 100; // no slash; just truncate sensibly
            size_t prefix_len = split;
            size_t name_len = nl - split - 1;
            if (prefix_len > 155) prefix_len = 155;
            if (name_len > 100) name_len = 100;
            memcpy(h, name + split + 1, name_len);
            memcpy(h + 345, name, prefix_len);
        }
        uint32_t mode = ents[i].mode ? ents[i].mode : (ents[i].is_dir ? 0755 : 0644);
        octal((char *)h + 100, 8, mode & 07777);   // mode
        octal((char *)h + 108, 8, 0);              // uid
        octal((char *)h + 116, 8, 0);              // gid
        octal((char *)h + 124, 12, ents[i].is_dir ? 0 : (uint32_t)ents[i].size); // size
        octal((char *)h + 136, 12, 0);             // mtime
        memset(h + 148, ' ', 8);                   // chksum field = spaces
        h[156] = ents[i].is_dir ? '5' : '0';       // typeflag
        memcpy(h + 257, "ustar", 5);               // magic
        h[263] = '0'; h[264] = '0';                // version "00"
        // checksum
        uint32_t sum = 0;
        for (int k = 0; k < 512; k++) sum += h[k];
        // store as 6 octal digits, NUL, space
        char cs[8];
        octal(cs, 7, sum & 0777777); // 6 digits + NUL
        memcpy(h + 148, cs, 7);
        h[154] = '\0';
        h[155] = ' ';
        o += 512;
        if (!ents[i].is_dir && ents[i].size) {
            memcpy(out + o, ents[i].data, ents[i].size);
            o += (ents[i].size + 511) & ~((size_t)511);
        }
    }
    *out_len = total;
    return out;
}

arc_entry *arc_tar_extract(const uint8_t *tar, size_t len, int *out_count) {
    int cap = 8, count = 0;
    arc_entry *ents = (arc_entry *)ARC_MALLOC(sizeof(arc_entry) * cap);
    if (!ents) return NULL;
    size_t o = 0;
    while (o + 512 <= len) {
        const uint8_t *h = tar + o;
        // end-of-archive: a zero block
        int allzero = 1;
        for (int k = 0; k < 512; k++) if (h[k]) { allzero = 0; break; }
        if (allzero) break;
        o += 512;

        char typeflag = (char)h[156];
        uint32_t size = parse_octal((const char *)h + 124, 12);
        uint32_t mode = parse_octal((const char *)h + 100, 8);

        if (count >= cap) {
            cap *= 2;
            arc_entry *ne = (arc_entry *)ARC_REALLOC(ents, sizeof(arc_entry) * cap);
            if (!ne) { arc_free_entries(ents, count); return NULL; }
            ents = ne;
        }
        arc_entry *e = &ents[count];
        memset(e, 0, sizeof(*e));
        // assemble name from prefix + name
        char nm[256]; int ni = 0;
        if (h[345]) {
            for (int k = 0; k < 155 && h[345 + k] && ni < 255; k++) nm[ni++] = (char)h[345 + k];
            if (ni < 255) nm[ni++] = '/';
        }
        for (int k = 0; k < 100 && h[k] && ni < 255; k++) nm[ni++] = (char)h[k];
        nm[ni] = '\0';
        memcpy(e->name, nm, ni + 1);
        e->mode = mode;

        if (typeflag == '5') {
            e->is_dir = 1;
            e->data = NULL;
            e->size = 0;
        } else {
            // treat everything else (incl '0' and '\0') as a regular file
            e->is_dir = 0;
            e->size = size;
            if (size) {
                if (o + size > len) { arc_free_entries(ents, count); return NULL; }
                e->data = (uint8_t *)ARC_MALLOC(size);
                if (!e->data) { arc_free_entries(ents, count); return NULL; }
                memcpy(e->data, tar + o, size);
            } else {
                e->data = (uint8_t *)ARC_MALLOC(1);
            }
            o += ((size_t)size + 511) & ~((size_t)511);
        }
        count++;
    }
    *out_count = count;
    return ents;
}

uint8_t *arc_targz_create(const arc_entry *ents, int n, size_t *out_len) {
    size_t tlen = 0;
    uint8_t *tar = arc_tar_create(ents, n, &tlen);
    if (!tar) return NULL;
    uint8_t *gz = arc_gzip_compress(tar, tlen, NULL, out_len);
    ARC_FREE(tar);
    return gz;
}

arc_entry *arc_targz_extract(const uint8_t *gz, size_t len, int *out_count) {
    size_t tlen = 0;
    uint8_t *tar = arc_gzip_decompress(gz, len, &tlen);
    if (!tar) return NULL;
    arc_entry *e = arc_tar_extract(tar, tlen, out_count);
    ARC_FREE(tar);
    return e;
}

// ========================================================================
// zip
// ========================================================================
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    int      err;
} membuf;

static void mb_need(membuf *m, size_t extra) {
    if (m->err) return;
    if (m->len + extra > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 256;
        while (nc < m->len + extra) nc *= 2;
        uint8_t *nb = (uint8_t *)ARC_REALLOC(m->buf, nc);
        if (!nb) { m->err = 1; return; }
        m->buf = nb; m->cap = nc;
    }
}
static void mb_bytes(membuf *m, const void *p, size_t n) {
    mb_need(m, n);
    if (m->err) return;
    memcpy(m->buf + m->len, p, n);
    m->len += n;
}
static void mb_u16(membuf *m, uint16_t v) { uint8_t t[2]; put_le16(t, v); mb_bytes(m, t, 2); }
static void mb_u32(membuf *m, uint32_t v) { uint8_t t[4]; put_le32(t, v); mb_bytes(m, t, 4); }

uint8_t *arc_zip_create(const arc_entry *ents, int n, int use_deflate, size_t *out_len) {
    membuf m; m.buf = NULL; m.cap = 0; m.len = 0; m.err = 0;

    // local header offsets + per-entry compressed payloads
    uint32_t *offsets = (uint32_t *)ARC_MALLOC(sizeof(uint32_t) * (n > 0 ? n : 1));
    uint8_t **payload = (uint8_t **)ARC_MALLOC(sizeof(uint8_t *) * (n > 0 ? n : 1));
    uint32_t *csize    = (uint32_t *)ARC_MALLOC(sizeof(uint32_t) * (n > 0 ? n : 1));
    uint32_t *usize    = (uint32_t *)ARC_MALLOC(sizeof(uint32_t) * (n > 0 ? n : 1));
    uint32_t *crc      = (uint32_t *)ARC_MALLOC(sizeof(uint32_t) * (n > 0 ? n : 1));
    uint16_t *method   = (uint16_t *)ARC_MALLOC(sizeof(uint16_t) * (n > 0 ? n : 1));
    int *owns          = (int *)ARC_MALLOC(sizeof(int) * (n > 0 ? n : 1));
    if (!offsets || !payload || !csize || !usize || !crc || !method || !owns) goto fail_arrays;

    for (int i = 0; i < n; i++) {
        const arc_entry *e = &ents[i];
        usize[i] = e->is_dir ? 0 : (uint32_t)e->size;
        crc[i]   = e->is_dir ? 0 : arc_crc32(0, e->data, e->size);
        payload[i] = NULL; owns[i] = 0; method[i] = 0; csize[i] = usize[i];
        if (!e->is_dir && use_deflate && e->size > 0) {
            size_t dl = 0;
            uint8_t *def = arc_deflate(e->data, e->size, &dl);
            if (def && dl < e->size) {
                payload[i] = def; owns[i] = 1; method[i] = 8; csize[i] = (uint32_t)dl;
            } else {
                if (def) ARC_FREE(def);
                payload[i] = (uint8_t *)e->data; method[i] = 0; csize[i] = usize[i];
            }
        } else if (!e->is_dir) {
            payload[i] = (uint8_t *)e->data; method[i] = 0;
        }
    }

    // local file headers + data
    for (int i = 0; i < n; i++) {
        const arc_entry *e = &ents[i];
        offsets[i] = (uint32_t)m.len;
        size_t nl = arc_strlen(e->name);
        mb_u32(&m, 0x04034b50);
        mb_u16(&m, 20);          // version needed
        mb_u16(&m, 0);           // flags
        mb_u16(&m, method[i]);   // method
        mb_u16(&m, 0);           // mod time
        mb_u16(&m, 0x0021);      // mod date (1980-01-01)
        mb_u32(&m, crc[i]);
        mb_u32(&m, csize[i]);
        mb_u32(&m, usize[i]);
        mb_u16(&m, (uint16_t)nl);
        mb_u16(&m, 0);           // extra len
        mb_bytes(&m, e->name, nl);
        if (csize[i]) mb_bytes(&m, payload[i], csize[i]);
    }

    // central directory
    uint32_t cd_off = (uint32_t)m.len;
    for (int i = 0; i < n; i++) {
        const arc_entry *e = &ents[i];
        size_t nl = arc_strlen(e->name);
        uint32_t ext_attr = (e->mode ? e->mode : (e->is_dir ? 0755 : 0644)) << 16;
        if (e->is_dir) ext_attr |= 0x10; // FILE_ATTRIBUTE_DIRECTORY
        mb_u32(&m, 0x02014b50);
        mb_u16(&m, 0x031E);      // version made by (unix, 3.0)
        mb_u16(&m, 20);          // version needed
        mb_u16(&m, 0);           // flags
        mb_u16(&m, method[i]);
        mb_u16(&m, 0);           // mod time
        mb_u16(&m, 0x0021);      // mod date
        mb_u32(&m, crc[i]);
        mb_u32(&m, csize[i]);
        mb_u32(&m, usize[i]);
        mb_u16(&m, (uint16_t)nl);
        mb_u16(&m, 0);           // extra len
        mb_u16(&m, 0);           // comment len
        mb_u16(&m, 0);           // disk number start
        mb_u16(&m, 0);           // internal attrs
        mb_u32(&m, ext_attr);    // external attrs
        mb_u32(&m, offsets[i]);  // local header offset
        mb_bytes(&m, e->name, nl);
    }
    uint32_t cd_size = (uint32_t)m.len - cd_off;

    // EOCD
    mb_u32(&m, 0x06054b50);
    mb_u16(&m, 0);
    mb_u16(&m, 0);
    mb_u16(&m, (uint16_t)n);
    mb_u16(&m, (uint16_t)n);
    mb_u32(&m, cd_size);
    mb_u32(&m, cd_off);
    mb_u16(&m, 0);

    for (int i = 0; i < n; i++) if (owns[i] && payload[i]) ARC_FREE(payload[i]);
    ARC_FREE(offsets); ARC_FREE(payload); ARC_FREE(csize);
    ARC_FREE(usize); ARC_FREE(crc); ARC_FREE(method); ARC_FREE(owns);

    if (m.err) { if (m.buf) ARC_FREE(m.buf); return NULL; }
    *out_len = m.len;
    return m.buf;

fail_arrays:
    if (offsets) ARC_FREE(offsets);
    if (payload) ARC_FREE(payload);
    if (csize) ARC_FREE(csize);
    if (usize) ARC_FREE(usize);
    if (crc) ARC_FREE(crc);
    if (method) ARC_FREE(method);
    if (owns) ARC_FREE(owns);
    return NULL;
}

arc_entry *arc_zip_extract(const uint8_t *zip, size_t len, int *out_count) {
    if (len < 22) return NULL;
    // find EOCD by scanning backwards for the signature
    size_t eocd = 0; int found = 0;
    size_t scan_start = (len > 22 + 65535) ? (len - 22 - 65535) : 0;
    for (size_t i = len - 22; i + 1 > scan_start; i--) {
        if (get_le32(zip + i) == 0x06054b50) { eocd = i; found = 1; break; }
        if (i == 0) break;
    }
    if (!found) return NULL;

    uint16_t total = get_le16(zip + eocd + 10);
    uint32_t cd_off = get_le32(zip + eocd + 16);
    if (cd_off > len) return NULL;

    arc_entry *ents = (arc_entry *)ARC_MALLOC(sizeof(arc_entry) * (total > 0 ? total : 1));
    if (!ents) return NULL;
    int count = 0;
    size_t p = cd_off;
    for (int i = 0; i < total; i++) {
        if (p + 46 > len) break;
        if (get_le32(zip + p) != 0x02014b50) break;
        uint16_t method = get_le16(zip + p + 10);
        uint32_t crc    = get_le32(zip + p + 16);
        uint32_t csize  = get_le32(zip + p + 20);
        uint32_t usize  = get_le32(zip + p + 24);
        uint16_t nl     = get_le16(zip + p + 28);
        uint16_t el     = get_le16(zip + p + 30);
        uint16_t cl     = get_le16(zip + p + 32);
        uint32_t lho    = get_le32(zip + p + 42);
        const char *name = (const char *)(zip + p + 46);

        arc_entry *e = &ents[count];
        memset(e, 0, sizeof(*e));
        int ni = 0;
        for (int k = 0; k < nl && k < 255; k++) e->name[ni++] = name[k];
        e->name[ni] = '\0';
        int isdir = (ni > 0 && e->name[ni - 1] == '/');
        e->is_dir = isdir;
        e->mode = 0;

        if (!isdir) {
            // locate data via the local header
            if (lho + 30 > len) { arc_free_entries(ents, count); return NULL; }
            if (get_le32(zip + lho) != 0x04034b50) { arc_free_entries(ents, count); return NULL; }
            uint16_t lnl = get_le16(zip + lho + 26);
            uint16_t lel = get_le16(zip + lho + 28);
            size_t data = lho + 30 + lnl + lel;
            if (data + csize > len) { arc_free_entries(ents, count); return NULL; }
            e->size = usize;
            e->data = (uint8_t *)ARC_MALLOC(usize ? usize : 1);
            if (!e->data) { arc_free_entries(ents, count); return NULL; }
            if (method == 0) {
                if (csize != usize) { arc_free_entries(ents, count + 1); return NULL; }
                memcpy(e->data, zip + data, usize);
            } else if (method == 8) {
                size_t got = 0;
                if (arc_inflate(zip + data, csize, e->data, usize, &got) != 0 || got != usize) {
                    arc_free_entries(ents, count + 1); return NULL;
                }
            } else {
                arc_free_entries(ents, count + 1); return NULL;
            }
            if (usize && arc_crc32(0, e->data, usize) != crc) {
                arc_free_entries(ents, count + 1); return NULL;
            }
        }
        count++;
        p += 46 + nl + el + cl;
    }
    *out_count = count;
    return ents;
}

void arc_free_entries(arc_entry *ents, int count) {
    if (!ents) return;
    for (int i = 0; i < count; i++) if (ents[i].data) ARC_FREE(ents[i].data);
    ARC_FREE(ents);
}
