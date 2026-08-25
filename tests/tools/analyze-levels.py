# -*- coding: utf-8 -*-
"""analyze-levels.py <image.bmp|image.png> ...

AviUtl2 が保存した画像の黒レベル・白レベルを調べる。

YUY2 (リミテッドレンジ Y:16-235) を渡した時、AviUtl2 が 0-255 へ伸張して
いるかどうかを判定する為の物。
  伸張あり : 黒帯が RGB 0 付近
  伸張なし : 黒帯が RGB 16 付近で止まる (眠い絵になる)

BMP は自前で読む。PNG は zlib で展開して読む (どちらも標準ライブラリのみ)。
"""
import struct
import sys
import zlib


def read_bmp(path):
    d = open(path, "rb").read()
    if d[:2] != b"BM":
        raise ValueError("not a BMP")
    offset = struct.unpack_from("<I", d, 10)[0]
    hdr_size = struct.unpack_from("<I", d, 14)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp != 24:
        raise ValueError("only 24bpp BMP is supported (got %d)" % bpp)
    bottom_up = h > 0
    h = abs(h)
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h):
        src = offset + (h - 1 - y if bottom_up else y) * stride
        line = d[src:src + w * 3]
        # BMP はメモリ上 B,G,R の順。PNG と揃える為に R,G,B に直す
        rows.append(bytes(line[i + 2 - j] for i in range(0, w * 3, 3) for j in range(3)))
    return w, h, rows


def read_png(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    idat = b""
    w = h = bitdepth = colortype = None
    while pos < len(d):
        (length,) = struct.unpack_from(">I", d, pos)
        tag = d[pos + 4:pos + 8]
        data = d[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            w, h, bitdepth, colortype = struct.unpack(">IIBB", data[:10])
        elif tag == b"IDAT":
            idat += data
        elif tag == b"IEND":
            break
        pos += 12 + length
    if bitdepth != 8 or colortype not in (2, 6):
        raise ValueError("only 8bit RGB/RGBA PNG is supported")

    channels = 3 if colortype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * channels
    rows = []
    prev = bytearray(stride)
    pos = 0
    for _y in range(h):
        f = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            x = line[i]
            if f == 1:
                x += a
            elif f == 2:
                x += b
            elif f == 3:
                x += (a + b) >> 1
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                x += a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[i] = x & 0xFF
        prev = line
        if channels == 3:
            rows.append(bytes(line))
        else:
            rows.append(bytes(b for i in range(0, stride, 4) for b in line[i:i + 3]))
    return w, h, rows


def analyze(path):
    if path.lower().endswith(".bmp"):
        w, h, rows = read_bmp(path)
    else:
        w, h, rows = read_png(path)

    hist = [0] * 256
    total = 0
    for r in rows:
        for v in r:
            hist[v] += 1
            total += 1

    lo = next(v for v in range(256) if hist[v])
    hi = next(v for v in range(255, -1, -1) if hist[v])

    def pct(v):
        return sum(hist[:v + 1]) * 100.0 / total

    print("%s  %dx%d" % (path, w, h))
    print("  min=%d max=%d" % (lo, hi))
    print("  <=0 : %.3f%%   <=8 : %.3f%%   <=16 : %.3f%%   <=20 : %.3f%%"
          % (pct(0), pct(8), pct(16), pct(20)))
    print("  >=235: %.3f%%  >=250: %.3f%%  >=255: %.3f%%"
          % (100 - pct(234), 100 - pct(249), 100 - pct(254)))

    # 画像の四隅 8x8 の平均 (レターボックスの黒帯を見る)
    for name, (x0, y0) in (("top-left", (0, 0)), ("top-right", (w - 8, 0)),
                           ("bottom-left", (0, h - 8)), ("bottom-right", (w - 8, h - 8))):
        s = n = 0
        for y in range(y0, y0 + 8):
            row = rows[y]
            for x in range(x0, x0 + 8):
                s += row[x * 3] + row[x * 3 + 1] + row[x * 3 + 2]
                n += 3
        print("  corner %-13s avg=%.1f" % (name, s / n))
    print()


if __name__ == "__main__":
    for p in sys.argv[1:]:
        analyze(p)
