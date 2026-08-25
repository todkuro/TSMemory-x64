#!/usr/bin/env bash
#
# デコードのホットスポットを測る。
#
#   bash tests/tools/m2v-profile.sh <ts-file> [frames]
#
# デバッグ情報付きの aux2 を build/prof/ に作り、実際の入力プラグイン経由で
# デコードしながら全スレッドの RIP を採取して、関数別の割合を出す。
#
# 「どこが遅いか」を推測せずに決める為の物。実際、当初は IDCT や動き補償が
# 重いと考えていたが、測ると resize.c の component_resize が 7 割超だった。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLCHAIN="$ROOT/compilers/llvm-mingw"
BUILD="$ROOT/build"
PROF="$BUILD/prof"

TS="${1:-$ROOT/build/ts-examples/sample.ts}"
FRAMES="${2:-60}"

if [ ! -f "$TS" ]; then
	echo "usage: bash tests/tools/m2v-profile.sh <ts-file> [frames]" >&2
	echo "  TS サンプルがありません: $TS" >&2
	exit 1
fi

export PATH="$TOOLCHAIN/bin:$PATH"
PY="$ROOT/compilers/python/python.exe"
if [ ! -x "$PY" ]; then
	echo "error: compilers/python がありません。bash tools/setup-python.sh を実行してください。" >&2
	exit 1
fi

echo "[1/3] デバッグ情報付きの aux2 をビルド"
rm -rf "$PROF"
mkdir -p "$PROF/m2v" "$PROF/aviutl2"

for f in "$ROOT"/src/m2v/*.c; do
	b="$(basename "$f" .c)"
	case "$b" in m2v|mpeg2edit|idct_reference_sse) continue ;; esac
	clang -c -O2 -g -m64 -fms-extensions -Wno-everything -DWIN32 -D_WINDOWS \
		-I"$ROOT/src/m2v" -o "$PROF/m2v/$b.o" "$f"
done
for b in input_tvtv bridge capture exitguard preset inifile plugin_main; do
	clang++ -c -O2 -g -m64 -fms-extensions -municode -std=c++17 -Wno-everything \
		-I"$ROOT/src/m2v" -I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" -I"$ROOT/src/aviutl2" \
		-o "$PROF/aviutl2/$b.o" "$ROOT/src/aviutl2/$b.cpp"
done
windres -I "$ROOT/src/m2v" -o "$PROF/aviutl2/rc.o" "$ROOT/src/aviutl2/tsmemory.rc"
clang++ -shared -O2 -g -static -o "$PROF/TSMemory-prof.aux2" \
	"$PROF"/m2v/*.o "$PROF"/aviutl2/*.o -Wl,--error-limit=0 \
	-lshlwapi -lcomctl32 -lgdi32 -luser32 -lole32 -loleaut32 -lwindowscodecs -luuid

echo "[2/3] サンプリング"
clang++ -O2 -static -std=c++17 -fms-extensions -Wno-everything \
	-I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" \
	-o "$BUILD/tests/m2v-profile.exe" "$ROOT/tests/tools/m2v-profile.cpp" -luser32
"$BUILD/tests/m2v-profile.exe" "$TS" "$PROF/TSMemory-prof.aux2" "$FRAMES" \
	> "$BUILD/tests/prof.txt" 2>&1 || true
grep -E "decoded|samples" "$BUILD/tests/prof.txt" | tr -d '\000' || true

echo "[3/3] 記号化"
"$PY" - "$PROF/TSMemory-prof.aux2" "$BUILD/tests/prof.txt" <<'PYEOF'
import subprocess, collections, io, sys, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

obj, prof = sys.argv[1], sys.argv[2]

#	PE の ImageBase を足さないと llvm-symbolizer が解決出来ない
d = open(obj, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
opt = pe + 24
magic, = struct.unpack_from('<H', d, opt)
imagebase, = struct.unpack_from('<Q', d, opt + 24) if magic == 0x20b \
             else struct.unpack_from('<I', d, opt + 28)

lines = open(prof, 'rb').read().decode('utf-8', 'replace').splitlines()
head = [n for n, l in enumerate(lines) if l.startswith('--- RIP')]
if not head:
    sys.exit('プロファイルが取れていません')
pairs = [(int(a[0]), int(a[1])) for a in (l.split() for l in lines[head[0] + 1:])
         if len(a) == 2 and a[0].isdigit()]

r = subprocess.run(['llvm-symbolizer', '--obj=' + obj, '--functions=short', '--demangle'],
                   input='\n'.join('0x%x' % (imagebase + o) for o, _ in pairs),
                   capture_output=True, text=True)
agg = collections.Counter()
for (o, c), blk in zip(pairs, r.stdout.split('\n\n')):
    ls = [x for x in blk.splitlines() if x.strip()]
    agg[ls[0].strip() if ls else '?'] += c

tot = sum(agg.values()) or 1
print()
print('%-34s %8s %7s' % ('関数', 'サンプル', '割合'))
print('-' * 54)
for fn, c in agg.most_common(15):
    print('%-34s %8d %6.1f%%' % (fn[:34], c, 100.0 * c / tot))
PYEOF
