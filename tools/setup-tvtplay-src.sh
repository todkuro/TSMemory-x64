#!/usr/bin/env bash
#
# TvtPlay と BonDriver_Pipe のソースを third_party/TvtPlay に用意する。
#
# tests/tools/test-live.sh (TVTest を実際に走らせる通し確認) でしか使わない。
# 通常のビルドと tests/tools/test.sh には不要。
#
#   bash tools/setup-tvtplay-src.sh
#
# 取得先を変えたい場合:
#   TVTPLAY_REF=work-plus bash tools/setup-tvtplay-src.sh
#   TVTPLAY_DEST=/tmp/tvtplay bash tools/setup-tvtplay-src.sh
#
# ブランチについて:
#   master / work … 同じコミットを指している (既定)
#   work-plus     … 配布物の x64/plus/TvtPlay.tvtp に当たる派生
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TVTPLAY_URL="${TVTPLAY_URL:-https://github.com/xtne6f/TvtPlay.git}"
TVTPLAY_REF="${TVTPLAY_REF:-master}"
DEST="${TVTPLAY_DEST:-$ROOT/third_party/TvtPlay}"

if [ -d "$DEST/.git" ]; then
	echo "TvtPlay source already present: $DEST"
	echo "  取得し直す場合はフォルダごと削除してください。"
else
	echo "cloning $TVTPLAY_URL ($TVTPLAY_REF)"
	mkdir -p "$(dirname "$DEST")"
	rm -rf "$DEST"

	CLONE="git -c core.autocrlf=false -c core.eol=native clone --quiet"

	if ! $CLONE --depth 1 --branch "$TVTPLAY_REF" "$TVTPLAY_URL" "$DEST" 2>/dev/null; then
		echo "  (branch/tag として取得できなかったのでコミット指定として扱います)"
		$CLONE "$TVTPLAY_URL" "$DEST"
		git -C "$DEST" -c core.autocrlf=false checkout --quiet "$TVTPLAY_REF"
	fi
fi

for f in "$DEST/src/TvtPlay.cpp" "$DEST/BonDriver_Pipe_src/BonDriver_Pipe.cpp"; do
	if [ ! -f "$f" ]; then
		echo "error: 期待したソースがありません: $f" >&2
		exit 1
	fi
done

echo
echo "TvtPlay source ready: $DEST"
echo "  $(git -C "$DEST" rev-parse --short HEAD) ($TVTPLAY_REF)"
echo
echo "※ ソースのみです。TvtPlay.tvtp / BonDriver_Pipe.dll のビルドには"
echo "   MSVC が要ります (bash tests/tools/setup-msvc.sh)。"
echo "   配置先は docs/development.md の test-live.sh の項を参照してください。"
