//----------------------------------------------------------------------------
//	音声用の TS デマルチプレクサ
//
//	共有メモリ上の MPEG-2 TS から AAC (ADTS) のフレーム列を取り出す。
//
//	**映像側 (m2v) とは完全に独立している。** m2v の音声デコーダは
//	Program Stream 専用・実ファイル前提・Layer II 専用で流用出来ない
//	(docs/audio-support.md の 2-2 を参照)。
//
//	A/V 同期の為に映像の開始 PTS も一緒に拾う。m2v は TS から ES へ
//	落とす際に PTS を捨てており、映像フレーム 0 の時刻を m2v から
//	取得する手段が無い為 (同 6-2)。
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

//	ADTS フレーム 1 つ
struct TSAudioFrame {
	uint32_t Offset;		// ES の先頭からのバイト位置
	uint32_t Length;		// ADTS ヘッダを含む長さ
	int64_t  StartSample;	// このフレームの先頭サンプル位置
	int64_t  Pts;			// 90kHz。判らない場合は -1
};

class CTSAudioSource
{
public:
	CTSAudioSource();
	~CTSAudioSource();

	CTSAudioSource(const CTSAudioSource &) = delete;
	CTSAudioSource &operator=(const CTSAudioSource &) = delete;

	//	共有メモリ名 (= .tvtv のファイル名部分) を渡して開く。
	//	m2v の open_shared_memory() と同じ規約で読む。
	bool Open(const char *pszSharedName);

	bool IsValid() const { return !m_Frames.empty(); }

	int GetSampleRate() const { return m_SampleRate; }
	int GetChannels() const { return m_Channels; }
	int64_t GetTotalSamples() const { return m_TotalSamples; }

	int GetFrameCount() const { return static_cast<int>(m_Frames.size()); }
	const TSAudioFrame &GetFrame(int Index) const { return m_Frames[Index]; }

	//	ES の生データ (ADTS フレームが連なっている)
	const BYTE *GetData() const { return m_Es.empty() ? nullptr : m_Es.data(); }
	size_t GetDataSize() const { return m_Es.size(); }

	//	A/V 同期用。判らない場合は -1
	int64_t GetVideoStartPts() const { return m_VideoStartPts; }
	int64_t GetAudioStartPts() const { return m_Frames.empty() ? -1 : m_Frames[0].Pts; }

	//	映像の先頭に対して音声が何秒ずれているか。
	//	  > 0 … 音声の方が先に始まっている (先頭を捨てる)
	//	  < 0 … 映像の方が先に始まっている (無音を詰める)
	//	どちらかの PTS が不明なら 0
	double GetAudioLeadSeconds() const;

	//	開始サンプル位置からフレーム番号を引く (見つからなければ -1)
	int FindFrameBySample(int64_t Sample) const;

	//	PMT に在ったが復号出来なかった音声の stream_type (無ければ 0)。
	//	  0x11 … MPEG-4 AAC (LATM)   0x1C … MPEG-4 raw audio
	//	いずれも新 4K8K 衛星放送で使われる。ADTS 専用の索引作りでは
	//	見つけられない。「音声が無い」と取り違えない為に持つ
	BYTE GetUnsupportedAudioType() const { return m_UnsupportedAudioType; }

private:
	BYTE m_UnsupportedAudioType = 0;
	std::vector<BYTE> m_Es;			// 音声 ES (ADTS の連なり)
	std::vector<TSAudioFrame> m_Frames;

	int m_SampleRate;
	int m_Channels;
	int64_t m_TotalSamples;
	int64_t m_VideoStartPts;
};
