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
exceeded a 12-second screening limit. The first tuned level 8 therefore
differed from level 7 only in `--zopfli_trystatic 300` instead of `0`.
This added a fixed-Huffman
trial for small blocks. A later search refined this preset below.

Each method had one warm-up and five timed runs in shuffled order. Every ZIP
member was checked against the input. The Canterbury files were the training
set; the secondary set was the six-file set described above.

| Data set and method | Median time | DEFLATE bytes | ZIP bytes |
| --- | ---: | ---: | ---: |
| Canterbury: selected `-7` | 0.638 s | 668,486 | 669,536 |
| Canterbury: former `-8` | 4.373 s | 673,015 | 674,065 |
| Canterbury: first tuned `-8` | 0.666 s | 668,486 | 669,536 |
| Canterbury: Info-ZIP + ECT | 8.212 s | 673,015 | 674,065 |
| Secondary: selected `-7` | 0.215 s | 430,890 | 431,500 |
| Secondary: former `-8` | 0.834 s | 430,091 | 430,701 |
| Secondary: first tuned `-8` | 0.218 s | 430,789 | 431,399 |
| Secondary: Info-ZIP + ECT | 1.806 s | 430,091 | 430,701 |

The first tuned `-8` tied `-7` on Canterbury and saved 101 DEFLATE bytes on
the secondary set. It beat ECT in both size and time on Canterbury. On the
secondary set it remained faster, but ECT compressed 698 bytes more densely.
These results do not support a general claim that `-8` always beats ECT.

A competing Turtledeflate profile reduced the Canterbury stream to 668,286
bytes, but took 22.583 s for the complete archive because `ptt5` was slow.
Reducing Turtledeflate's block size did not remove that delay. That profile
was rejected under the speed objective.

## Zopfli level 8 refinement

Applying ECT `-9 -zip` to the first tuned `-8` ZIP reduced its size from
669,536 to 669,189 bytes. All 347 saved bytes came from DEFLATE streams;
ZIP overhead was unchanged. Nine of the 11 members became smaller, with
the largest changes in `sum` (171 bytes) and `plrabn12.txt` (60 bytes).
This showed a concrete target for a new parameter search.

The search compared member sizes as well as the corpus total. It swept
individual options, then paired the number of optimization passes with
the minimum LZ77 token count for block splitting. Nearby thresholds and
other options were checked on Canterbury and on the six-file secondary
set above. Candidates taking more than 8–12 seconds per run were excluded.
An independent archive of the project's C sources and README provided a
third check. No compiler or third-party source code changed.

The interaction mattered: changing `--zopfli_numiterations` from 3 to 4
alone increased Canterbury's DEFLATE size from 668,486 to 668,583 bytes.
Changing only `--zopfli_noblocksplitlz` from 200 to 2250 reduced it to
668,277 bytes. Changing both reduced it to **668,003 bytes**. Smaller
threshold changes and additional option sweeps did not improve the combined
result on both main data sets enough to justify a third changed option.
The selected level 8 preset therefore retains `--zopfli_trystatic 300`
and changes only these two further values:

```text
--zopfli_numiterations 4
--zopfli_noblocksplitlz 2250
```

Final measurements used one warm-up and five timed runs per method in
shuffled order. Every ZIP member was verified against its source. The
medians and sizes were:

| Data set and method | Time | DEFLATE bytes | ZIP bytes |
| --- | ---: | ---: | ---: |
| Canterbury: level 7 | 0.629 s | 668,486 | 669,536 |
| Canterbury: first tuned level 8 | 0.628 s | 668,486 | 669,536 |
| Canterbury: refined level 8 | 0.562 s | 668,003 | 669,053 |
| Secondary: level 7 | 0.203 s | 430,890 | 431,500 |
| Secondary: first tuned level 8 | 0.213 s | 430,789 | 431,399 |
| Secondary: refined level 8 | 0.222 s | 430,634 | 431,244 |

The refined preset saves 483 DEFLATE bytes on Canterbury and 155 on the
secondary set relative to the first tuned level 8. On a third set of C
sources and README, it saved 4 bytes (29,288 versus 29,292). It also
beats the ECT-optimized first level 8 ZIP on Canterbury by 136 bytes.
Applying ECT again to the refined ZIP still saves 201 bytes, reaching
668,852 bytes in a single 7.450-second run. Thus the search improved the
one-pass result but did not eliminate all potential for later optimization.
On the secondary set, the earlier ECT result remains 543 DEFLATE bytes
smaller than refined level 8. These data do not establish a general optimum.

## Turtledeflate level 9 speed search

The goal was to make level 9 much faster without adding much to the ZIP
size. The original Canterbury result was 668,238 bytes in 437.514 seconds
(one run). The 11 inputs are listed in the
[benchmark](benchmark_cantrbry.md). ECT's Zopfli alone took 5.221 seconds
and produced 673,015 DEFLATE bytes. Turtledeflate supplied most of the
original level 9 size advantage, but its default block searches were costly.

The search first used `kennedy.xls`, where Zopfli level 9 produced a
180,781-byte stream and the original level 9 archive held 175,406 bytes.
It screened 32 completed baseline and single-option profiles, followed
by 26 nearby combinations. Runs longer than 15 seconds on that file
were discarded. `ptt5`, the slowest
Canterbury member, then screened the speed of the promising profiles.
The final candidates were checked on all 11 files and on the separate
six-file set. Every ZIP member was compared byte for byte with its input.

The chosen profile keeps Turtledeflate effort 9 and a 1,000,000-byte
superblock. It changes the remaining search settings to:

```text
--turtledeflate_i_maximum_subblocks 8
--turtledeflate_i_max_block_splitter_iterations 1
--turtledeflate_i_max_internal_block_splitter_iterations 8
--turtledeflate_i_block_splitter_num_points 3
--turtledeflate_i_block_splitter_center_dist 1
--turtledeflate_i_block_splitter_min_range_for_points 256
--turtledeflate_b_block_splitter_push_split 0
--turtledeflate_i_min_start_fp -2
--turtledeflate_i_max_start_fp 2
--turtledeflate_i_num_start_fp 2
```

On `kennedy.xls`, reducing the minimum sampling range from 1,024 to 256
made eight internal passes about as fast as the earlier two-pass profile.
With effort 9, the chosen profile made a 175,521-byte stream in 4.694
seconds. On `ptt5` it made a 48,519-byte stream in 26.901 seconds.
An alternative with effort 7 and two outer splitting passes made a
48,519-byte `ptt5` stream, but took 50.587 seconds on that file. Its
Turtle-only Canterbury run took 52.356 seconds. Combining its member
sizes with the measured Zopfli sizes would save just 13 ZIP bytes against
the chosen competitive profile. The extra time was not justified by this
small estimated gain.

| Canterbury level 9 | Time | DEFLATE bytes | ZIP bytes |
| --- | ---: | ---: | ---: |
| Original profile | 437.514 s (one run) | 667,188 | 668,238 |
| Chosen profile | 28.648 s (median of three) | 667,751 | 668,801 |

The chosen profile was 15.3 times faster in these measurements, while its
ZIP was 563 bytes (0.084%) larger. The new size remains 252 bytes below
level 8's 669,053-byte ZIP. Its ZIP overhead is unchanged at 1,050 bytes.
On the separate six-file set it produced 430,091 DEFLATE bytes in 3.228
seconds. The original profile did not finish that set within a
180-second limit, so its compressed size there was not measured. ECT's
Zopfli settings and all upstream source files remained unchanged. The
original Canterbury level 9 timing had only one run; these results do
not establish a universal speed or compression ranking.
