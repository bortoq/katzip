#!/bin/bash
# Stage 2 acceptance: second engine linked, guards, DEFLATE-only, standalone.
set -e
echo "=== Stage 2 tests (second engine) ==="
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export KATZIP_INI="${KATZIP_INI:-/dev/null}"

# 1. unit tests for src/enhanced.c (guards, budgets, round-trips)
ZOPFLI_SRCS="third_party/zopfli/src/zopfli/blocksplitter.c third_party/zopfli/src/zopfli/cache.c third_party/zopfli/src/zopfli/deflate.c third_party/zopfli/src/zopfli/hash.c third_party/zopfli/src/zopfli/katajainen.c third_party/zopfli/src/zopfli/lz77.c third_party/zopfli/src/zopfli/squeeze.c third_party/zopfli/src/zopfli/tree.c third_party/zopfli/src/zopfli/util.c third_party/zopfli/src/zopfli/zlib_container.c third_party/zopfli/src/zopfli/gzip_container.c third_party/zopfli/src/zopfli/zopfli_lib.c"
gcc -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -Isrc -Ithird_party/zopfli/src \
  tests/tests_enhanced.c src/enhanced.c src/config.c src/competitor.c src/policy.c $ZOPFLI_SRCS \
  -o $TMP/test_enh -lm -lz $(test -f /usr/include/libdeflate.h && echo "-DHAVE_LIBDEFLATE -ldeflate" || echo "")
$TMP/test_enh | tail -1 | grep -q "ALL ENHANCED UNIT TESTS PASSED"
echo "unit: OK"

# 2. engine linked into the binary
strings ./katzip | grep -q "enhanced"
echo "linked: OK"

# 3. DEFLATE-only + readable everywhere on structured data
python3 -c "
import random
random.seed(99)
with open('$TMP/struct.txt','w') as f:
    for i in range(400):
        f.write('record_%04d: value=%08x status=OK padding..........\\n' % (i % 53, (i*2654435761) & 0xffffffff))
"
./katzip $TMP/s2 $TMP/struct.txt
unzip -t $TMP/s2.zip | grep -q "No errors"
python3 -c "
import zipfile
z=zipfile.ZipFile('$TMP/s2.zip')
for i in z.infolist():
    assert i.compress_type in (0,8), 'forbidden method'
open('$TMP/orig.txt','wb').write(open('$TMP/struct.txt','rb').read())
assert z.read(z.namelist()[0])==open('$TMP/struct.txt','rb').read(), 'roundtrip mismatch'
"
echo "deflate-only+roundtrip: OK"

# 4. still beats zip -9 and makes no child processes
rm -f $TMP/std.zip && zip -9 -j $TMP/std.zip $TMP/struct.txt >/dev/null
python3 -c "
import os
a=os.path.getsize('$TMP/s2.zip'); b=os.path.getsize('$TMP/std.zip')
print(f'stage2={a} zip-9={b}')
assert a <= b, 'lost to zip -9'
"
CNT=$(strace -f -e execve ./katzip $TMP/st2 $TMP/struct.txt 2>&1 | grep -c "execve")
if [ "$CNT" -gt 1 ]; then echo "FAIL: calls external"; exit 1; fi
echo "regression+standalone: OK"

# 5. katzip.ini actually tunes the contest
printf '[zopfli]\nenabled = off\n[enhanced]\nenabled = off\n' > $TMP/noheavy.ini
KATZIP_INI=$TMP/noheavy.ini ./katzip $TMP/noheavy $TMP/struct.txt
DESC=$(KATZIP_INI=$TMP/noheavy.ini ./katzip $TMP/noheavy2 $TMP/struct.txt 2>&1 | tr '\r' '\n' | tail -1)
echo "without heavy engines: $DESC"
echo "$DESC" | grep -Eq "Deflate (zlib|libdeflate)"
echo "$DESC" | grep -q "Zopfli\|enhanced" && { echo "FAIL: disabled engine won"; exit 1; }
unzip -t $TMP/noheavy.zip | grep -q "No errors"
printf '[zlib]\nstrategies = fixed\n[zopfli]\nenabled = off\n[libdeflate]\nenabled = off\n[enhanced]\nenabled = off\n' > $TMP/fixed.ini
D2=$(KATZIP_INI=$TMP/fixed.ini ./katzip $TMP/fixed $TMP/struct.txt 2>&1 | tr '\r' '\n' | tail -1)
echo "fixed-only: $D2"
echo "$D2" | grep -q "strategy 4"
echo "ini-tuning: OK"

# 6. config source printed at startup, master switch, exe-dir lookup
echo "probe probe probe" > $TMP/probe.txt
SC=$(./katzip $TMP/scq $TMP/probe.txt 2>&1 | head -1)
echo "startup: $SC"
echo "$SC" | grep -Eq "^config: "
printf '[zopfli]\nenabled = off\n' > $TMP/off.ini
echo "$(KATZIP_INI=$TMP/off.ini ./katzip $TMP/scq2 $TMP/probe.txt 2>&1 | head -1)" | grep -q "config: $TMP/off.ini"
# master switch: no Zopfli-named winner when zopfli is off (enhanced included)
D3=$(KATZIP_INI=$TMP/off.ini ./katzip $TMP/master $TMP/struct.txt 2>&1 | tr '\r' '\n' | tail -1)
echo "master-off: $D3"
echo "$D3" | grep -q "Zopfli" && { echo "FAIL: zopfli won while disabled"; exit 1; }
echo "$D3" | grep -q "enhanced" && { echo "FAIL: enhanced won while zopfli disabled"; exit 1; }
unzip -t $TMP/master.zip | grep -q "No errors"
# exe-dir lookup: binary copy + ini beside it, foreign CWD, clean env
mkdir -p $TMP/fakebin $TMP/elsewhere
cp ./katzip $TMP/fakebin/katzip
printf '[zopfli]\nenabled = off\n' > $TMP/fakebin/katzip.ini
cp $TMP/struct.txt $TMP/elsewhere/f.txt
D4=$(env -u KATZIP_INI sh -c "cd $TMP/elsewhere && $TMP/fakebin/katzip e.zip f.txt" 2>&1 | tr "\r" "\n" | tail -1)
echo "exe-dir: $D4"
echo "$D4" | grep -q "Zopfli\|enhanced" && { echo "FAIL: exe-dir ini ignored"; exit 1; }
SRC=$(env -u KATZIP_INI sh -c "cd $TMP/elsewhere && $TMP/fakebin/katzip eq.zip f.txt" 2>&1 | head -1)
echo "$SRC" | grep -q "fakebin/katzip.ini"
echo "config-source: OK"

echo "ALL STAGE 2 TESTS PASSED"
