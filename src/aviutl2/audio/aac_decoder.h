//----------------------------------------------------------------------------
//	AAC (ADTS) を PCM に直す
//
//	Windows 標準の Media Foundation の AAC デコーダ (MFT) を使う。
//	画像の保存で WIC を使ったのと同じ考え方で、追加の DLL は要らない。
//
//	**開いた時に全フレームを復号して PCM を持つ。**
//	都度復号にすると、任意位置を要求してくる func_read_audio() の度に
//	MFT を flush して数フレーム手前から復号し直す必要があり、
//	そこが遅いと操作感に響く。取り込みは MemorySize (既定 10MB =
//	およそ 10 秒) の範囲なので、48kHz ステレオでも数 MB に収まる。
//	詳細は docs/audio-support.md を参照。
//----------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

class CAacDecoder
{
public:
	CAacDecoder();
	~CAacDecoder();

	CAacDecoder(const CAacDecoder &) = delete;
	CAacDecoder &operator=(const CAacDecoder &) = delete;

	//	ADTS が連なった塊をまとめて復号する。
	//	pszError には失敗した段階が入る (診断用)。
	bool DecodeAll(const BYTE *pData, size_t Size, int SampleRate, int Channels,
				   size_t MaxSamples);

	const std::vector<int16_t> &GetPcm() const { return m_Pcm; }

	//	1 チャンネルあたりのサンプル数
	int64_t GetSampleCount() const
	{
		return m_Channels > 0
			? static_cast<int64_t>(m_Pcm.size()) / m_Channels : 0;
	}

	int GetChannels() const { return m_Channels; }
	LPCWSTR GetLastError() const { return m_szError; }

private:
	std::vector<int16_t> m_Pcm;
	int m_Channels;
	WCHAR m_szError[128];
};
