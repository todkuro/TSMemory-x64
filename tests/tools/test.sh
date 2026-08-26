#!/usr/bin/env bash
#
# ビルド済みのバイナリに対する動作確認
#   tests/test_shm.c      共有メモリ読み出し層 (64bit 化した部分)
#   tests/test_plugin.cpp AviUtl ExEdit2 のふりをして TSMemory-TVTestSrc.aux2 を読み込む
#   tests/test_selector.cpp 合成した 2 サービスの TS でサービス選択を確認
#   tests/test_multich.cpp  マルチ編成 TS でサブチャンネルをデコード
#   tests/test_decode.cpp MPEG-2 TS をデコードさせる
#
# TS を使うテストは build/ts-examples/*.ts を対象にする。
# 無ければ tests/tools/gen-ts-examples.cpp が合成する (放送 TS は
# 再配布出来ない為)。**そこに置いた TS は全て対象になる**ので、実際の
# 放送 TS があれば足すと、合成では出ない癖まで見られる。
#
#   TSMEMORY_TS_DIR=/path/to/ts bash tests/tools/test.sh   # 置き場所を変える
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLCHAIN="$ROOT/compilers/llvm-mingw"
BUILD="$ROOT/build"

export PATH="$TOOLCHAIN/bin:$PATH"
mkdir -p "$BUILD/tests"

echo "=== test_shm ==="
clang -O1 -m64 -fms-extensions -Wno-everything -I"$ROOT/src/m2v" \
	-o "$BUILD/tests/test_shm.exe" "$ROOT/tests/test_shm.c" \
	"$BUILD/m2v/shared_memory.o" "$BUILD/m2v/multi_file.o" \
	"$BUILD/m2v/registry.o" "$BUILD/m2v/instance_manager.o" "$BUILD/m2v/filename.o" \
	-luser32
"$BUILD/tests/test_shm.exe"

echo
echo "=== test_plugin ==="
clang++ -O1 -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	-I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" -I"$ROOT/tests" \
	-o "$BUILD/tests/test_plugin.exe" "$ROOT/tests/test_plugin.cpp" -luser32 -lshlwapi
"$BUILD/tests/test_plugin.exe" "$ROOT/dist/TSMemory-TVTestSrc.aux2"

# 終了時の確認の自動応答を有効にした場合も同じ様に動く事
echo
echo "=== test_plugin (SuppressExitConfirm=1) ==="
mkdir -p "$BUILD/tests/guard"
cp "$ROOT/dist/TSMemory-TVTestSrc.aux2" "$BUILD/tests/guard/"
printf '[Bridge]\r\nEnable=1\r\nSuppressExitConfirm=1\r\n[Capture]\r\nEnable=1\r\n' \
	> "$BUILD/tests/guard/TSMemory-TVTestSrc.ini"
"$BUILD/tests/test_plugin.exe" "$BUILD/tests/guard/TSMemory-TVTestSrc.aux2"

# =2 は編集していても応答する (編集の監視はしない)
echo
echo "=== test_plugin (SuppressExitConfirm=2) ==="
mkdir -p "$BUILD/tests/guard2"
cp "$ROOT/dist/TSMemory-TVTestSrc.aux2" "$BUILD/tests/guard2/"
printf '[Bridge]\r\nEnable=1\r\nSuppressExitConfirm=2\r\n[Capture]\r\nEnable=1\r\n' \
	> "$BUILD/tests/guard2/TSMemory-TVTestSrc.ini"
"$BUILD/tests/test_plugin.exe" "$BUILD/tests/guard2/TSMemory-TVTestSrc.aux2"

echo
echo "=== test_save ==="
mkdir -p "$BUILD/tests/save"
cp "$ROOT/dist/TSMemory-TVTestSrc.aux2" "$BUILD/tests/save/"
printf '[Bridge]\r\nEnable=0\r\n[Capture]\r\nEnable=1\r\n' > "$BUILD/tests/save/TSMemory-TVTestSrc.ini"
clang++ -O1 -static -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	-I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" -I"$ROOT/tests" \
	-o "$BUILD/tests/test_save.exe" "$ROOT/tests/test_save.cpp" \
	-lole32 -loleaut32 -lwindowscodecs -luuid -lshlwapi -luser32
"$BUILD/tests/test_save.exe" "$BUILD/tests/save/TSMemory-TVTestSrc.aux2" "$BUILD/tests/save"

# BonTsEngine のうち CTsSelector に必要な部分 (TSMemory.tvtp と同じ物を使う)
BONTS_OBJS=(
	"$BUILD/tvtp/TsSelector.o" "$BUILD/tvtp/TsTable.o" "$BUILD/tvtp/TsStream.o"
	"$BUILD/tvtp/TsDescriptor.o" "$BUILD/tvtp/TsEncode.o" "$BUILD/tvtp/MediaData.o"
	"$BUILD/tvtp/MediaDecoder.o" "$BUILD/tvtp/BonBaseClass.o" "$BUILD/tvtp/Exception.o"
	"$BUILD/tvtp/TsUtilClass.o" "$BUILD/tvtp/StdUtil.o"
)

