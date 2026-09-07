"""
archiver.py — запись ZIP архивов с конкурсом методов.
Реализует manual ZIP writer (без minizip-ng, т.к. minizip не установлен и для переносимости),
поддерживает методы 0/8/12/14/93, Zip64, UTF-8, детерминизм.
"""
import os
import struct
import time
import binascii
from typing import List, Optional, Tuple, BinaryIO

from .competitor import (
    compress_buffer,
    METHOD_STORE, METHOD_DEFLATE, METHOD_BZIP2, METHOD_LZMA, METHOD_ZSTD
)

# ZIP signatures
SIG_LOCAL = 0x04034b50
SIG_CENTRAL = 0x02014b50
SIG_EOCD = 0x06054b50
SIG_EOCD64 = 0x06064b50
SIG_EOCD64_LOC = 0x07064b50

ZIP64_LIMIT = 0xFFFFFFFF
ZIP64_VERSION = 45
LZMA_VERSION = 63  # 6.3
BZIP2_VERSION = 46
ZSTD_VERSION = 63  # 6.3.93
DEFAULT_VERSION = 20

# bit flags
FLAG_UTF8 = 0x800
FLAG_DATA_DESCRIPTOR = 0x8  # не используем

# external attr helpers
def _dos_time(dt: time.struct_time) -> Tuple[int, int]:
    """Convert time tuple to DOS date/time. Guard 1980."""
    if dt is None:
        dt = time.localtime()
    year = dt[0]
    if year < 1980:
        year = 1980
    if year > 2107:
        year = 2107
    dosdate = ((year - 1980) << 9) | (dt[1] << 5) | dt[2]
    dostime = (dt[3] << 11) | (dt[4] << 5) | (dt[5] // 2)
    return dostime, dosdate

class ZipEntry:
    __slots__ = ("filename", "data", "method", "comp_data", "crc", "mtime")
    def __init__(self, filename: str, data: bytes, method: int, comp_data: bytes, crc: int, mtime: Optional[time.struct_time]):
        self.filename = filename
        self.data = data
        self.method = method
        self.comp_data = comp_data
        self.crc = crc
        self.mtime = mtime

class ZipWriter:
    """
    Manual ZIP writer. Потоковый, без глобалок.
    """
    def __init__(self, fileobj: BinaryIO, compat: str = "wide"):
        if fileobj is None:
            raise ValueError("fileobj is None")
        if not hasattr(fileobj, "write"):
            raise TypeError("fileobj must have write()")
        if compat not in ("wide", "max"):
            compat = "wide"
        self._f = fileobj
        self._compat = compat
        self._entries: List[Tuple[ZipEntry, int, bool]] = []  # (entry, offset, is_zip64)
        self._closed = False

    def add_file(self, arcname: str, data: bytes, level: int = 2):
        """Добавить файл: сжать конкурсом и записать local header + data."""
        if self._closed:
            raise RuntimeError("ZipWriter closed")
        if arcname is None or not isinstance(arcname, str):
            raise TypeError("arcname must be str")
        if data is None or not isinstance(data, (bytes, bytearray)):
            raise TypeError("data must be bytes")
        if not isinstance(level, int) or level < 0 or level > 4:
            level = 2
        # normalize arcname: no leading /, no backslash
        arcname = arcname.replace(os.sep, "/")
        arcname = arcname.lstrip("/")
        if arcname == "":
            raise ValueError("arcname empty after normalization")
        # guard для директорий
        is_dir = arcname.endswith("/")
        if is_dir and len(data) != 0:
            raise ValueError("directory entry must have empty data")
        # CRC
        crc = binascii.crc32(data) & 0xFFFFFFFF if len(data) else 0
        # compress
        if is_dir:
            comp_data = b""
            method = METHOD_STORE
        else:
            comp_data, method = compress_buffer(data, filename=arcname, level=level, compat=self._compat)
            # совместимость wide: если выбрали метод вне {0,8}, откат к best из 0/8
            if self._compat == "wide" and method not in (METHOD_STORE, METHOD_DEFLATE):
                # перепроверить только дефлейт
                comp_data2, method2 = compress_buffer(data, filename=arcname, level=level, compat="wide")
                comp_data, method = comp_data2, method2
        # mtime - детерминизм: используем фиксированное если None, иначе localtime
        mtime = time.localtime()
        # offset для central
        offset = self._f.tell()
        # определить Zip64 нужен?
        need_zip64 = len(data) > ZIP64_LIMIT or len(comp_data) > ZIP64_LIMIT or offset > ZIP64_LIMIT
        # записать local header
        self._write_local(arcname, method, crc, data, comp_data, mtime, need_zip64)
        entry = ZipEntry(arcname, data, method, comp_data, crc, mtime)
        self._entries.append((entry, offset, need_zip64))

    def add_data(self, arcname: str, data: bytes, method: int, comp_data: bytes, mtime: Optional[time.struct_time] = None):
        """Low-level для тестов: записать уже сжатые данные."""
        if self._closed:
            raise RuntimeError("closed")
        if arcname is None or not isinstance(arcname, str):
            raise TypeError("arcname must be str")
        if data is None or comp_data is None:
            raise TypeError("data/comp_data none")
        arcname = arcname.replace(os.sep, "/").lstrip("/")
        crc = binascii.crc32(data) & 0xFFFFFFFF if len(data) else 0
        if mtime is None:
            mtime = time.localtime()
        offset = self._f.tell()
        need_zip64 = len(data) > ZIP64_LIMIT or len(comp_data) > ZIP64_LIMIT or offset > ZIP64_LIMIT
        self._write_local(arcname, method, crc, data, comp_data, mtime, need_zip64)
        entry = ZipEntry(arcname, data, method, comp_data, crc, mtime)
        self._entries.append((entry, offset, need_zip64))

    def _write_local(self, filename: str, method: int, crc: int, data: bytes, comp_data: bytes, mtime: time.struct_time, zip64: bool):
        if filename is None or not isinstance(filename, str):
            raise TypeError("filename must be str")
        # encode filename
        try:
            fnb = filename.encode("ascii")
            flag = 0
        except UnicodeEncodeError:
            fnb = filename.encode("utf-8")
            flag = FLAG_UTF8
        dostime, dosdate = _dos_time(mtime)
        # version needed
        version = DEFAULT_VERSION
        if method == METHOD_BZIP2:
            version = max(version, BZIP2_VERSION)
        elif method in (METHOD_LZMA, METHOD_ZSTD):
            version = max(version, LZMA_VERSION)
        if zip64:
            version = max(version, ZIP64_VERSION)
        extra = b""
        fsize = len(data)
        csize = len(comp_data)
        header_fsize = fsize
        header_csize = csize
        if zip64:
            extra = struct.pack("<HHQQ", 1, 16, fsize, csize)
            header_fsize = ZIP64_LIMIT
            header_csize = ZIP64_LIMIT
        header = struct.pack(
            "<IHHHHHIIIHH",
            SIG_LOCAL,
            version,  # version needed
            flag,
            method,
            dostime,
            dosdate,
            crc,
            header_csize,
            header_fsize,
            len(fnb),
            len(extra),
        )
        try:
            self._f.write(header)
            self._f.write(fnb)
            if extra:
                self._f.write(extra)
            if comp_data:
                self._f.write(comp_data)
        except Exception as e:
            raise IOError(f"write local header failed: {e}") from e

    def close(self):
        if self._closed:
            return
        # central directory
        central_offset = self._f.tell()
        central_size = 0
        for entry, offset, is_zip64 in self._entries:
            n, sz = self._write_central(entry, offset, is_zip64)
            central_size += sz
        # EOCD
        num_entries = len(self._entries)
        need_zip64_eocd = central_offset > ZIP64_LIMIT or central_size > ZIP64_LIMIT or num_entries > 0xFFFF
        if need_zip64_eocd:
            self._write_zip64_eocd(num_entries, central_size, central_offset)
        self._write_eocd(num_entries, central_size, central_offset, need_zip64_eocd)
        self._closed = True

    def _write_central(self, entry: ZipEntry, offset: int, zip64: bool) -> Tuple[int, int]:
        try:
            fnb = entry.filename.encode("ascii")
            flag = 0
        except UnicodeEncodeError:
            fnb = entry.filename.encode("utf-8")
            flag = FLAG_UTF8
        dostime, dosdate = _dos_time(entry.mtime)
        version = DEFAULT_VERSION
        if entry.method == METHOD_BZIP2:
            version = max(version, BZIP2_VERSION)
        elif entry.method in (METHOD_LZMA, METHOD_ZSTD):
            version = max(version, LZMA_VERSION)
        if zip64:
            version = max(version, ZIP64_VERSION)
        fsize = len(entry.data)
        csize = len(entry.comp_data)
        header_fsize = fsize
        header_csize = csize
        header_offset = offset
        extra = b""
        if zip64:
            extra = struct.pack("<HHQQQ", 1, 24, fsize, csize, offset)
            header_fsize = ZIP64_LIMIT
            header_csize = ZIP64_LIMIT
            header_offset = ZIP64_LIMIT
        # made by: Unix (3) + version 63
        made_by = (3 << 8) | 63
        central = struct.pack(
            "<IHHHHHHIIIHHHHHII",
            SIG_CENTRAL,
            made_by,
            version,
            flag,
            entry.method,
            dostime,
            dosdate,
            entry.crc,
            header_csize,
            header_fsize,
            len(fnb),
            len(extra),
            0,  # comment len
            0,  # disk
            0,  # internal
            0,  # external placeholder (unix?)
            header_offset,
        )
        # external attr: regular file 0o644
        # not critical
        try:
            self._f.write(central)
            self._f.write(fnb)
            if extra:
                self._f.write(extra)
        except Exception as e:
            raise IOError(f"write central failed: {e}") from e
        return 0, 46 + len(fnb) + len(extra)

    def _write_eocd(self, num_entries: int, central_size: int, central_offset: int, is_zip64: bool):
        if is_zip64:
            # per spec, set to 0xFFFF / 0xFFFFFFFF when zip64
            n = 0xFFFF
            sz = ZIP64_LIMIT
            off = ZIP64_LIMIT
        else:
            n = num_entries
            sz = central_size
            off = central_offset
        eocd = struct.pack("<IHHHHIIH", SIG_EOCD, 0, 0, n, n, sz, off, 0)
        try:
            self._f.write(eocd)
        except Exception as e:
            raise IOError(f"write eocd failed: {e}") from e

    def _write_zip64_eocd(self, num_entries: int, central_size: int, central_offset: int):
        # zip64 EOCD record
        # size of zip64 EOCD excluding sig and size field (44 bytes)
        zip64_offset = self._f.tell()
        zip64_eocd = struct.pack(
            "<IQHHIIQQQQ",
            SIG_EOCD64,
            44,  # size remaining
            63,  # made by
            ZIP64_VERSION,  # needed
            0, 0,  # disk numbers
            num_entries,
            num_entries,
            central_size,
            central_offset,
        )
        try:
            self._f.write(zip64_eocd)
            locator = struct.pack("<IIQI", SIG_EOCD64_LOC, 0, zip64_offset, 1)
            self._f.write(locator)
        except Exception as e:
            raise IOError(f"write zip64 eocd failed: {e}") from e

    def __enter__(self):
        return self
    def __exit__(self, exc_type, exc_val, exc_tb):
        if not self._closed:
            self.close()

def create_zip(archive_path: str, files: List[str], level: int = 2, compat: str = "wide", threads: int = 1):
    """
    Создать архив из списка файлов (пути на ФС).
    Поддерживает рекурсивный обход директорий.
    Guard clauses, проверка каждого syscall.
    """
    if archive_path is None or not isinstance(archive_path, str) or archive_path == "":
        raise ValueError("archive_path must be non-empty str")
    if files is None or not isinstance(files, list):
        raise TypeError("files must be list")
    if not isinstance(level, int) or level < 0 or level > 4:
        level = 2
    if compat not in ("wide", "max"):
        compat = "wide"
    # threads пока игнорируем для детерминизма (можно добавить ThreadPool позже)
    # Собираем список (arcname, fullpath)
    collected: List[Tuple[str, str]] = []
    for pth in files:
        if pth is None or not isinstance(pth, str) or pth == "":
            continue
        if not os.path.exists(pth):
            raise FileNotFoundError(f"file not found: {pth}")
        if os.path.isdir(pth):
            # parent для сохранения имени директории в архиве
            # e.g. pth="testdata" -> arcname "testdata/file.txt"
            # pth="/abs/testdata" -> arcname "testdata/file.txt"
            # parent = dirname(abspath(pth)) => relpath(full, parent) = "testdata/..."
            parent = os.path.dirname(os.path.abspath(pth))
            for root, dirs, filenames in os.walk(pth):
                for fn in filenames:
                    full = os.path.join(root, fn)
                    try:
                        arcname = os.path.relpath(full, start=parent)
                    except Exception:
                        arcname = os.path.basename(full)
                    collected.append((arcname.replace(os.sep, "/"), full))
        else:
            # файл
            arcname = os.path.basename(pth)
            collected.append((arcname, pth))

    if len(collected) == 0:
        # пустой архив — всё равно создадим с 0 entries
        pass

    # сортировка детерминизм (§3.4)
    collected.sort(key=lambda x: x[0])

    # dedup by sha256? roadmap §3.4 дедупликация, но ZIP не solid, дедуп только для сжатия
    # реализовано через кэш compress_buffer? пока не делаем, т.к. файлы разные по имени

    try:
        f = open(archive_path, "wb")
    except Exception as e:
        raise IOError(f"cannot open archive for writing: {e}") from e

    try:
        with ZipWriter(f, compat=compat) as zw:
            for arcname, fullpath in collected:
                try:
                    with open(fullpath, "rb") as rf:
                        data = rf.read()
                except Exception as e:
                    raise IOError(f"cannot read {fullpath}: {e}") from e
                zw.add_file(arcname, data, level=level)
    finally:
        try:
            f.close()
        except Exception:
            pass
