# -*- coding: utf-8 -*-
"""
TVTestSrc (MPEG-2 VIDEO VFAPI Plug-In) の 64bit 化パッチ。
src/m2v/ に対して冪等に適用する。ファイルは Shift_JIS のため latin-1 で
バイト列としてそのまま読み書きする (置換対象は全て ASCII)。
"""
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "m2v")


#   marker の代わりに渡す印 (下記 patch() の説明を参照)
BY_OLD = object()


def patch(name, subs):
    """subs は (old, new) か (old, new, marker) の並び。

    marker は「適用済みかどうか」の判定方法を指定する。

      省略      new が在れば適用済みと見なす。
                old が new の一部になる置換 (行の追加など) で
                重複適用を防ぐ為の既定の判定。

      文字列    その 1 行が在れば適用済み。
                **同じファイルの後続のパッチが同じ範囲を更に書き換える**
                場合、new は原形のまま残らず偽の "not found" が出る。
                その置換が入れた物にしか現れない 1 行を指定する
                (改行コードに依存しないよう 1 行で書く事)。

      BY_OLD    old の有無だけで判断する (old は置換後に消える前提)。
                **new が元のソースの別の箇所に既に在る**場合に使う。
                既定の判定だと初回から「適用済み」と誤判定され、
                警告も出ないまま黙って飛ばされる。

    どちらの marker も、判定を間違えると「黙って当たらない」形で
    壊れる。pristine なソースへ当てて意図どおりかを確かめる事。
    """
    path = os.path.join(ROOT, name)
    data = open(path, "rb").read().decode("latin-1")
    for sub in subs:
        old, new = sub[0], sub[1]
        marker = sub[2] if len(sub) > 2 else None

        if isinstance(marker, str):
            if marker in data:
                continue
            if "\n" in marker:
                sys.exit("marker に改行は使えません: %r" % marker[:70])

        # ファイルによって改行コードが混在しているので LF / CRLF 両方を試す
        done = False
        for eol in ("\n", "\r\n"):
            o, n = old.replace("\n", eol), new.replace("\n", eol)
            if marker is None and n in data:
                done = True
                break
            if o in data:
                data, done = data.replace(o, n), True
                break
        # BY_OLD は「old が無い = 適用済み」なので警告しない
        if not done and marker is not BY_OLD:
            print("  !! not found in %s: %r" % (name, old[:70]))
    blob = data.encode("latin-1")
    open(path, "wb").write(blob)
    print("  patched %s" % name)


print("[patch64] applying 64bit fixes")

# --- インクルードガードの綴り間違い (警告が出るだけだが直しておく) -------
patch("mpeg_audio.h", [
    ("#define MEPG_AUDIO_H", "#define MPEG_AUDIO_H"),
])

# --- 共有メモリハンドル: int -> intptr_t --------------------------------
patch("shared_memory.h", [
    ("#define SHARED_MEMORY_H",
     "#define SHARED_MEMORY_H\n\n#include <stdint.h>"),
    ("int open_shared_memory(const char *name);", "intptr_t open_shared_memory(const char *name);"),
    ("int shm_close(int id);", "int shm_close(intptr_t id);"),
    ("int shm_read(int id,void *buf,int length);", "int shm_read(intptr_t id,void *buf,int length);"),
    ("__int64 shm_tell(int id);", "__int64 shm_tell(intptr_t id);"),
    ("__int64 shm_seek(int id,__int64 offset,int origin);", "__int64 shm_seek(intptr_t id,__int64 offset,int origin);"),
])

patch("shared_memory.c", [
    ("int open_shared_memory(const char *name)\n{", "intptr_t open_shared_memory(const char *name)\n{"),
    ("\tinfo=malloc(sizeof(shm_info));", "\tinfo=(shm_info*)malloc(sizeof(shm_info));"),
    ("\t\tbuf=malloc(used);", "\t\tbuf=(void*)malloc(used);"),
    ("\treturn (int)info;", "\treturn (intptr_t)info;"),
    ("int shm_close(int id)", "int shm_close(intptr_t id)"),
    ("int shm_read(int id,void *buf,int length)", "int shm_read(intptr_t id,void *buf,int length)"),
    ("__int64 shm_tell(int id)", "__int64 shm_tell(intptr_t id)"),
    ("__int64 shm_seek(int id,__int64 offset,int origin)", "__int64 shm_seek(intptr_t id,__int64 offset,int origin)"),
    ("\tif (info->cur_pos+length>info->size)\n\t\tlength=info->size-info->cur_pos;",
     "\tif (info->cur_pos+(size_t)length>info->size)\n\t\tlength=(int)(info->size-info->cur_pos);"),
])

# --- TS / PS のストリームハンドル: int -> intptr_t ----------------------
#     ハンドルの実体は構造体へのポインタなので x64 では int に収まらない
patch("transport_stream.h", [
    ('#include "stream_type.h"', '#include <stdint.h>\n#include "stream_type.h"'),
    ("extern int ts_open(const char *filename, int stream_type);", "extern intptr_t ts_open(const char *filename, int stream_type);"),
    ("extern int ts_close(int in);", "extern int ts_close(intptr_t in);"),
    ("extern int ts_read(int in, void *data, unsigned int count);", "extern int ts_read(intptr_t in, void *data, unsigned int count);"),
    ("extern __int64 ts_seek(int in, __int64 offset, int origin);", "extern __int64 ts_seek(intptr_t in, __int64 offset, int origin);"),
    ("extern __int64 ts_tell(int in);", "extern __int64 ts_tell(intptr_t in);"),
])

patch("transport_stream.c", [
    ("int ts_open(const char *filename, int stream_type);", "intptr_t ts_open(const char *filename, int stream_type);"),
    ("int ts_close(int in);", "int ts_close(intptr_t in);"),
    ("int ts_read(int in, void *data, unsigned int count);", "int ts_read(intptr_t in, void *data, unsigned int count);"),
    ("__int64 ts_seek(int in, __int64 offset, int origin);", "__int64 ts_seek(intptr_t in, __int64 offset, int origin);"),
    ("__int64 ts_tell(int in);", "__int64 ts_tell(intptr_t in);"),
    ("int ts_open(const char *filename, int stream_type)\n{", "intptr_t ts_open(const char *filename, int stream_type)\n{"),
    ("int ts_close(int in)\n{", "int ts_close(intptr_t in)\n{"),
    ("int ts_read(int in, void *data, unsigned int count)\n{", "int ts_read(intptr_t in, void *data, unsigned int count)\n{"),
    ("__int64 ts_seek(int in, __int64 offset, int origin)\n{", "__int64 ts_seek(intptr_t in, __int64 offset, int origin)\n{"),
    ("__int64 ts_tell(int in)\n{", "__int64 ts_tell(intptr_t in)\n{"),
    ("ts_close((int)ts);", "ts_close((intptr_t)ts);"),
    ("ts_seek((int)ts, 0, SEEK_SET);", "ts_seek((intptr_t)ts, 0, SEEK_SET);"),
    ("ts_seek((int)ts, offset, SEEK_SET);", "ts_seek((intptr_t)ts, offset, SEEK_SET);"),
    ("\treturn (int)ts;", "\treturn (intptr_t)ts;"),
])

