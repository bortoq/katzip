#include "katzip_internal.h"

#define DEFAULT_ZLIB_AFTER (64U * 1024U * 1024U)

/* Presets are the only source for both compression and generated INI files. */
static const int fast_defaults[] = {1, 2, 3, 5, 6, 8};

/* ECT modes 7 and 9, expanded from ZopfliInitOptions(mode, 0, 0). */
static const ZopfliOptions ect_defaults[] = {
  {
    .numiterations = 13,
    .filter_style = 1,
    .skipdynamic = 80,
    .trystatic = 1800,
    .noblocksplit = 1000,
    .noblocksplitlz = 200,
    .num = 9,
    .searchext = 1,
    .reuse_costmodel = 1,
    .useCache = 1,
    .multithreading = 0,
    .isPNG = 0,
    .replaceCodes = 1001,
    .twice = 0,
    .ultra = 1,
    .greed = 258,
    .entropysplit = 0,
    .advanced = 1
  },
  {
    .numiterations = 60,
    .filter_style = 3,
    .skipdynamic = 80,
    .trystatic = 3000,
    .noblocksplit = 800,
    .noblocksplitlz = 100,
    .num = 9,
    .searchext = 2,
    .reuse_costmodel = 1,
    .useCache = 1,
    .multithreading = 0,
    .isPNG = 0,
    .replaceCodes = 1001,
    .twice = 0,
    .ultra = 1,
    .greed = 258,
    .entropysplit = 0,
    .advanced = 1
  },
  {
    .numiterations = 60,
    .filter_style = 3,
    .skipdynamic = 80,
    .trystatic = 3000,
    .noblocksplit = 800,
    .noblocksplitlz = 100,
    .num = 9,
    .searchext = 2,
    .reuse_costmodel = 1,
    .useCache = 1,
    .multithreading = 0,
    .isPNG = 0,
    .replaceCodes = 1001,
    .twice = 0,
    .ultra = 1,
    .greed = 258,
    .entropysplit = 0,
    .advanced = 1
  }
};

static const turtledeflate_config_t turtle_defaults[] = {
  {
    .i_compression_level = 9,
    .i_maximum_block_size = 1000000,
    .i_maximum_subblocks = 512,
    .i_max_block_splitter_iterations = 30,
    .i_max_internal_block_splitter_iterations = 100,
    .i_block_splitter_num_points = 31,
    .i_block_splitter_center_dist = 8,
    .i_block_splitter_min_range_for_points = 1024,
    .b_block_splitter_push_split = true,
    .i_min_start_fp = -6,
    .i_max_start_fp = 5,
    .i_num_start_fp = 16,
    .i_verbose = 0
  }
};

/* The table also assigns one bit to each required Turtledeflate setting. */
const CONFIG_FIELD config_fields[13] = {
  {"i_compression_level", offsetof(turtledeflate_config_t, i_compression_level), 0},
  {"i_maximum_block_size", offsetof(turtledeflate_config_t, i_maximum_block_size), 0},
  {"i_maximum_subblocks", offsetof(turtledeflate_config_t, i_maximum_subblocks), 0},
  {"i_max_block_splitter_iterations",
    offsetof(turtledeflate_config_t, i_max_block_splitter_iterations), 0},
  {"i_max_internal_block_splitter_iterations",
    offsetof(turtledeflate_config_t, i_max_internal_block_splitter_iterations), 0},
  {"i_block_splitter_num_points", offsetof(turtledeflate_config_t, i_block_splitter_num_points), 0},
  {"i_block_splitter_center_dist",
    offsetof(turtledeflate_config_t, i_block_splitter_center_dist), 0},
  {"i_block_splitter_min_range_for_points",
    offsetof(turtledeflate_config_t, i_block_splitter_min_range_for_points), 0},
  {"b_block_splitter_push_split", offsetof(turtledeflate_config_t, b_block_splitter_push_split), 1},
  {"i_min_start_fp", offsetof(turtledeflate_config_t, i_min_start_fp), 0},
  {"i_max_start_fp", offsetof(turtledeflate_config_t, i_max_start_fp), 0},
  {"i_num_start_fp", offsetof(turtledeflate_config_t, i_num_start_fp), 0},
  {"i_verbose", offsetof(turtledeflate_config_t, i_verbose), 0}
};

