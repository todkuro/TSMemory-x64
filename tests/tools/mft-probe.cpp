//----------------------------------------------------------------------------
//	Media Foundation の映像デコーダを列挙する。
//
//	  build/tests/mft-probe.exe
//
//	**H.264 / H.265 を扱えるかは環境で変わる。**
//	H.264 は Windows 標準だが、HEVC は Microsoft ストアの
//	「HEVC ビデオ拡張機能」を入れていないと出て来ない。
//	推測せずにこの一覧で確かめる為の物。
//
//	音声デコーダ (src/aviutl2/audio/aac_decoder.cpp) と同じく
//	MFTEnumEx で探す。CLSID を直接 CoCreateInstance すると環境に
//	よっては REGDB_E_CLASSNOTREG になる為。
//----------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstdio>

namespace {

struct Target {
	const GUID *pSubtype;
	const char *pszName;
};

void PrintOne(const Target &t)
{
	MFT_REGISTER_TYPE_INFO Info = { MFMediaType_Video, *t.pSubtype };

	IMFActivate **ppActivate = nullptr;
	UINT32 Count = 0;
	const HRESULT hr = ::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
								   MFT_ENUM_FLAG_SYNCMFT
								   | MFT_ENUM_FLAG_ASYNCMFT
								   | MFT_ENUM_FLAG_HARDWARE
								   | MFT_ENUM_FLAG_SORTANDFILTER,
								   &Info, nullptr, &ppActivate, &Count);

	if (FAILED(hr) || Count == 0) {
		std::printf("  %-12s : ありません (hr=0x%08lX)\n",
					t.pszName, static_cast<unsigned long>(hr));
		if (ppActivate != nullptr)
			::CoTaskMemFree(ppActivate);
		return;
	}

	std::printf("  %-12s : %u 個\n", t.pszName, Count);
	for (UINT32 i = 0; i < Count; i++) {
		WCHAR *pName = nullptr;
		UINT32 Len = 0;
		if (SUCCEEDED(ppActivate[i]->GetAllocatedString(
				MFT_FRIENDLY_NAME_Attribute, &pName, &Len))) {
			char sz[256] = {};
			::WideCharToMultiByte(CP_ACP, 0, pName, -1, sz, sizeof(sz),
								  nullptr, nullptr);
			std::printf("      %s\n", sz);
			::CoTaskMemFree(pName);
		}
		ppActivate[i]->Release();
	}
	::CoTaskMemFree(ppActivate);
}

}	// namespace


int main(void)
{
	if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
		std::printf("CoInitializeEx failed\n");
		return 1;
	}
	if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
		std::printf("MFStartup failed\n");
		return 1;
	}

	std::printf("Media Foundation の映像デコーダ\n");

	const Target Targets[] = {
		{ &MFVideoFormat_MPEG2, "MPEG-2" },
		{ &MFVideoFormat_H264,  "H.264" },
		{ &MFVideoFormat_HEVC,  "HEVC" },
		{ &MFVideoFormat_HEVC_ES, "HEVC(ES)" },
	};
	for (const Target &t : Targets)
		PrintOne(t);

	::MFShutdown();
	::CoUninitialize();
	return 0;
}
