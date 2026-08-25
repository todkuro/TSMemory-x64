//----------------------------------------------------------------------------
//	AviUtl ExEdit2 の代わりに TVTest からの読み込み要求を受け取り、
//	渡された .tvtv が実際にデコード出来るかを確認する。
//
//	  test_receiver <aux2-path> <out-prefix> [timeout-sec]
//
//	TSMemory-TVTestSrc.aux2 の待ち受け側と同じ名前付きオブジェクトを作って待機し、
//	要求が来たらその .tvtv を入力プラグインで開いてフレームを取り出す。
//	つまり TVTest プラグイン側 (ストリーム取り込み → スナップショット → 連携)
//	から入力プラグインまでを通しで確認する事になる。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cmath>
#include <cstdlib>

#include "plugin2.h"
#include "input2.h"

#include "tsmemory_ipc.h"
#include "ts_pts.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

//---------------------------------------------------------------------------
//	ホスト側のふり (入力プラグイン構造体を受け取るだけ)
//---------------------------------------------------------------------------
INPUT_PLUGIN_TABLE *g_pInputPluginTable = nullptr;
EDIT_HANDLE g_EditHandle = {};
HOST_APP_TABLE g_Host = {};

void fake_register_input_plugin(INPUT_PLUGIN_TABLE *table) { g_pInputPluginTable = table; }
void fake_register_window_client(LPCWSTR, HWND) {}
void (*g_pProjectLoadHandler)(PROJECT_FILE *) = nullptr;
void fake_register_project_load_handler(void (*func)(PROJECT_FILE *)) { g_pProjectLoadHandler = func; }
void fake_register_export_menu_param(LPCWSTR, void *, void (*)(void *)) {}
EDIT_HANDLE *fake_create_edit_handle() { return &g_EditHandle; }

