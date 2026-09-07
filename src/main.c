#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "archiver.h"

static void
print_usage (void)
{
  fprintf (stderr,
           "Usage: katzip <archive.zip> <files...>\n"
           "Example: katzip archive file.txt\n");
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
