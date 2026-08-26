//----------------------------------------------------------------------------
//	TSMemory (TVTest プラグイン) 64bit / AviUtl ExEdit2 対応版
//
//	SPDX-License-Identifier: GPL-2.0-or-later
//
//	このファイルは TVTest 由来の BonTsEngine (GPL-2.0-or-later) と結合される
//	為、GPL-2.0-or-later の対象です。全文は licenses/GPL-2.0.txt を、
//	適用範囲は LICENSE.md を参照してください。
//	AviUtl2 側のプラグイン (TSMemory-TVTestSrc.aux2) は GPL の対象外です。
//
//	選択中サービスの MPEG-2 VIDEO / 音声の TS パケットをリングバッファ状の
//	共有メモリに溜め込み、コマンド実行時にその時点の内容をスナップショットして
//	AviUtl ExEdit2 側のプラグイン (TSMemory-TVTestSrc.aux2) に読み込ませる。
//
//	AviUtl 1.xx 版はウィンドウクラス "AviUtl" のウィンドウへ WM_DROPFILES を
//	投げていたが、AviUtl ExEdit2 は OLE ドロップで D&D を受けており
//	WM_DROPFILES では反応しない。その為、名前付きイベントによる連携に変更した。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <tchar.h>

//	StringCchPrintf を使う。lstrcpy 等は既存のまま使うので非推奨化はしない
#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>
#include <vector>

#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "TVTestPlugin.h"
#include "BonTsEngine/TsSelector.h"

#include "tsmemory_ipc.h"

#pragma comment(lib, "shlwapi.lib")


#define DEFAULT_BUFFER_LENGTH	(1024 * 1024 * 10 / 188)
#define DEFAULT_SNAPSHOT_COUNT	4

#include <pshpack1.h>

struct BufferInfo {
	DWORD Size;
	DWORD Used;
	DWORD Pos;
	DWORD Reserved;
};

#include <poppack.h>


//	名前付きミューテックスによる排他
class CMutexLock
{
	HANDLE m_hMutex;
	bool m_fFailed;			// 待てずに諦めたか (溜め込みが止まった印)

public:
	CMutexLock() : m_hMutex(nullptr), m_fFailed(false) {}

	bool HasFailed() const { return m_fFailed; }
	~CMutexLock() { Close(); }

	CMutexLock(const CMutexLock &) = delete;
	CMutexLock &operator=(const CMutexLock &) = delete;

	bool Create(LPCTSTR pszName)
	{
		if (m_hMutex != nullptr)
			return false;

		SECURITY_DESCRIPTOR sd;
		SECURITY_ATTRIBUTES sa;
		TSMemoryInitSecurityAttributes(&sd, &sa);

		m_hMutex = ::CreateMutex(&sa, FALSE, pszName);
		return m_hMutex != nullptr;
	}

	void Close()
	{
		if (m_hMutex != nullptr) {
			::CloseHandle(m_hMutex);
			m_hMutex = nullptr;
		}
	}

	bool Lock()
	{
		if (m_hMutex == nullptr)
			return false;
		const DWORD Result = ::WaitForSingleObject(m_hMutex, 2000);
		if (Result != WAIT_OBJECT_0 && Result != WAIT_ABANDONED) {
			//	待てなかった場合は諦める。ここで諦めないと、ストリーム
			//	コールバック (InputMedia) が毎回 2 秒待つ事になり
			//	TVTest 側の受信が詰まる。
			//	ただし一度閉じると以降の Lock() は全て失敗し、
			//	溜め込みが黙って止まる為、判るように記録しておく。
			Close();
			m_fFailed = true;
			return false;
		}
		return true;
	}

	void Unlock()
	{
		if (m_hMutex != nullptr)
			::ReleaseMutex(m_hMutex);
	}
};


