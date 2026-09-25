# katzip

Dedicated to the memory of Phil Katz (1962–2000), the father of ZIP.

katzip creates ZIP files from regular files. Levels 1-6 use libdeflate for fast
compression. Levels 7-9 use Turtledeflate, with ECT's Zopfli variant also
competing at level 9. minizip-ng writes the ZIP containers.
The katzip source is written in C99; ECT's code also needs a C++ compiler.

## Build

Run `make`. This creates `./katzip`. You need C and C++ compilers, `make`,
`cmake`, `git`, zlib development files, and libdeflate development files
(for example, `zlib1g-dev` and `libdeflate-dev` on Debian).
The build copies Turtledeflate into a temporary directory, applies
`patches/turtledeflate.patch` there, then removes the directory. The original
files in `third_party/turtledeflate` stay untouched. Run `make ini` to generate
an editable `katzip.ini` from the built-in defaults.
Run `make test` to build and run the archive integration tests.

## Use

```sh
./katzip archive.zip file1.txt folder/file2.txt
./katzip -9 archive.zip file1.txt folder/file2.txt
./katzip -1 archive            # add matching files from the current directory
./katzip -r backup              # also search subdirectories
./katzip -r -9 texts @*.txt @*.fb2
./katzip --help
```

`--help` and `-h` print the v1.1 usage message. An archive name is required.
When no input is given, katzip uses the `@*` mask. Without `-r`, masks search
only the current directory. With `-r`, they also search subdirectories.

An optional `-1` to `-9` flag sets the compression level before the output path.
Level 1 uses the least compression work, and level 9 uses the most. The default
is level 7. Levels 1-6 use libdeflate on files up to 64 MiB and streaming zlib
on larger files. If libdeflate cannot make a file smaller, katzip stores it
without compression. A higher level can take much longer and does not always
make a smaller archive. Level 9 runs Turtledeflate with the same settings and
1,000,000-byte input blocks as `turtledeflate --9`, alongside ECT's Zopfli
variant at ECT level 9. katzip stores whichever complete DEFLATE stream is
smaller for each file. This preserves the original file bytes and needs more
memory and temporary disk space than the other levels. ZIP headers still add
their own bytes to the archive.

For DEFLATE entries, ZIP header bits 1-2 mark levels 1-3 as Super Fast,
4-6 as Fast, 7-8 as Normal, and 9 as Maximum. These bits are informational;
the key and the compressor name are not stored in the archive. Stored entries
leave the DEFLATE hint bits clear.

The default compression levels are compiled into katzip from
`config_defaults.h`. Levels `[libdeflate-1]` through `[libdeflate-6]` select
libdeflate levels; `[turtledeflate-7]` through `[turtledeflate-9]` contain
Turtledeflate settings. Run `make ini` to write an editable `katzip.ini` from
these defaults. The generated file is ignored by Git; running `make ini` again
replaces it. katzip first checks for `katzip.ini` in the current directory,
then next to the executable, including when launched through `PATH`. If neither
exists, it warns once and uses the built-in settings for every compression
level. Set `KATZIP_INI` to explicitly use another file; a missing or invalid
explicit file is an error. A found INI with a missing or invalid selected
section is also an error.
The maximum block size is 1,000,000 bytes; unsafe INI values are rejected
before creating an archive.

If the output name has no extension, katzip adds `.zip`. A name with an
extension is used as given. Each later argument is a file to add. The file
keeps its relative path inside the archive. UTF-8 file names are marked as
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

While compressing, katzip shows each file name and one increasing progress
percentage with two decimal places on standard error. It refreshes once per
second and stays below 100%. When a file is complete, the final line shows
`100 * compressed_size / original_size` instead. The percentage is 0.00% for
an empty input because the ratio is undefined. Stored files show 100.00%.
At levels 1-6, the progress indicator follows completed input chunks; libdeflate
does not report progress inside a buffer. Turtledeflate may revisit the same
input block many times and cannot know in advance how many passes it will need.
The percentage within a block is an estimate based on work already done; it
reaches the exact block boundary when that block finishes. At level 9, progress
follows Turtledeflate while ECT runs in parallel; the final ratio appears after
both candidates finish.

katzip writes to a temporary file and replaces an existing output only after
the new archive is complete. Ctrl+C (SIGINT) or SIGTERM removes the temporary
archive immediately; any existing output remains intact. It returns a nonzero
exit status on error. Without `-r`, it does not accept directories. Absolute
input paths and `..` path components are not accepted. Repeated files found during a recursive
search are added only once.

This program writes standard ZIP files without ZIP64. An archive and each
input file must be smaller than 4 GiB. The archive can contain at most 65,535
files. Turtledeflate favors compression over speed, so levels 7-9 can take
time to process. Levels 1-6 may use roughly twice the input file size in memory
for files up to 64 MiB.

minizip-ng writes raw DEFLATE streams without recompression.
For normal files it uses 16 fewer ZIP metadata bytes per entry than the
previous container writer. `make test` checks that metadata size and validates
each archive before an existing output is replaced.

## Third-party code

The system libdeflate library provides fast raw DEFLATE compression at levels
1-6. Its MIT license notice is in `THIRD_PARTY_NOTICES`. Its sources are not
included in this repository.

`third_party/turtledeflate` provides DEFLATE compression. Its license is in
`third_party/turtledeflate/LICENSE`. The build patch adds progress reports and
fixes memory cleanup and allocation failures in the temporary copy.

`third_party/minizip-ng` writes the ZIP container. Its license is in
`third_party/minizip-ng/LICENSE`.

`third_party/ect` contains the original ECT Zopfli sources used at level 9.
The build compiles them as separate C and C++ objects without editing the
original files. ECT uses Apache-2.0; its license is in
`third_party/ect/License.txt`. An LZ4-derived fragment in ECT carries a
BSD-2-Clause notice, reproduced in `THIRD_PARTY_NOTICES`.

katzip's own code uses the BSD-2-Clause license in `LICENSE`. Third-party
notices are collected in `THIRD_PARTY_NOTICES`.
