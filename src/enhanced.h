#ifndef ENHANCED_H
#define ENHANCED_H

#include <stddef.h>
#include <stdbool.h>

/* Stage 2 second engine (ECT/zenzop ideas, pure C, DEFLATE-only).
   No globals, no child processes, emits raw DEFLATE (method 8) only. */

/* Iteration budget by size class (mirrors Stage 1 fence). */
int enhanced_budget_for_size (size_t len);

/* Best raw DEFLATE found by the second engine.
   Returns true and mallocs *out on success (caller frees).
   desc receives the winning sub-strategy name (may be NULL). */
bool enhanced_compress (const unsigned char *data,
                        size_t len,
                        unsigned char **out,
                        size_t *out_len,
                        char *desc,
                        size_t desc_sz);

#endif /* ENHANCED_H */