//	フォルダを用意して、実際に書ける事まで確かめる。
//
//	作成に成功しても書けるとは限らない。%ProgramData% の下は
//	既定で「作成は出来るが、他の利用者が作った物には書けない」為、
//	フォルダの有無だけでは判定にならない。
static bool PrepareWritableDirectory(LPCTSTR pszDirectory)
{
	if (!::PathIsDirectory(pszDirectory)
			&& ::SHCreateDirectoryEx(nullptr, pszDirectory, nullptr) != ERROR_SUCCESS)
		return false;

	TCHAR szProbe[MAX_PATH];
	::lstrcpyn(szProbe, pszDirectory, MAX_PATH);
	::PathAppend(szProbe, TEXT("tsmemory.write-test"));

	HANDLE hFile = ::CreateFile(szProbe, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
								FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	::CloseHandle(hFile);
	return true;
}


//	スナップショット (ダミーファイル) を置くフォルダを決める。
//
//	以前は自分の DLL の隣、つまり TVTest の Plugins フォルダに作っていた。
//	そこは Program Files 配下だと書き込めない事があり、書けなくても
//	「映像データがまだ溜まっていません」としか出ない状態だった。
static void GetSnapshotDirectory(LPTSTR pszDirectory)
{
	TCHAR szPath[MAX_PATH] = {};

	//	1. %ProgramData%\aviutl2\temp
	if (::GetEnvironmentVariable(TEXT("ProgramData"), szPath, MAX_PATH) != 0) {
		::PathAppend(szPath, TEXT("aviutl2"));
		::PathAppend(szPath, TEXT("temp"));
		if (PrepareWritableDirectory(szPath)) {
			::lstrcpyn(pszDirectory, szPath, MAX_PATH);
			return;
		}
	}

	//	2. %AppData%\aviutl2\temp (利用者ごと。必ず書ける)
	if (::GetEnvironmentVariable(TEXT("AppData"), szPath, MAX_PATH) != 0) {
		::PathAppend(szPath, TEXT("aviutl2"));
		::PathAppend(szPath, TEXT("temp"));
		if (PrepareWritableDirectory(szPath)) {
			::lstrcpyn(pszDirectory, szPath, MAX_PATH);
			return;
		}
	}

	//	3. どちらも駄目なら従来どおり自分の隣
	::GetModuleFileName(g_hinstDLL, szPath, MAX_PATH);
	::PathRemoveFileSpec(szPath);
	::lstrcpyn(pszDirectory, szPath, MAX_PATH);
}


//	AviUtl2 に読ませたスナップショット 1 件分
struct Snapshot {
	HANDLE hFileMapping;
	HANDLE hMutex;
	TCHAR szFileName[MAX_PATH];
};


class CTSMemory : public TVTest::CTVTestPlugin, public CMediaDecoder
{
	HANDLE m_hMutex;
	BYTE *m_pBuffer;
	SIZE_T m_BufferSize;		// パケット数
	SIZE_T m_BufferPos;
	SIZE_T m_BufferUsed;
	HANDLE m_hFileMapping;
	CMutexLock m_BufferLock;
	CTsSelector m_TsSelector;
	CTsPacket m_TsPacket;
	BYTE m_ContCounter[0x1FFF];

	TCHAR m_szIniFileName[MAX_PATH];
	TCHAR m_szAviUtlPath[MAX_PATH];
	TCHAR m_szBaseName[MAX_PATH];	// tsmemoryN 相当のフルパス (拡張子なし)
	TCHAR m_szMutexName[MAX_PATH];
	TCHAR m_szTempDir[MAX_PATH];	// スナップショットを置くフォルダ
	bool m_fCloseAviUtl;
	bool m_fAudio;			// 音声も溜め込むか ([Settings] Audio)
	bool m_fSubtitle;		// 字幕も溜め込むか ([Settings] Subtitle)
	int m_SnapshotCount;
	int m_LaunchWaitSeconds;

	//	取り込み対象のサービスID
	//	CTsSelector では 0 が「全サービス」の意味になる為、視聴中のサービスが
	//	判らない間だけ 0 になる。マルチ編成でサブチャンネルを視聴している時に
	//	0 のままだと全サービスの映像が混ざり、デコーダが PAT の最初の映像
	//	(プライマリチャンネル) を拾ってしまう。
	WORD m_TargetServiceID;

	//	後始末に入った印。AviUtl2 の起動待ちをしているワーカーに
	//	待つのをやめさせる為に使う (Finalize() を参照)
	volatile LONG m_fShutdown;

	std::vector<Snapshot> m_Snapshots;
	DWORD m_Serial;
	DWORD m_AviUtlProcessID;
	HANDLE m_hAviUtlProcess;

	HANDLE m_hRequestThread;

	DWORD GetTargetStreams() const;
	void FreeBuffer();
	void PurgeBuffer();
	void UpdateTargetService();
	void ReleaseSnapshots();
	bool CreateSnapshot(LPTSTR pszFileNameBuf);
	bool IsAviUtlReady() const;
	bool IsAviUtlRunning() const;
	bool LaunchAviUtl();
	bool SendRequest(LPCTSTR pszFileName);
	void ExecuteCapture();

	static DWORD WINAPI RequestThreadProc(LPVOID pParameter);
	static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData);
	static BOOL CALLBACK StreamCallback(BYTE *pData, void *pClientData);

public:
	CTSMemory();
	~CTSMemory();

	virtual bool GetPluginInfo(TVTest::PluginInfo *pInfo) override;
	virtual bool Initialize() override;
	virtual bool Finalize() override;

	//	CMediaDecoder
	virtual const bool InputMedia(CMediaData *pMediaData, const DWORD dwInputIndex = 0UL) override;
};


CTSMemory::CTSMemory()
	: m_hMutex(nullptr)
	, m_pBuffer(nullptr)
	, m_BufferSize(DEFAULT_BUFFER_LENGTH)
	, m_BufferPos(0)
	, m_BufferUsed(0)
	, m_hFileMapping(nullptr)
	, m_fCloseAviUtl(false)
	, m_fAudio(false)
	, m_fSubtitle(false)
	, m_SnapshotCount(DEFAULT_SNAPSHOT_COUNT)
	, m_LaunchWaitSeconds(30)
	, m_TargetServiceID(0)
	, m_fShutdown(0)
	, m_Serial(0)
	, m_AviUtlProcessID(0)
	, m_hAviUtlProcess(nullptr)
	, m_hRequestThread(nullptr)
{
	m_szIniFileName[0] = _T('\0');
	m_szAviUtlPath[0] = _T('\0');
	m_szBaseName[0] = _T('\0');
	m_szMutexName[0] = _T('\0');
	m_szTempDir[0] = _T('\0');
}


CTSMemory::~CTSMemory()
{
}


bool CTSMemory::GetPluginInfo(TVTest::PluginInfo *pInfo)
{
	pInfo->Type           = TVTest::PLUGIN_TYPE_NORMAL;
	pInfo->Flags          = 0;
	pInfo->pszPluginName  = L"TSMemory";
	pInfo->pszCopyright   = L"Public Domain";
	pInfo->pszDescription = L"映像を AviUtl ExEdit2 に送ります";
	return true;
}


