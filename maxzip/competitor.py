"""
competitor.py — конкурс энкодеров (ядро max-сжатия).
Идея из roadmap §5.3: перебор zlib-strategies + libdeflate + bzip2 + lzma + zopfli + zstd.
Все функции — pure, без глобалок, с guard clauses и проверкой каждого syscall.
"""
import zlib
import bz2
import lzma
import struct
from typing import Tuple, Optional

# Опциональные зависимости
try:
    import zopfli.zopfli as _zopfli_mod
    _HAS_ZOPFLI = True
except Exception:
    _zopfli_mod = None
    _HAS_ZOPFLI = False

try:
    import zstandard as _zstd_mod
    _HAS_ZSTD = True
except Exception:
    _zstd_mod = None
    _HAS_ZSTD = False

from . import policy as _policy

# Method IDs per APPNOTE 6.3.10
METHOD_STORE = 0
METHOD_DEFLATE = 8
METHOD_BZIP2 = 12
METHOD_LZMA = 14
METHOD_ZSTD = 93
METHOD_XZ = 95
METHOD_PPMD = 98

# zlib strategies
_STRATEGIES = [
    zlib.Z_DEFAULT_STRATEGY,
    zlib.Z_FILTERED,
    zlib.Z_HUFFMAN_ONLY,
    # Z_RLE = 3
    3,  # RLE
    zlib.Z_FIXED,
]

def has_zopfli() -> bool:
    return _HAS_ZOPFLI

def has_zstd() -> bool:
    return _HAS_ZSTD

def _deflate_raw_zlib(data: bytes, level: int, strategy: int) -> Optional[bytes]:
    """Сжать raw deflate (wbits=-15). Guard + error handling."""
    if data is None or not isinstance(data, (bytes, bytearray)):
        return None
    if not isinstance(level, int) or level < 0 or level > 9:
        return None
    if not isinstance(strategy, int):
        return None
    try:
        comp = zlib.compressobj(level, zlib.DEFLATED, -15, memLevel=9, strategy=strategy)
        out = comp.compress(data)
        out += comp.flush(zlib.Z_FINISH)
        # verify round-trip in case of bug (roadmap §5.3)
        try:
            if zlib.decompress(out, -15) != data:
                return None
        except Exception:
            return None
        return out
    except Exception:
        return None

def _deflate_zopfli(data: bytes, iterations: int = 15) -> Optional[bytes]:
    """Zopfli -> raw deflate. Возвращает None если нет либы или ошибка."""
    if not _HAS_ZOPFLI:
        return None
    if data is None or not isinstance(data, (bytes, bytearray)):
        return None
    if len(data) == 0:
        # zopfli на пустом часто даёт 2 байта, но мы вернём пустой deflate? лучше Store
        return b"\x03\x00"  # empty stored block
    if not isinstance(iterations, int) or iterations < 1 or iterations > 100:
        iterations = 15
    # ограничим iterations разумно
    if iterations > 60:
        iterations = 60
    try:
        # zopfli.zopfli.compress возвращает zlib-container, нужно снять header/trailer
        # Используем fewer iterations для скорости на тестах
        zcontainer = _zopfli_mod.compress(
            data,
            verbose=0,
            numiterations=iterations,
            blocksplitting=1,
            blocksplittingmax=15,
        )
        if zcontainer is None or len(zcontainer) < 6:
            return None
        # zlib header 2 bytes + deflate + adler 4 bytes
        raw = zcontainer[2:-4]
        # verify
        try:
            if zlib.decompress(raw, -15) != data:
                return None
        except Exception:
            return None
        return raw
    except Exception:
        return None

def _compress_bzip2(data: bytes, level: int = 9) -> Optional[bytes]:
    if data is None or not isinstance(data, (bytes, bytearray)):
        return None
    if not isinstance(level, int) or level < 1 or level > 9:
        level = 9
    try:
        out = bz2.compress(data, compresslevel=level)
        # verify
        try:
            if bz2.decompress(out) != data:
                return None
        except Exception:
            return None
        return out
    except Exception:
        return None

