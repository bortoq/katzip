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
./turzip -r backup folder
./turzip -r -9 texts *.txt
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

If the output name has no extension, turzip adds `.zip`. A name with an
extension is used as given. Each later argument is a file to add. The file
keeps its relative path inside the archive. UTF-8 file names are marked as
UTF-8 in the ZIP headers.

Use `-r` to add all regular files in a directory and its subdirectories. With
`-r`, a file mask such as `*.txt` searches the current directory and all its
subdirectories. A mask such as `folder/*.txt` starts in `folder`. If the shell
expands `*.txt` first, turzip searches below the directory of each expanded
file for that file's extension. For more specific masks, pass the mask
literally so turzip can see its full pattern. Symbolic links found while
walking directories are skipped.

While compressing, turzip shows each file name and its completed percentage to
two decimal places on standard error. It refreshes the display once per second.
The file percentage advances after each compression block. During a block,
turzip also shows the current Turtledeflate pass and the percentage scanned in
that pass. Pass percentages restart at zero as the compressor tries new passes.

turzip replaces an existing output file. It returns a nonzero exit status on
error. Without `-r`, it does not accept directories. Absolute input paths and
`..` path components are not accepted. Repeated files found during a recursive
search are added only once.

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
