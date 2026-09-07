/* competitor.c — конкурс DEFLATE-энкодеров для максимальной компрессии.
 *
 * Single densest mode tries, per file, and keeps the smallest:
 *   Store(0) | Deflate(8): zlib exhaustive + libdeflate 1..12 + Zopfli max.
 * ONLY methods 0/8 are ever emitted (100% compatible: unzip + Python
 * zipfile + 7-Zip + Explorer + macOS). BZIP2(12)/LZMA(14)/ZSTD(93)/
 * PPMd(98)/XZ(95) are intentionally NOT used.
 * Beating kzip (deflate-only, Ken Silverman) is achieved by best-deflate:
 *   Zopfli (beats kzip ~1% per Zopfli paper) + exhaustive zlib/libdeflate
 *   search. Same method (8), smaller size — fair fight.
 */
#include "competitor.h"
#include "policy.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef HAVE_LIBDEFLATE
#include <libdeflate.h>
#endif
/* vendored bzip2 + zopfli are always present */
#include "bzlib.h"
#include "zopfli/zopfli.h"

// ---------- helpers ----------
static bool deflate_raw_zlib(const unsigned char *in, size_t in_len,
                             unsigned char **out, size_t *out_len,
                             int level, int strategy) {
    if (!in || !out || !out_len) return false;
    if (level <0 || level>9) return false;
    size_t bound = in_len + in_len/1000 + 128;
    if (bound < 128) bound = 128;
    unsigned char *buf = (unsigned char*)malloc(bound);
    if (!buf) return false;
    z_stream strm;
    memset(&strm,0,sizeof(strm));
    strm.next_in = (unsigned char*)in;
    strm.avail_in = (unsigned int)in_len;
    strm.next_out = buf;
    strm.avail_out = (unsigned int)bound;
    int ret = deflateInit2(&strm, level, Z_DEFLATED, -15, 9, strategy);
    if (ret != Z_OK) { free(buf); return false; }
    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) { deflateEnd(&strm); free(buf); return false; }
    *out_len = strm.total_out;
    *out = buf;
    deflateEnd(&strm);
    unsigned char *tmp = (unsigned char*)malloc(in_len ? in_len : 1);
    if (!tmp) { free(buf); return false; }
    z_stream d;
    memset(&d,0,sizeof(d));
    d.next_in = buf; d.avail_in = (unsigned int)*out_len;
    d.next_out = tmp; d.avail_out = (unsigned int)in_len;
    if (inflateInit2(&d, -15)!=Z_OK) { free(buf); free(tmp); return false; }
    int r = inflate(&d, Z_FINISH);
    bool ok = (r==Z_STREAM_END && d.total_out==in_len && memcmp(tmp,in,in_len)==0);
    inflateEnd(&d); free(tmp);
    if (!ok) { free(buf); return false; }
    return true;
}

#ifdef HAVE_LIBDEFLATE
static bool deflate_libdeflate(const unsigned char *in, size_t in_len,
                               unsigned char **out, size_t *out_len, int level) {
    if (!in || !out || !out_len) return false;
    struct libdeflate_compressor *c = libdeflate_alloc_compressor(level);
    if (!c) return false;
    size_t bound = libdeflate_deflate_compress_bound(c, in_len);
    unsigned char *buf = (unsigned char*)malloc(bound);
    if (!buf) { libdeflate_free_compressor(c); return false; }
    size_t sz = libdeflate_deflate_compress(c, in, in_len, buf, bound);
    libdeflate_free_compressor(c);
    if (sz==0) { free(buf); return false; }
    *out = buf; *out_len = sz;
    unsigned char *tmp = (unsigned char*)malloc(in_len ? in_len:1);
    if (!tmp) { free(buf); return false; }
    z_stream d; memset(&d,0,sizeof(d));
    d.next_in=buf; d.avail_in=(unsigned int)sz;
    d.next_out=tmp; d.avail_out=(unsigned int)in_len;
    bool ok=false;
    if (inflateInit2(&d,-15)==Z_OK) {
        if (inflate(&d,Z_FINISH)==Z_STREAM_END && d.total_out==in_len && memcmp(tmp,in,in_len)==0) ok=true;
        inflateEnd(&d);
    }
    free(tmp);
    if (!ok) { free(buf); return false; }
    return true;
}
#endif

