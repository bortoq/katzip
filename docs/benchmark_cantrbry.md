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
The commands ran from the extraction directory.

```sh
zip -q -9 "$OUT" "${FILES[@]}"
7z a -bd -bso0 -bsp0 -tzip -mx=9 -mm=Deflate "$OUT" "${FILES[@]}"
zip -q -9 "$OUT" "${FILES[@]}"; ect -quiet -9 -zip "$OUT"
KATZIP_INI=/home/user/work/katzip/katzip.ini katzip -6 "$OUT" "${FILES[@]}"
KATZIP_INI=/home/user/work/katzip/katzip.ini katzip -7 "$OUT" "${FILES[@]}"
KATZIP_INI=/home/user/work/katzip/katzip.ini katzip -9 "$OUT" "${FILES[@]}"
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

## Results

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
