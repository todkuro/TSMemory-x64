//----------------------------------------------------------------------------
//	TVTest Video Reader for AviUtl ExEdit2 (.aui2 相当)
//
//	TVTest プラグイン (TSMemory.tvtp) が共有メモリに書き出した MPEG-2 TS を
//	ダミーファイル *.tvtv 経由で読み込む入力プラグイン。
//	デコーダは茂木和洋氏の MPEG-2 VIDEO VFAPI Plug-In (m2v) をそのまま利用する。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>

//	StringCchPrintfW を使う。lstrcpynW 等は引き続き使うので非推奨化はしない
#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>
#include <memory>
#include <new>
#include <vector>

#include "input2.h"
#include "input_tvtv.h"
#include "plugin_main.h"

extern "C" {
#include "mpeg_video.h"
}

//	音声は src/aviutl2/audio/ に閉じている。
//	m2v の音声デコーダは Program Stream 専用・実ファイル前提・
//	Layer II 専用で流用出来ない (docs/audio-support.md の 2-2)
#include "tvtv_audio.h"

namespace {

//	ワイド文字のパスを m2v (ANSI API 前提) に渡せる形へ変換する。
//
//	m2v は渡されたパスでファイルを開かない。open_shared_memory() が
//	get_filename() でファイル名部分だけを取り出し、それを共有メモリと
//	ミューテックスの名前に使う (shared_memory.c)。
//	そのファイル名は常に "tsmemoryN_M.tvtv" という ASCII 名。
//
//	一方フォルダ側には何が入るか判らない。CP_ACP (日本語環境では CP932) で
//	表せない文字 — 例えば利用者名がキリル文字やハングル — があると、
//	既定文字 '?' に置き換わったまま気付けない。
//	その場合はファイル名だけを渡す。m2v が使うのはそこだけなので、
//	フォルダを落としても動作は変わらない。
bool ToAnsiPath(LPCWSTR src, char *dst, int dst_size)
{
	BOOL fUsedDefault = FALSE;
	int len = ::WideCharToMultiByte(CP_ACP, 0, src, -1, dst, dst_size,
									nullptr, &fUsedDefault);

	if (len > 0 && !fUsedDefault)
		return true;

	//	表せない文字があった (または収まらなかった) のでファイル名だけにする
	LPCWSTR pszName = ::PathFindFileNameW(src);

	fUsedDefault = FALSE;
	len = ::WideCharToMultiByte(CP_ACP, 0, pszName, -1, dst, dst_size,
								nullptr, &fUsedDefault);

	return len > 0 && !fUsedDefault;
}

//	m2v は BITMAPINFOHEADER の biHeight が正でも画像を上から下の順で
//	書き込む (AviUtl 1.xx はそれを前提にしていた)。
//	AviUtl ExEdit2 で上下が逆になる場合の逃げ道として ini で反転出来るようにする。
//	  [M2V]
//	  flip=1
bool GetFlipSetting()
{
	LPCWSTR pszIniFile = TSMemoryGetIniFileName();
	if (pszIniFile == nullptr || pszIniFile[0] == L'\0')
		return false;
	return ::GetPrivateProfileIntW(L"M2V", L"flip", 0, pszIniFile) != 0;
}

//	音声も扱うか。
//	  [M2V]
//	  audio=1
//
//	既定は無効。無効の間は音声デコーダを開かないので、
//	「毎回、居ない音声を探して回る」事も無くなる
//	(TVTest 側が [Settings] Audio=0 の時は音声 PID が落とされている)。
bool GetAudioSetting()
{
	LPCWSTR pszIniFile = TSMemoryGetIniFileName();
	if (pszIniFile == nullptr || pszIniFile[0] == L'\0')
		return false;
	return ::GetPrivateProfileIntW(L"M2V", L"audio", 0, pszIniFile) != 0;
}

//	開く処理を諦めるまでの時間 (秒)。
//	  [M2V]
//	  open_timeout=10
//
//	0 を書くと待ち続ける (従来の挙動)。
int GetOpenTimeoutMs()
{
	LPCWSTR pszIniFile = TSMemoryGetIniFileName();
	int Seconds = 10;

	if (pszIniFile != nullptr && pszIniFile[0] != L'\0') {
		Seconds = static_cast<int>(
			::GetPrivateProfileIntW(L"M2V", L"open_timeout", 10, pszIniFile));
	}

	if (Seconds <= 0)
		return -1;			// INFINITE
	if (Seconds > 600)
		Seconds = 600;
	return Seconds * 1000;
}

//	func_open() の度に読み直さないよう、登録時に一度だけ決める
bool g_fAudioEnabled = false;
int g_OpenTimeoutMs = 10000;

//	上下を入れ替える
void FlipRows(BYTE *pBuffer, int Height, int Pitch)
{
	std::vector<BYTE> Line(Pitch);

	for (int y = 0; y < Height / 2; y++) {
		BYTE *pTop = pBuffer + static_cast<size_t>(y) * Pitch;
		BYTE *pBottom = pBuffer + static_cast<size_t>(Height - 1 - y) * Pitch;
		::CopyMemory(Line.data(), pTop, Pitch);
		::CopyMemory(pTop, pBottom, Pitch);
		::CopyMemory(pBottom, Line.data(), Pitch);
	}
}

//----------------------------------------------------------------------------
//	映像デコーダのラッパ
//----------------------------------------------------------------------------
class M2V {
	MPEG_VIDEO *m_pVideo;