/* These are the public ECT ZopfliOptions fields, in struct order. */
const ECT_FIELD ect_fields[18] = {
  {"numiterations", offsetof(ZopfliOptions, numiterations), 1, 1000},
  {"filter_style", offsetof(ZopfliOptions, filter_style), 0, 3},
  {"skipdynamic", offsetof(ZopfliOptions, skipdynamic), 0, 1000000},
  {"trystatic", offsetof(ZopfliOptions, trystatic), 0, 1000000},
  {"noblocksplit", offsetof(ZopfliOptions, noblocksplit), 0, 1000000},
  {"noblocksplitlz", offsetof(ZopfliOptions, noblocksplitlz), 0, 1000000},
  {"num", offsetof(ZopfliOptions, num), 1, 64},
  {"searchext", offsetof(ZopfliOptions, searchext), 0, 2},
  {"reuse_costmodel", offsetof(ZopfliOptions, reuse_costmodel), 0, 1},
  {"useCache", offsetof(ZopfliOptions, useCache), 0, 1},
  {"multithreading", offsetof(ZopfliOptions, multithreading), 0, 0},
  {"isPNG", offsetof(ZopfliOptions, isPNG), 0, 0},
  {"replaceCodes", offsetof(ZopfliOptions, replaceCodes), 0, 100000},
  {"twice", offsetof(ZopfliOptions, twice), 0, 1},
  {"ultra", offsetof(ZopfliOptions, ultra), 0, 3},
  {"greed", offsetof(ZopfliOptions, greed), 0, 258},
  {"entropysplit", offsetof(ZopfliOptions, entropysplit), 0, 1},
  {"advanced", offsetof(ZopfliOptions, advanced), 0, 1}
};
void use_default_config(int level, COMPRESSION_CONFIG *config)
{
  memset(config, 0, sizeof(*config));
  config->zlib_level = level;
  config->zlib_after = UINT64_MAX;
  if(level <= (int)ARRAY_N(fast_defaults))
  {
    config->have_fast = 1;
    config->fast_level = fast_defaults[level - 1];
    config->zlib_level = fast_defaults[level - 1] > 9 ?
      9 : fast_defaults[level - 1];
    config->zlib_after = DEFAULT_ZLIB_AFTER;
  }
  else
  {
    config->have_ect = 1;
    config->ect = ect_defaults[level - 7];
  }
  if(level == 9)
  {
    config->have_turtle = 1;
    config->turtle = turtle_defaults[0];
  }
}

static int default_field_value(const turtledeflate_config_t *config,
  const CONFIG_FIELD *field)
{
  const unsigned char *address = (const unsigned char*)config + field->offset;
  if(field->boolean)
    return *(const bool*)address ? 1 : 0;
  return *(const int32_t*)address;
}

static int default_ect_value(const ZopfliOptions *options,
  const ECT_FIELD *field)
{
  const unsigned char *address = (const unsigned char*)options + field->offset;
  if(field == &ect_fields[0])
    return *(const int*)address;
  return (int)*(const unsigned*)address;
}

static int write_ect_settings(FILE *file, const ZopfliOptions *options)
{
  size_t field;
  for(field = 0; field < ARRAY_N(ect_fields); ++field)
    if(fprintf(file, "zopfli_%s = %d\n", ect_fields[field].name,
      default_ect_value(options, &ect_fields[field])) < 0)
      return -1;
  return 0;
}

static int write_turtle_settings(FILE *file,
  const turtledeflate_config_t *config)
{
  size_t field;
  for(field = 0; field < ARRAY_N(config_fields); ++field)
    if(fprintf(file, "turtledeflate_%s = %d\n",
      config_fields[field].name,
      default_field_value(config, &config_fields[field])) < 0)
      return -1;
  return 0;
}

static int write_zlib_threshold(FILE *file, const COMPRESSION_CONFIG *config)
{
  if(config->zlib_after == UINT64_MAX)
    return fputs("zlib_after = off\n", file) == EOF ? -1 : 0;
  return fprintf(file, "zlib_after = %llu\n",
    (unsigned long long)config->zlib_after) < 0 ? -1 : 0;
}

static int write_default_fast(FILE *file,
  const COMPRESSION_CONFIG *config)
{
  if(!config->have_fast)
    return 0;
  return fprintf(file, "libdeflate_level = %d\n",
    config->fast_level) < 0 ? -1 : 0;
}

static int write_default_ect(FILE *file,
  const COMPRESSION_CONFIG *config)
{
  if(!config->have_ect)
    return 0;
  return write_ect_settings(file, &config->ect);
}

static int write_default_turtle(FILE *file,
  const COMPRESSION_CONFIG *config)
{
  if(!config->have_turtle)
    return 0;
  return write_turtle_settings(file, &config->turtle);
}

static int write_default_engines(FILE *file,
  const COMPRESSION_CONFIG *config)
{
  if(write_default_fast(file, config))
    return -1;
  if(write_default_ect(file, config))
    return -1;
  return write_default_turtle(file, config);
}

static int write_default_section(FILE *file, int level)
{
  COMPRESSION_CONFIG config;
  use_default_config(level, &config);
  if(fprintf(file, "[%d]\n", level) < 0)
    return -1;
  if(write_default_engines(file, &config))
    return -1;
  if(write_zlib_threshold(file, &config))
    return -1;
  return fprintf(file, "zlib_level = %d\n", config.zlib_level) < 0 ? -1 : 0;
}

static int write_section_separator(FILE *file, int level)
{
  if(level == 9)
    return 0;
  return fputc('\n', file) == EOF ? -1 : 0;
}

static int write_default_sections(FILE *file)
{
  int level;
  for(level = 1; level <= 9; ++level)
  {
    if(write_default_section(file, level))
      return -1;
    if(write_section_separator(file, level))
      return -1;
  }
  return 0;
}

/* Generate the editable file from the same tables as the fallback. */
int write_default_ini(FILE *file)
{
  if(fputs("# One section per katzip level. Multiple compressor setting groups "
    "compete.\n# zlib_after replaces them at or above the given file size; "
    "off disables it.\n# Sizes accept bytes, KiB, MiB and GiB.\n\n",
    file) == EOF)
    return -1;
  if(write_default_sections(file))
    return -1;
  return ferror(file) ? -1 : 0;
}
