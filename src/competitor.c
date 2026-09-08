#include "competitor.h"
#include "policy.h"
#include "enhanced.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef HAVE_LIBDEFLATE
#include <libdeflate.h>
#endif

#include "zopfli/zopfli.h"

/* Progress callback for smooth single-file indication. */
static competitor_progress_cb g_progress_cb = NULL;
static void *g_progress_user = NULL;
static int g_progress_base = 0;
static int g_progress_range = 80;
/* Highest pct already reported for the current file: trials restart their
   own iteration counters, so raw values would jump backwards. Clamp. */
static int g_progress_max = 0;

void
competitor_set_progress_cb (competitor_progress_cb cb, void *user)
{
  g_progress_cb = cb;
  g_progress_user = user;
}

static void
report_progress (int pct)
{
  if (!g_progress_cb)
    return;
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  if (pct < g_progress_max)
    pct = g_progress_max;
  else
    g_progress_max = pct;
  g_progress_cb (pct, g_progress_user);
}

/* Called from Zopfli per iteration for smooth single-file progress. */
void
zopfli_report_iter (int iter, int total)
{
  if (!g_progress_cb || total <= 0)
    return;
  int pct = g_progress_base + iter * g_progress_range / total;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (pct < g_progress_max)
    pct = g_progress_max;
  else
    g_progress_max = pct;
  g_progress_cb (pct, g_progress_user);
}


/* Raw DEFLATE window in zlib: negative means no zlib header. */
static const int RAW_WINDOW_BITS = -15;
static const int ZLIB_MEM_LEVEL = 9;


/* Last winning encoder description (for final report). */
static char g_last_desc[256] = "Store";
static char g_best_overall_desc[256] = "Store";
static size_t g_best_overall_saved = 0;

const char *
competitor_last_desc (void)
{
  return g_last_desc;
}

/* For final archive summary. */
const char *
competitor_best_overall_desc (void)
{
  return g_best_overall_desc;
}

size_t
competitor_best_overall_saved (void)
{
  return g_best_overall_saved;
}

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

/* Keep the smallest buffer seen so far, with description. */
static void
consider_candidate (unsigned char *cand, size_t cand_len, int cand_method,
                    const char *cand_desc,
                    unsigned char **best, size_t *best_len,
                    int *best_method, char *best_desc, size_t desc_sz)
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
      if (cand_desc && best_desc)
        {
          strncpy (best_desc, cand_desc, desc_sz - 1);
          best_desc[desc_sz - 1] = '\0';
        }
    }
  else
    free (cand);
}

/* ------------------------------------------------------------------ */
/* Exhaustive DEFLATE search */

static void
try_zlib_exhaustive (const unsigned char *data, size_t len,
                     unsigned char **best, size_t *best_len,
                     int *best_method, char *best_desc)
{
  const katzip_config_t *cfg = config_get ();
  int strategies[5];
  int n_strat = 0;
  size_t i;
  int min_lvl, max_lvl;

  if (!data || !best || !best_len || !best_method || !best_desc)
    return;
  if (!cfg || !cfg->zlib_enabled)
    return;

  if (cfg->zlib_s_default)
    strategies[n_strat++] = Z_DEFAULT_STRATEGY;
  if (cfg->zlib_s_filtered)
    strategies[n_strat++] = Z_FILTERED;
  if (cfg->zlib_s_huffman)
    strategies[n_strat++] = Z_HUFFMAN_ONLY;
  if (cfg->zlib_s_rle)
    strategies[n_strat++] = 3 /* Z_RLE */;
  if (cfg->zlib_s_fixed)
    strategies[n_strat++] = Z_FIXED;
  if (n_strat == 0)
    return;

  min_lvl = cfg->zlib_min_level < 1 ? 1 : cfg->zlib_min_level;
  max_lvl = cfg->zlib_max_level > 9 ? 9 : cfg->zlib_max_level;
  if (min_lvl > max_lvl)
    return;

  /* Large files: only the top level is worth the time. */
  if (len > cfg->zlib_full_grid_max)
    {
      for (i = 0; i < (size_t) n_strat; i++)
        {
          unsigned char *c = NULL;
          size_t cl = 0;
          if (deflate_with_zlib (data, len, max_lvl, strategies[i],
                                 &c, &cl))
            {
              char desc[96];
              snprintf (desc, sizeof desc,
                        "Deflate zlib level %d strategy %d",
                        max_lvl, strategies[i]);
              consider_candidate (c, cl, COMP_METHOD_DEFLATE, desc,
                                  best, best_len, best_method, best_desc, 256);
            }
        }
      return;
    }

  /* Small and medium files: try all levels and strategies. */
  for (int lvl = min_lvl; lvl <= max_lvl; lvl++)
    for (i = 0; i < (size_t) n_strat; i++)
      {
        if ((strategies[i] == 3 || strategies[i] == Z_FIXED)
            && lvl != max_lvl)
          continue;

        {
          unsigned char *c = NULL;
          size_t cl = 0;
          if (deflate_with_zlib (data, len, lvl, strategies[i], &c, &cl))
            {
              char desc[96];
              snprintf (desc, sizeof desc,
                        "Deflate zlib level %d strategy %d", lvl, strategies[i]);
              consider_candidate (c, cl, COMP_METHOD_DEFLATE, desc,
                                  best, best_len, best_method, best_desc, 256);
            }
        }
      }
}

