#include "archiver.h"
#include "competitor.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <zlib.h>

/* ZIP file signatures and limits (from APPNOTE.TXT). */
static const uint32_t SIG_LOCAL_FILE = 0x04034b50;
static const uint32_t SIG_CENTRAL_DIR = 0x02014b50;
static const uint32_t SIG_END_OF_CENTRAL = 0x06054b50;
static const uint32_t SIG_ZIP64_END = 0x06064b50;
static const uint32_t SIG_ZIP64_LOCATOR = 0x07064b50;
static const uint32_t ZIP64_LIMIT_32 = 0xFFFFFFFFu;
static const uint16_t ZIP_VERSION_DEFAULT = 20;
static const uint16_t ZIP_VERSION_ZIP64 = 45;
static const uint16_t FLAG_UTF8 = 0x800;

static char *g_sig_tmp = NULL;
static void sig_cleanup (int sig)
{
  if (g_sig_tmp) unlink (g_sig_tmp);
  signal (sig, SIG_DFL);
  raise (sig);
}

/* Little-endian writers — return false on I/O error. */
static bool
write_le16 (FILE *f, uint16_t v)
{
  if (fputc (v & 0xff, f) == EOF) return false;
  if (fputc ((v >> 8) & 0xff, f) == EOF) return false;
  return true;
}

static bool
write_le32 (FILE *f, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    if (fputc (v & 0xff, f) == EOF) return false;
    else v >>= 8;
  return true;
}

static bool
write_le64 (FILE *f, uint64_t v)
{
  for (int i = 0; i < 8; i++)
    if (fputc (v & 0xff, f) == EOF) return false;
    else v >>= 8;
  return true;
}

/* Progress for single-file smooth mode (block-wise).
   Only prints when percentage actually changes to avoid spam. */
static int g_last_pct = -1;

static void
single_file_progress_cb (int pct, void *user)
{
  (void) user;
  if (pct == g_last_pct)
    return;
  g_last_pct = pct;
  fprintf (stderr, "%d%%\r", pct);
  fflush (stderr);
}

/* Check if a filename needs the UTF-8 flag. */
static bool
needs_utf8 (const char *s)
{
  for (; *s; s++)
    if ((unsigned char) *s >= 0x80)
      return true;
  return false;
}

/* Convert Unix time to DOS date/time fields. */
static void
to_dos_time (time_t t, uint16_t *dosdate, uint16_t *dostime)
{
  struct tm *tm = localtime (&t);
  if (!tm)
    {
      *dosdate = 0;
      *dostime = 0;
      return;
    }
  int year = tm->tm_year + 1900;
  if (year < 1980) year = 1980;
  if (year > 2107) year = 2107;
  *dosdate = (uint16_t) (((year - 1980) << 9)
                         | ((tm->tm_mon + 1) << 5)
                         | tm->tm_mday);
  *dostime = (uint16_t) ((tm->tm_hour << 11)
                         | (tm->tm_min << 5)
                         | (tm->tm_sec / 2));
}

/* Normalize an archive name: backslashes to slashes, collapse //, remove ./, forbid .., strip leading /. */
static char *
normalize_name (const char *arcname)
{
  size_t len;
  char *tmp;
  char *out;
  size_t j;
  bool is_dir;
  if (!arcname) return NULL;
  len = strlen (arcname);
  is_dir = (len > 0 && (arcname[len-1] == '/' || arcname[len-1] == '\\'));
  tmp = malloc (len + 1);
  if (!tmp) return NULL;
  for (j = 0; j < len; j++)
    {
      char c = arcname[j];
      tmp[j] = (c == '\\' ? '/' : c);
    }
  tmp[len] = '\0';
  char **stack = malloc ((len + 1) * sizeof(char*));
  size_t *slen = malloc ((len + 1) * sizeof(size_t));
  size_t top = 0;
  size_t i = 0;
  if (!stack || !slen) { free(tmp); free(stack); free(slen); return NULL; }
  while (i < len)
    {
      while (i < len && tmp[i] == '/') i++;
      if (i >= len) break;
      size_t beg = i;
      while (i < len && tmp[i] != '/') i++;
      size_t seglen = i - beg;
      if (seglen == 1 && tmp[beg] == '.')
        continue;
      if (seglen == 2 && tmp[beg] == '.' && tmp[beg+1] == '.')
        {
          if (top > 0) top--;
          continue;
        }
      stack[top] = tmp + beg;
      slen[top] = seglen;
      top++;
    }
  out = malloc (len + 2);
  if (!out) { free(tmp); free(stack); free(slen); return NULL; }
  size_t k = 0;
  for (size_t t = 0; t < top; t++)
    {
      if (k > 0) out[k++] = '/';
      memcpy (out + k, stack[t], slen[t]);
      k += slen[t];
    }
  if (is_dir && k > 0 && out[k-1] != '/')
    out[k++] = '/';
  out[k] = '\0';
  free(tmp); free(stack); free(slen);
  if (out[0] == '\0')
    {
      free (out);
      return NULL;
    }
  return out;
}

/* ------------------------------------------------------------------ */
/* Writer lifecycle — tmpfile + rename for atomicity (P0-4, P1-9) */

