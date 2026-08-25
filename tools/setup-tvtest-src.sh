#!/usr/bin/env bash
#
# TVTest のビルド用ソースツリーを third_party/TVTest に用意する。
#
# 本家 (DBCTRADO/TVTest) の develop ブランチを submodule ごと取得する。
# TVTest 本体のビルドは任意 (動作確認用の 64bit TVTest を作る時だけ必要)。
#
#   bash tools/setup-tvtest-src.sh
#
# 取得先を変えたい場合:
#   TVTEST_REF=v0.10.0 bash tools/setup-tvtest-src.sh   # タグ・コミットも可
#   TVTEST_DEST=/tmp/tvtest bash tools/setup-tvtest-src.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TVTEST_URL="${TVTEST_URL:-https://github.com/DBCTRADO/TVTest.git}"
TVTEST_REF="${TVTEST_REF:-develop}"
DEST="${TVTEST_DEST:-$ROOT/third_party/TVTest}"

# LibISDB (submodule) が更に持つ fdk-aac。submodule 定義に無い場合に備えて
# 直接取得できるようにしておく。
FDKAAC_URL="${FDKAAC_URL:-https://github.com/mstorsjo/fdk-aac.git}"

if [ -d "$DEST/.git" ]; then
	echo "TVTest source already present: $DEST"
	echo "  取得し直す場合はフォルダごと削除してください。"
else
	echo "cloning $TVTEST_URL ($TVTEST_REF)"
	mkdir -p "$(dirname "$DEST")"
	rm -rf "$DEST"

	#	submodule (LibISDB) まで一度に取得する。
	#	--branch はタグにも使えるが、コミット指定には使えないので
	#	失敗したら clone してから checkout する。
	if ! git clone --quiet --branch "$TVTEST_REF" --recurse-submodules \
			"$TVTEST_URL" "$DEST" 2>/dev/null; then
		echo "  (branch/tag として取得できなかったのでコミット指定として扱います)"
		git clone --quiet --recurse-submodules "$TVTEST_URL" "$DEST"
		git -C "$DEST" checkout --quiet "$TVTEST_REF"
		git -C "$DEST" submodule update --quiet --init --recursive
	fi
fi

LIBISDB="$DEST/src/LibISDB"
if [ ! -f "$LIBISDB/LibISDB/LibISDB.hpp" ]; then
	echo "error: LibISDB が取得できていません: $LIBISDB" >&2
	echo "  git -C \"$DEST\" submodule update --init --recursive を試してください。" >&2
	exit 1
fi

#	fdk-aac は LibISDB の submodule だが、入っていない事があるので補う
FDKAAC="$LIBISDB/Thirdparty/fdk-aac"
if [ ! -f "$FDKAAC/libSYS/include/machine_type.h" ]; then
	echo "cloning fdk-aac"
	rm -rf "$FDKAAC"
	git clone --quiet --depth 1 "$FDKAAC_URL" "$FDKAAC"
fi

echo
echo "TVTest source ready: $DEST"
echo "  TVTest  : $(git -C "$DEST" rev-parse --short HEAD) ($(git -C "$DEST" rev-parse --abbrev-ref HEAD))"
echo "  LibISDB : $(git -C "$LIBISDB" rev-parse --short HEAD 2>/dev/null || echo '(not a git tree)')"
echo
echo "続けて 64bit の TVTest をビルドする場合:"
echo "  bash tests/tools/setup-msvc.sh"
echo "  compilers/python/python.exe tests/tools/build-tvtest.py"