patch("program_stream.h", [
    ('#include "stream_type.h"', '#include <stdint.h>\n#include "stream_type.h"'),
    ("extern int ps_open(const char *filename, int stream_type);", "extern intptr_t ps_open(const char *filename, int stream_type);"),
    ("extern int ps_close(int in);", "extern int ps_close(intptr_t in);"),
    ("extern int ps_read(int in, void *data, unsigned int count);", "extern int ps_read(intptr_t in, void *data, unsigned int count);"),
    ("extern __int64 ps_seek(int in, __int64 offset, int origin);", "extern __int64 ps_seek(intptr_t in, __int64 offset, int origin);"),
    ("extern __int64 ps_tell(int in);", "extern __int64 ps_tell(intptr_t in);"),
])

patch("program_stream.c", [
    ("int ps_open(const char *filename, int stream_type);", "intptr_t ps_open(const char *filename, int stream_type);"),
    ("int ps_close(int in);", "int ps_close(intptr_t in);"),
    ("int ps_read(int in, void *data, unsigned int count);", "int ps_read(intptr_t in, void *data, unsigned int count);"),
    ("__int64 ps_seek(int in, __int64 offset, int origin);", "__int64 ps_seek(intptr_t in, __int64 offset, int origin);"),
    ("__int64 ps_tell(int in);", "__int64 ps_tell(intptr_t in);"),
    ("int ps_open(const char *filename, int stream_type)\n{", "intptr_t ps_open(const char *filename, int stream_type)\n{"),
    ("int  ps_close(int in)\n{", "int  ps_close(intptr_t in)\n{"),
    ("int  ps_read(int in, void *data, unsigned int count)\n{", "int  ps_read(intptr_t in, void *data, unsigned int count)\n{"),
    ("__int64  ps_seek(int in, __int64 offset, int origin)\n{", "__int64  ps_seek(intptr_t in, __int64 offset, int origin)\n{"),
    ("__int64  ps_tell(int in)\n{", "__int64  ps_tell(intptr_t in)\n{"),
    ("\t\t\t\treturn (int)ps;", "\t\t\t\treturn (intptr_t)ps;"),
])

# --- MULTI_FILE のファイルディスクリプタ --------------------------------
patch("multi_file.c", [
    ("#include <stdio.h>", "#include <stdio.h>\n#include <stdint.h>"),
    ("\t__int64 offset;\n\tint   fd;", "\t__int64 offset;\n\tintptr_t fd;"),
    ("\tMULTI_FILE *r;\n\tMF_PRIVATE_DATA *prv;\n\tint fd;",
     "\tMULTI_FILE *r;\n\tMF_PRIVATE_DATA *prv;\n\tintptr_t fd;"),
    ("static int try_next(char *path);", "static intptr_t try_next(char *path);"),
    #  宣言 (上) を先に当てると、定義側の new が宣言の中に substring として
    #  現れてしまい「適用済み」と誤判定される。'{' まで含めて区別する
    ("static int try_next(char *path)\n{", "static intptr_t try_next(char *path)\n{"),
])

# --- VIDEO_STREAM -------------------------------------------------------
patch("video_stream.h", [
    ("#include <string.h>", "#include <string.h>\n#include <stdint.h>"),
    ("\tint            fd;", "\tintptr_t       fd;"),
    ("\tint           (* close)(int);", "\tint           (* close)(intptr_t);"),
    ("\tint           (* read)(int, void *, unsigned int);", "\tint           (* read)(intptr_t, void *, unsigned int);"),
    ("\t__int64       (* seek)(int, __int64, int);", "\t__int64       (* seek)(intptr_t, __int64, int);"),
    ("\t__int64       (* tell)(int);", "\t__int64       (* tell)(intptr_t);"),
])

patch("video_stream.c", [
    ("typedef int (* RAW_CLOSE)(int);", "typedef int (* RAW_CLOSE)(intptr_t);"),
    ("typedef int (* RAW_READ)(int, void *, unsigned int);", "typedef int (* RAW_READ)(intptr_t, void *, unsigned int);"),
    ("typedef __int64 (* RAW_SEEK)(int, __int64, int);", "typedef __int64 (* RAW_SEEK)(intptr_t, __int64, int);"),
    ("typedef __int64 (* RAW_TELL)(int);", "typedef __int64 (* RAW_TELL)(intptr_t);"),
    ("\t\t\tout->fd = (int)mf;", "\t\t\tout->fd = (intptr_t)mf;"),
])

# --- AUDIO_STREAM -------------------------------------------------------
patch("audio_stream.h", [
    ("#define AUDIO_STREAM_H", "#define AUDIO_STREAM_H\n\n#include <stdint.h>"),
    ("\tint           stream;", "\tintptr_t      stream;"),
    ("\t__int64      (* tell)(int stream);", "\t__int64      (* tell)(intptr_t stream);"),
    ("\t__int64      (* seek)(int stream, __int64 sample);", "\t__int64      (* seek)(intptr_t stream, __int64 sample);"),
    ("\tint          (* read)(int stream, void *buffer, int size);", "\tint          (* read)(intptr_t stream, void *buffer, int size);"),
    ("\tunsigned int (* next_sync)(int stream);", "\tunsigned int (* next_sync)(intptr_t stream);"),
    ("\tvoid         (* get_info)(int stream, AUDIO_INFO *info);", "\tvoid         (* get_info)(intptr_t stream, AUDIO_INFO *info);"),
])

patch("audio_stream.c", [
    ("#include <io.h>", "#include <io.h>\n#include <stdint.h>"),
    ("static __int64 tell_ps(int stream);", "static __int64 tell_ps(intptr_t stream);"),
    ("static __int64 seek_ps(int stream, __int64 sample);", "static __int64 seek_ps(intptr_t stream, __int64 sample);"),
    ("static int read_ps(int stream, void *buffer, int size);", "static int read_ps(intptr_t stream, void *buffer, int size);"),
    ("static unsigned int next_sync_ps(int stream);", "static unsigned int next_sync_ps(intptr_t stream);"),
    ("static void get_info_ps(int stream, AUDIO_INFO *info);", "static void get_info_ps(intptr_t stream, AUDIO_INFO *info);"),
    ("static __int64 tell_ps(int stream)\n{", "static __int64 tell_ps(intptr_t stream)\n{"),
    ("static __int64 seek_ps(int stream, __int64 sample)\n{", "static __int64 seek_ps(intptr_t stream, __int64 sample)\n{"),
    ("static int read_ps(int stream, void *buffer, int size)\n{", "static int read_ps(intptr_t stream, void *buffer, int size)\n{"),
    ("static unsigned int next_sync_ps(int stream)\n{", "static unsigned int next_sync_ps(intptr_t stream)\n{"),
    ("static void get_info_ps(int stream, AUDIO_INFO *info)\n{", "static void get_info_ps(intptr_t stream, AUDIO_INFO *info)\n{"),
    ("\tint fd;\n\t\n\tAUDIO_STREAM *r;", "\tintptr_t fd;\n\t\n\tAUDIO_STREAM *r;"),
    ("\t\tr->stream = (int)open_ps(path);", "\t\tr->stream = (intptr_t)open_ps(path);"),
    ("\tseek_ps((int)r, 0);", "\tseek_ps((intptr_t)r, 0);"),
])

