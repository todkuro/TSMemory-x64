# TSMemory 64bit 版 — 開発メモ

> 開発者向けの文書です。移植の過程で判明した事や実装上の判断、動作確認の
> 内容、隠し設定などを記録してあります。
> 導入方法や使い方は [../README.md](../README.md) を参照してください。

TVTest と AviUtl を連携させて映像を送る TSMemory を、**64bit ビルド**かつ
**AviUtl ExEdit2 (AviUtl2) 対応**にしたものです。静止画キャプチャ用途を想定しています。

オリジナルは AviUtl 1.xx / 32bit 専用でした。本リポジトリはそれを土台に、
64bit 化と AviUtl2 のプラグイン API への移植を行っています。

---

## 構成

| ファイル | 配置先 | 内容 |
| --- | --- | --- |
| `TSMemory.tvtp` | TVTest の `Plugins` | TVTest プラグイン (x64)。選択中サービスの映像・音声 TS を共有メモリに溜め、コマンド実行で AviUtl2 に渡す |
| `TSMemory.ini` | 同上 | TVTest 側の設定 |
| `TSMemory-TVTestSrc.aux2` | aviutl2 の `Plugin\TSMemory-TVTestSrc` | AviUtl2 汎用プラグイン。下記 3 つの機能をまとめて持つ |
| `TSMemory-TVTestSrc.ini` | 同上 | AviUtl2 側の設定 |
| `English.TSMemory-TVTestSrc.aul2` | aviutl2 の `Language` | 英語表示用の言語ファイル |

`TSMemory-TVTestSrc.aux2` の中身:

1. **`*.tvtv` 入力プラグイン** — 共有メモリ上の MPEG-2 TS を読む
   (茂木和洋氏の *MPEG-2 VIDEO VFAPI Plug-In* を 64bit 化して利用)
2. **連携の待ち受け** — TVTest からの要求を受けてタイムラインに配置する
3. **キャプチャ・ユーティリティ** — 現在のフレームを画像ファイルに保存する

AviUtl 1.xx 版の `TVTestSrc.aui` と `CaptureUtil.auf` は、
AviUtl2 では汎用プラグイン 1 つに統合しています。

---

## インストール

1. `TSMemory-TVTestSrc.au2pkg.zip` を AviUtl ExEdit2 のプレビュー画面に D&D する
   (手動で置く場合は上表のとおりに配置してください)
2. `TSMemory.tvtp` と `TSMemory.ini` を TVTest の `Plugins` フォルダに置く
3. `TSMemory.ini` (TVTest 側) を開き、`AviUtlPath` に `aviutl2.exe` のパスを設定する
4. TVTest のメニューから [設定] → [キー割り当て] で TSMemory の `Execute` にキーを割り当てる
5. TVTest のメニューの [プラグイン] から TSMemory をチェック状態にする

> **注意**: TVTest / AviUtl2 / 本プラグインは全て 64bit で揃える必要があります。
> 32bit の TVTest では `TSMemory.tvtp` は読み込めません。

> **更新時の注意**: パッケージを入れ直すと AviUtl2 はプラグインを差し替える為に
> 一度アンロードします。この時プラグイン側が後始末をしないと、アンロード済みの
> コードが呼ばれて AviUtl2 ごと落ちます
> (WER に `TSMemory-TVTestSrc.aux2_unloaded` として記録されます)。
> ver.0.3.0 でウィンドウとウィンドウクラスを確実に破棄するようにしています。
> それでも失敗する場合は AviUtl2 を終了してから、
> `%ProgramData%\aviutl2\Plugin\TSMemory-TVTestSrc` を削除してやり直してください。
> 更新で `TSMemory-TVTestSrc.au2pkg.zip` のD&Dを使った場合、**設定ファイル** が
> 配布のもので上書きされる可能性があります。

---

## 使い方

1. キャプチャしたいシーンで、割り当てたキーを押す
2. AviUtl2 が起動していなければ起動され、映像がタイムラインに配置される
3. 画像を保存するには下記のどちらかを使います。
   - ファイルメニューの「画像として保存 (TSMemory)」
     … `TSMemory-TVTestSrc.ini` の `[Capture]` の設定で現在のフレームを保存します
   - 「キャプチャ・ユーティリティ」ウィンドウ
     … ファイル名・形式・JPEG 品質をその場で指定して [保存]

同名のファイルが既にある場合はベース名の末尾に連番が付きます。
保存先はログに出力されます。

> **キャプチャ・ユーティリティのウィンドウが見当たらない場合**
> 追加直後は非表示になっています。メニューバーの **[表示]** に
> 「キャプチャ・ユーティリティ」が追加されているので、そこから表示に
> 切り替えてください (AviUtl2 のプラグインウィンドウ共通の挙動です)。
> ログに `register window client [キャプチャ・ユーティリティ]` が出ていれば
> 登録自体は成功しています。

---

## AviUtl 1.xx 版からの変更点

### 1. 連携方法

AviUtl 1.xx 版は、ウィンドウクラス名 `AviUtl` のウィンドウを探して
`WM_DROPFILES` を投げていました。AviUtl ExEdit2 はファイルの D&D を
OLE (`IDropTarget`) で受けているため、この方法では反応しません。

そこで、AviUtl2 側プラグインが名前付きオブジェクトを作って待ち受け、
TVTest 側がそこへ読み込ませたいパスを渡す方式に変更しました。

| 名前 | 種類 | 用途 |
| --- | --- | --- |
| `TSMemoryBridge.Ready` | Mutex | AviUtl2 側プラグインの生存確認 |
| `TSMemoryBridge.Param` | FileMapping | 読み込ませるファイルのパス |
| `TSMemoryBridge.ParamLock` | Mutex | 上記の排他 |
| `TSMemoryBridge.Request` | Event | 要求の通知 |

受け取った側は `EDIT_SECTION::create_object_from_media_file()` で
タイムラインにメディアオブジェクトを作成します。

> **待ち受けはプロジェクトの初期化が終わってから始めます。**
> AviUtl2 は「プラグインの登録 (`RegisterPlugin`)」→「プロジェクトの初期化」
> の順で起動します。登録の時点で `TSMemoryBridge.Ready` を作ってしまうと、
> TVTest から AviUtl2 を起動した場合に初期化前の状態でオブジェクトを作って
> しまい、直後の初期化でタイムラインごと消えてしまいます
> (AviUtl2 が既に起動している場合は起きません)。
> その為 `register_project_load_handler()` の通知を受けてから
> `Ready` を作るようにしています。`ReadyDelay` で余裕を調整出来ます。
>
> TVTest 側の `LaunchWait` は「諦めるまでの上限」で、待ち受け開始は
> 0.25 秒毎に確認して確認出来次第すぐ進みます。AviUtl2 が起動直後に
> 終了した場合も待たずに切り上げます。
> **時間の設定はどれも「秒」です** (`ReadyDelay` は小数も可)。

> **起動前に AviUtl2 のプロセスが在るかを必ず確認します。**
> `IsAviUtlReady()` は「AviUtl2 側プラグインが待ち受けているか」しか
> 見ません。待ち受けはプロジェクトの初期化後に始まる為、起動直後は
> まだ偽です。更にプラグインが入っていない・無効な場合は**永久に偽の
> まま**になります。
>
> これだけを見て `LaunchAviUtl()` を呼ぶと、既に AviUtl2 が動いていても
> もう 1 つ起動してしまい、キーを押す度にインスタンスが増えます。
> 二重起動したインスタンスはプラグインの初期化を終える前に異常終了し、
> 「TVTest から起動した時だけ AviUtl2 が落ちる」という形で表面化しました。
> (WER のモジュール一覧に `TSMemory-TVTestSrc.aux2` が現れず、
>  先に読み込まれる入力プラグインだけが残っている、という状態になります。)
>
> その為 `IsAviUtlRunning()` で実行ファイル名からプロセスの在否を調べ、
> 既に在る場合は起動せずに待ち受けの開始だけを待ちます。
> 待っても応答しない場合は「起動しているがプラグインが応答しない」と
> 判る文面のメッセージを出します。

> **オブジェクトの長さは明示的に渡しています。**
> `length` に 0 を渡すと「追加位置から自動調整」になりますが、これは
> 取り込んだ映像の長さではなく AviUtl2 側の**既定のオブジェクト長**に
> なってしまい、後ろが切れます (別々の TS を取り込んでも毎回同じ
> 81 フレームになる、という形で表面化しました)。
> `get_media_info()` で取得した長さをシーンのフレームレートで
> フレーム数に直して渡しています。

### 2. スナップショット方式

AviUtl2 はメディアファイルの内容をパスをキーにキャッシュします。
1.xx 版のように毎回同じ名前のダミーファイルを読ませると、2 回目以降に
前回の映像が表示されてしまいます。

そのため、実行の度に**別名の共有メモリとダミーファイル**
(`tsmemory<N>_<連番>.tvtv`) を作り、その時点のリングバッファの内容を
線形化してコピーするようにしました。

`SnapshotCount` で指定した数だけ過去のスナップショットが保持され、
古いものから順に破棄されます (破棄されたものはタイムラインに残っていても
再読み込み出来なくなります)。

### 3. 画像の保存

1.xx 版の `CaptureUtil.auf` は TVTest 付属の `TVTest_Image.dll` を使って
いましたが、本版は Windows 標準の **WIC (Windows Imaging Component)** を
使うため追加の DLL は不要です。png / jpeg / bmp / tiff に対応します。

画像は `EDIT_HANDLE::rendering_scene_video()` で現在のフレームを
レンダリングして取得します (フィルタ効果も反映されます)。

### 4. デコーダの 64bit 化

`TVTestSrc.aui` の中身である m2v (MPEG-2 VIDEO VFAPI Plug-In) に対して
以下の変更を行っています (`tools/patch64.py` に集約)。

- ポインタを `int` に入れていた箇所を `intptr_t` に変更
  (`shared_memory.c` / `multi_file.c` / `video_stream.*` / `audio_stream.*` /
  `transport_stream.*` / `program_stream.*`)
- `resize.c` のポインタ配列確保サイズ (`sizeof(int)` → `sizeof(int *)`)。
  直っていないと `int **` の配列を必要量の半分しか確保しません
- `audio_stream.c` の巨大なスタック上のバッファ (約 600KB / 256KB) を
  `malloc` に変更。AviUtl2 が `func_open()` を呼ぶスレッドのスタックが
  1MB しかない事があり、そのままではスタックオーバーフロー
  (`0xC00000FD`) でプロセスごと落ちます
- `GWL_USERDATA` → `GWLP_USERDATA` (`gl_dialog.c`)
- MSVC のインラインアセンブラ (`__asm { emms }`) を削除
- **元々あった MMX/SSE/SSE2 のアセンブラルーチンは使用しない**。
  32bit x86 のアセンブラ (`*.asm`) と MSVC のインラインアセンブラで
  書かれており x64 ではビルド出来ない為です。
  `get_simd_mode()` が常に 0 を返すようにし、`mpeg_video.c` の
  関数テーブルでは C 実装が選ばれます。
  呼び出し側のコードは残っている為、リンクを通すためのダミー定義が
  `src/m2v/simd_stub.c` にあります (`tools/gen_simd_stub.sh` が生成)。

> **「x64 で SIMD を使っていない」という意味ではありません。**
> 32bit アセンブラを移植するのではなく、必要な所を intrinsics で
> 書き直す方針です。現在は `resize.c` の `component_resize()` が
> SSE2 を使っています (下記「デコード速度の改善」)。
> 一番重い処理はそちらなので、`simd_stub.c` の足場は x64 では
> 使われないまま残っています。
>
> 足場を消さずに残しているのは、次の最適化候補
> (`yuv422_to_yuy2` / `chroma420i_to_422`) が
> `setup_convert_function()` / `setup_chroma_upsampling_function()` の
> 分岐先そのもので、実装を差し替える場所として使える為です。

> 1.xx 版は SIMD が効いていた為、単純な比較ではまだ不利です。
> ただし下記「デコード速度の改善」で最大のボトルネックは潰してあります。
> それでもシークが重い場合は `TSMemory-TVTestSrc.ini` (AviUtl2 側) の
> `aspect_ratio=0` を試してください。

#### デコード速度の改善 (resize.c)

**当初は IDCT と動き補償が重いと考えていましたが、測ったら違いました。**
`tests/tools/m2v-profile.sh` でサンプリングした結果:

```
component_resize                      73.1%   ← アスペクト比補正のリサイズ
yuv422_to_yuy2                         4.8%
chroma420i_to_422                      4.7%
idct_ap922_row                         3.5%
add_block_data_to_frame                1.2%
prediction_*                           3.8%  (合計)
```

`resize.c` の**横方向リサンプル 1 関数で 7 割超**でした。
IDCT と動き補償を合わせても 1 割に届かず、そこを SIMD 化しても
全体では数 % にしかなりません。対象を `component_resize` に変更しました。

元の実装は 1 出力画素・1 タップ毎に `index[x][i]` / `weight[x][i]` という
`int**` の二段間接参照を行っており、これが支配的でした。

**2 段階で最適化しています** (どちらも `tools/patch64.py`)。

1. **テーブルの平坦化** — 重みと添字を連続した配列に置き直し、
   タップが連続している画素 (端以外は必ず連続) は添字を介さず
   入力を直接連続読みする。Lanczos3 拡大の 6 タップは展開する。
2. **SSE2 化** — 内側は 6 タップの積和なので `_mm_madd_epi16` で処理する。
   重みは `1<<16` スケールの `int32` で `int16` に収まらない為、
   `w = (w >> 8) * 256 + (w & 255)` と上下に分けて 2 回 madd し、
   `(hi << 8) + lo` で合成する。**整数として元と完全に同じ値**になる。
   x64 では SSE2 は常に使えるので CPU 判定は不要。

実測 (1920x1080 / 60 フレーム連続デコード):

| | ms/frame | component_resize の割合 |
| --- | --- | --- |
| 最適化前 | 37.8 | 73.1% |
| 平坦化のみ | 27.4 | 58.1% |
| + SSE2 | **25.3** | 42.0% + 14.3% (SSE2 本体) |

**全体で約 1.5 倍**です。

> **出力はビット単位で一致します。** 加算の順序も精度も変えていません。
> 最適化前のデコード結果を保存しておき、変更後の出力と SHA-256 で
> 比較して確認しました (4 フレーム × 上下反転の有無 = 8 ファイル、全一致)。
> 同じ確認は下記で再現できます。
>
> ```bash
> # 変更前に基準を保存
> ./build/tests/test_decode.exe build/ts-examples/sample.ts build/tests/refout/f dist/TSMemory-TVTestSrc.aux2
> # 変更後に比較
> ./build/tests/test_decode.exe build/ts-examples/sample.ts build/tests/newout/f dist/TSMemory-TVTestSrc.aux2
> ```

> **`tools/patch64.py` の `patch()` を冪等にしました。**
> 従来は「置換前の文字列」を先に探していた為、行を追加するだけの置換
> (置換前が置換後の一部になる形) では、実行の度に重複適用されていました。
> 「置換後の文字列が既にあるか」を先に見るように直しています。

#### 適用済みの判定 (`marker`)

「置換後があるか」だけで判断すると、**逆向きに壊れます**。
実際に 3 通りの失敗が起きていたので、判定方法を選べるようにしました。

| `subs` の書き方 | 判定 | 使う場面 |
| --- | --- | --- |
| `(old, new)` | `new` があれば適用済み | 既定 |
| `(old, new, "目印")` | その 1 行があれば適用済み | **後続のパッチが同じ範囲を更に書き換える**場合。`new` が原形で残らず、偽の `!! not found` が出る |
| `(old, new, BY_OLD)` | `old` があれば未適用 | **`new` が元のソースの別の箇所に既にある**場合。既定だと初回から「適用済み」と誤判定され、警告も出ないまま黙って飛ばされる |

実際に見つかった不具合:

- `resize.c` の平坦化テーブル — 後続の SSE2 化が同じ範囲を書き換える為、
  毎回 `!! not found` が出ていた (**誤検知**。効果自体は入っていた)
