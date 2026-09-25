/*
 * katzip has five stages: read settings, collect inputs, compress entries,
 * validate the temporary ZIP, and publish it with rename(). Each stage owns
 * its resources and returns an error to the next outer stage for cleanup.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <libdeflate.h>
#include <zlib.h>

#include "config_defaults.h"
#include "turtledeflate_api.h"
#include "mz.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"
#include "third_party/ect/src/zopfli/deflate.h"
#include "third_party/ect/src/zopfli/zopfli.h"

typedef struct {
  const char *path;
  const char *name;
  uint16_t name_len;
  uint16_t flags;
  uint32_t mode;
  uint32_t expected_size;
  uint32_t size;
  uint64_t compressed_size;
  time_t mtime;
} ENTRY;

typedef struct {
  const char *name;
  size_t offset;
  int boolean;
} CONFIG_FIELD;

typedef struct {
  ENTRY *entries;
  size_t count;
  size_t capacity;
  struct stat archive_stat;
  int archive_exists;
} ENTRY_LIST;

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  pthread_t thread;
  const ENTRY *entry;
  uint32_t done;
  uint32_t block_size;
  double pass_scale;
  uint32_t pass;
  uint32_t pass_done;
  uint32_t pass_total;
  uint64_t displayed_percent;
  int display_width;
  int active;
  int stop;
} PROGRESS;

typedef struct {
  pthread_t thread;
  unsigned char *input;
  unsigned char *output;
  size_t input_size;
  size_t output_size;
  uint32_t crc;
  int started;
} ECT_JOB;

#define ARRAY_N(A) (sizeof(A) / sizeof((A)[0]))
/* the largest upstream preset also bounds Turtledeflate's int32 allocation arithmetic */
#define MAX_BLOCK_SIZE 1000000
#define FAST_FILE_LIMIT (64U * 1024U * 1024U)

static const char *volatile signal_temp_path;

static void remove_temp_on_signal(int signal_number)
{
  if(signal_temp_path)
    unlink(signal_temp_path);
  _exit(128 + signal_number);
}

static uint32_t update_crc(uint32_t crc, const unsigned char *data, size_t size)
{
  return (uint32_t)crc32(crc ^ UINT32_MAX, data, (uInt)size) ^ UINT32_MAX;
}

static void *ect_worker(void *argument)
{
  ECT_JOB *job = (ECT_JOB*)argument;
  ZopfliOptions options;
  unsigned char bit_position = 0;
  ZopfliInitOptions(&options, 9, 0, 0);
  ZopfliDeflate(&options, 1, job->input, job->input_size,
    &bit_position, &job->output, &job->output_size);
  free(job->input);
  job->input = NULL;
  return NULL;
}

