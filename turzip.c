#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "turtledeflate_api.h"

enum { BLOCK_SIZE = 32768 };

typedef struct {
  const char *path;
  const char *name;
  uint16_t name_len;
  uint16_t flags;
  uint16_t dos_time;
  uint16_t dos_date;
  uint32_t mode;
  uint32_t offset;
  uint32_t crc;
  uint32_t compressed_size;
  uint32_t expected_size;
  uint32_t size;
} ENTRY;

typedef struct {
  FILE *file;
  uint64_t offset;
} OUTPUT;

typedef struct {
  int subblocks;
  int split_iterations;
  int internal_iterations;
  int points;
  int starts;
  int min_fp;
  int max_fp;
} PRESET;

static int write_bytes(OUTPUT *out, const void *data, size_t size)
{
  if(out->offset + size > UINT32_MAX || fwrite(data, 1, size, out->file) != size)
    return -1;
  out->offset += size;
  return 0;
}

static int write_u16(OUTPUT *out, uint16_t value)
{
  unsigned char bytes[2];
  bytes[0] = (unsigned char)value;
  bytes[1] = (unsigned char)(value >> 8);
  return write_bytes(out, bytes, sizeof(bytes));
}

static int write_u32(OUTPUT *out, uint32_t value)
{
  unsigned char bytes[4];
  bytes[0] = (unsigned char)value;
  bytes[1] = (unsigned char)(value >> 8);
  bytes[2] = (unsigned char)(value >> 16);
  bytes[3] = (unsigned char)(value >> 24);
  return write_bytes(out, bytes, sizeof(bytes));
}

