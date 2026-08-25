//----------------------------------------------------------------------------
//	キャプチャ・ユーティリティ (AviUtl 1.xx 版の CaptureUtil.auf 相当)
//
//	現在のフレームをレンダリングして画像ファイルに保存する。
//	AviUtl 1.xx 版は TVTest_Image.dll を使っていたが、こちらは Windows 標準の
//	WIC (Windows Imaging Component) を使うので追加 DLL は要らない。
//----------------------------------------------------------------------------
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <wincodec.h>

//	StringCchPrintfW を使う。lstrcpynW 等は引き続き使うので非推奨化はしない
#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#include <string>
#include <vector>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"
#include "filter2.h"

#include "inifile.h"
#include "capture.h"
#include "plugin_main.h"

namespace {

#define CAPTURE_WINDOW_CLASS	L"TSMemoryCaptureUtility"

enum {
	IDC_FILENAME = 1000,
	IDC_FORMAT,
	IDC_QUALITY,
	IDC_SAVE,
};

struct FormatInfo {
	LPCWSTR Name;
	LPCWSTR Extension;
	const GUID *ContainerFormat;
	bool HasQuality;
};

const FormatInfo g_Formats[] = {
	{ L"png",  L"png", &GUID_ContainerFormatPng,  false },
	{ L"jpeg", L"jpg", &GUID_ContainerFormatJpeg, true  },
	{ L"bmp",  L"bmp", &GUID_ContainerFormatBmp,  false },
	{ L"tiff", L"tif", &GUID_ContainerFormatTiff, false },
};
const int g_FormatCount = static_cast<int>(sizeof(g_Formats) / sizeof(g_Formats[0]));

struct CaptureState {
	EDIT_HANDLE *Edit = nullptr;
	LOG_HANDLE *Logger = nullptr;
	CONFIG_HANDLE *Config = nullptr;

	HWND hwnd = nullptr;
	HWND hwndFileName = nullptr;
	HWND hwndFormat = nullptr;
	HWND hwndQuality = nullptr;
	HWND hwndSave = nullptr;

	HINSTANCE hinst = nullptr;		// ウィンドウクラス登録に使ったインスタンス

	//	Lock を初期化したか。一度立てたら下ろさない
	//	(下ろして DeleteCriticalSection() すると、判定を通った直後の
	//	 レンダリング完了通知スレッドが破棄済みの CS に入ってしまう)
	bool fLockInitialized = false;

	WCHAR szIniFileName[MAX_PATH] = {};
	WCHAR szSaveFileName[MAX_PATH] = {};
	int Format = 0;
	int Quality = 90;
	bool CopyFileName = false;

