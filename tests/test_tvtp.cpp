//----------------------------------------------------------------------------
//	TVTest プラグイン (TSMemory.tvtp) を TVTest 無しで動かす。
//
//	  test_tvtp <TSMemory.tvtp のパス> <作業フォルダ>
//
//	TVTest 本体の代わりになる最小のホストを用意し、
//	TVTInitialize() → EVENT_PLUGINENABLE → ストリーム投入 →
//	EVENT_COMMAND → TVTFinalize() までを実際に呼ぶ。
//
//	AviUtl2 側 (aux2) には test_plugin.cpp があるが、TVTest 側は
//	これまで一度も実行されていなかった。設定値の扱いと終了処理の
//	不具合がここから出た為に追加した物。
//
//	※ aviutl2.exe は起動しない。
//	   AviUtlPath に「今動いているプロセスの名前」を指定すると
//	   IsAviUtlRunning() が真になり、起動せずに待ち受け待ちへ入る。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "TVTestPlugin.h"

#include "tsmemory_ipc.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

//----------------------------------------------------------------------------
//	TVTest の代わり
//----------------------------------------------------------------------------
struct FakeHost {
	TVTest::PluginParam Param;

	TVTest::EventCallbackFunc EventCallback;
	void *pEventData;
	TVTest::StreamCallbackFunc StreamCallback;
	void *pStreamData;

	bool fCommandRegistered;
	WORD ServiceID;
	std::vector<std::wstring> Log;
};

FakeHost g_Host;

LRESULT CALLBACK HostCallback(TVTest::PluginParam *pParam, UINT Message,
							  LPARAM lParam1, LPARAM lParam2)
{
	(void)pParam;

	switch (Message) {
	case TVTest::MESSAGE_GETVERSION:
		return TVTEST_PLUGIN_VERSION_(0, 0, 14);

	case TVTest::MESSAGE_QUERYMESSAGE:
		//	この模擬ホストが応えられる物だけを真にする
		switch (lParam1) {
		case TVTest::MESSAGE_SETEVENTCALLBACK:
		case TVTest::MESSAGE_SETSTREAMCALLBACK:
		case TVTest::MESSAGE_REGISTERCOMMAND:
		case TVTest::MESSAGE_ADDLOG:
		case TVTest::MESSAGE_GETSERVICE:
		case TVTest::MESSAGE_GETSERVICEINFO:
			return TRUE;
		}
		return FALSE;

	case TVTest::MESSAGE_SETEVENTCALLBACK:
		g_Host.EventCallback = reinterpret_cast<TVTest::EventCallbackFunc>(lParam1);
		g_Host.pEventData = reinterpret_cast<void *>(lParam2);
		return TRUE;

	case TVTest::MESSAGE_SETSTREAMCALLBACK:
		{
			const TVTest::StreamCallbackInfo *pInfo =
				reinterpret_cast<const TVTest::StreamCallbackInfo *>(lParam1);
			if (pInfo == nullptr)
				return FALSE;
			if ((pInfo->Flags & TVTest::STREAM_CALLBACK_REMOVE) != 0) {
				g_Host.StreamCallback = nullptr;
			} else {
				g_Host.StreamCallback = pInfo->Callback;
				g_Host.pStreamData = pInfo->pClientData;
			}
			return TRUE;
		}

	case TVTest::MESSAGE_REGISTERCOMMAND:
		g_Host.fCommandRegistered = true;
		return TRUE;

	case TVTest::MESSAGE_ADDLOG:
		if (lParam1 != 0)
			g_Host.Log.push_back(reinterpret_cast<LPCWSTR>(lParam1));
		return TRUE;

	case TVTest::MESSAGE_GETSERVICE:
		if (lParam1 != 0)
			*reinterpret_cast<int *>(lParam1) = 1;
		//	-1 = 視聴中のサービスが判らない。
		//	CTsSelector のサービスID は 0 (全サービス) のままになり、
		//	どの TS を流し込んでも素通りする。
		//	サービスを絞る経路は test_multich.cpp が見ている。
		return -1;

	case TVTest::MESSAGE_GETSERVICEINFO:
		{
			TVTest::ServiceInfo *pInfo = reinterpret_cast<TVTest::ServiceInfo *>(lParam2);
			if (pInfo == nullptr || lParam1 != 0)
				return FALSE;
			const DWORD Size = pInfo->Size;
			::ZeroMemory(pInfo, Size);
			pInfo->Size = Size;
			pInfo->ServiceID = g_Host.ServiceID;
			pInfo->VideoPID = 0x0100;
			return TRUE;
		}
	}

	return 0;
}

