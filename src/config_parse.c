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

static int valid_turtle(const turtledeflate_config_t *config)
{
  return valid_turtle_basic(config) &&
    valid_turtle_iterations(config) &&
    valid_turtle_sampling(config) &&
    valid_turtle_precision(config) &&
    config->i_verbose >= TURTLEDEFLATE_VERBOSE_NONE &&
    config->i_verbose <= TURTLEDEFLATE_VERBOSE_SQUISHITER;
}

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

static const ECT_FIELD *find_ect_field(const char *name)
{
  size_t i;
  for(i = 0; i < ARRAY_N(ect_fields); ++i)
    if(strcmp(name, ect_fields[i].name) == 0)
      return &ect_fields[i];
  return NULL;
}

static const CONFIG_FIELD *find_turtle_field(const char *name)
{
  size_t i;
  for(i = 0; i < ARRAY_N(config_fields); ++i)
    if(strcmp(name, config_fields[i].name) == 0)
      return &config_fields[i];
  return NULL;
}

int config_option_known(const char *option)
{
  if(strcmp(option, "--zlib_after") == 0 ||
    strcmp(option, "--zlib_level") == 0 ||
    strcmp(option, "--libdeflate_level") == 0)
    return 1;
  if(strncmp(option, "--zopfli_", 9) == 0)
    return find_ect_field(option + 9) != NULL;
  if(strncmp(option, "--turtledeflate_", 16) == 0)
    return find_turtle_field(option + 16) != NULL;
  return 0;
}

static int set_ect(const ECT_FIELD *field, int number,
  COMPRESSION_CONFIG *config)
{
  unsigned char *address;
  if(!field || number < field->minimum || number > field->maximum)
    return -1;
  address = (unsigned char*)&config->ect + field->offset;
  if(field == &ect_fields[0])
    *(int*)address = number;
  else
    *(unsigned*)address = (unsigned)number;
  config->have_ect = 1;
  return 0;
}

static int set_turtle(const CONFIG_FIELD *field, int number,
  COMPRESSION_CONFIG *config)
{
  unsigned char *address;
  if(!field)
    return -1;
  address = (unsigned char*)&config->turtle + field->offset;
  if(field->boolean)
  {
    if(number != 0 && number != 1)
      return -1;
    *(bool*)address = number != 0;
  }
  else
    *(int32_t*)address = (int32_t)number;
  config->have_turtle = 1;
  return 0;
}

static int set_numeric(const char *option, int number,
  COMPRESSION_CONFIG *config)
{
  if(strcmp(option, "--zlib_level") == 0)
  {
    if(number < 1 || number > 9)
      return -1;
    config->zlib_level = number;
    return 0;
  }
  if(strcmp(option, "--libdeflate_level") == 0)
  {
    if(number < 1 || number > 12)
      return -1;
    config->fast_level = number;
    config->have_fast = 1;
    return 0;
  }
  if(strncmp(option, "--zopfli_", 9) == 0)
    return set_ect(find_ect_field(option + 9), number, config);
  if(strncmp(option, "--turtledeflate_", 16) == 0)
    return set_turtle(find_turtle_field(option + 16), number, config);
  return -1;
}

int apply_config_option(const char *option, const char *value,
  COMPRESSION_CONFIG *config)
{
  int number;
  if(strcmp(option, "--zlib_after") == 0)
    return parse_size(value, &config->zlib_after);
  if(!config_option_known(option) || parse_number(value, &number))
    return -1;
  return set_numeric(option, number, config);
}

int validate_compression_config(const COMPRESSION_CONFIG *config)
{
  if(config->zlib_after != 0 && !config->have_fast &&
    !config->have_ect && !config->have_turtle)
    return -1;
  if(config->have_turtle && !valid_turtle(&config->turtle))
    return -1;
  return 0;
}

static int parse_ini_option(char *text, COMPRESSION_CONFIG *config,
  OPTIONS *options)
{
  char *value = text;
  while(*value && !isspace((unsigned char)*value))
    ++value;
  if(*value)
    *value++ = 0;
  value = trim(value);
  if(strcmp(text, "-r") == 0 && !*value)
  {
    options->recursive = 1;
    return 0;
  }
  if(!*value || !config_option_known(text))
    return -1;
  if(strpbrk(value, " \t\r\n"))
    return -1;
  return apply_config_option(text, value, config);
}

static int section_header(const char *text, int level)
{
  return text[0] == '[' && text[1] == (char)('0' + level) &&
    text[2] == ']' && text[3] == 0;
}

static int read_selected_section(FILE *file, const char *path,
  int level, COMPRESSION_CONFIG *config, OPTIONS *options)
{
  char *line = NULL;
  size_t capacity = 0;
  int active = 0;
  int found = 0;
  int line_number = 0;
  int invalid = 0;
  while(getline(&line, &capacity, file) >= 0)
  {
    char *text = trim(line);
    ++line_number;
    if(*text == '[')
    {
      active = section_header(text, level);
      if(active && found)
      {
        invalid = 1;
        break;
      }
      found |= active;
      continue;
    }
    if(!active || !*text || *text == '#' || *text == ';')
      continue;
    if(parse_ini_option(text, config, options) == 0)
      continue;
    invalid = 1;
    break;
  }
  free(line);
  if(ferror(file))
  {
    fprintf(stderr, "katzip: cannot read %s\n", path);
    return -1;
  }
  if(invalid)
  {
    fprintf(stderr, "katzip: invalid setting in %s:%d\n",
      path, line_number);
    return -1;
  }
  if(!found)
  {
    fprintf(stderr, "katzip: missing section [%d] in %s\n", level, path);
    return -1;
  }
  return 0;
}

int load_config(const char *program, int level,
  COMPRESSION_CONFIG *config, OPTIONS *options)
{
  char *path = NULL;
  FILE *file;
  int result;
  use_default_config(level, config);
  result = open_config(program, &file, &path);
  if(result != 0)
  {
    free(path);
    return result < 0 ? -1 : 0;
  }
  config->have_fast = 0;
  config->have_ect = 0;
  config->have_turtle = 0;
  result = read_selected_section(file, path, level, config, options);
  fclose(file);
  free(path);
  return result;
}
