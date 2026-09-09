#include "config.h"

#include <ctype.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static katzip_config_t g_cfg;
static int g_loaded = 0;
static char g_source[1024] = "";

static const char *default_extensions[] =
  {
    "jpg", "jpeg", "png", "gif", "webp", "avif",
    "mp4", "mkv", "avi", "mov", "mp3", "ogg", "flac",
    "zip", "gz", "bz2", "xz", "7z", "zst", "rar",
    "woff", "woff2", "mpg", "mpeg", "webm", "opus",
    NULL
  };

void
config_defaults (katzip_config_t *cfg)
{
  int i;

  if (!cfg)
    return;
  memset (cfg, 0, sizeof *cfg);

  cfg->small_file_limit = 4096;
  cfg->zopfli_size_limit = 32UL * 1024UL * 1024UL;
  cfg->entropy_limit = 7.85;
  cfg->entropy_sample = 32768;
  cfg->store_high_entropy = 1;
  cfg->try_gated = 1;
  for (i = 0; default_extensions[i] != NULL
              && i < KATZIP_MAX_SKIP_EXT; i++)
    {
      strncpy (cfg->skip_ext[i], default_extensions[i], KATZIP_EXT_LEN - 1);
      cfg->skip_ext[i][KATZIP_EXT_LEN - 1] = '\0';
    }
  cfg->n_skip_ext = i;

  cfg->zlib_enabled = 1;
  cfg->zlib_min_level = 1;
  cfg->zlib_max_level = 9;
  cfg->zlib_s_default = 1;
  cfg->zlib_s_filtered = 1;
  cfg->zlib_s_huffman = 1;
  cfg->zlib_s_rle = 1;
  cfg->zlib_s_fixed = 1;
  cfg->zlib_full_grid_max = 1024UL * 1024UL;
  cfg->zlib_extra_retry = 0;
  cfg->zlib_extra_retry_max = 64UL * 1024UL;

  cfg->libdeflate_enabled = 1;
  cfg->libdeflate_min_level = 1;
  cfg->libdeflate_max_level = 12;

  cfg->zopfli_enabled = 1;
  cfg->zopfli_tiny_max = 64UL * 1024UL;
  cfg->zopfli_iter_tiny = 1000;
  cfg->zopfli_small_max = 256UL * 1024UL;
  cfg->zopfli_iter_small = 200;
  cfg->zopfli_med_max = 1024UL * 1024UL;
  cfg->zopfli_iter_medium = 60;
  cfg->zopfli_iter_large = 15;
  cfg->zopfli_splitmax[0] = 15;
  cfg->zopfli_splitmax[1] = 0;
  cfg->zopfli_n_splitmax = 2;
  cfg->zopfli_last[0] = 0;
  cfg->zopfli_n_last = 1;
  cfg->zopfli_nosplit_max = 64UL * 1024UL;

  cfg->enh_enabled = 1;
  cfg->enh_fixed = 1;
  cfg->enh_nosplit = 1;
  cfg->enh_split5 = 1;
  cfg->enh_split5_max = 5;
  cfg->enh_singleblock = 1;
  cfg->enh_singleblock_max = 64UL * 1024UL;
  cfg->enh_custom_iter_cap = 200;
  cfg->enh_split15 = 1;
  cfg->enh_merge_blocks = 1;
  cfg->enh_kzip_split = 1;
  cfg->enh_recode_iters = 500;
  cfg->threads = 0;
}

/* ------------------------------------------------------------------ */
/* Tiny ini parser (no dependencies, fenced everywhere). */

static char *
trim (char *s)
{
  char *end;
  size_t n;

  if (!s)
    return s;
  while (*s && isspace ((unsigned char) *s))
    s++;
  n = strlen (s);
  while (n > 0 && isspace ((unsigned char) s[n - 1]))
    s[--n] = '\0';
  end = s;
  return end;
}

static int
parse_bool (const char *v, int *out)
{
  char buf[16];
  size_t i;

  if (!v || !out)
    return 0;
  for (i = 0; i + 1 < sizeof buf && v[i]; i++)
    buf[i] = (char) tolower ((unsigned char) v[i]);
  buf[i] = '\0';
  if (!strcmp (buf, "1") || !strcmp (buf, "true") || !strcmp (buf, "on")
      || !strcmp (buf, "yes"))
    {
      *out = 1;
      return 1;
    }
  if (!strcmp (buf, "0") || !strcmp (buf, "false") || !strcmp (buf, "off")
      || !strcmp (buf, "no"))
    {
      *out = 0;
      return 1;
    }
  return 0;
}

