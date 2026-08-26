//----------------------------------------------------------------------------
//	TVTest からの読み込み要求を受けて .tvtv をタイムラインに配置する
//----------------------------------------------------------------------------
#include <windows.h>
#include <cwchar>
#include <shlwapi.h>

//	StringCchPrintfW を使う。lstrcpynW 等は引き続き使うので非推奨化はしない
#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#include "plugin2.h"
#include "logger2.h"

#include "tsmemory_ipc.h"
#include "bridge.h"
#include "ts_caption.h"
#include "drcs_font.h"
#include "exitguard.h"
#include "inifile.h"
#include "plugin_main.h"
#include "preset.h"

namespace {

struct BridgeState {
	EDIT_HANDLE *Edit = nullptr;
	LOG_HANDLE *Logger = nullptr;

	HANDLE hReadyMutex = nullptr;
	HANDLE hParamMap = nullptr;
	HANDLE hParamMutex = nullptr;
	HANDLE hRequestEvent = nullptr;
	HANDLE hQuitEvent = nullptr;
	HANDLE hProjectReady = nullptr;	// プロジェクトの初期化完了
	HANDLE hThread = nullptr;
	TSMEMORY_REQUEST *pParam = nullptr;

	int Layer = 0;			// 配置先レイヤー番号 (0 起点)
	int Frame = 0;			// 配置先フレーム番号 (0 起点)
	bool ReplaceLayer = true;	// 配置先レイヤーの既存オブジェクトを消すか
	bool Activate = true;		// 読み込み時にウィンドウを手前に出すか
	bool LockLayer = false;		// 配置後に配置先レイヤーをロックするか
	bool SeekToEnd = false;

	//	字幕 (src/aviutl2/caption/)。既定は無効
	bool CaptionEnable = false;
	int CaptionLayer = 1;                 // 映像とは別のレイヤー (0 起点)
	std::wstring CaptionPreset;           // 本文の先頭に入れる <$...>
	std::wstring CaptionDrcsFont = L"TSMemory DRCS";
	bool CaptionBroadcastColor = true;    // 放送の指定した色をそのまま使うか
	bool CaptionBackColor = true;         // 放送の背景色を影・縁色に流すか
	int ReadyDelay = 500;		// 初期化完了から待ち受け開始までの余裕 (ms)
	int ReadyTimeout = 30000;	// 初期化完了の通知が来ない場合の打ち切り (ms)

