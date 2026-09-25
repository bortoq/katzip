# Compression progress experiment

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
