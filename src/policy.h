#ifndef POLICY_H
#define POLICY_H

#include <stddef.h>
#include <stdbool.h>

/* Threshold for skipping heavy encoders on tiny files. */
#define POLICY_SMALL_FILE_LIMIT (4096)

/* Above this size Zopfli is disabled to bound time and memory. */
#define POLICY_ZOPFLI_SIZE_LIMIT (32 * 1024 * 1024)

/* Shannon entropy (bits per byte) above which data is treated as already
   compressed or encrypted. */
#define POLICY_ENTROPY_LIMIT (7.85)

bool policy_is_incompressible_extension (const char *filename);

double policy_entropy (const unsigned char *data,
                       size_t len);

bool policy_is_high_entropy (const unsigned char *data,
                             size_t len);

bool policy_should_store_only (const char *filename,
                               const unsigned char *data,
                               size_t len);

bool policy_zopfli_allowed (const unsigned char *data,
                            size_t len);

#endif /* POLICY_H */