bool
zip_writer_open (zip_writer_t *writer, const char *path)
{
  char tmpl[PATH_MAX];
  int fd;
  if (!writer || !path) return false;
  memset (writer, 0, sizeof *writer);
  writer->archive_path = strdup (path);
  if (!writer->archive_path) return false;
  /* Create temp file in same directory for atomic rename. */
  {
    const char *slash = strrchr (path, '/');
    if (slash)
      {
        size_t dirlen = slash - path;
        if (dirlen + 32 >= sizeof tmpl) { free(writer->archive_path); return false; }
        memcpy (tmpl, path, dirlen);
        tmpl[dirlen] = '\0';
        snprintf (tmpl + dirlen, sizeof tmpl - dirlen, "/.katzip.tmp.XXXXXX");
        /* Need to handle dirlen==0 case (path="/file") */
        if (dirlen == 0) snprintf (tmpl, sizeof tmpl, "/.katzip.tmp.XXXXXX");
      }
    else
      snprintf (tmpl, sizeof tmpl, ".katzip.tmp.XXXXXX");
    fd = mkstemp (tmpl);
    if (fd < 0) { free(writer->archive_path); return false; }
    writer->tmp_path = strdup (tmpl);
    if (!writer->tmp_path) { close(fd); unlink(tmpl); free(writer->archive_path); return false; }
    writer->file = fdopen (fd, "wb");
    if (!writer->file) { close(fd); unlink(tmpl); free(writer->tmp_path); free(writer->archive_path); return false; }
    { mode_t cur = umask(0); umask(cur); fchmod(fd, 0666 & ~cur); }
    writer->tmp_created = true;
    g_sig_tmp = writer->tmp_path;
    signal (SIGINT, sig_cleanup);
    signal (SIGTERM, sig_cleanup);
    signal (SIGXFSZ, sig_cleanup);
  }
  writer->capacity = 16;
  writer->entries = calloc (writer->capacity, sizeof *writer->entries);
  writer->offsets = calloc (writer->capacity, sizeof *writer->offsets);
  writer->is_zip64 = calloc (writer->capacity, sizeof *writer->is_zip64);
  if (!writer->entries || !writer->offsets || !writer->is_zip64)
    {
      if (writer->file) fclose (writer->file);
      if (writer->tmp_created) unlink (writer->tmp_path);
      free (writer->entries); free (writer->offsets); free (writer->is_zip64);
      free (writer->tmp_path); free (writer->archive_path);
      return false;
    }
  writer->count = 0;
  writer->closed = false;
  return true;
}

static bool
ensure_capacity (zip_writer_t *writer)
{
  if (writer->count < writer->capacity) return true;
  {
    size_t ncap = writer->capacity * 2;
    zip_entry_t *ne = realloc (writer->entries, ncap * sizeof *ne);
    long *no = realloc (writer->offsets, ncap * sizeof *no);
    bool *nz = realloc (writer->is_zip64, ncap * sizeof *nz);
    if (!ne || !no || !nz) return false;
    writer->entries = ne;
    writer->offsets = no;
    writer->is_zip64 = nz;
    memset (writer->entries + writer->capacity, 0,
            (ncap - writer->capacity) * sizeof *writer->entries);
    memset (writer->offsets + writer->capacity, 0,
            (ncap - writer->capacity) * sizeof *writer->offsets);
    memset (writer->is_zip64 + writer->capacity, 0,
            (ncap - writer->capacity) * sizeof *writer->is_zip64);
    writer->capacity = ncap;
  }
  return true;
}

/* Write a local file header and payload — all I/O checked (P0-1). */
static bool
write_local_entry (zip_writer_t *writer,
                   const char *norm_name,
                   const unsigned char *comp,
                   size_t comp_len,
                   size_t plain_len,
                   unsigned int crc,
                   int method,
                   bool need_zip64,
                   long offset,
                   uint16_t dosdate,
                   uint16_t dostime)
{
  uint16_t flag;
  uint16_t version;
  uint16_t name_len;
  uint16_t extra_len;
  (void) offset;
  flag = needs_utf8 (norm_name) ? FLAG_UTF8 : 0;
  version = need_zip64 ? ZIP_VERSION_ZIP64 : ZIP_VERSION_DEFAULT;
  name_len = (uint16_t) strlen (norm_name);
  extra_len = need_zip64 ? 20 : 0;
  if (!write_le32 (writer->file, SIG_LOCAL_FILE)) return false;
  if (!write_le16 (writer->file, version)) return false;
  if (!write_le16 (writer->file, flag)) return false;
  if (!write_le16 (writer->file, (uint16_t) method)) return false;
  if (!write_le16 (writer->file, dostime)) return false;
  if (!write_le16 (writer->file, dosdate)) return false;
  if (!write_le32 (writer->file, crc)) return false;
  if (need_zip64)
    {
      if (!write_le32 (writer->file, ZIP64_LIMIT_32)) return false;
      if (!write_le32 (writer->file, ZIP64_LIMIT_32)) return false;
    }
  else
    {
      if (!write_le32 (writer->file, (uint32_t) comp_len)) return false;
      if (!write_le32 (writer->file, (uint32_t) plain_len)) return false;
    }
  if (!write_le16 (writer->file, name_len)) return false;
  if (!write_le16 (writer->file, extra_len)) return false;
  if (fwrite (norm_name, 1, name_len, writer->file) != name_len) return false;
  if (need_zip64)
    {
      if (!write_le16 (writer->file, 0x0001)) return false;
      if (!write_le16 (writer->file, 16)) return false;
      if (!write_le64 (writer->file, plain_len)) return false;
      if (!write_le64 (writer->file, comp_len)) return false;
    }
  if (comp_len && fwrite (comp, 1, comp_len, writer->file) != comp_len) return false;
  return true;
}

