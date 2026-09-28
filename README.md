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
| 7-Zip `-tzip -mx=9` | 0.419 | 674,217 | 672,771 | 23.94% |
| Info-ZIP + ECT `-9 -zip` | 8.212 | 674,065 | 673,015 | 23.94% |
| katzip `-6` | 0.061 | 698,170 | 697,120 | 24.80% |
| katzip `-7` | 0.709 | 669,327 | 668,277 | 23.78% |
| katzip `-8` | 1.359 | 669,165 | 668,115 | 23.77% |
| katzip `-9` | — | 668,229 | 667,179 | 23.74% |

The new level 8 row is the median of three runs on 28 September 2026. It
adds 112 bytes and takes longer than the previous level 8 preset on this
corpus (669,053 bytes, 0.649 s in the same session). The level 7 time is
from 27 September. Level 9's size is from the latest full-effort density
search; its time was not measured. The 7-Zip and ECT rows use 27 September
measurements; other rows retain the 26 September measurements. Times from
different sessions should be compared only with care.

The level 8 update was tuned on twelve FB2 files from a collected edition
of the Strugatsky brothers (26,939,192 input bytes). Each file was stored
as a separate ZIP entry and checked byte for byte after extraction. ECT's
time includes creating the initial ZIP with Info-ZIP.

| Method | Time (s) | ZIP (bytes) | DEFLATE (bytes) |
| --- | ---: | ---: | ---: |
| Former katzip `-8` | 3.770 | 7,346,443 | 7,345,383 |
| New katzip `-8` | 6.387 | 7,335,966 | 7,334,906 |
| Info-ZIP + ECT `-9 -zip` | 62.5 | 7,338,469 | 7,337,409 |

The katzip times are medians of three shuffled runs; ECT is the median of
two runs. The new preset saves 10,477 DEFLATE bytes against the former
level 8 and 2,503 bytes against ECT on these files. It uses more time than
the former level 8 but remains about ten times faster than ECT here.

These results describe the tested files and machine; they do not establish a
ranking for other data. See the [Canterbury benchmark](docs/benchmark_cantrbry.md)
and [FB2 tuning notes](docs/reseach.md#level-8-fb2-density-search) for methods
and detailed results.

## Further reading

- [Project architecture and dependencies](docs/architecture.md)
- [Compression settings and algorithms](docs/deflate_settings.md)
- [Compression research](docs/reseach.md)

katzip is licensed under [BSD-2-Clause](LICENSE). Dependency licenses and
notices are listed in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
