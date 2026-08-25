//----------------------------------------------------------------------------
//	AAC (ADTS) -> PCM (aac_decoder.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>

#include <shlwapi.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <cstring>

#include "aac_decoder.h"

namespace {

//	COM の解放を書き忘れない為の最小限の入れ物
template <class T> class CRelease
{
	T *m_p;
public:
	CRelease() : m_p(nullptr) {}
	~CRelease() { if (m_p != nullptr) m_p->Release(); }

	CRelease(const CRelease &) = delete;
	CRelease &operator=(const CRelease &) = delete;

	T **operator&() { return &m_p; }
	T *operator->() const { return m_p; }
	operator T *() const { return m_p; }
	T *Get() const { return m_p; }
};

//	AAC の入力メディアタイプに要る HEAACWAVEINFO の後半
//	  wPayloadType = 1 (ADTS)
//	  wAudioProfileLevelIndication = 0x29 (AAC-LC / Level 2)
//	  wStructType = 0
//	AudioSpecificConfig は ADTS では要らない
const BYTE AAC_USER_DATA[] = {
	0x01, 0x00,		// wPayloadType = 1 (ADTS)
	0x00, 0x00,		// wAudioProfileLevelIndication = 未指定
	0x00, 0x00,		// wStructType = 0
};

//	CLSID_CMSAACDecMFT {32D186A7-935F-4EC4-8C61-E0B3D9BF9F2E}
//	llvm-mingw の import ライブラリには入っていないので自前で持つ
const CLSID CLSID_AacDecoderMFT = {
	0x32d186a7, 0x935f, 0x4ec4,
	{ 0x8c, 0x61, 0xe0, 0xb3, 0xd9, 0xbf, 0x9f, 0x2e }
};

}	// namespace


CAacDecoder::CAacDecoder()
	: m_Channels(0)
{
	m_szError[0] = L'\0';
}

CAacDecoder::~CAacDecoder()
{
}