/* Integers with optional K/M/G suffix. */
static int
parse_size (const char *v, size_t *out)
{
  char *end;
  unsigned long long n;
  unsigned long long mul = 1;

  if (!v || !*v || !out)
    return 0;
  n = strtoull (v, &end, 10);
  if (end == v)
    return 0;
  if (*end == 'k' || *end == 'K')
    {
      mul = 1024ULL;
      end++;
    }
  else if (*end == 'm' || *end == 'M')
    {
      mul = 1024ULL * 1024ULL;
      end++;
    }
  else if (*end == 'g' || *end == 'G')
    {
      mul = 1024ULL * 1024ULL * 1024ULL;
      end++;
    }
  if (*end == 'b' || *end == 'B')
    end++;
  if (*end != '\0')
    return 0;
  *out = (size_t) (n * mul);
  return 1;
}

static int
parse_int (const char *v, int *out)
{
  char *end;
  long n;

  if (!v || !*v || !out)
    return 0;
  n = strtol (v, &end, 10);
  if (end == v || *end != '\0')
    return 0;
  *out = (int) n;
  return 1;
}

static int
parse_double (const char *v, double *out)
{
  char *end;
  double d;

  if (!v || !*v || !out)
    return 0;
  d = strtod (v, &end);
  if (end == v || *end != '\0')
    return 0;
  *out = d;
  return 1;
}

static int
parse_int_list (const char *v, int *arr, int cap)
{
  char buf[256];
  int n = 0;
  char *tok;

  if (!v || !arr || cap <= 0)
    return -1;
  strncpy (buf, v, sizeof buf - 1);
  buf[sizeof buf - 1] = '\0';
  for (tok = strtok (buf, ","); tok && n < cap;
       tok = strtok (NULL, ","))
    {
      int x;
      tok = trim (tok);
      if (!parse_int (tok, &x))
        return -1;
      arr[n++] = x;
    }
  return n;
}

static void
set_policy_key (katzip_config_t *cfg, const char *key, const char *val)
{
  int b;
  size_t z;
  double d;

  if (!cfg || !key || !val)
    return;
  if (!strcmp (key, "small_file_limit") && parse_size (val, &z))
    cfg->small_file_limit = z;
  else if (!strcmp (key, "zopfli_size_limit") && parse_size (val, &z))
    cfg->zopfli_size_limit = z;
  else if (!strcmp (key, "entropy_limit") && parse_double (val, &d))
    {
      if (d >= 0.0 && d <= 8.0)
        cfg->entropy_limit = d;
    }
  else if (!strcmp (key, "entropy_sample") && parse_size (val, &z))
    cfg->entropy_sample = z;
  else if (!strcmp (key, "store_high_entropy") && parse_bool (val, &b))
    cfg->store_high_entropy = b;
  else if (!strcmp (key, "try_gated") && parse_bool (val, &b))
    cfg->try_gated = b;
  else if (!strcmp (key, "skip_extensions"))
    {
      char buf[512];
      char *tok;
      cfg->n_skip_ext = 0;
      strncpy (buf, val, sizeof buf - 1);
      buf[sizeof buf - 1] = '\0';
      for (tok = strtok (buf, ",");
           tok && cfg->n_skip_ext < KATZIP_MAX_SKIP_EXT;
           tok = strtok (NULL, ","))
        {
          tok = trim (tok);
          if (*tok == '.')
            tok++;
          if (*tok == '\0')
            continue;
          strncpy (cfg->skip_ext[cfg->n_skip_ext], tok,
                   KATZIP_EXT_LEN - 1);
          cfg->skip_ext[cfg->n_skip_ext][KATZIP_EXT_LEN - 1] = '\0';
          cfg->n_skip_ext++;
        }
    }
}

static void
set_zlib_key (katzip_config_t *cfg, const char *key, const char *val)
{
  int b;
  int x;
  size_t z;

  if (!cfg || !key || !val)
    return;
  if (!strcmp (key, "enabled") && parse_bool (val, &b))
    cfg->zlib_enabled = b;
  else if (!strcmp (key, "min_level") && parse_int (val, &x))
    cfg->zlib_min_level = x;
  else if (!strcmp (key, "max_level") && parse_int (val, &x))
    cfg->zlib_max_level = x;
  else if (!strcmp (key, "full_grid_max_size") && parse_size (val, &z))
    cfg->zlib_full_grid_max = z;
  else if (!strcmp (key, "extra_retry") && parse_bool (val, &b))
    cfg->zlib_extra_retry = b;
  else if (!strcmp (key, "extra_retry_max_size") && parse_size (val, &z))
    cfg->zlib_extra_retry_max = z;
  else if (!strcmp (key, "strategies"))
    {
      char buf[256];
      char *tok;
      cfg->zlib_s_default = cfg->zlib_s_filtered = 0;
      cfg->zlib_s_huffman = cfg->zlib_s_rle = 0;
      cfg->zlib_s_fixed = 0;
      strncpy (buf, val, sizeof buf - 1);
      buf[sizeof buf - 1] = '\0';
      for (tok = strtok (buf, ","); tok; tok = strtok (NULL, ","))
        {
          char low[32];
          size_t i;
          tok = trim (tok);
          for (i = 0; i + 1 < sizeof low && tok[i]; i++)
            low[i] = (char) tolower ((unsigned char) tok[i]);
          low[i] = '\0';
          if (!strcmp (low, "default"))
            cfg->zlib_s_default = 1;
          else if (!strcmp (low, "filtered"))
            cfg->zlib_s_filtered = 1;
          else if (!strcmp (low, "huffman_only") || !strcmp (low, "huffman"))
            cfg->zlib_s_huffman = 1;
          else if (!strcmp (low, "rle"))
            cfg->zlib_s_rle = 1;
          else if (!strcmp (low, "fixed"))
            cfg->zlib_s_fixed = 1;
        }
    }
}

