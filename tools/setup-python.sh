#!/usr/bin/env bash
#
# 補助スクリプト用の Python をプロジェクト内 (compilers/) に用意する。
# システムにインストールされた Python は使わない。
#
#   tools/zipdir.py       配布用 zip の作成
#   tools/patch64.py      src/m2v の 64bit 化パッチ
#   tests/tools/build-tvtest.py TVTest 本体のビルド (任意)
#   その他の解析ツール
#
# Windows では python.org の embeddable package を展開する。pip は入らないが、
# 本プロジェクトのスクリプトは標準ライブラリしか使わない。
#
# Windows 以外では PATH にある python3 を使う。python.org が可搬な
# Linux 版を配っていない為で、compilers/ には何も置かない。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPILERS="$ROOT/compilers"
PYDIR="$COMPILERS/python"

#	compilers/python の中の実体を返す (Windows は python.exe、
#	それ以外は bin/python3)。tools/build.sh も同じ探し方をする
find_python() {
	if [ -x "$PYDIR/python.exe" ]; then
		printf '%s' "$PYDIR/python.exe"
	elif [ -x "$PYDIR/bin/python3" ]; then
		printf '%s' "$PYDIR/bin/python3"
	fi
}

PY="$(find_python)"
if [ -n "$PY" ]; then
	echo "python already present: $PYDIR"
	"$PY" --version
	exit 0
fi

mkdir -p "$PYDIR"
cd "$COMPILERS"

case "$(uname -s)" in
	Linux|Darwin)
		#	python.org は可搬な Linux 版を配っていない。
		#	Windows 以外では PATH にある python3 を使って構わない
		#	(そのぶん compilers/ には何も置かない)
		if command -v python3 >/dev/null 2>&1; then
			echo "PATH の python3 を使います: $(command -v python3)"
			python3 --version
			exit 0
		fi
		echo "error: python3 が見つかりません。" >&2
		echo "  ディストリビューションの python3 を入れてください。" >&2
		echo "  (Windows 以外では compilers/ に Python を展開しません)" >&2
		exit 1
		;;
	*)
		VERSION="${PYTHON_VERSION:-3.12.10}"
		ARCHIVE="python-${VERSION}-embed-amd64.zip"
		URL="https://www.python.org/ftp/python/${VERSION}/${ARCHIVE}"

		echo "downloading $URL"
		curl -L --fail -o "$ARCHIVE" "$URL"

		echo "extracting"
		# Git for Windows には unzip が入っていない事があるので PowerShell に退避する
		if command -v unzip >/dev/null 2>&1; then
			unzip -q -o "$ARCHIVE" -d "$PYDIR"
		elif command -v powershell >/dev/null 2>&1; then
			powershell -NoProfile -Command \
				"Expand-Archive -LiteralPath '$ARCHIVE' -DestinationPath 'python' -Force"
		else
			echo "error: unzip も powershell も見つかりません。unzip を入れてください。" >&2
			exit 1
		fi
		rm -f "$ARCHIVE"
		;;
esac

PY="$(find_python)"
if [ -z "$PY" ]; then
	echo "error: python が見つかりません: $PYDIR" >&2
	exit 1
fi

"$PY" --version
echo "python ready: $PY"