- `multi_file.c` の `try_next()` — 宣言を先に当てると、定義側の `new`
  (`static intptr_t try_next(char *path)`) が宣言の中に部分文字列として
  現れる為、定義が**黙って飛ばされて**いた。
  pristine から再生成すると `conflicting types` でビルドが通らない状態
- `resize.c` の `sizeof(int)` → `sizeof(int *)` — 正しい形が元のソースの
  別の 2 箇所に既にある為、**バグのある 1 箇所だけが直らないまま**だった。
  `int **` の配列を必要量の半分しか確保しない (ヒープの破壊)

> **コミット済みの `src/m2v/` にも重複が残っていました。**
> 冪等でなかった頃の実行が積み上がった物で、
> `#include <stdint.h>` が最大 13 個、`gl_dialog.c` の DLL 名リストが
> 10 組、そして `audio_stream.c` の `free(buffer);` が **8 個連続**
> (= 7 回の二重解放) という状態でした。
> pristine から再生成した物に差し替えてあります。

**pristine から再生成して確認する事**が唯一の確実な検証です。

```bash
bash tools/setup-tsmemory-src.sh   # 原典を third_party/TSMemory に取得
bash tools/regen-m2v.sh            # 戻して当て直し、simd_stub.c も作り直す
bash tools/build.sh && bash tests/tools/test.sh
```

`regen-m2v.sh` は `!! not found` が 1 つでも出たら**失敗で終わります**。
黙って当たっていない状態で先へ進まない為です。

実測 (2026-08-24): 原典 (`dtvgit/TSMemory` master `59f0fe0`) から
作り直した結果は、commit 済みの `src/m2v/` と**バイト単位で一致**しました
(`diff -rq` で差分なし)。

> **`core.autocrlf` に注意。** この原典は殆どが CRLF ですが
> `idct_reference.c` / `instance_manager.c` / `plugin.cpp` の 3 つだけ
> LF です。Windows の既定 (`core.autocrlf=true`) で clone すると
> この 3 つが CRLF に変換され、作り直した結果が全行差分になります。
> `setup-tsmemory-src.sh` は `-c core.autocrlf=false` を強制します。


### 5. マルチ編成 (サブチャンネル) 対応

1.xx 版では、マルチ編成の局でサブチャンネル (MX2 など) を視聴していても
**プライマリチャンネルの映像しか取り込めません**でした。本版では
視聴中のチャンネルの映像が取り込まれます。

原因は、TS を切り出す `CTsSelector` に**サービスID 0** を渡していた事です。
`CTsSelector` はサービスID 0 を「全サービス」の意味として扱う為、
全チャンネルの映像 PID がそのまま残ります。それをデコーダに渡すと、
デコーダは PAT に最初に出てくる映像 (＝プライマリチャンネル) を拾います。

本版では TVTest から視聴中のサービスIDを取得し、
`SetTargetServiceID(視聴中のサービスID, STREAM_MPEG2VIDEO)` を設定します。
更新するタイミングは以下の通りで、切り替わった時点でリングバッファを
破棄し、前のチャンネルのパケットが混ざらないようにしています。

- プラグイン有効化時
- チャンネル変更 (`EVENT_CHANNELCHANGE`)
- BonDriver 変更 (`EVENT_DRIVERCHANGE`)
- サービス変更 (`EVENT_SERVICECHANGE` / `EVENT_SERVICEUPDATE`)

TVTest のログに `取り込み対象をサービス NNNNN に切り替えました。` と
出るので、意図したチャンネルになっているか確認できます。

> **全チャンネルを同時に取り込んでレイヤーを分ける**方式は採っていません。
> 共有メモリと `.tvtv` をチャンネル数だけ用意する事自体は可能ですが、
> リングバッファがチャンネル数で分割されて 1 チャンネルあたりの
> 取り込み秒数が短くなり、視聴していないチャンネルの為に常時デコードと
> メモリを消費する事になります。実用上ほぼ必要とされない一方で
> 代償が大きい為、視聴中のチャンネルのみを対象としています。

### 6. フィルタのプリセット

AviUtl 1.xx 版は各フィルタの設定を保存しておけた為、動画を開いた時点で
インターレース解除やノイズ除去といったテレビ向けの設定が効いた状態に
出来ました。AviUtl2 は基本の設定しか初期値を持てず、各フィルタは
OFF の状態で追加されます。

その代わり AviUtl2 には**フィルタのプリセット**があるので、
TSMemory が映像を配置した直後にプリセットを適用出来るようにしました。

`TSMemory-TVTestSrc.ini` (AviUtl2 側) にプリセット名を書きます。

```ini
[Bridge]
Preset=キャプチャ用フィルタ
```

プリセットは AviUtl2 の設定ウィンドウのメニューから作成でき、

```
%ProgramData%\aviutl2\Preset\動画ファイル.<プリセット名>.preset
```

に保存されます。`Preset=` にはこの `<プリセット名>` の部分だけを書きます
(前の `動画ファイル.` と後ろの `.preset` は不要です)。

適用されると AviUtl2 のログに次のように出ます。

```
TSMemory: フィルタプリセットを読み込みました (C:\ProgramData\aviutl2\Preset\動画ファイル.キャプチャ用フィルタ.preset)
TSMemory: プリセットを適用しました (エフェクト 5 件 / 設定 12 件)
```

動作は次の通りです。

- プリセットは `[Effect.N]` の並びなので、書かれている順にエフェクトを
  組み立てます (`create_effect()` + `set_effect_item_value()`)。
- `動画ファイル` や `映像再生` の様に**既にあるエフェクトは作り直さず**、
  設定値だけを反映します。同名のエフェクトが複数ある場合は出現順に対応します。
- **取り込んだ `.tvtv` のパスは上書きしません。** プリセットにはそれを作った
  時に開いていたファイルのパスが残っている為、既存エフェクトの `ファイル`
  項目だけは読み飛ばします。
- `effect.disable=1` のエフェクトは無効の状態で追加されます
  (普段は切っておき、必要な時だけ ON にする、という使い方が出来ます)。
- 適用出来なかった項目があった場合は件数と最初の 1 件をログに出します。
  プリセットを別のオブジェクト種別向けに作った場合などに起きます。

> **設定ファイルの文字コードについて**
> `TSMemory-TVTestSrc.ini` は UTF-8 です。Windows の `GetPrivateProfileString()` は
> BOM の無いファイルを ANSI (日本語環境では CP932) として読む為、
> そのままではプリセット名の様な日本語の値が文字化けします。
> その為、日本語が入り得る設定 (`Preset` / `PresetFile` /
> `[Capture] FileName` / `ExitConfirmText` / `ExitConfirmButton`) は
> UTF-8 対応の読み出し (`src/aviutl2/inifile.cpp`) を通しています。
> UTF-16 や CP932 で保存された設定ファイルもそのまま読めます。

### 7. 配置先レイヤーのロック (`LockLayer`)

`LockLayer=1` にすると、取り込んだ後に `set_layer_lock()` で配置先レイヤーを
ロックし、プレビュー画面のドラッグでオブジェクトを動かしてしまう事故を防ぎます。
次の取り込みでは配置の前に自動で解除します
(`LockLayer=0` の間は、手で掛けたロックを勝手に外す事はしません)。

**AviUtl2 のオブジェクトリストにある「プレビュー編集の操作をロック」は
オブジェクト単位のロックで、プラグイン API には公開されていません。**

`sdk/aviutl2/plugin2.h` が公開しているロックは 2 つだけです。

| API | 対象 |
| --- | --- |
| `get_layer_lock` / `set_layer_lock` | レイヤー |
| `get_effect_lock` / `set_effect_lock` | エフェクト |

一方 `aviutl2.exe` の中には、この 2 つとは別に
オブジェクト単位のコマンドがあります。

```
Edit::EditCommand::setLockObject     "set lock object num=%d lock=%d"
Edit::EditCommand::setLockEffect     "set lock effect [%s] num=%d enable=%d"
```

「プレビュー編集の操作をロック」という文言は、`ObjectListComponent` の
UI 文字列の並び (`オブジェクトの表示・非表示` の隣) にあります。
つまりオブジェクトリストの行に付く前者で、SDK からは触れません。

> **エイリアス経由という手はあります (採用せず)。**
> エイリアス／exo のパーサには `effect.disable` と並んで
> `display.lock` `display.hide` というキーがあります。
> `get_object_alias()` で取り出して `display.lock=1` を足し、
> `delete_object()` してから `create_object_from_alias()` で作り直せば
> オブジェクト単位のロックに届く可能性があります。
> ただしオブジェクトを作り直す＝`.tvtv` を開き直す事になり、
> GOP リストの再作成 (数秒) が毎回入ります。
> 取り込みが目に見えて遅くなる為、採用していません。
> なお `display.lock` がオブジェクト単位のロックである事自体、
> 文字列の並びからの推定で**未検証**です。

TSMemory は専用レイヤーに置く前提 (`Layer` / `ReplaceLayer`) な為、
レイヤー単位のロックで実用上は同じ効果になります。

### 8. 作業ファイルの置き場所

AviUtl2 に渡すダミーファイル (`tsmemoryN_M.tvtv`) は、以前は自分の DLL の
隣 — つまり **TVTest の `Plugins` フォルダ**に作っていました。
そこが `Program Files` 配下だと書けず、しかも書けなかった事が判らない
まま「映像データがまだ溜まっていません」とだけ出る状態でした。

書ける場所を選ぶようにしています。

1. `%ProgramData%\aviutl2\temp`
2. `%AppData%\aviutl2\temp` (利用者ごと。必ず書ける)
3. 従来どおり DLL の隣

フォルダを作れても書けるとは限りません (`%ProgramData%` の下は既定で
「作成は出来るが他人が作った物には書けない」)。その為、作成した後に
`FILE_FLAG_DELETE_ON_CLOSE` で試し書きをして確かめています。
選んだ場所は起動時にログへ出します。

`CreateFile()` に失敗した場合の理由もログに残すようにしました。

前回 TVTest が異常終了すると自分の番号のダミーファイルが残る為
(消すのは `Finalize()` だけ)、起動時に**自分の番号の残骸だけ**を
消します。ミューテックスを取れた番号は自分の物なので安全です。

### 9. 音声 (既定は無効)

音声は**両側で有効にした時だけ**扱います。

| 側 | 設定 |
| --- | --- |
| TVTest | `[Settings] Audio=1` — `CTsSelector` に AAC の PID も残させる |
| AviUtl2 | `[M2V] audio=1` — 音声を開き、`FLAG_AUDIO` を申告する |

申告だけして中身が無いと AviUtl2 側に空の音声トラックが出来る為、
無効時は `INPUT_PLUGIN_TABLE` からも `FLAG_AUDIO` を落とします。

#### 片方だけ設定した場合

**`[M2V] audio=1` だけ入れて TVTest 側が `Audio=0` のまま**というのが
いちばん起きやすい間違いです。この時に何が起きるかを実測しました。

| | |
| --- | --- |
| 音声 | 出ません |
| 一時的なメモリ | 共有メモリ全体 (= `MemorySize` 相当) を 1 部余分に複製します |
| 時間 | 約 1ms/MB を無駄に使います (既定 10MB なら 10ms 程度) |
| 空の音声トラック | 出来ません (`INPUT_INFO::FLAG_AUDIO` は立たない) |

無駄が生じるのは、**`CTsSelector` が PMT を書き換えない**為です。
音声 PID のパケットは落とされているのに PMT には載ったままなので、
AviUtl2 側は音声 PID を見つけて走査し、**走査し終わってから**
「1 つも無い」と判ります。

> **理由をログに出します。** 以前は `CTvtvAudio` が理由を記録するだけで
> どこからも読まれず、利用者は「なぜ音が出ないのか」が判りませんでした。
>
> ```
> TSMemory: 音声が見つかりません。TVTest 側の TSMemory.ini で
> [Settings] Audio=1 になっているか確認してください
> (両方を 1 にする必要があります)
> ```
>
> 成功時も 1 行出します。設定が効いた事と A/V の時間差が確認できます。
>
> ```
> TSMemory: 音声を取り込みました (2 ch / 48000 Hz / 11.94 秒 / 映像との差 +0.750 秒)
> ```
>
> ロガーは `TSMemoryLog()` / `TSMemoryLogWarn()` (`plugin_main.h`) で
> 入力プラグインからも使えるようにしてあります。

#### 既存のコードと分けてある

**音声は `src/aviutl2/audio/` に閉じています。**
入力プラグインが見るのは `tvtv_audio.h` の 1 つだけで、
無効時は `CTvtvAudio` を作らないので音声側は一切動きません。

| ファイル | 役割 |
| --- | --- |
| `audio/ts_audio.*` | 共有メモリの TS から AAC (ADTS) を取り出し、フレーム索引と PTS を作る |
| `audio/aac_decoder.*` | Media Foundation の AAC デコーダで PCM に直す |
| `audio/tvtv_audio.*` | 上 2 つをまとめ、映像との時間差を詰めて任意位置の読み出しに応える |

m2v には一切依存していません。**m2v の音声デコーダは使えません** —
Program Stream 専用・実ファイル前提・MPEG-1/2 Layer II 専用の 3 つが
独立に効いていて、日本の地上波 (AAC) は原理的に扱えません
([audio-support.md](audio-support.md) の 2-2)。

#### 開いた時に全部復号する

都度復号にすると、任意位置を要求してくる `func_read_audio()` の度に
MFT を flush して数フレーム手前から復号し直す必要があります。
そこが遅いと操作感に響く為、**開いた時にまとめて復号して PCM を持ちます**。
読み出しは memcpy になります。

取り込みは `MemorySize` の範囲なので、48kHz ステレオで 1 分あたり
およそ 11MB です。

確保量は**固定値ではなく、demux が数えた ADTS のフレーム数から決めます**。
取り込んだ映像と同じ時間ぶんしか音声は無いので、これが本来の必要量です
(実測でも復号結果はフレーム数と完全に一致しました)。
固定値で頭打ちにすると、長い取り込みで**音声だけ途中で切れます**。

最後の歯止めとして 1G 要素 (= 2GB) を置いてありますが、普段はここに
当たりません。壊れた入力で ADTS の数え上げが暴れた時だけの保険です
(ADTS は 1 フレーム最小 7 バイトで 1024 サンプルを名乗れる為、
同期語だらけの ES を食わせると実際の内容より桁違いに大きくなります)。

#### Media Foundation で詰まった所

実測で判った事が 2 つあります。どちらも「そう書いてある通りにやると
動かない」類です。

- **`CLSID_CMSAACDecMFT` を直接 `CoCreateInstance` すると
  `REGDB_E_CLASSNOTREG` (0x80040154)** になります。
  `MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, ...)` で
  「AAC を入力に取れる音声デコーダ」を問い合わせるのが正解です
- **入力メディアタイプを自分で組み立てると
  `MF_E_INVALIDMEDIATYPE` (0xC00D36B4)** で弾かれます。
  `MF_MT_USER_DATA` に何を入れるかが決め打ち出来ない為で、
  この環境では 12 バイト要求されました。
  `GetInputAvailableType()` が返す型のうち
  `MF_MT_AAC_PAYLOAD_TYPE == 1` (ADTS) の物を土台にして、
  チャンネル数とサンプリング周波数だけ差し替えます

加えて、`ProcessOutput()` は `MF_E_TRANSFORM_STREAM_CHANGE` を
返してくるので、出力型を選び直して続ける必要があります。

#### 走査は 1 周にまとめてある

音声の ES と、A/V 同期に使う映像の開始 PTS は、**同じパケット列を
1 周する間に両方拾います**。以前は別々に 2 周していました。

映像の方は「最初のシーケンスヘッダ (`00 00 01 B3`)」が見つかれば用済みな為、
PES が 1 つ出来る度に探して、見つかった時点で収集をやめます。
見つからないまま 4MB を超えた場合も諦めます。

PES を繋いで ES にする処理は音声と映像で同じなので `CPesAssembler` に
まとめてあります (以前は同じ Flush の実装が 2 箇所にありました)。

