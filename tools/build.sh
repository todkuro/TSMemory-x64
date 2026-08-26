#!/usr/bin/env bash
#
# TSMemory 64bit (AviUtl ExEdit2 対応) 版のビルド
#
#   dist/TSMemory.tvtp  ... TVTest (x64) 用プラグイン
#   dist/TSMemory-TVTestSrc.aux2  ... AviUtl ExEdit2 用汎用プラグイン
#                           (*.tvtv 入力プラグイン + 連携 + キャプチャ窓)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN="$ROOT/compilers/llvm-mingw"
BUILD="$ROOT/build"
DIST="$ROOT/dist"

# 作る物は常に Windows 向け。Linux ではクロスコンパイルになる。
#
# 接頭辞なしの clang は、Windows ホストなら Windows を狙うが Linux では
# ホスト (ELF) を狙ってしまう。どちらでも同じ物を指す
# x86_64-w64-mingw32-* を使う。
TRIPLE=x86_64-w64-mingw32

if [ -d "$TOOLCHAIN/bin" ]; then
	export PATH="$TOOLCHAIN/bin:$PATH"
elif command -v "$TRIPLE-clang" >/dev/null 2>&1; then
	# Windows 以外では PATH に入っている物を使っても構わない
	TOOLCHAIN="$(dirname "$(dirname "$(command -v "$TRIPLE-clang")")")"
	echo "using the toolchain on PATH: $TOOLCHAIN"
else
	echo "error: toolchain not found. run tools/setup-toolchain.sh first." >&2
	exit 1
fi

CC="$TRIPLE-clang"
CXX="$TRIPLE-clang++"
RC="$TRIPLE-windres"

COMMON_FLAGS="-O2 -m64 -fms-extensions -municode"

# 64bit 化で最も危ないのはポインタの int への切り詰め。
# 古いコードの雑多な警告は落としつつ、この種の警告だけはエラーにする。
PTR64_FLAGS="-Werror=pointer-to-int-cast -Werror=int-to-pointer-cast \
-Werror=void-pointer-to-int-cast -Werror=int-to-void-pointer-cast \
-Werror=int-conversion \
-Werror=incompatible-function-pointer-types"

CFLAGS="$COMMON_FLAGS -Wno-everything $PTR64_FLAGS"
CXXFLAGS="$COMMON_FLAGS -std=c++17"

mkdir -p "$BUILD/m2v" "$BUILD/aviutl2" "$BUILD/tvtp" "$BUILD/generated" "$DIST"

#---------------------------------------------------------------------------
# 版 (CHANGELOG.md が唯一の正)
#---------------------------------------------------------------------------
#	版をソースや ini に直書きすると、リリースの度に何箇所も直す事になり
#	必ずどこかが古いまま残る。一番上の見出しから読み取って流し込む。
VERSION="$(sed -nE 's/^## +ver\.([0-9]+(\.[0-9]+)*).*/\1/p' "$ROOT/CHANGELOG.md" | head -1)"
if [ -z "$VERSION" ]; then
	echo "error: CHANGELOG.md から版を読み取れません。" >&2
	echo "  一番上に '## ver.X.Y.Z ...' の見出しが要ります。" >&2
	exit 1
fi
echo "version: $VERSION  (CHANGELOG.md)"

#	AviUtl2 のプラグイン一覧に出る文字列。src/aviutl2/plugin_main.cpp が読む
cat > "$BUILD/generated/tsmemory_version.h" <<EOF
//	tools/build.sh が CHANGELOG.md から生成します。編集しないでください。
#pragma once
#define TSMEMORY_VERSION_TEXT	L"TSMemory version $VERSION (64bit / AviUtl ExEdit2)"
EOF

#---------------------------------------------------------------------------
# compile_commands.json (VS Code の IntelliSense 用)
#---------------------------------------------------------------------------
#	実際に使った引数をそのまま残す。これが無いと、-include で入れている
#	src/tvtp/msvc_compat.h が IntelliSense から見えず、BonTsEngine の
#	min/max が「識別子が定義されていません」になる。
#
#	VS Code は Windows のパスしか解釈しないので /g/... を G:/... に直す。
CCDB="$BUILD/compile_commands.json"
CCDB_PARTS="$BUILD/.compile_commands.parts"
: > "$CCDB_PARTS"

