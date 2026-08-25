//----------------------------------------------------------------------------
//	音声の窓口 (src/aviutl2/audio/tvtv_audio.cpp) を確認する。
//
//	  test_audio <ts-file>
//
//	demux とデコードは test_adts / test_aac_decode で見ているので、
//	ここでは「入力プラグインから見た振る舞い」を確かめる。
//
//	  ・映像との時間差を詰めた分だけサンプル数が減っている事
//	  ・任意位置の読み出しが出来る事
//	  ・範囲外を無音で埋める事 (AviUtl2 は端を跨いだ範囲を要求してくる)
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <vector>

#include "tvtv_audio.h"
#include "ts_audio.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

bool Publish(const char *name, const std::vector<BYTE> &ts)
{
	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;

	::ZeroMemory(&sd, sizeof(sd));
	::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	::SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
	::ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = &sd;

	char szMutex[MAX_PATH];
	::wnsprintfA(szMutex, MAX_PATH, "%s.mutex", name);
	if (::CreateMutexA(&sa, FALSE, szMutex) == nullptr)
		return false;

	const DWORD Total = static_cast<DWORD>(ts.size());
	HANDLE hMap = ::CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
									   sizeof(DWORD) * 4 + Total, name);
	if (hMap == nullptr)
		return false;

	DWORD *p = static_cast<DWORD *>(::MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0));
	if (p == nullptr)
		return false;

	p[0] = Total; p[1] = Total; p[2] = 0; p[3] = 0;
	::CopyMemory(reinterpret_cast<BYTE *>(p) + sizeof(DWORD) * 4, ts.data(), Total);
	return true;
}

