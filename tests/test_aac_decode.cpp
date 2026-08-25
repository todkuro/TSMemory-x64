//----------------------------------------------------------------------------
//	AAC のデコード (src/aviutl2/audio/aac_decoder.cpp) を確認する。
//
//	  test_aac_decode <ts-file>
//
//	demux で取り出した ADTS を Media Foundation に通し、
//	PCM が「無音でない」「長さが辻褄の合う範囲にある」事を見る。
//
//	波形の正しさは判定しない (正解が無い)。代わりに、
//	デコード出来ていない時に必ず落ちる形の表明にしてある。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ts_audio.h"
#include "aac_decoder.h"

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

}	// namespace


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::printf("usage: test_aac_decode <ts-file>\n");
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
	if (Ts.empty()) {
		std::printf("empty TS\n");
		return 1;
	}

	check("published the TS to shared memory", Publish("tsaac.tvtv", Ts));

	CTSAudioSource Source;
	check("the demux found AAC audio", Source.Open("tsaac.tvtv"));
	if (!Source.IsValid()) {
		std::printf("\nFAIL (%d failures)\n", g_failures + 1);
		return 1;
	}

	CAacDecoder Decoder;
	const bool fDecoded = Decoder.DecodeAll(Source.GetData(), Source.GetDataSize(),
											Source.GetSampleRate(), Source.GetChannels(),
											64u * 1024 * 1024);
	if (!fDecoded)
		std::printf("  decoder error : %ls\n", Decoder.GetLastError());
	check("Media Foundation decoded the AAC", fDecoded);
	if (!fDecoded) {
		std::printf("\nFAIL (%d failures)\n", g_failures);
		return 1;
	}

	const int64_t Samples = Decoder.GetSampleCount();
	const double Seconds = static_cast<double>(Samples) / Source.GetSampleRate();
	const double Expected =
		static_cast<double>(Source.GetTotalSamples()) / Source.GetSampleRate();

	std::printf("\n  decoded     : %lld samples  (%.2f sec)\n",
				static_cast<long long>(Samples), Seconds);
	std::printf("  expected    : %.2f sec (from the ADTS frame count)\n\n", Expected);

	check("the decoder reports the same channel count",
		  Decoder.GetChannels() == Source.GetChannels());

	//	AAC は前後のフレームと重なる為、端で 1 フレーム程度前後する
	check("the decoded length matches the frame count",
		  Seconds > Expected - 0.2 && Seconds < Expected + 0.2);

	//	--- 無音でない事 ---------------------------------------------------
	//	デコードに失敗して 0 で埋まっている場合をここで捕まえる
	{
		const std::vector<int16_t> &Pcm = Decoder.GetPcm();
		int64_t Sum = 0;
		int16_t Peak = 0;

		for (size_t i = 0; i < Pcm.size(); i++) {
			const int v = Pcm[i] < 0 ? -Pcm[i] : Pcm[i];
			Sum += v;
			if (v > Peak)
				Peak = static_cast<int16_t>(v);
		}
		const double Average = Pcm.empty() ? 0.0
			: static_cast<double>(Sum) / static_cast<double>(Pcm.size());

		std::printf("  peak %d / average %.1f\n\n", Peak, Average);

		check("the PCM is not silence", Peak > 256);
		check("the PCM is not stuck at full scale", Peak < 32767 || Average < 20000.0);
	}

	std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
