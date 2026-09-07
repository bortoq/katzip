#include "competitor.h"
#include "policy.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef HAVE_LIBDEFLATE
#include <libdeflate.h>
#endif

#include "zopfli/zopfli.h"

/* Raw DEFLATE window in zlib: negative means no zlib header. */
static const int RAW_WINDOW_BITS = -15;
static const int ZLIB_MEM_LEVEL = 9;

/* Stage 1: full Zopfli grid. Iteration budget per size class.
   Small files can afford deep search (hours per file at 1000 iters
   is still bounded because file is small). */
static const int ZOPFLI_ITER_TINY = 1000;  /* <= 64 KiB */
static const int ZOPFLI_ITER_SMALL = 200;  /* <= 256 KiB */
static const int ZOPFLI_ITER_MEDIUM = 60;  /* <= 1 MiB */
static const int ZOPFLI_ITER_LARGE = 15;   /* > 1 MiB */

/* Minimum size for extra strategy pass on small files. */
static const size_t SMALL_FILE_EXTRA_PASS = 64 * 1024;

/* ------------------------------------------------------------------ */
/* Small helpers                                                     */

static bool
verify_deflate (const unsigned char *plain, size_t plain_len,
                const unsigned char *comp, size_t comp_len)
{
  unsigned char *tmp;
  z_stream stream;
  int ret;
  bool ok;

  tmp = malloc (plain_len ? plain_len : 1);
  if (!tmp)
    return false;

  memset (&stream, 0, sizeof stream);
  stream.next_in = (unsigned char *) comp;
  stream.avail_in = (unsigned int) comp_len;
  stream.next_out = tmp;
  stream.avail_out = (unsigned int) plain_len;

  if (inflateInit2 (&stream, RAW_WINDOW_BITS) != Z_OK)
    {
      free (tmp);
      return false;
    }

  ret = inflate (&stream, Z_FINISH);
  ok = (ret == Z_STREAM_END
        && stream.total_out == plain_len
        && memcmp (tmp, plain, plain_len) == 0);

  inflateEnd (&stream);
  free (tmp);
  return ok;
}

static bool
deflate_with_zlib (const unsigned char *in, size_t in_len,
                   int level, int strategy,
                   unsigned char **out, size_t *out_len)
{
  z_stream stream;
  size_t bound;
  unsigned char *buf;
  int ret;

  if (!in || !out || !out_len)
    return false;
  if (level < 0 || level > 9)
    return false;

  bound = in_len + in_len / 1000 + 128;
  if (bound < 128)
    bound = 128;

  buf = malloc (bound);
  if (!buf)
    return false;

  memset (&stream, 0, sizeof stream);
  stream.next_in = (unsigned char *) in;
  stream.avail_in = (unsigned int) in_len;
  stream.next_out = buf;
  stream.avail_out = (unsigned int) bound;

  ret = deflateInit2 (&stream, level, Z_DEFLATED,
                      RAW_WINDOW_BITS, ZLIB_MEM_LEVEL, strategy);
  if (ret != Z_OK)
    {
      free (buf);
      return false;
    }

  ret = deflate (&stream, Z_FINISH);
  if (ret != Z_STREAM_END)
    {
      deflateEnd (&stream);
      free (buf);
      return false;
    }

  *out_len = stream.total_out;
  *out = buf;
  deflateEnd (&stream);

  if (!verify_deflate (in, in_len, buf, *out_len))
    {
      free (buf);
      return false;
    }

  return true;
}

#ifdef HAVE_LIBDEFLATE
static bool
deflate_with_libdeflate (const unsigned char *in, size_t in_len,
                         int level,
                         unsigned char **out, size_t *out_len)
{
  struct libdeflate_compressor *comp;
  size_t bound;
  unsigned char *buf;
  size_t sz;

  if (!in || !out || !out_len)
    return false;

  comp = libdeflate_alloc_compressor (level);
  if (!comp)
    return false;

  bound = libdeflate_deflate_compress_bound (comp, in_len);
  buf = malloc (bound);
  if (!buf)
    {
      libdeflate_free_compressor (comp);
      return false;
    }

  sz = libdeflate_deflate_compress (comp, in, in_len, buf, bound);
  libdeflate_free_compressor (comp);

  if (sz == 0)
    {
      free (buf);
      return false;
    }

  *out = buf;
  *out_len = sz;

  if (!verify_deflate (in, in_len, buf, sz))
    {
      free (buf);
      return false;
    }

  return true;
}
#endif

