#include "archiver.h"
#include "competitor.h"
#include "policy.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <zlib.h>

#define SIG_LOCAL  0x04034b50
#define SIG_CENTRAL 0x02014b50
#define SIG_EOCD 0x06054b50
#define SIG_EOCD64 0x06064b50
#define SIG_EOCD64_LOC 0x07064b50
#define ZIP64_LIMIT 0xFFFFFFFFu
#define ZIP64_VERSION 45
#define FLAG_UTF8 0x800

static void write_le16(FILE *f, uint16_t v){ fputc(v&0xff,f); fputc((v>>8)&0xff,f); }
static void write_le32(FILE *f, uint32_t v){ for(int i=0;i<4;i++){ fputc(v&0xff,f); v>>=8; } }
static void write_le64(FILE *f, uint64_t v){ for(int i=0;i<8;i++){ fputc(v&0xff,f); v>>=8; } }

static bool is_utf8(const char *s){
    for(;*s;s++) if((unsigned char)*s>=0x80) return true;
    return false;
}
static void dos_time(time_t t, uint16_t *dosdate, uint16_t *dostime){
    struct tm *tm = localtime(&t);
    if(!tm){ *dosdate=0; *dostime=0; return; }
    int year=tm->tm_year+1900;
    if(year<1980) year=1980;
    if(year>2107) year=2107;
    *dosdate = (uint16_t)(((year-1980)<<9)|(tm->tm_mon+1)<<5|tm->tm_mday);
    *dostime = (uint16_t)(tm->tm_hour<<11|tm->tm_min<<5|tm->tm_sec/2);
}

static char* normalize_arcname(const char *arcname){
    if(!arcname) return NULL;
    // replace backslash, strip leading /
    size_t len=strlen(arcname);
    char *out=(char*)malloc(len+1);
    if(!out) return NULL;
    size_t j=0;
    for(size_t i=0;i<len;i++){
        char c=arcname[i];
        if(c=='\\') c='/';
        out[j++]=c;
    }
    out[j]='\0';
    // lstrip '/'
    size_t start=0;
    while(out[start]=='/') start++;
    if(start) memmove(out,out+start,strlen(out+start)+1);
    if(out[0]=='\0'){ free(out); return NULL; }
    return out;
}

bool zip_writer_init(zip_writer_t *w, const char *path, int compat){
    if(!w||!path) return false;
    memset(w,0,sizeof(*w));
    if(compat!=ZIP_COMPAT_WIDE && compat!=ZIP_COMPAT_MAX) compat=ZIP_COMPAT_WIDE;
    w->compat=compat;
    w->f=fopen(path,"wb");
    if(!w->f) return false;
    w->cap=16;
    w->entries=(zip_entry_t*)calloc(w->cap,sizeof(zip_entry_t));
    w->offsets=(long*)calloc(w->cap,sizeof(long));
    w->is_zip64=(bool*)calloc(w->cap,sizeof(bool));
    if(!w->entries||!w->offsets||!w->is_zip64){ fclose(w->f); return false; }
    w->count=0; w->closed=false;
    return true;
}

static bool ensure_cap(zip_writer_t *w){
    if(w->count < w->cap) return true;
    size_t ncap=w->cap*2;
    zip_entry_t *ne=(zip_entry_t*)realloc(w->entries,ncap*sizeof(zip_entry_t));
    long *no=(long*)realloc(w->offsets,ncap*sizeof(long));
    bool *nz=(bool*)realloc(w->is_zip64,ncap*sizeof(bool));
    if(!ne||!no||!nz) return false;
    w->entries=ne; w->offsets=no; w->is_zip64=nz;
    // zero new
    memset(w->entries+w->cap,0,(ncap-w->cap)*sizeof(zip_entry_t));
    memset(w->offsets+w->cap,0,(ncap-w->cap)*sizeof(long));
    memset(w->is_zip64+w->cap,0,(ncap-w->cap)*sizeof(bool));
    w->cap=ncap; return true;
}

