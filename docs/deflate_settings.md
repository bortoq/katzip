# Deflate compression settings

This document describes every compression setting in `katzip.ini` and
`--full-help`. INI sections `[1]` through `[9]` correspond to command-line
levels `-1` through `-9`. Each setting uses the command-line syntax, one
option and value per line. For example:

```ini
[7]
--zopfli_numiterations 20
--zlib_after off
```

The program scans the command line for the selected level first. It parses
only the matching INI section, whose scope ends at the next section header
or end of file. Other sections are ignored. Then it applies compression
options from the command line, wherever they occur before `--`. Command-line
values override INI values. Comments beginning with `#` or `;` on their own
line are allowed, but generated INI files contain no comments.

## How katzip chooses a compressor

A `--libdeflate_level` option activates libdeflate. Any `--zopfli_*` option
activates ECT's Zopfli-derived Deflate encoder. Any `--turtledeflate_*`
option activates Turtledeflate. Missing options for an activated compressor
use compiled defaults. If several compressors are active, katzip compresses
with each and stores the smallest output. Repeating an option is allowed;
the last value wins within each source. `--zlib_after` makes zlib replace
the candidates at or above the given uncompressed file size. `off` disables
that switch. `--zlib_level` may be omitted when zlib is disabled. All engines
write ordinary Deflate streams inside ZIP.

## Algorithms

### ECT's Zopfli-derived encoder

