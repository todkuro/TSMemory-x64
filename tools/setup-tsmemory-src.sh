#!/usr/bin/env bash
#
# オリジナルの TSMemory / TVTestSrc (MPEG-2 VIDEO VFAPI Plug-In) のソースを
# third_party/TSMemory に用意する。
#
# src/m2v/ は 64bit 化済みの物が commit されている為、**ビルドとテストには
# 不要**。tools/patch64.py を pristine なソースへ当て直して検証する時
# (docs/development.md の「適用済みの判定」を参照) だけ必要になる。
#
#   bash tools/setup-tsmemory-src.sh
#
# 取得先を変えたい場合:
#   TSMEMORY_SRC_REF=<tag/commit> bash tools/setup-tsmemory-src.sh
#   TSMEMORY_SRC_DEST=/tmp/tsmemory bash tools/setup-tsmemory-src.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TSMEMORY_SRC_URL="${TSMEMORY_SRC_URL:-https://github.com/dtvgit/TSMemory.git}"
TSMEMORY_SRC_REF="${TSMEMORY_SRC_REF:-master}"
DEST="${TSMEMORY_SRC_DEST:-$ROOT/third_party/TSMemory}"

if [ -d "$DEST/.git" ]; then
	echo "TSMemory source already present: $DEST"
	echo "  取得し直す場合はフォルダごと削除してください。"
else
	echo "cloning $TSMEMORY_SRC_URL ($TSMEMORY_SRC_REF)"
	mkdir -p "$(dirname "$DEST")"
	rm -rf "$DEST"

	#	--- core.autocrlf=false は必須 ---------------------------------------
	#	このリポジトリのソースは殆どが CRLF だが、idct_reference.c /
	#	instance_manager.c / plugin.cpp の 3 つだけ LF で入っている。
	#	Windows の既定 (core.autocrlf=true) で clone すると、この 3 つが
	#	CRLF に変換されて配布物と別の中身になる。
	#	patch64.py は LF / CRLF の両方を試すので当たりはするが、
	#	src/m2v/ を作り直した時に commit 済みの物と全行が差分になり、
	#	「パッチの結果が変わっていないか」を diff で見られなくなる。
	CLONE="git -c core.autocrlf=false -c core.eol=native clone --quiet"

	#	--branch はタグにも使えるが、コミット指定には使えないので
	#	失敗したら clone してから checkout する
	if ! $CLONE --depth 1 --branch "$TSMEMORY_SRC_REF" \
			"$TSMEMORY_SRC_URL" "$DEST" 2>/dev/null; then
		echo "  (branch/tag として取得できなかったのでコミット指定として扱います)"
		$CLONE "$TSMEMORY_SRC_URL" "$DEST"
		git -C "$DEST" -c core.autocrlf=false checkout --quiet "$TSMEMORY_SRC_REF"
	fi
fi

if [ ! -f "$DEST/TVTestSrc/pes.c" ]; then
	echo "error: TVTestSrc が見つかりません: $DEST/TVTestSrc" >&2
	exit 1
fi

echo
echo "TSMemory source ready: $DEST"
echo "  $(git -C "$DEST" rev-parse --short HEAD) ($TSMEMORY_SRC_REF)"
echo
echo "src/m2v/ を pristine から作り直してパッチを当て直す場合:"
echo "  bash tools/regen-m2v.sh"
