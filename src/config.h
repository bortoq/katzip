#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdio.h>

/* Runtime tuning for the compression contest (katzip.ini).
   Missing file or keys -> built-in defaults (= previous behavior).
   Path: $KATZIP_INI if set, else ./katzip.ini in the working directory. */

#define KATZIP_MAX_SPLITMAX 8
#define KATZIP_MAX_LAST 4
#define KATZIP_MAX_SKIP_EXT 40
#define KATZIP_EXT_LEN 16

typedef struct
{
  /* [policy] */
  size_t small_file_limit;   /* below: heavy encoders off */
  size_t zopfli_size_limit;  /* above: zopfli off */
  double entropy_limit;      /* bits/byte above: store */
  size_t entropy_sample;     /* bytes sampled for entropy */
  int store_high_entropy;    /* bool */
  char skip_ext[KATZIP_MAX_SKIP_EXT][KATZIP_EXT_LEN];
  int n_skip_ext;

  /* [zlib] */
  int zlib_enabled;
  int zlib_min_level;
  int zlib_max_level;
  int zlib_s_default;
  int zlib_s_filtered;
  int zlib_s_huffman;
  int zlib_s_rle;
  int zlib_s_fixed;
  size_t zlib_full_grid_max; /* above: only max level tried */
  int zlib_extra_retry;      /* bool: extra level-6 pass for tiny files */
  size_t zlib_extra_retry_max;

  /* [libdeflate] */
  int libdeflate_enabled;
  int libdeflate_min_level;
  int libdeflate_max_level;

  /* [zopfli] */
  int zopfli_enabled;
  size_t zopfli_tiny_max;
  int zopfli_iter_tiny;
  size_t zopfli_small_max;
  int zopfli_iter_small;
  size_t zopfli_med_max;
  int zopfli_iter_medium;
  int zopfli_iter_large;
  int zopfli_splitmax[KATZIP_MAX_SPLITMAX];
  int zopfli_n_splitmax;
  int zopfli_last[KATZIP_MAX_LAST];
  int zopfli_n_last;
  size_t zopfli_nosplit_max; /* extra no-split trial when len <= this (0: never) */

  /* [enhanced] */
  int enh_enabled;
  int enh_fixed;
  int enh_nosplit;
  int enh_split5;
  int enh_split5_max;
  int enh_singleblock;
  size_t enh_singleblock_max;
  int enh_custom_iter_cap;
  int enh_split15;
  int enh_merge_blocks;
  int enh_kzip_split;
  int enh_recode_iters;
  int threads; /* 0=auto, 1=single, N=max */
} katzip_config_t;

/* Fill with built-in defaults. Guarded: NULL is a no-op. */
void config_defaults (katzip_config_t *cfg);

/* Loaded-once read-only config (defaults + file overrides).
   Never returns NULL. Thread-unsafe first call only.
   Search order: $KATZIP_INI, ./katzip.ini (working dir),
   <binary-dir>/katzip.ini. */
const katzip_config_t *config_get (void);

/* Path of the loaded file, or NULL when built-in defaults are used. */
const char *config_source (void);


/* Drop cached config so the next config_get() reloads (tests). */
void config_reset (void);

#endif /* CONFIG_H */
