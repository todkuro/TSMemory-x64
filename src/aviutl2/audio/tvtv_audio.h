//----------------------------------------------------------------------------
//	*.tvtv の音声
//
//	**入力プラグイン (src/aviutl2/input_tvtv.cpp) から見える唯一の窓口。**
//	音声対応は src/aviutl2/audio/ に閉じており、この 1 つのヘッダだけを
//	include すれば足りる。無効時 (`[M2V] audio=0`) はこのクラスを
//	作らないので、音声側のコードは一切動かない。
//
//	中でやっている事:
//	  ts_audio    … 共有メモリの TS から AAC (ADTS) を取り出す
//	  aac_decoder … Media Foundation で PCM に直す
//	  ここ        … 映像との時間差を詰めて、任意位置の読み出しに応える
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <mmreg.h>

#include <cstdint>
#include <vector>

class CTvtvAudio
{
public:
	CTvtvAudio();
	~CTvtvAudio();

	CTvtvAudio(const CTvtvAudio &) = delete;
	CTvtvAudio &operator=(const CTvtvAudio &) = delete;

	//	m2v に渡すのと同じパスを渡す。
	//	共有メモリ名として使うのはファイル名部分だけ
	//	(m2v の open_shared_memory() と同じ)。
	bool Open(const char *pszPath);

	bool IsValid() const { return !m_Pcm.empty(); }

	const WAVEFORMATEX *GetFormat() const { return &m_Format; }

	//	1 チャンネルあたりのサンプル数
	int64_t GetSampleCount() const;

	//	Start から Length サンプル読む。返り値は実際に書いたサンプル数。
	//	範囲外は無音で埋める (AviUtl2 は端を要求してくる)。
	int Read(int64_t Start, int Length, void *pBuffer) const;

	//	開けなかった理由。呼び出し側がログの文面を選ぶのに使う
	enum class Result {
		NotTried,		// audio=0 なので開いていない
		Ok,
		NoStream,		// 音声のストリームが無い (TVTest 側が落としている等)
		UnsupportedFormat,	// 音声は在るが復号出来ない形式 (4K8K の LATM 等)
		DecodeFailed,	// Media Foundation で復号出来なかった
		Empty,			// 時間差を詰めた結果、残らなかった
	};

	Result GetResult() const { return m_Result; }

	//	UnsupportedFormat の時の stream_type (0x11 / 0x1C)
	BYTE GetUnsupportedAudioType() const { return m_UnsupportedAudioType; }

	//	診断用
	double GetAudioLeadSeconds() const { return m_LeadSeconds; }
	LPCWSTR GetLastError() const { return m_szError; }

private:
	std::vector<int16_t> m_Pcm;		// 映像の先頭に合わせた後の PCM
	WAVEFORMATEX m_Format;
	double m_LeadSeconds;
	Result m_Result;
	BYTE m_UnsupportedAudioType = 0;
	WCHAR m_szError[128];
};
