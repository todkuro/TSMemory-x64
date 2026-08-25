#!/usr/bin/env bash
#
# TVTest で実際に TS を再生しながらの通し確認。
#
#   TvtPlay + BonDriver_Pipe で build/ts-examples/sample.ts を再生
#     -> TSMemory.tvtp が共有メモリに取り込む
#     -> ホットキーで実行 (スナップショット作成 + 連携要求)
#     -> AviUtl2 のふりをした tests/test_receiver.cpp が受け取ってデコード
#
# 事前に tools/build.sh と tests/tools/build-tvtest.py が済んでいる事。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLCHAIN="$ROOT/compilers/llvm-mingw"
TVTEST="$ROOT/dist/TVTest-x64"
BUILD="$ROOT/build/tests"
TS="${TSMEMORY_TS_SAMPLE:-$ROOT/build/ts-examples/sample.ts}"

export PATH="$TOOLCHAIN/bin:$PATH"

for f in "$TVTEST/TVTest.exe" "$TVTEST/BonDriver_Pipe.dll" \
         "$TVTEST/Plugins/TvtPlay.tvtp" "$TVTEST/Plugins/TSMemory.tvtp" "$TS"; do
	if [ ! -f "$f" ]; then
		echo "error: $f not found" >&2
		exit 1
	fi
done

mkdir -p "$BUILD/plugin"

# 受け側は自前で待ち受けオブジェクトを作るので、aux2 側の待ち受けは止めておく
cp "$ROOT/dist/TSMemory-TVTestSrc.aux2" "$BUILD/plugin/"
printf '[Bridge]\r\nEnable=0\r\n[Capture]\r\nEnable=0\r\n[M2V]\r\naspect_ratio=1\r\n' \
	> "$BUILD/plugin/TSMemory-TVTestSrc.ini"

echo "building test_receiver"
clang++ -O1 -static -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	-I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" -I"$ROOT/tests" \
	-o "$BUILD/test_receiver.exe" "$ROOT/tests/test_receiver.cpp" -luser32 -lshlwapi

echo
powershell -NoProfile -ExecutionPolicy Bypass -File "$ROOT/tests/tools/test-live.ps1" \
	-TVTestDir "$TVTEST" -TsFile "$TS" -BuildDir "$BUILD" -BufferSeconds "${TSMEMORY_BUFFER_SECONDS:-20}"
