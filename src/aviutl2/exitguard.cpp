//----------------------------------------------------------------------------
//	終了時の保存確認ダイアログの自動応答 (既定では無効)
//
//	AviUtl2 には編集済みフラグを解除する API が無い
//	(EDIT_SECTION::set_edited_state() は設定専用) 為、
//	ダイアログを検出して応答する以外に抑止する手段が無い。
//	本体の動作に割り込む形になるので、下記の条件を満たす時だけ応答する。
//
//	 ・ini で明示的に有効にしている
//	 ・TSMemory がオブジェクトを配置している
//	 ・その後にユーザーの編集が検出されていない (SuppressExitConfirm=1 のみ)
//
//	SuppressExitConfirm:
//	  0 : 何もしない (既定)
//	  1 : TSMemory が配置しただけの状態に限り応答する
//	  2 : TSMemory が配置した後であれば、編集していても応答する
//	      ※ 編集内容は保存されずに破棄される。キャプチャを見るだけの
//	         使い方で、確認が毎回出るのが煩わしい場合に使う。
//----------------------------------------------------------------------------
#include <windows.h>

#include "plugin2.h"
#include "logger2.h"

#include <shlwapi.h>

//	StringCchPrintfW を使う。lstrcpynW 等は引き続き使うので非推奨化はしない
#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#include "inifile.h"
#include "exitguard.h"

namespace {

//	TSMemory の配置直後は自分が起こした更新通知が届くので、その間は
//	ユーザーの編集と見なさない (ミリ秒)
constexpr DWORD PLACEMENT_WINDOW = 2000;

struct ExitGuardState {
	LOG_HANDLE *Logger = nullptr;

	int Mode = 0;					// SuppressExitConfirm の値 (0/1/2)
	volatile LONG Placed = 0;		// TSMemory が配置したか
	volatile LONG UserEdited = 0;	// その後にユーザーが編集したか (Mode==1 のみ)
	volatile LONG PlacedTick = 0;	// 配置した時刻 (GetTickCount)

	HANDLE hThread = nullptr;
	DWORD ThreadID = 0;

	WCHAR szMatchText[128] = L"現在の編集データは更新されています";
	WCHAR szButtonText[64] = L"いいえ";
};

ExitGuardState g_State;

void Log(LPCWSTR message)
{
	if (g_State.Logger != nullptr)
		g_State.Logger->log(g_State.Logger, message);
}

//---------------------------------------------------------------------------
//	ユーザーの編集の検出
//---------------------------------------------------------------------------
void CALLBACK OnUpdateObject(void *)
{
	//	自分が配置した直後の通知は無視する
	const DWORD Tick = ::GetTickCount();
	const DWORD Placed = static_cast<DWORD>(::InterlockedCompareExchange(&g_State.PlacedTick, 0, 0));
	if (Placed != 0 && Tick - Placed < PLACEMENT_WINDOW)
		return;

	::InterlockedExchange(&g_State.UserEdited, 1);
}

//---------------------------------------------------------------------------
//	ダイアログの検出
//---------------------------------------------------------------------------
struct DialogScan {
	bool fTextFound = false;	// 目的の文言が見つかったか
	HWND hwndButton = nullptr;	// 押すべきボタン
	WCHAR szContents[512] = {};	// 見つからなかった時の診断用
};

void AppendContents(DialogScan *pScan, LPCWSTR pszText)
{
	const int Length = ::lstrlenW(pScan->szContents);
	if (Length + ::lstrlenW(pszText) + 4 >= 512)
		return;
	if (Length > 0)
		::lstrcatW(pScan->szContents, L" / ");
	::lstrcatW(pScan->szContents, pszText);
}

BOOL CALLBACK ScanDialogProc(HWND hwnd, LPARAM lParam)
{
	DialogScan *pScan = reinterpret_cast<DialogScan *>(lParam);
	WCHAR szClass[64] = {}, szText[512] = {};

	::GetClassNameW(hwnd, szClass, 64);
	if (::GetWindowTextW(hwnd, szText, 512) <= 0)
		return TRUE;

	AppendContents(pScan, szText);

	if (::StrStrIW(szText, g_State.szMatchText) != nullptr)
		pScan->fTextFound = true;

	//	応答するボタンを探す (見つからなければ WM_COMMAND で代用する)
	if (pScan->hwndButton == nullptr
			&& ::lstrcmpiW(szClass, L"Button") == 0
			&& ::StrStrIW(szText, g_State.szButtonText) != nullptr) {
		pScan->hwndButton = hwnd;
	}

	return TRUE;
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
						   LONG idObject, LONG, DWORD, DWORD)
{
	if (event != EVENT_SYSTEM_DIALOGSTART || hwnd == nullptr || idObject != OBJID_WINDOW)
		return;

	//	自プロセスのダイアログのみ
	DWORD ProcessID = 0;
	::GetWindowThreadProcessId(hwnd, &ProcessID);
	if (ProcessID != ::GetCurrentProcessId())
		return;

	if (::InterlockedCompareExchange(&g_State.Placed, 0, 0) == 0)
		return;

	//	SuppressExitConfirm=2 は編集していても応答する
	if (g_State.Mode < 2
			&& ::InterlockedCompareExchange(&g_State.UserEdited, 0, 0) != 0) {
		Log(L"TSMemory: 編集されているので終了時の確認には応答しません");
		return;
	}

	DialogScan Scan;
	WCHAR szTitle[256] = {};
	if (::GetWindowTextW(hwnd, szTitle, 256) > 0)
		AppendContents(&Scan, szTitle);
	if (::StrStrIW(szTitle, g_State.szMatchText) != nullptr)
		Scan.fTextFound = true;
	::EnumChildWindows(hwnd, ScanDialogProc, reinterpret_cast<LPARAM>(&Scan));

	if (!Scan.fTextFound) {
		//	外した時に何を見ていたのかが判るように残す
		//	(ExitConfirmText / ExitConfirmButton の調整に使う)
		WCHAR szMessage[640];
		::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage), L"TSMemory: 対象外のダイアログを検出しました : %s",
					Scan.szContents);
		Log(szMessage);
		return;
	}

	if (Scan.hwndButton != nullptr) {
		::PostMessageW(Scan.hwndButton, BM_CLICK, 0, 0);
	} else {
		//	ボタンが見つからない場合は MessageBox 想定で IDNO を送る
		::PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
	}

	WCHAR szMessage[128];
	::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage), L"TSMemory: 終了時の保存確認に「%s」で応答しました",
				g_State.szButtonText);
	Log(szMessage);
}

