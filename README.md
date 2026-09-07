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
./katzip --help   # Usage: katzip <archive.zip> <files...>
```

Build needs: `gcc`, `zlib1g-dev`, `libdeflate-dev` (optional, for even smaller files).
Zopfli source is vendored in `third_party/zopfli` and built together — no extra install.

## Use

```bash
katzip <archive.zip> <files...>
Example: katzip archive file.txt

# Examples:
katzip docs README.md src/        # creates docs.zip if name has no .zip
katzip backup.zip image.png notes.txt

# Check:
unzip -l backup.zip
7z l backup.zip
```

## How it works

* `src/policy.*` — skip already compressed files (jpg, mp4, zip, ...), high entropy check, size limits.
* `src/competitor.*` — competition: `zlib` (levels and strategies) + `libdeflate` (1..12) + Zopfli (max). The smallest valid DEFLATE wins. BZIP2, LZMA, ZSTD and PPMd are not used — deflate only, like kzip, for a fair comparison.
* `src/archiver.*` — writes ZIP by hand (local file, central directory, EOCD, Zip64 when needed), UTF-8, CRC32.
* `src/main.c` — very small CLI with auto `.zip` handling.
* `third_party/zopfli` — Google Zopfli, Apache 2.0.

## Test

```bash
make test   # builds katzip and runs ./tests_c.sh
```

Tests check help text, auto extension, empty files, directories, incompressible files, that `katzip` beats `zip -9` and `7z -mx=9` on text and code, and that no external programs are called.

## License

Project code MIT. Vendored Zopfli Apache 2.0, libdeflate MIT.