/* Full Zopfli call with all knobs exposed.
   split_max == 0 means unlimited blocks. */
static bool
deflate_with_zopfli (const unsigned char *in, size_t in_len,
                     int iterations, int split_max,
                     int do_split, int split_last,
                     unsigned char **out, size_t *out_len)
{
  ZopfliOptions opts;
  unsigned char *zbuf = NULL;
  size_t zlen = 0;

  if (!in || !out || !out_len)
    return false;

  if (in_len == 0)
    {
      unsigned char *buf = malloc (2);
      if (!buf)
        return false;
      buf[0] = 0x03;
      buf[1] = 0x00;
      *out = buf;
      *out_len = 2;
      return true;
    }

  ZopfliInitOptions (&opts);
  opts.numiterations = iterations;
  opts.blocksplitting = do_split;
  opts.blocksplittinglast = split_last;
  opts.blocksplittingmax = split_max;

  /* Zopfli produces raw DEFLATE when asked for DEFLATE format. */
  ZopfliCompress (&opts, ZOPFLI_FORMAT_DEFLATE, in, in_len, &zbuf, &zlen);

  if (!zbuf || zlen == 0)
    {
      if (zbuf)
        free (zbuf);
      return false;
    }

  if (!verify_deflate (in, in_len, zbuf, zlen))
    {
      free (zbuf);
      return false;
    }

  *out = zbuf;
  *out_len = zlen;
  return true;
}

/* Keep the smallest buffer seen so far. */
static void
consider_candidate (unsigned char *cand, size_t cand_len, int cand_method,
                    unsigned char **best, size_t *best_len, int *best_method)
{
  if (!cand)
    return;

  if (cand_len < *best_len)
    {
      if (*best)
        free (*best);
      *best = cand;
      *best_len = cand_len;
      *best_method = cand_method;
    }
  else
    free (cand);
}

/* ------------------------------------------------------------------ */
/* Exhaustive DEFLATE search */

static void
try_zlib_exhaustive (const unsigned char *data, size_t len,
                     unsigned char **best, size_t *best_len,
                     int *best_method)
{
  const int strategies[] =
    { Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY,
      3 /* Z_RLE */, Z_FIXED };
  size_t n_strat = sizeof strategies / sizeof strategies[0];
  size_t i;

  /* Large files: only level 9 is worth the time. */
  if (len > 1024 * 1024)
    {
      for (i = 0; i < n_strat; i++)
        {
          unsigned char *c = NULL;
          size_t cl = 0;
          if (deflate_with_zlib (data, len, 9, strategies[i], &c, &cl))
            consider_candidate (c, cl, COMP_METHOD_DEFLATE,
                                best, best_len, best_method);
        }
      return;
    }

  /* Small and medium files: try all levels and strategies.
     RLE and FIXED only matter at max level, so skip them otherwise
     to save time. */
  for (int lvl = 1; lvl <= 9; lvl++)
    for (i = 0; i < n_strat; i++)
      {
        if ((strategies[i] == 3 || strategies[i] == Z_FIXED) && lvl != 9)
          continue;

        {
          unsigned char *c = NULL;
          size_t cl = 0;
          if (deflate_with_zlib (data, len, lvl, strategies[i], &c, &cl))
            consider_candidate (c, cl, COMP_METHOD_DEFLATE,
                                best, best_len, best_method);
        }
      }
}

#ifdef HAVE_LIBDEFLATE
static void
try_libdeflate_all (const unsigned char *data, size_t len,
                    unsigned char **best, size_t *best_len,
                    int *best_method)
{
  for (int lvl = 1; lvl <= 12; lvl++)
    {
      unsigned char *c = NULL;
      size_t cl = 0;
      if (deflate_with_libdeflate (data, len, lvl, &c, &cl))
        consider_candidate (c, cl, COMP_METHOD_DEFLATE,
                            best, best_len, best_method);
    }
}
#endif

/* Stage 1: full Zopfli grid — 4 combos of (last, splitmax) plus
   no-split for tiny files. Keeps the best stream seen. */
