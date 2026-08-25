# -*- coding: utf-8 -*-
"""
Shift_JIS のソースを UTF-8 に変換する。

clang は -finput-charset に UTF-8 しか受け付けないため、TVTest プラグイン側の
ソース (文字列リテラルに日本語を含む) は UTF-8 に変換しておく必要がある。
既に UTF-8 のファイルは変更しない。
"""
import os
import sys


def convert(path):
    raw = open(path, "rb").read()
    if not raw:
        return "empty"
    try:
        raw.decode("ascii")
        return "ascii"
    except UnicodeDecodeError:
        pass
    try:
        raw.decode("utf-8")
        return "utf-8 (already)"
    except UnicodeDecodeError:
        pass
    text = raw.decode("cp932")
    open(path, "wb").write(text.encode("utf-8"))
    return "converted"


def main(paths):
    for root in paths:
        if os.path.isfile(root):
            print("%-12s %s" % (convert(root), root))
            continue
        for dirpath, _dirnames, filenames in os.walk(root):
            for name in filenames:
                if os.path.splitext(name)[1].lower() in (".c", ".cpp", ".h", ".hpp", ".rc", ".def"):
                    p = os.path.join(dirpath, name)
                    print("%-12s %s" % (convert(p), p))


if __name__ == "__main__":
    main(sys.argv[1:] or ["src"])
