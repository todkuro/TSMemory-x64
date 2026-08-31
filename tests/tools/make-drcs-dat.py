# -*- coding: utf-8 -*-
#
#	検証用の TSMemoryDRCS.dat を作る。
#
#	  compilers/python/python.exe tests/tools/make-drcs-dat.py <出力先>
#
#	**外字の起動時の経路を、放送を待たずに確かめる為の道具。**
#	外字の字形は放送からしか手に入らず、しかも対応表で本物の文字に
#	置き換わる物は貯め込みに入らない。その為「貯め込みを読んで
#	フォントとして登録する」経路 (RegisterPlugin) を試したくても、
#	手元に .dat が無いと何も起きない。ここで作った物を置けば、
#	AviUtl2 の起動ログで下記まで確かめられる。
#
#	  TSMemory: 外字の字形を 2 個読み込みました : ...\TSMemoryDRCS.dat
#	  TSMemory: 外字のフォント「TSMemory DRCS」を 2 字形で登録しました
#
#	字形は幾何学模様にしてある。**放送の字形とぶつからない**ので、
#	対応表に当たったり本物の字幕を邪魔したりしない。
#	テキストオブジェクトで
#	  <@TSMemory DRCS> + U+E000 (1 個目) / U+E001 (2 個目)
#	と書けば目で見る事も出来る。
#
#	書式は src/aviutl2/caption/drcs_store.cpp と揃えてある。
#	  "TSMDRCS1" / 字形数 (LE32) /
#	  [深さ, 幅, 高さ, 詰め物, バイト数 (LE32), ビットマップ] * 字形数
#
import struct
import sys

#	実測の放送に合わせる (36x36 / 深さの値 2 = 4 階調 = 2 ビット/画素)。
#	drcs_ttf.cpp は値が 2 以上の画素を塗る (FillThreshold)
SIZE = 36
DEPTH = 2			# ARIB の深さの生値。階調数 = DEPTH + 2
BITS = 2
LEVEL_ON = 3		# 塗る (閾値は 2)


def pack(pixels):
    """画素 (0-3) の並びを MSB 詰めの 2 ビット/画素にする"""
    out = bytearray()
    acc = 0
    n = 0
    for v in pixels:
        acc = (acc << BITS) | (v & ((1 << BITS) - 1))
        n += BITS
        if n == 8:
            out.append(acc)
            acc = 0
            n = 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def glyph_box_slash():
    """太い枠 + 左上から右下への斜線"""
    px = []
    for y in range(SIZE):
        for x in range(SIZE):
            edge = x < 3 or y < 3 or x >= SIZE - 3 or y >= SIZE - 3
            slash = abs(x - y) <= 2
            px.append(LEVEL_ON if (edge or slash) else 0)
    return px


def glyph_diamond():
    """塗り潰した菱形"""
    px = []
    c = (SIZE - 1) / 2.0
    for y in range(SIZE):
        for x in range(SIZE):
            px.append(LEVEL_ON if abs(x - c) + abs(y - c) <= c else 0)
    return px


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make-drcs-dat.py <出力先の TSMemoryDRCS.dat>")
    out = sys.argv[1]

    glyphs = [glyph_box_slash(), glyph_diamond()]

    data = bytearray(b"TSMDRCS1")
    data += struct.pack("<I", len(glyphs))
    for px in glyphs:
        bits = pack(px)
        assert len(bits) == SIZE * SIZE * BITS // 8, len(bits)
        data += bytes([DEPTH, SIZE, SIZE, 0])
        data += struct.pack("<I", len(bits))
        data += bits

    with open(out, "wb") as f:
        f.write(data)

    print("%d 字形 / %d バイト -> %s" % (len(glyphs), len(data), out))
    print("AviUtl2 を起動し直すと登録されます。")
    print("見る場合はテキストオブジェクトに <@TSMemory DRCS> と U+E000 / U+E001。")


main()
