# 音声取り込み対応のための作業メモ

TSMemory 64bit 版 (AviUtl ExEdit2 対応) に音声の取り込みを追加する場合の
調査結果と作業項目。**2026-08-20 時点の調査メモ**で、この文書は
着手する際にそのまま作業に入れるようにする為に書いた物です。

> # 実装しました (2026-08-24)
>
> この文書は**着手前の調査メモ**です。実装後の状況は
> [development.md](development.md) の「音声」を参照してください。
> ここには調査の経緯と、実装で判った差分だけを残しています。
>
> 実装の要点:
>
> | 項目 | 調査時の想定 | 実装 |
> | --- | --- | --- |
> | 置き場所 | 未定 | **`src/aviutl2/audio/`** に分離。入力プラグインからは `tvtv_audio.h` だけを見る |
> | デコード | 都度復号 + シーク時に再同期 | **開いた時に全部復号**。任意位置の読み出しが memcpy になり、7-3 の難所が消えた |
> | 入力型の設定 | 自分で組み立てる | **MFT が列挙する型を土台にする**。自作の型は `MF_E_INVALIDMEDIATYPE` で弾かれた |
> | デコーダの取得 | `CLSID_CMSAACDecMFT` | **`MFTEnumEx`**。CLSID 直指定は `REGDB_E_CLASSNOTREG` になった |
> | A/V 同期 | 案 A / 案 B のどちらか | **案 A** で実装。実測 +0.750 秒 |
>
> **実機で音声が出る事も確認済みです (2026-08-24)。**
> 案 B (m2v に PTS を通す) は不要でした。

調査時点の到達点は「AAC-LC のまま共有メモリに載せて、AviUtl2 側で
Media Foundation にデコードさせる」方針が妥当と判明したところまで。
**この方針のまま実装して動きました。**

有効化は両側の設定が要ります。片方だけでは何も起きません。

| 側 | 設定 | 既定 |
| --- | --- | --- |
| TVTest | `[Settings] Audio` | `0` |
| AviUtl2 | `[M2V] audio` | `0` |

---

## 1. 結論

**可能。ただし m2v の音声デコーダは一切流用できず、音声パスを新規に作る。**

- TVTest 側は 1 行の変更で音声を共有メモリに載せられる
- AviUtl2 側の入力プラグインは既に音声用のインターフェースを実装済みで、
  差し替えるのは「デコーダのラッパ」1 クラスだけ
- デコーダは Windows 標準の Media Foundation を使えば追加 DLL は不要
  (画像保存で WIC を使ったのと同じ考え方)
- **最大の難所は A/V 同期**。詳細は「6. A/V 同期 (最大の課題)」を参照

---

## 2. なぜ今は音声が入らないのか

### 2-1. TVTest 側で捨てている

`src/tvtp/TSMemory.cpp` の `CTSMemory::UpdateTargetService()`:

```cpp
m_TsSelector.SetTargetServiceID(ServiceID, CTsSelector::STREAM_MPEG2VIDEO);
```

`CTsSelector` のフラグ (`src/tvtp/BonTsEngine/TsSelector.h`) には
`STREAM_AAC = 0x00000010` が既にあるので、

```cpp
m_TsSelector.SetTargetServiceID(
    ServiceID, CTsSelector::STREAM_MPEG2VIDEO | CTsSelector::STREAM_AAC);
```

とするだけで音声 PID が共有メモリに載る。**ここは 1 行**。

`TsSelector.cpp` のストリーム選択部は

```cpp
static const BYTE StreamTypeList [] = { 0x01, 0x02, 0x06, 0x0D, 0x0F, 0x1B };
bTarget = (pThis->m_TargetStream & (1 << j)) != 0;
```

となっていて、フラグのビット位置がこの配列の添字に対応する。
`STREAM_AAC` は bit 4 なので **stream_type 0x0F** に対応し、
地上波の AAC 音声 (3 章参照) がそのまま選択される。

