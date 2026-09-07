#!/usr/bin/env python3
"""
bench.py — сравнение с Variant C (готовые бинари) как референс.
Сравнивает maxzip vs zip -9 vs 7z.
"""
import subprocess, os, tempfile, sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent.parent))
from maxzip.archiver import create_zip
import zipfile

def bench(files):
    with tempfile.TemporaryDirectory() as td:
        std = os.path.join(td, "std.zip")
        mx = os.path.join(td, "max.zip")
        # std
        with zipfile.ZipFile(std, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in files:
                z.write(f, os.path.basename(f))
        # max
        create_zip(mx, files, level=4, compat="wide")
        print(f"std {os.path.getsize(std)}  max {os.path.getsize(mx)}  ratio {os.path.getsize(mx)/os.path.getsize(std):.3f}")

if __name__ == "__main__":
    bench(sys.argv[1:] if len(sys.argv)>1 else ["maxzip/policy.py", "maxzip/competitor.py"])