	void ReadFields(int frame, OUT_BUFFER_ELEMENT *&top, OUT_BUFFER_ELEMENT *&bottom) const
	{
		top = bottom = read_frame(m_pVideo, frame);

		if (top == nullptr)
			return;

		switch (m_pVideo->config.field_mode) {
		case 0:	// keep original frame
			break;

		case 1:	// top field first
			if (!top->prm.top_field_first && !top->prm.repeat_first_field)
				bottom = read_frame(m_pVideo, frame + 1);
			if (bottom == nullptr)
				bottom = top;
			break;

		case 2:	// bottom field first
			if (bottom->prm.top_field_first && !bottom->prm.repeat_first_field)
				top = read_frame(m_pVideo, frame + 1);
			if (top == nullptr)
				top = bottom;
			break;
		}
	}

public:
	explicit M2V(const char *file) : m_pVideo(open_mpeg_video(const_cast<char *>(file))) {}

	~M2V()
	{
		if (m_pVideo != nullptr) {
			close_mpeg_video(m_pVideo);
			m_pVideo = nullptr;
		}
	}

	M2V(const M2V &) = delete;
	M2V &operator=(const M2V &) = delete;

	MPEG_VIDEO *operator->() const { return m_pVideo; }
	operator MPEG_VIDEO *() const { return m_pVideo; }

	bool ReadYUY2(int frame, void *buf, int pitch) const
	{
		OUT_BUFFER_ELEMENT *fields[2];

		::EnterCriticalSection(&m_pVideo->lock);

		ReadFields(frame, fields[0], fields[1]);

		if (fields[0] == nullptr) {
			::LeaveCriticalSection(&m_pVideo->lock);
			return false;
		}

		m_pVideo->bgr_prm.prm.out_step = pitch;

		//	フィールドの実高さが想定より小さい場合に合わせる (元実装と同じ)
		const int SaveHeight = m_pVideo->bgr_prm.prm.height;
		for (int i = 0; i < 2; i++) {
			if (m_pVideo->bgr_prm.prm.height > fields[i]->data.height)
				m_pVideo->bgr_prm.prm.height = fields[i]->data.height;
		}

		m_pVideo->to_yuy2(&fields[0]->data, &fields[1]->data,
						  static_cast<unsigned char *>(buf), &m_pVideo->bgr_prm.prm);
		m_pVideo->yuy2_cc(static_cast<unsigned char *>(buf), pitch,
						  m_pVideo->bgr_prm.prm.height, &m_pVideo->ycc_prm);

		m_pVideo->bgr_prm.prm.height = SaveHeight;

		//	x64 ビルドでは MMX を使わないので emms は不要

		::LeaveCriticalSection(&m_pVideo->lock);
		return true;
	}
};

//	音声を開いた結果をログに出す。
//
//	`[M2V] audio=1` にしたのに TVTest 側の `[Settings] Audio` が 0 のままだと
//	音声 PID がそもそも溜め込まれていない。この時に何も出ないと、
//	利用者は「なぜ音が出ないのか」が判らない。
void LogAudioResult(const CTvtvAudio &Audio)
{
	WCHAR szMessage[256];

	switch (Audio.GetResult()) {
	case CTvtvAudio::Result::Ok:
		::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage),
						   L"TSMemory: 音声を取り込みました "
						   L"(%d ch / %lu Hz / %.2f 秒 / 映像との差 %+.3f 秒)",
						   Audio.GetFormat()->nChannels,
						   Audio.GetFormat()->nSamplesPerSec,
						   Audio.GetFormat()->nSamplesPerSec > 0
							   ? static_cast<double>(Audio.GetSampleCount())
								   / Audio.GetFormat()->nSamplesPerSec
							   : 0.0,
						   Audio.GetAudioLeadSeconds());
		TSMemoryLog(szMessage);
		break;