> **PMT は素通しされる。** `CTsSelector` には `MakePat` はあるが
> `MakePmt` は無く、PMT は書き換えられずにそのまま流れる。
> その為、aux2 側のデマルチプレクサは**元の PMT を読んで音声 PID を
> 特定できる** (7-2 の作業項目はこれを前提にしている)。
> PMT には除外された字幕・データ放送の PID も載ったままなので、
> stream_type で絞る事。

> **ライセンス上の補足**: 音声対応で新規に作る物は全て aux2 側に入る。
> aux2 は BonTsEngine (GPL-2.0-or-later) をリンクしていない為、
> 音声対応で GPL の適用範囲は広がらない。TVTest 側の依存は
> 引き続き `CTsSelector` の 1 機能のみ。

### 2-2. m2v の音声デコーダが使えない (壁が 3 つ)

`src/m2v/audio_stream.c` / `src/m2v/mpeg_audio.c` を読んだ結果、
独立した理由が 3 つあり、どれか 1 つを直しても動かない。

| # | 場所 | 内容 |
| --- | --- | --- |
| 1 | `audio_stream_open()` | `check_ps()` → `open_ps()` で **Program Stream 専用**。TS を渡すと `r->stream = 0` になり NULL を返す |
| 2 | `audio_stream_open()` | `_open(path, ...)` で**実ファイル**を読む。`.tvtv` は 0 バイトのダミーなので何も読めない (映像側は `multi_file.c` が `_open` を `open_shared_memory` に `#define` で差し替えているが、音声側にはその仕掛けが無い) |
| 3 | `mpeg_audio.c` | `parse_layer2_header()` / `decode_layer2()` で **MPEG-1/2 Audio Layer II 専用**。日本の地上波は AAC なので原理的に扱えない |

3 番目が決定的。**パッチで済む話ではないので、m2v の音声側には手を入れず
丸ごと別実装にする。**

---

## 3. 実データで確認した事実

実際の放送 TS (TOKYO MX のフル TS) の service 23608 (MX1) で確認。

> この TS は放送された物で再配布出来ない為、リポジトリには入っていません
> (`development.md` の「テスト用の TS」を参照)。ここの確認に使う TS は
> マルチ編成の実 TS であれば何でも構いません。

### サービス構成 (`compilers/python/python.exe tests/tools/ts-services.py <ts>`)

```
- service_id=23608 (0x5C38)  PMT PID=0x0101
    PID 0x0111  MPEG-2 Video  <-- video
    PID 0x0112  AAC
    PID 0x0114  private (caption etc.)
    ...
```

### 音声 PID 0x0112 の中身

PES を剥がして ADTS の同期を 8 フレーム分連鎖検証した結果:

```
elementary stream : 289624 bytes
ADTS chain starts at offset 516
  profile        : LC
  sampling rate  : 48000 Hz
  channel config : 2
  CRC            : yes
  frames         : 425  -> 9.07 sec
```

**AAC-LC / 48000 Hz / ステレオ / ADTS フレーム / CRC あり。**
Media Foundation の AAC デコーダがそのまま扱える標準的な構成。

> 検証に使った使い捨てスクリプトは残していない。再確認する場合は
> PES ヘッダ (`00 00 01` + stream_id、`payload[8]` がヘッダ長) を剥がして
> ES を連結し、ADTS 同期語 `FFFx` を**連鎖で**検証すること。
> 単発の同期語検出だと音声データ中の偶然の `FFFx` を拾って
> 「SSR / 8000Hz / 7ch」のような明らかにおかしい値になる (実際に一度踏んだ)。

---

## 4. AviUtl2 側の受け皿は既にある

`src/aviutl2/input_tvtv.cpp` は既に音声対応の形になっている。

- `INPUT_PLUGIN_TABLE` に `FLAG_AUDIO` を宣言済み
- `func_read_audio` を配線済み
- `CTvtvFile::GetInfo()` が `INPUT_INFO::FLAG_AUDIO` / `audio_n` /
  `audio_format` (`WAVEFORMATEX`) を埋める実装済み
- `CTvtvFile::ReadAudio()` が実装済み

現状は `M2A` クラス (= `open_mpeg_audio()` のラッパ) が必ず NULL になる為、
`m_Audio != nullptr` が偽になって音声なし扱いになっているだけ。

