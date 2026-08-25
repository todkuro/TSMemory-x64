//----------------------------------------------------------------------------
//	実際の MPEG-2 TS を共有メモリに載せて TSMemory-TVTestSrc.aux2 の入力プラグインで
//	デコード出来るかを確認する。
//
//	  test_decode <ts-file> [out-prefix]
//
//	フレーム 0 とフレーム 30 を <out-prefix>0.raw / <out-prefix>30.raw に
//	RGB24 (トップダウン) で書き出す。サイズは <out-prefix>.txt に出力する。
//----------------------------------------------------------------------------
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "plugin2.h"
#include "input2.h"
#include "logger2.h"

#include "ts_pts.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-52s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

//---------------------------------------------------------------------------
//	ホスト側のふり (入力プラグイン構造体を受け取るだけ)
//---------------------------------------------------------------------------
INPUT_PLUGIN_TABLE *g_pInputPluginTable = nullptr;
EDIT_HANDLE g_EditHandle = {};
HOST_APP_TABLE g_Host = {};

//	プラグインのログを控える (音声を開けなかった理由の確認に使う)
LOG_HANDLE g_Log = {};
std::vector<std::wstring> g_LogLines;

void log_message(LOG_HANDLE *, LPCWSTR message)
{
	g_LogLines.push_back(message);

	//	コンソールの既定コードページでは日本語が落ちるので明示的に変換する
	char szText[1024];
	::WideCharToMultiByte(CP_ACP, 0, message, -1, szText, sizeof(szText), nullptr, nullptr);
	std::printf("  [plugin] %s\n", szText);
}

bool LogContains(LPCWSTR text)
{
	for (const std::wstring &s : g_LogLines) {
		if (s.find(text) != std::wstring::npos)
			return true;
	}
	return false;
}

void fake_register_input_plugin(INPUT_PLUGIN_TABLE *table) { g_pInputPluginTable = table; }
void fake_register_window_client(LPCWSTR, HWND) {}
void (*g_pProjectLoadHandler)(PROJECT_FILE *) = nullptr;
void fake_register_project_load_handler(void (*func)(PROJECT_FILE *)) { g_pProjectLoadHandler = func; }
void fake_register_export_menu_param(LPCWSTR, void *, void (*)(void *)) {}
EDIT_HANDLE *fake_create_edit_handle() { return &g_EditHandle; }

//---------------------------------------------------------------------------
//	TS を TSMemory.tvtp と同じ形の共有メモリに載せる
//---------------------------------------------------------------------------
struct SharedTs {
	HANDLE hMutex = nullptr;
	HANDLE hMap = nullptr;
	void *pView = nullptr;

	~SharedTs()
	{
		if (pView != nullptr) ::UnmapViewOfFile(pView);
		if (hMap != nullptr) ::CloseHandle(hMap);
		if (hMutex != nullptr) ::CloseHandle(hMutex);
	}
};

bool PublishTs(SharedTs *pShm, const char *pszName, const std::vector<BYTE> &Data)
{
	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;

	::ZeroMemory(&sd, sizeof(sd));
	::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	::SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
	::ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = &sd;

	char szMutexName[MAX_PATH];
	std::snprintf(szMutexName, sizeof(szMutexName), "%s.mutex", pszName);

	const DWORD DataSize = static_cast<DWORD>(Data.size());
	const DWORD MapSize = static_cast<DWORD>(sizeof(DWORD) * 4) + DataSize;

	pShm->hMutex = ::CreateMutexA(&sa, FALSE, szMutexName);
	pShm->hMap = ::CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, MapSize, pszName);
	if (pShm->hMutex == nullptr || pShm->hMap == nullptr)
		return false;

	pShm->pView = ::MapViewOfFile(pShm->hMap, FILE_MAP_WRITE, 0, 0, 0);
	if (pShm->pView == nullptr)
		return false;

	DWORD *pInfo = static_cast<DWORD *>(pShm->pView);
	pInfo[0] = DataSize;	// Size
	pInfo[1] = DataSize;	// Used
	pInfo[2] = 0;			// Pos
	pInfo[3] = 0;			// Reserved
	::CopyMemory(static_cast<BYTE *>(pShm->pView) + sizeof(DWORD) * 4, Data.data(), DataSize);
	return true;
}

