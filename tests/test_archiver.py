import sys, os, tempfile, zipfile, pathlib
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from maxzip.archiver import ZipWriter, create_zip, METHOD_STORE, METHOD_DEFLATE
from maxzip.competitor import METHOD_DEFLATE  # noqa: F401 (0/8 only), METHOD_LZMA, METHOD_ZSTD
import io, zlib, bz2, lzma

def test_empty_archive():
    buf = io.BytesIO()
    with ZipWriter(buf, compat="wide") as zw:
        pass
    buf.seek(0)
    # should be readable as zip with 0 entries
    with zipfile.ZipFile(buf, 'r') as z:
        assert len(z.infolist()) == 0

def test_single_file_store_deflate():
    data = b"hello world "*1000
    buf = io.BytesIO()
    with ZipWriter(buf, compat="wide") as zw:
        zw.add_file("hello.txt", data, level=2)
    buf.seek(0)
    with zipfile.ZipFile(buf, 'r') as z:
        assert z.read("hello.txt") == data
        info = z.getinfo("hello.txt")
        assert info.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)

def test_utf8_filename():
    data = b"test"
    buf = io.BytesIO()
    with ZipWriter(buf) as zw:
        zw.add_file("тест/файл.txt", data, level=2)
    buf.seek(0)
    with zipfile.ZipFile(buf, 'r') as z:
        assert z.read("тест/файл.txt") == data

def test_directory():
    buf = io.BytesIO()
    with ZipWriter(buf) as zw:
        zw.add_file("emptydir/", b"", level=2)
    buf.seek(0)
    with zipfile.ZipFile(buf, 'r') as z:
        # may not list dirs, but should be valid
        assert len(z.infolist()) >= 1

def test_roundtrip_levels():
    data = b"a"*5000 + b"b"*5000
    for lvl in [0,1,2,3,4]:
        buf = io.BytesIO()
        with ZipWriter(buf, compat="wide") as zw:
            zw.add_file("a.txt", data, level=lvl)
        buf.seek(0)
        with zipfile.ZipFile(buf, 'r') as z:
            assert z.read("a.txt") == data, f"level {lvl} failed"

def test_create_zip_filesystem():
    with tempfile.TemporaryDirectory() as td:
        # create source files
        p1 = os.path.join(td, "file1.txt")
        p2 = os.path.join(td, "subdir")
        os.makedirs(p2, exist_ok=True)
        p3 = os.path.join(p2, "file2.txt")
        with open(p1, "wb") as f: f.write(b"hello "*2000)
        with open(p3, "wb") as f: f.write(b"world "*2000)
        out = os.path.join(td, "out.zip")
        create_zip(out, [p1, p2], level=2, compat="wide")
        assert os.path.exists(out)
        with zipfile.ZipFile(out, 'r') as z:
            names = sorted(z.namelist())
            # should contain file1.txt and subdir/file2.txt with prefix
            assert any("file1.txt" in n for n in names)
            assert any("file2.txt" in n for n in names)
            for info in z.infolist():
                data = z.read(info.filename)
                # verify not empty
                assert len(data) > 0

def test_guard_invalid():
    buf = io.BytesIO()
    zw = ZipWriter(buf)
    try:
        zw.add_file("", b"hi", level=2)
        assert False
    except ValueError:
        pass
    try:
        zw.add_file(None, b"hi")
        assert False
    except TypeError:
        pass
    try:
        zw.add_file("a.txt", None)
        assert False
    except TypeError:
        pass
    try:
        ZipWriter(None)
        assert False
    except (ValueError, TypeError):
        pass
    zw.close()

def test_compat_max_bzip2_lzma():
    data = b"hello world "*2000
    for compat in ["wide", "max"]:
        buf = io.BytesIO()
        with ZipWriter(buf, compat=compat) as zw:
            zw.add_file("a.txt", data, level=3)
        buf.seek(0)
        # For wide, zipfile should always be able to read
        if compat == "wide":
            with zipfile.ZipFile(buf, 'r') as z:
                assert z.read("a.txt") == data
        else:
            # max = 0/8/12 only: zipfile (and unzip) must ALWAYS read it.
            # LZMA/ZSTD are excluded — they break `unzip -t`.
            with zipfile.ZipFile(buf, 'r') as z:
                assert z.read("a.txt") == data
                info = z.getinfo("a.txt")
                assert info.compress_type in (
                    zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED,
                ), f"forbidden method {info.compress_type} (deflate-only)"

def test_incompressible_stored():
    # jpg should be stored
    data = b"fake jpg data"*1000
    buf = io.BytesIO()
    with ZipWriter(buf, compat="wide") as zw:
        zw.add_file("photo.jpg", data, level=4)
    buf.seek(0)
    with zipfile.ZipFile(buf, 'r') as z:
        info = z.getinfo("photo.jpg")
        assert info.compress_type == zipfile.ZIP_STORED
        assert z.read("photo.jpg") == data

def test_create_zip_missing_file():
    try:
        create_zip("/tmp/out_missing.zip", ["/nonexistent/file"], level=2)
        assert False
    except FileNotFoundError:
        pass
    try:
        create_zip("", ["a.txt"])
        assert False
    except ValueError:
        pass

if __name__ == "__main__":
    test_empty_archive()
    test_single_file_store_deflate()
    test_utf8_filename()
    test_directory()
    test_roundtrip_levels()
    test_create_zip_filesystem()
    test_guard_invalid()
    test_compat_max_bzip2_lzma()
    test_incompressible_stored()
    test_create_zip_missing_file()
    print("archiver tests pass")
