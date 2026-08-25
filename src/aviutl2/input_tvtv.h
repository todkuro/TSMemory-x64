#pragma once

struct INPUT_PLUGIN_TABLE;

//	*.tvtv 用入力プラグイン構造体を返す
INPUT_PLUGIN_TABLE *TSMemoryGetInputPluginTable();

//	開いているハンドルの管理を開始する
void TSMemoryInputInitialize();

//	開きっぱなしのハンドルを全て閉じる。
//
//	m2v はファイルを開くとデコード用のスレッドを起動する為、ハンドルを
//	開いたままプラグインをアンロードされるとスレッドのコードがアンマップ
//	されて落ちる。アンロード前に必ず呼ぶ事。
//	戻り値は閉じたハンドルの数。
int TSMemoryInputUninitialize();
