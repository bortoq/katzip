#include "katzip_internal.h"

void finish_candidate(CANDIDATE *candidate)
{
  free(candidate->data);
  if(candidate->file)
    fclose(candidate->file);
}

static int read_candidate_input(FILE *in, unsigned char *input,
  uint32_t size)
{
  if(fseek(in, 0, SEEK_SET))
    return -1;
  if(fread(input, 1, size, in) != size)
    return -1;
  if(fgetc(in) != EOF || ferror(in))
    return -1;
  return 0;
}

static void compress_fast_candidate(CANDIDATE *candidate,
  struct libdeflate_compressor *compressor,
  const unsigned char *input, uint32_t size)
{
  size_t result;
  candidate->crc = update_crc(UINT32_MAX, input, size) ^ UINT32_MAX;
  if(!size)
  {
    candidate->data[0] = 0x03;
    candidate->data[1] = 0x00;
    candidate->size = 2;
    return;
  }
  result = libdeflate_deflate_compress(compressor, input, size,
    candidate->data, (size_t)size + 16);
  candidate->size = result ? result : UINT64_MAX;
}

/* libdeflate needs a complete input buffer. Retain only its DEFLATE result. */
int make_fast_candidate(CANDIDATE *candidate, FILE *in,
  uint32_t size, int level)
{
  struct libdeflate_compressor *compressor;
  unsigned char *input;
  int result = -1;
  if((uint64_t)size + 16 > SIZE_MAX)
    return -1;
  input = malloc(size ? (size_t)size : 1);
  candidate->data = malloc((size_t)size + 16);
  compressor = libdeflate_alloc_compressor(level);
  if(input && candidate->data && compressor &&
    !read_candidate_input(in, input, size))
  {
    compress_fast_candidate(candidate, compressor, input, size);
    result = 0;
  }
  free(input);
  if(compressor)
    libdeflate_free_compressor(compressor);
  return result;
}

/* Turtledeflate streams to a temporary file so its result need not fit RAM. */
int make_turtle_candidate(CANDIDATE *candidate, FILE *in,
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

static int copy_candidate_chunk(void *zip, FILE *file,
  unsigned char *buffer, size_t chunk)
{
  if(fread(buffer, 1, chunk, file) != chunk)
    return -1;
  return write_zip_bytes(zip, buffer, chunk);
}

static size_t candidate_chunk_size(uint64_t remaining, size_t capacity)
{
  return remaining < capacity ? (size_t)remaining : capacity;
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
    size_t chunk = candidate_chunk_size(remaining, sizeof(buffer));
    if(copy_candidate_chunk(zip, candidate->file, buffer, chunk))
      return -1;
    remaining -= chunk;
  }
  return 0;
}

static int candidate_enabled(const COMPRESSION_CONFIG *config, int index)
{
  if(index == 0)
    return config->have_fast;
  if(index == 1)
    return config->have_ect;
  return config->have_turtle;
}

static int candidate_preferred(int index, int best, uint64_t size,
  uint64_t best_size)
{
  if(size < best_size)
    return 1;
  return index == 2 && best >= 0 && size == best_size;
}

typedef struct {
  uint64_t size;
  uint32_t crc;
  int index;
  int have_crc;
} BEST_CANDIDATE;

/* Return -1 if compressors saw different input data. */
static int consider_candidate(BEST_CANDIDATE *best,
  const CANDIDATE *candidate, int index)
{
  if(!best->have_crc)
  {
    best->crc = candidate->crc;
    best->have_crc = 1;
  }
  else if(candidate->crc != best->crc)
    return -1;
  if(candidate_preferred(index, best->index, candidate->size, best->size))
  {
    best->size = candidate->size;
    best->index = index;
  }
  return 0;
}

/* Return -2 if encoders disagree on the input checksum. */
static int select_best_candidate(CANDIDATE *candidates,
  const COMPRESSION_CONFIG *config, uint64_t *best_size, uint32_t *crc)
{
  BEST_CANDIDATE best = {UINT64_MAX, 0, -1, 0};
  int i;
  for(i = 0; i < 3; ++i)
  {
    if(!candidate_enabled(config, i))
      continue;
    if(consider_candidate(&best, &candidates[i], i))
      return -2;
  }
  *best_size = best.size;
  *crc = best.crc;
  return best.index;
}

static int selected_candidate_is_stored(const ENTRY *entry,
  CANDIDATE *candidates, int best)
{
  if(!entry->expected_size)
    return 0;
  return best < 0 || candidates[best].size >= entry->expected_size;
}

static int prepare_selected_entry(ENTRY *entry, CANDIDATE *candidates,
  int best, int store)
{
  if(!store && best < 0)
    return -1;
  entry->size = entry->expected_size;
  entry->compressed_size = store ? entry->size : candidates[best].size;
  return entry->compressed_size > UINT32_MAX ? -1 : 0;
}

