# katzip — ZIP архиватор с максимальным сжатием (DEFLATE-only)

Реализация по `roadmap.md` (самый плотный режим `-4`). Создаёт `.zip` меньше чем `zip -9` / `7z -mx=9` / `kzip` за счёт конкурса DEFLATE-энкодеров на каждый файл. **Только методы Store(0)/Deflate(8)** — честный бой с kzip (тоже deflate-only) и 100% совместимость: `unzip` + Python `zipfile` + 7-Zip + Explorer + macOS. **Самостоятельная программа на C, не вызывает внешние утилиты** (проверено `strace`). Если расширение `.zip` пропущено, добавляется автоматически.

## Сборка (C)

```bash
make          # -> ./katzip (динамический, требует libz/liblzma/libzstd/libdeflate)
make static   # -> ./katzip_static (статический, без зависимостей)
./katzip --help
```

Зависимости для сборки: `gcc`, `zlib1g-dev`, `liblzma-dev`, `libzstd-dev`, `libdeflate-dev` (опционально `libbz2-dev`). Авто-детект в Makefile.

## Использование

```bash
katzip <archive.zip> <files...>
Example: katzip archive file.txt
# Пример:
katzip mydocs README.md src/
# создаст mydocs.zip если указано mydocs без расширения

# Python версия (аналог):
python -m maxzip.cli archive file.txt
```

Проверка:
```bash
katzip archive file.txt
unzip -l archive.zip
7z l archive.zip
make test && python -m pytest -q
```

## Архитектура

- `c_src/policy.{h,c}` — эвристики (§3.4): skip по расширению, энтропия >7.85, пороги 4KB/32MB
- `c_src/competitor.{h,c}` — конкурс DEFLATE (§5.3, §8): `zlib(уровни×стратегии) + libdeflate(1..12) + Zopfli(max)` → минимум, проверка `inflate==orig`; BZIP2/LZMA/ZSTD/PPMd НЕ используются при сжатии (только Deflate — как kzip)
- `c_src/archiver.{h,c}` — manual ZIP writer (Store/Deflate/BZIP2/LZMA/ZSTD), Zip64, UTF-8, CRC32
- `c_src/main.c` — CLI `katzip <archive.zip> <files...>` (только самый плотный режим, авто `.zip`)
- `maxzip/*.py` — Python референс

Лицензии: `libdeflate MIT`, `zopfli Apache-2.0`, `zstd BSD`, `bzip2 BSD`, `LZMA public domain`, код проекта MIT.
