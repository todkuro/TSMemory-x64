# TSMemory (64bit / AviUtl ExEdit2 対応版)

TSMemory は、TVTest と AviUtl を連携させて映像メモリ機能を実現するためのプラグインセットです。
静止画キャプチャ用途での使用を想定しています。

これは 64bit の TVTest と AviUtl ExEdit2 (AviUtl2) で動作するようにしたバージョンです。
AviUtl 1.xx 版の `TVTestSrc.aui` と `CaptureUtil.auf` にあたる部分は、
AviUtl2 の汎用プラグイン `TSMemory-TVTestSrc.aux2` ひとつにまとまっています。

TVTest ver.0.10.0-dev-dce8b3e (x64)  と AviUtl ExEdit2 ver.2.1.5 で動作確認しています。


## 構成（ビルド後）

TVTest 側

    TSMemory.tvtp                   TVTest プラグイン
    TSMemory.ini                    その設定

AviUtl2 側

    TSMemory-TVTestSrc.aux2         汎用プラグイン (映像の読み込み・キャプチャ)
    TSMemory-TVTestSrc.ini          その設定
    English.TSMemory-TVTestSrc.aul2 英語表示用の言語ファイル
    TSMemory字幕背景.anm2           字幕の背景を敷くスクリプト


## ビルド

**本プロジェクトはソースのみを公開しており、バイナリの配布は行いません。**
使用する場合はご自身でビルドしてください。 
※用意されているスクリプトによってビルドしやすくなっています。

### 用意するもの

簡易ビルドは Windows 上で行います。必要なのは次の 1 つだけです。

- **Git for Windows** — 同梱の **Git Bash** でコマンドを実行します
  <https://gitforwindows.org/>

**コンパイラ等の環境はスクリプトがダウンロードして用意します。**
コンパイラ (llvm-mingw) は次の手順でプロジェクト内の `compilers/` に
展開され、システムには一切触れません。

※Linuxでもビルドできないことはありませんが、テストスクリプトは動きません。

### 手順

**コマンドプロンプトや PowerShell ではなく、Git Bash を開いてください。**
スタートメニューの「Git Bash」、またはフォルダを右クリックして
「Open Git Bash here」で開けます。

```bash
git clone https://github.com/todkuro/TSMemory-x64.git
cd TSMemory-x64
bash tools/setup-toolchain.sh
bash tools/build.sh
```

`setup-toolchain.sh` は初回のみ必要です。llvm-mingw をダウンロードして
`compilers/` に展開します (展開後で約 730MB あります)。

`dist/` に以下が生成されます。

    dist/TSMemory.tvtp                     TVTest プラグイン
    dist/TSMemory-TVTestSrc.aux2           AviUtl2 プラグイン
    dist/TSMemory-TVTestSrc.au2pkg.zip     AviUtl2 用インストールパッケージ
    dist/TSMemory-x64.zip                  上記一式

### 動作確認 (任意)

```bash
bash tests/tools/test.sh
```

ビルドしたプラグインに対する自動テストが走ります。

TS を使うテストは `build/ts-examples/` の TS を対象にします。
放送された TS は再配布出来ないので、無ければ初回に合成します
(MPEG-2 の映像と AAC の音声を含む TS を作ります)。
実際の放送 TS を持っている場合は同じ場所に置いてください。
**置いた TS も含めて全て検査されます。**

```bash
# 別の場所を使う
TSMEMORY_TS_DIR=/path/to/ts bash tests/tools/test.sh

# TS を作り直す (ディレクトリごと消してから)
rm -rf build/ts-examples && bash tests/tools/gen-ts-examples.sh

# 実物の ISDB-T サンプルを足す (手動。計 260MB を取得します)
bash tests/tools/fetch-ts-samples.sh
```

ビルドの仕組みや、TVTest 本体を 64bit ビルドする手順などは
[docs/development.md](docs/development.md) を参照してください。


## 導入方法

1. ビルドで生成された `dist/TSMemory-TVTestSrc.au2pkg.zip` を
   AviUtl2 のプレビュー画面にドラッグ＆ドロップします。
   手動で入れる場合は以下に配置してください。

       %ProgramData%\aviutl2\Plugin\TSMemory-TVTestSrc\TSMemory-TVTestSrc.aux2
       %ProgramData%\aviutl2\Plugin\TSMemory-TVTestSrc\TSMemory-TVTestSrc.ini
       %ProgramData%\aviutl2\Language\English.TSMemory-TVTestSrc.aul2
       %ProgramData%\aviutl2\Script\TSMemory字幕背景.anm2

