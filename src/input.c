#include "katzip_internal.h"

static char *copy_text(const char *text)
{
  char *copy = malloc(strlen(text) + 1);
  if(copy)
    strcpy(copy, text);
  return copy;
}

static const char *archive_suffix(const char *base)
{
  const char *dot = strrchr(base, '.');
  if(dot && dot > base && dot[1])
    return NULL;
  if(dot && !dot[1])
    return "zip";
  return ".zip";
}

char *archive_name(const char *argument)
{
  const char *base = strrchr(argument, '/');
  const char *suffix;
  char *name;
  base = base ? base + 1 : argument;
  if(!*base)
    return NULL;
  suffix = archive_suffix(base);
  if(!suffix)
    return copy_text(argument);
  name = malloc(strlen(argument) + strlen(suffix) + 1);
  if(!name)
    return NULL;
  strcpy(name, argument);
  strcat(name, suffix);
  return name;
}

static int inspect_input(const char *path, const char *name,
  struct stat *file_stat)
{
  if(!valid_name(name) || strlen(name) > UINT16_MAX ||
    stat(path, file_stat) || !S_ISREG(file_stat->st_mode) ||
    file_stat->st_size < 0 ||
    (uint64_t)file_stat->st_size > UINT32_MAX)
  {
    fprintf(stderr, "katzip: invalid input file: %s\n", path);
    return -1;
  }
  return 0;
}

static int same_as_archive(const ENTRY_LIST *list,
  const struct stat *file_stat)
{
  return list->archive_exists &&
    list->archive_stat.st_dev == file_stat->st_dev &&
    list->archive_stat.st_ino == file_stat->st_ino;
}

static int duplicate_entry_status(const char *message,
  const char *name, int recursive)
{
  if(recursive)
    return 1;
  fprintf(stderr, "katzip: %s: %s\n", message, name);
  return -1;
}

/* Return 1 when recursion has already discovered this file. */
static int already_added(const ENTRY_LIST *list, const char *path,
  const char *name, const struct stat *file_stat, int recursive)
{
  size_t i;
  if(same_as_archive(list, file_stat))
    return duplicate_entry_status("archive is an input file", path,
      recursive);
  for(i = 0; i < list->count; ++i)
    if(strcmp(name, list->entries[i].name) == 0)
      return duplicate_entry_status("duplicate entry", name, recursive);
  return 0;
}

