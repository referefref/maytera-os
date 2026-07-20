// make_fixture.c - generate a tiny, structurally-valid WinHelp .HLP file to
// exercise the libhelp .HLP reader on the host (task #267).
//
// This is NOT a faithful HC31 compiler. It builds the minimum the MVP reader
// needs: the file header, one directory B+tree leaf page mapping "|SYSTEM" and
// "|TOPIC" to file offsets, an uncompressed |SYSTEM (HC31, with a TITLE record),
// and an uncompressed |TOPIC stream with topic-header + text TOPICLINK records.
//
// It is honest about scope: it produces uncompressed records so the reader's
// directory walk, |SYSTEM title parse, and TOPICLINK text extraction are all
// covered end to end. (LZ77 / phrase paths are unit-reachable separately.)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned char buf[16384];
static size_t pos = 0;

// little-endian 32-bit write at an arbitrary offset (used by topic builder)
static void wat32_local(unsigned char *b, size_t at, uint32_t v) {
    b[at]=v&0xFF; b[at+1]=(v>>8)&0xFF; b[at+2]=(v>>16)&0xFF; b[at+3]=(v>>24)&0xFF;
}

static void w8(uint8_t v)  { buf[pos++] = v; }
static void w16(uint16_t v){ w8(v & 0xFF); w8((v >> 8) & 0xFF); }
static void w32(uint32_t v){ w16(v & 0xFFFF); w16((v >> 16) & 0xFFFF); }
static void wat32(size_t at, uint32_t v) {
    buf[at] = v & 0xFF; buf[at+1] = (v>>8)&0xFF;
    buf[at+2] = (v>>16)&0xFF; buf[at+3] = (v>>24)&0xFF;
}

// Build a |SYSTEM internal file body (HC31 with a TITLE record).
static size_t build_system(unsigned char *out) {
    size_t p = 0;
    // SYSTEMHEADER: Magic, Minor(>=16 => HC31), Major, GenDate(4), Flags(2)
    out[p++]=0x6C; out[p++]=0x03;   // Magic 0x036C
    out[p++]=21;   out[p++]=0;      // Minor = 21 (HC31)
    out[p++]=1;    out[p++]=0;      // Major
    out[p++]=0;out[p++]=0;out[p++]=0;out[p++]=0; // GenDate
    out[p++]=0;    out[p++]=0;      // Flags = 0 (no compression)
    // SYSTEMREC: TITLE (RecordType 1)
    const char *title = "Fixture Help";
    uint16_t ds = (uint16_t)(strlen(title) + 1);
    out[p++]=1; out[p++]=0;         // RecordType 1 = TITLE
    out[p++]=ds & 0xFF; out[p++]=(ds>>8)&0xFF;
    memcpy(out+p, title, strlen(title)); p += strlen(title);
    out[p++]=0;                     // NUL
    return p;
}