//---------------------------------------------------------------------------
//	フック用スレッド (WINEVENT_OUTOFCONTEXT はメッセージループが要る)
//---------------------------------------------------------------------------
DWORD WINAPI HookThread(LPVOID)
{
	HWINEVENTHOOK hHook = ::SetWinEventHook(
		EVENT_SYSTEM_DIALOGSTART, EVENT_SYSTEM_DIALOGSTART,
		nullptr, WinEventProc, ::GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);

	if (hHook == nullptr) {
		Log(L"TSMemory: 終了時の確認を監視出来ませんでした");
		return 0;
	}

	MSG msg;
	while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
		::TranslateMessage(&msg);
		::DispatchMessageW(&msg);
	}

	::UnhookWinEvent(hHook);
	return 0;
}

}	// namespace

bool TSMemoryExitGuardStart(HOST_APP_TABLE *host, LOG_HANDLE *logger, LPCWSTR ini_file)
{
	g_State.Logger = logger;

	if (host == nullptr || ini_file == nullptr)
		return false;

	g_State.Mode =
		::GetPrivateProfileIntW(L"Bridge", L"SuppressExitConfirm", 0, ini_file);
	if (g_State.Mode <= 0) {
		g_State.Mode = 0;
		return false;
	}
	if (g_State.Mode > 2)
		g_State.Mode = 2;

	//	どちらも日本語の為、UTF-8 対応の読み出しを使う
	TSMemoryGetIniString(ini_file, L"Bridge", L"ExitConfirmText",
						 L"現在の編集データは更新されています",
						 g_State.szMatchText, 128);
	TSMemoryGetIniString(ini_file, L"Bridge", L"ExitConfirmButton", L"いいえ",
						 g_State.szButtonText, 64);

	//	ユーザーの編集を検出する為にオブジェクトの更新を監視する。
	//	Mode==2 は編集の有無を見ないので監視しない。
	if (g_State.Mode < 2)
		host->register_event_listener(EVENT_TYPE::UPDATE_OBJECT, nullptr, OnUpdateObject);

	g_State.hThread = ::CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_State.ThreadID);
	if (g_State.hThread == nullptr)
		return false;

	if (g_State.Mode >= 2) {
		Log(L"TSMemory: 終了時の保存確認を自動応答します "
			L"(編集していても応答します。編集内容は保存されません)");
	} else {
		Log(L"TSMemory: 終了時の保存確認を自動応答します "
			L"(TSMemory が配置しただけの状態に限る)");
	}
	return true;
}

void TSMemoryExitGuardNotifyPlaced()
{
	if (g_State.Mode == 0)
		return;

	//	配置直後の更新通知を自分の物と見なす為に時刻を先に入れる
	DWORD Tick = ::GetTickCount();
	if (Tick == 0)
		Tick = 1;
	::InterlockedExchange(&g_State.PlacedTick, static_cast<LONG>(Tick));
	::InterlockedExchange(&g_State.UserEdited, 0);
	::InterlockedExchange(&g_State.Placed, 1);
}

void TSMemoryExitGuardStop()
{
	if (g_State.hThread != nullptr) {
		::PostThreadMessageW(g_State.ThreadID, WM_QUIT, 0, 0);
		::WaitForSingleObject(g_State.hThread, 5000);
		::CloseHandle(g_State.hThread);
		g_State.hThread = nullptr;
	}
}
