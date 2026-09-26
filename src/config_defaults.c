#include "katzip_internal.h"

#include "defaults.h"

/* Names and offsets shared by the INI parser, CLI parser, and generator. */
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
  int fast_index = level <= FAST_LEVEL_COUNT ?
    level - 1 : FAST_LEVEL_COUNT - 1;
  int ect_index = level <= ECT_FIRST_LEVEL ?
    0 : level - ECT_FIRST_LEVEL;
  memset(config, 0, sizeof(*config));
  config->fast_level = fast_defaults[fast_index];
  config->ect = ect_defaults[ect_index];
  config->turtle = turtle_defaults[0];
  config->zlib_after = UINT64_MAX;
  config->zlib_level = level;
  if(level <= FAST_LEVEL_COUNT)
  {
    config->have_fast = 1;
    config->zlib_level = config->fast_level;
    config->zlib_after = DEFAULT_ZLIB_AFTER;
    return;
  }
  config->have_ect = 1;
  config->have_turtle = level == HIGHEST_LEVEL;
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
    if(fprintf(file, "--zopfli_%s %d\n", ect_fields[field].name,
      default_ect_value(options, &ect_fields[field])) < 0)
      return -1;
  return 0;
}

static int write_turtle_settings(FILE *file,
  const turtledeflate_config_t *config)
{
  size_t field;
  for(field = 0; field < ARRAY_N(config_fields); ++field)
    if(fprintf(file, "--turtledeflate_%s %d\n",
      config_fields[field].name,
      default_field_value(config, &config_fields[field])) < 0)
      return -1;
  return 0;
}

static int write_zlib_threshold(FILE *file, const COMPRESSION_CONFIG *config)
{
  if(config->zlib_after == UINT64_MAX)
    return fputs("--zlib_after off\n", file) == EOF ? -1 : 0;
  return fprintf(file, "--zlib_after %llu\n",
    (unsigned long long)config->zlib_after) < 0 ? -1 : 0;
}

static int write_default_fast(FILE *file,
  const COMPRESSION_CONFIG *config)
{
  if(!config->have_fast)
    return 0;
  return fprintf(file, "--libdeflate_level %d\n",
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
  return fprintf(file, "--zlib_level %d\n", config.zlib_level) < 0 ? -1 : 0;
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
  if(write_default_sections(file))
    return -1;
  return ferror(file) ? -1 : 0;
}
