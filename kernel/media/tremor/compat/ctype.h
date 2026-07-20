#ifndef _MAYTERA_TREMOR_CTYPE_H
#define _MAYTERA_TREMOR_CTYPE_H
static __inline__ int toupper(int c){ return (c>='a'&&c<='z')?c-32:c; }
static __inline__ int tolower(int c){ return (c>='A'&&c<='Z')?c+32:c; }
static __inline__ int isspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static __inline__ int isdigit(int c){ return c>='0'&&c<='9'; }
#endif
