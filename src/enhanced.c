#include "enhanced.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "zopfli/zopfli.h"
#include "zopfli/deflate.h"
#include "zopfli/lz77.h"
#include "zopfli/squeeze.h"
#include "zopfli/tree.h"
#include "zopfli/symbols.h"
#include "zopfli/util.h"
#include "zopfli/hash.h"
#include "config.h"
#include "zopfli/blocksplitter.h"

/* Reuse vendored RLE histogram optimizer (global in deflate.c). */
extern void OptimizeHuffmanForRle (int length, size_t *counts);

static const int RAW_WINDOW_BITS = -15;
/* Size/iter fences for single-block diversification live in config
   (enh_singleblock_max, enh_custom_iter_cap). */

int
enhanced_budget_for_size (size_t len)
{
  const katzip_config_t *cfg = config_get ();

  if (!cfg)
    {
      if (len <= 64UL * 1024UL)
        return 1000;
      if (len <= 256UL * 1024UL)
        return 200;
      if (len <= 1024UL * 1024UL)
        return 60;
      return 15;
    }
  if (len <= cfg->zopfli_tiny_max)
    return cfg->zopfli_iter_tiny;
  if (len <= cfg->zopfli_small_max)
    return cfg->zopfli_iter_small;
  if (len <= cfg->zopfli_med_max)
    return cfg->zopfli_iter_medium;
  return cfg->zopfli_iter_large;
}

/* ------------------------------------------------------------------ */
/* Verify raw DEFLATE inflates to the original. */