2. TVTest の Plugins フォルダに `TSMemory.tvtp` と `TSMemory.ini` を入れます。

3. `TSMemory.ini` を開いて、`AviUtlPath` に `aviutl2.exe` のパスを設定します。
   遡れる長さを変えたい場合は `MemorySize` も調整してください。

4. 必要に応じて `TSMemory-TVTestSrc.ini` を編集して設定を変更してください。

5. TVTest を起動し、メニューの [設定] で設定ダイアログを表示させ、
   左のリストから [キー割り当て] を選択して、TSMemory の `Execute` に
   キーを割り当てます。

6. メニューの [プラグイン] から、TSMemory をチェック状態にします。

AviUtl 1.xx 版で必要だった「入力プラグイン優先度の設定」「最大画像サイズの
変更」「TVTest_Image.dll のコピー」は、いずれも不要です。


## 使い方

1. キャプチャしたいシーンになったら、導入方法の 5 で割り当てたキーを押します。
   AviUtl2 が起動して、映像メモリの内容がタイムラインに配置されます。

2. AviUtl2 のメニューから「キャプチャ・ユーティリティ」を表示させます。
   上段に保存する画像のファイル名を入力します。
   ファイル名の拡張子が省略されている場合は拡張子が付加されます。
   既に同名のファイルが存在する場合は、末尾に連番が付加されます。
   下段でフォーマットと JPEG の品質を指定します。

3. 「保存」ボタンを押すと画像がファイルに保存されます。
   ファイルメニューの「画像として保存 (TSMemory)」からも保存できます。

4. 以後、キャプチャ実行キーを押すと AviUtl2 で読み込まれます。


## 字幕を取り込む

字幕を、映像とは別のレイヤーに**テキストオブジェクトとして**並べます。
画像の焼き込みではないので、後から書体も色も文言も直せます。
既定では無効です。

### 1. AviUtl2 側の ini を有効にする

**片方だけでは字幕は出ません。** TVTest 側で落とされると
AviUtl2 側では何もできない為です。

    TSMemory.ini (TVTest 側)           TSMemory-TVTestSrc.ini (AviUtl2 側)
    [Settings]                         [Caption]
    Subtitle=1  ← 既定で 1             Enable=1    ← ここを 1 にする
                                       Layer=2

**TVTest 側は既定で通してあります**ので、普通は AviUtl2 側の
`Enable=1` だけで済みます。

`[Caption] Layer` は**映像 (`[Bridge] Layer`) とは別の番号**にしてください。
重なると映像が消えてしまう為、その場合は字幕を置かずに
ログへ警告を出します。

**1 つの字幕の行ごとにレイヤーを 1 本ずつ使います。**同じレイヤーには
時間の重なるオブジェクトを置けない為、2 行の字幕なら 2 本使います
(最大 8 本)。何本使ったかは取り込み後のログに出ます。

**ini はどちらもプラグインの読み込み時にしか読みません。**
`TSMemory.ini` を変えたら TVTest を、`TSMemory-TVTestSrc.ini` を変えたら
AviUtl2 を、それぞれ再起動してください
(プラグインのチェックを外して入れ直すだけでは読み直されません)。

#### TVTest 側だけ既定で 1 にしている理由

再起動の要る側が違う為です。**視聴中・録画中に落としたくないのは
TVTest の方**なので、TVTest 側は最初から通しておき、
AviUtl2 側で切ってあります。この形なら字幕が欲しくなった時に
**AviUtl2 の再起動だけ**で切り替えられます。

`[Caption] Enable=0` の時は AviUtl2 側で字幕を一切読まないので、
処理は増えません。共有メモリに字幕のぶんが載りますが、
**取り込む量の 0.01〜0.02%** です (実測。放送 8 番組)。
`MemorySize` で溜められる秒数はほぼ変わりません。

字幕が要らず 1 バイトも通したくない場合は `Subtitle=0` にしてください。

