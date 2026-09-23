#include "formats.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// Top-level format dispatcher. Owner: agent 3 (officemodel).
// office_detect classifies a file by extension AND container magic bytes:
//   zip      = "PK\x03\x04"  (OOXML docx/xlsx/pptx and ODF odt/ods/odp)
//   CFB/OLE2 = D0 CF 11 E0 A1 B1 1A E1  (legacy .doc/.xls/.ppt, read-only)
// The kind (1=doc 2=sheet 3=pres) comes from the extension family; the magic
// decides the container, so a legacy .doc misnamed .docx still routes through
// the OLE2 reader (and vice-versa). office_open_* read the file, detect, and
// call the matching loader from formats.h.
//
// NOTE for format agents: office_open_* frees the file buffer after your
// load() returns, so copy anything you retain into the model.

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

enum { MAGIC_NONE=0, MAGIC_ZIP=1, MAGIC_CFB=2 };

static const char *ext(const char *p){
    const char *d=0;
    for(;p&&*p;p++) if(*p=='.') d=p;
    return d?d:"";
}

static int ext_kind(const char *path){
    const char *e=ext(path);
    if(!strcasecmp(e,".docx")||!strcasecmp(e,".odt")||!strcasecmp(e,".doc")) return 1;
    if(!strcasecmp(e,".xlsx")||!strcasecmp(e,".ods")||!strcasecmp(e,".xls")) return 2;
    if(!strcasecmp(e,".pptx")||!strcasecmp(e,".odp")||!strcasecmp(e,".ppt")) return 3;
    return 0;
}

static int magic_of(const unsigned char *b, unsigned long n){
    if(n>=4 && b[0]=='P'&&b[1]=='K'&&b[2]==0x03&&b[3]==0x04) return MAGIC_ZIP;
    if(n>=8 && b[0]==0xD0&&b[1]==0xCF&&b[2]==0x11&&b[3]==0xE0&&
              b[4]==0xA1&&b[5]==0xB1&&b[6]==0x1A&&b[7]==0xE1) return MAGIC_CFB;
    return MAGIC_NONE;
}

int office_detect(const char *path, const unsigned char *b, unsigned long n){
    int k=ext_kind(path);
    if(b && n){
        int m=magic_of(b,n);
        if(m==MAGIC_NONE) return -1;   // buffer present but not an office container
        if(k) return k;                // extension gives the kind
        return -1;                     // recognized container, unknown kind
    }
    return k ? k : -1;                 // no buffer: extension-only (menu wiring)
}

static int read_file(const char *path, unsigned char **outb, unsigned long *outn){
    int fd=open(path,O_RDONLY);
    if(fd<0) return -1;
    long sz=lseek(fd,0,SEEK_END);
    if(sz<0){ close(fd); return -1; }
    lseek(fd,0,SEEK_SET);
    unsigned char *buf=(unsigned char*)malloc(sz>0?(size_t)sz:1);
    if(!buf){ close(fd); return -1; }
    long got=0;
    while(got<sz){
        long r=read(fd,buf+got,(size_t)(sz-got));
        if(r<0){ free(buf); close(fd); return -1; }
        if(r==0) break;
        got+=r;
    }
    close(fd);
    *outb=buf; *outn=(unsigned long)got;
    return 0;
}

int office_open_document(const char *path, document **out, int *readonly){
    if(out) *out=0;
    if(readonly) *readonly=0;
    unsigned char *buf=0; unsigned long n=0;
    if(read_file(path,&buf,&n)!=0) return -1;
    int rc=-1;
    if(office_detect(path,buf,n)==1){
        if(magic_of(buf,n)==MAGIC_CFB){
            if(readonly) *readonly=1;
            rc=doc_ole_load(buf,n,out);
        } else if(!strcasecmp(ext(path),".odt")){
            rc=odt_load(buf,n,out);
        } else {
            rc=docx_load(buf,n,out);
        }
    }
    free(buf);
    return rc;
}

int office_open_workbook(const char *path, workbook **out, int *readonly){
    if(out) *out=0;
    if(readonly) *readonly=0;
    unsigned char *buf=0; unsigned long n=0;
    if(read_file(path,&buf,&n)!=0) return -1;
    int rc=-1;
    if(office_detect(path,buf,n)==2){
        if(magic_of(buf,n)==MAGIC_CFB){
            if(readonly) *readonly=1;
            rc=xls_ole_load(buf,n,out);
        } else if(!strcasecmp(ext(path),".ods")){
            rc=ods_load(buf,n,out);
        } else {
            rc=xlsx_load(buf,n,out);
        }
    }
    free(buf);
    return rc;
}

int office_open_presentation(const char *path, presentation **out, int *readonly){
    if(out) *out=0;
    if(readonly) *readonly=0;
    unsigned char *buf=0; unsigned long n=0;
    if(read_file(path,&buf,&n)!=0) return -1;
    int rc=-1;
    if(office_detect(path,buf,n)==3){
        if(magic_of(buf,n)==MAGIC_CFB){
            if(readonly) *readonly=1;
            rc=ppt_ole_load(buf,n,out);
        } else if(!strcasecmp(ext(path),".odp")){
            rc=odp_load(buf,n,out);
        } else {
            rc=pptx_load(buf,n,out);
        }
    }
    free(buf);
    return rc;
}