# --- audio_stream.c: 巨大なスタック上のバッファをヒープに移す -----------
#     audio_stream_open() は 512*1204 = 約 600KB、setup_format_ps() は
#     256KB をスタックに取る。AviUtl2 が func_open() を呼ぶスレッドの
#     スタックは 1MB しかない事があり、そのままだとスタックオーバーフロー
#     (0xC00000FD) でプロセスごと落ちる。
patch("audio_stream.c", [
    ("\tint n;\n\tunsigned char buffer[512*1204];\n"
     "\n"
     "\tfd = _open(path, _O_BINARY|_O_RDONLY|_O_SEQUENTIAL);\n"
     "\tif(fd < 0){\n"
     "\t\treturn NULL;\n"
     "\t}\n"
     "\tn = _read(fd, buffer, sizeof(buffer));\n"
     "\t_close(fd);\n"
     "\t\n"
     "\tr = (AUDIO_STREAM *)calloc(1, sizeof(AUDIO_STREAM));\n"
     "\tif(r == NULL){\n"
     "\t\treturn NULL;\n"
     "\t}\n"
     "\t\n"
     "\tif(check_ps(buffer, n)){",
     "\tint n;\n"
     "\tconst size_t buffer_size = 512*1204;\n"
     "\tunsigned char *buffer = (unsigned char *)malloc(buffer_size);\n"
     "\tint is_ps;\n"
     "\n"
     "\tif(buffer == NULL){\n"
     "\t\treturn NULL;\n"
     "\t}\n"
     "\tfd = _open(path, _O_BINARY|_O_RDONLY|_O_SEQUENTIAL);\n"
     "\tif(fd < 0){\n"
     "\t\tfree(buffer);\n"
     "\t\treturn NULL;\n"
     "\t}\n"
     "\tn = _read(fd, buffer, (unsigned int)buffer_size);\n"
     "\t_close(fd);\n"
     "\t\n"
     "\tis_ps = check_ps(buffer, n);\n"
     "\tfree(buffer);\n"
     "\t\n"
     "\tr = (AUDIO_STREAM *)calloc(1, sizeof(AUDIO_STREAM));\n"
     "\tif(r == NULL){\n"
     "\t\treturn NULL;\n"
     "\t}\n"
     "\t\n"
     "\tif(is_ps){"),

    ("\tLAYER2_HEADER hd;\n"
     "\t\n"
     "\tunsigned char buffer[256*1024];\n"
     "\tunsigned char *p,*last;",
     "\tLAYER2_HEADER hd;\n"
     "\t\n"
     "\tconst size_t buffer_size = 256*1024;\n"
     "\tunsigned char *buffer = (unsigned char *)malloc(buffer_size);\n"
     "\tunsigned char *p,*last;"),

    ("\tif(ps->stream_id == 0){\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\n"
     "\textract_pes_packet_data(&packet, buffer, (unsigned int *)&pos);",
     "\tif( (ps->stream_id == 0) || (buffer == NULL) ){\n"
     "\t\tfree(buffer);\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\n"
     "\textract_pes_packet_data(&packet, buffer, (unsigned int *)&pos);"),

    ("\tif(n == 0){\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\t\n"
     "\tps->frequency = hd.frequency;\n"
     "\tps->channel = hd.channel;\n"
     "\n"
     "\treturn 1;\n"
     "}",
     "\tfree(buffer);\n"
     "\n"
     "\tif(n == 0){\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\t\n"
     "\tps->frequency = hd.frequency;\n"
     "\tps->channel = hd.channel;\n"
     "\n"
     "\treturn 1;\n"
     "}"),
])

# --- out_buffer.c: LLP64 では long が 32bit なのでポインタを入れられない -
#     使うのは下位 4bit だけなので実害は無かったが uintptr_t に直す
patch("out_buffer.c", [
    ("#include <stdlib.h>", "#include <stdlib.h>\n#include <stdint.h>"),
    ("\tr->data.y = (ptr + 16) - (((long)ptr) & 0x0f);",
     "\tr->data.y = (ptr + 16) - (((uintptr_t)ptr) & 0x0f);"),
])

# --- resize.c の高速化 --------------------------------------------------
#     component_resize() がデコード時間の 7 割以上を占める (実測)。
#     元の実装は 1 出力画素・1 タップ毎に index[x][i] / weight[x][i] という
#     int** の二段間接参照を行っており、これが支配的だった。
#
#     ・重みと添字を連続した配列に平坦化する
#     ・タップが連続している画素 (端以外は必ず連続) は添字を介さず
#       入力を直接連続読みする
#     ・Lanczos3 拡大の 6 タップは展開する
#
#     加算の順序も精度も元のままなので、出力はビット単位で一致する。
#
#     ※ 挿入する C のコメントは ASCII のみ (このファイルは latin-1 で
#        書き戻す為、日本語を入れるとエンコード出来ない)
patch("resize.h", [
    ("\tint **index;\n\tint **weight;\n",
     "\tint **index;\n\tint **weight;\n"
     "\n"
     "\t/* added by tools/patch64.py : flattened tables for speed */\n"
     "\tint  *flat_weight;  /* length*tap entries, contiguous */\n"
     "\tint  *flat_index;   /* ditto */\n"
     "\tint  *start;        /* first tap index, or -1 if not contiguous */\n"),
])

