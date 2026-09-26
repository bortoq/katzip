#include "katzip_internal.h"
#include <wchar.h>

/* Count terminal columns, with a UTF-8 fallback for a single-byte locale. */
static int utf8_columns(const char *name)
{
  const unsigned char *byte = (const unsigned char*)name;
  int columns = 0;
  while(*byte)
  {
    if((*byte & 0xc0) != 0x80)
      ++columns;
    ++byte;
  }
  return columns;
}

static int name_columns(const char *name)
{
  mbstate_t state = {0};
  const char *next = name;
  int columns = 0;
  if(MB_CUR_MAX == 1)
    return utf8_columns(name);
  while(*next)
  {
    wchar_t character;
    size_t length = mbrtowc(&character, next, MB_CUR_MAX, &state);
    int width;
    if(length == (size_t)-1 || length == (size_t)-2)
    {
      memset(&state, 0, sizeof(state));
      ++columns;
      ++next;
      continue;
    }
    if(length == 0)
      break;
    width = wcwidth(character);
    columns += width < 0 ? 1 : width;
    next += length;
  }
  return columns;
}

static void print_spaces(int count)
{
  while(count-- > 0)
    fputc(' ', stderr);
}

static int print_name(const PROGRESS *progress)
{
  int columns = name_columns(progress->entry->name);
  fputc('\r', stderr);
  fputs(progress->entry->name, stderr);
  print_spaces(progress->name_width - columns + 2);
  return progress->name_width + 2;
}

static void print_progress_value(PROGRESS *progress, uint64_t percent)
{
  int columns = print_name(progress);
  columns += fprintf(stderr, "%llu.%02llu%%",
    (unsigned long long)(percent / 100),
    (unsigned long long)(percent % 100));
  print_spaces(progress->display_width - columns);
  progress->display_width = columns;
  fflush(stderr);
}

static void print_completed_value(PROGRESS *progress, uint64_t ratio)
{
  int columns = print_name(progress);
  columns += fprintf(stderr, "%llu.%02llu%%",
    (unsigned long long)(ratio / 100),
    (unsigned long long)(ratio % 100));
  print_spaces(progress->display_width - columns);
  fputc('\n', stderr);
  progress->display_width = 0;
  fflush(stderr);
}

static double estimated_input_done(const PROGRESS *progress)
{
  double done = progress->done;
  double work;
  if(!progress->pass || !progress->pass_total ||
    done >= progress->entry->expected_size)
    return done;
  work = progress->pass - 1 +
    (double)progress->pass_done / progress->pass_total;
  return done + progress->block_size * work / (work + progress->pass_scale);
}

static uint64_t progress_percent(PROGRESS *progress, int timer_tick)
{
  double done = (double)progress->completed_work;
  double percent = 0;
  if(!progress->parallel)
    done += 6.0 * estimated_input_done(progress);
  if(progress->total_work)
    percent = done * 10000 / progress->total_work;
  if(progress->entries_written < progress->entries_total &&
    percent > 9999)
    percent = 9999;
  if((uint64_t)percent < progress->displayed_percent)
    percent = progress->displayed_percent;
  if(timer_tick && progress->block_size &&
    (uint64_t)percent == progress->displayed_percent &&
    progress->displayed_percent < 9999)
    percent = progress->displayed_percent + 1;
  return (uint64_t)percent;
}

static void show_progress(PROGRESS *progress, int timer_tick)
{
  progress->displayed_percent = progress_percent(progress, timer_tick);
  print_progress_value(progress, progress->displayed_percent);
}

static void wait_for_active_progress(PROGRESS *progress)
{
  while(!progress->active && !progress->stop)
    pthread_cond_wait(&progress->condition, &progress->mutex);
}

static void show_timed_progress(PROGRESS *progress)
{
  struct timespec deadline;
  int result;
  clock_gettime(CLOCK_REALTIME, &deadline);
  ++deadline.tv_sec;
  result = pthread_cond_timedwait(&progress->condition,
    &progress->mutex, &deadline);
  if(result == ETIMEDOUT && progress->active)
    show_progress(progress, 1);
}

