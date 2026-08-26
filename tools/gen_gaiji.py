# -*- coding: utf-8 -*-
#
#	libaribcaption の対応表から src/aviutl2/caption/arib_gaiji.h を作る。
#
#	  compilers/python/python.exe tools/gen_gaiji.py <libaribcaption> <出力先>
#
#	借りるのは「ARIB の区点 -> Unicode」の対応表だけ。
#	MIT (ISC 形式) なので、著作権表示を生成物の冒頭に残している。
#
#	**なぜ要るのか**
#	区 85-94 は ARIB の追加漢字・追加記号で、Shift_JIS (CP932) には
#	同じ位置に別の文字が載っている。CP932 経由で変換すると、
#	例えば区 92 点 92 が「釗」になってしまう (実測)。
#	区 1-84 も JIS X 0213 の文字が CP932 に無い事があり、その分は落ちる。
#
import re
import sys


def read_u32_array(path, name):
    """`inline constexpr uint32_t <name>[] = { ... };` を読む"""
    text = open(path, encoding="utf-8-sig").read()
    m = re.search(r"\b" + re.escape(name) + r"\s*\[\s*\]\s*=\s*\{(.*?)\n\};",
                  text, re.S)
    if m is None:
        raise SystemExit("表が見つかりません: %s (%s)" % (name, path))
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [int(v, 0) for v in re.findall(r"0[xX][0-9a-fA-F]+|\b\d+\b", body)]


#	未定義の区点はこの値で埋まっている (0 ではない)
REPLACEMENT = 0xFFFD


def to_utf16(cp):
    """Unicode の符号位置を UTF-16 のコード単位にする"""
    if cp == 0 or cp == REPLACEMENT:
        return []
    if cp < 0x10000:
        return [cp]
    cp -= 0x10000
    return [0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)]


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_gaiji.py <libaribcaption> <out.h>")
    src, out = sys.argv[1], sys.argv[2]

    gaiji_h = src + "/src/decoder/b24_gaiji_table.hpp"
    conv_h = src + "/src/decoder/b24_conv_tables.hpp"

    kanji = read_u32_array(conv_h, "kKanjiTable")
    gaiji = read_u32_array(gaiji_h, "kAdditionalSymbolsTable_Unicode")

    #	区 1-84 が kKanjiTable、区 85-94 が追加表。どちらも 94 点ずつ
    if len(kanji) < 84 * 94:
        raise SystemExit("kKanjiTable が短すぎます: %d" % len(kanji))
    if len(gaiji) < 10 * 94:
        raise SystemExit("追加表が短すぎます: %d" % len(gaiji))

    #	1 つに繋げて「区 1-94 x 点 1-94」の表にする
    table = list(kanji[:84 * 94]) + list(gaiji[:10 * 94])

    #	UTF-16 に直す。1 文字が 2 コード単位になる物があるので
    #	「開始位置 + 長さ」で引く形にする
    pool = []
    offset = []
    length = []
    cache = {}
    for cp in table:
        units = to_utf16(cp)
        key = tuple(units)
        if key in cache:
            o = cache[key]
        else:
            o = len(pool)
            pool.extend(units)
            cache[key] = o
        offset.append(o)
        length.append(len(units))

    defined = sum(1 for cp in table if cp != 0 and cp != REPLACEMENT)

    lines = []
    w = lines.append
    w("//" + "-" * 74)
    w("//\tARIB STD-B24 の区点 -> Unicode の対応表 (**自動生成**)")
    w("//")
    w("//\t作り直す場合は tools/regen-gaiji.sh を使う事。手で直さない。")
    w("//\t元にしているのは libaribcaption の対応表:")
    w("//")
    w("//\t  Copyright (C) 2021 magicxqq <xqq@xqq.im>. All rights reserved.")
    w("//")
    w("//\t  Permission to use, copy, modify, and distribute this software for any")
    w("//\t  purpose with or without fee is hereby granted, provided that the above")
    w("//\t  copyright notice and this permission notice appear in all copies.")
    w("//")
    w("//\t  THE SOFTWARE IS PROVIDED \"AS IS\" AND THE AUTHOR DISCLAIMS ALL WARRANTIES")
    w("//\t  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF")
    w("//\t  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR")
    w("//\t  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES")
    w("//\t  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN")
    w("//\t  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF")
    w("//\t  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.")
    w("//")
    w("//\t区 1-94 x 点 1-94 の %d 個。うち %d 個が定義されている。" % (len(table), defined))
    w("//\t**区 85-94 は ARIB の追加漢字・追加記号**で、CP932 の同じ位置には")
    w("//\t別の文字が載っている。CP932 経由で変換すると化ける。")
    w("//" + "-" * 74)
    w("#pragma once")
    w("")
    w("#include <windows.h>")
    w("")
    w("//\tUTF-16 のコード単位をまとめて置いた場所")
    w("extern const WCHAR TSMemoryAribGaijiPool[];")
    w("")
    w("//\t区点 (どちらも 1 起点) から 1 文字を得る。無ければ空を返す")
    w("const WCHAR *TSMemoryAribKuTen(int Ku, int Ten, int *pLength);")

    open(out, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")

    #	--- 実体 -------------------------------------------------------------
    cpp = out[:-2] + ".cpp"
    lines = []
    w = lines.append
    w("//" + "-" * 74)
    w("//\tarib_gaiji.h の実体 (**自動生成**。tools/regen-gaiji.sh で作り直す)")
    w("//" + "-" * 74)
    w("#include \"arib_gaiji.h\"")
    w("")
    w("const WCHAR TSMemoryAribGaijiPool[] = {")
    for i in range(0, len(pool), 12):
        w("\t" + " ".join("0x%04X," % u for u in pool[i:i + 12]))
    w("};")
    w("")
    w("namespace {")
    w("")
    w("//\t区点ごとの「Pool の何番目から何個か」")
    w("const DWORD g_Offset[%d] = {" % len(offset))
    for i in range(0, len(offset), 12):
        w("\t" + " ".join("%d," % v for v in offset[i:i + 12]))
    w("};")
    w("")
    w("const BYTE g_Length[%d] = {" % len(length))
    for i in range(0, len(length), 24):
        w("\t" + " ".join("%d," % v for v in length[i:i + 24]))
    w("};")
    w("")
    w("}\t// namespace")
    w("")
    w("")
    w("const WCHAR *TSMemoryAribKuTen(int Ku, int Ten, int *pLength)")
    w("{")
    w("\tif (pLength != nullptr)")
    w("\t\t*pLength = 0;")
    w("\tif (Ku < 1 || Ku > 94 || Ten < 1 || Ten > 94)")
    w("\t\treturn nullptr;")
    w("")
    w("\tconst int Index = (Ku - 1) * 94 + (Ten - 1);")
    w("\tif (g_Length[Index] == 0)")
    w("\t\treturn nullptr;")
    w("")
    w("\tif (pLength != nullptr)")
    w("\t\t*pLength = g_Length[Index];")
    w("\treturn &TSMemoryAribGaijiPool[g_Offset[Index]];")
    w("}")

    open(cpp, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")

    print("区点 %d 個中 %d 個が定義されています" % (len(table), defined))
    print("  %s" % out)
    print("  %s" % cpp)


main()