echo
echo "=== test_inifile ==="
clang++ -O1 -static -std=c++17 -fms-extensions -I"$ROOT/src/aviutl2" \
	-o "$BUILD/tests/test_inifile.exe" "$ROOT/tests/test_inifile.cpp" \
	"$ROOT/src/aviutl2/inifile.cpp" -lshlwapi -luser32
"$BUILD/tests/test_inifile.exe" "$BUILD/tests/ini"

echo
echo "=== test_preset ==="
# preset.cpp は aux2 から公開していないので直接取り込む
clang++ -O1 -static -std=c++17 -fms-extensions \
	-I"$ROOT/sdk/aviutl2" -I"$ROOT/src/aviutl2" -I"$ROOT/src/common" \
	-o "$BUILD/tests/test_preset.exe" "$ROOT/tests/test_preset.cpp" \
	"$ROOT/src/aviutl2/preset.cpp" -lshlwapi -luser32
"$BUILD/tests/test_preset.exe" "$BUILD/tests/preset"

echo
echo "=== test_selector ==="
clang++ -O1 -static -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	-I"$ROOT/src/tvtp" -I"$ROOT/tests" \
	-o "$BUILD/tests/test_selector.exe" "$ROOT/tests/test_selector.cpp" \
	"${BONTS_OBJS[@]}" -luser32
"$BUILD/tests/test_selector.exe"

#	外字 (DRCS) から組み立てた TTF が DirectWrite に通るか。
#	自前でフォントを作る以上、「壊れていない」の判定は本物に読ませて行う。
echo
echo "=== test_drcs_ttf ==="
clang++ -O1 -static -municode -std=c++17 -fms-extensions \
	-I"$ROOT/src/aviutl2/caption" \
	-o "$BUILD/tests/test_drcs_ttf.exe" "$ROOT/tests/test_drcs_ttf.cpp" \
	"$ROOT/src/aviutl2/caption/drcs_ttf.cpp" -ldwrite -lole32
"$BUILD/tests/test_drcs_ttf.exe" "$BUILD/tests/drcs.ttf"

#	TVTest 側のプラグインを TVTest 無しで動かす。
#	TS サンプルが無ければ「実際に溜める」確認だけを飛ばして通る。
echo
echo "=== test_tvtp ==="
mkdir -p "$BUILD/tests/tvtp"
clang++ -O1 -static -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	-I"$ROOT/src/tvtp" -I"$ROOT/src/common" \
	-o "$BUILD/tests/test_tvtp.exe" "$ROOT/tests/test_tvtp.cpp" -lshlwapi -luser32
TS_DIR="${TSMEMORY_TS_DIR:-$BUILD/ts-examples}"

