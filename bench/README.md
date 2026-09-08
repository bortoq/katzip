# bench — reproducible size regression

Corpus: `bench/corpus/bench.txt` — 27 024 B English text, seed=1 (similar to audit's 27 004 B, but generated from `/usr/share/dict/words` or 34-word fallback; `bench.txt` is committed so baseline is reproducible).

Run:
```
./bench/run.sh
```
Prints markdown table: katzip raw/zip vs zip -9 / 7z -mx=9 / zopfli / advzip / ect (if installed).
The katzip line is also the regression baseline — `bench/baseline.txt` stores the
expected raw size and fails if it grows (acceptance Stage 1).

Check:
```
./bench/run.sh --check
```
