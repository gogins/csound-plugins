/*
   pathtools.c

  Copyright (C) 2020 Eduardo Moguillansky

  This file is part of Csound.

  The Csound Library is free software; you can redistribute it
  and/or modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  Csound is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with Csound; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
  02110-1301 USA

  */

/*


*/

#include "csdl.h"
#include <math.h>
#include <unistd.h>
#include <ctype.h>

#define min(x, y) (((x) < (y)) ? (x) : (y))
#define max(x, y) (((x) > (y)) ? (x) : (y))

#define MSGF(fmt, ...) (csound->Message(csound, fmt, __VA_ARGS__))
#define MSG(s) (csound->Message(csound, s))
#define INITERR(m) (csound->InitError(csound, "%s", m))
#define INITERRF(fmt, ...) (csound->InitError(csound, fmt, __VA_ARGS__))
#define PERFERR(m) (csound->PerfError(csound, &(p->h), "%s", m))
#define PERFERRF(fmt, ...) (csound->PerfError(csound, &(p->h), fmt, __VA_ARGS__))


// #define DEBUG

#ifdef DEBUG
    #define DBG(fmt, ...) printf(">>>> "fmt"\n", __VA_ARGS__); fflush(stdout);
    #define DBG_(m) DBG("%s", m)
#else
    #define DBG(fmt, ...)
    #define DBG_(m)
#endif


#define SAMPLE_ACCURATE(out) \
    uint32_t n, nsmps = CS_KSMPS;                                    \
    uint32_t offset = p->h.insdshead->ksmps_offset;                  \
    uint32_t early = p->h.insdshead->ksmps_no_end;                   \
    if (UNLIKELY(offset)) memset(out, '\0', offset*sizeof(MYFLT));   \
    if (UNLIKELY(early)) {                                           \
        nsmps -= early;                                              \
        memset(&out[nsmps], '\0', early*sizeof(MYFLT));              \
    }                                                                \


#define register_deinit(csound, p, func) \
    csound->RegisterDeinitCallback(csound, p, (int32_t(*)(CSOUND*, void*))(func))


static inline char _get_path_separator() {
#ifdef WIN32
    return '\\';
#else
    return '/';
#endif
}

static inline char _get_path_delim() {
#ifdef WIN32
    return ';';
#else
    return ':';
#endif
}


unsigned long next_power_of_two(unsigned long v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}


// like strncpy but really makes sure that the dest str is 0 terminated
// dest should have allocated at least n+1
static inline
void strncpy0(char *dest, const char *src, size_t n) {
    strncpy(dest, src, n);
    dest[n] = '\0';
}



// make sure that the out string has enough allocated space to hold
// a string of `size` chars (the \0 should be taken into account)
// This can be run only at init time
static int32_t _string_ensure(CSOUND *csound, STRINGDAT *s, size_t size) {
    if (s->size >= (int)size)
        return OK;
    size = next_power_of_two(size);
    s->data = csound->ReAlloc(csound, s->data, size);
    s->size = (int)size;
    return OK;
}


static int32_t _str_rfind(const char *s, char ch) {
    // find first occurrence of ch in s from right to left
    // returns -1 if not found
    int len = (int)strlen(s);
    if(len == 0)
        return -1;
    for(int i=len-1; i>=0; i--) {
        if(s[i] == ch)
            return i;
    }
    return -1;
}


static void stringdat_copy_cstr(CSOUND *csound, STRINGDAT *dest, char *src, size_t len) {
    _string_ensure(csound, dest, len+1);
    strncpy0(dest->data, src, len);
}

static void stringdat_clear(CSOUND *csound, STRINGDAT *s) {
    _string_ensure(csound, s, 1);
    s->data[0] = '\0';
}


// Sdir, Sfolder pathSplit Sfile
typedef struct {
    OPDS h;
    STRINGDAT *S1, *S2;
    STRINGDAT *Spath;
} PATHSPLIT;


