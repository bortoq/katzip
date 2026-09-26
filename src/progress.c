#include "katzip_internal.h"

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
  double done = estimated_input_done(progress);
  double percent = 0;
  if(progress->entry->expected_size)
    percent = done * 10000 / progress->entry->expected_size;
  if(percent > 9999)
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
  progress->displayed_percent = 0;
  progress->display_width = 0;
  progress->active = 1;
  show_progress(progress, 0);
  pthread_cond_signal(&progress->condition);
  pthread_mutex_unlock(&progress->mutex);
}

void progress_block(PROGRESS *progress, uint32_t size)
{
  pthread_mutex_lock(&progress->mutex);
  progress->block_size = size;
  progress->pass = 0;
  pthread_mutex_unlock(&progress->mutex);
}

void progress_update(PROGRESS *progress, uint32_t done)
{
  pthread_mutex_lock(&progress->mutex);
  progress->done = done;
  progress->block_size = 0;
  progress->pass = 0;
  pthread_mutex_unlock(&progress->mutex);
}

void progress_wait(PROGRESS *progress)
{
  pthread_mutex_lock(&progress->mutex);
  progress->block_size = 1;
  pthread_mutex_unlock(&progress->mutex);
}

void progress_callback(void *user, uint32_t pass, uint32_t completed, uint32_t total)
{
  PROGRESS *progress = (PROGRESS*)user;
  pthread_mutex_lock(&progress->mutex);
  progress->pass = pass;
  progress->pass_done = completed;
  progress->pass_total = total;
  pthread_mutex_unlock(&progress->mutex);
}

void progress_finish(PROGRESS *progress, int success)
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
