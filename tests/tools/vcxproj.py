# -*- coding: utf-8 -*-
"""
最小限の .vcxproj ビルダ。

ポータブル MSVC (compilers/msvc) には MSBuild が含まれないため、
.vcxproj から必要な情報だけを読み取って cl / rc / lib / link を直接叩く。

対応しているのは TVTest / LibISDB のプロジェクトが使っている範囲のみ:
  - ItemDefinitionGroup の ClCompile / ResourceCompile / Link / Lib
  - ClCompile / ResourceCompile / None のアイテム列挙
  - プリコンパイル済みヘッダ (Create / Use / NotUsing)
  - ConfigurationType (StaticLibrary / DynamicLibrary / Application)
MSBuild のプロパティ関数や条件式の一般評価は行わない。
"""
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor

NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}


def _cond_matches(elem, config, platform):
    cond = elem.get("Condition")
    if cond is None:
        return True
    want = "'$(Configuration)|$(Platform)'=='%s|%s'" % (config, platform)
    return cond.strip() == want


class Project:
    def __init__(self, path, config="Release", platform="x64", macros=None):
        self.path = os.path.abspath(path)
        self.dir = os.path.dirname(self.path)
        self.name = os.path.splitext(os.path.basename(self.path))[0]
        self.config = config
        self.platform = platform
        self.macros = dict(macros or {})
        self.macros.setdefault("Configuration", config)
        self.macros.setdefault("Platform", platform)
        self.macros.setdefault("ProjectDir", self.dir + os.sep)
        self.tree = ET.parse(self.path)
        self.root = self.tree.getroot()

    # -- プロパティ展開 ---------------------------------------------------
    def expand(self, text):
        if not text:
            return text
        for _ in range(8):
            m = re.search(r"\$\((\w+)\)", text)
            if m is None:
                break
            text = text[:m.start()] + self.macros.get(m.group(1), "") + text[m.end():]
        return text

    # -- ItemDefinitionGroup ----------------------------------------------
    def tool_settings(self, tool):
        """ItemDefinitionGroup から指定ツールの設定を辞書で得る"""
        out = {}
        for idg in self.root.findall("m:ItemDefinitionGroup", NS):
            if not _cond_matches(idg, self.config, self.platform):
                continue
            node = idg.find("m:" + tool, NS)
            if node is None:
                continue
            for child in node:
                if not _cond_matches(child, self.config, self.platform):
                    continue
                tag = child.tag.split("}")[-1]
                out[tag] = (child.text or "").strip()
        return out

    def _config_property(self, name, default):
        for pg in self.root.findall("m:PropertyGroup", NS):
            if pg.get("Label") != "Configuration":
                continue
            if not _cond_matches(pg, self.config, self.platform):
                continue
            node = pg.find("m:" + name, NS)
            if node is not None and node.text:
                return node.text.strip()
        return default

    def configuration_type(self):
        return self._config_property("ConfigurationType", "Application")

    def target_name(self):
        """出力ファイル名 (未指定ならプロジェクト名)。BaseClasses -> strmbase 等"""
        name = self.name
        for pg in self.root.findall("m:PropertyGroup", NS):
            node = pg.find("m:TargetName", NS)
            if node is None or not node.text:
                continue
            #	PropertyGroup 自体か TargetName 要素のどちらかに条件が付く
            if _cond_matches(pg, self.config, self.platform) and _cond_matches(node, self.config, self.platform):
                if pg.get("Condition") or node.get("Condition"):
                    name = node.text.strip()
        return name

    def character_set(self):
        #	MSBuild は CharacterSet に応じて _UNICODE / _MBCS を自動で定義する
        return self._config_property("CharacterSet", "MultiByte")

    # -- アイテム ----------------------------------------------------------
    def items(self, tool):
        """(絶対パス, 個別設定) のリスト"""
        result = []
        for ig in self.root.findall("m:ItemGroup", NS):
            for item in ig.findall("m:" + tool, NS):
                inc = item.get("Include")
                if inc is None:
                    continue
                settings = {}
                excluded = False
                for child in item:
                    if not _cond_matches(child, self.config, self.platform):
                        continue
                    tag = child.tag.split("}")[-1]
                    value = (child.text or "").strip()
                    if tag == "ExcludedFromBuild" and value.lower() == "true":
                        excluded = True
                    settings[tag] = value
                if excluded:
                    continue
                result.append((os.path.normpath(os.path.join(self.dir, inc)), settings))
        return result


