#!/usr/bin/env bash
#
# 実物の ISDB-T サンプルストリームを build/ts-examples に取って来る。
#
#   bash tests/tools/fetch-ts-samples.sh
#
# **手動実行のみ。tests/tools/test.sh からは呼びません。**
# 合成した TS (gen-ts-examples) だけでは出ない実放送の癖
# (可変 GOP、フィールド picture、ARIB の記述子、字幕やデータ放送の PID 等)
# を通したい時に使います。置いた TS は test.sh が自動で拾います。
#
# 取得元:
#   https://www.erb.jp/2013/12/28/samplestream/
#     isdbt188.ts        188 バイト/パケット (PC のチューナ等で録画した形)
#     isdbt192bdav.m2ts  192 バイト/パケット (BD レコーダ等で録画した形)
#
# 合計 260MB 程あります。リポジトリには入りません (build/ は .gitignore)。
#
# 192 バイト版は各パケットの先頭に 4 バイトの TP_extra_header が付いた形
# なので、それを外して 188 バイトの TS に直してから置きます。TSMemory が
# TVTest から受け取るのは常に 188 バイトのパケットで、192 バイトのまま
# 扱う口が無い為です。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build"
DEST="${1:-$BUILD/ts-examples}"
PYTHON="$ROOT/compilers/python/python.exe"

BASE_URL="${ERB_BASE_URL:-https://www.erb.jp/labo/}"

if [ ! -x "$PYTHON" ]; then
	echo "error: python が見つかりません: $PYTHON" >&2
	echo "  bash tools/setup-python.sh を先に実行してください。" >&2
	exit 1
fi

mkdir -p "$DEST" "$BUILD/tmp"

#---------------------------------------------------------------------------
#	188 バイト版はそのまま置く
#---------------------------------------------------------------------------
if [ -f "$DEST/isdbt188.ts" ]; then
	echo "already present: $DEST/isdbt188.ts"
else
	echo "downloading isdbt188.ts (about 126 MB)"
	#	-C - : 途中で切れた時に続きから
	curl -fL -C - --retry 3 --max-time 1800 "$BASE_URL/isdbt188.ts" \
		-o "$DEST/isdbt188.ts.part"
	mv "$DEST/isdbt188.ts.part" "$DEST/isdbt188.ts"
fi

#---------------------------------------------------------------------------
#	192 バイト版は 188 に直してから置く
#---------------------------------------------------------------------------
if [ -f "$DEST/isdbt192bdav.ts" ]; then
	echo "already present: $DEST/isdbt192bdav.ts"
else
	M2TS="$BUILD/tmp/isdbt192bdav.m2ts"
	if [ ! -f "$M2TS" ]; then
		echo "downloading isdbt192bdav.m2ts (about 130 MB)"
		curl -fL -C - --retry 3 --max-time 1800 "$BASE_URL/isdbt192bdav.m2ts" \
			-o "$M2TS.part"
		mv "$M2TS.part" "$M2TS"
	fi

	echo "converting 192 -> 188 bytes per packet"
	"$PYTHON" "$ROOT/tests/tools/m2ts2ts.py" "$M2TS" "$DEST/isdbt192bdav.ts"

	#	元の 192 バイト版は使い道が無いので消す (要れば再取得できる)
	rm -f "$M2TS"
fi

echo
echo "TS in $DEST :"
ls -la "$DEST"/*.ts
echo
echo "そのまま bash tests/tools/test.sh を走らせると、合成した TS と"
echo "一緒にここに在る TS も全て検査されます。"
