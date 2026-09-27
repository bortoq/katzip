# Compression research

## ECT integration

ECT's ZIP optimizer reads each DEFLATE entry, restores the original bytes,
compresses them with its modified Zopfli, and keeps a replacement only when it
is smaller. Its ZIP writer can also remove redundant ZIP metadata. katzip
already uses local and central headers without data descriptors or extra
fields, so there is no redundant per-entry metadata for ECT to remove.

The code bundled in `third_party/ect` is an unchanged subset of ECT commit
`e711c5ea9d725d02db546ce926a66b91b68ecb3a`: its Zopfli sources,
`LzFind.c`, `LzFind.h`, `threadLocal.h`, and the original license. The build
compiles these C and C++ files into separate temporary objects. At level 9,
katzip calls
`ZopfliInitOptions(..., 9, 0, 0)` and `ZopfliDeflate(...)`, matching ECT's
single-threaded level 9 for regular file data. A 3,901-byte test input
produced a 43-byte DEFLATE stream, byte-for-byte identical to `ect -9 -zip`.
The full `2.fb2` stream also matched the installed ECT executable byte for
byte: 388,654 bytes in both cases.

At level 9, katzip compresses each input with ECT's Zopfli and Turtledeflate
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

Running `ect -9 -zip` on an earlier test archive from the supplied files
saved 0 bytes. Its DEFLATE stream was already 387,677 bytes. This shows why a
single compressor cannot be assumed to be smaller for every input.
Running it on katzip's new full-file test archive also saved 0 bytes; both
archives were 387,799 bytes, and both passed ZIP extraction checks.

## Compression progress experiment

Turtledeflate can revisit the same block many times. The number of passes is
decided while compressing. This experiment measured one completed squish pass
count and wall time for each block, using a temporary instrumented build.
The instrumented code was not added to the project.

| Level | Input | Passes per block | Time |
| --- | --- | --- | --- |
| 7 | Eight separate 4 KiB random files | 21 each | 1.67 s total |
| 7 | One 32 KiB file with the same bytes | 21 | 0.91 s |
| 9 | Three separate 4 KiB random files | 583 each | 16.71 s total |
| 9 | One 12 KiB file with the same bytes | 583 | 14.71 s |
| 9 | A compressible 12 KiB sample | 3764 | about 7 s |

Two of the 4 KiB files took 2.34 s and 12.11 s despite having the same size
and pass count. Repeating those files gave 2.53 s and 13.07 s. The time per
pass depends strongly on the data and on work outside the measured squish loop.

Pass count, input size, and compression level did not give a stable estimate of
remaining time across these cases. The current percentage remains an estimate;
the experiment did not justify changing its formula. An exact work percentage
would require a compressor algorithm with a known amount of planned work.

## Zopfli level 7 preset search

The aim was to reduce level 7's time and DEFLATE size relative to its former
preset, with 7-Zip `-tzip -mx=9 -mm=Deflate` as a reference. The search used
the 11 files from `cantrbry.zip` described in the
[benchmark](benchmark_cantrbry.md). It tested 274 parameter combinations,
starting with individual options and then combinations near the best results.
Six configurations exceeded a 12-second screening limit. Successful ZIPs
were checked byte for byte. Final results are medians of five timed runs,
with one warm-up run and shuffled method order.

The selected preset changes these five options; the other level 7 settings
retain their former values:

```text
--zopfli_numiterations 3
--zopfli_trystatic 0
--zopfli_twice 1
--zopfli_greed 48
--zopfli_entropysplit 1
```

| Data set and method | Time | DEFLATE bytes | ZIP bytes |
| --- | ---: | ---: | ---: |
| Canterbury: 7-Zip | 0.419 s | 672,771 | 674,217 |
| Canterbury: former `-7` | 1.783 s | 677,018 | 678,068 |
| Canterbury: selected `-7` | 0.614 s | 668,486 | 669,536 |
| Holdout: 7-Zip | 0.263 s | 432,944 | 433,770 |
| Holdout: former `-7` | 0.255 s | 430,199 | 430,809 |
| Holdout: selected `-7` | 0.204 s | 430,890 | 431,500 |

On Canterbury, the selected preset saved 8,532 DEFLATE bytes and 1.169 s
against the former `-7`. It produced 4,285 fewer DEFLATE bytes than 7-Zip,
but took 0.195 s longer. The holdout was not used for tuning: it contained
an FB2 document, the katzip executable, three source files, and 100,000
pseudorandom bytes. Its uncompressed total was 1,003,694 bytes. On that set,
the selected preset was faster than both alternatives and smaller than 7-Zip,
but 691 bytes larger than the former `-7`. The results do not establish a
global optimum or a uniform improvement for all data.

`--zopfli_useCache 0` caused an assertion in the vendored ECT code and was
excluded. The former level 8 produced 673,015 DEFLATE bytes on Canterbury,
more than the selected level 7. Its follow-up search is described below.
Level 9 produced 667,188 bytes in the original benchmark, taking 437.514 s.

## Zopfli level 8 preset search

The aim was to make `-8` at least as dense as the selected `-7` on Canterbury
and denser than Info-ZIP followed by ECT `-9 -zip`, without approaching the
long ECT run time. Starting from the new level 7 preset, 266 distinct Zopfli
configurations were measured. A sweep of `greed`, changes to iteration and
block-splitting settings, and random combinations found none smaller than
668,486 DEFLATE bytes on Canterbury. Several higher-effort configurations
exceeded a 12-second screening limit. The selected level 8 therefore differs
from level 7 only in `--zopfli_trystatic 300` instead of `0`. This adds a
fixed-Huffman trial for small blocks.

Each method had one warm-up and five timed runs in shuffled order. Every ZIP
member was checked against the input. The Canterbury files were the training
set; the secondary set was the six-file set described above.

| Data set and method | Median time | DEFLATE bytes | ZIP bytes |
| --- | ---: | ---: | ---: |
| Canterbury: selected `-7` | 0.638 s | 668,486 | 669,536 |
| Canterbury: former `-8` | 4.373 s | 673,015 | 674,065 |
| Canterbury: selected `-8` | 0.666 s | 668,486 | 669,536 |
| Canterbury: Info-ZIP + ECT | 8.212 s | 673,015 | 674,065 |
| Secondary: selected `-7` | 0.215 s | 430,890 | 431,500 |
| Secondary: former `-8` | 0.834 s | 430,091 | 430,701 |
| Secondary: selected `-8` | 0.218 s | 430,789 | 431,399 |
| Secondary: Info-ZIP + ECT | 1.806 s | 430,091 | 430,701 |

The selected `-8` tied `-7` on Canterbury and saved 101 DEFLATE bytes on
the secondary set. It beat ECT in both size and time on Canterbury. On the
secondary set it remained faster, but ECT compressed 698 bytes more densely.
These results do not support a general claim that `-8` always beats ECT.

A competing Turtledeflate profile reduced the Canterbury stream to 668,286
bytes, but took 22.583 s for the complete archive because `ptt5` was slow.
Reducing Turtledeflate's block size did not remove that delay. That profile
was rejected under the speed objective.
