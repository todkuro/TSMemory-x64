//----------------------------------------------------------------------------
//	テスト用の MPEG-2 TS を合成する
//
//	実際の放送 TS は再配布出来ない為、テストが使う TS をここで作る。
//	これでクローンした直後から全てのテストが走る。
//
//	  build/ts-examples/sample.ts  1 サービス (映像 + 音声)
//	  build/ts-examples/multi.ts   2 サービス (マルチ編成の確認用)
//
//	作る物:
//	  映像  MPEG-2 Video 1440x1080 16:9 29.97fps。
//	        **イントラの DC 係数だけ**で符号化する。AC を出さないので
//	        VLC の表 (Table B-14/15) が要らず、絵は 8x8 のモザイクになる。
//	        m2v 側はシーケンス/GOP/ピクチャの解析、IDCT、4:2:0 -> YUY2、
//	        resize.c まで通る。
//	  音声  AAC LC 48kHz ステレオ。Media Foundation の AAC エンコーダに
//	        正弦波を食わせ、生の AAC フレームに ADTS ヘッダを付ける。
//	        (外部ツールを入れずに済ませる為)
//
//	合成なので実放送の癖 (可変 GOP、フィールド picture、TEI、連続性の飛び、
//	ARIB の記述子等) は入っていない。実 TS があるならそちらも併せて
//	build/ts-examples/ に置く事。
//
//	使い方:
//	  gen-ts-examples.exe <出力ディレクトリ> [秒数]
//----------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

//---------------------------------------------------------------------------
//	ビット列の書き出し (MSB first)
//---------------------------------------------------------------------------
class CBitWriter
{
public:
	std::vector<BYTE> Data;

	void Put(uint32_t Value, int Bits)
	{
		for (int i = Bits - 1; i >= 0; i--)
			PutBit((Value >> i) & 1);
	}

	void PutBit(int Bit)
	{
		if (m_Count == 0) {
			Data.push_back(0);
			m_Count = 8;
		}
		m_Count--;
		if (Bit)
			Data.back() |= static_cast<BYTE>(1 << m_Count);
	}

	//	次のスタートコードの前に入れる 0 詰め (next_start_code)
	void AlignZero() { m_Count = 0; }

	bool IsAligned() const { return m_Count == 0; }

	void StartCode(BYTE Code)
	{
		AlignZero();
		Data.push_back(0x00);
		Data.push_back(0x00);
		Data.push_back(0x01);
		Data.push_back(Code);
	}

private:
	int m_Count = 0;
};


//---------------------------------------------------------------------------
//	MPEG-2 Video (イントラ DC のみ)
//---------------------------------------------------------------------------
const int VIDEO_WIDTH = 1440;
const int VIDEO_HEIGHT = 1080;
const int MB_WIDTH = VIDEO_WIDTH / 16;					// 90
const int MB_HEIGHT = (VIDEO_HEIGHT + 15) / 16;			// 68 (1080 は 16 の倍数でない)
const int GOP_LENGTH = 12;								// 0.4 秒。切れ端の欠落を小さく保つ
const int FRAME_RATE_CODE = 4;							// 29.97
const double FRAME_RATE = 30000.0 / 1001.0;

//	dct_dc_size_luminance (Table B-12)
const char *DC_SIZE_LUMA[] = {
	"100", "00", "01", "101", "110", "1110", "11110", "111110",
	"1111110", "11111110", "111111110", "1111111110",
};
//	dct_dc_size_chrominance (Table B-13)
const char *DC_SIZE_CHROMA[] = {
	"00", "01", "10", "110", "1110", "11110", "111110", "1111110",
	"11111110", "111111110", "1111111110", "11111111110",
};

void PutCode(CBitWriter *pBw, const char *pszBits)
{
	for (const char *p = pszBits; *p != '\0'; p++)
		pBw->PutBit(*p == '1' ? 1 : 0);
}