static int32_t pathSplit_opcode(CSOUND *csound, PATHSPLIT *p) {
    size_t pathlen = strlen(p->Spath->data);
    if(pathlen == 0) {
        return PERFERR("pathSplit: source path is empty");
    }

    char sep = _get_path_separator();
    int i = _str_rfind(p->Spath->data, sep) + 1;
    if(i == 0) {
        // no path separator, so no dir, all base
        stringdat_clear(csound, p->S1);
        stringdat_copy_cstr(csound, p->S2, p->Spath->data, pathlen);
        return OK;
    }
    if(i == 1) {
        // the path is of the type /filename.ext
        // so Sdir is /
        stringdat_copy_cstr(csound, p->S1, "/", 1);
        stringdat_copy_cstr(csound, p->S2, p->Spath->data+1, pathlen-1);
        return OK;
    }

    stringdat_copy_cstr(csound, p->S1, p->Spath->data, i-1);
    stringdat_copy_cstr(csound, p->S2, p->Spath->data+i, pathlen-i);
    return OK;
}

static int32_t pathSplitExt_opcode(CSOUND *csound, PATHSPLIT *p) {
    size_t pathlen = strlen(p->Spath->data);
    if(pathlen == 0) {
        return PERFERR("pathSplit: source path is empty");
    }

    int i = _str_rfind(p->Spath->data, '.');
    if(i == -1) {
        // no . so no extension
        stringdat_copy_cstr(csound, p->S1, p->Spath->data, pathlen);
        stringdat_clear(csound, p->S2);
        return OK;
    }
    if(i == 0) {
        // Only an extension is not an extension but a hidden file
        stringdat_clear(csound, p->S1);
        stringdat_copy_cstr(csound, p->S2, p->Spath->data, pathlen);
        return OK;
    }
    stringdat_copy_cstr(csound, p->S1, p->Spath->data, i);
    stringdat_copy_cstr(csound, p->S2, p->Spath->data+i, pathlen-i);
    return OK;
}

typedef struct {
    OPDS h;
    MYFLT *out;
    STRINGDAT *s;
} K_S;

static int32_t pathIsAbsolute(CSOUND *csound, K_S *p) {
    if(strlen(p->s->data) == 0) {
        MSG("Path is empty\n");
        return NOTOK;
    }
    char sep = _get_path_separator();
    *p->out = p->s->data[0] == sep ? 1 : 0;
    return OK;
}


typedef struct {
    OPDS h;
    STRINGDAT *Sout;
} S_;


typedef struct {
    OPDS h;
    STRINGDAT *Sout;
    STRINGDAT *s;
} S_S;

typedef struct {
    OPDS h;
    STRINGDAT *Sout;
    STRINGDAT *S1, *S2;
} S_SS;


static int32_t pathJoin(CSOUND *csound, S_SS *p) {
    size_t len1 = strlen(p->S1->data);
    size_t len2 = strlen(p->S2->data);
    char sep = _get_path_separator();
    if(len1 == 0) {
        stringdat_copy_cstr(csound, p->Sout, p->S2->data, len2);
        return OK;
    }
    _string_ensure(csound, p->Sout, len1+1+len2+1);
    strncpy0(p->Sout->data, p->S1->data, len1);
    if(p->Sout->data[len1-1] != sep) {
        p->Sout->data[len1] = sep;
        strncpy0(p->Sout->data + len1 + 1, p->S2->data, len2);
    } else {
        // it already has a separator, just join them
        strncpy0(p->Sout->data + len1, p->S2->data, len2);
    }
    return OK;
}


