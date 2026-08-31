//----------------------------------------------------------------------------
//	外字 (DRCS) のビットマップから作った TTF が本当に使えるかを確かめる。
//
//	自前で組み立てたフォントなので、「壊れていない」の判定は
//	**DirectWrite に読ませて通るか**で行う。AviUtl2 も DirectWrite で
//	描画するので、ここが通れば本番でも読める。
//
//	  test_drcs_ttf [出力先の .ttf]
//----------------------------------------------------------------------------
#include <windows.h>
#include <dwrite_3.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "drcs_ttf.h"

namespace {

int g_failures = 0;

void check(const char *what, bool ok)
{
	std::printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		g_failures++;
}

//	4 階調 (2bit/画素) の字形を作る
TSMemoryDrcsGlyph MakeGlyph(wchar_t Code, int Size, int Kind)
{
	TSMemoryDrcsGlyph g;
	g.Code = Code;
	g.Width = Size;
	g.Height = Size;
	g.Depth = 2;						// 4 階調

	const size_t Bits = static_cast<size_t>(Size) * Size * 2;
	g.Pattern.assign((Bits + 7) / 8, 0);

	for (int y = 0; y < Size; y++) {
		for (int x = 0; x < Size; x++) {
			int v = 0;
			switch (Kind) {
			case 0:	// 全面塗り
				v = 3;
				break;
			case 1:	// 枠
				v = (x < 2 || y < 2 || x >= Size - 2 || y >= Size - 2) ? 3 : 0;
				break;
			case 2:	// 斜め
				v = (x == y || x == Size - 1 - y) ? 3 : 0;
				break;
			case 3:	// 市松 (輪郭が細かくなる最悪の形)
				v = ((x ^ y) & 1) ? 3 : 0;
				break;
			}
			if (v == 0)
				continue;
			const size_t Index = static_cast<size_t>(y) * Size + x;
			const size_t BitPos = Index * 2;
			g.Pattern[BitPos / 8] |= static_cast<BYTE>(v << (8 - 2 - (BitPos % 8)));
		}
	}
	return g;
}

}	// namespace


