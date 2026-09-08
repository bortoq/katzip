#!/bin/bash
set -e
echo "=== C tests (katzip) ==="
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export KATZIP_INI="${KATZIP_INI:-/dev/null}"

# 1. help
./katzip --help 2>&1 | grep -q "Usage:.*katzip"
./katzip 2>&1 | grep -q "Usage:.*katzip"
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
cp src/policy.c $TMP/src/c.jpg
./katzip $TMP/dir $TMP/src
unzip -t $TMP/dir.zip | grep -q "No errors"
7z l $TMP/dir.zip >/dev/null
python3 -c "import zipfile; z=zipfile.ZipFile('$TMP/dir.zip'); assert len([n for n in z.namelist() if not n.endswith('/')])==3"
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

# 7. COMPRESSION REGRESSION: katzip must beat zip -9 and 7z -mx9
python3 -c "open('$TMP/text.txt','w').write('Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor. '*2000)"
cp src/competitor.c $TMP/code.c
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

# 9. lean container (Leanify rule): no extra fields, no data descriptors,
#    no comments anywhere — our writer emits the minimal layout already.
python3 -c "
import struct
d=open('$TMP/b.zip','rb').read()
ver,flag,meth,mt,md,crc,cs,us,fnl,efl = struct.unpack('<HHHHHIIIHH', d[4:30])
assert efl == 0, 'local extra field present'
assert flag & 0x08 == 0, 'data descriptor bit set'
ci = d.find(b'PK\x01\x02')
c = struct.unpack('<HHHHHHIIIHHHHHII', d[ci+4:ci+46])
assert c[10] == 0, 'central extra field present'
assert c[11] == 0, 'file comment present'
ei = d.rfind(b'PK\x05\x06')
e = struct.unpack('<HHHHIIH', d[ei+4:ei+22])
assert e[6] == 0, 'archive comment present'
# exact overhead: 30 + 46 + 22 + 2*namelen
assert len(d)-cs == 30+46+22+2*fnl, 'container overhead drifted'
print('overhead bytes:', len(d)-cs)
"
echo "lean container: OK"

# 10. P0 fixes: permissions, mtime, duplicates, self-overwrite, empty dirs, symlinks, atomicity
# permissions
echo "test perm" > $TMP/perm.txt; chmod 750 $TMP/perm.txt
./katzip $TMP/perm.zip $TMP/perm.txt
python3 -c "
import struct; d=open('$TMP/perm.zip','rb').read()
ci=d.find(b'PK\x01\x02'); ext=struct.unpack('<I', d[ci+38:ci+42])[0]
mode=(ext>>16)&0o777
assert mode==0o750, f'perm {oct(mode)} != 0o750'
"
echo "permissions: OK"
# mtime sync
touch -d "2001-02-03 04:05:06" $TMP/perm.txt
./katzip $TMP/mtime.zip $TMP/perm.txt
python3 -c "
import struct; d=open('$TMP/mtime.zip','rb').read()
ldos=struct.unpack('<HH', d[10:14]); ci=d.find(b'PK\x01\x02'); cdos=struct.unpack('<HH', d[ci+12:ci+16])
assert ldos==cdos, f'mtime desync {ldos} vs {cdos}'
"
echo "mtime: OK"
# duplicate
mkdir -p $TMP/d1 $TMP/d2; echo one > $TMP/d1/x.txt; echo two > $TMP/d2/x.txt
if ./katzip $TMP/dup.zip $TMP/d1/x.txt $TMP/d2/x.txt 2>/dev/null; then echo "FAIL dup should reject"; exit 1; fi
echo "duplicate: OK"
# self-overwrite
cp $TMP/perm.txt $TMP/self.zip
if ./katzip $TMP/self.zip $TMP/self.zip 2>/dev/null; then echo "FAIL self should reject"; exit 1; fi
test -f $TMP/self.zip
echo "self-overwrite: OK"
# empty dir
mkdir -p $TMP/emptydir
./katzip $TMP/empty_dir.zip $TMP/emptydir
python3 -c "import zipfile; z=zipfile.ZipFile('$TMP/empty_dir.zip'); assert any(n.endswith('/') for n in z.namelist()), 'empty dir not preserved'"
echo "empty dir: OK"
# symlink all filtered
ln -sf $TMP/perm.txt $TMP/link.txt
if ./katzip $TMP/symonly.zip $TMP/link.txt 2>/dev/null; then echo "FAIL symonly should reject"; exit 1; fi
echo "symlink filter: OK"
# atomicity: existing archive not deleted on failure (duplicate triggers fail)
echo "keep" > $TMP/keep_a.txt; ./katzip $TMP/keep.zip $TMP/keep_a.txt >/dev/null 2>&1
cp $TMP/keep.zip $TMP/keep_before.zip
mkdir -p $TMP/adup1 $TMP/adup2; echo a > $TMP/adup1/dup.txt; echo b > $TMP/adup2/dup.txt
if ./katzip $TMP/keep.zip $TMP/adup1/dup.txt $TMP/adup2/dup.txt 2>/dev/null; then echo "FAIL should fail on duplicate"; exit 1; fi
test -f $TMP/keep.zip
python3 -c "import zipfile; z=zipfile.ZipFile('$TMP/keep_before.zip'); assert z.namelist()==zipfile.ZipFile('$TMP/keep.zip').namelist()"
echo "atomic keep: OK"
# archive mode 0644
umask 0022; echo "mode" > $TMP/mode2.txt; ./katzip $TMP/mode2.zip $TMP/mode2.txt >/dev/null 2>&1
python3 -c "import os, stat; mode=oct(os.stat('$TMP/mode2.zip').st_mode & 0o777); assert mode=='0o644', f'mode {mode} != 0o644'"
echo "archive mode: OK"

echo "ALL C TESTS PASSED"