	TSMemoryPreset Preset;		// 配置後に適用するフィルタプリセット
};

BridgeState g_State;

void Log(LPCWSTR message)
{
	if (g_State.Logger != nullptr)
		g_State.Logger->log(g_State.Logger, message);
}

void LogWarn(LPCWSTR message)
{
	if (g_State.Logger != nullptr)
		g_State.Logger->warn(g_State.Logger, message);
}

//	ウィンドウを手前に出す
void ActivateWindow(HWND hwnd)
{
	if (!::IsWindow(hwnd))
		return;

	if (::IsIconic(hwnd))
		::ShowWindow(hwnd, SW_RESTORE);

	HWND hwndFore = ::GetForegroundWindow();
	if (hwndFore == hwnd)
		return;

	const DWORD ThreadID = ::GetWindowThreadProcessId(hwndFore, nullptr);
	::AttachThreadInput(::GetCurrentThreadId(), ThreadID, TRUE);
	::SetForegroundWindow(hwnd);
	::AttachThreadInput(::GetCurrentThreadId(), ThreadID, FALSE);
}

//	字幕をタイムラインに置く。
//
//	**映像とは別のレイヤーに、字幕ごとに 1 つのテキストオブジェクトを作る。**
//	書体は本文の先頭に入れた <$プリセット名> が決めるので、利用者は
//	AviUtl2 側でそのプリセットを 1 つ直せば全ての字幕に効く
//	(タイムライン上で個別に触らなくてよい)。
//
//	置いたら true を返す。呼び出し側はこれを見てロックを掛ける
//	(何も置いていない空のレイヤーをロックしても紛らわしいだけな為)。
bool PlaceCaptions(EDIT_SECTION *edit, LPCWSTR pszFile,
				   const OBJECT_LAYER_FRAME &Video)
{
	if (!g_State.CaptionEnable)
		return false;

	const int Layer = g_State.CaptionLayer;

	//	**映像と同じレイヤーには置けない。**
	//	下の掃除で、直前に置いた映像のオブジェクトごと消してしまう。
	//	黙って消えると原因が判らない為、置かずに伝える
	if (Layer == g_State.Layer) {
		LogWarn(L"TSMemory: [Caption] Layer が [Bridge] Layer と同じです "
				L"(字幕は置きません。別のレイヤーを指定してください)");
		return false;
	}

	//	**ロックされたレイヤーにはオブジェクトを置けない。**
	//	前回 LockLayer で掛けたものなら外す。手で掛けたものは
	//	勝手に外さず、置けない事を伝えて諦める (映像側と同じ扱い)
	if (edit->get_layer_lock(Layer)) {
		if (!g_State.LockLayer) {
			LogWarn(L"TSMemory: 字幕のレイヤーがロックされています "
					L"(字幕は置きません。ロックを外してください)");
			return false;
		}
		edit->set_layer_lock(Layer, false);
	}

	//	共有メモリ名は .tvtv のファイル名部分だけ (m2v と同じ規約)
	char szName[MAX_PATH];
	if (::WideCharToMultiByte(CP_ACP, 0, ::PathFindFileNameW(pszFile), -1,
							  szName, MAX_PATH, nullptr, nullptr) <= 0)
		return false;

	AribToAviUtl2Options opt;
	opt.Preset = g_State.CaptionPreset;
	opt.DrcsFont = g_State.CaptionDrcsFont;
	opt.UseBroadcastColor = g_State.CaptionBroadcastColor;
	opt.UseBroadcastBackColor = g_State.CaptionBackColor;

	CTSCaptionSource Source;
	if (!Source.Open(szName, opt)) {
		WCHAR sz[192];
		::StringCchPrintfW(sz, ARRAYSIZE(sz),
						   L"TSMemory: 字幕を取り込めませんでした : %s",
						   Source.GetLastError());
		LogWarn(sz);
		return false;
	}

	//	外字のフォントを本体に登録する。
	//	メモリ上のまま渡せるのでファイルは作らない
	if (!Source.GetFont().empty()) {
		TSMemoryRegisterFontCollection(Source.GetFont().data(),
									   Source.GetFont().size());
	}

	const double Rate = (edit->info != nullptr && edit->info->scale > 0)
						? static_cast<double>(edit->info->rate) / edit->info->scale : 0.0;

	//	配置先レイヤーを空けてから置く
	for (int i = 0; i < 1024; i++) {
		OBJECT_HANDLE o = edit->find_object(Layer, 0);
		if (o == nullptr)
			break;
		edit->delete_object(o);
	}

	int Placed = 0;
	for (int i = 0; i < Source.GetCount(); i++) {
		const TSMemoryCaption &c = Source.Get(i);
		if (c.Text.empty())
			continue;

		//	映像の先頭からの秒数をフレーム番号に直す。
		//	判らない物は置かない (どこに出せばよいか決められない)
		if (c.Seconds < 0.0 || Rate <= 0.0)
			continue;
		int Start = Video.start + static_cast<int>(c.Seconds * Rate + 0.5);
		if (Start < Video.start)
			Start = Video.start;
		if (Start > Video.end)
			continue;

		//	次の字幕までを表示の長さにする。最後の 1 つは映像の末尾まで
		int End = Video.end;
		for (int k = i + 1; k < Source.GetCount(); k++) {
			if (Source.Get(k).Seconds > c.Seconds) {
				End = Video.start
					+ static_cast<int>(Source.Get(k).Seconds * Rate + 0.5) - 1;
				break;
			}
		}
		if (End > Video.end)
			End = Video.end;
		if (End < Start)
			End = Start;

		OBJECT_HANDLE o = edit->create_object(L"テキスト", Layer, Start,
											  End - Start + 1);
		if (o == nullptr)
			continue;

		//	設定値は UTF-8 で渡す
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, c.Text.c_str(), -1,
											nullptr, 0, nullptr, nullptr);
		if (n > 0) {
			std::vector<char> u8(n);
			::WideCharToMultiByte(CP_UTF8, 0, c.Text.c_str(), -1,
								  u8.data(), n, nullptr, nullptr);
			edit->set_object_item_value(o, L"テキスト", L"テキスト", u8.data());
		}
		Placed++;
	}