bool zip_writer_add_file(zip_writer_t *w, const char *arcname, const unsigned char *data, size_t len, int level){
    if(!w||w->closed||!arcname||!data) return false;
    if(level<0||level>4) level=2;
    char *norm=normalize_arcname(arcname);
    if(!norm) return false;
    bool is_dir = norm[strlen(norm)-1]=='/';
    unsigned char *comp=NULL; size_t comp_len=0; int method=METHOD_STORE;
    unsigned int crc=0;
    if(!is_dir){
        if(len) crc=crc32(0L,data,(uInt)len); else crc=0;
        if(!compress_buffer(data,len,norm,level,w->compat,&comp,&comp_len,&method)){
            free(norm); return false;
        }
        if(w->compat==ZIP_COMPAT_WIDE && method!=METHOD_STORE && method!=METHOD_DEFLATE){
            // fallback to wide
            free(comp);
            if(!compress_buffer(data,len,norm,level,ZIP_COMPAT_WIDE,&comp,&comp_len,&method)){ free(norm); return false; }
        }
    } else {
        // directory
        comp=NULL; comp_len=0; method=METHOD_STORE; crc=0;
        if(len!=0){ free(norm); return false; }
    }

    long offset=ftell(w->f);
    if(offset<0){ free(norm); if(comp) free(comp); return false; }
    bool need_zip64 = len > ZIP64_LIMIT || comp_len > ZIP64_LIMIT || (uint64_t)offset > ZIP64_LIMIT;

    // write local header
    uint16_t flag = is_utf8(norm) ? FLAG_UTF8 : 0;
    uint16_t dosdate, dostime;
    dos_time(time(NULL),&dosdate,&dostime);
    uint16_t version=20;
    if(method==METHOD_BZIP2) version=46;
    else if(method==METHOD_LZMA||method==METHOD_ZSTD) version=63;
    if(need_zip64) version=45;
    uint16_t fnlen=(uint16_t)strlen(norm);
    uint16_t extralen = need_zip64 ? 20 : 0; // 20 = 2+2+8+8 for local
    // local extra: header 0x0001 + 16 bytes (uncomp, comp)
    write_le32(w->f,SIG_LOCAL);
    write_le16(w->f,version);
    write_le16(w->f,flag);
    write_le16(w->f,(uint16_t)method);
    write_le16(w->f,dostime);
    write_le16(w->f,dosdate);
    write_le32(w->f,crc);
    if(need_zip64){ write_le32(w->f,ZIP64_LIMIT); write_le32(w->f,ZIP64_LIMIT);} else { write_le32(w->f,(uint32_t)comp_len); write_le32(w->f,(uint32_t)len); }
    write_le16(w->f,fnlen);
    write_le16(w->f,extralen);
    if(fwrite(norm,1,fnlen,w->f)!=fnlen){ free(norm); if(comp) free(comp); return false; }
    if(need_zip64){
        write_le16(w->f,0x0001); write_le16(w->f,16);
        write_le64(w->f,len); write_le64(w->f,comp_len);
    }
    if(comp_len && fwrite(comp,1,comp_len,w->f)!=comp_len){ free(norm); free(comp); return false; }

    if(!ensure_cap(w)){ free(norm); if(comp) free(comp); return false; }
    w->entries[w->count].filename = norm; // take ownership
    w->entries[w->count].data = NULL; // not needed
    w->entries[w->count].data_len = len;
    w->entries[w->count].comp = comp; // keep for central (needs comp_len/crc)
    w->entries[w->count].comp_len = comp_len;
    w->entries[w->count].method = method;
    w->entries[w->count].crc = crc;
    w->offsets[w->count]=offset;
    w->is_zip64[w->count]=need_zip64;
    w->count++;
    return true;
}

