#ifndef COMPETITOR_H
#define COMPETITOR_H

#include <stddef.h>
#include <stdbool.h>

/* ZIP compression methods we emit.
   Only Store and Deflate are used — 100% compatible. */
#define COMP_METHOD_STORE   0
#define COMP_METHOD_DEFLATE 8

/* Choose and compress a buffer with the densest DEFLATE.
   On success returns true and allocates *out (caller must free).
   *method is set to COMP_METHOD_STORE or COMP_METHOD_DEFLATE. */
bool competitor_compress (const unsigned char *data,
                          size_t len,
                          const char *filename,
                          unsigned char **out,
                          size_t *out_len,
                          int *method);

/* Human-readable description of the winning encoder for the last
   call to competitor_compress. Valid until next call. */
const char *competitor_last_desc (void);

/* Description of the best overall saving seen so far. */
const char *competitor_best_overall_desc (void);
size_t competitor_best_overall_saved (void);

/* Verify a compressed buffer by decompressing it. */
bool competitor_decompress (const unsigned char *comp,
                            size_t comp_len,
                            int method,
                            unsigned char *out,
                            size_t out_len);

#endif /* COMPETITOR_H */
