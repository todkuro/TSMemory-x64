//----------------------------------------------------------------------------
//	フォント名を指定しなかった時に GDI が何を選ぶかを調べる。
//
//	  build/tests/font-probe.exe
//
//	TVCaptionMod2 は FaceName が空の時、
//	    logFont.lfFaceName    = ""
//	    logFont.lfCharSet     = DEFAULT_CHARSET
//	    logFont.lfPitchAndFamily = FIXED_PITCH | FF_DONTCARE
//	で CreateFontIndirect する (src/TVCaption2.cpp AddOsdText)。
//	**何が選ばれるかは環境依存**なので、推測せずにここで確かめる。
//----------------------------------------------------------------------------
#include <windows.h>

#include <cstdio>

namespace {

void Probe(const char *pszLabel, BYTE PitchAndFamily, BYTE CharSet,
		   LPCWSTR pszFace)
{
	LOGFONTW lf = {};
	lf.lfHeight = -36;
	lf.lfCharSet = CharSet;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = DRAFT_QUALITY;
	lf.lfPitchAndFamily = PitchAndFamily;
	::lstrcpynW(lf.lfFaceName, pszFace, LF_FACESIZE);

	HFONT hFont = ::CreateFontIndirectW(&lf);
	if (hFont == nullptr) {
		std::printf("  %-28s : CreateFontIndirect failed\n", pszLabel);
		return;
	}

	HDC hDC = ::GetDC(nullptr);
	HGDIOBJ hOld = ::SelectObject(hDC, hFont);

	WCHAR szName[LF_FACESIZE] = {};
	::GetTextFaceW(hDC, LF_FACESIZE, szName);

	char sz[256] = {};
	::WideCharToMultiByte(CP_UTF8, 0, szName, -1, sz, sizeof(sz),
						  nullptr, nullptr);
	std::printf("  %-28s : %s\n", pszLabel, sz);

	::SelectObject(hDC, hOld);
	::ReleaseDC(nullptr, hDC);
	::DeleteObject(hFont);
}

}	// namespace


int main(void)
{
	std::printf("フォント名を空にした時に GDI が選ぶ書体\n");

	Probe("FIXED_PITCH   (TVCaptionMod2)", FIXED_PITCH | FF_DONTCARE,
		  DEFAULT_CHARSET, L"");
	Probe("FIXED_PITCH   +SHIFTJIS", FIXED_PITCH | FF_DONTCARE,
		  SHIFTJIS_CHARSET, L"");
	Probe("DEFAULT_PITCH", DEFAULT_PITCH | FF_DONTCARE,
		  DEFAULT_CHARSET, L"");
	Probe("VARIABLE_PITCH", VARIABLE_PITCH | FF_DONTCARE,
		  DEFAULT_CHARSET, L"");

	return 0;
}