//---------------------------------------------------------------------------
//	YUY2 -> RGB24 (トップダウン)
//---------------------------------------------------------------------------
BYTE Clip(int v) { return static_cast<BYTE>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void Yuy2ToRgb(const BYTE *pSrc, int Width, int Height, int Pitch,
			   bool fBottomUp, std::vector<BYTE> *pDest)
{
	pDest->resize(static_cast<size_t>(Width) * Height * 3);

	for (int y = 0; y < Height; y++) {
		const int SrcY = fBottomUp ? (Height - 1 - y) : y;
		const BYTE *p = pSrc + static_cast<size_t>(SrcY) * Pitch;
		BYTE *q = pDest->data() + static_cast<size_t>(y) * Width * 3;

		for (int x = 0; x < Width; x += 2) {
			const int y0 = p[0], u = p[1], y1 = p[2], v = p[3];
			p += 4;

			for (int i = 0; i < 2 && x + i < Width; i++) {
				const int yy = (i == 0 ? y0 : y1) - 16;
				const int uu = u - 128;
				const int vv = v - 128;
				*q++ = Clip((298 * yy + 409 * vv + 128) >> 8);			// R
				*q++ = Clip((298 * yy - 100 * uu - 208 * vv + 128) >> 8);	// G
				*q++ = Clip((298 * yy + 516 * uu + 128) >> 8);			// B
			}
		}
	}
}

//	YUY2 のまま輝度・色差の分布を調べる。
//
//	YUY2 は AviUtl2 側で RGB に変換されるので、こちらが渡す値が
//	リミテッドレンジ (Y:16-235 / C:16-240) なのかフルレンジ (0-255) なのかで
//	黒浮き・白飛びが変わる。放送の MPEG-2 はリミテッドレンジ。
void ReportYuy2Range(const BYTE *pSrc, int Width, int Height, int Pitch)
{
	std::vector<int> HistY(256, 0), HistC(256, 0);

	for (int y = 0; y < Height; y++) {
		const BYTE *p = pSrc + static_cast<size_t>(y) * Pitch;
		for (int x = 0; x < Width; x += 2) {
			HistY[p[0]]++;
			HistY[p[2]]++;
			HistC[p[1]]++;
			HistC[p[3]]++;
			p += 4;
		}
	}

	auto Report = [](const char *name, const std::vector<int> &hist, int lo, int hi) {
		long long total = 0;
		for (int v : hist)
			total += v;
		if (total == 0)
			return;

		int MinV = 255, MaxV = 0;
		long long Below = 0, Above = 0;
		for (int v = 0; v < 256; v++) {
			if (hist[v] == 0)
				continue;
			if (v < MinV) MinV = v;
			if (v > MaxV) MaxV = v;
			if (v < lo) Below += hist[v];
			if (v > hi) Above += hist[v];
		}
		std::printf("  %-2s range: %3d - %3d   (outside %d-%d : %.3f%% below / %.3f%% above)\n",
					name, MinV, MaxV, lo, hi,
					Below * 100.0 / total, Above * 100.0 / total);
	};

	Report("Y", HistY, 16, 235);
	Report("UV", HistC, 16, 240);
}

//	自プロセスのスレッド数を数える
int CountThreads()
{
	HANDLE hSnapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE)
		return -1;

	const DWORD ProcessID = ::GetCurrentProcessId();
	int Count = 0;
	THREADENTRY32 te = {};
	te.dwSize = sizeof(te);

	if (::Thread32First(hSnapshot, &te)) {
		do {
			if (te.th32OwnerProcessID == ProcessID)
				Count++;
		} while (::Thread32Next(hSnapshot, &te));
	}

	::CloseHandle(hSnapshot);
	return Count;
}

void WriteRaw(const char *pszPath, const std::vector<BYTE> &Data)
{
	FILE *fp = std::fopen(pszPath, "wb");
	if (fp != nullptr) {
		std::fwrite(Data.data(), 1, Data.size(), fp);
		std::fclose(fp);
	}
}

