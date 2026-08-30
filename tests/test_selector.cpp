//----------------------------------------------------------------------------
//	マルチ編成 (サブチャンネル) の選択を確認する。
//
//	2 サービス分の PAT / PMT / 映像を含む TS を組み立てて CTsSelector に
//	通し、指定したサービスの映像だけが出てくるかを見る。
//
//	CTsSelector はサービスID 0 を「全サービス」として扱う為、視聴中の
//	サービスを設定し損ねると全サービスの映像が混ざる。その状態で m2v に
//	渡すと PAT の最初の映像 (プライマリチャンネル) が拾われてしまう。
//
//	SPDX-License-Identifier: GPL-2.0-or-later
//	BonTsEngine (GPL-2.0-or-later) を利用する為、このファイルは GPL の対象です。
//	詳細は LICENSE.md を参照してください。
//----------------------------------------------------------------------------
#include <windows.h>
#include <cstdio>
#include <map>
#include <vector>

#include "BonTsEngine/TsSelector.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

//---------------------------------------------------------------------------
//	テスト用の TS を組み立てる
//---------------------------------------------------------------------------
constexpr WORD PMT_PID_1 = 0x0100, VIDEO_PID_1 = 0x0111;	// プライマリ
constexpr WORD PMT_PID_2 = 0x0200, VIDEO_PID_2 = 0x0211;	// サブチャンネル
constexpr WORD AUDIO_PID_1 = 0x0112, AUDIO_PID_2 = 0x0212;	// AAC (stream_type 0x0F)
constexpr WORD SERVICE_1 = 1024, SERVICE_2 = 1025;

DWORD Crc32(const BYTE *pData, size_t Size)
{
	static DWORD Table[256];
	static bool fInit = false;
	if (!fInit) {
		for (DWORD i = 0; i < 256; i++) {
			DWORD c = i << 24;
			for (int j = 0; j < 8; j++)
				c = (c & 0x80000000) ? ((c << 1) ^ 0x04C11DB7) : (c << 1);
			Table[i] = c;
		}
		fInit = true;
	}

	DWORD Crc = 0xFFFFFFFF;
	for (size_t i = 0; i < Size; i++)
		Crc = (Crc << 8) ^ Table[((Crc >> 24) ^ pData[i]) & 0xFF];
	return Crc;
}

//	セクションを 1 パケットに詰める (188 バイトに収まる範囲のみ)
void AppendSection(std::vector<BYTE> *pTs, WORD PID, BYTE Counter,
				   const std::vector<BYTE> &Section)
{
	std::vector<BYTE> Body = Section;
	const DWORD Crc = Crc32(Body.data(), Body.size());
	Body.push_back(static_cast<BYTE>(Crc >> 24));
	Body.push_back(static_cast<BYTE>(Crc >> 16));
	Body.push_back(static_cast<BYTE>(Crc >> 8));
	Body.push_back(static_cast<BYTE>(Crc));

	BYTE p[188];
	::FillMemory(p, sizeof(p), 0xFF);
	p[0] = 0x47;
	p[1] = 0x40 | static_cast<BYTE>(PID >> 8);	// payload_unit_start
	p[2] = static_cast<BYTE>(PID);
	p[3] = 0x10 | (Counter & 0x0F);
	p[4] = 0x00;								// pointer_field
	::CopyMemory(p + 5, Body.data(), min(Body.size(), sizeof(p) - 5));

	pTs->insert(pTs->end(), p, p + sizeof(p));
}

std::vector<BYTE> MakePatFor(const WORD (*Programs)[2], size_t Count)
{
	std::vector<BYTE> s;
	s.push_back(0x00);					// table_id
	s.push_back(0xB0);					// section_syntax_indicator
	s.push_back(0x00);					// section_length (後で埋める)
	s.push_back(0x00); s.push_back(0x01);	// transport_stream_id
	s.push_back(0xC1);					// version / current_next
	s.push_back(0x00); s.push_back(0x00);	// section_number / last

	for (size_t i = 0; i < Count; i++) {
		const WORD (&e)[2] = Programs[i];
		s.push_back(static_cast<BYTE>(e[0] >> 8));
		s.push_back(static_cast<BYTE>(e[0]));
		s.push_back(0xE0 | static_cast<BYTE>(e[1] >> 8));
		s.push_back(static_cast<BYTE>(e[1]));
	}

	s[2] = static_cast<BYTE>(s.size() - 3 + 4);	// 残りの長さ + CRC
	return s;
}

