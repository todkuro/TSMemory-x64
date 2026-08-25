# 更新履歴

このファイルが**版の唯一の正**です。
`tools/build.sh` が一番上の `## ver.X.Y.Z` を読み取り、

- AviUtl2 のプラグイン一覧に出る版 (`TSMEMORY_VERSION_TEXT`)
- パッケージの `package.ini` / `package.txt`

に流し込みます。**リリース時に書き換えるのはこのファイルだけです。**

## ver.0.3.0 (64bit / AviUtl ExEdit2 対応版)

- 64bit ビルドに対応し、AviUtl ExEdit2 で動作するようにした
- AviUtl 側の `TVTestSrc.aui` と `CaptureUtil.auf` を、AviUtl2 の汎用
  プラグイン `TSMemory-TVTestSrc.aux2` ひとつにまとめた
- 画像の保存に Windows 標準の WIC を使うようにした
  (`TVTest_Image.dll` が不要になり、png / jpeg / bmp / tiff に対応)
- マルチ編成でサブチャンネルを視聴している時に、そのチャンネルの映像を
  取り込むようにした (従来はプライマリチャンネルの映像しか取れなかった)
- 取り込んだ映像にフィルタのプリセットを自動適用する設定を追加した
- 終了時の保存確認に自動応答する設定を追加した
- 実行の度に別名の共有メモリを作るようにした (AviUtl2 のキャッシュ対策)
- 取り込み後のシーク位置と、配置先レイヤーのロックを設定できるようにした
- 音声 (AAC) の取り込みに対応した (既定は無効)

---

ver.0.2.1 以前の更新履歴は、AviUtl 1.xx 版の `TSMemory.txt` を参照してください。
