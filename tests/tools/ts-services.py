# -*- coding: utf-8 -*-
"""ts-services.py <file.ts> [scan-MB]

TS の PAT / PMT を読んでサービス (チャンネル) の一覧と、
各サービスの映像・音声 PID を表示する。

マルチ編成 (サブチャンネル) の TS かどうか、サービス毎に映像 PID が
分かれているかを確認する為の物。
"""
import sys

PACKET = 188


def packets(data):
    for i in range(0, len(data) - PACKET + 1, PACKET):
        p = data[i:i + PACKET]
        if p[0] != 0x47:
            continue
        yield p


def section_payload(p):
    """PSI セクションの payload を返す (payload_unit_start のもののみ)"""
    if (p[1] & 0x40) == 0:
        return None
    if (p[3] & 0x10) == 0:
        return None
    off = 4
    if p[3] & 0x20:
        off += 1 + p[4]
    if off >= PACKET:
        return None
    pointer = p[off]
    off += 1 + pointer
    if off >= PACKET:
        return None
    return p[off:]


def parse_pat(sec):
    if not sec or sec[0] != 0x00:
        return {}
    length = ((sec[1] & 0x0F) << 8) | sec[2]
    end = 3 + length - 4
    out = {}
    i = 8
    while i + 4 <= end:
        sid = (sec[i] << 8) | sec[i + 1]
        pid = ((sec[i + 2] & 0x1F) << 8) | sec[i + 3]
        if sid != 0:
            out[sid] = pid
        i += 4
    return out


STREAM_TYPES = {
    0x02: "MPEG-2 Video",
    0x0F: "AAC",
    0x1B: "H.264",
    0x24: "H.265",
    0x06: "private (caption etc.)",
    0x0D: "data carousel",
    0x0B: "DSM-CC",
}


def parse_pmt(sec):
    if not sec or sec[0] != 0x02 or len(sec) < 12:
        return None
    length = ((sec[1] & 0x0F) << 8) | sec[2]
    end = min(3 + length - 4, len(sec))
    sid = (sec[3] << 8) | sec[4]
    info_len = ((sec[10] & 0x0F) << 8) | sec[11]
    i = 12 + info_len
    streams = []
    while i + 5 <= end:
        stype = sec[i]
        pid = ((sec[i + 1] & 0x1F) << 8) | sec[i + 2]
        es_len = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4]
        streams.append((stype, pid))
        i += 5 + es_len
    return sid, streams


def continuation_payload(p):
    """PSI セクションの続き (payload_unit_start でない packet の payload)"""
    if (p[1] & 0x40) != 0 or (p[3] & 0x10) == 0:
        return None
    off = 4
    if p[3] & 0x20:
        off += 1 + p[4]
    if off >= PACKET:
        return None
    return p[off:]


def section_length(sec):
    if len(sec) < 3:
        return None
    return 3 + (((sec[1] & 0x0F) << 8) | sec[2])


def main(path, scan_mb=32):
    data = open(path, "rb").read(scan_mb * 1024 * 1024)
    print("%s (scanned %.1f MB)" % (path, len(data) / 1024 / 1024))

    pat = {}
    pmts = {}
    pmt_pids = {}
    partial = {}

    for p in packets(data):
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid == 0x0000 and not pat:
            sec = section_payload(p)
            if sec:
                pat = parse_pat(sec)
                pmt_pids = {v: k for k, v in pat.items()}
        elif pid in pmt_pids and pid not in pmts:
            #	PMT は 1 パケットに収まらない事があるので繋ぎ合わせる
            sec = section_payload(p)
            if sec is not None:
                partial[pid] = bytearray(sec)
            elif pid in partial:
                cont = continuation_payload(p)
                if cont:
                    partial[pid] += cont

            buf = partial.get(pid)
            if buf:
                need = section_length(buf)
                if need is not None and len(buf) >= need:
                    r = parse_pmt(bytes(buf[:need]))
                    if r:
                        pmts[pid] = r
                    partial.pop(pid, None)
        if pat and len(pmts) == len(pat):
            break

    if not pat:
        print("  PAT not found")
        return

    print("  services in PAT: %d" % len(pat))
    for sid, pmt_pid in sorted(pat.items()):
        print("  - service_id=%d (0x%04X)  PMT PID=0x%04X" % (sid, sid, pmt_pid))
        r = pmts.get(pmt_pid)
        if r is None:
            print("      (PMT not seen in the scanned range)")
            continue
        for stype, pid in r[1]:
            name = STREAM_TYPES.get(stype, "type 0x%02X" % stype)
            mark = "  <-- video" if stype in (0x02, 0x1B, 0x24) else ""
            print("      PID 0x%04X  %s%s" % (pid, name, mark))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 32)