bool HostLogContains(LPCWSTR text)
{
	for (const std::wstring &s : g_Host.Log) {
		if (::StrStrIW(s.c_str(), text) != nullptr)
			return true;
	}
	return false;
}

//----------------------------------------------------------------------------
//	道具
//----------------------------------------------------------------------------
WCHAR g_szDir[MAX_PATH];
WCHAR g_szPlugin[MAX_PATH];

//	tvtp を作業フォルダに複製し、隣に ini を書く。
//	設定は起動時に読まれるので、条件ごとに別名で複製する。
bool PreparePlugin(LPCWSTR name, const char *ini)
{
	WCHAR szSrc[MAX_PATH];
	::lstrcpynW(szSrc, g_szPlugin, MAX_PATH);

	::lstrcpynW(g_szPlugin, g_szDir, MAX_PATH);
	::PathAppendW(g_szPlugin, name);
	if (!::CopyFileW(szSrc, g_szPlugin, FALSE))
		return false;

	WCHAR szIni[MAX_PATH];
	::lstrcpynW(szIni, g_szPlugin, MAX_PATH);
	::PathRenameExtensionW(szIni, L".ini");

	HANDLE hFile = ::CreateFileW(szIni, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
								 FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;
	DWORD Written = 0;
	::WriteFile(hFile, ini, static_cast<DWORD>(std::strlen(ini)), &Written, nullptr);
	::CloseHandle(hFile);
	return true;
}

//	共有メモリの名前は tsmemory<N>.tvtv で、N は空いている番号が使われる。
//	**本物の TVTest が動いていると 0 番はそちらの物**になる為、
//	名前を決め打ちで開くと他人のバッファを見てしまう。
//	Initialize() の前後でミューテックスの有無を比べて、自分の番号を割り出す。
int g_MyIndex = -1;

void SnapshotUsedIndices(bool *pUsed)
{
	for (int i = 0; i < 100; i++) {
		WCHAR szName[64];
		::wnsprintfW(szName, 64, L"tsmemory%d.tvtv.mutex", i);
		HANDLE h = ::OpenMutexW(SYNCHRONIZE, FALSE, szName);
		pUsed[i] = (h != nullptr);
		if (h != nullptr)
			::CloseHandle(h);
	}
}

//	before との差分から、今回のインスタンスが取った番号を求める
int FindMyIndex(const bool *pBefore)
{
	bool After[100];
	SnapshotUsedIndices(After);

	for (int i = 0; i < 100; i++) {
		if (After[i] && !pBefore[i])
			return i;
	}
	return -1;
}

HANDLE OpenBufferMap()
{
	if (g_MyIndex < 0)
		return nullptr;

	WCHAR szName[64];
	::wnsprintfW(szName, 64, L"tsmemory%d.tvtv", g_MyIndex);
	return ::OpenFileMappingW(FILE_MAP_READ, FALSE, szName);
}

//	TS サンプルを読む。
//	CTsSelector は PAT / PMT を見て通す物を決める為、自作の
//	ダミーパケットでは 1 つも溜まらない。実物を流し込む必要がある。
std::vector<BYTE> g_Ts;

bool LoadTs(LPCWSTR path, size_t max_bytes)
{
	HANDLE hFile = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
								 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	g_Ts.resize(max_bytes / 188 * 188);
	DWORD Read = 0;
	const BOOL fOK = ::ReadFile(hFile, g_Ts.data(), static_cast<DWORD>(g_Ts.size()),
								&Read, nullptr);
	::CloseHandle(hFile);
	if (!fOK)
		return false;

	g_Ts.resize(Read / 188 * 188);

	//	同期バイトの位置に合わせる
	size_t Offset = 0;
	while (Offset < 188 && Offset < g_Ts.size() && g_Ts[Offset] != 0x47)
		Offset++;
	if (Offset > 0 && Offset < g_Ts.size())
		g_Ts.erase(g_Ts.begin(), g_Ts.begin() + static_cast<ptrdiff_t>(Offset));

	g_Ts.resize(g_Ts.size() / 188 * 188);
	return !g_Ts.empty();
}

void FeedTs()
{
	for (size_t i = 0; i + 188 <= g_Ts.size(); i += 188)
		g_Host.StreamCallback(&g_Ts[i], g_Host.pStreamData);
}

//	壊した状態で流し込む。
//	  0 : transport_error_indicator を立てる (受信時のビット誤り)
//	  1 : 同期バイトを潰す
void FeedBrokenTs(int Mode)
{
	std::vector<BYTE> Packet(188);

	for (size_t i = 0; i + 188 <= g_Ts.size(); i += 188) {
		::CopyMemory(Packet.data(), &g_Ts[i], 188);
		if (Mode == 0)
			Packet[1] |= 0x80;			// TEI
		else
			Packet[0] = 0x00;			// 同期バイト
		g_Host.StreamCallback(Packet.data(), g_Host.pStreamData);
	}
}

struct PluginApi {
	HMODULE hModule;
	TVTest::GetVersionFunc GetVersion;
	TVTest::GetPluginInfoFunc GetPluginInfo;
	TVTest::InitializeFunc Initialize;
	TVTest::FinalizeFunc Finalize;
};

bool LoadPlugin(PluginApi *pApi)
{
	::ZeroMemory(pApi, sizeof(*pApi));

	pApi->hModule = ::LoadLibraryW(g_szPlugin);
	if (pApi->hModule == nullptr)
		return false;

	pApi->GetVersion = reinterpret_cast<TVTest::GetVersionFunc>(
		::GetProcAddress(pApi->hModule, "TVTGetVersion"));
	pApi->GetPluginInfo = reinterpret_cast<TVTest::GetPluginInfoFunc>(
		::GetProcAddress(pApi->hModule, "TVTGetPluginInfo"));
	pApi->Initialize = reinterpret_cast<TVTest::InitializeFunc>(
		::GetProcAddress(pApi->hModule, "TVTInitialize"));
	pApi->Finalize = reinterpret_cast<TVTest::FinalizeFunc>(
		::GetProcAddress(pApi->hModule, "TVTFinalize"));

	return pApi->GetVersion != nullptr && pApi->GetPluginInfo != nullptr
		&& pApi->Initialize != nullptr && pApi->Finalize != nullptr;
}

void ResetHost()
{
	g_Host.EventCallback = nullptr;
	g_Host.pEventData = nullptr;
	g_Host.StreamCallback = nullptr;
	g_Host.pStreamData = nullptr;
	g_Host.fCommandRegistered = false;
	g_Host.ServiceID = 0x0408;
	g_Host.Log.clear();

	g_Host.Param.Callback = HostCallback;
	g_Host.Param.hwndApp = nullptr;
	g_Host.Param.pClientData = nullptr;
	g_Host.Param.pInternalData = nullptr;
}

}	// namespace


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 3) {
		std::printf("usage: test_tvtp <TSMemory.tvtp> <work-dir> [ts-file]\n");
		return 1;
	}
	::MultiByteToWideChar(CP_ACP, 0, argv[1], -1, g_szPlugin, MAX_PATH);
	::MultiByteToWideChar(CP_ACP, 0, argv[2], -1, g_szDir, MAX_PATH);
	::CreateDirectoryW(g_szDir, nullptr);

	//	TS サンプルは任意。無ければ「実際に溜める」確認だけを飛ばす
	if (argc > 3) {
		WCHAR szTs[MAX_PATH];
		::MultiByteToWideChar(CP_ACP, 0, argv[3], -1, szTs, MAX_PATH);
		if (LoadTs(szTs, 4 * 1024 * 1024))
			std::printf("ts sample : %ls (%llu packets)\n\n",
						szTs, static_cast<unsigned long long>(g_Ts.size() / 188));
	}
	if (g_Ts.empty())
		std::printf("(no TS sample : buffering checks are skipped)\n\n");

	//	自分自身の実行ファイル名を AviUtlPath に使う。
	//	IsAviUtlRunning() が真になるので aviutl2.exe は起動されない。
	WCHAR szSelf[MAX_PATH];
	::GetModuleFileNameW(nullptr, szSelf, MAX_PATH);

	char szSelfA[MAX_PATH * 2];
	::WideCharToMultiByte(CP_UTF8, 0, szSelf, -1, szSelfA, sizeof(szSelfA), nullptr, nullptr);

	//	3. で使う (goto を跨がないようにここで宣言しておく)
	HANDLE hReady = nullptr;

	const WCHAR *const OriginalPlugin = g_szPlugin;
	(void)OriginalPlugin;

	//------------------------------------------------------------------
	//	1. 通常の設定で一通り動く事
	//------------------------------------------------------------------
	{
		char Ini[MAX_PATH * 3];
		::wnsprintfA(Ini, sizeof(Ini),
					 "[Settings]\r\n"
					 "MemorySize=10\r\n"
					 "AviUtlPath=%s\r\n"
					 "SnapshotCount=2\r\n"
					 "LaunchWait=600\r\n",
					 szSelfA);

		check("prepared the plugin and its ini", PreparePlugin(L"tvtp_basic.tvtp", Ini));

		PluginApi Api;
		check("the plugin exports the TVTest entry points", LoadPlugin(&Api));
		if (Api.hModule == nullptr || Api.Initialize == nullptr)
			goto done;

		TVTest::PluginInfo Info = {};
		check("TVTGetPluginInfo() succeeds", Api.GetPluginInfo(&Info) != FALSE);
		check("the plugin name is TSMemory",
			  Info.pszPluginName != nullptr && ::lstrcmpW(Info.pszPluginName, L"TSMemory") == 0);

		ResetHost();
		bool Before[100];
		SnapshotUsedIndices(Before);
		check("TVTInitialize() succeeds", Api.Initialize(&g_Host.Param) != FALSE);
		g_MyIndex = FindMyIndex(Before);
		std::printf("  this instance took tsmemory%d\n", g_MyIndex);
		check("the instance took a free slot", g_MyIndex >= 0);
		//	作業フォルダは Plugins ではなく書ける場所を選ぶ。
		//	書けない所を使っていると、原因不明のまま取り込みが失敗する
		check("the working directory is reported", HostLogContains(L"作業フォルダ"));
		check("the working directory is not the plugin folder",
			  !HostLogContains(L"Plugins"));

		check("a command was registered", g_Host.fCommandRegistered);
		check("an event callback was registered", g_Host.EventCallback != nullptr);
		check("a stream callback was registered", g_Host.StreamCallback != nullptr);

		//	--- 有効化すると共有メモリが出来る ---------------------------
		const LRESULT Enabled =
			g_Host.EventCallback(TVTest::EVENT_PLUGINENABLE, 1, 0, g_Host.pEventData);
		check("EVENT_PLUGINENABLE is accepted", Enabled != 0);

		HANDLE hMap = OpenBufferMap();
		check("the shared buffer was created", hMap != nullptr);

		if (hMap != nullptr) {
			BYTE *p = static_cast<BYTE *>(::MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
			check("the shared buffer can be mapped", p != nullptr);

			if (p != nullptr) {
				MEMORY_BASIC_INFORMATION mbi = {};
				::VirtualQuery(p, &mbi, sizeof(mbi));

				const DWORD *pInfoDw = reinterpret_cast<const DWORD *>(p);
				const DWORD Size = pInfoDw[0];

				std::printf("  buffer : Size=%lu bytes  mapped=%llu bytes\n",
							Size, static_cast<unsigned long long>(mbi.RegionSize));

				check("the buffer is about MemorySize (10MB)",
					  Size > 9 * 1024 * 1024 && Size <= 10 * 1024 * 1024);

				//	確保された領域が BufferInfo の言う長さを満たしている事。
				//	切り捨てが起きるとここが破れる (MemorySize の不具合)
				check("the mapping is large enough for the declared size",
					  mbi.RegionSize >= 16 + static_cast<SIZE_T>(Size));

				//	--- 壊れたパケットは捨てられる -------------------------
				//
				//	壊れたまま溜め込むと m2v のパーサに渡る事になる。
				//	m2v は壊れた MPEG-2 で GOP リストの作成が返って
				//	こなくなる事がある (tests/test_fuzz.cpp で再現)。
				if (!g_Ts.empty()) {
					FeedBrokenTs(0);
					check("packets with the transport error indicator are dropped",
						  pInfoDw[1] == 0);

					FeedBrokenTs(1);
					check("packets with a broken sync byte are dropped",
						  pInfoDw[1] == 0);
				}

				//	--- ストリームを流し込むと溜まる -----------------------
				if (!g_Ts.empty()) {
					FeedTs();
					const DWORD Used = pInfoDw[1];
					std::printf("  buffered %lu bytes of %llu fed\n",
								Used, static_cast<unsigned long long>(g_Ts.size()));
					check("feeding a real TS fills the buffer", Used > 0);
					check("Used never exceeds the buffer", Used <= Size);
				}

				::UnmapViewOfFile(p);
			}
			::CloseHandle(hMap);
		}

		//	--- 無効化で解放される ---------------------------------------
		g_Host.EventCallback(TVTest::EVENT_PLUGINENABLE, 0, 0, g_Host.pEventData);
		HANDLE hGone = OpenBufferMap();
		check("the shared buffer is released on disable", hGone == nullptr);
		if (hGone != nullptr)
			::CloseHandle(hGone);

		check("TVTFinalize() succeeds", Api.Finalize() != FALSE);
		::FreeLibrary(Api.hModule);
	}

	//------------------------------------------------------------------
	//	2. MemorySize が大きすぎる時に制限される事
	//
	//	共有メモリの大きさは DWORD で扱う為、制限が無いと
	//	CreateFileMapping() に切り捨てられた値が渡り、
	//	確保した範囲の外へ書き込む事になる。
	//------------------------------------------------------------------
	{
		char Ini[MAX_PATH * 3];
		::wnsprintfA(Ini, sizeof(Ini),
					 "[Settings]\r\n"
					 "MemorySize=8192\r\n"
					 "AviUtlPath=%s\r\n"
					 "LaunchWait=600\r\n",
					 szSelfA);

		check("prepared a plugin with MemorySize=8192", PreparePlugin(L"tvtp_huge.tvtp", Ini));

		PluginApi Api;
		if (!LoadPlugin(&Api)) {
			check("loaded the MemorySize=8192 plugin", false);
			goto done;
		}

		ResetHost();
		bool Before[100];
		SnapshotUsedIndices(Before);
		Api.Initialize(&g_Host.Param);
		g_MyIndex = FindMyIndex(Before);

		check("an oversized MemorySize is reported in the log",
			  HostLogContains(L"MemorySize"));

		//	実際に確保させて、宣言した長さを満たしているかを見る
		g_Host.EventCallback(TVTest::EVENT_PLUGINENABLE, 1, 0, g_Host.pEventData);

		//	制限後は約 4GB になる。ページファイルが足りない環境では
		//	確保自体が失敗し得るので、そこは失敗にせず飛ばす
		HANDLE hMap = OpenBufferMap();
		if (hMap == nullptr)
			std::printf("  (could not commit the clamped 4GB buffer : checks skipped)\n");

		if (hMap != nullptr) {
			BYTE *p = static_cast<BYTE *>(::MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
			if (p != nullptr) {
				MEMORY_BASIC_INFORMATION mbi = {};
				::VirtualQuery(p, &mbi, sizeof(mbi));
				const DWORD Size = *reinterpret_cast<const DWORD *>(p);

				std::printf("  buffer : Size=%lu bytes  mapped=%llu bytes\n",
							Size, static_cast<unsigned long long>(mbi.RegionSize));

				check("MemorySize is clamped below the 32bit limit",
					  static_cast<SIZE_T>(Size) + 16 <= 0xFFFFFFFFu);
				check("the mapping is large enough for the declared size",
					  mbi.RegionSize >= 16 + static_cast<SIZE_T>(Size));

				::UnmapViewOfFile(p);
			}
			::CloseHandle(hMap);
		}

		g_Host.EventCallback(TVTest::EVENT_PLUGINENABLE, 0, 0, g_Host.pEventData);
		Api.Finalize();
		::FreeLibrary(Api.hModule);
	}

	//------------------------------------------------------------------
	//	3. 起動待ちの最中に終了しても、待たされずに片付く事
	//
	//	EVENT_COMMAND は AviUtl2 の起動待ちでTVTest を止めない為に
	//	ワーカースレッドで処理する。この待ちは LaunchWait (ここでは 600 秒)。
	//	TVTFinalize() がワーカーを置き去りにすると、この後の
	//	FreeLibrary でアンロード済みのコードを実行する事になる。
	//
	//	TS が溜まっていないと ExecuteCapture() は待ちループまで進まず、
	//	素通りして「待たされない」が偽陽性になる。サンプルが要る。
	//
	//	同じ理由で、**AviUtl2 が起動して待ち受けている場合も駄目**。
	//	IsAviUtlReady() が真になって待ちループへ入らない為、
	//	何を測っても 0ms になり通ってしまう。その場合は飛ばす。
	//------------------------------------------------------------------
	hReady = ::OpenMutexW(SYNCHRONIZE, FALSE, TSMEMORY_IPC_READY_MUTEX);
	if (hReady != nullptr)
		::CloseHandle(hReady);

	if (g_Ts.empty()) {
		std::printf("\n(no TS sample : the shutdown-during-launch-wait check is skipped)\n");
	} else if (hReady != nullptr) {
		std::printf("\n(AviUtl2 is listening : the shutdown-during-launch-wait check is skipped)\n"
					"  close AviUtl2 to exercise it\n");
	} else {
		char Ini[MAX_PATH * 3];
		::wnsprintfA(Ini, sizeof(Ini),
					 "[Settings]\r\n"
					 "MemorySize=1\r\n"
					 "AviUtlPath=%s\r\n"
					 "LaunchWait=600\r\n",
					 szSelfA);

		check("prepared a plugin with LaunchWait=600", PreparePlugin(L"tvtp_exit.tvtp", Ini));

		PluginApi Api;
		if (!LoadPlugin(&Api)) {
			check("loaded the LaunchWait=600 plugin", false);
			goto done;
		}

		ResetHost();
		bool Before[100];
		SnapshotUsedIndices(Before);
		Api.Initialize(&g_Host.Param);
		g_MyIndex = FindMyIndex(Before);
		g_Host.EventCallback(TVTest::EVENT_PLUGINENABLE, 1, 0, g_Host.pEventData);

		//	スナップショットを作れるだけのパケットを入れておく。
		//	溜まっていないと ExecuteCapture() が待ちループまで進まない。
		HANDLE hMap = OpenBufferMap();
		BYTE *pShared = hMap != nullptr
			? static_cast<BYTE *>(::MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0)) : nullptr;

		FeedTs();

		const DWORD Used = pShared != nullptr ? reinterpret_cast<const DWORD *>(pShared)[1] : 0;
		std::printf("  buffered %lu bytes before the command\n", Used);

		check("the buffer holds data for the snapshot", Used > 0);

		//	コマンド実行 -> ワーカーが起動待ちに入る
		g_Host.EventCallback(TVTest::EVENT_COMMAND, 1, 0, g_Host.pEventData);
		::Sleep(500);

		//	スナップショットが作れていない場合、ワーカーは待ちループへ
		//	進まないので、この後の計測が偽陽性になる
		check("the worker really reached the launch wait",
			  !HostLogContains(L"まだ溜まっていません"));

		//	ここで終了。ワーカーを待ち切らないと FreeLibrary で落ちる
		const DWORD t0 = ::GetTickCount();
		const BOOL fFinalized = Api.Finalize();
		const DWORD Elapsed = ::GetTickCount() - t0;

		std::printf("  TVTFinalize() took %lu ms (LaunchWait=600s)\n", Elapsed);

		check("TVTFinalize() succeeds while a request is in flight", fFinalized != FALSE);

		//	待ちループは 250ms 毎に終了要求を見る。
		//	見ていないと Finalize() の join が 5 秒で諦める事になる
		check("TVTFinalize() does not wait for LaunchWait to expire", Elapsed < 3000);

		if (pShared != nullptr)
			::UnmapViewOfFile(pShared);
		if (hMap != nullptr)
			::CloseHandle(hMap);

		::FreeLibrary(Api.hModule);

		//	ワーカーが取り残されていればここで落ちる
		::Sleep(500);
		check("no crash after unloading with a request in flight", true);
	}

done:
	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