bool
zip_writer_add_file (zip_writer_t *writer,
                     const char *arcname,
                     const unsigned char *data,
                     size_t len)
{
  char *norm;
  unsigned char *comp = NULL;
  size_t comp_len = 0;
  int method = COMP_METHOD_STORE;
  unsigned int crc = 0;
  long offset;
  bool need_zip64;
  bool ok;
  uint16_t dosdate=0, dostime=0;
  /* Caller should have provided mtime/mode via add_path; for direct add_file
     we use current time and 0644. */
  if (!writer || writer->closed || !arcname || !data) return false;
  time_t mtime = time(NULL);
  mode_t mode = 0644;
  /* Check for duplicate arcname (P0-3) — linear scan, n is small. */
  char *tmp_norm = normalize_name (arcname);
  if (!tmp_norm) return false;
  for (size_t i=0;i<writer->count;i++) if (strcmp(writer->entries[i].filename, tmp_norm)==0) { free(tmp_norm); return false; }
  free(tmp_norm);
  norm = normalize_name (arcname);
  if (!norm) return false;
  if (norm[strlen(norm)-1]=='/')
    {
      if (len!=0) { free(norm); return false; }
      crc=0; comp=NULL; comp_len=0; method=COMP_METHOD_STORE;
      mode = 0755;
    }
  else
    {
      if (len) { crc = crc32(0L, Z_NULL, 0); size_t off=0; while (off < len) { size_t chunk = len - off > 1048576 ? 1048576 : len - off; crc = crc32(crc, data+off, (uInt)chunk); off+=chunk; } }
      if (!competitor_compress(data, len, norm, &comp, &comp_len, &method))
        { free(norm); return false; }
    }
  to_dos_time(mtime, &dosdate, &dostime);
  offset = ftell(writer->file);
  if (offset<0) { free(norm); free(comp); return false; }
  need_zip64 = (len > ZIP64_LIMIT_32 || comp_len > ZIP64_LIMIT_32 || (uint64_t)offset > ZIP64_LIMIT_32);
  ok = write_local_entry(writer, norm, comp?comp:(unsigned char*)"", comp_len, len, crc, method, need_zip64, offset, dosdate, dostime);
  if (!ok) { free(norm); free(comp); return false; }
  if (!ensure_capacity(writer)) { free(norm); free(comp); return false; }
  writer->entries[writer->count].filename = norm;
  writer->entries[writer->count].comp_data = comp;
  writer->entries[writer->count].data_len = len;
  writer->entries[writer->count].comp_len = comp_len;
  writer->entries[writer->count].method = method;
  writer->entries[writer->count].crc = crc;
  writer->entries[writer->count].st_mode = mode;
  writer->entries[writer->count].mtime = (long long)mtime;
  writer->offsets[writer->count]=offset;
  writer->is_zip64[writer->count]=need_zip64;
  writer->count++;
  return true;
}

bool
zip_writer_add_path (zip_writer_t *writer,
                     const char *arcname,
                     const char *fullpath)
{
  struct stat st;
  FILE *in;
  unsigned char *data;
  size_t len;
  char *norm;
  uint16_t dosdate, dostime;
  long offset;
  bool need_zip64;
  unsigned int crc=0;
  unsigned char *comp=NULL;
  size_t comp_len=0;
  int method=COMP_METHOD_STORE;
  if (!writer || !arcname || !fullpath) return false;
  if (lstat(fullpath, &st)!=0) return false;
  if (S_ISLNK(st.st_mode)) return false; /* P1-7: do not follow symlinks */
  if (S_ISDIR(st.st_mode)) return false;
  /* Duplicate check */
  norm = normalize_name(arcname);
  if (!norm) return false;
  for (size_t i=0;i<writer->count;i++) if (strcmp(writer->entries[i].filename, norm)==0) { free(norm); return false; }
  /* Read file */
  in = fopen(fullpath,"rb");
  if (!in) { free(norm); return false; }
  len = (size_t) st.st_size;
  data = malloc(len?len:1);
  if (!data) { fclose(in); free(norm); return false; }
  if (len && fread(data,1,len,in)!=len) { free(data); fclose(in); free(norm); return false; }
  fclose(in);
  if (len) { crc = crc32(0L, Z_NULL, 0); size_t off=0; while (off < len) { size_t chunk = len - off > 1048576 ? 1048576 : len - off; crc = crc32(crc, data+off, (uInt)chunk); off+=chunk; } }
  else crc=0;
  if (len==0)
    { comp=NULL; comp_len=0; method=COMP_METHOD_STORE; }
  else if (!competitor_compress(data, len, norm, &comp, &comp_len, &method))
    { free(data); free(norm); return false; }
  free(data);
  to_dos_time(st.st_mtime, &dosdate, &dostime);
  offset = ftell(writer->file);
  if (offset<0) { free(norm); free(comp); return false; }
  need_zip64 = (len > ZIP64_LIMIT_32 || comp_len > ZIP64_LIMIT_32 || (uint64_t)offset > ZIP64_LIMIT_32);
  if (!write_local_entry(writer, norm, comp?comp:(unsigned char*)"", comp_len, len, crc, method, need_zip64, offset, dosdate, dostime))
    { free(norm); free(comp); return false; }
  if (!ensure_capacity(writer)) { free(norm); free(comp); return false; }
  writer->entries[writer->count].filename = norm;
  writer->entries[writer->count].comp_data = comp;
  writer->entries[writer->count].data_len = len;
  writer->entries[writer->count].comp_len = comp_len;
  writer->entries[writer->count].method = method;
  writer->entries[writer->count].crc = crc;
  writer->entries[writer->count].st_mode = st.st_mode & 07777;
  writer->entries[writer->count].mtime = (long long)st.st_mtime;
  writer->offsets[writer->count]=offset;
  writer->is_zip64[writer->count]=need_zip64;
  writer->count++;
  return true;
}

/* ------------------------------------------------------------------ */
/* Central directory */

