# -*- coding: utf-8 -*-
"""192 バイト/パケットの TS (BDAV, *.m2ts) を 188 バイトに直す。

BDAV は各 TS パケットの前に 4 バイトの TP_extra_header が付いた形で
記録されている。それを外すだけ。

    python m2ts2ts.py <入力.m2ts> <出力.ts>

先頭が同期していない事があるので、最初に 0x47 が 192 バイト周期で
並ぶ位置を探してから切り出す。
"""
import os
import sys

PACKET = 192
BODY = 188
CHUNK = PACKET * 4096


def find_offset(data):
    """192 バイト周期で 0x47 が並び始める位置を返す。見つからなければ -1"""
    limit = min(len(data), PACKET * 2)
    for base in range(limit):
        if base + PACKET * 8 > len(data):
            break
        #   TP_extra_header 4 バイトの次が同期バイト
        if all(data[base + i * PACKET + 4] == 0x47 for i in range(8)):
            return base
    return -1


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)

    src, dst = sys.argv[1], sys.argv[2]

    with open(src, "rb") as fin:
        head = fin.read(PACKET * 16)
        offset = find_offset(head)
        if offset < 0:
            sys.exit("error: 192 バイト周期の同期が見つかりません: %s" % src)

        fin.seek(offset)

        written = 0
        dropped = 0
        tmp = dst + ".part"
        with open(tmp, "wb") as fout:
            rest = b""
            while True:
                buf = rest + fin.read(CHUNK)
                if len(buf) < PACKET:
                    break
                n = len(buf) // PACKET
                rest = buf[n * PACKET:]
                out = bytearray()
                for i in range(n):
                    p = buf[i * PACKET + 4: i * PACKET + 4 + BODY]
                    if p[0] != 0x47:
                        #   同期が外れた分は捨てる (数だけ数えて報告する)
                        dropped += 1
                        continue
                    out += p
                fout.write(out)
                written += len(out) // BODY

    os.replace(tmp, dst)

    print("  %s -> %s" % (os.path.basename(src), os.path.basename(dst)))
    print("  %d packets (%.1f MB)%s"
          % (written, written * BODY / 1024.0 / 1024.0,
             "" if dropped == 0 else ", %d dropped (out of sync)" % dropped))
    if offset != 0:
        print("  started at offset %d" % offset)


if __name__ == "__main__":
    main()