	//	レンダリング完了コールバックへ渡す保存先
	CRITICAL_SECTION Lock;
	WCHAR szPendingFileName[MAX_PATH] = {};
	int PendingFormat = 0;
	int PendingQuality = 90;
	bool Pending = false;
	bool fShutdown = false;			// 後始末済み。Lock の中でのみ読み書きする
};

CaptureState g_State;

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

LPCWSTR Translate(LPCWSTR text)
{
	if (g_State.Config == nullptr)
		return text;
	return g_State.Config->translate(g_State.Config, text);
}

int LayoutSize(LPCSTR key, int def)
{
	if (g_State.Config == nullptr)
		return def;
	const int Size = g_State.Config->get_layout_size(g_State.Config, key);
	return Size > 0 ? Size : def;
}

//----------------------------------------------------------------------------
//	WIC による保存
//----------------------------------------------------------------------------
bool SaveImageFile(LPCWSTR pszFileName, int FormatIndex, int Quality,
				   const void *pBuffer, int Width, int Height, int Pitch)
{
	if (FormatIndex < 0 || FormatIndex >= g_FormatCount)
		return false;

	const bool fComInitialized =
		SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

	bool fOK = false;
	IWICImagingFactory *pFactory = nullptr;
	IWICBitmapEncoder *pEncoder = nullptr;
	IWICBitmapFrameEncode *pFrame = nullptr;
	IPropertyBag2 *pProperties = nullptr;
	IWICStream *pStream = nullptr;
	IWICBitmap *pBitmap = nullptr;

	//	レンダリング結果は PIXEL_RGBA (メモリ上で R,G,B,A の順)。
	//	これを WIC で最も広く扱える 24bppBGR (メモリ上で B,G,R の順) に直す。
	//	アルファは落とす (AviUtl 1.xx 版の CaptureUtil も 24bit RGB だった)。
	const UINT Stride = (static_cast<UINT>(Width) * 3 + 3) & ~3u;
	std::vector<BYTE> Buffer(static_cast<size_t>(Stride) * Height);

	for (int y = 0; y < Height; y++) {
		const BYTE *pSrc = static_cast<const BYTE *>(pBuffer) + static_cast<size_t>(y) * Pitch;
		BYTE *pDst = Buffer.data() + static_cast<size_t>(y) * Stride;
		for (int x = 0; x < Width; x++) {
			pDst[0] = pSrc[2];	// B
			pDst[1] = pSrc[1];	// G
			pDst[2] = pSrc[0];	// R
			pSrc += 4;
			pDst += 3;
		}
	}

	//	エンコーダが対応する形式は種類ごとに違う (jpeg はアルファ非対応等)。
	//	SetPixelFormat() は要求した形式が使えない場合に別の形式へ差し替えるので、
	//	自前で WritePixels() せずにビットマップを渡して WIC に変換させる。
	//	どの段階で失敗したかが判るようにログを残す。
	LPCWSTR pszStep = L"";
	HRESULT hr = S_OK;

	auto Step = [&](LPCWSTR name, HRESULT result) {
		if (SUCCEEDED(hr)) {
			hr = result;
			if (FAILED(hr))
				pszStep = name;
		}
		return SUCCEEDED(hr);
	};

	if (Step(L"CoCreateInstance(WICImagingFactory)",
			 ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
								IID_PPV_ARGS(&pFactory)))
			&& Step(L"CreateBitmapFromMemory",
					pFactory->CreateBitmapFromMemory(
						Width, Height, GUID_WICPixelFormat24bppBGR, Stride,
						static_cast<UINT>(Buffer.size()), Buffer.data(), &pBitmap))
			&& Step(L"CreateStream", pFactory->CreateStream(&pStream))
			&& Step(L"InitializeFromFilename",
					pStream->InitializeFromFilename(pszFileName, GENERIC_WRITE))
			&& Step(L"CreateEncoder",
					pFactory->CreateEncoder(*g_Formats[FormatIndex].ContainerFormat,
											nullptr, &pEncoder))
			&& Step(L"Encoder::Initialize", pEncoder->Initialize(pStream, WICBitmapEncoderNoCache))
			&& Step(L"CreateNewFrame", pEncoder->CreateNewFrame(&pFrame, &pProperties))) {

		if (g_Formats[FormatIndex].HasQuality && pProperties != nullptr) {
			PROPBAG2 option = {};
			option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
			VARIANT value;
			::VariantInit(&value);
			value.vt = VT_R4;
			value.fltVal = static_cast<float>(Quality) / 100.0f;
			pProperties->Write(1, &option, &value);
		}

		WICPixelFormatGUID PixelFormat = GUID_WICPixelFormat24bppBGR;
		if (Step(L"Frame::Initialize", pFrame->Initialize(pProperties))
				&& Step(L"SetSize", pFrame->SetSize(Width, Height))
				&& Step(L"SetPixelFormat", pFrame->SetPixelFormat(&PixelFormat))
				&& Step(L"WriteSource", pFrame->WriteSource(pBitmap, nullptr))
				&& Step(L"Frame::Commit", pFrame->Commit())
				&& Step(L"Encoder::Commit", pEncoder->Commit())) {
			fOK = true;
		}
	}

	if (!fOK) {
		WCHAR szMessage[MAX_PATH + 256];
		::StringCchPrintfW(szMessage, MAX_PATH + 256,
						   L"TSMemory: 画像の保存に失敗しました (%s hr=0x%08X) : %s",
						   pszStep, static_cast<unsigned int>(hr), pszFileName);
		LogWarn(szMessage);
	}

	if (pProperties != nullptr) pProperties->Release();
	if (pFrame != nullptr) pFrame->Release();
	if (pEncoder != nullptr) pEncoder->Release();
	if (pStream != nullptr) pStream->Release();
	if (pBitmap != nullptr) pBitmap->Release();
	if (pFactory != nullptr) pFactory->Release();

	if (fComInitialized)
		::CoUninitialize();

	return fOK;
}

