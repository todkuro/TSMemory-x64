//----------------------------------------------------------------------------
//	TSMemory : TVTest プラグイン <-> AviUtl ExEdit2 プラグイン間の連携定義
//----------------------------------------------------------------------------
//
//	AviUtl 1.xx 版では TVTest プラグイン側が AviUtl のメインウィンドウを
//	ウィンドウクラス名 "AviUtl" で探して WM_DROPFILES を投げていたが、
//	AviUtl ExEdit2 はファイルの D&D を OLE (IDropTarget) で受けており
//	WM_DROPFILES を投げても何も起こらない。
//
//	そこで 64bit 版では AviUtl2 側のプラグイン (TSMemory-TVTestSrc.aux2) が
//	下記の名前付きオブジェクトを作って待ち受け、TVTest 側はそこへ
//	読み込ませたいダミーファイル (*.tvtv) のパスを渡す方式にしている。
//
//	  TSMemoryBridge.Ready     : ミューテックス
//	                             AviUtl2 側プラグインの生存確認用。
//	                             OpenMutex 出来れば AviUtl2 が起動中。
//	  TSMemoryBridge.Param     : ファイルマッピング (TSMEMORY_REQUEST)
//	  TSMemoryBridge.ParamLock : 上記への排他用ミューテックス
//	  TSMemoryBridge.Request   : 自動リセットイベント
//	                             パラメータ書き込み後に TVTest 側が set する
//
#pragma once

#include <windows.h>

//	名前は TCHAR に依存させず常に Unicode で扱う
//	(TVTest 側と AviUtl2 側で UNICODE の定義状態が違っても食い違わないように)
#define TSMEMORY_IPC_READY_MUTEX	L"TSMemoryBridge.Ready"
#define TSMEMORY_IPC_PARAM_MAP		L"TSMemoryBridge.Param"
#define TSMEMORY_IPC_PARAM_MUTEX	L"TSMemoryBridge.ParamLock"
#define TSMEMORY_IPC_REQUEST_EVENT	L"TSMemoryBridge.Request"

#define TSMEMORY_IPC_VERSION		1

struct TSMEMORY_REQUEST {
	DWORD	Version;			// TSMEMORY_IPC_VERSION
	DWORD	Serial;				// 要求ごとに増える通し番号
	WCHAR	FileName[MAX_PATH];	// 読み込ませる .tvtv のフルパス
};

//	NULL DACL のセキュリティ属性を用意する
//	(TVTest と AviUtl2 の整合性レベルが違っても開けるようにするため)
static inline void TSMemoryInitSecurityAttributes(SECURITY_DESCRIPTOR *psd, SECURITY_ATTRIBUTES *psa)
{
	::ZeroMemory(psd, sizeof(*psd));
	::InitializeSecurityDescriptor(psd, SECURITY_DESCRIPTOR_REVISION);
	::SetSecurityDescriptorDacl(psd, TRUE, NULL, FALSE);
	::ZeroMemory(psa, sizeof(*psa));
	psa->nLength = sizeof(*psa);
	psa->lpSecurityDescriptor = psd;
	psa->bInheritHandle = FALSE;
}