#ifdef HAVE_LIBDEFLATE
static void
try_libdeflate_all (const unsigned char *data, size_t len,
                    unsigned char **best, size_t *best_len,
                    int *best_method, char *best_desc)
{
  const katzip_config_t *cfg = config_get ();
  int min_lvl, max_lvl;

  if (!data || !best || !best_len || !best_method || !best_desc)
    return;
  if (!cfg || !cfg->libdeflate_enabled)
    return;
  min_lvl = cfg->libdeflate_min_level < 1 ? 1 : cfg->libdeflate_min_level;
  max_lvl = cfg->libdeflate_max_level > 12 ? 12 : cfg->libdeflate_max_level;
  if (min_lvl > max_lvl)
    return;

  for (int lvl = min_lvl; lvl <= max_lvl; lvl++)
    {
      unsigned char *c = NULL;
      size_t cl = 0;
      if (deflate_with_libdeflate (data, len, lvl, &c, &cl))
        {
          char desc[96];
          snprintf (desc, sizeof desc,
                    "Deflate libdeflate level %d", lvl);
          consider_candidate (c, cl, COMP_METHOD_DEFLATE, desc,
                              best, best_len, best_method, best_desc, 256);
        }
    }
}
#endif

/* Stage 1: Zopfli grid over configured (last x splitmax) combos plus
   an optional no-split trial. Keeps the best stream seen. */
static void
try_zopfli_max (const unsigned char *data, size_t len,
                unsigned char **best, size_t *best_len,
                int *best_method, char *best_desc)
{
  const katzip_config_t *cfg = config_get ();
  int iter;
  int n_grid;
  int n_trials;
  int trial_idx;

  if (!data || !best || !best_len || !best_method || !best_desc)
    return;
  if (!cfg || !cfg->zopfli_enabled)
    return;
  if (!policy_zopfli_allowed (data, len))
    return;
  if (cfg->zopfli_n_last <= 0 || cfg->zopfli_n_splitmax <= 0)
    {
      if (len > cfg->zopfli_nosplit_max)
        return;
    }

  iter = enhanced_budget_for_size (len);
  if (iter <= 0)
    iter = 15;

  /* Progress for single-file smooth mode: each trial covers
     an equal slice of the Zopfli phase. */
  {
    int saved_base = g_progress_base;
    int saved_range = g_progress_range;
    n_grid = cfg->zopfli_n_last * cfg->zopfli_n_splitmax;
    n_trials = n_grid;
    if (len <= cfg->zopfli_nosplit_max)
      n_trials++;
    if (n_trials <= 0)
      return;
    trial_idx = 0;
    for (int li = 0; li < cfg->zopfli_n_last; li++)
      {
        for (int sm = 0; sm < cfg->zopfli_n_splitmax; sm++)
          {
          int last = cfg->zopfli_last[li];
          int split_max = cfg->zopfli_splitmax[sm];
          int trial_base = saved_base + trial_idx * saved_range / n_trials;
          int trial_range = saved_range / n_trials;
          g_progress_base = trial_base;
          g_progress_range = trial_range;

          {
            unsigned char *c = NULL;
            size_t cl = 0;
            report_progress (trial_base);
            if (deflate_with_zopfli (data, len, iter, split_max,
                                     1, last, &c, &cl))
            {
              if (getenv ("KATZIP_DEBUG"))
                fprintf (stderr, "[dbg] zopfli iter %d splitmax %d last %d -> %zu\n",
                         iter, split_max, last, cl);
              char desc[128];
              snprintf (desc, sizeof desc,
                        "Deflate Zopfli iter %d splitmax %d last %d",
                        iter, split_max, last);
              consider_candidate (c, cl, COMP_METHOD_DEFLATE, desc,
                                  best, best_len, best_method, best_desc, 256);
            }
          }
            trial_idx++;
          }
      }
      /* Extra trial for small files: no block splitting at all. */
      if (len <= cfg->zopfli_nosplit_max)
        {
          int trial_base = saved_base + trial_idx * saved_range / n_trials;
          g_progress_base = trial_base;
          g_progress_range = saved_range / n_trials;
          report_progress (trial_base);
          unsigned char *c = NULL;
          size_t cl = 0;
          if (deflate_with_zopfli (data, len, iter, 0, 0, 0, &c, &cl))
            {
              if (getenv ("KATZIP_DEBUG"))
                fprintf (stderr, "[dbg] zopfli iter %d nosplit -> %zu\n",
                         iter, cl);
              char desc[128];
              snprintf (desc, sizeof desc,
                        "Deflate Zopfli iter %d nosplit", iter);
              consider_candidate (c, cl, COMP_METHOD_DEFLATE, desc,
                                  best, best_len, best_method, best_desc, 256);
            }
        }
      report_progress (saved_base + saved_range);
    }
}