	WCHAR sz[224];
	::StringCchPrintfW(sz, ARRAYSIZE(sz),
					   L"TSMemory: 字幕を %d 件配置しました (レイヤー %d"
					   L"%s%d 字形)",
					   Placed, Layer + 1,
					   L" / 外字 ", Source.GetGlyphCount());
	Log(sz);

	if (Source.GetMissingGlyphCount() > 0) {
		//	**字形の定義がリングバッファの窓より前にあると起こる。**
		//	再送間隔は実測で中央値 14 秒、最大 210 秒あり、既定の
		//	MemorySize=10 (8〜14 秒) では拾えない事がある
		::StringCchPrintfW(sz, ARRAYSIZE(sz),
						   L"TSMemory: 外字 %d 個は字形が届いていません "
						   L"(MemorySize を大きくすると拾えます)",
						   Source.GetMissingGlyphCount());
		LogWarn(sz);
	}

	return Placed > 0;
}

//	編集セクション内での実処理
//	param には読み込むファイルのパスが入る
void ProcEdit(void *param, EDIT_SECTION *edit)
{
	LPCWSTR pszFile = static_cast<LPCWSTR>(param);

	if (!edit->is_support_media_file(pszFile, false)) {
		LogWarn(L"TSMemory: .tvtv に対応する入力プラグインが見つかりません");
		return;
	}

	//	前回の取り込みで掛けたロックを外す。
	//	ロックされたレイヤーにはオブジェクトを置けない為。
	//	LockLayer が無効な時は触らない (手でロックしたものを勝手に外さない)。
	if (g_State.LockLayer && edit->get_layer_lock(g_State.Layer))
		edit->set_layer_lock(g_State.Layer, false);

	//	配置先レイヤーを空ける。
	//	find_object() は指定フレーム以降で最初に見つかったものを返すので、
	//	見つからなくなるまで削除する (無限ループ防止に上限を設ける)。
	if (g_State.ReplaceLayer) {
		for (int i = 0; i < 1024; i++) {
			OBJECT_HANDLE object = edit->find_object(g_State.Layer, 0);
			if (object == nullptr)
				break;
			edit->delete_object(object);
		}
	}

	//	オブジェクトの長さは明示的に指定する。
	//	length に 0 を渡すと「追加位置から自動調整」になり、取り込んだ映像の
	//	長さではなく AviUtl2 側の既定のオブジェクト長になってしまう為。
	MEDIA_INFO Media = {};
	int Length = 0;
	if (edit->get_media_info(pszFile, &Media, sizeof(Media))
			&& Media.total_time > 0.0
			&& edit->info != nullptr && edit->info->scale > 0) {
		Length = static_cast<int>(
			Media.total_time * edit->info->rate / edit->info->scale + 0.5);
	}

	{
		WCHAR szMessage[256];
		::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage),
					L"TSMemory: 取り込んだ映像 %dx%d %d.%03d 秒 -> %d フレームで配置します",
					Media.width, Media.height,
					static_cast<int>(Media.total_time),
					static_cast<int>(Media.total_time * 1000) % 1000,
					Length);
		Log(szMessage);
	}

	OBJECT_HANDLE object = edit->create_object_from_media_file(
		pszFile, g_State.Layer, g_State.Frame, Length);

	if (object == nullptr) {
		LogWarn(L"TSMemory: オブジェクトを作成出来ませんでした");
		return;
	}

	//	フィルタプリセットを適用する。
	//	AviUtl2 は 1.xx と違い各フィルタの初期値を保存出来ない為、
	//	インターレース解除やノイズ除去などはここで組み立てる。
	if (!g_State.Preset.IsEmpty()) {
		TSMemoryPresetResult Result;
		TSMemoryPresetApply(edit, object, g_State.Preset, &Result);

		WCHAR szMessage[512];
		::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage), L"TSMemory: プリセットを適用しました (エフェクト %d 件 / 設定 %d 件)",
					Result.Effects, Result.Items);
		Log(szMessage);

		if (Result.Failed > 0) {
			::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage),
						L"TSMemory: プリセットの %d 件を適用出来ませんでした (例: %s)",
						Result.Failed, Result.FirstFailure.c_str());
			LogWarn(szMessage);
		}
	}

	edit->set_focus_object(object);

	const OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);

	//	シーク位置。SeekToEnd=1 なら取り込んだ映像の末尾に置く。
	//	OBJECT_LAYER_FRAME::end は終了フレーム番号そのもの (0 起点)。
	//	TVTest から渡ってくるのは「今まさに映っていた所まで」なので、
	//	末尾が目的のフレームになる事が多い。
	const int Cursor = g_State.SeekToEnd ? lf.end : g_State.Frame;
	edit->set_cursor_layer_frame(g_State.Layer, Cursor);

	{
		WCHAR szMessage[256];
		::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage),
					L"TSMemory: 映像を読み込みました "
					L"(レイヤー %d / フレーム %d - %d / シーク位置 %d)",
					lf.layer + 1, lf.start + 1, lf.end + 1, Cursor + 1);
		Log(szMessage);
	}

	//	字幕を置く。映像とは別のレイヤーに、時間に合わせて並べる
	const bool fCaptionPlaced = PlaceCaptions(edit, pszFile, lf);

	//	プレビュー上での誤操作を防ぐ為に配置先レイヤーをロックする。
	//
	//	AviUtl2 のオブジェクトリストにある「プレビュー編集の操作をロック」は
	//	オブジェクト単位のロック (内部の setLockObject) で、プラグイン API には
	//	公開されていない。公開されているのはレイヤー単位のロックだけな為、
	//	こちらを使う。TSMemory は専用レイヤーに置く前提なので実質同じになる。
	if (g_State.LockLayer) {
		edit->set_layer_lock(g_State.Layer, true);
		Log(L"TSMemory: 配置先レイヤーをロックしました "
			L"(プレビュー上での誤操作を防ぎます)");

		//	字幕のレイヤーも同じ扱いにする。次の取り込みでは
		//	PlaceCaptions() が自分で外すので、掛けたままで構わない
		if (fCaptionPlaced) {
			edit->set_layer_lock(g_State.CaptionLayer, true);
			Log(L"TSMemory: 字幕のレイヤーもロックしました");
		}
	}

	//	ここまでの変更は TSMemory によるもの、と記録しておく
	TSMemoryExitGuardNotifyPlaced();
}

