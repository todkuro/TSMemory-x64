//----------------------------------------------------------------------------
//	TSMemory-TVTestSrc.aux2 を AviUtl ExEdit2 の代わりに読み込んで動作を確認する。
//
//	・DLL がロード出来て必要なエクスポートが揃っているか
//	・RegisterPlugin() で *.tvtv の入力プラグインが登録されるか
//	・TVTest 側と同じ手順で要求を送るとタイムラインへの配置処理まで届くか
//	・存在しない *.tvtv を開こうとしても落ちないか
//----------------------------------------------------------------------------
#include <windows.h>
#include <cstdio>
#include <cstring>

#include "plugin2.h"
#include "input2.h"

#include <shlwapi.h>

#include "tsmemory_ipc.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-52s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

//---------------------------------------------------------------------------
//	ホスト側のふり
//---------------------------------------------------------------------------
INPUT_PLUGIN_TABLE *g_pInputPluginTable = nullptr;
HWND g_hwndWindowClient = nullptr;
LPCWSTR g_pszWindowClientName = nullptr;
LPCWSTR g_pszExportMenuName = nullptr;

EDIT_INFO g_EditInfo = {};
WCHAR g_szCreatedFile[MAX_PATH] = {};
int g_CreatedLayer = -1;
int g_CreatedFrame = -1;
int g_DeletedObjects = 0;
HANDLE g_hCreatedEvent = nullptr;

int g_FakeObject = 0;	//	オブジェクトハンドルの代わり

EDIT_SECTION g_EditSection = {};
EDIT_HANDLE g_EditHandle = {};
HOST_APP_TABLE g_Host = {};

bool fake_is_support_media_file(LPCWSTR file, bool /*strict*/)
{
	//	入力プラグインのファイルフィルタで判定する代わりに拡張子だけ見る
	const size_t len = ::wcslen(file);
	return len > 5 && ::_wcsicmp(file + len - 5, L".tvtv") == 0;
}

OBJECT_HANDLE fake_find_object(int /*layer*/, int /*frame*/)
{
	//	1 回だけ既存オブジェクトがある事にする
	if (g_DeletedObjects == 0)
		return &g_FakeObject;
	return nullptr;
}

void fake_delete_object(OBJECT_HANDLE /*object*/)
{
	g_DeletedObjects++;
}

//	取り込んだ映像の長さ (7.37 秒 = 30000/1001 fps で 221 フレーム相当)
constexpr double MEDIA_TOTAL_TIME = 7.37;
constexpr int MEDIA_EXPECTED_FRAMES = 221;

int g_CreatedLength = -1;

bool fake_get_media_info(LPCWSTR /*file*/, MEDIA_INFO *info, int info_size)
{
	if (info == nullptr || info_size < static_cast<int>(sizeof(MEDIA_INFO)))
		return false;
	info->video_track_num = 1;
	info->audio_track_num = 0;
	info->total_time = MEDIA_TOTAL_TIME;
	info->width = 1920;
	info->height = 1080;
	return true;
}

OBJECT_LAYER_FRAME fake_get_object_layer_frame(OBJECT_HANDLE /*object*/)
{
	OBJECT_LAYER_FRAME lf = {};
	lf.layer = g_CreatedLayer;
	lf.start = g_CreatedFrame;
	lf.end = g_CreatedFrame + (g_CreatedLength > 0 ? g_CreatedLength - 1 : 0);
	return lf;
}

OBJECT_HANDLE fake_create_object_from_media_file(LPCWSTR file, int layer, int frame, int length)
{
	::lstrcpynW(g_szCreatedFile, file, MAX_PATH);
	g_CreatedLayer = layer;
	g_CreatedFrame = frame;
	g_CreatedLength = length;
	::SetEvent(g_hCreatedEvent);
	return &g_FakeObject;
}

void fake_set_focus_object(OBJECT_HANDLE) {}
void fake_set_cursor_layer_frame(int, int) {}

