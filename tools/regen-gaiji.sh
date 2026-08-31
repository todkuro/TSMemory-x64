#!/usr/bin/env bash
#
# ARIB STD-B24 の追加漢字・追加記号の対応表を
# src/aviutl2/caption/arib_gaiji.h に作り直す。
#
#   bash tools/setup-libaribcaption-src.sh   # 先に原典を取る
#   bash tools/regen-gaiji.sh
#
# **手動更新用の道具であり、ビルドからは呼ばない。**
# 生成物は commit されているので、クローン直後でもビルドとテストは通る。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ARIBCAPTION_SRC_DEST:-$ROOT/third_party/libaribcaption}"
PY="$ROOT/compilers/python/python.exe"

if [ ! -f "$SRC/src/decoder/b24_gaiji_table.hpp" ]; then
	echo "error: libaribcaption のソースがありません: $SRC" >&2
	echo "  bash tools/setup-libaribcaption-src.sh" >&2
	exit 1
fi
if [ ! -x "$PY" ]; then
	echo "error: compilers/python がありません" >&2
	echo "  bash tools/setup-python.sh" >&2
	exit 1
fi

"$PY" "$ROOT/tools/gen_gaiji.py" "$SRC" "$ROOT/src/aviutl2/caption/arib_gaiji.h"

echo
echo "できました: src/aviutl2/caption/arib_gaiji.h"
echo "  bash tools/build.sh && bash tests/tools/test.sh"
