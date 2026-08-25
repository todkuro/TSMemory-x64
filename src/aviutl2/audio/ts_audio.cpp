//----------------------------------------------------------------------------
//	音声用の TS デマルチプレクサ (ts_audio.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>

#include "ts_audio.h"

namespace {

const int TS_PACKET_SIZE = 188;

//	AAC-LC は 1 フレーム 1024 サンプル
const int AAC_SAMPLES_PER_FRAME = 1024;

//	PTS は 33bit で折り返す
const int64_t PTS_WRAP = 1LL << 33;

const int ADTS_SAMPLE_RATE[16] = {
	96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
	16000, 12000, 11025,  8000,  7350,     0,     0,     0,
};

//----------------------------------------------------------------------------
//	共有メモリの読み出し
//
//	m2v の shared_memory.c と同じ規約:
//	  先頭に DWORD が 4 つ (Size / Used / Pos / Reserved) 並び、
//	  その後ろが Size バイトのリングバッファ。
//	  Pos から Used バイトを繋げたものが中身になる。
//	排他は "<名前>.mutex"。
//----------------------------------------------------------------------------
bool ReadSharedMemory(const char *pszName, std::vector<BYTE> *pOut)
{
	char szMutexName[MAX_PATH];
	std::snprintf(szMutexName, sizeof(szMutexName), "%s.mutex", pszName);

	HANDLE hMutex = ::OpenMutexA(SYNCHRONIZE, FALSE, szMutexName);
	if (hMutex == nullptr)
		return false;

	if (::WaitForSingleObject(hMutex, 3000) != WAIT_OBJECT_0) {
		::CloseHandle(hMutex);
		return false;
	}

	bool fOK = false;
	HANDLE hMap = ::OpenFileMappingA(FILE_MAP_READ, FALSE, pszName);
	if (hMap != nullptr) {
		const BYTE *pView = static_cast<const BYTE *>(
			::MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
		if (pView != nullptr) {
			const DWORD *pInfo = reinterpret_cast<const DWORD *>(pView);
			const DWORD Size = pInfo[0];
			const DWORD Used = pInfo[1];
			const DWORD Pos = pInfo[2];
			const BYTE *pData = pView + sizeof(DWORD) * 4;

			if (Used > 0 && Size > 0 && Pos < Size && Used <= Size) {
				pOut->resize(Used);
				const DWORD First = Used < (Size - Pos) ? Used : (Size - Pos);
				::CopyMemory(pOut->data(), pData + Pos, First);
				if (First < Used)
					::CopyMemory(pOut->data() + First, pData, Used - First);
				fOK = true;
			}
			::UnmapViewOfFile(pView);
		}
		::CloseHandle(hMap);
	}

	::ReleaseMutex(hMutex);
	::CloseHandle(hMutex);
	return fOK;
}

//----------------------------------------------------------------------------
//	TS パケット
//----------------------------------------------------------------------------
struct TsPacket {
	WORD Pid;
	bool fStart;			// payload_unit_start_indicator
	const BYTE *pPayload;
	int PayloadSize;
};

bool ParseTsPacket(const BYTE *p, TsPacket *pOut)
{
	if (p[0] != 0x47)
		return false;
	if ((p[1] & 0x80) != 0)		// transport_error_indicator
		return false;

	pOut->Pid = static_cast<WORD>(((p[1] & 0x1F) << 8) | p[2]);
	pOut->fStart = (p[1] & 0x40) != 0;

	const int Control = (p[3] >> 4) & 0x03;
	if ((Control & 0x01) == 0)	// ペイロード無し
		return false;

	int Offset = 4;
	if ((Control & 0x02) != 0) {
		const int Length = p[4];
		//	アダプテーションフィールド長は 183 まで
		if (Length > 183)
			return false;
		Offset = 5 + Length;
		if (Offset >= TS_PACKET_SIZE)
			return false;
	}

	pOut->pPayload = p + Offset;
	pOut->PayloadSize = TS_PACKET_SIZE - Offset;
	return pOut->PayloadSize > 0;
}

//----------------------------------------------------------------------------
//	セクションの再組み立て (PAT / PMT は 1 パケットに収まらない事がある)
//----------------------------------------------------------------------------
class CSectionAssembler
{
	std::vector<BYTE> m_Buffer;
	int m_Expected;

public:
	CSectionAssembler() : m_Expected(0) {}

	//	完成したセクションを返す (未完成なら nullptr)
	const std::vector<BYTE> *Feed(const TsPacket &Packet)
	{
		const BYTE *p = Packet.pPayload;
		int Size = Packet.PayloadSize;

		if (Packet.fStart) {
			//	pointer_field の分を飛ばす
			const int Pointer = p[0];
			if (1 + Pointer >= Size)
				return nullptr;
			p += 1 + Pointer;
			Size -= 1 + Pointer;

			m_Buffer.clear();
			m_Expected = 0;
		} else if (m_Buffer.empty()) {
			return nullptr;		// 途中から拾っても組み立てられない
		}

		m_Buffer.insert(m_Buffer.end(), p, p + Size);

		if (m_Expected == 0) {
			if (m_Buffer.size() < 3)
				return nullptr;
			//	section_length は先頭 3 バイトの後ろに続く
			m_Expected = 3 + (((m_Buffer[1] & 0x0F) << 8) | m_Buffer[2]);
			//	異常な長さは相手にしない
			if (m_Expected < 8 || m_Expected > 4096) {
				m_Buffer.clear();
				m_Expected = 0;
				return nullptr;
			}
		}

		if (static_cast<int>(m_Buffer.size()) < m_Expected)
			return nullptr;

		m_Buffer.resize(m_Expected);
		return &m_Buffer;
	}
};

//----------------------------------------------------------------------------
//	PES
//----------------------------------------------------------------------------
//	PES ヘッダから PTS を取り出す。無ければ -1
int64_t ExtractPts(const BYTE *p, int Size)
{
	if (Size < 14)
		return -1;
	if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01)
		return -1;
	if ((p[6] & 0xC0) != 0x80)		// MPEG-2 PES では無い
		return -1;
	if ((p[7] & 0x80) == 0)			// PTS 無し
		return -1;

	const BYTE *q = p + 9;
	return ((static_cast<int64_t>(q[0] & 0x0E) << 29)
			| (static_cast<int64_t>(q[1]) << 22)
			| (static_cast<int64_t>(q[2] & 0xFE) << 14)
			| (static_cast<int64_t>(q[3]) << 7)
			| (static_cast<int64_t>(q[4]) >> 1));
}

//	PES ペイロードの開始位置。判らなければ -1
int PesPayloadOffset(const BYTE *p, int Size)
{
	if (Size < 9)
		return -1;
	if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01)
		return -1;