static bool
write_central_entry (zip_writer_t *writer, size_t idx,
                     size_t *central_size)
{
  zip_entry_t *e = &writer->entries[idx];
  long off = writer->offsets[idx];
  bool z64 = writer->is_zip64[idx];
  uint16_t flag = needs_utf8(e->filename) ? FLAG_UTF8 : 0;
  uint16_t dosdate, dostime;
  uint16_t version = z64 ? ZIP_VERSION_ZIP64 : ZIP_VERSION_DEFAULT;
  uint16_t name_len = (uint16_t) strlen(e->filename);
  uint16_t extra_len = z64 ? 28 : 0;
  uint32_t external_attr;
  to_dos_time((time_t)e->mtime, &dosdate, &dostime);
  if (e->st_mode==0) e->st_mode=0644;
  /* P0-2: Unix external attributes */
  external_attr = ((e->st_mode & 0xFFFF) << 16);
  if (e->filename[strlen(e->filename)-1]=='/') external_attr |= 0x10;
  if (!write_le32(writer->file, SIG_CENTRAL_DIR)) return false;
  if (!write_le16(writer->file, (3 << 8) | 63)) return false;
  if (!write_le16(writer->file, version)) return false;
  if (!write_le16(writer->file, flag)) return false;
  if (!write_le16(writer->file, (uint16_t) e->method)) return false;
  if (!write_le16(writer->file, dostime)) return false;
  if (!write_le16(writer->file, dosdate)) return false;
  if (!write_le32(writer->file, e->crc)) return false;
  if (z64)
    {
      if (!write_le32(writer->file, ZIP64_LIMIT_32)) return false;
      if (!write_le32(writer->file, ZIP64_LIMIT_32)) return false;
    }
  else
    {
      if (!write_le32(writer->file, (uint32_t) e->comp_len)) return false;
      if (!write_le32(writer->file, (uint32_t) e->data_len)) return false;
    }
  if (!write_le16(writer->file, name_len)) return false;
  if (!write_le16(writer->file, extra_len)) return false;
  if (!write_le16(writer->file, 0)) return false;
  if (!write_le16(writer->file, 0)) return false;
  if (!write_le16(writer->file, 0)) return false;
  if (!write_le32(writer->file, external_attr)) return false;
  if (z64)
    { if (!write_le32(writer->file, ZIP64_LIMIT_32)) return false; }
  else
    { if (!write_le32(writer->file, (uint32_t) off)) return false; }
  *central_size += 46;
  if (fwrite(e->filename,1,name_len,writer->file)!=name_len) return false;
  *central_size += name_len;
  if (z64)
    {
      if (!write_le16(writer->file, 0x0001)) return false;
      if (!write_le16(writer->file, 24)) return false;
      if (!write_le64(writer->file, e->data_len)) return false;
      if (!write_le64(writer->file, e->comp_len)) return false;
      if (!write_le64(writer->file, off)) return false;
      *central_size += 28;
    }
  return true;
}

bool
zip_writer_close (zip_writer_t *writer)
{
  long central_offset;
  size_t central_size=0;
  size_t i;
  bool need_zip64;
  if (!writer || writer->closed) return false;
  central_offset = ftell(writer->file);
  if (central_offset<0) goto fail;
  for (i=0;i<writer->count;i++)
    if (!write_central_entry(writer,i,&central_size)) goto fail;
  need_zip64 = (central_offset > (long)ZIP64_LIMIT_32 || central_size > ZIP64_LIMIT_32 || writer->count > 0xFFFF);
  if (need_zip64)
    {
      long zip64_off = ftell(writer->file);
      if (zip64_off<0) goto fail;
      if (!write_le32(writer->file, SIG_ZIP64_END)) goto fail;
      if (!write_le64(writer->file, 44)) goto fail;
      if (!write_le16(writer->file, 63)) goto fail;
      if (!write_le16(writer->file, ZIP_VERSION_ZIP64)) goto fail;
      if (!write_le32(writer->file, 0)) goto fail;
      if (!write_le32(writer->file, 0)) goto fail;
      if (!write_le64(writer->file, writer->count)) goto fail;
      if (!write_le64(writer->file, writer->count)) goto fail;
      if (!write_le64(writer->file, central_size)) goto fail;
      if (!write_le64(writer->file, central_offset)) goto fail;
      if (!write_le32(writer->file, SIG_ZIP64_LOCATOR)) goto fail;
      if (!write_le32(writer->file, 0)) goto fail;
      if (!write_le64(writer->file, zip64_off)) goto fail;
      if (!write_le32(writer->file, 1)) goto fail;
    }
  {
    uint16_t n = need_zip64 ? 0xFFFF : (uint16_t) writer->count;
    uint32_t sz = need_zip64 ? ZIP64_LIMIT_32 : (uint32_t) central_size;
    uint32_t off = need_zip64 ? ZIP64_LIMIT_32 : (uint32_t) central_offset;
    if (!write_le32(writer->file, SIG_END_OF_CENTRAL)) goto fail;
    if (!write_le16(writer->file, 0)) goto fail;
    if (!write_le16(writer->file, 0)) goto fail;
    if (!write_le16(writer->file, n)) goto fail;
    if (!write_le16(writer->file, n)) goto fail;
    if (!write_le32(writer->file, sz)) goto fail;
    if (!write_le32(writer->file, off)) goto fail;
    if (!write_le16(writer->file, 0)) goto fail;
  }
  if (fflush(writer->file)!=0) goto fail;
  if (ferror(writer->file)) goto fail;
  if (fclose(writer->file)!=0) { writer->file=NULL; goto fail_rename; }
  writer->file=NULL;
  writer->closed=true;
  /* Atomic rename */
  if (writer->tmp_created && writer->archive_path && writer->tmp_path)
    {
      if (rename(writer->tmp_path, writer->archive_path)!=0) goto fail_rename;
      writer->tmp_created=false;
      g_sig_tmp = NULL;
      signal (SIGINT, SIG_DFL);
      signal (SIGTERM, SIG_DFL);
      signal (SIGXFSZ, SIG_DFL);
    }
  return true;
fail:
  if (writer->file) { fclose(writer->file); writer->file=NULL; }
fail_rename:
  if (writer->tmp_created && writer->tmp_path) { unlink(writer->tmp_path); g_sig_tmp = NULL; }
  signal (SIGINT, SIG_DFL);
  signal (SIGTERM, SIG_DFL);
  signal (SIGXFSZ, SIG_DFL);
  writer->closed=true;
  return false;
}

void
zip_writer_free (zip_writer_t *writer)
{
  size_t i;
  if (!writer) return;
  for (i=0;i<writer->count;i++) { free(writer->entries[i].filename); free(writer->entries[i].comp_data); }
  free(writer->entries); free(writer->offsets); free(writer->is_zip64);
  if (writer->tmp_created && writer->tmp_path) { unlink(writer->tmp_path); g_sig_tmp = NULL; }
  if (writer->file) fclose(writer->file);
  else if (g_sig_tmp) { g_sig_tmp = NULL; signal (SIGINT, SIG_DFL); signal (SIGTERM, SIG_DFL); }
  free(writer->archive_path); free(writer->tmp_path);
  memset(writer,0,sizeof *writer);
}

