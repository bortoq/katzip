#include "katzip_internal.h"

uint32_t update_crc(uint32_t crc, const unsigned char *data, size_t size)
{
  return (uint32_t)crc32(crc ^ UINT32_MAX, data, (uInt)size) ^ UINT32_MAX;
}
int zip_level_hint(int level)
{
  if(level <= 3)
    return 1;
  if(level <= 6)
    return 2;
  if(level <= 8)
    return 6;
  return 9;
}

/* minizip-ng packages raw DEFLATE without recompressing it. */
static mz_zip_file zip_file_info(const ENTRY *entry, int method)
{
  mz_zip_file info;
  memset(&info, 0, sizeof(info));
  info.version_madeby = (3 << 8) | 20;
  info.version_needed = 20;
  info.flag = entry->flags;
  info.compression_method = method;
  info.uncompressed_size = entry->expected_size;
  info.zip64 = MZ_ZIP64_DISABLE;
  info.modified_date = entry->mtime;
  info.filename = entry->name;
  info.filename_size = entry->name_len;
  info.external_fa = entry->mode << 16;
  return info;
}

int open_zip_entry(void *zip, const ENTRY *entry, int method,
  int level)
{
  mz_zip_file info = zip_file_info(entry, method);
  return mz_zip_entry_write_open(zip, &info, level,
    1, NULL) == MZ_OK ? 0 : -1;
}

int close_zip_entry(void *zip, const ENTRY *entry, uint32_t crc)
{
  return mz_zip_entry_write_close(zip, crc ^ UINT32_MAX,
    entry->compressed_size, entry->size) == MZ_OK ? 0 : -1;
}

int write_zip_bytes(void *zip, const unsigned char *data,
  size_t size)
{
  size_t offset = 0;
  while(offset < size)
  {
    size_t chunk = size - offset;
    if(chunk > 1048576)
      chunk = 1048576;
    if(mz_zip_entry_write(zip, data + offset, (int32_t)chunk) !=
      (int32_t)chunk)
      return -1;
    offset += chunk;
  }
  return 0;
}
static int copy_stored_chunk(void *zip, FILE *in,
  unsigned char *buffer, size_t chunk, uint32_t *crc)
{
  if(fread(buffer, 1, chunk, in) != chunk)
    return -1;
  *crc = update_crc(*crc, buffer, chunk);
  return write_zip_bytes(zip, buffer, chunk);
}

static int stored_input_matches(FILE *in, uint32_t crc,
  uint32_t expected_crc)
{
  if(fgetc(in) != EOF || ferror(in))
    return 0;
  return (crc ^ UINT32_MAX) == expected_crc;
}

static size_t stored_chunk_size(uint32_t remaining, size_t capacity)
{
  return remaining < capacity ? remaining : capacity;
}

/* Read again only when DEFLATE would be larger than the original file. */
int write_stored_input(void *zip, FILE *in,
  uint32_t size, uint32_t expected_crc)
{
  unsigned char buffer[65536];
  uint32_t crc = UINT32_MAX;
  uint32_t remaining = size;
  if(fseek(in, 0, SEEK_SET))
    return -1;
  while(remaining)
  {
    size_t chunk = stored_chunk_size(remaining, sizeof(buffer));
    if(copy_stored_chunk(zip, in, buffer, chunk, &crc))
      return -1;
    remaining -= (uint32_t)chunk;
  }
  return stored_input_matches(in, crc, expected_crc) ? 0 : -1;
}

/* ECT compresses the entire file at once, so levels 7-8 need no
 * Turtledeflate pass or temporary DEFLATE stream. */
