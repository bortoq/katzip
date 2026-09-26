# Using katzip

## Command line

```text
katzip [-1..-9] [-r] <archive.zip> [[@]input_files...]
```

The first argument that is neither an option nor its value is the archive
name. Later such arguments are inputs. Options can occur anywhere before
`--`; after `--`, every argument is a file name. Use `--` or a `./` prefix
for a name beginning with `-`. `-h` and `--help` show short help;
`--full-help` describes all compression options. The default level is 7.
The last level option wins. If the archive name has no extension, katzip
appends `.zip`.

Without inputs, katzip uses the mask `@*` in the current directory. An
explicit file keeps its relative path in the archive. An explicit directory
requires `-r`, which visits its regular files and subdirectories. A mask
starts with `@`, for example `@*.txt` or `@nested/*.txt`. With `-r`, a mask
without `/` matches file names at any depth; a mask with `/` matches paths
relative to each named directory. Files matching any mask are included.
Without `-r`, masks search only the current directory. On supported POSIX
systems, masks use POSIX `fnmatch` rules. The `@` prefix normally prevents
shell expansion without quotes. As with POSIX globbing, `@*` skips names
beginning with a dot. With `-r` and masks, explicit files are also filtered
by those masks. Traversal skips symbolic links and removes duplicate files.
Absolute input paths and `..` path components are rejected.

## Compression settings

The compiled presets are defined in `src/defaults.h`. On an archive run,
katzip checks for `katzip.ini` in the current directory, then beside its
executable, including when launched through `PATH`. If neither exists, it
tries to create the default INI beside the executable. If creation fails,
it warns and uses the compiled presets. It never overwrites an existing INI.
`KATZIP_INI` selects a specific INI file, which must exist.

The INI has sections `[1]` through `[9]`, each containing options in the
same form as the command line, one per line. Only the selected section is
parsed. Its scope ends at the next section or end of file. Command-line
compression options override that section. Repeated options use the last
value within each source. Unknown options, invalid values, and a missing
selected section are errors. Older INI formats must be replaced with this
option syntax. To regenerate defaults, rename the INI beside the executable
and run katzip again. A line starting with `#` or `;` is a comment.

The built-in levels 1–6 use libdeflate below 64 MiB and streaming zlib at
or above that size. Levels 7–8 use ECT's Zopfli variant. Level 9 compares
ECT with Turtledeflate and writes the smaller DEFLATE stream. Users may
configure several compressors at any level. The candidates compete on each
file; `--zlib_after` can select streaming zlib for large files instead.
Level numbers and compressor settings are independent. See
[compression settings](deflate_settings.md) for syntax and the meaning of
each option. A higher level may take much longer and need more memory without
always making a smaller file.

## Progress and output

During compression, katzip shows the current file and an estimated
archive-wide percentage with two decimal places. It refreshes about once per
second and stays below 100% until all files have been written. File names, including UTF-8 names,
are padded so the percentages align. After a file completes, its line shows `100 × compressed size / original size`; an empty
file shows 0.00%, and a stored file shows 100.00%. Parallel compressor tasks
contribute to the archive-wide estimate when they finish. libdeflate and ECT
do not report progress within a compression pass, so intermediate values are
estimates. See the [progress study](progress-study.md).

katzip writes a temporary archive, validates it, and replaces an existing
output only after completion. Ctrl+C (SIGINT) and SIGTERM remove the
incomplete temporary archive. Errors return a nonzero exit status. UTF-8 file
names are marked as UTF-8 in ZIP headers.

For DEFLATE entries, ZIP header bits 1–2 record a compression hint: levels
1–3 are Super Fast, 4–6 Fast, 7–8 Normal, and 9 Maximum. The hint does not
identify the actual encoder. Stored entries leave these hint bits clear.

## Limits

The current writer does not support ZIP64. Each input file and the output
archive must be smaller than 4 GiB; an archive can contain at most 65,535
files. The compressors also use 32-bit input sizes, so changing only the ZIP
container would not remove the file-size limit. Levels 1–6 can use roughly
twice the input size in memory for files below 64 MiB. Levels 7–8 load each
whole file into memory, and ECT may use several times its size. Competing
compressors use more memory and temporary disk space. Custom configurations
that disable the zlib size threshold can substantially increase memory use.
