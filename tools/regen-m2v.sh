#!/usr/bin/env bash
#
# src/m2v/ を原典から作り直し、64bit 化パッチを当て直す。
#
# 「パッチが冪等か」「当たったつもりで黙って飛んでいないか」を確かめる
# 唯一の確実な方法が、pristine なソースへの当て直し。
# 過去に 3 通りの当たらない不具合が見つかっている
# (docs/development.md の「適用済みの判定」を参照)。
#
#   bash tools/setup-tsmemory-src.sh   # 先に原典を取得しておく
#   bash tools/regen-m2v.sh
#
# 原典に無いファイル (simd_stub.c 等、こちらで足した物) は残す。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${TSMEMORY_SRC_DEST:-$ROOT/third_party/TSMemory}/TVTestSrc"
PYTHON="$ROOT/compilers/python/python.exe"

if [ ! -f "$SRC/pes.c" ]; then
	echo "error: 原典が見つかりません: $SRC" >&2
	echo "  bash tools/setup-tsmemory-src.sh を先に実行してください。" >&2
	exit 1
fi

if [ ! -x "$PYTHON" ]; then
	echo "error: python が見つかりません: $PYTHON" >&2
	echo "  bash tools/setup-python.sh を先に実行してください。" >&2
	exit 1
fi

echo "restoring src/m2v from $SRC"
n=0
for f in "$SRC"/*.[ch]; do
	b="$(basename "$f")"
	if [ -f "$ROOT/src/m2v/$b" ]; then
		cp "$f" "$ROOT/src/m2v/$b"
		n=$((n + 1))
	fi
done
echo "  $n files"

#	当たらなかったパッチは "!! not found" として出る。
#	見逃すと「直したつもりで直っていない」状態のまま進むので、
#	ここで失敗にする
echo
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
"$PYTHON" "$ROOT/tools/patch64.py" 2>&1 | tee "$LOG"

if grep -q '!! not found' "$LOG"; then
	echo
	echo "error: 当たらなかったパッチがあります (上の !! not found)。" >&2
	echo "  原典が変わったか、patch64.py の marker の指定が誤っています。" >&2
	exit 1
fi

echo
bash "$ROOT/tools/gen_simd_stub.sh"

echo
echo "src/m2v を作り直しました。続けて下記を通してください。"
echo "  bash tools/build.sh"
echo "  bash tests/tools/test.sh"