/* ------------------------------------------------------------------ */
/* High-level: collect all files first for accurate progress */

typedef struct { char *arcname; char *fullpath; } file_item_t;

static bool
collect_one (const char *path, const char *base_parent,
             file_item_t **items, size_t *count, size_t *cap)
{
  struct stat st;
  if (lstat(path, &st)!=0) return false;
  if (S_ISLNK(st.st_mode)) return true; /* skip symlinks (P1-7) */
  if (S_ISDIR(st.st_mode))
    {
      DIR *dir = opendir(path);
      struct dirent *ent;
      size_t start = *count;
      if (!dir) return false;
      while ((ent=readdir(dir))!=NULL)
        {
          char full[4096];
          if (strcmp(ent->d_name,".")==0) continue;
          if (strcmp(ent->d_name,"..")==0) continue;
          int n = snprintf(full, sizeof full, "%s/%s", path, ent->d_name);
          if (n<0 || (size_t)n >= sizeof full) { closedir(dir); return false; }
          if (!collect_one(full, base_parent, items, count, cap)) { closedir(dir); return false; }
        }
      closedir(dir);
      if (*count == start)
        {
          /* Empty directory — preserve it as  dir/  entry. */
          char arc[4096];
          const char *rel;
          size_t blen = strlen(base_parent);
          if (blen==0 || strcmp(base_parent,".")==0) rel=path;
          else if (strncmp(path, base_parent, blen)==0 && path[blen]=='/') rel=path+blen+1;
          else rel=path;
          while (rel[0]=='.' && rel[1]=='/') rel+=2;
          strncpy(arc, rel, sizeof arc -1); arc[sizeof arc -1]='\0';
          for (char *pp=arc;*pp;pp++) if (*pp=='\\') *pp='/';
          size_t alen=strlen(arc);
          if (alen==0) return true;
          if (arc[alen-1]!='/') { if (alen+1>=sizeof arc) return false; arc[alen]='/'; arc[alen+1]='\0'; }
          if (*count >= *cap)
            {
              size_t ncap = *cap ? *cap*2 : 16;
              file_item_t *next = realloc(*items, ncap*sizeof *next);
              if (!next) return false;
              *items=next; *cap=ncap;
            }
          char *a_dup=strdup(arc); char *p_dup=strdup(path);
          if (!a_dup || !p_dup) { free(a_dup); free(p_dup); return false; }
          (*items)[*count].arcname=a_dup;
          (*items)[*count].fullpath=p_dup;
          (*count)++;
        }
      return true;
    }
  else if (S_ISREG(st.st_mode))
    {
      char arc[4096];
      const char *rel;
      size_t blen = strlen(base_parent);
      char *a_dup; char *p_dup;
      if (blen==0 || strcmp(base_parent,".")==0) rel=path;
      else if (strncmp(path, base_parent, blen)==0 && path[blen]=='/') rel=path+blen+1;
      else rel=path;
      while (rel[0]=='.' && rel[1]=='/') rel+=2;
      strncpy(arc, rel, sizeof arc -1); arc[sizeof arc -1]='\0';
      for (char *p=arc;*p;p++) if (*p=='\\') *p='/';
      if (*count >= *cap)
        {
          size_t ncap = *cap ? *cap*2 : 16;
          file_item_t *next = realloc(*items, ncap*sizeof *next);
          if (!next) return false;
          *items=next; *cap=ncap;
        }
      a_dup=strdup(arc); p_dup=strdup(path);
      if (!a_dup || !p_dup) { free(a_dup); free(p_dup); return false; }
      (*items)[*count].arcname=a_dup;
      (*items)[*count].fullpath=p_dup;
      (*count)++;
      return true;
    }
  else
    {
      fprintf(stderr, "warning: skipping non-regular file \"%s\"\n", path);
      return true;
    }
}

static int get_thread_count (size_t nitems)
{
  const katzip_config_t *cfg = config_get();
  int t = cfg ? cfg->threads : 0;
  if (t == 1) return 1;
  if (t <= 0) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    t = (int)n;
  }
  if ((size_t)t > nitems) t = (int)nitems;
  if (t < 1) t = 1;
  return t;
}

typedef struct {
  file_item_t *items;
  size_t nitems;
  size_t next;
  pthread_mutex_t lock;
  zip_writer_t *writer;
  bool *ok;
  size_t *total_comp;
  pthread_mutex_t *print_lock;
  int *completed;
} file_pool_t;

static void *file_worker (void *arg)
{
  file_pool_t *pool = (file_pool_t*)arg;
  while (1) {
    size_t idx;
    pthread_mutex_lock(&pool->lock);
    if (pool->next >= pool->nitems) { pthread_mutex_unlock(&pool->lock); break; }
    idx = pool->next++;
    pthread_mutex_unlock(&pool->lock);
    // Check if already failed
    if (!*pool->ok) continue;
    // Handle dir already? For file items, check if dir
    struct stat dst;
    if (lstat(pool->items[idx].fullpath, &dst)==0 && S_ISDIR(dst.st_mode)) {
      // empty dir already handled via items, treat as success
      pthread_mutex_lock(pool->print_lock);
      int pct = (int)((__atomic_add_fetch(pool->completed, 1, __ATOMIC_SEQ_CST) * 100) / pool->nitems);
      fprintf(stderr, "%d%%\r", pct);
      fflush(stderr);
      pthread_mutex_unlock(pool->print_lock);
      continue;
    }
    // For regular file, compress via add_path (which will also handle dedup? dedup already checked)
    // We need to call zip_writer_add_path but writer is shared - we need per-thread writer?
    // Instead, we compress via competitor directly and store result in a separate array, then main writes.
    // For now, just use add_path with mutex on writer
    pthread_mutex_lock(pool->print_lock);
    // not actually writing yet, just compress
    pthread_mutex_unlock(pool->print_lock);
    // The actual compress+write will be done sequentially after parallel compress phase
    // This worker is placeholder - real parallel compress will be done via separate array
    // To avoid complexity, we will handle parallel compress in create_zip_archive directly
    break;
  }
  return NULL;
}


