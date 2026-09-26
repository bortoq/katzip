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
    "  --full-help Show compression settings and this help.\n"
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

static void print_full_help(void)
{
  static const char *ect_help[] = {
    "LZ77 optimization passes",
    "Reserved ECT preset; unused for raw Deflate",
    "Byte cutoff for skipping dynamic Huffman blocks",
    "Token cutoff for trying fixed Huffman blocks",
    "Byte cutoff for block splitting",
    "LZ77 token cutoff for block splitting",
    "Candidate split positions per round",
    "Huffman header search effort",
    "Reuse the preceding pass's cost model",
    "Cache matches between passes",
    "ECT internal threading; only 0 is allowed",
    "PNG tuning; only 0 is allowed for ZIP",
    "Short-match replacement effort",
    "Run block splitting a second time",
    "Additional LZ77 cost-model refinement",
    "Match length for greedy search",
    "Estimate split cost with Shannon entropy",
    "Optimize Huffman trees and block headers"
  };
  static const char *turtle_help[] = {
    "Turtle effort; levels above 7 enable extra checks",
    "Maximum source bytes per superblock",
    "Maximum Deflate subblocks per superblock",
    "Outer subblock partition passes",
    "Inner split and merge attempts per pass",
    "Candidate positions in coarse split search",
    "Search radius around the best split candidate",
    "Minimum range for sampled split search",
    "Try an additional split when optimization stalls",
    "Lowest initial fixed-point precision",
    "Highest initial fixed-point precision",
    "Number of initial precisions to sample",
    "Diagnostic detail; 0 is quiet"
  };
  size_t i;
  print_help(stdout);
  fputs("\nCompression settings:\n"
    "  The selected [1]...[9] INI section uses the same options as the\n"
    "  command line. The section ends at the next section header.\n"
    "  Command-line settings override the selected section. A compressor\n"
    "  participates when any of its settings appears in that section or\n"
    "  on the command line. Missing settings use compiled defaults.\n"
    "  Multiple compressors compete; the smallest Deflate stream wins.\n"
    "  Use -r in a section to enable recursive search.\n"
    "\n"
    "  --libdeflate_level N  libdeflate effort, 1..12.\n"
    "  --zlib_after SIZE    Use zlib at or above SIZE; off disables it.\n"
    "  --zlib_level N       zlib effort, 1..9.\n"
    "  SIZE accepts B, KiB, MiB, GiB, or plain bytes.\n"
    "\nECT Zopfli options:\n", stdout);
  for(i = 0; i < ARRAY_N(ect_fields); ++i)
    printf("  --zopfli_%s N (%d..%d): %s\n", ect_fields[i].name,
      ect_fields[i].minimum, ect_fields[i].maximum, ect_help[i]);
  fputs("\nTurtledeflate options:\n", stdout);
  for(i = 0; i < ARRAY_N(config_fields); ++i)
    printf("  --turtledeflate_%s N: %s\n", config_fields[i].name,
      turtle_help[i]);
}

static int level_flag(const char *argument)
{
  if(argument[0] != '-' || argument[1] < '1' ||
    argument[1] > '9' || argument[2])
    return 0;
  return argument[1] - '0';
}

/* Find the selected level before reading its INI section. */
int scan_options(int argc, char **argv, OPTIONS *options)
{
  int i;
  int done = 0;
  options->level = DEFAULT_LEVEL;
  options->recursive = 0;
  options->archive_arg = 1;
  for(i = 1; i < argc; ++i)
  {
    int level;
    if(done)
      continue;
    if(strcmp(argv[i], "--") == 0)
    {
      done = 1;
      continue;
    }
    if(strcmp(argv[i], "-h") == 0 ||
      strcmp(argv[i], "--help") == 0)
    {
      print_help(stdout);
      return 1;
    }
    if(strcmp(argv[i], "--full-help") == 0)
    {
      print_full_help();
      return 1;
    }
    level = level_flag(argv[i]);
    if(level)
      options->level = level;
    if(config_option_known(argv[i]))
      ++i;
  }
  return 0;
}

/* Return 0 for an input name, 1 for an option, -1 for an error. */
static int consume_option(int argc, char **argv, int *index,
  OPTIONS *options, COMPRESSION_CONFIG *config, int *done)
{
  const char *argument = argv[*index];
  int level;
  if(*done)
    return 0;
  if(strcmp(argument, "--") == 0)
  {
    *done = 1;
    return 1;
  }
  if(strcmp(argument, "-r") == 0)
  {
    options->recursive = 1;
    return 1;
  }
  level = level_flag(argument);
  if(level)
    return 1;
  if(config_option_known(argument))
  {
    if(*index + 1 >= argc ||
      apply_config_option(argument, argv[++*index], config))
    {
      fprintf(stderr, "katzip: invalid value for %s\n", argument);
      return -1;
    }
    return 1;
  }
  if(argument[0] != '-')
    return 0;
  fprintf(stderr, "katzip: unknown option: %s\n", argument);
  return -1;
}

int parse_options(int argc, char **argv, OPTIONS *options,
  COMPRESSION_CONFIG *config)
{
  int next_position = 1;
  int done = 0;
  int i;
  for(i = 1; i < argc; ++i)
  {
    int action = consume_option(argc, argv, &i, options, config, &done);
    if(action < 0)
      return -1;
    if(action == 0)
      argv[next_position++] = argv[i];
  }
  options->argument_count = next_position;
  if(next_position > options->archive_arg)
    return 0;
  print_help(stderr);
  return -1;
}