	const int Offset = 9 + p[8];
	return Offset <= Size ? Offset : -1;
}

//----------------------------------------------------------------------------
//	ADTS
//----------------------------------------------------------------------------
struct AdtsHeader {
	int Length;
	int SampleRate;
	int Channels;
};

bool ParseAdts(const BYTE *p, size_t Rest, AdtsHeader *pOut)
{
	if (Rest < 7)
		return false;
	if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)		// 同期語 + Layer=00
		return false;

	const int RateIndex = (p[2] >> 2) & 0x0F;
	const int ChannelConfig = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);

	pOut->SampleRate = ADTS_SAMPLE_RATE[RateIndex];
	pOut->Channels = ChannelConfig;
	pOut->Length = ((p[3] & 0x03) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x07);

	if (pOut->SampleRate == 0 || pOut->Channels < 1 || pOut->Channels > 8)
		return false;
	//	長さ 0 のフレームを許すと走査が進まなくなる
	if (pOut->Length < 7 || static_cast<size_t>(pOut->Length) > Rest)
		return false;

	return true;
}

//----------------------------------------------------------------------------
//	PES を繋いで ES にする
//
//	音声と映像で同じ事をするので 1 つにまとめてある。
//	ES のどのバイト位置がどの PES から来たかを控えておき、
//	後から PTS を引けるようにする。
//----------------------------------------------------------------------------
class CPesAssembler
{
	std::vector<BYTE> m_Pes;
	int64_t m_Pts;

public:
	std::vector<BYTE> Es;
	std::map<uint32_t, int64_t> PtsAt;

	CPesAssembler() : m_Pts(-1) {}

	//	1 つの PES が完成したら Es に足す。
	//	返り値は今回足した分の開始位置 (何も足していなければ Es.size())
	size_t Flush()
	{
		const size_t Before = Es.size();

		if (m_Pes.empty())
			return Before;

		const int Offset = PesPayloadOffset(m_Pes.data(), static_cast<int>(m_Pes.size()));
		if (Offset >= 0) {
			PtsAt[static_cast<uint32_t>(Es.size())] = m_Pts;
			Es.insert(Es.end(), m_Pes.begin() + Offset, m_Pes.end());
		}
		m_Pes.clear();
		m_Pts = -1;
		return Before;
	}