bool CTSMemory::Initialize()
{
	//	設定の読み込み
	::GetModuleFileName(g_hinstDLL, m_szIniFileName, MAX_PATH);
	::PathRenameExtension(m_szIniFileName, TEXT(".ini"));

	int MemorySize = ::GetPrivateProfileInt(TEXT("Settings"), TEXT("MemorySize"), 10, m_szIniFileName);
	if (MemorySize < 1)
		MemorySize = 1;
	m_BufferSize = (SIZE_T)MemorySize * (1024 * 1024) / 188;

	//	共有メモリの大きさは DWORD で扱う (BufferInfo も IPC も 32bit)。
	//	そのまま渡すと CreateFileMapping() に切り捨てられた値が渡り、
	//	確保した範囲の外へ書き込む事になる。
	//	例) MemorySize=4097 -> 必要 4296015840 バイトに対して
	//	    実際に確保されるのは 1048544 バイト
	{
		const SIZE_T MaxPackets = (0xFFFFFFFFu - sizeof(BufferInfo)) / 188;

		if (m_BufferSize > MaxPackets) {
			TCHAR szLog[160];

			m_BufferSize = MaxPackets;
			::StringCchPrintf(szLog, ARRAYSIZE(szLog),
							  TEXT("MemorySize=%d は大きすぎる為 %u MB に制限しました。"),
							  MemorySize,
							  static_cast<unsigned int>(m_BufferSize * 188 / (1024 * 1024)));
			m_pApp->AddLog(szLog);
		}
	}

	::GetPrivateProfileString(TEXT("Settings"), TEXT("AviUtlPath"), NULL,
							  m_szAviUtlPath, MAX_PATH, m_szIniFileName);
	if (m_szAviUtlPath[0] == _T('\0')) {
		m_pApp->AddLog(TEXT("aviutl2.exe のパスが設定されていません。"));
		return false;
	}

	m_fCloseAviUtl = ::GetPrivateProfileInt(TEXT("Settings"), TEXT("AutoClose"), 0, m_szIniFileName) != 0;

	//	音声も溜め込むか。既定は映像のみ (従来どおり)。
	//	AviUtl2 側の [M2V] audio と揃えて使う。
	m_fAudio = ::GetPrivateProfileInt(TEXT("Settings"), TEXT("Audio"), 0, m_szIniFileName) != 0;
	m_fSubtitle = ::GetPrivateProfileInt(TEXT("Settings"), TEXT("Subtitle"), 0, m_szIniFileName) != 0;

	m_SnapshotCount = ::GetPrivateProfileInt(TEXT("Settings"), TEXT("SnapshotCount"),
											 DEFAULT_SNAPSHOT_COUNT, m_szIniFileName);
	if (m_SnapshotCount < 1)
		m_SnapshotCount = 1;
	else if (m_SnapshotCount > 32)
		m_SnapshotCount = 32;

	//	上限が無いと ExecuteCapture() の (秒 * 1000) で int が溢れる。
	//	AviUtl2 側の ReadyTimeout と同じ 600 秒までにする。
	m_LaunchWaitSeconds = ::GetPrivateProfileInt(TEXT("Settings"), TEXT("LaunchWait"), 30, m_szIniFileName);
	if (m_LaunchWaitSeconds < 5)
		m_LaunchWaitSeconds = 5;
	else if (m_LaunchWaitSeconds > 600)
		m_LaunchWaitSeconds = 600;

	//	スナップショットの置き場所を決める
	GetSnapshotDirectory(m_szTempDir);
	{
		TCHAR szLog[MAX_PATH + 64];
		::StringCchPrintf(szLog, ARRAYSIZE(szLog),
						  TEXT("作業フォルダ : %s"), m_szTempDir);
		m_pApp->AddLog(szLog);
	}

	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;
	TSMemoryInitSecurityAttributes(&sd, &sa);

	//	TVTest の多重起動に備えて空いている番号を取る
	for (int i = 0; i < 100; i++) {
		TCHAR szName[64];

		::StringCchPrintf(szName, ARRAYSIZE(szName), TEXT("tsmemory%d.tvtv.mutex"), i);
		m_hMutex = ::CreateMutex(&sa, FALSE, szName);
		if (m_hMutex != nullptr) {
			if (::GetLastError() != ERROR_ALREADY_EXISTS) {
				::lstrcpy(m_szMutexName, szName);
				::lstrcpyn(m_szBaseName, m_szTempDir, MAX_PATH);
				::PathAppend(m_szBaseName, TEXT("x"));
				//	ファイル名部分を差し替える。書ける残りの分しか使わない
				LPTSTR pszName = ::PathFindFileName(m_szBaseName);
				::StringCchPrintf(pszName,
								  MAX_PATH - static_cast<size_t>(pszName - m_szBaseName),
								  TEXT("tsmemory%d"), i);
				break;
			}
			::CloseHandle(m_hMutex);
			m_hMutex = nullptr;
		}
	}
	if (m_hMutex == nullptr) {
		m_pApp->AddLog(TEXT("Mutex を作成できません。"));
		return false;
	}

	//	前回 TVTest が異常終了した場合、自分の番号のダミーファイルが
	//	残ったままになる (消すのは Finalize() だけの為)。
	//	ミューテックスを取れた = この番号は自分の物なので、消して構わない。
	{
		TCHAR szPattern[MAX_PATH];
		WIN32_FIND_DATA fd;

		::StringCchPrintf(szPattern, ARRAYSIZE(szPattern), TEXT("%s_*.tvtv"), m_szBaseName);

		HANDLE hFind = ::FindFirstFile(szPattern, &fd);
		if (hFind != INVALID_HANDLE_VALUE) {
			int Removed = 0;
			do {
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
					continue;
				TCHAR szPath[MAX_PATH];
				::lstrcpyn(szPath, m_szTempDir, MAX_PATH);
				::PathAppend(szPath, fd.cFileName);
				if (::DeleteFile(szPath))
					Removed++;
			} while (::FindNextFile(hFind, &fd));
			::FindClose(hFind);

			if (Removed > 0) {
				TCHAR szLog[128];
				::StringCchPrintf(szLog, ARRAYSIZE(szLog),
								  TEXT("前回の残骸 %d 個を削除しました。"), Removed);
				m_pApp->AddLog(szLog);
			}
		}
	}

	m_BufferLock.Create(m_szMutexName);

	m_TsPacket.GetBuffer(188);
	::FillMemory(m_ContCounter, sizeof(m_ContCounter), 0x10);

	m_TsSelector.SetOutputDecoder(this);

	m_pApp->RegisterCommand(1, L"Execute", L"実行");
	m_pApp->SetEventCallback(EventCallback, this);
	m_pApp->SetStreamCallback(0, StreamCallback, this);

	return true;
}


