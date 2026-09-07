#include "archiver.h"
#include "competitor.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
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

/* Little-endian writers. */

static void
write_le16 (FILE *f, uint16_t v)
{
  fputc (v & 0xff, f);
  fputc ((v >> 8) & 0xff, f);
}

static void
write_le32 (FILE *f, uint32_t v)
{
  for (int i = 0; i < 4; i++)
    {
      fputc (v & 0xff, f);
      v >>= 8;
    }
}

static void
write_le64 (FILE *f, uint64_t v)
{
  for (int i = 0; i < 8; i++)
    {
      fputc (v & 0xff, f);
      v >>= 8;
    }
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
  if (year < 1980)
    year = 1980;
  if (year > 2107)
    year = 2107;

  *dosdate = (uint16_t) (((year - 1980) << 9)
                         | ((tm->tm_mon + 1) << 5)
                         | tm->tm_mday);
  *dostime = (uint16_t) ((tm->tm_hour << 11)
                         | (tm->tm_min << 5)
                         | (tm->tm_sec / 2));
}

/* Normalize an archive name: backslashes to slashes, strip leading /. */
static char *
normalize_name (const char *arcname)
{
  size_t len;
  char *out;
  size_t j;
  size_t start;

  if (!arcname)
    return NULL;

  len = strlen (arcname);
  out = malloc (len + 1);
  if (!out)
    return NULL;

  for (j = 0; j < len; j++)
    {
      char c = arcname[j];
      out[j] = (c == '\\' ? '/' : c);
    }
  out[len] = '\0';

  start = 0;
  while (out[start] == '/')
    start++;

  if (start)
    memmove (out, out + start, strlen (out + start) + 1);

  if (out[0] == '\0')
    {
      free (out);
      return NULL;
    }

  return out;
}

/* ------------------------------------------------------------------ */
/* Writer lifecycle */

bool
zip_writer_open (zip_writer_t *writer, const char *path)
{
  if (!writer || !path)
    return false;

  memset (writer, 0, sizeof *writer);

  writer->file = fopen (path, "wb");
  if (!writer->file)
    return false;

  writer->capacity = 16;
  writer->entries = calloc (writer->capacity, sizeof *writer->entries);
  writer->offsets = calloc (writer->capacity, sizeof *writer->offsets);
  writer->is_zip64 = calloc (writer->capacity, sizeof *writer->is_zip64);

  if (!writer->entries || !writer->offsets || !writer->is_zip64)
    {
      if (writer->file)
        fclose (writer->file);
      free (writer->entries);
      free (writer->offsets);
      free (writer->is_zip64);
      return false;
    }

  writer->count = 0;
  writer->closed = false;
  return true;
}