//	要求を 1 件処理する
void HandleRequest()
{
	WCHAR szFileName[MAX_PATH];

	//	パラメータの取り出し (共有領域は素早く手放す)
	if (::WaitForSingleObject(g_State.hParamMutex, 3000) != WAIT_OBJECT_0)
		return;
	const DWORD Version = g_State.pParam->Version;
	::lstrcpynW(szFileName, g_State.pParam->FileName, MAX_PATH);
	::ReleaseMutex(g_State.hParamMutex);

	if (Version != TSMEMORY_IPC_VERSION) {
		LogWarn(L"TSMemory: TVTest 側プラグインとの連携バージョンが一致しません");
		return;
	}
	if (szFileName[0] == L'\0')
		return;

	if (g_State.Activate && g_State.Edit != nullptr)
		ActivateWindow(g_State.Edit->get_host_app_window());

	if (!g_State.Edit->call_edit_section_param(szFileName, ProcEdit))
		LogWarn(L"TSMemory: 出力中などで編集出来ないため読み込みを中止しました");
}

//	プロジェクトの初期化が終わってから待ち受けを開始する。
//
//	AviUtl2 は「プラグインの登録」→「プロジェクトの初期化」の順で起動する。
//	登録の時点で待ち受けを始めてしまうと、TVTest から AviUtl2 を起動した時に
//	初期化前にオブジェクトを作ってしまい、その後の初期化でタイムラインごと
//	消えてしまう (AviUtl2 が既に起動している場合は起きない)。
//	その為、初期化完了の通知を受けてから Ready のミューテックスを作る。
bool CreateReadyMutex()
{
	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;
	TSMemoryInitSecurityAttributes(&sd, &sa);

	g_State.hReadyMutex = ::CreateMutexW(&sa, FALSE, TSMEMORY_IPC_READY_MUTEX);
	if (g_State.hReadyMutex == nullptr) {
		LogWarn(L"TSMemory: 連携用ミューテックスを作成出来ませんでした");
		return false;
	}
	if (::GetLastError() == ERROR_ALREADY_EXISTS) {
		LogWarn(L"TSMemory: 既に他のプロセスが TVTest からの要求を待ち受けています");
		::CloseHandle(g_State.hReadyMutex);
		g_State.hReadyMutex = nullptr;
		return false;
	}
	return true;
}

