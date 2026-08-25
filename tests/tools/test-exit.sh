#!/usr/bin/env bash
#
# AviUtl2 の「取り込み後に終了するとクラッシュする」問題の回帰確認。
#
#   bash tests/tools/test-exit.sh <aviutl2.exe のパス> [ts-file] [回数]
#
# AviUtl2 を実際に起動し、TVTest を模擬して取り込み要求を送り、
# 閉じて終了コードを見る。0 以外ならクラッシュしている。
#
# ※ 画面に AviUtl2 のウィンドウが出ます。TVTest は不要です。
# ※ プラグイン (TSMemory-TVTestSrc.aux2) は導入済みである必要があります。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLCHAIN="$ROOT/compilers/llvm-mingw"
BUILD="$ROOT/build"

AVIUTL="${1:-}"
TS="${2:-$ROOT/build/ts-examples/sample.ts}"
RUNS="${3:-3}"

if [ -z "$AVIUTL" ] || [ ! -f "$AVIUTL" ]; then
	echo "usage: bash tests/tools/test-exit.sh <aviutl2.exe のパス> [ts-file] [回数]" >&2
	exit 1
fi
if [ ! -f "$TS" ]; then
	echo "error: TS サンプルがありません: $TS" >&2
	exit 1
fi

export PATH="$TOOLCHAIN/bin:$PATH"
mkdir -p "$BUILD/tests/live"

echo "building tvtest_sim"
clang++ -O1 -static -municode -std=c++17 -I"$ROOT/src/common" \
	-o "$BUILD/tests/tvtest_sim.exe" "$ROOT/tests/tvtest_sim.cpp" -luser32

# Windows 形式のパスに直す (PowerShell に渡す為)
winpath() { printf '%s' "$(cd "$(dirname "$1")" && pwd -W 2>/dev/null || pwd)/$(basename "$1")" | tr '/' '\\'; }

AVIUTL_W="$(winpath "$AVIUTL")"
AVIUTL_DIR_W="$(dirname "$AVIUTL_W")"
TS_W="$(winpath "$TS")"
SIM_W="$(winpath "$BUILD/tests/tvtest_sim.exe")"
OUT_W="$(winpath "$BUILD/tests/live")"

echo "aviutl2 : $AVIUTL_W"
echo "ts      : $TS_W"
echo

fail=0
for i in $(seq 1 "$RUNS"); do
	code=$(powershell -NoProfile -Command "
		\$p = Start-Process -FilePath '$AVIUTL_W' -WorkingDirectory '$AVIUTL_DIR_W' -PassThru
		Start-Sleep -Seconds 8
		\$s = Start-Process -FilePath '$SIM_W' -ArgumentList '$TS_W','$OUT_W\\tsmemexit$i.tvtv' -PassThru -WindowStyle Hidden
		Start-Sleep -Seconds 18
		\$p.CloseMainWindow() | Out-Null
		Start-Sleep -Seconds 12
		if (\$p.HasExited) { '{0}' -f \$p.ExitCode } else { \$p.Kill(); 'TIMEOUT' }
		if (-not \$s.HasExited) { \$s.Kill() }
	" | tr -d '\r')

	if [ "$code" = "0" ]; then
		echo "  run $i : exit code 0                                   ok"
	else
		echo "  run $i : exit code $code                               FAILED"
		fail=$((fail + 1))
	fi
	sleep 2
done

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS (0 failures)"
else
	echo "FAIL ($fail failures)"
	exit 1
fi
