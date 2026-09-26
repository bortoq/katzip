# Project architecture

## Build and tests

`make` builds the `katzip` executable. It requires C and C++ compilers,
`make`, `cmake`, `git`, zlib development files, and libdeflate development
files. On Debian, the latter two are commonly supplied by `zlib1g-dev` and
`libdeflate-dev`. ECT is built as C and C++ objects; katzip itself is C99.

The Makefile copies Turtledeflate and ECT sources to a temporary build
directory, applies the patches in `patches/`, compiles the copies, and
removes the directory. The sources in `third_party/` remain unchanged.
`make test` builds the executable and runs the archive integration tests.
`make asan` and `make tsan` build separate sanitizer executables and run the
tests against them. `make distclean` removes binaries but keeps
`katzip.ini`, which may contain user edits.

## Data flow

The `src/` directory separates command-line parsing, input discovery,
configuration, progress reporting, ZIP entries, and compression engines.
`src/archive.c` coordinates archive creation, validation, and publication.

`main` selects the requested level, loads its INI section, applies command
line options, and creates the archive. `collect_entries` builds the input
list. `write_archive_entries` sends work for multiple files to the bounded
scheduler in `parallel.c`. `parallel_tasks.c` prepares compressor jobs and
selects the smallest result for each entry. ZIP entries are written in input
order. The worker pool uses the available CPU count and limits concurrent
memory use. It also handles configurations that enable several compressors
for one file.

minizip-ng writes raw DEFLATE streams without recompressing them. It uses
local and central headers without redundant data descriptors or extra
fields; this saves 16 ZIP metadata bytes per ordinary file compared with
the previous container writer. `validate_archive` checks the complete ZIP
size and reopens the temporary archive. `publish_archive` sets permissions,
flushes the file, and replaces the destination. The integration tests check
the metadata size and validate archives before publication.

Resources belong to the function or context that creates them. `TURTLE_WORK`
owns Turtledeflate buffers, its compressor, and an optional temporary
stream. `ARCHIVE_OUTPUT` owns the temporary archive, ZIP reader and writer,
progress thread, and signal handlers. During a comparison, each `CANDIDATE`
owns its compressed buffer or temporary stream. Cleanup functions run on
both success and failure.

## Dependencies and licenses

The system libdeflate library provides fast raw DEFLATE compression at
levels 1–6. Its sources are not bundled; its MIT notice is in
`THIRD_PARTY_NOTICES`.

`third_party/turtledeflate` provides a high-effort DEFLATE compressor. Its
license is in `third_party/turtledeflate/LICENSE`. The temporary-copy
patches add progress reports, fix memory cleanup and allocation failures,
and guard a length lookup against reserved symbols 286 and 287.

`third_party/minizip-ng` writes the ZIP container. Its license is in
`third_party/minizip-ng/LICENSE`.

`third_party/ect` contains ECT's Zopfli-derived compressor. Its sources
are compiled from a temporary patched copy. The patch fixes unaligned
match reads. ECT uses Apache-2.0; its license is in
`third_party/ect/License.txt`. An LZ4-derived fragment has a BSD-2-Clause
notice, reproduced in `THIRD_PARTY_NOTICES`.

katzip's own code uses BSD-2-Clause in `LICENSE`. All dependency notices
are collected in `THIRD_PARTY_NOTICES`.
