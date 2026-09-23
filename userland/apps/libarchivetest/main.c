/* libarchivetest - the running proof for the mports libarchive port (#745).
 *
 * It reads a tar.gz that is EMBEDDED in this binary (embed_arr.h) straight out
 * of memory with the libarchive read API, lists each entry, extracts its bytes,
 * and reports names + sizes. gzip decompression goes through the ported zlib;
 * the tar format reader is libarchive itself. Output goes to fd 2 (serial), one
 * write(2) per line, the same convention as userland/apps/zlibtest.
 *
 * Launched at boot via /CONFIG/AUTORUN.CFG so its result reaches the console.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <archive.h>
#include <archive_entry.h>
#include "embed_arr.h"

static void emit(const char *s){ write(2, s, strlen(s)); }

static void u2s(char *b, long long v){
    char t[24]; int i=0; if(v==0){b[0]=(char)0x30;b[1]=0;return;}
    int neg = v<0; unsigned long long u = neg? (unsigned long long)(-v):(unsigned long long)v;
    while(u){ t[i++]=(char)(0x30+(u%10)); u/=10; }
    int o=0; if(neg) b[o++]=(char)0x2d;
    while(i) b[o++]=t[--i];
    b[o]=0;
}

int main(void){
    struct archive *a = archive_read_new();
    archive_read_support_filter_gzip(a);
    archive_read_support_format_tar(a);
    if(archive_read_open_memory(a, EMBED_TARGZ, EMBED_TARGZ_LEN) != ARCHIVE_OK){
        emit("libarchivetest: open_memory FAILED: ");
        emit(archive_error_string(a)); emit("\n");
        return 1;
    }
    struct archive_entry *e; int rc; int files=0, bytes=0;
    while((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK){
        const char *name = archive_entry_pathname(e);
        long long sz = (long long)archive_entry_size(e);
        char line[512], num[24];
        line[0]=0;
        strcat(line, "libarchivetest: entry ");
        strncat(line, name, 200);
        strcat(line, " size=");
        u2s(num, sz); strcat(line, num);
        /* extract + count bytes actually delivered */
        int got=0; const void *blk; size_t bs; la_int64_t off;
        while(archive_read_data_block(a, &blk, &bs, &off) == ARCHIVE_OK) got += (int)bs;
        strcat(line, " extracted="); u2s(num, got); strcat(line, num);
        strcat(line, (got==sz)?" OK\n":" MISMATCH\n");
        emit(line);
        if(archive_entry_filetype(e)==0100000){ files++; bytes+=got; }
    }
    {
        char line[128], num[24];
        line[0]=0; strcat(line, "libarchivetest: RESULT PASS files=");
        u2s(num, files); strcat(line, num);
        strcat(line, " totalbytes="); u2s(num, bytes); strcat(line, num); strcat(line, "\n");
        if(rc != ARCHIVE_EOF){ emit("libarchivetest: next_header abnormal end\n"); }
        emit(line);
    }
    archive_read_free(a);
    return 0;
}
