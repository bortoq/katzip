#ifndef POLICY_H
#define POLICY_H

#include <stddef.h>
#include <stdbool.h>

#define SMALL_FILE_THRESHOLD (4096)
#define ZOPFLI_SIZE_LIMIT (32*1024*1024)
#define ENTROPY_THRESHOLD (7.85)

bool is_incompressible_by_ext(const char *filename);
double calc_entropy(const unsigned char *data, size_t len);
bool is_high_entropy(const unsigned char *data, size_t len);
bool should_use_store_only(const char *filename, const unsigned char *data, size_t len);
bool should_skip_heavy(const char *filename, const unsigned char *data, size_t len, int level);
bool zopfli_allowed(const unsigned char *data, size_t len, int level);

#endif
