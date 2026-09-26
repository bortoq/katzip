#include "katzip_internal.h"

void *ect_worker(void *argument)
{
  ECT_JOB *job = (ECT_JOB*)argument;
  unsigned char bit_position = 0;
  ZopfliDeflate(&job->options, 1, job->input, job->input_size,
    &bit_position, &job->output, &job->output_size);
  free(job->input);
  job->input = NULL;
  return NULL;
}

static int ect_input_unchanged(FILE *in)
{
  int next = fgetc(in);
  if(next != EOF || ferror(in))
    return 0;
  return fseek(in, 0, SEEK_SET) == 0;
}

static int read_ect_input(FILE *in, ECT_JOB *job, uint32_t size)
{
  if(fread(job->input, 1, size, in) != size)
    return -1;
  return ect_input_unchanged(in) ? 0 : -1;
}

/* Read a stable snapshot before ECT starts compressing in the worker. */
int start_ect_job(ECT_JOB *job, FILE *in, uint32_t size,
  const ZopfliOptions *options)
{
  memset(job, 0, sizeof(*job));
  job->options = *options;
  if(!size)
    return 0;
  if((uint64_t)size + 16 > SIZE_MAX)
    return 0;
  job->input = malloc((size_t)size + 16);
  if(!job->input)
    return 0;
  if(read_ect_input(in, job, size))
    return -1;
  memset(job->input + size, 0, 16);
  job->input_size = size;
  job->crc = update_crc(UINT32_MAX, job->input, size) ^ UINT32_MAX;
  if(pthread_create(&job->thread, NULL, ect_worker, job))
    return 0;
  job->started = 1;
  return 0;
}
void complete_ect_job(ECT_JOB *job, PROGRESS *progress)
{
  if(job->started)
  {
    progress_wait(progress);
    pthread_join(job->thread, NULL);
    job->started = 0;
  }
  else if(job->input)
    ect_worker(job);
}

static int ect_result_is_stored(const ECT_JOB *job, uint32_t size)
{
  return size && (!job->output || !job->output_size ||
    job->output_size >= size);
}

static int prepare_ect_entry(ENTRY *entry, const ECT_JOB *job)
{
  static const unsigned char empty_deflate[] = {0x03, 0x00};
  int store = ect_result_is_stored(job, entry->size);
  entry->compressed_size = store ? entry->size :
    entry->size ? job->output_size : sizeof(empty_deflate);
  return entry->compressed_size > UINT32_MAX ? -1 : store;
}

static int write_ect_payload(void *zip, ENTRY *entry, FILE *in,
  const ECT_JOB *job, int store)
{
  static const unsigned char empty_deflate[] = {0x03, 0x00};
  const unsigned char *data = entry->size ? job->output : empty_deflate;
  if(store)
    return write_stored_input(zip, in, entry->size, job->crc);
  return write_zip_bytes(zip, data, (size_t)entry->compressed_size);
}

static int write_ect_result(void *zip, ENTRY *entry, FILE *in,
  const ECT_JOB *job, int zip_level)
{
  int store = prepare_ect_entry(entry, job);
  int method;
  if(store < 0)
    return -1;
  method = store ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE;
  if(open_zip_entry(zip, entry, method, zip_level))
    return -1;
  if(write_ect_payload(zip, entry, in, job, store))
    return -1;
  return close_zip_entry(zip, entry, job->crc ^ UINT32_MAX);
}

/* ECT compresses the entire file at once, with no Turtledeflate pass. */
int write_ect_entry(void *zip, ENTRY *entry, FILE *in,
  const ZopfliOptions *options, int zip_level, PROGRESS *progress)
{
  ECT_JOB job;
  int result;
  if(start_ect_job(&job, in, entry->expected_size, options))
  {
    free(job.input);
    return -1;
  }
  if(entry->expected_size && !job.started && !job.input)
    return -1;
  progress_start(progress, entry, NULL);
  complete_ect_job(&job, progress);
  entry->size = entry->expected_size;
  result = write_ect_result(zip, entry, in, &job, zip_level);
  free(job.input);
  free(job.output);
  progress_finish(progress, result == 0);
  return result;
}

/* Run ECT in the caller's worker; no nested thread is created. */
int make_ect_candidate(CANDIDATE *candidate, FILE *in,
  uint32_t size, const ZopfliOptions *options)
{
  ECT_JOB job = {0};
  if(!size)
  {
    candidate->data = malloc(2);
    if(!candidate->data)
      return -1;
    candidate->data[0] = 0x03;
    candidate->data[1] = 0x00;
    candidate->size = 2;
    return 0;
  }
  if((uint64_t)size + 16 > SIZE_MAX)
    return -1;
  job.input = malloc((size_t)size + 16);
  if(!job.input)
    return -1;
  if(read_ect_input(in, &job, size))
  {
    free(job.input);
    return -1;
  }
  memset(job.input + size, 0, 16);
  job.input_size = size;
  job.options = *options;
  job.crc = update_crc(UINT32_MAX, job.input, size) ^ UINT32_MAX;
  ect_worker(&job);
  candidate->data = job.output;
  candidate->size = job.output && job.output_size ?
    job.output_size : UINT64_MAX;
  candidate->crc = job.crc;
  return 0;
}
