#!/usr/bin/env bash
#
# TVTest のビルドに使うポータブル MSVC を compilers/msvc に用意する。
#
# システムに Visual Studio をインストールするのではなく、Microsoft の公式 CDN
# から MSVC と Windows SDK を取得してプロジェクト内に展開するだけなので
# インストーラもレジストリ変更も無い。消す時はフォルダごと削除すれば良い。
#
# ※ 取得には Microsoft のライセンス条項への同意が必要。
#    条項の URL は Microsoft のチャネルマニフェストから実行時に取得され、
#    portable-msvc.py が表示して同意を求める (Y/N)。
#    参考 (2026-08 時点):
#      Visual Studio Build Tools 2026
#        https://go.microsoft.com/fwlink/?LinkId=2327714
#      Visual Studio Build Tools 2022
#        https://go.microsoft.com/fwlink/?LinkId=2179911
#
#    確認を出さずに同意する場合 (CI 等):
#      TSMEMORY_ACCEPT_MSVC_LICENSE=1 bash tests/tools/setup-msvc.sh
#
# 取得には mmozeiko 氏の portable-msvc.py を使う。
#   https://gist.github.com/mmozeiko/7f3162ec2988e81e56d5c4e22cde9977
#
# ※ portable-msvc.py は第三者のコードで、ファイル中にライセンスの表示が
#   無い。**リポジトリには入れず、ここで取得する** (compilers/ 配下)。
#   他の外部の物と同じ扱い (LICENSE.md の「取得物」を参照)。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMPILERS="$ROOT/compilers"
# システムの Python は使わない (CLAUDE.md の方針)
PY="$ROOT/compilers/python/python.exe"
if [ ! -x "$PY" ]; then
	echo "error: compilers/python がありません。先に tools/setup-python.sh を実行してください。" >&2
	exit 1
fi

if [ -f "$COMPILERS/msvc/setup_x64.bat" ]; then
	echo "portable MSVC already present: $COMPILERS/msvc"
	exit 0
fi

mkdir -p "$COMPILERS"
cd "$COMPILERS"

# portable-msvc.py を用意する (リポジトリには入っていない)
PORTABLE="$COMPILERS/portable-msvc.py"
PORTABLE_URL="${PORTABLE_MSVC_URL:-https://gist.githubusercontent.com/mmozeiko/7f3162ec2988e81e56d5c4e22cde9977/raw/portable-msvc.py}"
if [ ! -f "$PORTABLE" ]; then
	echo "downloading portable-msvc.py"
	curl -fsSL --max-time 120 "$PORTABLE_URL" -o "$PORTABLE.part"
	# 取り違え・取得失敗をここで弾く
	if ! grep -q "accept-license" "$PORTABLE.part"; then
		rm -f "$PORTABLE.part"
		echo "error: portable-msvc.py の取得に失敗しました: $PORTABLE_URL" >&2
		exit 1
	fi
	mv "$PORTABLE.part" "$PORTABLE"
fi

# 既定では portable-msvc.py にライセンス条項の URL を表示させ、同意を求める。
# ここで --accept-license を無条件に渡すと、何に同意するのかが
# 利用者に見えないまま同意した事になってしまう。
ACCEPT=""
if [ "${TSMEMORY_ACCEPT_MSVC_LICENSE:-0}" = "1" ]; then
	echo "TSMEMORY_ACCEPT_MSVC_LICENSE=1 の為、ライセンス条項に同意済みとして進めます。"
	ACCEPT="--accept-license"
fi

"$PY" "$PORTABLE" $ACCEPT --host x64 --target x64

# ライセンスに同意しなかった場合 portable-msvc.py は何もせず終了する
if [ ! -f "$COMPILERS/msvc/setup_x64.bat" ]; then
	rm -rf "$COMPILERS/downloads"
	echo
	echo "ライセンスに同意されなかった為、MSVC の取得を中止しました。"
	echo "MSVC は TVTest 本体をビルドする時だけ必要です。"
	echo "TSMemory 自体のビルド (bash tools/build.sh) には不要です。"
	exit 1
fi

# ダウンロードした一時ファイルは残さない
rm -rf "$COMPILERS/downloads"

echo
cat "$COMPILERS/msvc/setup_x64.bat" | grep -E "VCToolsVersion|WindowsSDKVersion"
echo "portable MSVC ready: $COMPILERS/msvc"
