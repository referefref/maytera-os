// arctest - in-OS self-test for the archiver core (#321).
// Streams its verdict over TCP to a host listener (<BUILD_SERVER>:9999) so the
// result is captured headlessly regardless of how the VFS maps the root or
// whether ext2 writes are flushed. Also writes the verdict to disk as a
// fallback. Runs the gzip / tar / tar.gz / zip create+extract round-trips in
// memory and asserts byte-identical results + correct CRC32.
#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "arc.h"

#define HOST_IP   ((192u << 24) | (168u << 16) | (1u << 8) | 251u)
#define HOST_PORT 9999

static char g_log[8192];
static int  g_loglen;
static int  g_fail;
static int  g_sk = -1;

static void emit(const char *s, int n) {
    for (int i = 0; i < n && g_loglen < (int)sizeof(g_log) - 1; i++) g_log[g_loglen++] = s[i];
    if (g_sk >= 0) {
        int off = 0;
        while (off < n) { int w = tcp_send(g_sk, s + off, n - off); if (w <= 0) break; off += w; }
    }
}
static void logline(const char *s) {
    int n = 0; while (s[n]) n++;
    emit(s, n); emit("\n", 1);
}
static void chk(const char *what, int ok) {
    char line[160]; int p = 0;
    const char *tag = ok ? "ok:   " : "FAIL: ";
    for (int i = 0; tag[i]; i++) line[p++] = tag[i];
    for (int i = 0; what[i] && p < 150; i++) line[p++] = what[i];
    line[p] = 0;
    logline(line);
    if (!ok) g_fail++;
}
static int eq(const uint8_t *a, const uint8_t *b, size_t n) { return a && b && memcmp(a, b, n) == 0; }

static int write_file(const char *path, const char *buf, int len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    int off = 0; while (off < len) { long w = write(fd, buf + off, len - off); if (w <= 0) break; off += (int)w; }
    close(fd);
    return off == len ? 0 : -1;
}

