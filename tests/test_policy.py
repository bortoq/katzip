import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from maxzip.policy import (
    is_incompressible_by_ext, entropy, is_high_entropy,
    should_use_store_only, should_skip_heavy, zopfli_allowed,
    INCOMPRESSIBLE_EXTS, ENTROPY_THRESHOLD
)

def test_incompressible_ext():
    assert is_incompressible_by_ext("photo.jpg") is True
    assert is_incompressible_by_ext("PHOTO.JPG") is True
    assert is_incompressible_by_ext("archive.zip") is True
    assert is_incompressible_by_ext("video.mp4") is True
    assert is_incompressible_by_ext("text.txt") is False
    assert is_incompressible_by_ext("") is False
    assert is_incompressible_by_ext(None) is False
    assert is_incompressible_by_ext("noext") is False

def test_entropy():
    assert entropy(b"") == 0.0
    assert abs(entropy(b"a"*1000) - 0.0) < 0.01
    # random-ish high entropy
    import os as _os
    rnd = _os.urandom(2048)
    e = entropy(rnd)
    assert e > 7.0, f"entropy {e}"
    # low entropy
    assert entropy(b"aaaaabbbbb") < 1.5
    # type guard
    try:
        entropy("not bytes")
        assert False
    except TypeError:
        pass

def test_high_entropy():
    assert is_high_entropy(b"") is False
    assert is_high_entropy(b"a"*100) is False  # <256 len => false
    assert is_high_entropy(b"a"*300) is False
    import os as _os
    rnd = _os.urandom(2048)
    assert is_high_entropy(rnd) is True
    assert is_high_entropy(b"hello world "*100) is False

def test_should_use_store_only():
    assert should_use_store_only("a.txt", b"") is True
    assert should_use_store_only("a.jpg", b"hello"*1000) is True
    assert should_use_store_only("a.txt", b"hello world "*1000) is False
    assert should_use_store_only("a.txt", None) is True
    import os as _os
    assert should_use_store_only("a.txt", _os.urandom(2048)) is True

def test_should_skip_heavy():
    assert should_skip_heavy("a.txt", b"a"*100, 2) is True  # level<3
    assert should_skip_heavy("a.txt", b"a"*100, 3) is True  # small
    assert should_skip_heavy("a.txt", b"a"*5000, 3) is False
    assert should_skip_heavy("a.txt", b"a"*5000, 4) is False
    assert should_skip_heavy("a.txt", None, 4) is True

def test_zopfli_allowed():
    assert zopfli_allowed(b"a"*5000, 4) is True
    assert zopfli_allowed(b"a"*100, 4) is False
    assert zopfli_allowed(b"a"*5000, 3) is False
    assert zopfli_allowed(b"a"*5000, 2) is False
    assert zopfli_allowed(None, 4) is False
    big = b"a" * (33*1024*1024)
    # don't actually allocate 33M in test? skip if too big
    # use guard: we test logic, but avoid allocating huge in CI
    if len(big) > 32*1024*1024:
        assert zopfli_allowed(big, 4) is False

if __name__ == "__main__":
    test_incompressible_ext()
    test_entropy()
    test_high_entropy()
    test_should_use_store_only()
    test_should_skip_heavy()
    test_zopfli_allowed()
    print("policy tests pass")
