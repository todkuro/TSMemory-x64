#pragma once

struct HOST_APP_TABLE;
struct EDIT_HANDLE;
struct LOG_HANDLE;

//	TVTest 側からの読み込み要求を待ち受けるスレッドを開始する
bool TSMemoryBridgeStart(HOST_APP_TABLE *host, EDIT_HANDLE *edit, LOG_HANDLE *logger,
						 LPCWSTR ini_file);

//	待ち受けを終了する
void TSMemoryBridgeStop();
