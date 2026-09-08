#include "policy.h"
#include "config.h"

#include <math.h>
#include <string.h>
#include <strings.h>

bool
policy_is_incompressible_extension (const char *filename)
{
  const char *dot;
  int i;

  if (!filename || *filename == '\0')
    return false;

  dot = strrchr (filename, '.');
  if (!dot)
    return false;

  const katzip_config_t *cfg = config_get ();

  if (!cfg)
    return false;
  /* Config entries carry no leading dot (stripped at load). */
  for (i = 0; i < cfg->n_skip_ext; i++)
    if (strcasecmp (dot + 1, cfg->skip_ext[i]) == 0)
      return true;

  return false;
}

double
policy_entropy (const unsigned char *data, size_t len)
{
  size_t freq[256] = {0};
  size_t n;
  size_t i;
  double entropy = 0.0;

  const katzip_config_t *cfg = config_get ();
  size_t sample = cfg ? cfg->entropy_sample : 32768;

  if (!data || len == 0)
    return 0.0;

  if (sample == 0)
    sample = 32768;
  n = len > sample ? sample : len;

  for (i = 0; i < n; i++)
    freq[data[i]]++;

  for (i = 0; i < 256; i++)
    {
      if (freq[i] == 0)
        continue;
      {
        double p = (double) freq[i] / (double) n;
        entropy -= p * (log (p) / log (2.0));
      }
    }

  return entropy;
}

bool
policy_is_high_entropy (const unsigned char *data, size_t len)
{
  const katzip_config_t *cfg = config_get ();

  if (!data || len < 256)
    return false;
  if (!cfg)
    return policy_entropy (data, len) > 7.85;

  return policy_entropy (data, len) > cfg->entropy_limit;
}

bool
policy_should_store_only (const char *filename,
                          const unsigned char *data,
                          size_t len)
{
  if (!data)
    return true;
  if (len == 0)
    return true;
  const katzip_config_t *cfg = config_get ();

  if (policy_is_incompressible_extension (filename))
    return true;
  if (cfg && !cfg->store_high_entropy)
    return false;
  if (policy_is_high_entropy (data, len))
    return true;

  return false;
}

bool
policy_zopfli_allowed (const unsigned char *data, size_t len)
{
  const katzip_config_t *cfg = config_get ();

  if (!data)
    return false;
  if (!cfg)
    return len >= 4096 && len <= 32UL * 1024UL * 1024UL;
  if (len < cfg->small_file_limit)
    return false;
  if (len > cfg->zopfli_size_limit)
    return false;

  return true;
}
