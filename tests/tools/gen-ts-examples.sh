#!/usr/bin/env bash
#
# テスト用の TS を合成する (tests/tools/gen-ts-examples.cpp)。
#
#   bash tests/tools/gen-ts-examples.sh [出力先] [秒数]
#
# 既定の出力先は build/ts-examples。tests/tools/test.sh は、そこに *.ts が
# 1 つも無い時にこれを呼ぶ。**既に置いてある物は消さない**ので、実際の
# 放送 TS を足しておけばそちらも一緒にテストされる。
#
# 作り直したい時はディレクトリごと消してから実行してください。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLCHAIN="$ROOT/compilers/llvm-mingw"
BUILD="$ROOT/build"

DEST="${1:-$BUILD/ts-examples}"
SECONDS_ARG="${2:-8}"

if [ ! -d "$TOOLCHAIN/bin" ]; then
	echo "error: toolchain not found. run tools/setup-toolchain.sh first." >&2
	exit 1
fi
export PATH="$TOOLCHAIN/bin:$PATH"

mkdir -p "$BUILD/tests" "$DEST"

clang++ -O2 -static -std=c++17 -fms-extensions \
	-o "$BUILD/tests/gen-ts-examples.exe" "$ROOT/tests/tools/gen-ts-examples.cpp" \
	-lmfplat -lmfuuid -lole32 -loleaut32

"$BUILD/tests/gen-ts-examples.exe" "$DEST" "$SECONDS_ARG"
