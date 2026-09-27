/* Test helper: change one ZIP payload byte when validation opens the file. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int (*original_open)(const char *, int, ...);
static int changed;

int open(const char *path, int flags, ...)
{
  unsigned char header[30];
  unsigned char byte;
  off_t payload;
  int file;
  int writer;
  mode_t mode = 0;
  if(flags & O_CREAT)
  {
    va_list arguments;
    va_start(arguments, flags);
    mode = (mode_t)va_arg(arguments, int);
    va_end(arguments);
  }
  if(!original_open)
    original_open = dlsym(RTLD_NEXT, "open");
  file = original_open(path, flags, mode);
  if(file < 0 || changed || (flags & O_ACCMODE) != O_RDONLY ||
    !strstr(path, "archive.zip.tmp."))
    return file;
  changed = 1;
  writer = original_open(path, O_RDWR);
  if(writer < 0)
    return file;
  if(pread(writer, header, sizeof(header), 0) == sizeof(header) &&
    memcmp(header, "PK\003\004", 4) == 0)
  {
    payload = 30 + (off_t)header[26] + ((off_t)header[27] << 8) +
      (off_t)header[28] + ((off_t)header[29] << 8);
    if(pread(writer, &byte, 1, payload) == 1)
    {
      byte ^= 0x80;
      pwrite(writer, &byte, 1, payload);
    }
  }
  close(writer);
  return file;
}
