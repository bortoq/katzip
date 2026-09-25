#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "turtledeflate_api.h"

typedef struct {
  const char *path;
  const char *name;
  uint16_t name_len;
  uint16_t flags;
  uint16_t dos_time;
  uint16_t dos_date;
  uint32_t mode;
  uint32_t offset;
  uint32_t crc;
  uint32_t compressed_size;
  uint32_t expected_size;
  uint32_t size;
} ENTRY;

typedef struct {
  FILE *file;
  uint64_t offset;
} OUTPUT;

typedef struct {
  const char *name;
  size_t offset;
  int boolean;
} CONFIG_FIELD;

typedef struct {
  ENTRY *entries;
  size_t count;
  size_t capacity;
  char **searched;
  size_t searched_count;
  size_t searched_capacity;
  struct stat archive_stat;
  int archive_exists;
} ENTRY_LIST;

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  pthread_t thread;
  const ENTRY *entry;
  uint32_t done;
  uint32_t pass;
  uint32_t pass_done;
  uint32_t pass_total;
  int display_width;
  int active;
  int stop;
} PROGRESS;

#define ARRAY_N(A) (sizeof(A) / sizeof((A)[0]))

static int write_bytes(OUTPUT *out, const void *data, size_t size)
{
  if(out->offset + size > UINT32_MAX || fwrite(data, 1, size, out->file) != size)
    return -1;
  out->offset += size;
  return 0;
}

static int write_u16(OUTPUT *out, uint16_t value)
{
  unsigned char bytes[2];
  bytes[0] = (unsigned char)value;
  bytes[1] = (unsigned char)(value >> 8);
  return write_bytes(out, bytes, sizeof(bytes));
}

static int write_u32(OUTPUT *out, uint32_t value)
{
  unsigned char bytes[4];
  bytes[0] = (unsigned char)value;
  bytes[1] = (unsigned char)(value >> 8);
  bytes[2] = (unsigned char)(value >> 16);
  bytes[3] = (unsigned char)(value >> 24);
  return write_bytes(out, bytes, sizeof(bytes));
}