static void connect_host(void) {
    sys_ping(HOST_IP, 1000);  // warm ARP so the first SYN is not dropped
    for (int a = 0; a < 25; a++) {
        int sk = tcp_socket();
        if (sk >= 0) {
            if (tcp_connect(sk, HOST_IP, HOST_PORT) == 0) { g_sk = sk; return; }
            tcp_close(sk);
        }
        sys_sleep(200);  // yield so the net stack can process RX
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    g_loglen = 0; g_fail = 0;

    connect_host();
    logline("========== ARCHIVER SELFTEST (#321) ==========");

    size_t N = 40000;
    uint8_t *payload = (uint8_t *)malloc(N);
    if (!payload) { logline("FAIL: alloc"); goto done; }
    for (size_t i = 0; i < N; i++)
        payload[i] = (i % 11 == 0) ? (uint8_t)((i * 2654435761u) >> 13) : (uint8_t)('A' + (i % 26));
    const char *text = "MayteraOS archiver self-test. Quick brown fox jumps over the lazy dog 0123456789.";
    size_t tlen = strlen(text);
    uint32_t want_crc = arc_crc32(0, payload, N);

    {   // DEFLATE / INFLATE
        size_t dl = 0; uint8_t *def = arc_deflate(payload, N, &dl);
        uint8_t *back = (uint8_t *)malloc(N); size_t got = 0;
        int ok = def && back && arc_inflate(def, dl, back, N, &got) == 0 && got == N && eq(back, payload, N);
        chk("deflate->inflate identical", ok);
        if (def) free(def);
        if (back) free(back);
    }
    {   // GZIP
        size_t gl = 0; uint8_t *gz = arc_gzip_compress(payload, N, "data.bin", &gl);
        size_t bl = 0; uint8_t *back = gz ? arc_gzip_decompress(gz, gl, &bl) : 0;
        chk("gzip round-trip + CRC/ISIZE verified", back && bl == N && eq(back, payload, N));
        if (gz) free(gz);
        if (back) free(back);
    }

    arc_entry ents[3];
    memset(ents, 0, sizeof(ents));
    strcpy(ents[0].name, "dir/");          ents[0].is_dir = 1; ents[0].mode = 0755;
    strcpy(ents[1].name, "dir/note.txt");  ents[1].data = (uint8_t *)text; ents[1].size = tlen; ents[1].mode = 0644;
    strcpy(ents[2].name, "payload.bin");   ents[2].data = payload; ents[2].size = N; ents[2].mode = 0644;

    {   // TAR
        size_t tl = 0; uint8_t *tar = arc_tar_create(ents, 3, &tl);
        int cnt = 0; arc_entry *ex = tar ? arc_tar_extract(tar, tl, &cnt) : 0;
        int ok = ex && cnt == 3 && ex[0].is_dir &&
                 ex[1].size == tlen && eq(ex[1].data, (const uint8_t *)text, tlen) &&
                 ex[2].size == N && eq(ex[2].data, payload, N);
        chk("tar create->extract (dir+file+binary)", ok);
        if (ex) arc_free_entries(ex, cnt);
        if (tar) free(tar);
    }
    {   // TAR.GZ
        size_t gl = 0; uint8_t *gz = arc_targz_create(ents, 3, &gl);
        int cnt = 0; arc_entry *ex = gz ? arc_targz_extract(gz, gl, &cnt) : 0;
        int ok = ex && cnt == 3 && ex[2].size == N && eq(ex[2].data, payload, N);
        chk("tar.gz create->extract", ok);
        if (ex) arc_free_entries(ex, cnt);
        if (gz) free(gz);
    }
    for (int usedef = 0; usedef <= 1; usedef++) {   // ZIP store + deflate
        size_t zl = 0; uint8_t *zip = arc_zip_create(ents, 3, usedef, &zl);
        int cnt = 0; arc_entry *ex = zip ? arc_zip_extract(zip, zl, &cnt) : 0;
        int ok = ex && cnt == 3 &&
                 ex[1].size == tlen && eq(ex[1].data, (const uint8_t *)text, tlen) &&
                 ex[2].size == N && eq(ex[2].data, payload, N) &&
                 arc_crc32(0, ex[2].data, ex[2].size) == want_crc;
        chk(usedef ? "zip(deflate) create->extract + CRC" : "zip(store) create->extract + CRC", ok);
        if (ex) arc_free_entries(ex, cnt);
        if (zip) free(zip);
    }
    {   // on-disk round-trip via a real file
        size_t zl = 0; uint8_t *zip = arc_zip_create(ents, 3, 1, &zl);
        int ok = 0;
        if (zip) {
            if (write_file("/CONFIG/ARCTEST.ZIP", (const char *)zip, (int)zl) == 0) {
                int rfd = open("/CONFIG/ARCTEST.ZIP", O_RDONLY);
                if (rfd >= 0) {
                    long sz = lseek(rfd, 0, SEEK_END); lseek(rfd, 0, SEEK_SET);
                    uint8_t *rb = (uint8_t *)malloc(sz > 0 ? sz : 1);
                    long o = 0; while (o < sz) { long r = read(rfd, rb + o, sz - o); if (r <= 0) break; o += r; }
                    close(rfd);
                    int cnt = 0; arc_entry *ex = arc_zip_extract(rb, (size_t)o, &cnt);
                    ok = ex && cnt == 3 && ex[2].size == N && eq(ex[2].data, payload, N);
                    if (ex) arc_free_entries(ex, cnt);
                    free(rb);
                }
            }
            free(zip);
        }
        chk("on-disk .zip write->read->extract", ok);
    }

    free(payload);
done:
    if (g_fail == 0) logline("========== ARCHIVER SELFTEST: PASS ==========");
    else             logline("========== ARCHIVER SELFTEST: FAIL ==========");

    if (g_sk >= 0) { char ack[8]; tcp_recv(g_sk, ack, sizeof(ack)); sys_sleep(200); tcp_close(g_sk); }
    write_file("/CONFIG/ARCTEST.LOG", g_log, g_loglen);
    write_file("/HOME/ARCTEST.LOG", g_log, g_loglen);
    return g_fail ? 1 : 0;
}
