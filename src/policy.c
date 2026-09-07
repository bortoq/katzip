#include "policy.h"

#include <math.h>
#include <string.h>
#include <strings.h>

/* Already compressed media and archives.
   PDF is intentionally not listed: many PDFs are text and compress well. */
static const char *skip_extensions[] =
  {
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif",
    ".mp4", ".mkv", ".avi", ".mov", ".mp3", ".ogg", ".flac",
    ".zip", ".gz", ".bz2", ".xz", ".7z", ".zst", ".rar",
    ".woff", ".woff2", ".mpg", ".mpeg", ".webm", ".opus",
    NULL
  };

/* Maximum bytes sampled for entropy estimation. */
static const size_t ENTROPY_SAMPLE_LIMIT = 32768;

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

  for (i = 0; skip_extensions[i] != NULL; i++)
    if (strcasecmp (dot, skip_extensions[i]) == 0)
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

  if (!data || len == 0)
    return 0.0;

  n = len > ENTROPY_SAMPLE_LIMIT ? ENTROPY_SAMPLE_LIMIT : len;

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
  if (!data || len < 256)
    return false;

  return policy_entropy (data, len) > POLICY_ENTROPY_LIMIT;
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
  if (policy_is_incompressible_extension (filename))
    return true;
  if (policy_is_high_entropy (data, len))
    return true;

  return false;
}

bool
policy_zopfli_allowed (const unsigned char *data, size_t len)
{
  if (!data)
    return false;
  if (len < POLICY_SMALL_FILE_LIMIT)
    return false;
  if (len > POLICY_ZOPFLI_SIZE_LIMIT)
    return false;

  return true;
}