> **速度目当ての変更ではありません。** 実測ではデマルチプレクサ全体で
> **21MB あたり 15ms** しか掛かっておらず、走査は元々ボトルネック
> ではありませんでした (`test_adts` が毎回この値を出します)。
> 重いのは共有メモリの複製の方で、m2v と音声側でそれぞれ 1 部ずつ、
> 計 2 部の複製が発生します。
>
> 複製をやめて共有メモリ上で直接解析する事も出来ますが、
> **排他を握ったまま解析する事になります**。TVTest 側の
> `InputMedia()` は 2 秒待って取れないと**恒久的に諦める**作りなので
> (`CMutexLock::Lock()`)、`MemorySize` を大きくした環境では
> かえって危険です。複製のままにしてあります。

#### A/V 同期

リングバッファは GOP の途中で切れるので、映像と音声で開始位置が違います。
**m2v は TS から ES に落とす際に PTS を捨てている**ので、映像フレーム 0 の
時刻を m2v から取る手段がありません。

その為、音声の demux のついでに映像 PID も走査し、
**最初のシーケンスヘッダ (`00 00 01 B3`) を含む PES の PTS** を
映像の開始時刻として拾っています (m2v も最初の GOP から復号を始める為)。

```
offset = (映像の開始PTS - 音声の開始PTS) / 90000
  > 0 … 音声が先に始まっている -> 音声の先頭を捨てる
  < 0 … 映像が先に始まっている -> 音声の先頭に無音を詰める
```

実際の放送 TS (TOKYO MX のフル TS。再配布出来ない為リポジトリには無い) での実測は **+0.750 秒**で、
609,280 サンプルのうち 36,022 サンプルを捨てて 573,258 になりました。
PTS は 33bit で折り返す為、差を取る前に折り返しを処理しています。

#### 確認済みの事

**実機で音声が出る事を確認済みです (2026-08-24)。**
TVTest から取り込んで AviUtl2 のタイムラインに音声が乗り、
映像と合って聞こえる所まで見ています。

自動テストでは `tests/test_adts.cpp` / `test_aac_decode.cpp` /
`test_audio.cpp` と `test_decode ([M2V] audio=1)` で下記を実測しています。

- 実 TS から AAC-LC / 48000Hz / 2ch を取り出せる事
- ADTS の索引が連続し、1 フレーム 1024 サンプルで進む事
- Media Foundation が復号し、**無音でない** PCM が出る事
  (長さが ADTS のフレーム数と 0.2 秒以内で一致する)
- 時間差の分だけサンプル数が詰まっている事
- 任意位置の読み出しと、範囲外の無音埋め
- `[Settings] Audio` で音声 PID が残る / 落ちる事 (`test_selector`)
- 壊れた TS を食わせても落ちない事 (`test_fuzz` を audio=1 で実行)

> **既定を `0` にしてあるのは、動かないからではありません。**
> 主な用途が静止画キャプチャで音声を必要としない事と、
> 音声 PID の分だけリングバッファを食う (同じ `MemorySize` なら
> 遡れる時間が短くなる) 為です。

### 10. 終了時のクラッシュ (アンロード後の参照)

**AviUtl2 は終了時にプラグインを `FreeLibrary` した後も、登録済みの
ポインタを参照し続けます。**

デバッガ配下で AviUtl2 を起動し、取り込み要求を送ってから閉じた時の記録:

```
[unload] TSMemory-TVTestSrc.aux2                      ← アンロード
[exception] 0xC0000005 addr=00007FFF59BB1BD0
    -> どのモジュール範囲にも該当せず (解放済みメモリ)  ← アンロード済み領域を実行
[exception] 0xC0000005 KernelBase+0x59C7D
    read from address 00007FFF59BFE36E                 ← 同じ領域から文字列を読む
[exit] code=0x000000FF
```

2 つのアドレスはいずれもアンロードされた `.aux2` のイメージ範囲内でした。
`register_input_plugin()` に渡す `INPUT_PLUGIN_TABLE` は、関数ポインタも
`name` / `filefilter` / `information` の文字列も全て DLL の中にあります。
SDK には**登録を解除する API がありません**。

その為、`RegisterPlugin()` の先頭で
`GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN)` により
**自分自身をピン留めして、アンロードされないようにしています**
(`PinSelfModule()`)。

> 副作用として `.aux2` のファイルが掴まれたままになります。
> パッケージの入れ直しは AviUtl2 を終了してから行ってください
> (README に元から記載しています)。

> **ピン留めで別の不具合が表面化しました。**
> DLL が残るようになった結果、AviUtl2 が `UninitializePlugin()` の
> **後にも** `func_close()` を呼んでくる事が判りました。
> `UninitializePlugin()` で破棄済みのハンドルをもう一度 `delete` すると
> 二重解放になり、終了時にヒープ破損 (`0xC0000374`) で落ちます。
> `func_close()` は**管理リストから外せた時だけ破棄する**ようにして、
> どちらの順序で呼ばれても一度しか解放しないようにしています。

> **TVTest 側にも同じ形がありました。**
> `EVENT_COMMAND` は AviUtl2 の起動待ちで TVTest を止めない為に
> ワーカースレッドで処理しますが、この待ちは既定で 30 秒 (`LaunchWait`)
> あります。一方 `Finalize()` はワーカーを 5 秒しか待たずに先へ進み、
> スナップショットの削除・共有メモリの解放・ミューテックスの破棄を
> 行っていました。**その後 TVTest はプラグインを delete して DLL を
> アンロードする**ので、起動待ち中に TVTest を終了すると、生きている
> ワーカーが無効になった `this` / `m_pApp` / アンロード済みのコードを
> 触る事になります。
>
> `m_fShutdown` を立ててから join するようにしました。待ちループが
> 250ms 毎に見るので、通常はすぐ降りてきます。
> それでも終わらない場合 (要求の結果をメッセージボックスで出していて
> ユーザーが閉じていない等) は**後始末を見送ります**。
> ここで `WaitForSingleObject(INFINITE)` にすると、そのメッセージ
> ボックスが閉じられるまで TVTest の終了が固まる為です。

> **同じ「終了と非同期処理の競合」がキャプチャ側にもありました。**
> `rendering_scene_video()` は非同期で、完了時に別スレッドから
> `OnRenderingVideo()` が呼ばれます。以前は
> `TSMemoryCaptureUninitialize()` が `DeleteCriticalSection()` していた為、
> 「`fLockInitialized` の判定を通った直後に CS が破棄され、
> 破棄済みの CS へ `EnterCriticalSection()` する」という競合が残っていました。
> **CRITICAL_SECTION は破棄せずプロセス終了まで残し**、
> 後始末は CS の中の `fShutdown` で示すようにしています
> (保存要求が飛んでいる最中に AviUtl2 を終了した場合に成立します)。
>
> **入力プラグイン側 (`input_tvtv.cpp`) も同じ形にしてあります。**
> `g_HandleLock` を破棄せず `g_fShutdown` で示します。
> 加えて、後始末の後に `func_open()` が呼ばれた場合は
> **開かずに `nullptr` を返します**。プラグインは固定されて残る為
> AviUtl2 はいつでも呼べる状態のままで、ここで開くと管理リストに
> 載らないままデコードスレッドだけが残ってしまう為です
> (`tests/test_decode.cpp` で確認しています)。
>
> `TSMemoryBridgeStop()` も同様で、受信スレッドを待ち切れなかった場合に
> 共有メモリと同期オブジェクトを解放すると、生きているスレッドが
> 解放済みの領域に触ります。**タイムアウトした時は解放を見送り**、
> OS のプロセス終了時の回収に任せます。

回帰確認は `bash tests/tools/test-exit.sh <aviutl2.exe のパス>` で行えます。
AviUtl2 を実際に起動し、`tests/tvtest_sim.cpp` が TVTest を模擬して
取り込み要求を送り、閉じた時の終了コードを見ます (TVTest は不要)。

---

## ビルド

システムにインストールされたコンパイラは使いません。
ツールチェインはすべてプロジェクト内の `compilers/` に展開して使います。

### 実行環境

スクリプトは bash で書かれていますが、**Windows 上で動かす前提**です。
llvm-mingw は Windows 版 (`clang.exe`) を取得し、`py` (Windows の Python
ランチャー) も使う為、**WSL ではなく Git Bash または MSYS2** で実行します。

| 必要な物 | 用途 |
| --- | --- |
| Git Bash (Git for Windows 同梱) | シェル。`curl` も同梱されている |
| `unzip` | ツールチェインの展開。無い場合は PowerShell の `Expand-Archive` に自動で切り替わる |
| Python 3 (`py` ランチャー) | **任意**。`zipdir.py` によるパッケージ作成のみに使う。無ければ zip を飛ばして続行する。`patch64.py` や `build-tvtest.py` を使う場合は必要 |

プラグイン本体のビルド (`.aux2` / `.tvtp`) と `tests/tools/test.sh` は
Python を使いません。

Python の場所を明示したい場合は `TSMEMORY_PYTHON` を使います
(環境変数 `PYTHON` は見ません。他の用途で設定されている事がある為)。

```bash
TSMEMORY_PYTHON="C:/Python312/python.exe" bash tools/build.sh
```

### プラグイン本体

llvm-mingw (clang + lld + mingw-w64) を使います。

```bash
bash tools/setup-toolchain.sh
bash tools/build.sh
bash tests/tools/test.sh
```

生成物は `dist/` に出力されます。

```
dist/TSMemory.tvtp          TVTest プラグイン (x64)
dist/TSMemory-TVTestSrc.aux2          AviUtl ExEdit2 汎用プラグイン (x64)
dist/TSMemory-TVTestSrc.au2pkg.zip    AviUtl2 用パッケージ (プレビュー画面に D&D)
dist/TSMemory-x64.zip       配置先ごとに整理した配布用アーカイブ
```

`src/m2v` を原典から作り直す場合は、`tools/regen-m2v.sh` が
64bit 化パッチとダミー定義の生成までまとめて行います。

```bash
bash tools/setup-tsmemory-src.sh
bash tools/regen-m2v.sh
```

### TVTest 本体 (64bit)

TVTest は DirectShow BaseClasses など MSVC 前提のコードを多く含むため、
MSVC が必要です。Visual Studio をインストールする代わりに、Microsoft の
公式 CDN から MSVC と Windows SDK をプロジェクト内に展開して使います
(インストーラもレジストリ変更もありません)。

```bash
bash tools/setup-tvtest-src.sh   # 本家 develop を submodule ごと clone
bash tests/tools/setup-msvc.sh   # ※ ライセンス条項の URL が表示され Y/N を聞かれます
compilers/python/python.exe tests/tools/build-tvtest.py
```

生成物は `dist/TVTest-x64/` に出力されます。

ポータブル MSVC には MSBuild が含まれないため、`.vcxproj` を読んで
`cl` / `rc` / `mt` / `lib` / `link` を直接叩く簡易ビルダ (`tests/tools/vcxproj.py`)
を用意しています。MSBuild の完全な代替ではなく、TVTest / LibISDB の
プロジェクトが使っている範囲だけに対応しています。

> **ソースは本家から取得します。** `tools/setup-tvtest-src.sh` が
> <https://github.com/DBCTRADO/TVTest> の **develop** ブランチを
> submodule (LibISDB、更にその中の fdk-aac) ごと clone します。
> 原典を手で置く必要はありません。
>
> LibISDB は submodule のポインタが指すコミットが使われます
> (TVTest の master 相当では TVTest 0.10.0-dev が要求する API が
>  足りない為、以前は develop の特定コミットを手で固定していました。
>  submodule 経由なら本家が指す版がそのまま入ります)。
>
> 別の版を使いたい場合は `TVTEST_REF` で指定できます。
>
> ```bash
> TVTEST_REF=v0.10.0 bash tools/setup-tvtest-src.sh
> ```

### BonTsEngine と LibISDB (移行の可否を調べた記録)

`src/tvtp/BonTsEngine/` は **TVTest 本家から 2017-09-30 に削除された系統**です。

```
9352e7e8  2017-09-30  BonTsEngine を LibISDB に置き換え
```

由来を辿ると、TVTest から直接ではなく**原典の TSMemory 経由**でした。
`third_party/TSMemory/TSMemory/BonTsEngine/` と突き合わせると、文字コード
(cp932 → utf-8) と改行を揃えた後で **24/24 ファイルが一致**します
(本リポジトリが加えた変更は無く、`LICENSE.txt` を 1 つ足しただけ)。

**上流の修正が流れて来る事はもうありません。** 移植上の傷はこちらの
判断で直す事になります (下記の `<Vector>` がその例)。

#### 移行は可能。ただし今はしない

LibISDB の `StreamSelector` (`ServiceSelectorFilter` の実体) で
置き換えられるかを実際に試しました。**ビルドし、実 TS を通すところまで
動きました。**

```
multi.ts    service 23608 : out=54601  PIDs: 0x0000 0x0100 0x0111 0x0112
            service 23610 : out=54611  PIDs: 0x0000 0x0200 0x0131 0x0132
isdbt188.ts service 23664 : out=699678 (ARIB の記述子を含む実放送)
```

API の対応も素直で、むしろ構造は単純になります。

| 現行 | LibISDB |
| --- | --- |
| `CTsSelector::SetTargetServiceID(id, streams)` | `StreamSelector::SetTarget(id, StreamFlag)` |
| `SetOutputDecoder(this)` + `CMediaDecoder` 継承 | **不要**。`InputPacket()` が戻り値で返す |
| `CTsPacket::ParsePacket(cc)` / `EC_FORMAT` | `TSPacket::ParsePacket(cc)` / `ParseResult::OK` |
| `STREAM_MPEG2VIDEO` / `STREAM_AAC` | `StreamFlag::MPEG2Video` / `StreamFlag::AAC` |

**代償 1 — llvm-mingw 用の補完層が要る。** LibISDB は MSVC 前提で、
そのままではビルド出来ません。原因は全て特定済みで、LibISDB 本体に
手を入れずに shim だけで通せます。

| 問題 | 対処 |
| --- | --- |
| `std::endian` を使う (C++20 必須) | `-std=c++17` → `-std=c++20` |
| `std::basic_string<uint8_t>` (ARIBString)。新しい libc++ は `char_traits` の一次テンプレートを持たない | `char_traits<unsigned char>` を特殊化 |
| mingw の `winnt.h` が `RotateLeft32` 等をマクロ定義し同名関数と衝突 | `#undef` |
| `wcscasecmp` / `wcsncasecmp` が mingw に無い | `_wcsicmp` へのラッパ |
| `FileStreamGenericC` が `_MSC_VER` 判定でワイド文字列を `std::fopen` に渡す | `_wfopen` の多重定義 |

**代償 2 — 規模が 6 倍。**

| | ファイル数 | オブジェクト合計 |
| --- | --- | --- |
| 現行 BonTsEngine | 11 | 220KB |
| LibISDB (必要分) | 45 | 1.3MB |

`StreamSelector` は `PSITable` → `Tables` → `DescriptorBlock` →
`Descriptors` → `ARIBString` と芋づるで引き込み、EPG の記述子や
ARIB 文字列デコードまでリンクされます。使うのは「サービスで絞る」
だけなので、大半は使いません。

**結論: 移行しない。** ライセンスは双方 GPL-2.0-or-later で変わらず、
現状の用途では機能も変わりません。動いているコードを 6 倍の依存と
新しい shim 層と引き換えに置き換える理由が今はありません。

価値が出るのは**字幕か H.265 に手を付ける時**です
(`StreamFlag` に `Caption` / `H265` があり、`CaptionParser` 等も使う事に
なる為、依存の大きさが無駄でなくなります)。

### 版の管理

**`CHANGELOG.md` の一番上の `## ver.X.Y.Z` が唯一の正**です。
`tools/build.sh` がそこから読み取り、下記に流し込みます。

| 行き先 | 使われ方 |
| --- | --- |
| `build/generated/tsmemory_version.h` | `src/aviutl2/plugin_main.cpp` が読む。AviUtl2 のプラグイン一覧に出る |
| `res/package.ini` の `@VERSION@` | AviUtl2 のインストール時の表示 |
| `res/package.txt` の `@VERSION@` | パッケージの説明 |

