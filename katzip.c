#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
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

static void show_progress(PROGRESS *progress, int timer_tick)
{
  const ENTRY *entry = progress->entry;
  double done = progress->done;
  double percent;
  int width;
  int i;
  if(progress->pass && progress->pass_total && done < entry->expected_size)
  {
    double work = progress->pass - 1 + (double)progress->pass_done / progress->pass_total;
    done += progress->block_size * work / (work + progress->pass_scale);
  }
  percent = entry->expected_size ? done * 10000 / entry->expected_size : 10000;
  if(progress->block_size && percent > 9999)
    percent = 9999;
  if(percent > 10000)
    percent = 10000;
  if((uint64_t)percent < progress->displayed_percent)
    percent = progress->displayed_percent;
  if(timer_tick && progress->block_size &&
    (uint64_t)percent == progress->displayed_percent && progress->displayed_percent < 9999)
    percent = progress->displayed_percent + 1;
  progress->displayed_percent = (uint64_t)percent;
  width = fprintf(stderr, "\r%s %llu.%02llu%%", entry->name,
    (unsigned long long)(percent / 100), (unsigned long long)((uint64_t)percent % 100));
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

static void progress_start(PROGRESS *progress, const ENTRY *entry, const turtledeflate_config_t *config)
{
  pthread_mutex_lock(&progress->mutex);
  progress->entry = entry;
  progress->done = 0;
  progress->block_size = 0;
  progress->pass_scale = config ? 2.0 * config->i_num_start_fp * config->i_max_block_splitter_iterations : 1.0;
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
  pthread_mutex_lock(&progress->mutex);
  if(success)
    progress->done = progress->entry->expected_size;
  progress->block_size = 0;
  progress->pass = 0;
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

static char *config_path(const char *program)
{
  const char *override = getenv("KATZIP_INI");
  const char *slash;
  const char *name = "katzip.ini";
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

static int load_config(const char *program, int level, turtledeflate_config_t *config, int *fast_level)
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
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  file = fopen(path, "r");
  if(!file)
  {
    fprintf(stderr, "katzip: cannot open %s: %s\n", path, strerror(errno));
    free(path);
    return -1;
  }
  snprintf(section, sizeof(section), level < 7 ? "[libdeflate-%d]" : "[turtledeflate-%d]", level);
  memset(config, 0, sizeof(*config));
  *fast_level = 0;
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
    if(level < 7)
    {
      if(strcmp(key, "level") || seen || number < 1 || number > 12)
        goto bad_line;
      *fast_level = (int)number;
      seen = 1;
      continue;
    }
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
    fprintf(stderr, "katzip: cannot read %s\n", path);
    goto done;
  }
  if(!found || (level < 7 ? (seen != 1 || !*fast_level) :
    (seen != (UINT32_C(1) << ARRAY_N(fields)) - 1 || !valid_config(config, level))))
  {
    fprintf(stderr, "katzip: missing or invalid settings in %s %s\n", path, section);
    goto done;
  }
  status = 0;
  goto done;

bad_line:
  fprintf(stderr, "katzip: invalid setting in %s:%d\n", path, line_number);
done:
  fclose(file);
  free(path);
  return status;
}

static int write_deflate_chunk(void *zip, FILE *temporary, const unsigned char *data, int size)
{
  if(temporary)
    return fwrite(data, 1, (size_t)size, temporary) == (size_t)size ? 0 : -1;
  return mz_zip_entry_write(zip, data, size) == size ? 0 : -1;
}

/* libdeflate compresses a complete buffer; large files use streaming zlib. */
static int write_fast_entry(void *zip, ENTRY *entry, FILE *in, int level, PROGRESS *progress)
{
  mz_zip_file file_info;
  struct libdeflate_compressor *compressor = NULL;
  unsigned char *input = NULL;
  unsigned char *output = NULL;
  z_stream stream;
  uint32_t crc = UINT32_MAX;
  size_t size;
  size_t output_size = 0;
  size_t offset;
  int method = MZ_COMPRESS_METHOD_DEFLATE;
  int started = 0;
  int status = -1;
  int result;

  memset(&file_info, 0, sizeof(file_info));
  file_info.version_madeby = (3 << 8) | 20;
  file_info.version_needed = 20;
  file_info.flag = entry->flags;
  file_info.uncompressed_size = entry->expected_size;
  file_info.zip64 = MZ_ZIP64_DISABLE;
  file_info.modified_date = entry->mtime;
  file_info.filename = entry->name;
  file_info.filename_size = entry->name_len;
  file_info.external_fa = entry->mode << 16;
  progress_start(progress, entry, NULL);
  started = 1;

  if(entry->expected_size <= FAST_FILE_LIMIT)
  {
    size = entry->expected_size;
    input = malloc(size ? size : 1);
    if(!input || fread(input, 1, size, in) != size || fgetc(in) != EOF || ferror(in))
      goto done;
    entry->size = (uint32_t)size;
    crc = update_crc(crc, input, size);
    progress_block(progress, (uint32_t)size);
    compressor = libdeflate_alloc_compressor(level);
    if(!compressor)
      goto done;
    /* A result that does not save space is written as ZIP Store. */
    output = malloc(size + 16);
    if(!output)
      goto done;
    if(size)
      output_size = libdeflate_deflate_compress(compressor, input, size, output, size + 16);
    else
    {
      output[0] = 0x03;
      output[1] = 0x00;
      output_size = 2;
    }
    if(size && (!output_size || output_size >= size))
      method = MZ_COMPRESS_METHOD_STORE;
    file_info.compression_method = method;
    if(mz_zip_entry_write_open(zip, &file_info, 6, 1, NULL) != MZ_OK)
      goto done;
    for(offset = 0; offset < (method == MZ_COMPRESS_METHOD_STORE ? size : output_size);)
    {
      size_t left = (method == MZ_COMPRESS_METHOD_STORE ? size : output_size) - offset;
      size_t chunk = left > 1048576 ? 1048576 : left;
      const unsigned char *data = method == MZ_COMPRESS_METHOD_STORE ? input : output;
      if(mz_zip_entry_write(zip, data + offset, (int32_t)chunk) != (int32_t)chunk)
        goto done;
      offset += chunk;
    }
    entry->compressed_size = method == MZ_COMPRESS_METHOD_STORE ? size : output_size;
    progress_update(progress, entry->size);
  }
  else
  {
    input = malloc(1048576);
    output = malloc(1048576);
    if(!input || !output)
      goto done;
    memset(&stream, 0, sizeof(stream));
    if(deflateInit2(&stream, level > 9 ? 9 : level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
      goto done;
    file_info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
    if(mz_zip_entry_write_open(zip, &file_info, 6, 1, NULL) != MZ_OK)
    {
      deflateEnd(&stream);
      goto done;
    }
    do
    {
      size = fread(input, 1, 1048576, in);
      if(ferror(in) || (uint64_t)entry->size + size > entry->expected_size)
      {
        deflateEnd(&stream);
        goto done;
      }
      crc = update_crc(crc, input, size);
      entry->size += (uint32_t)size;
      progress_block(progress, (uint32_t)size);
      stream.next_in = input;
      stream.avail_in = (uInt)size;
      do
      {
        stream.next_out = output;
        stream.avail_out = 1048576;
        result = deflate(&stream, size ? Z_NO_FLUSH : Z_FINISH);
        if(result != Z_OK && result != Z_STREAM_END)
        {
          deflateEnd(&stream);
          goto done;
        }
        output_size = 1048576 - stream.avail_out;
        if(output_size && mz_zip_entry_write(zip, output, (int32_t)output_size) != (int32_t)output_size)
        {
          deflateEnd(&stream);
          goto done;
        }
        entry->compressed_size += output_size;
      } while(stream.avail_in || stream.avail_out == 0 || (!size && result != Z_STREAM_END));
      progress_update(progress, entry->size);
    } while(size);
    deflateEnd(&stream);
    if(entry->size != entry->expected_size)
      goto done;
  }
  if(mz_zip_entry_write_close(zip, crc ^ UINT32_MAX, entry->compressed_size, entry->size) != MZ_OK)
    goto done;
  status = 0;

done:
  if(compressor)
    libdeflate_free_compressor(compressor);
  free(output);
  free(input);
  if(started)
    progress_finish(progress, status == 0);
  return status;
}

static int write_entry(void *zip, ENTRY *entry, FILE *in, const turtledeflate_config_t *config, PROGRESS *progress)
{
  unsigned char *buffer;
  unsigned char *compressed;
  turtledeflate_config_t compressor_config = *config;
  mz_zip_file file_info;
  ECT_JOB ect;
  FILE *turtle_temp = NULL;
  void *compressor = NULL;
  uint32_t crc = UINT32_MAX;
  int64_t compressed_total = 0;
  int64_t chosen_total;
  size_t size;
  size_t transferred;
  int next;
  int compressed_size;
  int progress_started = 0;
  int use_ect = 0;
  int status = -1;

  memset(&ect, 0, sizeof(ect));
  buffer = malloc((size_t)config->i_maximum_block_size);
  if(!buffer)
    return -1;
  if(config->i_compression_level == 9 && start_ect_job(&ect, in, entry->expected_size))
    goto done;
  if(ect.started)
  {
    turtle_temp = tmpfile();
    if(!turtle_temp)
      goto done;
  }
  memset(&file_info, 0, sizeof(file_info));
  file_info.version_madeby = (3 << 8) | 20;
  file_info.version_needed = 20;
  file_info.flag = entry->flags;
  file_info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
  file_info.uncompressed_size = entry->expected_size;
  file_info.zip64 = MZ_ZIP64_DISABLE;
  file_info.modified_date = entry->mtime;
  file_info.filename = entry->name;
  file_info.filename_size = entry->name_len;
  file_info.external_fa = entry->mode << 16;
  if(!turtle_temp && mz_zip_entry_write_open(zip, &file_info, 6, 1, NULL) != MZ_OK)
    goto done;
  size = fread(buffer, 1, (size_t)config->i_maximum_block_size, in);
  if(ferror(in))
    goto done;
  if(size && !turtledeflate_create(&compressor, &compressor_config))
    goto done;
  progress_start(progress, entry, config);
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
    progress_block(progress, (uint32_t)size);
    compressed_size = turtledeflate_block(compressor, (int32_t)size, buffer, &compressed, NULL, next == EOF);
    if(compressed_size < 0 || write_deflate_chunk(zip, turtle_temp, compressed, compressed_size))
      goto done;
    compressed_total += compressed_size;
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
    if(write_deflate_chunk(zip, turtle_temp, empty_deflate, sizeof(empty_deflate)))
      goto done;
    compressed_total = sizeof(empty_deflate);
  }
  if(entry->size != entry->expected_size)
    goto done;
  chosen_total = compressed_total;
  if(ect.started)
  {
    progress_wait(progress);
    pthread_join(ect.thread, NULL);
    ect.started = 0;
    if(ect.crc != (crc ^ UINT32_MAX))
      goto done;
    use_ect = ect.output && ect.output_size < (uint64_t)compressed_total && ect.output_size <= UINT32_MAX;
    if(use_ect)
      chosen_total = (int64_t)ect.output_size;
    if(mz_zip_entry_write_open(zip, &file_info, 6, 1, NULL) != MZ_OK)
      goto done;
    if(!use_ect && fseek(turtle_temp, 0, SEEK_SET))
      goto done;
    transferred = 0;
    while(transferred < (size_t)chosen_total)
    {
      size_t chunk = (size_t)chosen_total - transferred;
      if(chunk > 65536)
        chunk = 65536;
      if(!use_ect && fread(buffer, 1, chunk, turtle_temp) != chunk)
        goto done;
      if(mz_zip_entry_write(zip, use_ect ? ect.output + transferred : buffer, (int32_t)chunk) != (int32_t)chunk)
        goto done;
      transferred += chunk;
    }
  }
  if(mz_zip_entry_write_close(zip, crc ^ UINT32_MAX, chosen_total, entry->size) != MZ_OK)
    goto done;
  entry->compressed_size = (uint64_t)chosen_total;
  status = 0;

done:
  if(ect.started)
    pthread_join(ect.thread, NULL);
  free(ect.input);
  free(ect.output);
  if(turtle_temp)
    fclose(turtle_temp);
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
    fprintf(stderr, "katzip: invalid input file: %s\n", path);
    return -1;
  }
  if(list->archive_exists && list->archive_stat.st_dev == file_stat.st_dev &&
    list->archive_stat.st_ino == file_stat.st_ino)
  {
    if(recursive)
      return 0;
    fprintf(stderr, "katzip: archive is an input file: %s\n", path);
    return -1;
  }
  for(i = 0; i < list->count; ++i)
  {
    if(strcmp(name, list->entries[i].name) == 0)
    {
      if(recursive)
        return 0;
      fprintf(stderr, "katzip: duplicate entry: %s\n", name);
      return -1;
    }
  }
  if(list->count == UINT16_MAX)
  {
    fprintf(stderr, "katzip: too many files\n");
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
      fprintf(stderr, "katzip: out of memory\n");
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
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  entry->name_len = (uint16_t)strlen(name);
  entry->flags = (uint16_t)(valid_utf8(name) ? MZ_ZIP_FLAG_UTF8 : 0);
  entry->mode = (uint32_t)file_stat.st_mode;
  entry->expected_size = (uint32_t)file_stat.st_size;
  entry->mtime = file_stat.st_mtime;
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

static int matches_masks(const char **masks, size_t count, const char *relative, const char *basename)
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

static int walk_directory(ENTRY_LIST *list, const char *directory, const char *root, const char **masks, size_t mask_count)
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
    else if(S_ISDIR(file_stat.st_mode))
      status = walk_directory(list, path, root, masks, mask_count);
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

static int add_argument(ENTRY_LIST *list, const char *argument, int recursive, const char **masks, size_t mask_count)
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
    status = walk_directory(list, root, root, masks, mask_count);
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

int main(int argc, char **argv)
{
  ENTRY_LIST list;
  PROGRESS progress;
  void *writer = NULL;
  void *zip = NULL;
  void *reader = NULL;
  FILE *in;
  char *archive_path;
  char *temporary_path = NULL;
  const char **masks = NULL;
  size_t mask_count = 0;
  turtledeflate_config_t config;
  int fast_level = 0;
  int archive_arg = 1;
  int level = 7;
  int recursive = 0;
  int source_count = 0;
  int progress_ready = 0;
  int error_number;
  struct stat output_stat;
  mode_t output_mode;
  mode_t process_umask;
  uint64_t expected_archive_size = 22;
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
      fprintf(stderr, "katzip: unknown option: %s\n", argv[archive_arg]);
      return 1;
    }
    ++archive_arg;
  }
  if(argc <= archive_arg)
  {
    fprintf(stderr, "usage: katzip [-1..-9] [-r] archive_name file[s] [@MASK ...]\n");
    return 1;
  }
  if(load_config(argv[0], level, &config, &fast_level))
    return 1;
  archive_path = archive_name(argv[archive_arg]);
  if(!archive_path)
  {
    fprintf(stderr, "katzip: invalid archive name\n");
    return 1;
  }
  memset(&list, 0, sizeof(list));
  list.archive_exists = stat(archive_path, &list.archive_stat) == 0;
  process_umask = umask(0);
  umask(process_umask);
  output_mode = list.archive_exists ? list.archive_stat.st_mode & 0777 : 0666 & ~process_umask;
  masks = calloc((size_t)argc, sizeof(*masks));
  if(!masks)
  {
    fprintf(stderr, "katzip: out of memory\n");
    goto done;
  }
  for(i = archive_arg + 1; i < argc; ++i)
  {
    if(recursive && argv[i][0] == '@')
    {
      if(!argv[i][1])
      {
        fprintf(stderr, "katzip: empty file mask\n");
        goto done;
      }
      masks[mask_count++] = argv[i] + 1;
    }
    else
      ++source_count;
  }
  if(!source_count && !mask_count)
  {
    fprintf(stderr, "katzip: no input files or masks\n");
    goto done;
  }
  if(!source_count)
  {
    if(add_argument(&list, ".", recursive, masks, mask_count))
      goto done;
  }
  for(i = archive_arg + 1; i < argc; ++i)
  {
    if(recursive && argv[i][0] == '@')
      continue;
    if(add_argument(&list, argv[i], recursive, masks, mask_count))
      goto done;
  }
  if(!list.count)
  {
    fprintf(stderr, "katzip: no files to archive\n");
    goto done;
  }
  if(progress_init(&progress))
  {
    fprintf(stderr, "katzip: cannot start progress display\n");
    goto done;
  }
  progress_ready = 1;
  temporary_path = malloc(strlen(archive_path) + sizeof(".tmp.XXXXXX"));
  if(!temporary_path)
  {
    fprintf(stderr, "katzip: out of memory\n");
    goto done;
  }
  sprintf(temporary_path, "%s.tmp.XXXXXX", archive_path);
  i = mkstemp(temporary_path);
  if(i < 0)
  {
    fprintf(stderr, "katzip: cannot create temporary archive: %s\n", strerror(errno));
    goto done;
  }
  if(close(i))
  {
    error_number = errno;
    goto remove_temp;
  }
  writer = mz_zip_writer_create();
  if(!writer || mz_zip_writer_open_file(writer, temporary_path, 0, 0) != MZ_OK ||
    mz_zip_writer_get_zip_handle(writer, &zip) != MZ_OK)
    goto output_error;
  for(i = 0; i < (int)list.count; ++i)
  {
    in = fopen(list.entries[i].path, "rb");
    if(!in || (level < 7 ? write_fast_entry(zip, &list.entries[i], in, fast_level, &progress) :
      write_entry(zip, &list.entries[i], in, &config, &progress)))
    {
      fprintf(stderr, "katzip: cannot archive %s\n", list.entries[i].path);
      if(in)
        fclose(in);
      goto output_error;
    }
    if(fclose(in))
      goto output_error;
  }
  if(mz_zip_writer_close(writer) != MZ_OK)
    goto output_error;
  mz_zip_writer_delete(&writer);
  for(i = 0; i < (int)list.count; ++i)
    expected_archive_size += list.entries[i].compressed_size + 76 + 2 * list.entries[i].name_len;
  if(expected_archive_size > UINT32_MAX || stat(temporary_path, &output_stat) || output_stat.st_size < 0 ||
    (uint64_t)output_stat.st_size != expected_archive_size)
  {
    errno = EIO;
    goto output_error;
  }
  reader = mz_zip_reader_create();
  if(!reader || mz_zip_reader_open_file(reader, temporary_path) != MZ_OK)
  {
    errno = EIO;
    goto output_error;
  }
  mz_zip_reader_delete(&reader);
  i = open(temporary_path, O_RDONLY);
  if(i < 0)
  {
    error_number = errno;
    goto remove_temp;
  }
  if(fchmod(i, output_mode))
  {
    error_number = errno;
    close(i);
    goto remove_temp;
  }
  if(fsync(i))
  {
    error_number = errno;
    close(i);
    goto remove_temp;
  }
  if(close(i))
  {
    error_number = errno;
    goto remove_temp;
  }
  if(rename(temporary_path, archive_path))
  {
    error_number = errno;
    goto remove_temp;
  }
  status = 0;
  goto done;

output_error:
  error_number = errno ? errno : EIO;
remove_temp:
  if(reader)
    mz_zip_reader_delete(&reader);
  if(writer)
    mz_zip_writer_delete(&writer);
  fprintf(stderr, "katzip: failed to write archive %s: %s\n", archive_path, strerror(error_number));
  remove(temporary_path);
done:
  if(progress_ready)
    progress_destroy(&progress);
  free_entries(&list);
  free(masks);
  free(temporary_path);
  free(archive_path);
  return status;
}
