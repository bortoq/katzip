#include "katzip_internal.h"

static void print_help(FILE *stream)
{
  fputs("KATZip v1.1 — Deflating with extreme devotion.\n"
    "Dedicated to the memory of Phil Katz (1962–2000), the father of ZIP.\n"
    "\n"
    "Usage:\n"
    "  katzip [options] <archive[.zip]> [input ...]\n"
    "\n"
    "Options:\n"
    "  -1 ... -9   Compression level (default: -7).\n"
    "  -r          Search directories recursively.\n"
    "  -h, --help  Show this help.\n"
    "  --          Treat all following arguments as input names.\n"
    "\n"
    "Input:\n"
    "  file        Add a file.\n"
    "  directory   Add files from this directory with -r.\n"
    "  @mask       Add files matching a mask; with -r, search subdirectories.\n"
    "              Multiple masks may be supplied.\n"
    "\n"
    "If input is omitted, @* is used. If the archive name has no extension,\n"
    ".zip is added. Options may appear anywhere before --.\n"
    "\n"
    "Examples:\n"
    "  katzip archive report.txt\n"
    "  katzip -r backup documents @*.txt\n"
    "  katzip texts @*.txt @*.fb2 -9\n"
    "  katzip archive -- -report.txt\n"
    "\n"
    "Environment:\n"
    "  KATZIP_INI  Path to a specific compression settings file.\n", stream);
}
static int level_flag(const char *argument)
{
  if(argument[0] != '-' || argument[1] < '1' ||
    argument[1] > '9' || argument[2])
    return 0;
  return argument[1] - '0';
}

static int consume_level_or_unknown(const char *argument,
  OPTIONS *options)
{
  int level = level_flag(argument);
  if(level)
  {
    options->level = level;
    return 1;
  }
  if(argument[0] != '-')
    return 0;
  fprintf(stderr, "katzip: unknown option: %s\n", argument);
  return -1;
}

/* 0 means a positional argument; 1 means a consumed option. */
static int consume_option(const char *argument, OPTIONS *options,
  int *options_done)
{
  if(*options_done)
    return 0;
  if(strcmp(argument, "--") == 0)
  {
    *options_done = 1;
    return 1;
  }
  if(strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0)
  {
    print_help(stdout);
    return 2;
  }
  if(strcmp(argument, "-r") == 0)
  {
    options->recursive = 1;
    return 1;
  }
  return consume_level_or_unknown(argument, options);
}

static int parse_command_argument(char **argv, int index,
  OPTIONS *options, int *options_done, int *next_position)
{
  int action = consume_option(argv[index], options, options_done);
  if(action == 2)
    return 1;
  if(action < 0)
    return -1;
  if(!action)
    argv[(*next_position)++] = argv[index];
  return 0;
}

int parse_options(int argc, char **argv, OPTIONS *options)
{
  int next_position = 1;
  int options_done = 0;
  int i;
  options->level = 7;
  options->recursive = 0;
  options->archive_arg = 1;
  for(i = 1; i < argc; ++i)
  {
    int result = parse_command_argument(argv, i, options,
      &options_done, &next_position);
    if(result)
      return result;
  }
  options->argument_count = next_position;
  if(next_position == options->archive_arg)
  {
    print_help(stderr);
    return -1;
  }
  return 0;
}

/* Read masks before visiting explicit files or directories. */
