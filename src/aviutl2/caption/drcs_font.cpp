//----------------------------------------------------------------------------
//	外字 (DRCS) をフォントとして AviUtl2 に渡す (drcs_font.h を参照)
//----------------------------------------------------------------------------
#include <windows.h>
#include <dwrite_3.h>

#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#include <vector>

#include "plugin2.h"
#include "drcs_font.h"
#include "plugin_main.h"

namespace {

//	DirectWrite はフォントの実体を参照し続ける。
//	登録した分は解放せずに持ち続ける (プラグインは終了まで常駐する)。
std::vector<std::vector<BYTE>> g_Fonts;

HOST_APP_TABLE *g_pHost = nullptr;

//	**初期化の中かどうか。**AviUtl2 は register_font_collection を
//	RegisterPlugin の中でしか受け付けず、後から呼ぶと例外を投げて
//	AviUtl2 ごと落ちる (実機で確認)
bool g_fInitializing = true;
EDIT_HANDLE *g_pEdit = nullptr;

void LogHr(LPCWSTR pszWhat, HRESULT hr)
{
	WCHAR sz[160];
	::StringCchPrintfW(sz, ARRAYSIZE(sz), L"TSMemory: %s hr=0x%08lX", pszWhat, hr);
	if (FAILED(hr))
		TSMemoryLogWarn(sz);
	else
		TSMemoryLog(sz);
}

}	// namespace


void TSMemoryFontSetHost(HOST_APP_TABLE *host, EDIT_HANDLE *edit)
{
	g_pHost = host;
	g_pEdit = edit;
}


bool TSMemoryFontCanRegister()
{
	return g_fInitializing;
}


void TSMemoryFontEndInitialize()
{
	g_fInitializing = false;
}


bool TSMemoryRegisterFontCollection(const BYTE *pData, size_t Size)
{
	//	**初期化の外では呼んではいけない。**
	//	本体が例外を投げ、コールバックの中で落ちる
	if (!g_fInitializing) {
		TSMemoryLogWarn(L"TSMemory: フォントは初期化の時にしか登録できません "
						L"(登録を見送りました)");
		return false;
	}
	if (g_pHost == nullptr || g_pHost->register_font_collection == nullptr) {
		TSMemoryLogWarn(L"TSMemory: register_font_collection が使えません "
						L"(本体が対応していない可能性があります)");
		return false;
	}
	if (pData == nullptr || Size == 0)
		return false;

	//	DirectWrite が後から読むので、こちらで複製を保持する
	g_Fonts.emplace_back(pData, pData + Size);
	const std::vector<BYTE> &Font = g_Fonts.back();

	//	SDK のコメントに従い SHARED のファクトリから作る
	IDWriteFactory5 *pFactory = nullptr;
	HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
									   __uuidof(IDWriteFactory5),
									   reinterpret_cast<IUnknown **>(&pFactory));
	if (FAILED(hr)) {
		LogHr(L"DWriteCreateFactory(IDWriteFactory5)", hr);
		return false;
	}

	IDWriteInMemoryFontFileLoader *pLoader = nullptr;
	IDWriteFontFile *pFile = nullptr;
	IDWriteFontSetBuilder1 *pBuilder = nullptr;
	IDWriteFontSet *pSet = nullptr;
	IDWriteFontCollection1 *pCollection = nullptr;
	bool fOK = false;

	hr = pFactory->CreateInMemoryFontFileLoader(&pLoader);
	if (SUCCEEDED(hr))
		hr = pFactory->RegisterFontFileLoader(pLoader);
	if (SUCCEEDED(hr)) {
		hr = pLoader->CreateInMemoryFontFileReference(
				pFactory, Font.data(), static_cast<UINT32>(Font.size()),
				nullptr, &pFile);
	}
	if (SUCCEEDED(hr))
		hr = pFactory->CreateFontSetBuilder(&pBuilder);
	if (SUCCEEDED(hr))
		hr = pBuilder->AddFontFile(pFile);
	if (SUCCEEDED(hr))
		hr = pBuilder->CreateFontSet(&pSet);
	if (SUCCEEDED(hr))
		hr = pFactory->CreateFontCollectionFromFontSet(pSet, &pCollection);

	if (SUCCEEDED(hr) && pCollection != nullptr) {
		g_pHost->register_font_collection(
			reinterpret_cast<::IDWriteFontCollection *>(
				static_cast<IDWriteFontCollection *>(pCollection)));
		fOK = true;

		//	**名前も出す。**利用者が `<@名前>` で指すのはこの名前で、
		//	AviUtl2 のフォント一覧に出るかどうかとは別の話。
		//	一覧に出なくても、この名前で描画は出来る
		std::wstring Names;
		const UINT32 Count = pCollection->GetFontFamilyCount();
		for (UINT32 n = 0; n < Count && n < 8; n++) {
			//	IDWriteFontCollection1 の GetFontFamily は
			//	IDWriteFontFamily1 を返す
			IDWriteFontFamily1 *pFamily = nullptr;
			if (FAILED(pCollection->GetFontFamily(n, &pFamily)) || pFamily == nullptr)
				continue;
			IDWriteLocalizedStrings *pNames = nullptr;
			if (SUCCEEDED(pFamily->GetFamilyNames(&pNames)) && pNames != nullptr) {
				WCHAR szName[64] = {};
				if (SUCCEEDED(pNames->GetString(0, szName, ARRAYSIZE(szName)))) {
					if (!Names.empty())
						Names += L" / ";
					Names += szName;
				}
				pNames->Release();
			}
			pFamily->Release();
		}

		WCHAR sz[256];
		::StringCchPrintfW(sz, ARRAYSIZE(sz),
						   L"TSMemory: フォントを登録しました (%u ファミリ : %s)",
						   Count, Names.empty() ? L"名前を取れません" : Names.c_str());
		TSMemoryLog(sz);
	} else {
		LogHr(L"フォントコレクションの作成に失敗", hr);
	}

	//	コレクションは本体が保持する。ローダは登録したまま残す
	if (pSet != nullptr) pSet->Release();
	if (pBuilder != nullptr) pBuilder->Release();
	if (pFile != nullptr) pFile->Release();
	//	pCollection は本体に渡した物なので解放しない
	if (pLoader != nullptr) pLoader->Release();
	pFactory->Release();

	return fOK;
}