//	画像がまともにデコードされているかの目安。
//	全面が単色 (デコード失敗で真っ黒/真緑等) でない事を確認する。
bool LooksDecoded(const std::vector<BYTE> &Rgb, double *pStdDev)
{
	if (Rgb.empty())
		return false;

	double sum = 0.0, sum2 = 0.0;
	const size_t n = Rgb.size();
	for (size_t i = 0; i < n; i++) {
		sum += Rgb[i];
		sum2 += static_cast<double>(Rgb[i]) * Rgb[i];
	}
	const double mean = sum / n;
	const double var = sum2 / n - mean * mean;
	*pStdDev = var > 0.0 ? std::sqrt(var) : 0.0;
	return *pStdDev > 5.0;
}

}	// namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const char *pszTsPath = argc > 1 ? argv[1] : "build/ts-examples/sample.ts";
	const char *pszPrefix = argc > 2 ? argv[2] : "build/tests/frame";
	const char *pszDll = argc > 3 ? argv[3] : "dist/TSMemory-TVTestSrc.aux2";
	//	切り出し方の指定 "head:10" / "tail:10" / "mid:10" (単位 MB)
	//
	//	  head : 先頭は正しいストリームの開始、末尾だけが GOP の途中で切れる
	//	  tail : 先頭が GOP の途中、末尾はファイルの終端
	//	  mid  : 両端とも GOP の途中 (TSMemory のリングバッファと同じ状況)
	//
	//	head と mid を比べる事で、欠けているのが先頭側か末尾側かが判る。
	const char *pszSlice = argc > 4 ? argv[4] : nullptr;

	//	--- TS を読む --------------------------------------------------------
	std::vector<BYTE> Ts;
	{
		FILE *fp = std::fopen(pszTsPath, "rb");
		if (fp == nullptr) {
			std::printf("cannot open %s\n", pszTsPath);
			return 1;
		}
		std::fseek(fp, 0, SEEK_END);
		const long Size = std::ftell(fp);
		std::fseek(fp, 0, SEEK_SET);
		Ts.resize(static_cast<size_t>(Size));
		const size_t Read = std::fread(Ts.data(), 1, Ts.size(), fp);
		std::fclose(fp);
		Ts.resize(Read);
	}
	std::printf("TS file : %s (%zu bytes, %zu packets)\n",
				pszTsPath, Ts.size(), Ts.size() / 188);

	const size_t FullPackets = Ts.size() / 188;
	bool fSliced = false;
	if (pszSlice != nullptr) {
		const char *pszColon = std::strchr(pszSlice, ':');
		const int SizeMB = pszColon != nullptr ? std::atoi(pszColon + 1) : 0;
		const size_t Keep = (static_cast<size_t>(SizeMB) * 1024 * 1024 / 188) * 188;

		if (SizeMB > 0 && Keep < Ts.size()) {
			size_t Begin = 0;
			if (std::strncmp(pszSlice, "tail", 4) == 0) {
				Begin = ((Ts.size() - Keep) / 188) * 188;
			} else if (std::strncmp(pszSlice, "mid", 3) == 0) {
				Begin = (((Ts.size() - Keep) / 2) / 188) * 188;
			}
			Ts = std::vector<BYTE>(Ts.begin() + Begin, Ts.begin() + Begin + Keep);
			fSliced = true;
			std::printf("slice   : %s -> %zu bytes (%zu packets) from offset %zu\n",
						pszSlice, Ts.size(), Ts.size() / 188, Begin);
		}
	}

	//	切り出した範囲に何秒分の映像が入っているか (PTS から)
	double PtsSpan = 0.0;
	{
		int64_t First = PTS_NONE, Last = PTS_NONE;
		if (ScanVideoPts(Ts.data(), Ts.size(), &First, &Last)) {
			PtsSpan = PtsDiffSeconds(First, Last);
			std::printf("pts span: %.2f sec of video is present in the given data\n", PtsSpan);
		}
	}
	std::printf("\n");
	check("TS file has a valid sync byte", !Ts.empty() && Ts[0] == 0x47);

	//	--- 共有メモリに載せる ----------------------------------------------
	SharedTs Shm;
	check("published the TS to shared memory", PublishTs(&Shm, "tsdecode.tvtv", Ts));

	//	ダミーファイル (AviUtl2 に渡す物と同じ扱い。中身は読まれない)
	WCHAR szTvtv[MAX_PATH];
	::GetCurrentDirectoryW(MAX_PATH, szTvtv);
	::lstrcatW(szTvtv, L"\\build\\tests\\tsdecode.tvtv");
	{
		HANDLE hFile = ::CreateFileW(szTvtv, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
									 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile != INVALID_HANDLE_VALUE)
			::CloseHandle(hFile);
	}

	g_Host.register_input_plugin = fake_register_input_plugin;
	g_Host.register_window_client = fake_register_window_client;
	g_Host.register_project_load_handler = fake_register_project_load_handler;
	g_Host.register_export_menu_param = fake_register_export_menu_param;
	g_Host.create_edit_handle = fake_create_edit_handle;

	HMODULE hModule = ::LoadLibraryA(pszDll);
	check("LoadLibrary(TSMemory-TVTestSrc.aux2)", hModule != nullptr);
	if (hModule == nullptr)
		return 1;

	//	音声を開けなかった理由がログに出る事を見る為、ロガーを渡しておく
	{
		auto pInitializeLogger = reinterpret_cast<void (*)(LOG_HANDLE *)>(
			::GetProcAddress(hModule, "InitializeLogger"));
		if (pInitializeLogger != nullptr) {
			g_Log.log = log_message;
			g_Log.warn = log_message;
			g_Log.error = log_message;
			pInitializeLogger(&g_Log);
		}
	}

	auto pInitializePlugin = reinterpret_cast<bool (*)(DWORD)>(::GetProcAddress(hModule, "InitializePlugin"));
	auto pUninitializePlugin = reinterpret_cast<void (*)()>(::GetProcAddress(hModule, "UninitializePlugin"));
	auto pRegisterPlugin = reinterpret_cast<void (*)(HOST_APP_TABLE *)>(::GetProcAddress(hModule, "RegisterPlugin"));
	pInitializePlugin(0);
	pRegisterPlugin(&g_Host);
	check("input plugin was registered", g_pInputPluginTable != nullptr);
	if (g_pInputPluginTable == nullptr)
		return 1;

	INPUT_PLUGIN_TABLE *ip = g_pInputPluginTable;

	//	--- オープン ---------------------------------------------------------
	const DWORD OpenStart = ::GetTickCount();
	INPUT_HANDLE ih = ip->func_open(szTvtv);
	const DWORD OpenMs = ::GetTickCount() - OpenStart;
	check("func_open() succeeded", ih != nullptr);
	if (ih == nullptr) {
		std::printf("\nFAIL (%d failures)\n", g_failures + 1);
		return 1;
	}
	std::printf("  open took %lu ms\n", OpenMs);

	//	--- 情報取得 ---------------------------------------------------------
	INPUT_INFO Info = {};
	check("func_info_get() succeeded", ip->func_info_get(ih, &Info));

	const bool fHasVideo = (Info.flag & INPUT_INFO::FLAG_VIDEO) != 0;
	const bool fHasAudio = (Info.flag & INPUT_INFO::FLAG_AUDIO) != 0;

	check("video stream was found", fHasVideo);
	if (fHasVideo) {
		std::printf("  video   : %ldx%ld  %d/%d fps  %d frames  compression=%.4s\n",
					Info.format->biWidth, Info.format->biHeight,
					Info.rate, Info.scale, Info.n,
					reinterpret_cast<const char *>(&Info.format->biCompression));
		check("video size is sane",
			  Info.format->biWidth > 0 && Info.format->biWidth <= 1920
			  && Info.format->biHeight > 0 && Info.format->biHeight <= 1088);
		check("pixel format is YUY2",
			  Info.format->biCompression == MAKEFOURCC('Y', 'U', 'Y', '2'));
		check("frame rate is sane", Info.rate > 0 && Info.scale > 0);
		check("frame count is positive", Info.n > 0);

		//	与えたデータに含まれる映像の長さ (PTS) と、実際にデコード出来た
		//	長さを比べる。差が GOP の切れ端で失われた分になる。
		if (Info.rate > 0 && Info.scale > 0) {
			const double Seconds = static_cast<double>(Info.n) * Info.scale / Info.rate;
			std::printf("  duration: %.2f sec decoded", Seconds);
			if (PtsSpan > 0.0) {
				std::printf(" / %.2f sec present (%.2f sec lost at the GOP edges)",
							PtsSpan, PtsSpan - Seconds);
			}
			std::printf("\n");
			(void)FullPackets;

			if (fSliced && PtsSpan > 0.0) {
				check("the loss at the GOP edges is under 1.5 sec",
					  PtsSpan - Seconds < 1.5);
			}
		}
	}

	if (fHasAudio) {
		std::printf("  audio   : %d ch  %lu Hz  %d bit  %d samples\n",
					Info.audio_format->nChannels, Info.audio_format->nSamplesPerSec,
					Info.audio_format->wBitsPerSample, Info.audio_n);
		check("audio format is PCM 16bit",
			  Info.audio_format->wFormatTag == WAVE_FORMAT_PCM
			  && Info.audio_format->wBitsPerSample == 16);
		//	申告だけして中身が無い状態をここで捕まえる
		//	(実際の読み出しは後段の「音声の読み出し」で見る)
		check("audio has samples", Info.audio_n > 0);
		check("the log says the audio was taken", LogContains(L"音声を取り込みました"));
	} else {
		std::printf("  audio   : (none)\n");

		//	[M2V] audio=1 なのに音声が無い場合は、理由がログに出る事。
		//	いちばん多いのは TVTest 側の [Settings] Audio が 0 のまま
		if (!g_LogLines.empty() && LogContains(L"音声")) {
			check("the reason the audio is missing is logged",
				  LogContains(L"音声が見つかりません")
				  || LogContains(L"音声を取り込めませんでした"));
		}
	}
	std::printf("\n");

	//	--- 映像の読み出し ---------------------------------------------------
	if (fHasVideo) {
		const int Width = Info.format->biWidth;
		const int Height = Info.format->biHeight;
		const int Pitch = ((16 * Width + 31) & ~31) >> 3;
		std::vector<BYTE> Frame(static_cast<size_t>(Pitch) * Height + 64);

		//	フレーム 0 を控えておいて、後で戻って読み直した時に一致するか見る
		std::vector<BYTE> First;
		bool fAnyPicture = false;

		const int Frames[] = { 0, 30, Info.n / 2, Info.n - 1, 0 };
		const int FrameCount = static_cast<int>(sizeof(Frames) / sizeof(Frames[0]));
		for (int k = 0; k < FrameCount; k++) {
			const int FrameNo = Frames[k];
			if (FrameNo >= Info.n)
				continue;

			const DWORD Start = ::GetTickCount();
			const int Bytes = ip->func_read_video(ih, FrameNo, Frame.data());
			const DWORD Ms = ::GetTickCount() - Start;

			char what[64];
			std::snprintf(what, sizeof(what), "func_read_video(%d) returned data", FrameNo);
			check(what, Bytes == Pitch * Height);
			std::printf("  frame %d decoded in %lu ms (%d bytes)\n", FrameNo, Ms, Bytes);

			if (Bytes <= 0)
				continue;

			if (FrameNo == Frames[0])
				ReportYuy2Range(Frame.data(), Width, Height, Pitch);

			//	同じフレームを読み直したら同じ絵になる事 (シークの一貫性)
			if (FrameNo == 0) {
				if (First.empty()) {
					First.assign(Frame.begin(), Frame.begin() + Bytes);
				} else {
					check("re-reading frame 0 after seeking gives the same image",
						  std::memcmp(First.data(), Frame.data(), Bytes) == 0);
				}
			}

			for (int Orientation = 0; Orientation < 2; Orientation++) {
				std::vector<BYTE> Rgb;
				Yuy2ToRgb(Frame.data(), Width, Height, Pitch, Orientation != 0, &Rgb);

				char path[MAX_PATH];
				std::snprintf(path, sizeof(path), "%s%d_%s.raw",
							  pszPrefix, FrameNo, Orientation == 0 ? "asis" : "flipped");
				WriteRaw(path, Rgb);

				if (Orientation == 0) {
					double StdDev = 0.0;
					if (LooksDecoded(Rgb, &StdDev))
						fAnyPicture = true;
					std::printf("  frame %d pixel stddev = %.1f%s\n",
								FrameNo, StdDev, StdDev <= 5.0 ? "  (flat)" : "");
				}
			}
		}

		//	「平坦でない絵が出た」事だけを見る。
		//	どのフレームが平坦かは中身次第で、実際の放送を録った物は
		//	先頭と末尾が黒画面である事が珍しくない
		//	(erb.jp の ISDB-T サンプルがそう)。フレーム単位で要求すると
		//	デコーダではなく素材を見ている事になる
		check("at least one of the sampled frames has a picture", fAnyPicture);

		char path[MAX_PATH];
		std::snprintf(path, sizeof(path), "%s.txt", pszPrefix);
		FILE *fp = std::fopen(path, "w");
		if (fp != nullptr) {
			std::fprintf(fp, "%d %d\n", Width, Height);
			std::fclose(fp);
		}
	}

	//	--- 音声の読み出し ---------------------------------------------------
	if (fHasAudio) {
		const int Channels = Info.audio_format->nChannels;
		const int Length = 4096;
		std::vector<short> Audio(static_cast<size_t>(Length) * Channels);

		//	先頭・真ん中・終わり際を見る。
		//	**先頭が無音の素材は珍しくない** (黒画面から始まる録画等) ので、
		//	どこか 1 箇所で音が出ていれば良しとする。
		//	1 箇所に決め打ちすると、デコーダではなく素材を見る事になる
		const int64_t Positions[] = {
			0,
			Info.audio_n / 2,
			Info.audio_n > Length ? Info.audio_n - Length : 0,
		};

		bool fRead = false;
		bool fAnySound = false;
		for (int64_t Pos : Positions) {
			const int Samples = ip->func_read_audio(ih, Pos, Length, Audio.data());
			if (Samples <= 0)
				continue;
			fRead = true;

			long long Sum = 0;
			for (int i = 0; i < Samples * Channels; i++)
				Sum += std::abs(Audio[i]);
			const double Mean = static_cast<double>(Sum) / (Samples * Channels);
			std::printf("  audio at %lld : %d samples, mean |sample| = %.1f%s\n",
						static_cast<long long>(Pos), Samples, Mean,
						Mean > 1.0 ? "" : "  (silent)");
			if (Mean > 1.0)
				fAnySound = true;
		}

		check("func_read_audio() returned samples", fRead);
		check("the audio is not silent everywhere", fAnySound);
	}

	//	--- 後始末 -----------------------------------------------------------
	check("func_close() succeeded", ip->func_close(ih));

	//	--- ハンドルを開いたままアンロードしても落ちない事 -------------------
	//
	//	AviUtl2 はパッケージの入れ替え等でプラグインをアンロードするが、
	//	タイムラインにオブジェクトが残っていると入力ハンドルは開いたままになる。
	//	m2v はファイル毎にデコードスレッドを起動するので、閉じずにアンロード
	//	するとスレッドのコードがアンマップされて落ちる
	//	(TSMemory-TVTestSrc.aux2_unloaded として記録される)。
	//
	//	デコードスレッドは待機中の事が多く、アンロード直後に必ず落ちるとは
	//	限らないので「スレッドが残っていない事」を直接数えて確認する。
	{
		const int Before = CountThreads();

		INPUT_HANDLE ihLeft = ip->func_open(szTvtv);
		check("re-opened a handle for the unload test", ihLeft != nullptr);

		const int Opened = CountThreads();
		check("opening a .tvtv starts a decoder thread", Opened > Before);

		//	わざと func_close() せずに終了処理へ進む
		pUninitializePlugin();

		//	スレッドの終了が反映されるまで少し待つ
		int After = Opened;
		for (int i = 0; i < 50 && After > Before; i++) {
			::Sleep(100);
			After = CountThreads();
		}
		//	スレッド数は他の要因でも増減するので参考値として出すだけにする
		std::printf("  threads : %d -> %d (open) -> %d (after uninitialize)\n",
					Before, Opened, After);

		::FreeLibrary(hModule);
		hModule = nullptr;

		//	プラグインは自分自身をピン留めしている為、FreeLibrary しても
		//	メモリ上に残るのが正しい。
		//
		//	AviUtl2 は終了時にプラグインをアンロードした後も、登録済みの
		//	INPUT_PLUGIN_TABLE の関数ポインタや文字列を参照し続ける。
		//	アンロードされてしまうとアンマップ領域を実行・参照して落ちる
		//	(終了時のクラッシュとして実際に発生した)。
		//	SDK に登録解除の API が無い為、残す事が正しい振る舞いになる。
		check("the plugin stays loaded (pinned) after FreeLibrary",
			  ::GetModuleHandleW(L"TSMemory-TVTestSrc.aux2") != nullptr);

		//	ハンドルを開いたまま UninitializePlugin しても落ちない事
		//	(ここまで到達出来ている時点で満たしている)
		check("no crash when a handle is left open at uninitialize", true);

		//	後始末の後に呼ばれても新しく開かない事。
		//
		//	プラグインは固定されて残る為、AviUtl2 は
		//	UninitializePlugin() の後でも func_open() / func_close() を
		//	呼べる状態のままになる。ここで開いてしまうと管理リストに
		//	載らないままデコードスレッドだけが残る。
		INPUT_HANDLE ihAfter = ip->func_open(szTvtv);
		check("func_open() refuses to open after uninitialize", ihAfter == nullptr);
		if (ihAfter != nullptr)
			ip->func_close(ihAfter);

		//	既に破棄済みのハンドルを渡されても二重解放しない事
		check("func_close() is safe for an already released handle",
			  ip->func_close(ihLeft));

		::Sleep(500);
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
