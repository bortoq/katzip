# katzip — Maximum Compression ZIP Archiver (DEFLATE-only)

A standalone C tool that creates smaller `.zip` files than `zip -9`, `7z -mx=9` or `kzip`.
It tries every DEFLATE encoder on each file and keeps the smallest result.
Only Store (0) and Deflate (8) are used — so archives open everywhere
(`unzip`, Python `zipfile`, 7-Zip, Windows Explorer, macOS).

If the output name has no `.zip` extension, it is added automatically.

## Build

```bash
make              # builds ./katzip (needs libz + libdeflate)
make static       # builds ./katzip_static (no shared libs)
./katzip --help
```

Build needs: `gcc`, `zlib1g-dev`, `libdeflate-dev` (optional, for even smaller files).
Zopfli source is vendored in `third_party/zopfli` and built together — no extra install.

## Use

```bash
katzip <archive.zip> <input_files...>
Example: katzip APPNOTE APPNOTE.TXT

# Examples:
katzip docs README.md src/        # creates docs.zip if name has no .zip
katzip backup.zip image.png notes.txt

# Check:
unzip -l backup.zip
7z l backup.zip
```

While compressing, overall progress is shown as `33%` etc., updated on the
same line via `\r`. When finished, the best method, its parameters and the
total compressed size without ZIP overhead are printed on the same line:

```
33%
66%
100%
Deflate Zopfli iter 200 splitmax 15 last 0 626 bytes
```

## How it works

* `src/policy.*` — skip already compressed files (jpg, mp4, zip, ...), high entropy check, size limits.
* `katzip.ini` — all contest settings. Search order: `$KATZIP_INI`,
  `./katzip.ini` (working dir), `<binary-dir>/katzip.ini`;
  every run prints the loaded file first as `config: <path>`
  (`KATZIP_INI=/dev/null` forces built-in defaults):
  `[policy]` entropy/limits/skip list, `[zlib]` on/off + levels + strategies,
  `[libdeflate]` on/off + levels, `[zopfli]` on/off + per-size iterations +
  split grid, `[enhanced]` on/off + per-trial toggles (needs `[zopfli] enabled`,
  which is the master switch for anything zopfli-powered). Missing file/keys fall
  back to built-in defaults, so deleting the file changes nothing. Example:
  disable the heavy engines with `[zopfli] enabled = off` + `[enhanced]
  enabled = off` and watch `zlib`/`libdeflate` win instead.
* `src/config.*` — tiny dependency-free INI reader with validation/clamping.
* `KATZIP_DEBUG=1` — prints every accepted trial (`[dbg] engine ... -> size`)
  to stderr; for landscape analysis like the ECT gap chase.
* `src/competitor.*` — competition: `zlib` (levels and strategies) + `libdeflate` (1..12) + Zopfli Stage 1 grid (`iter up to 1000, splitmax 0/15, last 0/1`) + Stage 2 second engine (`src/enhanced.*`: forced-fixed, nosplit/limited-split joints, parser-diversified single-block coding, per-block actual-size recoding with `recode_iters=500`, Kzip+Rezop foreign-map hybrid, merge-blocks). The smallest valid DEFLATE wins. For large texts (>1M, e.g. FB2) the default is `iter_large=15 + recode=500` — shallow grid + deep per-block recoding, which beats `ect -9 --strict -zip` at a fraction of the cost of running everything deep (`recode_iters=0` follows the grid budget; Stage 2 benchmarks vs ECT are considered closed on this setting).
* `src/archiver.*` — writes ZIP by hand (local file, central directory, EOCD, Zip64 when needed), UTF-8, CRC32, progress and final summary.
* `src/main.c` — CLI `katzip <archive.zip> <input_files...>` with auto `.zip`.
* `third_party/zopfli` — Google Zopfli, Apache 2.0.

## Test

```bash
make test   # builds katzip and runs ./tests_c.sh + ./tests_stage2.sh
```

Tests check help text, auto extension, progress, final method line, and that `katzip` beats `zip -9` and `7z -mx9`.

## Help

```
KATZip v1.0 - Deflating with extreme devotion.
Dedicated to the memory of Phil Katz (1962–2000), the father of ZIP.

Usage:   katzip <archive.zip> <input_files...>
Example: katzip APPNOTE APPNOTE.TXT
Config:  $KATZIP_INI, ./katzip.ini, or <binary-dir>/katzip.ini.
```

## License

Project code MIT. Vendored Zopfli Apache 2.0, libdeflate MIT.
