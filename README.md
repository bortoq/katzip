# katzip

Dedicated to the memory of Phil Katz (1962–2000), the father of ZIP.

katzip creates ZIP files from regular files. Each compression level has its
own settings in `katzip.ini`. The built-in settings use libdeflate at levels
1-6, ECT's Zopfli variant at levels 7-8, and a comparison of ECT and
Turtledeflate at level 9. minizip-ng writes the ZIP containers.
The katzip source is written in C99; ECT's code also needs a C++ compiler.

## Build

Run `make`. This creates `./katzip`. You need C and C++ compilers, `make`,
`cmake`, `git`, zlib development files, and libdeflate development files
(for example, `zlib1g-dev` and `libdeflate-dev` on Debian).
The build copies Turtledeflate into a temporary directory, applies
`patches/turtledeflate.patch` there, then removes the directory. The original
files in `third_party/turtledeflate` stay untouched.
Run `make test` to build and run the archive integration tests.

The `src/` directory separates command-line parsing, input discovery,
configuration, progress reporting, ZIP entries, and the compression engines.
`src/archive.c` coordinates archive creation, validation, and publication.

## Use

```sh
./katzip archive.zip file1.txt folder/file2.txt
./katzip -9 archive.zip file1.txt folder/file2.txt
./katzip -1 archive            # add matching files from the current directory
./katzip -r backup              # also search subdirectories
./katzip -r -9 texts @*.txt @*.fb2
./katzip texts @*.txt -r -9  # options can follow the archive and mask
./katzip archive -- -leading-name.txt
./katzip --help
./katzip --full-help
./katzip -7 archive file.txt --zopfli_numiterations 20
```

`--help` and `-h` print the v1.1 usage message. `--full-help`
lists every compression setting and its meaning. An archive name is required.
Options may appear before or after the archive name and input files. The first
argument that is not an option is the archive name; later such arguments are
inputs. Use the existing `--` separator before a file name beginning with `-`,
or prefix that name with `./`. After `--`, all arguments are file names.
When no input is given, katzip uses the `@*` mask. Without `-r`, masks search
only the current directory. With `-r`, they also search subdirectories.

An optional `-1` to `-9` flag selects one INI section from any position.
The default is level 7. The built-in settings use libdeflate for files under
64 MiB at levels 1-6 and streaming zlib for larger files. Levels 7-8 use ECT;
level 9 compares ECT and Turtledeflate and keeps the smaller raw DEFLATE
stream. With multiple input files, katzip schedules each configured compressor
as a separate task in a bounded worker pool, then writes ZIP entries in input
order. The pool uses the available CPU count and reserves part of available
memory for concurrent encoders. The same scheduling applies when an INI
section enables a compressor competition. A higher level can take much longer
and does not always make a smaller archive. Competing compressors need more memory and temporary disk space.
ZIP headers add their own bytes to the archive.

For DEFLATE entries, ZIP header bits 1-2 mark levels 1-3 as Super Fast,
4-6 as Fast, 7-8 as Normal, and 9 as Maximum. These bits are informational;
the key and the compressor name are not stored in the archive. Stored entries
leave the DEFLATE hint bits clear.

Default compression presets are tables in `src/defaults.h`. On the
first archive run, katzip checks for `katzip.ini` in the current directory and
then next to its executable, including when launched through `PATH`. If neither
exists, katzip creates `katzip.ini` next to the executable. The working
directory receives a new INI only when it is also the executable directory.

The INI has one section per command-line level, `[1]` through `[9]`.
Each section is a list of command-line options, one per line, such as
`--zopfli_numiterations 20`. A section ends at the next section header or
end of file. The program first selects the final `-1` through `-9` option
from the command line (default `-7`), then reads only that INI section.
Options from other sections are ignored. Compression options on the command
line may appear before or after the archive and input names; they override
the selected section. `--` ends option parsing, so following names beginning
with `-` are treated as input names. `-r` may also be placed in an INI
section.

`--libdeflate_level` enables libdeflate. Any `--zopfli_*` option enables
ECT's Zopfli variant, and any `--turtledeflate_*` option enables
Turtledeflate. Unspecified settings for an enabled compressor come from the
compiled defaults. When more than one compressor is enabled, katzip runs
them on the same file and writes the shortest Deflate stream. Options may
repeat; the last value in each source wins, and command-line values take
priority. Unknown options and invalid values in the selected section are
errors. Turtledeflate's `i_compression_level` is independent of the section
number.

`--zlib_after` replaces the competing compressors with streaming zlib when
a file is at least the specified size. The value can be bytes, `KiB`, `MiB`
or `GiB`, such as `16MiB`; `off` disables the switch and `0` uses zlib for
every file. `--zlib_level` selects zlib compression from 1 to 9. It can be
omitted when zlib is disabled; the compiled level remains available if the
threshold is later changed. The built-in thresholds are 64 MiB for levels
1-6 and `off` for levels 7-9. libdeflate and ECT read a whole file into
memory, so disabling the zlib switch for large files can require a lot of
RAM. `--zopfli_multithreading` and `--zopfli_isPNG` must stay zero in this
build.