[Zopfli](https://github.com/google/zopfli) searches for a compact Deflate
representation rather than stopping after a single LZ77 match pass. It
repeatedly optimizes the literal/match path against an estimated bit-cost
model, chooses block boundaries, and builds Huffman codes for each block.
The vendored [ECT fork](../third_party/ect/src/zopfli/zopfli.h) adds options
for more block and header search, caching, and extra cost-model refinement.
More search usually costs CPU time and memory; it does not guarantee a smaller
stream for every input. `--zopfli_*` options configure this fork, not stock Google
Zopfli. The `filter_style` preset field is currently inert for raw Deflate:
this fork stores it but no encoder path reads it.

### Turtledeflate

[Turtledeflate](https://github.com/rwillenbacher/turtledeflate) divides input
into superblocks, then repeatedly searches, splits, and merges Deflate block
boundaries. It estimates each candidate's coded bit cost. For the final
blocks it tries several LZ77 paths with different initial fixed-point
precisions, refines them, and retains the best result. These searches can be
far slower than zlib or libdeflate; compression gains vary by file. Its
upstream API header labels `i_compression_level` "unused", but the vendored
implementation **does** read it: values above 7 enable extra Huffman run
length encoding and block-cost checks.

### libdeflate and zlib

Both are conventional fast Deflate encoders. `--libdeflate_level` sets the
libdeflate effort for a candidate. `--zlib_after` and `--zlib_level` choose zlib
for large inputs. The level number is specific to each library; it is not a
common cross-encoder scale.

## Setting reference

All names are command-line options and INI settings. Boolean values use `0` for off and `1` for on.
Numeric limits are enforced by katzip where noted; a higher value is not
always better. See the [ECT options structure](../third_party/ect/src/zopfli/zopfli.h),
[Turtle API](../third_party/turtledeflate/inc/turtledeflate_api.h), and
[Turtle block splitter](../third_party/turtledeflate/lib/turtledeflate_block.c)
for the corresponding implementation.

### Selection and fast encoders

| Key | Meaning |
| --- | --- |
| `--libdeflate_level` | libdeflate effort (1..12). Used below --zlib_after; other engines compete if configured. |
| `--zlib_after` | Input size at which zlib replaces all configured engines. Bytes, KiB, MiB, GiB; off disables zlib. |
| `--zlib_level` | zlib effort (1..9). Optional when --zlib_after is off; then it has no effect. |

### ECT Zopfli-derived encoder

| Key | Meaning |
| --- | --- |
| `--zopfli_numiterations` | Number of iterative LZ77 cost-model optimization passes. More passes take longer. |
| `--zopfli_filter_style` | Reserved ECT preset value (0..3); the vendored raw Deflate encoder does not read it. |
| `--zopfli_skipdynamic` | Byte-size cutoff: do not try a dynamic-Huffman block at or below this size. |
| `--zopfli_trystatic` | Try a fixed-Huffman block below this token-count cutoff; should exceed skipdynamic. |
| `--zopfli_noblocksplit` | Do not split a block when its source data has fewer than this many bytes. |
| `--zopfli_noblocksplitlz` | Do not split a block when its LZ77 stream has fewer than this many tokens. |
| `--zopfli_num` | Number of candidate split positions sampled in each block-splitting round. |
| `--zopfli_searchext` | Huffman header search effort: 0 basic, 1 extended, 2 most extensive. |
| `--zopfli_reuse_costmodel` | 1 reuses the previous LZ77 pass cost model; 0 starts each pass afresh. |
| `--zopfli_useCache` | 1 caches matches across optimization passes; uses more memory but saves match searches. |
| `--zopfli_multithreading` | Per-block ECT worker option; katzip accepts only 0 in this build. |
| `--zopfli_isPNG` | PNG-specific ECT tuning; katzip accepts only 0 for ordinary ZIP members. |
| `--zopfli_replaceCodes` | Effort for replacing short matches with literals when this reduces bit cost. |
| `--zopfli_twice` | 1 runs block splitting a second time; 0 uses one splitting pass. |
| `--zopfli_ultra` | Additional LZ77 cost-model refinement effort (0..3). |
| `--zopfli_greed` | Match-length threshold for switching from lazy to greedy search (0..258). |
| `--zopfli_entropysplit` | 1 estimates split cost using Shannon entropy; 0 uses estimated code lengths. |
| `--zopfli_advanced` | 1 enables additional Huffman-tree and block-header optimization. |

### Turtledeflate

| Key | Meaning |
| --- | --- |
| `--turtledeflate_i_compression_level` | Turtle effort (1..9); values above 7 enable extra block-cost and Huffman RLE checks. |
| `--turtledeflate_i_maximum_block_size` | Maximum source bytes in one superblock; also determines major buffer allocations. |
| `--turtledeflate_i_maximum_subblocks` | Maximum Deflate subblocks allowed in one superblock (1..512). |
| `--turtledeflate_i_max_block_splitter_iterations` | Maximum outer passes that refine the subblock partition. |
| `--turtledeflate_i_max_internal_block_splitter_iterations` | Maximum inner split/merge refinement attempts per outer pass. |
| `--turtledeflate_i_block_splitter_num_points` | Number of sampled candidate positions in each coarse split search. |
| `--turtledeflate_i_block_splitter_center_dist` | Sample-index radius around the best candidate for the next split-search round. |
| `--turtledeflate_i_block_splitter_min_range_for_points` | Minimum search-range size for sampling; narrower ranges use direct search. |
| `--turtledeflate_b_block_splitter_push_split` | 1 tries a forced extra split when ordinary split/merge stalls; keeps it if cheaper. |
| `--turtledeflate_i_min_start_fp` | Lowest initial fixed-point precision tried by final LZ77 path tracing. |
| `--turtledeflate_i_max_start_fp` | Highest initial fixed-point precision tried by final LZ77 path tracing. |
| `--turtledeflate_i_num_start_fp` | Number of starting precisions sampled between min_start_fp and max_start_fp. |
| `--turtledeflate_i_verbose` | Turtle diagnostic detail (0 quiet, 1..5 progressively more verbose). |

## Editing notes

An option takes one value separated by whitespace. The selected section is
parsed before command-line overrides. Unselected sections are skipped. A
compressor is activated by its first option, even if its other options are
omitted. The built-in tables supply those defaults. Unknown options or
invalid values in the selected section stop the program before it creates
an archive. `--full-help` lists the options and short explanations.