//	intra ブロックを 1 つ書く。DC の差分値だけを出し、すぐ EOB
void PutIntraBlock(CBitWriter *pBw, int Diff, bool fLuma)
{
	int Size = 0;
	int Abs = Diff < 0 ? -Diff : Diff;
	while (Abs >> Size)
		Size++;

	PutCode(pBw, fLuma ? DC_SIZE_LUMA[Size] : DC_SIZE_CHROMA[Size]);

	if (Size > 0) {
		//	負の値は 2^Size - 1 を足して MSB が 0 になるようにする
		const int Bits = Diff > 0 ? Diff : Diff + (1 << Size) - 1;
		pBw->Put(static_cast<uint32_t>(Bits), Size);
	}

	//	End of Block (Table B-14。intra_vlc_format=0)
	PutCode(pBw, "10");
}

//	絵柄。マクロブロック毎に平坦な値を置くだけだが、位置と時間で
//	散らすので標準偏差が出る (テストは「平坦でない事」を見る)
int LumaAt(int mbx, int mby, int Frame, int Variant)
{
	const int v = ((mbx * 7) ^ (mby * 13)) + Frame * 3 + Variant * 97;
	return 16 + ((v % 200) + 200) % 200;			// 16..215
}

int ChromaAt(int mbx, int mby, int Frame, int Variant, int Plane)
{
	const int v = (mbx * 5 + mby * 3 + Frame * 2 + Variant * 41 + Plane * 61);
	return 32 + ((v % 180) + 180) % 180;			// 32..211
}

void PutSequenceHeader(CBitWriter *pBw)
{
	pBw->StartCode(0xB3);
	pBw->Put(VIDEO_WIDTH, 12);
	pBw->Put(VIDEO_HEIGHT, 12);
	pBw->Put(3, 4);					// aspect_ratio_information : 16:9
	pBw->Put(FRAME_RATE_CODE, 4);
	pBw->Put(37500, 18);			// bit_rate_value (400bps 単位) = 15Mbps
	pBw->PutBit(1);					// marker_bit
	pBw->Put(112, 10);				// vbv_buffer_size_value
	pBw->PutBit(0);					// constrained_parameters_flag
	pBw->PutBit(0);					// load_intra_quantiser_matrix
	pBw->PutBit(0);					// load_non_intra_quantiser_matrix

	//	sequence_extension
	pBw->StartCode(0xB5);
	pBw->Put(1, 4);					// extension_start_code_identifier
	pBw->Put(0x48, 8);				// profile_and_level : Main@Main
	pBw->PutBit(1);					// progressive_sequence
	pBw->Put(1, 2);					// chroma_format : 4:2:0
	pBw->Put(0, 2);					// horizontal_size_extension
	pBw->Put(0, 2);					// vertical_size_extension
	pBw->Put(0, 12);				// bit_rate_extension
	pBw->PutBit(1);					// marker_bit
	pBw->Put(0, 8);					// vbv_buffer_size_extension
	pBw->PutBit(0);					// low_delay
	pBw->Put(0, 2);					// frame_rate_extension_n
	pBw->Put(0, 5);					// frame_rate_extension_d
}

void PutGopHeader(CBitWriter *pBw, int Frame)
{
	const int Seconds = static_cast<int>(Frame / FRAME_RATE);
	pBw->StartCode(0xB8);
	pBw->PutBit(1);								// drop_frame_flag
	pBw->Put(static_cast<uint32_t>(Seconds / 3600), 5);
	pBw->Put(static_cast<uint32_t>((Seconds / 60) % 60), 6);
	pBw->PutBit(1);								// marker_bit
	pBw->Put(static_cast<uint32_t>(Seconds % 60), 6);
	pBw->Put(static_cast<uint32_t>(Frame % 30), 6);
	pBw->PutBit(1);								// closed_gop
	pBw->PutBit(0);								// broken_link
}