### 2. 取り込む

いつも通りキャプチャ実行キーを押します。映像が置かれた後、
`[Caption] Layer` のレイヤーに字幕が並びます。**放送は行ごとに位置を
持っている**ので、1 行につき 1 つのテキストオブジェクトになります。

うまくいくと AviUtl2 のログにこう出ます。

    TSMemory: 字幕を 34 件配置しました (レイヤー 2 / 外字 6 字形)

ログウィンドウは古い行が流れてしまうので、
`C:\ProgramData\aviutl2\Log\aviutl2_*.log` を見るのが確実です。

外字 (DRCS) は放送されてきた字形をその場でフォントに組み立てて使うので、
「〓」のような代替文字にはなりません。フォントのインストールも不要です。
ただし**字形の定義は数十秒おきにしか流れて来ない**ので、
`MemorySize` が小さいと間に合わない事があります。その場合は

    TSMemory: 外字 2 個は字形が届いていません (MemorySize を大きくすると拾えます)

と出るので、TVTest 側の `MemorySize` を大きくしてください。

### 3. 書体をまとめて変える (任意)

字幕 1 つずつ触らなくて済むように、**AviUtl2 のテキストプリセット**を
使います。

1. テキストオブジェクトを 1 つ選び、「オブジェクト設定」ウィンドウで
   フォント・文字装飾・色・サイズを好みに整えます
2. 同ウィンドウのアイコンメニューから**プリセットを作成**します。
   名前は `字幕` など。作成時に「保存対象とする設定項目」を選べます
3. `TSMemory-TVTestSrc.ini` に名前を書きます

       [Caption]
       Preset=字幕

以後、全ての字幕の先頭に `<$字幕>` が入ります。**プリセットを 1 つ直せば
全ての字幕に効きます。**プリセットは
`%ProgramData%\aviutl2\Preset\テキスト.字幕.preset` に保存される
テキストファイルなので、手で編集したり持ち運んだりもできます。

反映されるのは制御文字で変更できる項目 (フォント・文字装飾・色・サイズ・
字間・行間) だけです。「配置」のような項目はオブジェクト設定で直接
変えてください。

### 設定一覧 (`[Caption]`)

| 設定 | 既定 | 説明 |
| --- | --- | --- |
| `Enable` | 0 | 字幕を取り込むか |
| `Layer` | 2 | 配置先レイヤーの**先頭**。行の数だけ下へ使う |
| `Preset` | (空) | 先頭に入れるテキストプリセット名 |
| `UseBroadcastColor` | 1 | 放送の文字色を使うか。0 でプリセット任せ |
| `UseBackColor` | 1 | 放送の背景色を影・縁色として使うか |
| `DrcsFont` | TSMemory DRCS | 外字用に組み立てるフォントの名前 |
| `UseBroadcastSize` | 0 | 放送の文字サイズに合わせるか |
| `UsePosition` | 1 | 放送の表示位置に合わせるか |
| `OffsetX` / `OffsetY` | 0 | 位置の微調整 (ピクセル) |
| `BackOpacity` | 50 | 背景の箱の不透明度 (0 で背景なし) |
| `BackOutline` | 2 | 文字の縁取りの太さ (0 で縁取りなし) |
| `Ruby` | 1 | ふりがなを `</>漢字<!>ふりがな</>` にするか |

`[Bridge] LockLayer=1` にしている場合は、字幕のレイヤーも配置後に
ロックされます (次の取り込みでは自動で外れます)。

### 背景の箱

放送の字幕には**半透明の黒い箱**が敷かれています。これは同梱の
スクリプト「TSMemory字幕背景」で再現します。取り込み時に自動で
テキストに貼られるので、操作は要りません。

**描画後の実寸を見て敷く**ので、後からフォントや文字サイズ・文言を
変えても箱が付いて来ます。余白・角丸・色はオブジェクト設定の
スクリプトの項目で変えられます。

`[Caption] BackOpacity=0` にすると背景を付けません。

### 文字の縁取り

同じスクリプトが**文字に黒い縁**も付けます。TVCaptionMod2 の字幕に
付いているものと同じで、背景の上でも文字が読みやすくなります。