std::vector<BYTE> MakePat()
{
	const WORD Programs[][2] = { { SERVICE_1, PMT_PID_1 }, { SERVICE_2, PMT_PID_2 } };
	return MakePatFor(Programs, 2);
}

//	VideoStreamType で映像の種別を変えられる。
//	  0x02 = MPEG-2 Video / 0x1B = H.264 / 0x24 = H.265 (HEVC)
//	PcrPID に 0 を渡すと VideoPID を PCR にする (実放送はこの形が多い)。
//	**PCR の PID は stream_type に関わらず必ず残る** (TsSelector.cpp) 為、
//	ES の選別だけを見たい時は別の PID を指定する。
std::vector<BYTE> MakePmt(WORD ServiceID, WORD VideoPID, WORD AudioPID,
						  BYTE VideoStreamType = 0x02, WORD PcrPID = 0,
						  BYTE AudioStreamType = 0x0F)
{
	if (PcrPID == 0)
		PcrPID = VideoPID;
	std::vector<BYTE> s;
	s.push_back(0x02);					// table_id
	s.push_back(0xB0);
	s.push_back(0x00);					// section_length (後で埋める)
	s.push_back(static_cast<BYTE>(ServiceID >> 8));
	s.push_back(static_cast<BYTE>(ServiceID));
	s.push_back(0xC1);
	s.push_back(0x00); s.push_back(0x00);
	s.push_back(0xE0 | static_cast<BYTE>(PcrPID >> 8));		// PCR_PID
	s.push_back(static_cast<BYTE>(PcrPID));
	s.push_back(0xF0); s.push_back(0x00);					// program_info_length

	s.push_back(VideoStreamType);		// stream_type (映像)
	s.push_back(0xE0 | static_cast<BYTE>(VideoPID >> 8));
	s.push_back(static_cast<BYTE>(VideoPID));
	s.push_back(0xF0); s.push_back(0x00);					// ES_info_length

	s.push_back(AudioStreamType);		// stream_type (音声)
	s.push_back(0xE0 | static_cast<BYTE>(AudioPID >> 8));
	s.push_back(static_cast<BYTE>(AudioPID));
	s.push_back(0xF0); s.push_back(0x00);					// ES_info_length

	s[2] = static_cast<BYTE>(s.size() - 3 + 4);
	return s;
}

void AppendVideo(std::vector<BYTE> *pTs, WORD PID, BYTE Counter, BYTE Fill)
{
	BYTE p[188];
	::FillMemory(p, sizeof(p), Fill);
	p[0] = 0x47;
	p[1] = static_cast<BYTE>(PID >> 8);
	p[2] = static_cast<BYTE>(PID);
	p[3] = 0x10 | (Counter & 0x0F);
	pTs->insert(pTs->end(), p, p + sizeof(p));
}

//	4K8K を模した TS。映像 HEVC + 音声 LATM (0x11) / MPEG-4 raw (0x1C)。
//	地上波/BS の AAC (0x0F, ADTS) とは同期層が違う為、AviUtl2 側では
//	復号出来ないが、**TVTest 側で捨てると後で取り返せない**
constexpr WORD SERVICE_4K1 = 3072, SERVICE_4K2 = 3073;
constexpr WORD PMT_PID_4K1 = 0x0500, VIDEO_PID_4K1 = 0x0511, AUDIO_PID_4K1 = 0x0512;
constexpr WORD PMT_PID_4K2 = 0x0600, VIDEO_PID_4K2 = 0x0611, AUDIO_PID_4K2 = 0x0612;
constexpr WORD PCR_PID_4K1 = 0x051F, PCR_PID_4K2 = 0x061F;

