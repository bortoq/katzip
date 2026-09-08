#!/usr/bin/env python3
"""Generate bench.txt — 27k English text from dictionary, seed=1 (audit corpus)."""
import random, pathlib
random.seed(1)
# Use /usr/share/dict/words if available, else fallback word list
try:
    words = pathlib.Path("/usr/share/dict/words").read_text().splitlines()
    words = [w for w in words if w.isalpha() and len(w) >= 2][:5000]
    if len(words) < 500:
        raise FileNotFoundError
except:
    words = ["lorem","ipsum","dolor","sit","amet","consectetur","adipiscing","elit","sed","do","eiusmod","tempor","incididunt","labore","et","dolore","magna","aliqua","enim","ad","minim","veniam","quis","nostrud","exercitation","ullamco","laboris","nisi","ut","aliquip","ex","ea","commodo","consequat"]
random.shuffle(words)
# Build 27k like audit: repeated sampling
text=[]
while len(" ".join(text)) < 27004:
    text.append(random.choice(words))
out=" ".join(text)[:27004]
path=pathlib.Path(__file__).parent / "bench.txt"
path.write_text(out)
print(f"wrote {path} {len(out)} bytes")
