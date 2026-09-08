#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "enhanced.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); failures++; } else { printf("ok: %s\n", msg); } } while (0)

/* inflate check independent of enhanced.c internals */
static int roundtrip_ok(const unsigned char *plain, size_t plen,
                        const unsigned char *comp, size_t clen) {
  unsigned char *tmp = malloc(plen ? plen : 1);
  z_stream s; int ret; int ok = 0;
  if (!tmp) return 0;
  memset(&s, 0, sizeof s);
  s.next_in = (unsigned char*)comp; s.avail_in = (unsigned)clen;
  s.next_out = tmp; s.avail_out = (unsigned)plen;
  if (inflateInit2(&s, -15) != Z_OK) { free(tmp); return 0; }
  ret = inflate(&s, Z_FINISH);
  ok = (ret == Z_STREAM_END && s.total_out == plen && memcmp(tmp, plain, plen) == 0);
  inflateEnd(&s); free(tmp);
  return ok;
}

int main(void) {
  /* guards */
  { unsigned char *o=NULL; size_t ol=0; char d[64];
    CHECK(!enhanced_compress(NULL, 10, &o, &ol, d, sizeof d), "NULL data -> false");
    unsigned char x[4] = {1,2,3,4};
    CHECK(!enhanced_compress(x, 4, NULL, &ol, d, sizeof d), "NULL out -> false");
    CHECK(!enhanced_compress(x, 4, &o, NULL, d, sizeof d), "NULL out_len -> false");
    CHECK(!enhanced_compress(x, 0, &o, &ol, d, sizeof d), "len 0 -> false");
    CHECK(!enhanced_compress(NULL, 0, NULL, NULL, NULL, 0), "all NULL -> false");
    /* NULL desc must still work */
    CHECK(enhanced_compress((unsigned char*)"hello hello hello hello hello hello hello hello", 47, &o, &ol, NULL, 0) && o && ol,
          "NULL desc allowed");
    free(o);
  }
  /* budget boundaries */
  CHECK(enhanced_budget_for_size(1) == 1000, "budget tiny=1000");
  CHECK(enhanced_budget_for_size(64*1024) == 1000, "budget 64K=1000");
  CHECK(enhanced_budget_for_size(64*1024+1) == 200, "budget 64K+1=200");
  CHECK(enhanced_budget_for_size(256*1024) == 200, "budget 256K=200");
  CHECK(enhanced_budget_for_size(256*1024+1) == 60, "budget 256K+1=60");
  CHECK(enhanced_budget_for_size(1024*1024) == 60, "budget 1M=60");
  CHECK(enhanced_budget_for_size(1024*1024+1) == 15, "budget 1M+1=15");
  /* repetitive: must compress and roundtrip */
  { size_t n = 20000; unsigned char *in = malloc(n); unsigned char *o=NULL; size_t ol=0; char d[256]={0};
    memset(in, 'a', n);
    CHECK(enhanced_compress(in, n, &o, &ol, d, sizeof d), "repetitive compresses");
    if (o) {
      CHECK(ol < n, "repetitive smaller than input");
      CHECK(roundtrip_ok(in, n, o, ol), "repetitive roundtrip");
      CHECK(d[0] != '\0', "desc non-empty");
      printf("  desc: %s (%zu -> %zu)\n", d, n, ol);
      free(o);
    }
    free(in);
  }
  /* lorem-like text */
  { const char *frag = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor. ";
    size_t fl = strlen(frag), n = fl*500; unsigned char *in = malloc(n);
    for (size_t i=0;i<500;i++) memcpy(in+i*fl, frag, fl);
    unsigned char *o=NULL; size_t ol=0; char d[256]={0};
    CHECK(enhanced_compress(in, n, &o, &ol, d, sizeof d), "lorem compresses");
    if (o) {
      CHECK(roundtrip_ok(in, n, o, ol), "lorem roundtrip");
      printf("  desc: %s (%zu -> %zu)\n", d, n, ol);
      free(o);
    }
    free(in);
  }
  /* single byte + tiny */
  { unsigned char in[1] = {'x'}; unsigned char *o=NULL; size_t ol=0; char d[64]={0};
    int r = enhanced_compress(in, 1, &o, &ol, d, sizeof d);
    CHECK(r ? (roundtrip_ok(in,1,o,ol)) : 1, "single byte roundtrip-or-false");
    if (r) free(o);
  }
  /* random incompressible: must not crash; if it returns true, roundtrip must hold */
  { size_t n = 5000; unsigned char *in = malloc(n); unsigned char *o=NULL; size_t ol=0; char d[64]={0};
    unsigned seed = 12345;
    for (size_t i=0;i<n;i++) { seed = seed*1103515245+12345; in[i] = (unsigned char)(seed>>16); }
    int r = enhanced_compress(in, n, &o, &ol, d, sizeof d);
    CHECK(r ? roundtrip_ok(in,n,o,ol) : 1, "random roundtrip-or-false");
    printf("  random: %s (compressed=%d)\n", d, r);
    if (r) free(o);
    free(in);
  }
  if (failures) { printf("FAILURES: %d\n", failures); return 1; }
  printf("ALL ENHANCED UNIT TESTS PASSED\n");
  return 0;
}