std::vector<BYTE> BuildUhdStream()
{
	std::vector<BYTE> Ts;
	const WORD Programs[][2] = {
		{ SERVICE_4K1, PMT_PID_4K1 }, { SERVICE_4K2, PMT_PID_4K2 },
	};

	for (BYTE round = 0; round < 4; round++) {
		AppendSection(&Ts, 0x0000, round, MakePatFor(Programs, 2));
		AppendSection(&Ts, PMT_PID_4K1, round,
					  MakePmt(SERVICE_4K1, VIDEO_PID_4K1, AUDIO_PID_4K1,
							  0x24, PCR_PID_4K1, 0x11));
		AppendSection(&Ts, PMT_PID_4K2, round,
					  MakePmt(SERVICE_4K2, VIDEO_PID_4K2, AUDIO_PID_4K2,
							  0x24, PCR_PID_4K2, 0x1C));

		for (BYTE i = 0; i < 8; i++) {
			AppendVideo(&Ts, VIDEO_PID_4K1, i, 0x99);
			AppendVideo(&Ts, AUDIO_PID_4K1, i, 0xAA);
			AppendVideo(&Ts, VIDEO_PID_4K2, i, 0xBB);
			AppendVideo(&Ts, AUDIO_PID_4K2, i, 0xCC);
		}
	}
	return Ts;
}

//---------------------------------------------------------------------------
//	CTsSelector の出力を受け取る
//---------------------------------------------------------------------------
class CSink : public CMediaDecoder
{
public:
	std::map<WORD, int> PidCount;
	std::vector<std::vector<BYTE>> PatSections;

	void Clear()
	{
		PidCount.clear();
		PatSections.clear();
	}

	const bool InputMedia(CMediaData *pMediaData, const DWORD /*Index*/ = 0UL) override
	{
		const BYTE *p = pMediaData->GetData();
		if (p == nullptr || pMediaData->GetSize() < 188)
			return true;

		const WORD PID = ((p[1] & 0x1F) << 8) | p[2];
		PidCount[PID]++;

		if (PID == 0x0000 && (p[1] & 0x40) != 0) {
			const int Pointer = p[4];
			PatSections.emplace_back(p + 5 + Pointer, p + 188);
		}
		return true;
	}
};

//	再生成された PAT に載っているサービスID
std::vector<WORD> ServicesInPat(const std::vector<BYTE> &Section)
{
	std::vector<WORD> Out;
	if (Section.size() < 12 || Section[0] != 0x00)
		return Out;

	const int Length = ((Section[1] & 0x0F) << 8) | Section[2];
	const int End = min(3 + Length - 4, static_cast<int>(Section.size()));
	for (int i = 8; i + 4 <= End; i += 4) {
		const WORD Sid = (Section[i] << 8) | Section[i + 1];
		if (Sid != 0)
			Out.push_back(Sid);
	}
	return Out;
}

//	組み立てた TS を CTsSelector に流す
void Feed(CTsSelector *pSelector, const std::vector<BYTE> &Ts)
{
	CTsPacket Packet;
	Packet.GetBuffer(188);

	BYTE ContCounter[0x1FFF];
	::FillMemory(ContCounter, sizeof(ContCounter), 0x10);

	for (size_t i = 0; i + 188 <= Ts.size(); i += 188) {
		Packet.SetData(Ts.data() + i, 188);
		Packet.ParsePacket(ContCounter);
		pSelector->InputMedia(&Packet);
	}
}

std::vector<BYTE> BuildStream()
{
	std::vector<BYTE> Ts;

	//	PSI を先に流してから映像を流す。PSI は繰り返し送る。
	for (BYTE round = 0; round < 4; round++) {
		AppendSection(&Ts, 0x0000, round, MakePat());
		AppendSection(&Ts, PMT_PID_1, round, MakePmt(SERVICE_1, VIDEO_PID_1, AUDIO_PID_1));
		AppendSection(&Ts, PMT_PID_2, round, MakePmt(SERVICE_2, VIDEO_PID_2, AUDIO_PID_2));

		for (BYTE i = 0; i < 8; i++) {
			AppendVideo(&Ts, VIDEO_PID_1, i, 0x11);
			AppendVideo(&Ts, VIDEO_PID_2, i, 0x22);
			//	中身は何でもよい。PID が残るかどうかだけを見る
			AppendVideo(&Ts, AUDIO_PID_1, i, 0x33);
			AppendVideo(&Ts, AUDIO_PID_2, i, 0x44);
		}
	}
	return Ts;
}

