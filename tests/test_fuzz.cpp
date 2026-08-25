//----------------------------------------------------------------------------
//	壊れた TS を食わせても落ちない事を確認する。
//
//	  test_fuzz <ts-file> <aux2-path> [iterations] [seed]
//
//	m2v (MPEG-2 VIDEO VFAPI Plug-In) は 2003 年頃の 32bit 前提の C コードで、
//	放送波という信頼できない入力を直接パースする。受信エラーで壊れた TS は
//	日常的に発生するが、パーサの境界は一度も監査していない。
//
//	既存の test_decode は「GOP の途中で切れた」入力を通しているが、
//	これは**切れた**入力であって**壊れた**入力ではない。
//	ここではビット反転・パケット欠落・同期バイト破壊を混ぜた物を通し、
//	「例外で落ちない事」と「戻り値が矛盾しない事」だけを見る。
//
//	※ 画が正しいかは見ない。壊した入力に正解は無い。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "plugin2.h"
#include "input2.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

INPUT_PLUGIN_TABLE *g_pInputPluginTable = nullptr;
EDIT_HANDLE g_EditHandle = {};
HOST_APP_TABLE g_Host = {};

void fake_register_input_plugin(INPUT_PLUGIN_TABLE *table) { g_pInputPluginTable = table; }
void fake_register_window_client(LPCWSTR, HWND) {}
void fake_register_project_load_handler(void (*)(PROJECT_FILE *)) {}
void fake_register_export_menu_param(LPCWSTR, void *, void (*)(void *)) {}
EDIT_HANDLE *fake_create_edit_handle() { return &g_EditHandle; }

struct SharedTs {
	HANDLE hMutex = nullptr;
	HANDLE hMap = nullptr;
	void *pView = nullptr;

	void Close()
	{
		if (pView != nullptr) { ::UnmapViewOfFile(pView); pView = nullptr; }
		if (hMap != nullptr) { ::CloseHandle(hMap); hMap = nullptr; }
		if (hMutex != nullptr) { ::CloseHandle(hMutex); hMutex = nullptr; }
	}

	~SharedTs() { Close(); }
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
	pInfo[0] = DataSize;
	pInfo[1] = DataSize;
	pInfo[2] = 0;
	pInfo[3] = 0;
	::CopyMemory(static_cast<BYTE *>(pShm->pView) + sizeof(DWORD) * 4, Data.data(), DataSize);
	return true;
}

//	再現出来るよう自前の乱数を使う (処理系の rand() に依存しない)
struct Rng {
	unsigned int s;
	explicit Rng(unsigned int seed) : s(seed != 0 ? seed : 1) {}
	unsigned int Next()
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	unsigned int Below(unsigned int n) { return n != 0 ? Next() % n : 0; }
};

//	1 回あたりの制限時間
const DWORD TIMEOUT_MS = 20000;

//	壊し方を 3 種類混ぜる
const char *const MODE_NAME[] = { "bit flips", "dropped packets", "broken sync bytes" };

void Corrupt(std::vector<BYTE> *pTs, Rng *pRng, int Mode)
{
	const size_t Packets = pTs->size() / 188;
	if (Packets == 0)
		return;

	switch (Mode) {
	case 0:
		//	ビット反転。パケットあたり平均 1 ビット程度
		for (unsigned int i = 0; i < Packets; i++) {
			const size_t Offset = pRng->Below(static_cast<unsigned int>(pTs->size()));
			(*pTs)[Offset] ^= static_cast<BYTE>(1u << pRng->Below(8));
		}
		break;

	case 1:
		//	パケットを丸ごと落とす (連続性が壊れる)
		{
			std::vector<BYTE> Out;
			Out.reserve(pTs->size());
			for (size_t i = 0; i < Packets; i++) {
				if (pRng->Below(100) < 5)
					continue;
				Out.insert(Out.end(), pTs->begin() + static_cast<ptrdiff_t>(i * 188),
						   pTs->begin() + static_cast<ptrdiff_t>((i + 1) * 188));
			}
			pTs->swap(Out);
		}
		break;

	default:
		//	同期バイトを潰す (パケット境界を見失わせる)
		for (size_t i = 0; i < Packets; i++) {
			if (pRng->Below(100) < 10)
				(*pTs)[i * 188] = static_cast<BYTE>(pRng->Below(256));
		}
		break;
	}
}

