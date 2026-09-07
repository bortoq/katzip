# Roadmap: ZIP-архиватор с максимальным сжатием

> Для кодера: этот документ — полный план реализации. Цель — создать утилиту `maxzip`,
> которая из набора файлов создаёт `.zip` меньшего размера, чем `7z -mx=9`, `WinZip Best`,
> `advzip -4`, за счёт перебора open-source энкодеров и выбора лучшего на каждый файл.
> Исследование проведено 2026-09-07. Исходная точка — `3rdpart/advancecomp-2.6.tar.gz`.

---

## 1. Что уже лежит в папке (анализ AdvanceCOMP 2.6)

**AdvanceCOMP — это НЕ архиватор, а рекомпрессор.** Он не создаёт ZIP с нуля,
а берёт готовый `.zip/.gz/.png/.mng` и пережимает внутренние deflate-потоки.

Ключевые файлы (внутри tar.gz):

| Файл | Роль |
|------|------|
| `rezip.cc` / `advzip.c` | CLI рекомпрессии ZIP: `open() → load() → shrink() → save()` |
| `zip.cc/.h`, `data.cc/.h`, `file.cc` | Свой парсер ZIP (local/central/EOCD, Zip64), CRC, extra-fields |
| `compress.cc/.h` | Ядро: `compress_deflate()` / `compress_zlib()` — каскадный перебор |
| `libdeflate/` | Вендоренная libdeflate (Eric Biggers) — быстрый + плотный deflate, уровни 1..12 |
| `zopfli/` | Вендоренный Zopfli (Google, Apache-2.0) — медленный, но самый плотный deflate |
| `7z/` (подключается `compress.h` → `7z/7z.h`, в тарболле 2.6 часть 7z-кода вынесена/подхватывается при сборке) | Deflate-энкодер 7-Zip (Igor Pavlov, LGPL) — `compress_rfc1950_7z(pass, fastbytes)` |
| `lib/` | Парсеры PNG/MNG |
| `configure`, `Makefile.am` | Autotools-сборка, опция `USE_BZIP2` |

Логика `compress_deflate(level)` (упрощённо):

```
shrink_insane (-4): Zopfli(numiterations=5..iter) → взять если меньше
shrink_normal/extra/insane: libdeflate(level 6/12/12) → взять если меньше
shrink_extra (-3): 7z-deflate(passes=15..255, fastbytes=255) → взять если меньше
shrink_none/fast (-0/-1/-2): zlib(Z_NO_COMPRESSION / Z_BEST_COMPRESSION)
+ для файлов < 64KB (RETRY_FOR_SMALL_FILES): пробовать несколько алгоритмов
```

Выводы для нас:
1. **Переиспользовать идею «конкурса энкодеров»** — сжать один буфер 3–4 библиотеками и оставить минимум. Это даёт 3–8% сверх `zlib -9` легально в рамках Deflate.
2. **Не копировать `zip.cc` целиком** — он старый, свой велосипед под рекомпрессию, без Zip64-генерации с нуля, без LZMA/BZIP2/ZSTD-записи, под GPLv2+. Для архиватора лучше взять `minizip-ng`/`libzip`.
3. **Лицензионная ловушка:** весь AdvanceCOMP — GPL (v2, в тарболле COPYING уже GPLv3-текст, фактически GPL). Прямое копирование `compress.cc/zip.cc` заражает проект GPL. Zopfli — Apache-2.0, libdeflate — MIT, 7-Zip SDK — LGPL/BSD-часть, minizip-ng — zlib-лицензия. Если нужен проприетарный результат — не вендорить код AdvanceCOMP, а только идею + прямые зависимости.

---

## 2. Спецификация ZIP (что можно класть внутрь, APPNOTE 6.3.10)