太さは `[Caption] BackOutline` (既定 2 ピクセル、0 で付けない)、
色はオブジェクト設定のスクリプトの「縁色」で変えられます。
`BackOpacity=0` で背景を切っていても、縁だけ付ける事ができます。

既定の 2 ピクセルは、**1080p で放送どおりの文字サイズ**にした時に
TVCaptionMod2 と同じ位になる値です。**文字を大きくしても縁は
太くなりません**ので、大きさを変えた場合はスクリプトの
「縁取り」で合わせてください。

なお ARIB にも縁取りの指定 (ORN) はありますが、**送って来る放送は
限られる**ので当てにしていません (実測: 字幕を持つ 8 本中 2 本)。
TVCaptionMod2 も同じく、既定では全ての字幕に縁を付けています。

### できない事

- **文字スーパー**(緊急放送等の別系統の字幕) は取り込みません。


## ヒント

- AviUtl2 でのシーク操作が重い場合は、`TSMemory-TVTestSrc.ini` の
  `aspect_ratio` を 0 に設定して AviUtl2 の側でリサイズするようにすれば
  多少軽くなる可能性があります。
- 画像の保存時にファイル名をクリップボードにコピーしたい場合は、
  `TSMemory-TVTestSrc.ini` の `CopyFileName` を 1 にしてください。

- AviUtl2 は 1.xx 版と違って各フィルタの初期値を保存しておけません。
  インターレース解除やノイズ除去などの設定を AviUtl2 の
  「フィルタのプリセット」として保存しておき、その名前を
  `TSMemory-TVTestSrc.ini` の `Preset` に書いておくと、取り込んだ映像に
  自動で適用されます。

      [Bridge]
      Preset=キャプチャ用フィルタ

- TVTest から渡されるのはコマンドを実行した時点までの映像なので、
  目的の場面は末尾にある事が多くなります。`TSMemory-TVTestSrc.ini` の
  `SeekToEnd` を 1 にすると、取り込んだ直後のシーク位置が先頭ではなく
  末尾になります。

      [Bridge]
      SeekToEnd=1

- プレビュー画面をドラッグして取り込んだ映像を動かしてしまう事故を
  防ぎたい場合は、`TSMemory-TVTestSrc.ini` の `LockLayer` を 1 にすると
  取り込んだ後に配置先レイヤーがロックされます。次の取り込みでは自動的に
  解除して置き直すので、手での操作は要りません。

      [Bridge]
      LockLayer=1

  オブジェクトリストにある「プレビュー編集の操作をロック」はオブジェクト
  単位のロックで、プラグインからは操作出来ません。こちらはレイヤー単位の
  ロックになりますが、TSMemory は専用レイヤーに置く為 効果は同じです。

- TSMemory がタイムラインに映像を置くと編集済み扱いになる為、AviUtl2 の
  終了時に「現在の編集データは更新されています」という確認が出ます。
  煩わしい場合は `TSMemory-TVTestSrc.ini` の `SuppressExitConfirm` を
  1 か 2 にすると自動で「いいえ」と応答します。

      1 … TSMemory が配置しただけの状態に限って応答します
      2 … 編集していても応答します (編集内容は保存されません)
  確認ダイアログが表示される際のシステム音は消せません。

- TVTest の終了時に AviUtl2 も終了させたい場合は、`TSMemory.ini` の
  `AutoClose` を 1 にしてください。

- 音声の取り込みに対応しています。有効にするには**両方**の設定が要ります。

      TSMemory.ini (TVTest 側)           TSMemory-TVTestSrc.ini (AviUtl2 側)
      [Settings]                         [M2V]
      Audio=1                            audio=1

  音声 PID の分だけリングバッファを食うので、同じ `MemorySize` なら
  遡れる時間はその分短くなります。
  静止画キャプチャが主な用途な為、既定は映像のみにしてあります。

