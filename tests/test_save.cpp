//----------------------------------------------------------------------------
//	キャプチャ・ユーティリティの画像保存を確認する。
//
//	  test_save <aux2-path> <out-dir>
//
//	AviUtl ExEdit2 のふりをして TSMemory-TVTestSrc.aux2 を読み込み、
//	ファイルメニューのコールバックを呼んで画像を保存させる。
//	レンダリング結果としては既知のテストパターン (PIXEL_RGBA) を渡し、
//	保存されたファイルを読み戻して色が合っているかを確認する。
//
//	※ WIC のエンコーダは種類ごとに対応するピクセル形式が違うため、
//	   ここを間違えると png は R と B が入れ替わり、jpeg は縦縞になる。
//----------------------------------------------------------------------------
#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "plugin2.h"
#include "input2.h"
#include "logger2.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

//---------------------------------------------------------------------------
//	テストパターン (4 分割)
//---------------------------------------------------------------------------
constexpr int TEST_WIDTH = 320;
constexpr int TEST_HEIGHT = 240;

struct Rgb { BYTE r, g, b; };

//	左上=赤 / 右上=緑 / 左下=青 / 右下=灰
Rgb ExpectedColor(int x, int y)
{
	const bool fRight = x >= TEST_WIDTH / 2;
	const bool fBottom = y >= TEST_HEIGHT / 2;
	if (!fBottom && !fRight) return { 255, 0, 0 };
	if (!fBottom && fRight)  return { 0, 255, 0 };
	if (fBottom && !fRight)  return { 0, 0, 255 };
	return { 128, 128, 128 };
}

//---------------------------------------------------------------------------
//	ホスト側のふり
//---------------------------------------------------------------------------
HWND g_hwndWindowClient = nullptr;
void (*g_pExportProc)(void *) = nullptr;
void *g_pExportParam = nullptr;

EDIT_INFO g_EditInfo = {};
EDIT_HANDLE g_EditHandle = {};
HOST_APP_TABLE g_Host = {};

//	プラグインのログをそのまま出す (失敗時の切り分け用)
LOG_HANDLE g_Log = {};

void log_message(LOG_HANDLE *, LPCWSTR message)
{
	//	コンソールの既定コードページでは日本語が落ちるので明示的に変換する
	char szText[1024];
	::WideCharToMultiByte(CP_ACP, 0, message, -1, szText, sizeof(szText), nullptr, nullptr);
	std::printf("  [plugin] %s\n", szText);
}

void fake_register_input_plugin(INPUT_PLUGIN_TABLE *) {}
void fake_register_window_client(LPCWSTR, HWND hwnd) { g_hwndWindowClient = hwnd; }
void fake_register_project_load_handler(void (*)(PROJECT_FILE *)) {}
EDIT_HANDLE *fake_create_edit_handle() { return &g_EditHandle; }

void fake_register_export_menu_param(LPCWSTR, void *param, void (*func)(void *))
{
	g_pExportParam = param;
	g_pExportProc = func;
}

void fake_get_edit_info(EDIT_INFO *info, int info_size)
{
	::CopyMemory(info, &g_EditInfo,
				 static_cast<size_t>(info_size) < sizeof(g_EditInfo)
					? static_cast<size_t>(info_size) : sizeof(g_EditInfo));
}

HWND fake_get_host_app_window() { return nullptr; }

//	レンダリングを依頼されたらその場でテストパターンを返す
bool fake_rendering_scene_video(int frame, void *param,
								void (*func)(void *, int, const void *, int, int, int))
{
	const int Pitch = TEST_WIDTH * 4;
	std::vector<BYTE> Buffer(static_cast<size_t>(Pitch) * TEST_HEIGHT);

	for (int y = 0; y < TEST_HEIGHT; y++) {
		BYTE *p = Buffer.data() + static_cast<size_t>(y) * Pitch;
		for (int x = 0; x < TEST_WIDTH; x++) {
			const Rgb c = ExpectedColor(x, y);
			//	PIXEL_RGBA は r,g,b,a の順
			p[0] = c.r;
			p[1] = c.g;
			p[2] = c.b;
			p[3] = 255;
			p += 4;
		}
	}

	func(param, frame, Buffer.data(), TEST_WIDTH, TEST_HEIGHT, Pitch);
	return true;
}