| Method ID | Имя | Open-source реализация | Совместимость распаковки |
|-----------|-----|------------------------|--------------------------|
| 0 | Store | — | 100% всё |
| 8 | Deflate (RFC1951) | zlib, zlib-ng, libdeflate, Zopfli, 7-Zip-Deflate, zultra, ECT, igzip | 100% всё (база, must-have) |
| 9 | Deflate64 | 7-Zip | Windows Explorer/7-Zip/WinZip — да; Info-ZIP/unzip, Python — НЕТ |
| 12 | BZIP2 | bzip2 (BSD), lbzip2 | 7-Zip, WinZip≥10, Info-ZIP, Python — да |
| 14 | LZMA | LZMA SDK / XZ Utils (public domain/LGPL) | 7-Zip, WinZip≥12, minizip-ng — да; Explorer — НЕТ |
| 93 (ex-20) | Zstandard (zstd) | facebook/zstd (BSD) | 7-Zip ≥21, WinZip ≥?, minizip-ng — да; Explorer/macOS — НЕТ |
| 95 | XZ (LZMA2) | XZ Utils | 7-Zip, WinZip — частично; узкая поддержка |
| 98 | PPMd | PPMd (public domain, из 7-Zip SDK) | 7-Zip, WinZip — да; остальное — НЕТ |
| 96 / 97 / 94 | Jpeg / WavPack / MP3 | разные | Только WinZip «Best method», почти нигде не читается |
| 99 | AES-шифрование | minizip-ng | Отдельная тема, на max-ratio не влияет |

Вывод: **только метод 8 (Deflate) даёт 100% совместимость.** Всё остальное — опциональный
режим «.zipx / max-ratio ценой совместимости», как делает WinZip (`.zipx` для LZMA/PPMd/BZIP2/JPEG).

---

## 3. Весь доступный open-source (инвентарь, проверено поиском 2026)

### 3.1 Deflate-энкодеры (все дают поток метода 8, отличаются только плотностью/скоростью)

| Проект | Лицензия | Плотность vs zlib -9 | Скорость | Брать? |
|--------|----------|----------------------|----------|--------|
| `zlib` 1.3.x / `zlib-ng` 2.x | zlib | база (0%) | быстро | Да — fallback и декодер |
| `libdeflate` 1.23 (ebiggers) | MIT | −2…−4% | очень быстро (6 MB/s, декомпрессия 500+ MB/s) | Да — уровень по умолчанию |
| `7-Zip Deflate` (SDK 24.x, `yumeyao/7zDeflate` порт) | LGPL/BSD | −3…−5%, иногда лучше Zopfli на мелких файлах | медленно (секунды) | Да — 2-й участник конкурса |
| `Zopfli` (google/zopfli) | Apache-2.0 | −3…−8% (эталон deflate-max) | очень медленно (~0.3 MB/s, в 80× медленнее gzip) | Да — режим `-4/--insane` |
| `ECT` Efficient-Compression-Tool (fhanau, форк Zopfli) | Apache-2.0 | чуть лучше Zopfli на high-levels, быстрее | средне-медленно | Да — альтернатива/замена Zopfli, уровни 1..9 |
| `zultra` (emmanuel-marty) | CC0/zlib/Apache | ≈ Zopfli, в 8–10× быстрее Zopfli | ~3.4 MB/s | Кандидат — если нужен «быстрый max» |
| `KZIP` (Ken Silverman, kzipmix) | freeware, не OSI | ≈ Zopfli | очень медленно | Нет — лицензия + снят с развития |
| `igzip` (Intel) | MIT | хуже max, но 3× быстрее zlib-1 | очень быстро | Нет для max-режима, да для fast-режима |

### 3.2 Не-Deflate методы для ZIP

| Проект | Метод | Лицензия |
|--------|-------|----------|
| `bzip2` 1.0.8 / `lbzip2` | 12 | BSD |
| `LZMA SDK` (7-Zip) / `XZ Utils` | 14 / 95 | Public domain + LGPL |
| `PPMd` (из 7-Zip SDK, `ppmd7`) | 98 | Public domain |
| `zstd` 1.5.6+ | 93 | BSD |

### 3.3 Каркас ZIP-архиватора (не писать свой zip.cc!)

| Проект | Лицензия | Почему |
|--------|----------|--------|
| `minizip-ng` (nmoinvaz/minizip-ng) | zlib | **Рекомендован.** Пишет/читает Store/Deflate/BZIP2/LZMA/PPMd/XZ/ZSTD, Zip64, AES, streaming, buffered I/O, NTFS-время, UTF-8. Активен в 2025–2026. |
| `libzip` 1.11 | BSD-3 | Хорошая альтернатива, но методов меньше (нет ZSTD из коробки во всех сборках). |
| `7-Zip`/`p7zip` исходники | LGPL | Эталон совместимости, но архитектура тяжёлая для встраивания. Брать только энкодеры. |
| `Info-ZIP` (zip/unzip) | BSD-like | Эталон совместимости CLI, код архаичный. |

### 3.4 Предобработка (даёт +1…+10% бесплатно до энкодера)