/* Stage 2: second engine (ECT/zenzop ideas, pure C, in-house).
   Adds joint-cost-friendly black-box trials plus parser-diversified
   single-block coding. Min-preserving: keeps global best only if smaller. */
static void
try_enhanced (const unsigned char *data, size_t len,
              unsigned char **best, size_t *best_len,
              int *best_method, char *best_desc)
{
  unsigned char *c = NULL;
  size_t cl = 0;
  char desc[256] = "";

  if (!data || len == 0 || !best || !best_len || !best_method || !best_desc)
    return;
  /* [zopfli] enabled=off is the master switch: it also disables the
     second engine, which is zopfli-powered internally. */
  if (!config_get ()->enh_enabled || !config_get ()->zopfli_enabled)
    return;
  if (!policy_zopfli_allowed (data, len))
    return;

  /* Second engine owns the 80-100% slice: internal Zopfli iterations
     keep reporting live through the same hook. */
  g_progress_base = 80;
  g_progress_range = 20;

  if (enhanced_compress (data, len, &c, &cl, desc, sizeof desc))
    consider_candidate (c, cl, COMP_METHOD_DEFLATE, desc,
                        best, best_len, best_method, best_desc, 256);
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
  char best_desc[256];

  if (!data || !out || !out_len || !method)
    return false;

  strncpy (best_desc, "Store", sizeof best_desc);
  best_desc[sizeof best_desc - 1] = '\0';

  if (len == 0)
    {
      unsigned char *buf = malloc (1);
      if (!buf)
        return false;
      *out = buf;
      *out_len = 0;
      *method = COMP_METHOD_STORE;
      strncpy (g_last_desc, "Store", sizeof g_last_desc);
      report_progress (100);
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
      strncpy (g_last_desc, "Store", sizeof g_last_desc);
      report_progress (100);
      return true;
    }

  best_len = len;
  best_method = COMP_METHOD_STORE;
  best = NULL;

  /* Progress split: Stage 1 grid reports 0-80%, Stage 2 engine 80-100%
     (archiver only sets the callback, the mapping lives here). */
  g_progress_base = 0;
  g_progress_range = 80;
  g_progress_max = 0;

  try_zlib_exhaustive (data, len, &best, &best_len, &best_method, best_desc);

#ifdef HAVE_LIBDEFLATE
  try_libdeflate_all (data, len, &best, &best_len, &best_method, best_desc);
#endif

  /* Extra strategy for very small files mimics AdvanceCOMP retry.
     Honors the configured level range and strategy set. */
  if (config_get ()->zlib_enabled
      && config_get ()->zlib_extra_retry
      && config_get ()->zlib_s_default
      && len < config_get ()->zlib_extra_retry_max
      && 6 >= (config_get ()->zlib_min_level < 1
               ? 1 : config_get ()->zlib_min_level)
      && 6 <= (config_get ()->zlib_max_level > 9
               ? 9 : config_get ()->zlib_max_level))
    {
      unsigned char *c = NULL;
      size_t cl = 0;
      if (deflate_with_zlib (data, len, 6, Z_DEFAULT_STRATEGY, &c, &cl))
        {
          char desc[96];
          snprintf (desc, sizeof desc, "Deflate zlib level 6 strategy 0");
          consider_candidate (c, cl, COMP_METHOD_DEFLATE, desc,
                              &best, &best_len, &best_method, best_desc, 256);
        }
    }

  try_zopfli_max (data, len, &best, &best_len, &best_method, best_desc);

  try_enhanced (data, len, &best, &best_len, &best_method, best_desc);

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
      strncpy (g_last_desc, "Store", sizeof g_last_desc);
      report_progress (100);
      return true;
    }

  *out = best;
  *out_len = best_len;
  *method = best_method;
  strncpy (g_last_desc, best_desc, sizeof g_last_desc);
  report_progress (100);

  /* Track overall best for final summary (largest saving). */
  {
    size_t saved = len > best_len ? len - best_len : 0;
    if (saved > g_best_overall_saved)
      {
        g_best_overall_saved = saved;
        strncpy (g_best_overall_desc, best_desc, sizeof g_best_overall_desc);
      }
  }

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
