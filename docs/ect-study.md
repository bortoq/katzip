# ECT integration study

ECT's ZIP optimizer reads each DEFLATE entry, restores the original bytes,
compresses them with its modified Zopfli, and keeps a replacement only when it
is smaller. Its ZIP writer can also remove redundant ZIP metadata. turzip
already uses local and central headers without data descriptors or extra
fields, so there is no redundant per-entry metadata for ECT to remove.

The code bundled in `third_party/ect` is an unchanged subset of ECT commit
`e711c5ea9d725d02db546ce926a66b91b68ecb3a`: its Zopfli sources,
`LzFind.c`, `LzFind.h`, `threadLocal.h`, and the original license. The build
compiles these C and C++ files into separate temporary objects. turzip calls
`ZopfliInitOptions(..., 9, 0, 0)` and `ZopfliDeflate(...)`, matching ECT's
single-threaded level 9 for regular file data. A 3,901-byte test input
produced a 43-byte DEFLATE stream, byte-for-byte identical to `ect -9 -zip`.
The full `2.fb2` stream also matched the installed ECT executable byte for
byte: 388,654 bytes in both cases.

At level 9, turzip compresses each input with ECT's Zopfli and Turtledeflate
in one run, then writes the smaller complete DEFLATE stream to the ZIP entry.
The input is read once for the ECT candidate and once by Turtledeflate, but
the archive is written once and no ZIP recompression pass is required.
Turtledeflate's result is retained if it is smaller or if the ECT worker cannot
be started. Keeping the candidates separate also leaves the upstream ECT and
Turtledeflate source files unchanged.

Sample results before the full-file check:

| Input | Turtledeflate `-9` | ECT Zopfli `-9` | Chosen |
| --- | ---: | ---: | --- |
| First 32 KiB of `2.fb2` | 9,679 bytes | 9,707 bytes | Turtledeflate |
| First 80 KiB of `2.fb2` | 22,472 bytes | 22,570 bytes | Turtledeflate |
| Complete `2.fb2` (1,465,014 bytes) | 387,677 bytes | 388,654 bytes | Turtledeflate |

Running `ect -9 -zip` on the existing `2.turzip.zip` from the supplied files
saved 0 bytes. Its DEFLATE stream was already 387,677 bytes. This shows why a
single compressor cannot be assumed to be smaller for every input.
Running it on turzip's new full-file test archive also saved 0 bytes; both
archives were 387,799 bytes, and both passed ZIP extraction checks.
