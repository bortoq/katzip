# bench — reproducible size regression

Corpus: `bench/corpus/bench.txt` — 27k English text, seed=1 (same as audit).
Generated deterministically via `bench/corpus/gen.py`.

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
