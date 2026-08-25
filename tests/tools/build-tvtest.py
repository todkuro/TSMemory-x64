# -*- coding: utf-8 -*-
"""
TVTest (x64) をソースからビルドする。

システムに入っている Visual Studio ではなく、compilers/msvc に展開した
ポータブル MSVC だけを使う (tests/tools/setup-msvc.sh で用意)。
MSBuild は含まれないので tests/tools/vcxproj.py の簡易ビルダで .vcxproj を処理する。

  py tests/tools/build-tvtest.py [--config Release]

生成物は dist/TVTest-x64/ に出力される。
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vcxproj import Project, Builder  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
MSVC = os.path.join(ROOT, "compilers", "msvc")
SRC = os.path.join(ROOT, "third_party", "TVTest", "src")
OUT = os.path.join(ROOT, "dist", "TVTest-x64")
OBJ = os.path.join(ROOT, "build", "tvtest")
LIBS = os.path.join(ROOT, "build", "tvtest", "lib")


def msvc_env():
    """compilers/msvc/setup_x64.bat と同じ環境を組み立てる"""
    if not os.path.isdir(MSVC):
        sys.exit("error: %s not found. run tests/tools/setup-msvc.sh first." % MSVC)

    setup = os.path.join(MSVC, "setup_x64.bat")
    tools_ver = sdk_ver = None
    for line in open(setup, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("set VCToolsVersion="):
            tools_ver = line.split("=", 1)[1].strip()
        elif line.startswith("set WindowsSDKVersion="):
            sdk_ver = line.split("=", 1)[1].strip().rstrip("\\")
    if not tools_ver or not sdk_ver:
        sys.exit("error: cannot parse %s" % setup)

    vc = os.path.join(MSVC, "VC", "Tools", "MSVC", tools_ver)
    kits = os.path.join(MSVC, "Windows Kits", "10")

    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([
        os.path.join(vc, "bin", "Hostx64", "x64"),
        os.path.join(kits, "bin", sdk_ver, "x64"),
        os.path.join(kits, "bin", sdk_ver, "x64", "ucrt"),
        env.get("PATH", ""),
    ])
    env["INCLUDE"] = os.pathsep.join([
        os.path.join(vc, "include"),
        os.path.join(kits, "Include", sdk_ver, "ucrt"),
        os.path.join(kits, "Include", sdk_ver, "shared"),
        os.path.join(kits, "Include", sdk_ver, "um"),
        os.path.join(kits, "Include", sdk_ver, "winrt"),
        os.path.join(kits, "Include", sdk_ver, "cppwinrt"),
    ])
    env["LIB"] = os.pathsep.join([
        os.path.join(vc, "lib", "x64"),
        os.path.join(kits, "Lib", sdk_ver, "ucrt", "x64"),
        os.path.join(kits, "Lib", sdk_ver, "um", "x64"),
    ])
    # MSBuild を使わないので不要なものは消しておく
    env.pop("VCINSTALLDIR", None)
    env.pop("VSINSTALLDIR", None)
    return env


def write_version_hash():
    """PreBuildEvent 相当。git のハッシュを埋めたヘッダを作る (無ければ作らない)"""
    targets = [
        (os.path.join(SRC, "TVTestVersionHash.h"), "VERSION_HASH_A",
         os.path.join(ROOT, "third_party", "TVTest")),
        (os.path.join(SRC, "LibISDB", "LibISDB", "LibISDBVersionHash.hpp"), "LIBISDB_VERSION_HASH_",
         os.path.join(SRC, "LibISDB")),
    ]
    for path, macro, repo in targets:
        try:
            out = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=repo,
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            if out.returncode != 0:
                continue
            h = out.stdout.decode().strip()
            if h:
                with open(path, "w", encoding="ascii") as f:
                    f.write('#define %s "%s"\n' % (macro, h))
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="Release", choices=["Release", "Release_MD", "Debug"])
    ap.add_argument("--jobs", type=int, default=0)
    args = ap.parse_args()

    if not os.path.isdir(os.path.join(SRC, "LibISDB", "LibISDB")):
        sys.exit("error: LibISDB is missing. run tools/setup-tvtest-src.sh first.")

    env = msvc_env()
    write_version_hash()

    os.makedirs(LIBS, exist_ok=True)
    builder = Builder(env, LIBS, OBJ, jobs=args.jobs or None)

    proj = os.path.join(SRC, "LibISDB", "Projects")
    image = os.path.join(SRC, "TVTest_Image")

    def make(path, solution_dir):
        #	$(SolutionDir) は .sln のあるフォルダ。$(IntDir) は中間出力先。
        name = os.path.splitext(os.path.basename(path))[0]
        return Project(path, config=args.config, platform="x64", macros={
            "SolutionDir": os.path.join(solution_dir, ""),
            "IntDir": os.path.join(OBJ, name, ""),
            "OutDir": os.path.join(LIBS, ""),
        })

    # --- LibISDB とその依存 ---------------------------------------------
    static_libs = [
        os.path.join(proj, "BaseClasses.vcxproj"),
        os.path.join(proj, "liba52.vcxproj"),
        os.path.join(proj, "libfaad.vcxproj"),
        os.path.join(proj, "libmad.vcxproj"),
        os.path.join(proj, "fdk-aac.vcxproj"),
        os.path.join(proj, "LibISDB.vcxproj"),
        os.path.join(proj, "LibISDBWindows.vcxproj"),
        # --- 画像ライブラリ ---
        os.path.join(image, "zlib", "zlib.vcxproj"),
        os.path.join(image, "libjpeg", "libjpeg.vcxproj"),
        os.path.join(image, "libpng", "libpng.vcxproj"),
        os.path.join(image, "ImageLib.vcxproj"),
    ]

    for path in static_libs:
        p = make(path, proj if os.path.dirname(path) == proj else SRC)
        print("[lib ] %s" % p.name)
        builder.build(p)

    # --- TVTest_Image.dll ------------------------------------------------
    p = make(os.path.join(image, "TVTest_Image.vcxproj"), SRC)
    print("[dll ] %s" % p.name)
    dll = builder.build(p, lib_dirs=[LIBS],
                        def_file=os.path.join(image, "TVTest_Image.def"))

    # --- TVTest.exe ------------------------------------------------------
    p = make(os.path.join(SRC, "TVTest.vcxproj"), SRC)
    print("[exe ] %s" % p.name)
    exe = builder.build(p, lib_dirs=[LIBS])

    # --- 配置 ------------------------------------------------------------
    os.makedirs(OUT, exist_ok=True)
    for f in (exe, dll):
        shutil.copy2(f, OUT)
    data = os.path.join(ROOT, "third_party", "TVTest", "data")
    if os.path.isdir(data):
        for name in os.listdir(data):
            src = os.path.join(data, name)
            if os.path.isfile(src):
                shutil.copy2(src, OUT)
    os.makedirs(os.path.join(OUT, "Plugins"), exist_ok=True)

    print()
    print("built:")
    for name in sorted(os.listdir(OUT)):
        print("  %s" % name)


if __name__ == "__main__":
    main()