//	H.264 / H.265 の TS。
//
//	AviUtl2 側の m2v はこれらを復号出来ないが、**TVTest 側で捨ててしまうと
//	後段で何をしても取り返せない**。ここで見るのは「PID が残るか」だけで、
//	中身が復号可能である必要は無い。その為ダミーのペイロードで足りる
//	(実際に符号化した映像を作らなくても、この確認は成立する)。
constexpr WORD SERVICE_H264 = 2048, SERVICE_H265 = 2049;
constexpr WORD PMT_PID_H264 = 0x0300, VIDEO_PID_H264 = 0x0311;
constexpr WORD PMT_PID_H265 = 0x0400, VIDEO_PID_H265 = 0x0411;
constexpr WORD AUDIO_PID_H264 = 0x0312, AUDIO_PID_H265 = 0x0412;
//	PCR は映像とは別の PID にする。同じにすると「PCR だから残った」のか
//	「stream_type が選ばれたから残った」のかを区別出来ない
constexpr WORD PCR_PID_H264 = 0x031F, PCR_PID_H265 = 0x041F;

std::vector<BYTE> BuildAvcHevcStream()
{
	std::vector<BYTE> Ts;

	const WORD Programs[][2] = {
		{ SERVICE_H264, PMT_PID_H264 },
		{ SERVICE_H265, PMT_PID_H265 },
	};

	for (BYTE round = 0; round < 4; round++) {
		AppendSection(&Ts, 0x0000, round, MakePatFor(Programs, 2));
		AppendSection(&Ts, PMT_PID_H264, round,
					  MakePmt(SERVICE_H264, VIDEO_PID_H264, AUDIO_PID_H264, 0x1B,
							  PCR_PID_H264));
		AppendSection(&Ts, PMT_PID_H265, round,
					  MakePmt(SERVICE_H265, VIDEO_PID_H265, AUDIO_PID_H265, 0x24,
							  PCR_PID_H265));

		for (BYTE i = 0; i < 8; i++) {
			AppendVideo(&Ts, VIDEO_PID_H264, i, 0x55);
			AppendVideo(&Ts, VIDEO_PID_H265, i, 0x66);
			AppendVideo(&Ts, AUDIO_PID_H264, i, 0x77);
			AppendVideo(&Ts, AUDIO_PID_H265, i, 0x88);
		}
	}
	return Ts;
}

}	// namespace

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const std::vector<BYTE> Ts = BuildStream();
	std::printf("synthetic TS : %zu packets, service %u (video PID 0x%04X) and "
				"service %u (video PID 0x%04X)\n\n",
				Ts.size() / 188, SERVICE_1, VIDEO_PID_1, SERVICE_2, VIDEO_PID_2);

	//	--- サブチャンネル (2 番目のサービス) を選ぶ -------------------------
	{
		CSink Sink;
		CTsSelector Selector;
		Selector.SetOutputDecoder(&Sink);
		Selector.SetTargetServiceID(SERVICE_2, CTsSelector::STREAM_MPEG2VIDEO);
		Feed(&Selector, Ts);

		check("sub channel: its own video is kept", Sink.PidCount[VIDEO_PID_2] > 0);
		check("sub channel: the primary video is dropped", Sink.PidCount[VIDEO_PID_1] == 0);

		bool fPatOK = !Sink.PatSections.empty();
		for (const auto &s : Sink.PatSections) {
			const auto Services = ServicesInPat(s);
			if (Services.size() != 1 || Services[0] != SERVICE_2)
				fPatOK = false;
		}
		check("sub channel: the regenerated PAT lists only that service", fPatOK);
		std::printf("  video 0x%04X: %d packets / video 0x%04X: %d packets\n\n",
					VIDEO_PID_1, Sink.PidCount[VIDEO_PID_1],
					VIDEO_PID_2, Sink.PidCount[VIDEO_PID_2]);
	}

	//	--- プライマリ (1 番目のサービス) を選ぶ -----------------------------
	{
		CSink Sink;
		CTsSelector Selector;
		Selector.SetOutputDecoder(&Sink);
		Selector.SetTargetServiceID(SERVICE_1, CTsSelector::STREAM_MPEG2VIDEO);
		Feed(&Selector, Ts);

		check("primary channel: its own video is kept", Sink.PidCount[VIDEO_PID_1] > 0);
		check("primary channel: the sub channel video is dropped",
			  Sink.PidCount[VIDEO_PID_2] == 0);
	}

	//	--- サービスID 0 は「全サービス」になる ------------------------------
	//	視聴中のサービスを設定し損ねるとこの状態になり、映像が混ざる。
	{
		CSink Sink;
		CTsSelector Selector;
		Selector.SetOutputDecoder(&Sink);
		Selector.SetTargetServiceID(0, CTsSelector::STREAM_MPEG2VIDEO);
		Feed(&Selector, Ts);

		check("service id 0 mixes every service (this is what we must avoid)",
			  Sink.PidCount[VIDEO_PID_1] > 0 && Sink.PidCount[VIDEO_PID_2] > 0);
	}

	//	--- 音声 (AAC) を残す指定 --------------------------------------------
	//	TSMemory.cpp の GetTargetStreams() が渡すのと同じ組み合わせ。
	//	STREAM_AAC は StreamTypeList[] の添字 4 = stream_type 0x0F に対応する
	{
		CSink Sink;
		CTsSelector Selector;
		Selector.SetOutputDecoder(&Sink);
		Selector.SetTargetServiceID(SERVICE_1,
									CTsSelector::STREAM_MPEG2VIDEO | CTsSelector::STREAM_AAC);
		Feed(&Selector, Ts);

		check("audio on: the video of that service is kept",
			  Sink.PidCount[VIDEO_PID_1] > 0);
		check("audio on: the audio of that service is kept",
			  Sink.PidCount[AUDIO_PID_1] > 0);
		check("audio on: the other service is still dropped",
			  Sink.PidCount[VIDEO_PID_2] == 0 && Sink.PidCount[AUDIO_PID_2] == 0);
	}

	//	--- 音声を指定しなければ落ちる事 --------------------------------------
	{
		CSink Sink;
		CTsSelector Selector;
		Selector.SetOutputDecoder(&Sink);
		Selector.SetTargetServiceID(SERVICE_1, CTsSelector::STREAM_MPEG2VIDEO);
		Feed(&Selector, Ts);

		check("audio off: the audio PID is dropped", Sink.PidCount[AUDIO_PID_1] == 0);
	}

	//	--- H.264 / H.265 ------------------------------------------------------
	//	AviUtl2 側の m2v はこれらを復号出来ないが、**TVTest 側で捨てると
	//	後段で何をしても取り返せない**。ここで確実に残す。
	{
		const std::vector<BYTE> Ts2 = BuildAvcHevcStream();
		std::printf("\nH.264/H.265 TS : %zu packets, service %u (0x1B, PID 0x%04X) and "
					"service %u (0x24, PID 0x%04X)\n",
					Ts2.size() / 188, SERVICE_H264, VIDEO_PID_H264,
					SERVICE_H265, VIDEO_PID_H265);

		//	TSMemory.cpp の GetTargetStreams() が渡すのと同じ組み合わせ
		const DWORD Streams = CTsSelector::STREAM_MPEG2VIDEO
							| CTsSelector::STREAM_H264
							| CTsSelector::STREAM_H265;
		{
			CSink Sink;
			CTsSelector Selector;
			Selector.SetOutputDecoder(&Sink);
			Selector.SetTargetServiceID(SERVICE_H264, Streams);
			Feed(&Selector, Ts2);
			check("H.264 (0x1B) video is kept", Sink.PidCount[VIDEO_PID_H264] > 0);
			check("H.264: the other service is dropped",
				  Sink.PidCount[VIDEO_PID_H265] == 0);
		}
		{
			CSink Sink;
			CTsSelector Selector;
			Selector.SetOutputDecoder(&Sink);
			Selector.SetTargetServiceID(SERVICE_H265, Streams);
			Feed(&Selector, Ts2);
			check("H.265 (0x24) video is kept", Sink.PidCount[VIDEO_PID_H265] > 0);
			check("H.265: the other service is dropped",
				  Sink.PidCount[VIDEO_PID_H264] == 0);
		}

		//	MPEG-2 だけを要求した場合は落ちる事。
		//	これが落ちなければ StreamTypeList[] の並びと STREAM_* の
		//	ビット位置がずれている
		{
			CSink Sink;
			CTsSelector Selector;
			Selector.SetOutputDecoder(&Sink);
			Selector.SetTargetServiceID(SERVICE_H264, CTsSelector::STREAM_MPEG2VIDEO);
			Feed(&Selector, Ts2);
			check("asking for MPEG-2 only drops the H.264 video",
				  Sink.PidCount[VIDEO_PID_H264] == 0);
		}
		{
			CSink Sink;
			CTsSelector Selector;
			Selector.SetOutputDecoder(&Sink);
			Selector.SetTargetServiceID(SERVICE_H265, CTsSelector::STREAM_H264);
			Feed(&Selector, Ts2);
			check("asking for H.264 only drops the H.265 video",
				  Sink.PidCount[VIDEO_PID_H265] == 0);
		}
	}

	//	--- 4K8K の音声 (LATM 0x11 / MPEG-4 raw 0x1C) --------------------------
	//	AviUtl2 側は ADTS (0x0F) しか復号出来ないが、ここで捨てると
	//	後から取り返せない
	{
		const std::vector<BYTE> Ts3 = BuildUhdStream();
		std::printf("\nUHD TS : service %u (HEVC + LATM 0x11) and "
					"service %u (HEVC + MPEG-4 raw 0x1C)\n",
					SERVICE_4K1, SERVICE_4K2);

		//	TSMemory.cpp の GetTargetStreams() が音声ありで渡す組み合わせ
		const DWORD Streams = CTsSelector::STREAM_MPEG2VIDEO
							| CTsSelector::STREAM_H264
							| CTsSelector::STREAM_H265
							| CTsSelector::STREAM_AAC
							| CTsSelector::STREAM_AAC_LATM
							| CTsSelector::STREAM_MPEG4_AUDIO;
		{
			CSink Sink;
			CTsSelector Selector;
			Selector.SetOutputDecoder(&Sink);
			Selector.SetTargetServiceID(SERVICE_4K1, Streams);
			Feed(&Selector, Ts3);
			check("UHD: the HEVC video is kept", Sink.PidCount[VIDEO_PID_4K1] > 0);
			check("UHD: the LATM audio (0x11) is kept",
				  Sink.PidCount[AUDIO_PID_4K1] > 0);
		}
		{
			CSink Sink;
			CTsSelector Selector;
			Selector.SetOutputDecoder(&Sink);
			Selector.SetTargetServiceID(SERVICE_4K2, Streams);
			Feed(&Selector, Ts3);
			check("UHD: the MPEG-4 raw audio (0x1C) is kept",
				  Sink.PidCount[AUDIO_PID_4K2] > 0);
		}

		//	ADTS だけを要求した場合は落ちる事。
		//	落ちなければ StreamTypeList[] の並びとビット位置がずれている
		{
			CSink Sink;
			CTsSelector Selector;
			Selector.SetOutputDecoder(&Sink);
			Selector.SetTargetServiceID(SERVICE_4K1,
										CTsSelector::STREAM_H265 | CTsSelector::STREAM_AAC);
			Feed(&Selector, Ts3);
			check("UHD: asking for ADTS only drops the LATM audio",
				  Sink.PidCount[AUDIO_PID_4K1] == 0);
			check("UHD: the video is still kept in that case",
				  Sink.PidCount[VIDEO_PID_4K1] > 0);
		}
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