#---------------------------------------------------------------------------
#	cl / rc / lib / link の引数組み立て
#---------------------------------------------------------------------------
RUNTIME_FLAGS = {
    "MultiThreaded": "/MT",
    "MultiThreadedDebug": "/MTd",
    "MultiThreadedDLL": "/MD",
    "MultiThreadedDebugDLL": "/MDd",
}

STANDARD_FLAGS = {
    "stdcpp14": "/std:c++14",
    "stdcpp17": "/std:c++17",
    "stdcpp20": "/std:c++20",
    "stdcpplatest": "/std:c++latest",
}


def _split_list(value, project, extra_relative_to=None):
    out = []
    for part in (value or "").replace("\r", "").split(";"):
        part = part.strip()
        if not part or part.startswith("%("):
            continue
        out.append(project.expand(part))
    return out


def cl_flags(project, settings, obj_dir):
    flags = ["cl.exe", "/nologo", "/c", "/EHsc", "/GS-", "/Gy", "/GF"]

    opt = settings.get("Optimization", "MaxSpeed")
    flags.append({"Disabled": "/Od", "MinSpace": "/O1", "MaxSpeed": "/O2", "Full": "/Ox"}.get(opt, "/O2"))

    flags.append(RUNTIME_FLAGS.get(settings.get("RuntimeLibrary", "MultiThreaded"), "/MT"))

    std = settings.get("LanguageStandard")
    if std:
        flags.append(STANDARD_FLAGS.get(std, "/std:c++latest"))

    if settings.get("ConformanceMode", "").lower() == "true":
        flags.append("/permissive-")
    if settings.get("FloatingPointModel", "") == "Fast":
        flags.append("/fp:fast")
    if settings.get("WholeProgramOptimization", "").lower() == "true":
        flags.append("/GL")

    charset = project.character_set()
    if charset == "Unicode":
        flags += ["/D_UNICODE", "/DUNICODE"]
    elif charset == "MultiByte":
        flags.append("/D_MBCS")

    for d in _split_list(settings.get("PreprocessorDefinitions"), project):
        flags.append("/D" + d)
    for i in _split_list(settings.get("AdditionalIncludeDirectories"), project):
        flags.append("/I" + os.path.normpath(os.path.join(project.dir, i)))

    extra = project.expand(settings.get("AdditionalOptions", ""))
    if extra:
        flags += [o for o in extra.split() if not o.startswith("%(")]

    # 警告は出させるが止めない
    flags += ["/W3", "/wd4996", "/wd4267", "/wd4244", "/wd4018"]
    flags.append("/Fo" + obj_dir + os.sep)
    return flags