bool CTSMemory::Finalize()
{
	//	AviUtl2 の待ち受け開始を待っているワーカーに、待つのをやめさせる。
	//	既定で 30 秒 (LaunchWait) 待つ為、これを先に立てておかないと
	//	下の join が待ち切れない。
	::InterlockedExchange(&m_fShutdown, 1);

	if (m_fCloseAviUtl && m_AviUtlProcessID != 0) {
		HANDLE hProcess = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, m_AviUtlProcessID);
		if (hProcess != nullptr) {
			//	起動した AviUtl2 のウィンドウに WM_CLOSE を投げる
			struct EnumInfo {
				DWORD ProcessID;
			} Info = { m_AviUtlProcessID };

			::EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
				EnumInfo *pInfo = reinterpret_cast<EnumInfo *>(lParam);
				DWORD ProcessID = 0;
				::GetWindowThreadProcessId(hwnd, &ProcessID);
				if (ProcessID == pInfo->ProcessID && ::GetWindow(hwnd, GW_OWNER) == nullptr
						&& ::IsWindowVisible(hwnd)) {
					::PostMessage(hwnd, WM_CLOSE, 0, 0);
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&Info));

			::CloseHandle(hProcess);
		}
	}

	if (m_hRequestThread != nullptr) {
		//	m_fShutdown を見て 250ms 以内に降りてくる。
		//	降りて来ない場合 (要求の結果をメッセージボックスで出していて
		//	ユーザーが閉じていない等) は、この後の後始末をやめる。
		//	スナップショットの削除や共有メモリの解放を先にやってしまうと、
		//	生きているワーカーが無効になった this / m_pApp を触る事になる。
		//	何もしなければプロセスの終了時に OS が回収する。
		const DWORD Result = ::WaitForSingleObject(m_hRequestThread, 5000);

		::CloseHandle(m_hRequestThread);
		m_hRequestThread = nullptr;

		if (Result != WAIT_OBJECT_0) {
			m_pApp->AddLog(TEXT("要求の処理が終わらない為、後始末を見送りました。"));
			return true;
		}
	}

	if (m_hAviUtlProcess != nullptr) {
		::CloseHandle(m_hAviUtlProcess);
		m_hAviUtlProcess = nullptr;
	}

	ReleaseSnapshots();
	FreeBuffer();
	m_BufferLock.Close();

	if (m_hMutex != nullptr) {
		::CloseHandle(m_hMutex);
		m_hMutex = nullptr;
	}

	return true;
}


void CTSMemory::FreeBuffer()
{
	if (m_BufferLock.Lock()) {
		if (m_pBuffer != nullptr) {
			::UnmapViewOfFile(m_pBuffer);
			m_pBuffer = nullptr;
			::CloseHandle(m_hFileMapping);
			m_hFileMapping = nullptr;
		}
		m_BufferLock.Unlock();
	}
}


//	CTsSelector に残させるストリームの種別。
//
//	音声を含めないと、AviUtl2 側の m2v は音声を 1 つも見つけられない。
//	既定が映像のみなのは、音声の取り込みが未検証な為
//	(詳細は docs/audio-support.md)。
DWORD CTSMemory::GetTargetStreams() const
{
	DWORD Streams = CTsSelector::STREAM_MPEG2VIDEO;

	if (m_fAudio)
		Streams |= CTsSelector::STREAM_AAC;

	//	字幕 (stream_type 0x06)。**AviUtl2 側だけを 1 にしても届かない。**
	//	ここで落とすと後段で何をしても取り返せない
	if (m_fSubtitle)
		Streams |= CTsSelector::STREAM_SUBTITLE;

	return Streams;
}


//	視聴中のサービスを取り込み対象にする。
//
//	マルチ編成 (サブチャンネル) の場合、対象を絞らないと全サービスの映像が
//	混ざり、デコーダが PAT の最初の映像 = プライマリチャンネルを拾ってしまう。
void CTSMemory::UpdateTargetService()
{
	WORD ServiceID = 0;

	const int Service = m_pApp->GetService();
	if (Service >= 0) {
		TVTest::ServiceInfo Info;

		if (m_pApp->GetServiceInfo(Service, &Info))
			ServiceID = Info.ServiceID;
	}

	if (ServiceID == m_TargetServiceID)
		return;

	m_TargetServiceID = ServiceID;
	m_TsSelector.SetTargetServiceID(ServiceID, GetTargetStreams());

	//	切り替え前のサービスのパケットを残さない
	PurgeBuffer();

	TCHAR szLog[128];
	if (ServiceID != 0) {
		::StringCchPrintf(szLog, ARRAYSIZE(szLog), TEXT("取り込み対象をサービス %u に切り替えました。"), ServiceID);
	} else {
		::lstrcpy(szLog, TEXT("視聴中のサービスが判らない為、全サービスを取り込みます。"));
	}
	m_pApp->AddLog(szLog);
}


