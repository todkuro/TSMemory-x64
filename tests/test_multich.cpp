//----------------------------------------------------------------------------
//	マルチ編成の TS から、指定したサービスの映像が取り出せるかを確認する。
//
//	  test_multich <ts-file> [out-prefix] [aux2-path]
//
//	TSMemory.tvtp と同じ様に CTsSelector でサービスを絞ってから共有メモリに
//	載せ、入力プラグインでデコードする。サービス毎に別の絵が出てくれば
//	サブチャンネルのキャプチャが出来ている事になる。
//
//	※ サービスID 0 は CTsSelector では「全サービス」の意味になり、その状態で
//	   デコーダに渡すと PAT の最初の映像 (プライマリチャンネル) が拾われる。
//
//	SPDX-License-Identifier: GPL-2.0-or-later
//	BonTsEngine (GPL-2.0-or-later) を利用する為、このファイルは GPL の対象です。
//	詳細は LICENSE.md を参照してください。
//----------------------------------------------------------------------------
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <vector>

#include "plugin2.h"
#include "input2.h"

#include "BonTsEngine/TsSelector.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

//---------------------------------------------------------------------------
//	ホスト側のふり
//---------------------------------------------------------------------------
INPUT_PLUGIN_TABLE *g_pInputPluginTable = nullptr;
EDIT_HANDLE g_EditHandle = {};
HOST_APP_TABLE g_Host = {};

void fake_register_input_plugin(INPUT_PLUGIN_TABLE *table) { g_pInputPluginTable = table; }
void fake_register_window_client(LPCWSTR, HWND) {}
void fake_register_project_load_handler(void (*)(PROJECT_FILE *)) {}
void fake_register_export_menu_param(LPCWSTR, void *, void (*)(void *)) {}
EDIT_HANDLE *fake_create_edit_handle() { return &g_EditHandle; }

//---------------------------------------------------------------------------
//	CTsSelector の出力を集める
//---------------------------------------------------------------------------
class CCollector : public CMediaDecoder
{
public:
	std::vector<BYTE> Packets;
	std::map<WORD, int> PidCount;

	const bool InputMedia(CMediaData *pMediaData, const DWORD /*Index*/ = 0UL) override
	{
		const BYTE *p = pMediaData->GetData();
		if (p == nullptr || pMediaData->GetSize() < 188)
			return true;
		PidCount[((p[1] & 0x1F) << 8) | p[2]]++;
		Packets.insert(Packets.end(), p, p + 188);
		return true;
	}
};

void FilterService(const std::vector<BYTE> &Ts, WORD ServiceID, CCollector *pOut)
{
	CTsSelector Selector;
	Selector.SetOutputDecoder(pOut);
	Selector.SetTargetServiceID(ServiceID, CTsSelector::STREAM_MPEG2VIDEO);

	CTsPacket Packet;
	Packet.GetBuffer(188);

	BYTE ContCounter[0x1FFF];
	::FillMemory(ContCounter, sizeof(ContCounter), 0x10);

	for (size_t i = 0; i + 188 <= Ts.size(); i += 188) {
		Packet.SetData(Ts.data() + i, 188);
		Packet.ParsePacket(ContCounter);
		Selector.InputMedia(&Packet);
	}
}

//---------------------------------------------------------------------------
//	共有メモリに載せる (TSMemory.tvtp のスナップショットと同じ形)
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
	pShm->hMutex = ::CreateMutexA(&sa, FALSE, szMutexName);
	pShm->hMap = ::CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
									  static_cast<DWORD>(sizeof(DWORD) * 4) + DataSize, pszName);
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

//---------------------------------------------------------------------------
//	YUY2 -> RGB24
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

double MeanAbsDiff(const std::vector<BYTE> &a, const std::vector<BYTE> &b)
{
	if (a.size() != b.size() || a.empty())
		return -1.0;
	double s = 0.0;
	for (size_t i = 0; i < a.size(); i++)
		s += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
	return s / a.size();
}

