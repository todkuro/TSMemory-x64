//----------------------------------------------------------------------------
//	音声用の TS demux (src/aviutl2/audio/ts_audio.cpp) を確認する。
//
//	  test_adts <ts-file>
//
//	実 TS を共有メモリに載せて CTSAudioSource に読ませ、
//	AAC の構成・フレーム索引・PTS が取れているかを見る。
//
//	同期語は音声データ中にも偶然現れる為、単発の検出では
//	「SSR / 8000Hz / 7ch」の様な値を拾ってしまう。
//	ここでは「まともな構成である事」を表明して、それを防いでいる。
//----------------------------------------------------------------------------
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <vector>

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

}	// namespace


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::printf("usage: test_adts <ts-file>\n");
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
	check("read the TS sample", !Ts.empty());
	if (Ts.empty())
		return 1;

	check("published the TS to shared memory", Publish("tsadts.tvtv", Ts));

	//	GetTickCount() は分解能が約 15ms あり、この処理には粗すぎる
	LARGE_INTEGER Freq, t0, t1;
	::QueryPerformanceFrequency(&Freq);

	CTSAudioSource Source;
	::QueryPerformanceCounter(&t0);
	const bool fOpened = Source.Open("tsadts.tvtv");
	::QueryPerformanceCounter(&t1);

	check("the demux found AAC audio", fOpened);
	std::printf("  demux took %.1f ms for %llu MB of TS\n",
				1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart)
					/ static_cast<double>(Freq.QuadPart),
				static_cast<unsigned long long>(Ts.size() / (1024 * 1024)));
	if (!Source.IsValid()) {
		std::printf("\nFAIL (%d failures)\n", g_failures + 1);
		return 1;
	}

	std::printf("\n  sample rate : %d Hz\n", Source.GetSampleRate());
	std::printf("  channels    : %d\n", Source.GetChannels());
	std::printf("  frames      : %d  (%.2f sec)\n", Source.GetFrameCount(),
				Source.GetSampleRate() > 0
					? static_cast<double>(Source.GetTotalSamples()) / Source.GetSampleRate()
					: 0.0);
	std::printf("  es size     : %llu bytes\n",
				static_cast<unsigned long long>(Source.GetDataSize()));
	std::printf("  video pts   : %lld\n", static_cast<long long>(Source.GetVideoStartPts()));
	std::printf("  audio pts   : %lld\n", static_cast<long long>(Source.GetAudioStartPts()));
	std::printf("  audio lead  : %+.3f sec\n\n", Source.GetAudioLeadSeconds());

	//	--- 構成がまともである事 ------------------------------------------
	//	同期語を単発で拾うと、ここが破れる
	const int Rate = Source.GetSampleRate();
	check("the sampling rate is one broadcasting uses",
		  Rate == 48000 || Rate == 44100 || Rate == 32000);
	check("the channel count is sane",
		  Source.GetChannels() >= 1 && Source.GetChannels() <= 8);
	check("more than a second of audio was found",
		  Source.GetTotalSamples() > Rate);

	//	--- 索引が連続している事 ------------------------------------------
	{
		bool fContiguous = true;
		bool fSamples = true;

		for (int i = 0; i + 1 < Source.GetFrameCount(); i++) {
			const TSAudioFrame &a = Source.GetFrame(i);
			const TSAudioFrame &b = Source.GetFrame(i + 1);
			if (a.Offset + a.Length != b.Offset)
				fContiguous = false;
			if (b.StartSample != a.StartSample + 1024)
				fSamples = false;
		}
		check("the frames are contiguous in the elementary stream", fContiguous);
		check("each frame advances by 1024 samples", fSamples);
	}

	//	--- 最後のフレームが ES に収まっている事 ---------------------------
	{
		const TSAudioFrame &Last = Source.GetFrame(Source.GetFrameCount() - 1);
		check("the last frame stays inside the elementary stream",
			  Last.Offset + Last.Length <= Source.GetDataSize());
	}

	//	--- サンプル位置からの逆引き --------------------------------------
	{
		bool fOK = true;
		for (int i = 0; i < Source.GetFrameCount(); i += 37) {
			const int64_t Sample = Source.GetFrame(i).StartSample;
			if (Source.FindFrameBySample(Sample) != i)
				fOK = false;
			if (Source.FindFrameBySample(Sample + 500) != i)
				fOK = false;
		}
		check("a sample position maps back to its frame", fOK);
	}

	//	--- PTS ------------------------------------------------------------
	check("the first frame carries a PTS", Source.GetAudioStartPts() >= 0);
	check("the video start PTS was found", Source.GetVideoStartPts() >= 0);

	//	リングバッファは GOP の途中で切れる為、映像と音声の開始はずれる。
	//	ただし秒単位でずれる事は無い
	check("the A/V offset is within a second",
		  Source.GetAudioLeadSeconds() > -1.0 && Source.GetAudioLeadSeconds() < 1.0);

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
