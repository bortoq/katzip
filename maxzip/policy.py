"""
policy.py — эвристики выбора метода сжатия.
Идея из roadmap §3.4 (skip-list, энтропия, пороги).
"""
import os
import math
from typing import Optional

# Расширения которые уже сжаты — нет смысла жать (roadmap §3.4)
INCOMPRESSIBLE_EXTS = frozenset({
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif",
    ".mp4", ".mkv", ".avi", ".mov", ".mp3", ".ogg", ".flac",
    ".zip", ".gz", ".bz2", ".xz", ".7z", ".zst", ".rar",
    ".woff", ".woff2",
    ".mpg", ".mpeg", ".webm", ".opus",
})

# Маленькие файлы — не гонять тяжёлые энкодеры (roadmap §3.4, RETRY_FOR_SMALL_FILES)
SMALL_FILE_THRESHOLD = 4096  # 4KB
# Для zopfli лимит — не палить CPU на огромных файлах в insane
ZOPFLI_SIZE_LIMIT = 32 * 1024 * 1024  # 32 MB

# Энтропия > 7.85 бит/байт => почти несжимаемо
ENTROPY_THRESHOLD = 7.85


def _guard_filename(filename: Optional[str]) -> str:
    if not isinstance(filename, str):
        return ""
    return filename


def is_incompressible_by_ext(filename: str) -> bool:
    """Guard: filename может быть None/пустым."""
    if not filename or not isinstance(filename, str):
        return False
    _, ext = os.path.splitext(filename.lower())
    if not ext:
        return False
    return ext in INCOMPRESSIBLE_EXTS


def entropy(data: bytes) -> float:
    """Шеннон энтропия бит/байт. 0..8. Guard на пустые данные."""
    if not data:
        return 0.0
    if not isinstance(data, (bytes, bytearray)):
        raise TypeError("entropy expects bytes")
    if len(data) == 0:
        return 0.0
    # частотный анализ 256 байт
    freq = [0] * 256
    for b in data:
        freq[b] += 1
    ent = 0.0
    n = len(data)
    for c in freq:
        if c == 0:
            continue
        p = c / n
        ent -= p * math.log2(p)
    return ent


def is_high_entropy(data: bytes, threshold: float = ENTROPY_THRESHOLD) -> bool:
    """High entropy => compressed/encrypted content."""
    if not data:
        return False
    if not isinstance(data, (bytes, bytearray)):
        raise TypeError("is_high_entropy expects bytes")
    if len(data) < 256:
        # для мелких выборок энтропия шумит — не доверяем
        return False
    try:
        return entropy(data) > threshold
    except Exception:
        return False


def should_use_store_only(filename: str, data: bytes) -> bool:
    """
    Решает — пробовать ли вообще жать или сразу Store.
    Аналог policy из AdvanceCOMP: skip incompressible.
    """
    if data is None:
        return True
    if not isinstance(data, (bytes, bytearray)):
        return True
    if len(data) == 0:
        return True
    if is_incompressible_by_ext(filename):
        return True
    if is_high_entropy(data):
        return True
    return False


def should_skip_heavy(filename: str, data: bytes, level: int) -> bool:
    """
    Тяжёлые энкодеры (zopfli/7z) имеют смысл только на level>=3 и >4KB.
    """
    if level is None or not isinstance(level, int):
        return True
    if level < 3:
        return True
    if data is None or len(data) < SMALL_FILE_THRESHOLD:
        return True
    if len(data) > ZOPFLI_SIZE_LIMIT and level >= 4:
        # zopfli на >32MB — риск OOM/CPU, пропускаем
        return True
    return False


def zopfli_allowed(data: bytes, level: int) -> bool:
    if level < 4:
        return False
    if data is None:
        return False
    if len(data) < SMALL_FILE_THRESHOLD:
        return False
    if len(data) > ZOPFLI_SIZE_LIMIT:
        return False
    return True