bool CAacDecoder::DecodeAll(const BYTE *pData, size_t Size, int SampleRate, int Channels,
							size_t MaxSamples)
{
	m_Pcm.clear();
	m_Channels = 0;
	m_szError[0] = L'\0';

	if (pData == nullptr || Size == 0 || SampleRate <= 0 || Channels <= 0)
		return false;

	auto Fail = [this](LPCWSTR pszWhere, HRESULT hr) {
		::wnsprintfW(m_szError, 128, L"%s (hr=0x%08X)", pszWhere,
					 static_cast<unsigned int>(hr));
		return false;
	};

	//	MF は COM の上に乗っている。この呼び出しの間だけ用意する
	const HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool fCoInit = SUCCEEDED(hrCo);

	HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(hr)) {
		if (fCoInit) ::CoUninitialize();
		return Fail(L"MFStartup", hr);
	}

	bool fOK = false;
	{
		CRelease<IMFTransform> Decoder;

		//	AAC デコーダを探す。
		//
		//	CLSID を直接 CoCreateInstance すると環境によっては
		//	REGDB_E_CLASSNOTREG (0x80040154) になる。MFTEnumEx で
		//	「AAC を入力に取れる音声デコーダ」を問い合わせるのが正しい。
		{
			MFT_REGISTER_TYPE_INFO InInfo = { MFMediaType_Audio, MFAudioFormat_AAC };
			IMFActivate **ppActivate = nullptr;
			UINT32 Count = 0;

			hr = ::MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
							 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT
								 | MFT_ENUM_FLAG_SORTANDFILTER,
							 &InInfo, nullptr, &ppActivate, &Count);
			if (SUCCEEDED(hr) && Count > 0) {
				hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&Decoder));
				for (UINT32 i = 0; i < Count; i++)
					ppActivate[i]->Release();
			} else if (SUCCEEDED(hr)) {
				hr = REGDB_E_CLASSNOTREG;
			}
			if (ppActivate != nullptr)
				::CoTaskMemFree(ppActivate);
		}

		//	見つからない場合は CLSID 直指定も試す
		if (Decoder.Get() == nullptr) {
			hr = ::CoCreateInstance(CLSID_AacDecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
									IID_PPV_ARGS(&Decoder));
		}
		if (Decoder.Get() == nullptr) {
			Fail(L"finding the AAC decoder", hr);
			goto done;
		}

		//	--- 入力: ADTS の AAC ---------------------------------------
		//
		//	型を自分で組み立てると MF_E_INVALIDMEDIATYPE で弾かれる。
		//	MF_MT_USER_DATA に何を入れるかが決め打ち出来ない為
		//	(この環境では 12 バイト要求される)。
		//	**MFT が列挙する型を土台にして**、チャンネル数と
		//	サンプリング周波数だけ差し替える。
		{
			bool fSet = false;

			for (DWORD i = 0; i < 32 && !fSet; i++) {
				CRelease<IMFMediaType> Available;
				if (FAILED(Decoder->GetInputAvailableType(0, i, &Available)))
					break;

				UINT32 Payload = 0xFFFF;
				Available->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &Payload);
				if (Payload != 1)		// ADTS 以外は使わない
					continue;

				CRelease<IMFMediaType> Type;
				if (FAILED(::MFCreateMediaType(&Type)))
					continue;
				if (FAILED(Available->CopyAllItems(Type)))
					continue;

				Type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(Channels));
				Type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
								static_cast<UINT32>(SampleRate));

				hr = Decoder->SetInputType(0, Type, 0);
				if (SUCCEEDED(hr))
					fSet = true;
			}

			//	列挙が使えない環境向けの保険
			if (!fSet) {
				CRelease<IMFMediaType> Type;
				if (SUCCEEDED(::MFCreateMediaType(&Type))) {
					Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
					Type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
					Type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(Channels));
					Type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
									static_cast<UINT32>(SampleRate));
					Type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1);
					Type->SetBlob(MF_MT_USER_DATA, AAC_USER_DATA, sizeof(AAC_USER_DATA));
					hr = Decoder->SetInputType(0, Type, 0);
					fSet = SUCCEEDED(hr);
				}
			}

			if (!fSet) { Fail(L"SetInputType", hr); goto done; }
		}

		//	--- 出力: 16bit PCM -----------------------------------------
		{
			CRelease<IMFMediaType> Type;
			hr = ::MFCreateMediaType(&Type);
			if (FAILED(hr)) { Fail(L"MFCreateMediaType(out)", hr); goto done; }

			Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			Type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
			Type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(Channels));
			Type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(SampleRate));
			Type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
			Type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(Channels * 2));
			Type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
							static_cast<UINT32>(SampleRate * Channels * 2));

			hr = Decoder->SetOutputType(0, Type, 0);
			if (FAILED(hr)) { Fail(L"SetOutputType", hr); goto done; }
		}

		Decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
		Decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
		Decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

		//	出力の受け皿。MFT が自前で確保しない場合に備えて用意しておく
		MFT_OUTPUT_STREAM_INFO StreamInfo = {};
		Decoder->GetOutputStreamInfo(0, &StreamInfo);
		const DWORD OutSize = StreamInfo.cbSize > 0 ? StreamInfo.cbSize : 65536;

		//	--- 流し込む -------------------------------------------------
		auto Drain = [&]() -> bool {
			for (;;) {
				CRelease<IMFSample> Sample;
				CRelease<IMFMediaBuffer> Buffer;

				if (::MFCreateSample(&Sample) != S_OK)
					return false;
				if (::MFCreateMemoryBuffer(OutSize, &Buffer) != S_OK)
					return false;
				Sample->AddBuffer(Buffer);

				MFT_OUTPUT_DATA_BUFFER Out = {};
				Out.pSample = Sample;
				DWORD Status = 0;

				const HRESULT hrOut = Decoder->ProcessOutput(0, 1, &Out, &Status);
				if (hrOut == MF_E_TRANSFORM_NEED_MORE_INPUT)
					return true;

				//	MFT が出力型を決め直したがっている。
				//	AAC デコーダでは普通に起きるので、付き合って再設定する。
				if (hrOut == MF_E_TRANSFORM_STREAM_CHANGE) {
					bool fRenegotiated = false;

					for (DWORD i = 0; i < 32 && !fRenegotiated; i++) {
						CRelease<IMFMediaType> Candidate;
						if (FAILED(Decoder->GetOutputAvailableType(0, i, &Candidate)))
							break;

						GUID Subtype = {};
						if (FAILED(Candidate->GetGUID(MF_MT_SUBTYPE, &Subtype)))
							continue;
						if (Subtype != MFAudioFormat_PCM)
							continue;

						if (SUCCEEDED(Decoder->SetOutputType(0, Candidate, 0)))
							fRenegotiated = true;
					}
					if (!fRenegotiated) {
						Fail(L"renegotiating the output type", hrOut);
						return false;
					}
					continue;
				}

				if (FAILED(hrOut)) {
					Fail(L"ProcessOutput", hrOut);
					return false;
				}

				CRelease<IMFMediaBuffer> Got;
				if (SUCCEEDED(Sample->ConvertToContiguousBuffer(&Got))) {
					BYTE *p = nullptr;
					DWORD Length = 0;
					if (SUCCEEDED(Got->Lock(&p, nullptr, &Length))) {
						const size_t n = Length / sizeof(int16_t);
						const size_t Room = MaxSamples > m_Pcm.size()
							? MaxSamples - m_Pcm.size() : 0;
						const size_t Take = n < Room ? n : Room;
						if (Take > 0) {
							const int16_t *q = reinterpret_cast<const int16_t *>(p);
							m_Pcm.insert(m_Pcm.end(), q, q + Take);
						}
						Got->Unlock();
					}
				}
			}
		};

		size_t Offset = 0;
		LONGLONG FrameIndex = 0;
		while (Offset + 7 <= Size) {
			//	ADTS の長さ欄。
			//	先頭が必ずフレーム境界とは限らず、途中で崩れる事もある。
			//	同期を取り直せる様にしておく (打ち切ると 1 枚も出ない)
			const BYTE *p = pData + Offset;
			if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) {
				Offset++;
				continue;
			}
			const int Length = ((p[3] & 0x03) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x07);
			if (Length < 7 || Offset + Length > Size) {
				Offset++;
				continue;
			}

			CRelease<IMFSample> Sample;
			CRelease<IMFMediaBuffer> Buffer;

			if (::MFCreateSample(&Sample) != S_OK) break;
			if (::MFCreateMemoryBuffer(Length, &Buffer) != S_OK) break;

			BYTE *pDst = nullptr;
			if (FAILED(Buffer->Lock(&pDst, nullptr, nullptr))) break;
			::CopyMemory(pDst, p, Length);
			Buffer->Unlock();
			Buffer->SetCurrentLength(Length);
			Sample->AddBuffer(Buffer);

			//	時刻と長さを付けないと出力を出さない MFT がある。
			//	AAC-LC は 1 フレーム 1024 サンプル。単位は 100ns
			const LONGLONG Duration =
				10000000LL * 1024 / SampleRate;
			Sample->SetSampleTime(Duration * FrameIndex);
			Sample->SetSampleDuration(Duration);
			FrameIndex++;

			const HRESULT hrIn = Decoder->ProcessInput(0, Sample, 0);
			if (hrIn == MF_E_NOTACCEPTING) {
				if (!Drain())
					break;
				if (FAILED(Decoder->ProcessInput(0, Sample, 0)))
					break;
			} else if (FAILED(hrIn)) {
				Fail(L"ProcessInput", hrIn);
				break;
			}

			if (!Drain())
				break;

			if (m_Pcm.size() >= MaxSamples)
				break;

			Offset += Length;
		}

		Decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		Decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
		Drain();

		Decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);

		if (!m_Pcm.empty()) {
			m_Channels = Channels;
			fOK = true;
		} else if (m_szError[0] == L'\0') {
			::lstrcpynW(m_szError, L"no PCM was produced", 128);
		}
	}

done:
	::MFShutdown();
	if (fCoInit)
		::CoUninitialize();

	return fOK;
}