static int write_selected_payload(void *zip, ENTRY *entry, FILE *in,
  CANDIDATE *candidates, int best, uint32_t crc, int store)
{
  if(store)
    return write_stored_input(zip, in, entry->size, crc);
  return copy_candidate(zip, &candidates[best]);
}

static int write_selected_candidate(void *zip, ENTRY *entry, FILE *in,
  CANDIDATE *candidates, int best, uint32_t crc, int zip_level)
{
  int store = selected_candidate_is_stored(entry, candidates, best);
  int method = store ? MZ_COMPRESS_METHOD_STORE :
    MZ_COMPRESS_METHOD_DEFLATE;
  if(prepare_selected_entry(entry, candidates, best, store))
    return -1;
  if(open_zip_entry(zip, entry, method, zip_level))
    return -1;
  if(write_selected_payload(zip, entry, in, candidates, best, crc, store))
    return -1;
  return close_zip_entry(zip, entry, crc ^ UINT32_MAX);
}

static int write_best_candidate(void *zip, ENTRY *entry, FILE *in,
  CANDIDATE *candidates, const COMPRESSION_CONFIG *config, int zip_level)
{
  uint64_t best_size;
  uint32_t crc = 0;
  int best = select_best_candidate(candidates, config, &best_size, &crc);
  if(best == -2)
    return -1;
  return write_selected_candidate(zip, entry, in, candidates, best,
    crc, zip_level);
}

/* Package a prepared raw stream without changing ZIP metadata. */
int write_prepared_entry(void *zip, ENTRY *entry, FILE *in,
  CANDIDATE *candidate, int store, int zip_level)
{
  int method = store ? MZ_COMPRESS_METHOD_STORE :
    MZ_COMPRESS_METHOD_DEFLATE;
  entry->size = entry->expected_size;
  entry->compressed_size = store ? entry->size : candidate->size;
  if(entry->compressed_size > UINT32_MAX)
    return -1;
  if(open_zip_entry(zip, entry, method, zip_level))
    return -1;
  if(store ? write_stored_input(zip, in, entry->size,
    candidate->crc) : copy_candidate(zip, candidate))
    return -1;
  return close_zip_entry(zip, entry, candidate->crc ^ UINT32_MAX);
}

static int start_race_ect(ECT_JOB *job, FILE *in,
  const ENTRY *entry, const COMPRESSION_CONFIG *config)
{
  if(!config->have_ect)
    return 0;
  if(start_ect_job(job, in, entry->expected_size, &config->ect))
    return -1;
  if(entry->expected_size && !job->started && !job->input)
    return -1;
  return 0;
}

static int collect_non_ect_candidates(CANDIDATE *candidates,
  FILE *in, const ENTRY *entry, const COMPRESSION_CONFIG *config,
  PROGRESS *progress)
{
  if(config->have_fast && make_fast_candidate(&candidates[0], in,
    entry->expected_size, config->fast_level))
    return -1;
  if(config->have_turtle && make_turtle_candidate(&candidates[2], in,
    entry, &config->turtle, progress))
    return -1;
  return 0;
}

static int collect_finished_ect(CANDIDATE *candidate, ECT_JOB *ect,
  uint32_t size)
{
  collect_ect_candidate(candidate, ect, size);
  if(!size && !candidate->data)
    return -1;
  return 0;
}

static int collect_race_candidates(CANDIDATE *candidates, ECT_JOB *ect,
  FILE *in, const ENTRY *entry, const COMPRESSION_CONFIG *config,
  PROGRESS *progress)
{
  int result = collect_non_ect_candidates(candidates, in, entry,
    config, progress);
  if(ect->started)
    complete_ect_job(ect, progress);
  else if(!result && ect->input)
    ect_worker(ect);
  if(result || !config->have_ect)
    return result;
  return collect_finished_ect(&candidates[1], ect, entry->expected_size);
}

static void finish_race_candidates(CANDIDATE *candidates, ECT_JOB *ect)
{
  int i;
  free(ect->input);
  free(ect->output);
  for(i = 0; i < 3; ++i)
    finish_candidate(&candidates[i]);
}

/* All enabled compressors process the same input; keep the shortest stream. */
int write_race_entry(void *zip, ENTRY *entry, FILE *in,
  const COMPRESSION_CONFIG *config, int zip_level, PROGRESS *progress)
{
  CANDIDATE candidates[3] = {{0}};
  ECT_JOB ect = {0};
  int result;
  progress_start(progress, entry,
    config->have_turtle ? &config->turtle : NULL);
  result = start_race_ect(&ect, in, entry, config);
  if(!result)
    result = collect_race_candidates(candidates, &ect, in, entry,
      config, progress);
  if(ect.started)
    complete_ect_job(&ect, progress);
  if(!result)
    result = write_best_candidate(zip, entry, in,
      candidates, config, zip_level);
  finish_race_candidates(candidates, &ect);
  progress_finish(progress, result == 0);
  return result;
}