bool fake_call_edit_section_param(void *param, void (*func)(void *, EDIT_SECTION *))
{
	func(param, &g_EditSection);
	return true;
}

void fake_get_edit_info(EDIT_INFO *info, int info_size)
{
	::CopyMemory(info, &g_EditInfo, min(static_cast<size_t>(info_size), sizeof(g_EditInfo)));
}

HWND fake_get_host_app_window()
{
	return nullptr;
}

void fake_register_input_plugin(INPUT_PLUGIN_TABLE *table)
{
	g_pInputPluginTable = table;
}

void fake_register_window_client(LPCWSTR name, HWND hwnd)
{
	g_pszWindowClientName = name;
	g_hwndWindowClient = hwnd;
}

void fake_register_export_menu_param(LPCWSTR name, void * /*param*/, void (* /*func*/)(void *))
{
	g_pszExportMenuName = name;
}

//	プロジェクトの初期化完了を通知するコールバック
void (*g_pProjectLoadHandler)(PROJECT_FILE *) = nullptr;

void fake_register_project_load_handler(void (*func)(PROJECT_FILE *))
{
	g_pProjectLoadHandler = func;
}

//	SuppressExitConfirm の値 (0 / 1 / 2)
int ExitGuardMode = 0;

//	終了時の確認の自動応答が有効な時だけ登録されるイベントリスナ
//	(SuppressExitConfirm=2 は編集の有無を見ないので登録されない)
int g_EventListenerCount = 0;
void (*g_pUpdateObjectListener)(void *) = nullptr;
void *g_pUpdateObjectParam = nullptr;

void fake_register_event_listener(EVENT_TYPE type, void *param, void (*func)(void *))
{
	g_EventListenerCount++;
	if (type == EVENT_TYPE::UPDATE_OBJECT) {
		g_pUpdateObjectListener = func;
		g_pUpdateObjectParam = param;
	}
}

EDIT_HANDLE *fake_create_edit_handle()
{
	return &g_EditHandle;
}

void SetupHost()
{
	g_EditSection.info = &g_EditInfo;
	g_EditSection.is_support_media_file = fake_is_support_media_file;
	g_EditSection.find_object = fake_find_object;
	g_EditSection.delete_object = fake_delete_object;
	g_EditSection.create_object_from_media_file = fake_create_object_from_media_file;
	g_EditSection.set_focus_object = fake_set_focus_object;
	g_EditSection.set_cursor_layer_frame = fake_set_cursor_layer_frame;
	g_EditSection.get_media_info = fake_get_media_info;
	g_EditSection.get_object_layer_frame = fake_get_object_layer_frame;

	//	シーンのフレームレート (オブジェクト長の算出に使われる)
	g_EditInfo.rate = 30000;
	g_EditInfo.scale = 1001;

	g_EditHandle.call_edit_section_param = fake_call_edit_section_param;
	g_EditHandle.get_edit_info = fake_get_edit_info;
	g_EditHandle.get_host_app_window = fake_get_host_app_window;

	g_Host.register_input_plugin = fake_register_input_plugin;
	g_Host.register_window_client = fake_register_window_client;
	g_Host.register_export_menu_param = fake_register_export_menu_param;
	g_Host.register_project_load_handler = fake_register_project_load_handler;
	g_Host.register_event_listener = fake_register_event_listener;
	g_Host.create_edit_handle = fake_create_edit_handle;
}