bool IsSilence(const std::vector<int16_t> &Buffer)
{
	for (size_t i = 0; i < Buffer.size(); i++) {
		if (Buffer[i] != 0)
			return false;
	}
	return true;
}

}	// namespace


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::printf("usage: test_audio <ts-file>\n");
		return 1;
	}

	std::vector<BYTE> Ts;
	{
		FILE *fp = std::fopen(argv[1], "rb");
		if (fp == nullptr) {
			std::printf("cannot open %s\n", argv[1]);
			return 1;
		}
		std::fseek(fp, 0, SEEK_END);
		long n = std::ftell(fp);
		std::fseek(fp, 0, SEEK_SET);
		if (n > 24 * 1024 * 1024)
			n = 24 * 1024 * 1024;
		Ts.resize(static_cast<size_t>(n) / 188 * 188);
		Ts.resize(std::fread(Ts.data(), 1, Ts.size(), fp) / 188 * 188);
		std::fclose(fp);
	}

	check("published the TS to shared memory", Publish("tsaudio.tvtv", Ts));

	//	同期を詰める前のサンプル数を控えておく
	int64_t RawSamples = 0;
	double Lead = 0.0;
	int Rate = 0;
	{
		CTSAudioSource Source;
		if (Source.Open("tsaudio.tvtv")) {
			RawSamples = Source.GetTotalSamples();
			Lead = Source.GetAudioLeadSeconds();
			Rate = Source.GetSampleRate();
		}
	}
	check("the demux saw the stream", RawSamples > 0);

	//	入力プラグインは m2v と同じパスを渡してくる。
	//	共有メモリ名として使われるのはファイル名部分だけ
	CTvtvAudio Audio;
	const bool fOpened = Audio.Open("C:\\somewhere\\tsaudio.tvtv");
	if (!fOpened)
		std::printf("  error : %ls\n", Audio.GetLastError());
	check("the audio opened through a full path", fOpened);
	if (!fOpened) {
		std::printf("\nFAIL (%d failures)\n", g_failures);
		return 1;
	}

	const WAVEFORMATEX *pFormat = Audio.GetFormat();
	const int64_t Samples = Audio.GetSampleCount();

	std::printf("\n  %d ch  %lu Hz  %d bit\n", pFormat->nChannels,
				pFormat->nSamplesPerSec, pFormat->wBitsPerSample);
	std::printf("  samples : %lld  (%.2f sec)\n", static_cast<long long>(Samples),
				static_cast<double>(Samples) / pFormat->nSamplesPerSec);
	std::printf("  a/v lead: %+.3f sec  (raw %lld samples)\n\n",
				Audio.GetAudioLeadSeconds(), static_cast<long long>(RawSamples));

	check("the format is 16bit PCM",
		  pFormat->wFormatTag == WAVE_FORMAT_PCM && pFormat->wBitsPerSample == 16);
	check("the block alignment matches the channel count",
		  pFormat->nBlockAlign == pFormat->nChannels * 2);
	check("there is more than a second of audio",
		  Samples > pFormat->nSamplesPerSec);

	//	--- 時間差の分だけ詰まっている事 -----------------------------------
	//
	//	リングバッファは GOP の途中で切れるので映像と音声の開始がずれる。
	//	その差の分だけ先頭を捨てる (または無音を詰める) のがこの層の仕事
	if (Rate > 0 && RawSamples > 0) {
		const int64_t Shift = static_cast<int64_t>(Lead * Rate + 0.5);
		const int64_t Expected = RawSamples - Shift;

		std::printf("  shift %lld samples -> expected about %lld\n\n",
					static_cast<long long>(Shift), static_cast<long long>(Expected));

		//	デコーダの出力は端で 1 フレーム前後する
		check("the A/V offset was applied to the sample count",
			  Samples > Expected - 2048 && Samples < Expected + 2048);

		//	復号量の上限は demux が数えたフレーム数から決まる。
		//	固定値で頭打ちにすると、長い取り込みで音声だけ途中で切れる。
		//	「時間差を詰めた分を戻すと、映像と同じ長さになる」事で確かめる
		const int64_t Restored = Samples + Shift;
		check("nothing was cut off by a fixed cap",
			  Restored > RawSamples - 2048 && Restored < RawSamples + 2048);
	}

	//	--- 読み出し -------------------------------------------------------
	const int Channels = pFormat->nChannels;

	{
		std::vector<int16_t> Buffer(1024 * Channels, 0x5A5A);
		const int Read = Audio.Read(0, 1024, Buffer.data());
		check("reading from the start returns the requested length", Read == 1024);
	}

	{
		//	何箇所か見て、どこかで音が出ていれば良しとする。
		//
		//	**実際の録画は無音で始まる事も、静かな場面に当たる事もある**。
		//	1 箇所に決め打ちすると、デコーダではなく素材を見る事になる
		//	(test_decode.cpp の音声の判定と同じ考え方)。
		const int64_t Positions[] = {
			0, Samples / 4, Samples / 2, Samples * 3 / 4,
			Samples > 1024 ? Samples - 1024 : 0,
		};

		bool fAnySound = false;
		for (int64_t Pos : Positions) {
			std::vector<int16_t> Buffer(1024 * Channels, 0x5A5A);
			if (Audio.Read(Pos, 1024, Buffer.data()) <= 0)
				continue;
			const bool fSilent = IsSilence(Buffer);
			std::printf("  audio at %lld : %s\n",
						static_cast<long long>(Pos), fSilent ? "silent" : "sound");
			if (!fSilent)
				fAnySound = true;
		}
		check("the audio is not silent everywhere", fAnySound);
	}

	{
		//	同じ位置を 2 回読んだら同じ物が返る事
		std::vector<int16_t> A(512 * Channels, 0), B(512 * Channels, 1);
		Audio.Read(12345, 512, A.data());
		Audio.Read(12345, 512, B.data());
		check("reading the same position twice gives the same samples", A == B);
	}

	{
		//	範囲外は無音
		std::vector<int16_t> Buffer(256 * Channels, 0x5A5A);
		Audio.Read(Samples + 10000, 256, Buffer.data());
		check("reading past the end returns silence", IsSilence(Buffer));

		std::vector<int16_t> Before(256 * Channels, 0x5A5A);
		Audio.Read(-1000, 256, Before.data());
		check("reading before the start returns silence", IsSilence(Before));
	}

	{
		//	末尾を跨ぐ範囲。前半は中身、後半は無音になる
		std::vector<int16_t> Buffer(2048 * Channels, 0x5A5A);
		Audio.Read(Samples - 1024, 2048, Buffer.data());

		bool fTailSilent = true;
		for (size_t i = 1024 * Channels; i < Buffer.size(); i++) {
			if (Buffer[i] != 0)
				fTailSilent = false;
		}
		check("a range crossing the end is padded with silence", fTailSilent);
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
