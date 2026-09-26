#include "katzip_internal.h"

static int write_deflated_output(void *zip, FILE *temporary, z_stream *stream,
  unsigned char *output, int flush, ENTRY *entry, int *result)
{
  size_t produced;
  stream->next_out = output;
  stream->avail_out = 1048576;
  *result = deflate(stream, flush);
  if(*result != Z_OK && *result != Z_STREAM_END)
    return -1;
  produced = 1048576 - stream->avail_out;
  if(temporary ? fwrite(output, 1, produced, temporary) != produced :
    write_zip_bytes(zip, output, produced))
    return -1;
  entry->compressed_size += produced;
  return 0;
}

static int deflate_has_pending_output(const z_stream *stream,
  int flush, int result)
{
  return stream->avail_in || stream->avail_out == 0 ||
    (flush == Z_FINISH && result != Z_STREAM_END);
}

/* zlib keeps memory bounded when a file is too large for libdeflate. */
static int deflate_stream_chunk(void *zip, FILE *temporary, z_stream *stream,
  unsigned char *output, int flush, ENTRY *entry)
{
  int result;
  do
  {
    if(write_deflated_output(zip, temporary, stream, output,
      flush, entry, &result))
      return -1;
  } while(deflate_has_pending_output(stream, flush, result));
  return 0;
}

static int compress_input_chunk(void *zip, FILE *temporary,
  ENTRY *entry, FILE *in,
  z_stream *stream, unsigned char *input, unsigned char *output,
  PROGRESS *progress, uint32_t *crc, size_t *size)
{
  *size = fread(input, 1, 1048576, in);
  if(ferror(in) || (uint64_t)entry->size + *size > entry->expected_size)
    return -1;
  *crc = update_crc(*crc, input, *size);
  entry->size += (uint32_t)*size;
  progress_block(progress, (uint32_t)*size);
  stream->next_in = input;
  stream->avail_in = (uInt)*size;
  if(deflate_stream_chunk(zip, temporary, stream, output,
    *size ? Z_NO_FLUSH : Z_FINISH, entry))
    return -1;
  progress_update(progress, entry->size);
  return 0;
}

static int compress_stream_data(void *zip, FILE *temporary,
  ENTRY *entry, FILE *in,
  z_stream *stream, unsigned char *input, unsigned char *output,
  PROGRESS *progress, uint32_t *crc)
{
  size_t size;
  do
  {
    if(compress_input_chunk(zip, temporary, entry, in, stream,
      input, output,
      progress, crc, &size))
      return -1;
  } while(size);
  return entry->size == entry->expected_size ? 0 : -1;
}

static int compress_stream_entry(void *zip, FILE *temporary,
  ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress, uint32_t *crc)
{
  unsigned char *input = malloc(1048576);
  unsigned char *output = malloc(1048576);
  z_stream stream;
  int result = -1;
  if(!input || !output)
  {
    free(output);
    free(input);
    return -1;
  }
  memset(&stream, 0, sizeof(stream));
  if(deflateInit2(&stream, level > 9 ? 9 : level, Z_DEFLATED,
    -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
  {
    free(output);
    free(input);
    return -1;
  }
  if(temporary || !open_zip_entry(zip, entry,
    MZ_COMPRESS_METHOD_DEFLATE, zip_level))
    result = compress_stream_data(zip, temporary, entry, in, &stream,
      input, output, progress, crc);
  deflateEnd(&stream);
  free(output);
  free(input);
  return result;
}
int write_zlib_entry(void *zip, ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress)
{
  uint32_t crc = UINT32_MAX;
  int result;
  progress_start(progress, entry, NULL);
  result = compress_stream_entry(zip, NULL, entry, in, level,
    zip_level, progress, &crc);
  if(!result)
    result = close_zip_entry(zip, entry, crc);
  progress_finish(progress, result == 0);
  return result;
}

int make_zlib_candidate(CANDIDATE *candidate, FILE *in,
  const ENTRY *entry, int level)
{
  ENTRY copy = *entry;
  uint32_t crc = UINT32_MAX;
  int result;
  candidate->file = tmpfile();
  if(!candidate->file)
    return -1;
  copy.size = 0;
  copy.compressed_size = 0;
  result = compress_stream_entry(NULL, candidate->file, &copy, in,
    level, 0, NULL, &crc);
  if(result)
    return -1;
  candidate->size = copy.compressed_size;
  candidate->crc = crc ^ UINT32_MAX;
  return 0;
}