void CTSMemory::PurgeBuffer()
{
	if (m_BufferLock.Lock()) {
		m_BufferUsed = 0;
		m_BufferPos = 0;
		if (m_pBuffer != nullptr) {
			BufferInfo *pInfo = reinterpret_cast<BufferInfo *>(m_pBuffer);
			pInfo->Used = 0;
			pInfo->Pos = 0;
		}
		m_BufferLock.Unlock();
	}
}


void CTSMemory::ReleaseSnapshots()
{
	for (Snapshot &s : m_Snapshots) {
		if (s.hFileMapping != nullptr)
			::CloseHandle(s.hFileMapping);
		if (s.hMutex != nullptr)
			::CloseHandle(s.hMutex);
		if (s.szFileName[0] != _T('\0'))
			::DeleteFile(s.szFileName);
	}
	m_Snapshots.clear();
}


//	現在のリングバッファの内容を、先頭から連続した別の共有メモリに複製する。
//
//	AviUtl ExEdit2 はメディアファイルの内容をパスをキーにキャッシュするので、
//	AviUtl 1.xx 版のように毎回同じ名前のファイルを読ませると 2 回目以降に
//	前回の映像が出てしまう。その為キャプチャの度に別名で作る。
bool CTSMemory::CreateSnapshot(LPTSTR pszFileNameBuf)
{
	if (!m_BufferLock.Lock())
		return false;

	bool fOK = false;

	if (m_pBuffer != nullptr && m_BufferUsed > 0) {
		const DWORD DataSize = static_cast<DWORD>(m_BufferUsed * 188);
		const DWORD MapSize = static_cast<DWORD>(sizeof(BufferInfo)) + DataSize;

		SECURITY_DESCRIPTOR sd;
		SECURITY_ATTRIBUTES sa;
		TSMemoryInitSecurityAttributes(&sd, &sa);

		Snapshot s = {};
		TCHAR szName[MAX_PATH];
		TCHAR szMutexName[MAX_PATH];

		m_Serial++;
		::StringCchPrintf(s.szFileName, ARRAYSIZE(s.szFileName), TEXT("%s_%u.tvtv"), m_szBaseName, m_Serial);
		::lstrcpy(szName, ::PathFindFileName(s.szFileName));
		::StringCchPrintf(szMutexName, ARRAYSIZE(szMutexName), TEXT("%s.mutex"), szName);

		//	open_shared_memory() は "<名前>.mutex" の存在を前提にしている
		s.hMutex = ::CreateMutex(&sa, FALSE, szMutexName);
		s.hFileMapping = ::CreateFileMapping(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
											 0, MapSize, szName);
		if (s.hMutex != nullptr && s.hFileMapping != nullptr) {
			BYTE *pDest = static_cast<BYTE *>(::MapViewOfFile(s.hFileMapping, FILE_MAP_WRITE, 0, 0, 0));
			if (pDest != nullptr) {
				BufferInfo *pInfo = reinterpret_cast<BufferInfo *>(pDest);
				pInfo->Size = DataSize;
				pInfo->Used = DataSize;
				pInfo->Pos = 0;
				pInfo->Reserved = 0;

				//	リングバッファを線形化してコピーする
				const BYTE *pSrc = m_pBuffer + sizeof(BufferInfo);
				const SIZE_T FirstPackets = min(m_BufferUsed, m_BufferSize - m_BufferPos);
				::CopyMemory(pDest + sizeof(BufferInfo),
							 pSrc + m_BufferPos * 188, FirstPackets * 188);
				if (FirstPackets < m_BufferUsed) {
					::CopyMemory(pDest + sizeof(BufferInfo) + FirstPackets * 188,
								 pSrc, (m_BufferUsed - FirstPackets) * 188);
				}

				::UnmapViewOfFile(pDest);

				//	AviUtl2 に渡すダミーファイル (0 バイト) を作る
				HANDLE hFile = ::CreateFile(s.szFileName, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
											CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (hFile != INVALID_HANDLE_VALUE) {
					::CloseHandle(hFile);
					::lstrcpy(pszFileNameBuf, s.szFileName);
					m_Snapshots.push_back(s);
					fOK = true;

					TCHAR szLog[MAX_PATH + 128];
					::StringCchPrintf(szLog, ARRAYSIZE(szLog),
							   TEXT("%u パケット (%u.%02u MB / バッファの %u%%) を書き出しました : %s"),
							   static_cast<DWORD>(m_BufferUsed),
							   DataSize / (1024 * 1024),
							   DataSize % (1024 * 1024) * 100 / (1024 * 1024),
							   static_cast<DWORD>(m_BufferUsed * 100 / m_BufferSize),
							   ::PathFindFileName(s.szFileName));
					m_pApp->AddLog(szLog);
				} else {
					//	ここで黙って戻ると、呼び出し側が
					//	「映像データがまだ溜まっていません」と出してしまう。
					//	書けない場所を使っているだけの事があるので理由を残す
					TCHAR szLog[MAX_PATH + 128];
					::StringCchPrintf(szLog, ARRAYSIZE(szLog),
									  TEXT("ファイルを作成できません (エラー %lu) : %s"),
									  ::GetLastError(), s.szFileName);
					m_pApp->AddLog(szLog);
				}
			}
		}

		if (!fOK) {
			if (s.hFileMapping != nullptr)
				::CloseHandle(s.hFileMapping);
			if (s.hMutex != nullptr)
				::CloseHandle(s.hMutex);
		}
	}

	m_BufferLock.Unlock();

	//	古いスナップショットを破棄する
	while (static_cast<int>(m_Snapshots.size()) > m_SnapshotCount) {
		Snapshot &s = m_Snapshots.front();
		if (s.hFileMapping != nullptr)
			::CloseHandle(s.hFileMapping);
		if (s.hMutex != nullptr)
			::CloseHandle(s.hMutex);
		::DeleteFile(s.szFileName);
		m_Snapshots.erase(m_Snapshots.begin());
	}

	return fOK;
}


//	AviUtl2 側のプラグインが待ち受けているか
bool CTSMemory::IsAviUtlReady() const
{
	HANDLE hMutex = ::OpenMutexW(SYNCHRONIZE, FALSE, TSMEMORY_IPC_READY_MUTEX);
	if (hMutex == nullptr)
		return false;
	::CloseHandle(hMutex);
	return true;
}


//	AviUtl2 のプロセスが既に在るか
//
//	IsAviUtlReady() は「AviUtl2 側プラグインが待ち受けているか」しか見ない。
//	待ち受けはプロジェクトの初期化後に始まる為、起動直後はまだ偽になる。
//	またプラグインが入っていない・無効な場合は永久に偽のままになる。
//	その状態で LaunchAviUtl() を呼ぶと AviUtl2 を何重にも起動してしまい、
//	二重起動したインスタンスが異常終了する。実行ファイル名で在否を確認する。
bool CTSMemory::IsAviUtlRunning() const
{
	LPCTSTR pszExeName = ::PathFindFileName(m_szAviUtlPath);
	if (pszExeName == nullptr || pszExeName[0] == _T('\0'))
		return false;

	HANDLE hSnapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE)
		return false;

	PROCESSENTRY32 pe = {};
	pe.dwSize = sizeof(pe);
	bool fFound = false;

	if (::Process32First(hSnapshot, &pe)) {
		do {
			if (::lstrcmpi(pe.szExeFile, pszExeName) == 0) {
				fFound = true;
				break;
			}
		} while (::Process32Next(hSnapshot, &pe));
	}

	::CloseHandle(hSnapshot);
	return fFound;
}