#---------------------------------------------------------------------------
#	ビルド実行
#---------------------------------------------------------------------------
class Builder:
    def __init__(self, env, out_dir, obj_root, jobs=None):
        self.env = env
        self.out_dir = os.path.abspath(out_dir)
        self.obj_root = os.path.abspath(obj_root)
        self.jobs = jobs or (os.cpu_count() or 4)
        self._tools = {}
        os.makedirs(self.out_dir, exist_ok=True)

    def _resolve(self, tool):
        """Windows の CreateProcess は env の PATH を見ないので絶対パスにする"""
        if tool in self._tools:
            return self._tools[tool]
        for d in self.env.get("PATH", "").split(os.pathsep):
            p = os.path.join(d, tool)
            if os.path.isfile(p):
                self._tools[tool] = p
                return p
        raise SystemExit("error: %s not found in the portable MSVC toolchain" % tool)

    def run(self, args, cwd):
        args = [self._resolve(args[0])] + list(args[1:])
        proc = subprocess.run(args, cwd=cwd, env=self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return proc.returncode, proc.stdout.decode("cp932", "replace")

    def build(self, project, extra_defines=(), extra_includes=(), extra_libs=(),
              lib_dirs=(), out_name=None, subsystem=None, def_file=None):
        obj_dir = os.path.join(self.obj_root, project.name)
        os.makedirs(obj_dir, exist_ok=True)

        cl_settings = project.tool_settings("ClCompile")
        base_flags = cl_flags(project, cl_settings, obj_dir)
        for d in extra_defines:
            base_flags.append("/D" + d)
        for i in extra_includes:
            base_flags.append("/I" + os.path.abspath(i))

        sources = project.items("ClCompile")
        pch_header = cl_settings.get("PrecompiledHeaderFile") or "stdafx.h"
        use_pch = cl_settings.get("PrecompiledHeader", "NotUsing")
        pch_path = os.path.join(obj_dir, project.name + ".pch")

        # 1. PCH の作成
        if use_pch == "Use":
            creator = None
            for src, s in sources:
                if s.get("PrecompiledHeader") == "Create":
                    creator = src
                    break
            if creator is None:
                for src, _s in sources:
                    if os.path.basename(src).lower() in ("stdafx.cpp", "pch.cpp"):
                        creator = src
                        break
            if creator is not None:
                args = base_flags + ["/Yc" + pch_header, "/Fp" + pch_path, creator]
                rc, log = self.run(args, project.dir)
                if rc != 0:
                    print(log)
                    raise SystemExit("failed to create PCH for %s" % project.name)
                sources = [(s, st) for (s, st) in sources if s != creator]
                objs_pch = [os.path.join(obj_dir, os.path.splitext(os.path.basename(creator))[0] + ".obj")]
            else:
                use_pch = "NotUsing"
                objs_pch = []
        else:
            objs_pch = []

        # 2. 各ソースのコンパイル
        def compile_one(item):
            src, st = item
            flags = list(base_flags)

            #	ファイル個別の設定 (%(...) は継承なので既定値に追加する形になる)
            for d in _split_list(st.get("PreprocessorDefinitions"), project):
                flags.append("/D" + d)
            for i in _split_list(st.get("AdditionalIncludeDirectories"), project):
                flags.append("/I" + os.path.normpath(os.path.join(project.dir, i)))
            for w in _split_list(st.get("DisableSpecificWarnings"), project):
                flags.append("/wd" + w)
            extra = project.expand(st.get("AdditionalOptions", ""))
            if extra:
                flags += [o for o in extra.split() if not o.startswith("%(")]

            mode = st.get("PrecompiledHeader", use_pch)
            if mode == "Use":
                flags += ["/Yu" + pch_header, "/Fp" + pch_path]
            else:
                flags.append("/Y-")
            if src.lower().endswith(".c"):
                flags = [f for f in flags if not f.startswith("/std:") and f not in ("/EHsc", "/permissive-")]
                flags.append("/TC")
            return src, self.run(flags + [src], project.dir)

        objs = list(objs_pch)
        failures = []
        with ThreadPoolExecutor(max_workers=self.jobs) as pool:
            for src, (rc, log) in pool.map(compile_one, sources):
                objs.append(os.path.join(obj_dir, os.path.splitext(os.path.basename(src))[0] + ".obj"))
                if rc != 0:
                    failures.append((src, log))
        if failures:
            for src, log in failures[:5]:
                print("--- %s" % src)
                print(log)
            raise SystemExit("%s: %d source file(s) failed to compile" % (project.name, len(failures)))

        # 3. リソース
        res_settings = project.tool_settings("ResourceCompile")
        res_files = []
        for rc_src, _st in project.items("ResourceCompile"):
            args = ["rc.exe", "/nologo"]
            for d in _split_list(res_settings.get("PreprocessorDefinitions"), project):
                args.append("/d" + d)
            for i in _split_list(res_settings.get("AdditionalIncludeDirectories"), project):
                args.append("/i" + os.path.normpath(os.path.join(project.dir, project.expand(i))))
            args.append("/i" + project.dir)
            res = os.path.join(obj_dir, os.path.splitext(os.path.basename(rc_src))[0] + ".res")
            args += ["/fo" + res, rc_src]
            rc, log = self.run(args, project.dir)
            if rc != 0:
                print(log)
                raise SystemExit("%s: resource compile failed" % project.name)
            res_files.append(res)

        # 4. リンク
        conf_type = project.configuration_type()
        name = out_name or project.target_name()

        if conf_type == "StaticLibrary":
            target = os.path.join(self.out_dir, name + ".lib")
            args = ["lib.exe", "/nologo", "/OUT:" + target] + objs + res_files
            rc, log = self.run(args, project.dir)
            if rc != 0:
                print(log)
                raise SystemExit("%s: lib failed" % project.name)
            return target

        link_settings = project.tool_settings("Link")
        ext = ".dll" if conf_type == "DynamicLibrary" else ".exe"
        target = os.path.join(self.out_dir, name + ext)

        args = ["link.exe", "/nologo", "/OUT:" + target, "/MACHINE:X64", "/INCREMENTAL:NO"]
        if conf_type == "DynamicLibrary":
            args.append("/DLL")
        if def_file:
            args.append("/DEF:" + os.path.abspath(def_file))

        sub = subsystem or link_settings.get("SubSystem", "Windows")
        minver = link_settings.get("MinimumRequiredVersion")
        args.append("/SUBSYSTEM:" + sub.upper() + (("," + minver + ".00") if minver else ""))

        #	遅延読み込み (これが無いと mf.dll 等が無い環境で起動出来ない)
        delay = _split_list(link_settings.get("DelayLoadDLLs"), project)
        for d in delay:
            args.append("/DELAYLOAD:" + d)
        if delay:
            args.append("delayimp.lib")

        if link_settings.get("OptimizeReferences", "").lower() == "true":
            args.append("/OPT:REF")
        if link_settings.get("EnableCOMDATFolding", "").lower() == "true":
            args.append("/OPT:ICF")
        if link_settings.get("LargeAddressAware", "").lower() == "true":
            args.append("/LARGEADDRESSAWARE")
        if link_settings.get("RandomizedBaseAddress", "").lower() == "false":
            args.append("/DYNAMICBASE:NO")
        stack = link_settings.get("StackReserveSize")
        if stack:
            args.append("/STACK:" + stack)
        extra_link = project.expand(link_settings.get("AdditionalOptions", ""))
        if extra_link:
            args += [o for o in extra_link.split() if not o.startswith("%(")]

        for d in lib_dirs:
            args.append("/LIBPATH:" + os.path.abspath(d))
        args.append("/LIBPATH:" + self.out_dir)

        args += objs + res_files
        args += list(extra_libs)
        #	MSBuild が既定で足すライブラリ
        args += ["kernel32.lib", "user32.lib", "gdi32.lib", "winspool.lib", "comdlg32.lib",
                 "advapi32.lib", "shell32.lib", "ole32.lib", "oleaut32.lib", "uuid.lib",
                 "odbc32.lib", "odbccp32.lib"]
        for dep in _split_list(link_settings.get("AdditionalDependencies"), project):
            args.append(dep)

        rc, log = self.run(args, project.dir)
        if rc != 0:
            print(log)
            raise SystemExit("%s: link failed" % project.name)

        self._embed_manifest(project, target, conf_type)
        return target

    def _embed_manifest(self, project, target, conf_type):
        """アプリケーションマニフェストを埋め込む。

        MSBuild は既定のマニフェストと AdditionalManifestFiles を mt.exe で
        合成して埋め込む。これが無いと COMCTL32 v6 が有効にならず、
        v6 にしか無い序数 (380 等) の解決に失敗して起動出来ない。
        """
        settings = project.tool_settings("Manifest")
        files = _split_list(settings.get("AdditionalManifestFiles"), project)
        files = [os.path.normpath(os.path.join(project.dir, f)) for f in files]
        files = [f for f in files if os.path.isfile(f)]
        if not files:
            return

        resource_id = 1 if conf_type != "DynamicLibrary" else 2
        args = ["mt.exe", "-nologo"]
        for f in files:
            args += ["-manifest", f]
        args.append("-outputresource:%s;#%d" % (target, resource_id))
        rc, log = self.run(args, project.dir)
        if rc != 0:
            print(log)
            raise SystemExit("%s: embedding the manifest failed" % project.name)