**リリース時に書き換えるのは `CHANGELOG.md` だけです。**
以前は 5 箇所 (README の見出し・更新履歴、`package.ini`、`package.txt`、
`plugin_main.cpp`) に直書きしており、どれか 1 つが古いまま残る形でした。

`README.md` には版を書きません。リポジトリでは git のタグが版の所在で、
見出しの版は更新し忘れても誰も気付かない為です
(オリジナルの `TSMemory.txt` が版を持っていたのは、zip に同梱される
単独のテキストで他に版の在り処が無かった為)。

実測 (`CHANGELOG.md` の見出しだけを `0.9.9` に変えてビルド):

```
version: 0.9.9  (CHANGELOG.md)
  aux2        : TSMemory version 0.9.9 (64bit / AviUtl ExEdit2)
  package.ini : information=TSMemory-TVTestSrc ver.0.9.9 (64bit / AviUtl ExEdit2)
  package.txt : TSMemory-TVTestSrc ver.0.9.9 (64bit / AviUtl ExEdit2 対応版)
```

見出しが読み取れない場合はビルドを失敗させます (黙って版無しで進まない)。

> `docs/` 中の「ver.0.3.0 で〜を変えた」という記述は、**いつ変わったかの
> 記録**なので直書きのままで構いません。生成の対象にはしません。

### VS Code の IntelliSense

`tools/build.sh` は **実際に使ったコンパイル引数**を
`build/compile_commands.json` に書き出します (63 件)。
`.vscode/c_cpp_properties.json` がそれを参照します。

これが無いと、`src/tvtp/BonTsEngine/` で

```
識別子 "max" が定義されていません   C/C++(20)
```

が出ます。`max` / `min` は MSVC の `<windows.h>` が C++ でも定義する
マクロで、mingw-w64 は C の時しか定義しません。その差を
`src/tvtp/msvc_compat.h` で埋め、ビルド時に `-include` で強制的に
入れています。**コンパイラの引数を見ないと分からない**定義なので、
IntelliSense からは見えないのが原因です。ビルドは通ります。

```bash
bash tools/build.sh   # 一度通せば compile_commands.json が出来る
```

クローンした直後でまだビルドしていない場合に備えて、
`c_cpp_properties.json` には `forcedInclude` と `includePath` の控えも
書いてあります。`tests/` のソース (test.sh がその場でコンパイルする為
`compile_commands.json` に載らない) もこちらで解決されます。

> `build/compile_commands.json` は生成物なのでバージョン管理しません
> (`/build/` が `.gitignore` 対象)。管理するのは
> `.vscode/c_cpp_properties.json` だけです。

### Linux でのビルド (クロスコンパイル)

作る物は常に Windows の DLL なので、Linux では**クロスコンパイル**に
なります。`bash tools/build.sh` までは通ります。

```bash
bash tools/setup-toolchain.sh   # Linux 版の llvm-mingw を取得
bash tools/build.sh
```

| | |
| --- | --- |
| `dist/TSMemory.tvtp` / `TSMemory-TVTestSrc.aux2` の生成 | 通る |
| `bash tests/tools/test.sh` | **通らない** (下記) |
| TVTest 本体のビルド | 不可 (MSVC は Windows 専用) |

対応した内容:

- `tools/setup-toolchain.sh` が `uname` を見て配布物を選びます
  (`llvm-mingw-*-ucrt-ubuntu-22.04-x86_64.tar.xz` / aarch64 / macOS)。
- コンパイラは **`x86_64-w64-mingw32-clang` 等の接頭辞つき**に統一しました。
  接頭辞なしの `clang` は、Windows では Windows を狙いますが
  **Linux ではホスト (ELF) を狙ってしまう**為です。
  `compilers/llvm-mingw` が無ければ PATH にある物を使います。
- `src/tvtp/BonTsEngine/TsDescriptor.h` の `#include <Vector>` を
  `<vector>` に直しました。Windows は大文字小文字を区別しないので
  通っていましたが、**Linux では該当ファイルが無くエラー**になります。
  本家では既に消えたコードで上流に取り込まれる先が無い為
  (上記「BonTsEngine と LibISDB」)、ソースを直接直しています。
  併せて `-Wno-nonportable-include-path` も外しました。
  全ソースを走査して、大文字小文字の合わないインクルードが
  **0 件**である事を確認しています。
- `zipdir.py` に使う Python は、Windows では `compilers/python/`、
  **Windows 以外では PATH の `python3`** を使います
  (python.org が可搬な Linux 版を配っていない為)。
- `compile_commands.json` を書く時の `cygpath` は Git Bash にしか無いので、
  無ければパスをそのまま使うようにしてあります。

**テストが通らない理由**は、ビルドした Windows の `.exe` をその場で
実行する為です。Wine が要る上に、音声まわりが Media Foundation に
依存しています (`gen-ts-examples` が AAC エンコーダ、`aac_decoder.cpp` が
AAC デコーダ)。検証は Windows で行ってください。

> Linux でのビルドは**この環境からは検証できていません**
> (Windows 上で開発している為)。分岐の選択と配布物 URL の存在は
> 確認済みですが、実際に通した記録ではありません。

### ディレクトリ構成

```
compilers/     ツールチェイン (setup-*.sh が展開; リポジトリには含めない)
third_party/   原典のソースと外部の本体 (tools/setup-*.sh が取得)
               TVTest / TSMemory (原典) / TvtPlay / aviutl2。
               ※ バージョン管理対象外
sdk/aviutl2/   AviUtl ExEdit2 Plugin SDK のヘッダ (バージョン管理する)
src/common/    TVTest 側と AviUtl2 側で共有する連携定義
src/tvtp/      TVTest プラグイン
src/aviutl2/   AviUtl ExEdit2 汎用プラグイン
src/aviutl2/audio/  音声 (既存のコードと分けてある)
src/m2v/       MPEG-2 デコーダ (64bit 化済み)
res/           設定ファイル・言語ファイル等
licenses/      ライセンス全文 (配布物に同梱する)
tests/         動作確認用のテスト
tests/tools/   テスト・デバッグ用のスクリプト (TS の合成・取得を含む)
tools/         ツールチェイン取得・パッチ・ビルドスクリプト
docs/          開発者向けの文書 (本書と音声対応の調査メモ)
.vscode/       IntelliSense の設定 (compile_commands.json を参照する)
build/         中間生成物。テスト用の TS (build/ts-examples/) もここ
dist/          ビルド結果と配布用パッケージ
```

`build/` `dist/` `compilers/` `third_party/` はバージョン管理対象外です。

### 外から持って来る物

**クローンした直後の状態で、ビルドと全てのテストが通ります。**
手で置く必要がある物はありません。以前は原典や放送 TS を `originals/`
に手で置く必要がありましたが、全て取得スクリプトか合成に置き換えました。

`src/m2v/` は 64bit 化済みのものが commit されているので、通常のビルドに
原典は要りません。

原典のソースはいずれも `third_party/` (`.gitignore` 対象) に clone します。

| 作業 | 取得スクリプト |
| --- | --- |
| TVTest 本体を 64bit ビルドする | `bash tools/setup-tvtest-src.sh` |
| `src/m2v/` を原典から作り直す | `bash tools/setup-tsmemory-src.sh` → `bash tools/regen-m2v.sh` |
| `test-live.sh` 用の TvtPlay / BonDriver_Pipe | `bash tools/setup-tvtplay-src.sh` (ビルドには MSVC が要る) |
| 実機で確認する為の AviUtl ExEdit2 本体 | `bash tools/setup-aviutl2.sh` |

テストが使う TS は合成します。「テスト用の TS」の項を参照してください。

### テスト用の TS

放送された TS は再配布出来ないので、リポジトリには入れません
(**TS はバージョン管理せず、合成する側だけを管理します**)。
`tests/tools/gen-ts-examples.cpp` が `build/ts-examples/` に作ります。
`tests/tools/test.sh` は、そこに `*.ts` が 1 つも無ければ自動で呼びます。

| ファイル | 中身 |
| --- | --- |
| `sample.ts` | 1 サービス。映像 + 音声 |
| `multi.ts` | 2 サービス (23608 / 23610)。マルチ編成の確認用 |

```bash
bash tests/tools/gen-ts-examples.sh              # build/ts-examples に作る
bash tests/tools/gen-ts-examples.sh /tmp/ts 20   # place と秒数を指定
TSMEMORY_TS_DIR=/path/to/ts bash tests/tools/test.sh
```

**`build/ts-examples/` に置いた TS は全て検査対象になります。**
実際の放送 TS を持っている場合はそこに足してください。合成した物と
一緒に、`test_decode` / `test_adts` / `test_aac_decode` / `test_audio` /
`test_fuzz` / `test_multich` が 1 本ずつ通ります
(`test_multich` はサービスが 1 つしかない TS では自分で skip します)。

#### 実物のサンプルを足す