	case CTvtvAudio::Result::NoStream:
		//	いちばん多いのは設定の片方だけを入れた場合
		TSMemoryLogWarn(
			L"TSMemory: 音声が見つかりません。"
			L"TVTest 側の TSMemory.ini で [Settings] Audio=1 になっているか"
			L"確認してください (両方を 1 にする必要があります)");
		break;

	case CTvtvAudio::Result::UnsupportedFormat:
		//	設定は正しい。形式が違うだけなので、設定を疑わせない
		::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage),
						   L"TSMemory: 音声はありますが対応していない形式です "
						   L"(stream_type 0x%02X)。"
						   L"対応しているのは AAC (ADTS, stream_type 0x0F) だけです",
						   Audio.GetUnsupportedAudioType());
		TSMemoryLogWarn(szMessage);
		break;

	default:
		::StringCchPrintfW(szMessage, ARRAYSIZE(szMessage),
						   L"TSMemory: 音声を取り込めませんでした : %s",
						   Audio.GetLastError());
		TSMemoryLogWarn(szMessage);
		break;
	}
}

//----------------------------------------------------------------------------
//	入力ファイルハンドルの実体
//----------------------------------------------------------------------------
class CTvtvFile {
	M2V m_Video;
	CTvtvAudio m_Audio;
	BITMAPINFOHEADER m_bih;
	WAVEFORMATEX m_wfex;
	int m_Pitch;
	bool m_fFlip;

public:
	explicit CTvtvFile(const char *file)
		: m_Video(file)
		, m_bih()
		, m_wfex()
		, m_Pitch(0)
		, m_fFlip(GetFlipSetting())
	{
		//	音声は設定で有効にした時だけ扱う。
		//	無効なら CTvtvAudio は開かず、audio/ 側は一切動かない
		if (g_fAudioEnabled) {
			m_Audio.Open(file);
			LogAudioResult(m_Audio);
		}

		if (m_Video != nullptr) {
			m_bih.biSize = sizeof(BITMAPINFOHEADER);
			m_bih.biWidth = m_Video->width;
			m_bih.biHeight = m_Video->height;
			m_bih.biPlanes = 1;
			m_bih.biBitCount = 16;
			m_bih.biCompression = MAKEFOURCC('Y', 'U', 'Y', '2');

			m_Pitch = ((m_bih.biBitCount * m_bih.biWidth + 31) & ~31) >> 3;
			m_bih.biSizeImage = m_Pitch * m_bih.biHeight;
		}

		if (m_Audio.IsValid())
			m_wfex = *m_Audio.GetFormat();
	}

	bool IsValid() const { return m_Video != nullptr || m_Audio.IsValid(); }

	bool GetInfo(INPUT_INFO *iip) const
	{
		iip->flag = 0;

		if (m_Video != nullptr) {
			iip->flag |= INPUT_INFO::FLAG_VIDEO;
			iip->rate = m_Video->rate;
			iip->scale = m_Video->scale;
			iip->n = static_cast<int>(m_Video->total);
			iip->format = const_cast<BITMAPINFOHEADER *>(&m_bih);
			iip->format_size = sizeof(BITMAPINFOHEADER);
		}

		if (m_Audio.IsValid()) {
			iip->flag |= INPUT_INFO::FLAG_AUDIO;
			iip->audio_n = static_cast<int>(m_Audio.GetSampleCount());
			iip->audio_format = const_cast<WAVEFORMATEX *>(&m_wfex);
			iip->audio_format_size = sizeof(WAVEFORMATEX);
		}

		return iip->flag != 0;
	}

	int ReadVideo(int frame, void *buf) const
	{
		if (m_Video != nullptr && m_Video.ReadYUY2(frame, buf, m_Pitch)) {
			if (m_fFlip)
				FlipRows(static_cast<BYTE *>(buf), m_bih.biHeight, m_Pitch);
			return m_bih.biSizeImage;
		}
		return 0;
	}

	int ReadAudio(int start, int length, void *buf) const
	{
		if (m_Audio.IsValid())
			return m_Audio.Read(start, length, buf);
		return 0;
	}
};