void CopyToClipboard(LPCWSTR pszText)
{
	if (!::OpenClipboard(g_State.hwnd))
		return;

	::EmptyClipboard();
	const size_t Size = (::lstrlenW(pszText) + 1) * sizeof(WCHAR);
	HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE, Size);
	if (hGlobal != nullptr) {
		::CopyMemory(::GlobalLock(hGlobal), pszText, Size);
		::GlobalUnlock(hGlobal);
		::SetClipboardData(CF_UNICODETEXT, hGlobal);
	}
	::CloseClipboard();
}

//	レンダリング完了コールバック (イベント通知スレッドから呼ばれる)
void OnRenderingVideo(void * /*param*/, int /*frame*/, const void *buffer,
					  int width, int height, int pitch)
{
	WCHAR szFileName[MAX_PATH];
	int Format, Quality;

	//	CS を作る前なら触れない。fLockInitialized は一度立つと下りないので、
	//	ここを通った後に CS が消える事は無い
	if (!g_State.fLockInitialized)
		return;

	::EnterCriticalSection(&g_State.Lock);
	//	後始末済みなら何もしない (アンロード間際に呼ばれる可能性がある)
	const bool fPending = g_State.Pending && !g_State.fShutdown;
	const bool fCopyFileName = g_State.CopyFileName;
	::lstrcpynW(szFileName, g_State.szPendingFileName, MAX_PATH);
	Format = g_State.PendingFormat;
	Quality = g_State.PendingQuality;
	g_State.Pending = false;
	::LeaveCriticalSection(&g_State.Lock);

	if (!fPending || buffer == nullptr || width <= 0 || height <= 0)
		return;

	if (SaveImageFile(szFileName, Format, Quality, buffer, width, height, pitch)) {
		WCHAR szMessage[MAX_PATH + 64];
		::StringCchPrintfW(szMessage, MAX_PATH + 64,
						   L"TSMemory: 画像を保存しました : %s", szFileName);
		Log(szMessage);
		if (fCopyFileName)
			CopyToClipboard(szFileName);
	}
	//	ここはレンダリング完了通知スレッドなので、
	//	モーダルダイアログは出さずにログだけに残す (失敗理由は SaveImageFile が出力する)
}

//	重複しないファイル名を作る (拡張子の前に連番を付ける)
void MakeUniqueFileName(LPWSTR pszFileName, size_t cchFileName, int FormatIndex)
{
	LPWSTR pszExtension = ::PathFindExtensionW(pszFileName);
	if (*pszExtension == L'\0') {
		//	書ける残りの分しか足さない。ini の FileName が長いと溢れる
		const size_t Used = static_cast<size_t>(pszExtension - pszFileName);
		::StringCchPrintfW(pszExtension, cchFileName - Used, L".%s",
						   g_Formats[FormatIndex].Extension);
		pszExtension = ::PathFindExtensionW(pszFileName);
	}

	if (!::PathFileExistsW(pszFileName))
		return;

	WCHAR szBase[MAX_PATH], szExtension[MAX_PATH];
	::lstrcpynW(szExtension, pszExtension, MAX_PATH);
	::lstrcpynW(szBase, pszFileName, static_cast<int>(pszExtension - pszFileName) + 1);

	for (int i = 1; i < 100000; i++) {
		WCHAR szPath[MAX_PATH];
		//	切り詰めた候補は元の名前と衝突しかねないので、
		//	収まらない時は連番を諦めてそのまま返す
		if (FAILED(::StringCchPrintfW(szPath, MAX_PATH, L"%s%d%s", szBase, i, szExtension)))
			return;
		if (!::PathFileExistsW(szPath)) {
			::lstrcpynW(pszFileName, szPath, static_cast<int>(cchFileName));
			return;
		}
	}
}

