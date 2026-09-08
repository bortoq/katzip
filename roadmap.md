# Roadmap: katzip — maximum DEFLATE compression in ZIP

Goal: `katzip <archive.zip> <files...>` packs the absolute physical minimum
of bytes technically possible in DEFLATE/ZIP (methods Store/Deflate only),
readable everywhere (`unzip`, Python `zipfile`, 7-Zip, Explorer, macOS).
Speed does not matter; size is the only metric.

Current state: per-file contest `zlib (levels x strategies)` +
`libdeflate (1..12)` + `Zopfli Stage 1 grid (iter 1000/200/60/15, splitmax 15,0, last 0)` +
Stage 2 second engine (`src/enhanced.*`: forced-fixed, nosplit, split5, parser-diversified
single-block, per-block recode `recode_iters=500`, Kzip foreign-map hybrid, merge-blocks)
wins against `zip -9` / `7z -mx=9` / `ect -9 --strict -zip` on text and code (e.g. 2M FB2:
katzip 1426632 vs ect 1426753). Zopfli is vendored in `third_party/zopfli` (modified
only for progress hook, see NOTICE). Remaining is Stage 3 `deflate_polish` and optional Stage 4.

---

## 1. Hard constraint: method 8 only

Only Store (0) and Deflate (8) are emitted. BZIP2/LZMA/ZSTD/PPMd/XZ give
smaller files but break `unzip 6.0` and/or Python `zipfile`, so they are out.
Kzip plays by the same rule (deflate-only) — the fight is fair.

## 2. Encoder landscape (verified 2026-09)

| Tool | License | Verdict for katzip |
|------|---------|--------------------|
| Zopfli (Google, vendored) | Apache-2.0 | Base engine. Default 15 iters is NOT the limit. |
| ECT `-9 --strict` (fhanau) | check before vendoring | Beats Zopfli via better Huffman-cost heuristics at block joints. Candidate engine #2. |
| zenzop (imazen, Rust) | Apache-2.0 | Zopfli fork: default mode byte-identical + faster; `enhanced` mode (ECT-derived: expanded precode search, multi-strategy Huffman, parser diversification) beats ECT-9 at 60 iters. Candidate engine #2 alt. Rust = FFI or reference. |
| Kzip (Ken Silverman) | freeware, NOT open source | Cannot vendor. Idea only: its block-split points sometimes beat Zopfli's. |
| Rezop | obscure | Cannot rely on. Idea only: rescore foreign split-map through Zopfli. |
| DeflOpt / Defluff | CLOSED source | Cannot vendor. Reimplementable ideas only (see §4). |
| deft4j / JarTighten (NeRdTheNed) | permissive (open) | Usable ideas/code for a post-pass optimizer: dynamic-block header recoding, len-3 match/literal swaps. |
| Columbo (ace-dent, 2026) | open work-in-progress | Watch and evaluate: combines deflopt+defluff+deft4j methods in one pass. |
| 7-Zip Deflate band `mfb 128..258` | LGPL | +0.1–0.5% over the trio; vendor only if cheap. Deferred. |

## 3. Analysis of the proposed tips

### Tip 1 — ECT at `-9 --strict --zip`. ADOPT (as engine or reference).
Claim confirmed: ECT is not just "fast Zopfli"; at max flags its own
shortest-path heuristics beat the original on long runs. Action: benchmark
`ect -9 --strict --zip` vs katzip on our corpus; if the gap is stable,
vendor ECT's deflate core next to Zopfli (license check first) or port the
Huffman-cost correction. Effort: days. Risk: C++ core, bigger binary.

### Tip 2 — Zopfli forks with infinite iterations (`--i1000`, `blocksplitmax=0`). ADOPT NOW.
Confirmed via zenzop/pyzopfli docs: knobs are `numiterations`,
`blocksplitting`, `blocksplittinglast`, `blocksplittingmax` (0 = unlimited).
Current katzip uses only a fraction of the grid (one `blocksplittinglast`
value, two `splitmax` values, iters ≤300). Cost of the fix is near zero —
same vendored code, more trials. `--i1000` can take hours per file, so it
must be fenced by file size (see plan). zenzop `enhanced` is the maintained
form of "zopfli-patched"; prefer evaluating it over random forks.

### Tip 3 — Kzip + Rezop hybrid. ADAPT (idea only, no binaries).
Kzip cannot be vendored (license) and Rezop is not a dependable dependency.
But the mechanism is reproducible inside Zopfli: try SEVERAL split-finding
strategies per file (`blocksplittinglast` 0/1, `splitmax` 15/0, `splitting`
on/off) and keep the best stream. That is "foreign split-map + Zopfli
rescoring" without foreign code. Effort: hours. This is Stage 1 below.