//----------------------------------------------------------------------------
//	開いているハンドルの管理
//
//	m2v はファイルを開くとデコード用のスレッドを起動する (mpeg_video.c の
//	decode_thread)。AviUtl2 がパッケージの入れ替え等でプラグインを
//	アンロードする際、ハンドルが開いたままだとスレッドのコードが
//	アンマップされて落ちる (TSMemory-TVTestSrc.aux2_unloaded として記録される)。
//	その為、アンロード前に開きっぱなしのハンドルを閉じられるようにしておく。
//----------------------------------------------------------------------------
//
//	CRITICAL_SECTION は一度作ったら破棄しない。
//	後始末済みかどうかは g_fShutdown (ロックの中で読み書きする) で示す。
//	破棄してしまうと「g_fHandleLockReady の判定を通った直後に破棄され、
//	破棄済みの CS に EnterCriticalSection() する」競合が残る為。
CRITICAL_SECTION g_HandleLock;
bool g_fHandleLockReady = false;
bool g_fShutdown = false;
std::vector<CTvtvFile *> g_OpenHandles;

//----------------------------------------------------------------------------
//	開く処理の時間切れ
//
//	m2v は壊れた MPEG-2 を渡されると、GOP リストの作成が現実的な時間で
//	終わらなくなる事がある。無限ループではなく、TS -> PES -> ES の
//	組み立てを延々とやり直している (実測: extract_standard_pes_header /
//	find_next_001 に時間が集中する)。
//	そのまま func_open() の中で待つと AviUtl2 が固まる。
//
//	別スレッドで開き、時間内に終わらなければ「開けなかった」事にする。
//	遅れて出来上がった物は、そのスレッド自身が片付ける。
//----------------------------------------------------------------------------
struct OpenRequest {
	volatile LONG RefCount;			// 呼び出し側とスレッドで 1 つずつ持つ
	char AnsiPath[MAX_PATH * 2];
	CTvtvFile *pFile;
};

void ReleaseOpenRequest(OpenRequest *p)
{
	if (::InterlockedDecrement(&p->RefCount) != 0)
		return;

	//	最後の 1 つ = 呼び出し側は既に諦めている。作った物ごと捨てる
	delete p->pFile;
	delete p;
}

DWORD WINAPI OpenThreadProc(LPVOID pParameter)
{
	OpenRequest *p = static_cast<OpenRequest *>(pParameter);

	CTvtvFile *pFile = new (std::nothrow) CTvtvFile(p->AnsiPath);
	if (pFile != nullptr && !pFile->IsValid()) {
		delete pFile;
		pFile = nullptr;
	}
	p->pFile = pFile;

	ReleaseOpenRequest(p);
	return 0;
}

CTvtvFile *OpenWithTimeout(const char *pszAnsiPath)
{
	OpenRequest *p = new (std::nothrow) OpenRequest;
	if (p == nullptr)
		return nullptr;

	p->RefCount = 2;
	p->pFile = nullptr;
	::lstrcpynA(p->AnsiPath, pszAnsiPath, sizeof(p->AnsiPath));

	HANDLE hThread = ::CreateThread(nullptr, 0, OpenThreadProc, p, 0, nullptr);
	if (hThread == nullptr) {
		delete p;
		return nullptr;
	}

	const bool fFinished =
		::WaitForSingleObject(hThread, static_cast<DWORD>(g_OpenTimeoutMs)) == WAIT_OBJECT_0;
	::CloseHandle(hThread);

	CTvtvFile *pFile = fFinished ? p->pFile : nullptr;
	if (fFinished)
		p->pFile = nullptr;		// こちらが引き取る

	ReleaseOpenRequest(p);
	return pFile;
}

//----------------------------------------------------------------------------
//	INPUT_PLUGIN_TABLE のコールバック
//----------------------------------------------------------------------------
INPUT_HANDLE func_open(LPCWSTR file)
{
	char AnsiPath[MAX_PATH * 2];

	if (file == nullptr || !ToAnsiPath(file, AnsiPath, sizeof(AnsiPath)))
		return nullptr;

	//	後始末済みなら開かない。
	//
	//	AviUtl2 は UninitializePlugin() の後もこちらを呼べる状態のままなので
	//	(モジュールを固定している為)、ここで開くと管理リストに載らないまま
	//	デコードスレッドだけが残ってしまう。
	if (!g_fHandleLockReady)
		return nullptr;

	::EnterCriticalSection(&g_HandleLock);
	const bool fShutdown = g_fShutdown;
	::LeaveCriticalSection(&g_HandleLock);
	if (fShutdown)
		return nullptr;

	CTvtvFile *pFile = OpenWithTimeout(AnsiPath);
	if (pFile == nullptr)
		return nullptr;

	::EnterCriticalSection(&g_HandleLock);
	if (g_fShutdown) {
		//	開いている間に後始末が走った場合はここで閉じる
		::LeaveCriticalSection(&g_HandleLock);
		delete pFile;
		return nullptr;
	}
	g_OpenHandles.push_back(pFile);
	::LeaveCriticalSection(&g_HandleLock);

	return pFile;
}

