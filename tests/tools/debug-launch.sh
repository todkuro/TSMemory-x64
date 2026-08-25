#!/usr/bin/env bash
#
# tests/tools/debug-launch.cpp をビルドして実行する。
#
#   bash tests/tools/debug-launch.sh <exe> [作業ディレクトリ]
#
# デバッガとして対象を起動し、例外の発生位置をモジュール名で報告する。
# アンロード済みモジュールの範囲も覚えているので、
# 「アンロードされた DLL のコードを実行していた」類の問題を特定出来る。
#
# 例) AviUtl2 の終了時クラッシュを調べる:
#   bash tests/tools/debug-launch.sh "D:/programs/Multimedia/AviUtl2/aviutl2.exe"
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLCHAIN="$ROOT/compilers/llvm-mingw"
BUILD="$ROOT/build"

if [ $# -lt 1 ]; then
	echo "usage: bash tests/tools/debug-launch.sh <exe> [作業ディレクトリ]" >&2
	exit 1
fi

export PATH="$TOOLCHAIN/bin:$PATH"
mkdir -p "$BUILD/tests/tools"

clang++ -O1 -static -municode -std=c++17 \
	-o "$BUILD/tests/tools/debug-launch.exe" "$ROOT/tests/tools/debug-launch.cpp"

exec "$BUILD/tests/tools/debug-launch.exe" "$@"