bool zip_writer_add_path(zip_writer_t *w, const char *arcname, const char *fullpath, int level){
    if(!w||!arcname||!fullpath) return false;
    struct stat st;
    if(stat(fullpath,&st)!=0) return false;
    if(S_ISDIR(st.st_mode)) return false; // caller should not call for dir directly
    FILE *rf=fopen(fullpath,"rb");
    if(!rf) return false;
    size_t len=(size_t)st.st_size;
    unsigned char *data=(unsigned char*)malloc(len?len:1);
    if(!data){ fclose(rf); return false; }
    if(len && fread(data,1,len,rf)!=len){ free(data); fclose(rf); return false; }
    fclose(rf);
    bool ok=zip_writer_add_file(w,arcname,data,len,level);
    free(data);
    return ok;
}

bool zip_writer_close(zip_writer_t *w){
    if(!w||w->closed) return false;
    long central_offset=ftell(w->f);
    if(central_offset<0) return false;
    size_t central_size=0;
    for(size_t i=0;i<w->count;i++){
        zip_entry_t *e=&w->entries[i];
        long off=w->offsets[i];
        bool z64=w->is_zip64[i];
        uint16_t flag=is_utf8(e->filename)?FLAG_UTF8:0;
        uint16_t dosdate,dostime; dos_time(time(NULL),&dosdate,&dostime);
        uint16_t version=20;
        if(e->method==METHOD_BZIP2) version=46;
        else if(e->method==METHOD_LZMA||e->method==METHOD_ZSTD) version=63;
        if(z64) version=45;
        uint16_t fnlen=(uint16_t)strlen(e->filename);
        uint16_t extralen = z64 ? 28 : 0; // central extra 28 = 2+2+8+8+8
        // central header
        write_le32(w->f,SIG_CENTRAL);
        write_le16(w->f, (3<<8)|63); // made by
        write_le16(w->f,version);
        write_le16(w->f,flag);
        write_le16(w->f,(uint16_t)e->method);
        write_le16(w->f,dostime);
        write_le16(w->f,dosdate);
        write_le32(w->f,e->crc);
        if(z64){ write_le32(w->f,ZIP64_LIMIT); write_le32(w->f,ZIP64_LIMIT);} else { write_le32(w->f,(uint32_t)e->comp_len); write_le32(w->f,(uint32_t)e->data_len); }
        write_le16(w->f,fnlen);
        write_le16(w->f,extralen);
        write_le16(w->f,0); // comment
        write_le16(w->f,0); // disk
        write_le16(w->f,0); // internal
        write_le32(w->f,0); // external (unix 644)
        if(z64) write_le32(w->f,ZIP64_LIMIT); else write_le32(w->f,(uint32_t)off);
        central_size += 46;
        if(fwrite(e->filename,1,fnlen,w->f)!=fnlen) return false;
        central_size+=fnlen;
        if(z64){
            write_le16(w->f,0x0001); write_le16(w->f,24);
            write_le64(w->f,e->data_len); write_le64(w->f,e->comp_len); write_le64(w->f,off);
            central_size+=28;
        }
    }
    long central_end=ftell(w->f);
    bool need_zip64 = central_offset > ZIP64_LIMIT || central_size > ZIP64_LIMIT || w->count > 0xFFFF;
    if(need_zip64){
        long zip64_off=central_end;
        // zip64 EOCD
        write_le32(w->f,SIG_EOCD64);
        write_le64(w->f,44);
        write_le16(w->f,63); write_le16(w->f,ZIP64_VERSION);
        write_le32(w->f,0); write_le32(w->f,0);
        write_le64(w->f,w->count); write_le64(w->f,w->count);
        write_le64(w->f,central_size); write_le64(w->f,central_offset);
        // locator
        write_le32(w->f,SIG_EOCD64_LOC);
        write_le32(w->f,0);
        write_le64(w->f,zip64_off);
        write_le32(w->f,1);
    }
    // EOCD
    uint16_t n = need_zip64 ? 0xFFFF : (uint16_t)w->count;
    uint32_t sz = need_zip64 ? ZIP64_LIMIT : (uint32_t)central_size;
    uint32_t off = need_zip64 ? ZIP64_LIMIT : (uint32_t)central_offset;
    write_le32(w->f,SIG_EOCD);
    write_le16(w->f,0); write_le16(w->f,0);
    write_le16(w->f,n); write_le16(w->f,n);
    write_le32(w->f,sz); write_le32(w->f,off);
    write_le16(w->f,0);
    w->closed=true;
    return true;
}

