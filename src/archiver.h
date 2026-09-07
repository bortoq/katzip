#ifndef ARCHIVER_H
#define ARCHIVER_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

/* Writer that creates a ZIP file with Store(0) and Deflate(8) only. */
typedef struct
{
  char *filename;
  unsigned char *comp_data;
  size_t data_len;
  size_t comp_len;
  int method;
  unsigned int crc;
} zip_entry_t;

typedef struct
{
  FILE *file;
  zip_entry_t *entries;
  size_t count;
  size_t capacity;
  long *offsets;
  bool *is_zip64;
  bool closed;
} zip_writer_t;

/* Start a new archive at PATH. */
bool zip_writer_open (zip_writer_t *writer,
                      const char *path);

/* Add one file with raw DATA of LEN, stored as ARCNAME.
   Uses the densest DEFLATE available. */
bool zip_writer_add_file (zip_writer_t *writer,
                          const char *arcname,
                          const unsigned char *data,
                          size_t len);

/* Add a file from disk: read FULLPATH and store as ARCNAME. */
bool zip_writer_add_path (zip_writer_t *writer,
                          const char *arcname,
                          const char *fullpath);

/* Finalize central directory and close the file. */
bool zip_writer_close (zip_writer_t *writer);

/* Free memory and close file (safe to call after close). */
void zip_writer_free (zip_writer_t *writer);

/* High-level helper: create ARCHIVE from FILES (paths on disk). */
bool create_zip_archive (const char *archive,
                         char **files,
                         size_t nfiles);

#endif /* ARCHIVER_H */