//---------------------------------------------------------------------------
//	保存されたファイルを読み戻す
//---------------------------------------------------------------------------
bool LoadImage(LPCWSTR pszFileName, std::vector<Rgb> *pPixels, int *pWidth, int *pHeight)
{
	bool fOK = false;
	IWICImagingFactory *pFactory = nullptr;
	IWICBitmapDecoder *pDecoder = nullptr;
	IWICBitmapFrameDecode *pFrame = nullptr;
	IWICFormatConverter *pConverter = nullptr;

	if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
									 IID_PPV_ARGS(&pFactory)))
			&& SUCCEEDED(pFactory->CreateDecoderFromFilename(
				pszFileName, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder))
			&& SUCCEEDED(pDecoder->GetFrame(0, &pFrame))
			&& SUCCEEDED(pFactory->CreateFormatConverter(&pConverter))
			&& SUCCEEDED(pConverter->Initialize(pFrame, GUID_WICPixelFormat24bppBGR,
												WICBitmapDitherTypeNone, nullptr, 0.0,
												WICBitmapPaletteTypeCustom))) {
		UINT w = 0, h = 0;
		pFrame->GetSize(&w, &h);
		const UINT Stride = (w * 3 + 3) & ~3u;
		std::vector<BYTE> Buffer(static_cast<size_t>(Stride) * h);

		if (SUCCEEDED(pConverter->CopyPixels(nullptr, Stride,
											 static_cast<UINT>(Buffer.size()), Buffer.data()))) {
			pPixels->resize(static_cast<size_t>(w) * h);
			for (UINT y = 0; y < h; y++) {
				const BYTE *p = Buffer.data() + static_cast<size_t>(y) * Stride;
				for (UINT x = 0; x < w; x++) {
					(*pPixels)[static_cast<size_t>(y) * w + x] = { p[2], p[1], p[0] };
					p += 3;
				}
			}
			*pWidth = static_cast<int>(w);
			*pHeight = static_cast<int>(h);
			fOK = true;
		}
	}

	if (pConverter != nullptr) pConverter->Release();
	if (pFrame != nullptr) pFrame->Release();
	if (pDecoder != nullptr) pDecoder->Release();
	if (pFactory != nullptr) pFactory->Release();
	return fOK;
}

//	キャプチャ・ユーティリティのウィンドウのコントロール ID
enum { IDC_FILENAME = 1000, IDC_FORMAT, IDC_QUALITY, IDC_SAVE };

struct FormatCase {
	const wchar_t *Name;
	const wchar_t *Extension;
	int Index;			// コンボボックスの位置 (capture.cpp の g_Formats の順)
	int Tolerance;		// 非可逆圧縮の許容誤差
};

const FormatCase g_Cases[] = {
	{ L"png",  L"png", 0, 2 },
	{ L"jpeg", L"jpg", 1, 40 },
	{ L"bmp",  L"bmp", 2, 2 },
	{ L"tiff", L"tif", 3, 2 },
};