**つまり `M2A` を新クラスに差し替えるだけで済む。** 新クラスに必要な形:

```cpp
class CAacAudio {
public:
    explicit CAacAudio(const char *file);     // 共有メモリ名 = ファイル名
    bool IsValid() const;
    int  GetChannels() const;                 // WAVEFORMATEX::nChannels
    int  GetSampleRate() const;               // WAVEFORMATEX::nSamplesPerSec
    int64_t GetTotalSamples() const;          // INPUT_INFO::audio_n
    int  Read(int start, int length, void *buf);   // func_read_audio
};
```

`CTvtvFile` 側の変更は `m_Audio` の型と、`m_wfex` を埋める数行のみ。

---

## 5. 設計方針

```
TVTest 側                          AviUtl2 側 (TSMemory-TVTestSrc.aux2)
─────────                          ──────────────────────────────────
CTsSelector                        open_shared_memory("tsmemoryN_M.tvtv")
  STREAM_MPEG2VIDEO                        │
  | STREAM_AAC        ──> 共有メモリ ──────┤
                                           ├─> m2v (映像) …既存のまま
                                           │
                                           └─> 新規: TS demux (音声 PID)
                                                 ├ PMT から音声 PID を特定
                                                 ├ PES 分解 → ADTS フレーム
                                                 ├ インデックス作成
                                                 └ Media Foundation AAC → PCM
```

### 5-1. TS の読み出しは既存コードを再利用できる

`src/m2v/shared_memory.h` の API はそのまま C++ から呼べる。

```c
intptr_t open_shared_memory(const char *name);   /* name はファイル名でよい */
int      shm_read(intptr_t id, void *buf, int length);
__int64  shm_seek(intptr_t id, __int64 offset, int origin);
__int64  shm_tell(intptr_t id);
int      shm_close(intptr_t id);
```

`open_shared_memory()` は名前付きミューテックスと file mapping を開き、
**リングバッファを線形化して malloc したバッファにコピーして返す**
(`shared_memory.c` を参照)。64bit 化済みでテスト (`tests/test_shm.c`) もある。

> **注意**: 映像用に m2v が既に 1 回コピーしている。音声側でも
> `open_shared_memory()` を呼ぶと同じ TS をもう 1 回コピーする事になる
> (既定 `MemorySize=10MB` なので実害は小さいが、`MemorySize` を大きく
> している環境では効いてくる)。気になるなら共有メモリを 1 回だけ読んで
> 映像・音声で共有する作りにする。ただし m2v 側の口を開ける必要があり、
> 手間の割に得は少ない。**初手は素直に 2 回読む**のがよい。

### 5-2. AAC デコーダは Media Foundation

- MFT: `CLSID_CMSAACDecMFT` (Windows 7 以降に標準搭載、追加 DLL 不要)
- 入力メディアタイプ: `MFAudioFormat_AAC` +
  `HEAACWAVEFORMAT` (`wPayloadType = 1` で ADTS を直接渡せる)
- 出力: `MFAudioFormat_PCM` 16bit
- リンクに `-lmfplat -lmfuuid -lmfreadwrite -lole32` が要る
  (`tools/build.sh` の aux2 のリンク行に追加)

`MFStartup()` / `MFShutdown()` の呼び出し場所に注意。
`plugin_main.cpp` の `InitializePlugin()` / `UninitializePlugin()` で
やるのが素直だが、**アンロード時の後始末を誤ると
`TSMemory-TVTestSrc.aux2_unloaded` で落ちる**
([development.md](development.md) の「更新時の注意」と過去の事例を参照)。

代替案として `IMFSourceReader` に TS ごと食わせる手もあるが、
共有メモリ上のバイト列を渡すのに `IMFByteStream` の自前実装が要り、
かつ ARIB の TS を MF が正しく開ける保証が無い。**MFT を直接叩く方が確実。**

---

## 6. A/V 同期 (最大の課題)

### 6-1. 何が問題か

