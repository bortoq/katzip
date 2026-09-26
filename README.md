# katzip

Dedicated to the memory of Phil Katz (1962–2000), the father of ZIP.

katzip creates standard ZIP archives with DEFLATE compression. Levels 1–6
favor speed; levels 7–9 spend more time searching for smaller streams. Level 9
compares two compressors for each file and keeps the smaller result. The
program is written in C99, with a C++ dependency for ECT's compressor.

## Build

Install C and C++ compilers, `make`, `cmake`, `git`, zlib development files,
and libdeflate development files. Then run:

```sh
make
make test
```

The executable is `./katzip`. The build uses temporary copies of the bundled
third-party sources and leaves their originals unchanged.

## Use

```sh
./katzip archive.zip file1.txt folder/file2.txt
./katzip -9 archive.zip file1.txt
./katzip -r texts @*.txt @*.fb2
./katzip -1 archive
./katzip --full-help
```

The archive name is required. If no input is given, katzip uses `@*` to select
files in the current directory. `-r` searches subdirectories, and `@` marks a
mask that the shell normally passes without quotes. Options may appear before
or after file names. A missing archive extension becomes `.zip`. The default
compression level is 7.

Compression presets are read from `katzip.ini`; the program can create a
file with default presets beside its executable. See [usage and limits](docs/usage.md)
for paths, masks, configuration, and archive limits, and
[compression settings](docs/deflate_settings.md) for each available option.

## Compression benchmark

The 11 files in `cantrbry.zip` contain 2,810,784 uncompressed bytes. The table
shows wall time and size when those files were archived as separate ZIP entries
on an AMD Ryzen 5 PRO 5650U. Lower values are better. ECT's time includes
creating a ZIP with Info-ZIP before optimizing it.

| Method | Time (s) | ZIP (bytes) | DEFLATE (bytes) | DEFLATE / input |
| --- | ---: | ---: | ---: | ---: |
| Info-ZIP `zip -9` | 0.479 | 732,049 | 730,427 | 25.99% |
| 7-Zip `-tzip -mx=9` | 0.435 | 674,217 | 672,771 | 23.94% |
| Info-ZIP + ECT `-9 -zip` | 8.409 | 674,065 | 673,015 | 23.94% |
| katzip `-6` | 0.061 | 698,170 | 697,120 | 24.80% |
| katzip `-7` | 1.942 | 678,068 | 677,018 | 24.09% |
| katzip `-9` | 437.514 | 668,238 | 667,188 | 23.74% |

Times are medians of five runs, except katzip `-9`, which was run once.

The results describe this corpus and machine; they do not establish a ranking
for other files. [Benchmark method and complete results](docs/benchmark_cantrbry.md)
include commands, repetitions, and file checks.

## Further reading

- [Project architecture and dependencies](docs/architecture.md)
- [Compression settings and algorithms](docs/deflate_settings.md)
- [ECT integration study](docs/ect-study.md)
- [Progress measurement study](docs/progress-study.md)

katzip is licensed under [BSD-2-Clause](LICENSE). Dependency licenses and
notices are listed in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