patch("resize.c", [
    # 追加したフィールドを NULL にしておく (malloc なのでゼロ初期化されない)
    ("\tr = (RESIZE_PARAMETER *)malloc(sizeof(RESIZE_PARAMETER));\n"
     "\tif(r == NULL){\n"
     "\t\treturn NULL;\n"
     "\t}",
     "\tr = (RESIZE_PARAMETER *)malloc(sizeof(RESIZE_PARAMETER));\n"
     "\tif(r == NULL){\n"
     "\t\treturn NULL;\n"
     "\t}\n"
     "\tmemset(r, 0, sizeof(RESIZE_PARAMETER));"),

    # 平坦化テーブルの構築と、それを使う高速経路
    ("static void component_resize(unsigned char *in, unsigned char *out, COMPONENT_RESIZE_PARAMETER *prm)\n"
     "{\n"
     "\tint x,y;\n"
     "\tint i;\n"
     "\tint w;\n"
     "\n"
     "\tin += prm->in_offset;\n"
     "\tout += prm->out_offset;\n",

     "/* Flatten index/weight so the inner loop reads them sequentially.\n"
     "   The original code did two pointer indirections per tap, which\n"
     "   dominated the decoding time (measured: over 70%). */\n"
     "static void build_fast_resize_table(COMPONENT_RESIZE_PARAMETER *prm)\n"
     "{\n"
     "\tint x, i;\n"
     "\tconst int tap = prm->tap;\n"
     "\tconst int len = prm->length;\n"
     "\n"
     "\tprm->flat_weight = (int *)malloc(sizeof(int) * len * tap);\n"
     "\tprm->flat_index  = (int *)malloc(sizeof(int) * len * tap);\n"
     "\tprm->start       = (int *)malloc(sizeof(int) * len);\n"
     "\n"
     "\tif(prm->flat_weight == NULL || prm->flat_index == NULL || prm->start == NULL){\n"
     "\t\tfree(prm->flat_weight); prm->flat_weight = NULL;\n"
     "\t\tfree(prm->flat_index);  prm->flat_index  = NULL;\n"
     "\t\tfree(prm->start);       prm->start       = NULL;\n"
     "\t\treturn;\n"
     "\t}\n"
     "\n"
     "\tfor(x=0;x<len;x++){\n"
     "\t\tint contiguous = 1;\n"
     "\t\tfor(i=0;i<tap;i++){\n"
     "\t\t\tprm->flat_weight[x*tap+i] = prm->weight[x][i];\n"
     "\t\t\tprm->flat_index [x*tap+i] = prm->index[x][i];\n"
     "\t\t\tif(prm->index[x][i] != prm->index[x][0] + i){\n"
     "\t\t\t\tcontiguous = 0;\n"
     "\t\t\t}\n"
     "\t\t}\n"
     "\t\tprm->start[x] = contiguous ? prm->index[x][0] : -1;\n"
     "\t}\n"
     "}\n"
     "\n"
     "static void component_resize(unsigned char *in, unsigned char *out, COMPONENT_RESIZE_PARAMETER *prm)\n"
     "{\n"
     "\tint x,y;\n"
     "\tint i;\n"
     "\tint w;\n"
     "\tconst int tap = prm->tap;\n"
     "\tconst int width = prm->width;\n"
     "\tconst int *fw;\n"
     "\tconst int *fi;\n"
     "\n"
     "\tin += prm->in_offset;\n"
     "\tout += prm->out_offset;\n"
     "\n"
     "\tif(prm->flat_weight == NULL && width <= prm->length){\n"
     "\t\tbuild_fast_resize_table(prm);\n"
     "\t}\n"
     "\n"
     "\t/* fast path : same arithmetic, sequential tables, direct reads */\n"
     "\tif(prm->flat_weight != NULL && width <= prm->length){\n"
     "\t\tfor(y=0;y<prm->height;y++){\n"
     "\t\t\tfw = prm->flat_weight;\n"
     "\t\t\tfi = prm->flat_index;\n"
     "\t\t\tif(tap == 6){\n"
     "\t\t\t\t/* Lanczos3 upscale is always 6 taps */\n"
     "\t\t\t\tfor(x=0;x<width;x++){\n"
     "\t\t\t\t\tconst int s = prm->start[x];\n"
     "\t\t\t\t\tif(s >= 0){\n"
     "\t\t\t\t\t\tconst unsigned char *p = in + s;\n"
     "\t\t\t\t\t\tw = p[0]*fw[0] + p[1]*fw[1] + p[2]*fw[2]\n"
     "\t\t\t\t\t\t  + p[3]*fw[3] + p[4]*fw[4] + p[5]*fw[5];\n"
     "\t\t\t\t\t}else{\n"
     "\t\t\t\t\t\tw = in[fi[0]]*fw[0] + in[fi[1]]*fw[1] + in[fi[2]]*fw[2]\n"
     "\t\t\t\t\t\t  + in[fi[3]]*fw[3] + in[fi[4]]*fw[4] + in[fi[5]]*fw[5];\n"
     "\t\t\t\t\t}\n"
     "\t\t\t\t\tw += 32768;\n"
     "\t\t\t\t\tout[x] = uchar_clip_table[UCHAR_CLIP_TABLE_OFFSET+(w>>16)];\n"
     "\t\t\t\t\tfw += 6;\n"
     "\t\t\t\t\tfi += 6;\n"
     "\t\t\t\t}\n"
     "\t\t\t}else{\n"
     "\t\t\t\tfor(x=0;x<width;x++){\n"
     "\t\t\t\t\tconst int s = prm->start[x];\n"
     "\t\t\t\t\tw = 0;\n"
     "\t\t\t\t\tif(s >= 0){\n"
     "\t\t\t\t\t\tconst unsigned char *p = in + s;\n"
     "\t\t\t\t\t\tfor(i=0;i<tap;i++){\n"
     "\t\t\t\t\t\t\tw += p[i] * fw[i];\n"
     "\t\t\t\t\t\t}\n"
     "\t\t\t\t\t}else{\n"
     "\t\t\t\t\t\tfor(i=0;i<tap;i++){\n"
     "\t\t\t\t\t\t\tw += in[fi[i]] * fw[i];\n"
     "\t\t\t\t\t\t}\n"
     "\t\t\t\t\t}\n"
     "\t\t\t\t\tw += 32768;\n"
     "\t\t\t\t\tout[x] = uchar_clip_table[UCHAR_CLIP_TABLE_OFFSET+(w>>16)];\n"
     "\t\t\t\t\tfw += tap;\n"
     "\t\t\t\t\tfi += tap;\n"
     "\t\t\t\t}\n"
     "\t\t\t}\n"
     "\t\t\tout += prm->out_step;\n"
     "\t\t\tin += prm->in_step;\n"
     "\t\t}\n"
     "\t\treturn;\n"
     "\t}\n",
     #  この後の「SSE2 化」パッチが同じ範囲を書き換えるので、new では
     #  適用済みを判定出来ない。ここで入れた定義の 1 行を目印にする
     "static void build_fast_resize_table("),

    # 後始末
    ("\tfree(prm->l.index);\n\tfree(prm->l.weight);",
     "\tfree(prm->l.index);\n\tfree(prm->l.weight);\n"
     "\tfree(prm->l.flat_weight);\n\tfree(prm->l.flat_index);\n\tfree(prm->l.start);"),
    ("\tfree(prm->c.index);\n\tfree(prm->c.weight);",
     "\tfree(prm->c.index);\n\tfree(prm->c.weight);\n"
     "\tfree(prm->c.flat_weight);\n\tfree(prm->c.flat_index);\n\tfree(prm->c.start);"),
])

