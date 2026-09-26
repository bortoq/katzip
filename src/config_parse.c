#include "katzip_internal.h"

static int valid_turtle_basic(const turtledeflate_config_t *config)
{
  return config->i_compression_level >= 1 &&
    config->i_compression_level <= 9 &&
    config->i_maximum_block_size >= TURTLEDEFLATE_MIN_BLOCK_SIZE &&
    config->i_maximum_block_size <= MAX_BLOCK_SIZE &&
    config->i_maximum_subblocks >= TURTLEDEFLATE_MIN_SUBBLOCKS &&
    config->i_maximum_subblocks <= TURTLEDEFLATE_MAX_SUBBLOCKS;
}

static int valid_turtle_iterations(const turtledeflate_config_t *config)
{
  return config->i_max_block_splitter_iterations > 0 &&
    config->i_max_block_splitter_iterations <= 1000 &&
    config->i_max_internal_block_splitter_iterations > 0 &&
    config->i_max_internal_block_splitter_iterations <= 1000;
}

static int valid_turtle_sampling(const turtledeflate_config_t *config)
{
  return config->i_block_splitter_num_points > 0 &&
    config->i_block_splitter_num_points <= TURTLEDEFLATE_BSPLIT_MAX_NUM_POINTS &&
    config->i_block_splitter_center_dist > 0 &&
    config->i_block_splitter_center_dist <= config->i_block_splitter_num_points &&
    config->i_block_splitter_min_range_for_points > 0;
}

static int valid_turtle_precision(const turtledeflate_config_t *config)
{
  return config->i_min_start_fp >= -32 &&
    config->i_max_start_fp <= 32 &&
    config->i_min_start_fp <= config->i_max_start_fp &&
    config->i_num_start_fp >= 2 &&
    config->i_num_start_fp <= TURTLEDEFLATE_MAX_NUM_FP_START / 2;
}

static int valid_config(const turtledeflate_config_t *config)
{
  return valid_turtle_basic(config) &&
    valid_turtle_iterations(config) &&
    valid_turtle_sampling(config) &&
    valid_turtle_precision(config) &&
    config->i_verbose >= TURTLEDEFLATE_VERBOSE_NONE &&
    config->i_verbose <= TURTLEDEFLATE_VERBOSE_SQUISHITER;
}

typedef struct {
  uint32_t fast;
  uint32_t ect;
  uint32_t turtle;
  int zlib_after;
  int zlib_level;
} CONFIG_SEEN;

static int size_multiplier(const char *suffix, uint64_t *multiplier)
{
  static const struct {
    const char *suffix;
    uint64_t multiplier;
  } units[] = {
    {"", 1}, {"B", 1}, {"KiB", 1024},
    {"MiB", UINT64_C(1024) * 1024},
    {"GiB", UINT64_C(1024) * 1024 * 1024}
  };
  size_t i;
  for(i = 0; i < ARRAY_N(units); ++i)
  {
    if(strcmp(suffix, units[i].suffix) != 0)
      continue;
    *multiplier = units[i].multiplier;
    return 0;
  }
  return -1;
}

static int parse_size(const char *text, uint64_t *size)
{
  char *end;
  unsigned long long number;
  uint64_t multiplier;
  if(strcmp(text, "off") == 0)
  {
    *size = UINT64_MAX;
    return 0;
  }
  if(!isdigit((unsigned char)*text))
    return -1;
  errno = 0;
  number = strtoull(text, &end, 10);
  if(errno == ERANGE || size_multiplier(end, &multiplier))
    return -1;
  if(number > UINT64_MAX / multiplier)
    return -1;
  *size = (uint64_t)number * multiplier;
  return 0;
}

static int parse_number(const char *text, int *number)
{
  char *end;
  long value;
  errno = 0;
  value = strtol(text, &end, 10);
  if(!*text || *end || errno == ERANGE ||
    value < INT32_MIN || value > INT32_MAX)
    return -1;
  *number = (int)value;
  return 0;
}