static bool
enh_verify (const unsigned char *plain, size_t plain_len,
            const unsigned char *comp, size_t comp_len)
{
  unsigned char *tmp;
  z_stream stream;
  int ret;
  bool ok;

  if (!plain || !comp)
    return false;
  if (plain_len == 0 || comp_len == 0)
    return false;

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

/* Keep smallest buffer; takes ownership of cand. */
static void
enh_consider (unsigned char *cand, size_t cand_len, const char *cand_desc,
              unsigned char **best, size_t *best_len,
              char *best_desc, size_t best_sz)
{
  if (!cand || !best || !best_len)
    {
      if (cand)
        free (cand);
      return;
    }
  if (getenv ("KATZIP_DEBUG") && cand_desc)
    fprintf (stderr, "[dbg] enhanced %s -> %zu\n", cand_desc, cand_len);
  if (cand_len < *best_len)
    {
      if (*best)
        free (*best);
      *best = cand;
      *best_len = cand_len;
      if (cand_desc && best_desc && best_sz > 0)
        {
          strncpy (best_desc, cand_desc, best_sz - 1);
          best_desc[best_sz - 1] = '\0';
        }
    }
  else
    free (cand);
}

/* ------------------------------------------------------------------ */
/* Bit writer with preallocated buffer (all writes bounds-checked). */

typedef struct
{
  unsigned char *buf;
  size_t cap;
  size_t size;
  unsigned bit; /* next bit position 0..7 in buf[size-1]; 0 = need new byte */
} EnhWriter;

static bool
enh_writer_init (EnhWriter *w, size_t cap)
{
  if (!w)
    return false;
  if (cap < 2048)
    cap = 2048;
  w->buf = malloc (cap);
  if (!w->buf)
    return false;
  w->cap = cap;
  w->size = 0;
  w->bit = 0;
  return true;
}

static void
enh_writer_free (EnhWriter *w)
{
  if (!w)
    return;
  if (w->buf)
    free (w->buf);
  w->buf = NULL;
  w->cap = 0;
  w->size = 0;
  w->bit = 0;
}

static bool
enh_write_bit (EnhWriter *w, int bit)
{
  if (!w || !w->buf)
    return false;
  if (w->bit == 0)
    {
      if (w->size >= w->cap)
        return false;
      w->buf[w->size++] = 0;
    }
  if (bit)
    w->buf[w->size - 1] |= (unsigned char) (1u << w->bit);
  w->bit = (w->bit + 1) & 7;
  return true;
}

static bool
enh_write_bits (EnhWriter *w, unsigned symbol, unsigned length)
{
  unsigned i;

  if (!w)
    return false;
  for (i = 0; i < length; i++)
    if (!enh_write_bit (w, (int) ((symbol >> i) & 1)))
      return false;
  return true;
}

static bool
enh_write_huffman (EnhWriter *w, unsigned symbol, unsigned length)
{
  unsigned i;

  if (!w)
    return false;
  for (i = 0; i < length; i++)
    if (!enh_write_bit (w, (int) ((symbol >> (length - i - 1)) & 1)))
      return false;
  return true;
}

/* ------------------------------------------------------------------ */
/* Dynamic tree coding with expanded precode search (all 8 use_16/17/18
   combos scored by real bit size, best encoded). Adapted from Zopfli
   deflate.c EncodeTree (Apache-2.0, same vendored source). */

static size_t
enh_encode_tree (const unsigned *ll_lengths, const unsigned *d_lengths,
                 int use_16, int use_17, int use_18, EnhWriter *w)
{
  static const unsigned order[19] =
    { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
  unsigned hlit = 29;
  unsigned hdist = 29;
  unsigned hlit2;
  unsigned hclen;
  size_t lld_total;
  size_t i, j;
  size_t clcounts[19];
  unsigned clcl[19];
  unsigned clsymbols[19];
  unsigned rle[2048];
  unsigned rle_bits[2048];
  size_t rle_size = 0;
  int size_only = (w == NULL);
  size_t result = 0;

  if (!ll_lengths || !d_lengths)
    return 0;

  for (i = 0; i < 19; i++)
    clcounts[i] = 0;

  while (hlit > 0 && ll_lengths[257 + hlit - 1] == 0)
    hlit--;
  while (hdist > 0 && d_lengths[1 + hdist - 1] == 0)
    hdist--;
  hlit2 = hlit + 257;
  lld_total = hlit2 + hdist + 1;
  if (lld_total > 2048)
    return 0;

  for (i = 0; i < lld_total; i++)
    {
      unsigned char symbol =
        (unsigned char) (i < hlit2 ? ll_lengths[i] : d_lengths[i - hlit2]);
      unsigned count = 1;
      if (use_16 || (symbol == 0 && (use_17 || use_18)))
        for (j = i + 1;
             j < lld_total
               && symbol == (j < hlit2 ? ll_lengths[j] : d_lengths[j - hlit2]);
             j++)
          count++;
      i += count - 1;

      if (symbol == 0 && count >= 3)
        {
          if (use_18)
            while (count >= 11)
              {
                unsigned c2 = count > 138 ? 138 : count;
                if (!size_only)
                  {
                    if (rle_size + 1 >= 2048)
                      return 0;
                    rle[rle_size] = 18;
                    rle_bits[rle_size] = c2 - 11;
                    rle_size++;
                  }
                clcounts[18]++;
                count -= c2;
              }
          if (use_17)
            while (count >= 3)
              {
                unsigned c2 = count > 10 ? 10 : count;
                if (!size_only)
                  {
                    if (rle_size + 1 >= 2048)
                      return 0;
                    rle[rle_size] = 17;
                    rle_bits[rle_size] = c2 - 3;
                    rle_size++;
                  }
                clcounts[17]++;
                count -= c2;
              }
        }

      if (use_16 && count >= 4)
        {
          count--;
          clcounts[symbol]++;
          if (!size_only)
            {
              if (rle_size + 1 >= 2048)
                return 0;
              rle[rle_size] = symbol;
              rle_bits[rle_size] = 0;
              rle_size++;
            }
          while (count >= 3)
            {
              unsigned c2 = count > 6 ? 6 : count;
              if (!size_only)
                {
                  if (rle_size + 1 >= 2048)
                    return 0;
                  rle[rle_size] = 16;
                  rle_bits[rle_size] = c2 - 3;
                  rle_size++;
                }
              clcounts[16]++;
              count -= c2;
            }
        }

      clcounts[symbol] += count;
      while (count > 0)
        {
          if (!size_only)
            {
              if (rle_size + 1 >= 2048)
                return 0;
              rle[rle_size] = symbol;
              rle_bits[rle_size] = 0;
              rle_size++;
            }
          count--;
        }
    }

  ZopfliCalculateBitLengths (clcounts, 19, 7, clcl);
  if (!size_only)
    ZopfliLengthsToSymbols (clcl, 19, 7, clsymbols);

  hclen = 15;
  while (hclen > 0 && clcounts[order[hclen + 4 - 1]] == 0)
    hclen--;

  if (!size_only)
    {
      if (!enh_write_bits (w, hlit, 5))
        return 0;
      if (!enh_write_bits (w, hdist, 5))
        return 0;
      if (!enh_write_bits (w, hclen, 4))
        return 0;
      for (i = 0; i < hclen + 4; i++)
        if (!enh_write_bits (w, clcl[order[i]], 3))
          return 0;
      for (i = 0; i < rle_size; i++)
        {
          unsigned sym = clsymbols[rle[i]];
          if (!enh_write_huffman (w, sym, clcl[rle[i]]))
            return 0;
          if (rle[i] == 16 && !enh_write_bits (w, rle_bits[i], 2))
            return 0;
          else if (rle[i] == 17 && !enh_write_bits (w, rle_bits[i], 3))
            return 0;
          else if (rle[i] == 18 && !enh_write_bits (w, rle_bits[i], 7))
            return 0;
        }
    }

  result += 14;
  result += (hclen + 4) * 3;
  for (i = 0; i < 19; i++)
    result += clcl[i] * clcounts[i];
  result += clcounts[16] * 2;
  result += clcounts[17] * 3;
  result += clcounts[18] * 7;
  return result;
}

static bool
enh_add_dynamic_tree (const unsigned *ll_lengths, const unsigned *d_lengths,
                      EnhWriter *w)
{
  int i;
  int best = 0;
  size_t bestsize = 0;

  if (!ll_lengths || !d_lengths || !w)
    return false;

  for (i = 0; i < 8; i++)
    {
      size_t s = enh_encode_tree (ll_lengths, d_lengths,
                                  i & 1, i & 2, i & 4, NULL);
      if (s == 0)
        return false;
      if (bestsize == 0 || s < bestsize)
        {
          bestsize = s;
          best = i;
        }
    }
  return enh_encode_tree (ll_lengths, d_lengths,
                          best & 1, best & 2, best & 4, w) != 0;
}

static void
enh_fixed_tree (unsigned *ll_lengths, unsigned *d_lengths)
{
  size_t i;

  if (!ll_lengths || !d_lengths)
    return;
  for (i = 0; i < 144; i++)
    ll_lengths[i] = 8;
  for (i = 144; i < 256; i++)
    ll_lengths[i] = 9;
  for (i = 256; i < 280; i++)
    ll_lengths[i] = 7;
  for (i = 280; i < 288; i++)
    ll_lengths[i] = 8;
  for (i = 0; i < 32; i++)
    d_lengths[i] = 5;
}

/* Compat padding for buggy decoders (same rule as Zopfli). */
static void
enh_patch_dist (unsigned *d_lengths)
{
  int n = 0;
  int i;

  if (!d_lengths)
    return;
  for (i = 0; i < 30; i++)
    {
      if (d_lengths[i])
        n++;
      if (n >= 2)
        return;
    }
  if (n == 0)
    d_lengths[0] = d_lengths[1] = 1;
  else if (n == 1)
    d_lengths[d_lengths[0] ? 1 : 0] = 1;
}

static bool
enh_add_lz77_data (const ZopfliLZ77Store *lz77,
                   const unsigned *ll_symbols, const unsigned *ll_lengths,
                   const unsigned *d_symbols, const unsigned *d_lengths,
                   EnhWriter *w)
{
  size_t i;

  if (!lz77 || !ll_symbols || !ll_lengths || !d_symbols || !d_lengths || !w)
    return false;

  for (i = 0; i < lz77->size; i++)
    {
      unsigned dist = lz77->dists[i];
      unsigned litlen = lz77->litlens[i];
      if (dist == 0)
        {
          if (litlen >= 256 || ll_lengths[litlen] == 0)
            return false;
          if (!enh_write_huffman (w, ll_symbols[litlen], ll_lengths[litlen]))
            return false;
        }
      else
        {
          unsigned ls;
          unsigned ds;
          if (litlen < 3 || litlen > 288)
            return false;
          ls = (unsigned) ZopfliGetLengthSymbol ((int) litlen);
          ds = (unsigned) ZopfliGetDistSymbol ((int) dist);
          if (ls >= 288 || ds >= 32)
            return false;
          if (ll_lengths[ls] == 0 || d_lengths[ds] == 0)
            return false;
          if (!enh_write_huffman (w, ll_symbols[ls], ll_lengths[ls]))
            return false;
          if (!enh_write_bits (w,
                               (unsigned) ZopfliGetLengthExtraBitsValue (
                                 (int) litlen),
                               (unsigned) ZopfliGetLengthExtraBits (
                                 (int) litlen)))
            return false;
          if (!enh_write_huffman (w, d_symbols[ds], d_lengths[ds]))
            return false;
          if (!enh_write_bits (w,
                               (unsigned) ZopfliGetDistExtraBitsValue (
                                 (int) dist),
                               (unsigned) ZopfliGetDistExtraBits ((int) dist)))
            return false;
        }
    }
  return true;
}

/* Huffman lengths for one store; use_rle selects ECT-style RLE shaping. */
static void
enh_dynamic_lengths (const ZopfliLZ77Store *lz77, int use_rle,
                     unsigned *ll_lengths, unsigned *d_lengths)
{
  size_t ll_counts[ZOPFLI_NUM_LL];
  size_t d_counts[ZOPFLI_NUM_D];

  if (!lz77 || !ll_lengths || !d_lengths)
    return;
  ZopfliLZ77GetHistogram (lz77, 0, lz77->size, ll_counts, d_counts);
  ll_counts[256] = 1;
  if (use_rle)
    {
      OptimizeHuffmanForRle (ZOPFLI_NUM_LL, ll_counts);
      OptimizeHuffmanForRle (ZOPFLI_NUM_D, d_counts);
    }
  ZopfliCalculateBitLengths (ll_counts, ZOPFLI_NUM_LL, 15, ll_lengths);
  ZopfliCalculateBitLengths (d_counts, ZOPFLI_NUM_D, 15, d_lengths);
  enh_patch_dist (d_lengths);
}

/* Emit single fixed block (BFINAL=1, BTYPE=01). */
static bool
enh_emit_fixed (const ZopfliLZ77Store *lz77, size_t raw_len,
                unsigned char **out, size_t *out_len)
{
  EnhWriter w;
  unsigned ll_lengths[ZOPFLI_NUM_LL];
  unsigned d_lengths[ZOPFLI_NUM_D];
  unsigned ll_symbols[ZOPFLI_NUM_LL];
  unsigned d_symbols[ZOPFLI_NUM_D];

  if (!lz77 || !out || !out_len)
    return false;
  if (!enh_writer_init (&w, raw_len * 2 + 2048))
    return false;

  enh_fixed_tree (ll_lengths, d_lengths);
  ZopfliLengthsToSymbols (ll_lengths, ZOPFLI_NUM_LL, 15, ll_symbols);
  ZopfliLengthsToSymbols (d_lengths, ZOPFLI_NUM_D, 15, d_symbols);

  if (!enh_write_bit (&w, 1)
      || !enh_write_bit (&w, 1)
      || !enh_write_bit (&w, 0)
      || !enh_add_lz77_data (lz77, ll_symbols, ll_lengths,
                             d_symbols, d_lengths, &w)
      || !enh_write_huffman (&w, ll_symbols[256], ll_lengths[256]))
    {
      enh_writer_free (&w);
      return false;
    }

  *out = w.buf;
  *out_len = w.size;
  return true;
}

/* Emit single dynamic block (BFINAL=1, BTYPE=10) with given lengths. */
static bool
enh_emit_dynamic (const ZopfliLZ77Store *lz77, size_t raw_len,
                  const unsigned *ll_lengths, const unsigned *d_lengths,
                  unsigned char **out, size_t *out_len)
{
  EnhWriter w;
  unsigned ll_symbols[ZOPFLI_NUM_LL];
  unsigned d_symbols[ZOPFLI_NUM_D];

  if (!lz77 || !ll_lengths || !d_lengths || !out || !out_len)
    return false;
  if (!enh_writer_init (&w, raw_len * 2 + 2048))
    return false;

  ZopfliLengthsToSymbols (ll_lengths, ZOPFLI_NUM_LL, 15, ll_symbols);
  ZopfliLengthsToSymbols (d_lengths, ZOPFLI_NUM_D, 15, d_symbols);

  if (!enh_write_bit (&w, 1)
      || !enh_write_bit (&w, 0)
      || !enh_write_bit (&w, 1)
      || !enh_add_dynamic_tree (ll_lengths, d_lengths, &w)
      || !enh_add_lz77_data (lz77, ll_symbols, ll_lengths,
                             d_symbols, d_lengths, &w)
      || !enh_write_huffman (&w, ll_symbols[256], ll_lengths[256]))
    {
      enh_writer_free (&w);
      return false;
    }

  *out = w.buf;
  *out_len = w.size;
  return true;
}

/* Best single-block coding of one parse: fixed vs dynamic raw vs dynamic
   RLE-shaped, winner chosen by real byte size (not estimated cost). */
static bool
enh_best_of_store (const unsigned char *data, size_t len,
                   const ZopfliLZ77Store *store, const char *parser,
                   unsigned char **out, size_t *out_len,
                   char *desc, size_t desc_sz)
{
  unsigned ll_raw[ZOPFLI_NUM_LL];
  unsigned d_raw[ZOPFLI_NUM_D];
  unsigned ll_rle[ZOPFLI_NUM_LL];
  unsigned d_rle[ZOPFLI_NUM_D];
  unsigned char *best = NULL;
  size_t best_len = (size_t) -1;
  char best_desc[128] = "";
  unsigned char *c = NULL;
  size_t cl = 0;

  if (!data || !store || !parser || !out || !out_len)
    return false;
  if (len == 0)
    return false;

  if (enh_emit_fixed (store, len, &c, &cl) && enh_verify (data, len, c, cl))
    {
      char d[128];
      snprintf (d, sizeof d, "Deflate enhanced singleblock %s fixed", parser);
      enh_consider (c, cl, d, &best, &best_len, best_desc, sizeof best_desc);
    }
  else if (c)
    {
      free (c);
      c = NULL;
    }

  enh_dynamic_lengths (store, 0, ll_raw, d_raw);
  c = NULL;
  cl = 0;
  if (enh_emit_dynamic (store, len, ll_raw, d_raw, &c, &cl)
      && enh_verify (data, len, c, cl))
    {
      char d[128];
      snprintf (d, sizeof d, "Deflate enhanced singleblock %s dynamic", parser);
      enh_consider (c, cl, d, &best, &best_len, best_desc, sizeof best_desc);
    }
  else if (c)
    {
      free (c);
      c = NULL;
    }

  enh_dynamic_lengths (store, 1, ll_rle, d_rle);
  c = NULL;
  cl = 0;
  if (enh_emit_dynamic (store, len, ll_rle, d_rle, &c, &cl)
      && enh_verify (data, len, c, cl))
    {
      char d[128];
      snprintf (d, sizeof d,
                "Deflate enhanced singleblock %s dynamic-rle", parser);
      enh_consider (c, cl, d, &best, &best_len, best_desc, sizeof best_desc);
    }
  else if (c)
    {
      free (c);
      c = NULL;
    }

  if (!best)
    return false;
  *out = best;
  *out_len = best_len;
  if (desc && desc_sz > 0)
    {
      strncpy (desc, best_desc, desc_sz - 1);
      desc[desc_sz - 1] = '\0';
    }
  return true;
}

/* ------------------------------------------------------------------ */
/* Parser diversification: three LZ77 front-ends. */

static bool
enh_parse_optimal (const unsigned char *data, size_t len, int iter,
                   ZopfliLZ77Store *store)
{
  ZopfliOptions opts;
  ZopfliBlockState s;

  if (!data || len == 0 || !store || iter <= 0)
    return false;
  ZopfliInitOptions (&opts);
  opts.numiterations = iter;
  ZopfliInitLZ77Store (data, store);
  ZopfliInitBlockState (&opts, 0, len, 1, &s);
  ZopfliLZ77Optimal (&s, data, 0, len, iter, store);
  ZopfliCleanBlockState (&s);
  return true;
}

static bool
enh_parse_fixed (const unsigned char *data, size_t len,
                 ZopfliLZ77Store *store)
{
  ZopfliOptions opts;
  ZopfliBlockState s;

  if (!data || len == 0 || !store)
    return false;
  ZopfliInitOptions (&opts);
  ZopfliInitLZ77Store (data, store);
  ZopfliInitBlockState (&opts, 0, len, 1, &s);
  ZopfliLZ77OptimalFixed (&s, data, 0, len, store);
  ZopfliCleanBlockState (&s);
  return true;
}

static bool
enh_parse_greedy (const unsigned char *data, size_t len,
                  ZopfliLZ77Store *store)
{
  ZopfliOptions opts;
  ZopfliBlockState s;
  ZopfliHash h;

  if (!data || len == 0 || !store)
    return false;
  ZopfliInitOptions (&opts);
  ZopfliInitLZ77Store (data, store);
  ZopfliInitBlockState (&opts, 0, len, 0, &s);
  ZopfliAllocHash (ZOPFLI_WINDOW_SIZE, &h);
  ZopfliLZ77Greedy (&s, data, 0, len, store, &h);
  ZopfliCleanHash (&h);
  ZopfliCleanBlockState (&s);
  return true;
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Writer-based block primitives (share bit writer across blocks). */

static size_t
enh_writer_bits (const EnhWriter *w)
{
  if (!w || w->size == 0)
    return (size_t) -1;
  if (w->bit == 0)
    return w->size * 8;
  return (w->size - 1) * 8 + w->bit;
}

static bool
enh_block_fixed (EnhWriter *w, const ZopfliLZ77Store *store, int is_final)
{
  unsigned ll_lengths[ZOPFLI_NUM_LL];
  unsigned d_lengths[ZOPFLI_NUM_D];
  unsigned ll_symbols[ZOPFLI_NUM_LL];
  unsigned d_symbols[ZOPFLI_NUM_D];

  if (!w || !store)
    return false;
  enh_fixed_tree (ll_lengths, d_lengths);
  ZopfliLengthsToSymbols (ll_lengths, ZOPFLI_NUM_LL, 15, ll_symbols);
  ZopfliLengthsToSymbols (d_lengths, ZOPFLI_NUM_D, 15, d_symbols);

  if (!enh_write_bit (w, is_final ? 1 : 0)
      || !enh_write_bit (w, 1)
      || !enh_write_bit (w, 0)
      || !enh_add_lz77_data (store, ll_symbols, ll_lengths,
                             d_symbols, d_lengths, w)
      || !enh_write_huffman (w, ll_symbols[256], ll_lengths[256]))
    return false;
  return true;
}

static bool
enh_block_dynamic (EnhWriter *w, const ZopfliLZ77Store *store,
                   const unsigned *ll_lengths, const unsigned *d_lengths,
                   int is_final)
{
  unsigned ll_symbols[ZOPFLI_NUM_LL];
  unsigned d_symbols[ZOPFLI_NUM_D];

  if (!w || !store || !ll_lengths || !d_lengths)
    return false;
  ZopfliLengthsToSymbols (ll_lengths, ZOPFLI_NUM_LL, 15, ll_symbols);
  ZopfliLengthsToSymbols (d_lengths, ZOPFLI_NUM_D, 15, d_symbols);

  if (!enh_write_bit (w, is_final ? 1 : 0)
      || !enh_write_bit (w, 0)
      || !enh_write_bit (w, 1)
      || !enh_add_dynamic_tree (ll_lengths, d_lengths, w)
      || !enh_add_lz77_data (store, ll_symbols, ll_lengths,
                             d_symbols, d_lengths, w)
      || !enh_write_huffman (w, ll_symbols[256], ll_lengths[256]))
    return false;
  return true;
}

/* Range-limited parses (block subdomain with full backward window). */
static bool
enh_parse_optimal_range (const unsigned char *data, size_t instart,
                         size_t inend, int iter, ZopfliLZ77Store *store)
{
  ZopfliOptions opts;
  ZopfliBlockState s;

  if (!data || instart >= inend || !store || iter <= 0)
    return false;
  ZopfliInitOptions (&opts);
  opts.numiterations = iter;
  ZopfliInitLZ77Store (data, store);
  ZopfliInitBlockState (&opts, instart, inend, 1, &s);
  ZopfliLZ77Optimal (&s, data, instart, inend, iter, store);
  ZopfliCleanBlockState (&s);
  return true;
}

static bool
enh_parse_fixed_range (const unsigned char *data, size_t instart,
                       size_t inend, ZopfliLZ77Store *store)
{
  ZopfliOptions opts;
  ZopfliBlockState s;

  if (!data || instart >= inend || !store)
    return false;
  ZopfliInitOptions (&opts);
  ZopfliInitLZ77Store (data, store);
  ZopfliInitBlockState (&opts, instart, inend, 1, &s);
  ZopfliLZ77OptimalFixed (&s, data, instart, inend, store);
  ZopfliCleanBlockState (&s);
  return true;
}

/* Trial-encode one block option into a temp writer, return real bit size. */
static size_t
enh_trial_bits (const ZopfliLZ77Store *store, size_t raw_len,
                const unsigned *ll_lengths, const unsigned *d_lengths,
                int is_fixed, int is_final)
{
  EnhWriter w;
  size_t bits;
  bool ok;

  if (!store)
    return (size_t) -1;
  if (!enh_writer_init (&w, raw_len * 2 + 512))
    return (size_t) -1;
  if (is_fixed)
    ok = enh_block_fixed (&w, store, is_final);
  else
    {
      if (!ll_lengths || !d_lengths)
        {
          enh_writer_free (&w);
          return (size_t) -1;
        }
      ok = enh_block_dynamic (&w, store, ll_lengths, d_lengths, is_final);
    }
  bits = ok ? enh_writer_bits (&w) : (size_t) -1;
  enh_writer_free (&w);
  return bits;
}

/* ------------------------------------------------------------------ */
/* Shared range machinery: best coding per range, merge sweep, emit. */

#define ENH_MAX_BLOCKS 16 /* == max zopfli split points (15) +1; truncates silently if exceeded — verify-fenced */

typedef struct
{
  size_t start;
  size_t end;
  size_t bits;
  ZopfliLZ77Store store;
  unsigned ll[ZOPFLI_NUM_LL];
  unsigned d[ZOPFLI_NUM_D];
  int fixed;
} EnhBlock;

/* Best single coding (fixed/dynamic-raw/dynamic-rle by real bits) of one
   parse store. Returns bits ((size_t)-1 on failure). */
static size_t
enh_store_best (const ZopfliLZ77Store *store, size_t raw_len,
                unsigned *ll_best, unsigned *d_best, int *is_fixed)
{
  unsigned ll_raw[ZOPFLI_NUM_LL], d_raw[ZOPFLI_NUM_D];
  unsigned ll_rle[ZOPFLI_NUM_LL], d_rle[ZOPFLI_NUM_D];
  size_t b_fixed, b_raw, b_rle, best;

  if (!store || !ll_best || !d_best || !is_fixed)
    return (size_t) -1;
  enh_dynamic_lengths (store, 0, ll_raw, d_raw);
  enh_dynamic_lengths (store, 1, ll_rle, d_rle);
  b_fixed = enh_trial_bits (store, raw_len, NULL, NULL, 1, 0);
  b_raw = enh_trial_bits (store, raw_len, ll_raw, d_raw, 0, 0);
  b_rle = enh_trial_bits (store, raw_len, ll_rle, d_rle, 0, 0);

  best = b_fixed;
  *is_fixed = 1;
  memcpy (ll_best, ll_raw, sizeof ll_raw);
  memcpy (d_best, d_raw, sizeof d_raw);
  if (b_raw < best)
    {
      best = b_raw;
      *is_fixed = 0;
      memcpy (ll_best, ll_raw, sizeof ll_raw);
      memcpy (d_best, d_raw, sizeof d_raw);
    }
  if (b_rle < best)
    {
      best = b_rle;
      *is_fixed = 0;
      memcpy (ll_best, ll_rle, sizeof ll_rle);
      memcpy (d_best, d_rle, sizeof d_rle);
    }
  return best;
}

/* Best coding of one byte range: optimal + optimal-fixed parses, each in
   3 codings; winning store is COPIED into out_store (caller must clean). */
static size_t
enh_block_best (const unsigned char *data, size_t start, size_t end,
                int iter, int recode, ZopfliLZ77Store *out_store,
                unsigned *ll, unsigned *d, int *fixed)
{
  ZopfliLZ77Store st_opt, st_fix;
  int have_opt = 0, have_fix = 0;
  size_t raw = end - start;
  size_t b1 = (size_t) -1, b2 = (size_t) -1;
  unsigned ll1[ZOPFLI_NUM_LL], d1[ZOPFLI_NUM_D];
  unsigned ll2[ZOPFLI_NUM_LL], d2[ZOPFLI_NUM_D];
  int f1 = 1, f2 = 1;
  size_t best;
  int use_opt;

  if (!data || start >= end || !out_store || !ll || !d || !fixed || iter <= 0)
    return (size_t) -1;
  if (recode <= 0)
    recode = iter;

  have_opt = enh_parse_optimal_range (data, start, end, recode, &st_opt);
  have_fix = enh_parse_fixed_range (data, start, end, &st_fix);
  if (have_opt)
    b1 = enh_store_best (&st_opt, raw, ll1, d1, &f1);
  if (have_fix)
    b2 = enh_store_best (&st_fix, raw, ll2, d2, &f2);
  if (b1 == (size_t) -1 && b2 == (size_t) -1)
    {
      if (have_opt)
        ZopfliCleanLZ77Store (&st_opt);
      if (have_fix)
        ZopfliCleanLZ77Store (&st_fix);
      return (size_t) -1;
    }
  use_opt = (b1 <= b2);
  /* Init first: Copy cleans dest (would free stack garbage otherwise). */
  ZopfliInitLZ77Store (data, out_store);
  if (use_opt)
    {
      ZopfliCopyLZ77Store (&st_opt, out_store);
      memcpy (ll, ll1, sizeof ll1);
      memcpy (d, d1, sizeof d1);
      *fixed = f1;
      best = b1;
    }
  else
    {
      ZopfliCopyLZ77Store (&st_fix, out_store);
      memcpy (ll, ll2, sizeof ll2);
      memcpy (d, d2, sizeof d2);
      *fixed = f2;
      best = b2;
    }
  if (have_opt)
    ZopfliCleanLZ77Store (&st_opt);
  if (have_fix)
    ZopfliCleanLZ77Store (&st_fix);
  return best;
}

static void
enh_block_clean (EnhBlock *b)
{
  if (!b)
    return;
  ZopfliCleanLZ77Store (&b->store);
}

/* Encode ranges: best coding per range, optional adjacent-merge sweep
   (JarTighten -b idea: merge pays when split headers cost more than they
   save), then single continuous emit. Min-preserving, verify-fenced. */
static bool
enh_encode_ranges (const unsigned char *data, size_t len, int iter,
                   int recode,
                   const size_t *starts, const size_t *ends, int n,
                   int do_merge,
                   unsigned char **out, size_t *out_len)
{
  EnhBlock blocks[ENH_MAX_BLOCKS];
  int nblocks = 0;
  int i, pass;
  EnhWriter mainw;
  bool ok = false;

  if (!data || len == 0 || !starts || !ends || n <= 0 || n > ENH_MAX_BLOCKS
      || !out || !out_len || iter <= 0)
    return false;

  for (i = 0; i < n; i++)
    {
      size_t bits = enh_block_best (data, starts[i], ends[i], iter,
                                    recode, &blocks[i].store,
                                    blocks[i].ll, blocks[i].d,
                                    &blocks[i].fixed);
      if (bits == (size_t) -1)
        {
          int k;
          for (k = 0; k < i; k++)
            enh_block_clean (&blocks[k]);
          return false;
        }
      blocks[i].start = starts[i];
      blocks[i].end = ends[i];
      blocks[i].bits = bits;
    }
  nblocks = n;

  /* Merge sweep: join a pair when the joint coding is really smaller. */
  if (do_merge)
    for (pass = 0; pass < ENH_MAX_BLOCKS; pass++)
      {
        int changed = 0;
        i = 0;
        while (i < nblocks - 1)
          {
            ZopfliLZ77Store tmp;
            unsigned tll[ZOPFLI_NUM_LL], td[ZOPFLI_NUM_D];
            int tfix = 1;
            size_t tb = enh_block_best (data, blocks[i].start,
                                        blocks[i + 1].end, iter, recode,
                                        &tmp, tll, td, &tfix);
            if (tb != (size_t) -1
                && tb < blocks[i].bits + blocks[i + 1].bits)
              {
                int k;
                size_t ns = blocks[i].start;
                size_t ne = blocks[i + 1].end;
                enh_block_clean (&blocks[i]);
                enh_block_clean (&blocks[i + 1]);
                for (k = i + 1; k < nblocks - 1; k++)
                  blocks[k] = blocks[k + 1];
                nblocks--;
                /* Rebuild slot i from the winning merge. */
                blocks[i].store = tmp;
                memcpy (blocks[i].ll, tll, sizeof tll);
                memcpy (blocks[i].d, td, sizeof td);
                blocks[i].fixed = tfix;
                blocks[i].bits = tb;
                blocks[i].start = ns;
                blocks[i].end = ne;
                changed = 1;
                continue;
              }
            if (tb != (size_t) -1)
              ZopfliCleanLZ77Store (&tmp);
            i++;
          }
        if (!changed)
          break;
      }

  if (!enh_writer_init (&mainw, len * 2 + 2048))
    {
      for (i = 0; i < nblocks; i++)
        enh_block_clean (&blocks[i]);
      return false;
    }
  for (i = 0; i < nblocks; i++)
    {
      int is_final = (i == nblocks - 1) ? 1 : 0;
      bool w = blocks[i].fixed
        ? enh_block_fixed (&mainw, &blocks[i].store, is_final)
        : enh_block_dynamic (&mainw, &blocks[i].store,
                             blocks[i].ll, blocks[i].d, is_final);
      if (!w)
        {
          int k;
          enh_writer_free (&mainw);
          for (k = 0; k < nblocks; k++)
            enh_block_clean (&blocks[k]);
          return false;
        }
    }
  for (i = 0; i < nblocks; i++)
    enh_block_clean (&blocks[i]);
  if (mainw.size == 0)
    {
      enh_writer_free (&mainw);
      return false;
    }
  if (!enh_verify (data, len, mainw.buf, mainw.size))
    {
      enh_writer_free (&mainw);
      return false;
    }
  ok = true;
  *out = mainw.buf;
  *out_len = mainw.size;
  return ok;
}

/* Foreign split map A (Kzip/Rezop idea): greedy LZ77 front-end (different
   parse family than optimal-based maps) + BlockSplitLZ77, rescored through
   our per-block machinery. Byte coordinates via store pos[]. */
static bool
enh_greedy_map (const unsigned char *data, size_t len,
                size_t *starts, size_t *ends, int *n_out)
{
  ZopfliOptions opts;
  ZopfliBlockState s;
  ZopfliHash h;
  ZopfliLZ77Store store;
  size_t *lzpts = NULL;
  size_t nlz = 0;
  size_t i;
  int n = 0;

  if (!data || len == 0 || !starts || !ends || !n_out)
    return false;

  ZopfliInitOptions (&opts);
  ZopfliInitLZ77Store (data, &store);
  ZopfliInitBlockState (&opts, 0, len, 0, &s);
  ZopfliAllocHash (ZOPFLI_WINDOW_SIZE, &h);
  ZopfliLZ77Greedy (&s, data, 0, len, &store, &h);
  ZopfliCleanHash (&h);
  ZopfliCleanBlockState (&s);

  if (store.size < 10)
    {
      /* Too small for the splitter: single range. */
      starts[0] = 0;
      ends[0] = len;
      *n_out = 1;
      ZopfliCleanLZ77Store (&store);
      return true;
    }

  ZopfliBlockSplitLZ77 (&opts, &store, 15, &lzpts, &nlz);
  {
    size_t prev = 0;
    for (i = 0; i < nlz && n < ENH_MAX_BLOCKS - 1; i++)
      {
        size_t at = (lzpts[i] < store.size) ? store.pos[lzpts[i]] : len;
        if (at <= prev || at >= len)
          continue;
        starts[n] = prev;
        ends[n] = at;
        n++;
        prev = at;
      }
    starts[n] = prev;
    ends[n] = len;
    n++;
  }
  if (lzpts)
    free (lzpts);
  ZopfliCleanLZ77Store (&store);
  if (n <= 0)
    return false;
  *n_out = n;
  return true;
}

/* T8: Kzip + Rezop hybrid (adapted, in-house): foreign split maps
   (greedy-rescored, uniform) through Zopfli rescoring, keep the best. */
static bool
enh_trial_kzip (const unsigned char *data, size_t len, int iter,
                int recode, int do_merge,
                unsigned char **out, size_t *out_len)
{
  unsigned char *best = NULL;
  size_t best_len = (size_t) -1;
  char best_desc[128] = "";
  size_t starts[ENH_MAX_BLOCKS], ends[ENH_MAX_BLOCKS];
  int n = 0;
  int i;

  if (!data || len == 0 || !out || !out_len || iter <= 0)
    return false;

  /* Map A: greedy front-end split map. */
  if (enh_greedy_map (data, len, starts, ends, &n))
    {
      unsigned char *c = NULL;
      size_t cl = 0;
      if (enh_encode_ranges (data, len, iter, recode, starts, ends, n,
                             do_merge, &c, &cl))
        enh_consider (c, cl, "greedy-map", &best, &best_len,
                      best_desc, sizeof best_desc);
    }

  /* Map B: uniform segmentation (maximally foreign map). */
  {
    int parts = 8;
    size_t step;
    if ((size_t) parts > len)
      parts = (int) len;
    if (parts >= 2)
      {
        step = len / (size_t) parts;
        n = 0;
        for (i = 0; i < parts && n < ENH_MAX_BLOCKS; i++)
          {
            starts[n] = (size_t) i * step;
            ends[n] = (i == parts - 1) ? len : (size_t) (i + 1) * step;
            if (ends[n] > starts[n])
              n++;
          }
        if (n > 1)
          {
            unsigned char *c = NULL;
            size_t cl = 0;
            if (enh_encode_ranges (data, len, iter, recode, starts, ends, n,
                                   do_merge, &c, &cl))
              enh_consider (c, cl, "uniform-map", &best, &best_len,
                            best_desc, sizeof best_desc);
          }
      }
  }

  if (!best)
    return false;
  *out = best;
  *out_len = best_len;
  return true;
}

/* T7: own 15-block split map, per-block parse pair and coding chosen by
   real bits, optional adjacent-merge sweep. ECT-style joint correction:
   Zopfli picks block types by estimated cost, we pick by actual bits. */
static bool
enh_trial_split15 (const unsigned char *data, size_t len, int iter,
                   int recode, int do_merge,
                   unsigned char **out, size_t *out_len)
{
  ZopfliOptions opts;
  size_t *pts = NULL;
  size_t npts = 0;
  size_t starts[ENH_MAX_BLOCKS], ends[ENH_MAX_BLOCKS];
  size_t b;
  int n = 0;
  bool ok;

  if (!data || len == 0 || !out || !out_len || iter <= 0)
    return false;

  ZopfliInitOptions (&opts);
  opts.numiterations = iter;
  ZopfliBlockSplit (&opts, data, 0, len, 15, &pts, &npts);

  for (b = 0; b <= npts && n < ENH_MAX_BLOCKS; b++)
    {
      size_t start = (b == 0) ? 0 : pts[b - 1];
      size_t end = (b == npts) ? len : pts[b];
      if (end <= start || end > len)
        continue;
      starts[n] = start;
      ends[n] = end;
      n++;
    }
  if (pts)
    free (pts);
  if (n <= 0)
    return false;

  ok = enh_encode_ranges (data, len, iter, recode, starts, ends, n,
                          do_merge, out, out_len);
  return ok;
}

/* Black-box trials through the vendored core with joint-cost-friendly
   settings Stage 1 never tries for all sizes. */

static bool
enh_bb (const unsigned char *data, size_t len, int iter,
        int kind, unsigned char **out, size_t *out_len)
{
  ZopfliOptions opts;
  unsigned char *buf = NULL;
  size_t blen = 0;

  if (!data || len == 0 || !out || !out_len || iter <= 0)
    return false;
  if (kind < 0 || kind > 2)
    return false;

  ZopfliInitOptions (&opts);
  opts.numiterations = iter;

  if (kind == 0)
    {
      /* Forced fixed-tree multiblock (OptimalFixed parse, fixed coding). */
      unsigned char bp = 0;
      unsigned char *o = NULL;
      size_t os = 0;
      ZopfliDeflatePart (&opts, 1, 1, data, 0, len, &bp, &o, &os);
      buf = o;
      blen = os;
    }
  else
    {
      unsigned char *o = NULL;
      size_t os = 0;
      if (kind == 1)
        {
          /* No splitting at all (block-joint correction: fewer blocks). */
          opts.blocksplitting = 0;
          opts.blocksplittingmax = 0;
        }
      else
        {
          /* Conservative split cap (between 15 and unlimited). */
          const katzip_config_t *cfg = config_get ();
          opts.blocksplitting = 1;
          opts.blocksplittingmax = cfg ? cfg->enh_split5_max : 5;
        }
      ZopfliCompress (&opts, ZOPFLI_FORMAT_DEFLATE, data, len, &o, &os);
      buf = o;
      blen = os;
    }

  if (!buf || blen == 0)
    {
      if (buf)
        free (buf);
      return false;
    }
  if (!enh_verify (data, len, buf, blen))
    {
      free (buf);
      return false;
    }
  *out = buf;
  *out_len = blen;
  return true;
}

/* ------------------------------------------------------------------ */

bool
enhanced_compress (const unsigned char *data,
                   size_t len,
                   unsigned char **out,
                   size_t *out_len,
                   char *desc,
                   size_t desc_sz)
{
  int iter;
  unsigned char *best = NULL;
  size_t best_len = (size_t) -1;
  char best_desc[256] = "";

  if (!data || len == 0 || !out || !out_len)
    return false;

  iter = enhanced_budget_for_size (len);
  if (iter <= 0)
    iter = 15;
  if (iter > 1000)
    iter = 1000;

  const katzip_config_t *cfg = config_get ();
  int single_cap = cfg ? cfg->enh_custom_iter_cap : 200;
  size_t single_max = cfg ? cfg->enh_singleblock_max : 64UL * 1024UL;

  if (!cfg)
    return false;
  if (!cfg->zopfli_enabled)
    return false;

  /* T1: forced fixed multiblock. */
  if (cfg->enh_fixed)
  {
    unsigned char *c = NULL;
    size_t cl = 0;
    char d[128];
    if (enh_bb (data, len, iter, 0, &c, &cl))
      {
        snprintf (d, sizeof d, "Deflate enhanced fixed iter %d", iter);
        enh_consider (c, cl, d, &best, &best_len,
                      best_desc, sizeof best_desc);
      }
  }
  /* T2: single block for every size (Stage 1 has it only for tiny). */
  if (cfg->enh_nosplit)
  {
    unsigned char *c = NULL;
    size_t cl = 0;
    char d[128];
    if (enh_bb (data, len, iter, 1, &c, &cl))
      {
        snprintf (d, sizeof d, "Deflate enhanced nosplit iter %d", iter);
        enh_consider (c, cl, d, &best, &best_len,
                      best_desc, sizeof best_desc);
      }
  }
  /* T3: conservative split cap. */
  if (cfg->enh_split5)
  {
    unsigned char *c = NULL;
    size_t cl = 0;
    char d[128];
    if (enh_bb (data, len, iter, 2, &c, &cl))
      {
        snprintf (d, sizeof d, "Deflate enhanced splitmax 5 iter %d", iter);
        enh_consider (c, cl, d, &best, &best_len,
                      best_desc, sizeof best_desc);
      }
  }

  /* T7: split15 map with per-block actual-size recoding. */
  if (cfg->enh_split15)
  {
    unsigned char *c = NULL;
    size_t cl = 0;
    char d[128];
    if (enh_trial_split15 (data, len, iter, cfg->enh_recode_iters, cfg->enh_merge_blocks,
                               &c, &cl))
      {
        if (cfg->enh_recode_iters > 0 && cfg->enh_recode_iters != iter)
          snprintf (d, sizeof d, "Deflate enhanced split15-recode iter %d+re%d",
                    iter, cfg->enh_recode_iters);
        else
          snprintf (d, sizeof d, "Deflate enhanced split15-recode iter %d", iter);
        enh_consider (c, cl, d, &best, &best_len,
                      best_desc, sizeof best_desc);
      }
  }

  /* T8: Kzip + Rezop hybrid (foreign maps + rescoring). */
  if (cfg->enh_kzip_split)
  {
    unsigned char *c = NULL;
    size_t cl = 0;
    char d[128];
    if (enh_trial_kzip (data, len, iter, cfg->enh_recode_iters, cfg->enh_merge_blocks, &c, &cl))
      {
        if (cfg->enh_recode_iters > 0 && cfg->enh_recode_iters != iter)
          snprintf (d, sizeof d, "Deflate enhanced kzip-split iter %d+re%d",
                    iter, cfg->enh_recode_iters);
        else
          snprintf (d, sizeof d, "Deflate enhanced kzip-split iter %d", iter);
        enh_consider (c, cl, d, &best, &best_len,
                      best_desc, sizeof best_desc);
      }
  }

  /* T4-T6: parser-diversified single-block coding (time-fenced). */
  if (cfg->enh_singleblock && len <= single_max)
    {
      int cit = iter > single_cap ? single_cap : iter;
      ZopfliLZ77Store store;

      if (enh_parse_optimal (data, len, cit, &store))
        {
          unsigned char *c = NULL;
          size_t cl = 0;
          char d[128] = "";
          if (enh_best_of_store (data, len, &store, "optimal",
                                 &c, &cl, d, sizeof d))
            enh_consider (c, cl, d, &best, &best_len,
                          best_desc, sizeof best_desc);
          ZopfliCleanLZ77Store (&store);
        }
      if (enh_parse_fixed (data, len, &store))
        {
          unsigned char *c = NULL;
          size_t cl = 0;
          char d[128] = "";
          if (enh_best_of_store (data, len, &store, "optfixed",
                                 &c, &cl, d, sizeof d))
            enh_consider (c, cl, d, &best, &best_len,
                          best_desc, sizeof best_desc);
          ZopfliCleanLZ77Store (&store);
        }
      if (enh_parse_greedy (data, len, &store))
        {
          unsigned char *c = NULL;
          size_t cl = 0;
          char d[128] = "";
          if (enh_best_of_store (data, len, &store, "greedy",
                                 &c, &cl, d, sizeof d))
            enh_consider (c, cl, d, &best, &best_len,
                          best_desc, sizeof best_desc);
          ZopfliCleanLZ77Store (&store);
        }
    }

  if (!best)
    return false;
  *out = best;
  *out_len = best_len;
  if (desc && desc_sz > 0)
    {
      strncpy (desc, best_desc, desc_sz - 1);
      desc[desc_sz - 1] = '\0';
    }
  return true;
}
