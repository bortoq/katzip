#!/bin/bash
set -e
echo "=== C tests (katzip) ==="
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# 1. help
./katzip --help 2>&1 | grep -q "Usage: katzip"
./katzip 2>&1 | grep -q "Usage: katzip"
echo "help: OK"

# 2. auto .zip extension
echo "hello" > $TMP/a.txt
rm -f $TMP/auto.zip
./katzip $TMP/auto $TMP/a.txt
test -f $TMP/auto.zip
rm -f $TMP/explicit.zip
./katzip $TMP/explicit.zip $TMP/a.txt
test -f $TMP/explicit.zip
./katzip $TMP/already.zip $TMP/a.txt
test -f $TMP/already.zip
test ! -f $TMP/already.zip.zip
./katzip $TMP/upper.ZIP $TMP/a.txt
test -f $TMP/upper.ZIP
echo "auto extension: OK"

# 3. empty file
touch $TMP/empty.txt
./katzip $TMP/empty $TMP/empty.txt
unzip -l $TMP/empty.zip >/dev/null
python3 -c "import zipfile; assert zipfile.ZipFile('$TMP/empty.zip').read('empty.txt')==b''"
echo "empty: OK"

# 4. small text (must be unzip+python readable)
echo "hello world hello world hello world" > $TMP/b.txt
./katzip $TMP/b.zip $TMP/b.txt
unzip -t $TMP/b.zip | grep -q "No errors"
python3 -c "import zipfile; assert b'hello' in zipfile.ZipFile('$TMP/b.zip').read('b.txt')"
echo "small text: OK"

# 5. directory with random + jpg (methods must stay 0/8 = DEFLATE-only)
mkdir -p $TMP/src
echo "text data text data text data" > $TMP/src/a.txt
head -c 10000 /dev/urandom > $TMP/src/b.bin
cp c_src/policy.c $TMP/src/c.jpg
./katzip $TMP/dir $TMP/src
unzip -t $TMP/dir.zip | grep -q "No errors"
7z l $TMP/dir.zip >/dev/null
python3 -c "import zipfile; z=zipfile.ZipFile('$TMP/dir.zip'); assert len([n for n in z.namelist() if not n.endswith('/')])==3"
# invariant: only Store/Deflate/BZIP2 (unzip-readable)
python3 -c "
import zipfile
z=zipfile.ZipFile('$TMP/dir.zip')
for i in z.infolist():
    assert i.compress_type in (0,8), f'forbidden {i.compress_type} in {i.filename} (deflate-only)'
"
echo "directory: OK"

# 6. incompressible store (jpg)
./katzip $TMP/jpg $TMP/src/c.jpg
python3 -c "import zipfile; p='$TMP/jpg.zip'; z=zipfile.ZipFile(p); i=z.getinfo(z.namelist()[0]); assert i.compress_type==0, 'jpg should be stored'"
echo "incompressible: OK"

# 7. COMPRESSION REGRESSION: katzip must beat zip -9 and 7z -mx9 (kzip-class target)
python3 -c "open('$TMP/text.txt','w').write('Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor. '*2000)"
cp c_src/competitor.c $TMP/code.c
./katzip $TMP/kz_text $TMP/text.txt
./katzip $TMP/kz_code $TMP/code.c
rm -f $TMP/std_text.zip && zip -9 -j $TMP/std_text.zip $TMP/text.txt >/dev/null
rm -f $TMP/s7_text.zip && 7z a -tzip -mx=9 $TMP/s7_text.zip $TMP/text.txt >/dev/null
rm -f $TMP/std_code.zip && zip -9 -j $TMP/std_code.zip $TMP/code.c >/dev/null
rm -f $TMP/s7_code.zip && 7z a -tzip -mx=9 $TMP/s7_code.zip $TMP/code.c >/dev/null
python3 -c "
import os
kz_t=os.path.getsize('$TMP/kz_text.zip'); z_t=os.path.getsize('$TMP/std_text.zip'); s_t=os.path.getsize('$TMP/s7_text.zip')
kz_c=os.path.getsize('$TMP/kz_code.zip'); z_c=os.path.getsize('$TMP/std_code.zip'); s_c=os.path.getsize('$TMP/s7_code.zip')
print(f'text: katzip={kz_t} zip-9={z_t} 7z-mx9={s_t}')
print(f'code: katzip={kz_c} zip-9={z_c} 7z-mx9={s_c}')
assert kz_t <= z_t and kz_t <= s_t, 'katzip lost on text'
assert kz_c <= z_c and kz_c <= s_c, 'katzip lost on code'
"
unzip -t $TMP/kz_text.zip | grep -q "No errors"
unzip -t $TMP/kz_code.zip | grep -q "No errors"
python3 -c "import zipfile; zipfile.ZipFile('$TMP/kz_text.zip').read('text.txt'); zipfile.ZipFile('$TMP/kz_code.zip').read('code.c')"
7z e -so $TMP/kz_code.zip code.c > $TMP/out.c 2>/dev/null && diff $TMP/code.c $TMP/out.c && echo "roundtrip 7z: OK"
echo "regression: OK"

# 8. no external exec
CNT=$(strace -f -e execve ./katzip $TMP/strace $TMP/b.txt 2>&1 | grep -c "execve")
if [ "$CNT" -gt 1 ]; then echo "FAIL: calls external"; exit 1; fi
echo "no external calls: OK"

# 9. compare with python katzip
python3 -m maxzip.cli $TMP/py $TMP/code.c
test -f $TMP/py.zip
unzip -t $TMP/py.zip | grep -q "No errors"
7z l $TMP/py.zip >/dev/null
echo "python compat: OK"

echo "ALL C TESTS PASSED"