static int32_t pathAbsolute(CSOUND *csound, S_S *p) {
    if(strlen(p->s->data) == 0) {
        MSG("pathAbsolute: Path is empty\n");
        return NOTOK;
    }
    char sep = _get_path_separator();
    int isabsolute = p->s->data[0] == sep ? 1 : 0;
    size_t slen = strlen(p->s->data);
    if(slen > 1000) {
        MSG("pathAbsolute: Path two long!\n");
        return NOTOK;
    }
    if (isabsolute) {
        stringdat_copy_cstr(csound, p->Sout, p->s->data, slen);
    } else {
        _string_ensure(csound, p->Sout, 1024);
        if (getcwd(p->Sout->data, p->Sout->size - slen - 2) == NULL) {
            stringdat_clear(csound, p->Sout);
            MSG("Could not get the current working directory\n");
            return NOTOK;
        }
        // now cat the abs path
        size_t lenout = strlen(p->Sout->data);
        _string_ensure(csound, p->Sout, lenout + 2 + slen);
        char *outdata = p->Sout->data;
        if(outdata[lenout-1] != sep) {
            outdata[lenout] = sep;
            strncpy0(outdata + lenout+1, p->s->data, slen);
        } else {
            strncpy0(outdata + lenout, p->s->data, slen);
        }
    }
    return OK;
}

static int32_t findFile(CSOUND *csound, S_S *p) {
    char *outpath = csound->FindInputFile(csound, p->s->data, "SSDIR");
    if(outpath == NULL) {
        stringdat_clear(csound, p->Sout);
    } else {
        stringdat_copy_cstr(csound, p->Sout, outpath, strlen(outpath));
        csound->Free(csound, outpath);
    }
    return OK;
}

static int32_t getEnvVar(CSOUND *csound, S_S *p) {
    const char *val = csound->GetEnv(csound, p->s->data);
    if(val != NULL) {
        stringdat_copy_cstr(csound, p->Sout, val, strlen(val));
        return OK;
    }
    val = getenv(p->s->data);
    if(val != NULL) {
        stringdat_copy_cstr(csound, p->Sout, val, strlen(val));
        return OK;
    }
    stringdat_clear(csound, p->Sout);
    return OK;
}


static int32_t pathOfScript(CSOUND *csound, S_ *p) {
    // return the path of the current script
    const char *val = csound->GetEnv(csound, "SSDIR");
    if(val == NULL) {
        stringdat_clear(csound, p->Sout);
        return OK;
    }
    char delim = _get_path_delim();
    int delimidx = _str_rfind(val, delim);
    if(delimidx < 0) {
        stringdat_copy_cstr(csound, p->Sout, val, strlen(val));
        return OK;
    }
    stringdat_copy_cstr(csound, p->Sout, val+delimidx+1, strlen(val)-delimidx+1);
    return OK;
}


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


#define S(x) sizeof(x)

static OENTRY localops[] = {
    { "pathSplit", S(PATHSPLIT), 0, 1, "SS", "S", (SUBR)pathSplit_opcode },
    { "pathSplitk", S(PATHSPLIT), 0, 2, "SS", "S", NULL, (SUBR)pathSplit_opcode },
    { "pathSplitExt", S(PATHSPLIT), 0, 1, "SS", "S", (SUBR)pathSplitExt_opcode },
    { "pathSplitExtk", S(PATHSPLIT), 0, 2, "SS", "S", NULL, (SUBR)pathSplitExt_opcode },
    { "pathIsAbsolute.i", S(K_S), 0, 1, "i", "S", (SUBR)pathIsAbsolute},
    { "pathIsAbsolute.k", S(K_S), 0, 2, "k", "S", NULL, (SUBR)pathIsAbsolute},
    { "pathAbsolute", S(S_S), 0, 1, "S", "S", (SUBR)pathAbsolute},
    { "pathJoin", S(S_SS), 0, 1, "S", "SS", (SUBR)pathJoin},
    { "findFileInPath", S(S_S), 0, 1, "S", "S", (SUBR)findFile},
    { "getEnvVar", S(S_S), 0, 1, "S", "S", (SUBR)getEnvVar },
    { "scriptDir", S(S_), 0, 1, "S", "", (SUBR)pathOfScript }

};

LINKAGE