# --- resize.c の SSE2 化 ------------------------------------------------
#     平坦化だけでも 1.7 倍になったが、まだ全体の 6 割弱を占める。
#     内側は「6 タップの積和」なので SSE2 で 8 レーン同時に処理する。
#
#     重みは 1<<16 スケールの int32 で int16 に入らない為、
#         w = (w >> 8) * 256 + (w & 255)
#     と上位・下位に分ける。どちらも int16 に収まり、_mm_madd_epi16 を
#     2 回使って合成すれば **元と完全に同じ整数値**が得られる。
#
#     x64 では SSE2 は常に使えるので CPU 判定は不要。
#
#     ※ 挿入する C のコメントは ASCII のみ
patch("resize.c", [
    ('#include "resize.h"',
     '#include <emmintrin.h>\n#include "resize.h"'),

    # 16bit 分割した重みも用意する
    ("\tprm->start       = (int *)malloc(sizeof(int) * len);\n"
     "\n"
     "\tif(prm->flat_weight == NULL || prm->flat_index == NULL || prm->start == NULL){",
     "\tprm->start       = (int *)malloc(sizeof(int) * len);\n"
     "\t/* 8 lanes per pixel, zero padded, for _mm_madd_epi16 */\n"
     "\tprm->w16hi       = (short *)malloc(sizeof(short) * len * 8);\n"
     "\tprm->w16lo       = (short *)malloc(sizeof(short) * len * 8);\n"
     "\n"
     "\tif(prm->w16hi != NULL && prm->w16lo != NULL){\n"
     "\t\tmemset(prm->w16hi, 0, sizeof(short) * len * 8);\n"
     "\t\tmemset(prm->w16lo, 0, sizeof(short) * len * 8);\n"
     "\t}\n"
     "\n"
     "\tif(prm->flat_weight == NULL || prm->flat_index == NULL || prm->start == NULL){"),

    ("\t\tfree(prm->start);       prm->start       = NULL;\n"
     "\t\treturn;\n"
     "\t}",
     "\t\tfree(prm->start);       prm->start       = NULL;\n"
     "\t\tfree(prm->w16hi);       prm->w16hi       = NULL;\n"
     "\t\tfree(prm->w16lo);       prm->w16lo       = NULL;\n"
     "\t\treturn;\n"
     "\t}"),

    ("\t\tprm->start[x] = contiguous ? prm->index[x][0] : -1;\n"
     "\t}\n"
     "}",
     "\t\tprm->start[x] = contiguous ? prm->index[x][0] : -1;\n"
     "\n"
     "\t\tif(prm->w16hi != NULL && prm->w16lo != NULL && tap <= 8){\n"
     "\t\t\tfor(i=0;i<tap;i++){\n"
     "\t\t\t\tconst int v = prm->weight[x][i];\n"
     "\t\t\t\tprm->w16hi[x*8+i] = (short)(v >> 8);\n"
     "\t\t\t\tprm->w16lo[x*8+i] = (short)(v & 255);\n"
     "\t\t\t}\n"
     "\t\t}\n"
     "\t}\n"
     "}\n"
     "\n"
     "/* 6 tap dot product with SSE2.\n"
     "   weight = (w>>8)*256 + (w&255) ; both halves fit in int16 so the\n"
     "   result is bit exact with the scalar version. */\n"
     "static __inline int resize_dot_sse2(const unsigned char *p,\n"
     "                                    const short *whi, const short *wlo)\n"
     "{\n"
     "\tconst __m128i zero = _mm_setzero_si128();\n"
     "\t__m128i pix = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)p), zero);\n"
     "\t__m128i hi  = _mm_madd_epi16(pix, _mm_loadu_si128((const __m128i *)whi));\n"
     "\t__m128i lo  = _mm_madd_epi16(pix, _mm_loadu_si128((const __m128i *)wlo));\n"
     "\t__m128i acc = _mm_add_epi32(_mm_slli_epi32(hi, 8), lo);\n"
     "\tacc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(1,0,3,2)));\n"
     "\tacc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(2,3,0,1)));\n"
     "\treturn _mm_cvtsi128_si32(acc);\n"
     "}",
     #  この後の「索引を行の中へ丸める」当て込みが同じ範囲を書き換える
     "\t\t\t\tprm->w16hi[x*8+i] = (short)(v >> 8);"),

    # 6 タップの経路を SSE2 に置き換える
    ("\t\t\tif(tap == 6){\n"
     "\t\t\t\t/* Lanczos3 upscale is always 6 taps */\n"
     "\t\t\t\tfor(x=0;x<width;x++){\n"
     "\t\t\t\t\tconst int s = prm->start[x];\n"
     "\t\t\t\t\tif(s >= 0){\n"
     "\t\t\t\t\t\tconst unsigned char *p = in + s;\n"
     "\t\t\t\t\t\tw = p[0]*fw[0] + p[1]*fw[1] + p[2]*fw[2]\n"
     "\t\t\t\t\t\t  + p[3]*fw[3] + p[4]*fw[4] + p[5]*fw[5];\n"
     "\t\t\t\t\t}else{",
     "\t\t\tif(tap == 6){\n"
     "\t\t\t\t/* Lanczos3 upscale is always 6 taps */\n"
     "\t\t\t\tfor(x=0;x<width;x++){\n"
     "\t\t\t\t\tconst int s = prm->start[x];\n"
     "\t\t\t\t\tif(s >= 0 && use_sse2 && s + 8 <= prm->in_step){\n"
     "\t\t\t\t\t\tw = resize_dot_sse2(in + s,\n"
     "\t\t\t\t\t\t                    prm->w16hi + x*8, prm->w16lo + x*8);\n"
     "\t\t\t\t\t}else if(s >= 0){\n"
     "\t\t\t\t\t\tconst unsigned char *p = in + s;\n"
     "\t\t\t\t\t\tw = p[0]*fw[0] + p[1]*fw[1] + p[2]*fw[2]\n"
     "\t\t\t\t\t\t  + p[3]*fw[3] + p[4]*fw[4] + p[5]*fw[5];\n"
     "\t\t\t\t\t}else{"),

    # use_sse2 の判定を関数の先頭に置く
    ("\tif(prm->flat_weight == NULL && width <= prm->length){\n"
     "\t\tbuild_fast_resize_table(prm);\n"
     "\t}",
     "\tif(prm->flat_weight == NULL && width <= prm->length){\n"
     "\t\tbuild_fast_resize_table(prm);\n"
     "\t}\n"
     "\n"
     "\t{\n"
     "\tconst int use_sse2 = (prm->w16hi != NULL && prm->w16lo != NULL);\n"),

    ("\t\t\tout += prm->out_step;\n"
     "\t\t\tin += prm->in_step;\n"
     "\t\t}\n"
     "\t\treturn;\n"
     "\t}",
     "\t\t\tout += prm->out_step;\n"
     "\t\t\tin += prm->in_step;\n"
     "\t\t}\n"
     "\t\treturn;\n"
     "\t}\n"
     "\t}"),

    # 後始末
    ("\tfree(prm->l.flat_weight);\n\tfree(prm->l.flat_index);\n\tfree(prm->l.start);",
     "\tfree(prm->l.flat_weight);\n\tfree(prm->l.flat_index);\n\tfree(prm->l.start);\n"
     "\tfree(prm->l.w16hi);\n\tfree(prm->l.w16lo);"),
    ("\tfree(prm->c.flat_weight);\n\tfree(prm->c.flat_index);\n\tfree(prm->c.start);",
     "\tfree(prm->c.flat_weight);\n\tfree(prm->c.flat_index);\n\tfree(prm->c.start);\n"
     "\tfree(prm->c.w16hi);\n\tfree(prm->c.w16lo);"),

    #  索引を行の中へ丸める。
    #
    #  壊れた入力では prm->index[x][i] が行の外を指す事がある。
    #  元の m2v も in[prm->index[x][i]] と読むので平坦化で持ち込んだ物
    #  ではないが、実際にアクセス違反になる
    #  (tests/tools/fuzz-locate.sh 1004 3 で再現した)。
    #
    #  正常な入力では表は同じ幅から作られるのでこの条件には掛からない。
    #  出力が変わらない事は test_decode の 1 フレーム目の SHA-256 で確認。
    #
    #  ※ 上の平坦化 / SSE2 の当て込みより後に置く事。
    #     先に当てると、そちらの old が一致しなくなる
    ("\t\tfor(i=0;i<tap;i++){\n"
     "\t\t\tprm->flat_weight[x*tap+i] = prm->weight[x][i];\n"
     "\t\t\tprm->flat_index [x*tap+i] = prm->index[x][i];\n"
     "\t\t\tif(prm->index[x][i] != prm->index[x][0] + i){\n"
     "\t\t\t\tcontiguous = 0;\n"
     "\t\t\t}\n"
     "\t\t}\n"
     "\t\tprm->start[x] = contiguous ? prm->index[x][0] : -1;\n",

     "\t\tfor(i=0;i<tap;i++){\n"
     "\t\t\tint idx = prm->index[x][i];\n"
     "\t\t\tif(idx < 0){\n"
     "\t\t\t\tidx = 0;\n"
     "\t\t\t}else if(limit > 0 && idx >= limit){\n"
     "\t\t\t\tidx = limit - 1;\n"
     "\t\t\t}\n"
     "\t\t\tprm->flat_weight[x*tap+i] = prm->weight[x][i];\n"
     "\t\t\tprm->flat_index [x*tap+i] = idx;\n"
     "\t\t\tif(idx != prm->flat_index[x*tap] + i){\n"
     "\t\t\t\tcontiguous = 0;\n"
     "\t\t\t}\n"
     "\t\t}\n"
     "\t\tprm->start[x] = contiguous ? prm->flat_index[x*tap] : -1;\n"
     "\t\tif(prm->start[x] >= 0 && limit > 0 && prm->start[x] + tap > limit){\n"
     "\t\t\tprm->start[x] = -1;\n"
     "\t\t}\n"),

    ("\tconst int tap = prm->tap;\n"
     "\tconst int len = prm->length;\n",

     "\tconst int tap = prm->tap;\n"
     "\tconst int len = prm->length;\n"
     "\t/* reads have to stay inside one input row */\n"
     "\tconst int limit = prm->in_step;\n"),

    #  prm を作った時の入力の大きさを憶えておき、渡されたフレームと
    #  食い違ったらリサイズを行わない。
    #
    #  prm はファイルを開いた時のシーケンスヘッダから作る。壊れた入力では
    #  後から別の大きさが現れ、prm (行数・行の幅・索引・クロマの間引き) が
    #  実フレームと噛み合わなくなる。読み出し側を 1 つずつ塞いでも
    #  輝度・クロマそれぞれで別の場所が破れる為、根元で止める
    #  (tests/tools/fuzz-locate.sh 1004 3 で追った)。
    #
    #  正常な入力では大きさは一致するので、この条件には掛からない。
    ("\t\tsetup_interpolation_parameter(src_width[1], r->c.width, &(r->c));\n"
     "\t}\n"
     "\n"
     "\treturn r;\n",

     "\t\tsetup_interpolation_parameter(src_width[1], r->c.width, &(r->c));\n"
     "\t}\n"
     "\n"
     "\t/* remember what this parameter was built for (see resize()) */\n"
     "\tr->src_width = seq->h_size;\n"
     "\tr->src_height = seq->v_size;\n"
     "\n"
     "\treturn r;\n"),

    ("void resize(FRAME *in, FRAME *out, RESIZE_PARAMETER *prm)\n"
     "{\n"
     "\tcomponent_resize(in->y, out->y, &(prm->l));\n"
     "\tcomponent_resize(in->u, out->u, &(prm->c));\n"
     "\tcomponent_resize(in->v, out->v, &(prm->c));\n"
     "}",

     "void resize(FRAME *in, FRAME *out, RESIZE_PARAMETER *prm)\n"
     "{\n"
     "\t/* A damaged stream can present a different size after the parameter\n"
     "\t   was built. Then the row count, the row stride, the index table and\n"
     "\t   the chroma subsampling all disagree with the frame, and the reads\n"
     "\t   run off the plane. Nothing sane can be produced, so skip it. */\n"
     "\tif(prm->src_width != 0 && prm->src_height != 0\n"
     "\t   && (in->width != prm->src_width || in->height != prm->src_height)){\n"
     "\t\treturn;\n"
     "\t}\n"
     "\n"
     "\tcomponent_resize(in->y, out->y, &(prm->l));\n"
     "\tcomponent_resize(in->u, out->u, &(prm->c));\n"
     "\tcomponent_resize(in->v, out->v, &(prm->c));\n"
     "}"),
])