//	1 ピクチャ分の ES を作る
std::vector<BYTE> EncodePicture(int Frame, int Variant)
{
	CBitWriter Bw;

	//	シーケンスヘッダは**毎ピクチャ**の前に置く。
	//
	//	m2v の gop_list.c は、シーケンスヘッダより先に I ピクチャを見ると
	//	その場で失敗する (「壊れたストリームだけが来る」という前提の分岐で、
	//	探し続けない)。実放送は GOP 毎にしか置かないが、途中で切った時に
	//	最初に来るのは大抵 P/B ピクチャなので表面化しない。
	//	ここは全て I ピクチャなので、毎回置かないとリングバッファの様に
	//	途中から始まるデータが開けなくなる。
	PutSequenceHeader(&Bw);
	if (Frame % GOP_LENGTH == 0)
		PutGopHeader(&Bw, Frame);

	//	picture_header
	Bw.StartCode(0x00);
	Bw.Put(static_cast<uint32_t>(Frame % GOP_LENGTH), 10);	// temporal_reference
	Bw.Put(1, 3);								// picture_coding_type : I
	Bw.Put(0xFFFF, 16);							// vbv_delay

	//	picture_coding_extension
	Bw.StartCode(0xB5);
	Bw.Put(8, 4);								// extension_start_code_identifier
	Bw.Put(15, 4); Bw.Put(15, 4);				// f_code[0][0], [0][1]
	Bw.Put(15, 4); Bw.Put(15, 4);				// f_code[1][0], [1][1]
	Bw.Put(0, 2);								// intra_dc_precision : 8bit
	Bw.Put(3, 2);								// picture_structure : frame
	Bw.PutBit(1);								// top_field_first
	Bw.PutBit(1);								// frame_pred_frame_dct
	Bw.PutBit(0);								// concealment_motion_vectors
	Bw.PutBit(0);								// q_scale_type
	Bw.PutBit(0);								// intra_vlc_format
	Bw.PutBit(0);								// alternate_scan
	Bw.PutBit(0);								// repeat_first_field
	Bw.PutBit(1);								// chroma_420_type
	Bw.PutBit(1);								// progressive_frame
	Bw.PutBit(0);								// composite_display_flag

	//	slice を 1 マクロブロック行につき 1 つ
	for (int mby = 0; mby < MB_HEIGHT; mby++) {
		Bw.StartCode(static_cast<BYTE>(mby + 1));	// slice_vertical_position
		Bw.Put(8, 5);							// quantiser_scale_code
		Bw.PutBit(0);							// extra_bit_slice

		//	DC の予測値は slice の先頭で 128 に戻る
		int PredY = 128, PredCb = 128, PredCr = 128;

		for (int mbx = 0; mbx < MB_WIDTH; mbx++) {
			Bw.PutBit(1);						// macroblock_address_increment = 1
			Bw.PutBit(1);						// macroblock_type : Intra (Table B-2)

			const int Y = LumaAt(mbx, mby, Frame, Variant);
			const int Cb = ChromaAt(mbx, mby, Frame, Variant, 0);
			const int Cr = ChromaAt(mbx, mby, Frame, Variant, 1);

			//	輝度 4 ブロック。ブロック毎に少しずらして絵を細かくする
			for (int b = 0; b < 4; b++) {
				const int Value = Y + (b - 2) * 6;
				const int Clamped = Value < 16 ? 16 : (Value > 235 ? 235 : Value);
				PutIntraBlock(&Bw, Clamped - PredY, true);
				PredY = Clamped;
			}
			PutIntraBlock(&Bw, Cb - PredCb, false);
			PredCb = Cb;
			PutIntraBlock(&Bw, Cr - PredCr, false);
			PredCr = Cr;
		}

		Bw.AlignZero();
	}

	return Bw.Data;
}


//---------------------------------------------------------------------------
//	AAC (Media Foundation のエンコーダを使う)
//---------------------------------------------------------------------------
const int AUDIO_RATE = 48000;
const int AUDIO_CHANNELS = 2;
const int AAC_FRAME_SAMPLES = 1024;

void PutAdtsHeader(std::vector<BYTE> *pOut, size_t PayloadSize)
{
	const size_t Len = PayloadSize + 7;
	const int Profile = 1;			// AAC LC
	const int FreqIndex = 3;		// 48000
	const int Channels = AUDIO_CHANNELS;

	pOut->push_back(0xFF);
	pOut->push_back(0xF1);			// MPEG-4, no CRC
	pOut->push_back(static_cast<BYTE>((Profile << 6) | (FreqIndex << 2) | ((Channels >> 2) & 1)));
	pOut->push_back(static_cast<BYTE>(((Channels & 3) << 6) | ((Len >> 11) & 0x03)));
	pOut->push_back(static_cast<BYTE>((Len >> 3) & 0xFF));
	pOut->push_back(static_cast<BYTE>(((Len & 7) << 5) | 0x1F));
	pOut->push_back(0xFC);
}