- 字幕の取り込みに対応しています。設定と手順は
  「[字幕を取り込む](#字幕を取り込む)」を参照してください。

## 注意

- このプログラムによるいかなる損害も補償しません。

- TVTest・AviUtl2・本プラグインは全て 64bit で揃える必要があります。
  32bit の TVTest では `TSMemory.tvtp` は読み込めません。

- 字幕には対応していません。

- 遡れる長さは `MemorySize` (既定 10MB) が上限です。地上デジタルの 1080i で
  おおよそ 8〜14 秒程度です。また、溜め込みはプラグインを有効にした時点から
  始まるので、有効にした直後にキャプチャするとその分しか取れません。

- 取り込んだ映像の前後は、切れた位置の GOP が途中で終わっているぶん
  短くなります。先頭が 0.4 秒程度、末尾が 0.2 秒程度です。

- マルチ編成の局では、視聴中のチャンネルの映像が取り込まれます。
  全チャンネルを同時に取り込む事は出来ません。

- パッケージを入れ直す時は、**必ず AviUtl2 を終了させてから**行ってください。
  AviUtl2 は終了時にプラグインをアンロードした後もプラグインが登録した
  情報を参照する為、本プラグインは動作中アンロードされないようにして
  います。その為 AviUtl2 の実行中はファイルが掴まれたままになり、
  入れ替えに失敗します。

- ver.0.2.1 以前の AviUtl 1.xx 版とは別物です。同じ AviUtl2 に旧版の
  `TSMemory` パッケージが入っている場合は、先に削除してください。


## ライセンス

プラグインごとに異なります。

| ファイル | ライセンス |
| --- | --- |
| `TSMemory.tvtp` | **GPL-2.0-or-later** |
| `TSMemory-TVTestSrc.aux2` | GPL の対象外 (GPL のコードを含みません) |

`TSMemory.tvtp` は TVTest 由来の BonTsEngine を組み込んでいる為、
バイナリ全体が GPL-2.0-or-later になります。全文は
`licenses\GPL-2.0.txt` にあります。

`TSMemory-TVTestSrc.aux2` には GPL のコードが含まれません。
2 つのプラグインは共有メモリ経由で通信する別プロセスです。

適用範囲の詳細と、その確認方法は [LICENSE.md](LICENSE.md) を参照してください。


## 謝辞

`TSMemory-TVTestSrc.aux2` の映像デコード部は、茂木和洋氏の開発した
MPEG-2 VIDEO VFAPI Plug-In (ver.0.7.14 ベース) を元にしています。
このような有用なプログラムを開発・公開して下さっている茂木氏に感謝します。

    https://www.marumo.ne.jp/mpeg2/

利用条件は同ページの【ソースコードの利用について】のとおりで、
無保証を受け入れる以外の制限はありません (全文は LICENSE.md に引用)。

また、オリジナルの TSMemory およびその改造版の作者の方々に感謝します。

`TSMemory-TVTestSrc.aux2` は AviUtl ExEdit2 Plugin SDK を利用しています。

    The MIT License
    Copyright (c) 2025 Kenkun

全文は `AviUtl2-Plugin-SDK-license.txt` にあります
(プラグインをインストールしたフォルダと、配布物の `licenses` フォルダ)。


## 動作原理

映像メモリ機能を実行すると、TVTest が溜め込んでいる TS データを
`tsmemory*.tvtv` という名前の共有メモリに写し、同じ名前の 0 バイトの
ダミーファイルを作成します。

AviUtl2 が起動していない場合は、このダミーファイルを引数に渡して
起動します。既に起動している場合は、名前付きイベントで
`TSMemory-TVTestSrc.aux2` に読み込みを要求します
(AviUtl2 は `WM_DROPFILES` を受け付けない為、1.xx 版とは方法が異なります)。

`TSMemory-TVTestSrc.aux2` は拡張子が `.tvtv` のファイルの入力プラグインとして
振る舞い、ファイル名と同じ名前の共有メモリから TS データを取得して開きます。

AviUtl2 はメディアファイルの内容をパスをキーにキャッシュする為、実行の度に
別名の共有メモリとダミーファイルを作っています。`SnapshotCount` で指定した
数だけ過去の読み込み結果が有効なまま残り、古いものから順に破棄されます。


## 更新履歴

[CHANGELOG.md](CHANGELOG.md) を参照してください。


## 開発者向け

ビルド方法、実装上の判断、隠し設定などは
[docs/development.md](docs/development.md) にまとめてあります。

音声対応の経緯 (調査メモ) は
[docs/audio-support.md](docs/audio-support.md) にあります。

全てAIを利用し書かせています。
読みにくいかもですが、これらをAIに読ませればバグの検証などがやりやすくなるはずです。