static int reserve_entry(ENTRY_LIST *list)
{
  ENTRY *grown;
  size_t capacity;
  if(list->count == UINT16_MAX)
  {
    fprintf(stderr, "katzip: too many files\n");
    return -1;
  }
  if(list->count < list->capacity)
    return 0;
  capacity = list->capacity ? list->capacity * 2 : 16;
  if(capacity > UINT16_MAX)
    capacity = UINT16_MAX;
  grown = realloc(list->entries, capacity * sizeof(*grown));
  if(!grown)
  {
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  list->entries = grown;
  list->capacity = capacity;
  return 0;
}

static int append_entry(ENTRY_LIST *list, const char *path,
  const char *name, const struct stat *file_stat)
{
  ENTRY *entry = &list->entries[list->count];
  memset(entry, 0, sizeof(*entry));
  entry->path = copy_text(path);
  entry->name = copy_text(name);
  if(!entry->path || !entry->name)
  {
    free((void*)entry->path);
    free((void*)entry->name);
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  entry->name_len = (uint16_t)strlen(name);
  entry->flags = (uint16_t)(valid_utf8(name) ? MZ_ZIP_FLAG_UTF8 : 0);
  entry->mode = (uint32_t)file_stat->st_mode;
  entry->expected_size = (uint32_t)file_stat->st_size;
  entry->mtime = file_stat->st_mtime;
  ++list->count;
  return 0;
}

static int add_distinct_entry(ENTRY_LIST *list, const char *path,
  const char *name, const struct stat *file_stat, int recursive)
{
  int duplicate = already_added(list, path, name, file_stat, recursive);
  if(duplicate < 0)
    return -1;
  if(duplicate)
    return 0;
  if(reserve_entry(list))
    return -1;
  return append_entry(list, path, name, file_stat);
}

static int add_entry(ENTRY_LIST *list, const char *path, int recursive)
{
  const char *name = path;
  struct stat file_stat;
  while(name[0] == '.' && name[1] == '/')
    name += 2;
  if(inspect_input(path, name, &file_stat))
    return -1;
  return add_distinct_entry(list, path, name, &file_stat, recursive);
}

static char *join_path(const char *directory, const char *name)
{
  size_t prefix = strcmp(directory, ".") == 0 ? 0 : strlen(directory);
  size_t length = strlen(name);
  char *path = malloc(prefix + length + 2);
  if(!path)
    return NULL;
  if(prefix)
  {
    memcpy(path, directory, prefix);
    path[prefix++] = '/';
  }
  memcpy(path + prefix, name, length + 1);
  return path;
}

static int mask_matches(const char *mask, const char *relative,
  const char *basename)
{
  if(strchr(mask, '/'))
    return fnmatch(mask, relative, FNM_PATHNAME | FNM_PERIOD) == 0;
  return fnmatch(mask, basename, FNM_PERIOD) == 0;
}

static int matches_masks(const char **masks, size_t count,
  const char *relative, const char *basename)
{
  size_t i;
  if(!count)
    return 1;
  for(i = 0; i < count; ++i)
    if(mask_matches(masks[i], relative, basename))
      return 1;
  return 0;
}

static int walk_directory(ENTRY_LIST *list, const char *directory,
  const char *root, const char **masks, size_t mask_count, int recursive);

static int visit_regular_file(ENTRY_LIST *list, const char *path,
  const char *root, const char **masks, size_t mask_count,
  const char *name)
{
  const char *relative = strcmp(root, ".") == 0 ?
    path : path + strlen(root) + 1;
  if(!matches_masks(masks, mask_count, relative, name))
    return 0;
  return add_entry(list, path, 1);
}

static int visit_existing_path(ENTRY_LIST *list, const char *path,
  const char *root, const char **masks, size_t mask_count,
  int recursive, const char *name, const struct stat *file_stat)
{
  if(S_ISDIR(file_stat->st_mode) && recursive)
    return walk_directory(list, path, root, masks, mask_count, recursive);
  if(S_ISREG(file_stat->st_mode))
    return visit_regular_file(list, path, root, masks, mask_count, name);
  return 0;
}

static int visit_directory_item(ENTRY_LIST *list, const char *directory,
  const char *root, const char **masks, size_t mask_count, int recursive,
  const char *name)
{
  struct stat file_stat;
  char *path = join_path(directory, name);
  int status;
  if(!path)
    return -1;
  if(lstat(path, &file_stat))
  {
    fprintf(stderr, "katzip: cannot inspect %s: %s\n", path, strerror(errno));
    status = -1;
  }
  else
    status = visit_existing_path(list, path, root, masks, mask_count,
      recursive, name, &file_stat);
  free(path);
  return status;
}

/* Return 1 at end of directory, 0 after a child, -1 on error. */
static int visit_next_directory_item(DIR *stream, ENTRY_LIST *list,
  const char *directory, const char *root, const char **masks,
  size_t mask_count, int recursive)
{
  struct dirent *item;
  errno = 0;
  item = readdir(stream);
  if(!item)
    return errno ? -1 : 1;
  if(strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
    return 0;
  return visit_directory_item(list, directory, root, masks,
    mask_count, recursive, item->d_name);
}

static int scan_directory(DIR *stream, ENTRY_LIST *list,
  const char *directory, const char *root, const char **masks,
  size_t mask_count, int recursive)
{
  int status;
  for(;;)
  {
    status = visit_next_directory_item(stream, list, directory, root,
      masks, mask_count, recursive);
    if(status)
      return status < 0 ? -1 : 0;
  }
}

static int walk_directory(ENTRY_LIST *list, const char *directory,
  const char *root, const char **masks, size_t mask_count, int recursive)
{
  DIR *stream = opendir(directory);
  int status;
  if(!stream)
  {
    fprintf(stderr, "katzip: cannot open directory %s: %s\n",
      directory, strerror(errno));
    return -1;
  }
  status = scan_directory(stream, list, directory, root,
    masks, mask_count, recursive);
  if(closedir(stream))
    return -1;
  return status;
}

static int add_named_directory(ENTRY_LIST *list, const char *argument,
  const char **masks, size_t mask_count)
{
  char *root = copy_text(argument);
  size_t length;
  int status;
  if(!root)
    return -1;
  length = strlen(root);
  while(length > 1 && root[length - 1] == '/')
    root[--length] = 0;
  status = walk_directory(list, root, root, masks, mask_count, 1);
  free(root);
  return status;
}

static int add_masked_file(ENTRY_LIST *list, const char *argument,
  const char *name, const char **masks, size_t mask_count)
{
  const char *base = strrchr(argument, '/');
  base = base ? base + 1 : argument;
  if(!matches_masks(masks, mask_count, name, base))
    return 0;
  return add_entry(list, argument, 1);
}

static int invalid_argument(const char *argument)
{
  fprintf(stderr, "katzip: invalid input: %s\n", argument);
  return -1;
}

static int add_recursive_argument(ENTRY_LIST *list,
  const char *argument, const char *name,
  const char **masks, size_t mask_count)
{
  struct stat file_stat;
  if(stat(argument, &file_stat))
    return invalid_argument(argument);
  if(S_ISDIR(file_stat.st_mode))
    return add_named_directory(list, argument, masks, mask_count);
  if(S_ISREG(file_stat.st_mode))
    return add_masked_file(list, argument, name, masks, mask_count);
  return invalid_argument(argument);
}

static int add_argument(ENTRY_LIST *list, const char *argument,
  int recursive, const char **masks, size_t mask_count)
{
  const char *name = argument;
  while(name[0] == '.' && name[1] == '/')
    name += 2;
  if(!valid_name(name))
    return invalid_argument(argument);
  if(!recursive)
    return add_entry(list, argument, 0);
  return add_recursive_argument(list, argument, name, masks, mask_count);
}

void free_entries(ENTRY_LIST *list)
{
  size_t i;
  for(i = 0; i < list->count; ++i)
  {
    free((void*)list->entries[i].path);
    free((void*)list->entries[i].name);
  }
  free(list->entries);
}
static int gather_masks(int argc, char **argv, const OPTIONS *options,
  const char **masks, size_t *mask_count, int *source_count)
{
  int i;
  for(i = options->archive_arg + 1; i < argc; ++i)
  {
    if(argv[i][0] != '@')
    {
      ++*source_count;
      continue;
    }
    if(!argv[i][1])
    {
      fprintf(stderr, "katzip: empty file mask\n");
      return -1;
    }
    masks[(*mask_count)++] = argv[i] + 1;
  }
  return 0;
}

static int add_source_arguments(ENTRY_LIST *list, int argc, char **argv,
  const OPTIONS *options, const char **masks, size_t mask_count)
{
  int i;
  for(i = options->archive_arg + 1; i < argc; ++i)
  {
    if(argv[i][0] == '@')
      continue;
    if(add_argument(list, argv[i], options->recursive, masks, mask_count))
      return -1;
  }
  return 0;
}

static int collect_input_sources(ENTRY_LIST *list, int argc, char **argv,
  const OPTIONS *options, const char **masks,
  size_t *mask_count, int source_count)
{
  int result = 0;
  if(!source_count)
  {
    if(!*mask_count)
      masks[(*mask_count)++] = "*";
    result = walk_directory(list, ".", ".", masks, *mask_count,
      options->recursive);
  }
  if(result)
    return result;
  return add_source_arguments(list, argc, argv, options,
    masks, *mask_count);
}

int collect_entries(ENTRY_LIST *list, int argc, char **argv,
  const OPTIONS *options)
{
  const char **masks = calloc((size_t)argc, sizeof(*masks));
  size_t mask_count = 0;
  int source_count = 0;
  int result;
  if(!masks)
  {
    fprintf(stderr, "katzip: out of memory\n");
    return -1;
  }
  result = gather_masks(argc, argv, options, masks, &mask_count,
    &source_count);
  if(!result)
    result = collect_input_sources(list, argc, argv, options,
      masks, &mask_count, source_count);
  free(masks);
  if(!result && !list->count)
  {
    fprintf(stderr, "katzip: no files to archive\n");
    return -1;
  }
  return result;
}