ISDB-T のサンプルストリームが
[erb.jp](https://www.erb.jp/2013/12/28/samplestream/) で公開されています。
**手動実行のみ**の取得スクリプトを用意してあります
(`test.sh` からは呼びません)。

```bash
bash tests/tools/fetch-ts-samples.sh
```

| 取得する物 | 置かれる物 |
| --- | --- |
| `isdbt188.ts` (126MB) | `build/ts-examples/isdbt188.ts` |
| `isdbt192bdav.m2ts` (130MB) | `build/ts-examples/isdbt192bdav.ts` |

192 バイト版 (BDAV) は各パケットの先頭に 4 バイトの
`TP_extra_header` が付いた形なので、`tests/tools/m2ts2ts.py` で外して
188 バイトの TS に直してから置きます。TSMemory が TVTest から受け取る
のは常に 188 バイトのパケットで、192 バイトのまま扱う口が無い為です。
(実測: 711,648 パケット、同期外れ 0)

#### 何を作っているか

| | |
| --- | --- |
| 映像 | MPEG-2 Video 1440x1080 16:9 29.97fps。**イントラの DC 係数だけ**で符号化する。AC を出さないので VLC の表 (Table B-14/15) が要らず、絵は 8x8 のモザイクになる。m2v 側はシーケンス/GOP/ピクチャの解析、IDCT、4:2:0 → YUY2、`resize.c` まで通る |
| 音声 | AAC LC 48kHz ステレオ。Media Foundation の AAC **エンコーダ**に正弦波を食わせ、生の AAC フレームに ADTS ヘッダを付ける (外部ツールを入れずに済ませる為) |
| A/V のずれ | 音声を映像より 0.100 秒先に始める。ずれを詰める処理が実際に動く形にしてある |

合成なので**実放送の癖は入っていません** (可変 GOP、フィールド
picture、TEI、連続性カウンタの飛び、ARIB の記述子、デュアルモノ等)。
そこを見たい場合は実 TS を足してください。

#### シーケンスヘッダを毎ピクチャ置いている理由

m2v の `gop_list.c` には、**シーケンスヘッダより先に I ピクチャを見ると
その場で失敗する**分岐があります (探し続けません)。

```c
if(pic.picture_coding_type == 1){
    if(sc == NULL){
        /* A picture before any sequence header. */
        return NULL;
```

実放送では GOP 毎にしかシーケンスヘッダが無くても、途中で切った時に
最初に来るのは大抵 P/B ピクチャなので、次のシーケンスヘッダまで
読み進んで問題になりません。合成した TS は全て I ピクチャな為、
GOP の途中で切ると必ずここに当たります。

実測: シーケンスヘッダを GOP 毎 (12 フレーム) だけにした場合、
`test_decode ... mid:4` は `func_open()` が失敗しました。
毎ピクチャ置く形にすると通ります (増えるのは 14 バイト/フレーム)。

> **これは m2v 側の弱点でもあります。** 全て I ピクチャの MPEG-2 を
> GOP の途中から渡すと開けません。放送波では起きませんが、
> オールイントラで収録した素材を扱うと当たり得ます。
> 現状は合成側で回避しており、m2v には手を入れていません。

### AviUtl ExEdit2 本体と SDK

本体は `tools/setup-aviutl2.sh` が配布ページ
([spring-fragrance.mints.ne.jp/aviutl/](https://spring-fragrance.mints.ne.jp/aviutl/))
から zip 版を取り、`third_party/aviutl2/` に展開します。zip の名前に版が
入る為、ページを見て最新の `aviutl2_v*.zip` を選びます (インストーラは
使いません)。版を固定する場合は `AVIUTL2_ZIP=aviutl2_v2.1.5.zip` のように
zip 名を渡してください。

SDK のヘッダ (`sdk/aviutl2/`) は**バージョン管理しており**、
`tools/update-aviutl2-sdk.sh` で更新します。**自動では行いません**
(API が変われば移植側の修正が要る為)。

```bash
bash tools/update-aviutl2-sdk.sh           # 差分を見るだけ (既定)
bash tools/update-aviutl2-sdk.sh --apply   # 入れ替える
```

差分は Shift_JIS のヘッダを UTF-8 に直して表示します
(`compilers/python` を使用。ファイル自体は原典のまま入れます)。
入れ替えた後は `bash tools/build.sh && bash tests/tools/test.sh` を通してください。

TS サンプルは環境変数でも指定できます。

```bash
TSMEMORY_TS_SAMPLE=/path/to/sample.ts TSMEMORY_TS_MULTI=/path/to/multi.ts bash tests/tools/test.sh
```

サンプルが無い場合、該当するテストは skip されます
(`test_selector` は合成 TS を使う為、常に走ります)。

`docs/` の内容:

| ファイル | 内容 |
| --- | --- |
| [audio-support.md](audio-support.md) | 音声取り込みの調査結果と作業項目。**実装済み**で、着手前の調査記録として残しています (実装の説明は本書の「9. 音声 (既定は無効)」) |

---

## 動作確認

`bash tests/tools/test.sh` で下記を確認しています。

> **AviUtl2 を終了してから走らせる事。**
> `test_plugin` はプラグインを直接読み込んで待ち受けまで動かす為、
> 既に AviUtl2 が動いていると待ち受けの名前付きミューテックス
> (`TSMEMORY_IPC_READY_MUTEX`) を先に取られていて、
> **8 件が失敗する** (「bridge does NOT accept requests before the
> project is initialized」以下)。設定やコードの不具合ではない。

TS を使うテスト (`test_decode` / `test_adts` / `test_aac_decode` /
`test_audio` / `test_fuzz` / `test_multich`) は、
**`build/ts-examples/` に在る TS を 1 本ずつ**通します。
置いた本数だけスイート数が増えます (「テスト用の TS」を参照)。

実測 (2026-08-25、合成 2 本 + erb.jp のサンプル 2 本 + 実際の録画 3 本):
**66 スイート PASS / 失敗 0**。

音声の 3 つ (`test_adts` / `test_aac_decode` / `test_audio`) は
「9. 音声 (既定は無効)」の「確認済みの事」にまとめてあります。

### tests/test_shm.c

64bit 化した共有メモリ読み出し層 (`shared_memory.c` / `multi_file.c`) に対して、
TSMemory.tvtp が作るのと同じ形の共有メモリを用意し、リングバッファの線形化・
シーク・読み出しが期待通りかを確認します。

### tests/test_plugin.cpp

AviUtl ExEdit2 のふりをして `TSMemory-TVTestSrc.aux2` を読み込み、
入力プラグインの登録から、TVTest 側と同じ手順で要求を送って
`create_object_from_media_file()` が呼ばれるところまでを通します。

### tests/test_decode.cpp

`build/ts-examples/*.ts` を共有メモリに載せて
入力プラグインでデコードさせます。
確認済みの結果 (`isdbt188.ts` = erb.jp の ISDB-T サンプル、2026-08-25):

```
open took 422 ms
video   : 1920x1080  30000/1001 fps  1812 frames  compression=YUY2
frame 0    decoded in  31 ms (4147200 bytes)
frame 30   decoded in  63 ms
frame 906  decoded in 156 ms
frame 1811 decoded in 266 ms
re-reading frame 0 after seeking gives the same image ... ok
```

第 4 引数に `head:10` / `mid:10` / `tail:10` を渡すと、指定 MB だけを
切り出して読み込ませます。TS に含まれる映像の長さ (PTS) とデコード出来た
長さを比べる事で、GOP の切れ端で失われる分を測れます。

| 切り出し方 | PTS 上の長さ | デコード結果 | 欠落 |
| --- | --- | --- | --- |
| `head:10` (末尾だけが GOP の途中) | 11.21 秒 | 11.04 秒 | 0.17 秒 (末尾側) |
| `mid:10` (両端が GOP の途中) | 6.64 秒 | 6.04 秒 | 0.60 秒 (先頭 0.43 + 末尾 0.17) |

※ 上表は当時使っていた実 TS (ts-sample2.ts) での値。
**末尾で失われるのは 0.2 秒程度**です。

erb.jp の `isdbt188.ts` での実測 (2026-08-24):

| 切り出し方 | PTS 上の長さ | デコード結果 | 欠落 |
| --- | --- | --- | --- |
| 全体 | 60.43 秒 | 60.46 秒 | (なし) |
| `head:10` | 4.67 秒 | 4.70 秒 | (なし) |
| `mid:10` | 4.90 秒 | 4.54 秒 | 0.37 秒 |

> **平坦な絵・無音を失敗にしない事。**
> この判定は当初「先頭フレームが平坦でない」「先頭 4096 サンプルが
> 無音でない」としていましたが、**実際の録画は黒画面と無音で始まる事が
> 珍しくありません** (erb.jp のサンプルがまさにそう。先頭と末尾が
> 黒画面 Y=16/UV=128、冒頭の音声も無音)。それでは素材を見ている事に
> なるので、複数のフレーム・複数の位置を見て
> **どこかに絵と音があれば良し**とする形に直しました。

デコード結果は `build/tests/frameN_asis.raw` に RGB24 で書き出されるので、
`tests/tools/raw2png.py` で PNG にして目視確認出来ます。

```bash
compilers/python/python.exe tests/tools/raw2png.py build/tests/frame0_asis.raw 1920 1080 frame0.png 3
```

**画像の向き**: m2v は `biHeight` が正でも画像を上から下の順に書き込みます
(AviUtl 1.xx がそれを前提にしていたため)。実際にデコードした画像でも
そのまま読んだ方が正しい向きでした。もし AviUtl ExEdit2 で上下が逆に
なる場合は `TSMemory-TVTestSrc.ini` の `[M2V] flip=1` で反転出来ます。

test_plugin では、`UninitializePlugin()` の後に `FreeLibrary()` して
**アンロード後に何も残っていない事**も確認しています。

> AviUtl2 はパッケージの入れ直し等でプラグインを差し替える際に DLL を
> アンロードします。アンロード後に呼ばれる物を残すと落ちます
> (WER に `TSMemory-TVTestSrc.aux2_unloaded` として記録されます)。
> 対処したのは下記の 2 点です。
>
> - **ウィンドウとウィンドウクラス**
>   ウィンドウを残すとウィンドウプロシージャがアンロード済みのコードを
>   指したままになります。クラスは `GetModuleHandle(nullptr)` (= 実行ファイル)
>   ではなく**自分の DLL のインスタンス**で登録し、終了時に
>   `DestroyWindow()` と `UnregisterClass()` をします。
>
> - **入力ハンドル (m2v のデコードスレッド)**
>   m2v はファイルを開くとデコード用スレッドを起動します。
>   元の m2v は `instance_manager.c` の `DllMain(DLL_PROCESS_DETACH)` で
>   閉じ直していましたが、これは AviUtl2 の `func_close()` と別スレッドで
>   競合して同じ `MPEG_VIDEO` を二重解放し、`release_out_buffer()` で
>   落ちます (ローダーロックを持ったままスレッド終了を待つ点も危険です)。
>   `DllMain` での後始末をやめ、`UninitializePlugin()` の中で
>   排他をとって確実に閉じるようにしています
>   (`TSMemoryInputUninitialize()`)。

### tests/test_save.cpp

AviUtl ExEdit2 のふりをしてファイルメニューのコールバックを呼び、
既知のテストパターン (PIXEL_RGBA) を保存させて、書き出されたファイルを
読み戻して色とストライドを確認します。png / jpeg / bmp / tiff の 4 形式。

> WIC の `SetPixelFormat()` は**入出力引数**で、エンコーダが対応していない
> 形式を要求すると別の形式に差し替えられます。これを無視して書き込むと
> png は R と B が入れ替わり、jpeg は縦縞になります。
> 現在は `CreateBitmapFromMemory()` + `WriteSource()` で WIC 側に
> 変換させており、このテストで検出出来るようにしてあります。

### tests/test_inifile.cpp

UTF-8 の設定ファイルから日本語の値が読めるかを確認します。
UTF-8 (BOM 有無) / CP932 / UTF-16 のそれぞれで同じ値が取れる事、
`GetPrivateProfileStringW()` では文字化けする事 (この関数を作った理由) を
併せて見ています。

### tests/test_preset.cpp

`EDIT_SECTION` のふりをして、プリセットの通りにエフェクトが
組み立てられるかを確認します。

- プリセット名からファイルを探せる事 (紛らわしい名前の物と区別出来る事)
- 既にあるエフェクトを作り直さず、順序が保たれる事
- 取り込んだ `.tvtv` のパスがプリセットの古いパスで潰されない事
- `effect.disable=1` が無効状態として反映される事
- `[Preset]` 節を項目として拾わない事

### tests/test_selector.cpp

2 サービス分の PAT / PMT / 映像を含む TS をその場で組み立てて
`CTsSelector` に通し、サービス選択が効いているかを確認します。

- サブチャンネルを指定した時、その映像だけが残りプライマリの映像が落ちる事
- 再生成された PAT に指定したサービスだけが載る事
- **サービスID 0 を指定すると全サービスの映像が混ざる事**
  (1.xx 版の不具合の再現。これを踏まないようにするのが目的)

### tests/test_fuzz.cpp

**壊れた TS を食わせても落ちない事**を確認します。
ビット反転・パケット欠落・同期バイト破壊の 3 通りを混ぜて
入力プラグインに通し、「例外で死なない事」「報告した大きさの外へ
書かない事 (番兵で確認)」を見ます。画が正しいかは見ません
(壊した入力に正解はありません)。

乱数は自前の xorshift で、seed を指定すれば再現します。

> **m2v は壊れた MPEG-2 で返って来なくなる事があります。**
> 実測 (実際の放送 TS の先頭 2MB にビット反転。当時 originals/ に置いていた物):
>
> | seed | 結果 |
> | --- | --- |
> | 20260824 | **60 秒経っても返らない** |
> | 1 | 398 ms |
> | 2 | 381 ms |
> | 3 | **60 秒経っても返らない** |
>
> クラッシュではなくハングです。`func_open()` の中の GOP リスト
> 作成から戻りません。m2v のパーサを直すのは現実的でない為、
> **手前で落とす**方針にしました (下記)。
>
> テスト側は 1 回ずつ別スレッドで走らせて 20 秒で見切り、
> 返らなかった数を数えて出します。**失敗にはしません** —
> 直せない外部コードの既知の弱点で、赤くしても情報になりません。

#### 壊れた TS への耐性

`TSMemory.cpp` の `StreamCallback()` で、壊れたパケットを溜め込む前に
捨てるようにしました。`CTsPacket::ParsePacket()` は元々この判定を
していましたが、**戻り値を捨てていました**。

| 戻り値 | 内容 | 扱い |
| --- | --- | --- |
| `EC_FORMAT` | 同期バイト不正、未定義 PID 等 | **捨てる** |
| `EC_TRANSPORT` | `transport_error_indicator` (受信時のビット誤り) | **捨てる** |
| `EC_CONTINUITY` | 連続性カウンタのずれ (ドロップ) | 残す |

`EC_CONTINUITY` を捨てないのは、パケット自体は正常でただ前が落ちた
だけであり、受信状況が悪い時に全部捨てる事になる為です。

実機の受信エラーは復調段で `transport_error_indicator` が立つので、
これで大半は届かなくなります。`tests/test_tvtp.cpp` で
TEI を立てたパケットと同期バイトを潰したパケットが 1 バイトも
溜まらない事を確認しています。

> **これで全部塞がった訳ではありません。**
> ペイロード内部のビット化けは TEI が立たないので通ります
> (`test_fuzz` が作るのはまさにそれです)。

#### 読み込みの時間切れ (`open_timeout`)

TEI をすり抜けたビット化けが m2v に届くと、**GOP リストの作成が
現実的な時間で終わらなくなる**事があります。無限ループではなく、
TS → PES → ES の組み立てを延々とやり直しています。

サンプリングで採取した内訳 (`func_open()` が返らない状態):

```
extract_standard_pes_header   17.5%
find_next_001                 17.2%
append_pes_first_data          8.0%
ms_read_bits / fill_bits / ms_erase_bits  17.0%
get_pes_packet_data_length     4.5%
```

10 分待っても返りませんでした。その為 `func_open()` は
**別スレッドで開いて時間を区切り**、間に合わなければ
「読み込めなかった」事にします (既定 10 秒、`[M2V] open_timeout`)。
遅れて出来上がった物は、そのスレッド自身が片付けます
(参照カウントで持ち主を決める)。

これで `test_fuzz` の「返って来ない」は無くなりました。

#### 壊れたデータでのクラッシュ

ファザーで見つかった m2v の不具合を直しました。いずれも
「壊れた入力でしか通らない経路」で、**正常な TS の出力は
ビット単位で変わりません** (`test_decode` の 1 フレーム目を
SHA-256 で確認)。

| 場所 | 内容 |
| --- | --- |
| `pes.c` の `*_stream_data_length()` 3 つ | `extract_standard_pes_header()` の戻り値を見ておらず、`p->size - header_length` を **unsigned で** 返していた。長さが化けると `ref_pes_packet_data()` のポインタが範囲外へ飛び、`ts_read()` の `memcpy` が明後日の番地を読む |
| `transport_stream.c` の `ts_read()` | `(packet_data, packet_rest)` は呼び出しを跨いで保持されるのに、その間に PES バッファが realloc / free され得る。使う直前に PES バッファの範囲へ収める `ts_clamp_packet()` を追加 |
| `gop_list.c` の `new_gop_list()` | `malloc` の戻り値を見ずに書き込む箇所が 2 つ。更に、シーケンスヘッダより先にピクチャが来ると `sc->index` が NULL 参照になる。1 段目の出口で `c` / `sc` の両方を確かめる |
| `resize.c` の `build_fast_resize_table()` | 索引が行の外を指す事がある（元の m2v も `in[prm->index[x][i]]` と読むので平坦化で持ち込んだ物ではない）。表を作る時に行の中へ丸める |
| `resize.c` の `resize()` | `prm` を作った時の入力の大きさを憶えておき、渡されたフレームと食い違ったらリサイズしない |

#### 位置の特定に使う道具

`tests/tools/fuzz-locate.sh` は、`test_fuzz` が見つけた
seed / iteration の壊し方を再現して落ちた位置を関数名で出します。

```bash
# 落ちる seed を探す
cd build/tests
./test_fuzz.exe ../../build/ts-examples/sample.ts ../../dist/TSMemory-TVTestSrc.aux2 9 1004

# 出力の最後に見えた iteration の次を指定する
bash tests/tools/fuzz-locate.sh 1004 3
```

VEH で例外の番地を採り、スタックに残る戻り番地を
`llvm-symbolizer` で解決します（デバッグ情報付きの aux2 は自動で作ります）。

#### 残っている物 — リサイズと実フレームの食い違い

上記の修正で `seed 1000-1014` の失敗は **6/30 相当から 2/15** に減りましたが、
ゼロにはなっていません。

追い掛けた結果、残りは**同じ系統**です。壊れたシーケンスヘッダによって
`RESIZE_PARAMETER` と実際のフレームの形が食い違い、
`component_resize()` が面の外を読みます。潰した順に、

1. 索引が行の外 → 表を作る時に丸めた
2. 行数がフレームより多い → 入力の大きさを照合するようにした
3. **クロマ面だけが噛み合わない** ← いまここ

3 は輝度の寸法が一致していても起きます。`chroma_format`
(4:2:0 / 4:2:2) がパラメータ作成時とデコード時で食い違うと、
クロマ面の行数が合いません。`FRAME` は輝度の幅・高さしか持たないので、
**読み出し側を 1 つずつ塞ぐやり方では届きません**。

まともに直すなら `component_resize()` に面の大きさを渡す
(= `FRAME` に面ごとの寸法を持たせる) 事になり、
ここまでの外科的な当て込みより踏み込んだ改造になります。

> **実運用で踏む可能性は低い、と判断して打ち切りました。**
>
> 「チャンネルを何度も切り替えれば、1 つのバッファに別解像度の映像が
> 並んで再現するのでは」を実験で確かめました。**再現しません。**
>
> - `EVENT_CHANNELCHANGE` で `PurgeBuffer()` が走り、切り替え後の
>   データしか残らない
> - 仮に混ざっても、`mpeg_video.c` は
>   `orig_h_size` / `orig_v_size` の変化を見て
>   **リサイズパラメータを作り直す** (line 380 付近と 714 付近)
> - 日本の地上波は映像 PID が `0x0111` で共通な為、局を跨ぐと同じ PID に
>   別の映像が並ぶ。これを `ts-sample3.ts` の別解像度サービス
>   (PID `0x0131`) を `0x0111` に載せ替えて合成し、通した結果は
>   **PASS / 530 フレーム / クラッシュなし**。
>   **上記の寸法照合を外した状態でも落ちない**事も確認した
>   (= m2v 本来の作り直しで足りている)
>
> 残るクラッシュの条件は、作り直しの判定が見る `orig_*` と、
> `create_resize_parameter()` が使う `display_*` が食い違う事です。
> ビット反転で `display_*` だけが壊れた時にしか起きず、
> 正常な放送では発生しません。
>
> 再開する場合は `tests/tools/fuzz-locate.sh` で追えます。

> **まだ残っています。**
> seed を変えて回すと、GOP リスト作成中に落ちる事があります。
>
> ```bash
> cd build/tests && ./test_fuzz.exe ../../build/ts-examples/sample.ts \
>     ../../dist/TSMemory-TVTestSrc.aux2 9 1004
> ```
>
> **構造化例外では包めません。** llvm-mingw
> (`x86_64-w64-windows-gnu`) の `__try` / `__except` は
> アクセス違反を捕まえず、そのまま落ちます
> (`-fms-extensions` / `-fseh-exceptions` のどちらでも確認済み)。
>
> 残りの選択肢:
>
> - 見つかった順に m2v を直していく (今回の 3 件と同じやり方)
> - VEH (`AddVectoredExceptionHandler`) で捕まえ、
>   デコード用スレッドだけを畳む。プロセス全体に影響する仕掛けで、
>   例外時にロックを握ったままになる危険がある
> - デコードを別プロセスに追い出す (確実だが大掛かり)
>
> テストスイートは既定の seed で走る為、現状は緑です。
> **この件を隠している事になるので注意。**

### tests/test_tvtp.cpp

**TVTest 側のプラグイン (`TSMemory.tvtp`) を TVTest 無しで動かします。**
TVTest 本体の代わりになる最小のホスト (`MESSAGE_*` を処理する
コールバック) を用意し、`TVTInitialize()` →
`EVENT_PLUGINENABLE` → ストリーム投入 → `EVENT_COMMAND` →
`TVTFinalize()` までを実際に呼びます。

長い間、TVTest 側だけテストが 1 つも無く、設定値の扱いと終了処理の
不具合はここから出ました。

- ホストの登録 (コマンド・イベント・ストリームコールバック)
- 有効化で共有メモリが出来て、無効化で解放される事
- 実物の TS を流し込むと溜まる事
- **`MemorySize` が大きすぎる時に制限される事** — 制限が無いと
  `CreateFileMapping()` に切り捨てられた値が渡る
- **起動待ちの最中に終了しても待たされない事** — `TVTFinalize()` が
  ワーカーを置き去りにすると、この後の `FreeLibrary` で
  アンロード済みのコードを実行する事になる

> **`aviutl2.exe` は起動しません。**
> `AviUtlPath` に「今動いているプロセスの名前」(テスト自身の実行ファイル)
> を書くと `IsAviUtlRunning()` が真になり、起動せずに待ち受け待ちへ
> 入ります。
>
> 共有メモリの名前は `tsmemory<N>.tvtv` で、N は空いている番号です。
> **本物の TVTest が動いていると 0 番はそちら**なので、
> `Initialize()` の前後でミューテックスの有無を比べて自分の番号を
> 割り出しています。
>
> 「起動待ちの最中に終了」の確認は、**AviUtl2 が待ち受けていると
> 飛ばします**。`IsAviUtlReady()` が真になって待ちループに入らず、
> 何を測っても 0ms で通ってしまう為です。

### tests/test_multich.cpp

マルチ編成の TS (`build/ts-examples/multi.ts`。実際の放送 TS を置けばそちらも) を
サービス毎に `CTsSelector` で絞り、共有メモリに載せて入力プラグインで
デコードし、チャンネル毎に別の絵が出てくる事を確認します。

```
--- service 23608 : MX1 (primary)
    selected 81035 packets, video PIDs: 0x0100(217) 0x0101(253) 0x0111(79422)
    decoded 1920x1080  368 frames
--- service 23610 : MX2 (sub channel)
    selected 20246 packets, video PIDs: 0x0100(217) 0x0103(253) 0x0131(18633)
    decoded 854x480  365 frames
picture size differs : 1920x1080 vs 854x480
```

サブチャンネルは解像度が落ちる事が多いので、大きさが違えばそれだけで
別サービスをデコードしている事になります。同じ大きさの場合は
フレーム 0 同士の平均絶対差で判定します。

デコード結果を目視する場合:

```bash
compilers/python/python.exe tests/tools/raw2png.py build/tests/ch23610.raw 854 480 build/tests/ch23610.png 3
```

TS の構成 (サービスと映像 PID の一覧) は次で確認できます。

```bash
compilers/python/python.exe tests/tools/ts-services.py build/ts-examples/multi.ts
```

### tests/tools/test-live.sh (TVTest を実際に走らせる通し確認)

TvtPlay + BonDriver_Pipe で TS ファイルを再生し、TSMemory.tvtp が取り込んだ
映像を AviUtl ExEdit2 の代わりのプロセス ([tests/test_receiver.cpp](../tests/test_receiver.cpp)) が
受け取ってデコードするところまでを通します。

```bash
bash tests/tools/test-live.sh
```

事前に下記を配置しておく必要があります。

```
dist/TVTest-x64/BonDriver_Pipe.dll        <- BonDriver_Pipe (x64)
dist/TVTest-x64/Plugins/TvtPlay.tvtp      <- TvtPlay (x64)
dist/TVTest-x64/Plugins/Buttons*.bmp      <- third_party/TvtPlay/src/Buttons*.bmp
dist/TVTest-x64/Plugins/TSMemory.tvtp     <- dist/TSMemory.tvtp
dist/TVTest-x64/Plugins/TSMemory.ini      <- res/TSMemory.tvtp.ini
```

TvtPlay と BonDriver_Pipe のソースは `bash tools/setup-tvtplay-src.sh` で
`third_party/TvtPlay/` に取得できます (`src/` が TvtPlay 本体、
`BonDriver_Pipe_src/` が BonDriver_Pipe)。**ビルドは vcxproj なので
MSVC が要ります** (`bash tests/tools/setup-msvc.sh`)。
配布されているビルド済みバイナリを使っても構いません。

> ブランチは `master` と `work` が同じコミットを指しています。
> 配布物の `x64/plus/TvtPlay.tvtp` に当たるのは `work-plus` で、
> `TVTPLAY_REF=work-plus bash tools/setup-tvtplay-src.sh` で取れます。

確認済みの結果:

```
a request arrived from TSMemory.tvtp                     ok
  serial   : 1
  file     : ...\dist\TVTest-x64\Plugins\tsmemory0_1.tvtv
the dummy .tvtv file exists                              ok
func_open() on the captured .tvtv succeeded              ok
  captured : 1920x1080  30000/1001 fps  407 frames
decoded captured frame 0                                 ok
decoded captured frame 203                               ok
```

取り込み量の実測 (`TSMEMORY_BUFFER_SECONDS` でキャプチャまでの待ち時間を変えられます):

| 待ち時間 | 取り込み量 | デコード結果 |
| --- | --- | --- |
| 8 秒 | 7.91 MB (満杯前) | 213 フレーム / 7.11 秒 |
| 40 秒 | 10.00 MB (満杯) | 407 フレーム / 13.58 秒 |

引数に元の TS を渡すと、取り込んだ範囲が元動画のどのあたりなのかを
PTS から算出して出力します。TVTest 起動から 30.1 秒後にキャプチャした場合:

```
=== sending the TSMemory Execute command (30.1 sec after launching TVTest) ===
  captured : 10.00 MB (55775 packets)
  pts span : 8.07 sec (from the first to the last video PTS)
  position : the capture covers 21.59 - 29.66 sec of ts-sample2.ts
```

**取り込みの末尾はキャプチャした時点にほぼ追いついています**
(TVTest の起動〜ストリーム開始に 1 秒前後かかる為、実質の遅れはほぼ 0)。
取り込める長さは `MemorySize` が上限になります。

これで下記が実機で動く事を確認しています。

- TVTest (x64) のストリームコールバック → `CTsSelector` → リングバッファ
- コマンド実行 → スナップショット用の共有メモリとダミーファイルの作成
- 名前付きイベントによる連携要求の送信と受信
- 受け取った `.tvtv` を入力プラグインで開いてデコード

> 検証中に判明した注意点: TVTest の ini の真偽値は `yes`/`no` でないと
> 読まれません (`1` は無視されます)。また TvtPlay は既に起動している
> TVTest を見つけるとそちらにファイルを渡すので、古いプロセスが残っていると
> 再生が始まりません。`test-live.ps1` は毎回この 2 つを整えてから実行します。

### TVTest 本体

ビルドした TVTest を `/nodriver` で起動し、ログで確認しています。

```
******** TVTest ver.0.10.0-dev (Release x64) 起動 ********
Work with LibISDB ver.0.2.0 7c4fdaa
Compiled with MSVC 19.51.36256.0
プラグインを "...\dist\TVTest-x64\Plugins" から読み込みます...
TSMemory.tvtp を読み込みました。
```

64bit の TVTest に 64bit の `TSMemory.tvtp` が読み込まれ、終了処理まで
正常に通る事を確認しています。

### AviUtl ExEdit2 上での確認 (実機)

自動テストでは AviUtl2 本体を動かせないため、下記は実機で確認しています。

| 項目 | 結果 |
| --- | --- |
| `TSMemory-TVTestSrc.aux2` がプラグインとして認識される | OK |
| TVTest からの実行でタイムラインにメディアオブジェクトが配置される | OK (`tsmemory1_3.tvtv` が Layer1 に配置され、1920x1080 で再生) |
| 配置された映像の上下 | 正常 (`flip` 不要) |
| パッケージのインストール | 2 回目以降 OK (上記「更新時の注意」参照) |
| キャプチャ・ユーティリティのウィンドウ | 追加直後は非表示。[表示] メニューから表示する |

ウィンドウを使わずに保存出来るよう、ファイルメニューにも
「画像として保存 (TSMemory)」を追加しています。

---

## 隠し設定

設定ファイル (TVTest 側の `TSMemory.ini` / AviUtl2 側の `TSMemory-TVTestSrc.ini`)
には常用する設定だけを書いてあります。
下記は**環境的に値が決まっていて変更する必要が無い**もの、または
問題が起きた時の調整用です。必要な時だけ ini に追記してください。
書かなければ既定値で動きます。

### TVTest 側 (`[Settings]`)

| キー | 既定 | 内容 |
| --- | --- | --- |
| `LaunchWait` | `30` | AviUtl2 を起動した際に待ち受け開始を待つ上限 (秒)。毎回待つ訳ではなく、0.25 秒毎に確認して開始次第すぐ進みます。AviUtl2 が起動直後に終了した場合も待たずに切り上げます。実際にここまで待つのは「AviUtl2 は動いているが TSMemory-TVTestSrc.aux2 が応答しない」時だけです。起動が極端に遅い環境でタイムアウトする場合に増やしてください。指定できる範囲は 5〜600 秒です。 |

### AviUtl2 側 (`[Bridge]`)

| キー | 既定 | 内容 |
| --- | --- | --- |
| `Enable` | `1` | TVTest からの読み込み要求の待ち受けを行うか。`0` にすると入力プラグインとキャプチャ機能だけになります。 |
| `ReplaceLayer` | `1` | 配置先レイヤーの既存オブジェクトを削除してから配置するか。`0` にすると既存オブジェクトと重なった場合に配置が失敗します。 |
| `ReadyDelay` | `0.5` | プロジェクトの初期化完了から待ち受け開始までの余裕 (秒、小数可、0〜10)。TVTest から AviUtl2 を起動した直後の読み込みが空になる場合に増やしてください。 |
| `ReadyTimeout` | `30` | プロジェクトの初期化通知が来ない場合に、待たずに待ち受けを開始するまでの時間 (秒、1〜600)。 |
| `ExitConfirmText` | `現在の編集データは更新されています` | `SuppressExitConfirm` の応答対象を判別する文言。表示が変わっている場合に調整します。 |
| `ExitConfirmButton` | `いいえ` | 上記で押すボタンの文言。 |
| `PresetFile` | (空) | フィルタプリセットのファイルを絶対パスで直接指定します。`Preset` (名前指定) で見つからない場合や、AviUtl2 のデータフォルダ以外に置いた物を使いたい場合に。 |

### AviUtl2 側 (`[Capture]`)

| キー | 既定 | 内容 |
| --- | --- | --- |
| `Enable` | `1` | キャプチャ・ユーティリティ (ウィンドウとファイルメニュー) を追加するか。 |

### AviUtl2 側 (`[M2V]` / MPEG-2 デコーダ)

> **ver.0.3.0 でセクション名を `[settings]` から `[M2V]` に変えました。**
> `[settings]` は m2v が単体プラグインだった頃の名前で、1 つの ini に
> 連携・キャプチャの設定も同居する今の構成では何の設定か判りません。
>
> 変更は `tools/patch64.py` の `registry.c` への当て込みで行っています。
> **古い ini がそのまま動くよう `[settings]` も読みます**
> (`[M2V]` に無い時だけ。キーは全て非負なので `-1` を「無い」の印に使う)。
> `src/aviutl2/input_tvtv.cpp` が読む `flip` も揃えてあります。
>
> 実測 (`ts-sample.ts` の 1 フレーム目の SHA-256 上位):
>
> | ini | 結果 |
> | --- | --- |
> | `[M2V] idct_func=2` (既定) | `a3a361a4c923313b` |
> | `[M2V] idct_func=0` | `4ec8901929536707` ← 新セクションが効いている |
> | `[settings] idct_func=0` | `4ec8901929536707` ← 旧セクションも読める |
> | `[Other] idct_func=0` | `a3a361a4c923313b` ← 無関係な名前は無視される |
> | `[M2V]=2` と `[settings]=0` の両方 | `a3a361a4c923313b` ← `[M2V]` が優先 |

| キー | 既定 | 内容 |
| --- | --- | --- |
| `flip` | `0` | 画像の上下を反転するか。m2v は `biHeight` が正でも画像を上から下の順に書き込みますが、**AviUtl2 はそれで正しく表示される事を確認済み**です。反転が必要になる状況は今のところありません。 |
| `yuy2_matrix` | `0` | YUY2 の色変換行列。**`0` (変換しない) 以外にしてはいけません。** YUV→RGB は AviUtl2 が行う (高さ 1080 なら BT.709 を自動選択) ので、ここで変換すると二重変換になります。 |
| `field_order` | `0` | フィールドオーダー (0=元のまま / 1=トップファースト / 2=ボトムファースト)。 |
| `idct_func` | `2` | IDCT の種類 (0=reference / 1=LLM int / 2=AP922 int)。 |
| `simd` | — | **64bit 版では常に無効**です (MMX/SSE ルーチンは 32bit x86 専用)。値を書いても無視されます。 |
| `re_map` / `color_matrix` | — | **YUY2 経路では使われません。** m2v の BGR 変換専用の設定で、本プラグインは BGR 経路を通りません。 |
| `audio` | `0` | 音声を取り込むか。**TVTest 側の `[Settings] Audio=1` と両方**が要ります (「9. 音声 (既定は無効)」)。※ 既定の ini にも書いてあるので隠し設定ではありません。 |
| `open_timeout` | `10` | 読み込みの時間切れ (**秒**、1〜600)。壊れた TS で m2v が返って来ない場合に諦める為の上限です。`0` を書くと待ち続けます (従来の挙動)。詳細は「読み込みの時間切れ」。※ 既定の ini にも書いてあるので隠し設定ではありません。 |
| `limit` | `8388608` | m2v が GOP リストの作成に使う上限 (バイト)。共有メモリから読む本プラグインでは変更する意味がありません。 |
| `gl` | `0` | GOP リストをファイルに保存するかの指定 (m2v が単体プラグインだった頃の機能)。**64bit 版では常に「保存しない」**です。 |
| `file` | `0` | 連番ファイルを 1 つのストリームとして扱うか (`1` で有効)。共有メモリを 1 つ読むだけなので使いません。 |

> `flip` / `yuy2_matrix` / レンジの扱いは実際のキャプチャ画像で検証済みです。
> 詳細は「YUY2 の扱いについて」を参照してください。

---

## YUY2 の扱いについて

入力プラグインは**デコーダの生の YUV をそのまま YUY2 で AviUtl2 に渡し**、
YUV→RGB 変換は AviUtl2 に任せています (`yuy2_matrix=0`)。
実機のキャプチャ画像で下記を確認済みです。

| 項目 | 結果 |
| --- | --- |
| 上下の向き | 正常 (`flip=0` のまま) |
| レンジ | リミテッド (Y:16-235) のまま渡し、AviUtl2 が 0-255 へ伸張 |
| 色行列 | AviUtl2 が高さ 1080 から BT.709 を自動選択 |

レンジの判定は、黒帯 (レターボックス) のあるフレームを保存して調べました。

```
capture1.png  1920x1080
  min=0  max=251   <=0 : 25.357%
  corner top-left avg=0.0   top-right avg=0.0
  corner bottom-left avg=0.0   bottom-right avg=0.0
```

黒帯が四隅とも RGB 0 なので、AviUtl2 側がリミテッドレンジとして
正しく伸張しています (伸張していなければ RGB 16 前後で止まります)。
保存した画像の解析には `tests/tools/analyze-levels.py` を使えます。

```bash
compilers/python/python.exe tests/tools/analyze-levels.py capture.png
```

---

## 速度の調査 (tests/tools/m2v-profile)

デコードのどこが遅いかをサンプリングで測ります。

```bash
bash tests/tools/m2v-profile.sh build/ts-examples/sample.ts 60
```

デバッグ情報付きの aux2 を `build/prof/` に作り、実際の入力プラグイン経由で
デコードしながら全スレッドの RIP を採取して、llvm-symbolizer で関数別に
集計します。外部のプロファイラは要りません。

**推測で最適化しない為の物です。** 実際、当初は IDCT と動き補償が重いと
考えていましたが、測ると `component_resize` が 7 割超でした
(「デコード速度の改善」を参照)。

> スレッドの列挙 (`CreateToolhelp32Snapshot`) は数十 ms 掛かるので、
> 最初に一度だけ開いて使い回しています。毎回列挙すると
> 2 秒間で 20 サンプルしか取れません (実際に踏みました)。

> PE の場合 llvm-symbolizer に渡すのは **ImageBase を足した仮想アドレス**
> です。RVA のままだと全て `??` になります。

---

## クラッシュの調査 (tests/tools/debug-launch)

WER のレポートは**読み込み中のモジュールしか列挙しません**。
その為「アンロード済みの DLL のコードを実行していた」類の問題は、
障害モジュールが `KERNELBASE.dll` とだけ記録され、原因のプラグインは
一覧に現れません (実際にこれで判断を誤りました)。

`tests/tools/debug-launch.cpp` はデバッガとして対象を起動し、
**アンロード済みモジュールの範囲も覚えたまま**例外の発生位置を
モジュール名で報告します。Visual Studio も WinDbg も要らず、
レジストリ (WER の LocalDumps) も触りません。

```bash
bash tests/tools/debug-launch.sh "D:/programs/Multimedia/AviUtl2/aviutl2.exe"
```

終了時クラッシュを特定した時の出力:

```
[unload] ...\TSMemory-TVTestSrc.aux2
[exception] code=0xC0000005 addr=00007FFF59BB1BD0 first=1 thread=33040
    -> ★アンロード済み ...\TSMemory-TVTestSrc.aux2 + 0x1BD0
[exception] code=0xC0000005 addr=00007FFFD0EC9C7D first=1 thread=33040
    -> ...\KernelBase.dll + 0x59C7D
    read from address 00007FFF59BFE36E
[exit] code=0x000000FF
```

正常な場合は例外が出ず `[exit] code=0x00000000` で終わります
(起動途中に出る `0xE06D7363` は AviUtl2 が内部で処理している
C++ 例外なので無視して構いません)。

イメージサイズは対象プロセスの PE ヘッダを読んで求めている為、
アドレスからモジュールへの対応は正確です。

---

## dist/ の ini は毎回入れ直す

プラグインは**自分と同じ場所の ini** を読む
(`src/aviutl2/plugin_main.h`)。テストは `dist/TSMemory-TVTestSrc.aux2` を
そのまま読み込む為、`dist/TSMemory-TVTestSrc.ini` が無いと既定値で、
古いままだと**古い設定でテストが走る**。

更に `[Capture]` の設定は終了時に `WritePrivateProfileStringW()` で
書き戻される。この API は無ければファイルを作り、あれば該当キーだけ
書き換える。その結果、放っておくと
**「更新日時だけ新しくなって中身は古い」**ファイルが残る
(`[Caption]` の節が丸ごと無い、等)。

その為 `tools/build.sh` が毎回 `res/TSMemory-TVTestSrc.aux2.ini` を
`dist/` に上書きする。**`dist/` の ini を手で直しても次のビルドで消える。**
手で試したい設定は `build/plugin/` (`tests/tools/test-live.sh`) か、
実際にインストールした先で変える事。

配布物に入る ini は `build/package/` と `build/au2pkg/` 経由で
zip に入る物で、こちらは元から `res/` の写しになっている。

## うまく取り込めない時の確認

TVTest と AviUtl2 の両方のログに、取り込み量と配置結果が出力されます。

TVTest のログ (実行した時):

```
TSMemory.tvtp : 55775 パケット (10.00 MB / バッファの 100%) を書き出しました : tsmemory0_1.tvtv
```

- `バッファの 100%` … リングバッファが満杯。これ以上遡るには `MemorySize` を増やす
- `バッファの 30%` 等 … まだ溜まりきっていない。有効にしてから間もない状態

AviUtl2 のログ (起動時と読み込んだ時):

```
TSMemory: プロジェクトの初期化を待っています
TSMemory: TVTest からの要求の待ち受けを開始しました
TSMemory: 取り込んだ映像 1920x1080 7.640 秒 -> 229 フレームで配置します
TSMemory: 映像を読み込みました (レイヤー 1 / フレーム 1 - 229)
```

- 「取り込んだ映像 ... 秒」が短い … TVTest 側で溜まっていない (上記を確認)
- 「... -> 0 フレームで配置します」 … 長さを取得出来ていない。この場合は
  AviUtl2 の既定のオブジェクト長になるため後ろが切れます

---

## 終了時の「現在の編集データは更新されています」

TSMemory がタイムラインにオブジェクトを置くと編集済み扱いになるので、
AviUtl2 の終了時に保存の確認が出ます。

**AviUtl ExEdit2 には編集済みフラグを解除する API がありません**
(`EDIT_SECTION::set_edited_state()` は設定専用で、`EDIT_HANDLE` や
`PROJECT_FILE` にも保存・解除の関数はありません)。
その為、正規の方法でこの確認を消す事は出来ません。

その為、消したい場合は確認ダイアログを検出して自動応答する機能を使います。
本体の動作に割り込む形になるので**既定では無効**です。

```ini
[Bridge]
SuppressExitConfirm=1
```

値は 3 段階です。

| 値 | 動作 |
| --- | --- |
| `0` | 何もしない (既定) |
| `1` | TSMemory が配置しただけの状態に限って応答する |
| `2` | TSMemory が取り込んだ後であれば、編集していても応答する |

`1` の場合は下記を全て満たす時だけ「いいえ」(保存しない) と応答します。

- TSMemory がオブジェクトを配置済みである
- その後にオブジェクトの更新 (`EVENT_TYPE::UPDATE_OBJECT`) が検出されていない

つまり手で編集した内容を黙って捨てる事はありません。
ただし**配置直後 2 秒以内の編集は TSMemory 自身の更新と区別出来ない**ので、
その間の編集は検出されません。

`2` は上記のうち後者の条件を外したものです。トリミングや色調整をしてから
画像を保存する、といった**保存する必要が無い編集を毎回する**使い方で、
`1` だと結局確認が出てしまう場合に使います。

> **`2` は編集内容を確認無しで破棄します。** プロジェクトを保存して続きを
> やる使い方には向きません。
> なお `2` でも**TSMemory が一度も取り込んでいない場合は応答しません**。
> TSMemory と無関係に開いたプロジェクトを壊す事はありません。
> `2` では編集の有無を見ないので `EVENT_TYPE::UPDATE_OBJECT` の監視自体を
> 登録しません。

ダイアログの判別は文言の一致で行っています。表示が変わっている場合は
`ExitConfirmText` (メッセージ) と `ExitConfirmButton` (押すボタン) で
調整してください。一致しなかった場合はログに

```
TSMemory: 対象外のダイアログを検出しました : ...
```

としてそのダイアログの文言が出力されるので、それを見て調整出来ます。

動作は `tests/test_plugin.cpp` で 3 つの値それぞれについて確認しています
(実際に MessageBox を出して、`0` は何もしない事・`1` は自動応答され編集後は
応答しない事・`2` は配置直後の猶予を過ぎても応答し続ける事)。

---

## 字幕対応 (既定は無効)

`feature/caption` で進めている。ここには**実測で確定した事**だけを書く。

### 放送に何が入っているか (実測)

NAS の録画 6 本 (約 3 時間) を走査した結果。

```
PID 0x0130  stream_type 0x06  component_tag=0x30 (字幕)      data_component_id=0x0008
PID 0x0138  stream_type 0x06  component_tag=0x38 (文字スーパー) data_component_id=0x0008 + CA
```

PES は `stream_id=0xBD` (private_stream_1)、`data_identifier=0x80`、
`data_group_id` は 0x00/0x20 が字幕管理データ、0x01〜/0x21〜 が字幕文データ。
**ARIB STD-B24 の標準的な構成**で、録画は既に平文 (`scrambling_control=0`)。

| | |
| --- | --- |
| 本文 (data_unit 0x20) | 2850 個 |
| DRCS (data_unit 0x30) | 87 個。**6 本中 3 本はゼロ** |
| DRCS の字形 | **全て 36x36 / depth=2 (4 階調 = 2bit/画素)** |
| 異なり字形 | 1 本あたり 4 種程度。6 本合計でも 10 種未満 |

字形を描画すると `→` `♬` `〳〵` といった**記号**だった。
そして**同じ符号 `0x4121` が番組ごとに別の字形**を指す。

```
md5=583134b8...  code=0x4121,0x4122  → 矢印
md5=37f6ecf3...  code=0x4121         → 音符
```

**符号は識別子にならない。ビットマップが実体**である。

### 外字 (DRCS) をフォントとして渡せる

DRCS は**字形のビットマップが放送に乗って来る**ので、受信側は常に正確な
字形を持っている。それをフォントに仕立てて AviUtl2 に渡せれば、
**字幕をテキストのまま放送どおりの字形で出せる**。

まず駄目だった方法。

```
before: GDI=0 DWrite=0
AddFontResourceEx(FR_PRIVATE)=1     ← 追加自体は成功
after : GDI=1 DWrite(noupd)=0 DWrite(upd)=0
```

`AddFontResourceEx(FR_PRIVATE)` はプロセス内に足せるが
**DirectWrite からは見えない**。AviUtl2 は DirectX11 ベースなので使えない。

代わりに SDK の `HOST_APP_TABLE::register_font_collection()` を使う。
ブラウザの `@font-face` に相当し、**ホスト側が自分のフォント集合に
加えてくれる**。実機で下記まで確認済み。

| 確認項目 | 結果 |
| --- | --- |
| メモリ上のフォントから `IDWriteFontCollection` を作る | **可** (`CreateInMemoryFontFileLoader` → `FontSetBuilder` → `CreateFontCollectionFromFontSet`)。**ファイル不要** |
| `register_font_collection()` で渡す | **可**。AviUtl2 が `[INFO] [Media::FontManager::registerFontCollection] register font [...]` と記録する |
| フォント一覧に出る・選択できる | **可** |
| テキストの `<@フォント名>` で切り替わる | **可**。同じ本文を 2 行並べ、片方だけ制御文字を付けると字形が変わる |
| 字が無い時のフォールバック | **起きない**。`.notdef` (豆腐) が出る。**勝手に別の字形へ差し替えられない**ので好都合 |

> **`enum_font_name()` は判定に使えない。**
> `RegisterPlugin` の中で呼ぶと登録の前後どちらも 0 件を返す。
> それでも一覧に出て描画もされるので、この API は別の時点でしか
> 使えないと思われる。判定には使わない事。

> **ログはファイルを見る事。**
> ログウィンドウは既定 200 行で、`RegisterPlugin` の出力は流れてしまう。
> `C:\ProgramData\aviutl2\Log\aviutl2_*.log` に全て残る (cp932)。

### 字幕をどう配置するか

AviUtl2 のテキスト制御文字が ARIB 字幕の表現とほぼ対応する。

| ARIB | AviUtl2 |
| --- | --- |
| 文字色・影縁色 | `<#ffffff>` `<#ffffff,000000>` |
| 文字サイズ (SSZ/MSZ/NSZ) | `<s32>` `<s*1.5>` |
| 表示位置 (APS) | `<p20,40>` |
| 字間・行間 | `<gw10>` `<gh10>` |
| **書体の一括変更** | **`<$プリセット名>`** |

`<$字幕>` を本文の先頭に置けば、利用者が AviUtl2 側でその
テキストプリセットを 1 つ直すだけで**全ての字幕に効く**。
タイムライン上で個別に触る必要が無い。

### 実装

`src/aviutl2/caption/` に閉じている。`bridge.cpp` から見えるのは
`ts_caption.h` だけで、`[Caption] Enable=0` の時はクラスを作らないので
字幕側のコードは一切動かない。

| ファイル | 役割 |
| --- | --- |
| `ts_caption.cpp` | 共有メモリの TS から字幕のデータユニットを取り出す |
| `arib_text.cpp` | ARIB STD-B24 の 8 単位符号を解く |
| `arib_to_aviutl2.cpp` | AviUtl2 のテキスト制御文字に直す |
| `drcs_ttf.cpp` | 外字の字形を TrueType に組み立てる |
| `drcs_font.cpp` | それを AviUtl2 に登録する |

配置は `bridge.cpp` の `PlaceCaptions()`。字幕文 1 つにつき
「テキスト」オブジェクトを 1 つ、`[Caption] Layer` のレイヤーへ、
映像の先頭からの秒数に合わせて並べる。

#### レイヤーの扱い

**`[Caption] Layer` は映像 (`[Bridge] Layer`) とは別にする。**
同じにすると、字幕を置く前の掃除 (`find_object` → `delete_object` の
繰り返し) が直前に置いた映像のオブジェクトまで消してしまう。
同じ番号だった場合は置かずに警告を出す。

**ロックされたレイヤーにはオブジェクトを置けない。**
映像側と同じ扱いにしてある。

| 状況 | 動き |
| --- | --- |
| `LockLayer=1` で前回掛けたロック | 置く前に外し、置いた後に掛け直す |
| 手で掛けたロック (`LockLayer=0`) | 勝手に外さず、置かずに警告 |
| 1 件も置けなかった | ロックは掛けない (空のレイヤーをロックしても紛らわしい) |

`create_object()` は失敗しても `nullptr` を返すだけなので、
何もせず素通りすると**ログには「字幕を 0 件配置しました」としか出ず、
原因が判らない**。上の判定はその為に入れている。

#### 実測で決めた所

- **`0x90` (CDC) の引数の長さは固定ではない。**
  実データに `90 20 44` と `90 51` の両方が現れる。COL (`0x8C`) と同じで
  「`0x20` が続けば 2 バイト、そうでなければ 1 バイト」。
  1 バイト固定にすると本文が丸ごとずれ、
  `＃僉淵灰織蹇次縫＃＃は今年…` のように化ける
  (外字と誤判定される数が 12 → 206 に増える)。
- **属性は「変わった時に、本文の直前で」出す。**
  受け取った順にそのまま出すと放送側が本文の無い所で何度も指定し直す為、
  `<s*0.5><s*0.5><#00ffff><#0000ff><#ff0000>` が延々と並ぶ。
- **本文の `<` は `<<` に直す。** 「<笑い>」のような表記が実際に出てくる。
- **`90 20 4n` は色ではなく色配列 (CLUT) の選択。**
  これを前景色として扱うと**字幕がほぼ全て同じ色に染まる**
  (実測では全ての字幕文が `<#0000ff>` = 青字になっていた)。
  放送 25 番組の字幕文 36 件を調べたところ、この形は
  `90 20 44` (22 件) と `90 20 40` (4 件) の 2 種類しか現れず、
  番組をまたいで同じ値だった。話者ごとに変わる色ではない。
  直した後は白 (ナレーション) と黄・シアン (話者) が正しく出る。

#### 改行は座標から起こす

**放送の字幕は改行を送って来ない。** 1 行ごとに ACPS
(`CSI x;y SP 'a'`) で座標を打つ形なので、CSI を丸ごと読み飛ばすと
**全ての行が繋がって 1 行になる**。`ParseCsi()` で

| CSI | 意味 | 使い道 |
| --- | --- | --- |
| `SP _` (SDP) | 表示領域の左上 | APS をドットに直す |
| `SP W` (SSM) | 文字の大きさ | 行送りの高さ |
| `SP X` (SHS) | 字間 | APS をドットに直す |
| `SP Y` (SVS) | 行間 | 行送りの高さ |
| `SP a` (ACPS) | 表示位置 (ドット) | 改行の判定 |

を読み、`Position` (X, Y, 行送り) として渡す。APS (C0 の `0x1C`) は
文字単位なので、同じドット単位に直してから渡す
(**単位が混ざったままだと呼び出し側で区別できない**)。

**ルビは行として数えない。** ルビは本文の 1 行上に、本文より**先に**
書かれる (実測: ルビ y=449 → 本文 y=509、行送り 60)。位置が動いたら
即改行にすると「ルビ / 本文」で毎回割れる。小型 (SSZ) で書かれて
いるかどうかで見分ける為、改行するかどうかは**位置指定の時点では
決めず、次に本文が来た時に決める**。

#### 背景の箱はスクリプトで敷く

放送の背景は半透明の黒い箱 (上記)。AviUtl2 のテキストには背景色が
無いので、`res/script/TSMemory字幕背景.anm2` を
`create_effect()` でテキストに貼る。

**図形オブジェクトを後ろに置く方式にしていない理由。**
図形は寸法が作った時点で固定されるので、後からフォントや文字サイズを
変えると箱がずれる。`UseBroadcastSize=0` だと寸法を計算する根拠すら無い。
スクリプトなら**描画後の `obj.w` / `obj.h`** を見るので付いて来る。

やっている事:

```lua
obj.copybuffer("cache:...", "object")   -- テキストを退避 (先に w/h を取る)
obj.load("figure", "四角形", 色, size, 0, 角丸, aspect)
obj.draw(0, 0, 0, 1.0, 不透明度)        -- 背景
obj.copybuffer("object", "cache:...")   -- テキストを戻す
obj.draw()                              -- 上に重ねる
```

四角形は正方形で作られるので、長い方を `size` にして `aspect` で潰す
(プラス = 横を縮める / マイナス = 縦を縮める)。1 ドットの板を
`drawpoly()` で伸ばす手もあるが、それだと角丸が保てない。

**`obj.draw()` を呼んだ後は自動描画されない**ので、テキストも自分で描く。

`.au2pkg.zip` は `Plugin\` `Script\` `Figure\` `Transition\`
`Language\` `Alias\` `Preset\` `Default\` の配下を扱えるので
(`aviutl2.exe` 内の一覧より。`Font\` と `Batch\` は不可)、
スクリプトも同梱できる。

スクリプトが入っていないと `create_effect()` が `nullptr` を返す。
背景が無いだけで字幕自体は出るので、警告に留める。

> **スクリプトからログに出すのは `print()`。**
> `obj.mes()` はログではなく「テキストオブジェクトのテキストに文字を
> 挿入する」関数なので、呼んでも何も出ない (実測で 1 往復無駄にした)。

> **図形は 4 引数までにする。**
> `obj.load("figure",name,color,size,line,round,aspect)` と説明されているが、
> 同じ説明の例が `obj.load("figure","円",0xffffff,100,true)` と
> 5 番目に `true` を渡しており、`line` 以降の解釈が怪しい。
> 大きさは `drawpoly()` で伸ばす方が確実で、縦横比の指定も要らない。

> **スクリプトの項目はオブジェクトごとの設定。**
> ツールウィンドウでチェックを入れても、それは**そのオブジェクトだけ**。
> 次の取り込みでは新しいオブジェクトが作られるので戻る。
> その為、項目は `create_effect()` の直後に
> `set_effect_item_value()` で全て入れ直している。

> **AviUtl2 が保存した既定値はスクリプトの宣言より強い。**
> `%ProgramData%viutl2\Default\<スクリプト名>.effect` があると
> そちらの値が使われる。スクリプトの `--track@` の既定を変えても
> 反映されないので、切り分けの時はこのファイルを消す。

#### 行ごとに 1 つのオブジェクトにする

**同じレイヤーには時間の重なるオブジェクトを置けない。**
1 つの字幕の行は同じ時間に出るので、行ごとにレイヤーを分ける。
分けないと 2 行目以降で `create_object()` が `nullptr` を返し、
AviUtl2 のログに `create object failed (object overlap)` が出るだけで
**黙って 1 行しか出なくなる** (実測)。

必要な本数は「同じ秒数が続く数の最大」で先に数える (上限 8 本)。
`[Caption] Layer` はその**先頭の 1 本**になる。

**放送は行ごとに座標を持っている。**まとめて 1 つのテキストオブジェクトに
すると、

- 行の長さが違っても位置は先頭行のものになる
- 背景の箱が全体を囲む 1 つになってしまう (放送は行ごとに別の箱)

`AribItemsToAviUtl2()` は `AribCaptionLayout::Lines` に
「本文 + 左上の座標」を行ごとに入れる。`CTSCaptionSource` はそれを
`TSMemoryCaption` 1 件ずつに展開し、`PlaceCaptions()` が 1 行につき
1 つのテキストオブジェクトを置く。

行ごとに別のオブジェクトになるので、**色やプリセットは行の先頭で
出し直す**。大きさだけは「既定」を覚えたままにする
(`EmittedSize = -1` にすると標準の行の先頭に無駄な `<s>` が付く)。

#### 画面上の位置

字幕平面 (SWF で 960x540 等) の中の座標で送られて来るので、
出力の解像度へ割り直し、**画面中央からのずれ**としてテキスト
オブジェクトの `X` / `Y` に入れる (AviUtl2 のオブジェクト座標は
画面中央が原点)。効果名は `標準描画`。

**ACPS が指しているのは行の下端。** 1 行分引いて上端に直す
(実測: 行送り 60 で本文 y=509、その 1 行上のルビが y=449)。

**X は行の「中央」。**「左寄せ[上]」は縦だけ上端基準で、横は中央基準。
実測 (X=-620 / サイズ 72 / 10 文字) で、左端ではなく中央が
指定した座標に来た。

| | 指定 | 実際に来た所 |
| --- | --- | --- |
| X=-620 → 絶対 x | 340 (左端のつもり) | 左端 16 / **中央 376** |
| Y=240 → 絶対 y | 780 | 上端 750 |

その為、行の右端も出して中央を指定する。右端は
**ペンの進み** (文字ごとに `PitchX()` だけ進める) で決まるので、
`AribItem::C` に「その文字を書き終えた後のペンの X」を入れて渡す。

**APS の行送りは文字サイズで変わる。**
小型 (SSZ) の行に標準の送りを使うと画面の外を指す。
実測では区切りの行が y=990 になり、540 の字幕平面をはみ出していた
(`88 1C 4F 40` = SSZ + APS 行 15。送りが 60 なら 990、30 なら 510)。

`set_object_item_value()` は失敗しても何も言わないので、
最初の 1 件だけ `get_object_item_value()` で読み返して確かめ、
違っていたらログに出す。

| 前提 | |
| --- | --- |
| テキストの「配置」 | **左寄せ[上]** である事。プリセットで変えているとずれる |
| 行ごとに位置が違う場合 | 一番左・一番上に揃える |
| 微調整 | `[Caption] OffsetX` / `OffsetY` |

#### G3 の初期値はマクロ

**カタカナにしておくと、字幕が全て「メ」で始まる。**
実測した NHK の字幕は `1D 61` (SS3 + マクロ 1) で始まっており、
G3 をカタカナにしていた為に `0x61` が区 5 点 65 =「メ」になっていた。

マクロの中身は**文字集合を割り当てる ESC の並び**で、本文は入っていない。
既定のマクロ 16 個も規格の表なので `arib_gaiji.h` に一緒に生成し、
呼ばれたらその並びを読み直して状態だけ変える。

```
マクロ 1 = 1B 24 42  1B 29 31  1B 2A 30  1B 2B 20 70  0F  1B 7D
           G0=漢字   G1=カナ   G2=かな   G3=マクロ    LS0 LS1R
```

#### 1 バイトの文字集合は区で引けない

ひらがな・カタカナを「JIS X 0208 の区 4 / 区 5」で代用してはいけない。
**末尾に「ー」「。」「、」「「」「」」が入っている**ので、区で引くと落ちる。

```
直す前 : 宇宙ステション   明るいんですよ
直した後: 宇宙ステーション  明るいんですよ。
```

英数も**全角**が正しい。半角で出すと、中型 (MSZ) の `<tw50>` と
重なって細くなり過ぎる。

#### 追加漢字・追加記号 (区 85-94)

**Shift_JIS (CP932) 経由で変換してはいけない。**
区 85-94 は ARIB の追加漢字・追加記号で、CP932 の同じ位置には別の文字が
載っている。以前は CP932 に通していた為、
区 92 点 92 (継続を示す ➡) が **「釗」** になっていた。

機械的に数えた結果 (区点 8836 個):

| | 個数 | |
| --- | --- | --- |
| 一致 | 6949 | |
| **食い違う** | **214** | 90-94 区の記号がほぼ全滅。`〜` が `～` になる等 |
| 表だけにある | **1200** | CP932 に無く、出せていなかった (`♬` 等) |
| CP932 だけ | 173 | 表では未定義。外字と同じ扱いにするのが正しい |
| どちらも無し | 300 | |

対応表は [libaribcaption](https://github.com/xqq/libaribcaption) (MIT) から
借りて `tools/regen-gaiji.sh` で `src/aviutl2/caption/arib_gaiji.h` / `.cpp`
を生成する。**生成物は commit する** (クローン直後にビルドが通る必要がある)。
借りたのは表だけで、コードは借りていない。詳細は `LICENSE.md`。

実測 (放送 1 番組):

```
直す前 : ♬〜  -> <@TSMemoryDRCS><@>〜 (字形が無く四角になる)
         ➡     -> 釗
直した後: 外字の参照が 12 件 -> 0 件
```

#### 色は ARIB の 128 色表で引く

**索引 65 = `(0, 0, 0, α128)` = 半透明の黒**が字幕の背景の正体。
放送は色配列 4 を選び背景に 1 番を使うので `4 * 16 + 1 = 65` になる。
TVCaptionMod2 に「背景の不透明度」の設定があるのは、
このアルファを触る為。

以前は既定の 16 色しか持っておらず、これを「判らない色」として
捨てていた為に背景色が一切出ていなかった。128 色表 (アルファ付き) を
`arib_gaiji` に生成して引く。

**透明 (アルファ 0) は「色が無い」として扱う。**
背景の既定は 8 番 = 透明で、これを黒として出すと何も無い所に黒が付く。

**AviUtl2 のテキストには「背景の箱」が無い。**
制御文字で指定できる 2 つ目の色は影・縁色
(`aviutl2.txt`「色の変更(文字色,影縁色)」) なので、
`[Caption] UseBackColor` はそちらへ流している (実測: `<#00ffff,000000>`)。
プリセット側で文字装飾を縁取りにすると見える。

**アルファは制御文字では表せない。**
半透明の箱そのものを再現するには、字幕の後ろに図形オブジェクトを
置く必要がある (未着手)。

#### リングバッファの窓

DRCS の定義は再送されるが、実測で**間隔の中央値が 14.2 秒**、最大 210 秒。
既定の `MemorySize=10` (8〜14 秒) とほぼ同じで、
**字形の定義が窓に入らない事が半々程度で起こる**。
その場合は `CTSCaptionSource::GetMissingGlyphCount()` が 0 より大きくなり、
ログに「`MemorySize` を大きくすると拾えます」と出る。

### 両方の設定が要る

音声と同じく、**TVTest 側と AviUtl2 側の両方**を有効にしないと届かない。
TVTest 側で `CTsSelector` に落とされると後段では取り返せない。

    TSMemory.ini (TVTest 側)      TSMemory-TVTestSrc.ini (AviUtl2 側)
    [Settings]                    [Caption]
    Subtitle=1                    Enable=1

`GetTargetStreams()` に `CTsSelector::STREAM_SUBTITLE` (stream_type 0x06)
を足している。`tests/test_selector.cpp` で「頼めば通る/頼まなければ落ちる」
の両方を確認している。

### 残っている作業

- 背景の箱は作れていない (上記)。位置と文字の大きさが取れたので、
  字幕の後ろに図形オブジェクトを置く形なら再現できる見込み。
- ルビは本文と同じ行に並べて出している。AviUtl2 には
  `</>漢字<!>ふりがな</>` があるので、X 座標の重なりから
  本文とルビを対応付ければ本来の形にできる。
- 文字スーパー (component_tag 0x38〜0x3F) は取っていない。

## 制限・既知の注意点

- 64bit 版では MPEG-2 デコードの SIMD 最適化が効きません (上記参照)。
  1920x1080 のフレームで 50〜300ms 程度です (静止画キャプチャ用途なら十分)。
- **音声は既定で無効です。** 対応済みですが、有効にするには
  **TVTest 側と AviUtl2 側の両方**の設定が要ります
  (詳細は上の「9. 音声 (既定は無効)」)。

      TSMemory.ini (TVTest 側)      TSMemory-TVTestSrc.ini (AviUtl2 側)
      [Settings]                    [M2V]
      Audio=1                       audio=1

  m2v 自身の音声デコーダは使っていません
  (Program Stream 専用かつ MPEG-1/2 Audio Layer II 専用で、
  日本の地上波の AAC も 0 バイトのダミーファイルも扱えない為)。
  `src/aviutl2/audio/` に別途用意し、AAC の復号は Windows 標準の
  Media Foundation に任せています。
  片方だけ有効にした場合の挙動は「片方だけ設定した場合」を参照。
- **字幕は既定で無効です。** 有効にするには**両方**の設定が要ります
  (`TSMemory.ini` の `[Settings] Subtitle=1` と
  `TSMemory-TVTestSrc.ini` の `[Caption] Enable=1`)。詳細は上の
  「字幕対応」。絶対位置指定と文字スーパーは未対応です。
- スクランブルされたパケットは捨てられます。TVTest 側で解除された状態で
  受け取る必要があります。
- **遡れる長さは `MemorySize` (既定 10MB) が上限です。**
  1080i で 10MB はおおよそ 8〜14 秒分にしかなりません (ビットレート次第)。
  長く遡りたい場合は TVTest 側の `TSMemory.ini` で大きくしてください。
  また溜め込みはプラグインを有効にした時点から始まるので、
  有効にした直後にキャプチャするとその分しか取れません。
- **`MemorySize` に指定できる上限は 4096 (MB) です。**
  共有メモリの大きさは `BufferInfo` も IPC も `DWORD` で扱っており、
  `CreateFileMapping()` に渡す時点で 32bit に収める必要があります。

  | `MemorySize` | パケット数 | バイト数 | |
  | --- | --- | --- | --- |
  | 4095 | 22,839,993 | 4,293,918,684 | そのまま |
  | **4096** | 22,845,570 | 4,294,967,160 | **そのまま (境界)** |
  | 4097 | 22,851,148 | 4,296,015,824 | 制限される |

  上限は `(0xFFFFFFFF - sizeof(BufferInfo)) / 188` = 22,845,570 パケット
  = 4,294,967,160 バイト (4095.99987 MB) です。
  `MemorySize=4096` はここにちょうど収まります
  (4096 × 1048576 ÷ 188 を切り捨てると同じ値になる為)。

  制限が無いと `MemorySize=4097` で必要 4,296,015,840 バイトに対して
  実際には 1,048,544 バイトしか確保されず、確保した範囲の外へ
  書き込む事になります。超える値は上限に切り詰めてログに残します。

  実測 (`TVTInitialize()` を呼んでログを見る):

  ```
  1 / 10 / 1024 / 4094 / 4095 / 4096   -> そのまま
  4097 / 4098 / 8192 / 99999           -> 制限される
  ```
- キャプチャの前後は、切れた位置の GOP が途中で終わっているぶん短くなります。
  実測では**末尾が 0.2 秒程度、先頭が 0.4 秒程度**です。
- **TvtPlay でファイル再生している場合、シークバーの位置と実際に TVTest へ
  届いている位置はずれます。** TvtPlay はパイプへ先読みして書き込むので、
  シークバーが 30 秒を指していても TVTest が受け取っているのは数秒手前です。
  TSMemory が取り込めるのは「TVTest に届いた分」までなので、
  シークバーを基準にすると数秒足りなく見えます (実放送では起きません)。
- `SnapshotCount` を超えて古くなったスナップショットは共有メモリごと破棄
  されるため、タイムラインに残っているオブジェクトを再読み込みしようとしても
  映像が出なくなります。
- AviUtl2 は 1 プロセスのみ待ち受けます。複数の AviUtl2 を起動している場合、
  先に起動したほうが TVTest からの要求を受け取ります。
- マルチ編成では**視聴中の 1 チャンネルのみ**が取り込み対象です
  (全チャンネルを別レイヤーに、という方式は採っていません。上記参照)。
  チャンネルを切り替えた時点でリングバッファは破棄されるので、
  切り替え直後は遡れる長さが短くなります。

---

## ライセンス・謝辞

- オリジナルの TSMemory は Public Domain です。
- `src/m2v` は茂木和洋氏の **MPEG-2 VIDEO VFAPI Plug-In (m2v)** を基にしています。
  有用なプログラムを公開・改良してくださっている茂木氏に感謝します。
- `src/tvtp/BonTsEngine` は TVTest (BonTsEngine) に由来します。
- `sdk/aviutl2` は ＫＥＮくん氏の **AviUtl ExEdit2 Plugin SDK** (MIT License) です。
