//----------------------------------------------------------------------------
//	TSMemory for AviUtl ExEdit2 (汎用プラグイン .aux2)
//
//	・*.tvtv (TVTest の共有メモリ) を読み込む入力プラグインを登録する
//	・TVTest プラグイン (TSMemory.tvtp) からの読み込み要求を待ち受ける
//	・キャプチャ・ユーティリティのウィンドウを追加する
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>

//	StringCchPrintfW を使う。lstrcpynW 等は引き続き使うので非推奨化はしない
#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#include "plugin2.h"
#include "input2.h"
#include "logger2.h"
#include "config2.h"

#include "plugin_main.h"
#include "input_tvtv.h"
#include "bridge.h"
#include "capture.h"
#include "exitguard.h"

//	版は CHANGELOG.md が唯一の正で、tools/build.sh が
//	build/generated/tsmemory_version.h を作って渡す。
//	ここに直接書くとリリースの度に食い違う。
#if __has_include("tsmemory_version.h")
#include "tsmemory_version.h"
#endif
#ifndef TSMEMORY_VERSION_TEXT
//	エディタで開いた時など、生成前でも読めるようにする為の控え。
//	ビルドすると必ず上書きされる (build.sh は必ず生成する)
#define TSMEMORY_VERSION_TEXT	L"TSMemory version (unbuilt)"
#endif

namespace {

EDIT_HANDLE *g_pEdit = nullptr;
LOG_HANDLE *g_pLogger = nullptr;
CONFIG_HANDLE *g_pConfig = nullptr;
HINSTANCE g_hinstDLL = nullptr;
WCHAR g_szIniFileName[MAX_PATH] = {};

//	AviUtl2 のプラグイン一覧に出る名前。
//	AviUtl 1.xx 版の AviUtl 側が TVTestSrc という名前だった為、
//	TVTest 側の TSMemory と対にして TSMemory-TVTestSrc としている。
COMMON_PLUGIN_TABLE g_CommonPluginTable = {
	L"TSMemory-TVTestSrc",
	TSMEMORY_VERSION_TEXT,
};

void SetupIniFileName()
{
	::GetModuleFileNameW(g_hinstDLL, g_szIniFileName, MAX_PATH);
	::PathRenameExtensionW(g_szIniFileName, L".ini");
}

}	// namespace

LPCWSTR TSMemoryGetIniFileName()
{
	return g_szIniFileName;
}

HINSTANCE TSMemoryGetModuleHandle()
{
	return g_hinstDLL;
}

//	入力プラグイン等、ロガーを直接持たない所から使う
void TSMemoryLog(LPCWSTR pszMessage)
{
	if (g_pLogger != nullptr && pszMessage != nullptr)
		g_pLogger->log(g_pLogger, pszMessage);
}

void TSMemoryLogWarn(LPCWSTR pszMessage)
{
	if (g_pLogger != nullptr && pszMessage != nullptr)
		g_pLogger->warn(g_pLogger, pszMessage);
}

//----------------------------------------------------------------------------
//	DLL エントリ
//----------------------------------------------------------------------------
//	m2v 側 (instance_manager.c) にも DllMain があるため、こちらでは定義せず
//	HINSTANCE は GetModuleHandleEx で取得する。
static HINSTANCE GetSelfModule()
{
	HMODULE hModule = nullptr;
	::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
						 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						 reinterpret_cast<LPCWSTR>(&SetupIniFileName), &hModule);
	return hModule;
}

//	自分自身をアンロードされないように固定する
//
//	AviUtl2 は終了時にプラグインを FreeLibrary した後も、登録済みの
//	INPUT_PLUGIN_TABLE の関数ポインタや name / filefilter / information の
//	文字列を参照し続ける。これらは全て DLL の中にある為、アンマップされた
//	領域を実行・参照して落ちる (終了時のクラッシュとして表面化した)。
//	SDK には登録を解除する API が無いので、こちらがアンロードされない
//	ようにするしかない。
//
//	※ 固定するとファイルが掴まれたままになる為、パッケージの入れ直しは
//	   AviUtl2 を終了してから行う必要がある (README に記載済み)。
static void PinSelfModule()
{
	HMODULE hModule = nullptr;
	::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
						 | GET_MODULE_HANDLE_EX_FLAG_PIN,
						 reinterpret_cast<LPCWSTR>(&SetupIniFileName), &hModule);
}

//----------------------------------------------------------------------------
//	必要とする本体のバージョン番号
//----------------------------------------------------------------------------
EXTERN_C __declspec(dllexport) DWORD RequiredVersion()
{
	return 2003300;
}

//----------------------------------------------------------------------------
//	ログ出力機能初期化
//----------------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE *handle)
{
	g_pLogger = handle;
}

//----------------------------------------------------------------------------
//	設定関連初期化
//----------------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE *handle)
{
	g_pConfig = handle;
}

//----------------------------------------------------------------------------
//	プラグイン DLL 初期化
//----------------------------------------------------------------------------
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD /*version*/)
{
	g_hinstDLL = GetSelfModule();
	SetupIniFileName();
	return true;
}

//----------------------------------------------------------------------------
//	プラグイン DLL 終了
//----------------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void UninitializePlugin()
{
	TSMemoryExitGuardStop();
	TSMemoryBridgeStop();
	TSMemoryCaptureUninitialize();

	//	開きっぱなしの入力ハンドルを閉じてデコードスレッドを止める。
	//	これをしないとアンロード後にスレッドが動き続けて落ちる。
	const int Closed = TSMemoryInputUninitialize();
	if (Closed > 0 && g_pLogger != nullptr) {
		WCHAR szMessage[128];
		::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage), L"TSMemory: 開いたままの %d 個の入力を閉じました", Closed);
		g_pLogger->log(g_pLogger, szMessage);
	}
}

//----------------------------------------------------------------------------
//	汎用プラグイン構造体
//----------------------------------------------------------------------------
EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE *GetCommonPluginTable(void)
{
	return &g_CommonPluginTable;
}

//----------------------------------------------------------------------------
//	プラグイン登録
//----------------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE *host)
{
	if (g_szIniFileName[0] == L'\0') {
		g_hinstDLL = GetSelfModule();
		SetupIniFileName();
	}

	//	本体に渡したテーブル・文字列・関数ポインタは、アンロード後も
	//	参照され続ける。登録の前に自分を固定しておく (PinSelfModule 参照)。
	PinSelfModule();

	//	*.tvtv の入力プラグインを登録
	TSMemoryInputInitialize();
	host->register_input_plugin(TSMemoryGetInputPluginTable());

	g_pEdit = host->create_edit_handle();

	//	キャプチャ・ユーティリティのウィンドウを登録
	if (::GetPrivateProfileIntW(L"Capture", L"Enable", 1, g_szIniFileName) != 0)
		TSMemoryCaptureRegister(host, g_pEdit, g_pLogger, g_pConfig, g_szIniFileName);

	//	TVTest からの読み込み要求の待ち受けを開始
	if (::GetPrivateProfileIntW(L"Bridge", L"Enable", 1, g_szIniFileName) != 0)
		TSMemoryBridgeStart(host, g_pEdit, g_pLogger, g_szIniFileName);

	//	終了時の保存確認の自動応答 (既定では無効)
	TSMemoryExitGuardStart(host, g_pLogger, g_szIniFileName);
}
