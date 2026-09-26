#include "katzip_internal.h"

int main(int argc, char **argv)
{
  OPTIONS options;
  COMPRESSION_CONFIG config;
  int result = scan_options(argc, argv, &options);
  if(result)
    return result < 0 ? 1 : 0;
  if(load_config(argv[0], options.level, &config, &options))
    return 1;
  result = parse_options(argc, argv, &options, &config);
  if(result)
    return result < 0 ? 1 : 0;
  if(validate_compression_config(&config))
  {
    fputs("katzip: invalid compressor configuration\n", stderr);
    return 1;
  }
  return run_archive(argv, &options, &config);
}