int wmain(int argc, wchar_t **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	//	実測に合わせて 36x36 / 4 階調
	std::vector<TSMemoryDrcsGlyph> Glyphs;
	for (int i = 0; i < 4; i++)
		Glyphs.push_back(MakeGlyph(static_cast<wchar_t>(0xE000 + i), 36, i));

	//	画素の読み出しが往復するか
	{
		const TSMemoryDrcsGlyph &g = Glyphs[1];		// 枠
		check("pixel readback: the border is filled",
			  TSMemoryDrcsPixel(g, 0, 0) == 3 && TSMemoryDrcsPixel(g, 35, 35) == 3);
		check("pixel readback: the middle is empty",
			  TSMemoryDrcsPixel(g, 18, 18) == 0);
		check("pixel readback: out of range is 0",
			  TSMemoryDrcsPixel(g, -1, 0) == 0 && TSMemoryDrcsPixel(g, 36, 0) == 0);
	}

	std::vector<BYTE> Font;
	check("the font was built", TSMemoryBuildDrcsFont(Glyphs, L"TSMemoryDRCS", &Font));

	//	**既定のフォント名には空白が入っている** ([Caption] DrcsFont の
	//	既定は "TSMemory DRCS")。name テーブルに入って引ける事を確かめる
	{
		std::vector<BYTE> F2;
		check("a family name with a space builds",
			  TSMemoryBuildDrcsFont(Glyphs, L"TSMemory DRCS", &F2)
			  && F2.size() > 0);
	}
	std::printf("  font size = %zu bytes / %zu glyphs\n", Font.size(), Glyphs.size());
	check("the font is not empty", Font.size() > 1024);
	check("the sfnt version is 1.0",
		  Font.size() > 4 && Font[0] == 0 && Font[1] == 1 && Font[2] == 0 && Font[3] == 0);

	if (argc > 1 && !Font.empty()) {
		HANDLE h = ::CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr,
								 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			DWORD w = 0;
			::WriteFile(h, Font.data(), static_cast<DWORD>(Font.size()), &w, nullptr);
			::CloseHandle(h);
			std::printf("  wrote %ls\n", argv[1]);
		}
	}

	//	--- DirectWrite に読ませる ------------------------------------------
	IDWriteFactory5 *pFactory = nullptr;
	HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,
									   __uuidof(IDWriteFactory5),
									   reinterpret_cast<IUnknown **>(&pFactory));
	check("DWriteCreateFactory(IDWriteFactory5)", SUCCEEDED(hr));
	if (FAILED(hr)) {
		std::printf("\nFAIL (%d failures)\n", ++g_failures);
		return 1;
	}

	IDWriteInMemoryFontFileLoader *pLoader = nullptr;
	IDWriteFontFile *pFile = nullptr;
	IDWriteFontSetBuilder1 *pBuilder = nullptr;
	IDWriteFontSet *pSet = nullptr;
	IDWriteFontCollection1 *pCollection = nullptr;

	hr = pFactory->CreateInMemoryFontFileLoader(&pLoader);
	if (SUCCEEDED(hr)) hr = pFactory->RegisterFontFileLoader(pLoader);
	if (SUCCEEDED(hr)) {
		hr = pLoader->CreateInMemoryFontFileReference(
				pFactory, Font.data(), static_cast<UINT32>(Font.size()), nullptr, &pFile);
	}
	check("CreateInMemoryFontFileReference", SUCCEEDED(hr));

	//	**壊れたフォントはここで弾かれる**
	if (SUCCEEDED(hr)) {
		BOOL fSupported = FALSE;
		DWRITE_FONT_FILE_TYPE Type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
		DWRITE_FONT_FACE_TYPE Face = DWRITE_FONT_FACE_TYPE_UNKNOWN;
		UINT32 Faces = 0;
		const HRESULT hrAnalyze = pFile->Analyze(&fSupported, &Type, &Face, &Faces);
		check("DirectWrite accepts the font (Analyze)",
			  SUCCEEDED(hrAnalyze) && fSupported);
		std::printf("  fileType=%d faceType=%d faces=%u\n",
					static_cast<int>(Type), static_cast<int>(Face), Faces);
	}

	if (SUCCEEDED(hr)) hr = pFactory->CreateFontSetBuilder(&pBuilder);
	if (SUCCEEDED(hr)) hr = pBuilder->AddFontFile(pFile);
	if (SUCCEEDED(hr)) hr = pBuilder->CreateFontSet(&pSet);
	if (SUCCEEDED(hr)) hr = pFactory->CreateFontCollectionFromFontSet(pSet, &pCollection);
	check("a font collection was created", SUCCEEDED(hr) && pCollection != nullptr);

	if (SUCCEEDED(hr) && pCollection != nullptr) {
		UINT32 Index = 0;
		BOOL fExists = FALSE;
		pCollection->FindFamilyName(L"TSMemoryDRCS", &Index, &fExists);
		check("the family name can be found", fExists != FALSE);

		//	字形が引けるか
		IDWriteFontFamily1 *pFamily = nullptr;
		IDWriteFont3 *pFont = nullptr;
		IDWriteFontFace3 *pFace = nullptr;
		if (fExists && SUCCEEDED(pCollection->GetFontFamily(Index, &pFamily))) {
			if (SUCCEEDED(pFamily->GetFont(0, &pFont)))
				pFont->CreateFontFace(&pFace);
		}
		check("a font face was created", pFace != nullptr);

		if (pFace != nullptr) {
			std::printf("  glyph count = %u\n", pFace->GetGlyphCount());
			check("the glyph count matches (glyphs + .notdef)",
				  pFace->GetGlyphCount() == Glyphs.size() + 1);

			//	**登録した符号が字形に繋がっているか**
			UINT32 Codes[4];
			UINT16 Indices[4] = {};
			for (int i = 0; i < 4; i++)
				Codes[i] = 0xE000 + i;
			const HRESULT hrIdx = pFace->GetGlyphIndices(Codes, 4, Indices);
			bool fMapped = SUCCEEDED(hrIdx);
			for (int i = 0; i < 4; i++)
				fMapped = fMapped && Indices[i] != 0;
			check("every code maps to a glyph", fMapped);
			std::printf("  U+E000..E003 -> glyph %u %u %u %u\n",
						Indices[0], Indices[1], Indices[2], Indices[3]);

			//	輪郭が入っているか (空でない事)
			DWRITE_GLYPH_METRICS Metrics[4] = {};
			if (SUCCEEDED(pFace->GetDesignGlyphMetrics(Indices, 4, Metrics, FALSE))) {
				bool fHasInk = true;
				for (int i = 0; i < 4; i++) {
					const int w = Metrics[i].advanceWidth
								- Metrics[i].leftSideBearing - Metrics[i].rightSideBearing;
					if (w <= 0)
						fHasInk = false;
				}
				check("every glyph has an outline", fHasInk);
				std::printf("  advance=%u  ink width=%d\n", Metrics[0].advanceWidth,
							Metrics[0].advanceWidth - Metrics[0].leftSideBearing
								- Metrics[0].rightSideBearing);
			} else {
				check("glyph metrics could be read", false);
			}
			pFace->Release();
		}
		if (pFont != nullptr) pFont->Release();
		if (pFamily != nullptr) pFamily->Release();
	}

	if (pCollection != nullptr) pCollection->Release();
	if (pSet != nullptr) pSet->Release();
	if (pBuilder != nullptr) pBuilder->Release();
	if (pFile != nullptr) pFile->Release();
	if (pLoader != nullptr) pLoader->Release();
	pFactory->Release();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
