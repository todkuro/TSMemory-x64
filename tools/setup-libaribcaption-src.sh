#!/usr/bin/env bash
#
# libaribcaption (xqq) のソースを third_party/libaribcaption に用意する。
#
# ARIB STD-B24 の**追加記号の対応表**を借りる為だけに使う。
# src/aviutl2/caption/arib_gaiji.h は生成済みの物が commit されている為、
# **ビルドとテストには不要**。表を作り直す時
# (tools/regen-gaiji.sh) だけ必要になる。
#
#   bash tools/setup-libaribcaption-src.sh
#
# 取得先を変えたい場合:
#   ARIBCAPTION_SRC_REF=<tag/commit> bash tools/setup-libaribcaption-src.sh
#   ARIBCAPTION_SRC_DEST=/tmp/aribcaption bash tools/setup-libaribcaption-src.sh
#
# ライセンス:
#   MIT (リポジトリ) / 各ファイルの冒頭は ISC 形式の許諾文。
#   借りるのは表だけで、著作権表示を生成物に残している
#   (src/aviutl2/caption/arib_gaiji.h の冒頭を参照)。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ARIBCAPTION_SRC_URL="${ARIBCAPTION_SRC_URL:-https://github.com/xqq/libaribcaption.git}"
ARIBCAPTION_SRC_REF="${ARIBCAPTION_SRC_REF:-master}"
DEST="${ARIBCAPTION_SRC_DEST:-$ROOT/third_party/libaribcaption}"

if [ -d "$DEST/.git" ]; then
	echo "libaribcaption source already present: $DEST"
	echo "  取得し直す場合はフォルダごと削除してください。"
else
	echo "cloning $ARIBCAPTION_SRC_URL ($ARIBCAPTION_SRC_REF)"
	mkdir -p "$(dirname "$DEST")"
	rm -rf "$DEST"

	CLONE="git -c core.autocrlf=false -c core.eol=native clone --quiet"

	if ! $CLONE --depth 1 --branch "$ARIBCAPTION_SRC_REF" \
			"$ARIBCAPTION_SRC_URL" "$DEST" 2>/dev/null; then
		echo "  (branch/tag として取得できなかったのでコミット指定として扱います)"
		$CLONE "$ARIBCAPTION_SRC_URL" "$DEST"
		git -C "$DEST" -c core.autocrlf=false checkout --quiet "$ARIBCAPTION_SRC_REF"
	fi
fi

for f in src/decoder/b24_gaiji_table.hpp src/decoder/b24_conv_tables.hpp LICENSE; do
	if [ ! -f "$DEST/$f" ]; then
		echo "error: $f が見つかりません: $DEST/$f" >&2
		exit 1
	fi
done

echo
echo "libaribcaption source ready: $DEST"
echo "  $(git -C "$DEST" rev-parse --short HEAD) ($ARIBCAPTION_SRC_REF)"
echo
echo "追加記号の表を作り直す場合:"
echo "  bash tools/regen-gaiji.sh"
