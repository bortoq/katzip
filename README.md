# turzip

turzip creates ZIP files from regular files. It uses Turtledeflate to compress
each file. The program is written in C99.

## Build

Run `make`. This creates `./turzip`. You need a C compiler and `make`.

## Use

```sh
./turzip archive.zip file1.txt folder/file2.txt
```

The first argument is the output path. turzip uses it exactly as given, so add
`.zip` yourself if you want that extension. Each later argument is a file to
add. The file keeps its relative path inside the archive.

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
