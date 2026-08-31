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


def read_clut(path):
    """`extern const ColorRGBA kB24ColorCLUT[][16] = { ... };` を読む"""
    text = open(path, encoding="utf-8-sig").read()
    m = re.search(r"kB24ColorCLUT\s*\[\s*\]\s*\[\s*16\s*\]\s*=\s*\{(.*?)\n\};",
                  text, re.S)
    if m is None:
        raise SystemExit("色表が見つかりません: %s" % path)
    out = [tuple(int(v) for v in c)
           for c in re.findall(r"ColorRGBA\(\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\)",
                               m.group(1))]
    if len(out) < 128:
        raise SystemExit("色が 128 個に足りません: %d" % len(out))
    return out[:128]


def read_macros(path):
    """`inline constexpr uint8_t kDefaultMacros[][20] = { {..}, ... };` を読む"""
    text = open(path, encoding="utf-8-sig").read()
    m = re.search(r"kDefaultMacros\s*\[\s*\]\s*\[\s*\d+\s*\]\s*=\s*\{(.*?)\n\};",
                  text, re.S)
    if m is None:
        raise SystemExit("マクロの表が見つかりません: %s" % path)
    out = []
    for row in re.findall(r"\{([^{}]*)\}", m.group(1)):
        out.append([int(v, 0) for v in re.findall(r"0[xX][0-9a-fA-F]+", row)])
    if len(out) != 16:
        raise SystemExit("マクロが 16 個ではありません: %d" % len(out))
    return out


#	未定義の区点はこの値で埋まっている (0 ではない)
REPLACEMENT = 0xFFFD


def is_halfwidth(cp):
    """半角の文字か。libaribcaption src/base/unicode_helper.hpp と同じ判定"""
    return ((cp != 0 and cp <= 0xFF)
            or 0xFF61 <= cp <= 0xFF9F
            or 0xFFE8 <= cp <= 0xFFEE)