patch("resize.h", [
    ("typedef struct {\n"
     "\tCOMPONENT_RESIZE_PARAMETER l; /* luminance   */\n"
     "\tCOMPONENT_RESIZE_PARAMETER c; /* chrominance */\n"
     "} RESIZE_PARAMETER;",

     "typedef struct {\n"
     "\tCOMPONENT_RESIZE_PARAMETER l; /* luminance   */\n"
     "\tCOMPONENT_RESIZE_PARAMETER c; /* chrominance */\n"
     "\tint src_width;                /* what the parameter was built for */\n"
     "\tint src_height;\n"
     "} RESIZE_PARAMETER;"),
])

patch("resize.h", [
    ("\tint  *start;        /* first tap index, or -1 if not contiguous */\n",
     "\tint  *start;        /* first tap index, or -1 if not contiguous */\n"
     "\tshort *w16hi;       /* weight >> 8   , 8 lanes per pixel */\n"
     "\tshort *w16lo;       /* weight & 255  , 8 lanes per pixel */\n"),
])

# --- resize.c のポインタ配列確保サイズ (32bit 前提のバグ) ---------------
patch("resize.c", [
    #  正しい形 (sizeof(int *)) は元のソースの別の箇所に既に在る為、
    #  new を見る既定の判定では初回から「適用済み」と誤判定され、
    #  この 1 箇所だけが直らないまま黙って残る。old で判断する。
    #
    #  直っていないと int ** の配列を sizeof(int) 個分しか確保せず、
    #  x64 では必要量の半分になる (ヒープの破壊)。
    ("\tout->index = (int **)malloc(sizeof(int)*out->length);",
     "\tout->index = (int **)malloc(sizeof(int *)*out->length);",
     BY_OLD),
])

# plugin.cpp (AviUtl 1.xx 用の入力プラグイン glue) は使わない。
# AviUtl ExEdit2 用の実装は src/aviutl2/input_tvtv.cpp を参照。

# --- gl_dialog.c: 進捗ダイアログのリソース取得元 DLL 名 ------------------
patch("gl_dialog.c", [
    ('\t\t"m2v.vfp",\n\t\t"m2v.aui",',
     '\t\t"TSMemory-TVTestSrc.aux2",\n\t\t"TVTestSrc.aui2",\n\t\t"m2v.vfp",\n\t\t"m2v.aui",'),
])

# --- gl_dialog.c: GWL_USERDATA は x64 に無い ----------------------------
patch("gl_dialog.c", [
    ("\t\tSetWindowLong(hwnd, GWL_USERDATA, lparam); ",
     "\t\tSetWindowLongPtr(hwnd, GWLP_USERDATA, lparam);"),
    ("\t\t\tdata = (GL_DIALOG_PRIVATE_DATA *)GetWindowLong(hwnd, GWL_USERDATA);",
     "\t\t\tdata = (GL_DIALOG_PRIVATE_DATA *)GetWindowLongPtr(hwnd, GWLP_USERDATA);"),
    # DLGPROC の戻り値は x64 では INT_PTR
    ("static BOOL CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);",
     "static INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);"),
    ("static BOOL CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)\n{",
     "static INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)\n{"),
])