リングバッファは GOP の途中で切れるので、**映像と音声で開始位置が違う**。
実測では先頭が 0.4 秒程度、末尾が 0.2 秒程度欠ける
([development.md](development.md) の「制限・既知の注意点」)。単純に「音声の先頭 = 映像の先頭」と
すると最大 0.4 秒ずれる。

### 6-2. m2v は PTS を捨てている (重要)

調査の結果:

- `src/m2v/transport_stream.c` には **PTS を扱うコードが一切無い**。
  TS → ES に落とす際にタイムスタンプを捨てている
  (`grep -n "pts" src/m2v/transport_stream.c` が空)
- `MPEG_VIDEO` 構造体 (`src/m2v/mpeg_video.h`) にも PTS のフィールドは無い
- `pes.c` に `extract_pes_pts_dts()` はあるが、これを使うのは
  Program Stream 経路 (`program_stream.c` / `audio_stream.c`) だけ

**つまり「映像フレーム 0 の PTS」を m2v から取得する手段が無い。**
ここが同期実装の肝。

### 6-3. 対処案

**案 A (推奨): 自前スキャンで映像の開始 PTS を求める**

音声用の demux を作る際、ついでに映像 PID も走査して
「ES のバイトオフセット ↔ それを含む PES の PTS」の対応を持つ。
その上で:

1. 映像 ES の先頭から最初のシーケンスヘッダ `00 00 01 B3` を探す
2. そのオフセットを含む PES の PTS を映像の開始 PTS とする
   (m2v も最初の GOP から復号を始めるので、ここが frame 0 に対応する)
3. 音声の最初の ADTS フレームの PTS との差を求める
4. 差の分だけ音声の先頭に無音を詰める / または音声を切り落とす

`GOP` 構造体 (`src/m2v/gop.h`) が持つ `offset` は ES のバイトオフセット
なので、上記 1〜2 の考え方は m2v の GOP リストと整合する。

> **要検証**: m2v が壊れた先頭 GOP を読み飛ばす場合、自前スキャンの
> 「最初のシーケンスヘッダ」と m2v の frame 0 がずれる可能性がある。
> 実装後に必ず実測で突き合わせる (テスト計画 8-3 を参照)。

**案 B (正確だが侵襲的): m2v に PTS を通す**

`transport_stream.c` に PES の PTS を保持させ、`MPEG_VIDEO` に
「frame 0 の PTS」を持たせる。正確だが `tools/patch64.py` に
大きめのパッチが増える。案 A で実測誤差が許容できない場合の保険。

### 6-4. 同期の計算

```
offset_sec = (video_start_pts - audio_start_pts) / 90000.0
```

- `offset_sec > 0` … 音声の方が先に始まっている → 音声の先頭を捨てる
- `offset_sec < 0` … 映像の方が先に始まっている → 音声の先頭に無音を詰める

PTS は 33bit で折り返す (`1 << 33`) 為、差を取る前に折り返し処理が要る。
`tests/ts_pts.h` の `PtsDiffSeconds()` が同じ処理をしているので流用できる。

---

## 7. 作業項目

**2026-08-24 に全て実装しました。** 以下は着手時のチェックリストで、
実装がどこに入ったかと、想定と違った点を添えてあります。

### 7-1. TVTest 側 — 済

- [x] `UpdateTargetService()` に `STREAM_AAC` を追加 → `GetTargetStreams()`
- [x] `[Settings] Audio=0/1` を追加 (既定 0)
- [x] `tests/test_selector.cpp` に「音声 PID も残る / 落ちる」の確認を追加
      → 合成 TS に stream_type 0x0F の PID を足して実測

### 7-2. AviUtl2 側 — TS demux → `src/aviutl2/audio/ts_audio.cpp`

- [x] PAT / PMT を読んで音声 PID を特定 (セクションの再組み立て込み)
- [x] PES を分解して ADTS フレーム列に
- [x] ADTS ヘッダの解析 (同期語は**連鎖で**検証)
- [x] `フレーム番号 → (ESオフセット, 開始サンプル, PTS)` の索引

> 映像の開始 PTS も**同じ 1 周で**拾います。当初は別走査の想定でした。

