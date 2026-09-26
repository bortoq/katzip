#ifndef KATZIP_INTERNAL_H
#define KATZIP_INTERNAL_H

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
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
  uint64_t completed_work;
  uint64_t total_work;
  size_t entries_total;
  size_t entries_written;
  int display_width;
  int name_width;
  int parallel;
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
/* The largest upstream preset also bounds Turtledeflate's int32 allocation arithmetic. */
#define MAX_BLOCK_SIZE 1000000
#define DEFAULT_LEVEL 7

typedef struct {
  const char *name;
  size_t offset;
  int minimum;
  int maximum;
} ECT_FIELD;

extern const CONFIG_FIELD config_fields[13];
extern const ECT_FIELD ect_fields[18];

typedef struct {
  unsigned char *data;
  FILE *file;
  uint64_t size;
  uint32_t crc;
} CANDIDATE;

typedef struct {
  unsigned char *buffer;
  void *compressor;
  FILE *temporary;
  uint32_t crc;
  uint64_t compressed_size;
} TURTLE_WORK;

typedef struct {
  int level;
  int recursive;
  int archive_arg;
  int argument_count;
} OPTIONS;

char *trim(char *text);
void use_default_config(int level, COMPRESSION_CONFIG *config);
int write_default_ini(FILE *file);
int open_config(const char *program, FILE **file, char **path);
int load_config(const char *program, int level,
  COMPRESSION_CONFIG *config, OPTIONS *options);
int config_option_known(const char *option);
int apply_config_option(const char *option, const char *value,
  COMPRESSION_CONFIG *config);
int validate_compression_config(const COMPRESSION_CONFIG *config);

int valid_name(const char *name);
int valid_utf8(const char *name);
char *archive_name(const char *argument);
void free_entries(ENTRY_LIST *list);
int collect_entries(ENTRY_LIST *list, int argc, char **argv,
  const OPTIONS *options);
int scan_options(int argc, char **argv, OPTIONS *options);
int parse_options(int argc, char **argv, OPTIONS *options,
  COMPRESSION_CONFIG *config);

int progress_init(PROGRESS *progress);
void progress_set_total(PROGRESS *progress, const ENTRY_LIST *list,
  int parallel);
void progress_task_done(PROGRESS *progress, const ENTRY *entry,
  int task_count);
void progress_start(PROGRESS *progress, const ENTRY *entry,
  const turtledeflate_config_t *config);
void progress_block(PROGRESS *progress, uint32_t size);
void progress_update(PROGRESS *progress, uint32_t done);
void progress_wait(PROGRESS *progress);
void progress_callback(void *user, uint32_t pass, uint32_t completed,
  uint32_t total);
void progress_finish(PROGRESS *progress, int success);
void progress_destroy(PROGRESS *progress);

uint32_t update_crc(uint32_t crc, const unsigned char *data, size_t size);
int zip_level_hint(int level);
int open_zip_entry(void *zip, const ENTRY *entry, int method, int level);
int close_zip_entry(void *zip, const ENTRY *entry, uint32_t crc);
int write_zip_bytes(void *zip, const unsigned char *data, size_t size);
int write_stored_input(void *zip, FILE *in, uint32_t size, uint32_t crc);

void finish_candidate(CANDIDATE *candidate);
int make_fast_candidate(CANDIDATE *candidate, FILE *in,
  uint32_t size, int level);
int make_ect_candidate(CANDIDATE *candidate, FILE *in,
  uint32_t size, const ZopfliOptions *options);
int make_turtle_candidate(CANDIDATE *candidate, FILE *in,
  const ENTRY *entry, const turtledeflate_config_t *config,
  PROGRESS *progress);
int make_zlib_candidate(CANDIDATE *candidate, FILE *in,
  const ENTRY *entry, int level);
int write_prepared_entry(void *zip, ENTRY *entry, FILE *in,
  CANDIDATE *candidate, int store, int zip_level);
void *ect_worker(void *argument);
int start_ect_job(ECT_JOB *job, FILE *in, uint32_t size,
  const ZopfliOptions *options);
void complete_ect_job(ECT_JOB *job, PROGRESS *progress);
void finish_turtle_work(TURTLE_WORK *work);
int prepare_turtle_work(TURTLE_WORK *work,
  const turtledeflate_config_t *config);
int compress_turtle_blocks(void *zip, ENTRY *entry, FILE *in,
  const turtledeflate_config_t *config, TURTLE_WORK *work,
  PROGRESS *progress);
int write_fast_entry(void *zip, ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress);
int write_zlib_entry(void *zip, ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress);
int write_ect_entry(void *zip, ENTRY *entry, FILE *in,
  const ZopfliOptions *options, int zip_level, PROGRESS *progress);
int write_entry(void *zip, ENTRY *entry, FILE *in,
  const turtledeflate_config_t *config, int zip_level,
  PROGRESS *progress);
int write_race_entry(void *zip, ENTRY *entry, FILE *in,
  const COMPRESSION_CONFIG *config, int zip_level, PROGRESS *progress);
void set_signal_snapshot_path(const char *path);
int write_parallel_entries(void *zip, ENTRY_LIST *list,
  const COMPRESSION_CONFIG *config, int zip_level, PROGRESS *progress);
int run_archive(char **argv, const OPTIONS *options,
  const COMPRESSION_CONFIG *config);

#endif