DWORD WINAPI ListenThread(LPVOID)
{
	//	初期化完了を待ってから待ち受けを開始する
	{
		HANDLE Handles[2] = { g_State.hProjectReady, g_State.hQuitEvent };
		const DWORD Result =
			::WaitForMultipleObjects(2, Handles, FALSE, g_State.ReadyTimeout);
		if (Result == WAIT_OBJECT_0 + 1)
			return 0;
		if (Result == WAIT_TIMEOUT) {
			LogWarn(L"TSMemory: プロジェクトの初期化通知が来ないまま待ち受けを開始します");
		} else if (g_State.ReadyDelay > 0) {
			//	初期化直後は各ウィンドウの準備中なので少しだけ間を置く
			if (::WaitForSingleObject(g_State.hQuitEvent, g_State.ReadyDelay) == WAIT_OBJECT_0)
				return 0;
		}
	}

	if (!CreateReadyMutex())
		return 0;

	Log(L"TSMemory: TVTest からの要求の待ち受けを開始しました");

	HANDLE Handles[2] = { g_State.hRequestEvent, g_State.hQuitEvent };

	for (;;) {
		const DWORD Result = ::WaitForMultipleObjects(2, Handles, FALSE, INFINITE);
		if (Result != WAIT_OBJECT_0)
			break;
		HandleRequest();
	}
	return 0;
}

//	プロジェクトの初期化・読み込みが終わった時に呼ばれる
void OnProjectLoaded(PROJECT_FILE *)
{
	if (g_State.hProjectReady != nullptr)
		::SetEvent(g_State.hProjectReady);
}

int GetIniInt(LPCWSTR ini, LPCWSTR key, int def)
{
	if (ini == nullptr)
		return def;
	return static_cast<int>(::GetPrivateProfileIntW(L"Bridge", key, def, ini));
}

