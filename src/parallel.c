#include "parallel_internal.h"

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t *threads;
  COMPRESS_TASK **queue;
  size_t capacity;
  size_t head;
  size_t tail;
  size_t count;
  size_t thread_count;
  uint64_t memory_used;
  uint64_t memory_limit;
  int stop;
  int failed;
  const COMPRESSION_CONFIG *config;
  PROGRESS *progress;
} SCHEDULER;

static int next_task_fits(const SCHEDULER *scheduler)
{
  const COMPRESS_TASK *task;
  if(!scheduler->count)
    return 0;
  task = scheduler->queue[scheduler->head];
  if(!scheduler->memory_used)
    return 1;
  if(scheduler->memory_used >= scheduler->memory_limit)
    return 0;
  return task->memory_cost <=
    scheduler->memory_limit - scheduler->memory_used;
}

static COMPRESS_TASK *take_task(SCHEDULER *scheduler)
{
  COMPRESS_TASK *task;
  pthread_mutex_lock(&scheduler->mutex);
  while(!scheduler->stop && !scheduler->failed &&
    !next_task_fits(scheduler))
    pthread_cond_wait(&scheduler->changed, &scheduler->mutex);
  if(scheduler->stop || scheduler->failed)
  {
    pthread_mutex_unlock(&scheduler->mutex);
    return NULL;
  }
  task = scheduler->queue[scheduler->head];
  scheduler->head = (scheduler->head + 1) % scheduler->capacity;
  --scheduler->count;
  scheduler->memory_used += task->memory_cost;
  pthread_mutex_unlock(&scheduler->mutex);
  return task;
}

static void *compress_worker(void *argument)
{
  SCHEDULER *scheduler = argument;
  COMPRESS_TASK *task;
  while((task = take_task(scheduler)))
  {
    int result = compress_task(task, scheduler->config);
    if(!result)
      result = spool_candidate(&task->candidate);
    if(fclose(task->input))
      result = -1;
    task->input = NULL;
    if(!result)
      progress_task_done(scheduler->progress, task->slot->entry,
        task->slot->task_count);
    pthread_mutex_lock(&scheduler->mutex);
    scheduler->memory_used -= task->memory_cost;
    ++task->slot->completed;
    if(result)
      scheduler->failed = 1;
    pthread_cond_broadcast(&scheduler->changed);
    pthread_mutex_unlock(&scheduler->mutex);
  }
  return NULL;
}

static void stop_scheduler(SCHEDULER *scheduler);

static size_t task_count(const ENTRY_LIST *list,
  const COMPRESSION_CONFIG *config)
{
  size_t count = 0;
  size_t i;
  for(i = 0; i < list->count; ++i)
  {
    if((uint64_t)list->entries[i].expected_size >= config->zlib_after)
      ++count;
    else
      count += config->have_fast + config->have_ect +
        config->have_turtle;
  }
  return count;
}

static int create_worker_threads(SCHEDULER *scheduler)
{
  size_t i;
  for(i = 0; i < scheduler->thread_count; ++i)
  {
    if(pthread_create(&scheduler->threads[i], NULL,
      compress_worker, scheduler))
    {
      scheduler->thread_count = i;
      stop_scheduler(scheduler);
      return -1;
    }
  }
  return 0;
}

/* Reserve only part of currently available RAM for encoder buffers. */
static uint64_t compression_memory_budget(void)
{
  uint64_t fallback = 512ULL * 1024 * 1024;
#ifdef _SC_AVPHYS_PAGES
  long pages = sysconf(_SC_AVPHYS_PAGES);
  long page_size = sysconf(_SC_PAGESIZE);
  if(pages > 0 && page_size > 0 &&
    (uint64_t)pages <= UINT64_MAX / (uint64_t)page_size)
  {
    uint64_t budget = (uint64_t)pages * (uint64_t)page_size / 2;
    return budget > 64ULL * 1024 * 1024 ?
      budget : 64ULL * 1024 * 1024;
  }
#endif
  return fallback;
}