//	登録したフォントが使えるかの確認。
//
//	**enum_font_name() は判定に使えない。**
//	RegisterPlugin の中で呼ぶと、登録の前も後も 0 件を返す (実測)。
//	それでも AviUtl2 のフォント一覧には出て、テキストオブジェクトの
//	<@フォント名> でも描画される事を実機で確認済み。
//	この API は別の時点でしか使えないと思われる為、判定には使わない。
void TSMemoryVerifyFont(LPCWSTR pszFamily)
{
	if (g_pEdit == nullptr || g_pEdit->enum_font_name == nullptr)
		return;

	struct Ctx { LPCWSTR pszWant; bool fFound; int Count; } ctx = { pszFamily, false, 0 };
	g_pEdit->enum_font_name(&ctx, [](void *p, LPCWSTR name) {
		Ctx *c = static_cast<Ctx *>(p);
		c->Count++;
		if (::lstrcmpiW(name, c->pszWant) == 0)
			c->fFound = true;
	});

	//	0 件なら「この時点では数えられない」だけなので黙っている
	if (ctx.Count == 0)
		return;

	WCHAR sz[192];
	::StringCchPrintfW(sz, ARRAYSIZE(sz),
					   L"TSMemory: フォント一覧 %d 件 / \"%s\" は %s",
					   ctx.Count, pszFamily, ctx.fFound ? L"あり" : L"なし");
	TSMemoryLog(sz);
}


//	検証用: フォントファイルを読み、登録して、見えるかを確かめる。
//	  [Caption] FontProbe=C:\path\to\font.ttf
void TSMemoryFontProbe(LPCWSTR pszPath)
{
	WCHAR sz[MAX_PATH + 96];

	HANDLE hFile = ::CreateFileW(pszPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
								 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		::StringCchPrintfW(sz, ARRAYSIZE(sz),
						   L"TSMemory: FontProbe: 開けません : %s", pszPath);
		TSMemoryLogWarn(sz);
		return;
	}

	const DWORD Size = ::GetFileSize(hFile, nullptr);
	std::vector<BYTE> Data(Size);
	DWORD Read = 0;
	const BOOL fRead = ::ReadFile(hFile, Data.data(), Size, &Read, nullptr);
	::CloseHandle(hFile);

	if (!fRead || Read != Size) {
		TSMemoryLogWarn(L"TSMemory: FontProbe: 読み込みに失敗しました");
		return;
	}

	::StringCchPrintfW(sz, ARRAYSIZE(sz),
					   L"TSMemory: FontProbe: %s (%lu bytes)", pszPath, Size);
	TSMemoryLog(sz);

	//	登録の前後でフォント名の一覧がどう変わるかを見る
	TSMemoryVerifyFont(L"TSMemoryDRCSTst");
	if (TSMemoryRegisterFontCollection(Data.data(), Data.size()))
		TSMemoryVerifyFont(L"TSMemoryDRCSTst");
}