#	--- TS を用意する --------------------------------------------------------
#	放送 TS は再配布出来ないので、無ければ合成する。
#	既に *.ts が置いてあれば作らない (実 TS を置いた場合を潰さない)
if ! ls "$TS_DIR"/*.ts >/dev/null 2>&1; then
	echo
	echo "=== generating test streams ==="
	bash "$ROOT/tests/tools/gen-ts-examples.sh" "$TS_DIR"
fi

#	対象は TS_DIR に在る物すべて。名前順で並べる
TS_LIST=()
for f in "$TS_DIR"/*.ts; do
	[ -f "$f" ] && TS_LIST+=("$f")
done

if [ ${#TS_LIST[@]} -eq 0 ]; then
	echo "error: TS が 1 つもありません: $TS_DIR" >&2
	exit 1
fi

echo
echo "TS examples in $TS_DIR :"
for f in "${TS_LIST[@]}"; do
	echo "  $(basename "$f")  ($(( $(stat -c %s "$f") / 1024 / 1024 )) MB)"
done

#	最初の 1 つは「代表」。1 本で足りる確認に使う
TS_FIRST="${TS_LIST[0]}"

"$BUILD/tests/test_tvtp.exe" "$ROOT/dist/TSMemory.tvtp" "$BUILD/tests/tvtp" "$TS_FIRST"

#	--- ここから TS を使うテスト ---------------------------------------------
#	置いてある TS を 1 本ずつ通す
AUDIO_INC="-I$ROOT/src/aviutl2/audio"
AUDIO_SRC="$ROOT/src/aviutl2/audio/ts_audio.cpp $ROOT/src/aviutl2/audio/aac_decoder.cpp"
AUDIO_LIB="-lmfplat -lmfuuid -lole32 -loleaut32 -lshlwapi -luser32"

clang++ -O1 -static -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	$AUDIO_INC -o "$BUILD/tests/test_adts.exe" "$ROOT/tests/test_adts.cpp" \
	"$ROOT/src/aviutl2/audio/ts_audio.cpp" -lshlwapi -luser32
clang++ -O1 -static -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	$AUDIO_INC -o "$BUILD/tests/test_aac_decode.exe" "$ROOT/tests/test_aac_decode.cpp" \
	$AUDIO_SRC $AUDIO_LIB
clang++ -O1 -static -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	$AUDIO_INC -o "$BUILD/tests/test_audio.exe" "$ROOT/tests/test_audio.cpp" \
	$AUDIO_SRC "$ROOT/src/aviutl2/audio/tvtv_audio.cpp" $AUDIO_LIB
clang++ -O1 -static -std=c++17 -fms-extensions \
	-I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" \
	-o "$BUILD/tests/test_fuzz.exe" "$ROOT/tests/test_fuzz.cpp" -lshlwapi -luser32
clang++ -O1 -static -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	-I"$ROOT/src/tvtp" -I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" -I"$ROOT/tests" \
	-o "$BUILD/tests/test_multich.exe" "$ROOT/tests/test_multich.cpp" \
	"${BONTS_OBJS[@]}" -luser32
# -static: 起動時に libunwind.dll 等を探しに行かないようにする
clang++ -O1 -static -std=c++17 -fms-extensions -include "$ROOT/src/tvtp/msvc_compat.h" \
	-I"$ROOT/sdk/aviutl2" -I"$ROOT/src/common" -I"$ROOT/tests" \
	-o "$BUILD/tests/test_decode.exe" "$ROOT/tests/test_decode.cpp" -luser32

#	入力プラグイン越しに音声が出る事。ini は起動時に読まれるので
#	audio=1 の設定を隣に置いた複製を使う
mkdir -p "$BUILD/tests/withaudio"
cp "$ROOT/dist/TSMemory-TVTestSrc.aux2" "$BUILD/tests/withaudio/"
printf '[M2V]\r\naudio=1\r\naspect_ratio=1\r\n' \
	> "$BUILD/tests/withaudio/TSMemory-TVTestSrc.ini"

for TS in "${TS_LIST[@]}"; do
	NAME="$(basename "$TS" .ts)"

	echo
	echo "=== test_adts [$NAME] ==="
	(cd "$BUILD/tests" && ./test_adts.exe "$TS")

	echo
	echo "=== test_aac_decode [$NAME] ==="
	(cd "$BUILD/tests" && ./test_aac_decode.exe "$TS")

	echo
	echo "=== test_audio [$NAME] ==="
	(cd "$BUILD/tests" && ./test_audio.exe "$TS")

	echo
	echo "=== test_decode ([M2V] audio=1) [$NAME] ==="
	"$BUILD/tests/test_decode.exe" "$TS" "$BUILD/tests/wa-$NAME" \
		"$BUILD/tests/withaudio/TSMemory-TVTestSrc.aux2"

	#	壊れた TS を食わせても落ちない事。
	#	返って来ない物 (m2v の既知の弱点) は数えて出すだけで失敗にはしない。
	echo
	echo "=== test_fuzz [$NAME] ==="
	(cd "$BUILD/tests" && ./test_fuzz.exe "$TS" "$ROOT/dist/TSMemory-TVTestSrc.aux2" 6)

	#	サービスが 1 つしかない TS では test_multich 側が skip する
	echo
	echo "=== test_multich [$NAME] ==="
	"$BUILD/tests/test_multich.exe" "$TS" "$BUILD/tests/ch-$NAME" \
		"$ROOT/dist/TSMemory-TVTestSrc.aux2"

	echo
	echo "=== test_decode [$NAME] ==="
	"$BUILD/tests/test_decode.exe" "$TS" "$BUILD/tests/frame-$NAME" \
		"$ROOT/dist/TSMemory-TVTestSrc.aux2"

	# 切り出したストリームでどれだけ欠けるかを見る。
	#   head : 末尾だけが GOP の途中で切れる -> 末尾側の欠落が判る
	#   mid  : 両端が GOP の途中 (リングバッファと同じ状況)
	echo
	echo "=== test_decode (first 10MB : only the end is cut) [$NAME] ==="
	"$BUILD/tests/test_decode.exe" "$TS" "$BUILD/tests/head-$NAME" \
		"$ROOT/dist/TSMemory-TVTestSrc.aux2" head:10

	echo
	echo "=== test_decode (middle 10MB : both ends are cut) [$NAME] ==="
	"$BUILD/tests/test_decode.exe" "$TS" "$BUILD/tests/mid-$NAME" \
		"$ROOT/dist/TSMemory-TVTestSrc.aux2" mid:10
done

echo
echo "デコード結果を目視確認する場合:"
echo "  compilers/python/python.exe tests/tools/raw2png.py build/tests/frame-sample0_asis.raw 1920 1080 build/tests/frame0.png 3"
