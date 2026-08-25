# ライセンスと、その適用範囲

本リポジトリには**由来もライセンスも異なる成果物が同居**しています。
一律のライセンスではない為、どこに何が適用されるかをここに明記します。

**GPL が及ぶのは `TSMemory.tvtp` (TVTest プラグイン) だけです。**
TVTest 由来の BonTsEngine を組み込む為、これはバイナリ全体が
GPL-2.0-or-later の対象になります。
`TSMemory-TVTestSrc.aux2` (AviUtl2 プラグイン) には GPL のコードが
含まれません。2 つは共有メモリ経由で通信する別プロセスで、
コードの結合はありません。

GPL は「同じリポジトリにある」「同じフォルダにある」ことでは伝播しません。
1 つの実行物に結合されているかどうかで決まります。独立した著作物を同じ
配布物に同梱すること (mere aggregation) は GPLv2 第 2 条の対象外です。

---

## 1. ディレクトリごとの由来とライセンス

| ディレクトリ | 由来 | ライセンス |
| --- | --- | --- |
| `src/tvtp/BonTsEngine/` | TVTest 本体 | **GPL-2.0-or-later** |
| `src/tvtp/TSMemory.cpp` | オリジナルの TSMemory を改変 | 上と結合する為 **GPL-2.0-or-later** として扱う |
| `src/tvtp/TVTestPlugin.h` | TVTest Plugin SDK | 「ヘッダ及びサンプルは自由に利用・改変・再配布できます」(TVTestSDK.txt) |
| `sdk/aviutl2/` | AviUtl ExEdit2 Plugin SDK ([配布元](https://spring-fragrance.mints.ne.jp/aviutl/)。更新は `tools/update-aviutl2-sdk.sh`) | **MIT** — Copyright (c) 2025 Kenkun |
| `src/m2v/` | MPEG-2 VIDEO VFAPI Plug-In (茂木和洋氏) を 64bit 化 | 作者による許諾 (下記 3 章)。無保証を受け入れる以外の制限なし |
| `src/m2v/idct_reference.c` | MPEG Software Simulation Group | ファイル冒頭の表示のとおり (無償・無保証) |
| `src/aviutl2/` (音声の `audio/` を含む), `src/common/`, `tools/` | 本リポジトリで新規に作成 | 制約なし |
| `tests/test_selector.cpp`, `tests/test_multich.cpp` | BonTsEngine のヘッダを include | **GPL-2.0-or-later** |
| `tests/` のその他 (`tests/tools/` を含む) | 本リポジトリで新規に作成 | 制約なし |
| `res/`, `docs/`, `licenses/`, `.vscode/` | 本リポジトリで新規に作成 (`licenses/` に置くライセンス全文を除く) | 制約なし |

`tests/tools/` に第三者のコードは置いていません。MSVC の取得に使う
`portable-msvc.py` だけは第三者の物なので、リポジトリに入れず
`tests/tools/setup-msvc.sh` が実行時に取得します (2 章)。

ライセンス全文は `licenses/` にあります。

- `licenses/GPL-2.0.txt`
- `licenses/AviUtl2-Plugin-SDK-license.txt`

### 確認方法

`src/tvtp/BonTsEngine/` の外で BonTsEngine に依存しているのは 3 ファイルだけです。

```sh
grep -rl BonTsEngine src tests
#   src/tvtp/TSMemory.cpp        -> TSMemory.tvtp に入る
#   tests/test_multich.cpp       -> テストのみ
#   tests/test_selector.cpp      -> テストのみ
#   tests/tools/test.sh          -> ビルド手順のコメントに名前が出るだけ
```

`tools/build.sh` を見ると、`TSMemory-TVTestSrc.aux2` のリンク対象は
`build/m2v/*.o` `build/aviutl2/*.o` `build/aviutl2/audio/*.o` だけで、
BonTsEngine を含む `build/tvtp/*.o` は入っていません。

**ビルドした物を直接調べるのが確実です。**

```bash
export PATH="$PWD/compilers/llvm-mingw/bin:$PATH"
llvm-nm dist/TSMemory-TVTestSrc.aux2 | grep -cE "CTsSelector|CTsPacket|CMediaDecoder"
llvm-nm dist/TSMemory.tvtp           | grep -cE "CTsSelector|CTsPacket|CMediaDecoder"
```

実測 (2026-08-24):

| バイナリ | BonTsEngine 由来の記号 |
| --- | --- |
| `TSMemory-TVTestSrc.aux2` (AviUtl2 側) | **0** |
| `TSMemory.tvtp` (TVTest 側) | 63 |

---

## 2. 配布形態

**本プロジェクトはソースのみを公開し、バイナリの配布は行いません。**
利用者は `tools/build.sh` で自分でビルドします。

その為、

- **GPLv2 第 3 条 (バイナリ配布時のソース提供義務) は適用されません。**
  ソース配布 (第 1 条) として、著作権表示・無保証の告知を残し、
  ライセンスの写しを添付していれば足ります。本文書と `licenses/` が
  それにあたります。
- **m2v の再配布に関する懸念も生じません** (下記 3 章)。
  作者の記載は「ソースコードの利用について」であり、まさにその範囲での
  利用になります。

### ビルドで生成されるパッケージ

`tools/build.sh` は利用者の手元に下記を作ります。配布物ではありませんが、
手元の複製にも表示が残るようライセンスを含めています。

| パッケージ | 中身 | 含まれるライセンス |
| --- | --- | --- |
| `TSMemory-x64.zip` | `TSMemory.tvtp` + `TSMemory-TVTestSrc.aux2` | GPL-2.0 全文 + AviUtl2 SDK (MIT) + 本文書 |
| `TSMemory-TVTestSrc.au2pkg.zip` | `TSMemory-TVTestSrc.aux2` のみ | AviUtl2 SDK (MIT) のみ |

**au2pkg には GPL の全文を入れていません。** GPL コードを含まない為です。
この違い自体が、適用範囲を示しています。

### ビルド時に取得する物 (配布物ではない)

`compilers/` `third_party/` と、テスト用の `build/ts-examples/` の中身は、
ビルド・テスト時に各配布元から取得します。
**バージョン管理せず** (`.gitignore`)、**配布物にも含めません**。
本リポジトリはこれらを再配布しないので、各ライセンスの再配布条項は
こちらには掛かりません。

| 取得物 | 取得元 | ライセンス | 取得方法 |
| --- | --- | --- | --- |
| llvm-mingw (clang / lld / mingw-w64) | [mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | Apache License v2.0 with LLVM Exceptions ほか (`LICENSE.TXT`) | `tools/setup-toolchain.sh` |
| Python 3 (embeddable) | [python.org](https://www.python.org/) | PSF License (`compilers/python/LICENSE.txt`) | `tools/setup-python.sh` (`build.sh` が自動実行) |
| TVTest | [DBCTRADO/TVTest](https://github.com/DBCTRADO/TVTest) | **GPL-2.0-or-later** | `tools/setup-tvtest-src.sh` |
| LibISDB | [DBCTRADO/LibISDB](https://github.com/DBCTRADO/LibISDB) | **GPL-2.0-or-later** | 同上 (submodule) |
| fdk-aac | [mstorsjo/fdk-aac](https://github.com/mstorsjo/fdk-aac) | Fraunhofer FDK AAC Codec Library ライセンス (`NOTICE`) | 同上 (submodule) |
| TSMemory (原典) | [dtvgit/TSMemory](https://github.com/dtvgit/TSMemory) | ライセンス表記なし (下記の未確認事項を参照) | `tools/setup-tsmemory-src.sh` |
| TvtPlay / BonDriver_Pipe | [xtne6f/TvtPlay](https://github.com/xtne6f/TvtPlay) | 同梱の `TvtPlay_Readme.txt` を参照 | `tools/setup-tvtplay-src.sh` |
| AviUtl ExEdit2 本体 | [spring-fragrance.mints.ne.jp/aviutl/](https://spring-fragrance.mints.ne.jp/aviutl/) | 同梱の `aviutl2.txt` / `credits.txt` を参照 | `tools/setup-aviutl2.sh` |
| MSVC / Windows SDK | Microsoft の配信 CDN | Visual Studio Build Tools のライセンス条項 (下記) | `tests/tools/setup-msvc.sh` |
| `portable-msvc.py` (MSVC の取得に使う) | [mmozeiko 氏の gist](https://gist.github.com/mmozeiko/7f3162ec2988e81e56d5c4e22cde9977) | **表示なし** (ファイル中にライセンスの記載が無い) | 同上 (`compilers/` に取得) |
| ISDB-T のサンプルストリーム (テスト用・任意) | [erb.jp](https://www.erb.jp/2013/12/28/samplestream/) | 配布ページの記載に従う | `tests/tools/fetch-ts-samples.sh` (手動のみ。`build/ts-examples/` に取得) |

テストが既定で使う TS (`build/ts-examples/sample.ts` / `multi.ts`) は
`tests/tools/gen-ts-examples.cpp` が**その場で合成する**もので、
第三者の著作物を含みません。放送された TS は再配布出来ない為、
リポジトリには一切入れていません。

TVTest / LibISDB / fdk-aac / MSVC は、**動作確認用の 64bit TVTest を
自分でビルドする場合にだけ**必要です。TSMemory 自体のビルド
(`bash tools/build.sh`) には要りません。

> **`src/tvtp/BonTsEngine/` とは別物です。**
> あちらは TVTest 由来のコードを本リポジトリが commit している物で、
> `TSMemory.tvtp` に組み込まれます (1 章)。
> ここで取得する `third_party/TVTest` は TVTest 本体をビルドする為だけの
> もので、`TSMemory.tvtp` には一切入りません。

### MSVC の取得とライセンス同意

MSVC だけは取得にライセンス条項への同意が要る為、**自動実行しません**。
`tests/tools/setup-msvc.sh` を手で実行した時に、Microsoft のチャネル
マニフェストから取得した条項の URL を表示して同意を求めます。

```
Do you accept Visual Studio license at https://go.microsoft.com/fwlink/?LinkId=2327714 [Y/N] ?
```

URL は実行時に取得される為、常に配信側の最新の物になります。
2026-08 時点では下記でした。

- Visual Studio Build Tools 2026 — <https://go.microsoft.com/fwlink/?LinkId=2327714>
- Visual Studio Build Tools 2022 — <https://go.microsoft.com/fwlink/?LinkId=2179911>

同意しなかった場合は取得を中止します。確認を省略したい場合
(CI 等) は明示的に指定します。

```bash
TSMEMORY_ACCEPT_MSVC_LICENSE=1 bash tests/tools/setup-msvc.sh
```

### ビルド結果を第三者に配布する場合

もし利用者がビルドしたバイナリを配布する場合は、その利用者が
GPLv2 第 3 条の義務 (ソースの提供) を負います。`TSMemory.tvtp` が対象で、
`TSMemory-TVTestSrc.aux2` は対象外です。
また m2v のバイナリ再配布については 3 章の「確認が残っている点」を
参照してください。

---

## 3. `src/m2v/` (MPEG-2 VIDEO VFAPI Plug-In) について

作者である茂木和洋氏の配布ページに、以下の記載があります。

> 【ソースコードの利用について】
> 　ソースを利用したことによって何らかの損害（例えば特許問題とか、
> プログラムのバグとか）が発生しても作者は何ら責任を負いません。
> 　以上の条件に従える方のみソースを利用ください。
> これ以外の制限はありません

出典: <https://www.marumo.ne.jp/mpeg2/>

無保証を受け入れる以外に制限は無く、**著作権表示の保持義務も課されていない**
為、MIT より緩い条件です。既存のライセンス名 (MIT 等) に置き換えると
作者の定めた条件を正確に伝えない事になる為、**原文のまま記録しています**。

`src/m2v/LICENSE.txt` にも同じ内容を置いてあります。

### 確認が残っている点

- 上記は**ソースコードの利用**に関する記載です。本プロジェクトは
  ソースのみを公開する為 (2 章)、この記載の範囲内に収まっており、
  **現時点で問題はありません**。
  ただし、ビルドしたバイナリを第三者に配布する場合は、配布ページの
  他の項にアーカイブ・バイナリの再配布に関する別の条件が無いかを
  確認してください (未確認です)。
- 本リポジトリの m2v は **ver.0.7.14 ベース**です
  (オリジナルの TSMemory の更新履歴による)。配布ページの最新は 0.7.16 で、
  版によって記載が異なる可能性があります。

---

## 4. 未解決の事項

- **`src/tvtp/TSMemory.cpp` の元になったオリジナルの TSMemory** —
  ライセンスの記載が見つかっていません
  (配布物に同梱の `TSMemory.txt` にも記載なし)。
  BonTsEngine と結合する以上 GPL-2.0-or-later として扱うほかありませんが、
  原著作者がそれに同意しているかは別問題です。

原著作者への確認が確実です。

---

## 5. 免責

本プログラムによるいかなる損害も補償しません。

この文書はリポジトリ内の実際の構成を調査してまとめたものであり、
法的な助言ではありません。
