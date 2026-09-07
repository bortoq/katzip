#ifndef COMPETITOR_H
#define COMPETITOR_H

#include <stddef.h>
#include <stdbool.h>

#define METHOD_STORE   0
#define METHOD_DEFLATE 8
#define METHOD_BZIP2   12
#define METHOD_LZMA    14
#define METHOD_ZSTD    93

bool has_zopfli(void);
bool has_zstd(void);
bool has_bzip2(void);
bool has_lzma(void);

// возвращает true если выбрал кандидата. *out нужно free().
// method - METHOD_*
bool compress_buffer(const unsigned char *data, size_t len,
                     const char *filename, int level, int compat,
                     unsigned char **out, size_t *out_len, int *method);

bool decompress_buffer(const unsigned char *comp, size_t comp_len,
                       int method, unsigned char *out, size_t out_len);

#endif