//	現在のフレームを保存する。
//	ウィンドウが無くても動くようにここには UI を持ち込まない。
//	fQuiet が true の時はメッセージボックスを出さずログだけに残す。
bool SaveCurrentFrame(LPCWSTR pszFileName, int Format, int Quality, bool fQuiet)
{
	if (g_State.Edit == nullptr)
		return false;

	if (pszFileName == nullptr || pszFileName[0] == L'\0') {
		if (!fQuiet) {
			::MessageBoxW(g_State.hwnd, Translate(L"ファイル名を入力してください。"),
						  nullptr, MB_OK | MB_ICONEXCLAMATION);
		}
		LogWarn(L"TSMemory: 保存先のファイル名が設定されていません");
		return false;
	}
	if (Format < 0 || Format >= g_FormatCount)
		return false;

	WCHAR szFileName[MAX_PATH];
	::lstrcpynW(szFileName, pszFileName, MAX_PATH);
	MakeUniqueFileName(szFileName, MAX_PATH, Format);

	//	保存先のフォルダが無ければ作る
	WCHAR szDirectory[MAX_PATH];
	::lstrcpynW(szDirectory, szFileName, MAX_PATH);
	::PathRemoveFileSpecW(szDirectory);
	if (szDirectory[0] != L'\0' && !::PathIsDirectoryW(szDirectory))
		::SHCreateDirectoryExW(nullptr, szDirectory, nullptr);

	EDIT_INFO Info = {};
	g_State.Edit->get_edit_info(&Info, sizeof(Info));

	::EnterCriticalSection(&g_State.Lock);
	::lstrcpynW(g_State.szPendingFileName, szFileName, MAX_PATH);
	g_State.PendingFormat = Format;
	g_State.PendingQuality = Quality;
	g_State.Pending = true;
	::LeaveCriticalSection(&g_State.Lock);

	//	レンダリングは非同期。完了時に OnRenderingVideo() が呼ばれる。
	if (!g_State.Edit->rendering_scene_video(Info.frame, nullptr, OnRenderingVideo)) {
		::EnterCriticalSection(&g_State.Lock);
		g_State.Pending = false;
		::LeaveCriticalSection(&g_State.Lock);
		LogWarn(L"TSMemory: 画像を取得出来ませんでした");
		if (!fQuiet) {
			::MessageBoxW(g_State.hwnd, Translate(L"画像を取得出来ませんでした。"),
						  nullptr, MB_OK | MB_ICONEXCLAMATION);
		}
		return false;
	}

	return true;
}

void OnSave()
{
	WCHAR szFileName[MAX_PATH];

	::GetWindowTextW(g_State.hwndFileName, szFileName, MAX_PATH);
	const int Format = ComboBox_GetCurSel(g_State.hwndFormat);
	const int Quality = ::GetDlgItemInt(g_State.hwnd, IDC_QUALITY, nullptr, FALSE);

	SaveCurrentFrame(szFileName, Format, Quality, false);
}

void UpdateQualityState()
{
	const int Format = ComboBox_GetCurSel(g_State.hwndFormat);
	::EnableWindow(g_State.hwndQuality,
				   Format >= 0 && Format < g_FormatCount && g_Formats[Format].HasQuality);
}

void LayoutControls(int cx, int cy)
{
	const int ItemHeight = LayoutSize("SettingItemHeight", 24);
	const int Margin = 4;
	const int Row2 = ItemHeight + Margin * 2;

	if (cy < Row2 + ItemHeight + Margin)
		cy = Row2 + ItemHeight + Margin;

	::MoveWindow(g_State.hwndFileName, Margin, Margin, cx - Margin * 2, ItemHeight, TRUE);

	const int ComboWidth = 96;
	const int QualityWidth = 64;
	const int ButtonWidth = 96;
	::MoveWindow(g_State.hwndFormat, Margin, Row2, ComboWidth, ItemHeight * 8, TRUE);
	::MoveWindow(g_State.hwndQuality, Margin * 2 + ComboWidth, Row2, QualityWidth, ItemHeight, TRUE);
	::MoveWindow(g_State.hwndSave, Margin * 3 + ComboWidth + QualityWidth, Row2,
				 ButtonWidth, ItemHeight, TRUE);
	(void)cy;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case WM_CREATE:
		g_State.hwnd = hwnd;

		g_State.hwndFileName = ::CreateWindowExW(
			WS_EX_CLIENTEDGE, WC_EDITW, g_State.szSaveFileName,
			WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
			0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_FILENAME), g_State.hinst, nullptr);
		Edit_LimitText(g_State.hwndFileName, MAX_PATH - 1);

		g_State.hwndFormat = ::CreateWindowExW(
			0, WC_COMBOBOXW, L"",
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
			0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_FORMAT), g_State.hinst, nullptr);
		for (int i = 0; i < g_FormatCount; i++)
			ComboBox_AddString(g_State.hwndFormat, g_Formats[i].Name);
		ComboBox_SetCurSel(g_State.hwndFormat, g_State.Format);

		g_State.hwndQuality = ::CreateWindowExW(
			WS_EX_CLIENTEDGE, WC_EDITW, L"",
			WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT,
			0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_QUALITY), g_State.hinst, nullptr);
		::SetDlgItemInt(hwnd, IDC_QUALITY, g_State.Quality, FALSE);

		g_State.hwndSave = ::CreateWindowExW(
			0, WC_BUTTONW, Translate(L"保存"),
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE), g_State.hinst, nullptr);

		UpdateQualityState();
		return 0;

	case WM_SIZE:
		LayoutControls(LOWORD(lparam), HIWORD(lparam));
		return 0;

	case WM_COMMAND:
		switch (LOWORD(wparam)) {
		case IDC_SAVE:
			OnSave();
			::SetFocus(nullptr);
			return 0;

		case IDC_FORMAT:
			if (HIWORD(wparam) == CBN_SELCHANGE)
				UpdateQualityState();
			return 0;
		}
		break;
	}

	return ::DefWindowProc(hwnd, message, wparam, lparam);
}

