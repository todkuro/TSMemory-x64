//--------------------------------------------------------------------------
//	ARIB STD-B24 の区点 -> Unicode の対応表 (**自動生成**)
//
//	作り直す場合は tools/regen-gaiji.sh を使う事。手で直さない。
//	元にしているのは libaribcaption の対応表:
//
//	  Copyright (C) 2021 magicxqq <xqq@xqq.im>. All rights reserved.
//
//	  Permission to use, copy, modify, and distribute this software for any
//	  purpose with or without fee is hereby granted, provided that the above
//	  copyright notice and this permission notice appear in all copies.
//
//	  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
//	  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
//	  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
//	  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
//	  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
//	  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
//	  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
//	区 1-94 x 点 1-94 の 8836 個。うち 8363 個が定義されている。
//	**区 85-94 は ARIB の追加漢字・追加記号**で、CP932 の同じ位置には
//	別の文字が載っている。CP932 経由で変換すると化ける。
//--------------------------------------------------------------------------
#pragma once

#include <windows.h>

//	UTF-16 のコード単位をまとめて置いた場所
extern const WCHAR TSMemoryAribGaijiPool[];

//	区点 (どちらも 1 起点) から 1 文字を得る。無ければ空を返す
const WCHAR *TSMemoryAribKuTen(int Ku, int Ten, int *pLength);

//	既定のマクロ (0x60-0x6F の 0-15) の中身を得る。
//	**中身は文字集合を割り当てる ESC の並び**で、本文は入っていない。
//	G3 の初期値がマクロなので `SS3 0x61` のような形で普通に出て来る。
const BYTE *TSMemoryAribDefaultMacro(int Index, int *pLength);

//	1 バイトの文字集合。符号 0x21-0x7E をそのまま渡す。
//
//	**区 4 / 区 5 で代用してはいけない。**末尾に「ー」「、」等が
//	入っており、区で引くと落ちる (実測: ステーション -> ステション)。
//	英数は全角。中型 (MSZ) の時に半角相当の見た目になる
WCHAR TSMemoryAribAlnum(BYTE Code);
WCHAR TSMemoryAribHiragana(BYTE Code);
WCHAR TSMemoryAribKatakana(BYTE Code);
WCHAR TSMemoryAribJisKatakana(BYTE Code);

//	ARIB の色表 (128 色)。色番号は「色配列 * 16 + 番号」。
//
//	**背景は半透明の黒。**放送は色配列 4 を選び背景に 1 番を使うので、
//	索引 65 = (0, 0, 0, α128) になる。字幕の背景が黒い箱に見えるのは
//	この為で、TVCaptionMod2 の「背景の不透明度」もこのアルファを触る。
struct TSMemoryAribColor {
	BYTE R, G, B, A;
};

//	範囲の外なら透明 (A = 0) を返す
TSMemoryAribColor TSMemoryAribClut(int Index);
