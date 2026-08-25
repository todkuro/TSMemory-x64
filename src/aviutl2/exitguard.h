#pragma once

struct HOST_APP_TABLE;
struct LOG_HANDLE;

//	終了時の「現在の編集データは更新されています」の自動応答。
//
//	TSMemory がタイムラインにオブジェクトを置くと編集済み扱いになり、
//	AviUtl2 の終了時に保存の確認が出る。キャプチャを見るだけの用途では
//	毎回これが出るのは煩わしいので、設定で自動応答出来るようにする。
//
//	※ AviUtl2 には編集済みフラグを解除する API が無い為、ダイアログを
//	   検出して応答するという本体の動作に割り込む方法しか無い。
//	   その為、既定では無効で、TSMemory が置いた以外の編集が
//	   検出された場合は応答しない。
bool TSMemoryExitGuardStart(HOST_APP_TABLE *host, LOG_HANDLE *logger, LPCWSTR ini_file);

//	TSMemory がオブジェクトを配置した事を伝える
void TSMemoryExitGuardNotifyPlaced();

void TSMemoryExitGuardStop();