- **Детект несжимаемого:** `jpg/jpeg/png/gif/mp4/mp3/zip/gz/xz` — сразу Store без попыток жать (экономия CPU, иногда −байты).
- **Дедупликация:** одинаковые файлы (по SHA-256) — сжать один раз (ZIP не solid, повторы жмутся плохо).
- **Сортировка:** сначала большие текстовые/бинарные, мелкие в конец — не влияет на размер ZIP (не solid), но влияет на скорость конкурса и детерминизм.
- **Фильтры:** BCJ (x86/ARM) для `.exe/.dll/.so` перед Deflate (как в 7z). Осторожно: в чистом ZIP нет стандарта BCJ-префильтра — применять только если распаковщик наш же, иначе НЕ применять (сломает совместимость). Поэтому для метода 8 — без BCJ.
- **Зачистка:** не писать extra-fields, выровнять время в UTC, UTF-8-флаги, удалить data-descriptor где не нужен — экономит сотни байт на файл.
- **Порог мелких файлов:** файлы < 2–4 KB или с энтропией > 7.9 бит/байт — пробовать только Store + libdeflate-fast, пропускать Zopfli (как `RETRY_FOR_SMALL_FILES` в AdvanceCOMP).

---

## 4. Варианты решения (на выбор заказчику)

### Вариант A — «Deflate-Max, 100% совместимость» (рекомендуемый базовый)

Суть: архиватор, который пишет **только Store(0) + Deflate(8)**, но каждый файл сжимает
конкурсом `libdeflate → 7-Zip-Deflate → Zopfli/ECT` и оставляет минимум. Плюс предобработка из §3.4.
Распаковывается **везде** (Explorer, macOS, Python, Java, unzip).

| Критерий | Оценка |
|----------|--------|
| Сложность | **3–5 дней** (MVP) + 2–3 дня на тесты/бенчмарки |
| Риски | Низкие. Все зависимости зрелые. Главный риск — скорость Zopfli (лечится флагом и лимитом размера + многопоточностью) |
| Плюсы | 100% совместимость; −3…−8% к `zip -9` / −2…−5% к `7z -mx=9 -mfb=...` в ZIP; простая архитектура; лицензии чистые (MIT/Apache/zlib, без GPL если не копировать AdvanceCOMP) |
| Минусы | Не побьёт `7z -mx=9 -m0=lzma2` в формате `.7z` (другой формат, несравнимо); ZIP не solid — потолок плотности ограничен |
| Влияние на архитектуру | Тонкая обёртка над `minizip-ng`: модуль `competitor.c` (перебор энкодеров), модуль `policy.c` (выбор Store/Deflate, скип несжимаемого), CLI как у `advzip`. Свой ZIP-парсер писать не надо |

### Вариант B — «Multi-Method Max (.zip как у WinZip Best / .zipx)»

Суть: всё из A + на каждый файл пробуем **Deflate(Zopfli) vs BZIP2 vs LZMA vs PPMd vs ZSTD vs Store**
и пишем победителя с соответствующим method ID. Побеждает почти всегда LZMA/PPMd на тексте и ZSTD на бинарях.
Итого −10…−30% к чистому Deflate на смешанных корпусах.

| Критерий | Оценка |
|----------|--------|
| Сложность | **2–3 недели** (каркас minizip-ng уже умеет все методы, работа — в политике выбора, уровнях, тестах совместимости, CLI) |
| Риски | Средние. (1) Совместимость: Explorer/macOS/Python частично не откроют LZMA/PPMd/ZSTD — нужен флаг `--compat={max|wide}` и дефолт `wide`. (2) Память/CPU: LZMA/PPMd требуют настройки словаря. (3) Лицензии XZ/LZMA — ок, но следить за LGPL-динамикой |
| Плюсы | Абсолютный минимум размера в рамках-spec ZIP; режим `--compat=max` бьёт любой Deflate-архиватор; один бинарь покрывает и A (флагом) |
| Минусы | `.zip` с LZMA/PPMd/ZSTD не везде откроется; медленнее (ZSTD спасает, но LZMA-max медленный); сложнее тесты (матрица распаковщиков) |
| Влияние на архитектуру | Тот же каркас, + `method_policy` (эвристика по расширению/энтропии какой метод пробовать первым), + конфиг уровней на метод, + два пресета. Без BCAA-архитектурных ломок |

