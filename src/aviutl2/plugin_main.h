#pragma once

//	プラグインの設定ファイル (TSMemory-TVTestSrc.aux2 と同じ場所の TSMemory-TVTestSrc.ini) のパス。
//	InitializePlugin() / RegisterPlugin() で設定される。
LPCWSTR TSMemoryGetIniFileName();

//	このプラグイン DLL のインスタンスハンドル。
//	ウィンドウクラスはこれで登録する事 (アンロード時に確実に登録解除する為)。
HINSTANCE TSMemoryGetModuleHandle();

//	AviUtl2 のログへ出す。
//	ロガーが未設定 (InitializeLogger より前 / 提供されない) 場合は何もしない。
void TSMemoryLog(LPCWSTR pszMessage);
void TSMemoryLogWarn(LPCWSTR pszMessage);
