#include "katzip_internal.h"

void finish_turtle_work(TURTLE_WORK *work)
{
  if(work->temporary)
    fclose(work->temporary);
  if(work->compressor)
    turtledeflate_destroy(work->compressor);
  free(work->buffer);
}

int prepare_turtle_work(TURTLE_WORK *work,
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

static int start_turtle_blocks(TURTLE_WORK *work,
  const turtledeflate_config_t *config, PROGRESS *progress)
{
  turtledeflate_config_t copy = *config;
  if(!turtledeflate_create(&work->compressor, &copy))
    return -1;
  turtledeflate_set_progress_callback(work->compressor,
    progress_callback, progress);
  return 0;
}

/* Return 1 if another block follows, 0 for the final block, -1 on error. */
static int compress_turtle_block(void *zip, ENTRY *entry, FILE *in,
  TURTLE_WORK *work, PROGRESS *progress, size_t size)
{
  unsigned char *compressed;
  int more = input_has_more(in);
  int compressed_size;
  if(more < 0 || (uint64_t)entry->size + size > UINT32_MAX)
    return -1;
  work->crc = update_crc(work->crc, work->buffer, size);
  entry->size += (uint32_t)size;
  progress_block(progress, (uint32_t)size);
  compressed_size = turtledeflate_block(work->compressor,
    (int32_t)size, work->buffer, &compressed, NULL, !more);
  if(compressed_size < 0 || write_deflate_chunk(zip, work->temporary,
    compressed, (size_t)compressed_size))
    return -1;
  work->compressed_size += (uint32_t)compressed_size;
  progress_update(progress, entry->size);
  return more;
}

static int write_empty_turtle_block(void *zip, TURTLE_WORK *work)
{
  const unsigned char empty_deflate[] = {0x03, 0x00};
  if(write_deflate_chunk(zip, work->temporary,
    empty_deflate, sizeof(empty_deflate)))
    return -1;
  work->compressed_size = sizeof(empty_deflate);
  return 0;
}

static int prepare_turtle_blocks(TURTLE_WORK *work, FILE *in,
  const turtledeflate_config_t *config, PROGRESS *progress, size_t *size)
{
  *size = fread(work->buffer, 1,
    (size_t)config->i_maximum_block_size, in);
  if(ferror(in))
    return -1;
  if(!*size)
    return 0;
  return start_turtle_blocks(work, config, progress);
}

static int compress_next_turtle_block(void *zip, ENTRY *entry, FILE *in,
  const turtledeflate_config_t *config, TURTLE_WORK *work,
  PROGRESS *progress, size_t *size)
{
  int more = compress_turtle_block(zip, entry, in, work, progress, *size);
  if(more < 0)
    return -1;
  if(!more)
  {
    *size = 0;
    return 0;
  }
  *size = fread(work->buffer, 1,
    (size_t)config->i_maximum_block_size, in);
  if(!*size || ferror(in))
    return -1;
  return 0;
}

static int finish_empty_turtle_input(void *zip, ENTRY *entry,
  TURTLE_WORK *work)
{
  if(entry->expected_size)
    return -1;
  return write_empty_turtle_block(zip, work);
}

int compress_turtle_blocks(void *zip, ENTRY *entry, FILE *in,
  const turtledeflate_config_t *config, TURTLE_WORK *work,
  PROGRESS *progress)
{
  size_t size;
  if(prepare_turtle_blocks(work, in, config, progress, &size))
    return -1;
  if(!size)
    return finish_empty_turtle_input(zip, entry, work);
  while(size)
    if(compress_next_turtle_block(zip, entry, in, config, work,
      progress, &size))
      return -1;
  return entry->size == entry->expected_size ? 0 : -1;
}

int write_entry(void *zip, ENTRY *entry, FILE *in,
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