static uint32_t update_crc(uint32_t crc, const unsigned char *data, size_t size)
{
  size_t i;
  int bit;
  for(i = 0; i < size; ++i)
  {
    crc ^= data[i];
    for(bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (crc & 1 ? UINT32_C(0xedb88320) : 0);
  }
  return crc;
}

static int valid_name(const char *name)
{
  const char *part;
  const char *end;
  if(!*name || *name == '/')
    return 0;
  for(part = name; *part; part = *end ? end + 1 : end)
  {
    end = strchr(part, '/');
    if(!end)
      end = part + strlen(part);
    if(end == part || (end - part == 2 && part[0] == '.' && part[1] == '.'))
      return 0;
    if(!*end)
      break;
  }
  return 1;
}

/* return 1 only for well-formed utf-8 */
static int valid_utf8(const char *name)
{
  const unsigned char *p = (const unsigned char*)name;
  uint32_t codepoint;
  uint32_t minimum;
  int count;
  int i;
  while(*p)
  {
    if(*p < 0x80)
    {
      ++p;
      continue;
    }
    if(*p >= 0xc2 && *p <= 0xdf)
    {
      codepoint = *p++ & 0x1f;
      minimum = 0x80;
      count = 1;
    }
    else if(*p >= 0xe0 && *p <= 0xef)
    {
      codepoint = *p++ & 0x0f;
      minimum = 0x800;
      count = 2;
    }
    else if(*p >= 0xf0 && *p <= 0xf4)
    {
      codepoint = *p++ & 0x07;
      minimum = 0x10000;
      count = 3;
    }
    else
      return 0;
    for(i = 0; i < count; ++i)
    {
      if(!*p || (*p & 0xc0) != 0x80)
        return 0;
      codepoint = (codepoint << 6) | (*p++ & 0x3f);
    }
    if(codepoint < minimum || (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff)
      return 0;
  }
  return 1;
}

static void show_progress(PROGRESS *progress)
{
  const ENTRY *entry = progress->entry;
  uint32_t done = progress->done;
  uint64_t percent = entry->expected_size ? (uint64_t)done * 10000 / entry->expected_size : 10000;
  int width;
  int i;
  if(percent > 10000)
    percent = 10000;
  width = fprintf(stderr, "\r%s %llu.%02llu%%", entry->name,
    (unsigned long long)(percent / 100), (unsigned long long)(percent % 100));
  if(progress->pass && progress->pass_total && done < entry->expected_size)
  {
    uint64_t pass_percent = (uint64_t)progress->pass_done * 10000 / progress->pass_total;
    width += fprintf(stderr, " (pass %u: %llu.%02llu%%)", progress->pass,
      (unsigned long long)(pass_percent / 100), (unsigned long long)(pass_percent % 100));
  }
  for(i = width; i < progress->display_width; ++i)
    fputc(' ', stderr);
  progress->display_width = width;
  fflush(stderr);
}

static void *progress_thread(void *argument)
{
  PROGRESS *progress = (PROGRESS*)argument;
  struct timespec deadline;
  int result;
  pthread_mutex_lock(&progress->mutex);
  while(!progress->stop)
  {
    while(!progress->active && !progress->stop)
      pthread_cond_wait(&progress->condition, &progress->mutex);
    if(progress->stop)
      break;
    clock_gettime(CLOCK_REALTIME, &deadline);
    ++deadline.tv_sec;
    result = pthread_cond_timedwait(&progress->condition, &progress->mutex, &deadline);
    if(result == ETIMEDOUT && progress->active)
      show_progress(progress);
  }
  pthread_mutex_unlock(&progress->mutex);
  return NULL;
}

static int progress_init(PROGRESS *progress)
{
  memset(progress, 0, sizeof(*progress));
  if(pthread_mutex_init(&progress->mutex, NULL))
    return -1;
  if(pthread_cond_init(&progress->condition, NULL))
  {
    pthread_mutex_destroy(&progress->mutex);
    return -1;
  }
  if(pthread_create(&progress->thread, NULL, progress_thread, progress))
  {
    pthread_cond_destroy(&progress->condition);
    pthread_mutex_destroy(&progress->mutex);
    return -1;
  }
  return 0;
}

static void progress_start(PROGRESS *progress, const ENTRY *entry)
{
  pthread_mutex_lock(&progress->mutex);
  progress->entry = entry;
  progress->done = 0;
  progress->pass = 0;
  progress->pass_done = 0;
  progress->pass_total = 0;
  progress->display_width = 0;
  progress->active = 1;
  show_progress(progress);
  pthread_cond_signal(&progress->condition);
  pthread_mutex_unlock(&progress->mutex);
}

static void progress_update(PROGRESS *progress, uint32_t done)
{
  pthread_mutex_lock(&progress->mutex);
  progress->done = done;
  progress->pass = 0;
  pthread_mutex_unlock(&progress->mutex);
}

static void progress_callback(void *user, uint32_t pass, uint32_t completed, uint32_t total)
{
  PROGRESS *progress = (PROGRESS*)user;
  pthread_mutex_lock(&progress->mutex);
  progress->pass = pass;
  progress->pass_done = completed;
  progress->pass_total = total;
  pthread_mutex_unlock(&progress->mutex);
}

static void progress_finish(PROGRESS *progress, int success)
{
  pthread_mutex_lock(&progress->mutex);
  if(success)
    progress->done = progress->entry->expected_size;
  progress->pass = 0;
  show_progress(progress);
  fputc('\n', stderr);
  progress->active = 0;
  pthread_cond_signal(&progress->condition);
  pthread_mutex_unlock(&progress->mutex);
}

static void progress_destroy(PROGRESS *progress)
{
  pthread_mutex_lock(&progress->mutex);
  progress->stop = 1;
  pthread_cond_signal(&progress->condition);
  pthread_mutex_unlock(&progress->mutex);
  pthread_join(progress->thread, NULL);
  pthread_cond_destroy(&progress->condition);
  pthread_mutex_destroy(&progress->mutex);
}

static void set_dos_time(ENTRY *entry, time_t value)
{
  struct tm *date = localtime(&value);
  int year;
  if(!date)
  {
    entry->dos_time = 0;
    entry->dos_date = (1 << 5) | 1;
    return;
  }
  year = date->tm_year + 1900;
  if(year < 1980)
    year = 1980;
  if(year > 2107)
    year = 2107;
  entry->dos_time = (uint16_t)((date->tm_hour << 11) | (date->tm_min << 5) | (date->tm_sec / 2));
  entry->dos_date = (uint16_t)(((year - 1980) << 9) | ((date->tm_mon + 1) << 5) | date->tm_mday);
}

static int write_local_header(OUTPUT *out, const ENTRY *entry)
{
  return write_u32(out, UINT32_C(0x04034b50)) ||
    write_u16(out, 20) || write_u16(out, entry->flags) || write_u16(out, 8) ||
    write_u16(out, entry->dos_time) || write_u16(out, entry->dos_date) ||
    write_u32(out, 0) || write_u32(out, 0) || write_u32(out, 0) ||
    write_u16(out, entry->name_len) || write_u16(out, 0) ||
    write_bytes(out, entry->name, entry->name_len) ? -1 : 0;
}

static int write_descriptor(OUTPUT *out, const ENTRY *entry)
{
  return write_u32(out, UINT32_C(0x08074b50)) || write_u32(out, entry->crc) ||
    write_u32(out, entry->compressed_size) || write_u32(out, entry->size) ? -1 : 0;
}

static int write_central_header(OUTPUT *out, const ENTRY *entry)
{
  return write_u32(out, UINT32_C(0x02014b50)) ||
    write_u16(out, (3 << 8) | 20) || write_u16(out, 20) ||
    write_u16(out, entry->flags) || write_u16(out, 8) ||
    write_u16(out, entry->dos_time) || write_u16(out, entry->dos_date) ||
    write_u32(out, entry->crc) || write_u32(out, entry->compressed_size) ||
    write_u32(out, entry->size) || write_u16(out, entry->name_len) ||
    write_u16(out, 0) || write_u16(out, 0) || write_u16(out, 0) ||
    write_u16(out, 0) || write_u32(out, entry->mode << 16) ||
    write_u32(out, entry->offset) ||
    write_bytes(out, entry->name, entry->name_len) ? -1 : 0;
}

static int write_end(OUTPUT *out, uint16_t count, uint32_t central_offset, uint32_t central_size)
{
  return write_u32(out, UINT32_C(0x06054b50)) ||
    write_u16(out, 0) || write_u16(out, 0) ||
    write_u16(out, count) || write_u16(out, count) ||
    write_u32(out, central_size) || write_u32(out, central_offset) ||
    write_u16(out, 0) ? -1 : 0;
}

static char *trim(char *text)
{
  size_t size;
  while(isspace((unsigned char)*text))
    ++text;
  size = strlen(text);
  while(size && isspace((unsigned char)text[size - 1]))
    text[--size] = 0;
  return text;
}

static char *config_path(const char *program)
{
  const char *override = getenv("TURZIP_INI");
  const char *slash;
  const char *name = "turzip.ini";
  char *path;
  size_t prefix;
  if(override && *override)
  {
    path = malloc(strlen(override) + 1);
    if(path)
      strcpy(path, override);
    return path;
  }
  slash = strrchr(program, '/');
  prefix = slash ? (size_t)(slash - program + 1) : 0;
  path = malloc(prefix + strlen(name) + 1);
  if(path)
  {
    memcpy(path, program, prefix);
    strcpy(path + prefix, name);
  }
  return path;
}

static int valid_config(const turtledeflate_config_t *config, int level)
{
  return config->i_compression_level == level &&
    config->i_maximum_block_size >= TURTLEDEFLATE_MIN_BLOCK_SIZE &&
    config->i_maximum_block_size <= INT32_MAX / 3 &&
    config->i_maximum_subblocks >= TURTLEDEFLATE_MIN_SUBBLOCKS &&
    config->i_maximum_subblocks <= TURTLEDEFLATE_MAX_SUBBLOCKS &&
    config->i_max_block_splitter_iterations > 0 &&
    config->i_max_internal_block_splitter_iterations > 0 &&
    config->i_block_splitter_num_points > 0 &&
    config->i_block_splitter_num_points <= TURTLEDEFLATE_BSPLIT_MAX_NUM_POINTS &&
    config->i_block_splitter_center_dist > 0 &&
    config->i_block_splitter_center_dist <= config->i_block_splitter_num_points &&
    config->i_block_splitter_min_range_for_points > 0 &&
    config->i_min_start_fp <= config->i_max_start_fp &&
    config->i_num_start_fp >= 2 &&
    config->i_num_start_fp <= TURTLEDEFLATE_MAX_NUM_FP_START / 2 &&
    config->i_verbose >= TURTLEDEFLATE_VERBOSE_NONE &&
    config->i_verbose <= TURTLEDEFLATE_VERBOSE_SQUISHITER;
}

static int load_config(const char *program, int level, turtledeflate_config_t *config)
{
  static const CONFIG_FIELD fields[] = {
    {"i_compression_level", offsetof(turtledeflate_config_t, i_compression_level), 0},
    {"i_maximum_block_size", offsetof(turtledeflate_config_t, i_maximum_block_size), 0},
    {"i_maximum_subblocks", offsetof(turtledeflate_config_t, i_maximum_subblocks), 0},
    {"i_max_block_splitter_iterations", offsetof(turtledeflate_config_t, i_max_block_splitter_iterations), 0},
    {"i_max_internal_block_splitter_iterations", offsetof(turtledeflate_config_t, i_max_internal_block_splitter_iterations), 0},
    {"i_block_splitter_num_points", offsetof(turtledeflate_config_t, i_block_splitter_num_points), 0},
    {"i_block_splitter_center_dist", offsetof(turtledeflate_config_t, i_block_splitter_center_dist), 0},
    {"i_block_splitter_min_range_for_points", offsetof(turtledeflate_config_t, i_block_splitter_min_range_for_points), 0},
    {"b_block_splitter_push_split", offsetof(turtledeflate_config_t, b_block_splitter_push_split), 1},
    {"i_min_start_fp", offsetof(turtledeflate_config_t, i_min_start_fp), 0},
    {"i_max_start_fp", offsetof(turtledeflate_config_t, i_max_start_fp), 0},
    {"i_num_start_fp", offsetof(turtledeflate_config_t, i_num_start_fp), 0},
    {"i_verbose", offsetof(turtledeflate_config_t, i_verbose), 0}
  };
  char section[32];
  char line[256];
  char *path = config_path(program);
  char *key;
  char *value;
  char *end;
  FILE *file;
  uint32_t seen = 0;
  long number;
  size_t i;
  int active = 0;
  int found = 0;
  int line_number = 0;
  int status = -1;
  if(!path)
  {
    fprintf(stderr, "turzip: out of memory\n");
    return -1;
  }
  file = fopen(path, "r");
  if(!file)
  {
    fprintf(stderr, "turzip: cannot open %s: %s\n", path, strerror(errno));
    free(path);
    return -1;
  }
  snprintf(section, sizeof(section), "[turtledeflate-%d]", level);
  memset(config, 0, sizeof(*config));
  while(fgets(line, sizeof(line), file))
  {
    ++line_number;
    if(!strchr(line, '\n') && !feof(file))
      goto bad_line;
    key = trim(line);
    if(!*key || *key == '#' || *key == ';')
      continue;
    if(*key == '[')
    {
      active = strcmp(key, section) == 0;
      if(active && found++)
        goto bad_line;
      continue;
    }
    if(!active)
      continue;
    value = strchr(key, '=');
    if(!value)
      goto bad_line;
    *value++ = 0;
    key = trim(key);
    value = trim(value);
    errno = 0;
    number = strtol(value, &end, 10);
    if(!*value || *end || errno == ERANGE || number < INT32_MIN || number > INT32_MAX)
      goto bad_line;
    for(i = 0; i < ARRAY_N(fields); ++i)
    {
      if(strcmp(key, fields[i].name) == 0)
        break;
    }
    if(i == ARRAY_N(fields) || (seen & (UINT32_C(1) << i)))
      goto bad_line;
    if(fields[i].boolean)
    {
      if(number != 0 && number != 1)
        goto bad_line;
      *(bool*)((unsigned char*)config + fields[i].offset) = number != 0;
    }
    else
      *(int32_t*)((unsigned char*)config + fields[i].offset) = (int32_t)number;
    seen |= UINT32_C(1) << i;
  }
  if(ferror(file))
  {
    fprintf(stderr, "turzip: cannot read %s\n", path);
    goto done;
  }
  if(!found || seen != (UINT32_C(1) << ARRAY_N(fields)) - 1 || !valid_config(config, level))
  {
    fprintf(stderr, "turzip: missing or invalid settings in %s %s\n", path, section);
    goto done;
  }
  status = 0;
  goto done;

bad_line:
  fprintf(stderr, "turzip: invalid setting in %s:%d\n", path, line_number);
done:
  fclose(file);
  free(path);
  return status;
}

static int write_entry(OUTPUT *out, ENTRY *entry, FILE *in, const turtledeflate_config_t *config, PROGRESS *progress)
{
  unsigned char *buffer;
  unsigned char *compressed;
  turtledeflate_config_t compressor_config = *config;
  void *compressor = NULL;
  uint64_t start;
  uint32_t crc = UINT32_MAX;
  size_t size;
  int next;
  int compressed_size;
  int progress_started = 0;
  int status = -1;

  buffer = malloc((size_t)config->i_maximum_block_size);
  if(!buffer)
    return -1;
  entry->offset = (uint32_t)out->offset;
  if(write_local_header(out, entry))
    goto done;
  start = out->offset;
  size = fread(buffer, 1, (size_t)config->i_maximum_block_size, in);
  if(ferror(in))
    goto done;
  if(size && !turtledeflate_create(&compressor, &compressor_config))
    goto done;
  progress_start(progress, entry);
  progress_started = 1;
  if(compressor)
    turtledeflate_set_progress_callback(compressor, progress_callback, progress);
  while(size)
  {
    next = fgetc(in);
    if(next != EOF && ungetc(next, in) == EOF)
      goto done;
    if(next == EOF && ferror(in))
      goto done;
    if((uint64_t)entry->size + size > UINT32_MAX)
      goto done;
    crc = update_crc(crc, buffer, size);
    entry->size += (uint32_t)size;
    compressed_size = turtledeflate_block(compressor, (int32_t)size, buffer, &compressed, NULL, next == EOF);
    if(compressed_size < 0 || write_bytes(out, compressed, (size_t)compressed_size))
      goto done;
    progress_update(progress, entry->size);
    if(next == EOF)
      break;
    size = fread(buffer, 1, (size_t)config->i_maximum_block_size, in);
    if(!size || ferror(in))
      goto done;
  }
  if(!entry->size)
  {
    const unsigned char empty_deflate[] = {0x03, 0x00};
    if(write_bytes(out, empty_deflate, sizeof(empty_deflate)))
      goto done;
  }
  entry->crc = crc ^ UINT32_MAX;
  entry->compressed_size = (uint32_t)(out->offset - start);
  if(write_descriptor(out, entry))
    goto done;
  status = 0;

done:
  if(compressor)
    turtledeflate_destroy(compressor);
  free(buffer);
  if(progress_started)
    progress_finish(progress, status == 0);
  return status;
}

static char *copy_text(const char *text)
{
  char *copy = malloc(strlen(text) + 1);
  if(copy)
    strcpy(copy, text);
  return copy;
}

static char *archive_name(const char *argument)
{
  const char *base = strrchr(argument, '/');
  const char *dot;
  size_t length = strlen(argument);
  char *name;
  base = base ? base + 1 : argument;
  if(!*base)
    return NULL;
  dot = strrchr(base, '.');
  if(dot && dot > base && dot[1])
    return copy_text(argument);
  name = malloc(length + 5);
  if(!name)
    return NULL;
  strcpy(name, argument);
  if(dot && dot[1] == 0)
    strcat(name, "zip");
  else
    strcat(name, ".zip");
  return name;
}

static int add_entry(ENTRY_LIST *list, const char *path, int recursive)
{
  const char *name = path;
  struct stat file_stat;
  ENTRY *entry;
  ENTRY *grown;
  size_t capacity;
  size_t i;
  while(name[0] == '.' && name[1] == '/')
    name += 2;
  if(!valid_name(name) || strlen(name) > UINT16_MAX || stat(path, &file_stat) ||
    !S_ISREG(file_stat.st_mode) || file_stat.st_size < 0 ||
    (uint64_t)file_stat.st_size > UINT32_MAX)
  {
    fprintf(stderr, "turzip: invalid input file: %s\n", path);
    return -1;
  }
  if(list->archive_exists && list->archive_stat.st_dev == file_stat.st_dev &&
    list->archive_stat.st_ino == file_stat.st_ino)
  {
    if(recursive)
      return 0;
    fprintf(stderr, "turzip: archive is an input file: %s\n", path);
    return -1;
  }
  for(i = 0; i < list->count; ++i)
  {
    if(strcmp(name, list->entries[i].name) == 0)
    {
      if(recursive)
        return 0;
      fprintf(stderr, "turzip: duplicate entry: %s\n", name);
      return -1;
    }
  }
  if(list->count == UINT16_MAX)
  {
    fprintf(stderr, "turzip: too many files\n");
    return -1;
  }
  if(list->count == list->capacity)
  {
    capacity = list->capacity ? list->capacity * 2 : 16;
    if(capacity > UINT16_MAX)
      capacity = UINT16_MAX;
    grown = realloc(list->entries, capacity * sizeof(*grown));
    if(!grown)
    {
      fprintf(stderr, "turzip: out of memory\n");
      return -1;
    }
    list->entries = grown;
    list->capacity = capacity;
  }
  entry = &list->entries[list->count];
  memset(entry, 0, sizeof(*entry));
  entry->path = copy_text(path);
  entry->name = copy_text(name);
  if(!entry->path || !entry->name)
  {
    free((void*)entry->path);
    free((void*)entry->name);
    fprintf(stderr, "turzip: out of memory\n");
    return -1;
  }
  entry->name_len = (uint16_t)strlen(name);
  entry->flags = (uint16_t)(8 | (valid_utf8(name) ? 0x0800 : 0));
  entry->mode = (uint32_t)file_stat.st_mode;
  entry->expected_size = (uint32_t)file_stat.st_size;
  set_dos_time(entry, file_stat.st_mtime);
  ++list->count;
  return 0;
}

static char *join_path(const char *directory, const char *name)
{
  size_t prefix = strcmp(directory, ".") == 0 ? 0 : strlen(directory);
  size_t length = strlen(name);
  char *path = malloc(prefix + length + 2);
  if(!path)
    return NULL;
  if(prefix)
  {
    memcpy(path, directory, prefix);
    path[prefix++] = '/';
  }
  memcpy(path + prefix, name, length + 1);
  return path;
}

static int matches_pattern(const char *pattern, const char *relative, const char *basename)
{
  const char *suffix;
  if(!pattern)
    return 1;
  if(!strchr(pattern, '/'))
    return fnmatch(pattern, basename, 0) == 0;
  for(suffix = relative; ; ++suffix)
  {
    if(fnmatch(pattern, suffix, FNM_PATHNAME) == 0)
      return 1;
    suffix = strchr(suffix, '/');
    if(!suffix)
      break;
  }
  return 0;
}

static int walk_directory(ENTRY_LIST *list, const char *directory, const char *root, const char *pattern, size_t *matched)
{
  DIR *stream = opendir(directory);
  struct dirent *item;
  struct stat file_stat;
  char *path;
  const char *relative;
  int status = 0;
  if(!stream)
  {
    fprintf(stderr, "turzip: cannot open directory %s: %s\n", directory, strerror(errno));
    return -1;
  }
  for(;;)
  {
    errno = 0;
    item = readdir(stream);
    if(!item)
    {
      if(errno)
        status = -1;
      break;
    }
    if(strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
      continue;
    path = join_path(directory, item->d_name);
    if(!path)
    {
      status = -1;
      break;
    }
    if(lstat(path, &file_stat))
    {
      fprintf(stderr, "turzip: cannot inspect %s: %s\n", path, strerror(errno));
      status = -1;
    }
    else if(S_ISDIR(file_stat.st_mode))
      status = walk_directory(list, path, root, pattern, matched);
    else if(S_ISREG(file_stat.st_mode))
    {
      relative = strcmp(root, ".") == 0 ? path : path + strlen(root) + 1;
      if(matches_pattern(pattern, relative, item->d_name))
      {
        if(matched)
          ++*matched;
        status = add_entry(list, path, 1);
      }
    }
    free(path);
    if(status)
      break;
  }
  if(closedir(stream))
    status = -1;
  return status;
}

static int search_files(ENTRY_LIST *list, const char *root, const char *pattern)
{
  char *key = join_path(root, pattern);
  char **grown;
  size_t capacity;
  size_t matched = 0;
  size_t i;
  int status;
  if(!key)
    return -1;
  for(i = 0; i < list->searched_count; ++i)
  {
    if(strcmp(key, list->searched[i]) == 0)
    {
      free(key);
      return 0;
    }
  }
  if(list->searched_count == list->searched_capacity)
  {
    capacity = list->searched_capacity ? list->searched_capacity * 2 : 8;
    grown = realloc(list->searched, capacity * sizeof(*grown));
    if(!grown)
    {
      free(key);
      return -1;
    }
    list->searched = grown;
    list->searched_capacity = capacity;
  }
  list->searched[list->searched_count++] = key;
  status = walk_directory(list, root, root, pattern, &matched);
  if(!status && !matched)
  {
    fprintf(stderr, "turzip: no files match %s\n", key);
    return -1;
  }
  return status;
}

static int add_argument(ENTRY_LIST *list, const char *argument, int recursive)
{
  const char *magic;
  const char *slash;
  const char *name = argument;
  const char *base;
  const char *dot;
  struct stat file_stat;
  char *root;
  char *extension_pattern;
  const char *pattern;
  size_t prefix;
  int status;
  while(name[0] == '.' && name[1] == '/')
    name += 2;
  if(!valid_name(name))
  {
    fprintf(stderr, "turzip: invalid input: %s\n", argument);
    return -1;
  }
  magic = strpbrk(argument, "*?[");
  if(recursive && magic)
  {
    slash = NULL;
    for(pattern = argument; pattern < magic; ++pattern)
    {
      if(*pattern == '/')
        slash = pattern;
    }
    prefix = slash ? (size_t)(slash - argument) : 0;
    root = prefix ? malloc(prefix + 1) : copy_text(".");
    if(!root)
      return -1;
    if(prefix)
    {
      memcpy(root, argument, prefix);
      root[prefix] = 0;
    }
    pattern = slash ? slash + 1 : argument;
    status = search_files(list, root, pattern);
    free(root);
  }
  else if(recursive && !stat(argument, &file_stat) && S_ISDIR(file_stat.st_mode))
  {
    root = copy_text(argument);
    if(!root)
      return -1;
    prefix = strlen(root);
    while(prefix > 1 && root[prefix - 1] == '/')
      root[--prefix] = 0;
    status = walk_directory(list, root, root, NULL, NULL);
    free(root);
  }
  else if(recursive && !stat(argument, &file_stat) && S_ISREG(file_stat.st_mode))
  {
    status = add_entry(list, argument, 1);
    if(status)
      return status;
    slash = strrchr(argument, '/');
    base = slash ? slash + 1 : argument;
    dot = strrchr(base, '.');
    if(!dot || dot == base || !dot[1])
      return 0;
    prefix = slash ? (size_t)(slash - argument) : 0;
    root = prefix ? malloc(prefix + 1) : copy_text(".");
    extension_pattern = malloc(strlen(dot) + 2);
    if(!root || !extension_pattern)
    {
      free(root);
      free(extension_pattern);
      return -1;
    }
    if(prefix)
    {
      memcpy(root, argument, prefix);
      root[prefix] = 0;
    }
    extension_pattern[0] = '*';
    strcpy(extension_pattern + 1, dot);
    status = search_files(list, root, extension_pattern);
    free(root);
    free(extension_pattern);
  }
  else
    status = add_entry(list, argument, 0);
  return status;
}

static void free_entries(ENTRY_LIST *list)
{
  size_t i;
  for(i = 0; i < list->count; ++i)
  {
    free((void*)list->entries[i].path);
    free((void*)list->entries[i].name);
  }
  free(list->entries);
  for(i = 0; i < list->searched_count; ++i)
    free(list->searched[i]);
  free(list->searched);
}

int main(int argc, char **argv)
{
  ENTRY_LIST list;
  PROGRESS progress;
  OUTPUT out;
  FILE *in;
  char *archive_path;
  uint32_t central_offset;
  uint32_t central_size;
  turtledeflate_config_t config;
  int archive_arg = 1;
  int level = 7;
  int recursive = 0;
  int progress_ready = 0;
  int i;
  int status = 1;

  while(archive_arg < argc && argv[archive_arg][0] == '-')
  {
    if(strcmp(argv[archive_arg], "--") == 0)
    {
      ++archive_arg;
      break;
    }
    if(strcmp(argv[archive_arg], "-r") == 0)
      recursive = 1;
    else if(argv[archive_arg][1] >= '1' && argv[archive_arg][1] <= '9' && !argv[archive_arg][2])
      level = argv[archive_arg][1] - '0';
    else
    {
      fprintf(stderr, "turzip: unknown option: %s\n", argv[archive_arg]);
      return 1;
    }
    ++archive_arg;
  }
  if(argc <= archive_arg + 1)
  {
    fprintf(stderr, "usage: turzip [-1..-9] [-r] archive_name file[s]\n");
    return 1;
  }
  if(load_config(argv[0], level, &config))
    return 1;
  archive_path = archive_name(argv[archive_arg]);
  if(!archive_path)
  {
    fprintf(stderr, "turzip: invalid archive name\n");
    return 1;
  }
  memset(&list, 0, sizeof(list));
  list.archive_exists = stat(archive_path, &list.archive_stat) == 0;
  for(i = archive_arg + 1; i < argc; ++i)
  {
    if(add_argument(&list, argv[i], recursive))
      goto done;
  }
  if(!list.count)
  {
    fprintf(stderr, "turzip: no files to archive\n");
    goto done;
  }
  if(progress_init(&progress))
  {
    fprintf(stderr, "turzip: cannot start progress display\n");
    goto done;
  }
  progress_ready = 1;
  out.file = fopen(archive_path, "wb");
  if(!out.file)
  {
    fprintf(stderr, "turzip: cannot create %s: %s\n", archive_path, strerror(errno));
    goto done;
  }
  out.offset = 0;
  for(i = 0; i < (int)list.count; ++i)
  {
    in = fopen(list.entries[i].path, "rb");
    if(!in || write_entry(&out, &list.entries[i], in, &config, &progress))
    {
      fprintf(stderr, "turzip: cannot archive %s\n", list.entries[i].path);
      if(in)
        fclose(in);
      goto output_error;
    }
    if(fclose(in))
      goto output_error;
  }
  central_offset = (uint32_t)out.offset;
  for(i = 0; i < (int)list.count; ++i)
  {
    if(write_central_header(&out, &list.entries[i]))
      goto output_error;
  }
  central_size = (uint32_t)out.offset - central_offset;
  if(write_end(&out, (uint16_t)list.count, central_offset, central_size))
    goto output_error;
  if(fclose(out.file))
  {
    remove(archive_path);
    goto done;
  }
  status = 0;
  goto done;

output_error:
  fprintf(stderr, "turzip: failed to write archive %s\n", archive_path);
  fclose(out.file);
  remove(archive_path);
done:
  if(progress_ready)
    progress_destroy(&progress);
  free_entries(&list);
  free(archive_path);
  return status;
}
