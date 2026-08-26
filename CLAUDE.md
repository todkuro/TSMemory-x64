# 開発ルール

## 外部ツールの扱い（厳守）

- ビルド・テスト・補助スクリプトが使う外部プログラムは、**全て
  `compilers/` 配下に用意する**。システムにインストールされたものを
  使わない。**コンパイラに限らず、Python 等のスクリプト実行系も含む。**
- `compilers/` に無いものが必要になった場合は、**勝手にローカルのもので
  代替せず、まず報告して指示を仰ぐ**。解決する場合は取得スクリプトを
  `tools/setup-*.sh` として追加し、`compilers/` に展開する形にする。
- **例外**: シェル (`bash`) と、Git Bash 同梱の基本コマンド
  (`curl` `tar` `unzip` `git` `sed` `grep` `awk` `find` 等) のみ
  使用してよい。これらはコマンドを実行する土台そのものである為。
- 環境変数の恒久的な変更、システムへのインストール、レジストリの変更は
  行わない。一時的な `PATH` の前置き (`export PATH="$TOOLCHAIN/bin:$PATH"`)
  はスクリプト内に限り可。

### 現在 `compilers/` に置くもの

| 用途 | 取得スクリプト | 展開先 |
| --- | --- | --- |
| clang / lld / windres (llvm-mingw) | `tools/setup-toolchain.sh` | `compilers/llvm-mingw/` |
| Python 3 (embeddable) | `tools/setup-python.sh` | `compilers/python/` |
| MSVC (TVTest 本体のビルド用・任意) | `tests/tools/setup-msvc.sh` | `compilers/msvc/` |

原典のソースは `tools/setup-*-src.sh` が本家から clone する
(`third_party/` 以下。`.gitignore` 対象)。

| 原典 | 取得スクリプト | 展開先 | 何に要るか |
| --- | --- | --- | --- |
| TVTest (DBCTRADO) | `tools/setup-tvtest-src.sh` | `third_party/TVTest/` | 動作確認用の 64bit TVTest をビルドする時 |
| TSMemory (dtvgit) | `tools/setup-tsmemory-src.sh` | `third_party/TSMemory/` | `src/m2v/` を pristine から作り直す時 (`tools/regen-m2v.sh`) |
| TvtPlay (xtne6f) | `tools/setup-tvtplay-src.sh` | `third_party/TvtPlay/` | `test-live.sh` (TVTest 実走行) |
| libaribcaption (xqq) | `tools/setup-libaribcaption-src.sh` | `third_party/libaribcaption/` | 字幕の追加記号表を作り直す時 (`tools/regen-gaiji.sh`) |
| AviUtl ExEdit2 本体 | `tools/setup-aviutl2.sh` | `third_party/aviutl2/` | 実機での確認・`test-live.sh` |

いずれも `bash tools/build.sh` と `bash tests/tools/test.sh` には不要。

`sdk/aviutl2/` (プラグイン SDK のヘッダ) は**バージョン管理する**。
更新は `tools/update-aviutl2-sdk.sh` で行う。**手動更新用の道具であり、
ビルドからは呼ばない**。既定は差分を見せるだけで、`--apply` を付けた
時だけ入れ替える。

`compilers/` は `.gitignore` 対象。各 `setup-*.sh` で再取得できる事。

## ディレクトリの使い分け

- `tools/` … **構築に必要な最低限のみ**。
  `build.sh` / `zipdir.py`、
  原典とツールチェインの取得 `setup-toolchain.sh` / `setup-python.sh` /
  `setup-tvtest-src.sh` / `setup-tsmemory-src.sh` / `setup-tvtplay-src.sh` /
  `setup-aviutl2.sh`、
  ソースを再生成する `regen-m2v.sh` / `patch64.py` / `gen_simd_stub.sh` /
  `to_utf8.py` / `regen-gaiji.sh` / `gen_gaiji.py`、
  SDK を手動更新する `update-aviutl2-sdk.sh`。
- `tests/` … テストのソース (`test_*.cpp` 等)。**バージョン管理する**。
- `tests/tools/` … テスト・デバッグ用のスクリプトとツール。
  **環境に依存しない物だけをバージョン管理する** (`.gitignore` で選別)。

  **このリポジトリで書いた物は全て管理する。**
  MSVC も TVTest も AviUtl2 も取得スクリプトで揃うようになったので、
  「MSVC が要る」「TVTest が要る」は除外の理由にならない
  (要る物を取る手順ごと管理する)。

  | 管理しない物 | 理由 |
  | --- | --- |
  | `portable-msvc.py` | 第三者のコード (mmozeiko 氏の gist)。ライセンスの表示が無いので再配布しない。`setup-msvc.sh` が `compilers/` に取得する |
  | `__pycache__` | 生成物 |

新しくスクリプトを足す時は、

1. **構築に要るか** → 要るなら `tools/`
2. そうでなければ `tests/tools/` に置いて `.gitignore` に `!` を足す。
   **第三者のコードを持ち込む場合だけ**、リポジトリに入れず
   取得スクリプト経由にする

## 報告義務

- 上記に抵触する可能性のある操作を行う場合は、**実行前に申告する**。
- 調査の過程で一時的にローカルのツールを使う必要が生じた場合も、
  その旨を明示する。黙って使わない。

## リポジトリの方針

- **ソースのみを公開し、バイナリの配布は行わない。**
  ライセンスの適用範囲は `LICENSE.md` を参照。
- **クローンした直後の状態でビルドと全てのテストが通る事。**
  外から持って来る物は全て取得スクリプト経由にする。
  原典は `tools/setup-*.sh` で `third_party/` に取る。
- **テスト用の TS はバージョン管理しない。**
  放送 TS は再配布出来ない為、`tests/tools/gen-ts-examples.cpp` が
  `build/ts-examples/` に合成する。管理するのは合成する側だけ。
  実際の放送 TS を持っている場合は同じ場所に足せば一緒に検査される。
- `src/m2v/` への変更は `tools/patch64.py` に集約する。
  ソースを直接編集して済ませない (オリジナルから再生成できる状態を保つ)。
- **生成したソースは commit する。**`src/m2v/` と
  `src/aviutl2/caption/arib_gaiji.*` は生成物だが、クローン直後に
  ビルドとテストが通る必要がある為に管理する。作り直す道具
  (`regen-m2v.sh` / `regen-gaiji.sh`) は**ビルドから呼ばない**。

## 作業の進め方

- コードを変更したら `bash tools/build.sh` と `bash tests/tools/test.sh` を通す。
- 動作の根拠は実測で示す。推測で「動くはず」と述べない。
- **版は `CHANGELOG.md` の一番上の `## ver.X.Y.Z` が唯一の正。**
  `tools/build.sh` がそこから読み取り、プラグインの版文字列
  (`build/generated/tsmemory_version.h`) と `package.ini` / `package.txt`
  の `@VERSION@` に流し込む。**ソースや ini に版を直書きしない。**
  リリース時に書き換えるのは `CHANGELOG.md` だけ。
- ドキュメントは以下の役割で分ける。
  - `README.md` … 利用者向け。オリジナルの `TSMemory.txt` と同じ粒度。
    **版は書かない** (`CHANGELOG.md` を参照させる)
  - `CHANGELOG.md` … 更新履歴。版の正
  - `docs/development.md` … 開発者向け。実装上の判断や動作確認の記録
  - `docs/audio-support.md` … 音声対応の調査メモ (実装済み。
    実装前の調査と作業項目の記録として残している)
  - `LICENSE.md` … ライセンスとその適用範囲