/* compress_bzip2 removed — DEFLATE-only output (decode still supported below). */
/* Zopfli raw DEFLATE (ZOPFLI_FORMAT_DEFLATE — no zlib wrapper to strip). */
static bool deflate_zopfli(const unsigned char *in, size_t in_len,
                           unsigned char **out, size_t *out_len,
                           int iter, int splitmax) {
    if (!in || !out || !out_len) return false;
    if (in_len==0) {
        unsigned char *buf=(unsigned char*)malloc(2);
        if (!buf) return false;
        buf[0]=0x03; buf[1]=0x00;
        *out=buf; *out_len=2; return true;
    }
    ZopfliOptions opts;
    ZopfliInitOptions(&opts);
    opts.numiterations = iter > 1000 ? 1000 : iter;
    opts.blocksplitting = 1;
    opts.blocksplittingmax = splitmax;
    unsigned char *zbuf=NULL;
    size_t zlen=0;
    ZopfliCompress(&opts, ZOPFLI_FORMAT_DEFLATE, in, in_len, &zbuf, &zlen);
    if (!zbuf || zlen==0) { if(zbuf) free(zbuf); return false; }
    unsigned char *tmp=(unsigned char*)malloc(in_len);
    if (!tmp) { free(zbuf); return false; }
    z_stream d; memset(&d,0,sizeof(d));
    d.next_in=zbuf; d.avail_in=(unsigned int)zlen;
    d.next_out=tmp; d.avail_out=(unsigned int)in_len;
    bool ok=false;
    if (inflateInit2(&d,-15)==Z_OK) {
        if (inflate(&d,Z_FINISH)==Z_STREAM_END && d.total_out==in_len && memcmp(tmp,in,in_len)==0) ok=true;
        inflateEnd(&d);
    }
    free(tmp);
    if (!ok) { free(zbuf); return false; }
    *out=zbuf; *out_len=zlen; return true;
}

bool has_zopfli(void){ return true; }
bool has_zstd(void){
#ifdef HAVE_ZSTD
    return true;
#else
    return false;
#endif
}
bool has_bzip2(void){ return true; }
bool has_lzma(void){
#ifdef HAVE_LZMA
    return true;
#else
    return false;
#endif
}

