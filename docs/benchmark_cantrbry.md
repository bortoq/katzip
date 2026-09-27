# Canterbury Corpus ZIP benchmark

## Method

The input was `/home/user/Downloads/cantrbry.zip` (SHA-256
`c44b686dfc137e74aba4db0540e5d6568cb09e270ba8f8411d2f9df24f39a1a6`).
Its 11 files were extracted once and then passed separately, in the same
order, to each program. Their total uncompressed size was 2,810,784 bytes.
The archive was created anew for each run. The original ZIP container was
not used as an input file.

The test ran on Linux 6.2.0, on an AMD Ryzen 5 PRO 5650U with 12 logical
CPUs. The programs were Info-ZIP Zip 3.0, 7-Zip 26.00, ECT 0.9.5, and the
katzip build from this repository. katzip used the repository's
`katzip.ini` (SHA-256
`630304944358a404d9687343486f04dfee5e7700a13d9709d5e018d119f350c7`).
The runs were made on 26 September 2026. No file from this corpus reached
the 64 MiB zlib threshold in the default presets.

The following commands show the measured operations. `OUT` is a new ZIP
path and `FILES` is the ordered list of the 11 extracted file names.
The commands ran from the extraction directory. For the original katzip
rows, set `PRESET_INI` to a copy of the historical INI from commit
`aacf96f`. The current INI contains the updated level 7 and 8 presets.

```sh
zip -q -9 "$OUT" "${FILES[@]}"
7z a -bd -bso0 -bsp0 -tzip -mx=9 -mm=Deflate "$OUT" "${FILES[@]}"
zip -q -9 "$OUT" "${FILES[@]}"; ect -quiet -9 -zip "$OUT"
KATZIP_INI="$PRESET_INI" katzip -6 "$OUT" "${FILES[@]}"
KATZIP_INI="$PRESET_INI" katzip -7 "$OUT" "${FILES[@]}"
KATZIP_INI="$PRESET_INI" katzip -9 "$OUT" "${FILES[@]}"
```

Wall time was measured around each process invocation with Python's
`time.perf_counter_ns()`. ECT's reported time is the sum of the Info-ZIP
creation time and the ECT optimization time. It excludes extraction and
ZIP verification. Five runs were made for every method except katzip `-9`,
which was run once because that run took over seven minutes. The table
reports the median wall time for the five-run methods. No warm-up run was
excluded. Every output was opened with Python's `zipfile`, and every member
was compared byte for byte with its extracted source. All 11 members used
DEFLATE in every output; none was stored without compression.

## Original results (26 September 2026)

`DEFLATE bytes` is the sum of the 11 compressed member sizes and excludes
ZIP headers and the directory. `DEFLATE / input` is that sum divided by
2,810,784. A smaller percentage means denser compression on this corpus.
`ZIP bytes` includes all container overhead.

| Method | Runs | Wall time (s) | DEFLATE bytes | DEFLATE / input | ZIP bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Info-ZIP `zip -9` | 5 | 0.479 | 730,427 | 25.99% | 732,049 |
| 7-Zip `-tzip -mx=9` | 5 | 0.435 | 672,771 | 23.94% | 674,217 |
| Info-ZIP + ECT `-9 -zip` | 5 | 8.409 | 673,015 | 23.94% | 674,065 |
| katzip `-6` | 5 | 0.061 | 697,120 | 24.80% | 698,170 |
| katzip `-7` | 5 | 1.942 | 677,018 | 24.09% | 678,068 |
| katzip `-9` | 1 | 437.514 | 667,188 | 23.74% | 668,238 |

Individual wall times, in seconds, were:

| Method | Runs in measurement order |
| --- | --- |
| Info-ZIP `zip -9` | 0.481, 0.478, 0.479, 0.476, 0.479 |
| 7-Zip `-tzip -mx=9` | 0.435, 0.421, 0.426, 0.451, 0.450 |
| Info-ZIP + ECT `-9 -zip` | 7.900, 8.351, 8.411, 8.409, 8.468 |
| katzip `-6` | 0.061, 0.061, 0.061, 0.061, 0.064 |
| katzip `-7` | 1.829, 1.921, 1.956, 1.942, 1.965 |
| katzip `-9` | 437.514 |