//---------------------------------------------------------------------------
//	YUY2 -> RGB24 (トップダウン)
//---------------------------------------------------------------------------
BYTE Clip(int v) { return static_cast<BYTE>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void Yuy2ToRgb(const BYTE *pSrc, int Width, int Height, int Pitch, std::vector<BYTE> *pDest)
{
	pDest->resize(static_cast<size_t>(Width) * Height * 3);

	for (int y = 0; y < Height; y++) {
		const BYTE *p = pSrc + static_cast<size_t>(y) * Pitch;
		BYTE *q = pDest->data() + static_cast<size_t>(y) * Width * 3;

		for (int x = 0; x < Width; x += 2) {
			const int y0 = p[0], u = p[1], y1 = p[2], v = p[3];
			p += 4;
			for (int i = 0; i < 2 && x + i < Width; i++) {
				const int yy = (i == 0 ? y0 : y1) - 16;
				const int uu = u - 128, vv = v - 128;
				*q++ = Clip((298 * yy + 409 * vv + 128) >> 8);
				*q++ = Clip((298 * yy - 100 * uu - 208 * vv + 128) >> 8);
				*q++ = Clip((298 * yy + 516 * uu + 128) >> 8);
			}
		}
	}
}

double PixelStdDev(const std::vector<BYTE> &Rgb)
{
	if (Rgb.empty())
		return 0.0;
	double sum = 0.0, sum2 = 0.0;
	for (BYTE b : Rgb) {
		sum += b;
		sum2 += static_cast<double>(b) * b;
	}
	const double mean = sum / Rgb.size();
	const double var = sum2 / Rgb.size() - mean * mean;
	return var > 0.0 ? std::sqrt(var) : 0.0;
}

//	元の TS ファイルの先頭付近から最初の映像 PTS を得る
bool GetSourceStartPts(const char *pszPath, int64_t *pPts)
{
	FILE *fp = std::fopen(pszPath, "rb");
	if (fp == nullptr)
		return false;

	std::vector<BYTE> Buffer(8 * 1024 * 1024);
	const size_t Read = std::fread(Buffer.data(), 1, Buffer.size(), fp);
	std::fclose(fp);

	int64_t First = PTS_NONE, Last = PTS_NONE;
	if (!ScanVideoPts(Buffer.data(), Read, &First, &Last))
		return false;
	*pPts = First;
	return true;
}

void WriteRaw(const char *path, const std::vector<BYTE> &data)
{
	FILE *fp = std::fopen(path, "wb");
	if (fp != nullptr) {
		std::fwrite(data.data(), 1, data.size(), fp);
		std::fclose(fp);
	}
}

}	// namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const char *pszDll = argc > 1 ? argv[1] : "dist/TSMemory-TVTestSrc.aux2";
	const char *pszPrefix = argc > 2 ? argv[2] : "build/tests/live";
	const int TimeoutSec = argc > 3 ? std::atoi(argv[3]) : 60;
	//	取り込んだ範囲が元動画のどこなのかを調べる為の元 TS (任意)
	const char *pszSourceTs = argc > 4 ? argv[4] : nullptr;

	//	--- 待ち受け側のオブジェクトを作る (bridge.cpp と同じ) --------------
	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;
	TSMemoryInitSecurityAttributes(&sd, &sa);

	HANDLE hReady = ::CreateMutexW(&sa, FALSE, TSMEMORY_IPC_READY_MUTEX);
	check("created the ready mutex", hReady != nullptr && ::GetLastError() != ERROR_ALREADY_EXISTS);
	HANDLE hParamMutex = ::CreateMutexW(&sa, FALSE, TSMEMORY_IPC_PARAM_MUTEX);
	HANDLE hParamMap = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
											0, sizeof(TSMEMORY_REQUEST), TSMEMORY_IPC_PARAM_MAP);
	HANDLE hRequest = ::CreateEventW(&sa, FALSE, FALSE, TSMEMORY_IPC_REQUEST_EVENT);
	check("created the request objects",
		  hParamMutex != nullptr && hParamMap != nullptr && hRequest != nullptr);
	if (g_failures > 0)
		return 1;

	auto *pParam = static_cast<TSMEMORY_REQUEST *>(::MapViewOfFile(hParamMap, FILE_MAP_WRITE, 0, 0, 0));
	if (pParam == nullptr) {
		std::printf("cannot map the parameter area\n");
		return 1;
	}
	::ZeroMemory(pParam, sizeof(TSMEMORY_REQUEST));

	std::printf("waiting for TVTest (up to %d seconds) ...\n", TimeoutSec);

	//	--- 要求を待つ -------------------------------------------------------
	if (::WaitForSingleObject(hRequest, TimeoutSec * 1000) != WAIT_OBJECT_0) {
		check("a request arrived from TSMemory.tvtp", false);
		std::printf("\nFAIL (%d failures)\n", g_failures);
		return 1;
	}
	check("a request arrived from TSMemory.tvtp", true);

	WCHAR szFileName[MAX_PATH];
	DWORD Version = 0, Serial = 0;
	if (::WaitForSingleObject(hParamMutex, 5000) == WAIT_OBJECT_0) {
		Version = pParam->Version;
		Serial = pParam->Serial;
		::lstrcpynW(szFileName, pParam->FileName, MAX_PATH);
		::ReleaseMutex(hParamMutex);
	} else {
		szFileName[0] = L'\0';
	}

	check("the request has the expected version", Version == TSMEMORY_IPC_VERSION);
	std::printf("  serial   : %lu\n", Serial);
	std::printf("  file     : %ls\n", szFileName);
	check("the request carries a file name", szFileName[0] != L'\0');
	check("the dummy .tvtv file exists",
		  ::GetFileAttributesW(szFileName) != INVALID_FILE_ATTRIBUTES);
	if (g_failures > 0) {
		std::printf("\nFAIL (%d failures)\n", g_failures);
		return 1;
	}

	//	--- 取り込まれた量を見る --------------------------------------------
	//	スナップショットの共有メモリは .tvtv のファイル名と同じ名前で作られ、
	//	先頭に BufferInfo { Size, Used, Pos, Reserved } が入っている。
	DWORD CapturedBytes = 0;
	int64_t CapFirstPts = PTS_NONE, CapLastPts = PTS_NONE;
	{
		HANDLE hMap = ::OpenFileMappingW(FILE_MAP_READ, FALSE, ::PathFindFileNameW(szFileName));
		if (hMap != nullptr) {
			const BYTE *pView = static_cast<const BYTE *>(::MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
			if (pView != nullptr) {
				CapturedBytes = reinterpret_cast<const DWORD *>(pView)[1];
				//	スナップショットは Pos=0 で線形化済みなので先頭から読める
				ScanVideoPts(pView + sizeof(DWORD) * 4, CapturedBytes, &CapFirstPts, &CapLastPts);
				::UnmapViewOfFile(pView);
			}
			::CloseHandle(hMap);
		}
	}
	check("the snapshot shared memory is reachable", CapturedBytes > 0);
	std::printf("  captured : %.2f MB (%lu packets)\n",
				CapturedBytes / (1024.0 * 1024.0), CapturedBytes / 188);

	if (CapFirstPts != PTS_NONE) {
		std::printf("  pts span : %.2f sec (from the first to the last video PTS)\n",
					PtsDiffSeconds(CapFirstPts, CapLastPts));

		//	元の TS が判るなら、取り込んだ範囲が元動画のどこなのかを出す
		if (pszSourceTs != nullptr) {
			int64_t SourcePts = PTS_NONE;
			if (GetSourceStartPts(pszSourceTs, &SourcePts)) {
				const double Start = PtsDiffSeconds(SourcePts, CapFirstPts);
				const double End = PtsDiffSeconds(SourcePts, CapLastPts);
				std::printf("  position : the capture covers %.2f - %.2f sec of %s\n",
							Start, End, pszSourceTs);
			} else {
				std::printf("  position : could not read the source PTS from %s\n", pszSourceTs);
			}
		}
	}

	//	--- 入力プラグインで開く --------------------------------------------
	g_Host.register_input_plugin = fake_register_input_plugin;
	g_Host.register_window_client = fake_register_window_client;
	g_Host.register_project_load_handler = fake_register_project_load_handler;
	g_Host.register_export_menu_param = fake_register_export_menu_param;
	g_Host.create_edit_handle = fake_create_edit_handle;

	HMODULE hModule = ::LoadLibraryA(pszDll);
	check("LoadLibrary(TSMemory-TVTestSrc.aux2)", hModule != nullptr);
	if (hModule == nullptr)
		return 1;

	auto pInitializePlugin = reinterpret_cast<bool (*)(DWORD)>(::GetProcAddress(hModule, "InitializePlugin"));
	auto pRegisterPlugin = reinterpret_cast<void (*)(HOST_APP_TABLE *)>(::GetProcAddress(hModule, "RegisterPlugin"));
	pInitializePlugin(0);
	pRegisterPlugin(&g_Host);
	if (g_pInputPluginTable == nullptr) {
		check("input plugin was registered", false);
		return 1;
	}

	INPUT_PLUGIN_TABLE *ip = g_pInputPluginTable;
	INPUT_HANDLE ih = ip->func_open(szFileName);
	check("func_open() on the captured .tvtv succeeded", ih != nullptr);
	if (ih == nullptr) {
		std::printf("\nFAIL (%d failures)\n", g_failures);
		return 1;
	}

	INPUT_INFO Info = {};
	check("func_info_get() succeeded", ip->func_info_get(ih, &Info));
	const bool fHasVideo = (Info.flag & INPUT_INFO::FLAG_VIDEO) != 0;
	check("the captured stream has video", fHasVideo);

	if (fHasVideo) {
		std::printf("  decoded  : %ldx%ld  %d/%d fps  %d frames\n",
					Info.format->biWidth, Info.format->biHeight,
					Info.rate, Info.scale, Info.n);
		check("captured frame count is positive", Info.n > 0);

		if (Info.rate > 0 && Info.scale > 0 && CapturedBytes > 0) {
			const double Seconds = static_cast<double>(Info.n) * Info.scale / Info.rate;
			std::printf("  duration : %.2f sec (%.1f Mbps)\n",
						Seconds, CapturedBytes * 8.0 / Seconds / 1000000.0);
		}

		const int Width = Info.format->biWidth;
		const int Height = Info.format->biHeight;
		const int Pitch = ((16 * Width + 31) & ~31) >> 3;
		std::vector<BYTE> Frame(static_cast<size_t>(Pitch) * Height + 64);

		const int Frames[] = { 0, Info.n / 2 };
		for (int k = 0; k < 2; k++) {
			const int FrameNo = Frames[k];
			if (FrameNo < 0 || FrameNo >= Info.n)
				continue;

			const int Bytes = ip->func_read_video(ih, FrameNo, Frame.data());
			char what[80];
			std::snprintf(what, sizeof(what), "decoded captured frame %d", FrameNo);
			check(what, Bytes == Pitch * Height);
			if (Bytes <= 0)
				continue;

			std::vector<BYTE> Rgb;
			Yuy2ToRgb(Frame.data(), Width, Height, Pitch, &Rgb);

			char path[MAX_PATH];
			std::snprintf(path, sizeof(path), "%s%d.raw", pszPrefix, FrameNo);
			WriteRaw(path, Rgb);

			const double StdDev = PixelStdDev(Rgb);
			std::snprintf(what, sizeof(what), "captured frame %d is not a flat image", FrameNo);
			check(what, StdDev > 5.0);
			std::printf("  frame %d stddev = %.1f -> %s\n", FrameNo, StdDev, path);
		}

		char path[MAX_PATH];
		std::snprintf(path, sizeof(path), "%s.txt", pszPrefix);
		FILE *fp = std::fopen(path, "w");
		if (fp != nullptr) {
			std::fprintf(fp, "%d %d\n", Width, Height);
			std::fclose(fp);
		}
	}

	check("func_close() succeeded", ip->func_close(ih));

	::UnmapViewOfFile(pParam);
	::CloseHandle(hRequest);
	::CloseHandle(hParamMap);
	::CloseHandle(hParamMutex);
	::CloseHandle(hReady);

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