void WriteRaw(const char *pszPath, const std::vector<BYTE> &Data)
{
	FILE *fp = std::fopen(pszPath, "wb");
	if (fp != nullptr) {
		std::fwrite(Data.data(), 1, Data.size(), fp);
		std::fclose(fp);
	}
}

//	PAT からサービス ID を拾う。
//	どの TS を渡されても動くように、番組の一覧は決め打ちにせず読み取る
//	(build/ts-examples/ に別の TS を置いた場合にもそのまま使える)。
std::vector<WORD> FindServices(const std::vector<BYTE> &Ts)
{
	std::vector<WORD> Services;

	for (size_t i = 0; i + 188 <= Ts.size(); i += 188) {
		const BYTE *p = &Ts[i];
		if (p[0] != 0x47)
			continue;
		const WORD Pid = static_cast<WORD>(((p[1] & 0x1F) << 8) | p[2]);
		if (Pid != 0x0000 || (p[1] & 0x40) == 0)
			continue;					// PAT の先頭パケットだけ見る
		if ((p[3] & 0x10) == 0)
			continue;					// ペイロード無し

		size_t Offset = 4;
		if (p[3] & 0x20)
			Offset += 1 + p[4];			// アダプテーションフィールド
		if (Offset >= 188)
			continue;
		Offset += 1 + p[Offset];		// pointer_field
		if (Offset + 8 > 188 || p[Offset] != 0x00)
			continue;					// table_id : PAT

		const size_t Length = ((p[Offset + 1] & 0x0F) << 8) | p[Offset + 2];
		const size_t End = Offset + 3 + Length - 4;		// CRC の手前
		if (End > 188)
			continue;					// 分割されている PAT は諦める

		for (size_t q = Offset + 8; q + 4 <= End; q += 4) {
			const WORD Program = static_cast<WORD>((p[q] << 8) | p[q + 1]);
			if (Program == 0)
				continue;				// NIT
			bool fKnown = false;
			for (WORD s : Services)
				fKnown = fKnown || (s == Program);
			if (!fKnown)
				Services.push_back(Program);
		}
		if (!Services.empty())
			break;
	}

	return Services;
}

}	// namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const char *pszTsPath = argc > 1 ? argv[1] : "build/ts-examples/multi.ts";
	const char *pszPrefix = argc > 2 ? argv[2] : "build/tests/ch";
	const char *pszDll = argc > 3 ? argv[3] : "dist/TSMemory-TVTestSrc.aux2";

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
		Ts.resize(std::fread(Ts.data(), 1, Ts.size(), fp));
		std::fclose(fp);
	}
	std::printf("TS file : %s (%zu packets)\n\n", pszTsPath, Ts.size() / 188);

	//	--- プラグインを読み込む --------------------------------------------
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
	auto pUninitializePlugin = reinterpret_cast<void (*)()>(::GetProcAddress(hModule, "UninitializePlugin"));
	auto pRegisterPlugin = reinterpret_cast<void (*)(HOST_APP_TABLE *)>(::GetProcAddress(hModule, "RegisterPlugin"));
	pInitializePlugin(0);
	pRegisterPlugin(&g_Host);
	if (g_pInputPluginTable == nullptr) {
		check("input plugin was registered", false);
		return 1;
	}
	INPUT_PLUGIN_TABLE *ip = g_pInputPluginTable;

	//	--- サービス毎に絞ってデコードする -----------------------------------
	//	サービス ID は PAT から読む。先頭がプライマリ、2 つ目がサブチャンネル
	struct Target { WORD ServiceID; const char *Name; };
	std::vector<Target> Targets;
	{
		const std::vector<WORD> Services = FindServices(Ts);
		std::printf("PAT services :");
		for (WORD s : Services)
			std::printf(" %u", s);
		std::printf("\n\n");

		if (Services.size() < 2) {
			std::printf("this TS has %zu service(s) - nothing to compare, skipped\n",
						Services.size());
			return 0;
		}
		Targets.push_back({ Services[0], "primary" });
		Targets.push_back({ Services[1], "sub channel" });
	}

	struct Decoded { std::vector<BYTE> Rgb; int Width, Height; };
	std::vector<Decoded> Frames;

	for (const Target &t : Targets) {
		std::printf("--- service %u : %s\n", t.ServiceID, t.Name);

		CCollector Collector;
		FilterService(Ts, t.ServiceID, &Collector);

		char what[96];
		std::snprintf(what, sizeof(what), "service %u : packets were selected", t.ServiceID);
		check(what, Collector.Packets.size() >= 188 * 100);

		std::printf("    selected %zu packets, video PIDs:", Collector.Packets.size() / 188);
		for (const auto &e : Collector.PidCount) {
			if (e.first >= 0x0100 && e.first != 0x0000)
				std::printf(" 0x%04X(%d)", e.first, e.second);
		}
		std::printf("\n");

		char szName[64];
		std::snprintf(szName, sizeof(szName), "tsmulti%u.tvtv", t.ServiceID);

		SharedTs Shm;
		if (!PublishTs(&Shm, szName, Collector.Packets)) {
			check("published to shared memory", false);
			continue;
		}

		WCHAR szTvtv[MAX_PATH];
		::GetCurrentDirectoryW(MAX_PATH, szTvtv);
		::lstrcatW(szTvtv, L"\\build\\tests\\");
		WCHAR szWide[64];
		::MultiByteToWideChar(CP_ACP, 0, szName, -1, szWide, 64);
		::lstrcatW(szTvtv, szWide);
		{
			HANDLE hFile = ::CreateFileW(szTvtv, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
										 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hFile != INVALID_HANDLE_VALUE)
				::CloseHandle(hFile);
		}

		INPUT_HANDLE ih = ip->func_open(szTvtv);
		std::snprintf(what, sizeof(what), "service %u : func_open() succeeded", t.ServiceID);
		check(what, ih != nullptr);
		if (ih == nullptr)
			continue;

		INPUT_INFO Info = {};
		ip->func_info_get(ih, &Info);
		const int Width = Info.format != nullptr ? Info.format->biWidth : 0;
		const int Height = Info.format != nullptr ? Info.format->biHeight : 0;
		std::printf("    decoded %dx%d  %d frames\n", Width, Height, Info.n);

		std::snprintf(what, sizeof(what), "service %u : video was decoded", t.ServiceID);
		check(what, Width > 0 && Height > 0 && Info.n > 0);

		if (Width > 0 && Height > 0) {
			const int Pitch = ((16 * Width + 31) & ~31) >> 3;
			std::vector<BYTE> Frame(static_cast<size_t>(Pitch) * Height + 64);
			if (ip->func_read_video(ih, 0, Frame.data()) > 0) {
				std::vector<BYTE> Rgb;
				Yuy2ToRgb(Frame.data(), Width, Height, Pitch, &Rgb);

				char path[MAX_PATH];
				std::snprintf(path, sizeof(path), "%s%u.raw", pszPrefix, t.ServiceID);
				WriteRaw(path, Rgb);
				std::printf("    wrote %s (%dx%d)\n", path, Width, Height);
				Frames.push_back({ std::move(Rgb), Width, Height });
			}
		}

		ip->func_close(ih);
		std::printf("\n");
	}

	//	--- サブチャンネルが別の絵になっている事 ------------------------------
	if (Frames.size() == 2) {
		if (Frames[0].Width != Frames[1].Width || Frames[0].Height != Frames[1].Height) {
			//	マルチ編成ではサブチャンネルの解像度が下がる事が多い。
			//	大きさが違う時点で別のサービスをデコードしている。
			std::printf("picture size differs : %dx%d vs %dx%d\n",
						Frames[0].Width, Frames[0].Height,
						Frames[1].Width, Frames[1].Height);
			check("the sub channel really gives a different picture", true);
		} else {
			const double Diff = MeanAbsDiff(Frames[0].Rgb, Frames[1].Rgb);
			std::printf("mean abs difference between the two services : %.1f\n", Diff);
			check("the sub channel really gives a different picture", Diff > 10.0);
		}
	} else {
		check("both services were decoded", false);
	}

	pUninitializePlugin();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