void LoadSettings(LPCWSTR ini_file)
{
	::lstrcpynW(g_State.szIniFileName, ini_file, MAX_PATH);

	//	保存先には日本語が入る為、UTF-8 対応の読み出しを使う
	TSMemoryGetIniString(g_State.szIniFileName, L"Capture", L"FileName", L"",
						 g_State.szSaveFileName, MAX_PATH);
	if (g_State.szSaveFileName[0] == L'\0') {
		PWSTR pszPictures = nullptr;
		if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pszPictures))) {
			::lstrcpynW(g_State.szSaveFileName, pszPictures, MAX_PATH);
			::CoTaskMemFree(pszPictures);
			::PathAppendW(g_State.szSaveFileName, L"Capture");
		}
	}

	WCHAR szFormat[16];
	::GetPrivateProfileStringW(L"Capture", L"Format", L"png", szFormat, 16, g_State.szIniFileName);
	g_State.Format = 0;
	for (int i = 0; i < g_FormatCount; i++) {
		if (::lstrcmpiW(szFormat, g_Formats[i].Name) == 0) {
			g_State.Format = i;
			break;
		}
	}

	g_State.Quality = static_cast<int>(
		::GetPrivateProfileIntW(L"Capture", L"JpegQuality", 90, g_State.szIniFileName));
	if (g_State.Quality < 1 || g_State.Quality > 100)
		g_State.Quality = 90;

	g_State.CopyFileName =
		::GetPrivateProfileIntW(L"Capture", L"CopyFileName", 0, g_State.szIniFileName) != 0;
}

void SaveSettings()
{
	if (g_State.szIniFileName[0] == L'\0' || !::IsWindow(g_State.hwnd))
		return;

	WCHAR szFileName[MAX_PATH];
	::GetWindowTextW(g_State.hwndFileName, szFileName, MAX_PATH);
	::WritePrivateProfileStringW(L"Capture", L"FileName", szFileName, g_State.szIniFileName);

	const int Format = ComboBox_GetCurSel(g_State.hwndFormat);
	if (Format >= 0 && Format < g_FormatCount)
		::WritePrivateProfileStringW(L"Capture", L"Format", g_Formats[Format].Name, g_State.szIniFileName);

	WCHAR szValue[16];
	::StringCchPrintfW(szValue, ARRAYSIZE(szValue), L"%d",
					   ::GetDlgItemInt(g_State.hwnd, IDC_QUALITY, nullptr, FALSE));
	::WritePrivateProfileStringW(L"Capture", L"JpegQuality", szValue, g_State.szIniFileName);
}

}	// namespace