### Вариант C — «Пайплайн из готовых бинарей» (самый дешёвый, без C-кода)

Суть: не писать Си вообще. Python/Shell-скрипт `maxzip`: `7z a -mx=9 tmp.zip → advzip -4 → ect --zip` (или `7z + zopfli`).
Фактически воспроизводит AdvanceCOMP-пайплайн как внешний процесс.

| Критерий | Оценка |
|----------|--------|
| Сложность | **1–2 дня** |
| Риски | Средне-высокие для продукта: зависимость от установленных `7z/advzip/ect`, хрупкость (парсинг stdout, временные файлы), нет per-file политики, нет параллелизма, GPL-заражение если шиппить `advzip` внутри; для внутреннего скрипта — ок |
| Плюсы | Ноль разработки; сразу −3…−8%; хорошо для разового прогона/CI-артефактов |
| Минусы | Не продукт: медленно (последовательные перезаписи архива), нет контроля памяти, нет библиотеки/API, сложно чинить баги чужих CLI |
| Влияние на архитектуру | Архитектуры нет — скрипт. Как прототип для замера «потолка выгоды» перед вариантами A/B — идеально |

**Рекомендация исследователя:** делать **A как MVP (неделя), затем инкремент до B флагом `--best-method`**.
C — сделать за 2 часа как бенчмарк-скрипт для валидации A/B, не как продукт.

---

## 5. План для кодера (брать Вариант A, с заделом под B)

### 5.1 Структура репозитория

```
maxzip/
  CMakeLists.txt
  README.md
  LICENSE (Apache-2.0 или MIT — НЕ GPL, чтобы не тянуть copyleft AdvanceCOMP)
  src/
    main.c            # CLI, пресеты --fast/--normal/--max, --compat
    archiver.c/.h     # обход файлов, minizip-ng writer, Zip64, UTF-8, mtime
    competitor.c/.h   # конкурс энкодеров: compress_buffer(in) -> (out, method)
    policy.c/.h       # эвристики: skip-list расширений, энтропия, пороги размера
    mt.c/.h           # thread-pool (C++ std::thread или pthreads), очередь файлов
    bench.py          # скрипт сравнения с zip/7z/advzip (вариант C как референс)
  third_party/        # сабмодули, НЕ копии:
    minizip-ng/  libdeflate/  zopfli/ (или ECT-core)  7zip-sdk/ (только Deflate/LZMA)  zstd/  bzip2/ xz/
  tests/
    corpus/ (silesia, enwik8, набор jpg/exe/txt)
    test_roundtrip.sh # пожать → распаковать 5 распаковщиками → diff
```

Зависимости ставить сабмодулями + `find_package`: `minizip-ng ≥4.0`, `libdeflate ≥1.20`,
`zopfli master` или `ECT ≥0.9`, `7-Zip SDK 24.x` (только `C/7zDeflate.c` + `LZMA`), `zstd ≥1.5` (для B).

### 5.2 CLI-спецификация (совместимая с advzip/7z ментально)

```
maxzip a archive.zip <files...> [--level 0..4] [--compat max|wide] [--threads N] [--keep-time]
  --level 0 : Store + zlib-1 (fast, отладка)
  --level 1 : libdeflate-6 (fast)
  --level 2 : libdeflate-12 (normal, дефолт)
  --level 3 : libdeflate-12 + 7z-deflate passes=15 (extra)
  --level 4 : level3 + Zopfli/ECT iter=5..60 (insane/max, как advzip -4)
  --compat wide (дефолт): только методы 0/8 → откроется везде
  --compat max  (задел под B): разрешить 12/14/93/98/95, расширение .zipx при не-Deflate внутри
```

### 5.3 Алгоритм на файл (псевдокод — ядро max-сжатия)

```
for each file F (в пуле потоков):
  data = read(F)
  if ext in {jpg,jpeg,png,gif,mp4,mkv,mp3,ogg,zip,gz,xz,7z,zst} or entropy(data) > 7.85:
      candidate = Store(data)          # не тратить CPU
  else:
      best = Store(data); best_size = |data|
      c1 = libdeflate(data, lvl)       # всегда
      best = min(best, c1)
      if level>=3 and |data| > 4KB:
          c2 = 7z_deflate(data, passes, fb=255)
          best = min(best, c2)
      if level>=4 and |data| > 4KB and |data| < LIMIT (напр. 32MB):
          c3 = zopfli(data, iter)      # или ECT(data, level 5..9)
          best = min(best, c3)
      # задел B: if compat==max: c4=bzip2/c5=lzma/c6=zstd/c7=ppmd → min
  write_entry(archive, F, best)        # через minizip-ng, метод 0 или 8 (+B: 12/14/93/98)
```

