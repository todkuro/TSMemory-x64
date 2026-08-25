# -*- coding: utf-8 -*-
"""zipdir.py <src-dir> <out.zip>  -- ディレクトリの中身を zip にまとめる"""
import os
import sys
import zipfile


def main(src, out):
    src = os.path.abspath(src)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for dirpath, _dirnames, filenames in os.walk(src):
            for name in sorted(filenames):
                full = os.path.join(dirpath, name)
                arc = os.path.relpath(full, src).replace(os.sep, "/")
                z.write(full, arc)
    print("wrote %s" % out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