static void
set_libdeflate_key (katzip_config_t *cfg, const char *key, const char *val)
{
  int b;
  int x;

  if (!cfg || !key || !val)
    return;
  if (!strcmp (key, "enabled") && parse_bool (val, &b))
    cfg->libdeflate_enabled = b;
  else if (!strcmp (key, "min_level") && parse_int (val, &x))
    cfg->libdeflate_min_level = x;
  else if (!strcmp (key, "max_level") && parse_int (val, &x))
    cfg->libdeflate_max_level = x;
}

static void
set_zopfli_key (katzip_config_t *cfg, const char *key, const char *val)
{
  int b;
  int x;
  size_t z;
  int tmp[8];
  int n;

  if (!cfg || !key || !val)
    return;
  if (!strcmp (key, "enabled") && parse_bool (val, &b))
    cfg->zopfli_enabled = b;
  else if (!strcmp (key, "tiny_max_size") && parse_size (val, &z))
    cfg->zopfli_tiny_max = z;
  else if (!strcmp (key, "iter_tiny") && parse_int (val, &x))
    cfg->zopfli_iter_tiny = x;
  else if (!strcmp (key, "small_max_size") && parse_size (val, &z))
    cfg->zopfli_small_max = z;
  else if (!strcmp (key, "iter_small") && parse_int (val, &x))
    cfg->zopfli_iter_small = x;
  else if (!strcmp (key, "med_max_size") && parse_size (val, &z))
    cfg->zopfli_med_max = z;
  else if (!strcmp (key, "iter_medium") && parse_int (val, &x))
    cfg->zopfli_iter_medium = x;
  else if (!strcmp (key, "iter_large") && parse_int (val, &x))
    cfg->zopfli_iter_large = x;
  else if (!strcmp (key, "nosplit_max_size") && parse_size (val, &z))
    cfg->zopfli_nosplit_max = z;
  else if (!strcmp (key, "splitmax_values"))
    {
      n = parse_int_list (val, tmp, 8);
      if (n > 0)
        {
          memcpy (cfg->zopfli_splitmax, tmp, (size_t) n * sizeof tmp[0]);
          cfg->zopfli_n_splitmax = n;
        }
    }
  else if (!strcmp (key, "last_values"))
    {
      n = parse_int_list (val, tmp, 4);
      if (n > 0)
        {
          memcpy (cfg->zopfli_last, tmp, (size_t) n * sizeof tmp[0]);
          cfg->zopfli_n_last = n;
        }
    }
}

static void
set_enhanced_key (katzip_config_t *cfg, const char *key, const char *val)
{
  int b;
  int x;
  size_t z;

  if (!cfg || !key || !val)
    return;
  if (!strcmp (key, "enabled") && parse_bool (val, &b))
    cfg->enh_enabled = b;
  else if (!strcmp (key, "trial_fixed") && parse_bool (val, &b))
    cfg->enh_fixed = b;
  else if (!strcmp (key, "trial_nosplit") && parse_bool (val, &b))
    cfg->enh_nosplit = b;
  else if (!strcmp (key, "trial_split5") && parse_bool (val, &b))
    cfg->enh_split5 = b;
  else if (!strcmp (key, "split5_max") && parse_int (val, &x))
    cfg->enh_split5_max = x;
  else if (!strcmp (key, "singleblock") && parse_bool (val, &b))
    cfg->enh_singleblock = b;
  else if (!strcmp (key, "singleblock_max_size") && parse_size (val, &z))
    cfg->enh_singleblock_max = z;
  else if (!strcmp (key, "custom_iter_cap") && parse_int (val, &x))
    cfg->enh_custom_iter_cap = x;
  else if (!strcmp (key, "split15_recode") && parse_bool (val, &b))
    cfg->enh_split15 = b;
  else if (!strcmp (key, "merge_blocks") && parse_bool (val, &b))
    cfg->enh_merge_blocks = b;
  else if (!strcmp (key, "kzip_split") && parse_bool (val, &b))
    cfg->enh_kzip_split = b;
  else if (!strcmp (key, "recode_iters") && parse_int (val, &x))
    cfg->enh_recode_iters = x;
}

