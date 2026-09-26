#include "katzip_internal.h"

static size_t compress_small_buffer(struct libdeflate_compressor *compressor,
  const unsigned char *input, unsigned char *output, size_t size)
{
  if(size)
    return libdeflate_deflate_compress(compressor, input, size,
      output, size + 16);
  output[0] = 0x03;
  output[1] = 0x00;
  return 2;
}

static int small_result_is_stored(uint32_t size, size_t compressed_size)
{
  return size && (!compressed_size || compressed_size >= size);
}

static int write_small_result(void *zip, ENTRY *entry,
  const unsigned char *input, const unsigned char *output,
  size_t compressed_size, int zip_level)
{
  int store = small_result_is_stored(entry->size, compressed_size);
  int method = store ? MZ_COMPRESS_METHOD_STORE :
    MZ_COMPRESS_METHOD_DEFLATE;
  const unsigned char *data = store ? input : output;
  entry->compressed_size = store ? entry->size : compressed_size;
  if(open_zip_entry(zip, entry, method, zip_level))
    return -1;
  return write_zip_bytes(zip, data, (size_t)entry->compressed_size);
}

/* Small files fit in memory, so libdeflate can choose Store when useful. */
static int compress_small_data(void *zip, ENTRY *entry, FILE *in,
  struct libdeflate_compressor *compressor, unsigned char *input,
  unsigned char *output, int zip_level, PROGRESS *progress, uint32_t *crc)
{
  size_t size = entry->expected_size;
  size_t compressed_size;
  if(fread(input, 1, size, in) != size || fgetc(in) != EOF || ferror(in))
    return -1;
  entry->size = (uint32_t)size;
  *crc = update_crc(*crc, input, size);
  progress_block(progress, entry->size);
  compressed_size = compress_small_buffer(compressor, input, output, size);
  if(write_small_result(zip, entry, input, output,
    compressed_size, zip_level))
    return -1;
  progress_update(progress, entry->size);
  return 0;
}

static int compress_small_entry(void *zip, ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress, uint32_t *crc)
{
  size_t size = entry->expected_size;
  unsigned char *input = malloc(size ? size : 1);
  unsigned char *output = malloc(size + 16);
  struct libdeflate_compressor *compressor =
    libdeflate_alloc_compressor(level);
  int result = -1;
  if(input && output && compressor)
    result = compress_small_data(zip, entry, in, compressor, input,
      output, zip_level, progress, crc);
  if(compressor)
    libdeflate_free_compressor(compressor);
  free(output);
  free(input);
  return result;
}
int write_fast_entry(void *zip, ENTRY *entry, FILE *in,
  int level, int zip_level, PROGRESS *progress)
{
  uint32_t crc = UINT32_MAX;
  int result;
  progress_start(progress, entry, NULL);
  result = compress_small_entry(zip, entry, in, level,
    zip_level, progress, &crc);
  if(!result)
    result = close_zip_entry(zip, entry, crc);
  progress_finish(progress, result == 0);
  return result;
}
