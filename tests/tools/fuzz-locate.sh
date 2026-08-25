#!/usr/bin/env bash
#
# tests/test_fuzz.cpp が見つけたクラッシュの位置を関数名で出す。
#
#   bash tests/tools/fuzz-locate.sh <seed> <iteration> [ts-file]
#
# デバッグ情報付きの aux2 を build/prof/ に作り、指定の seed / iteration の
# 壊し方を再現して通し、落ちた番地と呼び出し元を記号化する。
#
# 落ちる seed / iteration の探し方:
#
#   cd build/tests
#   ./test_fuzz.exe ../../build/ts-examples/sample.ts \
#       ../../dist/TSMemory-TVTestSrc.aux2 9 <seed>
#
# 出力された行の次が落ちた iteration。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLCHAIN="$ROOT/compilers/llvm-mingw"
BUILD="$ROOT/build"
PROF="$BUILD/prof"

if [ $# -lt 2 ]; then
	echo "usage: bash tests/tools/fuzz-locate.sh <seed> <iteration> [ts-file]" >&2
	exit 1
fi

SEED="$1"
ITER="$2"
TS="${3:-$ROOT/build/ts-examples/sample.ts}"

if [ ! -f "$TS" ]; then
	echo "error: TS サンプルがありません: $TS" >&2
	exit 1
fi

export PATH="$TOOLCHAIN/bin:$PATH"
PY="$ROOT/compilers/python/python.exe"
if [ ! -x "$PY" ]; then
	echo "error: compilers/python がありません。bash tools/setup-python.sh を実行してください。" >&2
	exit 1
fi

#	m2v は変更が多いので毎回作り直す。aviutl2 側は変わっていなければ使い回す
echo "[1/3] デバッグ情報付きの aux2"
mkdir -p "$PROF/m2v" "$PROF/aviutl2"

for f in "$ROOT"/src/m2v/*.c; do
	b="$(basename "$f" .c)"
	case "$b" in m2v|mpeg2edit|idct_reference_sse) continue ;; esac
	if [ ! -f "$PROF/m2v/$b.o" ] || [ "$f" -nt "$PROF/m2v/$b.o" ]; then
		clang -c -O1 -g -m64 -fms-extensions -Wno-everything -DWIN32 -D_WINDOWS \
			-I"$ROOT/src/m2v" -o "$PROF/m2v/$b.o" "$f"
	fi
done
for b in input_tvtv bridge capture exitguard preset inifile plugin_main; do
	f="$ROOT/src/aviutl2/$b.cpp"
	if [ ! -f "$PROF/aviutl2/$b.o" ] || [ "$f" -nt "$PROF/aviutl2/$b.o" ]; then
		clang++ -c -O1 -g -m64 -fms-extensions -municode -std=c++17 -Wno-everything \
			-I"$ROOT/src/m2v" -I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" -I"$ROOT/src/aviutl2" \
			-o "$PROF/aviutl2/$b.o" "$f"
	fi
done
[ -f "$PROF/aviutl2/rc.o" ] || windres -I "$ROOT/src/m2v" -o "$PROF/aviutl2/rc.o" \
	"$ROOT/src/aviutl2/tsmemory.rc"

clang++ -shared -O1 -g -static -o "$PROF/TSMemory-prof.aux2" \
	"$PROF"/m2v/*.o "$PROF"/aviutl2/*.o -Wl,--error-limit=0 \
	-lshlwapi -lcomctl32 -lgdi32 -luser32 -lole32 -loleaut32 -lwindowscodecs -luuid

echo "[2/3] 再現"
mkdir -p "$BUILD/tests"
clang++ -O1 -static -std=c++17 -fms-extensions -Wno-everything \
	-I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" \
	-o "$BUILD/tests/fuzz-locate.exe" "$ROOT/tests/tools/fuzz-locate.cpp" -lshlwapi -luser32

(cd "$BUILD/tests" && ./fuzz-locate.exe "$TS" "$PROF/TSMemory-prof.aux2" "$SEED" "$ITER") \
	> "$BUILD/tests/locate.txt" 2>&1 || true
grep -vE "^(FAULT|CALLER) " "$BUILD/tests/locate.txt" | tail -12

echo
echo "[3/3] 記号化"
"$PY" - "$PROF/TSMemory-prof.aux2" "$BUILD/tests/locate.txt" <<'PYEOF'
import subprocess, io, sys, struct

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
obj, rep = sys.argv[1], sys.argv[2]

#	PE の ImageBase を足さないと llvm-symbolizer が解決出来ない
d = open(obj, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
opt = pe + 24
magic, = struct.unpack_from('<H', d, opt)
imagebase, = struct.unpack_from('<Q', d, opt + 24) if magic == 0x20b \
             else struct.unpack_from('<I', d, opt + 28)

rows = []
for line in open(rep, 'rb').read().decode('utf-8', 'replace').splitlines():
    a = line.split()
    if len(a) == 2 and a[0] in ('FAULT', 'CALLER'):
        rows.append((a[0], int(a[1], 16)))

if not rows:
    sys.exit('落ちませんでした (この seed / iteration では再現しません)')

r = subprocess.run(['llvm-symbolizer', '--obj=' + obj, '--functions=short', '--demangle'],
                   input='\n'.join('0x%x' % (imagebase + o) for _, o in rows),
                   capture_output=True, text=True)

for (kind, off), blk in zip(rows, r.stdout.split('\n\n')):
    ls = [x for x in blk.splitlines() if x.strip()]
    fn = ls[0].strip() if ls else '?'
    loc = ls[1].strip() if len(ls) > 1 else ''
    print('%-7s 0x%-8X %-30s %s' % (kind, off, fn[:30], loc))
PYEOF