static void *progress_thread(void *argument)
{
  PROGRESS *progress = (PROGRESS*)argument;
  pthread_mutex_lock(&progress->mutex);
  while(!progress->stop)
  {
    wait_for_active_progress(progress);
    if(progress->stop)
      break;
    show_timed_progress(progress);
  }
  pthread_mutex_unlock(&progress->mutex);
  return NULL;
}

int progress_init(PROGRESS *progress)
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

static uint64_t entry_weight(const ENTRY *entry)
{
  return entry->expected_size ? entry->expected_size : 1;
}

void progress_set_total(PROGRESS *progress, const ENTRY_LIST *list,
  int parallel)
{
  size_t i;
  pthread_mutex_lock(&progress->mutex);
  progress->parallel = parallel;
  progress->entries_total = list->count;
  progress->total_work = 0;
  progress->name_width = 0;
  for(i = 0; i < list->count; ++i)
  {
    int width = name_columns(list->entries[i].name);
    progress->total_work += 6 * entry_weight(&list->entries[i]);
    if(width > progress->name_width)
      progress->name_width = width;
  }
  pthread_mutex_unlock(&progress->mutex);
}

/* Each competing encoder contributes one share of its file's weight. */
void progress_task_done(PROGRESS *progress, const ENTRY *entry,
  int task_count)
{
  pthread_mutex_lock(&progress->mutex);
  progress->completed_work += 6 * entry_weight(entry) / task_count;
  if(progress->active)
    show_progress(progress, 0);
  pthread_mutex_unlock(&progress->mutex);
}

void progress_start(PROGRESS *progress, const ENTRY *entry,
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
  progress->display_width = 0;
  progress->active = 1;
  show_progress(progress, 0);
  pthread_cond_signal(&progress->condition);
  pthread_mutex_unlock(&progress->mutex);
}

void progress_block(PROGRESS *progress, uint32_t size)
{
  if(!progress)
    return;
  pthread_mutex_lock(&progress->mutex);
  progress->block_size = size;
  progress->pass = 0;
  pthread_mutex_unlock(&progress->mutex);
}

void progress_update(PROGRESS *progress, uint32_t done)
{
  if(!progress)
    return;
  pthread_mutex_lock(&progress->mutex);
  progress->done = done;
  progress->block_size = 0;
  progress->pass = 0;
  pthread_mutex_unlock(&progress->mutex);
}

void progress_wait(PROGRESS *progress)
{
  if(!progress)
    return;
  pthread_mutex_lock(&progress->mutex);
  progress->block_size = 1;
  pthread_mutex_unlock(&progress->mutex);
}

void progress_callback(void *user, uint32_t pass, uint32_t completed, uint32_t total)
{
  if(!user)
    return;
  PROGRESS *progress = (PROGRESS*)user;
  pthread_mutex_lock(&progress->mutex);
  progress->pass = pass;
  progress->pass_done = completed;
  progress->pass_total = total;
  pthread_mutex_unlock(&progress->mutex);
}

static uint64_t compression_ratio(const ENTRY *entry)
{
  if(!entry->size)
    return 0;
  return (entry->compressed_size * 10000 + entry->size / 2) /
    entry->size;
}

void progress_finish(PROGRESS *progress, int success)
{
  uint64_t ratio;
  pthread_mutex_lock(&progress->mutex);
  progress->block_size = 0;
  progress->pass = 0;
  if(success)
  {
    if(!progress->parallel)
      progress->completed_work += 6 * entry_weight(progress->entry);
    ++progress->entries_written;
    progress->done = 0;
    ratio = compression_ratio(progress->entry);
    print_completed_value(progress, ratio);
  }
  else
  {
    show_progress(progress, 0);
    fputc('\n', stderr);
  }
  progress->active = 0;
  pthread_cond_signal(&progress->condition);
  pthread_mutex_unlock(&progress->mutex);
}

void progress_destroy(PROGRESS *progress)
{
  pthread_mutex_lock(&progress->mutex);
  progress->stop = 1;
  pthread_cond_signal(&progress->condition);
  pthread_mutex_unlock(&progress->mutex);
  pthread_join(progress->thread, NULL);
  pthread_cond_destroy(&progress->condition);
  pthread_mutex_destroy(&progress->mutex);
}