def build_halfwidth(conv_h, kanji, singles):
    """全角 -> 半角の対応を作る。

    ARIB の「中型 (MSZ)」は字を横に潰す指定ではなく、**その字の半角形を
    使う**指定。潰すと `。` の丸が楕円になる (実機で発生)。
    libaribcaption も decoder_impl.cpp で
    `char_horizontal_scale_ * 2 == char_vertical_scale_` の時に
    半角の表へ差し替えている。ここではその対応だけを取り出す。

    どの文字集合から来たかに関わらず引けるよう、
    「全角の符号位置 -> 半角の符号位置」の 1 つの表にまとめる。
    """
    pairs = [
        #	区 1-2 の記号 (kKanjiTable の先頭 188 文字)
        (kanji[:2 * 94], read_u32_array(conv_h, "kKanjiSymbolsTable_Halfwidth")),
        #	ひらがな・カタカナの末尾 6 文字 (符号 0x79 以降)
        (singles["Hiragana"][0x79 - 0x21:],
         read_u32_array(conv_h, "kKanaSymbolsTable_Halfwidth")),
        (singles["Katakana"][0x79 - 0x21:],
         read_u32_array(conv_h, "kKanaSymbolsTable_Halfwidth")),
        #	**JIS X 0201 片仮名はここに入れない。**
        #	この表だけは全角の片仮名を丸ごと半角に写す。混ぜると
        #	片仮名集合 (ESC 0x31) から来た「ア」まで半角になってしまう。
        #	libaribcaption も文字集合ごとに使う表を分けており、
        #	片仮名集合では符号 0x79 以降の記号しか差し替えない。
        #	JIS X 0201 片仮名の分は TSMemoryAribJisKatakanaHalf で引く
        #	全角英数 -> ASCII
        (singles["Alnum"],
         read_u32_array(conv_h, "kAlphanumericTable_Halfwidth")),
    ]

    out = {}
    for full, halfwidth in pairs:
        for f, h in zip(full, halfwidth):
            if f in (0, REPLACEMENT) or h in (0, REPLACEMENT):
                continue
            if f == h or f >= 0x10000 or h >= 0x10000:
                continue
            if f in out and out[f] != h:
                #	**文字集合ごとに違う半角形を持つ字がある。**
                #	例: ￣ (U+FFE3) は区 1 の表では U+00AF、英数の表では
                #	U+203E。半角と判定される方を採る。どちらも半角
                #	(あるいはどちらも違う) なら先に入った方を残す
                if is_halfwidth(h) and not is_halfwidth(out[f]):
                    out[f] = h
                continue
            out[f] = h
    return sorted(out.items())


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
    macro_h = src + "/src/decoder/b24_macros.hpp"
    color_c = src + "/src/decoder/b24_colors.cpp"

    clut = read_clut(color_c)
    macros = read_macros(macro_h)
    kanji = read_u32_array(conv_h, "kKanjiTable")

    #	1 バイトの文字集合。**区 4 / 区 5 で代用してはいけない。**
    #	末尾に「ー」「、」等が入っており、区で引くと落ちる
    singles = [
        ("Alnum", "kAlphanumericTable_Fullwidth"),
        ("Hiragana", "kHiraganaTable"),
        ("Katakana", "kKatakanaTable"),
        ("JisKatakana", "kJISX0201KatakanaTable"),
        #	**JIS X 0201 片仮名だけは半角形の表も持つ。**
        #	この文字集合はもともと半角の片仮名で、中型 (MSZ) の時は
        #	丸ごと半角に写す。全角の片仮名集合とは扱いが違う
        ("JisKatakanaHalf", "kJISX0201KatakanaTable_Halfwidth"),
    ]
    single_tables = [(name, read_u32_array(conv_h, sym)) for name, sym in singles]
    for name, v in single_tables:
        if len(v) != 94:
            raise SystemExit("%s が 94 個ではありません: %d" % (name, len(v)))
    gaiji = read_u32_array(gaiji_h, "kAdditionalSymbolsTable_Unicode")

    half = build_halfwidth(conv_h, kanji, dict(single_tables))

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
    w("")
    w("//\t既定のマクロ (0x60-0x6F の 0-15) の中身を得る。")
    w("//\t**中身は文字集合を割り当てる ESC の並び**で、本文は入っていない。")
    w("//\tG3 の初期値がマクロなので `SS3 0x61` のような形で普通に出て来る。")
    w("const BYTE *TSMemoryAribDefaultMacro(int Index, int *pLength);")
    w("")
    w("//\t1 バイトの文字集合。符号 0x21-0x7E をそのまま渡す。")
    w("//")
    w("//\t**区 4 / 区 5 で代用してはいけない。**末尾に「ー」「、」等が")
    w("//\t入っており、区で引くと落ちる (実測: ステーション -> ステション)。")
    w("//\t英数は全角。中型 (MSZ) の時に半角へ差し替える")
    for name, _ in singles:
        w("WCHAR TSMemoryArib%s(BYTE Code);" % name)
    w("")
    w("//\t全角の文字を半角に直す。対応が無ければ 0 を返す (%d 文字)。" % len(half))
    w("//")
    w("//\t**中型 (MSZ) は「横に潰す」指定ではない。**")
    w("//\tその字の半角形を使うという意味で、潰すと `。` の丸が")
    w("//\t楕円になる (実機で発生)。libaribcaption も")
    w("//\tdecoder_impl.cpp で横倍率が縦の半分の時に半角の表へ")
    w("//\t差し替えており、描画側は半角になった字に横倍率を掛けない")
    w("//\t(text_renderer_directwrite.cpp の needless_horizontal_scaling)。")
    w("WCHAR TSMemoryAribHalfwidth(WCHAR c);")
    w("")
    w("//\t既に半角の字か。libaribcaption の")
    w("//\tsrc/base/unicode_helper.hpp IsHalfwidthCharacter と同じ判定。")
    w("//\t**これが真なら横倍率を掛けてはいけない**")
    w("bool TSMemoryAribIsHalfwidth(WCHAR c);")
    w("")
    w("//\tARIB の色表 (128 色)。色番号は「色配列 * 16 + 番号」。")
    w("//")
    w("//\t**背景は半透明の黒。**放送は色配列 4 を選び背景に 1 番を使うので、")
    w("//\t索引 65 = (0, 0, 0, α128) になる。字幕の背景が黒い箱に見えるのは")
    w("//\tこの為で、TVCaptionMod2 の「背景の不透明度」もこのアルファを触る。")
    w("struct TSMemoryAribColor {")
    w("\tBYTE R, G, B, A;")
    w("};")
    w("")
    w("//\t範囲の外なら透明 (A = 0) を返す")
    w("TSMemoryAribColor TSMemoryAribClut(int Index);")

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
    w("")
    w("")
    w("//\t既定のマクロ 16 個")
    w("namespace {")
    w("")
    for i, m in enumerate(macros):
        w("const BYTE g_Macro%X[] = { %s };"
          % (i, " ".join("0x%02X," % b for b in m)))
    w("")
    w("const BYTE * const g_Macros[16] = {")
    w("\t" + " ".join("g_Macro%X," % i for i in range(16)))
    w("};")
    w("")
    w("const int g_MacroLength[16] = {")
    w("\t" + " ".join("%d," % len(m) for m in macros))
    w("};")
    w("")
    w("}\t// namespace")
    w("")
    w("")
    w("const BYTE *TSMemoryAribDefaultMacro(int Index, int *pLength)")
    w("{")
    w("\tif (pLength != nullptr)")
    w("\t\t*pLength = 0;")
    w("\tif (Index < 0 || Index > 15)")
    w("\t\treturn nullptr;")
    w("\tif (pLength != nullptr)")
    w("\t\t*pLength = g_MacroLength[Index];")
    w("\treturn g_Macros[Index];")
    w("}")

    w("")
    w("")
    w("TSMemoryAribColor TSMemoryAribClut(int Index)")
    w("{")
    w("\tstatic const TSMemoryAribColor Table[128] = {")
    for i in range(0, 128, 4):
        w("\t\t" + " ".join("{%3d,%3d,%3d,%3d}," % c for c in clut[i:i + 4]))
    w("\t};")
    w("")
    w("\tif (Index < 0 || Index >= 128) {")
    w("\t\tconst TSMemoryAribColor Clear = { 0, 0, 0, 0 };")
    w("\t\treturn Clear;")
    w("\t}")
    w("\treturn Table[Index];")
    w("}")

    for name, values in single_tables:
        w("")
        w("")
        w("WCHAR TSMemoryArib%s(BYTE Code)" % name)
        w("{")
        w("\tstatic const WCHAR Table[94] = {")
        for i in range(0, 94, 10):
            w("\t\t" + " ".join(
                "0x%04X," % (0 if v in (0, REPLACEMENT) or v >= 0x10000 else v)
                for v in values[i:i + 10]))
        w("\t};")
        w("")
        w("\tif (Code < 0x21 || Code > 0x7E)")
        w("\t\treturn 0;")
        w("\treturn Table[Code - 0x21];")
        w("}")

    #	--- 全角 -> 半角 -----------------------------------------------------
    w("")
    w("")
    w("WCHAR TSMemoryAribHalfwidth(WCHAR c)")
    w("{")
    w("\t//\t{ 全角, 半角 } を符号順に並べた物。二分探索で引く")
    w("\tstatic const WCHAR Table[][2] = {")
    for i in range(0, len(half), 6):
        w("\t\t" + " ".join("{0x%04X,0x%04X}," % p for p in half[i:i + 6]))
    w("\t};")
    w("")
    w("\tint Lo = 0, Hi = %d;" % len(half))
    w("\twhile (Lo < Hi) {")
    w("\t\tconst int Mid = (Lo + Hi) / 2;")
    w("\t\tif (Table[Mid][0] == c)")
    w("\t\t\treturn Table[Mid][1];")
    w("\t\tif (Table[Mid][0] < c)")
    w("\t\t\tLo = Mid + 1;")
    w("\t\telse")
    w("\t\t\tHi = Mid;")
    w("\t}")
    w("\treturn 0;")
    w("}")
    w("")
    w("")
    w("bool TSMemoryAribIsHalfwidth(WCHAR c)")
    w("{")
    w("\treturn (c != 0 && c <= 0xFF)")
    w("\t\t|| (c >= 0xFF61 && c <= 0xFF9F)")
    w("\t\t|| (c >= 0xFFE8 && c <= 0xFFEE);")
    w("}")

    open(cpp, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")

    print("区点 %d 個中 %d 個が定義されています" % (len(table), defined))
    print("全角 -> 半角 の対応 %d 文字 (うち半角判定を通る物 %d)"
          % (len(half), sum(1 for _, h in half if is_halfwidth(h))))
    print("  %s" % out)
    print("  %s" % cpp)


main()