def _compress_lzma(data: bytes, preset: int = 9) -> Optional[bytes]:
    """
    LZMA для ZIP method 14: формат RAW + props header как в zipfile.LZMACompressor.
    Возвращает байты уже с props-префиксом (что пишет ZipFile).
    """
    if data is None or not isinstance(data, (bytes, bytearray)):
        return None
    if not isinstance(preset, int) or preset < 0 or preset > 9:
        preset = 9
    try:
        # Используем логику CPython zipfile.LZMACompressor
        # props = encode_filter_properties(LZMA1)
        props = lzma._encode_filter_properties({"id": lzma.FILTER_LZMA1})
        comp = lzma.LZMACompressor(lzma.FORMAT_RAW, filters=[
            lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)
        ])
        header = struct.pack("<BBH", 9, 4, len(props)) + props
        out = header + comp.compress(data) + comp.flush()
        # verify: декодировать обратно RAW
        try:
            # декодируем для проверки
            dec = lzma.decompress(out[4+len(props)-4:], format=lzma.FORMAT_RAW, filters=[
                lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)
            ])
            # Но header содержит несколько байт - проще проверить через полный RAW без header
            # Пропустим строгую проверку, проверим что lzma compress standalone совпадает по декомпрессии через xz? 
            # Так как verification сложная для RAW, просто проверим что не пусто и размер разумный
            if len(out) == 0:
                return None
        except Exception:
            # если проверка упала, но сжатие прошло — считаем ок
            pass
        # дополнительная проверка через decompress с props
        try:
            # Используем lzma decompress с теми же props
            # Для ZIP нужно именно RAW, так что пробуем
            _props_len = struct.unpack("<H", out[2:4])[0]
            _props = out[4:4+_props_len]
            _data = out[4+_props_len:]
            dec2 = lzma.decompress(_data, format=lzma.FORMAT_RAW, filters=[
                lzma._decode_filter_properties(lzma.FILTER_LZMA1, _props)
            ])
            if dec2 != data:
                return None
        except Exception:
            return None
        return out
    except Exception:
        return None

def _compress_zstd(data: bytes, level: int = 19) -> Optional[bytes]:
    if not _HAS_ZSTD:
        return None
    if data is None or not isinstance(data, (bytes, bytearray)):
        return None
    if not isinstance(level, int) or level < 1 or level > 22:
        level = 19
    try:
        cctx = _zstd_mod.ZstdCompressor(level=level)
        out = cctx.compress(data)
        # verify
        try:
            dctx = _zstd_mod.ZstdDecompressor()
            if dctx.decompress(out) != data:
                return None
        except Exception:
            return None
        return out
    except Exception:
        return None

def _compress_store(data: bytes) -> bytes:
    if data is None:
        return b""
    if not isinstance(data, (bytes, bytearray)):
        raise TypeError("store expects bytes")
    return bytes(data)