//	正弦波を AAC LC に符号化し、ADTS フレームの並びを返す
bool EncodeAac(double Seconds, double ToneHz, std::vector<std::vector<BYTE>> *pFrames)
{
	//	--- 元になる PCM -----------------------------------------------------
	const int64_t Total = static_cast<int64_t>(Seconds * AUDIO_RATE);
	std::vector<int16_t> Pcm(static_cast<size_t>(Total) * AUDIO_CHANNELS);
	for (int64_t i = 0; i < Total; i++) {
		//	左右で少し変えておく (ステレオが潰れていない事が判るように)
		const double t = static_cast<double>(i) / AUDIO_RATE;
		const double L = std::sin(2.0 * 3.14159265358979 * ToneHz * t);
		const double R = std::sin(2.0 * 3.14159265358979 * (ToneHz * 1.5) * t);
		Pcm[static_cast<size_t>(i) * 2 + 0] = static_cast<int16_t>(L * 12000.0);
		Pcm[static_cast<size_t>(i) * 2 + 1] = static_cast<int16_t>(R * 12000.0);
	}

	//	--- エンコーダを探す -------------------------------------------------
	MFT_REGISTER_TYPE_INFO InInfo = { MFMediaType_Audio, MFAudioFormat_PCM };
	MFT_REGISTER_TYPE_INFO OutInfo = { MFMediaType_Audio, MFAudioFormat_AAC };
	IMFActivate **ppActivate = nullptr;
	UINT32 Count = 0;

	HRESULT hr = ::MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
							 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT
							 | MFT_ENUM_FLAG_SORTANDFILTER,
							 &InInfo, &OutInfo, &ppActivate, &Count);
	if (FAILED(hr) || Count == 0) {
		std::printf("error: AAC encoder not found (hr=0x%08lX)\n", hr);
		return false;
	}

	IMFTransform *pMft = nullptr;
	hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&pMft));
	for (UINT32 i = 0; i < Count; i++)
		ppActivate[i]->Release();
	::CoTaskMemFree(ppActivate);
	if (FAILED(hr)) {
		std::printf("error: ActivateObject failed (hr=0x%08lX)\n", hr);
		return false;
	}

	//	--- 型 ---------------------------------------------------------------
	//	エンコーダは出力を先に決める
	IMFMediaType *pOutType = nullptr;
	::MFCreateMediaType(&pOutType);
	pOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	pOutType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
	pOutType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	pOutType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_RATE);
	pOutType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AUDIO_CHANNELS);
	pOutType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);	// 128kbps
	pOutType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);					// 生の AAC
	pOutType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
	hr = pMft->SetOutputType(0, pOutType, 0);
	pOutType->Release();
	if (FAILED(hr)) {
		std::printf("error: SetOutputType failed (hr=0x%08lX)\n", hr);
		pMft->Release();
		return false;
	}

	IMFMediaType *pInType = nullptr;
	::MFCreateMediaType(&pInType);
	pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	pInType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	pInType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	pInType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_RATE);
	pInType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AUDIO_CHANNELS);
	pInType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, AUDIO_CHANNELS * 2);
	pInType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AUDIO_RATE * AUDIO_CHANNELS * 2);
	hr = pMft->SetInputType(0, pInType, 0);
	pInType->Release();
	if (FAILED(hr)) {
		std::printf("error: SetInputType failed (hr=0x%08lX)\n", hr);
		pMft->Release();
		return false;
	}

	pMft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

	//	--- 流し込む ---------------------------------------------------------
	HRESULT LastError = S_OK;
	auto Drain = [&](void) {
		for (;;) {
			MFT_OUTPUT_STREAM_INFO si = {};
			pMft->GetOutputStreamInfo(0, &si);

			IMFSample *pSample = nullptr;
			IMFMediaBuffer *pBuffer = nullptr;
			::MFCreateSample(&pSample);
			::MFCreateMemoryBuffer(si.cbSize > 0 ? si.cbSize : 4096, &pBuffer);
			pSample->AddBuffer(pBuffer);

			MFT_OUTPUT_DATA_BUFFER db = {};
			db.pSample = pSample;
			DWORD Status = 0;
			const HRESULT hrOut = pMft->ProcessOutput(0, 1, &db, &Status);
			if (hrOut == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hrOut)) {
				if (FAILED(hrOut) && hrOut != MF_E_TRANSFORM_NEED_MORE_INPUT)
					LastError = hrOut;
				pBuffer->Release();
				pSample->Release();
				break;
			}

			BYTE *pData = nullptr;
			DWORD Length = 0;
			pBuffer->Lock(&pData, nullptr, &Length);
			if (Length > 0) {
				std::vector<BYTE> Frame;
				PutAdtsHeader(&Frame, Length);
				Frame.insert(Frame.end(), pData, pData + Length);
				pFrames->push_back(Frame);
			}
			pBuffer->Unlock();
			pBuffer->Release();
			pSample->Release();
		}
	};

	const size_t ChunkSamples = AAC_FRAME_SAMPLES;
	for (int64_t Pos = 0; Pos < Total; Pos += ChunkSamples) {
		const size_t n = static_cast<size_t>(
			(Pos + static_cast<int64_t>(ChunkSamples) <= Total)
			? ChunkSamples : (Total - Pos));
		const DWORD Bytes = static_cast<DWORD>(n * AUDIO_CHANNELS * 2);

		IMFSample *pSample = nullptr;
		IMFMediaBuffer *pBuffer = nullptr;
		::MFCreateSample(&pSample);
		::MFCreateMemoryBuffer(Bytes, &pBuffer);

		BYTE *pData = nullptr;
		pBuffer->Lock(&pData, nullptr, nullptr);
		std::memcpy(pData, &Pcm[static_cast<size_t>(Pos) * AUDIO_CHANNELS], Bytes);
		pBuffer->Unlock();
		pBuffer->SetCurrentLength(Bytes);
		pSample->AddBuffer(pBuffer);

		//	エンコーダは時刻の付いていないサンプルを受け取らない
		//	(MF_E_NO_SAMPLE_TIMESTAMP)。単位は 100ns
		pSample->SetSampleTime(Pos * 10000000 / AUDIO_RATE);
		pSample->SetSampleDuration(static_cast<LONGLONG>(n) * 10000000 / AUDIO_RATE);

		HRESULT hrIn = pMft->ProcessInput(0, pSample, 0);
		if (hrIn == MF_E_NOTACCEPTING) {
			Drain();
			hrIn = pMft->ProcessInput(0, pSample, 0);
		}
		if (FAILED(hrIn) && LastError == S_OK)
			LastError = hrIn;
		pBuffer->Release();
		pSample->Release();

		Drain();
	}

	pMft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
	Drain();
	pMft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
	pMft->Release();

	if (pFrames->empty()) {
		std::printf("error: the AAC encoder produced no frames (last hr=0x%08lX)\n",
					LastError);
		return false;
	}
	return true;
}