### 7-3. AviUtl2 側 — デコード → `src/aviutl2/audio/aac_decoder.cpp`

- [x] MFT の初期化 / 入出力メディアタイプ
      → **`CLSID_CMSAACDecMFT` 直指定は使えず** `MFTEnumEx` で探す。
      入力型も**自作すると弾かれる**ので MFT が列挙する型を土台にする
- [x] ADTS を流し込んで PCM を得る
      → `MF_E_TRANSFORM_STREAM_CHANGE` での出力型の再交渉が要る
- [x] ~~シーク対応 (MFT を flush して数フレーム手前から復号し直す)~~
      → **不要になりました。** 開いた時に全部復号して PCM を持つ方式に
      した為、任意位置の読み出しは memcpy です。
      当初「最大の難所」に挙げていた所が丸ごと消えました
- [x] `MFStartup()` / `MFShutdown()` の寿命管理
      → `DecodeAll()` の中で対にする。プロセス単位で参照カウントされる
      為、AviUtl2 本体が MF を使っていても干渉しません

### 7-4. AviUtl2 側 — 結線 — 済

- [x] `M2A` を `CTvtvAudio` に差し替え (`audio/tvtv_audio.cpp`)
- [x] `m_wfex` を埋める
- [x] 同期オフセットの反映
- [x] `tools/build.sh` にビルド対象とリンク (`-lmfplat -lmfuuid`) を追加

### 7-5. ドキュメント — 済

- [x] README.md / development.md の「音声は取得されません」を差し替え
- [x] 両方の ini に設定の説明を追加

### 7-6. 着手時には無かった仕掛けへの対応 — 済

- [x] `open_timeout` に収まる事
      → 実測でデマルチプレクサ 15ms / 21MB、復号を足しても 400ms 程度
- [x] 諦めたスレッドが自分で後始末する事 → `OpenRequest` の参照カウント
- [x] `ToAnsiPath()` がファイル名だけを渡す事がある
      → `CTvtvAudio::Open()` は共有メモリ名としてファイル名部分だけを使う
- [x] 壊れた TS への耐性 → `test_fuzz` を `audio=1` で通して確認
- [x] `test_fuzz` に音声経路 (`func_read_audio`) を通す

## 8. テスト計画

既存のテストの作りに合わせる (`tests/tools/test.sh` に足す)。

### 8-1. `tests/test_adts.cpp` (新規)

マルチ編成の TS から音声 PID を取り出し、

- PMT から音声 PID が正しく特定できる事 (MX1 = 0x0112)
- ADTS の連鎖が取れる事、パラメータが AAC-LC / 48000Hz / 2ch である事
- フレーム数 × 1024 が PTS から求まる長さと概ね一致する事

### 8-2. `tests/test_aac_decode.cpp` (新規)

- MFT で PCM に落として WAV に書き出す
- 全サンプルが 0 でない事 (無音でない)
- 任意位置シーク後の内容が、先頭から連続して読んだ場合と一致する事
  (**シーク実装のバグを一番よく捕まえる**)

### 8-3. A/V 同期の実測 (最重要)

- `tests/test_multich.cpp` と同じ枠組みで、映像 frame 0 の PTS と
  音声先頭の PTS を突き合わせる
- **6-3 の案 A の前提** (自前スキャンの最初のシーケンスヘッダ = m2v の frame 0)
  が成り立っているかをここで確認する。ずれていたら案 B に切り替える
- 実機では「時報」「拍手」など音の立ち上がりが明確な素材で目視/耳で確認する

---

## 9. 考慮が必要なケース