static size_t ect_field_index(const char *name)
{
  size_t i;
  for(i = 0; i < ARRAY_N(ect_fields); ++i)
    if(strcmp(name, ect_fields[i].name) == 0)
      return i;
  return ARRAY_N(ect_fields);
}

static int valid_ect_field(size_t index, int number,
  const CONFIG_SEEN *seen)
{
  if(index == ARRAY_N(ect_fields))
    return 0;
  if(seen->ect & (UINT32_C(1) << index))
    return 0;
  return number >= ect_fields[index].minimum &&
    number <= ect_fields[index].maximum;
}

static int parse_ect_setting(const char *name, int number,
  COMPRESSION_CONFIG *config, CONFIG_SEEN *seen)
{
  size_t i = ect_field_index(name);
  if(!valid_ect_field(i, number, seen))
    return -1;
  if(i == 0)
    config->ect.numiterations = number;
  else
    *(unsigned*)((unsigned char*)&config->ect + ect_fields[i].offset) =
      (unsigned)number;
  seen->ect |= UINT32_C(1) << i;
  return 0;
}

static size_t turtle_field_index(const char *name)
{
  size_t i;
  for(i = 0; i < ARRAY_N(config_fields); ++i)
    if(strcmp(name, config_fields[i].name) == 0)
      return i;
  return ARRAY_N(config_fields);
}

static int store_turtle_field(turtledeflate_config_t *config,
  const CONFIG_FIELD *field, int number)
{
  unsigned char *address = (unsigned char*)config + field->offset;
  if(!field->boolean)
  {
    *(int32_t*)address = (int32_t)number;
    return 0;
  }
  if(number != 0 && number != 1)
    return -1;
  *(bool*)address = number != 0;
  return 0;
}

static int parse_turtle_setting(const char *name, int number,
  COMPRESSION_CONFIG *config, CONFIG_SEEN *seen)
{
  size_t i = turtle_field_index(name);
  if(i == ARRAY_N(config_fields))
    return -1;
  if(seen->turtle & (UINT32_C(1) << i))
    return -1;
  if(store_turtle_field(&config->turtle, &config_fields[i], number))
    return -1;
  seen->turtle |= UINT32_C(1) << i;
  return 0;
}

static int parse_zlib_after(const char *value, COMPRESSION_CONFIG *config,
  CONFIG_SEEN *seen)
{
  if(seen->zlib_after || parse_size(value, &config->zlib_after))
    return -1;
  seen->zlib_after = 1;
  return 0;
}

static int parse_zlib_level(int number, COMPRESSION_CONFIG *config,
  CONFIG_SEEN *seen)
{
  if(seen->zlib_level || number < 1 || number > 9)
    return -1;
  config->zlib_level = number;
  seen->zlib_level = 1;
  return 0;
}

static int parse_fast_level(int number, COMPRESSION_CONFIG *config,
  CONFIG_SEEN *seen)
{
  if(seen->fast || number < 1 || number > 12)
    return -1;
  config->fast_level = number;
  seen->fast = 1;
  return 0;
}

static int parse_numeric_setting(const char *name, int number,
  COMPRESSION_CONFIG *config, CONFIG_SEEN *seen)
{
  if(strcmp(name, "zlib_level") == 0)
    return parse_zlib_level(number, config, seen);
  if(strcmp(name, "libdeflate_level") == 0)
    return parse_fast_level(number, config, seen);
  if(strncmp(name, "zopfli_", 7) == 0)
    return parse_ect_setting(name + 7, number, config, seen);
  if(strncmp(name, "turtledeflate_", 14) == 0)
    return parse_turtle_setting(name + 14, number, config, seen);
  return -1;
}

static int parse_setting(char *line, COMPRESSION_CONFIG *config,
  CONFIG_SEEN *seen)
{
  char *value = strchr(line, '=');
  int number;
  if(!value)
    return -1;
  *value++ = 0;
  line = trim(line);
  value = trim(value);
  if(strcmp(line, "zlib_after") == 0)
    return parse_zlib_after(value, config, seen);
  if(parse_number(value, &number))
    return -1;
  return parse_numeric_setting(line, number, config, seen);
}