bool func_close(INPUT_HANDLE ih)
{
	CTvtvFile *pFile = static_cast<CTvtvFile *>(ih);

	if (pFile == nullptr)
		return true;

	//	管理リストに載っている物だけを破棄する。
	//
	//	AviUtl2 は UninitializePlugin() の後にも func_close() を呼んでくる。
	//	そこで既に破棄済みの物をもう一度 delete すると二重解放になり、
	//	終了時にヒープ破損 (0xC0000374) で落ちる。
	//	リストから外せた時だけ実際に破棄する事で、どちらの順序で来ても
	//	一度しか解放しないようにする。
	bool fOwned = false;

	if (!g_fHandleLockReady)
		return true;

	::EnterCriticalSection(&g_HandleLock);
	for (auto it = g_OpenHandles.begin(); it != g_OpenHandles.end(); ++it) {
		if (*it == pFile) {
			g_OpenHandles.erase(it);
			fOwned = true;
			break;
		}
	}
	::LeaveCriticalSection(&g_HandleLock);

	if (!fOwned)
		return true;

	delete pFile;
	return true;
}

bool func_info_get(INPUT_HANDLE ih, INPUT_INFO *iip)
{
	if (ih == nullptr || iip == nullptr)
		return false;
	return static_cast<CTvtvFile *>(ih)->GetInfo(iip);
}

int func_read_video(INPUT_HANDLE ih, int frame, void *buf)
{
	if (ih == nullptr || buf == nullptr)
		return 0;
	return static_cast<CTvtvFile *>(ih)->ReadVideo(frame, buf);
}

int func_read_audio(INPUT_HANDLE ih, int start, int length, void *buf)
{
	if (ih == nullptr || buf == nullptr)
		return 0;
	return static_cast<CTvtvFile *>(ih)->ReadAudio(start, length, buf);
}

#define TVTV_FILE_FILTER	L"TVTest Video File (*.tvtv)\0*.tvtv\0"

INPUT_PLUGIN_TABLE g_InputPluginTable = {
	INPUT_PLUGIN_TABLE::FLAG_VIDEO | INPUT_PLUGIN_TABLE::FLAG_AUDIO,
	L"TVTest Video Reader",
	TVTV_FILE_FILTER,
	L"TVTest Video Reader (m2v 0.7.14 base) for AviUtl ExEdit2",
	func_open,
	func_close,
	func_info_get,
	func_read_video,
	func_read_audio,
	nullptr,	// func_config
	nullptr,	// func_set_track
	nullptr,	// func_time_to_frame
};

}	// namespace

INPUT_PLUGIN_TABLE *TSMemoryGetInputPluginTable()
{
	return &g_InputPluginTable;
}

void TSMemoryInputInitialize()
{
	//	音声を扱うかは登録時に決める。
	//	扱わない場合は INPUT_PLUGIN_TABLE からも FLAG_AUDIO を落とす
	//	(申告だけして中身が無いと、AviUtl2 側に空の音声が出来てしまう)。
	g_fAudioEnabled = GetAudioSetting();
	g_OpenTimeoutMs = GetOpenTimeoutMs();
	if (g_fAudioEnabled)
		g_InputPluginTable.flag |= INPUT_PLUGIN_TABLE::FLAG_AUDIO;
	else
		g_InputPluginTable.flag &= ~static_cast<int>(INPUT_PLUGIN_TABLE::FLAG_AUDIO);

	//	Uninitialize() では破棄しないので、二度目は作り直さない
	if (!g_fHandleLockReady) {
		::InitializeCriticalSection(&g_HandleLock);
		g_fHandleLockReady = true;
	}

	::EnterCriticalSection(&g_HandleLock);
	g_fShutdown = false;
	::LeaveCriticalSection(&g_HandleLock);
}

int TSMemoryInputUninitialize()
{
	if (!g_fHandleLockReady)
		return 0;

	//	開きっぱなしのハンドルを閉じてデコードスレッドを止める。
	//	これをしないとアンロード後にスレッドが動き続けて落ちる。
	std::vector<CTvtvFile *> Handles;

	::EnterCriticalSection(&g_HandleLock);
	g_fShutdown = true;
	Handles.swap(g_OpenHandles);
	::LeaveCriticalSection(&g_HandleLock);

	for (CTvtvFile *pFile : Handles)
		delete pFile;

	//	g_HandleLock は破棄しない (上の宣言部の説明を参照)。
	//	g_fHandleLockReady もそのままにして、以降の func_open() /
	//	func_close() が安全に空振り出来る状態を保つ。
	return static_cast<int>(Handles.size());
}