You can edit the INI; katzip will not overwrite an existing one. Older INI
formats must be replaced with the option syntax shown above. If katzip
cannot create a new INI, it warns and uses its compiled settings.
`KATZIP_INI` selects an explicit file and requires that file to exist. A
missing or invalid selected section is an error. To regenerate defaults,
rename the existing `katzip.ini` beside the executable; katzip creates a
new one on the next archive run. Invalid settings are rejected before an
archive is created. See [compression setting details](docs/deflate_settings.md).

If the output name has no extension, katzip adds `.zip`. A name with an
extension is used as given. Each later non-option argument is a file to add.
The file keeps its relative path inside the archive. UTF-8 file names are marked as
UTF-8 in the ZIP headers.

Use `-r` to visit all regular files in each named directory and its
subdirectories. Add one or more masks with an `@` prefix, such as `@*.txt` or
`@nested/*.txt`. Without `-r`, masks search the current directory only. A mask
without `/` matches file names at any visited depth; a mask with `/` matches
paths relative to each named directory. Files matching any mask are included.
Masks use POSIX `fnmatch` rules on the supported POSIX systems.
The `@` prefix normally lets a shell pass a mask without quotes. The default
`@*` follows POSIX glob rules and skips names beginning with a dot. An explicit
file argument adds only that file; with `-r` and masks, explicit files are also
filtered by those masks. Symbolic links found during traversal are skipped.

While compressing, katzip shows the current file name, archive-wide
percentage with two decimal places. The percentage refreshes every second and stays
below 100% until every file is written. After a file completes, its active
line becomes a stable line with its compression ratio:
`100 * compressed_size / original_size`. Names are padded so percentages
align, including for UTF-8 Cyrillic names. An empty file has a 0.00% ratio;
a stored file has a 100.00% ratio. In parallel mode, each completed compressor
contributes an equal share of its file's original size. libdeflate and ECT
do not report work inside a compression pass, so updates between completed
tasks are estimates.

katzip writes to a temporary file and replaces an existing output only after
the new archive is complete. Ctrl+C (SIGINT) or SIGTERM removes the temporary
archive immediately; any existing output remains intact. It returns a nonzero
exit status on error. Without `-r`, it does not accept directories. Absolute
input paths and `..` path components are not accepted. Repeated files found during a recursive
search are added only once.

This program writes standard ZIP files without ZIP64. An archive and each
input file must be smaller than 4 GiB. The archive can contain at most 65,535
files. Turtledeflate favors compression over speed, so level 9 can take
time to process. Levels 1-6 may use roughly twice the input file size in memory
for files up to 64 MiB. Levels 7-8 load each entire input file into memory;
ECT may use several times the file size while optimizing it. Custom
configurations can use libdeflate or ECT on much larger files.

minizip-ng writes raw DEFLATE streams without recompression.
For normal files it uses 16 fewer ZIP metadata bytes per entry than the
previous container writer. `make test` checks that metadata size and validates
each archive before an existing output is replaced.

## Reading the source

The program follows a short pipeline in `main`: find the requested level,
load its INI section, apply command-line options, then run the archive. `collect_entries` builds
the input list. `write_archive_entries` sends multi-file work to the bounded scheduler in
`parallel.c`. `parallel_tasks.c` prepares independent compressor jobs and
selects the smallest result for each ZIP entry.
`validate_archive` checks the exact ZIP size and reopens the temporary file.
`publish_archive` changes its permissions, flushes it, then replaces the output.

Resources belong to the function or context that creates them. `TURTLE_WORK`
owns Turtledeflate's buffers, compressor and optional temporary stream.
`ARCHIVE_OUTPUT` owns the temporary archive, ZIP reader and writer,
progress thread and signal handlers. During a comparison, each `CANDIDATE`
owns its compressed buffer or temporary stream. The corresponding cleanup
functions run on both success and failure. Third-party sources remain
unchanged.

## Third-party code

The system libdeflate library provides fast raw DEFLATE compression at levels
1-6. Its MIT license notice is in `THIRD_PARTY_NOTICES`. Its sources are not
included in this repository.

`third_party/turtledeflate` provides DEFLATE compression. Its license is in
`third_party/turtledeflate/LICENSE`. The build patch adds progress reports and
fixes memory cleanup and allocation failures in the temporary copy.

`third_party/minizip-ng` writes the ZIP container. Its license is in
`third_party/minizip-ng/LICENSE`.

`third_party/ect` contains the original ECT Zopfli sources used at levels 7-9.
The build compiles them as separate C and C++ objects without editing the
original files. ECT uses Apache-2.0; its license is in
`third_party/ect/License.txt`. An LZ4-derived fragment in ECT carries a
BSD-2-Clause notice, reproduced in `THIRD_PARTY_NOTICES`.

katzip's own code uses the BSD-2-Clause license in `LICENSE`. Third-party
notices are collected in `THIRD_PARTY_NOTICES`.