### Tip 4 — DeflOpt / Defluff post-pass. REIMPLEMENT (open ideas only).
Both tools are closed source, so no vendoring. What they do is public
knowledge (encode.su analyses, `infgen` dumps): strip compliance/padding
bits at block ends, re-pack dynamic Huffman headers (precode search),
swap length-3 matches for literals where cheaper. Open implementations of
the same ideas: deft4j (permissive) and Columbo (in progress). Action:
write our own `deflate_polish()` post-pass in C operating on the produced
bitstream: re-encode each dynamic header with fuller precode search and
drop trailing pad bits; verify by inflate + byte comparison. Expected gain:
2–15 bytes per entry (matches published DeflOpt reports). Effort: 1–2 weeks.
Risk: bit-level bugs — fenced by mandatory round-trip check per entry.

## 4. Plan: "Zopfli killer" stages (DEFLATE-only, no format change)

### Stage 1 — Full Zopfli grid (hours, ~zero risk). DONE.
- Raise iteration cap to 1000; per-size budget: `<=64K -> 1000`,
  `<=256K -> 200`, `<=1M -> 60`, `>1M -> 15` (time fence).
- Try option grid per file and keep best: `blocksplittinglast` {0,1} x
  `splitmax` {15,0} (+ `splitting` off as 5th trial for tiny files).
- Keep existing zlib/libdeflate trials as floor (they sometimes win fast).
- Acceptance: `tests_c.sh` regression still green (katzip <= zip-9 AND
  <= 7z-mx9); new invariant: best stream never worse than current katzip
  on the corpus (store before/after sizes in test log).

### Stage 2 — Second engine: in-house ECT/zenzop ideas (days). DONE (in-house C, benches vs ECT closed).
- Benchmark `ect -9 --strict --zip` and zenzop-enhanced-60 on our corpus.
- Vendor the winner's core (license check!) as second contestant, or shell
  out NOTHING (standalone rule: link it in, never call external tools).
- If Rust (zenzop) is chosen: evaluate C FFI vs. re-implementing its
  three enhanced tricks (precode search, Huffman multi-strategy, parser
  diversification) on our C Zopfli. Prefer the smaller diff that closes
  the measured gap.
- Acceptance: corpus size strictly decreases vs Stage 1; `unzip -t` green. DONE: bench/corpus 27K katzip 12316 = zopfli 12316 (tie) < 7z 12384, 2M FB2 katzip 1426632 < ect 1426753.

### Stage 3 — Post-pass `deflate_polish()` (1–2 weeks).
- Re-encode dynamic-block headers (full precode search à la deft4j),
  len-3 match/literal swap trial, trailing-bit trim.
- Run AFTER the winning stream is chosen; keep result only if smaller.
- Mandatory per-entry `inflate == original` check; fuzz with `infgen`.
- Expected: single-digit bytes per entry; acceptance: no size regression
  on any corpus file, ever (polish is min-preserving by construction).

### Stage 4 — Kzip-class split seeding (days, optional).
- If Stage 1–3 still lose to Kzip on some file class: add an independent
  split-point finder (cheap greedy LZ77 pass with different cost model),
  feed its split map into Zopfli rescoring, keep best. Pure in-house code.
- Stop rule: when katzip <= Kzip on the whole corpus, freeze the grid.

## 5. What is explicitly NOT planned
- Non-deflate methods (LZMA/ZSTD/PPMd/BZIP2 in output): break `unzip`.
- BCJ/delta pre-filters: no ZIP standard for method 8, breaks decoders.
- External-tool pipeline (`advzip`/`ect` binaries at runtime): violates
  the standalone rule (`strace` must show no child `execve`).
- 7-Zip Deflate band: keep deferred unless Stages 1–2 leave >0.5% gap.

## 6. Sources
- Zopfli options (`numiterations`, `blocksplitting{,last,max}`): google/zopfli,
  fonttools/py-zopfli docs, zenzop docs (`splitmax 0 = unlimited`).
- zenzop (imazen, Apache-2.0): faster byte-identical fork + `enhanced` mode
  beating ECT-9 at 60 iters; ECT-derived optimizations listed in README.
- ECT `-9 --strict --zip` superiority reports: encode.su threads 2274/2774.
- DeflOpt/Defluff behavior and numbers: encode.su thread 1214; closed source
  (oxipng issue #763 confirms no source); open counterparts: deft4j/
  JarTighten (NeRdTheNed, permissive), Columbo (ace-dent, 2026).
- Kzip (Ken Silverman, advsys.net/ken/utils.htm): deflate-only, freeware —
  ideas only, no vendoring.