	//	そのストリームのパケットを渡す。PES が 1 つ完成したら true
	bool Feed(const TsPacket &Packet)
	{
		bool fFlushed = false;

		if (Packet.fStart) {
			if (!m_Pes.empty()) {
				Flush();
				fFlushed = true;
			}
			m_Pts = ExtractPts(Packet.pPayload, Packet.PayloadSize);
		} else if (m_Pes.empty()) {
			return false;		// 途中から拾っても組み立てられない
		}

		m_Pes.insert(m_Pes.end(), Packet.pPayload,
					 Packet.pPayload + Packet.PayloadSize);
		return fFlushed;
	}

	//	Es のバイト位置を含む PES の PTS。判らなければ -1
	int64_t PtsFor(size_t Offset) const
	{
		auto it = PtsAt.upper_bound(static_cast<uint32_t>(Offset));
		if (it == PtsAt.begin())
			return -1;
		--it;
		return it->second;
	}
};

//	同期語は音声データの中にも偶然現れる。**連鎖で**確かめる。
//	(単発の検出で拾うと「SSR / 8000Hz / 7ch」の様な値になる)
bool IsAdtsChain(const BYTE *p, size_t Rest, int Links)
{
	AdtsHeader Header;

	for (int i = 0; i < Links; i++) {
		if (!ParseAdts(p, Rest, &Header))
			return false;
		p += Header.Length;
		Rest -= Header.Length;
		if (Rest == 0)
			return i + 1 >= Links / 2;		// 末尾で尽きたなら良しとする
	}
	return true;
}

}	// namespace


//----------------------------------------------------------------------------
CTSAudioSource::CTSAudioSource()
	: m_SampleRate(0)
	, m_Channels(0)
	, m_TotalSamples(0)
	, m_VideoStartPts(-1)
{
}

CTSAudioSource::~CTSAudioSource()
{
}


double CTSAudioSource::GetAudioLeadSeconds() const
{
	const int64_t Video = GetVideoStartPts();
	const int64_t Audio = GetAudioStartPts();

	if (Video < 0 || Audio < 0)
		return 0.0;

	//	33bit で折り返す。近い方の差を採る
	int64_t Diff = Video - Audio;
	if (Diff > PTS_WRAP / 2)
		Diff -= PTS_WRAP;
	else if (Diff < -PTS_WRAP / 2)
		Diff += PTS_WRAP;

	return static_cast<double>(Diff) / 90000.0;
}


int CTSAudioSource::FindFrameBySample(int64_t Sample) const
{
	if (m_Frames.empty() || Sample < 0)
		return -1;

	//	等間隔なので割り算で当たりを付けてから前後を詰める
	int Index = static_cast<int>(Sample / AAC_SAMPLES_PER_FRAME);
	if (Index >= static_cast<int>(m_Frames.size()))
		Index = static_cast<int>(m_Frames.size()) - 1;

	while (Index > 0 && m_Frames[Index].StartSample > Sample)
		Index--;
	while (Index + 1 < static_cast<int>(m_Frames.size())
			&& m_Frames[Index + 1].StartSample <= Sample)
		Index++;

	return Index;
}