static bool
ensure_capacity (zip_writer_t *writer)
{
  if (writer->count < writer->capacity)
    return true;

  {
    size_t ncap = writer->capacity * 2;
    zip_entry_t *ne;
    long *no;
    bool *nz;

    ne = realloc (writer->entries, ncap * sizeof *ne);
    no = realloc (writer->offsets, ncap * sizeof *no);
    nz = realloc (writer->is_zip64, ncap * sizeof *nz);

    if (!ne || !no || !nz)
      return false;

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

/* Write a local file header and payload. */
static bool
write_local_entry (zip_writer_t *writer,
                   const char *norm_name,
                   const unsigned char *comp,
                   size_t comp_len,
                   size_t plain_len,
                   unsigned int crc,
                   int method,
                   bool need_zip64,
                   long offset)
{
  uint16_t flag;
  uint16_t dosdate;
  uint16_t dostime;
  uint16_t version;
  uint16_t name_len;
  uint16_t extra_len;
  (void) offset;

  flag = needs_utf8 (norm_name) ? FLAG_UTF8 : 0;
  to_dos_time (time (NULL), &dosdate, &dostime);

  version = ZIP_VERSION_DEFAULT;
  if (need_zip64)
    version = ZIP_VERSION_ZIP64;

  name_len = (uint16_t) strlen (norm_name);
  extra_len = need_zip64 ? 20 : 0;

  write_le32 (writer->file, SIG_LOCAL_FILE);
  write_le16 (writer->file, version);
  write_le16 (writer->file, flag);
  write_le16 (writer->file, (uint16_t) method);
  write_le16 (writer->file, dostime);
  write_le16 (writer->file, dosdate);
  write_le32 (writer->file, crc);

  if (need_zip64)
    {
      write_le32 (writer->file, ZIP64_LIMIT_32);
      write_le32 (writer->file, ZIP64_LIMIT_32);
    }
  else
    {
      write_le32 (writer->file, (uint32_t) comp_len);
      write_le32 (writer->file, (uint32_t) plain_len);
    }

  write_le16 (writer->file, name_len);
  write_le16 (writer->file, extra_len);

  if (fwrite (norm_name, 1, name_len, writer->file) != name_len)
    return false;

  if (need_zip64)
    {
      write_le16 (writer->file, 0x0001);
      write_le16 (writer->file, 16);
      write_le64 (writer->file, plain_len);
      write_le64 (writer->file, comp_len);
    }

  if (comp_len && fwrite (comp, 1, comp_len, writer->file) != comp_len)
    return false;

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

  if (!writer || writer->closed || !arcname || !data)
    return false;

  norm = normalize_name (arcname);
  if (!norm)
    return false;

  /* Directory entries end with '/', store them empty. */
  if (norm[strlen (norm) - 1] == '/')
    {
      if (len != 0)
        {
          free (norm);
          return false;
        }
      crc = 0;
      comp = NULL;
      comp_len = 0;
      method = COMP_METHOD_STORE;
    }
  else
    {
      if (len)
        crc = crc32 (0L, data, (uInt) len);

      if (!competitor_compress (data, len, norm, &comp, &comp_len, &method))
        {
          free (norm);
          return false;
        }
    }

  offset = ftell (writer->file);
  if (offset < 0)
    {
      free (norm);
      free (comp);
      return false;
    }

  need_zip64 = (len > ZIP64_LIMIT_32
                || comp_len > ZIP64_LIMIT_32
                || (uint64_t) offset > ZIP64_LIMIT_32);

  ok = write_local_entry (writer, norm, comp ? comp : (unsigned char *) "",
                          comp_len, len, crc, method, need_zip64, offset);
  if (!ok)
    {
      free (norm);
      free (comp);
      return false;
    }

  if (!ensure_capacity (writer))
    {
      free (norm);
      free (comp);
      return false;
    }

  writer->entries[writer->count].filename = norm;
  writer->entries[writer->count].comp_data = comp;
  writer->entries[writer->count].data_len = len;
  writer->entries[writer->count].comp_len = comp_len;
  writer->entries[writer->count].method = method;
  writer->entries[writer->count].crc = crc;
  writer->offsets[writer->count] = offset;
  writer->is_zip64[writer->count] = need_zip64;
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
  bool ok;

  if (!writer || !arcname || !fullpath)
    return false;
  if (stat (fullpath, &st) != 0)
    return false;
  if (S_ISDIR (st.st_mode))
    return false;

  in = fopen (fullpath, "rb");
  if (!in)
    return false;

  len = (size_t) st.st_size;
  data = malloc (len ? len : 1);
  if (!data)
    {
      fclose (in);
      return false;
    }

  if (len && fread (data, 1, len, in) != len)
    {
      free (data);
      fclose (in);
      return false;
    }

  fclose (in);
  ok = zip_writer_add_file (writer, arcname, data, len);
  free (data);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Central directory */

static void
write_central_entry (zip_writer_t *writer, size_t idx,
                     size_t *central_size)
{
  zip_entry_t *e = &writer->entries[idx];
  long off = writer->offsets[idx];
  bool z64 = writer->is_zip64[idx];
  uint16_t flag = needs_utf8 (e->filename) ? FLAG_UTF8 : 0;
  uint16_t dosdate, dostime;
  uint16_t version = ZIP_VERSION_DEFAULT;
  uint16_t name_len = (uint16_t) strlen (e->filename);
  uint16_t extra_len = z64 ? 28 : 0;

  to_dos_time (time (NULL), &dosdate, &dostime);

  if (z64)
    version = ZIP_VERSION_ZIP64;

  write_le32 (writer->file, SIG_CENTRAL_DIR);
  write_le16 (writer->file, (3 << 8) | 63);
  write_le16 (writer->file, version);
  write_le16 (writer->file, flag);
  write_le16 (writer->file, (uint16_t) e->method);
  write_le16 (writer->file, dostime);
  write_le16 (writer->file, dosdate);
  write_le32 (writer->file, e->crc);

  if (z64)
    {
      write_le32 (writer->file, ZIP64_LIMIT_32);
      write_le32 (writer->file, ZIP64_LIMIT_32);
    }
  else
    {
      write_le32 (writer->file, (uint32_t) e->comp_len);
      write_le32 (writer->file, (uint32_t) e->data_len);
    }

  write_le16 (writer->file, name_len);
  write_le16 (writer->file, extra_len);
  write_le16 (writer->file, 0);
  write_le16 (writer->file, 0);
  write_le16 (writer->file, 0);
  write_le32 (writer->file, 0);

  if (z64)
    write_le32 (writer->file, ZIP64_LIMIT_32);
  else
    write_le32 (writer->file, (uint32_t) off);

  *central_size += 46;
  fwrite (e->filename, 1, name_len, writer->file);
  *central_size += name_len;

  if (z64)
    {
      write_le16 (writer->file, 0x0001);
      write_le16 (writer->file, 24);
      write_le64 (writer->file, e->data_len);
      write_le64 (writer->file, e->comp_len);
      write_le64 (writer->file, off);
      *central_size += 28;
    }
}

bool
zip_writer_close (zip_writer_t *writer)
{
  long central_offset;
  size_t central_size = 0;
  size_t i;
  bool need_zip64;

  if (!writer || writer->closed)
    return false;

  central_offset = ftell (writer->file);
  if (central_offset < 0)
    return false;

  for (i = 0; i < writer->count; i++)
    write_central_entry (writer, i, &central_size);

  need_zip64 = (central_offset > (long) ZIP64_LIMIT_32
                || central_size > ZIP64_LIMIT_32
                || writer->count > 0xFFFF);

  if (need_zip64)
    {
      long zip64_off = ftell (writer->file);

      write_le32 (writer->file, SIG_ZIP64_END);
      write_le64 (writer->file, 44);
      write_le16 (writer->file, 63);
      write_le16 (writer->file, ZIP_VERSION_ZIP64);
      write_le32 (writer->file, 0);
      write_le32 (writer->file, 0);
      write_le64 (writer->file, writer->count);
      write_le64 (writer->file, writer->count);
      write_le64 (writer->file, central_size);
      write_le64 (writer->file, central_offset);

      write_le32 (writer->file, SIG_ZIP64_LOCATOR);
      write_le32 (writer->file, 0);
      write_le64 (writer->file, zip64_off);
      write_le32 (writer->file, 1);
    }

  {
    uint16_t n = need_zip64 ? 0xFFFF : (uint16_t) writer->count;
    uint32_t sz = need_zip64 ? ZIP64_LIMIT_32 : (uint32_t) central_size;
    uint32_t off = need_zip64 ? ZIP64_LIMIT_32 : (uint32_t) central_offset;

    write_le32 (writer->file, SIG_END_OF_CENTRAL);
    write_le16 (writer->file, 0);
    write_le16 (writer->file, 0);
    write_le16 (writer->file, n);
    write_le16 (writer->file, n);
    write_le32 (writer->file, sz);
    write_le32 (writer->file, off);
    write_le16 (writer->file, 0);
  }

  writer->closed = true;
  return true;
}

void
zip_writer_free (zip_writer_t *writer)
{
  size_t i;

  if (!writer)
    return;

  for (i = 0; i < writer->count; i++)
    {
      free (writer->entries[i].filename);
      free (writer->entries[i].comp_data);
    }

  free (writer->entries);
  free (writer->offsets);
  free (writer->is_zip64);

  if (writer->file)
    fclose (writer->file);

  memset (writer, 0, sizeof *writer);
}

/* ------------------------------------------------------------------ */
/* High-level: collect all files first for accurate progress */

typedef struct
{
  char *arcname;
  char *fullpath;
} file_item_t;

static bool
collect_one (const char *path, const char *base_parent,
             file_item_t **items, size_t *count, size_t *cap)
{
  struct stat st;

  if (stat (path, &st) != 0)
    return false;

  if (S_ISDIR (st.st_mode))
    {
      DIR *dir = opendir (path);
      struct dirent *ent;

      if (!dir)
        return false;

      while ((ent = readdir (dir)) != NULL)
        {
          char full[4096];

          if (strcmp (ent->d_name, ".") == 0)
            continue;
          if (strcmp (ent->d_name, "..") == 0)
            continue;

          snprintf (full, sizeof full, "%s/%s", path, ent->d_name);
          if (!collect_one (full, base_parent, items, count, cap))
            {
              closedir (dir);
              return false;
            }
        }

      closedir (dir);
      return true;
    }
  else
    {
      char arc[4096];
      const char *rel;
      size_t blen = strlen (base_parent);
      char *a_dup;
      char *p_dup;

      if (blen == 0 || strcmp (base_parent, ".") == 0)
        rel = path;
      else if (strncmp (path, base_parent, blen) == 0
               && path[blen] == '/')
        rel = path + blen + 1;
      else
        rel = path;

      while (rel[0] == '.' && rel[1] == '/')
        rel += 2;

      strncpy (arc, rel, sizeof arc - 1);
      arc[sizeof arc - 1] = '\0';

      for (char *p = arc; *p; p++)
        if (*p == '\\')
          *p = '/';

      if (*count >= *cap)
        {
          size_t ncap = *cap ? *cap * 2 : 16;
          file_item_t *next = realloc (*items, ncap * sizeof *next);
          if (!next)
            return false;
          *items = next;
          *cap = ncap;
        }

      a_dup = strdup (arc);
      p_dup = strdup (path);
      if (!a_dup || !p_dup)
        {
          free (a_dup);
          free (p_dup);
          return false;
        }

      (*items)[*count].arcname = a_dup;
      (*items)[*count].fullpath = p_dup;
      (*count)++;
      return true;
    }
}

bool
create_zip_archive (const char *archive,
                    char **files,
                    size_t nfiles)
{
  zip_writer_t writer;
  file_item_t *items = NULL;
  size_t nitems = 0;
  size_t cap = 0;
  size_t i;
  bool ok = true;
  size_t total_comp = 0;

  if (!archive || !files || nfiles == 0)
    return false;

  /* First pass: collect every regular file. */
  for (i = 0; i < nfiles; i++)
    {
      const char *p = files[i];
      struct stat st;

      if (!p)
        continue;
      if (stat (p, &st) != 0)
        {
          ok = false;
          break;
        }

      if (S_ISDIR (st.st_mode))
        {
          char abspath[4096];
          char parent[4096];
          char *slash;

          if (!realpath (p, abspath))
            {
              ok = false;
              break;
            }

          slash = strrchr (abspath, '/');
          if (slash)
            {
              size_t len = slash - abspath;
              if (len == 0)
                strcpy (parent, "/");
              else
                {
                  strncpy (parent, abspath, len);
                  parent[len] = '\0';
                }
            }
          else
            strcpy (parent, ".");

          if (!collect_one (p, parent, &items, &nitems, &cap))
            {
              ok = false;
              break;
            }
        }
      else
        {
          const char *base = strrchr (p, '/');
          const char *arc = base ? base + 1 : p;
          char *a_dup = strdup (arc);
          char *p_dup = strdup (p);

          if (!a_dup || !p_dup)
            {
              free (a_dup);
              free (p_dup);
              ok = false;
              break;
            }

          if (nitems >= cap)
            {
              size_t ncap = cap ? cap * 2 : 16;
              file_item_t *next = realloc (items, ncap * sizeof *next);
              if (!next)
                {
                  free (a_dup);
                  free (p_dup);
                  ok = false;
                  break;
                }
              items = next;
              cap = ncap;
            }

          items[nitems].arcname = a_dup;
          items[nitems].fullpath = p_dup;
          nitems++;
        }
    }

  if (!ok)
    {
      for (i = 0; i < nitems; i++)
        {
          free (items[i].arcname);
          free (items[i].fullpath);
        }
      free (items);
      return false;
    }

  if (nitems == 0)
    {
      free (items);
      if (!zip_writer_open (&writer, archive))
        return false;
      ok = zip_writer_close (&writer);
      zip_writer_free (&writer);
      if (!ok)
        unlink (archive);
      return ok;
    }

  if (!zip_writer_open (&writer, archive))
    {
      for (i = 0; i < nitems; i++)
        {
          free (items[i].arcname);
          free (items[i].fullpath);
        }
      free (items);
      return false;
    }

  /* Second pass: compress each file with progress. */
  for (i = 0; i < nitems; i++)
    {
      int pct = (int) ((i + 1) * 100 / nitems);

      /* Show overall progress on the same line. */
      fprintf (stderr, "%d%%\r", pct);
      fflush (stderr);

      if (!zip_writer_add_path (&writer,
                                items[i].arcname,
                                items[i].fullpath))
        {
          ok = false;
          break;
        }
    }

  /* Free the collected list (writer keeps its own copies). */
  for (i = 0; i < nitems; i++)
    {
      free (items[i].arcname);
      free (items[i].fullpath);
    }
  free (items);

  if (ok)
    {
      /* Total compressed data without ZIP overhead. */
      for (i = 0; i < writer.count; i++)
        total_comp += writer.entries[i].comp_len;

      ok = zip_writer_close (&writer);
    }

  /* Final line overwrites the progress line via \r. */
  if (ok)
    {
      const char *best = competitor_best_overall_desc ();
      if (!best || !*best)
        best = competitor_last_desc ();
      if (!best || !*best)
        best = "Deflate";

      fprintf (stderr, "\r%s %zu bytes\n", best, total_comp);
      fflush (stderr);
    }
  else
    {
      fprintf (stderr, "\n");
      fflush (stderr);
    }

  zip_writer_free (&writer);

  if (!ok)
    unlink (archive);

  return ok;
}