void zip_writer_free(zip_writer_t *w){
    if(!w) return;
    for(size_t i=0;i<w->count;i++){
        free(w->entries[i].filename);
        free(w->entries[i].comp);
    }
    free(w->entries); free(w->offsets); free(w->is_zip64);
    if(w->f) fclose(w->f);
    memset(w,0,sizeof(*w));
}

// ---------- high level create_zip with traversal ----------
static bool add_recursively(zip_writer_t *w, const char *path, const char *base_parent, int level, bool is_top_dir);

static bool add_recursively(zip_writer_t *w, const char *path, const char *base_parent, int level, bool is_top_dir){
    struct stat st;
    if(stat(path,&st)!=0) return false;
    if(S_ISDIR(st.st_mode)){
        DIR *d=opendir(path);
        if(!d) return false;
        // if top dir and is directory, we want to include its basename as prefix
        // base_parent is dirname(parent) for arcname calc
        struct dirent *ent;
        while((ent=readdir(d))){
            if(strcmp(ent->d_name,".")==0 || strcmp(ent->d_name,"..")==0) continue;
            char full[4096];
            snprintf(full,sizeof(full),"%s/%s",path,ent->d_name);
            if(!add_recursively(w, full, base_parent, level, false)) { closedir(d); return false; }
        }
        closedir(d);
        // handle empty dir: add entry with trailing /
        // check if dir empty => we already handled files, need to add dir entry itself
        // For simplicity, we don't add empty dir entries unless top?
        return true;
    } else {
        // file: compute arcname = relpath(path, base_parent)
        char arc[4096];
        // simple relpath: if base_parent is prefix of path, arc = path+len(parent)+1
        size_t blen=strlen(base_parent);
        const char *rel;
        if(blen==0 || strcmp(base_parent,".")==0) rel=path;
        else if(strncmp(path,base_parent,blen)==0 && path[blen]=='/') rel=path+blen+1;
        else rel=path; // fallback
        // if rel starts with ./ strip
        while(rel[0]=='.' && rel[1]=='/') rel+=2;
        strncpy(arc,rel,sizeof(arc)-1); arc[sizeof(arc)-1]='\0';
        for(char *p=arc;*p;p++) if(*p=='\\') *p='/';
        return zip_writer_add_path(w, arc, path, level);
    }
}

bool create_zip(const char *archive, char **files, size_t nfiles, int level, int compat){
    if(!archive||!files||nfiles==0) return false;
    if(level<0||level>4) level=2;
    if(compat!=ZIP_COMPAT_WIDE && compat!=ZIP_COMPAT_MAX) compat=ZIP_COMPAT_WIDE;
    zip_writer_t w;
    if(!zip_writer_init(&w, archive, compat)) return false;
    bool ok=true;
    for(size_t i=0;i<nfiles;i++){
        const char *p=files[i];
        if(!p) continue;
        struct stat st;
        if(stat(p,&st)!=0){ ok=false; break; }
        if(S_ISDIR(st.st_mode)){
            // base_parent = dirname(abspath(p))
            char abspath[4096];
            if(!realpath(p,abspath)){ ok=false; break; }
            char *slash=strrchr(abspath,'/');
            char parent[4096];
            if(slash){ size_t len=slash-abspath; if(len==0) strcpy(parent,"/"); else { strncpy(parent,abspath,len); parent[len]='\0'; } }
            else strcpy(parent,".");
            if(!add_recursively(&w, p, parent, level, true)){ ok=false; break; }
        } else {
            char *base = strrchr(p,'/');
            const char *arc = base ? base+1 : p;
            if(!zip_writer_add_path(&w, arc, p, level)){ ok=false; break; }
        }
    }
    if(ok) ok=zip_writer_close(&w);
    zip_writer_free(&w);
    if(!ok) unlink(archive);
    return ok;
}
