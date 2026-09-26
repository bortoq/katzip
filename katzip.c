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
  turtledeflate_config_t turtle;
  ZopfliOptions ect;
  int fast_level;
  int zlib_level;
  uint64_t zlib_after;
  int have_fast;
  int have_ect;
  int have_turtle;
} COMPRESSION_CONFIG;

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
  ZopfliOptions options;
  int started;
} ECT_JOB;

#define ARRAY_N(A) (sizeof(A) / sizeof((A)[0]))
/* the largest upstream preset also bounds Turtledeflate's int32 allocation arithmetic */
#define MAX_BLOCK_SIZE 1000000
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

typedef struct {
  const char *name;
  size_t offset;
  int minimum;
  int maximum;
} ECT_FIELD;

/* These are the public ECT ZopfliOptions fields, in struct order. */
static const ECT_FIELD ect_fields[] = {
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
  unsigned char bit_position = 0;
  ZopfliDeflate(&job->options, 1, job->input, job->input_size,
    &bit_position, &job->output, &job->output_size);
  free(job->input);
  job->input = NULL;
  return NULL;
}

/* Read a stable snapshot before ECT starts compressing in the worker. */
static int start_ect_job(ECT_JOB *job, FILE *in, uint32_t size,
  const ZopfliOptions *options)
{
  int next;
  memset(job, 0, sizeof(*job));
  job->options = *options;
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

typedef struct {
  unsigned char low;
  unsigned char high;
  unsigned char mask;
  uint32_t minimum;
  int continuations;
} UTF8_LEAD;

/* Reject invalid leading bytes, incomplete sequences and invalid code points. */
static int valid_utf8(const char *name)
{
  static const UTF8_LEAD leads[] = {
    {0xc2, 0xdf, 0x1f, 0x80, 1},
    {0xe0, 0xef, 0x0f, 0x800, 2},
    {0xf0, 0xf4, 0x07, 0x10000, 3}
  };
  const unsigned char *p = (const unsigned char*)name;
  while(*p)
  {
    uint32_t codepoint;
    size_t lead_index;
    int i;
    if(*p < 0x80)
    {
      ++p;
      continue;
    }
    for(lead_index = 0; lead_index < ARRAY_N(leads); ++lead_index)
    {
      if(*p >= leads[lead_index].low && *p <= leads[lead_index].high)
        break;
    }
    if(lead_index == ARRAY_N(leads))
      return 0;
    codepoint = *p++ & leads[lead_index].mask;
    for(i = 0; i < leads[lead_index].continuations; ++i)
    {
      if(!*p || (*p & 0xc0) != 0x80)
        return 0;
      codepoint = (codepoint << 6) | (*p++ & 0x3f);
    }
    if(codepoint < leads[lead_index].minimum ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
      codepoint > 0x10ffff)
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

/* Each built-in mode defines both its engine and its size policy. */
static void use_default_config(int level, COMPRESSION_CONFIG *config)
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

/* Generate the editable file from the same tables as the fallback. */
static int write_default_ini(FILE *file)
{
  int level;
  if(fputs("# One section per katzip level. Multiple compressor setting groups "
    "compete.\n# zlib_after replaces them at or above the given file size; "
    "off disables it.\n# Sizes accept bytes, KiB, MiB and GiB.\n\n",
    file) == EOF)
    return -1;
  for(level = 1; level <= 9; ++level)
  {
    COMPRESSION_CONFIG config;
    use_default_config(level, &config);
    if(fprintf(file, "[%d]\n", level) < 0)
      return -1;
    if(config.have_fast && fprintf(file,
      "libdeflate_level = %d\n", config.fast_level) < 0)
      return -1;
    if(config.have_ect && write_ect_settings(file, &config.ect))
      return -1;
    if(config.have_turtle &&
      write_turtle_settings(file, &config.turtle))
      return -1;
    if(config.zlib_after == UINT64_MAX)
    {
      if(fputs("zlib_after = off\n", file) == EOF)
        return -1;
    }
    else if(fprintf(file, "zlib_after = %llu\n",
      (unsigned long long)config.zlib_after) < 0)
      return -1;
    if(fprintf(file, "zlib_level = %d\n", config.zlib_level) < 0)
      return -1;
    if(level < 9 && fputc('\n', file) == EOF)
      return -1;
  }
  return ferror(file) ? -1 : 0;
}

/* Link publishes a complete file without replacing another process's INI. */
static int create_default_ini(const char *path)
{
  char *temporary = malloc(strlen(path) + sizeof(".tmp.XXXXXX"));
  FILE *file;
  int descriptor;
  int result;
  int saved_error;
  if(!temporary)
  {
    errno = ENOMEM;
    return -1;
  }
  sprintf(temporary, "%s.tmp.XXXXXX", path);
  descriptor = mkstemp(temporary);
  if(descriptor < 0)
  {
    free(temporary);
    return -1;
  }
  file = fdopen(descriptor, "w");
  if(!file)
  {
    saved_error = errno;
    close(descriptor);
    unlink(temporary);
    free(temporary);
    errno = saved_error;
    return -1;
  }
  result = write_default_ini(file);
  if(fflush(file))
    result = -1;
  if(result == 0 && fsync(descriptor))
    result = -1;
  saved_error = errno;
  if(fclose(file))
  {
    result = -1;
    saved_error = errno;
  }
  if(result == 0 && link(temporary, path) && errno != EEXIST)
  {
    result = -1;
    saved_error = errno;
  }
  if(unlink(temporary) && result == 0)
  {
    result = -1;
    saved_error = errno;
  }
  free(temporary);
  if(result)
    errno = saved_error;
  return result;
}

/* 0: opened INI; 1: use compiled presets; -1: invalid existing source. */
static int open_config(const char *program, FILE **file, char **path)
{
  const char *override = getenv("KATZIP_INI");
  char *installed_path;
  int result;
  if(override && *override)
  {
    result = open_named_config(override, file, path);
    if(result == 1)
      fprintf(stderr, "katzip: cannot open %s: %s\n",
        override, strerror(ENOENT));
    return result == 0 ? 0 : -1;
  }
  result = open_named_config("katzip.ini", file, path);
  if(result != 1)
    return result;
  installed_path = executable_config_path(program);
  if(installed_path)
  {
    result = open_named_config(installed_path, file, path);
    if(result == 1)
    {
      if(create_default_ini(installed_path))
      {
        fprintf(stderr, "katzip: warning: cannot create %s: %s; "
          "using built-in compression settings\n",
          installed_path, strerror(errno));
        free(installed_path);
        return 1;
      }
      result = open_named_config(installed_path, file, path);
    }
    free(installed_path);
    if(result != 1)
      return result;
  }
  fprintf(stderr, "katzip: warning: katzip.ini unavailable beside "
    "the executable; using built-in compression settings\n");
  return 1;
}

static int valid_config(const turtledeflate_config_t *config)
{
  return config->i_compression_level >= 1 &&
    config->i_compression_level <= 9 &&
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

typedef struct {
  uint32_t fast;
  uint32_t ect;
  uint32_t turtle;
  int zlib_after;
  int zlib_level;
} CONFIG_SEEN;

static int parse_size(const char *text, uint64_t *size)
{
  char *end;
  unsigned long long number;
  uint64_t multiplier = 1;
  if(strcmp(text, "off") == 0)
  {
    *size = UINT64_MAX;
    return 0;
  }
  if(!isdigit((unsigned char)*text))
    return -1;
  errno = 0;
  number = strtoull(text, &end, 10);
  if(errno == ERANGE)
    return -1;
  if(strcmp(end, "KiB") == 0)
    multiplier = 1024;
  else if(strcmp(end, "MiB") == 0)
    multiplier = UINT64_C(1024) * 1024;
  else if(strcmp(end, "GiB") == 0)
    multiplier = UINT64_C(1024) * 1024 * 1024;
  else if(*end && strcmp(end, "B") != 0)
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

static int parse_ect_setting(const char *name, int number,
  COMPRESSION_CONFIG *config, CONFIG_SEEN *seen)
{
  size_t i;
  for(i = 0; i < ARRAY_N(ect_fields); ++i)
    if(strcmp(name, ect_fields[i].name) == 0)
      break;
  if(i == ARRAY_N(ect_fields) ||
    (seen->ect & (UINT32_C(1) << i)) ||
    number < ect_fields[i].minimum || number > ect_fields[i].maximum)
    return -1;
  if(i == 0)
    config->ect.numiterations = number;
  else
    *(unsigned*)((unsigned char*)&config->ect + ect_fields[i].offset) =
      (unsigned)number;
  seen->ect |= UINT32_C(1) << i;
  return 0;
}

static int parse_turtle_setting(const char *name, int number,
  COMPRESSION_CONFIG *config, CONFIG_SEEN *seen)
{
  size_t i;
  for(i = 0; i < ARRAY_N(config_fields); ++i)
    if(strcmp(name, config_fields[i].name) == 0)
      break;
  if(i == ARRAY_N(config_fields) ||
    (seen->turtle & (UINT32_C(1) << i)))
    return -1;
  if(config_fields[i].boolean)
  {
    if(number != 0 && number != 1)
      return -1;
    *(bool*)((unsigned char*)&config->turtle +
      config_fields[i].offset) = number != 0;
  }
  else
    *(int32_t*)((unsigned char*)&config->turtle +
      config_fields[i].offset) = (int32_t)number;
  seen->turtle |= UINT32_C(1) << i;
  return 0;
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
  {
    if(seen->zlib_after || parse_size(value, &config->zlib_after))
      return -1;
    seen->zlib_after = 1;
    return 0;
  }
  if(parse_number(value, &number))
    return -1;
  if(strcmp(line, "zlib_level") == 0)
  {
    if(seen->zlib_level || number < 1 || number > 9)
      return -1;
    config->zlib_level = number;
    seen->zlib_level = 1;
    return 0;
  }
  if(strcmp(line, "libdeflate_level") == 0)
  {
    if(seen->fast || number < 1 || number > 12)
      return -1;
    config->fast_level = number;
    seen->fast = 1;
    return 0;
  }
  if(strncmp(line, "zopfli_", 7) == 0)
    return parse_ect_setting(line + 7, number, config, seen);
  if(strncmp(line, "turtledeflate_", 14) == 0)
    return parse_turtle_setting(line + 14, number, config, seen);
  return -1;
}

static int config_is_complete(int found, const CONFIG_SEEN *seen,
  COMPRESSION_CONFIG *config)
{
  uint32_t all_ect = (UINT32_C(1) << ARRAY_N(ect_fields)) - 1;
  uint32_t all_turtle = (UINT32_C(1) << ARRAY_N(config_fields)) - 1;
  if(!found || !seen->zlib_after || !seen->zlib_level ||
    (!seen->fast && !seen->ect && !seen->turtle &&
      config->zlib_after != 0) ||
    (seen->ect && seen->ect != all_ect) ||
    (seen->turtle && seen->turtle != all_turtle) ||
    (seen->turtle && !valid_config(&config->turtle)))
    return 0;
  config->have_fast = seen->fast != 0;
  config->have_ect = seen->ect != 0;
  config->have_turtle = seen->turtle != 0;
  return 1;
}

static int read_config(FILE *file, const char *path, int level,
  COMPRESSION_CONFIG *config)
{
  char section[16];
  char line[256];
  char *text;
  CONFIG_SEEN seen = {0};
  int active = 0;
  int found = 0;
  int line_number = 0;
  int invalid = 0;
  snprintf(section, sizeof(section), "[%d]", level);
  memset(config, 0, sizeof(*config));
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
      if(active && found)
      {
        invalid = 1;
        break;
      }
      if(active)
        found = 1;
      continue;
    }
    if(active && parse_setting(text, config, &seen))
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
  if(!config_is_complete(found, &seen, config))
  {
    fprintf(stderr, "katzip: missing or invalid settings in %s %s\n",
      path, section);
    return -1;
  }
  return 0;
}

static int load_config(const char *program, int level,
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
  result = compress_small_entry(zip, entry, in, level,
    zip_level, progress, &crc);
  if(!result)
    result = close_zip_entry(zip, entry, crc);
  progress_finish(progress, result == 0);
  return result;
}

static int write_zlib_entry(void *zip, ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress)
{
  uint32_t crc = UINT32_MAX;
  int result;
  progress_start(progress, entry, NULL);
  result = compress_stream_entry(zip, entry, in, level,
    zip_level, progress, &crc);
  if(!result)
    result = close_zip_entry(zip, entry, crc);
  progress_finish(progress, result == 0);
  return result;
}

/* Read again only when DEFLATE would be larger than the original file. */
static int write_stored_input(void *zip, FILE *in,
  uint32_t size, uint32_t expected_crc)
{
  unsigned char buffer[65536];
  uint32_t crc = UINT32_MAX;
  uint32_t remaining = size;
  if(fseek(in, 0, SEEK_SET))
    return -1;
  while(remaining)
  {
    size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    if(fread(buffer, 1, chunk, in) != chunk)
      return -1;
    crc = update_crc(crc, buffer, chunk);
    if(write_zip_bytes(zip, buffer, chunk))
      return -1;
    remaining -= (uint32_t)chunk;
  }
  if(fgetc(in) != EOF || ferror(in) ||
    (crc ^ UINT32_MAX) != expected_crc)
    return -1;
  return 0;
}

/* ECT compresses the entire file at once, so levels 7-8 need no
 * Turtledeflate pass or temporary DEFLATE stream. */
static int write_ect_entry(void *zip, ENTRY *entry, FILE *in,
  const ZopfliOptions *options, int zip_level, PROGRESS *progress)
{
  static const unsigned char empty_deflate[] = {0x03, 0x00};
  ECT_JOB job;
  const unsigned char *compressed = empty_deflate;
  uint32_t crc;
  int store;
  int result;
  if(start_ect_job(&job, in, entry->expected_size, options))
  {
    free(job.input);
    return -1;
  }
  if(entry->expected_size && !job.started && !job.input)
    return -1;
  progress_start(progress, entry, NULL);
  if(job.started)
  {
    progress_wait(progress);
    pthread_join(job.thread, NULL);
    job.started = 0;
  }
  else if(job.input)
    ect_worker(&job);
  entry->size = entry->expected_size;
  store = entry->size &&
    (!job.output || !job.output_size ||
      job.output_size >= entry->size);
  entry->compressed_size = store ? entry->size :
    entry->size ? job.output_size : sizeof(empty_deflate);
  crc = job.crc ^ UINT32_MAX;
  if(entry->size)
    compressed = job.output;
  result = entry->compressed_size <= UINT32_MAX ? 0 : -1;
  if(!result)
    result = open_zip_entry(zip, entry, store ?
      MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE,
      zip_level);
  if(!result)
  {
    if(store)
      result = write_stored_input(zip, in, entry->size, job.crc);
    else
      result = write_zip_bytes(zip, compressed,
        (size_t)entry->compressed_size);
  }
  if(!result)
    result = close_zip_entry(zip, entry, crc);
  free(job.input);
  free(job.output);
  progress_finish(progress, result == 0);
  return result;
}

typedef struct {
  unsigned char *buffer;
  void *compressor;
  FILE *temporary;
  uint32_t crc;
  uint64_t compressed_size;
} TURTLE_WORK;

static void finish_turtle_work(TURTLE_WORK *work)
{
  if(work->temporary)
    fclose(work->temporary);
  if(work->compressor)
    turtledeflate_destroy(work->compressor);
  free(work->buffer);
}

static int prepare_turtle_work(TURTLE_WORK *work,
  const turtledeflate_config_t *config)
{
  memset(work, 0, sizeof(*work));
  work->crc = UINT32_MAX;
  work->buffer = malloc((size_t)config->i_maximum_block_size);
  return work->buffer ? 0 : -1;
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

static int write_entry(void *zip, ENTRY *entry, FILE *in,
  const turtledeflate_config_t *config, int zip_level,
  PROGRESS *progress)
{
  TURTLE_WORK work;
  int result;
  int started = 0;
  result = prepare_turtle_work(&work, config);
  if(!result)
    result = open_zip_entry(zip, entry, MZ_COMPRESS_METHOD_DEFLATE,
      zip_level);
  if(!result)
  {
    progress_start(progress, entry, config);
    started = 1;
    result = compress_turtle_blocks(zip, entry, in, config,
      &work, progress);
  }
  entry->compressed_size = work.compressed_size;
  if(!result)
    result = close_zip_entry(zip, entry, work.crc);
  finish_turtle_work(&work);
  if(started)
    progress_finish(progress, result == 0);
  return result;
}

typedef struct {
  unsigned char *data;
  FILE *file;
  uint64_t size;
  uint32_t crc;
} CANDIDATE;

static void finish_candidate(CANDIDATE *candidate)
{
  free(candidate->data);
  if(candidate->file)
    fclose(candidate->file);
}

/* libdeflate needs a complete input buffer. Retain only its DEFLATE result. */
static int make_fast_candidate(CANDIDATE *candidate, FILE *in,
  uint32_t size, int level)
{
  struct libdeflate_compressor *compressor;
  unsigned char *input;
  size_t result;
#if SIZE_MAX <= UINT32_MAX
  if(size > SIZE_MAX - 16)
    return -1;
#endif
  input = malloc(size ? (size_t)size : 1);
  candidate->data = malloc((size_t)size + 16);
  compressor = libdeflate_alloc_compressor(level);
  if(!input || !candidate->data || !compressor)
  {
    free(input);
    if(compressor)
      libdeflate_free_compressor(compressor);
    return -1;
  }
  if(fseek(in, 0, SEEK_SET) || fread(input, 1, size, in) != size ||
    fgetc(in) != EOF || ferror(in))
  {
    free(input);
    libdeflate_free_compressor(compressor);
    return -1;
  }
  candidate->crc = update_crc(UINT32_MAX, input, size) ^ UINT32_MAX;
  if(size)
    result = libdeflate_deflate_compress(compressor, input, size,
      candidate->data, (size_t)size + 16);
  else
  {
    candidate->data[0] = 0x03;
    candidate->data[1] = 0x00;
    result = 2;
  }
  candidate->size = result ? result : UINT64_MAX;
  free(input);
  libdeflate_free_compressor(compressor);
  return 0;
}

/* Turtledeflate streams to a temporary file so its result need not fit RAM. */
static int make_turtle_candidate(CANDIDATE *candidate, FILE *in,
  const ENTRY *entry, const turtledeflate_config_t *config,
  PROGRESS *progress)
{
  TURTLE_WORK work = {0};
  ENTRY copy = *entry;
  int result;
  copy.size = 0;
  work.crc = UINT32_MAX;
  work.buffer = malloc((size_t)config->i_maximum_block_size);
  work.temporary = tmpfile();
  if(!work.buffer || !work.temporary || fseek(in, 0, SEEK_SET))
  {
    finish_turtle_work(&work);
    return -1;
  }
  result = compress_turtle_blocks(NULL, &copy, in, config,
    &work, progress);
  if(!result)
  {
    candidate->file = work.temporary;
    candidate->size = work.compressed_size;
    candidate->crc = work.crc ^ UINT32_MAX;
    work.temporary = NULL;
  }
  finish_turtle_work(&work);
  return result;
}

static void collect_ect_candidate(CANDIDATE *candidate, ECT_JOB *job,
  uint32_t size)
{
  if(!size)
  {
    candidate->data = malloc(2);
    if(candidate->data)
    {
      candidate->data[0] = 0x03;
      candidate->data[1] = 0x00;
      candidate->size = 2;
    }
    return;
  }
  candidate->data = job->output;
  candidate->size = job->output && job->output_size ?
    job->output_size : UINT64_MAX;
  candidate->crc = job->crc;
  job->output = NULL;
}

static int copy_candidate(void *zip, CANDIDATE *candidate)
{
  unsigned char buffer[65536];
  uint64_t remaining = candidate->size;
  if(candidate->data)
    return write_zip_bytes(zip, candidate->data, (size_t)remaining);
  if(fseek(candidate->file, 0, SEEK_SET))
    return -1;
  while(remaining)
  {
    size_t chunk = remaining < sizeof(buffer) ?
      (size_t)remaining : sizeof(buffer);
    if(fread(buffer, 1, chunk, candidate->file) != chunk ||
      write_zip_bytes(zip, buffer, chunk))
      return -1;
    remaining -= chunk;
  }
  return 0;
}

static int write_best_candidate(void *zip, ENTRY *entry, FILE *in,
  CANDIDATE *candidates, const COMPRESSION_CONFIG *config, int zip_level)
{
  uint64_t best_size = UINT64_MAX;
  uint32_t crc = 0;
  int best = -1;
  int i;
  int store;
  int result;
  int have_crc = 0;
  for(i = 0; i < 3; ++i)
  {
    int enabled = i == 0 ? config->have_fast :
      i == 1 ? config->have_ect : config->have_turtle;
    if(!enabled)
      continue;
    if(!have_crc)
    {
      crc = candidates[i].crc;
      have_crc = 1;
    }
    else if(candidates[i].crc != crc)
      return -1;
    /* Preserve Turtledeflate's result when it ties with ECT. */
    if(candidates[i].size < best_size ||
      (i == 2 && best >= 0 && candidates[i].size == best_size))
    {
      best_size = candidates[i].size;
      best = i;
    }
  }
  store = entry->expected_size &&
    (best < 0 || best_size >= entry->expected_size);
  if(!store && best < 0)
    return -1;
  entry->size = entry->expected_size;
  entry->compressed_size = store ? entry->size : best_size;
  if(entry->compressed_size > UINT32_MAX)
    return -1;
  result = open_zip_entry(zip, entry, store ?
    MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE,
    zip_level);
  if(!result)
  {
    if(store)
      result = write_stored_input(zip, in, entry->size, crc);
    else
      result = copy_candidate(zip, &candidates[best]);
  }
  if(!result)
    result = close_zip_entry(zip, entry, crc ^ UINT32_MAX);
  return result;
}

/* All enabled compressors process the same input; keep the shortest stream. */
static int write_race_entry(void *zip, ENTRY *entry, FILE *in,
  const COMPRESSION_CONFIG *config, int zip_level, PROGRESS *progress)
{
  CANDIDATE candidates[3] = {{0}};
  ECT_JOB ect = {0};
  int result = 0;
  int i;
  progress_start(progress, entry,
    config->have_turtle ? &config->turtle : NULL);
  if(config->have_ect)
  {
    result = start_ect_job(&ect, in, entry->expected_size,
      &config->ect);
    if(!result && entry->expected_size && !ect.started && !ect.input)
      result = -1;
  }
  if(!result && config->have_fast)
    result = make_fast_candidate(&candidates[0], in,
      entry->expected_size, config->fast_level);
  if(!result && config->have_turtle)
    result = make_turtle_candidate(&candidates[2], in, entry,
      &config->turtle, progress);
  if(ect.started)
  {
    progress_wait(progress);
    pthread_join(ect.thread, NULL);
    ect.started = 0;
  }
  else if(!result && ect.input)
    ect_worker(&ect);
  if(!result && config->have_ect)
  {
    collect_ect_candidate(&candidates[1], &ect,
      entry->expected_size);
    if(entry->expected_size == 0 && !candidates[1].data)
      result = -1;
  }
  if(!result)
    result = write_best_candidate(zip, entry, in,
      candidates, config, zip_level);
  free(ect.input);
  free(ect.output);
  for(i = 0; i < 3; ++i)
    finish_candidate(&candidates[i]);
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

typedef struct {
  int level;
  int recursive;
  int archive_arg;
  int argument_count;
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

/* Keep positional arguments in order while accepting options anywhere. */
static int parse_options(int argc, char **argv, OPTIONS *options)
{
  int next_position = 1;
  int options_done = 0;
  int i;
  options->level = 7;
  options->recursive = 0;
  options->archive_arg = 1;
  for(i = 1; i < argc; ++i)
  {
    const char *argument = argv[i];
    if(!options_done && strcmp(argument, "--") == 0)
    {
      options_done = 1;
      continue;
    }
    if(!options_done && strcmp(argument, "--help") == 0)
    {
      print_help(stdout);
      return 1;
    }
    if(!options_done && strcmp(argument, "-h") == 0)
    {
      print_help(stdout);
      return 1;
    }
    if(!options_done && strcmp(argument, "-r") == 0)
    {
      options->recursive = 1;
      continue;
    }
    if(!options_done && argument[0] == '-' &&
      argument[1] >= '1' && argument[1] <= '9' &&
      !argument[2])
    {
      options->level = argument[1] - '0';
      continue;
    }
    if(!options_done && argument[0] == '-')
    {
      fprintf(stderr, "katzip: unknown option: %s\n", argument);
      return -1;
    }
    argv[next_position++] = argv[i];
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
  const COMPRESSION_CONFIG *config)
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
    if((uint64_t)list->entries[i].expected_size >= config->zlib_after)
      result = write_zlib_entry(output->zip, &list->entries[i], in,
        config->zlib_level, zip_level_hint(options->level),
        &output->progress);
    else if(config->have_fast + config->have_ect +
      config->have_turtle > 1)
      result = write_race_entry(output->zip, &list->entries[i], in,
        config, zip_level_hint(options->level), &output->progress);
    else if(config->have_fast)
      result = write_fast_entry(output->zip, &list->entries[i], in,
        config->fast_level, zip_level_hint(options->level),
        &output->progress);
    else if(config->have_ect)
      result = write_ect_entry(output->zip, &list->entries[i], in,
        &config->ect, zip_level_hint(options->level),
        &output->progress);
    else
      result = write_entry(output->zip, &list->entries[i], in,
        &config->turtle, zip_level_hint(options->level),
        &output->progress);
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

static int run_archive(char **argv, const OPTIONS *options,
  const COMPRESSION_CONFIG *config)
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
  result = collect_entries(&list, options->argument_count, argv, options);
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
    result = write_archive_entries(&output, &list, options, config);
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
  COMPRESSION_CONFIG config;
  int result = parse_options(argc, argv, &options);
  if(result)
    return result < 0 ? 1 : 0;
  if(load_config(argv[0], options.level, &config))
    return 1;
  return run_archive(argv, &options, &config);
}