The ECT process itself took 7.420 seconds in its first run; the Info-ZIP
step took 0.480 seconds. The two stages produced a 674,065-byte ZIP in
total. 7-Zip produced 244 fewer DEFLATE bytes than this ECT result, but its
ZIP was 152 bytes larger because it wrote more container metadata. katzip
`-9` produced the smallest ZIP, 5,827 bytes smaller than the ECT result,
at much greater cost in time. These observations apply to this corpus and
machine. The single katzip `-9` timing has no measured run-to-run range.

## Updated level 7 preset (27 September 2026)

The original table above records the former level 7 preset. A later local
[parameter search](reseach.md#zopfli-level-7-preset-search) selected a new
preset. The same Canterbury files were used for a new comparison. Each method
ran once to warm the machine, then five timed runs in shuffled order.
All output members were checked byte for byte against their inputs.

| Method | Median wall time | DEFLATE bytes | ZIP bytes |
| --- | ---: | ---: | ---: |
| 7-Zip `-tzip -mx=9` | 0.419 s | 672,771 | 674,217 |
| Former katzip `-7` | 1.783 s | 677,018 | 678,068 |
| Selected katzip `-7` | 0.614 s | 668,486 | 669,536 |

The selected preset used `--zopfli_numiterations 3`,
`--zopfli_trystatic 0`, `--zopfli_twice 1`,
`--zopfli_greed 48`, and `--zopfli_entropysplit 1`.
All other level 7 settings stayed as before. The DEFLATE ratio for the
selected preset was 23.78% of the 2,810,784 input bytes.

The INI used for this level 7 retest had SHA-256
`b5080d5bdd3138a701c46f84347874800eb12155aaf1bae2a12ebc983bf12b91`.
The subsequent level 8 update superseded that snapshot.

## First level 8 preset (27 September 2026)

Level 8 was first tuned after level 7. This intermediate preset used the
level 7 Zopfli settings, except `--zopfli_trystatic 300` replaced `0`.
The test used the same extracted Canterbury files. Each method ran once
to warm the system, then five timed runs in shuffled order. Every ZIP
member was checked byte for byte. ECT time includes the Info-ZIP step.

| Method | Median wall time | DEFLATE bytes | ZIP bytes |
| --- | ---: | ---: | ---: |
| Selected katzip `-7` | 0.638 s | 668,486 | 669,536 |
| Former katzip `-8` | 4.373 s | 673,015 | 674,065 |
| First tuned katzip `-8` | 0.666 s | 668,486 | 669,536 |
| Info-ZIP + ECT `-9 -zip` | 8.212 s | 673,015 | 674,065 |

The two katzip levels tied at this stage. The intermediate `katzip.ini`
had SHA-256
`26c19849399c9689e42148abc5715f1225797e2bedc928653fb2f5234fdcbcf6`.

## Refined level 8 preset (27 September 2026)

A [targeted search](reseach.md#zopfli-level-8-refinement) changed
`--zopfli_numiterations` from 3 to 4 and `--zopfli_noblocksplitlz`
from 200 to 2250. The same two data sets were tested with one warm-up
and five timed runs per method, in shuffled order. Every member was
compared with its source.

| Data set and method | Median wall time | DEFLATE bytes | ZIP bytes |
| --- | ---: | ---: | ---: |
| Canterbury: katzip `-7` | 0.629 s | 668,486 | 669,536 |
| Canterbury: first tuned `-8` | 0.628 s | 668,486 | 669,536 |
| Canterbury: refined `-8` | 0.562 s | 668,003 | 669,053 |
| Secondary: katzip `-7` | 0.203 s | 430,890 | 431,500 |
| Secondary: first tuned `-8` | 0.213 s | 430,789 | 431,399 |
| Secondary: refined `-8` | 0.222 s | 430,634 | 431,244 |

The refined `-8` ZIP is 136 bytes smaller than the 669,189-byte
ECT-optimized first level 8 ZIP on Canterbury. Running ECT `-9 -zip` on the refined
669,053-byte ZIP saved a further 201 bytes, producing 668,852 bytes in
7.450 seconds in one run. The current `katzip.ini` has SHA-256
`a167f712f0de5c0dc58adf9fd1de4b24e4b74eab063bb7ffc13a8d8573fd0a9b`.