static uint32_t update_crc(uint32_t crc, const unsigned char *data, size_t size)
{
  size_t i;
  int bit;
  for(i = 0; i < size; ++i)
  {
    crc ^= data[i];
    for(bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (crc & 1 ? UINT32_C(0xedb88320) : 0);
  }
  return crc;
}

static int valid_name(const char *name)
{
  const char *part;
  const char *end;
  if(!*name || *name == '/')
    return 0;
  for(part = name; *part; part = *end ? end + 1 : end)
  {
    end = strchr(part, '/');
    if(!end)
      end = part + strlen(part);
    if(end == part || (end - part == 2 && part[0] == '.' && part[1] == '.'))
      return 0;
    if(!*end)
      break;
  }
  return 1;
}

/* return 1 only for well-formed utf-8 */
static int valid_utf8(const char *name)
{
  const unsigned char *p = (const unsigned char*)name;
  uint32_t codepoint;
  uint32_t minimum;
  int count;
  int i;
  while(*p)
  {
    if(*p < 0x80)
    {
      ++p;
      continue;
    }
    if(*p >= 0xc2 && *p <= 0xdf)
    {
      codepoint = *p++ & 0x1f;
      minimum = 0x80;
      count = 1;
    }
    else if(*p >= 0xe0 && *p <= 0xef)
    {
      codepoint = *p++ & 0x0f;
      minimum = 0x800;
      count = 2;
    }
    else if(*p >= 0xf0 && *p <= 0xf4)
    {
      codepoint = *p++ & 0x07;
      minimum = 0x10000;
      count = 3;
    }
    else
      return 0;
    for(i = 0; i < count; ++i)
    {
      if(!*p || (*p & 0xc0) != 0x80)
        return 0;
      codepoint = (codepoint << 6) | (*p++ & 0x3f);
    }
    if(codepoint < minimum || (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff)
      return 0;
  }
  return 1;
}

static void show_progress(const ENTRY *entry, int percent)
{
  int i;
  fprintf(stderr, "\r%s [", entry->name);
  for(i = 0; i < 30; ++i)
    fputc(i < percent * 30 / 100 ? '#' : '-', stderr);
  fprintf(stderr, "] %3d%%", percent);
  fflush(stderr);
}

static void set_dos_time(ENTRY *entry, time_t value)
{
  struct tm *date = localtime(&value);
  int year;
  if(!date)
  {
    entry->dos_time = 0;
    entry->dos_date = (1 << 5) | 1;
    return;
  }
  year = date->tm_year + 1900;
  if(year < 1980)
    year = 1980;
  if(year > 2107)
    year = 2107;
  entry->dos_time = (uint16_t)((date->tm_hour << 11) | (date->tm_min << 5) | (date->tm_sec / 2));
  entry->dos_date = (uint16_t)(((year - 1980) << 9) | ((date->tm_mon + 1) << 5) | date->tm_mday);
}

static int write_local_header(OUTPUT *out, const ENTRY *entry)
{
  return write_u32(out, UINT32_C(0x04034b50)) ||
    write_u16(out, 20) || write_u16(out, entry->flags) || write_u16(out, 8) ||
    write_u16(out, entry->dos_time) || write_u16(out, entry->dos_date) ||
    write_u32(out, 0) || write_u32(out, 0) || write_u32(out, 0) ||
    write_u16(out, entry->name_len) || write_u16(out, 0) ||
    write_bytes(out, entry->name, entry->name_len) ? -1 : 0;
}

static int write_descriptor(OUTPUT *out, const ENTRY *entry)
{
  return write_u32(out, UINT32_C(0x08074b50)) || write_u32(out, entry->crc) ||
    write_u32(out, entry->compressed_size) || write_u32(out, entry->size) ? -1 : 0;
}

static int write_central_header(OUTPUT *out, const ENTRY *entry)
{
  return write_u32(out, UINT32_C(0x02014b50)) ||
    write_u16(out, (3 << 8) | 20) || write_u16(out, 20) ||
    write_u16(out, entry->flags) || write_u16(out, 8) ||
    write_u16(out, entry->dos_time) || write_u16(out, entry->dos_date) ||
    write_u32(out, entry->crc) || write_u32(out, entry->compressed_size) ||
    write_u32(out, entry->size) || write_u16(out, entry->name_len) ||
    write_u16(out, 0) || write_u16(out, 0) || write_u16(out, 0) ||
    write_u16(out, 0) || write_u32(out, entry->mode << 16) ||
    write_u32(out, entry->offset) ||
    write_bytes(out, entry->name, entry->name_len) ? -1 : 0;
}

static int write_end(OUTPUT *out, uint16_t count, uint32_t central_offset, uint32_t central_size)
{
  return write_u32(out, UINT32_C(0x06054b50)) ||
    write_u16(out, 0) || write_u16(out, 0) ||
    write_u16(out, count) || write_u16(out, count) ||
    write_u32(out, central_size) || write_u32(out, central_offset) ||
    write_u16(out, 0) ? -1 : 0;
}

static turtledeflate_config_t compression_config(int level)
{
  static const PRESET presets[9] = {
    {1, 1, 1, 3, 2, -3, 1},
    {2, 1, 1, 3, 2, -3, 1},
    {2, 1, 2, 5, 2, -3, 1},
    {4, 1, 2, 5, 3, -3, 1},
    {4, 2, 3, 7, 3, -3, 1},
    {8, 2, 3, 7, 3, -3, 1},
    {8, 2, 4, 7, 3, -3, 1},
    {16, 4, 10, 15, 6, -4, 3},
    {32, 8, 24, 31, 10, -6, 5}
  };
  const PRESET *preset = &presets[level - 1];
  turtledeflate_config_t config;
  memset(&config, 0, sizeof(config));
  config.i_compression_level = level;
  config.i_maximum_block_size = BLOCK_SIZE;
  config.i_maximum_subblocks = preset->subblocks;
  config.i_max_block_splitter_iterations = preset->split_iterations;
  config.i_max_internal_block_splitter_iterations = preset->internal_iterations;
  config.i_block_splitter_num_points = preset->points;
  config.i_block_splitter_center_dist = preset->points / 4 + 1;
  config.i_block_splitter_min_range_for_points = 1024;
  config.i_min_start_fp = preset->min_fp;
  config.i_max_start_fp = preset->max_fp;
  config.i_num_start_fp = preset->starts;
  return config;
}

static int write_entry(OUTPUT *out, ENTRY *entry, FILE *in, int level)
{
  unsigned char buffer[BLOCK_SIZE];
  unsigned char *compressed;
  turtledeflate_config_t config = compression_config(level);
  void *compressor = NULL;
  uint64_t start;
  uint32_t crc = UINT32_MAX;
  size_t size;
  int next;
  int compressed_size;
  int percent;
  int last_percent = 0;
  int status = -1;

  entry->offset = (uint32_t)out->offset;
  if(write_local_header(out, entry))
    return -1;
  start = out->offset;
  size = fread(buffer, 1, sizeof(buffer), in);
  if(ferror(in))
    return -1;
  if(size && !turtledeflate_create(&compressor, &config))
    return -1;
  show_progress(entry, 0);
  while(size)
  {
    next = fgetc(in);
    if(next != EOF && ungetc(next, in) == EOF)
      goto done;
    if(next == EOF && ferror(in))
      goto done;
    if((uint64_t)entry->size + size > UINT32_MAX)
      goto done;
    crc = update_crc(crc, buffer, size);
    entry->size += (uint32_t)size;
    compressed_size = turtledeflate_block(compressor, (int32_t)size, buffer, &compressed, NULL, next == EOF);
    if(compressed_size < 0 || write_bytes(out, compressed, (size_t)compressed_size))
      goto done;
    percent = entry->expected_size ? (int)((uint64_t)entry->size * 100 / entry->expected_size) : 100;
    if(percent > 100)
      percent = 100;
    if(percent > last_percent)
    {
      show_progress(entry, percent);
      last_percent = percent;
    }
    if(next == EOF)
      break;
    size = fread(buffer, 1, sizeof(buffer), in);
    if(!size || ferror(in))
      goto done;
  }
  if(!entry->size)
  {
    const unsigned char empty_deflate[] = {0x03, 0x00};
    if(write_bytes(out, empty_deflate, sizeof(empty_deflate)))
      goto done;
  }
  entry->crc = crc ^ UINT32_MAX;
  entry->compressed_size = (uint32_t)(out->offset - start);
  if(write_descriptor(out, entry))
    goto done;
  status = 0;

done:
  if(compressor)
    turtledeflate_destroy(compressor);
  if(status == 0 && last_percent < 100)
    show_progress(entry, 100);
  fputc('\n', stderr);
  return status;
}

int main(int argc, char **argv)
{
  ENTRY *entries;
  OUTPUT out;
  struct stat archive_stat;
  struct stat file_stat;
  FILE *in;
  uint32_t central_offset;
  uint32_t central_size;
  int archive_arg = 1;
  int file_arg;
  int level = 7;
  int i;
  int j;
  int status = 1;

  if(argc > 1 && argv[1][0] == '-')
  {
    if(argv[1][1] < '1' || argv[1][1] > '9' || argv[1][2])
    {
      fprintf(stderr, "turzip: invalid compression level: %s\n", argv[1]);
      return 1;
    }
    level = argv[1][1] - '0';
    archive_arg = 2;
  }
  file_arg = archive_arg + 1;
  if(argc <= file_arg || argc - file_arg > UINT16_MAX)
  {
    fprintf(stderr, "usage: turzip [-1..-9] archive_name file[s]\n");
    return 1;
  }
  entries = calloc((size_t)(argc - file_arg), sizeof(*entries));
  if(!entries)
  {
    fprintf(stderr, "turzip: out of memory\n");
    return 1;
  }
  for(i = file_arg; i < argc; ++i)
  {
    const char *name = argv[i];
    while(name[0] == '.' && name[1] == '/')
      name += 2;
    if(!valid_name(name) || strlen(name) > UINT16_MAX || stat(argv[i], &file_stat) ||
      !S_ISREG(file_stat.st_mode) || file_stat.st_size < 0 ||
      (uint64_t)file_stat.st_size > UINT32_MAX)
    {
      fprintf(stderr, "turzip: invalid input file: %s\n", argv[i]);
      goto done;
    }
    for(j = file_arg; j < i; ++j)
    {
      if(strcmp(name, entries[j - file_arg].name) == 0)
      {
        fprintf(stderr, "turzip: duplicate entry: %s\n", name);
        goto done;
      }
    }
    if(!stat(argv[archive_arg], &archive_stat) && archive_stat.st_dev == file_stat.st_dev &&
      archive_stat.st_ino == file_stat.st_ino)
    {
      fprintf(stderr, "turzip: archive is an input file: %s\n", argv[i]);
      goto done;
    }
    entries[i - file_arg].path = argv[i];
    entries[i - file_arg].name = name;
    entries[i - file_arg].name_len = (uint16_t)strlen(name);
    entries[i - file_arg].flags = (uint16_t)(8 | (valid_utf8(name) ? 0x0800 : 0));
    entries[i - file_arg].mode = (uint32_t)file_stat.st_mode;
    entries[i - file_arg].expected_size = (uint32_t)file_stat.st_size;
    set_dos_time(&entries[i - file_arg], file_stat.st_mtime);
  }
  out.file = fopen(argv[archive_arg], "wb");
  if(!out.file)
  {
    fprintf(stderr, "turzip: cannot create %s: %s\n", argv[archive_arg], strerror(errno));
    goto done;
  }
  out.offset = 0;
  for(i = 0; i < argc - file_arg; ++i)
  {
    in = fopen(entries[i].path, "rb");
    if(!in || write_entry(&out, &entries[i], in, level))
    {
      fprintf(stderr, "turzip: cannot archive %s\n", entries[i].path);
      if(in)
        fclose(in);
      goto output_error;
    }
    if(fclose(in))
      goto output_error;
  }
  central_offset = (uint32_t)out.offset;
  for(i = 0; i < argc - file_arg; ++i)
  {
    if(write_central_header(&out, &entries[i]))
      goto output_error;
  }
  central_size = (uint32_t)out.offset - central_offset;
  if(write_end(&out, (uint16_t)(argc - file_arg), central_offset, central_size))
    goto output_error;
  if(fclose(out.file))
  {
    remove(argv[archive_arg]);
    goto done;
  }
  status = 0;
  goto done;

output_error:
  fprintf(stderr, "turzip: failed to write archive %s\n", argv[archive_arg]);
  fclose(out.file);
  remove(argv[archive_arg]);
done:
  free(entries);
  return status;
}