static int no_configured_engines(const CONFIG_SEEN *seen)
{
  return !seen->fast && !seen->ect && !seen->turtle;
}

static int ect_group_complete(const CONFIG_SEEN *seen)
{
  uint32_t all = (UINT32_C(1) << ARRAY_N(ect_fields)) - 1;
  return !seen->ect || seen->ect == all;
}

static int turtle_group_complete(const CONFIG_SEEN *seen,
  const COMPRESSION_CONFIG *config)
{
  uint32_t all = (UINT32_C(1) << ARRAY_N(config_fields)) - 1;
  if(!seen->turtle)
    return 1;
  return seen->turtle == all && valid_config(&config->turtle);
}

static int config_engines_complete(const CONFIG_SEEN *seen,
  const COMPRESSION_CONFIG *config)
{
  if(no_configured_engines(seen))
    return config->zlib_after == 0;
  if(!ect_group_complete(seen))
    return 0;
  return turtle_group_complete(seen, config);
}

static int config_is_complete(int found, const CONFIG_SEEN *seen,
  COMPRESSION_CONFIG *config)
{
  if(!found || !seen->zlib_after || !seen->zlib_level)
    return 0;
  if(!config_engines_complete(seen, config))
    return 0;
  config->have_fast = seen->fast != 0;
  config->have_ect = seen->ect != 0;
  config->have_turtle = seen->turtle != 0;
  return 1;
}

static int read_section_header(const char *text, const char *section,
  int *active, int *found)
{
  *active = strcmp(text, section) == 0;
  if(!*active)
    return 0;
  if(*found)
    return -1;
  *found = 1;
  return 0;
}

static int read_config_line(char *line, const char *section,
  int *active, int *found, COMPRESSION_CONFIG *config, CONFIG_SEEN *seen)
{
  char *text = trim(line);
  if(!*text || *text == '#' || *text == ';')
    return 0;
  if(*text == '[')
    return read_section_header(text, section, active, found);
  if(!*active)
    return 0;
  return parse_setting(text, config, seen);
}

static int read_selected_section(FILE *file, const char *path,
  const char *section, COMPRESSION_CONFIG *config,
  CONFIG_SEEN *seen, int *found)
{
  char line[256];
  int active = 0;
  int line_number = 0;
  while(fgets(line, sizeof(line), file))
  {
    ++line_number;
    if((!strchr(line, '\n') && !feof(file)) ||
      read_config_line(line, section, &active, found, config, seen))
    {
      fprintf(stderr, "katzip: invalid setting in %s:%d\n",
        path, line_number);
      return -1;
    }
  }
  return 0;
}

static int read_config(FILE *file, const char *path, int level,
  COMPRESSION_CONFIG *config)
{
  char section[16];
  CONFIG_SEEN seen = {0};
  int found = 0;
  snprintf(section, sizeof(section), "[%d]", level);
  memset(config, 0, sizeof(*config));
  if(read_selected_section(file, path, section, config, &seen, &found))
    return -1;
  if(ferror(file))
  {
    fprintf(stderr, "katzip: cannot read %s\n", path);
    return -1;
  }
  if(!config_is_complete(found, &seen, config))
  {
    fprintf(stderr, "katzip: missing or invalid settings in %s %s\n",
      path, section);
    return -1;
  }
  return 0;
}

int load_config(const char *program, int level,
  COMPRESSION_CONFIG *config)
{
  char *path = NULL;
  FILE *file;
  int result;
  result = open_config(program, &file, &path);
  if(result != 0)
  {
    free(path);
    if(result > 0)
    {
      use_default_config(level, config);
      return 0;
    }
    return -1;
  }
  result = read_config(file, path, level, config);
  fclose(file);
  free(path);
  return result;
}

/* raw DEFLATE is already compressed; this level only sets the ZIP hint bits. */