//---------------------------------------------------------------------------
//	TVTest 側と同じ手順で要求を送る
//---------------------------------------------------------------------------
bool SendRequest(LPCWSTR pszFileName)
{
	HANDLE hMap = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, TSMEMORY_IPC_PARAM_MAP);
	HANDLE hMutex = ::OpenMutexW(SYNCHRONIZE, FALSE, TSMEMORY_IPC_PARAM_MUTEX);
	HANDLE hEvent = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, TSMEMORY_IPC_REQUEST_EVENT);
	bool fOK = false;

	if (hMap != nullptr && hMutex != nullptr && hEvent != nullptr) {
		auto *pParam = static_cast<TSMEMORY_REQUEST *>(::MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0));
		if (pParam != nullptr) {
			if (::WaitForSingleObject(hMutex, 5000) == WAIT_OBJECT_0) {
				pParam->Version = TSMEMORY_IPC_VERSION;
				pParam->Serial = 1;
				::lstrcpynW(pParam->FileName, pszFileName, MAX_PATH);
				::ReleaseMutex(hMutex);
				fOK = ::SetEvent(hEvent) != FALSE;
			}
			::UnmapViewOfFile(pParam);
		}
	}

	if (hEvent != nullptr) ::CloseHandle(hEvent);
	if (hMutex != nullptr) ::CloseHandle(hMutex);
	if (hMap != nullptr) ::CloseHandle(hMap);
	return fOK;
}

//---------------------------------------------------------------------------
//	終了時の確認ダイアログを模して自動応答されるかを見る
//---------------------------------------------------------------------------
struct MessageBoxResult {
	int Result = 0;
	volatile LONG Done = 0;
};

DWORD WINAPI MessageBoxThread(LPVOID pParameter)
{
	auto *pResult = static_cast<MessageBoxResult *>(pParameter);
	pResult->Result = ::MessageBoxW(
		nullptr,
		L"現在の編集データは更新されています\nプロジェクトを保存しますか？",
		L"AviUtl ExEdit2", MB_YESNOCANCEL | MB_ICONQUESTION);
	::InterlockedExchange(&pResult->Done, 1);
	return 0;
}

//	開いたままのダイアログを閉じる (自動応答されなかった時の後始末)
BOOL CALLBACK CloseDialogProc(HWND hwnd, LPARAM lParam)
{
	DWORD ProcessID = 0;
	::GetWindowThreadProcessId(hwnd, &ProcessID);
	if (ProcessID != ::GetCurrentProcessId())
		return TRUE;

	WCHAR szClass[64] = {};
	::GetClassNameW(hwnd, szClass, 64);
	if (::lstrcmpW(szClass, L"#32770") == 0) {
		::PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
		*reinterpret_cast<bool *>(lParam) = true;
	}
	return TRUE;
}

}	// namespace

