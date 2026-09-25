# turzip

turzip creates ZIP files from regular files. It uses Turtledeflate to compress
each file. The program is written in C99.

## Build

Run `make`. This creates `./turzip`. You need a C compiler and `make`.
Keep `turzip.ini` next to the executable.

## Use

```sh
./turzip archive.zip file1.txt folder/file2.txt
./turzip -9 archive.zip file1.txt folder/file2.txt
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

The output path is used exactly as given, so add
`.zip` yourself if you want that extension. Each later argument is a file to
add. The file keeps its relative path inside the archive. UTF-8 file names are
marked as UTF-8 in the ZIP headers.

While compressing, turzip shows each file name, a progress bar, and the percent
of that file processed on standard error.

turzip replaces an existing output file. It returns a nonzero exit status on
error. It does not accept directories, absolute input paths, duplicate entry
names, or `..` path components.

This program writes standard ZIP files without ZIP64. An archive and each
input file must be smaller than 4 GiB. The archive can contain at most 65,535
files. Turtledeflate favors compression over speed, so large files can take
time to process.

## Third-party code

`third_party/turtledeflate` provides DEFLATE compression. Its license is in
`third_party/turtledeflate/LICENSE`. The local copy includes a fix for memory
cleanup and allocation failures.

`third_party/gz2zip/gz2zip.c` is a ZIP format reference. It is not linked into
turzip. Its license notice is at the top of that file.