bool CTSMemory::LaunchAviUtl()
{
	STARTUPINFO si = {};
	PROCESS_INFORMATION pi = {};
	TCHAR szCommandLine[MAX_PATH + 4];
	TCHAR szDirectory[MAX_PATH];

	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOWNORMAL;

	::StringCchPrintf(szCommandLine, ARRAYSIZE(szCommandLine), TEXT("\"%s\""), m_szAviUtlPath);

	//	カレントディレクトリは AviUtl2 の実行ファイルの場所にする。
	//
	//	指定しないと TVTest のカレントディレクトリを引き継いでしまう。
	//	AviUtl2 は自分のフォルダに style.conf / effect.conf / script.*2 /
	//	Default.aul2 等の起動時に必要なファイルを持っており、これらを
	//	カレントディレクトリ基準で探されると見つからず、起動途中で落ちる。
	//	エクスプローラから起動した場合は自分のフォルダがカレントになる為、
	//	「TVTest から起動した時だけ落ちる」という形で表面化する。
	::lstrcpyn(szDirectory, m_szAviUtlPath, MAX_PATH);
	if (!::PathRemoveFileSpec(szDirectory))
		szDirectory[0] = _T('\0');

	if (!::CreateProcess(nullptr, szCommandLine, nullptr, nullptr, FALSE,
						 NORMAL_PRIORITY_CLASS, nullptr,
						 szDirectory[0] != _T('\0') ? szDirectory : nullptr,
						 &si, &pi)) {
		return false;
	}

	::CloseHandle(pi.hThread);
	m_AviUtlProcessID = pi.dwProcessId;

	//	待ち受け開始を待つ間、プロセスが落ちていないかを見る為に保持しておく
	if (m_hAviUtlProcess != nullptr)
		::CloseHandle(m_hAviUtlProcess);
	m_hAviUtlProcess = pi.hProcess;
	return true;
}


