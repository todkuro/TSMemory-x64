#!/usr/bin/env bash
#
# sdk/aviutl2/ (AviUtl ExEdit2 プラグイン SDK のヘッダ) を更新する。
#
# **手動更新用の道具。ビルドからは呼ばれません。**
# sdk/ はバージョン管理対象で、更新すると API が変わる可能性がある為、
# 中身を確かめてから入れ替える。既定は差分を見せるだけです。
#
#   bash tools/update-aviutl2-sdk.sh           # 差分を見るだけ (既定)
#   bash tools/update-aviutl2-sdk.sh --apply   # 実際に入れ替える
#
# 入れ替えた後は必ず下記を通してください。
#   bash tools/build.sh && bash tests/tools/test.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BASE_URL="${AVIUTL2_BASE_URL:-https://spring-fragrance.mints.ne.jp/aviutl/}"
SDK_ZIP="${AVIUTL2_SDK_ZIP:-aviutl2_sdk.zip}"
SDK="$ROOT/sdk/aviutl2"

APPLY=0
[ "${1:-}" = "--apply" ] && APPLY=1

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "downloading $BASE_URL$SDK_ZIP"
curl -fsSL --max-time 300 "$BASE_URL$SDK_ZIP" -o "$TMP/sdk.zip"
unzip -q "$TMP/sdk.zip" -d "$TMP/sdk"

#	zip によっては 1 階層挟まる事がある
NEW="$TMP/sdk"
if [ ! -f "$NEW/plugin2.h" ]; then
	inner="$(find "$TMP/sdk" -name plugin2.h -print -quit)"
	[ -n "$inner" ] && NEW="$(dirname "$inner")"
fi

if [ ! -f "$NEW/plugin2.h" ]; then
	echo "error: plugin2.h が見つかりません。zip の構成が変わった可能性があります。" >&2
	exit 1
fi

#	取り込むのはヘッダとライセンスだけ。サンプルの .cpp や .obj2 は入れない
#	(ビルドに使わない物を版管理に入れない)
list_new() { (cd "$NEW" && ls *.h license.txt 2>/dev/null); }

echo
echo "=== 変更点 ==="
changed=0
for f in $(list_new); do
	if [ ! -f "$SDK/$f" ]; then
		echo "  + $f (新規)"
		changed=1
	elif ! cmp -s "$NEW/$f" "$SDK/$f"; then
		echo "  M $f"
		changed=1
	fi
done
for f in $(cd "$SDK" && ls 2>/dev/null); do
	if [ ! -f "$NEW/$f" ]; then
		echo "  - $f (配布物から消えています。要確認)"
		changed=1
	fi
done

if [ "$changed" = "0" ]; then
	echo "  (差分なし。sdk/aviutl2/ は最新です)"
	exit 0
fi

#	SDK のヘッダは Shift_JIS。端末へ出す時だけ UTF-8 に直す
#	(ファイル自体は変換しない。原典のまま sdk/ に入れる)
#	※ iconv は Git Bash に無いので compilers/python を使う。
#	   それも無ければ変換せずそのまま出す
PYTHON="$ROOT/compilers/python/python.exe"
if [ -x "$PYTHON" ]; then
	show() {
		"$PYTHON" -c 'import sys;sys.stdout.reconfigure(encoding="utf-8");
sys.stdout.write(sys.stdin.buffer.read().decode("cp932","replace"))'
	}
else
	show() { cat; }
fi

echo
echo "=== 内容 ==="
for f in $(list_new); do
	[ -f "$SDK/$f" ] || continue
	cmp -s "$NEW/$f" "$SDK/$f" || diff -u "$SDK/$f" "$NEW/$f" | show || true
done

if [ "$APPLY" = "0" ]; then
	echo
	echo "差分を見せただけです。入れ替えるには:"
	echo "  bash tools/update-aviutl2-sdk.sh --apply"
	exit 0
fi

echo
echo "updating $SDK"
for f in $(list_new); do
	cp "$NEW/$f" "$SDK/$f"
	echo "  $f"
done

echo
echo "入れ替えました。API が変わっている場合があるので必ず通してください。"
echo "  bash tools/build.sh && bash tests/tools/test.sh"