static void
try_zopfli_max (const unsigned char *data, size_t len,
                unsigned char **best, size_t *best_len,
                int *best_method)
{
  int iter;

  if (!policy_zopfli_allowed (data, len))
    return;

  if (len <= 64 * 1024)
    iter = ZOPFLI_ITER_TINY;
  else if (len <= 256 * 1024)
    iter = ZOPFLI_ITER_SMALL;
  else if (len <= 1024 * 1024)
    iter = ZOPFLI_ITER_MEDIUM;
  else
    iter = ZOPFLI_ITER_LARGE;

  /* Grid: blocksplittinglast {0,1} x splitmax {15,0} = 4 trials. */
  for (int last = 0; last <= 1; last++)
    for (int sm = 0; sm < 2; sm++)
      {
        int split_max = (sm == 0 ? 15 : 0);
        unsigned char *c = NULL;
        size_t cl = 0;
        if (deflate_with_zopfli (data, len, iter, split_max,
                                 1, last, &c, &cl))
          consider_candidate (c, cl, COMP_METHOD_DEFLATE,
                              best, best_len, best_method);
      }

  /* Fifth trial for tiny files: no block splitting at all. */
  if (len <= 64 * 1024)
    {
      unsigned char *c = NULL;
      size_t cl = 0;
      if (deflate_with_zopfli (data, len, iter, 0, 0, 0, &c, &cl))
        consider_candidate (c, cl, COMP_METHOD_DEFLATE,
                            best, best_len, best_method);
    }
}

/* ------------------------------------------------------------------ */

bool
competitor_compress (const unsigned char *data, size_t len,
                     const char *filename,
                     unsigned char **out, size_t *out_len,
                     int *method)
{
  unsigned char *best = NULL;
  size_t best_len;
  int best_method;

  if (!data || !out || !out_len || !method)
    return false;

  if (len == 0)
    {
      unsigned char *buf = malloc (1);
      if (!buf)
        return false;
      *out = buf;
      *out_len = 0;
      *method = COMP_METHOD_STORE;
      return true;
    }

  if (policy_should_store_only (filename, data, len))
    {
      unsigned char *buf = malloc (len);
      if (!buf)
        return false;
      memcpy (buf, data, len);
      *out = buf;
      *out_len = len;
      *method = COMP_METHOD_STORE;
      return true;
    }

  best_len = len;
  best_method = COMP_METHOD_STORE;
  best = NULL;

  try_zlib_exhaustive (data, len, &best, &best_len, &best_method);

#ifdef HAVE_LIBDEFLATE
  try_libdeflate_all (data, len, &best, &best_len, &best_method);
#endif

  /* Extra strategy for very small files mimics AdvanceCOMP retry. */
  if (len < SMALL_FILE_EXTRA_PASS)
    {
      unsigned char *c = NULL;
      size_t cl = 0;
      if (deflate_with_zlib (data, len, 6, Z_DEFAULT_STRATEGY, &c, &cl))
        consider_candidate (c, cl, COMP_METHOD_DEFLATE,
                            &best, &best_len, &best_method);
    }

  try_zopfli_max (data, len, &best, &best_len, &best_method);

  /* If nothing beat the original size, store. */
  if (!best || best_len >= len)
    {
      unsigned char *buf;

      if (best)
        free (best);

      buf = malloc (len);
      if (!buf)
        return false;

      memcpy (buf, data, len);
      *out = buf;
      *out_len = len;
      *method = COMP_METHOD_STORE;
      return true;
    }

  *out = best;
  *out_len = best_len;
  *method = best_method;
  return true;
}

bool
competitor_decompress (const unsigned char *comp, size_t comp_len,
                       int method,
                       unsigned char *out, size_t out_len)
{
  z_stream stream;

  if (!comp || !out)
    return false;

  if (method == COMP_METHOD_STORE)
    {
      if (comp_len != out_len)
        return false;
      memcpy (out, comp, comp_len);
      return true;
    }

  if (method != COMP_METHOD_DEFLATE)
    return false;

  memset (&stream, 0, sizeof stream);
  stream.next_in = (unsigned char *) comp;
  stream.avail_in = (unsigned int) comp_len;
  stream.next_out = out;
  stream.avail_out = (unsigned int) out_len;

  if (inflateInit2 (&stream, RAW_WINDOW_BITS) != Z_OK)
    return false;

  {
    int ret = inflate (&stream, Z_FINISH);
    inflateEnd (&stream);
    return ret == Z_STREAM_END && stream.total_out == out_len;
  }
}