//	AviUtl2 側へ読み込み要求を送る
bool CTSMemory::SendRequest(LPCTSTR pszFileName)
{
	HANDLE hMap = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, TSMEMORY_IPC_PARAM_MAP);
	HANDLE hMutex = ::OpenMutexW(SYNCHRONIZE, FALSE, TSMEMORY_IPC_PARAM_MUTEX);
	HANDLE hEvent = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, TSMEMORY_IPC_REQUEST_EVENT);
	bool fOK = false;

	if (hMap != nullptr && hMutex != nullptr && hEvent != nullptr) {
		TSMEMORY_REQUEST *pParam =
			static_cast<TSMEMORY_REQUEST *>(::MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0));
		if (pParam != nullptr) {
			if (::WaitForSingleObject(hMutex, 5000) == WAIT_OBJECT_0) {
				pParam->Version = TSMEMORY_IPC_VERSION;
				pParam->Serial = m_Serial;
#ifdef UNICODE
				::lstrcpynW(pParam->FileName, pszFileName, MAX_PATH);
#else
				::MultiByteToWideChar(CP_ACP, 0, pszFileName, -1, pParam->FileName, MAX_PATH);
#endif
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


//	コマンド実行時の処理本体 (ワーカースレッドから呼ばれる)
void CTSMemory::ExecuteCapture()
{
	TCHAR szFileName[MAX_PATH];

	if (!CreateSnapshot(szFileName)) {
		if (m_BufferLock.HasFailed()) {
			m_pApp->AddLog(TEXT("排他に失敗した為、映像の溜め込みが止まっています。")
						   TEXT("TVTest を再起動してください。"));
		} else {
			m_pApp->AddLog(TEXT("映像データがまだ溜まっていません。"));
		}
		return;
	}

	if (!IsAviUtlReady()) {
		//	既に AviUtl2 が動いている場合は起動しない。
		//	待ち受けはプロジェクトの初期化後に始まる為、起動直後はまだ
		//	応答しない。ここで起動してしまうと二重起動になる。
		const bool fAlreadyRunning = IsAviUtlRunning();

		if (!fAlreadyRunning) {
			if (!::PathFileExists(m_szAviUtlPath)) {
				::MessageBox(m_pApp->GetAppWindow(),
							 TEXT("aviutl2.exe のパスが間違っていると思われます。\n")
							 TEXT("TSMemory.ini の設定を見直してみてください。"),
							 NULL, MB_OK | MB_ICONEXCLAMATION);
				return;
			}

			if (!LaunchAviUtl()) {
				::MessageBox(m_pApp->GetAppWindow(), TEXT("AviUtl ExEdit2 を起動できません。"),
							 NULL, MB_OK | MB_ICONEXCLAMATION);
				return;
			}
		} else {
			m_pApp->AddLog(TEXT("AviUtl ExEdit2 は既に起動しています。")
						   TEXT("待ち受けの開始を待ちます。"));
		}

		//	AviUtl2 側のプラグインが待ち受け状態になるのを待つ。
		//	待ち受け開始は AviUtl2 のプロジェクト初期化が終わってからなので、
		//	起動が遅い環境では数秒〜十数秒かかる事がある。
		const DWORD Limit = ::GetTickCount()
			+ static_cast<DWORD>(m_LaunchWaitSeconds) * 1000;
		while (!IsAviUtlReady()) {
			//	プラグインの無効化・TVTest の終了に入ったら待たない。
			//	待ち続けると Finalize() に取り残され、アンロード済みの
			//	コードや無効になった this を触る事になる。
			if (::InterlockedCompareExchange(&m_fShutdown, 0, 0) != 0)
				return;

			//	自分で起動した AviUtl2 が落ちた場合は待たずに切り上げる
			if (!fAlreadyRunning && m_hAviUtlProcess != nullptr
					&& ::WaitForSingleObject(m_hAviUtlProcess, 0) == WAIT_OBJECT_0) {
				::MessageBox(m_pApp->GetAppWindow(),
							 TEXT("AviUtl ExEdit2 が起動直後に終了しました。"),
							 NULL, MB_OK | MB_ICONEXCLAMATION);
				return;
			}

			if (static_cast<LONG>(::GetTickCount() - Limit) >= 0) {
				if (fAlreadyRunning) {
					::MessageBox(m_pApp->GetAppWindow(),
								 TEXT("AviUtl ExEdit2 は起動していますが、\n")
								 TEXT("TSMemory-TVTestSrc.aux2 が応答しません。\n")
								 TEXT("プラグインが正しく配置され、有効になっているか\n")
								 TEXT("確認してください。"),
								 NULL, MB_OK | MB_ICONEXCLAMATION);
				} else {
					::MessageBox(m_pApp->GetAppWindow(),
								 TEXT("AviUtl ExEdit2 側の TSMemory-TVTestSrc.aux2 が応答しません。\n")
								 TEXT("プラグインが正しく配置されているか確認してください。\n")
								 TEXT("起動に時間がかかる環境の場合は TSMemory.ini の\n")
								 TEXT("LaunchWait を長くしてください。"),
								 NULL, MB_OK | MB_ICONEXCLAMATION);
				}
				return;
			}
			::Sleep(250);
		}
	}

	if (!SendRequest(szFileName)) {
		::MessageBox(m_pApp->GetAppWindow(),
					 TEXT("AviUtl ExEdit2 に読み込み要求を送れませんでした。"),
					 NULL, MB_OK | MB_ICONEXCLAMATION);
	}
}


DWORD WINAPI CTSMemory::RequestThreadProc(LPVOID pParameter)
{
	static_cast<CTSMemory *>(pParameter)->ExecuteCapture();
	return 0;
}


LRESULT CALLBACK CTSMemory::EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData)
{
	CTSMemory *pThis = static_cast<CTSMemory *>(pClientData);

	switch (Event) {
	case TVTest::EVENT_PLUGINENABLE:
		if (lParam1 != 0) {
			BOOL fOK = FALSE;

			if (!pThis->m_BufferLock.Lock())
				return FALSE;
			if (pThis->m_pBuffer == nullptr) {
				const DWORD BufferSize =
					static_cast<DWORD>(sizeof(BufferInfo) + pThis->m_BufferSize * 188);
				SECURITY_DESCRIPTOR sd;
				SECURITY_ATTRIBUTES sa;
				TCHAR szName[MAX_PATH];

				TSMemoryInitSecurityAttributes(&sd, &sa);
				::StringCchPrintf(szName, ARRAYSIZE(szName), TEXT("%s.tvtv"), ::PathFindFileName(pThis->m_szBaseName));

				pThis->m_BufferPos = 0;
				pThis->m_BufferUsed = 0;
				pThis->m_hFileMapping = ::CreateFileMapping(INVALID_HANDLE_VALUE, &sa,
															PAGE_READWRITE, 0, BufferSize, szName);
				if (pThis->m_hFileMapping != nullptr) {
					if (::GetLastError() != ERROR_ALREADY_EXISTS) {
						pThis->m_pBuffer = static_cast<BYTE *>(
							::MapViewOfFile(pThis->m_hFileMapping, FILE_MAP_WRITE, 0, 0, 0));
						if (pThis->m_pBuffer != nullptr) {
							BufferInfo *pInfo = reinterpret_cast<BufferInfo *>(pThis->m_pBuffer);
							pInfo->Size = static_cast<DWORD>(pThis->m_BufferSize * 188);
							pInfo->Used = 0;
							pInfo->Pos = 0;
							pInfo->Reserved = 0;
							fOK = TRUE;
						}
					}
					if (!fOK) {
						::CloseHandle(pThis->m_hFileMapping);
						pThis->m_hFileMapping = nullptr;
					}
				}
			} else {
				fOK = TRUE;
			}
			pThis->m_BufferLock.Unlock();

			//	有効化した時点で視聴中のサービスを取り込み対象にする
			if (fOK)
				pThis->UpdateTargetService();

			return fOK;
		} else {
			pThis->FreeBuffer();
		}
		return TRUE;

	case TVTest::EVENT_COMMAND:
		if (pThis->m_pBuffer != nullptr) {
			//	AviUtl2 の起動待ちで TVTest がブロックしないようにワーカーで処理する
			if (pThis->m_hRequestThread != nullptr) {
				if (::WaitForSingleObject(pThis->m_hRequestThread, 0) != WAIT_OBJECT_0) {
					pThis->m_pApp->AddLog(TEXT("前回の要求を処理中です。"));
					return TRUE;
				}
				::CloseHandle(pThis->m_hRequestThread);
				pThis->m_hRequestThread = nullptr;
			}
			pThis->m_hRequestThread =
				::CreateThread(nullptr, 0, RequestThreadProc, pThis, 0, nullptr);
		}
		return TRUE;

	case TVTest::EVENT_CHANNELCHANGE:
	case TVTest::EVENT_DRIVERCHANGE:
		pThis->PurgeBuffer();
		pThis->UpdateTargetService();
		return TRUE;

	case TVTest::EVENT_SERVICECHANGE:
	case TVTest::EVENT_SERVICEUPDATE:
		pThis->UpdateTargetService();
		return TRUE;
	}

	return 0;
}


BOOL CALLBACK CTSMemory::StreamCallback(BYTE *pData, void *pClientData)
{
	CTSMemory *pThis = static_cast<CTSMemory *>(pClientData);

	pThis->m_TsPacket.SetData(pData, 188);

	//	壊れたパケットは捨てる。
	//
	//	ParsePacket() は同期バイト不正 (EC_FORMAT) と
	//	受信時のビット誤り (EC_TRANSPORT、TS ヘッダの
	//	transport_error_indicator) を判定するが、従来は戻り値を
	//	捨てていた。壊れたまま溜め込むと m2v のパーサに渡る事になる。
	//	m2v は 2003 年頃の実装で、壊れた MPEG-2 で GOP リストの作成が
	//	返ってこなくなる事がある (tests/test_fuzz.cpp で再現する)。
	//
	//	EC_CONTINUITY は「パケットが落ちた」だけでパケット自体は正常
	//	なので捨てない。受信状況が悪い時に全部捨ててしまう事になる。
	const DWORD Result = pThis->m_TsPacket.ParsePacket(pThis->m_ContCounter);
	if (Result == CTsPacket::EC_FORMAT || Result == CTsPacket::EC_TRANSPORT)
		return TRUE;

	pThis->m_TsSelector.InputMedia(&pThis->m_TsPacket);
	return TRUE;
}


const bool CTSMemory::InputMedia(CMediaData *pMediaData, const DWORD dwInputIndex)
{
	if (static_cast<CTsPacket *>(pMediaData)->IsScrambled())
		return true;

	//	下で 188 バイト固定で複製する為、実際の長さを確かめておく
	//	(CTsSelector が渡すのは常に 188 バイトの TS パケットだが、
	//	 固定長のコピー元としては確認しておく)
	if (pMediaData->GetSize() != 188)
		return true;

	if (m_BufferLock.Lock()) {
		if (m_pBuffer != nullptr) {
			BufferInfo *pInfo = reinterpret_cast<BufferInfo *>(m_pBuffer);

			::CopyMemory(m_pBuffer + sizeof(BufferInfo)
							+ (m_BufferPos + m_BufferUsed) % m_BufferSize * 188,
						 pMediaData->GetData(), 188);
			if (m_BufferUsed < m_BufferSize) {
				m_BufferUsed++;
				pInfo->Used = static_cast<DWORD>(m_BufferUsed * 188);
			} else {
				m_BufferPos++;
				if (m_BufferPos == m_BufferSize)
					m_BufferPos = 0;
				pInfo->Pos = static_cast<DWORD>(m_BufferPos * 188);
			}
		}
		m_BufferLock.Unlock();
	}

	return true;
}


TVTest::CTVTestPlugin *CreatePluginClass()
{
	return new CTSMemory;
}