# --- instance_manager.c: DllMain での後始末をやめる ---------------------
#
#     元は AviSynth の LoadVFAPIPlugin 対策 (close されずに FreeLibrary
#     される) で、DLL_PROCESS_DETACH のタイミングで close_mpeg_video を
#     呼び直していた。
#
#     AviUtl ExEdit2 ではこれが問題になる。パッケージの入れ替え等で
#     プラグインがアンロードされる時、AviUtl2 側の func_close() と
#     DllMain 側の後始末が別スレッドで競合して同じ MPEG_VIDEO を二重に
#     解放し、release_out_buffer() で落ちる
#     (TSMemory-TVTestSrc.aux2_unloaded として記録される)。
#
#     しかも DllMain はローダーロックを持ったまま呼ばれるので、その中で
#     デコードスレッドの終了待ちをするのは元々危険。
#
#     AviUtl ExEdit2 版では UninitializePlugin() で確実に閉じる様にした
#     (src/aviutl2/input_tvtv.cpp の TSMemoryInputUninitialize) ので、
#     ここではノードの解放だけを行う。
patch("instance_manager.c", [
    ("\twhile ((p = get_head(&(manager.work))) != NULL) {\n"
     "\t\tif ((p->body != NULL) && (p->teardown != NULL)) {\n"
     "\t\t\tp->teardown(p->body);\n"
     "\t\t}\n"
     "\t\tfree(p);\n"
     "\t}",
     "\t/* Do not run teardown procs from DllMain (loader lock).\n"
     "\t   On AviUtl ExEdit2 this races with func_close() during plugin\n"
     "\t   unload and double-frees the MPEG_VIDEO.\n"
     "\t   Handles are closed by TSMemoryInputUninitialize() instead. */\n"
     "\twhile ((p = get_head(&(manager.work))) != NULL) {\n"
     "\t\tfree(p);\n"
     "\t}"),
])

# --- pes.c: PES のデータ長が壊れた入力で破綻する ------------------------
#
#  3 つの *_stream_data_length() が
#    ・extract_standard_pes_header() の戻り値を見ていない
#      (失敗すると sph が未初期化のまま使われる)
#    ・p->size - sph.header_length を **unsigned で** 返す
#      (header_length > size だと巨大な値に化ける)
#  という形になっている。
#
#  呼び出し側の ts_read() は
#      ts->packet_rest = get_pes_packet_data_length(&ts->packet);
#      ts->packet_data = ref_pes_packet_data(&ts->packet);
#      memcpy(data, ts->packet_data, r);
#  と使う。ref_pes_packet_data() は (p->data + p->size - len) なので、
#  len が化けるとポインタが範囲外へ飛び、memcpy が明後日の番地を読む。
#  ビット反転させた TS で実際にアクセス違反になる
#  (tests/test_fuzz.cpp の seed 7 で再現した)。
#
#  private_1 は更に、境界を確かめる前に w[0] を読んでいる。
#
#  ※ 挿入する C のコメントは ASCII のみ
patch("pes.c", [
    ("static unsigned int video_stream_data_length(PES_PACKET *p)\n"
     "{\n"
     "\tSTANDARD_PES_HEADER sph;\n"
     "\n"
     "\textract_standard_pes_header(p, &sph);\n"
     "\n"
     "\treturn p->size - sph.header_length;\n"
     "}",

     "static unsigned int video_stream_data_length(PES_PACKET *p)\n"
     "{\n"
     "\tSTANDARD_PES_HEADER sph;\n"
     "\n"
     "\t/* A damaged stream may fail to parse, and even when it parses the\n"
     "\t   reported header length can exceed the packet. The result is\n"
     "\t   unsigned, so p->size - header_length wraps to a huge value and\n"
     "\t   ts_read() then memcpy()s from a pointer far out of range. */\n"
     "\tmemset(&sph, 0, sizeof(sph));\n"
     "\tif(!extract_standard_pes_header(p, &sph)){\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\tif(p->size <= 0 || sph.header_length < 0 || sph.header_length > p->size){\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\n"
     "\treturn p->size - sph.header_length;\n"
     "}"),

    ("static unsigned int audio_stream_data_length(PES_PACKET *p)\n"
     "{\n"
     "\tSTANDARD_PES_HEADER sph;\n"
     "\n"
     "\textract_standard_pes_header(p, &sph);\n"
     "\n"
     "\treturn p->size - sph.header_length;\n"
     "}",

     "static unsigned int audio_stream_data_length(PES_PACKET *p)\n"
     "{\n"
     "\tSTANDARD_PES_HEADER sph;\n"
     "\n"
     "\t/* see video_stream_data_length() */\n"
     "\tmemset(&sph, 0, sizeof(sph));\n"
     "\tif(!extract_standard_pes_header(p, &sph)){\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\tif(p->size <= 0 || sph.header_length < 0 || sph.header_length > p->size){\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\n"
     "\treturn p->size - sph.header_length;\n"
     "}"),

    ("\textract_standard_pes_header(p, &sph);\n"
     "\tw = p->data + sph.header_length;\n"
     "\n"
     "\tr = 0;\n"
     "\tif( (w[0] >= 0x80) && (w[0] <= 0x8f) ){\n"
     "\t\t/* AC3 stream */\n"
     "\t\tr = p->size - sph.header_length - 4;\n",

     "\t/* see video_stream_data_length(). w[0] is read below, so the\n"
     "\t   header length has to stay strictly inside the packet. */\n"
     "\tmemset(&sph, 0, sizeof(sph));\n"
     "\tif(!extract_standard_pes_header(p, &sph)){\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\tif(p->size <= 0 || sph.header_length < 0 || sph.header_length >= p->size){\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\tw = p->data + sph.header_length;\n"
     "\n"
     "\tr = 0;\n"
     "\tif( (w[0] >= 0x80) && (w[0] <= 0x8f) ){\n"
     "\t\t/* AC3 stream */\n"
     "\t\tif(p->size - sph.header_length < 4){\n"
     "\t\t\treturn 0;\n"
     "\t\t}\n"
     "\t\tr = p->size - sph.header_length - 4;\n"),
])

# --- transport_stream.c: memcpy の直前で (ポインタ, 長さ) を検算する ----
#
#  pes.c 側を直しても、壊れた入力では ts_read() の memcpy が
#  範囲外を読む事がある。原因になり得る所が複数ある。
#    ・packet_rest / packet_data は**呼び出しを跨いで保持される**が、
#      その間に read_pes_packet() が PES バッファを realloc / free する
#    ・PES ヘッダの長さ欄が壊れていると size と capacity がずれる
#
#  どれが化けたかに関わらず、使う直前に PES バッファの範囲へ収める。
#  ここは「壊れた分は無かった事にする」で構わない
#  (正常な入力では条件に掛からないので影響しない)。
#
#  ※ 挿入する C のコメントは ASCII のみ
patch("transport_stream.c", [
    ("int ts_read(intptr_t in, void *data, unsigned int count)\n"
     "{\n",

     "/* Keep (packet_data, packet_rest) inside the PES buffer.\n"
     "\n"
     "   Both are kept across calls while read_pes_packet() may realloc or\n"
     "   free that buffer, and on a damaged stream the length fields can be\n"
     "   inconsistent with the allocation. Either way the memcpy() below\n"
     "   would read from an unrelated address.\n"
     "\n"
     "   Dropping the leftover is fine: there is nothing sane to hand back\n"
     "   for a damaged packet. A healthy stream never trips this. */\n"
     "static int ts_clamp_packet(TRANSPORT_STREAM *ts)\n"
     "{\n"
     "\tunsigned char *head;\n"
     "\tunsigned char *tail;\n"
     "\n"
     "\tif(ts->packet_rest <= 0){\n"
     "\t\tts->packet_rest = 0;\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\n"
     "\thead = ts->packet.data;\n"
     "\tif(head == NULL || ts->packet.capacity <= 0){\n"
     "\t\tts->packet_rest = 0;\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\ttail = head + ts->packet.capacity;\n"
     "\n"
     "\tif(ts->packet_data < head || ts->packet_data >= tail){\n"
     "\t\tts->packet_rest = 0;\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\tif(ts->packet_rest > (int)(tail - ts->packet_data)){\n"
     "\t\tts->packet_rest = (int)(tail - ts->packet_data);\n"
     "\t}\n"
     "\n"
     "\treturn ts->packet_rest;\n"
     "}\n"
     "\n"
     "int ts_read(intptr_t in, void *data, unsigned int count)\n"
     "{\n",
     "static int ts_clamp_packet(TRANSPORT_STREAM *ts)"),

    ("\tif(ts->packet_rest){\n"
     "\t\tif(ts->packet_rest <= count){",

     "\tif(ts_clamp_packet(ts)){\n"
     "\t\tif((unsigned int)ts->packet_rest <= count){"),

    ("\t\t\tts->packet_rest = get_pes_packet_data_length(&ts->packet);\n"
     "\t\t\tts->packet_data = ref_pes_packet_data(&ts->packet);\n"
     "\t\t\tif(ts->packet_rest <= count){",

     "\t\t\tts->packet_rest = (int)get_pes_packet_data_length(&ts->packet);\n"
     "\t\t\tts->packet_data = ref_pes_packet_data(&ts->packet);\n"
     "\t\t\tif(!ts_clamp_packet(ts)){\n"
     "\t\t\t\tcontinue;\n"
     "\t\t\t}\n"
     "\t\t\tif((unsigned int)ts->packet_rest <= count){"),
])

