#!/bin/bash
# bench/run.sh — reproducible size regression vs zip -9 / 7z / zopfli
# Generates bench/corpus/bench.txt if missing, then prints markdown table.
# Stores baseline in bench/baseline.txt for acceptance check.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$ROOT/bench/corpus/bench.txt"
OUTDIR=$(mktemp -d)
trap 'rm -rf "$OUTDIR"' EXIT
if [ ! -f "$CORPUS" ]; then
  python3 "$ROOT/bench/corpus/gen.py"
fi
PLAIN=$(stat -c%s "$CORPUS")
echo "corpus: $CORPUS ($PLAIN B)"

# Helpers: raw payload size (deflate) and zip file size
raw_size() { python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); print(z.infolist()[0].compress_size)" "$1"; }
zip_size() { stat -c%s "$1"; }

echo ""
echo "| tool | raw | zip | time |"
echo "|---|---|---|---|"

# katzip
if [ -x "$ROOT/katzip" ]; then
  t0=$(date +%s.%N)
  KATZIP_INI=/dev/null "$ROOT/katzip" "$OUTDIR/k.zip" "$CORPUS" >/dev/null 2>&1
  t1=$(date +%s.%N)
  dt=$(python3 -c "print(f'{(float(\"$t1\")-float(\"$t0\")):.1f}s')")
  echo "| katzip | $(raw_size "$OUTDIR/k.zip") | $(zip_size "$OUTDIR/k.zip") | $dt |"
else
  echo "| katzip | — | — | — |"
fi

# zopfli --i1000 if available (via python or binary)
if command -v zopfli >/dev/null 2>&1; then
  zopfli --i1000 --deflate -c "$CORPUS" > "$OUTDIR/z.raw" 2>/dev/null || true
  if [ -f "$OUTDIR/z.raw" ]; then echo "| zopfli --i1000 | $(stat -c%s "$OUTDIR/z.raw") | — | — |"; fi
fi

# zip -9
if command -v zip >/dev/null 2>&1; then
  rm -f "$OUTDIR/s.zip"; zip -9 -j -q "$OUTDIR/s.zip" "$CORPUS"
  echo "| zip -9 | $(raw_size "$OUTDIR/s.zip") | $(zip_size "$OUTDIR/s.zip") | — |"
fi

# 7z
if command -v 7z >/dev/null 2>&1; then
  rm -f "$OUTDIR/7.zip"; 7z a -tzip -mx=9 "$OUTDIR/7.zip" "$CORPUS" >/dev/null 2>&1 || true
  if [ -f "$OUTDIR/7.zip" ]; then echo "| 7z -mx=9 | $(raw_size "$OUTDIR/7.zip") | $(zip_size "$OUTDIR/7.zip") | — |"; fi
fi

# advzip
if command -v advzip >/dev/null 2>&1; then
  cp "$OUTDIR/s.zip" "$OUTDIR/a.zip" 2>/dev/null || true
  advzip -z -4 "$OUTDIR/a.zip" >/dev/null 2>&1 || true
  if [ -f "$OUTDIR/a.zip" ]; then echo "| advzip -z -4 | $(raw_size "$OUTDIR/a.zip") | $(zip_size "$OUTDIR/a.zip") | — |"; fi
fi

# ECT if in ~/bin
if [ "$1" = "--check" ]; then
  BASE=$(cat "$ROOT/bench/baseline.txt" 2>/dev/null | grep -oE "[0-9]+" | head -1)
  if [ -n "$BASE" ]; then
    KRAW=$(python3 -c "import zipfile,sys; print(zipfile.ZipFile(sys.argv[1]).infolist()[0].compress_size)" "$OUTDIR/k.zip")
    if [ "$KRAW" -gt "$BASE" ]; then echo "FAIL: katzip raw $KRAW > baseline $BASE" >&2; exit 1; fi
    echo "baseline check: $KRAW <= $BASE OK"
  fi
  exit 0
fi

if [ -x "$HOME/bin/ect" ]; then
  cp "$CORPUS" "$OUTDIR/e.dat"
  "$HOME/bin/ect" -9 --strict -zip "$OUTDIR/e.dat" >/dev/null 2>&1 || true
  if [ -f "$OUTDIR/e.dat.zip" ] || [ -f "$OUTDIR/e.zip" ]; then
    F=$(ls "$OUTDIR"/*.zip 2>/dev/null | head -1)
    # fallback
    if [ -f "$OUTDIR/e.dat.zip" ]; then F="$OUTDIR/e.dat.zip"; fi
    echo "| ect -9 --strict | $(raw_size "$F" 2>/dev/null || echo "?") | $(zip_size "$F" 2>/dev/null || echo "?") | — |"
  fi
fi