bool NearEnough(Rgb a, Rgb b, int tolerance)
{
	return std::abs(a.r - b.r) <= tolerance
		&& std::abs(a.g - b.g) <= tolerance
		&& std::abs(a.b - b.b) <= tolerance;
}

}	// namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

	const char *pszDll = argc > 1 ? argv[1] : "build/tests/save/TSMemory-TVTestSrc.aux2";
	const char *pszOutDir = argc > 2 ? argv[2] : "build/tests/save";

	WCHAR szRelative[MAX_PATH], szOutDir[MAX_PATH];
	::MultiByteToWideChar(CP_ACP, 0, pszOutDir, -1, szRelative, MAX_PATH);
	//	GetFullPathNameW は入出力で同じバッファを使うと結果が壊れる
	if (::GetFullPathNameW(szRelative, MAX_PATH, szOutDir, nullptr) == 0)
		::lstrcpynW(szOutDir, szRelative, MAX_PATH);
	std::printf("output directory : %ls\n", szOutDir);

	g_Host.register_input_plugin = fake_register_input_plugin;
	g_Host.register_window_client = fake_register_window_client;
	g_Host.register_project_load_handler = fake_register_project_load_handler;
	g_Host.register_export_menu_param = fake_register_export_menu_param;
	g_Host.create_edit_handle = fake_create_edit_handle;

	g_EditHandle.get_edit_info = fake_get_edit_info;
	g_EditHandle.get_host_app_window = fake_get_host_app_window;
	g_EditHandle.rendering_scene_video = fake_rendering_scene_video;

	HMODULE hModule = ::LoadLibraryA(pszDll);
	check("LoadLibrary(TSMemory-TVTestSrc.aux2)", hModule != nullptr);
	if (hModule == nullptr) {
		std::printf("  GetLastError() = %lu\n", ::GetLastError());
		return 1;
	}

	g_Log.log = log_message;
	g_Log.info = log_message;
	g_Log.warn = log_message;
	g_Log.error = log_message;
	g_Log.verbose = log_message;
	auto pInitializeLogger = reinterpret_cast<void (*)(LOG_HANDLE *)>(::GetProcAddress(hModule, "InitializeLogger"));
	if (pInitializeLogger != nullptr)
		pInitializeLogger(&g_Log);

	auto pInitializePlugin = reinterpret_cast<bool (*)(DWORD)>(::GetProcAddress(hModule, "InitializePlugin"));
	auto pUninitializePlugin = reinterpret_cast<void (*)()>(::GetProcAddress(hModule, "UninitializePlugin"));
	auto pRegisterPlugin = reinterpret_cast<void (*)(HOST_APP_TABLE *)>(::GetProcAddress(hModule, "RegisterPlugin"));
	pInitializePlugin(0);
	pRegisterPlugin(&g_Host);

	check("the capture window was registered", g_hwndWindowClient != nullptr);
	check("the export menu callback was registered", g_pExportProc != nullptr);
	if (g_hwndWindowClient == nullptr || g_pExportProc == nullptr)
		return 1;

	HWND hwndFileName = ::GetDlgItem(g_hwndWindowClient, IDC_FILENAME);
	HWND hwndFormat = ::GetDlgItem(g_hwndWindowClient, IDC_FORMAT);
	check("the window has the filename / format controls",
		  hwndFileName != nullptr && hwndFormat != nullptr);
	if (hwndFileName == nullptr || hwndFormat == nullptr)
		return 1;

	for (const FormatCase &Case : g_Cases) {
		WCHAR szBase[MAX_PATH], szPath[MAX_PATH];
		::wsprintfW(szBase, L"%s\\save_%s", szOutDir, Case.Name);
		::wsprintfW(szPath, L"%s.%s", szBase, Case.Extension);
		::DeleteFileW(szPath);

		::SetWindowTextW(hwndFileName, szBase);
		::SendMessageW(hwndFormat, CB_SETCURSEL, Case.Index, 0);
		::SetDlgItemInt(g_hwndWindowClient, IDC_QUALITY, 95, FALSE);

		//	ファイルメニューから保存した時と同じ経路を通す
		g_pExportProc(g_pExportParam);

		char what[128];
		std::snprintf(what, sizeof(what), "%ls : the file was created", Case.Name);
		check(what, ::PathFileExistsW(szPath) != FALSE);
		if (!::PathFileExistsW(szPath))
			continue;

		std::vector<Rgb> Pixels;
		int Width = 0, Height = 0;
		std::snprintf(what, sizeof(what), "%ls : the file can be decoded", Case.Name);
		check(what, LoadImage(szPath, &Pixels, &Width, &Height));
		if (Pixels.empty())
			continue;

		std::snprintf(what, sizeof(what), "%ls : the image size is correct", Case.Name);
		check(what, Width == TEST_WIDTH && Height == TEST_HEIGHT);
		if (Width != TEST_WIDTH || Height != TEST_HEIGHT)
			continue;

		//	各象限の中心の色を確認する (R と B が入れ替わっていればここで落ちる)
		bool fColorOK = true;
		const int Points[4][2] = {
			{ TEST_WIDTH / 4,     TEST_HEIGHT / 4 },
			{ TEST_WIDTH * 3 / 4, TEST_HEIGHT / 4 },
			{ TEST_WIDTH / 4,     TEST_HEIGHT * 3 / 4 },
			{ TEST_WIDTH * 3 / 4, TEST_HEIGHT * 3 / 4 },
		};
		for (const auto &pt : Points) {
			const Rgb Expected = ExpectedColor(pt[0], pt[1]);
			const Rgb Actual = Pixels[static_cast<size_t>(pt[1]) * Width + pt[0]];
			if (!NearEnough(Expected, Actual, Case.Tolerance)) {
				fColorOK = false;
				std::printf("    (%d,%d) expected RGB(%d,%d,%d) but got RGB(%d,%d,%d)\n",
							pt[0], pt[1], Expected.r, Expected.g, Expected.b,
							Actual.r, Actual.g, Actual.b);
			}
		}
		std::snprintf(what, sizeof(what), "%ls : the colors are correct (no R/B swap)", Case.Name);
		check(what, fColorOK);

		//	縦縞になっていないか (1 行が単調でない = ストライドのずれ) の確認。
		//	jpeg は色の境界にリンギングが出るので象限の内側だけを見る。
		bool fRowOK = true;
		const int y = TEST_HEIGHT / 4;
		for (int x = 16; x < TEST_WIDTH / 2 - 16; x++) {
			if (!NearEnough(Pixels[static_cast<size_t>(y) * Width + x],
							Pixels[static_cast<size_t>(y) * Width + 16], Case.Tolerance)) {
				fRowOK = false;
				break;
			}
		}
		std::snprintf(what, sizeof(what), "%ls : a flat area stays flat (no stride glitch)", Case.Name);
		check(what, fRowOK);
	}

	pUninitializePlugin();
	::CoUninitialize();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