//	1 回分。落ちたらプロセスごと死ぬので、そこは呼び出し側 (シェル) が見る
bool RunOnce(INPUT_PLUGIN_TABLE *ip, LPCWSTR pszPath, int *pDecoded)
{
	*pDecoded = 0;

	INPUT_HANDLE ih = ip->func_open(pszPath);
	if (ih == nullptr)
		return true;		// 開けないのは正しい振る舞い

	INPUT_INFO info = {};
	if (!ip->func_info_get(ih, &info) || info.format == nullptr) {
		ip->func_close(ih);
		return true;
	}

	const int Width = info.format->biWidth;
	const int Height = info.format->biHeight;

	//	デコーダが出す寸法が異常なら、そこで打ち切る
	if (Width <= 0 || Height <= 0 || Width > 16384 || Height > 16384) {
		ip->func_close(ih);
		return false;
	}

	const int Pitch = ((16 * Width + 31) & ~31) >> 3;
	std::vector<BYTE> Buffer(static_cast<size_t>(Pitch) * Height + 4096, 0xCD);

	//	番兵。書き過ぎたら判る
	const size_t Guard = static_cast<size_t>(Pitch) * Height;

	bool fOK = true;
	for (int i = 0; i < 8 && i < info.n; i++) {
		const int Read = ip->func_read_video(ih, i, Buffer.data());
		if (Read < 0 || static_cast<size_t>(Read) > Guard) {
			fOK = false;
			break;
		}
		if (Read > 0)
			(*pDecoded)++;
	}

	for (size_t i = Guard; i < Buffer.size(); i++) {
		if (Buffer[i] != 0xCD) {
			fOK = false;
			break;
		}
	}

	//	音声も通す ([M2V] audio=1 の時だけ中身がある)。
	//	壊れた TS で ADTS の走査や Media Foundation が落ちない事を見る
	if ((info.flag & INPUT_INFO::FLAG_AUDIO) != 0 && info.audio_format != nullptr) {
		const int Channels = info.audio_format->nChannels;
		if (Channels > 0 && Channels <= 8) {
			std::vector<int16_t> Audio(1024 * Channels + 64, 0x5A5A);
			const size_t AudioGuard = static_cast<size_t>(1024) * Channels;

			//	先頭・真ん中・範囲外
			const int Positions[] = { 0, info.audio_n / 2, info.audio_n + 10000 };
			for (int k = 0; k < 3; k++) {
				const int Got = ip->func_read_audio(ih, Positions[k], 1024, Audio.data());
				if (Got < 0 || Got > 1024) {
					fOK = false;
					break;
				}
			}
			for (size_t i = AudioGuard; i < Audio.size(); i++) {
				if (Audio[i] != 0x5A5A) {
					fOK = false;
					break;
				}
			}
		}
	}

	ip->func_close(ih);
	return fOK;
}

//	別スレッドで走らせて、返らない時は諦める。
//
//	壊れた MPEG-2 で m2v の GOP リスト作成が返ってこなくなる事がある。
//	同じスレッドで呼ぶとテスト自体が固まるので、時間を区切って見る。
//	止まったスレッドは放置する (プロセスの終了で回収される)。
struct RunArgs {
	INPUT_PLUGIN_TABLE *ip;
	WCHAR szPath[MAX_PATH];
	int Decoded;
	bool fOK;
};

