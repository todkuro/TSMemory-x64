#!/usr/bin/env bash
#
# ビルドに使うコンパイラをプロジェクト内 (compilers/) に用意する。
# システムの MSVC / MinGW は一切使わず、ここに展開したものだけを使う。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPILERS="$ROOT/compilers"
TOOLCHAIN="$COMPILERS/llvm-mingw"

# llvm-mingw (clang + lld + mingw-w64, UCRT)
#
# Windows でも Linux でも「Windows 向けの物」を作る。Linux ではクロス
# コンパイルになるので、ホストに合わせて配布物を選ぶ。
VERSION="${LLVM_MINGW_VERSION:-20260616}"

case "$(uname -s)" in
	Linux)
		case "$(uname -m)" in
			aarch64|arm64) NAME="llvm-mingw-${VERSION}-ucrt-ubuntu-22.04-aarch64" ;;
			*)             NAME="llvm-mingw-${VERSION}-ucrt-ubuntu-22.04-x86_64" ;;
		esac
		ARCHIVE="${NAME}.tar.xz"
		;;
	Darwin)
		NAME="llvm-mingw-${VERSION}-ucrt-macos-universal"
		ARCHIVE="${NAME}.tar.xz"
		;;
	*)
		NAME="llvm-mingw-${VERSION}-ucrt-x86_64"
		ARCHIVE="${NAME}.zip"
		;;
esac

URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${VERSION}/${ARCHIVE}"

if [ -x "$TOOLCHAIN/bin/x86_64-w64-mingw32-clang.exe" ] \
	|| [ -x "$TOOLCHAIN/bin/x86_64-w64-mingw32-clang" ]; then
	echo "toolchain already present: $TOOLCHAIN"
	exit 0
fi

mkdir -p "$COMPILERS"
cd "$COMPILERS"

echo "downloading $URL"
curl -L --fail -o "$ARCHIVE" "$URL"

echo "extracting"
case "$ARCHIVE" in
	*.tar.xz)
		tar -xf "$ARCHIVE"
		;;
	*)
		# Git for Windows には unzip が入っていない事があるので PowerShell に退避する
		if command -v unzip >/dev/null 2>&1; then
			unzip -q "$ARCHIVE"
		elif command -v powershell >/dev/null 2>&1; then
			powershell -NoProfile -Command \
				"Expand-Archive -LiteralPath '$ARCHIVE' -DestinationPath . -Force"
		else
			echo "error: unzip も powershell も見つかりません。unzip を入れてください。" >&2
			exit 1
		fi
		;;
esac
mv "$NAME" "$TOOLCHAIN"
rm -f "$ARCHIVE"

#	Windows ホストでは接頭辞なしの clang も Windows を狙うが、Linux では
#	ホスト (ELF) を狙ってしまう。どちらでも同じ物を指す
#	x86_64-w64-mingw32-* を使う (tools/build.sh も同じ)
"$TOOLCHAIN/bin/x86_64-w64-mingw32-clang" --version
echo "toolchain ready: $TOOLCHAIN"