| ケース | 内容 |
| --- | --- |
| 5.1ch 放送 | channel config 6。そのまま 6ch で渡すか 2ch にダウンミックスするか要判断。AviUtl2 のシーンの音声設定との兼ね合いもある |
| デュアルモノ (二カ国語) | 1 本の AAC に主音声/副音声が入る ARIB 固有の形式。MF は主/副を分離しない。どちらを出すか、あるいは非対応とするか要判断 |
| 音声のみ / 映像のみ | 片方しか取れない場合に `INPUT_INFO::flag` を正しく立てる (`CTvtvFile::IsValid()` は既に `||` になっている) |
| サンプリングレート | ARIB は 48000Hz 固定なのでリサンプルは不要。ただし AviUtl2 のシーンのサンプリングレートが異なる場合の挙動は要確認 (`EDIT_SECTION::set_scene_sample_rate()` がある) |
| リングバッファの消費 | 音声 ~0.2Mbps に対し映像 ~15Mbps。遡れる秒数は 1〜2% 減る程度で実用上は無視できる |
| 音声が無いサービス | データ放送のみのサービス等。音声 PID が見つからない場合に落ちない事 |
| スクランブル | 解除済みのものが来る前提 (映像と同じ) |

---

## 10. 参考: 再利用できる既存コード

| 用途 | 場所 |
| --- | --- |
| 共有メモリの読み出し | `src/m2v/shared_memory.h` (`open_shared_memory` ほか)。C++ から直接呼べる |
| PAT / PMT の解析 (移植元) | `tests/tools/ts-services.py`。セクション再組み立て込み |
| PES の PTS 取り出し | `tests/ts_pts.h` (`GetPacketVideoPts` / `PtsDiffSeconds`)。33bit 折り返しの処理あり |
| サービス単位の TS 切り出し | `tests/test_multich.cpp` の `FilterService()` |
| 合成 TS の組み立て方 | `tests/test_selector.cpp` (PAT/PMT を CRC 込みで手組みしている) |
| 入力プラグインをホスト無しで叩く | `tests/test_decode.cpp` |
| ini の読み方 (UTF-8 対応) | `src/aviutl2/inifile.h` |

---

## 11. 見送りの判断 (2026-08-20)

現状の主な用途が「TVTest から取り込んで、キャプチャ・ユーティリティで
静止画を保存して終わり」である為、音声の必要性が低いと判断して見送り。
オリジナルの TSMemory (AviUtl 1.xx 版) も音声非対応だった。

必要になった場合はこの文書の 7 章のチェックリストから着手する。
最初にやるべきは **8-3 の A/V 同期の実測**で、ここで案 A / 案 B の
どちらを採るかが決まると、残りは手を動かすだけになる。

## 12. 残作業の見積り (2026-08-24)

7 章のうち済んでいるのは配管とスイッチだけで、**本体は手付かず**。

| 塊 | 中身 | 目安 |
| --- | --- | --- |
| TS demux (7-2) | PAT/PMT のセクション再組み立て、PES 分解、ADTS 走査、インデックス構築 | 400〜500 行 |
| AAC デコーダ (7-3) | MFT の初期化、シーク時の再同期、キャッシュ、寿命管理 | 250〜350 行 |
| A/V 同期 (6 章) | 映像の開始 PTS を求めて差を詰める。**ここだけ read-only の調査が先** | 100〜150 行 + 実測 |
| 結線 (7-4) | `M2A` の差し替え、`m_wfex`、ビルド設定 | 50 行 |
| テスト (8 章) | `test_adts` / `test_aac_decode` / 同期の実測 | 400 行 |

合計 **1,200〜1,500 行**程度。今の `src/aviutl2/` が 1,850 行なので、
規模としては小さくない。

**難所は 2 つだけ**で、他は手を動かす作業:

1. **A/V 同期** (6 章)。m2v が PTS を捨てている為、映像の frame 0 が
   どの PTS かを外から求める必要がある。案 A の「自前スキャンで求めた
   最初のシーケンスヘッダ」と m2v の frame 0 が一致するかは**未検証**
2. **シーク時の AAC 再同期** (7-3)。`func_read_audio()` は任意位置を
   要求してくるので、MFT を flush して数フレーム手前から復号し直す。
   ここが遅いと AviUtl2 の操作感に響く

逆に、**楽になっている点**もある:

- TVTest 側は済んでいる (7-1)
- 有効・無効の分岐は既にある (7-4)
- `tests/ts_pts.h` の `PtsDiffSeconds()` が PTS の折り返し処理を持っている
- `tests/tools/ts-services.py` が PAT/PMT の解析を Python で実装済みで、
  移植元にできる
