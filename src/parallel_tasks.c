#include "parallel_internal.h"

enum { FAST_ENGINE, ECT_ENGINE, TURTLE_ENGINE, ZLIB_ENGINE };

static int copy_snapshot(FILE *input, FILE *snapshot, uint32_t size)
{
  unsigned char buffer[65536];
  uint64_t copied = 0;
  while(copied < size)
  {
    size_t chunk = (size - copied < sizeof(buffer)) ?
      (size_t)(size - copied) : sizeof(buffer);
    if(fread(buffer, 1, chunk, input) != chunk ||
      fwrite(buffer, 1, chunk, snapshot) != chunk)
      return -1;
    copied += chunk;
  }
  if(fgetc(input) != EOF || ferror(input) || fflush(snapshot))
    return -1;
  return fseek(snapshot, 0, SEEK_SET);
}

static char *snapshot_name(void)
{
  const char *directory = getenv("TMPDIR");
  char *name;
  size_t length;
  if(!directory || !*directory)
    directory = "/tmp";
  length = strlen(directory) + sizeof("/katzip-input-XXXXXX");
  name = malloc(length);
  if(name)
    snprintf(name, length, "%s/katzip-input-XXXXXX", directory);
  return name;
}

static int create_snapshot_file(FILE_SLOT *slot, char *name,
  int *created)
{
  int fd = mkstemp(name);
  if(fd < 0)
    return -1;
  *created = 1;
  slot->snapshot = fdopen(fd, "w+b");
  if(slot->snapshot)
    return 0;
  close(fd);
  return -1;
}

static int fill_snapshot(FILE_SLOT *slot)
{
  FILE *input = fopen(slot->entry->path, "rb");
  int result;
  if(!input)
    return -1;
  result = copy_snapshot(input, slot->snapshot,
    slot->entry->expected_size);
  if(fclose(input))
    result = -1;
  return result;
}

static int open_snapshot_readers(FILE_SLOT *slot, const char *name)
{
  int i;
  for(i = 0; i < slot->task_count; ++i)
  {
    slot->tasks[i].input = fopen(name, "rb");
    if(!slot->tasks[i].input)
      return -1;
  }
  return 0;
}

/* Each candidate gets a separate file position over one immutable copy. */
static int prepare_snapshot(FILE_SLOT *slot)
{
  char *name = snapshot_name();
  int created = 0;
  int result;
  if(!name)
    return -1;
  set_signal_snapshot_path(name);
  result = create_snapshot_file(slot, name, &created);
  if(!result)
    result = fill_snapshot(slot);
  if(!result)
    result = open_snapshot_readers(slot, name);
  if(created)
    unlink(name);
  set_signal_snapshot_path(NULL);
  free(name);
  return result;
}

static void add_task(FILE_SLOT *slot, int engine)
{
  COMPRESS_TASK *task = &slot->tasks[slot->task_count++];
  task->slot = slot;
  task->engine = engine;
  if(engine == FAST_ENGINE)
    task->memory_cost = 3 * (uint64_t)slot->entry->expected_size + 1048576;
  else if(engine == ECT_ENGINE)
    task->memory_cost = 8 * (uint64_t)slot->entry->expected_size + 1048576;
  else if(engine == TURTLE_ENGINE)
    task->memory_cost = MAX_BLOCK_SIZE * 4;
  else
    task->memory_cost = 4 * 1048576;
}

static void choose_tasks(FILE_SLOT *slot, const COMPRESSION_CONFIG *config)
{
  if((uint64_t)slot->entry->expected_size >= config->zlib_after)
  {
    slot->zlib_only = 1;
    add_task(slot, ZLIB_ENGINE);
    return;
  }
  if(config->have_fast)
    add_task(slot, FAST_ENGINE);
  if(config->have_ect)
    add_task(slot, ECT_ENGINE);
  if(config->have_turtle)
    add_task(slot, TURTLE_ENGINE);
}

int prepare_slot(FILE_SLOT *slot, ENTRY *entry,
  const COMPRESSION_CONFIG *config)
{
  int i;
  memset(slot, 0, sizeof(*slot));
  slot->entry = entry;
  choose_tasks(slot, config);
  if(slot->task_count > 1)
    return prepare_snapshot(slot);
  for(i = 0; i < slot->task_count; ++i)
  {
    slot->tasks[i].input = fopen(entry->path, "rb");
    if(!slot->tasks[i].input)
      return -1;
  }
  return slot->task_count ? 0 : -1;
}

void finish_slot(FILE_SLOT *slot)
{
  int i;
  for(i = 0; i < slot->task_count; ++i)
  {
    if(slot->tasks[i].input)
      fclose(slot->tasks[i].input);
    finish_candidate(&slot->tasks[i].candidate);
  }
  if(slot->snapshot)
    fclose(slot->snapshot);
  memset(slot, 0, sizeof(*slot));
}

int compress_task(COMPRESS_TASK *task,
  const COMPRESSION_CONFIG *config)
{
  CANDIDATE *candidate = &task->candidate;
  ENTRY *entry = task->slot->entry;
  if(task->engine == FAST_ENGINE)
    return make_fast_candidate(candidate, task->input,
      entry->expected_size, config->fast_level);
  if(task->engine == ECT_ENGINE)
    return make_ect_candidate(candidate, task->input,
      entry->expected_size, &config->ect);
  if(task->engine == TURTLE_ENGINE)
    return make_turtle_candidate(candidate, task->input,
      entry, &config->turtle, NULL);
  return make_zlib_candidate(candidate, task->input,
    entry, config->zlib_level);
}

/* Keep only tiny results in RAM; tmpfile() removes the rest on close. */
int spool_candidate(CANDIDATE *candidate)
{
  FILE *file;
  if(candidate->size == UINT64_MAX)
  {
    free(candidate->data);
    candidate->data = NULL;
    return 0;
  }
  if(!candidate->data || candidate->size <= 65536)
    return 0;
  file = tmpfile();
  if(!file)
    return -1;
  if(fwrite(candidate->data, 1, (size_t)candidate->size, file) !=
    candidate->size)
  {
    fclose(file);
    return -1;
  }
  candidate->file = file;
  free(candidate->data);
  candidate->data = NULL;
  return 0;
}

static int best_task(FILE_SLOT *slot)
{
  uint64_t smallest = UINT64_MAX;
  uint32_t crc = 0;
  int best = -1;
  int i;
  for(i = 0; i < slot->task_count; ++i)
  {
    CANDIDATE *candidate = &slot->tasks[i].candidate;
    if(i && candidate->crc != crc)
      return -2;
    crc = candidate->crc;
    if(best < 0 || candidate->size < smallest ||
      (candidate->size == smallest &&
        slot->tasks[i].engine == TURTLE_ENGINE))
    {
      smallest = candidate->size;
      best = i;
    }
  }
  return best;
}

int write_slot(void *zip, FILE_SLOT *slot, int zip_level)
{
  FILE *input = slot->snapshot;
  CANDIDATE *candidate;
  int best = best_task(slot);
  int store;
  int result;
  if(best < 0)
    return -1;
  candidate = &slot->tasks[best].candidate;
  store = !slot->zlib_only && slot->entry->expected_size &&
    candidate->size >= slot->entry->expected_size;
  if(store && !input)
  {
    input = fopen(slot->entry->path, "rb");
    if(!input)
      return -1;
  }
  result = write_prepared_entry(zip, slot->entry, input,
    candidate, store, zip_level);
  if(input && input != slot->snapshot)
    fclose(input);
  return result;
}