static int start_scheduler(SCHEDULER *scheduler, const ENTRY_LIST *list,
  const COMPRESSION_CONFIG *config, PROGRESS *progress)
{
  long cores = sysconf(_SC_NPROCESSORS_ONLN);
  size_t total_tasks = task_count(list, config);
  memset(scheduler, 0, sizeof(*scheduler));
  scheduler->config = config;
  scheduler->progress = progress;
  scheduler->thread_count = cores > 0 ? (size_t)cores : 1;
  if(scheduler->thread_count > total_tasks)
    scheduler->thread_count = total_tasks;
  if(!scheduler->thread_count)
    return -1;
  scheduler->capacity = scheduler->thread_count * 4;
  scheduler->memory_limit = compression_memory_budget();
  scheduler->threads = calloc(scheduler->thread_count,
    sizeof(*scheduler->threads));
  scheduler->queue = calloc(scheduler->capacity,
    sizeof(*scheduler->queue));
  if(!scheduler->threads || !scheduler->queue)
  {
    free(scheduler->queue);
    free(scheduler->threads);
    return -1;
  }
  if(pthread_mutex_init(&scheduler->mutex, NULL))
  {
    free(scheduler->queue);
    free(scheduler->threads);
    return -1;
  }
  if(pthread_cond_init(&scheduler->changed, NULL))
  {
    pthread_mutex_destroy(&scheduler->mutex);
    free(scheduler->queue);
    free(scheduler->threads);
    return -1;
  }
  return create_worker_threads(scheduler);
}

static void stop_scheduler(SCHEDULER *scheduler)
{
  size_t i;
  pthread_mutex_lock(&scheduler->mutex);
  scheduler->stop = 1;
  pthread_cond_broadcast(&scheduler->changed);
  pthread_mutex_unlock(&scheduler->mutex);
  for(i = 0; i < scheduler->thread_count; ++i)
    pthread_join(scheduler->threads[i], NULL);
  pthread_cond_destroy(&scheduler->changed);
  pthread_mutex_destroy(&scheduler->mutex);
  free(scheduler->queue);
  free(scheduler->threads);
}

static void submit_slot(SCHEDULER *scheduler, FILE_SLOT *slot)
{
  int i;
  pthread_mutex_lock(&scheduler->mutex);
  for(i = 0; i < slot->task_count; ++i)
  {
    scheduler->queue[scheduler->tail] = &slot->tasks[i];
    scheduler->tail = (scheduler->tail + 1) % scheduler->capacity;
    ++scheduler->count;
  }
  pthread_cond_broadcast(&scheduler->changed);
  pthread_mutex_unlock(&scheduler->mutex);
}

static int wait_for_slot(SCHEDULER *scheduler, FILE_SLOT *slot)
{
  int failed;
  pthread_mutex_lock(&scheduler->mutex);
  while(slot->completed < slot->task_count && !scheduler->failed)
    pthread_cond_wait(&scheduler->changed, &scheduler->mutex);
  failed = scheduler->failed;
  pthread_mutex_unlock(&scheduler->mutex);
  return failed ? -1 : 0;
}

static int fill_window(SCHEDULER *scheduler, FILE_SLOT *slots,
  ENTRY_LIST *list, const COMPRESSION_CONFIG *config,
  size_t window, size_t written, size_t *prepared)
{
  while(*prepared < list->count && *prepared - written < window)
  {
    FILE_SLOT *slot = &slots[*prepared % window];
    if(prepare_slot(slot, &list->entries[*prepared], config))
      return -1;
    submit_slot(scheduler, slot);
    ++*prepared;
  }
  return 0;
}

static int write_current_slot(void *zip, SCHEDULER *scheduler,
  FILE_SLOT *slot, int zip_level, PROGRESS *progress)
{
  int result;
  progress_wait(progress);
  result = wait_for_slot(scheduler, slot);
  if(!result)
  {
    progress_update(progress, slot->entry->expected_size);
    result = write_slot(zip, slot, zip_level);
  }
  progress_finish(progress, result == 0);
  if(!result)
    finish_slot(slot);
  return result;
}

static void finish_all_slots(SCHEDULER *scheduler,
  FILE_SLOT *slots, size_t window)
{
  size_t i;
  stop_scheduler(scheduler);
  for(i = 0; i < window; ++i)
    finish_slot(&slots[i]);
  free(slots);
}

int write_parallel_entries(void *zip, ENTRY_LIST *list,
  const COMPRESSION_CONFIG *config, int zip_level, PROGRESS *progress)
{
  SCHEDULER scheduler;
  FILE_SLOT *slots;
  size_t window;
  size_t prepared = 0;
  size_t written = 0;
  int result = 0;
  if(start_scheduler(&scheduler, list, config, progress))
    return -1;
  window = scheduler.thread_count;
  slots = calloc(window, sizeof(*slots));
  if(!slots)
  {
    stop_scheduler(&scheduler);
    return -1;
  }
  while(written < list->count)
  {
    progress_start(progress, &list->entries[written], NULL);
    result = fill_window(&scheduler, slots, list, config,
      window, written, &prepared);
    if(result)
    {
      progress_finish(progress, 0);
      break;
    }
    result = write_current_slot(zip, &scheduler,
      &slots[written % window], zip_level, progress);
    if(result)
      break;
    ++written;
  }
  finish_all_slots(&scheduler, slots, window);
  return result;
}