//	時間の設定は全て「秒」で指定する (小数可)。返り値はミリ秒。
//	min_sec / max_sec で範囲を制限する。
int GetIniMilliseconds(LPCWSTR ini, LPCWSTR key, double def_sec, double min_sec, double max_sec)
{
	double Seconds = def_sec;

	if (ini != nullptr) {
		WCHAR szValue[32] = {};
		::GetPrivateProfileStringW(L"Bridge", key, L"", szValue, 32, ini);
		if (szValue[0] != L'\0') {
			WCHAR *pEnd = nullptr;
			const double Value = ::wcstod(szValue, &pEnd);
			if (pEnd != szValue)
				Seconds = Value;
		}
	}

	if (Seconds < min_sec)
		Seconds = min_sec;
	else if (Seconds > max_sec)
		Seconds = max_sec;

	return static_cast<int>(Seconds * 1000.0 + 0.5);
}

}	// namespace

bool TSMemoryBridgeStart(HOST_APP_TABLE *host, EDIT_HANDLE *edit, LOG_HANDLE *logger,
						 LPCWSTR ini_file)
{
	g_State.Edit = edit;
	g_State.Logger = logger;

	if (edit == nullptr)
		return false;

	g_State.Layer = GetIniInt(ini_file, L"Layer", 1) - 1;
	if (g_State.Layer < 0)
		g_State.Layer = 0;
	g_State.Frame = GetIniInt(ini_file, L"Frame", 1) - 1;
	if (g_State.Frame < 0)
		g_State.Frame = 0;
	g_State.ReplaceLayer = GetIniInt(ini_file, L"ReplaceLayer", 1) != 0;
	g_State.Activate = GetIniInt(ini_file, L"Activate", 1) != 0;
	g_State.LockLayer = GetIniInt(ini_file, L"LockLayer", 0) != 0;
	g_State.SeekToEnd = GetIniInt(ini_file, L"SeekToEnd", 0) != 0;

	//	字幕。書体は <$プリセット名> が決めるので、利用者は AviUtl2 側で
	//	そのテキストプリセットを 1 つ直せば全ての字幕に効く
	{
		g_State.CaptionEnable =
			::GetPrivateProfileIntW(L"Caption", L"Enable", 0, ini_file) != 0;
		g_State.CaptionLayer =
			::GetPrivateProfileIntW(L"Caption", L"Layer", 2, ini_file) - 1;
		if (g_State.CaptionLayer < 0)
			g_State.CaptionLayer = 0;
		g_State.CaptionBroadcastColor =
			::GetPrivateProfileIntW(L"Caption", L"UseBroadcastColor", 1, ini_file) != 0;
		g_State.CaptionBackColor =
			::GetPrivateProfileIntW(L"Caption", L"UseBackColor", 1, ini_file) != 0;

		WCHAR sz[128];
		TSMemoryGetIniString(ini_file, L"Caption", L"Preset", L"", sz, ARRAYSIZE(sz));
		g_State.CaptionPreset = sz;
		TSMemoryGetIniString(ini_file, L"Caption", L"DrcsFont", L"TSMemory DRCS",
							 sz, ARRAYSIZE(sz));
		g_State.CaptionDrcsFont = sz;
	}
	//	時間の設定は秒 (小数可) で指定する
	g_State.ReadyDelay = GetIniMilliseconds(ini_file, L"ReadyDelay", 0.5, 0.0, 10.0);
	g_State.ReadyTimeout = GetIniMilliseconds(ini_file, L"ReadyTimeout", 30.0, 1.0, 600.0);

	//	フィルタプリセット。名前で指定すると AviUtl2 のデータフォルダの
	//	Preset\<種別>.<名前>.preset を探す。PresetFile でパス直接指定も出来る。
	{
		//	プリセット名には日本語が入る為、UTF-8 対応の読み出しを使う
		WCHAR szPreset[128] = {}, szPresetFile[MAX_PATH] = {};
		TSMemoryGetIniString(ini_file, L"Bridge", L"Preset", L"", szPreset, 128);
		TSMemoryGetIniString(ini_file, L"Bridge", L"PresetFile", L"", szPresetFile, MAX_PATH);

		if (szPreset[0] != L'\0' || szPresetFile[0] != L'\0') {
			WCHAR szMessage[MAX_PATH + 128];
			if (TSMemoryPresetLoad(szPreset, szPresetFile, TSMemoryGetModuleHandle(),
								   &g_State.Preset)) {
				::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage), L"TSMemory: フィルタプリセットを読み込みました (%s)",
							g_State.Preset.Path.c_str());
				Log(szMessage);
			} else {
				::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage),
							L"TSMemory: フィルタプリセット「%s」が見つかりませんでした",
							szPresetFile[0] != L'\0' ? szPresetFile : szPreset);
				LogWarn(szMessage);
			}
		}
	}

	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;
	TSMemoryInitSecurityAttributes(&sd, &sa);

	//	Ready のミューテックスはプロジェクトの初期化が終わってから作る
	//	(ListenThread を参照)
	g_State.hProjectReady = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (host != nullptr)
		host->register_project_load_handler(OnProjectLoaded);

	g_State.hParamMutex = ::CreateMutexW(&sa, FALSE, TSMEMORY_IPC_PARAM_MUTEX);
	g_State.hParamMap = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
											0, sizeof(TSMEMORY_REQUEST), TSMEMORY_IPC_PARAM_MAP);
	if (g_State.hParamMap != nullptr) {
		g_State.pParam = static_cast<TSMEMORY_REQUEST *>(
			::MapViewOfFile(g_State.hParamMap, FILE_MAP_WRITE, 0, 0, 0));
	}
	g_State.hRequestEvent = ::CreateEventW(&sa, FALSE, FALSE, TSMEMORY_IPC_REQUEST_EVENT);
	g_State.hQuitEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);

	if (g_State.hParamMutex == nullptr || g_State.pParam == nullptr
			|| g_State.hRequestEvent == nullptr || g_State.hQuitEvent == nullptr) {
		LogWarn(L"TSMemory: 連携用のオブジェクトを作成出来ませんでした");
		TSMemoryBridgeStop();
		return false;
	}

	::ZeroMemory(g_State.pParam, sizeof(TSMEMORY_REQUEST));

	g_State.hThread = ::CreateThread(nullptr, 0, ListenThread, nullptr, 0, nullptr);
	if (g_State.hThread == nullptr) {
		TSMemoryBridgeStop();
		return false;
	}

	Log(L"TSMemory: プロジェクトの初期化を待っています");
	return true;
}