bool CTSAudioSource::Open(const char *pszSharedName)
{
	std::vector<BYTE> Ts;
	if (!ReadSharedMemory(pszSharedName, &Ts))
		return false;

	const size_t Packets = Ts.size() / TS_PACKET_SIZE;
	if (Packets == 0)
		return false;

	//	--- 1 回目: PAT / PMT から映像・音声の PID を求める ----------------
	WORD PmtPid = 0;
	WORD AudioPid = 0;
	WORD VideoPid = 0;
	{
		CSectionAssembler Pat, Pmt;

		for (size_t i = 0; i < Packets && (AudioPid == 0 || VideoPid == 0); i++) {
			TsPacket Packet;
			if (!ParseTsPacket(&Ts[i * TS_PACKET_SIZE], &Packet))
				continue;

			if (Packet.Pid == 0x0000 && PmtPid == 0) {
				const std::vector<BYTE> *pSection = Pat.Feed(Packet);
				if (pSection == nullptr || (*pSection)[0] != 0x00)
					continue;
				//	最初のプログラム (program_number != 0) を採る
				const std::vector<BYTE> &s = *pSection;
				for (size_t k = 8; k + 4 <= s.size() - 4; k += 4) {
					const WORD Program = static_cast<WORD>((s[k] << 8) | s[k + 1]);
					if (Program != 0) {
						PmtPid = static_cast<WORD>(((s[k + 2] & 0x1F) << 8) | s[k + 3]);
						break;
					}
				}
			} else if (PmtPid != 0 && Packet.Pid == PmtPid) {
				const std::vector<BYTE> *pSection = Pmt.Feed(Packet);
				if (pSection == nullptr || (*pSection)[0] != 0x02)
					continue;

				const std::vector<BYTE> &s = *pSection;
				if (s.size() < 12)
					continue;
				const size_t InfoLength = ((s[10] & 0x0F) << 8) | s[11];
				size_t k = 12 + InfoLength;

				while (k + 5 <= s.size() - 4) {
					const BYTE StreamType = s[k];
					const WORD Pid = static_cast<WORD>(((s[k + 1] & 0x1F) << 8) | s[k + 2]);
					const size_t EsLength = ((s[k + 3] & 0x0F) << 8) | s[k + 4];

					if (StreamType == 0x0F && AudioPid == 0)
						AudioPid = Pid;			// AAC
					if ((StreamType == 0x02 || StreamType == 0x01) && VideoPid == 0)
						VideoPid = Pid;			// MPEG-1/2 Video

					k += 5 + EsLength;
				}
			}
		}
	}

	if (AudioPid == 0)
		return false;

	//	--- 2 回目: 音声と映像を 1 回の走査で拾う --------------------------
	//
	//	音声の ES と、映像の開始 PTS の両方をここで得る。
	//	以前は別々に 2 周していたが、同じパケット列を舐めるだけなので
	//	1 周にまとめてある。
	//
	//	映像は「最初のシーケンスヘッダ (00 00 01 B3)」が見つかった時点で
	//	用済みなので、そこで収集をやめて ES も捨てる。
	CPesAssembler Audio;
	{
		CPesAssembler Video;
		size_t Searched = 0;			// 映像 ES のどこまで探したか
		bool fVideoDone = (VideoPid == 0);

		//	見つかったシーケンスヘッダの PTS を採る
		auto ScanVideo = [&]() {
			const std::vector<BYTE> &Es = Video.Es;
			for (size_t i = Searched; i + 3 < Es.size(); i++) {
				if (Es[i] == 0x00 && Es[i + 1] == 0x00
						&& Es[i + 2] == 0x01 && Es[i + 3] == 0xB3) {
					m_VideoStartPts = Video.PtsFor(i);
					fVideoDone = true;
					return;
				}
			}
			//	境界を跨ぐ 4 バイトを見落とさないよう少し戻す
			Searched = Es.size() > 3 ? Es.size() - 3 : 0;

			//	見つからないまま膨らみ続ける場合は諦める
			if (Es.size() > 4 * 1024 * 1024)
				fVideoDone = true;
		};

		for (size_t i = 0; i < Packets; i++) {
			TsPacket Packet;
			if (!ParseTsPacket(&Ts[i * TS_PACKET_SIZE], &Packet))
				continue;

			if (Packet.Pid == AudioPid) {
				Audio.Feed(Packet);
			} else if (!fVideoDone && Packet.Pid == VideoPid) {
				if (Video.Feed(Packet))
					ScanVideo();		// PES が 1 つ出来た時だけ探す
			}
		}

		Audio.Flush();

		if (!fVideoDone) {
			Video.Flush();
			ScanVideo();
		}
	}

	m_Es.swap(Audio.Es);
	if (m_Es.empty())
		return false;

	//	--- ADTS の連なりを探して索引を作る --------------------------------
	size_t Start = 0;
	{
		bool fFound = false;
		//	先頭が ADTS とは限らない。連鎖で確かめながら探す
		for (size_t i = 0; i + 7 < m_Es.size() && i < 4096; i++) {
			if (IsAdtsChain(&m_Es[i], m_Es.size() - i, 8)) {
				Start = i;
				fFound = true;
				break;
			}
		}
		if (!fFound)
			return false;
	}

	{
		size_t Offset = Start;
		int64_t Sample = 0;
		AdtsHeader Header;

		while (ParseAdts(&m_Es[Offset], m_Es.size() - Offset, &Header)) {
			if (m_SampleRate == 0) {
				m_SampleRate = Header.SampleRate;
				m_Channels = Header.Channels;
			}
			//	途中で構成が変わったら、そこで打ち切る
			if (Header.SampleRate != m_SampleRate || Header.Channels != m_Channels)
				break;

			TSAudioFrame Frame;
			Frame.Offset = static_cast<uint32_t>(Offset);
			Frame.Length = static_cast<uint32_t>(Header.Length);
			Frame.StartSample = Sample;
			Frame.Pts = -1;

			//	このフレームを含む PES の PTS を引く
			Frame.Pts = Audio.PtsFor(Offset);

			m_Frames.push_back(Frame);

			Sample += AAC_SAMPLES_PER_FRAME;
			Offset += Header.Length;
			if (Offset + 7 >= m_Es.size())
				break;
		}
		m_TotalSamples = Sample;
	}

	if (m_Frames.empty())
		return false;

	return true;
}
