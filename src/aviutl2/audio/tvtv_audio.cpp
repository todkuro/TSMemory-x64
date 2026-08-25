//----------------------------------------------------------------------------
//	*.tvtv の音声 (tvtv_audio.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>

#include <cstring>

#include "tvtv_audio.h"
#include "ts_audio.h"
#include "aac_decoder.h"

namespace {

//	復号の前後で端が 1 フレーム前後する事があるので、その分の余裕
const int64_t DECODE_MARGIN_SAMPLES = 4096;

//	最後の歯止め (int16 の要素数なので 2GB 相当)。
//
//	普段は下の「demux が数えたフレーム数」の方が遥かに小さく、ここには
//	当たらない。壊れた入力で ADTS の数え上げが暴れた時だけの保険
//	(ADTS は 1 フレーム最小 7 バイトで 1024 サンプルを名乗れる為、
//	 同期語だらけの ES を食わせると実際の内容より桁違いに大きくなる)。
const size_t ABSOLUTE_MAX_PCM = 1024u * 1024 * 1024;

}	// namespace


CTvtvAudio::CTvtvAudio()
	: m_LeadSeconds(0.0)
	, m_Result(Result::NotTried)
{
	::ZeroMemory(&m_Format, sizeof(m_Format));
	m_szError[0] = L'\0';
}

CTvtvAudio::~CTvtvAudio()
{
}


int64_t CTvtvAudio::GetSampleCount() const
{
	if (m_Format.nChannels == 0)
		return 0;
	return static_cast<int64_t>(m_Pcm.size()) / m_Format.nChannels;
}


bool CTvtvAudio::Open(const char *pszPath)
{
	m_Pcm.clear();
	::ZeroMemory(&m_Format, sizeof(m_Format));
	m_LeadSeconds = 0.0;
	m_szError[0] = L'\0';
	m_Result = Result::NotTried;

	if (pszPath == nullptr)
		return false;

	//	共有メモリ名はファイル名部分だけ
	const char *pszName = ::PathFindFileNameA(pszPath);

	CTSAudioSource Source;
	if (!Source.Open(pszName)) {
		m_Result = Result::NoStream;
		::lstrcpynW(m_szError, L"音声のストリームが見つかりません", 128);
		return false;
	}

	//	--- 復号 -----------------------------------------------------------
	//	ES の先頭はフレーム境界とは限らない。最初のフレームから渡す
	const TSAudioFrame &First = Source.GetFrame(0);
	const BYTE *pEs = Source.GetData() + First.Offset;
	const size_t EsSize = Source.GetDataSize() - First.Offset;

	//	必要量は demux が数えた ADTS のフレーム数から決まる。
	//	取り込んだ映像と同じ時間ぶんしか音声は無いので、これが本来の上限。
	//	固定値で頭打ちにすると、長い取り込みで音声だけ途中で切れる
	size_t MaxSamples = ABSOLUTE_MAX_PCM;
	{
		const int64_t Needed =
			(Source.GetTotalSamples() + DECODE_MARGIN_SAMPLES) * Source.GetChannels();
		if (Needed > 0 && static_cast<size_t>(Needed) < MaxSamples)
			MaxSamples = static_cast<size_t>(Needed);
	}

	CAacDecoder Decoder;
	if (!Decoder.DecodeAll(pEs, EsSize, Source.GetSampleRate(), Source.GetChannels(),
						   MaxSamples)) {
		m_Result = Result::DecodeFailed;
		::lstrcpynW(m_szError, Decoder.GetLastError(), 128);
		return false;
	}

	const int Channels = Decoder.GetChannels();
	if (Channels <= 0)
		return false;

	//	--- 映像との時間差を詰める -----------------------------------------
	//
	//	リングバッファは GOP の途中で切れるので、映像と音声で開始位置が違う。
	//	m2v は PTS を捨てている為、映像の開始 PTS は demux 側で
	//	「最初のシーケンスヘッダを含む PES の PTS」として拾ってある。
	m_LeadSeconds = Source.GetAudioLeadSeconds();

	const std::vector<int16_t> &Pcm = Decoder.GetPcm();
	const int64_t Shift =
		static_cast<int64_t>(m_LeadSeconds * Source.GetSampleRate() + 0.5);

	if (Shift > 0) {
		//	音声の方が先に始まっている -> 先頭を捨てる
		const size_t Drop = static_cast<size_t>(Shift) * Channels;
		if (Drop < Pcm.size())
			m_Pcm.assign(Pcm.begin() + static_cast<ptrdiff_t>(Drop), Pcm.end());
	} else if (Shift < 0) {
		//	映像の方が先に始まっている -> 先頭に無音を詰める
		const size_t Pad = static_cast<size_t>(-Shift) * Channels;
		if (Pad < ABSOLUTE_MAX_PCM) {
			m_Pcm.assign(Pad, 0);
			m_Pcm.insert(m_Pcm.end(), Pcm.begin(), Pcm.end());
		}
	} else {
		m_Pcm = Pcm;
	}

	if (m_Pcm.empty()) {
		m_Result = Result::Empty;
		::lstrcpynW(m_szError, L"時間差を詰めた結果、音声が残りませんでした", 128);
		return false;
	}

	//	--- 形式 -----------------------------------------------------------
	m_Format.wFormatTag = WAVE_FORMAT_PCM;
	m_Format.nChannels = static_cast<WORD>(Channels);
	m_Format.nSamplesPerSec = static_cast<DWORD>(Source.GetSampleRate());
	m_Format.wBitsPerSample = 16;
	m_Format.nBlockAlign = static_cast<WORD>(Channels * 2);
	m_Format.nAvgBytesPerSec = m_Format.nSamplesPerSec * m_Format.nBlockAlign;
	m_Format.cbSize = 0;

	m_Result = Result::Ok;
	return true;
}


int CTvtvAudio::Read(int64_t Start, int Length, void *pBuffer) const
{
	if (pBuffer == nullptr || Length <= 0 || m_Format.nChannels == 0)
		return 0;

	const int Channels = m_Format.nChannels;
	const int64_t Total = GetSampleCount();

	int16_t *pDst = static_cast<int16_t *>(pBuffer);

	//	AviUtl2 は端を跨いだ範囲も要求してくる。範囲外は無音で埋める
	for (int i = 0; i < Length; i++) {
		const int64_t Sample = Start + i;

		if (Sample < 0 || Sample >= Total) {
			for (int c = 0; c < Channels; c++)
				pDst[i * Channels + c] = 0;
		} else {
			const size_t Index = static_cast<size_t>(Sample) * Channels;
			for (int c = 0; c < Channels; c++)
				pDst[i * Channels + c] = m_Pcm[Index + c];
		}
	}

	return Length;
}