void TSMemoryBridgeStop()
{
	if (g_State.hThread != nullptr) {
		::SetEvent(g_State.hQuitEvent);
		const DWORD Result = ::WaitForSingleObject(g_State.hThread, 5000);
		::CloseHandle(g_State.hThread);
		g_State.hThread = nullptr;

		//	待ち切れなかった場合、受信スレッドはまだ共有メモリと
		//	同期オブジェクトを使っている。ここで解放すると生きている
		//	スレッドが解放済みの領域に触るので、後始末をやめる。
		//	プロセス終了時に OS がまとめて回収する
		//	(要求の処理中に AviUtl2 側の呼び出しでブロックした場合に起きる)。
		if (Result != WAIT_OBJECT_0) {
			LogWarn(L"TSMemory: 待ち受けスレッドが終了しない為、"
					L"連携用オブジェクトの解放を見送りました");
			return;
		}
	}

	if (g_State.pParam != nullptr) {
		::UnmapViewOfFile(g_State.pParam);
		g_State.pParam = nullptr;
	}

	HANDLE *const handles[] = {
		&g_State.hQuitEvent, &g_State.hProjectReady, &g_State.hRequestEvent,
		&g_State.hParamMap, &g_State.hParamMutex, &g_State.hReadyMutex,
	};
	for (HANDLE *p : handles) {
		if (*p != nullptr) {
			::CloseHandle(*p);
			*p = nullptr;
		}
	}
}
