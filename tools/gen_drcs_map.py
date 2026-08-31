# -*- coding: utf-8 -*-
#
#	libaribcaption の DRCS 置換表から
#	src/aviutl2/caption/arib_drcs_map.h を作る。
#
#	  compilers/python/python.exe tools/gen_drcs_map.py <libaribcaption> <出力先>
#
#	借りるのは「字形の md5 -> Unicode」の対応表だけ。
#	ISC 形式なので、著作権表示を生成物の冒頭に残している。
#
#	**なぜ要るのか**
#	放送の外字 (DRCS) の多くは、Unicode に実在する文字を字形の
#	ビットマップとして送っているだけ (『』〜㊙｟｠、德 髙 﨑 塚 𠮷 等)。
#	対応表で本物の文字に直せば、フォントを組み立てずにその場で出せる。
#	AviUtl2 はフォントの登録を初期化時にしか受け付けない為、
#	この経路が無いと「初めて見た外字は次の起動まで出ない」事になる。
#
import re
import sys

HEADER = """\
//--------------------------------------------------------------------------
//	DRCS (外字) の字形 -> Unicode の対応表 (**自動生成**)
//
//	作り直す場合は tools/regen-drcs-map.sh を使う事。手で直さない。
//	元にしているのは libaribcaption の kDRCSReplacementMap:
//
//	  Copyright (C) 2022 magicxqq <xqq@xqq.im>. All rights reserved.
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
//	**鍵は字形のビットマップの md5。**ARIB の符号 (0x21 から順) の意味は
//	番組ごとに変わるので鍵にならない。md5 を取る対象は
//	`(幅 * 高さ * 深さのビット数 + 7) / 8` バイトの生のビットマップで、
//	幅・高さ・深さ自体は含めない (libaribcaption と同じ)。
//
//	%(md5)d 通りの md5 が %(chars)d 文字に対応する。
//	同じ文字に複数の md5 があるのは、局ごとに字形が微妙に違う為。
//--------------------------------------------------------------------------
#pragma once

#include <windows.h>

struct TSMemoryDrcsMapEntry {
	BYTE Md5[16];		// 字形のビットマップの md5
	DWORD Code;			// 置き換える Unicode の符号位置 (UCS-4)
};

//	**md5 の昇順**に並べてある (二分探索する為)
extern const TSMemoryDrcsMapEntry TSMemoryDrcsMap[];
extern const int TSMemoryDrcsMapCount;
"""


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_drcs_map.py <libaribcaption> <出力先>")
    src, out = sys.argv[1], sys.argv[2]

    path = src + "/src/decoder/b24_drcs_conv.cpp"
    text = open(path, encoding="utf-8-sig").read()
    pairs = re.findall(r'\{\s*"([0-9a-fA-F]{32})"\s*,\s*(0[xX][0-9a-fA-F]+)\s*\}',
                       text)
    if not pairs:
        raise SystemExit("置換表が見つかりません: %s" % path)

    #	md5 が重複していないかを見る。重複していたら元が壊れている
    table = {}
    for md5, code in pairs:
        md5 = md5.lower()
        code = int(code, 16)
        if md5 in table and table[md5] != code:
            raise SystemExit("同じ md5 に別の文字: %s" % md5)
        table[md5] = code

    items = sorted(table.items())
    chars = len(set(table.values()))

    lines = [HEADER % {"md5": len(items), "chars": chars}]
    lines.append("")
    lines.append("#ifdef TSMEMORY_DRCS_MAP_DEFINE")
    lines.append("")
    lines.append("const TSMemoryDrcsMapEntry TSMemoryDrcsMap[] = {")
    for md5, code in items:
        raw = ",".join("0x%s" % md5[i:i + 2] for i in range(0, 32, 2))
        ch = chr(code)
        #	コメントに実物を入れる。ソースは UTF-8 で持つ
        lines.append("\t{ { %s }, 0x%05X },\t// %s" % (raw, code, ch))
    lines.append("};")
    lines.append("")
    lines.append("const int TSMemoryDrcsMapCount = %d;" % len(items))
    lines.append("")
    lines.append("#endif\t// TSMEMORY_DRCS_MAP_DEFINE")
    lines.append("")

    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print("%d 通りの md5 -> %d 文字" % (len(items), chars))


main()