Детали из AdvanceCOMP, которые обязательно сохранить:
- `oversize_*` буферы: `out = in + in/1000 + 12 + extra` (deflate может раздуться).
- Мелкие файлы: дублировать попытки (у 7z и libdeflate разные эвристики на <64KB).
- Всегда проверять `decompress(compress(data)) == data` в debug-сборке (Zopfli/7z-порты исторически давали битые потоки при кривой интеграции).
- Детерминизм: фиксировать `mtime`/`extra` или флаг `--keep-time`, иначе бенчмарки плавают.

### 5.4 Этапы (оценка под одного кодера)

1. **День 1 — каркас:** CMake + minizip-ng + libdeflate, `maxzip a` с методами 0/8, roundtrip-тест. Критерий: побайтово равен `unzip -p` оригиналу.
2. **День 2 — политика:** skip-list, энтропия, пороги, `--level 0..2`, многопоточность. Бенчмарк vs `zip -9`.
3. **День 3–4 — extra-плотность:** подключить 7z-Deflate + Zopfli/ECT, конкурс, `--level 3..4`. Бенчмарк vs `advzip -4` (цель: паритет ±0.5% на Deflate).
4. **День 5 — совместимость:** матрица распаковки (Explorer/7z/unzip/Python/Info-ZIP), Zip64 >4GB, UTF-8-имена, CI.
5. **Задел B (недели 2–3):** добавить bzip2/lzma/zstd/ppmd в конкурс, флаг `--compat max`, `.zipx`-детект, доку.

### 5.5 Тесты и метрики приёмки

- Корпусы: `enwik8 (100MB)`, `silesia`, смесь `txt+exe+jpg` 1GB.
- Метрики: `размер (байты)`, `время`, `ratio`, `открывается в {7z, unzip, Python zipfile, WinZip}`.
- Цель A: `size(maxzip -4) ≤ size(advzip -4) + 0.5%` и `< size(7z -mx=9 -mm=Deflate)`.
- Цель B: `size(--compat max) < size(--compat wide)` на тексте ≥5%, все методы читаются 7-Zip и minizip-ng.
- Фаззинг: `afl`/`libFuzzer` на декодер-чек + `zipdetails` валидация структуры.

### 5.6 Лицензии (не нарушить)

- Не копировать файлы AdvanceCOMP (`compress.cc`, `zip.cc`) в репо — только идея. Иначе весь `maxzip` станет GPL.
- Прямые зависимости: libdeflate MIT ✅, Zopfli/ECT Apache-2.0 ✅, minizip-ng zlib ✅, zstd BSD ✅, bzip2 BSD ✅, LZMA SDK public domain + LGPL-часть ⚠️ (линковать динамически или вынести в плагин).
- Итоговую лицензию держать Apache-2.0/MIT, в README — таблица лицензий third_party.

---

## 6. Источники

- Тарболл `3rdpart/advancecomp-2.6.tar.gz`: `README`, `compress.h/.cc`, `rezip.cc`, `zip.cc` (разобран в §1).
- https://github.com/google/zopfli — эталон max-deflate, Apache-2.0.
- https://github.com/ebiggers/libdeflate — быстрый плотный deflate, MIT.
- https://github.com/fhanau/Efficient-Compression-Tool — ECT, быстрее Zopfli при том же ratio.
- https://github.com/emmanuel-marty/zultra — near-Zopfli за 1/10 времени.
- https://www.7-zip.org/ + https://github.com/yumeyao/7zDeflate — 7-Zip Deflate/LZMA SDK.
- https://github.com/nmoinvaz/minizip-ng — каркас ZIP с методами 0/8/12/14/93/95/98, Zip64, AES.
- PKWARE APPNOTE 6.3.10 — method IDs; WinZip KB «Choosing a Compression Method» — матрица методов; en.wikipedia «ZIP (file format)» — история версий.
- encode.su треды «Optimal Deflate», «ECT» — практика: ECT ≥ Zopfli на high, libdeflate — best для fast.

---

## 7. Решение исследователя (итог для заказчика)

