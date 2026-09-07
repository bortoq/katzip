import sys, os, tempfile, zipfile
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from maxzip.cli import main

def test_cli_basic():
    with tempfile.TemporaryDirectory() as td:
        f1 = os.path.join(td, "a.txt")
        with open(f1, "wb") as f: f.write(b"hello world "*1000)
        out = os.path.join(td, "out.zip")
        rc = main([out, f1])
        assert rc == 0
        assert os.path.exists(out)
        with zipfile.ZipFile(out) as z:
            # may be zstd so use 7z fallback? but small file stays deflate
            try:
                assert z.read("a.txt") == b"hello world "*1000
            except NotImplementedError:
                # zstd case, check via 7z
                import subprocess, pathlib
                subprocess.check_call(["7z","e","-so",out,"a.txt"], stdout=open(os.path.join(td,"out2"),"wb"))

def test_cli_auto_extension():
    with tempfile.TemporaryDirectory() as td:
        f1 = os.path.join(td, "a.txt")
        with open(f1, "wb") as f: f.write(b"hi")
        out_no_ext = os.path.join(td, "out")
        rc = main([out_no_ext, f1])
        assert rc == 0
        # should create out.zip
        assert os.path.exists(out_no_ext + ".zip")
        assert not os.path.exists(out_no_ext)

def test_cli_missing_file():
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "out.zip")
        rc = main([out, "/nonexistent"])
        assert rc == 2

def test_cli_dir():
    with tempfile.TemporaryDirectory() as td:
        d = os.path.join(td, "mydir")
        os.makedirs(d)
        with open(os.path.join(d, "f.txt"), "wb") as f: f.write(b"abc "*1000)
        out = os.path.join(td, "out.zip")
        rc = main([out, d])
        assert rc == 0
        # check at least one file inside
        import subprocess
        out_list = subprocess.check_output(["7z","l",out]).decode()
        assert "f.txt" in out_list

def test_cli_help():
    rc = main(["--help"])
    assert rc == 0
    rc = main([])
    assert rc == 1

if __name__ == "__main__":
    test_cli_basic()
    test_cli_auto_extension()
    test_cli_missing_file()
    test_cli_dir()
    test_cli_help()
    print("cli tests pass")