# --- gop_list.c: malloc の戻り値を見ていない箇所 -------------------------
#
#  new_gop_list() の 1 段目には malloc の結果を確かめずに書き込む所が
#  2 箇所ある。壊れた入力ではエントリの生成が止まらなくなり、
#  実際に malloc が NULL を返してアクセス違反になる
#  (tests/test_fuzz.cpp の seed 42 / 555 で再現した)。
#
#  同じ関数の 2 段目 (line 266 付近) は既に確かめて後始末しているので、
#  それに揃える。
#
#  ※ 挿入する C のコメントは ASCII のみ
patch("gop_list.c", [
    ("\t\t\tsc = (SEQUENCE_ENTRY *)malloc(sizeof(SEQUENCE_ENTRY));\n"
     "\t\t\tsc->index = 0;\n"
     "\t\t\tsc->prev = NULL;\n",

     "\t\t\tsc = (SEQUENCE_ENTRY *)malloc(sizeof(SEQUENCE_ENTRY));\n"
     "\t\t\tif(sc == NULL){\n"
     "\t\t\t\t/* out of memory : a damaged stream can keep producing\n"
     "\t\t\t\t   entries until the allocation fails */\n"
     "\t\t\t\tdlg->delete(dlg);\n"
     "\t\t\t\treturn NULL;\n"
     "\t\t\t}\n"
     "\t\t\tsc->index = 0;\n"
     "\t\t\tsc->prev = NULL;\n"),

    ("\t\t\t\tc = (GOP_ENTRY *)malloc(sizeof(GOP_ENTRY));\n"
     "\t\t\t\tc->index = 0;\n"
     "\t\t\t\tc->sh_index = sc->index;\n"
     "\t\t\t\tc->start = 0;\n",

     "\t\t\t\tc = (GOP_ENTRY *)malloc(sizeof(GOP_ENTRY));\n"
     "\t\t\t\tif(c == NULL){\n"
     "\t\t\t\t\t/* see above */\n"
     "\t\t\t\t\tdlg->delete(dlg);\n"
     "\t\t\t\t\trelease_sequence_entries(sc);\n"
     "\t\t\t\t\treturn NULL;\n"
     "\t\t\t\t}\n"
     "\t\t\t\tc->index = 0;\n"
     "\t\t\t\tc->sh_index = sc->index;\n"
     "\t\t\t\tc->start = 0;\n"),

    #  sc (シーケンスヘッダ) を見つける前にピクチャが来ると
    #  sc->index が NULL 参照になる
    ("\t\t\tif(pic.picture_coding_type == 1){\n"
     "\t\t\t\tc = (GOP_ENTRY *)malloc(sizeof(GOP_ENTRY));\n",

     "\t\t\tif(pic.picture_coding_type == 1){\n"
     "\t\t\t\tif(sc == NULL){\n"
     "\t\t\t\t\t/* A picture before any sequence header. Only a damaged\n"
     "\t\t\t\t\t   stream gets here; sc->index below would read NULL. */\n"
     "\t\t\t\t\tdlg->delete(dlg);\n"
     "\t\t\t\t\treturn NULL;\n"
     "\t\t\t\t}\n"
     "\t\t\t\tc = (GOP_ENTRY *)malloc(sizeof(GOP_ENTRY));\n"),

    #  1 段目が I ピクチャを見つけずに終わる事もある。
    #  以降は c と sc の両方を無条件に辿るので、ここで止める
    ("\tif(closed_gop){\n"
     "\t\tbroken_link = 0;\n"
     "\t}else{\n"
     "\t\tbroken_link = 1;\n"
     "\t}\n",

     "\t/* The first step can end without an I picture, or without any\n"
     "\t   sequence header. Everything below dereferences both. */\n"
     "\tif(c == NULL || sc == NULL){\n"
     "\t\tdlg->delete(dlg);\n"
     "\t\trelease_sequence_entries(sc);\n"
     "\t\trelease_gop_entries(c);\n"
     "\t\treturn NULL;\n"
     "\t}\n"
     "\n"
     "\tif(closed_gop){\n"
     "\t\tbroken_link = 0;\n"
     "\t}else{\n"
     "\t\tbroken_link = 1;\n"
     "\t}\n"),
])

# --- registry.c ---------------------------------------------------------
#  ・SIMD は x64 ビルドでは常に無効
#  ・設定セクションを [settings] から [M2V] に変える。
#    元は m2v 単体の ini だったので [settings] で足りていたが、
#    こちらでは 1 つの ini に連携・キャプチャの設定も同居する為。
#    古い ini がそのまま動くように [settings] も読む。
#
#  ※ simd の置換を先に置く事。こちらの old に "settings" が入っている
#     ので、セクション名の置換を先に行うと一致しなくなる。
patch("registry.c", [
    ('\tvalue=GetPrivateProfileIntA("settings","simd",0,inifile);\n\n\treturn value;',
     '\t(void)inifile;\n\tvalue=0; /* x64 build: no MMX/SSE assembly available */\n\n\treturn value;'),

    ('static void get_ini_filename(char *filename)',
     '/* Read a decoder setting.\n'
     '\n'
     '   The ini is shared with the AviUtl2 side of TSMemory, so these live\n'
     '   in their own [M2V] section. [settings] is the name m2v used as a\n'
     '   standalone plug-in; it is still read so older ini files keep\n'
     '   working. Every key here is non-negative, so -1 works as "absent". */\n'
     'static int get_ini_int(const char *key, int def, const char *inifile)\n'
     '{\n'
     '\tint value = GetPrivateProfileIntA("M2V", key, -1, inifile);\n'
     '\n'
     '\tif (value < 0)\n'
     '\t\tvalue = GetPrivateProfileIntA("settings", key, def, inifile);\n'
     '\n'
     '\treturn value;\n'
     '}\n'
     '\n'
     'static void get_ini_filename(char *filename)'),

    #  適用済みの判定に上の get_ini_int() の定義が引っ掛からないよう、
    #  呼び出しの形 (=... とキーの引用符) まで含めて一致させる
    ('=GetPrivateProfileIntA("settings","', '=get_ini_int("'),
])

print("[patch64] done")
