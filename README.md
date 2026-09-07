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
33%66%100%Deflate Zopfli iter 200 splitmax 15 last 0 626 bytes
```

## How it works

* `src/policy.*` — skip already compressed files (jpg, mp4, zip, ...), high entropy check, size limits.
* `src/competitor.*` — competition: `zlib` (levels and strategies) + `libdeflate` (1..12) + Zopfli (full grid `iter up to 1000, splitmax 0/15, last 0/1`). The smallest valid DEFLATE wins.
* `src/archiver.*` — writes ZIP by hand (local file, central directory, EOCD, Zip64 when needed), UTF-8, CRC32, progress and final summary.
* `src/main.c` — CLI `katzip <archive.zip> <input_files...>` with auto `.zip`.
* `third_party/zopfli` — Google Zopfli, Apache 2.0.

## Test

```bash
make test   # builds katzip and runs ./tests_c.sh
```

Tests check help text, auto extension, progress, final method line, and that `katzip` beats `zip -9` and `7z -mx9`.

## Help

```
KATZip v1.0 — Deflating with extreme devotion.
Dedicated to the memory of Phil Katz (1962–2000), the father of ZIP.

Usage:   katzip <archive.zip> <input_files...>
Example: katzip APPNOTE APPNOTE.TXT
```

## License

Project code MIT. Vendored Zopfli Apache 2.0, libdeflate MIT.