#	cygpath は Git Bash にしか無い。無ければパスをそのまま使う
WROOT="$(cygpath -m "$ROOT" 2>/dev/null || printf '%s' "$ROOT")"
WTOOLCHAIN="$(cygpath -m "$TOOLCHAIN" 2>/dev/null || printf '%s' "$TOOLCHAIN")"
EXE=""
[ -x "$TOOLCHAIN/bin/$CC.exe" ] && EXE=".exe"

#	コンパイルしつつ 1 件分を書き足す。使い方は `cc $CXX -c ... file.cpp`
cc() {
	"$@"

	local args=("$@")
	local src="${args[${#args[@]}-1]}"
	local out="" a
	for a in "${args[@]}"; do
		case "$a" in
			"$CC"|"$CXX") a="$WTOOLCHAIN/bin/$a$EXE" ;;
			*)            a="${a//$ROOT/$WROOT}" ;;
		esac
		out="$out\"${a//\"/\\\"}\", "
	done
	printf '  {"directory": "%s", "file": "%s", "arguments": [%s]},\n' \
		"$WROOT" "${src//$ROOT/$WROOT}" "${out%, }" >> "$CCDB_PARTS"
}

#---------------------------------------------------------------------------
# 1. m2v (MPEG-2 VIDEO VFAPI Plug-In) のデコーダ部
#---------------------------------------------------------------------------
#   除外するもの:
#     m2v.c               VFAPI プラグインのエントリ (AviUtl2 では使わない)
#     mpeg2edit.c         VFAPI 編集拡張 (同上)
#     idct_reference_sse.c  MSVC の 32bit インラインアセンブラを使用
echo "[1/4] m2v"
for f in "$ROOT"/src/m2v/*.c; do
	b="$(basename "$f" .c)"
	case "$b" in
		m2v|mpeg2edit|idct_reference_sse) continue ;;
	esac
	cc $CC -c $CFLAGS -DWIN32 -D_WINDOWS -I"$ROOT/src/m2v" -o "$BUILD/m2v/$b.o" "$f"
done

#---------------------------------------------------------------------------
# 2. AviUtl ExEdit2 側プラグイン
#---------------------------------------------------------------------------
echo "[2/4] TSMemory-TVTestSrc.aux2"
AUX2_INC="-I$BUILD/generated -I$ROOT/src/m2v -I$ROOT/sdk/aviutl2 -I$ROOT/src/common -I$ROOT/src/aviutl2 -I$ROOT/src/aviutl2/audio -I$ROOT/src/aviutl2/caption"
for b in input_tvtv bridge capture exitguard preset inifile plugin_main; do
	cc $CXX -c $CXXFLAGS -Wall -Wno-unknown-pragmas $AUX2_INC \
		-o "$BUILD/aviutl2/$b.o" "$ROOT/src/aviutl2/$b.cpp"
done

#	音声 (src/aviutl2/audio/)。既存のコードとは分けてある。
#	入力プラグインからは tvtv_audio.h だけを見る
mkdir -p "$BUILD/aviutl2/audio"
for b in ts_audio aac_decoder tvtv_audio; do
	cc $CXX -c $CXXFLAGS -Wall -Wno-unknown-pragmas $AUX2_INC \
		-o "$BUILD/aviutl2/audio/$b.o" "$ROOT/src/aviutl2/audio/$b.cpp"
done

#	字幕 (src/aviutl2/caption/)。音声と同じく既存のコードとは分けてある
mkdir -p "$BUILD/aviutl2/caption"
for b in drcs_font drcs_ttf arib_text arib_to_aviutl2 ts_caption; do
	cc $CXX -c $CXXFLAGS -Wall -Wno-unknown-pragmas $AUX2_INC \
		-o "$BUILD/aviutl2/caption/$b.o" "$ROOT/src/aviutl2/caption/$b.cpp"
done

$RC -I "$ROOT/src/m2v" -o "$BUILD/aviutl2/tsmemory_rc.o" "$ROOT/src/aviutl2/tsmemory.rc"

#	-lmfplat -lmfuuid は音声 (Media Foundation の AAC デコーダ) 用
#	-ldwrite は字幕の外字 (DRCS) をフォントとして渡す為
$CXX -shared -O2 -static -o "$DIST/TSMemory-TVTestSrc.aux2" \
	"$BUILD"/m2v/*.o "$BUILD"/aviutl2/*.o "$BUILD"/aviutl2/audio/*.o \
	"$BUILD"/aviutl2/caption/*.o \
	-Wl,--error-limit=0 \
	-lshlwapi -lcomctl32 -lgdi32 -luser32 -lole32 -loleaut32 -lwindowscodecs -luuid \
	-lmfplat -lmfuuid -ldwrite

#---------------------------------------------------------------------------
# 3. TVTest プラグイン
#---------------------------------------------------------------------------
echo "[3/4] TSMemory.tvtp"
TVTP_FLAGS="$CXXFLAGS -DUNICODE -D_UNICODE -Wall -Wno-unknown-pragmas -Wno-unused-parameter -Wno-pragma-pack"
TVTP_INC="-I$ROOT/src/tvtp -I$ROOT/src/common"
PREINC="-include $ROOT/src/tvtp/msvc_compat.h"

for f in "$ROOT"/src/tvtp/BonTsEngine/*.cpp; do
	b="$(basename "$f" .cpp)"
	cc $CXX -c $TVTP_FLAGS -Wno-everything $PREINC $TVTP_INC -o "$BUILD/tvtp/$b.o" "$f"
done
cc $CXX -c $TVTP_FLAGS $PREINC $TVTP_INC -o "$BUILD/tvtp/TSMemory.o" "$ROOT/src/tvtp/TSMemory.cpp"

$CXX -shared -O2 -static -municode -o "$DIST/TSMemory.tvtp" \
	"$BUILD"/tvtp/*.o "$ROOT/src/tvtp/Exports.def" \
	-lshlwapi -luser32 -lkernel32

#	集めた分を compile_commands.json にまとめる (最後の , を落とす)
{
	echo "["
	sed '$ s/,$//' "$CCDB_PARTS"
	echo "]"
} > "$CCDB"
rm -f "$CCDB_PARTS"

#	プラグインは自分と同じ場所の ini を読む (src/aviutl2/plugin_main.h)。
#	テストは dist/ の .aux2 をそのまま読み込む為、ここにも置いておかないと
#	**テストが古い設定で走る**。
#	更に [Capture] の設定は終了時に書き戻されるので、放っておくと
#	「更新日時だけ新しく中身は古い」ファイルが残る。毎回入れ直す
cp "$ROOT/res/TSMemory-TVTestSrc.aux2.ini" "$DIST/TSMemory-TVTestSrc.ini"

#---------------------------------------------------------------------------
# 4. 配布用のフォルダ構成とパッケージファイル
#---------------------------------------------------------------------------
echo "[4/4] packaging"

PKG="$BUILD/package"
rm -rf "$PKG"
mkdir -p "$PKG/TVTest/Plugins" "$PKG/aviutl2/Plugin/TSMemory-TVTestSrc" "$PKG/aviutl2/Language"

cp "$DIST/TSMemory.tvtp"          "$PKG/TVTest/Plugins/"
cp "$ROOT/res/TSMemory.tvtp.ini"  "$PKG/TVTest/Plugins/TSMemory.ini"

cp "$DIST/TSMemory-TVTestSrc.aux2"          "$PKG/aviutl2/Plugin/TSMemory-TVTestSrc/"
cp "$ROOT/res/TSMemory-TVTestSrc.aux2.ini"  "$PKG/aviutl2/Plugin/TSMemory-TVTestSrc/TSMemory-TVTestSrc.ini"
cp "$ROOT/res/English.TSMemory-TVTestSrc.aul2" "$PKG/aviutl2/Language/"
cp "$ROOT/README.md"              "$PKG/"
cp "$ROOT/CHANGELOG.md"           "$PKG/"
# README から参照している調査メモ
mkdir -p "$PKG/docs"
cp "$ROOT/docs/"*.md               "$PKG/docs/"
# ライセンス表記。TSMemory.tvtp は BonTsEngine を含む為 GPL の全文が必要、
# AviUtl2 SDK は MIT の表示が必要 (適用範囲は LICENSE.md を参照)
mkdir -p "$PKG/licenses"
cp "$ROOT/licenses/GPL-2.0.txt"                    "$PKG/licenses/"
cp "$ROOT/licenses/AviUtl2-Plugin-SDK-license.txt" "$PKG/licenses/"
cp "$ROOT/LICENSE.md"                              "$PKG/"
# aviutl2\ 配下だけを手でコピーした場合にも表記が残るようにする
cp "$ROOT/sdk/aviutl2/license.txt" 	"$PKG/aviutl2/Plugin/TSMemory-TVTestSrc/AviUtl2-Plugin-SDK-license.txt"

# 検証用の TVTest ツリーがある場合はそちらのプラグインも更新しておく。
# ここを忘れると古いプラグインのまま検証してしまう。
if [ -d "$DIST/TVTest-x64/Plugins" ]; then
	cp "$DIST/TSMemory.tvtp" "$DIST/TVTest-x64/Plugins/"
	echo "  updated $DIST/TVTest-x64/Plugins/TSMemory.tvtp"
fi

# AviUtl2 のプレビュー画面に D&D でインストール出来るパッケージファイル
AU2PKG="$BUILD/au2pkg"
rm -rf "$AU2PKG"
mkdir -p "$AU2PKG/Plugin/TSMemory-TVTestSrc" "$AU2PKG/Language"
cp "$DIST/TSMemory-TVTestSrc.aux2"             "$AU2PKG/Plugin/TSMemory-TVTestSrc/"
cp "$ROOT/res/TSMemory-TVTestSrc.aux2.ini"     "$AU2PKG/Plugin/TSMemory-TVTestSrc/TSMemory-TVTestSrc.ini"
cp "$ROOT/res/English.TSMemory-TVTestSrc.aul2" "$AU2PKG/Language/"
# au2pkg は Plugin\ Script\ Language\ 等の配下しかインストールされない為、
# ライセンス表記はプラグイン本体と同じフォルダに入れる
cp "$ROOT/sdk/aviutl2/license.txt" 	"$AU2PKG/Plugin/TSMemory-TVTestSrc/AviUtl2-Plugin-SDK-license.txt"
sed "s/@VERSION@/$VERSION/g" "$ROOT/res/package.ini" > "$AU2PKG/package.ini"
sed "s/@VERSION@/$VERSION/g" "$ROOT/res/package.txt" > "$AU2PKG/package.txt"

rm -f "$DIST/TSMemory-TVTestSrc.au2pkg.zip" "$DIST/TSMemory-x64.zip"

# zip の作成に Python を使う。
# Windows では compilers/python/ の物だけを使う (CLAUDE.md の方針)。
# Windows 以外は可搬な配布が無いので PATH の python3 を使ってよい。
PY="$ROOT/compilers/python/python.exe"
if [ ! -x "$PY" ]; then
	if [ "$(uname -s)" != "Linux" ] && [ "$(uname -s)" != "Darwin" ]; then
		echo "  Python が無いので取得します"
		bash "$ROOT/tools/setup-python.sh"
	elif command -v python3 >/dev/null 2>&1; then
		PY="$(command -v python3)"
	else
		echo "error: python3 が見つかりません (zip の作成に使います)。" >&2
		exit 1
	fi
fi

"$PY" "$ROOT/tools/zipdir.py" "$AU2PKG" "$DIST/TSMemory-TVTestSrc.au2pkg.zip"
"$PY" "$ROOT/tools/zipdir.py" "$PKG"    "$DIST/TSMemory-x64.zip"

echo
echo "built:"
ls -la "$DIST"
