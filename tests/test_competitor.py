import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from maxzip.competitor import (
    compress_buffer, decompress_buffer,
    METHOD_STORE, METHOD_DEFLATE,
    has_zopfli, has_zstd
)
import zlib, bz2, lzma

def test_store_empty():
    c, m = compress_buffer(b"", "a.txt", level=2, compat="wide")
    assert m == METHOD_STORE
    assert c == b""

def test_store_incompressible():
    # jpg extension -> store
    data = b"hello world "*500
    c, m = compress_buffer(data, "photo.jpg", level=4, compat="max")
    assert m == METHOD_STORE
    assert c == data

def test_deflate_compressible():
    data = b"hello world "*2000  # ~24KB
    c, m = compress_buffer(data, "a.txt", level=2, compat="wide")
    assert m == METHOD_DEFLATE
    assert len(c) < len(data)
    # roundtrip
    assert decompress_buffer(c, m, len(data)) == data

def test_levels():
    data = b"aaaaabbbbbccccc"*2000
    for lvl in [0,1,2,3,4]:
        c, m = compress_buffer(data, "a.txt", level=lvl, compat="wide")
        assert m in (METHOD_STORE, METHOD_DEFLATE)
        if len(c) < len(data):
            assert decompress_buffer(c, m, len(data)) == data

def test_level0_fast():
    data = b"hello "*1000
    c, m = compress_buffer(data, "a.txt", level=0, compat="wide")
    assert m in (METHOD_STORE, METHOD_DEFLATE)

def test_guard_invalid():
    try:
        compress_buffer(None, "a.txt")
        assert False
    except TypeError:
        pass
    try:
        compress_buffer("not bytes", "a.txt")
        assert False
    except TypeError:
        pass
    # invalid level clamped
    c, m = compress_buffer(b"hello world "*100, "a.txt", level=999, compat="wide")
    assert m in (METHOD_STORE, METHOD_DEFLATE)
    c, m = compress_buffer(b"hello"*100, "a.txt", level=2, compat="invalid")
    assert m in (METHOD_STORE, METHOD_DEFLATE)

def test_bzip2_lzma_wide_vs_max():
    data = b"hello world "*2000
    c_wide, m_wide = compress_buffer(data, "a.txt", level=3, compat="wide")
    assert m_wide in (METHOD_STORE, METHOD_DEFLATE)
    c_max, m_max = compress_buffer(data, "a.txt", level=3, compat="max")
    # DEFLATE-only: must stay 0/8 (fair fight vs kzip)
    assert m_max in (METHOD_STORE, METHOD_DEFLATE)
    if m_max != METHOD_STORE:
        assert decompress_buffer(c_max, m_max, len(data)) == data

def test_zopfli_if_available():
    if not has_zopfli():
        print("skip zopfli (not installed)")
        return
    data = b"hello world "*1000  # 12KB
    c, m = compress_buffer(data, "a.txt", level=4, compat="wide")
    # zopfli result should decompress
    assert decompress_buffer(c, m, len(data)) == data
    # zopfli should be at least as good as zlib 9
    cz = zlib.compressobj(9, zlib.DEFLATED, -15)
    zlib_raw = cz.compress(data)+cz.flush()
    assert len(c) <= len(zlib_raw) + 5  # allow small overhead

def test_high_entropy_store():
    import os as _os
    rnd = _os.urandom(2048)
    c, m = compress_buffer(rnd, "a.bin", level=4, compat="max")
    # high entropy often store, but if compressed bigger than original -> store
    if m != METHOD_STORE:
        assert decompress_buffer(c, m, len(rnd)) == rnd
    else:
        assert c == rnd

def test_small_file_skip_heavy():
    data = b"hi"
    c, m = compress_buffer(data, "a.txt", level=4, compat="wide")
    # small file should be store or deflate via zlib, not heavy
    assert m in (METHOD_STORE, METHOD_DEFLATE)
    assert decompress_buffer(c, m, len(data)) == data if m != METHOD_STORE else c == data

def test_decompress_store():
    data = b"hello"
    assert decompress_buffer(data, METHOD_STORE, 5) == data
    assert decompress_buffer(b"", METHOD_STORE, 0) == b""

def test_zstd_never_emitted():
    # LZMA/ZSTD must never be emitted (unzip-incompatible), even if libs present
    for name in ["a.txt", "code.c", "data.bin"]:
        data = (b"hello world hello " * 2000) + name.encode()
        for lvl in [3, 4]:
            c, m = compress_buffer(data, name, level=lvl, compat="max")
            assert m in (METHOD_STORE, METHOD_DEFLATE), f"forbidden method {m}"
            if m != METHOD_STORE:
                assert decompress_buffer(c, m, len(data)) == data

if __name__ == "__main__":
    test_store_empty()
    test_store_incompressible()
    test_deflate_compressible()
    test_levels()
    test_level0_fast()
    test_guard_invalid()
    test_bzip2_lzma_wide_vs_max()
    test_zopfli_if_available()
    test_high_entropy_store()
    test_small_file_skip_heavy()
    test_decompress_store()
    test_zstd_never_emitted()
    print("competitor tests pass")