//---------------------------------------------------------------------------
//	TS の多重化
//---------------------------------------------------------------------------
const int TS_PACKET_SIZE = 188;

struct Service {
	WORD ServiceID;
	WORD PmtPid;
	WORD VideoPid;
	WORD AudioPid;
	int Variant;			// 絵柄を変える為の種
};

class CTsMuxer
{
public:
	std::vector<BYTE> Out;

	void SetServices(const std::vector<Service> &Services) { m_Services = Services; }

	//	PES を TS パケットに割って追記する
	void WritePes(WORD Pid, BYTE StreamID, int64_t Pts, const std::vector<BYTE> &Payload,
				  bool fPcr)
	{
		std::vector<BYTE> Pes;
		Pes.push_back(0x00); Pes.push_back(0x00); Pes.push_back(0x01);
		Pes.push_back(StreamID);

		const size_t PesLen = Payload.size() + 8;		// ヘッダ 3 + PTS 5
		//	映像は 65535 を超える事があるので 0 (不定長) にする
		const size_t Field = (PesLen > 0xFFFF) ? 0 : PesLen;
		Pes.push_back(static_cast<BYTE>(Field >> 8));
		Pes.push_back(static_cast<BYTE>(Field & 0xFF));
		Pes.push_back(0x80);							// '10' + フラグ無し
		Pes.push_back(0x80);							// PTS のみ
		Pes.push_back(0x05);							// ヘッダ長
		PutPts(&Pes, Pts);
		Pes.insert(Pes.end(), Payload.begin(), Payload.end());

		size_t Pos = 0;
		bool fFirst = true;
		while (Pos < Pes.size()) {
			BYTE Packet[TS_PACKET_SIZE];
			std::memset(Packet, 0xFF, sizeof(Packet));

			Packet[0] = 0x47;
			Packet[1] = static_cast<BYTE>((fFirst ? 0x40 : 0x00) | ((Pid >> 8) & 0x1F));
			Packet[2] = static_cast<BYTE>(Pid & 0xFF);

			const bool fWantPcr = fFirst && fPcr;
			size_t Offset = 4;

			//	残りが少ない時はアダプテーションフィールドで詰める
			const size_t Remain = Pes.size() - Pos;
			size_t Space = TS_PACKET_SIZE - 4;
			size_t AdaptLen = 0;

			if (fWantPcr)
				AdaptLen = 8;								// 長さ + フラグ + PCR 6
			if (Remain < Space - (AdaptLen > 0 ? AdaptLen : 0))
				AdaptLen = Space - Remain;

			if (AdaptLen > 0) {
				Packet[1] |= 0x00;
				Packet[3] = static_cast<BYTE>(0x30 | (m_Cc[Pid] & 0x0F));
				Packet[4] = static_cast<BYTE>(AdaptLen - 1);
				if (AdaptLen >= 2) {
					Packet[5] = fWantPcr ? 0x10 : 0x00;
					for (size_t i = 6; i < 4 + AdaptLen; i++)
						Packet[i] = 0xFF;
					if (fWantPcr && AdaptLen >= 8)
						PutPcr(&Packet[6], Pts);
				}
				Offset = 4 + AdaptLen;
			} else {
				Packet[3] = static_cast<BYTE>(0x10 | (m_Cc[Pid] & 0x0F));
			}

			const size_t Take = TS_PACKET_SIZE - Offset;
			const size_t n = Remain < Take ? Remain : Take;
			std::memcpy(&Packet[Offset], &Pes[Pos], n);
			Pos += n;

			m_Cc[Pid] = (m_Cc[Pid] + 1) & 0x0F;
			Out.insert(Out.end(), Packet, Packet + TS_PACKET_SIZE);
			fFirst = false;
		}
	}