bool TSMemoryCaptureRegister(HOST_APP_TABLE *host, EDIT_HANDLE *edit,
							 LOG_HANDLE *logger, CONFIG_HANDLE *config,
							 LPCWSTR ini_file)
{
	g_State.Edit = edit;
	g_State.Logger = logger;
	g_State.Config = config;

	if (host == nullptr || edit == nullptr)
		return false;

	//	Uninitialize() では破棄しないので、二度目は作り直さない
	if (!g_State.fLockInitialized) {
		::InitializeCriticalSection(&g_State.Lock);
		g_State.fLockInitialized = true;
	}
	::EnterCriticalSection(&g_State.Lock);
	g_State.fShutdown = false;
	::LeaveCriticalSection(&g_State.Lock);

	LoadSettings(ini_file);

	//	ウィンドウクラスは自分の DLL のインスタンスで登録する。
	//	アンロード時に確実に登録解除する為 (残すとウィンドウプロシージャが
	//	解放済みのコードを指したままになり、次のメッセージで落ちる)。
	g_State.hinst = TSMemoryGetModuleHandle();
	if (g_State.hinst == nullptr)
		g_State.hinst = ::GetModuleHandle(nullptr);

	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof(wcex);
	wcex.lpszClassName = CAPTURE_WINDOW_CLASS;
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = g_State.hinst;
	wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	wcex.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
	if (::RegisterClassExW(&wcex) == 0) {
		LogWarn(L"TSMemory: ウィンドウクラスを登録出来ませんでした");
		return false;
	}

	//	親ウィンドウは register_window_client() で設定されるので一旦 WS_POPUP で作る
	HWND hwnd = ::CreateWindowExW(
		0, CAPTURE_WINDOW_CLASS, Translate(L"キャプチャ・ユーティリティ"),
		WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 480, 80,
		nullptr, nullptr, g_State.hinst, nullptr);
	if (hwnd == nullptr) {
		LogWarn(L"TSMemory: キャプチャ・ユーティリティのウィンドウを作成出来ませんでした");
		return false;
	}

	host->register_window_client(Translate(L"キャプチャ・ユーティリティ"), hwnd);
	Log(L"TSMemory: キャプチャ・ユーティリティのウィンドウを登録しました "
		L"(表示メニューから表示・非表示を切り替えられます)");

	//	ウィンドウを使わずに保存出来るようにファイルメニューにも追加する
	host->register_export_menu_param(
		Translate(L"画像として保存 (TSMemory)"), nullptr,
		[](void *) {
			WCHAR szFileName[MAX_PATH];
			int Format = g_State.Format;
			int Quality = g_State.Quality;

			//	ウィンドウがあればその内容を優先する
			if (::IsWindow(g_State.hwndFileName)) {
				::GetWindowTextW(g_State.hwndFileName, szFileName, MAX_PATH);
				const int Sel = ComboBox_GetCurSel(g_State.hwndFormat);
				if (Sel >= 0 && Sel < g_FormatCount)
					Format = Sel;
				Quality = ::GetDlgItemInt(g_State.hwnd, IDC_QUALITY, nullptr, FALSE);
			} else {
				::lstrcpynW(szFileName, g_State.szSaveFileName, MAX_PATH);
			}

			SaveCurrentFrame(szFileName, Format, Quality, false);
		});
	Log(L"TSMemory: ファイルメニューに「画像として保存 (TSMemory)」を追加しました");

	return true;
}

void TSMemoryCaptureUninitialize()
{
	SaveSettings();

	//	ウィンドウとウィンドウクラスは必ず後始末する。
	//
	//	パッケージの再インストール等で AviUtl2 がプラグインを差し替える際は
	//	DLL がアンロードされる。ウィンドウを残したままにするとウィンドウ
	//	プロシージャがアンロード済みのコードを指したままになり、その後の
	//	メッセージでアクセス違反になる (TSMemory-TVTestSrc.aux2_unloaded として記録される)。
	if (::IsWindow(g_State.hwnd))
		::DestroyWindow(g_State.hwnd);
	g_State.hwnd = nullptr;
	g_State.hwndFileName = nullptr;
	g_State.hwndFormat = nullptr;
	g_State.hwndQuality = nullptr;
	g_State.hwndSave = nullptr;

	if (g_State.hinst != nullptr) {
		::UnregisterClassW(CAPTURE_WINDOW_CLASS, g_State.hinst);
		g_State.hinst = nullptr;
	}

	//	保存処理が動いていても参照しない様に印を付ける。
	//
	//	rendering_scene_video() は非同期で、完了時に別スレッドから
	//	OnRenderingVideo() が呼ばれる。保存要求が飛んでいる最中に
	//	AviUtl2 が終了するとここと競合する為、
	//	CRITICAL_SECTION は破棄せずプロセス終了まで残す
	//	(破棄すると、判定を通った直後のスレッドが
	//	 破棄済みの CS に EnterCriticalSection() する)。
	if (g_State.fLockInitialized) {
		::EnterCriticalSection(&g_State.Lock);
		g_State.Pending = false;
		g_State.fShutdown = true;
		g_State.Edit = nullptr;
		::LeaveCriticalSection(&g_State.Lock);
	}
}
