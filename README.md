# turzip

turzip creates ZIP files from regular files. It uses Turtledeflate to compress
each file and minizip-ng to write ZIP containers. The program is written in C99.

## Build

Run `make`. This creates `./turzip`. You need a C compiler, `make`, `cmake`,
`git`, and zlib development files.
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
is level 7. A higher level can take much longer and does not always make a
smaller archive. Level 9 uses the same compression settings and 1,000,000-byte
input blocks as `turtledeflate --9`. It can use much more memory than the other
levels. ZIP headers still add their own bytes to the archive.

All Turtledeflate settings come from `turzip.ini`. Each level has a section from
`[turtledeflate-1]` through `[turtledeflate-9]`. You can edit the values before
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
Turtledeflate may revisit the same input block many times and cannot know in
advance how many passes it will need. The percentage within a block is an
estimate based on work already done; it reaches the exact block boundary when
that block finishes.

turzip writes to a temporary file and replaces an existing output only after
the new archive is complete. It returns a nonzero exit status on
error. Without `-r`, it does not accept directories. Absolute input paths and
`..` path components are not accepted. Repeated files found during a recursive
search are added only once.

This program writes standard ZIP files without ZIP64. An archive and each
input file must be smaller than 4 GiB. The archive can contain at most 65,535
files. Turtledeflate favors compression over speed, so large files can take
time to process.

minizip-ng writes Turtledeflate's raw DEFLATE stream without recompression.
For normal files it uses 16 fewer ZIP metadata bytes per entry than the
previous container writer. `make test` checks that metadata size and validates
each archive before an existing output is replaced.

## Third-party code

`third_party/turtledeflate` provides DEFLATE compression. Its license is in
`third_party/turtledeflate/LICENSE`. The build patch adds progress reports and
fixes memory cleanup and allocation failures in the temporary copy.

`third_party/minizip-ng` writes the ZIP container. Its license is in
`third_party/minizip-ng/LICENSE`.

turzip's own code uses the BSD-2-Clause license in `LICENSE`. Third-party
notices are collected in `THIRD_PARTY_NOTICES`.