	void WritePat()
	{
		std::vector<BYTE> Section;
		Section.push_back(0x00);						// table_id
		Section.push_back(0x00); Section.push_back(0x00);	// 長さは後で
		Section.push_back(0x00); Section.push_back(0x01);	// transport_stream_id
		Section.push_back(0xC1);						// version 0, current
		Section.push_back(0x00);						// section_number
		Section.push_back(0x00);						// last_section_number
		for (const Service &s : m_Services) {
			Section.push_back(static_cast<BYTE>(s.ServiceID >> 8));
			Section.push_back(static_cast<BYTE>(s.ServiceID & 0xFF));
			Section.push_back(static_cast<BYTE>(0xE0 | ((s.PmtPid >> 8) & 0x1F)));
			Section.push_back(static_cast<BYTE>(s.PmtPid & 0xFF));
		}
		WriteSection(0x0000, &Section);
	}

	void WritePmt(const Service &s)
	{
		std::vector<BYTE> Section;
		Section.push_back(0x02);						// table_id
		Section.push_back(0x00); Section.push_back(0x00);
		Section.push_back(static_cast<BYTE>(s.ServiceID >> 8));
		Section.push_back(static_cast<BYTE>(s.ServiceID & 0xFF));
		Section.push_back(0xC1);
		Section.push_back(0x00);
		Section.push_back(0x00);
		Section.push_back(static_cast<BYTE>(0xE0 | ((s.VideoPid >> 8) & 0x1F)));	// PCR_PID
		Section.push_back(static_cast<BYTE>(s.VideoPid & 0xFF));
		Section.push_back(0xF0); Section.push_back(0x00);	// program_info_length

		const BYTE Streams[][2] = {
			{ 0x02, 0 },			// MPEG-2 Video
			{ 0x0F, 1 },			// AAC (ADTS)
		};
		for (const auto &e : Streams) {
			const WORD Pid = (e[1] == 0) ? s.VideoPid : s.AudioPid;
			Section.push_back(e[0]);
			Section.push_back(static_cast<BYTE>(0xE0 | ((Pid >> 8) & 0x1F)));
			Section.push_back(static_cast<BYTE>(Pid & 0xFF));
			Section.push_back(0xF0); Section.push_back(0x00);
		}
		WriteSection(s.PmtPid, &Section);
	}

private:
	std::vector<Service> m_Services;
	BYTE m_Cc[0x2000] = {};