- Потолок в **совместимом ZIP (метод 8)** — это конкурс **libdeflate + 7-Zip-Deflate + Zopfli/ECT** (идея AdvanceCOMP), плюс гигиена (Store для уже сжатого, зачистка extra, Zip64). Это **Вариант A, 3–5 дней**.
- Абсолютный потолок в **ZIP-спецификации** — это **Вариант B**: per-file конкурс с LZMA/BZIP2/PPMd/ZSTD (WinZip-подход), −10…−30% сверх Deflate, но с оговоркой совместимости. Делать вторым шагом.
- Стоп-проблем не обнаружено. Блокеров «невозможно решить» нет: все энкодеры и каркас — зрелый open-source с совместимыми лицензиями.

---

## 8. Дополнение: проверка против kzip и доводка до наилучшей компрессии (2026-09-07)

**kzip** (Ken Silverman, advsys.net/ken/utils.htm, FreeBSD-порт `archivers/kzip`) —
PKZIP-совместимый компрессор «space over speed», только Deflate(8). По исследованию
Zopfli-статьи (Calgary/Canterbury/enwik8/Alexa): `Zopfli < kzip < 7-Zip < gzip -9`
на всех корпусах (~1% выигрыш Zopfli над kzip). ECT (fhanau) — чуть плотнее Zopfli.

### Найденные проблемы katzip (почему проигрывал)
1. `HAVE_ZOPFLI=0` в Makefile — лучший Deflate-энкодер был скомпилирован, но выключен.
   Конкурс фактически был `zlib + libdeflate` — слабее kzip.
2. `HAVE_BZ2=0` (нет `bzlib.h` в системе) — BZIP2-кандидат отсутствовал.
3. Политика скипала `.pdf` целиком — текстовые PDF не сжимались вообще.
4. Промежуточно включались LZMA(14)/ZSTD(93) — меньше размер, но `unzip 6.0` и Python
   `zipfile` такие архивы не открывают (`need PK compat. v6.3`) — дисквалификация в тестах.

### Что сделано (итог — DEFLATE-only по требованию)
- Завендорены `third_party/zopfli` (google/zopfli, Apache-2.0) и `third_party/bzip2`
  (libarchive-зеркало, BSD) — сборка без `apt`, `make` компилирует их статически внутрь.
- Конкурс на файл: `zlib(уровни×5 стратегий) + libdeflate(1..12) + Zopfli(max, 2 прохода
  blocksplit) + BZIP2 → минимум` (промежуточно), затем сужено до **только 0/8**:
  BZIP2/LZMA/ZSTD/PPMd исключены из сжатия — выход всегда Store/Deflate,
  как у kzip, читается везде (`unzip -t`, Python, 7-Zip).
- Zopfli с time-bound: `≤64KB→300 итераций (+2-й проход), ≤256KB→60, ≤1MB→30, иначе 15`.
  Zopfli@15 уже бьёт kzip; высокие итерации — только там, где дёшево.
- `.pdf` убран из skip-листа (текстовый PDF 158KB → 404B); энтропия считается по первым
  32KB (скорость); `rand.bin` 200KB → Store за 0.002с.
- Регрессия в `tests_c.sh`: `katzip ≤ zip -9 И ≤ 7z -mx9` на тексте и коде, иначе FAIL.
  Замеры: текст 160KB `katzip 740 < zip 793 < 7z 803`; код 11KB `katzip ~3000 < 7z ~3046
  < zip ~3126` — всё методом 8 (Deflate), `unzip -t` + Python OK.

### Дальнейшие методы улучшения (не внедрены, причины)
| Метод | Оценка |
|-------|--------|
| 7-Zip Deflate (`-mfb 128..258` band, yumeyao/7zDeflate) | +0.1–0.5% сверх трио; требует вендоринга LGPL-кода; отложено |
| ECT вместо Zopfli | чуть плотнее/быстрее Zopfli; отдельный C++ движок, сложнее вендорить; отложено |
| DeflOpt (переоптимизация Хаффмана) | +байты на готовом deflate; freeware-бинарь без библиотеки; нельзя встроить |
| PPMd(98)/LZMA(14)/ZSTD(93) в ZIP | −10–30% размера, но ломают `unzip`/Python — несовместимо с требованием Deflate-only |
| BCJ/дельта-префильтры | нет стандарта в ZIP для метода 8 — сломали бы распаковку чужими tools |
| Дедупликация одинаковых файлов | ZIP не solid: выигрыш только CPU, не размер; не влияет на тесты |