def compress_buffer(data: bytes, filename: str = "", level: int = 2, compat: str = "wide") -> Tuple[bytes, int]:
    """
    Главный конкурс. Возвращает (compressed_bytes, method_id).
    - level 0..4 как в roadmap §5.2
    - compat игнорируется: всегда только 0/8 (Deflate-only, как kzip)
    Guard clauses в начале, без глобалок, все syscalls проверены.
    """
    # guard
    if data is None:
        raise TypeError("data must be bytes")
    if not isinstance(data, (bytes, bytearray)):
        raise TypeError("data must be bytes")
    if not isinstance(level, int):
        level = 2
    if level < 0:
        level = 0
    if level > 4:
        level = 4
    if compat not in ("wide", "max"):
        compat = "wide"
    if filename is None:
        filename = ""
    if not isinstance(filename, str):
        filename = str(filename)

    data = bytes(data)  # ensure bytes

    # быстрый путь Store
    if len(data) == 0:
        return b"", METHOD_STORE
    if _policy.should_use_store_only(filename, data):
        # всё равно проверим deflate level1 на случай если entropy ложноположительно?
        # но для high-entropy Store всегда wins, экономим CPU
        return bytes(data), METHOD_STORE

    best_data = bytes(data)
    best_method = METHOD_STORE
    best_len = len(data)

    # helpers
    def _consider(candidate: Optional[bytes], method: int):
        nonlocal best_data, best_method, best_len
        if candidate is None:
            return
        if not isinstance(candidate, (bytes, bytearray)):
            return
        clen = len(candidate)
        if clen < best_len:
            best_len = clen
            best_data = bytes(candidate)
            best_method = method

    # LEVEL mapping
    if level == 0:
        # store + zlib-1 default
        c = _deflate_raw_zlib(data, 1, zlib.Z_DEFAULT_STRATEGY)
        _consider(c, METHOD_DEFLATE)
        return best_data, best_method

    if level == 1:
        c = _deflate_raw_zlib(data, 6, zlib.Z_DEFAULT_STRATEGY)
        _consider(c, METHOD_DEFLATE)
        return best_data, best_method

    if level == 2:
        c = _deflate_raw_zlib(data, 9, zlib.Z_DEFAULT_STRATEGY)
        _consider(c, METHOD_DEFLATE)
        return best_data, best_method

    if level >= 3:
        # пробуем все стратегии на уровне 9 (эмулирует libdeflate-12 конкурсом)
        for strat in _STRATEGIES:
            c = _deflate_raw_zlib(data, 9, strat)
            _consider(c, METHOD_DEFLATE)
        # также уровень 6 DEFAULT как fallback для мелких файлов (как AdvanceCOMP RETRY)
        if len(data) < 65536:
            c = _deflate_raw_zlib(data, 6, zlib.Z_DEFAULT_STRATEGY)
            _consider(c, METHOD_DEFLATE)
        # DEFLATE-only: BZIP2/LZMA/ZSTD не участвуют в сжатии.

    if level >= 4:
        # zopfli — только если разрешено политикой
        if _policy.zopfli_allowed(data, level) and _HAS_ZOPFLI:
            # time-bounded как в C: мелкие файлы — глубокий поиск
            if len(data) <= 64 * 1024:
                iters = 60
            elif len(data) <= 256 * 1024:
                iters = 30
            else:
                iters = 15
            cz = _deflate_zopfli(data, iterations=iters)
            _consider(cz, METHOD_DEFLATE)


    # финальная защита: если compressed >= original, оставить Store (ZIP не любит раздувание)
    # но если best_method != STORE и best_len == len(data) — лучше Store
    if best_len >= len(data) and best_method != METHOD_STORE:
        # равенство — предпочитаем Store (быстрее распаковка)
        if best_len == len(data):
            return bytes(data), METHOD_STORE
        # если всё хуже оригинала — Store
        if best_len > len(data):
            return bytes(data), METHOD_STORE

    return best_data, best_method


def decompress_buffer(comp_data: bytes, method: int, uncomp_size: int) -> Optional[bytes]:
    """
    Декодер для проверки. Поддерживает 0/8/12/14/93.
    """
    if comp_data is None or not isinstance(comp_data, (bytes, bytearray)):
        return None
    if not isinstance(method, int):
        return None
    if method == METHOD_STORE:
        return bytes(comp_data)
    try:
        if method == METHOD_DEFLATE:
            return zlib.decompress(bytes(comp_data), -15)
        elif method == METHOD_BZIP2:
            return bz2.decompress(bytes(comp_data))
        elif method == METHOD_LZMA:
            # LZMA RAW + header
            if len(comp_data) < 4:
                return None
            props_len = struct.unpack("<H", comp_data[2:4])[0]
            if 4 + props_len > len(comp_data):
                return None
            props = comp_data[4:4+props_len]
            lzdata = comp_data[4+props_len:]
            return lzma.decompress(lzdata, format=lzma.FORMAT_RAW, filters=[
                lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)
            ])
        elif method == METHOD_ZSTD:
            if not _HAS_ZSTD:
                return None
            return _zstd_mod.ZstdDecompressor().decompress(bytes(comp_data), max_output_size=uncomp_size if uncomp_size else 0)
    except Exception:
        return None
    return None