	static void PutPts(std::vector<BYTE> *pOut, int64_t Pts)
	{
		pOut->push_back(static_cast<BYTE>(0x21 | ((Pts >> 29) & 0x0E)));
		pOut->push_back(static_cast<BYTE>((Pts >> 22) & 0xFF));
		pOut->push_back(static_cast<BYTE>(0x01 | ((Pts >> 14) & 0xFE)));
		pOut->push_back(static_cast<BYTE>((Pts >> 7) & 0xFF));
		pOut->push_back(static_cast<BYTE>(0x01 | ((Pts << 1) & 0xFE)));
	}

	static void PutPcr(BYTE *p, int64_t Pts)
	{
		const int64_t Base = Pts;
		p[0] = static_cast<BYTE>((Base >> 25) & 0xFF);
		p[1] = static_cast<BYTE>((Base >> 17) & 0xFF);
		p[2] = static_cast<BYTE>((Base >> 9) & 0xFF);
		p[3] = static_cast<BYTE>((Base >> 1) & 0xFF);
		p[4] = static_cast<BYTE>(((Base & 1) << 7) | 0x7E);
		p[5] = 0x00;
	}

	static uint32_t Crc32(const BYTE *p, size_t Size)
	{
		uint32_t Crc = 0xFFFFFFFF;
		for (size_t i = 0; i < Size; i++) {
			Crc ^= static_cast<uint32_t>(p[i]) << 24;
			for (int b = 0; b < 8; b++)
				Crc = (Crc & 0x80000000) ? ((Crc << 1) ^ 0x04C11DB7) : (Crc << 1);
		}
		return Crc;
	}

	//	セクションを 1 パケットに収めて書く (短いので分割しない)
	void WriteSection(WORD Pid, std::vector<BYTE> *pSection)
	{
		//	section_length = 残り + CRC 4
		const size_t Length = pSection->size() - 3 + 4;
		(*pSection)[1] = static_cast<BYTE>(0xB0 | ((Length >> 8) & 0x0F));
		(*pSection)[2] = static_cast<BYTE>(Length & 0xFF);

		const uint32_t Crc = Crc32(pSection->data(), pSection->size());
		pSection->push_back(static_cast<BYTE>(Crc >> 24));
		pSection->push_back(static_cast<BYTE>(Crc >> 16));
		pSection->push_back(static_cast<BYTE>(Crc >> 8));
		pSection->push_back(static_cast<BYTE>(Crc));

		BYTE Packet[TS_PACKET_SIZE];
		std::memset(Packet, 0xFF, sizeof(Packet));
		Packet[0] = 0x47;
		Packet[1] = static_cast<BYTE>(0x40 | ((Pid >> 8) & 0x1F));
		Packet[2] = static_cast<BYTE>(Pid & 0xFF);
		Packet[3] = static_cast<BYTE>(0x10 | (m_Cc[Pid] & 0x0F));
		Packet[4] = 0x00;								// pointer_field
		std::memcpy(&Packet[5], pSection->data(), pSection->size());

		m_Cc[Pid] = (m_Cc[Pid] + 1) & 0x0F;
		Out.insert(Out.end(), Packet, Packet + TS_PACKET_SIZE);
	}
};