int main(int argc, char **argv)
{
	const char *pszDllPath = argc > 1 ? argv[1] : "dist/TSMemory-TVTestSrc.aux2";

	g_hCreatedEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
	SetupHost();

	HMODULE hModule = ::LoadLibraryA(pszDllPath);
	check("LoadLibrary(TSMemory-TVTestSrc.aux2)", hModule != nullptr);
	if (hModule == nullptr) {
		std::printf("  GetLastError() = %lu\n", ::GetLastError());
		return 1;
	}

	auto pRequiredVersion = reinterpret_cast<DWORD (*)()>(
		::GetProcAddress(hModule, "RequiredVersion"));
	auto pInitializePlugin = reinterpret_cast<bool (*)(DWORD)>(
		::GetProcAddress(hModule, "InitializePlugin"));
	auto pUninitializePlugin = reinterpret_cast<void (*)()>(
		::GetProcAddress(hModule, "UninitializePlugin"));
	auto pGetCommonPluginTable = reinterpret_cast<COMMON_PLUGIN_TABLE *(*)()>(
		::GetProcAddress(hModule, "GetCommonPluginTable"));
	auto pRegisterPlugin = reinterpret_cast<void (*)(HOST_APP_TABLE *)>(
		::GetProcAddress(hModule, "RegisterPlugin"));

	check("exports RequiredVersion", pRequiredVersion != nullptr);
	check("exports InitializePlugin", pInitializePlugin != nullptr);
	check("exports UninitializePlugin", pUninitializePlugin != nullptr);
	check("exports GetCommonPluginTable", pGetCommonPluginTable != nullptr);
	check("exports RegisterPlugin", pRegisterPlugin != nullptr);
	if (pRegisterPlugin == nullptr || pInitializePlugin == nullptr)
		return 1;

	COMMON_PLUGIN_TABLE *pCommon = pGetCommonPluginTable();
	check("GetCommonPluginTable() name is TSMemory-TVTestSrc",
		  pCommon != nullptr && ::lstrcmpW(pCommon->name, L"TSMemory-TVTestSrc") == 0);

	check("InitializePlugin() succeeds", pInitializePlugin(pRequiredVersion()));

	pRegisterPlugin(&g_Host);

	check("register_input_plugin() was called", g_pInputPluginTable != nullptr);
	check("register_window_client() was called", g_hwndWindowClient != nullptr);
	check("register_export_menu_param() was called", g_pszExportMenuName != nullptr);

	//	終了時の確認への自動応答は本体の動作に割り込むので、
	//	ini で明示的に有効にしない限り何もしない事
	{
		WCHAR szIni[MAX_PATH];
		::MultiByteToWideChar(CP_ACP, 0, pszDllPath, -1, szIni, MAX_PATH);
		::PathRenameExtensionW(szIni, L".ini");
		ExitGuardMode =
			::GetPrivateProfileIntW(L"Bridge", L"SuppressExitConfirm", 0, szIni);
		const int Mode = ExitGuardMode;

		if (Mode >= 2) {
			//	編集の有無を見ないので、更新の監視は登録しない
			check("SuppressExitConfirm=2 does not watch object updates",
				  g_EventListenerCount == 0);
		} else if (Mode == 1) {
			//	編集を検出する為に UPDATE_OBJECT を監視する
			check("SuppressExitConfirm=1 watches object updates",
				  g_EventListenerCount > 0);
		} else {
			check("the exit confirmation guard stays off by default",
				  g_EventListenerCount == 0);
		}
	}

	//	AviUtl2 で窓が出ない時の切り分け用。
	//	ここが通っていればウィンドウ自体は正しく作られており、
	//	あとは AviUtl2 の表示メニューで表示されているかだけの問題になる。
	if (g_hwndWindowClient != nullptr) {
		check("the registered window is a real window", ::IsWindow(g_hwndWindowClient) != FALSE);

		int Children = 0;
		::EnumChildWindows(g_hwndWindowClient,
						   [](HWND, LPARAM lParam) -> BOOL {
							   (*reinterpret_cast<int *>(lParam))++;
							   return TRUE;
						   },
						   reinterpret_cast<LPARAM>(&Children));
		check("the window has its controls (filename/format/quality/save)", Children == 4);

		WCHAR szClass[64] = {};
		::GetClassNameW(g_hwndWindowClient, szClass, 64);
		check("the window uses the expected window class",
			  ::lstrcmpW(szClass, L"TSMemoryCaptureUtility") == 0);
	}

	if (g_pInputPluginTable != nullptr) {
		check("input plugin handles video",
			  (g_pInputPluginTable->flag & INPUT_PLUGIN_TABLE::FLAG_VIDEO) != 0);

		//	音声は [M2V] audio=1 の時だけ申告する。
		//	申告だけして中身が無いと AviUtl2 側に空の音声が出来る。
		//	この ini は音声を有効にしていないので落ちている事
		check("input plugin does not claim audio unless it is enabled",
			  (g_pInputPluginTable->flag & INPUT_PLUGIN_TABLE::FLAG_AUDIO) == 0);
		check("input plugin file filter mentions *.tvtv",
			  ::wcsstr(g_pInputPluginTable->filefilter, L"*.tvtv") != nullptr);
		check("input plugin has all required callbacks",
			  g_pInputPluginTable->func_open != nullptr
			  && g_pInputPluginTable->func_close != nullptr
			  && g_pInputPluginTable->func_info_get != nullptr
			  && g_pInputPluginTable->func_read_video != nullptr
			  && g_pInputPluginTable->func_read_audio != nullptr);

		//	共有メモリが無いファイルを開こうとしても落ちずに失敗する事
		check("func_open() on a missing .tvtv returns null",
			  g_pInputPluginTable->func_open(L"C:\\nowhere\\no_such_file.tvtv") == nullptr);
	}

	//	--- 連携 -------------------------------------------------------------
	//	プロジェクトの初期化前に待ち受けを始めてしまうと、TVTest から
	//	AviUtl2 を起動した時に初期化でタイムラインごと消えてしまう。
	check("bridge registered a project load handler", g_pProjectLoadHandler != nullptr);

	HANDLE hReady = ::OpenMutexW(SYNCHRONIZE, FALSE, TSMEMORY_IPC_READY_MUTEX);
	check("bridge does NOT accept requests before the project is initialized",
		  hReady == nullptr);
	if (hReady != nullptr)
		::CloseHandle(hReady);

	//	初期化完了を通知する
	if (g_pProjectLoadHandler != nullptr)
		g_pProjectLoadHandler(nullptr);

	//	ReadyDelay (既定 0.5 秒) のぶん間を置いてから待ち受けが始まる。
	//	秒の解釈を間違えると 0 秒 (下の 1 つ目) か 500 秒 (2 つ目) で落ちる。
	::Sleep(100);
	hReady = ::OpenMutexW(SYNCHRONIZE, FALSE, TSMEMORY_IPC_READY_MUTEX);
	check("bridge waits for ReadyDelay before accepting requests", hReady == nullptr);
	if (hReady != nullptr)
		::CloseHandle(hReady);

	for (int i = 0; i < 100 && hReady == nullptr; i++) {
		::Sleep(50);
		hReady = ::OpenMutexW(SYNCHRONIZE, FALSE, TSMEMORY_IPC_READY_MUTEX);
	}
	check("bridge created the ready mutex after the project was initialized",
		  hReady != nullptr);
	if (hReady != nullptr)
		::CloseHandle(hReady);

	check("request was sent", SendRequest(L"C:\\somewhere\\tsmemory0_1.tvtv"));
	check("bridge called create_object_from_media_file()",
		  ::WaitForSingleObject(g_hCreatedEvent, 5000) == WAIT_OBJECT_0);
	check("bridge passed the requested path",
		  ::lstrcmpW(g_szCreatedFile, L"C:\\somewhere\\tsmemory0_1.tvtv") == 0);
	check("bridge cleared the target layer first", g_DeletedObjects > 0);
	check("bridge used layer 0 / frame 0 by default",
		  g_CreatedLayer == 0 && g_CreatedFrame == 0);

	//	長さに 0 を渡すと AviUtl2 の既定のオブジェクト長になってしまい、
	//	取り込んだ映像の後ろが切れる。取り込んだ長さを明示的に渡す事。
	check("bridge passed the captured length instead of 0",
		  g_CreatedLength == MEDIA_EXPECTED_FRAMES);
	if (g_CreatedLength != MEDIA_EXPECTED_FRAMES)
		std::printf("  expected %d frames but got %d\n", MEDIA_EXPECTED_FRAMES, g_CreatedLength);

	//	--- 終了時の確認の自動応答 -------------------------------------------
	//	有効にしてある時だけ。実際に MessageBox を出して応答されるか確認する。
	if (ExitGuardMode > 0) {
		MessageBoxResult Result;
		HANDLE hThread = ::CreateThread(nullptr, 0, MessageBoxThread, &Result, 0, nullptr);

		const bool fAnswered =
			hThread != nullptr && ::WaitForSingleObject(hThread, 5000) == WAIT_OBJECT_0;

		if (!fAnswered) {
			bool fClosed = false;
			::EnumWindows(CloseDialogProc, reinterpret_cast<LPARAM>(&fClosed));
			if (hThread != nullptr)
				::WaitForSingleObject(hThread, 5000);
		}
		if (hThread != nullptr)
			::CloseHandle(hThread);

		check("the exit confirmation dialog was answered automatically", fAnswered);
		check("it was answered with 'No' (do not save)", Result.Result == IDNO);
		if (fAnswered && Result.Result != IDNO)
			std::printf("  MessageBox returned %d (IDNO=%d)\n", Result.Result, IDNO);

		//	SuppressExitConfirm=2 は編集を見ないので、配置直後の猶予を過ぎても
		//	応答し続ける事 (編集した後でも出さない、という設定)
		if (ExitGuardMode >= 2) {
			::Sleep(2200);

			MessageBoxResult Later;
			HANDLE hLaterThread =
				::CreateThread(nullptr, 0, MessageBoxThread, &Later, 0, nullptr);
			const bool fAnsweredLater =
				hLaterThread != nullptr
				&& ::WaitForSingleObject(hLaterThread, 5000) == WAIT_OBJECT_0;

			if (!fAnsweredLater) {
				bool fClosed = false;
				::EnumWindows(CloseDialogProc, reinterpret_cast<LPARAM>(&fClosed));
				if (hLaterThread != nullptr)
					::WaitForSingleObject(hLaterThread, 5000);
			}
			if (hLaterThread != nullptr)
				::CloseHandle(hLaterThread);

			check("SuppressExitConfirm=2 keeps answering after the placement window",
				  fAnsweredLater && Later.Result == IDNO);
		}

		//	ユーザーが編集した後は応答しない事 (手の編集を捨てない)
		if (g_pUpdateObjectListener != nullptr) {
			//	配置直後の一定時間は TSMemory 自身の更新と区別出来ないので、
			//	その時間を過ぎてから編集を通知する
			::Sleep(2200);
			g_pUpdateObjectListener(g_pUpdateObjectParam);

			MessageBoxResult Edited;
			HANDLE hEditedThread =
				::CreateThread(nullptr, 0, MessageBoxThread, &Edited, 0, nullptr);
			const bool fAnsweredAfterEdit =
				hEditedThread != nullptr
				&& ::WaitForSingleObject(hEditedThread, 3000) == WAIT_OBJECT_0;

			check("it does NOT answer once the user has edited something",
				  !fAnsweredAfterEdit);

			bool fClosed = false;
			::EnumWindows(CloseDialogProc, reinterpret_cast<LPARAM>(&fClosed));
			if (hEditedThread != nullptr) {
				::WaitForSingleObject(hEditedThread, 5000);
				::CloseHandle(hEditedThread);
			}
		}
	}

	pUninitializePlugin();

	//	--- アンロード後に何も残さない事 -------------------------------------
	//	パッケージの再インストール時、AviUtl2 は差し替えの為にプラグインを
	//	アンロードする。ウィンドウやウィンドウクラスを残したままにすると
	//	その後のメッセージでアンロード済みのコードに飛んで落ちる。
	{
		const HWND hwndCapture = g_hwndWindowClient;
		::FreeLibrary(hModule);

		check("the capture window is destroyed on uninitialize",
			  hwndCapture == nullptr || ::IsWindow(hwndCapture) == FALSE);

		WNDCLASSEXW wcex = {};
		wcex.cbSize = sizeof(wcex);
		const bool fClassLeft =
			::GetClassInfoExW(::GetModuleHandleW(nullptr), L"TSMemoryCaptureUtility", &wcex) != 0;
		check("the window class is unregistered on uninitialize", !fClassLeft);
	}

	//	待ち受けを止めたら ready mutex も消えている事
	hReady = ::OpenMutexW(SYNCHRONIZE, FALSE, TSMEMORY_IPC_READY_MUTEX);
	check("ready mutex is released on uninitialize", hReady == nullptr);
	if (hReady != nullptr)
		::CloseHandle(hReady);

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