bool compress_buffer(const unsigned char *data, size_t len,
                     const char *filename, int level, int compat,
                     unsigned char **out, size_t *out_len, int *method) {
    (void)compat; /* single densest compatible mode: 0/8/12 always */
    if (!data || !out || !out_len || !method) return false;
    if (level<0) level=0;
    if (level>4) level=4;

    if (len==0) {
        *out=(unsigned char*)malloc(1);
        if (!*out) return false;
        *out_len=0; *method=METHOD_STORE; return true;
    }
    if (should_use_store_only(filename, data, len)) {
        unsigned char *buf=(unsigned char*)malloc(len);
        if (!buf) return false;
        memcpy(buf,data,len);
        *out=buf; *out_len=len; *method=METHOD_STORE; return true;
    }

    unsigned char *best=NULL; size_t best_len=len; int best_method=METHOD_STORE;

    #define CONSIDER(cand, cand_len, meth) do{ if(cand && cand_len < best_len){ if(best) free(best); best=cand; best_len=cand_len; best_method=meth; } else { if(cand) free(cand); } }while(0)

    /* 1) zlib exhaustive: levels x strategies.
     * Big files: only level 9 (time bound). Small: 1..9. */
    {
        int strategies[]={Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, 3/*RLE*/, Z_FIXED};
        if (len > 1024*1024) {
            for (int i=0;i<5;i++) {
                unsigned char *c=NULL; size_t cl=0;
                if (deflate_raw_zlib(data,len,&c,&cl,9,strategies[i])) CONSIDER(c,cl,METHOD_DEFLATE);
            }
        } else {
            for (int lvl=1; lvl<=9; lvl++) {
                for (int i=0;i<5;i++) {
                    /* skip slow combos on tiny gain: RLE/FIXED only at 9 */
                    if ((strategies[i]==3 || strategies[i]==Z_FIXED) && lvl!=9) continue;
                    unsigned char *c=NULL; size_t cl=0;
                    if (deflate_raw_zlib(data,len,&c,&cl,lvl,strategies[i])) CONSIDER(c,cl,METHOD_DEFLATE);
                }
            }
        }
    }
#ifdef HAVE_LIBDEFLATE
    /* 2) libdeflate all levels 1..12 (fast, whole-buffer) */
    for (int lvl=1; lvl<=12; lvl++) {
        unsigned char *d=NULL; size_t dl=0;
        if (deflate_libdeflate(data,len,&d,&dl,lvl)) CONSIDER(d,dl,METHOD_DEFLATE);
    }
#endif
    /* BZIP2 intentionally disabled — DEFLATE-only output. */
    /* 4) Zopfli max (beats kzip ~1% per Zopfli paper; ECT-level).
     * Two passes: blocksplittingmax=15 and unlimited(0), keep best. */
    if (zopfli_allowed(data,len,level)) {
        /* time-bounded: small files afford deep search, big files don't.
         * Zopfli@15 already beats kzip; higher iters give diminishing returns. */
        int iter = 15;
        if (len <= 64*1024) iter = 300;
        else if (len <= 256*1024) iter = 60;
        else if (len <= 1024*1024) iter = 30;
        else iter = 15;
        unsigned char *z1=NULL; size_t zl1=0;
        if (deflate_zopfli(data,len,&z1,&zl1,iter,15)) CONSIDER(z1,zl1,METHOD_DEFLATE);
        /* second pass with unlimited blocks only for small files (cheap there) */
        if (len <= 64*1024) {
            unsigned char *z2=NULL; size_t zl2=0;
            if (deflate_zopfli(data,len,&z2,&zl2,iter,0)) CONSIDER(z2,zl2,METHOD_DEFLATE);
        }
    }
    /* NOTE: LZMA(14)/ZSTD(93)/PPMd(98) deliberately excluded — unzip 6.0
     * and Python zipfile cannot extract them. See header comment. */

    if (best==NULL || best_len >= len) {
        unsigned char *buf=(unsigned char*)malloc(len);
        if(!buf) { if(best) free(best); return false; }
        memcpy(buf,data,len);
        if(best) free(best);
        *out=buf; *out_len=len; *method=METHOD_STORE; return true;
    }
    *out=best; *out_len=best_len; *method=best_method; return true;
}

bool decompress_buffer(const unsigned char *comp, size_t comp_len, int method, unsigned char *out, size_t out_len) {
    if (!comp || !out) return false;
    if (method==METHOD_STORE) { memcpy(out,comp,comp_len); return comp_len==out_len; }
    if (method==METHOD_DEFLATE) {
        z_stream d; memset(&d,0,sizeof(d));
        d.next_in=(unsigned char*)comp; d.avail_in=(unsigned int)comp_len;
        d.next_out=out; d.avail_out=(unsigned int)out_len;
        if (inflateInit2(&d,-15)!=Z_OK) return false;
        int r=inflate(&d,Z_FINISH);
        inflateEnd(&d);
        return r==Z_STREAM_END && d.total_out==out_len;
    }
    if (method==METHOD_BZIP2) {
        unsigned int dlen=(unsigned int)out_len;
        return BZ2_bzBuffToBuffDecompress((char*)out,&dlen,(char*)comp,(unsigned int)comp_len,0,0)==BZ_OK && dlen==out_len;
    }
    return false;
}
