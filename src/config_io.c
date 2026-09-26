#include "katzip_internal.h"

char *trim(char *text)
{
  size_t size;
  while(isspace((unsigned char)*text))
    ++text;
  size = strlen(text);
  while(size && isspace((unsigned char)text[size - 1]))
    text[--size] = 0;
  return text;
}

static char *path_candidate(const char *directory, size_t length,
  const char *program)
{
  size_t directory_size = length ? length : 1;
  char *candidate = malloc(directory_size + strlen(program) + 2);
  char *resolved;
  if(!candidate)
    return NULL;
  if(length)
    memcpy(candidate, directory, length);
  else
    candidate[0] = '.';
  candidate[directory_size] = '/';
  strcpy(candidate + directory_size + 1, program);
  resolved = access(candidate, X_OK) == 0 ? realpath(candidate, NULL) : NULL;
  free(candidate);
  return resolved;
}

/* Resolve PATH only when argv[0] does not contain a directory. */
static char *executable_from_path(const char *program)
{
  const char *search = getenv("PATH");
  while(search)
  {
    const char *end = strchr(search, ':');
    size_t length = end ? (size_t)(end - search) : strlen(search);
    char *resolved = path_candidate(search, length, program);
    if(resolved)
      return resolved;
    search = end ? end + 1 : NULL;
  }
  return NULL;
}

/* /proc finds the real binary even when it was launched through PATH. */
static char *executable_config_path(const char *program)
{
  char *executable = realpath("/proc/self/exe", NULL);
  const char *slash;
  char *path;
  size_t directory_size;
  if(!executable)
    executable = strchr(program, '/') ?
      realpath(program, NULL) : executable_from_path(program);
  if(!executable)
    return NULL;
  slash = strrchr(executable, '/');
  directory_size = slash ? (size_t)(slash - executable + 1) : 0;
  path = malloc(directory_size + sizeof("katzip.ini"));
  if(path)
  {
    memcpy(path, executable, directory_size);
    strcpy(path + directory_size, "katzip.ini");
  }
  free(executable);
  return path;
}

static int missing_config_error(int error_number)
{
  return error_number == ENOENT || error_number == ENOTDIR;
}

/* Return 1 when a candidate does not exist, and -1 for other errors. */
static int open_named_config(const char *name, FILE **file, char **path)
{
  int saved_error;
  *path = strdup(name);
  if(!*path)
  {
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  *file = fopen(*path, "r");
  if(*file)
    return 0;
  saved_error = errno;
  if(!missing_config_error(saved_error))
    fprintf(stderr, "katzip: cannot open %s: %s\n",
      name, strerror(saved_error));
  free(*path);
  *path = NULL;
  return missing_config_error(saved_error) ? 1 : -1;
}

/* Each built-in mode defines both its engine and its size policy. */
static int write_and_sync_ini(FILE *file, int descriptor)
{
  int result = write_default_ini(file);
  int saved_error;
  if(fflush(file))
    result = -1;
  if(!result && fsync(descriptor))
    result = -1;
  saved_error = errno;
  if(fclose(file))
  {
    result = -1;
    saved_error = errno;
  }
  if(result)
    errno = saved_error;
  return result;
}

static int publish_default_ini(const char *temporary, const char *path,
  int result)
{
  int saved_error = errno;
  if(!result && link(temporary, path) && errno != EEXIST)
  {
    result = -1;
    saved_error = errno;
  }
  if(unlink(temporary) && !result)
  {
    result = -1;
    saved_error = errno;
  }
  if(result)
    errno = saved_error;
  return result;
}

/* Link publishes a complete file without replacing another process's INI. */
static int create_default_ini(const char *path)
{
  char *temporary = malloc(strlen(path) + sizeof(".tmp.XXXXXX"));
  FILE *file;
  int descriptor;
  int result;
  if(!temporary)
  {
    errno = ENOMEM;
    return -1;
  }
  sprintf(temporary, "%s.tmp.XXXXXX", path);
  descriptor = mkstemp(temporary);
  if(descriptor < 0)
  {
    free(temporary);
    return -1;
  }
  file = fdopen(descriptor, "w");
  if(!file)
  {
    int saved_error = errno;
    close(descriptor);
    unlink(temporary);
    free(temporary);
    errno = saved_error;
    return -1;
  }
  result = write_and_sync_ini(file, descriptor);
  result = publish_default_ini(temporary, path, result);
  free(temporary);
  return result;
}

static int open_override_config(const char *override,
  FILE **file, char **path)
{
  int result = open_named_config(override, file, path);
  if(result == 1)
    fprintf(stderr, "katzip: cannot open %s: %s\n",
      override, strerror(ENOENT));
  return result == 0 ? 0 : -1;
}

static int open_installed_config(const char *installed_path,
  FILE **file, char **path)
{
  int result = open_named_config(installed_path, file, path);
  if(result != 1)
    return result;
  if(create_default_ini(installed_path))
  {
    fprintf(stderr, "katzip: warning: cannot create %s: %s; "
      "using built-in compression settings\n",
      installed_path, strerror(errno));
    return 2;
  }
  return open_named_config(installed_path, file, path);
}

static int open_auto_config(const char *program,
  FILE **file, char **path)
{
  char *installed_path;
  int result = open_named_config("katzip.ini", file, path);
  if(result != 1)
    return result;
  installed_path = executable_config_path(program);
  if(!installed_path)
  {
    fprintf(stderr, "katzip: warning: katzip.ini unavailable beside "
      "the executable; using built-in compression settings\n");
    return 1;
  }
  result = open_installed_config(installed_path, file, path);
  free(installed_path);
  if(result == 2)
    return 1;
  if(result != 1)
    return result;
  fprintf(stderr, "katzip: warning: katzip.ini unavailable beside "
    "the executable; using built-in compression settings\n");
  return 1;
}

/* 0: opened INI; 1: use compiled presets; -1: invalid existing source. */
int open_config(const char *program, FILE **file, char **path)
{
  const char *override = getenv("KATZIP_INI");
  if(override && *override)
    return open_override_config(override, file, path);
  return open_auto_config(program, file, path);
}
