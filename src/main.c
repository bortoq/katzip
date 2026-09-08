#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "archiver.h"
#include "config.h"

static void
print_usage (void)
{
  fprintf (stderr,
           "KATZip v1.0 - Deflating with extreme devotion.\n"
           "Dedicated to the memory of Phil Katz (1962-2000), the father of ZIP.\n"
           "\n"
           "Usage:   katzip <archive.zip> <input_files...>\n"
           "Example: katzip APPNOTE APPNOTE.TXT\n"
            "Config:  $KATZIP_INI, ./katzip.ini, or <binary-dir>/katzip.ini.\n");
}

/* Append .zip if missing (case-insensitive). Caller must free. */
static char *
ensure_zip_extension (const char *archive)
{
  size_t len;

  if (!archive)
    return NULL;

  len = strlen (archive);

  if (len >= 4 && strcasecmp (archive + len - 4, ".zip") == 0)
    return strdup (archive);

  {
    char *out = malloc (len + 5);
    if (!out)
      return NULL;

    memcpy (out, archive, len);
    memcpy (out + len, ".zip", 5);
    return out;
  }
}

int
main (int argc, char *argv[])
{
  char *archive;
  char **files;
  size_t nfiles;
  bool ok;

  if (argc < 2)
    {
      print_usage ();
      return 1;
    }

  if (strcmp (argv[1], "--help") == 0
      || strcmp (argv[1], "-h") == 0
      || strcmp (argv[1], "-?") == 0)
    {
      print_usage ();
      return 0;
    }
  if (strcmp (argv[1], "--version") == 0
      || strcmp (argv[1], "-V") == 0)
    {
      fprintf (stderr, "katzip 1.0 (DEFLATE-only)\n");
      return 0;
    }
  /* Archive name starting with '-' is ambiguous — require ./ prefix. */
  if (argv[1][0] == '-')
    {
      fprintf (stderr, "archive name '%s' looks like an option; use ./ prefix\n", argv[1]);
      print_usage ();
      return 1;
    }

  if (argc < 3)
    {
      print_usage ();
      return 1;
    }

  /* Reject any option-like arguments: only help is allowed. */
  for (int i = 2; i < argc; i++)
    if (argv[i][0] == '-')
      {
        fprintf (stderr, "unknown option %s\n", argv[i]);
        print_usage ();
        return 1;
      }

  {
    const char *src = config_source ();
    fprintf (stderr, "config: %s\n", src ? src : "(built-in defaults)");
    fflush (stderr);
  }

  archive = ensure_zip_extension (argv[1]);
  if (!archive)
    {
      fprintf (stderr, "memory error\n");
      return 1;
    }

  files = &argv[2];
  nfiles = (size_t) (argc - 2);

  ok = create_zip_archive (archive, files, nfiles);

  if (!ok)
    fprintf (stderr, "error: failed to create %s\n", archive);

  free (archive);
  return ok ? 0 : 1;
}
