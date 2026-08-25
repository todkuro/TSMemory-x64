#pragma once

struct HOST_APP_TABLE;
struct EDIT_HANDLE;
struct LOG_HANDLE;
struct CONFIG_HANDLE;

//	キャプチャ・ユーティリティのウィンドウを登録する
bool TSMemoryCaptureRegister(HOST_APP_TABLE *host, EDIT_HANDLE *edit,
							 LOG_HANDLE *logger, CONFIG_HANDLE *config,
							 LPCWSTR ini_file);

//	設定を ini に書き戻して後始末する
void TSMemoryCaptureUninitialize();
