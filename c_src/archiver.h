#ifndef ARCHIVER_H
#define ARCHIVER_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

#define ZIP_COMPAT_WIDE 0
#define ZIP_COMPAT_MAX  1

typedef struct {
    char *filename;
    unsigned char *data;
    size_t data_len;
    unsigned char *comp;
    size_t comp_len;
    int method;
    unsigned int crc;
} zip_entry_t;

typedef struct {
    FILE *f;
    int compat;
    zip_entry_t *entries;
    size_t count;
    size_t cap;
    long *offsets;
    bool *is_zip64;
    bool closed;
} zip_writer_t;

bool zip_writer_init(zip_writer_t *w, const char *path, int compat);
bool zip_writer_add_file(zip_writer_t *w, const char *arcname, const unsigned char *data, size_t len, int level);
bool zip_writer_add_path(zip_writer_t *w, const char *arcname, const char *fullpath, int level);
bool zip_writer_close(zip_writer_t *w);
void zip_writer_free(zip_writer_t *w);

bool create_zip(const char *archive, char **files, size_t nfiles, int level, int compat);

#endif