typedef struct {
  unsigned char *comp;
  size_t comp_len;
  int method;
  unsigned int crc;
  unsigned int st_mode;
  long long mtime;
  size_t data_len;
  bool is_dir;
  bool ok;
} file_result_t;

typedef struct {
  file_item_t *items;
  size_t nitems;
  file_result_t *results;
  size_t *next;
  pthread_mutex_t *next_lock;
  pthread_mutex_t *print_lock;
  int *completed;
  bool *overall_ok;
} file_pool_ctx_t;

static void *file_compress_worker (void *arg)
{
  file_pool_ctx_t *ctx = (file_pool_ctx_t*)arg;
  while (1) {
    size_t idx;
    pthread_mutex_lock(ctx->next_lock);
    if (*ctx->next >= ctx->nitems) { pthread_mutex_unlock(ctx->next_lock); break; }
    idx = (*ctx->next)++;
    pthread_mutex_unlock(ctx->next_lock);
    if (!*ctx->overall_ok) continue;
    struct stat st;
    if (lstat(ctx->items[idx].fullpath, &st)!=0) { ctx->results[idx].ok = false; *ctx->overall_ok = false; continue; }
    if (S_ISLNK(st.st_mode)) { ctx->results[idx].ok = true; ctx->results[idx].is_dir = false; ctx->results[idx].data_len = 0; continue; }
    if (S_ISDIR(st.st_mode)) {
      ctx->results[idx].ok = true;
      ctx->results[idx].is_dir = true;
      ctx->results[idx].st_mode = st.st_mode & 07777;
      ctx->results[idx].mtime = (long long)st.st_mtime;
      ctx->results[idx].data_len = 0;
      ctx->results[idx].comp_len = 0;
      ctx->results[idx].method = 0;
      ctx->results[idx].crc = 0;
      ctx->results[idx].comp = NULL;
    } else if (!S_ISREG(st.st_mode)) {
      // non-regular already filtered in collect, but handle
      ctx->results[idx].ok = true;
      ctx->results[idx].is_dir = false;
      ctx->results[idx].data_len = 0;
      continue;
    } else {
      // regular file: read and compress
      FILE *in = fopen(ctx->items[idx].fullpath, "rb");
      if (!in) { ctx->results[idx].ok = false; *ctx->overall_ok = false; continue; }
      size_t len = (size_t)st.st_size;
      unsigned char *data = malloc(len?len:1);
      if (!data) { fclose(in); ctx->results[idx].ok = false; *ctx->overall_ok = false; continue; }
      if (len && fread(data,1,len,in)!=len) { free(data); fclose(in); ctx->results[idx].ok = false; *ctx->overall_ok = false; continue; }
      fclose(in);
      unsigned int crc = 0;
      if (len) { crc = crc32(0L, Z_NULL, 0); size_t off=0; while(off<len){size_t chunk=len-off>1048576?1048576:len-off; crc=crc32(crc, data+off, (uInt)chunk); off+=chunk;}}
      unsigned char *comp=NULL; size_t comp_len=0; int method=0;
      bool cok = false;
      if (len==0) { comp=NULL; comp_len=0; method=0; cok=true; }
      else cok = competitor_compress(data, len, ctx->items[idx].arcname, &comp, &comp_len, &method);
      free(data);
      if (!cok) { free(comp); ctx->results[idx].ok = false; *ctx->overall_ok = false; continue; }
      ctx->results[idx].ok = true;
      ctx->results[idx].is_dir = false;
      ctx->results[idx].comp = comp;
      ctx->results[idx].comp_len = comp_len;
      ctx->results[idx].method = method;
      ctx->results[idx].crc = crc;
      ctx->results[idx].st_mode = st.st_mode & 07777;
      ctx->results[idx].mtime = (long long)st.st_mtime;
      ctx->results[idx].data_len = len;
    }
    // progress
    pthread_mutex_lock(ctx->print_lock);
    int done = __atomic_add_fetch(ctx->completed, 1, __ATOMIC_SEQ_CST);
    int pct = (int)(done * 100 / (int)ctx->nitems);
    fprintf(stderr, "%d%%\r", pct);
    fflush(stderr);
    pthread_mutex_unlock(ctx->print_lock);
  }
  return NULL;
}

