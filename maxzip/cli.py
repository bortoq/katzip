"""
cli.py — CLI katzip (только самый плотный режим, без лишних аргументов)
"""
import sys
import os

from .archiver import create_zip

def _ensure_zip(name: str) -> str:
    if not name.lower().endswith(".zip"):
        return name + ".zip"
    return name

def print_usage():
    sys.stderr.write("Usage: katzip <archive.zip> <files...>\n")
    sys.stderr.write("Example: katzip archive file.txt\n")

def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    # help
    if not argv or argv[0] in ("--help", "-h", "-?"):
        print_usage()
        return 0 if argv else 1
    if len(argv) < 2:
        print_usage()
        return 1
    # no options allowed (only help)
    for a in argv:
        if a.startswith("-"):
            sys.stderr.write(f"unknown option {a}\n")
            print_usage()
            return 1
    archive = _ensure_zip(argv[0])
    files = argv[1:]
    # most dense mode: level 4, max compat
    try:
        create_zip(archive, files, level=4, compat="max", threads=1)
    except FileNotFoundError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
