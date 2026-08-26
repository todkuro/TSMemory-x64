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
