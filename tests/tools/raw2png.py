# -*- coding: utf-8 -*-
"""raw2png.py <in.raw> <width> <height> <out.png> [scale-divisor]

RGB24 のべた画像を PNG にする (標準ライブラリのみ)。
test_decode が書き出したフレームを目視確認するためのもの。
"""
import struct
import sys
import zlib


def write_png(path, width, height, rows):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main(src, width, height, out, div=1):
    data = open(src, "rb").read()
    need = width * height * 3
    if len(data) < need:
        sys.exit("raw file too small: %d < %d" % (len(data), need))

    ow, oh = width // div, height // div
    rows = []
    for y in range(oh):
        base = (y * div) * width * 3
        if div == 1:
            rows.append(data[base:base + width * 3])
        else:
            row = bytearray()
            for x in range(ow):
                o = base + (x * div) * 3
                row += data[o:o + 3]
            rows.append(bytes(row))
    write_png(out, ow, oh, rows)
    print("wrote %s (%dx%d)" % (out, ow, oh))


if __name__ == "__main__":
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4],
         int(sys.argv[5]) if len(sys.argv) > 5 else 1)