DWORD WINAPI RunThread(LPVOID pParameter)
{
	RunArgs *p = static_cast<RunArgs *>(pParameter);
	p->fOK = RunOnce(p->ip, p->szPath, &p->Decoded);
	return 0;
}

}	// namespace


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 3) {
		std::printf("usage: test_fuzz <ts-file> <aux2-path> [iterations] [seed]\n");
		return 1;
	}

	const int Iterations = argc > 3 ? std::atoi(argv[3]) : 12;
	const unsigned int Seed = argc > 4 ? static_cast<unsigned int>(std::atoi(argv[4])) : 20260824u;

	//	元の TS を読む (先頭 2MB あれば足りる)
	std::vector<BYTE> Original;
	{
		FILE *fp = std::fopen(argv[1], "rb");
		if (fp == nullptr) {
			std::printf("cannot open %s\n", argv[1]);
			return 1;
		}
		Original.resize(2 * 1024 * 1024 / 188 * 188);
		Original.resize(std::fread(Original.data(), 1, Original.size(), fp) / 188 * 188);
		std::fclose(fp);
	}
	check("read the TS sample", !Original.empty());
	if (Original.empty())
		return 1;

	HMODULE hModule = ::LoadLibraryA(argv[2]);
	check("loaded the plugin", hModule != nullptr);
	if (hModule == nullptr)
		return 1;

	auto pInitialize = reinterpret_cast<bool (*)(DWORD)>(
		::GetProcAddress(hModule, "InitializePlugin"));
	auto pRegister = reinterpret_cast<void (*)(HOST_APP_TABLE *)>(
		::GetProcAddress(hModule, "RegisterPlugin"));
	auto pUninitialize = reinterpret_cast<void (*)()>(
		::GetProcAddress(hModule, "UninitializePlugin"));
	check("found the plugin entry points",
		  pInitialize != nullptr && pRegister != nullptr && pUninitialize != nullptr);
	if (pInitialize == nullptr || pRegister == nullptr)
		return 1;

	g_Host.register_input_plugin = fake_register_input_plugin;
	g_Host.register_window_client = fake_register_window_client;
	g_Host.register_project_load_handler = fake_register_project_load_handler;
	g_Host.register_export_menu_param = fake_register_export_menu_param;
	g_Host.create_edit_handle = fake_create_edit_handle;

	pInitialize(0);
	pRegister(&g_Host);
	check("the input plugin was registered", g_pInputPluginTable != nullptr);
	if (g_pInputPluginTable == nullptr)
		return 1;

	WCHAR szDir[MAX_PATH];
	::GetCurrentDirectoryW(MAX_PATH, szDir);

	Rng Rng(Seed);
	int Survived = 0, Decoded = 0, Stuck = 0;
	bool fSane = true;

	std::printf("seed %u / %d iterations / %llu packets each\n\n",
				Seed, Iterations, static_cast<unsigned long long>(Original.size() / 188));

	for (int i = 0; i < Iterations; i++) {
		const int Mode = i % 3;

		std::vector<BYTE> Ts = Original;
		Corrupt(&Ts, &Rng, Mode);
		if (Ts.empty())
			continue;

		char szName[64];
		std::snprintf(szName, sizeof(szName), "tsfuzz%d.tvtv", i);

		SharedTs Shm;
		if (!PublishTs(&Shm, szName, Ts))
			continue;

		WCHAR szPath[MAX_PATH];
		::wnsprintfW(szPath, MAX_PATH, L"%s\\%hs", szDir, szName);

		RunArgs Args = {};
		Args.ip = g_pInputPluginTable;
		::lstrcpynW(Args.szPath, szPath, MAX_PATH);
		Args.fOK = true;

		HANDLE hThread = ::CreateThread(nullptr, 0, RunThread, &Args, 0, nullptr);
		if (hThread == nullptr)
			continue;

		const bool fFinished =
			::WaitForSingleObject(hThread, TIMEOUT_MS) == WAIT_OBJECT_0;
		::CloseHandle(hThread);

		if (!fFinished) {
			//	止まったスレッドが共有メモリを掴んだままなので閉じない
			Shm.pView = nullptr;
			Shm.hMap = nullptr;
			Shm.hMutex = nullptr;
			Stuck++;
			std::printf("  %2d %-18s -> did not return within %lu ms\n",
						i, MODE_NAME[Mode], TIMEOUT_MS);
			continue;
		}

		Survived++;
		Decoded += Args.Decoded;
		if (!Args.fOK)
			fSane = false;

		std::printf("  %2d %-18s -> %d frames%s\n", i, MODE_NAME[Mode], Args.Decoded,
					Args.fOK ? "" : "   <- INCONSISTENT");

		Shm.Close();
	}

	//	ここまで来ている = 例外で死んでいない
	check("no crash while decoding corrupted streams", Survived > 0);
	check("all runs stayed within the reported buffer", fSane);

	//	壊した程度では大抵まだデコード出来る。1 枚も出ないのは
	//	「壊し過ぎ」か「開けなくなっている」ので、テストとして意味が無くなる
	check("corrupted streams still decode something", Decoded > 0);

	//	m2v は壊れた MPEG-2 で開く処理が現実的な時間で終わらなくなる事が
	//	ある。入力プラグイン側が時間を区切って「開けなかった」事にするので、
	//	ここまで返って来ないのは、その仕組みが効いていない事を意味する。
	//	詳細は docs/development.md の「壊れた TS への耐性」を参照。
	if (Stuck > 0) {
		std::printf("  %d of %d runs did not return\n", Stuck, Stuck + Survived);
	}
	check("every run returned (the open timeout is working)", Stuck == 0);

	if (pUninitialize != nullptr)
		pUninitialize();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