// Build a |TOPIC stream (single 0x1000 block, uncompressed).
// One topic-header record (type 2) then one text record (type 0x20).
static size_t build_topic(unsigned char *out) {
    size_t p = 0;
    // TOPICBLOCKHEADER (12 bytes): LastTopicLink, FirstTopicLink, LastTopicHeader
    for (int i = 0; i < 12; i++) out[p++] = 0;

    // ---- TOPICLINK record 1: topic header (type 2) ----
    {
        size_t rec = p;
        const char *txt = "";            // header text empty
        uint32_t data1 = 21;             // no extra linkdata1
        uint32_t block_size = 21 + (uint32_t)strlen(txt);
        wat32_local(out, rec+0, block_size); // BlockSize
        wat32_local(out, rec+4, (uint32_t)strlen(txt)); // DataLen2
        wat32_local(out, rec+8, 0);      // PrevBlock
        wat32_local(out, rec+12, 0);     // NextBlock
        wat32_local(out, rec+16, data1); // DataLen1
        out[rec+20] = 2;                 // RecordType = topic header
        p = rec + block_size;
    }
    // ---- TOPICLINK record 2: text (type 0x20) ----
    {
        size_t rec = p;
        const char *txt = "Welcome to the fixture topic. This text proves the "
                          "TOPICLINK extractor works.";
        uint32_t data1 = 21;
        uint32_t tlen = (uint32_t)strlen(txt);
        uint32_t block_size = 21 + tlen;
        wat32_local(out, rec+0, block_size);
        wat32_local(out, rec+4, tlen);
        wat32_local(out, rec+8, 0);
        wat32_local(out, rec+12, 0);
        wat32_local(out, rec+16, data1);
        out[rec+20] = 0x20;
        memcpy(out + rec + 21, txt, tlen);
        p = rec + block_size;
    }
    // ---- TOPICLINK record 3: second topic ----
    {
        size_t rec = p;
        wat32_local(out, rec+0, 21);
        wat32_local(out, rec+4, 0);
        wat32_local(out, rec+8, 0);
        wat32_local(out, rec+12, 0);
        wat32_local(out, rec+16, 21);
        out[rec+20] = 2;
        p = rec + 21;
    }
    {
        size_t rec = p;
        const char *txt = "Second topic body about settings and the desktop.";
        uint32_t tlen = (uint32_t)strlen(txt);
        uint32_t block_size = 21 + tlen;
        wat32_local(out, rec+0, block_size);
        wat32_local(out, rec+4, tlen);
        wat32_local(out, rec+8, 0);
        wat32_local(out, rec+12, 0);
        wat32_local(out, rec+16, 21);
        out[rec+20] = 0x20;
        memcpy(out + rec + 21, txt, tlen);
        p = rec + block_size;
    }
    return p; // less than 0x1000, fine
}

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "fixture.hlp";

    // --- file header (16 bytes) ---
    w32(0x00035F3F);   // Magic "?_\3\0"
    size_t dir_off_at = pos; w32(0);  // DirectoryStart (patch later)
    w32(0xFFFFFFFF);   // FreeChainStart
    size_t size_at = pos; w32(0);     // EntireFileSize (patch later)

    // --- build internal files in scratch then place them ---
    unsigned char sysbody[256]; size_t syslen = build_system(sysbody);
    unsigned char topbody[8192]; size_t toplen = build_topic(topbody);

    // Place |SYSTEM FILEHEADER + body.
    size_t sys_fh = pos;
    w32((uint32_t)(9 + syslen)); // ReservedSpace
    w32((uint32_t)syslen);       // UsedSpace
    w8(0);                       // FileFlags
    memcpy(buf+pos, sysbody, syslen); pos += syslen;

    // Place |TOPIC FILEHEADER + body.
    size_t top_fh = pos;
    w32((uint32_t)(9 + toplen));
    w32((uint32_t)toplen);
    w8(0);
    memcpy(buf+pos, topbody, toplen); pos += toplen;

    // --- directory internal file ---
    size_t dir_off = pos;
    wat32(dir_off_at, (uint32_t)dir_off);

    // Directory FILEHEADER (9 bytes): ReservedSpace, UsedSpace, FileFlags
    size_t dir_used_at = pos;
    w32(0);            // ReservedSpace (patch)
    w32(0);            // UsedSpace (patch)
    w8(0);             // FileFlags

    size_t bt_base = pos;
    // BTREEHEADER (38 bytes)
    w16(0x293B);       // Magic
    w16(0x0002);       // Flags
    uint16_t page_size = 0x0400;
    w16(page_size);    // PageSize
    char structure[16]; memset(structure, 0, 16);
    structure[0]='z'; structure[1]='4'; // "Lz4" style; content unused by reader
    memcpy(buf+pos, structure, 16); pos += 16;
    w16(0);            // MustBeZero
    w16(0);            // PageSplits
    w16(0);            // RootPage = 0
    w16(0xFFFF);       // MustBeNegOne
    w16(1);            // TotalPages = 1
    w16(1);            // NLevels = 1 (leaf only)
    w32(2);            // TotalBtreeEntries

    // --- one leaf page ---
    size_t page = pos;  // pages_base == bt_base + 38 == page
    // leaf header (8 bytes): NEntries, PreviousPage(i16), NextPage(i16) ... we
    // write NEntries then pad to 8.
    w16(2);            // NEntries
    w16(0xFFFF);       // unused
    w16(0xFFFF);       // unused
    w16(0);            // pad to 8
    // entry: name NUL-terminated + u32 FileOffset
    const char *n1 = "|SYSTEM";
    memcpy(buf+pos, n1, strlen(n1)+1); pos += strlen(n1)+1;
    w32((uint32_t)sys_fh);
    const char *n2 = "|TOPIC";
    memcpy(buf+pos, n2, strlen(n2)+1); pos += strlen(n2)+1;
    w32((uint32_t)top_fh);

    // pad the page out to page_size so block math is clean
    while (pos < page + page_size) w8(0);

    // patch directory used size
    size_t dir_total = pos - (dir_used_at + 9);
    wat32(dir_used_at + 0, (uint32_t)(9 + dir_total));
    wat32(dir_used_at + 4, (uint32_t)dir_total);
    (void)bt_base; (void)page;

    // patch EntireFileSize
    wat32(size_at, (uint32_t)pos);

    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); return 1; }
    fwrite(buf, 1, pos, f);
    fclose(f);
    printf("wrote %s (%zu bytes)\n", out, pos);
    return 0;
}
