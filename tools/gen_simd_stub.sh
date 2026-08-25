#!/usr/bin/env bash
#
# src/m2v/simd_stub.c を生成する。
#
# m2v が元々持っていた MMX/SSE/SSE2 ルーチンは 32bit x86 アセンブラなので
# x64 ではビルド出来ない。呼び出し側 (frame.c / mc.c / picture.c 等) は
# 残っている為、リンクを通すためのダミー定義が要る。
# 未定義シンボルの一覧はリンカに教えてもらう。
#
# ※ x64 で SIMD を使わない、という意味ではない。x64 用の SIMD は
#   intrinsics で別途書いており、現在は resize.c が SSE2 を使う
#   (docs/development.md の「デコード速度の改善」を参照)。
#
# ※ 通常は生成済みの simd_stub.c がリポジトリに入っているので実行不要。
#   src/m2v を原典から作り直した時だけ使う (tools/regen-m2v.sh が呼ぶ)。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$ROOT/compilers/llvm-mingw/bin:$PATH"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "compiling m2v objects"
mkdir -p "$TMP/obj"
for f in "$ROOT"/src/m2v/*.c; do
	b="$(basename "$f" .c)"
	case "$b" in
		m2v|mpeg2edit|idct_reference_sse|simd_stub) continue ;;
	esac
	clang -c -O2 -m64 -fms-extensions -Wno-everything -DWIN32 \
		-I"$ROOT/src/m2v" -o "$TMP/obj/$b.o" "$f"
done

echo "collecting undefined symbols"
clang++ -shared -o "$TMP/probe.dll" "$TMP"/obj/*.o -Wl,--error-limit=0 \
	-lgdi32 -luser32 -lcomctl32 2>&1 \
	| grep "undefined symbol" | sed 's/.*undefined symbol: //' | sort -u > "$TMP/syms.txt" || true

echo "found $(wc -l < "$TMP/syms.txt") symbols"

OUT="$ROOT/src/m2v/simd_stub.c"
cat > "$OUT" <<'HEADER'
/*
 * simd_stub.c - 32bit x86 アセンブラ版 SIMD ルーチンのダミー実装
 *
 * TVTestSrc (MPEG-2 VIDEO VFAPI Plug-In) が元々持っていた MMX/SSE/SSE2 の
 * ルーチンは 32bit の x86 アセンブラ (*.asm) と MSVC のインラインアセンブラで
 * 書かれており、x64 ではビルド出来ない。
 *
 * 64bit 版では registry.c の get_simd_mode() が常に 0 を返すようにしてあり、
 * mpeg_video.c の関数テーブル設定ではこれらが選ばれる事はない。
 * 呼び出し側のコードは残っている為、リンクを通すためだけのダミー定義。
 * (万一呼ばれた場合は OutputDebugString で判るようにしてある)
 *
 * ※ これは「x64 では SIMD を使わない」という意味ではない。
 *   x64 用の SIMD は 32bit アセンブラを移植するのではなく intrinsics で
 *   書いており、現在は resize.c の component_resize() が SSE2 を使う。
 *   一番重い処理はそちらなので、この足場は x64 では使っていない。
 *   詳細は docs/development.md の「デコード速度の改善」を参照。
 *
 * ※ このファイルは tools/gen_simd_stub.sh で生成される。
 */
#include <windows.h>

static void m2v_simd_unreachable(const char *name)
{
	OutputDebugStringA("TSMemory: SIMD routine called on x64 build: ");
	OutputDebugStringA(name);
	OutputDebugStringA("\n");
}

HEADER

while read -r s; do
	printf 'void %s(void) { m2v_simd_unreachable("%s"); }\n' "$s" "$s" >> "$OUT"
done < "$TMP/syms.txt"

echo "wrote $OUT"
