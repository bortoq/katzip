#include "katzip_internal.h"

static const char *volatile signal_temp_path;
static const char *volatile signal_snapshot_path;

void set_signal_snapshot_path(const char *path)
{
  signal_snapshot_path = path;
}

static void remove_temp_on_signal(int signal_number)
{
  if(signal_snapshot_path)
    unlink(signal_snapshot_path);
  if(signal_temp_path)
    unlink(signal_temp_path);
  _exit(128 + signal_number);
}

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

static int compress_archive_entry(ARCHIVE_OUTPUT *output, ENTRY *entry,
  FILE *in, const OPTIONS *options, const COMPRESSION_CONFIG *config)
{
  int hint = zip_level_hint(options->level);
  if((uint64_t)entry->expected_size >= config->zlib_after)
    return write_zlib_entry(output->zip, entry, in, config->zlib_level,
      hint, &output->progress);
  if(config->have_fast + config->have_ect + config->have_turtle > 1)
    return write_race_entry(output->zip, entry, in, config, hint,
      &output->progress);
  if(config->have_fast)
    return write_fast_entry(output->zip, entry, in, config->fast_level,
      hint, &output->progress);
  if(config->have_ect)
    return write_ect_entry(output->zip, entry, in, &config->ect, hint,
      &output->progress);
  return write_entry(output->zip, entry, in, &config->turtle, hint,
    &output->progress);
}

static int write_archive_entry(ARCHIVE_OUTPUT *output, ENTRY *entry,
  const OPTIONS *options, const COMPRESSION_CONFIG *config)
{
  FILE *in = fopen(entry->path, "rb");
  int result;
  if(!in)
  {
    fprintf(stderr, "katzip: cannot archive %s\n", entry->path);
    return -1;
  }
  result = compress_archive_entry(output, entry, in, options, config);
  if(fclose(in))
    result = -1;
  if(result)
    fprintf(stderr, "katzip: cannot archive %s\n", entry->path);
  return result;
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
  if(list->count > 1)
  {
    if(write_parallel_entries(output->zip, list, config,
      zip_level_hint(options->level), &output->progress))
      return -1;
  }
  else
  {
    for(i = 0; i < list->count; ++i)
      if(write_archive_entry(output, &list->entries[i], options, config))
        return -1;
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

static int sync_archive_file(const char *path, mode_t mode)
{
  int file = open(path, O_RDONLY);
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
    errno = saved_error;
  return result;
}

/* Publish only a complete archive, preserving an existing output on error. */
static int publish_archive(ARCHIVE_OUTPUT *output,
  const char *archive_path, mode_t mode)
{
  if(sync_archive_file(output->temporary_path, mode))
    return -1;
  if(rename(output->temporary_path, archive_path))
    return -1;
  output->temporary_created = 0;
  return 0;
}

static int initialize_archive_output(ARCHIVE_OUTPUT *output,
  const char *archive_path)
{
  if(create_temporary_archive(output, archive_path))
    return -1;
  if(progress_init(&output->progress))
  {
    fprintf(stderr, "katzip: cannot start progress display\n");
    return -1;
  }
  output->progress_ready = 1;
  return 0;
}

static int finish_archive(ARCHIVE_OUTPUT *output, ENTRY_LIST *list,
  const OPTIONS *options, const COMPRESSION_CONFIG *config,
  const char *archive_path, mode_t mode)
{
  if(write_archive_entries(output, list, options, config))
    return -1;
  if(validate_archive(output, list))
    return -1;
  return publish_archive(output, archive_path, mode);
}

static int process_archive(ARCHIVE_OUTPUT *output, ENTRY_LIST *list,
  char **argv, const OPTIONS *options, const COMPRESSION_CONFIG *config,
  const char *archive_path, mode_t mode)
{
  if(collect_entries(list, options->argument_count, argv, options))
    return -1;
  if(initialize_archive_output(output, archive_path))
    return -1;
  progress_set_total(&output->progress, list, list->count > 1);
  if(finish_archive(output, list, options, config,
    archive_path, mode))
    return 1;
  return 0;
}

static void report_archive_error(const ARCHIVE_OUTPUT *output,
  const char *archive_path)
{
  int error_number;
  if(!output->temporary_created)
    return;
  error_number = errno ? errno : EIO;
  fprintf(stderr, "katzip: failed to write archive %s: %s\n",
    archive_path, strerror(error_number));
}

int run_archive(char **argv, const OPTIONS *options,
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
  result = process_archive(&output, &list, argv, options, config,
    archive_path, mode);
  if(result > 0)
    report_archive_error(&output, archive_path);
  finish_archive_output(&output);
  free_entries(&list);
  free(archive_path);
  return result ? 1 : 0;
}