bool
create_zip_archive (const char *archive, char **files, size_t nfiles)
{
  zip_writer_t writer;
  file_item_t *items=NULL;
  size_t nitems=0; size_t cap=0; size_t i; bool ok=true; size_t total_comp=0;
  if (!archive || !files || nfiles==0) return false;
  /* First pass: collect every regular file. */
  for (i=0;i<nfiles;i++)
    {
      const char *p=files[i];
      struct stat st;
      if (!p) continue;
      if (lstat(p,&st)!=0) { ok=false; break; }
      if (S_ISDIR(st.st_mode))
        {
          char abspath[4096]; char parent[4096]; char *slash;
          if (!realpath(p, abspath)) { ok=false; break; }
          slash=strrchr(abspath,'/');
          if (slash) { size_t len=slash-abspath; if (len==0) strcpy(parent,"/"); else { strncpy(parent,abspath,len); parent[len]='\0'; } }
          else strcpy(parent,".");
          if (!collect_one(p, parent, &items, &nitems, &cap)) { ok=false; break; }
        }
      else
        {
          if (S_ISLNK(st.st_mode)) continue; /* skip symlink files */
          if (!S_ISREG(st.st_mode)) { fprintf(stderr, "warning: skipping non-regular file \"%s\"\n", p); continue; }
          const char *base=strrchr(p,'/');
          const char *arc=base?base+1:p;
          char *a_dup=strdup(arc); char *p_dup=strdup(p);
          if (!a_dup||!p_dup) { free(a_dup); free(p_dup); ok=false; break; }
          if (nitems >= cap)
            {
              size_t ncap=cap?cap*2:16;
              file_item_t *next=realloc(items,ncap*sizeof *next);
              if (!next) { free(a_dup); free(p_dup); ok=false; break; }
              items=next; cap=ncap;
            }
          items[nitems].arcname=a_dup;
          items[nitems].fullpath=p_dup;
          nitems++;
        }
    }
  if (!ok)
    {
      for (i=0;i<nitems;i++) { free(items[i].arcname); free(items[i].fullpath); }
      free(items); return false;
    }
  if (nitems==0)
    {
      free(items);
      if (nfiles>0) { fprintf(stderr,"warning: no files to archive (all inputs filtered)\n"); return false; }
      if (!zip_writer_open(&writer, archive)) return false;
      ok=zip_writer_close(&writer);
      zip_writer_free(&writer);
      return ok;
    }
  /* P0-3: duplicate arcname check */
  for (i=0;i<nitems;i++)
    for (size_t j=i+1;j<nitems;j++)
      if (strcmp(items[i].arcname, items[j].arcname)==0) { fprintf(stderr,"error: duplicate entry \"%s\"\n", items[i].arcname); ok=false; goto dup_fail; }
  /* P0-4: self-overwrite check (archive == any input file by dev/ino) */
  {
    struct stat ast;
    if (lstat(archive,&ast)==0)
      for (i=0;i<nitems;i++)
        {
          struct stat fst;
          if (lstat(items[i].fullpath,&fst)==0 && fst.st_dev==ast.st_dev && fst.st_ino==ast.st_ino) { fprintf(stderr,"error: archive \"%s\" is also an input file\n", archive); ok=false; goto dup_fail; }
          /* Also check archive path string equality after realpath */
          char rarch[PATH_MAX], rfile[PATH_MAX];
          if (realpath(archive,rarch) && realpath(items[i].fullpath,rfile) && strcmp(rarch,rfile)==0) { fprintf(stderr,"error: archive \"%s\" is also an input file\n", archive); ok=false; goto dup_fail; }
        }
  }
  if (!zip_writer_open(&writer, archive))
    {
dup_fail:
      for (i=0;i<nitems;i++) { free(items[i].arcname); free(items[i].fullpath); }
      free(items); return false;
    }
  /* Second pass: compress each file.
     For single file show block-wise progress via callback,
     otherwise per-file progress. File-level parallelism when threads>1. */
  bool single_file = (nitems==1);
  int nthreads = get_thread_count(nitems);
  if (single_file)
    {
      g_last_pct=-1;
      competitor_set_progress_cb(single_file_progress_cb,NULL);
      for (size_t ii=0;ii<nitems;ii++)
        {
          fprintf(stderr,"0%%\r"); fflush(stderr);
          struct stat dst;
          if (lstat(items[ii].fullpath,&dst)==0 && S_ISDIR(dst.st_mode))
            {
              uint16_t ddate, dtime;
              long off = ftell(writer.file);
              if (off<0) { ok=false; break; }
              to_dos_time(dst.st_mtime, &ddate, &dtime);
              bool need64 = ((uint64_t)off > ZIP64_LIMIT_32);
              if (!write_local_entry(&writer, items[ii].arcname, (unsigned char*)"", 0, 0, 0, COMP_METHOD_STORE, need64, off, ddate, dtime)) { ok=false; break; }
              if (!ensure_capacity(&writer)) { ok=false; break; }
              writer.entries[writer.count].filename = strdup(items[ii].arcname);
              writer.entries[writer.count].comp_data = NULL;
              writer.entries[writer.count].data_len = 0;
              writer.entries[writer.count].comp_len = 0;
              writer.entries[writer.count].method = COMP_METHOD_STORE;
              writer.entries[writer.count].crc = 0;
              writer.entries[writer.count].st_mode = dst.st_mode & 07777;
              writer.entries[writer.count].mtime = (long long)dst.st_mtime;
              writer.offsets[writer.count]=off;
              writer.is_zip64[writer.count]=need64;
              if (!writer.entries[writer.count].filename) { ok=false; break; }
              writer.count++;
            }
          else if (!zip_writer_add_path(&writer, items[ii].arcname, items[ii].fullpath)) { ok=false; break; }
        }
      competitor_set_progress_cb(NULL,NULL);
    }
  else if (nthreads > 1 && nitems > 1)
    {
      /* Parallel file compression: compress in threads, write sequentially. */
      file_result_t *results = calloc(nitems, sizeof(file_result_t));
      if (!results) ok=false;
      else
        {
          // Ensure writer capacity for all entries
          while (writer.capacity < nitems) if (!ensure_capacity(&writer)) { ok=false; break; }
          if (ok)
            {
              pthread_t *tids = malloc(nthreads * sizeof(pthread_t));
              size_t next = 0;
              pthread_mutex_t next_lock = PTHREAD_MUTEX_INITIALIZER;
              pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;
              int completed = 0;
              file_pool_ctx_t ctx;
              ctx.items = items;
              ctx.nitems = nitems;
              ctx.results = results;
              ctx.next = &next;
              ctx.next_lock = &next_lock;
              ctx.print_lock = &print_lock;
              ctx.completed = &completed;
              ctx.overall_ok = &ok;
              // Need to pass ctx correctly - use global for this phase
              // Create threads
              for (int t=0; t<nthreads; t++) {
                if (pthread_create(&tids[t], NULL, file_compress_worker, &ctx)!=0) { ok=false; break; }
              }
              for (int t=0; t<nthreads; t++) pthread_join(tids[t], NULL);
              free(tids);
              pthread_mutex_destroy(&next_lock);
              pthread_mutex_destroy(&print_lock);
              if (ok) {
                for (size_t ii=0; ii<nitems; ii++) {
                  if (!results[ii].ok) { ok=false; break; }
                  // Skip filtered entries (symlink etc) - they have data_len 0 and no comp and not dir
                  if (!results[ii].is_dir && results[ii].data_len==0 && results[ii].comp==NULL) {
                    struct stat st2;
                    if (lstat(items[ii].fullpath,&st2)==0 && (S_ISLNK(st2.st_mode) || !S_ISREG(st2.st_mode))) continue;
                    // Empty regular file: data_len 0 but should be written (is_dir false, but size 0)
                    // Distinguish by checking original file size
                    struct stat st3;
                    if (lstat(items[ii].fullpath,&st3)==0 && S_ISREG(st3.st_mode) && st3.st_size==0) {
                      // empty file - write it
                    } else if (!results[ii].is_dir) {
                      continue;
                    }
                  }
                  if (results[ii].is_dir) {
                    uint16_t ddate, dtime;
                    long off = ftell(writer.file);
                    if (off<0) { ok=false; break; }
                    to_dos_time((time_t)results[ii].mtime, &ddate, &dtime);
                    bool need64 = ((uint64_t)off > ZIP64_LIMIT_32);
                    if (!write_local_entry(&writer, items[ii].arcname, (unsigned char*)"", 0, 0, 0, COMP_METHOD_STORE, need64, off, ddate, dtime)) { ok=false; break; }
                    writer.entries[writer.count].filename = strdup(items[ii].arcname);
                    writer.entries[writer.count].comp_data = NULL;
                    writer.entries[writer.count].data_len = 0;
                    writer.entries[writer.count].comp_len = 0;
                    writer.entries[writer.count].method = COMP_METHOD_STORE;
                    writer.entries[writer.count].crc = 0;
                    writer.entries[writer.count].st_mode = results[ii].st_mode;
                    writer.entries[writer.count].mtime = results[ii].mtime;
                    writer.offsets[writer.count]=off;
                    writer.is_zip64[writer.count]=need64;
                    if (!writer.entries[writer.count].filename) { ok=false; break; }
                    writer.count++;
                  } else {
                    long off = ftell(writer.file);
                    if (off<0) { ok=false; break; }
                    uint16_t ddate, dtime;
                    to_dos_time((time_t)results[ii].mtime, &ddate, &dtime);
                    bool need64 = (results[ii].data_len > ZIP64_LIMIT_32 || results[ii].comp_len > ZIP64_LIMIT_32 || (uint64_t)off > ZIP64_LIMIT_32);
                    if (!write_local_entry(&writer, items[ii].arcname, results[ii].comp?results[ii].comp:(unsigned char*)"", results[ii].comp_len, results[ii].data_len, results[ii].crc, results[ii].method, need64, off, ddate, dtime)) { ok=false; break; }
                    writer.entries[writer.count].filename = strdup(items[ii].arcname);
                    writer.entries[writer.count].comp_data = results[ii].comp;
                    writer.entries[writer.count].data_len = results[ii].data_len;
                    writer.entries[writer.count].comp_len = results[ii].comp_len;
                    writer.entries[writer.count].method = results[ii].method;
                    writer.entries[writer.count].crc = results[ii].crc;
                    writer.entries[writer.count].st_mode = results[ii].st_mode;
                    writer.entries[writer.count].mtime = results[ii].mtime;
                    writer.offsets[writer.count]=off;
                    writer.is_zip64[writer.count]=need64;
                    if (!writer.entries[writer.count].filename) { ok=false; break; }
                    writer.count++;
                    results[ii].comp = NULL;
                  }
                }
              }
              for (size_t ii=0; ii<nitems; ii++) if (results[ii].comp) free(results[ii].comp);
            }
          free(results);
        }
    }
  else
    {
      for (size_t ii=0;ii<nitems;ii++)
        {
          int pct=(int)((ii+1)*100/nitems); fprintf(stderr,"%d%%\r",pct); fflush(stderr);
          struct stat dst;
          if (lstat(items[ii].fullpath,&dst)==0 && S_ISDIR(dst.st_mode))
            {
              uint16_t ddate, dtime;
              long off = ftell(writer.file);
              if (off<0) { ok=false; break; }
              to_dos_time(dst.st_mtime, &ddate, &dtime);
              bool need64 = ((uint64_t)off > ZIP64_LIMIT_32);
              if (!write_local_entry(&writer, items[ii].arcname, (unsigned char*)"", 0, 0, 0, COMP_METHOD_STORE, need64, off, ddate, dtime)) { ok=false; break; }
              if (!ensure_capacity(&writer)) { ok=false; break; }
              writer.entries[writer.count].filename = strdup(items[ii].arcname);
              writer.entries[writer.count].comp_data = NULL;
              writer.entries[writer.count].data_len = 0;
              writer.entries[writer.count].comp_len = 0;
              writer.entries[writer.count].method = COMP_METHOD_STORE;
              writer.entries[writer.count].crc = 0;
              writer.entries[writer.count].st_mode = dst.st_mode & 07777;
              writer.entries[writer.count].mtime = (long long)dst.st_mtime;
              writer.offsets[writer.count]=off;
              writer.is_zip64[writer.count]=need64;
              if (!writer.entries[writer.count].filename) { ok=false; break; }
              writer.count++;
            }
          else if (!zip_writer_add_path(&writer, items[ii].arcname, items[ii].fullpath)) { ok=false; break; }
        }
    }
  for (i=0;i<nitems;i++) { free(items[i].arcname); free(items[i].fullpath); }
  free(items);
  if (ok)
    {
      for (i=0;i<writer.count;i++) total_comp+=writer.entries[i].comp_len;
      ok=zip_writer_close(&writer);
    }
  else
    {
      /* Ensure writer file is closed and tmp removed */
      if (writer.file) { fclose(writer.file); writer.file=NULL; }
      if (writer.tmp_created && writer.tmp_path) unlink(writer.tmp_path);
      writer.closed=true;
    }
  if (ok)
    {
      const char *best=competitor_best_overall_desc();
      if (!best||!*best) best=competitor_last_desc();
      if (!best||!*best) best="Deflate";
      fprintf(stderr,"\r%s %zu bytes\n",best,total_comp); fflush(stderr);
    }
  else { fprintf(stderr,"\n"); fflush(stderr); if (writer.tmp_created && writer.tmp_path) unlink(writer.tmp_path); }
  zip_writer_free(&writer);
  return ok;
}