/* start ECT while Turtledeflate processes the same stable input */
static int start_ect_job(ECT_JOB *job, FILE *in, uint32_t size)
{
  int next;
  memset(job, 0, sizeof(*job));
  if(!size)
    return 0;
#if SIZE_MAX <= UINT32_MAX
  if(size > SIZE_MAX - 16)
    return 0;
#endif
  job->input = malloc((size_t)size + 16);
  if(!job->input)
    return 0;
  if(fread(job->input, 1, size, in) != size)
    return -1;
  next = fgetc(in);
  if(next != EOF || ferror(in) || fseek(in, 0, SEEK_SET))
    return -1;
  memset(job->input + size, 0, 16);
  job->input_size = size;
  job->crc = update_crc(UINT32_MAX, job->input, size) ^ UINT32_MAX;
  if(pthread_create(&job->thread, NULL, ect_worker, job))
    return 0;
  job->started = 1;
  return 0;
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

/* Percentages use hundredths of a percent to avoid floating output drift. */
static void print_progress_value(PROGRESS *progress, uint64_t percent)
{
  int width = fprintf(stderr, "\r%s %llu.%02llu%%",
    progress->entry->name, (unsigned long long)(percent / 100),
    (unsigned long long)(percent % 100));
  int i;
  for(i = width; i < progress->display_width; ++i)
    fputc(' ', stderr);
  progress->display_width = width;
  fflush(stderr);
}

static void show_progress(PROGRESS *progress, int timer_tick)
{
  const ENTRY *entry = progress->entry;
  double done = progress->done;
  double percent;
  if(progress->pass && progress->pass_total && done < entry->expected_size)
  {
    double work = progress->pass - 1 + (double)progress->pass_done / progress->pass_total;
    done += progress->block_size * work / (work + progress->pass_scale);
  }
  percent = entry->expected_size ? done * 10000 / entry->expected_size : 0;
  if(percent > 9999)
    percent = 9999;
  if((uint64_t)percent < progress->displayed_percent)
    percent = progress->displayed_percent;
  if(timer_tick && progress->block_size &&
    (uint64_t)percent == progress->displayed_percent && progress->displayed_percent < 9999)
    percent = progress->displayed_percent + 1;
  progress->displayed_percent = (uint64_t)percent;
  print_progress_value(progress, (uint64_t)percent);
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
      show_progress(progress, 1);
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

static void progress_start(PROGRESS *progress, const ENTRY *entry,
  const turtledeflate_config_t *config)
{
  pthread_mutex_lock(&progress->mutex);
  progress->entry = entry;
  progress->done = 0;
  progress->block_size = 0;
  progress->pass_scale = config ? 2.0 * config->i_num_start_fp *
    config->i_max_block_splitter_iterations : 1.0;
  progress->pass = 0;
  progress->pass_done = 0;
  progress->pass_total = 0;
  progress->displayed_percent = 0;
  progress->display_width = 0;
  progress->active = 1;
  show_progress(progress, 0);
  pthread_cond_signal(&progress->condition);
  pthread_mutex_unlock(&progress->mutex);
}

static void progress_block(PROGRESS *progress, uint32_t size)
{
  pthread_mutex_lock(&progress->mutex);
  progress->block_size = size;
  progress->pass = 0;
  pthread_mutex_unlock(&progress->mutex);
}

static void progress_update(PROGRESS *progress, uint32_t done)
{
  pthread_mutex_lock(&progress->mutex);
  progress->done = done;
  progress->block_size = 0;
  progress->pass = 0;
  pthread_mutex_unlock(&progress->mutex);
}

static void progress_wait(PROGRESS *progress)
{
  pthread_mutex_lock(&progress->mutex);
  progress->block_size = 1;
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
  uint64_t percent;
  pthread_mutex_lock(&progress->mutex);
  progress->block_size = 0;
  progress->pass = 0;
  if(success)
  {
    percent = progress->entry->size ?
      (progress->entry->compressed_size * 10000 +
        progress->entry->size / 2) / progress->entry->size : 0;
    print_progress_value(progress, percent);
  }
  else
    show_progress(progress, 0);
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

/* Resolve PATH only when argv[0] does not contain a directory. */
static char *executable_from_path(const char *program)
{
  const char *search = getenv("PATH");
  while(search)
  {
    const char *end = strchr(search, ':');
    size_t length = end ? (size_t)(end - search) : strlen(search);
    size_t directory_size = length ? length : 1;
    char *candidate = malloc(directory_size + strlen(program) + 2);
    char *resolved = NULL;
    if(!candidate)
      return NULL;
    if(length)
      memcpy(candidate, search, length);
    else
      candidate[0] = '.';
    candidate[directory_size] = '/';
    strcpy(candidate + directory_size + 1, program);
    if(access(candidate, X_OK) == 0)
      resolved = realpath(candidate, NULL);
    free(candidate);
    if(resolved)
      return resolved;
    search = end ? end + 1 : NULL;
  }
  return NULL;
}

/* /proc finds the real binary even when it was launched through PATH. */
static char *executable_config_path(const char *program)
{
  char *executable = realpath("/proc/self/exe", NULL);
  const char *slash;
  char *path;
  size_t directory_size;
  if(!executable)
    executable = strchr(program, '/') ?
      realpath(program, NULL) : executable_from_path(program);
  if(!executable)
    return NULL;
  slash = strrchr(executable, '/');
  directory_size = slash ? (size_t)(slash - executable + 1) : 0;
  path = malloc(directory_size + sizeof("katzip.ini"));
  if(path)
  {
    memcpy(path, executable, directory_size);
    strcpy(path + directory_size, "katzip.ini");
  }
  free(executable);
  return path;
}

/* Return 1 when a candidate does not exist, and -1 for other errors. */
static int open_named_config(const char *name, FILE **file, char **path)
{
  int saved_error;
  *path = strdup(name);
  if(!*path)
  {
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  *file = fopen(*path, "r");
  if(*file)
    return 0;
  saved_error = errno;
  if(saved_error != ENOENT && saved_error != ENOTDIR)
    fprintf(stderr, "katzip: cannot open %s: %s\n", name, strerror(saved_error));
  free(*path);
  *path = NULL;
  return saved_error == ENOENT || saved_error == ENOTDIR ? 1 : -1;
}

static int open_config(const char *program, FILE **file, char **path)
{
  const char *override = getenv("KATZIP_INI");
  char *installed_path;
  int result;
  if(override && *override)
  {
    result = open_named_config(override, file, path);
    if(result == 1)
      fprintf(stderr, "katzip: cannot open %s: %s\n", override, strerror(ENOENT));
    return result == 0 ? 0 : -1;
  }
  result = open_named_config("katzip.ini", file, path);
  if(result != 1)
    return result;
  installed_path = executable_config_path(program);
  if(installed_path)
  {
    result = open_named_config(installed_path, file, path);
    free(installed_path);
    if(result != 1)
      return result;
  }
  *path = strdup("built-in settings");
  if(!*path)
  {
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  *file = fmemopen((void*)katzip_default_ini,
    sizeof(katzip_default_ini) - 1, "r");
  if(!*file)
  {
    fprintf(stderr, "katzip: cannot read built-in settings: %s\n",
      strerror(errno));
    return -1;
  }
  fprintf(stderr, "katzip: warning: katzip.ini not found in current "
    "or executable directory; using built-in compression settings\n");
  return 0;
}

static int valid_config(const turtledeflate_config_t *config, int level)
{
  return config->i_compression_level == level &&
    config->i_maximum_block_size >= TURTLEDEFLATE_MIN_BLOCK_SIZE &&
    config->i_maximum_block_size <= MAX_BLOCK_SIZE &&
    config->i_maximum_subblocks >= TURTLEDEFLATE_MIN_SUBBLOCKS &&
    config->i_maximum_subblocks <= TURTLEDEFLATE_MAX_SUBBLOCKS &&
    config->i_max_block_splitter_iterations > 0 &&
    config->i_max_block_splitter_iterations <= 1000 &&
    config->i_max_internal_block_splitter_iterations > 0 &&
    config->i_max_internal_block_splitter_iterations <= 1000 &&
    config->i_block_splitter_num_points > 0 &&
    config->i_block_splitter_num_points <= TURTLEDEFLATE_BSPLIT_MAX_NUM_POINTS &&
    config->i_block_splitter_center_dist > 0 &&
    config->i_block_splitter_center_dist <= config->i_block_splitter_num_points &&
    config->i_block_splitter_min_range_for_points > 0 &&
    config->i_min_start_fp >= -32 &&
    config->i_max_start_fp <= 32 &&
    config->i_min_start_fp <= config->i_max_start_fp &&
    config->i_num_start_fp >= 2 &&
    config->i_num_start_fp <= TURTLEDEFLATE_MAX_NUM_FP_START / 2 &&
    config->i_verbose >= TURTLEDEFLATE_VERBOSE_NONE &&
    config->i_verbose <= TURTLEDEFLATE_VERBOSE_SQUISHITER;
}

/* The table also assigns one bit to each required Turtledeflate setting. */
static const CONFIG_FIELD config_fields[] = {
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

/* Keep parsing separate from file lookup so both INI sources use one validator. */
static int parse_setting(char *line, int level, turtledeflate_config_t *config,
  int *fast_level, uint32_t *seen)
{
  char *value = strchr(line, '=');
  char *end;
  long number;
  size_t i;
  if(!value)
    return -1;
  *value++ = 0;
  line = trim(line);
  value = trim(value);
  errno = 0;
  number = strtol(value, &end, 10);
  if(!*value || *end || errno == ERANGE ||
    number < INT32_MIN || number > INT32_MAX)
    return -1;
  if(level < 7)
  {
    if(strcmp(line, "level") || *seen || number < 1 || number > 12)
      return -1;
    *fast_level = (int)number;
    *seen = 1;
    return 0;
  }
  for(i = 0; i < ARRAY_N(config_fields); ++i)
  {
    if(strcmp(line, config_fields[i].name) == 0)
      break;
  }
  if(i == ARRAY_N(config_fields) || (*seen & (UINT32_C(1) << i)))
    return -1;
  if(config_fields[i].boolean)
  {
    if(number != 0 && number != 1)
      return -1;
    *(bool*)((unsigned char*)config + config_fields[i].offset) = number != 0;
  }
  else
    *(int32_t*)((unsigned char*)config + config_fields[i].offset) =
      (int32_t)number;
  *seen |= UINT32_C(1) << i;
  return 0;
}

static int config_is_complete(int level, int found, uint32_t seen,
  const turtledeflate_config_t *config)
{
  if(!found)
    return 0;
  if(level < 7)
    return seen == 1;
  return seen == (UINT32_C(1) << ARRAY_N(config_fields)) - 1 &&
    valid_config(config, level);
}

static int read_config(FILE *file, const char *path, int level,
  turtledeflate_config_t *config, int *fast_level)
{
  char section[32];
  char line[256];
  char *text;
  uint32_t seen = 0;
  int active = 0;
  int found = 0;
  int line_number = 0;
  int invalid = 0;
  snprintf(section, sizeof(section),
    level < 7 ? "[libdeflate-%d]" : "[turtledeflate-%d]", level);
  memset(config, 0, sizeof(*config));
  *fast_level = 0;
  while(fgets(line, sizeof(line), file))
  {
    ++line_number;
    if(!strchr(line, '\n') && !feof(file))
    {
      invalid = 1;
      break;
    }
    text = trim(line);
    if(!*text || *text == '#' || *text == ';')
      continue;
    if(*text == '[')
    {
      active = strcmp(text, section) == 0;
      if(active)
      {
        if(found)
        {
          invalid = 1;
          break;
        }
        found = 1;
      }
      continue;
    }
    if(active && parse_setting(text, level, config, fast_level, &seen))
    {
      invalid = 1;
      break;
    }
  }
  if(invalid)
  {
    fprintf(stderr, "katzip: invalid setting in %s:%d\n", path, line_number);
    return -1;
  }
  if(ferror(file))
  {
    fprintf(stderr, "katzip: cannot read %s\n", path);
    return -1;
  }
  if(!config_is_complete(level, found, seen, config))
  {
    fprintf(stderr, "katzip: missing or invalid settings in %s %s\n",
      path, section);
    return -1;
  }
  return 0;
}

static int load_config(const char *program, int level,
  turtledeflate_config_t *config, int *fast_level)
{
  char *path = NULL;
  FILE *file;
  int result;
  if(open_config(program, &file, &path))
  {
    free(path);
    return -1;
  }
  result = read_config(file, path, level, config, fast_level);
  fclose(file);
  free(path);
  return result;
}

/* raw DEFLATE is already compressed; this level only sets the ZIP hint bits. */
static int zip_level_hint(int level)
{
  if(level <= 3)
    return 1;
  if(level <= 6)
    return 2;
  if(level <= 8)
    return 6;
  return 9;
}

/* minizip-ng packages raw DEFLATE without recompressing it. */
static mz_zip_file zip_file_info(const ENTRY *entry, int method)
{
  mz_zip_file info;
  memset(&info, 0, sizeof(info));
  info.version_madeby = (3 << 8) | 20;
  info.version_needed = 20;
  info.flag = entry->flags;
  info.compression_method = method;
  info.uncompressed_size = entry->expected_size;
  info.zip64 = MZ_ZIP64_DISABLE;
  info.modified_date = entry->mtime;
  info.filename = entry->name;
  info.filename_size = entry->name_len;
  info.external_fa = entry->mode << 16;
  return info;
}

static int open_zip_entry(void *zip, const ENTRY *entry, int method,
  int level)
{
  mz_zip_file info = zip_file_info(entry, method);
  return mz_zip_entry_write_open(zip, &info, level,
    1, NULL) == MZ_OK ? 0 : -1;
}

static int close_zip_entry(void *zip, const ENTRY *entry, uint32_t crc)
{
  return mz_zip_entry_write_close(zip, crc ^ UINT32_MAX,
    entry->compressed_size, entry->size) == MZ_OK ? 0 : -1;
}

static int write_zip_bytes(void *zip, const unsigned char *data,
  size_t size)
{
  size_t offset = 0;
  while(offset < size)
  {
    size_t chunk = size - offset;
    if(chunk > 1048576)
      chunk = 1048576;
    if(mz_zip_entry_write(zip, data + offset, (int32_t)chunk) !=
      (int32_t)chunk)
      return -1;
    offset += chunk;
  }
  return 0;
}

/* Small files fit in memory, so libdeflate can choose Store when useful. */
static int compress_small_data(void *zip, ENTRY *entry, FILE *in,
  struct libdeflate_compressor *compressor, unsigned char *input,
  unsigned char *output, int zip_level, PROGRESS *progress, uint32_t *crc)
{
  size_t size = entry->expected_size;
  size_t compressed_size;
  int method = MZ_COMPRESS_METHOD_DEFLATE;
  if(fread(input, 1, size, in) != size || fgetc(in) != EOF || ferror(in))
    return -1;
  entry->size = (uint32_t)size;
  *crc = update_crc(*crc, input, size);
  progress_block(progress, entry->size);
  if(size)
    compressed_size = libdeflate_deflate_compress(compressor, input,
      size, output, size + 16);
  else
  {
    output[0] = 0x03;
    output[1] = 0x00;
    compressed_size = 2;
  }
  if(size && (!compressed_size || compressed_size >= size))
    method = MZ_COMPRESS_METHOD_STORE;
  if(open_zip_entry(zip, entry, method, zip_level))
    return -1;
  entry->compressed_size = method == MZ_COMPRESS_METHOD_STORE ?
    size : compressed_size;
  if(write_zip_bytes(zip, method == MZ_COMPRESS_METHOD_STORE ?
    input : output, (size_t)entry->compressed_size))
    return -1;
  progress_update(progress, entry->size);
  return 0;
}

static int compress_small_entry(void *zip, ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress, uint32_t *crc)
{
  size_t size = entry->expected_size;
  unsigned char *input = malloc(size ? size : 1);
  unsigned char *output = malloc(size + 16);
  struct libdeflate_compressor *compressor =
    libdeflate_alloc_compressor(level);
  int result = -1;
  if(input && output && compressor)
    result = compress_small_data(zip, entry, in, compressor, input,
      output, zip_level, progress, crc);
  if(compressor)
    libdeflate_free_compressor(compressor);
  free(output);
  free(input);
  return result;
}

/* zlib keeps memory bounded when a file is too large for libdeflate. */
static int deflate_stream_chunk(void *zip, z_stream *stream,
  unsigned char *output, int flush, ENTRY *entry)
{
  int result;
  do
  {
    size_t produced;
    stream->next_out = output;
    stream->avail_out = 1048576;
    result = deflate(stream, flush);
    if(result != Z_OK && result != Z_STREAM_END)
      return -1;
    produced = 1048576 - stream->avail_out;
    if(write_zip_bytes(zip, output, produced))
      return -1;
    entry->compressed_size += produced;
  } while(stream->avail_in || stream->avail_out == 0 ||
    (flush == Z_FINISH && result != Z_STREAM_END));
  return 0;
}

static int compress_stream_data(void *zip, ENTRY *entry, FILE *in,
  z_stream *stream, unsigned char *input, unsigned char *output,
  PROGRESS *progress, uint32_t *crc)
{
  size_t size;
  do
  {
    size = fread(input, 1, 1048576, in);
    if(ferror(in) || (uint64_t)entry->size + size > entry->expected_size)
      return -1;
    *crc = update_crc(*crc, input, size);
    entry->size += (uint32_t)size;
    progress_block(progress, (uint32_t)size);
    stream->next_in = input;
    stream->avail_in = (uInt)size;
    if(deflate_stream_chunk(zip, stream, output,
      size ? Z_NO_FLUSH : Z_FINISH, entry))
      return -1;
    progress_update(progress, entry->size);
  } while(size);
  return entry->size == entry->expected_size ? 0 : -1;
}

static int compress_stream_entry(void *zip, ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress, uint32_t *crc)
{
  unsigned char *input = malloc(1048576);
  unsigned char *output = malloc(1048576);
  z_stream stream;
  int result = -1;
  if(!input || !output)
  {
    free(output);
    free(input);
    return -1;
  }
  memset(&stream, 0, sizeof(stream));
  if(deflateInit2(&stream, level > 9 ? 9 : level, Z_DEFLATED,
    -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
  {
    free(output);
    free(input);
    return -1;
  }
  if(!open_zip_entry(zip, entry, MZ_COMPRESS_METHOD_DEFLATE, zip_level))
    result = compress_stream_data(zip, entry, in, &stream,
      input, output, progress, crc);
  deflateEnd(&stream);
  free(output);
  free(input);
  return result;
}

static int write_fast_entry(void *zip, ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress)
{
  uint32_t crc = UINT32_MAX;
  int result;
  progress_start(progress, entry, NULL);
  if(entry->expected_size <= FAST_FILE_LIMIT)
    result = compress_small_entry(zip, entry, in, level,
      zip_level, progress, &crc);
  else
    result = compress_stream_entry(zip, entry, in, level,
      zip_level, progress, &crc);
  if(!result)
    result = close_zip_entry(zip, entry, crc);
  progress_finish(progress, result == 0);
  return result;
}

typedef struct {
  unsigned char *buffer;
  void *compressor;
  FILE *temporary;
  ECT_JOB ect;
  uint32_t crc;
  uint64_t compressed_size;
} TURTLE_WORK;

static void finish_turtle_work(TURTLE_WORK *work)
{
  if(work->ect.started)
    pthread_join(work->ect.thread, NULL);
  free(work->ect.input);
  free(work->ect.output);
  if(work->temporary)
    fclose(work->temporary);
  if(work->compressor)
    turtledeflate_destroy(work->compressor);
  free(work->buffer);
}

static int prepare_turtle_work(TURTLE_WORK *work, FILE *in,
  const ENTRY *entry, const turtledeflate_config_t *config)
{
  memset(work, 0, sizeof(*work));
  work->crc = UINT32_MAX;
  work->buffer = malloc((size_t)config->i_maximum_block_size);
  if(!work->buffer)
    return -1;
  if(config->i_compression_level == 9 &&
    start_ect_job(&work->ect, in, entry->expected_size))
    return -1;
  if(work->ect.started)
  {
    work->temporary = tmpfile();
    if(!work->temporary)
      return -1;
  }
  return 0;
}

static int write_deflate_chunk(void *zip, FILE *temporary,
  const unsigned char *data, size_t size)
{
  if(temporary)
    return fwrite(data, 1, size, temporary) == size ? 0 : -1;
  return write_zip_bytes(zip, data, size);
}

static int input_has_more(FILE *in)
{
  int next = fgetc(in);
  if(next == EOF)
    return ferror(in) ? -1 : 0;
  return ungetc(next, in) == EOF ? -1 : 1;
}

static int compress_turtle_blocks(void *zip, ENTRY *entry, FILE *in,
  const turtledeflate_config_t *config, TURTLE_WORK *work,
  PROGRESS *progress)
{
  turtledeflate_config_t compressor_config = *config;
  unsigned char *compressed;
  size_t size = fread(work->buffer, 1,
    (size_t)config->i_maximum_block_size, in);
  if(ferror(in))
    return -1;
  if(size)
  {
    if(!turtledeflate_create(&work->compressor, &compressor_config))
      return -1;
    turtledeflate_set_progress_callback(work->compressor,
      progress_callback, progress);
  }
  while(size)
  {
    int more = input_has_more(in);
    int compressed_size;
    if(more < 0 || (uint64_t)entry->size + size > UINT32_MAX)
      return -1;
    work->crc = update_crc(work->crc, work->buffer, size);
    entry->size += (uint32_t)size;
    progress_block(progress, (uint32_t)size);
    compressed_size = turtledeflate_block(work->compressor,
      (int32_t)size, work->buffer, &compressed, NULL, !more);
    if(compressed_size < 0 || write_deflate_chunk(zip,
      work->temporary, compressed, (size_t)compressed_size))
      return -1;
    work->compressed_size += (uint32_t)compressed_size;
    progress_update(progress, entry->size);
    if(!more)
      break;
    size = fread(work->buffer, 1,
      (size_t)config->i_maximum_block_size, in);
    if(!size || ferror(in))
      return -1;
  }
  if(!entry->size)
  {
    const unsigned char empty_deflate[] = {0x03, 0x00};
    if(write_deflate_chunk(zip, work->temporary,
      empty_deflate, sizeof(empty_deflate)))
      return -1;
    work->compressed_size = sizeof(empty_deflate);
  }
  return entry->size == entry->expected_size ? 0 : -1;
}

/* Level 9 tries ECT and Turtledeflate concurrently, then writes the winner. */
static int write_turtle_winner(void *zip, ENTRY *entry,
  const turtledeflate_config_t *config, TURTLE_WORK *work,
  PROGRESS *progress)
{
  uint64_t chosen_size = work->compressed_size;
  const unsigned char *data = NULL;
  size_t offset = 0;
  if(!work->ect.started)
    return 0;
  progress_wait(progress);
  pthread_join(work->ect.thread, NULL);
  work->ect.started = 0;
  if(work->ect.crc != (work->crc ^ UINT32_MAX))
    return -1;
  if(work->ect.output && work->ect.output_size < chosen_size &&
    work->ect.output_size <= UINT32_MAX)
  {
    chosen_size = work->ect.output_size;
    data = work->ect.output;
  }
  if(open_zip_entry(zip, entry, MZ_COMPRESS_METHOD_DEFLATE,
    zip_level_hint(config->i_compression_level)))
    return -1;
  if(!data && fseek(work->temporary, 0, SEEK_SET))
    return -1;
  while(offset < chosen_size)
  {
    size_t chunk = (size_t)(chosen_size - offset);
    if(chunk > 65536)
      chunk = 65536;
    if(!data && fread(work->buffer, 1, chunk,
      work->temporary) != chunk)
      return -1;
    if(write_zip_bytes(zip, data ? data + offset :
      work->buffer, chunk))
      return -1;
    offset += chunk;
  }
  work->compressed_size = chosen_size;
  return 0;
}

static int write_entry(void *zip, ENTRY *entry, FILE *in,
  const turtledeflate_config_t *config, PROGRESS *progress)
{
  TURTLE_WORK work;
  int result;
  int started = 0;
  result = prepare_turtle_work(&work, in, entry, config);
  if(!result && !work.temporary)
    result = open_zip_entry(zip, entry, MZ_COMPRESS_METHOD_DEFLATE,
      zip_level_hint(config->i_compression_level));
  if(!result)
  {
    progress_start(progress, entry, config);
    started = 1;
    result = compress_turtle_blocks(zip, entry, in, config,
      &work, progress);
  }
  if(!result)
    result = write_turtle_winner(zip, entry, config, &work, progress);
  entry->compressed_size = work.compressed_size;
  if(!result)
    result = close_zip_entry(zip, entry, work.crc);
  finish_turtle_work(&work);
  if(started)
    progress_finish(progress, result == 0);
  return result;
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

static int inspect_input(const char *path, const char *name,
  struct stat *file_stat)
{
  if(!valid_name(name) || strlen(name) > UINT16_MAX ||
    stat(path, file_stat) || !S_ISREG(file_stat->st_mode) ||
    file_stat->st_size < 0 ||
    (uint64_t)file_stat->st_size > UINT32_MAX)
  {
    fprintf(stderr, "katzip: invalid input file: %s\n", path);
    return -1;
  }
  return 0;
}

/* Return 1 when recursion has already discovered this file. */
static int already_added(const ENTRY_LIST *list, const char *path,
  const char *name, const struct stat *file_stat, int recursive)
{
  size_t i;
  if(list->archive_exists &&
    list->archive_stat.st_dev == file_stat->st_dev &&
    list->archive_stat.st_ino == file_stat->st_ino)
  {
    if(!recursive)
      fprintf(stderr, "katzip: archive is an input file: %s\n", path);
    return recursive ? 1 : -1;
  }
  for(i = 0; i < list->count; ++i)
  {
    if(strcmp(name, list->entries[i].name) == 0)
    {
      if(!recursive)
        fprintf(stderr, "katzip: duplicate entry: %s\n", name);
      return recursive ? 1 : -1;
    }
  }
  return 0;
}

static int reserve_entry(ENTRY_LIST *list)
{
  ENTRY *grown;
  size_t capacity;
  if(list->count == UINT16_MAX)
  {
    fprintf(stderr, "katzip: too many files\n");
    return -1;
  }
  if(list->count < list->capacity)
    return 0;
  capacity = list->capacity ? list->capacity * 2 : 16;
  if(capacity > UINT16_MAX)
    capacity = UINT16_MAX;
  grown = realloc(list->entries, capacity * sizeof(*grown));
  if(!grown)
  {
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  list->entries = grown;
  list->capacity = capacity;
  return 0;
}

static int append_entry(ENTRY_LIST *list, const char *path,
  const char *name, const struct stat *file_stat)
{
  ENTRY *entry = &list->entries[list->count];
  memset(entry, 0, sizeof(*entry));
  entry->path = copy_text(path);
  entry->name = copy_text(name);
  if(!entry->path || !entry->name)
  {
    free((void*)entry->path);
    free((void*)entry->name);
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  entry->name_len = (uint16_t)strlen(name);
  entry->flags = (uint16_t)(valid_utf8(name) ? MZ_ZIP_FLAG_UTF8 : 0);
  entry->mode = (uint32_t)file_stat->st_mode;
  entry->expected_size = (uint32_t)file_stat->st_size;
  entry->mtime = file_stat->st_mtime;
  ++list->count;
  return 0;
}

static int add_entry(ENTRY_LIST *list, const char *path, int recursive)
{
  const char *name = path;
  struct stat file_stat;
  int duplicate;
  while(name[0] == '.' && name[1] == '/')
    name += 2;
  if(inspect_input(path, name, &file_stat))
    return -1;
  duplicate = already_added(list, path, name, &file_stat, recursive);
  if(duplicate)
    return duplicate < 0 ? -1 : 0;
  if(reserve_entry(list))
    return -1;
  return append_entry(list, path, name, &file_stat);
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

static int matches_masks(const char **masks, size_t count,
  const char *relative, const char *basename)
{
  size_t i;
  if(!count)
    return 1;
  for(i = 0; i < count; ++i)
  {
    if(strchr(masks[i], '/'))
    {
      if(fnmatch(masks[i], relative, FNM_PATHNAME | FNM_PERIOD) == 0)
        return 1;
    }
    else if(fnmatch(masks[i], basename, FNM_PERIOD) == 0)
      return 1;
  }
  return 0;
}

static int walk_directory(ENTRY_LIST *list, const char *directory,
  const char *root, const char **masks, size_t mask_count,
  int recursive)
{
  DIR *stream = opendir(directory);
  struct dirent *item;
  struct stat file_stat;
  char *path;
  const char *relative;
  int status = 0;
  if(!stream)
  {
    fprintf(stderr, "katzip: cannot open directory %s: %s\n", directory, strerror(errno));
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
      fprintf(stderr, "katzip: cannot inspect %s: %s\n", path, strerror(errno));
      status = -1;
    }
    else if(S_ISDIR(file_stat.st_mode) && recursive)
      status = walk_directory(list, path, root, masks, mask_count, recursive);
    else if(S_ISREG(file_stat.st_mode))
    {
      relative = strcmp(root, ".") == 0 ? path : path + strlen(root) + 1;
      if(matches_masks(masks, mask_count, relative, item->d_name))
        status = add_entry(list, path, 1);
    }
    free(path);
    if(status)
      break;
  }
  if(closedir(stream))
    status = -1;
  return status;
}

static int add_argument(ENTRY_LIST *list, const char *argument,
  int recursive, const char **masks, size_t mask_count)
{
  const char *name = argument;
  const char *base;
  struct stat file_stat;
  char *root;
  size_t prefix;
  int status;
  while(name[0] == '.' && name[1] == '/')
    name += 2;
  if(!valid_name(name))
  {
    fprintf(stderr, "katzip: invalid input: %s\n", argument);
    return -1;
  }
  if(!recursive)
    return add_entry(list, argument, 0);
  if(!stat(argument, &file_stat) && S_ISDIR(file_stat.st_mode))
  {
    root = copy_text(argument);
    if(!root)
      return -1;
    prefix = strlen(root);
    while(prefix > 1 && root[prefix - 1] == '/')
      root[--prefix] = 0;
    status = walk_directory(list, root, root, masks, mask_count, 1);
    free(root);
    return status;
  }
  if(!stat(argument, &file_stat) && S_ISREG(file_stat.st_mode))
  {
    base = strrchr(argument, '/');
    base = base ? base + 1 : argument;
    if(!matches_masks(masks, mask_count, name, base))
      return 0;
    return add_entry(list, argument, 1);
  }
  fprintf(stderr, "katzip: invalid input: %s\n", argument);
  return -1;
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
}

static void print_help(FILE *stream)
{
  fputs("KATZip v1.1 - Deflating with extreme devotion.\n"
    "Dedicated to the memory of Phil Katz (1962-2000), the father of ZIP.\n"
    "\n"
    "Usage:   katzip [-1..-9] [-r] <archive.zip> [[@]input_files...]\n"
    "Example: katzip APPNOTE APPNOTE.TXT\n", stream);
}

typedef struct {
  int level;
  int recursive;
  int archive_arg;
} OPTIONS;

typedef struct {
  char *temporary_path;
  void *writer;
  void *zip;
  void *reader;
  PROGRESS progress;
  int progress_ready;
  int temporary_created;
  int handlers_active;
  struct sigaction previous_int;
  struct sigaction previous_term;
  sigset_t interrupt_mask;
} ARCHIVE_OUTPUT;

/* A positive result means a help or defaults request was handled. */
static int parse_options(int argc, char **argv, OPTIONS *options)
{
  options->level = 7;
  options->recursive = 0;
  options->archive_arg = 1;
  while(options->archive_arg < argc &&
    argv[options->archive_arg][0] == '-')
  {
    const char *argument = argv[options->archive_arg];
    if(strcmp(argument, "--print-default-ini") == 0)
    {
      if(fputs(katzip_default_ini, stdout) == EOF ||
        fflush(stdout) == EOF)
      {
        fprintf(stderr, "katzip: cannot write default settings\n");
        return -1;
      }
      return 1;
    }
    if(strcmp(argument, "--help") == 0 ||
      strcmp(argument, "-h") == 0)
    {
      print_help(stdout);
      return 1;
    }
    if(strcmp(argument, "--") == 0)
    {
      ++options->archive_arg;
      break;
    }
    if(strcmp(argument, "-r") == 0)
      options->recursive = 1;
    else if(argument[1] >= '1' && argument[1] <= '9' &&
      !argument[2])
      options->level = argument[1] - '0';
    else
    {
      fprintf(stderr, "katzip: unknown option: %s\n", argument);
      return -1;
    }
    ++options->archive_arg;
  }
  if(options->archive_arg == argc)
  {
    print_help(stderr);
    return -1;
  }
  return 0;
}

/* Read masks before visiting explicit files or directories. */
static int collect_entries(ENTRY_LIST *list, int argc, char **argv,
  const OPTIONS *options)
{
  const char **masks = calloc((size_t)argc, sizeof(*masks));
  size_t mask_count = 0;
  int source_count = 0;
  int result = 0;
  int i;
  if(!masks)
  {
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  for(i = options->archive_arg + 1; i < argc; ++i)
  {
    if(argv[i][0] != '@')
    {
      ++source_count;
      continue;
    }
    if(!argv[i][1])
    {
      fprintf(stderr, "katzip: empty file mask\n");
      result = -1;
      break;
    }
    masks[mask_count++] = argv[i] + 1;
  }
  if(!result && !source_count)
  {
    if(!mask_count)
      masks[mask_count++] = "*";
    result = walk_directory(list, ".", ".", masks,
      mask_count, options->recursive);
  }
  for(i = options->archive_arg + 1; !result && i < argc; ++i)
  {
    if(argv[i][0] != '@')
      result = add_argument(list, argv[i], options->recursive,
        masks, mask_count);
  }
  free(masks);
  if(!result && !list->count)
  {
    fprintf(stderr, "katzip: no files to archive\n");
    return -1;
  }
  return result;
}

static mode_t archive_mode(const ENTRY_LIST *list)
{
  mode_t process_umask = umask(0);
  umask(process_umask);
  if(list->archive_exists)
    return list->archive_stat.st_mode & 0777;
  return 0666 & ~process_umask;
}

/* Signals stay blocked between creating the file and installing cleanup. */
static int install_interrupt_handlers(ARCHIVE_OUTPUT *output)
{
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = remove_temp_on_signal;
  sigemptyset(&action.sa_mask);
  signal_temp_path = output->temporary_path;
  if(sigaction(SIGINT, &action, &output->previous_int))
  {
    signal_temp_path = NULL;
    return -1;
  }
  if(sigaction(SIGTERM, &action, &output->previous_term))
  {
    int saved_error = errno;
    sigaction(SIGINT, &output->previous_int, NULL);
    signal_temp_path = NULL;
    errno = saved_error;
    return -1;
  }
  output->handlers_active = 1;
  return 0;
}

static int create_temporary_archive(ARCHIVE_OUTPUT *output,
  const char *archive_path)
{
  sigset_t previous_mask;
  int file;
  int saved_error;
  output->temporary_path = malloc(strlen(archive_path) +
    sizeof(".tmp.XXXXXX"));
  if(!output->temporary_path)
  {
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  sprintf(output->temporary_path, "%s.tmp.XXXXXX", archive_path);
  sigemptyset(&output->interrupt_mask);
  sigaddset(&output->interrupt_mask, SIGINT);
  sigaddset(&output->interrupt_mask, SIGTERM);
  if(sigprocmask(SIG_BLOCK, &output->interrupt_mask,
    &previous_mask))
  {
    fprintf(stderr, "katzip: cannot block interrupts: %s\n",
      strerror(errno));
    return -1;
  }
  file = mkstemp(output->temporary_path);
  if(file < 0)
  {
    saved_error = errno;
    sigprocmask(SIG_SETMASK, &previous_mask, NULL);
    fprintf(stderr, "katzip: cannot create temporary archive: %s\n",
      strerror(saved_error));
    return -1;
  }
  output->temporary_created = 1;
  if(install_interrupt_handlers(output))
  {
    saved_error = errno;
    close(file);
    sigprocmask(SIG_SETMASK, &previous_mask, NULL);
    fprintf(stderr, "katzip: cannot handle interrupts: %s\n",
      strerror(saved_error));
    return -1;
  }
  sigprocmask(SIG_SETMASK, &previous_mask, NULL);
  if(close(file))
  {
    fprintf(stderr, "katzip: cannot close temporary archive: %s\n",
      strerror(errno));
    return -1;
  }
  return 0;
}

static void restore_signal_handlers(ARCHIVE_OUTPUT *output)
{
  sigset_t previous_mask;
  if(!output->handlers_active)
    return;
  sigprocmask(SIG_BLOCK, &output->interrupt_mask, &previous_mask);
  sigaction(SIGINT, &output->previous_int, NULL);
  sigaction(SIGTERM, &output->previous_term, NULL);
  signal_temp_path = NULL;
  sigprocmask(SIG_SETMASK, &previous_mask, NULL);
}

static void finish_archive_output(ARCHIVE_OUTPUT *output)
{
  if(output->reader)
    mz_zip_reader_delete(&output->reader);
  if(output->writer)
    mz_zip_writer_delete(&output->writer);
  if(output->progress_ready)
    progress_destroy(&output->progress);
  restore_signal_handlers(output);
  if(output->temporary_created)
    remove(output->temporary_path);
  free(output->temporary_path);
}

static int write_archive_entries(ARCHIVE_OUTPUT *output,
  ENTRY_LIST *list, const OPTIONS *options,
  const turtledeflate_config_t *config, int fast_level)
{
  size_t i;
  output->writer = mz_zip_writer_create();
  if(!output->writer || mz_zip_writer_open_file(output->writer,
    output->temporary_path, 0, 0) != MZ_OK ||
    mz_zip_writer_get_zip_handle(output->writer,
      &output->zip) != MZ_OK)
    return -1;
  for(i = 0; i < list->count; ++i)
  {
    FILE *in = fopen(list->entries[i].path, "rb");
    int result;
    if(!in)
    {
      fprintf(stderr, "katzip: cannot archive %s\n",
        list->entries[i].path);
      return -1;
    }
    if(options->level < 7)
      result = write_fast_entry(output->zip, &list->entries[i], in,
        fast_level, zip_level_hint(options->level), &output->progress);
    else
      result = write_entry(output->zip, &list->entries[i], in,
        config, &output->progress);
    if(fclose(in))
      result = -1;
    if(result)
    {
      fprintf(stderr, "katzip: cannot archive %s\n",
        list->entries[i].path);
      return -1;
    }
  }
  if(mz_zip_writer_close(output->writer) != MZ_OK)
    return -1;
  mz_zip_writer_delete(&output->writer);
  return 0;
}

/* Check the exact metadata size and ask minizip-ng to read the result. */
static int validate_archive(ARCHIVE_OUTPUT *output,
  const ENTRY_LIST *list)
{
  struct stat output_stat;
  uint64_t expected_size = 22;
  size_t i;
  for(i = 0; i < list->count; ++i)
  {
    expected_size += list->entries[i].compressed_size + 76 +
      2 * list->entries[i].name_len;
  }
  if(expected_size > UINT32_MAX ||
    stat(output->temporary_path, &output_stat) ||
    output_stat.st_size < 0 ||
    (uint64_t)output_stat.st_size != expected_size)
  {
    errno = EIO;
    return -1;
  }
  output->reader = mz_zip_reader_create();
  if(!output->reader || mz_zip_reader_open_file(output->reader,
    output->temporary_path) != MZ_OK)
  {
    errno = EIO;
    return -1;
  }
  mz_zip_reader_delete(&output->reader);
  return 0;
}

/* Publish only a complete archive, preserving an existing output on error. */
static int publish_archive(ARCHIVE_OUTPUT *output,
  const char *archive_path, mode_t mode)
{
  int file = open(output->temporary_path, O_RDONLY);
  int result;
  int saved_error;
  if(file < 0)
    return -1;
  result = fchmod(file, mode);
  if(!result)
    result = fsync(file);
  saved_error = errno;
  if(close(file) && !result)
  {
    result = -1;
    saved_error = errno;
  }
  if(result)
  {
    errno = saved_error;
    return -1;
  }
  if(rename(output->temporary_path, archive_path))
    return -1;
  output->temporary_created = 0;
  return 0;
}

static int run_archive(int argc, char **argv, const OPTIONS *options,
  const turtledeflate_config_t *config, int fast_level)
{
  ENTRY_LIST list = {0};
  ARCHIVE_OUTPUT output = {0};
  char *archive_path = archive_name(argv[options->archive_arg]);
  mode_t mode;
  int result;
  if(!archive_path)
  {
    fprintf(stderr, "katzip: invalid archive name\n");
    return 1;
  }
  list.archive_exists = stat(archive_path, &list.archive_stat) == 0;
  mode = archive_mode(&list);
  result = collect_entries(&list, argc, argv, options);
  if(!result)
    result = create_temporary_archive(&output, archive_path);
  if(!result)
  {
    result = progress_init(&output.progress);
    if(result)
      fprintf(stderr, "katzip: cannot start progress display\n");
    else
      output.progress_ready = 1;
  }
  if(!result)
  {
    result = write_archive_entries(&output, &list, options,
      config, fast_level);
    if(!result)
      result = validate_archive(&output, &list);
    if(!result)
      result = publish_archive(&output, archive_path, mode);
    if(result)
    {
      int error_number = errno ? errno : EIO;
      fprintf(stderr, "katzip: failed to write archive %s: %s\n",
        archive_path, strerror(error_number));
    }
  }
  finish_archive_output(&output);
  free_entries(&list);
  free(archive_path);
  return result ? 1 : 0;
}

int main(int argc, char **argv)
{
  OPTIONS options;
  turtledeflate_config_t config;
  int fast_level = 0;
  int result = parse_options(argc, argv, &options);
  if(result)
    return result < 0 ? 1 : 0;
  if(load_config(argv[0], options.level, &config, &fast_level))
    return 1;
  return run_archive(argc, argv, &options, &config, fast_level);
}
