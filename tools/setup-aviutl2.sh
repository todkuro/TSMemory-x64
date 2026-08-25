#!/usr/bin/env bash
#
# AviUtl ExEdit2 (aviutl2) 本体を third_party/aviutl2 に用意する。
#
# 実機での動作確認と tests/tools/test-live.sh でしか使わない。
# 通常のビルドと tests/tools/test.sh には不要。
#
#   bash tools/setup-aviutl2.sh
#
# 版を固定したい場合 (配布ページの zip 名をそのまま渡す):
#   AVIUTL2_ZIP=aviutl2_v2.1.5.zip bash tools/setup-aviutl2.sh
#   AVIUTL2_DEST=/tmp/aviutl2 bash tools/setup-aviutl2.sh
#
# ※ インストーラ (AviUtl2_*_setup.exe) は使いません。zip 版を展開する
#   だけなので、レジストリもシステムも触りません。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BASE_URL="${AVIUTL2_BASE_URL:-https://spring-fragrance.mints.ne.jp/aviutl/}"
DEST="${AVIUTL2_DEST:-$ROOT/third_party/aviutl2}"

if [ -f "$DEST/aviutl2.exe" ]; then
	echo "aviutl2 already present: $DEST"
	echo "  取得し直す場合はフォルダごと削除してください。"
	echo
	echo "AviUtlPath=$(cygpath -w "$DEST/aviutl2.exe" 2>/dev/null || echo "$DEST/aviutl2.exe")"
	exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

#	--- 版の特定 -------------------------------------------------------------
#	zip の名前に版が入る (aviutl2_v2.1.6a.zip) ので、配布ページから拾う。
#	本体の zip だけを対象にする。setup.exe と sdk は拾わない
ZIP="${AVIUTL2_ZIP:-}"
if [ -z "$ZIP" ]; then
	echo "fetching $BASE_URL"
	curl -fsSL --max-time 60 "$BASE_URL" -o "$TMP/index.html"
	ZIP="$(grep -oiE 'href="aviutl2_v[0-9][^"]*\.zip"' "$TMP/index.html" \
		| sed -E 's/.*"(.*)"/\1/' | head -1)"
	if [ -z "$ZIP" ]; then
		echo "error: 配布ページから aviutl2 本体の zip を見つけられませんでした。" >&2
		echo "  ページの作りが変わった可能性があります。" >&2
		echo "  AVIUTL2_ZIP=<zip 名> を指定して実行してください: $BASE_URL" >&2
		exit 1
	fi
fi

echo "downloading $ZIP"
curl -fsSL --max-time 300 "$BASE_URL$ZIP" -o "$TMP/aviutl2.zip"

echo "extracting"
rm -rf "$DEST"
mkdir -p "$DEST"
unzip -q "$TMP/aviutl2.zip" -d "$DEST"

#	zip によっては 1 階層挟まる事があるので均す
if [ ! -f "$DEST/aviutl2.exe" ]; then
	inner="$(find "$DEST" -name aviutl2.exe -print -quit)"
	if [ -n "$inner" ]; then
		d="$(dirname "$inner")"
		mv "$d"/* "$DEST"/ 2>/dev/null || true
	fi
fi

if [ ! -f "$DEST/aviutl2.exe" ]; then
	echo "error: aviutl2.exe が見つかりません: $DEST" >&2
	exit 1
fi

EXE="$(cygpath -w "$DEST/aviutl2.exe" 2>/dev/null || echo "$DEST/aviutl2.exe")"

echo
echo "aviutl2 ready: $DEST  ($ZIP)"
echo
echo "TVTest 側から起動する場合は TSMemory.ini に下記を設定してください。"
echo "  AviUtlPath=$EXE"