static void
set_key (katzip_config_t *cfg, const char *section,
         const char *key, const char *val)
{
  if (!cfg || !section || !key || !val)
    return;
  if (!strcmp (section, "policy"))
    set_policy_key (cfg, key, val);
  else if (!strcmp (section, "zlib"))
    set_zlib_key (cfg, key, val);
  else if (!strcmp (section, "libdeflate"))
    set_libdeflate_key (cfg, key, val);
  else if (!strcmp (section, "zopfli"))
    set_zopfli_key (cfg, key, val);
  else if (!strcmp (section, "enhanced"))
    set_enhanced_key (cfg, key, val);
  else if (!strcmp (section, "core"))
    {
      int x; if (!strcmp (key, "threads") && parse_int (val, &x)) cfg->threads = x;
    }
}


static void
config_load_file (katzip_config_t *cfg, const char *path)
{
  FILE *f;
  char line[512];
  char section[64] = "";

  if (!cfg || !path || !*path)
    return;
  f = fopen (path, "r");
  if (!f)
    return;
  while (fgets (line, sizeof line, f))
    {
      char *s = trim (line);
      char *eq;
      if (*s == '\0' || *s == '#' || *s == ';')
        continue;
      if (*s == '[')
        {
          char *end = strchr (s, ']');
          size_t i;
          if (!end)
            continue;
          *end = '\0';
          strncpy (section, trim (s + 1), sizeof section - 1);
          section[sizeof section - 1] = '\0';
          for (i = 0; section[i]; i++)
            section[i] = (char) tolower ((unsigned char) section[i]);
          continue;
        }
      eq = strchr (s, '=');
      if (!eq || section[0] == '\0')
        continue;
      *eq = '\0';
      {
        char *key = trim (s);
        char *val = trim (eq + 1);
        char *hash;
        size_t i;
        /* Inline comments. */
        hash = strchr (val, '#');
        if (hash)
          {
            *hash = '\0';
            val = trim (val);
          }
        for (i = 0; key[i]; i++)
          key[i] = (char) tolower ((unsigned char) key[i]);
        set_key (cfg, section, key, val);
      }
    }
  fclose (f);
}

static int
file_readable (const char *path)
{
  FILE *f;

  if (!path || !*path)
    return 0;
  f = fopen (path, "r");
  if (!f)
    return 0;
  fclose (f);
  return 1;
}

/* <binary-dir>/katzip.ini via /proc/self/exe (Linux). Empty = unknown. */
static void
exe_dir_ini (char *buf, size_t bufsz)
{
  char exe[1024];
  ssize_t n;
  char *slash;

  if (!buf || bufsz == 0)
    return;
  buf[0] = '\0';
#ifdef __linux__
  n = readlink ("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0)
    return;
  exe[n] = '\0';
  slash = strrchr (exe, '/');
  if (!slash)
    return;
  if ((size_t) (slash - exe) + strlen ("/katzip.ini") + 1 > bufsz)
    return;
  memcpy (buf, exe, (size_t) (slash - exe));
  buf[slash - exe] = '\0';
  strcat (buf, "/katzip.ini");
#else
  (void) exe;
  (void) n;
  (void) slash;
#endif
}

const katzip_config_t *
config_get (void)
{
  if (!g_loaded)
    {
      const char *env;
      char exepath[1024];
      config_defaults (&g_cfg);
      g_source[0] = '\0';
      env = getenv ("KATZIP_INI");
      if (env && *env && file_readable (env))
        {
          config_load_file (&g_cfg, env);
          snprintf (g_source, sizeof g_source, "%s", env);
        }
      else if (file_readable ("katzip.ini"))
        {
          config_load_file (&g_cfg, "katzip.ini");
          snprintf (g_source, sizeof g_source, "katzip.ini");
        }
      else
        {
          exe_dir_ini (exepath, sizeof exepath);
          if (exepath[0] && file_readable (exepath))
            {
              config_load_file (&g_cfg, exepath);
              snprintf (g_source, sizeof g_source, "%s", exepath);
            }
        }
      g_loaded = 1;
    }
  return &g_cfg;
}

const char *
config_source (void)
{
  (void) config_get ();
  return g_source[0] ? g_source : NULL;
}

void
config_reset (void)
{
  g_loaded = 0;
  g_source[0] = '\0';
}