//---------------------------------------------------------------------------
bool Generate(const std::string &Path, const std::vector<Service> &Services, double Seconds)
{
	const int Frames = static_cast<int>(Seconds * FRAME_RATE);

	//	音声を先に作る。映像より少し早く始める事で
	//	A/V のずれを詰める処理が実際に動く形にする
	std::vector<std::vector<BYTE>> Aac;
	if (!EncodeAac(Seconds, 440.0, &Aac))
		return false;

	CTsMuxer Mux;
	Mux.SetServices(Services);

	const int64_t PTS_BASE = 90000;
	const int64_t VIDEO_LEAD = 9000;			// 音声が 0.1 秒先に始まる
	const double TICKS_PER_FRAME = 90000.0 / FRAME_RATE;
	const int64_t TICKS_PER_AAC = 90000LL * AAC_FRAME_SAMPLES / AUDIO_RATE;

	size_t AacPos = 0;
	int PatCounter = 0;

	for (int f = 0; f < Frames; f++) {
		//	PAT/PMT を 0.4 秒毎に入れる (切り出しても必ず入るように)
		if (f % 12 == 0) {
			Mux.WritePat();
			for (const Service &s : Services)
				Mux.WritePmt(s);
			PatCounter++;
		}

		const int64_t VideoPts =
			PTS_BASE + VIDEO_LEAD + static_cast<int64_t>(f * TICKS_PER_FRAME);

		for (const Service &s : Services) {
			const std::vector<BYTE> Es = EncodePicture(f, s.Variant);
			Mux.WritePes(s.VideoPid, 0xE0, VideoPts, Es, true);
		}

		//	この映像フレームの区間に入る音声フレームを流す
		while (AacPos < Aac.size()) {
			const int64_t AudioPts = PTS_BASE + static_cast<int64_t>(AacPos) * TICKS_PER_AAC;
			if (AudioPts > VideoPts)
				break;
			for (const Service &s : Services)
				Mux.WritePes(s.AudioPid, 0xC0, AudioPts, Aac[AacPos], false);
			AacPos++;
		}
	}

	//	残りの音声
	for (; AacPos < Aac.size(); AacPos++) {
		const int64_t AudioPts = PTS_BASE + static_cast<int64_t>(AacPos) * TICKS_PER_AAC;
		for (const Service &s : Services)
			Mux.WritePes(s.AudioPid, 0xC0, AudioPts, Aac[AacPos], false);
	}

	FILE *fp = std::fopen(Path.c_str(), "wb");
	if (fp == nullptr) {
		std::printf("error: cannot write %s\n", Path.c_str());
		return false;
	}
	std::fwrite(Mux.Out.data(), 1, Mux.Out.size(), fp);
	std::fclose(fp);

	std::printf("  %-28s %6.1f MB  %d frames  %zu aac frames  %d services\n",
				Path.c_str(), Mux.Out.size() / (1024.0 * 1024.0),
				Frames, Aac.size(), static_cast<int>(Services.size()));
	return true;
}

}	// namespace


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const std::string Dir = argc > 1 ? argv[1] : "build/ts-examples";
	const double Seconds = argc > 2 ? std::atof(argv[2]) : 8.0;

	::CreateDirectoryA(Dir.c_str(), nullptr);

	if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
		std::printf("error: MFStartup failed\n");
		return 1;
	}

	std::printf("generating test streams into %s\n", Dir.c_str());

	bool fOK = true;

	//	1 サービス
	{
		std::vector<Service> Services = {
			{ 23608, 0x0100, 0x0111, 0x0112, 0 },
		};
		fOK = Generate(Dir + "/sample.ts", Services, Seconds) && fOK;
	}

	//	2 サービス (マルチ編成)。絵柄を変えて別番組にする
	{
		std::vector<Service> Services = {
			{ 23608, 0x0100, 0x0111, 0x0112, 0 },
			{ 23610, 0x0200, 0x0131, 0x0132, 1 },
		};
		fOK = Generate(Dir + "/multi.ts", Services, Seconds) && fOK;
	}

	::MFShutdown();
	return fOK ? 0 : 1;
}
