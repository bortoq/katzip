# turzip

turzip creates ZIP files from regular files. Levels 1-6 use libdeflate for fast
compression. Levels 7-9 use Turtledeflate, with ECT's Zopfli variant also
competing at level 9. minizip-ng writes the ZIP containers.
The turzip source is written in C99; ECT's code also needs a C++ compiler.

## Build

Run `make`. This creates `./turzip`. You need C and C++ compilers, `make`,
`cmake`, `git`, zlib development files, and libdeflate development files
(for example, `zlib1g-dev` and `libdeflate-dev` on Debian).
The build copies Turtledeflate into a temporary directory, applies
`patches/turtledeflate.patch` there, then removes the directory. The original
files in `third_party/turtledeflate` stay untouched. Keep `turzip.ini` next to
the executable.
Run `make test` to build and run the archive integration tests.

## Use

```sh
./turzip archive.zip file1.txt folder/file2.txt
./turzip -9 archive.zip file1.txt folder/file2.txt
./turzip -r backup folder
./turzip -r -9 texts folder @*.txt @*.fb2
./turzip -r texts @*.txt
```

An optional `-1` to `-9` flag sets the compression level before the output path.
Level 1 uses the least compression work, and level 9 uses the most. The default
is level 7. Levels 1-6 use libdeflate on files up to 64 MiB and streaming zlib
on larger files. If libdeflate cannot make a file smaller, turzip stores it
without compression. A higher level can take much longer and does not always
make a smaller archive. Level 9 runs Turtledeflate with the same settings and
1,000,000-byte input blocks as `turtledeflate --9`, alongside ECT's Zopfli
variant at ECT level 9. turzip stores whichever complete DEFLATE stream is
smaller for each file. This preserves the original file bytes and needs more
memory and temporary disk space than the other levels. ZIP headers still add
their own bytes to the archive.

Compression levels come from `turzip.ini`: `[libdeflate-1]` through
`[libdeflate-6]` select libdeflate levels, and `[turtledeflate-7]` through
`[turtledeflate-9]` contain Turtledeflate settings. You can edit the values before
running turzip. If you call the program without a path, it looks for the INI file
in the current directory. Set `TURZIP_INI` to use a file at another path.
turzip stops with an error if the selected section or a required setting is
missing or invalid.
The maximum block size is 1,000,000 bytes; unsafe INI values are rejected
before creating an archive.

If the output name has no extension, turzip adds `.zip`. A name with an
extension is used as given. Each later argument is a file to add. The file
keeps its relative path inside the archive. UTF-8 file names are marked as
UTF-8 in the ZIP headers.

Use `-r` to visit all regular files in each named directory and its
subdirectories. Add one or more masks with an `@` prefix, such as `@*.txt` or
`@nested/*.txt`. A mask without `/` matches file names at any depth; a mask
with `/` matches paths relative to each named directory. Files matching any
mask are included. With masks and no directory, turzip searches the current
directory. Masks use POSIX `fnmatch` rules on the supported POSIX systems.
The `@` prefix normally lets a shell pass a mask without quotes. An explicit
file argument adds that file only. Symbolic links found during traversal are
skipped.

While compressing, turzip shows each file name and one increasing percentage
with two decimal places on standard error. It refreshes once per second.
At levels 1-6, the indicator follows completed files or input chunks; libdeflate
does not report progress inside a buffer. Turtledeflate may revisit the same input block many times and cannot know in
advance how many passes it will need. The percentage within a block is an
estimate based on work already done; it reaches the exact block boundary when
that block finishes. At level 9, the percentage follows Turtledeflate while
ECT runs in parallel; it reaches 100% after both candidates finish.

turzip writes to a temporary file and replaces an existing output only after
the new archive is complete. It returns a nonzero exit status on
error. Without `-r`, it does not accept directories. Absolute input paths and
`..` path components are not accepted. Repeated files found during a recursive
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

turzip's own code uses the BSD-2-Clause license in `LICENSE`. Third-party
notices are collected in `THIRD_PARTY_NOTICES`.
